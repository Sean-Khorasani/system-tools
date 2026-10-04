// ColumnsWin.h
// The parts of the column definitions that need a Windows type: the visible
// column mask and the clamp that keeps it non-zero.
//
// They cannot stay in Columns.h. That header is a deliberate, windows-free
// island so a settings/registry consumer can learn COL_COUNT without pulling
// in the whole model, and it is included at global scope by Settings.h and
// ConnectionStore.h *before* either of them includes windows.h - so UINT32
// simply is not declared yet at that point. Everything here is therefore
// after a real windows.h, and the values are unchanged from where they used
// to live: only the location moved, so the persisted column mask keeps its
// meaning across the split.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>

#include "Columns.h"

namespace wintcp {

// Default visible-column mask. Everything the user can act on out of the box
// is shown: identity, the live per-process stats, the combined Traffic
// column, a connection's age and its bookmark state. The split per-PID
// Received / Sent / Net-total columns are redundant next to Traffic, and
// Memory / Disk I/O / Speed / TLS / Country are opt-in enrichment. Must stay
// in step with the ColVersion migration in Settings.cpp.
//
// G6's four `ss -i` columns and G5's per-process rate are hidden here for the
// same reason TLS and Country are, and with the same reasoning as the ColVersion
// 4 migration: they are DIAGNOSTIC. RTT and retransmit counts are what you want
// while chasing one slow connection and are four columns of mostly em-dashes to
// everyone else - and on a machine whose socket scan could not read them, a
// default-visible set would look broken on first launch.
constexpr UINT32 kDefaultVisibleCols =
    ((1u << COL_COUNT) - 1u) & ~(1u << COL_RX) &
    ~(1u << COL_TX) & ~(1u << COL_NETTOTAL) & ~(1u << COL_MEM) &
    ~(1u << COL_DISK) & ~(1u << COL_BANDWIDTH) & ~(1u << COL_TLS) &
    ~(1u << COL_COUNTRY) & ~(1u << COL_RTT) & ~(1u << COL_MINRTT) &
    ~(1u << COL_CWND) & ~(1u << COL_RETRANS) & ~(1u << COL_GROUPRATE) &
    // 5.5: the Note column is HIDDEN by default, unlike the Bookmarks column
    // beside it. A pin or a colour tag is a two-character signal worth seeing
    // always; a note is prose that is empty on nearly every row, and a column
    // of em-dashes costs horizontal space that the connection identity needs.
    // It is one click away in View > Columns, and `note:` filters without it.
    ~(1u << COL_NOTE);

// Mask restricted to the columns that actually exist, with at least one bit
// set: a mask of 0 would persist a list view with no columns at all and no
// way back except the Columns menu.
constexpr UINT32 kAllColMask = (1u << COL_COUNT) - 1u;

inline UINT32 ClampVisibleCols(UINT32 mask) {
    mask &= kAllColMask;
    return (mask == 0) ? 1u : mask;
}

}  // namespace wintcp
