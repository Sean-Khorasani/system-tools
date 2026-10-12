; Minimal memset for no-CRT x64 builds
    .code
memset PROC
    mov     rax, rcx
    cmp     r8, 0
    je      done
loop_fill:
    mov     [rcx], dl
    inc     rcx
    dec     r8
    jnz     loop_fill
done:
    ret
memset ENDP
    END
