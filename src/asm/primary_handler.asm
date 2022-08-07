; primary_handler.asm
; Alexander Walford 2022

.a.str:
    ascii "ASM X86_64 NASM\n.. call Primary Handler proc."

.code
PrimaryHandler proc
    mov rax, a
    ret
PrimaryHandler endp
end