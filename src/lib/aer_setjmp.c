// setjmp / longjmp, written in assembly because they work on the stack and registers directly
// (cdecl: the arguments are on the stack, ebx / esi / edi / ebp must survive a call)

#include "aer_setjmp.h"

__asm__(
    ".pushsection .text\n"
    ".global setjmp\n"
    "setjmp:\n\t"
    "movl 4(%esp), %eax\n\t"      // eax = buf
    "movl %ebx, 0(%eax)\n\t"
    "movl %esi, 4(%eax)\n\t"
    "movl %edi, 8(%eax)\n\t"
    "movl %ebp, 12(%eax)\n\t"
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
    "movl 0(%edx), %ebx\n\t"
    "movl 4(%edx), %esi\n\t"
    "movl 8(%edx), %edi\n\t"
    "movl 12(%edx), %ebp\n\t"
    "movl 16(%edx), %esp\n\t"
    "jmp *20(%edx)\n"
    ".popsection\n");
