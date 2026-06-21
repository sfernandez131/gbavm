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

// A scene: its init script (run once on load) plus the per-frame actor update
// scripts and the runtime actor index each one drives (player = 0, placed actors
// = 1..). actor_updates / actor_update_actors hold a single null/0 slot when the
// count is 0 (C forbids zero-size arrays).
typedef struct GbaScene {
    unsigned char * init;
    unsigned char * const * actor_updates;
    const unsigned char * actor_update_actors;
    unsigned int actor_updates_count;
    unsigned short width_px;   // scene logical size in pixels (for camera clamping)
    unsigned short height_px;
} GbaScene;

// The project's scenes + which one to load at boot (both emitted by GBA Studio).
extern const GbaScene gba_scenes[];
extern const unsigned int gba_scenes_count;
extern const unsigned int gba_start_scene; // index into gba_scenes

// Patch every proc's local + symbolic relocations into its bytecode.
void gba_link_apply(void);

// Load scene `idx`: reset the script runner, run its init, then start each actor
// update as a persistent per-frame thread (activating that actor).
void gba_load_scene(unsigned int idx);

#ifdef __cplusplus
}
#endif

#endif // GBAVM_GBA_LINK_H
