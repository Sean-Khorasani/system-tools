// ChartsWindow.cpp
// See ChartsWindow.h. GDI line charts, double-buffered, DPI-scaled layout.
//
// Fixes live next to the code they address (each carries its
// original defect number in a comment).

#include "ChartsWindow.h"
#include "Alerts.h"   // FormatBps (C1: RateText removed, one rate formatter)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
// ws2tcpip.h must come first: netioapi.h only declares MIB_IF_ROW2 and
// GetIfTable2() when _WS2IPDEF_ (from ws2ipdef.h) is already defined, and
// iphlpapi.h may pull netioapi.h in before we'd get the chance otherwise.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Utils.h"
#include "WinCaps.h"   // DllAvailable: pdh.dll is delay-loaded (see kDelayedDlls)

namespace wintcp {
namespace {

const UINT_PTR kChartsTimerId = 1;
const UINT kChartsIntervalMs = 1000;   // B4: dev constant (with its ChartExport
                                       // twin below): 1 s sampling is the panels'
                                       // contract, not a preference.

// Ticks of the chart loop to wait before re-opening a failed PDH disk query.
// The loop ticks at kChartsIntervalMs, so 10 is ~10 s of silence: long enough
// to ride out a transient PDH failure, short enough that a disk counter gone
// missing for good does not stay dark all session. SysStats.cpp carries the
// same-named constant for its own sampler loop (same rate, same reasoning) —
// the two must stay in step, see the use site below.
const int kPdhReopenRetryTicks = 10;

// IF_TYPE_SOFTWARE_LOOPBACK - numeric to keep the include surface small.
const ULONG kIfTypeSoftwareLoopback = 24;

const COLORREF kLightBg   = RGB(0xFF, 0xFF, 0xFF);
const COLORREF kLightText = RGB(0x20, 0x20, 0x20);
const COLORREF kLightGrid = RGB(0xDD, 0xDD, 0xDD);
const COLORREF kDarkBg    = RGB(0x20, 0x20, 0x20);
const COLORREF kDarkText  = RGB(0xF0, 0xF0, 0xF0);
const COLORREF kDarkGrid  = RGB(0x4A, 0x4A, 0x4A);

const COLORREF kLineA = RGB(0x00, 0x78, 0xD7);   // primary (CPU / R)
const COLORREF kLineB = RGB(0xCA, 0x6E, 0x00);   // secondary (W)

ULONGLONG FtToU64(const FILETIME& ft) {
    ULARGE_INTEGER u = {};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

void PushCapped(std::vector<double>& v, double value, size_t cap) {
    v.push_back(value);
    if (v.size() > cap) v.erase(v.begin(), v.end() - cap);
}

double MaxOf(const std::vector<double>& a, const std::vector<double>& b) {
    double m = 0.0;
    for (double v : a) m = (std::max)(m, v);
    for (double v : b) m = (std::max)(m, v);
    return m;
}

// C1: RateText lived here and did FormatBytes(x) + "/s" — the same job as
// FormatBps (Alerts.h), with a different rounding ("2.0 KB/s" vs "2 KB/s").
// Two same-job formatters is how one surface prints "2.0" and another "2" for
// the same reading. Removed; the three call sites below use FormatBps, whose
// output is pinned by selftest. Visible difference: whole KB/s values lose
// the ".0" on chart captions and the y-axis — same numbers, one spelling.


}  // namespace

const wchar_t* ChartsWindow::kClassName = L"WinTcpChartsWnd";

ChartsWindow::~ChartsWindow() {
    // Close() runs WM_DESTROY synchronously, which already calls
    // DeleteFonts() and CleanupPdh(); the handles are nulled there, so no
    // second cleanup pass is needed (or wanted - it used to double up).
    Close();
}

void ChartsWindow::CenterOnOwner(HWND owner) {
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

void ChartsWindow::Show(HWND owner, bool dark) {
    dark_ = dark;
    if (hwnd_ == nullptr) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &ChartsWindow::WindowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        if (::RegisterClassExW(&wc) == 0 &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return;
        const UINT dpi = QueryDpiForWindow(owner);   // owner's monitor DPI
        hwnd_ = ::CreateWindowExW(
            0, kClassName, L"WinTCP - Performance graphs",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
            CW_USEDEFAULT, ::MulDiv(680, static_cast<int>(dpi), 96),
            ::MulDiv(640, static_cast<int>(dpi), 96), owner, nullptr,
            ::GetModuleHandleW(nullptr), this);
        if (hwnd_ == nullptr) return;
        CenterOnOwner(owner);
    }
    if (::IsWindowVisible(hwnd_) == FALSE) ::ShowWindow(hwnd_, SW_SHOW);
    StartSampling();                     // timer runs only while visible
    ::SetForegroundWindow(hwnd_);
}

void ChartsWindow::Close() {
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        // WM_DESTROY runs synchronously and clears hwnd_.
    }
}

bool ChartsWindow::IsVisible() const {
    return hwnd_ != nullptr && ::IsWindowVisible(hwnd_) == TRUE;
}

void ChartsWindow::SetDark(bool dark) {
    dark_ = dark;
    if (hwnd_ != nullptr) ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void ChartsWindow::StartSampling() {
    if (hwnd_ != nullptr && timerOn_ == 0) {
        timerOn_ = ::SetTimer(hwnd_, kChartsTimerId, kChartsIntervalMs,
                              nullptr);
    }
}

void ChartsWindow::StopSampling() {
    if (timerOn_ != 0 && hwnd_ != nullptr) {
        ::KillTimer(hwnd_, kChartsTimerId);
        timerOn_ = 0;
    }
}

void ChartsWindow::CreateFonts() {
    DeleteFonts();
    const UINT dpi = QueryDpiForWindow(hwnd_);
    LOGFONTW lf = {};
    lf.lfHeight = -::MulDiv(9, static_cast<int>(dpi), 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    ::wcscpy_s(lf.lfFaceName, L"Segoe UI");
    font_ = ::CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    boldFont_ = ::CreateFontIndirectW(&lf);
}

void ChartsWindow::DeleteFonts() {
    if (font_ != nullptr) { ::DeleteObject(font_); font_ = nullptr; }
    if (boldFont_ != nullptr) { ::DeleteObject(boldFont_); boldFont_ = nullptr; }
}

void ChartsWindow::CleanupPdh() {
    if (pdhQuery_ != nullptr) {
        ::PdhCloseQuery(static_cast<HQUERY>(pdhQuery_));
        pdhQuery_ = nullptr;
    }
    pdhRead_ = pdhWrite_ = nullptr;
    pdhFails_ = 0;
}

LRESULT CALLBACK ChartsWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {
    ChartsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<ChartsWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<ChartsWindow*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self != nullptr) return self->HandleMessage(msg, wParam, lParam);
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT ChartsWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            CreateFonts();
            Sample();   // establish baselines immediately (no history yet)
            return 0;
        case WM_TIMER:
            if (wParam == kChartsTimerId) OnTimer();
            return 0;
        case WM_PAINT:
            OnPaint();
            return 0;
        case WM_SIZE:
            ::InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        case WM_GETMINMAXINFO: {
            // Keep all four panels paintable.
            const UINT dpi = QueryDpiForWindow(hwnd_);
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = ::MulDiv(420, static_cast<int>(dpi), 96);
            mmi->ptMinTrackSize.y = ::MulDiv(340, static_cast<int>(dpi), 96);
            return 0;
        }
        case WM_DPICHANGED: {
            // Follow the suggested rect and rebuild DPI-scaled fonts
            //
            CreateFonts();
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            ::InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;   // fully painted in OnPaint (double-buffered)
        case WM_CLOSE:
            // Hide AND stop sampling: a background 1 s timer on a hidden
            // window was pure waste.
            StopSampling();
            ::ShowWindow(hwnd_, SW_HIDE);
            return 0;
        case WM_DESTROY:
            StopSampling();
            CleanupPdh();
            DeleteFonts();
            hwnd_ = nullptr;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void ChartsWindow::OnTimer() {
    Sample();
    if (hwnd_ != nullptr) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---- sampling ---------------------------------------------------------------

void ChartsWindow::Sample() {
    const ULONGLONG nowTick = ::GetTickCount64();

    // --- CPU (system-wide, from GetSystemTimes) ------------------------------
    FILETIME idle = {}, kernel = {}, user = {};
    if (::GetSystemTimes(&idle, &kernel, &user)) {
        bool cpuOk = false;
        if (haveCpuBase_) {
            const ULONGLONG idleD = FtToU64(idle) - FtToU64(prevIdle_);
            const ULONGLONG totalD =
                (FtToU64(kernel) - FtToU64(prevKernel_)) +
                (FtToU64(user) - FtToU64(prevUser_));
            if (totalD > 0) {
                double busy = 1.0 - static_cast<double>(idleD) /
                                        static_cast<double>(totalD);
                if (busy < 0.0) busy = 0.0;
                if (busy > 1.0) busy = 1.0;
                cpuPct_ = busy * 100.0;
                cpuOk = true;
            }
        }
        prevIdle_ = idle;
        prevKernel_ = kernel;
        prevUser_ = user;
        haveCpuBase_ = true;
        if (cpuOk) PushCapped(cpuHist_, cpuPct_, kMaxSamples);
    }

    // --- memory --------------------------------------------------------------
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (::GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0) {
        memTotal_ = ms.ullTotalPhys;
        memUsed_ = ms.ullTotalPhys - ms.ullAvailPhys;
        // Derive the percentage from the same two numbers the caption
        // prints instead of dwMemoryLoad.
        memPct_ = 100.0 * static_cast<double>(memUsed_) /
                  static_cast<double>(memTotal_);
        PushCapped(memHist_, memPct_, kMaxSamples);
    }

    // --- disk (PDH PhysicalDisk _Total, best effort) -------------------------
    //
    // DllAvailable gate: pdh.dll is delay-loaded, so a machine without it would
    // raise a delay-load exception on the first ::PdhOpenQueryW below instead
    // of reporting "counters unavailable" - which is what diskAvailable_=false
    // renders. Same pattern as the SysStats.cpp sampler.
    if (pdhQuery_ == nullptr && DllAvailable("pdh.dll")) {
        if (pdhRetryIn_ <= 0) {
            HQUERY q = nullptr;
            HCOUNTER rd = nullptr, wr = nullptr;
            // PdhAddEnglishCounterW: locale-independent counter path
            //
            if (::PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS &&
                ::PdhAddEnglishCounterW(
                    q, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",
                    0, &rd) == ERROR_SUCCESS &&
                ::PdhAddEnglishCounterW(
                    q, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",
                    0, &wr) == ERROR_SUCCESS &&
                ::PdhCollectQueryData(q) == ERROR_SUCCESS) {
                pdhQuery_ = q;
                pdhRead_ = rd;
                pdhWrite_ = wr;
                pdhFails_ = 0;
                diskAvailable_ = true;
            } else {
                if (q != nullptr) ::PdhCloseQuery(q);
                diskAvailable_ = false;
                // Same backoff as SysStats.cpp's disk-counter reopen (which see):
                // both loops tick at ~1 s, so 10 ticks is ~10 s of silence
                // before PDH is asked again. Kept as two same-named constants
                // rather than one shared home on purpose — the members belong
                // to different classes with different tick drivers, and a
                // shared header would couple them falsely. If either tick rate
                // stops being ~1 s, both constants must be revisited together.
                pdhRetryIn_ = kPdhReopenRetryTicks;
            }
        } else {
            --pdhRetryIn_;
        }
    }
    if (pdhQuery_ != nullptr) {
        bool ok = ::PdhCollectQueryData(static_cast<HQUERY>(pdhQuery_)) ==
                  ERROR_SUCCESS;
        PDH_FMT_COUNTERVALUE fv = {};
        if (ok && ::PdhGetFormattedCounterValue(
                       static_cast<HCOUNTER>(pdhRead_), PDH_FMT_DOUBLE,
                       nullptr, &fv) == ERROR_SUCCESS &&
            fv.CStatus == ERROR_SUCCESS) {
            diskReadBps_ = (fv.doubleValue < 0.0) ? 0.0 : fv.doubleValue;
        } else {
            ok = false;
        }
        if (ok && ::PdhGetFormattedCounterValue(
                       static_cast<HCOUNTER>(pdhWrite_), PDH_FMT_DOUBLE,
                       nullptr, &fv) == ERROR_SUCCESS &&
            fv.CStatus == ERROR_SUCCESS) {
            diskWriteBps_ = (fv.doubleValue < 0.0) ? 0.0 : fv.doubleValue;
        } else {
            ok = false;
        }
        if (ok) {
            pdhFails_ = 0;
            diskAvailable_ = true;
            PushCapped(diskReadHist_, diskReadBps_, kMaxSamples);
            PushCapped(diskWriteHist_, diskWriteBps_, kMaxSamples);
        } else if (++pdhFails_ >= 3) {
            // Three consecutive failures = counters really went away;
            // transient hiccups are tolerated.
            CleanupPdh();
            diskAvailable_ = false;
            pdhRetryIn_ = kPdhReopenRetryTicks;
            diskReadHist_.clear();    // panel switches to "n/a", no stale line
            diskWriteHist_.clear();
        }
    }

    // --- network (sum of interface octets, 64-bit via GetIfTable2) -----------
    PMIB_IF_TABLE2 ifTable = nullptr;
    if (::GetIfTable2(&ifTable) == NO_ERROR && ifTable != nullptr) {
        ULONGLONG inOctets = 0;
        ULONGLONG outOctets = 0;
        for (ULONG i = 0; i < ifTable->NumEntries; ++i) {
            const MIB_IF_ROW2& row = ifTable->Table[i];
            if (row.Type == kIfTypeSoftwareLoopback) continue;
            // UINT64 counters: no 4 GiB wrap.
            inOctets += row.InOctets;
            outOctets += row.OutOctets;
        }
        ::FreeMibTable(ifTable);

        bool netOk = false;
        if (haveNetBase_ && nowTick > prevNetTick_) {
            const double secs =
                static_cast<double>(nowTick - prevNetTick_) / 1000.0;
            if (secs > 0.0) {
                const ULONGLONG inD = inOctets - prevNetIn_;
                const ULONGLONG outD = outOctets - prevNetOut_;
                // Guard against table resets (interface replug shows up
                // as a huge spike): clamp to 100 Gbit/s per direction.
                const double cap = 100.0e9 / 8.0 * secs;
                netRecvBps_ = (static_cast<double>(inD) > cap)
                                  ? 0.0
                                  : static_cast<double>(inD) / secs;
                netSendBps_ = (static_cast<double>(outD) > cap)
                                  ? 0.0
                                  : static_cast<double>(outD) / secs;
                netOk = true;
            }
        }
        prevNetIn_ = inOctets;
        prevNetOut_ = outOctets;
        prevNetTick_ = nowTick;
        haveNetBase_ = true;
        if (netOk) {
            PushCapped(netRecvHist_, netRecvBps_, kMaxSamples);
            PushCapped(netSendHist_, netSendBps_, kMaxSamples);
        }
    }
}

// ---- painting ---------------------------------------------------------------

void ChartsWindow::OnPaint() {
    PAINTSTRUCT ps = {};
    HDC hdc = ::BeginPaint(hwnd_, &ps);
    RECT rc = {};
    ::GetClientRect(hwnd_, &rc);

    const UINT dpi = QueryDpiForWindow(hwnd_);
    const auto px = [dpi](int v) {
        return ::MulDiv(v, static_cast<int>(dpi), 96);
    };

    // Double buffer (avoids flicker at 1 Hz repaints).
    HDC mem = ::CreateCompatibleDC(hdc);
    const int bmpW = (std::max)(rc.right, 1L);
    const int bmpH = (std::max)(rc.bottom, 1L);
    HBITMAP bmp = ::CreateCompatibleBitmap(hdc, bmpW, bmpH);
    HGDIOBJ oldBmp = ::SelectObject(mem, bmp);
    const COLORREF bg = dark_ ? kDarkBg : kLightBg;
    const COLORREF fg = dark_ ? kDarkText : kLightText;
    const COLORREF grid = dark_ ? kDarkGrid : kLightGrid;
    HBRUSH bgBrush = ::CreateSolidBrush(bg);
    ::FillRect(mem, &rc, bgBrush);
    ::DeleteObject(bgBrush);

    const int pad = px(10);
    const int gap = px(8);
    const int panels = 4;
    const int panelH =
        (rc.bottom - pad * 2 - gap * (panels - 1)) / panels;

    struct Panel {
        const wchar_t* name;
        const std::vector<double>* a;
        const std::vector<double>* b;   // optional second line
        std::wstring caption;           // current value(s); empty = none yet
        double fixedMax;                // > 0 = fixed 0..fixedMax scale
        const wchar_t* emptyMsg;        // override for "collecting…"
    };

    // Captions exist only once their source produced a real reading
    // no fabricated "0.0 %" before the first delta.
    std::wstring cpuCap, memCap, diskCap, netCap;
    wchar_t buf[160] = {0};
    if (!cpuHist_.empty()) {
        ::swprintf_s(buf, L"%.1f %%", cpuPct_);
        cpuCap = buf;
    }
    if (!memHist_.empty()) {
        ::swprintf_s(buf, L"%.0f %%  (%s / %s)", memPct_,
                     FormatBytes(memUsed_).c_str(),
                     FormatBytes(memTotal_).c_str());
        memCap = buf;
    }
    if (diskAvailable_) {
        if (!diskReadHist_.empty()) {
            ::swprintf_s(buf, L"R %s   W %s", FormatBps(diskReadBps_).c_str(),
                         FormatBps(diskWriteBps_).c_str());
            diskCap = buf;
        }
    } else {
        diskCap = L"counters unavailable";
    }
    if (!netRecvHist_.empty()) {
        ::swprintf_s(buf, L"down %s   up %s", FormatBps(netRecvBps_).c_str(),
                     FormatBps(netSendBps_).c_str());
        netCap = buf;
    }

    const Panel defs[4] = {
        {L"CPU", &cpuHist_, nullptr, cpuCap, 100.0, nullptr},
        {L"Memory", &memHist_, nullptr, memCap, 100.0, nullptr},
        {L"Disk", &diskReadHist_, &diskWriteHist_, diskCap, 0.0,
         diskAvailable_ ? nullptr : L"n/a \u2014 performance counters unavailable"},
        {L"Network", &netRecvHist_, &netSendHist_, netCap, 0.0, nullptr},
    };

    HFONT oldFont = static_cast<HFONT>(
        ::SelectObject(mem, (boldFont_ != nullptr) ? boldFont_
                                                   : GetStockObject(
                                                         DEFAULT_GUI_FONT)));
    ::SetBkMode(mem, TRANSPARENT);
    ::SetTextColor(mem, fg);

    const int titleH = px(20);
    const COLORREF dim =
        dark_ ? RGB(0x9A, 0x9A, 0x9A) : RGB(0x66, 0x66, 0x66);
    for (int p = 0; p < panels; ++p) {
        const Panel& d = defs[p];
        const int top = pad + p * (panelH + gap);
        const RECT chart = {pad, top + titleH, rc.right - pad, top + panelH};

        // Caption line: name left, value(s) appended (no trailing spaces
        // when there is no value yet); ellipsize instead of overflowing.
        std::wstring line = d.name;
        if (!d.caption.empty()) {
            line += L"   ";
            line += d.caption;
        }
        RECT titleRc = {pad, top, rc.right - pad, top + titleH};
        ::DrawTextW(mem, line.c_str(), -1, &titleRc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                        DT_END_ELLIPSIS);

        // Border + horizontal gridlines at 25/50/75 %.
        HPEN gridPen = ::CreatePen(PS_SOLID, 1, grid);
        HGDIOBJ oldPen = ::SelectObject(mem, gridPen);
        ::MoveToEx(mem, chart.left, chart.top, nullptr);
        ::LineTo(mem, chart.right, chart.top);
        for (int g = 1; g <= 3; ++g) {
            const int y = chart.top + (chart.bottom - chart.top) * g / 4;
            ::MoveToEx(mem, chart.left, y, nullptr);
            ::LineTo(mem, chart.right, y);
        }
        ::MoveToEx(mem, chart.left, chart.bottom - 1, nullptr);
        ::LineTo(mem, chart.right, chart.bottom - 1);
        ::MoveToEx(mem, chart.left, chart.top, nullptr);
        ::LineTo(mem, chart.left, chart.bottom);
        ::MoveToEx(mem, chart.right, chart.top, nullptr);
        ::LineTo(mem, chart.right, chart.bottom);

        if (d.a->empty()) {
            // Per-panel status: one unavailable source no
            // longer freezes the other three behind "collecting…".
            const wchar_t* msg =
                (d.emptyMsg != nullptr) ? d.emptyMsg : L"collecting\u2026";
            RECT empty = chart;
            ::DrawTextW(mem, msg, -1, &empty,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                            DT_END_ELLIPSIS);
        } else {
            double yMax = d.fixedMax;
            if (yMax <= 0.0) {
                yMax = MaxOf(*d.a, (d.b != nullptr) ? *d.b : *d.a) * 1.2;
                if (yMax < 64.0 * 1024.0) yMax = 64.0 * 1024.0;
            }
            // Scale label (top-right inside the chart).
            wchar_t scale[64] = {0};
            if (d.fixedMax > 0.0)
                ::swprintf_s(scale, L"%.0f %%", yMax);
            else
                ::swprintf_s(scale, L"%s", FormatBps(yMax).c_str());
            RECT scaleRc = {chart.left, chart.top + px(2), chart.right - px(4),
                            chart.top + titleH};
            ::SetTextColor(mem, dim);
            ::DrawTextW(mem, scale, -1, &scaleRc,
                        DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
            ::SetTextColor(mem, fg);

            const auto plot = [&](const std::vector<double>& h,
                                  COLORREF color) {
                if (h.size() < 2) return;
                HPEN pen = ::CreatePen(PS_SOLID, px(2) < 2 ? 2 : px(2), color);
                HGDIOBJ old = ::SelectObject(mem, pen);
                const size_t n = h.size();
                const int w = chart.right - chart.left;
                const int hh = chart.bottom - chart.top;
                // Stretch over the samples we actually have: scaling by
                // kMaxSamples squashed the whole line against the right
                // edge until the buffer filled after 2 minutes.
                const double denom =
                    (n > 1) ? static_cast<double>(n - 1) : 1.0;
                for (size_t i = 0; i < n; ++i) {
                    // Oldest sample at the left edge, newest at the right.
                    const int x = chart.right - static_cast<int>(
                      (static_cast<double>(n - 1 - i) / denom) * w);
                    double v = h[i];
                    if (v < 0.0) v = 0.0;
                    if (v > yMax) v = yMax;
                    const int y =
                        chart.bottom - 1 -
                        static_cast<int>(v / yMax * (hh - 1));
                    if (i == 0)
                        ::MoveToEx(mem, x, y, nullptr);
                    else
                        ::LineTo(mem, x, y);
                }
                ::SelectObject(mem, old);
                ::DeleteObject(pen);
            };
            plot(*d.a, kLineA);
            if (d.b != nullptr) plot(*d.b, kLineB);
        }
        ::SelectObject(mem, oldPen);
        ::DeleteObject(gridPen);
    }

    ::SelectObject(mem, oldFont);
    ::BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    ::SelectObject(mem, oldBmp);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::EndPaint(hwnd_, &ps);
}

}  // namespace wintcp
