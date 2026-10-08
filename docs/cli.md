# CLI reference

← [Back to the README](../README.md)

`wintcp.exe` is both the GUI and the command-line tool. Started with no arguments it opens the window; started with a command it behaves like a conventional console utility.

`wintcp.exe help` prints the overview, and `wintcp.exe help <command>` (or `<command> --help`) prints full per-command help with examples and the exact list of switches that command honours. This page is the long-form companion to that help.

## Contents

- [Model](#model)
- [Commands](#commands)
- [Exit codes](#exit-codes)
- [Strict switch handling](#strict-switch-handling)
- [`list` switch reference](#list-switch-reference)
- [Switch dependencies](#switch-dependencies)
- [Column reference](#column-reference)
- [Output formats and streams](#output-formats-and-streams)
- [Per-command notes](#per-command-notes)
- [Migrating from earlier builds](#migrating-from-earlier-builds)

## Model

- **One command, one snapshot, then exit.** Nothing waits for a keypress.
- **Only `--watch` polls.** `--count N` bounds it, which is what makes it safe in a script. `--interval N` is an alias for `--watch N` (same 1..3600 range). Without `--count` the loop runs until Ctrl+C; a redirected stdout is warned once on stderr.
- **Data on stdout, advice on stderr.** A consumer can redirect either without losing the other.
- **Exit codes are part of the interface.** See below.

```text
Usage: wintcp.exe <command> [switches]
       wintcp.exe help <command>     full help + examples for one command
       wintcp.exe <command> --help   same as above
       wintcp.exe --help | -h        this overview
```

## Commands

| Group | Command | Description |
|---|---|---|
| Monitor | `list` (alias `conn`) | Connections table: filter, sort, group, choose columns. The widest verb; the others take subsets of its switches. |
| | `ps` | One row per process: connection count, CPU, memory, disk. |
| | `top` | Synonym for `ps`: same rows, same switches, same defaults. Not interactive: prints once and exits; use `list --watch` for live. |
| | `stat` (alias `sys`) | System CPU, memory, disk and network rates. |
| | `details` | Full report for exactly one connection. |
| Act | `kill` | End the process that owns a connection, or a PID. |
| | `close` | Tear down one TCP connection; the process survives. Admin, IPv4. |
| | `block` | Close a live connection **and** add an outbound firewall rule for its remote endpoint. Admin. |
| | `unblock` | Remove the rules `block` created for an address and port. Admin. |
| | `capture` (alias `follow`) | Record and reassemble one TCP stream for a fixed window. Admin. |
| Audit | `blocks` | Count firewall rules carrying the WinTCP tag. Read-only; exits `0`; needs no elevation and no `--yes`. If the ledger at `%APPDATA%\WinTCP\blocked.txt` cannot be read with confidence the count is **not** trustworthy and stderr says why — see [recipe 3](cookbook.md#3-block-a-peer-audit-the-rules-undo-them). |
| Library | `bookmark` | Pin a remote endpoint with a color and a note. |
| | `preset` | Save, show, apply and delete named views. |
| | `export` | Write a view to CSV, TSV or JSON. |
| | `geoip` | Inspect a `.mmdb` database and look up addresses. |
| Other | `version` | Report what is available on this system, and why anything is not. |
| | `doctor` | Environment diagnostics for this run: elevation, handle/GDI/USER counts, traffic source, GeoIP, firewall rules and capture availability. Exits `0`; degraded capability is reported as text. `--format json` for scripts. |
| | `help` | Overview or per-command help. |

The act verbs (`kill`, `close`, `block`, `unblock`, `capture`) require `--yes` and support `--dry-run`. `wintcp.exe help` lists `blocks` alongside them; unlike the others it only reads.

## Exit codes

| Code | Meaning | Examples |
|---|---|---|
| `0` | Success. | A command ran; a `--quiet` filter matched; a `--dry-run` printed its plan. |
| `1` | Failure or empty result. | No row matched; a `--select` matched 0 or more than 1 rows; `list --quiet` found nothing; `capture` or `close` was run unelevated. |
| `2` | Bad arguments. | Unknown or misplaced switch; `kill --pid 4` (a refused target); `export --group --columns …,remote`; `list --quiet --filter country:x` without `--db`. |
| `3` | Refused. | A mutating command without `--yes`; `preset save` over an existing name without `--force`. |

Exit `2` and exit `3` differ on purpose. `2` means the command line was wrong; `3` means it was well-formed but not confirmed. A script can branch on whether an action was *refused* as opposed to *failed*.

```bat
> wintcp.exe kill --pid 999999
kill PID 999999: refused: pass --yes to proceed (or --dry-run to preview).
:: exit code 3

> wintcp.exe kill --pid 999999 --dry-run
kill PID 999999
:: exit code 0

> wintcp.exe kill --pid 4
kill: refusing PID 4: That row has no process that can be ended. PID 0 and the System pseudo-process do not own a real image.
:: exit code 2 — a bad ARGUMENT, not a missing permission
```

## Strict switch handling

A switch that a command does not use is an error that names both the switch and the command:

```bat
> wintcp.exe export --out snap.csv --changes
export: --changes is not a switch of this command; it would be ignored.
Try 'wintcp.exe help export'.
export honours: --filter --sort --asc --desc --group --format --columns --quiet --traffic --dns --db --out --force.
:: exit code 2
```

This is deliberate. A silently swallowed switch gives a script a **successful** run and the wrong answer, with nothing in the exit code to say so. `wintcp.exe help <command>` lists exactly which switches that command honours.

## `list` switch reference

```text
--filter F    filter grammar: chrome, port:443, pid:1000-2000, state:estab,
              process:svchost, proto:udp, remote:1.2.3.4, note:"vendor api",
              cpu:12, rx:2.0. Space = AND, exclude:X negates, "quoted value"
              keeps a space inside one value. Run `wintcp help filters` for the
              full keyword list, and `wintcp help columns` for every column.
--sort COL    sort column (default pid). --desc reverses.
--group       one row per process instead of per connection.
--format S    table (default) = aligned columns, header printed first, rows
              as soon as they are ready; csv / tsv = raw delimiters (tsv =
              tabs) for scripts; json = objects in one array; jsonl = the
              same objects, one per line (NDJSON), so a --watch stream can
              be consumed as it runs instead of buffered to the closing
              bracket.
--columns C   default | minimal | full (wide = full), or a comma-separated list.
               Run `wintcp help columns` for the authoritative name list, header
               and which enrichment source fills each column.
--limit N     at most N rows.
--quiet       print nothing; exit 0 when any row matches, else 1. An
              enrichment filter without its source switch (--dns, --db,
              --traffic, --signatures) exits 2 instead: quiet would turn
              "unanswerable" into "no match".
--out FILE    write the table to FILE instead of stdout. csv/tsv carry
              the same BOM export uses; json/jsonl never do; an existing
              file needs --force. One snapshot: --out refuses --watch,
              --changes and --quiet (exit 2, naming the conflict).
--traffic     per-PID byte totals via one bounded socket scan.
--dns         reverse-DNS the printed rows only (slow; bound with --limit).
--dns-timeout MS  per-lookup budget (ms) for --dns. A lookup that exceeds it
              shows `host: pending` instead of stalling the run; the count of
              pending lookups is written to stderr. 1..60000; default 3000.
              The GUI applies the same budget automatically; this switch is
              the CLI binding of that per-tick ceiling.
--db FILE     load this .mmdb and join country codes for printed rows.
--asn-db FILE  load this GeoLite2-ASN .mmdb and join autonomous systems. A
               SEPARATE file from --db: the two databases have different record
               shapes, so one --db cannot supply both. Either, both or neither.
--signatures  verify each distinct process image with WinVerifyTrust so the
              signature column and signed: filter have an answer. SLOW: a
              certificate chain per image, cached per image path for the run.
              Revocation is NOT checked, so a revoked certificate can still
              read Signed.
--changes     with --watch/--count: print only APPEAR / DISAPPEAR / STATE
              deltas between polls. The first snapshot is the baseline.
--event LIST  with --changes: appear,disappear,state (comma separated, any
              order, case insensitive). Default: all three.
--watch [s]   re-print every s seconds (default 2, range 1..3600).
--count N     stop after N snapshots (bounds --watch for scripts).
```

Notes:

- `--watch` re-prints the table; with `--changes` it prints only deltas. The interval **is** the sensitivity: at 1 s you catch short-lived sockets, at 10 s you miss them.
- Closed sockets linger as **grey ghost rows** (F5.7): after a `DISAPPEAR` they stay in the table for up to 500 sockets (oldest dropped first) instead of vanishing on the next poll, so a `--watch` stream keeps a short history of what just closed. The change feed is unaffected - `DISAPPEAR` still fires exactly once per socket - only the retained row table holds them.
- `--count` counts **snapshots, not events**. On a busy machine four snapshots can emit hundreds of lines; pipe through `head` if you need exactly N.
- `--watch` without `--count` runs until Ctrl+C — right for a terminal, a trap for a redirected script. When stdout is **not** a console, a one-line warning is printed on stderr; pass `--count` in scripts.
- `--event` takes a comma-separated list. An unrecognised name exits `2`; it is never silently dropped.
- `--sort` takes the column names in the [column reference](#column-reference). `ps` uses its own six keys (see [`ps` and `top`](#ps-and-top)).

## Switch dependencies

Some columns and filters need the switch that supplies their data. When it is missing, the column is empty, and a `note:` on stderr names the switch (suppressed under `--quiet`).

| You want | Add | Why |
|---|---|---|
| `host` column, `host:` filter | `--dns` | Reverse DNS runs only on the printed rows. It is slow; bound it with `--limit`. |
| `country` column, `country:` filter | `--db FILE` | A memory-mapped lookup; no network. |
| `country` column's ASN half, `asn:` filter | `--asn-db FILE` | A second memory-mapped lookup, against GeoLite2-ASN. Separate from `--db` because the two are different files. |
| `traffic`, `rx`, `tx`, `nettotal` | `--traffic` | One bounded socket scan, joined per PID. |
| `duration` | `--traffic` | Ages come from the kernel's `ConnectionTimeMs` in the same scan. Without the scan the column reads `0s`. |
| `bandwidth` (per-socket rate) | `--traffic` and `--watch N` | A rate is the difference between two samples. The first tick shows `—` by design. |
| `procspeed` (per-process rate) | `--traffic` and `--watch N` | Sum of that process's per-socket rates. |
| `rtt`, `minrtt`, `cwnd`, `retrans` | `--traffic` | Same `SIO_TCP_INFO` call as the byte counters. |
| `signature` column, `signed:` / `signature:` filters | `--signatures` | A certificate chain per **distinct binary**, not per row — the resolver caches by image path, so twenty processes off one DLL pay for it once. Revocation is not checked. |
| `ppid` column, `ppid:` / `parent:` filters | *(nothing)* | `CreateToolhelp32Snapshot`, already taken once per refresh for process names. |
| `integrity` column, `integrity:` filter | *(nothing)* | `OpenProcessToken` with `TOKEN_QUERY`, read on the handle the resolver already opens. Reads `—` for a process this shell cannot open. |

## Column reference
There are exactly **32 columns and that is a deliberate freeze**, not a limit that
ran into. The persisted visible-set mask is one bit per column in a `UINT32`, and
it is already full; `architecture.md` records why widening it is an eight-part
change with a schema migration, and why a feature that needs a 33rd column should
reuse an existing one rather than grow the mask as a side effect. A
`static_assert` in `ColumnsWin.h` fails the build at 33 so that a new column
cannot arrive unnoticed.

**ASN reuses `country`, and now does so.** F5.4 was the live case that decision
was recorded for. The `country` cell renders the two together - `US \xB7 AS15169
Google LLC` - with the separator appearing only when both are known, so a
country-only row reads exactly as it always did. The AS number is placed *before*
the organisation name so that a narrow window, which ellipsises on overflow, can
never hide which network a row belongs to; `details` and the machine-readable
formats always carry both in full. There is no 33rd column and no 33rd bit.

Use `--columns default`, `minimal` or `full` (alias `wide`), or a comma-separated list. The same names are used by `list`, `export` and `--sort`. Run `wintcp.exe help list` for the authoritative list for your build.

| Group | Names |
|---|---|
| Connection | `proto`, `local`, `lport`, `remote`, `rport`, `state`, `pid` |
| Process | `process`, `service`, `path`, `ppid` (parent, as *`<pid> <name>`*) |
| Process trust | `integrity` (mandatory level, *`+AC`* for AppContainer), `signature` (Authenticode verdict; needs `--signatures`) |
| Live, per process | `traffic`, `rx`, `tx`, `nettotal`, `cpu`, `mem`, `disk`, `procspeed` |
| Live, per connection | `duration`, `bandwidth` (header *Speed*), `rtt`, `minrtt`, `cwnd`, `retrans` |
| Present but never populated | `tls` - see the note below |
| Enrichment | `host` (needs `--dns`), `country` (needs `--db`, and shows the ASN too when `--asn-db` is given), `pinned` (bookmark color; header *bookmarks*), `note` (the bookmark's text; header *Note*) |

> **The `tls` column is inert.** It is accepted, it sorts, it is rendered and it is unit-tested, but **nothing populates it**, so every row shows `-`. Measured on one Windows 11 host: **299 of 299** rows carried the unknown marker, including **62 of 62** established connections, while `host` over those same 62 rows filled **31** - so the enrichment pipeline works and `tls` alone has no producer. There is no cheap way to fill it: Windows has no socket-level TLS ioctl, and `TCP_INFO_v0` - the only socket info a separate process can read - carries no TLS fields at all. The two real sources both need elevation, and the capture-based one can only ever cover the connections you choose to capture, so it cannot fill a column across every row. Treat `tls` as a reserved column rather than a working one. The reasoning is recorded at the top of `Connection.h`.

Value conventions:

- **Unknown is `—`, never `0`.** "We could not measure this" and "this is zero" are different answers, and unknown values always sort last, in both directions.
- `rtt` and `minrtt` are in **milliseconds**. The kernel reports microseconds; the conversion happens once, at the read boundary. A sub-millisecond RTT prints `<1`, never `0`.
- `retrans` prints `0 B` for a connection that has genuinely never retransmitted. That is a real answer, not a missing one.
- `ppid` prints `<pid> <parent name>`, or just the number when the parent was not in the snapshot. An **unknown** parent prints `—`: the snapshot not covering a parent is not the same as a process having no parent, and the two are not collapsed.
- `host` prints the PTR name, or `—` when there is no name / the lookup failed. With `--dns-timeout` set, a lookup still in flight prints `pending` instead of `—` so a slow resolver is distinguishable from a missing one; the count of such stalls is one line on stderr.
- `integrity` prints the mandatory level as a word, never the RID: `12288` answers no question a reader has, `High` answers "could this have written to HKLM".
- `cwnd` is the kernel's congestion window in bytes. Each `rtt`/`minrtt`/`cwnd`/`retrans` field is gated on its own known flag, so a socket with TCP timestamps off still shows its real congestion window.
- UDP rows print `*:*` for the remote and `—` for the state, as `netstat` does. `proto` prints `UDPv4` / `UDPv6` rather than a generic `UDP`.
- Link-local IPv6 addresses carry their scope ID (`fe80::1%12`); IPv4-mapped IPv6 (`::ffff:1.2.3.4`) is normalised to `1.2.3.4`.

### Grouped views

`--group` makes one row per **process**. Some columns describe a single connection (remote address, hostname, duration, country, TLS) and have no honest per-process value. In machine-readable output a header is a schema claim, so these rules apply to `export` and to `list --format csv|tsv|json`:

- Name a per-connection column yourself with `--group` and the command **refuses** (exit `2`), naming the column. It does not substitute another.
- Name no columns and you get a group-safe default: `pid, process, proto, state, traffic, nettotal, cpu, mem, disk`.
- Columns that do have a group answer: `pid, process, proto, state, traffic, rx, tx, nettotal, cpu, mem, disk, service, path, pinned`. (`cpu`, `mem` and `disk` are per-process values already joined onto every row.)

The aligned `list --group` **table** and the GUI are exempt: there the row is visibly a group (its State cell reads "4 connections"), so one member's value is informative rather than misleading.

## Output formats and streams

| Format | Shape |
|---|---|
| `table` (default) | Aligned columns, header first, rows printed as soon as they are ready. |
| `csv`, `tsv` | Raw delimiters for scripts (`tsv` uses tabs). |
| `json` | A real **array** of objects, valid with no trailing commas, so both `jq` and a dashboard can consume it. |
| `jsonl` | The same objects as `json`, **one per line** (NDJSON) and no array wrapper, so a `--watch` stream can be consumed as it runs instead of after the closing bracket. |
| `json` or `jsonl` with `--changes` | **NDJSON** either way: a change feed is already a stream. |

`jsonl` is `json` with a different renderer, **not a fourth format**. Everything that is true of `json` is true of `jsonl`: the same keys, the same string-valued ports, the same refusal of a per-connection column under `--group`, and the same `--changes` event shape. Only the framing differs — one array, or one object per line.

**stdout is data; stderr is advice.** Missing-switch notes, the `baseline: N rows …` line of a change feed, and hints all go to stderr. Redirect it (`2>nul`) and stdout stays pure.

### JSON conventions

- Keys are stable, lowercase column names, and `--columns` decides which keys appear and in what order.
- In `list --format json`, values are display strings: `lport` and `rport` are **strings**, so a consumer never has to treat port `0` ("no port" for UDP) specially. An unreadable stat is `—`, not `0` or `null`.
- In the `--changes` NDJSON feed, `lport`, `rport` and `pid` are **numbers** (an event is a machine record a consumer will compare), and a `state` event carries `old_state` so the transition is visible without joining two records.
- Every `stat --format json` reading has a `*Known` flag. `"diskKnown":false` with `diskReadBps:0` is a different statement from a disk that genuinely read nothing.

```text
{"cpu":7.6,"cpuKnown":true,"memUsed":28333527040,"memTotal":68422742016,"memPct":41.4,"memKnown":true,"diskReadBps":0,"diskWriteBps":364468,"diskKnown":true,"netRecvBps":18016,"netSendBps":9241,"netKnown":true}
```

## Per-command notes

### `ps` and `top`

One row per process: PID, name, connection count, CPU %, working set, disk I/O. `top` is a **synonym for `ps`** - one code path, so the rows, the switches and the defaults are identical; both already sort by CPU, hottest first. `top` exists so that muscle memory from the Unix command lands somewhere useful, and `ps` is the spelling the rest of the documentation uses.

`ps --sort` takes its own vocabulary: `cpu`, `mem`, `disk`, `conns`, `pid`, `process`. It has no `--group`; it is already grouped. `--filter` selects **connection rows**, and `ps` then aggregates the processes that own them.

A CPU reading that cannot be taken (a protected process without elevation) shows `—` and sorts last in both directions.

### `stat`

Samples system CPU, memory, disk and network rates once and exits. Rates need two reads, so each run sleeps about one second internally. It prints one aligned line (`dir` behaviour, not `top` behaviour) or one JSON object with `--format json`. `--watch` and `--count` are available.

### `details`

A sectioned dossier for **exactly one** connection: process identity, command line, start
time, service, the process's threads, live CPU/memory/disk/network statistics, the selected
connection, and every other endpoint owned by the same PID. `--traffic` fills the network
lines and rate, `--dns` fills the hostname, `--db` fills the country. Ambiguity and
no-match are both exit `1`.

The **Threads** section lists every thread the process owns, ranked by CPU time, with
each thread's lifetime CPU, age and base priority. Threads whose times cannot be read
show `-` and sort last rather than showing `0`; and the note under the list states that
the CPU figure is total since each thread started, not since the command ran.

There is **no per-connection thread** to report, because Windows has no such thing: a
socket is owned by the process, not a thread, and `MIB_TCPROW_OWNER_PID` carries no
thread id. See [the GUI guide](gui.md#threads) for the full reasoning.

### `kill`

`--select SEL` resolves to one live row and ends the process that owns it; `--pid N` names a PID directly. Before acting the PID is re-validated against the process creation time, so a recycled PID is refused. PID 0 and PID 4 are always refused (exit `2`).

The default end-mode is the hybrid: `WM_CLOSE`, then terminate if the process is still alive after 3 s. `--close` asks and waits the same 3 s but **never forces** — a survivor is exit `1` with the reason; `--force` terminates immediately with no `WM_CLOSE` at all. The two switches are mutually exclusive (together, exit `2`), and a `--dry-run` plan names the mode it would use.

### `close`

`--select SEL` tears down a single TCP connection and leaves the process running. Needs administrator rights and supports IPv4 only: `SetTcpEntry` has no IPv6 form, so an IPv6 row is refused with a message rather than silently skipped.

### `block`, `unblock`, `blocks`

`block --select SEL` takes the live connection down **and** writes an outbound firewall rule derived from the row's *remote* endpoint, so you name the conversation you saw and never hand-type an address. Rules carry a WinTCP tag.

`unblock --address A --port P` removes the rules for that peer. It cannot take `--select`: by the time you want to undo, the connection is usually gone, so there is no row to select. Address and port are the rule's identity.

`blocks` counts rules carrying the WinTCP tag, in any direction and family.

### `capture`

`capture --select SEL --secs N --yes` records one TCP stream for a fixed window (`--secs` 1 to 60, default 5), then converts and reassembles it. The run is `N seconds + convert + parse` with no interaction.

- The plan and the `--yes` gate are evaluated **before** the elevation check, so `--dry-run` and refusals never demand elevation. Unelevated runs exit `1` and never attempt to elevate.
- A selector that would match more than one stream is refused, not narrowed.
- The packet-size limit is disabled in the underlying `pktmon` run, because truncated packets cannot be reassembled. The capture filter is always removed afterward, even on failure.
- Output: `packets=…` (total), `toServer` and `toClient` (the reassembly result in each direction), and `blocks` (the number of TCP segments parsed).

`pktmon` is a **sampling driver, not a tap**: only what happens after the filter is armed is recorded. A connection that has gone quiet during the window legitimately returns nothing, and an empty result is not evidence of a fault. Generate traffic during the window, or widen `--secs`.

#### What capture needs, and which reason you get

Capture needs **two** tools in the system directory, not one: `pktmon.exe` to record the ETL and `etl2pcap.exe` to convert it into the pcapng the parser reads. Both are checked **before** the token, and the tool's absence is what gets reported when both would refuse.

That order is deliberate. The alternative - asking the token first - produces "run as administrator" for a user whose machine has no `pktmon` at all, and that advice cannot help, because elevating does not install a tool. The refusal you get therefore names the thing that is actually missing.

The two questions are also reported separately, because they mean different things:

| Question | Answered by | Meaning |
| --- | --- | --- |
| Can this machine capture? | `Stream capture (pktmon)` row in `about` and `stat` | A property of the OS install. Missing means the exes are not there. |
| Can this process capture right now? | The refusal on a `capture` run | A permission. "Not right now" - the caller can offer a relaunch. See [elevation](#elevation). |

A standard user on a machine that has both tools gets the *elevation* reason, not the tool reason. That asymmetry is the point: the token is a permission the user may be able to change, the tool is not.

`etl2pcap.exe` is the one that is easy to miss. When it is absent the failure used to arrive late - the capture ran, waited out its window, wrote the ETL, and was then discarded at the conversion step. It is now reported up front, as a capability row, before any capture is attempted.

`pktmon` is a **sampling driver, not a tap**: only what happens after the filter is armed is recorded. A connection that has gone quiet during the window legitimately returns nothing, and an empty result is not evidence of a fault. Generate traffic during the window, or widen `--secs`.

#### Reading the TLS handshake

Every capture also prints `tls:` lines describing the TLS session, parsed out of the bytes it just reassembled. This is the **only** place WinTCP can report a TLS session: Windows has no socket-level TLS ioctl (there is no `SIO_TLS_INFO` in the SDK, and `TCP_INFO_v0` carries no TLS field), so the handshake is the only source, and it is visible only because TLS sends it in the clear. The `tls` **column** therefore stays empty - see its note in the [column reference](#column-reference).

What appears depends entirely on where the capture window fell relative to the handshake, and every case is stated rather than left to inference:

| Case | What is printed |
|---|---|
| The window includes the handshake | `ClientHello:` with SNI, the offered ALPN and the offered version; `ServerHello:` with the negotiated version and cipher; `certificate:` with subject, issuer and validity. |
| The stream starts mid-record | `does not begin with a TLS record, but N contiguous TLS records start at offset X: this is TLS whose records began before the capture window`. |
| The stream is TLS but carries no handshake message | `begins with TLS records but no handshake message was captured (the window opened after the handshake)`, plus the number of `application_data` bytes seen. |
| The stream is not TLS | `does not begin with a TLS record, and no run of contiguous TLS records appears anywhere in it`. |

- **A direction that captured no bytes is named, not omitted.** "One side of this connection has no bytes" is otherwise indistinguishable from "we only looked one way".
- **`not TLS` is only claimed when neither direction begins with a record *and* neither holds a run of them.** One direction holding a contiguous run is positive evidence of TLS, and it outranks the other direction's silence.
- The test for a run is **contiguity, not a header count**. Random bytes resemble a record header often enough to matter: on one real 131,072-byte mid-stream TLS capture, 13 offsets passed a header test and 5 of those were coincidence. A real record layer declares each record's length, so consecutive headers land at exact offsets, and coincidence cannot fake that.
- **To see an SNI, capture a connection that is being established** - not one that has been up. `pktmon` records only what happens after the filter is armed, so on an established connection the handshake is simply not there to find.
- Directions are named by endpoint, for the reason given under [Reading the stream](#reading-the-stream).

#### Reading the stream

| Switch | Behavior |
|---|---|
| `--text` | Print the reassembled stream to stdout as a hex dump, one block per direction. This is the command-line equivalent of the GUI's *Follow TCP stream*, which was removed on 2026-10-05. |
| `--dir both\|first\|second` | Which direction `--text` prints. `first`/`a` and `second`/`b` are accepted, as are the `server`/`client` spellings. Default `both`. An unrecognised value is refused, never treated as `both`. |
| `--out FILE` | Save the capture as **pcapng**, byte-for-byte, so it opens in Wireshark or `tshark` with no conversion by the reader. An existing file there is refused (exit `2`) unless `--force`. |
| `--bin` | Instead of the hex dump, write the **raw reassembled stream bytes** to `--out FILE`, with no hex or pcapng framing. Requires `--out`; mutually exclusive with `--text`. With `--dir both` (the default) the two directions are concatenated, `first` writes only to-server bytes, `second` only to-client. An existing file there is refused (exit `2`) unless `--force`. |
| `--force` | Authorise replacing an existing `--out` file. Checked **before** the capture window opens, so `--dry-run` reports a taken destination. |
| `--flags none\|syn\|fin\|rst\|all\|N` | Pass a `--flags` bitmask to `pktmon start`, selecting which TCP lifecycle events to record (`syn=1`, `fin=2`, `rst=4`). `all` is `7`. Default `none` emits no flag and records everything — the only mode that fully reassembles a stream. Narrowing the set lightens the capture but can prevent reassembly from completing. |
| `--filter IP` | Add a second address to the `pktmon filter add` line (`-i`), so the capture records an additional IPv4 or IPv6 address alongside the selected connection's ports. Must be a bare IP address — a non-IP value exits `2` before the snapshot runs. The extra address appears in `--dry-run`'s plan. |

- **Each direction is labelled by the endpoint that sent it**, not by `client`/`server`. `pktmon` does not reliably report which end sent the SYN, so those words would be a claim the capture cannot support. The labels are derived from the same endpoint ordering the reassembler used, so a label cannot disagree with which half of the bytes it is.
- The offsets in the dump are that direction's **own** stream offsets, so they line up with the TCP sequence base when a SYN was seen.
- **A hole in the stream is reported before the bytes, not after.** A gap or a truncated direction prints a `NOTE:` line before its dump; a reader who stops at the first block is still told the stream is incomplete. Silence about it would be worse than the gap itself, since every decode after a hole is wrong.
- `--text` and `--out` compose: one run can print the stream and save the capture. `--bin` is a **third** output mode: it writes the raw reassembled bytes to `--out` instead of the hex dump, so it requires `--out` and is refused alongside `--text`.

### `export`

`export --out FILE` writes a **view**, not a dump.

| Switch | Behavior |
|---|---|
| `--out FILE` | Required. Without `--format` the extension picks the format (`.csv`, `.tsv`, `.json`). An unknown extension is refused with instructions. An **existing** file at that path is refused (exit `2`) unless `--force` is given. |
| `--force` | Authorise replacing an existing `--out` file. Without it the write never happens. |
| `--format csv\|tsv\|json` | Overrides the extension. Use `tsv` when a value may contain a comma and the consumer is not Excel. |
| `--filter` | Same grammar as `list --filter`. |
| `--columns` | Exact column list in your order; `default`, `minimal`, `full` are shorthands. Applies identically to CSV, TSV and JSON. |
| `--group`, `--traffic`, `--sort` | As in `list`. See [grouped views](#grouped-views). |
| `--quiet` | Still writes the file but prints no confirmation line. The exit code still says whether rows were written. |

- CSV and TSV are UTF-8 **with a BOM** (otherwise Excel on a Western locale mangles non-ASCII paths and names). JSON is UTF-8 **without** one (a BOM makes some parsers reject the file).
- `--limit` is **refused**, not ignored. An export always writes the whole view, because a file that silently holds 5 of 300 rows while the tool reports success misrepresents the machine. Narrow with `--filter`, or pipe `list --limit`.
- **An existing `--out` file is refused** (exit `2`), not replaced. The write is `CREATE_ALWAYS`, so before this rule an export aimed at an occupied path destroyed that file with no warning and nothing in the exit code to say so. Measured on one host: a 6-byte file became an 844-byte export, exit `0`, one line naming the destination and nothing about what it had replaced. The GUI already asks, through the common dialog's own overwrite prompt; a script cannot be asked, so it says `--force` instead. The same rule covers `capture --out`, whose pcapng write is `CREATE_ALWAYS` too - and there the check runs before the capture window opens, so a dry run tells you the destination is taken.
- CSV fields are escaped **RFC 4180** style: a value containing a comma, a `"`, a CR or a LF is wrapped in quotes and every embedded `"` is doubled, so a path or a note can never split a row. `tsv` needs no quoting rule - a tab, CR or LF inside a value is replaced by a space instead.

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

### `bookmark`

A bookmark pins a **remote address + remote port**. That pair survives a reconnect, and the port is part of the match: a mark on `1.2.3.4:9999` does not light up a live `1.2.3.4:443` row.

| Subcommand | Behavior |
|---|---|
| `add --address A --port P [--tag 0..4] [--note TEXT]` | Creates a bookmark. Addresses normalise (`::ffff:1.2.3.4` becomes `1.2.3.4`); placeholders such as `0.0.0.0` are refused. Adding to an existing endpoint does **not** overwrite a note you wrote. |
| `note --address A --port P --note TEXT` | Replaces the note. |
| `colour --address A --port P --tag N` | Changes only the color. Tags: `0` none, `1` red, `2` amber, `3` blue, `4` green. |
| `list [--format json]` | The store. JSON keys: `address`, `port`, `tag`, `note`. |
| `remove --address A --port P` | Deletes the bookmark; color and note go with it. |

Bookmarks are stored per user in `HKCU`. The note is joined onto live rows, so `note:` is a real filter field, and the `pinned` column prints the color name.

### `preset`

Saved views, stored per user in `HKCU` and shown in the GUI **File** menu.

| Subcommand | Behavior |
|---|---|
| `save --name N [--filter F] [--sort COL]` | Stores a view. Overwriting an existing name exits `3` unless `--force` is given. |
| `list` | All saved views (`table` or `json`). |
| `show --name N` | The stored view as JSON. The state includes filter, sort column and direction, grouping, and the column mask (`colVisible`, a bitmask). |
| `apply --name N [--limit N] [--columns …]` | Prints the current table through the saved view. Output switches are layered **over** the preset. A view that matches nothing prints only its header and exits `0`, exactly as `list` does; `apply` takes no `--quiet` (that is `2`), so there is no match-or-not exit code to branch on. |
| `delete --name N` | Removes a preset. |

### `geoip`

`geoip info --db FILE` reports a database's type, record count and size: the first thing to check when a `country` column is empty, because an empty cell is otherwise ambiguous between "no database" and "no entry for this address". `geoip lookup` answers a one-off address question; see `wintcp.exe help geoip`.

Both sub-commands want `--db FILE`: without it they exit `2`, and a file that will not load exits `1` carrying the parser's reason. Neither ever fetches anything — obtaining a database, and keeping it current, is covered in [GeoIP database](../README.md#geoip-database).

Record layouts of 24, 28 and 32 bits are all supported (28 is what essentially every real MaxMind database uses). Any other declared size is refused by name, including `record_size = 4`, which is the 32-bit layout stated in bytes rather than bits.

## Migrating from earlier builds

There is no legacy flag mode. The bare flags `-c`, `--json`, `--selftest` and `--bench` are gone, and a leading `-` or `/` argument is now a usage error. The data verbs replace the data flags one for one (`--json` becomes `--format json`). The self-test and benchmark live in the separate development binary `wintcp-tests.exe`; see [Development](development.md).
