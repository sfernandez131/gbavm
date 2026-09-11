// gbavm - GBA Studio engine
//
// hUGE player (M14): plays GB Studio's native .uge music on the GBA's Game Boy PSG
// channels. A C++ port of hUGEDriver's player logic (see src/huge_player.cpp).

#ifndef GBAVM_HUGE_PLAYER_H
#define GBAVM_HUGE_PLAYER_H

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

// Call once per frame. Runs the driver at GB Studio's 64 Hz tick rate, not the frame rate.
void huge_update(void);

#ifdef __cplusplus
}
#endif

#endif
