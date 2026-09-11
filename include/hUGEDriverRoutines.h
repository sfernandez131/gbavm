// gbavm - GBA Studio engine
//
// The routine table every exported song references (`routines` in hUGESong_t). The
// hUGE "call routine" effect raises one of these from pattern data; they are what GB
// Studio's VM_MUSIC_ROUTINE ultimately listens to. Matches the header GB Studio vendors
// (appData/engine/gbvm/include/hUGEDriverRoutines.h), minus the SDCC NONBANKED keyword.

#ifndef GBAVM_HUGEDRIVER_ROUTINES_H
#define GBAVM_HUGEDRIVER_ROUTINES_H

#include "hUGEDriver.h"

#ifdef __cplusplus
extern "C" {
#endif

// Defined by the hUGE player (src/huge_player.cpp).
void hUGETrackerRoutine(unsigned char tick, unsigned int param);

#ifdef __cplusplus
}
#endif

// `static` on purpose: each exported song is its own translation unit and references this
// table by name, exactly as the GB build does.
static const hUGERoutine_t routines[] = {
    hUGETrackerRoutine, hUGETrackerRoutine, hUGETrackerRoutine, hUGETrackerRoutine,
    hUGETrackerRoutine, hUGETrackerRoutine, hUGETrackerRoutine, hUGETrackerRoutine,
    hUGETrackerRoutine, hUGETrackerRoutine, hUGETrackerRoutine, hUGETrackerRoutine,
    hUGETrackerRoutine, hUGETrackerRoutine, hUGETrackerRoutine, hUGETrackerRoutine
};

#endif
