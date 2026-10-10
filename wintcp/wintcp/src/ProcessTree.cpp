// ProcessTree.cpp
// SPDX-License-Identifier: Apache-2.0
// See ProcessTree.h.

#include "ProcessTree.h"

#include <algorithm>

namespace wintcp {
namespace {

// The ppid this row reports. 'hasParent' is separate from the value so a
// caller can distinguish "no parent recorded" from "parent recorded as 0" -
// and because a self-parent is NOT a parent for any of the tree's purposes.
struct Parent {
    bool present = false;
    DWORD pid = 0;
    bool self = false;
};

Parent RowParent(const TreeNodeInfo& r) {
    Parent p;
    if (!r.ppidKnown) return p;
    p.pid = r.ppid;
    p.present = true;
    p.self = (r.ppid == r.pid);
    return p;
}

}  // namespace

ProcessTree BuildProcessTree(const std::vector<TreeNodeInfo>& rows) {
    ProcessTree tree;
    if (rows.empty()) return tree;

    // One node per row that has a PID. Deliberately NOT deduplicated: the
    // tree mirrors what the user is looking at, and a set with several rows
    // for one process is a set the user is looking at a set of processes
    // drawn from several connections.
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].pid == 0) continue;
        TreeNode n;
        n.info = rows[i];
        n.rowIndex = i;
        tree.nodes.push_back(n);
    }
    if (tree.nodes.empty()) return tree;

    // Parent lookup, built once, by pid.
    //
    // A parent THAT IS NOT IN THE SET (exited, or a caller who ignored the
    // contract and passed a filtered view) is not an error: that node stays
    // a root here, which is what keeps such a tree renderable rather than
    // losing those subtrees entirely.
    std::vector<size_t> parentRow(tree.nodes.size(), SIZE_MAX);
    for (size_t i = 0; i < tree.nodes.size(); ++i) {
        const Parent p = RowParent(tree.nodes[i].info);
        if (!p.present || p.self) continue;
        for (size_t k = 0; k < tree.nodes.size(); ++k) {
            if (tree.nodes[k].info.pid == p.pid) {
                tree.nodes[k].children.push_back(i);
                parentRow[i] = k;
                break;
            }
        }
    }

    // A root is a node nothing else claims as its parent. That is the
    // definition rather than "PPID is 0", because a stale PPID is the one way
    // a real root can look like an orphan and an orphan look like a root.
    //
    // 'visited' is reused as the walk flag: a root is its own tree's visited
    // root, and a descendant reached twice is a cycle or a duplicate parent.
    for (size_t i = 0; i < tree.nodes.size(); ++i) {
        if (parentRow[i] == SIZE_MAX) {
            tree.nodes[i].visited = true;
            ++tree.rootCount;
        }
    }

    // Walk, iteratively, so a deep chain cannot exhaust the stack even if the
    // ceiling were ever mis-set. Every reachable node is visited, which is
    // what leaves an unreached one to be reported.
    struct Frame {
        size_t node;
        size_t nextChild;
        size_t depth;
    };
    std::vector<Frame> stack;
    for (size_t r = 0; r < tree.nodes.size(); ++r) {
        if (!tree.nodes[r].visited) continue;
        stack.push_back({r, 0, 0});
        while (!stack.empty()) {
            Frame& top = stack.back();
            const TreeNode& n = tree.nodes[top.node];
            bool descended = false;
            while (top.nextChild < n.children.size()) {
                const size_t child = n.children[top.nextChild];
                ++top.nextChild;
                if (tree.nodes[child].visited) continue;   // cycle or dup
                if (top.depth + 1 > kProcessTreeNodeMaxDepth) {
                    // Reported, not truncated silently: a caller that draws
                    // "more" must know there is a more.
                    tree.depthExceeded = true;
                    continue;
                }
                tree.nodes[child].visited = true;
                stack.push_back({child, 0, top.depth + 1});
                descended = true;
                break;
            }
            if (!descended) stack.pop_back();
        }
    }
    return tree;
}

std::vector<size_t> ProcessTreeRoots(const std::vector<TreeNodeInfo>& rows) {
    std::vector<size_t> roots;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Parent p = RowParent(rows[i]);
        if (!p.present || p.self) {
            roots.push_back(i);
            continue;
        }
        bool claimed = false;
        for (size_t k = 0; k < rows.size(); ++k) {
            if (rows[k].pid == p.pid) {
                claimed = true;
                break;
            }
        }
        if (!claimed) roots.push_back(i);
    }
    return roots;
}

std::vector<size_t> ProcessTreeDescendants(
    const std::vector<TreeNodeInfo>& rows, DWORD rootPid) {
    std::vector<size_t> out;
    if (rows.empty() || rootPid == 0) return out;

    // The root row for this pid: the first bearing it, the same rule
    // BuildProcessTree uses, so the two cannot disagree about which row a
    // PID means.
    size_t rootRow = rows.size();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].pid == rootPid) {
            rootRow = i;
            break;
        }
    }
    if (rootRow == rows.size()) return out;

    // Parent -> children, by pid, once. The inner scan stops at the first
    // row with that pid, exactly as BuildProcessTree builds children.
    std::vector<std::vector<size_t>> kids(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const Parent p = RowParent(rows[i]);
        if (!p.present || p.self) continue;
        for (size_t k = 0; k < rows.size(); ++k) {
            if (rows[k].pid == p.pid) {
                kids[k].push_back(i);
                break;
            }
        }
    }

    // Iterative depth-first with a visited set, so a cycle cannot loop for
    // ever and the answer is deterministic in row order.
    std::vector<bool> visited(rows.size(), false);
    std::vector<size_t> stack;
    stack.push_back(rootRow);
    while (!stack.empty()) {
        const size_t cur = stack.back();
        stack.pop_back();
        if (visited[cur]) continue;   // reached twice: the second path cycles
        visited[cur] = true;
        // Pushed back-to-front so the smaller index is popped first and the
        // pre-sort order is the natural one.
        for (size_t c = kids[cur].size(); c-- > 0;) {
            stack.push_back(kids[cur][c]);
        }
    }
    // Row order, roots included - the root is IN its own subtree, and
    // emitting it again from the walk would double it. This loop is the only
    // emitter, which is what keeps that impossible.
    for (size_t i = 0; i < rows.size(); ++i) {
        if (visited[i]) out.push_back(i);
    }
    return out;
}

bool IsAncestorOf(const std::vector<TreeNodeInfo>& rows, DWORD pid,
                  DWORD descendantPid) {
    if (pid == 0 || descendantPid == 0 || pid == descendantPid) return false;
    if (rows.empty()) return false;

    // Follow parents UPWARD rather than children downward: the answer is the
    // same and the walk is bounded by the descendant's own chain depth. A
    // cycle in the children direction would never terminate that way; this
    // way the bound is one more than the number of rows.
    DWORD cur = descendantPid;
    for (size_t steps = 0; steps <= rows.size() + 1; ++steps) {
        size_t idx = rows.size();
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].pid == cur) {
                idx = i;
                break;
            }
        }
        if (idx == rows.size()) return false;
        const Parent p = RowParent(rows[idx]);
        if (!p.present) return false;
        if (p.self) return false;   // a self-parent cannot lead anywhere new
        if (p.pid == pid) return true;
        cur = p.pid;
    }
    return false;
}

}  // namespace wintcp

