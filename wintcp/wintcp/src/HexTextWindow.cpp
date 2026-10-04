// HexTextWindow.cpp
// See HexTextWindow.h. Layout is computed once per content change; painting
// is a straight run of DrawText calls over a precomputed line list.

#include "HexTextWindow.h"

#include <commdlg.h>     // GetSaveFileNameW, OPENFILENAMEW
#include <windowsx.h>    // GET_X_LPARAM / GET_Y_LPARAM

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Utils.h"
#include "WinCaps.h"   // DelayLoadGuard: comdlg32.dll is delay-loaded

namespace wintcp {
namespace {

const UINT IDC_STREAM_COPY = 1;
const UINT IDC_STREAM_SAVE = 2;
const UINT IDC_STREAM_CLOSE = 3;

// Logical (96 dpi) metrics.
constexpr int kMargin = 8;
constexpr int kLineH = 16;
constexpr int kBtnH = 26;
constexpr int kBtnWCpy = 90;
constexpr int kBtnWSave = 110;
constexpr int kBtnWClose = 70;
constexpr int kStatusH = 34;
constexpr int kHexWidth = 400;   // hex column, logical
constexpr int kHeaderH = 20;

// Per-row scratch capacities, DERIVED from the row size rather than written as
// their own numbers. This is a buffer-boundary fix, not cosmetics: the hex cell
// is written 4 wchars per byte ("%02X " plus the NUL the next call overwrites),
// so the buffer must be kHexCols * 4 and nothing less, and the ASCII cell
// needs one char per byte PLUS the terminator the loop writes at [kHexCols].
// At 16 bytes/row the old literals (64 and 32) happened to fit with zero slack
// on the hex side — change the row width and one of them silently overflows a
// stack buffer inside the paint loop.
//
// kHexCols is the header's, so there is ONE definition of the row width and
// these capacities follow it. sizeof() rather than _countof: the expressions
// are computed, so the array bound has to be a constant expression either way,
// and sizeof states the capacity the way the rest of the file does.
constexpr int kHexCols = HexTextWindow::kBytesPerRow;
constexpr size_t kHexCharsPerByte = 4;          // "XX " + terminator
constexpr size_t kHexRowChars = kHexCols * kHexCharsPerByte;
constexpr size_t kAscRowChars = kHexCols + 1;   // + the NUL
// The offset column prints 8 hex digits, so 8 + terminator, rounded up.
constexpr size_t kOffsetChars = 16;
// The header row: "offset", the hex run, then the ASCII run.
constexpr size_t kHeaderChars = kOffsetChars + kHexRowChars + kAscRowChars;

// Pixel geometry of the three columns. These are layout constants that were
// bare numbers at their use sites, and the hex column's width MUST equal
// kHexRowChars*2/3 or the ASCII column is drawn over the last hex digits —
// naming them together with the character capacities is what makes that
// relationship checkable instead of coincidental.
constexpr int kHexColOffset = 60;   // px from the margin to the hex column
constexpr int kAscGapPx = 20;       // px between hex and decoded columns
constexpr int kColGapPx = 4;        // px trimmed between columns
constexpr int kOffsetColWidthPx = 60;  // px reserved for the offset column
// px per byte-cell in the hex column: 3 chars ("XX ") at the fixed-pitch font,
// which is also what the click handler divides by to find which byte a click
// landed on. Hit-testing and painting MUST use the same number or a click
// selects a neighbouring byte — so there is one, and both use it.
constexpr int kHexCellPx = 9;

const COLORREF kDarkBg = RGB(0x1E, 0x1E, 0x1E);
const COLORREF kDarkText = RGB(0xF0, 0xF0, 0xF0);
const COLORREF kDarkDim = RGB(0x9A, 0x9A, 0x9A);
const COLORREF kDarkAlt = RGB(0x26, 0x26, 0x26);
const COLORREF kDarkSel = RGB(0x2F, 0x54, 0x96);

COLORREF Mix(COLORREF a, COLORREF b, int t) {
    const int r = (GetRValue(a) * (255 - t) + GetRValue(b) * t) / 255;
    const int g = (GetGValue(a) * (255 - t) + GetBValue(b) * t) / 255;
    const int bl = (GetBValue(a) * (255 - t) + GetBValue(b) * t) / 255;
    return RGB(r, g, bl);
}

// Printable-ASCII test for the decoded pane. Deliberately conservative:
// bytes outside 0x20..0x7E render as '.', which is the convention every hex
// viewer uses and never invents structure.
inline bool Printable(unsigned char c) { return c >= 0x20 && c <= 0x7E; }

std::wstring WidenAscii(const std::string& s) {
    return Utf8ToWide(s.c_str());
}

}  // namespace

const wchar_t* HexTextWindow::kClassName = L"WinTcpStreamWnd";

HexTextWindow::~HexTextWindow() {
    Close();
    if (monoFont_ != nullptr) ::DeleteObject(monoFont_);
    if (smallFont_ != nullptr) ::DeleteObject(smallFont_);
    if (brushWindow_ != nullptr) ::DeleteObject(brushWindow_);
    if (brushAlt_ != nullptr) ::DeleteObject(brushAlt_);
    if (brushSel_ != nullptr) ::DeleteObject(brushSel_);
    if (brushHeader_ != nullptr) ::DeleteObject(brushHeader_);
}

int HexTextWindow::S(int px96) const {
    const UINT dpi = QueryDpiForWindow(hwnd_);
    return ::MulDiv(px96, static_cast<int>(dpi), 96);
}

void HexTextWindow::ApplyColors() {
    const auto drop = [](HBRUSH& b) {
        if (b != nullptr) { ::DeleteObject(b); b = nullptr; }
    };
    drop(brushWindow_); drop(brushAlt_); drop(brushSel_); drop(brushHeader_);

    COLORREF bg = ::GetSysColor(COLOR_WINDOW);
    COLORREF fg = ::GetSysColor(COLOR_WINDOWTEXT);
    COLORREF dim = ::GetSysColor(COLOR_GRAYTEXT);
    if (dark_) { bg = kDarkBg; fg = kDarkText; dim = kDarkDim; }
    clrText_ = fg;
    clrDim_ = dim;
    // Selection must stay legible against the text, so derive the text colour
    // from the selection rather than assuming white-on-blue.
    const COLORREF sel = dark_ ? kDarkSel
                               : ::GetSysColor(static_cast<int>(COLOR_HIGHLIGHT));
    clrSelText_ = dark_ ? RGB(0xFF, 0xFF, 0xFF)
                        : ::GetSysColor(static_cast<int>(COLOR_HIGHLIGHTTEXT));
    brushWindow_ = ::CreateSolidBrush(bg);
    brushAlt_ = ::CreateSolidBrush(Mix(bg, fg, 8));
    brushSel_ = ::CreateSolidBrush(sel);
    brushHeader_ = ::CreateSolidBrush(Mix(bg, fg, 18));
}

void HexTextWindow::RebuildLines() {
    lines_.clear();
    totalContentHeight_ = 0;
    // Point at the direction actually being shown. This must be set here, not
    // only in Show(): it is what every paint reads, and a null bytes_ with a
    // non-empty line list would dereference it.
    bytes_ = showServer_ ? &result_.toClient.bytes : &result_.toServer.bytes;
    if (bytes_ == nullptr) return;

    const size_t n = bytes_->size();
    // Which byte ranges are encrypted, so the decoded pane can say so instead
    // of printing ciphertext as if it were text. Read from the same direction
    // bytes_ points at.
    std::vector<std::pair<size_t, size_t>> encRanges;
    {
        const ReasmResult& r = showServer_ ? result_.toClient : result_.toServer;
        if (LooksLikeTls(r.bytes)) {
            for (const TlsRecord& rec : ParseTlsRecords(r.bytes)) {
                if (rec.type != kTlsApplicationData) continue;
                // payloadOffset already points past the 5-byte record header.
                encRanges.emplace_back(rec.payloadOffset, rec.payloadOffset +
                                                             rec.length);
            }
        }
    }
    const auto isEnc = [&encRanges](size_t off) {
        for (const auto& p : encRanges)
            if (off >= p.first && off < p.second) return true;
        return false;
    };

    lines_.reserve(n / kBytesPerRow + 8);
    for (size_t off = 0; off < n; off += kBytesPerRow) {
        Line l;
        l.offset = off;
        l.hasBytes = true;
        l.encrypted = isEnc(off);
        lines_.push_back(l);
        totalContentHeight_ += kLineH;
    }
    // One header row so the column meanings are visible without a tooltip.
    totalContentHeight_ += kHeaderH;
    const int maxScroll = MaxScroll();
    if (scrollPos_ > maxScroll) scrollPos_ = maxScroll;
}

int HexTextWindow::MaxScroll() const {
    const int over = totalContentHeight_ - viewHeight_;
    return (over > 0) ? over : 0;
}

void HexTextWindow::ScrollBy(int lines) {
    int next = scrollPos_ + lines * kLineH;
    next = (std::max)(0, (std::min)(next, MaxScroll()));
    if (next == scrollPos_) return;
    scrollPos_ = next;
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void HexTextWindow::Show(HWND owner, HFONT font, const std::wstring& title,
                          const CaptureResult& result,
                          const std::wstring& directionLabel) {
    result_ = result;
    dirLabelClient_ = L"Client → Server";
    dirLabelServer_ = L"Server → Client";
    tlsClient_ = ParseTlsHandshake(result.toServer.bytes);
    tlsServer_ = ParseTlsHandshake(result.toClient.bytes);
    if (font_ != font) ApplyFont(font);

    if (hwnd_ == nullptr) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &HexTextWindow::WindowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClassName;
        if (::RegisterClassExW(&wc) == 0 &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return;
        hwnd_ = ::CreateWindowExW(
            0, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 900, 620, owner, nullptr,
            ::GetModuleHandleW(nullptr), this);
        if (hwnd_ == nullptr) return;
        const UINT dpi = QueryDpiForWindow(hwnd_);
        ::SetWindowPos(hwnd_, nullptr, 0, 0, ::MulDiv(900, (int)dpi, 96),
                       ::MulDiv(620, (int)dpi, 96),
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        ::SetWindowTextW(hwnd_, title.c_str());
    }
    (void)directionLabel;
    RebuildLines();

    // Status line: say what was actually captured, including the unflattering
    // parts (partial stream, missing bytes, flow records). A window that just
    // shows bytes invites the reader to assume completeness.
    if (hwndStatus_ != nullptr) {
        const ReasmResult& shown = showServer_ ? result_.toServer
                                               : result_.toClient;
        std::wstring s = directionLabel.empty()
                             ? std::wstring(L"captured stream")
                             : directionLabel;
        s += L"   —   " + std::to_wstring(shown.bytes.size()) + L" bytes";
        s += L" in " + std::to_wstring(shown.segments) + L" segment(s)";
        if (shown.duplicates > 0)
            s += L", " + std::to_wstring(shown.duplicates) + L" duplicate(s) ignored";
        if (shown.bytesMissing > 0)
            s += L"   ⚠ " + std::to_wstring(shown.bytesMissing) +
                 L" byte(s) missing from the capture";
        if (shown.truncated)
            s += L"   ⚠ stream truncated at " +
                 std::to_wstring(shown.bytes.size()) + L" bytes";
        if (!result_.complete)
            s += L"   (SYN or FIN not captured — this is a partial view)";
        const TlsHandshake& hs = showServer_ ? tlsClient_ : tlsServer_;
        if (!hs.sni.empty()) {
            s += L"   —   " + WidenAscii(hs.summary());
            if (hs.sawApplicationData) {
                s += L"   —   application data is encrypted and cannot be "
                     L"decoded without the session keys; the handshake above "
                     L"is read in cleartext by design.";
            }
        }
        ::SetWindowTextW(hwndStatus_, s.c_str());
    }

    if (::IsWindowVisible(hwnd_) == FALSE) ::ShowWindow(hwnd_, SW_SHOW);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void HexTextWindow::SetDark(bool dark) {
    if (dark_ == dark) return;
    dark_ = dark;
    ApplyColors();
    if (hwnd_ != nullptr) ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void HexTextWindow::UpdateContent(const CaptureResult& result) {
    if (hwnd_ == nullptr) return;
    result_ = result;
    tlsClient_ = ParseTlsHandshake(result.toServer.bytes);
    tlsServer_ = ParseTlsHandshake(result.toClient.bytes);
    RebuildLines();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void HexTextWindow::Close() {
    if (hwnd_ != nullptr && ::IsWindow(hwnd_)) ::DestroyWindow(hwnd_);
}

void HexTextWindow::ApplyFont(HFONT font) {
    font_ = font;
    if (font_ == nullptr) return;
    if (hwnd_ != nullptr && !fontsReady_) {
        LOGFONTW lf = {};
        if (::GetObjectW(font_, sizeof(lf), &lf) != sizeof(lf)) return;
        const LONG base = lf.lfHeight;
        lf.lfWeight = FW_NORMAL;
        lf.lfHeight = base;
        if (monoFont_ != nullptr) ::DeleteObject(monoFont_);
        ::wcscpy_s(lf.lfFaceName, L"Consolas");
        monoFont_ = ::CreateFontIndirectW(&lf);
        lf.lfHeight = (base * 8) / 9;
        if (smallFont_ != nullptr) ::DeleteObject(smallFont_);
        ::wcscpy_s(lf.lfFaceName, L"Segoe UI");
        smallFont_ = ::CreateFontIndirectW(&lf);
        fontsReady_ = true;
        ApplyColors();
    }
    if (hwnd_ != nullptr) {
        RECT rc = {};
        ::GetClientRect(hwnd_, &rc);
        Layout(rc.right, rc.bottom);
        RebuildLines();
    }
}

void HexTextWindow::Layout(int cx, int cy) {
    if (hwnd_ == nullptr) return;
    const int m = S(kMargin);
    const int btnH = S(kBtnH);
    const int btnY = cy - m - btnH;
    const int statusH = S(kStatusH);
    const int top = m + S(kHeaderH);

    viewHeight_ = cy - btnY - top;
    if (viewHeight_ < 0) viewHeight_ = 0;

    if (hwndStatus_ != nullptr)
        ::MoveWindow(hwndStatus_, m, top, cx - 2 * m, statusH, TRUE);
    if (hwndCopy_ != nullptr)
        ::MoveWindow(hwndCopy_, m, btnY, S(kBtnWCpy), btnH, TRUE);
    if (hwndSave_ != nullptr)
        ::MoveWindow(hwndSave_, m + S(kBtnWCpy) + m, btnY, S(kBtnWSave), btnH,
                     TRUE);
    if (hwndClose_ != nullptr)
        ::MoveWindow(hwndClose_, cx - m - S(kBtnWClose), btnY, S(kBtnWClose),
                     btnH, TRUE);
}

void HexTextWindow::OnPaint() {
    PAINTSTRUCT ps = {};
    HDC hdc = ::BeginPaint(hwnd_, &ps);
    if (hdc == nullptr) return;
    RECT rc = {};
    ::GetClientRect(hwnd_, &rc);
    const int cx = rc.right, cy = rc.bottom;

    HDC mem = ::CreateCompatibleDC(hdc);
    HBITMAP bmp = ::CreateCompatibleBitmap(hdc, cx, cy);
    HGDIOBJ oldBmp = (bmp != nullptr) ? ::SelectObject(mem, bmp) : nullptr;
    if (brushWindow_ != nullptr) ::FillRect(mem, &rc, brushWindow_);
    ::SetBkMode(mem, TRANSPARENT);
    if (monoFont_ != nullptr) ::SelectObject(mem, monoFont_);

    const int m = S(kMargin);
    const size_t selStart = (std::min)(anchorByte_, focusByte_);
    const size_t selEnd = (std::max)(anchorByte_, focusByte_);
    const bool hasSel = selEnd > selStart;

    // Column headers.
    {
        wchar_t hdr[kHeaderChars] = {0};
        ::swprintf_s(hdr, L"%-9s  %-*s  %s", L"offset",
                     static_cast<int>(kHexRowChars), L"hex", L"decoded");
        RECT hr = {m, m, cx - m, m + S(kHeaderH)};
        if (brushHeader_ != nullptr) ::FillRect(mem, &hr, brushHeader_);
        ::SetTextColor(mem, clrDim_);
        ::DrawTextW(mem, hdr, -1, &hr,
                     DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }

    int y = m + S(kHeaderH) - scrollPos_;
    int row = 0;
    for (const Line& l : lines_) {
        const int lh = S(kLineH);
        if (y + lh >= m + S(kHeaderH) && y <= cy) {
            if ((row % 2) == 1 && brushAlt_ != nullptr) {
                RECT band = {0, y, cx, y + lh};
                ::FillRect(mem, &band, brushAlt_);
            }
            const size_t n = bytes_->size();
            const size_t start = l.offset;
            const size_t count = (std::min)((size_t)kBytesPerRow, n - start);

            wchar_t off[kOffsetChars] = {0};
            ::swprintf_s(off, L"%08llX", static_cast<unsigned long long>(start));
            wchar_t hexPart[kHexRowChars] = {0};
            wchar_t ascPart[kAscRowChars] = {0};
            for (size_t i = 0; i < kHexCols; ++i) {
                if (i < count) {
                    const unsigned char c =
                        static_cast<unsigned char>((*bytes_)[start + i]);
                    ::swprintf_s(hexPart + ::wcslen(hexPart), kHexCharsPerByte, L"%02X ", c);
                    ascPart[i] = Printable(c) ? static_cast<wchar_t>(c) : L'.';
                } else {
                    ::swprintf_s(hexPart + ::wcslen(hexPart), kHexCharsPerByte, L"   ");
                }
                ascPart[i + 1] = L'\0';
            }

            // Column x positions, computed once so the highlight and the
            // text can never disagree about where a byte is drawn.
            const int xOffset = m;
            const int xHex = m + S(kHexColOffset);
            const int xAsc = xHex + S(kHexRowChars) + S(kAscGapPx);

            ::SetTextColor(mem, hasSel ? clrSelText_ : clrText_);
            RECT orc = {xOffset, y, xHex - S(kColGapPx), y + lh};
            ::DrawTextW(mem, off, -1, &orc, DT_SINGLELINE | DT_VCENTER);

            // Selection highlight spans the byte range on this row, drawn
            // before the text so the text sits on top of it.
            if (hasSel) {
                const size_t rs = (std::max)(selStart, start);
                const size_t re = (std::min)(selEnd, start + count);
                if (re > rs) {
                    const int cellW = S(9);
                    RECT sel = {xHex + static_cast<int>(rs - start) * cellW, y,
                                xHex + static_cast<int>(re - start) * cellW,
                                y + lh};
                    if (brushSel_ != nullptr) ::FillRect(mem, &sel, brushSel_);
                }
            }

            RECT hrc = {xHex, y, xAsc - S(8), y + lh};
            ::DrawTextW(mem, hexPart, -1, &hrc, DT_SINGLELINE | DT_VCENTER);

            // Decoded pane: a TLS payload is labelled, never guessed at.
            RECT nrc = {xAsc, y, cx - m, y + lh};
            if (l.encrypted) {
                ::SetTextColor(mem, clrDim_);
                ::DrawTextW(mem, L"[encrypted TLS record]", -1, &nrc,
                            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            } else {
                ::SetTextColor(mem, hasSel ? clrSelText_ : clrText_);
                ::DrawTextW(mem, ascPart, -1, &nrc,
                            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
        }
        y += lh;
        ++row;
    }

    ::BitBlt(hdc, 0, 0, cx, cy, mem, 0, 0, SRCCOPY);
    if (oldBmp != nullptr) ::SelectObject(mem, oldBmp);
    if (bmp != nullptr) ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::EndPaint(hwnd_, &ps);
}

void HexTextWindow::CopySelection() {
    if (bytes_ == nullptr) return;
    const size_t start = (std::min)(anchorByte_, focusByte_);
    const size_t end = (std::min)((std::max)(anchorByte_, focusByte_),
                                  bytes_->size());
    if (end <= start) return;
    const std::string sel = bytes_->substr(start, end - start);
    if (!::OpenClipboard(hwnd_)) return;
    ::EmptyClipboard();
    const size_t bytesOut = (sel.size() + 1) * sizeof(char);
    HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytesOut);
    if (h != nullptr) {
        void* p = ::GlobalLock(h);
        if (p != nullptr) {
            std::memcpy(p, sel.data(), bytesOut);
            ::GlobalUnlock(h);
            if (::SetClipboardData(CF_TEXT, h) == nullptr) ::GlobalFree(h);
            h = nullptr;
        }
        if (h != nullptr) ::GlobalFree(h);
    }
    ::CloseClipboard();
}

void HexTextWindow::SaveToFile() {
    if (bytes_ == nullptr) return;
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Hex dumps (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    // comdlg32.dll is delay-loaded; DelayLoadGuard reports its absence, where a
    // bare `return` would look exactly like the user cancelling.
    if (!DelayLoadGuard(hwnd_, "comdlg32.dll")) return;
    if (::GetSaveFileNameW(&ofn) == FALSE) return;

    FILE* f = nullptr;
    if (::_wfopen_s(&f, path, L"wb") != 0 || f == nullptr) return;
    // Classic hexdump format so the file opens in any viewer.
    for (size_t off = 0; off < bytes_->size(); off += kBytesPerRow) {
        const size_t count = (std::min)((size_t)kBytesPerRow,
                                        bytes_->size() - off);
        std::fprintf(f, "%08llX  ", static_cast<unsigned long long>(off));
        for (size_t i = 0; i < kBytesPerRow; ++i) {
            if (i < count)
                std::fprintf(f, "%02X ",
                             (unsigned)(unsigned char)(*bytes_)[off + i]);
            else
                std::fprintf(f, "   ");
            if (i == 7) std::fprintf(f, " ");
        }
        std::fprintf(f, " |");
        for (size_t i = 0; i < count; ++i) {
            const unsigned char c = (unsigned char)(*bytes_)[off + i];
            std::fputc(Printable(c) ? c : '.', f);
        }
        std::fprintf(f, "|\n");
    }
    std::fclose(f);
}

void HexTextWindow::OnMouseDown(int x, int y) {
    if (bytes_ == nullptr || x < S(kOffsetColWidthPx) ||
        x > S(kOffsetColWidthPx) + S(kHexRowChars / kHexCharsPerByte * 3)) {
        return;
    }
    const int m = S(kMargin);
    int ly = m + S(kHeaderH) - scrollPos_;
    for (const Line& l : lines_) {
        const int lh = S(kLineH);
        if (y >= ly && y < ly + lh) {
            const int rel = (x - S(kOffsetColWidthPx)) / S(kHexCellPx);
            size_t byteOff = l.offset + (size_t)(rel < 0 ? 0 : rel);
            if (byteOff > bytes_->size()) byteOff = bytes_->size();
            anchorByte_ = focusByte_ = byteOff;
            selecting_ = true;
            ::SetCapture(hwnd_);
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        ly += lh;
    }
}

void HexTextWindow::OnMouseMove(int x, int y) {
    if (!selecting_ || bytes_ == nullptr) return;
    const int m = S(kMargin);
    int ly = m + S(kHeaderH) - scrollPos_;
    size_t byteOff = focusByte_;
    for (const Line& l : lines_) {
        const int lh = S(kLineH);
        if (y >= ly && y < ly + lh) {
            const int rel = (x - S(kOffsetColWidthPx)) / S(kHexCellPx);
            byteOff = l.offset + (size_t)(rel < 0 ? 0 : rel);
            break;
        }
        ly += lh;
    }
    if (byteOff > bytes_->size()) byteOff = bytes_->size();
    if (byteOff != focusByte_) {
        focusByte_ = byteOff;
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

LRESULT CALLBACK HexTextWindow::WindowProc(HWND hwnd, UINT msg,
                                           WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<HexTextWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<HexTextWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) self->hwnd_ = hwnd;
    }
    if (self != nullptr) return self->HandleMessage(msg, wParam, lParam);
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT HexTextWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            const HINSTANCE inst = ::GetModuleHandleW(nullptr);
            hwndStatus_ = ::CreateWindowExW(
                0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                0, 0, 0, 0, hwnd_, nullptr, inst, nullptr);
            hwndCopy_ = ::CreateWindowExW(
                0, L"BUTTON", L"&Copy bytes", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 0, 0, hwnd_,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_STREAM_COPY)),
                inst, nullptr);
            hwndSave_ = ::CreateWindowExW(
                0, L"BUTTON", L"&Save as...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 0, 0, hwnd_,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_STREAM_SAVE)),
                inst, nullptr);
            hwndClose_ = ::CreateWindowExW(
                0, L"BUTTON", L"C&lose",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                0, 0, 0, 0, hwnd_,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_STREAM_CLOSE)),
                inst, nullptr);
            ApplyFont(font_);
            return 0;
        }
        case WM_SIZE:
            Layout(LOWORD(lParam), HIWORD(lParam));
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            const UINT dpi = QueryDpiForWindow(hwnd_);
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = ::MulDiv(620, (int)dpi, 96);
            mmi->ptMinTrackSize.y = ::MulDiv(380, (int)dpi, 96);
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* sug = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(hwnd_, nullptr, sug->left, sug->top,
                           sug->right - sug->left, sug->bottom - sug->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            fontsReady_ = false;
            ApplyFont(font_);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                    case IDC_STREAM_COPY: CopySelection(); break;
                    case IDC_STREAM_SAVE: SaveToFile(); break;
                    case IDC_STREAM_CLOSE: ::ShowWindow(hwnd_, SW_HIDE); break;
                    default: break;
                }
            }
            return 0;
        case WM_CLOSE:
            ::ShowWindow(hwnd_, SW_HIDE);
            return 0;
        case WM_DESTROY:
            hwnd_ = nullptr;
            hwndStatus_ = hwndCopy_ = hwndSave_ = hwndClose_ = nullptr;
            fontsReady_ = false;
            return 0;
        case WM_MOUSEWHEEL: {
            const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            ScrollBy(-notches * 3);
            return 0;
        }
        case WM_LBUTTONDOWN: OnMouseDown(GET_X_LPARAM(lParam),
                                         GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE: OnMouseMove(GET_X_LPARAM(lParam),
                                       GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            if (selecting_) { ::ReleaseCapture(); selecting_ = false; }
            return 0;
        case WM_PAINT: OnPaint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            ApplyColors();
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { ::ShowWindow(hwnd_, SW_HIDE); return 0; }
            if (wParam == VK_PRIOR) { ScrollBy(-10); return 0; }
            if (wParam == VK_NEXT) { ScrollBy(10); return 0; }
            break;
        default: break;
    }
    return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

}  // namespace wintcp
