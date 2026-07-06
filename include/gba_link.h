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
// One placed actor's initial state, applied on scene load before any script runs.
typedef struct GbaActorInit {
    unsigned char index;   // runtime actor index (player = 0, placed actors = 1..)
    unsigned char dir;     // facing: 0 down, 1 right, 2 up, 3 left
    unsigned short x;      // initial position in subpixels (256 per 8px tile)
    unsigned short y;
    unsigned char * interact; // M6c: script run when the player faces this actor + presses A (0 if none)
    unsigned char move_speed; // M10a: authored speed in subpixels/frame (32 = 1px/frame); 0 = engine default
    unsigned char collision_group; // M10f: GB collision group bit (player 0x01, "1" 0x02, "2" 0x04, "3" 0x08)
} GbaActorInit;

// One projectile definition (M10f) - GB Studio's projectile_def_t mapped to the GBA
// asset model: `sprite` indexes the generated gba_projectile_sprite() table and
// `anim_state` selects the sprite's animation-state row (the projectile's 4
// direction anims are that row's idle set), replacing GB's sprite far-ptr +
// baked animation ranges. A scene's defs load into the engine's 5 runtime slots
// on scene load (GB data_manager parity); VM_PROJECTILE_LOAD_TYPE loads one from
// gba_global_projectile_defs[] instead.
typedef struct GbaProjectileDef {
    unsigned char sprite;          // index into the generated projectile sprite table
    unsigned char anim_state;      // animation-state row (statesOrder index)
    unsigned char move_speed;      // subpixels/frame (32 = 1px/frame)
    unsigned short life_time;      // frames until auto-despawn
    unsigned char collision_group; // group bit this projectile belongs to
    unsigned char collision_mask;  // actor groups it can hit
    unsigned char strong;          // 1 = survives hits (GB !destroyOnHit)
    unsigned char anim_tick;       // frame-advance mask: advance when (tick & mask) == 0
    unsigned char anim_noloop;     // 1 = clamp on the last frame instead of looping
    unsigned short initial_offset; // spawn offset along the launch angle, in subpixels
} GbaProjectileDef;

// A trigger zone (M6b): when the player enters its tile rect, its script runs once.
typedef struct GbaTrigger {
    unsigned char x, y;        // top-left of the zone, in tiles
    unsigned char w, h;        // size in tiles
    unsigned char * script;    // run on enter (a collected scene proc)
} GbaTrigger;

typedef struct GbaScene {
    unsigned char * init;
    unsigned char * const * actor_updates;
    const unsigned char * actor_update_actors;
    unsigned int actor_updates_count;
    unsigned short width_px;   // scene logical size in pixels (for camera clamping)
    unsigned short height_px;
    const GbaActorInit * actors_init; // placed actors' initial position + facing
    unsigned int actors_init_count;
    unsigned char player_move; // 1 = built-in top-down d-pad movement for actor 0
    const unsigned char * collisions; // one byte per tile (row-major, width_px/8 wide); 0 = open
    const GbaTrigger * triggers; // trigger zones (M6b)
    unsigned int triggers_count;
    const GbaProjectileDef * projectiles; // M10f: defs preloaded into the runtime slots on load
    unsigned int projectiles_count;
    // M10g: the scene's combined player-hit script (branches on the projectile's
    // collision group via GET_TLOCAL 0), run when a projectile hits actor 0. 0 = none.
    unsigned char * player_hit;
} GbaScene;

// Global projectile-def tables (M10f): every VM_PROJECTILE_LOAD_TYPE source table,
// flattened into one array. The bridge resolves each `_global_projectiles_<n>`
// symbol to its table's base index here; the op's source def = defs[base + src].
extern const GbaProjectileDef gba_global_projectile_defs[];
extern const unsigned int gba_global_projectile_defs_count;

// The project's scenes + which one to load at boot (both emitted by GBA Studio).
extern const GbaScene gba_scenes[];
extern const unsigned int gba_scenes_count;
extern const unsigned int gba_start_scene; // index into gba_scenes

// Patch every proc's local + symbolic relocations into its bytecode.
void gba_link_apply(void);

// Load scene `idx`: reset the script runner, run its init, then start each actor
// update as a persistent per-frame thread (activating that actor).
void gba_load_scene(unsigned int idx);

// Fire the trigger zone the player just entered (M6b); called each frame.
void gba_check_triggers(void);

// Run the interact script of the placed actor the player faces + presses A on (M6c);
// called each frame.
void gba_check_interact(void);

// SRAM save/load (M6a; multi-slot M6g). Save/load the global variables + scene + player to
// battery SRAM slot `slot` (each slot is its own GbaSave-sized region). The main loop calls
// these on EXCEPTION_SAVE / EXCEPTION_LOAD, passing the slot from the raise. gba_load_game
// returns 1 if a valid save was loaded, else 0.
void gba_save_game(int slot);
int gba_load_game(int slot);

#ifdef __cplusplus
}
#endif

#endif // GBAVM_GBA_LINK_H
