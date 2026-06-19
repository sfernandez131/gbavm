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

    script_runner_init(TRUE);
    script_execute(0, gba_scene_init, nullptr, 0); // start scene init: runs once
    for(unsigned int i = 0; i < gba_actor_updates_count; ++i)
    {
        // Activate the actor this update thread drives, then run the thread (it
        // self-loops via VM_IDLE/VM_JUMP, so one script_execute persists it).
        hw_actor_activate(gba_actor_update_actors[i]);
        script_execute(0, gba_actor_updates[i], nullptr, 0); // per-frame actor thread
    }

    while(true)
    {
        script_runner_update(); // run the editor-emitted bytecode
        hw_render();            // push actor state into sprites
        sys_time++;
        bn::core::update();
    }
}
