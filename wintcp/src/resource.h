// resource.h
// Control / menu / icon identifiers for wintcp.

#pragma once

#define IDI_APP                 100

#define IDR_MAINMENU            1000
#define IDR_ACCEL               1001

#define IDC_LIST                1100
#define IDC_STATUS              1101
#define IDC_BTN_REFRESH         1102
#define IDC_BTN_EXPORT          1103
#define IDC_CHK_AUTOREFRESH     1104
#define IDC_CBO_INTERVAL        1105
#define IDC_CBO_PROTO           1106
#define IDC_CBO_STATE           1107
#define IDC_EDIT_SEARCH         1108
#define IDC_STATIC_FILTER       1109

#define IDM_FILE_REFRESH        40001
#define IDM_FILE_EXPORT         40002
#define IDM_FILE_EXIT           40003
#define IDM_VIEW_AUTOREFRESH    40004
#define IDM_HELP_ABOUT          40005
#define IDM_VIEW_RESOLVE        40006
#define IDM_EDIT_SELECTALL      40007
#define IDM_EDIT_FOCUSFILTER    40008
#define IDM_EDIT_CLEARFILTER    40009
#define IDM_FILE_CHANGELOG      40010
#define IDM_CTX_EXPORT_SELECTION 40011
// 40012 was IDM_VIEW_DARK; the app now follows the system theme instead of
// offering a dark-mode toggle. The slot stays reserved so the remaining
// command IDs keep their values.
#define IDM_VIEW_TRAY           40013
#define IDM_VIEW_TOPMOST        40014
#define IDM_TRAY_RESTORE        40015
#define IDM_VIEW_TRAFFIC        40016
#define IDM_VIEW_CHARTS         40017

// 7.4. Placed after the existing View ids so no existing command number
// shifts - renumbering them would break any accelerator table referring to
// them by literal value.
#define IDM_VIEW_PRESERVE_SEL   40018

// 5.5 freeze and 5.1 grouping. After the existing View ids so no existing
// command number shifts.
#define IDM_VIEW_FREEZE         40019
#define IDM_VIEW_GROUP          40020

// 7.6: the shortcuts sheet, and 7.5 the About box split out so F1 can open
// the sheet while the About dialog keeps its own command id.
#define IDM_HELP_SHORTCUTS      40021
#define IDM_VIEW_GEOIP          40022

// 5.2 preset commands and 5.3 bookmark commands, wired into the GUI.
#define IDM_FILE_PRESET_SAVE    40023
#define IDM_FILE_PRESET_LOAD    40024
#define IDM_FILE_PRESET_DELETE  40025
#define IDM_FILE_BOOKMARKS      40026
#define IDM_CTX_BOOKMARK        40027
#define IDM_CTX_NOTE            40028
#define IDM_FILE_CHANGELOG_WIN  40029

#define IDM_CTX_COPY_SELECTED   40101
#define IDM_CTX_COPY_ALL        40102
#define IDM_CTX_DETAILS         40103
#define IDM_CTX_KILL_PROCESS    40104
#define IDM_CTX_REFRESH         40105
#define IDM_CTX_CLOSE_CONNECTION 40106

// Column-visibility menu: one item per ColumnId, in order.
#define IDM_COL_BASE            40200
#define IDM_COL_PROTO           (IDM_COL_BASE + 0)
#define IDM_COL_LOCAL           (IDM_COL_BASE + 1)
#define IDM_COL_LPORT           (IDM_COL_BASE + 2)
#define IDM_COL_REMOTE          (IDM_COL_BASE + 3)
#define IDM_COL_RPORT           (IDM_COL_BASE + 4)
#define IDM_COL_STATE           (IDM_COL_BASE + 5)
#define IDM_COL_PID             (IDM_COL_BASE + 6)
#define IDM_COL_PROCESS         (IDM_COL_BASE + 7)
#define IDM_COL_SERVICE         (IDM_COL_BASE + 8)
#define IDM_COL_HOST            (IDM_COL_BASE + 9)
#define IDM_COL_PATH            (IDM_COL_BASE + 10)
#define IDM_COL_TRAFFIC         (IDM_COL_BASE + 11)
#define IDM_COL_RX              (IDM_COL_BASE + 12)
#define IDM_COL_TX              (IDM_COL_BASE + 13)
#define IDM_COL_NETTOTAL        (IDM_COL_BASE + 14)
#define IDM_COL_CPU             (IDM_COL_BASE + 15)
#define IDM_COL_MEM             (IDM_COL_BASE + 16)
#define IDM_COL_DISK            (IDM_COL_BASE + 17)
#define IDM_COL_DURATION        (IDM_COL_BASE + 18)
#define IDM_COL_BANDWIDTH       (IDM_COL_BASE + 19)
#define IDM_COL_TLS             (IDM_COL_BASE + 20)
#define IDM_COL_COUNTRY         (IDM_COL_BASE + 21)
#define IDM_COL_PINNED          (IDM_COL_BASE + 22)
// G6: the `ss -i` columns, and G5's per-process rate. Appended, never
// renumbered - the ordinal is baked into nothing persistent, but keeping the
// existing values stable means a diff of this file shows only additions.
#define IDM_COL_RTT             (IDM_COL_BASE + 23)
#define IDM_COL_MINRTT          (IDM_COL_BASE + 24)
#define IDM_COL_CWND            (IDM_COL_BASE + 25)
#define IDM_COL_RETRANS         (IDM_COL_BASE + 26)
#define IDM_COL_GROUPRATE       (IDM_COL_BASE + 27)
// 5.5: the bookmark note as its own column.
#define IDM_COL_NOTE            (IDM_COL_BASE + 28)
// F5.1/F5.2/F5.3: the parent id, the mandatory integrity level and the
// Authenticode verdict. Appended, never renumbered, for the reason the G6 block
// above gives.
#define IDM_COL_PPID            (IDM_COL_BASE + 29)
#define IDM_COL_INTEGRITY       (IDM_COL_BASE + 30)
#define IDM_COL_SIGNATURE       (IDM_COL_BASE + 31)
// One past the last column command. Checked against COL_COUNT at compile
// time in MainWindow.cpp so a column added without a menu entry is an error
// rather than a checkbox that never appears.
#define IDM_COL_COUNT           32

// Row context menu. Placed after the column block so the
// IDM_COL_BASE..+COUNT range check cannot swallow it.
#define IDM_FOLLOW_STREAM       40060
#define IDM_OPEN_FILE_LOCATION  40061
#define IDM_PROCESS_PROPERTIES  40062
#define IDM_BLOCK_CONNECTION    40063
// D30: tray menu - remove every rule `block` ever created (the item the
// Block confirmation has always pointed at; it did not exist until now).
#define IDM_TRAY_UNBLOCK_ALL   40064

#define IDS_APP_TITLE           50000
