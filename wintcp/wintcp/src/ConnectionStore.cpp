// ConnectionStore.cpp
// Model implementation: diffing, filtering (expressions), sorting, text.

#include "ConnectionStore.h"

#include <tcpmib.h>

#include <algorithm>
#include <cerrno>
#include <cwctype>
#include <unordered_map>

#include "TcpTable.h"
#include "Utils.h"
// F5.1/F5.2/F5.3: the column cells below need the integrity and signature
// LABELS, and those are defined next to the enums they describe rather than in
// a presentation header - so the labels and the states cannot drift apart.
#include "ProcessInfo.h"

namespace wintcp {
namespace {

// Time units and the cell capacities derived from them. kMsPerSecondU is the
// unsigned twin of kMsPerSecond: the RTT formatter divides unsigned values, and
// mixing an unsigned dividend with a signed divisor is a narrowing that /W4
// would reject - so both exist and the pair is named rather than one being
// retyped at each site.
constexpr unsigned kMsPerSecondU = 1000;
constexpr unsigned kMsPerMinute = 60000;
// The RTT cell shows one decimal below a minute, so the divisor is a tenth of a
// second. Named because it is a TRUNCATION divisor (see FormatRttMs): rounding
// here would let the cell report more time than was measured.
constexpr unsigned kRttDecimalDivisor = kMsPerSecondU / 10;
// A formatted RTT: "1234", "12.3", or "1234" - at most 4 digits plus a decimal
// and a sign, so this is generous. Sized here rather than at the call site so
// the three formatters cannot disagree about the widest case.
constexpr size_t kRttCellChars = 32;

// ---- small helpers ---------------------------------------------------------

// 'haystack' must already be lowercase; 'needle' too (FilterClause::text).
bool Has(const std::wstring& haystack, const std::wstring& needle) {
    return needle.empty() || haystack.find(needle) != std::wstring::npos;
}

// Rebuild the aggregated lowercase key from the per-field keys. Shared by
// FinalizeRow and SetHostname so the two can never drift apart.
void RebuildLowerAll(Connection& c) {
    c.lowerAll = c.lowerLocal + L" " + c.lowerRemote + L" " + c.lowerState +
                 L" " + c.pidText + L" " + c.lowerProcess + L" " +
                 c.lowerPath + L" " + c.lowerService + L" " + c.lowerHost +
                 L" " + c.lowerProto + L" " + c.lowerParent;
}

bool StartsWithCi(const std::wstring& s, const wchar_t* prefix, std::wstring& rest) {
    const size_t n = ::wcslen(prefix);
    // Allow exact matches too: "ipv6:" / "tcp:" with an empty value are
    // valid family/proto-only tokens (caught by --selftest).
    if (s.size() < n) return false;
    if (::_wcsnicmp(s.c_str(), prefix, n) != 0) return false;
    rest = s.substr(n);
    return true;
}

// --- the filter vocabulary ---------------------------------------------------
//
// This table IS the parser, not a parallel copy of it. The previous form was a
// 25-branch if/else chain, and that shape had two consequences, both of which
// bit us:
//
//   1. Nothing could enumerate the accepted spellings, so no test could assert
//      anything about them. That is how `dis.` survived: debris from the 2026
//      identifier-corruption incident in temp/audit_report.md, where a regex
//      pass turned `Disk` into `Dis.` and the repair made that broken spelling
//      WORK instead of deleting it. `list --filter "dis.:1"` returned rows,
//      every gate passed, and no gate COULD fail - nothing asserted that
//      `dis.` should not match.
//   2. Any repair meant editing a 25-way conditional, which is precisely how
//      the debris got re-accepted in the first place.
//
// So: one table, exposed via FilterKeywordTable(), which makes the vocabulary
// inspectable data the selftest can hold to a policy. A second hand-written
// list of the same spellings would have been worse than the if-chain, because it
// could drift - and drift is the failure mode this whole change exists to
// remove.
//
// FilterKeyword itself is declared in ConnectionStore.h, NOT here. Declaring it
// in both places makes it an ambiguous symbol inside this file (C2872), and the
// header copy is the one the selftest sees, so the table must use that type or
// the two would silently be different types.
//
// The hyphen in `min-rtt` is deliberate - `ss -i` spells it that way - which is
// why the selftest policy permits `-` and forbids `.` specifically rather than
// demanding plain alphanumeric.
constexpr FilterKeyword kFilterKeywords[] = {
    {FilterField::Local,    L"local"},
    {FilterField::Remote,   L"remote"},
    {FilterField::LPort,    L"lport"},
    {FilterField::RPort,    L"rport"},
    {FilterField::Port,     L"port"},
    {FilterField::Pid,      L"pid"},
    {FilterField::Process,  L"process"},
    {FilterField::Path,     L"path"},
    {FilterField::State,    L"state"},
    {FilterField::Proto,    L"proto"},
    {FilterField::Host,     L"host"},
    {FilterField::Service,  L"service"},
    {FilterField::Cpu,      L"cpu"},
    {FilterField::Mem,      L"mem"},
    {FilterField::Mem,      L"memory"},
    // `disk` and `io`, and nothing else. `dis.` was accepted here until this
    // table existed; it was corruption debris a botched repair had turned into
    // a working keyword.
    {FilterField::Disk,     L"disk"},
    {FilterField::Disk,     L"io"},
    {FilterField::Rx,       L"rx"},
    {FilterField::Rx,       L"received"},
    {FilterField::Tx,       L"tx"},
    {FilterField::Tx,       L"sent"},
    {FilterField::Net,      L"net"},
    {FilterField::Net,      L"total"},
    {FilterField::Duration, L"duration"},
    {FilterField::Duration, L"age"},
    {FilterField::Speed,    L"speed"},
    {FilterField::Speed,    L"bps"},
    {FilterField::Speed,    L"bandwidth"},
    {FilterField::Tls,      L"tls"},
    {FilterField::Tls,      L"ssl"},
    {FilterField::Country,  L"country"},
    {FilterField::Country,  L"geo"},
    // D26: the bookmark note is joined onto the row, so it is a real,
    // searchable field. `note:` previously did not exist as a field name at
    // all, so `note:corp` degraded to a substring search for the literal text
    // "note:corp" and could never match anything.
    {FilterField::Note,     L"note"},
    // G6, `ss -i` field names. `rtt` and `minrtt` are MILLISECONDS, which is
    // what the columns show and what `ss -i` prints - the kernel's RttUs is
    // converted at the sampler boundary, so a filter threshold and the cell
    // beside it are always the same unit.
    //
    // `latency` is accepted as an alias because "rtt" is jargon and a person
    // looking for a slow connection may well reach for the plainer word first.
    {FilterField::Rtt,      L"rtt"},
    {FilterField::Rtt,      L"latency"},
    {FilterField::MinRtt,   L"minrtt"},
    {FilterField::MinRtt,   L"min-rtt"},
    {FilterField::Cwnd,     L"cwnd"},
    {FilterField::Cwnd,     L"window"},
    {FilterField::Retrans,  L"retrans"},
    {FilterField::Retrans,  L"retransmits"},
    // F5.1/F5.2/F5.3. `ppid` is also the column's underlying number, so
    // `ppid:1234` is an exact threshold rather than a substring; `parent` is the
    // image name beside it. `signed` is an alias of `signature` because the
    // question people actually ask is "is it signed", not "what is its
    // signature state" - the same reason `latency` aliases `rtt`.
    {FilterField::Ppid,      L"ppid"},
    {FilterField::Parent,    L"parent"},
    {FilterField::Integrity, L"integrity"},
    {FilterField::Signature, L"signature"},
    {FilterField::Signature, L"signed"},
};

constexpr size_t kFilterKeywordCount =
    sizeof(kFilterKeywords) / sizeof(kFilterKeywords[0]);

bool FieldFromName(const std::wstring& raw, FilterField& out) {
    const std::wstring n = ToLowerW(raw);
    for (size_t i = 0; i < kFilterKeywordCount; ++i) {
        if (n == kFilterKeywords[i].spelling) {
            out = kFilterKeywords[i].field;
            return true;
        }
    }
    return false;
}

// "443" or "1000-2000" -> inclusive range. Returns false if not numeric.
bool ParseNumberRange(const std::wstring& v, long long& lo, long long& hi) {
    const size_t dash = v.find(L'-');
    const std::wstring a = (dash == std::wstring::npos) ? v : v.substr(0, dash);
    const std::wstring b = (dash == std::wstring::npos) ? v : v.substr(dash + 1);
    if (a.empty() || b.empty()) return false;
    for (wchar_t ch : a) if (!::iswdigit(ch)) return false;
    for (wchar_t ch : b) if (!::iswdigit(ch)) return false;
    errno = 0;
    lo = ::wcstoll(a.c_str(), nullptr, 10);
    hi = ::wcstoll(b.c_str(), nullptr, 10);
    if (errno != 0 || lo < 0 || hi < 0 || lo > hi) return false;
    return true;
}

// The live-stat filter fields (cpu / mem / disk / rx / tx / net / speed).
bool IsStatField(FilterField f) {
    return f == FilterField::Cpu || f == FilterField::Mem ||
           f == FilterField::Disk || f == FilterField::Rx ||
           f == FilterField::Tx || f == FilterField::Net ||
           f == FilterField::Speed;
}

// One threshold in a stat field's own unit, not a substring of the printed
// cell. `cpu:12` means "12 % or more", `mem:100` "100 MB or more" - the units
// the documented examples already used - and a bare number is MB (cpu: %).
// An optional K/M/G suffix (a trailing `b` allowed, so 512KB and 512K agree)
// multiplies. False when there is no number: the caller then keeps the old
// text-clause behaviour instead of silently matching nothing.
bool ParseStatValue(const std::wstring& v, bool cpuUnit, long long* out) {
    size_t b = 0, e = v.size();
    while (b < e && ::iswspace(v[b])) ++b;
    while (e > b && ::iswspace(v[e - 1])) --e;
    if (b >= e) return false;

    size_t numEnd = e;
    wchar_t unit = 0;
    const wchar_t last = v[e - 1];
    if (last == L'b' || last == L'B') {
        // "512KB" / "1.5GB": the unit is TWO characters, so both go. "512B"
        // on its own is a plain byte count.
        if (e - b < 2) return false;
        const wchar_t u = v[e - 2];
        if (u == L'k' || u == L'K' || u == L'm' || u == L'M' ||
            u == L'g' || u == L'G') {
            numEnd = e - 2;
            unit = (u == L'k' || u == L'K') ? L'k'
                 : ((u == L'g' || u == L'G') ? L'g' : L'm');
        } else {
            numEnd = e - 1;
            unit = L'b';
        }
    } else if (last == L'k' || last == L'K' || last == L'm' || last == L'M' ||
               last == L'g' || last == L'G') {
        numEnd = e - 1;
        unit = (last == L'k' || last == L'K') ? L'k'
             : ((last == L'g' || last == L'G') ? L'g' : L'm');
    }

    bool digits = false;
    for (size_t i = b; i < numEnd; ++i)
        if (::iswdigit(v[i])) digits = true;
    if (!digits) return false;
    const std::wstring num = v.substr(b, numEnd - b);
    errno = 0;
    wchar_t* end = nullptr;
    const double d = ::wcstod(num.c_str(), &end);
    if (errno != 0 || d < 0.0 || end == nullptr || *end != L'\0') return false;

    if (cpuUnit) {
        *out = static_cast<long long>(d * 10.0 + 0.5);   // tenths of a percent
        return true;
    }
    long long mult = 1024LL * 1024LL;                     // default: MB
    if (unit == L'b') mult = 1;
    else if (unit == L'k') mult = 1024LL;
    else if (unit == L'g') mult = 1024LL * 1024LL * 1024LL;
    const double bytes = d * static_cast<double>(mult);
    if (bytes > 9.0e18) return false;
    *out = static_cast<long long>(bytes + 0.5);
    return true;
}

// "100" | "100MB" | "1.5-2" | "1MB-2GB". A bare value is a LOWER BOUND ONLY
// (hi = LLONG_MAX). Ports and PIDs deliberately keep the exact-match meaning
// ParseNumberRange has always had, which is why the two do not share a parser.
bool ParseStatRange(const std::wstring& v, bool cpuUnit, long long* lo, long long* hi) {
    const size_t dash = v.find(L'-', 1);
    if (dash == std::wstring::npos) {
        if (!ParseStatValue(v, cpuUnit, lo)) return false;
        *hi = static_cast<long long>(0x7FFFFFFFFFFFFFFFULL);
        return true;
    }
    return ParseStatValue(v.substr(0, dash), cpuUnit, lo) &&
           ParseStatValue(v.substr(dash + 1), cpuUnit, hi) && *lo <= *hi;
}

// A connection AGE, in seconds. Unli.e the byte fields - where a bare number is
// MB - a bare number here is SECONDS, and s/m/h/d are honoured so `duration:1h`
// reads the way a person writes it. `duration:` used to be a substring match on
// the PRINTED cell ("6d 6h"), which is why `duration:1h` matched nothing at all:
// the typed text and the displayed text rarely share a shape.
bool ParseDurationValue(const std::wstring& v, long long* out) {
    size_t b = 0, e = v.size();
    while (b < e && ::iswspace(v[b])) ++b;
    while (e > b && ::iswspace(v[e - 1])) --e;
    if (b >= e) return false;

    size_t numEnd = e;
    long long mult = 1;                          // bare = seconds
    const wchar_t last = v[e - 1];
    if (last == L's' || last == L'S')      { numEnd = e - 1; mult = 1; }
    else if (last == L'm' || last == L'M') { numEnd = e - 1; mult = 60; }
    else if (last == L'h' || last == L'H') { numEnd = e - 1; mult = 3600; }
    else if (last == L'd' || last == L'D') { numEnd = e - 1; mult = 86400; }

    bool digits = false;
    for (size_t i = b; i < numEnd; ++i)
        if (::iswdigit(v[i])) digits = true;
    if (!digits) return false;                   // "abc" stays a text clause
    const std::wstring num = v.substr(b, numEnd - b);
    errno = 0;
    wchar_t* end = nullptr;
    const double d = ::wcstod(num.c_str(), &end);
    if (errno != 0 || d < 0.0 || end == nullptr || *end != L'\0') return false;
    const double seconds = d * static_cast<double>(mult);
    if (seconds > 9.0e18) return false;
    *out = static_cast<long long>(seconds + 0.5);
    return true;
}

// A bare value is a LOWER BOUND ONLY (hi = LLONG_MAX), li.e the stat fields.
bool ParseDurationRange(const std::wstring& v, long long* lo, long long* hi) {
    const size_t dash = v.find(L'-', 1);
    if (dash == std::wstring::npos) {
        if (!ParseDurationValue(v, lo)) return false;
        *hi = static_cast<long long>(0x7FFFFFFFFFFFFFFFULL);
        return true;
    }
    return ParseDurationValue(v.substr(0, dash), lo) &&
           ParseDurationValue(v.substr(dash + 1), hi) && *lo <= *hi;
}

// Stable identity key of an endpoint: protocol + family + addresses + ports
// + OWNING PID.
//
// WHY THE PID IS PART OF IT. The 4-tuple alone is NOT unique on a real
// machine, and that is not a corner case - it is the normal shape of an mDNS
// endpoint. Measured on this host: `list --filter "lport:5353" --format csv`
// returns 86 rows over only 10 distinct (pid, proto, local) identities, with 62
// rows sharing the identical key UDPv4|0.0.0.0:5353|*|* and 24 sharing
// UDPv6|::|5353|*|*. Every browser process binds its own Socketet to port 5353.
//
// When ReplaceSnapshot's previous-row map was keyed without the PID, those 62
// rows collapsed onto ONE map entry (the last write wins), so on every single
// refresh 61 of the 62 previous rows matched nothing and were reported as
// DISAPPEAR ghosts, and the 62 fresh rows were reported as new. Measured
// effect: `list --changes --watch 1 --count 3` emitted 173 DISAPPEAR and 0
// APPEAR events, repeating the same false lines every tick, and the GUI
// re-flashed 62 rows red then 62 green forever. An event feed that can only
// ever say DISAPPEAR is worse than no feed.
//
// Adding the PID ma.es the key unique for the cases that actually occur, and
// `ReplaceSnapshot` additionally matches duplicate keys in order (see
// PrevKeyIndex), so even two rows that are genuinely indistinguishable keep a
// stable 1:1 pairing across refreshes instead of collapsing.
//
// The state is deliberately still NOT part of the key: a connection moving
// ESTABLISHED -> TIME_WAIT must stay the SAME row (that is what produces the
// STATE change event), and a closed socket also changes owner PID to 0, which
// is why the PID is matched by pairing rather than by equality on the previous
// snapshot's value alone.
ConnectionKey KeyOf(const Connection& c) {
    static_assert(2 + 16 + 4 + 16 + 4 + 4 <= ConnectionKey::kMaxBytes,
                  "the widest key (IPv6) must fit ConnectionKey::bytes");

    ConnectionKey k;
    const auto push = [&k](const void* src, size_t n) {
        std::memcpy(k.bytes + k.len, src, n);
        k.len += n;
    };

    // Byte for byte the layout the std::string version wrote, in the same
    // order and the same widths, so every pairing decision ReplaceSnapshot
    // makes is unchanged - only the allocation is gone.
    k.bytes[k.len++] = static_cast<unsigned char>(
        c.family == AF_INET6 ? '6' : '4');
    k.bytes[k.len++] = static_cast<unsigned char>(
        c.protocol == IPPROTO_UDP ? 'U' : 'T');
    if (c.family == AF_INET6) {
        push(&c.local6, 16);
        push(&c.localPort, sizeof(UINT));
        push(&c.remote6, 16);
    } else {
        // Only local4/remote4 are read for IPv4: the v6 fields of an IPv4 row
        // are untouched by the enumerator and may hold stale bytes from a
        // reused Connection, so including them would split one row into two.
        push(&c.local4, 4);
        push(&c.localPort, sizeof(UINT));
        push(&c.remote4, 4);
    }
    push(&c.remotePort, sizeof(UINT));
    push(&c.pid, sizeof(DWORD));
    return k;
}

// Natural TCP state ordering: active first, passive last.
int StateRank(DWORD s) {
    switch (s) {
        case MIB_TCP_STATE_ESTAB:      return 0;
        case MIB_TCP_STATE_SYN_SENT:   return 1;
        case MIB_TCP_STATE_SYN_RCVD:   return 2;
        case MIB_TCP_STATE_FIN_WAIT1:  return 3;
        case MIB_TCP_STATE_FIN_WAIT2:  return 4;
        case MIB_TCP_STATE_TIME_WAIT:  return 5;
        case MIB_TCP_STATE_CLOSE_WAIT: return 6;
        case MIB_TCP_STATE_CLOSING:    return 7;
        case MIB_TCP_STATE_LAST_ACK:   return 8;
        case MIB_TCP_STATE_LISTEN:     return 9;
        case MIB_TCP_STATE_CLOSED:     return 10;
        case MIB_TCP_STATE_DELETE_TCB: return 11;
        case 0:                        return 12;   // none (UDP)
        default:                       return 13;
    }
}

int CmpStr(const std::wstring& a, const std::wstring& b) {
    return a.compare(b);
}

int CmpInt(long long a, long long b) {
    return (a < b) ? -1 : ((a > b) ? 1 : 0);
}

int CmpU64(ULONGLONG a, ULONGLONG b) {
    return (a < b) ? -1 : ((a > b) ? 1 : 0);
}

int CmpDbl(double a, double b) {
    return (a < b) ? -1 : ((a > b) ? 1 : 0);
}

// Saturating add for byte totals (a pathological ETW/IO counter pair must
// never wrap the displayed or sorted total bac. to ~0).
ULONGLONG SatAdd(ULONGLONG a, ULONGLONG b) {
    const ULONGLONG r = a + b;
    return (r < a) ? static_cast<ULONGLONG>(~0ULL) : r;
}

// Display-style lowercase text of a live-stat field, for text filtering
//. Unknown/zero values yield "" so `cpu:12` only matches rows
// that actually have a reading.
std::wstring StatFieldText(const Connection& c, FilterField f) {
    wchar_t buf[64] = {0};
    switch (f) {
        case FilterField::Cpu:
            if (c.cpuPct < 0.0) return std::wstring();
            ::swprintf_s(buf, L"%.1f %%", c.cpuPct);
            return ToLowerW(buf);
        case FilterField::Mem:
            return c.memKnown ? ToLowerW(FormatBytes(c.memWorkingSet))
                              : std::wstring();
        case FilterField::Disk:
            return c.ioKnown
                       ? ToLowerW(FormatBytes(
                             SatAdd(c.diskReadBytes, c.diskWriteBytes)))
                       : std::wstring();
        case FilterField::Rx:
            return (c.trafficRx != 0) ? ToLowerW(FormatBytes(c.trafficRx))
                                      : std::wstring();
        case FilterField::Tx:
            return (c.trafficTx != 0) ? ToLowerW(FormatBytes(c.trafficTx))
                                      : std::wstring();
        case FilterField::Net:
            return (SatAdd(c.trafficRx, c.trafficTx) != 0)
                       ? ToLowerW(FormatBytes(
                             SatAdd(c.trafficRx, c.trafficTx)))
                       : std::wstring();
        default:
            return std::wstring();
    }
}

}  // namespace

// The two exported handles on the keyword table. They sit here, outside the
// anonymous namespace, and return the SAME array FieldFromName reads - so the
// table cannot be extended in one place and forgotten in the other, which is
// the drift risk a hand-maintained mirror list would have introduced.
const FilterKeyword* FilterKeywordTable() { return kFilterKeywords; }

size_t FilterKeywordCount() { return kFilterKeywordCount; }

// ---- exported display helpers ----------------------------------------------
// These are declared in ConnectionStore.h (and exercised by --selftest), so
// they live in namespace wintcp rather than the file-local anonymous
// namespace - otherwise the header declaration and the definition would be
// two different functions and every call site would be ambiguous.

// "1d 4h", "2h 17m", "8m 03s" - a connection's age at a glance. Deltas are
// floored rather than rounded so the display never shows "59s" for 59.7s
// worth of connection and never wraps to "0s" too early.
std::wstring FormatDuration(ULONGLONG seconds) {
    wchar_t buf[32] = {0};
    if (seconds >= 86400ULL) {
        ::swprintf_s(buf, L"%llud %lluh",
                     seconds / 86400ULL, (seconds % 86400ULL) / 3600ULL);
        return buf;
    }
    if (seconds >= 3600ULL) {
        ::swprintf_s(buf, L"%lluh %llum",
                     seconds / 3600ULL, (seconds % 3600ULL) / 60ULL);
        return buf;
    }
    if (seconds >= 60ULL) {
        ::swprintf_s(buf, L"%llum %02llus",
                     seconds / 60ULL, seconds % 60ULL);
        return buf;
    }
    ::swprintf_s(buf, L"%llus", seconds);
    return buf;
}

// Shared by COL_BANDWIDTH (per connection) and COL_GROUPRATE (per process).
// G5's reason for the extraction is in the header: two rate columns that must
// render identically, in one place so they cannot drift.
std::wstring FormatBpsCell(double rxBps, double txBps, bool known) {
    if (!known) return L"—";
    // Under half a byte per second in both directions is "nothing is moving",
    // and printing "↓ 0 B/s ↑ 0 B/s" for it is noise: the reader's eye goes to
    // a rate and finds a rounding artefact.
    if (rxBps < 0.5 && txBps < 0.5) return L"idle";
    wchar_t buf[64] = {0};
    ::swprintf_s(buf, L"↓ %s/s  ↑ %s/s",
                 FormatBytes(static_cast<ULONGLONG>(rxBps + 0.5)).c_str(),
                 FormatBytes(static_cast<ULONGLONG>(txBps + 0.5)).c_str());
    return buf;
}

// G6: milliseconds, the way `ss -i` prints an RTT.
//
// Three rules, each of which is a decision rather than a formatting habit:
//
//  * SUB-MILLISECOND PRINTS "<1", NOT "0". A loopbac. or same-switch RTT is
//    tens of microseconds, and the sampler rounds it to 0 ms. Printing "0" would
//    claim the round trip too. no time at all - a physically impossible reading
//    that a user would reasonably report as a bug. "<1" is what `ss` shows and
//    it is true.
//  * WHOLE MILLISECONDS PRINT WITHOUT A DECIMAL. "12" not "12.000": most
//    RTTs are not fractional, and trailing zeros ma.e the column wide enough to
//    push every other column across the table.
//  * FRACTIONAL VALUES KEEP ONE DECIMAL, THEN TRIM IT. 12.5 is a real reading
//    worth seeing exactly; 12.50 is noise.
std::wstring FormatRttMs(unsigned ms) {
    wchar_t buf[kRttCellChars] = {0};
    if (ms == 0) return L"<1";
    if (ms < kMsPerSecondU) {
        ::swprintf_s(buf, L"%u", ms);
        return buf;
    }
    if (ms < kMsPerMinute) {
        // One decimal place: (ms % 1000) / 100. So 1250 ms prints "1.2" and
        // 1499 ms prints "1.4" - a truncation, not a round-to-nearest. Both are
        // defensible, but a column that shows "1.4" beside a min RTT of "1.5"
        // would be self-evidently wrong, and truncation can never report a value
        // greater than the one measured.
        //
        // A trailing ".0" is then trimmed, so exactly 1000 ms reads "1" rather
        // than "1.0": the decimal exists to show SUB-second detail, and a whole
        // number of seconds has none.
        ::swprintf_s(buf, L"%u.%u", ms / kMsPerSecondU,
                             (ms % kMsPerSecondU) / kRttDecimalDivisor);
        std::wstring s(buf);
        const size_t dot = s.rfind(L'.');
        // Only trim when the '.0' is the whole fractional part. "10.0" -> "10",
        // and a value li.e "1.05" never reaches here (only one decimal is ever
        // produced), so there is no ris. of eating a significant digit.
        if (dot != std::wstring::npos && s.size() == dot + 2 &&
            s[dot + 1] == L'0') {
            s.erase(dot);
        }
        return s;
    }
    ::swprintf_s(buf, L"%llu",
                 static_cast<unsigned long long>(ms / kMsPerSecondU));
    return buf;
}

// Age of a connection in seconds, or 0 when it is unknown. 'nowTick' is
// GetTickCount64; both it and Connection::firstSeenTick are monotonic so
// the subtraction cannot go bac.wards the way a wall clock would across a
// DST or NTP change. Passing 0 (the default) means "now", so display code
// does not have to thread the current tick through every call site.
ULONGLONG DurationSeconds(const Connection& c, ULONGLONG nowTick) {
    if (nowTick == 0) nowTick = ::GetTickCount64();
    if (c.firstSeenTick == 0 || nowTick < c.firstSeenTick) return 0;
    return (nowTick - c.firstSeenTick) / 1000ULL;
}

// One cell of the TLS column. The most useful fact is the negotiated
// protocol; the SNI and certificate are appended when a capture-based or
// ETW source supplied them, because those are what identify the peer.
std::wstring TlsSummary(const TlsInfo& t) {
    if (!t.known || !t.secure) return L"—";
    std::wstring out = TlsProtocolName(t.protocol);
    if (t.haveSni && !t.sni.empty()) {
        out += L"  " + t.sni;
    } else if (t.cipherSuite != 0) {
        out += L"  " + TlsCipherName(t.cipherSuite);
    }
    return out;
}

// "TLS 1.3" / "TLS 1.2" / "SSL 3.0"; the wire bytes are major<<8 | minor.
std::wstring TlsProtocolName(USHORT v) {
    switch (v) {
        case 0x0300: return L"SSL 3.0";
        case 0x0301: return L"TLS 1.0";
        case 0x0302: return L"TLS 1.1";
        case 0x0303: return L"TLS 1.2";
        case 0x0304: return L"TLS 1.3";
        default:     return L"TLS";
    }
}

// Only the handful an analyst actually sees in a connection viewer; an
// unmapped id is reported numerically rather than guessed, so the column
// never asserts a cipher it does not know.
std::wstring TlsCipherName(USHORT id) {
    switch (id) {
        case 0x1301: return L"TLS_AES_128_GCM_SHA256";
        case 0x1302: return L"TLS_AES_256_GCM_SHA384";
        case 0x1303: return L"TLS_CHACHA20_POLY1305_SHA256";
        case 0xC02B: return L"TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256";
        case 0xC02F: return L"TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256";
        case 0xC030: return L"TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384";
        case 0xC034: return L"TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384";
        case 0x009C: return L"TLS_RSA_WITH_AES_128_GCM_SHA256";
        case 0x009D: return L"TLS_RSA_WITH_AES_256_GCM_SHA384";
        case 0xCCA8: return L"TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256";
        default: {
            wchar_t buf[16] = {0};
            ::swprintf_s(buf, L"0x%04X", id);
            return buf;
        }
    }
}

std::wstring TagLabel(unsigned tag) {
    switch (tag) {
        case kTagRed:   return L"red";
        case kTagAmber: return L"amber";
        case kTagBlue:  return L"blue";
        case kTagGreen: return L"green";
        default:        return L"—";
    }
}

bool ComputeBps(ULONGLONG prevBytes, ULONGLONG nowBytes,
                ULONGLONG elapsedMs, double* bytesPerSecond) {
    // No elapsed time means no rate. This is not a theoretical case: a
    // paused snapshot followed by an immediate manual F5 samples twice
    // inside the same millisecond.
    if (elapsedMs == 0) return false;
    // A counter that went bac.wards means the Socketet was recycled (or the
    // owning PID exited and the row was rebuilt). The unsigned difference
    // would be astronomically large, so report "no reading" for this
    // interval instead of a nonsense spi.e; the next sample is normal.
    if (nowBytes < prevBytes) return false;
    if (bytesPerSecond != nullptr) {
        *bytesPerSecond =
            static_cast<double>(nowBytes - prevBytes) * 1000.0 /
            static_cast<double>(elapsedMs);
    }
    return true;
}

// ---- filter parsing / matching -----------------------------------

bool ParseFilter(const std::wstring& text, std::vector<FilterClause>& out) {
    out.clear();
    const size_t len = text.size();
    size_t i = 0;
    while (i < len) {
        while (i < len && ::iswspace(text[i])) ++i;
        const size_t start = i;
        // A to.en may be a QUOTED string, and the quotes are part of the to.en
        // only so the shell and this scanner agree on where it ends.
        //
        // WHY. Space is AND in this grammar, so a value containing a space -
        // a bookmark note ("vendor api"), a path fragment with a space in the
        // directory name, a host suffix - was unreachable: `note:vendor api`
        // parsed as `note:vendor` AND a bare `api`, which is a different
        // question and almost never the one meant. Quoting is the standard
        // answer and costs one branch on the to.en scanner. The quotes are
        // stripped from the value, so `note:"vendor api"` and
        // `path:"program files"` mean what they loo. li.e.
        std::wstring t;
        if (i < len && (text[i] == L'"' || text[i] == L'\'')) {
            const wchar_t quote = text[i];
            ++i;
            while (i < len && text[i] != quote) t.push_back(text[i++]);
            if (i < len) ++i;              // closing quote (if present)
        } else {
            // A quoted PART of a to.en: `note:"vendor api"` keeps its field
            // prefix and quotes only the value. Handling only the "to.en starts
            // with a quote" case made every `field:"..."` split at the space,
            // which is precisely the form a user writes for a note or a path
            // with a space in it - the case quoting was added for.
            while (i < len && !::iswspace(text[i])) {
                if (text[i] == L'"' || text[i] == L'\'') {
                    const wchar_t quote = text[i++];
                    while (i < len && text[i] != quote) t.push_back(text[i++]);
                    if (i < len) ++i;
                    continue;
                }
                t.push_back(text[i++]);
            }
        }
        if (start == i) continue;

        FilterClause cl;
        std::wstring rest;
        if (StartsWithCi(t, L"exclude:", rest)) { cl.exclude = true; t = rest; }
        // Direction is stripped first and consumed on its own: it selects
        // the endpoint, it is not itself a field. Stripping it as a field
        // (the old code) made local:port:80 collapse into
        // {field=Local, text="port:80"}, which never matches.
        if (StartsWithCi(t, L"local:", rest))       { cl.direction = -1; t = rest; }
        else if (StartsWithCi(t, L"remote:", rest)) { cl.direction = +1; t = rest; }
        if (StartsWithCi(t, L"tcp:", rest))          { cl.proto = IPPROTO_TCP; t = rest; }
        else if (StartsWithCi(t, L"udp:", rest))     { cl.proto = IPPROTO_UDP; t = rest; }
        else if (StartsWithCi(t, L"ipv4:", rest))    { cl.family = 4; t = rest; }
        else if (StartsWithCi(t, L"ipv6:", rest))    { cl.family = 6; t = rest; }

        if (cl.field == FilterField::Any) {
            const size_t colon = t.find(L':');
            if (colon != std::wstring::npos && colon > 0) {
                FilterField f;
                if (FieldFromName(t.substr(0, colon), f)) {
                    cl.field = f;
                    t = t.substr(colon + 1);
                }
            }
        }

        const std::wstring value = t;
        const bool numericWanted =
            (cl.field == FilterField::Pid || cl.field == FilterField::Port ||
             cl.field == FilterField::LPort || cl.field == FilterField::RPort) ||
            (cl.proto != 0 && cl.field == FilterField::Any);
        long long lo = 0, hi = 0;
        if (!value.empty() && numericWanted && ParseNumberRange(value, lo, hi)) {
            cl.numeric = true;
            cl.lo = lo;
            cl.hi = hi;
        } else if (!value.empty() && IsStatField(cl.field) &&
                   ParseStatRange(value, cl.field == FilterField::Cpu, &lo, &hi)) {
            // Live stats compare as numbers: `mem:100` is "100 MB or more"
            // (bare = MB, K/M/G suffix allowed, `mem:100-500` a range) and
            // `cpu:12` is "12 % or more". A non-numeric value falls through to
            // the text clause below, exactly as an unknown port value does.
            cl.numeric = true;
            cl.lo = lo;
            cl.hi = hi;
        } else if (cl.field == FilterField::Duration && !value.empty() &&
                   ParseDurationRange(value, &lo, &hi)) {
            // `duration:` is a threshold in SECONDS, not a substring of the
            // printed cell: `duration:1h` means "at least an hour old" and
            // `duration:1h-2d` is a range. A non-numeric value falls through to
            // the text clause below, exactly as `mem:abc` does.
            cl.numeric = true;
            cl.lo = lo;
            cl.hi = hi;
        } else if ((cl.field == FilterField::Rtt ||
                    cl.field == FilterField::MinRtt ||
                    cl.field == FilterField::Cwnd ||
                    cl.field == FilterField::Retrans) && !value.empty() &&
                   (ParseNumberRange(value, lo, hi) ||
                    ((cl.field == FilterField::Cwnd ||
                      cl.field == FilterField::Retrans) &&
                     ParseStatRange(value, false, &lo, &hi)))) {
            // G6: a threshold in the units the column shows - ms for the RTTs,
            // bytes for cwnd and retransmits. `rtt:100` is "100 ms or worse".
            // The two time fields are deliberately NOT routed through
            // ParseStatRange: that would make `rtt:1M` mean 1 megabyte of
            // latency, which is a joke waiting to happen.
            //
            // The parse is part of the branch CONDITION on purpose. It used to
            // be an `if` inside the body, so a value the number parser rejected
            // - `retrans:1KB`, whose suffix is not a digit - left BOTH
            // cl.numeric false and cl.text unset. That empty text then reached
            // HasLowerSubstring, which returns true for an empty needle, so a
            // threshold silently degraded into "matches every row that has a
            // reading" - exactly the outcome `help list` promises never
            // happens. An unparsable value now falls through to the text clause
            // at the end of this chain, the way the stat and duration branches
            // already did.
            //
            // The two byte fields also accept the documented KB/MB/GB/B
            // suffixes, via ParseStatRange. A plain integer still takes the
            // first path, so `retrans:1024` and `cwnd:65536` keep the byte
            // meaning `help list` gives them.
            //
            // A BARE value is "at least this much", with NO upper bound - the
            // same rule `duration:1h` and `mem:100` follow. lo == hi would mean
            // `rtt:20` matches only a connection whose RTT is exactly 20 ms,
            // which is not a threshold at all and is the sort of thing that
            // ships because "it parsed" was taken for "it works". Measured:
            // `rtt:20` returned 0 rows against a row reading 24.
            cl.numeric = true;
            cl.lo = lo;
            cl.hi = (value.find(L'-') == std::wstring::npos)
                        ? LLONG_MAX
                        : hi;
        } else if (cl.field == FilterField::Proto && !value.empty()) {
            const std::wstring v = ToLowerW(value);
            if      (v == L"tcp")  cl.proto = IPPROTO_TCP;
            else if (v == L"udp")  cl.proto = IPPROTO_UDP;
            else if (v == L"4" || v == L"ipv4") cl.family = 4;
            else if (v == L"6" || v == L"ipv6") cl.family = 6;
            else cl.text = v;    // fall bac. to substring on the proto label
        } else if (value.empty() && cl.proto == 0 && cl.family == 0 &&
                   cl.field == FilterField::Any) {
            continue;            // bare "exclude:" etc. - no-op to.en
        } else {
            // A NAMED field with an empty value is a real clause, not a no-op:
            // `note:` means "has a note", `path:` means "has a path". It used
            // to be dropped with the bare-prefix to.ens above, so the useful
            // "show me everything I annotated" query silently became a no-op
            // and matched every row instead. The empty text then falls
            // through to Has(), which matches everything for most fields -
            // which is right for `path:` and `service:` and wrong only for
            // note, handled explicitly in MatchClause.
            cl.text = ToLowerW(value);
            if (cl.field == FilterField::State) {
                // State labels use underscores (TIME_WAIT); users type
                // hyphens (time-wait) just as often. Normalize once here so
                // both spellings match and the hot match path stays a plain
                // substring test.
                for (wchar_t& ch : cl.text) {
                    if (ch == L'-') ch = L'_';
                }
            }
        }
        out.push_back(std::move(cl));
    }
    return true;
}

bool MatchClause(const Connection& c, const FilterClause& cl) {
    if (cl.proto != 0 && c.protocol != cl.proto) return false;
    if (cl.family != 0) {
        const int f = (c.family == AF_INET6) ? 6 : 4;
        if (f != cl.family) return false;
    }

    // 'field' names the column; 'direction' narrows Port/LPort/RPort to one
    // side of the endpoint. A bare direction with no field keeps the
    // documented meaning "this side's address" ("remote:203").
    FilterField f = cl.field;

    if (cl.numeric) {
        const auto inRange = [&cl](long long v) { return v >= cl.lo && v <= cl.hi; };
        // A direction narrows Port to that side: "local:port:443" must not
        // match a row whose REMOTE port is 443.
        switch (f) {
            case FilterField::Pid:   return inRange(static_cast<long long>(c.pid));
            case FilterField::LPort: return inRange(static_cast<long long>(c.localPort));
            case FilterField::RPort: return inRange(static_cast<long long>(c.remotePort));
            case FilterField::Port:
                if (cl.direction < 0)
                    return inRange(static_cast<long long>(c.localPort));
                if (cl.direction > 0)
                    return inRange(static_cast<long long>(c.remotePort));
                return inRange(static_cast<long long>(c.localPort)) ||
                       inRange(static_cast<long long>(c.remotePort));
            // "tcp:80" with no side: either port may match. A direction
            // narrows it. The live-stat fields (cpu/mem/dis./rx/tx/net) are
            // NOT handled here - they have a unit-aware comparison further
            // down, so fall through to it rather than reject the row. Any
            // OTHER numeric field is not defined: return false rather than
            // falling bac. to ports, which made "pid:1-2" match any connection
            // using a port in 1..2.
            case FilterField::Any:
                if (cl.direction < 0)
                    return inRange(static_cast<long long>(c.localPort));
                if (cl.direction > 0)
                    return inRange(static_cast<long long>(c.remotePort));
                return inRange(static_cast<long long>(c.localPort)) ||
                       inRange(static_cast<long long>(c.remotePort));
            default: break;   // -> the live-stat bloc. below
        }
    }

    // Text match against one lowercased key.
    wchar_t tmp[24] = {0};
    const std::wstring* hay = nullptr;
    switch (f) {
        case FilterField::Any:
            // A bare direction restricts a text value to that endpoint;
            // without one it searches the aggregated key.
            if (cl.direction != 0)
                hay = (cl.direction < 0) ? &c.lowerLocal : &c.lowerRemote;
            else
                hay = &c.lowerAll;
            break;
        case FilterField::Local:   hay = &c.lowerLocal;  break;
        case FilterField::Remote:  hay = &c.lowerRemote; break;
        case FilterField::Process: hay = &c.lowerProcess; break;
        case FilterField::Path:    hay = &c.lowerPath;   break;
        case FilterField::Service: hay = &c.lowerService; break;
        case FilterField::Host:    hay = &c.lowerHost;   break;
        case FilterField::State:   hay = &c.lowerState;  break;
        case FilterField::Proto:   hay = &c.lowerProto;  break;
        case FilterField::Pid:     hay = &c.pidText;     break;
        // F5.1. `parent` searches the parent's IMAGE NAME. It is in the hay
        // switch rather than a case of its own because, like Process, it is a
        // precomputed lowercase string and needs no formatting.
        case FilterField::Parent:  hay = &c.lowerParent; break;
        // F5.1. `ppid` is a NUMBER, so it formats on demand exactly like the
        // ports do - including the same reuse of `tmp`, since that buffer is
        // already what this switch formats digits into.
        case FilterField::Ppid:
            if (!c.ppidKnown) return false;
            ::swprintf_s(tmp, L"%lu", static_cast<unsigned long>(c.ppid));
            return HasLowerSubstring(tmp, cl.text);
        // Ports are formatted digits: they have no case, so match the
        // buffer directly instead of paying for a ToLowerW copy of every
        // row, per clause, on every keystro.e.
        case FilterField::LPort:
            ::swprintf_s(tmp, L"%u", c.localPort);
            return HasLowerSubstring(tmp, cl.text);
        case FilterField::RPort:
            ::swprintf_s(tmp, L"%u", c.remotePort);
            return HasLowerSubstring(tmp, cl.text);
        case FilterField::Port:
            if (cl.text.empty()) return true;
            if (cl.direction < 0) {
                ::swprintf_s(tmp, L"%u", c.localPort);
                return HasLowerSubstring(tmp, cl.text);
            }
            if (cl.direction > 0) {
                ::swprintf_s(tmp, L"%u", c.remotePort);
                return HasLowerSubstring(tmp, cl.text);
            }
            ::swprintf_s(tmp, L"%u", c.localPort);
            if (HasLowerSubstring(tmp, cl.text)) return true;
            ::swprintf_s(tmp, L"%u", c.remotePort);
            return HasLowerSubstring(tmp, cl.text);
        case FilterField::Cpu:
        case FilterField::Mem:
        case FilterField::Disk:
        case FilterField::Rx:
        case FilterField::Tx:
        case FilterField::Net: {
            // Live per-process stats. A numeric clause is a threshold in the
            // field's real unit (cpu: tenths of a %, everything else bytes),
            // not a substring of the printed cell: the old text match only
            // ever satisfied `mem:100` by accident, when "100" happened to be
            // printed inside some other value. An unknown reading matches
            // nothing at all - a protected process must not slip through
            // `mem:0` just because it could not be read.
            if (cl.numeric) {
                switch (f) {
                    case FilterField::Cpu: {
                        if (c.cpuPct < 0.0) return false;
                        const long long v =
                            static_cast<long long>(c.cpuPct * 10.0 + 0.5);
                        return v >= cl.lo && v <= cl.hi;
                    }
                    case FilterField::Mem:
                        if (!c.memKnown) return false;
                        return c.memWorkingSet >= static_cast<ULONGLONG>(cl.lo) &&
                               c.memWorkingSet <= static_cast<ULONGLONG>(cl.hi);
                    case FilterField::Disk: {
                        if (!c.ioKnown) return false;
                        const ULONGLONG t = SatAdd(c.diskReadBytes, c.diskWriteBytes);
                        return t >= static_cast<ULONGLONG>(cl.lo) &&
                               t <= static_cast<ULONGLONG>(cl.hi);
                    }
                    case FilterField::Rx:
                        return c.trafficRx >= static_cast<ULONGLONG>(cl.lo) &&
                               c.trafficRx <= static_cast<ULONGLONG>(cl.hi);
                    case FilterField::Tx:
                        return c.trafficTx >= static_cast<ULONGLONG>(cl.lo) &&
                               c.trafficTx <= static_cast<ULONGLONG>(cl.hi);
                    case FilterField::Net: {
                        const ULONGLONG t = SatAdd(c.trafficRx, c.trafficTx);
                        return t >= static_cast<ULONGLONG>(cl.lo) &&
                               t <= static_cast<ULONGLONG>(cl.hi);
                    }
                    default:
                        return false;
                }
            }
            // Not numeric (e.g. "mem:abc"): keep the substring behaviour so
            // the box filter never silently changes shape.
            return Has(StatFieldText(c, f), cl.text);
        }
        case FilterField::Duration: {
            // Unknown age matches nothing rather than everything, so
            // "duration:" cannot silently select the rows that lac. a value.
            if (c.firstSeenTick == 0) return false;
            if (cl.numeric) {
                const long long secs = static_cast<long long>(
                    DurationSeconds(c));
                return secs >= cl.lo && secs <= cl.hi;
            }
            return HasLowerSubstring(FormatDuration(DurationSeconds(c)),
                                     cl.text);
        }
        case FilterField::Speed: {
            if (!c.bpsKnown) return false;
            const long long bps =
                static_cast<long long>((c.rxBps + c.txBps) + 0.5);
            if (cl.numeric) return bps >= cl.lo && bps <= cl.hi;
            return HasLowerSubstring(FormatBytes(static_cast<ULONGLONG>(bps)),
                                     cl.text);
        }
        case FilterField::Rtt:
        case FilterField::MinRtt:
        case FilterField::Cwnd:
        case FilterField::Retrans: {
            // G6. An UNKNOWN value matches nothing, never everything - the same
            // rule as `duration:` above, and for the same reason: a row the
            // kernel could not measure must not be selected by a query about a
            // measurement. `rtt:` alone therefore means "has an RTT reading",
            // which is a genuinely useful question ("which of these can I even
            // measure?"), rather than a no-op that matches all 300 rows.
            const bool known =
                (cl.field == FilterField::Rtt)     ? c.rttKnown
                : (cl.field == FilterField::MinRtt) ? c.rttKnown
                : (cl.field == FilterField::Cwnd)   ? c.cwndKnown
                :                                  c.retransKnown;
            if (!known) return false;
            const long long v =
                (cl.field == FilterField::Rtt)     ? static_cast<long long>(c.rttMs)
                : (cl.field == FilterField::MinRtt) ? static_cast<long long>(c.minRttMs)
                : (cl.field == FilterField::Cwnd)   ? static_cast<long long>(c.cwnd)
                :                                  static_cast<long long>(c.retransBytes);
            if (cl.numeric) return v >= cl.lo && v <= cl.hi;
            // Text form: match the cell the column would actually print, so a
            // filter and a display can never disagree. "12.5" matches "12.500",
            // and "50" matches a byte count formatted as "50 B".
            const std::wstring cell =
                (cl.field == FilterField::Rtt || cl.field == FilterField::MinRtt)
                    ? FormatRttMs(static_cast<unsigned>(v))
                    : FormatBytes(static_cast<ULONGLONG>(v));
            return HasLowerSubstring(cell, cl.text);
        }
        case FilterField::Tls:
            return Has(ToLowerW(TlsSummary(c.tls)), cl.text);
        case FilterField::Country:
            return Has(ToLowerW(c.country), cl.text);
        case FilterField::Note:
            // The joined bookmark note. A bare `note:` means "this row HAS a
            // note" - the useful "show me everything I annotated" query -
            // and is therefore false for an unbookmarked row, whose note is
            // empty. `note:x` matches the rows whose note contains x, which is
            // the whole point of writing one.
            if (cl.text.empty()) return !c.lowerNote.empty();
            return Has(c.lowerNote, cl.text);
        // F5.2. The level is a WORD, so it is matched as the text the column
        // prints - `integrity:high`, `integrity:system` - and never as the RID,
        // because "12288" is not an answer to any question a reader asks. The
        // bare form means "has a readable level", which is how `note:` and
        // `country:` behave: the useful query is "show me everything running as
        // System".
        case FilterField::Integrity: {
            if (c.integrity == 0 ||
                c.integrity >= static_cast<unsigned>(kIntegrityCount))
                return false;
            if (cl.text.empty()) return true;
            // ToLowerW, not a bare HasLowerSubstring: the parameter is named
            // `haystackLower` and does NOT fold case itself. The labels are
            // capitalised ("High", "Protected"), so matching the raw label
            // against a lower-case needle never found anything - `integrity:high`
            // returned 0 rows on a machine with eleven High processes, which is
            // precisely the "silently matches nothing" failure help list
            // forbids.
            //
            // The needle is matched against the CELL, marker included, so a
            // filter and the column cannot disagree: `integrity:ac` finds the
            // AppContainer rows because "+AC" is literally what the column
            // prints beside the level.
            std::wstring cell =
                ToLowerW(IntegrityLabel(static_cast<IntegrityLevel>(c.integrity)));
            if (cell.empty()) return false;
            if (c.appContainer) cell += L"+ac";
            return cell.find(cl.text) != std::wstring::npos;
        }
        // F5.3. Same shape: match the printed verdict, and let the bare form ask
        // the question worth asking - `signed:` alone means "WinTCP actually
        // verified this image", which is NOT the same as "the verdict was good".
        // A row whose image was never checked matches neither, because
        // reporting it would mean passing off an unasked question as an answer.
        case FilterField::Signature: {
            if (c.signature == 0 ||
                c.signature >= static_cast<unsigned>(kSigCount))
                return false;
            if (cl.text.empty())
                return c.signature == static_cast<unsigned>(kSigValid);
            // ToLowerW for the same reason as the integrity case above: the
            // haystack parameter does not fold case, and "BAD SIG" is the label
            // that needs folding most.
            const std::wstring label =
                ToLowerW(SignatureLabel(static_cast<SignatureState>(c.signature)));
            return label.find(cl.text) != std::wstring::npos;
        }
        default: return false;
    }
    return hay != nullptr && Has(*hay, cl.text);
}

bool MatchFilter(const Connection& c, const std::vector<FilterClause>& prog) {
    for (const FilterClause& cl : prog) {
        bool m = MatchClause(c, cl);
        if (cl.exclude) m = !m;
        if (!m) return false;
    }
    return true;
}

// ---- store -----------------------------------------------------------------

void ConnectionStore::FinalizeRow(Connection& c) {
    c.protoLabel = (c.protocol == IPPROTO_UDP) ? L"UDP" : L"TCP";
    c.protoLabel += (c.family == AF_INET6) ? L"v6" : L"v4";
    c.stateLabel = (c.protocol == IPPROTO_UDP) ? L"—" : TcpStateToString(c.state);

    wchar_t pidb[16] = {0};
    ::swprintf_s(pidb, L"%lu", static_cast<unsigned long>(c.pid));
    c.pidText = pidb;

    c.lowerLocal   = ToLowerW(c.localAddress);
    c.lowerRemote  = ToLowerW(c.remoteAddress);
    c.lowerProcess = ToLowerW(c.processName);
    c.lowerPath    = ToLowerW(c.processPath);
    c.lowerService = ToLowerW(c.serviceName);
    c.lowerHost    = ToLowerW(c.hostname);
    c.lowerState   = ToLowerW(c.stateLabel);
    c.lowerProto   = ToLowerW(c.protoLabel);
    // F5.1. Precomputed here for the same reason every other lower* field is:
    // MatchClause runs per row per clause, and ToLowerW allocates a fresh
    // string every time - doing it once per refresh instead of once per
    // keystroke is the whole reason this row of fields exists.
    c.lowerParent  = ToLowerW(c.parentName);

    RebuildLowerAll(c);
}

void ConnectionStore::ReplaceSnapshot(std::vector<Connection> fresh) {
    changes_.clear();
    const ULONGLONG nowTick = ::GetTickCount64();

    // Map previous (non-ghost) rows by identity key, keeping a QUEUE per key
    // rather than one entry.
    //
    // WHY NOT ONE ENTRY. A plain map assignment (`prevMap[KeyOf(row)] = i`) keeps
    // only the LAST row for a repeated key, so N rows sharing an identity match
    // one previous row between them and the other N-1 are reported as
    // DISAPPEAR ghosts - then reappear as new rows on the next refresh, forever.
    // That is not hypothetical: mDNS endpoints (UDP 5353) are shared by every
    // browser on the box, and 62 identical rows were measured here. It turned
    // `--changes` into a stream of 173 false DISAPPEAR / 0 APPEAR per run and
    // made the GUI flash rows red-then-green without end.
    //
    // With a queue, the N-th fresh row of a key pairs with the N-th previous
    // row of that key. Rows of one key are interchangeable to the user (same
    // endpoint, same owner, same state - that is why they share a key), so
    // first-come pairing is a stable 1:1 and nothing is reported as churn.
    // Built BACKWARDS - highest previous index first - so that a queue
    // consumed with pop_back() hands out ascending indexes, which is exactly
    // the first-come order front()/erase(begin()) used to produce.
    //
    // WHY THE DIRECTION MATTERS. Taking the front off a vector shifts every
    // element behind it, so one key of N previous rows cost O(N^2) across the
    // refresh while it should cost O(N); the mDNS case this queue exists for
    // is 62 rows of one key. Reversing the build buys the O(1) pop with no
    // second pass, no head index and no change to which row pairs with which -
    // the pairing is still first-come, only the container's idea of "first"
    // is now its back. Rows of a key are interchangeable to the pairing logic
    // anyway (same endpoint, same owner, same state - that is why they share
    // a key), but keeping the exact previous order means no change event,
    // age or bookmark can move to a different row than it did before.
    PrevKeyIndex prevMap;
    prevMap.reserve(rows_.size() * 2 + 1);
    for (size_t i = rows_.size(); i-- > 0;) {
        if (rows_[i].flags & kRowRemoved) continue;
        prevMap[KeyOf(rows_[i])].push_back(i);
    }
    std::vector<char> matched(rows_.size(), 0);

    std::vector<Connection> merged;
    merged.reserve(fresh.size() + rows_.size());

    for (Connection& f : fresh) {
        FinalizeRow(f);
        const ConnectionKey key = KeyOf(f);
        const auto it = prevMap.find(key);
        if (it != prevMap.end() && !it->second.empty()) {
            // Consume this key's queue: the queue was built backwards (see
            // above), so back() is the LOWEST previous index still waiting -
            // first-come order - and popping it means a later fresh row with
            // the same key gets the NEXT previous row, exactly as before.
            // pop_back() is O(1); erase(begin()) was O(N).
            const size_t pi = it->second.back();
            it->second.pop_back();
            const Connection& p = rows_[pi];
            matched[pi] = 1;
            f.id = p.id;
            f.hostname = p.hostname;         // carry reverse-DNS results
            // State that outlives a single refresh: when the endpoint first
            // appeared, and the user's own annotations. Both are keyed on
            // the endpoint identity, so a row that vanishes and comes bac.
            // with the same 4-tuple keeps its age and bookmark.
            f.firstSeenTick = p.firstSeenTick;
            f.pinned = p.pinned;
            f.tag = p.tag;
            f.flags = 0;
            if (p.state != f.state) {
                f.flags |= kRowChanged;
                RowChange ch;
                ch.kind = kChangeState;
                ch.row = f;
                ch.oldState = p.state;
                changes_.push_back(std::move(ch));
            }
            // Carry the PREVIOUS sample forward, and let ComputeRates() do the
            // arithmetic once the traffic join has written this tick's
            // counters.
            //
            // WHY NOT HERE. The bps used to be computed inline, which loo.s
            // right and is not: at this point the fresh row still has
            // perRowBytes == false and trafficRx == 0, because the byte counters
            // are joined onto the row AFTER the snapshot is installed. So the
            // test was always false and the Speed column was a permanent
            // em-dash even after the per-Socketet join started supplying real
            // per-row counters. Splitting it means the comparison is between
            // two REAL readings of the same Socketet, taken at two real times.
            f.perRowBytes = p.perRowBytes;
            f.lastSampleTick = p.lastSampleTick;
            f.lastRxBytes = p.lastRxBytes;
            f.lastTxBytes = p.lastTxBytes;
            // G5: the two rate accumulators are carried separately, and the
            // PREVIOUS tick's sample point is what both need - the previous
            // per-Socketet counters (above, for the row's own rate) and the
            // previous GROUP total (for the process rate). Sharing one
            // lastSampleTick between them is safe precisely because they are
            // sampled by the same scan at the same instant; carrying a second
            // tick would only create a way for them to disagree about when
            // "a second ago" was.
            f.lastGroupRxBytes = p.lastGroupRxBytes;
            f.lastGroupTxBytes = p.lastGroupTxBytes;
            f.lastGroupTick = p.lastGroupTick;
            // G6: congestion state is per-Socketet and absolute (not a delta), so
            // there is no previous sample to carry - the fresh join simply
            // overwrites it each tick. The CUMULATIVE parts are protected from
            // going bac.wards inside ApplySocketTcpInfo, which is where the
            // "a smaller reading means a recycled Socketet" rule belongs.
            f.rttKnown = false;
            f.cwndKnown = false;
            f.retransKnown = false;
            f.timestampsKnown = false;
        } else {
            f.id = nextId_++;
            f.firstSeenTick = nowTick;   // age starts now, not at launch
            f.flags |= kRowNew;
            RowChange ch;
            ch.kind = kChangeAppear;
            ch.row = f;
            changes_.push_back(std::move(ch));
        }
        merged.push_back(std::move(f));
    }

    // Previous rows that vanished become one-cycle ghosts (red), and only
    // if they were not already ghosts (ghosts never persist past one cycle).
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (matched[i]) continue;
        if (rows_[i].flags & kRowRemoved) continue;
        Connection ghost = rows_[i];
        ghost.flags |= kRowRemoved;
        RowChange ch;
        ch.kind = kChangeDisappear;
        ch.row = ghost;
        changes_.push_back(std::move(ch));
        merged.push_back(std::move(ghost));
    }

    rows_ = std::move(merged);
    RebuildIndexes();
}

// Turn this tick's byte counters into a per-connection RATE, now that the
// traffic join has written them.
//
// MUST be called AFTER SetTraffic/ApplySocketBytes and after ReplaceSnapshot:
// the rate is a difference between two readings of the SAME Socketet, so both
// the previous sample (carried forward by ReplaceSnapshot) and the current one
// (written by the join) have to be in place first. Calling it inside
// ReplaceSnapshot instead - which is where it used to live - compares a
// previous reading against a fresh row's zeros, and the `perRowBytes` test it
// guarded on was never true, so the Speed column was a permanent em-dash.
//
// The per-PID ETW path leaves perRowBytes false on purpose: a process total
// spread over its connection rows cannot honestly be attributed to one Socketet,
// so those rows keep showing the same em-dash as any other unreadable reading
// rather than an invented split.
//
// Returns the number of rows that now have a known rate.
int ConnectionStore::ComputeRates() {
    const ULONGLONG nowTick = ::GetTickCount64();
    int known = 0;
    for (Connection& r : rows_) {
        if (!r.perRowBytes) {
            r.bpsKnown = false;
            continue;
        }
        // First tick for this row: there is no previous reading yet, so there
        // is no rate. Record the baseline so the NEXT tick can produce one.
        if (r.lastSampleTick == 0) {
            r.lastSampleTick = nowTick;
            r.lastRxBytes = r.trafficRx;
            r.lastTxBytes = r.trafficTx;
            r.bpsKnown = false;
            continue;
        }
        const ULONGLONG elapsed = nowTick - r.lastSampleTick;
        double rx = 0.0, tx = 0.0;
        const bool okRx = ComputeBps(r.lastRxBytes, r.trafficRx, elapsed, &rx);
        const bool okTx = ComputeBps(r.lastTxBytes, r.trafficTx, elapsed, &tx);
        if (okRx && okTx) {
            r.rxBps = rx;
            r.txBps = tx;
            r.bpsKnown = true;
            ++known;
        } else {
            r.bpsKnown = false;
        }
        r.lastSampleTick = nowTick;
        r.lastRxBytes = r.trafficRx;
        r.lastTxBytes = r.trafficTx;
    }
    ComputeGroupRates();
    return known;
}

// G5: the per-PROCESS rate, summed over the process's Socketets.
//
// WHY A SEPARATE PASS AND NOT A DIVISION. The obvious way to get "how fast is
// this process moving bytes" from a per-PID total is to divide it by the
// connection count. That is inventing a number: the Socketets of one process carry
// wildly different shares of its traffic (one 500 MB download and forty
// keepalives), so the quotient is not any Socketet's rate and not the process's
// either - it is the average of a distribution, which is exactly the figure
// `perRowBytes` was introduced to forbid. The same honesty rule applies here,
// and the sum is the only honest answer available from per-Socketet readings.
//
// WHY THE SUM IS TRUSTWORTHY, and where D20 comes in. Summing is only correct
// if each Socketet is counted exactly once. Before D20 the row key omitted the
// PID, so 86 mDNS rows collapsed onto 10 identities and a per-key queue was
// needed to pair duplicates 1:1 - without that wor. a process holding several
// Socketets on the same endpoint would either double-count its own bytes or lose
// some, and this column would be wrong by an unknown factor. That is why the
// duplicate-identity fix is load-bearing here and not merely tidy.
//
// ONLY ROWS WITH PER-SOCKET BYTES COUNT. A row fed by the ETW source has
// perRowBytes false and represents a process total, not a Socketet. Including it
// would add the whole process figure to a sum that is already counting its
// Socketets - and so count the same bytes twice. Such rows are excluded, and a
// process whose traffic came only from ETW reports no group rate rather than a
// doubled one.
//
// A PROCESS WITH NO SOCKET-COUNTERED ROWS reports "unknown", never zero: zero is
// a claim about a process we did measure, and a dash is the honest answer for
// one we could not.
void ConnectionStore::ComputeGroupRates() {
    const ULONGLONG nowTick = ::GetTickCount64();

    // One pass: sum each PID's Socketet counters, remembering whether any of its
    // rows was Socketet-counted at all.
    std::map<DWORD, PidTraffic> sum;
    std::map<DWORD, bool> anyCounted;
    for (const Connection& r : rows_) {
        if (!r.perRowBytes) continue;
        PidTraffic& t = sum[r.pid];
        t.rx = SatAdd(t.rx, r.trafficRx);
        t.tx = SatAdd(t.tx, r.trafficTx);
        anyCounted[r.pid] = true;
    }

    // Second pass: difference, then write the result and the new baseline onto
    // EVERY row of that PID. Writing to all of them (rather than to one chosen
    // row) is what keeps the invariant true: the next tick reads the same
    // baseline from any row it happens to visit.
    for (Connection& r : rows_) {
        const auto it = sum.find(r.pid);
        const bool counted = it != sum.end() && anyCounted[r.pid];
        if (!counted) {
            r.groupBpsKnown = false;
            r.lastGroupTick = nowTick;
            continue;
        }
        const PidTraffic& t = it->second;
        // First tick for this process: no previous total, so no rate yet. The
        // baseline is recorded so the NEXT tick can produce one.
        if (r.lastGroupTick == 0) {
            r.lastGroupTick = nowTick;
            r.lastGroupRxBytes = t.rx;
            r.lastGroupTxBytes = t.tx;
            r.groupBpsKnown = false;
            continue;
        }
        const ULONGLONG elapsed = nowTick - r.lastGroupTick;
        double rx = 0.0, tx = 0.0;
        const bool okRx =
            ComputeBps(r.lastGroupRxBytes, t.rx, elapsed, &rx);
        const bool okTx =
            ComputeBps(r.lastGroupTxBytes, t.tx, elapsed, &tx);
        if (okRx && okTx) {
            r.groupRxBps = rx;
            r.groupTxBps = tx;
            r.groupBpsKnown = true;
        } else {
            r.groupBpsKnown = false;
        }
        r.lastGroupTick = nowTick;
        r.lastGroupRxBytes = t.rx;
        r.lastGroupTxBytes = t.tx;
    }
}

void ConnectionStore::RebuildIndexes() {
    pidRows_.clear();
    addrRows_.clear();
    stats_ = RowStats{};
    stats_.total = rows_.size();

    pidRows_.reserve(rows_.size() / 2 + 1);
    addrRows_.reserve(rows_.size() / 2 + 1);
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Connection& r = rows_[i];
        pidRows_[r.pid].push_back(i);
        if (!r.remoteAddress.empty()) addrRows_[r.remoteAddress].push_back(i);
        if (r.family == AF_INET6) ++stats_.ipv6; else ++stats_.ipv4;
        if (r.protocol == IPPROTO_UDP) ++stats_.udp;
        if (r.state < 13) ++stats_.byState[r.state];
        if (r.pinned || r.tag != kTagNone) ++stats_.pinned;
        if (r.tls.known && r.tls.secure) ++stats_.secure;
    }
}

void ConnectionStore::SetView(const ViewQuery& q) {
    flatIndex_.clear();
    flatIndex_.reserve(rows_.size());
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Connection& r = rows_[i];
        const unsigned famBit  = (r.family == AF_INET6) ? kProtoMaskV6 : kProtoMaskV4;
        const unsigned protoBit = (r.protocol == IPPROTO_UDP) ? kProtoMaskUDP : kProtoMaskTCP;
        if ((q.protoMask & famBit) == 0 || (q.protoMask & protoBit) == 0) continue;
        if (q.stateFilter != kAllStates && r.state != q.stateFilter) continue;
        if (q.program != nullptr && !q.program->empty() && !MatchFilter(r, *q.program))
            continue;
        flatIndex_.push_back(i);
    }
    std::sort(flatIndex_.begin(), flatIndex_.end(),
              [this](size_t a, size_t b) {
                  return CompareRows(rows_[a], rows_[b], sortColumn_, sortAsc_) < 0;
              });
    RebuildView();
}

// 5.1. Collapses the filtered+sorted flat list into one entry per process
// when grouping is on. The flat list is kept, so turning grouping off
// restores the previous view exactly rather than re-deriving it - the rows
// were never consumed to build the groups.
void ConnectionStore::RebuildView() {
    if (!grouped_) {
        viewIndex_ = flatIndex_;
        groups_.clear();
        return;
    }

    // Project the flat rows into the grouping module's minimal view. Names
    // point into rows_, which outlive the call.
    std::vector<GroupRow> proj;
    proj.reserve(flatIndex_.size());
    for (size_t rowIdx : flatIndex_) {
        const Connection& c = rows_[rowIdx];
        GroupRow g;
        g.pid = c.pid;
        g.processName = c.processName.empty() ? nullptr : &c.processName;
        g.tcp = (c.protocol != IPPROTO_UDP);
        g.trafficRx = c.trafficRx;
        g.trafficTx = c.trafficTx;
        g.connectionCount = 1;
        g.removed = (c.flags & kRowRemoved) != 0;
        proj.push_back(g);
    }
    groups_ = GroupByProcess(proj);

    // One view entry per group, pointing at that group's FIRST member row.
    // The list paints the row via ViewRow() and the aggregate via ViewGroup(),
    // so a group row and its totals always describe the same process.
    viewIndex_.clear();
    viewIndex_.reserve(groups_.size());
    for (const ProcessGroup& g : groups_) {
        if (g.members.empty()) continue;
        viewIndex_.push_back(flatIndex_[g.members.front()]);
    }
}

const ProcessGroup* ConnectionStore::ViewGroup(size_t viewIdx) const {
    if (!grouped_ || viewIdx >= groups_.size()) return nullptr;
    return &groups_[viewIdx];
}

void ConnectionStore::SetSort(int column, bool ascending) {
    if (column < 0 || column >= COL_COUNT) return;
    sortColumn_ = column;
    sortAsc_ = ascending;
}

const Connection* ConnectionStore::ViewRow(size_t viewIdx) const {
    if (viewIdx >= viewIndex_.size()) return nullptr;
    const size_t row = viewIndex_[viewIdx];
    return (row < rows_.size()) ? &rows_[row] : nullptr;
}

size_t ConnectionStore::ViewToRow(size_t viewIdx) const {
    return (viewIdx < viewIndex_.size()) ? viewIndex_[viewIdx] : SIZE_MAX;
}

long long ConnectionStore::RowToView(size_t rowIdx) const {
    for (size_t i = 0; i < viewIndex_.size(); ++i)
        if (viewIndex_[i] == rowIdx) return static_cast<long long>(i);
    return -1;
}

size_t ConnectionStore::CountForPid(DWORD pid) const {
    const auto it = pidRows_.find(pid);
    if (it == pidRows_.end()) return 0;
    size_t n = 0;
    for (size_t i : it->second)
        if (!(rows_[i].flags & kRowRemoved)) ++n;
    return n;
}

// 5.3. Join the bookmark store onto the rows: mar. every row whose remote
// endpoint is bookmarked, and set its colour tag and note.
//
// Called after the snapshot and after any add/remove, not per-row. It reads
// the registry-bac.ed store once and then wal.s the rows, which is the right
// way round: a registry read per row would be thousands of them on a busy
// machine.
//
// 'known' is the set of endpoints present in the bookmark store, so a row
// whose endpoint is NOT bookmarked is explicitly CLEARED rather than left
// alone. S.ipping the clear would leave a stale pin on a row after the user
// removed the bookmark, which is worse than never showing the column.
//
// THE PORT IS PART OF THE MATCH, and that is a fix, not a refinement.
// `bookmark add` stores its identity as address + remote port ("what survives a
// reconnect"), so that is what the join must use. Matching on the address
// alone painted the wrong conversation: with a bookmark on
// 107.155.105.90:9999, the live 107.155.105.90:443 row printed "red" in the
// bookmarks column. A user who mar.ed one peer "suspicious" therefore saw every
// unrelated session to that host flagged, and could not trust the column at all.
//
// Rows whose remote is a placeholder (`*`, `0.0.0.0`) carry remotePort 0, and
// a bookmark on port 0 is rejected at the CLI, so they can never match - which
// is correct: you cannot bookmark "somewhere".
void ConnectionStore::JoinBookmarks(const std::vector<BookmarkMark>& known) {
    // Key: address, a NUL, then the port in decimal. The separator cannot occur
    // inside an address, so no two distinct (address, port) pairs can collide
    // - an address li.e "1.2.3.4:5" is not a legal address, so there is no
    // "1.2.3.4:5" + ":12" vs "1.2.3.4" + ":5:12" ambiguity to guard against.
    std::unordered_map<std::wstring, const BookmarkMark*> byEndpoint;
    byEndpoint.reserve(known.size() * 2 + 1);
    for (const BookmarkMark& b : known) {
        if (b.address.empty()) continue;
        wchar_t port[16] = {0};
        ::swprintf_s(port, L"%u", b.port);
        byEndpoint.emplace(b.address + L"\x01" + port, &b);
    }

    for (Connection& r : rows_) {
        const BookmarkMark* hit = nullptr;
        if (!r.remoteAddress.empty() && r.remotePort != 0) {
            wchar_t port[16] = {0};
            ::swprintf_s(port, L"%u", r.remotePort);
            const auto it = byEndpoint.find(r.remoteAddress + L"\x01" + port);
            if (it != byEndpoint.end()) hit = it->second;
        }
        const bool marked = (hit != nullptr);
        r.pinned = marked;
        // The tag is per-ENDPOINT, so every row to the same peer shows the
        // same colour. That is what "this conversation is red" means.
        // Stored as unsigned on the row, and kTagNone is the anonymous enum's
        // 0, so an unbookmarked row needs no separate "unknown" state.
        r.tag = marked ? hit->tag : static_cast<unsigned>(kTagNone);
        // D26: the note used to live only in the registry, where the table
        // could not see it and no filter could reach it. Carrying it on the
        // row is what ma.es `note:` and the note-bearing columns work.
        r.note = marked ? hit->note : std::wstring();
        if (!r.note.empty()) {
            r.lowerNote = ToLowerW(r.note);
            RebuildLowerAll(r);
        } else if (!r.lowerNote.empty()) {
            r.lowerNote.clear();
            RebuildLowerAll(r);
        }
    }
    if (stats_.total != 0) {
        size_t n = 0;
        for (const Connection& r : rows_)
            if (r.pinned) ++n;
        stats_.pinned = n;
    }
}

bool ConnectionStore::SetCountry(const std::wstring& addr,
                                 const std::wstring& country) {
    if (addr.empty()) return false;
    const auto it = addrRows_.find(addr);
    if (it == addrRows_.end()) return false;
    bool changed = false;
    for (size_t i : it->second) {
        Connection& r = rows_[i];
        if (r.country == country) continue;
        r.country = country;
        // The lower-case copy is what the filter searches. S.ipping it would
        // ma.e `country:de` fail while the column visibly reads "DE" - a
        // filter that ignores a value shown on screen.
        RebuildLowerAll(r);
        changed = true;
    }
    return changed;
}

bool ConnectionStore::SetHostname(const std::wstring& addr,
                                  const std::wstring& host) {
    if (addr.empty()) return false;
    const auto it = addrRows_.find(addr);
    if (it == addrRows_.end()) return false;
    bool changed = false;
    for (size_t i : it->second) {
        Connection& r = rows_[i];
        if (r.hostname == host) continue;
        r.hostname = host;
        r.lowerHost = ToLowerW(host);
        RebuildLowerAll(r);
        changed = true;
    }
    return changed;
}

void ConnectionStore::SetTraffic(DWORD pid, ULONGLONG rx, ULONGLONG tx) {
    const auto it = pidRows_.find(pid);
    if (it == pidRows_.end()) return;
    for (size_t i : it->second) {
        rows_[i].trafficRx = rx;
        rows_[i].trafficTx = tx;
    }
}

void ConnectionStore::ClearTraffic() {
    for (Connection& r : rows_) {
        r.trafficRx = 0;
        r.trafficTx = 0;
        // A per-Socketet total and a per-PID total are different facts about the
        // same cell. Clearing one without the other would leave the row
        // claiming a source it no longer has, which is what keeps `perRowBytes`
        // honest: the flag is re-derived by whichever join runs next.
        r.perRowBytes = false;
        r.bpsKnown = false;
        r.rxBps = 0.0;
        r.txBps = 0.0;
    }
}

// Per-SOCKET counters, from the same scan the per-PID totals come from.
//
// This is the join that sets `perRowBytes`, and it is the ONLY one allowed to:
// a per-PID total spread over a process's connection rows cannot honestly be
// attributed to a single Socketet, and dividing it by the connection count would
// invent a plausible-looking rate. Before this existed nothing set the flag at
// all (a repo-wide grep found only the declaration and its single read), so
// `Connection::bpsKnown` was never true, the Speed column was a permanent
// em-dash and `speed:`/`bandwidth:` filters could match nothing - while `help
// list` and the README both advertised the column.
//
// Matching is on the full 4-tuple plus the protocol, and rows are only claimed
// once per sample: two rows that genuinely share a 4-tuple (the mDNS case) get
// the same cumulative counters, and the second row is left alone rather than
// being given an invented share.
int ConnectionStore::ApplySocketBytes(const std::vector<SocketBytes>& bytes) {
    if (bytes.empty()) return 0;
    int updated = 0;
    std::vector<char> claimed(rows_.size(), 0);
    for (const SocketBytes& b : bytes) {
        if (!b.known) continue;
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (claimed[i]) continue;
            Connection& r = rows_[i];
            if (r.localPort != b.localPort || r.remotePort != b.remotePort)
                continue;
            if (r.localAddress != b.localAddress ||
                r.remoteAddress != b.remoteAddress)
                continue;
            if (r.protocol != IPPROTO_TCP) continue;   // SIO_TCP_INFO is TCP
            r.trafficRx = b.rx;
            r.trafficTx = b.tx;
            r.perRowBytes = true;   // this row's counters ARE one Socketet's
            claimed[i] = 1;
            ++updated;
            break;
        }
    }
    return updated;
}

// G6: join TCP_INFO congestion state onto the rows.
//
// SAME MATCHING AS ApplySocketBytes, and for the same reason: these are
// per-sOCKET facts, so two rows that genuinely share a 4-tuple (the mDNS case
// D20 fixed) must not both claim the same reading. The first row to match wins
// and the Socketet is then mar.ed claimed, so the second row is left blanket rather
// than given a duplicate - which is the honest answer, since we genuinely
// cannot tell which of the two Socketets the reading came from.
//
// Per-field assignment rather than all-or-nothing, because the kernel populates
// these independently: a Socketet with TCP timestamps off still has a perfectly
// real congestion window. A blanketet copy guarded on one `known` would blanket the
// three fields that did come bac..
int ConnectionStore::ApplySocketTcpInfo(
    const std::vector<SocketTcpInfo>& infos) {
    if (infos.empty()) return 0;
    int updated = 0;
    std::vector<char> claimed(rows_.size(), 0);
    for (const SocketTcpInfo& t : infos) {
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (claimed[i]) continue;
            Connection& r = rows_[i];
            if (r.localPort != t.localPort || r.remotePort != t.remotePort)
                continue;
            if (r.localAddress != t.localAddress ||
                r.remoteAddress != t.remoteAddress)
                continue;
            if (r.protocol != IPPROTO_TCP) continue;   // TCP_INFO_v0 is TCP only
            // Cumulative/best-ever values only move in one direction, so a
            // smaller reading is a recycled Socketet rather than new information.
            // Letting it overwrite would reset the connection's history every
            // time a handle was reused.
            if (t.retransKnown &&
                (!r.retransKnown || t.retransBytes >= r.retransBytes)) {
                r.retransBytes = t.retransBytes;
                r.retransKnown = true;
            }
            if (t.rttKnown) {
                if (!r.rttKnown || t.minRttMs < r.minRttMs)
                    r.minRttMs = t.minRttMs;
                r.rttMs = t.rttMs;
                r.rttKnown = true;
            }
            if (t.cwndKnown) {
                r.cwnd = t.cwnd;
                r.cwndKnown = true;
            }
            if (t.timestampsKnown) {
                r.tcpTimestamps = t.timestamps;
                r.timestampsKnown = true;
            }
            claimed[i] = 1;
            ++updated;
            break;
        }
    }
    return updated;
}

// Kernel ages arrive keyed by the 4-tuple in the printable form the rows
// already carry, so this is a loo.up by identity, not by PID: a process with
// twenty connections reports twenty ages, each belonging to one row.
int ConnectionStore::ApplyKernelAges(const std::vector<SocketAge>& ages) {
    if (ages.empty()) return 0;
    const ULONGLONG nowTick = ::GetTickCount64();
    int updated = 0;
    for (const SocketAge& a : ages) {
        if (!a.known || a.ageMs == 0) continue;
        // A kernel age cannot exceed the time since boot, and a sample taken
        // from another machine's clock can be nonsense. Clamping keeps a bad
        // reading from printing "400d" for a Socketet opened a moment ago; the
        // row's own first-seen value is left alone if the age is unusable.
        if (a.ageMs > nowTick) continue;
        const ULONGLONG kernelSeen = nowTick - a.ageMs;
        for (Connection& r : rows_) {
            if (r.localPort != a.localPort || r.remotePort != a.remotePort)
                continue;
            if (r.localAddress != a.localAddress ||
                r.remoteAddress != a.remoteAddress)
                continue;
            if (r.protocol != IPPROTO_TCP) continue;   // TCP_INFO_v0 is TCP only
            // Bac.date only: the kernel's age is authoritative, but a shorter
            // reading must not ma.e a connection loo. younger than we have
            // already proven it to be.
            if (r.firstSeenTick == 0 || kernelSeen < r.firstSeenTick)
                r.firstSeenTick = kernelSeen;
            ++updated;
            break;   // the 4-tuple identifies exactly one row
        }
    }
    return updated;
}

void ConnectionStore::SetProcStats(DWORD pid, double cpuPct, bool memKnown,
                                   ULONGLONG memWs, ULONGLONG memPrivate,
                                   bool ioKnown, ULONGLONG diskRead,
                                   ULONGLONG diskWrite) {
    const auto it = pidRows_.find(pid);
    if (it == pidRows_.end()) return;
    for (size_t i : it->second) {
        Connection& r = rows_[i];
        r.cpuPct = cpuPct;
        r.memKnown = memKnown;
        r.memWorkingSet = memKnown ? memWs : 0;
        r.memPrivate = memKnown ? memPrivate : 0;
        r.ioKnown = ioKnown;
        r.diskReadBytes = ioKnown ? diskRead : 0;
        r.diskWriteBytes = ioKnown ? diskWrite : 0;
    }
}

std::vector<RowChange> ConnectionStore::TakeChangeEvents() {
    std::vector<RowChange> out;
    out.swap(changes_);
    return out;
}

ULONGLONG ConnectionStore::TrafficKey(const Connection& c) {
    return c.trafficRx + c.trafficTx;
}

const wchar_t* ConnectionStore::ColumnTitle(int column) {
    switch (column) {
        case COL_PROTO:   return L"Proto";
        case COL_LOCAL:   return L"Local address";
        case COL_LPORT:   return L"Local port";
        case COL_REMOTE:  return L"Remote address";
        case COL_RPORT:   return L"Remote port";
        case COL_STATE:   return L"State";
        case COL_PID:     return L"PID";
        case COL_PROCESS: return L"Process";
        case COL_SERVICE: return L"Service";
        case COL_HOST:    return L"Hostname";
        case COL_PATH:    return L"Path";
        case COL_TRAFFIC: return L"Traffic (rx/tx)";
        case COL_RX:      return L"Received";
        case COL_TX:      return L"Sent";
        case COL_NETTOTAL: return L"Net total";
        case COL_CPU:     return L"CPU %";
        case COL_MEM:     return L"Memory (WS)";
        case COL_DISK:    return L"Disk I/O";
        case COL_DURATION: return L"Duration";
        case COL_BANDWIDTH: return L"Speed";
        // G6. The headers are the `ss -i` field names in lower case, because
        // that is what someone comparing the two will be looking for. `RTT` is
        // rendered as "RTT" and not "Rtt" purely because it is an initialism;
        // the CLI COLUMN NAMES (what you type) are lower case separately, so
        // the two conventions never have to agree.
        case COL_RTT:      return L"RTT";
        case COL_MINRTT:   return L"Min RTT";
        case COL_CWND:     return L"Cwnd";
        case COL_RETRANS:  return L"Retrans";
        case COL_GROUPRATE: return L"Proc Speed";
        case COL_NOTE: return L"Note";
        case COL_TLS:     return L"TLS";
        case COL_COUNTRY: return L"Country";
        case COL_PINNED:  return L"bookmarks";
        case COL_PPID:    return L"Parent";
        case COL_INTEGRITY: return L"Integrity";
        case COL_SIGNATURE: return L"Signature";
        default:          return L"?";
    }
}

void ConnectionStore::GetColumnText(const Connection& c, int column,
                                    wchar_t* buf, size_t bufChars) {
    if (buf == nullptr || bufChars == 0) return;
    buf[0] = L'\0';
    // Copy, marking any elision with an ellipsis. wcsncpy_s(_TRUNCATE)
    // silently produced a cut-off path, which is worse than a visibly
    // shortened one: a long path truncated mid-component reads as a real
    // but wrong location.
    const auto set = [&](const std::wstring& s) {
        const size_t maxChars = bufChars - 1;   // leave room for the NUL
        if (s.size() <= maxChars) {
            ::wcsncpy_s(buf, bufChars, s.c_str(), _TRUNCATE);
            return;
        }
        if (maxChars == 0) return;              // no room for an ellipsis
        const size_t keep = (maxChars >= 2) ? (maxChars - 1) : 0;
        for (size_t i = 0; i < keep; ++i) buf[i] = s[i];
        buf[keep] = L'…';                       // horizontal ellipsis
        buf[keep + 1] = L'\0';
    };
    switch (column) {
        case COL_PROTO:   set(c.protoLabel); break;
        case COL_LOCAL:   set(c.localAddress); break;
        case COL_LPORT:   ::swprintf_s(buf, bufChars, L"%u", c.localPort); break;
        case COL_REMOTE:  set(c.remoteAddress); break;
        case COL_RPORT:
            if (c.protocol == IPPROTO_UDP && c.remotePort == 0)
                set(L"*");
            else
                ::swprintf_s(buf, bufChars, L"%u", c.remotePort);
            break;
        case COL_STATE:   set(c.stateLabel); break;
        case COL_PID:     ::swprintf_s(buf, bufChars, L"%lu",
                                       static_cast<unsigned long>(c.pid)); break;
        case COL_PROCESS: set(c.processName.empty() ? L"—" : c.processName); break;
        case COL_SERVICE: set(c.serviceName); break;
        case COL_HOST:    set(c.hostname); break;
        case COL_PATH:    set(c.processPath); break;
        case COL_TRAFFIC:
            if (c.trafficRx == 0 && c.trafficTx == 0) {
                set(L"—");
            } else {
                ::swprintf_s(buf, bufChars, L"%s / %s",
                             FormatBytes(c.trafficRx).c_str(),
                             FormatBytes(c.trafficTx).c_str());
            }
            break;
        case COL_RX:
            set((c.trafficRx == 0) ? L"—" : FormatBytes(c.trafficRx));
            break;
        case COL_TX:
            set((c.trafficTx == 0) ? L"—" : FormatBytes(c.trafficTx));
            break;
        case COL_NETTOTAL: {
            const ULONGLONG total = SatAdd(c.trafficRx, c.trafficTx);
            set((total == 0) ? L"—" : FormatBytes(total));
            break;
        }
        case COL_CPU:
            if (c.cpuPct < 0.0) {
                set(L"—");
            } else {
                ::swprintf_s(buf, bufChars, L"%.1f %%", c.cpuPct);
            }
            break;
        case COL_MEM:
            set(c.memKnown ? FormatBytes(c.memWorkingSet) : L"—");
            break;
        case COL_DISK:
            set(c.ioKnown ? FormatBytes(
                                SatAdd(c.diskReadBytes, c.diskWriteBytes))
                          : L"—");
            break;
        case COL_DURATION: set(FormatDuration(DurationSeconds(c))); break;
        case COL_BANDWIDTH:
            // Both directions are shown because the split is the useful part;
            // a single number would hide which way the bytes went.
            set(FormatBpsCell(c.rxBps, c.txBps, c.bpsKnown));
            break;
        case COL_GROUPRATE:
            // G5. Same SHAPE as COL_BANDWIDTH and the same helper, different
            // source: this is the PROCESS's rate, summed over its Socketets. It is
            // populated on flat rows too - where it is the truth and ma.es the
            // column useful ungrouped - and on a grouped row the group renderer
            // supplies it (see GroupColumnText).
            set(FormatBpsCell(c.groupRxBps, c.groupTxBps, c.groupBpsKnown));
            break;
        // G6. Each of these is gated on ITS OWN known flag, never on a shared
        // one: the kernel populates them independently, so a Socketet with TCP
        // timestamps off still has a real congestion window. Gating all four on
        // a single flag would show four dashes for a row that has three good
        // readings - the same "one missing value blankets the row" failure the
        // ApplySocketTcpInfo join would otherwise have introduced.
        case COL_RTT:
            set(c.rttKnown ? FormatRttMs(c.rttMs) : L"—");
            break;
        case COL_MINRTT:
            // Only meaningful alongside a live RTT: without one the "best ever
            // seen" has no measurement to be the best of. A zero minRtt beside a
            // real rttMs means the kernel reported an RTT but no running
            // minimum, so there is genuinely nothing to print.
            set((c.rttKnown && c.minRttMs != 0) ? FormatRttMs(c.minRttMs)
                                               : L"—");
            break;
        case COL_CWND:
            set(c.cwndKnown ? FormatBytes(c.cwnd) : L"—");
            break;
        case COL_RETRANS:
            // ZERO IS A REAL ANSWER here, not "unknown": zero retransmitted
            // bytes is the best possible result and is exactly what you want to
            // see. Printing a dash would hide the healthy case.
            set(c.retransKnown ? FormatBytes(c.retransBytes) : L"—");
            break;
        case COL_TLS:      set(TlsSummary(c.tls)); break;
        case COL_COUNTRY:  set(c.country); break;
        case COL_PINNED:
            // The tag letter only means something next to its colour, which
            // the row itself carries; a pin alone is the default 'P'.
            if (c.pinned && c.tag == kTagNone) set(L"pinned");
            else if (c.tag != kTagNone) set(TagLabel(c.tag));
            else set(L"—");
            break;
        case COL_NOTE:
            // The user's own text. An UNBOOKMARKED row shows the dash even if a
            // note somehow survived on it, because the bookmark join clears both
            // together and a note without a bookmark is not a state that can
            // exist - showing one would imply it could.
            set(c.note.empty() ? L"—" : c.note);
            break;
        // F5.1. "<ppid> <parent image name>". The name is omitted when the
        // snapshot did not contain the parent, which is NOT the same as having
        // no parent - so the number still prints on its own, because collapsing
        // the cell to a dash would tell a reader "this process has no parent",
        // which is the one answer the snapshot cannot actually give.
        case COL_PPID:
            if (!c.ppidKnown) {
                set(L"—");
            } else if (c.parentName.empty()) {
                ::swprintf_s(buf, bufChars, L"%lu",
                             static_cast<unsigned long>(c.ppid));
            } else {
                ::swprintf_s(buf, bufChars, L"%lu %s",
                             static_cast<unsigned long>(c.ppid),
                             c.parentName.c_str());
            }
            break;
        // F5.2. The level, plus "+AC" when the process is AppContainer. Shown
        // WITH the level rather than instead of it: a sandboxed process still
        // has an integrity level, and replacing it with the marker would lose
        // the answer to "how much is it trusted".
        case COL_INTEGRITY: {
            if (c.integrity >= static_cast<unsigned>(kIntegrityCount)) {
                set(L"—");
                break;
            }
            std::wstring s =
                IntegrityLabel(static_cast<IntegrityLevel>(c.integrity));
            if (c.appContainer && s != L"—") s += L"+AC";
            set(s);
            break;
        }
        // F5.3. Unchecked renders as the dash like every other unreadable
        // reading. That is the honest default: WinTCP has not looked at this
        // image, which is different from having looked and found nothing.
        case COL_SIGNATURE:
            if (c.signature >= static_cast<unsigned>(kSigCount)) {
                set(L"—");
            } else {
                set(SignatureLabel(static_cast<SignatureState>(c.signature)));
            }
            break;
        default: break;
    }
}

int ConnectionStore::CompareRows(const Connection& a, const Connection& b,
                                 int column, bool ascending) {
    int cmp = 0;
    // stat columns whose reading is unknown ("—")
    // sort to the END of the list in both directions instead of clumping
    // with the zero values.
    bool unknownLast = false;
    switch (column) {
        case COL_PROTO:
            cmp = CmpInt(static_cast<long long>(a.protocol),
                         static_cast<long long>(b.protocol));
            if (cmp == 0) cmp = CmpInt(a.family, b.family);
            break;
        case COL_LOCAL:   cmp = CmpStr(a.lowerLocal, b.lowerLocal); break;
        case COL_LPORT:   cmp = CmpInt(a.localPort, b.localPort); break;
        case COL_REMOTE:  cmp = CmpStr(a.lowerRemote, b.lowerRemote); break;
        case COL_RPORT:   cmp = CmpInt(a.remotePort, b.remotePort); break;
        case COL_STATE:   cmp = CmpInt(StateRank(a.state), StateRank(b.state)); break;
        case COL_PID:     cmp = CmpInt(a.pid, b.pid); break;
        case COL_PROCESS: cmp = CmpStr(a.lowerProcess, b.lowerProcess); break;
        case COL_SERVICE: cmp = CmpStr(a.lowerService, b.lowerService); break;
        case COL_HOST:    cmp = CmpStr(a.lowerHost, b.lowerHost); break;
        case COL_PATH:    cmp = CmpStr(a.lowerPath, b.lowerPath); break;
        case COL_TRAFFIC: cmp = CmpU64(TrafficKey(a), TrafficKey(b)); break;
        case COL_RX:      cmp = CmpU64(a.trafficRx, b.trafficRx); break;
        case COL_TX:      cmp = CmpU64(a.trafficTx, b.trafficTx); break;
        case COL_NETTOTAL:
            cmp = CmpU64(TrafficKey(a), TrafficKey(b));
            break;
        case COL_CPU: {
            const bool ka = a.cpuPct >= 0.0;
            const bool kb = b.cpuPct >= 0.0;
            if (ka != kb) { unknownLast = true; cmp = ka ? -1 : 1; }
            else cmp = ka ? CmpDbl(a.cpuPct, b.cpuPct) : 0;
            break;
        }
        case COL_MEM: {
            if (a.memKnown != b.memKnown) {
                unknownLast = true;
                cmp = a.memKnown ? -1 : 1;
            } else {
                cmp = CmpU64(a.memKnown ? a.memWorkingSet : 0,
                             b.memKnown ? b.memWorkingSet : 0);
            }
            break;
        }
        case COL_DISK: {
            if (a.ioKnown != b.ioKnown) {
                unknownLast = true;
                cmp = a.ioKnown ? -1 : 1;
            } else {
                cmp = CmpU64(
                    a.ioKnown ? SatAdd(a.diskReadBytes, a.diskWriteBytes) : 0,
                    b.ioKnown ? SatAdd(b.diskReadBytes, b.diskWriteBytes) : 0);
            }
            break;
        }
        case COL_DURATION:
            // Younger connections first is the useful default (the ones
            // worth investigating), and the ascending flip gives the
            // opposite. An unknown age (0) sorts as oldest, i.e. last when
            // ascending - it is the least actionable value.
            cmp = CmpU64(DurationSeconds(a), DurationSeconds(b));
            break;
        case COL_BANDWIDTH: {
            if (a.bpsKnown != b.bpsKnown) {
                unknownLast = true;
                cmp = a.bpsKnown ? -1 : 1;
            } else {
                cmp = CmpDbl(a.bpsKnown ? (a.rxBps + a.txBps) : 0.0,
                             b.bpsKnown ? (b.rxBps + b.txBps) : 0.0);
            }
            break;
        }
        case COL_GROUPRATE: {
            if (a.groupBpsKnown != b.groupBpsKnown) {
                unknownLast = true;
                cmp = a.groupBpsKnown ? -1 : 1;
            } else {
                cmp = CmpDbl(a.groupBpsKnown ? (a.groupRxBps + a.groupTxBps) : 0.0,
                             b.groupBpsKnown ? (b.groupRxBps + b.groupTxBps) : 0.0);
            }
            break;
        }
        // G6: unknown LAST, for every one of these, li.e every other measured
        // column - so `--sort rtt --asc` puts the genuinely fastest connection
        // at the top instead of the em-dashes. Within the known rows the value
        // is what sorts, which for RTT means ascending is "fastest first" and
        // descending surfaces the slow ones, which is the query someone
        // diagnosing a lag actually wants.
        case COL_RTT: {
            if (a.rttKnown != b.rttKnown) {
                unknownLast = true;
                cmp = a.rttKnown ? -1 : 1;
            } else {
                cmp = CmpInt(a.rttKnown ? static_cast<long long>(a.rttMs) : 0,
                             b.rttKnown ? static_cast<long long>(b.rttMs) : 0);
            }
            break;
        }
        case COL_MINRTT: {
            if (a.rttKnown != b.rttKnown) {
                unknownLast = true;
                cmp = a.rttKnown ? -1 : 1;
            } else {
                cmp = CmpInt(a.rttKnown ? static_cast<long long>(a.minRttMs) : 0,
                             b.rttKnown ? static_cast<long long>(b.minRttMs) : 0);
            }
            break;
        }
        case COL_CWND: {
            if (a.cwndKnown != b.cwndKnown) {
                unknownLast = true;
                cmp = a.cwndKnown ? -1 : 1;
            } else {
                cmp = CmpU64(a.cwndKnown ? a.cwnd : 0, b.cwndKnown ? b.cwnd : 0);
            }
            break;
        }
        case COL_RETRANS: {
            if (a.retransKnown != b.retransKnown) {
                unknownLast = true;
                cmp = a.retransKnown ? -1 : 1;
            } else {
                cmp = CmpU64(a.retransKnown ? a.retransBytes : 0,
                             b.retransKnown ? b.retransBytes : 0);
            }
            break;
        }
        case COL_TLS: {
            // Non-TLS and unknown rows group at the end, the way the other
            // "no reading" columns do, so the secure ones lead.
            const bool sa = a.tls.known && a.tls.secure;
            const bool sb = b.tls.known && b.tls.secure;
            if (sa != sb) { unknownLast = true; cmp = sa ? -1 : 1; }
            else if (sa) {
                cmp = CmpInt(a.tls.protocol, b.tls.protocol);
                if (cmp == 0) cmp = CmpInt(a.tls.cipherSuite, b.tls.cipherSuite);
            }
            break;
        }
        case COL_COUNTRY: cmp = CmpStr(a.country, b.country); break;
        case COL_PINNED: {
            // Pinned first regardless of direction, then by tag, so a
            // bookmarked row never sinks below unbookmarked noise.
            // NOTE: the unknownLast flag is required here, identical to
            // COL_NOTE's precedent below - it keeps the ascending-vs-descending
            // multiplier off the pinned comparison. Without it, sorting
            // descending by the pinned column pushed pinned rows to the
            // bottom, the exact opposite of "pinned first".
            if (a.pinned != b.pinned) { unknownLast = true; cmp = a.pinned ? -1 : 1; }
            else if (a.tag != b.tag) cmp = CmpInt(a.tag, b.tag);
            break;
        }
        case COL_NOTE: {
            // Annotated rows first, for the same reason bookmarks leads: a
            // note is the reader's own annotation and the rows carrying one are
            // the ones being loo.ed for. Within those, alphabetical, so
            // "everything I wrote about vendor X" sorts together.
            const bool na = !a.note.empty();
            const bool nb = !b.note.empty();
            if (na != nb) { unknownLast = true; cmp = na ? -1 : 1; }
            else if (na) cmp = CmpStr(a.lowerNote, b.lowerNote);
            break;
        }
        // F5.1. Sorts by the NUMBER, not by the cell text. The cell is
        // "<ppid> <name>", so a string sort would order "1000 svchost.exe"
        // before "900 wininit.exe" by the leading digit run only by accident -
        // and would order "12 a.exe" after "12.exe" for reasons that have
        // nothing to do with the hierarchy. Rows the snapshot did not cover sort
        // last, like every other "no reading" column.
        case COL_PPID: {
            if (a.ppidKnown != b.ppidKnown) {
                unknownLast = true;
                cmp = a.ppidKnown ? -1 : 1;
            } else if (a.ppidKnown) {
                cmp = CmpInt(a.ppid, b.ppid);
                if (cmp == 0) cmp = CmpStr(a.lowerParent, b.lowerParent);
            }
            break;
        }
        // F5.2. Orders by the enum, which is already in trust order
        // (Untrusted < Low < Medium < High < System < Protected), so sorting
        // ascending on the column puts the least trusted first - the direction
        // a reader wants when auditing. Unknown is last.
        case COL_INTEGRITY: {
            const bool ka = a.integrity != 0 &&
                            a.integrity < static_cast<unsigned>(kIntegrityCount);
            const bool kb = b.integrity != 0 &&
                            b.integrity < static_cast<unsigned>(kIntegrityCount);
            if (ka != kb) { unknownLast = true; cmp = ka ? -1 : 1; }
            else if (ka) cmp = CmpInt(a.integrity, b.integrity);
            break;
        }
        // F5.3. Orders by the enum (Unchecked < Valid < Invalid < Unsigned <
        // Error), then puts the AppContainer-ish detail aside - there is none
        // here, the state is the whole answer. A reviewer sorting descending
        // gets the BAD SIG rows first, which is the point of the highlight.
        case COL_SIGNATURE: {
            const bool ka = a.signature != 0 &&
                            a.signature < static_cast<unsigned>(kSigCount);
            const bool kb = b.signature != 0 &&
                            b.signature < static_cast<unsigned>(kSigCount);
            if (ka != kb) { unknownLast = true; cmp = ka ? -1 : 1; }
            else if (ka) cmp = CmpInt(a.signature, b.signature);
            break;
        }
        default:          cmp = 0; break;
    }
    // Direction-independent: "—" always ends up last (cmp is non-zero here,
    // so the flip below must not run).
    if (unknownLast) return cmp;
    if (cmp == 0) {
        // Deterministic tie-breaker so refreshes never visibly shuffle rows.
        cmp = CmpInt(a.family, b.family);
        if (cmp == 0) cmp = CmpInt(static_cast<long long>(a.protocol),
                                   static_cast<long long>(b.protocol));
        if (cmp == 0) cmp = CmpStr(a.lowerLocal, b.lowerLocal);
        if (cmp == 0) cmp = CmpInt(a.localPort, b.localPort);
        if (cmp == 0) cmp = CmpStr(a.lowerRemote, b.lowerRemote);
        if (cmp == 0) cmp = CmpInt(a.remotePort, b.remotePort);
        if (cmp == 0) cmp = CmpInt(a.pid, b.pid);
        if (cmp == 0) cmp = CmpU64(a.id, b.id);
    }
    return ascending ? cmp : -cmp;
}

// ---- headless row targeting (stage 2.1) -----------------------------------
// Answer "which rows does this command act on?" from data alone, with the
// same filter grammar as the GUI filter box. Empty text matches nothing, so
// a typo'd filter cannot quietly act on the whole machine.
std::vector<size_t> ConnectionStore::SelectByFilter(
        const std::wstring& text) const {
    std::vector<size_t> out;
    if (text.empty()) return out;
    std::vector<FilterClause> prog;
    if (!ParseFilter(text, prog)) return out;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (MatchFilter(rows_[i], prog)) out.push_back(i);
    }
    return out;
}

std::vector<size_t> ConnectionStore::SelectByFilterInView(
        const std::wstring& text) const {
    std::vector<size_t> out;
    if (text.empty()) return out;
    std::vector<FilterClause> prog;
    if (!ParseFilter(text, prog)) return out;
    for (size_t v = 0; v < viewIndex_.size(); ++v) {
        const size_t row = viewIndex_[v];
        if (row < rows_.size() && MatchFilter(rows_[row], prog))
            out.push_back(row);
    }
    return out;
}

const Connection* ConnectionStore::RowForViewIndex(int viewIdx) const {
    if (viewIdx < 0) return nullptr;
    const Connection* c = ViewRow(static_cast<size_t>(viewIdx));
    if (c == nullptr || (c->flags & kRowRemoved)) return nullptr;
    return c;
}

}  // namespace wintcp
