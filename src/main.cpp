// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// M1: runs a whole-project link image. GBA Studio compiles every project script
// proc to a byte array and emits a manifest (gba_procs[], src/gba_program.c) plus
// entry points (src/gba_entries.c). gba_link_apply() patches every proc's local
// and symbolic (cross-proc) relocations in place; we then run the start scene's
// init once and each actor update script as a persistent per-frame thread.

#include "bn_core.h"
#include "bn_timer.h"
#include "hw.h"
#include "gba_link.h"

#include <cstdint>

extern "C" {
#include "vm.h"
}

int main()
{
    bn::core::init();
    bn::timer rng_seed_timer; // free-running; sampled after boot work for a varying seed
    hw_init();

    // Resolve every script proc's code targets (local + cross-proc) before running.
    gba_link_apply();

    // Seed the VM PRNG (GBA has no DIV register) from accumulated boot ticks.
    vm_boot_seed((UWORD)(rng_seed_timer.elapsed_ticks() & 0xFFFF));

    gba_load_scene(gba_start_scene); // start scene: init + actor update threads

    while(true)
    {
        // Was a dialogue open before this frame's scripts ran? If so, the A that closes it
        // (consumed by script_runner_update) must not also start an NPC interaction (M6c).
        const int dialogue_was_open = hw_dialogue_active();
        // A script raises an exception for scene changes (EXCEPTION_CHANGE_SCENE -> load
        // the target scene) or SRAM save/load (M6a: EXCEPTION_SAVE/LOAD persist + restore
        // the variables; the script then continues).
        if(script_runner_update() == RUNNER_EXCEPTION)
        {
            switch(vm_get_exception())
            {
                case EXCEPTION_CHANGE_SCENE: gba_load_scene(vm_get_exception_param()); break;
                case EXCEPTION_SAVE: gba_save_game(vm_get_exception_param()); break;   // param = slot (M6g)
                case EXCEPTION_LOAD: gba_load_game(vm_get_exception_param()); break;
                default: break;
            }
        }
        hw_player_update();     // d-pad -> player (actor 0) movement, when enabled
        gba_check_triggers();   // fire a trigger zone's script when the player enters it (M6b)
        if(!dialogue_was_open) gba_check_interact(); // A + facing a placed actor -> interact (M6c)
        timers_update();        // fire any timer scripts whose countdown elapsed (M6f)
        hw_overlay_update();    // animate the dialogue overlay window box (slide in/out)
        hw_render();            // push actor state into sprites
        sys_time++;
        bn::core::update();
    }
}
