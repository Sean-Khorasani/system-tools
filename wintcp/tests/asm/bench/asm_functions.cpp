// asm_functions.cpp
// SPDX-License-Identifier: Apache-2.0
//
// Canonical bench_api implementations that delegate to the
// assembly/intrinsic-optimized functions declared in
// opt_functions.h. Linked into bench_asm.exe together with
// replaced_functions.cpp (the originals, commented out) and
// wintcp_asm_opt.lib (built from opt_functions.cpp).

#include "bench_api.h"
#include "opt_functions.h"

namespace wintcp {
namespace bench {

// 1. GeoIP tree walk -> ResolveOffsetOpt.
//
// The dataSectionSize parameter is the DATA SECTION size (it mirrors
// Loaded()): the optimized walk refuses unloaded trees and rejects
// data pointers past the section exactly like the original.
bool GeoIpWalk(const unsigned char bits[16], unsigned bitCount,
               size_t startNode, const GeoIpTree* tree, size_t* out) {
    if (tree == nullptr || out == nullptr) return false;
    return ResolveOffsetOpt(bits, bitCount, startNode,
                              tree->nodeCount, tree->nodeByteSize,
                              tree->record28, tree->recordBytes,
                              tree->view, tree->dataSectionSize,
                              16 /* kSeparatorLen */, out);
}

// 27b. Same walk, shift-register form: MSB-first bit register plus
// three straight-line record walkers, selected once per lookup.
bool GeoIpWalkShift(const unsigned char bits[16], unsigned bitCount,
                    size_t startNode, const GeoIpTree* tree, size_t* out) {
    if (tree == nullptr || out == nullptr) return false;
    return ResolveOffsetShiftOpt(bits, bitCount, startNode,
                                 tree->nodeCount, tree->nodeByteSize,
                                 tree->record28, tree->recordBytes,
                                 tree->view, tree->dataSectionSize,
                                 16 /* kSeparatorLen */, out);
}

// 2. TCP reassembly overlap -> AddSegmentOverlapOpt, using
// the struct-of-arrays view of the same logical segments.
bool ReasmOverlap(uint64_t seq, size_t len,
                  const ReasmWorkload* w, size_t* consumed, size_t* skip) {
    if (w == nullptr || consumed == nullptr || skip == nullptr) {
        return false;
    }
    return AddSegmentOverlapOpt(seq, len,
                                  w->seqs.data(), w->sizes.data(),
                                  w->seqs.size(), consumed, skip);
}

// 3. Filter substring match -> HasLowerSubstringOpt.
bool HasLowerSubstring(const std::wstring& haystackLower,
                       const std::wstring& needleLower) {
    return HasLowerSubstringOpt(haystackLower.c_str(),
                                  haystackLower.size(),
                                  needleLower.c_str(),
                                  needleLower.size());
}

// 4. TCP header parsing -> ParseTcpHeaderOpt. The
// optimized version signals failure by leaving the
// payload pointer null, so ok == (payload != nullptr).
bool ParseTcpHeader(const unsigned char* p, size_t tcpOff,
                      size_t end, TcpHeaderFields* out) {
    if (out == nullptr) return false;
    *out = TcpHeaderFields();
    ParseTcpHeaderOpt(p, &out->srcPort, &out->dstPort, &out->seq,
                        &out->ack, &out->tcpFlags, &out->window,
                        &out->payload, &out->payloadLen, tcpOff, end);
    out->ok = (out->payload != nullptr);
    out->dataOffOk = out->ok;
    return out->ok;
}

// 5. Byte formatting -> FormatBytesOpt.
size_t FormatBytes(wchar_t* buf, size_t bufSize, uint64_t bytes) {
    return FormatBytesOpt(buf, bufSize, bytes);
}

// 6. Connection key building -> KeyOfOpt.
void KeyOf(const ConnKeyInput* in, unsigned char* outBytes,
             size_t* outLen) {
    if (in == nullptr || outBytes == nullptr || outLen == nullptr) {
        if (outLen != nullptr) *outLen = 0;
        return;
    }
    KeyOfOpt(in->ipv6, in->udp, in->localAddr, in->localPort,
               in->remoteAddr, in->remotePort, in->pid,
               outBytes, outLen);
}

// 7. Wide string to UTF-8 -> WideToUtf8Opt.
size_t WideToUtf8(const wchar_t* src, size_t srcLen, char* dst,
                    size_t dstSize) {
    return WideToUtf8Opt(src, srcLen, dst, dstSize);
}

// 8. Stream hexdump -> FormatStreamHexOpt.
std::string FormatStreamHex(const std::string& bytes, size_t bytesPerLine) {
    return FormatStreamHexOpt(bytes, bytesPerLine);
}

// 9. Wide lowercase -> ToLowerWOpt.
std::wstring ToLowerW(const std::wstring& s) {
    return ToLowerWOpt(s);
}

// 10. GeoIP big-endian reads -> ReadU32BEOpt/ReadU64BEOpt/ReadBytesOpt.
uint32_t GeoReadU32BE(const unsigned char* p) {
    return ReadU32BEOpt(p);
}

uint64_t GeoReadU64BE(const unsigned char* p) {
    return ReadU64BEOpt(p);
}

bool GeoReadBytes(const unsigned char* p, size_t n, uint64_t* out) {
    return ReadBytesOpt(p, n, out);
}

// 11. TLS big-endian reads -> TlsBe16Opt/TlsBe24Opt/TlsBe32Opt.
uint16_t TlsBe16(const unsigned char* p) {
    return TlsBe16Opt(p);
}

uint32_t TlsBe24(const unsigned char* p) {
    return TlsBe24Opt(p);
}

uint32_t TlsBe32(const unsigned char* p) {
    return TlsBe32Opt(p);
}

// 12. TLS extension dispatch + SNI tail strip.
TlsExtAction ClassifyTlsExtension(uint16_t type) {
    // wintcp::TlsExtAction and bench::TlsExtAction are distinct enums
    // with identical values; cast at the layer boundary.
    return static_cast<TlsExtAction>(ClassifyTlsExtensionOpt(type));
}

size_t StripSniTail(char* s, size_t n) {
    return StripSniTailOpt(s, n);
}

// 13. Pcap endian loads -> Rd16Opt/Rd32Opt.
uint16_t Rd16(const unsigned char* p, bool swap) {
    return Rd16Opt(p, swap);
}

uint32_t Rd32(const unsigned char* p, bool swap) {
    return Rd32Opt(p, swap);
}

// 14. Socket-sample join -> JoinSamplesOpt.
int JoinSamples(std::vector<JoinRow>& rows,
                const std::vector<JoinSample>& samples) {
    return JoinSamplesOpt(rows, samples);
}

// 15. Long-needle substring -> FindSubstringLongOpt.
bool FindSubstringLong(const std::wstring& haystack,
                       const std::wstring& needle) {
    return FindSubstringLongOpt(haystack, needle);
}

// 16. Lower-all concat -> BuildLowerAllOpt.
std::wstring BuildLowerAll(const std::wstring* const* fields, size_t count) {
    return BuildLowerAllOpt(fields, count);
}

// 17. Wide string compare -> CompareWideOpt.
int CompareWide(const wchar_t* a, size_t na, const wchar_t* b, size_t nb) {
    return CompareWideOpt(a, na, b, nb);
}

// 18. Connection-key hash + equality.
size_t HashConnKey(const unsigned char* bytes, size_t len) {
    return HashConnKeyOpt(bytes, len);
}

bool EqualConnKey(const unsigned char* a, size_t na, const unsigned char* b,
                  size_t nb) {
    return EqualConnKeyOpt(a, na, b, nb);
}

// 19. Batch rate computation -> ComputeBpsBatchOpt.
size_t ComputeBpsBatch(const uint64_t* prevBytes, const uint64_t* nowBytes,
                       uint64_t elapsedMs, double* outBps, size_t n) {
    return ComputeBpsBatchOpt(prevBytes, nowBytes, elapsedMs, outBps, n);
}

// 20. Per-PID traffic sums -> SumPidTrafficOpt.
void SumPidTraffic(PidTrafficRow* rows, size_t n) {
    SumPidTrafficOpt(rows, n);
}

// 21. Rate cell formatting -> FormatBpsCellOpt.
size_t FormatBpsCell(wchar_t* buf, size_t bufSize, double rxBps,
                     double txBps, bool known) {
    return FormatBpsCellOpt(buf, bufSize, rxBps, txBps, known);
}

// 22. MMDB UTF-8 widen -> WidenUtf8Opt.
std::wstring WidenUtf8(const unsigned char* p, size_t n) {
    return WidenUtf8Opt(p, n);
}

// 23. Unicast classification.
bool IsGlobalV4(uint32_t a) {
    return IsGlobalUnicastV4Opt(a);
}

bool IsGlobalV6(const unsigned char a[16]) {
    return IsGlobalUnicastV6Opt(a);
}

// 24. Snapshot pairing -> PairSnapshotOpt.
int PairSnapshot(const SnapKey* prev, size_t nPrev, const SnapKey* fresh,
                 size_t nFresh, int* out) {
    return PairSnapshotOpt(prev, nPrev, fresh, nFresh, out);
}

// 25. Counted-key linear find -> FindCountedKeyOpt.
int FindCountedKey(const CountedKey* keys, size_t nKeys,
                   const unsigned char* want, size_t wantLen) {
    return FindCountedKeyOpt(keys, nKeys, want, wantLen);
}

// 26. Flow-key equality + address compare.
bool FlowKeyEqual(const FlowKey& a, const FlowKey& b) {
    return FlowKeyEqualOpt(a, b);
}

int CmpFlowAddr(const unsigned char* a, const unsigned char* b) {
    return CmpFlowAddrOpt(a, b);
}

// 27. Handle-table filter -> FilterHandlesOpt.
size_t FilterHandles(const HandleEntry* entries, size_t n,
                     const uint32_t* wantPids, size_t nPids,
                     const uint32_t* wantTypes, size_t nTypes,
                     bool typesKnown, const PidHandle* skip, size_t nSkip,
                     size_t* outIdx, size_t outCap) {
    return FilterHandlesOpt(entries, n, wantPids, nPids, wantTypes,
                            nTypes, typesKnown, skip, nSkip, outIdx,
                            outCap);
}

// 28. ETW event classifier + payload.
TrafficDir ClassifyEvent(const unsigned char guid[16], uint16_t id,
                         uint16_t opcode) {
    return ClassifyEventOpt(guid, id, opcode);
}

bool ParseEventPayload(const void* data, size_t length, uint32_t* pid,
                       uint32_t* size) {
    return ParseEventPayloadOpt(data, length, pid, size);
}

// 29. JSON string escape -> JsonEscapeOpt.
std::string JsonEscape(const std::wstring& s) {
    return JsonEscapeOpt(s);
}

// 30. CSV field escape -> CsvEscapeOpt.
std::string CsvEscape(const std::string& field) {
    return CsvEscapeOpt(field);
}

// 31. Small-integer formatting.
size_t FormatPort(wchar_t* buf, size_t bufSize, unsigned port) {
    return FormatPortOpt(buf, bufSize, port);
}

size_t FormatU64Dec(char* buf, size_t bufSize, uint64_t v) {
    return FormatU64DecOpt(buf, bufSize, v);
}

size_t FormatDuration(wchar_t* buf, size_t bufSize, uint64_t seconds) {
    return FormatDurationOpt(buf, bufSize, seconds);
}

// 32. IP address emitters.
std::wstring FormatIpv4(const unsigned char addr[4]) {
    return FormatIpv4Opt(addr);
}

std::wstring FormatIpv6(const unsigned char addr[16]) {
    return FormatIpv6Opt(addr);
}

// 33. Display width.
size_t CpWidth(uint32_t cp) {
    return CpWidthOpt(cp);
}

size_t NextCp(const std::string& s, size_t i, uint32_t* cp) {
    return NextCpOpt(s, i, cp);
}

size_t DisplayWidth(const std::string& s) {
    return DisplayWidthOpt(s);
}

std::string TruncateToWidth(const std::string& s, size_t width) {
    return TruncateToWidthOpt(s, width);
}

// 34. MMDB payload size + pointer read.
bool PayloadSize(const unsigned char* data, size_t size, unsigned char ctrl,
                 size_t pos, MmdbPayload* out) {
    return PayloadSizeOpt(data, size, ctrl, pos, out);
}

bool ReadPointer(const unsigned char* data, size_t size, unsigned char ctrl,
                 size_t pos, size_t* out) {
    return ReadPointerOpt(data, size, ctrl, pos, out);
}

// 35. VLAN skip + flow-record probe.
size_t SkipVlan(const unsigned char* p, size_t len, size_t off) {
    return SkipVlanOpt(p, len, off);
}

bool FlowProbe(const unsigned char* pkt, size_t capLen) {
    return FlowProbeOpt(pkt, capLen);
}

// 36. TCP reassembly render.
void RenderSegments(const ReasmRenderView& w, size_t duplicates,
                    bool truncated, ReasmRenderResult* out) {
    RenderSegmentsOpt(w, duplicates, truncated, out);
}

// 37. PID dedup + per-PID grouping.
std::vector<uint32_t> DistinctPids(const uint32_t* pids, size_t n) {
    return DistinctPidsOpt(pids, n);
}

PidGroups GroupByPid(const uint32_t* pids, size_t n) {
    return GroupByPidOpt(pids, n);
}

}  // namespace bench
}  // namespace wintcp
