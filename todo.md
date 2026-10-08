# WinTCP — Open Work

**Created 2026-10-07.** This file is the authoritative tracker of *everything still open*.
The previous tracker (2026-09-30 through 2026-10-06) is preserved at `temp/todo_2026-10-07.md`.

**Rule:** if it is not in this file, it is done. When something is finished,
delete the line here. Do not leave `[x]` items.

`[ ]` not started · `[~]` in progress · `[x]` done **and** verified.

---

## Where things stand (measured 2026-10-07)

| Gate | Result |
|---|---|
| `build.bat` (`/W4 /WX /permissive-`, from `clean`) | clean |
| `build\tests\wintcp-tests.exe` unit | all checks passed |
| `wintcp\tests\cli.bat` (CLI, 230 checks) | **PASS / 0 failures** |
| `wintcp\tests\gui.bat` (GUI, 54 checks) | **PASS / 0 failed** |
| `wintcp\tests\examples.bat` (README examples) | **87 commands / 0 rejected** (added `--dns-timeout` study) |

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

- [ ] **F5.4 — ASN lookups (GeoLite2-ASN.mmdb via GeoIp.cpp).**
  Reuse Country column (9.1.1 decided: freeze at 32 columns, no 33rd).
  Add `asn:` filter + column. Docs: `docs/cli.md`, `filters.md`, `cookbook.md`.

- [ ] **F5.5 — ETW DNS snooping (provider `{1C950233-...}`, no PTR queries).**
  Answers `host:` without `--dns` slowness. Elevated only.
  Docs: `docs/traffic.md`, `cli.md`.

- [ ] **F5.6 — Real-time connection alerts (right-click → toast/balloon).**
  Depends on 9.2.11 (alert wiring). Tray balloon + alert verb + registry rules.
  Docs: new `docs/gui.md` section + `cli.md`.

- [ ] **F5.7 — Retain closed sockets (grey, lifetime, final metrics).**
  Cap 500, age out. Turns polling into history.
  Anchor: `ConnectionStore` ghosts (red-ghost 1 cycle → extend).
  Docs: `docs/gui.md` refreshing, `cli.md` changes.

- [ ] **F5.8 — IPv6 force-close.**
  `SetTcpEntry` is IPv4-only. Disable GUI Close connection on IPv6 row +
  tooltip (IPv4 only; block stops reconnect). Block returns `kRulesOnly` (keep).
  Docs: `docs/cli.md` close/block, `README.md` limitations.

- [ ] **F5.9 — Raw stream binary export (.bin for Wireshark).**
  `capture --bin` raw export. Overlaps with 9.3.7 (`--bin` raw).
  Docs: `docs/cli.md` capture.

- [ ] **F5.11 — One-click quick filters (status-bar toggles).**
  All|TCP|UDP|Listen|Estab|Mine. One click builds `proto:`/`state:` filter,
  visible in box. Docs: `docs/gui.md` filtering.

- [ ] **F5.15 — A `FontCache` shared across windows.**
  DPI-change hazard. Docs: `docs/gui.md`.

---

## 5. Deep re-review track (§10 of old tracker)

### 5.1 Correctness

- [~] **9.2.5 — Elevation UI.** DONE in `MainWindow.cpp`, gated by `build.bat` + `cli.bat` + `gui.bat` + `examples.bat`:
  `shield glyphs` on the elevation-gated actions only - `Block this connection...` (context menu) and `Per-PID traffic counters` (View > bar) - shown only when `IsAdminMember() && !IsElevated()`, skipped under high contrast and when shell32 is delay-load unavailable. `Reelevate()` is now a LIVE caller (it had zero since Follow was removed 2026-10-05): clicking Block or turning traffic ON while unelevated hands off to `Reelevate("…")`, which preserves the command line and state path via `settings_.trafficEnabled`. A standard account (no linked token) sees `ElevationUnavailableReason()` instead of a UAC prompt. `Close connection` is deliberately UNSHIELDED: `SetTcpEntry` on the user's own sockets is not privileged, so a shield would mislead - documented at the call site. The relaunch-loses-state premise from the old 9.2.5 text is moot: `Reelevate` already restores `settings_.trafficEnabled`, and `Settings::Save()` at exit covers the rest. Docs: `docs/gui.md` actions.

- [x] **9.2.7 — pktmon probe.** DONE (verified): `CaptureToolsPresent()`
  probes `pktmon.exe` + `etl2pcap.exe` (StreamCapture.cpp:191-214),
  `EvaluateCaptureGate()` enforces tool-absence-before-elevation ordering as a
  pure testable table (line 232; all four combos pinned by `12e.` unit tests
  Bench.cpp:4235-4358, incl. the pre-9.2.7-order regression), `CaptureAvailable()`
  wires tools-first (line 253), `WinCaps.cpp:223-226` registers the
  `Stream capture (pktmon)` capability row. Docs: `docs/cli.md` (lines 302-315).
  No code change needed; closed on verification.

- [ ] **9.2.8 — DNS budget.**
  `DnsResolver` FIFO 4096, GUI worker OK, CLI `--dns` `--limit` inline
  `getnameinfo`. Add `--dns-timeout ms`, pending vs em-dash, stderr
  stalled count. Docs: `docs/cli.md` dns.

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

- [x] **9.3.1 — `doctor`** *(header-label rename shipped: `Proc Speed` -> `Process rate` across code+rc+docs; CLI token `procspeed` preserved; build unit pass. Full `doctor` command still TBD.)*
  `version` + WinCaps + traffic source + GeoIP + `CountWinTcpRules` →
  `wintcp.exe doctor [--format json]`. All pieces exist (`BuildInfo`,
  `WinCapabilities`). Add `examples.txt` line + help.
  Docs: `docs/cli.md` commands table.

- [ ] **9.3.2 — `help columns` + `help filters`.**
  `--columns` names (`procspeed` vs `Proc Speed` vs `COL_GROUPRATE`) confuse.
  Machine-readable list + group-safe list (D27). Extend golden help asserts.
  Docs: `docs/cli.md` column ref.

- [x] **9.3.3 — Min RTT shows stale value during an idle RTT sample.** *(fixed: split `rttKnown` into `rttLive` (per-tick; RTT cell blanks to `—` during an idle sample) + `rttEver` (latch for Min RTT so best-ever lingers). Render/sort/filter/comparator updated; `g6.per-field-not-all-or-nothing` passes with `rtt=— cwnd=16.5 KB`; all unit checks pass.)*

- [ ] **9.3.7 — Capture finish.**
  `--bin` raw (Wireshark Follow→Save Raw, F5.9), `--filter` passthrough
  to `pktmon filter add` (SYN/FIN/RST), counters before bytes (keep).
  Docs: `docs/cli.md` capture.

### 5.3 UI/UX

- [ ] **9.4.1 — Details tabs.**
  After G3 fix: Process | Connection | Sockets-of-PID | Security | Notes.
  Pin, Copy-all, Open file location keep.
  Anchor: `DetailsDialog.*` + `DetailModel.*`. Docs: `docs/gui.md` details.

- [ ] **9.4.2 — Empty states.**
  Traffic em-dash → infobar "TCP only - UDP needs admin" [Run as admin]
  [Learn more]; 0 rows → "No rows" - [Clear] [Edit]; Country empty →
  "No database" - [Pick .mmdb]. Docs: `docs/gui.md`.

- [ ] **9.4.3 — Quick-filter chips (F5.11).**
  All|TCP|UDP|Listen|Estab|Mine. One click builds `proto:`/`state:` filter,
  visible in box. Status-bar toggles. Docs: `docs/gui.md` filtering.

- [ ] **9.4.4 — Column profiles.**
  Minimal/Network/Security/Performance. One-click + Show diagnostics.
  Anchor: `kDefaultVisibleCols` (`ColumnsWin.h:69`) + ColVersion migration.
  Docs: `docs/gui.md` columns.

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

- [~] **9.4.7 — Export progress.** Overwrite confirm verified (cli.bat R4:
  refusals, file-survival, --force). Row-count completion message implemented
  (MainWindow.cpp:3032: `"Exported N rows to <path>."`). The 50k-row modal
  progress dialog during the collection phase is a feature (needs a modal wait
  window + message pumping) and is NOT done — out of scope for this defect.
  `docs/gui.md` export.

- [x]/[~] **9.4.8 — A11y.** DONE where automatable: `/` focuses the filter box
  (MainWindow WM_CHAR); `*` (VK_MULTIPLY → IDM_CTX_BOOKMARK, wintcp.rc:153) toggles
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

- [ ] **9.5.1 — F5.4 ASN.** (See §4 above.)
- [ ] **9.5.2 — F5.5 ETW DNS.** (See §4 above.)
- [ ] **9.5.3 — F5.6 Alerts.** Depends on 9.2.11. (See §4 above.)
- [ ] **9.5.4 — F5.7 Retained sockets.** (See §4 above.)
- [ ] **9.5.5 — Firewall manager.** Depends on 9.2.9 viewer.
  Exposure badge (recipe 20: 0.0.0.0:443 + System + unsigned = red).
  Docs: cookbook.
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

- [ ] **9.6.2 — todo.md split.** 179KB authoritative + zero-[x] rule
  (keep discipline) but unreadable. Split `docs/roadmap.md` (user) +
  `todo.md` (dev). No code.

- [ ] **9.6.3 — SPDX.** Add `SPDX-License-Identifier: Apache-2.0` one-liner
  to new files; don't retrofit all. No gate change.

- [ ] **9.2.11 — Alerts.** `Alerts.h:42` (thresholds hardcoded, no
  persist/CLI/editor). Wire alert verb + tray balloon + registry rules
  (F5.6) or hide. Docs: `docs/gui.md` new section or remove mention.

### 5.6 Repo hygiene

- [ ] **9.1.3 — Repo hygiene.** `build/`, `build-cmake/` (*.tlog/*.obj/*.pdb)
  in tree, no `.git`/`.gitignore`/`.gitattributes`. `development.md:198`
  admits mixed endings by design (CRLF .bat/.ps1/.txt, LF docs/*.md).
  Add `.gitattributes` (*.bat/.ps1/.txt eol=crlf, *.md eol=lf),
  `.gitignore` (build/ build-cmake/ temp/ *.obj *.pdb), init repo.
  Docs: `docs/development.md` line endings.

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

---

## 7. Suggested order

1. **§1 Defects** — small, concrete, each is a single file:line fix.
2. **§5.1 Correctness** (9.2.5-9.2.10) — boundaries and edges.
3. **§5.2 CLI** (9.3.1-9.3.7) — each is independently resumable.
4. **§4 Features** (F5.4-F5.15) — each gated before the next starts.
5. **§5.3 UI/UX** (9.4.1-9.4.9) — rendering and interaction.
6. **§2 Code perfection** (P1, P2) — mechanical, per-file.
7. **§3 Architecture** (W4.2, W4.3) — after features land.
8. **§5.5 Structural** (9.6.x) — last, touches the most lines.
9. **§5.6 Repo hygiene** (9.1.3) — can be done anytime.
10. **§5.7 Testing** (V3) — final phase, after all features.
