// MainWindow.h
// Main application window: control bar + virtual ListView (LVS_OWNERDATA)
// + status bar. Owns the RefreshEngine worker, the ConnectionStore model,
// filter/sort/export logic, selection preservation, and column layout.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>   // SelectedCopy: "no selection" and "a row" as one value
#include <set>
#include <string>
#include <vector>

#include "Alerts.h"
#include "BlockConn.h"
#include "ConnectionStore.h"
#include "ChartsWindow.h"
#include "DetailModel.h"
#include "DetailsDialog.h"
#include "DnsResolver.h"
#include "Elevate.h"
#include "EtwTraffic.h"
#include "GeoIp.h"
#include "RefreshEngine.h"
#include "Settings.h"
#include "Presets.h"
#include "Bookmarks.h"
#include "ChangeLogWindow.h"
#include "TypeToJump.h"
#include "SocketTraffic.h"

namespace wintcp {

// Private messages posted by worker threads (payload = registered pointer).
enum : UINT {
    WM_APP_REFRESH_RESULT = WM_APP + 1,   // RefreshResult*
    WM_APP_DNS_RESULT     = WM_APP + 2,   // DnsResolver::Result*
    WM_APP_TRAY           = WM_APP + 3,   // tray icon callback
    // Posted to self when a header drag starts, so the resulting order is read
    // back AFTER the drag has finished (7.1). Posting rather than reading
    // inside LVN_BEGINDRAG is deliberate: at that point the header still holds
    // the OLD order, and there is no end-of-drag notification to hook.
    WM_APP_COLUMNORDER_SYNC = WM_APP + 4,
};

class MainWindow {
public:
    MainWindow() = default;
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Register the window class (call once at startup).
    static bool RegisterClass(HINSTANCE hInstance);

    // Create the top-level window. Returns nullptr on failure.
    HWND Create(HINSTANCE hInstance, int nCmdShow);

    HWND Handle() const { return hwnd_; }

    // In-process UI harness. See UiHarness.cpp.
    //
    // Everything the harness needs is already reachable through
    // HandleMessage() - the same entry point the real WndProc uses - so the
    // harness drives the production WndProc, not a parallel copy of it. What
    // it cannot see is the resulting STATE, because every field is private and
    // friend would not survive: this exists so a test can ask the window
    // itself what it did, rather than reading the controls back from another
    // process and guessing.
    //
    // Returns a pointer to static storage valid until the next call, or
    // nullptr if the op name is not recognised.
    const wchar_t* UiProbe(const wchar_t* op, const wchar_t* arg) const;

    // 5.3: direct access to the change-log window for the UI harness.
    //
    // Not a UiProbe op, because UiProbe is deliberately a string-keyed READ
    // interface: it answers questions about state and performs no actions. The
    // harness needs to SET the event mask and push synthetic events, which is
    // exactly what that interface is shaped to prevent. This is one typed
    // accessor for one test, which is a smaller concession than widening UiProbe
    // into a general command channel.
    ChangeLogWindow* ChangeLogForTest() { return &changeLog_; }

private:
    static const wchar_t* kClassName;
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnCreate();
    void OnSize(int cx, int cy);
    void OnCommand(WORD id, WORD notifyCode, HWND ctl);
    void OnNotify(NMHDR* hdr, LPARAM lParam);
    void SyncColumnOrderFromHeader();   // 7.1: persist header drag-reorder
    bool OnTypeJumpChar(wchar_t ch);    // 7.2: type-a-few-letters navigation
    void OnTimer(UINT_PTR timerId);
    // R8: refresh watchdog, run from the 1 s status tick. Declares the view
    // stale and restarts the worker past the threshold, switches auto-refresh
    // off past the restart cap. Pure policy lives in RefreshWatchdogNext
    // (RefreshEngine.h, selftested); this only executes the verdict.
    void CheckRefreshWatchdog();
    void OnContextMenu(HWND target, int x, int y);
    void OnDestroy();

    void OnRefreshResult(RefreshResult* payload);   // takes ownership if registered
    void OnDnsResult(DnsResolver::Result* payload); // takes ownership if registered

    void CreateControls();
    void LayoutControls(int cx, int cy);
    void RebuildColumns();                 // apply visibility + widths (tasks 16)
    void HarvestColumnWidths();            // physical -> logical (96-dpi) units
    void InitFilterControls();
    void StartWorker();                    
    void StartDnsWorker();                 

    // DPI: logical layout units are 96-dpi pixels; S() scales them.
    int S(int px96) const { return ::MulDiv(px96, static_cast<int>(dpi_), 96); }
    void RecreateFont();                   // Segoe UI 9pt at current DPI
    void ApplyFontToChildren();

    // Change highlighting: custom-draw rows flagged by the diff.
    LRESULT OnCustomDraw(NMLVCUSTOMDRAW* cd);

    // Close connection.
    void CloseSelectedConnection();

    // Theming: the app has no theme of its own and follows the system one,
    // so "dark mode" as a setting is gone. High contrast (WO_HC_ACTIVE) is
    // picked up from the system automatically - see RefreshSystemColors().
    bool HighContrastActive() const;
    bool ThemeIsDark() const;      // system theme is dark (for highlights)
    void RefreshSystemColors();    // WM_THEMECHANGED / WM_SYSCOLORCHANGE

    // Tray icon + minimize-to-tray.
    void TrayAdd();
    void TrayRemove();
    void ShowTrayMenu();

    // Reverse-DNS plumbing.
    void OfferDnsForAllRows();
    void OfferGeoIpForAllRows();           // 4.3: fill Country from the MMDB
    void LoadGeoIpDatabase();              // 4.3: pick a .mmdb, load, refresh

    // Filter + sort + repaint the virtual list, preserving selection/scroll.
    void ApplyView();                      // captures selection from current view
    void ApplyViewWith(const std::vector<std::uint64_t>& ids,
                       std::uint64_t focusedId, int topIdx);
    void CaptureSelection(std::vector<std::uint64_t>& ids,
                          std::uint64_t* focusedId, int* topIdx) const;
    void RestoreSelection(const std::vector<std::uint64_t>& ids,
                          std::uint64_t focusedId, int topIdx);
    void UpdateSortIndicator();            
    void UpdateStatusBar(const std::wstring& errorText);
    // Status-bar panes 3 and 4, factored out so the countdown stays a pure
    // function of the last refresh tick and the auto-refresh interval.
    std::wstring SelectedSummary() const;
    std::wstring RefreshAgeText() const;

    void Refresh(bool reportErrors);
    void ExportCsv();                     // -> DoExport(false)
    void DoExport(bool selectionOnly);    // CSV/TSV/JSON + last dir
    std::wstring SelectedRowsText(bool copyAll) const;
    void CopySelectionToClipboard(bool copyAll);
    void KillSelectedProcess();            // PID-reuse verification
    void ShowDetailsOfSelectedRow();       // modeless details window
    // REMOVED 2026-10-05 (todo.md 8.7 G2). The capability moved to the CLI as
    // `capture --text` / `capture --out`.
    // void FollowSelectedStream();        // pktmon + hex view
    void OpenSelectedFileLocation();      // 4.6: ShellExecute "explore"
    // REMOVED 2026-10-05 (todo.md 8.7 G1): the `properties` shell verb cannot
    // be invoked with a bare path, so this action never worked.
    // void ShowSelectedProcessProperties(); // 4.6: ShellExecute "properties"
    void BlockSelectedConnection();       // 4.4: firewall block
    // sectioned builder + silent live-update while open.
    DetailModel BuildDetails(const Connection& c) const;
    void RefreshDetailsWindow();           // no-op unless details visible
    void ShowAboutBox();
    void ShowShortcutsSheet();

    // 5.2 presets + 5.3 bookmarks, wired to the File and Process menus.
    PresetView CurrentPresetView() const;
    void ApplyPresetView(const PresetView& v);
    void SavePreset();
    void LoadPreset();
    void DeletePreset();
    void ShowBookmarksList();
    void RefreshBookmarkMarks();            // 5.3: registry -> row model
    void BookmarkSelectedConnection();
    void EditSelectedBookmarkNote();
    void ToggleColumn(int columnId);       
    void SyncColumnMenuChecks();           // View > Columns ticks track visibleCols_
    bool AnyTrafficColVisible() const;     // TRAFFIC/RX/TX/NETTOTAL on screen?
    void EnsureTrafficCounters();          // ETW try + socket fallback

    // Change log: append APPEAR/DISAPPEAR/STATE lines to a CSV
    // file the user picked; persists across restarts via Settings.
    void ToggleChangeLog();
    bool OpenLogFromSettings();            // reopen at startup (best effort)
    void WriteChangeLog();                 // drains store_.TakeChangeEvents()

    // Settings: loaded in OnCreate, saved in OnDestroy.
    void SaveSettings();

    // Keyboard UX: handlers for the Edit menu / accelerators.
    void SelectAllRows();
    void FocusFilterBox();
    void ClearFilterBox();

    std::wstring CurrentSearchText() const;
    unsigned CurrentProtoFilterMask() const;   // kProtoMask* combination
    DWORD CurrentStateFilter() const;          // kAllStates or MIB_TCP_STATE_*

    // First selected connection (nullptr when nothing is selected).
    //
    // The POINTER form. It stays for the two call sites that legitimately want
    // the live row and provably outlive it (the context-menu builder, and the
    // read-only probes). Every handler that can open a dialog, a prompt or any
    // other message pump must use SelectedCopy() instead - see there.
    const Connection* FirstSelectedRow() const;

    // The COPY form, for every handler that hands control to a modal loop.
    // Returns false when nothing is selected, and writes the row BY VALUE.
    //
    // Why this exists: a refresh arriving during a modal loop replaces rows_
    // and frees the row the pointer referred to, so a handler that kept
    // `const Connection* c` across a MessageBox read freed memory. D29 was
    // fixed at three such sites individually; the fix did not stick because
    // the safe pattern was not the easy one. This makes it the easy one: one
    // call, and the lifetime hazard is closed by construction rather than by
    // remembering a rule at each new site.
    //
    // std::optional rather than a bool + out-param because "no selection" and
    // "a row" are one value, not two, and the caller cannot forget to check.
    std::optional<Connection> SelectedCopy() const;

    bool SelectedRowIndices(std::vector<int>& out) const;

    int VisibleToCol(int visibleIndex) const;   // -1 when out of range
    int ColToVisible(int columnId) const;       // -1 when hidden

    // members ----------------------------------------------------------------
    HINSTANCE hInstance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND hwndList_ = nullptr;
    HWND hwndStatus_ = nullptr;
    HWND hwndRefreshBtn_ = nullptr;
    HWND hwndExportBtn_ = nullptr;
    HWND hwndAutoChk_ = nullptr;
    HWND hwndIntervalCbo_ = nullptr;
    HWND hwndProtoCbo_ = nullptr;
    HWND hwndStateCbo_ = nullptr;
    HWND hwndSearchEdit_ = nullptr;

    ConnectionStore store_;
    RefreshEngine engine_;
    DnsResolver dns_;
    EtwTraffic etw_;                      // per-PID traffic
    // non-admin per-PID traffic. Heap-allocated and never freed: a
    // scan thread wedged in SIO_TCP_INFO can outlive the window, and it holds
    // a pointer to its sampler, so the sampler must outlive it too. See
    // ~SocketTrafficSampler for the reasoning; `this` is never deleted, which
    // is a deliberate one-object leak at process exit.
    SocketTrafficSampler* socketTraffic_ = nullptr;
    // Whether the socket fallback (not ETW) feeds the traffic columns.
    // Heap-owned and shared with the refresh worker: after Stop() detaches
    // the worker, a late pass may still read this, so it must not die with
    // the window. Read by the worker thread -> atomic.
    std::shared_ptr<std::atomic<bool>> fallbackFlag_ =
        std::make_shared<std::atomic<bool>>(false);
    DetailsDialog details_;
    ChartsWindow charts_;                    // performance graphs
    DWORD detailsPid_ = 0;                   // PID the details window shows
    DetailModel detailsModel_;               // last content shown (7.18)
    bool detailsExitedNoted_ = false;        // exit note shown once
    Settings settings_;
    std::vector<FilterClause> filterProgram_;

    // change log: append-mode handle while logging is active
    HANDLE logFile_ = INVALID_HANDLE_VALUE;
    std::wstring logPath_;

    // worker -> UI payload registry. Everything a worker-thread callback
    // touches lives in this heap block, never in the window: if Stop()
    // detaches the worker, its late pass still holds a shared_ptr to the
    // sink, so the callback can run to completion against freed-window-safe
    // state (a PostMessage to the dead hwnd simply fails and the payload is
    // deleted). This is what makes the bounded-join detach provably harmless
    // rather than hopefully so. mutable so the const UiProbe accessor can
    // read the pending counts.
    struct WorkerSink {
        std::mutex m;
        std::set<RefreshResult*> pendingRefresh;
        std::set<DnsResolver::Result*> pendingDns;
        bool shuttingDown = false;
        HWND hwnd = nullptr;
    };
    mutable std::shared_ptr<WorkerSink> sink_;
    // Lazy owner: created on first use so a window that never starts its
    // workers still destroys cleanly.
    std::shared_ptr<WorkerSink> Sink() const;

    bool reportErrorsNextResult_ = false;
    bool dnsEnabled_ = false;              // View > Resolve hostnames
    bool topMost_ = false;                 // View > Always on top
    bool trayEnabled_ = false;             // View > Tray icon
    bool trayIconShown_ = false;
    bool etwAutoAttempted_ = false;        // at most one implicit ETW start
    NOTIFYICONDATAW trayNid_ = {};
    HBRUSH hwndBrushWindow_ = nullptr;   // WM_CTLCOLOR* backdrop (system)
    std::wstring lastError_;
    std::wstring lastRefreshTime_;
    // GetTickCount64 of the last completed snapshot; drives the status bar's
    // age and next-refresh countdown. 0 = nothing has completed yet.
    ULONGLONG lastRefreshTick_ = 0;
    // R8 liveness stamp: GetTickCount64 of the last POSTED result, success or
    // error — either proves the worker alive. lastRefreshTick_ stays
    // success-only (it drives the display age); this one drives the watchdog.
    ULONGLONG lastResultTick_ = 0;
    // Watchdog state (R8). workerStartTick_ seeds the age before the first
    // result; the rest is the restart state machine in CheckRefreshWatchdog.
    ULONGLONG workerStartTick_ = 0;
    bool watchdogRestarting_ = false;
    ULONGLONG watchdogRestartTick_ = 0;
    int watchdogRestarts_ = 0;

    // DPI
    UINT dpi_ = 96;
    HFONT font_ = nullptr;

    // columns
    UINT32 visibleCols_ = 0;               // bitmask over ColumnId
    int colWidths_[COL_COUNT] = {};
    std::vector<int> visToCol_;            // visible index -> ColumnId
    // The user's column order (7.1): a permutation over ALL ColumnIds, seeded
    // from kDisplayOrder and rewritten by OnColumnDrag. A vector, not an array,
    // so erase/insert cannot leave a stale tail.
    //
    // Sized at CONSTRUCTION from COL_COUNT. OnCreate writes all COL_COUNT
    // entries into it, and subscripting an empty vector writes through a null
    // data pointer - which is an access violation on the very first line of
    // startup, and only in a release build with the accesses inlined. The
    // crash showed up as a fault in SaveSettings' colWidths copy loop because
    // the optimiser hoisted and merged the two identical loops.
    std::vector<int> colOrder_ = std::vector<int>(COL_COUNT, 0);

    // 7.4: keep the selection across auto-refresh ticks. On by default -
    // a row the user is inspecting must not jump to the top of the list every
    // two seconds - but on a busy machine the tracked row can end up far down
    // the view, so it is a toggle rather than a hard-coded behaviour.
    bool preserveSelection_ = true;

    // 7.2 type-to-jump state.
    TypeToJump typeToJump_;

    // 5.5 freeze. When set, auto-refresh keeps sampling but the list is not
    // replaced, so the user can read a table without it changing under them.
    bool frozen_ = false;
    ULONGLONG frozenAt_ = 0;

    // 5.1 group rows by process. Changes the SHAPE of the list only; the
    // underlying rows are untouched, so toggling it off restores the flat
    // list exactly.
    bool groupByProcess_ = false;

    // 5.4 change-log window. Fed from the SAME drain the file writer uses -
    // TakeChangeEvents is destructive, so there is exactly one drain and both
    // consumers get the batch. See WriteChangeLog().
    ChangeLogWindow changeLog_;

    // 4.3 GeoIP. Loaded lazily from the user-supplied .mmdb; never
    // downloaded. The module existed and was fully tested but was never
    // actually consulted, so the Country column was permanently empty - a
    // 778-line feature wired to nothing.
    GeoIpDatabase geo_;

    wchar_t dispBuf_[kMaxColumnText] = {0};  // LVN_GETDISPINFO scratch buffer

    std::wstring filterDebounceText_;
    bool autoRefresh_ = false;
    UINT autoRefreshMs_ = kDefaultIntervalMs;
};

}  // namespace wintcp
