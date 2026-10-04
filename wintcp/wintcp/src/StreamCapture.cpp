// StreamCapture.cpp
// See StreamCapture.h. Process spawning, temp files and pktmon invocation
// live here and nowhere else.

#include "StreamCapture.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "Elevate.h"
#include "Pcapng.h"

namespace wintcp {
namespace {

// How long any pktmon stage may take before it is given up on. A ceiling, not
// a wait: the return value is only used to decide success, and pktmon's own
// output is discarded, so the number exists to stop a wedged child from
// hanging the capture forever. 2 min is far above a real conversion (seconds on
// a filtered capture) and far below "the user gave up".
constexpr DWORD kToolTimeoutMs = 120000;

// Fixed capacities for the command lines and paths built below. Named because
// each one is the buffer for a format string whose length depends on a PATH, and
// a path that outgrows the buffer is a silently truncated command — pktmon then
// reports a syntax error about a file the user never named.
constexpr size_t kAddrFilterChars = 64;        // widest address, plus NUL
constexpr size_t kTempNameChars = 64;          // "wintcp-stream-<pid>-<tick>"
constexpr size_t kFilterArgChars = 96;         // "-t TCP -p N -p N -i <addr>"
constexpr size_t kMaxPathPlus = MAX_PATH + 64;      // a path plus its switches
constexpr size_t kTwoMaxPathPlus = 2 * MAX_PATH + 64;  // two quoted paths
constexpr size_t kOverSizeMsgChars = 160;      // the "too large" message

// Run a console tool with no window and capture its exit code. Output is
// discarded: pktmon's messages are diagnostics, and the caller reports a
// stage name instead, which reads better than a wall of text.
int RunTool(const std::wstring& exe, const std::wstring& args) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    // CREATE_NO_WINDOW is what keeps a console flash from appearing behind
    // the GUI on every capture.
    if (::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) == 0)
        return -1;
    // If the tool hangs we must NOT just close the handle and walk away:
    // pktmon.exe then keeps running, alone and locked onto the capture
    // drivers, indefinitely. A timeout is an error, and the only sane
    // recovery is to kill the child before we report failure.
    const DWORD wait = ::WaitForSingleObject(pi.hProcess, kToolTimeoutMs);
    if (wait == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 2000);   // let it die
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return -2;   // distinct from -1 (launch failure)
    }
    DWORD code = 0;
    if (::GetExitCodeProcess(pi.hProcess, &code) == 0) code = 0xFFFFFFFFu;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::wstring PktmonPath() {
    wchar_t buf[MAX_PATH] = {0};
    const DWORD n = ::GetSystemDirectoryW(buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"pktmon.exe";
    return std::wstring(buf, n) + L"\\pktmon.exe";
}

// A temp path in the user's temp dir, created with a distinctive name so a
// stray file is recognisable. Returns empty on failure.
std::wstring MakeTempPath(const wchar_t* ext) {
    wchar_t dir[MAX_PATH] = {0};
    if (::GetTempPathW(MAX_PATH, dir) == 0) return std::wstring();
    wchar_t name[kTempNameChars] = {0};
    ::swprintf_s(name, L"wintcp-stream-%lu-%lu%s",
                 ::GetCurrentProcessId(), ::GetTickCount(), ext);
    return std::wstring(dir) + name;
}

void RemoveIfPresent(const std::wstring& p) {
    if (!p.empty()) ::DeleteFileW(p.c_str());
}

// The ETL path chosen by StartCapture, read back by StopCapture. A file-scope
// variable rather than a ".path" sidecar file: two instances (or a crashed
// run leaving a stale sidecar) would otherwise disagree about which file is
// live, and a stale file would send StopCapture to convert something else.
std::wstring g_pendingEtl;

// Format an address for the pktmon -i filter. pktmon accepts bare IPv4; for
// IPv6 the plain form is used too, which is what its own help shows.
std::wstring FormatAddrForFilter(const unsigned char* a, bool v4) {
    wchar_t buf[kAddrFilterChars] = {0};
    if (v4) {
        ::swprintf_s(buf, L"%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    } else {
        ::swprintf_s(buf, L"%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                         L"%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                     a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                     a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
    }
    return buf;
}

}  // namespace

// Elevation is delegated to Elevate.cpp, which owns the token logic and the
// "can this user elevate at all" question. The extra function here would
// exist only to rename it, so it is gone.
bool CaptureAvailable(std::wstring* whyNot) {
    // pktmon's driver is only loaded for an elevated process. If this process
    // is not elevated but the user COULD elevate, the correct answer is "not
    // right now" rather than "impossible" - the caller offers a relaunch.
    // See Elevate.h for why the whole process relaunches.
    if (!IsElevated()) {
        if (whyNot != nullptr) *whyNot = ElevationUnavailableReason();
        return false;
    }
    if (::GetFileAttributesW(PktmonPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (whyNot != nullptr)
            *whyNot = L"pktmon.exe was not found in the system directory.";
        return false;
    }
    return true;
}

void ClearCaptureFilter() {
    RunTool(PktmonPath(), L"filter remove");
}

bool StartCapture(const CaptureTarget& target, std::wstring* error) {
    const std::wstring exe = PktmonPath();
    const auto fail = [error](const wchar_t* what) {
        if (error != nullptr) *error = what;
        return false;
    };

    // The filter set is global and persists after our process exits, so it is
    // cleared both before and after. A leftover filter would silently
    // narrow some later, unrelated capture.
    RunTool(exe, L"filter remove");

    wchar_t ports[kFilterArgChars] = {0};
    ::swprintf_s(ports, L"-t TCP -p %u -p %u -i %s", target.localPort,
                 target.remotePort,
                 FormatAddrForFilter(target.localAddr, target.ipV4).c_str());
    if (RunTool(exe, std::wstring(L"filter add wintcp ") + ports) != 0)
        return fail(L"Could not start packet capture (pktmon filter). "
                    L"Another capture tool may be running.");

    const std::wstring etl = MakeTempPath(L".etl");
    if (etl.empty()) return fail(L"Could not create a temporary file name.");

    wchar_t start[kMaxPathPlus] = {0};
    // --pkt-size 0 = no truncation. Omitting it truncates each packet and
    // the stream becomes unreassemblable.
    ::swprintf_s(start, L"start --capture --pkt-size 0 --file-name \"%s\"",
                 etl.c_str());
    if (RunTool(exe, start) != 0) {
        RunTool(exe, L"filter remove");
        return fail(L"Could not start packet capture (pktmon start).");
    }
    g_pendingEtl = etl;
    return true;
}

CaptureResult StopCapture(const CaptureTarget& target) {
    CaptureResult res;
    const std::wstring exe = PktmonPath();

    // Stop must precede the conversion: pktmon only merges and flushes the
    // ETL when it is stopped, and etl2pcap on an unflushed file yields
    // nothing at all.
    res.stage = L"pktmon stop";
    RunTool(exe, L"stop");
    ClearCaptureFilter();

    // The ETL path was chosen by StartCapture and kept in g_pendingEtl.
    const std::wstring etl = g_pendingEtl;
    g_pendingEtl.clear();
    if (etl.empty()) {
        res.stage = L"pktmon start";
        res.error = L"Capture was never started, so there is nothing to read.";
        return res;
    }

    std::wstring pcapng = etl;
    const size_t dot = pcapng.rfind(L'.');
    if (dot != std::wstring::npos) pcapng.resize(dot);
    pcapng += L".pcapng";

    res.stage = L"pktmon etl2pcap";
    wchar_t conv[kTwoMaxPathPlus] = {0};
    // etl.c_str(), not etl: a std::wstring passed to a variadic function is
    // bitwise-copied, so its buffer is never read and the path comes out as
    // whatever happened to be on the stack.
    ::swprintf_s(conv, L"etl2pcap \"%s\" --out \"%s\"", etl.c_str(),
                 pcapng.c_str());
    if (RunTool(exe, conv) != 0) {
        RemoveIfPresent(etl);
        RemoveIfPresent(pcapng);
        res.error = L"pktmon could not convert the capture to pcapng.";
        return res;
    }

    // Read the whole pcapng. ParsedPacket holds views into this buffer, so
    // it must stay alive for as long as the results do - hence 'buffer' is
    // declared before them and released only at the end of this function,
    // with the reassembled bytes copied out beforehand.
    res.stage = L"read";
    HANDLE f = ::CreateFileW(pcapng.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        RemoveIfPresent(etl);
        RemoveIfPresent(pcapng);
        res.error = L"The converted capture file could not be opened.";
        return res;
    }
    // Largest capture this tool will read into memory in one go. 512 MB is far
    // past any reasonable filtered capture and safely below the 2 GB/4 GB
    // cliffs further down (vector size, DWORD read length); the number and the
    // user-facing prose below come from this one constant so they cannot
    // disagree about the limit. B4: dev constant — memory policy, not a knob.
    constexpr LONGLONG kMaxCaptureBytes = 512ll * 1024 * 1024;
    LARGE_INTEGER size = {};
    ::GetFileSizeEx(f, &size);
    if (size.QuadPart <= 0 || size.QuadPart > kMaxCaptureBytes) {
        ::CloseHandle(f);
        RemoveIfPresent(etl);
        RemoveIfPresent(pcapng);
        if (size.QuadPart <= 0) {
            res.error = L"The capture produced no packets.";
        } else {
            // The MB figure is derived from the constant, not retyped: the
            // message and the check cannot disagree about the limit.
            wchar_t over[kOverSizeMsgChars] = {0};
            ::swprintf_s(over, L"The capture is larger than %lld MB; narrow it "
                               L"with a filter and try again.",
                         kMaxCaptureBytes / (1024 * 1024));
            res.error = over;
        }
        return res;
    }
    std::vector<unsigned char> buffer(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const BOOL readOk =
        ::ReadFile(f, buffer.data(), static_cast<DWORD>(buffer.size()), &got,
                   nullptr);
    ::CloseHandle(f);
    res.fileBytes = got;

    // Temp files go away now regardless of what follows: they can be tens of
    // megabytes and are not worth keeping after a parse.
    RemoveIfPresent(etl);
    RemoveIfPresent(pcapng);

    if (readOk == FALSE) {
        res.stage = L"read";
        res.error = L"The converted capture file could not be read.";
        return res;
    }

    res.stage = L"parse";
    const PcapngParse parse = ParsePcapng(buffer.data(), got);
    res.packetsParsed = parse.packets.size();
    res.flowRecords = parse.flowRecords;
    res.blocksSeen = parse.blocksSeen;
    res.linkType = parse.linkType;
    if (!parse.ok) {
        res.error = L"Could not read the capture: " + parse.error;
        return res;
    }

    res.stage = L"reassemble";
    TcpKey want;
    std::memcpy(want.addrA, target.localAddr, 16);
    std::memcpy(want.addrB, target.remoteAddr, 16);
    want.portA = target.localPort;
    want.portB = target.remotePort;
    // Canonicalise: the two endpoints are given in caller order, but
    // MakeTcpKey-style sorting is what the packets were matched against.
    {
        const int c = std::memcmp(want.addrA, want.addrB, 16);
        const bool swap = (c > 0) || (c == 0 && want.portA > want.portB);
        if (swap) {
            unsigned char ta[16];
            std::memcpy(ta, want.addrA, 16);
            std::memcpy(want.addrA, want.addrB, 16);
            std::memcpy(want.addrB, ta, 16);
            const uint16_t tp = want.portA;
            want.portA = want.portB;
            want.portB = tp;
        }
    }
    res.stats = ReassembleStream(parse.packets, want, &res.toServer,
                                 &res.toClient);
    res.complete = res.stats.sawSyn && res.stats.sawFin && !res.stats.sawRst;
    res.ok = (res.stats.packetsMatched > 0);
    if (!res.ok) {
        res.error =
            L"No packets for this connection were captured. The connection "
            L"may have been idle while the capture was running, or it may "
            L"have been established before it started.";
    }
    return res;
}

CaptureTarget MakeCaptureTarget(const Connection& c) {
    CaptureTarget t;
    // Connection stores ports as UINT (they arrive that way from the MIB),
    // the capture target as uint16_t. The cast is exact: a TCP/UDP port is
    // 16-bit by definition, so the value can never be out of range.
    t.localPort = static_cast<uint16_t>(c.localPort);
    t.remotePort = static_cast<uint16_t>(c.remotePort);
    const bool v6 = (c.family == AF_INET6);
    t.ipV4 = !v6;
    if (v6) {
        ::memcpy(t.localAddr, c.local6.s6_addr, 16);
        ::memcpy(t.remoteAddr, c.remote6.s6_addr, 16);
    } else {
        ::memcpy(t.localAddr, &c.local4, 4);
        ::memcpy(t.remoteAddr, &c.remote4, 4);
    }
    // The endpoint strings ALREADY include the port - appending it again
    // would render "10.0.0.5:52341:52341". Use them as-is.
    t.label = c.localEndpoint + L"  \xE2\x86\x94  " + c.remoteEndpoint;
    return t;
}

}  // namespace wintcp
