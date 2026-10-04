// ProcessInfo.cpp
// PID -> process name resolution. Strategy, in order:
//   1. Well-known pseudo PIDs (0 = ownerless rows like TIME_WAIT -> "-",
//      4 = System).
//   2. Cache hit revalidated with OpenProcess + GetProcessTimes (creation
//      time) - the cheap path used on every auto-refresh.
//   3. OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ) +
//      QueryFullProcessImageNameW() for the full path; derive base name.
//   4. Fallback: GetModuleBaseNameW().
//   5. Fallback: one Toolhelp32 snapshot per refresh batch.
// Access-denied / exited processes yield a descriptive label.

#include "ProcessInfo.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>

#include "Utils.h"

namespace wintcp {
namespace {

// Extract L"foo.exe" from L"C:\\dir\\foo.exe".
std::wstring BaseName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return path;
    if (pos + 1 >= path.size()) return path;
    return path.substr(pos + 1);
}

}  // namespace

void ProcessResolver::Clear() {
    cache_.clear();
    snapshot_.clear();
    snapshotBuilt_ = false;
}

void ProcessResolver::BuildSnapshotIfNeeded() {
    if (snapshotBuilt_) return;
    snapshotBuilt_ = true;
    snapshot_.clear();
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            snapshot_.emplace(pe.th32ProcessID, std::wstring(pe.szExeFile));
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
}

void ProcessResolver::ResolveOne(DWORD pid, Entry& e) {
    e = Entry();

    if (pid == 0) {
        // Ownerless rows (TIME_WAIT, unowned UDP endpoints): show an em dash
        // instead of a misleading process name.
        e.name = L"—";
        return;
    }
    if (pid == 4) {
        e.name = L"System";
        e.path = L"System";
        return;
    }

    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                             FALSE, pid);
    if (h != nullptr) {
        wchar_t full[MAX_PATH * 2] = {0};
        DWORD len = static_cast<DWORD>(sizeof(full) / sizeof(full[0]));
        if (::QueryFullProcessImageNameW(h, 0, full, &len) && len > 0) {
            e.path.assign(full, len);
            e.name = BaseName(e.path);
        } else {
            // Fallback for the rare process whose image path cannot be queried
            // (protected, or gone between the snapshot and this call).
            //
            // NO GATE HERE, and that is deliberate. A DllAvailable("psapi.dll")
            // guard was added and then removed: on this SDK psapi.h #defines
            // GetModuleBaseNameW to K32GetModuleBaseNameW, which KERNEL32
            // exports, so this call never touches psapi.dll. The gate would have
            // asked about a library the process is not going to load. See the
            // note on kDelayedDlls in WinCaps.cpp - which DLL a call resolves to
            // is decided by the headers, not by the .lib in the link line.
            wchar_t base[MAX_PATH] = {0};
            const DWORD n = ::GetModuleBaseNameW(h, nullptr, base,
                static_cast<DWORD>(sizeof(base) / sizeof(base[0])));
            if (n > 0) e.name.assign(base, n);
        }
        FILETIME cr {}, ex {}, kr {}, ui {};
        if (::GetProcessTimes(h, &cr, &ex, &kr, &ui)) {
            e.create = cr;
            e.createKnown = true;
        }
        ::CloseHandle(h);
    }

    if (e.name.empty()) {
        BuildSnapshotIfNeeded();
        const auto it = snapshot_.find(pid);
        if (it != snapshot_.end() && !it->second.empty()) e.name = it->second;
    }

    if (e.name.empty()) {
        // Protected / elevated / dead process: pick the best label.
        HANDLE hq = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hq != nullptr) {
            e.name = L"<access denied>";
            ::CloseHandle(hq);
        } else {
            const DWORD err = ::GetLastError();
            e.name = (err == ERROR_INVALID_PARAMETER) ? L"<exited>" : L"<access denied>";
        }
    }
}

const ProcessResolver::Entry* ProcessResolver::ResolvePid(DWORD pid) {
    bool valid = false;
    auto it = cache_.find(pid);

    if (it != cache_.end()) {
        if (pid == 4) {
            valid = true;   // static label, never changes
        } else {
            HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (h != nullptr) {
                FILETIME cr {}, ex {}, kr {}, ui {};
                if (::GetProcessTimes(h, &cr, &ex, &kr, &ui) &&
                    it->second.createKnown &&
                    ::CompareFileTime(&cr, &it->second.create) == 0) {
                    valid = true;
                }
                ::CloseHandle(h);
            } else {
                const DWORD err = ::GetLastError();
                if (err != ERROR_INVALID_PARAMETER) {
                    // Still not openable (denied/protected): reuse the
                    // cached label; we cannot observe a recycle here.
                    valid = true;
                }
            }
            if (!valid) cache_.erase(it);
        }
    }

    if (!valid) {
        Entry fresh;
        ResolveOne(pid, fresh);
        it = cache_.emplace(pid, std::move(fresh)).first;
    }
    return &it->second;
}

void ProcessResolver::ResolveBatch(std::vector<Connection>& rows) {
    // The snapshot is built at most once per refresh, and only if some PID
    // actually falls through to it.
    snapshotBuilt_ = false;
    snapshot_.clear();

    // ONE resolution per DISTINCT pid per batch, not one per row.
    //
    // The result for a pid is a function of the pid and the cached entry
    // alone - it never depends on which row asked - so the first row carrying
    // a pid resolves it and every later row with the same pid copies that
    // answer instead of paying another OpenProcess + GetProcessTimes +
    // CloseHandle round trip to reach the identical conclusion. A busy machine
    // shows thousands of connections spread over a few hundred processes, so
    // most rows repeat a pid already resolved earlier in the same batch: the
    // per-row form scaled its kernel traffic with ROWS, this scales it with
    // DISTINCT PIDS. Bench stage [E] measures that difference directly
    // (before: 142.9 ms/op over 50000 rows; after: see todo.md).
    //
    // 'done' holds pointers, not copies: entries are stable in cache_ across
    // later insertions, and an entry is only ever erased while its own pid is
    // being re-resolved - before its pointer is published.
    std::unordered_map<DWORD, const Entry*> done;

    for (Connection& c : rows) {
        const DWORD pid = c.pid;
        auto seen = done.find(pid);
        if (seen == done.end()) {
            seen = done.emplace(pid, ResolvePid(pid)).first;
        }
        const Entry& e = *seen->second;
        c.processName = e.name;
        c.processPath = e.path;
        c.processCreate = e.create;
        c.processCreateKnown = e.createKnown;
    }
}

bool ProcessResolver::VerifyProcess(DWORD pid, const FILETIME& expectedCreate,
                                    bool expectKnown, std::wstring& errorMessage) {
    if (pid == 0 || pid == 4) {
        errorMessage = L"That row does not belong to a killable user process.";
        return false;
    }
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) {
        errorMessage = L"Could not open process: " + FormatSystemError(::GetLastError());
        return false;
    }
    bool ok = true;
    if (expectKnown) {
        FILETIME cr {}, ex {}, kr {}, ui {};
        if (::GetProcessTimes(h, &cr, &ex, &kr, &ui)) {
            if (::CompareFileTime(&cr, &expectedCreate) != 0) {
                errorMessage =
                    L"PID reuse detected: this PID now belongs to a different "
                    L"process than when the row was captured.\n"
                    L"Press F5 to refresh and try again.";
                ok = false;
            }
        }
        // If GetProcessTimes fails we cannot verify - proceed cautiously.
    }
    ::CloseHandle(h);
    if (ok) errorMessage.clear();
    return ok;
}

void QueryServiceNames(std::map<DWORD, std::wstring>& out) {
    out.clear();
    SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (scm == nullptr) return;   // best effort; Service column stays empty

    std::map<DWORD, std::wstring> found;
    DWORD resume = 0;
    for (;;) {
        DWORD bytesNeeded = 0, count = 0;
        ::EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                                SERVICE_STATE_ALL, nullptr, 0, &bytesNeeded,
                                &count, &resume, nullptr);
        if (::GetLastError() != ERROR_MORE_DATA || bytesNeeded == 0) break;
        std::vector<BYTE> buf(bytesNeeded);
        const BOOL ok = ::EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO,
                                                SERVICE_WIN32, SERVICE_STATE_ALL,
                                                buf.data(), bytesNeeded,
                                                &bytesNeeded, &count, &resume, nullptr);
        if (!ok) break;
        const auto* entries =
            reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
        for (DWORD i = 0; i < count; ++i) {
            const DWORD pid = entries[i].ServiceStatusProcess.dwProcessId;
            if (pid == 0 || entries[i].lpServiceName == nullptr) continue;
            auto& slot = found[pid];
            if (!slot.empty()) slot += L", ";
            slot += entries[i].lpServiceName;
        }
        if (bytesNeeded == 0) break;   // all pages read (defensive)
    }
    ::CloseServiceHandle(scm);
    out.swap(found);
}

std::wstring QueryProcessCommandLine(DWORD pid) {
    // ProcessCommandLineInformation = 60 (stable, undocumented-but-widely
    // used NT information class); resolved dynamically from ntdll.
    using NtQueryInformationProcess_t = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static NtQueryInformationProcess_t ntqip = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll"))
            ntqip = reinterpret_cast<NtQueryInformationProcess_t>(
                ::GetProcAddress(ntdll, "NtQueryInformationProcess"));
    }
    if (ntqip == nullptr) return std::wstring();

    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                             FALSE, pid);
    if (h == nullptr)
        h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return std::wstring();

    std::wstring result;
    struct UnicodeStringHeader {
        USHORT Length;
        USHORT MaximumLength;
        PWSTR Buffer;              // compiler inserts alignment padding
    };
    // ProcessCommandLineInformation class id (undocumented, stable — same
    // lineage as kSystemExtendedHandleInformation in SocketTraffic.cpp, which
    // names its class the same way). A command line over 1 MB is not a command
    // line, it is a corrupt length; fall back to one page rather than trusting
    // it, the way the handle-table query falls back to a slab.
    constexpr ULONG kProcessCommandLineInfoClass = 60;
    constexpr ULONG kMaxCommandLineBytes = 1024u * 1024u;
    constexpr ULONG kCommandLineFallbackBytes = 4096;
    ULONG len = 0;
    LONG st = ntqip(h, kProcessCommandLineInfoClass, nullptr, 0,
                    &len);   // STATUS_INFO_LENGTH_MISMATCH
    if (len < sizeof(UnicodeStringHeader) || len > kMaxCommandLineBytes)
        len = kCommandLineFallbackBytes;
    std::vector<BYTE> buf(len);
    st = ntqip(h, kProcessCommandLineInfoClass, buf.data(), len, &len);
    if (st >= 0) {                                // NT_SUCCESS
        const auto* us = reinterpret_cast<const UnicodeStringHeader*>(buf.data());
        if (us->Buffer != nullptr && us->Length > 0 && (us->Length % sizeof(wchar_t)) == 0)
            result.assign(us->Buffer, us->Length / sizeof(wchar_t));
    }
    ::CloseHandle(h);
    return result;
}

namespace {
// EnumWindows callback for PostCloseToProcessWindows. A file-scope function
// rather than a local lambda: a local function definition is ill-formed in
// C++ and the compiler rejects it outright.
struct CloseEnumCtx {
    DWORD pid;
    int posted;
};

BOOL CALLBACK CloseEnumProc(HWND hwnd, LPARAM param) {
    auto* c = reinterpret_cast<CloseEnumCtx*>(param);
    DWORD ownerPid = 0;
    ::GetWindowThreadProcessId(hwnd, &ownerPid);
    if (ownerPid != c->pid) return TRUE;    // keep enumerating
    // WM_CLOSE, exactly as clicking the X would send. Posting rather than
    // sending is required: SendMessage would block until the target handles
    // it, and a hung app would hang WinTCP.
    if (::PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE) ++c->posted;
    return TRUE;
}
}  // namespace

bool PostCloseToProcessWindows(DWORD pid) {
    if (pid == 0) return false;
    CloseEnumCtx ctx = {pid, 0};
    // EnumWindows visits every top-level window exactly once, which is what
    // makes this safe. A re-scan loop would post WM_CLOSE repeatedly to a
    // window that ignores the first message, because it stays in the list.
    ::EnumWindows(&CloseEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.posted > 0;
}

}  // namespace wintcp
