// ProcessTreeWindow.h
// SPDX-License-Identifier: Apache-2.0
// The GUI process-tree view: a tree control where a list view is.
//
// The engine half (ProcessTree: BuildProcessTree over the rows the store
// already holds) and the killtree verb both shipped; this is the view chrome
// over them - a modeless window with a SysTreeView32 beside Refresh/Close,
// in the style of Task Manager's Processes tab. One process per node,
// labelled "name (PID n, k connections)"; roots are processes nothing in the
// set claims as a child (the engine's definition, not "PPID 0").
//
// WHY A SUPPLIER, not a store pointer: the window is fed, like
// ChangeLogWindow is fed, rather than polling. MainWindow hands it a
// callable that returns the current snapshot rows; Refresh re-calls it, so
// the tree always renders what the main table holds without owning any of
// it. A snapshot that moved on mid-render cannot corrupt anything, because
// the rows are values taken in one call.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "Connection.h"

namespace wintcp {

class ProcessTreeWindow {
public:
    // How the window reads the main table: one call, one snapshot of rows
    // by value. By value, not by reference, so a refresh replacing the
    // store underneath the render is impossible by construction.
    using RowSupplier = std::function<std::vector<Connection>()>;

    ProcessTreeWindow() = default;
    ~ProcessTreeWindow();

    ProcessTreeWindow(const ProcessTreeWindow&) = delete;
    ProcessTreeWindow& operator=(const ProcessTreeWindow&) = delete;

    // Show (or reveal) the window; centers over 'owner' on first show and
    // rebuilds from the supplier. Replaces any previous supplier.
    void Show(HWND owner, HFONT font, RowSupplier supplier);
    // Re-read the supplier and rebuild. No-op without a window or supplier.
    void RefreshNow();
    void Close();   // destroy (app shutdown path); hide is SW_HIDE
    bool IsOpen() const { return hwnd_ != nullptr; }
    bool IsVisible() const;
    HWND Handle() const { return hwnd_; }

    void ApplyFont(HFONT font);   // on DPI change; borrowed, never owned

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                       LPARAM lParam);
    // The tree's own proc: Esc must hide even when the tree owns the focus,
    // and keys never reach the parent, so the list handles it itself.
    static LRESULT CALLBACK TreeSubclassProc(HWND hwnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam,
                                             UINT_PTR id, DWORD_PTR refData);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void Rebuild();   // supplier -> aggregated processes -> tree control
    void Layout(int cx, int cy);
    void CenterOnOwner(HWND owner);
    void UpdateStatus(size_t processes, size_t connections,
                      bool depthExceeded, bool empty);
    int S(int px96) const;

    HWND hwnd_ = nullptr;
    HWND hwndTree_ = nullptr;
    HWND hwndRefresh_ = nullptr;
    HWND hwndClose_ = nullptr;
    HWND hwndStatus_ = nullptr;

    HFONT font_ = nullptr;   // borrowed from MainWindow; never deleted here
    RowSupplier supplier_;

    static const wchar_t* kClassName;
};

}  // namespace wintcp
