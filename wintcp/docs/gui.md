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
- [Export](#export)
- [Change log](#change-log)
- [Performance graphs](#performance-graphs)
- [Traffic counters in the GUI](#traffic-counters-in-the-gui)
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

**Default layout** leads with identity: Process, CPU %, Traffic, Proto, Local, LPort, Remote, RPort, State, PID, Service, Host, Path. Memory, Disk I/O and the split Received / Sent / Net total columns are opt-in.

Five further diagnostic columns are available from **View → Columns** and hidden by default because they are valuable while chasing one slow connection and noise otherwise: `RTT`, `Min RTT`, `Cwnd`, `Retrans` and `Proc Speed`. Their meaning is explained in [Traffic counters](traffic.md#what-the-per-socket-scan-provides).

Readings that cannot be taken (for example a protected process without elevation) show `—` and sort to the end in **both** directions, rather than sorting to the top as a zero would.

The visible set, order, widths and sort persist between runs. The persisted column mask is versioned (`ColVersion`), so an existing profile gains newly default-visible columns exactly once and is not reset again afterwards.

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
| **Export selection…** | Writes the selected rows to a file. See [Export](#export). |
| **Refresh** | Same as F5. |

## Details window

A sectioned, modeless window with the process identity, command line, creation time and service; live CPU, memory, disk and network statistics; the full list of connections owned by the same PID; and the selected connection.

- The header line reads `process.exe (PID n)`.
- It opens centered over the main window and scales with DPI.
- While open it **refreshes silently on every auto-refresh**. If the tracked process exits, a note appears once.
- **Copy** and **Open file location** buttons are provided.

The same report is available from the command line with `wintcp.exe details`; see the [cookbook](cookbook.md#11-full-dossier-for-one-connection).

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

## Windows integration

- **DPI.** Per-Monitor V2 DPI awareness via the manifest, 9 pt Segoe UI. Widths and fonts are recomputed on `WM_DPICHANGED`.
- **Theme.** There is no dark-mode toggle. The list, status bar and filter box are painted from the live system colors (`GetSysColor`) and repaint on `WM_SYSCOLORCHANGE` and `WM_THEMECHANGED`, so light, dark and high-contrast schemes work without a restart.
- **High contrast.** `SPI_GETHIGHCONTRAST` takes precedence over the theme check.
- **Tray.** Minimize-to-tray hides the window; the tray menu offers Open, Always on top and Exit; double-click restores.
- **Always on top** can be toggled from the menu or the tray.

## Settings and reset

All toggles and window placement are stored in `HKCU\Software\WinTCP`. It is the only key WinTCP writes. Delete it to reset the application to its defaults.
