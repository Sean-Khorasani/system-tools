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
    };

    void BuildSnapshotIfNeeded();
    void ResolveOne(DWORD pid, Entry& e);

    std::unordered_map<DWORD, Entry> cache_;
    std::unordered_map<DWORD, std::wstring> snapshot_;   // one per batch
    bool snapshotBuilt_ = false;
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
