# SPDX-License-Identifier: GPL-3.0-or-later
# Microsoft ABI entry to freestanding OpenXeChain C, preserving full GPRs.
    .text
    .globl stage_entry
    .p2align 2
stage_entry:
    mflr 0
    stwu 1, -176(1)
    stw 0, 168(1)
    .irp r, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    std \r, (16 + (\r - 14) * 8)(1)
    .endr
    bl loader_main
    .irp r, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ld \r, (16 + (\r - 14) * 8)(1)
    .endr
    lwz 0, 168(1)
    mtlr 0
    addi 1, 1, 176
    blr
