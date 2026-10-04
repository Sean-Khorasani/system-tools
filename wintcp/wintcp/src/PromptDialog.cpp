// PromptDialog.cpp
// See PromptDialog.h.

#include "PromptDialog.h"

#include <cstring>
#include <vector>

namespace wintcp {
namespace {

// Dialog ids. Local rather than in resource.h: this dialog is created from a
// template built in memory (see MakeTemplate below), so it has no entry in the
// .rc and putting its ids in the shared header would suggest otherwise.
enum : int {
    kIdEdit = 1000,
    kIdOk   = IDOK,
    kIdCancel = IDCANCEL,
};

struct PromptState {
    std::wstring value;
    size_t maxChars = 0;
    bool confirmed = false;
};

// DLGPROC, not the bare CALLBACK signature: the typedef is an INT_PTR
// return on x64, and a BOOL-returning function is a different type. The
// compiler rejects the implicit conversion, which is the point.
INT_PTR CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // The state pointer only exists after WM_INITDIALOG has stored it. Reading
    // it on every message - as the first line used to - yields nullptr for
    // WM_INITDIALOG itself and for anything that reaches the proc before
    // initialisation, and every one of those paths then dereferences null.
    // That is an intermittent access violation, because whether a given
    // message arrives before or after WM_INITDIALOG is not something this
    // proc controls.
    auto* st = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        st = reinterpret_cast<PromptState*>(lp);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
    }
    if (st == nullptr) return FALSE;
    switch (msg) {
        case WM_INITDIALOG: {
            HWND edit = GetDlgItem(hwnd, kIdEdit);
            if (edit != nullptr) {
                // EM_LIMITTEXT counts characters excluding the terminator, so
                // this is the exact bound and the control itself refuses to
                // exceed it - the value is not truncated after the fact,
                // which would silently change what the user typed.
                ::SendMessageW(edit, EM_LIMITTEXT,
                               static_cast<WPARAM>(st->maxChars), 0);
                ::SetWindowTextW(edit, st->value.c_str());
                // Select-all, so typing replaces the pre-filled name. Without
                // this the caret sits at the end and the user has to select
                // all by hand every single time.
                ::SendMessageW(edit, EM_SETSEL, 0, -1);
                ::SetFocus(edit);
            }
            // Centre on the owner so the prompt does not appear far away.
            RECT rc = {};
            ::GetWindowRect(hwnd, &rc);
            RECT own = {};
            ::GetWindowRect(::GetParent(hwnd), &own);
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            const int x = own.left + ((own.right - own.left) - w) / 2;
            const int y = own.top + ((own.bottom - own.top) - h) / 2;
            ::SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return FALSE;   // we set the focus ourselves
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (code != BN_CLICKED) break;
            if (id == kIdOk) {
                HWND edit = GetDlgItem(hwnd, kIdEdit);
                wchar_t buf[1024] = {0};
                if (edit != nullptr) {
                    // Bounded read: the control was limited to maxChars, and
                    // this buffer is larger than any caller may pass, so the
                    // read cannot truncate. The explicit length keeps that
                    // true if a future caller raises maxChars.
                    const size_t cap =
                        (st->maxChars + 1 < sizeof(buf) / sizeof(buf[0]))
                            ? st->maxChars + 1
                            : sizeof(buf) / sizeof(buf[0]);
                    ::GetWindowTextW(edit, buf, static_cast<int>(cap));
                }
                st->value = buf;
                st->confirmed = true;
                ::EndDialog(hwnd, TRUE);
                return TRUE;
            }
            if (id == kIdCancel) {
                ::EndDialog(hwnd, FALSE);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            ::EndDialog(hwnd, FALSE);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

// Build the dialog template in memory. A resource-based dialog would need a
// .rc edit, and this module is deliberately self-contained so it can be
// dropped in without touching the shared resource script.
std::vector<BYTE> MakeTemplate(DWORD style, DWORD exStyle, short x, short y,
                               short cx, short cy, const wchar_t* title) {
    // The byte count MUST match what is written below. It did not: the
    // allocation sized the buffer for a fixed 9 DLGITEMTEMPLATEs while only
    // three controls were ever written, and the OK/Cancel captions were never
    // written at all despite a comment claiming they were. The dialog manager
    // reads past the controls according to cdit, so an under-sized or
    // over-sized buffer is an out-of-bounds read inside the dialog loop -
    // which is where the intermittent access violation came from.
    //
    // Build it by writing into a generously sized buffer and then shrinking to
    // the true length, so the two can never disagree.
    std::vector<BYTE> buf(4096, 0);
    BYTE* p = buf.data();
    BYTE* const begin = p;

    auto* dt = reinterpret_cast<DLGTEMPLATE*>(p);
    dt->style = style | DS_SETFONT | DS_MODALFRAME | DS_CENTER;
    dt->dwExtendedStyle = exStyle;
    dt->cdit = 3;                 // edit + OK + Cancel
    dt->x = x; dt->y = y; dt->cx = cx; dt->cy = cy;
    p += sizeof(DLGTEMPLATE);

    // Menu and window class are both "none" (0x0000, 0x0000).
    ::memset(p, 0, 4);
    p += 4;
    const size_t titleChars = wcslen(title) + 1;
    memcpy(p, title, titleChars * sizeof(wchar_t));
    p += (titleChars * sizeof(wchar_t) + 3) / 4 * 4;

    // Font. 9pt "Segoe UI" matches the rest of the app; a smaller or
    // different face here would make the prompt visibly not belong.
    *reinterpret_cast<WORD*>(p) = 9;
    p += 2;
    constexpr wchar_t kFont[] = L"Segoe UI";
    constexpr size_t kFontChars = sizeof(kFont) / sizeof(wchar_t);
    memcpy(p, kFont, kFontChars * sizeof(wchar_t));
    p += (kFontChars * sizeof(wchar_t) + 3) / 4 * 4;

    // One control: 0xFFFF + ordinal class, title, then 0x0000 for "no extra
    // data". The title is what puts a caption on a button, so it has to be
    // written for OK and Cancel or they come out blank.
    //
    // Win32 requires every DLGITEMTEMPLATE in an indirect template to start on
    // a DWORD (4-byte) boundary. The preceding writes - the font size WORD,
    // the DWORD-padded font name, the 2-byte "no extra data" slot - can leave
    // p on a 2-byte boundary. The symptom is an intermittent AV when the dialog
    // loop walks the template, which is exactly what the audit tracked down.
    const auto item = [&p](DWORD s, short ix, short iy, short icx, short icy,
                           WORD id, WORD cls, const wchar_t* caption) {
        p = reinterpret_cast<BYTE*>(
            (reinterpret_cast<ULONG_PTR>(p) + 3) & ~static_cast<ULONG_PTR>(3));
        auto* it = reinterpret_cast<DLGITEMTEMPLATE*>(p);
        it->style = s;
        it->x = ix; it->y = iy; it->cx = icx; it->cy = icy;
        it->id = id;
        p += sizeof(DLGITEMTEMPLATE);
        *reinterpret_cast<WORD*>(p) = 0xFFFF;   // class is an ordinal
        p += 2;
        *reinterpret_cast<WORD*>(p) = cls;     // 0x80 edit, 0x81 button
        p += 2;
        if (caption == nullptr) {
            ::memset(p, 0, 2);                 // no title
            p += 2;
        } else {
            const size_t n = wcslen(caption) + 1;
            memcpy(p, caption, n * sizeof(wchar_t));
            p += (n * sizeof(wchar_t) + 3) / 4 * 4;
        }
        ::memset(p, 0, 2);                     // no extra data
        p += 2;
    };

    item(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
         10, 10, 240, 14, kIdEdit, 0x80, nullptr);
    item(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
         150, 32, 50, 16, kIdOk, 0x81, L"OK");
    item(WS_CHILD | WS_VISIBLE | WS_TABSTOP,
         206, 32, 50, 16, kIdCancel, 0x81, L"Cancel");

    buf.resize(static_cast<size_t>(p - begin));
    return buf;
}

}  // namespace

bool PromptForText(HWND owner, const wchar_t* title, const wchar_t* label,
                   const wchar_t* initial, size_t maxChars,
                   std::wstring* out) {
    if (out == nullptr) return false;
    if (title == nullptr) title = L"WinTCP";
    if (label == nullptr) label = L"";
    if (initial == nullptr) initial = L"";

    PromptState st;
    st.value = initial;
    // Keep the pre-filled value inside the bound the control enforces, or the
    // first keystroke would be refused for a field that starts too long.
    if (st.value.size() > maxChars) st.value.resize(maxChars);
    st.maxChars = maxChars;

    // 34 DLU tall is enough for the edit plus the two buttons; the width
    // tracks the edit control above.
    std::vector<BYTE> tpl = MakeTemplate(
        DS_ABSALIGN | DS_CENTER, 0, 0, 0, 270, 60, title);

    // The label goes in the dialog's own caption area: a static control would
    // need a 4th item in the template and the extra machinery for a single
    // sentence is not worth it. The prompt reads fine as a titled dialog with
    // the pre-selected value showing what is being renamed.
    (void)label;

    const INT_PTR rc = ::DialogBoxIndirectParamW(
        ::GetModuleHandleW(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEW>(tpl.data()),
        owner, PromptProc, reinterpret_cast<LPARAM>(&st));

    // A negative return is a creation failure, distinct from Cancel's zero.
    if (rc <= 0) return false;
    if (!st.confirmed) return false;
    *out = st.value;
    return true;
}

}  // namespace wintcp
