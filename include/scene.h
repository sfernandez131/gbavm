// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Scene runtime (P2): loads/switches scenes around the single P1 linked bytecode
// image. C-callable so vm.c (C) and main.cpp/hw.cpp (C++) can all call in.

#ifndef GBAVM_SCENE_H
#define GBAVM_SCENE_H

#ifdef __cplusplus
extern "C" {
#endif

// Load the start scene (called once at boot).
void scene_boot(void);
// Tear down the current scene and load scene `index` (bg + actors + scripts).
void scene_load(int index);
// The currently-loaded scene index (-1 before boot).
int scene_current(void);

// Scene-stack opcodes (VM_SCENE_PUSH/POP/POP_ALL/STACK_RESET, 0x68-0x6B).
void scene_stack_push(void);
void scene_stack_pop(void);
void scene_stack_pop_all(void);
void scene_stack_reset(void);

#ifdef __cplusplus
}
#endif

#endif // GBAVM_SCENE_H
