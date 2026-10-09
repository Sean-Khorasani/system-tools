// DetailsDialog.h
// SPDX-License-Identifier: Apache-2.0
// Modeless "Details" window.
//
// Previously this was a header STATIC plus one read-only multiline EDIT fed
// a flat, hand-formatted string. That forced ES_AUTOVSCROLL (and with it a
// 640 px window and a horizontal scrollbar), could not wrap a long command
// line or path, and re-parsed the entire body on every auto-refresh tick.
//
// It is now a structured, owner-drawn control. The content arrives as a
// DetailModel - sections of label/value pairs - which the window lays out
// itself: section headers, a fixed label gutter, alternating row shading,
// a dimmer note line per section, collapsible sections, and monospace for
// the values where alignment matters. The model is built once per refresh
// and the layout is only recomputed when the client area, the model or the
// theme changes, so a live refresh is a repaint, not a rebuild.
//
// One window is reused: a second Show() while open just replaces the model;
// UpdateModel() refreshes it silently (used after every auto-refresh while
// the window is open).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>

#include <string>
#include <vector>

#include "DetailModel.h"

namespace wintcp {

class DetailsDialog {
public:
    DetailsDialog() = default;
    ~DetailsDialog();

    DetailsDialog(const DetailsDialog&) = delete;
    DetailsDialog& operator=(const DetailsDialog&) = delete;

    // Show (or refresh) the window for one connection's details.
    // 'filePath' enables the "Open file location" button (may be empty).
    // The window is centered over 'owner' when first created.
    void Show(HWND owner, HFONT font, const DetailModel& model,
              const std::wstring& filePath);

    // Same content update without showing, focusing or foregrounding:
    // keeps the live stats fresh while auto-refresh runs.
    void UpdateModel(const DetailModel& model);

    // Palette choice, derived by the caller from the live system theme
    // (MainWindow::ThemeIsDark) rather than a user setting; applied to the
    // next paint.
    void SetDark(bool dark);

    // Destroy the window. Idempotent, and safe to call when never created.
    void Close();
    bool IsOpen() const { return hwnd_ != nullptr; }
    bool IsVisible() const;
    HWND Handle() const { return hwnd_; }

    void ApplyFont(HFONT font);        // on DPI change

private:
    // One laid-out row. Everything is a single fixed-height row, so painting is
    // a straight run of DT_SINGLELINE draws with no wrapping pass.
    //
    // FieldRow is the whole point of the 2026-10-05 rework. A field used to be
    // TWO rows - a FieldLabel row and then a FieldValue row indented to
    // kLabelGutter - so each field cost 2 x kLineH and read as a caption
    // stacked over its own value. That is the "does not split correctly" report,
    // and it doubled the content height, which is what made the second fault
    // (painting under the button row) hit so easily. One row now carries both
    // halves: `text` is the label, `value` is the value, and they are drawn in
    // two columns separated by a dotted leader.
    struct Line {
        enum Kind { SectionHeader, SectionNote, FieldRow, FieldValue,
                    Connection, Blank,
                    // 9.4.1: a tab-bar slot. Painted in the fixed header region
                    // (does not scroll), unlike every other line which is part
                    // of the scrolled body. `text` is the tab label; `section`
                    // carries the DetailTab ordinal so OnPaint can distinguish
                    // active from inactive and OnMouseDown can switch.
                    Tab };
        Kind kind = FieldValue;
        std::wstring text;
        // The value column, for FieldRow only. Empty for every other kind,
        // which is why it lives here rather than replacing `text`: the
        // single-column rows (subtitle, note, connection) keep using `text`.
        std::wstring value;
        // Section index, or -1 for a line that belongs to no section (the
        // title block, the connection list). Signed on purpose: -1 is a
        // meaningful "none" and is compared directly against hotSection_.
        // For a Tab line, this is the DetailTab ordinal.
        int section = -1;
        int indent = 0;                // in logical pixels
        int height = 0;                // in logical pixels, incl. leading
        bool monospace = false;
        // For a Tab line, the measured width of the slot in logical px: the
        // paint path and the OnMouseDown hit test must agree on the slot size,
        // so it is measured once in RebuildLayout and reused for both. Zero
        // for every other kind.
        int width = 0;
        // True for the title/subtitle block and the tab bar: these are fixed
        // at the top of the window and never scroll, so a reader always sees
        // which process and which tab they're in. Only the section body
        // scrolls.
        bool pinned = false;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnCommand(WORD id);
    void Layout(int cx, int cy);
    void OnPaint();
    // 9.4.1: factored row painter. `withLeaders` is false for the pinned header
    // (the title/tab bar never draw a dotted leader); `fieldInSection` drives
    // the alternating-row shading and is advanced by the body loop in OnPaint.
    void PaintLine(HDC mem, const Line& l, int y, int lh, int cx, int m,
                   int ruleX, bool withLeaders, int fieldInSection = 0);
    void OnMouseDown(int x, int y);
    void RebuildLayout();
    void ScrollBy(int lines);
    int MaxScroll() const;
    void CopyTextToClipboard();
    void CenterOnOwner(HWND owner);
    void ApplyColors();
    // True when the system high-contrast scheme is active. Under HC the
    // system palette is already chosen for maximum contrast, so the dark_
    // flag is bypassed in ApplyColors() and the window defers to the
    // system's own colors. Mirrors MainWindow::HighContrastActive().
    static bool HighContrastActive();
    // Logical (96 dpi) -> physical for this window's monitor.
    int S(int px96) const;

    HWND hwnd_ = nullptr;
    HWND hwndCopy_ = nullptr;
    HWND hwndOpen_ = nullptr;
    HWND hwndClose_ = nullptr;

    DetailModel model_;
    std::wstring filePath_;

    // Rendered lines, in logical units; totalContentHeight_ is their sum.
    std::vector<Line> lines_;
    int totalContentHeight_ = 0;
    int scrollPos_ = 0;                  // in logical pixels
    int viewHeight_ = 0;                 // scrollable body height
    // Height of the fixed header (title + subtitle + tab bar) in logical px.
    // The body scrolls beneath it; it is recomputed in RebuildLayout so the
    // tab bar never overlaps the content.
    int headerHeight_ = 0;

    HFONT font_ = nullptr;               // borrowed from MainWindow
    HFONT headerFont_ = nullptr;         // bold, owned
    HFONT monoFont_ = nullptr;           // fixed-pitch, owned
    HFONT smallFont_ = nullptr;          // notes, owned
    bool fontsReady_ = false;
    bool dark_ = false;

    // System-derived palette, refreshed on WM_SYSCOLORCHANGE so high
    // contrast is honoured without a restart.
    HBRUSH brushWindow_ = nullptr;      // background
    HBRUSH brushAlternate_ = nullptr;   // alternating row shading
    HBRUSH brushSection_ = nullptr;     // section header band
    HBRUSH brushAccent_ = nullptr;      // section accent bar + collapse glyph
    COLORREF clrText_ = RGB(0, 0, 0);
    COLORREF clrDim_ = RGB(0x80, 0x80, 0x80);
    COLORREF clrHeader_ = RGB(0, 0, 0);

    // Which section the pointer is over, which tab it hovers, and which fields
    // are collapsed. Collapse state is per-window and never persisted: the
    // model is rebuilt every refresh, so persisting it would fight the rebuild.
    int hotSection_ = -1;
    int hotTab_ = -1;
    std::vector<unsigned char> collapsed_;

    static const wchar_t* kClassName;
};

}  // namespace wintcp
