// Commands.cpp
// See Commands.h. All headless; no HWND, no dialogs, no clipboard.

#include "Commands.h"

#include <algorithm>
#include <cwchar>
#include <map>
#include <set>
#include <sstream>
#include <tcpmib.h>

#include "BlockConn.h"
#include "Bookmarks.h"
#include "ColumnsWin.h"
#include "DnsResolver.h"
#include "Elevate.h"
#include "GeoIp.h"
#include "Presets.h"
#include "ProcessInfo.h"
#include "SocketTraffic.h"
#include "StreamCapture.h"
#include "TcpTable.h"
#include "Utils.h"

namespace wintcp {
// Forward: defined with the public serializers below; used by the
// anonymous-namespace list builders above it.
const char* JsonKeyFor(int column);
namespace {

// Grace period KillPid gives a process to exit after WM_CLOSE before falling
// back to TerminateProcess. 3 s is enough for a GUI app to run its normal
// shutdown and short enough that a scripted kill does not stall; the help text
// next to `kill` states this number, so change both together (see A4).
constexpr DWORD kKillGraceMs = 3000;   // B4: dev constant, no --wait flag:
// every switch is API surface, and no workflow needs a custom grace.

// Gap between the two samples `stat` differences into rates. 1 s matches the
// help text and makes the arithmetic trivially readable (delta == per-second);
// a shorter gap would quantise small rates, a longer one would make every
// `stat` call feel hung. B4 may surface this as --sample-ms; until then it is
// one constant, not two call sites.
constexpr DWORD kStatSampleGapMs = 1000;   // B4: dev constant, no --sample-ms:
// delta == per-second keeps the arithmetic readable; shorter quantises.

std::string JsonEscapeA(const std::wstring& s) {
    const std::string u8 = WideToUtf8(s);
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

std::string TsvCell(const std::wstring& w) {
    std::string s = WideToUtf8(w);
    for (char& ch : s) {
        if (ch == '\t' || ch == '\r' || ch == '\n') ch = ' ';
    }
    return s;
}

const int kDefaultCols[] = {COL_PROTO, COL_LOCAL, COL_LPORT, COL_REMOTE,
                            COL_RPORT, COL_STATE, COL_PID, COL_PROCESS,
                            COL_SERVICE, COL_PATH};

// THE refusal wording, exit 3. Every "we will not do that without being told
// twice" answer goes through here (C5), so a script can match one shape.
//
//   <verb>: refused: pass <FLAG> to proceed (or --dry-run to preview).
//
// <FLAG> differs because the remedy differs: --yes for the mutating verbs,
// --force for overwriting an existing named object. They share the exit code
// because they share the meaning - "your intent was not confirmed" - so
// treating them as one kind of message is honest; splitting them into two
// sentences was not.
//
// Deliberately NOT unified with these, because they mean something else:
//   * "kill: refusing PID 4: ..." (exit 2) is a BAD ARGUMENT, not a missing
//     confirmation. --yes does not make PID 4 killable, so telling the user to
//     pass --yes would be a false promise - the exact failure mode D29 was.
//   * "export --limit is not accepted" (exit 2) is the same: the switch does
//     not belong to the verb, and no flag makes it belong.
CommandResult Refused(const std::string& what, const char* flag = "--yes") {
    CommandResult r;
    r.exitCode = kExitRefused;
    r.err = what + ": refused: pass " + flag +
            " to proceed (or --dry-run to preview).\r\n";
    return r;
}

void ApplyListView(ConnectionStore& store, const ListOptions& opt,
                   std::vector<FilterClause>* prog = nullptr) {
    std::vector<FilterClause> local;
    std::vector<FilterClause>* p = prog != nullptr ? prog : &local;
    ViewState v;
    v.filter = opt.filter;
    v.protoMask = opt.protoMask;
    v.stateFilter = opt.stateFilter;
    v.sortColumn = opt.sortColumn;
    v.sortAsc = opt.sortAsc;
    v.grouped = opt.grouped;
    v.ApplyTo(&store, p);
}

std::string DelimitedHeader(const std::vector<int>& cols, char delim,
                            bool rfcCsv) {
    std::string out;
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i != 0) out += delim;
        const std::string title =
            WideToUtf8(ConnectionStore::ColumnTitle(cols[i]));
        out += rfcCsv ? CsvEscapeUtf8(title) : TsvCell(ConnectionStore::ColumnTitle(cols[i]));
    }
    return out;
}


// The default column set for a GROUPED machine-readable output.
//
// WHY IT EXISTS. The normal default set is ten columns and four of them are
// per-connection (local, lport, remote, rport), so keeping that set for a
// grouped CSV would write four arbitrary member addresses under four headers
// that promise otherwise. Every column here is one a group can actually
// answer: identity (pid, process), what kind of traffic (proto), the thing a
// group DOES have a single answer for (state -> the connection count), the
// aggregates (traffic, nettotal), and the per-process stats.
//
// Defined ABOVE ListColumns because that function calls it; the two are one
// idea (the shape-aware default) and splitting them across a declaration just
// makes the reader stitch it back together.
std::vector<int> GroupedDefaultColumns() {
    return {COL_PID, COL_PROCESS, COL_PROTO, COL_STATE,
            COL_TRAFFIC, COL_NETTOTAL, COL_CPU, COL_MEM, COL_DISK};
}

// The column set an output should render: what the user asked for, or the
// default for the output shape.
//
// D27: a GROUPED stream defaults to the group-answerable set rather than the
// flat one. The flat default is ten columns and four of them are
// per-connection, so a grouped CSV would otherwise carry four arbitrary
// member addresses under four headers that promise otherwise - and a header
// row in a data file is a SCHEMA CLAIM, not a label. A human reading the table
// can see the row is a group (its State cell reads "4 connections"); a
// consumer reading a CSV cannot, so the table keeps the representative value
// and the machine shapes do not.
//
// The explicit-`--columns` half of the rule is enforced separately, by
// refusal rather than substitution - see ValidateStreamColumns.
std::vector<int> ListColumns(const ListOptions& opt) {
    if (!opt.columns.empty()) return opt.columns;
    if (opt.grouped && opt.format != "table") return GroupedDefaultColumns();
    return DefaultExportColumns();
}

// D27: refuse a grouped MACHINE-READABLE output that explicitly asks for a
// per-connection column, by name, and say which columns do work.
//
// Why a refusal and not a substitution: silently swapping the user's chosen
// column for a different one would mean `export --columns pid,remote` produced
// a file with no remote column and no error. Why not on `table`: there the row
// is visibly a group (State reads "4 connections"), so one member's address is
// informative rather than misleading, and blanking it would be strictly less
// useful - which is why this lives here and not in GroupColumnText.
//
// Returns an empty string when the request is fine.
std::string ValidateStreamColumns(const ListOptions& opt) {
    if (!opt.grouped || opt.format == "table") return std::string();
    if (opt.columns.empty()) return std::string();   // the default is safe
    std::string bad;
    for (int c : opt.columns) {
        if (!ColumnIsPerConnectionOnly(c)) continue;
        if (!bad.empty()) bad += ", ";
        // ColumnTitle, not the JSON key: this is the exact string the refused
        // file's header row would have carried, so naming it lets the reader
        // see the mismatch they were about to get.
        bad += WideToUtf8(ConnectionStore::ColumnTitle(c));
    }
    if (bad.empty()) return std::string();
    return bad;
}

// ---- aligned "table" rendering (D14) ---------------------------------------
// `format table` is the default human shape, and raw tab stops do not make
// one: header and data cells start at different 8-column stops, so columns
// never line up vertically. The table now pads every cell to its column's
// computed width - text left, numbers right, the split the GUI's column
// list already uses - measured in DISPLAY columns so CJK names, the em-dash
// and the down/up arrows count the way a terminal draws them. csv/tsv keep
// their exact raw delimiters: they are machine shapes. The same job is done
// by `column -t`, docker ps, kubectl and Go's text/tabwriter.

// Display width of one decoded code point: a compact East Asian Width
// approximation (the W/F ranges terminals render double, combining marks
// zero, everything else one). ASCII - the common case - is exact.
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

// Decode the code point at 'i' (UTF-8 from WideToUtf8; a stray byte counts
// as itself) and return how many bytes it spans.
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

size_t DisplayWidth(const std::string& s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = 0;
        i += NextCp(s, i, &cp);
        w += CpWidth(cp);
    }
    return w;
}

// Truncate to at most 'width' display columns, marking the cut with U+2026
// so a shortened cell reads as shortened instead of as the whole value.
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

// The width cap in force for a column under 'opt' (0 = measure from the
// content). A cell a JOIN fills - or the clock ticks - takes its width from
// the cap whenever that source will run: the streamed header is printed
// BEFORE the join, the rows are padded AFTER it, and only a
// source-determined width makes the two passes compute the same table
// character for character (D15).
//
// Country is deliberately absent: an ISO alpha-2 code is 2 columns wide
// against a 7-column header, so its width is the header's in both passes
// without a budget (Columns.h explains when a budget is needed at all).
int ActiveCap(int col, const ListOptions& opt) {
    switch (col) {
        case COL_GROUPRATE:
            return GetColumnStyle(COL_GROUPRATE).cap;  // late-joined (G5)
        case COL_NOTE:
            // 5.5: filled by the bookmark join, which happens after the header
            // is printed - so the width must be the source's, not the
            // content's. Same rule as COL_HOST.
            return GetColumnStyle(COL_NOTE).cap;
        case COL_DURATION:
            return GetColumnStyle(COL_DURATION).cap;  // clock-filled: always
        case COL_HOST:
            return opt.dns ? GetColumnStyle(COL_HOST).cap : 0;
        case COL_RX:
        case COL_TX:
        case COL_NETTOTAL:
        case COL_TRAFFIC:
        case COL_BANDWIDTH:
            return opt.traffic ? GetColumnStyle(col).cap : 0;
        case COL_RTT:
        case COL_MINRTT:
        case COL_CWND:
        case COL_RETRANS:
            // G6: late-joined exactly like the byte counters - the kernel fills
            // these during the socket scan, so with no scan every cell is "—" and
            // the two width passes still have to agree on the same table. Same
            // rule as BANDWIDTH: the header is streamed before the join, so the
            // column takes the source's budget rather than its content's.
            return opt.traffic ? GetColumnStyle(col).cap : 0;
        default:
            return 0;
    }
}

std::vector<bool> RightFlags(const std::vector<int>& cols) {
    std::vector<bool> out;
    out.reserve(cols.size());
    for (int c : cols) out.push_back(GetColumnStyle(c).right);
    return out;
}

std::vector<std::string> HeaderCells(const std::vector<int>& cols) {
    std::vector<std::string> out;
    out.reserve(cols.size());
    for (int c : cols) {
        out.push_back(WideToUtf8(ConnectionStore::ColumnTitle(c)));
    }
    return out;
}

// Column widths for the aligned table: the widest of the header title, the
// active cap and every cell the table can print.
//
// Measured over the store's FLAT rows (never the view) so the result is a
// pure function of (rows, opt): the streamed header scans before the
// enrichment joins, the renderer scans after them, and the two must agree.
// Ghost rows and rows the filter hides only make the scan a superset of
// what the view shows - a column can come out a little wider than needed,
// never too narrow.
std::vector<size_t> ScanTableWidths(const ConnectionStore& store,
                                    const ListOptions& opt) {
    const std::vector<int> cols = ListColumns(opt);
    std::vector<size_t> w(cols.size(), 0);
    for (size_t i = 0; i < cols.size(); ++i) {
        w[i] = DisplayWidth(WideToUtf8(ConnectionStore::ColumnTitle(cols[i])));
        const int cap = ActiveCap(cols[i], opt);
        if (cap > 0 && static_cast<size_t>(cap) > w[i]) w[i] = static_cast<size_t>(cap);
    }
    wchar_t buf[kMaxColumnText] = {0};
    const std::vector<Connection>& rows = store.Rows();
    for (size_t r = 0; r < rows.size(); ++r) {
        for (size_t i = 0; i < cols.size(); ++i) {
            if (ActiveCap(cols[i], opt) > 0) continue;
            ConnectionStore::GetColumnText(rows[r], cols[i], buf, kMaxColumnText);
            const size_t cw = DisplayWidth(TsvCell(buf));
            if (cw > w[i]) w[i] = cw;
        }
    }
    if (opt.grouped) {
        // Cells the flat scan never sees: "N connections" under State,
        // "TCP+UDP" under Proto, a "PID n" placeholder name. Projected here
        // exactly the way ConnectionStore::RebuildView projects them, from
        // the same flat rows - so a group's cells are counted before (the
        // streamed header) and after (the rows) the joins with the same
        // result. Group columns that fall through to the representative
        // connection are flat cells and were counted above already.
        std::vector<GroupRow> proj;
        proj.reserve(rows.size());
        for (const Connection& c : rows) {
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
        const std::vector<ProcessGroup> groups = GroupByProcess(proj);
        for (const ProcessGroup& g : groups) {
            for (size_t i = 0; i < cols.size(); ++i) {
                if (ActiveCap(cols[i], opt) > 0) continue;
                wchar_t gb[kMaxColumnText] = {0};
                if (!GroupColumnText(g, cols[i], gb, kMaxColumnText)) continue;
                const size_t cw = DisplayWidth(TsvCell(gb));
                if (cw > w[i]) w[i] = cw;
            }
        }
    }
    return w;
}

// Widths for a table whose cells are all present before printing (ps,
// bookmarks): header vs content only, no late-join caps in play.
std::vector<size_t> ContentWidths(
    const std::vector<std::string>& header,
    const std::vector<std::vector<std::string>>& rows) {
    std::vector<size_t> w(header.size(), 0);
    for (size_t i = 0; i < header.size(); ++i) w[i] = DisplayWidth(header[i]);
    for (const std::vector<std::string>& cells : rows) {
        for (size_t i = 0; i < cells.size() && i < w.size(); ++i) {
            const size_t cw = DisplayWidth(cells[i]);
            if (cw > w[i]) w[i] = cw;
        }
    }
    return w;
}

// One padded table. The last column is never right-padded, so no line ends
// in whitespace (trailing spaces are invisible noise in a terminal and byte
// noise in a redirect).
std::string RenderAlignedLines(
    const std::vector<std::string>& header,
    const std::vector<std::vector<std::string>>& rows,
    const std::vector<bool>& right, const std::vector<size_t>& widths,
    bool includeHeader) {
    auto line = [&](const std::vector<std::string>& cells) {
        std::string out;
        for (size_t i = 0; i < cells.size() && i < widths.size(); ++i) {
            const size_t w = widths[i];
            const std::string cell = TruncateToWidth(cells[i], w);
            const size_t pad = w - DisplayWidth(cell);
            if (right[i]) {
                out.append(pad, ' ');
                out += cell;
            } else {
                out += cell;
                if (i + 1 < cells.size()) out.append(pad, ' ');
            }
            if (i + 1 < cells.size()) out += "  ";
        }
        // The last column is never right-padded, and an empty trailing cell
        // (no Path, no Service) must not leave the separator in place either:
        // no line ever ends in whitespace. Stripping here cannot lose
        // alignment - everything stripped is beyond the final non-empty cell.
        while (!out.empty() && out.back() == ' ') out.pop_back();
        out += "\r\n";
        return out;
    };
    std::string out;
    if (includeHeader) out += line(header);
    for (const std::vector<std::string>& cells : rows) out += line(cells);
    return out;
}

// The header exactly as RenderList would print it - the streamed copy for
// D15. Same functions, same widths as the row pass below.
std::string ListHeaderLine(const ConnectionStore& store,
                           const ListOptions& opt) {
    const std::vector<int> cols = ListColumns(opt);
    if (opt.format == "table") {
        return RenderAlignedLines(HeaderCells(cols), {}, RightFlags(cols),
                                  ScanTableWidths(store, opt), true);
    }
    const bool csv = (opt.format == "csv");
    return DelimitedHeader(cols, csv ? ',' : '\t', csv) + "\r\n";
}

std::string DelimitedList(const ConnectionStore& store, const ListOptions& opt,
                           bool skipHeader = false) {
    const bool table = (opt.format == "table");
    const bool csv = (opt.format == "csv");
    const char delim = csv ? ',' : '\t';
    const std::vector<int> cols = ListColumns(opt);
    if (!store.Grouped()) {
        std::vector<Connection> rows;
        rows.reserve(store.View().size());
        for (size_t v = 0; v < store.View().size(); ++v) {
            if (opt.limit != 0 && rows.size() >= opt.limit) break;
            const Connection* c = store.ViewRow(v);
            if (c != nullptr) rows.push_back(*c);
        }
        if (table) {
            std::vector<std::vector<std::string>> cells;
            cells.reserve(rows.size());
            wchar_t buf[kMaxColumnText] = {0};
            for (const Connection& c : rows) {
                std::vector<std::string> row;
                row.reserve(cols.size());
                for (int col : cols) {
                    ConnectionStore::GetColumnText(c, col, buf, kMaxColumnText);
                    row.push_back(TsvCell(buf));
                }
                cells.push_back(std::move(row));
            }
            return RenderAlignedLines(HeaderCells(cols), cells, RightFlags(cols),
                                      ScanTableWidths(store, opt), !skipHeader);
        }
        return RenderDelimitedRows(rows, cols, delim, csv, !skipHeader);
    }
    // Grouped: one row per process group. A column the group has no answer
    // for falls through to the representative connection - the GUI has
    // always rendered it that way, and the CLI used to print the cell
    // BLANK (a table whose columns are half empty is exactly the messy
    // output D14 exists to fix).
    std::vector<std::vector<std::string>> cells;
    size_t shown = 0;
    for (size_t v = 0; v < store.View().size(); ++v) {
        if (opt.limit != 0 && shown >= opt.limit) break;
        const ProcessGroup* g = store.ViewGroup(v);
        if (g == nullptr) continue;
        const Connection* rep = store.ViewRow(v);
        std::vector<std::string> row;
        row.reserve(cols.size());
        for (int col : cols) {
            wchar_t gb[kMaxColumnText] = {0};
            if (!GroupColumnText(*g, col, gb, kMaxColumnText)) {
                if (rep == nullptr) {
                    row.push_back(std::string());
                    continue;
                }
                ConnectionStore::GetColumnText(*rep, col, gb, kMaxColumnText);
            }
            row.push_back(TsvCell(gb));
        }
        cells.push_back(std::move(row));
        ++shown;
    }
    if (table) {
        return RenderAlignedLines(HeaderCells(cols), cells, RightFlags(cols),
                                  ScanTableWidths(store, opt), !skipHeader);
    }
    std::string out;
    if (!skipHeader) {
        out = DelimitedHeader(cols, delim, csv);
        out += "\r\n";
    }
    for (const std::vector<std::string>& row : cells) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i != 0) out += delim;
            out += csv ? CsvEscapeUtf8(row[i]) : row[i];
        }
        out += "\r\n";
    }
    return out;
}

std::string JsonList(const ConnectionStore& store, const ListOptions& opt) {
    const std::vector<int> cols = ListColumns(opt);
    if (!store.Grouped()) {
        std::vector<Connection> rows;
        rows.reserve(store.View().size());
        for (size_t v = 0; v < store.View().size(); ++v) {
            if (opt.limit != 0 && rows.size() >= opt.limit) break;
            const Connection* c = store.ViewRow(v);
            if (c != nullptr) rows.push_back(*c);
        }
        return RenderJsonRows(rows, cols, /*extraHostname=*/false,
                              opt.jsonLines);
    }
    // Grouped: same two shapes, so the same three edits as above - no
    // brackets, "\n" between records, one "\n" after the last.
    std::string out = opt.jsonLines ? "" : "[";
    bool first = true;
    size_t shown = 0;
    for (size_t v = 0; v < store.View().size(); ++v) {
        if (opt.limit != 0 && shown >= opt.limit) break;
        const ProcessGroup* g = store.ViewGroup(v);
        if (g == nullptr) continue;
        const Connection* rep = store.ViewRow(v);
        if (!first) out += (opt.jsonLines ? "\n" : ",");
        out += (opt.jsonLines ? "{" : "\n  {");
        bool needComma = false;
        for (size_t i = 0; i < cols.size(); ++i) {
            const char* key = JsonKeyFor(cols[i]);
            if (key == nullptr) continue;
            wchar_t gb[kMaxColumnText] = {0};
            // Same fall-through as the table/CSV branch (and the GUI): a
            // group column with no aggregate answer shows the representative
            // connection's cell, not an empty string.
            if (!GroupColumnText(*g, cols[i], gb, kMaxColumnText)) {
                if (rep == nullptr) continue;
                ConnectionStore::GetColumnText(*rep, cols[i], gb,
                                               kMaxColumnText);
            }
            if (needComma) out += ",";
            needComma = true;
            out += "\"";
            out += key;
            out += "\":\"";
            out += JsonEscapeA(gb);
            out += "\"";
        }
        out += "}";
        first = false;
        ++shown;
    }
    if (opt.jsonLines) {
        if (!out.empty()) out += "\n";
    } else {
        out += "\n]";
    }
    return out;
}


}  // namespace

// ---- shared serializers (public: Commands.h) --------------------------
std::string RenderDelimitedRows(const std::vector<Connection>& rows,
                                const std::vector<int>& cols, char delim,
                                bool rfcCsv, bool includeHeader) {
    std::string out;
    if (includeHeader) {
        out = DelimitedHeader(cols, delim, rfcCsv);
        out += "\r\n";
    }
    wchar_t buf[kMaxColumnText] = {0};
    for (const Connection& c : rows) {
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i != 0) out += delim;
            ConnectionStore::GetColumnText(c, cols[i], buf, kMaxColumnText);
            const std::string cell = WideToUtf8(buf);
            if (rfcCsv) {
                out += CsvEscapeUtf8(cell);
            } else {
                std::string t = cell;
                for (char& ch : t) {
                    if (ch == '\t' || ch == '\r' || ch == '\n') ch = ' ';
                }
                out += t;
            }
        }
        out += "\r\n";
    }
    return out;
}

const char* JsonKeyFor(int column) {
    // Frozen lowercase keys: the same names the legacy --json emitter and
    // the GUI file export have always written, so every JSON artifact from
    // this binary is interchangeable.
    switch (column) {
        case COL_PROTO: return "proto";
        case COL_LOCAL: return "local";
        case COL_LPORT: return "lport";
        case COL_REMOTE: return "remote";
        case COL_RPORT: return "rport";
        case COL_STATE: return "state";
        case COL_PID: return "pid";
        case COL_PROCESS: return "process";
        case COL_SERVICE: return "service";
        case COL_HOST: return "host";
        case COL_PATH: return "path";
        case COL_TRAFFIC: return "traffic";
        case COL_RX: return "rx";
        case COL_TX: return "tx";
        case COL_NETTOTAL: return "nettotal";
        case COL_CPU: return "cpu";
        case COL_MEM: return "mem";
        case COL_DISK: return "disk";
        case COL_DURATION: return "duration";
        case COL_BANDWIDTH: return "bandwidth";
        // G6: the `ss -i` names, so someone comparing the two is looking for
        // the same strings. JSON keys are lower case by convention (every other
        // key here is), which is why they differ from the CELL HEADERS - those
        // are "RTT" / "Min RTT" / "Cwnd" / "Retrans", and the two conventions
        // are deliberately allowed to differ.
        case COL_RTT: return "rtt";
        case COL_MINRTT: return "minrtt";
        case COL_CWND: return "cwnd";
        case COL_RETRANS: return "retrans";
        // G5. Named "procspeed" rather than "groupspeed" on purpose: the value
        // is a PROCESS total, and it is populated on flat rows too, so a key
        // that said "group" would mislead anyone reading a non-grouped JSON
        // document.
        case COL_GROUPRATE: return "procspeed";
        case COL_NOTE: return "note";
        case COL_TLS: return "tls";
        case COL_COUNTRY: return "country";
        case COL_PINNED: return "pinned";
        // F5.1/F5.2/F5.3. JSON keys match the --columns spellings, not the CELL
        // HEADERS, for the same reason the ss -i keys do: the header says
        // "Parent" and "Integrity", and the machine-readable name is the flat
        // one a jq author would type.
        case COL_PPID: return "ppid";
        case COL_INTEGRITY: return "integrity";
        case COL_SIGNATURE: return "signature";
        default: return nullptr;
    }
}

// 'lines' selects NDJSON over the JSON array - see ListOptions::jsonLines.
// One renderer for both, because everything else about the two shapes (the
// keys, the escaping, the limit, the group fall-through) is identical, and
// two renderers would drift on exactly the part nobody looks at. The only
// differences are the brackets, the separator and the indent: in lines mode
// a record is separated by "\n" and the last one is terminated by "\n", so
// appending the next --watch tick's output CONTINUES the stream instead of
// starting a second array inside the first.
std::string RenderJsonRows(const std::vector<Connection>& rows,
                           const std::vector<int>& cols, bool extraHostname,
                           bool lines) {
    std::string out = lines ? "" : "[";
    bool first = true;
    wchar_t buf[kMaxColumnText] = {0};
    for (const Connection& c : rows) {
        if (!first) out += (lines ? "\n" : ",");
        out += (lines ? "{" : "\n  {");
        bool needComma = false;
        for (size_t i = 0; i < cols.size(); ++i) {
            const char* key = JsonKeyFor(cols[i]);
            if (key == nullptr) continue;
            if (needComma) out += ",";
            needComma = true;
            out += "\"";
            out += key;
            out += "\":\"";
            ConnectionStore::GetColumnText(c, cols[i], buf, kMaxColumnText);
            out += JsonEscapeA(buf);
            out += "\"";
        }
        if (extraHostname) {
            if (needComma) out += ",";
            out += "\"hostname\":\"" + JsonEscapeA(c.hostname) + "\"";
        }
        out += "}";
        first = false;
    }
    if (lines) {
        // Terminate the last record - NDJSON defines each line as a complete
        // value, and the trailing newline is what lets the next one follow.
        // An EMPTY result stays empty: a run that matched no rows writes
        // nothing at all rather than one blank line a reader would try to
        // parse as an object.
        if (!out.empty()) out += "\n";
    } else {
        out += "\n]";
    }
    return out;
}

std::vector<int> DefaultExportColumns() {
    return std::vector<int>(kDefaultCols, kDefaultCols + 10);
}

bool BuildStoreSnapshot(SnapshotSource& source, ConnectionStore& store,
                        bool procStats, bool resolveDns, bool geoIp,
                        const wchar_t* geoIpPath, std::wstring* error) {
    SnapshotOptions so;
    so.resolveProcesses = true;
    so.procStats = procStats;
    so.resolveDns = resolveDns;
    so.geoIp = geoIp;
    so.geoIpPath = geoIpPath;
    Snapshot snap;
    if (!source.Build(so, &snap)) {
        if (error != nullptr) *error = snap.error;
        return false;
    }
    for (const auto& kv : snap.procStats) {
        // Join live stats into the snapshot rows before the diff, so the
        // store's first view already carries CPU/mem/disk.
        (void)kv;
    }
    store.ReplaceSnapshot(std::move(snap.rows));
    // Join services + procStats that Snapshot produced alongside rows.
    for (const auto& kv : snap.services) {
        (void)kv;  // already joined inside SnapshotSource::Build
    }
    for (const auto& kv : snap.procStats) {
        store.SetProcStats(kv.first, kv.second.cpuPct, kv.second.memKnown,
                           kv.second.memWs, kv.second.memPrivate,
                           kv.second.ioKnown, kv.second.ioRead,
                           kv.second.ioWrite);
    }
    // Join bookmarks on every snapshot (one cheap HKCU read): the pinned
    // column and tag then work in the CLI exactly as in the GUI, which
    // joins them per refresh in RefreshBookmarkMarks.
    //
    // Address AND port AND note are all carried: the port is the bookmark's
    // identity (matching on the address alone painted the wrong conversation -
    // see ConnectionStore::JoinBookmarks) and the note is what makes
    // `note:<text>` a working filter instead of a literal string search.
    {
        size_t unreadable = 0;
        const std::vector<Bookmark> all = Bookmarks::List(&unreadable);
        (void)unreadable;  // corrupt entries are skipped, same as the GUI
        std::vector<BookmarkMark> known;
        known.reserve(all.size());
        for (const Bookmark& b : all) {
            BookmarkMark m;
            m.address = b.address;
            m.port = b.port;
            m.tag = b.tag;
            m.note = b.note;
            known.push_back(std::move(m));
        }
        store.JoinBookmarks(known);
    }
    return true;
}

// Process-lifetime socket sampler for --traffic. Deliberately leaked (one
// object per process, never freed): a scan thread wedged in SIO_TCP_INFO
// holds a pointer to it and cannot be cancelled - the same documented
// trade-off as the GUI's sampler. Reused across polls so the learned
// socket-type filter persists and later scans stay fast.
SocketTrafficSampler*& TrafficSamplerSlot() {
    static SocketTrafficSampler* s = nullptr;
    return s;
}

SocketTrafficSampler& TrafficSampler() {
    SocketTrafficSampler*& s = TrafficSamplerSlot();
    if (s == nullptr) s = new SocketTrafficSampler();
    return *s;
}

void JoinDnsAddrs(ConnectionStore& store,
                  const std::vector<std::wstring>& addrs) {
    std::set<std::wstring> seen;
    for (const std::wstring& a : addrs) {
        if (a.empty() || !seen.insert(a).second) continue;
        const std::wstring host = DnsResolver::Lookup(a);
        if (!host.empty()) store.SetHostname(a, host);
    }
}

// A remote endpoint plus its family for GeoIP lookup (values, not row
// pointers: joins mutate the store).
struct GeoTarget {
    std::wstring address;
    bool ipv6 = false;
};

bool JoinGeoAddrs(ConnectionStore& store, GeoIpDatabase& db,
                  const std::vector<GeoTarget>& targets) {
    for (const GeoTarget& t : targets) {
        std::wstring code;
        if (t.ipv6) {
            IN6_ADDR a6 = {};
            if (::InetPtonW(AF_INET6, t.address.c_str(), &a6) != 1) continue;
            code = db.LookupV6(a6.s6_addr);
        } else {
            IN_ADDR a4 = {};
            if (::InetPtonW(AF_INET, t.address.c_str(), &a4) != 1) continue;
            code = db.LookupV4(ntohl(a4.S_un.S_addr));
        }
        if (!code.empty()) store.SetCountry(t.address, code);
    }
    return true;
}

void JoinTrafficPids(ConnectionStore& store, const std::vector<DWORD>& pids) {
    if (pids.empty()) return;
    const std::map<DWORD, PidTraffic> totals = TrafficSampler().Sample(pids);
    for (const auto& kv : totals)
        store.SetTraffic(kv.first, kv.second.rx, kv.second.tx);
    // Same scan, second reading: the kernel's own connection age. A single-shot
    // `list --traffic` then reports how old each connection REALLY is, instead
    // of 0s for everything (there is no first-seen to fall back on). Costs no
    // extra handle-table pass - the ages come from the sockets just read.
    const std::vector<SocketAge> ages = TrafficSampler().Ages(pids);
    store.ApplyKernelAges(ages);
    // ...and the same sockets' own byte counters, which is the only source
    // allowed to mark a row perRowBytes. Without this the Speed column and
    // `speed:` could never fire: nothing in the tree set the flag, so the
    // column read an em-dash on every tick of every watch while `help list`
    // advertised it. Applied AFTER SetTraffic on purpose - a per-socket total
    // must win over the per-PID total for the rows it covers, because it is
    // the more specific fact about the same cell.
    const std::vector<SocketBytes> bytes = TrafficSampler().Bytes(pids);
    store.ApplySocketBytes(bytes);
    // G6: `ss -i` for Windows, from the SAME TCP_INFO_v0 reading as the bytes
    // and ages above - no extra handle duplication, no extra ioctl, no extra
    // pass over the handle table. RTT, minimum RTT, congestion window and
    // retransmitted bytes are per-socket facts, so this joins on the 4-tuple
    // exactly as ApplySocketBytes does and leaves a duplicate 4-tuple's second
    // row blank rather than inventing a share.
    const std::vector<SocketTcpInfo> tcpInfo = TrafficSampler().TcpInfo(pids);
    store.ApplySocketTcpInfo(tcpInfo);
    // Only now are all the readings in place (the previous sample was carried
    // forward by ReplaceSnapshot, this tick's counters were just written), so
    // both the per-connection and the per-process rate can be computed. See
    // ConnectionStore::ComputeRates for why this cannot happen earlier.
    (void)store.ComputeRates();
}

// What the renderers will print for these options (remote addresses, GeoIP
// targets and PIDs as VALUES, never row pointers: the joins below mutate
// the store, which must not invalidate anything being iterated).
struct PrintSelection {
    std::vector<std::wstring> remotes;   // in view order, dedup not needed
    std::vector<GeoTarget> geoTargets;   // distinct remotes with family
    std::vector<DWORD> pids;             // distinct PIDs
};

// A clause on a JOINED column (tx/rx/net need --traffic, country needs
// --db, host needs --dns) cannot be evaluated before its join: the column is
// still empty, the clause rejects every row, and the join then has nothing
// left to fill - a filter that can only ever answer "no match". When the
// filter contains such a clause the candidate set is therefore the rows
// matching the OTHER clauses; the join fills those, and RenderList applies
// the full filter afterwards against the filled values.
//
// DURATION belongs here for the same reason. With --traffic the kernel's own
// connection age is one of the joined values: ApplyKernelAges backdates
// firstSeenTick, and the Duration column reads it. Before that join the only
// age a row has is the store's first-seen clock, which for a one-shot `list`
// is "right now" - so every row reads 0s. Pre-filtering on `duration:1h`
// therefore rejected the whole table, sel.pids came out EMPTY, no PID was
// ever sampled, and the filter could only ever answer "no rows" while the
// same rows were about to print "6d 8h". Measured: every row reached
// MatchClause with firstSeen == nowTick, i.e. secs == 0.
// G6: the four TCP_INFO fields join in exactly like the byte counters - they
// are filled by the socket scan - so a clause on any of them must be treated
// as enrichment-dependent or it is evaluated against an empty column.
//
// Measured consequence of getting this wrong, which is why it is called out
// rather than left implicit: `--filter "cwnd:"` printed 0 rows while the same
// command WITHOUT the filter showed a dozen rows with real congestion windows.
// The pre-join pass filtered on the empty column, so `sel.pids` came out EMPTY,
// so no PID was ever sampled, so the filter could only ever answer "no match" -
// and the columns it was asking about were about to be filled on rows that had
// already been discarded.
//
// SPEED was the last field added here, and it looked exempt for a reason that
// turns out not to be one: it needs two snapshots, so it is empty on the first
// and "would not have matched anyway". That only describes tick 1. The view is
// filtered BEFORE the join, the filtered view is what yields sel.pids, and
// JoinTrafficPids returns at once on an empty PID set - so the scan that would
// have filled the column on tick 2 never runs, and every tick after it is
// filtered against the same empty column. The column stays empty BECAUSE the
// filter is present, which reads to the user as "no row matches".
// Measured: `list --traffic --watch 1 --count 2 --filter "speed:0"` printed no
// data rows on either tick, while the same command WITHOUT the filter printed
// `idle` on the second tick.
bool DependsOnEnrichment(FilterField f) {
    return f == FilterField::Rx || f == FilterField::Tx ||
           f == FilterField::Net || f == FilterField::Country ||
           f == FilterField::Host || f == FilterField::Duration ||
           f == FilterField::Rtt || f == FilterField::MinRtt ||
           f == FilterField::Cwnd || f == FilterField::Retrans ||
           f == FilterField::Speed ||
           // F5.3 only. Ppid, Parent and Integrity are deliberately ABSENT:
           // they are filled by the process resolver during the snapshot
           // itself, which runs before any filter is applied, so a clause on
           // one of them sees a populated column and must not be deferred.
           // Marking them enrichment-dependent would be actively harmful - the
           // pre-join path would filter on the still-empty column, produce an
           // empty view, and skip the resolver, so the filter could only ever
           // answer "no match" for the same reason `cwnd:` did.
           f == FilterField::Signature;
}

// True when any clause of 'filter' needs an enrichment join first. Negated
// clauses count: they compare the same joined value.
bool HasEnrichmentClause(const std::wstring& filter) {
    if (filter.empty()) return false;
    std::vector<FilterClause> all;
    ParseFilter(filter, all);
    for (const FilterClause& cl : all)
        if (DependsOnEnrichment(cl.field)) return true;
    return false;
}

// 'filter' with every enrichment-dependent clause removed. Empty means "no
// restriction beyond the other switches", which is the widest candidate set
// the pre-pass can legitimately use.
std::vector<FilterClause> PreJoinClauses(const std::wstring& filter) {
    std::vector<FilterClause> out;
    if (filter.empty()) return out;
    std::vector<FilterClause> all;
    ParseFilter(filter, all);
    for (const FilterClause& cl : all)
        if (!DependsOnEnrichment(cl.field)) out.push_back(cl);
    return out;
}

// The switch each enrichment-dependent clause needs to mean anything. When
// that switch is off the joined column stays empty forever and the clause can
// only ever answer "no match" - measured: `list --filter host:easeus` printed
// a header, rc 0, and NOTHING on stderr, so the caller had no way to learn
// that the filter was being asked a question its own switches left
// unanswerable. This names the switch instead. Advisory only: never fatal,
// never on stdout (D4), never under --quiet (quiet = zero bytes).
//
// DURATION is deliberately absent: in --watch the persistent store's own
// clock ages rows with no --traffic involved, so "add --traffic" would be a
// lie there; in a one-shot the Duration column already shows the 0s the
// filter is failing on, so the empty match is visible in the row itself.
std::string MissingEnrichmentAdvice(const ListOptions& opt) {
    if (opt.quiet) return {};
    std::string out;
    if (!opt.filter.empty()) {
        std::vector<FilterClause> all;
        ParseFilter(opt.filter, all);
        bool needDns = false, needGeo = false, needTraffic = false;
        for (const FilterClause& cl : all) {
            if (!DependsOnEnrichment(cl.field)) continue;
            if (cl.field == FilterField::Host) {
                if (!opt.dns) needDns = true;
            } else if (cl.field == FilterField::Country) {
                if (opt.geoIpPath.empty()) needGeo = true;
            } else if (cl.field != FilterField::Duration) {
                if (!opt.traffic) needTraffic = true;
            }
        }
        if (needDns)
            out += "filter: \"host:\" matches reverse-DNS names; add --dns "
                   "(blocking; resolves every candidate remote).\r\n";
        if (needGeo)
            out += "filter: \"country:\" matches GeoIP codes; add --db "
                   "<path to a .mmdb database>.\r\n";
        if (needTraffic) {
            // Name the fields the user actually typed where possible. A bare
            // "add --traffic" for `rtt:100` would be true but unhelpful - the
            // reader has to work out which of seven traffic-dependent fields
            // their query is about.
            std::string named;
            auto addName = [&named](const char* n) {
                if (!named.empty()) named += ", ";
                named += n;
            };
            bool anyRtt = false, anyByte = false, anySpeed = false, anySig = false;
            for (const FilterClause& cl : all) {
                if (!DependsOnEnrichment(cl.field)) continue;
                switch (cl.field) {
                    case FilterField::Rtt:    addName("\"rtt:\""); anyRtt = true; break;
                    case FilterField::MinRtt: addName("\"minrtt:\""); anyRtt = true; break;
                    case FilterField::Cwnd:   addName("\"cwnd:\""); anyByte = true; break;
                    case FilterField::Retrans:addName("\"retrans:\""); anyByte = true; break;
                    case FilterField::Rx:     addName("\"rx:\""); anyByte = true; break;
                    case FilterField::Tx:     addName("\"tx:\""); anyByte = true; break;
                    case FilterField::Net:    addName("\"net:\""); anyByte = true; break;
                    // Speed is not a "traffic total" and must not be described
                    // as one: it is a RATE, and a rate takes two samples, so
                    // naming --traffic alone would send the reader into a
                    // second silent-empty run. Both facts go in the note.
                    case FilterField::Speed:  addName("\"speed:\""); anySpeed = true; break;
                    // F5.3. Its own note, and NOT the traffic wording: the
                    // signature verdict is read during the snapshot rather than
                    // by the socket scan, so telling the reader to add
                    // --traffic would send them down a switch that cannot
                    // possibly change the answer.
                    case FilterField::Signature:
                        addName("\"signature:\"");
                        anySig = true;
                        break;
                    default: break;
                }
            }
            // Signature is checked FIRST, and separately from the rest: the
            // advice is a sentence about one switch, and a filter mixing
            // `signature:` with `rx:` is two questions that need two answers.
            if (anySig) {
                out += "filter: " + named +
                       " match Authenticode verdicts, which are not read unless "
                       "you ask for them; add --signatures.\r\n";
            } else if (anyRtt) {
                out += "filter: " + named +
                       " match TCP congestion state read from the kernel's "
                       "SIO_TCP_INFO; add --traffic.\r\n";
            } else if (anySpeed) {
                out += "filter: " + named +
                       " is a rate between two samples of the same socket; add "
                       "--traffic and re-run with --watch 1 --count 2.\r\n";
            } else if (!named.empty()) {
                out += "filter: " + named +
                       " match traffic totals; add --traffic.\r\n";
            }
        }
    }
    // The columns half (D13): a printed enrichment column whose switch is off
    // is the same silent-empty failure this advice exists to prevent for
    // filters - every row comes out blank and nothing says why. Measured:
    // `list --columns rx` printed a full column of em-dashes with no
    // explanation.
    // Same contract as the filter half: stderr only, never fatal, nothing
    // under --quiet (handled by the guard above).
    //
    // Speed/TLS are deliberately absent: Bandwidth only ever fills in a
    // --watch poll (it is a delta between samples), so "add --traffic" would
    // not be enough of an answer, and no list switch decodes TLS at all.
    bool colHost = false, colGeo = false, colSig = false;
    std::string colTraffic;
    auto addColName = [](std::string& s, const char* n) {
        if (!s.empty()) s += ", ";
        s += n;
    };
    for (int c : ListColumns(opt)) {
        if (c == COL_HOST) colHost = true;
        else if (c == COL_COUNTRY) colGeo = true;
        else if (c == COL_RX) addColName(colTraffic, "\"rx\"");
        else if (c == COL_TX) addColName(colTraffic, "\"tx\"");
        else if (c == COL_NETTOTAL) addColName(colTraffic, "\"nettotal\"");
        else if (c == COL_TRAFFIC) addColName(colTraffic, "\"traffic\"");
        // G6: all four are filled by the same socket scan as the byte counters,
        // so "add --traffic" is exactly as true for them as for "rx".
        else if (c == COL_RTT)     addColName(colTraffic, "\"rtt\"");
        else if (c == COL_MINRTT)  addColName(colTraffic, "\"minrtt\"");
        else if (c == COL_CWND)    addColName(colTraffic, "\"cwnd\"");
        else if (c == COL_RETRANS) addColName(colTraffic, "\"retrans\"");
        // F5.3, and NOT folded into colTraffic: this column is empty because the
        // trust provider was never asked, which is a different problem from the
        // socket scan not having run, and the fix is a different switch.
        else if (c == COL_SIGNATURE) colSig = true;
    }
    if (colHost && !opt.dns)
        out += "column: \"host\" without --dns: reverse DNS never ran, so the "
               "cell stays empty; add --dns (blocking; resolves every "
               "candidate remote).\r\n";
    if (colGeo && opt.geoIpPath.empty())
        out += "column: \"country\" without --db: GeoIP never ran, so the cell "
               "stays empty; add --db <path to a .mmdb database>.\r\n";
    if (colSig && !opt.signatures)
        out += "column: \"signature\" without --signatures: WinVerifyTrust was "
               "never called, so the cell is \"—\" rather than a verdict; add "
               "--signatures (slow; paid once per distinct binary).\r\n";
    if (!colTraffic.empty() && !opt.traffic)
        out += "column: " + colTraffic +
               " without --traffic: the socket scan never ran, so every value "
               "is \"—\"; add --traffic.\r\n";
    return out;
}

PrintSelection SelectedForPrint(ConnectionStore& store,
                                const ListOptions& opt) {
    PrintSelection sel;
    std::set<std::wstring> seenAddr;
    std::set<DWORD> seenPid;

    if (HasEnrichmentClause(opt.filter)) {
        // Pre-join path. The view would be filtered by the very columns the
        // join has not filled yet, so candidates come from the flat rows with
        // those clauses removed. Traffic is one bounded scan over the PID
        // set and GeoIP is a memory-mapped lookup, so covering every
        // candidate is cheap; DNS is blocking, so it honours --limit unless a
        // host: clause makes resolving mandatory.
        const std::vector<FilterClause> pre = PreJoinClauses(opt.filter);
        bool needsHost = false;
        {
            std::vector<FilterClause> all;
            ParseFilter(opt.filter, all);
            for (const FilterClause& cl : all)
                if (cl.field == FilterField::Host) needsHost = true;
        }
        for (const Connection& c : store.Rows()) {
            if (c.flags & kRowRemoved) continue;
            if (!pre.empty() && !MatchFilter(c, pre)) continue;
            if (seenPid.insert(c.pid).second) sel.pids.push_back(c.pid);
            if (c.remoteAddress.empty()) continue;
            if (seenAddr.insert(c.remoteAddress).second) {
                GeoTarget t;
                t.address = c.remoteAddress;
                t.ipv6 = (c.family == AF_INET6);
                sel.geoTargets.push_back(t);
            }
            if (needsHost || opt.limit == 0 || sel.remotes.size() < opt.limit)
                sel.remotes.push_back(c.remoteAddress);
        }
        return sel;
    }

    std::vector<FilterClause> prog;
    ViewState v;
    v.filter = opt.filter;
    v.protoMask = opt.protoMask;
    v.stateFilter = opt.stateFilter;
    v.sortColumn = opt.sortColumn;
    v.sortAsc = opt.sortAsc;
    v.grouped = opt.grouped;
    v.ApplyTo(&store, &prog);
    if (store.Grouped()) {
        // Grouped output aggregates. Per-address enrichment (DNS/GeoIP) stays
        // skipped - a group header cannot resolve as one address - but TRAFFIC
        // is per-PID, and a group row IS one PID: skipping it blanked the very
        // columns `--sort nettotal` exists for (every group printed "—").
        // Collect the visible groups' PIDs here; the join lands on the flat
        // member rows, and RenderList's ApplyListView rebuilds the groups
        // (GroupByProcess takes max-over-members) with the totals attached.
        for (size_t i = 0; i < store.View().size(); ++i) {
            const Connection* c = store.ViewRow(i);
            if (c == nullptr) continue;
            if (seenPid.insert(c->pid).second) sel.pids.push_back(c->pid);
        }
        return sel;
    }
    // Traffic is one bounded scan for the whole PID set, so PIDs come from
    // every view row: sampling only the printed rows would make
    // `--sort nettotal` order unsampled (zero) rows. DNS/GeoIP are
    // per-address and expensive, so they honor --limit.
    for (size_t i = 0; i < store.View().size(); ++i) {
        const Connection* c = store.ViewRow(i);
        if (c == nullptr) continue;
        if (seenPid.insert(c->pid).second) sel.pids.push_back(c->pid);
    }
    for (size_t i = 0; i < store.View().size(); ++i) {
        if (opt.limit != 0 && sel.remotes.size() >= opt.limit) break;
        const Connection* c = store.ViewRow(i);
        if (c == nullptr) continue;
        sel.remotes.push_back(c->remoteAddress);
        if (seenAddr.insert(c->remoteAddress).second && !c->remoteAddress.empty()) {
            GeoTarget t;
            t.address = c->remoteAddress;
            t.ipv6 = (c->family == AF_INET6);
            sel.geoTargets.push_back(t);
        }
    }
    return sel;
}

// A row that can never carry enrichment: a wildcard or unspecified peer, a
// listener with no remote, or a row the kernel has already given up on
// (TIME_WAIT and friends keep their endpoint for a while, but the process is
// gone, so `process` stays an em-dash no matter what is switched on).
bool RowCanEnrich(const Connection& c) {
    if (c.remoteAddress.empty() || c.remoteAddress == L"*") return false;
    if (c.remoteAddress == L"0.0.0.0" || c.remoteAddress == L"::") return false;
    if (c.remotePort == 0) return false;              // listener / unbound
    const bool terminal = c.state == MIB_TCP_STATE_TIME_WAIT ||
                          c.state == MIB_TCP_STATE_CLOSED ||
                          c.state == MIB_TCP_STATE_DELETE_TCB;
    if (terminal && c.pid == 0) return false;
    return true;
}

// D3: `--limit N` truncates BEFORE the enrichment join, so when the head of
// the default (PID-ascending) table is TIME_WAIT / listener / wildcard rows
// the printed window is entirely unenrichable and every enrichment column
// reads `—` - which looks like the feature is broken when it is only that
// nothing in the window can be enriched.
//
// The default sort is NOT reordered to hide this: that is a CLI/GUI parity
// surface (the GUI list has the same head), and silently changing which rows
// `--limit` means is worse than saying so. Instead the caller gets one hint
// on STDERR - never on stdout, which must stay a clean table/CSV/JSON feed
// (the D4 lesson).
//
// Scoped to what the hint can honestly claim (D13): a per-ADDRESS join
// (DNS/GeoIP) over rows whose host/country columns are actually printed.
// Measured before that scoping: `list --group --traffic --sort tx --desc
// --limit 3 --columns process,tx,rx` printed rows carrying 43.80 GB of real
// traffic and then claimed "none of the 3 rows ... can be enriched", with
// advice that would not have helped anyway. Three cases it no longer
// pretends about: per-PID traffic (blank exactly when no totals were
// sampled - "state:estab" does not fix that, and no host/country is in
// play); grouped views (a group header cannot resolve as one address, so
// its blanks are design, not a bad --limit window - the advice would be a
// lie); and runs that print no host/country column at all (nothing visible
// is broken, so there is nothing to explain).
//
// Returns true when the hint was produced.
bool UnenrichableHeadWarning(const ConnectionStore& store,
                             const ListOptions& opt, std::string* err) {
    if (err == nullptr || opt.limit == 0 || opt.quiet) return false;
    if (!opt.dns && opt.geoIpPath.empty()) return false;  // no per-address join
    if (store.Grouped()) return false;                    // never resolvable
    bool showsRemote = false;
    for (int c : ListColumns(opt)) {
        if (c == COL_HOST || c == COL_COUNTRY) showsRemote = true;
    }
    if (!showsRemote) return false;  // no enrichment column printed
    if (opt.filter.find(L"state:") != std::wstring::npos) return false;
    size_t shown = 0, enrichable = 0;
    for (size_t i = 0; i < store.View().size(); ++i) {
        if (shown >= opt.limit) break;
        const Connection* c = store.ViewRow(i);
        if (c == nullptr) continue;
        ++shown;
        if (RowCanEnrich(*c)) ++enrichable;
    }
    if (shown == 0 || enrichable != 0) return false;
    // "note:", not the verb prefix: the run succeeded and printed; this is an
    // observation about the window, not a failure (an error-styled prefix made
    // it read like an internal fault).
    *err = "note: none of the " + std::to_string(shown) +
           " rows in --limit can be enriched (TIME_WAIT/listener/wildcard); "
           "add a filter such as \"state:estab\" to see host/country.\r\n";
    return true;
}

// View-scoped enrichment for list/export: DNS + GeoIP + traffic over the
// printed rows only. Returns false + err when the GeoIP database fails;
// DNS/traffic failures are silent per-address (em-dash), never fatal.
// 'advice', when non-null, receives an OPTIONAL hint (the D3 unenrichable
// window) that is never fatal: the run still succeeds and still prints.
bool EnrichViewForList(ConnectionStore& store, const ListOptions& opt,
                       std::wstring* err, std::string* advice) {
    // Missing-switch advice first: it must fire even when NO join runs below
    // (that early return is exactly the case it exists for).
    if (advice != nullptr) *advice = MissingEnrichmentAdvice(opt);
    if (!opt.dns && !opt.traffic && opt.geoIpPath.empty()) return true;
    const PrintSelection sel = SelectedForPrint(store, opt);
    if (opt.dns) JoinDnsAddrs(store, sel.remotes);
    if (!opt.geoIpPath.empty()) {
        GeoIpDatabase db;
        std::wstring loadErr;
        if (!db.Load(opt.geoIpPath.c_str(), &loadErr)) {
            if (err != nullptr) *err = loadErr;
            return false;
        }
        JoinGeoAddrs(store, db, sel.geoTargets);
    }
    if (opt.traffic) JoinTrafficPids(store, sel.pids);
    if (HasEnrichmentClause(opt.filter)) {
        // A filter with an enrichment clause has had NO view applied yet:
        // SelectedForPrint's pre-join branch works off the flat rows (the
        // columns the clause matches on were empty), and export applies its
        // view BEFORE the joins run. Measured: `export --dns --filter
        // "host:com"` wrote a silent 0-row file while the same filter on
        // `list` printed 55 rows, and D3's head hint saw view=0 on every
        // pre-join filter. The joins have filled those columns now, so apply
        // the filter for real: this is the view D3 inspects below, and the
        // rows export writes to its file. RenderList re-applies the same
        // filter afterwards and yields the same view, so list output is
        // unchanged by this.
        ApplyListView(store, opt);
    }
    // Advisory only: reported on stderr, never fatal, never on stdout.
    // Appended, not assigned: the missing-switch advice above may already be
    // present and both are worth seeing.
    std::string hint;
    if (UnenrichableHeadWarning(store, opt, &hint) && advice != nullptr) {
        *advice += hint;
    }
    return true;
}

CommandResult RenderList(ConnectionStore& store, const ListOptions& opt,
                         bool skipHeader) {
    CommandResult r;
    // D27, enforced HERE so it covers every machine-readable shape at once -
    // `export` and `list --format csv|tsv|json` - rather than per call site.
    //
    // A grouped stream cannot honestly carry a per-connection column: there is
    // no group-level answer for a remote address, a hostname, a duration or a
    // country, so the cell falls through to one arbitrary member while the
    // header still promises the column. Measured before this: a 4-connection
    // process exported the single value "0.0.0.0" under a header reading
    // "Remote address", and a spreadsheet loads that without complaint.
    //
    // The `table` format is deliberately exempt. There the row is visibly a
    // group - its State cell reads "4 connections" - so a representative
    // address is informative, and blanking it would be strictly less useful.
    // That is the same reason the GUI keeps it, and it is why this rule lives
    // in the stream path rather than in GroupColumnText.
    {
        const std::string bad = ValidateStreamColumns(opt);
        if (!bad.empty()) {
            r.exitCode = kExitArgs;
            r.err = "--group cannot report " + bad +
                    ": a group is one process, and that column describes one "
                    "connection, so a " +
                    (opt.format == "json" ? "JSON key" : "file header") +
                    " would promise a value the rows do not carry.\r\n"
                    "Drop the column, or drop --group. Columns that DO have a "
                    "group answer: pid, process, proto, state, traffic, rx, "
                    "tx, nettotal, cpu, mem, disk, service, path, pinned.\r\n"
                    "(The aligned `table` output is exempt: its State column "
                    "already shows the connection count, so a representative "
                    "value there is readable rather than misleading.)\r\n";
            return r;
        }
    }
    ApplyListView(store, opt);
    if (opt.quiet) {
        // No output at all; the exit code is the whole answer.
        r.exitCode = store.View().empty() ? kExitFail : kExitOk;
        return r;
    }
    if (opt.format == "json") {
        r.out = JsonList(store, opt);
    } else {
        r.out = DelimitedList(store, opt, skipHeader);
    }
    return r;
}

// One snapshot pass of `list` into a CALLER-owned store.
//
// CmdList (below) owns a throwaway store, which is right for a single shot.
// The watch loop cannot: it re-enters this function every tick, and a fresh
// store means a fresh firstSeenTick, so Duration read 0s on every poll - a
// connection looked older with every refresh. Keeping the store (and the
// SnapshotSource, which caches PID->name/service) across ticks is what makes
// the per-row lifetime state accumulate. Deliberately the ONLY implementation
// of the list pass, so the two entry points cannot drift apart.
CommandResult CmdListInto(SnapshotSource& source, ConnectionStore& store,
                          const ListOptions& opt) {
    CommandResult r;
    std::wstring err;
    // F5.3. Before the pass, because the resolver reads the flag while it
    // resolves - and on EVERY pass, not just the first: `list --watch` re-enters
    // this function per tick against the same source, so setting it once at
    // startup would be fine, but the GUI shares a source with `details` and
    // resetting it here keeps each pass independent of how the last one ran.
    source.SetVerifySignatures(opt.signatures);
    if (!BuildStoreSnapshot(source, store, /*procStats=*/true,
                            /*resolveDns=*/false, /*geoIp=*/false, nullptr,
                            &err)) {
        r.exitCode = kExitFail;
        r.err = "Enumeration failed: " + WideToUtf8(err) + "\r\n";
        return r;
    }
    // D15: the fixed frame first. The columns are known the moment the
    // snapshot exists, and the enrichment joins below can take seconds
    // (--dns resolves, --traffic scans) - so the header goes to stdout NOW,
    // while the rows follow in r.out when they are ready. json has no
    // header line to lead with; --quiet prints nothing at all. RenderList's
    // skipHeader keeps the combined byte stream identical to an unstreamed
    // run: same header, same widths, same rows.
    const bool streamHeader =
        opt.streamHeader && !opt.quiet && opt.format != "json";
    if (streamHeader) StreamOut(ListHeaderLine(store, opt));
    std::string advice;
    if (!EnrichViewForList(store, opt, &err, &advice)) {
        r.exitCode = kExitFail;
        r.err = "GeoIP: " + WideToUtf8(err) + "\r\n";
        return r;
    }
    // D3 hint. Appended AFTER RenderList fills r.out, and the caller prints r.err
    // on stderr, so the table/CSV/JSON on stdout stays byte-identical to a run
    // without the hint - a consumer parsing stdout cannot tell the difference.
    r = RenderList(store, opt, /*skipHeader=*/streamHeader);
    if (!advice.empty()) {
        if (!r.err.empty()) r.err += advice;
        else r.err = advice;
    }
    return r;
}

CommandResult CmdList(SnapshotSource& source, const ListOptions& opt) {
    ConnectionStore store;   // single shot: this store dies with the command
    return CmdListInto(source, store, opt);
}

CommandResult RenderPs(ConnectionStore& store, const PsOptions& opt) {
    CommandResult r;
    // Filter rows first, then aggregate the survivors by PID.
    std::vector<FilterClause> prog;
    if (!opt.filter.empty()) ParseFilter(opt.filter, prog);
    struct Agg {
        std::wstring name;
        size_t conns = 0;
        double cpu = -1.0;
        ULONGLONG mem = 0;
        bool memKnown = false;
        ULONGLONG disk = 0;
        bool diskKnown = false;
    };
    std::map<DWORD, Agg> byPid;
    for (const Connection& c : store.Rows()) {
        if (c.flags & kRowRemoved) continue;
        if (!prog.empty() && !MatchFilter(c, prog))
            continue;
        Agg& a = byPid[c.pid];
        if (a.name.empty()) a.name = c.processName;
        ++a.conns;
        if (c.cpuPct >= 0.0) a.cpu = c.cpuPct;
        if (c.memKnown) {
            a.memKnown = true;
            if (a.mem == 0) a.mem = c.memWorkingSet;
        }
        if (c.ioKnown) {
            a.diskKnown = true;
            if (a.disk == 0) a.disk = c.diskReadBytes + c.diskWriteBytes;
        }
    }
    struct Row {
        DWORD pid;
        Agg a;
    };
    std::vector<Row> rows;
    for (const auto& kv : byPid) rows.push_back(Row{kv.first, kv.second});
    if (opt.quiet) {
        r.exitCode = rows.empty() ? kExitFail : kExitOk;
        return r;
    }
    const std::string key = opt.sortBy;
    std::sort(rows.begin(), rows.end(), [&](const Row& x, const Row& y) {
        if (key == "mem") {
            if (x.a.memKnown != y.a.memKnown)
                return x.a.memKnown > y.a.memKnown;
            if (x.a.mem != y.a.mem) return x.a.mem > y.a.mem;
            return x.pid < y.pid;
        }
        if (key == "disk") {
            if (x.a.diskKnown != y.a.diskKnown)
                return x.a.diskKnown > y.a.diskKnown;
            if (x.a.disk != y.a.disk) return x.a.disk > y.a.disk;
            return x.pid < y.pid;
        }
        if (key == "conns") {
            if (x.a.conns != y.a.conns) return x.a.conns > y.a.conns;
            return x.pid < y.pid;
        }
        if (key == "pid") return x.pid < y.pid;
        if (key == "process") {
            if (x.a.name != y.a.name) return x.a.name < y.a.name;
            return x.pid < y.pid;
        }
        // "cpu" (default): known first, hottest first, pid tiebreak.
        if ((x.a.cpu >= 0.0) != (y.a.cpu >= 0.0))
            return x.a.cpu >= 0.0;
        if (x.a.cpu != y.a.cpu) return x.a.cpu > y.a.cpu;
        return x.pid < y.pid;
    });
    const bool csv = (opt.format == "csv");
    const char delim = csv ? ',' : '\t';
    if (opt.format == "json") {
        std::string out = "[";
        bool first = true;
        size_t shown = 0;
        for (const Row& row : rows) {
            if (opt.limit != 0 && shown >= opt.limit) break;
            if (!first) out += ",";
            out += "\n  {\"pid\":" + std::to_string(row.pid) + ",\"process\":\"" +
                   JsonEscapeA(row.a.name) + "\",\"connections\":" +
                   std::to_string(row.a.conns) + ",\"cpu\":\"" +
                   JsonEscapeA(row.a.cpu >= 0.0
                                   ? std::to_wstring(row.a.cpu)
                                   : std::wstring(L"")) +
                   "\",\"memory\":\"" +
                   JsonEscapeA(row.a.memKnown ? FormatBytes(row.a.mem)
                                              : std::wstring(L"")) +
                   "\",\"disk\":\"" +
                   JsonEscapeA(row.a.diskKnown ? FormatBytes(row.a.disk)
                                               : std::wstring(L"")) +
                   "\"}";
            first = false;
            ++shown;
        }
        out += "\n]";
        r.out = out;
        return r;
    }
    // Collect the printed cells once; the aligned table (D14) and the
    // delimited shapes then render from the same strings.
    std::vector<std::vector<std::string>> cells;
    for (const Row& row : rows) {
        if (opt.limit != 0 && cells.size() >= opt.limit) break;
        wchar_t cpu[32] = {0};
        if (row.a.cpu >= 0.0)
            ::swprintf_s(cpu, L"%.1f", row.a.cpu);
        else
            ::swprintf_s(cpu, L"—");
        const std::wstring mem =
            row.a.memKnown ? FormatBytes(row.a.mem) : std::wstring(L"—");
        const std::wstring disk =
            row.a.diskKnown ? FormatBytes(row.a.disk) : std::wstring(L"—");
        cells.push_back({std::to_string(row.pid), TsvCell(row.a.name),
                         std::to_string(row.a.conns), TsvCell(cpu),
                         TsvCell(mem), TsvCell(disk)});
    }
    if (opt.format == "table") {
        // Numbers right, names left - the same split the GUI's list shows.
        const std::vector<std::string> header = {"PID", "Process", "Conns",
                                                 "CPU%", "Memory", "Disk"};
        const std::vector<bool> right = {true, false, true, true, true, true};
        r.out = RenderAlignedLines(header, cells, right,
                                   ContentWidths(header, cells), true);
        return r;
    }
    std::string out = std::string("PID") + delim + "Process" + delim +
                      "Conns" + delim + "CPU%" + delim + "Memory" + delim +
                      "Disk\r\n";
    for (const std::vector<std::string>& f : cells) {
        for (size_t i = 0; i < f.size(); ++i) {
            if (i != 0) out += delim;
            // CSV must quote a field containing a comma (a process name
            // can have one); the old path wrote it raw and produced
            // columns that did not line up for any parser.
            out += csv ? CsvEscapeUtf8(f[i]) : f[i];
        }
        out += "\r\n";
    }
    r.out = out;
    return r;
}

CommandResult CmdPs(SnapshotSource& source, const PsOptions& opt) {
    CommandResult r;
    ConnectionStore store;
    std::wstring err;
    if (!BuildStoreSnapshot(source, store, /*procStats=*/true,
                            /*resolveDns=*/false, /*geoIp=*/false, nullptr,
                            &err)) {
        r.exitCode = kExitFail;
        r.err = "Enumeration failed: " + WideToUtf8(err) + "\r\n";
        return r;
    }
    return RenderPs(store, opt);
}

DetailModel BuildDetailModel(const Connection& c,
                             const ConnectionStore& store,
                             bool threadsMayBlock) {
    DetailModel m;
    m.title = c.processName.empty() ? c.protoLabel : c.processName;
    m.subtitle = L"PID " + std::to_wstring(c.pid) + L"   ·   " +
                 c.localEndpoint + L"  →  " + c.remoteEndpoint;
    const std::wstring created =
        FormatFileTimeLocal(c.processCreate, c.processCreateKnown);
    const std::wstring cmdline =
        (c.pid != 0) ? QueryProcessCommandLine(c.pid) : std::wstring();
    const auto add = [](DetailSection& sec, const wchar_t* label,
                        const std::wstring& value, bool mono = false) {
        DetailField f;
        f.label = label;
        f.value = value.empty() ? L"—" : value;
        f.monospace = mono;
        sec.fields.push_back(std::move(f));
    };
    DetailSection proc;
    proc.title = L"Process";
    add(proc, L"Name", c.processName);
    add(proc, L"PID", std::to_wstring(c.pid));
    add(proc, L"Path", c.processPath);
    add(proc, L"Command line", cmdline);
    add(proc, L"Started", created, /*mono=*/true);
    add(proc, L"Service", c.serviceName);
    if (c.firstSeenTick != 0) {
        add(proc, L"First seen", FormatDuration(DurationSeconds(c)));
    }
    m.sections.push_back(std::move(proc));

    // ---- Threads (todo.md 8.8 G5) ------------------------------------------
    //
    // What a process is doing, thread by thread. This is NOT a per-connection
    // thread, because no such thing is obtainable: MIB_TCPROW_OWNER_PID carries
    // no thread id and an AFD endpoint is owned by the process. See ProcessInfo.h.
    //
    // Rendered as ordinary fields rather than a special list: the label column
    // is 172 px, which suits "TID 8124", and reusing the two-column table means
    // the section scrolls, elides and copies for free.
    //
    // Ranked by CPU time, busiest first, because that ordering is the whole
    // answer to "what is this process doing" - a ranked list says which thread is
    // working, an unordered one says only how many there are. Ties break on
    // thread id so the list does not jitter between refreshes, and threads whose
    // times could not be read sort LAST rather than being treated as zero: a
    // protected process's threads have not done no work, they are unknown.
    {
        bool threadsKnown = false;
        unsigned threadsAgeMs = 0;
        std::vector<ThreadInfo> threads =
            ProcessThreads(c.pid, threadsMayBlock, &threadsKnown,
                            &threadsAgeMs);
        if (c.pid != 0 && (threadsKnown || !threads.empty())) {
            std::stable_sort(threads.begin(), threads.end(),
                             [](const ThreadInfo& a, const ThreadInfo& b) {
                                 if (a.timesKnown != b.timesKnown)
                                     return a.timesKnown;   // known before unknown
                                 if (a.cpu100ns != b.cpu100ns)
                                     return a.cpu100ns > b.cpu100ns;
                                 return a.tid < b.tid;
                             });

            DetailSection thr;
            thr.title = L"Threads (" + std::to_wstring(threads.size()) + L")";

            // "Now" in the same epoch as create100ns, for the age arithmetic.
            FILETIME nowFt = {};
            ::GetSystemTimeAsFileTime(&nowFt);
            const ULONGLONG now100ns =
                (static_cast<ULONGLONG>(nowFt.dwHighDateTime) << 32) |
                nowFt.dwLowDateTime;

            wchar_t label[32] = {0};
            for (const ThreadInfo& t : threads) {
                ::swprintf_s(label, L"TID %u", t.tid);
                std::wstring value;
                if (t.timesKnown) {
                    wchar_t buf[96] = {0};
                    // CPU as a duration, not bytes: this is the number a reader
                    // compares against the others, and "1.2 s" says that where
                    // "12000000" would only say that we counted.
                    const double cpuSec = static_cast<double>(t.cpu100ns) / 1e7;
                    ::swprintf_s(buf, L"cpu %.2f s", cpuSec);
                    value = buf;
                    if (t.create100ns != 0 && now100ns > t.create100ns) {
                        const double ageSec =
                            static_cast<double>(now100ns - t.create100ns) / 1e7;
                        value += L"   started " + FormatDuration(
                                                       static_cast<unsigned>(
                                                           ageSec));
                        value += L" ago";
                    }
                } else {
                    // Not zero. A protected process's threads have no readable
                    // times, and printing 0.00 s would be a fabricated
                    // measurement rather than an admission of ignorance.
                    value = L"cpu -   started -";
                }
                value += L"   pri " + std::to_wstring(t.basePriority);
                add(thr, label, value, /*mono=*/false);
            }

            // The note carries the two things the numbers cannot: how fresh they
            // are, and that the CPU figure is lifetime-since-thread-start rather
            // than since this window opened - which is otherwise a very easy
            // misreading.
            std::wstring note;
            if (threadsAgeMs != UINT_MAX) {
                note = L"sampled " + FormatDuration(threadsAgeMs / 1000) +
                       L" ago; CPU time is total since each thread started, "
                       L"not since this window opened.";
            } else {
                note = L"reading threads...";
            }
            if (!threads.empty()) {
                bool anyUnknown = false;
                for (const ThreadInfo& t : threads) {
                    if (!t.timesKnown) { anyUnknown = true; break; }
                }
                if (anyUnknown) {
                    note += L"  Some threads belong to a process whose times "
                            L"cannot be read; those show - rather than 0.";
                }
            }
            thr.note = note;
            m.sections.push_back(std::move(thr));
        } else if (c.pid == 0) {
            // An ownerless row (TIME_WAIT) has no process and therefore no
            // threads. Saying so beats an empty section with no explanation.
            DetailSection thr;
            thr.title = L"Threads";
            thr.note = L"this row has no owning process.";
            m.sections.push_back(std::move(thr));
        }
        // c.pid != 0, nothing published yet, empty: the first sample is still in
        // flight (see ProcessThreads). Rendered as NO section rather than an
        // empty one - an empty "Threads" heading would assert "zero threads",
        // and every process has at least one. The next refresh fills it in.
    }

    DetailSection live;
    live.title = L"Live stats (this refresh)";
    if (c.cpuPct >= 0.0) {
        wchar_t cpu[32] = {0};
        ::swprintf_s(cpu, L"%.1f %%", c.cpuPct);
        add(live, L"CPU", cpu);
    } else {
        add(live, L"CPU", std::wstring());
    }
    if (c.memKnown) {
        add(live, L"Memory (working set)", FormatBytes(c.memWorkingSet));
        add(live, L"Memory (private)", FormatBytes(c.memPrivate));
    } else {
        add(live, L"Memory", std::wstring());
    }
    if (c.ioKnown) {
        const ULONGLONG total = c.diskReadBytes + c.diskWriteBytes;
        add(live, L"Disk read", FormatBytes(c.diskReadBytes));
        add(live, L"Disk written", FormatBytes(c.diskWriteBytes));
        add(live, L"Disk total",
            (total == 0) ? std::wstring() : FormatBytes(total));
    } else {
        add(live, L"Disk I/O", std::wstring());
    }
    if (c.trafficRx != 0 || c.trafficTx != 0) {
        const ULONGLONG total = c.trafficRx + c.trafficTx;
        add(live, L"Network received", FormatBytes(c.trafficRx));
        add(live, L"Network sent", FormatBytes(c.trafficTx));
        add(live, L"Network total", FormatBytes(total));
        if (c.bpsKnown) {
            wchar_t rate[64] = {0};
            ::swprintf_s(rate, L"↓ %s/s   ↑ %s/s",
                         FormatBytes(static_cast<ULONGLONG>(c.rxBps + 0.5)).c_str(),
                         FormatBytes(static_cast<ULONGLONG>(c.txBps + 0.5)).c_str());
            add(live, L"Network rate", rate);
        }
    } else {
        add(live, L"Network", std::wstring());
        live.note =
            L"Enable View > Per-PID traffic counters (ETW, admin) for "
            L"per-process byte totals, or the non-admin per-socket fallback "
            L"fills TCP rows automatically.";
    }
    m.sections.push_back(std::move(live));

    if (c.tls.known) {
        DetailSection tls;
        tls.title = L"TLS";
        if (c.tls.secure) {
            add(tls, L"Protocol", TlsProtocolName(c.tls.protocol));
            add(tls, L"Cipher suite",
                c.tls.cipherSuite ? TlsCipherName(c.tls.cipherSuite)
                                  : std::wstring());
            add(tls, L"SNI", c.tls.haveSni ? c.tls.sni : std::wstring());
            add(tls, L"Certificate subject", c.tls.certSubject);
            add(tls, L"Certificate issuer", c.tls.certIssuer);
            add(tls, L"Valid", c.tls.certValidity);
        } else {
            add(tls, L"State", L"not encrypted");
        }
        m.sections.push_back(std::move(tls));
    }

    DetailSection conn;
    conn.title = L"Selected connection";
    add(conn, L"Protocol", c.protoLabel);
    add(conn, L"Local", c.localEndpoint);
    add(conn, L"Remote", c.remoteEndpoint);
    add(conn, L"State", c.stateLabel);
    add(conn, L"Hostname", c.hostname);
    if (!c.country.empty()) add(conn, L"Country", c.country);
    if (c.firstSeenTick != 0)
        add(conn, L"Duration", FormatDuration(DurationSeconds(c)));
    m.sections.push_back(std::move(conn));

    // EVERY sibling connection is listed. There was a 25-line cap here
    // ("25 sibling lines fit the dossier on one screen", with connectionTotal
    // still reporting the true count), and the justification had stopped being
    // true: the window scrolls.
    //
    // A user who opened Details on a browser with 46 sockets got 25 rows and
    // an unexpandable "... and 21 more". That answers none of the questions
    // they opened the window to ask, and the section it truncated is the only
    // one that shows what the process is actually connected to.
    //
    // The cap is removed rather than made expandable. An expander would have
    // been a second control on a window whose whole design is "read, and
    // collapse sections", and it would still have hidden the answer behind a
    // click. The selected row is in that list too, so scrolling to it was the
    // only other way to find it.
    //
    // connectionTotal therefore always equals connectionLines.size() for a
    // model built here. Both fields and the "... and N more" rendering stay:
    // DetailModel is a shared struct that the CLI `details` verb also reads,
    // and deleting the contract is a wider break than this fix is worth. The
    // renderer keeps its guard, which now simply never fires.
    for (const Connection& row : store.Rows()) {
        if (row.pid != c.pid || (row.flags & kRowRemoved)) continue;
        ++m.connectionTotal;
        std::wstring line = row.protoLabel + L"  " + row.localEndpoint +
                            L"   " + row.remoteEndpoint + L"  (" +
                            row.stateLabel + L")";
        if (!row.hostname.empty()) line += L"  @ " + row.hostname;
        m.connectionLines.push_back(std::move(line));
    }
    return m;
}

// Resolve a --select filter to exactly one live row index. The shared
// "find it, refuse to guess" core behind details/close/block/capture and
// kill-by-select, so every verb reports ambiguity the same way.
bool SelectSingleLiveIndex(const ConnectionStore& store,
                           const std::wstring& select, const char* verb,
                           size_t* out, std::string* err) {
    const std::vector<size_t> hits = store.SelectByFilter(select);
    std::vector<size_t> live;
    for (size_t i : hits) {
        if (i < store.Rows().size() &&
            !(store.Rows()[i].flags & kRowRemoved))
            live.push_back(i);
    }
    if (live.empty()) {
        if (err != nullptr)
            *err = std::string(verb) + ": no live row matches '" +
                   WideToUtf8(select) + "'.\r\n";
        return false;
    }
    if (live.size() > 1) {
        if (err != nullptr)
            *err = std::string(verb) + ": '" + WideToUtf8(select) +
                   "' matches " + std::to_string(live.size()) +
                   " rows; refine to one.\r\n";
        return false;
    }
    if (out != nullptr) *out = live[0];
    return true;
}

CommandResult CmdDetails(SnapshotSource& source, const std::wstring& select,
                         const EnrichOptions& eo) {
    CommandResult r;
    if (select.empty()) {
        r.exitCode = kExitArgs;
        r.err = "details: --select <filter> is required (e.g. --select pid:1234).\r\n";
        return r;
    }
    ConnectionStore store;
    std::wstring err;
    // F5.3, same reason and same position as in CmdListInto: the flag reaches
    // the resolver, not the row join, so it must be set before the pass.
    source.SetVerifySignatures(eo.signatures);
    if (!BuildStoreSnapshot(source, store, eo.procStats, /*resolveDns=*/false,
                            /*geoIp=*/false, nullptr, &err)) {
        r.exitCode = kExitFail;
        r.err = "Enumeration failed: " + WideToUtf8(err) + "\r\n";
        return r;
    }
    size_t idx = 0;
    std::string serr;
    if (!SelectSingleLiveIndex(store, select, "details", &idx, &serr)) {
        r.exitCode = kExitFail;
        r.err = serr;
        return r;
    }
    // Enrich exactly the chosen row (values, then join: joins mutate).
    const Connection chosen = store.Rows()[idx];
    if (eo.dns && !chosen.remoteAddress.empty()) {
        const std::wstring host = DnsResolver::Lookup(chosen.remoteAddress);
        if (!host.empty()) store.SetHostname(chosen.remoteAddress, host);
    }
    if (!eo.geoIpPath.empty()) {
        GeoIpDatabase db;
        std::wstring loadErr;
        if (!db.Load(eo.geoIpPath.c_str(), &loadErr)) {
            r.exitCode = kExitFail;
            r.err = "GeoIP: " + WideToUtf8(loadErr) + "\r\n";
            return r;
        }
        GeoTarget t;
        t.address = chosen.remoteAddress;
        t.ipv6 = (chosen.family == AF_INET6);
        JoinGeoAddrs(store, db, std::vector<GeoTarget>(1, t));
    }
    if (eo.traffic) {
        JoinTrafficPids(store, std::vector<DWORD>(1, chosen.pid));
    }
    // A one-shot command pays the ~48 ms thread snapshot inline, because the
    // alternative is to exit before the background worker ever answers - which
    // is what the first version did, and it printed no Threads section at all.
    const DetailModel m =
        BuildDetailModel(store.Rows()[idx], store, /*threadsMayBlock=*/true);
    r.out = WideToUtf8(m.ToPlainText()) + "\r\n";
    return r;
}

ViewState CurrentPresetViewFor(const std::wstring& filter, UINT32 colVisible,
                               int sortColumn, bool sortAsc,
                               unsigned sources) {
    ViewState v;
    v.filter = filter;
    v.colVisible = colVisible;
    v.sortColumn = sortColumn;
    v.sortAsc = sortAsc;
    v.sources = sources;
    return v;
}

void ApplyPresetViewToStore(const ViewState& v, ConnectionStore& store,
                            std::vector<FilterClause>* out) {
    v.ApplyTo(&store, out);
}

// D29. One rule, applied by both front ends, with the self-kill case in it.
//
// WHY THIS LIVES HERE AND NOT IN THE GUI. The GUI is supposed to render what
// this layer computes, never decide for itself (the 5.3 invariant). A rule that
// only the GUI enforced would leave the CLI able to do the same thing: `kill
// --select` resolves a selector to a row, and nothing stopped that row from
// being one of wintcp.exe's own connections - `kill --select "pid:<self>"` is
// just a command line. Placing the refusal here closes both.
//
// WHY THE SELF-CHECK IS NOT PARANOIA. D29 was first seen as "wintcp.exe closed
// when I killed something else", which turned out to be a use-after-free handing
// KillPid() a garbage PID. But the self-kill is reachable on its own, with no
// memory bug at all: wintcp.exe is a network tool, so it has its own sockets and
// therefore its own rows. Right-click one, choose End process, and the tool
// closes. There is no crash and no error - the program ends exactly as the user
// asked it to, having simply ended the wrong thing.
PidVerdict PidKillVerdict(DWORD rowPid, DWORD selfPid, bool exists) {
    if (!exists || rowPid == 0) return PidVerdict::Pseudo;
    if (rowPid == selfPid) return PidVerdict::IsSelf;
    // PID 4 is the System pseudo-process. It has no image to end, and OpenProcess
    // on it is a different story on different builds, so refuse rather than ask.
    if (rowPid == 4) return PidVerdict::Pseudo;
    return PidVerdict::Ok;
}

const wchar_t* PidKillRefusal(PidVerdict v) {
    switch (v) {
        case PidVerdict::IsSelf:
            return L"WinTCP cannot end its own process. Close the window "
                   L"instead - that is the same thing without the data loss.";
        case PidVerdict::Pseudo:
            return L"That row has no process that can be ended. PID 0 and the "
                   L"System pseudo-process do not own a real image.";
        case PidVerdict::NotPermitted:
            return L"This process cannot be ended with the rights this session "
                   L"holds. Run WinTCP elevated to end it.";
        case PidVerdict::Ok:
        default:
            return L"";
    }
}

CommandResult KillPid(DWORD pid, const FILETIME& create, bool createKnown,
                      const std::wstring& label, const MutateOptions& mo) {
    // Checked before anything else, including --dry-run: a plan that says
    // "kill PID 5168 (wintcp.exe)" is a bad thing to print even if nothing is
    // done, because the next thing a user does is paste it somewhere.
    const DWORD selfPid = ::GetCurrentProcessId();
    const PidVerdict verdict = PidKillVerdict(pid, selfPid, pid != 0);
    if (verdict != PidVerdict::Ok) {
        CommandResult r;
        r.exitCode = kExitArgs;
        r.err = "kill: refusing PID " + std::to_string(pid) + ": " +
                WideToUtf8(PidKillRefusal(verdict)) + "\r\n";
        return r;
    }
    std::string plan = "kill PID " + std::to_string(pid);
    if (!label.empty()) plan += " (" + WideToUtf8(label) + ")";
    plan += "\r\n";
    if (mo.dryRun) {
        CommandResult r;
        r.out = plan;
        return r;
    }
    if (!mo.yes) return Refused("kill PID " + std::to_string(pid));
    if (createKnown) {
        std::wstring verr;
        if (!ProcessResolver::VerifyProcess(pid, create, true, verr)) {
            CommandResult r;
            r.exitCode = kExitFail;
            r.err = "kill: PID " + std::to_string(pid) +
                    " no longer matches (" + WideToUtf8(verr) +
                    "); refusing.\r\n";
            return r;
        }
    }
    HANDLE h = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (h == nullptr) {
        CommandResult r;
        r.exitCode = kExitFail;
        wchar_t buf[128] = {0};
        ::swprintf_s(buf, L"kill: OpenProcess(%lu) failed: %ls.\r\n", pid,
                     FormatSystemError(::GetLastError()).c_str());
        r.err = WideToUtf8(buf);
        return r;
    }
    (void)PostCloseToProcessWindows(pid);
    const DWORD wait = ::WaitForSingleObject(h, kKillGraceMs);
    bool ok = false;
    if (wait == WAIT_OBJECT_0) {
        ok = true;
    } else {
        ok = ::TerminateProcess(h, 1) != FALSE;
    }
    ::CloseHandle(h);
    CommandResult r;
    if (!ok) {
        r.exitCode = kExitFail;
        r.err = "kill: PID " + std::to_string(pid) + " did not exit.\r\n";
        return r;
    }
    r.out = plan;
    return r;
}

CommandResult CmdKill(SnapshotSource& source, DWORD pid,
                      const MutateOptions& mo) {
    // D29. This used to carry its own copy of the pseudo-PID rule
    // (`if (pid == 0 || pid == 4)`) alongside the one in KillPid, and the two
    // had already drifted: the copy said "refusing PID 4." and the original
    // explained why. Two guards for one rule in one file is exactly the drift
    // the 5.3 review exists to catch, and it was only visible because a golden
    // check asserted on the MESSAGE rather than the exit code.
    //
    // Removed rather than kept in step: the rule lives in PidKillVerdict() and
    // the wording in PidKillRefusal(), and the only thing a caller should have
    // to decide is whether it has a PID to act on. `kill --pid 4` now reaches
    // KillPid's guard and prints the same explanation the GUI shows.
    // --dry-run still refuses here rather than at KillPid, because this
    // function is reached before the snapshot and the refusal should not
    // depend on how long enumeration takes.
    // Resolve the create time for PID-reuse verification.
    SnapshotOptions so;
    so.resolveProcesses = true;
    Snapshot snap;
    if (!source.Build(so, &snap)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "Enumeration failed: " + WideToUtf8(snap.error) + "\r\n";
        return r;
    }
    const Connection* rep = nullptr;
    for (const Connection& c : snap.rows) {
        if (c.pid == pid) {
            rep = &c;
            break;
        }
    }
    const FILETIME noCreate = {};
    return KillPid(pid,
                   rep != nullptr ? rep->processCreate : noCreate,
                   rep != nullptr && rep->processCreateKnown,
                   rep != nullptr ? rep->processName : std::wstring(), mo);
}

CommandResult CloseRow(const Connection& c, const MutateOptions& mo) {
    if (c.protocol != IPPROTO_TCP || c.family != AF_INET) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "close: only established IPv4 TCP connections can be closed "
                "(Windows exposes no public IPv6 teardown).\r\n";
        return r;
    }
    // SetTcpEntry refuses without elevation, so say so up front instead of
    // reporting the raw API failure afterwards. (Verified live: a medium-
    // integrity close of our own ESTABLISHED connection fails.)
    if (!IsElevated()) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "close: needs elevation (run as administrator); the row is "
                "untouched.\r\n";
        return r;
    }
    const std::string plan = "close " + WideToUtf8(c.localEndpoint) + " -> " +
                             WideToUtf8(c.remoteEndpoint) + " pid " +
                             std::to_string(c.pid) + "\r\n";
    if (mo.dryRun) {
        CommandResult r;
        r.out = plan;
        return r;
    }
    if (!mo.yes) return Refused("close connection");
    std::wstring cerr;
    if (!CloseTcpConnection(c, cerr)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "close failed: " + WideToUtf8(cerr) + "\r\n";
        return r;
    }
    CommandResult r;
    r.out = plan;
    return r;
}

CommandResult CmdClose(SnapshotSource& source, const std::wstring& select,
                       const MutateOptions& mo) {
    if (select.empty()) {
        CommandResult r;
        r.exitCode = kExitArgs;
        r.err = "close: --select <filter> is required.\r\n";
        return r;
    }
    ConnectionStore store;
    std::wstring err;
    if (!BuildStoreSnapshot(source, store, /*procStats=*/false,
                            /*resolveDns=*/false, /*geoIp=*/false, nullptr,
                            &err)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "Enumeration failed: " + WideToUtf8(err) + "\r\n";
        return r;
    }
    const std::vector<size_t> hits = store.SelectByFilter(select);
    std::vector<size_t> live;
    for (size_t i : hits) {
        if (i < store.Rows().size() &&
            !(store.Rows()[i].flags & kRowRemoved))
            live.push_back(i);
    }
    if (live.empty()) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "close: no live row matches '" + WideToUtf8(select) + "'.\r\n";
        return r;
    }
    if (live.size() > 1) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "close: '" + WideToUtf8(select) + "' matches " +
                std::to_string(live.size()) + " rows; refine to one.\r\n";
        return r;
    }
    const Connection& c = store.Rows()[live[0]];
    return CloseRow(c, mo);
}

CommandResult CmdKillSelect(SnapshotSource& source, const std::wstring& select,
                            const MutateOptions& mo) {
    if (select.empty()) {
        CommandResult r;
        r.exitCode = kExitArgs;
        r.err = "kill: --pid N or --select <filter> is required.\r\n";
        return r;
    }
    ConnectionStore store;
    std::wstring err;
    if (!BuildStoreSnapshot(source, store, /*procStats=*/false,
                            /*resolveDns=*/false, /*geoIp=*/false, nullptr,
                            &err)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "Enumeration failed: " + WideToUtf8(err) + "\r\n";
        return r;
    }
    size_t idx = 0;
    std::string serr;
    if (!SelectSingleLiveIndex(store, select, "kill", &idx, &serr)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = serr;
        return r;
    }
    const Connection& c = store.Rows()[idx];
    return KillPid(c.pid, c.processCreate, c.processCreateKnown,
                   c.processName, mo);
}

bool ConnectionToBlockRequest(const Connection& c, BlockRequest* out,
                              std::wstring* whyNot) {
    if (out == nullptr) return false;
    if (c.protocol != IPPROTO_TCP || c.state == MIB_TCP_STATE_LISTEN ||
        c.state == 0) {
        if (whyNot != nullptr)
            *whyNot = L"Only an established TCP connection can be blocked.";
        return false;
    }
    BlockRequest req;
    req.ipv6 = (c.family == AF_INET6);
    req.localPort = static_cast<uint16_t>(c.localPort);
    req.remotePort = static_cast<uint16_t>(c.remotePort);
    req.label = c.localEndpoint + L" -> " + c.remoteEndpoint;
    if (req.ipv6) {
        ::memcpy(req.remoteAddr, c.remote6.s6_addr, 16);
    } else {
        ::memcpy(req.remoteAddr, &c.remote4, 4);
    }
    *out = req;
    return true;
}

CommandResult BlockNow(const BlockRequest& req, const MutateOptions& mo) {
    const std::string plan = "block " + WideToUtf8(req.label) + "\r\n";
    if (mo.dryRun) {
        CommandResult r;
        r.out = plan;
        return r;
    }
    if (!mo.yes) return Refused("block connection");
    // Both layers (teardown + firewall rules) refuse without elevation.
    if (!IsElevated()) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "block: needs elevation (run as administrator); nothing was "
                "changed.\r\n";
        return r;
    }
    std::wstring berr;
    const BlockOutcome o = BlockConnection(req, &berr);
    CommandResult r;
    r.out = WideToUtf8(BlockOutcomeToString(o)) + ": " + plan;
    if (o == BlockOutcome::kFailed || o == BlockOutcome::kKilledOnly) {
        r.exitCode = kExitFail;
        r.err = WideToUtf8(berr) + "\r\n";
    }
    return r;
}

CommandResult CmdBlock(SnapshotSource& source, const std::wstring& select,
                       const MutateOptions& mo) {
    if (select.empty()) {
        CommandResult r;
        r.exitCode = kExitArgs;
        r.err = "block: --select <filter> is required.\r\n";
        return r;
    }
    ConnectionStore store;
    std::wstring err;
    if (!BuildStoreSnapshot(source, store, /*procStats=*/false,
                            /*resolveDns=*/false, /*geoIp=*/false, nullptr,
                            &err)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "Enumeration failed: " + WideToUtf8(err) + "\r\n";
        return r;
    }
    size_t idx = 0;
    std::string serr;
    if (!SelectSingleLiveIndex(store, select, "block", &idx, &serr)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = serr;
        return r;
    }
    BlockRequest req;
    std::wstring why;
    if (!ConnectionToBlockRequest(store.Rows()[idx], &req, &why)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "block: " + WideToUtf8(why) + "\r\n";
        return r;
    }
    return BlockNow(req, mo);
}

CommandResult CmdUnblock(const std::wstring& address, UINT port,
                         const MutateOptions& mo) {
    if (address.empty() || port == 0 || port > 65535) {
        CommandResult r;
        r.exitCode = kExitArgs;
        r.err = "unblock: --address <ip> and --port <1..65535> are required.\r\n";
        return r;
    }
    const std::wstring norm = NormalizeAddress(address);
    if (norm.empty()) {
        CommandResult r;
        r.exitCode = kExitArgs;
        r.err = "unblock: '" + WideToUtf8(address) +
                "' is not a usable address.\r\n";
        return r;
    }
    const bool v6 = (norm.find(L':') != std::wstring::npos);
    BlockRequest req;
    req.ipv6 = v6;
    req.remotePort = static_cast<uint16_t>(port);
    if (v6) {
        IN6_ADDR a6 = {};
        if (::InetPtonW(AF_INET6, norm.c_str(), &a6) != 1) {
            CommandResult r;
            r.exitCode = kExitArgs;
            r.err = "unblock: cannot parse IPv6 address.\r\n";
            return r;
        }
        ::memcpy(req.remoteAddr, a6.s6_addr, 16);
    } else {
        IN_ADDR a4 = {};
        if (::InetPtonW(AF_INET, norm.c_str(), &a4) != 1) {
            CommandResult r;
            r.exitCode = kExitArgs;
            r.err = "unblock: cannot parse IPv4 address.\r\n";
            return r;
        }
        ::memcpy(req.remoteAddr, &a4, 4);
    }
    const std::string plan =
        "unblock " + WideToUtf8(norm) + ":" + std::to_string(port) + "\r\n";
    if (mo.dryRun) {
        CommandResult r;
        r.out = plan;
        return r;
    }
    if (!mo.yes) return Refused("unblock");
    std::wstring uerr;
    CommandResult r;
    if (!UnblockConnection(req, &uerr)) {
        r.exitCode = kExitFail;
        r.err = "unblock failed: " + WideToUtf8(uerr) + "\r\n";
        return r;
    }
    r.out = plan;
    return r;
}

CommandResult CmdBlocks() {
    CommandResult r;
    const int n = CountWinTcpRules();
    r.out = "wintcp-firewall-rules: " + std::to_string(n) + "\r\n";
    return r;
}

CommandResult CmdBookmarkList(const std::string& format) {
    CommandResult r;
    size_t unreadable = 0;
    const std::vector<Bookmark> all = Bookmarks::List(&unreadable);
    if (format == "json") {
        std::string out = "[";
        bool first = true;
        for (const Bookmark& b : all) {
            if (!first) out += ",";
            out += "\n  {\"address\":\"" + JsonEscapeA(b.address) +
                   "\",\"port\":" + std::to_string(b.port) + ",\"tag\":\"" +
                   JsonEscapeA(BookmarkTagLabel(b.tag)) + "\",\"note\":\"" +
                   JsonEscapeA(b.note) + "\"}";
            first = false;
        }
        out += "\n]";
        r.out = out;
        return r;
    }
    if (format == "table") {
        // Aligned like every other human table (D14): addresses, tags and
        // notes left, the port number right.
        const std::vector<std::string> header = {"Address", "Port", "Tag",
                                                 "Note"};
        const std::vector<bool> right = {false, true, false, false};
        std::vector<std::vector<std::string>> cells;
        cells.reserve(all.size());
        for (const Bookmark& b : all) {
            cells.push_back({TsvCell(b.address), std::to_string(b.port),
                             TsvCell(BookmarkTagLabel(b.tag)),
                             TsvCell(b.note)});
        }
        r.out = RenderAlignedLines(header, cells, right,
                                   ContentWidths(header, cells), true);
    } else {
        const char delim = (format == "csv") ? ',' : '\t';
        std::string out = std::string("Address") + delim + "Port" + delim +
                          "Tag" + delim + "Note\r\n";
        for (const Bookmark& b : all) {
            std::string line = TsvCell(b.address) + delim +
                               std::to_string(b.port) + delim +
                               TsvCell(BookmarkTagLabel(b.tag)) + delim +
                               TsvCell(b.note) + "\r\n";
            if (format == "csv") {
                line = CsvEscapeUtf8(WideToUtf8(b.address)) + delim +
                       std::to_string(b.port) + delim +
                       CsvEscapeUtf8(WideToUtf8(BookmarkTagLabel(b.tag))) +
                       delim + CsvEscapeUtf8(WideToUtf8(b.note)) + "\r\n";
            }
            out += line;
        }
        r.out = out;
    }
    if (unreadable != 0) {
        r.err = "warning: " + std::to_string(unreadable) +
                " unreadable bookmark entries skipped.\r\n";
    }
    return r;
}

// The CLI parser enforces kMinTag..kMaxTag (Commands.h); the canonical tag
// set lives in Bookmarks.h. These must be the same range — a tag the parser
// accepts but the store rejects (or vice versa) is a refusal that depends on
// which layer you ask. Pinned at compile time, not by convention.
static_assert(kMinTag == kBookmarkTagNone && kMaxTag == kBookmarkTagGreen,
              "CLI tag range drifted from the bookmark tag set");
CommandResult CmdBookmarkAdd(const std::wstring& address, UINT port,
                             unsigned tag, const std::wstring& note) {
    CommandResult r;
    if (!IsValidBookmarkTag(tag)) {
        r.exitCode = kExitArgs;
        r.err = "bookmark add: --tag must be 0..4.\r\n";
        return r;
    }
    if (note.size() > Bookmarks::kMaxNoteChars) {
        r.exitCode = kExitArgs;
        r.err = "bookmark add: note too long.\r\n";
        return r;
    }
    if (!Bookmarks::Add(address, port, tag, note)) {
        r.exitCode = kExitFail;
        r.err = "bookmark add: failed (bad address, port, or registry).\r\n";
        return r;
    }
    r.out = "bookmark added: " + WideToUtf8(MakeBookmarkKey(
                                NormalizeAddress(address), port)) +
            "\r\n";
    return r;
}

CommandResult CmdBookmarkRemove(const std::wstring& address, UINT port) {
    CommandResult r;
    if (!Bookmarks::Remove(address, port)) {
        r.exitCode = kExitFail;
        r.err = "bookmark remove: failed.\r\n";
        return r;
    }
    r.out = "bookmark removed.\r\n";
    return r;
}

CommandResult CmdBookmarkNote(const std::wstring& address, UINT port,
                              const std::wstring& note) {
    CommandResult r;
    if (!Bookmarks::SetNote(address, port, note)) {
        r.exitCode = kExitFail;
        r.err = "bookmark note: failed (no such bookmark?).\r\n";
        return r;
    }
    r.out = "bookmark note updated.\r\n";
    return r;
}

CommandResult CmdBookmarkColour(const std::wstring& address, UINT port,
                                unsigned tag) {
    CommandResult r;
    if (!IsValidBookmarkTag(tag)) {
        r.exitCode = kExitArgs;
        r.err = "bookmark colour: --tag must be 0..4.\r\n";
        return r;
    }
    if (!Bookmarks::SetColour(address, port, tag)) {
        r.exitCode = kExitFail;
        r.err = "bookmark colour: failed (no such bookmark?).\r\n";
        return r;
    }
    r.out = "bookmark colour updated.\r\n";
    return r;
}

CommandResult CmdPresetList(const std::string& format) {
    CommandResult r;
    const std::vector<std::wstring> names = Presets::List();
    if (format == "json") {
        std::string out = "[";
        bool first = true;
        for (const std::wstring& n : names) {
            if (!first) out += ",";
            out += "\n  \"" + JsonEscapeA(n) + "\"";
            first = false;
        }
        out += "\n]";
        r.out = out;
        return r;
    }
    const char delim = (format == "csv") ? ',' : '\t';
    std::string out = std::string("Preset") + "\r\n";
    (void)delim;
    for (const std::wstring& n : names) {
        const std::string cell = WideToUtf8(n);
        out += (format == "csv" ? CsvEscapeUtf8(cell) : cell) + "\r\n";
    }
    r.out = out;
    return r;
}

CommandResult CmdPresetSave(const std::wstring& name, const ViewState& view,
                            bool overwrite) {
    CommandResult r;
    const PresetSave s = Presets::Save(name, view, overwrite);
    switch (s) {
        case PresetSave::kCreated:
            r.out = "preset created: " + WideToUtf8(name) + "\r\n";
            break;
        case PresetSave::kOverwrote:
            r.out = "preset overwritten: " + WideToUtf8(name) + "\r\n";
            break;
        case PresetSave::kExists:
            // The one non---yes route through Refused (C5): same exit code and
            // same meaning, different remedy, so the wording says --force.
            // Returned, not assigned + broken out of, because Refused() builds
            // the whole CommandResult including the exit code.
            return Refused("preset '" + WideToUtf8(name) + "' exists",
                           "--force");
        case PresetSave::kInvalidName:
            r.exitCode = kExitArgs;
            r.err = "preset: invalid name.\r\n";
            break;
        case PresetSave::kFailed:
        default:
            r.exitCode = kExitFail;
            r.err = "preset save failed (registry).\r\n";
            break;
    }
    return r;
}

CommandResult CmdPresetShow(const std::wstring& name) {
    CommandResult r;
    PresetView v;
    if (!Presets::Load(name, &v)) {
        r.exitCode = kExitFail;
        r.err = "preset '" + WideToUtf8(name) + "' not found or corrupt.\r\n";
        return r;
    }
    r.out = ViewStateToJson(v) + "\r\n";
    return r;
}

CommandResult CmdPresetDelete(const std::wstring& name) {
    CommandResult r;
    if (!Presets::Delete(name)) {
        r.exitCode = kExitFail;
        r.err = "preset delete failed.\r\n";
        return r;
    }
    r.out = "preset deleted: " + WideToUtf8(name) + "\r\n";
    return r;
}

CommandResult CmdPresetApply(SnapshotSource& source, const std::wstring& name,
                             ListOptions* appliedView,
                             const ListOptions* overrides) {
    CommandResult r;
    PresetView v;
    if (!Presets::Load(name, &v)) {
        r.exitCode = kExitFail;
        r.err = "preset '" + WideToUtf8(name) + "' not found or corrupt.\r\n";
        return r;
    }
    ListOptions opt;
    opt.filter = v.filter;
    opt.sortColumn = v.sortColumn;
    opt.sortAsc = v.sortAsc;
    opt.grouped = v.grouped;
    // D24: layer the user's OWN switches over the saved view. Before this the
    // command-line switches were parsed and then thrown away, so
    // `preset apply --name web --limit 2` printed the entire 27-row table in
    // the preset's default column set and exited 0 - a script asking for two
    // rows and a narrow table got both of neither.
    //
    // Only fields the caller ACTUALLY set are taken, which is why this takes a
    // ListOptions plus the set of switches the user typed: limit 0 and an empty
    // column list are both legitimate "unset" values, so a plain struct copy
    // would clobber the preset with defaults.
    if (overrides != nullptr) {
        if (overrides->limit != 0) opt.limit = overrides->limit;
        if (!overrides->columns.empty()) opt.columns = overrides->columns;
        if (overrides->sortColumn != COL_PID)
            opt.sortColumn = overrides->sortColumn;
        opt.sortAsc = overrides->sortAsc;
        opt.format = overrides->format;
        opt.traffic = overrides->traffic;
        opt.dns = overrides->dns;
        opt.geoIpPath = overrides->geoIpPath;
    }
    if (appliedView != nullptr) *appliedView = opt;
    ConnectionStore store;
    std::wstring err;
    if (!BuildStoreSnapshot(source, store, /*procStats=*/true,
                            (v.sources & kPresetSourceHosts) != 0,
                            (v.sources & kPresetSourceGeoIp) != 0, nullptr,
                            &err)) {
        r.exitCode = kExitFail;
        r.err = "Enumeration failed: " + WideToUtf8(err) + "\r\n";
        return r;
    }
    ApplyListView(store, opt);
    r.out = DelimitedList(store, opt);
    // An empty view is a real, reportable answer for an applied view, so the
    // exit code says so - the same contract `list` already has.
    if (r.out.empty()) r.exitCode = kExitFail;
    return r;
}

std::string FormatChangeEvents(const std::vector<RowChange>& events,
                               const std::string& format,
                               const std::wstring& stamp,
                               const std::vector<FilterClause>* prog,
                               unsigned eventMask) {
    const std::string t = WideToUtf8(stamp);
    const bool json = (format == "json");
    std::string out;
    for (const RowChange& e : events) {
        // D25: the event-kind gate, BEFORE the row filter, so a user asking for
        // only APPEAR never pays for (or is confused by) the DISAPPEAR half of
        // the diff. `eventMask` defaults to all three, so every existing caller
        // that passes 0x7 behaves exactly as before.
        if (eventMask != 0 && (eventMask & (1u << e.kind)) == 0) continue;
        if (prog != nullptr && !prog->empty() && !MatchFilter(e.row, *prog))
            continue;
        const char* kind = (e.kind == kChangeDisappear) ? "DISAPPEAR"
                           : (e.kind == kChangeState)   ? "STATE"
                                                        : "APPEAR";
        const char* jkind = (e.kind == kChangeDisappear) ? "disappear"
                            : (e.kind == kChangeState)   ? "state"
                                                         : "appear";
        if (json) {
            out += "{\"t\":\"" + JsonEscapeA(stamp) + "\",\"event\":\"" +
                   jkind + "\",\"proto\":\"" +
                   JsonEscapeA(e.row.protoLabel) + "\",\"local\":\"" +
                   JsonEscapeA(e.row.localAddress) +
                   "\",\"lport\":" + std::to_string(e.row.localPort) +
                   ",\"remote\":\"" + JsonEscapeA(e.row.remoteAddress) +
                   "\",\"rport\":" + std::to_string(e.row.remotePort) +
                   ",\"state\":\"" + JsonEscapeA(e.row.stateLabel) + "\"";
            if (e.kind == kChangeState) {
                out += ",\"old_state\":\"" +
                       JsonEscapeA(TcpStateToString(e.oldState)) + "\"";
            }
            out += ",\"pid\":" + std::to_string(e.row.pid) +
                   ",\"process\":\"" + JsonEscapeA(e.row.processName) +
                   "\"}\r\n";
            continue;
        }
        out += "[" + t + "] " + kind + " " +
               WideToUtf8(e.row.protoLabel) + " " +
               WideToUtf8(e.row.localEndpoint) + " -> " +
               WideToUtf8(e.row.remoteEndpoint) + " (" +
               WideToUtf8(e.row.stateLabel) + ")";
        if (e.kind == kChangeState) {
            out += " was " + WideToUtf8(TcpStateToString(e.oldState));
        }
        out += " pid=" + std::to_string(e.row.pid) + " " +
               WideToUtf8(e.row.processName) + "\r\n";
    }
    return out;
}

CommandResult CmdExport(SnapshotSource& source, const ListOptions& opt,
                        const std::wstring& outPath) {
    if (outPath.empty()) {
        CommandResult r;
        r.exitCode = kExitArgs;
        r.err = "export: --out <file> is required.\r\n";
        return r;
    }
    // D27: the same rule RenderList applies, needed here because export writes
    // the file itself and does not go through RenderList. Checked BEFORE the
    // snapshot so a refused request costs nothing and touches no state.
    {
        const std::string bad = ValidateStreamColumns(opt);
        if (!bad.empty()) {
            CommandResult r;
            r.exitCode = kExitArgs;
            r.err = "export: --group cannot report " + bad +
                    ": a group is one process, and that column describes one "
                    "connection, so the file's header would promise a value "
                    "the rows do not carry.\r\n"
                    "Drop the column, or drop --group. Columns that DO have a "
                    "group answer: pid, process, proto, state, traffic, rx, "
                    "tx, nettotal, cpu, mem, disk, service, path, pinned.\r\n";
            return r;
        }
    }
    ConnectionStore store;
    std::wstring err;
    if (!BuildStoreSnapshot(source, store, /*procStats=*/true,
                            /*resolveDns=*/false, /*geoIp=*/false, nullptr,
                            &err)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "Enumeration failed: " + WideToUtf8(err) + "\r\n";
        return r;
    }
    ApplyListView(store, opt);
    // Export means everything the view selects: --limit is a display
    // truncation for list/ps, and must not silently truncate a file while
    // the report claims the full count. The CLI therefore REFUSES --limit on
    // export (exit 2, naming the switch) rather than accepting and ignoring
    // it - D24: a swallowed switch is worse than a rejected one, because a
    // script cannot tell from the exit code that it got the whole file.
    ListOptions full = opt;
    full.limit = 0;
    full.quiet = false;
    // Enrichment flags (--dns/--db/--traffic) apply to exports too: the
    // file carries what the screen would show.
    std::string advice;
    if (!EnrichViewForList(store, full, &err, &advice)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "GeoIP: " + WideToUtf8(err) + "\r\n";
        return r;
    }
    const std::string content = (full.format == "json")
        ? JsonList(store, full)
        : DelimitedList(store, full);
    const bool isJson = (full.format == "json");
    const std::wstring werr =
        WriteUtf8FileWithBom(outPath, content, !isJson);
    CommandResult r;
    if (!werr.empty()) {
        r.exitCode = kExitFail;
        r.err = "export failed: " + WideToUtf8(werr) + "\r\n";
        return r;
    }
    r.out = "exported " + std::to_string(store.View().size()) + " rows to " +
            WideToUtf8(outPath) + "\r\n";
    // D24: --quiet now means what it means everywhere else. It used to be
    // accepted by export and then printed this line anyway, so
    // `export --out f.csv --quiet > log` still wrote a line into the log - a
    // quiet switch that is not quiet is a trap for exactly the scripts that
    // use it. The file is still written; only the confirmation line is
    // suppressed, and the exit code still says whether rows were written.
    if (opt.quiet) r.out.clear();
    // Missing-switch advice: without it an export written against an
    // enrichment clause its switches cannot answer is a silent zero-row file.
    if (!advice.empty()) r.err = advice;
    return r;
}

CommandResult CmdGeoIpLookup(SnapshotSource& source,
                             const std::wstring& geoIpPath,
                             const std::wstring& ip) {
    CommandResult r;
    if (ip.empty()) {
        r.exitCode = kExitArgs;
        r.err = "geoip lookup: <ip> is required.\r\n";
        return r;
    }
    GeoIpDatabase db;
    if (!geoIpPath.empty()) {
        std::wstring err;
        if (!db.Load(geoIpPath, &err)) {
            r.exitCode = kExitFail;
            r.err = "geoip: cannot load database: " + WideToUtf8(err) + "\r\n";
            return r;
        }
    } else {
        // Use the source's loaded database when no path is given.
        (void)source;
        r.exitCode = kExitArgs;
        r.err = "geoip lookup: --db <file.mmdb> is required.\r\n";
        return r;
    }
    IN_ADDR a4 = {};
    IN6_ADDR a6 = {};
    std::wstring code;
    if (::InetPtonW(AF_INET, ip.c_str(), &a4) == 1) {
        code = db.LookupV4(ntohl(a4.S_un.S_addr));
    } else if (::InetPtonW(AF_INET6, ip.c_str(), &a6) == 1) {
        code = db.LookupV6(a6.s6_addr);
    } else {
        r.exitCode = kExitArgs;
        r.err = "geoip lookup: '" + WideToUtf8(ip) + "' is not an IP.\r\n";
        return r;
    }
    r.out = WideToUtf8(code.empty() ? std::wstring(L"—") : code) + "\r\n";
    return r;
}

CommandResult CmdGeoIpInfo(SnapshotSource& /*source*/,
                           const std::wstring& geoIpPath) {
    CommandResult r;
    if (geoIpPath.empty()) {
        r.exitCode = kExitArgs;
        r.err = "geoip info: --db <file.mmdb> is required.\r\n";
        return r;
    }
    GeoIpDatabase db;
    std::wstring err;
    if (!db.Load(geoIpPath, &err)) {
        r.exitCode = kExitFail;
        r.err = "geoip: cannot load database: " + WideToUtf8(err) + "\r\n";
        return r;
    }
    wchar_t buf[256] = {0};
    ::swprintf_s(buf, L"%ls, %llu records, %llu nodes, %zu bytes",
                 db.DatabaseVersion().c_str(),
                 static_cast<unsigned long long>(db.RecordCount()),
                 static_cast<unsigned long long>(db.NodeCount()),
                 db.FileSize());
    r.out = WideToUtf8(buf) + "\r\n";
    return r;
}

CommandResult CmdCapture(SnapshotSource& source, const std::wstring& select,
                         const MutateOptions& mo, unsigned secs,
                         const CaptureOptions& co) {
    if (select.empty()) {
        CommandResult r;
        r.exitCode = kExitArgs;
        r.err = "capture: --select <filter> is required.\r\n";
        return r;
    }
    ConnectionStore store;
    std::wstring err;
    if (!BuildStoreSnapshot(source, store, /*procStats=*/false,
                            /*resolveDns=*/false, /*geoIp=*/false, nullptr,
                            &err)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "Enumeration failed: " + WideToUtf8(err) + "\r\n";
        return r;
    }
    size_t idx = 0;
    std::string selErr;
    if (!SelectSingleLiveIndex(store, select, "capture", &idx, &selErr)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = selErr;
        return r;
    }
    const Connection& c = store.Rows()[idx];
    // The direction selector is validated BEFORE any capture work, so a typo
    // costs a parse error rather than fifteen seconds of pktmon. Refused
    // explicitly rather than quietly defaulted to "both": a reader who asked for
    // one direction and got two would have no way to notice the mistake.
    CaptureDir dir = CaptureDir::kBoth;
    if (!ParseCaptureDir(co.dir, &dir)) {
        CommandResult r;
        r.exitCode = kExitArgs;
        r.err = "capture: --dir " + WideToUtf8(co.dir) +
                " is not a direction. Use both, first (a) or second (b).\r\n";
        return r;
    }
    // One builder for both front ends (see StreamCapture.h): binary fields,
    // never the display strings that carry the port.
    const CaptureTarget t = MakeCaptureTarget(c);
    // The plan (and the --yes gate) come before the elevation gate: a
    // dry run must never demand elevation, and refusing must not either.
    // Order: resolve -> dry-run -> confirm -> availability -> start.
    std::string plan = "capture " + WideToUtf8(c.localEndpoint) +
                       " -> " + WideToUtf8(c.remoteEndpoint) +
                       " for " + std::to_string(secs) + "s\r\n";
    // Say what the run WILL do, in the plan, before it does it. Without this a
    // --text run's own framing lines arrive looking like payload, and a reader
    // piping it somewhere would capture them too.
    if (co.text) plan += "mode: print the reassembled stream to stdout\r\n";
    if (!co.outPath.empty())
        plan += "saving the capture to " + WideToUtf8(co.outPath) + "\r\n";
    if (mo.dryRun) {
        CommandResult r;
        r.out = plan;
        return r;
    }
    if (!mo.yes) return Refused("capture (starts pktmon, needs elevation)");
    std::wstring why2;
    if (!CaptureAvailable(&why2)) {
        CommandResult r;
        r.exitCode = kExitFail;
        if (why2.empty())
            why2 = L"this run is not elevated (an elevated run would work)";
        r.err = "capture unavailable: " + WideToUtf8(why2) + ".\r\n";
        return r;
    }
    std::wstring serr;
    if (!StartCapture(t, &serr)) {
        CommandResult r;
        r.exitCode = kExitFail;
        r.err = "capture start failed: " + WideToUtf8(serr) + "\r\n";
        return r;
    }
    // Belt and braces with the parser (which enforces the same range): a
    // future in-process caller must get the same clamp the CLI promises.
    if (secs < kCaptureSecsMin) secs = kCaptureSecsMin;
    if (secs > kCaptureSecsMax) secs = kCaptureSecsMax;
    ::Sleep(secs * 1000u);   // bounded capture window
    // --out: hand the destination to StopCapture, which writes the capture
    // while the converted file is still on disk. Passing the path (not the
    // bytes) is deliberate - see StopCapture's header for the memory reason.
    const CaptureResult cr =
        StopCapture(t, co.outPath.empty() ? nullptr : &co.outPath);
    ClearCaptureFilter();
    CommandResult r;
    if (!cr.ok) {
        r.exitCode = kExitFail;
        r.err = "capture failed: " + WideToUtf8(cr.error) + "\r\n";
        return r;
    }
    char buf[256] = {0};
    ::sprintf_s(buf, "packets=%llu toServer=%llu toClient=%llu blocks=%llu\r\n",
                static_cast<unsigned long long>(cr.packetsParsed),
                static_cast<unsigned long long>(cr.toServer.bytes.size()),
                static_cast<unsigned long long>(cr.toClient.bytes.size()),
                static_cast<unsigned long long>(cr.blocksSeen));
    r.out = plan + buf;
    // A direction that hit the in-memory reassembly cap reports the CAP, not a
    // measurement, and that figure is indistinguishable from a real one on the
    // line above. ReasmResult::truncated exists for exactly this and the hex
    // view already honours it; printing the number without saying so would let
    // a reader treat 33554432 as "this stream carried 32 MB" when the truth is
    // "at least 32 MB, and we stopped looking". So name the direction that
    // stopped, without the limit itself - the cap is an implementation detail
    // that may move, and a stale number here would be a second thing to keep
    // true.
    if (cr.toServer.truncated || cr.toClient.truncated) {
        std::string capped;
        if (cr.toServer.truncated && cr.toClient.truncated)
            capped = "toServer and toClient";
        else if (cr.toServer.truncated)
            capped = "toServer";
        else
            capped = "toClient";
        r.out += "note: " + capped +
                 " reached the reassembly cap, so that byte count is truncated "
                 "- a floor, not a total.\r\n";
    }
    // --text. This is the CLI replacement for the GUI's Follow TCP stream.
    //
    // Each direction is introduced by the ENDPOINT that sent it, not by
    // "client"/"server": pktmon does not reliably report which end sent the SYN,
    // so those words would be an assertion the capture cannot support (see
    // ReasmResult's naming note). dir1Label/dir2Label come from the same
    // canonicalisation ReassembleStream used, so the label cannot disagree with
    // which half of the bytes this is.
    if (co.text) {
        const auto emit = [&r](const wchar_t* heading, const std::wstring& label,
                               const ReasmResult& dir) {
            r.out += "\r\n--- ";
            r.out += WideToUtf8(heading);
            r.out += ": ";
            r.out += label.empty() ? std::string("(unknown endpoint)")
                                   : WideToUtf8(label);
            r.out += " ---\r\n";
            // The honesty notes come BEFORE the bytes, not after, so a reader
            // who stops reading at the first block has still been told the
            // stream is incomplete.
            if (dir.bytes.empty()) {
                r.out += "(no payload captured in this direction)\r\n";
                return;
            }
            if (dir.hasGap || dir.bytesMissing != 0) {
                r.out += "NOTE: " + std::to_string(dir.bytesMissing) +
                         " byte(s) were never observed; the stream has a hole "
                         "at offset " + std::to_string(dir.firstSeq) +
                         " and every decode after it is unreliable.\r\n";
            }
            if (dir.truncated) {
                r.out +=
                    "NOTE: this direction hit the reassembly cap, so what "
                    "follows is a prefix, not the whole stream.\r\n";
            }
            r.out += FormatStreamHex(dir.bytes);
        };
        if (dir == CaptureDir::kBoth || dir == CaptureDir::kFirst)
            emit(L"to first endpoint", cr.dir1Label, cr.toServer);
        if (dir == CaptureDir::kBoth || dir == CaptureDir::kSecond)
            emit(L"from first endpoint", cr.dir2Label, cr.toClient);
    }
    return r;
}

CommandResult CmdStat(SystemStatsSampler& sampler, const std::string& format) {
    // Prime then sample: CPU/network rates need two reads.
    (void)sampler.Sample();
    ::Sleep(kStatSampleGapMs);
    const SystemStats s = sampler.Sample();
    CommandResult r;
    if (format == "json") {
        r.out = SystemStatsToJson(s) + "\r\n";
        return r;
    }
    r.out = FormatSystemStatsLine(s) + "\r\n";
    return r;
}

int ColumnIdForName(const std::wstring& name) {
    const std::wstring n = ToLowerW(name);
    if (n == L"proto") return COL_PROTO;
    if (n == L"local") return COL_LOCAL;
    if (n == L"lport") return COL_LPORT;
    if (n == L"remote") return COL_REMOTE;
    if (n == L"rport") return COL_RPORT;
    if (n == L"state") return COL_STATE;
    if (n == L"pid") return COL_PID;
    if (n == L"process") return COL_PROCESS;
    if (n == L"service") return COL_SERVICE;
    if (n == L"host") return COL_HOST;
    if (n == L"path") return COL_PATH;
    if (n == L"traffic") return COL_TRAFFIC;
    if (n == L"rx") return COL_RX;
    if (n == L"tx") return COL_TX;
    if (n == L"nettotal" || n == L"net") return COL_NETTOTAL;
    if (n == L"cpu") return COL_CPU;
    if (n == L"mem" || n == L"memory") return COL_MEM;
    if (n == L"disk") return COL_DISK;
    if (n == L"duration") return COL_DURATION;
    if (n == L"bandwidth" || n == L"speed") return COL_BANDWIDTH;
    // G5. "procspeed" is the canonical name; "groupspeed" is accepted because
    // that is what it reads as in a --group view, and refusing a name someone
    // can reasonably guess is the kind of friction D24 exists to remove.
    if (n == L"procspeed" || n == L"groupspeed") return COL_GROUPRATE;
    if (n == L"note" || n == L"notes") return COL_NOTE;
    // G6: the `ss -i` names. "rtt"/"latency" both accepted - "rtt" is jargon
    // and "latency" is the plainer word a person may reach for first.
    if (n == L"rtt" || n == L"latency") return COL_RTT;
    if (n == L"minrtt" || n == L"min-rtt") return COL_MINRTT;
    if (n == L"cwnd" || n == L"window") return COL_CWND;
    if (n == L"retrans" || n == L"retransmits") return COL_RETRANS;
    if (n == L"tls") return COL_TLS;
    if (n == L"country") return COL_COUNTRY;
    if (n == L"pinned" || n == L"bookmark") return COL_PINNED;
    // F5.1/F5.2/F5.3. Several spellings each, on the same principle as
    // procspeed/rtt above: refusing a name someone can reasonably guess is the
    // friction D24 exists to remove. "parent" is accepted for the column whose
    // header is "Parent", and "signed" for the one whose header is "Signature",
    // because those are the words the two things are actually called.
    if (n == L"ppid" || n == L"parent" || n == L"parentpid") return COL_PPID;
    if (n == L"integrity" || n == L"il" || n == L"integritylevel")
        return COL_INTEGRITY;
    if (n == L"signature" || n == L"signed" || n == L"sig") return COL_SIGNATURE;
    return -1;
}

bool ResolveColumnSpec(const std::wstring& spec, std::vector<int>& out) {
    const std::wstring n = ToLowerW(spec);
    auto setTo = [&](const ColumnSet& s) {
        out.assign(s.columns, s.columns + s.count);
        return true;
    };
    if (n == L"default") return setTo(kColumnSetDefault);
    if (n == L"minimal") return setTo(kColumnSetMinimal);
    if (n == L"full" || n == L"wide") return setTo(kColumnSetFull);
    std::vector<int> cols;
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(L',', i);
        if (j == std::wstring::npos) j = spec.size();
        std::wstring part = spec.substr(i, j - i);
        size_t a = 0;
        while (a < part.size() && part[a] == L' ') ++a;
        size_t b = part.size();
        while (b > a && part[b - 1] == L' ') --b;
        const int id = ColumnIdForName(part.substr(a, b > a ? b - a : 0));
        if (id >= 0) cols.push_back(id);
        i = j + 1;
    }
    if (cols.empty()) return false;
    out = cols;
    return true;
}

std::string ViewStateToJson(const ViewState& v) {    std::string out = "{\"filter\":\"" + JsonEscapeA(v.filter) +
                      "\",\"sortColumn\":" + std::to_string(v.sortColumn) +
                      ",\"sortAsc\":" + (v.sortAsc ? "true" : "false") +
                      ",\"grouped\":" + (v.grouped ? "true" : "false") +
                      ",\"colVisible\":" + std::to_string(v.colVisible) +
                      ",\"sources\":" + std::to_string(v.sources) + "}";
    return out;
}

bool ViewStateFromPreset(const PresetView& v, ViewState* out) {
    if (out == nullptr) return false;
    *out = v;
    return true;
}

std::string FormatSystemStatsLine(const SystemStats& s) {
    std::string line = "CPU ";
    char buf[256] = {0};
    if (s.cpuKnown)
        ::sprintf_s(buf, "%.1f%%", s.cpuPct);
    else
        ::sprintf_s(buf, "--");
    line += buf;
    line += "  MEM ";
    if (s.memKnown) {
        char mb[128] = {0};
        ::sprintf_s(mb, "%.1f%%", s.memPct);
        line += WideToUtf8(FormatBytes(s.memUsed)) + "/" +
                WideToUtf8(FormatBytes(s.memTotal)) + " (" + mb + ")";
    } else {
        line += "--";
    }
    line += "  DISK ";
    if (s.diskKnown) {
        line += WideToUtf8(FormatBytes(static_cast<ULONGLONG>(s.diskReadBps))) +
                "/s+" +
                WideToUtf8(FormatBytes(static_cast<ULONGLONG>(s.diskWriteBps))) +
                "/s";
    } else {
        line += "n/a";
    }
    line += "  NET ";
    if (s.netKnown) {
        line += WideToUtf8(FormatBytes(static_cast<ULONGLONG>(s.netRecvBps))) +
                "/s+" +
                WideToUtf8(FormatBytes(static_cast<ULONGLONG>(s.netSendBps))) +
                "/s";
    } else {
        line += "collecting";
    }
    return line;
}

std::string SystemStatsToJson(const SystemStats& s) {
    char buf[512] = {0};
    ::sprintf_s(buf,
                "{\"cpu\":%.1f,\"cpuKnown\":%s,\"memUsed\":%llu,\"memTotal\":"
                "%llu,\"memPct\":%.1f,\"memKnown\":%s,\"diskReadBps\":%.0f,"
                "\"diskWriteBps\":%.0f,\"diskKnown\":%s,\"netRecvBps\":%.0f,"
                "\"netSendBps\":%.0f,\"netKnown\":%s}",
                s.cpuPct, s.cpuKnown ? "true" : "false",
                static_cast<unsigned long long>(s.memUsed),
                static_cast<unsigned long long>(s.memTotal), s.memPct,
                s.memKnown ? "true" : "false", s.diskReadBps, s.diskWriteBps,
                s.diskKnown ? "true" : "false", s.netRecvBps, s.netSendBps,
                s.netKnown ? "true" : "false");
    return buf;
}

bool CliScanThreadMayBeLive() {
    // Ask only if a scan was ever started. Constructing the sampler here to
    // answer the question would probe SIO_TCP_INFO support and read the system
    // handle table during teardown, on every verb including `version`.
    SocketTrafficSampler* s = TrafficSamplerSlot();
    if (s == nullptr) return false;
    // A scan that is merely still finishing is given its budget to do so; only
    // one the kernel is holding past that leaves something running.
    return !s->ShutdownScanForExit();
}

}  // namespace wintcp
