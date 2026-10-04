// HexTextWindow.h
// The Wireshark-style "follow TCP stream" window.
//
// Two panes, hex on the left and decoded text on the right, scrolling
// together line-for-line, with the TLS handshake broken out into labelled
// fields. Owner-drawn rather than two EDIT controls for the same reasons the
// Details window is: a real offset column, per-byte highlight on selection,
// correct monospace metrics, and no horizontal-scrollbar fight.
//
// HONESTY RULE FOR THE DECODED PANE: TLS application data is encrypted and
// cannot be read by any means short of credential access, so it is rendered
// as a labelled placeholder saying exactly that. It is never rendered as
// guessed plaintext. A stream that is not TLS at all is decoded as text
// where it is printable, and marked unprintable where it is not.

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

#include "StreamCapture.h"
#include "TlsDecode.h"

namespace wintcp {

class HexTextWindow {
public:
    HexTextWindow() = default;
    ~HexTextWindow();

    HexTextWindow(const HexTextWindow&) = delete;
    HexTextWindow& operator=(const HexTextWindow&) = delete;

    // Show (or replace) the window's content. The window is created on first
    // use and reused thereafter.
    void Show(HWND owner, HFONT font, const std::wstring& title,
              const CaptureResult& result, const std::wstring& directionLabel);

    // Update without showing or focusing (used if a re-capture is merged in).
    void UpdateContent(const CaptureResult& result);

    void Close();
    bool IsOpen() const { return hwnd_ != nullptr; }
    HWND Handle() const { return hwnd_; }

    void ApplyFont(HFONT font);

    // Palette choice, derived by the caller from the live system theme.
    void SetDark(bool dark);

    // Bytes per row in the hex pane. 16 is the convention; exposed for tests.
    static constexpr int kBytesPerRow = 16;

private:
    // One rendered line. Both panes render the same line index, so the two
    // views scroll in lockstep with no cross-pane bookkeeping.
    struct Line {
        size_t offset = 0;       // absolute stream offset of bytes[0]
        bool hasBytes = false;
        bool encrypted = false;  // TLS application_data: do not decode
        bool gapMarker = false;  // a hole the capture never saw
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void OnPaint();
    void Layout(int cx, int cy);
    void OnMouseDown(int x, int y);
    void OnMouseMove(int x, int y);
    void CopySelection();
    void SaveToFile();
    void RebuildLines();
    void ScrollBy(int lines);
    int MaxScroll() const;
    void ApplyColors();
    int S(int px96) const;

    HWND hwnd_ = nullptr;
    HWND hwndStatus_ = nullptr;
    HWND hwndCopy_ = nullptr;
    HWND hwndSave_ = nullptr;
    HWND hwndClose_ = nullptr;

    // Which direction is shown. Both are kept so the user can flip between
    // them without re-capturing.
    CaptureResult result_;
    TlsHandshake tlsClient_;
    TlsHandshake tlsServer_;
    bool showServer_ = false;
    std::wstring dirLabelClient_;
    std::wstring dirLabelServer_;

    const std::string* bytes_ = nullptr;
    std::vector<Line> lines_;
    int totalContentHeight_ = 0;
    int scrollPos_ = 0;
    int viewHeight_ = 0;

    // Selection, in byte offsets. anchor_ is where the drag started, focus_
    // is the moving end; order is normalised at use.
    bool selecting_ = false;
    size_t anchorByte_ = 0;
    size_t focusByte_ = 0;

    HFONT font_ = nullptr;      // borrowed
    HFONT monoFont_ = nullptr;  // owned
    HFONT smallFont_ = nullptr; // owned
    bool fontsReady_ = false;
    bool dark_ = false;

    HBRUSH brushWindow_ = nullptr;
    HBRUSH brushAlt_ = nullptr;
    HBRUSH brushSel_ = nullptr;
    HBRUSH brushHeader_ = nullptr;
    COLORREF clrText_ = RGB(0, 0, 0);
    COLORREF clrDim_ = RGB(0x80, 0x80, 0x80);
    COLORREF clrSelText_ = RGB(0, 0, 0);

    static const wchar_t* kClassName;
};

}  // namespace wintcp
