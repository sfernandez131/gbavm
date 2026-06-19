// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Scene loading (Milestone M2). GBA Studio emits a table of scenes (gba_scenes[],
// gba_scenes.c); gba_load_scene resets the script runner and starts a scene's init
// + actor update threads. Switching/stack/camera/fades build on this.

#include "gba_link.h"
#include "hw.h"

extern "C" {
#include "vm.h"
}

void gba_load_scene(unsigned int idx)
{
    if(idx >= gba_scenes_count) return;
    const GbaScene & s = gba_scenes[idx];

    script_runner_init(TRUE); // reset all contexts for the new scene
    script_execute(0, s.init, nullptr, 0); // scene init: runs once

    for(unsigned int i = 0; i < s.actor_updates_count; ++i)
    {
        // Activate the actor this update thread drives, then run the thread (it
        // self-loops via VM_IDLE/VM_JUMP, so one script_execute persists it).
        hw_actor_activate(s.actor_update_actors[i]);
        script_execute(0, s.actor_updates[i], nullptr, 0);
    }
}
