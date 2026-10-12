// ConnectionsListOwner.cpp
// SPDX-License-Identifier: Apache-2.0
//
// The window's half of the connections list: what it answers when the list
// asks. The list reports up (a sort was requested, a row was activated, a
// drag started) and reads down (the painting state it paints with); this
// file is the up-direction, plus the shell-icon lookup the painting needs.
//
// Kept beside MainWindow.cpp rather than inside it, because these five
// methods exist only for the list and were previously interleaved with the
// window's own message handlers - which is how a reader had to skim three
// hundred lines of command dispatch to find out what a double-click does.

#include "MainWindow.h"

#include "MainWindowInternal.h"   // kMsPerSecond and the shared helpers
#include "WinCaps.h"              // DllAvailable: shell32 is delay-loaded

namespace wintcp {

void MainWindow::OnSortByColumn(int columnId, bool ascending) {
    // A header click. The window's half is exactly this: set the sort, move
    // the arrow, re-apply. The store decides what the sort MEANS, and the
    // column geometry object decides where the arrow goes.
    store_.SetSort(columnId, ascending);
    UpdateSortIndicator();
    ApplyView();
}

void MainWindow::OnRowActivated() {
    // Double-click or Enter.
    ShowDetailsOfSelectedRow();
}

void MainWindow::OnColumnDragStarted() {
    // Posted, not handled here: at LVN_BEGINDRAG the header still holds the
    // OLD order, and there is no end-of-drag notification to hook. The
    // posted message lands after the drag has finished, which is when the
    // new order is readable.
    ::PostMessageW(hwnd_, WM_APP_COLUMNORDER_SYNC, 0, 0);
}

int MainWindow::ProcessIconIndex(const std::wstring& path) const {
    // The shell's system image list is the source of every
    // index, so the index this returns is only meaningful with
    // that list attached to the list control. Without it (no
    // shell, or the shell list not yet resolved) there is no
    // list to index into, so -1 ("no icon") is the honest
    // answer rather than a number that would draw nothing.
    if (iconImageList_ == nullptr || path.empty()) return -1;
    if (!DllAvailable("shell32.dll")) return -1;
    SHFILEINFOW sfi = {};
    ::SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                     SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    // SHGetFileInfo leaves iIcon at -1 when it has no icon for
    // the path (a process that has already exited, say), which
    // is the list control's own "no icon" value, so it passes
    // straight through.
    return sfi.iIcon;
}

ConnectionsListSink::Painting MainWindow::QueryPainting() const {
    ConnectionsListSink::Painting p;
    p.highContrast = HighContrastActive();
    p.darkTheme = ThemeIsDark();
    p.snapshotTick = store_.SnapshotTick();
    // Only the cases with a fix carry a hint, so a healthy "no traffic yet"
    // table stays quiet.
    p.emptyStateHint = &emptyStateHint_;
    return p;
}

}  // namespace wintcp
