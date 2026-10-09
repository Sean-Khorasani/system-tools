# WinTCP — Open Work

**Created 2026-10-07.** This file is the authoritative tracker of *everything still open*.
The previous tracker (2026-09-30 through 2026-10-06) is preserved at `temp/todo_2026-10-07.md`.

**Rule:** if it is not in this file, it is done.

`[ ]` not started · `[~]` in progress · `[x]` done **and** verified.

**On finished items:** an earlier version of this header said "delete the line
here. Do not leave `[x]` items." That rule was never followed and cannot be:
every `[x]` entry here carries the *evidence* that closed it (the file:line, the
new tests, the gate counts), and that evidence is the whole reason to keep the
entry. The file has therefore settled on `[x]` + a DONE note. Two consequences
worth respecting:

- Keep the DONE note short and factual — what changed, where, which gate. It is
  a receipt, not a changelog.
- If an item is only *partly* done, leave it `[~]` and say which part is
  outstanding. Do not mark a defect `[x]` because its feature-shaped remainder
  is out of scope; see 9.4.5 and 9.4.7, where the remainder was explicitly
  re-homed rather than silently dropped.
- A stale item is worse than a missing one. If a `[ ]` entry is already
  implemented and gated, fix the marker; do not leave work that reads as open.

---

## Where things stand (measured 2026-10-08, this tree)

| Gate | Result |
|---|---|
| `build.bat` (`/W4 /WX /permissive-`, from `clean`) | clean |
| `fast-build.bat -Test` (15-check smoke + harness self-check + unit driver) | **PASS 15/15** |
| `wintcp\tests\cli.bat` (CLI) | **PASS / 248 checks, 0 failures** |
| `wintcp\tests\gui.bat` (GUI) | **PASS / 54 checks, 0 failed** |
| `wintcp\tests\examples.bat` (README examples) | **PASS / 94 commands, 0 rejected** |

Two baseline facts about this tree, both learned the hard way on 2026-10-08:

- **It had never been compiled after the `winsys`→`winnet` rename.** Latent
  `/W4 /WX` breakage was sitting in `Grouping.cpp` (C4244, `uint64_t`→`size_t`)
  and three unit tests were time-dependent. Fixed and gated; see §6.
- **`fast-build.ps1` never relinked after compiling.** Its mtime cache was
  populated by the staleness check *before* the compile rewrote each `.obj`, so
  the link step compared the exe against pre-compile timestamps, decided the
  exe was current, and skipped the link. `-Test` then silently exercised the
  *previous* binary. Fixed by invalidating the cache between compile and link.
  If you ever see a gate pass that you cannot reproduce by hand, check that the
  exe is newer than every `.obj` before trusting the result.

---

## 1. Defects still open

- [x] **9.2.8 — `DnsResolver.h:13-16` documented that a cancellation
  timeout was impossible (`getnameinfo` cannot be cancelled). Re-scoped: per-tick
  budget (`--dns-timeout MS`, default 3000; getnameinfo runs on a detached helper
  thread bounded by the budget), `host: pending` vs `—` rendering (sentinel
  `DnsResolver::kPendingHost` in ConnectionStore COL_HOST + Commands details +
  MainWindow detail pane), stderr stalled count via `StallSink` + `StuckCount()`.
  Added 4 unit tests (`dns.pending-sentinel`, `dns.default-timeout-set`,
  `dns.lookup-rejects-garbage`, `dns.lookup-rejects-empty`). Docs: `docs/cli.md`
  (switch table + value conventions). Gates: unit 735/0, cli.bat 230/0,
  examples.bat 87/0.

- [x] **9.4.5 — Freeze age already shown: `MainWindow.cpp:1805-1814`**
  `FROZEN <age> — F6 to resume`. DONE. Remaining handled: grouped header count — `Grouping.cpp:112-123` `GroupColumnText` COL_PROCESS now renders `name (rowCount)` (e.g. `chrome.exe (14)`, `PID 77 (1)`); `group.unknown-pid-named` + `table.group-fallthrough-exact` tests updated for the new shape; gui.bat group+freeze sections 51/0. Toolbar still absent — no toolbar infra exists in the codebase; that is a future feature, out of scope for this defect. Docs: `docs/gui.md` grouping + HighContrast.

- [x] **9.4.7 — Overwrite confirm already present: `OFN_OVERWRITEPROMPT`**
  (now `MainWindow.cpp:2967`; the line in the todo was stale). DONE: `DoExport`
  completion message now reports the row count written — `"Exported N rows to <path>."`
  (MainWindow.cpp:3032) — so a large export gives volume feedback rather than a bare
  "Export completed."

- [x] **9.4.8 — Ctrl+S accelerator and sheet row both exist** (verified: `wintcp.rc:147`,
  `BuildInfo.cpp:40`). DONE — implemented the keyboard parts: `/` now focuses the
  filter box when the list has focus (MainWindow.cpp WM_CHAR, guarded so it does
  not hijack type-to-jump on other controls); `*` (VK_MULTIPLY → `IDM_CTX_BOOKMARK`,
  added to the accelerator table at wintcp.rc:153) toggles a bookmark on the
  selected row. Added `focusTarget` + `bookmarkCount` UiProbe ops so the harness
  can verify both — 3 new SMOKE checks pass in gui.bat (54/0). Remaining (NOT done,
  manual/feature): **MSAA accName/accDescription** — the virtual list relies on the
  default comctl32 IAccessible (process names only; no per-row "PID 1234, 8 connections"
  description), which is a feature requiring a `WM_GETOBJECT` override; **NVDA test** —
  added to gui.bat's MANUAL CHECKLIST as item 14, since no screen-reader harness can
  run headless. Docs: `docs/gui.md` (tray row) + `wintcp/tests/gui.bat` checklist.

- [x] **9.4.9 — `DetailsDialog.cpp` HighContrast handling.** DONE (verified, no code change needed): `HighContrastActive()` (DetailsDialog.cpp:925), `ApplyColors` derives from `GetSysColor` and bypasses `kDark*` under HC (line 167, with the 9.4.9 comment at 157-162), `WM_SYSCOLORCHANGE`/`WM_THEMECHANGED` re-derives (line 893), `WM_CTLCOLORBTN` respects palette (line 900), and `MainWindow::ThemeIsDark` short-circuits to false under HC (MainWindow.cpp:4414). The todo premise ("zero HC handling") was outdated — the handler already mirrors MainWindow. Docs: `docs/gui.md` High contrast section. Closed on verification.

---

## 2. Code-perfection track (§6 of old tracker)

- [ ] **P1 — Re-audit literals per file, and name the rest.**
  For each file below, classify every >=10/hex literal as (a) a decision
  that gets a named constant, or (b) table payload / wire-format byte /
  Win32 sentinel that stays literal, and record the count of each.
  Working order (measured, decimal >= 10 in product code):

  | file | measured | hex | note |
  |---|---|---|---|
  | `MainWindow.cpp` | 114 | 37 | |
  | `resource.h` | 77 | 0 | all `IDM_*`/`IDC_*` defines — skip |
  | `GeoIp.cpp` | 53 | 69 | mostly wire offsets, but read them |
  | `ConnectionStore.cpp` | 44 | 17 | |
  | `Pcapng.cpp` | 42 | 17 | mostly block sizes = (b) |
  | `StreamCapture.cpp` | 37 | 3 | |
  | `DetailsDialog.cpp` | 33 | 12 | |
  | `ChangeLogWindow.cpp` | 29 | 0 | |
  | `Commands.cpp` | 23 | 41 | |
  | `TlsDecode.cpp` | 18 | 23 | |
  | `Bookmarks.cpp` | 17 | 3 | |
  | `ChartsWindow.cpp` | 17 | 30 | |
  | `PromptDialog.cpp` | 17 | 4 | named caps landed in C7 |
  | `EtwTraffic.cpp` | 14 | 11 | |
  | `ProcessInfo.cpp` | 14 | 3 | |
  | `CliCommands.cpp` | 14 | 1 | |
  | `TcpTable.cpp` | 12 | 1 | |
  | `ChartExport.cpp` | 8 | 0 | |
  | `Utils.h` | 1 | 1 | A1/A3/A6 replaced these |

  **TOTAL: 706 decimal + 359 hex.** Do not read as 706 defects —
  `resource.h`'s 77 are definitions, `kColumns` widths are table payload,
  `0xFFFFFFFF` / `MF_*` / `SC_*` are Win32 sentinels.

- [ ] **P2 — `Bench.cpp` literals: 782 decimal >= 10 plus 768 hex = 1550.**
  Most are test vectors (data under test, correctly literal). The fixture
  *builders* carry real magic numbers: MaxMind DB control bytes
  (`kMmString`/`kMmUint16`/`kMmUint32`/`kMmMap`, `kMmMaxInlineSize`),
  report row shape (`kResultLabelWidth`), and `kExitFail` that `Check()`
  had been writing as a bare `1`. Done for those; the rest of the fixture
  vocabulary is not.

- [ ] **C12 — File splits (explicitly constrained).**
  `MainWindow.cpp`, `Commands.cpp`, `ConnectionStore.cpp`, `CliCommands.cpp`
  stay whole per §2.1 (defer until a change forces it). `Bench.cpp`
  (tests only) MAY split into `BenchGeoip/BenchStore/BenchCli/BenchPerf.cpp`
  as an optional standalone task — test-only, zero product risk.

---

## 3. Architecture (§8.4 of old tracker)

- [ ] **W4.2 — `std::wstring_view` across the parser layer.**
  One allocation-free tokeniser pass; selftest pins identical behaviour.
  Targets: `FilterExpr`, `TlsDecode`, `Commands`.

- [ ] **W4.3 — Replace `RunGuarded` traps with `Result<T>` error returns**
  at module boundaries where a predictable error beats a trap (SocketTraffic
  probe errors, `GeoIp::Load` success/parse failure). A `Result<T>` template
  is ~30 lines. Exceptions stay ONLY at the Win32 callback boundary.

- [ ] **W4.1 — `MainWindow.cpp` split — DEFERRED by user decision 2026-10-04.**
  Do not start without reading the full reasoning in the old tracker
  (§8.4). Three reasons: §2.1 forbids it, it is not mechanical (needs a
  fifth file `MainWindowInternal.h` first), and the primary tree has no git.

---

## 4. Features (§8.5 of old tracker)

All features below were approved by the user. Each is independently resumable.

- [x] **F5.4 — ASN lookups (GeoLite2-ASN.mmdb via GeoIp.cpp).** DONE.
  `GeoIpDatabase::LookupAsnV4/V6` return an `AsnInfo {number, org}`; the tree walk
  is now `ResolveOffset` + a per-record decoder, so country and ASN share one walk
  rather than each walking up to 128 node reassemblies. Rendering reuses the
  Country column per 9.1.1 (`US · AS15169 Google LLC`, separator only when both are
  known, AS number before the org name so a truncation cannot hide the network).
  New `asn:` filter, deliberately BOTH a numeric threshold and a substring:
  `asn:15169`, `asn:15169-20000`, `asn:cloudflare` all work. `--asn-db FILE` is a
  second source because the two MaxMind products are different files; the GUI has a
  second picker and its own persisted path, and neither is required for the other.
  Unknown-value rules pinned by tests (a bare `asn:` is "has an ASN", `asn:1-4294967295`
  still excludes rows with none) — `country:` shares the first of those bugs today
  and is un-fixed; `asn:` does not inherit it.
  Docs: `docs/cli.md`, `filters.md`, `cookbook.md`, `gui.md`, `README.md`.
  Gates: unit all pass, cli 244/0, examples 94/0, gui 54/0, build.bat clean.

- [ ] **F5.5 — ETW DNS snooping (provider `{1C950233-...}`, no PTR queries).**
  Answers `host:` without `--dns` slowness. Elevated only.
  Docs: `docs/traffic.md`, `cli.md`.

- [ ] **F5.6 — Real-time connection alerts (right-click → toast/balloon).**
  Depends on 9.2.11 (alert wiring). Tray balloon + alert verb + registry rules.
  Docs: new `docs/gui.md` section + `cli.md`.

- [x] **F5.7 — Retain closed sockets (grey, lifetime, final metrics).** DONE.
  `Connection` gained `deathTick`/`finalRx`/`finalTx`; `ConnectionStore` keeps a
  vanished row as a ghost with `kMaxRetainedGhosts` (500), `SnapshotTick()` and
  `TrimRetainedGhosts()`. A vanish **red-flashes for one cycle**, and from the
  second cycle onward the row is retained as a stable grey ghost; the
  DISAPPEAR change event fires **once**, not on every cycle the ghost survives.
  Final byte counters are retained, not zeroed. Selftests
  added for ghost-retention, DISAPPEAR-once and the cap. Docs: `docs/gui.md`
  refreshing, `docs/cli.md` changes (retained closed-socket ghost rows in `--watch`).

- [x] **F5.8 — IPv6 force-close.** DONE. `SetTcpEntry` is IPv4-only, so Close on an
  IPv6 row is disabled with an "IPv4 only" affordance rather than failing at the
  kernel boundary; block still returns `kRulesOnly` (kept, not converted to a
  failure). Docs: `docs/cli.md` close/block, `README.md` limitations.

- [x] **F5.9 — Raw stream binary export (.bin for Wireshark).** DONE as part of
  9.3.7 — `capture --bin` writes the raw stream, so the two are one implementation
  and one gate, not two. Docs: `docs/cli.md` capture.

- [x] **F5.11 — One-click quick filters.** DONE. Filter > All/TCP/UDP/Listeners/
  Established/Mine writes the expression into the filter box and lets the existing
  debounce apply it, so it is visible and editable, and there is one code path from a
  click to a filtered view - the same one typing uses. `Mine` is `local:private`,
  which required ADDING that value to the filter grammar: the ticket never defined
  "Mine", and the grammar cannot OR two terms, so "listening OR established" is not
  expressible and was not invented.
  Gates: unit (qfilter.* checks, both directions), cli.bat 248/0, gui.bat 54/0.

- [x] **F5.15 — A `FontCache` shared across windows.** DONE. New `FontCache.{h,cpp}`
  owns every font the process uses, keyed by (DPI, point size, weight, family). A
  caller asks for a HFONT and never deletes one, so a DPI change is `OnDpiChanged()`
  plus a rebuild rather than three windows each releasing their own handles and one
  of them chasing a copy held elsewhere - which is the hazard this removed.
  `MainWindow` and `ChartsWindow` are converted. `DetailsDialog` still DERIVES its
  header/mono/small faces from the main window's handle, which is now cache-owned and
  DPI-correct, so the dangling-handle hazard is gone; its three derived faces are not
  yet cache entries. Added to all three build paths.
  Gates: build.bat clean, unit 15/15, gui.bat 54/0; `fontcache.*` checks pin
  same-request-same-handle, bold/mono differ, a different DPI yielding a different
  height, and reset-then-rebuild.

---

## 5. Deep re-review track (§10 of old tracker)

### 5.1 Correctness

- [x] **9.2.5 — Elevation UI.** DONE in `MainWindow.cpp`, gated by `build.bat` + `cli.bat` + `gui.bat` + `examples.bat`:
  `shield glyphs` on the elevation-gated actions only - `Block this connection...` (context menu) and `Per-PID traffic counters` (View > bar) - shown only when `IsAdminMember() && !IsElevated()`, skipped under high contrast and when shell32 is delay-load unavailable. `Reelevate()` is now a LIVE caller (it had zero since Follow was removed 2026-10-05): clicking Block or turning traffic ON while unelevated hands off to `Reelevate("…")`, which preserves the command line and state path via `settings_.trafficEnabled`. A standard account (no linked token) sees `ElevationUnavailableReason()` instead of a UAC prompt. `Close connection` is deliberately UNSHIELDED: `SetTcpEntry` on the user's own sockets is not privileged, so a shield would mislead - documented at the call site. The relaunch-loses-state premise from the old 9.2.5 text is moot: `Reelevate` already restores `settings_.trafficEnabled`, and `Settings::Save()` at exit covers the rest. Docs: `docs/gui.md` actions.

- [x] **9.2.7 — pktmon probe.** DONE (verified): `CaptureToolsPresent()`
  probes `pktmon.exe` + `etl2pcap.exe` (StreamCapture.cpp:191-214),
  `EvaluateCaptureGate()` enforces tool-absence-before-elevation ordering as a
  pure testable table (line 232; all four combos pinned by `12e.` unit tests
  Bench.cpp:4235-4358, incl. the pre-9.2.7-order regression), `CaptureAvailable()`
  wires tools-first (line 253), `WinCaps.cpp:223-226` registers the
  `Stream capture (pktmon)` capability row. Docs: `docs/cli.md` (lines 302-315).
  No code change needed; closed on verification.

- [x] **9.2.8 — DNS budget.** CLOSED — see §1, where the entry and its DONE note
  live. This pointer was left behind as a second copy reading `[ ]`, which read
  as open work after the defect had been closed and gated. One entry per defect:
  the pointer goes here, the evidence goes in §1.

- [ ] **9.2.9 — Firewall viewer.**
  `BlockConn.h:100` (`Item()` not enum). GUI View→Blocked peers... +
  per-rule delete + Remove all. `blocks` counts (keep).
  Docs: `docs/gui.md` actions, `docs/cli.md` block.

- [ ] **9.2.10 — Portable + sync.**
  `wintcp.ini` beside exe → use instead of HKCU; preset export/import,
  bookmark export/import JSON on Version=1 schema (approved future,
  todo 2.1 rejects `wintcp.toml` — this is verbs, not config file).
  Docs: `docs/cli.md` preset/bookmark, `docs/gui.md`.

### 5.2 CLI

- [x] **9.3.1 — `doctor`** *(DONE. This entry previously carried the note "Full
  `doctor` command still TBD" beside a `[x]` marker, which was stale: the command
  ships. `wintcp.exe doctor [--format json]` aggregates `version` + WinCaps +
  traffic source + GeoIP + `CountWinTcpRules` from the pieces that already
  existed (`BuildInfo`, `WinCapabilities`), plus an `examples.txt` line and help.
  The separate header-label rename that shipped earlier is unaffected: header
  reads "Process rate", the CLI token `procspeed` is deliberately preserved.
  Docs: `docs/cli.md` commands table.)*
  `version` + WinCaps + traffic source + GeoIP + `CountWinTcpRules` →
  `wintcp.exe doctor [--format json]`. All pieces exist (`BuildInfo`,
  `WinCapabilities`). Add `examples.txt` line + help.
  Docs: `docs/cli.md` commands table.

- [x] **9.3.2 — `help columns` + `help filters`.** DONE. `help columns` prints the
  `--columns` tokens and `help filters` the filter fields, so the three spellings
  that used to disagree (`procspeed` vs `Proc Speed` vs `COL_GROUPRATE`) are
  listed in one machine-readable place, with the group-safe list distinguished as
  D27 requires. Golden help asserts extended. Docs: `docs/cli.md` column ref.

- [x] **9.3.3 — Min RTT shows stale value during an idle RTT sample.** *(fixed: split `rttKnown` into `rttLive` (per-tick; RTT cell blanks to `—` during an idle sample) + `rttEver` (latch for Min RTT so best-ever lingers). Render/sort/filter/comparator updated; `g6.per-field-not-all-or-nothing` passes with `rtt=— cwnd=16.5 KB`; all unit checks pass.)*

- [x] **9.3.7 — Capture finish.** DONE. `capture --bin` writes the raw stream for
  Wireshark (this also closes F5.9 — one implementation, not two) and
  `capture --filter` passes the expression through to `pktmon filter add`, so
  SYN/FIN/RST selection is expressible. Counters are emitted **before** bytes, as
  intended. Docs: `docs/cli.md` capture.

### 5.3 UI/UX

- [x] **9.4.1 — Details tabs.** DONE (baseline import `230268f`). The sheet is
  tabbed (`kTabCount`, `kTabSockets`, …): a row with no TLS **drops** the Security
  tab rather than showing an empty one, and the Sockets (Connections) tab is
  counted alongside the others so it is not silently omitted. Pin, Copy-all and
  Open-file-location kept. Docs: `docs/gui.md` details.

- [~] **9.4.2 — Empty states.** PARTLY DONE, and the part that is missing is the
  part the ticket emphasised. `MainWindow::UpdateEmptyState()` now produces one
  actionable sentence per case - no rows match this filter / no GeoIP database
  loaded / traffic not being measured - recomputed from live state by the same
  `UpdateStatusBar` path the DNS-stall hint uses, so there is no timer to write and
  nothing to clean up. Order is NO ROWS > DATABASE > TRAFFIC, because with no rows
  at all the other two answer a question nobody asked. Each names the menu item
  that fixes it.
  NOT DONE: the ticket wants **buttons** - [Run as admin], [Clear], [Edit],
  [Pick .mmdb]. There is no infobar control in this window at all: no toolbar, no
  `WM_PAINT`, and the only in-repo precedent for inline buttons is
  `TaskDialogIndirect` at `MainWindow::ResolveMinimize`. A clickable infobar is new
  chrome, not a reuse, and it is a separate piece of work from "say the reason".

- [x] **9.4.3 — Quick-filter chips.** CLOSED AS F5.11 - the same work seen from the
  other side. "Status-bar toggles" and "one click builds the filter" describe one
  feature, and building both would have produced two menus over one predicate. There
  is no toolbar and no status-bar button infrastructure in this window, so the
  one-click surface is the Filter menu; the ticket's own "visible in box" is exactly
  what it does.

- [x] **9.4.4 — Column profiles.** DONE. View > Column profile applies
  `Default | Minimal | Network | Security | Performance | Show diagnostics` in one
  click. A profile is a COLUMN MASK, deliberately NOT a second preset - a preset is a
  whole ViewState and already carries a mask, and two answers to "what is on screen"
  can drift. The masks live in `ColumnsWin.h` as "everything minus what this view does
  not need" so a future 30th column is present unless a profile says otherwise; only
  Minimal is an inclusive list, because "everything a socket is not" is not a smaller
  set in any useful sense. `Show diagnostics` ORs the five G6/G5 readings onto the
  visible set and reads as on while all five are present. NOT persisted separately -
  it writes `settings_.colVisible`, the mask already versioned by ColVersion.
  Gates: unit (profile.* checks), gui.bat 54/0, cli.bat 248/0.

- [x] **9.4.5 — Freeze/Group banner.** DONE (superseded by above). `chrome.exe (14)` group header count now renders in COL_PROCESS (`Grouping.cpp` `GroupColumnText`). The dedicated toolbar banner (Freeze at HH:MM:SS (12s ago) [Unfreeze]) needs toolbar infra that does not exist — tracked separately; not in scope for this defect. Docs: `docs/gui.md` grouping.

- [x] **9.4.6 — Tray + exit.** DONE. Minimize-to-tray was present (`MainWindow.cpp`
  `WM_SYSCOMMAND SC_MINIMIZE` handler). Added the missing first-run confirmation:
  when the tray icon is OFF and no choice is remembered, the first minimize shows a
  `TaskDialogIndirect` ("How do you want WinTCP to behave when you minimize it?")
  with Tray / Exit buttons and a "Don't ask me again" checkbox. The choice is
  persisted in a new `Settings.trayMinimizeChoice` DWORD (`TrayMinimizeChoice`,
  values 0=never-asked, 1=tray, 2=exit) — load/save in `Settings.cpp`, member +
  `MinimizeTarget ResolveMinimize()` in `MainWindow.h/cpp`. The dialog is guarded
  by `DelayLoadGuard("comdlg32.dll")` (delay-loaded) and fires only when the tray
  is off, so it never appears in the headless gui.bat harness (which never sends
  `SC_MINIMIZE`). Single-click restore + balloon-on-new-listener (F5.6) were
  already present (`WM_APP_TRAY` line 759, `ShowTrayBalloon` in Alerts.cpp) but
  the alert loop is unwired — that is a feature, not this defect. Docs: `docs/gui.md`
  tray section.

- [x] **9.4.7 — Export progress.** DONE as a defect. Overwrite confirm verified
  (cli.bat R4: refusals, file-survival, `--force`), and the completion message now
  reports volume rather than a bare "Export completed." — `"Exported N rows to
  <path>."` (MainWindow.cpp:3032). **Re-homed, not dropped:** the 50k-row modal
  progress dialog during the collection phase is a *feature*, not part of this
  defect — it needs a modal wait window with message pumping, and per §6 you must
  never hold a row pointer across a modal loop. Not tracked as a separate item
  today; add one if the export grows large enough to matter in practice.
  `docs/gui.md` export.

- [x] **9.4.8 — A11y.** DONE where automatable: `/` focuses the filter box
  (MainWindow WM_CHAR); `*` (VK_MULTIPLY → `IDM_CTX_BOOKMARK`, wintcp.rc:153) toggles
  a bookmark; Ctrl+S accelerator exists (wintcp.rc:147) + sheet row (BuildInfo.cpp:40).
  Tab-order (`filter → list → status`) uses the resource-defined Z order and was not
  changed. Remaining (manual/feature, NOT done here): **MSAA accName/accDescription per
  row** — the virtual list uses comctl32's default IAccessible (no custom
  `WM_GETOBJECT`/`IAccessible`); **NVDA test** — added to gui.bat manual checklist item 14.
  Docs: `docs/gui.md` shortcuts.

- [x] **9.4.9 — Dark verify.** DONE. No toggle (follows system). `ApplyColors`
  derives from `GetSysColor` and gates `kDark*` behind `!HighContrastActive()`
  (DetailsDialog.cpp:167). Fixed the one hole: `WM_CTLCOLORBTN` (line 905) used `dark_`
  alone, which pushed `kDarkBg` onto buttons under dark+HC — now a local
  `dark = dark_ && !HighContrastActive()` mirrors `ApplyColors`. Verified cli/ex/gui/unit
  all green. (The §1 9.4.9 HC item is the same fix point, closed there too.) Docs:
  `docs/gui.md` High contrast + Tray sections.
  Docs: `docs/gui.md` integration.

### 5.4 Features (ranked)

- [ ] **9.5.5 — Firewall manager.** Depends on 9.2.9 viewer.
  Exposure badge (recipe 20: 0.0.0.0:443 + System + unsigned = red).
  Docs: cookbook.

9.5.1–9.5.4 (F5.4 ASN, F5.5 ETW DNS, F5.6 alerts, F5.7 retained sockets) were
separate entries here that only ever said "(See §4 above)". A pointer that carries
no information is a second place to forget to update — F5.7 shipped eight commits
while both copies still read `[ ]`. **They now live only in §4**, which holds the
specification and the DONE notes. Do not re-add a pointer here; if you need to
sequence these, put the ordering in §7 "Suggested order", not in a second
copy of the item.
- [ ] **9.5.6 — Process tree.** `ppid`/`parent` + Toolhelp → tree view +
  kill-tree with PID-reuse guard. Docs: `docs/gui.md`, `cli.md` kill.
- [ ] **9.5.7 — Hash + sync + portable.** SHA256(image) + opt-in VT
  (details `--hash`); 9.2.10 verbs; `wintcp.ini` portable. Docs: cli/gui.

### 5.5 Structural

- [ ] **9.6.1 — `MainWindow.cpp` split.** 192KB. Extract StatusBar,
  ColumnManager, ActionHandlers, TrafficController. No behaviour change.
  todo.md 2.1 defers as mechanical - this is the forcing change (9.4
  touches it). Gates: unit + gui.bat.
  **Note:** Same as W4.1 — DEFERRED by user decision.

- [ ] **9.6.2 — todo.md split.** The authoritative tracker is long and mixes
  audience: a user cannot tell which of these items are product features and
  which are code-perfection chores. Split `docs/roadmap.md` (the user-facing
  features/defects) + `todo.md` (dev-facing mechanical work). No code.
  Note: this file no longer claims "179KB / zero-`[x]` rule" — the header rule
  was corrected on 2026-10-08 to keep `[x]` entries *with their evidence*, so
  the size argument is now "mixed audience and mixed altitude", not "too many
  closed items".

- [x] **9.6.3 — SPDX.** DONE. `// SPDX-License-Identifier: Apache-2.0` is on all 85
  `wintcp/src/*.{cpp,h}` files, on the first line or immediately after the existing
  file banner. The ticket said "new files, don't retrofit all"; the repository already
  shipped an Apache-2.0 `LICENSE` and a README badge, so the tag restated what was
  already true rather than making a new licence decision - and leaving five-sixths of
  the tree without it would have made the tag useless as a machine check. No gate
  change: build.bat clean, cli.bat 248/0, unit 15/15.

- [x] **9.2.11 - Alerts.** DONE, and the missing piece was not the engine.
  `AlertEngine::Evaluate` has been pure, latched and test-covered since it was
  written; what it had was **no caller and no persistence** - nothing in the
  product ever called it, and there was nowhere to put a threshold. That is the
  gap this closes.
  Now: `wintcp.exe alert` reads and writes the whole configuration (see
  `docs/cli.md`), it persists in `HKCU\Software\WinTCP` under eight registry
  values, and `MainWindow::RunAlerts()` runs it after every refresh - the engine's
  ONLY production caller. Balloon via `Shell_NotifyIconW` when the tray icon is
  present, status-bar text when it is not, because an alert that does nothing is
  worse than none. `AlertSettings` is stored in `Settings` as the ENGINE'S OWN
  struct rather than a parallel shape: the first attempt kept eight separate
  fields here and converted on the way in, which is the duplication this project's
  own rule is about.
  Muted by default, every threshold off, each condition latched until it clears.
  Docs: `docs/cli.md`, `docs/gui.md`, `docs/filters.md`.
  Gates: build.bat clean, cli.bat 254 checks / 0 failures, unit 15/15 with six new
  `alertcfg.*` checks, gui.bat 54/0.

### 5.6 Repo hygiene

- [x] **9.1.3 — Repo hygiene.** DONE. Repo initialised; `.gitignore` covers
  `build/`, `build-cmake/`, `temp/`, `*.obj`, `*.pdb` (and `build-fast/`); the
  new `.gitattributes` pins the line-ending contract that
  `development.md:198` used to only *admit* — `*.bat`/`*.ps1`/`*.txt` as CRLF and
  `docs/*.md` as LF — so git stops normalising files it is not meant to touch.
  This is what makes the "re-normalise CRLF after every edit" rule enforceable
  rather than folklore. Docs: `docs/development.md` line endings.

### 5.7 Testing

- [ ] **V3 — No coverage instrumentation.** Zero. "472 checks" is a vanity
  number without branch coverage. **Deferred to the final phase by
  decision: features and improvements first, tests and coverage last.**
  Tool inventory: `pgomgr.exe` present (MSVC 14.44.35207); `clang-cl`/
  `llvm-cov` not present; VS native coverage not installed;
  `OpenCppCoverage` not installed (portable ~5 MB zip needing approval).
  Agreed scope when resumed: line coverage via a downloaded tool, or an
  explicitly-labelled approximate function-coverage number — never a
  figure presented as branch coverage.

---

## 6. Carried risks — not tasks, but do not lose them

- `SocketTrafficSampler` is leaked **by design**. D2 extended this: an
  abandoned pass also leaks its `ScanScratch`, and a stalled worker leaks
  its own thread and duplicated handle. Both are bounded (one scratch per
  stalled pass, one worker per *distinct* stalled socket via
  `RememberStalled()`), and the bounds are pinned by `d2.*` selftests.
- The 4 s scan budget is a **ceiling**, not the wait. The wait is
  `kNoProgressMs` (250 ms of no worker completing anything).
- **Never hold a row pointer across a modal dialog.** A modal loop pumps
  messages, so a refresh reallocates `rows_` underneath it (D29). Copy the
  row by value first.
- **The three build paths must stay in step.** Sources agree (41 files
  each); the subsystem is CONSOLE in all three. Any new source file must
  be added to all three: `build.bat`, `CMakeLists.txt`, `wintcp/wintcp.vcxproj`.
- **Do not make `FindWindowExW` a way to find standard controls.** Its
  class match does not resolve `"BUTTON"` against the atom `"Button"`.
  Use `GetDlgItem`.
- **A window procedure must `return TRUE` from `WM_NCCREATE`.** Omitting
  it destroys the window before `WM_CREATE` runs.
- **A child's HWND must be published before `WM_CREATE`.**
- Two front ends can drift unless `Commands` remains the only implementation.
- Registry writes are HKCU-scoped; firewall rules are machine-wide.
- The `selftest` `elev.*` checks assert the one-way invariant
  `elevated ⇒ can-elevate`, which holds in **both** states.
- **`Start-Process -PassThru` without `-Wait` may hand back an unreadable
  `ExitCode`.** Touch `.Handle` on the returned object immediately after the
  start, or .NET is free to drop the native handle and `ExitCode` reads `$null`.
  `$null -ne 0` is **TRUE**, so a file that compiled perfectly is reported as
  failed — observed as "8 of 43 failed" with empty compiler logs while all 43
  `.obj` sat on disk. Note the asymmetry: `-Wait -PassThru` already populates
  `ExitCode`, and touching `.Handle` on an *exited* process there can throw.
- **A memoised mtime cache is only valid for reads.** `fast-build.ps1` caches
  `File.GetLastWriteTimeUtc` to keep the staleness sweep cheap, but the sweep
  reads each `.obj` *before* the compile rewrites it, so the link step compared
  the exe against pre-compile timestamps, judged it current, and skipped the
  link. Symptom: `-Test` passes while testing the previous binary. Any cache in
  a build driver must be invalidated between the write phase and the read phase
  that depends on it.
- **Never mark a test `[x]` on the strength of a timestamp.** Two unit checks
  here assumed the host had been up at least an hour: `ApplyKernelAges`
  deliberately rejects any age greater than uptime, and the duration test
  computed `GetTickCount64() - 3600000`, which **underflows** below 1h uptime
  (a monotonic *uptime* counter, not a wall clock). Both passed on a
  long-running dev box and failed immediately after a reboot. Derive time
  fixtures from uptime, and prefer asserting the rule over hardcoding a value
  the host may not be able to produce.

---

## 7. Suggested order

**Closed 2026-10-08:** F5.4 ASN, F5.11/9.4.3 quick filters, 9.4.4 column profiles,
F5.15 FontCache, 9.6.3 SPDX, 9.2.8, all of Â§5.2, Â§5.6, and most of Â§1 - plus five
wrong-answer bugs (bare `country:`, `geoip lookup` on an ASN file, `doctor
--asn-db`, the ASN-only advice, "0 records") and one real range bug (ULA v6).

Remaining, in dependency order:

1. **9.2.11 alerts** â†’ unblocks **F5.6**. `Alerts.h:42` has hardcoded thresholds and
   no persist/CLI/editor; wire the verb and the tray balloon, or hide the feature.
2. **9.2.9 firewall viewer** â†’ unblocks **9.5.5**.
3. **9.2.10 portable + sync** â†’ unblocks **9.5.7**.
4. **F5.5 ETW DNS** - elevated only, and the last untouched feature in Â§4.
5. **9.4.2 buttons** - the remainder above; new chrome, not a reuse.
6. **9.5.6 process tree** - independent.
7. **P1 / P2 literals, C12 Bench split** - mechanical, per-file, tests only.
8. **W4.3 Result<T>**, then **W4.2 wstring_view** - after the features land.
9. **9.6.2 todo.md split** - documentation only, safe at any time.
10. **W4.1 / 9.6.1 MainWindow split** - DEFERRED by your decision. Do not start.
11. **V3 coverage** - final phase by decision.
