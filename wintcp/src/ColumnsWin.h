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

// The persisted column mask is a UINT32, so COL_COUNT may not exceed 32 - and
// the obvious spelling of the mask is a trap at exactly that value:
//
//     ((1u << COL_COUNT) - 1u)
//
// is `1u << 32` once there are 32 columns. That is undefined behaviour, and on
// MSVC it both warns (C4293) and evaluates to 1, so the mask becomes 0 and
// ClampVisibleCols collapses every visible column to a single bit - a persisted,
// silent and catastrophic default that no "the default set is non-empty" test
// would catch, because 1 IS non-empty.
//
// F5.1-F5.3 took COL_COUNT from 29 to exactly 32, so this is now load-bearing
// rather than theoretical, and /WX turns the C4293 into a build failure - which
// is how it was caught. The 64-bit shift below is well defined at 32 and is cast
// back down; the assert is the reminder that a 33rd column needs the stored mask
// widened first, not just a bigger shift.
static_assert(COL_COUNT <= 32,
              "ColumnId values are persisted in a UINT32 mask, and kAllColMask "
              "below caps at 32. Widen the stored mask (and Settings' "
              "ColVersion migration) BEFORE adding a 33rd column.");

// DECISION 2026-10-06 (9.1.1): FREEZE AT 32. Not "we ran out of room" - 32 was
// chosen over widening, and this is the note saying so at the place a 33rd
// column would be added.
//
// Why freeze rather than widen:
//   - There is no 33rd column. Nothing today is blocked by this.
//   - Widening is not one change. It is at least eight: the UINT32 mask, the
//     registry value from REG_DWORD to REG_BINARY at 8 bytes, a
//     kCurrentColVersion 6 migration (with the v4 and v5 precedents to follow),
//     the IDM_COL range in resource.h, and ColumnTitle / GetColumnText /
//     CompareRows / JsonKeyFor / GroupedDefaultColumns, plus the
//     `help list --columns` prose and its golden asserts.
//   - Every one of those is a place a persisted user layout can change
//     silently, which is the failure mode nobody would think to test for.
//
// So a feature that needs a column reuses an existing one rather than growing
// the mask as a side effect. ASN (9.5.1) is the live case: it reuses Country.
// A 33rd column is a deliberate act - widen the mask and migrate first, in its
// own commit - not something that arrives with a feature.
//
// The assert above is the enforcement: it names this at the point of failure,
// so "add a column" cannot be completed without reading the reason.
constexpr unsigned kMaxPersistedColumns = 32;

// Mask restricted to the columns that actually exist, with at least one bit
// set: a mask of 0 would persist a list view with no columns at all and no
// way back except the Columns menu.
constexpr UINT32 kAllColMask =
    static_cast<UINT32>((static_cast<UINT64>(1) << COL_COUNT) - 1u);

// The mask is exactly full at 32, so this is a real equality rather than a
// formality: it fails today if a column is ever removed, which would silently
// renumber every persisted bit and shift every user's saved layout. Spelled as
// a named constant so the two halves of the decision cannot disagree.
static_assert(COL_COUNT == kMaxPersistedColumns,
              "The 32-column freeze was decided together (9.1.1). Removing a "
              "column renumbers the persisted mask, so a removal needs the same "
              "migration a widening would.");

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
    kAllColMask & ~(1u << COL_RX) &
    ~(1u << COL_TX) & ~(1u << COL_NETTOTAL) & ~(1u << COL_MEM) &
    ~(1u << COL_DISK) & ~(1u << COL_BANDWIDTH) & ~(1u << COL_TLS) &
    ~(1u << COL_COUNTRY) & ~(1u << COL_RTT) & ~(1u << COL_MINRTT) &
    ~(1u << COL_CWND) & ~(1u << COL_RETRANS) & ~(1u << COL_GROUPRATE) &
    // F5.1/F5.2/F5.3 join TLS and Country as opt-in enrichment: a parent id,
    // an integrity level and a signature verdict are what you switch on to
    // audit a few processes, not three more columns of mostly em-dashes spread
    // across three hundred rows.
    ~(1u << COL_PPID) & ~(1u << COL_INTEGRITY) & ~(1u << COL_SIGNATURE) &
    // 5.5: the Note column is HIDDEN by default, unlike the Bookmarks column
    // beside it. A pin or a colour tag is a two-character signal worth seeing
    // always; a note is prose that is empty on nearly every row, and a column
    // of em-dashes costs horizontal space that the connection identity needs.
    // It is one click away in View > Columns, and `note:` filters without it.
    ~(1u << COL_NOTE);

inline UINT32 ClampVisibleCols(UINT32 mask) {
    mask &= kAllColMask;
    return (mask == 0) ? 1u : mask;
}

}  // namespace wintcp
