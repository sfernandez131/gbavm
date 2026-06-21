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

// --- hardware opcode handlers (invoked from VM_STEP cases) ---
void hw_set_sprites_visible(uint8_t mode);        // 0x51
void hw_actor_activate(int16_t actor);            // 0x31
void hw_actor_deactivate(int16_t actor);          // 0x33
void hw_actor_set_pos(uint16_t* pos);             // 0x35  pos -> {int16 ID, uint16 X, uint16 Y}
void hw_actor_get_pos(uint16_t* pos);             // 0x3A  writes X,Y back
void hw_actor_get_angle(uint16_t* params, int16_t* dest); // 0x86  dir -> BRADS angle
void hw_input_get(uint16_t* dst, uint8_t joyid);  // 0x54  GB-style button bitmask
// 0x57 VM_FADE: advance the screen fade one frame (flags bit 0x02 = fade in, else
// out). Returns 1 once the fade is complete; the VM blocks the thread until then.
int hw_fade_step(uint8_t flags);

#ifdef __cplusplus
}
#endif

#endif // GBAVM_HW_H
