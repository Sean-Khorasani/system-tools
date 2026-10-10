// wintcp_benchmark.cpp
// SPDX-License-Identifier: Apache-2.0
//
// Comprehensive correctness and performance benchmark for the
// assembly/intrinsic-optimized replacements of seven wintcp
// hot paths (see wintcp/tests/asm/opt_functions.h).
//
// ONE source, TWO executables:
//
//   bench_orig.exe  links orig_functions.cpp    - the original
//                       wintcp C++ implementations (faithful
//                       copies, cited file:line per function)
//   bench_asm.exe   links replaced_functions.cpp - the same
//                       copies with the C++ bodies commented out
//                       asm_functions.cpp          - delegates to
//                       wintcp_asm_opt.lib         the optimized
//                                                  implementations
//
// Because both variants implement the same wintcp::bench
// interface (bench_api.h), this harness is compiled identically
// for both and the only difference is the linked code under
// test. The report script (run_benchmark_report.py) runs both
// executables repeatedly, compares the correctness results
// test-by-test and computes performance statistics.
//
// Usage:
//   wintcp_benchmark.exe                    human-readable output
//   wintcp_benchmark.exe --json             JSON output
//   wintcp_benchmark.exe --phase=correctness|bench|all
//   wintcp_benchmark.exe --rounds=N --time-ms=MS
//   wintcp_benchmark.exe --quick            3 rounds x 40 ms
//   wintcp_benchmark.exe --variant=NAME     override the
//                                           variant label

#include "bench_api.h"

using wintcp::bench::GeoIpTree;
using wintcp::bench::ReasmSeg;
using wintcp::bench::ReasmWorkload;
using wintcp::bench::ReasmOverlap;
using wintcp::bench::GeoIpWalk;
using wintcp::bench::TcpHeaderFields;
using wintcp::bench::ParseTcpHeader;
using wintcp::bench::FormatBytes;
using wintcp::bench::ConnKeyInput;
using wintcp::bench::KeyOf;
using wintcp::bench::WideToUtf8;
using wintcp::bench::HasLowerSubstring;
using wintcp::bench::FormatStreamHex;
using wintcp::bench::ToLowerW;
using wintcp::bench::GeoReadU32BE;
using wintcp::bench::GeoReadU64BE;
using wintcp::bench::GeoReadBytes;
using wintcp::bench::TlsBe16;
using wintcp::bench::TlsBe24;
using wintcp::bench::TlsBe32;
using wintcp::bench::TlsExtAction;
using wintcp::bench::ClassifyTlsExtension;
using wintcp::bench::StripSniTail;
using wintcp::bench::Rd16;
using wintcp::bench::Rd32;
using wintcp::bench::JoinSamples;
using wintcp::bench::FindSubstringLong;
using wintcp::bench::BuildLowerAll;
using wintcp::bench::CompareWide;
using wintcp::bench::HashConnKey;
using wintcp::bench::EqualConnKey;
using wintcp::bench::ComputeBpsBatch;
using wintcp::bench::SumPidTraffic;
using wintcp::bench::FormatBpsCell;
using wintcp::bench::WidenUtf8;
using wintcp::bench::IsGlobalV4;
using wintcp::bench::IsGlobalV6;
using wintcp::bench::PairSnapshot;
using wintcp::bench::FindCountedKey;
using wintcp::bench::FlowKeyEqual;
using wintcp::bench::CmpFlowAddr;
using wintcp::bench::FilterHandles;
using wintcp::bench::ClassifyEvent;
using wintcp::bench::ParseEventPayload;
using wintcp::bench::JsonEscape;
using wintcp::bench::CsvEscape;
using wintcp::bench::FormatPort;
using wintcp::bench::FormatU64Dec;
using wintcp::bench::FormatDuration;
using wintcp::bench::FormatIpv4;
using wintcp::bench::FormatIpv6;
using wintcp::bench::CpWidth;
using wintcp::bench::NextCp;
using wintcp::bench::DisplayWidth;
using wintcp::bench::TruncateToWidth;
using wintcp::bench::PayloadSize;
using wintcp::bench::ReadPointer;
using wintcp::bench::GeoIpWalkShift;
using wintcp::bench::SkipVlan;
using wintcp::bench::FlowProbe;
using wintcp::bench::RenderSegments;
using wintcp::bench::DistinctPids;
using wintcp::bench::GroupByPid;
using wintcp::bench::ReasmRenderView;
using wintcp::bench::ReasmRenderResult;
using wintcp::bench::PidGroups;
using wintcp::HandleEntry;
using wintcp::PidHandle;
using wintcp::TrafficDir;
using wintcp::SnapKey;
using wintcp::CountedKey;
using wintcp::FlowKey;
using wintcp::PidTrafficRow;
using wintcp::JoinRow;
using wintcp::JoinSample;
using wintcp::MmdbPayload;

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ---------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------

// Global sink so the compiler cannot optimize away the
// benchmarked calls (every workload folds its outputs in).
static volatile uint64_t g_sink = 0;

// Deterministic PRNG (PCG-style linear congruential) so every
// run of every variant sees identical inputs.
static uint64_t NextRand(uint64_t& state) {
    state = state * 6364136223846793005ull +
            1442695040888963407ull;
    return state >> 33;
}

static std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) {
        s += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    return s;
}

static std::string ToHex(const unsigned char* p, size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += kHex[p[i] >> 4];
        s += kHex[p[i] & 0x0F];
    }
    return s;
}

static std::string JsonStr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf),
                                      "\\u%04x",
                                      static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

static std::string Num(double v, int prec = 3) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return buf;
}

// ---------------------------------------------------------------------
// Build / environment info (reported by both variants)
// ---------------------------------------------------------------------

struct BuildInfo {
    std::string compiler;
    std::string arch;
    std::string cpuName;
    size_t ramMb = 0;
    std::string osName;
    bool sse42 = false, avx2 = false, bmi1 = false, bmi2 = false,
         popcnt = false;
};

static BuildInfo CollectBuildInfo() {
    BuildInfo bi;
    bi.compiler = "MSVC " + std::to_string(_MSC_FULL_VER);
#ifdef _M_X64
    bi.arch = "x64";
#elif defined(_M_IX86)
    bi.arch = "x86";
#else
    bi.arch = "unknown";
#endif

    int regs[4] = {0, 0, 0, 0};
    __cpuid(regs, 1);
    bi.popcnt = (regs[2] & (1 << 23)) != 0;
    bi.sse42 = (regs[2] & (1 << 20)) != 0;
    __cpuidex(regs, 7, 0);
    bi.bmi1 = (regs[1] & (1 << 3)) != 0;
    bi.bmi2 = (regs[1] & (1 << 8)) != 0;
    bi.avx2 = (regs[1] & (1 << 5)) != 0;

    // CPU name from the registry.
    HKEY h = nullptr;
    if (::RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ | KEY_WOW64_64KEY, &h) == ERROR_SUCCESS) {
        wchar_t name[256] = {0};
        DWORD size = sizeof(name);
        if (::RegQueryValueExW(h, L"ProcessorNameString", nullptr,
                                  nullptr,
                                  reinterpret_cast<BYTE*>(name),
                                  &size) == ERROR_SUCCESS) {
            bi.cpuName = Narrow(name);
        }
        ::RegCloseKey(h);
    }

    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (::GlobalMemoryStatusEx(&ms)) {
        bi.ramMb = static_cast<size_t>(ms.ullTotalPhys /
                                           (1024ull * 1024ull));
    }

    // OS name from the registry.
    HKEY o = nullptr;
    if (::RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
            0, KEY_READ | KEY_WOW64_64KEY, &o) == ERROR_SUCCESS) {
        wchar_t product[128] = {0};
        wchar_t build[32] = {0};
        DWORD size = sizeof(product);
        ::RegQueryValueExW(o, L"ProductName", nullptr, nullptr,
                              reinterpret_cast<BYTE*>(product),
                              &size);
        size = sizeof(build);
        ::RegQueryValueExW(o, L"CurrentBuildNumber", nullptr,
                              nullptr,
                              reinterpret_cast<BYTE*>(build),
                              &size);
        bi.osName = Narrow(product) + " (build " + Narrow(build) + ")";
        ::RegCloseKey(o);
    }
    return bi;
}

static std::string ExeBaseName() {
    wchar_t path[MAX_PATH] = {0};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    const size_t slash = p.find_last_of(L"\\/");
    std::wstring base =
        (slash == std::wstring::npos) ? p : p.substr(slash + 1);
    const size_t dot = base.find_last_of(L'.');
    return Narrow(dot == std::wstring::npos
                      ? base
                      : base.substr(0, dot));
}

// ---------------------------------------------------------------------
// Synthetic GeoIP tree builder
// ---------------------------------------------------------------------
//
// Layout mirrors a real MaxMind-style database file:
//   [ node table: nodeCount * nodeByteSize ]
//   [ 16-byte separator ]
//   [ data section: dataSize bytes ]

struct SyntheticTree {
    std::vector<unsigned char> buffer;
    GeoIpTree tree;
};

// Writes one record (half 0 = left, 1 = right) of value 'rec'
// into a node at 'nodePos'.
static void WriteRecord(unsigned char* nodeBase, size_t nodeByteSize,
                          bool record28, size_t recordBytes,
                          unsigned half, size_t rec) {
    if (record28) {
        // 28-bit record: 7 bytes per node, top 4 bits of both
        // halves share the middle byte (GeoIp.cpp:953-970).
        unsigned char* p = nodeBase;
        const size_t left = half == 0 ? rec : 0;
        const size_t right = half == 1 ? rec : 0;
        // Read the existing sibling nibble first so a second
        // WriteRecord call does not clobber it.
        const unsigned char mid = p[3];
        if (half == 0) {
            p[0] = static_cast<unsigned char>((left >> 16) & 0xFF);
            p[1] = static_cast<unsigned char>((left >> 8) & 0xFF);
            p[2] = static_cast<unsigned char>(left & 0xFF);
            p[3] = static_cast<unsigned char>(
                (mid & 0x0Fu) |
                (static_cast<unsigned char>((left >> 24) & 0x0Fu)
                 << 4));
        } else {
            p[4] = static_cast<unsigned char>((right >> 16) & 0xFF);
            p[5] = static_cast<unsigned char>((right >> 8) & 0xFF);
            p[6] = static_cast<unsigned char>(right & 0xFF);
            p[3] = static_cast<unsigned char>(
                (mid & 0xF0u) |
                (static_cast<unsigned char>((right >> 24) & 0x0Fu)));
        }
        (void)nodeByteSize;
        (void)recordBytes;
        return;
     }
    if (recordBytes == 4) {
        unsigned char* p = nodeBase + (half == 0 ? 0 : 4);
        p[0] = static_cast<unsigned char>((rec >> 24) & 0xFF);
        p[1] = static_cast<unsigned char>((rec >> 16) & 0xFF);
        p[2] = static_cast<unsigned char>((rec >> 8) & 0xFF);
        p[3] = static_cast<unsigned char>(rec & 0xFF);
    } else {
        // 3-byte record (24-bit).
        unsigned char* p = nodeBase + (half == 0 ? 0 : 3);
        p[0] = static_cast<unsigned char>((rec >> 16) & 0xFF);
        p[1] = static_cast<unsigned char>((rec >> 8) & 0xFF);
        p[2] = static_cast<unsigned char>(rec & 0xFF);
    }
}

static SyntheticTree MakeTree(size_t nodeCount, bool record28,
                                size_t recordBytes, uint64_t seed,
                                size_t dataSize, double pRef,
                                double pData) {
    SyntheticTree st;
    st.tree.nodeCount = nodeCount;
    st.tree.record28 = record28;
    // Mirrors GeoIp.cpp:825-826: recordBytes is 3 even for 28-bit
    // databases (only the node layout differs).
    st.tree.recordBytes = record28 ? 3 : recordBytes;
    st.tree.nodeByteSize =
        record28 ? 7 : recordBytes * 2;
    st.tree.treeSize = nodeCount * st.tree.nodeByteSize;
    st.tree.dataSectionSize = dataSize;
    st.tree.fileSize = st.tree.treeSize + 16 + dataSize;
    st.buffer.assign(st.tree.fileSize, 0);
    st.tree.view = st.buffer.data();

    uint64_t rng = seed;
    for (size_t node = 0; node < nodeCount; ++node) {
        unsigned char* nodeBase =
            st.buffer.data() + node * st.tree.nodeByteSize;
        for (unsigned half = 0; half < 2; ++half) {
            const double roll =
                static_cast<double>(NextRand(rng) % 1000) / 1000.0;
            size_t rec;
            if (roll < pRef) {
                rec = NextRand(rng) % nodeCount;
            } else if (roll < pRef + pData) {
                rec = nodeCount + 16 + (NextRand(rng) % dataSize);
            } else {
                rec = nodeCount;  // no data
            }
            WriteRecord(nodeBase, st.tree.nodeByteSize, record28,
                          recordBytes, half, rec);
        }
    }
    return st;
}

// A chain: node i's left record points at node i+1, and the
// last node's left record points at a data record. Walking
// all-zero bits descends the chain one node per bit.
static SyntheticTree MakeChainTree(size_t nodeCount, bool record28,
                                     size_t recordBytes,
                                     size_t chainLen,
                                     size_t dataSize,
                                     size_t dataOffset) {
    SyntheticTree st;
    st.tree.nodeCount = nodeCount;
    st.tree.record28 = record28;
    // Mirrors GeoIp.cpp:825-826: recordBytes is 3 even for 28-bit
    // databases (only the node layout differs).
    st.tree.recordBytes = record28 ? 3 : recordBytes;
    st.tree.nodeByteSize = record28 ? 7 : recordBytes * 2;
    st.tree.treeSize = nodeCount * st.tree.nodeByteSize;
    st.tree.dataSectionSize = dataSize;
    st.tree.fileSize = st.tree.treeSize + 16 + dataSize;
    st.buffer.assign(st.tree.fileSize, 0);
    st.tree.view = st.buffer.data();

    for (size_t node = 0; node < nodeCount; ++node) {
        unsigned char* nodeBase =
            st.buffer.data() + node * st.tree.nodeByteSize;
        // Left record.
        if (node + 1 < chainLen) {
            WriteRecord(nodeBase, st.tree.nodeByteSize, record28,
                          recordBytes, 0, node + 1);
        } else if (node + 1 == chainLen) {
            WriteRecord(nodeBase, st.tree.nodeByteSize, record28,
                          recordBytes, 0,
                          nodeCount + 16 + dataOffset);
        } else {
            WriteRecord(nodeBase, st.tree.nodeByteSize, record28,
                          recordBytes, 0, nodeCount);
        }
        // Right record: no data.
        WriteRecord(nodeBase, st.tree.nodeByteSize, record28,
                      recordBytes, 1, nodeCount);
    }
    return st;
}

static void BitsFromUint32(unsigned char bits[16], uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        bits[i] = static_cast<unsigned char>(
            (v >> (24 - 8 * i)) & 0xFF);
    }
    for (int i = 4; i < 16; ++i) bits[i] = 0;
}

// ---------------------------------------------------------------------
// Correctness suite
// ---------------------------------------------------------------------

struct TestResult {
    std::string name;
    std::string group;
    bool pass = false;
    std::string detail;
};

static void Record(std::vector<TestResult>& tests,
                     const char* group, const char* name,
                     bool pass, const std::string& detail) {
    tests.push_back(TestResult{name, group, pass, detail});
}

static void GeoIpTests(std::vector<TestResult>& t) {
    // --- chain walks (24-bit records) ---
    {
        SyntheticTree st = MakeChainTree(64, false, 3, 30, 1024, 0);
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found =
            GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip",
                  "chain30 walk (24-bit) finds data at depth 29",
                 found && off == 0,
                 "found=" + std::string(found ? "1" : "0") +
                     " offset=" + std::to_string(off));
    }
    {
        SyntheticTree st = MakeChainTree(64, false, 3, 40, 1024, 0);
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip",
                 "chain40 walk exhausts 32 bits (no data)",
                 !found,
                 "found=" + std::string(found ? "1" : "0"));
    }
    {
        // Data on the very first (left) bit.
        SyntheticTree st = MakeChainTree(64, false, 3, 1, 1024, 5);
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip", "data at depth 0 via left record",
                 found && off == 5,
                 "found=" + std::string(found ? "1" : "0") +
                     " offset=" + std::to_string(off));
    }
    {
        // Data on the right record: all-ones walk.
        SyntheticTree st =
            MakeTree(8, false, 3, 4242, 1024, 0.0, 0.0);
        WriteRecord(st.buffer.data(), 6, false, 3, 1, 8 + 16 + 7);
        unsigned char bits[16];
        std::memset(bits, 0xFF, 16);
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip", "data at depth 0 via right record",
                 found && off == 7,
                 "found=" + std::string(found ? "1" : "0") +
                     " offset=" + std::to_string(off));
    }
    {
        // record == nodeCount means "no data here".
        SyntheticTree st = MakeTree(8, false, 3, 7, 1024, 0.0, 0.0);        WriteRecord(st.buffer.data(), 6, false, 3, 0, 8);
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip", "record == nodeCount means no data",
                 !found,
                 "found=" + std::string(found ? "1" : "0"));
    }
    {
        // record == nodeCount + 16 is data offset 0.
        SyntheticTree st = MakeTree(8, false, 3, 7, 1024, 0.0, 0.0);
        WriteRecord(st.buffer.data(), 6, false, 3, 0, 8 + 16);
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip",
                 "record == nodeCount+16 is data offset 0",
                 found && off == 0,
                 "found=" + std::string(found ? "1" : "0") +
                     " offset=" + std::to_string(off));
    }
    {
        // record == nodeCount + 16 + dataSize - 1 is the last
        // data byte.
        SyntheticTree st = MakeTree(8, false, 3, 7, 1024, 0.0, 0.0);
        WriteRecord(st.buffer.data(), 6, false, 3, 0,
                      8 + 16 + 1023);
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip",
                 "record == nodeCount+16+dataSize-1 is last byte",
                 found && off == 1023,
                 "found=" + std::string(found ? "1" : "0") +
                     " offset=" + std::to_string(off));
    }
    {
        // record == nodeCount + 16 + dataSize is past the data
        // section.
        SyntheticTree st = MakeTree(8, false, 3, 7, 1024, 0.0, 0.0);
        WriteRecord(st.buffer.data(), 6, false, 3, 0,
                      8 + 16 + 1024);
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip",
                 "record == nodeCount+16+dataSize is out of range",
                 !found,
                 "found=" + std::string(found ? "1" : "0") +
                     " offset=" + std::to_string(off));
    }
    {
        // Same chain in 28-bit record encoding.
        SyntheticTree st = MakeChainTree(64, true, 0, 30, 1024, 0);
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip", "chain30 walk (28-bit) finds data",
                 found && off == 0,
                 "found=" + std::string(found ? "1" : "0") +
                     " offset=" + std::to_string(off));
    }
    {
        // Same chain in 32-bit record encoding.
        SyntheticTree st = MakeChainTree(64, false, 4, 30, 1024, 0);
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip", "chain30 walk (32-bit) finds data",
                 found && off == 0,
                 "found=" + std::string(found ? "1" : "0") +
                     " offset=" + std::to_string(off));
    }
    {
        // PROBE: 32-bit right-branch. GeoIp.cpp:977 reads recPos for
        // both halves, so the ORIGINAL answers with the LEFT record
        // here (no data); the optimized version reads the selected
        // half (data at offset 0). Divergence EXPECTED until the
        // main-tree bug is fixed; the optimized behavior matches the
        // MaxMind layout (8-byte nodes: left[0..3] right[4..7]).
        SyntheticTree st = MakeTree(8, false, 4, 7, 1024, 0.0, 0.0);
        WriteRecord(st.buffer.data(), 8, false, 4, 1, 8 + 16);
        unsigned char bits[16];
        std::memset(bits, 0xFF, 16);
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip",
                 "PROBE 32-bit right-branch (known orig half bug)",
                 true,
                 "found=" + std::string(found ? "1" : "0") +
                     " offset=" + std::to_string(off));
    }
    {
        // IPv6: a 128-bit walk down a 64-node chain.
        SyntheticTree st = MakeChainTree(128, false, 3, 64, 2048, 3);
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 128, 0, &st.tree, &off);
        Record(t, "geoip", "IPv6 128-bit walk down a 64-node chain",
                 found && off == 3,
                 "found=" + std::string(found ? "1" : "0") +
                     " offset=" + std::to_string(off));
    }
    {
        // Random 65k tree: 64 deterministic walks. Well-formed
        // trees must produce identical outcomes in both variants.
        SyntheticTree st = MakeTree(65536, false, 3, 20240, 8192,
                                      0.6, 0.3);
        std::string outcomes;
        uint64_t rng = 777;
        for (int i = 0; i < 64; ++i) {
            unsigned char bits[16];
            BitsFromUint32(bits,
                             static_cast<uint32_t>(NextRand(rng)));
            size_t off = 999;
            const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
            outcomes += found ? 'T' : 'F';
            if (found) outcomes += std::to_string(off % 10);
        }
        Record(t, "geoip",
                 "random 65k-tree walk consistency (64 addresses)",
                 true, outcomes);
    }
    {
        // PROBE: startNode past the node table. Both variants reject
        // it (the original via NodeRecord's node >= nodeCount_ check,
        // the optimized via its own node >= nodeCount guard).
        SyntheticTree st = MakeTree(100, false, 3, 31, 64, 0.5, 0.25);
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found =
            GeoIpWalk(bits, 32, 100, &st.tree, &off);
        Record(t, "geoip",
                 "PROBE startNode past the node table (divergence)",
                 true,
                 "found=" + std::string(found ? "1" : "0") +
                     " offset=" + std::to_string(off));
    }
    {
        // PROBE: a data pointer that points past the end of the
        // data section. Both variants reject it (offset >=
        // dataSectionSize in each implementation).
        SyntheticTree st = MakeTree(100, false, 3, 41, 64, 0.5, 0.25);
        WriteRecord(st.buffer.data(), 6, false, 3, 0,
                      100 + 16 + 64 + 4);
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip",
                 "PROBE data pointer past the data section "
                 "(divergence)",
                 true,
                 "found=" + std::string(found ? "1" : "0") +
                     " offset=" + std::to_string(off));
    }
    {
        // An "unloaded" database (dataSectionSize == 0) refuses
        // every walk in the original. The optimized version has
        // no Loaded() guard, but with a zero-size data section
        // every data record is out of range, so both refuse.
        SyntheticTree st = MakeTree(16, false, 3, 51, 1024, 0.5, 0.25);
        st.tree.dataSectionSize = 0;
        unsigned char bits[16] = {0};
        size_t off = 999;
        const bool found = GeoIpWalk(bits, 32, 0, &st.tree, &off);
        Record(t, "geoip", "unloaded tree (dataSectionSize=0) refuses",
                 !found,
                 "found=" + std::string(found ? "1" : "0"));
    }
}

static void ReasmTests(std::vector<TestResult>& t) {
    auto run = [](const std::vector<ReasmSeg>& segs,
                    uint64_t seq, size_t len, size_t* consumed,
                    size_t* skip) {
        ReasmWorkload w;
        w.aos = segs;
        for (const ReasmSeg& s : segs) {
            w.seqs.push_back(s.seq);
            w.sizes.push_back(s.len);
        }
        return ReasmOverlap(seq, len, &w, consumed, skip);
    };

    {
        size_t consumed = 0, skip = 0;
        const bool hasNew = run({}, 100, 50, &consumed, &skip);
        Record(t, "reasm", "empty store: full segment is new",
                 hasNew && consumed == 0 && skip == 50,
                 "consumed=" + std::to_string(consumed) +
                     " skip=" + std::to_string(skip) +
                     " hasNew=" + std::string(hasNew ? "1" : "0"));
    }
    {
        size_t consumed = 0, skip = 0;
        const bool hasNew =
            run({{100, 50}}, 150, 50, &consumed, &skip);
        Record(t, "reasm", "append after existing segment",
                 hasNew && consumed == 0 && skip == 50,
                 "consumed=" + std::to_string(consumed) +
                     " skip=" + std::to_string(skip) +
                     " hasNew=" + std::string(hasNew ? "1" : "0"));
    }
    {
        size_t consumed = 0, skip = 0;
        const bool hasNew =
            run({{100, 50}}, 100, 50, &consumed, &skip);
        Record(t, "reasm", "fully covered segment is a duplicate",
                 !hasNew && consumed == 50 && skip == 0,
                 "consumed=" + std::to_string(consumed) +
                     " skip=" + std::to_string(skip) +
                     " hasNew=" + std::string(hasNew ? "1" : "0"));
    }
    {
        // New [80,180) vs stored [100,150): the head 70 bytes
        // are covered, the tail 30 are new.
        size_t consumed = 0, skip = 0;
        const bool hasNew =
            run({{100, 50}}, 80, 100, &consumed, &skip);
        Record(t, "reasm", "head trim: 70 consumed, 30 new",
                 hasNew && consumed == 70 && skip == 30,
                 "consumed=" + std::to_string(consumed) +
                     " skip=" + std::to_string(skip) +
                     " hasNew=" + std::string(hasNew ? "1" : "0"));
    }
    {
        size_t consumed = 1, skip = 1;
        const bool hasNew = run({}, 80, 0, &consumed, &skip);
        Record(t, "reasm", "zero-length segment",
                 !hasNew && consumed == 0 && skip == 0,
                 "consumed=" + std::to_string(consumed) +
                     " skip=" + std::to_string(skip) +
                     " hasNew=" + std::string(hasNew ? "1" : "0"));
    }
    {
        // Stored [0,10) and [20,30); new [0,40): bytes 30..39
        // are the only new run.
        size_t consumed = 0, skip = 0;
        const bool hasNew =
            run({{0, 10}, {20, 10}}, 0, 40, &consumed, &skip);
        Record(t, "reasm", "gap between segments: tail is new",
                 hasNew && consumed == 30 && skip == 10,
                 "consumed=" + std::to_string(consumed) +
                     " skip=" + std::to_string(skip) +
                     " hasNew=" + std::string(hasNew ? "1" : "0"));
    }
    {
        // Out-of-order store: stored [100,150) and [200,250),
        // new [80,300). The algorithm assumes in-order arrival;
        // the covered prefix is 170 bytes and the tail is 50.
        size_t consumed = 0, skip = 0;
        const bool hasNew =
            run({{100, 50}, {200, 50}}, 80, 220, &consumed, &skip);
        Record(t, "reasm", "out-of-order store: consumed+skip <= len",
                 hasNew && consumed == 170 && skip == 50,
                 "consumed=" + std::to_string(consumed) +
                     " skip=" + std::to_string(skip) +
                     " hasNew=" + std::string(hasNew ? "1" : "0"));
    }
    {
        // Boundary case: stored segment ends exactly where the
        // new one starts (gEnd == s -> "entirely before us").
        size_t consumed = 0, skip = 0;
        const bool hasNew =
            run({{100, 50}}, 150, 10, &consumed, &skip);
        Record(t, "reasm", "boundary-adjacent segment (gEnd == s)",
                 hasNew && consumed == 0 && skip == 10,
                 "consumed=" + std::to_string(consumed) +
                     " skip=" + std::to_string(skip) +
                     " hasNew=" + std::string(hasNew ? "1" : "0"));
    }
    {
        // Single-byte full duplicate.
        size_t consumed = 0, skip = 0;
        const bool hasNew =
            run({{100, 1}}, 100, 1, &consumed, &skip);
        Record(t, "reasm", "single-byte full duplicate",
                 !hasNew && consumed == 1 && skip == 0,
                 "consumed=" + std::to_string(consumed) +
                     " skip=" + std::to_string(skip) +
                     " hasNew=" + std::string(hasNew ? "1" : "0"));
    }
    {
        // Randomized 8192-segment store: assert the algorithm's
        // invariants and record the exact outcome for the
        // cross-variant comparison.
        std::vector<ReasmSeg> segs;
        segs.reserve(8192);
        uint64_t rng = 909;
        uint64_t seq = 1000;
        for (int i = 0; i < 8192; ++i) {
            const size_t len = 400 + (NextRand(rng) % 500);
            segs.push_back({seq, len});
            seq += len + (NextRand(rng) % 50);
        }
        const uint64_t newSeq =
            1000 + (NextRand(rng) % (seq - 2000));
        const size_t newLen = 600 + (NextRand(rng) % 400);
        size_t consumed = 0, skip = 0;
        const bool hasNew =
            run(segs, newSeq, newLen, &consumed, &skip);
        const bool invariants =
            consumed <= newLen && skip <= newLen - consumed &&
            (skip == 0) == !hasNew && consumed + skip <= newLen;
        Record(t, "reasm",
                 "8192-segment randomized store (invariants)",
                 invariants,
                 "consumed=" + std::to_string(consumed) +
                     " skip=" + std::to_string(skip) +
                     " hasNew=" + std::string(hasNew ? "1" : "0"));
    }
}

static void SubstringTests(std::vector<TestResult>& t) {
    auto run = [](const std::wstring& h, const std::wstring& n) {
        return HasLowerSubstring(h, n);
    };
    Record(t, "substring", "empty needle matches",
             run(L"chrome.exe", L""),
             "");
    Record(t, "substring", "needle longer than haystack",
             !run(L"abc", L"abcd"),
             "");
    Record(t, "substring", "match at start",
             run(L"chrome.exe", L"chrom"),
             "");
    Record(t, "substring", "match in middle",
             run(L"chrome.exe", L"rome"),
             "");
    Record(t, "substring", "match at end",
             run(L"chrome.exe", L".exe"),
             "");
    Record(t, "substring", "no match",
             !run(L"chrome.exe", L"firefox"),
             "");
    Record(t, "substring", "identical strings",
             run(L"chrome.exe", L"chrome.exe"),
             "");
    Record(t, "substring", "single-char needle at end",
             run(L"chrome.exe", L"e"),
             "");
    {
        std::wstring h(4096, L'a');
        Record(t, "substring",
                 "repeated-char haystack, absent needle",
                 !run(h, L"aab"),
                 "haystack=" + std::to_string(h.size()));
    }
    {
        std::wstring h(4096, L'x');
        h += L"needle";
        Record(t, "substring", "4K haystack, needle at end",
                 run(h, L"needle"),
                 "haystack=" + std::to_string(h.size()));
    }
    {
        std::wstring h(65536, L'q');
        Record(t, "substring", "64K haystack, absent needle",
                 !run(h, L"zzz"),
                 "haystack=" + std::to_string(h.size()));
    }
}

static void ParseTests(std::vector<TestResult>& t) {
    // Ethernet-less: the parser gets the TCP header directly.
    unsigned char pkt[64] = {0};
    // srcPort 0x1F90 (8080), dstPort 0x0050 (80)
    pkt[0] = 0x1F; pkt[1] = 0x90;
    pkt[2] = 0x00; pkt[3] = 0x50;
    // seq 0x11223344, ack 0x55667788
    pkt[4] = 0x11; pkt[5] = 0x22; pkt[6] = 0x33; pkt[7] = 0x44;
    pkt[8] = 0x55; pkt[9] = 0x66; pkt[10] = 0x77; pkt[11] = 0x88;
    // data offset 5 (20 bytes), flags 0x18 (PSH|ACK), window 0x2000
    pkt[12] = 0x50;
    pkt[13] = 0x18;
    pkt[14] = 0x20; pkt[15] = 0x00;

    {
        TcpHeaderFields f;
        const bool ok = ParseTcpHeader(pkt, 0, 20, &f);
        const bool pass =
            ok && f.ok && f.srcPort == 8080 && f.dstPort == 80 &&
            f.seq == 0x11223344u && f.ack == 0x55667788u &&
            f.tcpFlags == 0x18 && f.window == 0x2000 &&
            f.payloadLen == 0 && f.payload == pkt + 20;
        Record(t, "parse", "20-byte header: all fields match", pass,
                 "ok=" + std::string(ok ? "1" : "0") +
                     " sport=" + std::to_string(f.srcPort) +
                     " dport=" + std::to_string(f.dstPort) +
                     " seq=0x" +
                     [&]{ char b[16]; std::snprintf(b, sizeof(b),
                             "%08X", f.seq); return std::string(b); }() +
                     " ack=0x" +
                     [&]{ char b[16]; std::snprintf(b, sizeof(b),
                             "%08X", f.ack); return std::string(b); }() +
                     " flags=0x" +
                     [&]{ char b[8]; std::snprintf(b, sizeof(b), "%02X",
                             f.tcpFlags); return std::string(b); }());
    }
    {
        // 32-byte header (data offset 8) with an 8-byte payload.
        pkt[12] = 0x80;
        for (int i = 20; i < 52; ++i) pkt[i] = static_cast<unsigned char>(i);
        TcpHeaderFields f;
        const bool ok = ParseTcpHeader(pkt, 0, 32 + 8, &f);
        const bool pass =
            ok && f.dataOffOk && f.srcPort == 8080 &&
            f.payloadLen == 8 && f.payload == pkt + 32;
        Record(t, "parse", "32-byte header with options and 8-byte payload",
                 pass,
                 "ok=" + std::string(ok ? "1" : "0") +
                     " payloadLen=" + std::to_string(f.payloadLen));
        pkt[12] = 0x50;
    }
    {
        TcpHeaderFields f;
        const bool ok = ParseTcpHeader(pkt, 0, 20, &f);
        Record(t, "parse", "zero-length payload (dataOff == avail)",
                 ok && f.ok && f.payloadLen == 0,
                 "ok=" + std::string(ok ? "1" : "0"));
    }
    {
        TcpHeaderFields f;
        const bool ok = ParseTcpHeader(pkt, 0, 12, &f);
        Record(t, "parse", "truncated header (< 20 bytes) rejected",
                 !ok && !f.ok,
                 "ok=" + std::string(ok ? "1" : "0"));
    }
    {
        // data offset 0 is invalid.
        pkt[12] = 0x00;
        TcpHeaderFields f;
        const bool ok = ParseTcpHeader(pkt, 0, 60, &f);
        Record(t, "parse", "data offset 0 rejected", !ok && !f.ok,
                 "ok=" + std::string(ok ? "1" : "0"));
        pkt[12] = 0x50;
    }
    {
        // data offset 60 beyond the available bytes.
        pkt[12] = 0xF0;
        TcpHeaderFields f;
        const bool ok = ParseTcpHeader(pkt, 0, 40, &f);
        Record(t, "parse", "data offset beyond avail rejected",
                 !ok && !f.ok,
                 "ok=" + std::string(ok ? "1" : "0"));
        pkt[12] = 0x50;
    }
}

static void FormatTests(std::vector<TestResult>& t) {
    auto fmt = [](uint64_t v) {
        wchar_t buf[32] = {0};
        const size_t n = FormatBytes(buf, 32, v);
        return std::make_pair(Narrow(std::wstring(buf, n)), n);
    };
    auto check = [&t, &fmt](const char* name, uint64_t v,
                        const char* expect) {
        const auto r = fmt(v);
        Record(t, "format", name,
                 r.first == expect,
                 "got=\"" + r.first + "\" want=\"" + expect + "\"");
    };
    check("0 bytes", 0, "0 B");
    check("512 bytes", 512, "512 B");
    check("1023 bytes", 1023, "1023 B");
    check("1024 bytes", 1024, "1.0 KB");
    check("1536 bytes (1.5 KB)", 1536, "1.5 KB");
    check("5 MiB", 5ull * 1024 * 1024, "5.0 MB");
    // GB/TB precision: both variants use "%.2f" (the optimized version
    // was fixed to match; it previously used "%.1f" for every unit).
    check("PROBE 1 GiB (%.2f GB)",
            1073741824ull, "1.00 GB");
    check("PROBE 1 TiB (%.2f TB)",
            1099511627776ull, "1.00 TB");
    check("PROBE 2 TiB (%.2f TB)",
            2199023255552ull, "2.00 TB");
    {
        // Truncation: both variants use swprintf_s with the same
        // buffer size, so the truncated output must match.
        wchar_t buf[4] = {0};
        const size_t n = FormatBytes(buf, 4, 1073741824ull);
        Record(t, "format", "buffer too small truncates identically",
               true,
               "n=" + std::to_string(n) +
                   " got=\"" + Narrow(std::wstring(buf, n)) + "\"");
    }
}

static void KeyTests(std::vector<TestResult>& t) {
    unsigned char local4[16] = {192, 168, 1, 10};
    unsigned char remote4[16] = {8, 8, 8, 8};
    unsigned char local6[16] = {0x20, 0x01, 0x0d, 0xb8, 1, 2, 3, 4,
                                  5, 6, 7, 8, 9, 10, 11, 12};
    unsigned char remote6[16] = {0xfe, 0x80, 1, 2, 3, 4, 5, 6, 7, 8,
                                   9, 10, 11, 12, 13, 14};
    unsigned char out[64] = {0};
    size_t outLen = 0;

    {
        ConnKeyInput in;
        in.ipv6 = false; in.udp = false;
        in.localAddr = local4; in.remoteAddr = remote4;
        in.localPort = 51514; in.remotePort = 443; in.pid = 4242;
        KeyOf(&in, out, &outLen);
        // Expected layout: '4' 'T' local4(4) localPort(4 LE)
        // remote4(4) remotePort(4 LE) pid(4 LE) = 22 bytes.
        std::string want;
        want += static_cast<char>(0x34);
        want += static_cast<char>(0x54);
        for (int i = 0; i < 4; ++i) want += static_cast<char>(local4[i]);
        for (int i = 0; i < 4; ++i)
            want += static_cast<char>((51514u >> (8 * i)) & 0xFF);
        for (int i = 0; i < 4; ++i) want += static_cast<char>(remote4[i]);
        for (int i = 0; i < 4; ++i)
            want += static_cast<char>((443u >> (8 * i)) & 0xFF);
        for (int i = 0; i < 4; ++i)
            want += static_cast<char>((4242u >> (8 * i)) & 0xFF);
        Record(t, "key", "IPv4 TCP key: exact 22 bytes",
                 outLen == 22 &&
                     std::string(reinterpret_cast<char*>(out),
                                   outLen) == want,
                 "len=" + std::to_string(outLen) +
                     " key=" + ToHex(out, outLen));
    }
    {
        ConnKeyInput in;
        in.ipv6 = false; in.udp = true;
        in.localAddr = local4; in.remoteAddr = remote4;
        in.localPort = 53; in.remotePort = 5353; in.pid = 1;
        KeyOf(&in, out, &outLen);
        const bool pass =
            outLen == 22 && out[0] == 0x34 && out[1] == 0x55;
        Record(t, "key", "IPv4 UDP key: protocol byte", pass,
                 "len=" + std::to_string(outLen) +
                     " key=" + ToHex(out, outLen));
    }
    {
        ConnKeyInput in;
        in.ipv6 = true; in.udp = false;
        in.localAddr = local6; in.remoteAddr = remote6;
        in.localPort = 80; in.remotePort = 443; in.pid = 7;
        KeyOf(&in, out, &outLen);
        // '6' 'T' local6(16) localPort(4 LE) remote6(16)
        // remotePort(4 LE) pid(4 LE) = 46 bytes. (The old
        // testAssemblies assertion of 38 was wrong; the
        // original's own static_assert is 2+16+4+16+4+4 = 46.)
        const bool pass = outLen == 46 && out[0] == 0x36 &&
                            out[1] == 0x54 &&
                            std::memcmp(out + 2, local6, 16) == 0 &&
                            std::memcmp(out + 22, remote6, 16) == 0;
        Record(t, "key", "IPv6 TCP key: exact 46 bytes", pass,
                 "len=" + std::to_string(outLen) +
                     " key=" + ToHex(out, outLen));
    }
    {
        ConnKeyInput in;
        in.ipv6 = false; in.udp = false;
        in.localAddr = local4; in.remoteAddr = remote4;
        in.localPort = 12345; in.remotePort = 54321; in.pid = 99;
        KeyOf(&in, out, &outLen);
        Record(t, "key", "IPv4 key length is 22",
                 outLen == 22,
                 "len=" + std::to_string(outLen));
    }
}

static void Utf8Tests(std::vector<TestResult>& t) {
    auto conv = [](const std::wstring& s, size_t dstSize = 4096) {
        std::string out(dstSize, '\0');
        const size_t n = WideToUtf8(s.c_str(), s.size(),
                                      &out[0], dstSize);
        out.resize(n);
        return out;
    };
    {
        const std::string r = conv(L"hello");
        Record(t, "utf8", "ASCII string",
                 r == "hello",
                 "n=" + std::to_string(r.size()));
    }
    {
        const std::string r = conv(L"");
        Record(t, "utf8", "empty string", r.empty(),
                 "n=" + std::to_string(r.size()));
    }
    {
        // U+00C0 -> C3 80
        const std::string r = conv(L"\x00C0");
        const bool pass = r.size() == 2 &&
                            static_cast<unsigned char>(r[0]) == 0xC3 &&
                            static_cast<unsigned char>(r[1]) == 0x80;
        Record(t, "utf8", "2-byte character (U+00C0)", pass,
                 "n=" + std::to_string(r.size()) +
                     " hex=" + ToHex(
                         reinterpret_cast<const unsigned char*>(
                             r.data()),
                         r.size()));
    }
    {
        // U+3042 -> E3 81 82
        const std::string r = conv(L"\x3042");
        const bool pass = r.size() == 3 &&
                            static_cast<unsigned char>(r[0]) == 0xE3 &&
                            static_cast<unsigned char>(r[1]) == 0x81 &&
                            static_cast<unsigned char>(r[2]) == 0x82;
        Record(t, "utf8", "3-byte character (U+3042)", pass,
                 "n=" + std::to_string(r.size()) +
                     " hex=" + ToHex(
                         reinterpret_cast<const unsigned char*>(
                             r.data()),
                         r.size()));
    }
    {
        // "a" + U+00C0 + U+3042 = 1 + 2 + 3 = 6 bytes.
        const std::string r = conv(L"a\x00C0\x3042");
        Record(t, "utf8", "mixed ASCII + 2-byte + 3-byte",
                 r.size() == 6,
                 "n=" + std::to_string(r.size()));
    }
    {
        // U+1F600 (surrogate pair D83D DE00) -> F0 9F 98 80
        const std::wstring s{L'\xD83D', L'\xDE00'};
        const std::string r = conv(s);
        const bool pass = r.size() == 4 &&
                            static_cast<unsigned char>(r[0]) == 0xF0 &&
                            static_cast<unsigned char>(r[1]) == 0x9F &&
                            static_cast<unsigned char>(r[2]) == 0x98 &&
                            static_cast<unsigned char>(r[3]) == 0x80;
        Record(t, "utf8",
                 "surrogate pair (U+1F600) encodes as 4 bytes", pass,
                 "n=" + std::to_string(r.size()) +
                     " hex=" + ToHex(
                         reinterpret_cast<const unsigned char*>(
                             r.data()),
                         r.size()));
    }
    {
        // PROBE: an unpaired surrogate. The original routes
        // through WideCharToMultiByte (default substitution);
        // the optimized version emits U+FFFD (EF BF BD).
        // Divergence is EXPECTED and is reported for review.
        const std::wstring s{L'\xD800'};
        const std::string r = conv(s);
        Record(t, "utf8",
                 "PROBE unpaired surrogate (divergence)",
                 true,
                 "n=" + std::to_string(r.size()) +
                     " hex=" + ToHex(
                         reinterpret_cast<const unsigned char*>(
                             r.data()),
                         r.size()));
    }
    {
        // PROBE: truncation of a multi-byte character. The
        // original converts fully and memcpy's the first
        // dstSize bytes (it can split a character); the
        // optimized version stops before the character that
        // does not fit. Divergence is EXPECTED for a buffer
        // that ends mid-character.
        const std::string r = conv(L"a\x00C0", 2);
        Record(t, "utf8",
                 "PROBE truncation mid-character (divergence)",
                 true,
                 "n=" + std::to_string(r.size()) +
                     " hex=" + ToHex(
                         reinterpret_cast<const unsigned char*>(
                             r.data()),
                         r.size()));
    }
    {
        std::wstring s(4096, L'a');
        const std::string r = conv(s);
        Record(t, "utf8", "4K ASCII string",
                 r.size() == 4096,
                 "n=" + std::to_string(r.size()));
    }
}

static void HexTests(std::vector<TestResult>& t) {
    auto fmt = [](const std::string& b, size_t bpl) {
        return FormatStreamHex(b, bpl);
    };
    Record(t, "hex", "empty input",
             fmt("", 16).empty(), "");
    {
        // Single byte: offset + "41 " + 15 pads, no halfway gap
        // (run 1 <= 8), gutter "A".
        std::string b(1, '\x41');
        const std::string want =
            std::string("00000000  41 ") + std::string(15 * 3, ' ') +
            " |A|\r\n";
        const std::string got = fmt(b, 16);
        Record(t, "hex", "single byte exact",
                 got == want,
                 "n=" + std::to_string(got.size()));
    }
    {
        // Full 16-byte line 0x00..0x0F: gap after byte 7, all dots.
        std::string b;
        for (int i = 0; i < 16; ++i) b += static_cast<char>(i);
        const std::string want =
            "00000000  "
            "00 01 02 03 04 05 06 07 "
            " "
            "08 09 0a 0b 0c 0d 0e 0f "
            " |................|\r\n";
        Record(t, "hex", "full 16-byte line exact",
                 fmt(b, 16) == want, "");
    }
    {
        // 20 bytes: second line at offset 0x10, run 4, no gap.
        std::string b;
        for (int i = 0; i < 20; ++i) b += static_cast<char>(i);
        const std::string got = fmt(b, 16);
        const std::string second =
            "00000010  "
            "10 11 12 13 "
            + std::string(12 * 3, ' ') + " |....|\r\n";
        Record(t, "hex", "short final line",
                 got.size() == 80 + 67 &&
                     got.substr(got.size() - second.size()) == second,
                 "n=" + std::to_string(got.size()));
    }
    {
        // Gutter mapping: 0x20->' ', 0x7E->'~', 0x7F/0x80/0xFF/NUL->'.'.
        const char raw[] = {'\x20', '\x7E', '\x7F', '\x80',
                            '\xFF', '\x00'};
        const std::string got = fmt(std::string(raw, 6), 16);
        const size_t bar = got.find(" |");
        Record(t, "hex", "gutter mapping",
                 bar != std::string::npos &&
                     got.substr(bar + 2, 6) == " ~....",
                 got.substr(bar != std::string::npos ? bar : 0, 10));
    }
    {
        // Odd width 7: halfway gap at i == 3.
        std::string b(7, '\x61');
        const std::string got = fmt(b, 7);
        Record(t, "hex", "odd width gap",
                 got ==
                     "00000000  61 61 61  61 61 61 61  |aaaaaaa|\r\n",
                 "");
    }
    {
        // Clamping: 0 and 99 behave as 16.
        const std::string b(20, '\x62');
        Record(t, "hex", "bytesPerLine clamps to 16",
                 fmt(b, 0) == fmt(b, 16) && fmt(b, 99) == fmt(b, 16),
                 "");
    }
    {
        // Max width 64 single line.
        std::string b(64, '\x63');
        const std::string got = fmt(b, 64);
        size_t lines = 0;
        for (size_t p = 0; (p = got.find("\r\n", p)) != std::string::npos;
             ++p, ++lines) {
        }
        Record(t, "hex", "64-wide single line",
                 lines == 1 && got.compare(0, 10, "00000000  ") == 0,
                 "n=" + std::to_string(got.size()));
    }
}

static void LowerTests(std::vector<TestResult>& t) {
    auto low = [](const std::wstring& s) { return ToLowerW(s); };
    Record(t, "lower", "empty string", low(L"").empty(), "");
    Record(t, "lower", "ASCII fold only A-Z",
             low(L"AbC XyZ 09 @[`{") == L"abc xyz 09 @[`{", "");
    Record(t, "lower", "already lower unchanged",
             low(L"chrome.exe") == L"chrome.exe", "");
    {
        // Non-ASCII byte in block forces scalar fallback; 0x80 never
        // folds in any locale, A/B fold universally.
        const std::wstring got = low(L"A\x0080" L"B");
        Record(t, "lower", "fallback around non-ASCII",
                 got == L"a\x0080" L"b",
                 "n=" + std::to_string(got.size()));
    }
    {
        // Surrogate halves are never folded.
        const std::wstring in{L'\xD800', L'\xDC00', L'X'};
        const std::wstring want{L'\xD800', L'\xDC00', L'x'};
        Record(t, "lower", "surrogate halves preserved",
                 low(in) == want, "");
    }
    {
        // Length-based: embedded NUL folds around it, keeps it.
        const std::wstring in(L"A\0B", 3);
        const std::wstring got = low(in);
        Record(t, "lower", "embedded NUL preserved",
                 got.size() == 3 && got[0] == L'a' && got[1] == L'\0' &&
                     got[2] == L'b',
                 "");
    }
    {
        // Long ASCII (multi-block SSE path) + tail.
        std::wstring in;
        for (int i = 0; i < 100; ++i) in += L"AbC123";
        const std::wstring got = low(in);
        bool ok = got.size() == in.size();
        for (size_t i = 0; ok && i < got.size(); ++i) {
            const wchar_t want =
                (in[i] >= L'A' && in[i] <= L'Z')
                    ? static_cast<wchar_t>(in[i] + 32)
                    : in[i];
            ok = got[i] == want;
        }
        Record(t, "lower", "600-char ASCII", ok, "");
    }
}

static void BeReadsTests(std::vector<TestResult>& t) {
    const unsigned char buf[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB,
                                   0xCD, 0xEF, 0x11, 0x22, 0x33, 0x44,
                                   0x55, 0x66, 0x77, 0x88};
    Record(t, "beread", "u32 aligned",
             GeoReadU32BE(buf) == 0x01234567u, "");
    Record(t, "beread", "u32 unaligned",
             GeoReadU32BE(buf + 1) == 0x23456789u &&
                 GeoReadU32BE(buf + 5) == 0xABCDEF11u,
             "");
    Record(t, "beread", "u64 aligned",
             GeoReadU64BE(buf) == 0x0123456789ABCDEFull, "");
    Record(t, "beread", "u64 unaligned",
             GeoReadU64BE(buf + 3) == 0x6789ABCDEF112233ull, "");
    {
        // n = 0..8 sweep, incl. n = 0 reads 0.
        bool ok = true;
        for (size_t n = 0; n <= 8; ++n) {
            uint64_t want = 0;
            for (size_t i = 0; i < n; ++i) {
                want = (want << 8) | buf[i];
            }
            uint64_t got = ~0ull;
            ok = ok && GeoReadBytes(buf, n, &got) && got == want;
        }
        Record(t, "beread", "bytes n=0..8", ok, "");
    }
    {
        // PROBE: n = 9. The original shifts mod 2^64 and returns true
        // (0x23456789ABCDEF11 here); the optimized version refuses.
        // No caller passes n > 8 (Decode bounds size by width first);
        // the refusal is a deliberate hardening, reported for review.
        uint64_t got = 0;
        const bool found = GeoReadBytes(buf, 9, &got);
        char detail[64] = {0};
        std::snprintf(detail, sizeof(detail), "found=%d v=0x%llX",
                      found ? 1 : 0,
                      static_cast<unsigned long long>(got));
        Record(t, "beread", "PROBE bytes n=9 (wrap vs refuse)", true,
                 detail);
    }
}

static void TlsBeTests(std::vector<TestResult>& t) {
    const unsigned char hdr[8] = {0x16, 0x03, 0x03, 0x01,
                                  0x00, 0xAA, 0xBB, 0xCC};
    Record(t, "tlsbe", "u16 version",
             TlsBe16(hdr + 1) == 0x0303u, "");
    Record(t, "tlsbe", "u16 length unaligned",
             TlsBe16(hdr + 3) == 0x0100u, "");
    Record(t, "tlsbe", "u24 handshake len",
             TlsBe24(hdr + 5) == 0xAABBCCu, "");
    Record(t, "tlsbe", "u24 0x010203",
             TlsBe24(hdr + 3) == 0x0100AAu, "");
    Record(t, "tlsbe", "u32",
             TlsBe32(hdr + 4) == 0x00AABBCCu, "");
    {
        // Full record-header walk: type + version + length.
        const bool ok = hdr[0] == 0x16 && TlsBe16(hdr + 1) == 0x0303 &&
                        TlsBe32(hdr) == 0x16030301u;
        Record(t, "tlsbe", "record header walk", ok, "");
    }
}

static void TlsExtTests(std::vector<TestResult>& t) {
    Record(t, "tlsext", "server_name -> SNI",
             ClassifyTlsExtension(0x0000) == TlsExtAction::kSni, "");
    Record(t, "tlsext", "ALPN -> ALPN",
             ClassifyTlsExtension(0x0010) == TlsExtAction::kAlpn, "");
    {
        // Everything else skips, incl. neighbors and supported_versions.
        const uint16_t others[] = {1, 15, 17, 0x002B, 0x00FF, 0xFFFF};
        bool ok = true;
        for (uint16_t v : others) {
            ok = ok && ClassifyTlsExtension(v) == TlsExtAction::kSkip;
        }
        Record(t, "tlsext", "others skip", ok, "");
    }
    {
        char host[] = "example.com...";
        Record(t, "tlsext", "strip dots",
                 StripSniTail(host, sizeof(host) - 1) == 11 &&
                     std::memcmp(host, "example.com", 11) == 0,
                 "");
    }
    {
        char host[] = "example.com";
        Record(t, "tlsext", "no strip",
                 StripSniTail(host, sizeof(host) - 1) == 11, "");
    }
    {
        // Embedded NULs inside the kept prefix are untouched; only the
        // tail strips.
        char buf[8] = {'a', '\0', 'b', '.', '\0', '.', '\0', '\0'};
        Record(t, "tlsext", "interior NUL kept",
                 StripSniTail(buf, 8) == 3 &&
                     std::memcmp(buf, "a\0b", 3) == 0,
                 "");
    }
    {
        char all[64];
        std::memset(all, '.', sizeof(all));
        Record(t, "tlsext", "all-strip to zero",
                 StripSniTail(all, sizeof(all)) == 0, "");
    }
    Record(t, "tlsext", "empty", StripSniTail(nullptr, 0) == 0, "");
}

static void RdTests(std::vector<TestResult>& t) {
    const unsigned char buf[8] = {0x12, 0x34, 0x56, 0x78,
                                  0x9A, 0xBC, 0xDE, 0xF0};
    Record(t, "rd", "u16 LE", Rd16(buf, false) == 0x3412u, "");
    Record(t, "rd", "u16 BE", Rd16(buf, true) == 0x1234u, "");
    Record(t, "rd", "u16 unaligned BE", Rd16(buf + 1, true) == 0x3456u, "");
    Record(t, "rd", "u32 LE", Rd32(buf, false) == 0x78563412u, "");
    Record(t, "rd", "u32 BE", Rd32(buf, true) == 0x12345678u, "");
    Record(t, "rd", "u32 unaligned BE",
             Rd32(buf + 3, true) == 0x789ABCDEu, "");
    Record(t, "rd", "ports walk",
             Rd16(buf, true) == 0x1234 && Rd16(buf + 2, true) == 0x5678 &&
                 Rd32(buf, true) == 0x12345678,
             "");
}

static JoinRow MakeJoinRow(uint32_t lp, uint32_t rp, const wchar_t* la,
                           const wchar_t* ra, bool tcp = true) {
    JoinRow r;
    r.localPort = lp;
    r.remotePort = rp;
    r.local = la;
    r.remote = ra;
    r.tcp = tcp;
    return r;
}

static JoinSample MakeJoinSample(uint32_t lp, uint32_t rp,
                                 const wchar_t* la, const wchar_t* ra,
                                 uint64_t rx, uint64_t tx) {
    JoinSample s;
    s.localPort = lp;
    s.remotePort = rp;
    s.local = la;
    s.remote = ra;
    s.rx = rx;
    s.tx = tx;
    return s;
}

static void JoinTests(std::vector<TestResult>& t) {
    {
        std::vector<JoinRow> rows;
        std::vector<JoinSample> ss;
        Record(t, "join", "empty", JoinSamples(rows, ss) == 0, "");
    }
    {
        // Single match writes rx/tx/perRow.
        std::vector<JoinRow> rows = {
            MakeJoinRow(5000, 443, L"10.0.0.5", L"93.184.216.34")};
        std::vector<JoinSample> ss = {
            MakeJoinSample(5000, 443, L"10.0.0.5", L"93.184.216.34",
                           100, 200)};
        const int n = JoinSamples(rows, ss);
        Record(t, "join", "single match",
                 n == 1 && rows[0].rx == 100 && rows[0].tx == 200 &&
                     rows[0].perRow,
                 "n=" + std::to_string(n));
    }
    {
        // Unknown sample skipped; non-TCP row never matches.
        std::vector<JoinRow> rows = {
            MakeJoinRow(5000, 443, L"10.0.0.5", L"93.184.216.34",
                        false)};
        std::vector<JoinSample> ss = {
            MakeJoinSample(5000, 443, L"10.0.0.5", L"93.184.216.34",
                           100, 200)};
        ss[0].known = false;
        Record(t, "join", "unknown skipped", JoinSamples(rows, ss) == 0,
                 "");
        ss[0].known = true;
        Record(t, "join", "non-TCP row skipped",
                 JoinSamples(rows, ss) == 0 && !rows[0].perRow, "");
    }
    {
        // mDNS duplicates: two rows share a key; samples assign in
        // order (first-match-wins, claim-once).
        std::vector<JoinRow> rows = {
            MakeJoinRow(5353, 5353, L"10.0.0.5", L"224.0.0.251"),
            MakeJoinRow(5353, 5353, L"10.0.0.5", L"224.0.0.251")};
        std::vector<JoinSample> ss = {
            MakeJoinSample(5353, 5353, L"10.0.0.5", L"224.0.0.251", 1,
                           10),
            MakeJoinSample(5353, 5353, L"10.0.0.5", L"224.0.0.251", 2,
                           20)};
        const int n = JoinSamples(rows, ss);
        Record(t, "join", "duplicate keys assign in order",
                 n == 2 && rows[0].rx == 1 && rows[1].rx == 2,
                 "n=" + std::to_string(n));
    }
    {
        // Sample matches only the second row; first row untouched.
        std::vector<JoinRow> rows = {
            MakeJoinRow(1111, 80, L"10.0.0.5", L"93.184.216.34"),
            MakeJoinRow(2222, 80, L"10.0.0.5", L"93.184.216.34")};
        std::vector<JoinSample> ss = {
            MakeJoinSample(2222, 80, L"10.0.0.5", L"93.184.216.34", 5,
                           6)};
        const int n = JoinSamples(rows, ss);
        Record(t, "join", "second-row match",
                 n == 1 && !rows[0].perRow && rows[1].rx == 5, "");
    }
    {
        // More samples than duplicate rows: the third sample has no
        // unclaimed row left.
        std::vector<JoinRow> rows = {
            MakeJoinRow(5353, 5353, L"10.0.0.5", L"224.0.0.251")};
        std::vector<JoinSample> ss = {
            MakeJoinSample(5353, 5353, L"10.0.0.5", L"224.0.0.251", 1,
                           10),
            MakeJoinSample(5353, 5353, L"10.0.0.5", L"224.0.0.251", 2,
                           20)};
        Record(t, "join", "excess sample dropped",
                 JoinSamples(rows, ss) == 1 && rows[0].rx == 1, "");
    }
}

static void FindLongTests(std::vector<TestResult>& t) {
    auto run = [](const std::wstring& h, const std::wstring& n) {
        return FindSubstringLong(h, n);
    };
    Record(t, "findlong", "empty needle", run(L"chrome.exe", L""), "");
    Record(t, "findlong", "needle longer", !run(L"abc", L"abcdef"), "");
    Record(t, "findlong", "at start", run(L"chrome.exe", L"chro"), "");
    Record(t, "findlong", "in middle", run(L"chrome.exe", L"ome.e"), "");
    Record(t, "findlong", "at end", run(L"chrome.exe", L".exe"), "");
    Record(t, "findlong", "absent", !run(L"chrome.exe", L"firefox"), "");
    Record(t, "findlong", "identical", run(L"chrome.exe", L"chrome.exe"),
             "");
    Record(t, "findlong", "single char hit", run(L"chrome.exe", L"m"),
             "");
    Record(t, "findlong", "single char miss",
             !run(L"chrome.exe", L"q"), "");
    {
        // 32-char needle: present once near the end of 4K.
        std::wstring h(4096, L'k');
        h.replace(4000, 32, std::wstring(32, L'm'));
        Record(t, "findlong", "32-char needle present",
                 run(h, std::wstring(32, L'm')), "");
        Record(t, "findlong", "32-char needle absent",
                 !run(h, std::wstring(32, L'z')), "");
    }
    {
        // Repeated-prefix needle (BMH worst-ish case for shifts).
        Record(t, "findlong", "repeated prefix",
                 run(L"aaaaaab", L"aaab") && !run(L"aaaaaac", L"aaab"),
                 "");
    }
}

static void LowerAllTests(std::vector<TestResult>& t) {
    const std::wstring fv[10] = {L"10.0.0.5",      L"93.184.216.34",
                                L"established",   L"4242",
                                L"chrome.exe",    L"c:\\a\\chrome.exe",
                                L"svc",           L"host",
                                L"tcp",           L"parent"};
    const std::wstring* const f[] = {&fv[0], &fv[1], &fv[2], &fv[3],
                                     &fv[4], &fv[5], &fv[6], &fv[7],
                                     &fv[8], &fv[9]};
    const std::wstring want =
        L"10.0.0.5 93.184.216.34 established 4242 chrome.exe "
        L"c:\\a\\chrome.exe svc host tcp parent";
    Record(t, "lowerall", "10 fields exact",
             BuildLowerAll(f, 10) == want,
             "n=" + std::to_string(BuildLowerAll(f, 10).size()));
    {
        const std::wstring ev[10] = {};
        const std::wstring* const e[] = {&ev[0], &ev[1], &ev[2], &ev[3],
                                         &ev[4], &ev[5], &ev[6], &ev[7],
                                         &ev[8], &ev[9]};
        Record(t, "lowerall", "all empty",
                 BuildLowerAll(e, 10) == std::wstring(9, L' '), "");
    }
    {
        // Fewer than 10 fields: both variants return empty.
        Record(t, "lowerall", "short count empty",
                 BuildLowerAll(f, 3).empty(), "");
    }
    {
        // Over count: both variants join the first 10 only.
        const std::wstring gv[12] = {
            L"10.0.0.5", L"93.184.216.34", L"established", L"4242",
            L"chrome.exe", L"c:\\a\\chrome.exe", L"svc", L"host",
            L"tcp", L"parent", L"EXTRA1", L"EXTRA2"};
        const std::wstring* const g[] = {&gv[0], &gv[1], &gv[2], &gv[3],
                                         &gv[4], &gv[5], &gv[6], &gv[7],
                                         &gv[8], &gv[9], &gv[10], &gv[11]};
        Record(t, "lowerall", "over count joins first 10",
                 BuildLowerAll(g, 12) == want, "");
    }
}

static void CmpWideTests(std::vector<TestResult>& t) {
    auto signOf = [](int v) { return (v > 0) - (v < 0); };
    auto check = [&t, &signOf](const char* name, const std::wstring& a,
                               const std::wstring& b) {
        const int want = signOf(std::wstring(a).compare(std::wstring(b)));
        const int got = signOf(CompareWide(a.c_str(), a.size(),
                                           b.c_str(), b.size()));
        Record(t, "cmpwide", name, got == want,
                 "want=" + std::to_string(want) +
                     " got=" + std::to_string(got));
    };
    check("equal", L"chrome.exe", L"chrome.exe");
    check("both empty", L"", L"");
    check("empty vs non-empty", L"", L"a");
    check("prefix shorter", L"chrom", L"chrome");
    check("prefix longer", L"chrome", L"chrom");
    check("diff at 0", L"achrome", L"bchrome");
    check("diff at 7 (in-lane)", L"0123456x", L"0123456y");
    check("diff at 8 (lane edge)", L"01234567x", L"01234567y");
    check("diff at 15/16", std::wstring(15, L'a') + L"x",
          std::wstring(15, L'a') + L"y");
    check("high chars", L"\x00C0\x3042", L"\x00C0\x3043");
    {
        std::wstring a(256, L'm');
        std::wstring b(256, L'm');
        b[250] = L'n';
        check("late diff 256", a, b);
        check("equal 256", a, std::wstring(256, L'm'));
    }
}

static void KeyHashTests(std::vector<TestResult>& t) {
    const unsigned char k4[22] = {'4', 'T', 192, 168, 1, 10,
                                  0x39, 0xC8, 0x00, 0x00, 8, 8,
                                  8, 8, 0xBB, 0x01, 0x00, 0x00,
                                  0x92, 0x10, 0x00, 0x00};
    const unsigned char k6[46] = {
        '6', 'T', 0x20, 0x01, 0x0D, 0xB8, 1, 2,  3,  4,  5,  6,
        7,   8,   9,    10,   11,   12,   80, 0,  0,  0,  0xFE,
        0x80, 1,  2,    3,    4,    5,    6,  7,  8,  9,  10,
        11,  12,  13,   14,   0xBB, 0x01, 0,  0,  7,  0,  0,  0};
    Record(t, "keyhash", "equal keys equal hash",
             HashConnKey(k4, 22) == HashConnKey(k4, 22) &&
                 HashConnKey(k6, 46) == HashConnKey(k6, 46),
             "");
    Record(t, "keyhash", "distinct keys distinct hash",
             HashConnKey(k4, 22) != HashConnKey(k6, 46), "");
    Record(t, "keyhash", "equal bytes",
             EqualConnKey(k4, 22, k4, 22) &&
                 EqualConnKey(k6, 46, k6, 46),
             "");
    Record(t, "keyhash", "length differs",
             !EqualConnKey(k4, 22, k4, 21) &&
                 !EqualConnKey(k4, 22, k6, 46),
             "");
    {
        unsigned char mut[22];
        std::memcpy(mut, k4, 22);
        mut[21] ^= 0xFF;  // last byte differs
        Record(t, "keyhash", "last byte differs",
                 !EqualConnKey(k4, 22, mut, 22), "");
        mut[21] ^= 0xFF;
        mut[0] ^= 0xFF;  // first byte differs
        Record(t, "keyhash", "first byte differs",
                 !EqualConnKey(k4, 22, mut, 22), "");
    }
}

static void BpsTests(std::vector<TestResult>& t) {
    {
        // 1500 bytes over 1000 ms = 1500 B/s, twice.
        const uint64_t prev[2] = {1000, 5000};
        const uint64_t now[2] = {2500, 8000};
        double out[2] = {0, 0};
        const size_t ok = ComputeBpsBatch(prev, now, 1000, out, 2);
        Record(t, "bps", "basic rate",
                 ok == 2 && out[0] == 1500.0 && out[1] == 3000.0,
                 "ok=" + std::to_string(ok));
    }
    {
        // Zero elapsed: no readings.
        const uint64_t prev[1] = {1000};
        const uint64_t now[1] = {2500};
        double out[1] = {0};
        Record(t, "bps", "zero elapsed",
                 ComputeBpsBatch(prev, now, 0, out, 1) == 0 &&
                     out[0] == 0.0,
                 "");
    }
    {
        // Backwards counter (recycled socket): rejected.
        const uint64_t prev[2] = {5000, 1000};
        const uint64_t now[2] = {1000, 2500};
        double out[2] = {0, 0};
        const size_t ok = ComputeBpsBatch(prev, now, 1000, out, 2);
        Record(t, "bps", "backwards rejected",
                 ok == 1 && out[0] == 0.0 && out[1] == 1500.0, "");
    }
    {
        // Wraparound-adjacent: now == prev is a zero rate, accepted.
        const uint64_t prev[1] = {7000};
        const uint64_t now[1] = {7000};
        double out[1] = {0};
        Record(t, "bps", "flat counter",
                 ComputeBpsBatch(prev, now, 500, out, 1) == 1 &&
                     out[0] == 0.0,
                 "");
    }
}

static void PidSumTests(std::vector<TestResult>& t) {
    {
        std::vector<PidTrafficRow> rows(4);
        rows[0].pid = 7;
        rows[0].counted = true;
        rows[0].rx = 100;
        rows[0].tx = 10;
        rows[1].pid = 7;
        rows[1].counted = true;
        rows[1].rx = 50;
        rows[1].tx = 5;
        rows[2].pid = 9;
        rows[2].counted = false;  // ETW total row: rides no sum
        rows[3].pid = 11;
        rows[3].counted = true;
        rows[3].rx = 0xFFFFFFFFFFFFFFFFull;  // saturates with next
        SumPidTraffic(rows.data(), rows.size());
        Record(t, "pidsum", "sums + uncounted pid",
                 rows[0].hasSum && rows[0].sumRx == 150 &&
                     rows[0].sumTx == 15 && rows[1].sumRx == 150 &&
                     !rows[2].hasSum && rows[2].sumRx == 0,
                 "");
    }
    {
        // Saturation: max + 1 clamps, never wraps.
        std::vector<PidTrafficRow> rows(2);
        rows[0].pid = 3;
        rows[0].counted = true;
        rows[0].rx = 0xFFFFFFFFFFFFFFFFull;
        rows[1].pid = 3;
        rows[1].counted = true;
        rows[1].rx = 1;
        SumPidTraffic(rows.data(), rows.size());
        Record(t, "pidsum", "saturates",
                 rows[0].sumRx == 0xFFFFFFFFFFFFFFFFull, "");
    }
    {
        // Uncounted row of a COUNTED pid rides the pid sums.
        std::vector<PidTrafficRow> rows(2);
        rows[0].pid = 5;
        rows[0].counted = true;
        rows[0].rx = 40;
        rows[1].pid = 5;
        rows[1].counted = false;
        SumPidTraffic(rows.data(), rows.size());
        Record(t, "pidsum", "uncounted row rides sums",
                 rows[1].hasSum && rows[1].sumRx == 40, "");
    }
}

static void BpsCellTests(std::vector<TestResult>& t) {
    auto fmt = [](double rx, double tx, bool known) {
        wchar_t buf[64] = {0};
        const size_t n = FormatBpsCell(buf, 64, rx, tx, known);
        return std::make_pair(std::wstring(buf, n), n);
    };
    {
        const auto r = fmt(0, 0, false);
        Record(t, "bpscell", "unknown",
                 r.first == L"—" && r.second == 1, "");
    }
    {
        const auto r = fmt(0.1, 0.2, true);
        Record(t, "bpscell", "idle",
                 r.first == L"idle" && r.second == 4, "");
    }
    {
        const auto r = fmt(1500.0, 3000000.0, true);
        Record(t, "bpscell", "normal",
                 r.first == L"↓ 1.5 KB/s  ↑ 2.9 MB/s",
                 Narrow(r.first));
    }
    {
        // Boundary: exactly 0.5 is NOT idle (strict <).
        const auto r = fmt(0.5, 0.0, true);
        Record(t, "bpscell", "half boundary",
                 r.first == L"↓ 1 B/s  ↑ 0 B/s", Narrow(r.first));
    }
    {
        // Truncation: tiny buffer zeroes like swprintf_s failure.
        wchar_t tiny[4] = {0};
        const size_t n = FormatBpsCell(tiny, 4, 1500.0, 3000000.0, true);
        Record(t, "bpscell", "truncates",
                 n == 0 && tiny[0] == L'\0', "");
    }
}

static void WidenTests(std::vector<TestResult>& t) {
    auto wide = [](const std::string& b) {
        return WidenUtf8(
            reinterpret_cast<const unsigned char*>(b.data()), b.size());
    };
    Record(t, "widen", "empty", wide("").empty(), "");
    Record(t, "widen", "ascii", wide("US") == L"US", "");
    {
        // e-acute (2-byte) + hiragana a (3-byte).
        const char b[] = {'\xC3', '\xA9', '\xE3', '\x81', '\x82'};
        const std::wstring want{L'\x00E9', L'\x3042'};
        Record(t, "widen", "2+3 byte", wide(std::string(b, 5)) == want,
                 "");
    }
    {
        // Overlong C0 AF: two U+FFFD (lead rejected, then continuation
        // rejected as a lead).
        const char b[] = {'\xC0', '\xAF'};
        const std::wstring want{L'\xFFFD', L'\xFFFD'};
        Record(t, "widen", "overlong", wide(std::string(b, 2)) == want,
                 "");
    }
    {
        // Stray continuation + 4-byte emoji (wchar_t is 16-bit).
        const char b[] = {'\x80', '\xF0', '\x9F', '\x98', '\x80'};
        const std::wstring got = wide(std::string(b, 5));
        Record(t, "widen", "stray + 4-byte",
                 got.size() == 5 &&
                     got == std::wstring(5, L'\xFFFD'),
                 "n=" + std::to_string(got.size()));
    }
    {
        // Long ASCII org name (SIMD bulk path, 40 chars).
        const std::string org = "Cloudflare, Inc. - network services";
        Record(t, "widen", "40-char ASCII",
                 wide(org) ==
                     L"Cloudflare, Inc. - network services",
                 "");
    }
    {
        // Mixed: ASCII run, multibyte, ASCII run (resync path).
        const char b[] = {'a', 'b', '\xC3', '\xA9', 'c', 'd'};
        const std::wstring want{L'a', L'b', L'\x00E9', L'c', L'd'};
        Record(t, "widen", "mixed", wide(std::string(b, 6)) == want,
                 "");
    }
}

static uint64_t HashV4Space() {
    // Exhaustive over the first three octets (the predicate never
    // inspects the fourth): any table typo diverges the hash.
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t f = 0; f < 256; ++f) {
        for (uint32_t s = 0; s < 256; ++s) {
            for (uint32_t th = 0; th < 256; ++th) {
                const uint32_t a =
                    (f << 24) | (s << 16) | (th << 8) | 1u;
                h ^= IsGlobalV4(a) ? 0x9E3779B9u : 0x85EBCA6Bu;
                h *= 1099511628211ULL;
            }
        }
    }
    return h;
}

static void UnicastTests(std::vector<TestResult>& t) {
    auto v4 = [](uint32_t a, bool want, const char* name) {
        const bool got = IsGlobalV4(a);
        return std::make_tuple(got == want, name,
                               std::string(got ? "allow" : "deny"));
    };
    const std::tuple<bool, const char*, std::string> cases[] = {
        v4(0x08080808u, true, "public 8.8.8.8"),
        v4(0x01010101u, true, "public 1.1.1.1"),
        v4(0x00000001u, false, "0/8"),
        v4(0x0A000001u, false, "10/8"),
        v4(0x7F000001u, false, "127/8"),
        v4(0x64400001u, false, "100.64/10 in"),
        v4(0x647F0001u, false, "100.127/10 edge in"),
        v4(0x64800001u, true, "100.128/10 out"),
        v4(0x643F0001u, true, "100.63/10 out"),
        v4(0xA9FE0101u, false, "169.254/16"),
        v4(0xA9FD0101u, true, "169.253 out"),
        v4(0xAC100001u, false, "172.16/12 low"),
        v4(0xAC1F0001u, false, "172.31/12 high"),
        v4(0xAC0F0001u, true, "172.15 out"),
        v4(0xAC200001u, true, "172.32 out"),
        v4(0xC0000001u, false, "192.0.0/24"),
        v4(0xC0000201u, true, "192.0.2 out (no carve-out)"),
        v4(0xC0586301u, false, "192.88.99/24"),
        v4(0xC0A80101u, false, "192.168/16"),
        v4(0xC6120001u, false, "198.18/15 low"),
        v4(0xC6130001u, false, "198.19/15 high"),
        v4(0xC6140001u, true, "198.20 out"),
        v4(0xC6336401u, false, "198.51.100/24"),
        v4(0xCB007101u, false, "203.0.113/24"),
        v4(0xE0000001u, false, "224/4"),
        v4(0xF0000001u, false, "240/4"),
        v4(0xFFFFFFFFu, false, "broadcast"),
        v4(0xBA000001u, true, "186 ordinary"),
        v4(0xCC000001u, true, "204 ordinary"),
        v4(0xB0000001u, true, "176 ordinary"),
    };
    for (const auto& c : cases) {
        Record(t, "unicast", std::get<1>(c), std::get<0>(c),
                 std::get<2>(c));
    }
    {
        // Exhaustive V4 hash: complete agreement proof.
        char detail[32] = {0};
        std::snprintf(detail, sizeof(detail), "h=0x%llX",
                      static_cast<unsigned long long>(HashV4Space()));
        Record(t, "unicast", "exhaustive V4 (16.7M addrs)", true,
                 detail);
    }
    {
        // V6 sweep: first byte x second byte x tail shape.
        const unsigned char firsts[] = {0x00, 0x01, 0x20, 0xFC,
                                        0xFD, 0xFE, 0xFF};
        const unsigned char seconds[] = {0x00, 0x01, 0x0D, 0x80,
                                         0xC0, 0xFF};
        std::string bits;
        bits.reserve(7 * 6 * 4);
        for (unsigned char f : firsts) {
            for (unsigned char s : seconds) {
                for (int tail = 0; tail < 4; ++tail) {
                    unsigned char a[16] = {0};
                    a[0] = f;
                    a[1] = s;
                    if (f == 0x20) {
                        a[2] = 0x0D;
                        a[3] = 0xB8;  // 2001:db8 shape
                    }
                    if (tail == 1) {
                        a[15] = 1;  // ::1 shape
                    } else if (tail == 2) {
                        a[8] = 0xFF;  // non-zero middle
                    } else if (tail == 3) {
                        a[2] = 0x01;  // non-zero early tail
                    }
                    bits += IsGlobalV6(a) ? '1' : '0';
                }
            }
        }
        Record(t, "unicast", "V6 sweep (168 addrs)", true, bits);
    }
    {
        // Named V6 cases with asserted outcomes.
        const unsigned char loop[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                        0, 0, 0, 0, 0, 0, 0, 1};
        const unsigned char unspec[16] = {0};
        const unsigned char doc[16] = {0x20, 0x01, 0x0D, 0xB8, 0, 0,
                                       0,    0,    0,    0,    0, 0,
                                       0,    0,    0,    1};
        const unsigned char pub[16] = {0x26, 0x06, 0x47, 0x00, 0x47,
                                       0x00, 0,    0,    0,    0, 0,
                                       0,    0,    0,    0x11, 0x11};
        const unsigned char v4map[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                         0, 0, 0xFF, 0xFF, 0xC0, 0,
                                         0x02, 0x01};
        // ::1 has a non-zero tail so the predicate allows it; only the
        // all-zero ::/64 tail is denied.
        Record(t, "unicast", "V6 ::1 allowed", IsGlobalV6(loop), "");
        Record(t, "unicast", "V6 :: denied", !IsGlobalV6(unspec), "");
        Record(t, "unicast", "V6 2001:db8 denied", !IsGlobalV6(doc),
                 "");
        Record(t, "unicast", "V6 public allowed", IsGlobalV6(pub), "");
        Record(t, "unicast", "V6 v4-mapped allowed",
                 IsGlobalV6(v4map), "");
    }
}

static SnapKey MakeSnapKey(const char* s) {
    SnapKey k;
    k.len = std::strlen(s);
    if (k.len > 46) k.len = 46;
    std::memcpy(k.bytes, s, k.len);
    return k;
}

static void PairTests(std::vector<TestResult>& t) {
    {
        // Empty both sides.
        Record(t, "pair", "empty", PairSnapshot(nullptr, 0, nullptr, 0,
                                                nullptr) == 0,
                 "");
    }
    {
        SnapKey prev[1] = {MakeSnapKey("4Tkey-one")};
        SnapKey fresh[1] = {MakeSnapKey("4Tkey-one")};
        int out[1] = {-7};
        Record(t, "pair", "single",
                 PairSnapshot(prev, 1, fresh, 1, out) == 1 &&
                     out[0] == 0,
                 "");
    }
    {
        // Newcomer: no previous row offers the key.
        SnapKey prev[1] = {MakeSnapKey("4Tkey-one")};
        SnapKey fresh[1] = {MakeSnapKey("4Tkey-two")};
        int out[1] = {-7};
        Record(t, "pair", "newcomer",
                 PairSnapshot(prev, 1, fresh, 1, out) == 0 &&
                     out[0] == -1,
                 "");
    }
    {
        // mDNS duplicates: 3 previous, 2 fresh -> first two pair.
        SnapKey prev[3] = {MakeSnapKey("K"), MakeSnapKey("K"),
                           MakeSnapKey("K")};
        SnapKey fresh[2] = {MakeSnapKey("K"), MakeSnapKey("K")};
        int out[2] = {-7, -7};
        Record(t, "pair", "duplicates FIFO",
                 PairSnapshot(prev, 3, fresh, 2, out) == 2 &&
                     out[0] == 0 && out[1] == 1,
                 "");
    }
    {
        // Interleaved keys keep per-key FIFO order.
        SnapKey prev[3] = {MakeSnapKey("A"), MakeSnapKey("B"),
                           MakeSnapKey("A")};
        SnapKey fresh[3] = {MakeSnapKey("A"), MakeSnapKey("A"),
                            MakeSnapKey("B")};
        int out[3] = {-7, -7, -7};
        const int n = PairSnapshot(prev, 3, fresh, 3, out);
        Record(t, "pair", "interleaved",
                 n == 3 && out[0] == 0 && out[1] == 2 && out[2] == 1,
                 "n=" + std::to_string(n));
    }
    {
        // Ghost: previous row nobody wants stays unmatched (caller
        // reports the ghost); return counts pairs only.
        SnapKey prev[2] = {MakeSnapKey("A"), MakeSnapKey("gone")};
        SnapKey fresh[1] = {MakeSnapKey("A")};
        int out[1] = {-7};
        Record(t, "pair", "ghost ignored",
                 PairSnapshot(prev, 2, fresh, 1, out) == 1 &&
                     out[0] == 0,
                 "");
    }
}

static void CountedKeyTests(std::vector<TestResult>& t) {
    const char k0[] = "iso_code";
    const char k1[] = "names";
    const char k2[] = "autonomous_system_organization";
    const CountedKey keys[3] = {
        {reinterpret_cast<const unsigned char*>(k0), 8},
        {reinterpret_cast<const unsigned char*>(k1), 5},
        {reinterpret_cast<const unsigned char*>(k2), 30}};
    auto want = [](const char* s) {
        return std::make_pair(reinterpret_cast<const unsigned char*>(s),
                              std::strlen(s));
    };
    {
        const auto w = want("names");
        Record(t, "countedkey", "hit middle",
                 FindCountedKey(keys, 3, w.first, w.second) == 1, "");
    }
    {
        const auto w = want("iso_code");
        Record(t, "countedkey", "hit first",
                 FindCountedKey(keys, 3, w.first, w.second) == 0, "");
    }
    {
        // Same length as names(5) but different bytes: the u64
        // prefilter rejects without a full memcmp.
        const auto w = want("zzzzz");
        Record(t, "countedkey", "same-len miss",
                 FindCountedKey(keys, 3, w.first, w.second) == -1,
                 "");
    }
    {
        const auto w = want("autonomous_system_number");
        Record(t, "countedkey", "long miss",
                 FindCountedKey(keys, 3, w.first, w.second) == -1,
                 "");
    }
    {
        // Short want (< 8 bytes): prefilter skipped, memcmp decides.
        const auto w = want("en");
        Record(t, "countedkey", "short miss",
                 FindCountedKey(keys, 3, w.first, w.second) == -1,
                 "");
    }
    Record(t, "countedkey", "null",
             FindCountedKey(nullptr, 3, keys[0].p, keys[0].n) == -1 &&
                 FindCountedKey(keys, 3, nullptr, 5) == -1,
             "");
}

static FlowKey MakeFlowKey(const unsigned char* a, uint16_t ap,
                           const unsigned char* b, uint16_t bp) {
    FlowKey k;
    std::memcpy(k.addrA, a, 16);
    std::memcpy(k.addrB, b, 16);
    k.portA = ap;
    k.portB = bp;
    return k;
}

static void FlowTests(std::vector<TestResult>& t) {
    const unsigned char a1[16] = {10, 0, 0, 5, 0, 0, 0, 0,
                                  0,  0, 0, 0, 0, 0, 0, 0};
    const unsigned char b1[16] = {93, 184, 216, 34, 0, 0, 0, 0,
                                  0,  0,   0,   0,  0, 0, 0, 0};
    const FlowKey k1 = MakeFlowKey(a1, 5000, b1, 443);
    const FlowKey k2 = MakeFlowKey(a1, 5000, b1, 443);
    Record(t, "flow", "equal", FlowKeyEqual(k1, k2), "");
    {
        FlowKey k3 = k2;
        k3.portB = 80;
        Record(t, "flow", "port differs", !FlowKeyEqual(k1, k3), "");
    }
    {
        FlowKey k3 = k2;
        k3.addrB[15] ^= 0xFF;  // last byte differs
        Record(t, "flow", "addr late differs", !FlowKeyEqual(k1, k3),
                 "");
    }
    {
        FlowKey k3 = k2;
        k3.addrA[0] ^= 0xFF;  // first byte differs
        Record(t, "flow", "addr early differs", !FlowKeyEqual(k1, k3),
                 "");
    }
    {
        // CmpFlowAddr sign contract vs memcmp, incl. high bytes
        // (unsigned order, not signed-char order).
        const unsigned char x[16] = {0};
        const unsigned char y[16] = {0};
        const unsigned char hi[16] = {0xFF, 0, 0, 0, 0, 0, 0, 0,
                                      0,    0, 0, 0, 0, 0, 0, 0};
        const unsigned char lo[16] = {0x01, 0, 0, 0, 0, 0, 0, 0,
                                      0,    0, 0, 0, 0, 0, 0, 0};
        auto signOf = [](int v) { return (v > 0) - (v < 0); };
        Record(t, "flow", "cmp equal", CmpFlowAddr(x, y) == 0, "");
        Record(t, "flow", "cmp high-vs-low",
                 signOf(CmpFlowAddr(hi, lo)) ==
                     signOf(std::memcmp(hi, lo, 16)),
                 "");
        Record(t, "flow", "cmp reverse",
                 signOf(CmpFlowAddr(lo, hi)) ==
                     signOf(std::memcmp(lo, hi, 16)),
                 "");
    }
}

static void HandleTests(std::vector<TestResult>& t) {
    const uint32_t pids[3] = {100, 200, 300};
    const uint32_t types[2] = {5, 9};
    const PidHandle skip[1] = {{200, 0x40}};
    auto run = [&](const HandleEntry* e, size_t n, bool tk,
                   size_t cap) {
        std::vector<size_t> out(cap, 9999);
        const size_t acc = FilterHandles(
            e, n, pids, 3, types, 2, tk, skip, 1,
            cap == 0 ? nullptr : out.data(), cap);
        return std::make_pair(acc, out);
    };
    {
        // Empty table.
        const auto r = run(nullptr, 0, true, 8);
        Record(t, "handles", "empty", r.first == 0, "");
    }
    {
        // One wanted socket handle, one foreign pid, one wrong type.
        const HandleEntry e[3] = {
            {100, 5, 0x10}, {999, 5, 0x10}, {100, 7, 0x10}};
        const auto r = run(e, 3, true, 8);
        Record(t, "handles", "pid+type filter",
                 r.first == 1 && r.second[0] == 0, "");
    }
    {
        // typesKnown == false: type check skipped.
        const HandleEntry e[1] = {{100, 7, 0x10}};
        const auto r = run(e, 1, false, 8);
        Record(t, "handles", "types unknown skips check",
                 r.first == 1 && r.second[0] == 0, "");
    }
    {
        // Stalled pair skipped.
        const HandleEntry e[2] = {{200, 5, 0x40}, {200, 5, 0x41}};
        const auto r = run(e, 2, true, 8);
        Record(t, "handles", "skip pair",
                 r.first == 1 && r.second[0] == 1, "");
    }
    {
        // Wide PID (> DWORD) skipped before any set probe.
        const HandleEntry e[1] = {{0x1FFFFFFFFull, 5, 0x10}};
        const auto r = run(e, 1, true, 8);
        Record(t, "handles", "wide pid skipped", r.first == 0, "");
    }
    {
        // Small out buffer: count is full, writes are capped.
        const HandleEntry e[2] = {{100, 5, 0x10}, {300, 9, 0x20}};
        const auto r = run(e, 2, true, 1);
        Record(t, "handles", "capped writes",
                 r.first == 2 && r.second[0] == 0, "");
    }
}

static void EtwTests(std::vector<TestResult>& t) {
    const unsigned char tcp[16] = {
        0xC0, 0x0A, 0x28, 0x9A, 0xE0, 0xC8, 0xD1, 0x11,
        0x84, 0xE2, 0x00, 0xC0, 0x4F, 0xB9, 0x98, 0xA2};
    const unsigned char udp[16] = {
        0xC5, 0x50, 0x3A, 0xBF, 0xC9, 0xA9, 0x88, 0x49,
        0xA0, 0x05, 0x2D, 0xF0, 0xB7, 0xC8, 0x0F, 0x80};
    const unsigned char other[16] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    Record(t, "etw", "tcp send",
             ClassifyEvent(tcp, 0, 10) == TrafficDir::kSent, "");
    Record(t, "etw", "tcp recv",
             ClassifyEvent(tcp, 0, 11) == TrafficDir::kReceived, "");
    Record(t, "etw", "tcp send26",
             ClassifyEvent(tcp, 0, 26) == TrafficDir::kSent, "");
    Record(t, "etw", "udp recv27",
             ClassifyEvent(udp, 0, 27) == TrafficDir::kReceived, "");
    Record(t, "etw", "foreign guid",
             ClassifyEvent(other, 0, 10) == TrafficDir::kNone, "");
    Record(t, "etw", "opcode 18 excluded",
             ClassifyEvent(tcp, 0, 18) == TrafficDir::kNone, "");
    Record(t, "etw", "zero opcode+id",
             ClassifyEvent(tcp, 0, 0) == TrafficDir::kNone, "");
    Record(t, "etw", "id fallback",
             ClassifyEvent(tcp, 10, 0) == TrafficDir::kSent, "");
    Record(t, "etw", "null guid",
             ClassifyEvent(nullptr, 0, 10) == TrafficDir::kNone, "");
    {
        const unsigned char payload[8] = {0x2A, 0x00, 0x00, 0x00,
                                          0x40, 0x06, 0x00, 0x00};
        uint32_t pid = 0;
        uint32_t size = 0;
        Record(t, "etw", "payload",
                 ParseEventPayload(payload, 8, &pid, &size) &&
                     pid == 42 && size == 1600,
                 "");
    }
    {
        const unsigned char payload[8] = {0, 0, 0, 0, 1, 0, 0, 0};
        Record(t, "etw", "idle pid rejected",
                 !ParseEventPayload(payload, 8, nullptr, nullptr),
                 "");
    }
    Record(t, "etw", "short payload",
             !ParseEventPayload("1234567", 7, nullptr, nullptr), "");
}

static void JsonTests(std::vector<TestResult>& t) {
    auto esc = [](const std::wstring& s) { return JsonEscape(s); };
    Record(t, "json", "empty", esc(L"").empty(), "");
    Record(t, "json", "plain", esc(L"chrome.exe") == "chrome.exe", "");
    Record(t, "json", "quotes", esc(L"a\"b") == "a\\\"b", "");
    Record(t, "json", "backslash", esc(L"a\\b") == "a\\\\b", "");
    Record(t, "json", "newline", esc(L"a\nb") == "a\\nb", "");
    Record(t, "json", "cr-tab", esc(L"a\rb\t") == "a\\rb\\t", "");
    Record(t, "json", "control 0x01", esc(L"a\x01" L"b") == "a\\u0001b",
             "");
    Record(t, "json", "control 0x1F", esc(L"\x1F") == "\\u001f", "");
    Record(t, "json", "DEL passes", esc(L"\x7F") == "\x7F", "");
    {
        // e-acute widens to C3 A9 and passes through untouched.
        const std::string got = esc(L"caf\xE9");
        Record(t, "json", "utf8 passthrough",
                 got == std::string("caf\xC3\xA9"), "");
    }
    {
        // Mixed specials + unicode in one export-like row.
        const std::wstring in =
            L"path: \"C:\\Temp\\caf\xE9.txt\"\nline\x01";
        const std::string want =
            "path: \\\"C:\\\\Temp\\\\caf\xC3\xA9.txt\\\"\\nline\\u0001";
        Record(t, "json", "mixed row", esc(in) == want, "");
    }
}

static void CsvTests(std::vector<TestResult>& t) {
    auto esc = [](const std::string& s) { return CsvEscape(s); };
    Record(t, "csv", "empty", esc("").empty(), "");
    Record(t, "csv", "plain", esc("chrome.exe") == "chrome.exe", "");
    Record(t, "csv", "comma", esc("a,b") == "\"a,b\"", "");
    Record(t, "csv", "quote", esc("a\"b") == "\"a\"\"b\"", "");
    Record(t, "csv", "crlf", esc("a\r\nb") == "\"a\r\nb\"", "");
    Record(t, "csv", "all specials",
             esc(",\"\r\n") == "\",\"\"\r\n\"", "");
    // ---- the SIMD block: fields longer than 16 bytes ----
    // Every input above is under 16 bytes, so none of them ever entered the
    // SSE2 loop and a bug there passed 420/420. `list --format csv` found
    // it: "RpcEptMapper, RpcSs" (21 bytes, needs quoting) came out as
    // "cSs" - the clean 16-byte block was consumed but never emitted.
    {
        Record(t, "csv", "comma past the first block",
                 esc("RpcEptMapper, RpcSs") == "\"RpcEptMapper, RpcSs\"",
                 esc("RpcEptMapper, RpcSs"));
        // Exactly 16, 17, 31, 32 and 33 bytes: the block boundaries.
        Record(t, "csv", "16-byte comma field",
                 esc("aaaaaaaaaaaaaaa,b") == "\"aaaaaaaaaaaaaaa,b\"",
                 "");
        Record(t, "csv", "17-byte comma field",
                 esc("aaaaaaaaaaaaaaaa,b") == "\"aaaaaaaaaaaaaaaa,b\"",
                 "");
        Record(t, "csv", "32-byte comma field",
                 esc("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,b") ==
                     "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,b\"",
                 "");
        Record(t, "csv", "33-byte comma field",
                 esc("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,b") ==
                     "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,b\"",
                 "");
        // No specials at all but longer than a block: the fast path.
        Record(t, "csv", "40-byte plain field",
                 esc(std::string(40, 'a')) == std::string(40, 'a'),
                 "");
        // A quote inside the second block, so the span crosses a boundary.
        Record(t, "csv", "quote in the second block",
                 esc("0123456789abcdef\"ghij") == "\"0123456789abcdef\"\"ghij\"",
                 esc("0123456789abcdef\"ghij"));
        // Multiple quotes across several blocks.
        Record(t, "csv", "quotes across blocks",
                 esc("0123456789abc\"efghij\"mnopqrs\"tuvwxyz") ==
                     "\"0123456789abc\"\"efghij\"\"mnopqrs\"\"tuvwxyz\"",
                 "");
        // A long field whose special is the LAST byte.
        Record(t, "csv", "trailing comma after blocks",
                 esc(std::string(40, 'a') + ",") ==
                     "\"" + std::string(40, 'a') + ",\"",
                 "");
        // Long CR and LF, which reach the SIMD loop too.
        Record(t, "csv", "crlf after the first block",
                 esc(std::string(40, 'a') + "\r\n") ==
                     "\"" + std::string(40, 'a') + "\r\n\"",
                 "");
    }
    {
        // Differential sweep over lengths and special placements, against a
        // transcription of the original's per-character loop.
        auto ref = [](const std::string& field) {
            if (field.find_first_of(",\"\r\n") == std::string::npos)
                return field;
            std::string o;
            o.push_back('"');
            for (char c : field) {
                if (c == '"') o += "\"\"";
                else o.push_back(c);
            }
            o.push_back('"');
            return o;
        };
        std::string bad;
        for (size_t len = 0; len <= 80; ++len) {
            for (size_t at = 0; at < len; ++at) {
                for (int which = 0; which < 4; ++which) {
                    const char c = which == 0   ? ','
                                   : which == 1 ? '"'
                                   : which == 2 ? '\r'
                                                : '\n';
                    std::string f(len, 'a');
                    f[at] = c;
                    if (CsvEscape(f) != ref(f)) {
                        bad += " len=" + std::to_string(len) +
                               " at=" + std::to_string(at) +
                               " which=" + std::to_string(which);
                    }
                }
            }
            if (bad.size() > 160) break;
        }
        Record(t, "csv", "differential sweep (len x pos x special)", bad.empty(),
               bad);
    }
}

static void IntFmtTests(std::vector<TestResult>& t) {
    auto port = [](unsigned p) {
        wchar_t buf[16] = {0};
        const size_t n = FormatPort(buf, 16, p);
        return std::wstring(buf, n);
    };
    Record(t, "intfmt", "port 0", port(0) == L"0", "");
    Record(t, "intfmt", "port 80", port(80) == L"80", "");
    Record(t, "intfmt", "port 443", port(443) == L"443", "");
    Record(t, "intfmt", "port 8080", port(8080) == L"8080", "");
    Record(t, "intfmt", "port max", port(65535) == L"65535", "");
    Record(t, "intfmt", "uint max",
             port(4294967295u) == L"4294967295", "");
    {
        wchar_t tiny[2] = {0};
        Record(t, "intfmt", "port truncates",
                 FormatPort(tiny, 2, 8080) == 0 && tiny[0] == L'\0',
                 "");
    }
    auto u64 = [](uint64_t v) {
        char buf[32] = {0};
        const size_t n = FormatU64Dec(buf, 32, v);
        return std::string(buf, n);
    };
    Record(t, "intfmt", "u64 0", u64(0) == "0", "");
    Record(t, "intfmt", "u64 7", u64(7) == "7", "");
    Record(t, "intfmt", "u64 9digits", u64(123456789) == "123456789",
             "");
    Record(t, "intfmt", "u64 1e9", u64(1000000000) == "1000000000",
             "");
    Record(t, "intfmt", "u64 max",
             u64(18446744073709551615ull) == "18446744073709551615",
             "");
    Record(t, "intfmt", "u64 20digits",
             u64(10000000000000000000ull) == "10000000000000000000",
             "");
    auto dur = [](uint64_t s) {
        wchar_t buf[32] = {0};
        const size_t n = FormatDuration(buf, 32, s);
        return std::wstring(buf, n);
    };
    Record(t, "intfmt", "dur 0s", dur(0) == L"0s", "");
    Record(t, "intfmt", "dur 59s", dur(59) == L"59s", "");
    Record(t, "intfmt", "dur 60s", dur(60) == L"1m 00s", "");
    Record(t, "intfmt", "dur 61s", dur(61) == L"1m 01s", "");
    Record(t, "intfmt", "dur 3599s", dur(3599) == L"59m 59s", "");
    Record(t, "intfmt", "dur 3600s", dur(3600) == L"1h 0m", "");
    Record(t, "intfmt", "dur 3661s", dur(3661) == L"1h 1m", "");
    Record(t, "intfmt", "dur 86399s", dur(86399) == L"23h 59m", "");
    Record(t, "intfmt", "dur 86400s", dur(86400) == L"1d 0h", "");
    Record(t, "intfmt", "dur 90061s", dur(90061) == L"1d 1h", "");
}

static void IpFmtTests(std::vector<TestResult>& t) {
    auto v4 = [](unsigned a, unsigned b, unsigned c, unsigned d) {
        const unsigned char addr[4] = {
            static_cast<unsigned char>(a), static_cast<unsigned char>(b),
            static_cast<unsigned char>(c), static_cast<unsigned char>(d)};
        return FormatIpv4(addr);
    };
    Record(t, "ipfmt", "v4 private", v4(192, 168, 1, 1) == L"192.168.1.1",
             "");
    Record(t, "ipfmt", "v4 zeros", v4(0, 0, 0, 0) == L"0.0.0.0", "");
    Record(t, "ipfmt", "v4 max",
             v4(255, 255, 255, 255) == L"255.255.255.255", "");
    Record(t, "ipfmt", "v4 dns", v4(8, 8, 8, 8) == L"8.8.8.8", "");
    auto v6 = [](std::initializer_list<unsigned> w) {
        unsigned char addr[16] = {0};
        int i = 0;
        for (unsigned x : w) {
            addr[i++] = static_cast<unsigned char>(x >> 8);
            addr[i++] = static_cast<unsigned char>(x & 0xFF);
        }
        return FormatIpv6(addr);
    };
    Record(t, "ipfmt", "v6 unspecified",
             v6({0, 0, 0, 0, 0, 0, 0, 0}) == L"::", "");
    Record(t, "ipfmt", "v6 loopback",
             v6({0, 0, 0, 0, 0, 0, 0, 1}) == L"::1", "");
    Record(t, "ipfmt", "v6 full",
             v6({0x2001, 0x0DB8, 0x1234, 0x5678, 0x9ABC, 0xDEF0,
                 0x1234, 0x5678}) == L"2001:db8:1234:5678:9abc:def0:"
                                      L"1234:5678",
             "");
    Record(t, "ipfmt", "v6 single zero kept",
             v6({0x2001, 0x0DB8, 0, 1, 1, 1, 1, 1}) ==
                 L"2001:db8:0:1:1:1:1:1",
             "");
    Record(t, "ipfmt", "v6 trailing run",
             v6({0xFE80, 0, 0, 0, 0, 0, 0, 1}) == L"fe80::1", "");
    Record(t, "ipfmt", "v6 tie picks first",
             v6({0x2001, 0, 0, 1, 0, 0, 2, 3}) ==
                 L"2001::1:0:0:2:3",
             "");
    Record(t, "ipfmt", "v6-mapped",
             v6({0, 0, 0, 0, 0, 0xFFFF, 0xC000, 0x0201}) ==
                 L"::ffff:192.0.2.1",
             "");
}

// ---------------------------------------------------------------------
// Self-contained references for the candidates that have no literal
// expectation: each is a faithful transcription of the original wintcp
// code, so BOTH variants must agree with it test-by-test. These live in
// the harness (not in either bench provider) for exactly that reason.
// ---------------------------------------------------------------------

// Commands.cpp:270-291 CpWidth.
static size_t CpWidthRef(uint32_t cp) {
    if (cp < 0x0300) return 1;
    if (cp <= 0x036F) return 0;
    if (cp < 0x1100) return 1;
    if (cp <= 0x115F) return 2;
    if ((cp >= 0x2E80 && cp <= 0x303E) ||
        (cp >= 0x3041 && cp <= 0x33FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xA960 && cp <= 0xA97F) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||
        (cp >= 0x1F300 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

// Commands.cpp:295-322 NextCp.
static size_t NextCpRef(const std::string& s, size_t i, uint32_t* cp) {
    const unsigned char u = static_cast<unsigned char>(s[i]);
    size_t n = 1;
    uint32_t v = u;
    if (u >= 0xF0 && i + 4 <= s.size()) {
        n = 4;
        v = u & 0x07u;
    } else if (u >= 0xE0 && i + 3 <= s.size()) {
        n = 3;
        v = u & 0x0Fu;
    } else if (u >= 0xC0 && i + 2 <= s.size()) {
        n = 2;
        v = u & 0x1Fu;
    } else {
        *cp = u;
        return 1;
    }
    for (size_t k = 1; k < n; ++k) {
        const unsigned char c = static_cast<unsigned char>(s[i + k]);
        if ((c & 0xC0u) != 0x80u) {
            *cp = u;
            return 1;
        }
        v = (v << 6) | (c & 0x3Fu);
    }
    *cp = v;
    return n;
}

// Commands.cpp:324-332 DisplayWidth.
static size_t DisplayWidthRef(const std::string& s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = 0;
        i += NextCpRef(s, i, &cp);
        w += CpWidthRef(cp);
    }
    return w;
}

// Commands.cpp:336-350 TruncateToWidth.
static std::string TruncateRef(const std::string& s, size_t width) {
    if (DisplayWidthRef(s) <= width) return s;
    if (width == 0) return std::string();
    const size_t budget = width - 1;
    size_t w = 0, i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        const size_t n = NextCpRef(s, i, &cp);
        const size_t cw = CpWidthRef(cp);
        if (w + cw > budget) break;
        w += cw;
        i += n;
    }
    return s.substr(0, i) + "\xE2\x80\xA6";
}

// GeoIp.cpp:73-86, 134-139, 422-442 PayloadSize.
static bool PayloadSizeRef(const unsigned char* data, size_t size,
                           unsigned char ctrl, size_t pos,
                           MmdbPayload* out) {
    const uint32_t s = ctrl & 0x1Fu;
    if (s < 29) {
        out->size = s;
        out->pos = pos;
        return true;
    }
    if (s == 29) {
        if (pos >= size) return false;
        out->size = 29 + static_cast<uint32_t>(data[pos]);
        out->pos = pos + 1;
        return true;
    }
    const size_t extra = (s == 30) ? 2 : 3;
    if (pos + extra > size) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < extra; ++i) v = (v << 8) | data[pos + i];
    out->size = static_cast<uint32_t>((s == 30 ? 285u : 65821u) + v);
    out->pos = pos + extra;
    return true;
}

// GeoIp.cpp:53-56, 134-139, 492-511 ReadPointer.
static bool ReadPointerRef(const unsigned char* data, size_t size,
                           unsigned char ctrl, size_t pos, size_t* out) {
    const uint8_t psz = static_cast<uint8_t>((ctrl >> 3) & 0x3u);
    const size_t payload = static_cast<size_t>(psz) + 1;
    if (pos + payload > size) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < payload; ++i) v = (v << 8) | data[pos + i];
    if (psz != 3) {
        v |= (static_cast<uint64_t>(ctrl) & 0x7u) << (8 * payload);
        if (psz == 1) v += 2048;
        else if (psz == 2) v += 526336;
    }
    *out = static_cast<size_t>(v);
    return true;
}

// Pcapng.cpp:354-361 flow-record scan.
static bool FlowProbeRef(const unsigned char* pkt, size_t capLen) {
    for (size_t k = 12; k + 5 <= capLen && k < 40; k += 2) {
        if (pkt[k] == 0x08 && pkt[k + 1] == 0x00 &&
            (pkt[k + 2] >> 4) == 4) {
            return true;
        }
    }
    return false;
}

static void WidthTests(std::vector<TestResult>& t) {
    // ---- CpWidth against the original's own range chain ----
    {
        // plus the gaps between them, must agree.
        static const uint32_t kProbe[] = {
            0x0000, 0x007F, 0x0080, 0x02FF, 0x0300, 0x0301, 0x036F,
            0x0370, 0x0371, 0x10FF, 0x1100, 0x1101, 0x115F, 0x1160,
            0x1161, 0x2E7F, 0x2E80, 0x2E81, 0x2FFF, 0x3000, 0x303E,
            0x303F, 0x3040, 0x3041, 0x3042, 0x33FF, 0x3400, 0x3401,
            0x4DBF, 0x4DC0, 0x4E00, 0x4E01, 0x9FFF, 0xA000, 0xA001,
            0xA4CF, 0xA4D0, 0xA95F, 0xA960, 0xA97F, 0xA980, 0xABFF,
            0xAC00, 0xAC01, 0xD7A3, 0xD7A4, 0xF8FF, 0xF900, 0xFAFF,
            0xFB00, 0xFE2F, 0xFE30, 0xFE6F, 0xFE70, 0xFEFF, 0xFF00,
            0xFF01, 0xFF60, 0xFF61, 0xFFDF, 0xFFE0, 0xFFE6, 0xFFE7,
            0xFFFF, 0x10000, 0x1F2FF, 0x1F300, 0x1F301, 0x1FAFF,
            0x1FB00, 0x1FFFF, 0x20000, 0x20001, 0x2FFFF, 0x30000,
            0x3FFFD, 0x3FFFE, 0x40000, 0x10FFFF,
        };
        std::string bad;
        for (uint32_t cp : kProbe) {
            const size_t got = CpWidth(cp);
            if (got != CpWidthRef(cp)) {
                bad += " cp=" + std::to_string(cp) +
                       " got=" + std::to_string(got);
            }
        }
        Record(t, "width", "CpWidth matches at every range boundary", bad.empty(),
               bad);
    }
    {
        // Exhaustive sweep over the whole tabled range plus a coarse
        // sweep of everything above it: the table is generated from the
        // original chain, so any disagreement is a real defect.
        std::string bad;
        size_t checked = 0;
        for (uint32_t cp = 0; cp < 0x3000; ++cp) {
            if (CpWidth(cp) != CpWidthRef(cp)) {
                if (bad.size() < 200) {
                    bad += " " + std::to_string(cp);
                }
            }
            ++checked;
        }
        for (uint32_t cp = 0x3000; cp < 0x110000; cp += 7) {
            if (CpWidth(cp) != CpWidthRef(cp)) {
                if (bad.size() < 200) {
                    bad += " " + std::to_string(cp);
                }
            }
            ++checked;
        }
        const std::string label =
            "CpWidth exhaustive sweep (" + std::to_string(checked) +
            " code points)";
        Record(t, "width", label.c_str(), bad.empty(), bad);
    }

    // ---- NextCp ----
    Record(t, "width", "NextCp ascii", [] {
        uint32_t cp = 0;
        return NextCp("A", 0, &cp) == 1 && cp == 'A';
    }(), "");
    Record(t, "width", "NextCp 2-byte", [] {
        uint32_t cp = 0;
        // U+00E9 = C3 A9
        return NextCp("\xC3\xA9", 0, &cp) == 2 && cp == 0xE9;
    }(), "");
    Record(t, "width", "NextCp 3-byte", [] {
        uint32_t cp = 0;
        // U+3042 = E3 81 82
        return NextCp("\xE3\x81\x82", 0, &cp) == 3 && cp == 0x3042;
    }(), "");
    Record(t, "width", "NextCp 4-byte", [] {
        uint32_t cp = 0;
        // U+1F600 = F0 9F 98 80
        return NextCp("\xF0\x9F\x98\x80", 0, &cp) == 4 && cp == 0x1F600;
    }(), "");
    Record(t, "width", "NextCp truncated lead is a 1-byte stray", [] {
        uint32_t cp = 0;
        // No continuation available: counts the lead byte as itself.
        return NextCp("\xC3", 0, &cp) == 1 && cp == 0xC3;
    }(), "");
    Record(t, "width", "NextCp malformed continuation resyncs", [] {
        uint32_t cp = 0;
        // E3 followed by 'x': the lead byte counts alone.
        return NextCp("\xE3x", 0, &cp) == 1 && cp == 0xE3;
    }(), "");
    {
        // Walk a mixed string the way DisplayWidth does, comparing the
        // byte spans and code points one by one.
        const std::string s =
            "a\xC3\xA9\xE3\x81\x82\xF0\x9F\x98\x80z";
        std::string bad;
        size_t i = 0;
        while (i < s.size()) {
            uint32_t got = 0, want = 0;
            const size_t ng = NextCp(s, i, &got);
            const size_t nw = NextCpRef(s, i, &want);
            if (ng != nw || got != want) {
                bad += " at=" + std::to_string(i);
                break;
            }
            i += ng;
        }
        Record(t, "width", "NextCp matches the reference at every byte",
               i == s.size() && bad.empty(), bad);
    }

    // ---- DisplayWidth / TruncateToWidth ----
    auto width = [](const std::string& s) { return DisplayWidth(s); };
    auto trunc = [](const std::string& s, size_t w) {
        return TruncateToWidth(s, w);
    };
    Record(t, "width", "empty is width 0", width("") == 0, "");
    Record(t, "width", "ascii width == length",
             width("chrome.exe") == 10, "");
    Record(t, "width", "long ascii (bulk path)", width(std::string(1000, 'a')) == 1000,
             "");
    Record(t, "width", "ascii across 16-byte blocks", width(std::string(64, 'x')) == 64,
             "");
    Record(t, "width", "2-byte chars", width("caf\xC3\xA9") == 4, "");
    Record(t, "width", "4-byte emoji is width 2",
             width("\xF0\x9F\x98\x80") == 2, "");
    Record(t, "width", "combining accent is width 0",
             width("\xCC\x81") == 0, "");
    Record(t, "width", "cjk is width 2", width("\xE4\xB8\xAD") == 2, "");
    {
        // Alternating so no 16-byte block is all ASCII.
        std::string s;
        for (int i = 0; i < 200; ++i) s += "a\xE4\xB8\xAD";
        Record(t, "width", "mixed ascii+cjk 600 chars", width(s) == 600,
                 "got=" + std::to_string(width(s)));
    }
    {
        // Boundary stress: multi-byte sequences straddling 16-byte edges.
        std::string s;
        for (int i = 0; i < 40; ++i) s += "abcd";   // 160 ASCII
        s += "\xE4\xB8\xAD";      // now sits exactly on the 160 boundary
        s += std::string(40, 'e');
        Record(t, "width", "cjk straddling a block boundary",
               width(s) == 160 + 2 + 40,
               "got=" + std::to_string(width(s)));
    }
    {
        // A multi-byte sequence SPLIT across the 16-byte boundary, so the
        // SSE2 block ends mid-codepoint and the decoder must continue past
        // the block end rather than treat the tail as stray bytes.
        std::string s;
        for (int i = 0; i < 4; ++i) s += "abcd";     // 16 ASCII
        s += "abc";                                  // ASCII run of 19
        s += "\xE4\xB8\xAD";                         // CJK split at 19..21
        s += "de";
        Record(t, "width", "codepoint split across a block boundary",
               width(s) == 19 + 2 + 2,
               "got=" + std::to_string(width(s)));
    }

    Record(t, "width", "truncate fits -> unchanged",
             trunc("chrome.exe", 32) == "chrome.exe", "");
    Record(t, "width", "truncate exact fit -> unchanged",
             trunc("chrome.exe", 10) == "chrome.exe", "");
    Record(t, "width", "truncate one short adds the marker",
             trunc("chrome.exe", 9) == "chrome.e\xE2\x80\xA6", "");
    Record(t, "width", "truncate width 6", trunc("chrome.exe", 6) == "chrom\xE2\x80\xA6",
             "");
    Record(t, "width", "truncate width 1",
             trunc("chrome.exe", 1) == "\xE2\x80\xA6", "");
    Record(t, "width", "truncate width 0 is empty",
             trunc("chrome.exe", 0).empty(), "");
    Record(t, "width", "truncate empty stays empty",
             trunc("", 0).empty() && trunc("", 8).empty(), "");
    Record(t, "width", "truncate multi-byte is not split", [] {
        // Two 3-byte CJK chars, width 2 each; width 3 budget 2 keeps one.
        const std::string s = "\xE4\xB8\xAD\xE6\x96\x87";
        const std::string got = TruncateToWidth(s, 3);
        return got == std::string("\xE4\xB8\xAD\xE2\x80\xA6");
    }(), "");
    {
        // Wide sweep: every width against a mixed string, comparing the
        // byte-for-byte result with the original's two-pass answer.
        const std::string s =
            "chrome.exe:1234 -> 203.0.113.9:443 \xE4\xB8\xAD\xE6\x96\x87"
            "\xF0\x9F\x98\x80 established \xCC\x81";
        std::string bad;
        const size_t total = DisplayWidth(s);
        for (size_t w = 0; w <= total + 2; ++w) {
            const std::string got = trunc(s, w);
            const std::string want = TruncateRef(s, w);
            if (got != want) {
                bad += " w=" + std::to_string(w) + " got=\"" +
                       ToHex((const unsigned char*)got.data(), got.size()) +
                       "\" want=\"" +
                       ToHex((const unsigned char*)want.data(), want.size()) +
                       "\"";
                break;
            }
        }
        Record(t, "width", "truncate sweep over every width", bad.empty(), bad);
    }
}

// ---- 27b. GeoIP shift-register walk: same trees, second wiring --------
// Every tree already built in GeoIpTests is re-walked through the
// shift-register entry, so the two wirings are pinned to identical answers
// over the same addresses (including the 32-bit right-half divergence the
// builder encodes).
static void ShiftWalkTests(std::vector<TestResult>& t) {
    struct Case {
        const char* name;
        SyntheticTree (*make)();
        unsigned bitCount;
    };
    // Built by lambdas so the trees are constructed inside the loop.
    auto cases = [] {
        std::vector<std::tuple<const char*, SyntheticTree, unsigned>> v;
        v.push_back(std::make_tuple("chain30 24-bit",
                                     MakeChainTree(64, false, 3, 30, 1024, 0),
                                     32u));
        v.push_back(std::make_tuple("chain40 24-bit",
                                     MakeChainTree(64, false, 3, 40, 1024, 0),
                                     32u));
        v.push_back(std::make_tuple(
            "dense 24-bit",
            MakeTree(256, false, 3, 101, 512, 0.35, 0.45), 32u));
        v.push_back(std::make_tuple(
            "dense 28-bit",
            MakeTree(256, true, 3, 20243, 2048, 0.4, 0.4), 32u));
        v.push_back(std::make_tuple(
            "dense 32-bit",
            MakeTree(256, false, 4, 30301, 8192, 0.35, 0.45), 32u));
        v.push_back(std::make_tuple(
            "ipv6 128-bit",
            MakeTree(1024, false, 3, 40409, 4096, 0.5, 0.3), 128u));
        return v;
    }();
    for (auto& c : cases) {
        const std::string name = std::get<0>(c);
        SyntheticTree& st = std::get<1>(c);
        const unsigned bits = std::get<2>(c);
        std::string bad;
        size_t mismatches = 0;
        uint64_t rng = 7000;
        for (int i = 0; i < 200; ++i) {
            unsigned char addr[16] = {0};
            if (bits > 32) {
                for (int b = 0; b < 16; ++b) {
                    addr[b] = static_cast<unsigned char>(NextRand(rng));
                }
            } else {
                BitsFromUint32(addr, static_cast<uint32_t>(NextRand(rng)));
            }
            size_t a = 12345, b = 54321;
            const bool fa = GeoIpWalk(addr, bits, 0, &st.tree, &a);
            const bool fb = GeoIpWalkShift(addr, bits, 0, &st.tree, &b);
            // A refusal must leave *out alone, so compare the flags first
            // and only the offsets when both succeeded.
            if (fa != fb || (fa && a != b)) {
                ++mismatches;
                if (bad.size() < 160) {
                    bad += " i=" + std::to_string(i) +
                           " fa=" + std::string(fa ? "1" : "0") +
                           " fb=" + std::string(fb ? "1" : "0") +
                           " a=" + std::to_string(a) +
                           " b=" + std::to_string(b);
                }
            }
        }
        Record(t, "shiftwalk",
               (name + ": 200 random walks agree with the per-bit walker").c_str(),
               mismatches == 0, bad);
    }
    // The bit-order contract: address byte 0 first, MSB first. A walk over
    // 0x80.. (top bit set) must take the LEFT branch on the first step, so
    // a tree whose root's left record is a data pointer answers with it.
    {
        SyntheticTree st = MakeTree(8, false, 3, 7, 1024, 0.0, 0.0);
        WriteRecord(st.buffer.data(), 6, false, 3, 0, 8 + 16 + 3);
        unsigned char bits[16] = {0};
        bits[0] = 0x80;          // depth 0 == 1 -> RIGHT branch
        size_t off = 999;
        const bool found = GeoIpWalkShift(bits, 32, 0, &st.tree, &off);
        Record(t, "shiftwalk", "MSB-first order matches (depth 0)",
               !found || off != 3, "found=" + std::string(found ? "1" : "0"));
        bits[0] = 0x00;          // depth 0 == 0 -> LEFT branch
        off = 999;
        const bool f2 = GeoIpWalkShift(bits, 32, 0, &st.tree, &off);
        Record(t, "shiftwalk", "MSB-first order matches (depth 0, left)",
               f2 && off == 3,
               "found=" + std::string(f2 ? "1" : "0") +
                   " offset=" + std::to_string(off));
    }
}

// ---- 35. VLAN skip + flow-record probe ---------------------------------
// ---- 36. TCP reassembly render + 37. PID dedup ---------------------------

// The original TcpReasm.cpp:162-194 Render, transcribed against the AoS
// view ({seq, data} pairs) so both variants are compared with a
// self-contained reference rather than with each other.
static void RenderSegmentsRef(
    const std::vector<std::pair<uint64_t, std::string>>& segs,
    size_t duplicates, bool truncated, ReasmRenderResult* out) {
    out->bytes.clear();
    out->bytesMissing = 0;
    out->hasGap = false;
    out->firstSeq = 0;
    out->segments = segs.size();
    out->duplicates = duplicates;
    out->truncated = truncated;
    if (segs.empty()) return;

    std::vector<uint32_t> order(segs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (uint32_t)i;
    std::sort(order.begin(), order.end(), [&segs](uint32_t a, uint32_t b) {
        return segs[a].first < segs[b].first;
    });

    out->firstSeq = segs[order[0]].first;
    size_t total = 0;
    for (size_t i = 0; i < order.size(); ++i) total += segs[order[i]].second.size();
    out->bytes.resize(total);

    size_t at = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        const uint32_t s = order[i];
        if (i > 0) {
            const uint32_t p = order[i - 1];
            const uint64_t prevEnd = segs[p].first + segs[p].second.size();
            const uint64_t curStart = segs[s].first;
            if (curStart > prevEnd) {
                out->bytesMissing += (size_t)(curStart - prevEnd);
                out->hasGap = true;
            }
        }
        const std::string& d = segs[s].second;
        if (!d.empty()) {
            std::memcpy(out->bytes.data() + at, d.data(), d.size());
            at += d.size();
        }
    }
}

// Snapshot.cpp:37-45 DistinctPids, transcribed.
static std::vector<uint32_t> DistinctPidsRef(const uint32_t* pids, size_t n) {
    std::vector<uint32_t> out;
    if (pids == nullptr || n == 0) return out;
    out.reserve(n);
    std::unordered_set<uint32_t> seen;
    seen.reserve(n);
    for (size_t i = 0; i < n; ++i)
        if (seen.insert(pids[i]).second) out.push_back(pids[i]);
    return out;
}

// ConnectionStore.cpp:1599-1617 pidRows_ fill, transcribed with the
// first-appearance order made explicit.
static PidGroups GroupByPidRef(const uint32_t* pids, size_t n) {
    PidGroups g;
    if (pids == nullptr || n == 0) return g;
    std::unordered_map<uint32_t, size_t> index;
    index.reserve(n / 2 + 1);
    for (size_t i = 0; i < n; ++i) {
        const auto it = index.find(pids[i]);
        if (it == index.end()) {
            g.pids.push_back(pids[i]);
            index.emplace(pids[i], g.pids.size() - 1);
            g.rows.emplace_back();
            g.rows.back().push_back(i);
        } else {
            g.rows[it->second].push_back(i);
        }
    }
    return g;
}

static void RenderPidTests(std::vector<TestResult>& t) {
    // ---- pieces of Render's contract, with literal expectations ----
    struct Built {
        ReasmRenderView soa;
        std::vector<std::pair<uint64_t, std::string>> aos;
    };
    auto build = [](const std::vector<std::pair<uint64_t, std::string>>& in) {
        Built b;
        size_t pool = 0;
        for (const auto& s : in) {
            b.soa.seqs.push_back(s.first);
            b.soa.starts.push_back(pool);
            b.soa.sizes.push_back(s.second.size());
            b.soa.pool.insert(b.soa.pool.end(), s.second.begin(),
                              s.second.end());
            pool += s.second.size();
            b.aos.push_back(s);
        }
        return b;
    };
    const std::vector<unsigned char> kHello =
        {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '!'};

    // ---- pieces of Render's contract, with literal expectations ----
    {
        const std::vector<std::pair<uint64_t, std::string>> segs = {
            {100, "hello"}, {105, " world"}, {111, "!"},
        };
        Built b = build(segs);
        ReasmRenderResult got;
        ReasmRenderResult want;
        RenderSegments(b.soa, 3, false, &got);
        RenderSegmentsRef(b.aos, 3, false, &want);
        Record(t, "render", "contiguous: matches the reference",
               got.bytes == want.bytes && got.hasGap == want.hasGap &&
                   got.bytesMissing == want.bytesMissing,
               "");
        Record(t, "render", "contiguous: no gap, no missing",
               !got.hasGap && got.bytesMissing == 0 && got.firstSeq == 100 &&
                   got.segments == 3,
               "gap=" + std::string(got.hasGap ? "1" : "0") +
                   " missing=" + std::to_string(got.bytesMissing));
        Record(t, "render", "contiguous: bytes concatenated in seq order",
               got.bytes == std::vector<unsigned char>(
                                 {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o',
                                  'r', 'l', 'd', '!'}),
               "");
        Record(t, "render", "duplicates + truncated are carried",
               got.duplicates == 3 && !got.truncated, "");
    }
    {
        // The input is in capture order; the output must be in seq order.
        const std::vector<std::pair<uint64_t, std::string>> segs = {
            {111, "!"}, {100, "hello"}, {105, " world"},
        };
        Built b = build(segs);
        ReasmRenderResult got;
        RenderSegments(b.soa, 0, true, &got);
        Record(t, "render", "sort puts the stream in seq order",
               got.firstSeq == 100 && got.truncated &&
                   got.bytes == std::vector<unsigned char>(
                                     {'h', 'e', 'l', 'l', 'o', ' ', 'w',
                                      'o', 'r', 'l', 'd', '!'}),
               "");
    }
    {
        // A hole is recorded, not spliced away.
        const std::vector<std::pair<uint64_t, std::string>> segs = {
            {100, "hello"}, {200, "XXXXX"},
        };
        Built b = build(segs);
        ReasmRenderResult got;
        RenderSegments(b.soa, 7, false, &got);
        Record(t, "render", "gap: hole recorded, hasGap set",
               got.hasGap && got.bytesMissing == 95,
               "missing=" + std::to_string(got.bytesMissing));
        Record(t, "render", "gap: segments still concatenated",
               got.bytes.size() == 10 &&
                   got.firstSeq == 100 && got.duplicates == 7,
               "");
    }
    {
        // curStart == prevEnd is contiguous (the test is strict >).
        Record(t, "render", "touching segments are contiguous", [] {
            ReasmRenderView v;
            v.seqs = {100, 105};
            v.starts = {0, 5};
            v.sizes = {5, 5};
            v.pool.assign(10, 'x');
            ReasmRenderResult got;
            RenderSegments(v, 0, false, &got);
            return !got.hasGap && got.bytesMissing == 0 &&
                   got.bytes.size() == 10;
        }(), "");
    }
    {
        // Empty: nothing written, only the flags are carried.
        ReasmRenderView v;
        ReasmRenderResult got;
        RenderSegments(v, 4, true, &got);
        Record(t, "render", "empty: no segments, flags carried",
               got.segments == 0 && got.bytes.empty() && got.duplicates == 4 &&
                   got.truncated,
               "");
    }
    {
        // Zero-length segments contribute nothing but still order the walk.
        ReasmRenderView v;
        v.seqs = {100, 100, 100};
        v.starts = {0, 0, 0};
        v.sizes = {5, 0, 3};
        v.pool.assign(8, 'y');
        ReasmRenderResult got;
        RenderSegments(v, 0, false, &got);
        Record(t, "render", "zero-length segments are skipped",
               got.bytes.size() == 8 && !got.hasGap,
               "bytes=" + std::to_string(got.bytes.size()));
    }
    {
        // Differential sweep over every radix threshold: below it the sort
        // is std::sort, above it the radix sort, and both must agree.
        std::string bad;
        for (size_t count : {1u, 2u, 8u, 64u, 95u, 96u, 97u, 128u, 300u,
                             1000u}) {
            std::vector<std::pair<uint64_t, std::string>> segs;
            uint64_t rng = 40000 + count;
            uint64_t seq = 1000;
            for (size_t i = 0; i < count; ++i) {
                std::string data;
                const size_t len = NextRand(rng) % 64;
                for (size_t k = 0; k < len; ++k) {
                    data += static_cast<char>('a' + (NextRand(rng) % 26));
                }
                // Occasionally leave a hole so the gap path runs too.
                if (NextRand(rng) % 5 == 0) seq += NextRand(rng) % 100;
                segs.emplace_back(seq, data);
                seq += len;
            }
            Built b = build(segs);
            ReasmRenderResult got;
            RenderSegments(b.soa, count / 3, false, &got);
            ReasmRenderResult want;
            RenderSegmentsRef(b.aos, count / 3, false, &want);
            if (got.bytes != want.bytes || got.hasGap != want.hasGap ||
                got.bytesMissing != want.bytesMissing ||
                got.firstSeq != want.firstSeq ||
                got.segments != want.segments) {
                bad += " n=" + std::to_string(count);
            }
        }
        Record(t, "render", "differential sweep across the sort threshold",
               bad.empty(), bad);
    }

    // ---- 37. PID dedup ----
    auto pids = [](std::initializer_list<uint32_t> v) {
        std::vector<uint32_t> out(v.begin(), v.end());
        return out;
    };
    Record(t, "pids", "empty", DistinctPids(nullptr, 0).empty() &&
                                 DistinctPids(pids({1}).data(), 0).empty(),
           "");
    Record(t, "pids", "single", [](const std::vector<uint32_t>& p) {
        const std::vector<uint32_t> r = DistinctPids(p.data(), p.size());
        return r.size() == 1 && r[0] == 42;
    }(pids({42})), "");
    Record(t, "pids", "all duplicates collapse",
           [](const std::vector<uint32_t>& p) {
               const std::vector<uint32_t> r =
                   DistinctPids(p.data(), p.size());
               return r.size() == 1 && r[0] == 7;
           }(pids({7, 7, 7, 7, 7})), "");
    {
        // First-appearance order, which is the contract every caller of
        // DistinctPids depends on.
        const std::vector<uint32_t> in = {9, 4, 9, 1, 4, 9, 1};
        const std::vector<uint32_t> r = DistinctPids(in.data(), in.size());
        Record(t, "pids", "first-appearance order preserved",
               r == std::vector<uint32_t>({9, 4, 1}),
               "got " + std::to_string(r.size()) + " entries");
    }
    Record(t, "pids", "pid 0 and 0xFFFFFFFF are real keys",
           [](const std::vector<uint32_t>& p) {
               const std::vector<uint32_t> r =
                   DistinctPids(p.data(), p.size());
               return r == std::vector<uint32_t>({0, 5, 4294967295u});
           }(pids({0, 5, 0, 4294967295u, 5})), "");
    {
        // A browser with 2000 sockets and 12 processes: the realistic shape.
        std::vector<uint32_t> in;
        uint64_t rng = 5150;
        for (int i = 0; i < 2000; ++i) {
            in.push_back(static_cast<uint32_t>(1000 + (NextRand(rng) % 12)));
        }
        const std::vector<uint32_t> r = DistinctPids(in.data(), in.size());
        bool ordered = true;
        for (size_t i = 1; i < r.size(); ++i) {
            if (r[i] == r[i - 1]) ordered = false;
        }
        Record(t, "pids", "2000 sockets, 12 processes -> 12 distinct",
               r.size() == 12 && ordered, "got " + std::to_string(r.size()));
    }
    {
        // Differential sweep: random length, random multiplicity, including
        // 0 and 0xFFFFFFFF.
        std::string bad;
        for (unsigned seed = 0; seed < 12; ++seed) {
            uint64_t rng = 60000 + seed;
            const size_t n = NextRand(rng) % 500;
            std::vector<uint32_t> in(n);
            for (size_t i = 0; i < n; ++i) {
                const uint32_t roll = static_cast<uint32_t>(NextRand(rng) % 8);
                in[i] = (roll == 0)    ? 0u
                        : (roll == 1)  ? 4294967295u
                                       : static_cast<uint32_t>(
                                             NextRand(rng) % 40u);
            }
            const std::vector<uint32_t> got =
                DistinctPids(in.data(), in.size());
            const std::vector<uint32_t> want =
                DistinctPidsRef(in.data(), in.size());
            if (got != want) {
                bad += " seed=" + std::to_string(seed) + " n=" +
                       std::to_string(n);
            }
        }
        Record(t, "pids", "differential sweep (length x multiplicity)",
               bad.empty(), bad);
    }
    {
        // GroupByPid: rows in row order, pids in first-appearance order.
        const std::vector<uint32_t> in = {9, 4, 9, 1, 4, 9, 1};
        const PidGroups g = GroupByPid(in.data(), in.size());
        const PidGroups want = GroupByPidRef(in.data(), in.size());
        Record(t, "pids", "group: pids in first-appearance order",
               g.pids == std::vector<uint32_t>({9, 4, 1}),
               std::to_string(g.pids.size()));
        Record(t, "pids", "group: row indices per pid",
               g.rows.size() == 3 && g.rows[0] == std::vector<size_t>({0, 2, 5}) &&
                   g.rows[1] == std::vector<size_t>({1, 4}) &&
                   g.rows[2] == std::vector<size_t>({3, 6}),
               "");
        Record(t, "pids", "group: matches the reference exactly",
               g.pids == want.pids && g.rows == want.rows, "");
    }
    {
        // Differential sweep over the grouping, same shapes.
        std::string bad;
        for (unsigned seed = 0; seed < 8; ++seed) {
            uint64_t rng = 70000 + seed;
            const size_t n = NextRand(rng) % 400;
            std::vector<uint32_t> in(n);
            for (size_t i = 0; i < n; ++i) {
                in[i] = static_cast<uint32_t>(NextRand(rng) % 17u);
            }
            const PidGroups got = GroupByPid(in.data(), in.size());
            const PidGroups want = GroupByPidRef(in.data(), in.size());
            if (got.pids != want.pids || got.rows != want.rows) {
                bad += " seed=" + std::to_string(seed);
            }
        }
        Record(t, "pids", "group: differential sweep", bad.empty(), bad);
    }
}

static void VlanFlowTests(std::vector<TestResult>& t) {
    auto vlan = [](const std::vector<unsigned char>& b, size_t off) {
        return SkipVlan(b.data(), b.size(), off);
    };
    Record(t, "vlan", "plain ethernet: off 12 -> 14",
           vlan({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                 0x08, 0x00, 0x45, 0, 0, 0, 0, 0, 0, 0, 0, 0},
                12) == 14,
           "");
    Record(t, "vlan", "single 802.1Q: off 12 -> 18",
           vlan({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                 0x81, 0x00, 0x00, 0x64, 0x08, 0x00, 0x45, 0},
                12) == 18,
           "");
    Record(t, "vlan", "single 802.1ad: off 12 -> 18",
           vlan({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                 0x88, 0xA8, 0x00, 0x64, 0x08, 0x00, 0x45, 0},
                12) == 18,
           "");
    Record(t, "vlan", "QinQ: off 12 -> 22",
           vlan({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                 0x91, 0x00, 0x00, 0x0C, 0x81, 0x00, 0x00, 0x64,
                 0x08, 0x00, 0x45, 0},
                12) == 22,
           "");
    Record(t, "vlan", "truncated tag chain stops at the bound",
           vlan({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x81, 0x00},
                12) == 14,
           "");
    Record(t, "vlan", "too short for a tag: off 12 -> 14",
           vlan({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x81, 0x00},
                12) == 14,
           "");
    // The ethertype must not be consumed as a tag.
    Record(t, "vlan", "0x0800 is not a tag",
           vlan(std::vector<unsigned char>(40, 0x08), 12) == 14,
           "");

    // ---- FlowProbe ----
    auto frame = [](size_t capLen, unsigned seed) {
        std::vector<unsigned char> b(capLen, 0);
        uint64_t rng = seed;
        for (size_t i = 0; i < capLen; ++i) {
            b[i] = static_cast<unsigned char>(NextRand(rng));
        }
        return b;
    };
    // A flow signature at candidate 12.
    {
        auto b = frame(64, 11);
        b[12] = 0x08;
        b[13] = 0x00;
        b[14] = 0x45;
        Record(t, "flowprobe", "signature at candidate 12",
               FlowProbe(b.data(), b.size()), "");
    }
    // A signature at the LAST admitted candidate (38) needs k+5 <= capLen.
    {
        auto b = frame(64, 12);
        b[38] = 0x08;
        b[39] = 0x00;
        b[40] = 0x45;
        Record(t, "flowprobe", "signature at candidate 38",
               FlowProbe(b.data(), b.size()), "");
    }
    // Byte past k+4 must not be read: capLen = 42 admits candidate 38.
    {
        auto b = frame(43, 13);
        b[38] = 0x08;
        b[39] = 0x00;
        b[40] = 0x45;
        Record(t, "flowprobe", "candidate 38 needs 5 bytes (43 is enough)",
               FlowProbe(b.data(), b.size()), "");
        b.resize(42);   // 38 + 5 = 43 > 42, so candidate 38 is refused
        Record(t, "flowprobe", "capLen 42 refuses candidate 38",
               !FlowProbe(b.data(), b.size()), "");
    }
    // Version nibble must be 4, not merely byte 2 nonzero.
    {
        auto b = frame(64, 14);
        b[12] = 0x08;
        b[13] = 0x00;
        b[14] = 0x65;   // version 6
        Record(t, "flowprobe", "version 6 is not a flow record",
               !FlowProbe(b.data(), b.size()), "");
    }
    // Byte 1 must be exactly 0x00.
    {
        auto b = frame(64, 15);
        b[12] = 0x08;
        b[13] = 0x01;
        b[14] = 0x45;
        Record(t, "flowprobe", "0x0801 is not a flow record",
               !FlowProbe(b.data(), b.size()), "");
    }
    // Odd offsets are not candidates.
    {
        auto b = frame(64, 16);
        b[13] = 0x08;
        b[14] = 0x00;
        b[15] = 0x45;
        Record(t, "flowprobe", "odd offset 13 is not a candidate",
               !FlowProbe(b.data(), b.size()), "");
    }
    // Differential sweep over every capLen and every candidate offset,
    // which is what pins the SIMD block's stride-2 mask and its bound.
    {
        std::string bad;
        for (size_t capLen = 0; capLen <= 80; ++capLen) {
            for (size_t at = 10; at < 48; ++at) {
                for (int ver = 4; ver <= 5; ++ver) {
                    auto b = frame(
                        capLen != 0 ? capLen : 1,
                        static_cast<unsigned>(100 + capLen * 7 + at));
                    if (at + 2 < b.size()) {
                        b[at] = 0x08;
                        b[at + 1] = 0x00;
                        b[at + 2] = static_cast<unsigned char>(ver << 4);
                    }
                    const bool got = FlowProbe(b.data(), b.size());
                    const bool want = FlowProbeRef(b.data(), b.size());
                    if (got != want) {
                        bad += " capLen=" + std::to_string(capLen) +
                               " at=" + std::to_string(at) +
                               " ver=" + std::to_string(ver) +
                               " got=" + std::string(got ? "1" : "0") +
                               " want=" + std::string(want ? "1" : "0");
                    }
                }
            }
            if (bad.size() > 200) break;
        }
        Record(t, "flowprobe", "differential sweep (capLen x offset x version)",
               bad.empty(), bad);
    }
    // All-clear frames, short frames and empty frames.
    {
        auto b = frame(64, 77);
        Record(t, "flowprobe", "no signature -> false",
               !FlowProbe(b.data(), b.size()), "");
        Record(t, "flowprobe", "empty frame", !FlowProbe(b.data(), 0), "");
        std::vector<unsigned char> tiny(3, 0x08);
        Record(t, "flowprobe", "3-byte frame",
               !FlowProbe(tiny.data(), tiny.size()), "");
    }
}

static void MmdbTests(std::vector<TestResult>& t) {
    // A fake section: 32 bytes of payload, so every bound has a place to
    // land. Control byte 0x05 -> size code 5 (payload value 5).
    unsigned char sec[64] = {0};
    for (int i = 0; i < 64; ++i) sec[i] = static_cast<unsigned char>(i);

    // ---- PayloadSize: every size code ----
    for (unsigned code = 0; code < 32; ++code) {
        const unsigned char ctrl = static_cast<unsigned char>(code & 0x1Fu);
        MmdbPayload got;
        MmdbPayload want;
        const bool okg = PayloadSize(sec, 64, ctrl, 16, &got);
        const bool okw = PayloadSizeRef(sec, 64, ctrl, 16, &want);
        const bool pass = okg == okw && got.size == want.size &&
                          got.pos == want.pos;
        const std::string label = "PayloadSize code " + std::to_string(code);
        Record(t, "mmdb", label.c_str(), pass,
               "ok=" + std::string(okg ? "1" : "0") +
                   " size=" + std::to_string(got.size) +
                   " pos=" + std::to_string(got.pos));
    }
    {
        // Code 29 at the very last byte: 1 extra byte is available.
        MmdbPayload got;
        const bool ok = PayloadSize(sec, 64, 29, 63, &got);
        Record(t, "mmdb", "PayloadSize code 29 last byte",
               ok && got.size == 29u + sec[63] && got.pos == 64,
               "ok=" + std::string(ok ? "1" : "0"));
    }
    {
        // Code 29 with no byte left: refused, nothing read.
        MmdbPayload got;
        got.pos = 999;
        const bool ok = PayloadSize(sec, 64, 29, 64, &got);
        Record(t, "mmdb", "PayloadSize code 29 past the end", !ok, "");
    }
    {
        // Codes 30/31 need 2/3 bytes; a 1-byte tail is refused.
        MmdbPayload got;
        Record(t, "mmdb", "PayloadSize code 30 needs 2 bytes",
               !PayloadSize(sec, 64, 30, 63, &got) &&
                   !PayloadSize(sec, 64, 30, 63 + 1, &got),
               "");
        Record(t, "mmdb", "PayloadSize code 31 needs 3 bytes",
               !PayloadSize(sec, 64, 31, 62, &got) &&
                   PayloadSize(sec, 64, 31, 61, &got),
               "");
    }
    {
        // The 29/30/31 bases are what a hand-rolled reader gets wrong.
        sec[16] = 0xFF;
        MmdbPayload got;
        PayloadSize(sec, 64, 29, 16, &got);
        Record(t, "mmdb", "PayloadSize code 29 adds its base",
               got.size == 29 + 255, "size=" + std::to_string(got.size));
        sec[16] = 0;
    }
    {
        // Max values: 285 + 65535 and 65821 + 16777215.
        unsigned char big[8] = {0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0};
        MmdbPayload got;
        PayloadSize(big, 8, 30, 0, &got);
        Record(t, "mmdb", "PayloadSize code 30 max",
               got.size == 285 + 65535, "size=" + std::to_string(got.size));
        PayloadSize(big, 8, 31, 0, &got);
        Record(t, "mmdb", "PayloadSize code 31 max",
               got.size == 65821 + 16777215,
               "size=" + std::to_string(got.size));
    }

    // ---- ReadPointer: all four pointer sizes ----
    for (unsigned psz = 0; psz < 4; ++psz) {
        // ctrl low 3 bits = 7 so the MSB packing is exercised; bits 3-4
        // carry the pointer size, bits 5-7 must not be pointer (0 here).
        const unsigned char ctrl =
            static_cast<unsigned char>((psz << 3) | 0x07u);
        size_t got = 0, want = 0;
        const bool okg = ReadPointer(sec, 64, ctrl, 32, &got);
        const bool okw = ReadPointerRef(sec, 64, ctrl, 32, &want);
        const bool pass = okg == okw && got == want;
        const std::string label = "ReadPointer size " + std::to_string(psz);
        Record(t, "mmdb", label.c_str(), pass,
               "ok=" + std::string(okg ? "1" : "0") +
                   " out=" + std::to_string(got));
    }
    {
        // Size 3 IGNORES the control byte's low three bits.
        const unsigned char ctrl = static_cast<unsigned char>((3u << 3) | 0x07u);
        size_t got = 0;
        ReadPointer(sec, 64, ctrl, 32, &got);
        // 4 payload bytes 32,33,34,35 big-endian, no shift, no base.
        const size_t want =
            (32u << 24) | (33u << 16) | (34u << 8) | 35u;
        Record(t, "mmdb", "ReadPointer size 3 ignores the low bits",
               got == want, "got=" + std::to_string(got));
    }
    {
        // Size 0: payload is 1 byte and the 3 control bits are its top.
        const unsigned char ctrl = static_cast<unsigned char>((0u << 3) | 0x07u);
        size_t got = 0;
        ReadPointer(sec, 64, ctrl, 40, &got);
        const size_t want = (7u << 8) | 40u;
        Record(t, "mmdb", "ReadPointer size 0 packs the control bits",
               got == want, "got=" + std::to_string(got));
    }
    {
        // Size 1 adds 2048; size 2 adds 526336.
        size_t got = 0;
        ReadPointer(sec, 64, static_cast<unsigned char>((1u << 3) | 0x01u),
                    48, &got);
        Record(t, "mmdb", "ReadPointer size 1 adds 2048",
               got == 2048 + ((1u << 16) | (48u << 8) | 49u),
               "got=" + std::to_string(got));
        ReadPointer(sec, 64, static_cast<unsigned char>((2u << 3) | 0x01u),
                    48, &got);
        Record(t, "mmdb", "ReadPointer size 2 adds 526336",
               got == 526336 + ((1u << 24) | (48u << 16) | (49u << 8) | 50u),
               "got=" + std::to_string(got));
    }
    {
        // The bound is checked before the wide load: a payload that would
        // read past 'size' is refused, not clamped.
        size_t got = 1;
        Record(t, "mmdb", "ReadPointer refuses a partial payload",
               !ReadPointer(sec, 63, static_cast<unsigned char>((2u << 3) | 0x01u),
                            61, &got) &&
                   ReadPointer(sec, 64, static_cast<unsigned char>((2u << 3) | 0x01u),
                               61, &got),
               "got=" + std::to_string(got));
    }
    {
        // Boundary sweep: every pointer size at every position in the last
        // 8 bytes of the section. This is where the 4-byte fast path and
        // the byte-loop fallback hand over, so a wrong shift would show.
        std::string bad;
        for (size_t sz = 56; sz <= 64; ++sz) {
            for (size_t pos = 52; pos < 64; ++pos) {
                for (unsigned psz = 0; psz < 4; ++psz) {
                    const unsigned char ctrl =
                        static_cast<unsigned char>((psz << 3) | 0x03u);
                    size_t got = 1, want = 2;
                    const bool okg = ReadPointer(sec, sz, ctrl, pos, &got);
                    const bool okw = ReadPointerRef(sec, sz, ctrl, pos, &want);
                    // On refusal both leave their out-param untouched, so
                    // only the success case can be compared byte-for-byte.
                    if (okg != okw || (okg && got != want)) {
                        bad += " sz=" + std::to_string(sz) +
                               " pos=" + std::to_string(pos) +
                               " psz=" + std::to_string(psz) +
                               " got=" + std::to_string(got) +
                               " want=" + std::to_string(want);
                        pos = 64;
                        sz = 64;
                        break;
                    }
                }
            }
        }
        Record(t, "mmdb", "ReadPointer boundary sweep (size x pos x psz)",
               bad.empty(), bad);
    }
    {
        // Same for PayloadSize: codes 29/30/31 at every position in the
        // last 8 bytes, where the extra-byte bound bites.
        std::string bad;
        for (size_t sz = 56; sz <= 64; ++sz) {
            for (size_t pos = 56; pos < 64; ++pos) {
                for (unsigned code = 29; code < 32; ++code) {
                    MmdbPayload got;
                    MmdbPayload want;
                    const bool okg =
                        PayloadSize(sec, sz, static_cast<unsigned char>(code),
                                    pos, &got);
                    const bool okw =
                        PayloadSizeRef(sec, sz, static_cast<unsigned char>(code),
                                       pos, &want);
                    if (okg != okw ||
                        (okg && (got.size != want.size || got.pos != want.pos))) {
                        bad += " sz=" + std::to_string(sz) +
                               " pos=" + std::to_string(pos) +
                               " code=" + std::to_string(code) +
                               " got=" + std::to_string(got.size) + "/" +
                               std::to_string(got.pos) + " want=" +
                               std::to_string(want.size) + "/" +
                               std::to_string(want.pos);
                        pos = 64;
                        sz = 64;
                        break;
                    }
                }
            }
        }
        Record(t, "mmdb", "PayloadSize boundary sweep (size x pos x code)",
               bad.empty(), bad);
    }
}

// ---------------------------------------------------------------------
// Benchmark workloads
// ---------------------------------------------------------------------

struct Timing {
    double nsMedian = 0;
    double nsMean = 0;
    double nsStdev = 0;
    double nsMin = 0;
    double nsP90 = 0;
    size_t iterations = 0;
    std::vector<double> rounds;
};

using Clock = std::chrono::steady_clock;

template <class F>
static Timing TimeWorkload(F&& fn, int rounds, double timeMs,
                             double warmupMs) {
    // Warmup (not measured).
    const auto warmEnd =
        Clock::now() +
        std::chrono::duration<double, std::milli>(warmupMs);
    size_t warm = 0;
    while (Clock::now() < warmEnd) {
        fn();
        ++warm;
    }

    Timing t;
    for (int r = 0; r < rounds; ++r) {
        const auto t0 = Clock::now();
        const auto deadline =
            t0 + std::chrono::duration<double, std::milli>(timeMs);
        size_t iters = 0;
        for (;;) {
            fn();
            ++iters;
            if ((iters & 0x3FF) == 0 && Clock::now() >= deadline) {
                break;
            }
        }
        const double elapsedNs =
            std::chrono::duration<double, std::nano>(
                Clock::now() - t0)
                .count();
        t.rounds.push_back(elapsedNs / static_cast<double>(iters));
        t.iterations += iters;
    }

    std::vector<double> sorted = t.rounds;
    std::sort(sorted.begin(), sorted.end());
    const size_t n = sorted.size();
    t.nsMedian = sorted[n / 2];
    t.nsMin = sorted.front();
    t.nsP90 = sorted[static_cast<size_t>(
        std::min<size_t>(n - 1, (90 * n) / 100))];
    double sum = 0;
    for (double v : sorted) sum += v;
    t.nsMean = sum / static_cast<double>(n);
    double var = 0;
    for (double v : sorted) {
        const double d = v - t.nsMean;
        var += d * d;
    }
    t.nsStdev = std::sqrt(var / static_cast<double>(n));
    return t;
}

struct BenchResult {
    std::string function;
    std::string workload;
    std::string desc;
    Timing timing;
};

template <class F>
static void AddBench(std::vector<BenchResult>& out,
                       const char* function, const char* workload,
                       const char* desc, int rounds, double timeMs,
                       double warmupMs, F&& fn) {
    BenchResult r;
    r.function = function;
    r.workload = workload;
    r.desc = desc;
    r.timing = TimeWorkload(std::forward<F>(fn), rounds, timeMs,
                              warmupMs);
    out.push_back(std::move(r));
}

// ---------------------------------------------------------------------
// JSON output
// ---------------------------------------------------------------------

static void EmitJson(const std::string& exeName,
                       const std::string& variant,
                       const BuildInfo& bi, int rounds,
                       double timeMs, double warmupMs,
                       const std::vector<TestResult>& tests,
                       const std::vector<BenchResult>& benches) {
    std::string s;
    s += "{\n";
    s += "  \"schema\": \"wintcp-bench/1\",\n";
    s += "  \"exe\": " + JsonStr(exeName) + ",\n";
    s += "  \"variant\": " + JsonStr(variant) + ",\n";
    s += "  \"build\": {\n";
    s += "    \"compiler\": " + JsonStr(bi.compiler) + ",\n";
    s += "    \"arch\": " + JsonStr(bi.arch) + ",\n";
    s += "    \"cpu\": " + JsonStr(bi.cpuName) + ",\n";
    s += "    \"ram_mb\": " + std::to_string(bi.ramMb) + ",\n";
    s += "    \"os\": " + JsonStr(bi.osName) + ",\n";
    s += "    \"features\": {\"sse42\": " +
            std::string(bi.sse42 ? "true" : "false") +
            ", \"avx2\": " +
            std::string(bi.avx2 ? "true" : "false") +
            ", \"bmi1\": " +
            std::string(bi.bmi1 ? "true" : "false") +
            ", \"bmi2\": " +
            std::string(bi.bmi2 ? "true" : "false") +
            ", \"popcnt\": " +
            std::string(bi.popcnt ? "true" : "false") + "}\n";
    s += "  },\n";
    s += "  \"config\": {\"rounds\": " + std::to_string(rounds) +
            ", \"time_ms\": " + Num(timeMs, 1) +
            ", \"warmup_ms\": " + Num(warmupMs, 1) + "},\n";

    size_t passed = 0;
    for (const TestResult& r : tests) {
        if (r.pass) ++passed;
    }
    s += "  \"correctness\": {\n";
    s += "    \"passed\": " + std::to_string(passed) + ",\n";
    s += "    \"failed\": " +
            std::to_string(tests.size() - passed) + ",\n";
    s += "    \"total\": " + std::to_string(tests.size()) +
            ",\n";
    s += "    \"tests\": [\n";
    for (size_t i = 0; i < tests.size(); ++i) {
        const TestResult& r = tests[i];
        s += "      {\"name\": " + JsonStr(r.name) +
                ", \"group\": " + JsonStr(r.group) +
                ", \"pass\": " +
                std::string(r.pass ? "true" : "false") +
                ", \"detail\": " + JsonStr(r.detail) + "}";
        s += (i + 1 < tests.size()) ? ",\n" : "\n";
    }
    s += "    ]\n";
    s += "  },\n";

    s += "  \"benchmarks\": [\n";
    for (size_t i = 0; i < benches.size(); ++i) {
        const BenchResult& b = benches[i];
        s += "    {\"function\": " + JsonStr(b.function) +
                ", \"workload\": " + JsonStr(b.workload) +
                ", \"desc\": " + JsonStr(b.desc) +
                ", \"ns_per_op\": " + Num(b.timing.nsMedian, 3) +
                ", \"ns_min\": " + Num(b.timing.nsMin, 3) +
                ", \"ns_p90\": " + Num(b.timing.nsP90, 3) +
                ", \"ns_mean\": " + Num(b.timing.nsMean, 3) +
                ", \"ns_stdev\": " + Num(b.timing.nsStdev, 3) +
                ", \"iterations\": " +
                std::to_string(b.timing.iterations) +
                ", \"rounds\": [";
        for (size_t r = 0; r < b.timing.rounds.size(); ++r) {
            s += Num(b.timing.rounds[r], 3);
            s += (r + 1 < b.timing.rounds.size()) ? ", " : "";
        }
        s += "]}";
        s += (i + 1 < benches.size()) ? ",\n" : "\n";
    }
    s += "  ]\n";
    s += "}\n";
    std::fwrite(s.data(), 1, s.size(), stdout);
}

static void EmitHuman(const std::string& exeName,
                        const std::string& variant,
                        const BuildInfo& bi, int rounds,
                        double timeMs,
                        const std::vector<TestResult>& tests,
                        const std::vector<BenchResult>& benches) {
    std::printf("=== wintcp benchmark ===\n");
    std::printf("exe:     %s\n", exeName.c_str());
    std::printf("variant: %s\n", variant.c_str());
    std::printf("build:   %s %s\n", bi.compiler.c_str(),
                bi.arch.c_str());
    std::printf("cpu:     %s (%zu MB RAM)\n", bi.cpuName.c_str(),
                bi.ramMb);
    std::printf("os:      %s\n", bi.osName.c_str());
    std::printf("features: sse4.2=%d avx2=%d bmi1=%d bmi2=%d "
                "popcnt=%d\n",
                bi.sse42, bi.avx2, bi.bmi1, bi.bmi2, bi.popcnt);

    size_t passed = 0;
    for (const TestResult& r : tests) {
        if (r.pass) ++passed;
    }
    std::printf("\n--- correctness: %zu/%zu passed ---\n", passed,
                tests.size());
    for (const TestResult& r : tests) {
        std::printf("  [%s] %s: %s%s\n",
                    r.pass ? "PASS" : "FAIL", r.group.c_str(),
                    r.name.c_str(),
                    r.detail.empty() ? ""
                                     : ("  (" + r.detail + ")").c_str());
    }

    std::printf("\n--- performance: %d rounds x %.0f ms "
                "(median ns/op) ---\n",
                rounds, timeMs);
    std::printf("  %-14s %-24s %12s %12s %12s\n", "function",
                "workload", "ns/op", "p90", "stdev");
    for (const BenchResult& b : benches) {
        std::printf("  %-14s %-24s %12.3f %12.3f %12.3f\n",
                    b.function.c_str(), b.workload.c_str(),
                    b.timing.nsMedian, b.timing.nsP90,
                    b.timing.nsStdev);
    }
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------

int main(int argc, char** argv) {
    // MSVC's swprintf_s treats a buffer-too-small condition as an
    // "invalid parameter" and the default handler aborts the process.
    // Install a no-op handler so FormatBytes can detect truncation
    // gracefully (returns -1 -> 0) instead of crashing.
    typedef void (__cdecl* IpnHandler)(
        const wchar_t*, const wchar_t*, const wchar_t*,
        unsigned int, uintptr_t);
    static const IpnHandler kNoopHandler =
        [](const wchar_t*, const wchar_t*, const wchar_t*,
           unsigned int, uintptr_t) {};
    _set_invalid_parameter_handler(
        reinterpret_cast<_invalid_parameter_handler>(kNoopHandler));

    bool wantJson = false;
    std::string phase = "all";
    std::string variantOverride;
    int rounds = 5;
    double timeMs = 80.0;
    double warmupMs = 20.0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") {
            wantJson = true;
        } else if (a == "--quick") {
            rounds = 3;
            timeMs = 40.0;
            warmupMs = 10.0;
        } else if (a.rfind("--phase=", 0) == 0) {
            phase = a.substr(8);
        } else if (a.rfind("--rounds=", 0) == 0) {
            rounds = std::atoi(a.c_str() + 9);
        } else if (a.rfind("--time-ms=", 0) == 0) {
            timeMs = std::atof(a.c_str() + 10);
        } else if (a.rfind("--warmup-ms=", 0) == 0) {
            warmupMs = std::atof(a.c_str() + 12);
        } else if (a.rfind("--variant=", 0) == 0) {
            variantOverride = a.substr(10);
        } else if (a == "--help") {
            std::printf(
                "usage: %s [--json] [--phase=all|correctness|bench] "
                "[--rounds=N] [--time-ms=MS] [--warmup-ms=MS] "
                "[--quick] [--variant=NAME]\n",
                argv[0]);
            return 0;
        }
    }

    const BuildInfo bi = CollectBuildInfo();
    std::string exeName = ExeBaseName();
    std::string variant = variantOverride;
    if (variant.empty()) {
        if (exeName.find("orig") != std::string::npos) {
            variant = "original";
        } else if (exeName.find("asm") != std::string::npos) {
            variant = "asm";
        } else {
            variant = "unknown";
        }
    }

    const bool runCorrect = phase != "bench";
    const bool runBench = phase != "correctness";

    // ---- correctness ----
    std::vector<TestResult> tests;
    if (runCorrect) {
        GeoIpTests(tests);
        ShiftWalkTests(tests);
        ReasmTests(tests);
        SubstringTests(tests);
        ParseTests(tests);
        FormatTests(tests);
        KeyTests(tests);
        Utf8Tests(tests);
        HexTests(tests);
        LowerTests(tests);
        BeReadsTests(tests);
        TlsBeTests(tests);
        TlsExtTests(tests);
        RdTests(tests);
        JoinTests(tests);
        FindLongTests(tests);
        LowerAllTests(tests);
        CmpWideTests(tests);
        KeyHashTests(tests);
        BpsTests(tests);
        PidSumTests(tests);
        BpsCellTests(tests);
        WidenTests(tests);
        UnicastTests(tests);
        PairTests(tests);
        CountedKeyTests(tests);
        FlowTests(tests);
        HandleTests(tests);
        EtwTests(tests);
        JsonTests(tests);
        CsvTests(tests);
        IntFmtTests(tests);
        IpFmtTests(tests);
        WidthTests(tests);
        MmdbTests(tests);
        VlanFlowTests(tests);
        RenderPidTests(tests);
    }

    // ---- benchmarks ----
    std::vector<BenchResult> benches;
    if (runBench) {
        // 1. GeoIP tree walks.
        {
            SyntheticTree t256 =
                MakeTree(256, false, 3, 101, 512, 0.35, 0.45);
            unsigned char bits[64][16];
            uint64_t rng = 501;
            for (int i = 0; i < 64; ++i) {
                BitsFromUint32(
                    bits[i],
                    static_cast<uint32_t>(NextRand(rng)));
            }
            size_t walkIdx = 0;
            AddBench(benches, "GeoIpWalk", "tree_256_24bit",
                     "256-node tree, 24-bit records, 64 addresses",
                     rounds, timeMs, warmupMs,
                     [&] {
                         size_t off = 0;
                         const bool found = GeoIpWalk(
                             bits[walkIdx & 63], 32, 0,
                             &t256.tree, &off);
                         g_sink += found ? off : 1;
                         ++walkIdx;
                     });
        }
        {
            SyntheticTree t65k =
                MakeTree(65536, false, 3, 20240, 8192, 0.6, 0.3);
            unsigned char bits[64][16];
            uint64_t rng = 502;
            for (int i = 0; i < 64; ++i) {
                BitsFromUint32(
                    bits[i],
                    static_cast<uint32_t>(NextRand(rng)));
            }
            size_t walkIdx = 0;
            AddBench(benches, "GeoIpWalk", "tree_65k_24bit",
                     "65k-node tree, 24-bit records (~393 KB)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         size_t off = 0;
                         const bool found = GeoIpWalk(
                             bits[walkIdx & 63], 32, 0,
                             &t65k.tree, &off);
                         g_sink += found ? off : 1;
                         ++walkIdx;
                     });
        }
        {
            SyntheticTree t1m = MakeTree(1048576, false, 3, 777,
                                           65536, 0.6, 0.3);
            unsigned char bits[64][16];
            uint64_t rng = 503;
            for (int i = 0; i < 64; ++i) {
                BitsFromUint32(
                    bits[i],
                    static_cast<uint32_t>(NextRand(rng)));
            }
            size_t walkIdx = 0;
            AddBench(benches, "GeoIpWalk", "tree_1m_24bit",
                     "1M-node tree, 24-bit records (~6 MB, "
                     "cache-miss dominated)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         size_t off = 0;
                         const bool found = GeoIpWalk(
                             bits[walkIdx & 63], 32, 0,
                             &t1m.tree, &off);
                         g_sink += found ? off : 1;
                         ++walkIdx;
                     });
        }
        {
            SyntheticTree t28 =
                MakeTree(65536, true, 0, 20241, 8192, 0.6, 0.3);
            unsigned char bits[64][16];
            uint64_t rng = 504;
            for (int i = 0; i < 64; ++i) {
                BitsFromUint32(
                    bits[i],
                    static_cast<uint32_t>(NextRand(rng)));
            }
            size_t walkIdx = 0;
            AddBench(benches, "GeoIpWalk", "tree_65k_28bit",
                     "65k-node tree, packed 28-bit records "
                     "(7 bytes/node)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         size_t off = 0;
                         const bool found = GeoIpWalk(
                             bits[walkIdx & 63], 32, 0,
                             &t28.tree, &off);
                         g_sink += found ? off : 1;
                         ++walkIdx;
                     });
        }
        {
            SyntheticTree t32 =
                MakeTree(65536, false, 4, 20242, 8192, 0.6, 0.3);
            unsigned char bits[64][16];
            uint64_t rng = 505;
            for (int i = 0; i < 64; ++i) {
                BitsFromUint32(
                    bits[i],
                    static_cast<uint32_t>(NextRand(rng)));
            }
            size_t walkIdx = 0;
            AddBench(benches, "GeoIpWalk", "tree_65k_32bit",
                     "65k-node tree, 32-bit records (8 bytes/node)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         size_t off = 0;
                         const bool found = GeoIpWalk(
                             bits[walkIdx & 63], 32, 0,
                             &t32.tree, &off);
                         g_sink += found ? off : 1;
                         ++walkIdx;
                     });
        }
        {
            SyntheticTree tv6 =
                MakeTree(65536, false, 3, 20243, 8192, 0.6, 0.3);
            unsigned char bits[64][16];
            uint64_t rng = 506;
            for (int i = 0; i < 64; ++i) {
                for (int b = 0; b < 16; ++b) {
                    bits[i][b] =
                        static_cast<unsigned char>(NextRand(rng));
                }
            }
            size_t walkIdx = 0;
            AddBench(benches, "GeoIpWalk", "tree_65k_ipv6",
                     "65k-node tree, full 128-bit IPv6 walks",
                     rounds, timeMs, warmupMs,
                     [&] {
                         size_t off = 0;
                         const bool found = GeoIpWalk(
                             bits[walkIdx & 63], 128, 0,
                             &tv6.tree, &off);
                         g_sink += found ? off : 1;
                         ++walkIdx;
                     });
        }

        // 27b. Shift-register walker: same trees, same addresses, second
        // wiring of the walk (MSB-first shift register + straight-line
        // record walkers instead of per-bit division/modulo).
        {
            SyntheticTree s256 = MakeTree(256, false, 3, 101, 512, 0.35, 0.45);
            unsigned char bits[64][16];
            uint64_t rng = 501;
            for (int i = 0; i < 64; ++i) {
                BitsFromUint32(bits[i], static_cast<uint32_t>(NextRand(rng)));
            }
            size_t walkIdx = 0;
            AddBench(benches, "GeoIpWalkShift", "tree_256_24bit",
                     "shift-register walk, 256-node 24-bit tree",
                     rounds, timeMs, warmupMs,
                     [&] {
                         size_t off = 0;
                         const bool found = GeoIpWalkShift(
                             bits[walkIdx & 63], 32, 0, &s256.tree, &off);
                         g_sink += found ? off : 1;
                         ++walkIdx;
                     });
        }
        {
            SyntheticTree s65 = MakeTree(65536, false, 3, 10237, 1024, 0.35, 0.45);
            unsigned char bits[64][16];
            uint64_t rng = 502;
            for (int i = 0; i < 64; ++i) {
                BitsFromUint32(bits[i], static_cast<uint32_t>(NextRand(rng)));
            }
            size_t walkIdx = 0;
            AddBench(benches, "GeoIpWalkShift", "tree_65k_24bit",
                     "shift-register walk, 65k-node 24-bit tree",
                     rounds, timeMs, warmupMs,
                     [&] {
                         size_t off = 0;
                         const bool found = GeoIpWalkShift(
                             bits[walkIdx & 63], 32, 0, &s65.tree, &off);
                         g_sink += found ? off : 1;
                         ++walkIdx;
                     });
        }
        {
            SyntheticTree s28 = MakeTree(65536, true, 3, 7201, 2048, 0.3, 0.6);
            unsigned char bits[64][16];
            uint64_t rng = 503;
            for (int i = 0; i < 64; ++i) {
                BitsFromUint32(bits[i], static_cast<uint32_t>(NextRand(rng)));
            }
            size_t walkIdx = 0;
            AddBench(benches, "GeoIpWalkShift", "tree_65k_28bit",
                     "shift-register walk, 65k-node 28-bit tree",
                     rounds, timeMs, warmupMs,
                     [&] {
                         size_t off = 0;
                         const bool found = GeoIpWalkShift(
                             bits[walkIdx & 63], 32, 0, &s28.tree, &off);
                         g_sink += found ? off : 1;
                         ++walkIdx;
                     });
        }
        {
            SyntheticTree s32 = MakeTree(65536, false, 4, 14831, 8192, 0.3, 0.5);
            unsigned char bits[64][16];
            uint64_t rng = 504;
            for (int i = 0; i < 64; ++i) {
                BitsFromUint32(bits[i], static_cast<uint32_t>(NextRand(rng)));
            }
            size_t walkIdx = 0;
            AddBench(benches, "GeoIpWalkShift", "tree_65k_32bit",
                     "shift-register walk, 65k-node 32-bit tree",
                     rounds, timeMs, warmupMs,
                     [&] {
                         size_t off = 0;
                         const bool found = GeoIpWalkShift(
                             bits[walkIdx & 63], 32, 0, &s32.tree, &off);
                         g_sink += found ? off : 1;
                         ++walkIdx;
                     });
        }

        // 35. VLAN skip + flow-record probe (link-layer path).
        {
            std::vector<unsigned char> eth(128, 0);
            for (size_t i = 0; i < eth.size(); ++i) {
                eth[i] = static_cast<unsigned char>((i * 13) & 0xFF);
            }
            eth[12] = 0x08;
            eth[13] = 0x00;
            eth[14] = 0x45;
            // A QinQ frame: two tags before the ethertype.
            std::vector<unsigned char> qinq = eth;
            qinq[12] = 0x91;
            qinq[13] = 0x00;
            qinq[16] = 0x81;
            qinq[17] = 0x00;
            qinq[20] = 0x08;
            qinq[21] = 0x00;
            qinq[22] = 0x45;
            AddBench(benches, "SkipVlan", "vlan_plain",
                     "untagged ethernet frame",
                     rounds, timeMs, warmupMs,
                     [&] { g_sink += SkipVlan(eth.data(), eth.size(), 12); });
            AddBench(benches, "SkipVlan", "vlan_qinq",
                     "QinQ frame, two stacked tags",
                     rounds, timeMs, warmupMs,
                     [&] { g_sink += SkipVlan(qinq.data(), qinq.size(), 12); });

            // The flow probe only runs for frames that failed to parse, so
            // the workload is exactly that: an EPB-sized frame with no
            // parseable IP header.
            std::vector<unsigned char> flowish(96, 0);
            uint64_t frng = 909;
            for (size_t i = 0; i < flowish.size(); ++i) {
                flowish[i] = static_cast<unsigned char>(NextRand(frng));
            }
            flowish[12] = 0x08;
            flowish[13] = 0x00;
            flowish[14] = 0x45;
            std::vector<unsigned char> noflow = flowish;
            noflow[12] = 0x00;
            noflow[13] = 0x11;
            AddBench(benches, "FlowProbe", "flowprobe_hit",
                     "flow record found at candidate 12",
                     rounds, timeMs, warmupMs,
                     [&] {
                         if (FlowProbe(flowish.data(), flowish.size())) {
                             g_sink += 1;
                         }
                     });
            AddBench(benches, "FlowProbe", "flowprobe_miss",
                     "no signature, full window scanned",
                     rounds, timeMs, warmupMs,
                     [&] {
                         if (FlowProbe(noflow.data(), noflow.size())) {
                             g_sink += 1;
                         }
                     });
            AddBench(benches, "FlowProbe", "flowprobe_short",
                     "32-byte frame (one SIMD block + scalar tail)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         if (FlowProbe(noflow.data(), 32)) g_sink += 1;
                     });
        }

        // 36. TCP reassembly render: sort + concatenate one stream.
        // Segment counts straddle the radix/sort threshold so the report
        // shows which side of it wins.
        {
            auto makeRenderView = [](size_t count, uint64_t seed,
                                     bool withHoles) {
                ReasmRenderView v;
                uint64_t rng = seed;
                uint64_t seq = 100000;
                std::vector<unsigned char> pool;
                for (size_t i = 0; i < count; ++i) {
                    const size_t len = 64 + (NextRand(rng) % 900);
                    v.seqs.push_back(seq);
                    v.starts.push_back(pool.size());
                    v.sizes.push_back(len);
                    for (size_t k = 0; k < len; ++k) {
                        pool.push_back(
                            static_cast<unsigned char>((i * 7 + k) & 0xFF));
                    }
                    seq += len;
                    if (withHoles && (NextRand(rng) % 8 == 0)) {
                        seq += NextRand(rng) % 512 + 1;
                    }
                }
                v.pool = std::move(pool);
                return v;
            };
            const ReasmRenderView r64 = makeRenderView(64, 9101, true);
            const ReasmRenderView r96 = makeRenderView(96, 9102, true);
            const ReasmRenderView r300 = makeRenderView(300, 9103, true);
            const ReasmRenderView r4096 = makeRenderView(4096, 9104, true);
            const ReasmRenderView rSeq = makeRenderView(1024, 9105, false);
            for (const auto* r : {&r64, &r96, &r300, &r4096, &rSeq}) {
                std::string name;
                std::string desc;
                if (r == &r64) {
                    name = "render_64_segs";
                    desc = "64 segments (below the radix threshold)";
                } else if (r == &r96) {
                    name = "render_96_segs";
                    desc = "96 segments (at the radix threshold)";
                } else if (r == &r300) {
                    name = "render_300_segs";
                    desc = "300 segments with holes";
                } else if (r == &r4096) {
                    name = "render_4096_segs";
                    desc = "4096 segments, ~3 MB of stream";
                } else {
                    name = "render_1024_contig";
                    desc = "1024 contiguous segments, no gap path";
                }
                AddBench(benches, "ReasmRender", name.c_str(), desc.c_str(),
                         rounds, timeMs, warmupMs, [r] {
                             ReasmRenderResult out;
                             RenderSegments(*r, 3, false, &out);
                             g_sink += out.bytes.size() +
                                       out.bytesMissing +
                                       (out.hasGap ? 1u : 0u);
                         });
            }
        }

        // 37. PID dedup + grouping.
        {
            // r2k_rows_x12pids: a browser with 2000 sockets, 12 processes.
            auto rows = [](size_t n, uint32_t distinct, uint64_t seed) {
                std::vector<uint32_t> v;
                uint64_t rng = seed;
                v.reserve(n);
                for (size_t i = 0; i < n; ++i) {
                    v.push_back(static_cast<uint32_t>(
                        1000 + (NextRand(rng) % distinct)));
                }
                return v;
            };
            const std::vector<uint32_t> r2k = rows(2000, 12, 9201);
            const std::vector<uint32_t> r200 =
                rows(200, 200, 9202);   // one pid per row
            const std::vector<uint32_t> r20k = rows(20000, 400, 9203);
            for (const auto* p : {&r2k, &r200, &r20k}) {
                std::string name;
                std::string desc;
                if (p == &r2k) {
                    name = "distinctpids_2000x12";
                    desc = "2000 sockets across 12 processes";
                } else if (p == &r200) {
                    name = "distinctpids_200x200";
                    desc = "200 rows, all distinct (worst case)";
                } else {
                    name = "distinctpids_20kx400";
                    desc = "20k sockets across 400 processes";
                }
                AddBench(benches, "DistinctPids", name.c_str(), desc.c_str(),
                         rounds, timeMs, warmupMs, [p] {
                             const std::vector<uint32_t> r =
                                 DistinctPids(p->data(), p->size());
                             g_sink += r.size();
                         });
                AddBench(benches, "GroupByPid", name.c_str(), desc.c_str(),
                         rounds, timeMs, warmupMs, [p] {
                             const PidGroups g =
                                 GroupByPid(p->data(), p->size());
                             g_sink += g.pids.size() + g.rows.size();
                         });
            }
        }

        // 2. TCP reassembly overlap.
        {
            auto makeStore = [](size_t segCount, uint64_t seed) {
                ReasmWorkload w;
                uint64_t rng = seed;
                uint64_t seq = 1000;
                for (size_t i = 0; i < segCount; ++i) {
                    const size_t len = 400 + (NextRand(rng) % 500);
                    w.aos.push_back({seq, len});
                    seq += len + (NextRand(rng) % 50);
                }
                for (const ReasmSeg& s : w.aos) {
                    w.seqs.push_back(s.seq);
                    w.sizes.push_back(s.len);
                }
                return w;
            };
            struct Case {
                const char* workload;
                const char* desc;
                size_t segCount;
                uint64_t newSeq;
                size_t newLen;
            };
            const Case cases[] = {
                {"reasm_4", "4 segments, mid-store overlap", 4,
                 1200, 700},
                {"reasm_64", "64 segments, mid-store overlap", 64,
                 15000, 900},
                {"reasm_1024", "1024 segments, mid-store overlap",
                 1024, 300000, 1100},
                {"reasm_8192", "8192 segments, mid-store overlap",
                 8192, 3000000, 1200},
            };
            for (const Case& c : cases) {
                ReasmWorkload w = makeStore(c.segCount, 600 + c.segCount);
                size_t opIdx = 0;
                AddBench(benches, "ReasmOverlap", c.workload, c.desc,
                         rounds, timeMs, warmupMs,
                         [&w, c, &opIdx] {
                             size_t consumed = 0, skip = 0;
                             const bool hasNew = ReasmOverlap(
                                 c.newSeq + (opIdx & 1), c.newLen,
                                 &w, &consumed, &skip);
                             g_sink += consumed + skip +
                                           (hasNew ? 1u : 0u);
                             ++opIdx;
                         });
            }
            // Full-duplicate early-out case.
            ReasmWorkload w = makeStore(64, 699);
            if (!w.aos.empty()) {
                const ReasmSeg& first = w.aos.front();
                size_t opIdx = 0;
                AddBench(benches, "ReasmOverlap", "reasm_64_dup",
                         "64 segments, incoming is a full duplicate",
                         rounds, timeMs, warmupMs,
                         [&w, &first, &opIdx] {
                             size_t consumed = 0, skip = 0;
                             const bool hasNew = ReasmOverlap(
                                 first.seq, first.len, &w,
                                 &consumed, &skip);
                             g_sink += consumed + skip +
                                           (hasNew ? 1u : 0u);
                             ++opIdx;
                         });
            }
        }

        // 3. Filter substring search.
        {
            auto makeHay = [](size_t n, uint64_t seed) {
                std::wstring h(n, L'a');
                uint64_t rng = seed;
                for (size_t i = 0; i < n; ++i) {
                    h[i] = static_cast<wchar_t>(
                        L'a' + (NextRand(rng) % 26));
                }
                return h;
            };
            const std::wstring h16 = makeHay(16, 701);
            const std::wstring h256 = makeHay(256, 702);
            const std::wstring h4k = makeHay(4096, 703);
            const std::wstring h64k = makeHay(65536, 704);
            const std::wstring needle = L"wintcp";
            const std::wstring absent = L"zzqq";
            AddBench(benches, "HasLowerSubstring", "sub_16_present",
                     "16-char haystack, needle present",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += HasLowerSubstring(h16, needle)
                                       ? 1u
                                       : 0u;
                     });
            AddBench(benches, "HasLowerSubstring", "sub_16_absent",
                     "16-char haystack, needle absent",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += HasLowerSubstring(h16, absent)
                                       ? 1u
                                       : 0u;
                     });
            AddBench(benches, "HasLowerSubstring", "sub_256_absent",
                     "256-char haystack, needle absent",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += HasLowerSubstring(h256, absent)
                                       ? 1u
                                       : 0u;
                     });
            AddBench(benches, "HasLowerSubstring", "sub_4k_absent",
                     "4K-char haystack, needle absent (full scan)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += HasLowerSubstring(h4k, absent)
                                       ? 1u
                                       : 0u;
                     });
            AddBench(benches, "HasLowerSubstring", "sub_4k_present",
                     "4K-char haystack, needle near the end",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::wstring h = h4k.substr(0, 4090) +
                                                   needle;
                         g_sink += HasLowerSubstring(h, needle)
                                       ? 1u
                                       : 0u;
                     });
            AddBench(benches, "HasLowerSubstring", "sub_64k_absent",
                     "64K-char haystack, needle absent (full scan)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += HasLowerSubstring(h64k, absent)
                                       ? 1u
                                       : 0u;
                     });
        }

        // 4. TCP header parsing.
        {
            unsigned char pkt[1600] = {0};
            pkt[0] = 0x1F; pkt[1] = 0x90;   // sport 8080
            pkt[2] = 0x00; pkt[3] = 0x50;   // dport 80
            pkt[4] = 0x11; pkt[5] = 0x22; pkt[6] = 0x33;
            pkt[7] = 0x44;
            pkt[8] = 0x55; pkt[9] = 0x66; pkt[10] = 0x77;
            pkt[11] = 0x88;
            pkt[12] = 0x50;  // data offset 5
            pkt[13] = 0x18;  // PSH|ACK
            pkt[14] = 0x20; pkt[15] = 0x00;
            for (int i = 20; i < 1600; ++i) {
                pkt[i] = static_cast<unsigned char>(i);
            }
            AddBench(benches, "ParseTcpHeader", "tcp_20",
                     "minimal 20-byte TCP header",
                     rounds, timeMs, warmupMs,
                     [&] {
                         TcpHeaderFields f;
                         const bool ok =
                             ParseTcpHeader(pkt, 0, 20, &f);
                         g_sink += f.srcPort + f.dstPort + f.seq +
                                       f.ack + f.window +
                                       (ok ? 1u : 0u);
                     });
            pkt[12] = 0x80;  // data offset 8: 32-byte header
            AddBench(benches, "ParseTcpHeader", "tcp_52_payload",
                     "32-byte header + 1568-byte payload",
                     rounds, timeMs, warmupMs,
                     [&] {
                         TcpHeaderFields f;
                         const bool ok =
                             ParseTcpHeader(pkt, 0, 1600, &f);
                         g_sink += f.srcPort + f.dstPort + f.seq +
                                       f.ack + f.window +
                                       f.payloadLen +
                                       (ok ? 1u : 0u);
                     });
            pkt[12] = 0x50;
            AddBench(benches, "ParseTcpHeader", "tcp_12_invalid",
                     "truncated packet (failure path)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         TcpHeaderFields f;
                         const bool ok =
                             ParseTcpHeader(pkt, 0, 12, &f);
                         g_sink += (ok ? 1u : 0u) + f.window;
                     });
        }

        // 5. FormatBytes.
        {
            auto makeValues = [](uint64_t base, uint64_t span,
                                   uint64_t seed) {
                std::vector<uint64_t> v;
                uint64_t rng = seed;
                for (int i = 0; i < 100; ++i) {
                    v.push_back(base + (NextRand(rng) % span));
                }
                return v;
            };
            const std::vector<uint64_t> small =
                makeValues(0, 2048, 801);
            const std::vector<uint64_t> mb =
                makeValues(1ull << 20, 1ull << 28, 802);
            const std::vector<uint64_t> gb =
                makeValues(1ull << 30, 1ull << 36, 803);
            const std::vector<uint64_t> tb =
                makeValues(1ull << 40, 1ull << 44, 804);
            size_t idx = 0;
            wchar_t buf[32] = {0};
            AddBench(benches, "FormatBytes", "fmt_small",
                     "values 0-2 KB (integer path for < 1 KB)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const size_t n = FormatBytes(
                             buf, 32, small[idx % 100]);
                         g_sink += n + buf[0];
                         ++idx;
                     });
            AddBench(benches, "FormatBytes", "fmt_mb",
                     "values around 1 MB - 512 MB (%.1f MB)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const size_t n = FormatBytes(
                             buf, 32, mb[idx % 100]);
                         g_sink += n + buf[0];
                         ++idx;
                     });
            AddBench(benches, "FormatBytes", "fmt_gb",
                     "values around 1 GB - 64 GB (%.2f/%.1f GB)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const size_t n = FormatBytes(
                             buf, 32, gb[idx % 100]);
                         g_sink += n + buf[0];
                         ++idx;
                     });
            AddBench(benches, "FormatBytes", "fmt_tb",
                     "values around 1 TB - 16 TB (%.2f/%.1f TB)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const size_t n = FormatBytes(
                             buf, 32, tb[idx % 100]);
                         g_sink += n + buf[0];
                         ++idx;
                     });
        }

        // 6. Connection keys.
        {
            struct ConnCase {
                bool ipv6;
                unsigned char local[16];
                unsigned char remote[16];
                uint32_t localPort;
                uint32_t remotePort;
                uint32_t pid;
            };
            std::vector<ConnCase> conns;
            uint64_t rng = 901;
            for (int i = 0; i < 32; ++i) {
                ConnCase c;
                c.ipv6 = false;
                for (int b = 0; b < 16; ++b) {
                    c.local[b] =
                        static_cast<unsigned char>(NextRand(rng));
                    c.remote[b] =
                        static_cast<unsigned char>(NextRand(rng));
                }
                c.localPort =
                    static_cast<uint32_t>(NextRand(rng) & 0xFFFF);
                c.remotePort =
                    static_cast<uint32_t>(NextRand(rng) & 0xFFFF);
                c.pid = static_cast<uint32_t>(NextRand(rng));
                conns.push_back(c);
            }
            for (int i = 0; i < 32; ++i) {
                ConnCase c;
                c.ipv6 = true;
                for (int b = 0; b < 16; ++b) {
                    c.local[b] =
                        static_cast<unsigned char>(NextRand(rng));
                    c.remote[b] =
                        static_cast<unsigned char>(NextRand(rng));
                }
                c.localPort =
                    static_cast<uint32_t>(NextRand(rng) & 0xFFFF);
                c.remotePort =
                    static_cast<uint32_t>(NextRand(rng) & 0xFFFF);
                c.pid = static_cast<uint32_t>(NextRand(rng));
                conns.push_back(c);
            }
            unsigned char keyBuf[64] = {0};
            size_t keyLen = 0;
            size_t idx = 0;
            AddBench(benches, "KeyOf", "key_v4",
                     "64 prebuilt IPv4 connections",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const ConnCase& c = conns[idx & 31];
                         ConnKeyInput in;
                         in.ipv6 = false;
                         in.localAddr = c.local;
                         in.remoteAddr = c.remote;
                         in.localPort = c.localPort;
                         in.remotePort = c.remotePort;
                         in.pid = c.pid;
                         KeyOf(&in, keyBuf, &keyLen);
                         g_sink += keyLen + keyBuf[0] + keyBuf[1];
                         ++idx;
                     });
            AddBench(benches, "KeyOf", "key_v6",
                     "64 prebuilt IPv6 connections (46-byte keys)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const ConnCase& c = conns[32 + (idx & 31)];
                         ConnKeyInput in;
                         in.ipv6 = true;
                         in.localAddr = c.local;
                         in.remoteAddr = c.remote;
                         in.localPort = c.localPort;
                         in.remotePort = c.remotePort;
                         in.pid = c.pid;
                         KeyOf(&in, keyBuf, &keyLen);
                         g_sink += keyLen + keyBuf[0] + keyBuf[1];
                         ++idx;
                     });
        }

        // 7. WideToUtf8.
        {
            const std::wstring ascii(39, L'a');
            std::wstring mixed = L"a";
            for (int i = 0; i < 64; ++i) mixed += L"\x00C0\x3042";
            std::wstring emoji;
            for (int i = 0; i < 64; ++i) {
                emoji += L'\xD83D';
                emoji += L'\xDE00';
            }
            const std::wstring long4k(4096, L'x');
            std::string dst(8192, '\0');
            AddBench(benches, "WideToUtf8", "utf8_ascii",
                     "39-char ASCII string (1 byte/char)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const size_t n = WideToUtf8(
                             ascii.c_str(), ascii.size(),
                             &dst[0], dst.size());
                         g_sink += n + dst[0];
                     });
            AddBench(benches, "WideToUtf8", "utf8_mixed",
                     "129-char mixed ASCII/CJK (2-3 bytes/char)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const size_t n = WideToUtf8(
                             mixed.c_str(), mixed.size(),
                             &dst[0], dst.size());
                         g_sink += n + dst[0];
                     });
            AddBench(benches, "WideToUtf8", "utf8_emoji",
                     "128-char surrogate pairs (4 bytes/char)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const size_t n = WideToUtf8(
                             emoji.c_str(), emoji.size(),
                             &dst[0], dst.size());
                         g_sink += n + dst[0];
                     });
            AddBench(benches, "WideToUtf8", "utf8_4k",
                     "4K-char ASCII string",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const size_t n = WideToUtf8(
                             long4k.c_str(), long4k.size(),
                             &dst[0], dst.size());
                         g_sink += n + dst[0];
                     });
        }

        // 11. Big-endian field reads (GeoIP DataReader + TLS Reader).
        {
            unsigned char buf[64];
            uint64_t rng = 803;
            for (size_t i = 0; i < sizeof(buf); ++i) {
                buf[i] = static_cast<unsigned char>(NextRand(rng));
            }
            size_t offIdx = 0;
            size_t nIdx = 0;
            AddBench(benches, "GeoRead", "be_u32_unaligned",
                     "u32 loads at cycling offsets 0..7",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += GeoReadU32BE(buf + (offIdx++ & 7));
                     });
            AddBench(benches, "GeoRead", "be_u64_unaligned",
                     "u64 loads at cycling offsets 0..7",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += GeoReadU64BE(buf + (offIdx++ & 7)) & 0xFF;
                     });
            AddBench(benches, "GeoRead", "be_bytes_14",
                     "1..4-byte payloads (pointer/size fast path)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         uint64_t v = 0;
                         GeoReadBytes(buf + (offIdx & 7),
                                      1 + (nIdx++ & 3), &v);
                         g_sink += v & 0xFF;
                     });
            AddBench(benches, "GeoRead", "be_bytes_58",
                     "5..8-byte payloads (uint fields)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         uint64_t v = 0;
                         GeoReadBytes(buf + (offIdx & 7),
                                      5 + (nIdx & 3), &v);
                         g_sink += v & 0xFF;
                     });
            AddBench(benches, "TlsBe", "tls_rec_hdr",
                     "5-byte record header walk (type/ver/len)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const unsigned char* h = buf + (offIdx++ & 7);
                         g_sink += h[0] + TlsBe16(h + 1) +
                                       TlsBe16(h + 3);
                     });
            AddBench(benches, "TlsBe", "tls_u24",
                     "24-bit handshake lengths",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += TlsBe24(buf + (offIdx++ & 7)) & 0xFF;
                     });
        }

        // 15/16/17. Long-needle search, lower-all concat, wide compare
        // (filter + sort path).
        {
            std::wstring hay4k(4096, L'\0');
            uint64_t rng = 805;
            for (size_t i = 0; i < hay4k.size(); ++i) {
                hay4k[i] = static_cast<wchar_t>(
                    L'a' + (NextRand(rng) % 26));
            }
            const std::wstring ndl8 = L"wintcpfi";
            const std::wstring ndl16 = L"wintcpfiltertest";
            const std::wstring ndl32 =
                L"0123456789abcdef0123456789abcdef";
            const std::wstring absent(32, L'Z');
            std::wstring hayHit = hay4k.substr(0, 4000) + ndl32;
            AddBench(benches, "FindSubstringLong", "findlong_8",
                     "4K haystack, 8-char needle absent",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FindSubstringLong(hay4k, ndl8) ? 1u
                                                                  : 0u;
                     });
            AddBench(benches, "FindSubstringLong", "findlong_16",
                     "4K haystack, 16-char needle absent",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FindSubstringLong(hay4k, ndl16) ? 1u
                                                                   : 0u;
                     });
            AddBench(benches, "FindSubstringLong", "findlong_32_absent",
                     "4K haystack, 32-char needle absent",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FindSubstringLong(hay4k, absent) ? 1u
                                                                    : 0u;
                     });
            AddBench(benches, "FindSubstringLong", "findlong_32_present",
                     "4K haystack, 32-char needle at end",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FindSubstringLong(hayHit, ndl32) ? 1u
                                                                    : 0u;
                     });
            const std::wstring fv[10] = {
                L"10.0.0.5",   L"93.184.216.34", L"established",
                L"4242",       L"chrome.exe",    L"c:\\windows\\s.exe",
                L"svchost",    L"host.example",  L"tcp",
                L"parent.exe"};
            const std::wstring* const f[] = {
                &fv[0], &fv[1], &fv[2], &fv[3], &fv[4],
                &fv[5], &fv[6], &fv[7], &fv[8], &fv[9]};
            AddBench(benches, "BuildLowerAll", "lowerall_10",
                     "10-field row concat",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::wstring r = BuildLowerAll(f, 10);
                         g_sink += r.size() +
                                       static_cast<unsigned>(r[0]);
                     });
            std::wstring cmpA(256, L'm');
            std::wstring cmpB(256, L'm');
            cmpB[250] = L'n';
            std::wstring cmpC(256, L'm');
            AddBench(benches, "CompareWide", "cmpwide_eq_256",
                     "256-char equal strings",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += static_cast<uint64_t>(
                             CompareWide(cmpA.c_str(), 256,
                                         cmpC.c_str(), 256) +
                             1);
                     });
            AddBench(benches, "CompareWide", "cmpwide_late_256",
                     "256-char strings differ at 250",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += static_cast<uint64_t>(
                             CompareWide(cmpA.c_str(), 256,
                                         cmpB.c_str(), 256) +
                             1);
                     });
            const std::wstring shortA = L"chrome.exe";
            const std::wstring shortB = L"chromf.exe";
            AddBench(benches, "CompareWide", "cmpwide_short",
                     "10-char strings differ at 5",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += static_cast<uint64_t>(
                             CompareWide(shortA.c_str(), 10,
                                         shortB.c_str(), 10) +
                             1);
                     });
        }

        // 18/19/20. Connection-key hash/eq, batch rates, PID sums
        // (refresh-tick pairing + rate path).
        {
            unsigned char key22[22] = {
                '4', 'T', 192, 168, 1, 10, 0x39, 0xC8, 0x00, 0x00,
                8,   8,   8,   8,    0xBB, 0x01, 0x00, 0x00, 0x92,
                0x10, 0x00, 0x00};
            unsigned char key46[46] = {
                '6', 'T', 0x20, 0x01, 0x0D, 0xB8, 1, 2,  3,  4,  5,
                6,   7,   8,   9,    10,   11,   12, 80, 0,  0,  0,
                0xFE, 0x80, 1,  2,    3,    4,    5,  6,  7,  8,  9,
                10,  11,  12,  13,   14,   0xBB, 0x01, 0,  0,  7,  0,
                0,   0};
            unsigned char key46b[46];
            std::memcpy(key46b, key46, 46);
            key46b[45] ^= 0xFF;
            AddBench(benches, "ConnKey", "keyhash_22",
                     "22-byte IPv4 key hash",
                     rounds, timeMs, warmupMs,
                     [&] { g_sink += HashConnKey(key22, 22); });
            AddBench(benches, "ConnKey", "keyhash_46",
                     "46-byte IPv6 key hash",
                     rounds, timeMs, warmupMs,
                     [&] { g_sink += HashConnKey(key46, 46); });
            AddBench(benches, "ConnKey", "keyeq_46",
                     "46-byte key equality (equal)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += EqualConnKey(key46, 46, key46, 46)
                                       ? 1u
                                       : 0u;
                     });
            AddBench(benches, "ConnKey", "keyeq_46_diff",
                     "46-byte key equality (differ at 45)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += EqualConnKey(key46, 46, key46b, 46)
                                       ? 1u
                                       : 0u;
                     });

            const size_t kRates = 1024;
            std::vector<uint64_t> prevB(kRates), nowB(kRates);
            std::vector<double> outB(kRates);
            uint64_t rng = 806;
            for (size_t i = 0; i < kRates; ++i) {
                prevB[i] = NextRand(rng) % 1000000;
                nowB[i] = prevB[i] + (NextRand(rng) % 1460);
            }
            AddBench(benches, "ComputeBps", "bps_1k",
                     "1024 counters, uniform 1000 ms tick",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += ComputeBpsBatch(prevB.data(),
                                                   nowB.data(), 1000,
                                                   outB.data(), kRates);
                     });

            auto makePidRows = [](size_t n, size_t nPids,
                                  uint64_t seed) {
                std::vector<PidTrafficRow> rows(n);
                uint64_t rng = seed;
                for (size_t i = 0; i < n; ++i) {
                    rows[i].pid =
                        static_cast<uint32_t>(NextRand(rng) % nPids);
                    rows[i].counted = (NextRand(rng) % 10) != 0;
                    rows[i].rx = NextRand(rng) % 100000;
                    rows[i].tx = NextRand(rng) % 10000;
                }
                return rows;
            };
            std::vector<PidTrafficRow> pid500 = makePidRows(500, 50, 807);
            std::vector<PidTrafficRow> pid5k = makePidRows(5000, 700, 808);
            AddBench(benches, "SumPidTraffic", "pidsum_500_50",
                     "500 rows, 50 PIDs",
                     rounds, timeMs, warmupMs,
                     [&] {
                         SumPidTraffic(pid500.data(), pid500.size());
                         g_sink += pid500[0].sumRx + pid500[0].sumTx;
                     });
            AddBench(benches, "SumPidTraffic", "pidsum_5k_700",
                     "5000 rows, 700 PIDs",
                     rounds, timeMs, warmupMs,
                     [&] {
                         SumPidTraffic(pid5k.data(), pid5k.size());
                         g_sink += pid5k[0].sumRx + pid5k[0].sumTx;
                     });
        }

        // 21/22/23. Rate cells, MMDB widen, unicast filter (paint +
        // country/asn enrichment path).
        {
            wchar_t cell[64] = {0};
            AddBench(benches, "FormatBpsCell", "bpscell_hot",
                     "1.5 KB/s down, 2.9 MB/s up",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FormatBpsCell(cell, 64, 1500.0,
                                                 3000000.0, true);
                     });
            AddBench(benches, "FormatBpsCell", "bpscell_idle",
                     "idle sentinel",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FormatBpsCell(cell, 64, 0.1, 0.2,
                                                 true);
                     });
            const std::string org = "Cloudflare, Inc. - network services";
            const std::string iso = "US";
            const unsigned char mixed[] = {
                'e', 'x', 0xC3, 0xA9, 'm', 'p', 'l', 'e', 0xE3, 0x81,
                0x82, '.', 'c', 'o', 'm'};
            AddBench(benches, "WidenUtf8", "widen_org_34",
                     "34-char ASCII org name",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::wstring r = WidenUtf8(
                             reinterpret_cast<const unsigned char*>(
                                 org.data()),
                             org.size());
                         g_sink += r.size() +
                                       static_cast<unsigned>(r[0]);
                     });
            AddBench(benches, "WidenUtf8", "widen_iso",
                     "2-byte country code",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::wstring r = WidenUtf8(
                             reinterpret_cast<const unsigned char*>(
                                 iso.data()),
                             iso.size());
                         g_sink += r.size();
                     });
            AddBench(benches, "WidenUtf8", "widen_mixed_15",
                     "15 bytes with 2+3-byte sequences",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::wstring r = WidenUtf8(mixed, 15);
                         g_sink += r.size();
                     });
            uint32_t v4pool[64];
            uint64_t rng = 809;
            for (size_t i = 0; i < 64; ++i) {
                v4pool[i] = static_cast<uint32_t>(NextRand(rng));
            }
            v4pool[0] = 0x08080808u;
            v4pool[1] = 0x0A000001u;
            v4pool[2] = 0xC0A80101u;
            size_t v4Idx = 0;
            AddBench(benches, "IsGlobalV4", "unicast_v4_mixed",
                     "64-address pool (public/private/special)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += IsGlobalV4(v4pool[v4Idx++ & 63]) ? 1u
                                                                    : 0u;
                     });
            unsigned char v6pool[16][16] = {0};
            for (size_t i = 0; i < 16; ++i) {
                for (size_t b = 0; b < 16; ++b) {
                    v6pool[i][b] = static_cast<unsigned char>(
                        NextRand(rng) & 0xFF);
                }
            }
            // Pin the shapes that matter: loopback, public, ULA.
            std::memset(v6pool[0], 0, 16);
            v6pool[0][15] = 1;
            v6pool[1][0] = 0x26;
            v6pool[1][1] = 0x06;
            v6pool[2][0] = 0xFC;
            size_t v6Idx = 0;
            AddBench(benches, "IsGlobalV6", "unicast_v6_mixed",
                     "16-address pool",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += IsGlobalV6(v6pool[v6Idx++ & 15]) ? 1u
                                                                     : 0u;
                     });
        }

        // 24/25. Snapshot pairing + counted-key find (refresh pairing +
        // MMDB record decode path).
        {
            auto makeKeys = [](size_t n, size_t nDistinct,
                               uint64_t seed) {
                std::vector<SnapKey> keys(n);
                uint64_t rng = seed;
                for (size_t i = 0; i < n; ++i) {
                    const uint64_t k = NextRand(rng) % nDistinct;
                    char tmp[32] = {0};
                    std::snprintf(tmp, sizeof(tmp), "4Tkey-%05llu",
                                  static_cast<unsigned long long>(k));
                    keys[i] = MakeSnapKey(tmp);
                }
                return keys;
            };
            std::vector<SnapKey> prev500 = makeKeys(500, 400, 810);
            std::vector<SnapKey> fresh500 = makeKeys(500, 400, 811);
            std::vector<SnapKey> prev5k = makeKeys(5000, 4500, 812);
            std::vector<SnapKey> fresh5k = makeKeys(5000, 4500, 813);
            std::vector<SnapKey> prevMdns(500, MakeSnapKey("mdns-key"));
            std::vector<SnapKey> freshMdns(400, MakeSnapKey("mdns-key"));
            std::vector<int> out5k(5000, -7);
            AddBench(benches, "PairSnapshot", "pair_500",
                     "500x500 rows, 400 distinct keys",
                     rounds, timeMs, warmupMs,
                     [&] {
                         std::vector<int> out(500, -7);
                         g_sink += static_cast<uint64_t>(
                             PairSnapshot(prev500.data(), 500,
                                          fresh500.data(), 500,
                                          out.data()));
                     });
            AddBench(benches, "PairSnapshot", "pair_5k",
                     "5000x5000 rows, 4500 distinct keys",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += static_cast<uint64_t>(
                             PairSnapshot(prev5k.data(), 5000,
                                          fresh5k.data(), 5000,
                                          out5k.data()));
                     });
            AddBench(benches, "PairSnapshot", "pair_mdns",
                     "500x400 rows, single key (mDNS storm)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         std::vector<int> out(400, -7);
                         g_sink += static_cast<uint64_t>(
                             PairSnapshot(prevMdns.data(), 500,
                                          freshMdns.data(), 400,
                                          out.data()));
                     });

            const char* kShort[] = {"en", "id", "de", "fr",
                                    "iso_code", "names", "country"};
            const char* kLong[] = {
                "autonomous_system_number",
                "autonomous_system_organization",
                "database_type",
                "record_size"};
            std::vector<std::string> shortStore;
            std::vector<CountedKey> shortKeys;
            for (const char* s : kShort) {
                shortStore.emplace_back(s);
            }
            for (const std::string& s : shortStore) {
                shortKeys.push_back(
                    {reinterpret_cast<const unsigned char*>(s.data()),
                     s.size()});
            }
            std::vector<std::string> longStore;
            std::vector<CountedKey> longKeys;
            for (const char* s : kLong) {
                longStore.emplace_back(s);
            }
            for (const std::string& s : longStore) {
                longKeys.push_back(
                    {reinterpret_cast<const unsigned char*>(s.data()),
                     s.size()});
            }
            const unsigned char wantIso[] = {'i', 's', 'o', '_', 'c',
                                             'o', 'd', 'e'};
            const unsigned char wantAsn[] = {
                'a', 'u', 't', 'o', 'n', 'o', 'm', 'o', 'u', 's', '_',
                's', 'y', 's', 't', 'e', 'm', '_', 'n', 'u', 'm', 'b',
                'e', 'r'};
            const unsigned char wantMiss[] = {'n', 'o', 't', '_', 'h',
                                              'e', 'r', 'e', '!'};
            AddBench(benches, "FindCountedKey", "findkey_short_hit",
                     "7 short keys, hit iso_code",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += static_cast<uint64_t>(
                             FindCountedKey(shortKeys.data(),
                                            shortKeys.size(), wantIso,
                                            8) +
                             1);
                     });
            AddBench(benches, "FindCountedKey", "findkey_short_miss",
                     "7 short keys, miss",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += static_cast<uint64_t>(
                             FindCountedKey(shortKeys.data(),
                                            shortKeys.size(),
                                            wantMiss, 9) +
                             1);
                     });
            AddBench(benches, "FindCountedKey", "findkey_long_hit",
                     "4 long keys, hit 24-char ASN key",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += static_cast<uint64_t>(
                             FindCountedKey(longKeys.data(),
                                            longKeys.size(), wantAsn,
                                            24) +
                             1);
                     });
            // 64-entry map: same-length keys force full compares.
            std::vector<std::string> manyStore;
            std::vector<CountedKey> manyKeys;
            for (int i = 0; i < 64; ++i) {
                char tmp[32] = {0};
                std::snprintf(tmp, sizeof(tmp), "map-key-%02d-value",
                              i);
                manyStore.emplace_back(tmp);
            }
            for (const std::string& s : manyStore) {
                manyKeys.push_back(
                    {reinterpret_cast<const unsigned char*>(s.data()),
                     s.size()});
            }
            const unsigned char wantMany[] = {
                'm', 'a', 'p', '-', 'k', 'e', 'y', '-', '6', '3', '-',
                'v', 'a', 'l', 'u', 'e'};
            const unsigned char wantManyMiss[] = {
                'm', 'a', 'p', '-', 'k', 'e', 'y', '-', 'X', 'X', '-',
                'v', 'a', 'l', 'u', 'e'};
            AddBench(benches, "FindCountedKey", "findkey_64_hit",
                     "64 same-length keys, hit last",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += static_cast<uint64_t>(
                             FindCountedKey(manyKeys.data(),
                                            manyKeys.size(), wantMany,
                                            16) +
                             1);
                     });
            AddBench(benches, "FindCountedKey", "findkey_64_miss",
                     "64 same-length keys, miss",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += static_cast<uint64_t>(
                             FindCountedKey(manyKeys.data(),
                                            manyKeys.size(),
                                            wantManyMiss, 16) +
                             1);
                     });
        }

        // 26. Flow-key equality (reassembly stream filter path).
        {
            FlowKey want;
            std::memset(&want, 0, sizeof(want));
            want.addrA[0] = 10;
            want.addrA[3] = 5;
            want.addrB[0] = 93;
            want.addrB[1] = 184;
            want.addrB[2] = 216;
            want.addrB[3] = 34;
            want.portA = 5000;
            want.portB = 443;
            FlowKey other = want;
            other.addrB[15] = 0xFF;
            AddBench(benches, "FlowKeyEqual", "flowkey_hit",
                     "equal keys (full 32B compare)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FlowKeyEqual(want, want) ? 1u : 0u;
                     });
            AddBench(benches, "FlowKeyEqual", "flowkey_miss",
                     "differ at last byte",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FlowKeyEqual(want, other) ? 1u : 0u;
                     });
            unsigned char ca[16] = {0};
            unsigned char cb[16] = {0};
            cb[15] = 1;
            AddBench(benches, "CmpFlowAddr", "cmpaddr_late",
                     "16B addresses differ at byte 15",
                     rounds, timeMs, warmupMs,
                     [&] { g_sink += static_cast<uint64_t>(
                                       CmpFlowAddr(ca, cb) + 1); });
        }

        // 27/28. Handle-table filter + ETW classifier (sampler path).
        {
            const size_t kHandles = 100000;
            std::vector<HandleEntry> entries(kHandles);
            uint64_t rng = 812;
            for (size_t i = 0; i < kHandles; ++i) {
                entries[i].srcPid = NextRand(rng) % 2000;
                entries[i].typeIndex =
                    static_cast<uint32_t>(NextRand(rng) % 16);
                entries[i].handle = 0x10 + (i % 4096) * 4;
            }
            // 8 wanted PIDs (sorted), 3 socket types (sorted), 64
            // stalled pairs (sorted by pid, then handle).
            std::vector<uint32_t> wantPids;
            for (uint32_t p = 100; p < 900; p += 100) {
                wantPids.push_back(p);
            }
            const uint32_t wantTypes[3] = {3, 7, 11};
            std::vector<PidHandle> skip;
            for (uint64_t i = 0; i < 64; ++i) {
                skip.push_back(
                    {static_cast<uint32_t>(100 + (i % 8) * 100),
                     0x10 + (i % 4096) * 4});
            }
            std::sort(skip.begin(), skip.end(),
                      [](const PidHandle& a, const PidHandle& b) {
                          return (a.pid != b.pid) ? (a.pid < b.pid)
                                                  : (a.handle < b.handle);
                      });
            std::vector<size_t> outIdx(kHandles, 0);
            AddBench(benches, "FilterHandles", "handles_100k",
                     "100k entries, 8 PIDs, 3 types, 64 stalled",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FilterHandles(
                             entries.data(), entries.size(),
                             wantPids.data(), wantPids.size(),
                             wantTypes, 3, true, skip.data(),
                             skip.size(), outIdx.data(),
                             outIdx.size());
                     });

            const unsigned char tcp[16] = {
                0xC0, 0x0A, 0x28, 0x9A, 0xE0, 0xC8, 0xD1, 0x11,
                0x84, 0xE2, 0x00, 0xC0, 0x4F, 0xB9, 0x98, 0xA2};
            const unsigned char udp[16] = {
                0xC5, 0x50, 0x3A, 0xBF, 0xC9, 0xA9, 0x88, 0x49,
                0xA0, 0x05, 0x2D, 0xF0, 0xB7, 0xC8, 0x0F, 0x80};
            const unsigned char foreign[16] = {0xDE, 0xAD, 0xBE, 0xEF};
            struct Evt {
                const unsigned char* guid;
                uint16_t id;
                uint16_t opcode;
            };
            const Evt evts[5] = {{tcp, 0, 10},
                                 {tcp, 0, 11},
                                 {udp, 0, 26},
                                 {foreign, 0, 10},
                                 {tcp, 0, 18}};
            size_t evIdx = 0;
            AddBench(benches, "ClassifyEvent", "etw_mixed",
                     "tcp/udp/foreign/op18 cycle",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const Evt& e = evts[evIdx++ % 5];
                         g_sink += static_cast<unsigned>(
                             ClassifyEvent(e.guid, e.id, e.opcode));
                     });
            unsigned char payload[8] = {0x2A, 0x00, 0x00, 0x00,
                                        0x40, 0x06, 0x00, 0x00};
            AddBench(benches, "ParseEventPayload", "etw_payload",
                     "8-byte PID+size payload",
                     rounds, timeMs, warmupMs,
                     [&] {
                         uint32_t pid = 0;
                         uint32_t size = 0;
                         if (ParseEventPayload(payload, 8, &pid,
                                               &size)) {
                             g_sink += pid + size;
                         }
                     });
        }

        // 29/30. JSON + CSV escaping (export path).
        {
            const std::wstring proc = L"chrome.exe";
            const std::wstring row =
                L"path: \"C:\\Temp\\caf\xE9.txt\"\nline\x01";
            std::wstring cell64(64, L'\0');
            for (size_t i = 0; i < cell64.size(); ++i) {
                cell64[i] = static_cast<wchar_t>(
                    (i % 16 == 7) ? L'"' : (L'a' + (i % 26)));
            }
            auto foldJson = [](const std::string& r) {
                g_sink += r.size() +
                              (r.empty()
                                   ? 0u
                                   : static_cast<unsigned char>(r[0]));
            };
            AddBench(benches, "JsonEscape", "json_proc",
                     "10-char process name",
                     rounds, timeMs, warmupMs,
                     [&] { foldJson(JsonEscape(proc)); });
            AddBench(benches, "JsonEscape", "json_row",
                     "export row with quotes+unicode+control",
                     rounds, timeMs, warmupMs,
                     [&] { foldJson(JsonEscape(row)); });
            AddBench(benches, "JsonEscape", "json_64_quotes",
                     "64 chars with quotes every 16",
                     rounds, timeMs, warmupMs,
                     [&] { foldJson(JsonEscape(cell64)); });
            const std::string plain(64, 'x');
            const std::string quoted =
                "note: \"vendor api\", codename\r\nshipped";
            AddBench(benches, "CsvEscape", "csv_plain_64",
                     "64 chars, no quoting",
                     rounds, timeMs, warmupMs,
                     [&] { foldJson(CsvEscape(plain)); });
            AddBench(benches, "CsvEscape", "csv_quoted",
                     "commas + quotes + CRLF",
                     rounds, timeMs, warmupMs,
                     [&] { foldJson(CsvEscape(quoted)); });
        }

        // 31/32. Small-integer + IP formatting (cell/export path).
        {
            wchar_t wbuf[32] = {0};
            char nbuf[32] = {0};
            AddBench(benches, "FormatPort", "port_443",
                     "3-digit port",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FormatPort(wbuf, 32, 443);
                     });
            AddBench(benches, "FormatPort", "port_65535",
                     "5-digit port",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FormatPort(wbuf, 32, 65535);
                     });
            AddBench(benches, "FormatU64Dec", "u64_20digit",
                     "20-digit counter",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FormatU64Dec(
                             nbuf, 32, 18446744073709551615ull);
                     });
            AddBench(benches, "FormatDuration", "dur_hms",
                     "1h 1m shape",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += FormatDuration(wbuf, 32, 3661);
                     });
            const unsigned char v4[4] = {192, 168, 1, 1};
            const unsigned char v6full[16] = {
                0x20, 0x01, 0x0D, 0xB8, 0x12, 0x34, 0x56, 0x78,
                0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56, 0x78};
            const unsigned char v6zip[16] = {
                0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            AddBench(benches, "FormatIpv4", "ipv4",
                     "dotted quad",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::wstring r = FormatIpv4(v4);
                         g_sink += r.size();
                     });
            AddBench(benches, "FormatIpv6", "ipv6_full",
                     "8 full groups",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::wstring r = FormatIpv6(v6full);
                         g_sink += r.size();
                     });
            AddBench(benches, "FormatIpv6", "ipv6_zip",
                     "compressed fe80::1",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::wstring r = FormatIpv6(v6zip);
                         g_sink += r.size();
                     });
        }

        // 33. Display width (table render path).
        {
            // The realistic cell: ASCII with a couple of CJK/emoji, so
            // the bulk path and the per-codepoint path both run.
            std::string cell =
                "chrome.exe:1234 -> 203.0.113.9:443 established";
            std::string wide =
                "chrome.exe  ";
            wide += "\xE4\xB8\xAD\xE6\x96\x87\xE5\x90\x8D";  // 3 CJK
            wide += "\xF0\x9F\x98\x80";                       // 1 emoji
            wide += "  established";
            std::string longAscii(256, 'a');
            std::string mixedLong;
            for (int i = 0; i < 64; ++i) mixedLong += "abcd\xE4\xB8\xAD";

            // CpWidth alone: a cycle of boundaries plus a random sweep,
            // since that is where the range chain costs the most.
            static const uint32_t kCpCycle[] = {
                0x0041, 0x00E9, 0x0301, 0x1100, 0x2E80, 0x3042,
                0x4E2D, 0xAC00, 0xF900, 0xFF01, 0x1F600, 0x20000,
            };
            size_t cpIdx = 0;
            uint64_t cpRng = 12345;
            AddBench(benches, "CpWidth", "cp_boundaries",
                     "12 boundary code points in a cycle",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += CpWidth(kCpCycle[cpIdx++ % 12]);
                     });
            AddBench(benches, "CpWidth", "cp_random_sweep",
                     "random code points across all ranges",
                     rounds, timeMs, warmupMs,
                     [&] {
                         cpRng = cpRng * 6364136223846793005ull +
                                 1442695040888963407ull;
                         g_sink += CpWidth(
                             static_cast<uint32_t>(cpRng >> 33) % 0x40000);
                     });
            AddBench(benches, "DisplayWidth", "width_ascii_47",
                     "47-char process cell",
                     rounds, timeMs, warmupMs,
                     [&] { g_sink += DisplayWidth(cell); });
            AddBench(benches, "DisplayWidth", "width_mixed_30",
                     "30 columns with CJK + emoji",
                     rounds, timeMs, warmupMs,
                     [&] { g_sink += DisplayWidth(wide); });
            AddBench(benches, "DisplayWidth", "width_ascii_256",
                     "256-char ASCII path cell",
                     rounds, timeMs, warmupMs,
                     [&] { g_sink += DisplayWidth(longAscii); });
            AddBench(benches, "DisplayWidth", "width_mixed_320",
                     "320 chars, CJK every 4th byte",
                     rounds, timeMs, warmupMs,
                     [&] { g_sink += DisplayWidth(mixedLong); });
            AddBench(benches, "TruncateToWidth", "trunc_fits",
                     "cell already narrow enough",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::string r = TruncateToWidth(cell, 47);
                         g_sink += r.size();
                     });
            AddBench(benches, "TruncateToWidth", "trunc_cut",
                     "cell cut to 24 columns",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::string r = TruncateToWidth(cell, 24);
                         g_sink += r.size();
                     });
            AddBench(benches, "TruncateToWidth", "trunc_wide_cut",
                     "wide cell cut to 24 columns",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::string r = TruncateToWidth(wide, 24);
                         g_sink += r.size();
                     });
        }

        // 34. MMDB payload size + pointer read (Decode / SkipValue walk).
        {
            // A metadata-shaped run: 4096 control bytes cycling through
            // every size code, so the table and the branch chain both see
            // the full mix rather than one hot value.
            const size_t kSec = 4096;
            std::vector<unsigned char> sec(kSec + 32);
            for (size_t i = 0; i < sec.size(); ++i) {
                sec[i] = static_cast<unsigned char>((i * 7) & 0xFF);
            }
            size_t posIdx = 0;
            AddBench(benches, "PayloadSize", "size_code_mix",
                     "control bytes cycling all 32 size codes",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const size_t pos = (posIdx++ % (kSec - 8)) + 1;
                         MmdbPayload p;
                         if (PayloadSize(sec.data(), kSec, sec[pos], pos,
                                         &p)) {
                             g_sink += p.size + p.pos;
                         }
                     });
            AddBench(benches, "PayloadSize", "size_inline_only",
                     "size codes 0..28 (no extra bytes)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const unsigned char ctrl =
                             static_cast<unsigned char>((posIdx++ % 29));
                         MmdbPayload p;
                         if (PayloadSize(sec.data(), kSec, ctrl, 32, &p)) {
                             g_sink += p.size;
                         }
                     });
            AddBench(benches, "ReadPointer", "ptr_size_mix",
                     "pointer sizes 0..3 in a cycle",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const unsigned psz =
                             static_cast<unsigned>((posIdx++ % 4));
                         const unsigned char ctrl =
                             static_cast<unsigned char>((psz << 3) | 0x05u);
                         size_t out = 0;
                         if (ReadPointer(sec.data(), kSec, ctrl, 8, &out)) {
                             g_sink += out;
                         }
                     });
        }

        // 12/13. TLS extension dispatch + endian loads (ClientHello walk
        // and record-header parse path).
        {
            const uint16_t types[] = {0x0000, 0x0010, 0x0001, 0x002B,
                                      0x00FF, 0x0000, 0x0005, 0x0010};
            size_t tyIdx = 0;
            AddBench(benches, "TlsExt", "ext_classify",
                     "extension-type dispatch (8-type cycle)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         g_sink += static_cast<unsigned>(
                             ClassifyTlsExtension(types[tyIdx++ & 7]));
                     });
            char alldot[64];
            std::memset(alldot, '.', sizeof(alldot));
            char host[64] = "files.example-cdn.net";
            const size_t hostLen = std::strlen(host);
            AddBench(benches, "TlsExt", "sni_strip_all",
                     "64-byte all-strip name",
                     rounds, timeMs, warmupMs,
                     [&] { g_sink += StripSniTail(alldot, 64); });
            AddBench(benches, "TlsExt", "sni_strip_tail",
                     "21-char host + dot tail",
                     rounds, timeMs, warmupMs,
                     [&] {
                         char tmp[64];
                         std::memcpy(tmp, host, hostLen);
                         std::memset(tmp + hostLen, '.', 8);
                         g_sink += StripSniTail(tmp, hostLen + 8);
                     });
            unsigned char pkt[64];
            uint64_t rng = 804;
            for (size_t i = 0; i < sizeof(pkt); ++i) {
                pkt[i] = static_cast<unsigned char>(NextRand(rng));
            }
            size_t pkIdx = 0;
            AddBench(benches, "Rd", "rd16_mixed",
                     "u16 LE/BE alternating at cycling offsets",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const size_t o = pkIdx++ & 7;
                         g_sink += (o & 1) ? Rd16(pkt + o, true)
                                           : Rd16(pkt + o, false);
                     });
            AddBench(benches, "Rd", "rd32_mixed",
                     "u32 LE/BE alternating at cycling offsets",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const size_t o = pkIdx++ & 7;
                         g_sink += (o & 1) ? Rd32(pkt + o, true)
                                           : Rd32(pkt + o, false);
                     });
        }

        // 14. Socket-sample join (refresh-tick sampler join).
        {
            auto makeStore = [](size_t nRows, size_t nSamples,
                                uint64_t seed, size_t nKeys) {
                std::vector<JoinRow> rows;
                std::vector<JoinSample> ss;
                rows.reserve(nRows);
                ss.reserve(nSamples);
                uint64_t rng = seed;
                for (size_t i = 0; i < nRows; ++i) {
                    const uint64_t k =
                        (nKeys == 0) ? i : (NextRand(rng) % nKeys);
                    wchar_t la[32] = {0};
                    wchar_t ra[32] = {0};
                    ::swprintf_s(
                        la, 32, L"10.0.%u.%u",
                        static_cast<unsigned>((k >> 8) & 0xFF),
                        static_cast<unsigned>(k & 0xFF));
                    ::swprintf_s(
                        ra, 32, L"93.184.%u.%u",
                        static_cast<unsigned>((k >> 8) & 0xFF),
                        static_cast<unsigned>(k & 0xFF));
                    JoinRow r;
                    r.localPort =
                        static_cast<uint32_t>(1024 + (k % 50000));
                    r.remotePort = 443;
                    r.local = la;
                    r.remote = ra;
                    rows.push_back(r);
                }
                for (size_t i = 0; i < nSamples; ++i) {
                    const uint64_t k =
                        (nKeys == 0)
                            ? (NextRand(rng) % nRows)
                            : (NextRand(rng) % nKeys);
                    wchar_t la[32] = {0};
                    wchar_t ra[32] = {0};
                    ::swprintf_s(
                        la, 32, L"10.0.%u.%u",
                        static_cast<unsigned>((k >> 8) & 0xFF),
                        static_cast<unsigned>(k & 0xFF));
                    ::swprintf_s(
                        ra, 32, L"93.184.%u.%u",
                        static_cast<unsigned>((k >> 8) & 0xFF),
                        static_cast<unsigned>(k & 0xFF));
                    JoinSample s;
                    s.localPort =
                        static_cast<uint32_t>(1024 + (k % 50000));
                    s.remotePort = 443;
                    s.local = la;
                    s.remote = ra;
                    s.rx = NextRand(rng);
                    s.tx = NextRand(rng);
                    ss.push_back(s);
                }
                return std::make_pair(std::move(rows), std::move(ss));
            };
            struct Case {
                const char* workload;
                const char* desc;
                size_t nRows;
                size_t nSamples;
                uint64_t seed;
                size_t nKeys;  // 0 = unique-ish keys
            };
            const Case cases[] = {
                {"join_small", "16 rows x 8 samples", 16, 8, 901, 0},
                {"join_500_300", "500 rows x 300 samples (typical tick)",
                 500, 300, 902, 0},
                {"join_2k_2k", "2000 rows x 2000 samples (busy tick)",
                 2000, 2000, 903, 0},
                {"join_mdns", "400 rows x 400 samples over 20 keys",
                 400, 400, 904, 20},
            };
            for (const Case& c : cases) {
                auto store =
                    makeStore(c.nRows, c.nSamples, c.seed, c.nKeys);
                AddBench(benches, "JoinSamples", c.workload, c.desc,
                         rounds, timeMs, warmupMs,
                         [store]() mutable {
                             // Fresh row state per call: reset the
                             // written fields so every timed call does
                             // the full match + write work.
                             for (JoinRow& r : store.first) {
                                 r.rx = 0;
                                 r.tx = 0;
                                 r.perRow = false;
                             }
                             g_sink += static_cast<uint64_t>(
                                 JoinSamples(store.first, store.second));
                         });
            }
        }

        // 9. Stream hexdump (follow-stream view path).
        {
            std::string seg1460(1460, '\0');
            std::string seg64k(65536, '\0');
            std::string seg40(40, '\0');
            uint64_t rng = 801;
            for (size_t i = 0; i < seg1460.size(); ++i) {
                seg1460[i] =
                    static_cast<char>(NextRand(rng) & 0xFF);
            }
            for (size_t i = 0; i < seg64k.size(); ++i) {
                seg64k[i] = static_cast<char>(NextRand(rng) & 0xFF);
            }
            for (size_t i = 0; i < seg40.size(); ++i) {
                seg40[i] = static_cast<char>(NextRand(rng) & 0xFF);
            }
            AddBench(benches, "FormatStreamHex", "hex_1460_16",
                     "1460-byte segment, 16/line (typical MSS)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::string r =
                             FormatStreamHex(seg1460, 16);
                         g_sink += r.size() +
                                       static_cast<unsigned char>(r[0]);
                     });
            AddBench(benches, "FormatStreamHex", "hex_64k_64",
                     "64KB direction, 64/line (max width)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::string r =
                             FormatStreamHex(seg64k, 64);
                         g_sink += r.size() +
                                       static_cast<unsigned char>(r[0]);
                     });
            AddBench(benches, "FormatStreamHex", "hex_40_16",
                     "40-byte SYN-sized segment, 16/line",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const std::string r =
                             FormatStreamHex(seg40, 16);
                         g_sink += r.size() +
                                       static_cast<unsigned char>(r[0]);
                     });
        }

        // 10. Wide lowercase (per-row FinalizeRow + filter path).
        {
            const std::wstring proc = L"Chrome.EXE";
            std::wstring path(260, L'\0');
            uint64_t rng = 802;
            for (size_t i = 0; i < path.size(); ++i) {
                const unsigned c =
                    static_cast<unsigned>(NextRand(rng));
                path[i] = static_cast<wchar_t>(
                    (c % 2) ? L'a' + (c % 26) : L'A' + (c % 26));
            }
            std::wstring ascii4k(4096, L'\0');
            for (size_t i = 0; i < ascii4k.size(); ++i) {
                ascii4k[i] = static_cast<wchar_t>(
                    L'A' + (NextRand(rng) % 26));
            }
            std::wstring mixed512(512, L'\0');
            for (size_t i = 0; i < mixed512.size(); ++i) {
                mixed512[i] = (i % 32 == 31)
                                  ? L'\x0080'
                                  : static_cast<wchar_t>(
                                        L'A' + (NextRand(rng) % 26));
            }
            auto foldLower = [](const std::wstring& r) {
                g_sink += r.size() + static_cast<unsigned>(r[0]);
            };
            AddBench(benches, "ToLowerW", "lower_proc",
                     "10-char process name",
                     rounds, timeMs, warmupMs,
                     [&] { foldLower(ToLowerW(proc)); });
            AddBench(benches, "ToLowerW", "lower_path",
                     "260-char mixed-case path",
                     rounds, timeMs, warmupMs,
                     [&] { foldLower(ToLowerW(path)); });
            AddBench(benches, "ToLowerW", "lower_4k",
                     "4K ASCII string",
                     rounds, timeMs, warmupMs,
                     [&] { foldLower(ToLowerW(ascii4k)); });
            AddBench(benches, "ToLowerW", "lower_mixed",
                     "512 chars with non-ASCII every 32 (fallback)",
                     rounds, timeMs, warmupMs,
                     [&] { foldLower(ToLowerW(mixed512)); });
        }

        // 8. Combined per-packet hot path (the common UI loop:
        // parse -> key -> format -> filter).
        {
            struct Pkt {
                std::vector<unsigned char> raw;
                ConnKeyInput conn;
                std::wstring process;
            };
            std::vector<Pkt> pkts;
            uint64_t rng = 950;
            const wchar_t* kNames[] = {L"chrome.exe", L"firefox.exe",
                                         L"teams.exe", L"explorer.exe",
                                         L"wintcp.exe"};
            for (int i = 0; i < 200; ++i) {
                Pkt p;
                const size_t tcpHdr =
                    20 + ((NextRand(rng) % 4) * 4);
                const size_t payloadLen =
                    40 + (NextRand(rng) % 1460);
                p.raw.assign(14 + 20 + tcpHdr + payloadLen, 0);
                // Ethernet.
                p.raw[12] = 0x08;
                p.raw[13] = 0x00;
                // IPv4 at offset 14: version 4, IHL 5.
                p.raw[14] = 0x45;
                // Protocol TCP at offset 23.
                p.raw[14 + 9] = 6;
                // TCP at offset 34.
                const size_t tcpOff = 34;
                p.raw[tcpOff + 12] =
                    static_cast<unsigned char>(
                        ((tcpHdr / 4) << 4) & 0xF0);
                p.raw[tcpOff + 13] = 0x18;
                for (size_t b = 0; b < p.raw.size(); ++b) {
                    p.raw[b] ^= static_cast<unsigned char>(
                        NextRand(rng) & 0xFF);
                }
                p.raw[14] = 0x45;
                p.raw[14 + 9] = 6;
                p.raw[tcpOff + 12] =
                    static_cast<unsigned char>(
                        ((tcpHdr / 4) << 4) & 0xF0);
                p.conn.ipv6 = false;
                p.conn.localAddr = p.raw.data() + 14 + 12;
                p.conn.remoteAddr = p.raw.data() + 14 + 16;
                p.conn.localPort =
                    static_cast<uint32_t>(NextRand(rng) & 0xFFFF);
                p.conn.remotePort =
                    static_cast<uint32_t>(NextRand(rng) & 0xFFFF);
                p.conn.pid = static_cast<uint32_t>(NextRand(rng));
                p.process = kNames[NextRand(rng) % 5];
                pkts.push_back(std::move(p));
            }
            const std::wstring filter = L"chrome";
            unsigned char keyBuf[64] = {0};
            size_t keyLen = 0;
            wchar_t fmtBuf[32] = {0};
            size_t pktIdx = 0;
            AddBench(benches, "Pipeline", "packet_hotpath",
                     "per-packet loop: parse + key + format + "
                     "filter (200 packets)",
                     rounds, timeMs, warmupMs,
                     [&] {
                         const Pkt& p = pkts[pktIdx & 199];
                         TcpHeaderFields f;
                         const bool ok = ParseTcpHeader(
                             p.raw.data(), 34, p.raw.size(), &f);
                         KeyOf(&p.conn, keyBuf, &keyLen);
                         const size_t fn = FormatBytes(
                             fmtBuf, 32,
                             ok ? f.payloadLen : 0);
                         const bool hit =
                             HasLowerSubstring(p.process, filter);
                         g_sink += f.srcPort + f.dstPort + f.seq +
                                       keyLen + fn +
                                       (hit ? 1u : 0u) +
                                       (ok ? 1u : 0u);
                         ++pktIdx;
                     });
        }
    }

    if (wantJson) {
        EmitJson(exeName, variant, bi, rounds, timeMs, warmupMs,
                   tests, benches);
    } else {
        EmitHuman(exeName, variant, bi, rounds, timeMs, tests,
                    benches);
    }
    return 0;
}
