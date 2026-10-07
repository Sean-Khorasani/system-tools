// CliCommands.cpp
// See CliCommands.h. Verbs for users and for unattended scripts:
// every verb prints machine-readable table/csv/json, honours --watch/--count
// for polling, and never prompts: mutating verbs need --yes (exit 3 without).

#include "CliCommands.h"

#include <cstdio>
#include <cwchar>
#include <functional>
#include <set>
#include <string>
#include <vector>


#include "BuildInfo.h"
#include "Commands.h"
#include "ConnectionStore.h"
#include "CrashDump.h"   // CrashForTest (hidden `crashtest` verb, R1)
#include "Settings.h"   // kDefaultRefreshSec (B2: shared cadence value)
#include "Elevate.h"
#include "Snapshot.h"
#include "SocketTraffic.h"
#include "StreamCapture.h"   // ParseCaptureDir, for capture --dir
#include "SysStats.h"
#include "Utils.h"

namespace wintcp {
namespace {

// Slice the two watch loops sleep between Ctrl+C checks. 50 ms notices a
// keypress promptly without busy-spinning; both loops share it so the
// responsiveness guarantee is stated once. The comment at each loop already
// says why slices exist — this names how long one is.
constexpr DWORD kWatchPollMs = 50;

void WriteStream(HANDLE h, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (s.size() - off > (1u << 20)) ? (1u << 20) : (s.size() - off));
        DWORD written = 0;
        if (!::WriteFile(h, s.data() + off, chunk, &written, nullptr) ||
            written == 0)
            return;
        off += written;
    }
}

HANDLE AcquireStdHandle(DWORD id) {
    HANDLE h = ::GetStdHandle(id);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) return h;
    ::AttachConsole(ATTACH_PARENT_PROCESS);
    h = ::GetStdHandle(id);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) return h;
    return ::CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
}

void WriteOut(const std::string& s) {
    HANDLE h = AcquireStdHandle(STD_OUTPUT_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) WriteStream(h, s);
}

void WriteErr(const std::string& s) {
    HANDLE h = AcquireStdHandle(STD_ERROR_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) WriteStream(h, s);
}

void Emit(const CommandResult& r) {
    if (!r.out.empty()) WriteOut(r.out);
    if (!r.err.empty()) WriteErr(r.err);
}

const char* kHelp =
    "WinTCP - connections, processes and system stats (native, no dependencies)\r\n"
    "\r\n"
    "Usage: wintcp.exe <command> [switches]\r\n"
    "       wintcp.exe help <command>     full help + examples for one command\r\n"
    "       wintcp.exe <command> --help   same as above\r\n"
    "       wintcp.exe --help | -h        this overview\r\n"
    "\r\n"
    "Monitoring (ps/top/lsof-like, single snapshot unless --watch/--count):\r\n"
    "  list | conn   connections table (filter, sort, group, choose columns)\r\n"
    "  ps            one row per process (connections, CPU, memory, disk)\r\n"
    "  top           like ps, hottest CPU first\r\n"
    "  stat | sys    system CPU / memory / disk / network rates\r\n"
    "  details       full detail report for exactly one connection\r\n"
    "\r\n"
    "Actions (need --yes, support --dry-run; exit 3 when refused):\r\n"
    "  kill | close | block | unblock | blocks | capture | follow\r\n"
    "\r\n"
    "Library (bookmarks, presets, export, GeoIP):\r\n"
    "  bookmark | preset | export | geoip\r\n"
    "\r\n"
    "Other: version | help\r\n"
    "\r\n"
    "Exit codes: 0 ok, 1 failure or empty result, 2 bad arguments,\r\n"
    "            3 refused (mutating command without --yes).\r\n"
    "No command waits for a keypress. Only --watch polls, and --count N\r\n"
    "bounds any poll loop for scripts.\r\n"
    "\r\n"
    "Examples:\r\n"
    "  wintcp.exe list --filter \"port:443\" --format csv\r\n"
    "  wintcp.exe ps --limit 10\r\n"
    "  wintcp.exe top --count 3 --watch 1\r\n"
    "  wintcp.exe stat --format json\r\n"
    "  wintcp.exe help list\r\n";

// ---- per-command help ------------------------------------------------------
// One entry per verb: usage, switches, and copy-pasteable examples. Shown by
// `help <cmd>` and by `<cmd> --help|-h|help`. Aliases resolve to the same
// text, so `help conn` and `help list` agree by construction.
struct CommandHelp {
    const char* names;   // canonical + aliases, space separated
    const char* text;
};

const CommandHelp kCommandHelps[] = {
    {"list conn",
     "list | conn - live connection table (one snapshot, then exit)\r\n"
     "\r\n"
     "Usage: wintcp.exe list [--filter F] [--sort COL] [--asc|--desc]\r\n"
     "                       [--group] [--format table|csv|tsv|json|jsonl]\r\n"
     "                       [--columns SET|a,b,c] [--limit N] [--out FILE]\r\n"
     "                       [--quiet] [--watch [sec]] [--interval N] [--count N]\r\n"
     "\r\n"
     "  --filter F    filter-box grammar: chrome, port:443, pid:1000-2000,\r\n"
     "                state:estab, process:svchost, proto:udp, remote:1.2.3.4,\r\n"
     "                cpu:12, rx:2.0, rtt:100. Space = AND, exclude:X negates.\r\n"
     "                Live stats are thresholds, not text: cpu:12 means 12% or\r\n"
     "                more; mem:100 / disk:1.5 / rx:2.0 / tx:512 / net:2.5 are\r\n"
     "                MB or more (suffix KB/MB/GB/B, range mem:100-500).\r\n"
     "                rtt:100 / minrtt:100 are MILLISECONDS (ss -i units);\r\n"
     "                cwnd:65536 / retrans:1024 are bytes. A bare field name\r\n"
     "                means \"has that reading\", never \"matches all\": rtt:\r\n"
     "                selects only rows whose RTT could be measured.\r\n"
     "  --sort COL    sort column (default pid). --desc reverses.\r\n"
     "  --group       one row per process instead of per connection.\r\n"
     "  --format S    table (default) = aligned columns, header printed first,\r\n"
     "                rows as soon as they are ready; csv / tsv = raw\r\n"
     "                delimiters (tsv = tabs) for scripts; json = objects in\r\n"
     "                one array; jsonl = the same objects, one per line\r\n"
     "                (NDJSON), so a --watch stream can be consumed as it\r\n"
     "                runs instead of buffered to the closing bracket.\r\n"
     "  --columns C   default | minimal | full (wide = full), or a list:\r\n"
     "                proto,local,lport,remote,rport,state,pid,process,\r\n"
     "                service,host,path,traffic,rx,tx,nettotal,cpu,mem,\r\n"
     "                disk,duration,bandwidth,procspeed,tls,country,pinned,\r\n"
     "                note,rtt,minrtt,cwnd,retrans,ppid,integrity,signature.\r\n"
      "                tls is reserved: nothing populates it - use `capture --text`\r\n"
      "                to capture the handshake; see the note in the CLI reference.\r\n"
     "                Applies to every shape, json included.\r\n"
     // B6: the CLI has no saved column default and no widths. That is the
     // joint consequence of B2 (CLI stays hive-independent) and B3 (no config
     // file): there is nowhere to save one TO. So every run starts at the
     // frozen default set, and wide values elide with an ellipsis past their
     // width budget (cells hold 512 chars max). Said here so nobody files
     // "my columns did not stick" as a bug.
     "                No saved default: each run starts at the default set.\r\n"
     "                Wide cells elide with an ellipsis past their budget.\r\n"
     "                rtt/minrtt are MILLISECONDS (as ss -i prints them) and\r\n"
     "                cwnd/retrans are byte counts; all four need --traffic.\r\n"
     "                procspeed is the PROCESS rate summed over its sockets,\r\n"
     "                so it differs from bandwidth whenever a process holds\r\n"
     "                more than one connection.\r\n"
     "                ppid shows the parent as \"<pid> <name>\" and integrity\r\n"
     "                the mandatory level (\"+AC\" marks an AppContainer); both\r\n"
     "                are read on every pass. signature is the Authenticode\r\n"
     "                verdict - Signed, unsigned, or BAD SIG for a signed\r\n"
     "                image whose chain does not verify. \"unsigned\" is the\r\n"
     "                absence of a signature, not a finding, and needs\r\n"
     "                --signatures: without it the cell is \"—\".\r\n"
     "  --limit N     at most N rows.\r\n"
     "  --quiet       print nothing; exit 0 when any row matches, else 1.\r\n"
     "                An enrichment filter without its source switch (--dns,\r\n"
     "                --db, --traffic, --signatures) exits 2 instead: quiet\r\n"
     "                would turn \"unanswerable\" into \"no match\".\r\n"
     "  --out FILE    write the table to FILE instead of stdout. csv/tsv\r\n"
     "                carry the same BOM export uses; json/jsonl never\r\n"
     "                do; an existing file needs --force. One snapshot:\r\n"
     "                --out refuses --watch, --changes and --quiet.\r\n"
     "  --traffic     per-PID byte totals via one bounded socket scan over\r\n"
     "                the whole view (adds up to ~4 s on machines with wedged\r\n"
     "                sockets, usually ms). Sampled before sorting, so\r\n"
     "                `--sort nettotal` orders real totals.\r\n"
     "  --dns         reverse-DNS the printed rows only (slow; bound cost\r\n"
     "                with --limit).\r\n"
     "  --db FILE     load this .mmdb and join country codes for printed rows.\r\n"
     "  --signatures  verify each distinct process image with WinVerifyTrust\r\n"
     "                so the signature column and signed: filter have an\r\n"
     "                answer. SLOW: a certificate chain per image, cached per\r\n"
     "                image path for the run. Revocation is NOT checked, so a\r\n"
     "                revoked certificate can still read Signed.\r\n"
     "  --changes     with --watch/--count: print only APPEAR / DISAPPEAR /\r\n"
     "                STATE deltas between polls (table or json). The first\r\n"
     "                snapshot is the silent baseline.\r\n"
     "  --event LIST  with --changes: which kinds to print, from\r\n"
     "                appear,disappear,state (comma separated, any order,\r\n"
     "                case insensitive). Default: all three. This is how you\r\n"
     "                ask the one question an event feed exists for - what\r\n"
     "                OPENED - instead of wading through the closes.\r\n"
     // Keep-in-step: the 1..3600 range restates kWatchMinSec..kWatchMaxSec
     // (Commands.h), which the parser enforces, and "default 2" restates
     // kDefaultRefreshSec (Settings.h) — the shared cadence VALUE, not the
     // registry source (see Args::watchSec). A const char* table cannot
     // compute the numbers; golden asserts this prose, so drift fails loudly.
     "  --watch [s]   re-print every s seconds (default 2, 1..3600).\r\n"
     "  --count N     stop after N SNAPSHOTS (bounds --watch for scripts).\r\n"
    "                It does not cap how many change events a snapshot may\r\n"
    "                print, so `--changes --count 3` on a busy machine can\r\n"
    "                emit hundreds of lines; filter, --event or head it.\r\n"
     "\r\n"
     "A switch this command does not use is an ERROR (exit 2, naming it),\r\n"
     "never silently ignored: `export --changes` does not export deltas, it\r\n"
     "tells you so.\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe list\r\n"
     "  wintcp.exe list --filter \"port:443\" --format csv > https.csv\r\n"
     "  wintcp.exe list --filter \"process:chrome state:estab\" --sort pid\r\n"
     "  wintcp.exe list --group --sort process --limit 20\r\n"
     "  wintcp.exe list --format json --columns proto,remote,rport,process\r\n"
     "  wintcp.exe list --filter \"port:443\" --quiet && echo someone-is-on-https\r\n"
     "  wintcp.exe list --watch 5 --count 3\r\n"
     "  wintcp.exe list --watch 2 --changes --filter \"state:estab\"\r\n"
     "  wintcp.exe list --watch 1 --changes --event appear --count 20\r\n"
     "  wintcp.exe list --traffic --sort nettotal --desc --limit 10\r\n"
     "  wintcp.exe list --dns --limit 5\r\n"
     "  wintcp.exe list --db GeoLite2-Country.mmdb --columns remote,country,process\r\n"},
    {"ps",
     "ps - one row per process (ps/lsof-like, one snapshot, then exit)\r\n"
     "\r\n"
     "Usage: wintcp.exe ps [--filter F] [--format table|csv|json]\r\n"
     "                     [--sort cpu|mem|disk|conns|pid|process]\r\n"
     "                     [--limit N] [--quiet] [--watch [sec]] [--interval N] [--count N]\r\n"
     "\r\n"
     "Aggregates the connection rows by PID: process name, connection count,\r\n"
     "CPU %, working set and disk I/O. Default order is CPU, hottest first\r\n"
     "(unknown readings sort last); --sort picks another key. --filter\r\n"
     "selects connection rows before aggregating (same grammar as list).\r\n"
     "--quiet exits 0 when any process matches, else 1.\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe ps\r\n"
     "  wintcp.exe ps --limit 10\r\n"
     "  wintcp.exe ps --sort mem --limit 10\r\n"
     "  wintcp.exe ps --sort conns\r\n"
     "  wintcp.exe ps --format csv > procs.csv\r\n"
     "  wintcp.exe ps --filter \"proto:udp\"\r\n"
     "  wintcp.exe ps --watch 2 --count 5\r\n"},
    {"top",
     "top - hottest processes first (one snapshot, then exit)\r\n"
     "\r\n"
     "Usage: wintcp.exe top [--filter F] [--limit N]\r\n"
     "                      [--sort cpu|mem|disk|conns|pid|process]\r\n"
     "                      [--watch [sec]] [--interval N] [--count N]\r\n"
     "\r\n"
     "Same rows as ps, CPU-sorted. Unlike interactive top, a bare `top`\r\n"
     "prints once and exits - it never polls unless you ask: add --watch\r\n"
     "to poll (or --interval as its alias), --count N to bound the loop for scripts.\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe top --limit 5\r\n"
     "  wintcp.exe top --count 3 --watch 1\r\n"
     "  wintcp.exe top --filter \"chrome\"\r\n"},
    // Only `details`, deliberately: an earlier key also listed `show`, so
    // `help show` printed this page (exit 0) while `show` itself answered
    // "unknown command" (exit 2) - help advertising a spelling the
    // dispatcher does not have. A help key is a promise; keep it equal to
    // the verbs CanonicalVerbFor() resolves.
    {"details",
     "details - full report for exactly one connection\r\n"
     "\r\n"
     "Usage: wintcp.exe details --select <filter> [--traffic] [--dns]\r\n"
     "                          [--db FILE]\r\n"
     "\r\n"
     "The filter uses the list grammar and must match exactly one LIVE row:\r\n"
     "zero matches and ambiguous matches are errors (exit 1) - the command\r\n"
     "refuses to guess. Prints the same sections as the GUI Details window:\r\n"
     "process identity, live stats, TLS state, the connection, and the other\r\n"
     "connections owned by the same PID. --traffic fills the network totals,\r\n"
     "--dns the hostname, --db the country (same cost notes as list).\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe details --select \"pid:1234\"\r\n"
     "  wintcp.exe details --select \"pid:1234 remote:port:443\"\r\n"
     "  wintcp.exe details --select \"process:notepad local:port:50000\"\r\n"
     "  wintcp.exe details --select \"pid:1234\" --traffic\r\n"},
    {"kill",
     "kill - end a process by PID, or by the connection it owns\r\n"
     "\r\n"
     "Usage: wintcp.exe kill --pid N [--yes] [--dry-run] [--close|--force]\r\n"
     "       wintcp.exe kill --select <filter> [--yes] [--dry-run] [--close|--force]\r\n"
     "\r\n"
     "Asks the process to close first (WM_CLOSE), then terminates it if it\r\n"
     // Keep-in-step: "3 s" restates kKillGraceMs (Commands.cpp). Same
     // const-char*-table constraint as the --watch/--secs ranges above.
     "is still alive after 3 s. The PID is re-verified against its creation\r\n"
     "time first, so a recycled PID is refused, never killed. PID 0 and 4\r\n"
     "are always refused. --select resolves through the list grammar to\r\n"
     "exactly one row and kills its owner: `kill --select \"local:port:8080\"`\r\n"
     "answers \"which process holds this port, and end it\" in one step.\r\n"
     "Without --yes the command prints its plan and exits 3; --dry-run\r\n"
     "prints the plan and changes nothing (exit 0).\r\n"
     "\r\n"
     "Modes (mutually exclusive; together they are exit 2):\r\n"
     "  --close       WM_CLOSE only, wait the 3 s, NEVER force. If the\r\n"
     "                process survives, kill fails (exit 1) and says so.\r\n"
     "  --force       terminate immediately: no WM_CLOSE, no wait.\r\n"
     "  (default)     WM_CLOSE, then force after 3 s - the hybrid above.\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe kill --pid 1234 --dry-run\r\n"
     "  wintcp.exe kill --pid 1234 --yes\r\n"
     "  wintcp.exe kill --select \"local:port:8080\" --dry-run\r\n"
     "  wintcp.exe kill --select \"process:helper state:listen\" --yes\r\n"},
    {"close",
     "close - drop one established TCP connection (needs --yes)\r\n"
     "\r\n"
     "Usage: wintcp.exe close --select <filter> [--yes] [--dry-run]\r\n"
     "\r\n"
     "The filter must match exactly one live row, and it must be an\r\n"
     "established IPv4 TCP connection: Windows exposes no public API for\r\n"
     "tearing down IPv6 connections, and UDP has no connection to drop.\r\n"
     "Needs elevation (verified: medium-integrity closes fail); without\r\n"
     "--yes exits 3; --dry-run prints the plan (exit 0).\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe close --select \"pid:1234 remote:port:443\" --dry-run\r\n"
     "  wintcp.exe close --select \"pid:1234 remote:port:443\" --yes\r\n"},
    {"block",
     "block - kill a connection and firewall-block its peer (needs --yes)\r\n"
     "\r\n"
     "Usage: wintcp.exe block --select <filter> [--yes] [--dry-run]\r\n"
     "\r\n"
     "Two layers: the live connection is torn down now (IPv4 only - see\r\n"
     "`close`), and Windows Firewall outbound rules stop it coming back\r\n"
     "(both families). Both layers need elevation; unelevated runs exit 1\r\n"
     "saying so. Without --yes exits 3; --dry-run prints the plan (exit 0).\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe block --select \"remote:93.184.216.34\" --dry-run\r\n"
     "  wintcp.exe block --select \"pid:1234\" --yes\r\n"},
    {"unblock",
     "unblock - remove WinTCP firewall rules for a peer (needs --yes)\r\n"
     "\r\n"
     "Usage: wintcp.exe unblock --address <ip> --port N [--yes] [--dry-run]\r\n"
     "\r\n"
     "Removes exactly the rules `block` created for this peer. Needs\r\n"
     "elevation. Without --yes exits 3.\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe unblock --address 93.184.216.34 --port 443 --dry-run\r\n"
     "  wintcp.exe unblock --address 93.184.216.34 --port 443 --yes\r\n"},
    {"blocks",
     "blocks - how many WinTCP firewall rules exist right now\r\n"
     "\r\n"
     "Usage: wintcp.exe blocks\r\n"
     "\r\n"
     "Counts rules carrying the WinTCP tag, in any direction or family.\r\n"
     "Always succeeds (exit 0), no elevation needed to count.\r\n"
     "\r\n"
     "The count comes from a ledger this tool wrote when `block` succeeded,\r\n"
     "at %APPDATA%\\WinTCP\\blocked.txt. A ledger that cannot be read with\r\n"
     "confidence is REFUSED, not counted: over 4 MiB, a line over 4096 bytes,\r\n"
     "or a line that is not valid UTF-8. The count still prints on stdout so\r\n"
     "a script keeps its number, and stderr says why it must not be trusted.\r\n"
     "Treat that as \"unknown\", never as \"you have no blocks\".\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe blocks\r\n"},
    {"bookmark",
     "bookmark - mark remote peers (stored per-user in HKCU)\r\n"
     "\r\n"
     "Usage: wintcp.exe bookmark list [--format table|csv|json]\r\n"
     // Keep-in-step: 0..4 restates kMinTag..kMaxTag (Commands.h), pinned by
     // static_assert to kBookmarkTagNone..Green (Commands.cpp).
     "       wintcp.exe bookmark add --address <ip> --port N [--tag 0..4]\r\n"
     "                               [--note TEXT]\r\n"
     "       wintcp.exe bookmark remove --address <ip> --port N\r\n"
     "       wintcp.exe bookmark note --address <ip> --port N --note TEXT\r\n"
     "       wintcp.exe bookmark colour --address <ip> --port N --tag 0..4\r\n"
     "\r\n"
     "Identity is remote address + remote port (what survives a reconnect).\r\n"
     "Tags: 0 none, 1 red, 2 amber, 3 blue, 4 green. Addresses normalise\r\n"
     "(::ffff:1.2.3.4 becomes 1.2.3.4); placeholders like 0.0.0.0 refuse.\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe bookmark list\r\n"
     "  wintcp.exe bookmark add --address 93.184.216.34 --port 443 --tag 1 --note \"suspicious\"\r\n"
     "  wintcp.exe bookmark note --address 93.184.216.34 --port 443 --note \"cleared by netops\"\r\n"
     "  wintcp.exe bookmark colour --address 93.184.216.34 --port 443 --tag 4\r\n"
     "  wintcp.exe bookmark remove --address 93.184.216.34 --port 443\r\n"},
    {"preset",
     "preset - saved views (filter, sort, columns; stored per-user in HKCU)\r\n"
     "\r\n"
     "Usage: wintcp.exe preset list [--format table|json]\r\n"
     "       wintcp.exe preset save --name N [--force] [--filter F]\r\n"
     "                              [--sort COL] [--asc|--desc]\r\n"
     "       wintcp.exe preset show --name N\r\n"
     "       wintcp.exe preset delete --name N\r\n"
     "       wintcp.exe preset apply --name N [--limit N] [--columns SET]\r\n"
     "                                  [--format S] [--sort COL] [--traffic]\r\n"
     "\r\n"
     "`save` never overwrites: an existing name exits 3 unless --force is\r\n"
     "given. `apply` prints the current table through the saved view, and the\r\n"
     "output switches you type on the command line are LAYERED OVER it: `apply\r\n"
     "--name web --limit 5 --columns pid,remote` is the web view, five rows,\r\n"
     "two columns. (Before, those switches were parsed and dropped, so the\r\n"
     "whole saved table came out in the preset's own columns.)\r\n"
     "The same presets appear in the GUI File menu.\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe preset save --name web --filter \"port:443\" --sort pid\r\n"
     "  wintcp.exe preset list\r\n"
     "  wintcp.exe preset show --name web\r\n"
     "  wintcp.exe preset apply --name web\r\n"
     "  wintcp.exe preset apply --name web --limit 10 --columns pid,process,remote\r\n"
     "  wintcp.exe preset delete --name web\r\n"},
    {"export",
     "export - write the table to a file\r\n"
     "\r\n"
     "Usage: wintcp.exe export --out FILE [--format csv|tsv|json]\r\n"
     "                         [--filter F] [--sort COL] [--asc|--desc]\r\n"
     "                         [--columns SET|a,b,c] [--group] [--force]\r\n"
     "                         [--traffic] [--dns] [--db FILE] [--quiet]\r\n"
     "\r\n"
     "Same content the GUI export writes: CSV/TSV carry a BOM for Excel,\r\n"
     "JSON never does. The whole view is exported: --limit is REFUSED (exit\r\n"
     "2), not ignored, because an export that silently truncated would lie\r\n"
     "about its row count. Without --format the extension of --out picks the\r\n"
     "format (.csv, .tsv, .json); any other name needs an explicit --format.\r\n"
     "--quiet still writes the file but prints no confirmation line; the exit\r\n"
     "code still says whether any row was written.\r\n"
     "\r\n"
     "--out REPLACES the whole file, so an existing path is REFUSED (exit 2)\r\n"
     "unless --force is given. The GUI asks before overwriting; a script\r\n"
     "cannot be asked, so it has to say so.\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe export --out conns.csv --filter \"tcp:\"\r\n"
     "  wintcp.exe export --out conns.json --format json --filter \"port:443\"\r\n"
     "  wintcp.exe export --out procs.tsv --format tsv --columns proto,pid,process,state\r\n"
     "  wintcp.exe export --out by-proc.csv --group --traffic --sort nettotal --desc\r\n"
     "  wintcp.exe export --out today.csv --force\r\n"
     "  wintcp.exe export --out snap.csv --quiet\r\n"},
    {"geoip",
     "geoip - country lookup from a local MaxMind database\r\n"
     "\r\n"
     "Usage: wintcp.exe geoip lookup --db FILE <ip>\r\n"
     "       wintcp.exe geoip info --db FILE\r\n"
     "\r\n"
     "No database ships with WinTCP (MaxMind licensing) and none is ever\r\n"
     "downloaded: bring your own .mmdb. `info` describes the file\r\n"
     "(version, records, size). Link-local and private addresses have no\r\n"
     "country and report the placeholder.\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe geoip info --db GeoLite2-Country.mmdb\r\n"
     "  wintcp.exe geoip lookup --db GeoLite2-Country.mmdb 8.8.8.8\r\n"},
    {"capture follow",
     "capture - follow one TCP stream via pktmon (needs elevation)\r\n"
     "\r\n"
     "Usage: wintcp.exe capture --select <filter> [--secs N]\r\n"
     "                          [--yes] [--dry-run]\r\n"
     "                          [--text] [--dir both|first|second]\r\n"
     "                          [--out FILE] [--force]\r\n"
     "\r\n"
     "Installs a pktmon filter for the selected connection, records N\r\n"
     // Keep-in-step: default 5 / 1..60 restate kCaptureSecsDefault/Min/Max
     // (Commands.h), enforced by the parser and re-clamped by the executor.
     "seconds (default 5, 1..60), converts to pcapng, reassembles both\r\n"
     "directions and reports packet/byte counts. The filter must match\r\n"
     "exactly one live row. Unelevated runs exit 1 with a plain message -\r\n"
     "the command never tries to elevate itself. Without --yes exits 3.\r\n"
     "\r\n"
     "Output switches:\r\n"
     "  --text         print the reassembled stream to stdout as a hex dump,\r\n"
     "                 one block per direction, labelled by the endpoint that\r\n"
     "                 sent it. This is the command-line equivalent of the\r\n"
     "                 GUI's Follow TCP stream, removed 2026-10-05.\r\n"
     "  --dir LIST     which direction --text prints: both (default), first\r\n"
     "                 (a) or second (b).\r\n"
     "  --out FILE     save the capture as pcapng, byte-for-byte, so it opens\r\n"
     "                 in Wireshark or tshark. An existing path is REFUSED\r\n"
     "                 (exit 2) unless --force is given, because --out replaces\r\n"
     "                 the whole file and this command cannot ask.\r\n"
     "\r\n"
     "pktmon is a sampling driver, not a tap: only what happens AFTER the\r\n"
     "filter is armed is recorded, so a quiet connection legitimately returns\r\n"
     "nothing. Generate traffic during the window, or widen --secs.\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe capture --select \"pid:1234\" --dry-run\r\n"
     "  wintcp.exe capture --select \"pid:1234 remote:port:443\" --secs 10 --yes\r\n"
     "  wintcp.exe capture --select \"pid:1234\" --secs 20 --text --dir b --yes\r\n"
     "  wintcp.exe capture --select \"pid:1234\" --secs 20 --out c:\\tmp\\s.pcapng --yes\r\n"},
    {"stat sys",
     "stat - system CPU / memory / disk / network (one sample, then exit)\r\n"
     "\r\n"
     "Usage: wintcp.exe stat [--format table|json] [--watch [sec]]\r\n"
     "                       [--interval N] [--count N]\r\n"
     "\r\n"
     "Each run samples for ~1 s first (CPU and network are rates between\r\n"
     "two reads). Disk shows n/a when the PDH counters are disabled.\r\n"
     "Run `version` for this machine's optional-feature report: it names\r\n"
     "what is unavailable and what would provide it.\r\n"
     "Add --watch (or its --interval alias) to poll, --count N to bound it for scripts.\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe stat\r\n"
     "  wintcp.exe stat --format json\r\n"
     "  wintcp.exe stat --watch 2 --count 5\r\n"
     "  wintcp.exe stat --interval 2 --count 5\r\n"},
    {"version",
     "version - build banner and capability summary\r\n"
     "\r\n"
     "Usage: wintcp.exe version\r\n"
     "\r\n"
     "Same text as the GUI About box: version, what the binary is, and the\r\n"
     "state of this run (elevation, traffic source). Always exits 0.\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe version\r\n"},
    {"help",
     "help - this help, or full help for one command\r\n"
     "\r\n"
     "Usage: wintcp.exe help [command]\r\n"
     "       wintcp.exe <command> --help | -h | help\r\n"
     "\r\n"
     "Examples:\r\n"
     "  wintcp.exe help\r\n"
     "  wintcp.exe help list\r\n"
     "  wintcp.exe capture --help\r\n"},
    // `selftest` and `bench` used to be entries here, and `--uiharness` a
    // switch handled in main.cpp. All three moved to wintcp-tests.exe: the test
    // code is no longer compiled into the product, so advertising a verb the
    // product cannot answer would be a lie told by the help text. See
    // wintcp/tests/TestMain.cpp.
};
constexpr size_t kCommandHelpCount =
    sizeof(kCommandHelps) / sizeof(kCommandHelps[0]);

// Canonicalise aliases: conn->list, follow->capture, sys->stat.
// Returns "" when unknown.
//
// This is the HELP-TOPIC resolver - PrintCommandHelp is its only caller - so
// it must resolve exactly the same set CanonicalVerbFor() does. It used to
// also map show->details while CanonicalVerbFor() had no `show`, which made
// `help show` print the details page at exit 0 while `show` itself answered
// "unknown command" at exit 2: help advertising a verb spelling that nothing
// else would run. `show` is registered nowhere else - not in kVerbSwitches,
// not in the help overview, not in any document, never exercised by any test -
// so it was removed rather than quietly promoted to a real command.
std::wstring CanonicalCommand(const std::wstring& cmd) {
    const std::wstring n = ToLowerW(cmd);
    if (n == L"conn") return L"list";
    if (n == L"follow") return L"capture";
    if (n == L"sys") return L"stat";
    return n;
}

// True for the help spellings accepted after a command name.
bool IsHelpToken(const std::wstring& t) {
    return t == L"help" || t == L"--help" || t == L"-h" || t == L"-?" ||
           t == L"/?" || t == L"/help";
}

// Switches that consume the following token as their value. The help
// pre-scan skips those values, so `--note help` keeps its value instead
// of turning into a help request.
bool TakesValue(const std::wstring& t) {
    return t == L"--filter" || t == L"--sort" || t == L"--format" ||
           t == L"--columns" || t == L"--limit" || t == L"--watch" ||
            t == L"--interval" ||
           t == L"--count" || t == L"--out" || t == L"--select" ||
           t == L"--pid" || t == L"--address" || t == L"--port" ||
           t == L"--tag" || t == L"--note" || t == L"--name" ||
           t == L"--db" || t == L"--secs" || t == L"--event" ||
           t == L"--dir";
}

// D25: "appear", "disappear", "state", in any case, comma-separated, in any
// order. An empty value means "all three", which is what `--event` alone does.
// Returns false for an unrecognised name, and the caller reports the accepted
// list - guessing would silently drop a kind the user asked for.
bool ParseEventMask(const std::wstring& v, unsigned* mask) {
    unsigned m = 0;
    size_t i = 0;
    while (i <= v.size()) {
        size_t comma = v.find(L',', i);
        if (comma == std::wstring::npos) comma = v.size();
        std::wstring t = ToLowerW(v.substr(i, comma - i));
        while (!t.empty() && ::iswspace(t.front())) t.erase(t.begin());
        while (!t.empty() && ::iswspace(t.back())) t.pop_back();
        if (!t.empty()) {
            if (t == L"appear" || t == L"new" || t == L"open")
                m |= 1u << kChangeAppear;
            else if (t == L"disappear" || t == L"close" || t == L"gone")
                m |= 1u << kChangeDisappear;
            else if (t == L"state" || t == L"change")
                m |= 1u << kChangeState;
            else
                return false;
        }
        if (comma == v.size()) break;
        i = comma + 1;
    }
    *mask = m;
    return true;
}

// Print full help for one (canonical or alias) command name. False when the
// name is not a command, so the caller can report it precisely.
bool PrintCommandHelp(const std::wstring& cmd) {
    const std::wstring canon = CanonicalCommand(cmd);
    for (size_t i = 0; i < kCommandHelpCount; ++i) {
        std::string names = kCommandHelps[i].names;
        // Match any alias in the space-separated list.
        size_t pos = 0;
        for (;;) {
            const size_t sp = names.find(' ', pos);
            const std::string one =
                (sp == std::string::npos) ? names.substr(pos)
                                          : names.substr(pos, sp - pos);
            if (WideToUtf8(canon) == one) {
                WriteOut(kCommandHelps[i].text);
                return true;
            }
            if (sp == std::string::npos) break;
            pos = sp + 1;
        }
    }
    return false;
}

struct Args {
    std::wstring filter;
    std::wstring sort = L"pid";
    bool hasSort = false;
    bool desc = false;
    bool asc = false;
    bool group = false;
    std::string format = "table";
    bool formatSet = false;  // --format given explicitly (vs. the default)
    std::wstring columns;
    unsigned limit = 0;
    bool watch = false;
    // B2: the VALUE is shared with the GUI cadence (kDefaultRefreshSec), the
    // SOURCE is deliberately not the registry. A CLI that read HKCU would
    // behave differently per user hive, elevation and machine — poison for
    // scripts — so `--watch` with no seconds means 2 s everywhere, always.
    unsigned watchSec = kDefaultRefreshSec;
    unsigned count = 0;   // 0 = infinite (until Ctrl+C)
    bool yes = false;
    bool dryRun = false;
    bool force = false;
    // 9.3.6: kill's WM_CLOSE-only mode. `--force` shares `force` with
    // export/capture - the word means "don't ask / don't wait" in both.
    bool closeOnly = false;
    std::wstring out;
    std::wstring select;
    DWORD pid = 0;
    bool hasPid = false;   // --pid was typed, even as --pid 0 (see kill below)
    std::wstring address;
    unsigned port = 0;
    unsigned tag = 0;
    bool hasTag = false;
    std::wstring note;
    std::wstring name;
    std::wstring db;
    std::wstring ip;
    std::wstring sub;     // bookmark/preset/geoip subverb
    bool quiet = false;   // list/ps: no output, rc answers
    unsigned secs = kCaptureSecsDefault;   // recording window; range in Commands.h
    // capture: the CLI replacement for the removed GUI Follow TCP stream
    // (todo.md 8.7 G2). `--out` is NOT re-declared here - it already exists as
    // `out` for list/export, means the same thing (a file to write), and giving
    // one switch two homes is how a reader ends up with two different defaults.
    bool captureText = false;
    std::wstring captureDir = L"both";
    bool traffic = false; // list/details: per-PID byte totals (bounded scan)
    bool dns = false;     // list/details: reverse-DNS the printed rows
    // F5.3: verify each process image with WinVerifyTrust. OFF by default and
    // opt-in for a measured reason: WinVerifyTrust builds a certificate chain
    // and would otherwise be paid for every process on every refresh. Cached per
    // image path, so the cost is once per distinct binary, not once per row.
    bool signatures = false;   // list/details: Authenticode verdicts
    bool changes = false; // list --watch: deltas only
    // D25: which change kinds to print, as a bitmask of RowChangeKind. The
    // default is all three. Before this there was NO way to select a kind, so
    // the "what opened while I was away" question - the single most valuable
    // use of an event feed - could not be asked. `--filter event:appear` did
    // not work either: "event" was not a field name, so the token degraded to
    // a substring search for the literal text "event:appear" and matched
    // nothing while exiting 0.
    unsigned eventMask = 0x7u;   // appear | disappear | state
    bool hasEvent = false;
    // D24: every switch literally TYPED on this command line. The verb
    // allow-list reads this, because a defaulted flag and an asked-for flag
    // are indistinguishable in the fields above (every boolean switch defaults
    // to false) and only the second is an error on the wrong verb.
    std::set<std::wstring> given;
};

bool ParseUint(const std::wstring& t, unsigned* out) {
    if (t.empty()) return false;
    unsigned long acc = 0;
    for (wchar_t ch : t) {
        if (ch < L'0' || ch > L'9') return false;
        acc = acc * 10 + static_cast<unsigned long>(ch - L'0');
        if (acc > 4000000000ul) return false;
    }
    *out = static_cast<unsigned>(acc);
    return true;
}

// "need 1..3600" style range fragment, built from the Commands.h constants so
// the parser check and the error a user reads cannot disagree about the
// range. Every range error below goes through here; a hand-typed range string
// anywhere else is a latent lie.
std::string RangeErr(const char* what, unsigned lo, unsigned hi) {
    return std::string("bad ") + what + " (need " + std::to_string(lo) +
           ".." + std::to_string(hi) + ")";
}

int ColumnIdFor(const std::wstring& name) {
    return ColumnIdForName(name);
}

std::vector<int> ParseColumns(const std::wstring& csv) {
    std::vector<int> out;
    if (ResolveColumnSpec(csv, out)) return out;
    return std::vector<int>();
}

// 9.3.3/D33: nearest-known-name suggestion for a token the parser has just
// rejected. Defined beside kSwitchNames below; declared here because the
// parser is the first place a typo is seen.
std::string SuggestSwitchName(const std::wstring& typed);

// Parse switches starting at argv[pos]. Returns "" on ok, else an error.
std::string ParseSwitches(int argc, wchar_t** argv, int pos, Args* a) {
    for (int i = pos; i < argc; ++i) {
        std::wstring t = argv[i] != nullptr ? argv[i] : L"";
        // D24: remember that this switch was actually TYPED, before it is
        // consumed. A default value is not evidence of intent: --group and
        // --dns and --quiet all default to false, so "the flag is set" cannot
        // distinguish "asked for" from "defaulted", and the verb allow-list
        // needs exactly that distinction.
        if (!t.empty() && t[0] == L'-') a->given.insert(t);
        auto need = [&](std::wstring* out) -> bool {
            if (i + 1 >= argc) return false;
            *out = argv[++i] != nullptr ? argv[i] : L"";
            return true;
        };
        if (t == L"--filter") {
            if (!need(&a->filter)) return "missing value for --filter";
        } else if (t == L"--sort") {
            std::wstring v;
            if (!need(&v)) return "missing value for --sort";
            a->sort = v;
            a->hasSort = true;
        } else if (t == L"--desc") {
            a->desc = true;
        } else if (t == L"--asc") {
            a->asc = true;
        } else if (t == L"--group") {
            a->group = true;
        } else if (t == L"--format") {
            std::wstring v;
            if (!need(&v)) return "missing value for --format";
            a->format = WideToUtf8(ToLowerW(v));
            a->formatSet = true;
        } else if (t == L"--columns") {
            if (!need(&a->columns)) return "missing value for --columns";
        } else if (t == L"--limit") {
            std::wstring v;
            if (!need(&v)) return "missing value for --limit";
            if (!ParseUint(v, &a->limit)) return "bad --limit (need number)";
        } else if (t == L"--watch") {
            a->watch = true;
            if (i + 1 < argc && argv[i + 1] != nullptr &&
                argv[i + 1][0] != L'-') {
                unsigned s = 0;
                if (!ParseUint(argv[i + 1], &s) || s < kWatchMinSec ||
                    s > kWatchMaxSec)
                    return RangeErr("--watch", kWatchMinSec, kWatchMaxSec);
                a->watchSec = s;
                ++i;
            }
        } else if (t == L"--interval") {
            // 9.2.3: alias for --watch; same range (1..3600), same parsing shape.
            a->watch = true;
            if (i + 1 < argc && argv[i + 1] != nullptr &&
                argv[i + 1][0] != L'-') {
                unsigned s = 0;
                if (!ParseUint(argv[i + 1], &s) || s < kWatchMinSec ||
                    s > kWatchMaxSec)
                    return RangeErr("--interval", kWatchMinSec, kWatchMaxSec);
                a->watchSec = s;
                ++i;
            }
        } else if (t == L"--count") {
            std::wstring v;
            if (!need(&v)) return "missing value for --count";
            if (!ParseUint(v, &a->count) || a->count < kMinCount)
                return "bad --count (need number >= 1)";
        } else if (t == L"--yes" || t == L"-y") {
            a->yes = true;
        } else if (t == L"--dry-run") {
            a->dryRun = true;
        } else if (t == L"--force") {
            a->force = true;
        } else if (t == L"--out") {
            if (!need(&a->out)) return "missing value for --out";
        } else if (t == L"--text") {
            // capture only. No value: the only question is whether to print the
            // stream, and "how much of it" is --dir's job, not this switch's.
            a->captureText = true;
        } else if (t == L"--dir") {
            std::wstring v;
            if (!need(&v)) return "missing value for --dir";
            // Parsed HERE rather than only in the verb, so a typo is refused
            // before anything is captured. ParseCaptureDir is the single
            // spelling table, so the parser and the verb cannot disagree about
            // which names are valid.
            CaptureDir ignored = CaptureDir::kBoth;
            if (!ParseCaptureDir(v, &ignored))
                return "bad --dir (use both, first/a, or second/b)";
            a->captureDir = v;
        } else if (t == L"--select") {
            if (!need(&a->select)) return "missing value for --select";
        } else if (t == L"--pid") {
            std::wstring v;
            if (!need(&v)) return "missing value for --pid";
            unsigned p = 0;
            if (!ParseUint(v, &p)) return "bad --pid (need number)";
            a->pid = p;
            // Presence, not value. PID 0 is a real input (every TIME_WAIT and
            // wildcard row): without this, `kill --pid 0` reads as "no --pid
            // given" and reports a MISSING argument, which is a lie - the user
            // typed one - and makes the pseudo-PID guard unreachable by name.
            a->hasPid = true;
        } else if (t == L"--address") {
            if (!need(&a->address)) return "missing value for --address";
        } else if (t == L"--port") {
            std::wstring v;
            if (!need(&v)) return "missing value for --port";
            if (!ParseUint(v, &a->port) || a->port < kMinPort ||
                a->port > kMaxPort)
                return RangeErr("--port", kMinPort, kMaxPort);
        } else if (t == L"--tag") {
            std::wstring v;
            if (!need(&v)) return "missing value for --tag";
            if (!ParseUint(v, &a->tag) || a->tag < kMinTag ||
                a->tag > kMaxTag)
                return RangeErr("--tag", kMinTag, kMaxTag);
            a->hasTag = true;
        } else if (t == L"--note") {
            if (!need(&a->note)) return "missing value for --note";
        } else if (t == L"--name") {
            if (!need(&a->name)) return "missing value for --name";
        } else if (t == L"--db") {
            if (!need(&a->db)) return "missing value for --db";
        } else if (t == L"--traffic") {
            a->traffic = true;
        } else if (t == L"--dns") {
            a->dns = true;
        } else if (t == L"--signatures") {
            // F5.3. No value: the only question is whether to pay for the
            // verification. A threshold for "how trusted is it" would be a
            // second, differently-worded way of asking the same thing, and the
            // `signed:` filter already covers "give me the good ones".
            a->signatures = true;
        } else if (t == L"--changes") {
            a->changes = true;
        } else if (t == L"--event") {
            std::wstring v;
            // An optional value: `--event` with nothing after it means "all
            // three", but a VALUE that is not a kind name is an error rather
            // than a silently ignored token. `need()` cannot express that, so
            // the value is taken from the next token only when there is one and
            // it does not start with '-'.
            if (i + 1 < argc && argv[i + 1] != nullptr &&
                argv[i + 1][0] != L'-') {
                v = argv[i + 1];
                ++i;
            }
            if (!ParseEventMask(v, &a->eventMask))
                return "bad --event '" + WideToUtf8(v) +
                       "' (want appear,disappear,state in any order)";
            a->hasEvent = true;
        } else if (t == L"--quiet" || t == L"-q") {
            a->quiet = true;
        } else if (t == L"--close") {
            // 9.3.6: kill --close = WM_CLOSE only, never forced.
            a->closeOnly = true;
        } else if (t == L"--secs") {
            std::wstring v;
            if (!need(&v)) return "missing value for --secs";
            if (!ParseUint(v, &a->secs) || a->secs < kCaptureSecsMin ||
                a->secs > kCaptureSecsMax)
                return RangeErr("--secs", kCaptureSecsMin, kCaptureSecsMax);
        } else if (!t.empty() && t[0] == L'-') {
            // 9.3.3/D33: a near-miss switch gets its correction inline.
            const std::string sugg = SuggestSwitchName(t);
            return "unknown switch: " + WideToUtf8(t) +
                   (sugg.empty() ? std::string()
                                 : "\r\nDid you mean " + sugg + "?");
        } else {
            // positional: bookmark/preset/geoip subverb or geoip ip
            if (a->sub.empty())
                a->sub = t;
            else if (a->ip.empty())
                a->ip = t;
            else
                return "unexpected argument: " + WideToUtf8(t);
        }
    }
    return "";
}

ListOptions ToListOptions(const Args& a) {
    ListOptions o;
    o.filter = a.filter;
    o.sortColumn = ColumnIdFor(a.sort);
    if (o.sortColumn < 0) o.sortColumn = COL_PID;
    o.sortAsc = !a.desc;
    o.grouped = a.group;
    o.limit = a.limit;
    // --format jsonl is `json` with jsonLines set, not a fourth format: every
    // format gate downstream (the header rule, the grouped-column refusal,
    // the --changes shape check) is keyed on format == "json" and has to mean
    // the same thing for both. Only the renderer differs, which is the whole
    // of what jsonl adds.
    const bool jsonLines = (a.format == "jsonl");
    o.format = (a.format == "csv" || a.format == "json" || jsonLines ||
                a.format == "tsv")
                   ? (jsonLines ? std::string("json") : a.format)
                   : "table";
    o.jsonLines = jsonLines;
    if (!a.columns.empty()) o.columns = ParseColumns(a.columns);
    o.quiet = a.quiet;
    o.traffic = a.traffic;
    o.dns = a.dns;
    o.geoIpPath = a.db;
    o.changes = a.changes;
    // F5.3. Carried on ListOptions rather than applied at the parse site,
    // because the SnapshotSource owns the resolver and is created further down;
    // ListOptions is what survives from here to the pass that builds the rows.
    o.signatures = a.signatures;
    return o;
}

// Validate an output-shape switch against the shapes a verb supports.
// A silent fallback would print a different format than asked for.
bool CheckFormat(const std::string& fmt,
                 const std::vector<std::string>& allowed,
                 const std::string& verb, std::string* err) {
    for (const std::string& ok : allowed) {
        if (fmt == ok) return true;
    }
    std::string list;
    for (size_t i = 0; i < allowed.size(); ++i) {
        if (i != 0) list += "|";
        list += allowed[i];
    }
    if (err != nullptr)
        *err = verb + ": unknown --format '" + fmt + "' (want " + list + ").";
    return false;
}

// The file format for `export --out FILE` when --format was NOT given: the
// extension names it (.csv/.tsv/.json), which is what the help's own first
// example (`export --out conns.csv --filter "tcp:"`) and the bracketed
// [--format] in the usage line already promise - without this, that example
// failed with "unknown --format 'table'". "" = not inferable, and the caller
// then DEMANDS an explicit choice: guessing a format would write different
// bytes than the file's name suggests.
std::string InferExportFormat(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return "";
    const std::string ext = WideToUtf8(ToLowerW(path.substr(dot + 1)));
    if (ext == "csv") return "csv";
    if (ext == "tsv") return "tsv";
    if (ext == "json") return "json";
    return "";
}

// D24. Per-verb switch allow-lists.
//
// The switch parser is global, so before this every verb accepted every
// switch and silently dropped the ones it did not use. Measured, all rc 0 and
// all wrong: `export --changes` exported the whole table instead of deltas;
// `export --quiet` still printed "exported N rows" (breaking the quiet
// contract `list --quiet` honours - 0 bytes); `list --pid 1 --out x --secs 3`
// all ran as if untyped; `details --format json --limit 1` printed plain text;
// `ps --group --traffic` did nothing; `bookmark list --limit 1` printed every
// bookmark; `preset apply --limit 2` dumped the whole table.
//
// Silence is the worst possible answer here, because a script author who
// misspells a switch gets a successful run and the wrong result, and has no
// way to tell from the exit code. An unknown-for-this-verb switch is now exit
// 2 and names both the switch and the verb.
//
// The rule is deliberately about MEANING, not about the parser: a switch is
// listed for a verb when the verb actually acts on it. `export` keeps --group
// (it exports a grouped view), --quiet (it now honours it), --changes (it
// refuses it, see below) and the enrichments; it does not keep --limit,
// because CmdExport deliberately ignores it so a file never lies about its
// row count - that is a documented non-switch, not a silent one.
const wchar_t* const kSwitchNames[] = {
    L"--filter", L"--sort", L"--asc", L"--desc", L"--group", L"--format",
    L"--columns", L"--limit", L"--quiet", L"--watch", L"--interval", L"--count", L"--out",
    L"--select", L"--pid", L"--address", L"--port", L"--tag", L"--note",
    L"--name", L"--db", L"--secs", L"--traffic", L"--dns", L"--changes",
    L"--signatures",
    L"--text", L"--dir",
    L"--event", L"--yes", L"-y", L"--dry-run", L"--force", L"--close",
};
constexpr size_t kSwitchNameCount =
    sizeof(kSwitchNames) / sizeof(kSwitchNames[0]);

// 9.3.3/D33: bounded Levenshtein for "did you mean". Fixed rows, a 64-char
// cap (every switch and command name is far shorter); beyond the cap the
// answer is "far" rather than an allocation. ASCII case-fold only: every
// candidate is ASCII, and the parser's own matching is case-insensitive.
size_t EditDistance(const std::wstring& a, const std::wstring& b) {
    if (a.size() > 64 || b.size() > 64) return 99;
    size_t prev[65], cur[65];
    const size_t n = a.size(), m = b.size();
    for (size_t j = 0; j <= m; ++j) prev[j] = j;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            wchar_t ca = a[i - 1];
            wchar_t cb = b[j - 1];
            if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
            if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
            const size_t cost = (ca == cb) ? 0u : 1u;
            size_t best = prev[j] + 1;                      // deletion
            if (cur[j - 1] + 1 < best) best = cur[j - 1] + 1;   // insertion
            if (prev[j - 1] + cost < best) best = prev[j - 1] + cost;  // sub
            cur[j] = best;
        }
        for (size_t j = 0; j <= m; ++j) prev[j] = cur[j];
    }
    return prev[m];
}

// The nearest known switch within edit distance 2, or "" when nothing is
// close. Distance 2 covers the usual slips (doubled letter, dropped char,
// a transposition as two edits) without ever proposing something the user
// did not mean - a wrong suggestion is worse than none.
std::string SuggestSwitchName(const std::wstring& typed) {
    std::wstring bestName;
    size_t bestDist = 3;
    for (size_t i = 0; i < kSwitchNameCount; ++i) {
        const std::wstring cand(kSwitchNames[i]);
        const size_t d = EditDistance(typed, cand);
        if (d < bestDist) { bestDist = d; bestName = cand; }
    }
    if (bestName.empty()) return "";
    return WideToUtf8(bestName.c_str());
}

// Same for commands, over the names column of the help table (aliases
// included, so `cno` is told about `conn`).
std::string SuggestCommandName(const std::wstring& typed) {
    std::wstring bestName;
    size_t bestDist = 3;
    for (size_t i = 0; i < kCommandHelpCount; ++i) {
        const std::string names = kCommandHelps[i].names;
        size_t pos = 0;
        for (;;) {
            const size_t sp = names.find(' ', pos);
            const std::string one = (sp == std::string::npos)
                                        ? names.substr(pos)
                                        : names.substr(pos, sp - pos);
            if (!one.empty()) {
                const std::wstring cand(one.begin(), one.end());
                const size_t d = EditDistance(typed, cand);
                if (d < bestDist) { bestDist = d; bestName = cand; }
            }
            if (sp == std::string::npos) break;
            pos = sp + 1;
        }
    }
    if (bestName.empty()) return "";
    return WideToUtf8(bestName.c_str());
}

// "group-by verb" sets of switch names, space separated. A verb not listed
// here allows nothing and is reported by name, which keeps a new verb from
// silently accepting everything.
// "verb -> space separated switch names" table.
//
// nullptr for `allowed` means "this row is an ALIAS, resolve it elsewhere".
// An EMPTY string means "a real verb that takes no switches at all" -
// `version` and `help` are the two. Conflating the two would make every
// switch-less verb report itself as an unknown command, which is exactly what
// happened on the first pass of this table.
struct VerbSwitches {
    const wchar_t* verb;
    const wchar_t* allowed;
};

const VerbSwitches kVerbSwitches[] = {
    {L"list",
     // --out/--force: 9.3.4, the table written to a file instead of stdout;
     // --force only exists because --out refuses an existing path.
     L"--filter --sort --asc --desc --group --format --columns --limit "
     L"--quiet --watch --interval --count --traffic --dns --db --changes --event "
     L"--signatures --out --force"},
    {L"conn", nullptr},   // alias: same as list
    {L"ps",
     // --columns is deliberately absent. ps renders one fixed set of six
     // aggregate columns and PsOptions has nowhere to put a selection, so
     // listing it here would accept the switch and drop it - exactly the
     // silent swallow the table above exists to prevent, and what
     // `ps --columns nosuchcolumn` used to do: exit 0, value unchecked,
     // output unchanged, while `list --columns nosuchcolumn` exited 2.
     // list, export and preset honour --columns; ps does not, so it
     // refuses it like any other switch it does not act on.
     L"--filter --format --limit --quiet --watch --interval --count --sort "
     L"--asc --desc"},
    {L"top", nullptr},    // alias: same as ps
    {L"stat", L"--format --watch --interval --count"},
    {L"sys", nullptr},    // alias: same as stat
    {L"details", L"--select --traffic --dns --db --signatures"},
    {L"kill",
     // 9.3.6: --close (ask only) and --force (terminate at once); the
     // default stays the documented hybrid.
     L"--pid --select --yes --dry-run --close --force"},
    {L"close", L"--select --yes --dry-run"},
    {L"block", L"--select --yes --dry-run"},
    {L"unblock", L"--address --port --yes --dry-run"},
    {L"blocks", L""},     // takes no switches
    {L"capture", L"--select --secs --yes --dry-run --text --dir --out --force"},
    {L"follow", nullptr},  // alias: same as capture
    {L"bookmark", L"--address --port --tag --note --format"},
    {L"preset",
     L"--name --filter --sort --asc --desc --force --format --columns "
     L"--limit --traffic --dns --db --group"},
    {L"export",
     // --limit is LISTED even though it is refused further down. That is
     // deliberate: the generic D24 message ("this switch is not one of this
     // command's") is the wrong thing to say about it, because export has a
     // considered opinion - an export always writes the whole view, so a file
     // cannot hold a subset and misreport its own row count. Listing it lets
     // the export-specific explanation win.
     L"--out --format --filter --sort --asc --desc --columns --group "
     L"--traffic --dns --db --quiet --limit --force"},
    {L"geoip", L"--db"},
    {L"version", L""},    // takes no switches
    {L"help", L""},       // takes no switches
    // Hidden test verb for R1: raises a real access violation so that
    // wintcp\tests\cli.bat can assert the crash filter writes a minidump.
    // Deliberately absent from help, from the documentation and from
    // wintcp\tests\examples.txt - it is a fuse box, not a feature, and
    // advertising it would invite exactly the accidents it exists to
    // diagnose. Takes no switches; any switch is exit 2 like every verb.
    {L"crashtest", L""},
};

// Alias resolution for the allow-list: several verbs are spellings of one
// command, and the table above says so with a nullptr.
const wchar_t* CanonicalVerbFor(const std::wstring& verb) {
    if (verb == L"conn") return L"list";
    if (verb == L"top") return L"ps";
    if (verb == L"sys") return L"stat";
    if (verb == L"follow") return L"capture";
    for (const VerbSwitches& v : kVerbSwitches) {
        if (v.allowed == nullptr) continue;   // an alias row, not a verb
        if (verb == v.verb) return v.verb;
    }
    return nullptr;
}

// True when 'name' appears in the space-separated 'allowed' list. An empty or
// null list allows nothing, which is the correct answer for a verb that takes
// no switches.
//
// The list is compared as whole WORDS, not as a substring. A substring test
// needs a trailing space appended to the needle, which then fails to match the
// LAST entry in the list - so `--quiet` (last in export's list) was rejected
// and `--event` (last in list's) was rejected too, while everything before it
// worked. That is the worst possible shape for a validator: it passes in
// testing and fails on the ends.
bool SwitchAllowed(const wchar_t* allowed, const std::wstring& name) {
    if (allowed == nullptr || *allowed == L'\0') return false;
    std::wstring word;
    for (const wchar_t* p = allowed;; ++p) {
        if (*p == L' ' || *p == L'\0') {
            if (!word.empty() && ::_wcsicmp(word.c_str(), name.c_str()) == 0)
                return true;
            word.clear();
            if (*p == L'\0') break;
            continue;
        }
        word.push_back(*p);
    }
    return false;
}

// Reject every switch the verb does not act on. Returns false with a message
// naming the first offender, the verb, and where to look.
bool CheckVerbSwitches(const Args& a, const std::wstring& verb,
                       std::string* err) {
    const wchar_t* canonical = CanonicalVerbFor(verb);
    if (canonical == nullptr) {
        // An unknown COMMAND, not an unknown switch: point at the overview.
        // The switch-level errors below carry their own "try help <verb>",
        // and this is the one case where there is no verb to point at.
        // 9.3.3/D33: a near-miss spelling is corrected, not just refused.
        if (err != nullptr) {
            const std::string sugg = SuggestCommandName(verb);
            *err = WideToUtf8(verb) + ": unknown command.\r\n" +
                   (sugg.empty() ? std::string()
                                 : "Did you mean '" + sugg + "'?\r\n") +
                   "Try 'wintcp.exe help'.";
        }
        return false;
    }
    const wchar_t* allowed = nullptr;
    for (const VerbSwitches& v : kVerbSwitches) {
        if (std::wstring(v.verb) == canonical) { allowed = v.allowed; break; }
    }
    for (size_t i = 0; i < kSwitchNameCount; ++i) {
        const std::wstring name(kSwitchNames[i]);
        if (a.given.find(name) == a.given.end()) continue;  // not typed here
        if (SwitchAllowed(allowed, name)) continue;
        if (err != nullptr) {
            // 9.3.3: name the allowed set so a typo is debuggable, not just
            // refused. The full list is short and the verb owns it above.
            std::string hint;
            if (allowed != nullptr && *allowed != L'\0')
                hint = WideToUtf8(allowed);
            *err = WideToUtf8(verb) + ": " + WideToUtf8(name) +
                   " is not a switch of this command; it would be ignored.\r\n"
                   "Try 'wintcp.exe help " + WideToUtf8(canonical) + "'." +
                   (hint.empty() ? std::string()
                                 : "\r\n" + WideToUtf8(canonical) +
                                   " honours: " + hint + ".");
        }
        return false;
    }
    return true;
}

volatile LONG g_cmdStop = 0;

BOOL WINAPI CmdCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_cmdStop = 1;
        return TRUE;
    }
    return FALSE;
}

// 9.2.3: --watch without --count runs until Ctrl+C - right for a terminal,
// a trap for a redirected script. Warn once, on stderr, only when stdout is
// NOT a console: an interactive user watching output scroll needs no
// warning; a pipeline filling a disk does.
void WarnUnboundedWatch(const Args& a) {
    if (a.count != 0) return;
    DWORD mode = 0;
    if (::GetConsoleMode(::GetStdHandle(STD_OUTPUT_HANDLE), &mode)) return;
    WriteErr("warning: --watch without --count runs until Ctrl+C; "
             "add --count N to bound it for scripts.\r\n");
}

int RunPollLoop(const Args& a, std::function<CommandResult()> once) {    ::SetConsoleCtrlHandler(CmdCtrlHandler, TRUE);
    g_cmdStop = 0;
    WarnUnboundedWatch(a);
    unsigned n = 0;
    int rc = 0;
    for (;;) {
        CommandResult r = once();
        Emit(r);
        rc = r.exitCode;
        ++n;
        if (a.count != 0 && n >= a.count) break;
        // sleep in slices so Ctrl+C is noticed promptly
        const ULONGLONG due = ::GetTickCount64() +
                              static_cast<ULONGLONG>(a.watchSec) * 1000ull;
        bool intr = false;
        for (;;) {
            if (g_cmdStop != 0) {
                intr = true;
                break;
            }
            ::Sleep(kWatchPollMs);
            if (::GetTickCount64() >= due) break;
        }
        if (intr) {
            rc = 0;
            break;
        }
    }
    g_cmdStop = 0;
    ::SetConsoleCtrlHandler(CmdCtrlHandler, FALSE);
    return rc;
}

// Delta watch: persistent store across polls, printing only what changed.
// The first snapshot establishes the baseline (APPEARs discarded); every
// later poll prints APPEAR/DISAPPEAR/STATE lines. Ctrl+C ends it with 0.
int RunChangesLoop(const Args& a, const ListOptions& opt) {
    ::SetConsoleCtrlHandler(CmdCtrlHandler, TRUE);
    g_cmdStop = 0;
    std::vector<FilterClause> prog;
    if (!opt.filter.empty()) ParseFilter(opt.filter, prog);
    SnapshotSource source;
    ConnectionStore store;
    unsigned n = 0;
    int rc = 0;
    bool first = true;
    WarnUnboundedWatch(a);   // 9.2.3: same contract as RunPollLoop
    for (;;) {
        std::wstring err;
        if (!BuildStoreSnapshot(source, store, /*procStats=*/false,
                                /*resolveDns=*/false, /*geoIp=*/false,
                                nullptr, &err)) {
            WriteErr("Enumeration failed: " + WideToUtf8(err) + "\r\n");
            rc = 1;
            break;
        }
        const std::vector<RowChange> ev = store.TakeChangeEvents();
        if (first) {
            first = false;
            char b[128] = {0};
            ::sprintf_s(b, "baseline: %zu rows (further changes below)\r\n",
                        store.Rows().size());
            WriteErr(b);
        } else {
            WriteOut(FormatChangeEvents(ev, opt.format, FormatCurrentTime(),
                                        &prog, a.eventMask));
        }
        ++n;
        if (a.count != 0 && n >= a.count) break;
        const ULONGLONG due = ::GetTickCount64() +
                              static_cast<ULONGLONG>(a.watchSec) * 1000ull;
        bool intr = false;
        for (;;) {
            if (g_cmdStop != 0) {
                intr = true;
                break;
            }
            ::Sleep(kWatchPollMs);
            if (::GetTickCount64() >= due) break;
        }
        if (intr) {
            rc = 0;
            break;
        }
    }
    g_cmdStop = 0;
    ::SetConsoleCtrlHandler(CmdCtrlHandler, FALSE);
    return rc;
}

}  // namespace

// The streaming write for D15: runs DURING a command, straight to stdout
// (or its redirect), so a table's header lands before the slow enrichment
// joins finish. Deliberately the same writer Emit uses for the rows - one
// handle, one byte stream, order preserved.
void StreamOut(const std::string& s) { WriteOut(s); }

int RunCliCommand(int argc, wchar_t** argv) {
    const std::wstring cmd = ToLowerW(argv[1]);

    // `help [command]` - the help verb itself takes no switches.
    if (cmd == L"help") {
        std::wstring topic;
        for (int i = 2; i < argc; ++i) {
            const std::wstring t = argv[i] != nullptr ? argv[i] : L"";
            if (t.empty() || t[0] == L'-') continue;
            topic = t;
            break;
        }
        if (topic.empty()) {
            WriteOut(kHelp);
            return 0;
        }
        if (PrintCommandHelp(topic)) return 0;
        {
            // 9.3.3/D33: a near-miss topic gets its correction.
            const std::string sugg = SuggestCommandName(topic);
            WriteErr("unknown command: '" + WideToUtf8(topic) + "'." +
                     (sugg.empty() ? std::string()
                                   : " Did you mean '" + sugg + "'?") +
                     " Try 'wintcp.exe help'.\r\n");
        }
        return 2;
    }

    // `<cmd> --help|-h|help`: recognised before parsing, so it works even
    // when required switches are missing. Values of valued switches are
    // skipped, so `--note help` keeps its value.
    {
        bool skipNext = false;
        for (int i = 2; i < argc; ++i) {
            const std::wstring t = argv[i] != nullptr ? argv[i] : L"";
            if (skipNext) {
                skipNext = false;
                continue;
            }
            if (TakesValue(t)) {
                skipNext = true;
                continue;
            }
            if (IsHelpToken(t)) {
                if (PrintCommandHelp(cmd)) return 0;
                break;   // unknown command: fall through to the error below
            }
        }
    }

    Args a;
    const std::string perr = ParseSwitches(argc, argv, 2, &a);
    if (!perr.empty()) {
        WriteErr(perr + "\r\nTry 'wintcp.exe help " + WideToUtf8(cmd) +
                 "'.\r\n");
        return 2;
    }
    // D24: a switch this verb does not act on is an argument error, not
    // something to swallow. Placed right after parsing so every verb below
    // inherits it, and BEFORE any work is done - nothing is enumerated, no
    // file is written and no registry key is touched before the command line
    // has been found to make sense.
    {
        std::string verr2;
        if (!CheckVerbSwitches(a, cmd, &verr2)) {
            // CheckVerbSwitches' message already ends its own line(s); adding
            // another "\r\n" here would print a blank line after the error.
            WriteErr(verr2);
            return 2;
        }
    }
    if (cmd == L"version") {
        // D23. Two real bugs in one place, both about the summary claiming
        // things this process did not check.
        //
        // 1. `elevated` was IsElevatedInstance(), which tests for the RELAUNCH
        //    MARKER ARGUMENT, not the security token. Measured on an elevated
        //    shell: `selftest` reported `elev=1` and `close --yes` succeeded,
        //    while `version` printed "Administrator no - per-PID traffic
        //    counters and per-connection close are unavailable". IsElevated()
        //    is the real answer, and it is what every other capability check in
        //    the program already uses.
        // 2. "Columns shown 0 of 23" and "Connections 0" were hard-coded
        //    zeros: the CLI has no persisted view mask and took no snapshot, so
        //    both were meaningless. Reporting a count nobody measured is worse
        //    than not reporting one, so they are now filled with the real
        //    values where they exist and omitted where they do not.
        BuildSummary s;
        s.elevated = IsElevated();
        s.presetsAvailable = true;
        s.totalColumnCount = COL_COUNT;
        s.visibleColumnCount = 0;      // no persisted GUI mask in a CLI run
        s.columnCountKnown = false;   // so the line is omitted, not faked
        // One real snapshot, so "Connections" is a measured number rather than
        // the zero it used to print. Cheap: the same enumeration every verb
        // does, and this verb exists to describe the machine's state.
        {
            SnapshotSource src;
            ConnectionStore store;
            std::wstring err;
            if (BuildStoreSnapshot(src, store, /*procStats=*/false,
                                   /*resolveDns=*/false, /*geoIp=*/false,
                                   nullptr, &err)) {
                s.rowCount = store.Rows().size();
                s.rowCountKnown = true;
            }
        }
        // Traffic source: probe the fallback's support rather than asserting
        // "none". SIO_TCP_INFO needs Windows 10 1709+; on a machine that has it
        // the byte columns do work unelevated, and saying otherwise is exactly
        // the lie this fix exists to remove.
        {
            SocketTrafficSampler probe;
            s.trafficFallback = !s.elevated && probe.Supported();
        }
        WriteOut(WideToUtf8(AboutText(s)));
        return 0;
    }
    if (cmd == L"crashtest") {
        // R1 fuse box. Not reachable from help, the README or any script
        // but golden's; see CrashDump.h. This one verb STAYS in the product,
        // unlike selftest/bench: it exists so a system test can crash the
        // shipped binary and prove the crash filter writes a minidump. The
        // handler is installed by wmain, so a test exe cannot exercise it.
        CrashForTest();
    }
    if (cmd == L"list" || cmd == L"conn") {
        std::string verr;
        if (!CheckFormat(a.format, {"table", "csv", "tsv", "json", "jsonl"},
                         "list", &verr)) {
            WriteErr(verr + "\r\n");
            return 2;
        }
        if (ColumnIdFor(a.sort) < 0) {
            WriteErr("list: unknown --sort column '" +
                     WideToUtf8(a.sort.c_str()) +
                     "'. Try 'wintcp.exe help list'.\r\n");
            return 2;
        }
        if (!a.columns.empty() && ParseColumns(a.columns).empty()) {
            WriteErr("list: --columns names no known column. Try "
                     "'wintcp.exe help list'.\r\n");
            return 2;
        }
        ListOptions opt = ToListOptions(a);
        // 9.3.4: --out writes ONE rendered snapshot to a file. Each refusal
        // names its own contradiction instead of picking a side for the
        // user: a watch would overwrite the file every tick, a change
        // stream is not a table, and a quiet run renders nothing.
        if (!a.out.empty()) {
            if (a.watch) {
                WriteErr("list: --out writes one snapshot; --watch polls. "
                         "Drop one.\r\n");
                return 2;
            }
            if (opt.changes) {
                WriteErr("list: --out holds one table; --changes streams "
                         "deltas. Drop one.\r\n");
                return 2;
            }
            if (opt.quiet) {
                WriteErr("list: --quiet renders nothing, so --out would "
                         "write an empty file. Drop one.\r\n");
                return 2;
            }
        }
        // 9.2.2: --quiet turns the exit code into the ONLY answer, so a
        // quiet run whose filter asks an enrichment question its switches
        // left unanswerable would answer "no match" (1) as a fact. Refuse
        // instead (exit 2), naming the switch. The advisory text stays
        // suppressed under quiet - an argument error is not advice.
        if (opt.quiet) {
            const std::string missing = FilterEnrichmentAdvice(opt);
            if (!missing.empty()) {
                WriteErr(missing +
                         "list: refusing a --quiet run that can only answer "
                         "\"no match\" (exit 2). Add the switch above, or "
                         "drop --quiet to see this note as advice.\r\n");
                return 2;
            }
        }
        // D15: the CLI's list verb prints its fixed column header as soon
        // as the snapshot exists, not after the enrichment joins; json has
        // no header line, --quiet prints nothing (zero bytes, always).
        // 9.3.4: with --out nothing streams to stdout, so the header stays
        // in r.out and lands in the file.
        opt.streamHeader = !opt.quiet && opt.format != "json" && a.out.empty();
        if (opt.changes) {
            if (!a.watch && a.count == 0) {
                WriteErr("list: --changes needs --watch (or --count).\r\n");
                return 2;
            }
            if (opt.quiet) {
                WriteErr("list: --changes prints deltas; --quiet contradicts "
                         "it. Drop one.\r\n");
                return 2;
            }
            // --format jsonl arrives here already mapped to "json" (see
            // ToListOptions), so it needs no second arm - and --changes
            // already writes one object per line, which is exactly what
            // jsonl asks for, so the two are the same output rather than two
            // shapes to reconcile. The message names jsonl because that is
            // the spelling a user who just typed it will recognise.
            if (opt.format != "table" && opt.format != "json") {
                WriteErr("list: --changes supports table|json|jsonl only.\r\n");
                return 2;
            }
            // Deltas need no enrichment: events carry endpoint identity.
            if (opt.dns || opt.traffic || !opt.geoIpPath.empty()) {
                WriteErr("list: --changes ignores --dns/--traffic/--db.\r\n");
                return 2;
            }
            return RunChangesLoop(a, opt);
        }
        if (a.watch) {
            if (a.quiet) {
                // --quiet answers from the first snapshot; polling it would
                // never print anything and never stop on its own.
                SnapshotSource src;
                const CommandResult r = CmdList(src, opt);
                return r.exitCode;
            }
            // One store and one source for the whole watch. Re-creating them
            // per tick (the previous shape) restarted firstSeenTick every
            // poll, so the Duration column read 0s forever and a connection
            // never looked older no matter how long the watch ran.
            SnapshotSource src;
            ConnectionStore store;
            return RunPollLoop(a, [&]() {
                return CmdListInto(src, store, opt);
            });
        }
        SnapshotSource src;
        const CommandResult r = CmdList(src, opt);
        if (!a.out.empty()) {
            // Nothing worth writing: report the failure as it came, never
            // an empty file that looks like an answer.
            if (r.exitCode != 0 && r.out.empty()) {
                Emit(r);
                return r.exitCode;
            }
            // 9.3.4: same file rules as export --out. Refuse an existing
            // path without --force (an --out that overwrites by default is
            // how a script loses yesterday's data); BOM for csv/tsv/table,
            // never for json/jsonl - both verbs share
            // WriteUtf8FileWithBom so the rules cannot drift.
            {
                std::wstring existsErr;
                if (RefuseExistingOutput(L"list", a.out, a.force,
                                          &existsErr)) {
                    WriteErr(WideToUtf8(existsErr));
                    return 2;
                }
            }
            const bool isJson = (opt.format == "json");
            const std::wstring werr =
                WriteUtf8FileWithBom(a.out, r.out, !isJson);
            if (!werr.empty()) {
                WriteErr("list: cannot write --out: " + WideToUtf8(werr) +
                         "\r\n");
                return 1;
            }
            // Honest confirmation: the path is a fact, the row count is
            // not (CmdList owns its store) - so no invented number.
            WriteOut("wrote " + WideToUtf8(a.out) + "\r\n");
            if (!r.err.empty()) WriteErr(r.err);
            return r.exitCode;
        }
        Emit(r);
        return r.exitCode;
    }
    if (cmd == L"ps" || cmd == L"top") {
        std::string verr;
        if (!CheckFormat(a.format, {"table", "csv", "json"},
                         WideToUtf8(cmd.c_str()), &verr)) {
            WriteErr(verr + "\r\n");
            return 2;
        }
        PsOptions opt;
        opt.filter = a.filter;
        opt.format = a.format;
        opt.limit = a.limit;
        opt.quiet = a.quiet;
        // ps/top sort namespace differs from list's column namespace:
        // cpu|mem|disk|conns|pid|process. Default is cpu (hottest first).
        opt.sortBy = "cpu";
        if (a.hasSort) {
            const std::string key = WideToUtf8(ToLowerW(a.sort));
            if (key != "cpu" && key != "mem" && key != "disk" &&
                key != "conns" && key != "pid" && key != "process") {
                WriteErr(WideToUtf8(cmd.c_str()) +
                         ": unknown --sort '" + key +
                         "' (want cpu|mem|disk|conns|pid|process).\r\n");
                return 2;
            }
            opt.sortBy = key;
        }
        // A bare `top` prints once and exits like every other verb: only an
        // explicit --watch/--count polls.
        if (a.watch || a.count != 0) {
            if (a.quiet) {
                SnapshotSource src;
                const CommandResult r = CmdPs(src, opt);
                return r.exitCode;
            }
            Args loop = a;
            loop.watch = true;
            return RunPollLoop(loop, [&]() {
                SnapshotSource src;
                return CmdPs(src, opt);
            });
        }
        SnapshotSource src;
        const CommandResult r = CmdPs(src, opt);
        Emit(r);
        return r.exitCode;
    }
    if (cmd == L"details") {
        SnapshotSource src;
        EnrichOptions eo;
        eo.procStats = true;
        eo.traffic = a.traffic;
        eo.dns = a.dns;
        eo.geoIpPath = a.db;
        eo.signatures = a.signatures;
        const CommandResult r = CmdDetails(src, a.select, eo);
        Emit(r);
        return r.exitCode;
    }
    if (cmd == L"kill") {
        // 9.3.6: the two modes are exclusions, not preferences. Accepting
        // both and picking one silently would be the swallowed-switch bug.
        if (a.closeOnly && a.force) {
            WriteErr("kill: --close (never force) and --force (never ask) "
                     "exclude each other. Pick one.\r\n");
            return 2;
        }
        MutateOptions mo;
        mo.yes = a.yes;
        mo.dryRun = a.dryRun;
        mo.closeOnly = a.closeOnly;
        mo.forceNow = a.force;
        SnapshotSource src;
        if (!a.select.empty()) {
            // Find by connection, kill its process: the flow taskkill
            // cannot do (`kill --select "local:port:8080" --yes`).
            if (a.hasPid) {
                WriteErr("kill: --pid and --select exclude each other.\r\n");
                return 2;
            }
            const CommandResult r = CmdKillSelect(src, a.select, mo);
            Emit(r);
            return r.exitCode;
        }
        if (!a.hasPid) {
            WriteErr("kill: --pid N or --select <filter> is required.\r\n");
            return 2;
        }
        const CommandResult r = CmdKill(src, a.pid, mo);
        Emit(r);
        return r.exitCode;
    }
    if (cmd == L"close") {
        MutateOptions mo;
        mo.yes = a.yes;
        mo.dryRun = a.dryRun;
        SnapshotSource src;
        const CommandResult r = CmdClose(src, a.select, mo);
        Emit(r);
        return r.exitCode;
    }
    if (cmd == L"block") {
        MutateOptions mo;
        mo.yes = a.yes;
        mo.dryRun = a.dryRun;
        SnapshotSource src;
        const CommandResult r = CmdBlock(src, a.select, mo);
        Emit(r);
        return r.exitCode;
    }
    if (cmd == L"unblock") {
        MutateOptions mo;
        mo.yes = a.yes;
        mo.dryRun = a.dryRun;
        const CommandResult r = CmdUnblock(a.address, a.port, mo);
        Emit(r);
        return r.exitCode;
    }
    if (cmd == L"blocks") {
        const CommandResult r = CmdBlocks();
        Emit(r);
        return r.exitCode;
    }
    if (cmd == L"bookmark") {
        const std::wstring sub = ToLowerW(a.sub);
        if (sub.empty() || sub == L"list") {
            std::string verr;
            if (!CheckFormat(a.format, {"table", "csv", "json"}, "bookmark",
                             &verr)) {
                WriteErr(verr + "\r\n");
                return 2;
            }
            const CommandResult r = CmdBookmarkList(a.format);
            Emit(r);
            return r.exitCode;
        }
        if (sub == L"add") {
            const CommandResult r = CmdBookmarkAdd(
                a.address, a.port, a.hasTag ? a.tag : 0, a.note);
            Emit(r);
            return r.exitCode;
        }
        if (sub == L"remove" || sub == L"rm" || sub == L"delete") {
            const CommandResult r = CmdBookmarkRemove(a.address, a.port);
            Emit(r);
            return r.exitCode;
        }
        if (sub == L"note") {
            const CommandResult r =
                CmdBookmarkNote(a.address, a.port, a.note);
            Emit(r);
            return r.exitCode;
        }
        if (sub == L"colour" || sub == L"color") {
            if (!a.hasTag) {
                WriteErr("bookmark colour: --tag 0..4 is required.\r\n");
                return 2;
            }
            const CommandResult r =
                CmdBookmarkColour(a.address, a.port, a.tag);
            Emit(r);
            return r.exitCode;
        }
        WriteErr("unknown bookmark verb: " + WideToUtf8(a.sub) + "\r\n");
        return 2;
    }
    if (cmd == L"preset") {
        const std::wstring sub = ToLowerW(a.sub);
        if (sub.empty() || sub == L"list") {
            std::string verr;
            if (!CheckFormat(a.format, {"table", "json"}, "preset", &verr)) {
                WriteErr(verr + "\r\n");
                return 2;
            }
            const CommandResult r = CmdPresetList(a.format);
            Emit(r);
            return r.exitCode;
        }
        if (sub == L"save") {
            if (a.name.empty()) {
                WriteErr("preset save: --name N is required.\r\n");
                return 2;
            }
            if (ColumnIdFor(a.sort) < 0) {
                WriteErr("preset save: unknown --sort column '" +
                         WideToUtf8(a.sort.c_str()) + "'.\r\n");
                return 2;
            }
            ViewState v = CurrentPresetViewFor(
                a.filter, kDefaultVisibleCols, ColumnIdFor(a.sort),
                !a.desc, kPresetSourceNone);
            const CommandResult r = CmdPresetSave(a.name, v, a.force);
            Emit(r);
            return r.exitCode;
        }
        if (sub == L"show") {
            if (a.name.empty()) {
                WriteErr("preset show: --name N is required.\r\n");
                return 2;
            }
            const CommandResult r = CmdPresetShow(a.name);
            Emit(r);
            return r.exitCode;
        }
        if (sub == L"delete" || sub == L"rm") {
            if (a.name.empty()) {
                WriteErr("preset delete: --name N is required.\r\n");
                return 2;
            }
            const CommandResult r = CmdPresetDelete(a.name);
            Emit(r);
            return r.exitCode;
        }
        if (sub == L"apply") {
            if (a.name.empty()) {
                WriteErr("preset apply: --name N is required.\r\n");
                return 2;
            }
            // D24: the user's own output switches are layered over the saved
            // view instead of being silently dropped. `--limit 2` and
            // `--columns pid,remote` now mean what they say (before: the whole
            // table in the preset's default columns, rc 0).
            ListOptions over = ToListOptions(a);
            // Only mark a switch as "given" when it was actually typed, so an
            // absent --sort does not clobber the preset's sort with the
            // default. COL_PID is the default, so a preset that genuinely
            // sorts by pid is unaffected.
            if (!a.hasSort) over.sortColumn = -1;
            SnapshotSource src;
            const CommandResult r = CmdPresetApply(src, a.name, nullptr,
                                                    &over);
            Emit(r);
            return r.exitCode;
        }
        WriteErr("unknown preset verb: " + WideToUtf8(a.sub) + "\r\n");
        return 2;
    }
    if (cmd == L"export") {
        if (a.out.empty()) {
            WriteErr("export: --out FILE is required.\r\n");
            return 2;
        }
        // D24. --limit is refused, NOT silently dropped. CmdExport deliberately
        // exports the whole view, because a file that quietly holds 5 of 300
        // rows while the tool reports success is a lie about the machine.
        // Accepting the switch and ignoring it made that lie invisible; a
        // named refusal at least tells the writer to reach for `--filter`.
        if (a.limit != 0) {
            WriteErr("export: --limit is not accepted: an export always "
                     "writes the whole view, because a file that silently "
                     "held a subset would misreport its own row count.\r\n"
                     "Narrow the view with --filter, or pipe "
                     "`list --limit` output instead.\r\n");
            return 2;
        }
        if (!a.formatSet) {
            // No --format: the file's own extension names the format, so
            // `export --out conns.csv` does what its name says. A name with
            // no known extension is rejected instead of guessed at.
            a.format = InferExportFormat(a.out);
            if (a.format.empty()) {
                WriteErr("export: cannot infer a file format from \"" +
                         WideToUtf8(a.out) + "\" (no --format given).\r\n"
                         "Use --format csv|tsv|json, or name the file "
                         "*.csv, *.tsv or *.json.\r\n");
                return 2;
            }
        }
        std::string verr;
        if (!CheckFormat(a.format, {"csv", "tsv", "json"}, "export", &verr)) {
            WriteErr(verr + "\r\n"
                     "export --format names the file format: csv|tsv|json.\r\n");
            return 2;
        }
        if (ColumnIdFor(a.sort) < 0) {
            WriteErr("export: unknown --sort column '" +
                     WideToUtf8(a.sort.c_str()) + "'.\r\n");
            return 2;
        }
        if (!a.columns.empty() && ParseColumns(a.columns).empty()) {
            WriteErr("export: --columns names no known column.\r\n");
            return 2;
        }
        ListOptions opt = ToListOptions(a);
        // export --format csv|tsv|json names the file format here.
        SnapshotSource src;
        const CommandResult r = CmdExport(src, opt, a.out, a.force);
        Emit(r);
        return r.exitCode;
    }
    if (cmd == L"geoip") {
        const std::wstring sub = ToLowerW(a.sub);
        SnapshotSource src;
        if (sub == L"lookup") {
            std::wstring ip = a.ip;
            if (ip.empty()) {
                WriteErr("geoip lookup: <ip> is required.\r\n");
                return 2;
            }
            const CommandResult r = CmdGeoIpLookup(src, a.db, ip);
            Emit(r);
            return r.exitCode;
        }
        if (sub == L"info") {
            const CommandResult r = CmdGeoIpInfo(src, a.db);
            Emit(r);
            return r.exitCode;
        }
        WriteErr("usage: geoip lookup --db FILE <ip> | geoip info --db FILE\r\n");
        return 2;
    }
    if (cmd == L"capture" || cmd == L"follow") {
        MutateOptions mo;
        mo.yes = a.yes;
        mo.dryRun = a.dryRun;
        SnapshotSource src;
        CaptureOptions co;
        co.text = a.captureText;
        co.outPath = a.out;
        co.dir = a.captureDir;
        co.forceOverwrite = a.force;
        const CommandResult r = CmdCapture(src, a.select, mo, a.secs, co);
        Emit(r);
        return r.exitCode;
    }
    if (cmd == L"stat" || cmd == L"sys") {
        std::string verr;
        if (!CheckFormat(a.format, {"table", "json"}, "stat", &verr)) {
            WriteErr(verr + "\r\n");
            return 2;
        }
        const std::string fmt = a.format;
        if (a.watch || a.count != 0) {
            Args loop = a;
            if (!loop.watch && loop.count != 0) loop.watch = true;
            return RunPollLoop(loop, [&]() {
                SystemStatsSampler sampler;
                // sampler must persist across polls for rates; keep one
                // per loop via static thread-local.
                thread_local SystemStatsSampler keep;
                (void)sampler;
                return CmdStat(keep, fmt);
            });
        }
        SystemStatsSampler sampler;
        const CommandResult r = CmdStat(sampler, fmt);
        Emit(r);
        return r.exitCode;
    }
    {
        // 9.3.3/D33: near-miss command names are corrected here too - this
        // is the path `nonsense` takes (no --help token, parse succeeds).
        const std::string sugg = SuggestCommandName(argv[1]);
        WriteErr("unknown command: '" + WideToUtf8(argv[1]) + "'." +
                 (sugg.empty() ? std::string()
                               : " Did you mean '" + sugg + "'?") +
                 "\r\nTry 'wintcp.exe help'.\r\n");
    }
    return 2;
}

}  // namespace wintcp
