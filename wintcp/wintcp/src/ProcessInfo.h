// ProcessInfo.h
// PID -> process name / path / creation-time resolution with a persistent,
// creation-time-validated cache, plus service-name and command
// line queries (tasks 17 / 20).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "Connection.h"

namespace wintcp {

// F5.2: a process's integrity level, from TokenIntegrityLevel's mandatory
// label. The RID is what the label encodes (S-1-16-<rid>), and naming the
// levels rather than printing RIDs is the point: 12288 tells a reader nothing,
// "High" tells them whether the process could have written to HKLM.
enum IntegrityLevel : unsigned {
    kIntegrityUnknown = 0,
    kIntegrityUntrusted,    // S-1-16-0      - nothing has been assigned
    kIntegrityLow,          // S-1-16-4096
    kIntegrityMedium,       // S-1-16-8192   - the default for a normal app
    kIntegrityHigh,         // S-1-16-12288  - elevated, can touch HKLM
    kIntegritySystem,       // S-1-16-16384  - services and drivers
    kIntegrityProtected,    // S-1-16-28672  - Protected Process Light
    kIntegrityCount
};

// F5.3: the Authenticode verdict for an image, from WinVerifyTrust.
//
// kSigUnsigned is deliberately NOT the same as kSigInvalid. "Unsigned" is the
// honest answer for most of what runs (a script host, a portable binary) and is
// not a defect; conflating it with "invalid" would paint most of a machine red
// and train the reader to ignore the colour, which is the only thing a highlight
// exists to avoid. kSigError is separate again: that is WinTCP failing to ask,
// which says nothing about the binary.
enum SignatureState : unsigned {
    kSigUnchecked = 0,
    kSigValid,               // chains to a root the machine trusts
    kSigInvalid,             // carries a signature that does not verify
    kSigUnsigned,            // no embedded Authenticode signature
    kSigError,               // the trust provider could not decide
    kSigCount
};

// Display labels, exposed for selftest so the wording is pinned.
const wchar_t* IntegrityLabel(IntegrityLevel lvl);
const wchar_t* SignatureLabel(SignatureState state);

// Map a TokenIntegrityLevel mandatory-label RID onto a level. Exposed because
// the RID -> name mapping is pure policy and must not depend on a live token.
IntegrityLevel IntegrityFromRid(DWORD rid);

// F5.3: the FILE fingerprint a cached signature verdict was taken against.
// Present in this header only because it appears in the resolver's private
// cache type below; it carries no behaviour of its own.
//
// It exists because a path is not a stable identity for a file. Without it a
// binary updated in place would keep the verdict of the file it replaced -
// which is precisely the case where a signature check matters most, since
// replacing a signed binary with an unsigned one is the attack it exists to
// catch.
struct ImageStamp {
    ULONGLONG size = 0;      // bytes
    ULONGLONG write = 0;     // FILETIME of last write, raw
    bool valid = false;
};

// Resolves process names for PIDs seen during a refresh.
// The cache persists across refreshes; entries are revalidated cheaply via
// OpenProcess + GetProcessTimes (creation time), so a recycled PID is never
// shown with a stale name, and a Toolhelp snapshot is taken at most once per
// batch and only when some PID actually needs it.
class ProcessResolver {
public:
    ProcessResolver() = default;
    ProcessResolver(const ProcessResolver&) = delete;
    ProcessResolver& operator=(const ProcessResolver&) = delete;

    // Fill processName / processPath / processCreate* for every row.
    void ResolveBatch(std::vector<Connection>& rows);

    void Clear();

    // F5.3. Authenticode verification is OPT-IN and off by default, because
    // it cannot be anything else: WinVerifyTrust opens the file, builds a
    // certificate chain and may consult a revocation endpoint over the
    // network. That is tens to hundreds of milliseconds per image, which
    // would turn a refresh that currently costs one Toolhelp snapshot into one
    // that stalls for a second per process.
    //
    // It is cached per PATH rather than per PID, which is both cheaper and more
    // correct: a file's signature cannot change while a process is running it,
    // and twenty processes off one DLL share one verdict instead of paying
    // twenty trust-provider round trips.
    void SetVerifySignatures(bool on) { verifySignatures_ = on; }
    bool VerifySignatures() const { return verifySignatures_; }

    // Verify that 'pid' still is the same process instance that produced
    // 'expectedCreate' (when 'expectKnown') before terminating it.
    // Returns false and sets 'errorMessage' when it is unsafe to proceed.
    static bool VerifyProcess(DWORD pid, const FILETIME& expectedCreate,
                              bool expectKnown, std::wstring& errorMessage);

private:
    struct Entry {
        std::wstring name;
        std::wstring path;
        FILETIME create = {};
        bool createKnown = false;
        // F5.1. The parent, from the same Toolhelp snapshot that can supply a
        // name. ppidKnown is separate because the two fail independently: a
        // name can come from a snapshot carrying no PPID for a just-created
        // process, and a PPID can be readable while the name is not.
        DWORD ppid = 0;
        bool ppidKnown = false;
        std::wstring parentName;   // "" when the parent is not in the snapshot
        // F5.2 / F5.3.
        IntegrityLevel integrity = kIntegrityUnknown;
        // An AppContainer process runs at Low integrity by default, so the
        // level alone does not reveal that it is sandboxed. The two are kept
        // apart rather than flattened, because "Low" on its own makes every
        // sandboxed app indistinguishable from an ordinary low-integrity one.
        bool appContainer = false;
        SignatureState signature = kSigUnchecked;
    };

    // What ONE Toolhelp pass tells us about a process. Both fields come from the
    // same PROCESSENTRY32W, so carrying them together means the parent costs no
    // extra kernel call - which is the whole reason F5.1 reads the snapshot
    // rather than NtQueryInformationProcess per PID.
    struct SnapEntry {
        std::wstring name;
        DWORD ppid = 0;
    };

    void BuildSnapshotIfNeeded();
    void ResolveOne(DWORD pid, Entry& e);

    // F5.3. The SignatureState for one image path, together with the stamp it was
    // taken against. Keyed by path, not by PID - see the note on
    // SetVerifySignatures.
    SignatureState SignatureForPath(const std::wstring& path);

    // Resolve (or revalidate) ONE pid and return the stable cache entry for
    // it. The answer depends only on the pid and the cached entry - never on
    // which row asked - which is what lets ResolveBatch ask this once per
    // distinct pid instead of once per row. The returned pointer stays valid
    // across later cache_ inserts (node-based container: inserting or
    // rehashing never moves an element); it is only ever invalidated by
    // erasing this very pid, which happens before the pointer is handed out.
    const Entry* ResolvePid(DWORD pid);

    std::unordered_map<DWORD, Entry> cache_;
    std::unordered_map<DWORD, SnapEntry> snapshot_;   // one per batch
    bool snapshotBuilt_ = false;
    // F5.3: path -> (verdict, the file stamp the verdict is valid for). A hit
    // whose stamp no longer matches the file on disk is re-verified.
    std::unordered_map<std::wstring, std::pair<SignatureState, ImageStamp>>
        signatureCache_;
    bool verifySignatures_ = false;
};

// Map PID -> hosting service names ("Dnscache, Dnscache...") with a single
// SCM enumeration. Best effort: on failure 'out' stays empty (the Service
// column is optional decoration). Needs advapi32.
void QueryServiceNames(std::map<DWORD, std::wstring>& out);

// Full command line of a process (best effort via NtQueryInformationProcess
// ProcessCommandLineInformation; empty when unavailable).
std::wstring QueryProcessCommandLine(DWORD pid);

// Ask every top-level window owned by 'pid' to close, the way a user
// closing a window would (WM_CLOSE to each). Returns true if at least one
// message was posted, false if the process has no top-level windows - which
// is the normal case for a service or a console app, and the reason the
// caller needs a forceful fallback rather than merely waiting.
//
// Used by the graceful end-process path so an app that can save
// on close gets the chance to do so.
bool PostCloseToProcessWindows(DWORD pid);

}  // namespace wintcp
