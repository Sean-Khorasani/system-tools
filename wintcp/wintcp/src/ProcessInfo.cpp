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

// F5.3: WinVerifyTrust and the file-info wrapper. softpub.h supplies
// WINTRUST_ACTION_GENERIC_VERIFY_V2, wintrust.h the entry point.
#include <softpub.h>
#include <wintrust.h>

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

// F5.3: turn a WinVerifyTrust LONG into one of the five states. The
// distinction that matters is NOSIGNATURE vs everything else: an unsigned
// binary is the normal case for most of what runs and is NOT a finding, so it
// gets its own state instead of being folded into "invalid".
SignatureState StateFromTrustLONG(LONG rc) {
    if (rc == ERROR_SUCCESS) return kSigValid;
    // TRUST_E_NOSIGNATURE (0x800B0100) is the "there is no signature here"
    // answer; TRUST_E_SUBJECT_FORMUNKNOWN (0x800B0003) is the same idea for a
    // file that could not be a signed PE at all. Both HRESULTs have the
    // severity bit set, so they are compared as raw DWORDs.
    const DWORD code = static_cast<DWORD>(rc);
    if (code == 0x800B0100UL || code == 0x800B0003UL) return kSigUnsigned;
    if ((code & 0x80000000UL) != 0) return kSigInvalid;   // a TRUST_E_* verdict
    return kSigError;                                     // the provider failed
}

// F5.3: a cheap fingerprint of the FILE, so a path whose contents were
// replaced is re-verified rather than inheriting the old verdict. A path is
// not a stable identity for a file - which is why Windows ships versioned
// side-by-side directories and why "same path" cannot mean "same binary".
ImageStamp StampOf(const std::wstring& path) {
    ImageStamp s;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) == 0)
        return s;
    s.size = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) |
             fad.nFileSizeLow;
    s.write = (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
              fad.ftLastWriteTime.dwLowDateTime;
    s.valid = true;
    return s;
}

bool SameStamp(const ImageStamp& a, const ImageStamp& b) {
    return a.valid && b.valid && a.size == b.size && a.write == b.write;
}

// F5.2: what a process token says about its integrity.
struct IntegrityReading {
    IntegrityLevel level = kIntegrityUnknown;
    bool appContainer = false;
};

// F5.2. TOKEN_QUERY is the only access this needs, which is why an unelevated
// WinTCP can usually still read the answer - the token is opened for reading
// only, never asked for anything the caller does not already hold on its own
// process.
//
// The RID is the LAST subauthority of a mandatory label whose authority is
// (SECURITY_NT_AUTHORITY, SECURITY_MANDATORY_LABEL_AUTHORITY_RID); that is the
// documented encoding of S-1-16-<rid>. Anything else - a token with no label, a
// label from a different authority, a zero-length SID - is reported as unknown
// rather than defaulted to Medium, because "Medium" for a process whose level
// could not be read is a guess dressed as a measurement.
IntegrityReading ReadIntegrity(HANDLE process) {
    IntegrityReading out;
    HANDLE tok = nullptr;
    if (::OpenProcessToken(process, TOKEN_QUERY, &tok) == 0) return out;

    BYTE isApp = 0;
    DWORD got = 0;
    if (::GetTokenInformation(tok, TokenIsAppContainer, &isApp,
                              sizeof(isApp), &got) != 0) {
        out.appContainer = (isApp != 0);
    }

    // THE TWO-CALL PATTERN IS REQUIRED, not tidy. A mandatory label is a
    // TOKEN_MANDATORY_LABEL followed by the SID itself, and that SID is LONGER
    // than the PSID field that points at it - S-1-16-8192 is 12 bytes where the
    // pointer is 8. So a buffer of exactly sizeof(TOKEN_MANDATORY_LABEL) is
    // always too small, GetTokenInformation fails with ERROR_INSUFFICIENT_BUFFER,
    // and the obvious-looking code then reports "unknown integrity" for every
    // process on the machine. That is what the first version of this function
    // did, and the column rendered as an em-dash down both sides of the table.
    DWORD need = 0;
    ::GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &need);
    if (need == 0) { ::CloseHandle(tok); return out; }
    std::vector<BYTE> buf(need);
    got = 0;
    const BOOL ok = ::GetTokenInformation(tok, TokenIntegrityLevel, buf.data(),
                                          need, &got);
    ::CloseHandle(tok);
    if (ok == 0) return out;

    const TOKEN_MANDATORY_LABEL* tml =
        reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buf.data());
    if (tml->Label.Sid == nullptr) return out;

    // `Label` is a SID_AND_ATTRIBUTES rather than a bare PSID in this SDK, and
    // the attributes are not used here.
    PSID sid = tml->Label.Sid;
    PSID_IDENTIFIER_AUTHORITY auth = ::GetSidIdentifierAuthority(sid);
    if (auth == nullptr) return out;
    // A SID's IdentifierAuthority is SIX bytes, not a number: S-1-16-8192 encodes
    // it as {0,0,0,0,0,16} and carries 8192 as the single subauthority. So the
    // authority value being tested is the LAST byte - Value[5] - and both of the
    // two earlier versions of this check were wrong: one compared Value[0]
    // against SECURITY_NT_AUTHORITY read as a brace list (a syntax error), and
    // the next compared Value[0]/Value[1] against 5 and 16, which are zero in
    // every real mandatory label. Either way the function answered "unknown"
    // for every process on the machine, and the column was an em-dash down both
    // sides of a 310-row table while looking entirely plausible in review.
    //
    // Comparing Value[5] against the mandatory-label RID IS the whole test: an
    // integrity label carrying any other authority is not something this column
    // has an opinion about.
    constexpr UCHAR kAuthorityLastByte = 5;
    constexpr UCHAR kMandatoryLabelAuthorityRid = 16;
    if (auth->Value[kAuthorityLastByte] != kMandatoryLabelAuthorityRid)
        return out;

    const DWORD subCount = *::GetSidSubAuthorityCount(sid);
    if (subCount == 0) return out;
    out.level = IntegrityFromRid(*::GetSidSubAuthority(sid, subCount - 1));
    return out;
}

}  // namespace

const wchar_t* IntegrityLabel(IntegrityLevel lvl) {
    switch (lvl) {
        case kIntegrityUntrusted: return L"Untrusted";
        case kIntegrityLow:       return L"Low";
        case kIntegrityMedium:    return L"Medium";
        case kIntegrityHigh:      return L"High";
        case kIntegritySystem:    return L"System";
        case kIntegrityProtected: return L"Protected";
        default:                  return L"—";
    }
}

const wchar_t* SignatureLabel(SignatureState state) {
    switch (state) {
        case kSigValid:    return L"Signed";
        case kSigInvalid:  return L"BAD SIG";
        case kSigUnsigned: return L"unsigned";
        case kSigError:    return L"sig?";
        default:           return L"—";
    }
}

IntegrityLevel IntegrityFromRid(DWORD rid) {
    // Exact equality, not >= : these are fixed constants of the security
    // model, and an unfamiliar future RID must read as "unknown" rather than
    // silently claiming the nearest known level.
    switch (rid) {
        case 0:     return kIntegrityUntrusted;
        case 4096:  return kIntegrityLow;
        case 8192:  return kIntegrityMedium;
        case 12288: return kIntegrityHigh;
        case 16384: return kIntegritySystem;
        case 28672: return kIntegrityProtected;
        default:    return kIntegrityUnknown;
    }
}

void ProcessResolver::Clear() {
    cache_.clear();
    snapshot_.clear();
    snapshotBuilt_ = false;
    // F5.3. signatureCache_ is deliberately NOT cleared: it is keyed by path
    // and carries the file stamp each verdict was taken against, so a stale
    // entry is detected and re-verified on the next hit rather than trusted.
    // Dropping it would only buy re-verification work.
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
            SnapEntry se;
            se.name = pe.szExeFile;
            // F5.1. th32ParentProcessID comes free on the entry already being
            // walked, so the parent costs nothing beyond this snapshot. The
            // alternative - NtQueryInformationProcess(ProcessBasicInformation)
            // per PID - is one kernel round trip per process per refresh to
            // learn the same number.
            se.ppid = pe.th32ParentProcessID;
            snapshot_.emplace(pe.th32ProcessID, std::move(se));
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
        // F5.2. Read while the handle is open rather than reopening it: this
        // is the only handle the function has, and OpenProcessToken needs one.
        // A failure is silent by construction - ReadIntegrity answers
        // kIntegrityUnknown and the column shows the em-dash that every other
        // unreadable reading uses.
        const IntegrityReading ir = ReadIntegrity(h);
        e.integrity = ir.level;
        e.appContainer = ir.appContainer;
        ::CloseHandle(h);
    }

    // F5.1. The snapshot is taken at most once per batch, and is now wanted for
    // EVERY pid rather than only for a name that fell through - because the
    // parent is part of what a row shows. It is still one CreateToolhelp32Snapshot
    // per refresh, which is the cost this design already budgets for; the
    // previous "only on a name miss" rule could not supply a parent for a
    // process whose image path DID resolve.
    BuildSnapshotIfNeeded();
    {
        const auto it = snapshot_.find(pid);
        if (it != snapshot_.end()) {
            if (e.name.empty() && !it->second.name.empty())
                e.name = it->second.name;
            // ppidKnown is set even when ppid is 0: PID 0 is a real parent
            // answer (a process created by the kernel itself), and calling it
            // "unknown" would make those rows indistinguishable from the ones
            // the snapshot did not cover.
            e.ppid = it->second.ppid;
            e.ppidKnown = true;
            // A parent id equal to its own child is the classic Toolhelp race
            // (the parent exited and its id was recycled between the walking of
            // the two entries), and printing the child as its own parent would
            // be nonsense in a tree view.
            if (e.ppid != 0 && e.ppid != pid) {
                const auto pit = snapshot_.find(e.ppid);
                if (pit != snapshot_.end()) e.parentName = pit->second.name;
            }
        }
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

    // F5.3. LAST, because it needs the resolved image path, and only when the
    // caller opted in. A row whose path never resolved stays kSigUnchecked,
    // which renders as the em-dash - not as "unsigned", because an image
    // WinTCP could not find is not an image it has cleared.
    if (verifySignatures_ && !e.path.empty()) {
        e.signature = SignatureForPath(e.path);
    }
}

SignatureState ProcessResolver::SignatureForPath(const std::wstring& path) {
    // The pseudo-images for pid 0 / 4 have a "path" that is a label, not a
    // file. Asking the trust provider about them would only produce a
    // confusing NOSIGNATURE for a string.
    if (path.empty() || path == L"System" || path.find(L':') == std::wstring::npos)
        return kSigUnchecked;

    const ImageStamp stamp = StampOf(path);
    const auto it = signatureCache_.find(path);
    if (it != signatureCache_.end() && SameStamp(stamp, it->second.second))
        return it->second.first;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO fi = {};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = path.c_str();

    WINTRUST_DATA wd = {};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;          // never pop the trust dialog
    // Revocation is OFF deliberately. WTD_REVOKE_WHOLECHAIN would let the
    // provider reach a CRL/OCSP endpoint, turning a background verification
    // into a network wait that can outlive the refresh that asked for it. A
    // revoked-but-not-yet-expired certificate still reports as Signed here;
    // that is a documented limit of an offline check, not a silent one.
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    // nullptr, not INVALID_HANDLE_VALUE: the first parameter is an HWND (the
    // window a trust UI would parent to, and WTD_UI_NONE means there is none).
    // Passing a non-null HWND would tie a background verification to a window.
    const LONG rc = ::WinVerifyTrust(nullptr, &action, &wd);

    // The close leg is mandatory: WinVerifyTrust keeps state in the WINTRUST_DATA
    // between the VERIFY and CLOSE calls, and skipping it leaks per verification.
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    ::WinVerifyTrust(nullptr, &action, &wd);

    const SignatureState state = StateFromTrustLONG(rc);
    // A path whose stamp could not be read is still cached: an unreadable file
    // will not become readable on the next refresh, and re-running the trust
    // provider every refresh to reach the same answer is pure cost. The entry
    // is invalidated only if a LATER stamp is readable and different.
    signatureCache_[path] = std::make_pair(state, stamp);
    return state;
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
    // The snapshot is rebuilt at most once per batch. Since F5.1 every pid
    // wants it, not only a name lookup that missed - the cost is still one
    // CreateToolhelp32Snapshot per refresh, and BuildSnapshotIfNeeded is what
    // keeps it to exactly one even though ResolveOne asks for every pid.
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
    // F5.3 leans on this harder than anything else here: without it every ROW
    // of a process would call WinVerifyTrust for the same image, which is the
    // difference between one chain build per process and one per connection.
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
        // F5.1 / F5.2 / F5.3. Copied rather than pointed at, exactly like every
        // other piece of resolved metadata: the row outlives the entry whenever
        // the process dies and its cache slot is erased.
        c.ppid = e.ppid;
        c.ppidKnown = e.ppidKnown;
        c.parentName = e.parentName;
        c.integrity = static_cast<unsigned>(e.integrity);
        c.appContainer = e.appContainer;
        c.signature = static_cast<unsigned>(e.signature);
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
