; =============================================================================
;  startup_x64.asm — Minimal x64 entry point for no-CRT builds
;
;  Build:
;    ml64 /c /nologo /Fo startup.obj startup_x64.asm
;  Then link with /NODEFAULTLIB /ENTRY:WinMainCRTStartup /SUBSYSTEM:WINDOWS
; =============================================================================

    option casemap:none

; ---- External references --------------------------------------------------
    extern wWinMain : PROC
    extern GetModuleHandleW : PROC
    extern GetCommandLineW : PROC
    extern ExitProcess : PROC

; ---- Code segment ---------------------------------------------------------
    .code

; ---------------------------------------------------------------------------
;  x64 entry point — the PE loader calls here directly.
;  We ignore whatever the loader passes and fetch values ourselves.
; ---------------------------------------------------------------------------
WinMainCRTStartup PROC
    sub     rsp, 28h              ; shadow space (32) — keep 16-byte aligned

    ; hInstance = GetModuleHandleW(NULL)
    xor     ecx, ecx
    call    GetModuleHandleW
    mov     rbx, rax              ; save hInstance

    ; lpCmdLine = GetCommandLineW()
    call    GetCommandLineW
    mov     rsi, rax              ; save lpCmdLine

    ; Call wWinMain(hInstance, NULL, lpCmdLine, SW_SHOWDEFAULT=10)
    mov     rcx, rbx              ; hInstance
    xor     edx, edx              ; hPrevInstance = NULL
    mov     r8,  rsi              ; lpCmdLine
    mov     r9d, 10               ; nCmdShow = SW_SHOWDEFAULT
    call    wWinMain

    ; ExitProcess(return_code)
    mov     ecx, eax
    call    ExitProcess

    ; Not reached
    add     rsp, 28h
    ret
WinMainCRTStartup ENDP

    END
