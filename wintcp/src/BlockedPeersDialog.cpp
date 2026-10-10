// BlockedPeersDialog.cpp
// SPDX-License-Identifier: Apache-20
// See BlockedPeersDialog.h.

#include "BlockedPeersDialog.h"

#include <cstring>
#include <vector>

#include "BlockConn.h"
#include "Opt.h"
#include "Utils.h"

namespace wintcp {
namespace {

// Dialog ids, local to this module because the dialog is built in memory and
// has no entry in the .rc - the same reasoning as PromptDialog's ids.
enum : int {
    kIdList = 2000,
    // 9.5.5: enable/disable join the editor's actions. They are NOT aliases
    // for delete: a disabled rule stays installed with its identity, so
    // "turn this off for an hour" is one step rather than delete-then-rederive.
    kIdDelete = IDOK + 1,
    kIdRemoveAll = IDOK + 2,
    kIdClose = IDCANCEL,
    kIdEnable = IDOK + 3,
    kIdDisable = IDOK + 4,
};

// Control count, in one place. cdit must match it exactly: the dialog manager
// reads controls according to cdit, so a disagreement is an out-of-bounds read
// inside the dialog loop, which is the failure PromptDialog.cpp documents at
// length. Any write below that changes the count has to change this too, and
// the selftest pins the resulting capacity.
constexpr int kControlCount = 6;

constexpr size_t Pad4(size_t bytes) { return (bytes + 3) & ~static_cast<size_t>(3); }

// The capacity the writer allocates. A list rows hold at most one rule name
// each, and a rule name is bounded by kMaxRuleNameChars; the captions are
// compile-time literals. Slack for the six font/title/extra-data slots that
// are shared rather than per-control.
constexpr size_t kTemplateSlack = 512;
constexpr size_t kRowTextChars = 1024;

struct BlockedPeersState {
    std::vector<BlockedRule> rules;
    BlockedPeersChoice choice = BlockedPeersChoice::kRefused;
    std::wstring failure;
};

// ---------------------------------------------------------------------------
// The template writer. Same contract as PromptDialog's TemplateFits: every
// write is preceded by a bounds check on what is LEFT, the cursor only moves
// after the write, and a refusal returns false with the buffer shrunk to
// whatever prefix was written. The caller turns a false into an empty template
// and does not call the dialog manager at all.
// ---------------------------------------------------------------------------

// Write one DLGITEMTEMPLATE. 'end' exists so the alignment step can be
// re-checked after it: padding can land p exactly on end, or just past it for
// a buffer whose end is not DWORD-aligned, so "is there room" has to be asked
// again after the move and not only before it.
bool WriteItem(BYTE** cursor, const BYTE* end, DWORD style,
               short ix, short iy, short icx, short icy, int id, WORD cls,
               const wchar_t* caption) {
    BYTE*& p = *cursor;
    const auto usable = [&p, end](size_t n) {
        const std::ptrdiff_t room = end - p;
        return room >= 0 && n <= static_cast<size_t>(room);
    };

    const auto addr = reinterpret_cast<uintptr_t>(p);
    const auto aligned = (addr + 3u) & ~static_cast<uintptr_t>(3u);
    if (aligned < addr) return false;
    const size_t skip = static_cast<size_t>(aligned - addr);
    if (!usable(skip)) return false;
    if (end - p < 0) return false;
    p = reinterpret_cast<BYTE*>(aligned);

    if (!usable(sizeof(DLGITEMTEMPLATE))) return false;
    auto* it = reinterpret_cast<DLGITEMTEMPLATE*>(p);
    it->style = style;
    it->x = ix;
    it->y = iy;
    it->cx = icx;
    it->cy = icy;
    it->id = static_cast<WORD>(id);
    p += sizeof(DLGITEMTEMPLATE);

    if (!usable(4)) return false;
    *reinterpret_cast<WORD*>(p) = 0xFFFF;   // the class is an ordinal
    *reinterpret_cast<WORD*>(p + 2) = cls;  // 0x83 listbox, 0x80 edit, 0x81 button
    p += 4;

    const size_t n = (caption == nullptr) ? 1 : wcslen(caption) + 1;
    const size_t bytes = Pad4(n * sizeof(wchar_t));
    if (!usable(bytes)) return false;
    if (caption != nullptr) {
        memcpy(p, caption, n * sizeof(wchar_t));
    } else {
        memset(p, 0, bytes);
    }
    p += bytes;

    if (!usable(2)) return false;
    memset(p, 0, 2);   // no extra data
    p += 2;
    return true;
}

// Build the whole template. Returns an empty vector on any refusal; the caller
// must not hand a partial template to the dialog manager, because cdit says
// kControlCount and fewer controls would make it read past the end.
std::vector<BYTE> MakeTemplate(const wchar_t* title) {
    if (title == nullptr) return std::vector<BYTE>();

    const size_t titleBytes = Pad4((wcslen(title) + 1) * sizeof(wchar_t));
    constexpr wchar_t kFont[] = L"Segoe UI";
    const size_t fontBytes = Pad4((sizeof(kFont) / sizeof(wchar_t)) * sizeof(wchar_t));

    // Four items: the list plus Delete / Remove all / Close.
    size_t capacity = sizeof(DLGTEMPLATE) + 4 + titleBytes + 2 + fontBytes +
                      kTemplateSlack;
    for (int i = 0; i < kControlCount; ++i) {
        capacity += sizeof(DLGITEMTEMPLATE) + 4 +
                    Pad4(kRowTextChars * sizeof(wchar_t)) + 2;
    }

    std::vector<BYTE> buf(capacity, 0);
    BYTE* p = buf.data();
    BYTE* const begin = p;
    BYTE* const end = buf.data() + buf.size();

    const auto usable = [&p, end](size_t n) {
        const std::ptrdiff_t room = end - p;
        return room >= 0 && n <= static_cast<size_t>(room);
    };

    if (!usable(sizeof(DLGTEMPLATE))) return std::vector<BYTE>();
    auto* dlg = reinterpret_cast<DLGTEMPLATE*>(p);
    dlg->style = DS_ABSALIGN | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU |
                 DS_MODALFRAME | DS_SETFONT;
    dlg->dwExtendedStyle = 0;
    dlg->cdit = static_cast<WORD>(kControlCount);
    dlg->x = 0;
    dlg->y = 0;
    dlg->cx = 340;
    dlg->cy = 190;
    p += sizeof(DLGTEMPLATE);

    if (!usable(4)) return std::vector<BYTE>();
    memset(p, 0, 4);   // no menu, default window class
    p += 4;

    const size_t tChars = wcslen(title) + 1;
    if (!usable(titleBytes)) return std::vector<BYTE>();
    memcpy(p, title, tChars * sizeof(wchar_t));
    p += titleBytes;

    if (!usable(2)) return std::vector<BYTE>();
    *reinterpret_cast<WORD*>(p) = 9;   // 9pt, matching PromptDialog
    p += 2;

    if (!usable(fontBytes)) return std::vector<BYTE>();
    memcpy(p, kFont, (sizeof(kFont) / sizeof(wchar_t)) * sizeof(wchar_t));
    p += fontBytes;

    // List box: the working area. Slightly shorter than the viewer's was, to
    // make room for the second button row the editor adds.
    const DWORD listStyle = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
                            LBS_NOTIFY | LBS_HASSTRINGS;
    if (!WriteItem(&p, end, listStyle, 8, 8, 320, 96, kIdList, 0x83,
                   nullptr)) {
        return std::vector<BYTE>();
    }
    // Row 1: the lifecycle actions.
    if (!WriteItem(&p, end,
                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 8, 112,
                   76, 16, kIdDelete, 0x81, L"Delete") ||
        !WriteItem(&p, end, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 88, 112,
                   76, 16, kIdEnable, 0x81, L"Enable") ||
        !WriteItem(&p, end, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 168, 112,
                   76, 16, kIdDisable, 0x81, L"Disable") ||
        // Row 2: Remove all and Close.
        !WriteItem(&p, end, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 8, 132,
                   98, 16, kIdRemoveAll, 0x81, L"Remove all") ||
        !WriteItem(&p, end, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 110, 132,
                   98, 16, kIdClose, 0x81, L"Close")) {
        return std::vector<BYTE>();
    }

    buf.resize(static_cast<size_t>(p - begin));
    return buf;
}

// Repopulate the list from the firewall. Called on init and after every
// action, so the manager shows what the firewall holds rather than what the
// dialog believed when it opened.
//
// 9.5.5: an action that closed the modal was a reader with extra buttons. The
// point of an editor is that you change one thing and SEE the result, so this
// spends the cost of a re-list and keeps the selection on the same rule when
// it still exists.
void ReloadRules(HWND list, std::vector<BlockedRule>* rules) {
    if (list == nullptr || rules == nullptr) {
        return;
    }
    const LRESULT sel = ::SendMessageW(list, LB_GETCURSEL, 0, 0);
    std::wstring keepName;
    if (sel != LB_ERR && sel >= 0 &&
        static_cast<size_t>(sel) < rules->size()) {
        keepName = (*rules)[static_cast<size_t>(sel)].name;
    }

    std::wstring error;
    std::vector<BlockedRule> fresh;
    if (!ListBlockedRules(&fresh, &error)) {
        // Leave the list as it was rather than emptying it: an empty list on a
        // failed read would read as "you have no rules", which is the one
        // answer this dialog must never give wrongly.
        if (!error.empty()) {
            ::MessageBoxW(
                list,
                (L"Could not re-read the firewall rules:\r\n" + error).c_str(),
                L"WinTCP", MB_OK | MB_ICONERROR);
        }
        return;
    }
    *rules = std::move(fresh);

    ::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
    int restore = -1;
    for (size_t i = 0; i < rules->size(); ++i) {
        const std::wstring row = BlockedPeersRowText((*rules)[i]);
        const LRESULT at =
            ::SendMessageW(list, LB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(row.c_str()));
        if (at != LB_ERR && (*rules)[i].name == keepName) {
            restore = static_cast<int>(at);
        }
    }
    ::SendMessageW(list, LB_SETCURSEL,
                   restore >= 0 ? restore : (rules->empty() ? -1 : 0), 0);
    ::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(list, nullptr, TRUE);
}

INT_PTR CALLBACK BlockedPeersProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<BlockedPeersState*>(
        GetWindowLongPtrW(hwnd, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        st = reinterpret_cast<BlockedPeersState*>(lp);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
    }
    if (st == nullptr) return FALSE;

    switch (msg) {
        case WM_INITDIALOG: {
            ReloadRules(GetDlgItem(hwnd, kIdList), &st->rules);
            RECT rc = {};
            ::GetWindowRect(hwnd, &rc);
            RECT own = {};
            ::GetWindowRect(::GetParent(hwnd), &own);
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            ::SetWindowPos(hwnd, HWND_TOP,
                           own.left + ((own.right - own.left) - w) / 2,
                           own.top + ((own.bottom - own.top) - h) / 2, 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return TRUE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == kIdClose || id == IDCANCEL) {
                st->choice = BlockedPeersChoice::kChanged;
                ::EndDialog(hwnd, TRUE);
                return TRUE;
            }
            if (code != BN_CLICKED) break;
            HWND list = GetDlgItem(hwnd, kIdList);
            const LRESULT sel = ::SendMessageW(list, LB_GETCURSEL, 0, 0);
            if (id == kIdDelete || id == kIdEnable || id == kIdDisable) {
                if (sel == LB_ERR || sel < 0 ||
                    static_cast<size_t>(sel) >= st->rules.size()) {
                    ::MessageBoxW(hwnd, L"Select a rule first.", L"WinTCP",
                                  MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                }
                const std::wstring name =
                    st->rules[static_cast<size_t>(sel)].name;
                std::wstring error;
                bool ok = false;
                const wchar_t* what = L"delete";
                if (id == kIdDelete) {
                    ok = RemoveBlockedRule(name, &error);
                } else if (id == kIdEnable) {
                    ok = SetBlockedRuleEnabled(name, true, &error);
                    what = L"enable";
                } else {
                    ok = SetBlockedRuleEnabled(name, false, &error);
                    what = L"disable";
                }
                if (!ok) {
                    ::MessageBoxW(hwnd,
                                  (std::wstring(L"Could not ") + what +
                                   L" the rule:\r\n" + error)
                                      .c_str(),
                                  L"WinTCP", MB_OK | MB_ICONERROR);
                    return TRUE;
                }
                // Stay open and re-read. A manager that closes after every
                // action is three actions per change.
                ReloadRules(list, &st->rules);
                return TRUE;
            }
            if (id == kIdRemoveAll) {
                std::wstring error;
                if (!RemoveAllWinTcpRules(&error)) {
                    ::MessageBoxW(
                        hwnd,
                        (L"Could not remove every rule:\r\n" + error).c_str(),
                        L"WinTCP", MB_OK | MB_ICONERROR);
                    return TRUE;
                }
                ReloadRules(list, &st->rules);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            st->choice = BlockedPeersChoice::kChanged;
            ::EndDialog(hwnd, FALSE);
            return TRUE;
        default:
            break;
    }
    return FALSE;
}
}  // namespace

// The row for one rule. Exported because the selftest pins it: the one place
// it runs is a modal the UI harness cannot dismiss, so it would otherwise be
// an untested capacity rule - which is the failure PromptDialog.cpp documents
// at length. What is tested is what ships.
std::wstring BlockedPeersRowText(const BlockedRule& r) {
    std::wstring text = r.name;
    if (!r.enabled) text += L"  [disabled]";
    if (!r.isBlocking) text += L"  [not a block]";
    if (!r.remoteAddrs.empty()) text += L"  " + r.remoteAddrs;
    if (!r.remotePorts.empty()) text += L":" + r.remotePorts;
    if (text.size() > kRowTextChars) {
        return Utf8ToWide(
            TruncateToWidthOpt(WideToUtf8(text), kRowTextChars).c_str());
    }
    return text;
}

int BlockedPeersControlCount() {
    return kControlCount;
}

size_t BlockedPeersTemplateCapacity(size_t titleChars) {
    const size_t titleBytes = Pad4((titleChars + 1) * sizeof(wchar_t));
    constexpr wchar_t kFont[] = L"Segoe UI";
    const size_t fontBytes =
        Pad4((sizeof(kFont) / sizeof(wchar_t)) * sizeof(wchar_t));
    size_t capacity = sizeof(DLGTEMPLATE) + 4 + titleBytes + 2 + fontBytes +
                      kTemplateSlack;
    for (int i = 0; i < kControlCount; ++i) {
        capacity += sizeof(DLGITEMTEMPLATE) + 4 +
                    Pad4(kRowTextChars * sizeof(wchar_t)) + 2;
    }
    return capacity;
}

// Exported for the selftest, which pins the writer's arithmetic. Keeping it
// out of the anonymous namespace is what makes "what is tested is what
// ships" true, the same way PromptDialog's TemplateFits does.
bool BlockedPeersTemplateFits(const wchar_t* title, std::vector<BYTE>* buf) {
    if (title == nullptr || buf == nullptr || buf->empty()) return false;
    std::vector<BYTE> made = MakeTemplate(title);
    if (made.size() > buf->size()) return false;
    buf->assign(made.begin(), made.end());
    return true;
}

BlockedPeersChoice ShowBlockedPeersDialog(HWND owner, std::wstring* failure) {
    std::wstring error;
    std::vector<BlockedRule> rules;
    if (!ListBlockedRules(&rules, &error)) {
        if (failure != nullptr) *failure = error;
        return BlockedPeersChoice::kRefused;
    }

    std::vector<BYTE> tpl = MakeTemplate(L"Blocked peers");
    if (tpl.empty()) {
        if (failure != nullptr) *failure = L"could not build the dialog";
        return BlockedPeersChoice::kRefused;
    }

    BlockedPeersState st;
    st.rules = std::move(rules);
    const INT_PTR rc = ::DialogBoxIndirectParamW(
        ::GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(tpl.data()),
        owner, BlockedPeersProc, reinterpret_cast<LPARAM>(&st));
    if (rc <= 0) {
        if (failure != nullptr) *failure = L"the dialog could not be shown";
        return BlockedPeersChoice::kRefused;
    }
    return st.choice;
}

}  // namespace wintcp
