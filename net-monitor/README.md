# TCP/IP Connection Monitor

A minimal, high-performance Windows application that displays all active
TCP/IP and UDP connections grouped by owning process in an expandable tree view.

**Windows 10+  •  x86 / x64  •  Pure Win32 API  •  Independent self-contained .exe**

![screenshot of the tree view showing processes with connections]()

## Features

- **All connection types** — TCPv4, TCPv6, UDPv4, UDPv6
- **Process grouping** — each process shown with its connections as children
- **Expandable details** — click the `+` to see every socket per process
- **Connection state** — LISTENING, ESTABLISHED, TIME_WAIT, CLOSE_WAIT, etc.
- **Auto-refresh** — configurable interval with manual F5 override
- **Name resolution** — View → Name Resolution (`F9`) reverse-resolves hovered IP to hostname via `GetNameInfoW` (`ws2_32.dll`) with 256-entry cache — tooltip `IP → hostname` appears only while pointer is on that IP
- **Clipboard copy** — Ctrl+C copies the selected tree item text
- **Status bar** — live summary: process count, TCP/UDP breakdown
- **Resizable** — window sizes naturally, Consolas monospace font
- **DPI-aware** — PerMonitorV2 DPI awareness via manifest
- **Filter bar** — live filter by port, process, IP, state, PID, protocol (AND logic, `Ctrl+F`/`Esc`)

## Quick Start

### Prerequisites

- **Windows 10** or later (x86 or x64)
- **Visual Studio 2022** (Community is free) with "Desktop development with C++"
  workload, or the standalone **Build Tools for Visual Studio 2022**

### Build from Command Prompt

Open a **Developer Command Prompt for VS 2022** (or `cmd` — the script
auto-detects VS):

```batch
cd D:\src\system-tools

:: Build Release x64 (default)
build.bat

:: Build Release x86
build.bat x86

:: Build both
build.bat all

:: Clean
build.bat clean
```

Output: `tcplist_x64.exe` (~111 KB) or `tcplist_x86.exe`.

### Build from Visual Studio IDE

Open `tcplist.sln` in Visual Studio 2022.
Press **Ctrl+Shift+B** or select **Build → Build Solution**.
Output appears in `Release\x64\` or `Release\x86\`.

### Minimal No-CRT Build (~17 KB)

For the absolute smallest .exe without any CRT dependency:

```batch
build_nocrt.bat          :: x64 (~17 KB)
build_nocrt.bat x86      :: x86
```

This uses assembly entry points (`startup_x64.asm`, `memset_x64.asm`,
`memcpy_x64.asm`) and links with `/NODEFAULTLIB`. The resulting binary is
**17 KB — 6.5× smaller** than the CRT build.

## Design

| Layer | Technology |
|-------|-----------|
| **Language** | C (not C++) for smallest code generation |
| **UI** | Pure Win32 API — TreeView, StatusBar common controls |
| **Network data** | `GetExtendedTcpTable` / `GetExtendedUdpTable` from `iphlpapi.dll` |
| **Name resolution** | `GetNameInfoW` from `ws2_32.dll` (256-entry cache, on hover when enabled via View menu) |
| **Process names** | `CreateToolhelp32Snapshot` from `kernel32.dll` |
| **Assembly** | `startup_x64.asm` / `startup_x86.asm` — no-CRT entry point (ML64/MASM) |
|   | `memset_x64.asm` / `memcpy_x64.asm` — minimal memory functions |
| **Resources** | Embedded manifest for Common Controls v6 + DPI awareness |
| **No dependencies** | No MFC, no ATL, no .NET, no frameworks. Only system DLLs. |

### Architecture diagram

```
┌──────────────────────────────────────────────┐
│ Main Window (MainWndProc)                    │
│  ┌─────────────────────────────────────────┐ │
│  │ TreeView (SysTreeView32)                │ │
│  │  ├─ firefox.exe (PID:1234) — 15 conns  │ │
│  │  │  ├─ TCP 192.168.1.100:52134 → ...   │ │
│  │  │  └─ UDP 0.0.0.0:5353 → *:*         │ │
│  │  └─ svchost.exe (PID:5678) — 6 conns   │ │
│  │     └─ ...                              │ │
│  └─────────────────────────────────────────┘ │
│  StatusBar — 42 procs, 156 TCP, 34 UDP      │
└──────────────────────────────────────────────┘

   ┌──────────────┐     ┌──────────────────┐
   │ iphlpapi.dll │────▶│ Connection array │
   │  GetExtended │     │ (CONN_ENTRY[])   │
   │  TcpTable    │     └────────┬─────────┘
   │  GetExtended │              │ sort by PID
   │  UdpTable    │     ┌────────▼─────────┐
   └──────────────┘     │ Process array    │
                         │ (PROC_ENTRY[])   │
   ┌──────────────┐     └────────┬─────────┘
   │ kernel32.dll │              │ populate
   │  CreateTool- │     ┌────────▼─────────┐
   │  help32Snap- │────▶│ PID→Name map     │
   │  shot        │     │ (PID_NAME_ENTRY) │
   └──────────────┘     └──────────────────┘
```

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| **F5** | Manual refresh |
| **F6** | Toggle auto-refresh |
| **F7** | Sort by connections (toggle asc/desc) |
| **F8** | Sort by name (toggle asc/desc) |
| **F9** | Toggle Name Resolution (IP → DNS tooltip on hover) |
| **Ctrl+C** | Copy selected item to clipboard |
| **Ctrl+F** | Focus filter bar |
| **Ctrl+E** / **Ctrl+W** | Expand all / Collapse all |
| **Esc** | Clear filter (if active) else close application |

## File Manifest

```
tcplist.c          Main C source (single file, ~650 lines)
tcplist.rc         Resource file (manifest + version info)
tcplist.manifest   Common Controls v6 + DPI manifest
startup_x64.asm    x64 assembly entry point (ML64)
startup_x86.asm    x86 assembly entry point (MASM)
memset_x64.asm     x64 memset for no-CRT builds
memcpy_x64.asm     x64 memcpy for no-CRT builds
build.bat          Build script (CRT, ~111 KB)
build_nocrt.bat    Build script (no-CRT, ~17 KB)
tcplist.sln        Visual Studio 2022 solution
tcplist.vcxproj    Visual Studio 2022 project
CMakeLists.txt     Alternative CMake build
README.md          This file
```

## Technical Notes

### Size optimization techniques

- `/O1 /Os` — compiler: minimize space
- `/GS-` — no buffer security cookies
- `/Gy` — function-level linking
- `/GL /LTCG` — whole-program optimization at link time
- `/OPT:REF /OPT:ICF` — linker: strip dead code, fold duplicate COMDATs
- `/MERGE:.rdata=.text /MERGE:.pdata=.text` — single section
- `/SUBSYSTEM:WINDOWS` — GUI app (no console)
- `/NODEFAULTLIB` (no-CRT build) — zero CRT overhead
- `CopyBytes`/`ZeroBytes` loops — avoid `memcpy`/`memset` dependency
- `MyHtons` inline — avoids extra `ws2_32.lib` calls for port conversion (still linked for `GetNameInfoW` name resolution)
- `InsertionSort` — replacement for CRT `qsort`
- `GetNameInfoW` + 256-entry DNS cache (`ws2_32.lib`) — on-demand reverse lookup only when Name Resolution is enabled and pointer is on an IP
- `wsprintfW` from `user32.dll` — no CRT `swprintf` needed
- No MFC, no ATL, no exception handling, no RTTI
- Assembly `memset`/`memcpy` — 30-byte implementations (no-CRT build only)

### Why two build variants?

| Variant | Size | CRT | Use case |
|---------|------|-----|----------|
| `build.bat` | ~111 KB | Static `/MT` | Standard build, easier to debug |
| `build_nocrt.bat` | ~17 KB | None | Minimal size, embedded/distribution |

The no-CRT build is **6.5× smaller**. It uses:
- Assembly entry point instead of CRT `WinMainCRTStartup`
- Assembly `memset`/`memcpy` (30 bytes each)
- Custom `InsertionSort` instead of CRT `qsort`
- `HeapAlloc`/`HeapFree` instead of CRT `malloc`/`free`
- `lstrlenW`/`lstrcpynW` from `kernel32` instead of CRT string functions
- `wsprintfW` from `user32.dll` instead of CRT `swprintf`
