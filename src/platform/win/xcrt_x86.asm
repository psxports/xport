; Windows x86 ABI helpers for XCRT
.686p
.xmm
.model flat
option casemap:none
option prologue:none
option epilogue:none

.const
float_unsigned_offset REAL4 2147483648.0

.code

public __CIatan2
__CIatan2 proc
    fpatan
    ret
__CIatan2 endp

; Convert unsigned EDX:ECX to binary32 in XMM0
public __ultof3
__ultof3 proc
    sub esp, 8
    test edx, edx
    js upper_half
    mov [esp], ecx
    mov [esp+4], edx
    fild QWORD PTR[esp]
    fstp DWORD PTR[esp]
    movss xmm0, DWORD PTR[esp]
    add esp, 8
    ret
upper_half:
    ; Retain the discarded low bit for correct halfway rounding
    mov eax, ecx
    and eax, 1
    shrd ecx, edx, 1
    shr edx, 1
    or ecx, eax
    mov [esp], ecx
    mov [esp+4], edx
    fild QWORD PTR[esp]
    fstp DWORD PTR[esp]
    movss xmm0, DWORD PTR[esp]
    addss xmm0, xmm0
    add esp, 8
    ret
__ultof3 endp

public __ftol3
__ftol3 proc
    cvttss2si eax, xmm0
    test eax, eax
    jns done
    ucomiss xmm0, DWORD PTR float_unsigned_offset
    jb done
    sub esp, 16
    movups XMMWORD PTR[esp], xmm0
    subss xmm0, DWORD PTR float_unsigned_offset
    cvttss2si eax, xmm0
    xor eax, 80000000h
    movups xmm0, XMMWORD PTR[esp]
    add esp, 16
done:
    ret
__ftol3 endp

public __libm_sse2_sqrt_precise
__libm_sse2_sqrt_precise proc
    sqrtsd xmm0, xmm0
    ret
__libm_sse2_sqrt_precise endp

public __libm_sse2_sin_precise
__libm_sse2_sin_precise proc
    sub esp, 8
    movsd QWORD PTR[esp], xmm0
    fld QWORD PTR[esp]
    fsin
    fstp QWORD PTR[esp]
    movsd xmm0, QWORD PTR[esp]
    add esp, 8
    ret
__libm_sse2_sin_precise endp

public __libm_sse2_cos_precise
__libm_sse2_cos_precise proc
    sub esp, 8
    movsd QWORD PTR[esp], xmm0
    fld QWORD PTR[esp]
    fcos
    fstp QWORD PTR[esp]
    movsd xmm0, QWORD PTR[esp]
    add esp, 8
    ret
__libm_sse2_cos_precise endp
divide_words proc
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    sub esp, 8
    xor ebx, ebx
    mov [ebp-16], ebx
    mov [ebp-20], ebx
    mov eax, [ebp+8]
    mov edx, [ebp+12]
    mov esi, [ebp+16]
    mov edi, [ebp+20]
    cmp dword ptr [ebp+28], 0
    je magnitudes_ready
    test edx, edx
    jns numerator_positive
    neg edx
    neg eax
    sbb edx, 0
    mov ebx, 1
numerator_positive:
    mov [ebp-16], ebx
    test edi, edi
    jns denominator_positive
    neg edi
    neg esi
    sbb edi, 0
    xor ebx, 1
denominator_positive:
    mov [ebp-20], ebx
magnitudes_ready:
    test edi, edi
    jnz wide_divisor
    mov ebx, eax
    mov eax, edx
    xor edx, edx
    div esi
    mov edi, eax
    mov eax, ebx
    div esi
    cmp dword ptr [ebp+24], 0
    jne narrow_remainder
    mov edx, edi
    jmp apply_sign
narrow_remainder:
    mov eax, edx
    xor edx, edx
    jmp apply_sign
wide_divisor:
    mov ecx, edx
    xor edx, edx
    mov ebx, 32
divide_bit:
    shl eax, 1
    rcl ecx, 1
    rcl edx, 1
    jc subtract_divisor
    cmp edx, edi
    jb next_bit
    ja subtract_divisor
    cmp ecx, esi
    jb next_bit
subtract_divisor:
    sub ecx, esi
    sbb edx, edi
    inc eax
next_bit:
    dec ebx
    jnz divide_bit
    cmp dword ptr [ebp+24], 0
    jne wide_remainder
    xor edx, edx
    jmp apply_sign
wide_remainder:
    mov eax, ecx
apply_sign:
    mov ecx, [ebp-20]
    cmp dword ptr [ebp+24], 0
    je sign_ready
    mov ecx, [ebp-16]
sign_ready:
    test ecx, ecx
    jz divide_done
    neg edx
    neg eax
    sbb edx, 0
divide_done:
    add esp, 8
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
divide_words endp

public __allmul
__allmul proc
    push ebx
    mov eax, [esp+8]
    mov ecx, [esp+16]
    mul ecx
    mov ebx, edx
    mov edx, [esp+12]
    imul edx, ecx
    add ebx, edx
    mov ecx, [esp+20]
    mov edx, [esp+8]
    imul ecx, edx
    mov edx, ebx
    add edx, ecx
    pop ebx
    ret 16
__allmul endp

; Quotient and remainder helpers use the compiler's callee-cleanup ABI
DIVIDE_HELPER macro name, want_rem, signed_value
public name
name proc
    push ebp
    mov ebp, esp
    push signed_value
    push want_rem
    push [ebp+20]
    push [ebp+16]
    push [ebp+12]
    push [ebp+8]
    call divide_words
    add esp, 24
    pop ebp
    ret 16
name endp
endm
DIVIDE_HELPER __alldiv, 0, 1
DIVIDE_HELPER __aulldiv, 0, 0
DIVIDE_HELPER __allrem, 1, 1
DIVIDE_HELPER __aullrem, 1, 0

; Combined helpers return quotient in EDX:EAX and remainder in EBX:ECX
DIVREM_HELPER macro name, signed_value
public name
name proc
    push ebp
    mov ebp, esp
    push esi
    push edi
    push signed_value
    push 0
    push [ebp+20]
    push [ebp+16]
    push [ebp+12]
    push [ebp+8]
    call divide_words
    add esp, 24
    mov esi, eax
    mov edi, edx
    push signed_value
    push 1
    push [ebp+20]
    push [ebp+16]
    push [ebp+12]
    push [ebp+8]
    call divide_words
    add esp, 24
    mov ebx, edx
    mov ecx, eax
    mov edx, edi
    mov eax, esi
    pop edi
    pop esi
    pop ebp
    ret 16
name endp
endm
DIVREM_HELPER __alldvrm, 1
DIVREM_HELPER __aulldvrm, 0

public __allshl
__allshl proc
    cmp cl, 64
    jae empty
    cmp cl, 32
    jae shift_high
    shld edx, eax, cl
    shl eax, cl
    ret
shift_high:
    mov edx, eax
    xor eax, eax
    and cl, 31
    shl edx, cl
    ret
empty:
    xor eax, eax
    xor edx, edx
    ret
__allshl endp

public __aullshr
__aullshr proc
    cmp cl, 64
    jae empty
    cmp cl, 32
    jae shift_low
    shrd eax, edx, cl
    shr edx, cl
    ret
shift_low:
    mov eax, edx
    xor edx, edx
    and cl, 31
    shr eax, cl
    ret
empty:
    xor eax, eax
    xor edx, edx
    ret
__aullshr endp

public __allshr
__allshr proc
    cmp cl, 64
    jae sign_only
    cmp cl, 32
    jae shift_low
    shrd eax, edx, cl
    sar edx, cl
    ret
shift_low:
    mov eax, edx
    sar edx, 31
    and cl, 31
    sar eax, cl
    ret
sign_only:
    sar edx, 31
    mov eax, edx
    ret
__allshr endp

public __setjmp3
__setjmp3 proc
    mov edx, [esp+4]
    mov [edx], ebp
    mov [edx+4], ebx
    mov [edx+8], edi
    mov [edx+12], esi
    lea eax, [esp+4]
    mov [edx+16], eax
    mov eax, [esp]
    mov [edx+20], eax
    xor eax, eax
    ret
__setjmp3 endp

public _longjmp
_longjmp proc
    mov edx, [esp+4]
    mov eax, [esp+8]
    test eax, eax
    jnz restore
    inc eax
restore:
    mov ebp, [edx]
    mov ebx, [edx+4]
    mov edi, [edx+8]
    mov esi, [edx+12]
    mov esp, [edx+16]
    jmp dword ptr [edx+20]
_longjmp endp

end
