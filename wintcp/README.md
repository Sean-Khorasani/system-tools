# WinTCP — TCP/UDP Connections Viewer (Win32, C++)

A native Windows desktop application built with the **plain Win32 API** — no
MFC, ATL, Qt, or any third-party library — that lists **all TCP and UDP
connections (IPv4 + IPv6) together with their owning processes**, à la
Sysinternals TCPView / NirSoft CurrPorts, plus a `tcpvcon`-style CLI.

```text
Process     CPU %  Traffic (rx/tx)   Proto  Local         LPort  Remote       RPort  State         PID
chrome.exe  3.2 %  18.0 MB / 1.2 MB  TCPv4  192.168.1.10  51752  203.0.113.9    443  ESTABLISHED    42
System          —  —                 TCPv4  0.0.0.0         443  *:*              —  LISTENING       4
mdns.exe    0.1 %  0 B / 4.0 KB      UDPv4  0.0.0.0        5353  *:*              —  —            1234
```

12 connection columns plus 6 optional per-process live columns: the combined
**Traffic** (per-PID byte counters — the ETW kernel logger when elevated, an
automatic per-socket fallback for standard users, see
[Traffic counters](#traffic-counters-etw--non-admin-fallback)) plus the split
**Received / Sent / Net total** and **CPU %** (both shown by default, right
after **Process**), **Memory** (working set) and **Disk I/O** (bytes
read+written). All are sampled on the refresh worker every cycle — manual F5
and every auto-refresh tick alike — sortable, filterable and exported with
CSV/TSV when visible. CPU % is real from the **very first refresh** (the worker primes the
baseline with a short double sample); readings that cannot be taken (protected
processes without elevation) show `—` and always sort to the end of the list.

**Filtering the live columns is numeric, not textual.** `cpu:12` means *12 % or
more*; `mem:100`, `disk:1.5`, `rx:2.0`, `tx:512`, `net:2.5` are thresholds in
**MB** (a bare number is MB, so use `rx:2KB` for 2 KB), `K`/`M`/`G` suffixes
multiply, and `mem:100-500` is a range. A row whose reading could not be taken
matches no threshold at all — `mem:0` never selects a process that simply could
not be measured.

## Features

**Enumeration & model**

- TCP via `GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL)`, UDP via
  `GetExtendedUdpTable(UDP_TABLE_OWNER_PID)` — v4 + v6, `netstat`-style
  placeholders for UDP (`*`, `*:*`, state `—`).
- Process name/path/service resolution with a persistent cache
  (PID + creation-time validated, one Toolhelp snapshot per refresh);
  PID 0 shows `—`, PID 4 shows `System`.
- Stable row identity across refreshes: **new rows flash green, TCP state
  changes yellow, vanished rows linger one cycle as red ghosts**, then drop.
  The identity is **endpoint + PID**, and duplicates pair up in order rather
  than collapsing — the 4-tuple alone is not unique on a real machine, and it
  is not a corner case: every browser binds its own socket to mDNS port 5353,
  so a busy desktop has dozens of rows sharing one key (86 rows over 10
  distinct identities on the machine these studies were captured on). Without
  the ordering, each refresh reported N−1 false `DISAPPEAR`s and re-created all
  N as new, forever.
- Optional per-PID **byte counters** (ETW when elevated, automatic
  per-socket fallback for standard users) — see
  [Traffic counters](#traffic-counters-etw--non-admin-fallback) below.
- IPv6 link-local **scope IDs** (`fe80::…%12`).
- All heavy work (enumerate + resolve + reverse DNS) runs on **worker
  threads**; results are posted to the UI thread as registered payloads.
  The list itself is a **virtual ListView** (`LVS_OWNERDATA`) that stays
  responsive with tens of thousands of rows, preserving selection, focus
  and scroll across refreshes.

**View & filtering**

- 18 columns (Proto, Local, LPort, Remote, RPort, State, PID, Process,
  Service, Host, Path, Traffic, Received, Sent, Net total, CPU %, Memory,
  Disk I/O); the CLI additionally exposes five more — `duration`,
  `bandwidth`, `tls`, `country`, `pinned` — for 23 selectable names.
  Per-column show/hide, resize and sort with the persisted
  layout restored on next start. `View → Columns` shows a **check mark on
  every visible column** — the ticks follow the column state everywhere:
  at startup, after a header/context-menu toggle, and whenever the menu
  re-opens (`WM_INITMENUPOPUP` re-sync). Titles are shared by the GUI,
  CSV and CLI output, and unknown stat readings (`—`) always sort last in
  both directions.
- **Default layout leads with identity**: Process, CPU %, Traffic, Proto,
  Local, LPort, Remote, RPort, State, PID, Service, Hostname, Path. Traffic
  and CPU % are visible out of the box; Memory, Disk I/O and the split
  Received / Sent / Net-total columns stay opt-in from `View → Columns`.
  Persisted masks are migrated (`ColVersion`) so an existing profile gains
  the newly default-visible columns exactly once.
- Protocol combo (All/TCPv4/TCPv6/UDP…) and TCP-state combo, or type in the
  **filter box** (250 ms debounced) using field expressions:

  | Expression | Meaning |
  |---|---|
  | `chrome` | substring over every field |
  | `port:443`, `pid:42`, `state:listen`, `process:svchost`, `host:example`, `path:c:\bin`, `service:dnscache`, `proto:udp`, `lport:1000-60000` | field-restricted match (`pid:`/`port:` accept `a-b` ranges) |
  | `cpu:12`, `mem:117`, `disk:1.5`, `rx:2.0`, `tx:512`, `net:2.5` | live per-process stats — **numeric thresholds**, not text: `cpu:12` is 12 % or more, the rest are MB or more, `K`/`M`/`G` multiply, `mem:100-500` is a range |
  | `duration:1h`, `speed:1KB`, `tls:1.3`, `country:de` | per-connection age (seconds, with `s`/`m`/`h`/`d`), rate, TLS summary, GeoIP code |
  | `note:vendor`, `note:` | a bookmark's note; bare means "has a note" |
  | `local:10.`, `remote:203` | restrict the value to one side (address substring) |
  | `local:port:80`, `remote:port:443` | restrict the **port** to one side |
  | `local:port:1000-60000` | one side, numeric range |
  | `"two words"` | quoted, so the value keeps its space and is **one** term — `note:"vendor api"`, `path:"program files"` |
  | `tcp:`, `udp:`, `ipv4:`, `ipv6:` | protocol / address-family prefix (also usable alone: `ipv6:`) |
  | `exclude:chrome` | negate the next term |
  | `chrome port:443 exclude:remote:tcp:443` | space = AND |

  A threshold a reading could not satisfy matches **nothing** — `mem:0` never
  selects a process that simply could not be measured — so a stat filter can
  never smuggle in the rows whose value is unknown.

- Click a column header to sort (ascending/descending, natural TCP-state
  order); sort, column widths/visibility and the filter text persist.

**Actions**

- Context menu: **Details** (sectioned: process identity + command line +
  creation time + service, live CPU / memory / disk / network stats,
  per-PID connection list, selected connection — header line shows
  `process.exe (PID n)`, the window opens centered over the main window,
  scales with DPI and refreshes silently on every auto-refresh while open
  (a note appears once if the tracked process exits); Copy / Open
  file location buttons), Copy selected,
  Copy all, **End process** (with PID-reuse verification), **Close
  connection** (`SetTcpEntry(DELETE_TCB)`, IPv4), Export selection…,
  Refresh. Double-click opens Details.
- **Export** to CSV / TSV / JSON (UTF-8; BOM for CSV/TSV, none for JSON,
  RFC-4180 quoting) — whole table or just the selection; remembers the last
  folder; extension wins if present.
- **Change log** (`File → Change log`): append-only CSV of APPEAR /
  DISAPPEAR / STATE events, drained from the store's diff each refresh.
- **Performance graphs** (`View → Performance graphs...`): a modeless
  window with four live GDI line charts — system CPU %, memory used %,
  disk read/write B/s (PDH `PhysicalDisk(_Total)` via locale-independent
  English counter paths, per-panel `n/a` when counters are disabled) and
  network down/up B/s (64-bit interface octet deltas via `GetIfTable2`) —
  sampled every second with a 120-sample history stretched across the plot
  area, each panel independent (per-panel "collecting…"/`n/a` states),
  double-buffered, theme-aware, minimum window size enforced. Opens
  centered over the main window; X hides it and stops the 1 s sampling
  timer, the menu entry re-shows it (check mark re-syncs when the menu
  opens), and it is destroyed with the app.
- Select all / focus filter / clear filter menu commands; accelerator table.

**Modern Windows (tasks 10, 23, 30)**

- Per-Monitor V2 **DPI awareness** (manifest), 9 pt Segoe UI, widths and
  font re-derived on `WM_DPICHANGED`.
- **Follows the system theme**: there is no dark-mode toggle. The list,
  status bar and filter box are painted from the live system colors
  (`GetSysColor`) and repaint on `WM_SYSCOLORCHANGE` / `WM_THEMECHANGED`,
  so light, dark and high-contrast schemes all work without a restart.
- **High contrast**: `SPI_GETHIGHCONTRAST` wins over the theme check, and
  every highlight colour is derived from `COLOR_HIGHLIGHT` /
  `COLOR_WINDOW` rather than a hard-coded palette.
- **Tray icon**: minimize-to-tray hides the window, tray menu
  (Open / always-on-top / Exit), restore on double-click.
- Always-on-top toggle.
- All toggles and window placement persist in `HKCU\Software\WinTCP`
  (only ever that one key; delete it to reset — nothing else is written).

**Command-line mode** - see below.

**Self-test and benchmark** - *not in this tool.* They moved to
`wintcp-tests.exe`, a separate development binary that is not distributed. See
[Testing](#testing).

## Traffic counters (ETW + non-admin fallback)

The **Received / Sent / Net total** (and combined **Traffic**) columns show
cumulative per-PID byte totals. Two sources feed them; the app picks the
best one that can run:

**1. ETW kernel logger (elevated — full fidelity, TCP + UDP).**
`View → Per-PID traffic counters (ETW, admin)` starts the **NT Kernel
Logger** with `EVENT_TRACE_FLAG_NETWORK_TCPIP` and sums send/receive bytes
per PID from the classic `TcpIp`/`UdpIp` events (payload PID @0, size @4),
re-applied to every refresh. The session flushes at least once per second
(`FlushTimer = 1`) so a refresh right after some traffic already sees it,
and the buffer pool is widened (32–128 × 64 KB) to keep up with bursts —
like every other real-time ETW consumer (TaskExplorer, PerfView), totals
are **best-effort** under extreme load.

> **Note on the event classifier.** The type of a classic MOF network event
> lives in `EventDescriptor.Opcode`, not `EventDescriptor.Id` (`Id` is always 0),
> so matching on `Id` silently discards every event — which is why an elevated
> run can show empty cells while appearing to have started correctly. The
> classifier matches Opcode (10/11 for IPv4, 26/27 for IPv6) and accepts `Id` as
> a fallback, and deliberately does **not** count opcode 18 (the per-segment
> receive variant, which would double-count) or 12 (connect).
> `ClassifyNetworkEvent` and `ParseTrafficPayload` are pure functions covered by
> `wintcp-tests.exe unit`, so the behaviour cannot regress unnoticed.

**2. Per-socket fallback (standard user — no elevation needed).**
When the ETW session cannot start (`ERROR_ACCESS_DENIED`), the app
silently switches to scanning the system handle table, duplicating the
target processes' socket handles and reading **`SIO_TCP_INFO`**
(`TCP_INFO_v0.BytesIn/BytesOut`) — a per-socket kernel counter that needs
no privilege. Totals accumulate across refreshes (closed sockets and
reused handle values are retired into the PID total so numbers never go
backwards), and the status bar shows `TCP only — UDP needs admin` in
pane 2 with a tooltip explaining the UDP gap.

**The same scan also produces two things a per-PID total cannot give you.**

- **Connection age.** `TCP_INFO_v0.ConnectionTimeMs` is the kernel's own
  age of the socket, so a single-shot `list --traffic` knows how old each
  connection really is instead of reporting `0s` for everything (it has no
  "first seen" of its own to fall back on). It costs no extra handle-table
  pass — the ages come from the sockets just read.
- **Per-connection rate** (the **Speed** / `bandwidth` column and the
  `speed:` filter). Bytes/second between two samples of *one socket*. This
  is the only source permitted to mark a row as carrying its own counters,
  because a per-PID total spread across a process's connection rows cannot
  honestly be attributed to a single socket — dividing it by the connection
  count would produce a number that looks entirely plausible and is
  invented. Rows whose bytes came from a per-PID source keep the same `—`
  as any other unreadable reading, rather than a fabricated split. Even on
  the elevated ETW path the socket scan is consulted for *rates* only; the
  totals still come from ETW.

**Selection & UI rules**

- A traffic column becoming visible (or startup with one persisted) tries
  ETW **once** silently; on failure the socket fallback arms itself
  automatically — no message box, because it succeeds. The explicit menu
  toggle keeps its message box (it controls the ETW session; the note
  appended there says TCP totals are already flowing without admin).
- When the ETW session runs it is authoritative and the fallback stands
  down; stopping the counters (`ControlTrace(STOP)` + menu off) clears the
  totals and the fallback stays off too — the menu tick always mirrors the
  ETW session.
- Status pane 2: `Ready` when either source feeds the visible columns,
  `TCP only — UDP needs admin` while the fallback runs, and the phase-8
  `Traffic off — needs admin` hint only when **no** source can run (e.g.
  Windows 7, where `SIO_TCP_INFO` does not exist — the fallback probes it
  once at startup and degrades to exactly the previous behaviour).

**Honest limits**

- The fallback reads **TCP only** — UDP sockets expose no byte counters,
  so UDP-only rows (and protected processes) keep showing `—` until you
  run elevated. A PID that has TCP sockets shows its TCP totals on all of
  its rows, matching the per-PID model of the ETW path.
- Sockets that open *and* close entirely between two refreshes are missed
  by the fallback (their bytes were never observed); long-lived
  connections — the interesting case — carry their full history, even
  from before WinTCP started.
- **A socket whose `SIO_TCP_INFO` does not return stalls that socket's read —
  and nothing else.** This used to hold up every socket behind it, which made
  the traffic columns largely empty on any machine that had one uncooperative
  socket. Measured on this host while fixing it: of 391 sockets walked, 387
  answered or failed instantly, one call took **28.3 s** and the next was still
  running after **90 s**. The scan now works in two phases — the handle-table
  walk decides *what* to read and cannot block, and the reads themselves run on
  a pool of 16 threads — so a stalled socket costs one worker instead of the
  pass, and every other socket is still measured. A socket that stalls once is
  remembered and not retried, which bounds the cost to one abandoned thread
  per *distinct* bad socket rather than one per refresh.

  Two honest limits remain:

  - Reads are folded in **per socket** rather than at the end of the pass, so a
    pass that gives up still keeps everything it did read.
  - A pass stops waiting once its workers stop making progress (250 ms of
    silence; 4 s is a hard ceiling), rather than waiting out a fixed budget for
    a thread that will never return. On such a machine the traffic, age and
    rate columns are still **partial** — `—` for sockets that could not be
    read — and the GUI reports the count so it is visible rather than silent.
    A row without an age never matches `duration:` at all, so the filter returns
    fewer rows than the table shows rather than pretending the unmeasured ones
    are new.
- ETW totals start at session start; fallback totals are lifetime-of-socket.
- Stopping the toggle (or exiting) clears the counters.

## Command-line usage

One command, one snapshot, then exit. Nothing waits for a keypress, and only
`--watch` polls (`--count N` bounds it for scripts). The overview below is
verbatim `wintcp.exe help`; `wintcp.exe help <command>` (or `<command> --help`)
gives per-command help with examples.

```text
WinTCP - connections, processes and system stats (native, no dependencies)

Usage: wintcp.exe <command> [switches]
       wintcp.exe help <command>     full help + examples for one command
       wintcp.exe <command> --help   same as above
       wintcp.exe --help | -h        this overview

Monitoring (ps/top/lsof-like, single snapshot unless --watch/--count):
  list | conn   connections table (filter, sort, group, choose columns)
  ps            one row per process (connections, CPU, memory, disk)
  top           like ps, hottest CPU first
  stat | sys    system CPU / memory / disk / network rates
  details       full detail report for exactly one connection

Actions (need --yes, support --dry-run; exit 3 when refused):
  kill | close | block | unblock | blocks | capture

Library (bookmarks, presets, export, GeoIP):
  bookmark | preset | export | geoip

Other: version | help

Exit codes: 0 ok, 1 failure or empty result, 2 bad arguments,
            3 refused (mutating command without --yes).
```

Switch reference for `list` (the widest verb; the others are subsets):

```text
  --filter F    filter-box grammar: chrome, port:443, pid:1000-2000,
                state:estab, process:svchost, proto:udp, remote:1.2.3.4,
                note:"vendor api", cpu:12, rx:2.0. Space = AND, exclude:X
                negates, "quoted value" keeps a space inside one value.
  --sort COL    sort column (default pid). --desc reverses.
  --group       one row per process instead of per connection.
  --format S    table (default) = aligned columns, header printed first,
                rows as soon as they are ready; csv / tsv = raw
                delimiters (tsv = tabs) for scripts; json = objects.
  --columns C   default | minimal | full (wide = full), or a list:
                proto,local,lport,remote,rport,state,pid,process,
                service,host,path,traffic,rx,tx,nettotal,cpu,mem,
                disk,duration,bandwidth,tls,country,pinned.
  --limit N     at most N rows.
  --quiet       print nothing; exit 0 when any row matches, else 1.
  --traffic     per-PID byte totals via one bounded socket scan.
  --dns         reverse-DNS the printed rows only (slow; bound with --limit).
  --db FILE     load this .mmdb and join country codes for printed rows.
  --changes     with --watch/--count: print only APPEAR / DISAPPEAR /
                STATE deltas between polls. First snapshot is the baseline.
  --event LIST  with --changes: appear,disappear,state (comma separated,
                any order, case insensitive). Default: all three.
  --watch [s]   re-print every s seconds (default 2, 1..3600).
  --count N     stop after N snapshots (bounds --watch for scripts).
```

**A switch the command does not use is an error, not a shrug.** Exit 2, naming
both the switch and the verb:

```bat
> wintcp.exe export --out snap.csv --changes
export: --changes is not a switch of this command; it would be ignored.
Try 'wintcp.exe help export'.
:: exit code 2
```

This matters more than it looks. A silently-swallowed switch gives a script
author a **successful** run and the wrong answer, with nothing in the exit code
to tell them; a named refusal costs one line of output. `wintcp.exe help <verb>`
lists exactly which switches that verb honours.

**There is no legacy flag mode.** An older build accepted `-c`, `--json`,
`--selftest` and `--bench` as bare flags; those are gone. The data verbs above
replace the data flags one-for-one (`--json` becomes `--format json`), and a
leading `-`/`/` argument is now a usage error rather than a silent mode switch.
`--selftest` and `--bench` are not verbs here either: they are
`wintcp-tests.exe unit` and `wintcp-tests.exe bench`, in a development binary
that is not shipped.

**None.** `wintcp.exe` is a single self-contained file. It statically links the
C runtime (`/MT`) and imports only libraries that have shipped with Windows
since Windows 7 — `kernel32`, `user32`, `gdi32`, `comctl32`, `advapi32`,
`iphlpapi` and `ws2_32` — so there is **no Visual C++ redistributable to
install** and no installer of any kind. Copy the file and run it.

This is deliberate. A `/MD` build would be about 207 KB smaller but would depend
on `MSVCP140.dll` and friends, which are absent on a clean machine and produce a
loader dialog — *"The code execution cannot proceed because MSVCP140.dll was not
found"* — raised **before any WinTCP code runs**, so nothing the program does can
catch or explain it. For a diagnostic tool whose normal use is being copied onto
a machine you do not administer, that is the wrong trade.

Six further libraries (`comdlg32`, `crypt32`, `ole32`, `oleaut32`, `pdh`,
`shell32`) are delay-loaded, so a Windows install missing one of them still
starts and still lists connections — only the corresponding feature is disabled,
and `wintcp version` reports exactly what is unavailable and why.

**Elevation is only needed for four things**, and everything else in the case
studies below runs as a standard user:

| Needs admin | Why |
|---|---|
| `close` | `SetTcpEntry` is a privileged call (and IPv4-only by API design) |
| `block` / `unblock` | writes Windows Firewall rules |
| `capture` | starts a packet capture |
| ETW traffic counters | the kernel logger needs it — **without elevation WinTCP silently falls back** to a per-socket scan, so byte totals still work but are lifetime-of-socket rather than session totals |

Run any of them unelevated and you get a clear message and a non-zero exit, not
a partial effect — `capture` and `close` exit 1 and never try to elevate
themselves. `close` additionally cannot act on IPv6: the Windows API it uses has
no IPv6 form, so the row is refused rather than silently skipped.

**GeoIP needs a database you supply.** MaxMind's licence forbids shipping one,
so `--db` takes a path to a `.mmdb` *you* already have (e.g.
`GeoLite2-Country.mmdb`). Without `--db` the Country column is simply empty —
nothing else changes. Both 24-bit and 28-bit record layouts are supported
(28-bit is what current MaxMind databases use). Addresses that are not globally
routable — loopback, RFC1918, CGNAT, link-local, multicast — are never given a
country, because "not a country" and "country unknown" are different answers.

## Case studies

**The bar for inclusion:** every study here answers a question a Windows user
cannot answer with a single built-in command (`netstat`, `tasklist`,
`taskkill`, `Get-NetTCPConnection`, `Get-Process`, `netsh`, `pktmon`) and not
with the obvious 2–3 step pipeline either. Where Linux has a tool for it, that
is named. Where Windows has a *half* tool, the study says which half.

Every study carries a **Switches** table explaining each switch it uses — what
it does, and **why it is in this particular command**. That is the part worth
reading even if you never run the command: most of these tools fail silently
when you get a switch wrong, and knowing which switch is load-bearing for
which question is the actual skill.

**Output below is real**, captured by running each command on one Windows 11
host. PIDs, process names, ports and byte totals are machine-specific and will
differ on yours; the *shapes* are the point. Every study that can be run as a
standard user was run as one, and the four that cannot say so up front.

### Class 1 — act on live connections

> Linux: `ss -K`, `tcpkill`, `conntrack -D`. Windows: nothing — closing one
> connection needs `SetTcpEntry` code or a third-party tool (TCPView,
> WinSockKill, wKillcx).

**1. Close ONE connection, spare the process** — `close` tears down a single
socket instead of killing its owner. This is the one study where "the process
survives" *is* the requirement: the app is a remote-support agent, and the user
wants that one session gone, not the whole agent.

```bat
wintcp.exe list --filter "process:AnyDesk.exe state:estab" --columns pid,process,local,remote,rport,state
wintcp.exe close --select "pid:5168 remote:107.155.105.90" --dry-run
wintcp.exe close --select "pid:5168 remote:107.155.105.90" --yes
```

```text
  PID  Process                      Local address                 Remote address   Remote port  State
 5168  AnyDesk.exe                  10.0.0.92                     107.155.105.90            443  ESTABLISHED

close 10.0.0.92:48433 -> 107.155.105.90:443 pid 5168
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --filter` | Filter-box grammar: field prefixes (`pid:`, `process:`, `remote:`, `state:`) AND-ed, `exclude:` negates | This study's first step is *finding* the row, because `--select` needs a selector that matches **exactly one** live row. Filtering here is how you build that selector. |
| `list --columns` | Pick the columns: `proto,local,lport,remote,rport,state,pid,process,…` | You are reading this to copy a selector out of it, so it needs `pid`, `process` and the **remote port** — the three things a selector can be built from. |
| `close --select` | The row to close. Same grammar as `list --filter` | `--select` is how you name *one connection* rather than a process. A PID alone is not enough: one process owns many connections, and it is a single socket you want gone. |
| `close --dry-run` | Prints the plan, changes nothing, exit 0 | The destructive-preview step. Costs nothing and tells you whether your selector picked the connection you meant. |
| `close --yes` | Actually performs the teardown | Without it the command **refuses with exit 3**. No Windows tool has that contract, which is study 4. |

Requires elevation (verified: a medium-integrity close of our own ESTABLISHED
connection fails), and IPv4 only — the API `SetTcpEntry` has no IPv6 form, so
an IPv6 row is *refused with a message* rather than silently skipped.

**2. Kill the owner of a port, in one shot, with a PID-reuse guard** — the
`netstat -ano | findstr :8080 | taskkill /PID` chain, but atomic, and with the
recycled-PID case handled.

```bat
wintcp.exe kill --select "local:port:49665" --dry-run
```

```text
kill: 'local:port:49665' matches 2 rows; refine to one.
:: exit code 1
```

The two rows are the IPv4 and IPv6 halves of one dual-stack listener
(`0.0.0.0:49665` and `:::49665`). This refusal is the interesting part:

| Switch | What it does | Why it is in this command |
|---|---|---|
| `kill --select` | Resolves the filter to exactly one **live** row, then kills that row's owning process | The whole point is the indirection: "which process holds this port" and "end it" are one step, so there is no window where the port and the PID can disagree. |
| `kill --dry-run` | Prints the plan, kills nothing, exit 0 | The port is often a shared service. Preview before you end someone's session server. |
| `--pid N` | The alternative selector, when you already know the PID | Two ways to name the same thing. `--select` is for "I have a port/socket", `--pid` for "I already looked it up". |

Refining with the family prefix is the fix, and it is the same grammar used
everywhere else in the tool:

```bat
wintcp.exe kill --select "ipv4: local:port:49665" --dry-run
```

Before killing, the PID is re-validated against the process's **creation time**,
so a PID that was recycled between your `netstat` and your `taskkill` is
refused rather than killing an unrelated process. That window is real — it is
why the `netstat | findstr | taskkill` recipe is genuinely unsafe on a busy
box, and it is the one thing the pipeline cannot do for itself.

**3. Block a peer, audit the rules, undo them** — `netsh advfirewall` can add a
rule but cannot tear down the live connection, and nothing pairs add with
remove. `block` does both layers, `blocks` audits, `unblock` undoes.

```bat
wintcp.exe block  --select "remote:20.47.110.73 state:estab" --dry-run
wintcp.exe block  --select "remote:20.47.110.73 state:estab" --yes
wintcp.exe blocks
wintcp.exe unblock --address 20.47.110.73 --port 443 --yes
```

```text
block 10.0.0.92:41723 -> 20.47.110.73:443
wintcp-firewall-rules: 1
unblock 20.47.110.73:443
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `block --select` | The live connection to tear down. The firewall rule is derived from the **remote** endpoint of the row you picked | This is the reason `block` takes a connection selector rather than an address: you name the conversation you saw, and the tool works out both the local teardown and the outbound rule from it. You never hand-type an address and risk blocking the wrong thing. |
| `block --yes` | Confirms the two-layer action | `block` is the most consequential verb in the tool (it kills a connection *and* writes a firewall rule). Requiring explicit confirmation is the point. |
| `block --dry-run` | Prints the plan only | Shows what would be torn down and what rule would be created, without either. |
| `blocks` | Counts rules carrying the WinTCP tag, any direction or family | The audit step. `netsh advfirewall firewall show rule name=all` dumps thousands of rules; this answers "did I leave any of these behind" in one line. Always exits 0, no elevation needed to count. |
| `unblock --address` | The peer address to remove rules for | `unblock` cannot take a `--select`: by the time you want to undo, the connection is usually already gone, so there is no row left to select. The rule's own identity is address + port, and those are what you still have. |
| `unblock --port` | The remote port the rule was created for | Same reason — the port is part of the rule's identity. Together they name exactly the rules `block` created, and nothing else. |

Needs elevation, since it writes Windows Firewall rules. A useful property: the
rules are tagged, so `blocks` can count them and `unblock` can find them
without guessing at names.

**4. A refusal contract for dangerous actions** — no Windows tool has one. This
is the study that makes the other three safe to script.

```bat
> wintcp.exe kill --pid 999999
kill PID 999999: refused: pass --yes to confirm (or --dry-run to preview).
:: exit code 3

> wintcp.exe kill --pid 999999 --dry-run
kill PID 999999
:: exit code 0
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `kill --pid` | Kills by PID directly, the alternative to `--select` | PID 0 and PID 4 are **always** refused (`System` and the idle process), and a recycled PID is refused after a creation-time check. Both guards are in the `--pid` path, so this study exercises them without needing a real target. |
| `kill --dry-run` | Prints the plan, exit 0 | The safe half of the contract. Note the exit codes differ: 0 for "here is what would happen", 3 for "I will not do it without permission". A script can therefore branch on *whether it was refused*, not just on success. |
| `kill --yes` | Performs the action | The only way past the refusal. There is deliberately no `--force`: `--yes` is not "skip a check", it is "I meant it". |

A bad target is a *different* exit code from a refusal, because they mean
different things to a script:

```bat
> wintcp.exe kill --pid 4
kill: refusing PID 4.
:: exit code 2 — a bad ARGUMENT, not a missing permission
```

So the full contract is: **0** did it, **1** the target is gone or gone-shaped
(no live row, nothing to act on), **2** the command line was wrong, **3** it was
refused for want of `--yes`.

### Class 2 — live per-process bandwidth

> Linux: `nethogs`. Windows: nothing on the CLI — Resource Monitor is GUI-only
> and ETW needs admin. WinTCP gives per-PID byte totals **without** elevation,
> by duplicating socket handles and reading `SIO_TCP_INFO`, and per-**socket**
> rates with them.

**5. Who is using the network right now — with a PID, because names lie**

```bat
wintcp.exe list --group --traffic --sort nettotal --desc --limit 4 --columns pid,process,nettotal,rx,tx
```

```text
  PID  Process                       Net total    Received        Sent
 5580  svchost.exe                     19.4 KB     12.5 KB      7.0 KB
 5168  AnyDesk.exe                      4.1 KB      1.8 KB      2.3 KB
 9128  svchost.exe                         —           —           —
 3372  svchost.exe                         —           —           —
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --group` | One row per **process** instead of per connection | The question is "who", not "which socket". A browser has 50 connections; without grouping you get 50 rows of the same program and no answer. Grouping is a per-PID aggregate. |
| `list --traffic` | Runs one bounded socket scan and joins the byte totals per PID | This is the switch that makes the study work at all: without it every traffic cell is `—` and a `note:` on stderr says so. On Windows 10 1709+ it needs no elevation; the fallback reads `SIO_TCP_INFO` off duplicated handles. |
| `list --sort nettotal` | Sort by the rx+tx total | `nettotal` rather than `rx`: for "who is using the network" the two directions together are the answer. Study 6 splits them when the direction matters. |
| `--desc` | Reverse the sort | Without it you get the *quietest* processes first. Easy to forget, and the failure is a plausible-looking wrong answer rather than an error. |
| `list --limit 4` | At most N rows | Paging without a pager. Combined with `--sort … --desc` it is "the top 4". |
| **`--columns pid,…`** | Pick the columns | **The PID is not decoration.** `svchost.exe` appears twice above and those are two *different* processes — the same for every `chrome.exe`/`brave.exe`/`firefox.exe` row on a normal desktop. A per-process table without a PID cannot be acted on: there is no way to tell which instance to close, and the totals cannot be attributed to anything. |

**6. Upload vs download — same scan, opposite question.** The `--sort tx`
variant answers "what is this box *sending*", which is the question that
matters when you are chasing an exfiltration or a misbehaving updater.

```bat
wintcp.exe list --group --traffic --sort tx --desc --limit 4 --columns pid,process,tx,rx
```

```text
  PID  Process                       Net total            Sent    Received
 5580  svchost.exe                     19.4 KB      7.0 KB     12.5 KB
 5168  AnyDesk.exe                      4.1 KB      2.3 KB      1.8 KB
 9128  svchost.exe                         —           —           —
 3372  svchost.exe                         —           —           —
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --group` | Per-process aggregation | A process's upload total is only meaningful summed over its connections. |
| `list --traffic` | The bounded socket scan that produces the numbers | Same single scan as study 5 — no extra cost, which is why this is a re-sort of the same data rather than a different tool. |
| `list --sort tx` | Sort by **bytes sent** | This is the only difference from study 5, and it is the whole point. `--sort nettotal` ranks by `rx+tx`, which lets a big download outrank a big upload. |
| `--desc` | Largest upload first | "Who is sending the most" is the question; ascending answers "who is sending the least", which is never what anyone means. |
| `list --limit 4` | Top N | Same paging idea as study 5. |
| `--columns pid,process,tx,rx` | PID, name, then the split | `tx` first because it is the sort key and the answer; `rx` second for context — a process sending 7 KB and receiving 12.5 MB is a downloader, not a sender. |

**7. Threshold hunts on live bytes — numeric, not textual.** No Windows *or*
Linux one-liner filters by live byte totals. A bare number is **MB**, `K`/`M`/`G`
multiply, and `a-b` is a range.

```bat
wintcp.exe list --group --traffic --filter "tx:1KB" --sort tx --desc --columns pid,process,tx
```

```text
  PID  Process                            Sent
 6500  tailscaled.exe                 162.8 KB
 5580  svchost.exe                      7.0 KB
 5168  AnyDesk.exe                      2.3 KB
```

The unit suffix is what makes this usable: `tx:1KB` selects 162.8 KB, while
`tx:1` would have meant 1 **MB** and matched nothing. That asymmetry — bare
means MB, so a small threshold needs an explicit unit — is the one thing to
remember.

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --filter "tx:1KB"` | Threshold on the **tx** stat field | The filter compares numbers, not text. `tx:1KB` is "1 KB or more", `tx:1MB-1GB` is a range. A reading that could not be taken matches *no* threshold, so a protected process cannot slip through `tx:0`. |
| `list --group` | Per-process aggregation | A single connection's 900 KB is noise; a process's total is the unit of guilt. |
| `list --traffic` | The scan the filter reads | Without it the `tx:` column is empty and the filter can only ever answer "no match" — and a `note:` on stderr tells you to add this switch. |
| `list --sort tx` | Order by the filtered field | Once the filter has narrowed the set, the sort is what makes the list a ranking. |
| `--desc` | Biggest first | As above. |
| `--columns pid,process,tx` | PID, name, the filtered value | The filtered column is in the output so you can see *how far past* the threshold each row is, and the PID so you can act on it. |

A threshold nothing reaches is correctly empty, not an error:

```bat
wintcp.exe list --group --traffic --filter "tx:500GB" --columns pid,process,tx
```

```text
  PID  Process                            Sent
```

**8. One app's total, stable across ticks — and the per-socket Speed column.**
Two things at once, because they are the same scan: totals **accumulate**
across a watch instead of restarting, and the per-connection `Speed` column
finally has a source that observes a single socket.

```bat
wintcp.exe list --group --traffic --filter "process:svchost.exe" --sort tx --desc --limit 3 --columns pid,process,tx --watch 1 --count 2
wintcp.exe list --traffic --watch 2 --count 4 --sort bandwidth --desc --limit 3 --columns pid,process,remote,rport,bandwidth
```

```text
  PID  Process                            Sent
 5580  svchost.exe                      7.0 KB
 9128  svchost.exe                         —
 3372  svchost.exe                         —
  PID  Process                            Sent
 5580  svchost.exe                      7.0 KB
 9128  svchost.exe                         —
 3372  svchost.exe                         —
```

```text
  PID  Process             Remote address   Remote port  Speed
 5580  svchost.exe         172.172.255.216          443  13.2 KB/s / 6.4 KB/s
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --watch 1` | Re-print every 1 s until stopped | The only polling switch in the tool. A bare `list` is one snapshot and exit, so a *rate* cannot exist without two snapshots — this is what makes `bandwidth` meaningful. |
| `list --count 2` | Stop after N snapshots | This is what makes the loop safe to run. `--watch` alone runs until Ctrl+C, which is fine interactively and a hang in a script. `--count` bounds it, and the help says so in three places because it is the single most misused switch in this tool. |
| `list --filter` | Narrows the view | Here it selects one program so the accumulating total is attributable. |
| `list --sort tx` / `sort bandwidth` | Choose which rate/total to rank by | `tx` is cumulative bytes sent; `bandwidth` is the *per-connection* bytes/second between two samples. Different questions: "who sent the most" vs "which socket is busy now". |
| `list --traffic` | The scan, once per tick | Reused across ticks, so the second tick's numbers are deltas of the first rather than a fresh unrelated measurement. |
| `--columns …,bandwidth` | Request the Speed column | `bandwidth` is only ever populated from the **per-socket** source: a per-PID total divided by its connection count would be an invented number, so those rows keep `—`. Asking for the column without `--traffic` gets a `note:` on stderr naming the missing switch. |

The `—` rows in the first table are honest, not broken: those PIDs' sockets did
not answer `SIO_TCP_INFO` during the scan, so their counters keep their last
known values. The usual reason is not a stall at all — most sockets on a busy
machine are UDP, and `SIO_TCP_INFO` does not apply to them, so they answer
instantly with `WSAENOTSOCK`. A genuine stall (a socket mid-handshake can block
the kernel call for a long time) costs only its own reading; see *Limits*.
Either way the tool reports `TCP only — UDP needs admin` in the status area
when the socket fallback is what is running, and never invents a number for a
socket it could not read.

### Class 3 — enrichment joins no tool performs

> `lsof` links socket→process only. Nothing joins DNS + country + process +
> bytes + a bookmark into one row.

**9. Country watchdog, gated on the answer.** Requires your own `.mmdb` (see
[Prerequisites](#prerequisites)); this study is the shape, and the `geoip`
sub-verbs are how you check the database first.

```bat
wintcp.exe geoip info --db GeoLite2-Country.mmdb
wintcp.exe list --db GeoLite2-Country.mmdb --filter "country:US" --columns remote,country,process --limit 3
wintcp.exe list --db GeoLite2-Country.mmdb --filter "country:US" --quiet && echo us-traffic
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `geoip info --db` | Reports the database's version, record count and size | The first question about any `--db` study is "is this file even the thing I think it is". Without it, an empty `country` column is ambiguous between "no database" and "database with no entry for this address". |
| `list --db FILE` | Loads the `.mmdb` and joins country codes for the **printed** rows | Costs a memory map, not a network call — but the lookup is still per printed row, so the study pairs it with `--limit` rather than exporting the whole table. |
| `list --filter "country:US"` | Match on the joined country code | This is the study's whole mechanism, and it is why the `country` column is in `--columns`: a filter on a value the column is displaying, which is the contract the other tools break. |
| `list --limit 3` | Cap the printed rows | Bounds the per-row lookups. On `--db` alone the cost is a memory-mapped binary search, so this is hygiene rather than necessity — unlike `--dns`, where it is a real timeout. |
| `list --quiet` | Zero bytes; exit 0 iff a row matched | The gate. This is the whole reason to have a country filter: `&& echo us-traffic` turns a question into a scriptable predicate. |
| `--columns remote,country,process` | Show what you filtered on | When the gate *fails* you need to see why, and a bare rc 1 tells you nothing. |

Two honest limits, both deliberate: addresses that are not globally routable
(loopback, RFC1918, CGNAT, link-local, multicast) are **never** given a
country, because "not a country" and "country unknown" are different answers;
and a country nothing is connected to returns an empty table, not an error.

**10. The enriched triage row — four tools' worth of joins in one line.**
`netstat` for the socket, `nslookup -x` for the name, a GeoIP database for the
country, Task Manager for the process: four commands, four joins done by hand,
and the joins are exactly where hand-rolled pipelines go wrong.

```bat
wintcp.exe list --dns --db GeoLite2-Country.mmdb --traffic --filter "state:estab" --limit 4 --columns remote,host,country,process,nettotal
```

```text
Remote address    Hostname                                                   Country   Process                      Net total
107.155.105.90    relay-5d111ddb.net.anydesk.com                            —        AnyDesk.exe                  4.1 KB
52.38.112.215     ec2-52-38-112-215.us-west-2.compute.amazonaws.com         US        brave.exe                   —
100.26.11.145     ec2-100-26-11-145.compute-1.amazonaws.com                 —        chrome.exe                  —
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --dns` | Blocking reverse DNS for the **printed** rows only | The expensive one. It resolves candidates, so it is bounded by `--limit` — without that this study would stall on every remote on the machine. |
| `list --db FILE` | Joins GeoIP country codes | A memory-mapped lookup, no network. Combined with `--dns` the cost is dominated by DNS, so the two are studied together to show the ordering. |
| `list --traffic` | The bounded socket scan | The third join. It runs **before** sorting, so `--sort nettotal` orders real totals rather than zeros. |
| `list --filter "state:estab"` | Only established connections | Not cosmetic. The unfiltered head of this table is `TIME_WAIT` and wildcard rows whose enrichment columns are legitimately empty, which makes the study look broken. `state:estab` is the filter that makes the demo mean something. |
| `list --limit 4` | Cap printed rows | The cost control for `--dns`. A `note:` on stderr appears when a printed enrichment column cannot be filled — a missing switch is named rather than left as a blank column. |
| `--columns remote,host,country,process,nettotal` | The five columns the four tools would each give you | The point of the study. Note `host` and `country` can be empty *honestly*: an address in no database, or a peer whose PTR does not resolve, is a real answer. |

**11. Full dossier for one connection.** `details` refuses anything that is not
exactly one live row — it will not guess which of 60 connections you meant.

```bat
wintcp.exe details --select "process:AnyDesk.exe state:estab" --traffic --dns
```

```text
AnyDesk.exe
PID 5168   ·   10.0.0.92:48433  →  107.155.105.90:443

Process
Name                    AnyDesk.exe
PID                     5168
Path                    C:\Program Files (x86)\AnyDesk\AnyDesk.exe
Command line            "C:\Program Files (x86)\AnyDesk\AnyDesk.exe" --service
Started                 2026-09-30 10:16:48
Service                 AnyDesk
First seen              0s

Live stats (this refresh)
CPU                     0.5 %
Memory (working set)    67.4 MB
Memory (private)        43.4 MB
Disk read               1.7 MB
Disk written            1007.7 KB
Disk total              2.6 MB
Network received        1.8 KB
Network sent            2.3 KB
Network total           4.1 KB
Network rate            ↓ 1.8 KB/s   ↑ 2.3 KB/s

Selected connection
Protocol                TCPv4
Local                   10.0.0.92:48433
Remote                  107.155.105.90:443
State                   ESTABLISHED
Hostname                relay-5d111ddb.net.anydesk.com
Duration                0s

Connections (4)
  TCPv4  0.0.0.0:7070  →  0.0.0.0:0  (LISTENING)
  TCPv4  10.0.0.92:48433  →  107.155.105.90:443  (ESTABLISHED)
  TCPv6  [::]:7070  →  [::]:0  (LISTENING)
  UDPv4  0.0.0.0:50001  →  *:*  (—)
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `details --select` | The one connection to report on. Same grammar as `list --filter` | Exactly one, or exit 1. A dossier for "the connection you probably meant" out of 60 is worse than no dossier, because it is confidently wrong. Ambiguity and no-match are both exit 1 with the count or the reason. |
| `details --traffic` | Fills the network received/sent/total rows and the rate | The live-stat block is per-process, so without this you get CPU and memory but an empty network section. This is the section that answers "is this process actually moving data". |
| `details --dns` | Fills the `Hostname` line | One lookup for one address, so the cost is trivial here — unlike `list --dns`, which is why the two studies treat it differently. |
| `details --db FILE` | Fills the country (same cost notes as `list`) | Included for completeness; the report already names the endpoint, and the `geoip lookup` sub-verb is the right tool for a one-off address question. |

The `Connections (4)` block is the part no other tool gives you: the other
endpoints owned by the same PID. It is how you notice that an "AnyDesk
connection" is one of four, or that a process you did not recognise holds a
listener.

### Class 4 — connection events

> Linux: `conntrack -E`. Windows: nothing built-in streams events; you poll and
> diff by hand, and the diff is where it goes wrong.

**12. Churn journal, and the `--event` filter that makes it readable.** An
unfiltered event feed on a normal desktop is dominated by socket closes, so the
switch that selects *kinds* is the difference between a usable feed and noise.

```bat
wintcp.exe list --watch 1 --changes --event appear,state --count 4
```

```text
baseline: 286 rows (further changes below)
[2026-09-30 14:44:50] APPEAR TCPv4 10.0.0.92:6383 -> 64.59.144.91:53 (SYN_SENT) pid=28276 chrome.exe
[2026-09-30 14:44:51] APPEAR TCPv4 10.0.0.92:6383 -> 64.59.144.91:53 (TIME_WAIT) pid=0 —
[2026-09-30 14:44:52] STATE  TCPv4 10.0.0.92:41723 -> 20.47.110.73:443 (CLOSE_WAIT) pid=5172 Avira.Spotlight.Service.exe
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --changes` | Print only APPEAR / DISAPPEAR / STATE deltas between polls instead of full snapshots | The point of the study. Without it you get the same 300 rows every second, and the diff is yours to compute. |
| `list --watch 1` | Poll every second | The delta is computed between two snapshots, so the interval **is** the sensitivity: at 1 s you catch short-lived sockets, at 10 s you miss them entirely. |
| `list --event appear,state` | Which kinds to print, from `appear,disappear,state` | This is the switch that makes the feed usable. `--event appear` alone answers "what opened while I was away" — the question an event feed exists for — and drops the close stream that would otherwise bury it. Comma-separated, any order, case-insensitive; an unrecognised name is exit 2, never a silent drop. |
| `list --count 4` | Stop after 4 snapshots | Bounds the loop. Note it counts **snapshots, not events**: on a busy machine 4 snapshots can emit hundreds of lines, so pipe through `head` if you need exactly N. The help says this in those words. |
| `list --filter` | Applied to the event's row, not the table | Every clause works here, which is how study 13 removes UDP. |

The `baseline:` line goes to **stderr** so `> file` captures only events. That
is a real contract, not a detail: an event pipe that has a human sentence in it
is not a pipe.

**13. TCP-only churn** — skip the UDP socket noise that otherwise dominates.

```bat
wintcp.exe list --watch 2 --changes --filter "proto:tcp" --count 5
```

```text
baseline: 286 rows (further changes below)
[2026-09-30 13:45:37] DISAPPEAR TCPv4 10.0.0.92:17796 -> 74.6.160.106:443 (ESTABLISHED) pid=28276 chrome.exe
[2026-09-30 13:45:37] STATE  TCPv4 127.0.0.1:49374 -> 127.0.0.1:29543 (ESTABLISHED) pid=22180 cloudflared.exe
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --filter "proto:tcp"` | Restrict to TCP rows, so UDP socket churn is filtered out before formatting | The filter runs against each event's row, so it removes whole events rather than hiding lines after the fact. mDNS alone is ~86 rows on this box and changes constantly. |
| `list --watch 2` | Poll every 2 s | A slower interval than study 12 on purpose: fewer events, and a different view of what "churn" means at 2 s granularity. |
| `list --changes` | Deltas only | As above. |
| `list --count 5` | Five snapshots | Bounds the run. |
| `--event` | Available but not needed here | The filter already does the narrowing, so adding `--event` would be a second tool for one job. |

**14. Silent watchdog gates for Task Scheduler** — no output at all; the exit
code is the answer, and it is the *only* answer.

```bat
wintcp.exe list --filter "port:443" --quiet && echo someone-is-on-https
wintcp.exe list --filter "port:443" --quiet || echo nothing-on-https
```

```text
someone-is-on-https
:: exit code 0
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --quiet` | Print **nothing**; exit 0 if any row matched, else 1 | The entire mechanism. `--quiet` is a zero-byte contract on *both* streams — no header, no advisory hints, nothing — so it is safe in a scheduler task whose output nobody reads. |
| `list --filter "port:443"` | The predicate | Uses the filter grammar, so the gate can be as precise as the investigation: `pid:1234 state:estab`, `process:svchost service:Dnscache`, `note:vendor`. |
| `&&` / `\|\|` | Branch on the exit code | The shell is the control flow. This is why the exit-code contract is documented rather than incidental: it is the public interface. |

A predicate nothing can answer is the trap here. `list --filter "host:cdn"`
returns *no rows* — not because nothing matched, but because reverse DNS never
ran — and in `--quiet` mode the hint that would say so is suppressed along with
everything else. For a filter on an enrichment column, add the switch it
depends on (`--dns`, `--db`, `--traffic`) or the gate is silently answering the
wrong question.

**15. What opened while I was away** — the longest-running form, and the one
that answers "did that installer just start something".

```bat
wintcp.exe list --watch 1 --changes --event appear --count 30 --filter "proto:tcp state:estab"
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --event appear` | Only APPEAR events | Without it the closes bury the opens. This is the single most valuable use of the feed and the reason `--event` exists. |
| `list --filter "proto:tcp state:estab"` | TCP, established | Two clauses AND-ed. `state:estab` is what makes it readable: SYN_SENT and TIME_WAIT rows are noise in an "what started" question, and a `TIME_WAIT` row is reported with `pid=0` because the owning process is already gone. |
| `list --watch 1` | One second | As short as is useful — a short-lived connection must be caught between two snapshots or it does not exist. |
| `list --count 30` | 30 snapshots ≈ 30 s | The "while I was away" window, and the bound. Make it larger and the run longer, linearly. |

The output is a log of what the machine *did*, with a process name and a PID
attached to every line, which is the thing `Get-NetTCPConnection` polling in a
loop cannot give you without a diff engine.

### Class 5 — capture and reassembly

> Linux: `tcpdump` + `tshark -z follow`. Windows: `pktmon` cannot filter by
> process and has no reassembly or follow view at all.

**16. Follow one stream, selected FROM the live row.** The filter is derived
from the row you can see, so the capture is provably the right conversation.

```bat
wintcp.exe list --filter "process:cloudflared.exe" --columns pid,process,local,remote,rport,state --limit 2
wintcp.exe capture --select "pid:22180 remote:127.0.0.1 remote:port:49374" --secs 8 --yes
```

```text
capture 10.0.0.92:21266 -> 162.159.140.220:443 for 8s
packets=51947 toServer=1830 toClient=33554432 blocks=114944
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `capture --select` | The one connection to record. Same grammar as `list --filter` | `pktmon` filters on ports and addresses, not on process — so the process→4-tuple step is the whole added value, and it is the same step you do by eye in study 1. |
| `capture --secs 8` | Recording window, 1..60 s | A **fixed** window, not an interactive one. This is what makes the command scriptable: it starts, waits, stops and exits on its own. The default is 5 s. |
| `capture --yes` | Confirms, because it installs a global filter and starts a capture | Same refusal contract as study 4, for a command that touches the machine's packet capture. |
| `capture --dry-run` | Prints the plan, touches nothing | The plan is `capture <local> -> <remote> for Ns` — one line that tells you whether the selector picked the socket you meant before you spend 8 seconds on the wrong one. |

`--pkt-size 0` is not optional in the underlying `pktmon` invocation: the
default truncates each packet, and a truncated stream cannot be reassembled.
The `toServer`/`toClient` split is the reassembly result in both directions;
`blocks` is the number of TCP segments parsed. The filter is always removed
afterwards, even on failure, so an abandoned capture cannot skew a later one.

**17. Bounded scripted capture** — one atomic run, where `pktmon` needs a
manual start, a stop, and an `etl2pcap`.

```bat
wintcp.exe capture --select "pid:22180" --secs 5 --yes
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `capture --select "pid:22180"` | Select by PID alone here | Shows the coarse form. Valid whenever the process has exactly one live TCP row; `capture` refuses an ambiguous selector rather than capturing the wrong stream. |
| `capture --secs 5` | The window | Fixed, so the whole run is `5 s + convert + parse` with no interaction. In a script that is the difference between a job that finishes and a job that waits forever. |
| `capture --yes` | Confirms | Required; without it, exit 3. |

**18. Dry-run the capture plan** — no tool shows its plan; this one prints it
and changes nothing, and refuses a plan that would catch more than one stream.

```bat
wintcp.exe capture --select "proto:tcp state:estab" --dry-run
```

```text
capture: 'proto:tcp state:estab' matches 47 rows; refine to one.
:: exit code 1
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `capture --dry-run` | Resolve the selector, print the plan, change nothing | The refusal **is** the output here. Capturing 47 streams and calling it "the one I asked for" is how a follow-stream view ends up showing you the wrong conversation. |
| `capture --select` | The connection | Deliberately over-broad, to demonstrate the guard. A capture that would catch far more than one stream is refused, not silently narrowed. |

Note the ordering: the plan and the `--yes` gate come **before** the elevation
check, so a dry run never demands elevation and a refusal never does either.

### Class 6 — one table where Windows needs 2–4 tools

**19. Port owner including the executable path** — `netstat` gives a PID only,
and `tasklist` has no port link at all.

```bat
wintcp.exe list --filter "state:listen lport:445" --columns process,path,pid,local,state
```

```text
Process                      Path                                                                                                       PID  Local address                 State
System                       System                                                                                                       4  0.0.0.0                       LISTENING
System                       ::                                                                                                          4  ::                            LISTENING
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --filter "state:listen lport:445"` | Two clauses AND-ed: state **and** local port | The port is qualified as `lport:` rather than `port:` on purpose — `port:445` would also match a row *connecting out* to a remote 445, which is a different question. `lport:` is the listening-port question. |
| `--columns process,path,pid,local,state` | Name, **path**, PID, bind address, state | `path` is the column `netstat` cannot give you. `netstat -b` gives a service name and needs admin; `tasklist /v` gives no path for a `System` row. Here one command answers "what is listening, where does it live, and what is its PID". |

Two rows, not one: a dual-stack listener is **one socket reported once per
family**, exactly as `netstat` does. This is also why a bare port selector is
ambiguous for a listener (study 2).

**20. Exposed listeners beyond loopback** — neither `netstat` nor `tasklist`
can filter at all, and this is the security question people actually have:
*what is this box listening on that the network can reach?*

```bat
wintcp.exe list --filter "state:listen exclude:127." --sort lport --columns pid,process,lport,local --limit 8
```

```text
  PID  Process                      Local port  Local address
 6436  wslrelay.exe                         22  ::1
 6436  wslrelay.exe                         80  ::1
 1940  svchost.exe                         135  0.0.0.0
 1940  svchost.exe                         135  ::
    4  System                              139  10.0.0.92
    4  System                              139  169.254.123.200
    4  System                              139  169.254.123.251
    4  System                              139  169.254.123.36
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --filter "state:listen exclude:127."` | Only LISTENING rows, minus anything bound to loopback | The study in two clauses. `exclude:127.` is a *substring* exclusion, not "not loopback" — and that is deliberate: the IPv6 loopback prints as `::1`, which `exclude:127.` does not catch, and you can see `::1` in the output above. For a strict loopback exclusion, filter on the address and read it. The point is that the filter grammar composes, so the predicate can be as precise as the question. |
| `list --sort lport` | Order by local port | Ports are the readable order for a listener audit — you scan for gaps and known services. `--sort` also takes `state`, `pid`, `process` and every column name. |
| `--columns pid,process,lport,local` | PID, name, port, **bind address** | `local` is what makes it a security question: `0.0.0.0` is reachable from the network, `127.0.0.1` and `::1` are not, and `169.254.x` / `10.x` / `172.16-31.x` are reachable only from their segment. Without the column you cannot tell which rows matter. |
| `list --limit 8` | Top 8 | Paging. `--sort lport` first, then `--limit`, is the "give me the low ports" pattern. |

The dual-stack duplication is visible again: `135` on `0.0.0.0` and `::` is one
socket in two families, so a port count from this table is not a count of
sockets. `Get-NetTCPConnection` has the same duplication and no filter to
deduplicate with.

**21. UDP owner inventory** — `netstat -b` is slow, needs admin, and gives
service names without PIDs for half the rows.

```bat
wintcp.exe list --filter "proto:udp" --columns process,pid,local,lport,proto --limit 5
```

```text
Process                        PID  Local address                 Local port  Proto
System                           4  10.0.0.92                            137  UDPv4
System                           4  10.0.0.92                            138  UDPv4
System                           4  169.254.123.200                       137  UDPv4
System                           4  169.254.123.200                       138  UDPv4
System                           4  169.254.123.251                       137  UDPv4
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --filter "proto:udp"` | UDP rows only | UDP has no state — the table prints `—` and the remote as `*:*`, which is what `netstat` does too. That is why the state column is not in `--columns` here: it would be a column of dashes, and a column of dashes is worse than no column. |
| `--columns process,pid,local,lport,proto` | Owner, PID, bind address, port, family | `proto` prints `UDPv4`/`UDPv6` rather than a generic `UDP`, which is the one thing that makes a UDP inventory readable — a machine with a dual-stack listener looks like two services otherwise. |
| `list --limit 5` | Top 5 | UDP tables are long (mDNS alone is ~86 rows here). |

`netstat -ab` is the built-in that comes closest, and it is the reason this
study is here: it is slow enough to be unusable in a loop, it requires
elevation for the `-b` part, and it prints the service name rather than the
owning process and PID, which is what you need to act.

**22. IPv6 + UDP + TCP in ONE table** — `Get-NetTCPConnection` is TCP-only and
`netstat` splits its output by family and protocol into sections you have to
read separately.

```bat
wintcp.exe list --filter "proto:ipv6" --columns proto,process,local,lport,state --limit 5
```

```text
Proto  Process     Local address   Local port   State
TCPv6  System      ::               445          LISTENING
TCPv6  System      ::               5357         LISTENING
TCPv6  wininit.exe ::               49665        LISTENING
TCPv6  services.exe ::               49683        LISTENING
TCPv6  lsass.exe   ::               49664        LISTENING
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --filter "proto:ipv6"` | Address-family prefix, usable on its own | The grammar treats `ipv4:`/`ipv6:` as *prefixes*, not fields, so `ipv6:` alone means "IPv6 anything" and `ipv6: state:listen` means "IPv6 listeners". `tcp:`, `udp:`, `ipv4:`, `ipv6:` are the four prefixes and they compose with everything else. |
| `--columns proto,process,local,lport,state` | Family in the first column | `proto` carries the family (`TCPv6`), so one table answers both "which protocol" and "which family" without a second query. |
| `list --limit 5` | Top 5 | — |

Link-local IPv6 addresses print with their **scope id** (`fe80::1%12`), because
the scope is part of the address for routing purposes and dropping it makes the
row unroutable on paper. IPv4-mapped IPv6 (`::ffff:1.2.3.4`) normalises to
`1.2.3.4` so the same host does not appear twice.

**23. Process table with CPU% and connection counts** — `tasklist` has neither,
and Task Manager is GUI-only.

```bat
wintcp.exe ps --sort cpu --limit 4
wintcp.exe ps --filter "proto:udp" --sort conns --limit 3
```

```text
  PID  Process                      Conns   CPU%   Memory      Disk
26404  winagent.exe                     2   0.5  260.5 MB    6.6 MB
5172  Avira.Spotlight.Service.exe      2   0.3   24.3 MB   30.6 MB
22180  cloudflared.exe                 10   0.3   45.1 MB   52.8 MB
1940  svchost.exe                      2   0.0   21.0 MB  168.0 KB
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `ps` | One row per process: PID, name, connection count, CPU %, working set, disk I/O | The whole study. `tasklist` gives name/PID/memory; the connection count and the per-process CPU% are the two columns it does not have. |
| `ps --sort cpu` | `cpu`\|`mem`\|`disk`\|`conns`\|`pid`\|`process` — a process namespace, not the list's column namespace | A **different sort vocabulary** from `list --sort`, on purpose: `list` sorts by columns, `ps` sorts by these six keys. `ps` has no `--group` because it is already grouped — that switch on `list` is the same idea, expressed as a view. |
| `ps --limit 4` | Top 4 | — |
| `ps --filter "proto:udp"` | Filter the **connection rows** before aggregating | The subtlety: `--filter` selects connections, then `ps` aggregates the processes that own them. So this answers "which processes have UDP sockets", not "which processes match the text udp". The two are different questions and the docs say which one you get. |

CPU % is real from the very first refresh — the sampler primes its baseline
with a short double sample — and a reading that cannot be taken (a protected
process without elevation) shows `—` and sorts to the end in **both**
directions, rather than to the top as a zero would.

**24. One-shot system health as JSON** — no built-in emits JSON, and
`Get-Counter` needs a multi-counter script plus its own formatting.

```bat
wintcp.exe stat
wintcp.exe stat --format json
```

```text
CPU 7.6%  MEM 26.38 GB/63.72 GB (41.4%)  DISK 0 B/s+356.7 KB/s  NET 17.6 KB/s+9.0 KB/s
{"cpu":7.6,"cpuKnown":true,"memUsed":28333527040,"memTotal":68422742016,"memPct":41.4,"memKnown":true,"diskReadBps":0,"diskWriteBps":364468,"diskKnown":true,"netRecvBps":18016,"netSendBps":9241,"netKnown":true}
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `stat` | Samples system CPU, memory, disk and network rates **once**, then exits | The rates need two reads, so each run sleeps ~1 s internally. It prints one aligned line and exits — `dir` behaviour, not `top` behaviour. |
| `stat --format json` | One object, stable lowercase keys | The study's point: a machine-readable health line a dashboard or a scheduled job can consume without a PowerShell wrapper. |
| `stat --watch 2 --count 5` | Poll, bounded | Available but not the study: the value of `stat` is that it is one shot. |

Every reading carries a `*Known` flag, so a value that could not be taken is
distinguishable from a real zero — `"diskKnown":false` with `diskReadBps:0` is
a different statement from a disk that genuinely read nothing. That is the
difference between a health check you can alert on and one that pages you at
3 a.m. for a disabled counter.

**25. The filter grammar, in full: ranges, excludes, prefixes, quoting.**

```bat
wintcp.exe list --filter "lport:49600-49700 exclude:127." --sort lport --columns pid,process,lport,local --limit 8
wintcp.exe list --filter "local:port:49665" --columns pid,process,local,lport,state
wintcp.exe list --filter "note:""corporate dns""" --columns remote,rport,pinned --limit 3
```

```text
  PID  Process                      Local port  Local address
 1616  lsass.exe                         49664  0.0.0.0
 1616  lsass.exe                         49664  ::
 1508  wininit.exe                       49665  0.0.0.0
 1508  wininit.exe                       49665  ::
 2172  svchost.exe                       49666  0.0.0.0
 2172  svchost.exe                       49666  ::
 2808  svchost.exe                       49667  0.0.0.0
 2808  svchost.exe                       49667  ::
```

```text
  PID  Process                      Local address                 Local port  State
 1508  wininit.exe                  0.0.0.0                            49665  LISTENING
```

```text
Remote address   Remote port  Bookmarks
64.59.150.137             53  blue
```

The grammar in one place:

| Form | Meaning |
|---|---|
| `chrome` | substring over every field |
| `port:443`, `pid:42`, `state:listen`, `process:svchost`, `host:example`, `path:c:\bin`, `service:dnscache`, `proto:udp` | field-restricted match; `pid:`/`port:` accept `a-b` ranges |
| `lport:49600-49700` | a **numeric range**, on the local port only |
| `local:port:49665` | the port, restricted to the **local** side (so it cannot match a connection *to* a remote 49665) |
| `remote:1.2.3.4` | restrict the value to one endpoint |
| `cpu:12`, `mem:100`, `disk:1.5`, `rx:2.0`, `tx:512`, `net:2.5` | live-stat **thresholds**, not text: `cpu:12` is 12 % or more, the rest are MB or more, `K`/`M`/`G` multiply, `mem:100-500` is a range |
| `duration:1h` | threshold in **seconds**, with `s`/`m`/`h`/`d` suffixes and ranges |
| `speed:1KB`, `tls:1.3`, `country:de` | per-connection rate, TLS summary, GeoIP code |
| `note:vendor`, `note:` | the bookmark's note; bare means "has a note" |
| `"a value with spaces"` | quoted, so it is **one** clause — `note:"vendor api"` is one term, not two AND-ed ones |
| `tcp:`, `udp:`, `ipv4:`, `ipv6:` | protocol / address-family prefixes, usable alone or composed |
| `exclude:X` | negate the next term |
| `a b` | AND |

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --filter` | The grammar above | The study *is* the grammar. `netstat` cannot filter at all; PowerShell needs a `Where-Object` chain per predicate and gets the range/prefix composition wrong often enough that people avoid it. |
| `list --sort lport` | Order numerically by the column you filtered | Ranges in a filter and a numeric sort on the same column is the "find the gaps" pattern. |
| `--columns pid,process,lport,local` | The four things a range hunt needs | PID to act, port to see, bind address to know whether it is reachable. |
| `list --limit 8` | Top 8 | — |

Two details worth internalising, because both are places a filter silently
returns the wrong answer:

- **A threshold is a threshold, not a substring.** `mem:100` is "100 MB or
  more". An unknown reading matches **no** threshold at all, so `mem:0` never
  selects a process that simply could not be measured — which is the opposite
  of how a text match behaves.
- **Direction and field are separate concepts.** `local:`/`remote:` select
  which *endpoint*; `lport:`/`rport:`/`local:port:` select which *column*.
  Collapsing them is why `local:port:80` used to parse into
  `{field=Local, text="port:80"}`, which can never match anything.

### Class 7 — persistence, annotation and scripting

> No built-in analog: nothing on Windows stores a filter you reuse, or pins a
> peer with a note and then lets you *find* it again.

**26. Pin a peer with a tag and a note — and then find it again.** The
bookmark's identity is **remote address + remote port**, because that pair
survives a reconnect. The port is part of the match, not decoration: a mark on
`1.2.3.4:9999` does not light up a live `1.2.3.4:443` row.

```bat
wintcp.exe bookmark add   --address 64.59.150.137 --port 53 --tag 3 --note "corporate dns resolver"
wintcp.exe bookmark note   --address 64.59.150.137 --port 53 --note "confirmed with netops"
wintcp.exe bookmark colour --address 64.59.150.137 --port 53 --tag 1
wintcp.exe bookmark list --format json
wintcp.exe list --filter "note:netops" --columns remote,rport,pinned --limit 3
wintcp.exe bookmark remove --address 64.59.150.137 --port 53
```

```text
bookmark added: 64.59.150.137:53
bookmark note updated.
bookmark colour updated.
[
  {"address":"64.59.150.137","port":53,"tag":"Red","note":"confirmed with netops"}
]
Remote address   Remote port  Bookmarks
64.59.150.137             53  red
bookmark removed.
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `bookmark add --address` | The peer address | The bookmark's first half. Addresses normalise — `::ffff:1.2.3.4` becomes `1.2.3.4` — and placeholders like `0.0.0.0` are refused, because you cannot bookmark "somewhere". |
| `bookmark add --port` | The **remote** port | The second half, and the load-bearing one. A host speaks many protocols; a bookmark per (address, port) is what "this conversation" means, and it is why the join matches on both. |
| `bookmark add --tag 0..4` | 0 none, 1 red, 2 amber, 3 blue, 4 green | The colour. It is per endpoint, so every row to that peer shows the same colour — "this conversation is red". The `pinned` column prints the colour name, which is also what sorts first in both directions. |
| `bookmark add --note TEXT` | Free text on the endpoint | The annotation. Stored per-user in HKCU, so it survives restarts. |
| `bookmark note` | Replaces the note on an existing bookmark | Separate from `add` because `add` on an existing endpoint does **not** clobber a note you wrote — losing a note to a re-add is not a recoverable mistake. |
| `bookmark colour` | Changes only the tag | Likewise: the note and the colour are independent, so one verb each and neither can be lost by using the other. |
| `bookmark list --format json` | The store, machine-readable | For pasting into a ticket. Stable keys, and it is the only way to see the port you actually stored. |
| `list --filter "note:…"` | Finds annotated rows | The step that makes a note worth writing. The note is joined onto the row, so `note:` is a real filter field; **quote a multi-word note** (`note:"vendor api"`) or the space is read as AND. |
| `bookmark remove --address --port` | Deletes the bookmark | Undoes the mark, and the `pinned` colour and the note both go with it — the join clears both on every refresh, so nothing stale is left on screen. |

**27. Save and reuse a triage view, and adjust it on the way out.**

```bat
wintcp.exe preset save  --name web --filter "port:443" --sort pid
wintcp.exe preset list
wintcp.exe preset show  --name web
wintcp.exe preset apply --name web --limit 10 --columns pid,process,remote,rport
wintcp.exe preset save  --name web --filter "port:80"          :: exit 3, refuses to overwrite
wintcp.exe preset save  --name web --filter "port:80" --force
wintcp.exe preset delete --name web
```

```text
preset created: web
Preset
web
{"filter":"port:443","sortColumn":6,"sortAsc":true,"grouped":false,"colVisible":4493311,"sources":0}
  PID  Process                      Remote address   Remote port
    0  —                            172.253.117.19            443
 5168  AnyDesk.exe                  107.155.105.90            443
 5172  Avira.Spotlight.Service.exe  20.47.110.73               443
preset 'web' exists: pass --force to overwrite.
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `preset save --name` | Stores a view under a name, in HKCU | Names are the point. A filter you retype is a filter you will get subtly wrong next time. |
| `preset save --filter` / `--sort` | The view being saved | `--sort` takes the same column vocabulary as `list --sort`. The saved state also carries the column mask and the sort direction, which is why `preset show` prints `colVisible` as a number — it is a bitmask. |
| `preset save --force` | Overwrite an existing name | Without it an existing name is **exit 3**. Refusing to clobber a saved view by accident is worth one switch, and the refusal names the fix. |
| `preset list` | All saved views | `table` or `json`. |
| `preset show --name` | The stored view as JSON | Useful for scripting a preset's filter out, and for seeing the bitmask that `--force` will replace. |
| `preset apply --name` | Prints the current table **through** the saved view | The payoff. The same presets appear in the GUI File menu, so a view built on the command line is a view in the window. |
| `preset apply --limit` | Layers a limit over the saved view | `apply` takes the output switches too, and they are layered **over** the preset rather than ignored: `--limit 10 --columns pid,process,remote,rport` is the web view, ten rows, four columns. (They used to be parsed and dropped, so the whole saved table came out in the preset's own columns — the worst kind of silent no-op.) |
| `preset delete --name` | Removes a preset | — |

An applied view that matches nothing exits 1, the same contract `list` has, so
a script can branch on it.

**28. Excel-ready export with column algebra.** `netstat` output cannot be
columned at all, and `ConvertTo-Csv` gives you neither a BOM nor column
control.

```bat
wintcp.exe export --out conns.csv --filter "tcp: state:listen"
wintcp.exe export --out procs.tsv --format tsv --columns proto,pid,process,state
wintcp.exe export --out by-proc.csv --group --traffic --sort nettotal --desc
wintcp.exe export --out snap.csv --quiet
```

```text
exported 49 rows to conns.csv
exported 283 rows to procs.tsv
exported 48 rows to by-proc.csv
:: the --quiet run prints nothing at all
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `export --out FILE` | The destination | Mandatory. Without `--format`, the **extension** picks the format (`.csv`, `.tsv`, `.json`); a name with no known extension is refused with instructions rather than guessed at. |
| `export --filter` | The same grammar as `list --filter` | Exports are views, not dumps. The filter here is what keeps a spreadsheet from being 300 rows of `TIME_WAIT`. |
| `export --columns a,b,c` | The exact column list, in the order you want | The "column algebra". `--columns default\|minimal\|full` are shorthands; a list is the control. Applies to CSV, TSV and JSON alike, so the same expression names the same fields in every format. |
| `export --group` | Export a per-process view | `--traffic` and `--sort` then work on the group, so the file is "one row per process, heaviest first" — a shape `list` can print but a spreadsheet cannot derive. Naming `--columns` here changes which set you get; omitting it picks a group-safe default. Both halves are below, and the restriction is not arbitrary. |
| `export --traffic` | The scan, before sorting | So the exported totals are real, and `--sort nettotal` ranks them. |
| `export --format csv\|tsv\|json` | Overrides the extension | `tsv` is tabs, which is what you want when a column value may contain a comma and you are feeding something that is not Excel. |
| `export --quiet` | Still writes the file, prints **no** confirmation line | `--quiet` means quiet everywhere, including here. The exit code still says whether rows were written, so a scheduled snapshot does not need the line. |

**`--group` changes which columns a file gets.** A grouped row is one
*process*, and some columns describe one *connection* — a remote address, a
hostname, a duration, a country, a TLS session. There is no single value for
them. In a machine-readable output the header row is a **schema claim**:
`Remote address` promises that every cell under it is a remote address, and a
reader has no way to know that one of four connections was picked. So there are
two rules, and they differ because the two cases differ:

* **You name `--columns` yourself** → `export` refuses by name. Substituting a
  different column silently would be worse than refusing: the file would come
  out with no remote column and no error.
* **You name nothing** → the group gets a default set every column of which a
  group can honestly answer: `pid, process, proto, state, traffic, nettotal,
  cpu, mem, disk`. So the third command above wrote a `by-proc.csv` headed:

  ```text
  PID,Process,Proto,State,Traffic (rx/tx),Net total,CPU %,Memory (WS),Disk I/O
  9128,svchost.exe,UDP,30 connections,-,-,0.0 %,10.8 MB,0 B
  3372,svchost.exe,UDP,9 connections,-,-,0.0 %,10.8 MB,5.7 KB
  ```

  Compare the flat `conns.csv` header — `Proto,Local address,Local port,Remote
  address,Remote port,…` — and you can see why the two need different defaults:
  four of the ten flat columns have no meaning for a group.

```bat
> wintcp.exe export --out g.csv --group --columns pid,process,remote
export: --group cannot report Remote address: a group is one process, and that
column describes one connection, so the file's header would promise a value the
rows do not carry.
Drop the column, or drop --group. Columns that DO have a group answer: pid,
process, proto, state, traffic, rx, tx, nettotal, cpu, mem, disk, service,
path, pinned.
:: exit code 2, and no file is written
```

Both halves also apply to `list --format csv|tsv|json` — the rule is about the
output *shape*, not about the verb, so a piped stream cannot carry a header
that lies either. The aligned `list --group` **table** and the GUI are
deliberately exempt: there the row is visibly a group — its State cell reads
"4 connections" — so one member's value is informative rather than misleading,
and blanking it would be strictly less useful. The same column is unsafe in a
file and useful on screen, which is why the rule keys on the format.

Note that `cpu`, `mem` and `disk` **are** allowed with `--group`: they are
per-process values already joined onto every row carrying the PID, so the
group's answer is correct whichever row they come from.

Two details that matter more than they look:

- **CSV and TSV carry a UTF-8 BOM; JSON never does.** Without the BOM, Excel
  on a Western locale mangles a UTF-8 path or a process name; with a BOM in
  JSON, half the parsers reject the file. Both verified byte-wise by
  `golden.bat`.
- **`--limit` is refused, not ignored.** An export always writes the whole
  view, because a file that quietly holds 5 of 300 rows while the tool reports
  success is a lie about the machine. Narrow with `--filter`, or pipe
  `list --limit` output instead.

**29. A frozen JSON schema for dashboards.** Stable lowercase keys, so a
consumer is written once.

```bat
wintcp.exe list --format json --columns proto,local,lport,remote,rport,state,pid,process --limit 2
wintcp.exe list --format json --columns full --limit 1 2>nul
```

```text
[
  {"proto":"TCPv4","local":"10.0.0.92","lport":"9337","remote":"64.59.150.137","rport":"53","state":"TIME_WAIT","pid":"0","process":"—"}
]
```

```text
[
  {"proto":"TCPv4","local":"10.0.0.92","lport":"12924","remote":"104.18.24.129","rport":"443","state":"TIME_WAIT","pid":"0","process":"—","service":"","path":"","host":"","tls":"—","country":"","traffic":"—","bandwidth":"—","duration":"0s"}
]
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --format json` | An array of objects, one per row, valid JSON with no trailing commas | A real array rather than NDJSON, so `jq` and a dashboard can both consume it. A parser that chokes on a trailing comma is the usual reason people fall back to scraping a table. |
| `list --columns a,b,c` | Which keys appear, in which order | The frozen part. Asking for `pid,process` gives exactly those two keys, so the consumer's field list is under your control. |
| `list --limit 2` | Two rows | Paging for a dashboard poll. |
| `2>nul` | Drop stderr | The **advisory** messages (`column: "host" without --dns…`) go to stderr and never into the JSON. That separation is the study: stdout is data, stderr is advice, and a consumer can redirect one without losing the other. |

Types are the schema, and they are chosen for the consumer, not for the
table's convenience: `proto`/`state`/`process` are display strings, `lport`
and `rport` are **strings** so a consumer never has to care that port 0 means
"no port" for UDP, and an unreadable stat is the em-dash `—` rather than `0`
or `null`, because "we could not measure this" and "this is zero" are
different answers. `--columns full` is the widest set; the `changes` feed in
study 33 uses the same convention with numeric `lport`/`rport`/`pid`, so a
consumer of both knows the difference is deliberate.

**30. Quiet rc-only automation** — zero output, exit code is the answer. This
is study 14 seen as a composition: the same contract, wired into a scheduler
or a CI step.

```bat
wintcp.exe list --filter "state:listen exclude:127." --quiet || echo no-exposed-listeners
wintcp.exe list --group --traffic --filter "tx:1GB" --quiet && echo someone-is-uploading-a-lot
wintcp.exe ps --sort mem --limit 1 --quiet || echo no-processes
```

| Switch | What it does | Why it is in this command |
|---|---|---|
| `list --quiet` | Nothing printed; exit 0 iff a row matched | The contract. It is a **zero-byte** contract on both streams — no header, no hints, no progress — so it is safe where output would corrupt a pipeline. |
| `list --filter` | The predicate | Any grammar term, including thresholds. `--traffic` alongside it is required for `tx:`/`rx:`/`net:` to mean anything, and a `note:` on stderr says so when a switch is missing (except under `--quiet`, where the silence is the contract). |
| `list --group` | Per-process, so a threshold hits the process total | `--filter "tx:1GB"` over connections and over processes are different questions; grouping is how you ask the second one. |
| `ps --quiet` | The same contract on the process table | `ps --quiet` exits 0 when any process matched, so `ps --quiet` alone is a "is the process table readable" gate. |
| `\|\|` / `&&` | The branch | The shell is the control flow, which is why the exit-code contract is documented rather than incidental. |

### Class 8 — prove it, trust it

**31. Scale proof for the view pipeline** - 100k synthetic rows through
snapshot, filter, sort and render, with no enumeration and no window.

Run by `wintcp-tests.exe`, the development test driver. `bench` is **not** a
`wintcp.exe` command and this section is not part of the user-facing surface:
it measures the tool rather than the machine, which is a question for whoever
is working on the tool, not for whoever is using it.

```bat
wintcp-tests.exe bench 100000 10
```

```text
WinTCP benchmark: rows=100000 iters=10
  [A] ReplaceSnapshot + SetView (no filter)    260.902 ms/op      0.38 Mrow/s
  [B] SetView (filter tcp port:1000-60000)       21.770 ms/op      4.59 Mrow/s
  [C] SetSort + SetView (toggle direction)       53.179 ms/op      1.88 Mrow/s
  [D] GetColumnText (all columns x rows)         66.998 ms/op      1.49 Mrow/s
  total timed: 4028.5 ms
  (QueryPerformanceCounter; [D] iterates the current view, other stages scale with 'rows')
```

| Argument | What it does | Why |
|---|---|---|
| `bench` | Builds synthetic rows and times the four stages | No enumeration, no window, no network - so it is the one measurement here that reports the *tool* rather than the machine. It answers "would 50k sockets make the list unusable". |
| `bench 100000` | Row count, default 50000, clamped | 100k is comfortably above a busy machine, so the numbers are not flattering by accident. |
| `bench 10` | Iterations per stage, default 20 | Fewer iterations for a quick check; the reported figure is ms/op and Mrow/s, so the two arguments are independent knobs. |

Reading the four lines: **[A]** is the whole cost of a refresh (install a new
snapshot and rebuild the view) and is the one that has to fit in a refresh
interval; **[B]** shows filtering is cheap relative to it, which is why
filter-heavy use is fine; **[C]** is a header click; **[D]** is rendering every
column of every visible row, and it is the cost the GUI pays on paint. The
`Mrow/s` figures are throughput, not latency — the two together are what tell
you whether a machine with 100k endpoints is usable.

**32. Self-check the tool** - no window, no network, no registry writes, so it
runs in CI. It lives in `wintcp-tests.exe`, a separate development binary that
is not distributed; `wintcp.exe` carries no test code, so `wintcp.exe selftest`
is an unknown command. The checks themselves run the **production** code.

```bat
wintcp-tests.exe unit
```

```text
WinTCP selftest
PASS  filter.parse.chained-prefixes  (clauses=3)
PASS  filter.parse.pid-range
PASS  dupe.repeat-snapshot-is-quiet
PASS  rate.second-sample-produces-a-rate
PASS  bm.port-is-part-of-identity
PASS  note.quoted-value-is-one-clause
PASS  cmd.events-mask-appear-only
PASS  about.unknown-rows-omitted
...
selftest: all checks passed
```

```text
472 checks pass, 0 fail.
```

| Mode | What it does | Why |
|---|---|---|
| `unit` | Runs the **production** parser, diff, sort, grouping, filter, formatting and join code against fixed inputs and prints PASS/FAIL per check | The point is that these are the same functions the GUI and the CLI run - a test that exercised a copy would prove nothing. Exit 0 only when every check passes, so it is a CI gate. |
| `ui` | Constructs the real windows and dialogs and drives the real WndProc | Needs an interactive desktop session, which is why it is not part of `unit`. |
| `bench` | Times the view pipeline over synthetic rows | Measures the tool, not the machine. See study 31. |

The 472 checks are not padding. Each one pins a decision that is easy to get
wrong and impossible to eyeball in a list: that 12 identical mDNS sockets
produce **zero** change events on a repeat snapshot; that a per-PID byte total
is refused as a per-connection rate while a per-socket one is accepted; that a
bookmark on port 9999 does not mark a live port 443; that `--event appear`
prints no DISAPPEAR; that a count nobody measured is not printed. Those were
all real defects, and the checks are why they stay fixed.

One caveat worth stating plainly: a check count is not a coverage figure. There
is no coverage instrumentation in this project, so "472 checks" says how much
was *written*, not how much of the code was *exercised*.

**33. A machine-readable change feed.**

```bat
wintcp.exe list --watch 2 --changes --event appear,state --format json --count 5 2>nul
```

```json
{"t":"2026-09-30 14:44:50","event":"appear","proto":"TCPv4","local":"10.0.0.92","lport":6383,"remote":"64.59.144.91","rport":53,"state":"SYN_SENT","pid":28276,"process":"chrome.exe"}
{"t":"2026-09-30 14:44:51","event":"appear","proto":"TCPv4","local":"10.0.0.92","lport":6383,"remote":"64.59.144.91","rport":53,"state":"TIME_WAIT","pid":0,"process":"—"}
{"t":"2026-09-30 14:44:52","event":"state","proto":"TCPv4","local":"10.0.0.92","lport":41723,"remote":"20.47.110.73","rport":443,"state":"CLOSE_WAIT","old_state":"ESTABLISHED","pid":5172,"process":"Avira.Spotlight.Service.exe"}
```

| Switch | What it does | Why it is in this study |
|---|---|---|
| `list --changes` | Deltas between polls instead of snapshots | The feed. |
| `list --format json` | One JSON object **per line** (NDJSON) | Not the array of study 29, and deliberately: an event stream is consumed incrementally, so a line-at-a-time format can be piped into a reader that never buffers the whole thing. |
| `list --event appear,state` | Which kinds to print | The switch that makes the feed usable at scale, as in study 12. |
| `list --watch 2` | Poll interval | Also the **sensitivity**: a socket that opens and closes inside the interval is never seen. Choose it against the shortest event you care about. |
| `list --count 5` | Five snapshots | Bounds it. |
| `2>nul` | Drop the stderr baseline line | The baseline is a human sentence; the data stream must stay pure. |

**Caveat, stated by the tool's own help:** `--count` bounds watch *ticks*, not
emitted events, so a busy machine produces far more lines than 5. Pipe through
a counter if you need exactly N events. Types follow the same convention as
study 29 with one deliberate difference: `lport`, `rport` and `pid` are JSON
**numbers** here (an event is a machine record, and a consumer will compare
them), and a `state` event adds `old_state` so you can see the transition
rather than infer it from two records.

**34. Oldest connections by kernel age — and a real limit, stated.** `netstat`
shows no age at all, and PowerShell still cannot range-filter a `CreationTime`
pipeline.

```bat
wintcp.exe list --traffic --sort duration --desc --limit 4 --columns pid,process,remote,rport,duration
wintcp.exe list --traffic --filter "duration:1h" --sort duration --desc --limit 4 --columns pid,process,remote,rport,duration
```

```text
  PID  Process                      Remote address   Remote port      Duration
 5580  svchost.exe                  172.172.255.216          443        4h 12m
```

```text
  PID  Process                      Remote address   Remote port      Duration
 5580  svchost.exe                  172.172.255.216          443        4h 12m
```

| Switch | What it does | Why it is in this study |
|---|---|---|
| `list --traffic` | **Required** for the `duration` column | The ages come from the kernel (`SIO_TCP_INFO`'s `ConnectionTimeMs`), read by the same socket scan, so a single-shot run already knows them. Without `--traffic` the column reads `0s` for everything — honestly, because there is no age source. |
| `list --sort duration --desc` | Oldest first | Ascending would give youngest-first, which is the useful default for "what just happened" and useless for "what has been squatting on this port". |
| `list --filter "duration:1h"` | Threshold in **seconds**, with `s`/`m`/`h`/`d` suffixes and ranges | Because the age is a real number, it is filterable — and that is the more useful half. Note the asymmetry with the byte fields: a bare `duration:3600` is 3600 **seconds**, while a bare `mem:100` is 100 **MB**. |
| `list --limit 4` | Top 4 | — |
| `--columns pid,process,remote,rport,duration` | The five fields that identify the connection | `pid` and `remote` are needed to act on what you find; `rport` distinguishes two long-lived connections to the same host. |

**One current limitation, so the output above is not oversold:** ages come from
the same socket scan as the traffic columns, and a socket whose `SIO_TCP_INFO`
does not return cannot be measured at all — that is why the table above has one
row and not four. What changed is the blast radius: such a socket now costs
only its own reading, not every socket behind it (see *Limits* under the
traffic study for the measurement). A socket that stalls is remembered and not
retried, so the columns are complete for everything else and read `0s` — not
`—`, because an age of zero is what the kernel reported — for the rest, so the
ranking is real but may not be complete. The GUI reports the count so this is
visible rather than silent. Rows with no age never match `duration:` at all, so
the filter returns fewer rows than the table shows rather than pretending the
unmeasured ones are new.

**35. `ss -i` for Windows — RTT, minimum RTT, congestion window, retransmits.**
No built-in tool shows these. `netstat` has no notion of latency at all,
`Get-NetTCPConnection` has none either, and `pktmon` gives you the packets
without summarising what the stack concluded from them. These four numbers are
the kernel's own verdict on each connection, and they are what you actually want
when a link feels slow: a high RTT with a small congestion window is a *path*
problem, while a small RTT with a large one is a *bandwidth* problem.

```bat
wintcp.exe list --traffic --sort rtt --desc --limit 4 --columns process,remote,rport,rtt,minrtt,cwnd,retrans
wintcp.exe list --traffic --filter "rtt:100" --sort rtt --desc --limit 4 --columns process,remote,rport,rtt,minrtt,cwnd,retrans
wintcp.exe list --traffic --filter "retrans:1KB" --sort retrans --desc --limit 4 --columns process,remote,rport,rtt,minrtt,cwnd,retrans
```

```text
Process                      Remote address   Remote port        RTT    Min RTT          Cwnd       Retrans
chrome.exe                   74.6.160.107             443         30         20       21.2 KB           0 B
brave.exe                    185.199.110.133          443         17         15       16.6 KB           0 B
cline.exe                    127.0.0.1              19536         <1          —       63.8 KB           0 B
winagent.exe                 127.0.0.1              49374         <1          —       18.2 KB           0 B
```

```text
Process                      Remote address   Remote port        RTT    Min RTT          Cwnd       Retrans
chrome.exe                   157.90.91.72             443        219        165       21.3 KB        4.5 KB
Telegram.exe                 149.154.167.92           443        235        171       21.8 KB        2.8 KB
brave.exe                    140.248.153.91           443         25          6       23.8 KB         768 B
chrome.exe                   100.26.11.145            443         83         63       33.3 KB         232 B
```

```text
Process                      Remote address   Remote port        RTT    Min RTT          Cwnd       Retrans
chrome.exe                   157.90.91.72             443        219        165       21.3 KB        4.5 KB
Telegram.exe                 149.154.167.92           443        235        171       21.8 KB        2.8 KB
brave.exe                    140.248.153.91           443         25          6       23.8 KB         768 B
chrome.exe                   100.26.11.145            443         83         63       33.3 KB         232 B
```

Read the third table as the diagnostic it is: the two rows losing the most to
retransmission (`4.5 KB`, `2.8 KB`) also have the **highest** RTTs — 219 ms and
235 ms — while the two rows below them have retransmitted almost nothing and sit
at 25 ms and 83 ms. That correlation is the whole value of the column: those two
are losing packets on the path, and no amount of bandwidth will fix it.

| Switch | What it does | Why it is in this study |
|---|---|---|
| `list --traffic` | **Required** — runs the socket scan | The four values come out of the *same* `SIO_TCP_INFO` call the byte counters and connection age come from. There is no extra scan, no extra handle duplication and no extra cost: the kernel was already filling these fields and the code was discarding them. |
| `list --sort rtt --desc` | Worst latency first | The useful direction. Ascending would put the fastest connection on top, which is the answer to no question anyone asks. Unmeasured rows sort last, so the dashes never crowd out the top of the list. |
| `list --filter "rtt:100"` | Threshold in **milliseconds** | `rtt:100` is "100 ms or worse". It is a number, not a substring of the printed cell, so it cannot be fooled by a process name containing the digits. |
| `list --filter "retrans:1KB"` | Threshold in **bytes** | Retransmits are cumulative and monotonic, so a threshold means "this connection has lost at least this much to retransmission" — usually the single most diagnostic number here, and the one no Windows tool reports. |
| `--columns …,rtt,minrtt,cwnd,retrans` | The four `ss -i` fields | Named for `ss -i` so the two can be read side by side. In the GUI they are one click away in *View ▸ Columns*, hidden by default. |
| `--limit 4` | Top 4 | — |

Three details that matter more than they look:

- **The RTT columns are in milliseconds; the kernel reports microseconds.**
  `TCP_INFO_v0`'s fields are `RttUs` and `MinRttUs` — the only `Ms` field in the
  structure is `ConnectionTimeMs`. The conversion happens once, at the read
  boundary, because `ss -i` prints milliseconds and these columns are named for
  it. Consuming the raw value would report a 24 ms round trip as `24000`, and the
  mistake is invisible in a table because it still looks like a number.
- **A sub-millisecond RTT prints as `<1`, not `0`.** A loopback or same-switch
  round trip is tens of microseconds, which rounds to zero; printing `0` would
  claim a physically impossible zero-latency reading. `ss` prints `<1` for the
  same reason.
- **Zero retransmits is a real answer, not a dash.** "This connection has never
  retransmitted" is the healthiest possible result and the one you most want to
  see at a glance, so it prints `0 B`. Each field is gated on *its own* known
  flag, because the kernel populates them independently — a socket with TCP
  timestamps off still reports a perfectly real congestion window, and blanking
  the whole row for that would throw away three good readings to hide one
  missing one.

**36. Per-process rate — the number you want when asking "what is eating the
link?"** `bandwidth` is one socket's rate. A browser with 40 tabs has 40 rows,
each small; the useful answer is the sum.

```bat
wintcp.exe list --traffic --watch 1 --count 3 --filter "state:estab" --columns process,bandwidth,procspeed --limit 8
wintcp.exe list --traffic --group --sort procspeed --desc --limit 6 --columns pid,process,procspeed
```

```text
Process                      Speed                           Proc Speed
AnyDesk.exe                  idle                            idle
svchost.exe                  idle                            idle
cline.exe                    idle                            idle
```

```text
Process                      Speed                           Proc Speed
AnyDesk.exe                  idle                            idle
Avira.Spotlight.Service.exe  —                               —
svchost.exe                  idle                            idle
wslrelay.exe                 —                               —
tailscaled.exe               —                               —
cline.exe                    idle                            idle
```

```text
  PID  Process                      Proc Speed
 6500  tailscaled.exe               ↓ 0 B/s  ↑ 365 B/s
 20868  brave.exe                    idle
  6436  wslrelay.exe                 idle
  5404  jhi_service.exe             idle
```

Note which rows show `—` in both columns: those are processes whose traffic
came from the ETW path, or whose sockets the scan could not read. A dash in
`Proc Speed` means *unknown*, which is different from the `idle` beside it —
`idle` is a measurement ("nothing moved in that second"), `—` is an absence of
one.

| Switch | What it does | Why it is in this study |
|---|---|---|
| `list --traffic` | **Required** — the socket scan supplies the per-socket counters | Without it nothing is measured and both rate columns are dashes. |
| `--filter "state:estab"` | Established connections only | Not cosmetic: `TIME_WAIT` and wildcard rows have no owning process, so they can never carry a per-socket rate. Including them fills the table with permanent dashes and hides the rows that have answers. |
| `--watch 1 --count 3` | Three snapshots, one second apart | A rate is a *difference*. One snapshot cannot have a rate, so the first tick shows dashes by design and the second shows the answer. This is the same rule the `bandwidth` column follows. |
| `procspeed` | The **process's** bytes/second, summed over its sockets | nethogs parity. Note that it differs from `bandwidth` on exactly the rows you care about: `tailscaled` shows the same figure in both because that socket *is* the process's traffic, while a browser's per-socket figures are a fraction of its total. |
| `list --group` | One row per process | With `--group` the group row *is* a process, so `procspeed` is the column that makes sense there and `bandwidth` deliberately shows `—`. |
| `--sort procspeed --desc` | Busiest process first | Ranked by what the process is doing now, not by its lifetime total. |

**`procspeed` is a sum, and that is a correctness claim, not a style one.** The
obvious shortcut — take the per-PID byte total and divide by the connection count
— invents a number: one process's sockets carry wildly different shares of its
traffic, so the quotient is neither any socket's rate nor the process's, it is the
mean of a distribution. Summing the *per-socket* counters is the only honest
answer available, and it is only correct because each socket is counted exactly
once — which is what the row identity (PID plus endpoint, not endpoint alone)
guarantees. Two sockets on the same endpoint are two sockets.

`procspeed` reports `—` rather than a number for a process whose traffic came
from the ETW source. ETW is per-PID and cannot be split across a process's
sockets, so adding such a row to a sum of that same process's sockets would count
its bytes twice. Unknown is the honest answer; a doubled total is not.

In the GUI, `procspeed` is hidden by default alongside the four `ss -i` columns.
They are diagnostic columns: valuable while chasing one slow connection, and
noise to everyone else.


## Testing

The self-test, the benchmark and the GUI checks are **not** in `wintcp.exe`.
They live in `wintcp-tests.exe`, a development-only binary that is not
distributed. It links the *same* product sources as `wintcp.exe`, so a check
exercises the functions the GUI and the CLI actually run - a test against a copy
would prove nothing.

`build.bat` produces both binaries:

```bat
build	ests\wintcp-tests.exe unit              :: 472 internal checks; exit 0 only if all pass
build	ests\wintcp-tests.exe ui                :: 46 GUI checks (needs a desktop session)
build	ests\wintcp-tests.exe bench 100000 10   :: time the view pipeline
```

Four gate scripts, all runnable from a Developer Command Prompt:

| Script | What it asserts |
|---|---|
| `golden.bat` | CLI behaviour: 160 checks against the real `wintcp.exe`, including exit codes and refusal messages |
| `uigolden.bat` | the GUI harness; fails loudly if the harness cannot start, rather than reporting a vacuous pass |
| `readmegolden.bat` | every command shown in this README is accepted by the binary |
| `wintcp-tests.exe unit` | parser, snapshot diff, sort, grouping, filters, formatting, joins, GeoIP, TLS decode, reassembly, capability report |

`readmegolden.bat` and `uigolden.bat` both begin with a **self-test**: each
deliberately provokes a known rejection and confirms the harness notices, so a
green run means the gate can still fail. Never run two instances of one script
at once - stdout interleaves.

`tools\` holds the measurement helpers used while developing: `pe-size.ps1`
decodes a PE's sections and import table, and `size-matrix.ps1` builds flag
variants and compares their sizes.

## Building

Requirements: Visual Studio 2022/18 with C++ workload (MSVC + Windows 10/11
SDK). No admin rights, no package manager, no third-party dependencies.

```bat
:: primary build (Developer Command Prompt, or it will self-locate vcvars):
build.bat          :: -> build\wintcp.exe   (/std:c++17 /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
build.bat clean    :: remove build\ first

:: alternatives:
cmake -S . -B build-cmake && cmake --build build-cmake --config Release
:: or open wintcp\wintcp.vcxproj in Visual Studio
```

All three build paths compile at `/W4 /WX` (warnings are errors) with
`/permissive-` and `/Zc:__cplusplus`, so a warning or a non-conforming
construct fails the build rather than accumulating. `/guard:cf` is enabled
for the CMake and Visual Studio paths. `/analyze` is deliberately *not* part
of the default build — it is far too slow to gate every compile; run it
ad hoc with `/analyze` on the translation unit you are changing.

The app manifest (ComCtl32 v6 + PerMonitorV2 DPI) is embedded via
`wintcp\res\wintcp.manifest` in the `.rc` — all build paths disable the
linker's auto-manifest to avoid duplicates.

## Project layout

```text
./
  README.md              <- this file
  build.bat              <- primary build script (cl + rc + link)
  CMakeLists.txt         <- CMake alternative
  wintcp/
    wintcp.vcxproj(+.filters)  <- Visual Studio project
    res/                 <- manifest, icon
    src/
      main.cpp           <- wmain, CLI dispatch, message loop
      MainWindow.h/.cpp  <- window, controls, menus, list, tray, theming
      Connection.h       <- row struct + highlight flags
      Columns.h          <- column set + masks (shared with Settings)
      Version.h          <- single source of truth for the version
      EtwTrafficTypes.h  <- ETW GUIDs + pure event classifier
      ConnectionStore.h/.cpp <- model: stable-id diff, filter, sort, columns
      RefreshEngine.h/.cpp   <- background enumerate+resolve worker (+ traffic scan)
      DnsResolver.h/.cpp     <- background reverse-DNS worker
      EtwTraffic.h/.cpp      <- ETW kernel-logger traffic counters (elevated)
      SocketTraffic.h/.cpp   <- non-admin per-PID fallback over SIO_TCP_INFO
      TcpTable.h/.cpp        <- IP Helper table enumeration (+scope IDs)
      ProcessInfo.h/.cpp     <- process/service cache (Toolhelp, OpenProcess, SCM)
      Settings.h/.cpp        <- HKCU persistence
      DetailsDialog.h/.cpp   <- details modeless window
      ChartsWindow.h/.cpp    <- performance graphs window
      ProcStats.h/.cpp       <- per-process CPU/mem/IO sampler
      Commands.h/.cpp        <- abstract command layer (headless core)
      SysStats.h/.cpp        <- headless system CPU/mem/disk/net sampler
      Cli.h/.cpp             <- CLI entry point and single-mode dispatch
      CliCommands.h/.cpp     <- subcommand verbs (list/ps/top/stat/...)
      tests/Bench.h/.cpp      <- selftest / bench / ui harness
      Utils.h/.cpp           <- UTF-8, formatting, DPI, error text helpers
      resource.h, wintcp.rc  <- menu/accel/manifest/icon/version
```

## Design notes

- **Zero dependencies**: every helper (RAII handles, UTF-8 conversion,
  `FormatSystemError`, …) is in-tree; ETW comes straight from
  `evntrace.h`/`evntcons.h`.
- **Payload registry**: worker results are heap payloads registered under a
  lock before `PostMessage`; the receiver erases on receipt (a missing
  entry means foreign/late → not touched), and shutdown stops the workers
  before freeing anything still queued.
- **Diffing**: rows are keyed by **endpoint + PID** (state excluded, so
  `ESTABLISHED → TIME_WAIT` stays one row and produces a STATE event), ids
  are stable, and one-cycle ghosts give you a visual last-chance to see what
  closed. Duplicate keys pair up **in order** rather than collapsing: the
  4-tuple is not unique on a real machine — mDNS makes dozens of identical
  rows the normal case — and a single-index lookup turns each refresh into
  N−1 false closes followed by N "new" rows, which is what made the change
  feed useless exactly where the table is busiest.
- **A silent answer is the worst answer.** Three rules follow from this and
  are enforced in code and by `wintcp-tests.exe unit`, because each was a real defect:
  a stat reading that could not be taken matches **no** threshold (so `mem:0`
  never selects an unmeasured process); a byte total that cannot be
  attributed to one socket is **never** split across a process's connections
  to fake a rate; and a count nobody measured (`Columns shown`, `Connections`
  in `version`) is **omitted** rather than printed as a confident `0`.
- **A swallowed switch is worse than a wrong one.** A misspelled or
  misplaced switch used to be accepted and dropped, giving a script a
  successful run and the wrong answer. Every verb now declares which
  switches it acts on and refuses the rest with exit 2, naming both.
- **Self-test over test framework**: `wintcp-tests.exe unit` exercises the
  real production parser/diff/sort/filter/formatting/join code paths with
  fixed inputs and a pass/fail report — no framework dependency, CI-friendly
  exit code. The interesting checks are the ones for behaviour that is
  impossible to eyeball in a list: 12 identical sockets producing zero
  change events, a per-PID total refused as a per-connection rate, a
  bookmark on the wrong port not marking the right one.

## License / provenance

Original WinTCP sources. A from-scratch native Win32 application with no
third-party dependencies.
