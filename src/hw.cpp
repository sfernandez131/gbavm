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
#include "bn_bg_palettes.h"

#include "bn_sprite_items_hero.h"

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
        bn::optional<bn::sprite_ptr> sprite;   // created lazily on first render
    };

    Actor actors[MAX_ACTORS];
    bool sprites_hidden = false;

    // subpixel world coords -> Butano screen-centered pixel coords (0,0 == screen center)
    bn::fixed to_screen_x(uint16_t sx) { return bn::fixed(int(sx) / SUBPX) - 120; }
    bn::fixed to_screen_y(uint16_t sy) { return bn::fixed(int(sy) / SUBPX) - 80; }
}

void hw_init(void)
{
    // Distinct dark-blue backdrop so the magenta sprite stands out for verification.
    bn::bg_palettes::set_transparent_color(bn::color(2, 4, 12));
    // Actors are now activated/positioned by the VM script (increment 5b).
}

void hw_render(void)
{
    for(int i = 0; i < MAX_ACTORS; ++i)
    {
        Actor& a = actors[i];
        if(a.active)
        {
            if(!a.sprite)
            {
                a.sprite = bn::sprite_items::hero.create_sprite(0, 0);
            }
            a.sprite->set_position(to_screen_x(a.x), to_screen_y(a.y));
            a.sprite->set_visible(a.visible && !sprites_hidden);
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
    if(id >= 0 && id < MAX_ACTORS) { actors[id].x = pos[1]; actors[id].y = pos[2]; }
}

void hw_actor_get_pos(uint16_t* pos)
{
    int id = int16_t(pos[0]);
    if(id >= 0 && id < MAX_ACTORS) { pos[1] = actors[id].x; pos[2] = actors[id].y; }
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
