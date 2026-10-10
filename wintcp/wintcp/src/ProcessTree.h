// ProcessTree.h
// SPDX-License-Identifier: Apache-2.0
// 9.5.6 - the process tree.
//
// `ppid` and `parent` describe a row's parent; this answers the two questions
// those cannot: "what did this process start" (the descendants) and "kill this
// process and everything under it".
//
// Both halves are here, pure, because the interesting part is neither the
// Toolhelp snapshot nor the TerminateProcess handle - it is the shape of the
// tree and the rules for walking it. Those are pure functions over the rows
// the store already holds, which means they are testable without spawning
// anything and without an elevation prompt.
//
// THREE RULES the implementation follows, each learned from a way this can go
// wrong, and each pinned by the selftest:
//
//   1. A CYCLE IS REFUSED, not followed. Windows PIDs can form a spurious
//      cycle when a process is being torn down and its parent field is stale.
//      Following one produces an infinite list or a stack overflow, so
//      BuildProcessTree detects the back-edge and breaks it.
//
//   2. ROOT selection is explicit, NOT "PPID 0". A process whose parent has
//      already exited is re-parented and its PPID can be a recycled number,
//      which is how "the tree" comes to contain a row that is nobody's parent
//      and nobody's child. A root is a row that no OTHER row in the set
//      claims as its parent.
//
//   3. DEPTH IS BOUNDED. A deep chain (a service host spawning services that
//      spawn services) is legal, but an unbounded recursion is not - so the
//      walk has an explicit ceiling and reports it rather than truncating
//      silently.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>

#include <cstddef>
#include <vector>

namespace wintcp {

// The minimum of a process the tree module needs. The store's Connection
// already carries all of it, so nothing is copied twice to build a tree.
struct TreeNodeInfo {
    DWORD pid = 0;
    DWORD ppid = 0;
    bool ppidKnown = false;
    FILETIME processCreate = {};   // for the PID-reuse guard on a kill
};

// Depth ceiling for one walk. 64 is far above any real parent chain and low
// enough that a pathological cycle cannot exhaust the stack.
constexpr size_t kProcessTreeNodeMaxDepth = 64;

// One node of the built tree. 'children' indexes INTO THE TREE (not into the
// rows), so a caller can render the whole subtree without a second lookup.
struct TreeNode {
    TreeNodeInfo info;
    size_t rowIndex = 0;               // which row this came from
    std::vector<size_t> children;      // indices into the tree vector
    bool visited = false;              // internal: cycle detection
};

struct ProcessTree {
    std::vector<TreeNode> nodes;       // roots are nodes()[0..rootCount)
    size_t rootCount = 0;
    bool depthExceeded = false;        // the ceiling was hit, some nodes omitted
};

// Build a tree from 'rows' (every row, not a filtered view - a tree built from
// a filtered view is a broken tree, because the parents are missing).
// Never fails: a cycle or an over-deep chain is REPORTED, not returned as an
// error, because a partially-drawn tree is still more use than no tree.
ProcessTree BuildProcessTree(const std::vector<TreeNodeInfo>& rows);

// The descendants of 'rootPid', roots-first, depth-first. 'rows' must be every
// row, as above. An empty result means the root is not in the set at all.
//
// This is the set a kill-tree acts on, so it is what the PID-reuse guard is
// applied to: the caller re-checks each PID against the create time captured
// with the row before terminating it.
std::vector<size_t> ProcessTreeDescendants(
    const std::vector<TreeNodeInfo>& rows, DWORD rootPid);

// Is 'pid' an ancestor of 'descendantPid' within this set? Used by the
// caller to refuse "kill your own grandfather", which would leave the
// tooling that issued the kill running under a process that no longer exists.
bool IsAncestorOf(const std::vector<TreeNodeInfo>& rows, DWORD pid,
                  DWORD descendantPid);

// Every row that no OTHER row in the set claims as its parent, in row order.
// A "root" is defined by observation rather than by a PPID value, because a
// stale PPID is the one way a real root can look like an orphan.
std::vector<size_t> ProcessTreeRoots(const std::vector<TreeNodeInfo>& rows);

}  // namespace wintcp
