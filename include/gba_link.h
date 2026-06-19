// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Whole-project link tables (Milestone M1). GBA Studio's linker (linkGbaProgram)
// compiles every project script proc to its own byte array and emits a manifest
// (gba_procs[]) describing how to fix up each one at load:
//   * local relocations  - 4-byte code targets within the same proc, stored as
//     flat {field_offset, target_offset} pairs (patched to code + target_offset).
//   * symbolic relocations - 4-byte targets that point at ANOTHER proc's array
//     (script -> script: VM_CALL_FAR / VM_BEGINTHREAD / ...). The C linker resolves
//     &target; gba_link_apply just writes that address into the field.
// Both are byte-wise patched (the field may be unaligned; ARM7TDMI can't do
// unaligned 32-bit stores). Call gba_link_apply() once at boot, before any script.

#ifndef GBAVM_GBA_LINK_H
#define GBAVM_GBA_LINK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GbaSymReloc {
    unsigned short at;             // byte offset of the 4-byte field to patch
    const unsigned char * target; // &target proc array (linker-resolved)
} GbaSymReloc;

typedef struct GbaProc {
    unsigned char * code;            // the proc's bytecode (patched in place)
    unsigned int len;
    const unsigned short * relocs;   // flat {at, target} local-reloc pairs
    unsigned int relocs_count;
    const GbaSymReloc * symrelocs;
    unsigned int symrelocs_count;
} GbaProc;

// The project manifest + entry points, both emitted by GBA Studio's build:
extern const GbaProc gba_procs[];
extern const unsigned int gba_procs_count;

// gba_scene_init: the start scene's init script (run once at boot).
// gba_actor_updates[]: each start-scene actor update script (run as a persistent
// per-frame thread). Count may be 0 (the array then holds a single null slot,
// since C forbids zero-size arrays).
extern unsigned char * const gba_scene_init;
extern unsigned char * const gba_actor_updates[];
// Runtime actor index for each update script (player = 0, placed actors = 1..),
// so the engine can activate the actor its update thread drives.
extern const unsigned char gba_actor_update_actors[];
extern const unsigned int gba_actor_updates_count;

// Patch every proc's local + symbolic relocations into its bytecode.
void gba_link_apply(void);

#ifdef __cplusplus
}
#endif

#endif // GBAVM_GBA_LINK_H
