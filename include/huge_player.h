// gbavm - GBA Studio engine
//
// hUGE player (M14): plays GB Studio's native .uge music on the GBA's Game Boy PSG
// channels. A C++ port of hUGEDriver's player logic (see src/huge_player.cpp).

#ifndef GBAVM_HUGE_PLAYER_H
#define GBAVM_HUGE_PLAYER_H

#include <stdint.h>

#include "hUGEDriver.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start a song. If Butano's DMG (gbt) player is running it is stopped, and the PSG is
// taken over from the NEXT frame - bn::dmg_music::stop() is deferred to the frame commit,
// and a same-frame write would be clobbered by it (M14a).
void huge_play(const hUGESong_t* song);

// Stop the song and silence the four PSG channels.
void huge_stop(void);

// 1 while a song is playing.
int huge_playing(void);

// VM_MUSIC_SETPOS on hUGE: at the end of the current row, jump to order `pattern - 1`
// (one-based; 0 = the next order), row 0 - exactly as GBVM, which ignores the row.
void huge_set_position(uint8_t pattern);

// VM_SOUND_MASTERVOL on hUGE: a raw NR50 byte, persisting across songs as on the GB.
void huge_set_master_volume(uint8_t nr50);

// Call once per frame. Runs the driver at GB Studio's 64 Hz tick rate, not the frame rate.
void huge_update(void);

#ifdef __cplusplus
}
#endif

#endif
