// orig_functions.cpp
// SPDX-License-Identifier: Apache-2.0
//
// Faithful standalone copies of the seven wintcp C++
// implementations that the assembly-optimized functions in
// opt_functions.cpp are designed to replace. This file IS
// "the copy of the source codes" for the benchmark: the
// bodies are copied verbatim from the wintcp source (only
// member access is mapped onto plain structs, and the
// provenance of every copy is cited below).
//
// The benchmark links this file into bench_orig.exe, so the
// "original" variant is measured with the exact algorithm,
// data layout and formatting of the shipping wintcp code.

#include "bench_api.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <ws2tcpip.h>  // InetNtopW (orig Ipv4/Ipv6 copies)

namespace wintcp {
namespace bench {

// ====================================================================
// 1. GeoIP tree walk
//    Original: GeoIp.cpp:1002 GeoIpDatabase::ResolveOffset
//              GeoIp.cpp:947  GeoIpDatabase::NodeRecord
//              GeoIp.cpp:120  ReadU32BE
//              GeoIp.cpp:46   kSeparatorLen (= 16)
// ====================================================================

// Copy of GeoIp.cpp:120 ReadU32BE.
static uint32_t OrigReadU32BE(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Copy of GeoIp.cpp:947-992 GeoIpDatabase::NodeRecord, with the
// member fields (nodeByteSize_, treeSize_, nodeCount_, record28_,
// recordBytes_, mappedView_) mapped onto the GeoIpTree struct.
static bool OrigNodeRecord(const GeoIpTree& t, size_t node,
                             unsigned half, size_t* out) {
    if (t.nodeByteSize == 0 || t.treeSize == 0) return false;
    if (node >= t.nodeCount) return false;
    const size_t recPos = node * t.nodeByteSize;
    if (recPos + t.nodeByteSize > t.treeSize) return false;

    if (t.record28) {
        // A 28-bit record keeps the top four bits of both halves in
        // the middle byte of the node (GeoIp.cpp:953-970).
        const unsigned char* p = t.view + recPos;
        if (half == 0) {
            *out = (static_cast<size_t>(p[3] >> 4) << 24) |
                   (static_cast<size_t>(p[0]) << 16) |
                   (static_cast<size_t>(p[1]) << 8) |
                   static_cast<size_t>(p[2]);
        } else {
            *out = (static_cast<size_t>(p[3] & 0x0Fu) << 24) |
                   (static_cast<size_t>(p[4]) << 16) |
                   (static_cast<size_t>(p[5]) << 8) |
                   static_cast<size_t>(p[6]);
        }
        return true;
    }
    if (t.recordBytes == 4) {
        *out = OrigReadU32BE(t.view + recPos);
        return true;
    }
    if (t.recordBytes == 3) {
        // A 3-byte record is the 3 bytes of the node starting at the
        // half this bit selects (GeoIp.cpp:976-986).
        *out = (OrigReadU32BE(t.view + recPos + (half ? 3u : 0u)) >> 8) &
               0x00FFFFFFu;
        return true;
    }
    // Unreachable in wintcp; a refusal so a future record size
    // cannot quietly read as one of the others.
    return false;
}

// Copy of GeoIp.cpp:1002-1054 GeoIpDatabase::ResolveOffset.
// Loaded() is dataSectionSize_ != 0 (see the CountryAt comment,
// GeoIp.cpp:1131: "dataSectionSize_ only means 'not loaded'").
bool GeoIpWalk(const unsigned char bits[16], unsigned bitCount,
               size_t startNode, const GeoIpTree* tree, size_t* out) {
    if (tree == nullptr || out == nullptr) return false;
    const GeoIpTree& t = *tree;
    if (t.dataSectionSize == 0 || t.nodeCount == 0 ||
        t.recordBytes == 0) {
        return false;
    }

    const size_t nodeCount = t.nodeCount;
    size_t node = startNode;

    for (unsigned depth = 0; depth < bitCount; ++depth) {
        // NOTE: division and modulo preserved verbatim - the
        // optimization replaces these with shifts and masks.
        const unsigned bit = (bits[depth / 8] >> (7 - (depth % 8))) & 1u;
        size_t record = 0;

        if (!OrigNodeRecord(t, node, bit, &record)) return false;

        // Three cases: below the node count it is a node number;
        // equal to it the database has no data here; and node_count
        // + 16 or above it is a data pointer. The boundary is >=,
        // not >: node_count + 16 is EXACTLY offset 0, and offset 0
        // is the data section's first byte, which is a real record.
        if (record < nodeCount) {
            node = record;
        } else if (record >= nodeCount + 16 /* kSeparatorLen */) {
            const size_t offset = record - nodeCount - 16;
            if (offset >= t.dataSectionSize) return false;
            *out = offset;
            return true;
        } else {
            return false;
        }
    }
    return false;
}

// A second wiring of the same walk, so the harness can A/B the
// shift-register form against the per-bit form on identical trees.
// Semantics are identical to GeoIpWalk (including the 32-bit
// right-half divergence the tree builder deliberately encodes).
bool GeoIpWalkShift(const unsigned char bits[16], unsigned bitCount,
                    size_t startNode, const GeoIpTree* tree, size_t* out) {
    return GeoIpWalk(bits, bitCount, startNode, tree, out);
}

// ====================================================================
// 2. TCP reassembly segment overlap
//    Original: TcpReasm.cpp:91-147 Direction::Add (Pass A + Pass B)
//              TcpReasm.cpp:35  struct Segment { seq; data; }
// ====================================================================

// Copy of the two overlap passes of Direction::Add. The
// stream-cap (kMaxStreamBytes) logic and the push_back of the
// surviving bytes are omitted: they are constant work per call
// and not part of the overlap algorithm being optimized. The
// Segment.data string is reduced to its length, which is all
// the overlap passes read.
bool ReasmOverlap(uint64_t seq, size_t len,
                  const ReasmWorkload* w, size_t* consumed, size_t* skip) {
    if (w == nullptr || consumed == nullptr || skip == nullptr) {
        return false;
    }
    *consumed = 0;
    *skip = len;
    if (len == 0) return false;  // original: if (len == 0) return;

    // Trim against everything already stored, then take the bytes
    // that are genuinely new. With a small number of segments a
    // linear scan is faster and far simpler than an interval tree,
    // and a real capture of one connection has hundreds, not
    // millions. (Comment copied from TcpReasm.cpp:98-107.)
    //
    // 'consumed' tracks how much of the incoming segment has been
    // ruled out. Advancing the *pointer* as well as the count
    // matters: an earlier version trimmed 'skip' but still read
    // from the start of the buffer, which silently returned the
    // already-covered bytes and lost the tail.
    const std::vector<ReasmSeg>& segs = w->aos;
    uint64_t s = seq;
    size_t consumedVal = 0;

    // Pass A: skip the head that an existing segment already covers.
    // The store is not kept sorted (segments arrive in capture
    // order), so every segment is examined rather than breaking on
    // the first one that starts beyond us.
    for (const ReasmSeg& g : segs) {
        const uint64_t gStart = g.seq;
        const uint64_t gEnd = gStart + g.len;
        if (gEnd <= s) continue;                      // entirely before us
        if (gStart >= s + (len - consumedVal)) continue;  // can't overlap
        if (gEnd > s) {                               // overlaps our head
            const uint64_t d = gEnd - s;
            if (d >= len - consumedVal) {             // fully covered
                *consumed = len;
                *skip = 0;
                return false;  // original: ++duplicates_; return;
            }
            consumedVal += static_cast<size_t>(d);
            s = gEnd;
        }
    }

    // Pass B: stop at the first existing segment that starts inside
    // the still-unclaimed tail, since bytes are contiguous from here.
    size_t skipVal = len - consumedVal;
    for (const ReasmSeg& g : segs) {
        const uint64_t gStart = g.seq;
        if (gStart > s && gStart < s + skipVal) {
            skipVal = static_cast<size_t>(gStart - s);
        }
    }
    if (skipVal == 0) {
        *consumed = consumedVal;
        *skip = 0;
        return false;  // original: ++duplicates_; return;
    }

    *consumed = consumedVal;
    *skip = skipVal;
    return true;
}

// ====================================================================
// 3. Filter substring match
//    Original: Utils.cpp:209-213 HasLowerSubstring
// ====================================================================

bool HasLowerSubstring(const std::wstring& haystackLower,
                       const std::wstring& needleLower) {
    if (needleLower.empty()) return true;
    return haystackLower.find(needleLower) != std::wstring::npos;
}

// ====================================================================
// 4. TCP header parsing
//    Original: Pcapng.cpp:115-130 FinishTcp
//              Pcapng.cpp:76-84  Rd32
// ====================================================================

// Copy of Pcapng.cpp:76-84 Rd32.
static uint32_t OrigRd32(const unsigned char* p, bool swap) {
    uint32_t v = static_cast<uint32_t>(p[0]) |
                 (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16) |
                 (static_cast<uint32_t>(p[3]) << 24);
    if (!swap) return v;
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

// Copy of Pcapng.cpp:115-130 FinishTcp. 'out->malformed' in the
// original maps onto out->ok == false here; the field writes are
// identical.
bool ParseTcpHeader(const unsigned char* p, size_t tcpOff, size_t end,
                    TcpHeaderFields* out) {
    if (out == nullptr) return false;
    *out = TcpHeaderFields();
    const size_t avail = end - tcpOff;
    if (avail < 20) return false;  // original: out->malformed = true;
    const size_t dataOff = (static_cast<size_t>(p[tcpOff + 12]) >> 4) * 4;
    if (dataOff < 20 || dataOff > avail) return false;
    out->dataOffOk = true;
    out->srcPort = static_cast<uint16_t>((p[tcpOff] << 8) | p[tcpOff + 1]);
    out->dstPort = static_cast<uint16_t>((p[tcpOff + 2] << 8) | p[tcpOff + 3]);
    out->seq = OrigRd32(p + tcpOff + 4, true);
    out->ack = OrigRd32(p + tcpOff + 8, true);
    out->tcpFlags = p[tcpOff + 13];
    out->window = static_cast<uint16_t>((p[tcpOff + 14] << 8) | p[tcpOff + 15]);
    out->payload = p + tcpOff + dataOff;
    out->payloadLen = avail - dataOff;
    out->ok = true;
    return true;
}

// ====================================================================
// 5. Byte formatting
//    Original: Utils.cpp:192-207 FormatBytes
//              Utils.h:28-31  kBytesPerKB/MB/GB/TB
// ====================================================================

// Copy of Utils.cpp:192-207 FormatBytes. wintcp returns a
// std::wstring; the canonical interface writes into a caller
// buffer (what the optimized version does), so the returned
// std::wstring(buf) construction is paid by BOTH variants and
// does not distort the measurement.
//
// NOTE the precision policy, which the optimized version changes:
// GB and TB use "%.2f" here. See the divergence note in the
// benchmark report.
size_t FormatBytes(wchar_t* buf, size_t bufSize, uint64_t bytes) {
    if (buf == nullptr || bufSize == 0) return 0;
    const double b = static_cast<double>(bytes);
    if (b < 1024.0) {
        const int n = ::swprintf_s(buf, bufSize, L"%llu B",
                                   static_cast<unsigned long long>(bytes));
        return (n < 0) ? 0 : static_cast<size_t>(n);
    } else if (b < 1024.0 * 1024.0) {
        const int n = ::swprintf_s(buf, bufSize, L"%.1f KB",
                                   b / 1024.0);
        return (n < 0) ? 0 : static_cast<size_t>(n);
    } else if (b < 1024.0 * 1024.0 * 1024.0) {
        const int n = ::swprintf_s(buf, bufSize, L"%.1f MB",
                                   b / (1024.0 * 1024.0));
        return (n < 0) ? 0 : static_cast<size_t>(n);
    } else if (b < 1024.0 * 1024.0 * 1024.0 * 1024.0) {
        const int n = ::swprintf_s(buf, bufSize, L"%.2f GB",
                                   b / (1024.0 * 1024.0 * 1024.0));
        return (n < 0) ? 0 : static_cast<size_t>(n);
    } else {
        const int n = ::swprintf_s(buf, bufSize, L"%.2f TB",
                                   b / (1024.0 * 1024.0 * 1024.0 * 1024.0));
        return (n < 0) ? 0 : static_cast<size_t>(n);
    }
}

// ====================================================================
// 6. Connection key building
//    Original: ConnectionStore.cpp:358-390 KeyOf
// ====================================================================

// Copy of ConnectionStore.cpp:358-390 KeyOf. The Connection
// fields it reads are mapped onto ConnKeyInput; the layout written
// is byte-for-byte the one the std::string version wrote (family,
// protocol, addresses, ports, pid), so every pairing decision
// ReplaceSnapshot makes is unchanged. Only local4/remote4 are
// read for IPv4 (ConnectionStore.cpp:380-382: the v6 fields of
// an IPv4 row may hold stale bytes, so including them would
// split one row into two).
void KeyOf(const ConnKeyInput* in, unsigned char* outBytes, size_t* outLen) {
    if (in == nullptr || outBytes == nullptr || outLen == nullptr) {
        if (outLen != nullptr) *outLen = 0;
        return;
    }
    size_t len = 0;
    const auto push = [&outBytes, &len](const void* src, size_t n) {
        std::memcpy(outBytes + len, src, n);
        len += n;
    };

    outBytes[len++] = static_cast<unsigned char>(
        in->ipv6 ? '6' : '4');
    outBytes[len++] = static_cast<unsigned char>(
        in->udp ? 'U' : 'T');
    if (in->ipv6) {
        push(in->localAddr, 16);
        push(&in->localPort, sizeof(uint32_t));   // sizeof(UINT)
        push(in->remoteAddr, 16);
    } else {
        push(in->localAddr, 4);
        push(&in->localPort, sizeof(uint32_t));
        push(in->remoteAddr, 4);
    }
    push(&in->remotePort, sizeof(uint32_t));
    push(&in->pid, sizeof(uint32_t));             // sizeof(DWORD)
    *outLen = len;
}

// ====================================================================
// 7. Wide string to UTF-8
//    Original: Utils.cpp:94-108 WideToUtf8
// ====================================================================

// Copy of Utils.cpp:94-108 WideToUtf8. wintcp returns a
// std::string (resizing as needed); the canonical interface
// writes into a caller buffer. When the conversion needs more
// than dstSize bytes the original would resize - here it
// truncates to dstSize, which is what the optimized version
// does, so the two stay comparable. Benchmarks always pass a
// buffer large enough that the truncation path never runs.
size_t WideToUtf8(const wchar_t* src, size_t srcLen, char* dst,
                  size_t dstSize) {
    if (src == nullptr || dst == nullptr) return 0;
    if (dstSize == 0) return 0;
    if (srcLen == 0) return 0;

    int needed = ::WideCharToMultiByte(CP_UTF8, 0, src,
                                       static_cast<int>(srcLen),
                                       nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return 0;
    if (static_cast<size_t>(needed) > dstSize) {
        // Convert fully, then truncate to the caller's buffer.
        std::string tmp(static_cast<size_t>(needed), '\0');
        int written = ::WideCharToMultiByte(CP_UTF8, 0, src,
                                            static_cast<int>(srcLen),
                                            &tmp[0], needed, nullptr, nullptr);
        if (written <= 0) return 0;
        std::memcpy(dst, tmp.data(), dstSize);
        return dstSize;
    }
    int written = ::WideCharToMultiByte(CP_UTF8, 0, src,
                                        static_cast<int>(srcLen),
                                        dst, needed, nullptr, nullptr);
    if (written <= 0) return 0;
    return static_cast<size_t>(written);
}

// ====================================================================
// 8. Stream hexdump
//    Original: StreamCapture.cpp:578-631 FormatStreamHex
// ====================================================================

// Copy of StreamCapture.cpp:578-631 FormatStreamHex. Verbatim apart
// from the wintcp-namespace constants (spelled out) and std:: min.
std::string FormatStreamHex(const std::string& bytes, size_t bytesPerLine) {
    std::string out;
    constexpr size_t kMaxPerLine = 64;
    if (bytesPerLine == 0 || bytesPerLine > kMaxPerLine) bytesPerLine = 16;
    out.reserve(bytes.size() / bytesPerLine * 80 + 32);

    // Worst case per line: 8 for the offset, 2 spaces, three characters
    // per hex byte, two for the gutter bars, one per ASCII byte, and
    // the CRLF.
    char line[8 + 2 + kMaxPerLine * 3 + 2 + kMaxPerLine + 8];

    const size_t n = bytes.size();
    for (size_t off = 0; off < n; off += bytesPerLine) {
        const size_t run = (std::min)(bytesPerLine, n - off);
        const size_t room = sizeof(line);
        int at = 0;
        at += ::snprintf(line + at, room - static_cast<size_t>(at),
                         "%08zx  ", off);

        for (size_t i = 0; i < bytesPerLine; ++i) {
            if (i == (bytesPerLine / 2) && run > i)
                at += ::snprintf(line + at, room - static_cast<size_t>(at),
                                 " ");
            if (i < run) {
                at += ::snprintf(line + at, room - static_cast<size_t>(at),
                                 "%02x ",
                                 static_cast<unsigned>(
                                     static_cast<unsigned char>(
                                         bytes[off + i])));
            } else {
                at += ::snprintf(line + at, room - static_cast<size_t>(at),
                                 "   ");
            }
        }
        at += ::snprintf(line + at, room - static_cast<size_t>(at), " |");
        for (size_t i = 0; i < run; ++i) {
            const unsigned char c =
                static_cast<unsigned char>(bytes[off + i]);
            const bool printable = (c >= 0x20 && c < 0x7F);
            at += ::snprintf(line + at, room - static_cast<size_t>(at),
                             "%c", printable ? static_cast<char>(c) : '.');
        }
        at += ::snprintf(line + at, room - static_cast<size_t>(at),
                         "|\r\n");
        if (at > 0) out.append(line, static_cast<size_t>(at));
    }
    return out;
}

// ====================================================================
// 9. Wide lowercase
//    Original: Utils.cpp:105-111 ToLowerW
// ====================================================================

// Copy of Utils.cpp:105-111 ToLowerW.
std::wstring ToLowerW(const std::wstring& s) {
    std::wstring out(s);
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(::towlower(static_cast<wint_t>(ch)));
    }
    return out;
}

// ====================================================================
// 10. GeoIP big-endian field reads
//     Original: GeoIp.cpp:124-139
// ====================================================================

// Copy of GeoIp.cpp:124-128 ReadU32BE.
uint32_t GeoReadU32BE(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Copy of GeoIp.cpp:130-132 ReadU64BE.
uint64_t GeoReadU64BE(const unsigned char* p) {
    return (static_cast<uint64_t>(GeoReadU32BE(p)) << 32) |
           GeoReadU32BE(p + 4);
}

// Copy of GeoIp.cpp:134-139 ReadBytes.
bool GeoReadBytes(const unsigned char* p, size_t n, uint64_t* out) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
    *out = v;
    return true;
}

// ====================================================================
// 11. TLS big-endian field reads
//     Original: TlsDecode.cpp:23-36 Be16/Be24/Be32
// ====================================================================

// Copy of TlsDecode.cpp:23-25 Be16.
uint16_t TlsBe16(const unsigned char* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

// Copy of TlsDecode.cpp:27-30 Be24.
uint32_t TlsBe24(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 16) |
           (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

// Copy of TlsDecode.cpp:32-36 Be32.
uint32_t TlsBe32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// ====================================================================
// 12. TLS extension dispatch + SNI tail strip
//     Original: TlsDecode.cpp:227-266 (dispatch at 235/253,
//               strip loop at 247-250)
// ====================================================================

// Copy of the TlsDecode.cpp:235,253 extension-type dispatch.
TlsExtAction ClassifyTlsExtension(uint16_t type) {
    if (type == 0x0000) return TlsExtAction::kSni;   // server_name
    if (type == 0x0010) return TlsExtAction::kAlpn;  // ALPN
    return TlsExtAction::kSkip;
}

// Copy of the TlsDecode.cpp:247-250 SNI trailing-strip loop, as a
// length function (the original pops a std::string; same result).
size_t StripSniTail(char* s, size_t n) {
    size_t len = n;
    while (len > 0 && (s[len - 1] == '\0' || s[len - 1] == '.')) --len;
    return len;
}

// ====================================================================
// 13. Pcap endian loads
//     Original: Pcapng.cpp:90-105 Rd16/Rd32
// ====================================================================

// Copy of Pcapng.cpp:90-95 Rd16.
uint16_t Rd16(const unsigned char* p, bool swap) {
    uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
    return swap ? static_cast<uint16_t>(((v & 0xFF00u) >> 8) |
                                       ((v & 0x00FFu) << 8))
                : v;
}

// Copy of Pcapng.cpp:97-105 Rd32.
uint32_t Rd32(const unsigned char* p, bool swap) {
    uint32_t v = static_cast<uint32_t>(p[0]) |
                 (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16) |
                 (static_cast<uint32_t>(p[3]) << 24);
    if (!swap) return v;
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

// ====================================================================
// 14. Socket-sample join
//     Original: ConnectionStore.cpp:1888-1912 ApplySocketBytes
// ====================================================================

// Copy of the ConnectionStore.cpp:1888-1912 match kernel, mapped onto
// JoinRow/JoinSample (Connection -> row fields, SocketBytes -> sample).
int JoinSamples(std::vector<JoinRow>& rows,
                const std::vector<JoinSample>& samples) {
    if (samples.empty()) return 0;
    int updated = 0;
    std::vector<char> claimed(rows.size(), 0);
    for (const JoinSample& b : samples) {
        if (!b.known) continue;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (claimed[i]) continue;
            JoinRow& r = rows[i];
            if (r.localPort != b.localPort || r.remotePort != b.remotePort)
                continue;
            if (r.local != b.local || r.remote != b.remote) continue;
            if (!r.tcp) continue;  // SIO_TCP_INFO is TCP
            r.rx = b.rx;
            r.tx = b.tx;
            r.perRow = true;
            claimed[i] = 1;
            ++updated;
            break;
        }
    }
    return updated;
}

// ====================================================================
// 15. Long-needle substring
//     Original: ConnectionStore.cpp:87-89 Has
// ====================================================================

// Copy of ConnectionStore.cpp:87-89 Has.
bool FindSubstringLong(const std::wstring& haystack,
                       const std::wstring& needle) {
    return needle.empty() ||
           haystack.find(needle) != std::wstring::npos;
}

// ====================================================================
// 16. Lower-all concat
//     Original: ConnectionStore.cpp:93-98 RebuildLowerAll
// ====================================================================

// Copy of the ConnectionStore.cpp:93-98 +-chain for a fixed 10-field
// row (the production call always joins exactly these 10 fields).
std::wstring BuildLowerAll(const std::wstring* const* f, size_t count) {
    if (f == nullptr || count < 10) return std::wstring();
    return *f[0] + L" " + *f[1] + L" " + *f[2] + L" " + *f[3] + L" " + *f[4] +
           L" " + *f[5] + L" " + *f[6] + L" " + *f[7] + L" " + *f[8] + L" " +
           *f[9];
}

// ====================================================================
// 17. Wide string compare
//     Original: std::wstring::compare (no-alloc view form)
// ====================================================================

// std::wstring::compare over caller buffers without allocating (what
// CompareRows compares: existing row strings, not temporaries).
int CompareWide(const wchar_t* a, size_t na, const wchar_t* b, size_t nb) {
    return std::wstring_view(a, na).compare(std::wstring_view(b, nb));
}

// ====================================================================
// 18. Connection-key hash + equality
//     Original: ConnectionStore.h:255-268
// ====================================================================

// Copy of ConnectionStore.h:255-257 operator==.
bool EqualConnKey(const unsigned char* a, size_t na, const unsigned char* b,
                  size_t nb) {
    if (na != nb) return false;
    if (a == nullptr || b == nullptr) return a == b;
    return std::memcmp(a, b, na) == 0;
}

// Copy of ConnectionStore.h:263-268 ConnectionKeyHash.
size_t HashConnKey(const unsigned char* bytes, size_t len) {
    if (bytes == nullptr) return 0;
    return std::hash<std::string_view>()(std::string_view(
        reinterpret_cast<const char*>(bytes), len));
}

// ====================================================================
// 19. Batch rate computation
//     Original: ConnectionStore.cpp:686-703 ComputeBps
// ====================================================================

// Per-counter copy of ConnectionStore.cpp:686-703 ComputeBps, over a
// uniform-elapsed batch (the common tick: every row shares nowTick).
// Rejects write 0.0 so both variants leave identical buffers.
size_t ComputeBpsBatch(const uint64_t* prevBytes, const uint64_t* nowBytes,
                       uint64_t elapsedMs, double* outBps, size_t n) {
    if (prevBytes == nullptr || nowBytes == nullptr || outBps == nullptr) {
        return 0;
    }
    if (elapsedMs == 0) return 0;
    size_t ok = 0;
    for (size_t i = 0; i < n; ++i) {
        if (nowBytes[i] < prevBytes[i]) {
            outBps[i] = 0.0;
            continue;
        }
        outBps[i] = static_cast<double>(nowBytes[i] - prevBytes[i]) *
                    1000.0 / static_cast<double>(elapsedMs);
        ++ok;
    }
    return ok;
}

// ====================================================================
// 20. Per-PID traffic sums
//     Original: ConnectionStore.cpp:1548-1596 (map sum + write-back)
// ====================================================================

// Copy of the ComputeGroupRates sum/write-back shape, mapped onto
// PidTrafficRow (Connection -> counted/rx/tx, sums -> sumRx/sumTx).
void SumPidTraffic(PidTrafficRow* rows, size_t n) {
    if (rows == nullptr || n == 0) return;
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> sum;
    std::map<uint32_t, bool> anyCounted;
    for (size_t i = 0; i < n; ++i) {
        if (!rows[i].counted) continue;
        auto& t = sum[rows[i].pid];
        const uint64_t rx = t.first + rows[i].rx;  // SatAdd
        t.first = (rx < t.first) ? ~0ULL : rx;
        const uint64_t tx = t.second + rows[i].tx;
        t.second = (tx < t.second) ? ~0ULL : tx;
        anyCounted[rows[i].pid] = true;
    }
    for (size_t i = 0; i < n; ++i) {
        const auto it = sum.find(rows[i].pid);
        const bool counted =
            it != sum.end() && anyCounted[rows[i].pid];
        rows[i].sumRx = counted ? it->second.first : 0;
        rows[i].sumTx = counted ? it->second.second : 0;
        rows[i].hasSum = counted;
    }
}

// ====================================================================
// 21. Rate cell formatting
//     Original: ConnectionStore.cpp:554-565 FormatBpsCell
// ====================================================================

// Copy of ConnectionStore.cpp:554-565 FormatBpsCell, writing into a
// caller buffer (the original returns std::wstring; empty wstring on
// truncation maps to 0 + empty buffer here).
size_t FormatBpsCell(wchar_t* buf, size_t bufSize, double rxBps,
                     double txBps, bool known) {
    if (buf == nullptr || bufSize == 0) return 0;
    if (!known) {
        if (bufSize < 2) return 0;
        buf[0] = L'—';
        buf[1] = L'\0';
        return 1;
    }
    if (rxBps < 0.5 && txBps < 0.5) {
        if (bufSize < 5) {
            buf[0] = L'\0';
            return 0;
        }
        buf[0] = L'i';
        buf[1] = L'd';
        buf[2] = L'l';
        buf[3] = L'e';
        buf[4] = L'\0';
        return 4;
    }
    wchar_t rx[32] = {0};
    wchar_t tx[32] = {0};
    FormatBytes(rx, 32, static_cast<uint64_t>(rxBps + 0.5));
    FormatBytes(tx, 32, static_cast<uint64_t>(txBps + 0.5));
    const int n =
        ::swprintf_s(buf, bufSize, L"↓ %s/s  ↑ %s/s", rx, tx);
    return (n < 0) ? 0 : static_cast<size_t>(n);
}

// ====================================================================
// 22. MMDB UTF-8 widen
//     Original: GeoIp.cpp:541-579 AppendUtf8/Widen
// ====================================================================

// Copy of GeoIp.cpp:541-572 AppendUtf8 (+ :574-579 reserve), writing
// into a fresh string instead of appending.
std::wstring WidenUtf8(const unsigned char* p, size_t n) {
    std::wstring out;
    if (p == nullptr || n == 0) return out;
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        const unsigned char lead = p[i];
        if (lead < 0x80) {
            out.push_back(static_cast<wchar_t>(lead));
            ++i;
            continue;
        }
        uint32_t cp = 0xFFFD;
        size_t used = 1;
        if (lead >= 0xC2 && lead < 0xE0) {
            cp = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead < 0xF0) {
            cp = lead & 0x0Fu;
        } else {
            cp = 0xFFFD;
        }
        if (lead >= 0xC2 && lead < 0xF0 && i + 1 < n &&
            (p[i + 1] & 0xC0) == 0x80) {
            cp = (cp << 6) | (p[i + 1] & 0x3Fu);
            used = 2;
            if (lead >= 0xE0 && i + 2 < n && (p[i + 2] & 0xC0) == 0x80) {
                cp = (cp << 6) | (p[i + 2] & 0x3Fu);
                used = 3;
            }
        }
        if (cp > 0xFFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        out.push_back(static_cast<wchar_t>(cp));
        i += used;
    }
    return out;
}

// ====================================================================
// 23. Unicast classification
//     Original: GeoIp.cpp:1243-1309 IsGlobalUnicastV4/V6
// ====================================================================

// Copy of GeoIp.cpp:1243-1271 IsGlobalUnicastV4.
bool IsGlobalV4(uint32_t a) {
    const uint32_t first = (a >> 24) & 0xFFu;
    if (first == 0) return false;
    if (first == 10) return false;
    if (first == 127) return false;
    if (first == 100) {
        return (a & 0xFFC00000u) != 0x64400000u;
    }
    if (first == 169) {
        return (a >> 16) != 0xA9FEu;
    }
    if (first == 172) {
        const uint32_t second = (a >> 16) & 0xFFu;
        if (second >= 16 && second <= 31) return false;
    }
    if (first == 192) {
        if ((a >> 8) == 0xC00000u) return false;
        if ((a >> 8) == 0xC05863u) return false;
        if ((a >> 16) == 0xC0A8u) return false;
    }
    if (first == 198) {
        if (((a >> 16) & 0xFFFEu) == 0xC612u) return false;
        if ((a >> 8) == 0xC63364u) return false;
    }
    if (first == 203 && (a >> 8) == 0xCB0071u) return false;
    if ((a & 0xF0000000u) == 0xE0000000u) return false;
    if ((a & 0xF0000000u) == 0xF0000000u) return false;
    return true;
}

// Copy of GeoIp.cpp:1273-1309 IsGlobalUnicastV6.
bool IsGlobalV6(const unsigned char a[16]) {
    if (a[0] == 0xFF) return false;
    if (a[0] == 0xFC || a[0] == 0xFD) return false;
    if (a[0] == 0xFE && (a[1] & 0xC0) == 0xC0) return false;
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x0D && a[3] == 0xB8) {
        return false;
    }
    if (a[0] == 0x01 && a[1] == 0x00) {
        bool tailZero = true;
        for (size_t i = 2; i < 16; ++i) {
            if (a[i] != 0) {
                tailZero = false;
                break;
            }
        }
        if (tailZero) return false;
    }
    if (a[0] == 0x00 && a[1] == 0x00) {
        bool tailZero = true;
        for (size_t i = 2; i < 16; ++i) {
            if (a[i] != 0) {
                tailZero = false;
                break;
            }
        }
        if (tailZero) return false;
    }
    return true;
}

// ====================================================================
// 24. Snapshot pairing
//     Original: ConnectionStore.cpp:1304-1326 PrevKeyIndex
// ====================================================================

namespace {

// Local hasher mirroring ConnectionKeyHash (std::hash<string_view>
// over the key bytes); SnapKey::operator== is shared from the header.
struct SnapKeyHash {
    size_t operator()(const SnapKey& k) const {
        return std::hash<std::string_view>()(std::string_view(
            reinterpret_cast<const char*>(k.bytes), k.len));
    }
};

}  // namespace

// Copy of the ReplaceSnapshot pairing shape: backwards-built FIFO
// queues per key, consumed with pop_back (ascending previous index).
int PairSnapshot(const SnapKey* prev, size_t nPrev, const SnapKey* fresh,
                 size_t nFresh, int* out) {
    if (prev == nullptr || fresh == nullptr || out == nullptr) return 0;
    for (size_t i = 0; i < nFresh; ++i) out[i] = -1;
    if (nPrev == 0 || nFresh == 0) return 0;
    std::unordered_map<SnapKey, std::vector<size_t>, SnapKeyHash> prevMap;
    prevMap.reserve(nPrev * 2 + 1);
    for (size_t i = nPrev; i-- > 0;) {
        prevMap[prev[i]].push_back(i);
    }
    int paired = 0;
    for (size_t i = 0; i < nFresh; ++i) {
        const auto it = prevMap.find(fresh[i]);
        if (it == prevMap.end() || it->second.empty()) continue;
        out[i] = static_cast<int>(it->second.back());
        it->second.pop_back();
        ++paired;
    }
    return paired;
}

// ====================================================================
// 25. Counted-key linear find
//     Original: GeoIp.cpp:519-535 MapFind per-entry step
// ====================================================================

// Copy of the per-entry step: length check, then memcmp.
int FindCountedKey(const CountedKey* keys, size_t nKeys,
                   const unsigned char* want, size_t wantLen) {
    if (keys == nullptr || want == nullptr) return -1;
    for (size_t i = 0; i < nKeys; ++i) {
        if (keys[i].n != wantLen) continue;
        if (keys[i].p == nullptr) continue;
        if (std::memcmp(keys[i].p, want, wantLen) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ====================================================================
// 26. Flow-key equality + address compare
//     Original: TcpReasm.cpp:52 CmpAddr, :224-228 operator==
// ====================================================================

// Copy of TcpReasm.cpp:52 CmpAddr.
int CmpFlowAddr(const unsigned char* a, const unsigned char* b) {
    return std::memcmp(a, b, 16);
}

// Copy of TcpReasm.cpp:224-228 TcpKey::operator==.
bool FlowKeyEqual(const FlowKey& a, const FlowKey& b) {
    return a.portA == b.portA && a.portB == b.portB &&
           std::memcmp(a.addrA, b.addrA, 16) == 0 &&
           std::memcmp(a.addrB, b.addrB, 16) == 0;
}

// ====================================================================
// 27. Handle-table filter
//     Original: SocketTraffic.cpp:973-1012 ScanHandles phase 1
// ====================================================================

// Copy of the phase-1 membership probes, mapped onto plain arrays
// (unordered_set<DWORD> want, unordered_set<...> socketTypes,
// std::set<pair<DWORD,ULONG_PTR>> skip).
size_t FilterHandles(const HandleEntry* entries, size_t n,
                     const uint32_t* wantPids, size_t nPids,
                     const uint32_t* wantTypes, size_t nTypes,
                     bool typesKnown, const PidHandle* skip, size_t nSkip,
                     size_t* outIdx, size_t outCap) {
    if (entries == nullptr || outIdx == nullptr) return 0;
    const std::unordered_set<uint32_t> want(wantPids, wantPids + nPids);
    const std::unordered_set<uint32_t> types(wantTypes,
                                             wantTypes + nTypes);
    std::set<std::pair<uint32_t, uint64_t>> skipSet;
    for (size_t i = 0; i < nSkip; ++i) {
        skipSet.emplace(skip[i].pid, skip[i].handle);
    }
    size_t accepted = 0;
    for (size_t i = 0; i < n; ++i) {
        if (entries[i].srcPid > 0xFFFFFFFFu) continue;
        const uint32_t pid = static_cast<uint32_t>(entries[i].srcPid);
        if (want.count(pid) == 0) continue;
        if (typesKnown && types.count(entries[i].typeIndex) == 0) {
            continue;
        }
        if (skipSet.count({pid, entries[i].handle}) != 0) continue;
        if (accepted < outCap) outIdx[accepted] = i;
        ++accepted;
    }
    return accepted;
}

// ====================================================================
// 28. ETW event classifier + payload
//     Original: EtwTraffic.cpp:260-293
// ====================================================================

// Copy of EtwTraffic.cpp:260-279 ClassifyNetworkEvent, with GUIDs as
// the exact 16 bytes IsEqualGUID compares.
bool GuidEqual(const unsigned char* a, const unsigned char* b) {
    return std::memcmp(a, b, 16) == 0;
}

namespace {

// Exact bytes of kTcpIpProviderGuid / kUdpIpProviderGuid
// (EtwTrafficTypes.h:28-33).
const unsigned char kTcpGuid[16] = {
    0xC0, 0x0A, 0x28, 0x9A, 0xE0, 0xC8, 0xD1, 0x11,
    0x84, 0xE2, 0x00, 0xC0, 0x4F, 0xB9, 0x98, 0xA2};
const unsigned char kUdpGuid[16] = {
    0xC5, 0x50, 0x3A, 0xBF, 0xC9, 0xA9, 0x88, 0x49,
    0xA0, 0x05, 0x2D, 0xF0, 0xB7, 0xC8, 0x0F, 0x80};

}  // namespace

TrafficDir ClassifyEvent(const unsigned char guid[16], uint16_t id,
                         uint16_t opcode) {
    if (guid == nullptr) return TrafficDir::kNone;
    if (!GuidEqual(guid, kTcpGuid) && !GuidEqual(guid, kUdpGuid)) {
        return TrafficDir::kNone;
    }
    uint16_t type = 0;
    if (opcode == 10 || opcode == 11 || opcode == 26 || opcode == 27) {
        type = opcode;
    } else if (id == 10 || id == 11 || id == 26 || id == 27) {
        type = id;
    } else {
        return TrafficDir::kNone;
    }
    return (type == 10 || type == 26) ? TrafficDir::kSent
                                      : TrafficDir::kReceived;
}

// Copy of EtwTraffic.cpp:281-293 ParseTrafficPayload.
bool ParseEventPayload(const void* data, size_t length, uint32_t* pid,
                       uint32_t* size) {
    if (data == nullptr || length < 8) return false;
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    uint32_t p = 0;
    uint32_t s = 0;
    std::memcpy(&p, bytes, sizeof(p));
    std::memcpy(&s, bytes + sizeof(p), sizeof(s));
    if (p == 0 || s == 0) return false;
    if (pid != nullptr) *pid = p;
    if (size != nullptr) *size = s;
    return true;
}

// ====================================================================
// 29. JSON string escape
//     Original: Commands.cpp:62-85 JsonEscapeA
// ====================================================================

namespace {

// Local faithful WideToUtf8 (Utils.cpp:121-125 shape, -1 form) so the
// orig copy converts exactly like production before escaping.
std::string OrigWide(const std::wstring& s) {
    if (s.empty()) return std::string();
    int needed = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr,
                                       0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<size_t>(needed) - 1, '\0');
    int written = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1,
                                        out.data(), needed, nullptr,
                                        nullptr);
    if (written <= 0) return std::string();
    if (static_cast<size_t>(written - 1) != out.size()) {
        out.resize(static_cast<size_t>(written - 1));
    }
    return out;
}

}  // namespace

// Copy of Commands.cpp:62-85 JsonEscapeA.
std::string JsonEscape(const std::wstring& s) {
    const std::string u8 = OrigWide(s);
    std::string out;
    out.reserve(u8.size() + 8);
    for (unsigned char ch : u8) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[8] = {0};
                    ::sprintf_s(buf, "\\u%04x",
                                static_cast<unsigned int>(ch));
                    out += buf;
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    return out;
}

// ====================================================================
// 30. CSV field escape
//     Original: Utils.cpp:185-197 CsvEscapeUtf8
// ====================================================================

// Copy of Utils.cpp:185-197 CsvEscapeUtf8.
std::string CsvEscape(const std::string& field) {
    bool needQuote =
        field.find_first_of(",\"\r\n") != std::string::npos;
    if (!needQuote) return field;
    std::string out;
    out.reserve(field.size() + 2);
    out.push_back('"');
    for (char c : field) {
        if (c == '"')
            out += "\"\"";
        else
            out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// ====================================================================
// 31. Small-integer formatting
//     Original: Utils.cpp:199-203, ChartExport.cpp:171-181,
//               ConnectionStore.cpp:530-549
// ====================================================================

// Copy of Utils.cpp:199-203 FormatPort.
size_t FormatPort(wchar_t* buf, size_t bufSize, unsigned port) {
    if (buf == nullptr || bufSize == 0) return 0;
    wchar_t tmp[16] = {0};
    const int n = ::swprintf_s(tmp, L"%u", port);
    if (n < 0 || static_cast<size_t>(n) + 1 > bufSize) {
        buf[0] = L'\0';
        return 0;
    }
    for (size_t i = 0; i <= static_cast<size_t>(n); ++i) buf[i] = tmp[i];
    return static_cast<size_t>(n);
}

// Copy of ChartExport.cpp:171-181 FormatU64 (wide roundtrip).
size_t FormatU64Dec(char* buf, size_t bufSize, uint64_t v) {
    if (buf == nullptr || bufSize == 0) return 0;
    wchar_t wbuf[32] = {0};
    ::swprintf_s(wbuf, L"%llu", static_cast<unsigned long long>(v));
    char narrow[32] = {0};
    if (::WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, narrow,
                              static_cast<int>(sizeof(narrow)), nullptr,
                              nullptr) == 0) {
        buf[0] = '\0';
        return 0;
    }
    const size_t n = std::strlen(narrow);
    if (n + 1 > bufSize) {
        buf[0] = '\0';
        return 0;
    }
    std::memcpy(buf, narrow, n + 1);
    return n;
}

// Copy of ConnectionStore.cpp:530-549 FormatDuration.
size_t FormatDuration(wchar_t* buf, size_t bufSize, uint64_t seconds) {
    if (buf == nullptr || bufSize == 0) return 0;
    wchar_t tmp[64] = {0};
    int n = -1;
    if (seconds >= 86400ULL) {
        n = ::swprintf_s(tmp, L"%llud %lluh", seconds / 86400ULL,
                         (seconds % 86400ULL) / 3600ULL);
    } else if (seconds >= 3600ULL) {
        n = ::swprintf_s(tmp, L"%lluh %llum", seconds / 3600ULL,
                         (seconds % 3600ULL) / 60ULL);
    } else if (seconds >= 60ULL) {
        n = ::swprintf_s(tmp, L"%llum %02llus", seconds / 60ULL,
                         seconds % 60ULL);
    } else {
        n = ::swprintf_s(tmp, L"%llus", seconds);
    }
    if (n < 0 || static_cast<size_t>(n) + 1 > bufSize) {
        buf[0] = L'\0';
        return 0;
    }
    for (size_t i = 0; i <= static_cast<size_t>(n); ++i) buf[i] = tmp[i];
    return static_cast<size_t>(n);
}

// ====================================================================
// 32. IP address emitters
//     Original: TcpTable.cpp:42-64 PrintIpv4/PrintIpv6
// ====================================================================

// Copy of TcpTable.cpp:42-49 PrintIpv4 (InetNtopW, narrow result).
std::wstring FormatIpv4(const unsigned char addr[4]) {
    if (addr == nullptr) return std::wstring();
    IN_ADDR inAddr = {};
    std::memcpy(&inAddr.S_un.S_addr, addr, 4);
    wchar_t buf[16] = {0};
    if (::InetNtopW(AF_INET, &inAddr, buf, 16) == nullptr) {
        return std::wstring(L"?.?.?.?");
    }
    return std::wstring(buf);
}

// Copy of TcpTable.cpp:55-64 PrintIpv6 minus the scope suffix
// (the suffix rule lives with the caller; the emitter covers the
// address). INET6_ADDRSTRLEN-sized buffer, "::" fallback.
std::wstring FormatIpv6(const unsigned char addr[16]) {
    if (addr == nullptr) return std::wstring();
    IN6_ADDR in6 = {};
    std::memcpy(in6.s6_addr, addr, 16);
    wchar_t buf[46] = {0};
    if (::InetNtopW(AF_INET6, &in6, buf, 46) == nullptr) {
        return std::wstring(L"::");
    }
    return std::wstring(buf);
}

// ====================================================================
// 33. Display width
//     Original: Commands.cpp:270-350
// ====================================================================

// Copy of Commands.cpp:270-291 CpWidth.
size_t CpWidth(uint32_t cp) {
    if (cp < 0x0300) return 1;
    if (cp <= 0x036F) return 0;   // combining accents
    if (cp < 0x1100) return 1;
    if (cp <= 0x115F) return 2;   // Hangul Jamo
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

// Copy of Commands.cpp:295-322 NextCp.
size_t NextCp(const std::string& s, size_t i, uint32_t* cp) {
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
            *cp = u;  // malformed: count the lead byte, resync next call
            return 1;
        }
        v = (v << 6) | (c & 0x3Fu);
    }
    *cp = v;
    return n;
}

// Copy of Commands.cpp:324-332 DisplayWidth.
size_t DisplayWidth(const std::string& s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = 0;
        i += NextCp(s, i, &cp);
        w += CpWidth(cp);
    }
    return w;
}

// Copy of Commands.cpp:336-350 TruncateToWidth.
std::string TruncateToWidth(const std::string& s, size_t width) {
    if (DisplayWidth(s) <= width) return s;
    if (width == 0) return std::string();
    const size_t budget = width - 1;   // the ellipsis owns the last column
    size_t w = 0, i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        const size_t n = NextCp(s, i, &cp);
        const size_t cw = CpWidth(cp);
        if (w + cw > budget) break;
        w += cw;
        i += n;
    }
    return s.substr(0, i) + "\xE2\x80\xA6";
}

// ====================================================================
// 34. MMDB payload size + pointer read
//     Original: GeoIp.cpp:422-442, GeoIp.cpp:492-511
// ====================================================================

namespace {
// GeoIp.cpp:73-86.
constexpr uint8_t kSizeExtended29 = 29;
constexpr uint8_t kSizeExtended30 = 30;
constexpr uint32_t kSizeExtended30Base = 285;
constexpr uint32_t kSizeExtended31Base = 65821;
constexpr size_t kSizeExtraBytes30 = 2;
constexpr size_t kSizeExtraBytes31 = 3;
// GeoIp.cpp:53-56.
constexpr uint8_t kPtrSizeShift = 3;
constexpr uint8_t kPtrSizeMask = 0x3;
constexpr uint64_t kPtr2Base = 2048;     // size 1
constexpr uint64_t kPtr3Base = 526336;   // size 2

// GeoIp.cpp:134-139 ReadBytes.
bool ReadBytesHere(const unsigned char* p, size_t n, uint64_t* out) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
    *out = v;
    return true;
}
}  // namespace

// Copy of GeoIp.cpp:422-442 PayloadSize.
bool PayloadSize(const unsigned char* data, size_t size, unsigned char ctrl,
                 size_t pos, MmdbPayload* out) {
    const uint32_t s = ctrl & 0x1Fu;
    if (s < kSizeExtended29) {
        out->size = s;
        out->pos = pos;
        return true;
    }
    if (s == kSizeExtended29) {
        if (pos >= size) return false;
        out->size = kSizeExtended29 + static_cast<uint32_t>(data[pos]);
        out->pos = pos + 1;
        return true;
    }
    // 30 -> 2 bytes added to 285; 31 -> 3 bytes added to 65821.
    const size_t extra =
        (s == kSizeExtended30) ? kSizeExtraBytes30 : kSizeExtraBytes31;
    if (pos + extra > size) return false;
    uint64_t v = 0;
    ReadBytesHere(data + pos, extra, &v);
    out->size = static_cast<uint32_t>(
        (s == kSizeExtended30 ? kSizeExtended30Base : kSizeExtended31Base) + v);
    out->pos = pos + extra;
    return true;
}

// Copy of GeoIp.cpp:492-511 ReadPointer.
bool ReadPointer(const unsigned char* data, size_t size, unsigned char ctrl,
                 size_t pos, size_t* out) {
    const uint8_t psz = static_cast<uint8_t>((ctrl >> kPtrSizeShift) &
                                             kPtrSizeMask);
    const size_t payload = static_cast<size_t>(psz) + 1;
    if (pos + payload > size) return false;

    uint64_t v = 0;
    ReadBytesHere(data + pos, payload, &v);
    if (psz != 3) {
        // ReadBytes() replaced 'v', so the control byte's three bits go on
        // top of the payload now - they are the value's MOST significant
        // bits, which is what the (8 * payload) shift expresses.
        v |= (static_cast<uint64_t>(ctrl) & 0x7u) << (8 * payload);
        if (psz == 1) v += kPtr2Base;
        else if (psz == 2) v += kPtr3Base;
    }

    *out = static_cast<size_t>(v);
    return true;
}

// ====================================================================
// 35. VLAN skip + flow-record probe
//     Original: Pcapng.cpp:119-129, Pcapng.cpp:354-361
// ====================================================================

// Copy of Pcapng.cpp:119-129 SkipVlan.
size_t SkipVlan(const unsigned char* p, size_t len, size_t off) {
    while (off + 4 <= len) {
        const uint16_t t = static_cast<uint16_t>((p[off] << 8) | p[off + 1]);
        if (t == 0x8100 || t == 0x88A8 || t == 0x9100) {
            off += 4;
            continue;
        }
        break;
    }
    return off + 2;   // past the ethertype that terminates the chain
}

// Copy of Pcapng.cpp:354-361 flow-record scan.
bool FlowProbe(const unsigned char* pkt, size_t capLen) {
    for (size_t k = 12 /* kFlowScanFirstOffset */;
         k + 5 <= capLen && k < 40 /* kFlowScanLastOffset */; k += 2) {
        if (pkt[k] == 0x08 && pkt[k + 1] == 0x00 &&
            (pkt[k + 2] >> 4) == 4) {
            return true;
        }
    }
    return false;
}

// ====================================================================
// 36. TCP reassembly render
//     Original: TcpReasm.cpp:162-194 Direction::Render
// ====================================================================

// Copy of TcpReasm.cpp:162-194 Render, with Segment's data held in one
// pool (start/size) rather than a std::string per segment. The sort and
// the append chain are exactly the original's; only the payload storage
// differs, so the A/B compares the sort and the write, not the container.
void RenderSegments(const ReasmRenderView& w, size_t duplicates,
                    bool truncated, ReasmRenderResult* out) {
    out->bytes.clear();
    out->bytesMissing = 0;
    out->hasGap = false;
    out->firstSeq = 0;
    out->segments = w.seqs.size();
    out->duplicates = duplicates;
    out->truncated = truncated;
    if (w.seqs.empty()) return;

    const size_t n = w.seqs.size();
    std::vector<uint32_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = static_cast<uint32_t>(i);

    // std::sort, verbatim - including the fact that it is NOT stable, which
    // the workloads avoid by giving every segment a distinct seq (the
    // overlap trim in Direction::Add already produces that).
    std::sort(order.begin(), order.end(), [&w](uint32_t a, uint32_t b) {
        return w.seqs[a] < w.seqs[b];
    });

    out->firstSeq = w.seqs[order[0]];
    size_t total = 0;
    for (size_t i = 0; i < n; ++i) total += w.sizes[order[i]];
    // reserve + append, which is exactly what the original's
    // out->bytes += segs_[i].data does over a pre-reserved string.
    out->bytes.reserve(total);

    for (size_t i = 0; i < n; ++i) {
        const uint32_t s = order[i];
        if (i > 0) {
            const uint32_t p = order[i - 1];
            const uint64_t prevEnd = w.seqs[p] + w.sizes[p];
            const uint64_t curStart = w.seqs[s];
            if (curStart > prevEnd) {
                out->bytesMissing += static_cast<size_t>(curStart - prevEnd);
                out->hasGap = true;
            }
        }
        const size_t sz = w.sizes[s];
        if (sz != 0) {
            out->bytes.insert(
                out->bytes.end(),
                w.pool.begin() + static_cast<ptrdiff_t>(w.starts[s]),
                w.pool.begin() + static_cast<ptrdiff_t>(w.starts[s] + sz));
        }
    }
}

// ====================================================================
// 37. PID dedup + per-PID grouping
//     Original: Snapshot.cpp:37-45, ConnectionStore.cpp:1599-1617
// ====================================================================

// Copy of Snapshot.cpp:37-45 DistinctPids.
std::vector<uint32_t> DistinctPids(const uint32_t* pids, size_t n) {
    std::vector<uint32_t> out;
    if (pids == nullptr || n == 0) return out;
    out.reserve(n);
    std::unordered_set<uint32_t> seen;
    seen.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (seen.insert(pids[i]).second) out.push_back(pids[i]);
    }
    return out;
}

// Copy of the pidRows_ fill in ConnectionStore.cpp:1599-1617, with the
// bucket order made explicit: the original's unordered_map iteration order
// is unspecified, so the reference records first appearance and the
// optimized side produces the same thing by construction.
PidGroups GroupByPid(const uint32_t* pids, size_t n) {
    PidGroups g;
    if (pids == nullptr || n == 0) return g;
    std::unordered_map<uint32_t, size_t> index;   // pid -> slot in g.pids
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

}  // namespace bench
}  // namespace wintcp
