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
    hw_set_scene_type(s.scene_type);                   // controller dispatch (M13a)
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
        hw_actor_set_bounds(ai.index, ai.bounds);   // gbs2 G2: sprite bounding box
        // Authored movement speed (M10a); 0 keeps the engine default.
        if(ai.move_speed) hw_actor_set_move_speed(ai.index, ai.move_speed);
        // Authored collision group (M10f; the player row carries 0x01).
        hw_actor_set_collision_group(ai.index, ai.collision_group);
        // On Hit (M10g): a projectile hit fires the actor's combined
        // interact/hit script (its param branches pick the On Hit tab); the
        // player (actor 0) uses the scene's player-hit script instead.
        hw_actor_set_hit_script(ai.index, ai.index == 0 ? s.player_hit : ai.interact);
    }
    hw_topdown_init(); // gbvm's state_init, once the player stands where it arrives (gbs2 G5)

    // Preload the scene's projectile defs into the runtime slots (M10f) - GB
    // data_manager parity, so Launch Projectile works with no load op.
    for(unsigned int i = 0; i < s.projectiles_count; ++i)
    {
        hw_projectile_load_def((uint8_t)i, &s.projectiles[i]);
    }

    script_execute(0, s.init, nullptr, 0); // scene init: runs once (may reposition)

    for(unsigned int i = 0; i < s.actor_updates_count; ++i)
    {
        // Activate the actor this update thread drives, then run the thread (it
        // self-loops via VM_IDLE/VM_JUMP, so one script_execute persists it).
        const int16_t ai = (int16_t)s.actor_update_actors[i];
        hw_actor_activate(ai);
        // Slice D: register the script and hand script_execute the actor's handle slot,
        // so VM_ACTOR_BEGIN_UPDATE / TERMINATE_UPDATE can restart or kill this thread.
        hw_actor_set_update_script(ai, s.actor_updates[i]);
        script_execute(0, s.actor_updates[i], hw_actor_update_handle(ai), 0);
    }
}

// gbvm's trigger_at_intersection: the tiles under the player's bounding box
// (SUBPX_TO_TILE is `>> 8` to a byte) against each trigger's inclusive tile rect; the
// first hit wins. -1 for none (or no active player).
static int trigger_at_intersection(const GbaScene & s)
{
    uint16_t l, r, t, b;
    int collide;
    if(!hw_actor_abs_bounds(0, &l, &r, &t, &b, &collide)) return -1;
    const uint8_t tl = uint8_t(l >> 8), tr = uint8_t(r >> 8), tt = uint8_t(t >> 8), tb = uint8_t(b >> 8);
    for(unsigned int i = 0; i < s.triggers_count; ++i)
    {
        const GbaTrigger & g = s.triggers[i];
        if(tl <= g.x + g.w - 1 && tr >= g.x && tt <= g.y + g.h - 1 && tb >= g.y) return (int)i;
    }
    return -1;
}

// Trigger zones (M6b): each frame, fire the script of the trigger the player just
// entered. current_trigger debounces it (re-runs only after leaving + re-entering);
// the script runs as a new thread, like the scene init (it may switch scene, save, etc.).
// gbs2 G2 POINTNCLICK: gbvm's pointnclick_update, the part after the cursor moves
// (hw_player_update did that). Triggers do not fire by walking into them; instead the
// cursor's box picks out the first trigger its tiles overlap (trigger_at_intersection)
// and the first collidable actor it overlaps (actor_overlapping_player), shows the hover
// cursor over either one that has a script, and A runs it - the actor's with argument 0,
// else the trigger's with argument 1 (trigger_interact). Like the rest of the scene's
// state update, the main loop skips it while the VM is locked.
static void gba_pointnclick_update(const GbaScene & s)
{
    const int hit_trigger = trigger_at_intersection(s);

    // actor_overlapping_player: the first collidable actor whose box overlaps the cursor's.
    const int hit = hw_actor_overlapping_player();
    const GbaActorInit * hit_actor = nullptr;
    for(unsigned int i = 0; hit > 0 && i < s.actors_init_count && !hit_actor; ++i)
    {
        if(s.actors_init[i].index == (unsigned char)hit) hit_actor = &s.actors_init[i];
    }

    const bool hover_trigger = hit_trigger >= 0 && s.triggers[hit_trigger].script;
    const bool hover_actor = hit_actor && hit_actor->interact;
    hw_set_cursor_hover(hover_trigger || hover_actor);

    if(hw_a_pressed())
    {
        if(hover_actor) script_execute(0, hit_actor->interact, nullptr, 1, 0);
        else if(hover_trigger) script_execute(0, s.triggers[hit_trigger].script, nullptr, 1, 1);
    }
}

extern "C" void gba_check_triggers(void)
{
    // gbs2 G3: BLANK tests no triggers, and leaves the debounce as it was (gbvm
    // simply skips trigger_activate_at_intersection in that state).
    if(hw_player_blank()) return;
    const GbaScene & s = gba_scenes[current_scene];
    if(s.scene_type == 4) { gba_pointnclick_update(s); return; }   // gbs2 G2 POINTNCLICK
    if(!hw_player_on_grid()) return; // gbs2 G5: a TOPDOWN player tests triggers on landing
    // gbs2 G5: any tile under the player's box counts, as in gbvm - the tile at its
    // origin alone missed gbs2's exit from Path to Sample Town, which the player reaches
    // against a wall. A platformer re-fires the trigger it stands in when Up goes down
    // (INPUT_PLATFORM_FORCE_TRIGGER): gbs2's door to Player's House checks for Up.
    const int in = trigger_at_intersection(s);
    const bool force = s.scene_type == GBA_SCENE_PLATFORM && hw_up_pressed();
    if(in != current_trigger || (force && in >= 0))
    {
        current_trigger = in;
        // Parameter 1 = the on-enter section (M10g): a trigger with BOTH enter and
        // leave scripts compiles them into one script branching on thread arg 0
        // (1 = enter, 2 = leave; leave isn't fired on GBA yet). A trigger with only
        // an enter script is collapsed by the compiler and ignores the arg.
        if(in >= 0) script_execute(0, s.triggers[in].script, nullptr, 1, 1);
    }
}

// NPC interaction (M6c): each frame, if the player pressed A while facing an adjacent
// placed actor (no dialogue up), run that actor's interact script as a new thread. hw
// returns the runtime actor index; map it back to its GbaActorInit to find the script.
// gbs2 G3b: an actor with a collision group is never interacted with - its script's
// argument-0 section is its On Hit: Player tab, which runs when the player touches it
// (hw_player_collision_update). gbvm's TOPDOWN, PLATFORM and ADVENTURE skip it the same way.
// The scene data of a runtime actor, or null.
static const GbaActorInit * actor_init_of(int actor)
{
    const GbaScene & s = gba_scenes[current_scene];
    for(unsigned int i = 0; i < s.actors_init_count; ++i)
        if(s.actors_init[i].index == (unsigned char)actor) return &s.actors_init[i];
    return nullptr;
}

static int actor_has_interact(int actor)
{
    const GbaActorInit * ai = actor_init_of(actor);
    return (ai && ai->interact) ? 1 : 0;
}

extern "C" void gba_check_interact(void)
{
    // POINTNCLICK interacts through the cursor instead (gba_pointnclick_update).
    if(gba_scenes[current_scene].scene_type == 4) return;
    if(!hw_player_on_grid()) return; // gbs2 G5: TOPDOWN interacts only on the grid, as gbvm
    const int actor = hw_interact_actor(actor_has_interact);
    if(actor < 0) return;
    const GbaActorInit * ai = actor_init_of(actor);
    if(!ai || (ai->collision_group & 0x0F)) return;
    // gbvm's TOPDOWN turns the actor to face the player first (gbs2 G5).
    if(gba_scenes[current_scene].scene_type == GBA_SCENE_TOPDOWN) hw_actor_set_dir(int16_t(actor), uint8_t(hw_player_dir() ^ 2));
    // Pass parameter 0 (M10g), as gbvm does. Plain interact scripts never read the arg.
    script_execute(0, ai->interact, nullptr, 1, 0);
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
