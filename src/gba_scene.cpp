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

namespace
{
    constexpr int SCENE_STACK_MAX = 16;
    unsigned int current_scene = 0;       // the loaded scene
    unsigned int scene_stack[SCENE_STACK_MAX];
    int scene_sp = 0;
}

void gba_load_scene(unsigned int idx)
{
    if(idx >= gba_scenes_count) return;
    current_scene = idx;
    const GbaScene & s = gba_scenes[idx];

    hw_load_scene((int)idx);  // background + cleared actors for this scene
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

// Scene stack (VM_SCENE_PUSH/POP/POP_ALL). The VM calls these via extern "C";
// pop/pop_all return the scene index to load (the VM signals a scene change and
// the main loop calls gba_load_scene with the result).
extern "C" void gba_scene_push(void)
{
    if(scene_sp < SCENE_STACK_MAX) scene_stack[scene_sp++] = current_scene;
}

extern "C" unsigned short gba_scene_pop(void)
{
    return (unsigned short)(scene_sp > 0 ? scene_stack[--scene_sp] : current_scene);
}

extern "C" unsigned short gba_scene_pop_all(void)
{
    if(scene_sp > 0) { scene_sp = 0; return (unsigned short)scene_stack[0]; }
    return (unsigned short)current_scene;
}
