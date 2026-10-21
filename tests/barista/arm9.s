// SPDX-License-Identifier: GPL-3.0-or-later
// Original homebrew fixture; no Nintendo binaries or firmware required.
.syntax unified
.arm
.global _start
_start:
    ldr r0, =0x04000304
    ldr r1, =0x820f
    strh r1, [r0]
    ldr r0, =0x04000240
    mov r1, #0x80
    strb r1, [r0]
    ldr r0, =0x04000000
    mov r1, #0x20000
    str r1, [r0]
    ldr r0, =0x04001000
    mov r1, #0x10000
    str r1, [r0]
    ldr r0, =0x06800000
    mov r1, #31
    ldr r2, =24576
1:
    strh r1, [r0], #2
    subs r2, r2, #1
    bne 1b
    mov r1, #0x7c00
    ldr r2, =24576
2:
    strh r1, [r0], #2
    subs r2, r2, #1
    bne 2b
    ldr r0, =0x04000130
    ldr r2, =0x05000400
    ldr r3, =0x02001000
3:
    ldrh r4, [r0]
    tst r4, #1
    ldr r1, =0x03e0
    moveq r1, #0x7c00
    ldr r4, [r3]
    cmp r4, #0
    ldrne r1, =0x03ff
    strh r1, [r2]
    b 3b
