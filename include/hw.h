// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Hardware-ops bridge: the C-callable surface that the VM (vm.c, compiled as C)
// uses to drive the GBA via Butano (hw.cpp, compiled as C++). Handlers take only
// plain scalars / resolved pointers so no Butano types cross the C/C++ boundary.

#ifndef GBAVM_HW_H
#define GBAVM_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called once from main() after bn::core::init(), before the script runs.
void hw_init(void);
// Set up a scene's hardware state (background + cleared actors) before its scripts
// run. Called by gba_load_scene; scene_idx indexes the generated per-scene assets.
// width_px/height_px are the scene's logical size, used to clamp the camera.
void hw_load_scene(int scene_idx, int width_px, int height_px);
// Called once per frame from the main loop, AFTER script_runner_update() and
// BEFORE bn::core::update(). The ONLY place actor state is pushed into sprites.
void hw_render(void);

// Enable/disable built-in top-down player movement for the loaded scene (set by
// gba_load_scene from the scene's player_move flag).
void hw_set_player_move(uint8_t enabled);
// Set the loaded scene's collision grid (one byte/tile, row-major). Movement is
// blocked into any tile whose low nibble is non-zero, and outside the grid bounds.
void hw_set_collisions(const unsigned char* grid, int width_tiles, int height_tiles);
// Move the player (actor 0) from the live d-pad each frame when player movement is
// enabled. Called from the main loop before hw_render.
void hw_player_update(void);
// Animate the dialogue overlay window box one frame (slide in/out). Called from the
// main loop each frame, like hw_player_update; independent of the VM so the box can
// finish sliding out after the script that hid it has ended.
void hw_overlay_update(void);

// --- hardware opcode handlers (invoked from VM_STEP cases) ---
void hw_set_sprites_visible(uint8_t mode);        // 0x51
void hw_actor_activate(int16_t actor);            // 0x31
void hw_actor_deactivate(int16_t actor);          // 0x33
// Place an actor at its initial position + facing on scene load (activates it,
// no movement inference). Called from gba_load_scene before scripts run.
void hw_actor_place(int16_t id, uint16_t x, uint16_t y, uint8_t dir);

// --- actor movement (VM_ACTOR_MOVE_TO_* / SET_DIR) ---
// GB Studio's "Move To" compiles to MOVE_TO_INIT (set destination) then per-axis
// blocking MOVE_TO_X / MOVE_TO_Y (or MOVE_TO_XY for diagonal), with SET_DIR_X/Y
// in between to face the travel direction.
void hw_actor_move_init(int16_t id, uint16_t dest_x, uint16_t dest_y);
int  hw_actor_move_step(int16_t id, uint8_t axis); // axis 0=X 1=Y 2=both; 1 when arrived
void hw_actor_move_set_dir(int16_t id, uint8_t axis); // face toward dest along axis 0=X 1=Y
void hw_actor_move_cancel(int16_t id);              // stop moving (clear destination)
void hw_actor_set_dir(int16_t id, uint8_t dir);     // 0x34 explicit facing
void hw_actor_set_moving(int16_t id);               // mark moving this frame (walk anim)
void hw_actor_set_pos(uint16_t* pos);             // 0x35  pos -> {int16 ID, uint16 X, uint16 Y}
void hw_actor_get_pos(uint16_t* pos);             // 0x3A  writes X,Y back
void hw_actor_get_angle(uint16_t* params, int16_t* dest); // 0x86  dir -> BRADS angle
void hw_input_get(uint16_t* dst, uint8_t joyid);  // 0x54  GB-style button bitmask
// 0x57 VM_FADE: advance the screen fade one frame (flags bit 0x02 = fade in, else
// out). Returns 1 once the fade is complete; the VM blocks the thread until then.
int hw_fade_step(uint8_t flags);
// 0x90 VM_DISPLAY_TEXT: show `text` (the dialogue string, rendered via Butano's text
// generator) and wait for A. Returns 1 once dismissed. `text` follows the opcode
// inline (null-terminated) in the bytecode. `values`/`n_values` are the variable
// values substituted for the text's %d placeholders (M4i); pass NULL/0 for plain text.
// `avatar` is the dialogue avatar index to draw beside the text, or 0xFF for none (M4m).
// `preserve` (M4q): non-zero = VM_DISPLAY_TEXT_EX append (continue the existing box for
// !W: wait chunks); 0 = a fresh display. Returns 1 once the text is fully revealed.
int hw_text_step(const char* text, const int16_t* values, int n_values, int avatar, int preserve);
// VM_OVERLAY_WAIT (M4q): blocks until the requested UI conditions (window/text/button);
// owns the dialogue A-wait. Returns 1 when satisfied, 0 to keep waiting.
int hw_overlay_wait(int condition);
// --- dialogue overlay window box (M4d) ---
// A Butano panel drawn behind the dialogue text. The box spans the screen width and
// sits at the bottom; its height is derived from the GBVM overlay Y (rows from the
// top of an 18-row GB screen, so 18 = fully hidden, 14 = a 4-row box at the bottom).
// These set the box target only (non-blocking); hw_overlay_update animates the slide.
void hw_overlay_move_to(int x, int y, int speed); // 0x91  speed: -1 in, -2 out, -3 instant
void hw_overlay_show(int x, int y, int color);    // 0x92  show the box at row y (instant)
void hw_overlay_hide(void);                        // 0x93  hide the box (instant)

// --- DMG music (M5a) + sound effects (M5b) ---
void hw_music_play(int track, int loop);           // 0x60  play DMG track (loop != 0 = loop)
void hw_music_stop(void);                          // 0x61  stop DMG music
void hw_sfx_play(int sfx);                          // 0x66  play a .wav sound effect
void hw_sound_mastervol(int vol);                  // 0x63  set the master volume (0..8)

#ifdef __cplusplus
}
#endif

#endif // GBAVM_HW_H
