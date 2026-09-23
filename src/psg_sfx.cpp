// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// PSG sound effects (M14f): GB Studio's .vgm and FX Hammer sounds, and the Play Tone /
// Beep / Crash events, on the GBA's Game Boy PSG. The editor compiles all of them to gbvm's
// sfx_player stream; this plays that stream exactly as gbvm does. Ported from
// appData/engine/gbvm/src/core/sfx_player.c (sfx_play_isr, SM83 asm) and the sound-effect
// half of music_manager.c/.h. (.wav sounds are not here: they play on DirectSound, M5b.)
//
// The stream is a series of steps. A step's first byte packs a frame skip (high nibble:
// how many more ticks this step lasts) and a count of channel entries (low nibble). Each
// entry starts with a byte whose low 3 bits pick the target and whose high 5 bits say which
// of that target's 5 registers follow:
//   0-3  CH1..CH4: NRx0..NRx4        4  the master registers, NR50..
//   5    16 bytes of wave RAM         6  the same, then start CH3 playing it
//   7    end of the effect
//
// gbvm runs this from the timer interrupt at 256 Hz - four times the music's rate. The GBA
// port runs from the frame loop and accumulates GBA clock cycles instead, like the hUGE
// player's 64 Hz, so the long-run rate is exact; some frames run five steps, most four.

#include "psg.h"
#include "huge_player.h"

#include <cstdint>

// For gba-studio's CI runtime test: 256 Hz ticks spent playing effects, and effects that
// played to their end. An effect's length in ticks is fixed by its data, so the test can
// assert it exactly. Plain globals, because GDB reads those reliably.
extern "C" {
uint16_t psg_sfx_ticks = 0;
uint8_t psg_sfx_done = 0;
}

namespace
{
    constexpr uint8_t NR10 = 0x10, NR12 = 0x12, NR14 = 0x14, NR22 = 0x17, NR24 = 0x19;
    constexpr uint8_t NR30 = 0x1A, NR31 = 0x1B, NR32 = 0x1C, NR33 = 0x1D, NR34 = 0x1E;
    constexpr uint8_t NR42 = 0x21, NR44 = 0x23, NR51 = 0x25;
    constexpr uint8_t SFX_CH_RETRIGGER = 0xC0;
    constexpr uint8_t MUTE_MASK_NONE = 0;
    constexpr uint8_t PRIORITY_MINIMAL = 0;

    // sfx_player.c
    const uint8_t* sfx_play_sample = nullptr;   // gbvm stops by bank; a null sample is the same
    uint8_t sfx_frame_skip = 0;

    // music_manager.c
    uint8_t music_mute_mask = MUTE_MASK_NONE;          // channels the playing effect borrows
    uint8_t music_global_mute_mask = MUTE_MASK_NONE;   // VM_MUSIC_MUTE
    uint8_t music_effective_mute = MUTE_MASK_NONE;     // what the music driver was last told
    uint8_t music_sfx_priority = PRIORITY_MINIMAL;

    uint32_t tick_accum = 0;

    uint8_t driver_set_mute_mask(uint8_t mask)
    {
        huge_set_mute_mask(mask);
        return mask;
    }

    // sfx_sound_cut_mask: silence the given channels, and route everything to both sides.
    uint8_t sfx_sound_cut_mask(uint8_t mask)
    {
        if(mask & 1) { psg::write(NR12, 0); psg::write(NR14, SFX_CH_RETRIGGER); }
        if(mask & 2) { psg::write(NR22, 0); psg::write(NR24, SFX_CH_RETRIGGER); }
        if(mask & 4) { psg::write(NR32, 0); }
        if(mask & 8) { psg::write(NR42, 0); psg::write(NR44, SFX_CH_RETRIGGER); }
        psg::write(NR51, 0xFF);
        return mask;
    }

    // sfx_play_isr: one 256 Hz step. Returns false when the effect has ended.
    bool sfx_play_isr()
    {
        const uint8_t* hl = sfx_play_sample;
        if(!hl) return false;
        if(sfx_frame_skip)
        {
            --sfx_frame_skip;
            return true;
        }

        const uint8_t header = *hl++;
        sfx_frame_skip = header >> 4;
        uint8_t d = header & 0x0F;                  // channel entries this step
        bool playing = true;
        while(d)
        {
            uint8_t b = *hl++;
            const uint8_t target = b & 0x07;
            if(target < 5)
            {
                // copy_reg x5: each set bit, from bit 7 down, writes the next register.
                uint8_t c = uint8_t(NR10 + target * 5);
                for(int r = 0; r < 5; ++r, ++c, b = uint8_t(b << 1))
                {
                    if(b & 0x80) psg::write(c, *hl++);
                }
            }
            else if(target == 7)
            {
                hl = nullptr;                       // terminator: the effect is over
                playing = false;
                break;
            }
            else
            {
                // A wave: mute CH3's routing, stop it, load the 16 bytes, and for target 6
                // start it with gbvm's fixed settings; then restore the routing.
                const uint8_t routing = psg::read(NR51);
                psg::write(NR51, routing & 0xBB);
                psg::write(NR30, 0x00);
                psg::load_wave_ram(hl);
                hl += 16;
                if(target == 6)
                {
                    psg::write(NR30, 0x80);
                    psg::write(NR31, 0xFE);         // length of wave
                    psg::write(NR32, 0x20);         // volume
                    psg::write(NR33, 0x00);         // low frequency bits are zero
                    psg::write(NR34, 0xC7);         // start; no loop; high frequency bits 111
                }
                psg::write(NR51, routing);
            }
            --d;
        }
        sfx_play_sample = hl;
        return playing;
    }

    // The sound-effect half of music_play_isr, run once per 256 Hz tick.
    void music_play_isr_sfx()
    {
        if(!sfx_play_sample) return;
        ++psg_sfx_ticks;
        if(music_effective_mute != (music_global_mute_mask | music_mute_mask))
        {
            music_effective_mute = driver_set_mute_mask(music_global_mute_mask | music_mute_mask);
        }
        if(!sfx_play_isr())
        {
            music_effective_mute = driver_set_mute_mask(music_global_mute_mask);
            huge_reset_wave();                      // the effect may have used wave RAM
            music_mute_mask = MUTE_MASK_NONE;
            music_sfx_priority = PRIORITY_MINIMAL;
            sfx_play_sample = nullptr;
            ++psg_sfx_done;
        }
    }
}

extern "C" {

// music_play_sfx
void psg_sfx_play(const uint8_t* data, uint8_t mute_mask, uint8_t priority)
{
    if(!data) return;
    if(priority < music_sfx_priority) return;
    sfx_play_sample = nullptr;
    music_sfx_priority = priority;
    sfx_sound_cut_mask(music_mute_mask);            // cut the effect this one replaces
    music_mute_mask = mute_mask;
    sfx_frame_skip = 0;
    sfx_play_sample = data;
    psg::route_output();                            // the PSG may not be routed out yet
}

// vm_music_mute
void psg_music_mute(uint8_t channels)
{
    music_global_mute_mask = channels;
    music_effective_mute = driver_set_mute_mask(sfx_sound_cut_mask(channels) | music_mute_mask);
}

// vm_music_play's first line
void psg_music_unmute(void)
{
    music_global_mute_mask = MUTE_MASK_NONE;
    music_effective_mute = driver_set_mute_mask(music_mute_mask);
}

int psg_sfx_active(void) { return sfx_play_sample ? 1 : 0; }

void psg_sfx_update(void)
{
    constexpr uint32_t CYCLES_PER_FRAME = 280896;
    constexpr uint32_t CYCLES_PER_SECOND = 16777216;
    constexpr uint32_t TICKS_PER_SECOND = 256;
    tick_accum += CYCLES_PER_FRAME * TICKS_PER_SECOND;
    while(tick_accum >= CYCLES_PER_SECOND)
    {
        tick_accum -= CYCLES_PER_SECOND;
        music_play_isr_sfx();
    }
}

} // extern "C"
