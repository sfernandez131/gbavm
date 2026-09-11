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
// M14b scope: the tick/row/order machinery and note playback on the two pulse channels.
// Effects, subpattern tables and the wave/noise channels are M14c; the state they need
// (shadows, table pointers, envelope) is already kept here so they slot in.

#include "huge_player.h"

#include <cstdint>

#include "bn_dmg_music.h"

extern "C" {
#include "vm.h" // sys_time
}

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
    //   SOUND2CNT_L = NR21 | NR22 << 8,  SOUND2CNT_H = NR23 | NR24 << 8
    volatile uint16_t& reg(uintptr_t addr) { return *reinterpret_cast<volatile uint16_t*>(addr); }
    constexpr uintptr_t SOUND1CNT_L = 0x04000060;
    constexpr uintptr_t SOUND1CNT_H = 0x04000062;
    constexpr uintptr_t SOUND1CNT_X = 0x04000064;
    constexpr uintptr_t SOUND2CNT_L = 0x04000068;
    constexpr uintptr_t SOUND2CNT_H = 0x0400006C;
    constexpr uintptr_t SOUND3CNT_L = 0x04000070;
    constexpr uintptr_t SOUND4CNT_L = 0x04000078;
    constexpr uintptr_t SOUND4CNT_H = 0x0400007C;
    constexpr uintptr_t SOUNDCNT_L  = 0x04000080;
    constexpr uintptr_t SOUNDCNT_H  = 0x04000082;

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

    // The per-channel block hUGEDriver keeps in WRAM (channel1..channel4).
    struct channel_t
    {
        uint16_t period = 0;                    // channel_period
        uint8_t note = 0;                       // channel_note
        uint8_t highmask = 0;                   // NRx4 bits: 0x80 trigger, 0x40 length enable
        uint8_t envelope = 0;                   // envelopeN (effects, M14c)
        const unsigned char* table = nullptr;   // subpattern (M14c)
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
        uint8_t row_break = 0;                  // set by effects (M14c); 1-based, 0 = none
        uint8_t next_order = 0;                 // likewise
        uint8_t mute_mask = 0;
        const unsigned char* pattern[4] = {};
        channel_t ch[4];
        bool playing = false;
        bool took_over = false;                 // PSG claimed (M14a: not in the stop's frame)
        uint16_t play_frame = 0;
        uint32_t tick_accum = 0;
    };

    state_t s;

    bool muted(int c) { return (s.mute_mask >> c) & 1; }

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

    // play_ch1_note / play_ch2_note: write the period with the highmask on top. With the
    // trigger bit set this restarts the note; without it (a note with no instrument) it
    // only retunes the channel, which is how hUGE plays legato.
    void play_duty_note(int c)
    {
        if(muted(c)) return;
        const channel_t& ch = s.ch[c];
        const uint16_t x = uint16_t((ch.period & 0xFF) | ((ch.highmask | (ch.period >> 8)) << 8));
        reg(c == 0 ? SOUND1CNT_X : SOUND2CNT_H) = x;
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

            // setup_instrument_pointer: instrument IDs are 1-based, 0 = none. The driver
            // reads only the upper nibble of the second byte; DN's bit 4 in the note byte
            // is never consulted.
            const uint8_t id = fx >> 4;
            if(id == 0)
            {
                ch.highmask &= 0x7F;                    // no instrument: retune, don't retrigger
            }
            else if(!muted(c))
            {
                const hUGEDutyInstr_t& in = s.song->duty_instruments[id - 1];
                if(c == 0)
                {
                    reg(SOUND1CNT_L) = in.sweep;
                    reg(SOUND1CNT_H) = uint16_t(in.len_duty | (in.envelope << 8));
                }
                else
                {
                    reg(SOUND2CNT_L) = uint16_t(in.len_duty | (in.envelope << 8));
                }
                ch.table = in.subpattern;
                ch.table_row = 0;
                ch.highmask = in.highmask;
            }
            // A muted channel with an instrument leaves highmask untouched (checkMute
            // jumps past .write_maskN).
        }

        // do_effect: M14c.

        if(note) play_duty_note(c);

        // do_table: M14c.
    }

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
            // A row/pattern break (effects, M14c). Both counters are stored one-based so
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
            // process_ch3 / process_ch4: M14c.
        }
        // else process_effects: M14c.
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
        reg(SOUND3CNT_L) = 0;            // wave channel DAC off
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
    }
}

} // extern "C"
