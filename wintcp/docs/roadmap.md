# WinTCP — GUI Roadmap

This list is the specification of the G1-G36 GUI polish work, kept as
written when the items were filed. **All 36 shipped** - see Closed below
for the commit that closed each group. The line:column references
describe the state at filing time and are historical.

## GUI polish

**Defects**

- **G1 — double separator in the row context menu [Low, cosmetic, CONFIRMED].**
  `MainWindow.cpp` `OnContextMenu` appends `MF_SEPARATOR` at
  ~3485 and again at ~3507 with only removed-item comments
  between them, so the menu shows two bars with nothing between.
  Drop the first separator.
- **G2 — Details tabs mis-measured (the "not aligned like other
  apps" report).** `DetailsDialog.cpp` measures every tab slot
  with `headerFont_` (bold, 125%) in `RebuildLayout` but paints
  the label with `font_` (regular 9 pt) in `PaintLine` — the
  `Line::Tab` kind falls through to `font_`. Slots are sized from
  the bold face while the text draws in the regular one, so tabs
  are unevenly spaced and the active highlight is wider than its
  label. Measure and paint with the same face.
- **G3 — section-header hover is invisible; no collapse affordance.**
  `DetailsDialog.cpp` `PaintLine` (~640-647) draws the hover
  feedback with `brushSection_` — the same colour as the section
  band, so nothing changes — and the rect x is `S(kMargin - S(2))`,
  a nested-scale arithmetic bug. There is also no `[-]`/`[+]`
  chevron, so a reader cannot tell a section is collapsible.
- **G4 — `PromptDialog` drops its label [Medium-High, CONFIRMED].** `PromptDialog.cpp`
  `(void)label;` (~400) discards the instruction the caller
  passes (`L"Note:"`, …), so every prompt is a bare title + edit
  box with no explanation. Render the label as a static line.
  Worse: Load/Delete preset (`MainWindow.cpp` ~4549-4555, ~4595-4599)
  builds the numbered preset list into a local and never shows it,
  so the prompt also loses what the numbers mean.
- **G5 — Details window has min/max buttons [Low, style, CONFIRMED as fact].** `DetailsDialog.cpp`
  `Create()` uses `WS_OVERLAPPEDWINDOW`, which includes
  `WS_MINIMIZEBOX|WS_MAXIMIZEBOX` on a read-only detail pane.
  Use `WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME`.
  Harmless for an owned modeless window; maximize helps long tables.
- **G6 — control bar overflows a narrow main window [Medium-Low, CONFIRMED].**
  `MainWindow.cpp` `LayoutControls` places the State combo at a
  fixed `kMargin + S(612)` (`MainWindow.cpp:2149,2151`); below 760 px
  client width (not ~700) it runs off the client edge — `cx` is ignored
  for row 1 and there is no `WM_GETMINMAXINFO`. Right-anchor the bar,
  wrap it, or enforce a scaled min width. Fix together with G16.

**Refinements**

- **G7 — tab hover colour equals the active colour.** Hovered
  inactive tab paints `clrHeader_`, the same as the active tab
  (`DetailsDialog.cpp` `PaintLine`) — hover reads as selection.
  Use an intermediate tint.
- **G8 — hand-drawn scrollbar in the Details body.** Non-native
  thumb/track, no hover/press/keyboard states, inset from the
  right edge. Prefer a real scrollbar child (or a scrollable
  control).
- **G9 — no per-row process icons** in the main list (Task Manager
  and Process Explorer show the exe icon; it is the fastest
  recogniser in the list).
- **G10 — main ListView has no hover highlight or banding**; only
  selection shows. Add `LVS_EX` hover tracking / subtle banding.
- **G11 — Charts window two-line panels (Disk, Network) have no
  legend [Medium, CONFIRMED]**, so the two line colours are unexplained;
  one ceiling value only, no x/y ticks or titles; GDI `PS_SOLID` pens,
  no anti-aliasing (`ChartsWindow.cpp:566-572,602-616,633-644,648-678`).
  Add a colour key; consider per-series scale (shared max dwarfs the
  small series) and a 64 KB-floor note.
- **G12 — dark-mode palettes diverge [Low-Medium, PARTIAL].**
  Base `0x202020`/`0xF0F0F0` is actually identical in
  `DetailsDialog.cpp:116-117` and `ChartsWindow.cpp:68-69` (copy-paste,
  not shared). Real divergence: Charts grid/dim/line constants vs
  Details `Mix()` brushes vs main window live `GetSysColor`
  (`MainWindow.cpp:5187-5223`) + inline flash colours (`4138-4172`),
  and Charts has no high-contrast branch. Fix: one shared palette
  header + `HighContrastActive()` + `GetSysColor` derivation everywhere.
- **G13 — About box and shortcuts sheet are plain `MessageBoxW`**
  — no formatting, no icon on shortcuts, no monospace. A proper
  dialog each.
- **G14 — Blocked-peers dialog is a plain list box** (no column
  header, no sorting) with tiny 76×16 buttons and text-suffix
  state markers (`[disabled]`).
- **G15 — empty state is only a status-bar line.** Modern apps
  show an in-list banner with the action that clears it (e.g.
  "Turn on traffic monitoring").
- **G16 — the two filter-row combos (Protocol, State) have no
  visible captions**; only "Filter:" is labelled.
- **G17 — no keyboard navigation between Details tabs** (Ctrl+Tab
  / arrow keys); section headers keep the arrow cursor instead of
  a hand cursor. Body is also mouse/wheel-only: `WM_KEYDOWN`
  (`DetailsDialog.cpp:1087-1092`) handles only `VK_ESCAPE`, so
  keyboard users cannot scroll. Fix with G8 (real scrollbar child
  fixes scroll + keys); add hand cursor via `WM_SETCURSOR`.

**Safety / accessibility / feedback gaps **

- **G18 — Blocked-peers Delete / Remove-all has no confirmation.**
  `BlockedPeersDialog.cpp:286-331` acts on Delete/Remove-all after only
  a "select a rule first" guard, while End-process/Block/Close in
  `MainWindow.cpp:3856,3995,4033` demand `MB_YESNO`. Add
  `MB_YESNO + MB_DEFBUTTON2` naming the rule (and count for Remove-all).
- **G19 — `Del` is bound to killing a process.**
  `wintcp.rc:174` maps `VK_DELETE` to `IDM_CTX_KILL_PROCESS`. A stray
  keypress opens End-process on the selection. Unbind Del from kill;
  document `kill <pid> --yes` as the scriptable path.
- **G20 — unsafe default buttons are inconsistent.**
  End-process (`MainWindow.cpp:3856`) and Block (`3995`) use
  `MB_DEFBUTTON2`, Close-connection (`4033`) and Remove-all-blocks
  (`4085`) omit it, so Enter confirms the destructive path. Add
  `MB_DEFBUTTON2` to every YESNO confirm.
- **G21 — owner-drawn surfaces are invisible to screen readers.**
  `DetailsDialog.cpp:731-821` and `ChartsWindow.cpp` paint via GDI with
  no `WM_GETOBJECT`/MSAA/UIA (zero hits repo-wide). Expose
  `DetailModel::ToPlainText` as the accessible name; give Charts a
  keyboard-reachable values table. Needed for SR users and UIA agents.
- **G22 — Esc semantics differ per window; Cancel lies.**
  Details hides on Esc, ChangeLog/Charts ignore Esc (Alt+F4/X only),
  `BlockedPeersDialog.cpp:278-281` maps Esc/Cancel to `kChanged` so
  `MainWindow.cpp:2869` writes "Firewall rules changed". Make Esc =
  hide/no-change everywhere.
- **G23 — high-contrast handling exists only in two windows.**
  `MainWindow.cpp:4129` and `DetailsDialog.cpp:158-187` gate on
  `HighContrastActive()`; Charts/ChangeLog/BlockedPeers/Prompt have no
  HC branch and Charts uses fixed palettes (`ChartsWindow.cpp:65-73`).
  Extend the HC + `GetSysColor` pattern and forward `WM_SYSCOLORCHANGE`.
- **G24 — main window has no minimum size.**
  Details/Charts/ChangeLog enforce `WM_GETMINMAXINFO`; `MainWindow.cpp`
  has none (only restore-floor constants). Shrinkable to zero with G6
  overflow. Add scaled `ptMinTrackSize` (~760x420, the G6 threshold).
- **G25 — no in-flight refresh feedback.**
  `MainWindow::Refresh:1434` posts `engine_.Request()`; status shows only
  last-completed age (`UpdateStatusBar:2563-2567`). A slow enum/DNS stall
  looks idle. Show "Refreshing…" on Request, clear on result.
- **G26 — tray-hide is undiscoverable and lossy.**
  Single `WM_LBUTTONUP` restores (`MainWindow.cpp:848`, docs say
  double-click), tip is forever `L"WinTCP - TCP/UDP Connections"`
  (`5244`), nothing explains where the window went. Dynamic tooltip
  (rows/frozen/grouped/alerts) + first-hide balloon naming restore.
- **G27 — grouped/frozen modes show only in a status pane.**
  Toggles write only a menu check (`MainWindow.cpp:2887,2893`) + small
  `FROZEN <age>` text (`2562`). Caption/headers/exports never say rows
  are per-process aggregates or stale. Add caption suffix and a
  grouped-header label; mirror in copy headers for CLI parity.
- **G28 — persisted layout has no Reset.**
  Mask/order/widths/sort/filter persist (`OnCreate:987-1015`); last-column
  guard exists (`ToggleColumn:2042`) but a mangled layout recovers only via
  registry. Add `View > Reset columns & layout`.
- **G29 — `Filter > Custom... F3` promises a dialog it never opens.**
  `wintcp.rc:46` carries ellipsis + F3, `IDM_QFILTER_CLEAR`
  (`MainWindow.cpp:2834`) just focuses the filter box. Rename to "Focus
  custom filter" or ship the dialog; reserve F3 for find-next.
- **G30 — ChangeLog Clear destroys unconfirmed; Copy fails silently.**
  `ChangeLogWindow::Clear:539` / `OnCommand:880` wipes 5000 rows with no
  prompt; `CopySelection:637-640` returns silently on no selection; no
  Ctrl+A/Esc. Confirm Clear, fall back to copy-all with count, add
  Select-all + Esc-hide.
- **G31 — control bar has zero tooltips/accessible names.**
  No `TTM_*`/`TOOLINFO` in `MainWindow.cpp`; only filter edit has a cue
  banner, only pane 5 has a tooltip (`2611`). Add tips + `accName` to
  Refresh/Export/combos/filter; extend `SB_SETTIPTEXTW` to every pane.
- **G32 — per-block modal success punishes real use, hides CLI parity.**
  `BlockSelectedConnection:4005` pops one MessageBox per block and names
  no `wintcp.exe block/kill/close --yes` equivalent. Report success on the
  status bar (modal on error/partial only) and cite the CLI verb in each
  confirm.
- **G33 — Details hard-codes fonts outside FontCache.**
  `DetailsDialog.cpp:333,338` bakes `Consolas`/`Segoe UI` via
  `CreateFontIndirectW`; other windows use `FontCache::Get()`
  (`MainWindow.cpp:2110`, `ChartsWindow.cpp:231`). Route via cache with
  fallback stack (feeds G2-style mismeasure when substituted).
- **G34 — Charts window is display-only.**
  No keyboard/SR values, no in-window Export/Copy though `ChartExport.cpp`
  exists. Add focusable panels + values table + `&Export…` calling the
  existing exporter (also labels series, easing G11).
- **G35 — secondary-window empty states are blank.**
  Empty Blocked-peers listbox, zero-event ChangeLog (`UpdateStatus:797`),
  pre-baseline Charts (`collecting…`) offer no next action, unlike main
  `UpdateEmptyState:1778` + `ShowEmptyStateActions:1813`. Reuse the
  message-and-button pattern.
- **G36 — dotted leaders almost never draw; logical/physical mix at DPI.**
  `DetailsDialog.cpp:667+671` (`labelRect.right = left`, `want.cx < right`
  fails) leaves `dots` empty; `totalContentHeight_` logical vs
  `viewHeight_/headerBottom` physical skews thumb/scroll/click off-96dpi
  (`355-358,574-576,785,807-809,856`). Fix as part of G8.

## Open

Nothing is open. The GUI polish list above (G1-G36) shipped in three commits
- `42129c5` (main window), `1d78b9a` (details), `f4e9d86` (secondary
windows) - and the G21 MSAA wrapper completed it in `c81d6cb`. The GUI
process-tree view shipped in `675017e`. Every item was verified in code
and gated: `build.bat` clean, `cli.bat` 268/0, `examples.bat` 116/0,
`gui.bat` 56/0, unit 1052/0.

## Closed

- **G1-G36 — GUI polish.** Closed 2026-10-11. Three batch commits plus the
  G21 completion: the main-window batch (control-bar layout, per-row icons,
  hover/banding, empty-state banner, tray tooltip, captions, tooltips,
  reset-layout, confirms, `MB_DEFBUTTON2` everywhere), the details batch
  (tab measure/paint parity, section hover + chevron, native scrollbar,
  DPI-correct scroll math, dotted leaders, FontCache faces, keyboard
  navigation, shared `Theme.h` palette), and the secondary-windows batch
  (prompt labels with preset lists, chart legends/export/copy/values,
  blocked-peers report view with sort + confirms, ChangeLog confirms,
  empty states, Esc semantics). Docs: `docs/gui.md` per behavior change;
  `wintcp/tests/gui.bat` manual checklist for the UI-only surfaces.

- **G21 — per-row accessible names.** Closed 2026-10-11 (`c81d6cb`). A
  delegating `IAccessible` (`RowAccessible.{h,cpp}`) serves row
  name/description from the store and forwards the other 26 methods to
  comctl32's own object, so roles, states, navigation and hit-testing
  stay exactly what the list control reports. Wired by a list subclass,
  because `WM_GETOBJECT` for a child control never reaches the parent.
  6 `g21.acc.*` unit checks pin the pure text builders; 2 SMOKE checks
  read them back through the `accRow` probe.

- **The GUI process-tree view.** Closed 2026-10-11 (`675017e`). View >
  Process tree opens a tree window over the pure `ProcessTree` engine
  (the `killtree` verb's core), so the process hierarchy is browsable
  the way Task Manager's Processes tab shows it. New-file registration
  in all build paths.

- **F5.5 — ETW DNS snooping.** Closed 2026-10-10 by taking the alternative the
  ticket itself recommended, rather than by implementing the ETW route: a
  persistent cache of the PTR answers the tool already earns, so `--dns` reads
  `%APPDATA%\WinTCP\dns-cache.txt` for a peer an earlier run already saw. No
  ETW, no elevation, works everywhere. The ETW route is not disproved - a
  machine where `Microsoft-Windows-DNS-Client` delivers events would still have
  to prove its field layout - but nothing is blocked on finding one, because
  the cache already answers the question the feature was asking. `todo.md`
  holds the finding that made the ETW route unimplementable here, so it stays.

## Recommended order (complete - kept for history)

1. **Consistency polish: G12+G23, G11+G34, G13, G2, G7, G9, G5, G1, G28, G33.**
2. **Functional defects: G4 (incl. preset-list loss), G8+G36+G17/G21, G14, G6+G16+G24, G3.**
3. **The GUI tree view**
4. **Feedback/discoverability: G15+G35, G25-G27, G29-G32, G24, G10.**
5. **Safety first: G18-G22** (confirms, Del binding, defaults, Esc, SR names).