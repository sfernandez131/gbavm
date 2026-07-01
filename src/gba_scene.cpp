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
    int current_trigger = -1;             // trigger zone the player is in (-1 none), M6b

    // SRAM save (M6a). One slot at offset 0: a magic marker (valid-save check), the
    // saved scene, and the global variables (script_memory's heap region). The thread
    // stacks above VM_HEAP_SIZE are not saved (no mid-script VM-context resume yet).
    constexpr unsigned int SAVE_MAGIC = 0x47424153u; // "GBAS"
    struct GbaSave
    {
        unsigned int magic;
        unsigned short scene;        // scene to resume into (M6e)
        unsigned short player_x;     // player position + facing to resume at (M6e)
        unsigned short player_y;
        unsigned char player_dir;
        unsigned short vars[VM_HEAP_SIZE];
    };
}

void gba_load_scene(unsigned int idx)
{
    if(idx >= gba_scenes_count) return;

    // M6d: a Switch Scene event positions + faces the player (actor 0) for the destination
    // just before raising CHANGE_SCENE; keep that entry point across the load instead of
    // snapping back to the scene's authored start. On the initial boot load actor 0 isn't
    // active yet, so the authored start (the project start position) is used.
    unsigned short keep[3] = { 0, 0, 0 }; // [0]=id(in), [1]=x, [2]=y
    const int keep_player = hw_actor_active(0);
    unsigned char keep_dir = 0;
    if(keep_player) { hw_actor_get_pos(keep); keep_dir = hw_actor_dir(0); }

    current_scene = idx;
    current_trigger = -1; // re-arm trigger zones for the new scene (M6b)
    const GbaScene & s = gba_scenes[idx];

    hw_load_scene((int)idx, s.width_px, s.height_px);  // background + cleared actors for this scene
    hw_set_player_move(s.player_move);                 // built-in d-pad control for this scene type
    hw_set_collisions(s.collisions, s.width_px / 8, s.height_px / 8); // tile collision grid
    script_runner_init(TRUE); // reset all contexts for the new scene

    // Place every actor at its authored position + facing before scripts run, so
    // actors appear where the editor put them even without a Set Position script.
    // The player (actor 0) keeps the Switch Scene entry point when arriving from another scene.
    for(unsigned int i = 0; i < s.actors_init_count; ++i)
    {
        const GbaActorInit & ai = s.actors_init[i];
        if(ai.index == 0 && keep_player) hw_actor_place(0, keep[1], keep[2], keep_dir);
        else hw_actor_place(ai.index, ai.x, ai.y, ai.dir);
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

// Trigger zones (M6b): each frame, fire the script of the trigger the player just
// entered. current_trigger debounces it (re-runs only after leaving + re-entering);
// the script runs as a new thread, like the scene init (it may switch scene, save, etc.).
extern "C" void gba_check_triggers(void)
{
    const GbaScene & s = gba_scenes[current_scene];
    int in = -1;
    for(unsigned int i = 0; i < s.triggers_count; ++i)
    {
        const GbaTrigger & t = s.triggers[i];
        if(hw_player_in_rect(t.x, t.y, t.w, t.h)) { in = (int)i; break; }
    }
    if(in != current_trigger)
    {
        current_trigger = in;
        if(in >= 0) script_execute(0, s.triggers[in].script, nullptr, 0);
    }
}

// NPC interaction (M6c): each frame, if the player pressed A while facing an adjacent
// placed actor (no dialogue up), run that actor's interact script as a new thread. hw
// returns the runtime actor index; map it back to its GbaActorInit to find the script.
extern "C" void gba_check_interact(void)
{
    const int actor = hw_interact_actor();
    if(actor < 0) return;
    const GbaScene & s = gba_scenes[current_scene];
    for(unsigned int i = 0; i < s.actors_init_count; ++i)
    {
        const GbaActorInit & ai = s.actors_init[i];
        if(ai.index == (unsigned char)actor && ai.interact)
        {
            script_execute(0, ai.interact, nullptr, 0);
            return;
        }
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
// the main loop; VM_SAVE_PEEK/CLEAR call the rest from the VM. Multiple slots (M6g): each
// save slot occupies its own GbaSave-sized region of SRAM at slot * sizeof(GbaSave); the
// slot comes from the Save/Load event (the VM_RAISE payload, or the peek/clear operand). ---

// M6g: byte offset of a save slot's region in SRAM.
static int gba_save_offset(int slot) { return slot * (int)sizeof(GbaSave); }

extern "C" void gba_save_game(int slot)
{
    GbaSave s;
    s.magic = SAVE_MAGIC;
    s.scene = (unsigned short)current_scene;
    unsigned short pos[3] = { 0, 0, 0 }; // [0]=id in; hw writes [1]=x, [2]=y
    hw_actor_get_pos(pos);
    s.player_x = pos[1];
    s.player_y = pos[2];
    s.player_dir = hw_actor_dir(0);
    for(int i = 0; i < VM_HEAP_SIZE; ++i) s.vars[i] = script_memory[i];
    bn::sram::write_offset(s, gba_save_offset(slot));
}

extern "C" int gba_load_game(int slot)
{
    GbaSave s;
    bn::sram::read_offset(s, gba_save_offset(slot));
    if(s.magic != SAVE_MAGIC) return 0;
    // Resume into the saved scene (M6e). gba_load_scene runs script_runner_init(TRUE), which
    // wipes script_memory, and queues the scene init as a thread that runs NEXT frame - so:
    //   1. load the scene, then 2. restore the globals over the wiped memory, then 3. re-place
    //   the player at the saved spot (overriding the scene's authored start).
    // Caveat: a scene whose init writes these globals would clobber them when it runs next
    // frame; scenes shouldn't set persistent variables in their init on a resume.
    // Load Data with no save is a no-op (magic check above), so scene inits can call it
    // unconditionally to auto-resume on boot.
    gba_load_scene(s.scene);
    for(int i = 0; i < VM_HEAP_SIZE; ++i) script_memory[i] = s.vars[i];
    hw_actor_place(0, s.player_x, s.player_y, s.player_dir);
    return 1;
}

// VM_SAVE_PEEK existence check: true if a valid save exists in `slot`. The COUNT>0
// read-saved-vars form is a follow-up; the common "If Data Saved" use passes COUNT 0.
extern "C" int gba_save_peek(int slot)
{
    unsigned int magic = 0;
    bn::sram::read_offset(magic, gba_save_offset(slot)); // magic is the slot's first field
    return magic == SAVE_MAGIC ? 1 : 0;
}

extern "C" void gba_save_clear(int slot)
{
    unsigned int magic = 0; // overwrite the slot's magic to invalidate its save
    bn::sram::write_offset(magic, gba_save_offset(slot));
}
