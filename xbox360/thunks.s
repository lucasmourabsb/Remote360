# SPDX-License-Identifier: GPL-3.0-or-later
# Entry points called by Microsoft-ABI kernel/XAM code. OpenXeChain uses the ppc32 ABI,
# so preserve the full 64-bit nonvolatile GPRs around each C body.

    .macro NV64_THUNK name, target
    .text
    .globl \name
    .p2align 2
\name:
    mflr 0
    stwu 1, -176(1)
    stw 0, 168(1)
    .irp r, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    std \r, (16 + (\r - 14) * 8)(1)
    .endr
    bl \target
    .irp r, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ld \r, (16 + (\r - 14) * 8)(1)
    .endr
    lwz 0, 168(1)
    mtlr 0
    addi 1, 1, 176
    blr
    .endm

    NV64_THUNK DllMain, DllMainC
    NV64_THUNK R360NetThread, R360NetThreadC
#ifdef R360_PARALLEL_BC1
    NV64_THUNK R360EncodeThread, R360EncodeThreadC
#endif
#ifndef R360_VIDEO_ONLY
    NV64_THUNK XaReadStateHook, XaReadStateHookC
    NV64_THUNK XaCapsHook, XaCapsHookC
    NV64_THUNK XaSetStateHook, XaSetStateHookC
    NV64_THUNK XaXamThread, XaXamThreadC

    .text
    .globl xa_trampolines
    .p2align 4
xa_trampolines:
    .space 8 * 30 * 4
#endif

