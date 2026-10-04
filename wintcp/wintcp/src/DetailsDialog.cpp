// DetailsDialog.cpp
// Details window: registered class, a button row, and an owner-drawn body.
// No dialog template, matching the rest of the codebase's zero-dependency
// style (see DetailsDialog.h for why the body is custom-drawn).

#include "DetailsDialog.h"

#include <shellapi.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include <algorithm>
#include <cstring>

#include "Utils.h"

namespace wintcp {
namespace {

// Control ids are local to this window, so they cannot collide with the
// main window's control ids.
const UINT IDC_DLG_COPY  = 1;
const UINT IDC_DLG_OPEN  = 2;
const UINT IDC_DLG_CLOSE = 3;

// Logical (96 dpi) layout metrics. The renderer works in these units and
// scales once via S(), so a DPI change is one pass over the line list rather
// than a recomputation of every measurement.
constexpr int kMargin = 10;
constexpr int kBtnH = 26;
constexpr int kBtnWCpy = 90;
constexpr int kBtnWOpen = 190;
constexpr int kBtnWClose = 70;
constexpr int kTitleH = 26;         // process name
constexpr int kSubtitleH = 18;      // PID + endpoints
constexpr int kLineH = 17;          // one field row
constexpr int kSectionH = 24;       // section header band
constexpr int kNoteH = 16;          // dim note under a section
constexpr int kLabelGutter = 132;   // label column width
constexpr int kScrollBarW = 14;

// Window geometry, authored unscaled and multiplied by the DPI ratio (S/MulDiv).
// The design size appeared TWICE - once in the CreateWindowExW call and once in
// the WM_CREATE handler - and the minimum size is a separate pair that must be
// smaller than the design size or the window can never be opened at any DPI.
// Naming all four makes those two relationships checkable.
constexpr int kDesignWidth = 560;
constexpr int kDesignHeight = 520;
constexpr int kMinWidth = 420;
constexpr int kMinHeight = 300;
// The DPI a design pixel is 1:1 at, and the points->pixels divisor. Same two
// numbers as MainWindow.cpp has; not shared because this window scales its own
// layout and coupling the two would tie one window's geometry to the other's.
constexpr int kDpiUnscaled = 96;
constexpr int kPointsPerInch = 72;
constexpr int kBaseFontPt = 9;
// A colour-mix weight, in percent: 10 is a faint tint, 20 a readable one.
constexpr int kTintFaint = 10;
constexpr int kTintBody = 20;
// Indent for a field line and for the dim note under a section, in design px.
// Both sites had the same bare 12: the two are visually the same step, and a
// note that did not line up with its heading read as a layout bug.
constexpr int kIndentPx = 12;
// The scrollbar-thumb button at the right of a section row, and its width.
constexpr int kScrollThumbW = 24;

// Dark-theme substitutes. Everything else comes from GetSysColor, so a
// high-contrast scheme wins over these automatically.
const COLORREF kDarkBg = RGB(0x20, 0x20, 0x20);
const COLORREF kDarkText = RGB(0xF0, 0xF0, 0xF0);
const COLORREF kDarkDim = RGB(0x9A, 0x9A, 0x9A);
const COLORREF kDarkHeader = RGB(0xFF, 0xFF, 0xFF);

// Mix two colours; t=0 -> a, t=255 -> b. Used to derive the alternating row
// shade from the *current* window colour, so the two always belong to the
// same theme instead of being two hard-coded greys.
COLORREF Mix(COLORREF a, COLORREF b, int t) {
    const int r = (GetRValue(a) * (255 - t) + GetRValue(b) * t) / 255;
    const int g = (GetGValue(a) * (255 - t) + GetGValue(b) * t) / 255;
    const int bl = (GetBValue(a) * (255 - t) + GetBValue(b) * t) / 255;
    return RGB(r, g, bl);
}

void SetChildFont(HWND hwnd, HFONT font) {
    if (font != nullptr && hwnd != nullptr) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                       MAKELPARAM(TRUE, 0));
    }
}

}  // namespace

const wchar_t* DetailsDialog::kClassName = L"WinTcpDetailsWnd";

DetailsDialog::~DetailsDialog() {
    Close();
    if (headerFont_ != nullptr) ::DeleteObject(headerFont_);
    if (monoFont_ != nullptr) ::DeleteObject(monoFont_);
    if (smallFont_ != nullptr) ::DeleteObject(smallFont_);
    if (brushWindow_ != nullptr) ::DeleteObject(brushWindow_);
    if (brushAlternate_ != nullptr) ::DeleteObject(brushAlternate_);
    if (brushSection_ != nullptr) ::DeleteObject(brushSection_);
}

int DetailsDialog::S(int px96) const {
    const UINT dpi = QueryDpiForWindow(hwnd_);
    return ::MulDiv(px96, static_cast<int>(dpi), kDpiUnscaled);
}

void DetailsDialog::ApplyColors() {
    const auto drop = [](HBRUSH& b) {
        if (b != nullptr) { ::DeleteObject(b); b = nullptr; }
    };
    drop(brushWindow_);
    drop(brushAlternate_);
    drop(brushSection_);

    // Always derive from the live system colors. Under high contrast these
    // are the scheme's own values, which is exactly what we want; in a
    // normal theme they are the light palette and the dark branch below
    // substitutes its own.
    COLORREF bg = ::GetSysColor(COLOR_WINDOW);
    COLORREF fg = ::GetSysColor(COLOR_WINDOWTEXT);
    COLORREF dim = ::GetSysColor(COLOR_GRAYTEXT);
    COLORREF header = fg;
    if (dark_) {
        bg = kDarkBg;
        fg = kDarkText;
        dim = kDarkDim;
        header = kDarkHeader;
    }
    clrText_ = fg;
    clrDim_ = dim;
    clrHeader_ = header;
    brushWindow_ = ::CreateSolidBrush(bg);
    // A small shift is enough to read as a stripe without looking like a
    // different theme, and deriving it makes it work in both palettes.
    brushAlternate_ = ::CreateSolidBrush(Mix(bg, fg, kTintFaint));
    brushSection_ = ::CreateSolidBrush(Mix(bg, fg, kTintBody));
}

void DetailsDialog::CenterOnOwner(HWND owner) {
    if (hwnd_ == nullptr) return;
    RECT rcOwner = {};
    if (owner != nullptr && ::GetWindowRect(owner, &rcOwner)) {
        // Center over the parent window, then clamp into the
        // monitor work area so it never strands off-screen.
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

void DetailsDialog::Show(HWND owner, HFONT font, const DetailModel& model,
                         const std::wstring& filePath) {
    model_ = model;
    filePath_ = filePath;
    if (font_ != font) ApplyFont(font);

    const std::wstring caption =
        L"WinTCP - Details - " +
        (model_.title.empty() ? std::wstring(L"(none)") : model_.title);
    if (hwnd_ == nullptr) {
        // Idempotent registration (ERROR_CLASS_ALREADY_EXISTS is fine).
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &DetailsDialog::WindowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;   // fully painted in OnPaint
        wc.lpszClassName = kClassName;
        if (::RegisterClassExW(&wc) == 0 &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return;

        const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        hwnd_ = ::CreateWindowExW(0, kClassName, caption.c_str(), style,
                                  CW_USEDEFAULT, CW_USEDEFAULT, kDesignWidth, kDesignHeight,
                                  owner, nullptr, ::GetModuleHandleW(nullptr),
                                  this);
        if (hwnd_ == nullptr) return;
        // Scale the default size to this window's DPI so the dialog is not
        // physically small on 125/150% displays.
        const UINT dpi = QueryDpiForWindow(hwnd_);
        ::SetWindowPos(hwnd_, nullptr, 0, 0,
                       ::MulDiv(kDesignWidth, static_cast<int>(dpi), kDpiUnscaled),
                       ::MulDiv(kDesignHeight, static_cast<int>(dpi), kDpiUnscaled),
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        CenterOnOwner(owner);
    } else {
        ::SetWindowTextW(hwnd_, caption.c_str());
    }

    if (hwndOpen_ != nullptr)
        ::EnableWindow(hwndOpen_, filePath_.empty() ? FALSE : TRUE);

    // A new model can have a different number of sections; keep the
    // collapse vector in step rather than indexing past its end.
    collapsed_.assign(model_.sections.size(), 0);
    RebuildLayout();

    // Owned window: it hides with the owner and stays on top of it.
    if (::IsWindowVisible(hwnd_) == FALSE) ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void DetailsDialog::UpdateModel(const DetailModel& model) {
    // Silent refresh while auto-refresh runs: no show, no foreground, no
    // focus steal. No-op when the window was closed meanwhile.
    if (hwnd_ == nullptr) return;
    model_ = model;
    if (collapsed_.size() != model_.sections.size())
        collapsed_.assign(model_.sections.size(), 0);
    // RebuildLayout() clamps scrollPos_ to the new content height, so a
    // live refresh that shrinks the list cannot leave the view scrolled
    // past the end.
    RebuildLayout();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void DetailsDialog::SetDark(bool dark) {
    if (dark_ == dark) return;
    dark_ = dark;
    ApplyColors();
    if (hwnd_ != nullptr) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void DetailsDialog::Close() {
    if (hwnd_ != nullptr && ::IsWindow(hwnd_)) ::DestroyWindow(hwnd_);
}

bool DetailsDialog::IsVisible() const {
    return hwnd_ != nullptr && ::IsWindowVisible(hwnd_) == TRUE;
}

void DetailsDialog::ApplyFont(HFONT font) {
    font_ = font;   // borrowed from MainWindow; never deleted here
    if (font_ == nullptr) return;
    if (hwnd_ != nullptr && !fontsReady_) {
        // Derived faces. headerFont_ is the ~125% bold used for the title
        // and section bands; smallFont_ carries the dim notes; monoFont_
        // keeps digits aligned in values that are numbers.
        LOGFONTW lf = {};
        if (::GetObjectW(font_, sizeof(lf), &lf) != sizeof(lf)) return;
        const LONG base = lf.lfHeight;

        lf.lfWeight = FW_BOLD;
        lf.lfHeight = (base * 5) / 4;
        if (lf.lfHeight == 0) lf.lfHeight = -16;   // legacy fixed-pixel form
        if (headerFont_ != nullptr) ::DeleteObject(headerFont_);
        headerFont_ = ::CreateFontIndirectW(&lf);

        lf.lfWeight = FW_NORMAL;
        lf.lfHeight = base;
        if (monoFont_ != nullptr) ::DeleteObject(monoFont_);
        ::wcscpy_s(lf.lfFaceName, L"Consolas");
        monoFont_ = ::CreateFontIndirectW(&lf);

        lf.lfHeight = (base * 8) / 9;   // ~89% of the UI face
        if (smallFont_ != nullptr) ::DeleteObject(smallFont_);
        ::wcscpy_s(lf.lfFaceName, L"Segoe UI");
        smallFont_ = ::CreateFontIndirectW(&lf);
        fontsReady_ = true;

        ApplyColors();
    }
    SetChildFont(hwndCopy_, font_);
    SetChildFont(hwndOpen_, font_);
    SetChildFont(hwndClose_, font_);
    if (hwnd_ != nullptr) {
        RECT rc = {};
        ::GetClientRect(hwnd_, &rc);
        Layout(rc.right, rc.bottom);
        RebuildLayout();
    }
}

int DetailsDialog::MaxScroll() const {
    const int over = totalContentHeight_ - viewHeight_;
    return (over > 0) ? over : 0;
}

void DetailsDialog::ScrollBy(int lines) {
    // Extra parens: windows.h defines min/max as macros, so an unqualified
    // std::min/std::max would expand to ((a) < (b) ? ...).
    const int delta = lines * kLineH;
    int next = scrollPos_ + delta;
    next = (std::min)(next, MaxScroll());
    if (next < 0) next = 0;
    if (next == scrollPos_) return;
    scrollPos_ = next;
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void DetailsDialog::RebuildLayout() {
    // The model is turned into a flat list of physical lines once per
    // change; painting is then a straight run of DrawText calls. Line
    // heights are the fixed logical metrics above, so this needs no DC.
    lines_.clear();
    lines_.reserve(64);   // ~4 screens of fields
    totalContentHeight_ = 0;

    const auto push = [this](const Line& l) {
        lines_.push_back(l);
        totalContentHeight_ += l.height;
    };

    if (!model_.title.empty()) {
        Line l;
        l.kind = Line::SectionHeader;
        l.text = model_.title;
        l.height = kTitleH;
        l.section = -1;
        push(l);
    }
    if (!model_.subtitle.empty()) {
        Line l;
        l.kind = Line::FieldValue;
        l.text = model_.subtitle;
        l.height = kSubtitleH;
        l.indent = 2;
        l.section = -1;
        push(l);
    }
    if (!model_.title.empty() || !model_.subtitle.empty()) {
        Line l;
        l.kind = Line::Blank;
        l.height = kMargin;
        l.section = -1;
        push(l);
    }

    for (size_t s = 0; s < model_.sections.size(); ++s) {
        const DetailSection& sec = model_.sections[s];
        Line h;
        h.kind = Line::SectionHeader;
        h.text = sec.title;
        h.height = kSectionH;
        h.section = static_cast<int>(s);
        push(h);

        const bool isCollapsed =
            (s < collapsed_.size() && collapsed_[s] != 0);
        if (isCollapsed) continue;

        for (const DetailField& f : sec.fields) {
            Line l;
            l.kind = Line::FieldLabel;
            l.text = f.label;
            l.height = kLineH;
            l.section = static_cast<int>(s);
            push(l);

            Line v;
            v.kind = Line::FieldValue;
            v.text = f.value;
            v.monospace = f.monospace;
            v.section = static_cast<int>(s);
            v.indent = kLabelGutter;
            v.height = kLineH;
            push(v);
        }
        if (!sec.note.empty()) {
            Line n;
            n.kind = Line::SectionNote;
            n.text = sec.note;
            n.height = kNoteH;
            n.indent = kLabelGutter;
            n.section = static_cast<int>(s);
            push(n);
        }
    }

    if (!model_.connectionLines.empty()) {
        Line h;
        h.kind = Line::SectionHeader;
        h.text = L"Connections (" + std::to_wstring(model_.connectionTotal) +
                 L")";
        h.height = kSectionH;
        h.section = -1;
        push(h);
        for (const std::wstring& c : model_.connectionLines) {
            Line l;
            l.kind = Line::Connection;
            l.text = c;
            l.monospace = true;
            l.height = kLineH;
            l.indent = kIndentPx;
            l.section = -1;
            push(l);
        }
        if (model_.connectionTotal > model_.connectionLines.size()) {
            Line n;
            n.kind = Line::SectionNote;
            n.text = L"… and " +
                     std::to_wstring(model_.connectionTotal -
                                     model_.connectionLines.size()) +
                     L" more";
            n.height = kNoteH;
            n.indent = kIndentPx;
            n.section = -1;
            push(n);
        }
    }
    const int maxScroll = MaxScroll();
    if (scrollPos_ > maxScroll) scrollPos_ = maxScroll;
}

void DetailsDialog::Layout(int cx, int cy) {
    if (hwnd_ == nullptr) return;
    const int m = S(kMargin);
    const int btnH = S(kBtnH);
    const int btnY = cy - m - btnH;
    const int btnRowH = btnH + m;

    // The body is painted, not a child, so it only needs its height: the
    // area between the top margin and the button row.
    const int top = m;
    viewHeight_ = cy - btnRowH - top - m;
    if (viewHeight_ < 0) viewHeight_ = 0;

    if (hwndCopy_ != nullptr)
        ::MoveWindow(hwndCopy_, m, btnY, S(kBtnWCpy), btnH, TRUE);
    if (hwndOpen_ != nullptr)
        ::MoveWindow(hwndOpen_, m + S(kBtnWCpy) + m, btnY, S(kBtnWOpen), btnH,
                     TRUE);
    if (hwndClose_ != nullptr)
        ::MoveWindow(hwndClose_, cx - m - S(kBtnWClose), btnY,
                     S(kBtnWClose), btnH, TRUE);
}

void DetailsDialog::OnPaint() {
    PAINTSTRUCT ps = {};
    HDC hdc = ::BeginPaint(hwnd_, &ps);
    if (hdc == nullptr) return;

    RECT rc = {};
    ::GetClientRect(hwnd_, &rc);
    const int cx = rc.right, cy = rc.bottom;

    // Double-buffer: the body is a long run of DrawText calls, and doing
    // them straight to the window DC flickers on any redraw.
    HDC mem = ::CreateCompatibleDC(hdc);
    HBITMAP bmp = ::CreateCompatibleBitmap(hdc, cx, cy);
    HGDIOBJ oldBmp = (bmp != nullptr) ? ::SelectObject(mem, bmp) : nullptr;
    if (brushWindow_ != nullptr) ::FillRect(mem, &rc, brushWindow_);

    ::SetBkMode(mem, TRANSPARENT);

    const int m = S(kMargin);
    int y = m - scrollPos_;
    int fieldInSection = 0;

    for (const Line& l : lines_) {
        const int lh = S(l.height);
        if (l.kind == Line::FieldLabel) ++fieldInSection;
        if (y + lh >= 0 && y <= cy) {
            const HFONT face =
                (l.kind == Line::SectionHeader) ? headerFont_
                : (l.kind == Line::SectionNote)   ? smallFont_
                : l.monospace                     ? monoFont_
                                                  : font_;
            if (face != nullptr) ::SelectObject(mem, face);

            // Alternating shading on field rows only, restarting per
            // section, and a band behind every section header.
            if (l.kind == Line::FieldLabel) {
                if ((fieldInSection % 2) == 0 && brushAlternate_ != nullptr) {
                    RECT band = {0, y, cx, y + lh};
                    ::FillRect(mem, &band, brushAlternate_);
                }
            } else if (l.kind == Line::SectionHeader &&
                       brushSection_ != nullptr) {
                RECT band = {0, y, cx, y + lh};
                ::FillRect(mem, &band, brushSection_);
                // Accent bar for the section under the pointer, so the
                // collapse affordance is discoverable without a tooltip.
                if (l.section >= 0 && l.section == hotSection_) {
                    RECT bar = {0, y, S(3), y + lh};
                    ::FillRect(mem, &bar, brushSection_);
                    ::SetBkMode(mem, OPAQUE);
                    ::SetBkColor(mem, clrHeader_);
                    RECT dot = {S(kMargin - S(2)), y + lh / 2 - S(kNoteH / 2), S(kScrollThumbW / 2),
                                y + lh / 2 + S(2)};
                    ::FillRect(mem, &dot, brushSection_);
                    ::SetBkMode(mem, TRANSPARENT);
                }
            }

            ::SetTextColor(mem,
                           (l.kind == Line::SectionNote)     ? clrDim_
                           : (l.kind == Line::SectionHeader) ? clrHeader_
                                                             : clrText_);

            RECT draw = {m + S(l.indent), y, cx - m, y + lh};
            // DT_END_ELLIPSIS is a last-resort guard: the value column is
            // wide, and a visibly shortened value beats a silent cut.
            ::DrawTextW(mem, l.text.c_str(), static_cast<int>(l.text.size()),
                        &draw,
                        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
                            DT_END_ELLIPSIS);
        }
        y += lh;
    }

    // A scrollbar, drawn by hand: the body is not a child control, so there
    // is nothing to attach a real scrollbar to.
    const int maxScroll = MaxScroll();
    if (maxScroll > 0 && viewHeight_ > 0) {
        const int trackX = cx - S(kScrollBarW) - m;
        const int trackH = viewHeight_;
        RECT track = {trackX, m, cx - m, m + trackH};
        if (brushAlternate_ != nullptr)
            ::FillRect(mem, &track, brushAlternate_);
        const int thumbH = (std::max)(
            kScrollThumbW,
            trackH * viewHeight_ / (std::max)(1, totalContentHeight_));
        const int thumbY = m + (trackH - thumbH) * scrollPos_ / maxScroll;
        RECT thumb = {trackX, thumbY, cx - m, thumbY + thumbH};
        if (brushSection_ != nullptr) ::FillRect(mem, &thumb, brushSection_);
    }

    ::BitBlt(hdc, 0, 0, cx, cy, mem, 0, 0, SRCCOPY);
    if (oldBmp != nullptr) ::SelectObject(mem, oldBmp);
    if (bmp != nullptr) ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::EndPaint(hwnd_, &ps);
}

void DetailsDialog::OnMouseDown(int x, int y) {
    // Only section headers are interactive; a click elsewhere is left
    // alone so the window still behaves like a static read-out.
    const int m = S(kMargin);
    int ly = m - scrollPos_;
    for (const Line& l : lines_) {
        const int lh = S(l.height);
        if (y >= ly && y < ly + lh) {
            if (l.kind == Line::SectionHeader && l.section >= 0 &&
                static_cast<size_t>(l.section) < collapsed_.size()) {
                collapsed_[static_cast<size_t>(l.section)] ^= 1;
                RebuildLayout();
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        ly += lh;
    }
    (void)x;
}

void DetailsDialog::CopyTextToClipboard() {
    const std::wstring text = model_.ToPlainText();
    if (text.empty()) return;
    if (!::OpenClipboard(hwnd_)) return;   // another app holds it
    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem != nullptr) {
        void* dst = ::GlobalLock(hMem);
        if (dst != nullptr) {
            ::memcpy(dst, text.c_str(), bytes);
            ::GlobalUnlock(hMem);
            if (::SetClipboardData(CF_UNICODETEXT, hMem) == nullptr)
                ::GlobalFree(hMem);
            hMem = nullptr;
        }
        if (hMem != nullptr) ::GlobalFree(hMem);
    }
    ::CloseClipboard();
}

void DetailsDialog::OnCommand(WORD id) {
    switch (id) {
        case IDC_DLG_COPY:
            CopyTextToClipboard();
            break;
        case IDC_DLG_OPEN: {
            if (filePath_.empty()) break;
            // Show the file in Explorer, pre-selected.
            const std::wstring arg = L"/select,\"" + filePath_ + L"\"";
            ::ShellExecuteW(hwnd_, L"open", L"explorer.exe", arg.c_str(),
                            nullptr, SW_SHOWNORMAL);
            break;
        }
        case IDC_DLG_CLOSE:
            ::ShowWindow(hwnd_, SW_HIDE);
            break;
        default:
            break;
    }
}

LRESULT CALLBACK DetailsDialog::WindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                           LPARAM lParam) {
    auto* self = reinterpret_cast<DetailsDialog*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<DetailsDialog*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) self->hwnd_ = hwnd;
    }
    if (self != nullptr) return self->HandleMessage(msg, wParam, lParam);
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT DetailsDialog::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            const HINSTANCE inst = ::GetModuleHandleW(nullptr);
            hwndCopy_ = ::CreateWindowExW(
                0, L"BUTTON", L"&Copy", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 0, 0, hwnd_,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_DLG_COPY)),
                inst, nullptr);
            hwndOpen_ = ::CreateWindowExW(
                0, L"BUTTON", L"&Open file location",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED,
                0, 0, 0, 0, hwnd_,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_DLG_OPEN)),
                inst, nullptr);
            hwndClose_ = ::CreateWindowExW(
                0, L"BUTTON", L"C&lose",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                0, 0, 0, 0, hwnd_,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_DLG_CLOSE)),
                inst, nullptr);
            ApplyFont(font_);   // derived faces + palette + first layout
            RebuildLayout();
            return 0;
        }
        case WM_SIZE:
            Layout(LOWORD(lParam), HIWORD(lParam));
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            // Never let the window shrink into a state where Layout() would
            // compute a negative view height.
            const UINT dpi = QueryDpiForWindow(hwnd_);
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = ::MulDiv(kMinWidth, static_cast<int>(dpi), kDpiUnscaled);
            mmi->ptMinTrackSize.y = ::MulDiv(kMinHeight, static_cast<int>(dpi), kDpiUnscaled);
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            // The derived faces are relative to font_, so rebuild them and
            // re-measure: moving to another monitor can change the scale.
            fontsReady_ = false;
            ApplyFont(font_);
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_COMMAND:
            // BN_CLICKED only: without the notification check an
            // enable/disable notification would also trigger the command.
            if (HIWORD(wParam) == BN_CLICKED) OnCommand(LOWORD(wParam));
            return 0;
        case WM_CLOSE:
            ::ShowWindow(hwnd_, SW_HIDE);   // keep it around for reuse
            return 0;
        case WM_DESTROY:
            hwnd_ = nullptr;
            hwndCopy_ = hwndOpen_ = hwndClose_ = nullptr;
            fontsReady_ = false;
            return 0;
        case WM_MOUSEWHEEL: {
            // WHEEL_DELTA is 120 per notch; 3 lines per notch matches the
            // main list closely enough not to feel different.
            const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            ScrollBy(-notches * 3);
            return 0;
        }
        case WM_LBUTTONDOWN:
            OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE: {
            // Highlight the header under the pointer so the collapse
            // affordance is discoverable without a click.
            const int my = GET_Y_LPARAM(lParam);
            int hot = -1;
            const int m = S(kMargin);
            int ly = m - scrollPos_;
            for (const Line& l : lines_) {
                const int lh = S(l.height);
                if (my >= ly && my < ly + lh) {
                    if (l.kind == Line::SectionHeader && l.section >= 0)
                        hot = l.section;
                    break;
                }
                ly += lh;
            }
            if (hot != hotSection_) {
                hotSection_ = hot;
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_PAINT:
            OnPaint();
            return 0;
        case WM_ERASEBKGND:
            return 1;   // fully painted in OnPaint
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            // Re-derive the palette so a theme or high-contrast switch
            // takes effect without reopening the window.
            ApplyColors();
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_CTLCOLORBTN: {
            // Buttons are the one child control that would otherwise stay
            // light-on-light in a dark theme.
            HDC hdc = reinterpret_cast<HDC>(wParam);
            ::SetTextColor(hdc, clrText_);
            ::SetBkColor(hdc, dark_ ? kDarkBg : ::GetSysColor(COLOR_BTNFACE));
            // Assign to an HBRUSH first: a reinterpret_cast directly around
            // the GetStockObject call would be parsed against the macro.
            HBRUSH btn = reinterpret_cast<HBRUSH>(
                ::GetStockObject(COLOR_BTNFACE + 1));
            if (dark_ && brushWindow_ != nullptr) btn = brushWindow_;
            return reinterpret_cast<LRESULT>(btn);
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                ::ShowWindow(hwnd_, SW_HIDE);
                return 0;
            }
            break;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

}  // namespace wintcp
