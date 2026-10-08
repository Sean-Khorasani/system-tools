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
// One field row. It was 17 before the 2026-10-05 rework and each field was
// TWO of these (label, then value), so a section read as 34 px of stacked
// captions. Now it is one row per field, and the extra 2 px buys the vertical
// padding that makes a table legible rather than cramped.
constexpr int kLineH = 19;          // one field row: label | value on one line
constexpr int kSectionH = 24;       // section header band
constexpr int kNoteH = 16;          // dim note under a section
// Label column width, and the one number that decides whether the Details
// window looks like a table or like two columns of unrelated text.
//
// 132 -> 172, sized from the real label vocabulary rather than by eye. The
// longest labels BuildDetailModel emits are "Memory (working set)" (20 chars),
// "Certificate subject" (19) and "Certificate issuer" (18); at the 96 dpi UI
// font those measure roughly 130 px, and the label column is drawn only as far
// as ruleX - kColGap/2. With the gutter at 148 that left ~0 px of leader for the
// longest labels - the dots silently vanished on exactly the rows with the
// longest text, which is where they matter most. 172 leaves ~24 px of leader
// for the worst case and still leaves a 368 px value column at the design width
// of 560 (228 px at the 420 px minimum, enough for an address and a port).
constexpr int kLabelGutter = 172;
constexpr int kScrollBarW = 14;

// How the row is subdivided horizontally. `kColGap` is the clear space between
// the end of a label and the start of its value, which is also where the dotted
// leader is drawn - the leader is what makes the pairing read as a column
// relationship instead of two unrelated strings on one line.
constexpr int kColGap = 18;
// The leader is a run of full stops rather than a drawn line, because a dotted
// rule reads as "these belong together" while a solid one reads as a divider -
// and it costs one DrawText instead of per-pixel plotting.
constexpr int kLeaderDot = 3;
// Vertical padding inside a field row, top and bottom, in logical px.
constexpr int kRowPadY = 3;
// The hairline column rule sits this far left of the value column.
constexpr int kRuleGap = 9;

// Window geometry, authored unscaled and multiplied by the DPI ratio (S/MulDiv).
// The design size appeared TWICE - once in the CreateWindowExW call and once in
// the WM_CREATE handler - and the minimum size is a separate pair that must be
// smaller than the design size or the window can never be opened at any DPI.
// Naming all four makes those two relationships checkable.
constexpr int kDesignWidth = 560;
constexpr int kDesignHeight = 520;
constexpr int kMinWidth = 420;
constexpr int kMinHeight = 400;  // 9.4.1: +80 for the tab strip over 320
// The DPI a design pixel is 1:1 at, and the points->pixels divisor. Same two
// numbers as MainWindow.cpp has; not shared because this window scales its own
// layout and coupling the two would tie one window's geometry to the other's.
constexpr int kDpiUnscaled = 96;
constexpr int kPointsPerInch = 72;
constexpr int kBaseFontPt = 9;
// A colour-mix weight, in percent: 10 is a faint tint, 20 a readable one.
constexpr int kTintFaint = 10;
constexpr int kTintBody = 20;
// The accent bar and the leader dots. Strong enough to be seen as marks rather
// than as a tint, weak enough not to compete with the values themselves.
constexpr int kTintAccent = 46;
// Indent for a field line and for the dim note under a section, in design px.
// Both sites had the same bare 12: the two are visually the same step, and a
// note that did not line up with its heading read as a layout bug.
constexpr int kIndentPx = 12;
// The scrollbar-thumb button at the right of a section row, and its width.
constexpr int kScrollThumbW = 24;
// 9.4.1: the tab bar lives in the fixed header. One slot per tab; the slot
// width is measured from the label so short tabs don't hoard space and the
// whole row never overflows at the design width.
constexpr int kTabH = 24;
constexpr int kTabGap = 12;           // leading space before the first tab

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
if (brushAccent_ != nullptr) ::DeleteObject(brushAccent_);
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
    drop(brushAccent_);

    // Always derive from the live system colors. Under high contrast these
    // are the scheme's own values, which is exactly what we want; in a
    // normal theme they are the light palette and the dark branch below
    // substitutes its own.
    //
    // 9.4.9: high contrast BYPASSES the dark_ flag. The dark palette is a
    // hard-coded substitute for the light one; under HC the system palette is
    // already chosen for maximum contrast, and overriding it with kDarkBg
    // (near-black) on an HC-white scheme produces unreadable text. The
    // MainWindow pattern is the same: HighContrastActive() returns
    // CDRF_DODEFAULT and the custom draw defers to the system palette.
    COLORREF bg = ::GetSysColor(COLOR_WINDOW);
    COLORREF fg = ::GetSysColor(COLOR_WINDOWTEXT);
    COLORREF dim = ::GetSysColor(COLOR_GRAYTEXT);
    COLORREF header = fg;
    if (dark_ && !HighContrastActive()) {
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
    // Stronger than the section band, because the accent bar is the thing that
    // tells the eye where one section stops and the next begins. At kTintBody
    // it was indistinguishable from the band it sat on.
    brushAccent_ = ::CreateSolidBrush(Mix(bg, fg, kTintAccent));
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
    // The model is turned into a flat list of physical lines once per change;
    // painting is then a straight run of DrawText calls. Line heights are the
    // fixed logical metrics above; header slots are scaled (S()) here so the
    // pinned/header region is in physical pixels for the click and layout math.
    // A scratch DC is taken only to measure tab labels - at most kTabCount,
    // and never on the paint path.
    lines_.clear();
    lines_.reserve(64);   // ~4 screens of fields
    totalContentHeight_ = 0;
    headerHeight_ = 0;

    const auto push = [this](const Line& l) {
        lines_.push_back(l);
        totalContentHeight_ += l.height;
    };
    const auto pushHeader = [this, &push](const Line& l) {
        push(l);
        // headerHeight_ is physical (scaled), because Layout and the click/hover
        // bounds compare it against the physical client area. totalContentHeight_
        // stays logical to match the pre-9.4.1 scroll units.
        headerHeight_ += S(l.height);
    };

    if (!model_.title.empty()) {
        Line l;
        l.kind = Line::SectionHeader;
        l.text = model_.title;
        l.height = kTitleH;
        l.section = -1;
        l.pinned = true;
        pushHeader(l);
    }
    if (!model_.subtitle.empty()) {
        Line l;
        l.kind = Line::FieldValue;
        l.text = model_.subtitle;
        l.height = kSubtitleH;
        l.indent = 2;
        l.section = -1;
        l.pinned = true;
        pushHeader(l);
    }
    if (!model_.title.empty() || !model_.subtitle.empty()) {
        Line l;
        l.kind = Line::Blank;
        l.height = kMargin;
        l.section = -1;
        l.pinned = true;
        pushHeader(l);
    }

    // 9.4.1: the tab bar is fixed in the header, one slot per tab that the
    // current model actually has content for, in tab-ordinal order. Derive
    // the roster from the sections rather than hard-coding the five, so a
    // row that has no TLS simply drops the Security tab instead of showing an
    // empty one. connectionLines (the Sockets tab) are counted too.
    std::vector<DetailTab> tabs;
    for (const DetailSection& sec : model_.sections) {
        if (sec.tab != kTabSockets &&
            std::find(tabs.begin(), tabs.end(), sec.tab) == tabs.end())
            tabs.push_back(sec.tab);
    }
    if (!model_.connectionLines.empty() &&
        std::find(tabs.begin(), tabs.end(), kTabSockets) == tabs.end())
        tabs.push_back(kTabSockets);
    // Clamp the active tab to the roster; a new model may not contain it.
    if (tabs.empty() ||
        std::find(tabs.begin(), tabs.end(), model_.activeTab) == tabs.end())
        model_.activeTab = tabs.empty() ? kTabProcess : tabs.front();

    const DetailTab active = model_.activeTab;
    const int m = S(kMargin);
    const int tabSlotH = S(kTabH);
    // Measure tab labels with the header font. A scratch DC is fine here:
    // at most kTabCount labels, and RebuildLayout is not on the paint path,
    // so GetPaintDC (which only works during WM_PAINT) is intentionally NOT
    // used. hwnd_ is valid whenever RebuildLayout runs.
    HDC hdc = hwnd_ != nullptr ? ::GetDC(hwnd_) : ::GetDC(nullptr);
    HGDIOBJ oldFont = nullptr;
    if (hdc != nullptr && headerFont_ != nullptr)
        oldFont = ::SelectObject(hdc, headerFont_);
    int tabX = m;               // running x of the next tab slot
    for (DetailTab t : tabs) {
        Line l;
        l.kind = Line::Tab;
        l.text = TabLabel(t);
        l.height = kTabH;
        l.section = static_cast<int>(t);
        l.pinned = true;
        // Layout: position is resolved in OnPaint from tabX/tabY, stored via
        // the running cursor in this loop - but Line stores only logical
        // metrics. Keep the x cursor on the struct through 'indent' (reused
        // here as the slot's x origin so OnMouseDown can hit it).
        l.indent = tabX;
        pushHeader(l);
        // Measure the label so the slot width tracks its content, with a
        // minimum that keeps a short tab (e.g. "Notes") clickable. The width
        // is stored back on the pushed line so OnPaint and OnMouseDown share
        // one value.
        SIZE sz = {};
        if (hdc != nullptr && !l.text.empty())
            ::GetTextExtentPoint32W(hdc, l.text.c_str(),
                                    static_cast<int>(l.text.size()), &sz);
        const int slotW = (std::max)(S(48),
                                     static_cast<int>(sz.cx) + S(kColGap));
        lines_.back().width = slotW;
        tabX += slotW + S(kTabGap);
    }
    if (oldFont != nullptr) ::SelectObject(hdc, oldFont);
    if (hdc != nullptr) {
        if (hwnd_ != nullptr) ::ReleaseDC(hwnd_, hdc);
        else ::ReleaseDC(nullptr, hdc);
    }
    (void)tabSlotH;

    // The body scrolls. Only the active tab's sections render (plus the
    // Sockets-tab connection list), so switching tabs collapses the body to
    // the relevant fields without a separate hide/show pass.
    for (size_t s = 0; s < model_.sections.size(); ++s) {
        const DetailSection& sec = model_.sections[s];
        if (sec.tab != active) continue;
        Line h;
        h.kind = Line::SectionHeader;
        h.text = sec.title;
        h.height = kSectionH;
        h.section = static_cast<int>(s);
        push(h);

        const bool isCollapsed =
            (s < collapsed_.size() && collapsed_[s] != 0);
        if (isCollapsed) continue;

        // ONE row per field, carrying both halves - see Line::FieldRow for why.
        for (const DetailField& f : sec.fields) {
            Line l;
            l.kind = Line::FieldRow;
            l.text = f.label;
            l.value = f.value;
            // Monospace is a property of the VALUE, not the pair: a monospace
            // label would push the value column out of alignment, because the
            // two columns are aligned by position, not by text.
            l.monospace = f.monospace;
            l.section = static_cast<int>(s);
            l.height = kLineH;
            push(l);
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

    if (active == kTabSockets && !model_.connectionLines.empty()) {
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
    // area between the header (title + tab bar, pinned) and the button row.
    const int top = m + headerHeight_;
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

void DetailsDialog::PaintLine(HDC mem, const Line& l, int y, int lh, int cx,
                              int m, int ruleX, bool withLeaders,
                              int fieldInSection) {
    // Selects the right face for this row kind. Section headers and notes keep
    // their own faces; field rows honour the value's monospace flag separately
    // below (the label always uses the UI face).
    const HFONT face =
        (l.kind == Line::SectionHeader) ? headerFont_
        : (l.kind == Line::SectionNote)   ? smallFont_
        : l.monospace                     ? monoFont_
                                          : font_;
    if (face != nullptr) ::SelectObject(mem, face);

    if (l.kind == Line::Tab) {
        // 9.4.1: tab bar slot. Active tab = header colour with an accent
        // underline; inactive = dim; hover brightens the label. Layout stored
        // the slot origin in 'indent' and its measured width in 'width'.
        const bool active = (l.section == static_cast<int>(model_.activeTab));
        const COLORREF tabFg = (l.section == hotTab_)
                                  ? clrHeader_
                                  : (active ? clrHeader_ : clrDim_);
        ::SetTextColor(mem, tabFg);
        const int slotW = l.width;
        RECT tr = {l.indent, y, l.indent + slotW, y + lh};
        if (active && brushSection_ != nullptr) ::FillRect(mem, &tr, brushSection_);
        ::DrawTextW(mem, l.text.c_str(), static_cast<int>(l.text.size()),
                    &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
                        DT_END_ELLIPSIS);
        if (active && brushAccent_ != nullptr) {
            RECT bar = {l.indent, y + lh - S(2), l.indent + slotW, y + lh};
            ::FillRect(mem, &bar, brushAccent_);
        }
        return;
    }

    // Alternating shading on field rows only, and a band behind every section
    // header - the same policy as before 9.4.1, routed through the painter.
    if (l.kind == Line::FieldRow) {
        if ((fieldInSection % 2) == 0 && brushAlternate_ != nullptr) {
            RECT band = {0, y, cx, y + lh};
            ::FillRect(mem, &band, brushAlternate_);
        }
    } else if (l.kind == Line::SectionHeader && brushSection_ != nullptr) {
        RECT band = {0, y, cx, y + lh};
        ::FillRect(mem, &band, brushSection_);
        // Accent bar on EVERY section header, not only the hovered one.
        if (brushAccent_ != nullptr) {
            RECT bar = {0, y, S(3), y + lh};
            ::FillRect(mem, &bar, brushAccent_);
        }
        // Hover feedback is now the collapse glyph, which brightens - the bar
        // itself stays put so the layout does not jump.
        if (l.section >= 0 && l.section == hotSection_) {
            ::SetBkMode(mem, OPAQUE);
            ::SetBkColor(mem, clrHeader_);
            RECT dot = {S(kMargin - S(2)), y + lh / 2 - S(kNoteH / 2),
                        S(kScrollThumbW / 2), y + lh / 2 + S(2)};
            ::FillRect(mem, &dot, brushSection_);
            ::SetBkMode(mem, TRANSPARENT);
        }
    }

    if (l.kind == Line::FieldRow) {
        // ---- the two columns -------------------------------------
        // Label, in the gutter. Ellipsised, because a label that overruns
        // its column would otherwise push into the value.
        ::SetTextColor(mem, clrDim_);
        RECT labelRect = {m, y, ruleX - S(kColGap) / 2, y + lh};
        ::DrawTextW(mem, l.text.c_str(),
                    static_cast<int>(l.text.size()), &labelRect,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
                        DT_END_ELLIPSIS);

        // The dotted leader between them. Measured from the label that was
        // actually drawn, so it starts where the text ends rather than at a
        // guessed offset - with DT_END_ELLIPSIS the drawn label may be
        // shorter than the original string.
        if (brushAccent_ != nullptr && withLeaders && !l.text.empty() &&
            !l.value.empty()) {
            labelRect.right = labelRect.left;
            SIZE want = {};
            ::GetTextExtentPoint32W(
                mem, l.text.c_str(), static_cast<int>(l.text.size()), &want);
            if (want.cx > 0 && want.cx < labelRect.right) {
                SIZE dotSize = {};
                ::GetTextExtentPoint32W(mem, L"..", 2, &dotSize);
                const int step = (std::max)(
                    1, static_cast<int>(dotSize.cx) / 2 + S(kLeaderDot));
                const int from =
                    (std::min)(static_cast<int>(labelRect.left + want.cx),
                               static_cast<int>(labelRect.right)) +
                    S(kColGap) / 2;
                const int to = ruleX - S(kColGap) / 2 - S(kRuleGap) / 2;
                std::wstring dots;
                for (int dx = from; dx < to; dx += step) {
                    dots.push_back(L'.');
                    if (dots.size() >= 512) break;
                }
                if (!dots.empty() && clrDim_ != 0) {
                    ::SetTextColor(mem, clrDim_);
                    RECT dotRect = {from, y, to, y + lh};
                    ::DrawTextW(mem, dots.c_str(),
                                static_cast<int>(dots.size()), &dotRect,
                                DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                }
            }
        }

        // The hairline between the two columns, inset from the leader so it
        // never touches the value text.
        if (brushAccent_ != nullptr) {
            RECT rule = {ruleX, y + S(kRowPadY), ruleX + 1,
                         y + lh - S(kRowPadY)};
            ::FillRect(mem, &rule, brushAccent_);
        }

        // Value, starting at a fixed x so the column is aligned regardless of
        // label length. Monospace only when the field asked for it; the label
        // always uses the UI face.
        if (font_ != nullptr) ::SelectObject(mem, font_);
        ::SetTextColor(mem, clrText_);
        const HFONT valueFace = l.monospace ? monoFont_ : font_;
        if (valueFace != nullptr) ::SelectObject(mem, valueFace);
        RECT valueRect = {m + S(kLabelGutter), y, cx - m, y + lh};
        ::DrawTextW(mem, l.value.c_str(),
                    static_cast<int>(l.value.size()), &valueRect,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
                        DT_END_ELLIPSIS);
        return;
    }

    // Single-column rows: subtitle, note, connection line.
    ::SetTextColor(mem,
                   (l.kind == Line::SectionNote)     ? clrDim_
                   : (l.kind == Line::SectionHeader) ? clrHeader_
                                                    : clrText_);
    RECT draw = {m + S(l.indent), y, cx - m, y + lh};
    ::DrawTextW(mem, l.text.c_str(), static_cast<int>(l.text.size()),
                &draw,
                DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
                    DT_END_ELLIPSIS);
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
    const int ruleX = m + S(kLabelGutter) - S(kRuleGap);

    // --- pinned header: title + subtitle block + tab bar (fixed at the top) ---
    // Painted without the scroll offset, so the process identity and the tab
    // strip stay in view as the body scrolls beneath them.
    int y = m;
    for (const Line& l : lines_) {
        if (!l.pinned) break;
        const int lh = S(l.height);
        if (y + lh > 0 && y < cy) PaintLine(mem, l, y, lh, cx, m, ruleX, false);
        y += lh;
    }
    const int headerBottom = y;   // one past the last pinned line

    // --- scrollable body: the active tab's sections + Sockets list ---------------
    //
    // The body's own bottom edge, which is ABOVE the button row. The clip used
    // to be `y <= cy` - the full client height - so the last visible fields were
    // painted behind Copy / Open File Location / Close. The buttons are child
    // windows and cover part of it; the rest showed through around them. That is
    // the "the table goes under the buttons" report, and it was a missing bound
    // rather than a layout error: Layout() already computed viewHeight_ correctly
    // and the hand-drawn scrollbar already honoured it.
    //
    // DEPENDS ON viewHeight_ BEING SET BEFORE THE FIRST PAINT, because it
    // defaults to 0 and `m + 0 == m` would clip the body to nothing - a blank
    // window rather than a wrong one, which is at least not silently plausible.
    // Two independent paths establish it: WM_CREATE calls ApplyFont, which calls
    // Layout(GetClientRect) at the end; and WM_SIZE calls Layout directly, and a
    // created window is always sent WM_SIZE before it is ever painted. So this
    // bound is safe today, but it is a landmine for anyone who adds a paint path
    // that can run before both - if you add one, call Layout() first.
    const int contentBottom = headerBottom + viewHeight_;

    int fieldInSection = 0;
    y = headerBottom - scrollPos_;
    for (const Line& l : lines_) {
        if (l.pinned) continue;   // header already drawn above
        const int lh = S(l.height);
        if (l.kind == Line::FieldRow) ++fieldInSection;
        // Clipped to the BODY, not the client rect. `y < contentBottom` also
        // stops a row being drawn when only its very bottom edge is inside.
        if (y + lh > headerBottom && y < contentBottom)
            PaintLine(mem, l, y, lh, cx, m, ruleX, true, fieldInSection);
        y += lh;
    }

    // A scrollbar, drawn by hand: the body is not a child control, so there
    // is nothing to attach a real scrollbar to. It tracks the body region
    // (below the pinned header), not the full client height.
    const int maxScroll = MaxScroll();
    if (maxScroll > 0 && viewHeight_ > 0) {
        const int trackX = cx - S(kScrollBarW) - m;
        const int trackH = viewHeight_;
        RECT track = {trackX, headerBottom, cx - m, headerBottom + trackH};
        if (brushAlternate_ != nullptr)
            ::FillRect(mem, &track, brushAlternate_);
        const int thumbH = (std::max)(
            kScrollThumbW,
            trackH * viewHeight_ / (std::max)(1, totalContentHeight_));
        const int thumbY = headerBottom +
                           (trackH - thumbH) * scrollPos_ / maxScroll;
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
    // Two interactive surfaces: the tab bar (pinned header) and the section
    // headers in the body. A click elsewhere is left alone so the window still
    // behaves like a static read-out.
    const int m = S(kMargin);

    // --- tab bar: pinned at the top, before the body's scroll offset --------
    // Reconstruct each tab slot's rect from the tab Lines stored by
    // RebuildLayout (origin in 'indent', width measured from the label), so
    // the hit test and the paint stay in lockstep.
    int tabY = m;
    for (const Line& l : lines_) {
        if (!l.pinned) break;
        const int lh = S(l.height);
        if (l.kind == Line::Tab && y >= tabY && y < tabY + lh) {
            const int slotW = l.width;
            if (x >= l.indent && x < l.indent + slotW) {
                const DetailTab t = static_cast<DetailTab>(l.section);
                if (t != model_.activeTab) {
                    model_.activeTab = t;
                    RebuildLayout();
                    ::InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return;
            }
        }
        tabY += lh;
    }

    // --- body: the same missing bound as the paint loop, for the same reason -
    // a click in the button row used to match a row behind it and toggle that
    // section while the user was aiming at Close.
    if (y >= m + headerHeight_ + viewHeight_) return;
    int ly = m + headerHeight_ - scrollPos_;
    for (const Line& l : lines_) {
        if (l.pinned) { ly += S(l.height); continue; }   // header, already covered
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
            // Track two hover targets: the tab bar (pinned header) and the
            // section headers in the body. Only the pointer's own region is
            // tested, so a stray enter/leave does not flip both at once.
            const int mx = GET_X_LPARAM(lParam);
            const int my = GET_Y_LPARAM(lParam);
            const int m = S(kMargin);

            // Tab bar hover (pinned, before the scroll offset).
            int hotTab = -1;
            int tabY = m;
            for (const Line& l : lines_) {
                if (!l.pinned) break;
                const int lh = S(l.height);
                if (l.kind == Line::Tab && my >= tabY && my < tabY + lh) {
                    if (mx >= l.indent && mx < l.indent + l.width)
                        hotTab = l.section;
                    break;
                }
                tabY += lh;
            }
            if (hotTab != hotTab_) {
                hotTab_ = hotTab;
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }

            // Section-header hover (body). Same missing bound as the paint
            // and click paths: without it, hovering the button row lit up a
            // section header hidden behind it.
            int hot = -1;
            const int headerBottom = m + headerHeight_;
            if (my >= headerBottom && my < headerBottom + viewHeight_) {
                int ly = headerBottom - scrollPos_;
                for (const Line& l : lines_) {
                    if (l.pinned) { ly += S(l.height); continue; }
                    const int lh = S(l.height);
                    if (my >= ly && my < ly + lh) {
                        if (l.kind == Line::SectionHeader && l.section >= 0)
                            hot = l.section;
                        break;
                    }
                    ly += lh;
                }
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
            //
            // 9.4.9: the SAME HighContrastActive() gate ApplyColors uses. Without
            // it, a dark theme under high contrast would still push kDarkBg onto
            // the button background here (dark_ is true, HighContrastActive is
            // true, and the kDarkBg branch fired anyway), producing near-black
            // text on near-black in an HC scheme the system already picked for
            // contrast. dark_ alone is not enough - HC must win, exactly as it
            // does in ApplyColors line 167.
            HDC hdc = reinterpret_cast<HDC>(wParam);
            const bool dark = dark_ && !HighContrastActive();
            ::SetTextColor(hdc, clrText_);
            ::SetBkColor(hdc, dark ? kDarkBg : ::GetSysColor(COLOR_BTNFACE));
            HBRUSH btn = reinterpret_cast<HBRUSH>(
                ::GetStockObject(COLOR_BTNFACE + 1));
            if (dark && brushWindow_ != nullptr) btn = brushWindow_;
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

bool DetailsDialog::HighContrastActive() {
    HIGHCONTRASTW hc = {};
    hc.cbSize = sizeof(hc);
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) == 0)
        return false;
    return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

}  // namespace wintcp
