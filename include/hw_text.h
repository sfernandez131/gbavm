// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Dialogue/text bridge (P3): the flat C surface vm.c (C) uses to drive the Butano
// text renderer in hw_text.cpp (C++). Glyphs are drawn as sprites by a
// bn::sprite_text_generator built from the converted fonts (font_table). No Butano
// types cross this boundary.

#ifndef GBAVM_HW_TEXT_H
#define GBAVM_HW_TEXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// VM_LOAD_TEXT: copy the page string (NUL-terminated, control codes inline).
void hw_text_load(const uint8_t* str, int len);
// VM_DISPLAY_TEXT_EX: arm the typewriter reveal of the loaded page.
void hw_text_display(uint8_t options, uint8_t start_tile);
// VM_SET_FONT: select the active font (index into the font_table registry).
void hw_text_set_font(int index);
// VM_OVERLAY_* : show/hide/position the dialogue text region (tile coords).
void hw_text_overlay_move_to(uint8_t x, uint8_t y, int8_t speed);
void hw_text_overlay_setpos(uint8_t x, uint8_t y);
void hw_text_overlay_show(uint8_t x, uint8_t y, uint8_t color, uint8_t options);
void hw_text_overlay_clear(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                           uint8_t color, uint8_t options);
void hw_text_overlay_set_scroll(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                                uint8_t color);

// Per-frame tick: advance the box state + typewriter reveal. Called from main().
void hw_text_update(void);

// VM_OVERLAY_WAIT predicates.
int hw_text_window_ready(void); // dialogue region is shown (or fully hidden)
int hw_text_drawn(void);        // typewriter finished revealing the page
int hw_text_btn_a(void);        // a fresh A-press edge occurred this frame

#ifdef __cplusplus
}
#endif

#endif // GBAVM_HW_TEXT_H
