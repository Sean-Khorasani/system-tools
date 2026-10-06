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
#include "TcpReasm.h"
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
    // There is deliberately no "jsonl" VALUE here: --format jsonl is this
    // same "json" with jsonLines set, so every rule keyed on the format -
    // "json prints no header line", the grouped-column refusal, the
    // --changes shape check - already applies to it unchanged, and the two
    // can only differ where they are meant to differ: the renderer.
    std::string format = "table";
    // --format jsonl: emit NDJSON instead of a JSON array - one object per
    // line, every line terminated, no enclosing brackets. An array has to be
    // read to its closing bracket before any of it is usable; a line at a
    // time is consumable the moment it lands, which is what a SIEM pipe or
    // `jq -c` reading a --watch stream needs. Off by default.
    bool jsonLines = false;
    // Column ids for delimited output; empty = frozen default set.
    std::vector<int> columns;
    // Quiet: print nothing; exit 0 when the view is non-empty, else 1.
    // For scripts: `list --filter ... --quiet && ...`.
    bool quiet = false;
    // Opt-in enrichment (each costs time; all off by default):
    //   traffic: per-PID byte totals via the bounded socket sampler
    //   dns:     blocking reverse-DNS for the printed rows (use --limit!)
    //   geoIpPath: load this .mmdb and join country codes ("" = off)
    //   signatures: verify each distinct process image (F5.3, "" = off)
    bool traffic = false;
    bool dns = false;
    std::wstring geoIpPath;
    // F5.3: verify each distinct process image with WinVerifyTrust, so the
    // Signature column and the `signed:` filter have something to report.
    // Opt-in because the trust provider is far too slow to run per pass by
    // default; cached per image path, so the cost is once per binary.
    bool signatures = false;
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
    // F5.3, for `details`, which shares this shape with `list`.
    bool signatures = false;
};

// Build the Details model for the single row matching 'select' (filter
// grammar, must resolve to exactly one live row) and render plain text.
CommandResult CmdDetails(SnapshotSource& source, const std::wstring& select,
                         const EnrichOptions& eo);

// Pure builder lifted out of MainWindow::BuildDetails: the GUI calls this
// with its live store, the CLI with a one-shot store. Needs the store for
// the per-PID connection list.
//
// 'threadsMayBlock' is the one behavioural difference between the two callers,
// and it is forced by the measurement in ProcessInfo.h. Enumerating a
// process's threads costs ~48 ms, which the GUI must never pay inline (it
// rebuilds this model on every refresh tick) and a one-shot command must pay
// or it never sees the result at all. Default false, so the safe case is the
// default and the caller that knows better has to say so.
DetailModel BuildDetailModel(const Connection& c, const ConnectionStore& store,
                            bool threadsMayBlock = false);

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
// 'lines' renders NDJSON rather than a JSON array (see ListOptions::
// jsonLines); the default keeps the array every existing caller expects.
std::string RenderJsonRows(const std::vector<Connection>& rows,
                           const std::vector<int>& cols, bool extraHostname,
                           bool lines = false);

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
// 'forceOverwrite' is R4. The write underneath is CREATE_ALWAYS, so without it
// an export aimed at an existing path destroys that file's contents with no
// warning and no way to tell afterwards - silent data loss, which this codebase
// treats as the failure rather than as a trade-off. The house convention is
// Presets::Save, which reports kExists and leaves the caller to authorise the
// overwrite; a non-interactive CLI cannot ask, so the authorisation arrives as
// a switch instead. The GUI passes true, because it asks the user first.
CommandResult CmdExport(SnapshotSource& source, const ListOptions& opt,
                        const std::wstring& outPath,
                        bool forceOverwrite = false);

// R4. True when `path` exists and the caller did not authorise replacing it, with
// the reason written to `err`. Declared here rather than left file-local so the
// selftest can drive it against a real file: the failure mode being fixed is a
// SILENT one, and a test that only checked an exit code would pass even if the
// refusal arrived after the write had already destroyed the file.
bool RefuseExistingOutput(const wchar_t* verb, const std::wstring& path,
                          bool forceOverwrite, std::wstring* err);

// ---- geoip -----------------------------------------------------------------
CommandResult CmdGeoIpLookup(SnapshotSource& source,
                             const std::wstring& geoIpPath,
                             const std::wstring& ip);
CommandResult CmdGeoIpInfo(SnapshotSource& source,
                           const std::wstring& geoIpPath);

// ---- capture (follow stream) -----------------------------------------------
//
// `capture` grew from "print some counters" into the command-line replacement
// for the GUI's Follow TCP stream, which was removed 2026-10-05 (todo.md 8.7
// G2). The three switches below are that replacement, and the reasons they are
// switches rather than defaults are worth keeping:
//
//   text  - the payload was already in memory and was being thrown away. On the
//           command line, printing it costs nothing extra and makes the verb
//           worth running without piping into anything.
//   out   - the capture file was deleted immediately after parsing. Saving it is
//           the difference between "here are the bytes" and "here is the
//           capture, open it in Wireshark".
//   dir   - a stream has two halves and most questions are about one of them.
//           Default is both, because guessing for the user is worse than
//           printing twice.
struct CaptureOptions {
    // Print the reassembled stream to stdout as a hex dump, one block per
    // direction (or just the one --dir selects).
    bool text = false;
    // Save the capture as pcapng here. Empty = do not keep it.
    std::wstring outPath;
    // R4: the pcapng write is CREATE_ALWAYS, so an existing file at outPath
    // would be destroyed silently. Same convention as export: refuse without
    // this, and the CLI's --force is what sets it.
    bool forceOverwrite = false;
    // Which half --text prints. Parsed by ParseCaptureDir.
    std::wstring dir = L"both";
};

CommandResult CmdCapture(SnapshotSource& source, const std::wstring& select,
                         const MutateOptions& mo,
                         unsigned secs = kCaptureSecsDefault,
                         const CaptureOptions& co = CaptureOptions());

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

// ---- the TLS report capture prints -----------------------------------------
//
// Declared here, and not left as a private helper in Commands.cpp, for one
// reason: it is pure formatting over two reassembled directions, and the
// repository's convention is that pure logic gets tested rather than trusted
// (see SocketTraffic.h's "split out for --selftest"). The test drives the real
// captured ClientHello that Bench.cpp already carries.
//
// It is here, and only here, because it is the only place a TLS session can be
// reported at all. Windows has no socket-level TLS ioctl - no SIO_TLS_INFO
// exists in the SDK, and TCP_INFO_v0 carries no TLS field - so the handshake
// bytes are the only source, and they are readable only for a connection the
// user chose to capture. The `tls` column therefore stays unpopulated; see the
// TlsInfo comment in Connection.h.
std::string TlsCaptureLines(const ReasmResult& toServer,
                            const ReasmResult& toClient,
                            const std::wstring& dir1Label,
                            const std::wstring& dir2Label);

}  // namespace wintcp
