// Commands.h
// Abstract command layer: every feature as a pure function over
// (ConnectionStore, SnapshotSource, ViewState, args). No HWND here and none
// may be added. Both the CLI and the GUI call these; the GUI only gathers
// args from widgets and renders the returned strings/models.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

// winsock2.h must precede windows.h, or the winsock declarations are lost
// behind the older winsock.h that windows.h pulls in.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tcpmib.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "BlockConn.h"
#include "Connection.h"
#include "ConnectionStore.h"
#include "DetailModel.h"
#include "Snapshot.h"
#include "SysStats.h"
#include "ViewState.h"

namespace wintcp {

// Exit codes shared by CLI and scripts:
//   0 success (or --quiet matched), 1 failure/empty, 2 bad args, 3 refused.
constexpr int kExitOk = 0;
constexpr int kExitFail = 1;
constexpr int kExitArgs = 2;
constexpr int kExitRefused = 3;   // mutating command without --yes

// CLI validation ranges (A4). One home for the numbers the parser enforces,
// the executor re-clamps, and the error strings print — owned here (not in
// CliCommands.cpp) so the in-process selftest can also assert on them, and so
// a future second front end cannot invent a second set. The `help` prose
// states the same ranges as literals (a `const char*` table cannot compute
// them); each prose line carries a keep-in-step comment, and golden asserts
// the prose, so either side drifting fails loudly somewhere.
constexpr unsigned kWatchMinSec = 1;
constexpr unsigned kWatchMaxSec = 3600;
constexpr unsigned kCaptureSecsMin = 1;
constexpr unsigned kCaptureSecsMax = 60;
constexpr unsigned kCaptureSecsDefault = 5;
constexpr unsigned kMinPort = 1;
constexpr unsigned kMaxPort = 65535;
constexpr unsigned kMinTag = 0;
constexpr unsigned kMaxTag = 4;
constexpr unsigned kMinCount = 1;

// One command invocation. Text is UTF-8, ready to print; the CLI writes out
// and err to stdout/stderr, the GUI renders them into dialogs/status.
struct CommandResult {
    int exitCode = kExitOk;
    std::string out;
    std::string err;
};

// Write straight to stdout WHILE a command is still running (the CLI's
// streaming path, D15: the table's fixed header goes out before the slow
// enrichment joins, the rows follow in CommandResult.out when they are
// ready). Defined in CliCommands.cpp next to the writer it wraps; a verb
// only calls it when its own options opt in (ListOptions::streamHeader), so
// in-process callers (selftest, GUI) never see a mid-render write.
void StreamOut(const std::string& s);

// ---- snapshot --------------------------------------------------------------
// Fill 'store' with one enriched pass. procStats/dns/geoIp are opt-in so a
// fast `list` pays for nothing it does not show.
bool BuildStoreSnapshot(SnapshotSource& source, ConnectionStore& store,
                        bool procStats, bool resolveDns, bool geoIp,
                        const wchar_t* geoIpPath, std::wstring* error);

// ---- list (conn) -----------------------------------------------------------
struct ListOptions {
    std::wstring filter;          // filter-box grammar, "" = all
    unsigned protoMask = kProtoMaskAll;
    DWORD stateFilter = kAllStates;
    int sortColumn = COL_PID;
    bool sortAsc = true;
    bool grouped = false;
    size_t limit = 0;             // 0 = no limit
    // Output shape: "table" (aligned columns for humans - D14), "csv",
    // "tsv" (raw tabs, the machine shape table used to be) or "json".
    std::string format = "table";
    // Column ids for delimited output; empty = frozen default set.
    std::vector<int> columns;
    // Quiet: print nothing; exit 0 when the view is non-empty, else 1.
    // For scripts: `list --filter ... --quiet && ...`.
    bool quiet = false;
    // Opt-in enrichment (each costs time; all off by default):
    //   traffic: per-PID byte totals via the bounded socket sampler
    //   dns:     blocking reverse-DNS for the printed rows (use --limit!)
    //   geoIpPath: load this .mmdb and join country codes ("" = off)
    bool traffic = false;
    bool dns = false;
    std::wstring geoIpPath;
    // changes: with --watch/--count, print only APPEAR/DISAPPEAR/STATE
    // deltas between polls instead of full snapshots.
    bool changes = false;
    // Stream the fixed column header to stdout as soon as the snapshot is
    // built - before the (potentially multi-second) enrichment joins - and
    // send the rows along in CommandResult.out afterwards (D15). The CLI's
    // list verb sets it; json has no header line, --quiet prints nothing,
    // and in-process callers leave it false so their CommandResult.out stays
    // a complete, self-contained rendering.
    bool streamHeader = false;
};

CommandResult CmdList(SnapshotSource& source, const ListOptions& opt);

// One `list` snapshot pass into a CALLER-owned store. CmdList owns a
// throwaway one; the watch loop keeps a store across ticks so per-row
// lifetime state (Duration) accumulates instead of restarting at 0s.
CommandResult CmdListInto(SnapshotSource& source, ConnectionStore& store,
                          const ListOptions& opt);

// True when a detached socket-traffic scan may still be running. `list
// --traffic` and friends start one, and a detached thread keeps the process
// alive at exit, so a one-shot CLI verb that has already written its output
// must not simply return: the caller exits hard instead. False when no scan
// was ever started (every other verb), so the normal `return` applies.
bool CliScanThreadMayBeLive();

// ---- ps / top --------------------------------------------------------------
// One aggregated row per process (like ps/lsof): pid, name, connections,
// cpu %, working set, disk I/O totals. Sorted by cpu desc by default.
struct PsOptions {
    std::wstring filter;          // matched against connection rows first
    std::string format = "table"; // table | csv | json
    size_t limit = 0;
    // Sort key: cpu (default, hottest first), mem, disk, conns, pid, process.
    // Unknown-valued readings always sort last.
    std::string sortBy = "cpu";
    bool quiet = false;           // print nothing; 0 = any process, else 1
};

// Pure render over an already-filled store: apply the view, format, honour
// quiet/limit. No snapshot, no network, no registry - this is what
// --selftest drives with synthetic rows.
//
// skipHeader: the caller already streamed the header (CmdListInto with
// streamHeader), so the body must come back WITHOUT it - concatenating
// StreamOut(header) and this body reproduces the unstreamed rendering byte
// for byte. Default false = the complete output every other caller wants.
CommandResult RenderList(ConnectionStore& store, const ListOptions& opt,
                         bool skipHeader = false);
CommandResult RenderPs(ConnectionStore& store, const PsOptions& opt);

// Filter analysis for the enrichment pre-pass (see Commands.cpp). A clause on
// a joined column (tx/rx/net, country, host) cannot be evaluated before its
// join, so those clauses are held back until the join has filled the values.
// Exposed because they are pure functions of the filter string - exactly the
// decision --selftest can pin without a snapshot or a network.
bool DependsOnEnrichment(FilterField f);
bool HasEnrichmentClause(const std::wstring& filter);
std::vector<FilterClause> PreJoinClauses(const std::wstring& filter);
// The stderr advice naming the switch an enrichment clause is missing
// (host: -> --dns, country: -> --db, rx/tx/net: -> --traffic), so an
// unanswerable filter says so instead of printing an empty table. Empty when
// nothing is missing or --quiet is set. Pure: no snapshot, no network.
std::string MissingEnrichmentAdvice(const ListOptions& opt);
// View-scoped enrichment for list/export: DNS + GeoIP + traffic over the
// printed rows only. False + err when the GeoIP database fails. 'advice'
// (optional) receives advisory hints - the missing-switch note above and the
// D3 unenrichable-window hint - never fatal, never on stdout. When the filter
// holds an enrichment clause the view is (re)applied here, AFTER the joins,
// so the caller renders - and D3 inspects - the rows the joins actually
// filled (an export otherwise wrote a silent 0-row file).
bool EnrichViewForList(ConnectionStore& store, const ListOptions& opt,
                       std::wstring* err, std::string* advice = nullptr);

CommandResult CmdPs(SnapshotSource& source, const PsOptions& opt);

// ---- details ---------------------------------------------------------------
// What to enrich a details snapshot with. All off except procStats by
// default; each extra source costs time, so the CLI asks explicitly.
struct EnrichOptions {
    bool procStats = true;
    bool traffic = false;
    bool dns = false;
    std::wstring geoIpPath;
};

// Build the Details model for the single row matching 'select' (filter
// grammar, must resolve to exactly one live row) and render plain text.
CommandResult CmdDetails(SnapshotSource& source, const std::wstring& select,
                         const EnrichOptions& eo);

// Pure builder lifted out of MainWindow::BuildDetails: the GUI calls this
// with its live store, the CLI with a one-shot store. Needs the store for
// the per-PID connection list.
DetailModel BuildDetailModel(const Connection& c, const ConnectionStore& store);

// ---- view / preset helpers -------------------------------------------------
// Non-widget core of MainWindow::CurrentPresetView / ApplyPresetView.
ViewState CurrentPresetViewFor(const std::wstring& filter, UINT32 colVisible,
                               int sortColumn, bool sortAsc, unsigned sources);
void ApplyPresetViewToStore(const ViewState& v, ConnectionStore& store,
                            std::vector<FilterClause>* out = nullptr);

// ---- kill / close ----------------------------------------------------------
// Core used by both CmdKill (which enumerates for the create time) and the
// GUI (which already holds the row). Takes the row's identity directly so
// neither caller re-derives it.
struct MutateOptions {
    bool yes = false;             // confirmed
    bool dryRun = false;          // print plan, change nothing
};

// Can this process be ended, and if not, why.
//
// D29. A pure function on purpose: the decision belongs to the abstract layer
// so the GUI's context menu, the GUI's handler and the CLI all apply ONE rule,
// and so the rule can be pinned by a selftest with no window and no real
// process. "Can it be ended" sounds like a property of the process, but it is
// really a property of *who is asking* - the answer differs for wintcp.exe
// depending on the caller - so both PIDs are parameters rather than one being
// read out of global state.
enum class PidVerdict {
    Ok,             // may be ended
    IsSelf,         // it is wintcp.exe itself
    Pseudo,         // PID 0 / System: no real process to end
    NotPermitted,   // exists, but this token cannot end it
};

// 'selfPid' is the caller's own PID. 'rowPid' is the row's. 'exists' is
// whether the row's process was still resolvable (PID 0 rows are not).
PidVerdict PidKillVerdict(DWORD rowPid, DWORD selfPid, bool exists);

// The user-facing refusal, or an empty string when PidKillVerdict() is Ok.
// One string per verdict so the menu tooltip, the handler's message and the
// CLI's error can never word the same refusal differently.
const wchar_t* PidKillRefusal(PidVerdict v);

CommandResult KillPid(DWORD pid, const FILETIME& create, bool createKnown,
                      const std::wstring& label, const MutateOptions& mo);
// Resolve --select to exactly one live row and kill its PID. The "find by
// port/name, kill exactly that" flow taskkill cannot do.
CommandResult CmdKillSelect(SnapshotSource& source, const std::wstring& select,
                            const MutateOptions& mo);
CommandResult CloseRow(const Connection& c, const MutateOptions& mo);
CommandResult BlockNow(const BlockRequest& req, const MutateOptions& mo);

// ---- shared serializers ----------------------------------------------------
// The same row->text used by CmdList/CmdExport AND by the GUI export and
// copy paths, so the two front ends cannot drift. 'cols' is any ColumnId
// list (usually the visible columns in display order, or a CLI column set).
// includeHeader=false suppresses the title line so the streaming CLI can
// print it first (D15); the default keeps the historical one-shot output.
std::string RenderDelimitedRows(const std::vector<Connection>& rows,
                                const std::vector<int>& cols, char delim,
                                bool rfcCsv, bool includeHeader = true);
// 'extraHostname' appends the hostname field the GUI's export schema has
// always carried; the CLI's frozen --json schema passes false.
std::string RenderJsonRows(const std::vector<Connection>& rows,
                           const std::vector<int>& cols, bool extraHostname);

// The frozen ten-column default shared by every delimited/JSON export.
std::vector<int> DefaultExportColumns();

CommandResult CmdKill(SnapshotSource& source, DWORD pid,
                      const MutateOptions& mo);
CommandResult CmdClose(SnapshotSource& source, const std::wstring& select,
                       const MutateOptions& mo);

// ---- block -----------------------------------------------------------------
CommandResult CmdBlock(SnapshotSource& source, const std::wstring& select,
                       const MutateOptions& mo);
CommandResult CmdUnblock(const std::wstring& address, UINT port,
                         const MutateOptions& mo);
CommandResult CmdBlocks();

// Map a connection row onto a firewall request (binary remote address, not
// the display string that carries the port). False when the row cannot be
// blocked (UDP, listening, unspecified peer).
bool ConnectionToBlockRequest(const Connection& c, BlockRequest* out,
                              std::wstring* whyNot);

// ---- bookmark --------------------------------------------------------------
CommandResult CmdBookmarkList(const std::string& format);
CommandResult CmdBookmarkAdd(const std::wstring& address, UINT port,
                             unsigned tag, const std::wstring& note);
CommandResult CmdBookmarkRemove(const std::wstring& address, UINT port);
CommandResult CmdBookmarkNote(const std::wstring& address, UINT port,
                              const std::wstring& note);
CommandResult CmdBookmarkColour(const std::wstring& address, UINT port,
                                unsigned tag);

// ---- preset ----------------------------------------------------------------
CommandResult CmdPresetList(const std::string& format);
CommandResult CmdPresetSave(const std::wstring& name, const ViewState& view,
                            bool overwrite);
CommandResult CmdPresetShow(const std::wstring& name);
CommandResult CmdPresetDelete(const std::wstring& name);
// 'overrides' carries the switches the USER typed on the command line
// (--limit, --columns, --sort, --asc/--desc, --format, --traffic, --dns,
// --db). They are layered over the SAVED view, so `preset apply --name web
// --limit 5` means "the web view, five rows" rather than silently ignoring
// --limit - which is what it did before (measured: 27 rows, rc 0).
CommandResult CmdPresetApply(SnapshotSource& source, const std::wstring& name,
                             ListOptions* appliedView,
                             const ListOptions* overrides = nullptr);

// ---- export ----------------------------------------------------------------
CommandResult CmdExport(SnapshotSource& source, const ListOptions& opt,
                        const std::wstring& outPath);

// ---- geoip -----------------------------------------------------------------
CommandResult CmdGeoIpLookup(SnapshotSource& source,
                             const std::wstring& geoIpPath,
                             const std::wstring& ip);
CommandResult CmdGeoIpInfo(SnapshotSource& source,
                           const std::wstring& geoIpPath);

// ---- capture (follow stream) -----------------------------------------------
CommandResult CmdCapture(SnapshotSource& source, const std::wstring& select,
                         const MutateOptions& mo,
                         unsigned secs = kCaptureSecsDefault);

// ---- stat (system CPU/mem/disk/net) ----------------------------------------
CommandResult CmdStat(SystemStatsSampler& sampler, const std::string& format);

// ---- column specs ----------------------------------------------------------
// Resolve a --columns value: a set name (default | minimal | full | wide) or
// an explicit comma list (proto,local,lport,...). Returns false (leaving
// 'out' untouched) when nothing in the spec names a column.
bool ResolveColumnSpec(const std::wstring& spec, std::vector<int>& out);

// ColumnId for a single column name, or -1 when unknown.
int ColumnIdForName(const std::wstring& name);

// ---- change events ---------------------------------------------------------
// Format drained RowChange events as text lines or JSON lines. 'prog'
// (nullptr = no filter) keeps only events whose row matches, so
// `list --watch --changes --filter ...` watches a subset.
//
// 'eventMask' selects which kinds are printed, as a bitmask of RowChangeKind
// (1<<kChangeAppear | 1<<kChangeDisappear | 1<<kChangeState). It is applied
// BEFORE the row filter, so asking for APPEAR only costs nothing and shows
// nothing else. kAllChangeKinds is the default and reproduces the old
// behaviour exactly. This is what answers "what opened while I was away" - the
// single most valuable question for an event feed - which was previously not
// expressible at all.
constexpr unsigned kAllChangeKinds =
    (1u << kChangeAppear) | (1u << kChangeDisappear) | (1u << kChangeState);

std::string FormatChangeEvents(const std::vector<RowChange>& events,
                               const std::string& format,
                               const std::wstring& stamp,
                               const std::vector<FilterClause>* prog,
                               unsigned eventMask = kAllChangeKinds);

// ---- misc ------------------------------------------------------------------
std::string ViewStateToJson(const ViewState& v);
bool ViewStateFromPreset(const PresetView& v, ViewState* out);
std::string FormatSystemStatsLine(const SystemStats& s);
std::string SystemStatsToJson(const SystemStats& s);

}  // namespace wintcp
