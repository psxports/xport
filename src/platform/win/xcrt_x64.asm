; Windows x64 ABI helpers for XCRT
option casemap:none
.code

; Store the caller context using the MSVC _JUMP_BUFFER layout
_setjmp proc
    mov [rcx], rdx
    mov [rcx+8], rbx
    lea rax, [rsp+8]
    mov [rcx+16], rax
    mov [rcx+24], rbp
    mov [rcx+32], rsi
    mov [rcx+40], rdi
    mov [rcx+48], r12
    mov [rcx+56], r13
    mov [rcx+64], r14
    mov [rcx+72], r15
    mov rax, [rsp]
    mov [rcx+80], rax
    stmxcsr [rcx+88]
    fnstcw [rcx+92]
    mov word ptr [rcx+94], 0
    movdqu [rcx+96], xmm6
    movdqu [rcx+112], xmm7
    movdqu [rcx+128], xmm8
    movdqu [rcx+144], xmm9
    movdqu [rcx+160], xmm10
    movdqu [rcx+176], xmm11
    movdqu [rcx+192], xmm12
    movdqu [rcx+208], xmm13
    movdqu [rcx+224], xmm14
    movdqu [rcx+240], xmm15
    xor eax, eax
    ret
_setjmp endp

; Restore registers directly without C++ or SEH unwinding
longjmp proc
    mov eax, edx
    test eax, eax
    jnz restore
    inc eax
restore:
    mov rbx, [rcx+8]
    mov rbp, [rcx+24]
    mov rsi, [rcx+32]
    mov rdi, [rcx+40]
    mov r12, [rcx+48]
    mov r13, [rcx+56]
    mov r14, [rcx+64]
    mov r15, [rcx+72]
    ldmxcsr [rcx+88]
    fldcw [rcx+92]
    movdqu xmm6, [rcx+96]
    movdqu xmm7, [rcx+112]
    movdqu xmm8, [rcx+128]
    movdqu xmm9, [rcx+144]
    movdqu xmm10, [rcx+160]
    movdqu xmm11, [rcx+176]
    movdqu xmm12, [rcx+192]
    movdqu xmm13, [rcx+208]
    movdqu xmm14, [rcx+224]
    movdqu xmm15, [rcx+240]
    mov rsp, [rcx+16]
    jmp qword ptr [rcx+80]
longjmp endp

; Probe every crossed stack page while preserving RAX and argument registers
__chkstk proc
    mov r10, rax
    lea r11, [rsp+8]
probe_page:
    cmp r10, 1000h
    jb probe_tail
    sub r11, 1000h
    test byte ptr [r11], 0
    sub r10, 1000h
    jmp probe_page
probe_tail:
    sub r11, r10
    test byte ptr [r11], 0
    ret
__chkstk endp

; Math inputs and results use XMM registers and a balanced x87 stack
sin proc frame
    sub rsp, 8
    .allocstack 8
    .endprolog
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fsin
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
sin endp

sinf proc frame
    sub rsp, 8
    .allocstack 8
    .endprolog
    movss dword ptr [rsp], xmm0
    fld dword ptr [rsp]
    fsin
    fstp dword ptr [rsp]
    movss xmm0, dword ptr [rsp]
    add rsp, 8
    ret
sinf endp

cos proc frame
    sub rsp, 8
    .allocstack 8
    .endprolog
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fcos
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
cos endp

cosf proc frame
    sub rsp, 8
    .allocstack 8
    .endprolog
    movss dword ptr [rsp], xmm0
    fld dword ptr [rsp]
    fcos
    fstp dword ptr [rsp]
    movss xmm0, dword ptr [rsp]
    add rsp, 8
    ret
cosf endp

sqrt proc frame
    sub rsp, 8
    .allocstack 8
    .endprolog
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fsqrt
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
sqrt endp

sqrtf proc frame
    sub rsp, 8
    .allocstack 8
    .endprolog
    movss dword ptr [rsp], xmm0
    fld dword ptr [rsp]
    fsqrt
    fstp dword ptr [rsp]
    movss xmm0, dword ptr [rsp]
    add rsp, 8
    ret
sqrtf endp

atan2 proc frame
    sub rsp, 16
    .allocstack 16
    .endprolog
    movsd qword ptr [rsp], xmm0
    movsd qword ptr [rsp+8], xmm1
    fld qword ptr [rsp]
    fld qword ptr [rsp+8]
    fpatan
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 16
    ret
atan2 endp

atan2f proc frame
    sub rsp, 16
    .allocstack 16
    .endprolog
    movss dword ptr [rsp], xmm0
    movss dword ptr [rsp+8], xmm1
    fld dword ptr [rsp]
    fld dword ptr [rsp+8]
    fpatan
    fstp dword ptr [rsp]
    movss xmm0, dword ptr [rsp]
    add rsp, 16
    ret
atan2f endp

end
