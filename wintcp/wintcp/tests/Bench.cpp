// Bench.cpp
// See Bench.h. Everything here exercises the real production code paths
// (ConnectionStore, filter parser, column formatting) - no shims.

#include "Bench.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// After windows.h: the TCP state constants live in these, and they are only
// visible once the winsock/Windows headers have settled their macros.
#include <iphlpapi.h>
#include <tcpmib.h>
#include <tlhelp32.h>    // CreateToolhelp32Snapshot for bench stage [E]

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>   // std::runtime_error for the r2.* containment checks
#include <string>
#include <vector>

// Alerts.h is included from inside namespace wintcp (below), so a
// <windows.h>-derived macro that <tcpmib.h> needs would never be expanded.
// Pull it in here, exactly as TcpTable.h does. Without this the MIB_TCP_*
// constants are not visible when their macros are expanded.
#include <windows.h>

#include "Connection.h"
#include "ConnectionStore.h"
#include "Alerts.h"
#include "DetailModel.h"
#include "Elevate.h"
#include "Pcapng.h"
#include "Settings.h"      // IsColumnPermutation / FoldVisibleOrder (7.1)
#include "TypeToJump.h"    // type-to-jump matching (7.2)
#include "Grouping.h"      // row grouping by process (5.1)
#include "Freeze.h"        // frozen-view age (5.5)
#include "BuildInfo.h"     // shortcut sheet (7.6) + About summary (7.5)
#include "Bookmarks.h"     // kBookmarkTag* (5.3 join test)
#include "ChartExport.h"   // chart CSV (7.7a) + zoom reset (7.7b)
#include "ChangeLogWindow.h"  // change-log buffer cap (5.4)
#include "Commands.h"    // abstract layer: details/preset/select helpers
#include "RefreshEngine.h"  // RefreshWatchdogNext policy (r8.* below)
#include "WinCaps.h"       // capability-report policy (caps.* below)
#include "StreamCapture.h"  // MakeCaptureTarget mapping (follow-stream)
#include "SysStats.h"    // headless system stats formatting
#include "Presets.h"       // preset registry round-trip (5.2, via ViewState)
#include "ViewState.h"     // the view description every front end applies
#include "TcpReasm.h"
#include "TlsDecode.h"
#include "GeoIp.h"         // synthetic-database lookups (geoip.* checks)
#include "EtwTraffic.h"     // ClassifyNetworkEvent/Payload + GUIDs
#include "SocketTraffic.h"  // fallback accumulator
#include "Utils.h"
#include "ProcessInfo.h"  // ProcessResolver bench stage [E] (pid dedup gate)

namespace wintcp {
namespace {

// Parse a filter string into a program, for the cases that assert on matching.
// A helper rather than a four-line declaration at each call site, so the
// assertion being tested stays the thing on the screen.
std::vector<FilterClause> Prog(const std::wstring& text) {
    std::vector<FilterClause> out;
    ParseFilter(text, out);
    return out;
}

// The report's row shape (A6/C7). One definition, because the two prefixes MUST
// be the same width: they sit side by side in a transcript and a ragged edge
// makes every PASS/FAIL column fail to line up, which is the whole reason the
// format exists. Asserting the widths here means a future edit to one cannot
// silently misalign every line below it.
constexpr size_t kResultLabelWidth = 6;   // "PASS" / "FAIL" plus two spaces
// Array form, NOT `const char* const`: sizeof on a pointer yields the pointer
// size (8 on x64), so the static_assert below would have compared 8 to 6 and
// failed for a reason that had nothing to do with the strings.
const char kResultPassPrefix[] = "PASS  ";
const char kResultFailPrefix[] = "FAIL  ";
static_assert(sizeof(kResultPassPrefix) - 1 == kResultLabelWidth &&
                  sizeof(kResultFailPrefix) - 1 == kResultLabelWidth,
              "PASS and FAIL prefixes must be the same width");

void Check(TestResult& r, const char* name, bool ok,
           const std::string& detail = std::string()) {
    r.output += ok ? kResultPassPrefix : kResultFailPrefix;
    r.output += name;
    if (!detail.empty()) {
        r.output += "  (";   // two spaces: separates from the widest prefix
        r.output += detail;
        r.output += ")";
    }
    r.output += "\r\n";
    // kExitFail, not a bare 1: TestResult::exitCode IS the process exit code
    // (CliCommands returns it verbatim), so this is the same vocabulary as the
    // CLI's and already has a name for it.
    if (!ok) r.exitCode = kExitFail;
}

// ---- synthetic GeoIP database -------------------------------------------------
//
// The MaxMind DB wire format (A6/C7). Every literal this fixture used to spell
// inline, in one place, because the rule is NOT ours and getting it wrong is
// invisible: a wrong control byte produces a file the reader either rejects or,
// worse, parses into a plausible-looking wrong answer.
//
// The format's top THREE bits of a control byte are the type and the low five
// are the payload size, so a type constant and a size are the SAME byte:
//   0x40 = utf8_string, length inline        0xA0 = uint16, length inline
//   0xC0 = uint32, length inline              0xE0 = map,    entry count inline
// That is why `0xE1` and `0xE2` below are NOT types: they are a map holding one
// and two entries. Written as kMmMap + n the code says what it means.
constexpr unsigned char kMmString = 0x40;
constexpr unsigned char kMmUint16 = 0xA0;
constexpr unsigned char kMmUint32 = 0xC0;
constexpr unsigned char kMmMap    = 0xE0;

// The largest size the low five bits can carry WITHOUT escaping. 31 is the
// arithmetic limit, but 29/30/31 are reserved: a byte whose low five bits are
// one of those means "the real size follows in further bytes", so a value that
// fits must never reach 29. One function, because this clamp was written twice
// with two different expressions - the drift A2 exists to prevent.
constexpr size_t kMmMaxInlineSize = 29;

unsigned char MmControlByte(unsigned char type, size_t size) {
    const size_t n = (size < kMmMaxInlineSize) ? size : kMmMaxInlineSize;
    return static_cast<unsigned char>(type | n);
}

// Byte-shuffling constants for MmUint below: a uint64 is at most eight bytes
// wide, a byte is eight bits, and masking with 0xFF is what keeps the shifted
// value from dragging its neighbours along.
constexpr unsigned kMmBitsPerByte = 8;
constexpr size_t kMmMaxUintBytes = 8;   // sizeof(uint64_t), spelled as a limit
constexpr uint64_t kMmByteMask = 0xFF;

// A real MaxMind database cannot ship with WinTCP (licensing) and none is on a
// dev box, so until now this reader had only ever been exercised against its
// "cannot load database" rejection. These helpers write a REAL database - proper
// search tree, 16-byte separator, data section, the "\xab\xcd\xefMaxMind.com"
// marker and metadata - so the loader, the container skip, the record layout and
// the address byte order are all covered by a file the reader must accept.
//
// Six defects lived in exactly that code and none of them threw: they answered
// wrongly, or refused every real file. They are pinned here.
struct MmdbEntry {
    uint32_t prefix;     // host-order address of the network
    const char* iso;
    const char* name;
};

// Control byte, then the payload. An integer's size IS its byte width, so it is
// never long enough to need the 29/30/31 escape - which is why MmUint can push
// the width straight into the control byte without calling MmControlByte.
void MmUint(std::vector<unsigned char>* out, unsigned char type,
            uint64_t v) {
    unsigned char b[kMmMaxUintBytes] = {0};
    unsigned w = 0;
    for (uint64_t t = v; t; t >>= kMmBitsPerByte) ++w;
    if (w == 0) w = 1;   // zero encodes as one zero byte, never as no bytes
    for (unsigned i = 0; i < w; ++i) {
        b[w - 1 - i] = static_cast<unsigned char>(
            (v >> (kMmBitsPerByte * i)) & kMmByteMask);
    }
    out->push_back(static_cast<unsigned char>(type | w));
    out->insert(out->end(), b, b + w);
}

void MmStr(std::vector<unsigned char>* out, const char* s) {
    const size_t n = std::strlen(s);
    out->push_back(MmControlByte(kMmString, n));
    out->insert(out->end(), s, s + n);
}

// {"country": {"iso_code": .., "names": {"en": ..}}}
//
// Written as kMmMap + N rather than the bare 0xE1 / 0xE2 this used to spell,
// because those two numbers say nothing: they look like distinct types, and in
// fact they are a map holding one and two entries. The nested shape is then
// visible in the source - four one-entry maps around one two-entry map.
void MmCountry(std::vector<unsigned char>* out, const char* iso,
               const char* name) {
    out->push_back(kMmMap + 1);  MmStr(out, "country");
    out->push_back(kMmMap + 2);  MmStr(out, "iso_code"); MmStr(out, iso);
    out->push_back(kMmMap + 1);  MmStr(out, "names");
    out->push_back(kMmMap + 1);  MmStr(out, "en");       MmStr(out, name);
}

// One metadata key -> already-encoded value.
struct MmPair {
    const char* key;
    std::vector<unsigned char> val;
};

void MmMap(std::vector<unsigned char>* out, const std::vector<MmPair>& pairs) {
    out->push_back(MmControlByte(kMmMap, pairs.size()));
    for (const MmPair& p : pairs) {
        MmStr(out, p.key);
        out->insert(out->end(), p.val.begin(), p.val.end());
    }
}

// The spec's pointer bases: an 11-bit, 19-bit and 27-bit value start at 0,
// 2048 and 526336 so the three ranges cannot overlap. Not ours to choose.
constexpr size_t kMmPtr2Base = 2048;
constexpr size_t kMmPtr3Base = 526336;

// Where each awkward record's shared key string is parked. One offset per
// size code, each chosen to fall INSIDE that code's own range: if the bases
// were wrong the pointer would resolve outside the range and land on padding
// instead of on a string, so a wrong base cannot pass by accident.
constexpr size_t kMmPtrTarget0 = 1000;                    // size 0 and size 3
constexpr size_t kMmPtrTarget1 = kMmPtr2Base + 52;        // size 1
constexpr size_t kMmPtrTarget2 = kMmPtr3Base + 64;        // size 2
static_assert(kMmPtrTarget0 < kMmPtr2Base,
              "a size-0 pointer target must stay in the 11-bit range");
static_assert(kMmPtrTarget1 >= kMmPtr2Base && kMmPtrTarget1 < kMmPtr3Base,
              "a size-1 pointer target must stay in the 19-bit range");
static_assert(kMmPtrTarget2 >= kMmPtr3Base,
              "a size-2 pointer target must start at the 27-bit range");

// A data-section pointer: control byte 001SSVVV, then SS+1 bytes (four for
// SS=3, where VVV is IGNORED rather than being the top of the value).
void MmPointer(std::vector<unsigned char>* out, unsigned sizeCode,
               size_t target) {
    constexpr unsigned char kCtrl = 0x20;  // 001_00_000: pointer, size code 0
    if (sizeCode == 0) {
        out->push_back(
            static_cast<unsigned char>(kCtrl | ((target >> 8) & 0x07)));
        out->push_back(static_cast<unsigned char>(target & 0xFF));
    } else if (sizeCode == 1) {
        const size_t v = target - kMmPtr2Base;
        out->push_back(
            static_cast<unsigned char>(kCtrl | 0x08 | ((v >> 16) & 0x07)));
        out->push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
        out->push_back(static_cast<unsigned char>(v & 0xFF));
    } else if (sizeCode == 2) {
        const size_t v = target - kMmPtr3Base;
        out->push_back(
            static_cast<unsigned char>(kCtrl | 0x10 | ((v >> 24) & 0x07)));
        out->push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
        out->push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
        out->push_back(static_cast<unsigned char>(v & 0xFF));
    } else {
        // SS=3 carries a plain 32-bit offset and the spec says VVV is ignored.
        // It is deliberately written as 111: a reader that used those three
        // bits as the top of the value would turn 1000 into 0x070003E8 and
        // report no country, and this is the only way to notice.
        out->push_back(static_cast<unsigned char>(kCtrl | 0x18 | 0x07));
        for (int i = 3; i >= 0; --i) {
            out->push_back(
                static_cast<unsigned char>((target >> (8 * i)) & 0xFF));
        }
    }
}

// {"country": {"is_in_european_union": true, "iso_code": .., "names": ..}}
// where the OUTER KEY is a pointer to the string "country" instead of the
// string itself, and where a boolean sits ahead of iso_code.
//
// Two defects hide in this shape and neither of them throws:
//   * a map key MAY be a pointer - writers dedupe the key every record repeats
//     - so a reader insisting keys be inline UTF-8 strings gives up on the
//     whole map. It gives up on the FIRST key, which is why every record in a
//     real database failed at once while a hand-written one worked.
//   * 'is_in_european_union' is a boolean: an extended type whose five size
//     bits are its VALUE (0 or 1) and which has NO payload. Reading them as a
//     length resumes one byte into the next field, so 'iso_code' is never
//     reached and the row prints no country.
// Each record also uses a different pointer size code for that key.
void MmAwkwardCountry(std::vector<unsigned char>* out, unsigned ptrSize,
                      size_t target, const char* iso, const char* name) {
    out->push_back(kMmMap + 1);
    MmPointer(out, ptrSize, target);
    out->push_back(kMmMap + 3);
    MmStr(out, "is_in_european_union");
    out->push_back(0x01);  // extended (type 0), five size bits = 1
    out->push_back(0x07);  // type 14 - 7: boolean, value 1 (true), no payload
    MmStr(out, "iso_code");
    MmStr(out, iso);
    MmStr(out, "names");
    out->push_back(kMmMap + 1);
    MmStr(out, "en");
    MmStr(out, name);
}

// Builds the database. 'depth' levels of tree, so a network is a /depth and a
// 2**depth-1 node tree is only as large as that needs. 'recordBits' is 24 or 28:
// modern databases are 28 and older ones 24, and the two have completely
// different node byte layouts, both of which the reader claims to support.
//
// 'ipv6Tree' prepends the 96-node zero chain an ip_version 6 file carries in
// front of its IPv4 half, and says ip_version 6 in the metadata. 'awkward'
// writes each record as MmAwkwardCountry does and parks the shared key string
// at the fixed offset its size code needs.
// 'declaredRecordBits', when non-zero, overrides ONLY the record_size written
// into metadata; the tree is still laid out for 'recordBits'. That produces a
// file which is well formed apart from what it CLAIMS - the input F5.13's
// diagnostic exists for, and one that cannot be reached by asking the builder
// for a bogus layout, because a 4-bit record has no byte size to write.
std::vector<unsigned char> BuildSyntheticMmdb(const std::vector<MmdbEntry>& entries,
                                              int depth, unsigned recordBits,
                                              bool ipv6Tree = false,
                                              bool awkward = false,
                                              unsigned declaredRecordBits = 0) {
    // An ip_version 6 file holds the whole IPv4 space at ::/96, so the IPv4
    // half sits 96 zero-bit steps below the root and the tree needs those 96
    // nodes in front of it. Note that no shipped metadata says WHERE - the spec
    // carries 'ipv4_start_node' in prose but every real file omits it - so the
    // reader has to find it by walking. Starting an IPv4 lookup at the root
    // instead walks 00001000... into the first half-1 branch four bits in and
    // finds nothing, which is what every real database did.
    const int chain = ipv6Tree ? 96 : 0;
    const int subCount = (1 << depth) - 1;
    const int nodeCount = chain + subCount;
    const size_t recBytes = recordBits / 8;
    // 28-bit records are not a whole number of bytes: the node is 7 bytes, with
    // the top nibble of each record in the shared middle byte.
    const size_t nodeBytes = (recordBits == 28) ? 7u : recBytes * 2;

    // ---- data section ----
    // The first record starts at offset 0, and that is the point: the spec
    // says a tree record of node_count + 16 is a pointer to the data section's
    // FIRST byte, so offset 0 is an ordinary record holding real data. This
    // began with a pad byte, on the reasoning that a pointer to offset 0 meant
    // "no data" - which made the record stored there unreadable while every
    // other record in the same file worked, and hid it from every test. Real
    // databases do store a country at offset 0. Taking the pad away pins that:
    // the first entry below is read back from offset 0 or the check fails.
    std::vector<unsigned char> data;
    std::vector<size_t> offsets;
    if (awkward) {
        // One shared copy of "country" per size code, at an offset chosen to
        // sit inside that code's range. Records first, then the strings.
        const size_t targets[4] = {kMmPtrTarget0, kMmPtrTarget1, kMmPtrTarget2,
                                   kMmPtrTarget0};
        for (size_t k = 0; k < entries.size(); ++k) {
            offsets.push_back(data.size());
            MmAwkwardCountry(&data, static_cast<unsigned>(k % 4),
                             targets[k % 4], entries[k].iso, entries[k].name);
        }
        // The parked strings must come after every record, or a pointer would
        // read the middle of one. Refuse to build a file where that happens
        // rather than emit something the checks would blame the reader for.
        if (data.size() > kMmPtrTarget0) return {};
        data.resize(kMmPtrTarget0, 0); MmStr(&data, "country");
        data.resize(kMmPtrTarget1, 0); MmStr(&data, "country");
        data.resize(kMmPtrTarget2, 0); MmStr(&data, "country");
    } else {
        for (const MmdbEntry& e : entries) {
            offsets.push_back(data.size());
            MmCountry(&data, e.iso, e.name);
        }
    }

    // ---- tree: a record is the next node, nodeCount ("no data"), or a pointer
    std::vector<unsigned char> tree((size_t)nodeCount * nodeBytes, 0);
    const size_t ptrBase = (size_t)nodeCount + 16;
    const auto setRec = [&](size_t node, int half, size_t value) {
        unsigned char* p = &tree[node * nodeBytes];
        if (recordBits == 28) {
            // 28-bit: 24 value bits in the outer bytes and the top 4 packed into
            // the middle byte - high nibble for the left record, low for the
            // right one. Both records share that byte, so the nibble that is
            // NOT being written has to be preserved.
            unsigned char* mid = p + 3;
            unsigned char* v = p + (half == 0 ? 0 : 4);
            *mid = (half == 0)
                       ? (unsigned char)((*mid & 0x0F) |
                                         (((value >> 24) & 0x0F) << 4))
                       : (unsigned char)((*mid & 0xF0) | ((value >> 24) & 0x0F));
            v[0] = (unsigned char)((value >> 16) & 0xFF);
            v[1] = (unsigned char)((value >> 8) & 0xFF);
            v[2] = (unsigned char)(value & 0xFF);
        } else {
            // 24- or 32-bit: two whole records, back to back.
            unsigned char* v = p + half * recBytes;
            for (size_t i = 0; i < recBytes; ++i) {
                v[i] = (unsigned char)((value >> (8 * (recBytes - 1 - i))) & 0xFF);
            }
        }
    };
    for (int i = 0; i < nodeCount; ++i) {
        for (int b = 0; b < 2; ++b) {
            size_t value;
            if (i < chain) {
                // The chain: bit 0 walks down towards the IPv4 half, and the
                // IPv6-only prefixes on the way have no data. The halves being
                // the same node would let a reader that starts an IPv4 walk at
                // the root drift down anyway and look correct, so they differ.
                value = (b == 0) ? (size_t)(i + 1) : (size_t)nodeCount;
            } else {
                const int child = 2 * (i - chain) + 1 + b;
                value = (size_t)(child < subCount ? chain + child : nodeCount);
            }
            setRec((size_t)i, b, value);
        }
    }
    // Each entry claims the node its first (depth-1) bits lead to; both halves
    // carry the pointer, so the prefix's last bit does not matter.
    for (size_t k = 0; k < entries.size(); ++k) {
        // depth 0 builds an EMPTY tree - node_count 0 - which is the malformed
        // file the reader must refuse by name (F5.13). There is no node for an
        // entry to claim and there cannot be: the file is rejected at its
        // metadata, so the entries below travel only to give the data section
        // something to hold. Indexing the empty 'vector' in setRec instead
        // would write past it.
        if (nodeCount == 0) break;
        int sub = 0;
        for (int d = 0; d < depth - 1; ++d) {
            const int bit = (int)((entries[k].prefix >> (31 - d)) & 1u);
            sub = 2 * sub + 1 + bit;
        }
        // +chain: the subtree is laid out exactly as a bare IPv4 tree would be,
        // just relocated behind the 96 zero bits that reach ::/96.
        const size_t node = (size_t)(chain + sub);
        const size_t ptr = ptrBase + offsets[k];
        setRec(node, 0, ptr);
        setRec(node, 1, ptr);
    }
    // ---- metadata. Deliberately awkward, because these are the shapes the
    // reader has to survive: a uint64 (an extended type, whose size lives in
    // the control byte), a description map of 12 entries, and a languages
    // ARRAY - all of which sit before node_count and must be walked past.
    std::vector<unsigned char> langs;
    // An array is type 11, which does not fit the 3-bit type field, so it uses
    // the extended form: a control byte carrying the SIZE, then the type byte
    // (11 - 7 = 4). Writing a bare 0x02 here means "extended, size 2" and the
    // reader then reads the first character of "en" as the type.
    langs.push_back(0x02);
    langs.push_back(0x04);
    MmStr(&langs, "en"); MmStr(&langs, "de");
    std::vector<unsigned char> desc;
    desc.push_back(0xEC);                      // map of 12 entries
    for (int i = 0; i < 12; ++i) {
        const char k[2] = {'a' + (char)i, 0};
        MmStr(&desc, k);
        MmStr(&desc, "synthetic");
    }
    std::vector<unsigned char> major, minor, epoch, type_, ver, count, size_;
    MmUint(&major, kMmUint16, 2);
    MmUint(&minor, kMmUint16, 0);
    // build_epoch: the ONE metadata field whose type has no inline-size form, so
    // it uses the format's extended encoding: a control byte whose type is 0
    // ("the real type is in the next byte") carrying the byte width, then that
    // type byte. The spec's uint64 is type 9 and extended types are stored as
    // (type - 7), hence 2 - not a magic 2 but an arithmetic one.
    constexpr unsigned char kMmTypeExtended = 0x00;
    constexpr unsigned char kMmSpecTypeUint64 = 9;
    constexpr unsigned char kMmExtendedTypeBias = 7;
    constexpr unsigned char kMmEpochBytes = 4;   // the value below is 32 bits
    static_assert(kMmSpecTypeUint64 - kMmExtendedTypeBias == 2,
                  "MaxMind extended type encoding changed");
    epoch.push_back(static_cast<unsigned char>(kMmTypeExtended |
                                               kMmEpochBytes));
    epoch.push_back(kMmSpecTypeUint64 - kMmExtendedTypeBias);
    epoch.push_back(0x65); epoch.push_back(0x53);
    epoch.push_back(0xA8); epoch.push_back(0x00);
    MmStr(&type_, "Country");
    MmUint(&ver, kMmUint16, ipv6Tree ? 6 : 4);  // ip_version, uint16
    MmUint(&count, kMmUint32, static_cast<uint64_t>(nodeCount));
    // record_size, uint16 - unless the caller wants metadata to claim
    // something else (see the declaration of this function).
    MmUint(&size_, kMmUint16, declaredRecordBits != 0
                                  ? static_cast<uint64_t>(declaredRecordBits)
                                  : static_cast<uint64_t>(recordBits));
    const std::vector<MmPair> meta = {
        {"binary_format_major_version", major},
        {"binary_format_minor_version", minor},
        {"build_epoch", epoch},
        {"database_type", type_},
        {"description", desc},
        {"ip_version", ver},
        {"languages", langs},
        {"node_count", count},
        {"record_size", size_},
    };
    std::vector<unsigned char> metaBytes;
    MmMap(&metaBytes, meta);

    // ---- assemble: [tree][separator][data][marker][metadata] ----
    std::vector<unsigned char> out;
    out.insert(out.end(), tree.begin(), tree.end());
    const uint64_t base = (uint64_t)tree.size() + 16;
    for (int i = 0; i < 8; ++i) out.push_back(0);   // separator, low half
    for (int i = 7; i >= 0; --i) {
        out.push_back((unsigned char)((base >> (8 * i)) & 0xFF));
    }
    out.insert(out.end(), data.begin(), data.end());
    static const unsigned char kMarker[14] = {0xAB, 0xCD, 0xEF, 'M', 'a', 'x',
                                              'M', 'i', 'n', 'd', '.', 'c', 'o',
                                              'm'};
    out.insert(out.end(), kMarker, kMarker + sizeof(kMarker));
    out.insert(out.end(), metaBytes.begin(), metaBytes.end());
    return out;
}

// The one file every synthetic database below is written to. Fixed name, so a
// run overwrites the last one; it lives in %TEMP%, which is already the
// reader's scratch space for the real-file tests too.
std::wstring SyntheticDbPath() {
    wchar_t dir[MAX_PATH] = {0};
    const DWORD n = ::GetTempPathW(MAX_PATH, dir);
    std::wstring path =
        (n > 0 && n < MAX_PATH) ? std::wstring(dir) : std::wstring(L".\\");
    path += L"wintcp-selftest-geoip.mmdb";
    return path;
}

// Writes 'db' to that file. Split out of LoadSyntheticDb so the rejection
// check below shares it: two copies of this write would drift, and the second
// one failing is not a test failure, it is a test that quietly stops testing.
bool WriteSyntheticDb(const std::vector<unsigned char>& db) {
    const std::wstring path = SyntheticDbPath();
    bool wrote = false;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        wrote = ::WriteFile(h, db.data(), (DWORD)db.size(), &put, nullptr) &&
                put == db.size();
        ::CloseHandle(h);
    }
    return wrote;
}

// Writes 'db' to that file and loads it into '*out'. The tag names both checks
// this reports, so a case that failed to BUILD says which database it was
// instead of leaving the checks inside it quietly absent - which reads as a
// higher pass rate, not as a failure, and is the worst way for a suite to break.
bool LoadSyntheticDb(TestResult& r, const std::vector<unsigned char>& db,
                     const char* tag, GeoIpDatabase* out) {
    const std::string wroteTag = std::string(tag) + "-wrote";
    if (!WriteSyntheticDb(db)) {
        Check(r, wroteTag.c_str(), false, "temp file");
        return false;
    }
    std::wstring err;
    const bool loaded = out->Load(SyntheticDbPath(), &err);
    const std::string loadTag = std::string(tag) + "-load";
    Check(r, loadTag.c_str(), loaded, WideToUtf8(err));
    return loaded;
}

// The rejection half of LoadSyntheticDb: the load must FAIL, and the message
// must contain 'needle'. Asserting only "it did not load" is satisfied by any
// of a dozen unrelated breaks, so it pins nothing - this requires the reader to
// say WHICH fault it found, which is the whole of F5.13. The file is deleted
// here rather than left to the caller, because on the failure path there is no
// "if it loaded" branch to hang a cleanup on, and a leftover bad database would
// be re-read by whatever ran next.
void CheckSyntheticDbRejected(TestResult& r, const std::vector<unsigned char>& db,
                              const char* name, const char* needle) {
    if (!WriteSyntheticDb(db)) {
        const std::string wroteTag = std::string(name) + "-wrote";
        Check(r, wroteTag.c_str(), false, "temp file");
        return;
    }
    GeoIpDatabase g;
    std::wstring err;
    const bool loaded = g.Load(SyntheticDbPath(), &err);
    const std::string text = WideToUtf8(err);
    const bool named = text.find(needle) != std::string::npos;
    Check(r, name, !loaded && named, text);
    ::DeleteFileW(SyntheticDbPath().c_str());
}

void CheckSyntheticGeoIp(TestResult& r) {
    // /11 networks, chosen so that the REVERSED form of each address lands on a
    // different network: 13.248.151.210 reversed is 210.151.248.13, which is not
    // in the database, so a byte-order mistake surfaces as a miss rather than as
    // a plausible-looking hit.
    const std::vector<MmdbEntry> entries = {
        {0x08000000u, "US", "United States"},   // 8.0.0.0/11    -> 8.8.8.8
        {0x0DC00000u, "US", "United States"},   // 13.224.0.0/11 -> 13.248.151.210
        {0x01000000u, "AU", "Australia"},       // 1.0.0.0/11    -> 1.1.1.1
        {0x05200000u, "DE", "Germany"},         // 5.32.0.0/11   -> 5.40.1.1
    };
    for (const unsigned bits : {24u, 28u}) {     // both record layouts
        GeoIpDatabase g;
        if (LoadSyntheticDb(r, BuildSyntheticMmdb(entries, 11, bits),
                            "geoip.synthetic", &g)) {
            const std::string layout = (bits == 24) ? "rec24" : "rec28";
            // Each hit is in a different network, so one wrong byte anywhere in
            // the tree walk, the data pointer or the address byte order turns it
            // into a miss instead of a wrong-but-plausible answer. 'geoip.hit-us'
            // reads the record at DATA OFFSET 0, which is the whole reason the
            // pad byte is gone: node_count + 16 points at the section's first
            // byte and that byte may hold a country.
            Check(r, "geoip.hit-us", g.LookupV4(0x08080808u) == L"US", layout);
            Check(r, "geoip.hit-us-same-net", g.LookupV4(0x08080404u) == L"US",
                  layout);
            Check(r, "geoip.hit-us-asymmetric", g.LookupV4(0x0DF897D2u) == L"US",
                  layout);
            Check(r, "geoip.hit-au", g.LookupV4(0x01010101u) == L"AU", layout);
            Check(r, "geoip.hit-de", g.LookupV4(0x05280101u) == L"DE", layout);
            // Globally routable, but in none of the database's networks.
            Check(r, "geoip.miss-unmapped", g.LookupV4(0x68201001u).empty(),
                  layout);
            // Never looked up at all: loopback, RFC1918 and link-local.
            Check(r, "geoip.skip-non-global",
                  g.LookupV4(0x7F000001u).empty() &&
                      g.LookupV4(0xC0A80101u).empty() &&
                      g.LookupV4(0xA9FE0101u).empty(),
                  layout);
            Check(r, "geoip.metadata",
                  g.NodeCount() == (1u << 11) - 1 &&
                      g.DatabaseVersion() == L"Country",
                  layout);
        }
        ::DeleteFileW(SyntheticDbPath().c_str());
    }

    // The same IPv4 data inside an ip_version 6 file, behind the 96-zero-bit
    // chain that puts the IPv4 half at ::/96. The two families must start from
    // DIFFERENT places - an IPv6 address from the root, an IPv4 one from the
    // node those 96 zeros lead to - and neither may start at the other's node.
    for (const unsigned bits : {24u, 28u}) {
        GeoIpDatabase g;
        if (LoadSyntheticDb(r,
                            BuildSyntheticMmdb(entries, 11, bits, true, false),
                            "geoip.v6", &g)) {
            const std::string layout = (bits == 24) ? "rec24v6" : "rec28v6";
            // 1.1.1.1, not 8.8.8.8: the AU record sits after two others in the
            // data section, so these two checks cannot be satisfied or spoiled
            // by where the section's first byte happens to be.
            //
            // IPv4 starts BEHIND the chain. At the root instead, 1.1.1.1 walks
            // 00000001..., meets the first half-1 branch seven bits in, and
            // reports nothing - which is exactly what every real database did
            // before the start node was derived rather than defaulted.
            Check(r, "geoip.v6-ipv4-start-node",
                  g.LookupV4(0x01010101u) == L"AU", layout);
            // IPv6 starts at the ROOT, so ::1.1.1.1 spends all 96 leading zero
            // bits on the chain and then lands in the same subtree. At the IPv4
            // node instead it burns all 128 bits inside a subtree eleven levels
            // deep, falls out of the bottom, and finds nothing.
            unsigned char a6[16] = {0};
            a6[12] = 0x01;
            a6[13] = 0x01;
            a6[14] = 0x01;
            a6[15] = 0x01;
            Check(r, "geoip.v6-ipv6-from-root",
                  g.LookupV6(a6) == L"AU", layout);
            Check(r, "geoip.v6-metadata",
                  g.NodeCount() == 96u + (1u << 11) - 1 &&
                      g.DatabaseVersion() == L"Country",
                  layout);
        }
        ::DeleteFileW(SyntheticDbPath().c_str());
    }

    // Records shaped the way real ones are: the map's first key is a pointer,
    // a boolean sits ahead of iso_code, and one record per pointer size code
    // 0..3 carries it. Distinct isos per record, so a pointer resolving to the
    // wrong string is a wrong answer rather than a pass.
    const std::vector<MmdbEntry> awkward = {
        {0x08000000u, "AA", "Alpha"},     // data offset 0, pointer size 0
        {0x0DC00000u, "BB", "Bravo"},     //                 pointer size 1
        {0x01000000u, "CC", "Charlie"},   //                 pointer size 2
        {0x05200000u, "DD", "Delta"},     //                 pointer size 3
    };
    for (const unsigned bits : {24u, 28u}) {
        GeoIpDatabase g;
        if (LoadSyntheticDb(r,
                            BuildSyntheticMmdb(awkward, 11, bits, false, true),
                            "geoip.awkward", &g)) {
            const std::string layout = (bits == 24) ? "rec24" : "rec28";
            // The record this reads back lives at data offset 0 and its key is
            // an 11-bit pointer, so it fails for three separate reasons if any
            // of the three fixes regresses.
            Check(r, "geoip.awkward-offset-zero-ptr0",
                  g.LookupV4(0x08080808u) == L"AA", layout);
            Check(r, "geoip.awkward-ptr1",
                  g.LookupV4(0x0DF897D2u) == L"BB", layout);
            Check(r, "geoip.awkward-ptr2",
                  g.LookupV4(0x01010101u) == L"CC", layout);
            Check(r, "geoip.awkward-ptr3",
                  g.LookupV4(0x05280101u) == L"DD", layout);
        }
        ::DeleteFileW(SyntheticDbPath().c_str());
    }

    // F5.13. Both faults below used to be reported by ONE sentence
    // ("describes an impossible search tree") which the CLI then surfaced as a
    // bare "cannot load database" - a user with a bad file was told nothing
    // they could act on. The first database is laid out for 24-bit records, so
    // the ONLY thing wrong with it is the record_size metadata claims; the
    // second has a valid record size and no tree at all. Each must be refused,
    // and refused by name, with the offending value in the message.
    CheckSyntheticDbRejected(r, BuildSyntheticMmdb(entries, 11, 24, false, false, 4),
                             "geoip.reject-record-size-4", "record_size = 4");
    CheckSyntheticDbRejected(r, BuildSyntheticMmdb(entries, 0, 24),
                             "geoip.reject-node-count-0", "node_count = 0");
}


// ---- synthetic rows --------------------------------------------------------
// Deterministic mixed traffic: TCP/UDP, v4/v6, changing states, 700 PIDs,
// enough distinct endpoints that the diff has real work to do.
Connection MakeRow(unsigned i) {
    Connection c;
    c.family = (i % 5 == 0) ? AF_INET6 : AF_INET;
    c.protocol = (i % 3 == 0) ? IPPROTO_UDP : IPPROTO_TCP;
    c.localPort = 1024 + (i % 64000);
    c.remotePort = (c.protocol == IPPROTO_TCP) ? 80 + ((i * 7) % 64000) : 0;
    c.state = (c.protocol == IPPROTO_TCP)
                  ? ((i % 11 == 0)     ? MIB_TCP_STATE_LISTEN
                     : (i % 13 == 0)   ? MIB_TCP_STATE_CLOSE_WAIT
                                       : MIB_TCP_STATE_ESTAB)
                  : 0;
    c.pid = 100 + (i % 700);
    if (c.family == AF_INET) {
        c.localAddress = L"10." + std::to_wstring((i / 65536) % 256) + L"." +
                         std::to_wstring((i / 256) % 256) + L"." +
                         std::to_wstring(i % 256);
        c.remoteAddress = L"203.0.113." + std::to_wstring((i * 13) % 256);
    } else {
        c.localAddress = L"fe80::" + std::to_wstring(i % 65536);
        c.remoteAddress = L"2001:db8::" + std::to_wstring((i * 3) % 4096);
    }
    c.processName = L"proc" + std::to_wstring(i % 97) + L".exe";
    c.processPath = L"C:\\bin\\app" + std::to_wstring(i % 97) + L"\\svc.exe";
    if (i % 4 == 0) c.serviceName = L"svc" + std::to_wstring(i % 31);
    c.localEndpoint =
        JoinEndpoint(c.localAddress, c.localPort, c.family == AF_INET6);
    c.remoteEndpoint = (c.protocol == IPPROTO_UDP)
                           ? std::wstring(L"*:*")
                           : JoinEndpoint(c.remoteAddress, c.remotePort,
                                          c.family == AF_INET6);
    ConnectionStore::FinalizeRow(c);
    return c;
}

double NowMs() {
    LARGE_INTEGER f, c;
    ::QueryPerformanceFrequency(&f);
    ::QueryPerformanceCounter(&c);
    return 1000.0 * static_cast<double>(c.QuadPart) /
           static_cast<double>(f.QuadPart);
}

// A fully populated established TCP row used by the match tests.
Connection MakeReferenceTcpRow() {
    Connection c;
    c.family = AF_INET;
    c.protocol = IPPROTO_TCP;
    c.localAddress = L"192.168.1.10";
    c.remoteAddress = L"203.0.113.9";
    c.localPort = 51752;
    c.remotePort = 443;
    c.state = MIB_TCP_STATE_ESTAB;
    c.pid = 42;
    c.processName = L"chrome.exe";
    c.processPath = L"C:\\Program Files\\Chrome\\chrome.exe";
    c.localEndpoint = JoinEndpoint(c.localAddress, c.localPort, false);
    c.remoteEndpoint = JoinEndpoint(c.remoteAddress, c.remotePort, false);
    ConnectionStore::FinalizeRow(c);
    return c;
}

// ---- well-formedness oracles for the text converters -----------------------
// WideToUtf8 and Utf8ToWide sit between every row, every CSV cell, every log
// line and the outside world, and both take NUL-terminated input. "It
// returned" is therefore the weakest thing that could be asserted about them:
// a converter that quietly dropped half a string still returns. These two
// predicates are what the checks below actually assert - encoded bytes a
// conforming decoder must read back as the same text, and decoded text with
// no lone surrogate, because a lone surrogate is exactly what makes the NEXT
// encode fail and take the whole row with it.
//
// ValidUtf8 rejects the three ways UTF-8 can be malformed while still looking
// like UTF-8: an overlong encoding (reads a different character than the
// encoder meant), a surrogate (not a character at all), and anything past
// U+10FFFF.
bool ValidUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra;  // continuation bytes that must follow
        unsigned char lo = 0x80, hi = 0xBF;
        if (c < 0x80) { ++i; continue; }
        if (c >= 0xC2 && c <= 0xDF) { extra = 1; }
        else if (c == 0xE0) { extra = 2; lo = 0xA0; }  // no overlong 3-byte
        else if (c >= 0xE1 && c <= 0xEC) { extra = 2; }
        else if (c == 0xED) { extra = 2; hi = 0x9F; }  // no surrogate
        else if (c >= 0xEE && c <= 0xEF) { extra = 2; }
        else if (c == 0xF0) { extra = 3; lo = 0x90; }  // no overlong 4-byte
        else if (c >= 0xF1 && c <= 0xF3) { extra = 3; }
        else if (c == 0xF4) { extra = 3; hi = 0x8F; }  // cap at U+10FFFF
        else { return false; }                         // 80..C1 and F5..FF
        if (i + extra >= s.size()) return false;       // runs past the end
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char b = static_cast<unsigned char>(s[i + k]);
            const unsigned char klo = (k == 1) ? lo : static_cast<unsigned char>(0x80);
            const unsigned char khi = (k == 1) ? hi : static_cast<unsigned char>(0xBF);
            if (b < klo || b > khi) return false;
        }
        i += extra + 1;
    }
    return true;
}

// ValidUtf16 rejects a high surrogate not followed by a low one, and a low
// surrogate with no high in front: the shape a malformed UTF-8 decode leaves
// behind, and the shape that makes WideToUtf8 fail and return nothing.
bool ValidUtf16(const std::wstring& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= s.size()) return false;
            const wchar_t d = s[i + 1];
            if (d < 0xDC00 || d > 0xDFFF) return false;
            ++i;
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ---- selftest --------------------------------------------------------------

TestResult RunSelfTest() {
    TestResult r;
    r.output += "WinTCP selftest\r\n";

    // ---- filter vocabulary policy -------------------------------------------------
    // The accepted filter keywords are the one place this codebase accumulated
    // silently-wrong INPUT rather than wrong code. `dis.` - debris from the
    // 2026 identifier-corruption incident - was accepted as a filter keyword and
    // worked, so `list --filter "dis.:1"` filtered on disk I/O without anyone
    // deciding that it should. Every gate passed, because nothing asserted that
    // `dis.` should not match.
    //
    // So the policy is pinned on the TABLE, not on individual spellings. These
    // four checks are what stop the next one:
    {
        const FilterKeyword* kw = FilterKeywordTable();
        const size_t kwCount = FilterKeywordCount();
        Check(r, "filter.vocab.is-not-empty", kw != nullptr && kwCount > 0);
        if (kw == nullptr || kwCount == 0) {
            // Do not fall through and dereference nothing. Every later check in
            // this block assumes a non-empty table, and a null-deref inside a
            // selftest is a crash that hides the real failure.
            Check(r, "filter.vocab.table-is-readable", false,
                  "FilterKeywordTable() returned null or an empty table");
        } else {
            // 1. Every spelling is lower-case [a-z0-9-]. A '.' is the specific
            //    corruption artifact (`Dis.`), and this also excludes whitespace,
            //    punctuation and the trailing junk a truncated rename leaves.
            //    `min-rtt` is why '-' is allowed: `ss -i` spells it that way.
            bool spellingShapeOk = true;
            std::wstring offender;
            for (size_t i = 0; i < kwCount && spellingShapeOk; ++i) {
                const std::wstring s = kw[i].spelling == nullptr
                                           ? std::wstring()
                                           : std::wstring(kw[i].spelling);
                if (s.empty()) { spellingShapeOk = false; offender = L"<empty>"; break; }
                for (wchar_t ch : s) {
                    const bool ok = (ch >= L'a' && ch <= L'z') ||
                                    (ch >= L'0' && ch <= L'9') || ch == L'-';
                    if (!ok) {
                        spellingShapeOk = false;
                        offender = s;
                        break;
                    }
                }
            }
            Check(r, "filter.vocab.every-spelling-is-lowercase-alnum",
                  spellingShapeOk, WideToUtf8(offender));

            // 2. No spelling appears twice. A duplicate would be a copy-paste
            //    error, and with a table it would also mean one of the two rows
            //    can never be reached - the table's own order decides, silently.
            bool noDuplicates = true;
            std::wstring dupName;
            for (size_t i = 0; i < kwCount && noDuplicates; ++i) {
                for (size_t j = i + 1; j < kwCount; ++j) {
                    if (std::wcscmp(kw[i].spelling, kw[j].spelling) == 0) {
                        noDuplicates = false;
                        dupName = kw[i].spelling;
                        break;
                    }
                }
            }
            Check(r, "filter.vocab.no-duplicate-spellings", noDuplicates,
                  WideToUtf8(dupName));

            // 3. No field is unreachable. Every non-Any FilterField the table
            //    mentions must be a real enumerator, not a typo'd one - which is
            //    the other way corruption debris shows up here: `FilterField::Dis.`
            //    was the exact artefact, and a valid-looking row pointing at a
            //    field nothing renders would silently never match.
            //    The bound is the LAST enumerator, which is why it is named
            //    rather than spelled as a literal count: a new field added to
            //    FilterField must also move this line, and forgetting is
            //    caught here rather than by a keyword that silently matches
            //    nothing forever.
            bool everyFieldIsAKnownEnumerator = true;
            for (size_t i = 0; i < kwCount; ++i) {
                const int f = static_cast<int>(kw[i].field);
                if (f < 0 || f > static_cast<int>(FilterField::Signature)) {
                    everyFieldIsAKnownEnumerator = false;
                    break;
                }
            }
            Check(r, "filter.vocab.every-field-is-a-known-enumerator",
                  everyFieldIsAKnownEnumerator);

            // 4. The debris itself. Hard-coding the incident's artifact is
            //    deliberate: policy shape (check 1) would catch `dis.` today,
            //    but naming it means the failure message says WHY if a future
            //    rename produces a different spelling that happens to be
            //    alphanumeric.
            const wchar_t* const kIncidentDebris[] = {L"dis.", L"Dis.", L"push_bac.k",
                                                    L"bookmarkMar.", L"k.nown"};
            bool noIncidentDebris = true;
            std::wstring debrisHit;
            for (size_t d = 0;
                 d < sizeof(kIncidentDebris) / sizeof(wchar_t*) &&
                 noIncidentDebris;
                 ++d) {
                for (size_t i = 0; i < kwCount; ++i) {
                    if (_wcsicmp(kw[i].spelling, kIncidentDebris[d]) == 0) {
                        noIncidentDebris = false;
                        debrisHit = kw[i].spelling;
                        break;
                    }
                }
            }
            Check(r, "filter.vocab.no-incident-debris-spellings",
                  noIncidentDebris, WideToUtf8(debrisHit));

            // 5. The table and the parser cannot have drifted apart, because
            //    they are the same object. Spot-check three keywords end to end
            //    through ParseFilter: one alias, one hyphenated spelling (the
            //    case most likely to be mangled by any future rewrite), and one
            //    from the far end of the enum.
            std::vector<FilterClause> probe;
            ParseFilter(L"io:1", probe);
            const bool aliasOk = probe.size() == 1 &&
                                 probe[0].field == FilterField::Disk;
            ParseFilter(L"min-rtt:10", probe);
            const bool hyphenOk = probe.size() == 1 &&
                                  probe[0].field == FilterField::MinRtt;
            ParseFilter(L"retransmits:2", probe);
            const bool farEndOk = probe.size() == 1 &&
                                  probe[0].field == FilterField::Retrans;
            Check(r, "filter.vocab.table-matches-the-parser",
                  aliasOk && hyphenOk && farEndOk);
        }
    }

// ---- capability report (WinCaps) ------------------------------------------------
    // The report is a POLICY surface, so its policy is pinned here: every
    // capability must name the feature in user terms (never a DLL), must
    // explain itself when degraded, and must not invent a remedy where none
    // exists. These are the three ways a report like this goes wrong - it
    // prints "pdh.dll" to a user who does not know what that is, it says
    // "unavailable" with no reason, or it recommends a package that would not
    // help.
    {
        const std::vector<Capability>& caps = WinCapabilities();
        Check(r, "caps.report-is-not-empty", !caps.empty(),
              "an empty report tells a user nothing and hides every defect");
        bool allNamed = true;
        bool allExplained = true;
        bool noFakeRemedy = true;
        for (const Capability& c : caps) {
            if (c.what.empty()) allNamed = false;
            if (c.state != CapState::Available && c.detail.empty())
                allExplained = false;
            if (c.install.find(L".dll") != std::wstring::npos)
                noFakeRemedy = false;
        }
        Check(r, "caps.every-capability-is-named", allNamed);
        Check(r, "caps.every-degraded-one-explains-itself", allExplained);
        Check(r, "caps.no-remedy-names-a-dll", noFakeRemedy);
        // The aggregate bit must agree with the list, or the About box summary
        // and the detail lines can contradict each other.
        bool anyMissing = false;
        for (const Capability& c : caps) {
            if (c.state != CapState::Available) anyMissing = true;
        }
        Check(r, "caps.aggregate-agrees-with-the-list",
              WinAllCapabilitiesPresent() == !anyMissing);
        // iphlpapi is MANDATORY (it owns GetExtendedTcpTable), so a report that
        // said "install something for connection enumeration" would be wrong.
        for (const Capability& c : caps) {
            if (c.what == L"Connection enumeration") {
                Check(r, "caps.mandatory-capability-offers-no-package",
                      c.install.empty(), WideToUtf8(c.install));
            }
        }

        // NO DUPLICATE ROWS. One library, one row. The loop that emits the
        // generic "this library is absent" entries once also emitted one for
        // pdh.dll, which already had a dedicated probe - so a machine with the
        // counters switched off got TWO disk rows, the second strictly less
        // informative than the first. Asserting uniqueness is the only way this
        // stays fixed, because on a healthy machine the report shows nothing
        // and no smoke test can see it.
        bool noDuplicate = true;
        for (size_t i = 0; i < caps.size(); ++i) {
            for (size_t j = i + 1; j < caps.size(); ++j) {
                if (caps[i].what == caps[j].what) noDuplicate = false;
            }
        }
        Check(r, "caps.no-duplicate-rows", noDuplicate);

        // The user-facing names are the only thing a reader without root sees,
        // so they must look like prose: start uppercase, no trailing period,
        // and not be a raw library name (the caps.no-remedy-names-a-dll check
        // covers remedies, but the feature name had the same defect first).
        bool namesReadLikeProse = true;
        for (const Capability& c : caps) {
            if (c.what.empty()) continue;
            if (c.what[0] < L'A' || c.what[0] > L'Z') namesReadLikeProse = false;
            if (c.what.find(L".dll") != std::wstring::npos)
                namesReadLikeProse = false;
        }
        Check(r, "caps.names-read-like-prose", namesReadLikeProse);

        // Connection enumeration is an EAGER import - iphlpapi.dll owns
        // GetExtendedTcpTable - so if this process is running at all, that
        // library is already mapped and the capability must read Available. A
        // Missing here means the report is describing a host it cannot possibly
        // be running on, which is the one error in this whole mechanism that
        // would make the report actively misleading rather than merely absent.
        for (const Capability& c : caps) {
            if (c.what == L"Connection enumeration") {
                Check(r, "caps.mandatory-capability-is-available-on-a-running-process",
                      c.state == CapState::Available);
            }
        }

        // DllAvailable must be STABLE across calls - it caches, and a cache
        // that flapped would make the gate disagree with the report between two
        // adjacent calls. And it must return false for a library that cannot
        // exist, which also exercises the unrecognised-name path where the
        // LoadLibrary reference is released rather than held.
        const char* kAbsent = "wintcp-no-such-library-probe.dll";
        Check(r, "caps.probe-is-stable", DllAvailable("pdh.dll") == DllAvailable("pdh.dll"));
        Check(r, "caps.probe-answers-false-for-an-absent-library",
              !DllAvailable(kAbsent));
        Check(r, "caps.probe-answers-false-for-an-empty-name",
              !DllAvailable("") && !DllAvailable(nullptr));
    }


    // 1. Filter parser: chained prefixes (exclude / direction / proto / field).
    {
        std::vector<FilterClause> prog;
        ParseFilter(L"chrome port:443 exclude:remote:tcp:443", prog);
        bool ok = (prog.size() == 3);
        if (ok) {
            ok = prog[0].field == FilterField::Any && !prog[0].exclude &&
                 prog[0].text == L"chrome" &&
                 prog[1].field == FilterField::Port && prog[1].numeric &&
                 prog[1].lo == 443 && prog[1].hi == 443 &&
                 // "remote:" is a direction, not a field; with a proto
                 // prefix and no field the value is a port, so this is a
                 // numeric range - it used to be asserted as field=Remote
                 // with the text "443" (a clause that never matched).
                 prog[2].exclude && prog[2].direction == +1 &&
                 prog[2].field == FilterField::Any && prog[2].numeric &&
                 prog[2].proto == IPPROTO_TCP &&
                 prog[2].lo == 443 && prog[2].hi == 443;
        }
        Check(r, "filter.parse.chained-prefixes", ok,
              "clauses=" + std::to_string(prog.size()));
    }

    // 2. Filter parser: ranges, proto and state field values.
    {
        std::vector<FilterClause> prog;
        ParseFilter(L"pid:1000-2000", prog);
        bool ok = prog.size() == 1 && prog[0].field == FilterField::Pid &&
                  prog[0].numeric && prog[0].lo == 1000 && prog[0].hi == 2000;
        Check(r, "filter.parse.pid-range", ok);

        // D1: `duration:` is a numeric threshold in SECONDS with s/m/h/d
        // suffixes, not a substring of the printed cell. It used to be the
        // latter, so `duration:1h` matched nothing at all.
        prog.clear();
        ParseFilter(L"duration:1h", prog);
        ok = prog.size() == 1 && prog[0].field == FilterField::Duration &&
             prog[0].numeric && prog[0].lo == 3600 &&
             prog[0].hi == 0x7FFFFFFFFFFFFFFFULL;
        Check(r, "filter.parse.duration-hours", ok);

        prog.clear();
        ParseFilter(L"duration:1h-2d", prog);
        ok = prog.size() == 1 && prog[0].numeric && prog[0].lo == 3600 &&
             prog[0].hi == 2 * 86400;
        Check(r, "filter.parse.duration-range", ok);

        prog.clear();
        ParseFilter(L"duration:45m", prog);
        ok = prog.size() == 1 && prog[0].numeric && prog[0].lo == 2700;
        Check(r, "filter.parse.duration-minutes", ok);

        // A bare number is SECONDS here (not MB, which is the byte fields'
        // default), and a non-numeric value must stay a text clause rather than
        // silently becoming a threshold that matches nothing.
        prog.clear();
        ParseFilter(L"duration:90", prog);
        ok = prog.size() == 1 && prog[0].numeric && prog[0].lo == 90;
        Check(r, "filter.parse.duration-bare-seconds", ok);

        prog.clear();
        ParseFilter(L"duration:abc", prog);
        ok = prog.size() == 1 && !prog[0].numeric;
        Check(r, "filter.parse.duration-non-numeric-stays-text", ok);

        prog.clear();
        ParseFilter(L"state:estab proto:udp", prog);
        ok = prog.size() == 2 && prog[0].field == FilterField::State &&
             prog[0].text == L"estab" && prog[1].field == FilterField::Proto &&
             prog[1].proto == IPPROTO_UDP;
        Check(r, "filter.parse.state-and-proto", ok);

        prog.clear();
        ParseFilter(L"ipv6: tcp", prog);
        ok = prog.size() == 2 && prog[0].family == 6 &&
             prog[1].field == FilterField::Any && prog[1].text == L"tcp";
        Check(r, "filter.parse.family-and-bare-token", ok);

        prog.clear();
        ParseFilter(L"   spaced    tokens  ", prog);
        ok = prog.size() == 2;
        Check(r, "filter.parse.whitespace", ok);
    }

    // 3. Filter matching on a reference row (TCP, pid 42, 443, ESTABLISHED).
    {
        const Connection c = MakeReferenceTcpRow();
        const auto matches = [&c](const wchar_t* expr) {
            std::vector<FilterClause> prog;
            ParseFilter(expr, prog);
            return MatchFilter(c, prog);
        };
        Check(r, "filter.match.bare-substring", matches(L"chrome"));
        Check(r, "filter.match.pid", matches(L"pid:42") && !matches(L"pid:43"));
        Check(r, "filter.match.port",
              matches(L"port:443") && !matches(L"port:1-100"));
        Check(r, "filter.match.state",
              matches(L"state:established") && !matches(L"state:listen"));
        Check(r, "filter.match.proto",
              matches(L"proto:tcp") && !matches(L"proto:udp"));
        Check(r, "filter.match.direction",
              matches(L"remote:203") && !matches(L"remote:192.168"));
        Check(r, "filter.match.exclude", !matches(L"exclude:chrome"));
        Check(r, "filter.match.family",
              matches(L"ipv4:") && !matches(L"ipv6:"));
    }

    // 3b. Direction is a separate axis from field. These all used to parse
    //     into {field=Local, text="<rest>"} and could never match:
    //     "local:port:80", "remote:port:443", "local:51752".
    {
        const Connection c = MakeReferenceTcpRow();   // local 51752, remote 443, pid 42
        const auto matches = [&c](const wchar_t* expr) {
            std::vector<FilterClause> prog;
            ParseFilter(expr, prog);
            return MatchFilter(c, prog);
        };

        std::vector<FilterClause> prog;
        ParseFilter(L"local:port:80", prog);
        bool ok = prog.size() == 1 && prog[0].direction == -1 &&
                  prog[0].field == FilterField::Port && prog[0].numeric &&
                  prog[0].lo == 80 && prog[0].hi == 80;
        Check(r, "filter.parse.direction-plus-field", ok,
              "clauses=" + std::to_string(prog.size()));

        prog.clear();
        ParseFilter(L"local:port:1000-60000", prog);
        ok = prog.size() == 1 && prog[0].direction == -1 &&
             prog[0].field == FilterField::Port && prog[0].numeric &&
             prog[0].lo == 1000 && prog[0].hi == 60000;
        Check(r, "filter.parse.direction-plus-range", ok);

        // Matching: local port 51752 in 1000-60000, remote port 443 not.
        // "local:port:443" must NOT match - the direction narrows Port to
        // the local side, so the row's remote port 443 is irrelevant.
        Check(r, "filter.match.direction-port",
              matches(L"local:port:51752") && !matches(L"local:port:443") &&
                  matches(L"remote:port:443") && !matches(L"remote:51752") &&
                  matches(L"local:port:1000-60000") &&
                  !matches(L"remote:port:1000-60000"));

        // A bare direction with a text value keeps its documented meaning
        // ("remote:203" = remote ADDRESS contains 203), even when the
        // digits happen to look like a port.
        Check(r, "filter.match.direction-address",
              matches(L"local:192.168") && !matches(L"local:203.0.113") &&
                  matches(L"remote:203.0.113") && !matches(L"remote:192.168") &&
                  matches(L"remote:203") && !matches(L"local:443"));

        // A bare direction with text still restricts that side.
        Check(r, "filter.match.direction-text",
              matches(L"local:192.168") && !matches(L"local:203.0.113") &&
                  matches(L"remote:203.0.113") && !matches(L"remote:192.168"));

        // Numeric clauses must never fall back to port matching for a
        // field that is not a port: "pid:1-2" once matched any
        // connection using a port in 1..2 regardless of the PID.
        Check(r, "filter.match.numeric-no-port-fallback",
              matches(L"pid:1-100") && !matches(L"port:1-2") &&
                  matches(L"pid:42") && !matches(L"cpu:1-2"));
    }

    // 3c. Live-stat clauses are numeric THRESHOLDS in the field's own unit,
    //     not substrings of the printed cell. `mem:100` means 100 MB or more
    //     (bare = MB, K/M/G suffix, `mem:100-500` a range, cpu in %), and an
    //     unreadable stat matches NOTHING - it must never pass `mem:0` by
    //     accident, or a protected process would masquerade as a match.
    {
        Connection c = MakeReferenceTcpRow();
        c.cpuPct = 12.5;
        c.memKnown = true;
        c.memWorkingSet = 250ull * 1024ull * 1024ull;
        c.ioKnown = true;
        c.diskReadBytes = 0;
        c.diskWriteBytes = 900ull * 1024ull * 1024ull;
        c.trafficRx = 3ull * 1024ull * 1024ull * 1024ull;
        c.trafficTx = 2000;
        const auto m = [&c](const wchar_t* expr) {
            std::vector<FilterClause> prog;
            ParseFilter(expr, prog);
            return MatchFilter(c, prog);
        };
        Check(r, "filter.stat.cpu-threshold",
              m(L"cpu:12") && !m(L"cpu:13") && m(L"cpu:12-20") &&
                  !m(L"cpu:20-30"));
        Check(r, "filter.stat.mem-threshold",
              m(L"mem:100") && !m(L"mem:300") && m(L"mem:100-500") &&
                  m(L"mem:100MB") && !m(L"mem:1GB"));
        Check(r, "filter.stat.disk-sum",
              m(L"disk:1") && m(L"disk:900MB") && !m(L"disk:2GB"));
        // The corrupted spelling must stay rejected. `dis.` was debris from
        // the 2026 identifier-corruption incident: a regex pass turned `Disk`
        // into `Dis.`, and the repair made that broken spelling WORK instead
        // of deleting it, so `dis.:1` quietly filtered on disk I/O for years.
        // This asserts the observable behaviour, because that is what a user
        // would have typed - and what nothing else was checking.
        Check(r, "filter.stat.corrupted-disk-spelling-rejected",
              m(L"disk:1") && !m(L"dis.:1"));
        Check(r, "filter.stat.traffic-threshold",
              m(L"rx:1GB") && !m(L"rx:4GB") && m(L"tx:1KB") &&
                  m(L"net:3GB") && !m(L"net:4GB"));
        // Suffix arithmetic itself, every spelling: 1KB and 1kb and 1K are
        // all 1024, `512B` is 512 bytes, and the MB form is exact enough to
        // hit the 3 GB reading on the nose (3072 MB) and miss just above it.
        Check(r, "filter.stat.suffix-forms",
              m(L"tx:1KB") && m(L"tx:1K") && m(L"tx:1kb") &&
                  !m(L"tx:1MB") && !m(L"tx:4KB") &&
                  m(L"rx:2GB") && m(L"rx:2G") &&
                  m(L"rx:3072MB") && !m(L"rx:3073MB"));

        Connection u = MakeReferenceTcpRow();
        std::vector<FilterClause> p0;
        ParseFilter(L"mem:0", p0);
        std::vector<FilterClause> p1;
        ParseFilter(L"cpu:0", p1);
        Check(r, "filter.stat.unknown-never-matches",
              !MatchFilter(u, p0) && !MatchFilter(u, p1));
    }

    // 3d. The enrichment pre-pass (G2b). A `tx:`/`rx:`/`net:`/`country:`/
    //     `host:` clause reads a column that is still empty before its join,
    //     so the pre-pass must hold exactly those clauses back and keep the
    //     rest. Getting this wrong is silent: the filter rejects every row,
    //     the join then has nothing to fill, and the command reports "no
    //     match" for a row that was carrying 30 GB.
    {
        Check(r, "prejoin.detects-joined-columns",
              DependsOnEnrichment(FilterField::Tx) &&
                  DependsOnEnrichment(FilterField::Rx) &&
                  DependsOnEnrichment(FilterField::Net) &&
                  DependsOnEnrichment(FilterField::Country) &&
                  DependsOnEnrichment(FilterField::Host) &&
                  !DependsOnEnrichment(FilterField::Cpu) &&
                  !DependsOnEnrichment(FilterField::Mem) &&
                  !DependsOnEnrichment(FilterField::Pid) &&
                  !DependsOnEnrichment(FilterField::State));

        // SPEED is joined by the same scan, so it must be held back too - and
        // it is the one that hides the failure, because a rate is empty on tick
        // 1 whatever we do. Left out, the pre-pass filtered the view on an
        // empty Speed column, the empty view produced an empty PID set, and
        // JoinTrafficPids bailed out before the scan that fills the column had
        // run: `--watch 1 --count 2 --filter "speed:0"` then printed 0 rows on
        // both ticks while the unfiltered run printed `idle` on tick 2. The
        // column was empty BECAUSE of the filter, which reads as "no match".
        Check(r, "prejoin.speed-is-a-joined-column",
              DependsOnEnrichment(FilterField::Speed) &&
                  HasEnrichmentClause(L"speed:1KB") &&
                  PreJoinClauses(L"speed:1KB").empty() &&
                  // Still only the joined clauses are held back.
                  PreJoinClauses(L"proto:tcp speed:1KB").size() == 1);

        Check(r, "prejoin.which-filters-need-it",
              HasEnrichmentClause(L"tx:1GB") &&
                  HasEnrichmentClause(L"proto:tcp country:de") &&
                  HasEnrichmentClause(L"host:cdn") &&
                  // A negated clause compares the same joined value.
                  HasEnrichmentClause(L"exclude:net:1GB") &&
                  !HasEnrichmentClause(L"") &&
                  !HasEnrichmentClause(L"port:443 mem:100") &&
                  !HasEnrichmentClause(L"state:estab process:chrome"));

        // Only the joined clauses are held back; the other clauses survive so
        // the pre-pass stays a superset of the final match set.
        std::vector<FilterClause> pre = PreJoinClauses(L"proto:tcp tx:1GB mem:100");
        Check(r, "prejoin.holds-back-only-joined-clauses", pre.size() == 2,
              "kept=" + std::to_string(pre.size()));
        bool keptProto = false, keptMem = false;
        for (const FilterClause& cl : pre) {
            if (cl.field == FilterField::Proto) keptProto = true;
            if (cl.field == FilterField::Mem) keptMem = true;
        }
        Check(r, "prejoin.keeps-the-other-clauses", keptProto && keptMem);

        // A filter made only of joined clauses degenerates to "everything",
        // which is the widest correct candidate set.
        Check(r, "prejoin.all-joined-yields-empty-restriction",
              PreJoinClauses(L"tx:1GB rx:1MB").empty());

        // And the end-to-end shape: a row carrying 30 GB of traffic must
        // survive a `tx:1GB` filter, and fail a `tx:100GB` one.
        Connection heavy = MakeReferenceTcpRow();
        heavy.trafficTx = 30ull * 1024ull * 1024ull * 1024ull;
        const auto hit = [&heavy](const wchar_t* expr) {
            std::vector<FilterClause> prog;
            ParseFilter(expr, prog);
            return MatchFilter(heavy, prog);
        };
        Check(r, "prejoin.heavy-row-survives-threshold",
              hit(L"tx:1GB") && !hit(L"tx:100GB") && !hit(L"tx:1GB proto:udp"));
    }

    // 3d2. Missing-switch advice (G7). The pre-pass can only hold clauses
    //      back until their JOIN runs; when the switch that starts the join
    //      was never given, the clause could only ever answer "no match" and
    //      the user got an empty table with no explanation at all (measured:
    //      `list --filter host:easeus` -> header only, rc 0, silent stderr).
    //      The advice must name the switch, and must stay silent under
    //      --quiet (zero bytes is the whole contract) and for duration:
    //      --watch ages rows on the store clock with no --traffic involved,
    //      so nagging there would be false.
    {
        ListOptions o;
        o.filter = L"host:cdn";
        const std::string hostAdvice = MissingEnrichmentAdvice(o);
        Check(r, "enrich.advice.host-names-dns",
              hostAdvice.find("--dns") != std::string::npos, hostAdvice);
        o.dns = true;
        Check(r, "enrich.advice.host-ok-with-dns",
              MissingEnrichmentAdvice(o).empty());

        ListOptions g;
        g.filter = L"country:us";
        Check(r, "enrich.advice.country-names-db",
              MissingEnrichmentAdvice(g).find("--db") != std::string::npos);
        g.geoIpPath = L"x.mmdb";
        Check(r, "enrich.advice.country-ok-with-db",
              MissingEnrichmentAdvice(g).empty());

        ListOptions t;
        t.filter = L"tx:1GB";
        Check(r, "enrich.advice.traffic-names-switch",
              MissingEnrichmentAdvice(t).find("--traffic") != std::string::npos);
        t.traffic = true;
        Check(r, "enrich.advice.traffic-ok-with-switch",
              MissingEnrichmentAdvice(t).empty());

        ListOptions q;
        q.filter = L"host:cdn";
        q.quiet = true;
        Check(r, "enrich.advice.quiet-stays-silent",
              MissingEnrichmentAdvice(q).empty());

        ListOptions d;
        d.filter = L"duration:1h";
        Check(r, "enrich.advice.duration-exempt",
              MissingEnrichmentAdvice(d).empty());

        ListOptions e;   // no filter at all
        Check(r, "enrich.advice.no-filter-none",
              MissingEnrichmentAdvice(e).empty());

        // The columns half (D13): a printed enrichment column whose switch
        // is off names the switch instead of printing a silent column of
        // em-dashes. The filter half above never covered this - `list
        // --columns rx` with no --traffic explained nothing.
        ListOptions ch;
        ch.columns.push_back(COL_HOST);
        const std::string chAdv = MissingEnrichmentAdvice(ch);
        Check(r, "enrich.advice.column-host-needs-dns",
              chAdv.find("column:") != std::string::npos &&
                  chAdv.find("--dns") != std::string::npos, chAdv);
        ch.dns = true;
        Check(r, "enrich.advice.column-host-ok-with-dns",
              MissingEnrichmentAdvice(ch).empty(),
              MissingEnrichmentAdvice(ch));

        ListOptions cr;
        cr.columns.push_back(COL_RX);
        Check(r, "enrich.advice.column-rx-needs-traffic",
              MissingEnrichmentAdvice(cr).find("--traffic") != std::string::npos,
              MissingEnrichmentAdvice(cr));
        cr.traffic = true;
        Check(r, "enrich.advice.column-rx-ok-with-traffic",
              MissingEnrichmentAdvice(cr).empty(),
              MissingEnrichmentAdvice(cr));

        ListOptions cq;
        cq.columns.push_back(COL_COUNTRY);
        cq.quiet = true;
        Check(r, "enrich.advice.column-quiet-stays-silent",
              MissingEnrichmentAdvice(cq).empty(),
              MissingEnrichmentAdvice(cq));
    }

    // 3d3. The pre-join view. A filter with an enrichment clause had NO view
    //      applied when EnrichViewForList returned: the pre-join selection
    //      works off the flat rows (the joined columns were empty then), and
    //      export renders store.View() directly. Measured before the fix:
    //      `export --dns --filter "host:com"` wrote a silent 0-row file while
    //      `list` with the same filter printed 55 rows, and D3's head hint
    //      saw view=0 on every pre-join filter. The joins run inside
    //      EnrichViewForList, so the view must be applied after them.
    //
    //      D3's own firing is asserted here too, in the deterministic form:
    //      DNS is switched on and COL_HOST is printed (D13 scopes the hint to
    //      per-address enrichment on a row that is actually shown), and both
    //      rows have an EMPTY remote address so the join has no candidate -
    //      the selftest must never put a DNS query on the wire.
    {
        std::vector<Connection> rows;
        Connection listen = MakeReferenceTcpRow();
        listen.pid = 1;             // sorts to the head under the PID default
        listen.remotePort = 0;      // listener: RowCanEnrich can never hold
        listen.localPort = 51750;   // distinct 4-tuple from the row below
        listen.remoteAddress.clear();  // no join candidate (offline selftest)
        rows.push_back(listen);
        Connection peer = MakeReferenceTcpRow();  // same reference endpoint
        peer.remoteAddress.clear();               // ditto: no DNS on the wire
        rows.push_back(peer);

        ConnectionStore s;
        s.ReplaceSnapshot(rows);
        s.SetTraffic(1, 0, 0);    // known zeroes: net:0 must match these rows
        s.SetTraffic(42, 0, 0);   // whether or not the live sampler finds them

        ListOptions o;
        o.filter = L"net:0";      // enrichment clause -> pre-join path
        o.traffic = true;         // a join runs (samples pids; may stay 0)
        o.dns = true;             // per-address join: D3's territory (D13)
        o.columns.push_back(COL_HOST);  // ...and a printed host column
        o.limit = 1;
        std::wstring jerr;
        std::string advice;
        const bool ok = EnrichViewForList(s, o, &jerr, &advice);
        Check(r, "enrich.view.prejoin-applies-filtered-view",
              ok && s.View().size() == 2,
              "view=" + std::to_string(s.View().size()));
        // D3 walks that view: the head is the listener, so the hint must
        // fire - it could not even see the row before the view was applied -
        // and it must style itself as an observation, not an error (D13).
        Check(r, "enrich.view.prejoin-d3-sees-the-head",
              advice.find("note: none of the") != std::string::npos &&
                  advice.find("can be enriched") != std::string::npos,
              advice);
    }

    // 3e. Kernel ages (G3-B). A single-shot run has no "first seen" of its own,
    //     so the only honest age is the kernel's ConnectionTimeMs, backdated
    //     onto the row. The two rules that matter: it may only move the row's
    //     age BACKWARD (a shorter reading must not make a connection look
    //     younger than already proven), and nonsense must be ignored rather
    //     than printed as "400d".
    {
        ConnectionStore s2;
        std::vector<Connection> rows2;
        Connection live = MakeReferenceTcpRow();     // 192.168.1.10:51752 -> 203.0.113.9:443
        rows2.push_back(live);
        Connection udpRow = MakeReferenceTcpRow();
        udpRow.protocol = IPPROTO_UDP;
        udpRow.localPort = 5353;
        udpRow.remotePort = 0;
        rows2.push_back(udpRow);
        s2.ReplaceSnapshot(rows2);
        const ULONGLONG freshTick = s2.Rows()[0].firstSeenTick;
        Check(r, "kernel-age.baseline-is-fresh", freshTick != 0);

        const ULONGLONG oneHour = 60ull * 60ull * 1000ull;
        std::vector<SocketAge> ages;
        SocketAge a;
        a.localAddress = L"192.168.1.10";
        a.localPort = 51752;
        a.remoteAddress = L"203.0.113.9";
        a.remotePort = 443;
        a.ageMs = oneHour;
        a.known = true;
        ages.push_back(a);
        const int n = s2.ApplyKernelAges(ages);
        Check(r, "kernel-age.backdates-the-row", n == 1 &&
              s2.Rows()[0].firstSeenTick < freshTick);

        // A much SHORTER age must not pull the row forward again: a reused
        // handle or a bad sample cannot make a 1h-old row look 1s old.
        SocketAge shorter = a;
        shorter.ageMs = 1000;
        ages.clear();
        ages.push_back(shorter);
        s2.ApplyKernelAges(ages);
        Check(r, "kernel-age.never-moves-forward",
              s2.Rows()[0].firstSeenTick < freshTick - oneHour / 2);

        // Nonsense: an age beyond the uptime, and an unknown sample.
        SocketAge absurd = a;
        absurd.ageMs = ~0ull / 2;
        SocketAge unknown = a;
        unknown.known = false;
        ages.clear();
        ages.push_back(absurd);
        ages.push_back(unknown);
        const ULONGLONG before = s2.Rows()[0].firstSeenTick;
        s2.ApplyKernelAges(ages);
        Check(r, "kernel-age.ignores-nonsense", s2.Rows()[0].firstSeenTick == before);

        // UDP carries no TCP_INFO_v0, so a UDP row must never be aged by a
        // sample that happens to share its ports.
        SocketAge udpSample = a;
        udpSample.localPort = 5353;
        udpSample.remotePort = 0;
        const ULONGLONG udpBefore = s2.Rows()[1].firstSeenTick;
        ages.clear();
        ages.push_back(udpSample);
        s2.ApplyKernelAges(ages);
        Check(r, "kernel-age.skips-udp", s2.Rows()[1].firstSeenTick == udpBefore);

        // An endpoint nobody has is not an error, it is just unmatched.
        SocketAge ghost = a;
        ghost.localPort = 12345;
        ages.clear();
        ages.push_back(ghost);
        Check(r, "kernel-age.unmatched-is-harmless",
              s2.ApplyKernelAges(ages) == 0);
    }

    // 4b. DUPLICATE IDENTITIES (D20). The 4-tuple is not unique on a real
    // machine - mDNS alone gave 62 rows sharing UDPv4|0.0.0.0:5353|*|* here -
    // so the diff must pair N fresh rows with N previous rows instead of
    // collapsing them onto one. Before this, a repeat snapshot of N identical
    // sockets reported N-1 DISAPPEAR ghosts and re-created all N as new, on
    // every single refresh, forever: `--changes` printed 173 false DISAPPEAR
    // and 0 APPEAR per run, and the GUI flashed rows red then green without
    // end.
    {
        // 12 identical mDNS-style sockets, the shape that actually occurs.
        const size_t kDupes = 12;
        auto mdnSockets = [](size_t count) {
            std::vector<Connection> v;
            for (size_t i = 0; i < count; ++i) {
                Connection c;
                c.family = AF_INET;
                c.protocol = IPPROTO_UDP;
                c.pid = 4242;                 // same owner too
                c.localAddress = L"0.0.0.0";
                c.localPort = 5353;
                c.remoteAddress = L"0.0.0.0";
                c.remotePort = 0;
                c.state = 0;
                ::InetPtonW(AF_INET, L"0.0.0.0", &c.local4);
                ::InetPtonW(AF_INET, L"0.0.0.0", &c.remote4);
                v.push_back(c);
            }
            return v;
        };

        ConnectionStore dup;
        dup.ReplaceSnapshot(mdnSockets(kDupes));
        dup.TakeChangeEvents();               // baseline: N APPEAR, expected
        Check(r, "dupe.baseline-counts-every-row",
              dup.Rows().size() == kDupes,
              "rows=" + std::to_string(dup.Rows().size()));

        // The whole point: an UNCHANGED repeat snapshot must be silent.
        dup.ReplaceSnapshot(mdnSockets(kDupes));
        const std::vector<RowChange> quiet = dup.TakeChangeEvents();
        Check(r, "dupe.repeat-snapshot-is-quiet", quiet.empty(),
              "events=" + std::to_string(quiet.size()));
        Check(r, "dupe.repeat-keeps-every-row",
              dup.Rows().size() == kDupes,
              "rows=" + std::to_string(dup.Rows().size()));
        Check(r, "dupe.repeat-no-ghosts", [&dup]() {
            for (const Connection& c : dup.Rows())
                if (c.flags & kRowRemoved) return false;
            return true;
        }());

        // ...and every row must have kept its own id, so the GUI does not
        // repaint a stable table (rows carry ids across refreshes by design).
        {
            std::vector<std::uint64_t> ids;
            for (const Connection& c : dup.Rows()) ids.push_back(c.id);
            std::sort(ids.begin(), ids.end());
            const bool distinct = std::adjacent_find(ids.begin(), ids.end()) ==
                                  ids.end();
            Check(r, "dupe.ids-are-distinct", distinct);
        }

        // Genuine churn on top of the duplicates: one socket gone, one new.
        // Exactly one DISAPPEAR and one APPEAR - not N-1 of each.
        {
            std::vector<Connection> changed = mdnSockets(kDupes);
            changed.pop_back();               // one fewer
            Connection extra;
            extra.family = AF_INET;
            extra.protocol = IPPROTO_UDP;
            extra.pid = 4242;
            extra.localAddress = L"0.0.0.0";
            extra.localPort = 5354;           // a different port = new identity
            extra.remoteAddress = L"0.0.0.0";
            extra.remotePort = 0;
            ::InetPtonW(AF_INET, L"0.0.0.0", &extra.local4);
            ::InetPtonW(AF_INET, L"0.0.0.0", &extra.remote4);
            changed.push_back(extra);
            dup.ReplaceSnapshot(changed);
            int appears = 0, disappears = 0;
            for (const RowChange& ch : dup.TakeChangeEvents()) {
                if (ch.kind == kChangeAppear) ++appears;
                else if (ch.kind == kChangeDisappear) ++disappears;
            }
            Check(r, "dupe.churn-is-exactly-one-of-each",
                  appears == 1 && disappears == 1,
                  "a=" + std::to_string(appears) +
                      " d=" + std::to_string(disappears));
        }

        // The same PID on two DIFFERENT ports must also stay two rows: this
        // is the case the PID in the key exists for, and it is what
        // "keyed by endpoint+PID" in the README claims.
        {
            ConnectionStore two;
            auto twoRows = [](UINT lport) {
                Connection c;
                c.family = AF_INET;
                c.protocol = IPPROTO_TCP;
                c.pid = 77;
                c.state = MIB_TCP_STATE_ESTAB;
                c.localAddress = L"10.0.0.5";
                c.localPort = lport;
                c.remoteAddress = L"1.2.3.4";
                c.remotePort = 443;
                ::InetPtonW(AF_INET, L"10.0.0.5", &c.local4);
                ::InetPtonW(AF_INET, L"1.2.3.4", &c.remote4);
                return c;
            };
            std::vector<Connection> v;
            v.push_back(twoRows(5000));
            v.push_back(twoRows(5001));
            two.ReplaceSnapshot(v);
            two.TakeChangeEvents();
            two.ReplaceSnapshot(v);
            Check(r, "dupe.same-pid-diff-port-stable",
                  two.TakeChangeEvents().empty() && two.Rows().size() == 2,
                  "rows=" + std::to_string(two.Rows().size()));
        }
    }

    // 4c. PER-SOCKET BYTES + RATE (D21). The Speed column and `speed:` filter
    // were dead: nothing in the tree ever set Connection::perRowBytes, so
    // ComputeBps was unreachable and the column printed an em-dash on every
    // tick of every watch while `help list` advertised it. These checks pin
    // the whole chain: a per-socket sample marks the row, two samples produce
    // a real rate, and a per-PID total is refused the flag.
    {
        ConnectionStore sp;
        auto row = [](UINT lport, const wchar_t* peer) {
            Connection c;
            c.family = AF_INET;
            c.protocol = IPPROTO_TCP;
            c.pid = 5150;
            c.state = MIB_TCP_STATE_ESTAB;
            c.localAddress = L"10.0.0.92";
            c.localPort = lport;
            c.remoteAddress = peer;
            c.remotePort = 443;
            ::InetPtonW(AF_INET, L"10.0.0.92", &c.local4);
            ::InetPtonW(AF_INET, peer, &c.remote4);
            return c;
        };
        std::vector<Connection> v;
        v.push_back(row(50000, L"93.184.216.34"));
        v.push_back(row(50001, L"1.1.1.1"));
        sp.ReplaceSnapshot(v);
        sp.TakeChangeEvents();

        // A PER-PID total must NOT be enough: the flag stays clear and no
        // rate is invented from a process total.
        sp.SetTraffic(5150, 5000000, 1000);
        Check(r, "rate.pid-total-marks-nothing",
              !sp.Rows()[0].perRowBytes && sp.ComputeRates() == 0 &&
                  !sp.Rows()[0].bpsKnown);

        // First per-socket sample: the row is marked, but there is no rate yet
        // - a rate needs TWO readings, and printing one from a single sample
        // would be an invention.
        std::vector<SocketBytes> sb;
        SocketBytes b1;
        b1.localAddress = L"10.0.0.92"; b1.localPort = 50000;
        b1.remoteAddress = L"93.184.216.34"; b1.remotePort = 443;
        b1.rx = 1000; b1.tx = 100; b1.known = true;
        sb.push_back(b1);
        Check(r, "rate.per-socket-marks-the-row", sp.ApplySocketBytes(sb) == 1);
        Check(r, "rate.first-sample-has-no-rate",
              sp.Rows()[0].perRowBytes && !sp.Rows()[0].bpsKnown &&
                  sp.ComputeRates() == 0);

        // Second sample: now there is a real delta, and the row the sample
        // does not cover must be left alone rather than given a share.
        ::Sleep(60);
        SocketBytes b2 = b1;
        b2.rx = 1100; b2.tx = 150;
        sb.clear();
        sb.push_back(b2);
        sp.ApplySocketBytes(sb);
        const int known = sp.ComputeRates();
        Check(r, "rate.second-sample-produces-a-rate", known == 1,
              "known=" + std::to_string(known));
        Check(r, "rate.value-is-the-real-delta",
              sp.Rows()[0].bpsKnown && sp.Rows()[0].rxBps > 0.0 &&
                  sp.Rows()[0].txBps > 0.0 &&
                  sp.Rows()[0].rxBps < 100000.0);
        Check(r, "rate.uncovered-row-stays-unknown",
              !sp.Rows()[1].perRowBytes && !sp.Rows()[1].bpsKnown);

        // A per-socket total must WIN over the per-PID total for the row it
        // covers: it is the more specific fact about the same cell.
        Check(r, "rate.per-socket-wins-over-per-pid",
              sp.Rows()[0].trafficRx == 1100 && sp.Rows()[0].trafficTx == 150,
              "rx=" + std::to_string(sp.Rows()[0].trafficRx));

        // A counter that went backwards means a recycled socket: report no
        // reading for that interval rather than an astronomical spike.
        ::Sleep(30);
        SocketBytes back = b2;
        back.rx = 5;
        sb.clear();
        sb.push_back(back);
        sp.ApplySocketBytes(sb);
        Check(r, "rate.recycled-socket-reports-nothing",
              sp.ComputeRates() == 0 && !sp.Rows()[0].bpsKnown);

        // ClearTraffic must drop the flag too, or a row would keep claiming a
        // per-socket source that is no longer running.
        sp.ClearTraffic();
        Check(r, "rate.clear-drops-the-flag",
              !sp.Rows()[0].perRowBytes && !sp.Rows()[0].bpsKnown &&
                  sp.Rows()[0].trafficRx == 0);
    }

    // 3b. G5: the per-PROCESS rate. The rule being pinned is that it is a SUM
    //     over the process's socket-counted rows and never a division - and the
    //     test is built so the two would give different answers, because a test
    //     that cannot distinguish the right method from the plausible wrong one
    //     proves nothing.
    {
        ConnectionStore gp;
        // Two rows, ONE process, deliberately unequal traffic: 900 and 100.
        //   sum      = 1000  -> a rate of 1000/elapsed
        //   division = 1000/2 = 500 -> a rate of 500/elapsed
        // A browser-like split, which is the real shape: one big transfer and
        // one small keepalive.
        std::vector<Connection> rows;
        for (int i = 0; i < 2; ++i) {
            Connection c = MakeReferenceTcpRow();
            c.pid = 4242;
            c.localPort = 50000 + i;
            rows.push_back(c);
        }
        gp.ReplaceSnapshot(rows);
        // The rows are FINALIZED (MakeReferenceTcpRow calls FinalizeRow) and
        // carry identity fields that survive the copy: pid, ports, addresses.

        // The 4-tuples the rows actually have, read back from the store rather
        // than re-derived here. Deriving them a second time is how the first
        // version of this case got them wrong (it invented 192.168.1.10:
        // 50000+i) and every join then silently matched nothing - which fails
        // with "0", indistinguishable from an arithmetic bug. Reading the row's
        // own values cannot drift from the row.
        std::vector<SocketBytes> sb;
        for (const Connection& row : gp.Rows()) {
            SocketBytes b;
            b.localAddress = row.localAddress;
            b.localPort = row.localPort;
            b.remoteAddress = row.remoteAddress;
            b.remotePort = row.remotePort;
            b.rx = (row.localPort == 51752) ? 900 : 100;
            b.tx = (row.localPort == 51752) ? 90 : 10;
            b.known = true;
            sb.push_back(b);
        }
        gp.ApplySocketBytes(sb);
        Check(r, "g5.first-sample-has-no-rate", gp.ComputeRates() == 0 &&
              !gp.Rows()[0].groupBpsKnown);

        ::Sleep(60);
        // Both sockets gain 300 bytes over the interval: the process moved 600,
        // and each socket moved 300. A per-connection rate can only ever see
        // 300; a divided per-PID total would also give 300 (1000/2 * 0.6); only
        // the sum gives 600. So this single assertion separates all three.
        for (SocketBytes& b : sb) { b.rx += 300; b.tx += 30; }
        gp.ApplySocketBytes(sb);
        gp.ComputeRates();
        // Pick the row that was actually socket-counted. Index order is not the order
        // the join wrote, and asserting on rows_[0] blindly is how the first
        // version of this case reported "group=0 row=0" for a rate that had
        // been computed correctly on the other row.
        const Connection* counted = nullptr;
        for (const Connection& c : gp.Rows())
            if (c.perRowBytes && c.bpsKnown) { counted = &c; break; }
        const bool found = counted != nullptr;
        const double groupTotal = found ? counted->groupRxBps + counted->groupTxBps : 0.0;
        const double rowTotal = found ? counted->rxBps + counted->txBps : 0.0;
        Check(r, "g5.group-rate-is-the-sum-not-the-average",
              found && counted->groupBpsKnown && groupTotal > rowTotal * 1.9,
              found ? ("group=" + std::to_string(static_cast<long>(groupTotal)) +
                       " row=" + std::to_string(static_cast<long>(rowTotal)))
                    : std::string("no socket-counted row had a rate"));
        // The SUM is 330 rx+tx per socket-pair step: 600 + 60. The average would
        // be half. Assert the magnitude FLOOR too, so a future change that
        // keeps "bigger than the row rate" but scales wrongly still fails.
        // Floor only, no ceiling: the rate divides by the measured wall-clock
        // interval, and a loaded machine sleeps longer than asked — 14042 was
        // observed on a busy host for a nominally-60 ms sleep, failing a
        // 13000 ceiling that proved nothing about correctness. Absolute upper
        // bounds on wall-clock-derived values are flaky by construction; the
        // ratio check above is what pins the arithmetic.
        Check(r, "g5.group-rate-magnitude",
              groupTotal > 8000.0,
              "groupTotal=" + std::to_string(static_cast<long>(groupTotal)));
        // EVERY row of the process carries the same process figure - that is
        // what makes the group renderer's MAXIMUM-not-sum aggregation correct.
        Check(r, "g5.same-process-same-rate",
              gp.Rows()[0].groupBpsKnown && gp.Rows()[1].groupBpsKnown &&
                  gp.Rows()[0].groupRxBps == gp.Rows()[1].groupRxBps &&
                  gp.Rows()[0].groupTxBps == gp.Rows()[1].groupTxBps);
        // A process whose sockets were NOT read has no group rate, even though
        // its own socket IS readable when it is read. The negative case matters
        // as much as the positive one: a rate invented for a process we could
        // not measure is the exact failure perRowBytes was introduced to stop.
        ConnectionStore etw;
        etw.ReplaceSnapshot(rows);
        etw.SetTraffic(4242, 5000, 500);   // per-PID join: sets no perRowBytes
        etw.ComputeRates();
        Check(r, "g5.per-pid-source-has-no-group-rate",
              !etw.Rows()[0].groupBpsKnown && !etw.Rows()[1].groupBpsKnown);
    }

    // 3d. D29: the "may this process be ended" rule.
    //
    //     This is a pure function specifically so the rule can be pinned here
    //     rather than only exercised through a modal dialog on a live machine.
    //     The bug it guards against was a crash the user hit by hand, so a
    //     regression test that needs a GUI to run would be a test that never
    //     runs.
    {
        const DWORD self = 4242;
        // The self-kill. Reachable with no memory bug at all: wintcp.exe holds
        // its own connections, so it has its own rows, and "End process" on one
        // of them used to end WinTCP.
        Check(r, "d29.self-is-refused",
              PidKillVerdict(self, self, true) == PidVerdict::IsSelf);
        // ...and the refusal explains itself rather than being a bare false.
        Check(r, "d29.self-refusal-has-text",
              PidKillRefusal(PidVerdict::IsSelf) != nullptr &&
                  wcslen(PidKillRefusal(PidVerdict::IsSelf)) > 0);
        // A different process is fine, which is the half that would break if
        // the self-check were written as "refuse everything".
        Check(r, "d29.others-allowed",
              PidKillVerdict(1000, self, true) == PidVerdict::Ok &&
                  PidKillVerdict(4, self, true) != PidVerdict::Ok);
        // Pseudo-PIDs have no image to end. PID 0 arrives on every TIME_WAIT
        // and listening-wildcard row, so this is not a corner case.
        Check(r, "d29.pseudo-refused",
              PidKillVerdict(0, self, false) == PidVerdict::Pseudo &&
                  PidKillVerdict(4, self, true) == PidVerdict::Pseudo);
        // Every non-Ok verdict must carry a message. A refusal the user cannot
        // read is indistinguishable from a hang, and this one replaced a
        // silent wrong-process kill.
        Check(r, "d29.every-refusal-is-explainable",
              PidKillRefusal(PidVerdict::Ok) == nullptr ||
                      PidKillRefusal(PidVerdict::Ok)[0] == L'\0');
        for (PidVerdict v : {PidVerdict::IsSelf, PidVerdict::Pseudo,
                             PidVerdict::NotPermitted}) {
            Check(r, "d29.refusal-text-present",
                  PidKillRefusal(v) != nullptr &&
                      PidKillRefusal(v)[0] != L'\0');
        }
        // The rule must be a function of BOTH pids, not of a global. Two
        // processes, each self to itself and "other" to the other, is the
        // property that makes the same code safe in the GUI and the CLI.
        Check(r, "d29.verdict-is-symmetric-in-the-caller",
              PidKillVerdict(100, 100, true) == PidVerdict::IsSelf &&
                  PidKillVerdict(100, 200, true) == PidVerdict::Ok &&
                  PidKillVerdict(200, 100, true) == PidVerdict::Ok);
    }

    // 3d. R2: worker containment. RunGuarded is pure — no threads, no window —
    //     so the policy is pinned headlessly and the call sites (refresh, DNS,
    //     ETW, scan pool) only have to invoke it. A containment helper that
    //     itself throws is worse than none, so the first property tested is
    //     that success is silent and failures are values, not exits.
    {
        bool ran = false;
        Check(r, "r2.success-is-silent",
              RunGuarded([&] { ran = true; }).empty() && ran);
        const std::string stdErr = RunGuarded([] {
            throw std::runtime_error("synthetic");
        });
        Check(r, "r2.std-exception-is-named",
              stdErr.find("synthetic") != std::string::npos,
              stdErr);
        const std::string unknownErr = RunGuarded([] { throw 42; });
        Check(r, "r2.unknown-exception-is-labelled",
              unknownErr.find("unknown") != std::string::npos,
              unknownErr);
        // Errors are distinct values, not one shared string: a worker that
        // reports "failed" without saying how is only marginally better than
        // one that dies.
        Check(r, "r2.errors-are-distinct", stdErr != unknownErr);
    }

    // 3e. R8: the watchdog policy. Pure timestamps in, verdict out — the
    //     window only executes. Every bound is asserted here so the policy
    //     cannot drift without a failing test saying which side moved.
    {
        using WA = WatchdogAction;
        // Fresh results are healthy at any cadence, including a zero
        // interval (the floor dominates: max(4*0, 15000) = 15000).
        Check(r, "r8.fresh-is-healthy",
              RefreshWatchdogNext(100000, 99000, 2000, 0) == WA::Healthy &&
                  RefreshWatchdogNext(100000, 99000, 0, 0) == WA::Healthy);
        // The floor dominates fast cadences: 4 x 2 s = 8 s would
        // false-positive on two consecutive slow passes, so 10 s of silence
        // at a 2 s cadence is still healthy and 16 s is stale.
        Check(r, "r8.floor-dominates-fast-cadence",
              RefreshWatchdogNext(100000, 90000, 2000, 0) == WA::Healthy &&
                  RefreshWatchdogNext(106000, 90000, 2000, 0) ==
                      WA::StaleRestart);
        // The factor dominates slow cadences: at 60 s, four missed ticks is
        // four minutes of dead UI, so the threshold is 4x60 = 240 s, and
        // 200 s of silence is still (just) healthy.
        Check(r, "r8.factor-dominates-slow-cadence",
              RefreshWatchdogNext(300000, 100000, 60000, 0) == WA::Healthy &&
                  RefreshWatchdogNext(400000, 100000, 60000, 0) ==
                      WA::StaleRestart);
        // At the cap, staleness gives up instead of restarting forever. Below
        // the cap it restarts — including at cap-1, which is the boundary
        // that would silently become "restart forever" if written as >.
        Check(r, "r8.cap-gives-up",
              RefreshWatchdogNext(500000, 100000, 2000,
                                  kMaxWatchdogRestarts) == WA::StaleGiveUp &&
                  RefreshWatchdogNext(500000, 100000, 2000,
                                      kMaxWatchdogRestarts - 1) ==
                      WA::StaleRestart);
        // Recovery forgives: with a fresh base the same restart count is
        // healthy, which is what lets the caller reset history on recovery
        // instead of dooming the fourth restart hours later.
        Check(r, "r8.recovery-forgives",
              RefreshWatchdogNext(500000, 499000, 2000,
                                  kMaxWatchdogRestarts) == WA::Healthy);
        // A clock going backwards (base in the future) is age zero, never
        // stale: GetTickCount64 is monotonic, but the comparison must still
        // be total — a stuck future stamp must not read as "overdue by 2^64".
        Check(r, "r8.future-base-is-healthy",
              RefreshWatchdogNext(100000, 200000, 2000, 0) == WA::Healthy);
    }

    // 3f. C2: the IPv6 scope-suffix rule. Pure string/scalar logic over raw
    //     bytes, so the rule is pinned exactly — and it is the rule whose two
    //     copies disagreed, where disagreement silently breaks a join key.
    {
        // fe80::1 as 16 network-order bytes, and 2001:db8::1 for contrast.
        unsigned char linkLocal[16] = {0};
        linkLocal[0] = 0xFE; linkLocal[1] = 0x80; linkLocal[15] = 0x01;
        unsigned char global[16] = {0};
        global[0] = 0x20; global[1] = 0x01; global[2] = 0x0D; global[3] = 0xB8;
        global[15] = 0x01;
        // The /10 boundary: fe80:: through febf:: are link-local, fec0:: is not.
        unsigned char justOutside[16] = {0};
        justOutside[0] = 0xFE; justOutside[1] = 0xC0; justOutside[15] = 0x01;

        Check(r, "c2.link-local-gets-suffix",
              Ipv6ScopeSuffix(linkLocal, 12) == L"%12");
        // THE regression: the socket scan used to append a suffix for ANY
        // nonzero scope id while the store appended only for /10, so a
        // non-link-local socket with a scope id produced two spellings of one
        // address and the age join missed. Global + nonzero scope must be bare.
        Check(r, "c2.global-suppresses-suffix",
              Ipv6ScopeSuffix(global, 12).empty(),
              WideToUtf8(Ipv6ScopeSuffix(global, 12)));
        Check(r, "c2.outside-the-10-suppresses-suffix",
              Ipv6ScopeSuffix(justOutside, 3).empty());
        // Scope 0 means "no scope": never print "%0", which is a different
        // string from the bare address and would break the key the same way.
        Check(r, "c2.zero-scope-is-bare",
              Ipv6ScopeSuffix(linkLocal, 0).empty() &&
                  Ipv6ScopeSuffix(global, 0).empty());
        // A null byte pointer must not be dereferenced: the callers pass
        // sin6_addr.s6_addr / ucLocalAddr, and defensive here is free.
        Check(r, "c2.null-address-is-safe",
              Ipv6ScopeSuffix(nullptr, 12).empty());
    }

    // 3c. G6: the TCP_INFO columns. Everything here is pure formatting or
    //     join logic, so it can be pinned exactly - the part that needs a live
    //     socket (do the fields arrive at all, and in what unit) was measured
    //     separately with temp/d2probe.cpp and is recorded in todo.md.
    {
        // (a) THE UNIT CONVERSION. RttUs is microseconds; the column must show
        //     milliseconds, because it is named for `ss -i` and `ss -i` prints
        //     ms. Getting this wrong reports a 24 ms RTT as 24000, and the
        //     mistake is invisible in a table because it still looks like a
        //     number.
        // The fractional rule: ONE decimal digit, from (ms % 1000) / 100. So 1250ms
        // is "1.2" (not 1.3 - that would be rounding to the nearest tenth, a
        // different and equally defensible rule, but it must be ONE rule). The
        // explicit "!=" pins that choice so it cannot drift silently.
        Check(r, "g6.rtt-formats-as-milliseconds",
              FormatRttMs(24) == L"24" && FormatRttMs(0) == L"<1" &&
                  FormatRttMs(1250) == L"1.2" && FormatRttMs(1250) != L"1.25" &&
                  FormatRttMs(1000) == L"1" && FormatRttMs(999) == L"999",
              "1250ms -> " + WideToUtf8(FormatRttMs(1250)));
        // Sub-millisecond must NOT print as "0": a zero-latency reading is
        // physically impossible and a user would report it as a bug. "<1" is
        // what `ss` shows.
        Check(r, "g6.zero-rtt-is-not-zero-latency", FormatRttMs(0) == L"<1");

        // (b) THE JOIN, on a full 4-tuple.
        ConnectionStore ts;
        std::vector<Connection> t = {MakeReferenceTcpRow()};
        ts.ReplaceSnapshot(t);
        std::vector<SocketTcpInfo> ti;
        SocketTcpInfo info;
        // MakeReferenceTcpRow's ACTUAL 4-tuple (192.168.1.10:51752 ->
        // 203.0.113.9:443). The join matches on all four parts, so a made-up
        // address here matches nothing and every G6 assertion below fails with
        // a dash - a test-data mistake that looks exactly like a code bug.
        info.localAddress = L"192.168.1.10";
        info.localPort = 51752;
        info.remoteAddress = L"203.0.113.9";
        info.remotePort = 443;
        info.rttMs = 24;
        info.minRttMs = 16;
        info.cwnd = 16922;
        info.retransBytes = 2525;
        info.rttKnown = true;
        info.cwndKnown = true;
        info.retransKnown = true;
        info.timestamps = false;      // measured: this host reports ts=0
        info.timestampsKnown = true;
        ti.push_back(info);
        Check(r, "g6.join-hits-the-row", ts.ApplySocketTcpInfo(ti) == 1);

        wchar_t buf[128] = {0};
        ConnectionStore::GetColumnText(ts.Rows()[0], COL_RTT, buf, 128);
        const std::wstring rttCell(buf);
        ConnectionStore::GetColumnText(ts.Rows()[0], COL_MINRTT, buf, 128);
        const std::wstring minCell(buf);
        ConnectionStore::GetColumnText(ts.Rows()[0], COL_RETRANS, buf, 128);
        const std::wstring retxCell(buf);
        Check(r, "g6.columns-render-the-joined-values",
              rttCell == L"24" && minCell == L"16" && retxCell == L"2.5 KB",
              "rtt=" + WideToUtf8(rttCell) + " min=" + WideToUtf8(minCell) +
                  " retx=" + WideToUtf8(retxCell));

        // (c) ZERO RETRANSMIT IS A REAL ANSWER, not "unknown". A dash here would
        //     hide the healthiest possible result - "this connection has never
        //     retransmitted" is the thing you want to see.
        Check(r, "g6.zero-retrans-is-not-unknown",
              ts.Rows()[0].retransKnown && ts.Rows()[0].retransBytes == 2525);
        ConnectionStore zero;
        std::vector<Connection> z = {MakeReferenceTcpRow()};
        zero.ReplaceSnapshot(z);
        SocketTcpInfo zinfo = info;
        zinfo.retransBytes = 0;
        zinfo.retransKnown = true;
        std::vector<SocketTcpInfo> zt = {zinfo};
        zero.ApplySocketTcpInfo(zt);
        ConnectionStore::GetColumnText(zero.Rows()[0], COL_RETRANS, buf, 128);
        Check(r, "g6.zero-retrans-renders-as-zero", std::wstring(buf) == L"0 B",
              WideToUtf8(std::wstring(buf)));

        // (d) PER-FIELD, not all-or-nothing. The kernel populates these
        //     independently: a socket with TCP timestamps off still has a real
        //     congestion window (measured on this host - all three readable
        //     sockets reported ts=0 and two had a good RTT and cwnd). A blanket
        //     gate would show four dashes for a row with three good readings.
        ConnectionStore partial;
        std::vector<Connection> pr = {MakeReferenceTcpRow()};
        partial.ReplaceSnapshot(pr);
        SocketTcpInfo pinfo = info;
        pinfo.rttKnown = false;        // no RTT available...
        pinfo.minRttMs = 0;
        pinfo.cwndKnown = true;        // ...but a perfectly real cwnd
        std::vector<SocketTcpInfo> pt = {pinfo};
        partial.ApplySocketTcpInfo(pt);
        ConnectionStore::GetColumnText(partial.Rows()[0], COL_RTT, buf, 128);
        const std::wstring noRtt(buf);
        ConnectionStore::GetColumnText(partial.Rows()[0], COL_CWND, buf, 128);
        const std::wstring withCwnd(buf);
        Check(r, "g6.per-field-not-all-or-nothing",
              noRtt == L"—" && withCwnd != L"—" && withCwnd != L"",
              "rtt=" + WideToUtf8(noRtt) + " cwnd=" + WideToUtf8(withCwnd));

        // (e) A CUMULATIVE value must not go BACKWARDS. A smaller reading means
        //     a recycled socket, not new information; letting it overwrite
        //     would reset the connection's retransmit history on every handle
        //     reuse. Min RTT has the mirror-image rule: it only ever decreases.
        ConnectionStore mono;
        std::vector<Connection> mo = {MakeReferenceTcpRow()};
        mono.ReplaceSnapshot(mo);
        mono.ApplySocketTcpInfo(ti);
        SocketTcpInfo lower = info;
        lower.retransBytes = 10;       // smaller: must be ignored
        lower.minRttMs = 900;          // LARGER: must be ignored too
        std::vector<SocketTcpInfo> lt = {lower};
        mono.ApplySocketTcpInfo(lt);
        Check(r, "g6.cumulative-never-goes-backwards",
              mono.Rows()[0].retransBytes == 2525 &&
                  mono.Rows()[0].minRttMs == 16,
              "retx=" + std::to_string(mono.Rows()[0].retransBytes) +
                  " min=" + std::to_string(mono.Rows()[0].minRttMs));
        // ...but an instantaneous one (cwnd) IS replaced.
        SocketTcpInfo shrink = info;
        shrink.cwnd = 64;
        std::vector<SocketTcpInfo> st = {shrink};
        mono.ApplySocketTcpInfo(st);
        Check(r, "g6.instantaneous-is-replaced",
              mono.Rows()[0].cwnd == 64);

        // (f) THE FILTERS. A threshold in the unit the column SHOWS, so a
        //     filter and a cell can never disagree. Unknown matches NOTHING -
        //     the same rule as `duration:` - or `rtt:` would silently select
        //     every row the kernel could not measure.
        ConnectionStore fl;
        std::vector<Connection> f1 = {MakeReferenceTcpRow()};
        fl.ReplaceSnapshot(f1);
        fl.ApplySocketTcpInfo(ti);
        Check(r, "g6.rtt-filter-is-threshold-in-ms",
              MatchFilter(fl.Rows()[0], Prog(L"rtt:20")) &&
                  !MatchFilter(fl.Rows()[0], Prog(L"rtt:30")) &&
                  MatchFilter(fl.Rows()[0], Prog(L"rtt:20-30")));
        Check(r, "g6.cwnd-filter-is-threshold-in-bytes",
              MatchFilter(fl.Rows()[0], Prog(L"cwnd:16000")) &&
                  !MatchFilter(fl.Rows()[0], Prog(L"cwnd:17000")));
        Check(r, "g6.retrans-filter",
              MatchFilter(fl.Rows()[0], Prog(L"retrans:2000")) &&
                  !MatchFilter(fl.Rows()[0], Prog(L"retrans:3000")));
        // A bare field name means "has this reading", NOT "matches everything".
        Check(r, "g6.bare-field-means-has-a-reading",
              MatchFilter(fl.Rows()[0], Prog(L"rtt:")) &&
                  MatchFilter(fl.Rows()[0], Prog(L"cwnd:")));
        // An UNMEASURED row must match none of them.
        ConnectionStore un;
        std::vector<Connection> u = {MakeReferenceTcpRow()};
        un.ReplaceSnapshot(u);
        Check(r, "g6.unmeasured-row-matches-no-g6-filter",
              !MatchFilter(un.Rows()[0], Prog(L"rtt:")) &&
                  !MatchFilter(un.Rows()[0], Prog(L"rtt:0")) &&
                  !MatchFilter(un.Rows()[0], Prog(L"cwnd:0")) &&
                  !MatchFilter(un.Rows()[0], Prog(L"retrans:0")));

        // A BYTE SUFFIX must be a threshold, not a hole. `retrans:1KB` used to
        // match EVERY row carrying a reading - including rows below 1024 bytes
        // - because the parse sat in an `if` inside the branch: ParseNumberRange
        // rejects a non-digit, cl.text was left empty, and an empty needle is a
        // match in HasLowerSubstring. So a threshold silently became "all of
        // them", straight against `help list`'s "never matches all". The unit
        // suffixes are now parsed, so this asserts both sides of the boundary.
        Check(r, "g6.byte-suffix-is-a-threshold",
              MatchFilter(fl.Rows()[0], Prog(L"retrans:1KB")) &&
                  !MatchFilter(fl.Rows()[0], Prog(L"retrans:3KB")) &&
                  MatchFilter(fl.Rows()[0], Prog(L"cwnd:1KB")) &&
                  !MatchFilter(fl.Rows()[0], Prog(L"cwnd:20KB")));

        // A value that is not a number at all falls through to the text clause
        // and matches nothing here. It must never fall back to "everything":
        // that is the same defect, reached by a different spelling.
        Check(r, "g6.unparsable-value-matches-nothing",
              !MatchFilter(fl.Rows()[0], Prog(L"rtt:100ms")) &&
                  !MatchFilter(fl.Rows()[0], Prog(L"retrans:abc")) &&
                  !MatchFilter(fl.Rows()[0], Prog(L"cwnd:zzz")));

        // (g) THE COLUMN NAMES round-trip, which is what stops `help list` and
        //     the README advertising a column that cannot be typed.
        Check(r, "g6.column-names-resolve",
              ColumnIdForName(L"rtt") == COL_RTT &&
                  ColumnIdForName(L"latency") == COL_RTT &&
                  ColumnIdForName(L"minrtt") == COL_MINRTT &&
                  ColumnIdForName(L"cwnd") == COL_CWND &&
                  ColumnIdForName(L"retrans") == COL_RETRANS &&
                  ColumnIdForName(L"procspeed") == COL_GROUPRATE &&
                  ColumnIdForName(L"groupspeed") == COL_GROUPRATE);
        // (h) The columns are RIGHT-aligned numbers, and every G6 column has a
        //     width budget so a streamed header and a padded row agree.
        Check(r, "g6.columns-are-right-aligned",
              GetColumnStyle(COL_RTT).right && GetColumnStyle(COL_MINRTT).right &&
                  GetColumnStyle(COL_CWND).right &&
                  GetColumnStyle(COL_RETRANS).right &&
                  GetColumnStyle(COL_RTT).cap > 0 &&
                  GetColumnStyle(COL_MINRTT).cap > 0);
    }

    // 3d. F5.1 / F5.2 / F5.3: the parent, integrity and signature columns.
    //
    // What is testable here is exactly the part that went wrong three times
    // while this was being built: the RID -> level mapping, the label wording
    // the filter and the column share, and the filter's refusal to answer for
    // a row it never measured. What is NOT testable without a live token is
    // whether OpenProcessToken succeeds - that was measured on the machine
    // (122 Medium / 115 System / 11 High / 1 Low / 2 Untrusted over 328 rows,
    // 77 unreadable because the process could not be opened at all) and is
    // recorded in todo.md rather than pinned here.
    {
        // (a) THE RID MAPPING, exhaustively over the documented values. Exact
        //     equality, not >= : an unfamiliar future RID must read as unknown
        //     rather than silently claiming the nearest known level.
        Check(r, "f5.2.rid-maps-to-named-level",
              IntegrityFromRid(0)     == kIntegrityUntrusted &&
              IntegrityFromRid(4096)  == kIntegrityLow &&
              IntegrityFromRid(8192)  == kIntegrityMedium &&
              IntegrityFromRid(12288) == kIntegrityHigh &&
              IntegrityFromRid(16384) == kIntegritySystem &&
              IntegrityFromRid(28672) == kIntegrityProtected);

        Check(r, "f5.2.unknown-rid-is-not-the-nearest-level",
              IntegrityFromRid(1) == kIntegrityUnknown &&
              IntegrityFromRid(6144) == kIntegrityUnknown &&
              IntegrityFromRid(12287) == kIntegrityUnknown &&
              IntegrityFromRid(20480) == kIntegrityUnknown &&
              IntegrityFromRid(0xFFFFFFFFu) == kIntegrityUnknown);

        // (b) The enum ORDER is trust order, because CompareRows sorts by the
        //     enum value: ascending has to mean least-trusted first for the
        //     sort to be the useful direction, and this pins that decision so
        //     reordering the enum cannot silently reverse it.
        Check(r, "f5.2.enum-is-in-increasing-trust-order",
              kIntegrityUntrusted < kIntegrityLow && kIntegrityLow < kIntegrityMedium &&
              kIntegrityMedium < kIntegrityHigh && kIntegrityHigh < kIntegritySystem &&
              kIntegritySystem < kIntegrityProtected);

        // (c) LABELS. "unsigned" must stay lowercase because the filter matches
        //     a lower-case needle against it, and "BAD SIG" must stay shouty
        //     because it is the one state a reader must not miss. Both were
        //     asserted rather than assumed: the first version matched the RAW
        //     label, so `integrity:high` found nothing among eleven High rows.
        Check(r, "f5.2.labels-are-distinct-per-level",
              std::wcscmp(IntegrityLabel(kIntegrityLow), L"Low") == 0 &&
              std::wcscmp(IntegrityLabel(kIntegrityMedium), L"Medium") == 0 &&
              std::wcscmp(IntegrityLabel(kIntegrityHigh), L"High") == 0 &&
              std::wcscmp(IntegrityLabel(kIntegritySystem), L"System") == 0 &&
              std::wcscmp(IntegrityLabel(kIntegrityProtected), L"Protected") == 0 &&
              std::wcscmp(IntegrityLabel(kIntegrityUntrusted), L"Untrusted") == 0);

        Check(r, "f5.3.labels-separate-unsigned-from-invalid",
              std::wcscmp(SignatureLabel(kSigUnsigned), L"unsigned") == 0 &&
              std::wcscmp(SignatureLabel(kSigInvalid), L"BAD SIG") == 0 &&
              // The pair that must never collapse: an unsigned binary is the
              // normal case for most of what runs, and merging it into
              // "invalid" would paint most of a machine red and train the
              // reader to ignore the colour.
              std::wcscmp(SignatureLabel(kSigUnsigned),
                          SignatureLabel(kSigInvalid)) != 0);

        Check(r, "f5.3.unchecked-label-is-the-dash",
              std::wcscmp(SignatureLabel(kSigUnchecked), L"—") == 0);

        // (d) THE COLUMN CELLS. GetColumnText is what both surfaces print.
        {
            Connection c = MakeReferenceTcpRow();

            // F5.1. ppid unknown is NOT "no parent" - it is an unanswerable
            // question, and it prints the dash. ppid known with no name (the
            // parent was not in the snapshot) still prints the NUMBER, because
            // collapsing that to a dash would claim the process has no parent,
            // which is the one answer the snapshot cannot give.
            wchar_t buf[64] = {0};
            ConnectionStore::GetColumnText(c, COL_PPID, buf, 64);
            Check(r, "f5.1.unknown-ppid-dashes",
                  std::wcsstr(buf, L"—") != nullptr);

            c.ppidKnown = true;
            c.ppid = 4242;
            buf[0] = L'\0';
            ConnectionStore::GetColumnText(c, COL_PPID, buf, 64);
            Check(r, "f5.1.known-ppid-without-name-prints-the-number",
                  std::wcsstr(buf, L"4242") != nullptr);

            c.parentName = L"services.exe";
            buf[0] = L'\0';
            ConnectionStore::GetColumnText(c, COL_PPID, buf, 64);
            Check(r, "f5.1.known-ppid-prints-number-and-name",
                  std::wcsstr(buf, L"4242") != nullptr &&
                  std::wcsstr(buf, L"services.exe") != nullptr);

            // F5.2. Unmeasured integrity dashes, and an AppContainer marker is
            // ADDED to the level rather than replacing it.
            buf[0] = L'\0';
            ConnectionStore::GetColumnText(c, COL_INTEGRITY, buf, 64);
            Check(r, "f5.2.unmeasured-integrity-dashes",
                  std::wcsstr(buf, L"—") != nullptr);

            c.integrity = static_cast<unsigned>(kIntegrityHigh);
            c.appContainer = false;
            buf[0] = L'\0';
            ConnectionStore::GetColumnText(c, COL_INTEGRITY, buf, 64);
            Check(r, "f5.2.cell-is-the-level-name",
                  std::wcsstr(buf, L"High") != nullptr);

            c.appContainer = true;
            buf[0] = L'\0';
            ConnectionStore::GetColumnText(c, COL_INTEGRITY, buf, 64);
            Check(r, "f5.2.appcontainer-keeps-the-level-and-adds-a-marker",
                  std::wcsstr(buf, L"High") != nullptr &&
                  std::wcsstr(buf, L"+AC") != nullptr);

            // F5.3. Unchecked is a dash, NOT "unsigned": WinTCP not looking is
            // not the same as WinTCP looking and finding nothing.
            buf[0] = L'\0';
            ConnectionStore::GetColumnText(c, COL_SIGNATURE, buf, 64);
            Check(r, "f5.3.unchecked-dashes-rather-than-claiming-unsigned",
                  std::wcsstr(buf, L"—") != nullptr);

            c.signature = static_cast<unsigned>(kSigUnsigned);
            buf[0] = L'\0';
            ConnectionStore::GetColumnText(c, COL_SIGNATURE, buf, 64);
            Check(r, "f5.3.cell-is-the-verdict-label",
                  std::wcsstr(buf, L"unsigned") != nullptr);
        }

        // (e) THE FILTERS. The rule being pinned is the one this codebase cares
        //     most about: a row whose reading was never taken matches NOTHING,
        //     so an unmeasurable row can never masquerade as a match. `signed:`
        //     bare means "WinTCP verified this and it was good", which is NOT
        //     "the verdict exists".
        {
            std::vector<FilterClause> prog;
            auto matches = [&prog](const wchar_t* box, const Connection& row) {
                prog.clear();
                ParseFilter(box, prog);
                return !prog.empty() && MatchFilter(row, prog);
            };
            // FinalizeRow after every mutation, always. It is what fills the
            // lower* search keys, and the whole point of those fields is that a
            // filter never pays ToLowerW per row per clause. A row edited without
            // re-finalizing it is a row whose search keys describe a PREVIOUS
            // version of it - which is exactly how `parent:services` matched
            // nothing in the first draft of this block while looking correct.
            Connection c = MakeReferenceTcpRow();
            c.ppid = 4242;
            c.ppidKnown = true;
            c.parentName = L"services.exe";
            c.integrity = static_cast<unsigned>(kIntegrityHigh);
            ConnectionStore::FinalizeRow(c);

            Check(r, "f5.1.filter-ppid-matches-exactly",
                  matches(L"ppid:4242", c) && !matches(L"ppid:4243", c));
            Check(r, "f5.1.filter-parent-searches-the-name",
                  matches(L"parent:services", c) && !matches(L"parent:explorer", c));
            // A row with NO ppid answer must not match any threshold, including
            // the empty one that means "has a ppid".
            Connection unknown = MakeReferenceTcpRow();
            unknown.ppidKnown = false;
            Check(r, "f5.1.unmeasured-ppid-matches-nothing",
                  !matches(L"ppid:4242", unknown) && !matches(L"ppid:", unknown));

            Check(r, "f5.2.filter-integrity-matches-the-level",
                  matches(L"integrity:high", c) && matches(L"integrity:High", c) &&
                  !matches(L"integrity:system", c));
            // Unreadable integrity answers "no match", not "match everything".
            unknown.integrity = 0;
            Check(r, "f5.2.unmeasured-integrity-matches-nothing",
                  !matches(L"integrity:high", unknown) &&
                  !matches(L"integrity:", unknown));
            // A value that is not a level is a value that matches nothing -
            // never a degenerate substring test that selects the whole table.
            Check(r, "f5.2.bogus-integrity-matches-nothing",
                  !matches(L"integrity:bogus", c));
            Check(r, "f5.2.appcontainer-finds-its-marker",
                  !matches(L"integrity:ac", c));
            c.appContainer = true;
            ConnectionStore::FinalizeRow(c);
            Check(r, "f5.2.appcontainer-marker-is-searchable",
                  matches(L"integrity:ac", c));
            c.appContainer = false;

            // Signature is a raw enum, so it needs no re-finalize to be seen by
            // the filter - which is the reason the two features are not
            // symmetric, and worth stating rather than leaving to be discovered.
            c.signature = static_cast<unsigned>(kSigValid);
            Check(r, "f5.3.bare-signed-means-verified-and-good",
                  matches(L"signed:", c));
            c.signature = static_cast<unsigned>(kSigUnsigned);
            Check(r, "f5.3.bare-signed-excludes-unsigned",
                  !matches(L"signed:", c));
            Check(r, "f5.3.bogus-signature-matches-nothing",
                  !matches(L"signature:bogus", c));
            c.signature = static_cast<unsigned>(kSigInvalid);
            Check(r, "f5.3.bare-signed-excludes-a-bad-signature",
                  !matches(L"signed:", c));
            // Unchecked answers nothing at all, even to the bare form: reporting
            // an unasked question as if it had been asked is the failure mode.
            c.signature = 0;
            Check(r, "f5.3.unchecked-row-matches-nothing",
                  !matches(L"signed:", c) && !matches(L"signature:", c));
        }

        // (f) THE COLUMN MASKS. The bit for every ColumnId must exist and the
        //     three new columns must be OFF by default - asserted here because
        //     COL_COUNT reached exactly 32, where the naive
        //     `(1u << COL_COUNT) - 1` is a shift by the width of the type.
        Check(r, "f5.columns-exist-in-the-persisted-mask",
              COL_PPID < COL_COUNT && COL_INTEGRITY < COL_COUNT &&
              COL_SIGNATURE < COL_COUNT);
        Check(r, "f5.columns-are-hidden-by-default",
              (kDefaultVisibleCols & (1u << COL_PPID)) == 0 &&
              (kDefaultVisibleCols & (1u << COL_INTEGRITY)) == 0 &&
              (kDefaultVisibleCols & (1u << COL_SIGNATURE)) == 0);
        // The mask must still cover every column AND a normal majority of them:
        // a mask of 1 (the value `1u << 32` produces) is non-empty, so "not
        // empty" alone would not have caught the shift.
        Check(r, "f5.all-col-mask-covers-every-column",
              (kAllColMask & (1u << (COL_COUNT - 1))) != 0 &&
              (kAllColMask & (1u << COL_PPID)) != 0 &&
              (kAllColMask & (1u << COL_SIGNATURE)) != 0);
        {
            // A hand-rolled popcount rather than __builtin_popcount (no MSVC
            // equivalent without <intrin.h>) or _CountOneBits (an intrinsic for
            // one compiler). Four lines beat either dependency.
            UINT32 bits = kDefaultVisibleCols;
            int pop = 0;
            while (bits != 0) { bits &= bits - 1u; ++pop; }
            Check(r, "f5.default-set-is-more-than-one-column", pop > 4);
        }
    }

    // 4. Snapshot diff: appear / state-change / disappear + one-cycle ghosts.
    {
        ConnectionStore store;
        std::vector<Connection> a;
        a.push_back(MakeReferenceTcpRow());

        Connection b = MakeReferenceTcpRow();
        b.pid = 7;
        b.processName = L"other.exe";
        b.localPort = 50000;
        b.remotePort = 22;
        b.state = MIB_TCP_STATE_LISTEN;
        a.push_back(b);
        store.ReplaceSnapshot(a);

        std::vector<RowChange> ev = store.TakeChangeEvents();
        bool ok = ev.size() == 2;
        for (size_t i = 0; ok && i < ev.size(); ++i)
            ok = ev[i].kind == kChangeAppear;
        Check(r, "store.diff.first-snapshot-appears", ok,
              "events=" + std::to_string(ev.size()));

        // State change on row 1, new row, row 2 vanishes.
        Connection a1 = MakeReferenceTcpRow();
        a1.state = MIB_TCP_STATE_CLOSE_WAIT;
        std::vector<Connection> b2;
        b2.push_back(a1);
        Connection c3 = b;
        c3.pid = 9;
        c3.localPort = 50001;
        b2.push_back(c3);
        store.ReplaceSnapshot(b2);

        ev = store.TakeChangeEvents();
        int appears = 0, disappears = 0, states = 0;
        for (const RowChange& ch : ev) {
            if (ch.kind == kChangeAppear) ++appears;
            else if (ch.kind == kChangeDisappear) ++disappears;
            else if (ch.kind == kChangeState) {
                ++states;
                ok = ch.oldState == MIB_TCP_STATE_ESTAB;
            }
        }
        ok = appears == 1 && disappears == 1 && states == 1;
        Check(r, "store.diff.change-events", ok,
              "a=" + std::to_string(appears) +
                  " d=" + std::to_string(disappears) +
                  " s=" + std::to_string(states));
        // 2 fresh rows + 1 ghost row.
        Check(r, "store.diff.ghost-row", store.Rows().size() == 3,
              "rows=" + std::to_string(store.Rows().size()));

        // Same snapshot again: ghost drops out, no events.
        store.ReplaceSnapshot(b2);
        ev = store.TakeChangeEvents();
        Check(r, "store.diff.stable-second-cycle",
              ev.empty() && store.Rows().size() == 2,
              "events=" + std::to_string(ev.size()) +
                  " rows=" + std::to_string(store.Rows().size()));
    }

    // 5. UDP column text (netstat-style placeholders).
    {
        Connection u;
        u.family = AF_INET;
        u.protocol = IPPROTO_UDP;
        u.localAddress = L"0.0.0.0";
        u.localPort = 5353;
        u.state = 0;
        ConnectionStore::FinalizeRow(u);
        wchar_t buf[128] = {0};

        ConnectionStore::GetColumnText(u, COL_PROTO, buf, 128);
        const bool protoOk = std::wstring(buf) == L"UDPv4";
        ConnectionStore::GetColumnText(u, COL_RPORT, buf, 128);
        const bool starOk = std::wstring(buf) == L"*";
        ConnectionStore::GetColumnText(u, COL_STATE, buf, 128);
        const bool dashOk = std::wstring(buf) == L"\x2014";
        ConnectionStore::GetColumnText(u, COL_PROCESS, buf, 128);
        const bool emptyOk = std::wstring(buf) == L"\x2014";
        Check(r, "columns.udp-placeholders",
              protoOk && starOk && dashOk && emptyOk);
    }

    // 6. Shared column titles (GUI header = CSV = CLI).
    {
        bool ok = true;
        std::vector<std::wstring> seen;
        for (int c = 0; c < COL_COUNT; ++c) {
            const std::wstring t = ConnectionStore::ColumnTitle(c);
            if (t.empty()) ok = false;
            for (const std::wstring& s : seen)
                if (s == t) ok = false;
            seen.push_back(t);
        }
        Check(r, "columns.titles-unique-and-nonempty", ok);
    }

    // 7. Sort: pid ascending and deterministic across repeated SetView.
    {
        ConnectionStore store;
        std::vector<Connection> rows;
        for (unsigned i = 0; i < 40; ++i) {
            Connection c = MakeReferenceTcpRow();
            c.pid = 30 - (i % 10);       // lots of equal pids -> tie-breaks
            c.localPort = 40000 + i;
            c.remotePort = 1 + i;
            rows.push_back(c);
        }
        store.ReplaceSnapshot(rows);
        store.SetSort(COL_PID, true);
        ViewQuery q;
        store.SetView(q);

        bool monotonic = true;
        std::vector<std::uint64_t> first;
        for (size_t v = 0; v < store.View().size(); ++v) {
            const Connection* c = store.ViewRow(v);
            if (c == nullptr) continue;
            first.push_back(c->id);
            if (v > 0) {
                const Connection* p = store.ViewRow(v - 1);
                if (p != nullptr && p->pid > c->pid) monotonic = false;
            }
        }
        store.SetView(q);
        std::vector<std::uint64_t> second;
        for (size_t v = 0; v < store.View().size(); ++v) {
            const Connection* c = store.ViewRow(v);
            if (c != nullptr) second.push_back(c->id);
        }
        Check(r, "sort.pid-ascending", monotonic && first.size() == 40);
        Check(r, "sort.deterministic", first == second);
    }

    // 8. Traffic counters + connection counting.
    {
        ConnectionStore store;
        std::vector<Connection> rows;
        rows.push_back(MakeReferenceTcpRow());
        Connection two = MakeReferenceTcpRow();
        two.localPort = 60000;
        two.remotePort = 80;
        rows.push_back(two);
        store.ReplaceSnapshot(rows);

        store.SetTraffic(42, 1234, 567);
        bool ok = true;
        for (size_t i = 0; i < store.Rows().size() && ok; ++i) {
            if (store.Rows()[i].pid == 42) {
                ok = store.Rows()[i].trafficRx == 1234 &&
                     store.Rows()[i].trafficTx == 567;
            }
        }
        Check(r, "traffic.set-by-pid", ok);
        Check(r, "traffic.count-for-pid", store.CountForPid(42) == 2);

        store.ClearTraffic();
        ok = true;
        for (size_t i = 0; i < store.Rows().size() && ok; ++i)
            ok = store.Rows()[i].trafficRx == 0 &&
                 store.Rows()[i].trafficTx == 0;
        Check(r, "traffic.clear", ok);
    }

    // 9. UTF-8 / CSV helpers used by export, CSV and change log.
    {
        Check(r, "text.utf8-encode",
              WideToUtf8(L"\x2014") == "\xE2\x80\x94" &&
                  WideToUtf8(L"").empty());
        Check(r, "text.csv-escape",
              CsvEscapeUtf8("abc") == "abc" &&
                  CsvEscapeUtf8("a,b") == "\"a,b\"");
        Check(r, "text.format-port", FormatPort(443) == L"443");

        // The rest of block 9 exists because these two functions are the only
        // thing between an arbitrary string and a CSV cell, a JSON export or a
        // log line - and both take NUL-terminated input, so a partial answer
        // looks exactly like a complete one from the outside.

        // The oracles have to be pinned too, or "the converter's output is
        // well formed" says nothing: an oracle that accepted every byte string
        // would pass a converter that copied its input straight through. Each
        // term below is a case the predicate must take a side on - overlong,
        // surrogate, past U+10FFFF, truncated, never-a-byte, and their
        // well-formed counterparts at each of the four sequence lengths.
        Check(r, "text.utf8-oracle",
              !ValidUtf8("\x80") && !ValidUtf8("\xC0\xAF") &&
                  !ValidUtf8("\xE0\x80\xAF") && !ValidUtf8("\xED\xA0\x80") &&
                  !ValidUtf8("\xF4\x90\x80\x80") && !ValidUtf8("\xF5\x80\x80\x80") &&
                  !ValidUtf8("\xE2\x82") && !ValidUtf8("\xC2") &&
                  ValidUtf8("") && ValidUtf8("abc") && ValidUtf8("\x7F") &&
                  ValidUtf8("\xC2\x80") && ValidUtf8("\xE0\xA0\x80") &&
                  ValidUtf8("\xF0\x90\x80\x80") && ValidUtf8("\xF4\x8F\xBF\xBF") &&
                  !ValidUtf16(L"\xD800") && !ValidUtf16(L"\xDC00") &&
                  !ValidUtf16(L"ok\xDBFF") && !ValidUtf16(L"\xDFFF" L"ok") &&
                  ValidUtf16(L"") && ValidUtf16(L"abc") &&
                  ValidUtf16(L"\xD83D\xDE00"),
              "oracle");

        // Well-formed text must survive an encode followed by a decode with
        // nothing added, dropped, reordered or substituted. Every entry is
        // here for a reason: the em dash and CJK are the multi-byte shapes, the
        // emoji is a surrogate pair (four bytes in UTF-8, two units in wide),
        // the combining acute is a sequence rather than a precomposed letter,
        // U+FFFF and U+E000 are a noncharacter and a private-use code point
        // that a stricter converter might refuse, and the long strings go past
        // any single-allocation path.
        bool wideRt = true;
        for (const wchar_t* w : {
                 L"",
                 L"abc",
                 L"\x2014",             // em dash, 3-byte UTF-8
                 L"caf\xE9",            // Latin-1 with an accent
                 L"\x4E2D\x6587",       // CJK
                 L"\xD83D\xDE00",       // U+1F600, a surrogate pair
                 L"a\x0301" L"e",       // 'a' + combining acute + 'e'
                 L"\x05D0\x05D1",       // Hebrew, right-to-left
                 L"\x2028\x2029",       // line and paragraph separators
                 L"\xE000",             // private use
                 L"\xFFFF",             // noncharacter
                 L"\xFFFD",             // the replacement character itself
                 L"\x0001\x001F",       // C0 controls
                 L"\x007F",             // DEL
                 L"0123456789",
             }) {
            const std::wstring in(w);
            if (Utf8ToWide(WideToUtf8(in).c_str()) != in) { wideRt = false; break; }
        }
        std::wstring longw(20000, L'x');  // well past any small-string buffer
        for (size_t i = 0; i < longw.size(); i += 7) {
            longw[i] = static_cast<wchar_t>(0x4E00 + (i % 90));
        }
        wideRt = wideRt && Utf8ToWide(WideToUtf8(longw).c_str()) == longw;
        Check(r, "text.utf8-wide-roundtrip", wideRt);

        // The same trip the other way: bytes that are already well-formed
        // UTF-8 must come back byte for byte. Each entry is a boundary - the
        // last one-byte code point, the first and last two- and three-byte
        // code points, U+FFFF's neighbourhood, a BOM when it is text rather
        // than a file header, and U+10FFFF, the largest code point Unicode
        // defines, whose encoding ends in two bytes that look like a lone
        // continuation to anything that stopped counting early.
        bool byteRt = true;
        for (const char* p : {
                 "",
                 "abc",
                 "caf\xC3\xA9",
                 "\xE4\xB8\xAD\xE6\x96\x87",
                 "\xF0\x9F\x98\x80",      // U+1F600
                 "\x7F\xC2\x80",          // DEL, then the first 2-byte code point
                 "\xDF\xBF\xE0\xA0\x80",  // last 2-byte, first 3-byte
                 "\xEF\xBB\xBF",          // BOM as content
                 "\xED\x9F\xBF",          // U+D7FF, just below the surrogates
                 "\xEE\x80\x80",          // U+E000, first private use
                 "\xF4\x8F\xBF\xBF",      // U+10FFFF
             }) {
            const std::string in(p);
            if (WideToUtf8(Utf8ToWide(in.c_str())) != in) { byteRt = false; break; }
        }
        Check(r, "text.utf8-byte-roundtrip", byteRt);

        // Garbage in, and there is no correct answer to compare against - so
        // what is pinned is what has to hold REGARDLESS of the bytes. Every
        // entry is malformed in a different way: a stray continuation, an
        // overlong slash (which decodes to '/' if the overlong form is
        // accepted, a different character than was encoded), a CESU-8
        // surrogate, a code point past U+10FFFF, a truncated sequence, a bad
        // continuation between two ASCII bytes, and an embedded NUL, which
        // both functions stop at by construction.
        const std::string garbage[] = {
            std::string("\x80"),                    // lone continuation
            std::string("\xBF"),                    // lone continuation, high
            std::string("\xC0\xAF"),                // overlong '/'
            std::string("\xC1\xBF"),                // overlong U+007F
            std::string("\xE0\x80\xAF"),            // overlong, 3-byte form
            std::string("\xF0\x80\x80\xAF"),        // overlong, 4-byte form
            std::string("\xED\xA0\x80"),            // CESU-8 high surrogate
            std::string("\xED\xB0\x80"),            // lone low surrogate
            std::string("\xF4\x90\x80\x80"),        // past U+10FFFF
            std::string("\xF5\x80\x80\x80"),        // never a lead byte
            std::string("\xFF\xFE"),                // never any byte
            std::string("\xE2\x82"),                // truncated sequence
            std::string("\xF0\x9F\x98"),            // truncated 4-byte
            std::string("\xE2\x28\xA1"),            // bad continuation mid-sequence
            std::string("a\x80" "b"),               // stray byte between ASCII
            std::string("\xC2"),                    // lead byte with nothing after
            std::string("\xE2\x80\xAE" "abc"),      // RTL override, then text
            std::string("abc\0def", 7),             // embedded NUL, length kept
            std::string(1, '\0'),                   // nothing but a NUL
            std::string(),                          // nothing at all
        };
        bool gv = true, gs = true, gn = true;
        for (const std::string& in : garbage) {
            const std::wstring w = Utf8ToWide(in.c_str());
            const std::string z = WideToUtf8(w);
            // A decode must not hand a lone surrogate to the next encode, and
            // an encode must not hand malformed UTF-8 to anything downstream.
            if (!ValidUtf16(w)) gv = false;
            if (!ValidUtf8(z)) gv = false;
            // Neither output may contain a NUL: a row, a CSV cell or a log line
            // that carries one is cut in half at the first reader.
            if (w.find(L'\0') != std::wstring::npos) gn = false;
            if (z.find('\0') != std::string::npos) gn = false;
            // And the second trip must not move. A cell that reads one way on
            // the first export and another on the second is worse than one that
            // is wrong the same way every time.
            if (WideToUtf8(Utf8ToWide(z.c_str())) != z) gs = false;
        }
        Check(r, "text.utf8-garbage-wellformed", gv);
        Check(r, "text.utf8-garbage-no-nul", gn);
        Check(r, "text.utf8-garbage-stable", gs);

        // The same three properties for wide input that cannot be encoded: a
        // surrogate with no partner is not a character, so a converter has to
        // decide what to do with it, and "return nothing" would take the whole
        // string with it rather than just the offending unit.
        bool wv = true, wn = true, ws = true;
        for (const wchar_t* p : {
                 L"\xD800",        // lone high surrogate
                 L"\xDC00",        // lone low surrogate
                 L"ok\xDBFF",      // good prefix, then an unpaired high
                 L"\xDFFF" L"ok",  // unpaired low, then a good suffix
                 L"\xD83D",        // a high surrogate with its pair missing
                 L"\x0001",
             }) {
            const std::wstring in(p);
            const std::string z = WideToUtf8(in);
            if (!ValidUtf8(z)) wv = false;
            if (z.find('\0') != std::string::npos) wn = false;
            if (WideToUtf8(Utf8ToWide(z.c_str())) != z) ws = false;
        }
        Check(r, "text.utf8-lone-surrogate-wellformed", wv);
        Check(r, "text.utf8-lone-surrogate-no-nul", wn);
        Check(r, "text.utf8-lone-surrogate-stable", ws);
        // A code point that cannot be encoded must not take the rest of the
        // string with it: 'ok' is valid on both sides of the surrogate, and a
        // whole cell becoming empty because of one bad unit is precisely how a
        // good row turns into a blank one. Either dropping the unit or
        // replacing it with U+FFFD is an acceptable answer; losing 'ok' is not.
        const std::string kept = WideToUtf8(L"ok\xDBFF");
        Check(r, "text.utf8-lone-surrogate-keeps-prefix",
              kept == "ok" || kept == "ok\xEF\xBF\xBD",
              "len=" + std::to_string(kept.size()));
        Check(r, "text.utf8-null-arg", Utf8ToWide(nullptr).empty());

        // Deterministic fuzz: the same LCG every run, so a failure is
        // reproducible from the seed printed here rather than "it happened
        // once". Only code points that are legal on their own are generated,
        // which keeps every round-trip failure a converter bug instead of a
        // property of unpaired surrogate halves; the wide string is what gets
        // encoded, and its decode must come back identical.
        uint32_t seed = 0x9E3779B9u;
        bool fuzz = true;
        for (int it = 0; it < 4096 && fuzz; ++it) {
            seed = seed * 1664525u + 1013904223u;
            const size_t n = seed % 40;
            std::wstring w;
            for (size_t k = 0; k < n; ++k) {
                seed = seed * 1664525u + 1013904223u;
                const uint32_t cp = seed % 0x110000;
                if (cp < 0x20) continue;                          // controls, incl. NUL
                if (cp >= 0xD800 && cp <= 0xDFFF) continue;       // unpaired halves
                if (cp < 0x10000) {
                    w.push_back(static_cast<wchar_t>(cp));
                } else {
                    const uint32_t v = cp - 0x10000;
                    w.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
                    w.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
                }
            }
            const std::string z = WideToUtf8(w);
            if (!ValidUtf8(z)) fuzz = false;
            if (z.find('\0') != std::string::npos) fuzz = false;
            if (Utf8ToWide(z.c_str()) != w) fuzz = false;
        }
        Check(r, "text.utf8-fuzz-roundtrip", fuzz, "seed=" + std::to_string(seed));
    }

    // 10. Per-process stat columns.
    {
        Connection c = MakeReferenceTcpRow();
        c.cpuPct = 12.34;
        c.memKnown = true;
        c.memWorkingSet = 123456789;   // -> "117.7 MB"
        c.memPrivate = 4096;
        c.ioKnown = true;
        c.diskReadBytes = 1048576;
        c.diskWriteBytes = 524288;     // total -> "1.5 MB"
        c.trafficRx = 2048;            // -> "2.0 KB"
        c.trafficTx = 512;             // -> "512 B" (net total "2.5 KB")
        wchar_t buf[128] = {0};

        ConnectionStore::GetColumnText(c, COL_CPU, buf, 128);
        const bool cpuOk = std::wstring(buf) == L"12.3 %";
        ConnectionStore::GetColumnText(c, COL_MEM, buf, 128);
        const bool memOk = std::wstring(buf) == L"117.7 MB";
        ConnectionStore::GetColumnText(c, COL_DISK, buf, 128);
        const bool diskOk = std::wstring(buf) == L"1.5 MB";
        ConnectionStore::GetColumnText(c, COL_RX, buf, 128);
        const bool rxOk = std::wstring(buf) == L"2.0 KB";
        ConnectionStore::GetColumnText(c, COL_TX, buf, 128);
        const bool txOk = std::wstring(buf) == L"512 B";
        ConnectionStore::GetColumnText(c, COL_NETTOTAL, buf, 128);
        const bool netOk = std::wstring(buf) == L"2.5 KB";
        Check(r, "columns.stat-text",
              cpuOk && memOk && diskOk && rxOk && txOk && netOk);

        // Unknown readings render as em dashes.
        const Connection u = MakeReferenceTcpRow();
        ConnectionStore::GetColumnText(u, COL_CPU, buf, 128);
        const bool cpuDash = std::wstring(buf) == L"\x2014";
        ConnectionStore::GetColumnText(u, COL_MEM, buf, 128);
        const bool memDash = std::wstring(buf) == L"\x2014";
        ConnectionStore::GetColumnText(u, COL_DISK, buf, 128);
        const bool diskDash = std::wstring(buf) == L"\x2014";
        ConnectionStore::GetColumnText(u, COL_RX, buf, 128);
        const bool rxDash = std::wstring(buf) == L"\x2014";
        Check(r, "columns.stat-unknown-dashes",
              cpuDash && memDash && diskDash && rxDash);

        // SetProcStats joins readings into every row with the PID.
        ConnectionStore store;
        std::vector<Connection> rows;
        rows.push_back(MakeReferenceTcpRow());
        Connection two = MakeReferenceTcpRow();
        two.localPort = 60001;
        two.remotePort = 81;
        rows.push_back(two);
        store.ReplaceSnapshot(rows);
        store.SetProcStats(42, 5.5, true, 1048576, 2048, true, 100, 200);
        bool ok = true;
        for (size_t i = 0; i < store.Rows().size() && ok; ++i) {
            const Connection& rc = store.Rows()[i];
            ok = rc.cpuPct == 5.5 && rc.memKnown &&
                 rc.memWorkingSet == 1048576 && rc.memPrivate == 2048 &&
                 rc.ioKnown && rc.diskReadBytes == 100 &&
                 rc.diskWriteBytes == 200;
        }
        Check(r, "store.set-proc-stats", ok);

        // CPU sort + stat filter fields.
        store.SetSort(COL_CPU, true);
        ViewQuery q;
        store.SetView(q);
        Check(r, "sort.cpu-ascending",
              store.View().size() == 2);   // equal keys, stable/deterministic

        // Unknown readings ("—") sort LAST in both directions (7.4).
        {
            ConnectionStore s2;
            std::vector<Connection> r2;
            Connection known = MakeReferenceTcpRow();
            known.cpuPct = 5.0;
            known.localPort = 50000;
            known.remotePort = 443;
            r2.push_back(known);
            Connection unknown = MakeReferenceTcpRow();   // cpuPct == -1
            unknown.localPort = 50001;
            unknown.remotePort = 444;
            r2.push_back(unknown);
            s2.ReplaceSnapshot(r2);
            ViewQuery q2;
            s2.SetSort(COL_CPU, true);
            s2.SetView(q2);
            const bool ascOk =
                s2.View().size() == 2 && s2.ViewRow(0) != nullptr &&
                s2.ViewRow(0)->cpuPct >= 0.0 && s2.ViewRow(1) != nullptr &&
                s2.ViewRow(1)->cpuPct < 0.0;
            s2.SetSort(COL_CPU, false);
            s2.SetView(q2);
            const bool descOk =
                s2.View().size() == 2 && s2.ViewRow(1) != nullptr &&
                s2.ViewRow(1)->cpuPct < 0.0;
            Check(r, "sort.stat-unknown-last", ascOk && descOk);
        }
        std::vector<FilterClause> prog;
        ParseFilter(L"cpu:12 mem:117 disk:1.5 rx:2.0 tx:512 net:2.5", prog);
        ok = prog.size() == 6 && prog[0].field == FilterField::Cpu &&
             prog[1].field == FilterField::Mem &&
             prog[2].field == FilterField::Disk &&
             prog[3].field == FilterField::Rx &&
             prog[4].field == FilterField::Tx &&
             prog[5].field == FilterField::Net;
        Check(r, "filter.parse.stat-fields", ok);
        const auto matches = [&c](const wchar_t* expr) {
            std::vector<FilterClause> p2;
            ParseFilter(expr, p2);
            return MatchFilter(c, p2);
        };
        // These are THRESHOLDS now, and this row is the fixture that shows
        // why the old substring reading was never usable: it holds 2048 bytes
        // received, which printed as "2.0 KB" - so the old text match accepted
        // `rx:2.0` for a value of 2 KB, while `tx:512` accepted "512 B".
        // `rx:2.0` now means 2 MB and does NOT match; the way to ask for
        // 2 KB is `rx:2KB`. A bare number is MB, cpu is %.
        Check(r, "filter.match.stat-fields",
              matches(L"cpu:12") && !matches(L"cpu:99") &&
                  matches(L"mem:117") && !matches(L"mem:200") &&
                  matches(L"disk:1") && !matches(L"disk:2") &&
                  matches(L"rx:2KB") && !matches(L"rx:2.0") &&
                  matches(L"tx:512B") && !matches(L"tx:512") &&
                  matches(L"net:2.5KB") && !matches(L"net:9"));
    }

    // 11. ETW event classification. The elevated ground-truth
    //     probe (40443 events) proved classic MOF events carry their type
    //     in EventDescriptor.Opcode while Id is always 0 - the original
    //     consumer filtered on Id and counted nothing, which is
    //     why an elevated run still showed empty traffic cells.
    {
        const GUID otherGuid = {0x11111111, 0x2222, 0x3333,
                                {4, 5, 6, 7, 8, 9, 10, 11}};
        Check(r, "etw.classify.event-types",
              ClassifyNetworkEvent(kTcpIpProviderGuid, 0, 10) ==
                      TrafficDirection::Sent &&
                  ClassifyNetworkEvent(kTcpIpProviderGuid, 0, 11) ==
                      TrafficDirection::Received &&
                  ClassifyNetworkEvent(kTcpIpProviderGuid, 0, 26) ==
                      TrafficDirection::Sent &&
                  ClassifyNetworkEvent(kTcpIpProviderGuid, 0, 27) ==
                      TrafficDirection::Received &&
                  ClassifyNetworkEvent(kUdpIpProviderGuid, 0, 10) ==
                      TrafficDirection::Sent &&
                  // robustness: a build that fills Id instead still works
                  ClassifyNetworkEvent(kTcpIpProviderGuid, 10, 0) ==
                      TrafficDirection::Sent);
        // Never counted: foreign providers and the extra classic event
        // types (18 = per-segment receive variant, 12 = connect) - both
        // would inject phantom/double-counted bytes.
        Check(r, "etw.classify.rejects-noncounted",
              ClassifyNetworkEvent(otherGuid, 0, 10) ==
                      TrafficDirection::None &&
                  ClassifyNetworkEvent(kTcpIpProviderGuid, 0, 18) ==
                      TrafficDirection::None &&
                  ClassifyNetworkEvent(kTcpIpProviderGuid, 0, 12) ==
                      TrafficDirection::None &&
                  ClassifyNetworkEvent(kTcpIpProviderGuid, 0, 0) ==
                      TrafficDirection::None);
        // MOF payload: uint32 PID @0, uint32 size @4 (probe-confirmed).
        BYTE payload[8] = {};
        const DWORD expectPid = 42, expectSize = 1234;
        std::memcpy(payload, &expectPid, 4);
        std::memcpy(payload + 4, &expectSize, 4);
        DWORD pid = 0, size = 0;
        Check(r, "etw.payload-parse",
              ParseTrafficPayload(payload, 8, &pid, &size) && pid == expectPid &&
                  size == expectSize &&
                  !ParseTrafficPayload(payload, 4, &pid, &size) &&
                  !ParseTrafficPayload(nullptr, 8, &pid, &size));
    }

    // 12. The non-admin socket fallback's accumulator - closed
    //     sockets are retired (totals never drop), handle-value reuse
    //     splits old and new, exited PIDs are forgotten (PID-reuse
    //     safety), unscanned PIDs keep their last known state.
    {
        SocketTrafficSampler sampler;
        const std::vector<DWORD> pids = {42, 99};
        int stage = 1;
        bool ok = true;
        {
            std::vector<SocketSample> obs = {{42, 0x100, 1000, 2000},
                                             {42, 0x200, 10, 20}};
            sampler.MergeSample(obs, {42}, pids);
            auto t = sampler.Totals(pids);
            ok = t.size() == 1 && t.count(42) == 1 && t[42].rx == 1010 &&
                 t[42].tx == 2020;
        }
        if (ok) {   // 0x100 closes: retired into the PID total, sum grows
            stage = 2;
            const std::vector<SocketSample> obs = {{42, 0x200, 15, 25}};
            sampler.MergeSample(obs, {42}, pids);
            auto t = sampler.Totals(pids);
            ok = t.count(42) == 1 && t[42].rx == 1015 && t[42].tx == 2025;
        }
        if (ok) {   // handle reused with smaller counters: old retired first
            stage = 3;
            const std::vector<SocketSample> obs = {{42, 0x200, 5, 7}};
            sampler.MergeSample(obs, {42}, pids);
            auto t = sampler.Totals(pids);
            ok = t.count(42) == 1 && t[42].rx == 1020 && t[42].tx == 2032;
        }
        if (ok) {   // PID 42 leaves the connection rows: fully forgotten
            stage = 4;
            sampler.MergeSample({}, {42}, {99});
            const auto t = sampler.Totals({42, 99});
            ok = t.empty();
        }
        if (ok) {   // PID 99 reports normally...
            stage = 5;
            const std::vector<SocketSample> obs = {{99, 0x300, 77, 88}};
            sampler.MergeSample(obs, {99}, pids);
            auto t = sampler.Totals(pids);
            ok = t.count(99) == 1 && t[99].rx == 77 && t[99].tx == 88;
        }
        if (ok) {   // ...then becomes unscannable: totals must survive
            stage = 6;
            sampler.MergeSample({}, {}, pids);
            auto t = sampler.Totals(pids);
            ok = t.count(99) == 1 && t[99].rx == 77 && t[99].tx == 88;
        }
        Check(r, "socket.merge.lifecycle", ok,
              "stage=" + std::to_string(stage));
    }

    // 12b. D2. A stalled socket must not be able to damage the totals, and it
    //      must not be retried forever. Both are pure state logic, so they can
    //      be pinned here without needing a socket that actually stalls -
    //      which matters, because the machine that can produce one is exactly
    //      the machine where a test would be useless.
    {
        // (a) AN INCOMPLETE PASS MUST NOT LOWER ANYTHING.
        //     With the worker pool a pass can end before every socket is read.
        //     Sample answers that by merging the reads with an EMPTY `scanned`
        //     list, and this asserts why that matters: had it passed the PIDs
        //     it opened, every unread socket would look closed and the totals
        //     would DROP - a traffic column that decreases because the machine
        //     was busy is worse than a stale one.
        SocketTrafficSampler partial;
        const std::vector<DWORD> live = {7};
        std::vector<SocketSample> first = {{7, 0x10, 500, 600}};
        partial.MergeSample(first, {7}, live);
        const auto before = partial.Totals(live);
        // Same pass, but only ONE of the two sockets came back.
        std::vector<SocketSample> second = {{7, 0x10, 700, 800}};
        partial.MergeSample(second, {}, live);
        const auto after = partial.Totals(live);
        // at(), not []: 'before'/'after' are const maps and have no operator[].
        const long beforeRx =
            before.count(7) != 0 ? static_cast<long>(before.at(7).rx) : -1;
        const long afterRx =
            after.count(7) != 0 ? static_cast<long>(after.at(7).rx) : -1;
        const long afterTx =
            after.count(7) != 0 ? static_cast<long>(after.at(7).tx) : -1;
        Check(r, "d2.incomplete-pass-keeps-totals",
              beforeRx == 500 && afterRx == 700 && afterTx == 800,
              "beforeRx=" + std::to_string(beforeRx) +
                  " afterRx=" + std::to_string(afterRx) +
                  " afterTx=" + std::to_string(afterTx));

        // (b) A STALLED SOCKET IS REMEMBERED, AND ONLY ONCE.
        //     This is what bounds the leak: without it, one uncooperative
        //     socket costs a wedged worker thread on every refresh, for as long
        //     as the app is open.
        SocketTrafficSampler remember;
        Check(r, "d2.no-stalls-remembered-initially",
              remember.UnreadableCount() == 0,
              "count=" + std::to_string(remember.UnreadableCount()));
        std::vector<ProbeTarget> stalled = {{7, 0x20}, {7, 0x20}, {9, 0x30}};
        remember.RememberStalled(stalled);
        // Three entries, two of them the same socket: the set counts DISTINCT
        // sockets, which is the quantity that has to be bounded.
        Check(r, "d2.stalls-remembered-once",
              remember.UnreadableCount() == 2,
              "count=" + std::to_string(remember.UnreadableCount()));
        // Remembering nothing must not disturb what is already remembered, or
        // every healthy pass would clear the list and the bound would be void.
        remember.RememberStalled({});
        Check(r, "d2.empty-stall-list-is-a-no-op",
              remember.UnreadableCount() == 2);
        // And the pool must be big enough to survive the stalls seen in one
        // walk - this is a DIRECTION, not a performance number: with a pool
        // equal to the stall count, everything behind the stalls went unread
        // and tailscaled.exe reported no data at all.
        Check(r, "d2.pool-exceeds-observed-stalls",
              SocketTrafficSampler::kProbeWorkers >= 8,
              "workers=" + std::to_string(SocketTrafficSampler::kProbeWorkers));
        // The no-progress ceiling must be far below the hard budget, or the
        // "wait for progress" optimisation buys nothing: it is the difference
        // between a one-shot costing 8.3 s and one costing 1.1 s here.
        Check(r, "d2.no-progress-is-the-real-wait",
              SocketTrafficSampler::kNoProgressMs > 0 &&
                  SocketTrafficSampler::kNoProgressMs * 8 <=
                      SocketTrafficSampler::kScanTimeoutMs,
              "noProgress=" +
                  std::to_string(SocketTrafficSampler::kNoProgressMs) +
                  " budget=" +
                  std::to_string(SocketTrafficSampler::kScanTimeoutMs));
    }

    // 13. New columns (duration, speed, TLS, country, bookmarks). Each one
    //     carries an arithmetic or formatting decision that the list view
    //     cannot show, so they are pinned here with exact expected text.
    {
        // Duration formatting: each unit only when it is the leading unit,
        // always two-digit seconds under minutes.
        Check(r, "columns.duration-format",
              FormatDuration(0) == L"0s" &&
              FormatDuration(59) == L"59s" &&
              FormatDuration(60) == L"1m 00s" &&
              FormatDuration(3671) == L"1h 1m" &&
              FormatDuration(3600) == L"1h 0m" &&
              FormatDuration(86400) == L"1d 0h" &&
              FormatDuration(90000) == L"1d 1h");

        // Age arithmetic is monotonic-clocked, and an unknown first-seen
        // must report 0 rather than a wrapped-around huge value.
        Connection d;
        d.firstSeenTick = 0;
        const bool unknownAge = DurationSeconds(d, 1000000) == 0;
        d.firstSeenTick = 1000000;
        const bool zeroAge = DurationSeconds(d, 1000000) == 0;
        d.firstSeenTick = 1000000;
        // A snapshot taken "before" the row appeared must not wrap.
        const bool noWrap = DurationSeconds(d, 500) == 0;
        d.firstSeenTick = 1000;
        const bool oneSec = DurationSeconds(d, 4000) == 3;
        Check(r, "store.duration-arithmetic",
              unknownAge && zeroAge && noWrap && oneSec);

        // Bandwidth: a normal delta, the two refusal cases that would
        // otherwise render a nonsense or astronomically large rate.
        double bps = 0.0;
        const bool normal = ComputeBps(1000, 3000, 2000, &bps) && bps == 1000.0;
        const bool noTime = !ComputeBps(1000, 3000, 0, &bps);
        const bool rolled = !ComputeBps(5000, 1000, 1000, &bps);
        // No change between samples is a real 0 B/s, not "unknown".
        const bool idle = ComputeBps(1000, 1000, 1000, &bps) && bps == 0.0;
        Check(r, "store.bandwidth-bps", normal && noTime && rolled && idle);

        // The Speed cell must say "—" when the source was per-PID, and only
        // show a rate when per-row bytes were actually observed.
        Connection s;
        s.bpsKnown = false;
        wchar_t buf[128] = {0};
        ConnectionStore::GetColumnText(s, COL_BANDWIDTH, buf, 128);
        const bool unknownDash = (std::wstring(buf) == L"—");
        s.bpsKnown = true;
        s.rxBps = 0.0;
        s.txBps = 0.0;
        ConnectionStore::GetColumnText(s, COL_BANDWIDTH, buf, 128);
        const bool idleText = (std::wstring(buf) == L"idle");
        s.rxBps = 2048.0;
        s.txBps = 1024.0;
        ConnectionStore::GetColumnText(s, COL_BANDWIDTH, buf, 128);
        const bool rateText = (std::wstring(buf) == L"↓ 2.0 KB/s  ↑ 1.0 KB/s");
        Check(r, "columns.bandwidth-text",
              unknownDash && idleText && rateText, WideToUtf8(buf));

        // TLS naming: protocol from the wire bytes, cipher only for ids we
        // actually know, and an em-dash for a row that is not TLS.
        Check(r, "columns.tls-protocol-names",
              TlsProtocolName(0x0303) == L"TLS 1.2" &&
              TlsProtocolName(0x0304) == L"TLS 1.3" &&
              TlsProtocolName(0x0000) == L"TLS");
        Check(r, "columns.tls-cipher-names",
              TlsCipherName(0x1302) == L"TLS_AES_256_GCM_SHA384" &&
              TlsCipherName(0xC02F) ==
                  L"TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256" &&
              // Unknown id reports numerically rather than guessing.
              TlsCipherName(0xFFFF) == L"0xFFFF");

        TlsInfo t;
        t.known = true;
        t.secure = false;
        Check(r, "columns.tls-plaintext-dash", TlsSummary(t) == L"—");
        t.secure = true;
        t.protocol = 0x0303;
        t.cipherSuite = 0x1301;
        const bool cipherOnly = (TlsSummary(t) == L"TLS 1.2  TLS_AES_128_GCM_SHA256");
        t.haveSni = true;
        t.sni = L"example.com";
        // SNI is the more identifying fact, so it replaces the cipher.
        const bool sniWins = (TlsSummary(t) == L"TLS 1.2  example.com");
        Check(r, "columns.tls-summary", cipherOnly && sniWins,
              WideToUtf8(TlsSummary(t)));

        // Bookmarks cell: tag wins when set, plain "pinned" otherwise.
        Connection bm;
        ConnectionStore::GetColumnText(bm, COL_PINNED, buf, 128);
        const bool noneDash = (std::wstring(buf) == L"—");
        bm.pinned = true;
        ConnectionStore::GetColumnText(bm, COL_PINNED, buf, 128);
        const bool pinnedText = (std::wstring(buf) == L"pinned");
        bm.pinned = false;
        bm.tag = kTagAmber;
        ConnectionStore::GetColumnText(bm, COL_PINNED, buf, 128);
        const bool tagText = (std::wstring(buf) == L"amber");
        Check(r, "columns.bookmark-text", noneDash && pinnedText && tagText);
    }

    // 14. The new filter fields must parse and match, and must NOT match
    //     rows that lack the value (an "unknown" TLS or country must not be
    //     swept in by a substring test).
    {
        const auto matches = [](const std::wstring& expr,
                               const Connection& c) {
            std::vector<FilterClause> prog;
            ParseFilter(expr, prog);
            return !prog.empty() && MatchClause(c, prog[0]);
        };
        Connection c = MakeRow(1);
        c.country = L"Germany";
        c.firstSeenTick = ::GetTickCount64() - 3600000ULL;   // ~1h old
        c.bpsKnown = true;
        c.rxBps = 4096.0;
        c.txBps = 0.0;
        c.tls.known = true;
        c.tls.secure = true;
        c.tls.protocol = 0x0304;
        c.tls.haveSni = true;
        c.tls.sni = L"example.com";

        Check(r, "filter.match.country", matches(L"country:germ", c) &&
                                            !matches(L"country:france", c));
        Check(r, "filter.match.tls", matches(L"tls:example", c) &&
                                          matches(L"tls:1.3", c));
        // `speed:` is a THRESHOLD, not a substring of the printed cell. It used
        // to compare against FormatBytes(4096) == "4.0 KB", so `speed:4.0`
        // passed as a substring and `speed:1KB` - the example filters.md gives -
        // could never match anything, because FormatBytes never emits "1kb".
        Check(r, "filter.match.speed",
              matches(L"speed:4KB", c) && !matches(L"speed:8KB", c) &&
                  matches(L"speed:", c));
        Check(r, "filter.match.duration", matches(L"duration:1h", c));

        // A row with no reading must not match a filter on that field.
        Connection blank = MakeRow(2);
        Check(r, "filter.match.unknown-excluded",
              !matches(L"country:x", blank) &&
              !matches(L"tls:1.3", blank) &&
              !matches(L"speed:1", blank) &&
              !matches(L"duration:1h", blank));
    }

    // 15. Row lifetime and annotations survive a refresh cycle, and the
    //     snapshot's own first sample cannot produce a bandwidth reading.
    {
        ConnectionStore store;
        Connection a = MakeRow(1);
        a.pinned = true;
        a.tag = kTagRed;
        store.ReplaceSnapshot({a});
        // The store owns the first-seen stamp: a brand-new row gets the
        // current tick, whatever the caller put there.
        const ULONGLONG stamped = store.Rows()[0].firstSeenTick;
        const bool wasStamped = (stamped != 0);

        // Second cycle: same endpoint, fresh struct. Age and the user's
        // annotations must be carried over from the previous row.
        Connection b = MakeRow(1);
        b.firstSeenTick = 0;
        b.pinned = false;
        b.tag = kTagNone;
        store.ReplaceSnapshot({b});
        const Connection& kept = store.Rows()[0];
        const bool carried =
            (kept.firstSeenTick == stamped) && kept.pinned &&
            (kept.tag == kTagRed);
        Check(r, "store.row-lifetime-and-bookmarks", wasStamped && carried);

        // A different endpoint is a new row and gets its own stamp. Compare
        // by endpoint identity, not by PID: MakeRow reuses PIDs across
        // indices, so matching on pid would find the older row. The two
        // snapshots run in the same millisecond, so "not equal" is NOT the
        // invariant - a fresh stamp is non-zero and never precedes the old
        // one.
        const ULONGLONG otherPort = store.Rows()[0].localPort + 1;
        Connection c = MakeRow(9);
        c.localPort = static_cast<UINT>(otherPort);
        ConnectionStore::FinalizeRow(c);
        store.ReplaceSnapshot({c});
        bool freshStamped = false;
        for (const Connection& row : store.Rows())
            if (row.localPort == c.localPort && !(row.flags & kRowRemoved))
                freshStamped = (row.firstSeenTick != 0 &&
                                row.firstSeenTick >= stamped);
        Check(r, "store.new-row-gets-new-stamp", freshStamped);
    }

    // 20. Elevation.
    //     These assert the decision table, which is the part that can be
    //     wrong; the token queries themselves were verified live on this
    //     machine, elevated and filtered (see todo.md 4.1).
    //
    //     The invariant is ONE-WAY and only one-way: "elevated => this user
    //     CAN be elevated". The converse does not hold, and the old checks
    //     asserted it anyway: an administrator working in an ordinary
    //     unelevated shell is admin-member and NOT elevated at that moment -
    //     the single most common state on Windows, and the one these checks
    //     were getting wrong on an unelevated shell. IsElevated() means
    //     "elevated right now"; IsAdminMember() means "this token can elevate
    //     AT ALL", which is deliberately not IsUserAnAdmin(). A one-shot CLI
    //     also cannot self-elevate, so a non-elevated admin is normal there.
    {
        const bool elev = IsElevated();
        const bool admin = IsAdminMember();
        Check(r, "elev.self-consistent", !elev || admin,
              "admin=" + std::to_string(admin) + " elev=" + std::to_string(elev));
        // Same one-way shape: having no reason to elevate is only WRONG while
        // this process really is elevated. A non-elevated admin member has
        // every right to be told why it did not elevate, and that reason is
        // not an error - the user simply declined or the shell was not
        // elevated to begin with.
        Check(r, "elev.reason-matches-state",
              !elev || ElevationUnavailableReason().empty(),
              "elev=" + std::to_string(elev) + " admin=" + std::to_string(admin));
        // The marker check must be false for a normal launch, otherwise
        // Reelevate would refuse to ever run.
        Check(r, "elev.not-marker-on-plain-launch", !IsElevatedInstance());
    }

    // 22. Column drag-reorder (7.1). The persisted order is a permutation
    //     over ALL columns while the header only ever sees the visible ones;
    //     folding one into the other is where this goes wrong silently, so it
    //     is tested directly rather than through the UI.
    {
        // A small synthetic model: "full" is columns 0..5, of which 1, 3 and 4
        // are visible. Using a toy set rather than the real COL_COUNT keeps
        // the expected answers readable.
        const std::vector<int> full = {0, 1, 2, 3, 4, 5};
        const std::vector<int> visible = {1, 3, 4};
        // Mask marking which of the toy columns are visible: 1, 3 and 4.
        const UINT32 mask = (1u << 1) | (1u << 3) | (1u << 4);

        // Unchanged: folding the current visible order is the identity. This
        // is the property the "did the user actually drag?" check relies on,
        // and the first implementation of this function failed it by moving
        // every visible column to the front.
        const std::vector<int> same = FoldVisibleOrder(full, mask, visible);
        Check(r, "colorder.fold-identity", same == full);

        // Swapping the first and last visible columns.
        const std::vector<int> swapped = {4, 3, 1};
        const std::vector<int> afterSwap = FoldVisibleOrder(full, mask, swapped);
        // Visible order must now read 4, 3, 1 in that subsequence...
        std::vector<int> visOnly;
        for (const int c : afterSwap) {
            if ((mask & (1u << c)) != 0) visOnly.push_back(c);
        }
        Check(r, "colorder.fold-applies-swap", visOnly == swapped);
        // ...and the hidden columns must all still be present exactly once.
        bool perm[6] = {};
        bool ok = afterSwap.size() == full.size();
        for (const int c : afterSwap) {
            if (c < 0 || c >= 6 || perm[c]) { ok = false; break; }
            perm[c] = true;
        }
        Check(r, "colorder.fold-stays-permutation", ok);
        // Hidden columns keep their exact positions: 0 stays at 0, 2 at 2,
        // 5 at 5. This is what makes re-enabling a column land it back where
        // the user left it, instead of at the bottom of the list.
        Check(r, "colorder.fold-holds-hidden-slots",
              afterSwap[0] == 0 && afterSwap[2] == 2 && afterSwap[5] == 5);

        // Inconsistent inputs must be refused, returning the order untouched,
        // rather than producing a plausible but wrong layout.
        Check(r, "colorder.fold-rejects-short-list",
              FoldVisibleOrder(full, mask, {4, 3}) == full);
        Check(r, "colorder.fold-rejects-hidden-column",
              FoldVisibleOrder(full, mask, {4, 3, 0}) == full);
        Check(r, "colorder.fold-rejects-empty-full",
              FoldVisibleOrder({}, mask, {}).empty());

        // The predicate the registry loader trusts. The 6 is the toy column
        // count of these arrays, and it is passed explicitly: the function
        // takes a count precisely so a short array is never read as if it
        // were COL_COUNT long.
        int good[6] = {3, 0, 5, 1, 4, 2};
        int dup[6] = {0, 0, 1, 2, 3, 4};      // duplicate
        int oob[6] = {0, 1, 2, 3, 4, 99};    // out of range
        int neg[6] = {0, 1, 2, 3, 4, -1};    // negative
        Check(r, "colorder.perm-accepts-valid", IsColumnPermutation(good, 6));
        Check(r, "colorder.perm-rejects-dup", !IsColumnPermutation(dup, 6));
        Check(r, "colorder.perm-rejects-oob", !IsColumnPermutation(oob, 6));
        Check(r, "colorder.perm-rejects-negative", !IsColumnPermutation(neg, 6));
        // A count larger than the real column count must be refused outright.
        Check(r, "colorder.perm-rejects-huge-count",
              !IsColumnPermutation(good, 1000));
        Check(r, "colorder.perm-rejects-null", !IsColumnPermutation(nullptr, 6));
    }

    // 23. Type-to-jump (7.2). The behaviours worth pinning are the two that
    //     make it feel right: repeated characters CYCLE rather than sticking,
    //     and a pause RESETS rather than extending a prefix the user can no
    //     longer see.
    {
        const std::vector<JumpCandidate> rows = {
            {1, L"chrome.exe"},
            {2, L"Code.exe"},
            {3, L"svchost.exe"},
            {4, L"svchost.exe"},
            {5, L"svchost.exe"},
            {6, L"teams.exe"},
        };

        TypeToJump t;
        ULONGLONG now = 1000;
        // 'c' matches two rows (chrome, Code); the first wins.
        Check(r, "jump.first-match", t.Feed(L'c', now, rows) == 0);
        // Repeating cycles to the next match rather than sticking.
        Check(r, "jump.repeat-cycles", t.Feed(L'c', now, rows) == 1);
        // ...and wraps back around.
        Check(r, "jump.repeat-wraps", t.Feed(L'c', now, rows) == 0);
        // A repeat must NOT grow the prefix. Pressing "c" three times is three
        // navigation steps, not a search for "ccc" - the first version appended
        // the character, so the second press searched "cc", matched nothing,
        // and the feature looked completely dead.
        Check(r, "jump.repeat-keeps-prefix", t.Prefix() == L"c");

        // Multi-character prefix narrows to the right group.
        TypeToJump t2;
        Check(r, "jump.multi-char", t2.Feed(L's', now, rows) == 2);
        Check(r, "jump.multi-char-narrow",
              t2.Feed(L'v', now, rows) == 2);
        Check(r, "jump.multi-char-prefix", t2.Prefix() == L"sv");
        // 'c' after "sv" completes "svc", still matching svchost.
        Check(r, "jump.full-prefix", t2.Feed(L'c', now, rows) == 2);
        Check(r, "jump.full-prefix-text", t2.Prefix() == L"svc");

        // A prefix nothing matches returns -1 and does NOT move to row 0: a
        // failed jump must leave the selection alone.
        TypeToJump t3;
        Check(r, "jump.no-match", t3.Feed(L'z', now, rows) == -1);
        Check(r, "jump.no-match-keeps-prefix", t3.Prefix() == L"z");

        // Case-insensitive: 'S' must find svchost just as 's' does.
        TypeToJump t4;
        Check(r, "jump.case-insensitive", t4.Feed(L'S', now, rows) == 2);

        // A pause longer than the timeout restarts the search.
        TypeToJump t5;
        t5.Feed(L's', now, rows);
        const ULONGLONG later = now + kTypeToJumpTimeoutMs + 500;
        // 'v' after the pause must be treated as a NEW first character, so it
        // looks for rows starting "v" and finds none - not "sv".
        Check(r, "jump.timeout-resets", t5.Feed(L'v', later, rows) == -1);
        // Within the timeout it extends instead.
        TypeToJump t6;
        t6.Feed(L's', now, rows);
        Check(r, "jump.within-timeout-extends",
              t6.Feed(L'v', now + 100, rows) == 2);

        // Control characters and a stray high surrogate must be ignored rather
        // than accumulated - otherwise one IME commit breaks the feature for
        // the rest of the session.
        TypeToJump t7;
        Check(r, "jump.ignores-control", t7.Feed(L'\t', now, rows) == -1);
        Check(r, "jump.control-left-prefix-empty", t7.Prefix().empty());
        Check(r, "jump.ignores-delete", t7.Feed(L'\x7F', now, rows) == -1);
        Check(r, "jump.ignores-space-start", t7.Feed(L' ', now, rows) == -1);

        // Backspace backs out of a mistyped prefix.
        TypeToJump t8;
        t8.Feed(L's', now, rows);
        t8.Feed(L'z', now, rows);   // "sz" matches nothing
        Check(r, "jump.backspace-recovers", t8.Feed(L'\b', now, rows) == 2);
        Check(r, "jump.backspace-clears", t8.Feed(L'\b', now, rows) == -1);

        // Prefix matching is a prefix test, not a substring search.
        Check(r, "jump.prefix-not-substring",
              !LabelStartsWith(L"svchost.exe", L"host"));
        Check(r, "jump.prefix-basic", LabelStartsWith(L"svchost", L"svc"));
        Check(r, "jump.prefix-empty-matches", LabelStartsWith(L"anything", L""));

        // An empty list must never be indexed.
        TypeToJump t9;
        Check(r, "jump.empty-list", t9.Feed(L'a', now, {}) == -1);
    }

    // 24. Row grouping (5.1). The failure modes here are silent - a wrong
    //     count or a wrong total just looks like a rendering bug - so the
    //     arithmetic is pinned directly.
    {
        const std::wstring chrome = L"chrome.exe";
        const std::wstring svchost = L"svchost.exe";

        // Two processes, four rows, interleaved. Interleaving is the case
        // that matters: it proves groups collect members that are not
        // adjacent, which is what a sorted flat list looks like in practice.
        std::vector<GroupRow> rows;
        // Every row of a PID carries the SAME cumulative counters, because
        // SetTraffic writes per-PID. That is the property the max-not-sum
        // rule exists for.
        auto add = [&rows](std::uint32_t pid, const std::wstring* name,
                           bool tcp, std::uint64_t rx, std::uint64_t tx,
                           bool removed) {
            GroupRow r;
            r.pid = pid;
            r.processName = name;
            r.tcp = tcp;
            r.trafficRx = rx;
            r.trafficTx = tx;
            r.removed = removed;
            rows.push_back(r);
        };
        add(100, &chrome, true,  1000, 500, false);
        add(200, &svchost, true, 7000, 3000, false);
        add(100, &chrome, true,  1000, 500, false);
        add(200, &svchost, false, 7000, 3000, true);

        const std::vector<ProcessGroup> g = GroupByProcess(rows);
        Check(r, "group.count", g.size() == 2);
        // First-appearance order, so toggling grouping does not make rows
        // appear to move.
        Check(r, "group.first-appearance-order",
              g.size() == 2 && g[0].pid == 100 && g[1].pid == 200);
        Check(r, "group.members-collected",
              g.size() == 2 && g[0].members.size() == 2 &&
              g[1].members.size() == 2);
        // Non-adjacent members: chrome is row 0 and row 2.
        Check(r, "group.non-adjacent-members",
              g.size() == 2 && g[0].members[0] == 0 && g[0].members[1] == 2);
        // THE KEY TEST. Summing would give 2000/1000 for chrome; the real
        // per-PID total is 1000/500. A ten-fold over-report is exactly the
        // kind of number that ships unnoticed.
        Check(r, "group.traffic-not-summed",
              g.size() == 2 && g[0].trafficRx == 1000 && g[0].trafficTx == 500,
              "got " + std::to_string(g[0].trafficRx) + "/" +
                  std::to_string(g[0].trafficTx));
        Check(r, "group.total-traffic",
              g.size() == 2 && g[0].TotalTraffic() == 1500);
        Check(r, "group.ghost-count",
              g.size() == 2 && g[1].ghostCount == 1 && g[0].ghostCount == 0);
        Check(r, "group.proto-mixed",
              g.size() == 2 && g[1].hasTcp && g[1].hasUdp);
        Check(r, "group.name", g.size() == 2 && g[0].name == L"chrome.exe");
        Check(r, "group.name-known", g.size() == 2 && g[0].nameKnown);

        // An unresolved process must still produce a usable header naming the
        // PID, not an empty cell the user cannot act on.
        std::vector<GroupRow> anon = {GroupRow{4242, nullptr, true, 0, 0, 1, false}};
        const std::vector<ProcessGroup> ag = GroupByProcess(anon);
        Check(r, "group.unnamed-has-placeholder",
              ag.size() == 1 && ag[0].name == L"PID 4242" && !ag[0].nameKnown);

        // Two processes sharing a NAME must stay separate - grouping is by
        // PID precisely because svchost.exe is not one process.
        const std::wstring dup = L"svchost.exe";
        std::vector<GroupRow> twins = {
            GroupRow{10, &dup, true, 1, 1, 1, false},
            GroupRow{11, &dup, true, 2, 2, 1, false},
        };
        Check(r, "group.same-name-different-pid",
              GroupByProcess(twins).size() == 2);

        // Empty input.
        Check(r, "group.empty", GroupByProcess({}).empty());

        // Overflow: a counter near the top must saturate, not wrap to a small
        // plausible number.
        Check(r, "group.saturating-add",
              SaturatingAdd(UINT64_MAX, 5) == UINT64_MAX);
        Check(r, "group.saturating-add-normal", SaturatingAdd(2, 3) == 5);
    }

    // 25. Freeze / pause (5.5). The age must be honest, and a clock that
    //     appears to run backwards must not produce a negative age.
    {
        Check(r, "freeze.age", FrozenAgeMs(1000, 4000) == 3000);
        Check(r, "freeze.age-zero", FrozenAgeMs(5000, 5000) == 0);
        Check(r, "freeze.age-clamped-backwards", FrozenAgeMs(5000, 1000) == 0);
        Check(r, "freeze.age-huge", FrozenAgeMs(0, UINT64_MAX) == UINT64_MAX);
    }

    // 26. Shortcut sheet (7.6) + About summary (7.5). The sheet is data, so
    //     it can be checked against what the app actually binds. Every key
    //     listed must be one the accelerator table or the WM_CHAR handler
    //     handles - a sheet documenting a shortcut that does not work is worse
    //     than no sheet.
    {
        const std::vector<Shortcut>& sc = AllShortcuts();
        Check(r, "sheet.non-empty", !sc.empty());

        // No duplicate key entries, and none blank.
        bool dup = false, blank = false;
        for (size_t i = 0; i < sc.size(); ++i) {
            if (sc[i].keys.empty() || sc[i].description.empty()) blank = true;
            for (size_t j = i + 1; j < sc.size(); ++j)
                if (sc[i].keys == sc[j].keys) dup = true;
        }
        Check(r, "sheet.no-duplicate-keys", !dup);
        Check(r, "sheet.no-blank-entries", !blank);

        // The keys this build actually binds in the accelerator table. If one
        // of these is missing from the sheet the sheet is out of date.
        const wchar_t* const must[] = {L"F1", L"F5", L"F6", L"F7",
                                       L"Ctrl+C", L"Ctrl+A", L"Ctrl+F",
                                       L"Ctrl+E", L"Esc", L"Del"};
        for (const wchar_t* key : must) {
            bool found = false;
            for (const Shortcut& s : sc)
                if (s.keys == key) { found = true; break; }
            // Test names are narrow std::string, so the key must be converted
            // explicitly. std::string(key, key + len) from a wchar_t* is a
            // narrowing conversion and /WX rejects it - which is the compiler
            // catching exactly the kind of silent mojibake it should.
            std::string name = "sheet.documents-";
            for (const wchar_t* p = key; *p != L'\0'; ++p)
                name.push_back(static_cast<char>(*p & 0x7F));
            Check(r, name.c_str(), found);
        }

        // The rendered sheet must actually contain the keys, and must explain
        // type-to-jump - the one non-obvious interaction.
        const std::wstring sheet = ShortcutsText();
        Check(r, "sheet.renders-keys", sheet.find(L"F6") != std::wstring::npos);
        Check(r, "sheet.explains-type-to-jump",
              sheet.find(L"SAME letter") != std::wstring::npos);
        // Every listed key appears in the rendered output, so a formatting
        // bug cannot silently drop a row.
        bool allRendered = true;
        for (const Shortcut& s : sc)
            if (sheet.find(s.keys) == std::wstring::npos) allRendered = false;
        Check(r, "sheet.renders-every-entry", allRendered);
    }

    // 27. About summary (7.5). The capability block must reflect the state it
    //     is given, and must NOT claim a capability it does not have - the
    //     unelevated text is the one a user relies on to understand a blank
    //     Traffic column.
    {
        BuildSummary un;
        un.elevated = false;
        un.etwRunning = false;
        un.trafficFallback = false;
        un.geoIpLoaded = false;
        un.presetsAvailable = true;
        un.visibleColumnCount = 10;
        un.totalColumnCount = 23;
        un.rowCount = 137;
        const std::wstring low = AboutText(un);
        Check(r, "about.unelevated-says-no", low.find(L"Administrator") !=
                                                std::wstring::npos);
        Check(r, "about.mentions-needs-admin",
              low.find(L"needs administrator") != std::wstring::npos);
        Check(r, "about.reports-columns",
              low.find(L"10 of 23") != std::wstring::npos);
        Check(r, "about.reports-rows",
              low.find(L"137") != std::wstring::npos);

        BuildSummary hi = un;
        hi.elevated = true;
        hi.etwRunning = true;
        hi.geoIpLoaded = true;
        const std::wstring high = AboutText(hi);
        // The elevated copy must NOT still say the unelevated limitation.
        Check(r, "about.elevated-differs", high != low);
        Check(r, "about.elevated-has-etw",
              high.find(L"ETW kernel logger") != std::wstring::npos);
        Check(r, "about.elevated-needs-no-admin-hint",
              high.find(L"Traffic off") == std::wstring::npos);
        Check(r, "about.geoip-state-differs",
              low.find(L"not loaded") != std::wstring::npos &&
              high.find(L"loaded") != std::wstring::npos);

        // D23. A count nobody measured must NOT be printed. The CLI has no
        // persisted GUI view mask and (before the fix) took no snapshot, and
        // both printed a confident "0 of 23" / "Connections 0" - which reads
        // as a fact about the machine ("this box has no connections") rather
        // than as "nobody looked". An absent number is honest; a wrong one is
        // not, so the line is omitted instead.
        BuildSummary cli;
        cli.elevated = false;
        cli.presetsAvailable = true;
        cli.totalColumnCount = 23;
        cli.visibleColumnCount = 0;
        cli.rowCount = 0;
        cli.columnCountKnown = false;   // no GUI mask in a CLI run
        cli.rowCountKnown = false;      // no snapshot was taken
        const std::wstring bare = AboutText(cli);
        Check(r, "about.unknown-columns-omitted",
              bare.find(L"Columns shown") == std::wstring::npos,
              WideToUtf8(bare));
        Check(r, "about.unknown-rows-omitted",
              bare.find(L"Connections") == std::wstring::npos,
              WideToUtf8(bare));

        // ...and a MEASURED count is still printed, so the omission is not
        // just the line being dropped everywhere.
        BuildSummary measured = cli;
        measured.columnCountKnown = true;
        measured.rowCountKnown = true;
        measured.visibleColumnCount = 10;
        measured.rowCount = 137;
        const std::wstring shown = AboutText(measured);
        Check(r, "about.measured-columns-shown",
              shown.find(L"10 of 23") != std::wstring::npos,
              WideToUtf8(shown));
        Check(r, "about.measured-rows-shown",
              shown.find(L"137") != std::wstring::npos, WideToUtf8(shown));

        // The traffic line must reflect a real probe: a run that cannot use
        // the socket fallback says so, and one that can says which source it
        // has - the old build claimed "none (needs administrator)" even on
        // Windows 10 1709+ where the fallback works unelevated.
        BuildSummary fb;
        fb.elevated = false;
        fb.presetsAvailable = true;
        fb.totalColumnCount = 23;
        fb.columnCountKnown = false;
        fb.rowCountKnown = false;
        fb.trafficFallback = true;
        const std::wstring fbt = AboutText(fb);
        Check(r, "about.fallback-says-so",
              fbt.find(L"socket fallback") != std::wstring::npos,
              WideToUtf8(fbt));
        Check(r, "about.fallback-not-claimed-as-none",
              fbt.find(L"none (needs administrator)") == std::wstring::npos,
              WideToUtf8(fbt));

        // The combination that shipped broken: ELEVATED with no collector
        // running. The CLI never opens a session (correctly - it must not
        // claim one it did not start) and it forces trafficFallback to false
        // when elevated, so both traffic flags ended up false while
        // elevated was true, and the final branch - written only for the
        // unelevated case - answered "none (needs administrator)" to an
        // administrator, two lines under "Administrator yes". Every other
        // pairing of these flags is covered above; this one was not, which
        // is how it reached a user.
        BuildSummary ec = un;
        ec.elevated = true;
        ec.etwRunning = false;
        ec.trafficFallback = false;
        const std::wstring ect = AboutText(ec);
        Check(r, "about.elevated-none-not-needs-admin",
              ect.find(L"none (needs administrator)") == std::wstring::npos,
              WideToUtf8(ect));
        Check(r, "about.elevated-none-says-why",
              ect.find(L"no collector is running") != std::wstring::npos,
              WideToUtf8(ect));
    }

    // 28. SetCountry (4.3 join). The filter must be able to search a value the
    //      column is displaying - a lower-case key that is not rebuilt makes
    //      `country:de` fail while the column reads "DE".
    {
        ConnectionStore st;
        Connection a;
        a.family = AF_INET;
        a.protocol = IPPROTO_TCP;
        a.state = MIB_TCP_STATE_ESTAB;
        a.localAddress = L"10.0.0.1";
        a.remoteAddress = L"8.8.8.8";
        a.remotePort = 443;
        a.pid = 100;
        st.ReplaceSnapshot({a});
        // SetView is REQUIRED before ViewRow: the view index is empty until a
        // query is applied, so ViewRow(0) would return nullptr and the
        // dereferences below would fault. This is the store's contract, not a
        // quirk of the test.
        ViewQuery q;
        st.SetView(q);

        Check(r, "geo.setcountry", st.SetCountry(L"8.8.8.8", L"US"));
        Check(r, "geo.setcountry-again-is-noop",
              !st.SetCountry(L"8.8.8.8", L"US"));
        const Connection* r0 = st.ViewRow(0);
        Check(r, "geo.setcountry-row-exists", r0 != nullptr);
        Check(r, "geo.country-visible", r0 != nullptr && r0->country == L"US");
        // The lower-case key is what MatchFilter searches. Parsed through the
        // real public entry points rather than a private helper, so this
        // tests the same path the filter box uses.
        std::vector<FilterClause> prog;
        Check(r, "geo.filter-parses", ParseFilter(L"country:us", prog));
        Check(r, "geo.country-filterable",
              r0 != nullptr && MatchFilter(*r0, prog));
        // Clearing must work too - a peer moving out of a covered range.
        const Connection* afterClear = nullptr;
        Check(r, "geo.setcountry-clear",
              st.SetCountry(L"8.8.8.8", L"") &&
              (afterClear = st.ViewRow(0)) != nullptr &&
              afterClear->country.empty());
        // An address with no rows must not crash or claim success.
        Check(r, "geo.setcountry-unknown-addr",
              !st.SetCountry(L"9.9.9.9", L"US"));
        Check(r, "geo.setcountry-empty-addr",
              !st.SetCountry(L"", L"US"));
    }

    // 29. JoinBookmarks (5.3). The case that matters is the CLEAR: a row
    //     whose endpoint is not in the supplied set must lose its pin, or
    //     removing a bookmark leaves a pin stuck on the row forever.
    {
        ConnectionStore st;
        // Every row needs BOTH the text and the binary address forms set.
        // ConnectionStore::KeyOf builds the row identity from local4/localPort/
        // remote4/remotePort, so rows that share the (default-zero) binary
        // fields are the SAME row to the store no matter what their text says -
        // the first version of this test created three rows that all collapsed
        // into one and every count came out wrong.
        int nextLocalPort = 50000;
        auto mk = [&nextLocalPort](const wchar_t* remote) {
            Connection c;
            c.family = AF_INET;
            c.protocol = IPPROTO_TCP;
            c.state = MIB_TCP_STATE_ESTAB;
            c.localAddress = L"10.0.0.1";
            c.remoteAddress = remote;
            c.remotePort = 443;
            c.pid = 100;
            c.localPort = static_cast<UINT>(nextLocalPort++);
            ::InetPtonW(AF_INET, L"10.0.0.1", &c.local4);
            ::InetPtonW(AF_INET, remote, &c.remote4);
            return c;
        };
        st.ReplaceSnapshot({mk(L"1.1.1.1"), mk(L"2.2.2.2"), mk(L"3.3.3.3")});
        ViewQuery q;
        st.SetView(q);

        // Bookmark the middle row only, with a colour. Address AND port, which
        // is the bookmark's real identity.
        st.JoinBookmarks({BookmarkMark{L"2.2.2.2", 443, kBookmarkTagRed, L""}});
        int pinned = 0, tagged = 0;
        for (size_t i = 0; i < st.View().size(); ++i) {
            const Connection* c = st.ViewRow(i);
            if (c == nullptr) continue;
            if (c->pinned) ++pinned;
            if (c->tag == kBookmarkTagRed) ++tagged;
        }
        Check(r, "bm.join-marks-one", pinned == 1 && tagged == 1);
        Check(r, "bm.stats-counts-pinned", st.Stats().pinned == 1);

        // The Bookmarks column must show the tag, not a dash.
        bool sawTag = false;
        for (size_t i = 0; i < st.View().size(); ++i) {
            const Connection* c = st.ViewRow(i);
            if (c == nullptr || !c->pinned) continue;
            wchar_t buf[64] = {0};
            ConnectionStore::GetColumnText(*c, COL_PINNED, buf, 64);
            if (std::wstring(buf) != L"—" && buf[0] != L'\0') sawTag = true;
        }
        Check(r, "bm.column-shows-tag", sawTag);

        // THE CLEAR. An empty set must unpin everything.
        st.JoinBookmarks({});
        pinned = 0;
        for (size_t i = 0; i < st.View().size(); ++i) {
            const Connection* c = st.ViewRow(i);
            if (c != nullptr && c->pinned) ++pinned;
        }
        Check(r, "bm.clear-removes-pin", pinned == 0);
        Check(r, "bm.clear-resets-stats", st.Stats().pinned == 0);

        // Re-adding brings it back, and two DIFFERENT connections to the same
        // peer are both marked - a bookmark identifies the endpoint, not the
        // connection. mk() now gives each row its own local port, so these
        // are genuinely two connections rather than one row counted twice.
        st.ReplaceSnapshot({mk(L"2.2.2.2"), mk(L"2.2.2.2"), mk(L"1.1.1.1")});
        st.SetView(q);
        st.JoinBookmarks({BookmarkMark{L"2.2.2.2", 443, kBookmarkTagBlue, L""}});
        // Ghost rows (from the previous snapshot) are still counted by View
        // until a second cycle passes, so compare against the rows this
        // snapshot actually holds rather than against a hard-coded 2.
        size_t live = 0, livePinned = 0;
        for (const Connection& c : st.Rows()) {
            if (c.flags & kRowRemoved) continue;
            ++live;
            if (c.pinned) ++livePinned;
        }
        Check(r, "bm.all-rows-to-same-peer", live == 3 && livePinned == 2);

        // An empty store against an empty row set is a safe no-op.
        ConnectionStore empty;
        empty.JoinBookmarks({BookmarkMark{L"9.9.9.9", 443, kBookmarkTagRed, L""}});
        Check(r, "bm.empty-store-safe", empty.View().empty());

        // THE PORT IS PART OF THE IDENTITY (D22). A bookmark for the same
        // address on a DIFFERENT remote port must not mark this row: the join
        // used to match on the address alone, so a bookmark on
        // 107.155.105.90:9999 painted the live :443 row "red" (measured). A
        // "this peer is suspicious" mark that flags every unrelated session
        // to that host is worse than no mark at all.
        {
            ConnectionStore pt;
            Connection a1 = mk(L"5.5.5.5");
            Connection a2 = mk(L"5.5.5.5");
            a2.remotePort = 8443;   // same host, different service
            ::InetPtonW(AF_INET, L"5.5.5.5", &a2.remote4);
            pt.ReplaceSnapshot({a1, a2});
            pt.JoinBookmarks({BookmarkMark{L"5.5.5.5", 443, kBookmarkTagRed,
                                           L"vendor api"}});
            size_t marked443 = 0, marked8443 = 0, noted = 0;
            for (const Connection& c : pt.Rows()) {
                if (c.remotePort == 443 && c.pinned) ++marked443;
                if (c.remotePort == 8443 && c.pinned) ++marked8443;
                if (!c.note.empty()) ++noted;
            }
            Check(r, "bm.port-is-part-of-identity",
                  marked443 == 1 && marked8443 == 0,
                  "443=" + std::to_string(marked443) +
                      " 8443=" + std::to_string(marked8443));
            // ...and the note rode along with it, so `note:` can find it.
            Check(r, "bm.note-reaches-the-row", noted == 1,
                  "noted=" + std::to_string(noted));

            // A bookmark that has gone must take its note with it.
            pt.JoinBookmarks({});
            size_t stillNoted = 0;
            for (const Connection& c : pt.Rows())
                if (!c.note.empty()) ++stillNoted;
            Check(r, "bm.clear-removes-the-note", stillNoted == 0);
        }
    }

    // 32. The `note:` filter field (D26). Before this, "note" was not a field
    // name at all, so `--filter note:vendor` degraded to a substring search
    // for the literal text "note:vendor" and could never match: a note was
    // writable and listable but unreachable from the connection table, which
    // made it half a feature.
    {
        // A SINGLE-WORD note. `note:vendor api` is TWO AND-ed clauses
        // (`note:vendor` AND a bare `api`), which is the grammar working
        // correctly - a multi-word value needs quoting, and the check below
        // pins that too.
        std::vector<FilterClause> prog;
        ParseFilter(L"note:vendor", prog);
        const bool parsed = (prog.size() == 1) &&
                            (prog[0].field == FilterField::Note) &&
                            (prog[0].text == L"vendor");
        Check(r, "note.field-parses", parsed);

        Connection c;
        c.note = L"Vendor API";
        c.lowerNote = ToLowerW(c.note);
        Connection none;
        Check(r, "note.matches-case-insensitively",
              MatchClause(c, prog[0]));
        Check(r, "note.absent-note-does-not-match",
              !MatchClause(none, prog[0]));

        // A bare `note:` selects every row that HAS a note, which is the
        // useful "show me everything I have annotated" query.
        std::vector<FilterClause> bare;
        ParseFilter(L"note:", bare);
        Check(r, "note.bare-selects-annotated",
              bare.size() == 1 && MatchClause(c, bare[0]) &&
                  !MatchClause(none, bare[0]));

        // A quoted multi-word value is ONE clause, not two AND-ed tokens.
        // Without this a note like "vendor api" is unreachable, because the
        // space would have been read as AND - the note would be writable and
        // then unfindable, which is exactly the half-a-feature this fixes.
        std::vector<FilterClause> quoted;
        ParseFilter(L"note:\"vendor api\"", quoted);
        Check(r, "note.quoted-value-is-one-clause",
              quoted.size() == 1 &&
                  (quoted[0].field == FilterField::Note) &&
                  (quoted[0].text == L"vendor api") &&
                  MatchClause(c, quoted[0]),
              quoted.empty() ? "no clauses" : WideToUtf8(quoted[0].text));

        // The same for the single-quoted form, which cmd.exe users reach for
        // as readily as double quotes.
        std::vector<FilterClause> squoted;
        ParseFilter(L"note:'vendor api'", squoted);
        Check(r, "note.single-quoted-value",
              squoted.size() == 1 && squoted[0].text == L"vendor api");

        // Quoting works for any field, not just note: a path fragment with a
        // space in it is the other case that needs it.
        std::vector<FilterClause> pth;
        ParseFilter(L"path:\"program files\"", pth);
        Check(r, "quote.any-field",
              pth.size() == 1 && (pth[0].field == FilterField::Path) &&
                  (pth[0].text == L"program files"));

        // Quoting a BARE value changes tokenisation, not meaning: it stays a
        // substring search over the whole row, exactly like an unquoted one.
        std::vector<FilterClause> anyq;
        ParseFilter(L"\"chrome.exe\"", anyq);
        Check(r, "quote.bare-value-searches-everything",
              anyq.size() == 1 && (anyq[0].field == FilterField::Any) &&
                  (anyq[0].text == L"chrome.exe"));
    }

    // 31. ViewState and the PresetView alias.
    //
    //     PresetView WAS a separate struct and had ZERO test coverage - the
    //     module was exercised only through the GUI's Save/Load, behind a
    //     modal dialog, which is exactly how 5.1 grouping and GeoIP shipped as
    //     dead code. Merging PresetView into ViewState is therefore a change
    //     that could not have been verified at all. These tests are the
    //     verification: they drive ViewState::ApplyTo against a synthetic
    //     store, and they round-trip a real preset through the registry, so a
    //     future change to either type fails here in milliseconds instead of
    //     being discovered by a user.
    {
        ConnectionStore store;
        UINT nextPort = 50000;
        const auto mk = [&](const wchar_t* remote, DWORD pid) {
            Connection c;
            c.localAddress = L"10.0.0.1";
            c.remoteAddress = remote;
            c.remotePort = 443;
            c.pid = pid;
            c.localPort = static_cast<UINT>(nextPort++);
            c.processName = L"alpha.exe";
            ::InetPtonW(AF_INET, L"10.0.0.1", &c.local4);
            ::InetPtonW(AF_INET, remote, &c.remote4);
            ConnectionStore::FinalizeRow(c);
            return c;
        };
        // Two processes, three connections: the shape that makes grouping
        // observable (3 flat rows must become 2 group rows).
        store.ReplaceSnapshot({mk(L"1.1.1.1", 100), mk(L"2.2.2.2", 100),
                               mk(L"3.3.3.3", 200)});

        // A default ViewState is "show everything, default order": the same
        // thing an untouched GUI window shows.
        ViewState v;
        Check(r, "view.default-is-trivial", v.IsTrivial());
        v.ApplyTo(&store);
        Check(r, "view.default-shows-all", store.View().size() == 3);

        // Grouping collapses to one row per process.
        v.grouped = true;
        Check(r, "view.grouped-not-trivial", !v.IsTrivial());
        v.ApplyTo(&store);
        Check(r, "view.grouping-collapses", store.View().size() == 2);
        Check(r, "view.grouping-flag-agrees", store.Grouped());

        // Turning it off restores the flat list exactly - the property 5.1
        // was verified for, now covered without a window.
        v.grouped = false;
        v.ApplyTo(&store);
        Check(r, "view.ungrouping-restores-flat", store.View().size() == 3);

        // A filter narrows the view, and goes down the SAME ParseFilter path
        // the GUI's filter box uses.
        v.filter = L"2.2.2.2";
        v.ApplyTo(&store);
        Check(r, "view.filter-narrows", store.View().size() == 1);
        v.filter.clear();

        // Sort is applied by ApplyTo, so a caller cannot forget it: the store's
        // sort must match what the view asked for.
        v.sortColumn = COL_REMOTE;
        v.sortAsc = true;
        v.ApplyTo(&store);
        Check(r, "view.sort-is-applied", store.SortColumn() == COL_REMOTE);

        // A null store must be a safe no-op, not a crash: ApplyTo is called
        // from front ends that may have no store during teardown.
        v.ApplyTo(nullptr);
        Check(r, "view.null-store-safe", true);

        // ---- PresetView round-trip through the real registry ----
        //
        // PresetView is an alias of ViewState now. What must not change is the
        // on-disk schema, so a preset written by an earlier build still loads.
        const std::wstring kName = L"_selftest_viewstate_";
        Presets::Delete(kName);          // never assume a clean start

        PresetView pv;
        pv.filter = L"pid:100-200 proto:tcp";
        pv.colVisible = 0x0ABCu;
        pv.sortColumn = COL_REMOTE;
        pv.sortAsc = false;
        pv.sources = kPresetSourceHosts | kPresetSourceGeoIp;

        const PresetSave saved = Presets::Save(kName, pv, true);
        Check(r, "preset.saves", saved == PresetSave::kCreated ||
                             saved == PresetSave::kOverwrote);

        PresetView back;
        const bool loaded = Presets::Load(kName, &back);
        Check(r, "preset.round-trips", loaded &&
            back.filter == pv.filter &&
            back.colVisible == pv.colVisible &&
            back.sortColumn == pv.sortColumn &&
            back.sortAsc == pv.sortAsc &&
            back.sources == pv.sources);

        // A loaded preset must be directly usable as a view - that is the whole
        // point of making them one type.
        back.ApplyTo(&store);
        Check(r, "preset.applies-as-a-view", store.SortColumn() == COL_REMOTE);

        // Save must refuse to clobber without an explicit opt-in, and must
        // then succeed when given one.
        const PresetSave again = Presets::Save(kName, pv, false);
        Check(r, "preset.refuses-clobber", again == PresetSave::kExists);
        const PresetSave over = Presets::Save(kName, pv, true);
        Check(r, "preset.overwrites-when-allowed",
              over == PresetSave::kOverwrote);

        Check(r, "preset.deletes", Presets::Delete(kName));
        Check(r, "preset.delete-is-idempotent", Presets::Delete(kName));
    }

    // 30. Chart export (7.7a) and zoom reset (7.7b). The degenerate-range
    //     case is the one that produces infinities and NaNs on a live panel,
    //     and the locale case is the one that silently misaligns a whole
    //     file, so both are pinned here rather than only in a scratch test.
    {
        // --- 7.7b: zoom. A flat series (every sample identical) has
        // high == low, so anything dividing by the span yields inf/NaN.
        const std::vector<double> flat(10, 42.0);
        ZoomRect z;
        Check(r, "zoom.fit-flat", FitValueWindow(flat, nullptr, &z));
        Check(r, "zoom.flat-has-span", z.vHi > z.vLo);
        const ZoomRect def = ResetPanelZoom(flat, nullptr, 0.0);
        Check(r, "zoom.default-covers-all", def.kLo == 0 &&
                                            def.kHi == flat.size() - 1);
        Check(r, "zoom.default-has-span", def.vHi > def.vLo);
        Check(r, "zoom.isdefault-agrees", IsDefaultZoom(def, flat, nullptr, 0.0));

        // A deliberately broken rect: zero height. ClampZoom promises to
        // repair it, and that promise is what lets callers divide by the span
        // without their own guard.
        ZoomRect broken;
        broken.kLo = 0; broken.kHi = 0;
        broken.vLo = 5.0; broken.vHi = 5.0;
        const ZoomRect fixedZoom = ClampZoom(broken, flat.size(), def);
        Check(r, "zoom.clamp-repairs-zero-height", fixedZoom.vHi > fixedZoom.vLo);
        Check(r, "zoom.clamp-bounds-legal",
              fixedZoom.kLo <= fixedZoom.kHi &&
              fixedZoom.kHi < flat.size());

        // Empty series: a safe degenerate rect, not a crash or an infinity.
        const std::vector<double> none;
        const ZoomRect noneDef = ResetPanelZoom(none, nullptr, 0.0);
        Check(r, "zoom.empty-is-single-point",
              noneDef.kLo == noneDef.kHi);
        Check(r, "zoom.empty-has-span", noneDef.vHi > noneDef.vLo);
        // Resetting an empty panel list must be a no-op, not a crash.
        Check(r, "zoom.resetall-empty",
              ResetAllPanelZoom({}, {}, {}).empty());

        // --- 7.7a: CSV. A name containing a comma must be quoted, or the
        // column count silently shifts for every row after it.
        {
            const std::vector<double> a = {1.5, 2.5};
            std::vector<ChartSeriesInfo> si;
            ChartSeriesInfo s1;
            s1.name = L"has,comma";
            s1.unit = L"percent";
            s1.seriesA = &a;
            s1.seriesB = nullptr;
            si.push_back(s1);
            ChartSample s1m, s2m;
            s1m.value = 1.0;
            s2m.value = 2.0;
            ChartExport ex;
            ex.samples = {s1m, s2m};
            const std::wstring csv = SeriesToCsvWide(si, ex);
            Check(r, "csv.quotes-comma-name",
                  csv.find(L"\"has,comma (percent)\"") != std::wstring::npos);
            // An unknown sample must become an EMPTY cell, never a number a
            // spreadsheet would plot.
            ChartSample unk;
            unk.value = kChartUnknown;
            ChartExport exU;
            exU.samples = {unk, s2m};
            const std::wstring csvU = SeriesToCsvWide(si, exU);
            Check(r, "csv.unknown-is-empty-not-sentinel",
                  csvU.find(L",-1,") == std::wstring::npos);
            // Decimal separator must be '.' so a comma-decimal locale cannot
            // misalign every column in the file. Checked on the DATA rows
            // only: the "#" preamble carries a stats line with a comma in it
            // by design, so searching the whole document for a comma-decimal
            // finds that and says nothing about the numbers.
            //
            // The number has to live in BOTH places: 'seriesA' is what the
            // panel's own series holds, and each ChartSample is the reading
            // for one instant. The first version of this test set only the
            // ChartSample and left seriesA at 2.5, so the column rendered
            // 2.5 and the assertion correctly failed - the test was measuring
            // the wrong value, not the code emitting a bad one.
            const std::vector<double> bigA = {kChartUnknown, 1234.75};
            std::vector<ChartSeriesInfo> siL;
            ChartSeriesInfo sL;
            sL.name = L"rx";
            sL.unit = L"bytes_per_second";
            sL.seriesA = &bigA;
            sL.seriesB = nullptr;
            siL.push_back(sL);
            ChartSample big;
            big.value = 1234.75;
            ChartExport exL;
            exL.samples = {unk, big};
            const std::wstring csvL = SeriesToCsvWide(siL, exL);
            const size_t firstData = csvL.find(L"sample,time_sec");
            Check(r, "csv.has-data-rows", firstData != std::wstring::npos);
            if (firstData != std::wstring::npos) {
                const std::wstring body = csvL.substr(firstData);
                Check(r, "csv.decimal-point-not-comma",
                      body.find(L"1234.75") != std::wstring::npos);
                Check(r, "csv.no-comma-decimal-in-data",
                      body.find(L"1234,75") == std::wstring::npos);
            }
        }
    }

    // 31. Change-log buffer (5.4). The cap is the whole risk here: an
    //     unbounded log on a busy machine is a slow leak that shows up as
    //     the app getting heavier over hours, not as a crash. 20 000 events
    //     in proves the cap holds AND that the NEWEST survive - keeping the
    //     oldest would be the wrong truncation and is easy to get backwards.
    {
        ChangeLogWindow clw;
        Check(r, "clog.not-open-initially", !clw.IsOpen());
        Check(r, "clog.close-safe-when-never-created",
              (clw.Close(), true));

        std::vector<RowChange> batch;
        batch.reserve(20000);
        for (int i = 0; i < 20000; ++i) {
            RowChange ch;
            Connection c;
            c.family = AF_INET;
            c.protocol = IPPROTO_TCP;
            c.state = MIB_TCP_STATE_ESTAB;
            c.pid = 1000 + i;
            c.processName = L"probe.exe";
            c.localEndpoint = L"10.0.0.1:" + std::to_wstring(40000 + i);
            c.remoteEndpoint = L"93.184.216.34:443";
            c.stateLabel = L"ESTABLISHED";
            ch.kind = kChangeAppear;
            ch.row = c;
            batch.push_back(ch);
        }
        clw.AppendEvents(batch);
        Check(r, "clog.cap-holds",
              clw.Count() == ChangeLogWindow::kMaxEvents);
        Check(r, "clog.dropped-counted",
              clw.Dropped() == 20000ull - ChangeLogWindow::kMaxEvents);
        // Newest kept: the last event appended had port 40000 + 19999.
        Check(r, "clog.newest-survives",
              clw.ColumnText(clw.Count() - 1, ChangeLogWindow::kColLocal)
                      .find(L"59999") != std::wstring::npos);
        // Oldest evicted: the first retained is 20000 - 5000 = 15000.
        Check(r, "clog.truncates-oldest",
              clw.ColumnText(0, ChangeLogWindow::kColLocal)
                      .find(L"55000") != std::wstring::npos);
        // Out-of-range access must be safe, not a crash.
        Check(r, "clog.out-of-range-safe",
              clw.ColumnText(99999, ChangeLogWindow::kColLocal).empty());
        // Appending while closed must still buffer, so opening the window
        // later does not start from a blank slate after events were lost.
        clw.Clear();
        clw.AppendEvents({batch[0], batch[1]});
        Check(r, "clog.buffers-while-closed", clw.Count() == 2);
        clw.Clear();
        Check(r, "clog.clear-empties",
              clw.Count() == 0 && clw.Dropped() == 0);
    }

    // 20b. 5.3: the change-log event mask - the GUI half of `--event`.
    //     Pure logic, no window needed: the filter runs in AppendEvents, so it
    //     is fully testable headless and the UI harness only has to prove the
    //     CONTROLS are wired.
    {
        // A one-of-each batch, so any mask's effect is countable exactly.
        auto batchOf = [](int kind) {
            std::vector<RowChange> v;
            RowChange ch;
            ch.kind = kind;
            ch.row.localAddress = L"192.0.2.1";
            ch.row.localPort = 40000 + kind;
            ch.row.remoteAddress = L"198.51.100.1";
            ch.row.remotePort = 443;
            ch.row.stateLabel = L"ESTABLISHED";
            ch.row.pid = 4242;
            ch.row.processName = L"probe.exe";
            v.push_back(ch);
            return v;
        };
        const std::vector<RowChange> ap = batchOf(kChangeAppear);
        const std::vector<RowChange> di = batchOf(kChangeDisappear);
        const std::vector<RowChange> st = batchOf(kChangeState);

        // Default is EVERYTHING on, so anyone who never touches the boxes sees
        // the log they saw before this feature existed.
        ChangeLogWindow def;
        Check(r, "clog.mask-defaults-to-full", def.EventMaskIsFull() &&
              def.EventAppear() && def.EventDisappear() && def.EventState());
        def.AppendEvents(ap);
        def.AppendEvents(di);
        def.AppendEvents(st);
        Check(r, "clog.mask-default-keeps-everything", def.Count() == 3,
              "count=" + std::to_string(def.Count()));

        // One kind at a time.
        ChangeLogWindow onlyAppear;
        onlyAppear.SetEventMask(true, false, false);
        onlyAppear.AppendEvents(ap);
        onlyAppear.AppendEvents(di);
        onlyAppear.AppendEvents(st);
        Check(r, "clog.mask-selects-one-kind", onlyAppear.Count() == 1 &&
              onlyAppear.ColumnText(0, ChangeLogWindow::kColEvent) == L"APPEAR",
              "count=" + std::to_string(onlyAppear.Count()) + " first=" +
                  WideToUtf8(onlyAppear.ColumnText(
                      0, ChangeLogWindow::kColEvent)));

        ChangeLogWindow noAppear;
        noAppear.SetEventMask(false, true, true);
        noAppear.AppendEvents(ap);
        noAppear.AppendEvents(di);
        noAppear.AppendEvents(st);
        Check(r, "clog.mask-excludes-one-kind", noAppear.Count() == 2);

        // THE IMPORTANT ONE: a batch of entirely-excluded kinds must be a NO-OP,
        // not a reset. An early `return` on an empty batch is the natural way to
        // write this and the natural way to get it wrong is to fall through to
        // the cap arithmetic with a zero-sized batch - which would silently
        // discard the log's history for anyone who narrowed the mask while it
        // was open.
        ChangeLogWindow held;
        held.SetEventMask(true, true, true);
        held.AppendEvents(ap);
        held.AppendEvents(di);
        Check(r, "clog.mask-holds-before-narrowing", held.Count() == 2);
        held.SetEventMask(false, false, false);   // nothing selected
        held.AppendEvents(st);                      // entirely excluded
        Check(r, "clog.mask-excluded-batch-is-a-no-op", held.Count() == 2,
              "count=" + std::to_string(held.Count()));
        // ...and the retained rows are still the ORIGINAL ones: the mask is
        // applied on the way IN and never retroactively, so history is a record
        // of what happened rather than of what is currently selected.
        Check(r, "clog.mask-is-not-retroactive",
              held.ColumnText(0, ChangeLogWindow::kColEvent) == L"APPEAR" &&
                  held.ColumnText(1, ChangeLogWindow::kColEvent) == L"DISAPPEAR",
              "row0=" + WideToUtf8(
                            held.ColumnText(0, ChangeLogWindow::kColEvent)) +
                  " row1=" + WideToUtf8(
                            held.ColumnText(1, ChangeLogWindow::kColEvent)));

        // The CSV export sees exactly what the list shows - it is built from the
        // same buffer, and a mask that filtered only the RENDERER would leave a
        // Save that disagrees with the screen.
        Check(r, "clog.mask-agrees-with-the-csv",
              onlyAppear.ToCsv().find("APPEAR") != std::string::npos &&
                  onlyAppear.ToCsv().find("DISAPPEAR") == std::string::npos,
              onlyAppear.ToCsv());
        Check(r, "clog.narrowed-mask-is-reported",
              !onlyAppear.EventMaskIsFull() &&
                  noAppear.EventAppear() == false &&
                  noAppear.EventDisappear() == true);
    }

    // 21. Alerting (4.5). The property that matters is the LATCH: a
    //     condition that persists must not re-notify, or the feature is noise
    //     and the user turns it off.
    {
        auto mk = [](double bps, bool listening) {
            Connection c;
            c.protocol = IPPROTO_TCP;
            c.bpsKnown = true;
            c.rxBps = bps;
            c.txBps = 0.0;
            c.state = listening ? MIB_TCP_STATE_LISTEN : MIB_TCP_STATE_ESTAB;
            return c;
        };
        AlertSettings s;
        s.enabled = true;
        s.bpsWarn = 1000.0;
        s.bpsCritical = 10000.0;
        // These assertions are about the throughput latch specifically. A
        // fixture with a live connection also trips the new-connection
        // latch, so leaving those on would make "n == 1" count the wrong
        // alert. They get their own case below.
        s.alertOnNewListener = false;
        s.alertOnNewConnection = false;

        AlertEngine e;
        // Disabled by default: nothing may fire, ever.
        AlertSettings off = s;
        off.enabled = false;
        std::vector<Connection> hot;
        hot.push_back(mk(50000.0, false));
        Check(r, "alert.muted-by-default", e.Evaluate(hot, off).empty());
        Check(r, "alert.default-settings-off",
              !AlertSettings().enabled);

        // Rising edge fires once...
        const std::vector<Alert> first = e.Evaluate(hot, s);
        Check(r, "alert.rising-edge-fires", first.size() == 1,
              "n=" + std::to_string(first.size()));
        // ...and the steady state does not.
        const std::vector<Alert> second = e.Evaluate(hot, s);
        Check(r, "alert.no-repeat-while-held", second.empty(),
              "n=" + std::to_string(second.size()));
        Check(r, "alert.suppressed-counted", e.SuppressedCount() > 0);

        // Clearing then re-crossing must fire again: the latch releases.
        std::vector<Connection> cool;
        cool.push_back(mk(10.0, false));
        e.Evaluate(cool, s);
        const std::vector<Alert> third = e.Evaluate(hot, s);
        Check(r, "alert.refires-after-clearing", third.size() == 1,
              "n=" + std::to_string(third.size()));

        // Warn and critical must not both report the same event.
        AlertEngine e2;
        AlertSettings both = s;
        both.bpsWarn = 1000.0;
        both.bpsCritical = 1000.0;
        const std::vector<Alert> bothOut = e2.Evaluate(hot, both);
        Check(r, "alert.critical-suppresses-warn", bothOut.size() == 1 &&
                                                      bothOut[0].kind ==
                                                          AlertKind::kBpsCritical,
              "n=" + std::to_string(bothOut.size()));

        // An unknown rate must not be treated as "below threshold".
        AlertEngine e3;
        std::vector<Connection> unknown;
        Connection c;
        c.bpsKnown = false;
        c.protocol = IPPROTO_TCP;
        c.state = MIB_TCP_STATE_ESTAB;
        unknown.push_back(c);
        Check(r, "alert.unknown-rate-ignored", e3.Evaluate(unknown, s).empty());

        // The new-connection latch, tested on its own now that the
        // throughput cases have it disabled.
        {
            AlertEngine e4;
            AlertSettings c2 = s;
            c2.alertOnNewListener = true;
            c2.alertOnNewConnection = true;
            std::vector<Connection> conns;
            conns.push_back(mk(0.0, false));
            const std::vector<Alert> a1 = e4.Evaluate(conns, c2);
            const bool sawConn = std::any_of(
                a1.begin(), a1.end(),
                [](const Alert& a) { return a.kind == AlertKind::kNewConnection; });
            Check(r, "alert.new-connection-fires", sawConn,
                  "n=" + std::to_string(a1.size()));
            // Steady state: must not fire again.
            Check(r, "alert.new-connection-no-repeat",
                  e4.Evaluate(conns, c2).empty());
        }

        // Rate formatting.
        Check(r, "alert.format-bps",
              FormatBps(512.0) == L"512 B/s" && FormatBps(2048.0) == L"2 KB/s" &&
                  FormatBps(0.0) == L"0 B/s",
              WideToUtf8(FormatBps(2048.0)));
    }

    // 19. TlsDecode. The ClientHello below is a REAL captured
    //     one, not a hand-rolled synthetic - a synthetic hello only tests the
    //     synthetic case. The full byte sequence was captured with pktmon on
    //     a live Chrome connection; see todo.md 3.0.
    {
        // A REAL ClientHello, captured with pktmon from a live curl request
        // to https://www.cloudflare.com/ and then extracted byte-for-byte
        // by the parser under test. Not hand-written: a synthetic hello
        // would only exercise the synthetic case. Verifiable expectations,
        // all confirmed against this exact capture:
        //   SNI        www.cloudflare.com
        //   ALPN       http/1.1   (curl's offer list, not h2)
        //   legacy ver TLS 1.2 (0x0303), record 465 bytes
static const unsigned char kClientHello[] = {
    0x16, 0x03, 0x01, 0x01, 0xcc, 0x01, 0x00, 0x01, 0xc8, 0x03,
    0x03, 0x5c, 0x05, 0x98, 0x5a, 0xf0, 0x92, 0x82, 0xa0, 0x5a,
    0xba, 0xd8, 0xc0, 0x7a, 0x46, 0xe4, 0xb1, 0x33, 0x8e, 0x25,
    0x15, 0xa7, 0x01, 0x33, 0xba, 0x87, 0x66, 0xda, 0x14, 0xd7,
    0x0d, 0x77, 0xc0, 0x20, 0xf7, 0x10, 0x06, 0xc8, 0x98, 0xcf,
    0x5b, 0x6a, 0xba, 0x65, 0xd9, 0x6f, 0x4c, 0xf2, 0xff, 0x75,
    0x5b, 0x27, 0x58, 0xd9, 0xa0, 0x90, 0xfd, 0xdd, 0xe6, 0x48,
    0x9e, 0x9e, 0xd6, 0x8d, 0x6e, 0xd6, 0x00, 0x28, 0x13, 0x02,
    0x13, 0x01, 0xc0, 0x2c, 0xc0, 0x2b, 0xc0, 0x30, 0xc0, 0x2f,
    0xc0, 0x24, 0xc0, 0x23, 0xc0, 0x28, 0xc0, 0x27, 0xc0, 0x0a,
    0xc0, 0x09, 0xc0, 0x14, 0xc0, 0x13, 0x00, 0x9d, 0x00, 0x9c,
    0x00, 0x3d, 0x00, 0x3c, 0x00, 0x35, 0x00, 0x2f, 0x01, 0x00,
    0x01, 0x57, 0x00, 0x00, 0x00, 0x17, 0x00, 0x15, 0x00, 0x00,
    0x12, 0x77, 0x77, 0x77, 0x2e, 0x63, 0x6c, 0x6f, 0x75, 0x64,
    0x66, 0x6c, 0x61, 0x72, 0x65, 0x2e, 0x63, 0x6f, 0x6d, 0x00,
    0x05, 0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2b,
    0x00, 0x05, 0x04, 0x03, 0x04, 0x03, 0x03, 0x00, 0x0d, 0x00,
    0x1a, 0x00, 0x18, 0x08, 0x04, 0x08, 0x05, 0x08, 0x06, 0x04,
    0x01, 0x05, 0x01, 0x02, 0x01, 0x04, 0x03, 0x05, 0x03, 0x02,
    0x03, 0x02, 0x02, 0x06, 0x01, 0x06, 0x03, 0x00, 0x23, 0x00,
    0x00, 0x00, 0x0a, 0x00, 0x08, 0x00, 0x06, 0x00, 0x1d, 0x00,
    0x17, 0x00, 0x18, 0x00, 0x0b, 0x00, 0x02, 0x01, 0x00, 0x00,
    0x10, 0x00, 0x0b, 0x00, 0x09, 0x08, 0x68, 0x74, 0x74, 0x70,
    0x2f, 0x31, 0x2e, 0x31, 0x00, 0x33, 0x00, 0xd0, 0x00, 0xce,
    0x00, 0x1d, 0x00, 0x20, 0x60, 0xb6, 0x06, 0x52, 0x93, 0xbd,
    0x3f, 0x03, 0x53, 0xe1, 0xb9, 0xbd, 0x7b, 0xfd, 0xaa, 0xb6,
    0x94, 0x9d, 0xd7, 0x7c, 0x66, 0xed, 0xb7, 0x97, 0xf8, 0xc1,
    0x94, 0x70, 0x10, 0xcb, 0xb6, 0x5e, 0x00, 0x17, 0x00, 0x41,
    0x04, 0x50, 0x7c, 0x9e, 0xbb, 0x82, 0x2b, 0x42, 0x42, 0x07,
    0x73, 0x06, 0xaf, 0xfe, 0x11, 0xfb, 0x78, 0x2a, 0x7f, 0x90,
    0xc7, 0x12, 0x12, 0x8e, 0xb6, 0x8f, 0xcf, 0x2f, 0xda, 0xf3,
    0x7a, 0x24, 0x79, 0xd5, 0x8d, 0xe3, 0x21, 0x2b, 0x89, 0x4f,
    0xe2, 0xf6, 0xa8, 0x4f, 0x7f, 0xb7, 0x07, 0x9a, 0x98, 0x95,
    0xd7, 0x4c, 0x1b, 0x1d, 0x77, 0x6d, 0x0d, 0xb7, 0x6a, 0x0b,
    0x2b, 0x82, 0xf6, 0x8d, 0x97, 0x00, 0x18, 0x00, 0x61, 0x04,
    0x76, 0x24, 0x0d, 0x04, 0x11, 0xb3, 0x37, 0x01, 0x49, 0x0a,
    0xdc, 0xb7, 0xf8, 0xe1, 0x06, 0x38, 0xa2, 0x6d, 0xff, 0x13,
    0x1e, 0x4b, 0x27, 0x5e, 0xc0, 0x14, 0xae, 0x1f, 0xf3, 0xbd,
    0x8b, 0x37, 0x1a, 0x87, 0x19, 0x6f, 0xee, 0x51, 0x00, 0xc5,
    0xc2, 0x74, 0x2e, 0xba, 0xe5, 0x2a, 0x39, 0x2e, 0xe8, 0xf8,
    0x15, 0xef, 0x43, 0x0f, 0x95, 0xb0, 0x8e, 0x38, 0xbd, 0x98,
    0xcd, 0xdf, 0x38, 0x24, 0xe8, 0x9d, 0xae, 0xc1, 0x1a, 0x1d,
    0xa0, 0x4e, 0xd4, 0x87, 0x6a, 0x36, 0xa7, 0x19, 0xb1, 0x6a,
    0x2c, 0x07, 0x6c, 0xbe, 0x48, 0x40, 0x99, 0x9d, 0x64, 0xb7,
    0x44, 0x5b, 0x73, 0x08, 0x4c, 0x7d, 0x00, 0x31, 0x00, 0x00,
    0x00, 0x17, 0x00, 0x00, 0xff, 0x01, 0x00, 0x01, 0x00, 0x00,
    0x2d, 0x00, 0x02, 0x01, 0x01,
};
        std::string ch(reinterpret_cast<const char*>(kClientHello),
                       sizeof(kClientHello));

        // The record layer must parse as exactly one handshake record, and a
        // non-TLS blob must not be mistaken for one.
        const std::vector<TlsRecord> recs = ParseTlsRecords(ch);
        Check(r, "tls.record-parses",
              recs.size() == 1 && recs[0].type == kTlsHandshake &&
                  !recs[0].truncated,
              "n=" + std::to_string(recs.size()));
        Check(r, "tls.non-tls-rejected", !LooksLikeTls("GET / HTTP/1.1\r\n"));
        Check(r, "tls.looks-like-tls", LooksLikeTls(ch));

        // The real test: the SNI must come out of the real extension bytes.
        const TlsHandshake hs = ParseTlsHandshake(ch);
        Check(r, "tls.sni-extracted", hs.sawClientHello &&
                                            hs.sni == "www.cloudflare.com",
              "sni='" + hs.sni + "'");
        Check(r, "tls.client-version", hs.clientVersion == 0x0303,
              "v=" + std::to_string(hs.clientVersion));
        Check(r, "tls.alpn-extracted", hs.sniProto == "http/1.1",
              "alpn='" + hs.sniProto + "'");

        // A record whose length exceeds the buffer must be reported as
        // truncated, not read past the end - the normal end-of-capture case.
        {
            std::string over("\x16\x03\x03\xff\xff", 5);
            over.append("XYZ");
            const std::vector<TlsRecord> rs = ParseTlsRecords(over);
            Check(r, "tls.truncated-record-safe",
                  rs.size() == 1 && rs[0].truncated && rs[0].length == 3,
                  "n=" + std::to_string(rs.size()));
        }

        // Names resolve for known values and are empty (not guessed) for
        // unknown ones.
        Check(r, "tls.version-name",
              TlsVersionName(0x0303) == "TLS 1.2" &&
                  TlsVersionName(0x0304) == "TLS 1.3");
        Check(r, "tls.unknown-name-empty",
              TlsVersionName(0x0999).empty() &&
                  TlsCipherSuiteName(0xBEEF).empty());
        Check(r, "tls.cipher-name",
              TlsCipherSuiteName(0x1301) == "TLS_AES_128_GCM_SHA256");

        // A stream of pure application data is recognised as encrypted even
        // with no handshake, which is what a mid-capture start looks like.
        {
            std::string app("\x17\x03\x03\x00\x10", 5);
            app.append(16, '\xAB');
            const TlsHandshake h = ParseTlsHandshake(app);
            Check(r, "tls.appdata-detected",
                  h.sawApplicationData && h.encryptedBytes == 16,
                  "enc=" + std::to_string(h.encryptedBytes));
        }

        // Truncated handshake data must not yield a certificate.
        {
            std::string mixed("\x16\x03\x03\x00\x04\x02\x00\x00\x00", 9);
            const TlsHandshake h = ParseTlsHandshake(mixed);
            Check(r, "tls.truncated-handshake-safe", !h.sawCertificate);
        }

        // W1.2: a TLS 1.3 ServerHello's supported_versions extension carries
        // the real negotiated version as a single 2-byte field with NO length
        // prefix. The previous parser read a 1-byte length first and corrupted
        // the version. Build that exact message and assert the version.
        {
            auto push16 = [](std::string& s, uint16_t v) {
                s.push_back(static_cast<char>((v >> 8) & 0xFF));
                s.push_back(static_cast<char>(v & 0xFF));
            };

            // ServerHello body
            std::string body;
            push16(body, 0x0303);                  // legacy_version
            body.append(32, '\0');                 // random
            body.push_back(static_cast<char>(0));  // session_id_len = 0
            push16(body, 0x1301);                  // cipher_suite
            body.push_back(static_cast<char>(0));  // compression

            // one supported_versions extension
            std::string ext;
            push16(ext, 0x002B);                   // type
            push16(ext, 2);                        // value length
            push16(ext, 0x0304);                   // selected_version
            push16(body, static_cast<uint16_t>(ext.size()));  // extensions len
            body.append(ext);

            // Handshake header: type=2 (server_hello), 3-byte length
            std::string hsh;
            hsh.push_back(static_cast<char>(2));
            const uint32_t hlen = static_cast<uint32_t>(body.size());
            hsh.push_back(static_cast<char>((hlen >> 16) & 0xFF));
            hsh.push_back(static_cast<char>((hlen >> 8) & 0xFF));
            hsh.push_back(static_cast<char>(hlen & 0xFF));
            hsh.append(body);

            // Record layer: type=22, version=0x0303, 2-byte length
            std::string sh;
            sh.push_back(static_cast<char>(22));
            push16(sh, 0x0303);
            push16(sh, static_cast<uint16_t>(hsh.size()));
            sh.append(hsh);

            const TlsHandshake h = ParseTlsHandshake(sh);
            Check(r, "tls.serverhello-tls13-negotiated",
                  h.sawServerHello && h.serverVersion == 0x0304,
                  "ver=" + std::to_string(h.serverVersion) + " want 772 (0x0304)");
        }
    }
    //     (not capture order), deduplication (pktmon reports each frame at
    //     several layers) and honest gap accounting.
    {
        // Build packets for one connection 10.0.0.1:1000 -> 10.0.0.2:2000.
        // 'store' is the payload arena: ParsedPacket holds pointers into the
        // buffer it was parsed from, so the bytes must outlive the vector
        // rather than living in a temporary std::string. It is a plain local
        // (not static) so the lambda can capture it, and it is not resized
        // afterwards, so the pointers stay valid.
        std::string store = "ABCDREQ" "RESPONSE" "HELLO" "MINE" "OTHER" "WWXX";
        auto mk = [&store](uint32_t seq, size_t off, size_t len, uint8_t flags,
                           bool fromClient) {
            ParsedPacket p;
            p.ipVersion = 4;
            p.tcpFlags = flags;
            p.seq = seq;
            p.payloadLen = len;
            p.payload = reinterpret_cast<const unsigned char*>(store.data() + off);
            if (fromClient) {
                p.srcPort = 1000; p.dstPort = 2000;
                p.srcIp16[0] = 10; p.srcIp16[3] = 1;
                p.dstIp16[0] = 10; p.dstIp16[3] = 2;
            } else {
                p.srcPort = 2000; p.dstPort = 1000;
                p.srcIp16[0] = 10; p.srcIp16[3] = 2;
                p.dstIp16[0] = 10; p.dstIp16[3] = 1;
            }
            return p;
        };
        const TcpKey want = MakeTcpKey(mk(0, 0, 0, 0x02, true));
        // Offsets into "store".
        constexpr size_t kAB = 0, kREQ = 4, kRESP = 7, kHELLO = 15;
        constexpr size_t kMINE = 20, kOTHER = 24, kWW = 29;

        // Out of order: the "CD" segment is captured before "AB". The second
        // segment's sequence number must be the first's + 2 - a test that
        // reuses seq 1000 for both is not testing ordering at all, it is
        // testing overlap, and it passes for the wrong reason.
        std::vector<ParsedPacket> v;
        v.push_back(mk(1002, kAB + 2, 2, 0x18, true));   // "CD" first
        v.push_back(mk(1000, kAB, 2, 0x18, true));       // then "AB"
        ReasmResult cs, sc;
        ReassembleStream(v, want, &cs, &sc);
        Check(r, "reasm.out-of-order-sorted", cs.bytes == "ABCD" &&
                                                !cs.hasGap,
              "got=" + cs.bytes);

        // The same segment twice (pktmon layer duplicate / retransmit)
        // must not appear twice in the output.
        v.clear();
        v.push_back(mk(1000, kAB, 4, 0x18, true));
        v.push_back(mk(1000, kAB, 4, 0x18, true));
        ReassembleStream(v, want, &cs, &sc);
        Check(r, "reasm.duplicate-deduped", cs.bytes == "ABCD" &&
                                                cs.duplicates >= 1,
              "len=" + std::to_string(cs.bytes.size()) +
                  " dups=" + std::to_string(cs.duplicates));

        // A real gap must be reported, not silently spliced.
        v.clear();
        v.push_back(mk(1000, kAB, 2, 0x18, true));        // 1000..1001
        v.push_back(mk(1010, kAB + 2, 2, 0x18, true));    // 1010..1011
        ReassembleStream(v, want, &cs, &sc);
        Check(r, "reasm.gap-reported", cs.hasGap && cs.bytesMissing == 8,
              "missing=" + std::to_string(cs.bytesMissing));

        // Bidirectional separation, with the reverse direction carrying a
        // payload of a different length.
        v.clear();
        v.push_back(mk(1000, kREQ, 3, 0x18, true));
        v.push_back(mk(2000, kRESP, 8, 0x18, false));
        ReassembleStream(v, want, &cs, &sc);
        Check(r, "reasm.directions-separated",
              cs.bytes == "REQ" && sc.bytes == "RESPONSE",
              "cs=" + cs.bytes + " sc=" + sc.bytes);

        // Sequence-number wrap: the first segment ends at 0xFFFFFFFF and the
        // second begins at 0, so a naive unsigned compare puts the
        // post-wrap segment FIRST. The two are adjacent, so there is no gap
        // and the result must be exactly "WWXX" with none missing. Getting
        // this right is what Unwrap() exists for.
        v.clear();
        v.push_back(mk(0xFFFFFFFEu, kWW, 2, 0x18, true));   // seq .. 0xFFFFFFFF
        v.push_back(mk(0x00000000u, kWW + 2, 2, 0x18, true)); // seq 0 .. 1
        ReassembleStream(v, want, &cs, &sc);
        Check(r, "reasm.sequence-wrap-ordered",
              cs.bytes == "WWXX" && cs.bytesMissing == 0 && !cs.hasGap,
              "got='" + cs.bytes + "' len=" + std::to_string(cs.bytes.size()) +
                  " missing=" + std::to_string(cs.bytesMissing));

        // A SYN sets the data base one past its sequence number, so a
        // segment following it at seq+1 is the first data byte.
        v.clear();
        v.push_back(mk(500, 0, 0, 0x02, true));           // SYN
        v.push_back(mk(501, kHELLO, 5, 0x18, true));       // first data
        ReassembleStream(v, want, &cs, &sc);
        Check(r, "reasm.syn-sets-base", cs.bytes == "HELLO" && cs.sawSyn,
              "got=" + cs.bytes);

        // Unrelated traffic must be counted, never mixed in.
        v.clear();
        v.push_back(mk(1000, kMINE, 4, 0x18, true));
        ParsedPacket other = mk(1000, kOTHER, 5, 0x18, false);
        other.srcPort = 9999;                              // different conn
        v.push_back(other);
        ReasmStats st = ReassembleStream(v, want, &cs, &sc);
        Check(r, "reasm.other-stream-excluded",
              cs.bytes == "MINE" && st.otherStreams == 1,
              "cs=" + cs.bytes + " other=" + std::to_string(st.otherStreams));
    }
    //     pktmon capture, and the failure modes here are the ones that bit
    //     during that work - so each is pinned by a test.
    {
        // A hand-built Ethernet + IPv4 + TCP frame, 3 bytes of payload.
        // Offsets below are absolute into the frame, and each is annotated
        // because getting them wrong is the whole bug this test guards.
        unsigned char frame[128] = {0};
        // dst MAC (first byte deliberately 0x6c: a high nibble of 6 reads as
        // "IPv6 version 6" to a naive version probe - the exact mistake that
        // made 16488 good packets look malformed during development).
        frame[0] = 0x6c; frame[1] = 0x55; frame[5] = 0x08;
        // src MAC: last byte 0x02, so src = 192.0.2.1
        frame[6] = 0xc0; frame[7] = 0x00; frame[8] = 0x02; frame[9] = 0x01;
        frame[12] = 0x08; frame[13] = 0x00;          // ethertype IPv4
        // IPv4 starts at 14. Relative: +0 ver/ihl, +9 proto, +12 src, +16 dst.
        frame[14] = 0x45;                            // version 4, IHL 5
        // Total length INCLUDES the IP header: 20 (IP) + 20 (TCP) + 3
        // (payload) = 43 = 0x2B. The parser trusts this to trim padding AND
        // to bounds-check the TCP header, so understating it makes the
        // packet look truncated.
        frame[16] = 0x00; frame[17] = 0x2B;
        frame[23] = 6;                               // protocol TCP
        frame[26] = 0xc0; frame[27] = 0x00;          // src 192.0.2.1
        frame[30] = 0xc0; frame[31] = 0x02;          // dst 192.0.2.2
        // TCP starts at 34. Relative: +0 sport, +2 dport, +12 offset/flags.
        frame[34] = 0x1f; frame[35] = 0x90;          // src port 8080
        frame[36] = 0x01; frame[37] = 0xbb;          // dst port 443
        frame[46] = 0x50;                            // data offset 5
        frame[47] = 0x18;                            // PSH|ACK
        // Payload starts at 34 + 20 = 54. Total frame = 14 + 43 = 57.
        frame[54] = 'A'; frame[55] = 'B'; frame[56] = 'C';

        ParsedPacket p;
        const bool ok = ParseIpTcp(frame, 57, 1, &p);
        Check(r, "pcapng.ethernet-tcp-parses", ok && p.payloadLen == 3 &&
                                               p.srcPort == 8080 &&
                                               p.dstPort == 443 &&
                                               p.ipVersion == 4,
              std::string("ok=") + (ok ? "1" : "0") +
                  " sport=" + std::to_string(p.srcPort) +
                  " dport=" + std::to_string(p.dstPort) +
                  " ver=" + std::to_string(p.ipVersion) +
                  " len=" + std::to_string(p.payloadLen) +
                  " malformed=" + (p.malformed ? "1" : "0") +
                  " ihl=" + std::to_string(frame[14] & 0x0F) +
                  " totlen=" + std::to_string((frame[16] << 8) | frame[17]));
        Check(r, "pcapng.payload-offset-correct",
              ok && p.payload != nullptr && p.payload[0] == 'A' &&
                  p.payload[2] == 'C' && !p.malformed);

        // Auto-detect must land on the same answer as the explicit link
        // type.
        ParsedPacket q;
        Check(r, "pcapng.autodetect-matches-ethernet",
              ParseIpTcp(frame, 57, kLinkAuto, &q) && q.payloadLen == 3 &&
                  q.srcPort == 8080);

        // A frame whose payload runs past the captured bytes must be
        // rejected, not read out of bounds.
        Check(r, "pcapng.truncated-rejected",
              !ParseIpTcp(frame, 20, 1, &p) || p.malformed);

        // A UDP frame is not TCP: skipped, not mis-parsed.
        frame[23] = 17;
        Check(r, "pcapng.udp-not-tcp", !ParseIpTcp(frame, 57, 1, &p));
        frame[23] = 6;

        // Container-level guards. A truncated file must fail cleanly rather
        // than walk off the end.
        PcapngParse empty = ParsePcapng(nullptr, 0);
        Check(r, "pcapng.empty-rejected", !empty.ok && !empty.error.empty());

        const unsigned char junk[16] = {1, 2, 3, 4, 5, 6, 7, 8,
                                        9, 10, 11, 12, 13, 14, 15, 16};
        PcapngParse noShb = ParsePcapng(junk, sizeof(junk));
        Check(r, "pcapng.no-shb-rejected", !noShb.ok);
    }
    //     button must agree with what the window draws, must survive an
    //     empty model, and must not claim to list more rows than it has.
    {
        DetailModel empty;
        const std::wstring emptyText = empty.ToPlainText();
        Check(r, "details.empty-model-renders-empty", emptyText.empty());

        DetailModel m;
        m.title = L"chrome.exe";
        m.subtitle = L"PID 42   ·   10.0.0.1:5000  →  93.184.216.34:443";
        DetailSection sec;
        sec.title = L"Process";
        sec.fields.push_back(DetailField{L"Name", L"chrome.exe", false});
        sec.fields.push_back(DetailField{L"PID", L"42", true});
        // An empty value must have become the em-dash, not a blank gap.
        sec.fields.push_back(DetailField{L"Path", L"", false});
        m.sections.push_back(sec);

        DetailSection net;
        net.title = L"Live stats";
        net.note = L"enable the counters";
        m.sections.push_back(net);

        m.connectionLines = {L"TCPv4  10.0.0.1:5000  →  x:443  (ESTABLISHED)"};
        m.connectionTotal = 3;   // deliberately more than we listed

        const std::wstring t = m.ToPlainText();
        const auto has = [&t](const wchar_t* s) {
            return t.find(s) != std::wstring::npos;
        };
        // Every section title, every label, the em-dash substitution, the
        // note, the connection block, and the honest "and N more".
        const bool all = has(L"chrome.exe") && has(L"Process") &&
                         has(L"Name") && has(L"42") && has(L"—") &&
                         has(L"Live stats") && has(L"enable the counters") &&
                         has(L"Connections (3)") &&
                         has(L"ESTABLISHED") && has(L"and 2 more");
        Check(r, "details.plaintext-complete", all, WideToUtf8(t));

        // The copy text must not claim rows it did not include, and must
        // include every row it did.
        Check(r, "details.overflow-honest",
              t.find(L"and 2 more") != std::wstring::npos &&
                  m.connectionLines.size() == 1);
    }

    // The per-connection actions must take their addresses from the BINARY
    // fields, not the printable endpoint strings. Those strings carry the port
    // ("93.184.216.34:443"), and InetPtonW rejects them, so "Follow TCP
    // stream" reported "Could not read the addresses of this connection" for
    // EVERY row and never reached the capture. A selftest that only checks
    // the pure grouping logic could not see that; these check the join.
    {
        Connection v4;
        v4.family = AF_INET;
        v4.protocol = IPPROTO_TCP;
        v4.state = MIB_TCP_STATE_ESTAB;
        v4.localPort = 52341;
        v4.remotePort = 443;
        v4.local4.S_un.S_addr = ::htonl(0x0A000005);      // 10.0.0.5
        v4.remote4.S_un.S_addr = ::htonl(0x5DB8D822);    // 93.184.216.34
        v4.localAddress = L"10.0.0.5";
        v4.remoteAddress = L"93.184.216.34";
        v4.localEndpoint = JoinEndpoint(v4.localAddress, v4.localPort, false);
        v4.remoteEndpoint = JoinEndpoint(v4.remoteAddress, v4.remotePort, false);

        // The endpoint string is NOT a parseable address - this is the exact
        // condition that made the command fail on every row.
        unsigned char viaString[16] = {};
        Check(r, "capture.endpoint-string-is-not-an-address",
              ::InetPtonW(AF_INET, v4.remoteEndpoint.c_str(), viaString) != 1);

        // What the fixed code does instead: copy the binary bytes, and they
        // must equal what InetPtonW produces for the address ALONE.
        unsigned char want[4] = {};
        ::InetPtonW(AF_INET, L"93.184.216.34", want);
        unsigned char got[16] = {};
        ::memcpy(got, &v4.remote4, 4);
        Check(r, "capture.v4-addresses-come-from-binary-fields",
              std::memcmp(want, got, 4) == 0);

        // Same for IPv6, where the printable form is bracketed AND scoped -
        // two separate ways for a string to be unparseable on its own.
        Connection v6;
        v6.family = AF_INET6;
        v6.protocol = IPPROTO_TCP;
        v6.state = MIB_TCP_STATE_ESTAB;
        v6.localPort = 52342;
        v6.remotePort = 443;
        ::InetPtonW(AF_INET6, L"2606:4700::6810:85e5", v6.remote6.s6_addr);
        v6.local6.s6_addr[0] = 0xFE; v6.local6.s6_addr[1] = 0x80;
        v6.localAddress = L"fe80::1%12";
        v6.remoteAddress = L"2606:4700::6810:85e5";
        v6.localEndpoint = JoinEndpoint(v6.localAddress, v6.localPort, true);
        v6.remoteEndpoint = JoinEndpoint(v6.remoteAddress, v6.remotePort, true);

        unsigned char v6want[16] = {};
        ::InetPtonW(AF_INET6, L"2606:4700::6810:85e5", v6want);
        Check(r, "capture.v6-addresses-come-from-binary-fields",
              std::memcmp(v6want, v6.remote6.s6_addr, 16) == 0);

        // The caption must show each port ONCE. Appending the port to a string
        // that already ends in one produced "10.0.0.5:52341:52341".
        const std::wstring label = v4.localEndpoint + L" -> " + v4.remoteEndpoint;
        Check(r, "capture.label-has-no-duplicated-port",
              label.find(L"52341:52341") == std::wstring::npos &&
                  label.find(L"443:443") == std::wstring::npos,
              WideToUtf8(label));
    }

    // 5.1 grouping, as the list actually uses it: the store's grouped view and
    // the per-cell renderer. The pure aggregation is covered further up; what
    // matters here is that grouping is wired to a view at all - the module was
    // fully implemented and never called, which is the failure this guards.
    {
        const auto mk = [](DWORD pid, const wchar_t* name, USHORT port,
                           std::uint64_t rx) {
            Connection c;
            c.pid = pid;
            c.processName = name;
            c.localAddress = L"10.0.0.1";
            c.localPort = port;
            c.remoteAddress = L"93.184.216.34";
            c.remotePort = 443;
            c.state = MIB_TCP_STATE_ESTAB;
            c.protocol = IPPROTO_TCP;
            c.family = AF_INET;
            c.trafficRx = rx;
            c.trafficTx = 0;
            return c;
        };
        ConnectionStore st;
        // Three chrome connections and one curl, so grouping must collapse
        // four rows into two.
        std::vector<Connection> rows;
        rows.push_back(mk(100, L"chrome.exe", 5001, 1000));
        rows.push_back(mk(200, L"curl.exe", 5002, 200));
        rows.push_back(mk(100, L"chrome.exe", 5003, 1000));   // same PID
        rows.push_back(mk(100, L"chrome.exe", 5004, 1000));
        st.ReplaceSnapshot(std::move(rows));

        ViewQuery all;
        st.SetView(all);
        const size_t flat = st.View().size();
        st.SetGrouped(true);
        const size_t grouped = st.View().size();
        Check(r, "group.view-collapses", flat == 4 && grouped == 2,
              "flat " + std::to_string(flat) + " grouped " +
                  std::to_string(grouped));

        // The group's traffic is the MAX over its rows, not the sum: all
        // three chrome rows carry the same per-PID total, so a sum would
        // report 3000 for a process that moved 1000.
        wchar_t cell[128] = {0};
        const ProcessGroup* g = st.ViewGroup(0);
        bool ok = (g != nullptr && g->rowCount == 3);
        if (ok) {
            GroupColumnText(*g, COL_RX, cell, 128);
            ok = (std::wstring(cell) == L"1000 B");
        }
        Check(r, "group.traffic-is-max-not-sum", ok,
              WideToUtf8(std::wstring(cell)));

        // A group cell that the renderer does not own must fall through, so
        // the caller renders the real connection.
        wchar_t other[128] = {0};
        const bool fallsThrough =
            (g != nullptr) && !GroupColumnText(*g, COL_REMOTE, other, 128);
        Check(r, "group.unknown-column-falls-through", fallsThrough);

        // D27. The fall-through above is correct for a TABLE and for the GUI -
        // the row is visibly a group, so one member's remote address is
        // informative - and wrong for an EXPORT, where the header row is a
        // schema claim. These pin the split: per-connection columns are
        // flagged, group-answerable ones are not, and the per-PROCESS live
        // stats are explicitly NOT flagged (they fall through to the
        // representative but carry the same value on every member, so the
        // group's answer is already correct).
        Check(r, "d27.per-connection-flagged",
              ColumnIsPerConnectionOnly(COL_REMOTE) &&
                  ColumnIsPerConnectionOnly(COL_LOCAL) &&
                  ColumnIsPerConnectionOnly(COL_LPORT) &&
                  ColumnIsPerConnectionOnly(COL_RPORT) &&
                  ColumnIsPerConnectionOnly(COL_HOST) &&
                  ColumnIsPerConnectionOnly(COL_TLS) &&
                  ColumnIsPerConnectionOnly(COL_COUNTRY) &&
                  ColumnIsPerConnectionOnly(COL_DURATION));
        Check(r, "d27.group-answerable-not-flagged",
              !ColumnIsPerConnectionOnly(COL_PID) &&
                  !ColumnIsPerConnectionOnly(COL_PROCESS) &&
                  !ColumnIsPerConnectionOnly(COL_PROTO) &&
                  !ColumnIsPerConnectionOnly(COL_STATE) &&
                  !ColumnIsPerConnectionOnly(COL_TRAFFIC) &&
                  !ColumnIsPerConnectionOnly(COL_RX) &&
                  !ColumnIsPerConnectionOnly(COL_TX) &&
                  !ColumnIsPerConnectionOnly(COL_NETTOTAL) &&
                  !ColumnIsPerConnectionOnly(COL_BANDWIDTH) &&
                  !ColumnIsPerConnectionOnly(COL_SERVICE) &&
                  !ColumnIsPerConnectionOnly(COL_PATH) &&
                  !ColumnIsPerConnectionOnly(COL_PINNED));
        // cpu/mem/disk are per-PROCESS, joined onto every row carrying the PID,
        // so a group row prints the same number whichever member it falls
        // through to. Flagging them would refuse a correct column.
        Check(r, "d27.proc-stats-not-flagged",
              !ColumnIsPerConnectionOnly(COL_CPU) &&
                  !ColumnIsPerConnectionOnly(COL_MEM) &&
                  !ColumnIsPerConnectionOnly(COL_DISK));
        // Every flagged column must genuinely have no group answer, i.e.
        // GroupColumnText must NOT handle it. If a future change teaches it
        // one, this fails and the flag should be revisited in the same edit.
        Check(r, "d27.flagged-are-unhandled",
              !GroupColumnText(*g, COL_REMOTE, other, 128) &&
                  !GroupColumnText(*g, COL_HOST, other, 128) &&
                  !GroupColumnText(*g, COL_DURATION, other, 128) &&
                  !GroupColumnText(*g, COL_COUNTRY, other, 128));

        // Turning grouping off must restore the flat view EXACTLY - the rows
        // were never consumed to build the groups.
        st.SetGrouped(false);
        const size_t back = st.View().size();
        Check(r, "group.toggles-back-exactly", back == flat && back == 4,
              "flat was " + std::to_string(flat) + ", back to " +
                  std::to_string(back));

        // An unknown process still gets a usable header rather than a blank.
        ConnectionStore st2;
        std::vector<Connection> anon;
        anon.push_back(mk(77, L"", 6000, 0));
        st2.ReplaceSnapshot(std::move(anon));
        ViewQuery q2;
        st2.SetView(q2);
        st2.SetGrouped(true);
        const ProcessGroup* g2 = st2.ViewGroup(0);
        wchar_t name[128] = {0};
        const bool named = (g2 != nullptr) &&
                           GroupColumnText(*g2, COL_PROCESS, name, 128) &&
                           std::wstring(name) == L"PID 77";
        Check(r, "group.unknown-pid-named", named,
              WideToUtf8(std::wstring(name)));
    }

    // 23. Abstract command layer (Commands.h): headless row targeting, the
    //     shared details builder, preset-view helpers, block mapping and
    //     system-stats formatting. Synthetic rows only; no registry, no
    //     network, no window.
    {
        ConnectionStore st;
        std::vector<Connection> seed;
        for (unsigned i = 0; i < 4; ++i) seed.push_back(MakeRow(i));
        st.ReplaceSnapshot(std::move(seed));
        ViewQuery q;
        st.SetView(q);

        const std::vector<size_t> byPid =
            st.SelectByFilter(L"pid:" + std::to_wstring(st.Rows()[0].pid));
        Check(r, "cmd.select-by-filter-finds-row", !byPid.empty());
        Check(r, "cmd.select-empty-matches-nothing",
              st.SelectByFilter(std::wstring()).empty());
        Check(r, "cmd.select-nonsense-matches-nothing",
              st.SelectByFilter(L"process:definitely-not-a-process-xyz")
                  .empty());
        const std::vector<size_t> inView =
            st.SelectByFilterInView(L"pid:" + std::to_wstring(st.Rows()[0].pid));
        Check(r, "cmd.select-in-view-agrees", inView == byPid);
        const Connection* live = st.RowForViewIndex(0);
        Check(r, "cmd.row-for-view-index", live != nullptr);
        Check(r, "cmd.row-for-view-index-oor",
              st.RowForViewIndex(1 << 20) == nullptr &&
                  st.RowForViewIndex(-1) == nullptr);

        // Shared details builder: same model the GUI window renders.
        const DetailModel dm = BuildDetailModel(st.Rows()[0], st);
        Check(r, "cmd.detail-model-has-title", !dm.title.empty(),
              WideToUtf8(dm.title));
        Check(r, "cmd.detail-model-plain-text",
              !dm.ToPlainText().empty());

        // Preset-view helpers round-trip through the alias type.
        const ViewState pv = CurrentPresetViewFor(
            L"port:443", kDefaultVisibleCols, COL_PID, true,
            kPresetSourceNone);
        Check(r, "cmd.preset-view-keeps-filter", pv.filter == L"port:443");
        const std::string pvj = ViewStateToJson(pv);
        Check(r, "cmd.preset-view-json",
              pvj.find("port:443") != std::string::npos, pvj);

        // Block mapping: established IPv4 TCP maps, UDP/listening refuse.
        BlockRequest req;
        std::wstring why;
        Connection tcp = MakeReferenceTcpRow();
        Check(r, "cmd.block-maps-tcp",
              ConnectionToBlockRequest(tcp, &req, &why) && !req.ipv6 &&
                  req.remotePort == 443);
        Connection udp = tcp;
        udp.protocol = IPPROTO_UDP;
        udp.state = 0;
        Check(r, "cmd.block-refuses-udp",
              !ConnectionToBlockRequest(udp, &req, &why));
        Connection lst = tcp;
        lst.state = MIB_TCP_STATE_LISTEN;
        Check(r, "cmd.block-refuses-listen",
              !ConnectionToBlockRequest(lst, &req, &why));

        // System-stats formatting on synthetic readings (no sampling here).
        SystemStats ss;
        ss.cpuKnown = true;
        ss.cpuPct = 12.5;
        ss.memKnown = true;
        ss.memUsed = 1024;
        ss.memTotal = 4096;
        ss.memPct = 25.0;
        ss.diskKnown = false;
        ss.netKnown = false;
        const std::string sj = SystemStatsToJson(ss);
        Check(r, "cmd.sysstat-json",
              sj.find("\"cpu\":12.5") != std::string::npos, sj);
        const std::string sl = FormatSystemStatsLine(ss);
        Check(r, "cmd.sysstat-line-na",
              sl.find("n/a") != std::string::npos, sl);

        // Column specs: set names and explicit lists resolve headless, so
        // --columns/--format reach JSON as well as the delimited shapes.
        std::vector<int> cols;
        Check(r, "cmd.columns-full-resolves",
              ResolveColumnSpec(L"full", cols) && cols.size() == 16,
              std::to_string(cols.size()));
        Check(r, "cmd.columns-explicit",
              ResolveColumnSpec(L"proto,pid,process", cols) &&
                  cols.size() == 3 && cols[0] == COL_PROTO &&
                  cols[1] == COL_PID && cols[2] == COL_PROCESS);
        Check(r, "cmd.columns-bad-empty",
              !ResolveColumnSpec(L"no-such-column", cols));
        Check(r, "cmd.columns-single-name",
              ColumnIdForName(L"cpu") == COL_CPU &&
                  ColumnIdForName(L"nope") == -1);

        // ps sort keys over a synthetic store: mem desc, conns desc.
        {
            ConnectionStore pst;
            std::vector<Connection> rows;
            for (unsigned i = 0; i < 6; ++i) rows.push_back(MakeRow(i));
            pst.ReplaceSnapshot(std::move(rows));
            PsOptions mo;
            mo.sortBy = "mem";
            const CommandResult mr = RenderPs(pst, mo);
            Check(r, "cmd.ps-sort-mem", mr.exitCode == 0 && !mr.out.empty());
            PsOptions co;
            co.sortBy = "conns";
            co.format = "json";
            const CommandResult cr = RenderPs(pst, co);
            Check(r, "cmd.ps-sort-conns-json",
                  cr.exitCode == 0 &&
                      cr.out.find("\"connections\"") != std::string::npos);
        }
    }

    // 24. Pure list/ps render + change-event formatting (the single-mode
    //     CLI core). Synthetic store only: no snapshot, no network.
    {
        // State clauses accept hyphens and underscores alike: the labels
        // use TIME_WAIT, users type either spelling.
        std::vector<FilterClause> stc;
        ParseFilter(L"state:time-wait", stc);
        Check(r, "cmd.state-hyphen-normalised",
              stc.size() == 1 && stc[0].text == L"time_wait");
        Connection tw;
        tw.family = AF_INET;
        tw.protocol = IPPROTO_TCP;
        tw.state = MIB_TCP_STATE_TIME_WAIT;
        tw.localAddress = L"10.0.0.1";
        tw.remoteAddress = L"10.0.0.2";
        tw.localPort = 5000;
        tw.remotePort = 80;
        tw.pid = 9;
        tw.localEndpoint = JoinEndpoint(tw.localAddress, tw.localPort, false);
        tw.remoteEndpoint =
            JoinEndpoint(tw.remoteAddress, tw.remotePort, false);
        ConnectionStore::FinalizeRow(tw);
        Check(r, "cmd.state-hyphen-matches", MatchFilter(tw, stc));
        ConnectionStore st;
        std::vector<Connection> seed;
        for (unsigned i = 0; i < 4; ++i) seed.push_back(MakeRow(i));
        st.ReplaceSnapshot(std::move(seed));

        ListOptions lo;
        lo.quiet = true;
        const CommandResult lr = RenderList(st, lo);
        Check(r, "cmd.renderlist-quiet-nonempty",
              lr.exitCode == 0 && lr.out.empty());
        lo.filter = L"process:definitely-not-a-process-xyz";
        const CommandResult lr2 = RenderList(st, lo);
        Check(r, "cmd.renderlist-quiet-empty",
              lr2.exitCode == 1 && lr2.out.empty());

        ListOptions lj;
        lj.format = "json";
        lj.limit = 1;
        const CommandResult lr3 = RenderList(st, lj);
        Check(r, "cmd.renderlist-json-key",
              lr3.exitCode == 0 &&
                  lr3.out.find("\"proto\"") != std::string::npos);

        // F5.12: jsonl is this same serializer with `lines` set - the framing
        // differs, the schema does not. Three properties the array form
        // violates and this form must not, each one a line-at-a-time reader
        // depends on: no wrapper at column 0, every line a complete object,
        // and a terminating newline so the next --watch tick can follow.
        ListOptions ljn = lj;
        ljn.jsonLines = true;
        const CommandResult lr4 = RenderList(st, ljn);
        Check(r, "cmd.renderlist-jsonl-first-byte-is-brace",
              lr4.exitCode == 0 && !lr4.out.empty() && lr4.out[0] == '{' &&
                  lr4.out.back() == '\n');
        Check(r, "cmd.renderlist-array-opens-with-bracket",
              lr3.exitCode == 0 && lr3.out.size() >= 2 && lr3.out[0] == '[' &&
                  lr3.out[1] == '\n');
        bool jline = !lr4.out.empty();
        size_t jscan = 0;
        while (jline && jscan < lr4.out.size()) {
            if (lr4.out[jscan] != '{') {
                jline = false;
                break;
            }
            const size_t nl = lr4.out.find('\n', jscan);
            if (nl == std::string::npos) {
                jline = false;
                break;
            }
            jscan = nl + 1;
        }
        Check(r, "cmd.renderlist-jsonl-every-line-an-object", jline, lr4.out);

        // Empty must stay empty in lines mode: a reader that tries to parse a
        // blank line as an object is worse off than one that got nothing. The
        // array form still has to be a valid document, so it keeps "[" + NL +
        // "]" rather than collapsing to nothing too.
        ListOptions lje = ljn;
        lje.filter = L"process:definitely-not-a-process-xyz";
        const CommandResult lr5 = RenderList(st, lje);
        Check(r, "cmd.renderlist-jsonl-empty-writes-nothing", lr5.out.empty(),
              lr5.out);
        ListOptions ljea = lje;
        ljea.jsonLines = false;
        const CommandResult lr6 = RenderList(st, ljea);
        Check(r, "cmd.renderlist-json-empty-still-an-array",
              lr6.out == "[\n]", lr6.out);

        PsOptions po;
        po.quiet = true;
        const CommandResult pr = RenderPs(st, po);
        Check(r, "cmd.renderps-quiet-nonempty",
              pr.exitCode == 0 && pr.out.empty());
        po.filter = L"process:definitely-not-a-process-xyz";
        const CommandResult pr2 = RenderPs(st, po);
        Check(r, "cmd.renderps-quiet-empty",
              pr2.exitCode == 1 && pr2.out.empty());

        // D14/D15: the aligned table. Synthetic rows with known cell
        // widths, so the expected bytes are exact - padding, the numeric
        // right rule, no tabs anywhere in a table, and the header/body
        // split the streaming path relies on (header first via StreamOut,
        // rows after in r.out) concatenating back into this same string.
        {
            ConnectionStore ts;
            std::vector<Connection> trows;
            Connection a;
            a.family = AF_INET;
            a.protocol = IPPROTO_TCP;
            a.state = MIB_TCP_STATE_ESTAB;
            a.pid = 100;
            a.processName = L"ab.exe";
            a.localAddress = L"10.0.0.1";
            a.localPort = 5000;
            a.remoteAddress = L"203.0.113.9";
            a.remotePort = 443;
            a.trafficTx = 1073741824ull;    // "1.00 GB"
            a.trafficRx = 1048576ull;       // "1.0 MB"
            a.localEndpoint =
                JoinEndpoint(a.localAddress, a.localPort, false);
            a.remoteEndpoint =
                JoinEndpoint(a.remoteAddress, a.remotePort, false);
            ConnectionStore::FinalizeRow(a);
            Connection b = a;
            b.pid = 101;
            b.processName = L"code-sidecar.exe";
            b.trafficTx = 5368709120ull;    // "5.00 GB"
            b.trafficRx = 2147483648ull;    // "2.00 GB"
            ConnectionStore::FinalizeRow(b);
            trows.push_back(a);
            trows.push_back(b);
            ts.ReplaceSnapshot(std::move(trows));

            ListOptions ta;
            ta.columns = {COL_PROCESS, COL_TX, COL_RX};
            const CommandResult tr = RenderList(ts, ta);
            const std::string want =
                "Process              Sent  Received\r\n"
                "ab.exe            1.00 GB    1.0 MB\r\n"
                "code-sidecar.exe  5.00 GB   2.00 GB\r\n";
            Check(r, "table.align-exact", tr.out == want, tr.out);
            Check(r, "table.no-tabs", tr.out.find('\t') == std::string::npos,
                  tr.out);
            // The streamed header plus the skipHeader body must equal the
            // buffered rendering byte for byte (D15).
            const size_t cut = tr.out.find("\r\n");
            const CommandResult th =
                RenderList(ts, ta, /*skipHeader=*/true);
            Check(r, "table.skipheader-concat",
                  cut != std::string::npos &&
                      th.out == tr.out.substr(cut + 2),
                  th.out);

            // tsv keeps its machine contract: raw tab delimiters.
            ListOptions tt = ta;
            tt.format = "tsv";
            const CommandResult tv = RenderList(ts, tt);
            Check(r, "table.tsv-raw-tabs",
                  tv.out.find("Process\tSent\tReceived") == 0,
                  tv.out);

            // Grouped: a column the group has no aggregate answer for falls
            // through to the representative connection (the CLI used to
            // print it BLANK), and "N connections" is counted in the width.
            Connection g0;
            g0.family = AF_INET;
            g0.protocol = IPPROTO_TCP;
            g0.state = MIB_TCP_STATE_ESTAB;
            g0.pid = 7;
            g0.processName = L"grp.exe";
            g0.localAddress = L"10.1.2.3";
            g0.localPort = 5050;
            g0.remoteAddress = L"203.0.113.7";
            g0.remotePort = 443;
            g0.localEndpoint =
                JoinEndpoint(g0.localAddress, g0.localPort, false);
            g0.remoteEndpoint =
                JoinEndpoint(g0.remoteAddress, g0.remotePort, false);
            ConnectionStore::FinalizeRow(g0);
            Connection g1 = g0;
            g1.localPort = 5051;
            g1.localEndpoint =
                JoinEndpoint(g1.localAddress, g1.localPort, false);
            ConnectionStore::FinalizeRow(g1);
            ConnectionStore gs;
            std::vector<Connection> grows;
            grows.push_back(g0);
            grows.push_back(g1);
            gs.ReplaceSnapshot(std::move(grows));
            ListOptions lg;
            lg.grouped = true;
            lg.columns = {COL_PROCESS, COL_LOCAL, COL_STATE};
            const CommandResult gt = RenderList(gs, lg);
            const std::string gwant =
                "Process  Local address  State\r\n"
                "grp.exe  10.1.2.3       2 connections\r\n";
            Check(r, "table.group-fallthrough-exact", gt.out == gwant,
                  gt.out);

            // F5.12: the GROUPED shape carries the same two framings, and the
            // D27 refusal has to reach jsonl. jsonl is `json` with a different
            // renderer, so every rule written against `json` - group-safe
            // columns included - applies to it unchanged. Proving it here
            // rather than in the CLI is what makes the claim structural: the
            // renderer, not the argument parser, is what jsonl adds.
            ListOptions lgj;
            lgj.format = "json";
            lgj.jsonLines = true;
            lgj.grouped = true;
            lgj.columns = {COL_PID, COL_PROCESS, COL_STATE};
            const CommandResult gtj = RenderList(gs, lgj);
            bool gtline = gtj.exitCode == 0 && !gtj.out.empty();
            size_t gtscan = 0;
            while (gtline && gtscan < gtj.out.size()) {
                if (gtj.out[gtscan] != '{') {
                    gtline = false;
                    break;
                }
                const size_t gnl = gtj.out.find('\n', gtscan);
                if (gnl == std::string::npos) {
                    gtline = false;
                    break;
                }
                gtscan = gnl + 1;
            }
            Check(r, "cmd.renderlist-group-jsonl-frame", gtline, gtj.out);

            ListOptions lgr = lgj;
            lgr.columns = {COL_PROCESS, COL_LOCAL, COL_STATE};
            const CommandResult gtr = RenderList(gs, lgr);
            Check(r, "cmd.renderlist-group-jsonl-refuses-per-connection",
                  gtr.exitCode == 2 &&
                      gtr.err.find("JSON key would promise") !=
                          std::string::npos,
                  gtr.err);

            // Cap + truncation: with --dns on, Hostname takes its budget
            // width up front (the header can print before the join) and a
            // longer name ends in an ellipsis instead of shifting columns.
            // 70 > the 56-column budget, so the cut has to be visible.
            Connection h = a;
            h.hostname = std::wstring(70, L'a');
            ConnectionStore hs;
            std::vector<Connection> hrows;
            hrows.push_back(h);
            hs.ReplaceSnapshot(std::move(hrows));
            ListOptions hc;
            hc.columns = {COL_HOST, COL_PROCESS};
            hc.dns = true;
            const CommandResult hr = RenderList(hs, hc);
            const std::string hhead =
                std::string("Hostname") + std::string(50, ' ') + "Process";
            Check(r, "table.cap-host-budget",
                  hr.out.compare(0, hhead.size(), hhead) == 0 &&
                      hr.out.find("\xE2\x80\xA6") != std::string::npos,
                  hr.out);

            // Display width: the three CJK glyphs are double-width, so the
            // row underneath pads to the same column - and no line ends in
            // whitespace (the last column is never right-padded).
            ConnectionStore cs;
            std::vector<Connection> crows;
            Connection u = a;
            u.pid = 1;
            u.processName = L"日本語.exe";
            ConnectionStore::FinalizeRow(u);
            Connection v = a;
            v.pid = 2;
            v.processName = L"ab.exe";
            ConnectionStore::FinalizeRow(v);
            crows.push_back(u);
            crows.push_back(v);
            cs.ReplaceSnapshot(std::move(crows));
            ListOptions cu;
            cu.columns = {COL_PROCESS, COL_PID};
            const CommandResult cr = RenderList(cs, cu);
            const std::string cuwant =
                "Process     PID\r\n"
                "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.exe    1\r\n"
                "ab.exe        2\r\n";
            Check(r, "table.width-cjk-double", cr.out == cuwant, cr.out);

            // An empty trailing cell (no Service here) must not leave its
            // separator behind: no table line ever ends in whitespace.
            ConnectionStore es;
            std::vector<Connection> erows;
            Connection e = a;
            e.processName = L"x.exe";
            e.serviceName.clear();
            ConnectionStore::FinalizeRow(e);
            erows.push_back(e);
            es.ReplaceSnapshot(std::move(erows));
            ListOptions ec;
            ec.columns = {COL_PROCESS, COL_SERVICE};
            const CommandResult er2 = RenderList(es, ec);
            const std::string ewant = "Process  Service\r\nx.exe\r\n";
            Check(r, "table.no-trailing-space", er2.out == ewant, er2.out);
        }

        // ps: the same table machinery (D14), and CSV quoting that a comma
        // in a process name can actually use.
        {
            ConnectionStore pss;
            std::vector<Connection> prows;
            Connection p = MakeRow(0);
            p.processName = L"a,b.exe";
            ConnectionStore::FinalizeRow(p);
            prows.push_back(p);
            pss.ReplaceSnapshot(std::move(prows));

            PsOptions pt;
            const CommandResult ptbl = RenderPs(pss, pt);
            Check(r, "ps.table-no-tabs",
                  ptbl.out.find('\t') == std::string::npos, ptbl.out);
            PsOptions pcsv;
            pcsv.format = "csv";
            const CommandResult pcsvr = RenderPs(pss, pcsv);
            Check(r, "ps.csv-quotes-comma-name",
                  pcsvr.out.find("\"a,b.exe\"") != std::string::npos,
                  pcsvr.out);
        }

        // The style table itself: numeric right / text left (mirrors the
        // GUI's LVCFMT set), budgets only on columns a late join or the
        // clock fills - this is what keeps a streamed header and the rows
        // that follow it in the same columns.
        Check(r, "cols.style-right-numeric",
              GetColumnStyle(COL_LPORT).right &&
                  GetColumnStyle(COL_PID).right &&
                  GetColumnStyle(COL_RX).right &&
                  GetColumnStyle(COL_DURATION).right &&
                  !GetColumnStyle(COL_PROCESS).right &&
                  !GetColumnStyle(COL_HOST).right &&
                  !GetColumnStyle(COL_TRAFFIC).right);
        Check(r, "cols.style-cap-budgets",
              // Budgets sit only on columns a late join or the clock fills
              // whose values can outgrow the header: the byte counters, the
              // combined Traffic/rate columns, the clock's Duration and the
              // reverse-DNS name. Country deliberately has none - an ISO
              // alpha-2 code (2) fits the 7-column header, so measuring it
              // from content cannot disagree with a streamed header.
              GetColumnStyle(COL_HOST).cap == 56 &&
                  GetColumnStyle(COL_COUNTRY).cap == 0 &&
                  GetColumnStyle(COL_RX).cap == 10 &&
                  GetColumnStyle(COL_NETTOTAL).cap == 10 &&
                  GetColumnStyle(COL_TRAFFIC).cap > 8 &&
                  GetColumnStyle(COL_BANDWIDTH).cap > 20 &&
                  GetColumnStyle(COL_DURATION).cap > 0 &&
                  GetColumnStyle(COL_PROCESS).cap == 0 &&
                  GetColumnStyle(COL_PATH).cap == 0 &&
                  GetColumnStyle(COL_CPU).cap == 0);

        // Second snapshot: drop row 0, change row 1's state. Expect one
        // DISAPPEAR and one STATE event, and no APPEAR.
        std::vector<Connection> seed2;
        for (unsigned i = 1; i < 4; ++i) seed2.push_back(MakeRow(i));
        seed2[0].state = MIB_TCP_STATE_CLOSE_WAIT;
        ConnectionStore::FinalizeRow(seed2[0]);
        st.ReplaceSnapshot(std::move(seed2));
        const std::vector<RowChange> ev = st.TakeChangeEvents();
        const std::string te =
            FormatChangeEvents(ev, "table", L"stamp", nullptr);
        Check(r, "cmd.events-table",
              te.find("DISAPPEAR") != std::string::npos &&
                  te.find("STATE") != std::string::npos &&
                  te.find("] APPEAR ") == std::string::npos,
              te.substr(0, 120));
        const std::string je =
            FormatChangeEvents(ev, "json", L"stamp", nullptr);
        Check(r, "cmd.events-json",
              je.find("\"event\":\"disappear\"") != std::string::npos &&
                  je.find("\"old_state\"") != std::string::npos,
              je.substr(0, 120));
        std::vector<FilterClause> prog;
        ParseFilter(L"process:definitely-not-a-process-xyz", prog);
        Check(r, "cmd.events-filtered-empty",
              FormatChangeEvents(ev, "table", L"stamp", &prog).empty());

        // The event-kind gate (D25). Before this there was NO way to select a
        // kind, so the question an event feed exists for - what OPENED - could
        // not be asked. `--filter event:appear` did not help either: "event"
        // was not a field name, so the token degraded to a substring search
        // for the literal text "event:appear" and matched nothing at all.
        {
            const std::string onlyAppear = FormatChangeEvents(
                ev, "table", L"stamp", nullptr, 1u << kChangeAppear);
            const std::string onlyDisappear = FormatChangeEvents(
                ev, "table", L"stamp", nullptr, 1u << kChangeDisappear);
            const std::string onlyState = FormatChangeEvents(
                ev, "table", L"stamp", nullptr, 1u << kChangeState);
            Check(r, "cmd.events-mask-appear-only",
                  onlyAppear.empty(), onlyAppear);
            Check(r, "cmd.events-mask-disappear-only",
                  onlyDisappear.find("DISAPPEAR") != std::string::npos &&
                      onlyDisappear.find("] STATE ") == std::string::npos,
                  onlyDisappear);
            Check(r, "cmd.events-mask-state-only",
                  onlyState.find("STATE") != std::string::npos &&
                      onlyState.find("DISAPPEAR") == std::string::npos,
                  onlyState);
            // The default must keep printing everything, or this would be a
            // silent behaviour change for every existing caller.
            Check(r, "cmd.events-mask-default-is-all",
                  FormatChangeEvents(ev, "table", L"stamp", nullptr) == te);
            // A mask of 0 means "no filter on kind" - the same escape hatch a
            // zeroed default gives a caller that never heard of --event.
            Check(r, "cmd.events-mask-zero-is-all",
                  FormatChangeEvents(ev, "table", L"stamp", nullptr, 0) == te);
            // Two kinds at once, which is the ordinary "new or changed" feed.
            const std::string appearOrState = FormatChangeEvents(
                ev, "table", L"stamp", nullptr,
                (1u << kChangeAppear) | (1u << kChangeState));
            Check(r, "cmd.events-mask-combines",
                  appearOrState.find("DISAPPEAR") == std::string::npos,
                  appearOrState);
        }

        // Follow-stream target mapping comes from the BINARY row fields
        // (shared with the GUI): display strings carry the port and must
        // never be parsed as addresses.
        Connection v4;
        v4.family = AF_INET;
        v4.protocol = IPPROTO_TCP;
        v4.localPort = 50000;
        v4.remotePort = 443;
        v4.localEndpoint = L"10.0.0.5:50000";
        v4.remoteEndpoint = L"93.184.216.34:443";
        ::InetPtonW(AF_INET, L"10.0.0.5", &v4.local4);
        ::InetPtonW(AF_INET, L"93.184.216.34", &v4.remote4);
        const CaptureTarget ct = MakeCaptureTarget(v4);
        IN_ADDR want = {};
        ::InetPtonW(AF_INET, L"93.184.216.34", &want);
        Check(r, "cmd.capture-target-v4",
              ct.ipV4 && ct.localPort == 50000 && ct.remotePort == 443 &&
                  std::memcmp(ct.remoteAddr, &want, 4) == 0 &&
                  !ct.label.empty());
        Connection v6;
        v6.family = AF_INET6;
        v6.protocol = IPPROTO_TCP;
        v6.localPort = 50001;
        v6.remotePort = 443;
        v6.localEndpoint = L"[fe80::1]:50001";
        v6.remoteEndpoint = L"[2001:db8::1]:443";
        ::InetPtonW(AF_INET6, L"fe80::1", &v6.local6);
        ::InetPtonW(AF_INET6, L"2001:db8::1", &v6.remote6);
        const CaptureTarget ct6 = MakeCaptureTarget(v6);
        IN6_ADDR want6 = {};
        ::InetPtonW(AF_INET6, L"2001:db8::1", &want6);
        Check(r, "cmd.capture-target-v6",
              !ct6.ipV4 && ct6.localPort == 50001 &&
                  ct6.remotePort == 443 &&
                  std::memcmp(ct6.remoteAddr, &want6, 16) == 0 &&
                  !ct6.label.empty());
    }

    // The GeoIP reader against a database it is meant to accept: a real tree,
    // real metadata, both record layouts.
    CheckSyntheticGeoIp(r);

    r.output += "selftest: ";
    r.output += (r.exitCode == 0) ? "all checks passed" : "FAILURES detected";
    r.output += "\r\n";
    return r;
}

// ---- bench -----------------------------------------------------------------

std::string RunBench(unsigned rows, unsigned iters) {
    if (rows < kBenchMinRows) rows = kBenchMinRows;
    if (rows > kBenchMaxRows) rows = kBenchMaxRows;
    if (iters < kBenchMinIters) iters = kBenchMinIters;
    if (iters > kBenchMaxIters) iters = kBenchMaxIters;

    std::vector<Connection> base;
    base.reserve(rows);
    for (unsigned i = 0; i < rows; ++i) base.push_back(MakeRow(i));

    ConnectionStore store;
    ViewQuery qAll;

    // [A] snapshot diff + view rebuild (steady state: identical snapshots).
    double msA = 0;
    for (unsigned it = 0; it < iters; ++it) {
        std::vector<Connection> copy = base;      // untimed: pure input setup
        const double t0 = NowMs();
        store.ReplaceSnapshot(std::move(copy));
        store.SetView(qAll);
        msA += NowMs() - t0;
    }

    // [B] filtered view (field expression, keeps a large TCP subset).
    std::vector<FilterClause> program;
    ParseFilter(L"tcp port:1000-60000", program);
    ViewQuery qFiltered;
    qFiltered.program = &program;
    double msB = 0;
    for (unsigned it = 0; it < iters; ++it) {
        const double t0 = NowMs();
        store.SetView(qFiltered);
        msB += NowMs() - t0;
    }

    // [C] sort by process name with direction toggled each iteration.
    double msC = 0;
    for (unsigned it = 0; it < iters; ++it) {
        const double t0 = NowMs();
        store.SetSort(COL_PROCESS, (it % 2) == 0);
        store.SetView(qAll);
        msC += NowMs() - t0;
    }

    // [D] formatting every column of every visible row (GETDISPINFO cost).
    double msD = 0;
    {
        wchar_t buf[512];
        const double t0 = NowMs();
        for (unsigned it = 0; it < iters; ++it) {
            for (size_t v = 0; v < store.View().size(); ++v) {
                const Connection* c = store.ViewRow(v);
                if (c == nullptr) continue;
                for (int col = 0; col < COL_COUNT; ++col)
                    ConnectionStore::GetColumnText(*c, col, buf, 512);
            }
        }
        msD = NowMs() - t0;
    }

    // [E] ResolveBatch: pid -> name / path / creation time, over rows that
    // deliberately SHARE pids the way a real table does - one browser opens
    // hundreds of sockets, one service answers thousands of connections, so the
    // same pid appears on row after row. The cost that should scale is the
    // number of DISTINCT pids (one kernel round trip each); a per-row
    // implementation instead pays OpenProcess + GetProcessTimes + CloseHandle
    // once per ROW, and this stage is what tells the two apart.
    double msE = 0;
    {
        std::vector<DWORD> pids;   // the machine's real, live processes
        HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = {};
            pe.dwSize = sizeof(pe);
            if (::Process32FirstW(snap, &pe)) {
                do {
                    pids.push_back(pe.th32ProcessID);
                } while (::Process32NextW(snap, &pe));
            }
            ::CloseHandle(snap);
        }
        if (pids.empty()) pids.push_back(::GetCurrentProcessId());

        ProcessResolver resolver;
        std::vector<Connection> batch;
        batch.reserve(rows);
        for (unsigned i = 0; i < rows; ++i) {
            Connection c = MakeRow(i);
            c.pid = pids[i % pids.size()];   // 'rows' rows, few distinct pids
            batch.push_back(c);
        }
        // One resolver, so the cache carries across iterations: the timed part
        // is the steady-state revalidation every refresh actually does, not the
        // first-ever resolution of a pid nobody has seen before.
        const double t0 = NowMs();
        for (unsigned it = 0; it < iters; ++it) resolver.ResolveBatch(batch);
        msE = NowMs() - t0;
    }

    // [F] ReplaceSnapshot over rows with DUPLICATE identity keys - the mDNS
    // shape, where a real table holds many rows that share one 4-tuple + pid
    // (62 identical rows were measured on this host, and that is exactly the
    // case the per-key pairing queue exists for). Previous rows are indexed as
    // a queue per key, and taking the FRONT off a vector shifts everything
    // behind it, so one key of N rows costs O(N^2) where it should cost O(N).
    // Stage [A] never sees this: its rows all have distinct identity keys, so
    // every queue there is length 1 and the shift is free.
    double msF = 0;
    {
        constexpr size_t kDupGroup = 62;   // the measured real-world worst case
        std::vector<Connection> dup = base;
        for (size_t i = 0; i < dup.size(); ++i) {
            const size_t g = i / kDupGroup;
            const UINT port = static_cast<UINT>(10000 + g * 7);
            dup[i].family = AF_INET;
            dup[i].protocol = IPPROTO_UDP;      // one shared UDP endpoint
            dup[i].state = 0;                   // UDP carries no TCP state
            dup[i].localPort = port;
            dup[i].remotePort = 5353;
            dup[i].pid = static_cast<DWORD>(4000 + g);
            ::InetPtonW(AF_INET, L"224.0.0.251", &dup[i].local4);
            ::InetPtonW(AF_INET, L"224.0.0.251", &dup[i].remote4);
        }
        ConnectionStore dupStore;
        for (unsigned it = 0; it < iters; ++it) {
            std::vector<Connection> copy = dup;   // untimed: pure input setup
            const double s0 = NowMs();
            dupStore.ReplaceSnapshot(std::move(copy));
            msF += NowMs() - s0;
        }
    }

    // [G] The ETW event path itself: EtwTraffic::OnEvent() driven the way
    // ProcessTrace drives it, over a synthetic classic-MOF stream (half send
    // opcodes, half receive, 64 distinct PIDs). It times classify + payload
    // parse + the totals lock + the per-PID std::map update - the whole
    // per-event cost W3.5 proposes to optimise. The plan requires a number
    // before that hot path is touched, so this is the number. Reported as
    // ns/event rather than ms/op because it is the unit the claim is made in
    // (100k events per second).
    double nsG = 0;
    double nsG2 = 0;
    unsigned long long eventsG = 0;
    {
        struct Ev {
            BYTE payload[8];
            EVENT_RECORD rec;
        };
        std::vector<Ev> evs(64);
        for (unsigned i = 0; i < 64; ++i) {
            DWORD p = 1000 + i, sz = 1460;
            std::memcpy(evs[i].payload, &p, 4);
            std::memcpy(evs[i].payload + 4, &sz, 4);
            evs[i].rec = EVENT_RECORD{};
            evs[i].rec.EventHeader.ProviderId = kTcpIpProviderGuid;
            // Classic MOF carries the event type in Opcode, Id stays 0 (the
            // 40443-event probe established this; see the etw.classify block).
            evs[i].rec.EventHeader.EventDescriptor.Opcode =
                static_cast<UCHAR>((i & 1u) != 0 ? 11 : 10);
            evs[i].rec.UserData = evs[i].payload;
            evs[i].rec.UserDataLength = 8;
        }

        EtwTraffic etw;   // stack instance: no session, destructor is a no-op
        const unsigned kEvents = 4000000;
        const double t0 = NowMs();
        for (unsigned i = 0; i < kEvents; ++i) etw.OnEvent(&evs[i & 63u].rec);
        const double elapsed = NowMs() - t0;
        eventsG = kEvents;
        nsG = (elapsed * 1e6) / static_cast<double>(kEvents);

        // The same stream with the status bar reading the totals underneath
        // it - Snapshot() takes the same lock and copies the whole map. One
        // Snapshot per 1000 events is 100x FASTER than reality: the GUI reads
        // about once a second against a claimed 100000 events per second, i.e.
        // one read per 100000 events. So this is the contention case at 100x
        // its realistic rate, and any loss it produces would be worse than the
        // worst real one.
        const double t1 = NowMs();
        size_t snapRows = 0;
        for (unsigned i = 0; i < kEvents; ++i) {
            etw.OnEvent(&evs[i & 63u].rec);
            if ((i & 1023u) == 0) snapRows += etw.Snapshot().size();
        }
        nsG2 = ((NowMs() - t1) * 1e6) / static_cast<double>(kEvents);
        // Sanity, not decoration: 64 distinct PIDs go in, so every Snapshot
        // must report 64 rows and snapRows can never be 0. The branch is also
        // what stops the copies from being optimised away, so if this ever
        // trips the reported number is -1.000 rather than a quiet lie.
        if (snapRows == 0) nsG2 = -1.0;
    }

    const auto rowSpeed = [rows, iters](double totalMs) {
        if (totalMs <= 0.0) return 0.0;
        return (static_cast<double>(rows) * iters) / totalMs / 1000.0;
    };

    char line[192];
    std::string out = "WinTCP benchmark: rows=" +
                      std::to_string(rows) + " iters=" +
                      std::to_string(iters) + "\r\n";

    struct Stage { const char* name; double total; };
    const Stage stages[] = {
        {"[A] ReplaceSnapshot + SetView (no filter)", msA},
        {"[B] SetView (filter tcp port:1000-60000)  ", msB},
        {"[C] SetSort + SetView (toggle direction)  ", msC},
        {"[D] GetColumnText (all columns x rows)    ", msD},
        {"[E] ResolveBatch (rows share pids)        ", msE},
        {"[F] ReplaceSnapshot (62-way dup keys)     ", msF},
    };
    for (const Stage& s : stages) {
        ::sprintf_s(line, "  %s  %9.3f ms/op  %8.2f Mrow/s\r\n", s.name,
                    s.total / iters, rowSpeed(s.total));
        out += line;
    }
    ::sprintf_s(line, "  [G] EtwTraffic::OnEvent (ETW event path)   %9.3f ns/event"
                      "  over %llu events\r\n", nsG, eventsG);
    out += line;
    ::sprintf_s(line, "  [G]  same stream, Snapshot() every 1024     %9.3f ns/event"
                      "  (contended)\r\n", nsG2);
    out += line;
    ::sprintf_s(line, "  total timed: %.1f ms\r\n",
                msA + msB + msC + msD + msE + msF);
    out += line;
    out += "  (QueryPerformanceCounter; [D] iterates the current view, "
           "[E] resolves pid->name, [F] pairs duplicate keys, [G] is per "
           "event not per row, other stages scale with 'rows')\r\n";
    return out;
}

}  // namespace wintcp

