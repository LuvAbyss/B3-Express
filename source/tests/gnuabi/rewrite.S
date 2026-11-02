/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* Code rewritten after it ran must run as rewritten: a JIT's own code buffer. Exits 42. */
.text
.global _start
.type _start, %function
_start:
    mov x0, #0
    mov x1, #4096
    mov x2, #7              /* PROT_READ | PROT_WRITE | PROT_EXEC */
    mov x3, #0x22           /* MAP_PRIVATE | MAP_ANONYMOUS */
    mov x4, #-1
    mov x5, #0
    mov x8, #222            /* mmap */
    svc #0
    mov x19, x0
    ldr w1, =0xd2800020     /* mov x0, #1 */
    ldr w2, =0xd65f03c0     /* ret */
    str w1, [x19]
    str w2, [x19, #4]
    blr x19
    cmp x0, #1
    b.ne fail
    ldr w1, =0xd2800540     /* mov x0, #42 */
    str w1, [x19]
    dc cvau, x19
    dsb ish
    ic ivau, x19
    dsb ish
    isb
    blr x19
    mov x8, #93             /* exit */
    svc #0
fail:
    mov x0, #1
    mov x8, #93
    svc #0
.size _start, .-_start
.section .note.GNU-stack,"",%progbits
