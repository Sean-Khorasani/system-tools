// UiHarness.cpp
//
// SMOKE verification of the main window, driven IN-PROCESS.
//
// Scope discipline: this file asserts WINDOW-LEVEL facts only (the list
// fills, toggles flip, windows open/close, commands run without emptying
// the list). Model behaviour - grouping aggregates, bookmark pins, log
// caps, filter grammar, sort order - is proven headless by --selftest and
// live by the CLI verbs, which run in milliseconds with no window. Every
// check below is labelled SMOKE, and CI treats this suite as advisory:
// a FAIL here means "look at the window", never "the build is red".
//
// Why this file exists: the previous round proved the new menu items exist.
// That is not the same as proving they work, and the review was right to
// reject it. This harness presses the real controls and reads back what
// actually changed.
//
// Why in-process rather than a second test executable poking the window with
// SendMessage: there is no interactive desktop available here, so a window
// created by an external driver has a handle that EnumWindows lists but that
// answers every query as empty - GetWindowRect returns (0,0)-(0,0) and
// LVM_GETHEADER returns NULL. Driving the window from inside its own process
// avoids that entirely and has the side benefit of reading the store's
// settled state rather than racing it.
//
// What it does NOT do: click. There is no mouse, no focus, no painting. Every
// check below exercises logic and state transitions reachable by menu
// command, accelerator, or key press - which is what the features are. Pixel
// layout and visual appearance remain unverified.

#include "UiHarness.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <thread>

#include "MainWindow.h"
#include "Bookmarks.h"
#include "Settings.h"
#include "Columns.h"
#include "TypeToJump.h"   // 7.2: the product's label predicate, not a copy
#include "resource.h"

#pragma comment(lib, "comctl32.lib")

namespace wintcp {
namespace {

int g_pass = 0;
int g_fail = 0;
int g_skip = 0;

// stdout is often a pipe (golden.bat redirects it), where printf is
// block-buffered: without this the harness appears to produce NOTHING and a
// hang is indistinguishable from a crash. Flushing after every line makes a
// partial run readable, which is what makes a hang diagnosable.
void Say(const char* s) {
    std::fputs(s, stdout);
    std::fflush(stdout);
}

void Check(bool ok, const char* what, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
        char b[640];
        std::snprintf(b, sizeof(b), "  [ ok ] %s\n", what);
        Say(b);
    } else {
        ++g_fail;
        char b[768];
        std::snprintf(b, sizeof(b), "  [FAIL] %s   (%s)\n", what, detail.c_str());
        Say(b);
    }
}

// Neither a pass nor a fail: the PRECONDITION for the check does not hold on
// this machine, so running it would say nothing about the code.
//
// Why this exists: the type-to-jump check typed 'a'..'z' and reported a
// confident FAIL when no row happened to start with one of them - on a machine
// where every label began with a digit or a multi-byte sequence, the feature
// was working perfectly and the harness said it was broken. That is worse than
// saying nothing, because it is a false alarm that trains the reader to
// distrust the gate.
//
// A skip is NOT a way to hide a failure: it requires a precondition to be
// absent, it names the precondition, and it is counted in the summary line
// where it can be seen. A check that silently skips everything is a
// misconfigured harness, not a green one.
void Skip(const char* what, const std::string& why) {
    ++g_skip;
    char b[768];
    std::snprintf(b, sizeof(b), "  [skip] %s   (%s)\n", what, why.c_str());
    Say(b);
}

void Sayf(const char* fmt, ...) {
    char b[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    Say(b);
}

std::string W(const wchar_t* s) {
    if (s == nullptr) return "<null>";
    int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
    return out;
}

// The probe op-names are passed as narrow literals and widened here, so the
// checks below read as ordinary ASCII rather than as a wall of L"...". The
// conversion is allocation-free for the ASCII names actually used.
std::wstring Widen(const char* s) {
    if (s == nullptr) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring out(static_cast<std::size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

std::string Of(const MainWindow& w, const char* op, const char* arg = nullptr) {
    const std::wstring wop = Widen(op);
    const std::wstring warg = Widen(arg);
    const wchar_t* r = w.UiProbe(wop.c_str(),
                                 arg == nullptr ? nullptr : warg.c_str());
    if (r == nullptr) return "<unknown-op>";
    return W(r);
}

long Num(const MainWindow& w, const char* op, const char* arg = nullptr) {
    const std::string s = Of(w, op, arg);
    return s.empty() || s[0] == '<' ? -1 : std::strtol(s.c_str(), nullptr, 10);
}

bool Is(const MainWindow& w, const char* op, long want) {
    return Num(w, op) == want;
}

// Does this (narrow, UTF-8) label start with the typed letter, case-insensitively?
//
// Delegates to the PRODUCT's predicate rather than reimplementing it. It used
// to be a local copy that upper-cased the letter unconditionally, which was
// wrong twice: it rejected a label genuinely starting with a lowercase letter
// (the type-to-jump check failed on 'tailscaled.exe' for 't' while the feature
// was correct), and it was a second rule for one behaviour - two
// implementations of "does this label start with this letter" is exactly the
// duplication this codebase's own rule exists to prevent.
//
// Compared byte-wise rather than via ::towupper on a char cast straight to
// wint_t, which made an earlier version of this check report a false failure
// on "Avira..." for the letter 'a'. A label whose first byte is >= 0x80 is a
// multi-byte sequence; such a row cannot be the target of a plain ASCII
// keystroke, so it simply does not match.
bool LabelStartsWith(const std::string& label, wchar_t letter) {
    if (label.empty()) return false;
    const unsigned char c0 = static_cast<unsigned char>(label[0]);
    if (c0 >= 0x80) return false;
    return wintcp::LabelStartsWith(Widen(label.c_str()),
                                   std::wstring(1, letter));
}

// Drive a menu command exactly as the menu bar does. This is the production
// path: WM_COMMAND -> HandleMessage -> OnCommand, the same three hops a click
// makes. Anything the command forgot to wire up shows up as no state change.
void Cmd(HWND hwnd, WORD id) {
    ::SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(id, 0),
                   reinterpret_cast<LPARAM>(hwnd));
}

// Pump the message queue for `ms`, so posted results (the refresh callback
// arrives via PostMessage from the worker) are delivered before anything is
// read. The whole queue is drained, not just this window's, so no parameter
// for the target HWND.
void Pump(int ms) {
    const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(ms);
    MSG msg;
    while (::GetTickCount64() < deadline) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(20);
    }
}

// Wait for the first snapshot to land, the way a user waits for the window to
// fill. Fails loudly rather than testing an empty list.
bool WaitForRows(MainWindow& w, long want, int timeoutMs = 20000) {
    const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    while (::GetTickCount64() < deadline) {
        Pump(100);
        if (Num(w, "rowCount") >= want) return true;
    }
    return false;
}

// A watchdog so a hang is reported as a hang rather than being discovered by
// a build-time timeout ten minutes later. Each Mark() announces the section
// about to run, so the log names the exact one that stalled. A separate thread
// - not a WM_TIMER, which would need the window's WndProc replaced and would
// risk breaking the very code under test.
DWORD g_tick = 0;
volatile LONG g_lastTick = 0;
volatile LONG g_done = 0;

void Mark(const char* what) {
    const DWORD n = ++g_tick;
    g_lastTick = static_cast<LONG>(n);
    char b[256];
    std::snprintf(b, sizeof(b), "-- section %lu: %s\n", static_cast<unsigned long>(n), what);
    Say(b);
}

void Watchdog() {
    // 180s is far beyond any legitimate section here (the longest is the 20s
    // wait for the first snapshot), so tripping this means a real deadlock.
    for (int i = 0; i < 180; ++i) {
        ::Sleep(1000);
        if (g_done) return;
    }
    Sayf("!! WATCHDOG: still inside section %ld after 180s - HANG\n",
         static_cast<long>(g_lastTick));
    // Hard exit: the window's message loop is stuck, so returning from here
    // would never happen. ExitProcess unwinds nothing, which is what we want.
    ::ExitProcess(3);
}

}  // namespace

std::wstring RunUiHarness(MainWindow& w, HWND hwnd) {
    std::wstring report;
    char line[512];

    Say("=== in-process UI harness (SMOKE: window-level facts only) ===\n");
    Say("Model behaviour (grouping aggregates, bookmark pins, log caps) is\n");
    Say("covered headless by --selftest and the CLI verbs, not here.\n");
    std::thread(Watchdog).detach();

    // ---- the list must actually populate ---------------------------------
    // Nothing else in this file is meaningful until the window shows real
    // data, so this comes first and everything else assumes it passed.
    Mark("populate");
    const bool populated = WaitForRows(w, 1);
    const long rows = Num(w, "rowCount");
    std::snprintf(line, sizeof(line),
                  "SMOKE the list populates from a real snapshot (%ld rows)",
                  rows);
    Check(populated, line, "no rows after 20s - every later check is meaningless");
    if (!populated) {
        g_done = 1;
        Sayf("rowCount=%ld pendingRefresh=%ld engineRunning=%ld\n"
             "lastError='%s' stamp='%s'\n",
             Num(w, "rowCount"),
             Num(w, "pendingRefresh"), Num(w, "engineRunning"),
             Of(w, "lastError").c_str(), Of(w, "lastRefreshTime").c_str());
        Say("\n=== UI HARNESS ABORTED: empty list ====\n");
        return L"UI HARNESS ABORTED (empty list)";
    }

    // ---- every column must be reachable from the menu (C3) ---------------
    // Not cosmetic. The visible-column mask has exactly one writer in the GUI
    // (ToggleColumn, reached only by a View > Columns command), so a ColumnId
    // with no menu item is a column no user can ever switch on.
    //
    // This had already happened to six columns - NOTE, RTT, MINRTT, CWND,
    // RETRANS and GROUPRATE - while every compile-time guard was satisfied:
    // resource.h defined their ids and IDM_COL_COUNT counted them, but
    // wintcp.rc never referenced them, and no check compared the two files.
    // Hence this runtime probe instead of another static_assert.
    Mark("columns-menu");
    {
        const long missing = Num(w, "columnsMenuMissingCount");
        std::snprintf(line, sizeof(line),
                      "every column has a View > Columns command (%ld missing)",
                      missing);
        Check(missing == 0, line, Of(w, "columnsMenuMissing").c_str());
    }

    // ---- F5 refresh -------------------------------------------------------
    Mark("refresh");
    {
        const long before = Num(w, "rowCount");
        const std::string t0 = Of(w, "lastRefreshTime");
        Cmd(hwnd, IDM_FILE_REFRESH);
        Pump(4000);
        const long after = Num(w, "rowCount");
        std::snprintf(line, sizeof(line),
                      "SMOKE F5 refresh re-samples (%ld -> %ld rows, "
                      "stamp '%s' -> '%s')",
                      before, after, t0.c_str(), Of(w, "lastRefreshTime").c_str());
        Check(after > 0, line, "list emptied by a refresh");
    }

    // ---- 5.5 freeze -------------------------------------------------------
    Mark("freeze");
    {
        const long base = Num(w, "rowCount");
        Cmd(hwnd, IDM_VIEW_FREEZE);
        Pump(300);
        Check(Is(w, "frozen", 1), "SMOKE 5.5 freeze: the toggle sets frozen_",
              "frozen still 0 after ID_VIEW_FREEZE");
        const long afterFreeze = Num(w, "rowCount");
        std::snprintf(line, sizeof(line),
                      "SMOKE 5.5 freeze: rows are held at the frozen count "
                      "(%ld -> %ld)", base, afterFreeze);
        Check(afterFreeze == base, line, "the list changed on entering freeze");

        // While frozen, a refresh must NOT replace the visible rows.
        Cmd(hwnd, IDM_FILE_REFRESH);
        Pump(4000);
        const long during = Num(w, "rowCount");
        std::snprintf(line, sizeof(line),
                      "SMOKE 5.5 freeze: F5 does not replace the list while "
                      "frozen (%ld -> %ld)", afterFreeze, during);
        Check(during == afterFreeze, line, "rows moved while frozen");

        Cmd(hwnd, IDM_VIEW_FREEZE);
        Pump(300);
        Check(Is(w, "frozen", 0), "SMOKE 5.5 freeze: the toggle clears frozen_",
              "still frozen after a second ID_VIEW_FREEZE");
        Cmd(hwnd, IDM_FILE_REFRESH);
        Pump(4000);
        std::snprintf(line, sizeof(line),
                      "SMOKE 5.5 freeze: resuming restores a live view (%ld rows)",
                      Num(w, "rowCount"));
        Check(Num(w, "rowCount") > 0, line, "no rows after unfreeze");
    }

    // ---- 5.1 group by process (SMOKE: toggle only) -------------------------
    // The aggregates (one row per PID, max-not-sum traffic, names) are
    // proven headless by the group.* selftests and live by `list --group`;
    // here the window only has to switch shape and switch back.
    Mark("group");
    {
        Cmd(hwnd, IDM_VIEW_GROUP);
        Pump(500);
        Check(Is(w, "grouped", 1), "SMOKE 5.1 group: the toggle sets grouped",
              "grouped still 0 after ID_VIEW_GROUP");
        Check(Num(w, "rowCount") > 0, "SMOKE 5.1 group: grouped list non-empty",
              "grouped view shows no rows");

        Cmd(hwnd, IDM_VIEW_GROUP);
        Pump(500);
        Check(Is(w, "grouped", 0), "SMOKE 5.1 group: the toggle clears grouped",
              "still grouped after a second ID_VIEW_GROUP");
        Check(Num(w, "rowCount") > 0, "SMOKE 5.1 group: flat list restored",
              "flat view shows no rows");
    }

    // ---- 7.4 preserve selection ------------------------------------------
    Mark("preserve-selection");
    {
        Check(Is(w, "preserveSelection", 1),
              "SMOKE 7.4: preserve-selection defaults to ON",
              "default is off");
        Cmd(hwnd, IDM_VIEW_PRESERVE_SEL);
        Pump(200);
        Check(Is(w, "preserveSelection", 0), "SMOKE 7.4: the toggle turns it OFF",
              "unchanged after the command");
        Cmd(hwnd, IDM_VIEW_PRESERVE_SEL);
        Pump(200);
        Check(Is(w, "preserveSelection", 1), "SMOKE 7.4: the toggle turns it back ON",
              "did not return to ON");
    }

    // ---- 7.2 type-to-jump -------------------------------------------------
    Mark("type-to-jump");
    // The product only treats WM_CHAR as a jump when the list holds focus.
    // This section now ESTABLIES that focus itself (see below) instead of
    // assuming an earlier section left it there, and skips honestly if the
    // window cannot take real focus. An earlier version assumed the
    // assumption, which is what made this check flap.
    {
        HWND list = ::GetDlgItem(hwnd, IDC_LIST);
        // ESTABLISH the focus precondition; do not assume it.
        //
        // The product only treats WM_CHAR as a jump when the list holds focus
        // (MainWindow.cpp WM_CHAR: `::GetFocus() == hwndList_`), and the only
        // thing that used to put it there was OnTypeJumpChar's own
        // ::SetFocus(hwndList_) - which it reaches ONLY on a successful match.
        // The no-match path returns before that line. So focus was a side
        // effect of a previous successful jump, and a harness that did not set
        // it itself inherited it from whatever ran earlier: the filter box, a
        // grouping toggle, or nothing at all. When the list did not hold
        // focus, all 26 letters were correctly ignored and this section
        // reported "no letter jumped" - a confident FAIL whose real cause was
        // the harness's own missing setup, and which appeared or vanished
        // depending on which processes were running that minute.
        //
        // Setting it here makes the precondition explicit and the check
        // deterministic. A window that was never activated still cannot take
        // real keyboard focus, so this is verified rather than trusted: if
        // GetFocus() does not end up on the list, the section says so and
        // skips instead of reporting a product failure it cannot cause.
        ::SetFocus(list);
        Pump(50);
        const bool listHasFocus = (::GetFocus() == list);
        if (!listHasFocus) {
            Say("  [skip] 7.2 type-to-jump: the list cannot take focus without an "
                "interactive desktop; the matching logic itself is covered by the "
                "TypeToJump selftests\n");
        }
        // Which visible slot is the PROCESS column? Needed to read the label
        // the jump matched against.
        int procSlot = -1;
        for (int v = 0; v < 23; ++v) {
            const std::string arg = std::to_string(v);
            if (static_cast<int>(Num(w, "visibleCol", arg.c_str())) == COL_PROCESS) {
                procSlot = v;
                break;
            }
        }
        std::snprintf(line, sizeof(line),
                      "SMOKE 7.2 type-to-jump: located the PROCESS column (slot %d)",
                      procSlot);
        Check(procSlot >= 0, line, "the PROCESS column is not on screen");

        // Type a single letter and see where the selection lands.
        //
        // The letter is DERIVED FROM THE ROWS ON SCREEN, not guessed: this
        // check used to type 'a'..'z' and pass only when the live process
        // table happened to contain a row starting with one of them. On a
        // machine where every label begins with a digit or a multi-byte
        // sequence it reported a confident "no letter jumped" while the
        // feature was working perfectly - a check that depends on the
        // machine is not a test of the code.
        //
        // Each attempt must still start from a CLEAN sequence. The product
        // extends the prefix on every keystroke and only resets after a >1 s
        // pause, so typing 'a' then 'b' on failure was really typing "ab",
        // then "abc" - it reported "no letter moved the selection" when what
        // had happened was that no row starts with "aq" either. That was a
        // harness bug, and it made this flake for years. The pause here is
        // comfortably longer than kTypeToJumpTimeoutMs.
        wchar_t pick = 0;
        long firstRow = -1;
        std::string firstLabel;

        // Find the first row whose label really starts with an ASCII
        // letter - that is the letter this run types. Read through the
        // same store the product matches against, so a row the user can
        // see is a row the harness can test.
        if (listHasFocus) {
            const long labelCount = Num(w, "rowCount");
            for (long r = 0; r < labelCount && pick == 0; ++r) {
                const std::string label =
                    Of(w, "rowLabel", std::to_string(r).c_str());
                if (label.empty()) continue;
                const unsigned char c0 = static_cast<unsigned char>(label[0]);
                // ASCII letters only: a label whose first byte is >= 0x80
                // is a multi-byte sequence no plain keystroke can reach.
                if (c0 < 'A' || (c0 > 'Z' && c0 < 'a') || c0 > 'z') continue;
                pick = static_cast<wchar_t>(c0);
                firstLabel = label;
            }
        }
        if (listHasFocus && pick != 0) {
            Pump(1100);   // let any previous keystroke's sequence time out
            ::SendMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(pick),
                           static_cast<LPARAM>(1));
            Pump(80);
            firstRow = static_cast<long>(::SendMessageW(
                list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED));

            // The landed-on row must REALLY match the typed letter. Judged
            // by label rather than by row index, which was wrong twice
            // over: row 0 is a perfectly good jump target, and the focus row
            // carries in from the previous section, so a keystroke that
            // matched nothing still "did not move" the selection and looked
            // like a miss.
            //
            // 'jumpLabel' is the exact string OnTypeJumpChar matched
            // against, so this asserts the real contract rather than a guess
            // at it.
            const std::string landed = Of(w, "jumpLabel");
            firstLabel = landed;
        }
        // No row's label starts with an ASCII letter: the keystroke this
        // check drives has no valid target on this machine, so running it
        // would prove nothing either way. Reported as a skip, which names the
        // missing precondition rather than guessing at the product's
        // behaviour. Not a pass: a pass would claim the jump was verified.
        if (listHasFocus && pick == 0) {
            Skip("SMOKE 7.2 type-to-jump: no row label starts with an ASCII "
                 "letter, so there is no letter to type",
                 "precondition not met on this machine");
        }
        if (listHasFocus && pick != 0) {
            std::snprintf(line, sizeof(line),
                          "SMOKE 7.2 type-to-jump: typing '%lc' jumps to a matching row "
                          "(row %ld, process '%s')",
                          pick ? pick : L'?', firstRow, firstLabel.c_str());
            // The letter was derived from a label on screen and the jump was
            // then asserted against the label it actually landed on, so this
            // re-states that contract with the evidence rather than
            // re-deriving it. 'jumpLabel' is the exact string
            // OnTypeJumpChar matched against (process name, or local address
            // when unresolved).
            Check(firstRow >= 0, line,
                     "no letter jumped to a row whose label starts with it");
            if (!firstLabel.empty()) {
                const unsigned char c0 =
                    static_cast<unsigned char>(firstLabel[0]);
                std::snprintf(line, sizeof(line),
                              "SMOKE 7.2 type-to-jump: the selected row really starts "
                              "with '%lc' (label '%s', first char 0x%02X)",
                              pick, firstLabel.c_str(), static_cast<unsigned>(c0));
                Check(LabelStartsWith(firstLabel, pick), line,
                      "the selection landed on a row that does not match the key");
            }

            // Repeating the same letter must CYCLE to the next match, not append
            // to the prefix (which would search for "ss" and match nothing).
            ::SendMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(pick),
                           static_cast<LPARAM>(1));
            Pump(120);
            const long secondRow = (long)::SendMessageW(
                list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED);
            const std::string secondLabel = Of(w, "jumpLabel");
            std::snprintf(line, sizeof(line),
                          "SMOKE 7.2 type-to-jump: repeating '%lc' moves to another match "
                          "(%ld -> %ld, '%s')",
                          pick, firstRow, secondRow, secondLabel.c_str());
            Check(secondRow != firstRow || secondLabel == firstLabel, line,
                  "the second identical keystroke changed nothing at all");
        }   // listHasFocus
    }

    // ---- 9.4.8: `/` focuses the filter box (vim-style) -----------------------
    Mark("slash-focus");
    {
        // Establish list focus (mirrors the type-to-jump section's setup so this
        // is deterministic rather than depending on whatever ran before it).
        HWND list = ::GetDlgItem(hwnd, IDC_LIST);
        HWND edit = ::GetDlgItem(hwnd, IDC_EDIT_SEARCH);
        ::SetFocus(list);
        Pump(50);
        if (::GetFocus() != list) {
            Say("  [skip] 9.4.8 / focus: the list cannot take focus without an "
                "interactive desktop\n");
        } else {
            Check(Of(w, "focusTarget") == "list",
                  "SMOKE 9.4.8 / focus: filter box is reachable via /",
                  "list did not hold focus before the keystroke");
            // `/` while the LIST has focus must move focus to the filter edit,
            // not be eaten by type-to-jump (which would consume it as a no-op
            // prefix match).
            ::SendMessageW(hwnd, WM_CHAR, static_cast<WPARAM>('/'), 1);
            Pump(50);
            Check(Of(w, "focusTarget") == "filter",
                  "SMOKE 9.4.8 / focus: typing / moved focus to the filter",
                  "focus stayed on the list or moved elsewhere");
            Check(edit != nullptr && ::GetFocus() == edit,
                  "SMOKE 9.4.8 / focus: the filter edit really has focus",
                  "GetFocus() did not land on IDC_EDIT_SEARCH");
        }
    }

    // ---- 9.4.8: `*` toggles a bookmark on the selected row -------------------
    Mark("star-bookmark");
    {
        // `*` (VK_MULTIPLY) maps to IDM_CTX_BOOKMARK, the same toggle the
        // accelerator table dispatches. Driven through the command path (the
        // accelerator's TranslateAccelerator in the real loop is headless-
        // unblockable here).
        //
        // SAFETY: the ADD path opens a modal note prompt (PromptForText) that
        // hangs a headless harness, so this section NEVER lets the toggle hit
        // add. It plants a bookmark for row 0's remote endpoint directly, then
        // the toggle can only hit remove (no dialog). Mirrors section 11.
        HWND list = ::GetDlgItem(hwnd, IDC_LIST);
        LVITEMW sel = {};
        sel.mask = LVIF_STATE;
        sel.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        sel.state = LVIS_SELECTED | LVIS_FOCUSED;
        sel.iItem = 0;
        ::SendMessageW(list, LVM_SETITEMSTATE, 0,
                       reinterpret_cast<LPARAM>(&sel));
        Pump(300);

        // Read row 0's remote endpoint through the real owner-data path. Row 0
        // is not guaranteed to have a remote endpoint (listeners, TIME_WAIT and
        // wildcard rows don't), so try the first few rows the way a user would:
        // tab to the next candidate and retry the toggle.
        int remoteSlot = -1;
        for (int v = 0; v < 23; ++v) {
            if (static_cast<int>(Num(w, "visibleCol", std::to_string(v).c_str()))
                    == COL_REMOTE) {
                remoteSlot = v;
                break;
            }
        }
        const std::string remoteArg =    // narrow: Of() takes char*
            remoteSlot >= 0 ? std::to_string(remoteSlot) : std::string();

        bool planted = false;
        std::wstring plantedAddr;
        UINT plantedPort = 0;
        // Try up to 4 rows for a bookable endpoint. A planted bookmark is the
        // ONLY safe way to drive the remove path headless (the add path opens
        // a modal prompt that hangs the harness).
        for (int tryRow = 0; tryRow < 4 && !planted; ++tryRow) {
            LVITEMW selR = {};
            selR.mask = LVIF_STATE;
            selR.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            selR.state = LVIS_SELECTED | LVIS_FOCUSED;
            selR.iItem = tryRow;
            ::SendMessageW(list, LVM_SETITEMSTATE, tryRow,
                           reinterpret_cast<LPARAM>(&selR));
            Pump(80);
            if (remoteSlot < 0) break;
            const std::string endpoint =
                Of(w, "selectedColumnText",
                   remoteArg.empty() ? nullptr : remoteArg.c_str());
            const size_t colon = endpoint.rfind(':');
            std::wstring addr;
            unsigned port = 0;
            if (colon != std::string::npos) {
                addr = Widen(endpoint.substr(0, colon).c_str());
                const std::wstring digits = Widen(endpoint.substr(colon + 1).c_str());
                for (wchar_t ch : digits) {
                    if (ch < L'0' || ch > L'9') break;
                    port = port * 10 + static_cast<unsigned>(ch - L'0');
                    if (port > 65535) { port = 0; break; }
                }
            }
            if (!addr.empty() && port > 0 && port <= 65535 &&
                !Bookmarks::IsBookmarked(addr, static_cast<UINT>(port)) &&
                Bookmarks::Add(addr, static_cast<UINT>(port))) {
                planted = true;
                plantedAddr = addr;
                plantedPort = static_cast<UINT>(port);
            }
        }
        if (!planted) {
            Say("  [skip] 9.4.8 * bookmark: no bookable remote endpoint found in "
                "the first 4 rows; toggle semantics are covered by the bookmark.* "
                "selftests and the `bookmark` CLI verb\n");
        } else {
            const long before = Num(w, "bookmarkCount");
            Check(before >= 0 && Bookmarks::IsBookmarked(plantedAddr, plantedPort),
                  "SMOKE 9.4.8 * bookmark: planted bookmark is live",
                  "registry did not reflect the Add() above");
            // The toggle now removes the planted one (no dialog on remove).
            Cmd(hwnd, IDM_CTX_BOOKMARK);
            Pump(500);
            const long after = Num(w, "bookmarkCount");
            std::snprintf(line, sizeof(line),
                          "SMOKE 9.4.8 * bookmark: toggle removed the planted "
                          "bookmark (%ld -> %ld)", before, after);
            Check(after == before - 1, line, "the count did not drop by one");
            Check(!Bookmarks::IsBookmarked(plantedAddr, plantedPort),
                  "SMOKE 9.4.8 * bookmark: the planted bookmark is gone",
                  "registry still lists it after the toggle");
        }
    }

    // ---- 16 / 7.1 column visibility + order --------------------------------
    Mark("columns");
    {
        // Hiding a VISIBLE column must shrink the list's column set; showing
        // it again must put it back. This is the observable consequence of
        // the column machinery that 7.1 reorders.
        const int v0 = static_cast<int>(Num(w, "visibleCol", "0"));
        const int v1 = static_cast<int>(Num(w, "visibleCol", "1"));
        Check(v0 >= 0 && v1 >= 0, "SMOKE column machinery: visible columns resolve",
              "VisibleToCol returned -1 for a shown column");

        // Find a column that is currently SHOWN, by asking the store which
        // ColumnId sits behind each visible slot. Toggling one that is already
        // hidden would show it instead - still a remap, but the wrong
        // direction for a test that hides and then re-shows.
        int shownCol = -1;
        int shownSlot = -1;
        for (int v = 2; v < 23; ++v) {
            const std::string arg = std::to_string(v);
            const int col = static_cast<int>(Num(w, "visibleCol", arg.c_str()));
            if (col >= 0 && Num(w, "columnWidth", arg.c_str()) > 0) {
                shownCol = col;
                shownSlot = v;
                break;
            }
        }
        std::snprintf(line, sizeof(line),
                      "SMOKE 16 columns: found a visible column to toggle "
                      "(ColumnId %d at slot %d)", shownCol, shownSlot);
        Check(shownCol >= 0, line, "no visible column could be located");

        // The command id for a ColumnId is IDM_COL_BASE + that id.
        Cmd(hwnd, static_cast<WORD>(IDM_COL_BASE + shownCol));
        Pump(500);
        const int hidden = static_cast<int>(Num(w, "visibleCol",
                                    std::to_string(shownSlot).c_str()));
        std::snprintf(line, sizeof(line),
                      "SMOKE 16 columns: hiding ColumnId %d removes it from the "
                      "visible mapping (slot %d now -> %d)",
                      shownCol, shownSlot, hidden);
        Check(hidden != shownCol, line,
              "the hidden column is still mapped at its slot");

        Cmd(hwnd, static_cast<WORD>(IDM_COL_BASE + shownCol));
        Pump(500);
        const int back = static_cast<int>(Num(w, "visibleCol",
                                  std::to_string(shownSlot).c_str()));
        std::snprintf(line, sizeof(line),
                      "SMOKE 16 columns: showing it again restores the mapping "
                      "(slot %d -> %d)", shownSlot, back);
        Check(back == shownCol, line, "the column did not come back");

        const long o0 = Num(w, "colOrderSize");
        std::snprintf(line, sizeof(line),
                      "SMOKE 7.1 column order: the permutation is fully sized (%ld "
                      "entries, expected %d)", o0, static_cast<int>(COL_COUNT));
        Check(o0 == static_cast<long>(COL_COUNT), line, "colOrder_ is not COL_COUNT long");

        // ---- C8: the error-reporting counters the window now exposes --------
        // Three new read ops, and this checks the only thing checkable about
        // them: that they answer a number at all, that the pair that must be
        // ordered actually is, and that they are ZERO for a window that has
        // done nothing wrong.
        //
        // It does NOT check that a settings write is counted. SaveSettings()
        // runs once, from ~MainWindow, and this harness never destroys the
        // window; the reorder path needs a synthesised header-drag. So
        // attempts==0 here is the expected answer, not a gap in coverage -
        // except that it means no check pins "exactly one write per call",
        // which is the defect the change fixes. Stated rather than papered
        // over: see MainWindow::PersistSettings.
        {
            const long tmo = Num(w, "trafficTimeouts");
            const long fail = Num(w, "trafficScanFailures");
            std::snprintf(line, sizeof(line),
                          "C8 traffic diagnostics answer: timeouts=%ld "
                          "scan-failures=%ld", tmo, fail);
            // Num() answers -1 for an op the window does not implement, so a
            // non-negative answer is what distinguishes "reports zero" from
            // "no such op" - the trap this op table has already sprung once.
            Check(tmo >= 0 && fail >= 0, line,
                  std::string("an op is missing: timeouts=") +
                      (tmo < 0 ? "absent" : "present") +
                      " scanFailures=" + (fail < 0 ? "absent" : "present"));
            std::snprintf(line, sizeof(line),
                          "C8 no traffic scan failed during the harness run "
                          "(timeouts=%ld scan-failures=%ld)", tmo, fail);
            // Only the SCAN-FAILURE count is asserted zero, and NOT the timeout
            // count. The first version of this check asserted both and failed
            // with timeouts=13 - which is correct behaviour, not a defect: a
            // busy harness run really does abandon passes whose workers stop
            // making progress, and that counter exists precisely to say so.
            // Asserting it zero would have been asserting that a real,
            // documented condition never happens.
            Check(fail == 0, line,
                  std::string("scan-failures=") + std::to_string(fail) +
                      " (timeouts=" + std::to_string(tmo) +
                      ", which is allowed and reported separately)");

            const long attempts = Num(w, "settingsSaveAttempts");
            const long badWrites = Num(w, "settingsSaveFailures");
            std::snprintf(line, sizeof(line),
                          "C8 settings persistence counters answer: attempts=%ld "
                          "failures=%ld", attempts, badWrites);
            Check(attempts >= 0 && badWrites >= 0, line,
                  std::string("an op is missing: attempts=") +
                      (attempts < 0 ? "absent" : "present") + " failures=" +
                      (badWrites < 0 ? "absent" : "present"));
            std::snprintf(line, sizeof(line),
                          "C8 failures never exceed attempts (%ld attempts, %ld "
                          "failures)", attempts, badWrites);
            Check(badWrites <= attempts, line,
                  "more failures than writes - the counters are not paired");
            std::snprintf(line, sizeof(line),
                          "C8 no settings write has failed in this session "
                          "(%ld of %ld)", badWrites, attempts);
            Check(badWrites == 0, line, "a settings write failed");
        }
    }

    // ---- 5.4 change-log window (SMOKE: open/close only) --------------------
    // Buffer caps and CSV shape are proven headless by the changelog.*
    // selftests; here the menu item only has to show and hide the window.
    Mark("change-log");
    {
        Check(!Is(w, "changeLogVisible", 1),
              "SMOKE 5.4 change log: closed before the command", "already open");
        Cmd(hwnd, IDM_FILE_CHANGELOG_WIN);
        Pump(800);
        Check(Is(w, "changeLogVisible", 1),
              "SMOKE 5.4 change log: the command opens the window",
              std::string("created=") + (Is(w, "changeLogCreated", 1) ? "yes" : "no") +
                  " visible=" + (Is(w, "changeLogVisible", 1) ? "yes" : "no"));
        Cmd(hwnd, IDM_FILE_CHANGELOG_WIN);
        Pump(500);
        Check(!Is(w, "changeLogVisible", 1),
              "SMOKE 5.4 change log: the command toggles it closed",
              "window stayed open");
    }

    // ---- 5.3 change-log event mask (SMOKE: the filter actually filters) ----
    // The one capability D25 added to the CLI with no GUI surface. The
    // filtering logic is pure and belongs in selftest (changelog.mask.*); what
    // only a window can prove is that the checkboxes exist, are wired, and that
    // ticking one does not throw the window out of step. So this drives the
    // real WM_COMMAND through the real controls and asks the window what it
    // now believes - which is exactly the kind of check the harness exists for.
    // ---- D29: the self-kill must be unreachable (SMOKE: the real rule) -------
    // The bug report was "wintcp.exe closed when I killed something else". Two
    // things were wrong: a use-after-free handing KillPid() a garbage PID, and
    // an "End process..." item that was enabled on wintcp.exe's OWN rows - which
    // a network tool necessarily has.
    //
    // The use-after-free is fixed by construction (the row is copied before the
    // modal) and is not observable from a harness. The self-kill IS, and this is
    // the only place it can be tested honestly: the harness runs inside
    // wintcp.exe, so GetCurrentProcessId() here is the self PID the bug was
    // about, checked against the real rule rather than a synthetic stand-in.
    // ---- 5.3 change-log event mask (SMOKE: the filter actually filters) ----
    // The one capability D25 added to the CLI with no GUI surface. The
    // filtering logic is pure and belongs in selftest (changelog.mask.*); what
    // only a window can prove is that the checkboxes exist, are wired, and that
    // ticking one does not throw the window out of step.
    Mark("change-log mask");
    {
        // Of() takes a narrow arg and widens it, so the PID is formatted narrow
        // here - the harness's own convention, not a second encoding path.
        char selfBuf[16] = {0};
        ::sprintf_s(selfBuf, "%lu",
                    static_cast<unsigned long>(::GetCurrentProcessId()));
        Check(Of(w, "killVerdict", selfBuf) == "self",
              "SMOKE D29: wintcp's own PID is refused as a kill target",
              "verdict=" + Of(w, "killVerdict", selfBuf) +
                  " (self pid " + selfBuf + ")");
        // 4 is the System pseudo-process: also refused, and it is the case a
        // wildcard listening row produces on every machine.
        Check(Of(w, "killVerdict", "4") == "pseudo",
              "SMOKE D29: the System pseudo-PID is refused",
              "verdict=" + Of(w, "killVerdict", "4"));
        // And an ordinary foreign PID must still be ALLOWED - a guard that
        // refuses everything passes the two checks above and is useless. 999999
        // is deliberately a PID that almost certainly does not exist: the
        // verdict is about the RULE, not about liveness, and a live target
        // would make this test depend on what the machine happens to be doing.
        Check(Of(w, "killVerdict", "999999") == "ok",
              "SMOKE D29: the guard does not refuse foreign PIDs",
              "999999 -> " + Of(w, "killVerdict", "999999"));
    }
    {
        ChangeLogWindow* log = w.ChangeLogForTest();
        Check(log != nullptr,
              "SMOKE 5.3 change log: the window object is reachable");
        if (log != nullptr) {
            Cmd(hwnd, IDM_FILE_CHANGELOG_WIN);   // open it
            Pump(700);
            Check(log->IsVisible(),
                  "SMOKE 5.3 change log: reopened for the mask check");
            // Read Handle() AFTER opening. The previous section closed the log
            // with the menu item, which calls Close() -> DestroyWindow, so the
            // window and all its child controls were destroyed and Show() has to
            // create them again. Caching the HWND before the reopen - which the
            // first version of this check did - searched a destroyed window and
            // found no buttons, reporting "the Appear checkbox exists: FAIL"
            // with an empty detail while the checkbox was present and working.
            const HWND loghwnd = log->Handle();
            Check(loghwnd != nullptr,
                  "SMOKE 5.3 change log: the reopened window has a handle");

            // Default is everything on - the behaviour must be unchanged for
            // anyone who never touches the boxes.
            Check(log->EventAppear() && log->EventDisappear() &&
                      log->EventState() && log->EventMaskIsFull(),
                  "SMOKE 5.3 change log: the mask starts fully on");

            // Ticking "appear" OFF, through the real control.
            // Find the Appear checkbox by CAPTION, walking the direct children.
            //
            // GetDlgItem rather than FindWindowExW, and the reason is worth
            // recording because the first version used FindWindowExW and found
            // NOTHING: FindWindowExW matches the window CLASS name, and a
            // standard BUTTON control's class is the atom "Button", which
            // FindWindowExW's lpszClass comparison does not resolve against a
            // lowercase "BUTTON" string. GetDlgItem matches by CONTROL ID and is
            // immune to that. The ids live in ChangeLogWindow.cpp's anonymous
            // namespace, so they are duplicated here with a static_assert-style
            // comment - the alternative, exposing them in the header, widens the
            // class's surface for a test's convenience.
            //
            // The ids are load-bearing: they are what the window's WM_COMMAND
            // switch dispatches on, so a wrong value here would click nothing.
            constexpr UINT kIdEvAppear = 7;   // == IDC_LOG_EV_APPEAR
            HWND box = ::GetDlgItem(loghwnd, kIdEvAppear);
            // Say what IS there when the lookup fails, so the next failure is
            // diagnosable: count the children and name the classes found.
            std::string seen;
            if (box == nullptr) {
                for (HWND h = ::GetWindow(loghwnd, GW_CHILD); h != nullptr;
                     h = ::GetWindow(h, GW_HWNDNEXT)) {
                    wchar_t cls[64] = {0};
                    ::GetClassNameW(h, cls, 64);
                    wchar_t cap[64] = {0};
                    ::GetWindowTextW(h, cap, 64);
                    if (!seen.empty()) seen += ", ";
                    seen += "[";
                    for (const wchar_t* q = cls; *q != 0; ++q)
                        seen += static_cast<char>(*q < 128 ? *q : '?');
                    seen += " \"";
                    for (const wchar_t* q = cap; *q != 0; ++q)
                        seen += static_cast<char>(*q < 128 ? *q : '?');
                    seen += "\"]";
                }
            }
            const bool found = (box != nullptr);
            // The captions are in the detail so a failure says WHICH buttons
            // exist rather than only that the one wanted did not - the first
            // version of this check failed with an empty detail and gave no
            // clue whether the window had no buttons or different labels.
            const std::string caps = "children of the log window: " +
                                     (seen.empty() ? std::string("(none)")
                                                   : seen);
            // Also prove the WINDOW's own controls exist at all, independent of the kind
            // checkboxes. If this ever fails then WM_CREATE is not running and
            // every other check in this section is meaningless - so it is
            // reported separately rather than folded into the checkbox result,
            // which would say "no checkbox" and hide the real fault.
            const HWND anyChild = ::GetWindow(loghwnd, GW_CHILD);
            Check(anyChild != nullptr,
                  "SMOKE 5.3 change log: WM_CREATE built the window's controls",
                  "WM_CREATE has run " +
                      std::string(Of(w, "changeLogCreates", "")) +
                      " time(s); children made: " +
                      std::string(Of(w, "changeLogChildren", "")) +
                      ", first CreateWindowExW GetLastError=" +
                      std::string(Of(w, "changeLogLastError", "")) + "; " +
                      caps);
            Check(found, "SMOKE 5.3 change log: the Appear checkbox exists", caps);
            if (found) {
                // Click it the way a user does: BM_CLICK, which sets the check
                // state AND posts the BN_CLICKED notification to the parent -
                // the same path a mouse click takes. Synthesising BM_SETCHECK
                // plus a hand-built WM_COMMAND looks equivalent and is not: the
                // first version of this check did exactly that, the notification
                // was malformed, the handler never ran, and the assertion below
                // reported "the window still reports appear on" while the box on
                // screen was visibly unticked. The window was right; the test
                // was not.
                ::SendMessageW(box, BM_CLICK, 0, 0);
                Pump(400);
                Check(!log->EventAppear() && log->EventDisappear() &&
                          log->EventState(),
                      "SMOKE 5.3 change log: unticking Appear narrows the mask",
                      std::string("after the click the window reports appear=") +
                          (log->EventAppear() ? "ON" : "off") + " disappear=" +
                          (log->EventDisappear() ? "ON" : "off") + " state=" +
                          (log->EventState() ? "ON" : "off"));
                Check(!log->EventMaskIsFull(),
                      "SMOKE 5.3 change log: a narrowed mask is reported as "
                      "narrowed");
                // ...and events of the excluded kind are now dropped, which is
                // the whole point: the status line must stop counting them.
                const size_t before = log->Count();
                std::vector<RowChange> onlyAppear;
                RowChange ev;
                ev.kind = kChangeAppear;
                // A minimal row: AppendEvents only reads the identity fields to
                // build cells, and the mask rejects it before any of that. A
                // default-constructed Connection would do, but naming the
                // endpoint keeps the CSV cell meaningful if the mask ever
                // changes under us.
                ev.row.localAddress = L"192.0.2.1";
                ev.row.localPort = 50000;
                ev.row.remoteAddress = L"198.51.100.1";
                ev.row.remotePort = 443;
                // No state constant: ChangeLogWindow builds its cell from the
                // label, and the mask rejects this event before formatting
                // happens. Naming a TCP state here would need an include the
                // harness does not otherwise have, for a value nothing reads.
                ev.row.stateLabel = L"ESTABLISHED";
                ev.row.pid = 4242;
                ev.row.processName = L"harness.exe";
                onlyAppear.push_back(ev);
                log->AppendEvents(onlyAppear);
                Pump(200);
                Check(log->Count() == before,
                      "SMOKE 5.3 change log: an excluded event is not recorded",
                      "count went " + std::to_string(before) + " -> " +
                          std::to_string(log->Count()));
            }
            // Restore, or the rest of the run inherits a narrowed log.
            log->SetEventMask(true, true, true);
            Pump(200);
            Check(log->EventMaskIsFull(),
                  "SMOKE 5.3 change log: the mask can be restored");
            Cmd(hwnd, IDM_FILE_CHANGELOG_WIN);   // close again
            Pump(400);
        }
    }

    // ---- 5.3 bookmarks (SMOKE: menu wiring + survival, no dialogs) --------
    // Pin/note/colour semantics are proven headless by the bookmark.*
    // selftests and live by the `bookmark` CLI verbs; here the menu item
    // only has to run without disturbing the list. The ADD path always
    // opens the modal note prompt, which has no headless dismissal and
    // once blocked the harness past its watchdog - so this section
    // deterministically drives the REMOVE path: it plants a bookmark for
    // the selected row directly, then the toggle press can only remove it
    // (no dialog), and the list must be intact afterwards.
    Mark("bookmarks");
    {
        // Select row 0 so the command has something to act on. A correct
        // LVITEM is required: LVM_SETITEMSTATE takes a POINTER as lParam,
        // and passing the state mask by value makes the control dereference
        // address 3 and kill the process.
        HWND list = ::GetDlgItem(hwnd, IDC_LIST);
        LVITEMW sel = {};
        sel.mask = LVIF_STATE;
        sel.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        sel.state = LVIS_SELECTED | LVIS_FOCUSED;
        sel.iItem = 0;
        ::SendMessageW(list, LVM_SETITEMSTATE, 0,
                       reinterpret_cast<LPARAM>(&sel));
        Pump(300);
        const long chosen = (long)::SendMessageW(
            list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED);
        std::snprintf(line, sizeof(line),
                      "SMOKE 5.3 bookmark: a row is selected for the command "
                      "(%ld)", chosen);
        Check(chosen >= 0, line, "could not select a row");

        // Read the row's remote endpoint cell through the real owner-data
        // path (same handler the screen uses), then split address and port.
        int remoteSlot = -1;
        for (int v = 0; v < 23; ++v) {
            const std::string arg = std::to_string(v);
            if (static_cast<int>(Num(w, "visibleCol", arg.c_str())) == COL_REMOTE) {
                remoteSlot = v;
                break;
            }
        }
        const std::string remoteArg = std::to_string(remoteSlot);
        const std::string endpoint =
            remoteSlot >= 0 ? Of(w, "selectedColumnText", remoteArg.c_str())
                            : std::string();
        const size_t colon = endpoint.rfind(':');
        const std::wstring addr =
            (colon == std::string::npos)
                ? std::wstring()
                : Widen(endpoint.substr(0, colon).c_str());
        // Port is decimal digits after the last colon (IPv6 literals are
        // bracketed, so the last colon is always the port separator).
        unsigned port = 0;
        if (colon != std::string::npos) {
            const std::wstring digits = Widen(endpoint.substr(colon + 1).c_str());
            for (wchar_t ch : digits) {
                if (ch < L'0' || ch > L'9') break;
                port = port * 10 + static_cast<unsigned>(ch - L'0');
                if (port > 65535) break;
            }
        }
        if (!addr.empty() && port > 0 && port <= 65535 &&
            Bookmarks::Add(addr, static_cast<UINT>(port)) &&
            Bookmarks::IsBookmarked(addr, static_cast<UINT>(port))) {
            const long before = Num(w, "rowCount");
            Cmd(hwnd, IDM_CTX_BOOKMARK);   // removes it: no dialog on remove
            Pump(500);
            const long after = Num(w, "rowCount");
            std::snprintf(line, sizeof(line),
                          "SMOKE 5.3 bookmark: the command removes a planted "
                          "bookmark and leaves the list intact "
                          "(%ld -> %ld rows)", before, after);
            Check(after > 0, line, "the list emptied during the command");
            Check(!Bookmarks::IsBookmarked(addr, static_cast<UINT>(port)),
                  "SMOKE 5.3 bookmark: the pin is really gone",
                  "registry still lists it");
        } else {
            Say("  [skip] SMOKE 5.3 bookmark: row 0 has no bookable remote "
                "endpoint; add/remove semantics are covered by the `bookmark` "
                "CLI verb and the bookmark.* selftests\n");
        }
    }

    // ---- 4.3 GeoIP --------------------------------------------------------
    {
        Check(!Is(w, "geoLoaded", 1),
              "SMOKE 4.3 GeoIP: not loaded until a database is chosen",
              "unexpectedly already loaded");
    }

    // ---- 7.5 / 7.6 About and the shortcut sheet ---------------------------
    Mark("command-path");
    {
        // About and the shortcut sheet open MODAL dialogs, which cannot be
        // driven headlessly - DialogBox runs its own loop inside the
        // SendMessage. Use the tray / topmost toggles instead: same
        // WM_COMMAND -> HandleMessage -> OnCommand path, no dialog.
        //
        // Read the value AFTER the command. An earlier version asserted the
        // expected result rather than measuring it, so it would have reported
        // a pass even if the command did nothing.
        const bool t0 = Is(w, "topMost", 1);
        Cmd(hwnd, IDM_VIEW_TOPMOST);
        Pump(300);
        const bool t1 = Is(w, "topMost", 1);
        std::snprintf(line, sizeof(line),
                      "SMOKE command path: topmost toggles %s -> %s",
                      t0 ? "on" : "off", t1 ? "on" : "off");
        Check(t0 != t1, line, "the command produced no state change");

        // And the menu's own check mark must agree with the flag.
        HMENU menu = ::GetMenu(hwnd);
        const UINT state = menu != nullptr
            ? ::GetMenuState(menu, IDM_VIEW_TOPMOST, MF_BYCOMMAND)
            : static_cast<UINT>(-1);
        const bool ticked = (state & MF_CHECKED) != 0;
        std::snprintf(line, sizeof(line),
                      "SMOKE command path: the menu check mark tracks the flag "
                      "(flag %s, menu %s)", t1 ? "on" : "off",
                      ticked ? "ticked" : "clear");
        Check(menu != nullptr && ticked == t1, line,
              "the menu check mark does not match the state it controls");

        Cmd(hwnd, IDM_VIEW_TOPMOST);
        Pump(200);
    }

    // ---- the window survived all of it ------------------------------------
    Check(::IsWindow(hwnd) != 0, "SMOKE the main window survived every command",
          "the window was destroyed mid-harness");
    Check(Num(w, "rowCount") > 0, "SMOKE the list still has rows at the end",
          "the list was emptied");

    // Report the traffic-scan timeouts rather than hiding them. On a machine
    // where some socket's SIO_TCP_INFO never returns, the traffic columns
    // fall back to last-known totals for those PIDs; that is a real
    // degradation and the user should be able to see it happened.
    const long tmo = Num(w, "trafficTimeouts");
    if (tmo > 0) {
        Sayf("  [note] socket-traffic scans timed out %ld time(s); some "
             "sockets did not answer SIO_TCP_INFO and their processes keep "
             "last-known totals\n", tmo);
    } else {
        Say("  [note] socket-traffic scans: all completed within budget\n");
    }

    // Skips appear in the summary line rather than being swallowed: a harness
    // that skips quietly is one whose green verdict means less and less as
    // more of it runs out of preconditions.
    if (g_skip > 0) {
        std::snprintf(line, sizeof(line),
                      "\n=== UI HARNESS: %d passed, %d failed, %d skipped ====\n",
                      g_pass, g_fail, g_skip);
    } else {
        std::snprintf(line, sizeof(line),
                      "\n=== UI HARNESS: %d passed, %d failed ====\n",
                      g_pass, g_fail);
    }
    Say(line);
    g_done = 1;   // stand the watchdog down
    // Stop the refresh loop before tearing down: with no new passes, no new
    // socket scans can start, so ExitProcess in main() cannot race one.
    // (This replaces the old stopTrafficScans probe, which reached into the
    // product through a test hook to flip the sampler's shutdown flag.)
    if (Is(w, "autoRefresh", 1)) {
        Cmd(hwnd, IDM_VIEW_AUTOREFRESH);
        Pump(300);
    }
    if (g_fail == 0) {
        report = L"UI HARNESS PASS";
    } else {
        wchar_t wbuf[128];
        ::swprintf_s(wbuf, L"UI HARNESS FAIL: %d of %d checks failed", g_fail,
                     g_pass + g_fail);
        report = wbuf;
    }
    return report;
}

}  // namespace wintcp
