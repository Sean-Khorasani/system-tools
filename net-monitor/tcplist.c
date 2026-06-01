/******************************************************************************
 *  tcplist.c — TCP/IP Connection Monitor for Windows 10+
 *
 *  Displays all active TCP/UDP connections grouped by owning process in an
 *  expandable TreeView. Uses only native Windows system calls — no
 *  frameworks, no MFC, no ATL.
 *
 *  Features:
 *    - TCPv4, TCPv6, UDPv4, UDPv6 connections
 *    - Process name resolution via CreateToolhelp32Snapshot
 *    - Auto-refresh (configurable interval)
 *    - Manual refresh (F5)
 *    - Status bar with live statistics
 *    - Sortable, resizable, DPI-aware
 *
 *  Build (VS 2022 x64 Release):
 *    cl /nologo /O1 /Os /MT /GS- /Gy /GL /W4 /WX- tcplist.c tcplist.rc ^
 *       /link /OPT:REF /OPT:ICF /LTCG /ALIGN:16 ^
 *       kernel32.lib user32.lib comctl32.lib iphlpapi.lib advapi32.lib
 *
 *  The resulting .exe is a single, self-contained file under 64 KB.
 ******************************************************************************/

/* ---- Configuration macros ----------------------------------------------- */
#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif
#ifndef WINVER
#  define WINVER              0x0A00   /* Windows 10+             */
#endif
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT        0x0A00
#endif

#define APP_TITLE             L"TCP/IP Connection Monitor"
#define APP_CLASS             L"TCPListMainWnd"
#define IDT_REFRESH           100       /* Timer ID                */
#define DEFAULT_INTERVAL_MS   2000      /* Auto-refresh interval   */
#define MAX_PROCESSES         2048
#define MAX_CONNECTIONS       32768
#define TREE_PADDING          24        /* TVS_EX level indentation*/
#define IDC_FILTER            300       /* Filter edit control ID */
#define FILTER_HEIGHT         22        /* Filter bar height      */

#ifndef ARRAYSIZE
#  define ARRAYSIZE(x)  (sizeof(x) / sizeof((x)[0]))
#endif

/* ---- Windows headers ---------------------------------------------------- */
/* winsock2.h MUST precede windows.h to avoid Winsock 1.1 conflicts */
#include <winsock2.h>

#define WIN32_LEAN_AND_MEAN
/* We use menus, so don't define NOMENUS.  Keep size-saving defines.  */
#define NOGDICAPMASKS
#define NOSYSCOMMANDS
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

/* MIB_TCP* / GetExtendedTcpTable / etc. */
#include <ws2tcpip.h>    /* IN6_ADDR etc. required by tcpmib.h IPv6 types */
#include <tcpmib.h>
#include <iphlpapi.h>
/* Process enumeration */
#include <tlhelp32.h>
#ifndef NO_CRT
#  include <stdlib.h>   /* not actually used — InsertionSort replaces qsort */
#endif

/* --------------------------------------------------------------------------
 *  IPv6 MIB types — forward-declared for SDKs that guard these too tightly
 *  (harmless no-op if the SDK typedefs already exist).
 * ------------------------------------------------------------------------ */
typedef struct _MY_MIB_TCP6ROW_OWNER_PID {
    UCHAR ucLocalAddr[16];
    DWORD dwLocalScopeId;
    DWORD dwLocalPort;
    UCHAR ucRemoteAddr[16];
    DWORD dwRemoteScopeId;
    DWORD dwRemotePort;
    DWORD dwState;
    DWORD dwOwningPid;
} MY_MIB_TCP6ROW_OWNER_PID;

typedef struct _MY_MIB_UDP6ROW_OWNER_PID {
    UCHAR ucLocalAddr[16];
    DWORD dwLocalScopeId;
    DWORD dwLocalPort;
    DWORD dwOwningPid;
} MY_MIB_UDP6ROW_OWNER_PID;

/* ---- Memory block helpers (avoid memcpy/memset dependency) -------------- */
/* Always use loops instead of CRT memcpy/memset so NO_CRT builds work.    */
static void CopyBytes(void *dst, const void *src, size_t n)
{
    BYTE *d = (BYTE*)dst;
    const BYTE *s = (const BYTE*)src;
    while (n--) *d++ = *s++;
}
static void ZeroBytes(void *dst, size_t n)
{
    BYTE *d = (BYTE*)dst;
    while (n--) *d++ = 0;
}

/* ---- Linker directives for minimal binary ------------------------------- */
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")

#pragma comment(linker, "/MERGE:.rdata=.text")
#pragma comment(linker, "/MERGE:.pdata=.text")

/* --------------------------------------------------------------------------
 *  T Y P E   D E F I N I T I O N S
 * ------------------------------------------------------------------------ */

typedef enum { PROTO_TCP = 6, PROTO_UDP = 17 } PROTOCOL;

typedef struct _MY_TCP_STATE {
    DWORD  code;
    LPCWSTR name;
} MY_TCP_STATE;

/* Avoid ws2_32.lib dependency — implement htons inline */
static __forceinline WORD MyHtons(WORD v)
{
    return (WORD)(((v & 0xFF) << 8) | ((v & 0xFF00) >> 8));
}

/* Compact per-connection record (fits in one cache line on x64) */
typedef struct {
    DWORD  local_addr[4];     /* IPv6-capable storage (16 bytes)  */
    DWORD  remote_addr[4];
    DWORD  local_port;        /* Host byte order                   */
    DWORD  remote_port;
    DWORD  pid;
    DWORD  state;             /* MIB_TCP_STATE or 0 for UDP       */
    WORD   flags;             /* bit0: isIPv6, bit1-3: protocol   */
} CONN_ENTRY;

/* Per-process grouping record */
typedef struct {
    DWORD  pid;
    WCHAR  name[64];          /* Executable name (not full path)   */
    int    conn_count;        /* # connections owned               */
    int    listen_count;      /* # listening TCP sockets           */
    int    conn_idx;          /* First connection index in array   */
} PROC_ENTRY;

/* --------------------------------------------------------------------------
 *  G L O B A L   S T A T E
 * ------------------------------------------------------------------------ */
static HINSTANCE   g_hInst;                 /* Application instance          */
static HWND        g_hMainWnd;              /* Main window handle            */
static HWND        g_hTree;                 /* TreeView handle               */
static HWND        g_hFilter;               /* Filter edit control           */
static HWND        g_hStatus;               /* Status bar handle             */
static HFONT       g_hFont;                 /* Monospace font for alignment   */
static HMENU       g_hMenu;                 /* Main menu                     */
static CONN_ENTRY *g_pConns;               /* Dynamic connection array       */
static PROC_ENTRY *g_pProcs;               /* Dynamic process array          */
static int         g_connCount;            /* Total connections enumerated   */
static int         g_procCount;            /* Total unique processes         */
static int         g_refreshMs = DEFAULT_INTERVAL_MS;
static BOOL        g_autoRefresh = TRUE;
static BOOL        g_sortAscending = TRUE;
static int          g_sortMode = 0;           /* 0=name, 1=connection count */
static BOOL         g_allExpanded = TRUE;    /* Persist expand/collapse state */
static WCHAR        g_filterText[128];       /* Filter text (always lowered) */

/* TCP state name table */
static const MY_TCP_STATE g_tcpStates[] = {
    { MIB_TCP_STATE_CLOSED,     L"CLOSED"      },
    { MIB_TCP_STATE_LISTEN,     L"LISTENING"    },
    { MIB_TCP_STATE_SYN_SENT,   L"SYN_SENT"     },
    { MIB_TCP_STATE_SYN_RCVD,   L"SYN_RCVD"     },
    { MIB_TCP_STATE_ESTAB,      L"ESTABLISHED"  },
    { MIB_TCP_STATE_FIN_WAIT1,  L"FIN_WAIT1"    },
    { MIB_TCP_STATE_FIN_WAIT2,  L"FIN_WAIT2"    },
    { MIB_TCP_STATE_CLOSE_WAIT, L"CLOSE_WAIT"   },
    { MIB_TCP_STATE_CLOSING,    L"CLOSING"      },
    { MIB_TCP_STATE_LAST_ACK,   L"LAST_ACK"     },
    { MIB_TCP_STATE_TIME_WAIT,  L"TIME_WAIT"    },
    { MIB_TCP_STATE_DELETE_TCB, L"DELETE_TCB"   },
};

/* --------------------------------------------------------------------------
 *  U T I L I T Y   F U N C T I O N S
 * ------------------------------------------------------------------------ */

static LPCWSTR GetTcpStateName(DWORD state)
{
    for (int i = 0; i < ARRAYSIZE(g_tcpStates); i++)
        if (g_tcpStates[i].code == state)
            return g_tcpStates[i].name;
    return L"UNKNOWN";
}

/* Write IPv4/IPv6 address + port into dst.  Must hold at least 64 chars. */
static void FormatEndpoint(WCHAR *dst, DWORD *addr, int isIPv6, DWORD port)
{
    if (isIPv6) {
        /* IPv6 — use RtlIpv6AddressToStringW via ws2_32 or format manually.
         * We use WSAAddressToStringW indirectly here.  For simplicity and
         * zero extra dependencies, manually format as colon-hex.          */
        WORD *w = (WORD*)addr;
        /* Check for IPv4-mapped IPv6 (::ffff:a.b.c.d) */
        if (addr[0] == 0 && addr[1] == 0 && addr[2] == 0xFFFF0000) {
            BYTE *b = (BYTE*)&addr[3];
            wsprintfW(dst, L"%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], port);
        } else {
            wsprintfW(dst, L"[%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x]:%u",
                      w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], port);
        }
    } else {
        BYTE *b = (BYTE*)addr;
        wsprintfW(dst, L"%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], port);
    }
}

/* Format a connection tree-item string: "PROTO  local → remote  STATE" */
static void FormatConnItem(WCHAR *buf, int cch, CONN_ENTRY *c)
{
    WCHAR local[64], remote[64];
    UNREFERENCED_PARAMETER(cch);
    LPCWSTR proto = (c->flags & 2) ? L"UDP" : L"TCP";  /* bit1 = UDP flag */
    int isIPv6 = (c->flags & 1);

    FormatEndpoint(local, c->local_addr, isIPv6, c->local_port);

    if (c->flags & 2) {
        /* UDP — no remote or state */
        wsprintfW(buf, L"%-4s %-53s  ->  *:*", proto, local);
    } else {
        FormatEndpoint(remote, c->remote_addr, isIPv6, c->remote_port);
        LPCWSTR state = GetTcpStateName(c->state);
        wsprintfW(buf, L"%-4s %-53s  ->  %-53s  %s",
                  proto, local, remote, state);
    }
}

/* Format a process tree-item string */
static void FormatProcItem(WCHAR *buf, int cch, PROC_ENTRY *p)
{
    UNREFERENCED_PARAMETER(cch);
    wsprintfW(buf, L"%s  (PID: %lu)  --  %d conn%s, %d listening",
              p->name, p->pid, p->conn_count,
              p->conn_count == 1 ? L"" : L"s",
              p->listen_count);
}

/* --------------------------------------------------------------------------
 *  P R O C E S S   N A M E   R E S O L U T I O N
 * ------------------------------------------------------------------------ */

typedef struct {
    DWORD pid;
    WCHAR name[64];
} PID_NAME_ENTRY;

static PID_NAME_ENTRY *g_pidNames = NULL;
static int             g_pidNameCount = 0;

static void BuildProcessNameMap(void)
{
    HANDLE hSnapshot;
    PROCESSENTRY32W pe;
    int cap = 512;

    /* Free previous */
    if (g_pidNames) {
        HeapFree(GetProcessHeap(), 0, g_pidNames);
        g_pidNames = NULL;
    }

    g_pidNames = (PID_NAME_ENTRY*)HeapAlloc(GetProcessHeap(),
                                             HEAP_ZERO_MEMORY,
                                             cap * sizeof(PID_NAME_ENTRY));
    if (!g_pidNames) return;
    g_pidNameCount = 0;

    hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            if (g_pidNameCount >= cap) {
                cap *= 2;
                PID_NAME_ENTRY *tmp = (PID_NAME_ENTRY*)HeapReAlloc(
                    GetProcessHeap(), HEAP_ZERO_MEMORY, g_pidNames,
                    cap * sizeof(PID_NAME_ENTRY));
                if (!tmp) break;
                g_pidNames = tmp;
            }
            g_pidNames[g_pidNameCount].pid = pe.th32ProcessID;
            lstrcpynW(g_pidNames[g_pidNameCount].name,
                      pe.szExeFile, ARRAYSIZE(g_pidNames[0].name) - 1);
            g_pidNameCount++;
        } while (Process32NextW(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
}

static LPCWSTR LookupProcessName(DWORD pid)
{
    for (int i = 0; i < g_pidNameCount; i++)
        if (g_pidNames[i].pid == pid)
            return g_pidNames[i].name;
    return L"???";
}

/* --------------------------------------------------------------------------
 *  C O N N E C T I O N   E N U M E R A T I O N
 * ------------------------------------------------------------------------ */

static BOOL EnumTcpTableV4(CONN_ENTRY **conns, int *pCount, int *pCap)
{
    PMIB_TCPTABLE_OWNER_PID pTcp = NULL;
    DWORD dwSize = 0;
    DWORD ret;

    /* First call to get required buffer size */
    ret = GetExtendedTcpTable(NULL, &dwSize, FALSE, AF_INET,
                              TCP_TABLE_OWNER_PID_ALL, 0);
    if (ret != ERROR_INSUFFICIENT_BUFFER) return FALSE;

    pTcp = (PMIB_TCPTABLE_OWNER_PID)HeapAlloc(GetProcessHeap(), 0, dwSize);
    if (!pTcp) return FALSE;

    ret = GetExtendedTcpTable(pTcp, &dwSize, FALSE, AF_INET,
                              TCP_TABLE_OWNER_PID_ALL, 0);
    if (ret != NO_ERROR) {
        HeapFree(GetProcessHeap(), 0, pTcp);
        return FALSE;
    }

    for (DWORD i = 0; i < pTcp->dwNumEntries; i++) {
        if (*pCount >= *pCap) {
            *pCap *= 2;
            CONN_ENTRY *tmp = (CONN_ENTRY*)HeapReAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY, *conns,
                *pCap * sizeof(CONN_ENTRY));
            if (!tmp) break;
            *conns = tmp;
        }

        CONN_ENTRY *c = &(*conns)[*pCount];
        ZeroBytes(c, sizeof(*c));
        c->local_addr[0] = pTcp->table[i].dwLocalAddr;
        c->remote_addr[0] = pTcp->table[i].dwRemoteAddr;
        c->local_port = MyHtons((WORD)pTcp->table[i].dwLocalPort);
        c->remote_port = MyHtons((WORD)pTcp->table[i].dwRemotePort);
        c->pid = pTcp->table[i].dwOwningPid;
        c->state = pTcp->table[i].dwState;
        c->flags = 0; /* TCPv4 */
        (*pCount)++;
    }

    HeapFree(GetProcessHeap(), 0, pTcp);
    return TRUE;
}

static BOOL EnumTcpTableV6(CONN_ENTRY **conns, int *pCount, int *pCap)
{
    PVOID pRaw = NULL;
    DWORD dwSize = 0;
    DWORD ret;

    ret = GetExtendedTcpTable(NULL, &dwSize, FALSE, AF_INET6,
                              TCP_TABLE_OWNER_PID_ALL, 0);
    if (ret != ERROR_INSUFFICIENT_BUFFER) return FALSE;

    pRaw = HeapAlloc(GetProcessHeap(), 0, dwSize);
    if (!pRaw) return FALSE;

    ret = GetExtendedTcpTable(pRaw, &dwSize, FALSE, AF_INET6,
                              TCP_TABLE_OWNER_PID_ALL, 0);
    if (ret != NO_ERROR) {
        HeapFree(GetProcessHeap(), 0, pRaw);
        return FALSE;
    }

    DWORD dwNumEntries = *(DWORD*)pRaw;
    /* Rows start right after the DWORD count */
    MY_MIB_TCP6ROW_OWNER_PID *rows =
        (MY_MIB_TCP6ROW_OWNER_PID*)((BYTE*)pRaw + sizeof(DWORD));

    for (DWORD i = 0; i < dwNumEntries; i++) {
        if (*pCount >= *pCap) {
            *pCap *= 2;
            CONN_ENTRY *tmp = (CONN_ENTRY*)HeapReAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY, *conns,
                *pCap * sizeof(CONN_ENTRY));
            if (!tmp) break;
            *conns = tmp;
        }

        CONN_ENTRY *c = &(*conns)[*pCount];
        ZeroBytes(c, sizeof(*c));
        CopyBytes(c->local_addr, rows[i].ucLocalAddr, 16);
        CopyBytes(c->remote_addr, rows[i].ucRemoteAddr, 16);
        c->local_port = MyHtons((WORD)rows[i].dwLocalPort);
        c->remote_port = MyHtons((WORD)rows[i].dwRemotePort);
        c->pid = rows[i].dwOwningPid;
        c->state = rows[i].dwState;
        c->flags = 1; /* TCPv6 */
        (*pCount)++;
    }

    HeapFree(GetProcessHeap(), 0, pRaw);
    return TRUE;
}

static BOOL EnumUdpTableV4(CONN_ENTRY **conns, int *pCount, int *pCap)
{
    PMIB_UDPTABLE_OWNER_PID pUdp = NULL;
    DWORD dwSize = 0;
    DWORD ret;

    ret = GetExtendedUdpTable(NULL, &dwSize, FALSE, AF_INET,
                              UDP_TABLE_OWNER_PID, 0);
    if (ret != ERROR_INSUFFICIENT_BUFFER) return FALSE;

    pUdp = (PMIB_UDPTABLE_OWNER_PID)HeapAlloc(GetProcessHeap(), 0, dwSize);
    if (!pUdp) return FALSE;

    ret = GetExtendedUdpTable(pUdp, &dwSize, FALSE, AF_INET,
                              UDP_TABLE_OWNER_PID, 0);
    if (ret != NO_ERROR) {
        HeapFree(GetProcessHeap(), 0, pUdp);
        return FALSE;
    }

    for (DWORD i = 0; i < pUdp->dwNumEntries; i++) {
        if (*pCount >= *pCap) {
            *pCap *= 2;
            CONN_ENTRY *tmp = (CONN_ENTRY*)HeapReAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY, *conns,
                *pCap * sizeof(CONN_ENTRY));
            if (!tmp) break;
            *conns = tmp;
        }

        CONN_ENTRY *c = &(*conns)[*pCount];
        ZeroBytes(c, sizeof(*c));
        c->local_addr[0] = pUdp->table[i].dwLocalAddr;
        /* UDP has no remote address / state — mark appropriately */
        c->remote_addr[0] = 0;
        c->local_port = MyHtons((WORD)pUdp->table[i].dwLocalPort);
        c->remote_port = 0;
        c->pid = pUdp->table[i].dwOwningPid;
        c->state = 0;
        c->flags = 2; /* UDPv4 — bit1 = UDP */
        (*pCount)++;
    }

    HeapFree(GetProcessHeap(), 0, pUdp);
    return TRUE;
}

static BOOL EnumUdpTableV6(CONN_ENTRY **conns, int *pCount, int *pCap)
{
    PVOID pRaw = NULL;
    DWORD dwSize = 0;
    DWORD ret;

    ret = GetExtendedUdpTable(NULL, &dwSize, FALSE, AF_INET6,
                              UDP_TABLE_OWNER_PID, 0);
    if (ret != ERROR_INSUFFICIENT_BUFFER) return FALSE;

    pRaw = HeapAlloc(GetProcessHeap(), 0, dwSize);
    if (!pRaw) return FALSE;

    ret = GetExtendedUdpTable(pRaw, &dwSize, FALSE, AF_INET6,
                              UDP_TABLE_OWNER_PID, 0);
    if (ret != NO_ERROR) {
        HeapFree(GetProcessHeap(), 0, pRaw);
        return FALSE;
    }

    DWORD dwNumEntries = *(DWORD*)pRaw;
    MY_MIB_UDP6ROW_OWNER_PID *rows =
        (MY_MIB_UDP6ROW_OWNER_PID*)((BYTE*)pRaw + sizeof(DWORD));

    for (DWORD i = 0; i < dwNumEntries; i++) {
        if (*pCount >= *pCap) {
            *pCap *= 2;
            CONN_ENTRY *tmp = (CONN_ENTRY*)HeapReAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY, *conns,
                *pCap * sizeof(CONN_ENTRY));
            if (!tmp) break;
            *conns = tmp;
        }

        CONN_ENTRY *c = &(*conns)[*pCount];
        ZeroBytes(c, sizeof(*c));
        CopyBytes(c->local_addr, rows[i].ucLocalAddr, 16);
        c->local_port = MyHtons((WORD)rows[i].dwLocalPort);
        c->remote_port = 0;
        c->pid = rows[i].dwOwningPid;
        c->state = 0;
        c->flags = 3; /* UDPv6 — bit0=IPv6, bit1=UDP */
        (*pCount)++;
    }

    HeapFree(GetProcessHeap(), 0, pRaw);
    return TRUE;
}

/* Enumerate all connections into a dynamic array.
 * Returns TRUE on success. */
static BOOL EnumerateAllConnections(void)
{
    CONN_ENTRY *conns = NULL;
    int cap = 1024;
    int count = 0;

    /* Free previous data */
    if (g_pConns) {
        HeapFree(GetProcessHeap(), 0, g_pConns);
        g_pConns = NULL;
    }
    if (g_pProcs) {
        HeapFree(GetProcessHeap(), 0, g_pProcs);
        g_pProcs = NULL;
    }
    g_connCount = 0;
    g_procCount = 0;

    conns = (CONN_ENTRY*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   cap * sizeof(CONN_ENTRY));
    if (!conns) return FALSE;

    /* Enumerate all 4 tables */
    EnumTcpTableV4(&conns, &count, &cap);
    EnumTcpTableV6(&conns, &count, &cap);
    EnumUdpTableV4(&conns, &count, &cap);
    EnumUdpTableV6(&conns, &count, &cap);

    g_pConns = conns;
    g_connCount = count;
    return TRUE;
}

/* --------------------------------------------------------------------------
 *  P R O C E S S   G R O U P I N G   &   S O R T I N G
 * ------------------------------------------------------------------------ */

static int CompareConnsByPid(const void *a, const void *b)
{
    const CONN_ENTRY *ca = (const CONN_ENTRY*)a;
    const CONN_ENTRY *cb = (const CONN_ENTRY*)b;
    if (ca->pid != cb->pid) return (int)(ca->pid - cb->pid);
    /* Sort by protocol then port within a process */
    int pa = (ca->flags & 2) ? 1 : 0;
    int pb = (cb->flags & 2) ? 1 : 0;
    if (pa != pb) return pa - pb;
    return (int)(ca->local_port - cb->local_port);
}

static int CompareProcs(const void *a, const void *b)
{
    const PROC_ENTRY *pa = (const PROC_ENTRY*)a;
    const PROC_ENTRY *pb = (const PROC_ENTRY*)b;
    int cmp;
    if (g_sortMode == 1) {
        /* Sort by connection count, then by name */
        cmp = pb->conn_count - pa->conn_count;  /* descending by count */
        if (cmp == 0) cmp = lstrcmpiW(pa->name, pb->name);
    } else {
        /* Sort by name */
        cmp = lstrcmpiW(pa->name, pb->name);
    }
    return g_sortAscending ? cmp : -cmp;
}

/* Inline insertion sort — works without CRT, fast enough for < 2000 items */
static void InsertionSort(void *base, int count, int elemSize,
                          int (*cmp)(const void *, const void *))
{
    BYTE *b = (BYTE*)base;
    BYTE *tmp = (BYTE*)HeapAlloc(GetProcessHeap(), 0, elemSize);
    if (!tmp) return;

    for (int i = 1; i < count; i++) {
        CopyBytes(tmp, b + i * elemSize, elemSize);
        int j = i - 1;
        while (j >= 0 && cmp(b + j * elemSize, tmp) > 0) {
            CopyBytes(b + (j + 1) * elemSize, b + j * elemSize, elemSize);
            j--;
        }
        CopyBytes(b + (j + 1) * elemSize, tmp, elemSize);
    }

    HeapFree(GetProcessHeap(), 0, tmp);
}

/* Group sorted connections into per-process entries */
static void BuildProcessGroups(void)
{
    if (g_connCount == 0) return;

    /* Free previous process array before re-allocating */
    if (g_pProcs) {
        HeapFree(GetProcessHeap(), 0, g_pProcs);
        g_pProcs = NULL;
    }

    /* First sort connections by PID */
    InsertionSort(g_pConns, g_connCount, sizeof(CONN_ENTRY), CompareConnsByPid);

    /* First pass: count unique PIDs */
    DWORD prevPid = 0xFFFFFFFF;
    int nProcs = 0;

    for (int i = 0; i < g_connCount; i++) {
        if (g_pConns[i].pid != prevPid) {
            nProcs++;
            prevPid = g_pConns[i].pid;
        }
    }

    g_pProcs = (PROC_ENTRY*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      nProcs * sizeof(PROC_ENTRY));
    if (!g_pProcs) return;

    /* Second pass: build process entries */
    prevPid = 0xFFFFFFFF;
    int pi = -1;

    for (int i = 0; i < g_connCount; i++) {
        if (g_pConns[i].pid != prevPid) {
            pi++;
            g_pProcs[pi].pid = g_pConns[i].pid;
            g_pProcs[pi].conn_idx = i;
            g_pProcs[pi].conn_count = 0;
            g_pProcs[pi].listen_count = 0;
            lstrcpynW(g_pProcs[pi].name, LookupProcessName(g_pConns[i].pid), 63);
            prevPid = g_pConns[i].pid;
        }
        g_pProcs[pi].conn_count++;
        /* Count listening TCP sockets */
        if (!(g_pConns[i].flags & 2) && g_pConns[i].state == MIB_TCP_STATE_LISTEN)
            g_pProcs[pi].listen_count++;
    }
    g_procCount = pi + 1;

    /* Sort processes by selected sort mode */
    InsertionSort(g_pProcs, g_procCount, sizeof(PROC_ENTRY), CompareProcs);
}

/* --------------------------------------------------------------------------
 *  F I L T E R   M A T C H I N G
 * ------------------------------------------------------------------------ */

/* Case-insensitive substring search — no CRT dependency */
static BOOL MyStrStrI(LPCWSTR haystack, LPCWSTR needle)
{
    int nLen = lstrlenW(needle);
    if (nLen == 0) return TRUE;
    for (LPCWSTR h = haystack; *h; h++) {
        BOOL match = TRUE;
        for (int i = 0; i < nLen; i++) {
            WCHAR hc = h[i]; if (!hc) { match = FALSE; break; }
            WCHAR nc = needle[i];
            if (hc >= L'A' && hc <= L'Z') hc += (L'a' - L'A');
            if (nc >= L'A' && nc <= L'Z') nc += (L'a' - L'A');
            if (hc != nc) { match = FALSE; break; }
        }
        if (match) return TRUE;
    }
    return FALSE;
}

/* Check whether a connection matches all space-separated filter terms.
 * Searches: process name, PID, protocol, local addr:port, remote addr:port,
 *            state, raw PID as number string.                             */
static BOOL ConnectionMatchesFilter(CONN_ENTRY *conn, PROC_ENTRY *proc)
{
    if (g_filterText[0] == L'\0') return TRUE;

    WCHAR local[64], remote[64];
    int isIPv6 = (conn->flags & 1);
    LPCWSTR proto = (conn->flags & 2) ? L"udp" : L"tcp";

    FormatEndpoint(local,  conn->local_addr,  isIPv6, conn->local_port);
    FormatEndpoint(remote, conn->remote_addr, isIPv6, conn->remote_port);

    LPCWSTR state = (conn->flags & 2) ? L"" : GetTcpStateName(conn->state);

    WCHAR pidStr[16];
    wsprintfW(pidStr, L"%lu", proc->pid);

    /* Build a searchable blob: every field lowercased */
    WCHAR hay[512];
    wsprintfW(hay, L"%s %s %s %s %s %s",
              proc->name, proto, pidStr, local, remote, state);
    CharLowerW(hay);

    /* Walk space-separated filter terms */
    WCHAR filt[128];
    lstrcpynW(filt, g_filterText, ARRAYSIZE(filt));

    LPCWSTR p = filt;
    while (*p) {
        while (*p == L' ') p++;
        if (!*p) break;
        LPCWSTR end = p;
        while (*end && *end != L' ') end++;
        WCHAR saved = *end;
        *(WCHAR*)end = L'\0';

        if (!MyStrStrI(hay, p)) {
            *(WCHAR*)end = saved;
            return FALSE;
        }
        *(WCHAR*)end = saved;
        p = end;
        if (*p) p++;
    }
    return TRUE;
}

/* --------------------------------------------------------------------------
 *  T R E E V I E W   M A N A G E M E N T
 * ------------------------------------------------------------------------ */

static void ClearTreeView(void)
{
    /* Avoid redraw flicker */
    SendMessageW(g_hTree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(g_hTree);
    SendMessageW(g_hTree, WM_SETREDRAW, TRUE, 0);
}

static void PopulateTreeView(void)
{
    WCHAR buf[256];
    int totalTcp = 0, totalUdp = 0, totalListening = 0;
    int shownTcp = 0, shownUdp = 0, shownProcs = 0;
    BOOL filtering = (g_filterText[0] != L'\0');

    SendMessageW(g_hTree, WM_SETREDRAW, FALSE, 0);

    for (int pi = 0; pi < g_procCount; pi++) {
        PROC_ENTRY *proc = &g_pProcs[pi];

        /* Count matching connections for this process */
        int matchCount = 0;
        int *matches = (int*)HeapAlloc(GetProcessHeap(), 0,
                                       proc->conn_count * sizeof(int));
        if (matches) {
            for (int ci = 0; ci < proc->conn_count; ci++) {
                CONN_ENTRY *conn = &g_pConns[proc->conn_idx + ci];
                if (ConnectionMatchesFilter(conn, proc))
                    matches[matchCount++] = ci;
            }
        }

        /* Skip process if filter active and no connections match */
        if (filtering && matchCount == 0) {
            HeapFree(GetProcessHeap(), 0, matches);
            continue;
        }

        /* Determine visible child count */
        int visibleChildren = filtering ? matchCount : proc->conn_count;

        /* ---- Process (parent) node ---- */
        FormatProcItem(buf, ARRAYSIZE(buf), proc);

        TVINSERTSTRUCTW tvis = { 0 };
        tvis.hParent = TVI_ROOT;
        tvis.hInsertAfter = TVI_LAST;
        tvis.item.mask = TVIF_TEXT | TVIF_STATE | TVIF_CHILDREN | TVIF_PARAM;
        tvis.item.pszText = buf;
        tvis.item.cchTextMax = lstrlenW(buf);
        tvis.item.state = g_allExpanded ? TVIS_EXPANDED : 0;
        tvis.item.stateMask = TVIS_EXPANDED;
        tvis.item.cChildren = visibleChildren;
        tvis.item.lParam = (LPARAM)proc;

        HTREEITEM hProc = TreeView_InsertItem(g_hTree, &tvis);
        if (!hProc) { HeapFree(GetProcessHeap(), 0, matches); continue; }
        shownProcs++;

        /* ---- Connection (child) nodes ---- */
        int maxCi = filtering ? matchCount : proc->conn_count;
        for (int m = 0; m < maxCi; m++) {
            int ci = filtering ? matches[m] : m;
            CONN_ENTRY *conn = &g_pConns[proc->conn_idx + ci];

            FormatConnItem(buf, ARRAYSIZE(buf), conn);

            TVINSERTSTRUCTW tvisChild = { 0 };
            tvisChild.hParent = hProc;
            tvisChild.hInsertAfter = TVI_LAST;
            tvisChild.item.mask = TVIF_TEXT | TVIF_PARAM;
            tvisChild.item.pszText = buf;
            tvisChild.item.cchTextMax = lstrlenW(buf);
            tvisChild.item.lParam = (LPARAM)conn;

            TreeView_InsertItem(g_hTree, &tvisChild);

            if (conn->flags & 2) {
                totalUdp++;
                if (filtering) shownUdp++;
            } else {
                totalTcp++;
                if (conn->state == MIB_TCP_STATE_LISTEN)
                    totalListening++;
                if (filtering) shownTcp++;
            }
        }

        HeapFree(GetProcessHeap(), 0, matches);
    }

    SendMessageW(g_hTree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hTree, NULL, TRUE);

    /* Update status bar — show filtered counts when filter is active */
    if (filtering) {
        int shownTotal = shownTcp + shownUdp;
        int totalTotal = totalTcp + totalUdp;
        wsprintfW(buf, L"  Filter: \"%s\"  —  %d of %d connections shown (%d procs)  |  "
                  L"Refresh: %d s  |  Esc: clear filter  |  F5: Refresh",
                  g_filterText, shownTotal, totalTotal, shownProcs,
                  g_refreshMs / 1000);
    } else {
        wsprintfW(buf, L"  %d processes, %d TCP (%d listening), %d UDP  |  "
                  L"Refresh: %d s  |  F5: Refresh  |  Ctrl+F: Filter  |  Ctrl+C: Copy",
                  g_procCount, totalTcp, totalListening, totalUdp,
                  g_refreshMs / 1000);
    }
    SendMessageW(g_hStatus, SB_SETTEXTW, 0, (LPARAM)buf);
}

/* Copy selected tree item text to clipboard */
static void CopySelectedItem(void)
{
    HTREEITEM hSel = TreeView_GetSelection(g_hTree);
    if (!hSel) return;

    WCHAR buf[256] = { 0 };
    TVITEMW item = { 0 };
    item.hItem = hSel;
    item.mask = TVIF_TEXT;
    item.pszText = buf;
    item.cchTextMax = ARRAYSIZE(buf) - 1;

    if (TreeView_GetItem(g_hTree, &item)) {
        /* Open clipboard */
        if (OpenClipboard(g_hMainWnd)) {
            EmptyClipboard();
            int cch = lstrlenW(buf) + 1;
            int cb = cch * sizeof(WCHAR);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, cb);
            if (hMem) {
                CopyBytes(GlobalLock(hMem), buf, cb);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
            CloseClipboard();
        }
    }
}

/* --------------------------------------------------------------------------
 *  M E N U   &   K E Y B O A R D   H A N D L I N G
 * ------------------------------------------------------------------------ */

#define IDM_REFRESH       101
#define IDM_AUTO_REFRESH  102
#define IDM_COPY          103
#define IDM_EXIT          104
#define IDM_ABOUT         105
#define IDM_SORT_NAME     106
#define IDM_SORT_CONNS    107
#define IDM_EXPAND_ALL    108
#define IDM_COLLAPSE_ALL  109

static HMENU CreateMainMenu(void)
{
    HMENU hMenu = CreateMenu();
    HMENU hFile = CreatePopupMenu();
    HMENU hView = CreatePopupMenu();
    HMENU hHelp = CreatePopupMenu();

    AppendMenuW(hFile, MF_STRING, IDM_REFRESH, L"&Refresh\tF5");
    AppendMenuW(hFile, MF_STRING, IDM_COPY, L"&Copy\tCtrl+C");
    AppendMenuW(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFile, MF_STRING, IDM_EXIT, L"E&xit\tAlt+F4");

    AppendMenuW(hView, MF_STRING | (g_autoRefresh ? MF_CHECKED : 0),
                IDM_AUTO_REFRESH, L"&Auto-refresh\tF6");
    AppendMenuW(hView, MF_STRING | (g_sortMode == 1 ? MF_CHECKED : 0),
                IDM_SORT_CONNS, L"Sort by &Connections\tF7");
    AppendMenuW(hView, MF_STRING | (g_sortMode == 0 ? MF_CHECKED : 0),
                IDM_SORT_NAME, L"Sort by &Name\tF8");
    AppendMenuW(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hView, MF_STRING, IDM_EXPAND_ALL, L"Expand &All\tCtrl+E");
    AppendMenuW(hView, MF_STRING, IDM_COLLAPSE_ALL, L"&Collapse All\tCtrl+W");

    AppendMenuW(hHelp, MF_STRING, IDM_ABOUT, L"&About");

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hFile, L"&File");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hView, L"&View");
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hHelp, L"&Help");

    return hMenu;
}

/* --------------------------------------------------------------------------
 *  W I N D O W   C R E A T I O N
 * ------------------------------------------------------------------------ */

static void ResizeControls(HWND hWnd)
{
    RECT rc;
    GetClientRect(hWnd, &rc);
    UNREFERENCED_PARAMETER(hWnd);

    /* Let the status bar auto-size */
    SendMessageW(g_hStatus, WM_SIZE, 0, 0);

    /* Get status bar height */
    RECT rcStatus;
    GetWindowRect(g_hStatus, &rcStatus);
    int statusHeight = rcStatus.bottom - rcStatus.top;

    /* Filter bar at the top (fixed height) */
    int y = 0;
    if (g_hFilter) {
        MoveWindow(g_hFilter, 0, y, rc.right, FILTER_HEIGHT, TRUE);
        y += FILTER_HEIGHT + 2;  /* 2 px gap */
    }

    /* Tree fills remaining area */
    MoveWindow(g_hTree, 0, y, rc.right, rc.bottom - y - statusHeight, TRUE);
}

/* Subclass procedure — forwards function keys and Ctrl+letter combos
 * from child controls (filter edit, treeview) to the parent window
 * so F5, F6, F7, F8, Ctrl+C, Ctrl+F, Ctrl+E, Ctrl+W, Esc always work. */
static LRESULT CALLBACK ForwardKeysSubclass(HWND hWnd, UINT msg,
                                            WPARAM wp, LPARAM lp,
                                            UINT_PTR idSubclass, DWORD_PTR refData)
{
    UNREFERENCED_PARAMETER(idSubclass);
    UNREFERENCED_PARAMETER(refData);
    if (msg == WM_KEYDOWN) {
        if ((wp >= VK_F1 && wp <= VK_F12) ||
            wp == VK_ESCAPE ||
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            HWND hParent = GetParent(hWnd);
            SendMessageW(hParent, msg, wp, lp);
            return 0;
        }
    }
    return DefSubclassProc(hWnd, msg, wp, lp);
}

static HWND CreateFilterBar(HWND hParent)
{
    HWND hEdit = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL,
        0, 0, 0, FILTER_HEIGHT,
        hParent,
        (HMENU)IDC_FILTER,
        g_hInst,
        NULL);

    if (hEdit) {
        /* Match the tree's monospaced font for consistency */
        if (g_hFont)
            SendMessageW(hEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        /* Forward F-keys, Esc, and Ctrl+* to parent so hotkeys work */
        SetWindowSubclass(hEdit, ForwardKeysSubclass, 1, 0);

        /* Set cue banner text (requires ComCtl32 v6) */
        SendMessageW(hEdit, 0x1501, TRUE,  /* EM_SETCUEBANNER */
            (LPARAM)L"Filter: port, process, IP, state, PID, ...  (Esc to clear)");
    }

    return hEdit;
}

static HWND CreateTreeViewControl(HWND hParent)
{
    HWND hTree = CreateWindowExW(
        WS_EX_CLIENTEDGE | TVS_EX_DOUBLEBUFFER,
        WC_TREEVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
        TVS_SHOWSELALWAYS | TVS_INFOTIP,
        0, 0, 0, 0,           /* Will be sized by ResizeControls */
        hParent,
        (HMENU)100,
        g_hInst,
        NULL);

    if (hTree) {
        /* Forward F-keys, Esc, Ctrl+* to parent so hotkeys work
         * even when the TreeView has keyboard focus.                */
        SetWindowSubclass(hTree, ForwardKeysSubclass, 2, 0);

        /* Set extended style for smoother rendering */
        TreeView_SetExtendedStyle(hTree,
            TVS_EX_DOUBLEBUFFER | TVS_EX_AUTOHSCROLL,
            TVS_EX_DOUBLEBUFFER | TVS_EX_AUTOHSCROLL);

        /* Indent child items for visual clarity */
        TreeView_SetIndent(hTree, TREE_PADDING);

        /* Set a monospaced font so that the fixed-width format strings
         * (%-53s, etc.) produce properly aligned columns.              */
        g_hFont = CreateFontW(
            -12,                        /* 12 pt height                  */
            0, 0, 0,                    /* default width, escapement, orient */
            FW_NORMAL,                  /* normal weight                 */
            FALSE, FALSE, FALSE,        /* no italic, underline, strikeout */
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN,    /* monospaced                    */
            L"Consolas");
        if (g_hFont)
            SendMessageW(hTree, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    }

    return hTree;
}

static HWND CreateStatusBar(HWND hParent)
{
    HWND hStatus = CreateWindowExW(
        0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP | SBT_TOOLTIPS,
        0, 0, 0, 0,
        hParent, (HMENU)200, g_hInst, NULL);

    if (hStatus) {
        int parts[] = { -1 };  /* Single part */
        SendMessageW(hStatus, SB_SETPARTS, 1, (LPARAM)parts);
        SendMessageW(hStatus, SB_SETTEXTW, 0,
                     (LPARAM)L"  Enumerating connections...");
    }

    return hStatus;
}

/* --------------------------------------------------------------------------
 *  W I N D O W   P R O C E D U R E
 * ------------------------------------------------------------------------ */

static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE:
        {
            g_hMainWnd = hWnd;
            g_hMenu = CreateMainMenu();
            SetMenu(hWnd, g_hMenu);

            g_hFilter = CreateFilterBar(hWnd);
            g_hTree   = CreateTreeViewControl(hWnd);
            g_hStatus = CreateStatusBar(hWnd);

            /* Kick off first enumeration */
            PostMessageW(hWnd, WM_COMMAND, IDM_REFRESH, 0);

            /* Start auto-refresh timer */
            if (g_autoRefresh)
                SetTimer(hWnd, IDT_REFRESH, g_refreshMs, NULL);
        }
        return 0;

    case WM_SIZE:
        ResizeControls(hWnd);
        return 0;

    case WM_SETFOCUS:
        SetFocus(g_hTree);
        return 0;

    case WM_TIMER:
        if (wp == IDT_REFRESH) {
            PostMessageW(hWnd, WM_COMMAND, IDM_REFRESH, 0);
        }
        return 0;

    case WM_COMMAND:
        /* Filter bar text changed — update live */
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == IDC_FILTER) {
            GetWindowTextW(g_hFilter, g_filterText, ARRAYSIZE(g_filterText));
            CharLowerW(g_filterText);
            BuildProcessGroups();
            ClearTreeView();
            PopulateTreeView();
            return 0;
        }
        switch (LOWORD(wp)) {
        case IDM_REFRESH:
            {
                /* Show "refreshing..." in status bar */
                SendMessageW(g_hStatus, SB_SETTEXTW, 0,
                             (LPARAM)L"  Refreshing...");

                /* Enumerate and populate */
                BuildProcessNameMap();
                if (EnumerateAllConnections()) {
                    BuildProcessGroups();
                    ClearTreeView();
                    PopulateTreeView();
                } else {
                    SendMessageW(g_hStatus, SB_SETTEXTW, 0,
                                 (LPARAM)L"  Error: Failed to enumerate connections.");
                }
            }
            return 0;

        case IDM_AUTO_REFRESH:
            g_autoRefresh = !g_autoRefresh;
            CheckMenuItem(g_hMenu, IDM_AUTO_REFRESH,
                          g_autoRefresh ? MF_CHECKED : MF_UNCHECKED);
            if (g_autoRefresh) {
                SetTimer(hWnd, IDT_REFRESH, g_refreshMs, NULL);
            } else {
                KillTimer(hWnd, IDT_REFRESH);
            }
            return 0;

        case IDM_SORT_NAME:
            if (g_sortMode == 0)
                g_sortAscending = !g_sortAscending;  /* toggle direction */
            else
                g_sortMode = 0;                       /* switch to name */
            CheckMenuItem(g_hMenu, IDM_SORT_NAME,
                          (g_sortMode == 0) ? MF_CHECKED : MF_UNCHECKED);
            CheckMenuItem(g_hMenu, IDM_SORT_CONNS,
                          (g_sortMode == 1) ? MF_CHECKED : MF_UNCHECKED);
            BuildProcessGroups();
            ClearTreeView();
            PopulateTreeView();
            return 0;

        case IDM_SORT_CONNS:
            if (g_sortMode == 1)
                g_sortAscending = !g_sortAscending;  /* toggle direction */
            else
                g_sortMode = 1;                       /* switch to conn count */
            CheckMenuItem(g_hMenu, IDM_SORT_NAME,
                          (g_sortMode == 0) ? MF_CHECKED : MF_UNCHECKED);
            CheckMenuItem(g_hMenu, IDM_SORT_CONNS,
                          (g_sortMode == 1) ? MF_CHECKED : MF_UNCHECKED);
            BuildProcessGroups();
            ClearTreeView();
            PopulateTreeView();
            return 0;

        case IDM_EXPAND_ALL:
            g_allExpanded = TRUE;
            ClearTreeView();
            PopulateTreeView();
            return 0;

        case IDM_COLLAPSE_ALL:
            g_allExpanded = FALSE;
            ClearTreeView();
            PopulateTreeView();
            return 0;

        case IDM_COPY:
            CopySelectedItem();
            return 0;

        case IDM_EXIT:
            DestroyWindow(hWnd);
            return 0;

        case IDM_ABOUT:
            MessageBoxW(hWnd,
                        L"TCP/IP Connection Monitor v1.0\n\n"
                        L"Displays all TCP and UDP connections grouped\n"
                        L"by owning process, with expandable details.\n\n"
                        L"Filter: type in the filter bar to search by\n"
                        L"port, process name, IP, state, PID, or protocol.\n"
                        L"Multiple space-separated terms use AND logic.\n"
                        L"Ctrl+F = focus filter, Esc = clear filter.\n\n"
                        L"Built with pure Win32 API by Shahin Khorasani.\n"
                        L"Windows 10+  |  x86 / x64",
                        L"About TCP/IP Connection Monitor",
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        return 0;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_F5:
            PostMessageW(hWnd, WM_COMMAND, IDM_REFRESH, 0);
            return 0;
        case VK_F6:
            PostMessageW(hWnd, WM_COMMAND, IDM_AUTO_REFRESH, 0);
            return 0;
        case VK_F7:
            PostMessageW(hWnd, WM_COMMAND, IDM_SORT_CONNS, 0);
            return 0;
        case VK_F8:
            PostMessageW(hWnd, WM_COMMAND, IDM_SORT_NAME, 0);
            return 0;
        case VK_ESCAPE:
            /* Clear filter if active, otherwise close window */
            if (g_filterText[0] != L'\0') {
                g_filterText[0] = L'\0';
                SetWindowTextW(g_hFilter, L"");
                BuildProcessGroups();
                ClearTreeView();
                PopulateTreeView();
                SetFocus(g_hTree);
            } else {
                SendMessageW(hWnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        /* Ctrl+C */
        if (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            CopySelectedItem();
            return 0;
        }
        /* Ctrl+F — focus filter bar */
        if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SetFocus(g_hFilter);
            Edit_SetSel(g_hFilter, 0, -1);  /* select all */
            return 0;
        }
        /* Ctrl+E — expand all */
        if (wp == 'E' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            PostMessageW(hWnd, WM_COMMAND, IDM_EXPAND_ALL, 0);
            return 0;
        }
        /* Ctrl+W — collapse all */
        if (wp == 'W' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            PostMessageW(hWnd, WM_COMMAND, IDM_COLLAPSE_ALL, 0);
            return 0;
        }
        break;

    case WM_NOTIFY:
        {
            NMHDR *nmh = (NMHDR*)lp;
            if (nmh->idFrom == 100 && nmh->code == NM_DBLCLK) {
                /* Double-click on tree item to copy */
                CopySelectedItem();
                return 0;
            }
            if (nmh->idFrom == 100 && nmh->code == NM_RCLICK) {
                /* Right-click: select the item, then show context menu */
                TVHITTESTINFO ht = { 0 };
                DWORD dwPos = GetMessagePos();
                ht.pt.x = GET_X_LPARAM(dwPos);
                ht.pt.y = GET_Y_LPARAM(dwPos);
                ScreenToClient(g_hTree, &ht.pt);
                HTREEITEM hItem = TreeView_HitTest(g_hTree, &ht);
                if (hItem) {
                    TreeView_SelectItem(g_hTree, hItem);
                    /* Show context menu */
                    HMENU hCtx = CreatePopupMenu();
                    AppendMenuW(hCtx, MF_STRING, IDM_COPY, L"&Copy\tCtrl+C");
                    TrackPopupMenu(hCtx, TPM_RIGHTBUTTON | TPM_TOPALIGN,
                                   GET_X_LPARAM(dwPos), GET_Y_LPARAM(dwPos),
                                   0, hWnd, NULL);
                    DestroyMenu(hCtx);
                }
                return 0;
            }
        }
        break;

    case WM_DESTROY:
        KillTimer(hWnd, IDT_REFRESH);
        if (g_hFont) DeleteObject(g_hFont);
        PostQuitMessage(0);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wp, lp);
}

/* --------------------------------------------------------------------------
 *  W I N D O W   R E G I S T R A T I O N   &   E N T R Y   P O I N T
 * ------------------------------------------------------------------------ */

static ATOM RegisterMainWindowClass(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { 0 };

    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(100));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(100));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = APP_CLASS;

    return RegisterClassExW(&wc);
}

static HWND CreateMainWindow(HINSTANCE hInst, int nCmdShow)
{
    HWND hWnd = CreateWindowExW(
        0,
        APP_CLASS,
        APP_TITLE,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1100, 650,
        NULL, NULL, hInst, NULL);

    if (hWnd) {
        ShowWindow(hWnd, nCmdShow);
        UpdateWindow(hWnd);
    }

    return hWnd;
}

/* ---- Standard entry point ---- */
int WINAPI wWinMain(_In_ HINSTANCE hInstance,
                    _In_opt_ HINSTANCE hPrevInstance,
                    _In_ LPWSTR lpCmdLine,
                    _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    g_hInst = hInstance;

    /* Initialize common controls for TreeView */
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TREEVIEW_CLASSES };
    if (!InitCommonControlsEx(&icc))
        return 1;

    /* Register and create window */
    if (!RegisterMainWindowClass(hInstance))
        return 1;

    HWND hWnd = CreateMainWindow(hInstance, nCmdShow);
    if (!hWnd)
        return 1;

    /* Message loop */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* Clean up dynamic allocations */
    if (g_pConns) { HeapFree(GetProcessHeap(), 0, g_pConns); g_pConns = NULL; }
    if (g_pProcs) { HeapFree(GetProcessHeap(), 0, g_pProcs); g_pProcs = NULL; }
    if (g_pidNames) { HeapFree(GetProcessHeap(), 0, g_pidNames); g_pidNames = NULL; }

    return (int)msg.wParam;
}
