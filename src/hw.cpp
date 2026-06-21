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
#include "bn_sprite_palettes.h"
#include "bn_camera_ptr.h"

#include "bn_sprite_items_hero.h"
#include "gba_scene_assets.h" // generated: scene -> background + actor sprite table

namespace
{
    constexpr int MAX_ACTORS = 8;
    constexpr int SUBPX = 32;            // GBVM actor position units per pixel (256 per 8px tile)
    constexpr int HALF_W = 120;          // half the 240x160 GBA screen
    constexpr int HALF_H = 80;

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
    bn::optional<bn::camera_ptr> camera;         // bg + sprites scroll with this
    int current_scene = 0;                       // index for per-scene sprite lookup
    int scene_w_px = 240;                        // scene logical size (for camera bounds)
    int scene_h_px = 160;

    // GBVM actor subpixels -> Butano world pixels. The scene is centred on the world
    // origin (the bg content is centred on its padded map, which create_bg(0,0) puts
    // at the origin), so screen placement is left to the camera.
    bn::fixed to_world_x(uint16_t sx) { return bn::fixed(int(sx) / SUBPX - scene_w_px / 2); }
    bn::fixed to_world_y(uint16_t sy) { return bn::fixed(int(sy) / SUBPX - scene_h_px / 2); }

    // Clamp a camera centre (world px) so the 240x160 view stays within the scene.
    // A scene no bigger than the screen on an axis stays centred (no scroll).
    bn::fixed clamp_cam(bn::fixed c, int scene_size, int half)
    {
        const int limit = scene_size / 2 - half;
        if(limit <= 0) return 0;
        if(c < -limit) return bn::fixed(-limit);
        if(c >  limit) return bn::fixed(limit);
        return c;
    }

    // Screen fade (VM_FADE). fade_intensity: 0 = fully visible, 1 = fully black.
    // A fade runs over FADE_FRAMES frames toward its target; fade_dir is the active
    // direction (0 idle, -1 fading in, +1 fading out).
    constexpr int FADE_FRAMES = 16; // frames for a full fade (~0.27s)
    bn::fixed fade_intensity = 0;
    int fade_dir = 0;

    void apply_fade()
    {
        bn::bg_palettes::set_fade(bn::color(0, 0, 0), fade_intensity);
        bn::sprite_palettes::set_fade(bn::color(0, 0, 0), fade_intensity);
    }
}

void hw_init(void)
{
    bn::bg_palettes::set_transparent_color(bn::color(2, 4, 12));
}

void hw_load_scene(int scene_idx, int width_px, int height_px)
{
    // Swap in this scene's background and clear actors carried from a previous scene;
    // gba_load_scene then activates the new scene's actors. The camera (shared by bg
    // + sprites) follows the active actor each frame, clamped to the scene size.
    current_scene = scene_idx;
    scene_w_px = width_px  > 0 ? width_px  : 240;
    scene_h_px = height_px > 0 ? height_px : 160;
    if(!camera) camera = bn::camera_ptr::create(0, 0);
    else        camera->set_position(0, 0);
    scene_bg = gba_create_scene_bg(scene_idx);
    scene_bg->set_camera(*camera);
    for(int i = 0; i < MAX_ACTORS; ++i)
    {
        actors[i].active = false;
        actors[i].sprite.reset();
    }
}

void hw_render(void)
{
    // Camera follows the lowest-index active actor (the player / first placed actor),
    // clamped so the view never leaves the scene.
    if(camera)
    {
        for(int i = 0; i < MAX_ACTORS; ++i)
        {
            if(actors[i].active)
            {
                camera->set_position(clamp_cam(to_world_x(actors[i].x), scene_w_px, HALF_W),
                                     clamp_cam(to_world_y(actors[i].y), scene_h_px, HALF_H));
                break;
            }
        }
    }

    for(int i = 0; i < MAX_ACTORS; ++i)
    {
        Actor& a = actors[i];
        if(a.active)
        {
            const GbaActorSprite* def = gba_actor_sprite(current_scene, i);
            const bn::sprite_item* item = (def && def->item) ? def->item : nullptr;
            if(!a.sprite)
            {
                a.sprite = item ? item->create_sprite(0, 0)
                                : bn::sprite_items::hero.create_sprite(0, 0);
                if(camera) a.sprite->set_camera(*camera);
            }
            if(item)
            {
                // Select a frame for the actor's facing + moving state and animate.
                const int anim = (a.dir & 3) + (a.moving ? 4 : 0);
                const int len = def->anim_len[anim] ? def->anim_len[anim] : 1;
                const int frame = def->anim_start[anim] + ((a.anim_timer >> 3) % len);
                a.sprite->set_tiles(item->tiles_item(), frame);
            }
            a.sprite->set_position(to_world_x(a.x), to_world_y(a.y));
            a.sprite->set_visible(a.visible && !sprites_hidden);
            a.anim_timer++;
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

int hw_fade_step(uint8_t flags)
{
    const bool fade_in = (flags & 0x02) != 0; // .FADE_IN = 0x02, else fade out
    const int want_dir = fade_in ? -1 : 1;
    if(fade_dir != want_dir)
    {
        // First frame of this fade: snap to the opposite end before stepping.
        fade_intensity = fade_in ? bn::fixed(1) : bn::fixed(0);
        fade_dir = want_dir;
    }
    fade_intensity += bn::fixed(want_dir) / FADE_FRAMES;
    int done = 0;
    if(fade_in && fade_intensity <= 0)       { fade_intensity = 0; fade_dir = 0; done = 1; }
    else if(!fade_in && fade_intensity >= 1) { fade_intensity = 1; fade_dir = 0; done = 1; }
    apply_fade();
    return done;
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
