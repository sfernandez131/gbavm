// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Scene runtime (P2). Loads/switches scenes around the single P1 linked image:
// hw.cpp owns the Butano bg/sprite objects (hw_load_scene); this file owns the
// scene stack and starting each scene's init + actor-update script threads from
// their byte-offsets in game_image[]. The scene table (gba_scenes[]) is generated
// by GBA Studio's ejectGbaBuild into scene_table.h/.c.

#include "scene.h"
#include "scene_table.h"
#include "hw.h"

extern "C" {
#include "vm.h"
}

// The P1 whole-project linked bytecode image (generated game_image.c).
extern "C" unsigned char game_image[];

namespace
{
    int current_scene = -1;
    unsigned char scene_stack[8];
    unsigned char scene_sp = 0;
}

extern "C" void scene_load(int index)
{
    if(index < 0 || (unsigned int)index >= gba_scene_count) return;
    current_scene = index;
    const GbaScene& sc = gba_scenes[index];

    // hw.cpp: release the previous scene's sprites/bg, then load this scene's
    // background + actor sprite bindings (and activate sprited actors).
    hw_load_scene(index);

    // Start this scene's script threads from their byte-offsets in the linked
    // image: the init script (runs once), then each actor's persistent per-frame
    // update thread. (GB Studio's scene-init bytecode does not spawn the actor
    // update threads itself, so the manager starts them — generalizing the single
    // hardcoded actor-update boot the engine used before P2.)
    if(sc.init_off != GBA_NO_OFFSET)
    {
        script_execute(0, game_image + sc.init_off, nullptr, 0);
    }
    for(int i = 0; i < sc.n_actors; ++i)
    {
        if(sc.actors[i].update_off != GBA_NO_OFFSET)
        {
            script_execute(0, game_image + sc.actors[i].update_off, nullptr, 0);
        }
    }
}

extern "C" void scene_boot(void)
{
    scene_load((int)gba_start_scene);
}

extern "C" int scene_current(void) { return current_scene; }

// The stack stores scene INDICES only (no player pos/dir snapshot — gbavm has no
// PLAYER actor yet; that restore lands with the movement system in a later phase).
extern "C" void scene_stack_push(void)
{
    if(scene_sp < 8) scene_stack[scene_sp++] = (unsigned char)current_scene;
}

extern "C" void scene_stack_pop(void)
{
    if(scene_sp > 0)
    {
        --scene_sp;
        // Route through the same change-scene exception the main loop handles.
        vm_request_change_scene(scene_stack[scene_sp]);
    }
}

extern "C" void scene_stack_pop_all(void)
{
    if(scene_sp > 0)
    {
        unsigned char base = scene_stack[0];
        scene_sp = 0;
        vm_request_change_scene(base);
    }
}

extern "C" void scene_stack_reset(void) { scene_sp = 0; }
