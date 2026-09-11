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
// (M14b: pulse 1-2; M14c1: wave 3, noise 4). Effects and subpattern tables are M14c2/c3;
// the state they need (table pointers, envelope, step width) is already carried.

#include "huge_player.h"

#include <cstdint>

#include "bn_dmg_music.h"

extern "C" {
#include "vm.h" // sys_time
}

// ---- verification trace (never enabled in a real build) ------------------------------------
// Every PSG write the player makes goes through psg() below. When HUGE_TRACE is defined -
// which only gba-studio's scripts/huge/add_probe.py does, in a throwaway checkout - each
// write is also logged here for the GDB stub to dump and scripts/huge/reference.py to diff.
// Compiled out otherwise: no RAM, no cost.
#ifdef HUGE_TRACE
extern "C" {
struct huge_trace_t
{
    uint16_t frame;
    uint16_t tick;
    uint8_t order;
    uint8_t row;
    uint8_t ch;
    uint8_t what;
    uint16_t value;
    uint16_t pad;
};
constexpr int HUGE_TRACE_MAX = 1024;
// 12 KB, so it has to live in EWRAM - IWRAM is 32 KB and a project's scripts already sit
// there. The EWRAM section name differs by toolchain (the Wonderful build passes
// BN_EWRAM_BSS_SECTION=".ewram_bss"; devkitARM uses ".sbss"), so take Butano's own macro
// with Butano's own fallback rather than hard-coding either.
#ifndef BN_EWRAM_BSS_SECTION
#define BN_EWRAM_BSS_SECTION ".sbss"
#endif
__attribute__((section(BN_EWRAM_BSS_SECTION))) huge_trace_t huge_trace[HUGE_TRACE_MAX];
uint16_t huge_trace_n = 0;
uint16_t huge_ticks = 0;
// The last wave written, read back through the emulated bus while its bank is still the
// writable one. Needed because a debugger cannot check wave RAM: mGBA's GDB stub reads it
// as zero and does not pass writes through, so this is the only way to see the bytes land.
uint8_t huge_wave_readback[16];
}
#endif

namespace
{
    // ---- GBA PSG registers -----------------------------------------------------------
    // The GB writes the PSG a byte at a time (NR10, NR11, ...). The GBA exposes the same
    // channels as 16-bit registers whose byte layouts line up exactly, so each GB pair
    // becomes one halfword write - the way gbt-player, the PSG player already proven on
    // this hardware, writes them:
    //   SOUND1CNT_L = NR10                 (sweep)
    //   SOUND1CNT_H = NR11 | NR12 << 8     (length/duty | envelope)
    //   SOUND1CNT_X = NR13 | NR14 << 8     (period | trigger/length-enable)
    //   SOUND2CNT_L = NR21 | NR22 << 8,    SOUND2CNT_H = NR23 | NR24 << 8
    //   SOUND3CNT_H = NR31 | NR32 << 8,    SOUND3CNT_X = NR33 | NR34 << 8
    //   SOUND4CNT_L = NR41 | NR42 << 8,    SOUND4CNT_H = NR43 | NR44 << 8
    //   SOUNDCNT_L high byte = NR51        (routing: same bit layout on both machines)
    // SOUND3CNT_L is the one that DIFFERS - see load_wave().
    volatile uint16_t& reg(uintptr_t addr) { return *reinterpret_cast<volatile uint16_t*>(addr); }
    constexpr uintptr_t SOUND1CNT_L = 0x04000060;
    constexpr uintptr_t SOUND1CNT_H = 0x04000062;
    constexpr uintptr_t SOUND1CNT_X = 0x04000064;
    constexpr uintptr_t SOUND2CNT_L = 0x04000068;
    constexpr uintptr_t SOUND2CNT_H = 0x0400006C;
    constexpr uintptr_t SOUND3CNT_L = 0x04000070;
    constexpr uintptr_t SOUND3CNT_H = 0x04000072;
    constexpr uintptr_t SOUND3CNT_X = 0x04000074;
    constexpr uintptr_t SOUND4CNT_L = 0x04000078;
    constexpr uintptr_t SOUND4CNT_H = 0x0400007C;
    constexpr uintptr_t SOUNDCNT_L  = 0x04000080;
    constexpr uintptr_t SOUNDCNT_H  = 0x04000082;
    constexpr uintptr_t WAVE_RAM    = 0x04000090;

    constexpr uint16_t CH3_ROUTING = 0x4400;  // NR51 bits 6 and 2, in SOUNDCNT_L's high byte

    // What a traced write was, for the verification diff. Plumbing (routing mutes, DAC
    // toggles) is deliberately NOT traced - only writes that carry musical state.
    enum write_t : uint8_t
    {
        W_SWEEP = 1,        // NR10
        W_LEN_ENV = 2,      // NRx1 | NRx2 << 8   (ch3: NR31 | NR32 << 8)
        W_NOTE = 3,         // NRx3 | NRx4 << 8   (ch4: NR43 | NR44 << 8)
        W_WAVE = 4,         // a wave loaded into wave RAM; value = wave index
    };

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
    constexpr uint8_t FX_TONEPORTA = 3;
    constexpr uint8_t NO_WAVE = 100;        // hUGE_NO_WAVE: forces the first wave to load

    // The per-channel block hUGEDriver keeps in WRAM (channel1..channel4).
    struct channel_t
    {
        uint16_t period = 0;                    // channel_period (ch4: the poly byte)
        uint8_t note = 0;                       // channel_note
        uint8_t highmask = 0;                   // NRx4 bits: 0x80 trigger, 0x40 length enable
        uint8_t envelope = 0;                   // envelopeN (effects, M14c2)
        const unsigned char* table = nullptr;   // subpattern (M14c3)
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
        uint8_t row_break = 0;                  // set by effects (M14c2); 1-based, 0 = none
        uint8_t next_order = 0;                 // likewise
        uint8_t mute_mask = 0;
        uint8_t current_wave = NO_WAVE;
        uint8_t step_width4 = 0;                // NR43 bit 3 (7-bit noise), kept for effects
        const unsigned char* pattern[4] = {};
        channel_t ch[4];
        bool playing = false;
        bool took_over = false;                 // PSG claimed (M14a: not in the stop's frame)
        uint16_t play_frame = 0;
        uint32_t tick_accum = 0;
    };

    state_t s;

    void trace(uint8_t ch, write_t what, uint16_t value)
    {
#ifdef HUGE_TRACE
        if(huge_trace_n < HUGE_TRACE_MAX)
        {
            huge_trace[huge_trace_n++] = { sys_time, huge_ticks, s.current_order, s.row,
                                           ch, what, value, 0 };
        }
#else
        (void)ch;
        (void)what;
        (void)value;
#endif
    }

    // Every PSG write that carries musical state goes through here, so it can be traced.
    void psg(uint8_t ch, write_t what, uintptr_t addr, uint16_t value)
    {
        reg(addr) = value;
        trace(ch, what, value);
    }

    bool muted(int c) { return (s.mute_mask >> c) & 1; }

    // The Z80 SWAP instruction: exchange the nibbles of a byte. The driver uses it as a
    // cheap multiply by 16, but it only IS one for values below 16 - so where an input can
    // be larger, the port has to swap, not shift, to stay faithful.
    uint8_t swap(uint8_t v) { return uint8_t((v << 4) | (v >> 4)); }

    // load_patterns: point each channel at its pattern for the given order entry.
    void load_patterns(uint8_t order)
    {
        const hUGESong_t& song = *s.song;
        s.pattern[0] = song.order1[order];
        s.pattern[1] = song.order2[order];
        s.pattern[2] = song.order3[order];
        s.pattern[3] = song.order4[order];
    }

    // get_current_row / get_current_note. Returns true only for a real note (not a rest),
    // exactly as the driver's carry flag does: any first byte >= LAST_NOTE is "no note".
    // `fx` is the instrument's low nibble over the effect code; `param` the effect param.
    bool current_note(int c, uint16_t& period, uint8_t& fx, uint8_t& param)
    {
        const unsigned char* cell = s.pattern[c] + s.row * 3;
        const uint8_t note = cell[0];
        fx = cell[1];
        param = cell[2];
        if(note >= LAST_NOTE) return false;
        s.ch[c].note = note;
        period = note_table[note];
        return true;
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

    // setup_instrument_pointer: instrument IDs are 1-based, 0 = none. The driver reads only
    // the upper nibble of the second byte; DN's bit 4 in the note byte is never consulted.
    uint8_t instrument_id(uint8_t fx) { return fx >> 4; }

    // ---- pulse channels 1-2 (M14b) ---------------------------------------------------

    // play_ch1_note / play_ch2_note: write the period with the highmask on top. With the
    // trigger bit set this restarts the note; without it (a note with no instrument) it
    // only retunes the channel, which is how hUGE plays legato.
    void play_duty_note(int c)
    {
        if(muted(c)) return;
        const channel_t& ch = s.ch[c];
        psg(uint8_t(c), W_NOTE, c == 0 ? SOUND1CNT_X : SOUND2CNT_H,
            uint16_t((ch.period & 0xFF) | ((ch.highmask | (ch.period >> 8)) << 8)));
    }

    // The tick-0 path of hUGE_dosound / process_ch2 for a pulse channel.
    void row_duty(int c)
    {
        uint16_t period = 0;
        uint8_t fx = 0, param = 0;
        const bool note = current_note(c, period, fx, param);
        (void)param;

        // `jr nc, .do_setvolN`: a rest skips the period AND the instrument - only the
        // effect runs. (Easy to misread: the instrument block sits after this branch.)
        if(note)
        {
            channel_t& ch = s.ch[c];
            if((fx & 0x0F) != FX_TONEPORTA) ch.period = period;

            const uint8_t id = instrument_id(fx);
            if(id == 0)
            {
                ch.highmask &= 0x7F;                    // no instrument: retune, don't retrigger
            }
            else if(!muted(c))
            {
                const hUGEDutyInstr_t& in = s.song->duty_instruments[id - 1];
                if(c == 0)
                {
                    psg(0, W_SWEEP, SOUND1CNT_L, in.sweep);
                    psg(0, W_LEN_ENV, SOUND1CNT_H, uint16_t(in.len_duty | (in.envelope << 8)));
                }
                else
                {
                    psg(1, W_LEN_ENV, SOUND2CNT_L, uint16_t(in.len_duty | (in.envelope << 8)));
                }
                ch.table = in.subpattern;
                ch.table_row = 0;
                ch.highmask = in.highmask;
            }
            // A muted channel with an instrument leaves highmask untouched (checkMute
            // jumps past .write_maskN).
        }

        // do_effect: M14c2.

        if(note) play_duty_note(c);

        // do_table: M14c3.
    }

    // ---- wave channel 3 (M14c1) ------------------------------------------------------

    // update_ch3_waveform: copy one 16-byte wave into wave RAM.
    //
    // This is where a literal port would break. The GB toggles the channel with NR30 = 0
    // then NR30 = 0xFF (`cpl` of 0), and on the GB only bit 7 of NR30 means anything. On
    // the GBA the same register, SOUND3CNT_L, uses bit 6 to select a wave BANK and bit 5
    // for 64-sample mode - so 0xFF would switch banks and double the wave. And the GBA has
    // two wave-RAM banks, of which the CPU can only write the one that is NOT playing.
    //
    // So this follows gbt-player's GBA recipe instead: disable with bank 1 playing (making
    // bank 0 writable), write the 16 bytes, then enable playing bank 0 in 32-sample mode.
    // Wave bytes are high-nibble-first on both machines, so they copy straight through.
    // The routing mute around it is the driver's own, to avoid a click.
    void load_wave(uint8_t wave)
    {
        s.current_wave = wave;
        const unsigned char* src = s.song->waves + swap(wave); // `swap a / add [hl]`

        const uint16_t routing = reg(SOUNDCNT_L);
        reg(SOUNDCNT_L) = uint16_t(routing & ~CH3_ROUTING);
        reg(SOUND3CNT_L) = 0x40;                                // stop; play bank 1, write bank 0
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
        reg(SOUND3CNT_L) = 0x80;                                // play bank 0, 32 samples
        reg(SOUNDCNT_L) = routing;

        trace(2, W_WAVE, wave);
    }

    // play_ch3_note. The driver stops and restarts CH3 around the trigger because
    // retriggering while the DMG reads a wave byte corrupts wave RAM - a DMG bug the GBA
    // does not have, but harmless to keep. Restarting has to use 0x80 here, not the GB's
    // 0xFF, for the reason load_wave() explains.
    void play_wave_note()
    {
        if(muted(2)) return;
        const channel_t& ch = s.ch[2];
        const uint16_t routing = reg(SOUNDCNT_L);
        reg(SOUNDCNT_L) = uint16_t(routing & ~CH3_ROUTING);
        reg(SOUND3CNT_L) = 0x00;
        reg(SOUND3CNT_L) = 0x80;
        psg(2, W_NOTE, SOUND3CNT_X,
            uint16_t((ch.period & 0xFF) | ((ch.highmask | (ch.period >> 8)) << 8)));
        reg(SOUNDCNT_L) = routing;
    }

    // The tick-0 path of process_ch3.
    void row_wave()
    {
        uint16_t period = 0;
        uint8_t fx = 0, param = 0;
        const bool note = current_note(2, period, fx, param);
        (void)param;

        if(note)
        {
            channel_t& ch = s.ch[2];
            if((fx & 0x0F) != FX_TONEPORTA) ch.period = period;

            const uint8_t id = instrument_id(fx);
            if(id == 0)
            {
                ch.highmask &= 0x7F;
            }
            else if(!muted(2))
            {
                const hUGEWaveInstr_t& in = s.song->wave_instruments[id - 1];
                psg(2, W_LEN_ENV, SOUND3CNT_H, uint16_t(in.length | (in.volume << 8)));
                if(in.waveform != s.current_wave) load_wave(in.waveform);
                ch.table = in.subpattern;
                ch.table_row = 0;
                ch.highmask = in.highmask;
            }
        }

        // do_effect: M14c2.

        if(note) play_wave_note();

        // do_table: M14c3.
    }

    // ---- noise channel 4 (M14c1) -----------------------------------------------------

    // play_ch4_note: NR43 is the poly byte, NR44 the highmask (no period bits here).
    void play_noise_note()
    {
        if(muted(3)) return;
        const channel_t& ch = s.ch[3];
        psg(3, W_NOTE, SOUND4CNT_H, uint16_t((ch.period & 0xFF) | (ch.highmask << 8)));
    }

    // The tick-0 path of process_ch4. It reads the row directly rather than through
    // get_current_note, because CH4 plays a poly counter, not a period.
    void row_noise()
    {
        const unsigned char* cell = s.pattern[3] + s.row * 3;
        const uint8_t note_byte = cell[0];
        const uint8_t fx = cell[1];
        const bool note = note_byte < LAST_NOTE;

        if(note)
        {
            channel_t& ch = s.ch[3];
            ch.note = note_byte;
            ch.period = note_poly(note_byte);   // no toneporta check: CH4 cannot porta

            const uint8_t id = instrument_id(fx);
            if(id == 0)
            {
                // No instrument: the period stays WITHOUT the step width - the driver only
                // ORs step_width4 in on the instrument path.
                ch.highmask &= 0x7F;
            }
            else if(!muted(3))
            {
                const hUGENoiseInstr_t& in = s.song->noise_instruments[id - 1];
                // The noise instrument's "highmask" byte packs three fields: length in
                // bits 0-5, length-enable in bit 6, and 7-bit (short) mode in bit 7.
                psg(3, W_LEN_ENV, SOUND4CNT_L,
                    uint16_t((in.highmask & 0x3F) | (in.envelope << 8)));
                ch.table = in.subpattern;
                ch.table_row = 0;
                s.step_width4 = swap(uint8_t(in.highmask & 0x80));  // bit 7 -> NR43 bit 3
                ch.period = uint16_t(ch.period | s.step_width4);
                ch.highmask = uint8_t((in.highmask & 0x40) | 0x80);
            }
        }

        // do_effect: M14c2.

        if(note) play_noise_note();

        // do_table: M14c3.
    }

    // ---- the tick --------------------------------------------------------------------

    // tick_time: advance the tick, and on the last tick of a row, the row and order.
    void tick_time()
    {
        ++s.counter;
        if(++s.tick != s.ticks_per_row) return;
        s.tick = 0;

        uint8_t order_to_load;
        uint8_t start_row;
        if(s.row_break)
        {
            // A row/pattern break (effects, M14c2). Both counters are stored one-based so
            // zero can mean "not set".
            start_row = uint8_t(s.row_break - 1);
            s.row_break = 0;
            if(s.next_order)
            {
                order_to_load = uint8_t(s.next_order - 1);
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
            row_duty(0);
            row_duty(1);
            row_wave();
            row_noise();
        }
        // else process_effects: M14c2.
        tick_time();
    }

    // M14a: claim the whole PSG. Butano's gbt player leaves channels RUNNING when it
    // stops (SOUNDCNT_X still flagged ch2/ch3 after a stop), so silence all four rather
    // than assume they are quiet, then route them all to both speakers the way GB Studio
    // sets NR51, and run the PSG at full strength.
    void take_over_psg()
    {
        reg(SOUND1CNT_H) = 0;            // envelope volume 0
        reg(SOUND1CNT_X) = 0x8000;       // restart so the silent envelope takes effect
        reg(SOUND2CNT_L) = 0;
        reg(SOUND2CNT_H) = 0x8000;
        reg(SOUND3CNT_L) = 0;            // wave channel stopped (load_wave restarts it)
        reg(SOUND4CNT_L) = 0;
        reg(SOUND4CNT_H) = 0x8000;
        reg(SOUNDCNT_L) = 0xFF77;        // all four channels L+R, PSG master volume 7/7
        reg(SOUNDCNT_H) = uint16_t((reg(SOUNDCNT_H) & ~3u) | 2u); // PSG output at 100%
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

    // One PSG, one owner. Butano's stop is DEFERRED - it queues DMG_MUSIC_STOP and runs it
    // at this frame's bn::core::update() - so the takeover waits for a later frame.
    if(bn::dmg_music::playing()) bn::dmg_music::stop();

    s = state_t();                       // hUGE_init zeroes start_zero..end_zero
    s.song = song;
    s.ticks_per_row = song->tempo;
    s.order_count = uint8_t(*song->order_cnt / 2); // GB counts orders in 2-byte words
    s.ch[0].envelope = 0xF0;            // hUGE_init: %11110000 for both pulse channels
    s.ch[1].envelope = 0xF0;
    s.current_wave = NO_WAVE;            // hUGE_init: force the first wave to load
    load_patterns(0);
    s.playing = true;
    s.play_frame = sys_time;
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
#ifdef HUGE_TRACE
        ++huge_ticks;
#endif
    }
}

} // extern "C"
