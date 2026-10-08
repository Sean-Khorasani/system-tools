# GUI guide

← [Back to the README](../README.md)

This page covers the desktop application. For the command-line mode see the [CLI reference](cli.md).

## Contents

- [Main window](#main-window)
- [Refreshing and row highlighting](#refreshing-and-row-highlighting)
- [Columns](#columns)
- [Filtering and sorting](#filtering-and-sorting)
- [Actions](#actions)
- [Details window](#details-window)
- [Bookmarks](#bookmarks)
- [Grouping and freezing](#grouping-and-freezing)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Export](#export)
- [Change log](#change-log)
- [Performance graphs](#performance-graphs)
- [Traffic counters in the GUI](#traffic-counters-in-the-gui)
- [GeoIP in the window](#geoip-in-the-window)
- [Windows integration](#windows-integration)
- [Settings and reset](#settings-and-reset)

## Main window

The window lists every TCP and UDP endpoint, IPv4 and IPv6, with its owning process. Enumeration uses `GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL)` and `GetExtendedUdpTable(UDP_TABLE_OWNER_PID)`. UDP rows follow `netstat` conventions: local port only, remote shown as `*:*`, state shown as `—`.

Some conventions you will see in the list:

- PID 0 shows `—` and PID 4 shows `System`.
- IPv6 link-local addresses show their **scope ID** (`fe80::…%12`).
- Process name, path and service are resolved through a persistent cache that is validated by PID *and* creation time, using one Toolhelp snapshot per refresh.

All heavy work (enumeration, process resolution, reverse DNS) runs on worker threads and is posted to the UI thread when ready. The list itself is a **virtual ListView** (`LVS_OWNERDATA`), so it stays responsive with tens of thousands of rows and preserves selection, focus and scroll position across refreshes.

## Refreshing and row highlighting

The list refreshes automatically; **F5** refreshes immediately. The live per-process columns are sampled on the refresh worker on every cycle, whether manual or automatic. CPU % is meaningful from the very first refresh because the worker primes its baseline with a short double sample.

Rows keep a stable identity across refreshes, so changes are visible rather than the list simply flickering:

| Highlight | Meaning |
|---|---|
| Green | New row. |
| Yellow | The TCP state changed (for example `ESTABLISHED` → `TIME_WAIT`). |
| Red "ghost" | The row vanished. It stays visible for one more cycle, then drops. |

The identity of a row is **endpoint + PID**. State is not part of the identity, so a state change updates the same row instead of replacing it. Duplicates pair up in order rather than collapsing, which matters in practice: every browser binds its own socket to mDNS port 5353, so a busy desktop has dozens of rows that share one key. See [Architecture](architecture.md#row-identity-and-diffing) for why this matters.

In high-contrast mode every highlight color is derived from `COLOR_HIGHLIGHT` and `COLOR_WINDOW` rather than a fixed palette.

## Columns

Open **View → Columns** to show or hide columns. A check mark appears on every visible column and stays in sync when the menu is opened, when the header context menu is used, and at startup. Column titles are shared by the GUI, CSV export and CLI output.

| Column | Contents |
|---|---|
| Process | Executable name; `System` for PID 4, `—` for PID 0. |
| CPU % | Per-process CPU usage. |
| Traffic | Combined per-process bytes received / sent. Source: ETW when elevated, otherwise the per-socket fallback. See [Traffic counters](traffic.md). |
| Proto | `TCPv4`, `TCPv6`, `UDPv4`, `UDPv6`. |
| Local, LPort | Local address and port. |
| Remote, RPort | Remote address and port (`*:*` for UDP). |
| State | TCP state; `—` for UDP. |
| PID | Owning process ID. |
| Service | Windows service name, when the process hosts one. |
| Hostname | Reverse-DNS name of the remote address (resolved on a background worker). |
| Path | Full path of the executable. |
| Received, Sent, Net total | The split of the Traffic column. |
| Memory | Working set. |
| Disk I/O | Bytes read plus written. |
| Parent process | The owning process's parent, as `<pid> <name>` (header *Parent*). `—` when the Toolhelp snapshot did not cover it — which is not the same as "no parent". |
| Integrity | The process's mandatory integrity level as a word: `Medium`, `High`, `System`, `Protected`, `Low`, `Untrusted`, with `+AC` appended for an AppContainer process. |
| Signature | The Authenticode verdict: `Signed`, `unsigned`, or `BAD SIG`. In the GUI this reads `—` unless verification has been asked for, because the trust provider is far too slow to run on every refresh. |

**Default layout** leads with identity: Process, CPU %, Traffic, Proto, Local, LPort, Remote, RPort, State, PID, Service, Host, Path. Memory, Disk I/O and the split Received / Sent / Net total columns are opt-in.

Five further diagnostic columns are available from **View → Columns** and hidden by default because they are valuable while chasing one slow connection and noise otherwise: `RTT`, `Min RTT`, `Cwnd`, `Retrans` and `Process rate`. Their meaning is explained in [Traffic counters](traffic.md#what-the-per-socket-scan-provides).

**Parent process** and **Integrity** are hidden by default for a different reason than those five: they are *trust* columns, and a question about who launched a process or how much Windows trusts it is asked about a handful of rows, not about the three hundred on screen. They need no switch — both are read on the handle and the snapshot the resolver already has. `Signature` needs `--signatures` on the command line, because `WinVerifyTrust` builds a certificate chain per image; see the `list` verb in the [CLI reference](cli.md).

`unsigned` is **not** a warning. Most of what runs is unsigned and that is normal; `BAD SIG` is the state that means something. `—` means WinTCP did not look, which is a third answer again and is never rendered as `unsigned`.

Readings that cannot be taken (for example a protected process without elevation) show `—` and sort to the end in **both** directions, rather than sorting to the top as a zero would.

The visible set, order, widths and sort persist between runs. The persisted column mask is versioned (`ColVersion`), so an existing profile gains newly default-visible columns exactly once and is not reset again afterwards. The ceiling is 32 columns and that is a deliberate freeze, not a limit that ran into - see architecture.md; the 32nd column leaves no room for another, so a feature that needs one reuses an existing column or raises it deliberately.

## Filtering and sorting

Above the list are a **protocol** combo (All, TCPv4, TCPv6, UDP and so on), a **TCP-state** combo, and a **filter box** with a 250 ms debounce. The filter box accepts the field expressions described in the [filter language](filters.md): substring matches, field-restricted matches, ranges, numeric thresholds on live stats, side restrictions, quoting, `exclude:` and implicit AND.

Click a column header to sort, and click again to reverse. Sorting by State uses the natural TCP-state order rather than alphabetical order. Sort column, direction, column widths and visibility, and the filter text all persist.

Menu commands exist for *Select all*, *Focus filter* and *Clear filter*, and an accelerator table is provided.

Presets saved with `wintcp.exe preset save` appear in the **File** menu, so a view built on the command line is available in the window.

## Actions

Right-click a row (or double-click to open Details):

| Command | Behavior |
|---|---|
| **Details** | Opens the [Details window](#details-window). Double-clicking a row does the same. |
| **Copy selected / Copy all** | Copies rows to the clipboard. |
| **End process** | Ends the owning process after verifying that the PID still refers to the same process (PID-reuse check). |
| **Close connection** | Deletes the TCP entry with `SetTcpEntry(DELETE_TCB)`. IPv4 only, and requires administrator rights. |
| **Open file location** | Opens Explorer on the row's executable. |
| **Block this connection** | Adds a firewall block rule. IPv4 only, and requires administrator rights. |
| **Export selection.** | Writes the selected rows to a file. See [Export](#export). |
| **Refresh** | Same as F5. |

Two commands were **removed on 2026-10-05** after being reported not to work. Neither was repaired in place, and the reasoning is worth stating because both look like features one could still want:

- ***Process properties*** is gone from both the Process menu and the context menu. `properties` is a shell **verb**, run through a shell item rather than by handing `ShellExecuteW` a bare path - which is exactly the case where it fails. The old handler reported one indistinguishable message box for that, for a missing path, and for PID 4's placeholder `System` path.
- ***Follow TCP stream*** is gone from both menus. The capability is not lost: it is the [`capture`](cli.md#capture) verb on the command line, where the blocking it requires is the point rather than the defect. `capture --text` prints the reassembled stream to stdout the way `tcpflow -A` does, and `capture --out FILE` keeps the capture as pcapng for Wireshark or `tshark`.

## Details window

A sectioned, modeless window with the process identity, command line, creation time and service; live CPU, memory, disk and network statistics; the full list of connections owned by the same PID; and the selected connection.

- The header line reads `process.exe (PID n)`.
- It opens centered over the main window and scales with DPI.
- While open it **refreshes silently on every auto-refresh**. If the tracked process exits, a note appears once.
- **Copy** and **Open file location** buttons are provided, in a row below the body.
- **Every connection owned by that PID is listed** - all of them, not a preview. It used to stop at 25 rows and report *… and N more*, which hid the one section that answers "what is this process actually connected to"; the cap is gone rather than made expandable, because an expander would put the answer behind a click on a window whose whole model is "read, and collapse sections". A browser process with 70-odd sockets lists all of them.
- Note that a multi-process browser puts each process in its **own PID**, so `PID 50092` never mixes in another `chrome.exe`'s sockets, and rows belonging to other PIDs are never listed at all. The list is scoped to the selected row's process, not to everything in the snapshot.

### Threads

A **Threads** section lists the process's own threads, ranked by CPU time with the busiest first:

- Each row is one thread: `TID`, total CPU time since it started, how long ago it started, and its base priority.
- **Ranked by CPU**, because that ordering is the answer to "what is this process doing". A ranked list says which thread is working; an unordered one says only how many there are.
- All of them are listed. A 460-thread process shows 460 rows.
- Threads whose times cannot be read show `-`, not `0`, and sort **last**. That is a real condition for protected processes, and printing `0.00 s` for them would be a fabricated measurement rather than an admission of ignorance.
- The note says how long ago the sample was taken, and that the CPU figure is lifetime-since-thread-start rather than since the window opened - otherwise "cpu 3985 s" beside a window opened five minutes ago reads as a rate.
- The section appears within one refresh of opening, not instantly: the enumeration costs about 48 ms because `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)` walks every thread on the machine, and that runs on a worker so it never freezes the window. `wintcp.exe details` has no such problem - it is a one-shot command and waits for the answer.

**There is deliberately no per-connection thread column, because none exists.** Windows has no notion of a socket's owning thread: `MIB_TCPROW_OWNER_PID` - the row behind the `GetExtendedTcpTable` call this whole app is built on - carries no thread id, and an AFD endpoint is owned by the *process*. That is why TCPView, Process Explorer and Wireshark all stop at the PID too. The only thread association available comes from ETW event headers, and it is the thread that *performed an I/O*, not an owner: on a socket shared between threads it changes from packet to packet, and it is only observable while a trace session runs. A column headed *Thread* holding that would be a label the data does not support.

For the same reason the section omits each thread's current state. `GetThreadInfo`/`THREADINFO` is a legacy kernel32 export that the Windows SDK no longer declares for user mode, and its state and wait-reason values are not published - so "Waiting"/"Running" would mean inventing a mapping. Better absent than wrong.


### Layout

Fields are laid out as a **two-column table**, one row per field:

- The label sits in a fixed-width left column, so every value in the window starts at the same horizontal position regardless of how long its label is. The two are joined by a **dotted leader**, which is what makes a row read as a pair rather than as a caption with a value that happens to share a line.
- A **hairline rule** separates the label column from the value column.
- Values are drawn in a fixed-pitch face only where alignment matters (byte counts, timestamps, addresses); everything else uses the UI font, including the labels.
- Every section header carries an **accent bar** down its left edge, so the sections read as separated at a glance without hovering anything. Hovering a header highlights it, which is the cue that it can be collapsed.
- Sections are collapsible by clicking their header. The state is per-window and is never persisted.
- Long values are elided rather than wrapped; a visibly shortened value beats a silent cut. Copy the value to the clipboard to get all of it.

Two layout faults were fixed on 2026-10-05, both of which had been reported as "the window doesn't split correctly":

- Each field used to be **two** stacked rows - the label, then the value indented beneath it - so a section cost 34 px of captions and read as text rather than as a table. It is now one row per field.
- The body painted down to the **full client height** rather than to the bottom of the content region, so the last visible fields were drawn behind *Copy* / *Open file location* / *Close*. The click and hover handlers had the same missing bound, so a click in the button row could toggle a section hidden behind it.

The same report is available from the command line with `wintcp.exe details`; see the [cookbook](cookbook.md#14-full-dossier-for-one-connection).

## Bookmarks

A **bookmark** is a note and a colour attached to one remote address and port, so a peer you have decided something about can be found again later. Bookmarks live under `HKCU\Software\WinTCP\Bookmarks`, one subkey per endpoint - per user, never machine-wide.

| Where | What |
|---|---|
| **File -> Bookmarks...** | Opens the manager: every bookmark, editable, with its tag and note. |
| Select a row, then **Process -> Bookmark this connection** | Adds a bookmark for the selected row's remote address and port. |
| Select a row, then **Process -> Edit bookmark note...** | Attaches or changes that row's note. |
| **View -> Columns -> Bookmarks** | Adds the *Bookmarks* column: the tag, colour-coded. Hidden by default. |
| **View -> Columns -> Note** | Adds the *Note* column carrying the text. Hidden by default. |

Both **Process** entries act on the current selection rather than on the row under the cursor, so a bookmark is placed where you last clicked.

Five tags: **Red**, **Amber**, **Blue**, **Green**, and **None** for a bookmark that is deliberately unremarkable. The tag is the first thing the column shows, and the note is the second, so a glance answers "do I already know about this peer?" before you read anything.

This is the feature the `pinned` and `note` columns belong to, and it has a command-line face as well - see [`bookmark`](cli.md#bookmark). The notes are also searchable: `note:` is a filter field, so `--filter "note:corporate dns"` selects every bookmarked peer whose note mentions it.

## Grouping and freezing

Two **View** switches change what a row *means* rather than what it shows:

| Switch | Shortcut | Effect |
|---|---|---|
| **Group rows by process** | `F7` | Collapses the table to one row per **process** instead of one per socket. A browser with 70 sockets becomes one row. |
| **Freeze view** | `F6` | Stops the row list being repainted, so a selection and a scroll position stay put while the refresh continues behind it. Use it when reading a column and the list would otherwise move under the cursor. |

Grouping answers "who", not "which socket" - the difference between recipe 5 in the [cookbook](cookbook.md) (one row per process) and recipe 14 (everything known about one connection). Nine columns cannot survive that change honestly: **Local address**, **Local port**, **Remote address**, **Remote port**, **Duration**, **Hostname**, **TLS**, **Country** and **Note** each describe one connection, or in the case of a note one remote endpoint. Under `--group` the CLI refuses those nine in `csv`, `json` and `full`, because a file's header would promise a value its rows do not carry - and it names the offending columns so the mismatch is visible rather than silently repaired. The default `table` format is deliberately exempt: a grouped row already says what it is, because its **State** cell reads `4 connections`, its **Proto** cell reads `TCP+UDP`, and its **Process** cell falls back to the PID when the name is unknown. In a table a blank column next to those is self-explanatory; in a CSV it is a lie.

## Keyboard shortcuts

**Help -> Keyboard shortcuts** (`F1`) opens the in-app sheet. The same set, in the order the sheet groups it - by what the keys do, not alphabetically:

| Keys | Action |
|---|---|
| `F1` | Keyboard shortcuts (this sheet) |
| `F5` | Refresh now |
| `F6` | Freeze / unfreeze the view |
| `F7` | Group rows by process on / off |
| `Ctrl+C` | Copy selected connections |
| `Ctrl+A` | Select all rows |
| `Ctrl+F` | Focus the filter box |
| `Ctrl+S` | Save view as preset |
| `Ctrl+E` | Export to CSV |
| `Esc` | Clear the filter |
| `Del` | Graceful close: `WM_CLOSE`, then terminate |
| `A`-`Z` | Type to jump to a row by process name |
| `Backspace` | Back out of a type-to-jump prefix |
| `Alt` | Underline a letter to walk the menus (hold `Ctrl` as well) |
| `Ctrl`+click | Extend the selection |
| `Shift`+click | Select a range |
| Double-click | Open the details window for the row |
| `Shift`+`F10` | Open the context menu for the selected row |
| Drag a header | Reorder a column; the order is remembered |
| `Tab` | Move between the filter controls and the list |

Type-to-jump matches a prefix of the process name and moves the selection with each keystroke, so a stray letter typed over the list is harmless rather than destructive. Two details make it usable: pressing the **same character again cycles** to the next match, so a prefix shared by a dozen rows (`svchost.exe`) still moves; and a repeated character is never appended, so pressing `s` twice means "the next row starting with s" rather than a prefix of `ss` that matches nothing. A pause of about a second (the Explorer's interval) breaks the sequence, so the first keystroke after thinking always starts a fresh search. Matching is prefix-only by design: typing `svch` lands on rows *beginning* `svch`, never on the first row that merely contains it somewhere.

`Ctrl+S` saves the current filter, sort and column selection as a preset that **File -> Load preset...** restores; it is listed by the menu, bound in the accelerator table, and present in the F1 sheet like every other shortcut.

## Export

Export to **CSV**, **TSV** or **JSON**, either the whole table or just the selection.

- Files are UTF-8. CSV and TSV carry a BOM so Excel on a Western locale reads non-ASCII paths and process names correctly; JSON never carries one, because many parsers reject it.
- CSV uses RFC 4180 quoting.
- The extension picks the format when one is present.
- The last-used folder is remembered.

For scripted exports and column control use `wintcp.exe export`; see [CLI reference](cli.md#export).

## Change log

**File → Change log** writes an append-only CSV of `APPEAR`, `DISAPPEAR` and `STATE` events, taken from the same diff that drives the row highlighting on each refresh.

## Performance graphs

**View → Performance graphs…** opens a modeless window with four live line charts, sampled once per second with a 120-sample history stretched across the plot area:

| Panel | Source |
|---|---|
| System CPU % | System-wide CPU sampling. |
| Memory used % | System memory. |
| Disk read / write B/s | PDH `PhysicalDisk(_Total)`, using locale-independent English counter paths. Shows `n/a` for that panel when counters are disabled. |
| Network down / up B/s | 64-bit interface octet deltas from `GetIfTable2`. |

Each panel is independent and has its own "collecting…" and `n/a` states. The charts are double-buffered, follow the system theme and enforce a minimum window size. The window opens centered over the main window; closing it with **X** hides it and stops the one-second sampling timer, the menu entry shows it again (and its check mark re-syncs when the menu opens), and it is destroyed with the application.

## Traffic counters in the GUI

**View → Per-PID traffic counters (ETW, admin)** starts the kernel logger explicitly. Making any traffic column visible, or starting with one persisted, tries ETW **once** silently; if that fails, the unprivileged socket fallback arms itself with no message box, because it succeeds.

The status bar's second pane reports which source is feeding the columns:

| Pane text | Meaning |
|---|---|
| `Ready` | A source is feeding the visible traffic columns. |
| `TCP only — UDP needs admin` | The unprivileged fallback is running (tooltip explains the UDP gap). |
| `Traffic off — needs admin` | No source can run on this system. |

Full details, including limits, are in [Traffic counters](traffic.md).

## GeoIP in the window

**View → GeoIP database (.mmdb)...** opens a picker titled *Open a MaxMind .mmdb database*, filtered to `*.mmdb`. Pick a file and the `Country` column fills at once rather than at the next refresh: choosing a file is a request to see results, not to wait for a tick. Cancelling the picker is not an error and changes nothing.

A file that will not load reports the parser's own reason under **GeoIP database not loaded**, because a missing file, a truncated one, a file that is not an MMDB, and a database declaring an unsupported record size are four different problems that need four different answers from you.

Reloading works on a live session: picking another file replaces the previous one, and a peer that moves out of the new database's coverage has its country **cleared** rather than keeping a stale answer — the one way a swap could look like it worked while showing the wrong country. Nothing is downloaded either way. [GeoIP database](../README.md#geoip-database) explains why not, where a free `GeoLite2-Country.mmdb` comes from, and how `geoipupdate` keeps it current.

**The choice survives a restart.** Window placement, columns, sort, filter, always-on-top, tray and the database path all persist in `HKCU\Software\WinTCP`. Pick a database once and the next launch silently reloads the stored path, so the `Country` column repopulates without a second trip through the picker. A moved or deleted file fails to load quietly — `Country` shows "—" as it would for no database at all, which is the honest outcome — and **View → GeoIP database** still reports the parser's reason under **GeoIP database not loaded** when it does fail.

The path is the only thing remembered, not the database's contents: a file replaced in place is picked up on the next refresh. From the command line `--db FILE` stays explicit per run, and `list --watch --db FILE` re-reads the file on every tick, so a refreshed database shows up without restarting anything.

## Windows integration

- **DPI.** Per-Monitor V2 DPI awareness via the manifest, 9 pt Segoe UI. Widths and fonts are recomputed on `WM_DPICHANGED`.
- **Theme.** There is no dark-mode toggle. The list, status bar and filter box are painted from the live system colors (`GetSysColor`) and repaint on `WM_SYSCOLORCHANGE` and `WM_THEMECHANGED`, so light, dark and high-contrast schemes work without a restart.
- **High contrast (9.4.9).** `DetailsDialog`. `MainWindow` and `DetailsDialog` both call `HighContrastActive()` (`SPI_GETHIGHCONTRAST`) and bypass their dark palette when it is set, deriving every colour from `GetSysColor` so the scheme's own values win. `MainWindow::ThemeIsDark` short-circuits to `false` under HC, and `DetailsDialog::ApplyColors` re-derives on `WM_SYSCOLORCHANGE`/`WM_THEMECHANGED`, so toggling HC mid-session repaints immediately. The hard-coded `kDark*` substitutes are never applied on top of an HC palette.
- **Tray.** Minimize-to-tray hides the window; the tray menu offers Open, Always on top and Exit; double-click (or single-click, line 759) restores. A balloon fires on a new listener via `ShowTrayBalloon` (Alerts.cpp), though the alert loop itself is not yet wired into the live refresh. **First-run minimize prompt (9.4.6):** the first time you minimize while the tray icon is off, a dialog asks "Minimize to tray or exit?" and an unchecked "Don't ask me again" means it will ask again next time. The remembered choice and the tray-on flag persist in `HKCU\Software\WinTCP` as `TrayMinimizeChoice` and `TrayEnabled`.
- **Always on top** can be toggled from the menu or the tray.

## Settings and reset

