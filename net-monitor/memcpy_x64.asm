; Minimal memcpy for no-CRT x64 builds
    .code
memcpy PROC
    mov     rax, rcx
    cmp     r8, 0
    je      done
loop_copy:
    mov     r10b, [rdx]
    mov     [rcx], r10b
    inc     rcx
    inc     rdx
    dec     r8
    jnz     loop_copy
done:
    ret
memcpy ENDP
    END
