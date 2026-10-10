// main.cpp
// SPDX-License-Identifier: Apache-2.0
// Application entry point: wmain, common-controls init, main window, message loop.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cwchar>

#include "Cli.h"
#include "Commands.h"
#include "CrashDump.h"
#include "Elevate.h"
#include "MainWindow.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")

// The two startup-failure MessageBox bodies, which differ only in the API name
// and the error number that follow the fixed wording. One constant because the
// two sites were the same bare 256, and swprintf_s truncates rather than
// overruns so a longer message cannot corrupt anything.
constexpr size_t kStartupMsgChars = 256;

// wmain, not wWinMain: build.bat links /SUBSYSTEM:CONSOLE so an interactive
// cmd.exe waits for this process and prints its next prompt only after the
// command has really finished. A WINDOWS-subsystem exe hands the prompt back
// immediately (batch scripts DO wait, which is why the gates never saw it)
// and its output then lands on top of the prompt instead of after it. The
// GUI path below gives that console back before any window is created.
// hInstance/nCmdShow are what wWinMain would have received: the module
// handle, and the show hint from STARTUPINFO (SW_SHOWDEFAULT when the
// launcher leaves it unspecified).
int wmain(int /*argc*/, wchar_t** /*argv*/) {
    HINSTANCE hInstance = ::GetModuleHandleW(nullptr);
    const int nCmdShow = SW_SHOWDEFAULT;
    // DPI awareness comes from the embedded manifest: PerMonitorV2 on
    // Win10 1703+, falling back to dpiAware=true on older systems
    //. No runtime opt-in call is needed.

    // Command-line mode: any real argument -> run the unified CLI
    // (wintcp.exe <command> [switches]) and exit; launched bare - or with
    // only the elevation marker from a UAC relaunch - run the GUI as before.
    //
    // Decided FIRST, ahead of both early-init calls below, because the GUI
    // path's console handover has to happen while Windows is still showing the
    // console. Those calls used to sit above this decision, which is what left
    // the console on screen measurably longer than necessary:
    // InstallCrashHandler resolves the dump directory, and that delay-loads
    // shell32.dll and calls SHGetFolderPathW before returning.
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    const bool cli = (argv != nullptr) && wintcp::IsCliRequested(argc, argv);

    // GUI path, launched with no real arguments (a bare double-click, or the
    // lone elevation marker from a UAC relaunch). Windows already allocated a
    // console for this console-subsystem binary AND ALREADY PAINTED IT: that
    // happens during process init, before wmain is entered, so nothing here can
    // stop it from appearing. All this block can do is stop it hanging around,
    // which is why it runs before every other startup call. Measured on a bare
    // launch the console stayed visible 9.6-17.4 ms, and the two early-init
    // calls used to account for the part of that which followed.
    //
    // If launched from a shell (sharing that console), detach only - leave the
    // shell's window untouched - and remove this process from the console's
    // Ctrl+C list, so Ctrl+C at the prompt cannot reach the window.
    if (!cli) {
        DWORD attached[8] = {0};
        const bool soleOwner = (::GetConsoleProcessList(attached, 8) == 1);
        if (soleOwner) {
            HWND console = ::GetConsoleWindow();
            if (console != nullptr) ::ShowWindow(console, SW_HIDE);
        }
        ::FreeConsole();
    }

    // First, before anything that can crash (which is everything): without
    // this a field failure is "it just closed" with nothing to diagnose.
    // Deliberately AFTER the handover above - that ordering is the whole point,
    // because this is the one startup call that touches the disk and the
    // registry, so running it while the console is still up is precisely the
    // delay being removed. Everything that can realistically fault still sits
    // below it: WSAStartup, InitCommonControlsEx, RegisterClassEx, the window.
    wintcp::InstallCrashHandler();
    // Grant SeDebugPrivilege if the token carries it. Without it, opening
    // handles to system-level processes (lsass, svchost, services) fails with
    // ERROR_ACCESS_DENIED even for an Administrator, because the privilege
    // defaults to Present-but-disabled. Harmless for a standard user.
    wintcp::EnableDebugPrivilege();
    // CLI text is UTF-8 — set the code pages when we actually run CLI output.

    if (cli) {
        // A one-shot CLI verb must not outlive its output. `list --traffic`
        // starts a DETACHED socket-scan thread, and on a machine holding a
        // socket whose SIO_TCP_INFO never returns that thread is still in
        // the kernel at exit, still holding a duplicated socket handle.
        // That makes the teardown below unsafe in BOTH directions, so the
        // order matters:
        //  * WSACleanup() runs no thread-local winsock init, so it cannot
        //    itself be what wedges. But it refuses to complete while a
        //    duplicated socket is open in the process, so calling it
        //    before the scan has settled HANGS THE COMMAND - it printed its
        //    rows and then never returned (measured: 8/8 runs, the one
        //    defect this path is here to fix).
        //  * So settle the scan first, then clean up, and only skip the
        //    cleanup if something is still stuck inside the kernel.
        //
        // Winsock is required for InetNtopW(); the CLI path is the only
        // one that needs it before the window, so start it here.
        WSADATA wsaData = {};
        if (::WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            ::MessageBoxW(nullptr, L"WSAStartup failed; cannot format IP addresses.",
                          L"WinTCP", MB_OK | MB_ICONERROR);
            return 1;
        }

        // The CLI writes UTF-8 (WideToUtf8) — em-dash, arrows, ellipsis,
        // middle dot. Without this the console interprets those bytes as
        // Windows-1252 and shows mojibake (e.g. "ΓÇö" for "—"). Set both
        // input and output to UTF-8.
        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleCP(CP_UTF8);

        const int cliRc = wintcp::RunCli(argc, argv);
        const bool scanStuck = wintcp::CliScanThreadMayBeLive();
        if (!scanStuck) ::WSACleanup();
        ::LocalFree(argv);
        if (scanStuck) {
            // Nothing is left to flush: RunCli's output was already
            // written, and the detached thread is stuck in the kernel and
            // cannot be joined.
            ::ExitProcess(static_cast<UINT>(cliRc));
        }
        return cliRc;
    }

    // Winsock required for InetNtopW(); the GUI path is the only one that
    // reaches here, so start it before the window.
    WSADATA wsaData = {};
    if (::WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        ::MessageBoxW(nullptr, L"WSAStartup failed; cannot format IP addresses.",
                      L"WinTCP", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Register the ListView / status bar / other common-control classes and
    // enable visual styles (ComCtl32 v6, via the embedded manifest).
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&icc);

    if (!wintcp::MainWindow::RegisterClass(hInstance)) {
        DWORD err = ::GetLastError();
        wchar_t msg[kStartupMsgChars] = {0};
        ::swprintf_s(msg, L"RegisterClassEx failed (error %lu).", static_cast<unsigned long>(err));
        ::MessageBoxW(nullptr, msg, L"WinTCP", MB_OK | MB_ICONERROR);
        ::WSACleanup();
        return 1;
    }

    wintcp::MainWindow window;
    HWND hwnd = window.Create(hInstance, nCmdShow);
    if (hwnd == nullptr) {
        DWORD err = ::GetLastError();
        wchar_t msg[kStartupMsgChars] = {0};
        ::swprintf_s(msg, L"CreateWindowEx failed (error %lu).", static_cast<unsigned long>(err));
        ::MessageBoxW(nullptr, msg, L"WinTCP", MB_OK | MB_ICONERROR);
        ::WSACleanup();
        return 1;
    }

    MSG msg = {};
    HACCEL hAccel = ::LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDR_ACCEL));
    int ret = 0;

    while ((ret = ::GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) break;  // GetMessage error; exit the loop.
        // Keyboard shortcuts (F5 = refresh, F1 = about). The accelerator
        // table is optional: if loading failed, hAccel is null and this is a no-op.
        if (hAccel != nullptr && ::TranslateAcceleratorW(hwnd, hAccel, &msg)) continue;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    if (argv != nullptr) ::LocalFree(argv);
    ::WSACleanup();
    return static_cast<int>(msg.wParam);
}
