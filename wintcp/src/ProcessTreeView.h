// ProcessTreeView.h
// SPDX-License-Identifier: Apache-2.0
// The ONE implementation of "rows -> tree items" for the process-tree view.
//
// The engine half (ProcessTree: BuildProcessTree over the rows the store
// already holds) and the killtree verb are pure and shipped; this is the
// view-side translation - aggregate the rows to one node per process and
// insert them under their parents. It is a free function over a HWND rather
// than a window class because the view is a MODE of the main window (a tree
// control where the list view is, like Task Manager's Processes tab), not a
// window of its own: two surfaces showing one hierarchy are two answers to
// the same question, and answers drift.
//
// The rules come from the engine, not from here: a node whose parent is not
// in the set is a root ("nobody claims me", not "PPID is 0"), a cycle is
// broken rather than followed, and an over-deep chain is reported.

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

#include "Connection.h"

namespace wintcp {

// Empties 'tree' and refills it: one item per PROCESS (every row with the
// PID folded in, labelled "name  (PID n, k connections)"), placed under its
// parent where the parent is also in the set and as a root otherwise. Roots
// are expanded one level, so the forest reads as a tree on open.
//
// Returns the process count. '*depthExceeded' receives the engine's ceiling
// flag so the caller can say so rather than truncating silently.
//
// 'rows' is taken BY VALUE on the caller's side: a refresh that reallocates
// the store mid-render cannot corrupt the tree, because the values were
// copied before the first insert.
size_t ProcessTreeFill(HWND tree, const std::vector<Connection>& rows,
                       bool* depthExceeded);

}  // namespace wintcp
