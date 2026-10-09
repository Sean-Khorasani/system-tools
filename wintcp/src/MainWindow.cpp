// MainWindow.cpp
// SPDX-License-Identifier: Apache-2.0
// Main window implementation: controls, virtual ListView (LVS_OWNERDATA),
// worker-thread refresh plumbing, filter (debounced, expression-aware),
// sort (with header indicators), selection preservation, export.

#include "MainWindow.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>            // SHGetStockIconInfo: SIID_SHIELD (9.2.5)
#include <tcpmib.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <unordered_map>
#include <unordered_set>

#include "ProcessInfo.h"
#include "TcpTable.h"
#include "Utils.h"
#include "WinCaps.h"   // DllAvailable: shell32.dll is delay-loaded
#include "Version.h"
#include "BuildInfo.h"     // About summary (7.5) + shortcut sheet (7.6)
#include "Commands.h"      // abstract layer: BuildDetailModel, preset helpers
#include "Freeze.h"        // FrozenAgeMs (5.5)
#include "ViewState.h"     // the single place a view is built (stage 1.7)
#include "PromptDialog.h"  // one-line text prompt (5.2 / 5.3)
#include "FontCache.h"
#include "Alerts.h"     // F5.6: AlertEngine, ShowTrayBalloon     // F5.15: shared, DPI-correct fonts
#include "resource.h"

namespace wintcp {
namespace {

const UINT_PTR kRefreshTimerId = 1;     // auto-refresh tick -> worker request
const UINT_PTR kFilterTimerId  = 2;     // filter debounce
const UINT_PTR kStatusTimerId  = 3;     // 1 s tick for the status-bar clock
const UINT   kFilterDebounceMs = 250;   // B4: dev constant. One keystroke-delay
                                         // on any machine built this century.
const UINT   kStatusTickMs = 1000;

// Unit conversions (P1). Six sites spelled 1000 as "milliseconds per second"
// and three as 96 = "DPI at which the design pixel sizes are unscaled". Named
// because the failure mode of getting them wrong is a factor-of-1000 age or a
// window a third the intended size at 200% scaling, and neither looks like a
// bug where it is shown.
constexpr ULONGLONG kMsPerSecond = 1000;
constexpr int kDpiUnscaled = 96;

// The layout is authored at an unscaled 1080x640 and multiplied by the system
// DPI ratio (S() does that). Named because the pair appeared TWICE - once as
// the MulDiv fallback in Create() and once in the WM_CREATE handler calling
// LayoutControls - and the two must agree or the first painted frame has a
// different geometry from every later one.
constexpr int kDesignWidth = 1080;
constexpr int kDesignHeight = 640;

// What the filter box holds. One constant for BOTH ends of the round trip: the
// control's EM_LIMITTEXT (what the user may type) and the buffer
// CurrentSearchText() reads into. See the create site for why they must agree.
constexpr int kSearchTextMaxChars = 512;

// Themed colors: the app follows the system theme. High-contrast mode
// (WO_HC_ACTIVE) wins over everything - see RefreshSystemColors().
struct ColDef { int id; const wchar_t* title; int width; int fmt; };
const ColDef kColumns[COL_COUNT] = {
    // Header text comes from ConnectionStore::ColumnTitle so the GUI, CSV
    // export and CLI cannot drift apart; only width/format live here.
    { COL_PROTO,   ConnectionStore::ColumnTitle(COL_PROTO),   70, LVCFMT_LEFT  },
    { COL_LOCAL,   ConnectionStore::ColumnTitle(COL_LOCAL),  150, LVCFMT_LEFT  },
    { COL_LPORT,   ConnectionStore::ColumnTitle(COL_LPORT),   65, LVCFMT_RIGHT },
    { COL_REMOTE,  ConnectionStore::ColumnTitle(COL_REMOTE), 150, LVCFMT_LEFT  },
    { COL_RPORT,   ConnectionStore::ColumnTitle(COL_RPORT),   65, LVCFMT_RIGHT },
    { COL_STATE,   ConnectionStore::ColumnTitle(COL_STATE),  110, LVCFMT_LEFT  },
    { COL_PID,     ConnectionStore::ColumnTitle(COL_PID),     65, LVCFMT_RIGHT },
    { COL_PROCESS, ConnectionStore::ColumnTitle(COL_PROCESS),150, LVCFMT_LEFT  },
    { COL_SERVICE, ConnectionStore::ColumnTitle(COL_SERVICE),130, LVCFMT_LEFT  },
    { COL_HOST,    ConnectionStore::ColumnTitle(COL_HOST),   180, LVCFMT_LEFT  },
    { COL_PATH,    ConnectionStore::ColumnTitle(COL_PATH),   300, LVCFMT_LEFT  },
    { COL_TRAFFIC, ConnectionStore::ColumnTitle(COL_TRAFFIC),130, LVCFMT_LEFT  },
    { COL_RX,      ConnectionStore::ColumnTitle(COL_RX),      88, LVCFMT_RIGHT },
    { COL_TX,      ConnectionStore::ColumnTitle(COL_TX),      88, LVCFMT_RIGHT },
    { COL_NETTOTAL, ConnectionStore::ColumnTitle(COL_NETTOTAL),100, LVCFMT_RIGHT },
    { COL_CPU,     ConnectionStore::ColumnTitle(COL_CPU),     70, LVCFMT_RIGHT },
    { COL_MEM,     ConnectionStore::ColumnTitle(COL_MEM),    100, LVCFMT_RIGHT },
    { COL_DISK,    ConnectionStore::ColumnTitle(COL_DISK),   110, LVCFMT_RIGHT },
    { COL_DURATION, ConnectionStore::ColumnTitle(COL_DURATION), 80, LVCFMT_RIGHT },
    { COL_BANDWIDTH, ConnectionStore::ColumnTitle(COL_BANDWIDTH),140, LVCFMT_LEFT },
    { COL_TLS,     ConnectionStore::ColumnTitle(COL_TLS),   190, LVCFMT_LEFT  },
    { COL_COUNTRY, ConnectionStore::ColumnTitle(COL_COUNTRY), 90, LVCFMT_LEFT  },
    { COL_PINNED,  ConnectionStore::ColumnTitle(COL_PINNED),  90, LVCFMT_LEFT  },
    // G6, the `ss -i` columns. Widths are the GUI's own budget rather than
    // Columns.h's cap: the list view scrolls horizontally and each column is
    // independently resizable, so there is no table to keep aligned here. Right
    // aligned, because every one of these is a number.
    { COL_RTT,     ConnectionStore::ColumnTitle(COL_RTT),     80, LVCFMT_RIGHT },
    { COL_MINRTT,  ConnectionStore::ColumnTitle(COL_MINRTT),  90, LVCFMT_RIGHT },
    { COL_CWND,    ConnectionStore::ColumnTitle(COL_CWND),   100, LVCFMT_RIGHT },
    { COL_RETRANS, ConnectionStore::ColumnTitle(COL_RETRANS),100, LVCFMT_RIGHT },
    // G5. Wider than COL_BANDWIDTH because the value is a process total and so
    // carries more digits - the same shape, a larger number.
    { COL_GROUPRATE, ConnectionStore::ColumnTitle(COL_GROUPRATE),160, LVCFMT_LEFT },
    // 5.5. The bookmark note. Wider than most because the value is prose the
    // user typed, not a code or a number; anything longer elides and the
    // tooltip shows it whole.
    { COL_NOTE,    ConnectionStore::ColumnTitle(COL_NOTE),  180, LVCFMT_LEFT },
  // F5.1. 150 is generous for "<pid> <name>" because the name is an unbounded
  // process name; it elides with a tooltip beyond that.
  { COL_PPID,    ConnectionStore::ColumnTitle(COL_PPID),   150, LVCFMT_LEFT },
  // F5.2/F5.3. Both are short words, sized from their longest possible cell
  // ("Protected+AC" and "BAD SIG") rather than from a typical row, so enabling
  // either never shows a truncated header.
  { COL_INTEGRITY, ConnectionStore::ColumnTitle(COL_INTEGRITY), 90, LVCFMT_LEFT },
  { COL_SIGNATURE, ConnectionStore::ColumnTitle(COL_SIGNATURE), 90, LVCFMT_LEFT },
};

// Every column needs exactly one IDM_COL_* command (View > Columns) and one
// entry in kColumns; a mismatch shows up here instead of as a column that
// cannot be toggled.
static_assert(COL_COUNT == IDM_COL_COUNT,
              "resource.h IDM_COL_COUNT is out of step with Columns.h COL_COUNT");
static_assert(sizeof(kColumns) / sizeof(kColumns[0]) == COL_COUNT,
              "kColumns must describe every ColumnId");

// Display order for the list header. IDs, masks and widths stay keyed by
// ColumnId; the *visible* columns are inserted in this order. Identity
// leads (Process, CPU %, Traffic) because those are what a first-time
// user scans, then the connection identity (Proto/addresses/state/PID),
// then the auxiliary lookups. The hidden-by-default stat columns sit
// next to CPU so enabling them from View > Columns keeps metrics grouped.
// EVERY ColumnId must appear exactly once. The static_assert below does not
// catch a duplicate (the array would still have COL_COUNT entries), so the
// invariant is also enforced by the sort check: a display order that is not
// strictly increasing would mean two columns claimed the same slot.
const int kDisplayOrder[COL_COUNT] = {
    COL_PROCESS, COL_CPU, COL_TRAFFIC, COL_PROTO,
    COL_LOCAL, COL_LPORT, COL_REMOTE, COL_RPORT, COL_STATE, COL_PID,
    COL_SERVICE, COL_HOST, COL_PATH,
    // The two rate columns sit together, then the connection-health columns:
    // RTT next to Min RTT next to Cwnd next to Retrans is the order `ss -i`
    // prints, and keeping them adjacent means the relationship between a
    // current RTT and its best-ever value is visible at a glance.
    COL_DURATION, COL_BANDWIDTH, COL_GROUPRATE,
    COL_RTT, COL_MINRTT, COL_CWND, COL_RETRANS,
    COL_TLS, COL_COUNTRY, COL_PINNED, COL_NOTE,
    // F5.1/F5.2/F5.3 join the other per-process identity and trust columns:
    // after the bookmark fields, before the raw stat columns.
    COL_PPID, COL_INTEGRITY, COL_SIGNATURE,
    COL_MEM, COL_DISK, COL_RX, COL_TX, COL_NETTOTAL,
};

struct StateItem { const wchar_t* label; DWORD state; };
const StateItem kStateItems[] = {
    { L"All states",       kAllStates },
    { L"ESTABLISHED",      MIB_TCP_STATE_ESTAB },
    { L"LISTENING",        MIB_TCP_STATE_LISTEN },
    { L"TIME_WAIT",        MIB_TCP_STATE_TIME_WAIT },
    { L"SYN_SENT",         MIB_TCP_STATE_SYN_SENT },
    { L"SYN_RECEIVED",     MIB_TCP_STATE_SYN_RCVD },
    { L"FIN_WAIT_1",       MIB_TCP_STATE_FIN_WAIT1 },
    { L"FIN_WAIT_2",       MIB_TCP_STATE_FIN_WAIT2 },
    { L"CLOSE_WAIT",       MIB_TCP_STATE_CLOSE_WAIT },
    { L"CLOSING",          MIB_TCP_STATE_CLOSING },
    { L"LAST_ACK",         MIB_TCP_STATE_LAST_ACK },
    { L"CLOSED",           MIB_TCP_STATE_CLOSED },
    { L"DELETE_TCB",       MIB_TCP_STATE_DELETE_TCB },
    { L"— (none / UDP)", 0 },
};

void SetChildFont(HWND hwnd, HFONT font) {
    if (font != nullptr && hwnd != nullptr) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                       MAKELPARAM(TRUE, 0));
    }
}

void AddComboString(HWND cbo, const wchar_t* text, LPARAM itemData = 0) {
    const LRESULT idx = ::SendMessageW(cbo, CB_ADDSTRING, 0,
                                       reinterpret_cast<LPARAM>(text));
    if (idx != CB_ERR)
        ::SendMessageW(cbo, CB_SETITEMDATA, static_cast<WPARAM>(idx), itemData);
}

// Check/uncheck a menu item that lives in one of the top-level submenus:
// CheckMenuItem only inspects the given menu handle, and the item IDs
// live inside the File/View/Edit popups, not on the menu bar itself.
void SetMenuCheck(HMENU menu, UINT id, bool on) {
    if (menu == nullptr) return;
    const UINT flags = MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED);
    const int count = ::GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        if (::GetMenuItemID(menu, i) == static_cast<UINT>(-1)) {
            HMENU sub = ::GetSubMenu(menu, i);
            if (sub != nullptr &&
                ::GetMenuState(sub, id, MF_BYCOMMAND) != 0xFFFFFFFF) {
                ::CheckMenuItem(sub, id, flags);
                return;
            }
        }
    }
    ::CheckMenuItem(menu, id, flags);
}

// Locate the View > Columns popup by scanning for one of its command IDs
// instead of by hard-coded submenu index. New items have been inserted above
// "Columns" across phases (tray, traffic, graphs...), so any fixed
// index silently pointed at the wrong entry and left colsMenu NULL -- which
// meant no column check mark was ever shown.
HMENU FindColumnsMenu(HMENU mainMenu) {
    if (mainMenu == nullptr) return nullptr;
    const int topCount = ::GetMenuItemCount(mainMenu);
    for (int t = 0; t < topCount; ++t) {
        HMENU top = ::GetSubMenu(mainMenu, t);
        if (top == nullptr) continue;
        const int subCount = ::GetMenuItemCount(top);
        for (int i = 0; i < subCount; ++i) {
            HMENU sub = ::GetSubMenu(top, i);
            if (sub == nullptr) continue;
            const int n = ::GetMenuItemCount(sub);
            for (int j = 0; j < n; ++j) {
                if (::GetMenuItemID(sub, j) == IDM_COL_PROTO) return sub;
            }
        }
    }
    return nullptr;
}

// True for the four columns fed by the ETW per-PID counters.
// They all render "—" until a traffic session is running, so showing any
// of them is what triggers the implicit start + status-bar hint below.
bool IsTrafficColumn(int col) {
    return col == COL_TRAFFIC || col == COL_RX || col == COL_TX ||
           col == COL_NETTOTAL;
}

// 9.2.5: the admin-required actions (Block connection, ETW traffic counters)
// get the standard UAC shield glyph so a click reads as "this needs elevation"
// before the consent dialog appears. The icon is loaded once from the system
// via SHGetStockIconInfo (SIID_SHIELD); like every other shell32 call in this
// file it is skipped when shell32 is delay-load unavailable, and skipped under
// high contrast - HC already provides its own high-contrast palette and a
// custom bitmap there would be drawn over it illegibly.
//
// The shield is shown only when the action is actually elevation-gated AND the
// current process is not already elevated. A standard account that cannot
// elevate at all gets NO shield: there is nothing the glyph could promise, so
// clicking the item shows ElevationUnavailableReason() instead (see the
// OnCommand handlers for IDM_BLOCK_CONNECTION / IDM_VIEW_TRAFFIC).
HICON g_shieldIcon = nullptr;   // owned; never destroyed (leak == exit)
bool  g_shieldTried = false;    // attempted load once

HICON LoadShieldIcon() {
    // shell32.dll is delay-loaded (WinCaps.cpp); gate before touching it so
    // the absence is a clean no-shield rather than an import crash.
    if (!DllAvailable("shell32.dll")) return nullptr;
    SHSTOCKICONINFO sii = {};
    sii.cbSize = sizeof(sii);
    if (::SHGetStockIconInfo(SIID_SHIELD, SHGFI_ICON | SHGFI_SMALLICON, &sii)
            != S_OK || sii.hIcon == nullptr)
        return nullptr;
    return sii.hIcon;
}

HICON ShieldIcon() {
    if (!g_shieldTried) {
        g_shieldTried = true;
        g_shieldIcon = LoadShieldIcon();
    }
    return g_shieldIcon;
}

// True when the current process lacks an elevated token but its user still has
// a linked Administrators token - i.e. elevation is possible via UAC and the
// shield glyph is meaningful.
bool ElevationPossibleForUser() {
    return !IsElevated() && IsAdminMember();
}

// Put/take the shield glyph on a single menu item. `on == false` clears it.
// Draws (or clears) the UAC shield glyph on one menu item. `on` already folds
// in every precondition - the caller's HighContrastActive() result, and
// ElevationPossibleForUser() - so this function just owns the bitmap plumbing.
// No-op when there is no shield icon to draw (shell32 delay-loaded away, or
// the stock-icon query failed).
void SetMenuItemShield(HMENU menu, UINT id, bool on) {
    if (menu == nullptr) return;
    const HICON icon = ShieldIcon();
    if (icon == nullptr) return;               // no shell / icon unavailable
    MENUITEMINFO mii = {};
    mii.cbSize   = sizeof(mii);
    mii.fMask    = MIIM_BITMAP;
    mii.dwTypeData = nullptr;                  // not used with MIIM_BITMAP
    mii.hbmpItem = on ? reinterpret_cast<HBITMAP>(icon) : nullptr;
    ::SetMenuItemInfoW(menu, id, FALSE, &mii);
}

// 9.2.5: the bar-menu ETW item (View > Per-PID traffic counters). Its submenu
// is found by scanning for the item, so this fails closed if the menu layout
// in wintcp.rc changes rather than asserting a wrong submenu index.
// `highContrast` is supplied by the caller (MainWindow::HighContrastActive)
// because this is a free function and cannot call that member.
void ApplyTrafficShield(HMENU bar, bool highContrast) {
    if (bar == nullptr) return;
    const bool showShield = !highContrast && ElevationPossibleForUser();
    const int pops = ::GetMenuItemCount(bar);
    for (int i = 0; i < pops; ++i) {
        HMENU sub = ::GetSubMenu(bar, i);
        if (sub == nullptr) continue;
        const int subCount = ::GetMenuItemCount(sub);
        for (int j = 0; j < subCount; ++j) {
            MENUITEMINFO sm = {};
            sm.cbSize = sizeof(sm);
            sm.fMask = MIIM_ID;
            if (::GetMenuItemInfoW(sub, static_cast<UINT>(j), TRUE, &sm) &&
                sm.wID == IDM_VIEW_TRAFFIC) {
                SetMenuItemShield(sub, IDM_VIEW_TRAFFIC, showShield);
            }
        }
    }
}

}  // namespace

const wchar_t* MainWindow::kClassName = L"WinTcpViewerMainWindow";

std::shared_ptr<MainWindow::WorkerSink> MainWindow::Sink() const {
    if (sink_ == nullptr) sink_ = std::make_shared<WorkerSink>();
    return sink_;
}

// ---------------------------------------------------------------------------
// lifecycle -----------------------------------------------------------------

MainWindow::~MainWindow() {
    engine_.Stop();
    dns_.Stop();
    std::lock_guard<std::mutex> lk(Sink()->m);
    for (RefreshResult* p : Sink()->pendingRefresh) delete p;
    Sink()->pendingRefresh.clear();
    for (DnsResolver::Result* p : Sink()->pendingDns) delete p;
    Sink()->pendingDns.clear();
}

bool MainWindow::RegisterClass(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WindowProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInstance;
    wc.hIcon = static_cast<HICON>(::LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP),
                                               IMAGE_ICON, 32, 32, 0));
    if (wc.hIcon == nullptr) wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.lpszClassName = kClassName;
    wc.hIconSm = static_cast<HICON>(::LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP),
                                                 IMAGE_ICON, 16, 16, 0));
    if (wc.hIconSm == nullptr) wc.hIconSm = ::LoadIconW(nullptr, IDI_APPLICATION);
    return ::RegisterClassExW(&wc) != 0;
}

HWND MainWindow::Create(HINSTANCE hInstance, int nCmdShow) {
    hInstance_ = hInstance;
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    // Logical 1080x640 at the system DPI (PerMonitorV2 scales later per
    // monitor via WM_DPICHANGED).
    const UINT sysDpi = QueryDpiForWindow(nullptr);
    const int w = ::MulDiv(kDesignWidth, static_cast<int>(sysDpi), kDpiUnscaled);
    const int h = ::MulDiv(kDesignHeight, static_cast<int>(sysDpi), kDpiUnscaled);
    const HWND hwnd = ::CreateWindowExW(0, kClassName, L"WinTCP - TCP/UDP Connections",
                                        style, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                        nullptr, nullptr, hInstance, this);
    if (hwnd == nullptr) return nullptr;

    // Restore the last position/size. Settings were loaded in
    // WM_CREATE, which ran inside CreateWindowEx above. Only honor the
    // rect if it still intersects a live monitor (e.g. after unplugging
    // an external display); otherwise fall back to CW_USEDEFAULT.
    if (settings_.winPlaced && settings_.winW >= 200 && settings_.winH >= 140) {
        RECT wr = { settings_.winX, settings_.winY,
                    settings_.winX + settings_.winW,
                    settings_.winY + settings_.winH };
        if (::MonitorFromRect(&wr, MONITOR_DEFAULTTONULL) != nullptr) {
            ::SetWindowPos(hwnd, nullptr, wr.left, wr.top,
                           settings_.winW, settings_.winH,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            if (settings_.showCmd == SW_SHOWMAXIMIZED) nCmdShow = SW_SHOWMAXIMIZED;
        }
    }

    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);
    return hwnd;
}

// ---------------------------------------------------------------------------
// In-process UI harness (UiHarness.cpp) --------------------------------------
//
// The harness sends real messages - WM_COMMAND with resource ids, WM_CHAR,
// WM_NOTIFY - through the same HandleMessage() the user's keyboard and mouse
// go through. What it cannot do is read the RESULT, because every field here
// is private and there is no interactive desktop in this environment for an
// out-of-process reader to look at. So the window answers questions about
// itself.
//
// Every branch below reports OBSERVED state, never intent. If a menu command
// silently did nothing, frozen_ is still false and this says "false" - which
// is the whole point: the previous round's verification checked that menu
// items EXIST, and this one checks that activating them CHANGES anything.
// ---------------------------------------------------------------------------
const wchar_t* MainWindow::UiProbe(const wchar_t* op, const wchar_t* arg) const {
    static wchar_t buf[1024];
    if (op == nullptr) return nullptr;

    if (::wcscmp(op, L"frozen") == 0) {
        return frozen_ ? L"1" : L"0";
    }
    if (::wcscmp(op, L"grouped") == 0) {
        return groupByProcess_ ? L"1" : L"0";
    }
    if (::wcscmp(op, L"preserveSelection") == 0) {
        return preserveSelection_ ? L"1" : L"0";
    }
    if (::wcscmp(op, L"rowCount") == 0) {
        // The virtual list's count is the observable contract; read it from
        // the control rather than from the store, so a view that was never
        // pushed to the screen shows up as 0 here.
        const LRESULT n = ::SendMessageW(hwndList_, LVM_GETITEMCOUNT, 0, 0);
        ::swprintf_s(buf, L"%lld", (long long)n);
        return buf;
    }
    if (::wcscmp(op, L"columnsMenuMissing") == 0) {
        // C3 guard: the View > Columns popup must offer a command for EVERY
        // ColumnId, or that column cannot be shown in the GUI at all - there is
        // no other way to set its bit (the mask is only writable from here).
        // Six were once missing this way (NOTE, the four `ss -i` columns and
        // GROUPRATE): resource.h had their ids and IDM_COL_COUNT counted them,
        // so the compile-time guard was satisfied, while wintcp.rc had no
        // MENUITEM for them. The guard could not see it because resource.h and
        // wintcp.rc are different files, and nothing compared them.
        //
        // Reports the ColumnId ordinals, so a failure says WHICH column is
        // unreachable rather than only how many; 'columnsMenuMissingCount' is
        // the machine-readable form the harness asserts on.
        HMENU sub = FindColumnsMenu(::GetMenu(hwnd_));
        if (sub == nullptr) return L"-1";   // popup itself not found
        wchar_t missing[512] = {0};
        int count = 0;
        for (int c = 0; c < COL_COUNT; ++c) {
            if (::GetMenuState(sub, IDM_COL_BASE + c, MF_BYCOMMAND) ==
                0xFFFFFFFF) {
                wchar_t one[16] = {0};
                ::swprintf_s(one, L"%s%d", (count == 0) ? L"" : L",", c);
                ::wcscat_s(missing, one);
                ++count;
            }
        }
        if (count == 0) return L"0";
        // Into `buf`, not `missing`: every other branch returns the static
        // buffer, and returning a local's address would dangle the moment this
        // function returned.
        ::wcsncpy_s(buf, missing, _TRUNCATE);
        return buf;
    }
    if (::wcscmp(op, L"columnsMenuMissingCount") == 0) {
        HMENU sub = FindColumnsMenu(::GetMenu(hwnd_));
        if (sub == nullptr) return L"-1";
        int count = 0;
        for (int c = 0; c < COL_COUNT; ++c) {
            if (::GetMenuState(sub, IDM_COL_BASE + c, MF_BYCOMMAND) ==
                0xFFFFFFFF)
                ++count;
        }
        ::swprintf_s(buf, L"%d", count);
        return buf;
    }
    if (::wcscmp(op, L"lastRefreshTime") == 0) {
        ::wcsncpy_s(buf, lastRefreshTime_.c_str(), _TRUNCATE);
        return buf;
    }
    if (::wcscmp(op, L"pendingRefresh") == 0) {
        // The worker's callback inserts here BEFORE posting WM_APP_REFRESH_RESULT
        // and OnRefreshResult erases from it. A non-zero count with an empty
        // store therefore means the message was posted but never dispatched -
        // which is exactly the "list never populates" symptom.
        // mutable so this const accessor can take the lock.
        std::lock_guard<std::mutex> lk(Sink()->m);
        ::swprintf_s(buf, L"%zu", Sink()->pendingRefresh.size());
        return buf;
    }
    if (::wcscmp(op, L"engineRunning") == 0) {
        return engine_.Running() ? L"1" : L"0";
    }
    if (::wcscmp(op, L"lastError") == 0) {
        ::wcsncpy_s(buf, lastError_.c_str(), _TRUNCATE);
        return buf;
    }
    if (::wcscmp(op, L"autoRefresh") == 0) {
        return autoRefresh_ ? L"1" : L"0";
    }
    if (::wcscmp(op, L"detailsVisible") == 0) {
        return details_.IsVisible() ? L"1" : L"0";
    }
    if (::wcscmp(op, L"chartsVisible") == 0) {
        return charts_.IsVisible() ? L"1" : L"0";
    }
    if (::wcscmp(op, L"changeLogVisible") == 0) {
        return changeLog_.IsVisible() ? L"1" : L"0";
    }
    // 5.3: whether the change-log window was actually CREATED, as distinct from
    // visible. IsVisible() is false both for a window that was never created and
    // for one that exists and is hidden (the WM_CLOSE path), so on its own it
    // cannot tell "the menu item did nothing" from "the log is open but behind
    // something". The 5.3 harness check needs that distinction to report
    // anything useful when it fails.
    if (::wcscmp(op, L"changeLogCreated") == 0) {
        return changeLog_.IsOpen() ? L"1" : L"0";
    }
    // D29. What Commands::PidKillVerdict() says about a PID, as this process
    // sees it. The harness runs INSIDE wintcp.exe, so asking it about
    // GetCurrentProcessId() exercises the exact self-kill case the bug report
    // was about, with the real self PID and the real rule - which is the only
    // way to catch a wiring mistake that a selftest on synthetic PIDs cannot.
    if (::wcscmp(op, L"killVerdict") == 0) {
        DWORD pid = 0;
        if (arg != nullptr)
            pid = static_cast<DWORD>(::wcstoul(arg, nullptr, 10));
        switch (PidKillVerdict(pid, ::GetCurrentProcessId(), true)) {
            case PidVerdict::Ok:         return L"ok";
            case PidVerdict::IsSelf:     return L"self";
            case PidVerdict::Pseudo:     return L"pseudo";
            case PidVerdict::NotPermitted: return L"nopermit";
        }
        return L"?";
    }
    if (::wcscmp(op, L"changeLogMaskAppear") == 0) {
        return changeLog_.EventAppear() ? L"1" : L"0";
    }
    // 5.3 diagnostic: how many times the log window's WM_CREATE handler has run
    // over this session's lifetime. A count of 0 with the window VISIBLE means
    // the window was created but its controls were never built, which is the
    // one failure mode a visibility probe cannot distinguish from success.
    if (::wcscmp(op, L"changeLogCreates") == 0) {
        return std::to_wstring(changeLog_.CreateCount()).c_str();
    }
    // ...and how many children that handler currently holds. Zero with a
    // non-zero create count means the creations FAILED, which points at the
    // CreateWindowExW arguments rather than at the message plumbing.
    if (::wcscmp(op, L"changeLogChildren") == 0) {
        return std::to_wstring(changeLog_.ChildCount()).c_str();
    }
    if (::wcscmp(op, L"changeLogLastError") == 0) {
        return std::to_wstring(
                   static_cast<unsigned long>(changeLog_.LastChildError()))
            .c_str();
    }
    // "streamVisible" was removed with the hex window. Nothing queried it, and
    // a property that always answers "0" would be a lie waiting for a caller.
    if (::wcscmp(op, L"geoLoaded") == 0) {
        return geo_.Loaded() ? L"1" : L"0";
    }
    if (::wcscmp(op, L"trafficTimeouts") == 0) {
        if (socketTraffic_ == nullptr) return L"0";
        ::swprintf_s(buf, L"%u", socketTraffic_->TimeoutCount());
        return buf;
    }
    // C8: a pass that could not read the handle table at all. Kept beside
    // trafficTimeouts because the two look identical in a table - blank traffic
    // - and a reader who cannot name the cause cannot act on it.
    if (::wcscmp(op, L"trafficScanFailures") == 0) {
        if (socketTraffic_ == nullptr) return L"0";
        ::swprintf_s(buf, L"%u", socketTraffic_->ScanFailureCount());
        return buf;
    }
    // C8: settings writes attempted, and writes that failed. The gap between
    // them is the loss. Both read zero for the whole life of a healthy window -
    // SaveSettings runs once, from ~MainWindow - so these are diagnostic only.
    if (::wcscmp(op, L"settingsSaveAttempts") == 0) {
        ::swprintf_s(buf, L"%u", settingsSaveAttempts_);
        return buf;
    }
    if (::wcscmp(op, L"settingsSaveFailures") == 0) {
        ::swprintf_s(buf, L"%u", settingsSaveFailures_);
        return buf;
    }
    if (::wcscmp(op, L"selectionCount") == 0) {
        std::vector<int> idx;
        const bool any = SelectedRowIndices(idx);
        ::swprintf_s(buf, L"%zu", any ? idx.size() : 0);
        return buf;
    }
    if (::wcscmp(op, L"selectedColumnText") == 0) {
        // Read a cell back out of the virtual list. arg is the visible index.
        const int v = (arg != nullptr) ? ::_wtoi(arg) : -1;
        if (ColToVisible(VisibleToCol(v)) != v) return L"";
        const LRESULT row = ::SendMessageW(hwndList_, LVM_GETNEXTITEM,
                                           static_cast<WPARAM>(-1),
                                           LVNI_FOCUSED);
        if (row < 0) return L"";
        // Ask the OWNER-DATA handler for one cell, through the same WM_NOTIFY
        // the list itself uses. iSubItem lives on the item sub-struct, which
        // is how the production LVN_GETDISPINFO handler reads it.
        NMLVDISPINFO di = {};
        di.hdr.hwndFrom = hwndList_;
        di.hdr.idFrom = ::GetDlgCtrlID(hwndList_);
        di.hdr.code = LVN_GETDISPINFO;
        di.item.mask = LVIF_TEXT;
        di.item.iItem = static_cast<int>(row);
        di.item.iSubItem = v;
        di.item.cchTextMax = static_cast<int>(kMaxColumnText);
        di.item.pszText = buf;
        ::SendMessageW(hwnd_, WM_NOTIFY, 0,
                       reinterpret_cast<LPARAM>(&di));
        return buf;
    }
    if (::wcscmp(op, L"jumpLabel") == 0) {
        // The exact string OnTypeJumpChar builds its candidate list from for
        // the focused row: the process name where there is one, the local
        // address otherwise. Exposed so a test can assert the jump landed on
        // a row that really matches the typed prefix, rather than on a row
        // that merely moved.
        const LRESULT row = ::SendMessageW(hwndList_, LVM_GETNEXTITEM,
                                           static_cast<WPARAM>(-1),
                                           LVNI_FOCUSED);
        if (row < 0) return L"";
        const Connection* c = store_.ViewRow(static_cast<size_t>(row));
        if (c == nullptr) return L"";
        ::wcsncpy_s(buf, c->processName.empty() ? c->localAddress.c_str()
                                                : c->processName.c_str(),
                    _TRUNCATE);
        return buf;
    }
    if (::wcscmp(op, L"columnOrder") == 0) {
        // The persisted 7.1 order, as a comma list, so the harness can prove
        // a drag actually changed the order and that it round-tripped.
        for (std::size_t i = 0; i < colOrder_.size() && i < 32; ++i) {
            const std::wstring piece = L" " + std::to_wstring(colOrder_[i]);
            ::wcsncat_s(buf, piece.c_str(), _TRUNCATE);
        }
        return buf;
    }
    if (::wcscmp(op, L"visibleCol") == 0) {
        const int v = (arg != nullptr) ? ::_wtoi(arg) : -1;
        const int col = VisibleToCol(v);
        ::swprintf_s(buf, L"%d", col);
        return buf;
    }
    if (::wcscmp(op, L"columnWidth") == 0) {
        const int v = (arg != nullptr) ? ::_wtoi(arg) : -1;
        const int col = VisibleToCol(v);
        if (col < 0) return L"";
        ::swprintf_s(buf, L"%d", colWidths_[col]);
        return buf;
    }
    if (::wcscmp(op, L"trayShown") == 0) {
        return trayIconShown_ ? L"1" : L"0";
    }
    // 9.4.8: which child control holds keyboard focus, so the harness can prove
    // the `/` shortcut moved focus to the filter box. "list", "filter", or ""
    // (neither - the window or another control has it).
    if (::wcscmp(op, L"focusTarget") == 0) {
        const HWND fg = ::GetFocus();
        if (fg == hwndList_) return L"list";
        if (fg == hwndSearchEdit_) return L"filter";
        if (fg == hwnd_) return L"window";
        return L"";
    }
    // 9.4.8: the live bookmark count, so the harness can prove `*` toggled one.
    if (::wcscmp(op, L"bookmarkCount") == 0) {
        ::swprintf_s(buf, L"%zu", Bookmarks::List().size());
        return buf;
    }
    if (::wcscmp(op, L"topMost") == 0) {
        return topMost_ ? L"1" : L"0";
    }
    if (::wcscmp(op, L"colOrderSize") == 0) {
        ::swprintf_s(buf, L"%zu", colOrder_.size());
        return buf;
    }
    return nullptr;
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) return self->HandleMessage(msg, wParam, lParam);
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: OnCreate(); return 0;
        case WM_SIZE: OnSize(LOWORD(lParam), HIWORD(lParam)); return 0;
        case WM_COMMAND: {
            OnCommand(LOWORD(wParam), HIWORD(wParam),
                      reinterpret_cast<HWND>(lParam));
            return 0;
        }
        case WM_NOTIFY: {
            auto* n = reinterpret_cast<NMHDR*>(lParam);
            if (n != nullptr && n->code == NM_CUSTOMDRAW) {
                if (n->hwndFrom == hwndList_)
                    return OnCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(n));
            }
            // LVN_BEGINDRAG arrives from the LIST (not the header) and is the
            // only drag notification that exists: there is no LVN_COLUMNDRAG
            // and no end-of-drag notification in the Windows SDK - an
            // exhaustive search of CommCtrl.h found HDN_BEGINDRAG/HDN_ENDDRAG
            // and LVN_BEGINDRAG/LVN_BEGINRDRAG, and nothing else. So the drag
            // is ALLOWED here by returning TRUE, and the resulting order is
            // read back from the header once the drag has finished, rather
            // than inferred from a payload whose meaning is not documented.
            if (n != nullptr && n->code == LVN_BEGINDRAG &&
                n->hwndFrom == hwndList_) {
                ::PostMessageW(hwnd_, WM_APP_COLUMNORDER_SYNC, 0, 0);
                return 1;   // non-zero: permit the drag
            }
            OnNotify(n, lParam);
            return 0;
        }
        case WM_APP_COLUMNORDER_SYNC:
            SyncColumnOrderFromHeader();
            return 0;
        case WM_TIMER: OnTimer(static_cast<UINT_PTR>(wParam)); return 0;
        // 7.2 type-to-jump. Only when the LIST has focus: the filter box and
        // the interval combo are ordinary text entry, and stealing their
        // keystrokes would break typing a filter.
        case WM_CHAR:
            // 9.4.8: `/` focuses the filter box (vim-style). It is consumed
            // here only when the LIST holds focus - otherwise it falls through
            // to the filter edit, where it is ordinary text input. The keystroke
            // is NOT passed to OnTypeJumpChar, which would consume it as a
            // matching step (`/` is not a prefix of any process name, so it does
            // nothing visible but still eats the key).
            if (hwndList_ != nullptr &&
                ::GetFocus() == hwndList_ &&
                static_cast<wchar_t>(wParam) == L'/') {
                FocusFilterBox();
                return 0;
            }
            if (hwndList_ != nullptr &&
                ::GetFocus() == hwndList_) {
                if (OnTypeJumpChar(static_cast<wchar_t>(wParam)))
                    return 0;
            }
            break;
        case WM_SYSCOLORCHANGE:
            RefreshSystemColors();
            return 0;
        case WM_CONTEXTMENU:
            OnContextMenu(reinterpret_cast<HWND>(wParam),
                          static_cast<int>(GET_X_LPARAM(lParam)),
                          static_cast<int>(GET_Y_LPARAM(lParam)));
            return 0;
        case WM_SETFOCUS:
            if (hwndList_ != nullptr) ::SetFocus(hwndList_);
            return 0;
        case WM_APP_REFRESH_RESULT:
            OnRefreshResult(reinterpret_cast<RefreshResult*>(lParam));
            return 0;
        case WM_APP_DNS_RESULT:
            OnDnsResult(reinterpret_cast<DnsResolver::Result*>(lParam));
            return 0;
        case WM_APP_DNS_STALLED:
            // 9.2.8: wParam = count of lookups abandoned as stalled this session.
            // Append a hint to the status bar; the pending rows already paint
            // the `pending` cell inline.
            dnsStalledHint_ = L"DNS slow - some hosts show \"pending\"";
            UpdateStatusBar(lastError_);
            return 0;
        case WM_APP_TRAY: {
            // Classic (non-V4) tray callback: wParam = icon id, lParam =
            // mouse message.
            const UINT ev = static_cast<UINT>(lParam);
            if (ev == WM_LBUTTONDBLCLK || ev == WM_LBUTTONUP) {
                ::ShowWindow(hwnd_, SW_RESTORE);
                ::SetForegroundWindow(hwnd_);
            } else if (ev == WM_RBUTTONUP) {
                ShowTrayMenu();
            }
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            // Use the system window colors so the control bar follows the
            // user's theme - and, when high contrast is on, the
            // accessibility palette - without any theme of our own.
            if (hwndBrushWindow_ != nullptr) {
                SetTextColor(reinterpret_cast<HDC>(wParam),
                             ::GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(reinterpret_cast<HDC>(wParam),
                           ::GetSysColor(COLOR_WINDOW));
                return reinterpret_cast<LRESULT>(hwndBrushWindow_);
            }
            break;
        case WM_SYSCOMMAND:
            // Minimize-to-tray while the tray icon is enabled.
            if ((wParam & 0xFFF0) == SC_MINIMIZE) {
                // 9.4.6: when the tray icon is off and the user has not yet
                // answered, offer the one-shot "tray or exit" choice before
                // minimizing to the taskbar. ResolveMinimize enables the tray
                // when the user picks it.
                if (!trayEnabled_) {
                    switch (ResolveMinimize()) {
                        case MinimizeTarget::kTray:
                            // trayEnabled_ + trayIconShown_ are now set; fall
                            // through to the hide below.
                            break;
                        case MinimizeTarget::kExit:
                            ::DestroyWindow(hwnd_);
                            return 0;
                        case MinimizeTarget::kTaskbar:
                            // Unchanged default: stay in the taskbar.
                            break;
                    }
                }
                if (trayEnabled_ && trayIconShown_) {
                    ::ShowWindow(hwnd_, SW_HIDE);
                    return 0;
                }
            }
            break;
        case WM_INITMENUPOPUP:
            // Re-sync the check marks that can change outside the command
            // handlers: the charts window can be
            // closed with its own X button, and the ETW session can stop
            // on its own during shutdown paths.
            SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_CHARTS,
                         charts_.IsVisible());
            SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_TRAFFIC,
                         etw_.Running());
            // Re-sync the View > Columns ticks on every popup open so the
            // menu always reflects visibleCols_, even if state changed
            // through a path that did not update the menu itself.
            SyncColumnMenuChecks();
            break;
        case WM_DPICHANGED: {
            // Per-monitor DPI change: follow the suggested rect,
            // refresh the font, re-derive column widths and layout.
            HarvestColumnWidths();               // with the OLD dpi_
            // wParam packs the NEW dpi in the low word and the previous
            // dpi in the high word; taking HIWORD applied the old scale
            // and the 0-fallback then re-read the old value too.
            dpi_ = LOWORD(wParam);
            if (dpi_ == 0) dpi_ = QueryDpiForWindow(hwnd_);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            RecreateFont();
            RebuildColumns();
            RECT rc = {};
            ::GetClientRect(hwnd_, &rc);
            LayoutControls(rc.right, rc.bottom);
            ::RedrawWindow(hwnd_, nullptr, nullptr,
                           RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        }
        case WM_CLOSE: ::DestroyWindow(hwnd_); return 0;
        case WM_DESTROY: OnDestroy(); return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void MainWindow::OnCreate() {
    // Allocated here, never deleted. A scan thread blocked in SIO_TCP_INFO
    // holds a pointer to its sampler and cannot be cancelled, so freeing it
    // while that thread is alive would be a use-after-free; leaking one
    // object at exit is the smaller problem. See ~SocketTrafficSampler.
    socketTraffic_ = new SocketTrafficSampler();
    for (int i = 0; i < COL_COUNT; ++i) colWidths_[i] = kColumns[i].width;
    visibleCols_ = kDefaultVisibleCols;
    settings_.Load();                       // (pre-set by Create())

    // 9.1.5: auto-load the remembered GeoIP database on startup. This is
    // best-effort and silent - a moved file would pop a nag on every
    // launch, so a failed load just leaves Country as "—", which is the
    // same honest "unknown" as never having picked a database. The next
    // refresh fills the rows via OfferGeoIpForAllRows(); View > GeoIP
    // database still reports the parser's reason on demand.
    if (settings_.geoIpPath[0] != L'\0') {
        std::wstring err;
        if (geo_.Load(settings_.geoIpPath, &err)) {
            OfferGeoIpForAllRows();
            ApplyView();
        }
    }
    // F5.4. Same best-effort, silent contract for the ASN database - see the
    // comment above. Loaded independently of the country one, so either, both or
    // neither survives a relaunch.
    if (settings_.asnIpPath[0] != L'\0') {
        std::wstring asnErr;
        if (asnGeo_.Load(settings_.asnIpPath, &asnErr)) {
            OfferAsnForAllRows();
            ApplyView();
        }
    }

    visibleCols_ = settings_.colVisible;
    if (settings_.colWidthsValid) {
        for (int i = 0; i < COL_COUNT; ++i) {
            if (settings_.colWidths[i] > 0) colWidths_[i] = settings_.colWidths[i];
        }
    }
    // Column order (7.1). Start from the built-in order and only override it
    // if the registry holds a validated permutation; Settings::Load has
    // already rejected anything malformed, so no re-check is needed here.
    for (int i = 0; i < COL_COUNT; ++i) colOrder_[i] = kDisplayOrder[i];
    if (settings_.colOrderValid) {
        for (int i = 0; i < COL_COUNT; ++i) colOrder_[i] = settings_.colOrder[i];
    }
    autoRefresh_ = settings_.autoRefresh;
    autoRefreshMs_ = settings_.intervalMs;
    dnsEnabled_ = settings_.resolveHosts;
    topMost_ = settings_.topMost;
    trayEnabled_ = settings_.trayEnabled;
    trayMinimizeChoice_ = settings_.trayMinimizeChoice;
    store_.SetSort(settings_.sortCol, settings_.sortAsc);

    dpi_ = QueryDpiForWindow(hwnd_);
    CreateControls();
    // Backdrop for WM_CTLCOLOR* (filter box + labels). Recreated by
    // RefreshSystemColors() when the theme changes.
    hwndBrushWindow_ = ::CreateSolidBrush(::GetSysColor(COLOR_WINDOW));
    RecreateFont();
    InitFilterControls();
    ::SetWindowTextW(hwndSearchEdit_, settings_.filter);
    if (autoRefresh_) {
        ::SendMessageW(hwndAutoChk_, BM_SETCHECK, BST_CHECKED, 0);
        // select matching interval item
        for (LRESULT i = 0; i < ::SendMessageW(hwndIntervalCbo_, CB_GETCOUNT, 0, 0); ++i) {
            const LRESULT d = ::SendMessageW(hwndIntervalCbo_, CB_GETITEMDATA,
                                             static_cast<WPARAM>(i), 0);
            if (d == static_cast<LRESULT>(autoRefreshMs_)) {
                ::SendMessageW(hwndIntervalCbo_, CB_SETCURSEL,
                               static_cast<WPARAM>(i), 0);
                break;
            }
        }
    }
    RebuildColumns();
    LayoutControls(kDesignWidth, kDesignHeight);

    // Reflect default column visibility in the View > Columns menu and the
    // persisted toggle states in their menu check marks.
    SyncColumnMenuChecks();
    SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_AUTOREFRESH, autoRefresh_);
    SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_RESOLVE, dnsEnabled_);
    SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_TOPMOST, topMost_);
    SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_TRAY, trayEnabled_);
    // 7.4 defaults to ON, so the tick is set from the member rather than left
    // to whatever the resource compiled with - an unticked "Preserve
    // selection" that is actually on would be the reverse lie.
    SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_PRESERVE_SEL, preserveSelection_);
    if (autoRefresh_)
        ::SetTimer(hwnd_, kRefreshTimerId, autoRefreshMs_, nullptr);
    // Drives the status bar's age / countdown pane. Runs even with
    // auto-refresh off so "3m old" stays truthful after a manual F5.
    ::SetTimer(hwnd_, kStatusTimerId, kStatusTickMs, nullptr);
    if (topMost_)
        ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RefreshSystemColors();                // system theme / high contrast
    if (trayEnabled_) TrayAdd();

    // resume per-PID traffic counters if they were enabled while
    // elevated. A silent failure here (e.g. the user is no longer elevated)
    // just clears the flag; the menu toggle reports the reason on demand.
    if (settings_.trafficEnabled) {
        std::wstring etwError;
        if (!etw_.Start(etwError)) settings_.trafficEnabled = false;
        etwAutoAttempted_ = true;          // a start was already tried here
    }
    // Persisted traffic columns + no running session (e.g. the
    // app is no longer elevated) - make the same implicit attempt the
    // column-toggle path makes: ETW first, then the non-admin socket
    // fallback, so the state shown below is accurate at startup.
    if (!etw_.Running() && AnyTrafficColVisible()) EnsureTrafficCounters();
    SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_TRAFFIC, etw_.Running());
    SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_CHARTS, charts_.IsVisible());

    // 9.2.5: draw the UAC shield on the elevation-gated "Per-PID traffic
    // counters" item (shown when the user is an admin who is not currently
    // elevated; cleared if already elevated or under high contrast).
    ApplyTrafficShield(::GetMenu(hwnd_), HighContrastActive());

    StartWorker();
    StartDnsWorker();
    OpenLogFromSettings();                 // resume a saved log
}

void MainWindow::OnDestroy() {
    ::KillTimer(hwnd_, kRefreshTimerId);
    ::KillTimer(hwnd_, kStatusTimerId);
    ::KillTimer(hwnd_, kFilterTimerId);
    engine_.Stop();                      // no callbacks can be in flight after join
    dns_.Stop();
    etw_.Stop();                         // stop the kernel logger
    charts_.Close();                     // stop the 1 s sampler
    changeLog_.Close();                  // 5.4: destroy before the owner goes
    {
        std::lock_guard<std::mutex> lk(Sink()->m);
        Sink()->shuttingDown = true;
        // Free payloads posted but not yet dispatched (their messages may
        // still surface; the handler checks registration before touching).
        for (RefreshResult* p : Sink()->pendingRefresh) delete p;
        Sink()->pendingRefresh.clear();
        for (DnsResolver::Result* p : Sink()->pendingDns) delete p;
        Sink()->pendingDns.clear();
    }
    SaveSettings();                        // persist UI state
    TrayRemove();                          
    if (logFile_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(logFile_);
        logFile_ = INVALID_HANDLE_VALUE;
    }
    if (hwndBrushWindow_ != nullptr) {
        ::DeleteObject(hwndBrushWindow_);
        hwndBrushWindow_ = nullptr;
    }
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    ::PostQuitMessage(0);
}

// ---------------------------------------------------------------------------
// worker plumbing ---------------------------------------------------

void MainWindow::StartWorker() {
    // Both lambdas capture shared state, never `this`: after Stop()
    // detaches the worker, a late pass still runs them, and anything they
    // touch must outlive the window. The sampler is leaked by design (never
    // freed); the flag and the sink are refcounted.
    const std::shared_ptr<WorkerSink> sink = Sink();
    sink->hwnd = hwnd_;
    const std::shared_ptr<std::atomic<bool>> flag = fallbackFlag_;
    SocketTrafficSampler* const sampler = socketTraffic_;
    // the worker also feeds the non-admin traffic fallback, using
    // the same PID set as the stats sampling. The lambda is a no-op unless
    // the UI armed the fallback (atomic check, no handle scan).
    engine_.SetTrafficSource(
        [flag, sampler](const std::vector<DWORD>& pids)
            -> std::map<DWORD, PidTraffic> {
            if (sampler == nullptr ||
                !flag->load(std::memory_order_relaxed))
                return std::map<DWORD, PidTraffic>();
            return sampler->Sample(pids);
        });
    // The same scan, per-socket. This is the only source that observes ONE
    // socket, so it is the only one allowed to mark rows perRowBytes - and
    // nothing did before, which is why the Speed column was a permanent
    // em-dash in the GUI while the CLI help advertised the column.
    engine_.SetSocketByteSource(
        [flag, sampler](const std::vector<DWORD>& pids)
            -> std::vector<SocketBytes> {
            if (sampler == nullptr ||
                !flag->load(std::memory_order_relaxed))
                return std::vector<SocketBytes>();
            return sampler->Bytes(pids);
        });
    // R8: seed the watchdog age. A fresh worker legitimately takes seconds
    // for its first pass, and the threshold floor covers it — but without a
    // seed the age would measure from boot and declare a brand-new worker
    // stale on its first tick.
    workerStartTick_ = ::GetTickCount64();
    watchdogRestarting_ = false;
    watchdogRestarts_ = 0;
    engine_.Start([sink](std::unique_ptr<RefreshResult> r) {
        RefreshResult* raw = r.release();
        bool registered = false;
        {
            std::lock_guard<std::mutex> lk(sink->m);
            if (!sink->shuttingDown) {
                sink->pendingRefresh.insert(raw);
                registered = true;
            }
        }
        if (registered) {
            const BOOL ok = ::PostMessageW(sink->hwnd, WM_APP_REFRESH_RESULT,
                                           0, reinterpret_cast<LPARAM>(raw));
            if (ok == FALSE) {
                std::lock_guard<std::mutex> lk(sink->m);
                if (sink->pendingRefresh.erase(raw) != 0) delete raw;
            }
        } else {
            delete raw;
        }
    });
    if (!engine_.Running()) {
        // The worker never came up. Everything downstream silently shows an
        // empty list, so make it loud rather than letting the window look
        // healthy while doing nothing.
        lastError_ = L"Refresh worker failed to start.";
    }
}

void MainWindow::StartDnsWorker() {
    const std::shared_ptr<WorkerSink> sink = Sink();
    sink->hwnd = hwnd_;
    // 9.2.8: a per-lookup budget so a wedged resolver stalls at most one tick
    // instead of the whole queue. The GUI renders `pending` for the rows still
    // in flight; a status-bar hint fires on stalls.
    dns_.SetTimeoutMs(DnsResolver::kDnsTimeoutDefault);
    dns_.SetStallSink([sink](unsigned pending, unsigned /*ms*/) {
        // Post a status hint; the row cells already show `pending`. Guarded so
        // a late fire during shutdown does not dereference a torn-down window.
        if (sink->hwnd != nullptr && !sink->shuttingDown)
            ::PostMessageW(sink->hwnd, WM_APP_DNS_STALLED,
                           static_cast<WPARAM>(pending), 0);
    });
    dns_.Start([sink](std::unique_ptr<DnsResolver::Result> r) {
        DnsResolver::Result* raw = r.release();
        bool registered = false;
        {
            std::lock_guard<std::mutex> lk(sink->m);
            if (!sink->shuttingDown) {
                sink->pendingDns.insert(raw);
                registered = true;
            }
        }
        if (registered) {
            if (::PostMessageW(sink->hwnd, WM_APP_DNS_RESULT, 0,
                               reinterpret_cast<LPARAM>(raw)) == FALSE) {
                std::lock_guard<std::mutex> lk(sink->m);
                if (sink->pendingDns.erase(raw) != 0) delete raw;
            }
        } else {
            delete raw;
        }
    });
    dns_.SetEnabled(dnsEnabled_);
}

void MainWindow::OnDnsResult(DnsResolver::Result* payload) {
    if (payload == nullptr) return;
    {
        std::lock_guard<std::mutex> lk(Sink()->m);
        if (Sink()->pendingDns.erase(payload) == 0) return;   // not ours (shutdown)
    }
    const std::unique_ptr<DnsResolver::Result> result(payload);
    if (store_.SetHostname(result->address, result->hostname)) {
        // Hostname affects filtering/sorting only when one of them is in
        // play; otherwise a repaint is enough.
        if (store_.SortColumn() == COL_HOST || !filterProgram_.empty()) {
            ApplyView();
        } else if (hwndList_ != nullptr) {
            ::InvalidateRect(hwndList_, nullptr, FALSE);
        }
    }
}

void MainWindow::OfferDnsForAllRows() {
    for (const Connection& r : store_.Rows()) dns_.Offer(r.remoteAddress);
}

// 4.3 GeoIP join.
//
// Walks the rows and fills the Country column from the user-supplied MMDB.
// Unlike DNS this is entirely SYNCHRONOUS and local: a tree walk per distinct
// address, no thread, no queue, no re-entrancy. That is what makes it cheap
// enough to do inline on every refresh - a few hundred binary searches over a
// mapped file is microseconds, and a separate worker would cost more in
// synchronisation than the lookups cost.
//
// The distinct-address memo is what keeps it that way. A busy machine has
// thousands of rows but far fewer distinct peers (every one of a browser's
// connections to one CDN edge shares an address), and looking each up again is
// pure waste. 'seen' is scoped to this call rather than kept on the member
// because the row set is replaced wholesale on every refresh - a persistent
// cache would have to be invalidated anyway, and a per-pass set is provably
// correct with no invalidation logic to get wrong.
void MainWindow::OfferGeoIpForAllRows() {
    if (!geo_.Loaded()) return;
    std::unordered_set<std::wstring> seen;
    seen.reserve(store_.Rows().size());
    for (const Connection& r : store_.Rows()) {
        if (r.remoteAddress.empty()) continue;
        if (!seen.insert(r.remoteAddress).second) continue;
        std::wstring code;
        if (r.family == AF_INET) {
            // Rows carry the address as text, so parse back to the integer the
            // tree walk wants. A remote address in the store always came from
            // the stack, so this cannot fail; if it somehow does, skipping is
            // correct and must not blank a country another row already set.
            in_addr v4 = {};
            if (::InetPtonW(AF_INET, r.remoteAddress.c_str(), &v4) != 1)
                continue;
            code = geo_.LookupV4(ntohl(v4.S_un.S_addr));
        } else {
            IN6_ADDR v6 = {};
            if (::InetPtonW(AF_INET6, r.remoteAddress.c_str(), &v6) != 1)
                continue;
            code = geo_.LookupV6(reinterpret_cast<const unsigned char*>(&v6));
        }
        // An empty code is written too: it clears a stale value when a peer
        // moves out of a covered range, and SetCountry is a no-op when the
        // value is already right, so this costs nothing.
        store_.SetCountry(r.remoteAddress, code);
    }
}

// F5.4. The ASN pass. Separate from OfferGeoIpForAllRows rather than folded into
// it, because the two databases are independent: either may be loaded without
// the other, and a row must be able to carry a country with no ASN or an ASN with
// no country.
//
// It also has to run even when the COUNTRY database is absent - which is the
// case that would otherwise silently do nothing, since the country function
// returns early when its own database is not loaded.
void MainWindow::OfferAsnForAllRows() {
    if (!asnGeo_.Loaded()) return;
    std::unordered_set<std::wstring> seen;
    seen.reserve(store_.Rows().size());
    for (const Connection& r : store_.Rows()) {
        if (r.remoteAddress.empty()) continue;
        if (!seen.insert(r.remoteAddress).second) continue;
        AsnInfo info;
        if (r.family == AF_INET) {
            in_addr v4 = {};
            if (::InetPtonW(AF_INET, r.remoteAddress.c_str(), &v4) != 1)
                continue;
            info = asnGeo_.LookupAsnV4(ntohl(v4.S_un.S_addr));
        } else {
            IN6_ADDR v6 = {};
            if (::InetPtonW(AF_INET6, r.remoteAddress.c_str(), &v6) != 1)
                continue;
            info = asnGeo_.LookupAsnV6(reinterpret_cast<const unsigned char*>(&v6));
        }
        // Written even when empty: that clears a stale AS number when the file is
        // reloaded with different contents, exactly as the CLI path does.
        store_.SetAsn(r.remoteAddress, info.number, info.org);
    }
}

void MainWindow::OnRefreshResult(RefreshResult* payload) {
    if (payload == nullptr) return;
    {
        std::lock_guard<std::mutex> lk(Sink()->m);
        // 0 = we do not own this pointer (already freed during shutdown).
        if (Sink()->pendingRefresh.erase(payload) == 0) return;
    }
    const std::unique_ptr<RefreshResult> result(payload);

    // R8 liveness stamp: ANY posted result proves the worker alive, including
    // an error result (R2's containment posts those). The display age below
    // stays success-only; the watchdog reads this one.
    lastResultTick_ = ::GetTickCount64();
    if (!result->error.empty()) {
        lastError_ = result->error;
        if (reportErrorsNextResult_) {
            ::MessageBoxW(hwnd_, lastError_.c_str(), L"WinTCP - Refresh failed",
                          MB_OK | MB_ICONERROR);
        }
        reportErrorsNextResult_ = false;
        UpdateStatusBar(lastError_);
        return;
    }
    reportErrorsNextResult_ = false;
    lastError_.clear();
    lastRefreshTime_ = result->timeText;
    // 9.2.8: a completed refresh clears the stale stall hint; the next
    // stalled lookup re-posts WM_APP_DNS_STALLED if it recurs.
    dnsStalledHint_.clear();
    // Monotonic stamp for the status bar's age / next-refresh countdown.
    lastRefreshTick_ = ::GetTickCount64();

    // Capture selection/scroll while the old view is still consistent,
    // swap in the new snapshot, then rebuild the view and restore.
    //
    // 7.4: when preserveSelection_ is off, pass empty state so the rebuilt
    // view starts with nothing selected. Preserving the selection is the
    // better default for a live monitor - a row the user is inspecting
    // should not jump to the top of the list on every 2-second tick - but on
    // a busy machine the tracked row can be thousands of positions down, and
    // some workflows want a clean slate each cycle. Hence the toggle.
    std::vector<std::uint64_t> ids;
    std::uint64_t focusedId = 0;
    int topIdx = 0;
    if (preserveSelection_) {
        CaptureSelection(ids, &focusedId, &topIdx);
    }
    // 5.5 freeze. The sampler has already run and the new snapshot is in
    // hand, but the DISPLAY is left exactly as it was. Freezing the display
    // rather than the sampler is deliberate: counters keep accumulating, so
    // unfreezing shows what changed during the pause instead of a gap, and
    // the per-PID traffic does not arrive as one large lump.
    if (frozen_) {
        lastError_.clear();
        UpdateStatusBar(std::wstring());
        return;
    }
    store_.ReplaceSnapshot(std::move(result->rows));
    WriteChangeLog();                      // (no-op unless logging)
    if (etw_.Running()) {
        // re-apply the cumulative per-PID totals to the fresh
        // rows (ReplaceSnapshot rebuilt them with zeroed counters).
        const auto traffic = etw_.Snapshot();
        for (const auto& kv : traffic)
            store_.SetTraffic(kv.first, kv.second.rx, kv.second.tx);
        // Even on the ETW path the per-socket scan is consulted for RATES
        // only: ETW totals are per-PID and cannot be split across a process's
        // connections honestly, but the socket scan observes one socket, so it
        // is what makes the Speed column real in the common (elevated) case.
        // The totals themselves still come from ETW; ApplySocketBytes only
        // overwrites the rows it can attribute to a single socket, and marks
        // them perRowBytes.
        store_.ApplySocketBytes(result->socketBytes);
        (void)store_.ComputeRates();
    } else if (fallbackFlag_->load(std::memory_order_relaxed)) {
        // same join, sourced from the worker's socket scan. The
        // flag re-check drops data sampled just before the user turned
        // the counters off (ClearTraffic already ran).
        for (const auto& kv : result->traffic)
            store_.SetTraffic(kv.first, kv.second.rx, kv.second.tx);
        // Per-socket counters from that same scan, applied AFTER the per-PID
        // totals: a socket's own bytes are the more specific fact about the
        // same cell, and only this path may set perRowBytes, which is what
        // makes the Speed column real.
        store_.ApplySocketBytes(result->socketBytes);
        // Both readings in place now (previous sample carried by
        // ReplaceSnapshot, this tick's counters just written), so the Speed
        // column can be computed. See ConnectionStore::ComputeRates.
        (void)store_.ComputeRates();
    }
    // join the per-process live stats sampled on this worker
    // pass (manual refresh and every auto-refresh tick alike).
    for (const auto& kv : result->procStats) {
        const ProcStats& s = kv.second;
        store_.SetProcStats(kv.first, s.cpuKnown ? s.cpuPct : -1.0,
                            s.memKnown, s.memWs, s.memPrivate, s.ioKnown,
                            s.ioRead, s.ioWrite);
    }
    if (dnsEnabled_) OfferDnsForAllRows();
    OfferGeoIpForAllRows();
    RefreshBookmarkMarks();
    ApplyViewWith(ids, focusedId, topIdx);
    RefreshDetailsWindow();              // live stats stay fresh
    // F5.6. AFTER the view is rebuilt, so a balloon names rows the user can
    // actually look at - evaluated earlier it would answer about a table
    // that is about to be replaced.
    RunAlerts();
}

void MainWindow::Refresh(bool reportErrors) {
    reportErrorsNextResult_ = reportErrors;
    engine_.Request();
}

// ---------------------------------------------------------------------------
// controls ------------------------------------------------------------------

void MainWindow::CreateControls() {
    const DWORD btnStyle = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
    const DWORD chkStyle = WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX;
    const DWORD cboStyle = WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL;
    const DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;

    hwndRefreshBtn_ = ::CreateWindowExW(0, WC_BUTTONW, L"&Refresh",
                                        btnStyle, 0, 0, 0, 0, hwnd_,
                                        reinterpret_cast<HMENU>(IDC_BTN_REFRESH),
                                        hInstance_, nullptr);
    hwndExportBtn_ = ::CreateWindowExW(0, WC_BUTTONW, L"&Export CSV...",
                                       btnStyle, 0, 0, 0, 0, hwnd_,
                                       reinterpret_cast<HMENU>(IDC_BTN_EXPORT),
                                       hInstance_, nullptr);
    hwndAutoChk_ = ::CreateWindowExW(0, WC_BUTTONW, L"Auto-refresh",
                                     chkStyle, 0, 0, 0, 0, hwnd_,
                                     reinterpret_cast<HMENU>(IDC_CHK_AUTOREFRESH),
                                     hInstance_, nullptr);
    hwndIntervalCbo_ = ::CreateWindowExW(0, WC_COMBOBOXW, L"", cboStyle, 0, 0, 0, S(200),
                                         hwnd_,
                                         reinterpret_cast<HMENU>(IDC_CBO_INTERVAL),
                                         hInstance_, nullptr);
    hwndProtoCbo_ = ::CreateWindowExW(0, WC_COMBOBOXW, L"", cboStyle, 0, 0, 0, S(220),
                                      hwnd_,
                                      reinterpret_cast<HMENU>(IDC_CBO_PROTO),
                                      hInstance_, nullptr);
    hwndStateCbo_ = ::CreateWindowExW(0, WC_COMBOBOXW, L"", cboStyle, 0, 0, 0, S(240),
                                      hwnd_,
                                      reinterpret_cast<HMENU>(IDC_CBO_STATE),
                                      hInstance_, nullptr);
    HWND hwndFilterLabel = ::CreateWindowExW(0, WC_STATICW, L"Filter:",
                                             WS_CHILD | WS_VISIBLE | SS_LEFT,
                                             0, 0, 0, 0, hwnd_,
                                             reinterpret_cast<HMENU>(IDC_STATIC_FILTER),
                                             hInstance_, nullptr);
    (void)hwndFilterLabel;
    hwndSearchEdit_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                                        editStyle, 0, 0, 0, 0, hwnd_,
                                        reinterpret_cast<HMENU>(IDC_EDIT_SEARCH),
                                        hInstance_, nullptr);
    ::SendMessageW(hwndSearchEdit_, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(
                       L"Filter: text, port:443, pid:1234, cpu:12, exclude:remote:tcp:80..."));
    // Bound what can be TYPED to what can be READ (P1). A single-line EDIT
    // defaults to EM_LIMITTEXT 32767, while CurrentSearchText() reads into a
    // 512-wchar buffer - so a filter past 511 characters was silently truncated
    // on the way back in, and the user got results for a filter they had not
    // typed. Setting the control's own limit to the read buffer makes the
    // invariant "typeable == readable" true by construction: the control
    // refuses the extra characters at the keystroke, where they are visible,
    // instead of the read dropping them where they are not.
    ::SendMessageW(hwndSearchEdit_, EM_LIMITTEXT, kSearchTextMaxChars, 0);

    // Virtual list view: rows are supplied by LVN_GETDISPINFO from the
    // store, so refreshes never destroy/recreate items.
    const DWORD listStyle = WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT |
                            LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_CLIPCHILDREN |
                            WS_VSCROLL | WS_HSCROLL;
    hwndList_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", listStyle,
                                  0, 0, 0, 0, hwnd_,
                                  reinterpret_cast<HMENU>(IDC_LIST), hInstance_,
                                  nullptr);
    ::SendMessageW(hwndList_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                   LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER |
                   LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP | LVS_EX_INFOTIP);
    ::SendMessageW(hwndList_, LVM_SETITEMCOUNT, 0, 0);

    hwndStatus_ = ::CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
                                    WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                    0, 0, 0, 0, hwnd_,
                                    reinterpret_cast<HMENU>(IDC_STATUS),
                                    hInstance_, nullptr);

    const HFONT font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    SetChildFont(hwndRefreshBtn_, font);
    SetChildFont(hwndExportBtn_, font);
    SetChildFont(hwndAutoChk_, font);
    SetChildFont(hwndIntervalCbo_, font);
    SetChildFont(hwndProtoCbo_, font);
    SetChildFont(hwndStateCbo_, font);
    SetChildFont(hwndSearchEdit_, font);
    SetChildFont(hwndList_, font);
    SetChildFont(hwndStatus_, font);
    SetChildFont(hwndFilterLabel, font);

    // 7.3 tab order. Controls were created in a different order from the one a
    // user would expect to walk them in, and without an explicit order Tab
    // follows creation order - so Tab out of the filter box jumped to the
    // Refresh button. The sequence below matches the visual layout: the
    // filter controls left to right, then the list, which is where the
    // user's attention belongs after narrowing the view.
    //
    // Set by rewriting the GW_HWNDNEXT chain, which is exactly what Tab walks.
    // The old BuildTabOrder macro is absent from every current Windows SDK
    // (an exhaustive search finds no declaration), and its CreateWindowEx
    // equivalent is documented as unreliable for WS_GROUP controls, so the
    // chain is linked directly instead.
    //
    // Relinking from the END backwards matters: each SetWindowPos(GW_HWNDPREV)
    // moves a child to sit immediately after its parent, so walking backwards
    // guarantees every child is still where it is needed to be linked next.
    // Walking forwards would work only by accident of the original order.
    const HWND order[] = {
        hwndAutoChk_, hwndIntervalCbo_, hwndProtoCbo_, hwndStateCbo_,
        hwndSearchEdit_, hwndList_, hwndRefreshBtn_, hwndExportBtn_,
    };
    const int n = static_cast<int>(sizeof(order) / sizeof(order[0]));
    for (int i = n - 1; i > 0; --i) {
        if (order[i] == nullptr || order[i - 1] == nullptr) continue;
        ::SetWindowPos(order[i], order[i - 1], 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                           SWP_NOACTIVATE);
    }
}

// Index of the default cadence in the interval combo: the "2 seconds" row. A
// bare `1` here would silently select a different cadence the day someone
// reorders the AddComboString calls in InitFilterControls.
constexpr int kDefaultIntervalComboIndex = 1;

void MainWindow::InitFilterControls() {
    // The offered cadences. Labels carry the meaning; the default row is
    // kDefaultIntervalComboIndex, which must be the "2 seconds" row — see it.
    AddComboString(hwndIntervalCbo_, L"1 second", 1000);
    AddComboString(hwndIntervalCbo_, L"2 seconds", kDefaultIntervalMs);
    AddComboString(hwndIntervalCbo_, L"5 seconds", 5000);
    AddComboString(hwndIntervalCbo_, L"10 seconds", 10000);
    ::SendMessageW(hwndIntervalCbo_, CB_SETCURSEL, kDefaultIntervalComboIndex,
                   0);
    autoRefreshMs_ = kDefaultIntervalMs;

    // Protocol filter: family and protocol bits combined.
    AddComboString(hwndProtoCbo_, L"All (TCP + UDP, IPv4 + IPv6)",
                   static_cast<LPARAM>(kProtoMaskAll));
    AddComboString(hwndProtoCbo_, L"TCP only",
                   static_cast<LPARAM>(kProtoMaskV4 | kProtoMaskV6 | kProtoMaskTCP));
    AddComboString(hwndProtoCbo_, L"UDP only",
                   static_cast<LPARAM>(kProtoMaskV4 | kProtoMaskV6 | kProtoMaskUDP));
    AddComboString(hwndProtoCbo_, L"IPv4 only",
                   static_cast<LPARAM>(kProtoMaskV4 | kProtoMaskTCP | kProtoMaskUDP));
    AddComboString(hwndProtoCbo_, L"IPv6 only",
                   static_cast<LPARAM>(kProtoMaskV6 | kProtoMaskTCP | kProtoMaskUDP));
    ::SendMessageW(hwndProtoCbo_, CB_SETCURSEL, 0, 0);

    for (const StateItem& si : kStateItems)
        AddComboString(hwndStateCbo_, si.label, static_cast<LPARAM>(si.state));
    ::SendMessageW(hwndStateCbo_, CB_SETCURSEL, 0, 0);
}

void MainWindow::HarvestColumnWidths() {
    if (hwndList_ == nullptr) return;
    HWND hdr = ListView_GetHeader(hwndList_);
    if (hdr == nullptr) return;
    const int existing = Header_GetItemCount(hdr);
    for (int i = 0; i < existing && i < static_cast<int>(visToCol_.size()); ++i) {
        HDITEMW hdi = {};
        hdi.mask = HDI_WIDTH;
        if (Header_GetItem(hdr, i, &hdi)) {
            // Physical pixels -> logical 96-dpi units.
            colWidths_[visToCol_[i]] =
                ::MulDiv(hdi.cxy, kDpiUnscaled, static_cast<int>(dpi_));
        }
    }
}

void MainWindow::RebuildColumns() {
    if (hwndList_ == nullptr) return;

    // Remember current widths (user may have dragged separators).
    HarvestColumnWidths();
    HWND hdr = ListView_GetHeader(hwndList_);
    if (hdr != nullptr) {
        const int existing = Header_GetItemCount(hdr);
        for (int i = existing - 1; i >= 0; --i)
            ::SendMessageW(hwndList_, LVM_DELETECOLUMN, static_cast<WPARAM>(i), 0);
    }

    visToCol_.clear();
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
    // colOrder_ is sized at construction, but a defensive check here costs
    // nothing and turns a would-be null-deref into a visibly empty list: a
    // release-only crash in a column loop is not worth debugging twice.
    if (static_cast<int>(colOrder_.size()) != COL_COUNT) {
        colOrder_.assign(COL_COUNT, 0);
        for (int i = 0; i < COL_COUNT; ++i) colOrder_[i] = kDisplayOrder[i];
    }
    for (const int c : colOrder_) {
        if ((visibleCols_ & (1u << c)) == 0) continue;
        col.pszText = const_cast<LPWSTR>(kColumns[c].title);
        col.cx = S(colWidths_[c]);
        col.fmt = kColumns[c].fmt;
        col.iSubItem = static_cast<int>(visToCol_.size());
        const LRESULT idx = ::SendMessageW(
            hwndList_, LVM_INSERTCOLUMNW, static_cast<WPARAM>(visToCol_.size()),
            reinterpret_cast<LPARAM>(&col));
        if (idx >= 0) visToCol_.push_back(c);
    }
    UpdateSortIndicator();
}

int MainWindow::VisibleToCol(int visibleIndex) const {
    if (visibleIndex < 0 || visibleIndex >= static_cast<int>(visToCol_.size()))
        return -1;
    return visToCol_[static_cast<size_t>(visibleIndex)];
}

int MainWindow::ColToVisible(int columnId) const {
    for (size_t i = 0; i < visToCol_.size(); ++i)
        if (visToCol_[i] == columnId) return static_cast<int>(i);
    return -1;
}

void MainWindow::UpdateSortIndicator() {
    HWND hdr = ListView_GetHeader(hwndList_);
    if (hdr == nullptr) return;
    const int n = Header_GetItemCount(hdr);
    const int target = ColToVisible(store_.SortColumn());
    for (int i = 0; i < n; ++i) {
        HDITEMW hdi = {};
        hdi.mask = HDI_FORMAT;
        if (!Header_GetItem(hdr, i, &hdi)) continue;
        hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == target)
            hdi.fmt |= store_.SortAscending() ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hdr, i, &hdi);
    }
}

void MainWindow::SyncColumnMenuChecks() {
    HMENU colsMenu = FindColumnsMenu(::GetMenu(hwnd_));
    if (colsMenu == nullptr) return;
    for (int c = 0; c < COL_COUNT; ++c) {
        ::CheckMenuItem(colsMenu, IDM_COL_BASE + c,
                        MF_BYCOMMAND |
                        ((visibleCols_ & (1u << c)) ? MF_CHECKED
                                                    : MF_UNCHECKED));
    }
}

// ---- F5.6: the alert engine's only production caller ------------------------
// The engine has existed, pure and tested, with no caller at all. This is it.
//
// Three rules it must obey, and one it must NOT:
//
//   * The tray icon is OPTIONAL. Balloons need it; without one the alert still
//     fires, it just lands on the status bar rather than silently vanishing - an
//     alert that does nothing is worse than no alert at all.
//   * The engine latches. A condition already active is suppressed until it
//     clears, so a connection sitting above the threshold does not re-notify on
//     every refresh. That is the engine's job, not this function's.
//   * Settings are re-read FRESH each tick, because another process running
//     `wintcp.exe alert` can change them between two refreshes and the window
//     must not be the only thing that does not know.
//   * It must NOT hold a row pointer across the balloon call - a modal pumps
//     messages and a refresh can reallocate the store underneath. Everything
//     below is copied by value.
void MainWindow::RunAlerts() {
    Settings s;
    if (!s.Load()) return;              // no store, no alerting - say nothing
    alertHint_.clear();
    suppressedAlerts_ = 0;
    if (!s.alerts.enabled) {
        // Reset on disable: a stale latch would fire the instant alerting is
        // turned back on, for a condition the user never saw happen.
        alertEngine_.Reset();
        return;
    }
    const std::vector<Connection> rows = store_.Rows();   // a copy, on purpose
    const std::vector<Alert> fired = alertEngine_.Evaluate(rows, s.alerts);
    for (const Alert& a : fired) {
        if (trayIconShown_) {
            // NIM_MODIFY with NIF_INFO. trayNid_ is a member and stays alive for
            // the duration of the call; szInfo is cleared first so a shorter
            // previous message cannot leave its tail behind.
            ::ZeroMemory(&trayNid_.szInfo, sizeof(trayNid_.szInfo));
            ::ZeroMemory(&trayNid_.szInfoTitle, sizeof(trayNid_.szInfoTitle));
            ::wcsncpy_s(trayNid_.szInfo, a.text.c_str(), _TRUNCATE);
            ::wcsncpy_s(trayNid_.szInfoTitle, a.title.c_str(), _TRUNCATE);
            trayNid_.uFlags = NIF_INFO;
            trayNid_.dwInfoFlags = NIIF_WARNING;
            ::Shell_NotifyIconW(NIM_MODIFY, &trayNid_);
            trayNid_.uFlags = 0;
        } else {
            if (!alertHint_.empty()) alertHint_ += L"  \xB7  ";
            alertHint_ += a.title;
        }
    }
    // Surfaced, not hidden: "nothing appeared" and "it is all already on fire" are
    // different answers and only one of them is fine.
    suppressedAlerts_ = alertEngine_.SuppressedCount();
    UpdateStatusBar(lastError_);
}
// ---- 9.4.2 empty states -----------------------------------------------------
// A blank cell answers the wrong question. "No rows", "no country database
// loaded" and "traffic never ran" are three DIFFERENT reasons a column is empty,
// and on screen they are identical - so each gets one sentence that says what to
// do about it.
//
// Three deliberate choices:
//
// - It is recomputed, never stored. Nothing here is remembered between calls, so
//   no code path can leave a stale hint on screen. OnTimer already re-runs
//   UpdateStatusBar every second and ApplyViewWith calls this on every view
//   change, which is every path that can change any of the predicates.
// - Order is NO ROWS > DATABASE > TRAFFIC. With no rows at all the other two are
//   noise: telling someone their Country column is empty while the table is empty
//   is answering a question they did not ask.
// - Each message names the menu item that fixes it, because "add --db" is not
//   something a GUI user can act on and "pick one in View > GeoIP database" is.
void MainWindow::UpdateEmptyState() {
    emptyStateHint_.clear();

    if (store_.View().empty() && !store_.Rows().empty()) {
        // The table has traffic but the FILTER matches nothing - the case a user
        // reads as "the tool is broken". An empty table with no traffic at all is
        // normal and needs no banner.
        emptyStateHint_ = L"no rows match this filter - clear it to see everything";
        return;
    }

    // A country-bearing column with no database behind it. Both databases count,
    // because either fills the cell.
    const bool countryShown = (visibleCols_ & (1u << COL_COUNTRY)) != 0;
    if (countryShown && !geo_.Loaded() && !asnGeo_.Loaded()) {
        emptyStateHint_ = L"no GeoIP database loaded - pick one in View > GeoIP "
                          L"database to fill Country";
        return;
    }

    // A traffic column with nothing measuring it. AnyTrafficColVisible already
    // answers "does the user care", and the two sources are exactly the two
    // states UpdateStatusBar distinguishes - so this adds no new predicate.
    if (AnyTrafficColVisible() && !etw_.Running() && !fallbackFlag_->load()) {
        emptyStateHint_ = L"traffic not being measured - View > Per-PID traffic "
                          L"counters (needs admin)";
        return;
    }
}
// ---- 9.4.4 column profiles -------------------------------------------------
// A profile is a named COLUMN MASK, nothing else. That is a deliberate narrowing:
// a preset (File > Save view as preset) is a full ViewState - filter, sort,
// grouping, sources and mask - and these must not become a second, weaker preset
// that drifts from the real one. They also must not be persisted as preset
// entries; the visible mask is already persisted in settings_.colVisible.
//
// The masks are computed from the live ColumnId bits rather than hard-coded, so a
// future 33rd column cannot be silently missing from every profile.
UINT32 MainWindow::ColumnProfileMask(int profileId) const {
    switch (profileId) {
        case IDM_PROFILE_MINIMAL:
            return kMinimalProfileCols;
        case IDM_PROFILE_NETWORK:
            return kNetworkProfileCols;
        case IDM_PROFILE_SECURITY:
            return kSecurityProfileCols;
        case IDM_PROFILE_PERFORMANCE:
            return kPerformanceProfileCols;
        case IDM_PROFILE_DIAGNOSTICS:
            // Show diagnostics ORs the five G6/G5 readings onto whatever is
            // visible now, rather than replacing it: "show me the diagnostics
            // TOO" is the question, and a user who has hidden CPU would not
            // expect clicking this to bring it back.
            {
                const UINT32 diagnostics =
                    (1u << COL_RTT) | (1u << COL_MINRTT) | (1u << COL_CWND) |
                    (1u << COL_RETRANS) | (1u << COL_GROUPRATE);
                return visibleCols_ | diagnostics;
            }
        case IDM_PROFILE_DEFAULT:
        default:
            return kDefaultVisibleCols;
    }
}

void MainWindow::ApplyColumnProfile(int profileId) {
    RebuildColumns();                        // harvest widths before the swap
    visibleCols_ = ClampVisibleCols(ColumnProfileMask(profileId));
    settings_.colVisible = visibleCols_;
    RebuildColumns();
    SyncColumnMenuChecks();
    SyncColumnProfileChecks();
    ApplyView();
}

void MainWindow::SyncColumnProfileChecks() {
    const HMENU view = ::GetSubMenu(::GetMenu(hwnd_), 1);   // View popup
    if (view == nullptr) return;
    for (int id = IDM_PROFILE_DEFAULT; id <= IDM_PROFILE_DIAGNOSTICS; ++id) {
        // A profile is checked when the mask equals it EXACTLY. "Show diagnostics"
        // is the exception: it ors onto the current set, so it is checked when all
        // five of its bits are present, which survives the user hiding a column
        // afterwards without lying about what is on screen.
        const UINT32 mask = ColumnProfileMask(id);
        const bool on = (id == IDM_PROFILE_DIAGNOSTICS)
                            ? (visibleCols_ & mask) == mask
                            : visibleCols_ == mask;
        ::CheckMenuItem(view, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }
}

// ---- F5.11 quick filters ---------------------------------------------------
// One click writes an expression into the filter box and lets the existing
// debounce apply it, so the expression is VISIBLE and EDITABLE - the whole point
// of "one click builds the filter" rather than "one click filters".
void MainWindow::ApplyQuickFilter(int commandId) {
    const wchar_t* expr = L"";
    switch (commandId) {
        case IDM_QFILTER_TCP:    expr = L"proto:tcp";        break;
        case IDM_QFILTER_UDP:    expr = L"proto:udp";        break;
        case IDM_QFILTER_LISTEN: expr = L"state:listen";    break;
        case IDM_QFILTER_ESTAB:  expr = L"state:estab";     break;
        case IDM_QFILTER_MINE:
            // The ticket says "Mine" without defining it, and the filter grammar
            // cannot OR two terms - so "listening OR established" is not
            // expressible. What IS expressible, and what the name means:
            // connections whose LOCAL endpoint is on a non-routable range, i.e.
            // this machine talking to its own network rather than to the internet.
            expr = L"local:private";
            break;
        case IDM_QFILTER_ALL:
        default:                 expr = L"";                break;
    }
    if (hwndSearchEdit_ != nullptr && CurrentSearchText() != expr) {
        ::SetWindowTextW(hwndSearchEdit_, expr);
    }
    // Re-arm the debounce explicitly, exactly as ClearFilterBox does: a
    // programmatic SetWindowText does not guarantee EN_CHANGE, and without the
    // re-arm a box that already held the same text would not re-apply.
    ::KillTimer(hwnd_, kFilterTimerId);
    ::SetTimer(hwnd_, kFilterTimerId, kFilterDebounceMs, nullptr);
    SyncQuickFilterChecks();
}

void MainWindow::SyncQuickFilterChecks() {
    const HMENU bar = ::GetMenu(hwnd_);
    if (bar == nullptr) return;
    const wchar_t* want = nullptr;
    {
        const std::wstring cur = CurrentSearchText();
        if (cur == L"proto:tcp") want = L"proto:tcp";
        else if (cur == L"proto:udp") want = L"proto:udp";
        else if (cur == L"state:listen") want = L"state:listen";
        else if (cur == L"state:estab") want = L"state:estab";
        else if (cur == L"local:private") want = L"local:private";
        else if (cur.empty()) want = L"";
    }
    const int ids[] = {IDM_QFILTER_ALL,   IDM_QFILTER_TCP,    IDM_QFILTER_UDP,
                       IDM_QFILTER_LISTEN, IDM_QFILTER_ESTAB, IDM_QFILTER_MINE};
    for (int id : ids) {
        bool on = false;
        switch (id) {
            case IDM_QFILTER_ALL:     on = (want != nullptr && *want == L'\0'); break;
            case IDM_QFILTER_TCP:     on = (want != nullptr && ::wcscmp(want, L"proto:tcp") == 0); break;
            case IDM_QFILTER_UDP:     on = (want != nullptr && ::wcscmp(want, L"proto:udp") == 0); break;
            case IDM_QFILTER_LISTEN:  on = (want != nullptr && ::wcscmp(want, L"state:listen") == 0); break;
            case IDM_QFILTER_ESTAB:   on = (want != nullptr && ::wcscmp(want, L"state:estab") == 0); break;
            case IDM_QFILTER_MINE:    on = (want != nullptr && ::wcscmp(want, L"local:private") == 0); break;
            default: break;
        }
        ::CheckMenuItem(bar, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }
}
void MainWindow::ToggleColumn(int columnId) {
    if (columnId < 0 || columnId >= COL_COUNT) return;
    RebuildColumns();                     // harvests current widths first
    const UINT32 bit = 1u << columnId;
    // Never let the last visible column be hidden: the list would render
    // empty and the only way back is this same menu.
    if ((visibleCols_ & bit) != 0 && (visibleCols_ & bit) == visibleCols_) {
        ::MessageBoxW(hwnd_,
                      L"At least one column must stay visible.",
                      L"Columns", MB_OK | MB_ICONINFORMATION);
        return;
    }
    visibleCols_ ^= bit;
    SyncColumnMenuChecks();
    // Showing a traffic column with no source running would
    // leave it stuck on "—". Try the ETW counters once (elevated fills
    // immediately, UDP included) and fall back to the non-admin socket
    // totals otherwise; UpdateStatusBar explains whichever state applies.
    if (IsTrafficColumn(columnId)) {
        if ((visibleCols_ & (1u << columnId)) != 0) {
            EnsureTrafficCounters();
        } else if (!AnyTrafficColVisible() && fallbackFlag_->load()) {
            // Last traffic column hidden: stop paying for socket scans.
            // Totals are retained in socketTraffic_, so re-showing the
            // column resumes without losing history.
            fallbackFlag_->store(false);
        }
    }
    RebuildColumns();
    ApplyView();
}

bool MainWindow::AnyTrafficColVisible() const {
    return (visibleCols_ & ((1u << COL_TRAFFIC) | (1u << COL_RX) |
                            (1u << COL_TX) | (1u << COL_NETTOTAL))) != 0;
}

void MainWindow::EnsureTrafficCounters() {
    // whichever source can run fills the traffic columns.
    //  1) ETW kernel logger (elevated): full fidelity incl. UDP - tried
    //     silently at most once per session (this path was not requested
    //     by the user; the explicit menu toggle still reports why).
    //  2) otherwise the non-admin socket fallback (SIO_TCP_INFO) supplies
    //     per-PID TCP totals - also silently, because it succeeds: there
    //     is nothing to warn about.
    if (etw_.Running()) return;
    if (!etwAutoAttempted_) {
        etwAutoAttempted_ = true;
        std::wstring err;
        if (etw_.Start(err)) {
            settings_.trafficEnabled = true;
            fallbackFlag_->store(false);   // ETW is authoritative
            SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_TRAFFIC, true);
            Refresh(false);               // re-apply counters to the rows
            return;
        }
    }
    if (!fallbackFlag_->load() && socketTraffic_->Supported()) {
        fallbackFlag_->store(true);
        Refresh(false);                   // one pass so cells fill now
    }
}

void MainWindow::RecreateFont() {
    // F5.15. No handle is created or destroyed here any more. The cache owns every
    // font the process uses, so a DPI change is one call to OnDpiChanged and the
    // next Get() is correct by construction - instead of three windows each
    // releasing their own handles and one of them chasing a copy held elsewhere.
    FontCache::Get().OnDpiChanged();
    font_ = FontCache::Get().Get(kBodyPtSize, FW_NORMAL, dpi_);
    ApplyFontToChildren();
    // The details window is a separate top-level window holding a copy of
    // this handle; it must receive the replacement too (its header font is
    // derived from it), otherwise a monitor DPI change leaves it dangling.
    details_.ApplyFont(font_);
}

void MainWindow::ApplyFontToChildren() {
    HFONT f = font_;
    if (f == nullptr) f = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    for (HWND c = ::GetWindow(hwnd_, GW_CHILD); c != nullptr;
         c = ::GetWindow(c, GW_HWNDNEXT)) {
        SetChildFont(c, f);
    }
    if (hwndList_ != nullptr) {
        if (HWND hdr = ListView_GetHeader(hwndList_)) SetChildFont(hdr, f);
    }
}

void MainWindow::LayoutControls(int cx, int cy) {
    const int kMargin = S(8);
    const int kRow1Y = S(8);
    const int kRow1H = S(24);
    const int kRow2Y = S(38);
    const int kRow2H = S(22);
    const int kTopH = S(66);
    const int kStatusH = S(24);

    if (hwndRefreshBtn_ != nullptr)
        ::MoveWindow(hwndRefreshBtn_, kMargin, kRow1Y, S(90), kRow1H, TRUE);
    if (hwndExportBtn_ != nullptr)
        ::MoveWindow(hwndExportBtn_, kMargin + S(96), kRow1Y, S(100), kRow1H, TRUE);
    if (hwndAutoChk_ != nullptr)
        ::MoveWindow(hwndAutoChk_, kMargin + S(202), kRow1Y, S(104), kRow1H, TRUE);
    if (hwndIntervalCbo_ != nullptr)
        ::MoveWindow(hwndIntervalCbo_, kMargin + S(310), kRow1Y, S(100), S(200), TRUE);
    if (hwndProtoCbo_ != nullptr)
        ::MoveWindow(hwndProtoCbo_, kMargin + S(416), kRow1Y, S(190), S(220), TRUE);
    if (hwndStateCbo_ != nullptr)
        ::MoveWindow(hwndStateCbo_, kMargin + S(612), kRow1Y, S(140), S(240), TRUE);

    HWND hwndLabel = ::GetDlgItem(hwnd_, IDC_STATIC_FILTER);
    if (hwndLabel != nullptr)
        ::MoveWindow(hwndLabel, kMargin, kRow2Y + S(3), S(40), kRow2H, TRUE);
    if (hwndSearchEdit_ != nullptr)
        ::MoveWindow(hwndSearchEdit_, kMargin + S(44), kRow2Y, S(420), kRow2H, TRUE);

    if (hwndStatus_ != nullptr)
        ::MoveWindow(hwndStatus_, 0, cy - kStatusH, cx, kStatusH, TRUE);
    if (hwndList_ != nullptr) {
        int listH = cy - kTopH - kStatusH - kMargin;
        if (listH < S(40)) listH = S(40);
        ::MoveWindow(hwndList_, kMargin, kTopH, cx - 2 * kMargin, listH, TRUE);
    }
}

void MainWindow::OnSize(int cx, int cy) {
    LayoutControls(cx, cy);
    if (hwndStatus_ != nullptr) {
        // Six parts: counts, live state breakdown, last refresh time,
        // selection, refresh age / next tick, and the traffic-source or
        // error state. The first two grow with the window; the rest are
        // fixed-width from the right, so a narrow window truncates the
        // informational panes first. All part coordinates are clamped so a
        // very small client area cannot produce a negative or inverted
        // split, and they are DPI-scaled.
        const int w = (cx < 100) ? 100 : cx;
        // Right-hand panes, measured from the right edge inwards.
        const int wRight  = S(170);   // traffic source / error
        const int wAge    = S(150);   // age + countdown
        const int wSel    = S(110);   // selected count
        const int wTime   = S(150);   // last refresh time
        const int minLeft = S(140);   // never squeeze the counts pane to nil

        int right = w;
        const auto place = [&](int width) {
            right -= width;
            if (right < minLeft) right = minLeft;
            return right;
        };
        const int p5 = place(wRight);
        const int p4 = place(wAge);
        const int p3 = place(wSel);
        int p2 = place(wTime);
        const int p1 = minLeft;
        if (p2 < p1) p2 = p1;

        const int parts[6] = {p1, p2, p3, p4, p5, -1};
        ::SendMessageW(hwndStatus_, SB_SETPARTS, 6,
                       reinterpret_cast<LPARAM>(parts));
    }
}

// ---------------------------------------------------------------------------
// view (filter + sort + repaint, tasks 5/7/16) -------------------------------

std::wstring MainWindow::CurrentSearchText() const {
    // Sized from kSearchTextMaxChars, which is also the control's EM_LIMITTEXT,
    // so this read can never truncate what the user typed. Deliberately NOT
    // _countof: the capacity that matters is the one the control was bounded
    // to, and spelling it as the constant makes the two impossible to drift.
    wchar_t buf[kSearchTextMaxChars] = {0};
    ::GetWindowTextW(hwndSearchEdit_, buf,
                     static_cast<int>(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf);
}

unsigned MainWindow::CurrentProtoFilterMask() const {
    const LRESULT sel = ::SendMessageW(hwndProtoCbo_, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return kProtoMaskAll;
    const LRESULT data = ::SendMessageW(hwndProtoCbo_, CB_GETITEMDATA,
                                        static_cast<WPARAM>(sel), 0);
    if (data == CB_ERR || data == 0) return kProtoMaskAll;
    return static_cast<unsigned>(data);
}

DWORD MainWindow::CurrentStateFilter() const {
    const LRESULT sel = ::SendMessageW(hwndStateCbo_, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return kAllStates;
    const LRESULT data = ::SendMessageW(hwndStateCbo_, CB_GETITEMDATA,
                                        static_cast<WPARAM>(sel), 0);
    if (data == CB_ERR) return kAllStates;
    return static_cast<DWORD>(data);
}

void MainWindow::ApplyView() {
    std::vector<std::uint64_t> ids;
    std::uint64_t focusedId = 0;
    int topIdx = 0;
    CaptureSelection(ids, &focusedId, &topIdx);
    ApplyViewWith(ids, focusedId, topIdx);
}

void MainWindow::ApplyViewWith(const std::vector<std::uint64_t>& ids,
                               std::uint64_t focusedId, int topIdx) {
    // Assemble the view and hand it to ViewState, which is the ONLY thing that
    // turns a description into a built view. This method used to do that work
    // itself - build a ViewQuery by hand, parse the filter, set the view - and
    // a CLI had no way to build the same view except by copying those lines,
    // which is how the two front ends drifted apart last time (the CLI carried
    // a private copy of the snapshot pipeline and had silently fallen behind
    // the GUI's). One builder, both callers.
    //
    // The GUI's contribution is reading the widgets into a ViewState; every
    // decision about what that state MEANS belongs to ViewState::ApplyTo.
    ViewState v;
    v.filter = CurrentSearchText();
    v.protoMask = CurrentProtoFilterMask();
    v.stateFilter = CurrentStateFilter();
    v.sortColumn = store_.SortColumn();
    v.sortAsc = store_.SortAscending();
    v.grouped = groupByProcess_;
    v.frozen = frozen_;
    v.frozenAtMs = frozenAt_;
    v.preserveSelection = preserveSelection_;
    v.colVisible = visibleCols_;
    // ApplyTo hands back the parsed program so the filter is parsed ONCE here
    // rather than again below; filterProgram_ is a derived cache of v.filter,
    // kept for the two callers that ask "is a text filter active?" without
    // re-reading the box.
    v.ApplyTo(&store_, &filterProgram_);

    // 9.4.2. Recomputed here rather than only at startup because this is the
    // single funnel every path goes through: startup, a refresh result, a column
    // toggle, a preset, a GeoIP pick and a freeze all end here, and at this point
    // the row count, the column mask and both database states are simultaneously
    // valid.
    UpdateEmptyState();
    const size_t count = store_.View().size();
    ::SendMessageW(hwndList_, LVM_SETITEMCOUNT, static_cast<WPARAM>(count),
                   LVSICF_NOINVALIDATEALL);
    RestoreSelection(ids, focusedId, topIdx);
    ::InvalidateRect(hwndList_, nullptr, TRUE);
    UpdateStatusBar(lastError_);
}

void MainWindow::CaptureSelection(std::vector<std::uint64_t>& ids,
                                  std::uint64_t* focusedId, int* topIdx) const {
    ids.clear();
    *focusedId = 0;
    *topIdx = 0;
    if (hwndList_ == nullptr) return;
    int item = -1;
    while ((item = static_cast<int>(::SendMessageW(
                hwndList_, LVM_GETNEXTITEM, static_cast<WPARAM>(item),
                MAKELPARAM(LVNI_SELECTED, 0)))) != -1) {
        const Connection* c = store_.ViewRow(static_cast<size_t>(item));
        if (c != nullptr) ids.push_back(c->id);
    }
    const int f = static_cast<int>(::SendMessageW(
        hwndList_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
        MAKELPARAM(LVNI_FOCUSED, 0)));
    const Connection* fc = store_.ViewRow(static_cast<size_t>(f));
    if (fc != nullptr) *focusedId = fc->id;
    *topIdx = static_cast<int>(::SendMessageW(hwndList_, LVM_GETTOPINDEX, 0, 0));
}

void MainWindow::RestoreSelection(const std::vector<std::uint64_t>& ids,
                                  std::uint64_t focusedId, int topIdx) {
    if (hwndList_ == nullptr) return;

    // id -> view position, built once for all lookups.
    std::unordered_map<std::uint64_t, size_t> pos;
    const size_t count = store_.View().size();
    pos.reserve(count * 2 + 1);
    for (size_t v = 0; v < count; ++v) {
        const Connection* c = store_.ViewRow(v);
        if (c != nullptr) pos[c->id] = v;
    }

    LVITEMW st = {};
    st.mask = LVIF_STATE;
    st.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    ::SendMessageW(hwndList_, LVM_SETITEMSTATE, static_cast<WPARAM>(-1),
                   reinterpret_cast<LPARAM>(&st));   // clears all

    for (std::uint64_t id : ids) {
        const auto it = pos.find(id);
        if (it == pos.end()) continue;
        LVITEMW sel = {};
        sel.mask = LVIF_STATE;
        sel.stateMask = LVIS_SELECTED;
        sel.state = LVIS_SELECTED;
        ::SendMessageW(hwndList_, LVM_SETITEMSTATE,
                       static_cast<WPARAM>(it->second),
                       reinterpret_cast<LPARAM>(&sel));
    }
    if (focusedId != 0) {
        const auto it = pos.find(focusedId);
        if (it != pos.end()) {
            LVITEMW fo = {};
            fo.mask = LVIF_STATE;
            fo.stateMask = LVIS_FOCUSED;
            fo.state = LVIS_FOCUSED;
            ::SendMessageW(hwndList_, LVM_SETITEMSTATE,
                           static_cast<WPARAM>(it->second),
                           reinterpret_cast<LPARAM>(&fo));
        }
    }

    // Best-effort scroll preservation. Set the top index directly instead
    // of LVM_SCROLL with a pixel delta: the old code measured the row
    // height from item 0 (not the first visible item) and could scroll
    // far past the end of a list that shrank under the filter.
    //
    // LVM_SETTOPINDEX is behind an NTDDI_VERSION guard that the Win7
    // _WIN32_WINNT=0x0601 target hides, so the message is defined here
    // rather than bumping the project's minimum OS version for one call.
    const UINT msgSetTopIndex = LVM_FIRST + 119;   // LVM_SETTOPINDEX
    if (topIdx > 0 && count > 0) {
        const int cur = static_cast<int>(::SendMessageW(hwndList_,
                        LVM_GETTOPINDEX, 0, 0));
        const int maxTop = static_cast<int>(count) - 1;
        const int want = (topIdx < maxTop) ? topIdx : maxTop;
        if (cur >= 0 && cur != want)
            ::SendMessageW(hwndList_, msgSetTopIndex,
                           static_cast<WPARAM>(want), 0);
    }
}

void MainWindow::UpdateStatusBar(const std::wstring& errorText) {
    // Counts come from the snapshot pass, not from rescanning every row -
    // this runs on every debounced keystroke via ApplyView().
    const RowStats& st = store_.Stats();
    wchar_t statusText[320] = {0};
    // A live state breakdown is what an analyst actually reads off the bar;
    // listing every state that happens to be non-zero keeps it quiet on an
    // idle machine and informative on a busy one.
    ::swprintf_s(statusText, L"%zu shown / %zu  ·  v4 %zu · v6 %zu · UDP %zu",
                 store_.View().size(), st.total, st.ipv4, st.ipv6, st.udp);
    std::wstring states;
    for (DWORD s : {static_cast<DWORD>(MIB_TCP_STATE_ESTAB),
                    static_cast<DWORD>(MIB_TCP_STATE_LISTEN),
                    static_cast<DWORD>(MIB_TCP_STATE_TIME_WAIT),
                    static_cast<DWORD>(MIB_TCP_STATE_CLOSE_WAIT),
                    static_cast<DWORD>(MIB_TCP_STATE_SYN_SENT)}) {
        const size_t n = st.StateCount(s);
        if (n == 0) continue;
        if (!states.empty()) states += L" · ";
        states += TcpStateToString(s);
        states += L" ";
        states += std::to_wstring(n);
    }
    if (!states.empty()) states += L"  ·  ";
    states += L"UDP ";
    states += std::to_wstring(st.udp);
    if (st.secure != 0) {
        states += L"  ·  TLS ";
        states += std::to_wstring(st.secure);
    }
    if (st.pinned != 0) {
        states += L"  ·  ★ ";
        states += std::to_wstring(st.pinned);
    }
    // A visible traffic column with no source running renders
    // as a wall of "—". Say so instead of leaving the columns silently
    // empty; real errors keep priority. Three states:
    //   * ETW session running        -> "Ready" (full fidelity, UDP too)
    //   * socket fallback active     -> brief "TCP only" note + tooltip
    //   * nothing can run            -> the admin hint
    // Pane 2 is only ~170px wide (OnSize), so keep the labels short and
    // put the actionable detail in the pane's tooltip.
    const bool trafficColShown = AnyTrafficColVisible();
    const bool fallbackActive =
        trafficColShown && !etw_.Running() && fallbackFlag_->load();
    const bool trafficBlocked = trafficColShown && !etw_.Running() &&
                                !fallbackFlag_->load();
    // 9.2.4: what the traffic scan could not measure, said where the traffic
    // columns are. A blank traffic cell means three different things and the
    // reader could previously not tell them apart - the connection moved no
    // bytes, the row was unmeasurable, or the SCAN FAILED. Only the third is
    // actionable, so it gets the pane.
    //
    // Both counters are 0 on a healthy machine, so this is silent until it is
    // not, which is the point of a status pane. Failure outranks staleness
    // because a failed pass measured nothing while a dropped pass kept the
    // previous totals - and "nothing" is the worse of the two to trust.
    const unsigned scanGaps =
        (socketTraffic_ != nullptr) ? socketTraffic_->ScanFailureCount() : 0u;
    const unsigned staleGaps =
        (socketTraffic_ != nullptr) ? socketTraffic_->TimeoutCount() : 0u;
    // 9.2.1: name the traffic source in the status bar. "Ready" alone does not
    // distinguish a full ETW session (TCP+UDP, kernel-level) from a socket
    // fallback (TCP only, per-socket ioctl) - two very different fidelity levels
    // that the rest of the pane already distinguishes (see fallbackActive).
    std::wstring right =
        !errorText.empty()      ? (L"Error: " + errorText)
        : scanGaps != 0         ? L"Traffic FAILED"
        : staleGaps != 0        ? L"Traffic stale"
        : fallbackActive        ? L"TCP only - UDP needs admin"
        : trafficBlocked        ? L"Traffic off - needs admin"
                                 : etw_.Running()        ? L"Ready (ETW)"
                                : L"Ready";
    // 9.2.8: append the DNS-stall hint so a user knows some Host cells show
    // `pending` rather than `—`.
    // 9.4.2: the empty-state strip. One sentence per case, and the sentence is the
    // ACTION rather than a description of a blank - "no country database loaded -
    // pick one in View > GeoIP database", not "Country: empty".
    //
    // Recomputed from live state every second by OnTimer through this same
    // function, so there is no timer to write and nothing to clean up. That is
    // the shape dnsStalledHint_ already uses and the only time-scoped message
    // channel the window has: there is no toolbar and no WM_PAINT hook in this
    // window, so a banner over the list would be new chrome rather than a reuse.
    // F5.6. Same channel and same shape as the empty-state hint beside it, and
    // deliberately NOT stored: nothing about an alert survives a refresh, so a
    // cleared alert stops being reported without anyone removing it.
    if (!alertHint_.empty()) {
        if (!right.empty()) right += L"  \xB7  ";
        right += alertHint_;
    }    if (!emptyStateHint_.empty()) {
        if (!right.empty()) right += L"  ";
        right += emptyStateHint_;
    }

    // 5.5: while frozen, pane 2 must say so AND say how stale it is. Showing
    // the pre-freeze "Updated: 14:32:05" on its own would be a lie - it reads
    // as current, and the user has no way to tell the list is not updating.
    // FROZEN takes priority over the timestamp for exactly that reason.
    std::wstring mid;
    if (frozen_) {
        // FrozenAgeMs returns MILLISECONDS; FormatDuration takes seconds.
        // Dividing is not enough on its own: a view frozen for 900 ms would
        // render "0s", indistinguishable from one frozen for 0 ms, so a
        // sub-second freeze reads as no freeze at all. Show "<1s" instead.
        const ULONGLONG age = FrozenAgeMs(frozenAt_, ::GetTickCount64());
        const std::wstring ageText =
            (age < 1000) ? std::wstring(L"<1s")
                         : FormatDuration(age / kMsPerSecond);
        mid = L"FROZEN " + ageText + L" — F6 to resume";
    } else {
        mid = lastRefreshTime_.empty()
                  ? L"Not refreshed yet"
                  : (L"Updated: " + lastRefreshTime_);
    }
    ::SendMessageW(hwndStatus_, SB_SETTEXTW, 0,
                   reinterpret_cast<LPARAM>(statusText));
    ::SendMessageW(hwndStatus_, SB_SETTEXTW, 1,
                   reinterpret_cast<LPARAM>(states.c_str()));
    ::SendMessageW(hwndStatus_, SB_SETTEXTW, 2,
                   reinterpret_cast<LPARAM>(mid.c_str()));
    ::SendMessageW(hwndStatus_, SB_SETTEXTW, 3,
                   reinterpret_cast<LPARAM>(SelectedSummary().c_str()));
    ::SendMessageW(hwndStatus_, SB_SETTEXTW, 4,
                   reinterpret_cast<LPARAM>(RefreshAgeText().c_str()));
    ::SendMessageW(hwndStatus_, SB_SETTEXTW, 5,
                   reinterpret_cast<LPARAM>(right.c_str()));
    // The tooltip is where the ACTIONABLE detail goes, because pane 5 is only
    // ~170px wide (OnSize). 9.2.4 puts the two gap explanations ahead of the
    // elevation ones: a machine that dropped a pass needs to know that before
    // it needs to know it should have been run elevated.
    std::wstring tip;
    if (scanGaps != 0) {
        tip = L"Traffic is UNMEASURED for this run: " + std::to_wstring(scanGaps) +
              L" scan(s) could not read the process handle table, so nothing was"
              L" merged and every traffic cell is blank rather than zero. This"
              L" is not an elevation problem and no switch fixes it - the handle"
              L" table was denied. Re-run; if it persists, something on this"
              L" machine is blocking handle enumeration.";
    } else if (staleGaps != 0) {
        tip = L"Traffic is STALE: " + std::to_wstring(staleGaps) +
              L" pass(es) were dropped because a socket's SIO_TCP_INFO never"
              L" returned. The counters shown are the last values that were"
              L" read, not current ones. Closing the owning connection usually"
              L" clears it - the sockets are remembered, so no later pass wastes"
              L" a worker on them again.";
    } else if (fallbackActive) {
        tip = L"TCP totals are read per-socket (SIO_TCP_INFO) - no elevation"
              L" needed. UDP rows stay \"-\": run WinTCP as administrator and"
              L" enable View > Per-PID traffic counters for the full ETW"
              L" session (TCP+UDP).";
    } else if (trafficBlocked) {
        tip = L"Per-PID traffic counters need elevation: run WinTCP as"
              L" administrator (View > Per-PID traffic counters).";
    }
    // Empty tip means "nothing to add", which is what the old ternary's 0
    // meant; setting an empty string instead would create an empty tooltip
    // balloon on every healthy refresh.
    ::SendMessageW(hwndStatus_, SB_SETTIPTEXTW, 5,
                    tip.empty() ? 0 : reinterpret_cast<LPARAM>(tip.c_str()));
}

std::wstring MainWindow::SelectedSummary() const {
    // Counted with one LVM_GETSELECTEDCOUNT rather than walking the
    // selection: a multi-row selection on a 50k virtual list must not cost
    // a round trip per selected row on every status-bar refresh.
    const LRESULT n = ::SendMessageW(hwndList_, LVM_GETSELECTEDCOUNT, 0, 0);
    if (n <= 0) return L"no selection";
    if (n == 1) return L"1 selected";
    wchar_t buf[48] = {0};
    ::swprintf_s(buf, L"%lld selected", static_cast<long long>(n));
    return buf;
}

std::wstring MainWindow::RefreshAgeText() const {
    if (lastRefreshTick_ == 0) return L"—";
    const ULONGLONG ageSec =
        (::GetTickCount64() - lastRefreshTick_) / kMsPerSecond;
    wchar_t buf[64] = {0};
    if (autoRefresh_) {
        // Countdown to the next tick, derived from the interval timer that
        // is already running - no second timer and no drift of its own.
        const ULONGLONG nextDue = lastRefreshTick_ + autoRefreshMs_;
        const ULONGLONG now = ::GetTickCount64();
        const ULONGLONG inSec = (nextDue > now) ? (nextDue - now) / kMsPerSecond : 0;
        ::swprintf_s(buf, L"%llus old · next in %llus",
                     ageSec, inSec);
    } else {
        ::swprintf_s(buf, L"%llus old", ageSec);
    }
    return buf;
}

// ---------------------------------------------------------------------------
// notifications ---------------------------------------------------------------

void MainWindow::OnCommand(WORD id, WORD notifyCode, HWND ctl) {
    switch (id) {
        case IDC_BTN_REFRESH:
            if (notifyCode == BN_CLICKED) Refresh(true);
            break;
        case IDM_FILE_REFRESH:
        case IDM_CTX_REFRESH:
            Refresh(true);
            break;
        case IDC_BTN_EXPORT:
            if (notifyCode == BN_CLICKED) ExportCsv();
            break;
        case IDM_FILE_EXPORT:
            ExportCsv();
            break;
        case IDC_CHK_AUTOREFRESH:
            if (notifyCode == BN_CLICKED) {
                autoRefresh_ = (::SendMessageW(hwndAutoChk_, BM_GETCHECK, 0, 0) ==
                                BST_CHECKED);
                SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_AUTOREFRESH, autoRefresh_);
                if (autoRefresh_) {
                    ::SetTimer(hwnd_, kRefreshTimerId, autoRefreshMs_, nullptr);
                    Refresh(false);
                } else {
                    ::KillTimer(hwnd_, kRefreshTimerId);
                }
            }
            break;
        case IDC_CBO_INTERVAL:
            if (notifyCode == CBN_SELCHANGE) {
                const LRESULT sel = ::SendMessageW(hwndIntervalCbo_, CB_GETCURSEL, 0, 0);
                LRESULT ms = static_cast<LRESULT>(kDefaultIntervalMs);
                if (sel != CB_ERR)
                    ms = ::SendMessageW(hwndIntervalCbo_, CB_GETITEMDATA,
                                        static_cast<WPARAM>(sel), 0);
                autoRefreshMs_ = (ms == CB_ERR || ms <= 0) ? kDefaultIntervalMs
                                                           : static_cast<UINT>(ms);
                if (autoRefresh_)
                    ::SetTimer(hwnd_, kRefreshTimerId, autoRefreshMs_, nullptr);
            }
            break;
        case IDC_CBO_PROTO:
        case IDC_CBO_STATE:
            if (notifyCode == CBN_SELCHANGE) ApplyView();
            break;
        case IDC_EDIT_SEARCH:
            if (notifyCode == EN_CHANGE) {
                // Debounce keystrokes.
                ::KillTimer(hwnd_, kFilterTimerId);
                ::SetTimer(hwnd_, kFilterTimerId, kFilterDebounceMs, nullptr);
            }
            break;
        case IDM_FILE_EXIT:
            ::DestroyWindow(hwnd_);
            break;
        case IDM_VIEW_AUTOREFRESH: {
            autoRefresh_ = !autoRefresh_;
            ::SendMessageW(hwndAutoChk_, BM_SETCHECK,
                           autoRefresh_ ? BST_CHECKED : BST_UNCHECKED, 0);
            SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_AUTOREFRESH, autoRefresh_);
            if (autoRefresh_) {
                ::SetTimer(hwnd_, kRefreshTimerId, autoRefreshMs_, nullptr);
                Refresh(false);
            } else {
                ::KillTimer(hwnd_, kRefreshTimerId);
            }
            break;
        }
        case IDM_FILE_CHANGELOG_WIN:
            // Toggle rather than always-open: a second click closes it, which
            // is what a menu item named after a window should do. Show() is
            // idempotent and re-raises an already-open window, so this only
            // has to decide between opening and closing.
            if (changeLog_.IsOpen()) {
                changeLog_.Close();
            } else {
                changeLog_.Show(hwnd_, font_);
            }
            break;
        case IDM_FILE_PRESET_SAVE:
            SavePreset();
            break;
        case IDM_FILE_PRESET_LOAD:
            LoadPreset();
            break;
        case IDM_FILE_PRESET_DELETE:
            DeletePreset();
            break;
        case IDM_FILE_BOOKMARKS:
            ShowBookmarksList();
            break;
        case IDM_CTX_BOOKMARK:
            BookmarkSelectedConnection();
            break;
        case IDM_CTX_NOTE:
            EditSelectedBookmarkNote();
            break;
        case IDM_HELP_ABOUT:
            ShowAboutBox();
            break;
        case IDM_HELP_SHORTCUTS:
            ShowShortcutsSheet();
            break;
        case IDM_CTX_COPY_SELECTED:
            CopySelectionToClipboard(false);
            break;
        case IDM_CTX_COPY_ALL:
            CopySelectionToClipboard(true);
            break;
        case IDM_CTX_DETAILS:
            ShowDetailsOfSelectedRow();
            break;
        case IDM_CTX_KILL_PROCESS:
            KillSelectedProcess();
            break;
        case IDM_CTX_CLOSE_CONNECTION:
            CloseSelectedConnection();
            break;
        case IDM_FOLLOW_STREAM:
            // REMOVED 2026-10-05: this case is unreachable - the menu entry
            // that sent IDM_FOLLOW_STREAM is gone (see OnContextMenu). Kept as
            // a comment rather than deleted so the removal is reversible and so
            // anyone re-adding the entry finds this note. The capability itself
            // was NOT lost: it is the `capture` verb, which now prints the
            // reassembled stream to stdout (--text) and can write the capture
            // out as pcapng (--out). See todo.md 8.7 G2 for why the GUI form
            // was not salvageable: it re-elevated the whole application, then
            // blocked modally on a sampling capture that can legitimately come
            // back empty.
            // FollowSelectedStream();
            break;
        case IDM_OPEN_FILE_LOCATION:
            OpenSelectedFileLocation();
            break;
        // REMOVED 2026-10-05: IDM_PROCESS_PROPERTIES / ShowSelectedProcessProperties.
        // See the note in OnContextMenu for why the verb was removed rather than
        // repaired, and todo.md 8.7 G1.
        // case IDM_PROCESS_PROPERTIES:
        //     ShowSelectedProcessProperties();
        //     break;
        case IDM_BLOCK_CONNECTION:
            // 9.2.5: BlockSelectedConnection() writes firewall rules, which need
            // an elevated token. If the user is an admin who is not elevated,
            // hand off to Reelevate() (this call exits the old process) - now a
            // live caller. If the account cannot elevate at all, show why and
            // do not attempt the block (it would fail inside BlockConn anyway).
            if (!IsElevated()) {
                if (IsAdminMember() && Reelevate(L"Block connection")) return;
                const std::wstring reason = ElevationUnavailableReason();
                ::MessageBoxW(hwnd_, reason.c_str(), L"WinTCP - Blocked requires elevation",
                              MB_OK | MB_ICONINFORMATION);
                return;
            }
            BlockSelectedConnection();
            break;
        case IDM_TRAY_UNBLOCK_ALL:
            RemoveAllWinTcpBlocks();
            break;
        case IDM_VIEW_RESOLVE: {
            dnsEnabled_ = !dnsEnabled_;
            dns_.SetEnabled(dnsEnabled_);
            SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_RESOLVE, dnsEnabled_);
            if (dnsEnabled_) OfferDnsForAllRows();
            break;
        }
        case IDM_VIEW_PRESERVE_SEL:
            preserveSelection_ = !preserveSelection_;
            SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_PRESERVE_SEL,
                         preserveSelection_);
            break;
        case IDM_VIEW_ASNIP:
            LoadAsnDatabase();
            break;
        // F5.11. The quick filters write the box and let the debounce do the
        // work, so there is exactly one code path from a click to a filtered
        // view - the same one typing uses. Going straight to ApplyView here
        // would skip the EN_CHANGE path and leave the two able to disagree.
        case IDM_QFILTER_ALL:
        case IDM_QFILTER_TCP:
        case IDM_QFILTER_UDP:
        case IDM_QFILTER_LISTEN:
        case IDM_QFILTER_ESTAB:
        case IDM_QFILTER_MINE:
            ApplyQuickFilter(static_cast<int>(id));
            break;
        case IDM_QFILTER_CLEAR:
            FocusFilterBox();
            break;
        // 9.4.4.
        case IDM_PROFILE_DEFAULT:
        case IDM_PROFILE_MINIMAL:
        case IDM_PROFILE_NETWORK:
        case IDM_PROFILE_SECURITY:
        case IDM_PROFILE_PERFORMANCE:
        case IDM_PROFILE_DIAGNOSTICS:
            ApplyColumnProfile(static_cast<int>(id));
            break;
        case IDM_VIEW_GEOIP:
            LoadGeoIpDatabase();
            break;
        case IDM_VIEW_FREEZE:
            frozen_ = !frozen_;
            // Stamped from GetTickCount64 (a monotonic counter), not from
            // wall-clock time, so a clock change cannot make the frozen age
            // jump or run backwards.
            frozenAt_ = frozen_ ? ::GetTickCount64() : 0;
            SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_FREEZE, frozen_);
            UpdateStatusBar(std::wstring());
            break;
        case IDM_VIEW_GROUP:
            groupByProcess_ = !groupByProcess_;
            store_.SetGrouped(groupByProcess_);
            SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_GROUP, groupByProcess_);
            // The view is rebuilt from the same rows either way, so this is
            // safe to do mid-selection: the row ids are unchanged and
            // RestoreSelection puts the user's row back.
            ApplyView();
            break;
        case IDM_FILE_CHANGELOG:
            ToggleChangeLog();
            break;
        case IDM_CTX_EXPORT_SELECTION:
            DoExport(true);
            break;
        case IDM_EDIT_SELECTALL:
            SelectAllRows();
            break;
        case IDM_EDIT_FOCUSFILTER:
            FocusFilterBox();
            break;
        case IDM_EDIT_CLEARFILTER:
            ClearFilterBox();
            break;
        case IDM_VIEW_TOPMOST:
            topMost_ = !topMost_;
            settings_.topMost = topMost_;
            SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_TOPMOST, topMost_);
            ::SetWindowPos(hwnd_, topMost_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                           0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            break;
        case IDM_VIEW_TRAY:
            trayEnabled_ = !trayEnabled_;
            settings_.trayEnabled = trayEnabled_;
    settings_.trayMinimizeChoice = trayMinimizeChoice_;
            SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_TRAY, trayEnabled_);
            if (trayEnabled_) TrayAdd(); else TrayRemove();
            break;
        case IDM_TRAY_RESTORE:
            ::ShowWindow(hwnd_, SW_RESTORE);
            ::SetForegroundWindow(hwnd_);
            break;
        case IDM_VIEW_TRAFFIC: {
            // Toggle the ETW per-PID traffic counters.
            if (etw_.Running()) {
                etw_.Stop();
                settings_.trafficEnabled = false;
                // The user asked to stop counting: the non-admin fallback
                // must not silently keep feeding the columns either.
                fallbackFlag_->store(false);
                SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_TRAFFIC, false);
                store_.ClearTraffic();
                ApplyView();
            } else {
                // 9.2.5: starting a full ETW session needs elevation. If the user
                // is an admin who is not currently elevated, hand off to Reelevate()
                // (this call exits the old process - the new, elevated one resumes
                // here from settings_.trafficEnabled) rather than letting Start()
                // fail noisily below. A standard account shows the reason instead.
                if (!IsElevated()) {
                    if (IsAdminMember() && Reelevate(L"Traffic counters")) return;
                    const std::wstring reason = ElevationUnavailableReason();
                    ::MessageBoxW(hwnd_, reason.c_str(),
                                  L"WinTCP - Traffic counters require elevation",
                                  MB_OK | MB_ICONINFORMATION);
                    return;
                }
                std::wstring etwError;
                if (etw_.Start(etwError)) {
                    settings_.trafficEnabled = true;
                    fallbackFlag_->store(false);   // ETW authoritative
                    SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_TRAFFIC, true);
                    Refresh(false);        // pick counters up promptly
                } else {
                    // Graceful fallback: explain, keep the app running.
                    SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_TRAFFIC, false);
                    if (fallbackFlag_->load()) {
                        etwError +=
                            L"\r\n\r\nNote: per-socket TCP totals are already "
                            L"being collected\r\nwithout administrator rights "
                            L"- only UDP (and the\r\nfull-fidelity session) "
                            L"need elevation.";
                    }
                    ::MessageBoxW(hwnd_, etwError.c_str(),
                                  L"WinTCP - Traffic counters",
                                  MB_OK | MB_ICONWARNING);
                }
            }
            break;
        }
        case IDM_VIEW_CHARTS: {
            // toggle the performance graphs window.
            if (charts_.IsVisible()) {
                charts_.Close();
                SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_CHARTS, false);
            } else {
                charts_.Show(hwnd_, ThemeIsDark());
                SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_CHARTS, true);
            }
            break;
        }
        default:
            if (id >= IDM_COL_BASE && id < IDM_COL_BASE + IDM_COL_COUNT)
                ToggleColumn(static_cast<int>(id - IDM_COL_BASE));
            break;
    }
    (void)ctl;
}

void MainWindow::OnNotify(NMHDR* hdr, LPARAM /*lParam*/) {
    if (hdr == nullptr || hdr->hwndFrom != hwndList_) return;
    switch (hdr->code) {
        case LVN_GETDISPINFO: {
            auto* di = reinterpret_cast<NMLVDISPINFO*>(hdr);
            if ((di->item.mask & LVIF_TEXT) == 0) break;
            dispBuf_[0] = L'\0';
            const size_t viewIdx = static_cast<size_t>(di->item.iItem);
            const Connection* c = store_.ViewRow(viewIdx);
            if (c != nullptr) {
                const int col = VisibleToCol(di->item.iSubItem);
                if (col >= 0) {
                    // 5.1: when the view is grouped, a row is a process, not
                    // a connection. The aggregate columns show the group's
                    // totals; everything else falls through to the underlying
                    // connection so addresses, state and ids stay real.
                    if (const ProcessGroup* g = store_.ViewGroup(viewIdx)) {
                        if (wintcp::GroupColumnText(*g, col, dispBuf_,
                                                    kMaxColumnText)) {
                            di->item.pszText = dispBuf_;
                            break;
                        }
                    }
                    ConnectionStore::GetColumnText(*c, col, dispBuf_,
                                                   kMaxColumnText);
                }
            }
            di->item.pszText = dispBuf_;
            break;
        }
        case LVN_COLUMNCLICK: {
            const auto* click = reinterpret_cast<const NMLISTVIEW*>(hdr);
            const int col = VisibleToCol(click->iSubItem);
            if (col < 0) break;
            bool asc = true;
            if (col == store_.SortColumn()) asc = !store_.SortAscending();
            store_.SetSort(col, asc);
            UpdateSortIndicator();
            ApplyView();
            break;
        }
        case NM_DBLCLK:
            ShowDetailsOfSelectedRow();
            break;
        case LVN_GETINFOTIP: {
            // 7.8: full-value tooltips.
            //
            // The compact columns are abbreviated to fit their width, so a
            // 12-digit byte count or a long path is elided with an ellipsis
            // the reader cannot expand. The tooltip shows the value in full,
            // one item per line, for the columns where truncation actually
            // loses information.
            auto* info = reinterpret_cast<NMLVGETINFOTIPW*>(hdr);
            const int col = VisibleToCol(info->iSubItem);
            const Connection* c =
                store_.ViewRow(static_cast<size_t>(info->iItem));
            if (col < 0 || c == nullptr) break;

            std::wstring tip;
            const auto add = [&tip](const wchar_t* label,
                                    const std::wstring& value) {
                if (!tip.empty()) tip += L"\r\n";
                tip += label;
                tip += L": ";
                tip += value;
            };
            const auto bps = [](double v) {
                wchar_t b[48] = {0};
                ::swprintf_s(b, L"%.1f B/s", v);
                return std::wstring(b);
            };

            switch (col) {
                case COL_TRAFFIC:
                    // The cell reads "1.2 MB / 340 kB"; the tooltip separates
                    // the directions and names them, because "which way is
                    // this?" is the question the compact form raises.
                    add(L"Received", FormatBytes(c->trafficRx));
                    add(L"Sent", FormatBytes(c->trafficTx));
                    if (c->hostname == DnsResolver::kPendingHost)
                        add(L"Host", L"(resolving...)");
                    else if (!c->hostname.empty())
                        add(L"Host", c->hostname);
                    break;
                case COL_BANDWIDTH:
                    if (c->bpsKnown) {
                        add(L"Down", bps(c->rxBps));
                        add(L"Up", bps(c->txBps));
                    } else {
                        // Say WHY there is no figure instead of showing an
                        // em-dash that reads as "zero traffic".
                        add(L"Speed",
                            L"not available (no counter source running)");
                    }
                    break;
                case COL_PATH:
                    if (!c->processPath.empty()) add(L"Path", c->processPath);
                    else add(L"Path", L"(not available)");
                    break;
                case COL_HOST:
                    if (c->hostname == DnsResolver::kPendingHost)
                        add(L"Host", L"(resolving...)");
                    else if (!c->hostname.empty())
                        add(L"Host", c->hostname);
                    else
                        add(L"Host", L"(not resolved)");
                    break;
                case COL_CPU:
                    if (c->cpuPct >= 0.0) {
                        wchar_t b[48] = {0};
                        ::swprintf_s(b, L"%.1f%% (whole process, not this "
                                        L"connection)", c->cpuPct);
                        add(L"CPU", b);
                    } else {
                        add(L"CPU", L"not available");
                    }
                    break;
                case COL_NOTE:
                    // 5.5. The note in full, always - it is the one column
                    // whose whole value is text the user cannot reconstruct
                    // from the cell, and the cell elides anything past the
                    // column width. Also reachable from the Bookmarks column,
                    // so a note is findable even with the Note column hidden
                    // (which is the default).
                    if (!c->note.empty()) add(L"Note", c->note);
                    break;
                case COL_PINNED:
                    // A pin with a note is the common case, and the note is
                    // only visible here when the Note column is hidden - which
                    // it is by default. Without this the only way to read a
                    // note in the GUI would be to turn on a column first.
                    if (c->pinned && !c->note.empty()) add(L"Note", c->note);
                    break;
                case COL_MEM:
                case COL_DISK:
                case COL_COUNTRY:
                default:
                    // For every other column the cell already shows the whole
                    // value, so an empty string tells the control to use its
                    // own default rather than a tooltip that says nothing.
                    break;
                case COL_TLS:
                    // The cell shows the negotiated TLS layer, but only when the
                    // handshake was captured (`capture --text`) or via ETW; an
                    // empty cell really means "we did not capture it", and a
                    // plain em-dash would read as "no TLS" rather than
                    // "not available".
                    if (c->tls.known && c->tls.secure) {
                        add(L"TLS", TlsSummary(c->tls));
                    } else {
                        // Say WHY the cell is empty instead of showing an em-dash
                        // that reads as "no TLS".
                        add(L"TLS",
                            L"not available (TLS is only populated via capture --text)");
                    }
                    break;
            }
            if (tip.empty()) break;
            // LVN_GETINFOTIP must return a pointer the control frees with
            // LocalFree. A std::wstring's buffer would be freed wrongly and
            // corrupt the heap - this is the single most important detail in
            // implementing this notification.
            const size_t bytes = (tip.size() + 1) * sizeof(wchar_t);
            wchar_t* out = static_cast<wchar_t*>(
                ::LocalAlloc(LMEM_FIXED, bytes));
            if (out == nullptr) break;
            ::memcpy(out, tip.c_str(), bytes);
            info->pszText = out;
            break;
        }
        default:
            break;
    }
}

// Column drag-reorder.
//
// THE BUG THIS FIXES. LVS_EX_HEADERDRAGDROP was already set on the list, so
// dragging a header visibly moved the column and then it snapped back: the
// list control performs the drag itself and, with no notification handler
// telling it the drag was allowed to complete, discards the result. The
// feature looked present and was in fact dead.
//
// WHY THE ORDER IS READ BACK rather than taken from the notification payload.
// There is no LVN_COLUMNDRAG and no end-of-drag notification in the Windows
// SDK. LVN_BEGINDRAG arrives when the drag STARTS, with no destination, so
// its payload cannot say where the column was dropped. Rather than infer a
// destination from a field that is not documented for this purpose, the
// header is re-read AFTER the drag: at that point its item order IS the
// user's chosen order, whatever happened in between. That is ground truth
// instead of a guess.
//
// The subtle part is the translation back to colOrder_, which is a
// permutation over ALL columns, not just the visible ones. The header only
// knows the visible subset. Writing that straight into colOrder_ would
// renumber hidden columns and scramble them. Instead each visible column is
// moved to the position its new visible rank implies, leaving every hidden
// column exactly where it was - so hiding and re-showing a column preserves
// the slot the user gave it.
void MainWindow::SyncColumnOrderFromHeader() {
    if (hwndList_ == nullptr) return;
    HWND hdr = ListView_GetHeader(hwndList_);
    if (hdr == nullptr) return;
    const int count = Header_GetItemCount(hdr);
    if (count <= 0 || static_cast<int>(visToCol_.size()) != count) return;

    // The order the control ended up in, as ColumnIds.
    std::vector<int> newVisible;
    newVisible.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int col = VisibleToCol(i);
        if (col < 0) return;   // inconsistent; leave the order alone
        newVisible.push_back(col);
    }
    if (newVisible == visToCol_) return;   // no drag actually happened

    // Fold the visible result back into a full-order permutation. Shared with
    // the selftest (Settings.h) so the rule cannot drift: a copy of this
    // logic in the window would be a second thing to keep correct.
    const std::vector<int> result =
        FoldVisibleOrder(colOrder_, visibleCols_, newVisible);
    if (result.size() != colOrder_.size()) return;   // paranoia; never happens
    colOrder_ = result;

    // Persist immediately. A reorder is an explicit user action, and losing it
    // because the app was killed afterwards would look like the feature is
    // broken. Widths are harvested first so the rebuild does not clobber the
    // layout the user just made with pre-drag widths.
    //
    // visToCol_ is deliberately NOT written here: RebuildColumns rebuilds it
    // from colOrder_, and overwriting it first would discard the mapping the
    // header state was just read through.
    HarvestColumnWidths();
    settings_.colOrderValid = true;
    for (size_t i = 0; i < colOrder_.size() && i < COL_COUNT; ++i)
        settings_.colOrder[i] = colOrder_[i];
    PersistSettings("column reorder");
    RebuildColumns();
    ApplyView();
}

// Type-to-jump (7.2). Returns true when the keystroke was consumed.
//
// The candidate list is built from the VISIBLE rows, and the label matched
// against is the process name where there is one and the local address
// otherwise - those are the two things a user can actually see and type. A
// jump therefore lands on a row that is genuinely on screen; matching against
// a hidden column could select something the user cannot see.
bool MainWindow::OnTypeJumpChar(wchar_t ch) {
    if (hwndList_ == nullptr) return false;
    const size_t count = store_.View().size();
    if (count == 0) return false;

    // Build candidates. Rebuilt on every keystroke rather than cached: a cache
    // would need the store's generation counter to stay in step with the
    // view, and getting that wrong would jump to a stale row. A few hundred
    // short string copies at human typing speed is not a cost worth
    // engineering around.
    std::vector<JumpCandidate> rows;
    rows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const Connection* c = store_.ViewRow(i);
        if (c == nullptr) continue;
        JumpCandidate jc;
        jc.id = c->id;
        jc.label = !c->processName.empty() ? c->processName : c->localAddress;
        rows.push_back(jc);
    }
    if (rows.empty()) return false;

    const int hit = typeToJump_.Feed(ch, ::GetTickCount64(), rows);
    if (hit < 0 || hit >= static_cast<int>(rows.size())) {
        // No match: leave the selection exactly where it was. Jumping to the
        // top would be a surprising way to say "nothing matched".
        return true;   // still consumed: this keystroke is a search, not a beep
    }

    // Select and scroll, anchored on the row so it ends up visible rather than
    // just selected. LVM_ENSUREVISIBLE is enough; scrolling it to the middle
    // would fight the user's own scrolling.
    LVITEMW st = {};
    st.mask = LVIF_STATE;
    st.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    ::SendMessageW(hwndList_, LVM_SETITEMSTATE, static_cast<WPARAM>(-1),
                   reinterpret_cast<LPARAM>(&st));
    LVITEMW sel = {};
    sel.mask = LVIF_STATE;
    sel.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    sel.state = LVIS_SELECTED | LVIS_FOCUSED;
    ::SendMessageW(hwndList_, LVM_SETITEMSTATE, static_cast<WPARAM>(hit),
                   reinterpret_cast<LPARAM>(&sel));
    ::SendMessageW(hwndList_, LVM_ENSUREVISIBLE, static_cast<WPARAM>(hit), 0);
    ::SetFocus(hwndList_);
    return true;
}

void MainWindow::OnTimer(UINT_PTR timerId) {
    if (timerId == kRefreshTimerId) {
        if (autoRefresh_) engine_.Request();
    } else if (timerId == kFilterTimerId) {
        ::KillTimer(hwnd_, kFilterTimerId);
        ApplyView();
    } else if (timerId == kStatusTimerId) {
        CheckRefreshWatchdog();
        // Only the age/countdown pane changes on its own, so refresh the
        // whole bar rather than tracking which panes are time-dependent.
        UpdateStatusBar(lastError_);
    }
}

// R8: refresh watchdog. Runs on the 1 s status tick; the policy (thresholds,
// verdicts) is RefreshWatchdogNext in RefreshEngine.h — this only executes.
//
// State machine: Healthy clears history (a recovered worker is forgiven).
// StaleRestart asks the worker to exit WITHOUT waiting (RequestStop — Stop()
// would block this same UI thread up to 8 s) and completes the restart over
// later ticks once the old thread is reaped. StaleGiveUp switches
// auto-refresh off loudly instead of looping restarts forever. Every branch
// preserves the invariant that the status bar never shows a live view over a
// dead worker.
void MainWindow::CheckRefreshWatchdog() {
    if (!autoRefresh_) {
        // The watchdog judges the automatic cadence only: manual refreshes
        // are user-paced by definition, and nagging about them would train
        // the user to ignore the message when it matters.
        watchdogRestarting_ = false;
        watchdogRestarts_ = 0;
        return;
    }
    const ULONGLONG now = ::GetTickCount64();
    const ULONGLONG base =
        (lastResultTick_ != 0) ? lastResultTick_ : workerStartTick_;
    if (base == 0) return;   // worker never started; nothing to judge yet
    const WatchdogAction action =
        RefreshWatchdogNext(now, base, autoRefreshMs_, watchdogRestarts_);
    if (action == WatchdogAction::Healthy) {
        if (watchdogRestarting_) {
            // A result arrived after the stop was asked: the worker was slow,
            // not dead. Cancel the stop rather than killing a live worker —
            // killing it here would inflict the frozen list this exists to
            // prevent. CancelStop runs one pass immediately, proving liveness.
            engine_.CancelStop();
        }
        watchdogRestarting_ = false;
        watchdogRestarts_ = 0;
        return;
    }
    if (action == WatchdogAction::StaleGiveUp) {
        // Terminal: restarts changed nothing. Mirror the toggle-off path
        // exactly (member, checkbox, menu, timer) so no half-state survives:
        // a dead cadence still firing Request() is the frozen list that
        // looks alive.
        watchdogRestarting_ = false;
        autoRefresh_ = false;
        ::SendMessageW(hwndAutoChk_, BM_SETCHECK, BST_UNCHECKED, 0);
        SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_AUTOREFRESH, false);
        ::KillTimer(hwnd_, kRefreshTimerId);
        wchar_t msg[192] = {0};
        ::swprintf_s(msg,
                     L"Auto-refresh stopped: no refresh for %llus despite %d "
                     L"worker restarts. Press Refresh to try again.",
                     (now - base) / kMsPerSecond, watchdogRestarts_);
        lastError_ = msg;
        UpdateStatusBar(lastError_);
        return;
    }
    // StaleRestart.
    if (!watchdogRestarting_) {
        watchdogRestarting_ = true;
        watchdogRestartTick_ = now;
        engine_.RequestStop();
        wchar_t msg[192] = {0};
        ::swprintf_s(msg,
                     L"View stale: no refresh for %llus — restarting refresh "
                     L"worker…",
                     (now - base) / kMsPerSecond);
        lastError_ = msg;
        UpdateStatusBar(lastError_);
        return;
    }
    if (engine_.ReapIfExited()) {
        // Old worker gone: start a fresh one through StartWorker wholesale
        // (sources, sink, seed), so a restarted worker is identical to a
        // first-started one — including a fresh watchdog seed, which grants
        // the full threshold before anything is judged again.
        StartWorker();
        ++watchdogRestarts_;
        return;
    }
    // Restart asked but the old worker still lives. Past another full
    // threshold the stop itself has failed (thread in kernel): spend one
    // restart from the cap and re-arm, so the cap still converges instead of
    // waiting on a thread that will never exit.
    if (RefreshWatchdogNext(now, watchdogRestartTick_, autoRefreshMs_, 0) !=
        WatchdogAction::Healthy) {
        ++watchdogRestarts_;
        watchdogRestarting_ = false;
    }
}

void MainWindow::OnContextMenu(HWND target, int x, int y) {
    if (target != hwndList_) return;
    POINT pt = {x, y};

    if (x != -1 || y != -1) {
        // Mouse invocation: select the row under the cursor first,
        // otherwise Details/Copy/Close would act on an unrelated selection.
        POINT cp = pt;
        ::ScreenToClient(hwndList_, &cp);
        LVHITTESTINFO hti = {};
        hti.pt = cp;
        const int hit = static_cast<int>(::SendMessageW(
            hwndList_, LVM_HITTEST, 0, reinterpret_cast<LPARAM>(&hti)));
        if (hit >= 0) {
            const int selected = static_cast<int>(::SendMessageW(
                hwndList_, LVM_GETNEXTITEM, static_cast<WPARAM>(hit),
                MAKELPARAM(LVNI_SELECTED, 0)));
            if (selected != hit) {
                LVITEMW st = {};
                st.mask = LVIF_STATE;
                st.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                ::SendMessageW(hwndList_, LVM_SETITEMSTATE,
                               static_cast<WPARAM>(-1),
                               reinterpret_cast<LPARAM>(&st));
                LVITEMW sel = {};
                sel.mask = LVIF_STATE;
                sel.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                sel.state = LVIS_SELECTED | LVIS_FOCUSED;
                ::SendMessageW(hwndList_, LVM_SETITEMSTATE,
                               static_cast<WPARAM>(hit),
                               reinterpret_cast<LPARAM>(&sel));
            }
        }
    } else {
        // Keyboard invocation (Shift+F10 / menu key): anchor at focus.
        const int focus = static_cast<int>(::SendMessageW(
            hwndList_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
            MAKELPARAM(LVNI_FOCUSED, 0)));
        RECT rc = {};
        if (focus >= 0 &&
            ::SendMessageW(hwndList_, LVM_GETITEMRECT,
                           static_cast<WPARAM>(focus),
                           reinterpret_cast<LPARAM>(&rc))) {
            pt.x = rc.left + 16;
            pt.y = rc.top + 8;
        } else {
            pt.x = 0;
            pt.y = 0;
        }
        ::ClientToScreen(hwndList_, &pt);
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) return;
    const int selected = static_cast<int>(::SendMessageW(
        hwndList_, LVM_GETSELECTEDCOUNT, 0, 0));
    const UINT gray = (selected > 0) ? MF_ENABLED : MF_GRAYED;
    // The one remaining user of the POINTER form (C4), and deliberately so:
    // every read of `row` below happens BEFORE TrackPopupMenu runs its modal
    // loop, and only to compute a grey/enabled flag. Nothing here dereferences
    // the row after the menu returns, so there is no window in which a refresh
    // could free it. A copy would also be harmless but would copy a row whose
    // only use is four boolean reads.
    const Connection* row = FirstSelectedRow();
    // 9.2.6: "Close connection" only works for IPv4 - SetTcpEntry has no IPv6
    // variant (TcpTable.cpp:262 refuses it with a clear error). Disable the item
    // for IPv6 rows rather than letting the user invoke it and read the error,
    // and call the reason in the label the same way "End process..." does, so a
    // greyed item is not a silent mystery (see the D29 note above). The Block
    // item stays enabled: firewall rules are family-agnostic and stop the
    // connection from coming back, which is the right outcome for IPv6.
    const bool isV4Tcp = (row != nullptr && row->protocol == IPPROTO_TCP &&
                          row->family == AF_INET &&
                          row->state != MIB_TCP_STATE_LISTEN &&
                          row->state != 0);
    const bool isV6Tcp = (row != nullptr && row->protocol == IPPROTO_TCP &&
                          row->family != AF_INET &&
                          row->state != MIB_TCP_STATE_LISTEN &&
                          row->state != 0);
    const bool closable = isV4Tcp;
    ::AppendMenuW(menu, MF_STRING | gray, IDM_CTX_DETAILS, L"&Details...");
    ::AppendMenuW(menu, MF_STRING | gray, IDM_CTX_COPY_SELECTED, L"&Copy selected");
    ::AppendMenuW(menu, MF_STRING, IDM_CTX_COPY_ALL, L"Copy &all");
    ::AppendMenuW(menu, MF_STRING | gray, IDM_CTX_EXPORT_SELECTION,
                   L"&Export selection...");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // REMOVED 2026-10-05 (todo.md 8.7 G2). Both menu entries that used to live here
    // are gone; the capability was moved to the CLI rather than dropped.
    //
    // REMOVED: "Follow TCP &stream..." (IDM_FOLLOW_STREAM). It re-elevated the
    // whole application to get at pktmon - losing the window, the filter and the
    // column layout to read hex bytes - and then blocked modally on a sampling
    // capture that returns nothing unless the connection is still carrying data
    // after the filter is armed. See `FollowSelectedStream` below for the full
    // chain, and `capture --text` / `capture --out` in cli.md for what replaced
    // it.
    //
    // const bool canFollow = closable && CaptureAvailable(nullptr);
    // ::AppendMenuW(menu, MF_STRING | (canFollow ? MF_ENABLED : MF_GRAYED),
    //               IDM_FOLLOW_STREAM, L"Follow TCP &stream...");

    // Shell navigation (4.6). Both need a real path, which a row does not
    // always have: PID 4 and some service processes report a name but no
    // image path. Offering the item and then failing is worse than greying
    // it, so the predicate is the same condition the action depends on.
    const bool hasPath = (row != nullptr && !row->processPath.empty() &&
                          row->processPath != L"System");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (hasPath ? MF_ENABLED : MF_GRAYED),
                  IDM_OPEN_FILE_LOCATION, L"Open file &location");
    // REMOVED 2026-10-05: "Process &properties..." (IDM_PROCESS_PROPERTIES).
    // The user reported it does not work, and the cause is structural rather
    // than a bug to repair: `properties` is a shell VERB, registered against
    // shell object types, and calling it directly with a bare path is the case
    // where it fails - silently, since ShellExecuteW returns <= 32 instead of
    // raising. The old handler collapsed that, a missing path, and PID 4's
    // placeholder "System" path into one identical message box.
    //
    // Removed rather than fixed on purpose. A menu entry offering an action the
    // app cannot perform teaches the reader to distrust the rest of the menu,
    // which costs more than the feature is worth. "Open file location" above
    // still reaches the same file through Explorer, where the verb does exist,
    // so nothing that ever worked is lost.
    //
    // ::AppendMenuW(menu, MF_STRING | (hasPath ? MF_ENABLED : MF_GRAYED),
    //               IDM_PROCESS_PROPERTIES, L"Process &properties...");
    ::AppendMenuW(menu, MF_STRING | (row != nullptr && row->protocol == IPPROTO_TCP
                                         ? MF_ENABLED : MF_GRAYED),
                   IDM_BLOCK_CONNECTION, L"&Block this connection...");
    // 9.2.5: "Block this connection..." adds Windows Firewall rules, which need
    // an elevated token. Show the UAC shield when the user is an admin who is
    // not currently elevated (ElevationPossibleForUser). A standard account gets
    // no shield: the glyph means "can be elevated", and a click on that item
    // instead shows ElevationUnavailableReason() from the OnCommand handler.
    if (!HighContrastActive() && ElevationPossibleForUser())
        SetMenuItemShield(menu, IDM_BLOCK_CONNECTION, true);
    // Note on "Close connection": SetTcpEntry on a connection the current user
    // owns does NOT require elevation, so it deliberately takes no shield -
    // showing one would mislead users into thinking the action is privileged.

    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (isV6Tcp ? MF_GRAYED : (closable ? MF_ENABLED : MF_GRAYED)),
                   IDM_CTX_CLOSE_CONNECTION,
                   isV6Tcp ? L"&Close connection (IPv6 - use Block instead...)"
                           : L"&Close connection...");
    // D29: "End process..." was unconditionally enabled, so it was clickable on
    // a wintcp.exe row - and wintcp.exe is a network tool, so it has its own
    // rows. Clicking it ended WinTCP. The item is now greyed when the row's PID
    // cannot be ended, and says WHY in the label rather than greying silently,
    // because a greyed item with no explanation is the thing users report as a
    // bug. The rule comes from Commands.cpp so the CLI applies the same one.
    const PidVerdict killVerdict =
        PidKillVerdict(row != nullptr ? row->pid : 0, ::GetCurrentProcessId(),
                       row != nullptr);
    const bool canEnd = (killVerdict == PidVerdict::Ok);
    ::AppendMenuW(menu, MF_STRING | (canEnd ? MF_ENABLED : MF_GRAYED),
                  IDM_CTX_KILL_PROCESS,
                  canEnd ? L"&End process..."
                         : L"&End process... (not available for this row)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_CTX_REFRESH, L"&Refresh");
    ::TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_,
                     nullptr);
    ::DestroyMenu(menu);
}

// ---------------------------------------------------------------------------
// selection helpers ----------------------------------------------------------

const Connection* MainWindow::FirstSelectedRow() const {
    if (hwndList_ == nullptr) return nullptr;
    int item = static_cast<int>(::SendMessageW(
        hwndList_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
        MAKELPARAM(LVNI_FOCUSED | LVNI_SELECTED, 0)));
    if (item == -1)
        item = static_cast<int>(::SendMessageW(
            hwndList_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
            MAKELPARAM(LVNI_SELECTED, 0)));
    if (item == -1) return nullptr;
    return store_.ViewRow(static_cast<size_t>(item));
}

std::optional<Connection> MainWindow::SelectedCopy() const {
    const Connection* row = FirstSelectedRow();
    if (row == nullptr) return std::nullopt;
    return *row;
}

bool MainWindow::SelectedRowIndices(std::vector<int>& out) const {
    out.clear();
    if (hwndList_ == nullptr) return false;
    int item = -1;
    while ((item = static_cast<int>(::SendMessageW(
                hwndList_, LVM_GETNEXTITEM, static_cast<WPARAM>(item),
                MAKELPARAM(LVNI_SELECTED, 0)))) != -1) {
        out.push_back(item);
    }
    return !out.empty();
}

// ---------------------------------------------------------------------------
// actions --------------------------------------------------------------------

std::wstring MainWindow::SelectedRowsText(bool copyAll) const {
    std::wstring out;
    // Header = visible columns, tab separated.
    for (size_t i = 0; i < visToCol_.size(); ++i) {
        if (i != 0) out += L"\t";
        out += kColumns[visToCol_[i]].title;
    }
    out += L"\r\n";

    wchar_t buf[kMaxColumnText] = {0};
    const auto appendRow = [&](const Connection& c) {
        for (size_t i = 0; i < visToCol_.size(); ++i) {
            ConnectionStore::GetColumnText(c, visToCol_[i], buf,
                                           kMaxColumnText);
            if (i != 0) out += L"\t";
            out += buf;
        }
        out += L"\r\n";
    };

    if (copyAll) {
        for (size_t v = 0; v < store_.View().size(); ++v) {
            const Connection* c = store_.ViewRow(v);
            if (c != nullptr) appendRow(*c);
        }
        return out;
    }
    std::vector<int> sel;
    if (!SelectedRowIndices(sel)) return out;
    for (int item : sel) {
        const Connection* c = store_.ViewRow(static_cast<size_t>(item));
        if (c != nullptr) appendRow(*c);
    }
    return out;
}

void MainWindow::ExportCsv() {
    DoExport(false);
}

void MainWindow::DoExport(bool selectionOnly) {
    if (store_.View().empty()) {
        ::MessageBoxW(hwnd_, L"There are no rows to export.", L"WinTCP",
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (selectionOnly) {
        const int n = static_cast<int>(
            ::SendMessageW(hwndList_, LVM_GETSELECTEDCOUNT, 0, 0));
        if (n <= 0) {
            ::MessageBoxW(hwnd_, L"No rows are selected.", L"WinTCP",
                          MB_OK | MB_ICONINFORMATION);
            return;
        }
    }
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    ofn.lpstrFilter = L"CSV files (*.csv)\0*.csv\0"
                      L"TSV files (*.tsv)\0*.tsv\0"
                      L"JSON files (*.json)\0*.json\0"
                      L"All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (settings_.lastExportDir[0] != 0)
        ofn.lpstrInitialDir = settings_.lastExportDir;    // remember
    // comdlg32.dll is delay-loaded; DelayLoadGuard reports its absence, where a
    // bare `return` here would be indistinguishable from the user cancelling.
    if (!DelayLoadGuard(hwnd_, "comdlg32.dll")) return;
    if (!::GetSaveFileNameW(&ofn)) return;

    // Format: a typed extension wins, otherwise the dropdown decides and
    // the matching extension is appended when the name has none.
    std::wstring p(path);
    const size_t slash = p.find_last_of(L"\\/");
    const size_t dot = p.find_last_of(L'.');
    const bool hasExt = (dot != std::wstring::npos) &&
                        (slash == std::wstring::npos || dot > slash);
    int format = (ofn.nFilterIndex >= 1 && ofn.nFilterIndex <= 3)
                     ? static_cast<int>(ofn.nFilterIndex)
                     : 1;                                  // 1=csv 2=tsv 3=json
    if (hasExt) {
        const std::wstring ext = ToLowerW(p.substr(dot));
        if (ext == L".csv") format = 1;
        else if (ext == L".tsv" || ext == L".tab") format = 2;
        else if (ext == L".json") format = 3;
    } else {
        p += (format == 2) ? L".tsv" : (format == 3 ? L".json" : L".csv");
    }

    // Rows are collected first (copies, not pointers): the serializers in
    // Commands take values, so the GUI and the CLI export byte-identical
    // content from the same code.
    std::vector<Connection> rows;
    if (selectionOnly) {
        std::vector<int> sel;
        if (SelectedRowIndices(sel)) {
            for (int item : sel) {
                const Connection* c =
                    store_.ViewRow(static_cast<size_t>(item));
                if (c != nullptr) rows.push_back(*c);
            }
        }
    } else {
        for (size_t v = 0; v < store_.View().size(); ++v) {
            const Connection* c = store_.ViewRow(v);
            if (c != nullptr) rows.push_back(*c);
        }
    }
    const std::string content = (format == 3)
        ? RenderJsonRows(rows, DefaultExportColumns(),
                         /*extraHostname=*/true)
        : RenderDelimitedRows(rows, visToCol_, (format == 2) ? '\t' : ',',
                              /*rfcCsv=*/format != 2);
    // JSON must not carry a BOM (strict parsers reject it); CSV/TSV keep
    // the Excel-friendly BOM.
    const std::wstring err = WriteUtf8FileWithBom(p, content, format != 3);
    if (!err.empty()) {
        const std::wstring msg = L"Failed to write file:\n" + err;
        ::MessageBoxW(hwnd_, msg.c_str(), L"WinTCP - Export failed",
                      MB_OK | MB_ICONERROR);
        return;
    }
    // Remember the directory for the next export.
    const size_t lastSep = p.find_last_of(L"\\/");
    if (lastSep != std::wstring::npos && lastSep < MAX_PATH)
        ::wcsncpy_s(settings_.lastExportDir, p.substr(0, lastSep).c_str(),
                    _TRUNCATE);
    // 9.4.7: surface the row count the export actually wrote, so a user
    // exporting a 3000-row snapshot gets a confirmation that names the volume
    // rather than a bare "Export completed." The count is authoritative: it
    // is the number of rows fed to the serializer, which is exactly what
    // landed in the file.
    std::wstring msg = L"Exported " + std::to_wstring(rows.size()) +
                       L" rows to " + p + L".";
    ::MessageBoxW(hwnd_, msg.c_str(), L"WinTCP",
                  MB_OK | MB_ICONINFORMATION);
}

void MainWindow::CopySelectionToClipboard(bool copyAll) {
    if (!copyAll) {
        const int n = static_cast<int>(::SendMessageW(
            hwndList_, LVM_GETSELECTEDCOUNT, 0, 0));
        if (n <= 0) return;
    }
    const std::wstring text = SelectedRowsText(copyAll);
    if (text.empty()) return;
    if (!::OpenClipboard(hwnd_)) {
        ::MessageBoxW(hwnd_, L"Could not open the clipboard.", L"WinTCP",
                      MB_OK | MB_ICONWARNING);
        return;
    }
    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem != nullptr) {
        void* dst = ::GlobalLock(hMem);
        if (dst != nullptr) {
            ::memcpy(dst, text.c_str(), bytes);
            ::GlobalUnlock(hMem);
            if (::SetClipboardData(CF_UNICODETEXT, hMem) == nullptr)
                ::GlobalFree(hMem);       // ownership only transfers on success
            hMem = nullptr;
        }
        if (hMem != nullptr) ::GlobalFree(hMem);
    }
    ::CloseClipboard();
}

void MainWindow::KillSelectedProcess() {
    const std::optional<Connection> sel = SelectedCopy();
    if (!sel.has_value()) return;
    const Connection& c = *sel;

    // D29. THE ROW IS ALREADY A COPY — `sel` above, not `rows_[n]`.
    //
    // This handler used to read `c->pid` and friends *after* the confirm
    // MessageBox returned, and the comment above those reads explained why
    // that was wrong without doing anything about it: a modal dialog runs its
    // own message loop, so the refresh timer fires underneath it,
    // `ReplaceSnapshot` reallocates `rows_`, and `c` - a pointer INTO that
    // vector - dangles. Reading `pid` out of freed memory and handing it to
    // `KillPid` ends a process nobody selected. The report that started this
    // was wintcp.exe closing itself after "End process" on AnyDesk.exe, which
    // is exactly what a garbage PID that happened to be our own produces.
    //
    // The copy this handler relied on is no longer a local `const Connection
    // row = c;` that a future edit could move below the dialog: it is
    // `SelectedCopy()` at the top of every handler, so there is no way to write
    // this handler wrong (C4). It costs one Connection on a path the user just
    // opened a dialog for, and the whole row is copied rather than the few
    // fields this dialog happens to use, because a partial copy is a copy that
    // can be extended wrongly later.

    // D29, second half: refuse our own process BEFORE the dialog, so the user
    // is not asked to approve something that will be refused afterwards. The
    // rule itself lives in Commands.cpp (PidKillVerdict) because the CLI can
    // reach it too; this is only the GUI asking early.
    const PidVerdict verdict =
        PidKillVerdict(c.pid, ::GetCurrentProcessId(), true);
    if (verdict != PidVerdict::Ok) {
        ::MessageBoxW(hwnd_, PidKillRefusal(verdict), L"WinTCP - End process",
                      MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Refuse pseudo-pids and verify the PID still maps to the same process
    // instance we captured (PID-reuse protection).
    std::wstring verifyErr;
    if (!ProcessResolver::VerifyProcess(c.pid, c.processCreate,
                                        c.processCreateKnown, verifyErr)) {
        ::MessageBoxW(hwnd_, verifyErr.c_str(), L"WinTCP",
                      MB_OK | MB_ICONWARNING);
        return;
    }

    // Confirm with enough identity detail that the user is agreeing to end
    // the process they think they are ending. Name plus PID alone is not
    // enough: PIDs get recycled, and "chrome.exe (PID 1234)" is ambiguous
    // when two instances are running. Full path and start time disambiguate
    // completely, because the start time is what the verification above just
    // confirmed.
    const std::wstring path =
        c.processPath.empty() ? std::wstring(L"(path unavailable)")
                               : c.processPath;
    const std::wstring started = FormatFileTimeLocal(c.processCreate,
                                                      c.processCreateKnown);
    const std::wstring cmdLine = QueryProcessCommandLine(c.pid);
    wchar_t prompt[1536] = {0};
    if (cmdLine.empty() || cmdLine.size() > 300) {
        ::swprintf_s(prompt,
                     L"End this process?\n\n  Name:  %ls\n  PID:   %lu\n"
                     L"  Path:  %ls\n  Start: %ls\n\n"
                     L"WinTCP will ask the process to close normally first, "
                     L"and only end it forcibly if it does not exit within a "
                     L"few seconds.\n\nUnsaved data may still be lost.",
                     c.processName.c_str(),
                     static_cast<unsigned long>(c.pid), path.c_str(),
                     started.c_str());
    } else {
        ::swprintf_s(prompt,
                     L"End this process?\n\n  Name:  %ls\n  PID:   %lu\n"
                     L"  Path:  %ls\n  Start: %ls\n  Cmd:   %ls\n\n"
                     L"WinTCP will ask the process to close normally first, "
                     L"and only end it forcibly if it does not exit within a "
                     L"few seconds.\n\nUnsaved data may still be lost.",
                     c.processName.c_str(),
                     static_cast<unsigned long>(c.pid), path.c_str(),
                     started.c_str(), cmdLine.c_str());
    }
    if (::MessageBoxW(hwnd_, prompt, L"WinTCP - End process",
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    // From here nothing reads a Connection. The row is gone as far as this
    // function is concerned; only the by-value copy above is live, and
    // KillPid re-verifies the PID against its start time in case it was
    // recycled while the dialog was up.
    //
    // Execution lives in the abstract layer (Commands::KillPid): graceful
    // close first, then terminate.
    MutateOptions mo;
    mo.yes = true;
    const CommandResult kr =
        KillPid(c.pid, c.processCreate, c.processCreateKnown,
                c.processName, mo);
    if (kr.exitCode != kExitOk) {
        ::MessageBoxW(hwnd_, Utf8ToWide(kr.err.c_str()).c_str(), L"WinTCP",
                      MB_OK | MB_ICONERROR);
        return;
    }
    Refresh(false);
}

void MainWindow::OpenSelectedFileLocation() {
    const std::optional<Connection> sel = SelectedCopy();
    if (!sel.has_value()) return;
    const Connection& c = *sel;
    if (c.processPath.empty() || c.processPath == L"System") {
        ::MessageBoxW(hwnd_,
                      L"This row has no process path, so there is no file to "
                      L"open.\n\nSystem-owned processes and rows whose owning "
                      L"process has already exited do not have one.",
                      L"Open file location", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // "explore" selects the file in its folder, which is the useful meaning
    // here; "open" would launch the executable.
    const std::wstring verb = L"explore";
    // shell32.dll is delay-loaded; without this gate a machine without it
    // raises here instead of reporting the failure the message box below
    // already knows how to phrase.
    if (!DllAvailable("shell32.dll")) {
        ::MessageBoxW(hwnd_, L"Explorer could not open that location.",
                      L"Open file location", MB_OK | MB_ICONWARNING);
        return;
    }
    const HINSTANCE rc = ::ShellExecuteW(hwnd_, verb.c_str(), c.processPath.c_str(),
                                          nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecuteW returns a value > 32 on success; anything else is an
    // error code, and <= 32 is the documented failure range.
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        ::MessageBoxW(hwnd_, L"Explorer could not open that location.",
                      L"Open file location", MB_OK | MB_ICONWARNING);
    }
}

// REMOVED 2026-10-05 (todo.md 8.7 G1). Kept, commented, for the same reason as
// the menu entry: the reasoning is the valuable part and deleting it would lose
// the answer to "why doesn't Process properties work?".
//
// void MainWindow::ShowSelectedProcessProperties() {
//     const std::optional<Connection> sel = SelectedCopy();
//     if (!sel.has_value()) return;
//     const Connection& c = *sel;
//     if (c.processPath.empty() || c.processPath == L"System") {
//         ::MessageBoxW(hwnd_,
//                       L"This row has no process path, so there are no file "
//                       L"properties to show.",
//                       L"Process properties", MB_OK | MB_ICONINFORMATION);
//         return;
//     }
//     const std::wstring verb = L"properties";
//     // shell32.dll is delay-loaded; gate before the call so its absence is
//     // reported rather than raised. Same reason as ShowSelectedFileLocation.
//     if (!DllAvailable("shell32.dll")) {
//         ::MessageBoxW(hwnd_, L"Windows could not show properties for that file.",
//                       L"Process properties", MB_OK | MB_ICONWARNING);
//         return;
//     }
//     const HINSTANCE rc = ::ShellExecuteW(hwnd_, verb.c_str(), c.processPath.c_str(),
//                                           nullptr, nullptr, SW_SHOWNORMAL);
//     if (reinterpret_cast<INT_PTR>(rc) <= 32) {
//         ::MessageBoxW(hwnd_, L"Windows could not show properties for that file.",
//                       L"Process properties", MB_OK | MB_ICONWARNING);
//     }
// }
//
// What was wrong with it, having read it rather than guessed: `properties` is a
// shell verb, and the way to invoke one is through a shell item
// (ShellExecuteEx on an IShellItem, or "explorer /select,"), not by handing
// ShellExecuteW a raw path. Given a path it returns <= 32, which the three boxes
// above collapsed into a single indistinguishable message - so a user could not
// tell a genuinely missing file from a verb the shell declined to run, which is
// exactly the "it does not work" report. There was also no working version of
// the idea to fall back on: "Open file location" opens Explorer on the same
// file, and Explorer does have the verb.

void MainWindow::BlockSelectedConnection() {
    const std::optional<Connection> sel = SelectedCopy();
    if (!sel.has_value()) return;
    const Connection& c = *sel;

    // Row -> request mapping lives in the abstract layer
    // (Commands::ConnectionToBlockRequest), so the GUI and the `block` CLI
    // verb refuse the same rows for the same reason.
    BlockRequest req;
    std::wstring whyNot;
    if (!ConnectionToBlockRequest(c, &req, &whyNot)) {
        ::MessageBoxW(hwnd_,
                      (whyNot +
                       L"\n\nA listening socket has no peer yet, and a UDP row "
                       L"has no connection to stop.")
                          .c_str(),
                      L"Block connection", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Say plainly what will and will not happen, because the two layers have
    // different reach: the firewall rule always works, but Windows exposes no
    // public way to tear down a single live IPv6 connection, so on IPv6 only
    // future connections are stopped.
    wchar_t prompt[1024] = {0};
    if (req.ipv6) {
        ::swprintf_s(prompt,
                     L"Block %ls?\n\nWindows Firewall rules will be added so this "
                     L"peer cannot be reached again.\n\nThe currently open "
                     L"connection will NOT be dropped: Windows provides no "
                     L"supported way to close a single live IPv6 connection. "
                     L"It will end on its own when the application closes it.",
                     req.label.c_str());
    } else {
        ::swprintf_s(prompt,
                     L"Block %ls?\n\nThe connection will be closed now, and "
                     L"Windows Firewall rules will be added so this peer cannot "
                     L"be reached again.\n\nTo undo, use Remove All WinTCP "
                     L"Blocks from the tray menu.",
                     req.label.c_str());
    }
    if (::MessageBoxW(hwnd_, prompt, L"WinTCP - Block connection",
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    // Execution lives in the abstract layer (Commands::BlockNow); this only
    // confirms and renders the result.
    MutateOptions mo;
    mo.yes = true;
    const CommandResult br = BlockNow(req, mo);
    if (br.exitCode == kExitOk) {
        ::MessageBoxW(hwnd_, Utf8ToWide(br.out.c_str()).c_str(),
                      L"Block connection", MB_OK | MB_ICONINFORMATION);
    } else {
        // Partial success is reported as partial. Saying "done" when only
        // one of the two layers applied would leave the user believing
        // the connection is gone when it is still open.
        ::MessageBoxW(hwnd_, Utf8ToWide((br.out + br.err).c_str()).c_str(),
                      L"Block connection - partial",
                      MB_OK | MB_ICONWARNING);
    }
    Refresh(false);
}

void MainWindow::CloseSelectedConnection() {
    const std::optional<Connection> sel = SelectedCopy();
    if (!sel.has_value()) return;
    const Connection& c = *sel;
    if (c.protocol != IPPROTO_TCP || c.state == MIB_TCP_STATE_LISTEN)
        return;

    // `c` is a copy (SelectedCopy, C4): the confirm below runs a modal loop, and
    // a refresh arriving in that window reallocates rows_.

    wchar_t prompt[768] = {0};
    ::swprintf_s(prompt,
                 L"Close this TCP connection?\n\n  %ls  ->  %ls\n  [%ls]  PID %lu",
                 c.localEndpoint.c_str(), c.remoteEndpoint.c_str(),
                 c.stateLabel.c_str(), static_cast<unsigned long>(c.pid));
    if (::MessageBoxW(hwnd_, prompt, L"WinTCP - Close connection",
                      MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    // Execution lives in the abstract layer (Commands::CloseRow); this only
    // confirms and renders the result.
    MutateOptions mo;
    mo.yes = true;
    const CommandResult cr = CloseRow(c, mo);
    if (cr.exitCode != kExitOk) {
        ::MessageBoxW(hwnd_, Utf8ToWide(cr.err.c_str()).c_str(),
                      L"WinTCP - Close failed", MB_OK | MB_ICONWARNING);
        return;
    }
    Refresh(false);   // the row vanishes, flashes red, then lingers as a grey F5.7 ghost
}

void MainWindow::RemoveAllWinTcpBlocks() {
    // D30: remove every WinTCP block rule the ledger remembers - i.e., every
    // rule that `block` created. The ledger, not a full enumeration, is the
    // source of truth, so this removes exactly what this application added
    // and nothing else.
    const int count = CountWinTcpRules();

    if (count == 0) {
        // count == 0 also means "firewall API not accessible" (not
        // elevated); a removal attempt surfaces that before we say
        // "nothing to remove".
        std::wstring error;
        (void)RemoveAllWinTcpRules(&error);
        if (!error.empty()) {
            ::MessageBoxW(hwnd_, error.c_str(),
                          L"Remove All WinTCP Blocks", MB_OK | MB_ICONERROR);
            return;
        }
        ::MessageBoxW(hwnd_,
                      L"No WinTCP firewall rules are currently present. "
                      L"Nothing to remove.",
                      L"WinTCP - Remove all blocks",
                      MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Confirm before tearing down every rule at once - one wrong click
    // must not leave a whole range of peers suddenly open.
    wchar_t prompt[640] = {0};
    ::swprintf_s(prompt,
                 L"Remove ALL WinTCP firewall blocks?\n\n"
                 L"This removes %d rule(s) and unblocks every peer that was "
                 L"previously blocked with `block`. Rules can always be "
                 L"recreated by blocking the peer again.",
                 count);
    if (::MessageBoxW(hwnd_, prompt, L"WinTCP - Remove all blocks",
                      MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }

    std::wstring error;
    const bool removed = RemoveAllWinTcpRules(&error);
    if (!removed) {
        ::MessageBoxW(hwnd_, error.c_str(),
                      L"Remove All WinTCP Blocks", MB_OK | MB_ICONERROR);
        return;
    }

    ::MessageBoxW(hwnd_,
                  (L"Removed " + std::to_wstring(count) +
                   L" WinTCP firewall rule(s).").c_str(),
                  L"WinTCP - Remove all blocks",
                  MB_OK | MB_ICONINFORMATION);
}

LRESULT MainWindow::OnCustomDraw(NMLVCUSTOMDRAW* cd) {
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            // CDRF_NOTIFYITEMDRAW == 0x20 == "CDRF_NOTIFYITEMPREPAINT"
            // (the SDK defines only the generic name; same value).
            // Deliberately no CDRF_NOTIFYPOSTPAINT: post-paint is only
            // needed for sub-item draw stages this list does not use.
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            // Change highlighting: green = new, yellow = state changed, red =
            // vanished THIS refresh (then a grey F5.7 retained ghost). The colors
            // are chosen from the active theme: on a dark theme the pastel
            // fills would glare, so a darker background and a light
            // foreground are used instead.
            const Connection* c = store_.ViewRow(
                static_cast<size_t>(cd->nmcd.dwItemSpec));
            if (c == nullptr) return CDRF_DODEFAULT;
            // High Contrast schemes take precedence over every colour we would
            // paint. In HC, the system's COLOR_WINDOW/COLOR_WINDOWTEXT pairing
            // is the only legible answer: our palette would override it with
            // pale fills on the HC-black background, while the default text
            // colour would stay white. Returning CDRF_DODEFAULT lets the
            // ListView paint with the user's HC scheme. The row-flash hint is
            // a compromise we should keep off when that scheme is in effect.
            if (HighContrastActive()) return CDRF_DODEFAULT;
            const bool dark = ThemeIsDark();
            if (c->flags & kRowRemoved) {
                // F5.7: a socket that vanished THIS refresh flashes red once,
                // then settles to a muted grey retained ghost for the rest of its
                // life (up to kMaxRetainedGhosts). deathTick is touched only at
                // vanishing, so it equals this snapshot's tick for the flashing row.
                const bool justDied = (c->deathTick == store_.SnapshotTick());
                if (justDied) {
                    if (dark) {
                        cd->clrTextBk = RGB(0x4A, 0x1F, 0x1F);
                        cd->clrText = RGB(0xFF, 0xB4, 0xB4);
                    } else {
                        cd->clrTextBk = RGB(0xF8, 0xC8, 0xC8);
                        cd->clrText = RGB(0x7A, 0x1F, 0x1F);
                    }
                } else {
                    if (dark) {
                        cd->clrTextBk = RGB(0x2B, 0x2B, 0x2B);
                        cd->clrText = RGB(0xC0, 0xC0, 0xC0);
                    } else {
                        cd->clrTextBk = RGB(0xEA, 0xEA, 0xEA);
                        cd->clrText = RGB(0x5A, 0x5A, 0x5A);
                    }
                }
                return CDRF_NEWFONT;
            }
            if (c->flags & kRowNew) {
                if (dark) {
                    cd->clrTextBk = RGB(0x1C, 0x4A, 0x1C);
                    cd->clrText = RGB(0xB4, 0xFF, 0xB4);
                } else {
                    cd->clrTextBk = RGB(0xD6, 0xF5, 0xD6);
                }
                return CDRF_NEWFONT;
            }
            if (c->flags & kRowChanged) {
                if (dark) {
                    cd->clrTextBk = RGB(0x4A, 0x42, 0x1A);
                    cd->clrText = RGB(0xFF, 0xFF, 0xB4);
                } else {
                    cd->clrTextBk = RGB(0xF8, 0xF0, 0xB8);
                }
                return CDRF_NEWFONT;
            }
            return CDRF_DODEFAULT;
        }
        default:
            return CDRF_DODEFAULT;
    }
}

DetailModel MainWindow::BuildDetails(const Connection& c) const {
    // Delegates to the abstract layer so the GUI and the `details` CLI verb
    // render the same model. The DetailsDialog renderer is unchanged.
    return BuildDetailModel(c, store_);
}

void MainWindow::RefreshDetailsWindow() {
    // Silent live-update after every refresh: rebuild the text for the
    // tracked PID without showing, focusing or foregrounding anything.
    // A hidden or never-opened window is left alone.
    if (!details_.IsVisible() || detailsPid_ == 0) return;
    const Connection* found = nullptr;
    for (const Connection& r : store_.Rows()) {
        if (r.pid == detailsPid_ && !(r.flags & kRowRemoved)) {
            found = &r;
            break;
        }
    }
    if (found == nullptr) {
        // Process exited (or every connection closed): keep the last
        // sample but say so once instead of silently going stale
        //
        if (!detailsExitedNoted_ && !detailsModel_.sections.empty()) {
            detailsExitedNoted_ = true;
            // Appended to the model's own note, not to a string: the
            // renderer shows it in place without re-parsing anything.
            DetailModel stale = detailsModel_;
            if (!stale.sections.empty())
                stale.sections.back().note =
                    L"process no longer running — showing the last sample";
            details_.UpdateModel(std::move(stale));
        }
        return;
    }
    detailsExitedNoted_ = false;
    detailsModel_ = BuildDetails(*found);
    details_.UpdateModel(detailsModel_);
}

void MainWindow::ShowDetailsOfSelectedRow() {
    const std::optional<Connection> sel = SelectedCopy();
    if (!sel.has_value()) return;
    const Connection& c = *sel;

    detailsPid_ = c.pid;
    detailsExitedNoted_ = false;
    detailsModel_ = BuildDetails(c);
    details_.SetDark(ThemeIsDark());
    details_.Show(hwnd_, font_, detailsModel_,
                  c.processPath.empty() ? std::wstring() : c.processPath);
}

/* REMOVED 2026-10-05 (todo.md 8.7 G2) - the modal "capturing..." countdown
   window, and the whole of FollowSelectedStream's wait step.

   This existed for one caller: the GUI's Follow TCP stream action, which is
   gone. It could not simply be left in place, because an unreferenced function
   with internal linkage is C4505 - a warning, and /WX makes every warning an
   error. So it is commented rather than deleted: the timer handling and the
   countdown arithmetic are worth keeping a record of, and a future caller that
   wants a non-modal capture window should start from this instead of from
   nothing.

   Reinstating it means unwrapping this block, re-enabling IDM_FOLLOW_STREAM in
   wintcp.rc and OnContextMenu, and answering the question this window never
   did: how the user knows when to generate traffic. Better answered on the
   command line, which is where the feature went.
namespace {

// A small modal "capturing" window with a live countdown. Written here
// rather than reusing a .rc dialog so the whole feature stays in one place
// and needs no new resource ids.
//
// It is a plain window, not a dialog: no template, one STATIC, one BUTTON,
// one 1 Hz timer. The main window stays enabled underneath so the user can
// watch the connection they are capturing and generate traffic on it.
constexpr wchar_t kCaptureWaitClass[] = L"WinTcpCaptureWait";
constexpr UINT_PTR kCaptureWaitTimer = 1;
constexpr UINT IDM_WAIT_STOP = 2001;
constexpr UINT IDC_WAIT_TEXT = 2002;

// Seconds offered. Long enough to catch a page load, short enough that an
// impatient user is not trapped.
constexpr int kCaptureSeconds = 15;

// The wait dialog's countdown tick. 250 ms refreshes the displayed countdown
// promptly without repainting constantly; both creation sites share it so the
// rate is stated once.
constexpr UINT kCapturePollMs = 250;

LRESULT CALLBACK CaptureWaitProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
bool g_captureWaitRegistered = false;
int g_captureWaitDeadline = 0;   // GetTickCount at which to auto-close

INT_PTR ShowCaptureWait(HWND owner, const std::wstring& what) {
    if (!g_captureWaitRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &CaptureWaitProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kCaptureWaitClass;
        if (::RegisterClassExW(&wc) == 0 &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return 0;
        g_captureWaitRegistered = true;
    }

    HWND dlg = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME, kCaptureWaitClass, L"Capturing TCP stream",
        WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT,
        CW_USEDEFAULT, 460, 190, owner, nullptr, ::GetModuleHandleW(nullptr),
        nullptr);
    if (dlg == nullptr) return 0;

    ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                      16, 16, 410, 90, dlg,
                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(
                          IDC_WAIT_TEXT)),
                      ::GetModuleHandleW(nullptr), nullptr);
    ::CreateWindowExW(0, L"BUTTON", L"&Stop and view",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      330, 118, 110, 28, dlg,
                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(
                          IDM_WAIT_STOP)),
                      ::GetModuleHandleW(nullptr), nullptr);

    ::SetWindowTextW(
        dlg, (L"Capture: " + what).c_str());

    g_captureWaitDeadline =
        static_cast<int>(::GetTickCount()) + kCaptureSeconds * 1000;
    ::SetTimer(dlg, kCaptureWaitTimer, kCapturePollMs, nullptr);

    // Modal loop: own message pump so the main list keeps refreshing and the
    // user can interact with it while the capture runs.
    ::EnableWindow(owner, FALSE);
    MSG msg;
    for (;;) {
        if (::IsWindow(dlg) == FALSE) break;
        if (::GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            ::DestroyWindow(dlg);
            break;
        }
        if (::IsDialogMessageW(dlg, &msg) == 0) ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    ::EnableWindow(owner, TRUE);
    ::SetForegroundWindow(owner);
    return 1;
}

LRESULT CALLBACK CaptureWaitProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_CREATE: {
            g_captureWaitDeadline =
                static_cast<int>(::GetTickCount()) + kCaptureSeconds * 1000;
            ::SetTimer(hwnd, kCaptureWaitTimer, kCapturePollMs, nullptr);
            return 0;
        }
        case WM_TIMER: {
            const int left = (g_captureWaitDeadline -
                              static_cast<int>(::GetTickCount()) + kMsPerSecond - 1) / kMsPerSecond;
            HWND text = ::GetDlgItem(hwnd, IDC_WAIT_TEXT);
            if (left <= 0) {
                ::DestroyWindow(hwnd);
                return 0;
            }
            wchar_t buf[512] = {0};
            ::swprintf_s(buf, L"Capturing this connection for %d more "
                              L"second(s).\n\nBrowse or sync something that uses "
                              L"it now.\n\nFor an encrypted (TLS) connection the "
                              L"handshake and certificate will be shown in "
                              L"cleartext; the application data itself cannot "
                              L"be read.",
                         left);
            ::SetWindowTextW(text, buf);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == IDM_WAIT_STOP &&
                HIWORD(w) == BN_CLICKED) {
                ::DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
        case WM_DESTROY:
            ::KillTimer(hwnd, kCaptureWaitTimer);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, w, l);
}


*/


// REMOVED 2026-10-05 (todo.md 8.7 G2): FollowSelectedStream, the GUI half of
// "Follow TCP stream". Commented, not deleted, because the chain of reasons it
// failed IS the argument for moving the feature to the CLI:
//
//  1. It re-elevated the ENTIRE application to reach pktmon. Reelevate replaces
//     the process, so the window, the filter box, the column layout and the
//     scroll position are all gone - to look at hex bytes. The original comment
//     claimed the state "is still there on the other side"; the command line is,
//     the user's working state is not.
//  2. Even elevated it is modal: arm the filter, show a wait dialog, sleep, and
//     only THEN show the bytes. pktmon is a sampling driver, not a tap, so
//     anything before `pktmon filter add` is simply not recorded - and a
//     connection that goes quiet in the meantime yields an empty window that the
//     user cannot distinguish from a bug.
//  3. The bytes then land in a hand-rolled hex window. The hex window is the
//     right tool for the job; the flow around it is what made the feature read
//     as broken even when it worked.
//
// The capability was NOT dropped. `capture` already ran this exact pipeline
// (StartCapture -> StopCapture -> Pcapng -> TcpReasm) and then printed one line
// of counters, throwing the payload away. It now has `--text`, which writes the
// reassembled direction to stdout the way `tcpflow -A` does, and `--out FILE`,
// which keeps the pcapng for Wireshark or tshark. On the command line the
// blocking is the point rather than the defect.
//
// void MainWindow::FollowSelectedStream() {
//     const std::optional<Connection> sel = SelectedCopy();
//     if (!sel.has_value()) return;
//     const Connection& c = *sel;
//
//     std::wstring why;
//     if (!CaptureAvailable(&why)) {
//         if (IsAdminMember() && Reelevate(L"Follow TCP stream")) {
//             ::DestroyWindow(hwnd_);
//             return;
//         }
//         ::MessageBoxW(hwnd_, why.c_str(), L"Follow TCP stream",
//                       MB_OK | MB_ICONINFORMATION);
//         return;
//     }
//     if (c.protocol != IPPROTO_TCP || c.state == MIB_TCP_STATE_LISTEN ||
//         c.state == 0) {
//         ::MessageBoxW(hwnd_,
//                       L"Only an established TCP connection has a stream to "
//                       L"follow.\n\nA listening socket has no peer yet.",
//                       L"Follow TCP stream", MB_OK | MB_ICONINFORMATION);
//         return;
//     }
//
//     CaptureTarget target = MakeCaptureTarget(c);
//     std::wstring startError;
//     if (!StartCapture(target, &startError)) {
//         ::MessageBoxW(hwnd_, startError.c_str(), L"Follow TCP stream",
//                       MB_OK | MB_ICONERROR);
//         return;
//     }
//     ShowCaptureWait(hwnd_, target.label);
//     const CaptureResult result = StopCapture(target);
//     stream_.SetDark(ThemeIsDark());
//     stream_.Show(hwnd_, font_, L"WinTCP - TCP stream - " + target.label,
//                  result, target.label);
// }

// ---- 5.2 presets + 5.3 bookmarks -----------------------------------------

// Snapshot the current view into a PresetView.
//
// The traffic-source toggles are included deliberately: a preset that
// captured the columns and filter but not whether GeoIP/DNS/traffic were
// running would restore a view that looks identical on screen but silently
// behaves differently, which is worse than not having presets at all.
PresetView MainWindow::CurrentPresetView() const {
    unsigned sources = 0;
    if (dnsEnabled_) sources |= kPresetSourceHosts;
    if (etw_.Running() || fallbackFlag_->load(std::memory_order_relaxed))
        sources |= kPresetSourceEtw;
    // F5.4: the ASN database shares the GeoIP bit rather than taking a new one.
    // It is the same enrichment applied from a second file, and kPresetSourceAll
    // is a four-bit mask - adding a fifth bit would be a schema change to a
    // persisted value for no distinction a user can act on.
    if (geo_.Loaded() || asnGeo_.Loaded()) sources |= kPresetSourceGeoIp;
    // Widget reads stay here (search box, column mask, sort); the ViewState
    // assembly lives in the abstract layer so the CLI builds the same type.
    return CurrentPresetViewFor(CurrentSearchText(), visibleCols_,
                                store_.SortColumn(), store_.SortAscending(),
                                sources);
}

void MainWindow::ApplyPresetView(const PresetView& v) {
    // Filter first: ParseFilter runs on the text, and doing it before the
    // column rebuild means the filter box and the column mask change together
    // in one repaint rather than showing an intermediate state.
    ::SetWindowTextW(hwndSearchEdit_, v.filter.c_str());
    filterDebounceText_ = v.filter;
    visibleCols_ = ClampVisibleCols(v.colVisible);
    store_.SetSort(v.sortColumn, v.sortAsc);

    // Apply the source toggles. Turning one ON may start a counter source,
    // which is exactly what the preset asked for, so this is not a no-op.
    const bool wantDns = (v.sources & kPresetSourceHosts) != 0;
    if (wantDns != dnsEnabled_) {
        dnsEnabled_ = wantDns;
        dns_.SetEnabled(wantDns);
    }
    const bool wantTraffic = (v.sources & kPresetSourceEtw) != 0;
    if (wantTraffic) EnsureTrafficCounters();

    SyncColumnMenuChecks();
    SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_RESOLVE, dnsEnabled_);
    RebuildColumns();
    ApplyView();
}

void MainWindow::SavePreset() {
    std::wstring name;
    if (!PromptForText(hwnd_, L"Save view as preset",
                       L"Preset name:", L"", kMaxPresetNameChars, &name))
        return;
    // Presets::Save refuses to overwrite by default and says so. The confirm
    // is done HERE, not inside the store, so the store stays a dumb
    // persistence layer that cannot show UI.
    PresetSave result = Presets::Save(name, CurrentPresetView());
    if (result == PresetSave::kExists) {
        std::wstring msg = L"A preset named '" + name + L"' already exists.\r\n\r\n"
                           L"Replace it?";
        if (::MessageBoxW(hwnd_, msg.c_str(), L"Save preset",
                          MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
        result = Presets::Save(name, CurrentPresetView(), true);
    }
    switch (result) {
        case PresetSave::kCreated:
            ::MessageBoxW(hwnd_, L"View saved.", L"Save preset",
                          MB_OK | MB_ICONINFORMATION);
            break;
        case PresetSave::kOverwrote:
            ::MessageBoxW(hwnd_, L"View replaced.", L"Save preset",
                          MB_OK | MB_ICONINFORMATION);
            break;
        case PresetSave::kInvalidName:
            ::MessageBoxW(hwnd_,
                          L"That name cannot be used. It must be 1-"
                          L"64 characters, with no backslash or control "
                          L"characters.",
                          L"Save preset", MB_OK | MB_ICONERROR);
            break;
        case PresetSave::kFailed:
            ::MessageBoxW(hwnd_, L"Could not write the preset to the "
                                 L"registry.",
                          L"Save preset", MB_OK | MB_ICONERROR);
            break;
        case PresetSave::kExists:
            break;   // user declined the replace; not an error
    }
}

void MainWindow::LoadPreset() {
    const std::vector<std::wstring> names = Presets::List();
    if (names.empty()) {
        ::MessageBoxW(hwnd_, L"No saved presets yet.", L"Load preset",
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    // A numbered list rather than a combo: with more than a handful of
    // presets a list box scrolls, and the numbers double as a way to refer to
    // one in the message box.
    std::wstring text = L"Type the number of the preset to load:\r\n\r\n";
    for (size_t i = 0; i < names.size(); ++i)
        text += std::to_wstring(i + 1) + L".  " + names[i] + L"\r\n";

    std::wstring answer;
    if (!PromptForText(hwnd_, L"Load preset", L"", L"1", 8, &answer))
        return;
    // Parse strictly: the field is user input and "2abc" or "-1" must not
    // silently resolve to preset 2 or wrap to the last entry.
    size_t idx = 0;
    if (answer.empty() ||
        !std::all_of(answer.begin(), answer.end(),
                     [](wchar_t c) { return c >= L'0' && c <= L'9'; }) ||
        answer.size() > 4) {
        ::MessageBoxW(hwnd_, L"Enter just the number.", L"Load preset",
                      MB_OK | MB_ICONERROR);
        return;
    }
    for (wchar_t c : answer) idx = idx * 10 + static_cast<size_t>(c - L'0');
    if (idx == 0 || idx > names.size()) {
        ::MessageBoxW(hwnd_, L"There is no preset with that number.",
                      L"Load preset", MB_OK | MB_ICONERROR);
        return;
    }

    PresetView v;
    if (!Presets::Load(names[idx - 1], &v)) {
        // All-or-nothing load: a partially readable preset is reported as a
        // failure rather than applied, because half a view is a state the
        // user never saved and cannot get back to.
        ::MessageBoxW(hwnd_,
                      L"That preset could not be read. It may be corrupt or "
                      L"from a newer version.",
                      L"Load preset", MB_OK | MB_ICONERROR);
        return;
    }
    ApplyPresetView(v);
}

void MainWindow::DeletePreset() {
    const std::vector<std::wstring> names = Presets::List();
    if (names.empty()) {
        ::MessageBoxW(hwnd_, L"No saved presets to delete.", L"Delete preset",
                      MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring text = L"Type the number of the preset to delete:\r\n\r\n";
    for (size_t i = 0; i < names.size(); ++i)
        text += std::to_wstring(i + 1) + L".  " + names[i] + L"\r\n";
    std::wstring answer;
    if (!PromptForText(hwnd_, L"Delete preset", L"", L"1", 8, &answer))
        return;
    size_t idx = 0;
    if (answer.empty() ||
        !std::all_of(answer.begin(), answer.end(),
                     [](wchar_t c) { return c >= L'0' && c <= L'9'; }) ||
        answer.size() > 4) {
        ::MessageBoxW(hwnd_, L"Enter just the number.", L"Delete preset",
                      MB_OK | MB_ICONERROR);
        return;
    }
    for (wchar_t c : answer) idx = idx * 10 + static_cast<size_t>(c - L'0');
    if (idx == 0 || idx > names.size()) {
        ::MessageBoxW(hwnd_, L"There is no preset with that number.",
                      L"Delete preset", MB_OK | MB_ICONERROR);
        return;
    }
    const std::wstring victim = names[idx - 1];
    // Deleting a saved view is irreversible, so it gets a confirm naming the
    // preset. A typo must not cost the user a view they built.
    if (::MessageBoxW(hwnd_,
                      (L"Delete the preset '" + victim + L"'?").c_str(),
                      L"Delete preset",
                      MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    if (!Presets::Delete(victim)) {
        ::MessageBoxW(hwnd_, L"Could not delete the preset.", L"Delete preset",
                      MB_OK | MB_ICONERROR);
    }
}

void MainWindow::ShowBookmarksList() {
    size_t unreadable = 0;
    const std::vector<Bookmark> all = Bookmarks::List(&unreadable);
    if (all.empty()) {
        std::wstring msg = L"No bookmarks yet.\r\n\r\n"
                           L"Right-click a connection and choose "
                           L"\"Bookmark this connection\".";
        if (unreadable != 0)
            msg = L"No readable bookmarks.\r\n\r\n" +
                  std::to_wstring(unreadable) +
                  L" stored bookmark(s) could not be read and are being "
                  L"ignored.";
        ::MessageBoxW(hwnd_, msg.c_str(), L"Bookmarks", MB_OK);
        return;
    }
    std::wstring text;
    for (const Bookmark& b : all) {
        text += b.Key();
        if (b.tag != kBookmarkTagNone) {
            text += L"  [";
            text += BookmarkTagLabel(b.tag);
            text += L"]";
        }
        if (!b.note.empty()) {
            text += L"\r\n    ";
            text += b.note;
        }
        text += L"\r\n\r\n";
    }
    if (unreadable != 0) {
        // Never claim "you have N bookmarks" when some were skipped: a count
        // that quietly drops entries is how a user loses track of one.
        text += std::to_wstring(unreadable) +
                L" further bookmark(s) could not be read and are not shown.\r\n";
    }
    ::MessageBoxW(hwnd_, text.c_str(), L"Bookmarks", MB_OK);
}

// 5.3. Read the bookmark store once and push it into the row model.
//
// The registry read happens here, once per refresh, and the result is a plain
// vector the store can walk without touching the registry. Doing it the other
// way round (asking the store per row) would mean a registry query per
// connection on every tick.
//
// Address, PORT and note are all carried: the port is part of the bookmark's
// identity (see ConnectionStore::JoinBookmarks - matching on the address alone
// flagged the wrong conversation), and the note is what makes it visible in the
// GUI and reachable by a `note:` filter.
void MainWindow::RefreshBookmarkMarks() {
    std::vector<BookmarkMark> known;
    const std::vector<Bookmark> all = Bookmarks::List();
    known.reserve(all.size());
    for (const Bookmark& b : all) {
        BookmarkMark m;
        m.address = b.address;
        m.port = b.port;
        m.tag = b.tag;
        m.note = b.note;
        known.push_back(std::move(m));
    }
    store_.JoinBookmarks(known);
}

void MainWindow::BookmarkSelectedConnection() {
    const std::optional<Connection> sel = SelectedCopy();
    if (!sel.has_value()) return;
    const Connection& c = *sel;
    if (c.remoteAddress.empty()) {
        ::MessageBoxW(hwnd_, L"This row has no remote address to bookmark.",
                      L"Bookmark", MB_OK | MB_ICONERROR);
        return;
    }
    // COPY the endpoint before anything can pump messages.
    //
    // 'c' points into store_.rows_, and every message below that opens a
    // modal dialog runs a message loop - during which a queued refresh
    // arrives, ReplaceSnapshot reallocates rows_, and 'c' dangles. The final
    // SetNote(c.remoteAddress, ...) then read freed memory: an access
    // violation that reproduced roughly one run in three, always right after
    // the note prompt. Copying the two strings the handler needs is the fix;
    // everything past the copy below uses them, not the pointer.
    const std::wstring address = c.remoteAddress;
    const UINT port = c.remotePort;

    const bool exists = Bookmarks::IsBookmarked(address, port);
    if (exists) {
        // Toggle OFF rather than re-adding: the same menu item both creates
        // and removes, which is what a user expects from a checkable action
        // and saves a separate "remove" command. Storage goes through the
        // abstract layer so the GUI and the `bookmark` CLI verb share it.
        const CommandResult rr = CmdBookmarkRemove(address, port);
        if (rr.exitCode == kExitOk) {
            // Re-join rather than just repaint: the pin lives on the row, not
            // in the list view, so a repaint alone would show a bookmark that
            // no longer exists.
            RefreshBookmarkMarks();
            ApplyView();
        } else {
            ::MessageBoxW(hwnd_, Utf8ToWide(rr.err.c_str()).c_str(),
                          L"Bookmark", MB_OK | MB_ICONERROR);
        }
        return;
    }
    {
        const CommandResult ar = CmdBookmarkAdd(address, port, 0, L"");
        if (ar.exitCode != kExitOk) {
            ::MessageBoxW(hwnd_, Utf8ToWide(ar.err.c_str()).c_str(),
                          L"Bookmark", MB_OK | MB_ICONERROR);
            return;
        }
    }
    // Ask for the note straight away: the user is looking at the connection
    // now and this is the moment they know why it is interesting. An empty
    // answer is a perfectly good answer.
    std::wstring note;
    if (PromptForText(hwnd_, L"Bookmark note",
                      L"Note (optional):", L"", Bookmarks::kMaxNoteChars,
                      &note)) {
        (void)CmdBookmarkNote(address, port, note);
    }
    RefreshBookmarkMarks();
    ApplyView();
}

void MainWindow::EditSelectedBookmarkNote() {
    const std::optional<Connection> sel = SelectedCopy();
    if (!sel.has_value()) return;
    const Connection& c = *sel;
    // Copy before the prompt: it runs a modal loop, and a refresh arriving in
    // that window reallocates rows_, leaving 'c' dangling. Same defect as in
    // BookmarkSelectedConnection.
    const std::wstring address = c.remoteAddress;
    const UINT port = c.remotePort;
    Bookmark b;
    if (!Bookmarks::Get(address, port, &b)) {
        ::MessageBoxW(hwnd_, L"This connection is not bookmarked.",
                      L"Bookmark note", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring note;
    if (!PromptForText(hwnd_, L"Bookmark note", L"Note:", b.note.c_str(),
                       Bookmarks::kMaxNoteChars, &note))
        return;
    const CommandResult nr = CmdBookmarkNote(address, port, note);
    if (nr.exitCode != kExitOk) {
        ::MessageBoxW(hwnd_, Utf8ToWide(nr.err.c_str()).c_str(),
                      L"Bookmark note", MB_OK | MB_ICONERROR);
    }
}

void MainWindow::ShowAboutBox() {
    // 7.5. Every line of the capability block is read from live state, not
    // hard-coded: a summary that always said "Administrator: no" would be
    // wrong for exactly the user most likely to be reading it.
    BuildSummary s;
    // IsElevated(), not IsElevatedInstance(): the marker only says "this
    // process was relaunched by WinTCP". A user who launched an already-
    // elevated console has no marker and was told "Administrator no" while
    // every elevated feature worked (D23).
    s.elevated = IsElevated();
    s.etwRunning = etw_.Running();
    s.trafficFallback = fallbackFlag_->load(std::memory_order_relaxed);
    // 9.2.4: the same two counters the status bar shows, so the About box and
    // the pane cannot disagree. Reported only when the sampler exists at all:
    // a window with traffic off has no opinion, and printing a confident 0
    // there would claim a check nobody ran.
    if (socketTraffic_ != nullptr) {
        s.trafficScanRan = true;
        s.trafficTimeouts = socketTraffic_->TimeoutCount();
        s.trafficScanFailures = socketTraffic_->ScanFailureCount();
    }
    s.geoIpLoaded = geo_.Loaded();
    s.presetsAvailable = true;   // the store is in-process; always available
    for (int i = 0; i < COL_COUNT; ++i)
        if ((visibleCols_ & (1u << i)) != 0) ++s.visibleColumnCount;
    s.totalColumnCount = COL_COUNT;
    s.rowCount = store_.View().size();
    s.rowCountKnown = true;
    s.columnCountKnown = true;
    ::MessageBoxW(hwnd_, AboutText(s).c_str(),
                  L"About WinTCP", MB_OK | MB_ICONINFORMATION);
}

// 7.6. The sheet is rendered from the same list the selftest checks, so a
// shortcut that exists but is undocumented (or documented but absent) is a
// test failure rather than something a user discovers.
void MainWindow::ShowShortcutsSheet() {
    ::MessageBoxW(hwnd_, ShortcutsText().c_str(),
                  L"WinTCP — Keyboard shortcuts", MB_OK);
}

// 4.3. Pick a user-supplied MaxMind .mmdb and start filling the Country
// column.
//
// The database is NEVER downloaded. This is a deliberate constraint, not an
// omission: an auto-fetch would mean the app silently pulls tens of megabytes
// from a third party, and a privacy-sensitive network tool that phones home
// is a different product. The user supplies the file.
//
// A load failure reports the parser's own message rather than a generic
// "could not open": GeoIpDatabase::Load distinguishes a missing file from a
// truncated one from a file that is not an MMDB at all, and those need
// different things from the user.
void MainWindow::LoadGeoIpDatabase() {
    wchar_t file[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"MaxMind databases (*.mmdb)\0*.mmdb\0"
                      L"All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open a MaxMind .mmdb database";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    // comdlg32.dll is delay-loaded; see the export-dialog guard above.
    if (!DelayLoadGuard(hwnd_, "comdlg32.dll")) return;
    if (!::GetOpenFileNameW(&ofn)) return;   // cancelled: not an error

    std::wstring error;
    if (!geo_.Load(file, &error)) {
        ::MessageBoxW(hwnd_, error.c_str(), L"GeoIP database not loaded",
                      MB_OK | MB_ICONERROR);
        return;
    }

    // 9.1.5: persist the path the user picked, so it survives a relaunch.
    // Only reached on a successful load: a cancelled picker returned above,
    // and a load that failed returned with the parser's reason.
    ::wcsncpy_s(settings_.geoIpPath, file, _TRUNCATE);
    // Routed through PersistSettings like the other two, and this is the one
    // where the discarded bool hurt most: the user has just clicked a file in a
    // dialog, so a write that fails loses exactly the thing they chose to do,
    // and they find out at the next launch with no message in between. This was
    // the third discarding call site; the two the change names were not the
    // only ones.
    PersistSettings("geoip path picked");

    // Fill immediately rather than waiting for the next tick, so the Country
    // column appears at once. The user chose a file precisely to see results.
    OfferGeoIpForAllRows();
    ApplyView();
    UpdateStatusBar(std::wstring());
}

// F5.4. The ASN picker. A near-copy of LoadGeoIpDatabase, and deliberately so:
// the two differ only in which member they load, which settings key they write
// and the words in the message boxes. Factoring the shared half would mean
// threading a "which database" enum through the error strings for no gain, and
// the two will diverge anyway - this one names ASN in every message so a user
// who picked the wrong file can tell which half is unhappy.
void MainWindow::LoadAsnDatabase() {
    wchar_t file[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"MaxMind ASN databases (*.mmdb)\0*.mmdb\0"
                      L"All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    // Naming the expected product in the title: GeoLite2-Country is the file
    // most people already have, and picking it here produces an ASN column that
    // is always empty for a reason no message would otherwise explain.
    ofn.lpstrTitle = L"Open a GeoLite2-ASN .mmdb database";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!DelayLoadGuard(hwnd_, "comdlg32.dll")) return;
    if (!::GetOpenFileNameW(&ofn)) return;   // cancelled: not an error

    std::wstring error;
    if (!asnGeo_.Load(file, &error)) {
        ::MessageBoxW(hwnd_, error.c_str(),
                      L"ASN database not loaded (expected GeoLite2-ASN)",
                      MB_OK | MB_ICONERROR);
        return;
    }

    ::wcsncpy_s(settings_.asnIpPath, file, _TRUNCATE);
    PersistSettings("asn path picked");

    OfferAsnForAllRows();
    ApplyView();
    UpdateStatusBar(std::wstring());
}

// ---- settings ---------------------------------------------------

void MainWindow::SaveSettings() {
    RECT rc = {};
    if (hwnd_ != nullptr && ::GetWindowRect(hwnd_, &rc) != FALSE) {
        settings_.winX = rc.left;
        settings_.winY = rc.top;
        settings_.winW = rc.right - rc.left;
        settings_.winH = rc.bottom - rc.top;
        settings_.winPlaced = settings_.winW > 0 && settings_.winH > 0;
        settings_.showCmd = ::IsZoomed(hwnd_) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    }
    HarvestColumnWidths();                 // physical -> logical widths
    settings_.colVisible = visibleCols_;
    for (int i = 0; i < COL_COUNT; ++i) settings_.colWidths[i] = colWidths_[i];
    settings_.colWidthsValid = true;
    settings_.sortCol = store_.SortColumn();
    settings_.sortAsc = store_.SortAscending();
    settings_.autoRefresh = autoRefresh_;
    settings_.intervalMs = autoRefreshMs_;
    settings_.resolveHosts = dnsEnabled_;
    settings_.topMost = topMost_;
    settings_.trayEnabled = trayEnabled_;
    settings_.trayMinimizeChoice = trayMinimizeChoice_;
    settings_.trafficEnabled = etw_.Running();
    const std::wstring filter = CurrentSearchText();
    ::wcsncpy_s(settings_.filter, filter.c_str(), _TRUNCATE);
    // 9.1.5: the GeoIP path is part of the view, not just the data - it has
    // to leave here with everything else the user chose.
    //
    // Set BEFORE the single write below. It used to be set BETWEEN two calls to
    // settings_.Save(), so the first write could never have carried it and was
    // pure waste: two full registry writes on every exit, and the second
    // overwrote the first regardless.
    ::wcsncpy_s(settings_.geoIpPath,
                geo_.Loaded() ? geo_.SourcePath().c_str() : L"",
                _TRUNCATE);
    // F5.4: the ASN path is remembered on the same terms - whatever is actually
    // loaded - so a database the user cleared does not come back on the next
    // launch. Same single PersistSettings call below; do not add another.
    ::wcsncpy_s(settings_.asnIpPath,
                asnGeo_.Loaded() ? asnGeo_.SourcePath().c_str() : L"",
                _TRUNCATE);
    PersistSettings("shutdown");
}

// The one place this class writes settings. Every caller goes through here, and
// the reason is not tidiness: the two sites this replaced each called
// Settings::Save() directly and each DISCARDED the bool, so a write that failed
// was indistinguishable from one that succeeded and the user's column layout,
// window position, filter and GeoIP path were simply gone on the next launch.
//
// `why` names the caller. It costs one argument and buys the ability to answer
// "which of these lost my settings?" from a counter instead of a memory.
void MainWindow::PersistSettings(const char* why) {
    ++settingsSaveAttempts_;
    if (!settings_.Save()) {
        ++settingsSaveFailures_;
        // Deliberately NOT a MessageBox. One of the two callers runs from
        // ~MainWindow, where popping a dialog up during teardown is worse than
        // saying nothing; the other runs on a live window, where a box on every
        // drag would be intolerable. So the count is recorded and read through
        // the op table, and no attempt is made to interrupt the user for it.
        (void)why;
    }
}

// ---- change log -------------------------------------------------

namespace {

// Open 'path' in append mode, writing the CSV header when the file is
// new or empty. Returns INVALID_HANDLE_VALUE on failure.
HANDLE OpenLogAppend(const wchar_t* path, DWORD& error) {
    HANDLE h = ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = ::GetLastError();
        return h;
    }
    // OPEN_ALWAYS starts at offset 0; move to the end so we append.
    // SetFilePointerEx, not SetFilePointer: a file position of 0xFFFFFFFF
    // is a legal 4 GiB offset, so it cannot be distinguished from the
    // INVALID_SET_FILE_POINTER failure sentinel.
    LARGE_INTEGER end = {};
    end.QuadPart = 0;
    if (::SetFilePointerEx(h, end, nullptr, FILE_END) == FALSE) {
        error = ::GetLastError();
        ::CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    LARGE_INTEGER size = {};
    if (::GetFileSizeEx(h, &size) != FALSE && size.QuadPart == 0) {
        const char* header =
            "time,event,proto,local,remote,state,oldstate,pid,process\r\n";
        DWORD written = 0;
        ::WriteFile(h, header, static_cast<DWORD>(strlen(header)), &written,
                    nullptr);
    }
    error = NO_ERROR;
    return h;
}

}  // namespace

bool MainWindow::OpenLogFromSettings() {
    if (!settings_.logEnabled || settings_.logPath[0] == 0) return false;
    DWORD err = NO_ERROR;
    logFile_ = OpenLogAppend(settings_.logPath, err);
    if (logFile_ == INVALID_HANDLE_VALUE) {
        // The saved path is gone or unwritable: drop logging quietly and
        // keep the app usable (the user can pick a new file anytime).
        settings_.logEnabled = false;
        settings_.logPath[0] = 0;
        logPath_.clear();
        return false;
    }
    logPath_ = settings_.logPath;
    SetMenuCheck(::GetMenu(hwnd_), IDM_FILE_CHANGELOG, true);
    return true;
}

void MainWindow::ToggleChangeLog() {
    if (logFile_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(logFile_);
        logFile_ = INVALID_HANDLE_VALUE;
        logPath_.clear();
        settings_.logEnabled = false;
        settings_.logPath[0] = 0;
        SetMenuCheck(::GetMenu(hwnd_), IDM_FILE_CHANGELOG, false);
        return;
    }

    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    ofn.lpstrFilter = L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"csv";
    ofn.lpstrTitle = L"WinTCP - Select change log file";
    // Append semantics: no overwrite prompt (existing content is kept).
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    // comdlg32.dll is delay-loaded; see the export-dialog guard above.
    if (!DelayLoadGuard(hwnd_, "comdlg32.dll")) return;
    if (!::GetSaveFileNameW(&ofn)) return;

    DWORD err = NO_ERROR;
    HANDLE h = OpenLogAppend(path, err);
    if (h == INVALID_HANDLE_VALUE) {
        const std::wstring msg =
            L"Could not open the log file:\n" + FormatSystemError(err);
        ::MessageBoxW(hwnd_, msg.c_str(), L"WinTCP - Change log",
                      MB_OK | MB_ICONERROR);
        return;
    }
    logFile_ = h;
    logPath_ = path;
    settings_.logEnabled = true;
    ::wcsncpy_s(settings_.logPath, path, _TRUNCATE);
    SetMenuCheck(::GetMenu(hwnd_), IDM_FILE_CHANGELOG, true);
}

void MainWindow::WriteChangeLog() {
    // Always drain: events must not pile up while logging is off.
    //
    // TakeChangeEvents is DESTRUCTIVE - the first caller gets the events and
    // everyone after gets nothing. The file writer and the change-log window
    // are two independent consumers of the same stream, so the drain happens
    // ONCE here and the batch is handed to both. Draining inside the
    // "logging is off" early-return would starve the window of every event
    // on a machine where the file writer is not enabled, which is the default
    // and therefore the common case.
    const std::vector<RowChange> changes = store_.TakeChangeEvents();
    if (logFile_ == INVALID_HANDLE_VALUE || changes.empty()) {
        // The window is fed even when the file writer is off, and even when
        // there is nothing to write, so the two can never disagree about
        // which events exist.
        if (changeLog_.IsOpen()) changeLog_.AppendEvents(changes);
        return;
    }

    std::string out;
    for (const RowChange& ch : changes) {
        const char* event =
            (ch.kind == kChangeAppear)     ? "APPEAR"
            : (ch.kind == kChangeDisappear) ? "DISAPPEAR"
                                            : "STATE";
        std::string oldState;
        if (ch.kind == kChangeState) {
            Connection tmp = ch.row;
            tmp.state = ch.oldState;
            ConnectionStore::FinalizeRow(tmp);   // idempotent label refresh
            oldState = WideToUtf8(tmp.stateLabel);
        }
        out += WideToUtf8(FormatCurrentTime());
        out += ',';
        out += event;
        out += ',';
        out += CsvEscapeUtf8(WideToUtf8(ch.row.protoLabel));
        out += ',';
        out += CsvEscapeUtf8(WideToUtf8(ch.row.localEndpoint));
        out += ',';
        out += CsvEscapeUtf8(WideToUtf8(ch.row.remoteEndpoint));
        out += ',';
        out += CsvEscapeUtf8(WideToUtf8(ch.row.stateLabel));
        out += ',';
        out += CsvEscapeUtf8(oldState);
        out += ',';
        out += std::to_string(ch.row.pid);
        out += ',';
        out += CsvEscapeUtf8(WideToUtf8(ch.row.processName));
        out += "\r\n";
    }
    DWORD written = 0;
    ::WriteFile(logFile_, out.data(), static_cast<DWORD>(out.size()), &written,
                nullptr);
    // The window sees the same batch the file just got, from the same
    // drain. Appending here rather than at the drain means the file writer
    // cannot skip it on a write error.
    if (changeLog_.IsOpen()) changeLog_.AppendEvents(changes);
}

// ---- keyboard UX ------------------------------------------------

void MainWindow::SelectAllRows() {
    if (hwndList_ == nullptr) return;
    LVITEMW it = {};
    it.stateMask = LVIS_SELECTED;
    it.state = LVIS_SELECTED;
    ::SendMessageW(hwndList_, LVM_SETITEMSTATE,
                   static_cast<WPARAM>(-1) /* all items */,
                   reinterpret_cast<LPARAM>(&it));
}

void MainWindow::FocusFilterBox() {
    if (hwndSearchEdit_ == nullptr) return;
    ::SetFocus(hwndSearchEdit_);
    ::SendMessageW(hwndSearchEdit_, EM_SETSEL, 0, -1);
}

void MainWindow::ClearFilterBox() {
    if (hwndSearchEdit_ != nullptr && !CurrentSearchText().empty())
        ::SetWindowTextW(hwndSearchEdit_, L"");
    // Re-arm the debounce timer explicitly: EN_CHANGE is not guaranteed
    // for a programmatic SetWindowText when the box was already empty,
    // and ApplyView is cheap and idempotent either way.
    ::KillTimer(hwnd_, kFilterTimerId);
    ::SetTimer(hwnd_, kFilterTimerId, kFilterDebounceMs, nullptr);
}

// ---- system colors / high contrast -----------------------------------------
// The app has no theme of its own: it uses the system window colors, so it
// follows the user's light/dark preference automatically. When high
// contrast is active (WO_HC_ACTIVE) those colors are the accessibility
// palette, which is exactly what we want - so this is one code path, not a
// special case.

bool MainWindow::HighContrastActive() const {
    HIGHCONTRASTW hc = {};
    hc.cbSize = sizeof(hc);
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) == 0)
        return false;
    return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

// True when the system theme is dark, so highlight colors can be picked to
// stay legible. Derived from COLOR_WINDOW rather than a setting: the app
// has no theme of its own, it simply follows the system one. High contrast
// always counts as "light" for this purpose - its palette is chosen for
// maximum contrast, not for darkness.
bool MainWindow::ThemeIsDark() const {
    if (HighContrastActive()) return false;
    const COLORREF bg = ::GetSysColor(COLOR_WINDOW);
    const BYTE r = static_cast<BYTE>(bg & 0xFF);
    const BYTE g = static_cast<BYTE>((bg >> 8) & 0xFF);
    const BYTE b = static_cast<BYTE>((bg >> 16) & 0xFF);
    // Rec. 601 luma; below 128 counts as a dark background.
    return (299 * r + 587 * g + 114 * b) / 1000 < 128;
}

void MainWindow::RefreshSystemColors() {
    // 5.4: the change-log window is a top-level window, so it does not get
    // WM_SYSCOLORCHANGE from the main window. Forward it explicitly or the
    // log window keeps stale colours after a theme change while the rest of
    // the app has already repainted.
    if (changeLog_.IsOpen()) {
        ::SendMessageW(changeLog_.Handle(), WM_SYSCOLORCHANGE, 0, 0);
        ::RedrawWindow(changeLog_.Handle(), nullptr, nullptr,
                       RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    const COLORREF bg = ::GetSysColor(COLOR_WINDOW);
    const COLORREF tx = ::GetSysColor(COLOR_WINDOWTEXT);

    if (hwndBrushWindow_ != nullptr) ::DeleteObject(hwndBrushWindow_);
    hwndBrushWindow_ = ::CreateSolidBrush(bg);

    if (hwndList_ != nullptr) {
        ::SendMessageW(hwndList_, LVM_SETTEXTCOLOR, 0, tx);
        ::SendMessageW(hwndList_, LVM_SETTEXTBKCOLOR, 0, bg);
        ::SendMessageW(hwndList_, LVM_SETBKCOLOR, 0, bg);
    }
    if (hwndStatus_ != nullptr)
        ::SendMessageW(hwndStatus_, SB_SETBKCOLOR, 0,
                       static_cast<LPARAM>(::GetSysColor(COLOR_3DFACE)));
    ::RedrawWindow(hwnd_, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// ---- tray icon / minimize-to-tray -------------------------------

void MainWindow::TrayAdd() {
    if (trayIconShown_ || hwnd_ == nullptr) return;
    // shell32.dll is delay-loaded (kDelayedDlls in WinCaps.cpp) and
    // Shell_NotifyIconW below would RAISE on a machine without it. Gating
    // here means "minimize to tray" silently does not appear, which is the
    // honest degradation: there is no tray to minimize to.
    if (!DllAvailable("shell32.dll")) return;
    trayNid_ = {};
    trayNid_.cbSize = sizeof(trayNid_);
    trayNid_.hWnd = hwnd_;
    trayNid_.uID = 1;
    trayNid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    trayNid_.uCallbackMessage = WM_APP_TRAY;
    trayNid_.hIcon = static_cast<HICON>(::LoadImageW(
        hInstance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, 0));
    if (trayNid_.hIcon == nullptr)
        trayNid_.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    ::wcscpy_s(trayNid_.szTip, L"WinTCP - TCP/UDP Connections");
    trayIconShown_ = ::Shell_NotifyIconW(NIM_ADD, &trayNid_) != FALSE;
}

void MainWindow::TrayRemove() {
    if (!trayIconShown_) return;
    // shell32.dll is delay-loaded (kDelayedDlls in WinCaps.cpp). This gate is
    // load-bearing even though trayIconShown_ looks like it already implies the
    // library is there: it does NOT, because on a machine without shell32 the
    // TrayAdd call below would have raised and left the flag false - so the
    // invariant holds, and this guard makes that dependency explicit rather
    // than incidental.
    if (!DllAvailable("shell32.dll")) return;
    ::Shell_NotifyIconW(NIM_DELETE, &trayNid_);
    trayIconShown_ = false;
}

void MainWindow::ShowTrayMenu() {
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) return;
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_RESTORE, L"&Open WinTCP");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (topMost_ ? MF_CHECKED : 0),
                  IDM_VIEW_TOPMOST, L"Always on top");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // D30: this is the item the Block confirmation has always pointed at
    // ("use Remove All WinTCP Blocks from the tray menu"). It did not exist
    // until now, so the documented removal path has a handler; the handler
    // reports "no rules" / "needs elevation" honestly. Always shown - built
    // directly from the menu, no COM-query gymnastics required.
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_UNBLOCK_ALL,
                  L"Remove All WinTCP Blocks");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_FILE_EXIT, L"E&xit");

    POINT pt = {};
    ::GetCursorPos(&pt);
    // Required so the menu dismisses correctly when the window is hidden.
    ::SetForegroundWindow(hwnd_);
    const UINT cmd = ::TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
        hwnd_, nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);
    if (cmd != 0) {
        ::SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
    }
}

MainWindow::MinimizeTarget MainWindow::ResolveMinimize() {
    // 9.4.6: the prompt only fires when the tray icon is OFF. With the tray on,
    // the existing SC_MINIMIZE handler hides the window to the tray directly.
    if (trayEnabled_) return MinimizeTarget::kTray;

    // kTrayMinimizeChoice: 0 = never asked, 1 = remembered tray, 2 = remembered exit.
    if (trayMinimizeChoice_ == 1) return MinimizeTarget::kTray;
    if (trayMinimizeChoice_ == 2) return MinimizeTarget::kExit;

    // First minimize without a remembered choice: prompt. A TaskDialog gives us a
    // checkbox for "don't ask again" with no template to maintain, and is
    // available on every Windows version this tool supports (Vista+).
    //
    // comdlg32.dll (TaskDialogIndirect) is delay-loaded; if it is absent the
    // prompt cannot be shown, so fall back to the plain taskbar minimize the
    // user asked for.
    if (!DelayLoadGuard(hwnd_, "comdlg32.dll")) return MinimizeTarget::kTaskbar;

    TASKDIALOGCONFIG tc = {};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = hwnd_;
    tc.dwFlags = TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.hMainIcon = ::LoadIconW(nullptr, IDI_QUESTION);
    tc.pszWindowTitle = L"WinTCP";
    tc.pszMainInstruction =
        L"Minimize to tray hides WinTCP in the notification area instead of "
        L"keeping a taskbar button, and adds a tray icon you can right-click.";
    tc.pszContent =
        L"How do you want WinTCP to behave when you minimize it?";
    tc.pszVerificationText = L"&Don't ask me again (applies to both options)";

    enum { kBtnTray = 100, kBtnExit };
    TASKDIALOG_BUTTON btns[2] = {
        { kBtnTray, L"&Minimize to tray" },
        { kBtnExit, L"C&lose WinTCP (exit)" },
    };
    tc.pButtons = btns;
    tc.cButtons = 2;
    tc.nDefaultButton = kBtnTray;          // tray is the safer, more useful default

    int which = 0;
    int radio = 0;
    BOOL checked = FALSE;
    HRESULT hr = ::TaskDialogIndirect(&tc, &which, &radio, &checked);
    if (hr != S_OK || which == kBtnExit) {
        // A cancel / close (X) or explicit exit: treat as exit so the window
        // does not vanish into a hidden state with no icon to restore it.
        trayMinimizeChoice_ = 2;
        settings_.trayMinimizeChoice = 2;
        if (checked) settings_.Save();
        return MinimizeTarget::kExit;
    }

    // "Don't ask again" checkbox. When set, remember the chosen target so the
    // prompt never returns. When not set, leave choice = 0 and the dialog will
    // fire again next time - the user has not consented to a permanent decision.
    if (checked) {
        trayMinimizeChoice_ = 1;           // remember: tray
        settings_.trayMinimizeChoice = 1;
    }
    // Enable the tray icon so the chosen behaviour takes effect on the next
    // minimize and the icon survives this session.
    trayEnabled_ = true;
    settings_.trayEnabled = true;
    settings_.Save();
    SetMenuCheck(::GetMenu(hwnd_), IDM_VIEW_TRAY, true);
    TrayAdd();
    return MinimizeTarget::kTray;
}

}  // namespace wintcp
