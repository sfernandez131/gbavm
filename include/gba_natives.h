// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Native VM functions GB Studio scripts call by pointer via VM_INVOKE (a per-frame
// update handler, re-invoked until it returns TRUE) or VM_CALL_NATIVE (a one-shot
// call). The editor's linker (linkGbaProgram) resolves a script's "_name" reference
// to &name here, so these must match GBVM's symbol names (minus the leading "_").

#ifndef GBAVM_GBA_NATIVES_H
#define GBAVM_GBA_NATIVES_H

#include "vm.h"

#ifdef __cplusplus
extern "C" {
#endif

// VM_INVOKE update fns: return TRUE when complete, FALSE to be re-invoked next
// frame. They set THIS->waitable so the runner yields one call per frame.

// _wait_frames(n): block the calling script for n frames (the "Wait" event).
UBYTE wait_frames(void * THIS, UBYTE start, UWORD * stack_frame);

// _camera_shake_frames: no camera yet, so completes immediately (no shake/no wait).
UBYTE camera_shake_frames(void * THIS, UBYTE start, UWORD * stack_frame);

#ifdef __cplusplus
}
#endif

#endif // GBAVM_GBA_NATIVES_H
