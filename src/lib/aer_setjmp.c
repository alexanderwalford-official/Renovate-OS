// setjmp / longjmp, written in assembly because they work on the stack and registers directly
// (cdecl: the arguments are on the stack, ebx / esi / edi / ebp must survive a call)

#include "aer_setjmp.h"

// Both functions live in one top-level asm block rather than in C, because C code would set up
// its own stack frame and change the very registers they need to save. The jmp_buf is six
// 4 byte slots:
//
//     0 ebx   4 esi   8 edi   12 ebp   16 stack pointer   20 return address
//
// eax, ecx and edx don't need saving: cdecl lets any function change them.
__asm__(
    ".pushsection .text\n"
    ".global setjmp\n"
    "setjmp:\n\t"
    // on entry (%esp) is the return address and 4(%esp) is the first argument
    "movl 4(%esp), %eax\n\t"      // eax = buf
    // save the four registers the caller expects to survive
    "movl %ebx, 0(%eax)\n\t"
    "movl %esi, 4(%eax)\n\t"
    "movl %edi, 8(%eax)\n\t"
    "movl %ebp, 12(%eax)\n\t"
    // and where to come back to: the stack pointer as it will be after our ret has popped the
    // return address, and the return address itself
    "leal 4(%esp), %ecx\n\t"      // the caller's stack pointer once we have returned
    "movl %ecx, 16(%eax)\n\t"
    "movl (%esp), %ecx\n\t"       // our return address
    "movl %ecx, 20(%eax)\n\t"
    "xorl %eax, %eax\n\t"         // the first time round, setjmp returns 0
    "ret\n"

    ".global longjmp\n"
    "longjmp:\n\t"
    "movl 4(%esp), %edx\n\t"      // edx = buf
    "movl 8(%esp), %eax\n\t"      // eax = value, which setjmp will now return
    "testl %eax, %eax\n\t"
    "jnz 1f\n\t"
    "incl %eax\n"                 // longjmp(buf, 0) still has to look like a second return
    "1:\n\t"
    // put back the saved registers, including the stack pointer, which throws away every
    // stack frame made since setjmp was called
    "movl 0(%edx), %ebx\n\t"
    "movl 4(%edx), %esi\n\t"
    "movl 8(%edx), %edi\n\t"
    "movl 12(%edx), %ebp\n\t"
    "movl 16(%edx), %esp\n\t"
    // then jump (not ret: the return address is no longer on the stack) to just after the
    // setjmp call, with eax as setjmp's return value
    "jmp *20(%edx)\n"
    ".popsection\n");
