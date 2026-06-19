// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Hardware-ops bridge implementation (Butano / C++). Re-creates the slice of
// GB Studio's actor/input model needed for a controllable sprite, mapped onto
// Butano. Actor state is plain data; it is pushed into bn::sprite_ptr only in
// hw_render(), so VM opcode handlers never touch Butano objects directly.

#include "hw.h"

#include "bn_core.h"
#include "bn_color.h"
#include "bn_fixed.h"
#include "bn_keypad.h"
#include "bn_optional.h"
#include "bn_sprite_ptr.h"
#include "bn_sprite_tiles_item.h"
#include "bn_regular_bg_ptr.h"
#include "bn_bg_palettes.h"

#include "scene_table.h" // generated: per-scene bg + actor sprite bindings + script offsets

namespace
{
    constexpr int MAX_ACTORS = 8;
    constexpr int SUBPX = 16;            // subpixels per pixel (kept consistent with bytecode)

    struct Actor
    {
        bool active = false;
        bool visible = true;
        uint16_t x = 0;                  // position in subpixels
        uint16_t y = 0;
        uint8_t dir = 0;                 // facing: 0 down, 1 right, 2 up, 3 left
        bool moving = false;             // moved this frame (set by hw_actor_set_pos)
        uint16_t anim_timer = 0;         // advances per frame to cycle animation frames
        bn::optional<bn::sprite_ptr> sprite;   // created lazily on first render
    };

    Actor actors[MAX_ACTORS];
    bool sprites_hidden = false;
    bn::optional<bn::regular_bg_ptr> scene_bg;   // the current scene's background
    const GbaSceneActor* cur_actors = nullptr;   // current scene's actor bindings (scene_table)
    int cur_n = 0;

    // subpixel world coords -> Butano screen-centered pixel coords (0,0 == screen center)
    bn::fixed to_screen_x(uint16_t sx) { return bn::fixed(int(sx) / SUBPX) - 120; }
    bn::fixed to_screen_y(uint16_t sy) { return bn::fixed(int(sy) / SUBPX) - 80; }
}

void hw_init(void)
{
    bn::bg_palettes::set_transparent_color(bn::color(2, 4, 12));
    // The background + actors are loaded per-scene by scene_load() -> hw_load_scene().
}

void hw_load_scene(int index)
{
    hw_scene_unload();
    const GbaScene& sc = gba_scenes[index];
    scene_bg.reset();
    scene_bg = sc.bg->create_bg(0, 0); // centred; a >screen bg shows its middle
    cur_actors = sc.actors;
    cur_n = sc.n_actors;
    // Activate actors that have a sprite so they render; the scene init script
    // then positions them (VM_ACTOR_SET_POS) and may activate others.
    for(int i = 0; i < cur_n && i < MAX_ACTORS; ++i)
    {
        if(cur_actors[i].item) actors[i].active = true;
    }
}

void hw_scene_unload(void)
{
    for(int i = 0; i < MAX_ACTORS; ++i)
    {
        actors[i].sprite.reset(); // free Butano sprite-tile VRAM across switches
        actors[i].active = false;
        actors[i].visible = true;
        actors[i].x = 0;
        actors[i].y = 0;
        actors[i].dir = 0;
        actors[i].moving = false;
        actors[i].anim_timer = 0;
    }
    cur_actors = nullptr;
    cur_n = 0;
}

void hw_render(void)
{
    for(int i = 0; i < MAX_ACTORS; ++i)
    {
        Actor& a = actors[i];
        if(a.active)
        {
            const GbaSceneActor* def = (cur_actors && i < cur_n) ? &cur_actors[i] : nullptr;
            const bn::sprite_item* item = (def && def->item) ? def->item : nullptr;
            if(item)
            {
                if(!a.sprite) a.sprite = item->create_sprite(0, 0);
                // Select a frame for the actor's facing + moving state and animate.
                const int anim = (a.dir & 3) + (a.moving ? 4 : 0);
                const int len = def->anim_len[anim] ? def->anim_len[anim] : 1;
                const int frame = def->anim_start[anim] + ((a.anim_timer >> 3) % len);
                a.sprite->set_tiles(item->tiles_item(), frame);
                a.sprite->set_position(to_screen_x(a.x), to_screen_y(a.y));
                a.sprite->set_visible(a.visible && !sprites_hidden);
                a.anim_timer++;
            }
            a.moving = false; // re-set next frame if the script moves the actor again
        }
        else if(a.sprite)
        {
            a.sprite->set_visible(false);
        }
    }
}

void hw_set_sprites_visible(uint8_t mode)
{
    sprites_hidden = (mode != 0);
}

void hw_actor_activate(int16_t id)
{
    if(id >= 0 && id < MAX_ACTORS) { actors[id].active = true; actors[id].visible = true; }
}

void hw_actor_deactivate(int16_t id)
{
    if(id >= 0 && id < MAX_ACTORS) { actors[id].active = false; }
}

void hw_actor_set_pos(uint16_t* pos)
{
    int id = int16_t(pos[0]);
    if(id < 0 || id >= MAX_ACTORS) return;
    Actor& a = actors[id];
    // Infer facing from the movement delta so the actor turns as it walks
    // (until VM_ACTOR_SET_DIR lands in a later gameplay phase).
    const int dx = int(int16_t(pos[1])) - int(int16_t(a.x));
    const int dy = int(int16_t(pos[2])) - int(int16_t(a.y));
    if(dx != 0 || dy != 0)
    {
        a.moving = true;
        const int adx = dx < 0 ? -dx : dx;
        const int ady = dy < 0 ? -dy : dy;
        if(adx >= ady) a.dir = (dx >= 0) ? 1 : 3; // right / left
        else           a.dir = (dy >= 0) ? 0 : 2; // down / up
    }
    a.x = pos[1];
    a.y = pos[2];
}

void hw_actor_get_pos(uint16_t* pos)
{
    int id = int16_t(pos[0]);
    if(id >= 0 && id < MAX_ACTORS) { pos[1] = actors[id].x; pos[2] = actors[id].y; }
}

void hw_actor_set_dir(int16_t id, uint8_t dir)
{
    // dir encoding matches GB Studio's .DIR_* (0 down, 1 right, 2 up, 3 left).
    if(id >= 0 && id < MAX_ACTORS) actors[id].dir = dir & 3;
}

void hw_actor_get_angle(uint16_t* params, int16_t* dest)
{
    // dir encoding (see hw_actor_set_pos): 0=down,1=right,2=up,3=left.
    // GB Studio BRADS angles (clockwise, 256/turn, 0=up): up=0,right=64,down=128,left=192.
    static const uint8_t dir_angle_lookup[4] = { 128, 64, 0, 192 };
    int id = int16_t(params[0]);
    if(id >= 0 && id < MAX_ACTORS) *dest = dir_angle_lookup[actors[id].dir & 3];
}

void hw_input_get(uint16_t* dst, uint8_t joyid)
{
    (void)joyid;
    // Bit order must match GB Studio's KEY_BITS (the masks the editor emits in
    // EVENT_IF_INPUT): direction keys in the low nibble, buttons in the high nibble.
    uint16_t m = 0;
    if(bn::keypad::right_held())  m |= 0x01;
    if(bn::keypad::left_held())   m |= 0x02;
    if(bn::keypad::up_held())     m |= 0x04;
    if(bn::keypad::down_held())   m |= 0x08;
    if(bn::keypad::a_held())      m |= 0x10;
    if(bn::keypad::b_held())      m |= 0x20;
    if(bn::keypad::select_held()) m |= 0x40;
    if(bn::keypad::start_held())  m |= 0x80;
    *dst = m;
}
