// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Slice 2 milestone (Task 4): exercise the ported control-flow opcodes on GBA.
// The hand-assembled script runs a counting LOOP (conditional 32-bit jump via
// IF_CONST) and then a CALL into a subroutine that returns (RET) - validating the
// separate return stack. Result is shown as GREEN (pass) / RED (fail).
//
//   SET_CONST g0 = 0
// LOOP_TOP:
//   RPN       g0 = g0 + 1
//   IF_CONST  if g0 < 10 -> LOOP_TOP
//   CALL      SUBR
//   STOP
// SUBR:
//   SET_CONST g1 = 99
//   RET
//
// Expected afterwards: g0 == 10, g1 == 99.

#include "bn_core.h"
#include "bn_color.h"
#include "bn_bg_palettes.h"
#include "bn_log.h"

extern "C" {
#include "vm.h"
}

namespace
{
    // Mutable bytecode blob. The two code-target pointer fields (the 4-byte runs of
    // 0x00) are patched at runtime, since a plain byte array can't hold the address
    // of a forward label. The ported compiler will emit these as linker-resolved &labels.
    alignas(4) uint8_t script[] = {
        /* [0]  */ 0x14, 0x00,0x00, 0x00,0x00,       // SET_CONST g0 = 0
        /* [5]  LOOP_TOP: */
        /* [5]  */ 0x15,                             // RPN g0 = g0 + 1:
        /* [6]  */     0xFD, 0x00,0x00,              //   REF   g0
        /* [9]  */     0xFE, 0x01,0x00,              //   INT16 1
        /* [12] */     0x0A,                         //   ADD
        /* [13] */     0xFB, 0x00,0x00,              //   REF_SET g0
        /* [16] */     0x00,                         //   end
        /* [17] */ 0x1A, 0x02, 0x00,0x00, 0x0A,0x00, // IF_CONST g0 (LT) 10 ...
        /* [23] */     0x00,0x00,0x00,0x00,          //   ... -> LOOP_TOP  (patched)
        /* [27] */     0x00,                         //   n = 0
        /* [28] */ 0x04, 0x00,0x00,0x00,0x00,        // CALL SUBR          (patched)
        /* [33] */ 0x00,                             // STOP
        /* [34] SUBR: */
        /* [34] */ 0x14, 0x01,0x00, 0x63,0x00,       // SET_CONST g1 = 99
        /* [39] */ 0x05, 0x00                        // RET 0
    };

    constexpr int OFF_LOOP_TOP  = 5;
    constexpr int OFF_SUBR      = 34;
    constexpr int PATCH_IF_PC   = 23;   // 4-byte target field inside IF_CONST
    constexpr int PATCH_CALL_PC = 29;   // 4-byte target field inside CALL

    void write_ptr(uint8_t* dst, const void* p)
    {
        uintptr_t v = reinterpret_cast<uintptr_t>(p);
        dst[0] = uint8_t(v);
        dst[1] = uint8_t(v >> 8);
        dst[2] = uint8_t(v >> 16);
        dst[3] = uint8_t(v >> 24);
    }
}

int main()
{
    bn::core::init();

    write_ptr(&script[PATCH_IF_PC],   &script[OFF_LOOP_TOP]);
    write_ptr(&script[PATCH_CALL_PC], &script[OFF_SUBR]);

    script_runner_init(TRUE);
    script_execute(0, script, nullptr, 0);

    int guard = 0;
    while(script_runner_update() != RUNNER_DONE && guard++ < 100000)
    {
    }

    const uint16_t g0 = script_memory[0];   // expect 10 (loop body ran 10x)
    const uint16_t g1 = script_memory[1];   // expect 99 (set inside the called subroutine)
    const bool pass = (g0 == 10) && (g1 == 99) && (vm_last_unimplemented_op == 0);

    BN_LOG("gbavm VM slice2: g0=", g0, " g1=", g1,
           " unimpl=", vm_last_unimplemented_op, pass ? "  -> PASS" : "  -> FAIL");

    bn::bg_palettes::set_transparent_color(pass ? bn::color(0, 31, 0) : bn::color(31, 0, 0));

    while(true)
    {
        bn::core::update();
    }
}
