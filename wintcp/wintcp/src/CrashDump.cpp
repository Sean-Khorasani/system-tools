// CrashDump.cpp
// See CrashDump.h.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "CrashDump.h"

#include "WinCaps.h"   // DllAvailable: shell32.dll is delay-loaded

#include <windows.h>
#include <dbghelp.h>   // MINIDUMP_* types only; the DLL is loaded dynamically
#include <shlobj.h>    // SHGetFolderPathW (shell32.lib, already linked)
#include <stdio.h>

namespace wintcp {
namespace {

// At most this many dumps are kept (plus the one just written). A crash loop
// must not fill a disk: each crash past the cap deletes the single oldest, so
// the directory converges at ~11 x a few hundred KB. Filenames embed UTC
// time, so lexicographic order IS chronological order and no timestamps need
// parsing.
constexpr int kMaxCrashDumps = 10;

std::wstring CrashDirOnce() {
    // shell32.dll is delay-loaded, and this function is called from the crash
    // handler's own control flow. Two consequences, both deliberate:
    //
    //   1. The DllAvailable gate is required. Without it, a machine without
    //      shell32 would take a delay-load EXCEPTION inside the crash path.
    //      An exception escaping the unhandled-exception filter is not caught
    //      by that filter - it is a hard second fault, so the minidump would
    //      never be written. Losing the diagnostic is strictly worse than
    //      losing the dumps.
    //   2. There is NO DelayLoadGuard message box here, unlike every other
    //      gated call site. During crash reporting a modal dialog is the worst
    //      possible answer: it can block a recovering system on a message
    //      nobody is there to dismiss. Silently producing no dumps directory is
    //      the correct behaviour, and R1's own documentation already accepts
    //      that "no dumps written" is a valid outcome of this handler.
    if (!DllAvailable("shell32.dll")) return std::wstring();

    wchar_t appdata[MAX_PATH] = {0};
    if (::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                           SHGFP_TYPE_CURRENT, appdata) != S_OK ||
        appdata[0] == L'\0')
        return std::wstring();
    std::wstring dir(appdata);
    dir += L"\\WinTCP\\crashes";
    return dir;
}

void EnsureDir(const std::wstring& dir) {
    // CreateDirectory fails when the directory exists; that is the common
    // case, not an error. Create parents first - one call cannot make two
    // levels, and checking existence first would race a concurrent run.
    const size_t sep = dir.rfind(L'\\');
    if (sep != std::wstring::npos) ::CreateDirectoryW(dir.substr(0, sep).c_str(), nullptr);
    ::CreateDirectoryW(dir.c_str(), nullptr);
}

// Delete the single oldest dump when past the cap. One deletion per crash
// keeps the directory bounded at kMaxCrashDumps+1 without ever enumerating
// into a list: the whole function uses one stack buffer, no heap, no vector,
// because it runs inside the crash handler where allocation is suspect.
// Only our own "wintcp-*.dmp" names are managed; foreign files are left alone.
// Best-effort throughout: any failure leaves the new dump in place, because
// pruning must never endanger the artefact it was asked to make room for.
void PruneOldDumps(const std::wstring& dir) {
    wchar_t pattern[MAX_PATH * 2] = {0};
    ::swprintf_s(pattern, L"%ls\\wintcp-*.dmp", dir.c_str());
    WIN32_FIND_DATAW fd = {};
    const HANDLE h = ::FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int count = 0;
    wchar_t oldest[MAX_PATH] = {0};
    do {
        // FindFirstFile returns names in filesystem order, not sorted, so the
        // oldest is found by comparison: our names embed UTC time, which means
        // lexicographic order IS chronological order and no timestamps need
        // parsing. wcscmp on untrusted names is safe: all are NUL-terminated
        // by the API contract.
        if (count == 0 || ::wcscmp(fd.cFileName, oldest) < 0) {
            ::wcsncpy_s(oldest, fd.cFileName, _TRUNCATE);
        }
        ++count;
    } while (::FindNextFileW(h, &fd) != FALSE);
    ::FindClose(h);
    if (count > kMaxCrashDumps && oldest[0] != L'\0') {
        wchar_t victim[MAX_PATH * 2] = {0};
        ::swprintf_s(victim, L"%ls\\%ls", dir.c_str(), oldest);
        ::DeleteFileW(victim);
    }
}

// Everything below runs INSIDE the crashing process on the crashing thread,
// so it follows crash-handler discipline: stack buffers only, no heap, no
// locks, no CRT locale calls, and every API failure falls through to the next
// step rather than aborting. A half-written dump with a breadcrumb is worth
// more than a perfect dump that never got written because step two failed.
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* xp) {
    const DWORD code = (xp != nullptr && xp->ExceptionRecord != nullptr)
                           ? xp->ExceptionRecord->ExceptionCode
                           : 0xC0000005u;
    wchar_t path[MAX_PATH * 2] = {0};
    const std::wstring dir = CrashDirOnce();
    bool dumped = false;
    if (!dir.empty()) {
        EnsureDir(dir);
        SYSTEMTIME st = {};
        ::GetSystemTime(&st);
        ::swprintf_s(path, L"%ls\\wintcp-%04u%02u%02u-%02u%02u%02u-%lu.dmp",
                     dir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour,
                     st.wMinute, st.wSecond,
                     static_cast<unsigned long>(::GetCurrentProcessId()));
        // dbghelp is loaded dynamically, not linked: a missing DLL must degrade
        // to "breadcrumb only" rather than fail the whole filter at load time.
        HMODULE dbg = ::LoadLibraryW(L"dbghelp.dll");
        if (dbg != nullptr) {
            typedef BOOL(WINAPI* MiniDumpWriteDump_t)(
                HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                const PMINIDUMP_EXCEPTION_INFORMATION,
                const PMINIDUMP_USER_STREAM_INFORMATION,
                const PMINIDUMP_CALLBACK_INFORMATION);
            auto dump = reinterpret_cast<MiniDumpWriteDump_t>(
                ::GetProcAddress(dbg, "MiniDumpWriteDump"));
            if (dump != nullptr) {
                HANDLE f = ::CreateFileW(
                    path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    MINIDUMP_EXCEPTION_INFORMATION info = {};
                    info.ThreadId = ::GetCurrentThreadId();
                    info.ExceptionPointers = xp;
                    info.ClientPointers = FALSE;
                    dumped = dump(::GetCurrentProcess(),
                                  ::GetCurrentProcessId(), f, MiniDumpNormal,
                                  &info, nullptr, nullptr) != FALSE;
                    ::CloseHandle(f);
                    if (!dumped) ::DeleteFileW(path);   // no partial artefacts
                }
            }
            ::FreeLibrary(dbg);
        }
        if (dumped) PruneOldDumps(dir);
    }
    // The breadcrumb names the dump when there is one and says so when there
    // is not. stderr, not stdout: a crash report is diagnostics, never data
    // (the D4 rule), and a pipeline parsing stdout must not receive it.
    char msg[512] = {0};
    if (dumped) {
        char narrow[MAX_PATH * 2] = {0};
        for (size_t i = 0; i + 1 < sizeof(narrow) && path[i] != L'\0'; ++i)
            narrow[i] = (path[i] < 128) ? static_cast<char>(path[i]) : '?';
        ::sprintf_s(msg,
                    "fatal: unhandled exception 0x%08lX; minidump: %s\r\n",
                    code, narrow);
    } else {
        ::sprintf_s(msg,
                    "fatal: unhandled exception 0x%08lX; no minidump "
                    "(dump directory unavailable)\r\n",
                    code);
    }
    const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
    if (err != nullptr && err != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(err, msg,
                    static_cast<DWORD>(strlen(msg)), &written, nullptr);
    }
    ::OutputDebugStringA(msg);
    // Let the system terminate the process. Returning EXECUTE_HANDLER here
    // (rather than CONTINUE_SEARCH into WER) keeps the exit prompt: the dump
    // is already on disk and a WER dialog adds nothing but a hang for scripts.
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

std::wstring CrashDumpDir() {
    return CrashDirOnce();
}

void CrashForTest() {
    // Volatile through a function boundary so no optimizer may prove the null
    // dereference and delete it (which /O2 will do to a locally-obvious null
    // write). A genuine AV, not abort() or throw: those travel CRT paths that
    // may or may not reach the SEH filter, while an AV always does.
    static volatile int* const p = nullptr;
    *p = 1;
    // Unreachable, but a [[noreturn]] that returns is itself UB if ever
    // reached (e.g. under a hypervisor that maps page zero). Terminate loudly
    // rather than fall off the end.
    ::TerminateProcess(::GetCurrentProcess(), 0xC0000409u);
}

void InstallCrashHandler() {
    static bool installed = false;   // idempotent: main + harness share it
    if (installed) return;
    installed = true;
    ::SetUnhandledExceptionFilter(&CrashFilter);
}

}  // namespace wintcp
