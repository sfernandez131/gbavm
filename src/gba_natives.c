// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Native VM functions (VM_INVOKE / VM_CALL_NATIVE targets). See gba_natives.h.

#include "gba_natives.h"
#include "hw.h"

// _wait_frames(n): the "Wait" event. The VM re-invokes this each frame (waitable yields
// the quant) until the countdown hits 0. Mirrors GBVM's wait_frames: keep the countdown
// in a scratch slot just past the data stack (no interrupts -> not spoiled), resetting it
// on `start`, and leave the arg (stack_frame[0]) UNTOUCHED. GB Studio skips re-setting the
// count for equal consecutive waits (e.g. dialogue !W: chunks), reusing the arg, so
// destroying it (the old post-decrement on stack_frame[0]) made the 2nd wait underflow to
// ~65535 frames and appear to hang. M4q surfaced this.
UBYTE wait_frames(void * THIS, UBYTE start, UWORD * stack_frame) {
    SCRIPT_CTX * ctx = (SCRIPT_CTX *)THIS;
    if (start) *ctx->stack_ptr = stack_frame[0] + 1; // +1: pre-decrement below
    if ((--*ctx->stack_ptr) != 0) { ctx->waitable = TRUE; return (UBYTE)FALSE; }
    return (UBYTE)TRUE;
}

// _camera_shake_frames(n): the "Camera Shake" event (M6h). Like _wait_frames it blocks the
// script for n frames; on the first invoke it also kicks off the visible shake (hw_render
// jitters the camera with a decaying amplitude for those n frames).
UBYTE camera_shake_frames(void * THIS, UBYTE start, UWORD * stack_frame) {
    SCRIPT_CTX * ctx = (SCRIPT_CTX *)THIS;
    if (start) { *ctx->stack_ptr = stack_frame[0] + 1; hw_camera_shake(stack_frame[0]); }
    if ((--*ctx->stack_ptr) != 0) { ctx->waitable = TRUE; return (UBYTE)FALSE; }
    return (UBYTE)TRUE;
}
