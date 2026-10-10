// testAssemblies.cpp
// SPDX-License-Identifier: Apache-2.0
// Test harness for assembly-optimized functions.
//
// This file creates a standalone test executable that:
// 1. Verifies correctness of each optimized function
// 2. Benchmarks performance
// 3. Reports results in a human-readable format
//
// Build: cl /std:c++17 /EHsc /O2 opt_functions.cpp testAssemblies.cpp /Fe:testAssemblies.exe
// Or:   build.bat

#include "opt_functions.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace wintcp;

// ============================================================================
// Test utilities
// ============================================================================

static int testsPassed = 0;
static int testsFailed = 0;

void ReportTest(const char* name, bool passed, const std::string& detail = "") {
    if (passed) {
        printf("PASS  %s", name);
        ++testsPassed;
    } else {
        printf("FAIL  %s", name);
        ++testsFailed;
    }
    if (!detail.empty()) {
        printf("  [%s]", detail.c_str());
    }
    printf("\n");
}

using Clock = std::chrono::high_resolution_clock;
using Nanoseconds = std::chrono::nanoseconds;

// ============================================================================
// Test 1: GeoIP Tree Walk (ResolveOffset)
// ============================================================================

void TestGeoIpTreeWalk() {
    printf("\n=== GeoIP Tree Walk (ResolveOffset) ===\n");

    // Build a minimal test tree structure for 24-bit records
    // Node format: 6 bytes per node (two 3-byte records, big-endian)
    constexpr size_t kNodeCount = 100;
    constexpr size_t kNodeBytes = 6;  // 24-bit records
    constexpr size_t kSeparatorLen = 16;
    constexpr size_t kTreeSize = kNodeCount * kNodeBytes;
    constexpr size_t kDataSize = 500;
    constexpr size_t kTotalSize = kTreeSize + kSeparatorLen + kDataSize;

    std::vector<unsigned char> buffer(kTotalSize, 0);

    // Build a chain: node i has left = node (i+1), right = data offset
    for (size_t i = 0; i < kNodeCount - 1; ++i) {
        size_t offset = i * kNodeBytes;
        // Left record = node i+1 (big-endian 24-bit)
        buffer[offset] = static_cast<unsigned char>((i + 1) >> 16);
        buffer[offset + 1] = static_cast<unsigned char>((i + 1) >> 8);
        buffer[offset + 2] = static_cast<unsigned char>(i + 1);
        // Right record = data pointer (nodeCount + separator + offset)
        size_t dataPtr = kNodeCount + kSeparatorLen + (i % 10);
        buffer[offset + 3] = static_cast<unsigned char>(dataPtr >> 16);
        buffer[offset + 4] = static_cast<unsigned char>(dataPtr >> 8);
        buffer[offset + 5] = static_cast<unsigned char>(dataPtr);
    }

    // Last node: both records point to data
    size_t lastOff = (kNodeCount - 1) * kNodeBytes;
    size_t dataPtr = kNodeCount + kSeparatorLen;
    for (int h = 0; h < 2; ++h) {
        buffer[lastOff + h * 3] = static_cast<unsigned char>(dataPtr >> 16);
        buffer[lastOff + h * 3 + 1] = static_cast<unsigned char>(dataPtr >> 8);
        buffer[lastOff + h * 3 + 2] = static_cast<unsigned char>(dataPtr);
    }

    // Address 8.8.8.8 in network byte order: 0x08080808
    unsigned char addrBits[16] = {0x08, 0x08, 0x08, 0x08, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 0};

    // Test: walk with zero address (follows all left branches).
    // The chain below is 99 nodes deep but the walk is only 32 bits,
    // so it must end on an internal node with no data (returns false).
    unsigned char zeroAddr[16] = {0};
    size_t zeroOffset = 0;
    bool zeroResult = ResolveOffsetOpt(zeroAddr, 32, 0, kNodeCount, kNodeBytes,
                                       false, 3, buffer.data(), kTotalSize,
                                       kSeparatorLen, &zeroOffset);
    ReportTest("GeoIp: zero address walk exhausts bits (no data)",
               !zeroResult,
               std::string("result=") + (zeroResult ? "true" : "false") +
               " offset=" + std::to_string(zeroOffset));

    // Test: null bits
    {
        size_t off = 0;
        bool r = ResolveOffsetOpt(nullptr, 32, 0, kNodeCount, kNodeBytes,
                                  false, 3, buffer.data(), kTotalSize,
                                  kSeparatorLen, &off);
        ReportTest("GeoIp: null bits rejected", !r);
    }

    // Test: zero bitCount
    {
        size_t off = 0;
        bool r = ResolveOffsetOpt(addrBits, 0, 0, kNodeCount, kNodeBytes,
                                  false, 3, buffer.data(), kTotalSize,
                                  kSeparatorLen, &off);
        ReportTest("GeoIp: zero bitCount returns false", !r);
    }

    // Test: 32-bit record walk
    {
        constexpr size_t kNodeBytes32 = 8;
        constexpr size_t kTreeSize32 = kNodeCount * kNodeBytes32;
        constexpr size_t kTotalSize32 = kTreeSize32 + kSeparatorLen + kDataSize;
        std::vector<unsigned char> buf32(kTotalSize32, 0);

        for (size_t i = 0; i < kNodeCount - 1; ++i) {
            size_t offset = i * kNodeBytes32;
            // Left = node i+1 (big-endian 32-bit)
            buf32[offset] = static_cast<unsigned char>((i + 1) >> 24);
            buf32[offset + 1] = static_cast<unsigned char>((i + 1) >> 16);
            buf32[offset + 2] = static_cast<unsigned char>((i + 1) >> 8);
            buf32[offset + 3] = static_cast<unsigned char>(i + 1);
            // Right = data
            size_t dp = kNodeCount + kSeparatorLen + (i % 10);
            buf32[offset + 4] = static_cast<unsigned char>(dp >> 24);
            buf32[offset + 5] = static_cast<unsigned char>(dp >> 16);
            buf32[offset + 6] = static_cast<unsigned char>(dp >> 8);
            buf32[offset + 7] = static_cast<unsigned char>(dp);
        }
        // Patch node 10's left record to data: a 32-bit zero-walk reaches
        // node 10 at depth 10, well within the 32-bit budget (the full
        // 99-node chain is unreachable in 32 steps by construction).
        {
            const size_t dp10 = kNodeCount + kSeparatorLen + 7;
            const size_t o10 = 10 * kNodeBytes32;
            buf32[o10] = static_cast<unsigned char>(dp10 >> 24);
            buf32[o10 + 1] = static_cast<unsigned char>(dp10 >> 16);
            buf32[o10 + 2] = static_cast<unsigned char>(dp10 >> 8);
            buf32[o10 + 3] = static_cast<unsigned char>(dp10);
        }

        size_t off = 0;
        bool r = ResolveOffsetOpt(zeroAddr, 32, 0, kNodeCount, kNodeBytes32,
                                  false, 4, buf32.data(), kTotalSize32,
                                  kSeparatorLen, &off);
        ReportTest("GeoIp: 32-bit record walk", r && off == 7,
                   "result=" + std::to_string(r) + " offset=" + std::to_string(off));
    }

    // Test: 28-bit record walk (packed format)
    {
        constexpr size_t kNodeBytes28 = 7;
        constexpr size_t kTreeSize28 = kNodeCount * kNodeBytes28;
        constexpr size_t kTotalSize28 = kTreeSize28 + kSeparatorLen + kDataSize;
        std::vector<unsigned char> buf28(kTotalSize28, 0);

        // For 28-bit: node is 7 bytes, two records share the middle byte
        // Format: [byte0, byte1, byte2, byte3, byte4, byte5, byte6]
        //   left  = (byte3>>4, byte0, byte1, byte2)  -> 28 bits
        //   right = (byte3&0x0F, byte4, byte5, byte6) -> 28 bits
        for (size_t i = 0; i < kNodeCount - 1; ++i) {
            size_t offset = i * kNodeBytes28;
            size_t next = i + 1;
            // Left record
            buf28[offset + 0] = static_cast<unsigned char>(next >> 16);
            buf28[offset + 1] = static_cast<unsigned char>(next >> 8);
            buf28[offset + 2] = static_cast<unsigned char>(next);
            // Right record (data pointer)
            size_t dp = kNodeCount + kSeparatorLen + (i % 10);
            buf28[offset + 4] = static_cast<unsigned char>(dp >> 16);
            buf28[offset + 5] = static_cast<unsigned char>(dp >> 8);
            buf28[offset + 6] = static_cast<unsigned char>(dp);
            // Middle byte: left high nibble + right high nibble
            buf28[offset + 3] = static_cast<unsigned char>(
                ((next >> 20) & 0xF0) | ((dp >> 20) & 0x0F));
        }
        // Patch node 10's left record to data (offset 9), preserving the
        // right record's low nibble in the shared middle byte.
        {
            const size_t dp10 = kNodeCount + kSeparatorLen + 9;
            const size_t o10 = 10 * kNodeBytes28;
            buf28[o10 + 0] = static_cast<unsigned char>(dp10 >> 16);
            buf28[o10 + 1] = static_cast<unsigned char>(dp10 >> 8);
            buf28[o10 + 2] = static_cast<unsigned char>(dp10);
            buf28[o10 + 3] = static_cast<unsigned char>(
                (buf28[o10 + 3] & 0x0Fu) |
                ((static_cast<unsigned char>((dp10 >> 24) & 0x0Fu)) << 4));
        }

        size_t off = 0;
        bool r = ResolveOffsetOpt(zeroAddr, 32, 0, kNodeCount, kNodeBytes28,
                                  true, 3, buf28.data(), kTotalSize28,
                                  kSeparatorLen, &off);
        ReportTest("GeoIp: 28-bit record walk", r && off == 9,
                   "result=" + std::to_string(r) + " offset=" + std::to_string(off));
    }

    // Benchmark
    constexpr int kBenchIters = 10000;
    auto start = Clock::now();
    for (int i = 0; i < kBenchIters; ++i) {
        size_t off = 0;
        ResolveOffsetOpt(addrBits, 32, 0, kNodeCount, kNodeBytes,
                         false, 3, buffer.data(), kTotalSize,
                         kSeparatorLen, &off);
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op\n", kBenchIters, avgNs);
}

// ============================================================================
// Test 2: TCP Reassembly Segment Overlap
// ============================================================================

void TestTcpReassembly() {
    printf("\n=== TCP Reassembly Segment Overlap ===\n");

    std::vector<uint64_t> segSeqs = {100, 200, 300, 400};
    std::vector<size_t> segSizes = {50, 50, 50, 50};

    // Test 1: Non-overlapping (append)
    {
        uint64_t seq = 500;
        size_t len = 100, consumed = 0, skip = 0;
        bool r = AddSegmentOverlapOpt(seq, len, segSeqs.data(), segSizes.data(),
                                      segSeqs.size(), &consumed, &skip);
        ReportTest("Reassembly: non-overlapping", r && consumed == 0 && skip == 100);
    }

    // Test 2: Fully duplicated
    {
        uint64_t seq = 100;
        size_t len = 50, consumed = 0, skip = 0;
        bool r = AddSegmentOverlapOpt(seq, len, segSeqs.data(), segSizes.data(),
                                      segSeqs.size(), &consumed, &skip);
        ReportTest("Reassembly: fully duplicated", !r && consumed == 50 && skip == 0);
    }

    // Test 3: Partial overlap (trim head)
    {
        uint64_t seq = 80;
        size_t len = 100, consumed = 0, skip = 0;
        bool r = AddSegmentOverlapOpt(seq, len, segSeqs.data(), segSizes.data(),
                                      segSeqs.size(), &consumed, &skip);
        ReportTest("Reassembly: partial overlap", r && consumed == 70 && skip == 30);
    }

    // Test 4: Empty segment list
    {
        size_t consumed = 0, skip = 0;
        bool r = AddSegmentOverlapOpt(100, 50, nullptr, nullptr, 0, &consumed, &skip);
        ReportTest("Reassembly: empty list", r && consumed == 0 && skip == 50);
    }

    // Test 5: Zero-length segment
    {
        size_t consumed = 0, skip = 0;
        bool r = AddSegmentOverlapOpt(100, 0, segSeqs.data(), segSizes.data(),
                                      segSeqs.size(), &consumed, &skip);
        ReportTest("Reassembly: zero-length", !r && consumed == 0 && skip == 0);
    }

    // Benchmark
    constexpr int kBenchIters = 100000;
    auto start = Clock::now();
    for (int i = 0; i < kBenchIters; ++i) {
        size_t consumed = 0, skip = 0;
        AddSegmentOverlapOpt(500, 100, segSeqs.data(), segSizes.data(),
                             segSeqs.size(), &consumed, &skip);
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op\n", kBenchIters, avgNs);
}

// ============================================================================
// Test 3: Filter Matching Substring Search
// ============================================================================

void TestFilterMatching() {
    printf("\n=== Filter Matching Substring Search ===\n");

    // Basic match
    ReportTest("Filter: basic substring",
        HasLowerSubstringOpt(L"chrome.exe", 10, L"chrom", 5));

    // No match
    ReportTest("Filter: no match",
        !HasLowerSubstringOpt(L"chrome.exe", 10, L"firefox", 7));

    // Empty needle
    ReportTest("Filter: empty needle",
        HasLowerSubstringOpt(L"chrome.exe", 10, L"", 0));

    // Needle longer than haystack
    ReportTest("Filter: needle too long",
        !HasLowerSubstringOpt(L"abc", 3, L"abcdefgh", 8));

    // Match at end
    ReportTest("Filter: match at end",
        HasLowerSubstringOpt(L"chrome.exe", 10, L".exe", 4));

    // Match at beginning
    ReportTest("Filter: match at start",
        HasLowerSubstringOpt(L"chrome.exe", 10, L"chro", 4));

    // Null haystack
    ReportTest("Filter: null haystack",
        !HasLowerSubstringOpt(nullptr, 0, L"test", 4));

    // Long strings (lengths via wcslen: the literal is 46 chars;
    // a hardcoded 48 read two wchar past the NUL).
    const wchar_t* longHay = L"chrome.exe:1234 -> 203.0.113.9:443 established";
    ReportTest("Filter: long string match",
        HasLowerSubstringOpt(longHay, wcslen(longHay), L"established", 11));

    // Benchmark
    constexpr int kBenchIters = 100000;
    const wchar_t* hay = L"chrome.exe:1234 -> 203.0.113.9:443 established";
    const size_t hayLen = wcslen(hay);
    const wchar_t* needle = L"established";
    int matches = 0;
    auto start = Clock::now();
    for (int i = 0; i < kBenchIters; ++i) {
        if (HasLowerSubstringOpt(hay, hayLen, needle, 11)) ++matches;
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op, %d matches\n", kBenchIters, avgNs, matches);
}

// ============================================================================
// Test 4: Packet Parsing
// ============================================================================

void TestPacketParsing() {
    printf("\n=== Packet Parsing ===\n");

    unsigned char packet[50] = {0};
    packet[0] = 0x45;        // IPv4, IHL=5
    packet[3] = 0x28;        // total length 40
    packet[9] = 6;           // protocol TCP
    packet[12] = 192; packet[13] = 168; packet[14] = 1; packet[15] = 100;
    packet[16] = 203; packet[17] = 0; packet[18] = 113; packet[19] = 9;
    packet[20] = 0xC8; packet[21] = 0x28;  // sport = 0xC828
    packet[22] = 0x01; packet[23] = 0xBB;  // dport = 0x01BB
    packet[24] = 0x12; packet[25] = 0x34; packet[26] = 0x56; packet[27] = 0x78;
    packet[28] = 0x87; packet[29] = 0x65; packet[30] = 0x43; packet[31] = 0x21;
    packet[32] = 0x50;       // data offset = 5
    packet[33] = 0x18;       // PSH+ACK flags
    packet[34] = 0x01; packet[35] = 0x00;  // window

    const char* payload = "HelloWorld";
    std::memcpy(packet + 40, payload, 10);

    uint16_t sp = 0, dp = 0, win = 0;
    uint32_t s = 0, a = 0;
    uint8_t flags = 0;
    const unsigned char* pl = nullptr;
    size_t plen = 0;

    ParseTcpHeaderOpt(packet, &sp, &dp, &s, &a, &flags, &win, &pl, &plen, 20, 50);

    ReportTest("Packet: src port", sp == 0xC828);
    ReportTest("Packet: dst port", dp == 0x01BB);
    ReportTest("Packet: seq num", s == 0x12345678);
    ReportTest("Packet: ack num", a == 0x87654321);
    ReportTest("Packet: flags", flags == 0x18);
    ReportTest("Packet: window", win == 0x0100);
    ReportTest("Packet: payload len", plen == 10);
    ReportTest("Packet: payload", pl != nullptr && std::memcmp(pl, "HelloWorld", 10) == 0);

    // Truncated packet
    ParseTcpHeaderOpt(packet, &sp, &dp, &s, &a, &flags, &win, &pl, &plen, 20, 30);
    ReportTest("Packet: truncated rejected", plen == 0);

    // Benchmark
    constexpr int kBenchIters = 100000;
    auto start = Clock::now();
    for (int i = 0; i < kBenchIters; ++i) {
        ParseTcpHeaderOpt(packet, &sp, &dp, &s, &a, &flags, &win, &pl, &plen, 20, 50);
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op\n", kBenchIters, avgNs);
}

// ============================================================================
// Test 5: FormatBytes
// ============================================================================

void TestFormatBytes() {
    printf("\n=== FormatBytes ===\n");

    wchar_t buf[32];

    ReportTest("FormatBytes: 0 B", FormatBytesOpt(buf, 32, 0) && wcscmp(buf, L"0 B") == 0);
    ReportTest("FormatBytes: 512 B", FormatBytesOpt(buf, 32, 512) && wcscmp(buf, L"512 B") == 0);
    ReportTest("FormatBytes: 1.5 KB", FormatBytesOpt(buf, 32, 1536) && wcscmp(buf, L"1.5 KB") == 0);
    ReportTest("FormatBytes: 5.0 MB", FormatBytesOpt(buf, 32, 5242880) && wcscmp(buf, L"5.0 MB") == 0);
    ReportTest("FormatBytes: 1.0 GB", FormatBytesOpt(buf, 32, 1073741824) && wcscmp(buf, L"1.00 GB") == 0);
    ReportTest("FormatBytes: 1.0 TB", FormatBytesOpt(buf, 32, 1099511627776ULL) && wcscmp(buf, L"1.00 TB") == 0);

    // Small buffer: truncation must behave like swprintf_s failure
    // (return 0, empty buffer) and must NOT invoke the CRT
    // invalid-parameter handler (modal dialog in Debug builds).
    wchar_t tiny[4];
    size_t tinyRet = FormatBytesOpt(tiny, 4, 999999999);
    ReportTest("FormatBytes: small buffer", tinyRet == 0 && tiny[0] == L'\0');

    // Benchmark
    constexpr int kBenchIters = 100000;
    auto start = Clock::now();
    for (int i = 0; i < kBenchIters; ++i) {
        FormatBytesOpt(buf, 32, 5242880);
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op\n", kBenchIters, avgNs);
}

// ============================================================================
// Test 6: Connection Key Building
// ============================================================================

void TestConnectionKey() {
    printf("\n=== Connection Key Building ===\n");

    unsigned char localAddr[16] = {192, 168, 1, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    unsigned char remoteAddr[16] = {203, 0, 113, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t localPort = 51752;
    uint32_t remotePort = 443;
    uint32_t pid = 42;

    unsigned char keyBuf[100] = {0};
    size_t keyLen = 0;

    // IPv4 TCP
    KeyOfOpt(false, false, localAddr, localPort, remoteAddr, remotePort, pid, keyBuf, &keyLen);
    ReportTest("ConnectionKey: IPv4 length", keyLen == 22);
    ReportTest("ConnectionKey: IPv4 family", keyBuf[0] == '4');
    ReportTest("ConnectionKey: IPv4 proto", keyBuf[1] == 'T');
    ReportTest("ConnectionKey: IPv4 local addr", std::memcmp(keyBuf + 2, localAddr, 4) == 0);
    ReportTest("ConnectionKey: IPv4 local port LSB", keyBuf[6] == static_cast<unsigned char>(localPort));

    // IPv6 TCP
    unsigned char localV6[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    unsigned char remoteV6[16] = {0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1};
    KeyOfOpt(true, false, localV6, 80, remoteV6, 443, 123, keyBuf, &keyLen);
    // IPv6 key: '6' 'T' + 16 + 4 + 16 + 4 + 4 = 46 bytes (the old
    // expectation of 38 omitted remotePort+pid; ConnectionStore's own
    // static_assert is 2+16+4+16+4+4 = 46).
    ReportTest("ConnectionKey: IPv6 length", keyLen == 46);
    ReportTest("ConnectionKey: IPv6 family", keyBuf[0] == '6');
    ReportTest("ConnectionKey: IPv6 proto", keyBuf[1] == 'T');
    ReportTest("ConnectionKey: IPv6 local addr", std::memcmp(keyBuf + 2, localV6, 16) == 0);

    // UDP
    KeyOfOpt(false, true, localAddr, 5353, remoteAddr, 0, pid, keyBuf, &keyLen);
    ReportTest("ConnectionKey: UDP proto", keyBuf[1] == 'U');

    // Benchmark
    constexpr int kBenchIters = 100000;
    auto start = Clock::now();
    for (int i = 0; i < kBenchIters; ++i) {
        KeyOfOpt(false, false, localAddr, localPort, remoteAddr, remotePort, pid, keyBuf, &keyLen);
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op\n", kBenchIters, avgNs);
}

// ============================================================================
// Test 7: WideToUtf8 Conversion
// ============================================================================

void TestWideToUtf8() {
    printf("\n=== WideToUtf8 Conversion ===\n");

    // ASCII
    {
        const wchar_t* src = L"Hello, World!";
        char dst[64] = {0};
        size_t len = WideToUtf8Opt(src, wcslen(src), dst, 64);
        ReportTest("WideToUtf8: ASCII", len == 13 && strcmp(dst, "Hello, World!") == 0);
    }

    // Empty
    {
        char dst[64] = {0};
        ReportTest("WideToUtf8: empty", WideToUtf8Opt(L"", 0, dst, 64) == 0);
    }

    // 2-byte UTF-8
    {
        const wchar_t* src = L"\xC0";
        char dst[64] = {0};
        ReportTest("WideToUtf8: 2-byte", WideToUtf8Opt(src, wcslen(src), dst, 64) == 2);
    }

    // 3-byte UTF-8
    {
        const wchar_t* src = L"\x3042";
        char dst[64] = {0};
        ReportTest("WideToUtf8: 3-byte", WideToUtf8Opt(src, wcslen(src), dst, 64) == 3);
    }

    // Mixed
    {
        const wchar_t* src = L"Test \x3042 value";
        char dst[64] = {0};
        size_t len = WideToUtf8Opt(src, wcslen(src), dst, 64);
        ReportTest("WideToUtf8: mixed", len >= 10);
    }

    // Truncation: "Hello" into 3 bytes writes "Hel", no NUL.
    {
        const wchar_t* src = L"Hello";
        char dst[3] = {0};
        size_t n = WideToUtf8Opt(src, wcslen(src), dst, 3);
        ReportTest("WideToUtf8: truncation",
                   n == 3 && std::memcmp(dst, "Hel", 3) == 0);
    }

    // Null pointers
    ReportTest("WideToUtf8: null", WideToUtf8Opt(nullptr, 0, nullptr, 0) == 0);

    // Benchmark
    constexpr int kBenchIters = 100000;
    const wchar_t* benchSrc = L"The quick brown fox jumps over the lazy dog";
    char benchDst[256];
    auto start = Clock::now();
    for (int i = 0; i < kBenchIters; ++i) {
        WideToUtf8Opt(benchSrc, wcslen(benchSrc), benchDst, 256);
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op\n", kBenchIters, avgNs);
}

// ============================================================================
// Test 8: Stream Hexdump (P0 #1)
// ============================================================================

void TestStreamHex() {
    printf("\n=== Stream Hexdump ===\n");

    ReportTest("Hex: empty", FormatStreamHexOpt("", 16).empty());

    {
        std::string b(1, '\x41');
        const std::string want =
            std::string("00000000  41 ") + std::string(15 * 3, ' ') +
            " |A|\r\n";
        ReportTest("Hex: single byte", FormatStreamHexOpt(b, 16) == want);
    }

    {
        std::string b;
        for (int i = 0; i < 16; ++i) b += static_cast<char>(i);
        const std::string want =
            "00000000  "
            "00 01 02 03 04 05 06 07 "
            " "
            "08 09 0a 0b 0c 0d 0e 0f "
            " |................|\r\n";
        ReportTest("Hex: full line", FormatStreamHexOpt(b, 16) == want);
    }

    {
        std::string b(7, '\x61');
        ReportTest("Hex: odd width gap",
                   FormatStreamHexOpt(b, 7) ==
                       "00000000  61 61 61  61 61 61 61  |aaaaaaa|\r\n");
    }

    {
        const std::string b(20, '\x62');
        ReportTest("Hex: clamp",
                   FormatStreamHexOpt(b, 0) == FormatStreamHexOpt(b, 16) &&
                       FormatStreamHexOpt(b, 99) ==
                           FormatStreamHexOpt(b, 16));
    }

    {
        // All byte values round-trip through hex + gutter without
        // truncation: 256 bytes at 16/line = 16 lines.
        std::string all;
        for (int i = 0; i < 256; ++i) all += static_cast<char>(i);
        const std::string got = FormatStreamHexOpt(all, 16);
        size_t lines = 0;
        for (size_t p = 0; (p = got.find("\r\n", p)) != std::string::npos;
             ++p, ++lines) {
        }
        ReportTest("Hex: 256 bytes = 16 lines",
                   lines == 16 && got.size() == 16 * 80);
    }

    // Benchmark: typical 1460-byte segment.
    std::string seg(1460, '\0');
    for (size_t i = 0; i < seg.size(); ++i) {
        seg[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }
    constexpr int kBenchIters = 20000;
    auto start = Clock::now();
    size_t sink = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        sink += FormatStreamHexOpt(seg, 16).size();
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (sink=%zu)\n", kBenchIters,
           avgNs, sink);
}

// ============================================================================
// Test 9: Wide Lowercase (P0 #2)
// ============================================================================

void TestToLower() {
    printf("\n=== Wide Lowercase ===\n");

    ReportTest("Lower: empty", ToLowerWOpt(L"").empty());
    ReportTest("Lower: ASCII",
               ToLowerWOpt(L"AbC XyZ 09 @[`{") == L"abc xyz 09 @[`{");
    ReportTest("Lower: no-op", ToLowerWOpt(L"chrome.exe") == L"chrome.exe");

    {
        const std::wstring got = ToLowerWOpt(L"A\x0080" L"B");
        ReportTest("Lower: non-ASCII fallback", got == L"a\x0080" L"b");
    }

    {
        const std::wstring in{L'\xD800', L'\xDC00', L'X'};
        const std::wstring want{L'\xD800', L'\xDC00', L'x'};
        ReportTest("Lower: surrogates kept", ToLowerWOpt(in) == want);
    }

    {
        const std::wstring in(L"A\0B", 3);
        const std::wstring got = ToLowerWOpt(in);
        ReportTest("Lower: embedded NUL",
                   got.size() == 3 && got[0] == L'a' && got[1] == L'\0' &&
                       got[2] == L'b');
    }

    // Benchmark: 260-char mixed-case path.
    std::wstring path(260, L'\0');
    for (size_t i = 0; i < path.size(); ++i) {
        path[i] = static_cast<wchar_t>((i % 2) ? L'a' + (i % 26)
                                               : L'A' + (i % 26));
    }
    constexpr int kBenchIters = 100000;
    auto start = Clock::now();
    size_t sink = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        sink += ToLowerWOpt(path).size();
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (sink=%zu)\n", kBenchIters,
           avgNs, sink);
}

// ============================================================================
// Test 10/11: Big-Endian Field Reads (P0 #3/#4)
// ============================================================================

void TestBeReads() {
    printf("\n=== Big-Endian Field Reads ===\n");

    const unsigned char buf[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB,
                                   0xCD, 0xEF, 0x11, 0x22, 0x33, 0x44,
                                   0x55, 0x66, 0x77, 0x88};

    ReportTest("BE: u32", ReadU32BEOpt(buf) == 0x01234567u);
    ReportTest("BE: u32 unaligned",
               ReadU32BEOpt(buf + 1) == 0x23456789u &&
                   ReadU32BEOpt(buf + 5) == 0xABCDEF11u);
    ReportTest("BE: u64", ReadU64BEOpt(buf) == 0x0123456789ABCDEFull);
    ReportTest("BE: u64 unaligned",
               ReadU64BEOpt(buf + 3) == 0x6789ABCDEF112233ull);

    {
        bool ok = true;
        for (size_t n = 0; n <= 8; ++n) {
            uint64_t want = 0;
            for (size_t i = 0; i < n; ++i) {
                want = (want << 8) | buf[i];
            }
            uint64_t got = ~0ull;
            ok = ok && ReadBytesOpt(buf, n, &got) && got == want;
        }
        ReportTest("BE: bytes n=0..8", ok);
    }

    // Null guards (opt-only binary; the originals have no guards).
    {
        uint64_t v = 0;
        ReportTest("BE: null guards",
                   ReadU32BEOpt(nullptr) == 0 &&
                       ReadU64BEOpt(nullptr) == 0 &&
                       !ReadBytesOpt(nullptr, 4, &v) &&
                       !ReadBytesOpt(buf, 4, nullptr) &&
                       !ReadBytesOpt(buf, 9, &v) &&
                       TlsBe16Opt(nullptr) == 0 &&
                       TlsBe24Opt(nullptr) == 0 &&
                       TlsBe32Opt(nullptr) == 0);
    }

    const unsigned char hdr[8] = {0x16, 0x03, 0x03, 0x01,
                                  0x00, 0xAA, 0xBB, 0xCC};
    ReportTest("TLS: u16", TlsBe16Opt(hdr + 1) == 0x0303u);
    ReportTest("TLS: u16 unaligned", TlsBe16Opt(hdr + 3) == 0x0100u);
    ReportTest("TLS: u24", TlsBe24Opt(hdr + 5) == 0xAABBCCu);
    ReportTest("TLS: u24 0x010203", TlsBe24Opt(hdr + 3) == 0x0100AAu);
    ReportTest("TLS: u32", TlsBe32Opt(hdr + 4) == 0x00AABBCCu);

    // Benchmark: unaligned u32 loads.
    constexpr int kBenchIters = 1000000;
    auto start = Clock::now();
    uint64_t sink = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        sink += ReadU32BEOpt(buf + (i & 7));
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (sink=%llu)\n",
           kBenchIters, avgNs, static_cast<unsigned long long>(sink));
}

// ============================================================================
// Test 12/13: TLS Extension Dispatch + Pcap Loads (P0 #5/#6)
// ============================================================================

void TestTlsExtRd() {
    printf("\n=== TLS Extension Dispatch + Pcap Loads ===\n");

    ReportTest("Ext: SNI",
               ClassifyTlsExtensionOpt(0x0000) == TlsExtAction::kSni);
    ReportTest("Ext: ALPN",
               ClassifyTlsExtensionOpt(0x0010) == TlsExtAction::kAlpn);
    {
        const uint16_t others[] = {1, 15, 17, 0x002B, 0x00FF, 0xFFFF};
        bool ok = true;
        for (uint16_t v : others) {
            ok = ok && ClassifyTlsExtensionOpt(v) == TlsExtAction::kSkip;
        }
        ReportTest("Ext: others skip", ok);
    }

    {
        char host[] = "example.com...";
        ReportTest("SNI: strip",
                   StripSniTailOpt(host, sizeof(host) - 1) == 11 &&
                       std::memcmp(host, "example.com", 11) == 0);
    }
    {
        char all[64];
        std::memset(all, '.', sizeof(all));
        ReportTest("SNI: all-strip", StripSniTailOpt(all, 64) == 0);
    }
    ReportTest("SNI: null", StripSniTailOpt(nullptr, 0) == 0);

    const unsigned char buf[8] = {0x12, 0x34, 0x56, 0x78,
                                  0x9A, 0xBC, 0xDE, 0xF0};
    ReportTest("Rd: u16 LE/BE",
               Rd16Opt(buf, false) == 0x3412u &&
                   Rd16Opt(buf, true) == 0x1234u);
    ReportTest("Rd: u32 LE/BE",
               Rd32Opt(buf, false) == 0x78563412u &&
                   Rd32Opt(buf, true) == 0x12345678u);
    ReportTest("Rd: unaligned",
               Rd16Opt(buf + 1, true) == 0x3456u &&
                   Rd32Opt(buf + 3, true) == 0x789ABCDEu);
    ReportTest("Rd: null", Rd16Opt(nullptr, true) == 0 &&
                               Rd32Opt(nullptr, false) == 0);

    // Benchmark: mixed LE/BE u32 at cycling offsets.
    constexpr int kBenchIters = 1000000;
    auto start = Clock::now();
    uint64_t sink = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        const size_t o = static_cast<size_t>(i) & 7;
        sink += (o & 1) ? Rd32Opt(buf + o, true) : Rd32Opt(buf + o, false);
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (sink=%llu)\n",
           kBenchIters, avgNs, static_cast<unsigned long long>(sink));
}

// ============================================================================
// Test 14: Socket-Sample Join (P0 #7)
// ============================================================================

void TestJoin() {
    printf("\n=== Socket-Sample Join ===\n");

    auto row = [](uint32_t lp, uint32_t rp, const wchar_t* la,
                  const wchar_t* ra, bool tcp = true) {
        JoinRow r;
        r.localPort = lp;
        r.remotePort = rp;
        r.local = la;
        r.remote = ra;
        r.tcp = tcp;
        return r;
    };
    auto sample = [](uint32_t lp, uint32_t rp, const wchar_t* la,
                     const wchar_t* ra, uint64_t rx, uint64_t tx) {
        JoinSample s;
        s.localPort = lp;
        s.remotePort = rp;
        s.local = la;
        s.remote = ra;
        s.rx = rx;
        s.tx = tx;
        return s;
    };

    {
        std::vector<JoinRow> rows;
        std::vector<JoinSample> ss;
        ReportTest("Join: empty", JoinSamplesOpt(rows, ss) == 0);
    }

    {
        std::vector<JoinRow> rows = {
            row(5000, 443, L"10.0.0.5", L"93.184.216.34")};
        std::vector<JoinSample> ss = {
            sample(5000, 443, L"10.0.0.5", L"93.184.216.34", 100, 200)};
        ReportTest("Join: single",
                   JoinSamplesOpt(rows, ss) == 1 && rows[0].rx == 100 &&
                       rows[0].tx == 200 && rows[0].perRow);
    }

    {
        std::vector<JoinRow> rows = {
            row(5353, 5353, L"10.0.0.5", L"224.0.0.251"),
            row(5353, 5353, L"10.0.0.5", L"224.0.0.251")};
        std::vector<JoinSample> ss = {
            sample(5353, 5353, L"10.0.0.5", L"224.0.0.251", 1, 10),
            sample(5353, 5353, L"10.0.0.5", L"224.0.0.251", 2, 20)};
        ReportTest("Join: duplicates in order",
                   JoinSamplesOpt(rows, ss) == 2 && rows[0].rx == 1 &&
                       rows[1].rx == 2);
    }

    {
        std::vector<JoinRow> rows = {
            row(5000, 443, L"10.0.0.5", L"93.184.216.34", false)};
        std::vector<JoinSample> ss = {
            sample(5000, 443, L"10.0.0.5", L"93.184.216.34", 1, 2)};
        ReportTest("Join: non-TCP skipped",
                   JoinSamplesOpt(rows, ss) == 0 && !rows[0].perRow);
    }

    // Benchmark: 500 rows x 300 samples.
    std::vector<JoinRow> rows;
    std::vector<JoinSample> ss;
    for (uint64_t i = 0; i < 500; ++i) {
        wchar_t la[32] = {0};
        ::swprintf_s(la, 32, L"10.0.0.%llu", (i % 250) + 1);
        rows.push_back(row(5000 + static_cast<uint32_t>(i % 100), 443,
                           la, L"93.184.216.34"));
    }
    for (uint64_t i = 0; i < 300; ++i) {
        wchar_t la[32] = {0};
        ::swprintf_s(la, 32, L"10.0.0.%llu", (i % 250) + 1);
        ss.push_back(sample(5000 + static_cast<uint32_t>(i % 100), 443,
                            la, L"93.184.216.34", i, i + 1));
    }
    constexpr int kBenchIters = 2000;
    auto start = Clock::now();
    int total = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        for (JoinRow& r : rows) {
            r.rx = 0;
            r.tx = 0;
            r.perRow = false;
        }
        total += JoinSamplesOpt(rows, ss);
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (total=%d)\n",
           kBenchIters, avgNs, total);
}

// ============================================================================
// Test 15/16/17: BMH Search, Lower-All Concat, Wide Compare (P0 #8-#10)
// ============================================================================

void TestFindCompare() {
    printf("\n=== BMH Search + Concat + Wide Compare ===\n");

    ReportTest("BMH: empty needle",
               FindSubstringLongOpt(L"chrome.exe", L""));
    ReportTest("BMH: longer needle",
               !FindSubstringLongOpt(L"abc", L"abcdef"));
    ReportTest("BMH: start",
               FindSubstringLongOpt(L"chrome.exe", L"chro"));
    ReportTest("BMH: middle",
               FindSubstringLongOpt(L"chrome.exe", L"ome.e"));
    ReportTest("BMH: absent",
               !FindSubstringLongOpt(L"chrome.exe", L"firefox"));
    ReportTest("BMH: single hit",
               FindSubstringLongOpt(L"chrome.exe", L"m"));
    ReportTest("BMH: single miss",
               !FindSubstringLongOpt(L"chrome.exe", L"q"));
    {
        std::wstring h(4096, L'k');
        h.replace(4000, 32, std::wstring(32, L'm'));
        ReportTest("BMH: 32-char present",
                   FindSubstringLongOpt(h, std::wstring(32, L'm')));
        ReportTest("BMH: 32-char absent",
                   !FindSubstringLongOpt(h, std::wstring(32, L'z')));
    }

    {
        const std::wstring fv[10] = {
            L"10.0.0.5",   L"93.184.216.34", L"established",
            L"4242",       L"chrome.exe",    L"c:\\a\\b.exe",
            L"svc",        L"host",          L"tcp",
            L"parent"};
        const std::wstring* const f[] = {
            &fv[0], &fv[1], &fv[2], &fv[3], &fv[4],
            &fv[5], &fv[6], &fv[7], &fv[8], &fv[9]};
        ReportTest("LowerAll: exact",
                   BuildLowerAllOpt(f, 10) ==
                       L"10.0.0.5 93.184.216.34 established 4242 "
                       L"chrome.exe c:\\a\\b.exe svc host tcp parent");
        ReportTest("LowerAll: short empty",
                   BuildLowerAllOpt(f, 3).empty());
    }

    auto signOf = [](int v) { return (v > 0) - (v < 0); };
    auto cmpCheck = [&signOf](const wchar_t* a, size_t na,
                              const wchar_t* b, size_t nb) {
        const int want = signOf(
            std::wstring(a, na).compare(std::wstring(b, nb)));
        return signOf(CompareWideOpt(a, na, b, nb)) == want;
    };
    ReportTest("Cmp: equal", cmpCheck(L"chrome.exe", 10, L"chrome.exe", 10));
    ReportTest("Cmp: prefix", cmpCheck(L"chrom", 5, L"chrome", 6));
    ReportTest("Cmp: lane edge",
               cmpCheck(L"01234567x", 9, L"01234567y", 9));
    ReportTest("Cmp: empty", cmpCheck(L"", 0, L"a", 1));
    {
        std::wstring a(256, L'm');
        std::wstring b(256, L'm');
        b[250] = L'n';
        ReportTest("Cmp: late diff",
                   cmpCheck(a.c_str(), 256, b.c_str(), 256));
    }
    ReportTest("Cmp: null", CompareWideOpt(nullptr, 0, nullptr, 0) == 0 &&
                                 CompareWideOpt(nullptr, 0, L"a", 1) < 0 &&
                                 CompareWideOpt(L"a", 1, nullptr, 0) > 0);

    // Benchmarks: BMH 32-char absent + 256-char compare.
    std::wstring hay(4096, L'k');
    const std::wstring ndl(32, L'z');
    constexpr int kBenchIters = 50000;
    auto start = Clock::now();
    size_t hits = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        if (FindSubstringLongOpt(hay, ndl)) ++hits;
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (hits=%zu)\n",
           kBenchIters, avgNs, hits);

    std::wstring ca(256, L'm');
    std::wstring cb(256, L'm');
    cb[250] = L'n';
    auto start2 = Clock::now();
    long long acc = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        acc += CompareWideOpt(ca.c_str(), 256, cb.c_str(), 256);
    }
    auto end2 = Clock::now();
    auto elapsed2 = std::chrono::duration_cast<Nanoseconds>(end2 - start2);
    double avgNs2 = static_cast<double>(elapsed2.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (acc=%lld)\n",
           kBenchIters, avgNs2, acc);
}

// ============================================================================
// Test 18/19/20: Key Hash/Eq, Batch Rates, PID Sums (P1 #11/#13)
// ============================================================================

void TestKeyRates() {
    printf("\n=== Key Hash/Eq + Batch Rates + PID Sums ===\n");

    const unsigned char k4[22] = {'4', 'T', 192, 168, 1, 10,
                                  0x39, 0xC8, 0x00, 0x00, 8, 8,
                                  8, 8, 0xBB, 0x01, 0x00, 0x00,
                                  0x92, 0x10, 0x00, 0x00};
    ReportTest("Key: hash stable",
               HashConnKeyOpt(k4, 22) == HashConnKeyOpt(k4, 22));
    ReportTest("Key: eq",
               EqualConnKeyOpt(k4, 22, k4, 22) &&
                   !EqualConnKeyOpt(k4, 22, k4, 21));
    {
        unsigned char mut[22];
        std::memcpy(mut, k4, 22);
        mut[10] ^= 0x01;
        ReportTest("Key: mid differs",
                   !EqualConnKeyOpt(k4, 22, mut, 22) &&
                       HashConnKeyOpt(k4, 22) != HashConnKeyOpt(mut, 22));
    }
    ReportTest("Key: null", HashConnKeyOpt(nullptr, 0) == 0 &&
                                 !EqualConnKeyOpt(nullptr, 0, k4, 22));

    {
        const uint64_t prev[2] = {1000, 5000};
        const uint64_t now[2] = {2500, 8000};
        double out[2] = {0, 0};
        ReportTest("Bps: basic",
                   ComputeBpsBatchOpt(prev, now, 1000, out, 2) == 2 &&
                       out[0] == 1500.0 && out[1] == 3000.0);
    }
    {
        const uint64_t prev[1] = {1000};
        const uint64_t now[1] = {2500};
        double out[1] = {0};
        ReportTest("Bps: zero elapsed",
                   ComputeBpsBatchOpt(prev, now, 0, out, 1) == 0);
        ReportTest("Bps: backwards",
                   ComputeBpsBatchOpt(now, prev, 1000, out, 1) == 0 &&
                       out[0] == 0.0);
    }

    {
        std::vector<PidTrafficRow> rows(3);
        rows[0].pid = 7;
        rows[0].counted = true;
        rows[0].rx = 100;
        rows[1].pid = 7;
        rows[1].counted = true;
        rows[1].rx = 50;
        rows[2].pid = 9;
        rows[2].counted = false;
        SumPidTrafficOpt(rows.data(), rows.size());
        ReportTest("PidSum: sums",
                   rows[0].hasSum && rows[0].sumRx == 150 &&
                       rows[1].sumRx == 150 && !rows[2].hasSum);
    }

    // Benchmarks: 46B hash + 1K rates.
    unsigned char key46[46] = {0};
    for (size_t i = 0; i < 46; ++i) key46[i] = static_cast<unsigned char>(i);
    constexpr int kBenchIters = 200000;
    auto start = Clock::now();
    size_t acc = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        acc += HashConnKeyOpt(key46, 46);
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (acc=%zu)\n",
           kBenchIters, avgNs, acc);
}

// ============================================================================
// Test 21/22/23: Rate Cell, UTF-8 Widen, Unicast (P1 #14/#16/#17)
// ============================================================================

void TestBpsWidenUnicast() {
    printf("\n=== Rate Cell + Widen + Unicast ===\n");

    {
        wchar_t buf[64] = {0};
        size_t n = FormatBpsCellOpt(buf, 64, 0, 0, false);
        ReportTest("Cell: unknown",
                   n == 1 && std::wstring(buf, n) == L"—");
    }
    {
        wchar_t buf[64] = {0};
        size_t n = FormatBpsCellOpt(buf, 64, 0.1, 0.2, true);
        ReportTest("Cell: idle",
                   n == 4 && std::wstring(buf, n) == L"idle");
    }
    {
        wchar_t buf[64] = {0};
        size_t n = FormatBpsCellOpt(buf, 64, 1500.0, 3000000.0, true);
        ReportTest("Cell: normal",
                   n == 22 &&
                       std::wstring(buf, n) == L"↓ 1.5 KB/s  ↑ 2.9 MB/s");
    }
    {
        wchar_t tiny[4] = {0};
        ReportTest("Cell: truncate",
                   FormatBpsCellOpt(tiny, 4, 1500.0, 3000000.0,
                                    true) == 0 &&
                       tiny[0] == L'\0');
    }

    {
        const unsigned char iso[] = {'U', 'S'};
        ReportTest("Widen: ascii", WidenUtf8Opt(iso, 2) == L"US");
    }
    {
        const unsigned char b[] = {
            static_cast<unsigned char>(0xC3),
            static_cast<unsigned char>(0xA9),
            static_cast<unsigned char>(0xE3),
            static_cast<unsigned char>(0x81),
            static_cast<unsigned char>(0x82)};
        const std::wstring want{L'\x00E9', L'\x3042'};
        ReportTest("Widen: 2+3 byte", WidenUtf8Opt(b, 5) == want);
    }
    {
        const unsigned char b[] = {
            static_cast<unsigned char>(0xC0),
            static_cast<unsigned char>(0xAF),
            static_cast<unsigned char>(0x80)};
        ReportTest("Widen: malformed",
                   WidenUtf8Opt(b, 3) == std::wstring(3, L'\xFFFD'));
    }
    {
        const unsigned char b[] = {
            static_cast<unsigned char>(0xF0),
            static_cast<unsigned char>(0x9F),
            static_cast<unsigned char>(0x98),
            static_cast<unsigned char>(0x80)};
        ReportTest("Widen: 4-byte",
                   WidenUtf8Opt(b, 4) == std::wstring(4, L'\xFFFD'));
    }

    ReportTest("V4: public", IsGlobalUnicastV4Opt(0x08080808u));
    ReportTest("V4: private",
               !IsGlobalUnicastV4Opt(0x0A000001u) &&
                   !IsGlobalUnicastV4Opt(0xC0A80101u));
    ReportTest("V4: loopback", !IsGlobalUnicastV4Opt(0x7F000001u));
    ReportTest("V4: 172 range",
               !IsGlobalUnicastV4Opt(0xAC100001u) &&
                   !IsGlobalUnicastV4Opt(0xAC1F0001u) &&
                   IsGlobalUnicastV4Opt(0xAC0F0001u) &&
                   IsGlobalUnicastV4Opt(0xAC200001u));
    ReportTest("V4: 100.64/10",
               !IsGlobalUnicastV4Opt(0x64400001u) &&
                   IsGlobalUnicastV4Opt(0x64800001u));
    ReportTest("V4: multicast/broadcast",
               !IsGlobalUnicastV4Opt(0xE0000001u) &&
                   !IsGlobalUnicastV4Opt(0xFFFFFFFFu));
    {
        const unsigned char loop[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                        0, 0, 0, 0, 0, 0, 0, 1};
        const unsigned char pub[16] = {0x26, 0x06, 0x47, 0x00, 0x47,
                                       0x00, 0,    0,    0,    0, 0,
                                       0,    0,    0,    0x11, 0x11};
        const unsigned char ula[16] = {0xFC, 0, 0, 0, 0, 0, 0, 0,
                                       0,    0, 0, 0, 0, 0, 0, 1};
        const unsigned char unspec[16] = {0};
        // ::1 has a non-zero tail, so the predicate allows it (only the
        // all-zero ::/64 tail is denied); :: itself is denied.
        ReportTest("V6: loopback allowed", IsGlobalUnicastV6Opt(loop));
        ReportTest("V6: public allowed", IsGlobalUnicastV6Opt(pub));
        ReportTest("V6: unspecified denied",
                   !IsGlobalUnicastV6Opt(unspec));
        ReportTest("V6: ULA denied", !IsGlobalUnicastV6Opt(ula));
        ReportTest("V6: null", !IsGlobalUnicastV6Opt(nullptr));
    }

    // Benchmark: V4 pool + widen org.
    uint32_t pool[64];
    for (size_t i = 0; i < 64; ++i) {
        pool[i] = static_cast<uint32_t>(i * 0x04030201u);
    }
    constexpr int kBenchIters = 1000000;
    auto start = Clock::now();
    size_t hits = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        if (IsGlobalUnicastV4Opt(pool[i & 63])) ++hits;
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (hits=%zu)\n",
           kBenchIters, avgNs, hits);
}

// ============================================================================
// Test 24/25: Snapshot Pairing + Counted-Key Find (P1 #12/#15)
// ============================================================================

void TestPairFind() {
    printf("\n=== Snapshot Pairing + Counted-Key Find ===\n");

    auto skey = [](const char* s) {
        SnapKey k;
        k.len = std::strlen(s);
        if (k.len > 46) k.len = 46;
        std::memcpy(k.bytes, s, k.len);
        return k;
    };

    {
        SnapKey prev[1] = {skey("A")};
        SnapKey fresh[1] = {skey("A")};
        int out[1] = {-7};
        ReportTest("Pair: single",
                   PairSnapshotOpt(prev, 1, fresh, 1, out) == 1 &&
                       out[0] == 0);
    }
    {
        SnapKey prev[1] = {skey("A")};
        SnapKey fresh[1] = {skey("B")};
        int out[1] = {-7};
        ReportTest("Pair: newcomer",
                   PairSnapshotOpt(prev, 1, fresh, 1, out) == 0 &&
                       out[0] == -1);
    }
    {
        SnapKey prev[3] = {skey("K"), skey("K"), skey("K")};
        SnapKey fresh[2] = {skey("K"), skey("K")};
        int out[2] = {-7, -7};
        ReportTest("Pair: mDNS FIFO",
                   PairSnapshotOpt(prev, 3, fresh, 2, out) == 2 &&
                       out[0] == 0 && out[1] == 1);
    }
    {
        SnapKey prev[3] = {skey("A"), skey("B"), skey("A")};
        SnapKey fresh[3] = {skey("A"), skey("A"), skey("B")};
        int out[3] = {-7, -7, -7};
        ReportTest("Pair: interleaved",
                   PairSnapshotOpt(prev, 3, fresh, 3, out) == 3 &&
                       out[0] == 0 && out[1] == 2 && out[2] == 1);
    }

    const char k0[] = "iso_code";
    const char k1[] = "names";
    const CountedKey keys[2] = {
        {reinterpret_cast<const unsigned char*>(k0), 8},
        {reinterpret_cast<const unsigned char*>(k1), 5}};
    const unsigned char wIso[] = {'i', 's', 'o', '_', 'c', 'o', 'd', 'e'};
    const unsigned char wNo[] = {'x', 'x', 'x', 'x', 'x'};
    ReportTest("FindKey: hit",
               FindCountedKeyOpt(keys, 2, wIso, 8) == 0);
    ReportTest("FindKey: miss",
               FindCountedKeyOpt(keys, 2, wNo, 5) == -1);
    ReportTest("FindKey: null",
               FindCountedKeyOpt(nullptr, 2, wIso, 8) == -1);

    // Benchmark: 500x500 pairing.
    std::vector<SnapKey> prev(500);
    std::vector<SnapKey> fresh(500);
    for (size_t i = 0; i < 500; ++i) {
        char tmp[32] = {0};
        ::snprintf(tmp, sizeof(tmp), "4Tkey-%05zu", i % 400);
        prev[i] = skey(tmp);
        ::snprintf(tmp, sizeof(tmp), "4Tkey-%05zu", (i + 7) % 400);
        fresh[i] = skey(tmp);
    }
    std::vector<int> out(500, -7);
    constexpr int kBenchIters = 5000;
    auto start = Clock::now();
    int total = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        total += PairSnapshotOpt(prev.data(), 500, fresh.data(), 500,
                                 out.data());
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (total=%d)\n",
           kBenchIters, avgNs, total);
}

// ============================================================================
// Test 26: Flow-Key Equality (P1 #18)
// ============================================================================

void TestFlowKey() {
    printf("\n=== Flow-Key Equality ===\n");

    FlowKey k1;
    std::memset(&k1, 0, sizeof(k1));
    k1.addrA[0] = 10;
    k1.addrA[3] = 5;
    k1.addrB[0] = 93;
    k1.portA = 5000;
    k1.portB = 443;
    FlowKey k2 = k1;

    ReportTest("Flow: equal", FlowKeyEqualOpt(k1, k2));
    {
        FlowKey k3 = k2;
        k3.portA = 5001;
        ReportTest("Flow: port differs", !FlowKeyEqualOpt(k1, k3));
    }
    {
        FlowKey k3 = k2;
        k3.addrB[15] = 1;
        ReportTest("Flow: late differs", !FlowKeyEqualOpt(k1, k3));
    }
    ReportTest("Flow: cmp equal",
               CmpFlowAddrOpt(k1.addrA, k2.addrA) == 0);
    {
        unsigned char hi[16] = {0xFF, 0, 0, 0, 0, 0, 0, 0,
                                0,    0, 0, 0, 0, 0, 0, 0};
        unsigned char lo[16] = {0};
        auto signOf = [](int v) { return (v > 0) - (v < 0); };
        ReportTest("Flow: cmp unsigned",
                   signOf(CmpFlowAddrOpt(hi, lo)) ==
                       signOf(std::memcmp(hi, lo, 16)));
    }
    ReportTest("Flow: cmp null", CmpFlowAddrOpt(nullptr, nullptr) == 0);

    // Benchmark: equal-key compares.
    constexpr int kBenchIters = 1000000;
    auto start = Clock::now();
    size_t hits = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        if (FlowKeyEqualOpt(k1, k2)) ++hits;
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (hits=%zu)\n",
           kBenchIters, avgNs, hits);
}

// ============================================================================
// Test 27/28: Handle Filter + ETW Classify (P1 #19/#20)
// ============================================================================

void TestHandlesEtw() {
    printf("\n=== Handle Filter + ETW Classify ===\n");

    const uint32_t pids[3] = {100, 200, 300};
    const uint32_t types[2] = {5, 9};
    const PidHandle skip[1] = {{200, 0x40}};

    {
        size_t out[8] = {0};
        ReportTest("Handles: empty",
                   FilterHandlesOpt(nullptr, 0, pids, 3, types, 2,
                                    true, skip, 1, out, 8) == 0);
    }
    {
        const HandleEntry e[3] = {
            {100, 5, 0x10}, {999, 5, 0x10}, {100, 7, 0x10}};
        size_t out[8] = {9, 9, 9, 9, 9, 9, 9, 9};
        ReportTest("Handles: filter",
                   FilterHandlesOpt(e, 3, pids, 3, types, 2, true,
                                    skip, 1, out, 8) == 1 &&
                       out[0] == 0);
    }
    {
        // Stalled pair rejected, sibling accepted.
        const HandleEntry e[2] = {{200, 5, 0x40}, {200, 5, 0x41}};
        size_t out[8] = {9, 9, 9, 9, 9, 9, 9, 9};
        ReportTest("Handles: skip",
                   FilterHandlesOpt(e, 2, pids, 3, types, 2, true,
                                    skip, 1, out, 8) == 1 &&
                       out[0] == 1);
    }
    {
        // Wide PID skipped.
        const HandleEntry e[1] = {{0x1FFFFFFFFull, 5, 0x10}};
        size_t out[8] = {0};
        ReportTest("Handles: wide pid",
                   FilterHandlesOpt(e, 1, pids, 3, types, 2, true,
                                    skip, 1, out, 8) == 0);
    }

    const unsigned char tcp[16] = {
        0xC0, 0x0A, 0x28, 0x9A, 0xE0, 0xC8, 0xD1, 0x11,
        0x84, 0xE2, 0x00, 0xC0, 0x4F, 0xB9, 0x98, 0xA2};
    const unsigned char udp[16] = {
        0xC5, 0x50, 0x3A, 0xBF, 0xC9, 0xA9, 0x88, 0x49,
        0xA0, 0x05, 0x2D, 0xF0, 0xB7, 0xC8, 0x0F, 0x80};
    const unsigned char other[16] = {0};
    ReportTest("ETW: tcp send",
               ClassifyEventOpt(tcp, 0, 10) == TrafficDir::kSent);
    ReportTest("ETW: tcp recv",
               ClassifyEventOpt(tcp, 0, 11) == TrafficDir::kReceived);
    ReportTest("ETW: udp send",
               ClassifyEventOpt(udp, 0, 26) == TrafficDir::kSent);
    ReportTest("ETW: foreign",
               ClassifyEventOpt(other, 0, 10) == TrafficDir::kNone);
    ReportTest("ETW: op18 excluded",
               ClassifyEventOpt(tcp, 0, 18) == TrafficDir::kNone);
    ReportTest("ETW: zero-zero",
               ClassifyEventOpt(tcp, 0, 0) == TrafficDir::kNone);
    ReportTest("ETW: id fallback",
               ClassifyEventOpt(tcp, 27, 0) == TrafficDir::kReceived);
    {
        const unsigned char pl[8] = {0x2A, 0x00, 0x00, 0x00,
                                     0x40, 0x06, 0x00, 0x00};
        uint32_t pid = 0;
        uint32_t size = 0;
        ReportTest("ETW: payload",
                   ParseEventPayloadOpt(pl, 8, &pid, &size) &&
                       pid == 42 && size == 1600);
    }
    ReportTest("ETW: short",
               !ParseEventPayloadOpt("1234567", 7, nullptr, nullptr));

    // Benchmark: mixed classify cycle.
    constexpr int kBenchIters = 1000000;
    auto start = Clock::now();
    unsigned acc = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        const int m = i & 3;
        const unsigned char* g = (m == 3) ? other : ((m == 2) ? udp : tcp);
        acc += static_cast<unsigned>(
            ClassifyEventOpt(g, 0, static_cast<uint16_t>(10 + m)));
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (acc=%u)\n",
           kBenchIters, avgNs, acc);
}

// ============================================================================
// Test 29/30: JSON + CSV Escape (P2 #21/#23)
// ============================================================================

void TestJsonCsv() {
    printf("\n=== JSON + CSV Escape ===\n");

    ReportTest("JSON: empty", JsonEscapeOpt(L"").empty());
    ReportTest("JSON: plain", JsonEscapeOpt(L"chrome.exe") == "chrome.exe");
    ReportTest("JSON: quotes", JsonEscapeOpt(L"a\"b") == "a\\\"b");
    ReportTest("JSON: backslash", JsonEscapeOpt(L"a\\b") == "a\\\\b");
    ReportTest("JSON: controls",
               JsonEscapeOpt(L"a\nb\rc\td") == "a\\nb\\rc\\td");
    ReportTest("JSON: u0001", JsonEscapeOpt(L"a\x01" L"b") == "a\\u0001b");
    ReportTest("JSON: u001f", JsonEscapeOpt(L"\x1F") == "\\u001f");
    {
        const std::string got = JsonEscapeOpt(L"caf\xE9");
        ReportTest("JSON: passthrough",
                   got == std::string("caf\xC3\xA9"));
    }

    ReportTest("CSV: empty", CsvEscapeOpt("").empty());
    ReportTest("CSV: plain", CsvEscapeOpt("abc") == "abc");
    ReportTest("CSV: comma", CsvEscapeOpt("a,b") == "\"a,b\"");
    ReportTest("CSV: quote", CsvEscapeOpt("a\"b") == "\"a\"\"b\"");
    ReportTest("CSV: crlf", CsvEscapeOpt("a\r\nb") == "\"a\r\nb\"");
    // ---- the SIMD block: fields longer than 16 bytes ----
    // Every input above is under 16 bytes, so none of them ever entered the
    // SSE2 loop and a bug there passed 420/420 in the A/B bench.
    // `list --format csv` on a real service name found it:
    // "RpcEptMapper, RpcSs" (21 bytes, needs quoting) came out as "cSs",
    // because the clean 16-byte block was consumed but never emitted.
    ReportTest("CSV: comma past the first block",
               CsvEscapeOpt("RpcEptMapper, RpcSs") == "\"RpcEptMapper, RpcSs\"");
    ReportTest("CSV: exactly 16 bytes",
               CsvEscapeOpt("aaaaaaaaaaaaaaa,b") == "\"aaaaaaaaaaaaaaa,b\"");
    ReportTest("CSV: 17 bytes",
               CsvEscapeOpt("aaaaaaaaaaaaaaaa,b") == "\"aaaaaaaaaaaaaaaa,b\"");
    ReportTest("CSV: 32 bytes",
               CsvEscapeOpt("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,b") ==
                   "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,b\"");
    ReportTest("CSV: 33 bytes",
               CsvEscapeOpt("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,b") ==
                   "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,b\"");
    ReportTest("CSV: 40-byte plain field never quotes",
               CsvEscapeOpt(std::string(40, 'a')) == std::string(40, 'a'));
    ReportTest("CSV: quote in the second block",
               CsvEscapeOpt("0123456789abcdef\"ghij") ==
                   "\"0123456789abcdef\"\"ghij\"");
    ReportTest("CSV: quotes across several blocks",
               CsvEscapeOpt("0123456789abc\"efghij\"mnopqrs\"tuvwxyz") ==
                   "\"0123456789abc\"\"efghij\"\"mnopqrs\"\"tuvwxyz\"");
    ReportTest("CSV: trailing comma after blocks",
               CsvEscapeOpt(std::string(40, 'a') + ",") ==
                   "\"" + std::string(40, 'a') + ",\"");
    ReportTest("CSV: crlf after the first block",
               CsvEscapeOpt(std::string(40, 'a') + "\r\n") ==
                   "\"" + std::string(40, 'a') + "\r\n\"");

    // Benchmarks: JSON export row + quoted CSV field.
    const std::wstring row =
        L"path: \"C:\\Temp\\caf\xE9.txt\"\nline\x01";
    constexpr int kBenchIters = 50000;
    auto start = Clock::now();
    size_t sink = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        sink += JsonEscapeOpt(row).size();
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (sink=%zu)\n",
           kBenchIters, avgNs, sink);

    const std::string quoted = "note: \"vendor api\", done\r\nok";
    auto start2 = Clock::now();
    size_t sink2 = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        sink2 += CsvEscapeOpt(quoted).size();
    }
    auto end2 = Clock::now();
    auto elapsed2 = std::chrono::duration_cast<Nanoseconds>(end2 - start2);
    double avgNs2 = static_cast<double>(elapsed2.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (sink=%zu)\n",
           kBenchIters, avgNs2, sink2);
}

// ============================================================================
// Test 31/32: Small-Int + IP Formatting (P2 #24/#25)
// ============================================================================

void TestIntIpFmt() {
    printf("\n=== Small-Int + IP Formatting ===\n");

    auto port = [](unsigned p) {
        wchar_t buf[16] = {0};
        const size_t n = FormatPortOpt(buf, 16, p);
        return std::wstring(buf, n);
    };
    ReportTest("Port: 443", port(443) == L"443");
    ReportTest("Port: max", port(65535) == L"65535");
    ReportTest("Port: uint max", port(4294967295u) == L"4294967295");
    {
        wchar_t tiny[2] = {0};
        ReportTest("Port: truncates",
                   FormatPortOpt(tiny, 2, 8080) == 0 &&
                       tiny[0] == L'\0');
    }

    auto u64 = [](uint64_t v) {
        char buf[32] = {0};
        const size_t n = FormatU64DecOpt(buf, 32, v);
        return std::string(buf, n);
    };
    ReportTest("U64: max",
               u64(18446744073709551615ull) == "18446744073709551615");
    ReportTest("U64: 1e9", u64(1000000000) == "1000000000");
    ReportTest("U64: 1e9-1", u64(999999999) == "999999999");

    auto dur = [](uint64_t s) {
        wchar_t buf[32] = {0};
        const size_t n = FormatDurationOpt(buf, 32, s);
        return std::wstring(buf, n);
    };
    ReportTest("Dur: 0s", dur(0) == L"0s");
    ReportTest("Dur: 60s", dur(60) == L"1m 00s");
    ReportTest("Dur: 3661s", dur(3661) == L"1h 1m");
    ReportTest("Dur: 86400s", dur(86400) == L"1d 0h");

    auto v4 = [](unsigned a, unsigned b, unsigned c, unsigned d) {
        const unsigned char addr[4] = {
            static_cast<unsigned char>(a), static_cast<unsigned char>(b),
            static_cast<unsigned char>(c), static_cast<unsigned char>(d)};
        return FormatIpv4Opt(addr);
    };
    ReportTest("IPv4: quad", v4(192, 168, 1, 1) == L"192.168.1.1");
    ReportTest("IPv4: max", v4(255, 255, 255, 255) == L"255.255.255.255");

    auto v6 = [](std::initializer_list<unsigned> w) {
        unsigned char addr[16] = {0};
        int i = 0;
        for (unsigned x : w) {
            addr[i++] = static_cast<unsigned char>(x >> 8);
            addr[i++] = static_cast<unsigned char>(x & 0xFF);
        }
        return FormatIpv6Opt(addr);
    };
    ReportTest("IPv6: loopback",
               v6({0, 0, 0, 0, 0, 0, 0, 1}) == L"::1");
    ReportTest("IPv6: full",
               v6({0x2001, 0x0DB8, 0x1234, 0x5678, 0x9ABC, 0xDEF0,
                   0x1234, 0x5678}) == L"2001:db8:1234:5678:9abc:def0:"
                                        L"1234:5678");
    ReportTest("IPv6: single kept",
               v6({0x2001, 0x0DB8, 0, 1, 1, 1, 1, 1}) ==
                   L"2001:db8:0:1:1:1:1:1");
    ReportTest("IPv6: mapped",
               v6({0, 0, 0, 0, 0, 0xFFFF, 0xC000, 0x0201}) ==
                   L"::ffff:192.0.2.1");

    // Benchmarks: port + full IPv6.
    constexpr int kBenchIters = 200000;
    wchar_t wbuf[32] = {0};
    auto start = Clock::now();
    size_t acc = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        acc += FormatPortOpt(wbuf, 32, 443 + (i % 1000));
    }
    auto end = Clock::now();
    auto elapsed = std::chrono::duration_cast<Nanoseconds>(end - start);
    double avgNs = static_cast<double>(elapsed.count()) / kBenchIters;
    printf("  Benchmark: %d iterations, %.1f ns/op (acc=%zu)\n",
           kBenchIters, avgNs, acc);
}

// ---------------------------------------------------------------------------
// Reference transcriptions of the originals, for the candidates that have
// no literal expectation (Commands.cpp:270-291/422-442, GeoIp.cpp:492-511).
// ---------------------------------------------------------------------------

static size_t ReferenceCpWidth(uint32_t cp) {
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

static bool ReferenceReadBytes(const unsigned char* p, size_t n, uint64_t* v) {
    *v = 0;
    for (size_t i = 0; i < n; ++i) *v = (*v << 8) | p[i];
    return true;
}

static bool ReferencePayloadSize(const unsigned char* data, size_t size,
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
    ReferenceReadBytes(data + pos, extra, &v);
    out->size = static_cast<uint32_t>((s == 30 ? 285u : 65821u) + v);
    out->pos = pos + extra;
    return true;
}

static bool ReferenceReadPointer(const unsigned char* data, size_t size,
                                 unsigned char ctrl, size_t pos, size_t* out) {
    const uint8_t psz = static_cast<uint8_t>((ctrl >> 3) & 0x3u);
    const size_t payload = static_cast<size_t>(psz) + 1;
    if (pos + payload > size) return false;
    uint64_t v = 0;
    ReferenceReadBytes(data + pos, payload, &v);
    if (psz != 3) {
        v |= (static_cast<uint64_t>(ctrl) & 0x7u) << (8 * payload);
        if (psz == 1) v += 2048;
        else if (psz == 2) v += 526336;
    }
    *out = static_cast<size_t>(v);
    return true;
}

// ---------------------------------------------------------------------------
// Pcapng.cpp:354-361 flow-record scan, for the differential sweep.
// ---------------------------------------------------------------------------

static bool FlowProbeRefHere(const unsigned char* pkt, size_t capLen) {
    for (size_t k = 12; k + 5 <= capLen && k < 40; k += 2) {
        if (pkt[k] == 0x08 && pkt[k + 1] == 0x00 &&
            (pkt[k + 2] >> 4) == 4) {
            return true;
        }
    }
    return false;
}

void TestVlanFlow() {
    printf("\n=== VLAN Skip + Flow Probe ===\n");

    auto vlan = [](const std::vector<unsigned char>& b, size_t off) {
        return SkipVlanOpt(b.data(), b.size(), off);
    };
    const unsigned char kUntagged[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        0x08, 0x00, 0x45, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const unsigned char kTag8100[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        0x81, 0x00, 0x00, 0x64, 0x08, 0x00, 0x45, 0};
    const unsigned char kTag88A8[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        0x88, 0xA8, 0x00, 0x64, 0x08, 0x00, 0x45, 0};
    const unsigned char kQinQ[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        0x91, 0x00, 0x00, 0x0C, 0x81, 0x00, 0x00, 0x64,
        0x08, 0x00, 0x45, 0};
    const unsigned char kTrunc[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x81, 0x00};
    auto vec = [](const unsigned char* p, size_t n) {
        return std::vector<unsigned char>(p, p + n);
    };
    ReportTest("Vlan: untagged 12 -> 14",
               vlan(vec(kUntagged, sizeof(kUntagged)), 12) == 14);
    ReportTest("Vlan: 802.1Q 12 -> 18",
               vlan(vec(kTag8100, sizeof(kTag8100)), 12) == 18);
    ReportTest("Vlan: 802.1ad 12 -> 18",
               vlan(vec(kTag88A8, sizeof(kTag88A8)), 12) == 18);
    ReportTest("Vlan: QinQ 12 -> 22",
               vlan(vec(kQinQ, sizeof(kQinQ)), 12) == 22);
    ReportTest("Vlan: truncated chain stops at the bound",
               vlan(vec(kTrunc, sizeof(kTrunc)), 12) == 14);
    ReportTest("Vlan: 0x0800 is not a tag",
               vlan(std::vector<unsigned char>(40, 0x08), 12) == 14);

    // FlowProbe: every capLen x candidate offset x version, which is what
    // pins the SIMD block's stride-2 mask, its bound and its window end.
    size_t bad = 0;
    std::string detail;
    for (size_t capLen = 0; capLen <= 80; ++capLen) {
        for (size_t at = 10; at < 48; ++at) {
            for (int ver = 4; ver <= 5; ++ver) {
                std::vector<unsigned char> b(
                    capLen != 0 ? capLen : 1, 0);
                uint64_t rng = 100u + static_cast<unsigned>(capLen * 7 + at);
                for (auto& x : b) x = (rng = rng * 6364136223846793005ull +
                                              1442695040888963407ull,
                                       static_cast<unsigned char>(rng >> 33));
                if (at + 2 < b.size()) {
                    b[at] = 0x08;
                    b[at + 1] = 0x00;
                    b[at + 2] = static_cast<unsigned char>(ver << 4);
                }
                if (FlowProbeOpt(b.data(), b.size()) !=
                    FlowProbeRefHere(b.data(), b.size())) {
                    ++bad;
                    if (detail.size() < 120) {
                        detail += " cap=" + std::to_string(capLen) +
                                  " at=" + std::to_string(at);
                    }
                }
            }
        }
    }
    ReportTest("Flow: differential sweep (81 x 38 x 2 frames)", bad == 0,
               "mismatch=" + std::to_string(bad) + " " + detail);
    {
        std::vector<unsigned char> b(64, 0);
        b[12] = 0x08;
        b[13] = 0x00;
        b[14] = 0x45;
        ReportTest("Flow: signature at candidate 12",
                   FlowProbeOpt(b.data(), b.size()));
        b[12] = 0x08;
        b[13] = 0x01;
        ReportTest("Flow: 0x0801 is not a signature",
                   !FlowProbeOpt(b.data(), b.size()));
        b[12] = 0;
        b[13] = 0;
        b[14] = 0x65;
        ReportTest("Flow: version 6 is not a signature",
                   !FlowProbeOpt(b.data(), b.size()));
        ReportTest("Flow: empty frame", !FlowProbeOpt(b.data(), 0));
        ReportTest("Flow: candidate 38 needs 5 bytes",
                   FlowProbeOpt(b.data(), 43) ==
                       FlowProbeRefHere(b.data(), 43));
    }

    // The shift-register walker (ResolveOffsetShiftOpt) is pinned against
    // ResolveOffsetOpt by the differential sweep in wintcp_benchmark.cpp
    // (ShiftWalkTests), which builds the same variety of trees the harness
    // measures - six tree shapes x 200 random walks each.

    constexpr int kBenchIters = 200000;
    std::vector<unsigned char> eth(128, 0);
    for (size_t i = 0; i < eth.size(); ++i) {
        eth[i] = static_cast<unsigned char>((i * 13) & 0xFF);
    }
    eth[12] = 0x08;
    eth[13] = 0x00;
    eth[14] = 0x45;
    auto start = Clock::now();
    size_t acc = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        acc += SkipVlanOpt(eth.data(), eth.size(), 12);
    }
    auto end = Clock::now();
    printf("  Benchmark: SkipVlan %.1f ns/op (acc=%zu)\n",
           static_cast<double>(
               std::chrono::duration_cast<Nanoseconds>(end - start).count()) /
               kBenchIters,
           acc);
    start = Clock::now();
    long hits = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        if (FlowProbeOpt(eth.data(), eth.size())) ++hits;
    }
    end = Clock::now();
    printf("  Benchmark: FlowProbe %.1f ns/op (hits=%ld)\n",
           static_cast<double>(
               std::chrono::duration_cast<Nanoseconds>(end - start).count()) /
               kBenchIters,
           hits);
}

// ---------------------------------------------------------------------------
// Snapshot.cpp:37-45 DistinctPids and the ConnectionStore.cpp:1599 pidRows_
// fill, transcribed as references.
// ---------------------------------------------------------------------------

static std::vector<uint32_t> DistinctPidsRefHere(const uint32_t* pids,
                                                 size_t n) {
    std::vector<uint32_t> out;
    if (pids == nullptr || n == 0) return out;
    out.reserve(n);
    std::unordered_set<uint32_t> seen;
    seen.reserve(n);
    for (size_t i = 0; i < n; ++i)
        if (seen.insert(pids[i]).second) out.push_back(pids[i]);
    return out;
}

// A local LCG so the differential sweeps are reproducible. (The host
// project's NextRand lives in the bench harness, which is not linked here.)
static uint64_t NextRandLocal(uint64_t& state) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state >> 33;
}

void TestRenderPids() {
    printf("\n=== TCP Render + PID Dedup ===\n");

    // ---- RenderSegmentsOpt ----
    {
        ReasmRenderView v;
        v.seqs = {100, 105, 111};
        v.starts = {0, 5, 11};
        v.sizes = {5, 6, 1};
        v.pool = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o',
                  'r', 'l', 'd', '!'};
        ReasmRenderResult got;
        RenderSegmentsOpt(v, 3, false, &got);
        ReportTest("Render: contiguous output in seq order",
                   got.bytes.size() == 12 && got.firstSeq == 100 &&
                       !got.hasGap && got.bytesMissing == 0,
                   "size=" + std::to_string(got.bytes.size()));
        bool match = got.bytes == std::vector<unsigned char>(
                                     {'h', 'e', 'l', 'l', 'o', ' ', 'w',
                                      'o', 'r', 'l', 'd', '!'});
        ReportTest("Render: bytes are the payloads concatenated", match);
    }
    {
        // Capture order is reversed; the render must sort.
        ReasmRenderView v;
        v.seqs = {111, 100, 105};
        v.starts = {11, 0, 5};
        v.sizes = {1, 5, 6};
        v.pool = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o',
                  'r', 'l', 'd', '!'};
        ReasmRenderResult got;
        RenderSegmentsOpt(v, 0, true, &got);
        ReportTest("Render: sorts a reversed capture",
                   got.firstSeq == 100 && got.truncated &&
                       got.bytes.size() == 12,
                   "");
    }
    {
        // A hole is recorded, not spliced away.
        ReasmRenderView v;
        v.seqs = {100, 200};
        v.starts = {0, 0};
        v.sizes = {5, 5};
        v.pool = {'h', 'e', 'l', 'l', 'o', 'X', 'X', 'X', 'X', 'X'};
        ReasmRenderResult got;
        RenderSegmentsOpt(v, 7, false, &got);
        ReportTest("Render: gap is recorded, not spliced",
                   got.hasGap && got.bytesMissing == 95 &&
                       got.bytes.size() == 10 && got.duplicates == 7,
                   "missing=" + std::to_string(got.bytesMissing));
    }
    {
        // Zero-length segments contribute nothing.
        ReasmRenderView v;
        v.seqs = {100, 100, 100};
        v.starts = {0, 0, 0};
        v.sizes = {5, 0, 3};
        v.pool.assign(8, 'y');
        ReasmRenderResult got;
        RenderSegmentsOpt(v, 0, false, &got);
        ReportTest("Render: zero-length segments skipped",
                   got.bytes.size() == 8 && !got.hasGap,
                   "bytes=" + std::to_string(got.bytes.size()));
    }
    {
        // Empty: nothing written, only the flags carried.
        ReasmRenderView v;
        ReasmRenderResult got;
        RenderSegmentsOpt(v, 4, true, &got);
        ReportTest("Render: empty carries only the flags",
                   got.segments == 0 && got.bytes.empty() &&
                       got.duplicates == 4 && got.truncated,
                   "");
    }
    {
        // Differential sweep across the sort sizes, against a transcription
        // of TcpReasm.cpp:162-194 over the same logical segments.
        size_t bad = 0;
        std::string detail;
        for (size_t count : {1u, 2u, 8u, 64u, 95u, 96u, 97u, 128u, 300u,
                             1000u}) {
            std::vector<uint64_t> seqs;
            std::vector<unsigned char> pool;
            std::vector<size_t> starts;
            std::vector<size_t> sizes;
            uint64_t rng = 40000 + count;
            uint64_t seq = 1000;
            for (size_t i = 0; i < count; ++i) {
                const size_t len = NextRandLocal(rng) % 64;
                if (NextRandLocal(rng) % 5 == 0) seq += NextRandLocal(rng) % 100;
                seqs.push_back(seq);
                starts.push_back(pool.size());
                sizes.push_back(len);
                for (size_t k = 0; k < len; ++k) {
                    pool.push_back(
                        static_cast<unsigned char>('a' + (NextRandLocal(rng) % 26)));
                }
                seq += len;
            }
            ReasmRenderView v;
            v.seqs = seqs;
            v.starts = starts;
            v.sizes = sizes;
            v.pool = pool;
            ReasmRenderResult got;
            RenderSegmentsOpt(v, count / 3, false, &got);
            // Reference: the original's algorithm, transcribed.
            std::vector<uint32_t> order(count);
            for (size_t i = 0; i < count; ++i) order[i] = (uint32_t)i;
            std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                return seqs[a] < seqs[b];
            });
            std::vector<unsigned char> want;
            uint64_t missing = 0;
            bool gap = false;
            for (size_t i = 0; i < count; ++i) {
                if (i > 0) {
                    const uint64_t prevEnd =
                        seqs[order[i - 1]] + sizes[order[i - 1]];
                    if (seqs[order[i]] > prevEnd) {
                        missing += seqs[order[i]] - prevEnd;
                        gap = true;
                    }
                }
                want.insert(want.end(), pool.begin() + starts[order[i]],
                            pool.begin() + starts[order[i]] + sizes[order[i]]);
            }
            if (got.bytes != want || got.bytesMissing != missing ||
                got.hasGap != gap) {
                ++bad;
                if (detail.size() < 100) detail += " n=" + std::to_string(count);
            }
        }
        ReportTest("Render: differential sweep (10 sizes)", bad == 0,
                   "mismatch=" + std::to_string(bad) + detail);
    }

    // ---- DistinctPidsOpt ----
    {
        const uint32_t one[] = {42};
        const std::vector<uint32_t> r1 = DistinctPidsOpt(one, 1);
        ReportTest("Pids: single", r1.size() == 1 && r1[0] == 42);
        uint32_t dup[] = {7, 7, 7, 7, 7};
        const std::vector<uint32_t> r2 = DistinctPidsOpt(dup, 5);
        ReportTest("Pids: all duplicates collapse",
                   r2.size() == 1 && r2[0] == 7);
        uint32_t order[] = {9, 4, 9, 1, 4, 9, 1};
        const std::vector<uint32_t> r3 = DistinctPidsOpt(order, 7);
        const std::vector<uint32_t> want3 = {9, 4, 1};
        ReportTest("Pids: first-appearance order preserved", r3 == want3,
                   "got " + std::to_string(r3.size()));
        uint32_t edges[] = {0, 5, 0, 4294967295u, 5};
        const std::vector<uint32_t> r4 = DistinctPidsOpt(edges, 5);
        ReportTest("Pids: 0 and 0xFFFFFFFF are real keys",
                   r4 == std::vector<uint32_t>({0, 5, 4294967295u}),
                   "got " + std::to_string(r4.size()));
        // A browser with 2000 sockets and 12 processes.
        std::vector<uint32_t> browser;
        uint64_t rng = 5150;
        for (int i = 0; i < 2000; ++i) {
            browser.push_back(
                static_cast<uint32_t>(1000 + (NextRandLocal(rng) % 12)));
        }
        const std::vector<uint32_t> r5 =
            DistinctPidsOpt(browser.data(), browser.size());
        const std::vector<uint32_t> w5 =
            DistinctPidsRefHere(browser.data(), browser.size());
        ReportTest("Pids: 2000 sockets x 12 processes -> 12",
                   r5.size() == 12 && r5 == w5,
                   "got " + std::to_string(r5.size()));
    }
    {
        // Differential sweep: length x multiplicity, including 0 and max.
        size_t bad = 0;
        std::string detail;
        for (unsigned seed = 0; seed < 12; ++seed) {
            uint64_t rng = 60000 + seed;
            const size_t n = NextRandLocal(rng) % 500;
            std::vector<uint32_t> in(n);
            for (size_t i = 0; i < n; ++i) {
                const uint32_t roll = static_cast<uint32_t>(NextRandLocal(rng) % 8);
                in[i] = (roll == 0)    ? 0u
                        : (roll == 1)  ? 4294967295u
                                       : static_cast<uint32_t>(
                                             NextRandLocal(rng) % 40u);
            }
            if (DistinctPidsOpt(in.data(), in.size()) !=
                DistinctPidsRefHere(in.data(), in.size())) {
                ++bad;
                if (detail.size() < 100) detail += " s=" + std::to_string(seed);
            }
        }
        ReportTest("Pids: differential sweep (12 shapes)", bad == 0,
                   "mismatch=" + std::to_string(bad) + detail);
    }
    {
        // GroupByPidOpt.
        uint32_t in[] = {9, 4, 9, 1, 4, 9, 1};
        const PidGroups g = GroupByPidOpt(in, 7);
        const bool okPids = g.pids == std::vector<uint32_t>({9, 4, 1});
        const bool okRows =
            g.rows.size() == 3 &&
            g.rows[0] == std::vector<size_t>({0, 2, 5}) &&
            g.rows[1] == std::vector<size_t>({1, 4}) &&
            g.rows[2] == std::vector<size_t>({3, 6});
        ReportTest("Group: pids in first-appearance order", okPids,
                   std::to_string(g.pids.size()));
        ReportTest("Group: row indices per pid", okRows, "");
        size_t bad = 0;
        for (unsigned seed = 0; seed < 8; ++seed) {
            uint64_t rng = 70000 + seed;
            const size_t n = NextRandLocal(rng) % 400;
            std::vector<uint32_t> v(n);
            for (size_t i = 0; i < n; ++i) {
                v[i] = static_cast<uint32_t>(NextRandLocal(rng) % 17u);
            }
            const PidGroups got = GroupByPidOpt(v.data(), v.size());
            // Reference: first-appearance order + row order, both explicit.
            std::vector<uint32_t> pids;
            std::vector<std::vector<size_t>> rows;
            for (size_t i = 0; i < n; ++i) {
                size_t at = 0;
                for (; at < pids.size(); ++at) {
                    if (pids[at] == v[i]) break;
                }
                if (at == pids.size()) {
                    pids.push_back(v[i]);
                    rows.emplace_back();
                }
                rows[at].push_back(i);
            }
            if (got.pids != pids || got.rows != rows) ++bad;
        }
        ReportTest("Group: differential sweep (8 shapes)", bad == 0,
                   "mismatch=" + std::to_string(bad));
    }

    constexpr int kBenchIters = 2000;
    auto start = Clock::now();
    size_t acc = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        ReasmRenderView v;
        v.seqs = {111, 100, 105};
        v.starts = {11, 0, 5};
        v.sizes = {1, 5, 6};
        v.pool = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o',
                  'r', 'l', 'd', '!'};
        ReasmRenderResult out;
        RenderSegmentsOpt(v, 0, false, &out);
        acc += out.bytes.size();
    }
    auto end = Clock::now();
    printf("  Benchmark: RenderSegments %.1f ns/op (acc=%zu)\n",
           static_cast<double>(
               std::chrono::duration_cast<Nanoseconds>(end - start).count()) /
               kBenchIters,
           acc);
    std::vector<uint32_t> browser;
    uint64_t rng = 5150;
    for (int i = 0; i < 2000; ++i) {
        browser.push_back(
            static_cast<uint32_t>(1000 + (NextRandLocal(rng) % 12)));
    }
    start = Clock::now();
    acc = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        acc += DistinctPidsOpt(browser.data(), browser.size()).size();
    }
    end = Clock::now();
    printf("  Benchmark: DistinctPids %.1f ns/op (acc=%zu)\n",
           static_cast<double>(
               std::chrono::duration_cast<Nanoseconds>(end - start).count()) /
               kBenchIters,
           acc);
}

void TestWidthMmdb() {
    printf("\n=== Display Width + MMDB Reads ===\n");
    // CpWidth: every range boundary and the gaps between them.
    static const uint32_t kProbe[] = {
        0x0000, 0x02FF, 0x0300, 0x036F, 0x0370, 0x10FF, 0x1100, 0x115F,
        0x1160, 0x2E7F, 0x2E80, 0x2FFF, 0x3000, 0x303E, 0x303F, 0x3041,
        0x33FF, 0x3400, 0x4DBF, 0x4E00, 0x9FFF, 0xA4CF, 0xA960, 0xA97F,
        0xAC00, 0xD7A3, 0xF900, 0xFAFF, 0xFE30, 0xFE6F, 0xFF00, 0xFF60,
        0xFFE0, 0xFFE6, 0x1F300, 0x1FAFF, 0x20000, 0x3FFFD, 0x40000,
    };
    size_t bad = 0;
    for (uint32_t cp : kProbe) {
        const size_t want = ReferenceCpWidth(cp);
        if (CpWidthOpt(cp) != want) {
            printf("  CpWidth mismatch at U+%04X: got %zu want %zu\n", cp,
                   CpWidthOpt(cp), want);
            ++bad;
        }
    }
    ReportTest("CpWidth: all range boundaries", bad == 0);
    // Exhaustive over the tabled range and a coarse sweep above it.
    bad = 0;
    for (uint32_t cp = 0; cp < 0x3000 && bad == 0; ++cp) {
        if (CpWidthOpt(cp) != ReferenceCpWidth(cp)) {
            printf("  CpWidth mismatch at U+%04X\n", cp);
            ++bad;
        }
    }
    for (uint32_t cp = 0x3000; cp < 0x110000 && bad == 0; cp += 7) {
        if (CpWidthOpt(cp) != ReferenceCpWidth(cp)) ++bad;
    }
    ReportTest("CpWidth: exhaustive + coarse sweep", bad == 0);

    auto width = [](const std::string& s) { return DisplayWidthOpt(s); };
    ReportTest("Width: ascii == length", width("chrome.exe") == 10);
    ReportTest("Width: empty", width("") == 0);
    ReportTest("Width: long ascii", width(std::string(1000, 'a')) == 1000);
    ReportTest("Width: 2-byte", width("caf\xC3\xA9") == 4);
    ReportTest("Width: 4-byte emoji", width("\xF0\x9F\x98\x80") == 2);
    ReportTest("Width: combining", width("\xCC\x81") == 0);
    ReportTest("Width: cjk", width("\xE4\xB8\xAD") == 2);
    {
        // A codepoint split across a 16-byte block boundary.
        std::string s;
        for (int i = 0; i < 4; ++i) s += "abcd";
        s += "abc";
        s += "\xE4\xB8\xAD";
        s += "de";
        ReportTest("Width: split codepoint", width(s) == 23,
                   std::to_string(width(s)));
    }

    auto trunc = [](const std::string& s, size_t w) {
        return TruncateToWidthOpt(s, w);
    };
    ReportTest("Trunc: fits unchanged",
               trunc("chrome.exe", 32) == "chrome.exe");
    ReportTest("Trunc: exact fit unchanged",
               trunc("chrome.exe", 10) == "chrome.exe");
    ReportTest("Trunc: one short adds marker",
               trunc("chrome.exe", 9) == "chrome.e\xE2\x80\xA6");
    ReportTest("Trunc: width 6",
               trunc("chrome.exe", 6) == "chrom\xE2\x80\xA6");
    ReportTest("Trunc: width 1", trunc("chrome.exe", 1) == "\xE2\x80\xA6");
    ReportTest("Trunc: width 0 empty", trunc("chrome.exe", 0).empty());
    ReportTest("Trunc: never splits a codepoint",
               trunc("\xE4\xB8\xAD\xE6\x96\x87", 3) ==
                   std::string("\xE4\xB8\xAD\xE2\x80\xA6"));

    // ---- MMDB payload size / pointer ----
    unsigned char sec[64] = {0};
    for (int i = 0; i < 64; ++i) sec[i] = static_cast<unsigned char>(i);
    size_t mmdbBad = 0;
    for (unsigned code = 0; code < 32; ++code) {
        MmdbPayload got;
        MmdbPayload want;
        const bool okg = PayloadSizeOpt(sec, 64,
                                        static_cast<unsigned char>(code), 16,
                                        &got);
        const bool okw = ReferencePayloadSize(sec, 64,
                                              static_cast<unsigned char>(code),
                                              16, &want);
        if (okg != okw || got.size != want.size || got.pos != want.pos) {
            printf("  PayloadSize mismatch at code %u\n", code);
            ++mmdbBad;
        }
    }
    ReportTest("Mmdb: PayloadSize all 32 codes", mmdbBad == 0);
    mmdbBad = 0;
    for (unsigned psz = 0; psz < 4; ++psz) {
        for (size_t pos = 8; pos < 32; pos += 7) {
            const unsigned char ctrl =
                static_cast<unsigned char>((psz << 3) | 0x05u);
            size_t got = 0, want = 0;
            const bool okg = ReadPointerOpt(sec, 64, ctrl, pos, &got);
            const bool okw =
                ReferenceReadPointer(sec, 64, ctrl, pos, &want);
            if (okg != okw || (okg && got != want)) {
                printf("  ReadPointer mismatch at psz %u pos %zu\n", psz, pos);
                ++mmdbBad;
            }
        }
    }
    ReportTest("Mmdb: ReadPointer all sizes x positions", mmdbBad == 0);
    // Boundary sweep: the 4-byte fast path vs the byte-loop fallback.
    mmdbBad = 0;
    for (size_t sz = 56; sz <= 64; ++sz) {
        for (size_t pos = 52; pos < 64; ++pos) {
            for (unsigned psz = 0; psz < 4; ++psz) {
                const unsigned char ctrl =
                    static_cast<unsigned char>((psz << 3) | 0x03u);
                size_t got = 0, want = 0;
                const bool okg = ReadPointerOpt(sec, sz, ctrl, pos, &got);
                const bool okw = ReferenceReadPointer(sec, sz, ctrl, pos,
                                                      &want);
                if (okg != okw || (okg && got != want)) ++mmdbBad;
            }
        }
    }
    ReportTest("Mmdb: ReadPointer boundary sweep", mmdbBad == 0,
               std::to_string(mmdbBad));
    {
        // Size 3 IGNORES the control byte's low three bits.
        size_t out = 0;
        ReadPointerOpt(sec, 64, static_cast<unsigned char>((3u << 3) | 0x07u),
                       32, &out);
        ReportTest("Mmdb: size 3 ignores the low bits",
                   out == ((32u << 24) | (33u << 16) | (34u << 8) | 35u),
                   std::to_string(out));
    }
    {
        // Size 1 adds kPtr2Base 2048.
        size_t out = 0;
        ReadPointerOpt(sec, 64, static_cast<unsigned char>((1u << 3) | 0x01u),
                       48, &out);
        ReportTest("Mmdb: size 1 adds 2048",
                   out == 2048 + ((1u << 16) | (48u << 8) | 49u),
                   std::to_string(out));
    }

    // Benchmarks: width over a realistic cell, and a pointer read.
    const std::string cell =
        "chrome.exe:1234 -> 203.0.113.9:443 established";
    std::string wide;
    for (int i = 0; i < 64; ++i) wide += "abcd\xE4\xB8\xAD";
    constexpr int kBenchIters = 200000;
    auto start = Clock::now();
    size_t acc = 0;
    for (int i = 0; i < kBenchIters; ++i) acc += DisplayWidthOpt(cell);
    auto mid = Clock::now();
    for (int i = 0; i < kBenchIters; ++i) acc += DisplayWidthOpt(wide);
    auto end = Clock::now();
    double asciiNs =
        static_cast<double>(
            std::chrono::duration_cast<Nanoseconds>(mid - start).count()) /
        kBenchIters;
    double wideNs = static_cast<double>(
                        std::chrono::duration_cast<Nanoseconds>(end - mid)
                            .count()) /
                    kBenchIters;
    printf("  Benchmark: DisplayWidth %.1f ns/op (ascii), %.1f ns/op "
           "(mixed, acc=%zu)\n",
           asciiNs, wideNs, acc);
    start = Clock::now();
    acc = 0;
    size_t p = 0;
    for (int i = 0; i < kBenchIters; ++i) {
        ReadPointerOpt(sec, 64, static_cast<unsigned char>((i % 4) << 3), 8,
                       &p);
        acc += p;
    }
    end = Clock::now();
    double ptrNs =
        static_cast<double>(
            std::chrono::duration_cast<Nanoseconds>(end - start).count()) /
        kBenchIters;
    printf("  Benchmark: ReadPointer %.1f ns/op (acc=%zu)\n", ptrNs, acc);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    // Unattended mode: never show a modal dialog (abort/retry/ignore,
    // WER "stopped working"). A crash must exit with a code and leave
    // the stdout/stderr trail behind, so overnight runs never block.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                     SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // swprintf_s truncation is an "invalid parameter": silence the handler
    // so small-buffer probes return -1 instead of a modal dialog.
    _set_invalid_parameter_handler(
        [](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
           uintptr_t) {});
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    printf("=== WinTCP Assembly Optimization Tests ===\n");
    printf("Validating optimized replacements for hot-path functions\n\n");

    TestGeoIpTreeWalk();
    TestTcpReassembly();
    TestFilterMatching();
    TestPacketParsing();
    TestFormatBytes();
    TestConnectionKey();
    TestWideToUtf8();
    TestStreamHex();
    TestToLower();
    TestBeReads();
    TestTlsExtRd();
    TestJoin();
    TestFindCompare();
    TestKeyRates();
    TestBpsWidenUnicast();
    TestPairFind();
    TestFlowKey();
    TestHandlesEtw();
    TestJsonCsv();
    TestIntIpFmt();
    TestWidthMmdb();
    TestVlanFlow();
    TestRenderPids();

    printf("\n=== Summary ===\n");
    printf("Tests passed: %d\n", testsPassed);
    printf("Tests failed: %d\n", testsFailed);
    printf("Total tests:  %d\n", testsPassed + testsFailed);

    return testsFailed > 0 ? 1 : 0;
}
