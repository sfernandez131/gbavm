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
#include "bn_sprite_text_generator.h"
#include "bn_vector.h"
#include "bn_window.h"
#include "bn_rect_window.h"
#include "bn_dmg_music.h" // DMG (Game Boy) channel music playback (M5a)
#include "common_variable_8x16_sprite_font.h"

#include "bn_sprite_items_hero.h"
#include "bn_sprite_items_dialogue_frame.h" // committed asset: 2px frame line (top border)
#include "bn_regular_bg_items_dialogue_panel.h" // committed asset: solid dialogue panel bg
#include "gba_scene_assets.h" // generated: scene -> background + actor sprite table
#include "gba_avatar_assets.h" // generated: avatar index -> sprite (dialogue portraits)
#include "gba_font_assets.h" // generated: the project's default dialogue font
#include "gba_music_assets.h" // generated: DMG music track index -> dmg_music_item (M5a)
#include "gba_sfx_assets.h" // generated: sound index -> sound_item (M5b)

namespace
{
    constexpr int MAX_ACTORS = 8;
    constexpr int SUBPX = 32;            // GBVM actor position units per pixel (256 per 8px tile)
    constexpr int HALF_W = 120;          // half the 240x160 GBA screen
    constexpr int HALF_H = 80;
    constexpr int MOVE_SPEED = 16;       // actor move-to speed in subpixels/frame (~0.5px)

    struct Actor
    {
        bool active = false;
        bool visible = true;
        uint16_t x = 0;                  // position in subpixels
        uint16_t y = 0;
        uint16_t dest_x = 0;             // move-to target in subpixels
        uint16_t dest_y = 0;
        uint8_t dir = 0;                 // facing: 0 down, 1 right, 2 up, 3 left
        bool moving = false;             // moved this frame (set by hw_actor_set_pos)
        uint16_t anim_timer = 0;         // advances per frame to cycle animation frames
        bn::optional<bn::sprite_ptr> sprite;   // created lazily on first render
    };

    Actor actors[MAX_ACTORS];
    bool sprites_hidden = false;
    bool player_move_enabled = false;            // top-down d-pad control of actor 0
    const uint8_t* collisions = nullptr;         // scene collision grid (one byte/tile)
    int coll_w = 0, coll_h = 0;                  // collision grid size in tiles
    bn::vector<bn::sprite_ptr, 48> text_sprites;       // dialogue text glyph sprites
    bool text_showing = false;
    // Typewriter reveal (M4e): the line is revealed char-by-char over frames. A
    // press fast-forwards to the full line; once full, A dismisses (see hw_text_step).
    constexpr int REVEAL_FRAMES = 2;             // default frames per character (~30/s; no speed code)
    char text_buf[64];                           // the clamped text being revealed (multi-line, w/ codes)
    int text_len = 0;                            // total bytes in text_buf
    int text_revealed = 0;                       // bytes consumed so far (incl. control codes)
    int text_lines = 1;                          // number of '\n'-separated lines (M4f)
    int reveal_timer = 0;                        // frames since the last char appeared
    int reveal_frames = REVEAL_FRAMES;           // current per-char delay (set by \001 speed codes, M4g)
    int text_rendered = -1;                      // revealed count last drawn (skip redundant redraws)
    const char* last_text = nullptr;             // last display-op text ptr (detects a new op vs a rewind)

    // Set-speed text code (M4g): GB Studio's \001<n> sets the typewriter rate. n is
    // speed+1; speed indexes ui_time_masks (ported from gbvm ui_a.s) and the engine
    // draws a char when (game_time & mask)==0, i.e. mask+1 frames/char. Speed 0 is
    // instant (the whole line at once). Returns frames/char (0 = instant).
    int speed_to_frames(int param)
    {
        static const uint8_t ui_time_masks[8] = { 0, 0, 1, 3, 7, 15, 31, 63 };
        const int speed = (param - 1) & 7;
        return speed == 0 ? 0 : (ui_time_masks[speed] + 1);
    }
    // Write a signed value as decimal into `out` (up to `max` chars), padded to at
    // least `width` characters with leading zeros (width 0 = no padding). Returns the
    // number of chars written. Substitutes %d (width 0) and %D<width> placeholders.
    int format_dec(int v, int width, char* out, int max)
    {
        char tmp[8];
        int t = 0;
        unsigned int u = (v < 0) ? (unsigned int)(-v) : (unsigned int)v;
        do { tmp[t++] = (char)('0' + (u % 10)); u /= 10; } while(u && t < (int)sizeof(tmp));
        const int digits = t + (v < 0 ? 1 : 0);  // incl. the sign
        int n = 0;
        if(v < 0 && n < max) out[n++] = '-';
        for(int p = digits; p < width && n < max; ++p) out[n++] = '0'; // leading zeros
        while(t > 0 && n < max) out[n++] = tmp[--t];
        return n;
    }

    // Substitute %d/%D<w>/%c/%% placeholders from `values` into text_buf starting at
    // offset `n` (the rest of text is copied verbatim, incl. \001/\002 codes + \n), up
    // to the buffer cap. Adds any '\n' count to *lines. Returns the new length. Used for
    // both the fresh latch (n=0) and !W: append (n=text_len). Each chunk's values are
    // its own, so the value cursor restarts at 0.
    int subst_text(const char* text, const int16_t* values, int n_values, int n, int* lines)
    {
        const int cap = (int)sizeof(text_buf) - 1;
        int vi = 0;
        for(int i = 0; text && text[i] && n < cap; )
        {
            if(text[i] == '%' && text[i + 1] == 'd' && vi < n_values)
            {
                n += format_dec(values[vi++], 0, &text_buf[n], cap - n);
                i += 2;
            }
            else if(text[i] == '%' && text[i + 1] == 'D' && vi < n_values)
            {
                int w = 0, j = i + 2;
                while(text[j] >= '0' && text[j] <= '9') { w = w * 10 + (text[j] - '0'); ++j; }
                n += format_dec(values[vi++], w, &text_buf[n], cap - n);
                i = j;
            }
            else if(text[i] == '%' && text[i + 1] == 'c' && vi < n_values)
            {
                text_buf[n++] = (char)(values[vi++] & 0xff); // value as a character code
                i += 2;
            }
            else if(text[i] == '%' && text[i + 1] == '%')
            {
                text_buf[n++] = '%';
                i += 2;
            }
            else
            {
                if(text[i] == '\n') ++(*lines);
                text_buf[n++] = text[i++];
            }
        }
        text_buf[n] = '\0';
        return n;
    }

    // Consume any \001 set-speed codes at the reveal cursor (each is the code byte +
    // a param byte), applying the new rate. Control codes are instant (no reveal tick)
    // and are skipped when rendering glyphs, so the cursor steps past them here.
    void consume_text_codes()
    {
        while(text_revealed < text_len &&
              (text_buf[text_revealed] == 0x01 || text_buf[text_revealed] == 0x02))
        {
            if(text_buf[text_revealed] == 0x01) // set-speed: apply the new rate
            {
                const int param = (text_revealed + 1 < text_len)
                                    ? (uint8_t)text_buf[text_revealed + 1] : 2;
                reveal_frames = speed_to_frames(param);
            }
            // \002 set-font is applied at render time; just step the cursor past it.
            text_revealed += 2;
        }
    }

    // Render one same-font run of glyphs at (x,y); returns its pixel width so the
    // caller can advance x to the next segment. A generator is built per run so each
    // \002 segment can use its own project font (M4p).
    int render_text_run(int font_idx, int x, int y, const char* run)
    {
        bn::sprite_text_generator gen(gba_dialogue_font(font_idx));
        gen.set_left_alignment();
        gen.set_bg_priority(1); // in front of the overlay panel (priority 2)
        gen.generate(x, y, run, text_sprites);
        return gen.width(run);
    }
    // Dialogue text layout (M4f): bottom-align the N-line block inside the box so the
    // box keeps a steady bottom margin regardless of line count (1 line -> y=52).
    constexpr int TEXT_X = -112;                 // left edge (screen x; box is full-width)
    constexpr int FONT_GLYPH_H = 8;              // the project font (M4n) is 8px tall
    constexpr int TEXT_LINE_H = 16;              // line pitch (keeps the box tall enough for the avatar)
    constexpr int TEXT_LINE_OFFSET = (TEXT_LINE_H - FONT_GLYPH_H) / 2; // centre the 8px glyph in its slot
    constexpr int TEXT_TOP_PAD = 4;              // px between the box top and the first line
    constexpr int TEXT_BOTTOM_MARGIN = 8;        // px between the last line and the screen bottom
    // Dialogue avatar portrait (M4m): a 16x16 sprite at the box's lower-left; the text
    // shifts right past it. Created per-dialogue from the op-0x90 avatar index.
    constexpr int AVATAR_X = -104;               // sprite centre (16px spans -112..-96)
    constexpr int AVATAR_Y = 60;                 // centre (spans 52..68, aligned with line 1)
    constexpr int AVATAR_TEXT_SHIFT = 24;        // px the text moves right to clear the avatar
    bn::optional<bn::sprite_ptr> avatar_sprite;  // current dialogue portrait (if any)
    int text_x = TEXT_X;                         // line x; shifted right when an avatar shows

    // --- dialogue overlay window box (M4d) ---
    // A solid bg panel clipped by an internal rect window to a bottom strip, drawn
    // behind the text. GB Studio drives it in 18-row screen tiles (Y=18 hidden); we
    // anchor the box to the screen bottom (Butano y=+80) and grow it upward.
    constexpr int SCREEN_BOTTOM = 80;            // Butano y of the screen's bottom edge
    constexpr int OVERLAY_SLIDE_PX = 6;          // default slide speed (px/frame)
    bn::optional<bn::regular_bg_ptr> panel_bg;   // the dialogue panel (lazily created)
    // M4j: a light 2px line capping the box top edge (a GB-Studio-style frame). The
    // box is full-width, so the top is its only visible interior edge; four 64px
    // line sprites span it, repositioned to box_top each frame.
    bn::vector<bn::sprite_ptr, 4> frame_sprites; // top border, left-to-right
    static const int FRAME_SEG_X[4] = { -96, -32, 32, 96 }; // centres covering -120..120
    bool overlay_inited = false;
    bn::fixed box_top = SCREEN_BOTTOM;           // current window top (y); 80 = hidden
    bn::fixed box_top_target = SCREEN_BOTTOM;    // target top the box slides toward
    int box_slide_px = OVERLAY_SLIDE_PX;         // this move's slide speed (px/frame)

    // GBVM overlay row Y -> the box's target top edge in Butano screen y. The box
    // bottom is the screen bottom; (18 - y) rows * 8px tall, clamped to the screen.
    bn::fixed overlay_top_for_row(int y)
    {
        int rows = 18 - y;
        if(rows < 0) rows = 0;
        if(rows > 20) rows = 20;                 // never taller than the 160px screen
        return bn::fixed(SCREEN_BOTTOM - rows * 8);
    }

    // Create the panel bg + window on first use. The panel covers the screen (a solid
    // colour) but the internal rect window shows it only inside the box; priority 2
    // keeps it in front of the scene bg and over actor sprites, behind the text.
    void overlay_init()
    {
        if(overlay_inited) return;
        panel_bg = bn::regular_bg_items::dialogue_panel.create_bg(0, 0);
        panel_bg->set_priority(2);               // scene bg = 3, text sprites = bg_priority 1
        panel_bg->set_visible(false);
        bn::window::outside().set_show_bg(*panel_bg, false); // panel only inside the box rect
        // The top-border line sprites: priority 1 (in front of the panel), hidden until shown.
        for(int i = 0; i < 4; ++i)
        {
            bn::sprite_ptr s = bn::sprite_items::dialogue_frame.create_sprite(FRAME_SEG_X[i], 0);
            s.set_bg_priority(1);
            s.set_visible(false);
            frame_sprites.push_back(s);
        }
        overlay_inited = true;
    }
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

    // Would a point at (sx, sy) subpixels sit in a solid tile? Outside the scene
    // grid counts as solid, so this also enforces the scene bounds.
    bool is_solid_subpx(int sx, int sy)
    {
        const int tx = sx / SUBPX / 8, ty = sy / SUBPX / 8; // subpx -> px -> tile
        if(tx < 0 || ty < 0 || tx >= coll_w || ty >= coll_h) return true;
        if(!collisions) return false;
        return (collisions[ty * coll_w + tx] & 0x0f) != 0; // any COLLISION_* direction bit
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
    hw_overlay_hide(); // clear any dialogue box carried from the previous scene
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

void hw_set_player_move(uint8_t enabled)
{
    player_move_enabled = (enabled != 0);
}

void hw_set_collisions(const unsigned char* grid, int width_tiles, int height_tiles)
{
    collisions = grid;
    coll_w = width_tiles;
    coll_h = height_tiles;
}

void hw_player_update(void)
{
    // Built-in top-down control: move the player (actor 0) from the live d-pad.
    // Horizontal + vertical can combine (8-way); facing prefers the horizontal axis.
    if(!player_move_enabled) return;
    Actor& p = actors[0];
    if(!p.active) return;
    // Face + animate toward the held direction, but only advance into open tiles.
    if(bn::keypad::right_held())     { p.dir = 1; p.moving = true; const uint16_t n = p.x + MOVE_SPEED; if(!is_solid_subpx(n, p.y)) p.x = n; }
    else if(bn::keypad::left_held()) { p.dir = 3; p.moving = true; const uint16_t n = p.x - MOVE_SPEED; if(!is_solid_subpx(n, p.y)) p.x = n; }
    if(bn::keypad::up_held())        { if(!p.moving) p.dir = 2; p.moving = true; const uint16_t n = p.y - MOVE_SPEED; if(!is_solid_subpx(p.x, n)) p.y = n; }
    else if(bn::keypad::down_held()) { if(!p.moving) p.dir = 0; p.moving = true; const uint16_t n = p.y + MOVE_SPEED; if(!is_solid_subpx(p.x, n)) p.y = n; }
}

void hw_overlay_move_to(int x, int y, int speed)
{
    // Non-blocking: set the box target; hw_overlay_update slides it there. The box is
    // full-width at the bottom (x/width are ignored for the dialogue box for now).
    (void)x;
    overlay_init();
    box_top_target = overlay_top_for_row(y);
    box_slide_px = (speed > 0) ? speed : OVERLAY_SLIDE_PX; // negatives are sentinels
    if(speed == -3) box_top = box_top_target;              // .OVERLAY_SPEED_INSTANT
    // M4q: hiding the box (e.g. the dialogue's slide-out) ends the text, so clear the
    // glyphs + avatar (they aren't window-clipped) and reset for the next display op.
    if(box_top_target >= SCREEN_BOTTOM)
    {
        text_sprites.clear();
        avatar_sprite.reset();
        text_showing = false;
        last_text = nullptr;
    }
}

void hw_overlay_show(int x, int y, int color)
{
    // Show the box at row y immediately (used by menus/choices; colour ignored for now).
    (void)x; (void)color;
    overlay_init();
    box_top_target = overlay_top_for_row(y);
    box_top = box_top_target;
}

void hw_overlay_hide(void)
{
    // Snap the box off the bottom of the screen + clear its text (M4q).
    box_top_target = SCREEN_BOTTOM;
    box_top = SCREEN_BOTTOM;
    text_sprites.clear();
    avatar_sprite.reset();
    text_showing = false;
    last_text = nullptr;
}

// VM_OVERLAY_WAIT (M4q): return 1 once every requested UI condition is met, else 0 so
// the VM blocks (rewinds + yields). This is where dialogue waits for A now (the display
// op only reveals): the wait flags are .UI_WAIT_WINDOW (box finished sliding),
// .UI_WAIT_TEXT (text fully revealed), .UI_WAIT_BTN_A/_B/_ANY (button press).
int hw_overlay_wait(int condition)
{
    bool ok = true;
    if(condition & 0x01) ok = ok && (box_top == box_top_target);   // UI_WAIT_WINDOW
    if(condition & 0x02) ok = ok && (text_revealed >= text_len);   // UI_WAIT_TEXT
    if(condition & 0x04) ok = ok && bn::keypad::a_pressed();        // UI_WAIT_BTN_A
    if(condition & 0x08) ok = ok && bn::keypad::b_pressed();        // UI_WAIT_BTN_B
    if(condition & 0x10)                                            // UI_WAIT_BTN_ANY
        ok = ok && (bn::keypad::a_pressed() || bn::keypad::b_pressed() ||
                    bn::keypad::start_pressed() || bn::keypad::select_pressed());
    return ok ? 1 : 0;
}

// --- DMG music (M5a): VM_MUSIC_PLAY / VM_MUSIC_STOP via Butano's DMG audio backend ---
void hw_music_play(int track, int loop)
{
    const bn::dmg_music_item* item = gba_music_track(track);
    if(item) item->play(1, loop != 0); // speed 1 (the VGM/DMG default), loop per the op
}

void hw_music_stop(void)
{
    if(bn::dmg_music::playing()) bn::dmg_music::stop();
}

// VM_SFX_PLAY (M5b): play the resolved .wav sound on Butano's DirectSound mixer (Maxmod),
// separate from the DMG music channels, so SFX and music coexist.
void hw_sfx_play(int sfx)
{
    const bn::sound_item* s = gba_sfx(sfx);
    if(s) s->play();
}

void hw_overlay_update(void)
{
    if(!overlay_inited) return;
    // Slide the current top toward the target by the move's speed, snapping on arrival.
    if(box_top < box_top_target)
    {
        box_top += box_slide_px;
        if(box_top > box_top_target) box_top = box_top_target;
    }
    else if(box_top > box_top_target)
    {
        box_top -= box_slide_px;
        if(box_top < box_top_target) box_top = box_top_target;
    }
    // Show the panel + top-border line only while the box has height.
    const bool visible = box_top < SCREEN_BOTTOM;
    panel_bg->set_visible(visible);
    if(visible)
        bn::rect_window::internal().set_boundaries(box_top, -HALF_W, SCREEN_BOTTOM, HALF_W);
    // The frame line's 2px stripe sits at the top of the 32px sprite, so centre it
    // box_top + 16 to cap the panel's top edge; the sprites track the slide.
    for(bn::sprite_ptr& s : frame_sprites)
    {
        s.set_visible(visible);
        if(visible) s.set_y(box_top + 16);
    }
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

int hw_text_step(const char* text, const int16_t* values, int n_values, int avatar, int preserve)
{
    // Reveal the dialogue text char-by-char (typewriter), returning 1 once it is fully
    // revealed (on a previous frame) so the VM advances to VM_OVERLAY_WAIT, which now
    // owns the A-wait (M4q). The op rewinds its PC until we return 1, so the reveal
    // state persists across calls; `text != last_text` marks a NEW display op (vs a
    // rewind) to latch fresh, or to append when `preserve` (VM_DISPLAY_TEXT_EX) for !W:.
    if(text != last_text)
    {
        last_text = text;
        if(preserve && text_showing)
        {
            // !W: append this chunk, continuing the typewriter into the new text. Each
            // chunk carries its own %d values; resume revealing from where we stopped.
            int lines = text_lines;
            text_len = subst_text(text, values, n_values, text_len, &lines);
            text_lines = lines;
            text_rendered = -1;
            box_top_target = SCREEN_BOTTOM - (lines * TEXT_LINE_H + TEXT_TOP_PAD + TEXT_BOTTOM_MARGIN);
        }
        else
        {
            // Fresh: latch the text (substituting %d/%D/%c/%%), (re)create the avatar
            // portrait, and size the box. Speed/font codes are copied verbatim.
            int lines = 1;
            text_len = subst_text(text, values, n_values, 0, &lines);
            text_lines = lines;
            text_revealed = 0;
            reveal_timer = 0;
            reveal_frames = REVEAL_FRAMES;   // default until a \001 speed code changes it
            text_rendered = -1;
            text_sprites.clear();
            text_showing = true;
            consume_text_codes();            // apply any leading speed code before char 1
            // Avatar portrait (M4m): draw the sprite at the box's lower-left and shift
            // the text right to clear it; no avatar (0xFF) keeps the text at its normal x.
            const bn::sprite_item* av = (avatar != 0xff) ? gba_avatar_sprite(avatar) : nullptr;
            if(av)
            {
                avatar_sprite = av->create_sprite(AVATAR_X, AVATAR_Y);
                avatar_sprite->set_bg_priority(1); // in front of the panel, like the text
                text_x = TEXT_X + AVATAR_TEXT_SHIFT;
            }
            else
            {
                avatar_sprite.reset();
                text_x = TEXT_X;
            }
            // Size the box to fit this text (TEXT_LINE_H pitch keeps it tall enough for
            // the avatar); the script's overlay slide still controls show/hide.
            overlay_init();
            box_top_target = SCREEN_BOTTOM - (lines * TEXT_LINE_H + TEXT_TOP_PAD + TEXT_BOTTOM_MARGIN);
        }
    }
    const bool was_revealed = (text_revealed >= text_len); // already done on a prior frame?
    if(text_revealed < text_len)
    {
        // Still typing: A fast-forwards to the full text; speed 0 reveals instantly;
        // otherwise tick the timer, revealing one char and consuming any codes after it.
        if(bn::keypad::a_pressed() || reveal_frames <= 0)
        {
            text_revealed = text_len;
        }
        else if(++reveal_timer >= reveal_frames)
        {
            reveal_timer = 0;
            ++text_revealed;
            consume_text_codes(); // apply inline speed codes following this char
        }
    }
    // Redraw only when the revealed count changed (avoids rebuilding sprites each frame).
    if(text_revealed != text_rendered)
    {
        // Copy the revealed bytes, dropping \001 set-speed codes + their params (timing
        // only) but KEEPING \002 set-font codes as segment boundaries (and \n as line
        // boundaries) so the renderer can switch font mid-line.
        char shown[sizeof(text_buf)];
        int s = 0;
        for(int i = 0; i < text_revealed; ++i)
        {
            if(text_buf[i] == 0x01) { ++i; continue; } // drop speed code + its param byte
            shown[s++] = text_buf[i];
        }
        shown[s] = '\0';
        text_sprites.clear();
        // Walk the revealed text, rendering each same-font run on its line, bottom-
        // aligned (the box is sized taller for more lines). \002 switches font and the
        // x advances by each run's width; \n starts a new line (font persists). The
        // dialogue starts in the default font (index 0).
        int y = SCREEN_BOTTOM - text_lines * TEXT_LINE_H - TEXT_BOTTOM_MARGIN + TEXT_LINE_OFFSET;
        int x = text_x;
        int font_idx = 0;
        char run[sizeof(text_buf)];
        int r = 0;
        for(int i = 0;; ++i)
        {
            const char ch = shown[i];
            if(ch == 0x02) // set-font: flush the current run, then select the new font
            {
                run[r] = '\0';
                if(r > 0) x += render_text_run(font_idx, x, y, run);
                r = 0;
                const int param = shown[i + 1] ? (uint8_t)shown[i + 1] : 1;
                font_idx = param - 1;
                if(font_idx < 0 || font_idx >= gba_dialogue_font_count) font_idx = 0;
                ++i; // skip the param byte
            }
            else if(ch == '\n' || ch == '\0')
            {
                run[r] = '\0';
                if(r > 0) render_text_run(font_idx, x, y, run);
                r = 0;
                if(ch == '\0') break;
                y += TEXT_LINE_H;
                x = text_x; // new line: reset x (the avatar shift is baked into text_x)
            }
            else
            {
                run[r++] = ch;
            }
        }
        text_rendered = text_revealed;
    }
    // Done once fully revealed on a PRIOR frame: the VM advances to VM_OVERLAY_WAIT.
    // (Returning the frame after completion, not the same frame, keeps the A press that
    // fast-forwarded the typewriter from also satisfying the following A-wait.)
    return was_revealed ? 1 : 0;
}

void hw_actor_activate(int16_t id)
{
    if(id >= 0 && id < MAX_ACTORS) { actors[id].active = true; actors[id].visible = true; }
}

void hw_actor_place(int16_t id, uint16_t x, uint16_t y, uint8_t dir)
{
    if(id < 0 || id >= MAX_ACTORS) return;
    Actor& a = actors[id];
    a.active = true;
    a.visible = true;
    a.x = x;
    a.y = y;
    a.dir = dir & 3;
    a.moving = false; // a placement is not movement; don't trigger the walk frames
}

// Move one axis toward the destination by MOVE_SPEED, snapping when within range.
// `cross` is the other axis' position (for the collision check). Returns true once
// that axis reaches its target OR a solid tile blocks it (the move stops there).
static bool move_axis(uint16_t& pos, uint16_t dest, uint16_t cross, bool axis_x)
{
    const int d = int(dest) - int(pos);
    if(d == 0) return true;
    const uint16_t step = (d > 0) ? ((d <= MOVE_SPEED) ? dest : (uint16_t)(pos + MOVE_SPEED))
                                  : ((-d <= MOVE_SPEED) ? dest : (uint16_t)(pos - MOVE_SPEED));
    if(axis_x ? is_solid_subpx(step, cross) : is_solid_subpx(cross, step)) return true; // blocked: stop
    pos = step;
    return pos == dest;
}

void hw_actor_move_init(int16_t id, uint16_t dest_x, uint16_t dest_y)
{
    if(id < 0 || id >= MAX_ACTORS) return;
    actors[id].dest_x = dest_x;
    actors[id].dest_y = dest_y;
}

int hw_actor_move_step(int16_t id, uint8_t axis)
{
    if(id < 0 || id >= MAX_ACTORS) return 1;
    Actor& a = actors[id];
    bool done = true;
    if(axis == 0 || axis == 2) done &= move_axis(a.x, a.dest_x, a.y, true);
    if(axis == 1 || axis == 2) done &= move_axis(a.y, a.dest_y, a.x, false);
    a.moving = true; // animate as walking until the move op stops re-running
    return done ? 1 : 0;
}

void hw_actor_move_set_dir(int16_t id, uint8_t axis)
{
    if(id < 0 || id >= MAX_ACTORS) return;
    Actor& a = actors[id];
    if(axis == 0) { const int d = int(a.dest_x) - int(a.x); if(d) a.dir = (d > 0) ? 1 : 3; }
    else          { const int d = int(a.dest_y) - int(a.y); if(d) a.dir = (d > 0) ? 0 : 2; }
    a.moving = true;
}

void hw_actor_move_cancel(int16_t id)
{
    if(id < 0 || id >= MAX_ACTORS) return;
    actors[id].dest_x = actors[id].x;
    actors[id].dest_y = actors[id].y;
}

void hw_actor_set_dir(int16_t id, uint8_t dir)
{
    if(id >= 0 && id < MAX_ACTORS) actors[id].dir = dir & 3;
}

void hw_actor_set_moving(int16_t id)
{
    if(id >= 0 && id < MAX_ACTORS) actors[id].moving = true;
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
