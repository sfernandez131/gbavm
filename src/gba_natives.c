// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Native VM functions (VM_INVOKE / VM_CALL_NATIVE targets). See gba_natives.h.

#include "gba_natives.h"

// _wait_frames(n): the "Wait" event. stack_frame[0] holds the countdown; the VM
// re-invokes this each frame (waitable yields the quant) until it hits 0. Mirrors
// GBVM's wait_frames - post-decrement, so wait completes the frame the count is 0.
UBYTE wait_frames(void * THIS, UBYTE start, UWORD * stack_frame) {
    (void)start;
    ((SCRIPT_CTX *)THIS)->waitable = TRUE;
    return (UBYTE)(stack_frame[0]-- == 0);
}

// _camera_shake_frames: gbavm has no camera yet, so complete immediately - the
// script proceeds without a visible shake (camera lands in a later milestone).
UBYTE camera_shake_frames(void * THIS, UBYTE start, UWORD * stack_frame) {
    (void)THIS;
    (void)start;
    (void)stack_frame;
    return (UBYTE)TRUE;
}
