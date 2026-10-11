// ProcessTreeWindow.cpp
// SPDX-License-Identifier: Apache-2.0
// See ProcessTreeWindow.h. A modeless tree over one snapshot of rows.

#include "ProcessTreeWindow.h"

#include <commctrl.h>

#include <unordered_map>

#include "ProcessTree.h"
#include "Utils.h"

#pragma comment(lib, "comctl32.lib")

namespace wintcp {
namespace {

// Control ids are local to this window, so they cannot collide with the
// main window's control ids.
const UINT kIdTree = 1;
const UINT kIdRefresh = 2;
const UINT kIdClose = 3;

// Logical (96 dpi) layout metrics, in the grid every MulDiv here scales from.
constexpr int kMargin96 = 8;
constexpr int kBtnH96 = 26;
constexpr int kBtnW96 = 90;
constexpr int kBtnGap96 = 6;
constexpr int kStatusH96 = 22;
constexpr int kMetricsDpi = 96;

// Default and minimum window sizes in that grid. Wide enough that a
// "name (PID n, k connections)" label fits without horizontal scrolling
// for the common case; the tree scrolls when it does not.
constexpr int kDefaultWidth96 = 520;
constexpr int kDefaultHeight96 = 560;
constexpr int kMinTrackWidth96 = 320;
constexpr int kMinTrackHeight96 = 240;

// Longest tree label assembled below. A name plus " (PID 4294967295,
// 18446744073709551615 connections)" cannot reach this; swprintf_s
// truncates rather than overruns in any case.
constexpr size_t kLabelChars = 320;

void SetChildFont(HWND hwnd, HFONT font) {
    if (font != nullptr && hwnd != nullptr) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                       MAKELPARAM(TRUE, 0));
    }
}

}  // namespace

const wchar_t* ProcessTreeWindow::kClassName = L"WinTcpProcessTreeWnd";

ProcessTreeWindow::~ProcessTreeWindow() {
    // Nothing owned: the font is borrowed and the tree owns its own items,
    // so repeated open/close cycles cannot accumulate.
    Close();
}

int ProcessTreeWindow::S(int px96) const {
    const UINT dpi = QueryDpiForWindow(hwnd_);
    return ::MulDiv(px96, static_cast<int>(dpi), kMetricsDpi);
}

void ProcessTreeWindow::CenterOnOwner(HWND owner) {
    if (hwnd_ == nullptr) return;
    RECT rcOwner = {};
    if (owner != nullptr && ::GetWindowRect(owner, &rcOwner)) {
        RECT rcSelf = {};
        ::GetWindowRect(hwnd_, &rcSelf);
        const int w = rcSelf.right - rcSelf.left;
        const int h = rcSelf.bottom - rcSelf.top;
        int x = rcOwner.left + ((rcOwner.right - rcOwner.left) - w) / 2;
        int y = rcOwner.top + ((rcOwner.bottom - rcOwner.top) - h) / 2;
        HMONITOR mon = ::MonitorFromRect(&rcOwner, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (::GetMonitorInfoW(mon, &mi)) {
            const RECT& wa = mi.rcWork;
            if (x + w > wa.right) x = wa.right - w;
            if (y + h > wa.bottom) y = wa.bottom - h;
            if (x < wa.left) x = wa.left;
            if (y < wa.top) y = wa.top;
        }
        ::SetWindowPos(hwnd_, nullptr, x, y, 0, 0,
                       SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void ProcessTreeWindow::Show(HWND owner, HFONT font, RowSupplier supplier) {
    supplier_ = std::move(supplier);
    if (font_ != font) ApplyFont(font);

    if (hwnd_ == nullptr) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &ProcessTreeWindow::WindowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        if (::RegisterClassExW(&wc) == 0 &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return;

        const UINT dpi = QueryDpiForWindow(owner);
        hwnd_ = ::CreateWindowExW(
            0, kClassName, L"WinTCP - Process tree",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
                WS_CLIPCHILDREN | WS_MAXIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT,
            ::MulDiv(kDefaultWidth96, static_cast<int>(dpi), kMetricsDpi),
            ::MulDiv(kDefaultHeight96, static_cast<int>(dpi), kMetricsDpi),
            owner, nullptr, ::GetModuleHandleW(nullptr), this);
        if (hwnd_ == nullptr) return;
        CenterOnOwner(owner);
    }

    Rebuild();
    if (::IsWindowVisible(hwnd_) == FALSE) ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
}

void ProcessTreeWindow::RefreshNow() {
    if (hwnd_ == nullptr) return;
    Rebuild();
}

void ProcessTreeWindow::Close() {
    if (hwnd_ != nullptr && ::IsWindow(hwnd_)) ::DestroyWindow(hwnd_);
}

bool ProcessTreeWindow::IsVisible() const {
    return hwnd_ != nullptr && ::IsWindowVisible(hwnd_) == TRUE;
}

void ProcessTreeWindow::ApplyFont(HFONT font) {
    font_ = font;   // borrowed from MainWindow; never deleted here
    if (font_ == nullptr) return;
    SetChildFont(hwndTree_, font_);
    SetChildFont(hwndRefresh_, font_);
    SetChildFont(hwndClose_, font_);
    SetChildFont(hwndStatus_, font_);
}

// One process in the snapshot: every row with this PID folds into it, so
// the tree shows processes (like Task Manager) rather than sockets.
struct TreeProc {
    DWORD pid = 0;
    DWORD ppid = 0;
    bool ppidKnown = false;
    std::wstring name;
    size_t connections = 0;
};

void ProcessTreeWindow::Rebuild() {
    if (hwndTree_ == nullptr) return;
    ::SendMessageW(hwndTree_, TVM_DELETEITEM, 0,
                   reinterpret_cast<LPARAM>(TVI_ROOT));

    std::vector<Connection> rows;
    if (supplier_ != nullptr) rows = supplier_();

    if (rows.empty()) {
        UpdateStatus(0, 0, false, true);
        return;
    }

    // Aggregate rows by PID, first-seen order. The engine builds its tree
    // over one TreeNodeInfo per process, indexed by row position, so the
    // aggregation order here is the index space there.
    std::vector<TreeProc> procs;
    procs.reserve(rows.size());
    std::unordered_map<DWORD, size_t> byPid;
    byPid.reserve(rows.size() * 2 + 1);
    for (const Connection& r : rows) {
        auto it = byPid.find(r.pid);
        if (it == byPid.end()) {
            TreeProc p;
            p.pid = r.pid;
            p.ppid = r.ppid;
            p.ppidKnown = r.ppidKnown;
            p.name = r.processName.empty() ? std::wstring(L"(unknown)")
                                           : r.processName;
            p.connections = 1;
            byPid[r.pid] = procs.size();
            procs.push_back(std::move(p));
        } else {
            procs[it->second].connections += 1;
        }
    }

    std::vector<TreeNodeInfo> infos;
    infos.reserve(procs.size());
    for (const TreeProc& p : procs) {
        TreeNodeInfo info;
        info.pid = p.pid;
        info.ppid = p.ppid;
        info.ppidKnown = p.ppidKnown;
        infos.push_back(info);
    }
    // A cycle or an over-deep chain is REPORTED by the engine, never thrown:
    // a partially-drawn tree beats no tree, and depthExceeded is shown
    // rather than silently truncating.
    const ProcessTree tree = BuildProcessTree(infos);

    ::SendMessageW(hwndTree_, WM_SETREDRAW, FALSE, 0);
    for (size_t r = 0; r < tree.rootCount && r < tree.nodes.size(); ++r) {
        // Iterative descent with the engine's own visited set as the back
        // stop: BuildProcessTree already broke cycles, but a second guard
        // here means a future engine change cannot hang the UI.
        struct Frame {
            size_t node;
            HTREEITEM parent;
            size_t child;
            HTREEITEM self;
            bool entered;
        };
        std::vector<Frame> stack;
        stack.reserve(16);
        stack.push_back(Frame{r, TVI_ROOT, 0, nullptr, false});
        size_t guard = 0;
        const size_t guardCap = (tree.nodes.size() + 1) * 2;
        while (!stack.empty() && guard < guardCap) {
            ++guard;
            Frame& f = stack.back();
            if (f.node >= tree.nodes.size()) {
                stack.pop_back();
                continue;
            }
            const TreeNode& n = tree.nodes[f.node];
            if (!f.entered) {
                f.entered = true;
                const TreeProc& p =
                    procs[n.rowIndex < procs.size() ? n.rowIndex : 0];
                wchar_t label[kLabelChars] = {0};
                ::swprintf_s(label, L"%ls  (PID %lu, %zu connection%ls)",
                             p.name.c_str(),
                             static_cast<unsigned long>(p.pid),
                             p.connections,
                             (p.connections == 1) ? L"" : L"s");
                TVINSERTSTRUCTW ins = {};
                ins.hParent = f.parent;
                ins.hInsertAfter = TVI_LAST;
                ins.item.mask = TVIF_TEXT | TVIF_PARAM;
                ins.item.pszText = label;
                ins.item.lParam = static_cast<LPARAM>(p.pid);
                f.self = reinterpret_cast<HTREEITEM>(::SendMessageW(
                    hwndTree_, TVM_INSERTITEMW, 0,
                    reinterpret_cast<LPARAM>(&ins)));
                if (f.self == nullptr) {
                    stack.pop_back();
                    continue;
                }
            }
            if (f.child < n.children.size()) {
                const size_t next = n.children[f.child];
                ++f.child;
                // Never descend into an ancestor: the engine reports cycles
                // instead of following them, and so does this walk.
                bool backEdge = false;
                for (const Frame& a : stack) {
                    if (a.node == next) {
                        backEdge = true;
                        break;
                    }
                }
                if (!backEdge)
                    stack.push_back(Frame{next, f.self, 0, nullptr, false});
            } else {
                // Roots start expanded one level, so the forest reads as a
                // tree on open rather than a list of roots.
                if (f.parent == TVI_ROOT && f.self != nullptr)
                    ::SendMessageW(hwndTree_, TVM_EXPAND,
                                   static_cast<WPARAM>(TVE_EXPAND),
                                   reinterpret_cast<LPARAM>(f.self));
                stack.pop_back();
            }
        }
    }
    ::SendMessageW(hwndTree_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(hwndTree_, nullptr, TRUE);
    UpdateStatus(procs.size(), rows.size(), tree.depthExceeded, false);
}

void ProcessTreeWindow::UpdateStatus(size_t processes, size_t connections,
                                     bool depthExceeded, bool empty) {
    if (hwndStatus_ == nullptr) return;
    std::wstring text;
    if (empty) {
        // G35: an empty tree names its next action. The tree renders the
        // main table's snapshot, so an empty tree means an empty table -
        // and Refresh here only re-reads it.
        text = L"No processes \u2014 the main table has no rows. Refresh "
               L"the main window, then Refresh here.";
    } else {
        text = std::to_wstring(processes) + L" processes, " +
               std::to_wstring(connections) + L" connections";
        if (depthExceeded)
            text += L"  \u2022  very deep chain: some nodes omitted";
    }
    ::SetWindowTextW(hwndStatus_, text.c_str());
}

void ProcessTreeWindow::Layout(int cx, int cy) {
    if (hwnd_ == nullptr) return;
    const int m = S(kMargin96);
    const int btnH = S(kBtnH96);
    const int btnY = cy - m - btnH;

    int x = cx - m - S(kBtnW96);
    if (hwndClose_ != nullptr) {
        ::MoveWindow(hwndClose_, x, btnY, S(kBtnW96), btnH, TRUE);
        x -= S(kBtnW96) + S(kBtnGap96);
    }
    if (hwndRefresh_ != nullptr)
        ::MoveWindow(hwndRefresh_, x, btnY, S(kBtnW96), btnH, TRUE);

    const int statusH = S(kStatusH96);
    const int statusY = btnY + (btnH - statusH) / 2;
    if (hwndStatus_ != nullptr) {
        const int statusW = (x > m) ? (x - S(kBtnGap96) - m) : 0;
        ::MoveWindow(hwndStatus_, m, statusY,
                     (statusW > 0) ? statusW : 0, statusH, TRUE);
    }
    if (hwndTree_ != nullptr) {
        const int treeH = btnY - m - m;
        ::MoveWindow(hwndTree_, m, m, cx - 2 * m,
                     (treeH > 0) ? treeH : 0, TRUE);
    }
}

LRESULT CALLBACK ProcessTreeWindow::WindowProc(HWND hwnd, UINT msg,
                                               WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* self = static_cast<ProcessTreeWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        // Published before WM_CREATE, so the children built there parent
        // to the right window (see ChangeLogWindow for the failure this
        // ordering prevents).
        if (self != nullptr) self->hwnd_ = hwnd;
        // Must return TRUE or the window is destroyed before WM_CREATE.
        return TRUE;
    }
    auto* self = reinterpret_cast<ProcessTreeWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) return self->HandleMessage(msg, wParam, lParam);
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ProcessTreeWindow::TreeSubclassProc(HWND hwnd, UINT msg,
                                                     WPARAM wParam,
                                                     LPARAM lParam, UINT_PTR,
                                                     DWORD_PTR refData) {
    auto* self = reinterpret_cast<ProcessTreeWindow*>(refData);
    // G22: Esc hides even when the tree owns the focus - keys go to the
    // focused child, so the parent's own WM_KEYDOWN never fires for them.
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE && self != nullptr) {
        if (self->Handle() != nullptr) ::ShowWindow(self->Handle(), SW_HIDE);
        return 0;
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT ProcessTreeWindow::HandleMessage(UINT msg, WPARAM wParam,
                                         LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            hwndTree_ = nullptr;
            hwndRefresh_ = nullptr;
            hwndClose_ = nullptr;
            hwndStatus_ = nullptr;
            const HINSTANCE inst = ::GetModuleHandleW(nullptr);
            hwndTree_ = ::CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                    TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                    TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
                0, 0, 0, 0, hwnd_,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdTree)),
                inst, nullptr);
            if (hwndTree_ != nullptr) {
                ::SetWindowSubclass(hwndTree_,
                                    &ProcessTreeWindow::TreeSubclassProc, 1,
                                    reinterpret_cast<DWORD_PTR>(this));
            }
            hwndRefresh_ = ::CreateWindowExW(
                0, L"BUTTON", L"&Refresh",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd_,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdRefresh)),
                inst, nullptr);
            hwndClose_ = ::CreateWindowExW(
                0, L"BUTTON", L"C&lose",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                0, 0, 0, 0, hwnd_,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdClose)),
                inst, nullptr);
            hwndStatus_ = ::CreateWindowExW(
                0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0,
                0, 0, hwnd_, nullptr, inst, nullptr);
            ApplyFont(font_);
            Rebuild();
            return 0;
        }
        case WM_SIZE:
            Layout(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_GETMINMAXINFO: {
            const UINT dpi = QueryDpiForWindow(hwnd_);
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x =
                ::MulDiv(kMinTrackWidth96, static_cast<int>(dpi),
                         kMetricsDpi);
            mmi->ptMinTrackSize.y =
                ::MulDiv(kMinTrackHeight96, static_cast<int>(dpi),
                         kMetricsDpi);
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyFont(font_);
            RECT rc = {};
            ::GetClientRect(hwnd_, &rc);
            Layout(rc.right, rc.bottom);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (LOWORD(wParam) == kIdRefresh) {
                    RefreshNow();
                    return 0;
                }
                if (LOWORD(wParam) == kIdClose) {
                    ::ShowWindow(hwnd_, SW_HIDE);
                    return 0;
                }
            }
            break;
        case WM_KEYDOWN:
            // G22: Esc hides (the supplier snapshot is kept, so re-showing
            // without Refresh shows the same tree, not an empty one).
            if (wParam == VK_ESCAPE) {
                ::ShowWindow(hwnd_, SW_HIDE);
                return 0;
            }
            break;
        case WM_SYSCOLORCHANGE:
            // G23: every colour is live (system brush, default tree
            // colours), so a theme or high-contrast switch needs only a
            // repaint, not a re-derivation.
            ::InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        case WM_CLOSE:
            // Hide, keep the snapshot: reopening shows the same tree until
            // Refresh re-reads the main table.
            ::ShowWindow(hwnd_, SW_HIDE);
            return 0;
        case WM_DESTROY:
            hwndTree_ = nullptr;
            hwndRefresh_ = nullptr;
            hwndClose_ = nullptr;
            hwndStatus_ = nullptr;
            hwnd_ = nullptr;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

}  // namespace wintcp
