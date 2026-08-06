// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Hardware-ops bridge implementation (Butano / C++). Re-creates the slice of
// GB Studio's actor/input model needed for a controllable sprite, mapped onto
// Butano. Actor state is plain data; it is pushed into bn::sprite_ptr only in
// hw_render(), so VM opcode handlers never touch Butano objects directly.

#include "hw.h"
#include "gba_link.h" // GbaProjectileDef (M10f)
#include "vm.h"       // vm_sine (projectile launch math, M10f)

#include "bn_core.h"
#include "bn_color.h"
#include "bn_fixed.h"
#include "bn_keypad.h"
#include "bn_optional.h"
#include "bn_sprite_ptr.h"
#include "bn_sprite_tiles_item.h"
#include "bn_regular_bg_ptr.h"
#include "bn_affine_bg_ptr.h" // M8d Mode-7 scene backgrounds
#include "bn_bg_palettes.h"
#include "bn_bg_palette_ptr.h" // panel recolour for .UI_COLOR_BLACK (M11d)
#include "bn_sprite_palette_ptr.h" // sprite bank recolour for VM_LOAD_PALETTE (M12d)
#include "bn_sprite_palettes.h"
#include "bn_camera_ptr.h"
#include "bn_sprite_text_generator.h"
#include "bn_vector.h"
#include "bn_window.h"
#include "bn_rect_window.h"
#include "bn_dmg_music.h" // DMG (Game Boy) channel music playback (M5a)
#include "bn_music.h" // Maxmod (DirectSound) tracker music playback (GBA-native, alongside DMG)
#include "bn_sound.h" // DirectSound master volume (M5c)
#include "common_variable_8x16_sprite_font.h"

#include "bn_sprite_items_hero.h"
#include "bn_sprite_items_dialogue_frame.h" // committed asset: 2px frame line (top border)
#include "bn_regular_bg_items_dialogue_panel.h" // committed asset: solid dialogue panel bg
#include "gba_scene_assets.h" // generated: scene -> background + actor sprite table
#include "gba_avatar_assets.h" // generated: avatar index -> sprite (dialogue portraits)
#include "gba_font_assets.h" // generated: the project's default dialogue font
#include "gba_music_assets.h" // generated: DMG music track index -> dmg_music_item (M5a)
#include "gba_sfx_assets.h" // generated: sound index -> sound_item (M5b)
#include "gba_emote_assets.h" // generated: emote index -> sprite (M10d)

namespace
{
    // M8b: raise GB's tight actor cap. 12 active actors runs at a full 60fps
    // alongside the fixture's dialogue text + projectile sprites; 14+ trips
    // Butano's sprite budget (measured cliff, 60fps -> assert). Going higher
    // needs sprite-VRAM budgeting (shared actor tiles / a bigger sprite pool),
    // tracked in docs/M8_SUPERPOWERS_DESIGN.md. All actor loops use this
    // constant, so the bump is otherwise clean.
    // M8b: matches GB Studio's actor pool (gbvm actor.h MAX_ACTORS = 21, i.e. up
    // to 20 placed actors + the player). The GB engine only renders
    // MAX_ACTORS_ACTIVE = 12 at once (a GB hardware sprite limit); the GBA has 128
    // OAM entries, so all 21 can be active/rendered. Raised from 12 to close the
    // parity gap (GB projects with 13-20 placed actors previously lost actors on
    // GBA). Verified: 24 force-activated actors run with no Butano assert.
    constexpr int MAX_ACTORS = 21;
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
        uint8_t move_speed = MOVE_SPEED; // subpixels/frame (32 = 1px/frame); per-actor (M10a)
        uint8_t anim_state = 0;          // animation state row (M10c); 0 = default
        bool anim_noloop = false;        // M10e: clamp on the last frame instead of looping
        bool coll_enabled = true;        // M10e: false = doesn't block other actors
        uint8_t collision_group = 0;     // M10f: GB collision group bit (player 0x01)
        int sprite_sheet = -1;           // M10h: global-sprite override (-1 = the scene's sheet)
        unsigned char* hit_script = nullptr; // M10g: run on projectile hit (combined param script)
        uint16_t hit_handle = SCRIPT_TERMINATED; // M10g: refire gate (only when terminated)
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

    // Visible line count (M11b): \n advances the line and a goto code (\003 x y)
    // JUMPS to row y-2, but only rows where glyphs actually land count - so a
    // two-column menu (a \n straight into a goto back to the top row) measures
    // its visual height, not its \n count.
    int count_text_lines(const char* buf, int len)
    {
        int cur = 0, maxl = 0;
        for(int i = 0; i < len; ++i)
        {
            const char ch = buf[i];
            if(ch == 0x01 || ch == 0x02) { ++i; continue; } // skip code + 1 param
            if(ch == 0x03)                                   // goto x,y: jump rows
            {
                if(i + 2 < len)
                {
                    const int py = (uint8_t)buf[i + 2] - 2;
                    cur = py < 0 ? 0 : py;
                }
                i += 2;
                continue;
            }
            if(ch == '\n') { ++cur; continue; }
            if((uint8_t)ch >= 0x20 && (uint8_t)ch <= 0x7e && cur > maxl) maxl = cur;
        }
        return maxl + 1;
    }

    // Consume any \001 set-speed codes at the reveal cursor (each is the code byte +
    // a param byte), applying the new rate. Control codes are instant (no reveal tick)
    // and are skipped when rendering glyphs, so the cursor steps past them here.
    void consume_text_codes()
    {
        while(text_revealed < text_len &&
              (text_buf[text_revealed] == 0x01 || text_buf[text_revealed] == 0x02 ||
               text_buf[text_revealed] == 0x03))
        {
            if(text_buf[text_revealed] == 0x03) // goto x,y: layout only (code + 2 params)
            {
                text_revealed += 3;
                continue;
            }
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
    // Horizontal geometry (M11c): the box WIDTH comes from VM_OVERLAY_CLEAR and its
    // X from VM_OVERLAY_MOVE_TO, both in GB's 20-tile window space. Mapping onto the
    // 30-tile GBA screen: a full-width GB box (x 0, w 20) stays full-width; a box
    // touching GB's right edge (x + w >= 20, e.g. the "menu" layout at x 10, w 10)
    // stays right-anchored; anything else is left-anchored at x tiles.
    int box_move_x = 0;                          // VM_OVERLAY_MOVE_TO x (GB tiles)
    int box_clear_w = 20;                        // VM_OVERLAY_CLEAR w (GB tiles)
    int box_left = -HALF_W;                      // derived px bounds (see overlay_span)
    int box_right = HALF_W;
    // M11d box colour + frame option: VM_OVERLAY_CLEAR/SHOW's color picks the panel
    // fill (.UI_COLOR_WHITE = the standard panel colour, .UI_COLOR_BLACK = true
    // black - GB Studio's "Show Overlay" black screen cover) and .UI_DRAW_FRAME
    // (options bit 0) shows the top-border line. Like GB, the latch persists until
    // the next clear (every dialogue/menu emits one, so text boxes reset per use).
    bool box_frame = true;                       // draw the top-border line sprites
    bn::color panel_color;                       // the panel fill (authored or UI-palette)
    bool panel_color_overridden = false;         // M12d2: Set UI Palette replaced it
    bool panel_black_now = false;                // current fill is the black cover
    void panel_set_black(bool black)
    {
        panel_black_now = black;
        // The panel bmp is a solid fill of palette index 1; recolour it in place
        // (bg_palette_ptr is a shared handle - a copy mutates the same palette).
        bn::bg_palette_ptr pal = panel_bg->palette();
        pal.set_color(1, black ? bn::color(0, 0, 0) : panel_color);
    }
    void overlay_span()
    {
        if(box_move_x <= 0 && box_clear_w >= 20) { box_left = -HALF_W; box_right = HALF_W; }
        else if(box_move_x + box_clear_w >= 20)
        {
            box_right = HALF_W;
            box_left = HALF_W - box_clear_w * 8;
        }
        else
        {
            box_left = -HALF_W + box_move_x * 8;
            box_right = box_left + box_clear_w * 8;
        }
        if(box_left < -HALF_W) box_left = -HALF_W;
        if(box_right > HALF_W) box_right = HALF_W;
    }

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
        if(!panel_color_overridden)                     // M12d2: UI palette may pre-set it
            panel_color = panel_bg->palette().colors()[1]; // authored fill (M11d restore)
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
    // M8d: an affine (Mode-7) scene background - a scene is affine when its
    // script uses the Rotate/Scale Background event (eject emits an affine
    // asset + sets GbaScene.affine). Only one of scene_bg / scene_affine_bg is
    // live per scene. The transform op drives its rotation + scale.
    bn::optional<bn::affine_bg_ptr> scene_affine_bg;
    bn::fixed bg_rot = 0;                         // current rotation angle (degrees)
    bn::fixed bg_scale = 1;                       // current uniform scale
    bn::optional<bn::camera_ptr> camera;         // bg + sprites scroll with this

    // Choice / menu cursor state (M11a, VM_CHOICE). The menu text is a normal
    // dialogue (already displayed + revealed by the time VM_CHOICE runs); this
    // adds the cursor glyph and the item-graph navigation.
    bool choice_active = false;
    int choice_index = 0;                        // 0-based selected item
    bn::vector<bn::sprite_ptr, 2> choice_cursor; // the ">" glyph sprite(s)

    // Place the cursor glyph at a menu item's tile coords: x/y are GB window
    // tiles (1-based); lines match the dialogue layout (TEXT_LINE_H pitch).
    void choice_place_cursor(const unsigned char* item)
    {
        choice_cursor.clear();
        const int cx = text_x + (item[0] - 1) * 8;
        const int cy = SCREEN_BOTTOM - text_lines * TEXT_LINE_H - TEXT_BOTTOM_MARGIN +
                       TEXT_LINE_OFFSET + (item[1] - 1) * TEXT_LINE_H;
        bn::sprite_text_generator gen(gba_dialogue_font(0));
        gen.set_left_alignment();
        gen.set_bg_priority(1); // in front of the overlay panel, like the text
        gen.generate(cx, cy, ">", choice_cursor);
    }

    // Emote bubble state (M10d): one emote at a time, like the GB engine.
    constexpr int EMOTE_FRAMES = 60;             // ~1s on screen
    bn::optional<bn::sprite_ptr> emote_sprite;
    int emote_timer = 0;
    int emote_actor = -1;

    // Projectile pool (M10f) - GB parity: 5 runtime def slots (loaded from the
    // scene on load / VM_PROJECTILE_LOAD_TYPE) + 5 in-flight instances.
    constexpr int MAX_PROJECTILES = 5;
    constexpr int MAX_PROJECTILE_DEFS = 5;
    GbaProjectileDef projectile_defs[MAX_PROJECTILE_DEFS] = {};
    struct Projectile
    {
        bool active = false;
        GbaProjectileDef def = {};       // copied from the slot at launch
        int x = 0, y = 0;                // position in subpixels (signed: may leave the scene)
        int dx = 0, dy = 0;              // per-frame delta in subpixels
        int life = 0;                    // frames left
        uint8_t frame = 0;               // current sheet frame
        uint8_t frame_start = 0;         // the launch direction's animation range
        uint8_t frame_len = 1;
        uint8_t tick = 0;                // frame counter for the anim_tick mask
        bn::optional<bn::sprite_ptr> sprite;
    };
    Projectile projectiles[MAX_PROJECTILES];
    int current_scene = 0;                       // index for per-scene sprite lookup
    int scene_w_px = 240;                        // scene logical size (for camera bounds)
    int scene_h_px = 160;
    // M13f SHMUP scroll state: shmup_dir (player's initial facing: 0 down,
    // 1 right, 2 up, 3 left) is the scroll direction; the camera auto-scrolls
    // that way, dragging the player, until it hits the scene edge (shmup_end).
    bool shmup_inited = false;
    uint8_t shmup_dir = 0;
    bool shmup_end = false;
    bn::fixed shmup_cx = 0;
    bn::fixed shmup_cy = 0;
    int shake_frames = 0;                        // camera shake (M6h): frames of shake left
    int shake_total = 0;                         // total shake length, for amplitude decay

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
    // Audio levels. Butano's DMG master volume defaults to 25% (QUARTER) - raise it to
    // FULL so project music is clearly audible. Also set the DirectSound master volume so
    // Maxmod sound effects (M5b) are at full level (its default is not guaranteed high).
    bn::dmg_music::set_master_volume(bn::dmg_music_master_volume::FULL);
    bn::sound::set_master_volume(1);
    // GBA master sound enable (REG_SOUNDCNT_X, 0x04000084, bit 7). Butano's audio init leaves
    // this OFF in this build, and while it is off the sound hardware ignores every write to the
    // channel registers - so gbt-player's notes and Maxmod's DirectSound never take effect and
    // nothing is audible (verified via mGBA's I/O viewer: SOUNDCNT_X read 0x0000). Turn it on.
    *reinterpret_cast<volatile uint16_t*>(0x04000084) |= 0x0080;
}

// M12d: sprite palette latches. VM_LOAD_PALETTE .PALETTE_SPRITE can run before
// an actor's sprite exists (scene init - sprites are created lazily on first
// render), so each slot's last-set colours are latched and applied when a
// matching sprite comes alive. Cleared on scene load (GB reloads palettes).
namespace
{
    bn::color sprite_pal_latch[8][4];
    bool sprite_pal_set[8] = { false, false, false, false,
                               false, false, false, false };
    void apply_sprite_latch(bn::sprite_ptr& s, int slot)
    {
        if(slot < 0 || slot > 7 || !sprite_pal_set[slot]) return;
        bn::sprite_palette_ptr sp = s.palette();
        for(int c = 1; c < 4; ++c) sp.set_color(c, sprite_pal_latch[slot][c]);
    }
}

extern int16_t gba_bg_angle; // defined below (M8d)
extern int16_t gba_bg_spin;  // defined below (M8d): affine auto-spin, deg/frame x256
extern int16_t gba_bg_scale; // defined below (M8d): affine scale, x256 (256 = 1.0)

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
    // M8d: an affine scene swaps in an affine_bg (Mode-7) instead of the
    // regular bg; the transform op then rotates/scales it. Non-affine scenes
    // are unchanged (gba_create_scene_affine_bg returns nullopt in that case).
    bg_rot = 0; bg_scale = 1; gba_bg_angle = 0; gba_bg_spin = 0; gba_bg_scale = 256;
    scene_affine_bg = gba_create_scene_affine_bg(scene_idx);
    if(scene_affine_bg)
    {
        scene_bg.reset();
        scene_affine_bg->set_camera(*camera);
    }
    else
    {
        scene_bg = gba_create_scene_bg(scene_idx);
        scene_bg->set_camera(*camera);
    }
    hw_overlay_hide(); // clear any dialogue box carried from the previous scene
    for(int i = 0; i < 8; ++i) sprite_pal_set[i] = false; // palettes reload per scene (M12d)
    shmup_inited = false; // SHMUP re-inits from the new player's facing (M13f)
    emote_sprite.reset(); // drop any emote bubble from the previous scene (M10d)
    emote_timer = 0;
    emote_actor = -1;
    for(int i = 0; i < MAX_PROJECTILES; ++i) // clear in-flight projectiles + def slots (M10f)
    {
        projectiles[i].active = false;
        projectiles[i].sprite.reset();
        projectile_defs[i] = GbaProjectileDef{};
    }
    for(int i = 0; i < MAX_ACTORS; ++i)
    {
        actors[i].active = false;
        actors[i].sprite.reset();
        actors[i].collision_group = 0; // re-set per scene from GbaActorInit (M10f)
        actors[i].hit_script = nullptr; // re-set per scene (M10g)
        actors[i].hit_handle = SCRIPT_TERMINATED;
        actors[i].sprite_sheet = -1;    // back to the scene's sheet (M10h)
    }
}

// M6h: start a camera shake for `frames` frames; hw_render jitters the view (decaying
// amplitude) until it elapses. Called by the _camera_shake_frames native.
void hw_camera_shake(int frames)
{
    shake_frames = frames;
    shake_total = frames;
}

extern uint8_t gba_current_scene_type; // defined below (M13a); used by the SHMUP camera

void hw_render(void)
{
    // Camera follows the lowest-index active actor (the player / first placed actor),
    // clamped so the view never leaves the scene.
    if(camera)
    {
        bn::fixed cx = 0, cy = 0;
        if(gba_current_scene_type == 3 && shmup_inited)
        {
            cx = shmup_cx;                        // M13f: SHMUP auto-scroll camera
            cy = shmup_cy;
        }
        else
        for(int i = 0; i < MAX_ACTORS; ++i)
        {
            if(actors[i].active)
            {
                cx = clamp_cam(to_world_x(actors[i].x), scene_w_px, HALF_W);
                cy = clamp_cam(to_world_y(actors[i].y), scene_h_px, HALF_H);
                break;
            }
        }
        // Camera shake (M6h): jitter the view horizontally for shake_frames frames with an
        // amplitude that decays to 0, then settle back on the actor. Runs from hw_render so
        // it shakes even while a script blocks on the Camera Shake wait.
        if(shake_frames > 0)
        {
            const int amp = 10 * shake_frames / (shake_total ? shake_total : 1);
            cx += (shake_frames & 1) ? amp : -amp;
            --shake_frames;
        }
        camera->set_position(cx, cy);
    }

    // M8d: auto-spin the affine (Mode-7) scene bg by gba_bg_spin (deg/frame,
    // x256 fixed) each frame. Composes with the Rotate/Scale event, which sets
    // the base angle/scale; spinning advances the angle from there.
    if(scene_affine_bg && gba_bg_spin != 0)
    {
        bg_rot += bn::fixed(gba_bg_spin) / 256;
        while(bg_rot >= 360) bg_rot -= 360;
        while(bg_rot < 0)    bg_rot += 360;
        gba_bg_angle = (int16_t)bg_rot.right_shift_integer();
        scene_affine_bg->set_rotation_angle(bg_rot);
    }

    for(int i = 0; i < MAX_ACTORS; ++i)
    {
        Actor& a = actors[i];
        if(a.active)
        {
            const GbaActorSprite* def = (a.sprite_sheet >= 0)
                ? gba_global_sprite(a.sprite_sheet)      // runtime override (M10h)
                : gba_actor_sprite(current_scene, i);
            const bn::sprite_item* item = (def && def->item) ? def->item : nullptr;
            if(!a.sprite)
            {
                a.sprite = item ? item->create_sprite(0, 0)
                                : bn::sprite_items::hero.create_sprite(0, 0);
                if(camera) a.sprite->set_camera(*camera);
                if(item && def) apply_sprite_latch(*a.sprite, def->pal_slot); // M12d
            }
            if(item)
            {
                // Select a frame for the actor's animation state + facing + moving
                // and animate. States are rows of 8 engine animations (M10c); an
                // out-of-range state falls back to the default row 0.
                const int st = (a.anim_state < GBA_ANIM_STATES) ? a.anim_state : 0;
                const int anim = st * 8 + (a.dir & 3) + (a.moving ? 4 : 0);
                const int len = def->anim_len[anim] ? def->anim_len[anim] : 1;
                const int tick = a.anim_timer >> 3;
                const int frame = def->anim_start[anim] +
                    (a.anim_noloop ? ((tick < len) ? tick : len - 1) : (tick % len));
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

    // Emote bubble (M10d): follows its actor above the head, rising in over the
    // first frames, then disappears when the timer runs out. Single slot, like GB.
    if(emote_timer > 0 && emote_sprite && emote_actor >= 0)
    {
        const Actor& ea = actors[emote_actor];
        const int shown = EMOTE_FRAMES - emote_timer;
        const int lift = (shown < 8) ? shown : 8; // rise-in over the first 8 frames
        emote_sprite->set_position(to_world_x(ea.x), to_world_y(ea.y) - 12 - lift);
        emote_sprite->set_visible(ea.active && ea.visible && !sprites_hidden);
        if(--emote_timer == 0) emote_sprite.reset();
    }

    // Projectiles (M10f): move, animate, cull, hit-test, render. GB parity:
    // life_time expiry, off-view culling (GB culls outside the screen +/- 2 tiles),
    // anim advance when (tick & anim_tick) == 0, AABB hit vs actors whose
    // collision_group is in the mask (a hit removes the projectile unless strong;
    // the actor's On Hit script is the next slice, M10g).
    for(int i = 0; i < MAX_PROJECTILES; ++i)
    {
        Projectile& p = projectiles[i];
        if(!p.active) continue;
        if(--p.life <= 0) { p.active = false; p.sprite.reset(); continue; }

        p.x += p.dx;
        p.y += p.dy;

        // Cull when leaving the camera view + a 16px margin (positions are signed
        // ints here, so flying past the scene origin can't wrap).
        const int wx = p.x / SUBPX - scene_w_px / 2;
        const int wy = p.y / SUBPX - scene_h_px / 2;
        const int cam_x = camera ? camera->x().right_shift_integer() : 0;
        const int cam_y = camera ? camera->y().right_shift_integer() : 0;
        if(wx < cam_x - (HALF_W + 16) || wx > cam_x + (HALF_W + 16) ||
           wy < cam_y - (HALF_H + 16) || wy > cam_y + (HALF_H + 16))
        {
            p.active = false;
            p.sprite.reset();
            continue;
        }

        // Animate: advance a frame when the tick counter matches the mask, looping
        // (or clamping, anim_noloop) within the launch direction's range.
        if((++p.tick & p.def.anim_tick) == 0 && p.frame_len > 1)
        {
            if(p.frame + 1 < p.frame_start + p.frame_len) ++p.frame;
            else if(!p.def.anim_noloop) p.frame = p.frame_start;
        }

        // Hit test vs actors (12px AABB around both centres). Uses the same actor
        // gates as solidity (M10b/M10e): active + visible + collision-enabled.
        for(int a = 0; a < MAX_ACTORS; ++a)
        {
            const Actor& act = actors[a];
            if(!act.active || !act.visible || !act.coll_enabled) continue;
            if(!(act.collision_group & p.def.collision_mask)) continue;
            const int adx = p.x / SUBPX - int(act.x) / SUBPX;
            const int ady = p.y / SUBPX - int(act.y) / SUBPX;
            if(adx > -12 && adx < 12 && ady > -12 && ady < 12)
            {
                // On Hit (M10g): run the actor's combined hit script with the
                // projectile's collision group as thread arg 0 (the script's
                // param branches pick the On Hit tab). One at a time per actor,
                // GB parity: refire only after the previous run terminated.
                Actor& hit = actors[a];
                if(hit.hit_script && (hit.hit_handle & SCRIPT_TERMINATED))
                {
                    hit.hit_handle = 0;
                    script_execute(0, hit.hit_script, &hit.hit_handle, 1,
                                   (int)p.def.collision_group);
                }
                if(!p.def.strong) { p.active = false; p.sprite.reset(); }
                break;
            }
        }
        if(!p.active) continue;

        const GbaActorSprite* def = gba_global_sprite(p.def.sprite);
        const bn::sprite_item* item = (def && def->item) ? def->item : nullptr;
        if(!item) continue;
        if(!p.sprite)
        {
            p.sprite = item->create_sprite(0, 0);
            if(camera) p.sprite->set_camera(*camera);
        }
        p.sprite->set_tiles(item->tiles_item(), p.frame);
        p.sprite->set_position(wx, wy);
        p.sprite->set_visible(!sprites_hidden);
    }
}

void hw_set_sprites_visible(uint8_t mode)
{
    sprites_hidden = (mode != 0);
}

// M13a: the loaded scene's GBA_SCENE_* type. Exported (not anonymous-namespace)
// so the GDB stub and runtime tests can read it under LTO.
uint8_t gba_current_scene_type = 0;
// M8d: the affine scene bg's rotation angle (whole degrees), exported so the
// GDB stub / runtime tests can observe the Mode-7 transform.
int16_t gba_bg_angle = 0;
// M8d: affine auto-spin velocity (deg/frame, x256 fixed; signed). Set by the
// Spin Background event (op 0x98); hw_render advances gba_bg_angle by it each
// frame. Exported for the GDB stub / runtime tests. Reset on scene load.
int16_t gba_bg_spin = 0;
// M8d: the affine bg's current scale as x256 fixed point (256 = 1.0), exported so
// the GDB stub / runtime tests can observe it. Set by SET_BG_TRANSFORM (0x97) and
// SET_BG_SCALE_VAR (0x9A); reset to 256 on scene load.
int16_t gba_bg_scale = 256;

// --- M13b: core platformer physics (GROUND/JUMP/FALL) --------------------
// The plat_* tunables are GB Studio ENGINE FIELDS: initialized to GB's
// engine.json defaults and overwritten by compiled engine-field writes via
// M1b ram relocations, so scene-init values and runtime Engine Field Update
// events both work. extern "C" + exported so the editor's linker can bind
// relocs to them (and GDB can read them under LTO). Widths match GB
// (WORD -> int16_t, UBYTE -> uint8_t).
extern "C" {
int16_t plat_walk_vel = 6400;         // max walk velocity (GB Q8.8-ish units)
int16_t plat_walk_acc = 152;          // walk acceleration
int16_t plat_turn_acc = 712;          // extra friction when reversing
int16_t plat_dec = 208;               // ground deceleration
int16_t plat_air_dec = 208;           // air deceleration
int16_t plat_min_vel = 304;           // minimum applied velocity
int16_t plat_jump_vel = 16384;        // initial jump velocity (upward)
int16_t plat_jump_hold_vel = 0;       // extra per-frame boost while holding jump
uint8_t plat_jump_hold_frames = 1;    // frames of hold boost
int16_t plat_grav = 1792;             // gravity per frame
int16_t plat_hold_grav = 512;         // gravity while rising and holding jump
int16_t plat_max_fall_vel = 20000;    // terminal velocity
int16_t plat_climb_vel = 4000;        // ladder climb velocity (M13c)
uint8_t plat_drop_through_active = 1; // one-way drop-through enabled (M13c)
// M13d feel-parity tunables (GB engine.json defaults).
int16_t plat_run_vel = 10496;         // max run velocity (B held)
int16_t plat_run_acc = 228;           // run acceleration
int16_t plat_run_boost = 0;           // extra jump boost from horizontal speed
uint8_t plat_coyote_frames = 2;       // jump grace after leaving ground
uint8_t plat_jump_buffer_frames = 2;  // jump pre-press window before landing
uint8_t plat_extra_jumps = 1;         // air jumps (double jump)
int16_t plat_jump_reduction = 0;      // per-air-jump height reduction
uint8_t plat_air_control = 1;         // horizontal control while airborne
// Player platform state, GB's enum order: 0 FALL, 1 GROUND, 2 JUMP, (3 DASH,)
// 4 LADDER. Velocities + feel timers exported for GDB / runtime-test checks.
uint8_t plat_state = 0;
int16_t plat_vel_x = 0;
int16_t plat_vel_y = 0;
uint8_t plat_coyote_timer = 0;        // frames of coyote jump left (M13d)
uint8_t plat_jump_buffer_timer = 0;   // frames of buffered jump left (M13d)
uint8_t plat_extra_jumps_counter = 0; // air jumps left (M13d)
// M13f SHMUP: auto-scroll speed (GB subpx/frame, 16/px - so 32 = 2px/frame) +
// the exported scroll-camera centre in world px (readable for GDB / tests).
uint8_t shooter_scroll_speed = 32;
int16_t shmup_cam_x = 0;
int16_t shmup_cam_y = 0;
}

void hw_set_scene_type(uint8_t type)
{
    gba_current_scene_type = type;
}

void hw_set_player_move(uint8_t enabled)
{
    player_move_enabled = (enabled != 0);
}

namespace
{
    // M13b runtime state (velocities in GB Q8.8 units; positions in GBA 32/px
    // subpixels). GB integrates VEL_TO_SUBPX(v) = (v>>8)<<1 GB-subpixels
    // (16/px); GBA subpixels are 32/px, so the delta doubles: (v>>8)<<2.
    uint8_t plat_hold_timer = 0;
    uint8_t plat_drop_timer = 0;      // frames of suppressed TOP landings (drop-through)
    bool plat_ladder_block_v = false; // must release up/down after grabbing a ladder
    int16_t plat_jump_reduction_vel = 0; // accumulated reduction for stacked air jumps (M13d)
    int16_t plat_jump_vel_per_frame = 0; // hold + run boost applied per JUMP frame (M13d)
    inline int plat_delta_subpx(int v) { return (v >> 8) << 2; }
    inline uint8_t coll_at_px(int px, int py)
    {
        if(!collisions) return 0;
        const int tx = px >> 3, ty = py >> 3;
        if(tx < 0 || ty < 0 || tx >= coll_w || ty >= coll_h) return 0;
        return collisions[ty * coll_w + tx];
    }

    // M13c ladders: grab with UP (ladder tile at the head) or DOWN (below the
    // feet) from GROUND/FALL, snapping x to the ladder column's centre; climb
    // with plat_climb_vel; exit by jumping (A), dropping (DOWN+A), stepping
    // onto standable ground, or pressing left/right after releasing vertical
    // input (GB's plat_ladder_block_v rule).
    inline bool is_ladder(uint8_t t) { return (t & 0x10) != 0; }
    void ladder_check(Actor& p)
    {
        if(plat_ladder_block_v)
        {
            if(!(bn::keypad::up_held() || bn::keypad::down_held()))
                plat_ladder_block_v = false;
            else return;
        }
        if(bn::keypad::a_held() && bn::keypad::down_held()) return; // falling off
        const int cx_px = p.x >> 5;
        if(bn::keypad::up_held())
        {
            const int head_py = (int)(p.y >> 5) - 7;
            if(is_ladder(coll_at_px(cx_px, head_py)))
            {
                p.x = (uint16_t)(((((cx_px) >> 3) << 3) + 4) << 5); // snap to column centre
                plat_state = 4;
                plat_ladder_block_v = true;
            }
        }
        else if(bn::keypad::down_held())
        {
            const int below_py = (int)(p.y >> 5) + 8;
            if(is_ladder(coll_at_px(cx_px, below_py)))
            {
                p.x = (uint16_t)(((((cx_px) >> 3) << 3) + 4) << 5);
                plat_state = 4;
                plat_ladder_block_v = true;
            }
        }
    }
    void ladder_update(Actor& p)
    {
        plat_vel_x = 0;
        plat_vel_y = 0;
        if(bn::keypad::a_pressed())
        {
            if(bn::keypad::down_held()) { plat_state = 0; return; } // drop off
            plat_state = 2;                                          // jump off
            plat_hold_timer = plat_jump_hold_frames;
            plat_vel_y = (int16_t)-plat_jump_vel;
            return;
        }
        const int cx_px = p.x >> 5;
        if(bn::keypad::up_held())
        {
            if(is_ladder(coll_at_px(cx_px, (int)(p.y >> 5) - 7)))
                plat_vel_y = (int16_t)-plat_climb_vel;
            else if(coll_at_px(cx_px, (int)(p.y >> 5) + 8) & 0x01)
            {
                plat_state = 1;                    // topped out onto ground
                return;
            }
        }
        else if(bn::keypad::down_held())
        {
            const uint8_t below = coll_at_px(cx_px, (int)(p.y >> 5) + 8);
            if(is_ladder(below)) plat_vel_y = (int16_t)plat_climb_vel;
            else if(below & 0x01) { plat_state = 1; return; } // stepped off the bottom
        }
        else if(!plat_ladder_block_v &&
                (bn::keypad::left_held() || bn::keypad::right_held()))
        {
            plat_state = 0;                        // let go sideways
            return;
        }
        if(plat_ladder_block_v &&
           !(bn::keypad::up_held() || bn::keypad::down_held()))
            plat_ladder_block_v = false;
        int ny = (int)p.y + plat_delta_subpx(plat_vel_y);
        if(ny < 0) ny = 0;
        p.y = (uint16_t)ny;
        p.moving = plat_vel_y != 0;
    }

    // --- M13f SHMUP scroll controller (state hoisted to the camera block
    // above so hw_load_scene / hw_render can see it) ---------------------
    void shmup_update()
    {
        Actor& p = actors[0];
        if(!p.active) return;
        if(!shmup_inited)
        {
            shmup_inited = true;
            shmup_dir = p.dir;
            shmup_end = false;
            shmup_cx = clamp_cam(to_world_x(p.x), scene_w_px, HALF_W);
            shmup_cy = clamp_cam(to_world_y(p.y), scene_h_px, HALF_H);
        }

        // 8-way free movement (per-axis move_speed + tile collision).
        const uint8_t spd = p.move_speed;
        p.moving = false;
        if(bn::keypad::right_held())     { p.dir = 1; p.moving = true; const uint16_t n = p.x + spd; if(!is_solid_subpx(n, p.y)) p.x = n; }
        else if(bn::keypad::left_held()) { p.dir = 3; p.moving = true; const uint16_t n = p.x - spd; if(!is_solid_subpx(n, p.y)) p.x = n; }
        if(bn::keypad::up_held())        { if(!p.moving) p.dir = 2; p.moving = true; const uint16_t n = p.y - spd; if(!is_solid_subpx(p.x, n)) p.y = n; }
        else if(bn::keypad::down_held()) { if(!p.moving) p.dir = 0; p.moving = true; const uint16_t n = p.y + spd; if(!is_solid_subpx(p.x, n)) p.y = n; }

        // Auto-scroll: advance the camera one step in shmup_dir, drag the
        // player the same amount, and stop at the scene edge (clamp_cam pins
        // the camera -> no change -> reached the end).
        if(!shmup_end)
        {
            const bn::fixed step = bn::fixed(shooter_scroll_speed) / 16; // px/frame
            const int drag = shooter_scroll_speed * 2;                  // GBA subpx (32/px vs GB 16/px)
            bn::fixed tx = shmup_cx, ty = shmup_cy;
            if(shmup_dir == 3)      tx = shmup_cx - step;               // left
            else if(shmup_dir == 1) tx = shmup_cx + step;               // right
            else if(shmup_dir == 2) ty = shmup_cy - step;               // up
            else                    ty = shmup_cy + step;               // down
            const bn::fixed clx = clamp_cam(tx, scene_w_px, HALF_W);
            const bn::fixed cly = clamp_cam(ty, scene_h_px, HALF_H);
            if(clx == shmup_cx && cly == shmup_cy)
            {
                shmup_end = true;
            }
            else
            {
                if(shmup_dir == 3)      p.x = (uint16_t)((int)p.x - drag);
                else if(shmup_dir == 1) p.x = (uint16_t)((int)p.x + drag);
                else if(shmup_dir == 2) p.y = (uint16_t)((int)p.y - drag);
                else                    p.y = (uint16_t)((int)p.y + drag);
                shmup_cx = clx;
                shmup_cy = cly;
            }
        }

        // Lock the player to the visible screen (SHMUP keeps them on-screen).
        const int cam_x_px = shmup_cx.right_shift_integer();
        const int cam_y_px = shmup_cy.right_shift_integer();
        const int minx = (cam_x_px - HALF_W + 8 + scene_w_px / 2) * SUBPX;
        const int maxx = (cam_x_px + HALF_W - 8 + scene_w_px / 2) * SUBPX;
        const int miny = (cam_y_px - HALF_H + 8 + scene_h_px / 2) * SUBPX;
        const int maxy = (cam_y_px + HALF_H - 8 + scene_h_px / 2) * SUBPX;
        if((int)p.x < minx) p.x = (uint16_t)minx; else if((int)p.x > maxx) p.x = (uint16_t)maxx;
        if((int)p.y < miny) p.y = (uint16_t)miny; else if((int)p.y > maxy) p.y = (uint16_t)maxy;

        shmup_cam_x = (int16_t)cam_x_px;
        shmup_cam_y = (int16_t)cam_y_px;
    }

    // GB platform.c core loop, states only (M13b): GROUND/JUMP/FALL with
    // walk acc / turn friction / dec, jump + hold boost, gravity + hold
    // gravity, max fall; land on COLLISION_TOP under the feet, head-bump on
    // BOTTOM. Coyote/buffer/double-jump/run are M13d; one-ways from below and
    // ladders are M13c (a TOP-only tile already behaves one-way here: rising
    // passes it, falling lands).
    void platform_update()
    {
        Actor& p = actors[0];
        if(!p.active) return;
        if(plat_state == 4) { ladder_update(p); return; }  // LADDER (M13c)

        // Horizontal input (handle_horizontal_input parity). B is the run
        // modifier (M13d): running raises the cap + accel. Airborne with air
        // control disabled coasts - velocity unchanged, no input, no decel (GB).
        const bool left = bn::keypad::left_held();
        const bool right = bn::keypad::right_held();
        const bool run = bn::keypad::b_held();
        const int max_vel = run ? plat_run_vel : plat_walk_vel;
        const int acc = run ? plat_run_acc : plat_walk_acc;
        if(plat_state != 1 && plat_air_control == 0)
        {
            // no horizontal control while airborne: coast
        }
        else if(left || right)
        {
            const int dir = right ? 1 : -1;
            int v = plat_vel_x * dir;             // input-aligned velocity
            if(v < 0)
            {
                v += plat_turn_acc;               // reversing: turn friction
                if(v > 0) v = 0;
            }
            else if(v <= max_vel)
            {
                v += acc;
                if(v < plat_min_vel) v = plat_min_vel;
                if(v > max_vel) v = max_vel;
            }
            else v -= plat_dec;                   // above max: settle down
            plat_vel_x = (int16_t)(v * dir);
            p.dir = right ? 1 : 3;
            p.moving = true;
        }
        else if(plat_vel_x != 0)
        {
            const int dec = (plat_state == 1) ? plat_dec : plat_air_dec;
            if(plat_vel_x > 0) { plat_vel_x = (int16_t)(plat_vel_x - dec); if(plat_vel_x < 0) plat_vel_x = 0; }
            else               { plat_vel_x = (int16_t)(plat_vel_x + dec); if(plat_vel_x > 0) plat_vel_x = 0; }
            if(plat_vel_x != 0) p.moving = true;
        }

        // Body sample points: actor pos is the sprite centre in 32/px
        // subpixels; the body spans ~16px, so feet = +8px, head = -8px.
        const int cx_px = p.x >> 5;
        const bool jump_held = bn::keypad::a_held();
        const bool jump_pressed = bn::keypad::a_pressed();
        const uint8_t enter_state = plat_state;   // jumps decided on the entering state

        // state_enter_jump parity: upward velocity, hold-boost setup (+ run
        // boost from horizontal speed), and timers cleared.
        auto begin_jump = [&]() {
            plat_state = 2;
            plat_hold_timer = plat_jump_hold_frames;
            if(-plat_jump_vel < plat_vel_y) plat_vel_y = (int16_t)-plat_jump_vel;
            int jpf = plat_jump_hold_vel;
            if(plat_run_boost != 0 && plat_run_vel != 0)
            {
                const int avx = plat_vel_x < 0 ? -plat_vel_x : plat_vel_x;
                jpf += (avx / (plat_run_vel >> 7)) * (plat_run_boost >> 7);
            }
            plat_jump_vel_per_frame = (int16_t)jpf;
            plat_coyote_timer = 0;
            plat_jump_buffer_timer = 0;
        };

        if(enter_state == 1)                      // GROUND
        {
            // enter_ground parity: refresh jump grants every grounded frame.
            plat_coyote_timer = plat_coyote_frames;
            plat_extra_jumps_counter = plat_extra_jumps;
            plat_jump_reduction_vel = 0;
            if((jump_pressed || plat_jump_buffer_timer != 0) && plat_drop_timer == 0)
            {
                begin_jump();                     // standard / buffered jump
            }
            else
            {
                plat_jump_buffer_timer = 0;
                if(!(coll_at_px(cx_px, (int)(p.y >> 5) + 8) & 0x01))
                {
                    plat_state = 0;               // walked off a ledge -> FALL
                }
                else if(plat_drop_through_active && bn::keypad::down_held())
                {
                    // Drop-through (M13c): only one-way tiles (TOP, no other
                    // solid faces).
                    const uint8_t footing = coll_at_px(cx_px, (int)(p.y >> 5) + 8);
                    if((footing & 0x0f) == 0x01)
                    {
                        plat_state = 0;
                        plat_drop_timer = 6;      // suppress TOP landings briefly
                    }
                }
            }
        }
        else if(enter_state == 0 || enter_state == 2) // airborne: FALL / JUMP
        {
            // M13d: coyote time, double jump, jump buffering.
            if(plat_coyote_timer != 0) plat_coyote_timer--;
            if(plat_jump_buffer_timer != 0) plat_jump_buffer_timer--;
            if(jump_pressed)
            {
                if(plat_coyote_timer != 0)
                {
                    begin_jump();                 // coyote-time ground jump
                }
                else if(plat_extra_jumps_counter != 0)
                {
                    plat_extra_jumps_counter--;   // double / extra jump
                    plat_jump_reduction_vel += plat_jump_reduction;
                    begin_jump();
                }
                else
                {
                    plat_jump_buffer_timer = plat_jump_buffer_frames;
                }
            }
        }
        if(plat_state <= 1) ladder_check(p);      // grab from GROUND/FALL (M13c)
        if(plat_state == 4) return;               // grabbed: climb next frame
        if(plat_state == 2)                       // JUMP: hold boost
        {
            if(plat_hold_timer != 0 && jump_held)
            {
                // per-frame boost, then the accumulated double-jump reduction.
                int vy = plat_vel_y - plat_jump_vel_per_frame + plat_jump_reduction_vel;
                if(vy > 0) vy = 0;
                plat_vel_y = (int16_t)vy;
                plat_hold_timer--;
            }
            else plat_hold_timer = 0;
            if(plat_vel_y >= 0) plat_state = 0;   // apex -> FALL
        }
        if(plat_state != 1)                       // airborne gravity
        {
            const int g = (jump_held && plat_vel_y < 0) ? plat_hold_grav : plat_grav;
            int vy = plat_vel_y + g;
            if(vy > plat_max_fall_vel) vy = plat_max_fall_vel;
            plat_vel_y = (int16_t)vy;
        }

        // Integrate + collide. Horizontal: blocked by the facing edge bit
        // (moving right hits the target tile's LEFT face 0x04; moving left
        // its RIGHT face 0x08), sampled at body centre and near the feet.
        int nx = (int)p.x + plat_delta_subpx(plat_vel_x);
        if(plat_vel_x != 0)
        {
            const int edge_px = (nx >> 5) + (plat_vel_x > 0 ? 8 : -8);
            const uint8_t face = plat_vel_x > 0 ? 0x04 : 0x08;
            const int body_py = (int)(p.y >> 5);
            if((coll_at_px(edge_px, body_py) & face) ||
               (coll_at_px(edge_px, body_py + 7) & face))
            {
                nx = (int)p.x;
                plat_vel_x = 0;
            }
        }
        if(nx < 0) nx = 0;
        p.x = (uint16_t)nx;

        int ny = (int)p.y + plat_delta_subpx(plat_vel_y);
        if(plat_drop_timer != 0) plat_drop_timer--;
        if(plat_vel_y > 0 && plat_drop_timer == 0) // falling: land on TOP faces
        {
            // Tunnel guard (M13c): terminal velocity moves ~10px/frame, more
            // than a tile - sweep every row the feet cross, land on the first
            // TOP face instead of only testing the destination row.
            const int feet_from = ((int)(p.y >> 5) + 8) >> 3;
            const int feet_to = ((ny >> 5) + 8) >> 3;
            for(int ty = feet_from; ty <= feet_to; ++ty)
            {
                if(coll_at_px(cx_px, ty << 3) & 0x01)
                {
                    ny = ((ty << 3) - 8) << 5;    // snap feet to the tile top
                    plat_vel_y = 0;
                    plat_state = 1;               // land -> GROUND
                    break;
                }
            }
        }
        else if(plat_vel_y < 0)                   // rising: bump on BOTTOM faces
        {
            const int head_px = (ny >> 5) - 8;
            if(coll_at_px(cx_px, head_px) & 0x02)
            {
                ny = (int)p.y;
                plat_vel_y = 0;
            }
        }
        if(ny < 0) { ny = 0; if(plat_vel_y < 0) plat_vel_y = 0; }
        p.y = (uint16_t)ny;
    }
}

void hw_set_collisions(const unsigned char* grid, int width_tiles, int height_tiles)
{
    collisions = grid;
    coll_w = width_tiles;
    coll_h = height_tiles;
}

// M6b: 1 if the player (actor 0) currently stands within the given tile rect (used to
// fire trigger zones). The player's centre tile = position subpixels / SUBPX / 8.
int hw_player_in_rect(int tx, int ty, int w, int h)
{
    const Actor& p = actors[0];
    if(!p.active) return 0;
    const int px = (int)p.x / SUBPX / 8, py = (int)p.y / SUBPX / 8;
    return (px >= tx && px < tx + w && py >= ty && py < ty + h) ? 1 : 0;
}

// M6c: when the player presses A and faces an adjacent placed actor (no dialogue up),
// return that actor's runtime index so its interact script can run; else -1. The "front"
// tile is the player's tile stepped one unit in their facing direction. text_showing
// gates it so A dismisses an open dialogue instead of re-triggering the same NPC.
int hw_interact_actor(void)
{
    if(text_showing) return -1;
    if(!bn::keypad::a_pressed()) return -1;
    const Actor& p = actors[0];
    if(!p.active) return -1;
    int fx = (int)p.x / SUBPX / 8, fy = (int)p.y / SUBPX / 8;
    switch(p.dir) { case 0: ++fy; break; case 1: ++fx; break; case 2: --fy; break; default: --fx; break; }
    for(int i = 1; i < MAX_ACTORS; ++i)
    {
        const Actor& a = actors[i];
        if(a.active && (int)a.x / SUBPX / 8 == fx && (int)a.y / SUBPX / 8 == fy) return i;
    }
    return -1;
}

// M6c: 1 while a dialogue box is on screen. The main loop samples this at the start of a
// frame so the A press that dismisses a dialogue isn't also read as a fresh interaction
// (script_runner_update clears text_showing mid-frame, before gba_check_interact runs).
int hw_dialogue_active(void) { return text_showing ? 1 : 0; }

void hw_player_update(void)
{
    // Built-in top-down control: move the player (actor 0) from the live d-pad.
    // Horizontal + vertical can combine (8-way); facing prefers the horizontal axis.
    if(!player_move_enabled) return;
    // M13a/b dispatch: PLATFORM runs its own physics; SHMUP/ADVENTURE (M13f)
    // still use the top-down controller below.
    if(gba_current_scene_type == 1) { platform_update(); return; }
    if(gba_current_scene_type == 3) { shmup_update(); return; }   // M13f SHMUP
    Actor& p = actors[0];
    if(!p.active) return;
    // Face + animate toward the held direction, but only advance into open tiles.
    const uint8_t spd = p.move_speed;
    if(bn::keypad::right_held())     { p.dir = 1; p.moving = true; const uint16_t n = p.x + spd; if(!is_solid_subpx(n, p.y)) p.x = n; }
    else if(bn::keypad::left_held()) { p.dir = 3; p.moving = true; const uint16_t n = p.x - spd; if(!is_solid_subpx(n, p.y)) p.x = n; }
    if(bn::keypad::up_held())        { if(!p.moving) p.dir = 2; p.moving = true; const uint16_t n = p.y - spd; if(!is_solid_subpx(p.x, n)) p.y = n; }
    else if(bn::keypad::down_held()) { if(!p.moving) p.dir = 0; p.moving = true; const uint16_t n = p.y + spd; if(!is_solid_subpx(p.x, n)) p.y = n; }
}

// M8d VM_SET_BG_TRANSFORM: rotate + scale the affine scene background.
// angle is whole degrees; scale is fixed-point x256 (256 = 1.0). No-op on a
// regular (non-affine) scene. Exports the angle for GDB/runtime tests.
void hw_bg_transform(int angle, int scale)
{
    bg_rot = bn::fixed(((angle % 360) + 360) % 360);
    bg_scale = scale > 0 ? bn::fixed(scale) / 256 : bn::fixed(1);
    gba_bg_angle = (int16_t)bg_rot.right_shift_integer();
    gba_bg_scale = (int16_t)(scale > 0 ? scale : 256);
    if(scene_affine_bg)
    {
        scene_affine_bg->set_rotation_angle(bg_rot);
        scene_affine_bg->set_scale(bg_scale);
    }
}

// M8d VM_SET_BG_SPIN: set the affine scene bg's auto-spin velocity (deg/frame,
// x256 fixed; signed - negative spins the other way, 0 stops). hw_render applies
// it each frame. No visible effect on a regular (non-affine) scene.
void hw_bg_spin(int speed)
{
    gba_bg_spin = (int16_t)speed;
}

// M8d VM_SET_BG_ANGLE_VAR: set the affine scene bg's rotation angle (whole
// degrees) from a script variable's value, leaving the scale untouched. Lets a
// script drive Mode-7 rotation from any computed value (e.g. steering). No-op
// visible effect on a regular (non-affine) scene.
void hw_bg_set_angle(int angle)
{
    bg_rot = bn::fixed(((angle % 360) + 360) % 360);
    gba_bg_angle = (int16_t)bg_rot.right_shift_integer();
    if(scene_affine_bg)
    {
        scene_affine_bg->set_rotation_angle(bg_rot);
    }
}

// M8d VM_SET_BG_SCALE_VAR: set the affine scene bg's scale from a script
// variable's value, a PERCENTAGE (100 = original size), leaving the rotation
// untouched. Lets a script drive Mode-7 zoom from any computed value. No visible
// effect on a regular (non-affine) scene.
void hw_bg_set_scale(int scale_percent)
{
    const int scale256 = scale_percent > 0 ? scale_percent * 256 / 100 : 256;
    bg_scale = bn::fixed(scale256) / 256;
    gba_bg_scale = (int16_t)scale256;
    if(scene_affine_bg)
    {
        scene_affine_bg->set_scale(bg_scale);
    }
}

void hw_overlay_move_to(int x, int y, int speed)
{
    // Non-blocking: set the box target; hw_overlay_update slides it there. The box
    // X (GB window tiles) pairs with VM_OVERLAY_CLEAR's width (M11c).
    overlay_init();
    box_move_x = x;
    overlay_span();
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

void hw_overlay_show(int x, int y, int color, int options)
{
    // Show the box at (x, y) immediately. GB clears the whole visible window area
    // ((20-x) x (18-y)) in COLOR with no frame unless requested - the compiler's
    // "Show Overlay" event always passes options 0 (black = full-screen cover).
    overlay_init();
    box_move_x = x;
    box_clear_w = 20 - x;
    overlay_span();
    box_top_target = overlay_top_for_row(y);
    box_top = box_top_target;
    box_frame = (options & 0x01) != 0;           // .UI_DRAW_FRAME
    panel_set_black(color == 0);                 // .UI_COLOR_BLACK
}

// M11c VM_OVERLAY_CLEAR: latch the box width (GB window tiles). GB paints a w x h
// rect; on GBA the panel is a clipped bg, so only the geometry matters - the X
// comes from the paired VM_OVERLAY_MOVE_TO, the height from the text sizing.
// M11d: COLOR picks the panel fill and options bit 0 (.UI_DRAW_FRAME) the border.
void hw_overlay_clear(int x, int y, int w, int h, int color, int options)
{
    (void)x; (void)y; (void)h;
    overlay_init();
    box_clear_w = (w > 0) ? w : 20;
    overlay_span();
    box_frame = (options & 0x01) != 0;           // .UI_DRAW_FRAME
    panel_set_black(color == 0);                 // .UI_COLOR_BLACK
}

// M12c VM_LOAD_PALETTE: recolor the scene bg's GBC palette banks in place. The
// eject lays bank b's four colours at palette indices b*16+1..4 (M12a) and pads
// the palette item to 128 entries so every bank is addressable at runtime; a
// non-banked (mono) scene bg reports fewer colours and the op no-ops. Sprite
// palettes (.PALETTE_SPRITE) are M12d - rows are still consumed by the VM either
// way, so the stream stays in sync.
void hw_load_palette(int mask, int options, const unsigned char* rows)
{
    int row = 0;
    for(int b = 0; b < 8; ++b)
    {
        if(!(mask & (1 << b))) continue;
        const unsigned char* p = rows + row * 8;
        ++row;
        bn::color colors[4];
        for(int c = 0; c < 4; ++c)
        {
            const int v = p[c * 2] | (p[c * 2 + 1] << 8);
            colors[c] = bn::color(v & 31, (v >> 5) & 31, (v >> 10) & 31);
        }
        if((options & 0x02) && scene_bg)     // .PALETTE_BKG
        {
            bn::bg_palette_ptr pal = scene_bg->palette();
            if(pal.colors_count() >= 128)    // banked (colour) background only
                for(int c = 0; c < 4; ++c)
                    pal.set_color(b * 16 + 1 + c, colors[c]);
            // M12d2: bank 7 is GB's UI palette (Set UI Palette emits mask 128).
            // The GBA dialogue style is inverted (light text on a dark panel),
            // so the panel takes the palette's DARKEST colour. Latched so a
            // pre-dialogue event survives the panel's lazy creation.
            if(b == 7)
            {
                panel_color = colors[3];
                panel_color_overridden = true;
                if(overlay_inited && !panel_black_now) panel_set_black(false);
            }
        }
        if(options & 0x04)                   // .PALETTE_SPRITE (M12d)
        {
            for(int c = 0; c < 4; ++c) sprite_pal_latch[b][c] = colors[c];
            sprite_pal_set[b] = true;
            // Recolor every live actor whose sheet was baked with this GBC OBJ
            // slot (eject stamps GbaActorSprite.pal_slot). Colours 1..3 only -
            // index 0 is transparent. Shared bn palettes mean identical sheets
            // recolor together, which matches GBC slot semantics.
            for(int i = 0; i < MAX_ACTORS; ++i)
            {
                Actor& a = actors[i];
                if(!a.active || !a.sprite) continue;
                const GbaActorSprite* def = (a.sprite_sheet >= 0)
                    ? gba_global_sprite(a.sprite_sheet)
                    : gba_actor_sprite(current_scene, i);
                if(!def || def->pal_slot != b) continue;
                bn::sprite_palette_ptr sp = a.sprite->palette();
                for(int c = 1; c < 4; ++c) sp.set_color(c, colors[c]);
            }
            // GB's Set Emote Palette rewrites OBJ slot 7 (M12e).
            if(b == 7 && emote_sprite)
            {
                bn::sprite_palette_ptr sp = emote_sprite->palette();
                for(int c = 1; c < 4; ++c) sp.set_color(c, colors[c]);
            }
        }
    }
}

void hw_overlay_hide(void)
{
    choice_active = false; // a vanishing box takes any open menu with it (M11a)
    choice_cursor.clear();
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
    // GB Studio music ALWAYS loops until stopped: its compiler never sets the VM
    // loop operand (eventMusicPlay -> musicPlay(musicId) with loop defaulting to
    // false -> .MUSIC_NO_LOOP in every script), because the GB-side drivers
    // (hUGEDriver/gbt) loop unconditionally and ignore it. Honoring the operand
    // here made every song play once and stop ~30s in (found 2026-07-02 while
    // chasing a phantom Wonderful-Toolchain audio bug). Ignore it like GB does.
    (void)loop;

    if(gba_music_backend(track) == 1) // Maxmod (DirectSound) tracker music - GBA-native
    {
        // Playing a song replaces whatever is playing (GB semantics: one music
        // track at a time). play() below replaces same-backend music, but a
        // track on the OTHER backend would keep playing underneath - stop it.
        if(bn::dmg_music::playing()) bn::dmg_music::stop();

        const bn::music_item* item = gba_maxmod_music_track(track);
        if(item) item->play(bn::fixed(1), true);
    }
    else // DMG (gbt-player) chiptune - the 4 Game Boy PSG channels
    {
        if(bn::music::playing()) bn::music::stop();

        const bn::dmg_music_item* item = gba_dmg_music_track(track);
        // Speed 6 is gbt-player's default for MOD songs (GB Studio music is MOD-based).
        // Butano's play() would force speed 1, which is 6x too fast for a MOD that doesn't
        // self-specify its tempo; the song's own baked-in speed effects still override this.
        if(item) item->play(6, true);
    }
}

void hw_music_stop(void)
{
    if(bn::music::playing()) bn::music::stop();          // Maxmod (DirectSound) track
    if(bn::dmg_music::playing()) bn::dmg_music::stop();   // DMG (gbt-player) track
}

// VM_SFX_PLAY (M5b): play the resolved .wav sound on Butano's DirectSound mixer (Maxmod),
// separate from the DMG music channels, so SFX and music coexist.
void hw_sfx_play(int sfx)
{
    const bn::sound_item* s = gba_sfx(sfx);
    if(s) s->play();
}

// VM_SOUND_MASTERVOL (M5c): GB Studio's master volume. The GB scale tops out at ~8
// (a packed NR50 reads higher, so clamp); apply it to both the DMG music (fine volume,
// over the coarse master set in hw_init) and the DirectSound SFX mixer.
void hw_sound_mastervol(int vol)
{
    const bn::fixed v = bn::min(bn::fixed(vol) / 8, bn::fixed(1));
    bn::dmg_music::set_volume(v);   // DMG (gbt-player) music
    bn::music::set_volume(v);       // Maxmod (DirectSound) music
    bn::sound::set_master_volume(v); // DirectSound SFX mixer
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
        bn::rect_window::internal().set_boundaries(box_top, box_left, SCREEN_BOTTOM, box_right);
    // The frame line's 2px stripe sits at the top of the 32px sprite, so centre it
    // box_top + 16 to cap the panel's top edge; the sprites track the slide.
    for(int i = 0; i < frame_sprites.size(); ++i)
    {
        bn::sprite_ptr& s = frame_sprites[i];
        // Show only the segments overlapping the box span (M11c) - a right-side
        // menu box keeps its top line roughly capped, not full-width.
        const int cx = FRAME_SEG_X[i];
        const bool seg_visible = visible && box_frame &&
                                 (cx + 32 > box_left) && (cx - 32 < box_right);
        s.set_visible(seg_visible);
        if(seg_visible) s.set_y(box_top + 16);
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
            text_lines = count_text_lines(text_buf, text_len); // goto-aware (M11b)
            text_rendered = -1;
            box_top_target = SCREEN_BOTTOM - (text_lines * TEXT_LINE_H + TEXT_TOP_PAD + TEXT_BOTTOM_MARGIN);
        }
        else
        {
            // Fresh: latch the text (substituting %d/%D/%c/%%), (re)create the avatar
            // portrait, and size the box. Speed/font codes are copied verbatim.
            int lines = 1;
            text_len = subst_text(text, values, n_values, 0, &lines);
            text_lines = count_text_lines(text_buf, text_len); // goto-aware (M11b)
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
                text_x = box_left + 8 + AVATAR_TEXT_SHIFT; // 1-tile pad from the box edge (M11c)
            }
            else
            {
                avatar_sprite.reset();
                text_x = box_left + 8; // 1-tile pad from the box edge (M11c)
            }
            // Size the box to fit this text (TEXT_LINE_H pitch keeps it tall enough for
            // the avatar); the script's overlay slide still controls show/hide.
            overlay_init();
            box_top_target = SCREEN_BOTTOM - (text_lines * TEXT_LINE_H + TEXT_TOP_PAD + TEXT_BOTTOM_MARGIN);
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
        const int line0_y =
            SCREEN_BOTTOM - text_lines * TEXT_LINE_H - TEXT_BOTTOM_MARGIN + TEXT_LINE_OFFSET;
        int y = line0_y;
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
            else if(ch == 0x03) // goto x,y (M11a/M11b): jump to tile column x, row y
            {
                run[r] = '\0';
                if(r > 0) x += render_text_run(font_idx, x, y, run);
                r = 0;
                const int px = shown[i + 1] ? (uint8_t)shown[i + 1] : 1;
                const int py = shown[i + 2] ? (uint8_t)shown[i + 2] : 2;
                x = text_x + (px - 1) * 8;
                const int line = py - 2 < 0 ? 0 : py - 2; // window row 2 = line 0
                y = line0_y + line * TEXT_LINE_H;
                i += 2; // skip both param bytes
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
    a.anim_state = 0; // scene placement resets to the default animation state (M10c)
    a.anim_noloop = false;
    a.coll_enabled = true;
}

// Move one axis toward the destination by the actor's speed, snapping when within
// range. `cross` is the other axis' position (for the collision check). Returns true
// once that axis reaches its target OR a solid tile blocks it (the move stops there).
static bool move_axis(uint16_t& pos, uint16_t dest, uint16_t cross, bool axis_x, uint8_t speed)
{
    const int d = int(dest) - int(pos);
    if(d == 0) return true;
    const uint16_t step = (d > 0) ? ((d <= speed) ? dest : (uint16_t)(pos + speed))
                                  : ((-d <= speed) ? dest : (uint16_t)(pos - speed));
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
    if(axis == 0 || axis == 2) done &= move_axis(a.x, a.dest_x, a.y, true, a.move_speed);
    if(axis == 1 || axis == 2) done &= move_axis(a.y, a.dest_y, a.x, false, a.move_speed);
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

// M10a: per-actor movement speed (subpixels/frame; GB Studio speed 1 = 32).
void hw_actor_set_move_speed(int16_t id, uint8_t speed)
{
    if(id < 0 || id >= MAX_ACTORS) return;
    actors[id].move_speed = speed ? speed : 1;
}

// M10d: show an emote bubble above the actor's head for ~1s (VM_ACTOR_EMOTE).
// One bubble at a time (a new emote replaces the current one, like GB).
void hw_actor_emote(int16_t id, uint8_t emote)
{
    if(id < 0 || id >= MAX_ACTORS || !actors[id].active) return;
    const bn::sprite_item* item = gba_emote_sprite(emote);
    if(!item) return;
    emote_sprite = item->create_sprite(0, 0);
    if(camera) emote_sprite->set_camera(*camera);
    apply_sprite_latch(*emote_sprite, 7); // GB emotes ride OBJ slot 7 (M12e)
    emote_sprite->set_bg_priority(2); // in front of the scene bg (3)
    emote_timer = EMOTE_FRAMES;
    emote_actor = id;
}

// M10e: apply GB Studio actor flags (VM_ACTOR_SET_FLAGS). `mask` selects which
// bits change; `flags` gives their new values. Supported: HIDDEN (0x02),
// ANIM_NOLOOP (0x04), COLLISION (0x08). PINNED/PERSISTENT are ignored (no
// pinned rendering; persistence is engine-global).
void hw_actor_set_flags(int16_t id, uint8_t flags, uint8_t mask)
{
    if(id < 0 || id >= MAX_ACTORS) return;
    Actor& a = actors[id];
    if(mask & 0x02) a.visible = !(flags & 0x02);
    if(mask & 0x04)
    {
        const bool noloop = (flags & 0x04) != 0;
        if(a.anim_noloop != noloop) { a.anim_noloop = noloop; a.anim_timer = 0; }
    }
    if(mask & 0x08) a.coll_enabled = (flags & 0x08) != 0;
}

// M10e: enable/disable the actor as a collision blocker (VM_ACTOR_SET_COLL_ENABLED).
void hw_actor_set_coll_enabled(int16_t id, uint8_t enabled)
{
    if(id < 0 || id >= MAX_ACTORS) return;
    actors[id].coll_enabled = (enabled != 0);
}

// M10f: the actor's GB collision group bit, set from GbaActorInit on scene load
// (player 0x01). Projectiles hit an actor when its group is in their mask.
void hw_actor_set_collision_group(int16_t id, uint8_t group)
{
    if(id < 0 || id >= MAX_ACTORS) return;
    actors[id].collision_group = group;
}

// M10h: swap the actor's spritesheet (VM_ACTOR_SET_SPRITESHEET). The sprite is
// recreated from the new item on the next render; the animation restarts on the
// current facing/state (rows come from the new sheet's tables).
void hw_actor_set_spritesheet(int16_t id, uint8_t sheet)
{
    if(id < 0 || id >= MAX_ACTORS) return;
    Actor& a = actors[id];
    if(a.sprite_sheet == (int)sheet) return;
    a.sprite_sheet = (int)sheet;
    a.sprite.reset();
    a.anim_timer = 0;
}

// M10g: the script fired when a projectile hits this actor (0 = none). Set from
// GbaActorInit on scene load; the scene's player-hit script for actor 0.
void hw_actor_set_hit_script(int16_t id, unsigned char* script)
{
    if(id < 0 || id >= MAX_ACTORS) return;
    actors[id].hit_script = script;
    actors[id].hit_handle = SCRIPT_TERMINATED;
}

// M10c: switch the actor's animation state (VM_ACTOR_SET_ANIM_SET). The operand
// is the project-global state index (STATE_* in game_globals.i); render clamps
// unknown states to the default row, so sprites without that state just keep
// their normal animations.
void hw_actor_set_anim_state(int16_t id, uint8_t state)
{
    if(id < 0 || id >= MAX_ACTORS) return;
    Actor& a = actors[id];
    if(a.anim_state != state) { a.anim_state = state; a.anim_timer = 0; }
}

// M10a: hide/show an actor's sprite (Actor Show / Actor Hide events). A hidden
// actor keeps updating (scripts, movement) - only rendering is suppressed.
void hw_actor_set_hidden(int16_t id, uint8_t hidden)
{
    if(id < 0 || id >= MAX_ACTORS) return;
    actors[id].visible = !hidden;
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

// M6d: read an actor's runtime state. gba_load_scene uses these to carry the player across
// a scene change - a Switch Scene event positions/faces actor 0 just before the change, and
// the load must keep that instead of snapping the player back to the new scene's start.
int hw_actor_active(int16_t id) { return (id >= 0 && id < MAX_ACTORS && actors[id].active) ? 1 : 0; }
uint8_t hw_actor_dir(int16_t id) { return (id >= 0 && id < MAX_ACTORS) ? actors[id].dir : 0; }

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
    // M8a: GBA-exclusive L/R shoulder buttons in the high byte (bits 8/9). GB
    // has no L/R, so these are only referenced by GBA-gated editor input; the
    // dst is a uint16 mask so they survive the read.
    if(bn::keypad::l_held())      m |= 0x100;
    if(bn::keypad::r_held())      m |= 0x200;
    *dst = m;
}

// --- projectiles (M10f) -------------------------------------------------------

// Copy a def into a runtime slot. Scene defs load on scene load (gba_load_scene,
// GB data_manager parity); VM_PROJECTILE_LOAD_TYPE loads from the global tables.
void hw_projectile_load_def(uint8_t slot, const GbaProjectileDef* def)
{
    if(slot >= MAX_PROJECTILE_DEFS || !def) return;
    projectile_defs[slot] = *def;
}

// 0x81 VM_PROJECTILE_LOAD_TYPE: `index` = the source table's base index in the
// flattened gba_global_projectile_defs[] plus the in-table def index.
void hw_projectile_load_global(uint8_t slot, uint8_t index)
{
    if(index >= gba_global_projectile_defs_count) return;
    hw_projectile_load_def(slot, &gba_global_projectile_defs[index]);
}

// 0x80 VM_PROJECTILE_LAUNCH: spawn an instance of def slot `slot` at (x, y)
// subpixels, travelling at 8-bit BRADS `angle` (0 up, 64 right, 128 down,
// 192 left). Replicates GB Studio's projectile_launch: pick the direction
// animation from the angle quadrant, offset the spawn point by initial_offset
// along the angle (axis-aligned fast paths for the four exact directions),
// and set the per-frame delta from the sine table.
void hw_projectile_launch(uint8_t slot, uint16_t x, uint16_t y, uint8_t angle)
{
    if(slot >= MAX_PROJECTILE_DEFS) return;
    Projectile* p = nullptr;
    for(int i = 0; i < MAX_PROJECTILES; ++i)
    {
        if(!projectiles[i].active) { p = &projectiles[i]; break; }
    }
    if(!p) return; // pool exhausted, like GB (silently no-op)

    p->def = projectile_defs[slot];

    // Angle quadrant -> direction animation (GB's thresholds; our dir encoding
    // 0 down, 1 right, 2 up, 3 left).
    uint8_t dir = 2; // up
    if(angle <= 224)
    {
        if(angle >= 160)     dir = 3; // left
        else if(angle > 96)  dir = 0; // down
        else if(angle >= 32) dir = 1; // right
    }
    const GbaActorSprite* sdef = gba_global_sprite(p->def.sprite);
    if(sdef && sdef->anim_start)
    {
        const int st = (p->def.anim_state < GBA_ANIM_STATES) ? p->def.anim_state : 0;
        const int anim = st * 8 + dir; // the state row's idle set (M10c layout)
        p->frame_start = sdef->anim_start[anim];
        p->frame_len = sdef->anim_len[anim] ? sdef->anim_len[anim] : 1;
    }
    else
    {
        p->frame_start = 0;
        p->frame_len = 1;
    }
    p->frame = p->frame_start;
    p->tick = 0;

    p->x = int(x);
    p->y = int(y);
    const int offset = p->def.initial_offset;
    const int speed = p->def.move_speed;
    if(angle == 192)      { p->x -= offset; p->dx = -speed; p->dy = 0; } // left
    else if(angle == 64)  { p->x += offset; p->dx =  speed; p->dy = 0; } // right
    else if(angle == 0)   { p->y -= offset; p->dx = 0; p->dy = -speed; } // up
    else if(angle == 128) { p->y += offset; p->dx = 0; p->dy =  speed; } // down
    else
    {
        const int sinv = vm_sine(angle);
        const int cosv = vm_sine((uint8_t)(angle + 64u));
        p->x += (sinv * offset) >> 7;
        p->y -= (cosv * offset) >> 7;
        p->dx = (sinv * speed) >> 7;
        p->dy = -((cosv * speed) >> 7);
    }

    p->life = p->def.life_time > 0 ? p->def.life_time : 1;
    p->active = true;
    p->sprite.reset(); // created lazily on the next render
}

// --- choice / menu (M11a) -----------------------------------------------------

// 0x48 VM_CHOICE: one frame of menu interaction. The menu text is already on
// screen (a normal dialogue display precedes the op); this owns the cursor and
// the item-graph navigation. GB parity: iL/iR/iU/iD are 1-based item numbers
// (0 = the cursor stays); A selects (1-based result; the LAST item returns 0
// under .UI_MENU_LAST_0), B returns 0 under .UI_MENU_CANCEL_B.
int hw_choice_step(uint8_t options, uint8_t count, const unsigned char* items, int start)
{
    if(count == 0) return 0;
    if(!choice_active)
    {
        choice_active = true;
        choice_index = 0;
        if(options & 0x04) // .UI_MENU_SET_START: the result var holds the initial item
        {
            const int s0 = start - 1; // 1-based in the variable
            if(s0 >= 0 && s0 < (int)count) choice_index = s0;
        }
        choice_place_cursor(items + choice_index * 6);
        return -1; // swallow the first frame so the A that opened the menu can't select
    }

    const unsigned char* it = items + choice_index * 6;
    int next = choice_index;
    if(bn::keypad::left_pressed()  && it[2]) next = it[2] - 1;
    else if(bn::keypad::right_pressed() && it[3]) next = it[3] - 1;
    else if(bn::keypad::up_pressed()    && it[4]) next = it[4] - 1;
    else if(bn::keypad::down_pressed()  && it[5]) next = it[5] - 1;
    if(next != choice_index && next >= 0 && next < (int)count)
    {
        choice_index = next;
        choice_place_cursor(items + choice_index * 6);
    }

    if(bn::keypad::a_pressed())
    {
        const bool last_zero = (options & 0x01) && (choice_index == (int)count - 1);
        choice_active = false;
        choice_cursor.clear();
        return last_zero ? 0 : choice_index + 1;
    }
    if((options & 0x02) && bn::keypad::b_pressed())
    {
        choice_active = false;
        choice_cursor.clear();
        return 0;
    }
    return -1;
}
