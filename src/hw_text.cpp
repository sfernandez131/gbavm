// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Dialogue/text renderer (P3). Owns all Butano text objects (mirroring how hw.cpp
// owns scene bg + actor sprites). Glyphs are drawn as sprites by a
// bn::sprite_text_generator built from the converted fonts (font_table.cpp). The
// generator is camera-less, so the dialogue is pinned to the screen (it ignores
// the scene camera). A frame-paced typewriter reveals the page; A/B fast-forwards;
// VM_OVERLAY_WAIT parks the script thread until the page is drawn and A is pressed.
//
// P3 v1 renders the text only (no framed box yet — that needs a box bg asset and
// is a follow-up); the text shows in a fixed bottom region and types out.

#include "hw_text.h"
#include "font_table.h"

#include "bn_keypad.h"
#include "bn_optional.h"
#include "bn_sprite_ptr.h"
#include "bn_sprite_text_generator.h"
#include "bn_string_view.h"
#include "bn_vector.h"

namespace
{
    constexpr int MAX_GLYPHS = 64;     // OAM budget for one dialogue page
    constexpr int LINE_MAX = 40;       // chars per rendered line
    constexpr bn::fixed TEXT_X = -110; // screen-space origin (0,0 = screen centre)
    constexpr bn::fixed TEXT_Y = 36;   // lower third
    constexpr unsigned SPEED_MASK = 1; // reveal a char every (MASK+1) frames

    bn::optional<bn::sprite_text_generator> generator;
    int cur_font = -1;

    uint8_t page[256];
    int page_len = 0;
    int revealed = 0;
    bool active = false; // a page is loaded + being shown
    bool shown = false;  // dialogue region is on screen
    bool drawn = false;  // typewriter finished
    unsigned frame = 0;

    bn::vector<bn::sprite_ptr, MAX_GLYPHS> glyphs;

    void ensure_generator(int idx)
    {
        if(gba_font_count == 0) return; // no fonts in this project
        if(idx < 0 || (unsigned)idx >= gba_font_count) idx = (int)gba_default_font_index;
        if(idx == cur_font && generator) return;
        if(!gba_fonts[idx]) return;
        generator = bn::sprite_text_generator(*gba_fonts[idx]);
        generator->set_bg_priority(0);   // in front of the scene background
        generator->set_z_order(0);
        cur_font = idx;
    }

    // Re-render the revealed substring. Control codes are skipped (with their
    // inline args); 0x0A/0x0D break lines; printable ASCII is drawn.
    void render()
    {
        glyphs.clear();
        if(!generator || revealed <= 0) return;
        bn::fixed y = TEXT_Y;
        char line[LINE_MAX];
        int ln = 0;
        const int limit = revealed < page_len ? revealed : page_len;
        for(int i = 0; i < limit; ++i)
        {
            const uint8_t c = page[i];
            if(c == 0x0A || c == 0x0D) // newline / scroll
            {
                if(ln > 0 && glyphs.size() < MAX_GLYPHS - LINE_MAX)
                    generator->generate(TEXT_X, y, bn::string_view(line, ln), glyphs);
                y += 8;
                ln = 0;
                continue;
            }
            if(c == 0x01 || c == 0x02 || c == 0x06) { ++i; continue; } // 1-arg code
            if(c == 0x03 || c == 0x04) { i += 2; continue; }           // 2-arg gotoxy
            if(c < 0x20 || c > 0x7e) continue;                          // other non-printable
            if(ln < LINE_MAX) line[ln++] = (char)c;
        }
        if(ln > 0 && glyphs.size() < MAX_GLYPHS - LINE_MAX)
            generator->generate(TEXT_X, y, bn::string_view(line, ln), glyphs);
    }
}

void hw_text_load(const uint8_t* str, int len)
{
    if(len > (int)sizeof(page) - 1) len = (int)sizeof(page) - 1;
    for(int i = 0; i < len; ++i) page[i] = str[i];
    page_len = len;
    revealed = 0;
    drawn = false;
}

void hw_text_display(uint8_t options, uint8_t start_tile)
{
    (void)options;
    (void)start_tile;
    ensure_generator(cur_font < 0 ? (int)gba_default_font_index : cur_font);
    active = true;
    shown = true;
    revealed = 0;
    drawn = (page_len == 0);
    glyphs.clear();
}

void hw_text_set_font(int index) { ensure_generator(index); }

void hw_text_overlay_move_to(uint8_t x, uint8_t y, int8_t speed)
{
    (void)x;
    (void)speed;
    // MENU_CLOSED_Y (0x12 = 18) means the overlay is moved offscreen = hide.
    if(y >= 18)
    {
        active = false;
        shown = false;
        drawn = false;
        revealed = 0;
        glyphs.clear();
    }
    else
    {
        shown = true;
    }
}

void hw_text_overlay_setpos(uint8_t x, uint8_t y) { hw_text_overlay_move_to(x, y, 0); }

void hw_text_overlay_show(uint8_t x, uint8_t y, uint8_t color, uint8_t options)
{
    (void)x; (void)y; (void)color; (void)options;
    shown = true;
}

void hw_text_overlay_clear(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                           uint8_t color, uint8_t options)
{
    (void)x; (void)y; (void)w; (void)h; (void)color; (void)options;
    glyphs.clear();
    revealed = 0;
    drawn = false;
}

void hw_text_overlay_set_scroll(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t color)
{
    (void)x; (void)y; (void)w; (void)h; (void)color; // P3 v1 uses a fixed text region
}

void hw_text_update(void)
{
    if(!active) return;
    ++frame;
    if(drawn) return;
    bool changed = false;
    if(bn::keypad::a_held() || bn::keypad::b_held())
    {
        if(revealed < page_len) { revealed = page_len; changed = true; }
    }
    else if((frame & SPEED_MASK) == 0)
    {
        if(revealed < page_len) { ++revealed; changed = true; }
    }
    if(revealed >= page_len) drawn = true;
    if(changed) render();
}

int hw_text_window_ready(void) { return 1; } // v1 positions instantly
int hw_text_drawn(void) { return (active && drawn) ? 1 : 0; }
int hw_text_btn_a(void) { return bn::keypad::a_pressed() ? 1 : 0; }
