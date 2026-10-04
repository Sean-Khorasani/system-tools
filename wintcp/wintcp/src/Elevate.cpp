// Elevate.cpp
// See Elevate.h.

#include "Elevate.h"

#include <shellapi.h>

#include <cstring>
#include <vector>

namespace wintcp {
namespace {

// Marker switched into the command line by the relaunch. Chosen to be
// something no real user would pass.
constexpr wchar_t kElevatedMarker[] = L"--__wintcp-elevated";

// Cached so the answer is computed once per process; it cannot change.
bool g_cachedElevated = false;
bool g_cachedInitialized = false;
bool g_adminMember = false;
bool g_adminMemberInitialized = false;

// Note: the parameter cannot be called "access" - TOKEN_QUERY and friends
// are macros, and a macro that expands inside the parameter list produces
// exactly the broken declaration this used to have.
HANDLE OpenCurrentToken(DWORD access) {
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), access, &token) == FALSE)
        return nullptr;
    return token;
}

}  // namespace

bool IsElevated() {
    if (g_cachedInitialized) return g_cachedElevated;
    g_cachedInitialized = true;

    HANDLE token = OpenCurrentToken(TOKEN_QUERY);
    if (token == nullptr) {
        // No token to inspect: treat as unelevated rather than assuming the
        // best. A wrong "yes" here would mean silently skipping the consent
        // prompt, which is the failure mode that matters.
        g_cachedElevated = false;
        return false;
    }
    TOKEN_ELEVATION el = {};
    DWORD needed = 0;
    const BOOL got =
        ::GetTokenInformation(token, TokenElevation, &el, sizeof(el), &needed);
    ::CloseHandle(token);
    g_cachedElevated = (got != FALSE) && (el.TokenIsElevated != 0);
    return g_cachedElevated;
}

bool IsAdminMember() {
    if (g_adminMemberInitialized) return g_adminMember;
    g_adminMemberInitialized = true;

    // "Can this user elevate?" is answered by the LINKED token, not by
    // IsUserAnAdmin(). On a filtered launch of an admin account the
    // Administrators SID is not enabled in the current token, so
    // IsUserAnAdmin() returns FALSE - yet the user can perfectly well
    // consent to UAC. A linked token existing is precisely the
    // "elevation is possible" signal.
    HANDLE token = OpenCurrentToken(TOKEN_QUERY | TOKEN_DUPLICATE);
    if (token == nullptr) {
        g_adminMember = false;
        return false;
    }
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenLinkedToken, nullptr, 0, &needed);
    if (needed < sizeof(HANDLE)) {
        ::CloseHandle(token);
        g_adminMember = false;
        return false;
    }
    std::vector<BYTE> buf(needed);
    HANDLE linked = nullptr;
    const BOOL got = ::GetTokenInformation(token, TokenLinkedToken, buf.data(),
                                           needed, &needed);
    if (got != FALSE && needed >= sizeof(HANDLE))
        std::memcpy(&linked, buf.data(), sizeof(HANDLE));
    ::CloseHandle(token);

    if (linked != nullptr) {
        g_adminMember = true;
        ::CloseHandle(linked);
    } else {
        g_adminMember = false;
    }
    return g_adminMember;
}

std::wstring ElevationUnavailableReason() {
    if (IsElevated()) return std::wstring();
    if (IsAdminMember()) return std::wstring();

    // A standard account has no linked token and no way to gain one, so the
    // answer is permanent. Distinguishing it from "token introspection
    // failed" matters, because the advice differs: the first is final, the
    // second may be a transient policy artefact.
    HANDLE token = OpenCurrentToken(TOKEN_QUERY);
    if (token == nullptr) {
        return L"Could not inspect this process's security token, so "
               L"elevation cannot be attempted.";
    }
    ::CloseHandle(token);

    return L"This feature needs administrator rights, and this account is not "
           L"a member of the Administrators group.\n\nWinTCP does not need "
           L"elevation for the connection list, Details view, filters or "
           L"export - only for the features that read live packet or TLS "
           L"data.";
}

bool IsElevatedInstance() {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) return false;
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && ::wcscmp(argv[i], kElevatedMarker) == 0) {
            found = true;
            break;
        }
    }
    ::LocalFree(argv);
    return found;
}

bool WasRelaunchedForElevation() { return IsElevatedInstance(); }

bool Reelevate(const std::wstring& featureName) {
    if (IsElevated()) return false;            // nothing to do
    if (IsElevatedInstance()) return false;    // already the elevated copy
    if (!IsAdminMember()) return false;        // standard account

    // Rebuild the command line with the marker appended, preserving whatever
    // the user originally passed (including a CLI invocation, which must stay
    // a CLI invocation after the relaunch).
    std::wstring cmd = ::GetCommandLineW();

    // A quoted path is required if the image path contains spaces, which it
    // normally does. Quote it unconditionally - GetCommandLine returns the
    // raw string, and a bare path with spaces would be split into arguments.
    wchar_t exePath[MAX_PATH] = {0};
    if (::GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;

    std::wstring newCmd;
    newCmd.reserve(cmd.size() + MAX_PATH + 64);
    newCmd += L"\"";
    newCmd += exePath;
    newCmd += L"\"";
    // Carry over the original arguments (skip argv[0]).
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(cmd.c_str(), &argc);
    if (argv != nullptr) {
        for (int i = 1; i < argc; ++i) {
            newCmd += L" \"";
            newCmd += argv[i];
            newCmd += L"\"";
        }
        ::LocalFree(argv);
    }
    newCmd += L" \"";
    newCmd += kElevatedMarker;
    newCmd += L"\"";

    wchar_t dir[MAX_PATH] = {0};
    if (::GetCurrentDirectoryW(MAX_PATH, dir) == 0) dir[0] = L'\0';

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // "runas" is what makes UAC prompt. Returns FALSE with
    // ERROR_CANCELLED when the user clicks No - which must NOT be treated as
    // an error, just as "the user declined".
    const BOOL ok = ::CreateProcessW(nullptr, newCmd.data(), nullptr, nullptr,
                                     FALSE, 0, nullptr,
                                     (dir[0] != L'\0') ? dir : nullptr, &si,
                                     &pi);
    if (ok == FALSE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_CANCELLED) {
            ::MessageBoxW(nullptr,
                          L"Administrator permission was declined, so this "
                          L"feature is unavailable.\n\nThe rest of WinTCP "
                          L"works without it.",
                          L"WinTCP", MB_OK | MB_ICONINFORMATION);
        } else {
            wchar_t msg[256] = {0};
            ::swprintf_s(msg,
                         L"Could not start an elevated copy of WinTCP for "
                         L"'%ls' (error %lu).",
                         featureName.c_str(), static_cast<unsigned long>(err));
            ::MessageBoxW(nullptr, msg, L"WinTCP", MB_OK | MB_ICONERROR);
        }
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    (void)featureName;
    return true;
}

}  // namespace wintcp
