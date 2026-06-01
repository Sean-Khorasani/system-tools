; =============================================================================
;  startup_x86.asm — Minimal x86 entry point for no-CRT builds
;
;  Build:
;    ml /c /nologo /Fo startup.obj startup_x86.asm
;  Then link with /NODEFAULTLIB /ENTRY:WinMainCRTStartup /SUBSYSTEM:WINDOWS
; =============================================================================

    .386
    .model flat, stdcall
    option casemap:none

; ---- External references --------------------------------------------------
    extern _wWinMain@16 : PROC
    extern _GetModuleHandleW@4 : PROC
    extern _GetCommandLineW@0 : PROC
    extern _ExitProcess@4 : PROC

; ---- Code segment ---------------------------------------------------------
    .code

; ---------------------------------------------------------------------------
;  x86 entry point — stdcall, no CRT.
; ---------------------------------------------------------------------------
WinMainCRTStartup PROC
    ; GetModuleHandleW(NULL) → hInstance
    push    0
    call    _GetModuleHandleW@4
    mov     ebx, eax

    ; GetCommandLineW() → lpCmdLine
    call    _GetCommandLineW@0
    mov     esi, eax

    ; wWinMain(hInstance, NULL, lpCmdLine, SW_SHOWDEFAULT=10)
    push    10                    ; nCmdShow
    push    esi                   ; lpCmdLine
    push    0                     ; hPrevInstance
    push    ebx                   ; hInstance
    call    _wWinMain@16

    ; ExitProcess(return_code)
    push    eax
    call    _ExitProcess@4

    ; Never reached
    ret
WinMainCRTStartup ENDP

    END
