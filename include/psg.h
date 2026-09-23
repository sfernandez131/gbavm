// gbavm - GBA Studio engine
//
// The GBA's Game Boy PSG, as GB Studio's sound code sees it: GB register bytes (NR10..NR51),
// read back through the GB's read masks. Implemented in src/huge_player.cpp, which owns the
// register shadow; shared with the SFX player (src/psg_sfx.cpp).

#ifndef GBAVM_PSG_H
#define GBAVM_PSG_H

#include <stdint.h>

#ifdef __cplusplus
namespace psg
{
    // One GB register write (the low byte of its 0xFFxx address). NR52 and 0xFF27/28 are
    // ignored: NR52 would also gate the GBA's DirectSound.
    void write(uint8_t nr, uint8_t value);
    // What a DMG would read back: the last byte written, write-only bits as 1.
    uint8_t read(uint8_t nr);
    // 16 wave bytes into wave RAM, via the GBA's bank dance. Leaves CH3 stopped.
    void load_wave_ram(const uint8_t* src);
    // Route the PSG out (NR51, NR50) at full strength.
    void route_output();
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

// GB Studio's PSG sound effects (M14f): .vgm, FX Hammer, and the Play Tone / Beep / Crash
// events, all compiled to gbvm's sfx_player stream. A port of gbvm's music_manager
// sound-effect path and sfx_play_isr.

// VM_SFX_PLAY: start a sound effect (music_play_sfx). `mute_mask` is the music channels
// it borrows (bit 0 = CH1 .. bit 3 = CH4); a lower `priority` than the playing effect's
// is ignored.
void psg_sfx_play(const uint8_t* data, uint8_t mute_mask, uint8_t priority);
// VM_MUSIC_MUTE: mute music channels (vm_music_mute).
void psg_music_mute(uint8_t channels);
// VM_MUSIC_PLAY clears that mute first (vm_music_play).
void psg_music_unmute(void);
// 1 while an effect is playing.
int psg_sfx_active(void);
// Once a frame: run the effect at gbvm's 256 Hz.
void psg_sfx_update(void);

#ifdef __cplusplus
}
#endif

#endif
