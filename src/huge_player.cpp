// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// hUGE player (M14): plays GB Studio's native .uge music on the GBA's Game Boy PSG.
//
// A C++ port of hUGEDriver's player logic. hUGETracker and hUGEDriver are by SuperDisk and
// "dedicated to the public domain" (https://github.com/SuperDisk/hUGEDriver); the source
// ported from is the copy GB Studio vendors at
// appData/engine/gbvm/third-party/HUGE_TRACKER/hUGEDriver.asm. Labels in the comments
// below (get_current_note, tick_time, ...) name the routine each piece comes from.
//
// Why a port rather than converting .uge to .vgm at build time: see gba-studio
// docs/M14_MUSIC_DESIGN.md. In short, Butano's VGM player halts on a volume change and
// asserts on a pattern/row seek, and a register log has no rows for VM_MUSIC_SETPOS.
//
// Done so far: the tick/row/order machinery and note playback on all four channels
// (M14b: pulse 1-2; M14c1: wave 3, noise 4), all 16 effects (M14c2), and instrument
// subpattern tables (M14c3). That is the whole driver; what remains of M14 is wiring it to
// the engine (eject, VM_MUSIC_ROUTINE, SFX).

#include "huge_player.h"

#include <cstdint>

#include "bn_dmg_music.h"

extern "C" {
#include "vm.h" // sys_time
}

// ---- verification trace (never enabled in a real build) ------------------------------------
// Every Game Boy register write the player makes goes through nr_write() below. When
// HUGE_TRACE is defined - which only gba-studio's scripts/huge/add_probe.py does, in a
// throwaway checkout - each write is also logged here for the GDB stub to dump and
// scripts/huge/reference.py to diff. Compiled out otherwise: no RAM, no cost.
#ifdef HUGE_TRACE
extern "C" {
struct huge_trace_t
{
    uint16_t frame;
    uint16_t tick;
    uint8_t order;
    uint8_t row;
    uint8_t reg;        // GB register (low byte of 0xFFxx), or a TRACE_* pseudo-register
    uint8_t pad;
    uint16_t value;
    uint16_t pad2;
};
constexpr int HUGE_TRACE_MAX = 4096;
// 48 KB, so it has to live in EWRAM - IWRAM is 32 KB and a project's scripts already sit
// there. The EWRAM section name differs by toolchain (the Wonderful build passes
// BN_EWRAM_BSS_SECTION=".ewram_bss"; devkitARM uses ".sbss"), so take Butano's own macro
// with Butano's own fallback rather than hard-coding either.
#ifndef BN_EWRAM_BSS_SECTION
#define BN_EWRAM_BSS_SECTION ".sbss"
#endif
__attribute__((section(BN_EWRAM_BSS_SECTION))) huge_trace_t huge_trace[HUGE_TRACE_MAX];
uint16_t huge_trace_n = 0;
// The last wave written, read back through the emulated bus while its bank is still the
// writable one. Needed because a debugger cannot check wave RAM: mGBA's GDB stub reads it
// as zero and does not pass writes through, so this is the only way to see the bytes land.
uint8_t huge_wave_readback[16];
}
#endif

// Driver ticks since the song started (wraps at 65536). Always compiled in: 2 bytes that
// let gba-studio's CI runtime test prove a .uge track is playing AND at 64 Hz, by walking
// a known number of frames. A plain global, because GDB reads those reliably.
extern "C" {
uint16_t huge_ticks = 0;
}

namespace
{
    // ---- the Game Boy's sound registers, on the GBA ------------------------------------
    // The port talks to the PSG in the driver's own terms: one byte to one GB register
    // (NR10..NR51), exactly as each `ldh [rAUDxxx], a` does. The GBA has the same PSG, and
    // its registers carry the GB bytes at fixed offsets (SOUND1CNT_H = NR11 | NR12 << 8,
    // ...), each writable a byte at a time - mGBA even dispatches those byte writes straight
    // to its NR11/NR12/... handlers. So a GB register write becomes one GBA byte write, with
    // two exceptions handled below: NR30 and NR32.
    //
    // Why bytes, and not the paired halfwords M14b/c1 wrote: the effects write registers
    // ALONE (set duty writes NR11 without NR12; vol slide writes NR12 without NR11), and a
    // halfword would also rewrite the neighbour - reloading a length counter the GB never
    // touched.
    enum nr_t : uint8_t
    {
        NR10 = 0x10, NR11, NR12, NR13, NR14,
        NR21 = 0x16, NR22, NR23, NR24,
        NR30 = 0x1A, NR31, NR32, NR33, NR34,
        NR41 = 0x20, NR42, NR43, NR44,
        NR50 = 0x24, NR51,
    };
    constexpr int NR_COUNT = 0x16;          // NR10..NR51

    // Pseudo-registers, for the trace only: events that are not a single register byte.
    constexpr uint8_t TRACE_WAVE = 0x30;    // 16 bytes into wave RAM; value = wave index
    constexpr uint8_t TRACE_ROUTINE = 0x40; // "call routine"; value = channel << 8 | param

    // Each GB register's byte address on the GBA (offset into IO). 0 = no register there.
    constexpr uint8_t gba_offset[NR_COUNT] = {
        0x60, 0x62, 0x63, 0x64, 0x65,       // NR10 NR11 NR12 NR13 NR14
        0x00, 0x68, 0x69, 0x6C, 0x6D,       // ---- NR21 NR22 NR23 NR24
        0x70, 0x72, 0x73, 0x74, 0x75,       // NR30 NR31 NR32 NR33 NR34
        0x00, 0x78, 0x79, 0x7C, 0x7D,       // ---- NR41 NR42 NR43 NR44
        0x80, 0x81,                         // NR50 NR51
    };

    // The driver READS registers back (vol slide, set volume, set duty on CH4, the CH3
    // routing mute), and gets the GB's answer: the last byte written, with every write-only
    // bit reading as 1. The GBA's read masks differ (NR14's byte reads 0x00-or-0x40 there,
    // not 0xBF-or-0xFF), so reads come from a shadow of the GB registers, never the GBA.
    // Masks per the Pan Docs / blargg's dmg_sound tests.
    constexpr uint8_t gb_read_mask[NR_COUNT] = {
        0x80, 0x3F, 0x00, 0xFF, 0xBF,
        0xFF, 0x3F, 0x00, 0xFF, 0xBF,
        0x7F, 0xFF, 0x9F, 0xFF, 0xBF,
        0xFF, 0xFF, 0x00, 0x00, 0xBF,
        0x00, 0x00,
    };

    constexpr uintptr_t IO = 0x04000000;
    volatile uint16_t& reg(uintptr_t addr) { return *reinterpret_cast<volatile uint16_t*>(addr); }
    volatile uint8_t& reg8(uintptr_t addr) { return *reinterpret_cast<volatile uint8_t*>(addr); }
    constexpr uintptr_t SOUND1CNT_H = 0x04000062;
    constexpr uintptr_t SOUND1CNT_X = 0x04000064;
    constexpr uintptr_t SOUND2CNT_L = 0x04000068;
    constexpr uintptr_t SOUND2CNT_H = 0x0400006C;
    constexpr uintptr_t SOUND3CNT_L = 0x04000070;
    constexpr uintptr_t SOUND4CNT_L = 0x04000078;
    constexpr uintptr_t SOUND4CNT_H = 0x0400007C;
    constexpr uintptr_t SOUNDCNT_L  = 0x04000080;
    constexpr uintptr_t SOUNDCNT_H  = 0x04000082;
    constexpr uintptr_t WAVE_RAM    = 0x04000090;

    // hUGE_note_table.inc: the 11-bit GB period for each of the 72 notes.
    constexpr uint16_t note_table[LAST_NOTE] = {
        44, 156, 262, 363, 457, 547, 631, 710, 786, 854, 923, 986,
        1046, 1102, 1155, 1205, 1253, 1297, 1339, 1379, 1417, 1452, 1486, 1517,
        1546, 1575, 1602, 1627, 1650, 1673, 1694, 1714, 1732, 1750, 1767, 1783,
        1798, 1812, 1825, 1837, 1849, 1860, 1871, 1881, 1890, 1899, 1907, 1915,
        1923, 1930, 1936, 1943, 1949, 1954, 1959, 1964, 1969, 1974, 1978, 1982,
        1985, 1988, 1992, 1995, 1998, 2001, 2004, 2006, 2009, 2011, 2013, 2015,
    };

    constexpr int PATTERN_LENGTH = 64;
    constexpr int TABLE_LENGTH = 32;        // rows the exporter writes per subpattern
    constexpr uint8_t NO_NOTE = ___;
    constexpr uint8_t FX_TONEPORTA = 3;
    constexpr uint8_t NO_WAVE = 100;        // hUGE_NO_WAVE: forces the first wave to load

    // The per-channel block hUGEDriver keeps in WRAM (channel1..channel4).
    struct channel_t
    {
        uint16_t period = 0;                    // channel_period (ch4: the poly, low byte)
        uint16_t toneporta_target = 0;
        uint8_t note = 0;                       // channel_note
        uint8_t highmask = 0;                   // NRx4 bits: 0x80 trigger, 0x40 length enable
        const unsigned char* table = nullptr;   // the instrument's subpattern, or none
        uint8_t table_row = 0;
    };

    struct state_t
    {
        const hUGESong_t* song = nullptr;
        uint8_t ticks_per_row = 0;
        uint8_t order_count = 0;                // entries, NOT GB's words-times-two order_cnt
        uint8_t current_order = 0;
        uint8_t row = 0;
        uint8_t tick = 0;
        uint8_t counter = 0;
        uint8_t row_break = 0;                  // set by effects B/D; 1-based, 0 = none
        uint8_t next_order = 0;                 // set by effect B; 1-based, 0 = none
        uint8_t mute_mask = 0;
        uint8_t current_wave = NO_WAVE;
        uint8_t step_width4 = 0;                // NR43 bit 3 (7-bit noise)
        const unsigned char* pattern[4] = {};
        channel_t ch[4];
        uint8_t nr[NR_COUNT] = {};              // the GB register shadow
        bool playing = false;
        bool took_over = false;                 // PSG claimed (M14a: not in the stop's frame)
        uint16_t play_frame = 0;
        uint32_t tick_accum = 0;
    };

    state_t s;

    // NR50, the PSG master volume. Kept outside state_t because it outlives a song: GBVM
    // sets it to 0x77 once at sound init, and after that only VM_SOUND_MASTERVOL and the
    // master-volume effect change it - music_sound_cut resets NR51 on every track change
    // but never touches NR50.
    uint8_t master_nr50 = 0x77;

    void trace(uint8_t reg_id, uint16_t value)
    {
#ifdef HUGE_TRACE
        if(huge_trace_n < HUGE_TRACE_MAX)
        {
            huge_trace[huge_trace_n++] = { sys_time, huge_ticks, s.current_order, s.row,
                                           reg_id, 0, value, 0 };
        }
#else
        (void)reg_id;
        (void)value;
#endif
    }

    // One GB register write: shadow it, put it on the GBA, trace it.
    void nr_write(uint8_t nr, uint8_t value)
    {
        s.nr[nr - NR10] = value;
        if(nr == NR50) master_nr50 = value;
        uint8_t out = value;
        if(nr == NR30)
        {
            // SOUND3CNT_L is not NR30. The GB honours bit 7 only (DAC on); the GBA reads
            // bit 6 as a wave-BANK select and bit 5 as 64-sample mode, so the GB's 0xFF
            // would switch banks and double the wave. Keep bit 7 and play bank 0.
            out = value & 0x80;
        }
        else if(nr == NR32)
        {
            // Bits 5-6 are the volume code on both machines. Bit 7 is unused on the GB but
            // forces 75% volume on the GBA, and vol slide can compute one - drop it.
            out = value & 0x60;
        }
        reg8(IO + gba_offset[nr - NR10]) = out;
        trace(nr, value);
    }

    uint8_t nr_read(uint8_t nr) { return s.nr[nr - NR10] | gb_read_mask[nr - NR10]; }

    // A channel's registers are 5 apart: NR12, NR17 (NR22), NR1C (NR32), NR21 (NR42).
    uint8_t nrx2(int c) { return uint8_t(NR12 + 5 * c); }
    uint8_t nrx4(int c) { return uint8_t(NR14 + 5 * c); }

    bool muted(int c) { return (s.mute_mask >> c) & 1; }

    // The Z80 SWAP instruction: exchange the nibbles of a byte. The driver uses it as a
    // cheap multiply by 16, but it only IS one for values below 16 - so where an input can
    // be larger, the port has to swap, not shift, to stay faithful.
    uint8_t swap(uint8_t v) { return uint8_t((v << 4) | (v >> 4)); }

    // get_note_period. The driver indexes the table with `add a` - the note doubled in 8
    // bits - so it really looks up `note mod 128`, and notes 128..199 play notes 0..71
    // exactly; the port does the same. The rest land past the table, where the GB reads
    // whatever ROM follows, which is unknowable, so the port picks a sensible note - a
    // GBA-side decision, not a driver rule:
    //   72..127: above the top (an arpeggio adds up to 15 to a note up to 71): the top note.
    //   200..255: a subpattern offset taking a note below 0, wrapped: the bottom note.
    uint16_t note_period(uint8_t note)
    {
        if(note < LAST_NOTE) return note_table[note];
        if(note < 128) return note_table[LAST_NOTE - 1];
        if(note < 128 + LAST_NOTE) return note_table[note - 128];
        return note_table[0];
    }

    // get_note_poly: a note's noise "polynomial counter" (NR43), RichardULZ's formula. Kept
    // in 8-bit arithmetic with the real SWAP, because notes 64+ wrap `note + 192` and land
    // on an `l` above 15, where swap and shift disagree.
    uint8_t note_poly(uint8_t note)
    {
        uint8_t a = uint8_t(~uint8_t(note + 192));   // add 192 / cpl: 63 - note, wrapping
        if(a < 7) return a;
        const uint8_t l = uint8_t((a >> 2) - 1);     // srl a / srl a / dec a
        return uint8_t(((a & 3) + 4) | swap(l));
    }

    // load_patterns: point each channel at its pattern for the given order entry.
    void load_patterns(uint8_t order)
    {
        const hUGESong_t& song = *s.song;
        s.pattern[0] = song.order1[order];
        s.pattern[1] = song.order2[order];
        s.pattern[2] = song.order3[order];
        s.pattern[3] = song.order4[order];
    }

    // get_current_row: the three bytes of a channel's cell on the current row. `fx` is the
    // instrument (upper nibble) over the effect code (lower nibble).
    struct cell_t
    {
        uint8_t note;
        uint8_t fx;
        uint8_t param;
    };

    cell_t current_row(int c)
    {
        const unsigned char* cell = s.pattern[c] + s.row * 3;
        return { cell[0], cell[1], cell[2] };
    }

    // setup_instrument_pointer: instrument IDs are 1-based, 0 = none. The driver reads only
    // the upper nibble of the second byte; DN's bit 4 in the note byte is never consulted.
    uint8_t instrument_id(uint8_t fx) { return fx >> 4; }

    // ---- playing notes ---------------------------------------------------------------

    // update_ch3_waveform: copy one 16-byte wave into wave RAM.
    //
    // The GB side of this is the driver's: mute CH3's routing, NR30 = 0, the 16 bytes,
    // NR30 = 0x80, restore routing. The GBA needs one more step in the middle. It has two
    // wave-RAM banks, and the CPU can only write the one that is NOT playing - so, following
    // gbt-player's GBA recipe, stop with bank 1 selected (making bank 0 writable), write,
    // and let NR30 = 0x80 play bank 0. Wave bytes are high-nibble-first on both machines, so
    // they copy straight through.
    void load_wave(uint8_t wave)
    {
        s.current_wave = wave;
        // `swap a / add [hl]`. A wave index of 16 or more swaps to an offset that can run
        // past the 16 waves; the GB would read on into ROM. Hold the last wave instead.
        uint8_t offset = swap(wave);
        if(offset > 240) offset = 240;
        const unsigned char* src = s.song->waves + offset;

        const uint8_t routing = nr_read(NR51);
        nr_write(NR51, routing & 0xBB);
        nr_write(NR30, 0x00);
        reg(SOUND3CNT_L) = 0x40;                                // GBA: stop on bank 1, write bank 0
        for(int i = 0; i < 16; i += 2)
        {
            reg(WAVE_RAM + uintptr_t(i)) = uint16_t(src[i] | (src[i + 1] << 8));
        }
#ifdef HUGE_TRACE
        for(int i = 0; i < 16; i += 2)                          // still bank 0: read it back
        {
            const uint16_t v = reg(WAVE_RAM + uintptr_t(i));
            huge_wave_readback[i] = uint8_t(v);
            huge_wave_readback[i + 1] = uint8_t(v >> 8);
        }
#endif
        trace(TRACE_WAVE, wave);
        nr_write(NR30, 0x80);                                   // play bank 0, 32 samples
        nr_write(NR51, routing);
    }

    // play_ch1_note / play_ch2_note: write the period with the highmask on top. With the
    // trigger bit set this restarts the note; without it (a note with no instrument) it
    // only retunes the channel, which is how hUGE plays legato.
    void play_duty_note(int c)
    {
        if(muted(c)) return;
        const channel_t& ch = s.ch[c];
        nr_write(uint8_t(NR13 + 5 * c), uint8_t(ch.period));
        nr_write(nrx4(c), uint8_t(ch.highmask | (ch.period >> 8)));
    }

    // play_ch3_note. The driver stops and restarts CH3 around the trigger because
    // retriggering while the DMG reads a wave byte corrupts wave RAM - a DMG bug the GBA
    // does not have, but harmless to keep.
    void play_wave_note()
    {
        if(muted(2)) return;
        const channel_t& ch = s.ch[2];
        const uint8_t routing = nr_read(NR51);
        nr_write(NR51, routing & 0xBB);
        nr_write(NR30, 0x00);
        nr_write(NR30, 0xFF);                                   // `cpl`; nr_write keeps bit 7
        nr_write(NR33, uint8_t(ch.period));
        nr_write(NR34, uint8_t(ch.highmask | (ch.period >> 8)));
        nr_write(NR51, routing);
    }

    // play_ch4_note: NR43 is the poly byte, NR44 the highmask (no period bits here).
    void play_noise_note()
    {
        if(muted(3)) return;
        const channel_t& ch = s.ch[3];
        nr_write(NR43, uint8_t(ch.period));
        nr_write(NR44, ch.highmask);
    }

    // play_note: dispatch to the channel's routine.
    void play_note(int c)
    {
        switch(c)
        {
        case 2: play_wave_note(); break;
        case 3: play_noise_note(); break;
        default: play_duty_note(c); break;
        }
    }

    // update_channel_freq: retune a channel without touching the instrument, and possibly
    // restart it (`mask` is ORed into NRx4). For CH1-3, `period` is the new period and is
    // stored. CH4 is different in a way the effects inherit: it takes a NOTE in the low byte
    // (`E`), turns that into a poly, and stores nothing. The pitch effects pass it a PERIOD
    // anyway - so on CH4 they compute a poly from the period's low byte. That is what the
    // driver does, so it is what the port does.
    void update_channel_freq(int c, uint16_t period, uint8_t mask)
    {
        if(muted(c)) return;
        if(c == 3)
        {
            nr_write(NR43, uint8_t(note_poly(uint8_t(period)) | s.step_width4));
            nr_write(NR44, mask);
            return;
        }
        s.ch[c].period = period;
        nr_write(uint8_t(NR13 + 5 * c), uint8_t(period));
        nr_write(nrx4(c), uint8_t((period >> 8) | mask));
    }

    // ---- effects (M14c2) -------------------------------------------------------------
    // do_effect dispatches on the effect code with the driver's calling convention: the
    // zero flag set on tick 0. Each fx_ routine starts with the tick test it wants - `ret
    // nz` (tick 0 only), `ret z` (every tick but 0), or `nop` (every tick). Those tests are
    // in do_effect's switch below.

    // fx_arpeggio: cycle note, note + x, note + y on successive ticks, keyed off the global
    // tick counter (not the row's tick), so the phase carries across rows.
    void fx_arpeggio(int c, uint8_t param)
    {
        uint8_t phase = uint8_t(s.counter - 1);
        while(phase >= 3) phase -= 3;                   // "a crappy modulo"
        uint8_t note = s.ch[c].note;
        if(phase == 0) note = uint8_t(note + (param & 0x0F));
        else if(phase == 1) note = uint8_t(note + (param >> 4));
        update_channel_freq(c, note_period(note), 0);
    }

    // fx_vibrato: square-wave vibrato. The upper nibble masks the tick counter (the speed),
    // the lower nibble is how far up to bend.
    void fx_vibrato(int c, uint8_t param)
    {
        uint16_t period = note_period(s.ch[c].note);
        if((s.counter & (param >> 4)) == 0) period = uint16_t(period + (param & 0x0F));
        update_channel_freq(c, period, 0);
    }

    // fx_toneporta, ticks after 0: slide the period toward the target and stop exactly on
    // it. The first slide tick also carries the note's trigger bit, which it then clears -
    // so a toneporta row with a new instrument restarts the note once, on tick 1.
    void fx_toneporta(int c, uint8_t param)
    {
        channel_t& ch = s.ch[c];
        uint16_t period = ch.period;
        const uint16_t target = ch.toneporta_target;
        if(target < period)
        {
            period = uint16_t(period - param);
            if((period & 0x8000) || period < target) period = target;   // `bit 7, d`: underflow
        }
        else if(target > period)
        {
            period = uint16_t(period + param);
            if(period > target) period = target;
        }
        ch.period = period;                             // stored even on a muted channel
        const uint8_t mask = ch.highmask;
        ch.highmask &= 0x7F;
        update_channel_freq(c, period, mask);
    }

    // fx_set_duty: NRx1 on the pulse channels; on CH3 the "duty" is a wave index, loaded and
    // then restarted; on CH4 it replaces NR43's step-width bit.
    void fx_set_duty(int c, uint8_t param)
    {
        if(muted(c)) return;
        switch(c)
        {
        case 0: nr_write(NR11, param); break;
        case 1: nr_write(NR21, param); break;
        case 2:
            load_wave(param);
            play_note(2);
            break;
        default: nr_write(NR43, uint8_t((nr_read(NR43) & ~0x08) | param)); break;
        }
    }

    // fx_set_volume: the envelope's initial volume; CH3 quantises to its four levels.
    void fx_set_volume(int c, uint8_t param)
    {
        if(muted(c)) return;
        const uint8_t v = swap(param);
        switch(c)
        {
        case 2:
        {
            uint8_t level = 0x60;                       // 25%
            if(v >= 0xA0) level = 0x20;                 // 100%
            else if(v >= 0x50) level = 0x40;            // 50%
            else if(v == 0) level = 0x00;
            nr_write(NR32, level);
            break;
        }
        case 3:
            nr_write(NR42, v);                          // the whole swapped byte, low nibble too
            play_noise_note();
            break;
        default:
            nr_write(nrx2(c), uint8_t((nr_read(nrx2(c)) & 0x0F) | v));
            play_duty_note(c);
            break;
        }
    }

    // fx_vol_slide: really "retrigger with the volume moved": x up, y down, clamped to
    // 0..15. It drops the envelope's direction and pace, and it reads NRx2 and NRx4 back -
    // on CH3 that means NR32's 0x9F read mask feeds the arithmetic, as on the GB.
    void fx_vol_slide(int c, uint8_t param)
    {
        if(muted(c)) return;
        const uint8_t down = param & 0x0F;
        const uint8_t up = param >> 4;
        uint8_t v = uint8_t(nr_read(nrx2(c)) >> 4);
        v = v >= down ? uint8_t(v - down) : 0;
        v = uint8_t(v + up);
        if(v >= 0x10) v = 0x0F;
        nr_write(nrx2(c), uint8_t(v << 4));
        nr_write(nrx4(c), uint8_t(nr_read(nrx4(c)) | 0x80));
        play_note(c);
    }

    // note_cut: envelope to 0, then retrigger (not on CH3, which has no envelope to zero).
    void note_cut(int c)
    {
        nr_write(nrx2(c), 0);
        if(c == 2) return;
        nr_write(nrx4(c), 0xFF);
    }

    // do_effect. Returns false where the driver takes `ret_dont_play_note`: the tick-0
    // caller must then NOT play the row's note (toneporta slides to it instead; note delay
    // plays it later).
    //
    // `from_table` is do_effect.no_set_offset, the entry do_table uses: it jumps to each
    // effect routine ONE BYTE PAST ITS START. For most effects that byte is the tick test
    // (`ret z` / `ret nz`), so from a table they run on every tick, tick 0 included. Three
    // start differently, and what the skip does to them was read off the assembled driver
    // (GBVM's lib/hUGEDriver.lib), not guessed:
    //   toneporta  `jr z, .setup` (28 55): the skip executes the operand 0x55, `ld d, l`,
    //              which the next instruction overwrites - so it slides on every tick and
    //              never sets its target.
    //   note delay `jr z, ret_dont_play_note` (28 BD): the operand is `cp l`, whose flags
    //              the following `cp c` replaces - so it plays the note when tick == param,
    //              tick 0 included, and never suppresses anything.
    //   note cut   `cp c` (B9): skipped, so `ret nz` tests do_effect's `or a` on the tick -
    //              it cuts on tick 0, whatever its param.
    bool do_effect(int c, uint8_t fx, uint8_t param, bool from_table = false)
    {
        const uint8_t code = fx & 0x0F;
        if((code | param) == 0) return true;
        const uint8_t tick = s.tick;
        const bool tick0 = tick == 0;
        const bool on_tick0 = tick0 || from_table;      // routines opening with `ret nz`
        const bool after_tick0 = !tick0 || from_table;  // routines opening with `ret z`

        switch(code)
        {
        case 0x0:                                       // arpeggio: every tick
            fx_arpeggio(c, param);
            break;
        case 0x1:                                       // porta up
            if(after_tick0) update_channel_freq(c, uint16_t(s.ch[c].period + param), 0);
            break;
        case 0x2:                                       // porta down
            if(after_tick0) update_channel_freq(c, uint16_t(s.ch[c].period - param), 0);
            break;
        case 0x3:                                       // toneporta
            if(tick0 && !from_table)
            {
                s.ch[c].toneporta_target = note_period(s.ch[c].note);
                return false;
            }
            fx_toneporta(c, param);
            break;
        case 0x4:                                       // vibrato
            if(after_tick0) fx_vibrato(c, param);
            break;
        case 0x5:                                       // set master volume (global)
            if(on_tick0) nr_write(NR50, param);
            break;
        case 0x6:                                       // call routine: every tick
        {
            // SDCC's calling convention put the tick in A and channel << 8 | param in DE.
            const uint16_t arg = uint16_t((c << 8) | param);
            trace(TRACE_ROUTINE, arg);
            const hUGERoutine_t routine = s.song->routines ? s.song->routines[param & 0x0F] : nullptr;
            if(routine) routine(tick, arg);
            break;
        }
        case 0x7:                                       // note delay
            if(tick0 && !from_table) return false;
            if(tick == param) play_note(c);
            break;
        case 0x8:                                       // set pan (global)
            if(on_tick0) nr_write(NR51, param);
            break;
        case 0x9:                                       // set duty
            if(on_tick0) fx_set_duty(c, param);
            break;
        case 0xA:                                       // volume slide
            if(on_tick0) fx_vol_slide(c, param);
            break;
        case 0xB:                                       // position jump (global)
            if(on_tick0)
            {
                // `or [hl]` assumes A is 0 - true on tick 0. From a table on a later tick,
                // A holds the tick, so the break is NOT armed and only next_order is set.
                if((tick | s.row_break) == 0) s.row_break = 1;
                s.next_order = param;
            }
            break;
        case 0xC:                                       // set volume
            if(on_tick0) fx_set_volume(c, param);
            break;
        case 0xD:                                       // pattern break (global)
            if(on_tick0) s.row_break = param;
            break;
        case 0xE:                                       // note cut, on tick `param`
            if(tick == (from_table ? 0 : param) && !muted(c)) note_cut(c);
            break;
        default:                                        // 0xF set speed (global)
            if(on_tick0) s.ticks_per_row = param;
            break;
        }
        return true;
    }

    // ---- subpattern tables (M14c3) ---------------------------------------------------

    // do_table: run one row of the channel's table. Tables advance one row per TICK, not
    // per row, and run on every tick including 0. A table row is a pattern cell with the
    // instrument slot reused: jump (5 bits: the instrument nibble, bit 4 in the note byte),
    // a note OFFSET (36 = none, ___ = no change), and an effect.
    void do_table(int c)
    {
        channel_t& ch = s.ch[c];
        const uint8_t r = ch.table_row++;                   // ld a, [hl] / inc [hl]
        // Rows past the end: the exporter writes 32, and a table whose last row has no jump
        // runs off it (the editor can make one). The GB then reads whatever data follows;
        // the port reads empty rows instead - a GBA-side decision. table_row keeps counting
        // and wraps at 256 as on the GB, so the table replays from row 0 after that.
        static constexpr unsigned char empty[3] = { NO_NOTE, 0, 0 };
        const unsigned char* cell = r < TABLE_LENGTH ? ch.table + r * 3 : empty;
        uint8_t note = cell[0];
        const uint8_t fx = cell[1];
        const uint8_t param = cell[2];

        uint8_t jump = fx & 0xF0;                           // ld a, b / and $F0
        if(note & 0x80)                                     // bit 7, d: the jump's bit 4
        {
            note &= 0x7F;
            jump |= 1;
        }
        jump = swap(jump);
        if(jump) ch.table_row = uint8_t(jump - 1);          // 1-based; 0 = no jump

        if(note != NO_NOTE)
        {
            // An offset from the channel's note, in 8 bits; retune without retriggering.
            // CH4's update takes the NOTE and makes a poly; the others take a period.
            const uint8_t n = uint8_t(ch.note + uint8_t(note - 36));
            const uint8_t mask = ch.highmask & 0x7F;
            update_channel_freq(c, c == 3 ? n : note_period(n), mask);
        }

        do_effect(c, fx, param, true);
    }

    // ---- tick 0: the row -------------------------------------------------------------

    // The tick-0 path for one channel (hUGE_dosound's ch1 block, process_ch2/3/4): load the
    // note and instrument, run the effect, play the note, run the table.
    void row(int c)
    {
        const cell_t cell = current_row(c);
        // `jr nc, .do_setvolN`: a rest skips the period AND the instrument - only the effect
        // runs. (Easy to misread: the instrument block sits after this branch.)
        const bool note = cell.note < LAST_NOTE;
        channel_t& ch = s.ch[c];

        if(note)
        {
            ch.note = cell.note;
            if(c == 3)
            {
                // CH4 plays a poly counter, not a period, and has no toneporta check. Only
                // the low byte of channel_period4 is ever written here.
                ch.period = uint16_t((ch.period & 0xFF00) | note_poly(cell.note));
            }
            else if((cell.fx & 0x0F) != FX_TONEPORTA)
            {
                ch.period = note_table[cell.note];
            }

            const uint8_t id = instrument_id(cell.fx);
            if(id == 0)
            {
                // No instrument: retune, don't retrigger. (On CH4 the period also stays
                // WITHOUT the step width - the driver only ORs it in on the instrument path.)
                ch.highmask &= 0x7F;
            }
            else if(!muted(c))
            {
                // A muted channel with an instrument leaves highmask untouched (checkMute
                // jumps past .write_maskN).
                switch(c)
                {
                case 0:
                case 1:
                {
                    const hUGEDutyInstr_t& in = s.song->duty_instruments[id - 1];
                    if(c == 0) nr_write(NR10, in.sweep);
                    nr_write(uint8_t(NR11 + 5 * c), in.len_duty);
                    nr_write(nrx2(c), in.envelope);
                    ch.table = in.subpattern;
                    ch.highmask = in.highmask;
                    break;
                }
                case 2:
                {
                    const hUGEWaveInstr_t& in = s.song->wave_instruments[id - 1];
                    nr_write(NR31, in.length);
                    nr_write(NR32, in.volume);
                    if(in.waveform != s.current_wave) load_wave(in.waveform);
                    ch.table = in.subpattern;
                    ch.highmask = in.highmask;
                    break;
                }
                default:
                {
                    const hUGENoiseInstr_t& in = s.song->noise_instruments[id - 1];
                    // The noise instrument's "highmask" byte packs three fields: length in
                    // bits 0-5, length-enable in bit 6, and 7-bit (short) mode in bit 7.
                    nr_write(NR42, in.envelope);
                    ch.table = in.subpattern;
                    nr_write(NR41, in.highmask & 0x3F);
                    s.step_width4 = swap(uint8_t(in.highmask & 0x80));  // bit 7 -> NR43 bit 3
                    ch.period = uint16_t(ch.period | s.step_width4);
                    ch.highmask = uint8_t((in.highmask & 0x40) | 0x80);
                    break;
                }
                }
                ch.table_row = 0;
            }
        }

        const bool play = do_effect(c, cell.fx, cell.param);
        if(note && play) play_note(c);

        if(ch.table) do_table(c);
    }

    // process_effects: ticks after 0. The effect is skipped outright on a muted channel or
    // when its PARAM is zero - so, unlike on tick 0, an effect like E00 or 600 does nothing
    // here.
    void process_effects()
    {
        for(int c = 0; c < 4; ++c)
        {
            if(!muted(c))
            {
                const cell_t cell = current_row(c);
                if(cell.param != 0) do_effect(c, cell.fx, cell.param);
            }
            if(s.ch[c].table) do_table(c);              // even on a muted channel
        }
    }

    // tick_time: advance the tick, and on the last tick of a row, the row and order.
    void tick_time()
    {
        ++s.counter;
        if(++s.tick != s.ticks_per_row) return;     // uint8_t: a speed of 0 is 256 ticks
        s.tick = 0;

        uint8_t order_to_load;
        uint8_t start_row;
        if(s.row_break)
        {
            // A row/pattern break (effects B and D). Both counters are stored one-based so
            // zero can mean "not set". Out-of-range values would have the GB read past the
            // pattern or the order table; the port starts the row or order over instead.
            start_row = uint8_t(s.row_break - 1);
            if(start_row >= PATTERN_LENGTH) start_row = 0;
            s.row_break = 0;
            if(s.next_order)
            {
                order_to_load = uint8_t(s.next_order - 1);
                if(order_to_load >= s.order_count) order_to_load = 0;
                s.next_order = 0;
            }
            else
            {
                order_to_load = uint8_t(s.current_order + 1);
                if(order_to_load == s.order_count) order_to_load = 0;
            }
        }
        else
        {
            if(++s.row != PATTERN_LENGTH) return;
            start_row = 0;
            order_to_load = uint8_t(s.current_order + 1);
            if(order_to_load == s.order_count) order_to_load = 0; // songs loop
        }

        s.current_order = order_to_load;
        load_patterns(order_to_load);
        s.row = start_row;
    }

    // hUGE_dosound: one driver tick.
    void dosound()
    {
        if(s.tick == 0)
        {
            for(int c = 0; c < 4; ++c) row(c);
        }
        else
        {
            process_effects();
        }
        tick_time();
    }

    // M14a: claim the whole PSG. Butano's gbt player leaves channels RUNNING when it
    // stops (SOUNDCNT_X still flagged ch2/ch3 after a stop), so silence all four rather
    // than assume they are quiet, then route them all to both speakers as GBVM's
    // music_sound_cut does (NR51 = 0xFF), at the persisting master volume, and run the PSG
    // at full strength. Not traced: this is the GBA handover, not the driver - but the
    // shadow is set to match.
    void take_over_psg()
    {
        reg(SOUND1CNT_H) = 0;            // envelope volume 0
        reg(SOUND1CNT_X) = 0x8000;       // restart so the silent envelope takes effect
        reg(SOUND2CNT_L) = 0;
        reg(SOUND2CNT_H) = 0x8000;
        reg(SOUND3CNT_L) = 0;            // wave channel stopped (load_wave restarts it)
        reg(SOUND4CNT_L) = 0;
        reg(SOUND4CNT_H) = 0x8000;
        reg(SOUNDCNT_L) = uint16_t(0xFF00 | master_nr50); // NR51 = all L+R; NR50
        reg(SOUNDCNT_H) = uint16_t((reg(SOUNDCNT_H) & ~3u) | 2u); // PSG output at 100%
        s.nr[NR50 - NR10] = master_nr50;
        s.nr[NR51 - NR10] = 0xFF;
    }
}

extern "C" {

// The callback behind hUGE's "call routine" effect - what GB Studio's VM_MUSIC_ROUTINE
// listens to. Wired to music events in M14e; until then routines are raised into nothing.
void hUGETrackerRoutine(unsigned char tick, unsigned int param)
{
    (void)tick;
    (void)param;
}

void huge_play(const hUGESong_t* song)
{
    if(!song) return;

    // GBVM's music_load: asking for the track that is already playing does nothing, so
    // a scene that starts the same music as the last one carries on without a restart.
    // After a stop the same track starts over.
    if(s.playing && s.song == song) return;

    // One PSG, one owner. Butano's stop is DEFERRED - it queues DMG_MUSIC_STOP and runs it
    // at this frame's bn::core::update() - so the takeover waits for a later frame.
    if(bn::dmg_music::playing()) bn::dmg_music::stop();

    s = state_t();                       // hUGE_init zeroes start_zero..end_zero
    s.song = song;
    s.ticks_per_row = song->tempo;
    s.order_count = uint8_t(*song->order_cnt / 2); // GB counts orders in 2-byte words
    s.current_wave = NO_WAVE;            // hUGE_init: force the first wave to load
    load_patterns(0);
    s.playing = true;
    s.play_frame = sys_time;
    huge_ticks = 0;
}

void huge_stop(void)
{
    if(!s.playing) return;
    s.playing = false;
    if(s.took_over)
    {
        reg(SOUND1CNT_H) = 0;
        reg(SOUND1CNT_X) = 0x8000;
        reg(SOUND2CNT_L) = 0;
        reg(SOUND2CNT_H) = 0x8000;
        reg(SOUND3CNT_L) = 0;
        reg(SOUND4CNT_L) = 0;
        reg(SOUND4CNT_H) = 0x8000;
    }
}

int huge_playing(void) { return s.playing ? 1 : 0; }

void huge_set_position(uint8_t pattern)
{
    // hUGE_set_position is fx_pos_jump entered with A = 0: at the end of the current row,
    // jump to order `pattern - 1` (one-based, 0 = just the next order), row 0. GBVM's
    // music_setpos passes only the pattern - its row operand is ignored on hUGE too.
    if(!s.playing) return;
    if(s.row_break == 0) s.row_break = 1;
    s.next_order = pattern;
}

void huge_set_master_volume(uint8_t nr50)
{
    // VM_SOUND_MASTERVOL is a raw NR50 write on the GB. It persists; while the player owns
    // the PSG it applies at once, otherwise at the next takeover.
    master_nr50 = nr50;
    if(s.playing && s.took_over)
    {
        s.nr[NR50 - NR10] = nr50;
        reg8(IO + gba_offset[NR50 - NR10]) = nr50;
    }
}

void huge_update(void)
{
    if(!s.playing) return;

    if(!s.took_over)
    {
        if(sys_time == s.play_frame) return; // the queued gbt stop has not run yet
        take_over_psg();
        s.took_over = true;
    }

    // GB Studio drives hUGE from the timer interrupt: TAC 0x07 (16384 Hz) with TMA 0xC0
    // overflows at 256 Hz, and music_play_isr runs the driver on every 4th - exactly 64 Hz.
    // The GBA frame is 59.7275 Hz, so ticking once per frame would play every song ~6.7%
    // slow. Accumulate instead, in GBA clock cycles (16777216 Hz, 280896 per frame), which
    // makes the long-run rate exactly 64 Hz; some frames run two ticks.
    constexpr uint32_t CYCLES_PER_FRAME = 280896;
    constexpr uint32_t CYCLES_PER_SECOND = 16777216;
    constexpr uint32_t TICKS_PER_SECOND = 64;
    s.tick_accum += CYCLES_PER_FRAME * TICKS_PER_SECOND;
    while(s.tick_accum >= CYCLES_PER_SECOND)
    {
        s.tick_accum -= CYCLES_PER_SECOND;
        dosound();
        ++huge_ticks;
    }
}

} // extern "C"
