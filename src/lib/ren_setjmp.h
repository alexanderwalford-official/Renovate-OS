/*
    ren_setjmp.h
    Non-local jumps (freestanding replacement for <setjmp.h>).

    setjmp(buf) saves where it was called from and returns 0. A later longjmp(buf, value), from
    any depth of nested function calls, unwinds straight back so that setjmp returns again,
    this time with value. RenLang uses it to abandon a program the moment it hits an error.
    The function that called setjmp must not have returned in between.
*/
#ifndef REN_SETJMP_H
#define REN_SETJMP_H

// the callee-saved registers (ebx, esi, edi, ebp), the stack pointer and the return address
typedef unsigned int jmp_buf[6];

int setjmp(jmp_buf buf) __attribute__((returns_twice));
__attribute__((noreturn)) void longjmp(jmp_buf buf, int value);

#endif /* REN_SETJMP_H */
