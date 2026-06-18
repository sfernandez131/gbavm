// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Task 5 / increment 5b-2: a d-pad-controllable sprite, driven entirely by VM
// bytecode. Each frame the script reads the joypad (INPUT_GET), updates the
// actor's X/Y by a branchless RPN expression -- dx = (RIGHT!=0) - (LEFT!=0),
// dy = (DOWN!=0) - (UP!=0), times the move speed -- pushes it to hardware
// (ACTOR_SET_POS), yields one frame (IDLE), and loops (JUMP).
//
// Branchless RPN keeps the only runtime-patched pointer the loop target.
// Heap layout: g0 = input bitmask; g1 = actor ID; g2 = X (subpx); g3 = Y (subpx).
// Start position: screen center (120,80 px) == subpx (0x0780, 0x0500).

#include "bn_core.h"
#include "hw.h"

extern "C" {
#include "vm.h"
}

namespace
{
    // RPN sub-op bytes (signed): REF=-3=0xFD, INT16=-2=0xFE, REF_SET=-5=0xFB,
    // B_AND=0x0F, NE=0x06, SUB=0x0B, MUL=0x0C, ADD=0x0A, terminator=0x00.
    uint8_t script[] = {
        /* [0]  */ 0x14, 0x01,0x00, 0x00,0x00,   // SET_CONST g1 = 0        (actor ID)
        /* [5]  */ 0x14, 0x02,0x00, 0x80,0x07,   // SET_CONST g2 = 0x0780   (X = 120 px)
        /* [10] */ 0x14, 0x03,0x00, 0x00,0x05,   // SET_CONST g3 = 0x0500   (Y = 80 px)
        /* [15] */ 0x31, 0x00,0x00,              // ACTOR_ACTIVATE 0
        // LOOP_TOP (offset 18):
        /* [18] */ 0x54, 0x00, 0x00,0x00,        // INPUT_GET joy0 -> g0   (joyid=0, idx=g0)
        // RPN: g2 += ((g0 & RIGHT)!=0) - ((g0 & LEFT)!=0)) * 16
        /* [22] */ 0x15,
        /* [23] */     0xFD, 0x02,0x00,          //   REF g2
        /* [26] */     0xFD, 0x00,0x00,          //   REF g0
        /* [29] */     0xFE, 0x10,0x00,          //   INT16 0x10 (RIGHT)
        /* [32] */     0x0F,                     //   B_AND
        /* [33] */     0xFE, 0x00,0x00,          //   INT16 0
        /* [36] */     0x06,                     //   NE        -> right01
        /* [37] */     0xFD, 0x00,0x00,          //   REF g0
        /* [40] */     0xFE, 0x20,0x00,          //   INT16 0x20 (LEFT)
        /* [43] */     0x0F,                     //   B_AND
        /* [44] */     0xFE, 0x00,0x00,          //   INT16 0
        /* [47] */     0x06,                     //   NE        -> left01
        /* [48] */     0x0B,                     //   SUB       -> dx
        /* [49] */     0xFE, 0x10,0x00,          //   INT16 16  (1 px = 16 subpx)
        /* [52] */     0x0C,                     //   MUL
        /* [53] */     0x0A,                     //   ADD
        /* [54] */     0xFB, 0x02,0x00,          //   REF_SET g2
        /* [57] */     0x00,                     //   end
        // RPN: g3 += ((g0 & DOWN)!=0) - ((g0 & UP)!=0)) * 16
        /* [58] */ 0x15,
        /* [59] */     0xFD, 0x03,0x00,          //   REF g3
        /* [62] */     0xFD, 0x00,0x00,          //   REF g0
        /* [65] */     0xFE, 0x80,0x00,          //   INT16 0x80 (DOWN)
        /* [68] */     0x0F,                     //   B_AND
        /* [69] */     0xFE, 0x00,0x00,          //   INT16 0
        /* [72] */     0x06,                     //   NE        -> down01
        /* [73] */     0xFD, 0x00,0x00,          //   REF g0
        /* [76] */     0xFE, 0x40,0x00,          //   INT16 0x40 (UP)
        /* [79] */     0x0F,                     //   B_AND
        /* [80] */     0xFE, 0x00,0x00,          //   INT16 0
        /* [83] */     0x06,                     //   NE        -> up01
        /* [84] */     0x0B,                     //   SUB       -> dy
        /* [85] */     0xFE, 0x10,0x00,          //   INT16 16
        /* [88] */     0x0C,                     //   MUL
        /* [89] */     0x0A,                     //   ADD
        /* [90] */     0xFB, 0x03,0x00,          //   REF_SET g3
        /* [93] */     0x00,                     //   end
        /* [94] */ 0x35, 0x01,0x00,              // ACTOR_SET_POS g1
        /* [97] */ 0x18,                         // IDLE (yield one frame)
        /* [98] */ 0x09, 0x00,0x00,0x00,0x00     // JUMP LOOP_TOP   (target patched below)
    };

    constexpr int OFF_LOOP_TOP = 18;
    constexpr int PATCH_JUMP   = 99;   // 4-byte target field inside the final JUMP

    void write_ptr(uint8_t* dst, const void* p)
    {
        uintptr_t v = reinterpret_cast<uintptr_t>(p);
        dst[0] = uint8_t(v); dst[1] = uint8_t(v >> 8);
        dst[2] = uint8_t(v >> 16); dst[3] = uint8_t(v >> 24);
    }
}

int main()
{
    bn::core::init();
    hw_init();

    write_ptr(&script[PATCH_JUMP], &script[OFF_LOOP_TOP]);

    script_runner_init(TRUE);
    script_execute(0, script, nullptr, 0);

    while(true)
    {
        script_runner_update();   // one loop iteration per frame (IDLE yields)
        hw_render();              // push actor state into bn::sprite_ptr
        sys_time++;
        bn::core::update();
    }
}
