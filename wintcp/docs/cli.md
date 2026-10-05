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
- **Only `--watch` polls.** `--count N` bounds it, which is what makes it safe in a script.
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
| | `top` | Synonym for `ps`: same rows, same switches, same defaults. |
| | `stat` (alias `sys`) | System CPU, memory, disk and network rates. |
| | `details` | Full report for exactly one connection. |
| Act | `kill` | End the process that owns a connection, or a PID. |
| | `close` | Tear down one TCP connection; the process survives. Admin, IPv4. |
| | `block` | Close a live connection **and** add an outbound firewall rule for its remote endpoint. Admin. |
| | `unblock` | Remove the rules `block` created for an address and port. Admin. |
| | `capture` (alias `follow`) | Record and reassemble one TCP stream for a fixed window. Admin. |
| Audit | `blocks` | Count firewall rules carrying the WinTCP tag. Read-only; exits `0`; needs no elevation and no `--yes`. |
| Library | `bookmark` | Pin a remote endpoint with a color and a note. |
| | `preset` | Save, show, apply and delete named views. |
| | `export` | Write a view to CSV, TSV or JSON. |
| | `geoip` | Inspect a `.mmdb` database and look up addresses. |
| Other | `version` | Report what is available on this system, and why anything is not. |
| | `help` | Overview or per-command help. |

The act verbs (`kill`, `close`, `block`, `unblock`, `capture`) require `--yes` and support `--dry-run`. `wintcp.exe help` lists `blocks` alongside them; unlike the others it only reads.

## Exit codes

| Code | Meaning | Examples |
|---|---|---|
| `0` | Success. | A command ran; a `--quiet` filter matched; a `--dry-run` printed its plan. |
| `1` | Failure or empty result. | No row matched; a `--select` matched 0 or more than 1 rows; `list --quiet` found nothing; `capture` or `close` was run unelevated. |
| `2` | Bad arguments. | Unknown or misplaced switch; `kill --pid 4` (a refused target); `export --group --columns …,remote`. |
| `3` | Refused. | A mutating command without `--yes`; `preset save` over an existing name without `--force`. |

Exit `2` and exit `3` differ on purpose. `2` means the command line was wrong; `3` means it was well-formed but not confirmed. A script can branch on whether an action was *refused* as opposed to *failed*.

```bat
> wintcp.exe kill --pid 999999
kill PID 999999: refused: pass --yes to confirm (or --dry-run to preview).
:: exit code 3

> wintcp.exe kill --pid 999999 --dry-run
kill PID 999999
:: exit code 0

> wintcp.exe kill --pid 4
kill: refusing PID 4.
:: exit code 2 — a bad ARGUMENT, not a missing permission
```

## Strict switch handling

A switch that a command does not use is an error that names both the switch and the command:

```bat
> wintcp.exe export --out snap.csv --changes
export: --changes is not a switch of this command; it would be ignored.
Try 'wintcp.exe help export'.
:: exit code 2
```

This is deliberate. A silently swallowed switch gives a script a **successful** run and the wrong answer, with nothing in the exit code to say so. `wintcp.exe help <command>` lists exactly which switches that command honours.

## `list` switch reference

```text
--filter F    filter grammar: chrome, port:443, pid:1000-2000, state:estab,
              process:svchost, proto:udp, remote:1.2.3.4, note:"vendor api",
              cpu:12, rx:2.0. Space = AND, exclude:X negates, "quoted value"
              keeps a space inside one value.
--sort COL    sort column (default pid). --desc reverses.
--group       one row per process instead of per connection.
--format S    table (default) = aligned columns, header printed first, rows
              as soon as they are ready; csv / tsv = raw delimiters (tsv =
              tabs) for scripts; json = objects in one array; jsonl = the
              same objects, one per line (NDJSON), so a --watch stream can
              be consumed as it runs instead of buffered to the closing
              bracket.
--columns C   default | minimal | full (wide = full), or a comma-separated list.
--limit N     at most N rows.
--quiet       print nothing; exit 0 when any row matches, else 1.
--traffic     per-PID byte totals via one bounded socket scan.
--dns         reverse-DNS the printed rows only (slow; bound with --limit).
--db FILE     load this .mmdb and join country codes for printed rows.
--changes     with --watch/--count: print only APPEAR / DISAPPEAR / STATE
              deltas between polls. The first snapshot is the baseline.
--event LIST  with --changes: appear,disappear,state (comma separated, any
              order, case insensitive). Default: all three.
--watch [s]   re-print every s seconds (default 2, range 1..3600).
--count N     stop after N snapshots (bounds --watch for scripts).
```

Notes:

- `--watch` re-prints the table; with `--changes` it prints only deltas. The interval **is** the sensitivity: at 1 s you catch short-lived sockets, at 10 s you miss them.
- `--count` counts **snapshots, not events**. On a busy machine four snapshots can emit hundreds of lines; pipe through `head` if you need exactly N.
- `--event` takes a comma-separated list. An unrecognised name exits `2`; it is never silently dropped.
- `--sort` takes the column names in the [column reference](#column-reference). `ps` uses its own six keys (see [`ps` and `top`](#ps-and-top)).

## Switch dependencies

Some columns and filters need the switch that supplies their data. When it is missing, the column is empty, and a `note:` on stderr names the switch (suppressed under `--quiet`).

| You want | Add | Why |
|---|---|---|
| `host` column, `host:` filter | `--dns` | Reverse DNS runs only on the printed rows. It is slow; bound it with `--limit`. |
| `country` column, `country:` filter | `--db FILE` | A memory-mapped lookup; no network. |
| `traffic`, `rx`, `tx`, `nettotal` | `--traffic` | One bounded socket scan, joined per PID. |
| `duration` | `--traffic` | Ages come from the kernel's `ConnectionTimeMs` in the same scan. Without the scan the column reads `0s`. |
| `bandwidth` (per-socket rate) | `--traffic` and `--watch N` | A rate is the difference between two samples. The first tick shows `—` by design. |
| `procspeed` (per-process rate) | `--traffic` and `--watch N` | Sum of that process's per-socket rates. |
| `rtt`, `minrtt`, `cwnd`, `retrans` | `--traffic` | Same `SIO_TCP_INFO` call as the byte counters. |

## Column reference

Use `--columns default`, `minimal` or `full` (alias `wide`), or a comma-separated list. The same names are used by `list`, `export` and `--sort`. Run `wintcp.exe help list` for the authoritative list for your build.

| Group | Names |
|---|---|
| Connection | `proto`, `local`, `lport`, `remote`, `rport`, `state`, `pid` |
| Process | `process`, `service`, `path` |
| Live, per process | `traffic`, `rx`, `tx`, `nettotal`, `cpu`, `mem`, `disk`, `procspeed` |
| Live, per connection | `duration`, `bandwidth` (header *Speed*), `rtt`, `minrtt`, `cwnd`, `retrans`, `tls` |
| Enrichment | `host` (needs `--dns`), `country` (needs `--db`), `pinned` (bookmark color; header *Bookmarks*), `note` (the bookmark's text; header *Note*) |

Value conventions:

- **Unknown is `—`, never `0`.** "We could not measure this" and "this is zero" are different answers, and unknown values always sort last, in both directions.
- `rtt` and `minrtt` are in **milliseconds**. The kernel reports microseconds; the conversion happens once, at the read boundary. A sub-millisecond RTT prints `<1`, never `0`.
- `retrans` prints `0 B` for a connection that has genuinely never retransmitted. That is a real answer, not a missing one.
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

A sectioned dossier for **exactly one** connection: process identity, command line, start time, service, live CPU/memory/disk/network statistics, the selected connection, and every other endpoint owned by the same PID. `--traffic` fills the network lines and rate, `--dns` fills the hostname, `--db` fills the country. Ambiguity and no-match are both exit `1`.

### `kill`

`--select SEL` resolves to one live row and ends the process that owns it; `--pid N` names a PID directly. Before acting the PID is re-validated against the process creation time, so a recycled PID is refused. PID 0 and PID 4 are always refused (exit `2`).

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

### `export`

`export --out FILE` writes a **view**, not a dump.

| Switch | Behavior |
|---|---|
| `--out FILE` | Required. Without `--format` the extension picks the format (`.csv`, `.tsv`, `.json`). An unknown extension is refused with instructions. |
| `--format csv\|tsv\|json` | Overrides the extension. Use `tsv` when a value may contain a comma and the consumer is not Excel. |
| `--filter` | Same grammar as `list --filter`. |
| `--columns` | Exact column list in your order; `default`, `minimal`, `full` are shorthands. Applies identically to CSV, TSV and JSON. |
| `--group`, `--traffic`, `--sort` | As in `list`. See [grouped views](#grouped-views). |
| `--quiet` | Still writes the file but prints no confirmation line. The exit code still says whether rows were written. |

- CSV and TSV are UTF-8 **with a BOM** (otherwise Excel on a Western locale mangles non-ASCII paths and names). JSON is UTF-8 **without** one (a BOM makes some parsers reject the file).
- `--limit` is **refused**, not ignored. An export always writes the whole view, because a file that silently holds 5 of 300 rows while the tool reports success misrepresents the machine. Narrow with `--filter`, or pipe `list --limit`.
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
| `apply --name N [--limit N] [--columns …]` | Prints the current table through the saved view. Output switches are layered **over** the preset. A view that matches nothing exits `1`, the same contract as `list`. |
| `delete --name N` | Removes a preset. |

### `geoip`

`geoip info --db FILE` reports a database's version, record count and size: the first thing to check when a `country` column is empty, because an empty cell is otherwise ambiguous between "no database" and "no entry for this address". `geoip lookup` answers a one-off address question; see `wintcp.exe help geoip`.

Both 24-bit and 28-bit record layouts are supported (28-bit is what current MaxMind databases use).

## Migrating from earlier builds

There is no legacy flag mode. The bare flags `-c`, `--json`, `--selftest` and `--bench` are gone, and a leading `-` or `/` argument is now a usage error. The data verbs replace the data flags one for one (`--json` becomes `--format json`). The self-test and benchmark live in the separate development binary `wintcp-tests.exe`; see [Development](development.md).
