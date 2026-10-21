// SPDX-License-Identifier: GPL-3.0-or-later
// Original homebrew fixture; no Nintendo binaries or firmware required.
.syntax unified
.arm
.global _start
_start:
    ldr r0, =0x04000136
    ldr r1, =0x02001000
1:
    ldrh r2, [r0]
    and r2, r2, #0x40
    eor r2, r2, #0x40
    str r2, [r1]
    b 1b
