// bench_api.h
// SPDX-License-Identifier: Apache-2.0
//
// Canonical interface for the wintcp benchmark. The benchmark app
// (wintcp_benchmark.cpp) is compiled ONCE and linked twice:
//
//   bench_orig.exe  <- orig_functions.cpp    (faithful copies of the
//                                              original wintcp C++)
//   bench_asm.exe   <- replaced_functions.cpp (the same copies with the
//                                              C++ bodies commented out)
//                        asm_functions.cpp    (delegates to the
//                                              assembly/intrinsic
//                                              implementations in
//                                              opt_functions.cpp)
//                        wintcp_asm_opt.lib
//
// Both variants implement the exact same declarations below, so the
// harness source is identical and only the linked implementation
// differs. Every function here mirrors one hot path in wintcp that
// opt_functions.h claims to optimize; the provenance (original
// wintcp source location) is documented per function.

#pragma once

// JoinRow/JoinSample live here so the benchmark and the optimized
// implementation share the exact row/sample layout (types only;
// bench_orig links no optimized code).
#include "opt_functions.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wintcp {
namespace bench {

// ---- 1. GeoIP tree walk ------------------------------------------------
// Original: GeoIp.cpp:1002 GeoIpDatabase::ResolveOffset, which calls
//           GeoIp.cpp:947 NodeRecord per bit.
//
// The walk state lives in the database object in wintcp; here it is
// passed explicitly so the benchmark can build synthetic trees.
struct GeoIpTree {
    size_t nodeCount = 0;         // nodeCount_
    size_t treeSize = 0;          // treeSize_  (bytes of the node table)
    size_t dataSectionSize = 0;   // dataSectionSize_ (also means "loaded")
    size_t nodeByteSize = 0;      // nodeByteSize_ (bytes per node = 2 records)
    bool record28 = false;        // record28_
    size_t recordBytes = 0;       // recordBytes_ (3 or 4; ignored if record28)
    const unsigned char* view = nullptr;  // mappedView_ (base of the file)
    size_t fileSize = 0;          // total mapped bytes under view
};

// Walks 'bitCount' address bits through the tree starting at
// 'startNode'. Returns true and sets *out to the data-section offset
// when the walk lands on a data record.
bool GeoIpWalk(const unsigned char bits[16], unsigned bitCount,
               size_t startNode, const GeoIpTree* tree, size_t* out);

// Second wiring of the same walk: an MSB-first shift register over the
// address bits with three straight-line record walkers. Same contract,
// so the harness can A/B the two against each other when the tree is
// built with the original's 32-bit right-half bug present.
bool GeoIpWalkShift(const unsigned char bits[16], unsigned bitCount,
                    size_t startNode, const GeoIpTree* tree, size_t* out);

// ---- 2. TCP reassembly segment overlap ---------------------------------
// Original: TcpReasm.cpp:91 Direction::Add, Pass A and Pass B over
//           std::vector<Segment> (an array of structs: seq + data).
//
// The optimized layout keeps the same logical segments in parallel
// arrays (struct of arrays). The harness owns both views of the same
// workload so neither variant pays a conversion inside the timed loop
// - the layout difference is part of the optimization package being
// measured, exactly as the integration would restructure Direction.
struct ReasmSeg {
    uint64_t seq = 0;
    size_t len = 0;
};

struct ReasmWorkload {
    std::vector<ReasmSeg> aos;    // AoS view (original Direction::Add)
    std::vector<uint64_t> seqs;   // SoA view (optimized layout)
    std::vector<size_t> sizes;    // SoA view (optimized layout)
};

// Computes how much of a new segment [seq, seq+len) is already
// covered (*consumed) and how much is genuinely new (*skip).
// Returns true when there are new bytes to store, false when the
// segment is a full duplicate.
bool ReasmOverlap(uint64_t seq, size_t len,
                  const ReasmWorkload* w, size_t* consumed, size_t* skip);

// ---- 3. Filter substring match -----------------------------------------
// Original: Utils.cpp:209 HasLowerSubstring (std::wstring::find).
//           Both inputs are already lowercased by the caller.
bool HasLowerSubstring(const std::wstring& haystackLower,
                       const std::wstring& needleLower);

// ---- 4. TCP header parsing ---------------------------------------------
// Original: Pcapng.cpp:115 FinishTcp (the TCP field extraction half
//           of ParseIpTcp; the IP half is unchanged by the
//           optimization).
struct TcpHeaderFields {
    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    uint16_t window = 0;
    uint32_t seq = 0;
    uint32_t ack = 0;
    uint8_t tcpFlags = 0;
    const unsigned char* payload = nullptr;
    size_t payloadLen = 0;
    bool ok = false;  // true when the header parsed (payload may be 0)
    bool dataOffOk = false;  // true when data offset [20..avail] is valid
};

// Parses the TCP header of the packet in p[tcpOff..end).
bool ParseTcpHeader(const unsigned char* p, size_t tcpOff, size_t end,
                    TcpHeaderFields* out);

// ---- 5. Byte formatting ------------------------------------------------
// Original: Utils.cpp:192 FormatBytes (swprintf_s). Returns the
// number of wchar_t written excluding the NUL.
size_t FormatBytes(wchar_t* buf, size_t bufSize, uint64_t bytes);

// ---- 6. Connection key building ----------------------------------------
// Original: ConnectionStore.cpp:358 KeyOf (lambda push + memcpy).
//           The key layout is byte-for-byte what std::string wrote:
//           family, protocol, addresses, ports, pid - ports and pid
//           as native-endian 4-byte values.
struct ConnKeyInput {
    bool ipv6 = false;
    bool udp = false;
    const unsigned char* localAddr = nullptr;   // 16 bytes (v4 uses 4)
    const unsigned char* remoteAddr = nullptr;  // 16 bytes (v4 uses 4)
    uint32_t localPort = 0;
    uint32_t remotePort = 0;
    uint32_t pid = 0;
};

void KeyOf(const ConnKeyInput* in, unsigned char* outBytes, size_t* outLen);

// ---- 7. Wide string to UTF-8 -------------------------------------------
// Original: Utils.cpp:94 WideToUtf8 (WideCharToMultiByte twice).
//           Writes up to dstSize bytes; returns the number written.
size_t WideToUtf8(const wchar_t* src, size_t srcLen, char* dst, size_t dstSize);

// ---- 8. Stream hexdump -------------------------------------------------
// Original: StreamCapture.cpp:578 FormatStreamHex (per-byte snprintf).
//           Clamps bytesPerLine to [1,64] (0 maps to 16).
std::string FormatStreamHex(const std::string& bytes, size_t bytesPerLine);

// ---- 9. Wide lowercase -------------------------------------------------
// Original: Utils.cpp:105 ToLowerW (towlower per char).
std::wstring ToLowerW(const std::wstring& s);

// ---- 10. GeoIP big-endian field reads ----------------------------------
// Original: GeoIp.cpp:124-139 ReadU32BE/ReadU64BE/ReadBytes.
//           n = 0 reads 0; callers bound n <= 8.
uint32_t GeoReadU32BE(const unsigned char* p);
uint64_t GeoReadU64BE(const unsigned char* p);
bool GeoReadBytes(const unsigned char* p, size_t n, uint64_t* out);

// ---- 11. TLS big-endian field reads ------------------------------------
// Original: TlsDecode.cpp:23-36 Be16/Be24/Be32.
uint16_t TlsBe16(const unsigned char* p);
uint32_t TlsBe24(const unsigned char* p);
uint32_t TlsBe32(const unsigned char* p);

// ---- 12. TLS extension dispatch + SNI tail strip ------------------------
// Original: TlsDecode.cpp:227-266 ParseClientHelloExtensions (the
//           type dispatch and the trailing-strip loop).
//           StripSniTail mutates nothing: it returns the kept length.
enum class TlsExtAction : unsigned char { kSkip = 0, kSni = 1, kAlpn = 2 };
TlsExtAction ClassifyTlsExtension(uint16_t type);
size_t StripSniTail(char* s, size_t n);

// ---- 13. Pcap endian loads ------------------------------------------------
// Original: Pcapng.cpp:90-105 Rd16/Rd32.
uint16_t Rd16(const unsigned char* p, bool swap);
uint32_t Rd32(const unsigned char* p, bool swap);

// ---- 14. Socket-sample join -----------------------------------------------
// Original: ConnectionStore.cpp:1888-1912 ApplySocketBytes (match kernel
//           shared with ApplySocketTcpInfo and ApplyKernelAges).
//           First-match-wins, claim-once; returns rows updated.
int JoinSamples(std::vector<JoinRow>& rows,
                const std::vector<JoinSample>& samples);

// ---- 15. Long-needle substring --------------------------------------------
// Original: ConnectionStore.cpp:87-89 Has (wstring::find).
bool FindSubstringLong(const std::wstring& haystack,
                       const std::wstring& needle);

// ---- 16. Lower-all concat --------------------------------------------------
// Original: ConnectionStore.cpp:93-98 RebuildLowerAll (10-field chain).
std::wstring BuildLowerAll(const std::wstring* const* fields, size_t count);

// ---- 17. Wide string compare ------------------------------------------------
// Original: std::wstring::compare (CompareRows CmpStr arms).
//           Returns <0 / 0 / >0.
int CompareWide(const wchar_t* a, size_t na, const wchar_t* b, size_t nb);

// ---- 18. Connection-key hash + equality ----------------------------------
// Original: ConnectionStore.h:255-268 (len + memcmp,
//           std::hash<string_view>). Keys are 22 (IPv4) / 46 (IPv6) bytes.
//           Hash values differ by design; equality is bit-identical.
size_t HashConnKey(const unsigned char* bytes, size_t len);
bool EqualConnKey(const unsigned char* a, size_t na, const unsigned char* b,
                  size_t nb);

// ---- 19. Batch rate computation --------------------------------------------
// Original: ConnectionStore.cpp:686-703 ComputeBps, uniform elapsed.
//           Rejects (false/0.0) on zero elapsed or backwards counters.
//           Returns counters accepted.
size_t ComputeBpsBatch(const uint64_t* prevBytes, const uint64_t* nowBytes,
                       uint64_t elapsedMs, double* outBps, size_t n);

// ---- 20. Per-PID traffic sums ----------------------------------------------
// Original: ConnectionStore.cpp:1543-1597 ComputeGroupRates sum phase
//           (std::map sum + write-back). Saturating adds; rows of a PID
//           with no counted row get hasSum == false.
void SumPidTraffic(PidTrafficRow* rows, size_t n);

// ---- 21. Rate cell formatting -----------------------------------------------
// Original: ConnectionStore.cpp:554-565 FormatBpsCell.
//           Returns chars written excluding NUL; 0 + empty on truncation.
size_t FormatBpsCell(wchar_t* buf, size_t bufSize, double rxBps,
                     double txBps, bool known);

// ---- 22. MMDB UTF-8 widen ----------------------------------------------------
// Original: GeoIp.cpp:541-579 AppendUtf8/Widen (U+FFFD rules, 4-byte
//           sequences widen to U+FFFD for 16-bit wchar_t).
std::wstring WidenUtf8(const unsigned char* p, size_t n);

// ---- 23. Unicast classification -----------------------------------------------
// Original: GeoIp.cpp:1243-1309 IsGlobalUnicastV4/V6.
//           V4 input is network order (first octet = top byte).
bool IsGlobalV4(uint32_t a);
bool IsGlobalV6(const unsigned char a[16]);

// ---- 24. Snapshot pairing ----------------------------------------------------
// Original: ConnectionStore.cpp:1304-1326 PrevKeyIndex (map + FIFO
//           queues). out[i] = previous index or -1; returns paired.
int PairSnapshot(const SnapKey* prev, size_t nPrev, const SnapKey* fresh,
                 size_t nFresh, int* out);

// ---- 25. Counted-key linear find -----------------------------------------------
// Original: GeoIp.cpp:519-535 MapFind per-entry step. Returns index or
//           -1; null inputs give -1 in both variants.
int FindCountedKey(const CountedKey* keys, size_t nKeys,
                   const unsigned char* want, size_t wantLen);

// ---- 26. Flow-key equality + address compare ----------------------------------
// Original: TcpReasm.cpp:52 CmpAddr, :224-228 TcpKey::operator==.
//           CmpFlowAddr returns memcmp sign over 16 bytes.
bool FlowKeyEqual(const FlowKey& a, const FlowKey& b);
int CmpFlowAddr(const unsigned char* a, const unsigned char* b);

// ---- 27. Handle-table filter --------------------------------------------------
// Original: SocketTraffic.cpp:973-1012 ScanHandles phase 1.
//           wantPids/wantTypes/skip arrive sorted; outIdx takes up to
//           outCap accepted indexes. Returns accepted count.
size_t FilterHandles(const HandleEntry* entries, size_t n,
                     const uint32_t* wantPids, size_t nPids,
                     const uint32_t* wantTypes, size_t nTypes,
                     bool typesKnown, const PidHandle* skip, size_t nSkip,
                     size_t* outIdx, size_t outCap);

// ---- 28. ETW event classifier + payload ------------------------------------------
// Original: EtwTraffic.cpp:260-293 ClassifyNetworkEvent (GUID +
//           opcode/id chain) and ParseTrafficPayload (PID @0, size @4).
TrafficDir ClassifyEvent(const unsigned char guid[16], uint16_t id,
                         uint16_t opcode);
bool ParseEventPayload(const void* data, size_t length, uint32_t* pid,
                       uint32_t* size);

// ---- 29. JSON string escape ----------------------------------------------------
// Original: Commands.cpp:62-85 JsonEscapeA (WideToUtf8 + per-byte
//           switch). Wide string in, escaped UTF-8 out.
std::string JsonEscape(const std::wstring& s);

// ---- 30. CSV field escape -------------------------------------------------------
// Original: Utils.cpp:185-197 CsvEscapeUtf8. Empty stays empty;
//           quotes double; reserve(size+2) kept.
std::string CsvEscape(const std::string& field);

// ---- 31. Small-integer formatting -----------------------------------------------
// Original: Utils.cpp:199-203 FormatPort, ChartExport.cpp:171-181
//           FormatU64, ConnectionStore.cpp:530-549 FormatDuration.
//           Counts exclude NUL; 0 + empty on truncation.
size_t FormatPort(wchar_t* buf, size_t bufSize, unsigned port);
size_t FormatU64Dec(char* buf, size_t bufSize, uint64_t v);
size_t FormatDuration(wchar_t* buf, size_t bufSize, uint64_t seconds);

// ---- 32. IP address emitters ------------------------------------------------------
// Original: TcpTable.cpp:42-64 PrintIpv4/PrintIpv6 (InetNtopW).
//           Scope suffixes stay out (caller rule).
std::wstring FormatIpv4(const unsigned char addr[4]);
std::wstring FormatIpv6(const unsigned char addr[16]);

// ---- 33. Display width -----------------------------------------------------------
// Original: Commands.cpp:270-350 CpWidth / NextCp / DisplayWidth /
//           TruncateToWidth. UTF-8 in; the ellipsis is U+2026 (three
//           bytes) and owns the last column, so the usable budget is
//           width - 1. A value whose width fits is returned unchanged.
size_t CpWidth(uint32_t cp);
size_t NextCp(const std::string& s, size_t i, uint32_t* cp);
size_t DisplayWidth(const std::string& s);
std::string TruncateToWidth(const std::string& s, size_t width);

// ---- 34. MMDB payload size + pointer read ----------------------------------------
// Original: GeoIp.cpp:422-442 PayloadSize and GeoIp.cpp:492-511
//           ReadPointer, both members of DataReader (the section base
//           'data' and its length 'size' are passed explicitly here).
//           'pos' is the first byte after the control byte.
// 'MmdbPayload' itself is declared once in opt_functions.h (namespace
// wintcp) so the two sides share one type.
using wintcp::MmdbPayload;
bool PayloadSize(const unsigned char* data, size_t size, unsigned char ctrl,
                 size_t pos, MmdbPayload* out);
bool ReadPointer(const unsigned char* data, size_t size, unsigned char ctrl,
                 size_t pos, size_t* out);

// ---- 35. VLAN skip + flow-record probe -----------------------------------
// Original: Pcapng.cpp:119-129 SkipVlan (given the ethertype offset),
//           Pcapng.cpp:354-361 flow-record scan (candidate offsets
//           12,14,...38 admitting only k+5 <= capLen).
size_t SkipVlan(const unsigned char* p, size_t len, size_t off);
bool FlowProbe(const unsigned char* pkt, size_t capLen);

// ---- 36. TCP reassembly render -------------------------------------------
// Original: TcpReasm.cpp:162-194 Direction::Render: std::sort over
//           {u64 seq, std::string data}, then out->bytes += seg.data.
//
// The optimized side receives the SAME logical segments in
// struct-of-arrays form. The harness builds both views, so neither variant
// pays a conversion inside the timed loop - the layout is part of the
// optimization being measured.
//
// All four types are declared once in opt_functions.h (namespace wintcp)
// so both sides share one definition.
using wintcp::ReasmRenderView;
using wintcp::ReasmRenderResult;

void RenderSegments(const ReasmRenderView& w, size_t duplicates,
                    bool truncated, ReasmRenderResult* out);

// ---- 37. PID dedup + per-PID grouping -------------------------------------
// Original: Snapshot.cpp:37-45 DistinctPids (unordered_set, first-
//           appearance order) and ConnectionStore.cpp:1599-1617
//           RebuildIndexes' pid grouping (unordered_map of row lists).
using wintcp::PidGroups;
std::vector<uint32_t> DistinctPids(const uint32_t* pids, size_t n);
PidGroups GroupByPid(const uint32_t* pids, size_t n);

}  // namespace bench
}  // namespace wintcp
