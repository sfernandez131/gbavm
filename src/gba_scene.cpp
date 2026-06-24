// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Scene loading (Milestone M2). GBA Studio emits a table of scenes (gba_scenes[],
// gba_scenes.c); gba_load_scene resets the script runner and starts a scene's init
// + actor update threads. Switching/stack/camera/fades build on this.

#include "gba_link.h"
#include "hw.h"
#include "bn_sram.h" // battery-backed save RAM (M6a)

extern "C" {
#include "vm.h"
}

namespace
{
    constexpr int SCENE_STACK_MAX = 16;
    unsigned int current_scene = 0;       // the loaded scene
    unsigned int scene_stack[SCENE_STACK_MAX];
    int scene_sp = 0;

    // SRAM save (M6a). One slot at offset 0: a magic marker (valid-save check), the
    // saved scene, and the global variables (script_memory's heap region). The thread
    // stacks above VM_HEAP_SIZE are not saved (no mid-script VM-context resume yet).
    constexpr unsigned int SAVE_MAGIC = 0x47424153u; // "GBAS"
    struct GbaSave
    {
        unsigned int magic;
        unsigned short scene;
        unsigned short vars[VM_HEAP_SIZE];
    };
}

void gba_load_scene(unsigned int idx)
{
    if(idx >= gba_scenes_count) return;
    current_scene = idx;
    const GbaScene & s = gba_scenes[idx];

    hw_load_scene((int)idx, s.width_px, s.height_px);  // background + cleared actors for this scene
    hw_set_player_move(s.player_move);                 // built-in d-pad control for this scene type
    hw_set_collisions(s.collisions, s.width_px / 8, s.height_px / 8); // tile collision grid
    script_runner_init(TRUE); // reset all contexts for the new scene

    // Place every actor at its authored position + facing before scripts run, so
    // actors appear where the editor put them even without a Set Position script.
    for(unsigned int i = 0; i < s.actors_init_count; ++i)
    {
        const GbaActorInit & ai = s.actors_init[i];
        hw_actor_place(ai.index, ai.x, ai.y, ai.dir);
    }

    script_execute(0, s.init, nullptr, 0); // scene init: runs once (may reposition)

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

// --- SRAM save/load (M6a). EXCEPTION_SAVE/LOAD (via VM_RAISE) call the first two from
// the main loop; VM_SAVE_PEEK/CLEAR call the rest from the VM. Single slot for now (the
// GB Studio slot operand is ignored). ---

extern "C" void gba_save_game(void)
{
    GbaSave s;
    s.magic = SAVE_MAGIC;
    s.scene = (unsigned short)current_scene;
    for(int i = 0; i < VM_HEAP_SIZE; ++i) s.vars[i] = script_memory[i];
    bn::sram::write(s);
}

extern "C" int gba_load_game(void)
{
    GbaSave s;
    bn::sram::read(s);
    if(s.magic != SAVE_MAGIC) return 0;
    // Restore the global variables in place; the script resumes after the Load event.
    // The saved scene is NOT reloaded yet (that re-runs the scene init and can loop if
    // the init itself loads) - full scene/VM-context resume is a follow-up.
    for(int i = 0; i < VM_HEAP_SIZE; ++i) script_memory[i] = s.vars[i];
    return 1;
}

// VM_SAVE_PEEK existence check (slot ignored): true if a valid save exists. The COUNT>0
// read-saved-vars form is a follow-up; the common "If Data Saved" use passes COUNT 0.
extern "C" int gba_save_peek(int slot)
{
    (void)slot;
    unsigned int magic = 0;
    bn::sram::read(magic); // magic is the first field of the slot at offset 0
    return magic == SAVE_MAGIC ? 1 : 0;
}

extern "C" void gba_save_clear(int slot)
{
    (void)slot;
    unsigned int magic = 0; // overwrite the magic to invalidate the save
    bn::sram::write(magic);
}
