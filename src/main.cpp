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
        script_runner_update(); // run the editor-emitted bytecode
        hw_render();            // push actor state into sprites
        sys_time++;
        bn::core::update();
    }
}
