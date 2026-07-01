// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
//
// Portions of this file are derived from GBVM (https://github.com/chrismaltby/gbvm),
// Copyright (c) 2020 Toxa, used under the MIT License. See the LICENSE file.
//
// gbavm VM core implementation - ported from GB Studio's GBVM src/core/vm.c.
//
// Faithful port of the system opcodes (data, arithmetic, control flow) and the
// thread scheduler. Differences from the original GB/Z80 version:
//   * SDCC qualifiers (OLDCALL/BANKED/NONBANKED/NAKED) removed; handlers are static.
//   * ROM bank-switching removed - the GBA has a flat address space.
//   * Code targets are native 32-bit pointers; return addresses use a separate
//     per-thread return stack (SCRIPT_CTX.ret_sp) instead of the 16-bit data stack.
//   * VM_STEP (hand-written Z80 asm in the original) is a portable C dispatch loop.
//   * Helper math (isqrt / rng) is local; atan2 is a TODO stub.
//
// Not yet ported: vm_asm (inline GB machine code - irrelevant on ARM) and the
// VM_OP_REF_MEM* raw-pointer memory ops in RPN (need 32-bit addressing; stubbed).

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>

#include "vm.h"
#include "hw.h"

// ---- shared state -------------------------------------------------------------

UWORD script_memory[VM_HEAP_SIZE + (VM_MAX_CONTEXTS * VM_CONTEXT_STACK_SIZE)];

static SCRIPT_CTX CTXS[VM_MAX_CONTEXTS];
static const UBYTE * ret_stacks[VM_MAX_CONTEXTS][VM_RET_STACK_DEPTH];
static SCRIPT_CTX * first_ctx;
static SCRIPT_CTX * free_ctxs;
static SCRIPT_CTX * old_executing_ctx;
static SCRIPT_CTX * executing_ctx;

static UBYTE vm_lock_state;
static UBYTE vm_loaded_state;
static UBYTE vm_exception_code;
static UWORD vm_exception_param; // payload of the pending exception (e.g. scene index)

// Scene stack hooks (gba_scene.cpp). pop/pop_all return the scene index to load.
extern void gba_scene_push(void);
extern UWORD gba_scene_pop(void);
extern UWORD gba_scene_pop_all(void);
extern int gba_save_peek(int slot);   // M6a: 1 if a valid SRAM save exists
extern void gba_save_clear(int slot); // M6a: invalidate the SRAM save

UBYTE vm_last_unimplemented_op = 0;
UWORD sys_time = 0;

// Timers (M6f): VM_TIMER_PREPARE stores a slot's script; VM_TIMER_SET arms it to fire that
// script every `interval` ticks (1 tick = TIMER_CYCLES frames). timers_update() (main loop)
// counts down + fires. Timers are per-scene - script_runner_init clears them on scene load.
#define TIMER_CYCLES 16
#define VM_MAX_TIMERS 8
typedef struct { UBYTE bank; UBYTE * pc; UBYTE interval; UWORD countdown; UBYTE active; } VM_TIMER;
static VM_TIMER vm_timers[VM_MAX_TIMERS];

#define EXCEPTION_CODE_NONE 0

// Resolve operand index to a typed pointer (negative = stack-relative, positive = global).
#define I16P(idx) ((INT16 *)(((idx) < 0) ? THIS->stack_ptr + (idx) : script_memory + (idx)))
#define U16P(idx) ((UWORD *)(((idx) < 0) ? THIS->stack_ptr + (idx) : script_memory + (idx)))

// Resolve a VM operand index to a pointer (used by the hardware bridge in hw.cpp).
void * vm_resolve_ref(SCRIPT_CTX * THIS, INT16 idx) { return VM_REF_TO_PTR(idx); }

// ---- local math helpers (replacements for GBDK's math.h / rand.h) -------------

// PRNG ported byte-for-byte from GBDK sm83.lib rand.o (the sequence GB Studio uses):
// new = old*17 + 0x5C93 (mod 2^16); returns the post-update state.
static UWORD vm_rng_state = 0;
static void  vm_initrand(UWORD seed) { vm_rng_state = seed; }
static UWORD vm_randw(void) { vm_rng_state = (UWORD)(vm_rng_state * 17u + 0x5C93u); return vm_rng_state; }
// Boot-time seed hook (GBA has no DIV register; main.cpp seeds from a hardware timer).
void vm_boot_seed(UWORD s) { vm_initrand(s); }
static UWORD vm_isqrt(UWORD x) {
    UWORD res = 0, bit = (UWORD)1 << 14;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= res + bit) { x -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return res;
}
// Sine table + SIN/COS + scale ops, ported verbatim from GB Studio
// (appData/engine/gbvm/include/sincos.h + src/core/vm_math.c). Angle is 8-bit BRADS.
static const INT8 sine_wave[256] = {
0,3,6,9,12,16,19,22,25,28,31,34,37,40,43,46,49,51,54,57,60,63,65,68,71,73,76,78,81,83,85,88,
90,92,94,96,98,100,102,104,106,107,109,111,112,113,115,116,117,118,120,121,122,122,123,124,125,125,126,126,126,127,127,127,
127,127,127,127,126,126,126,125,125,124,123,122,122,121,120,118,117,116,115,113,112,111,109,107,106,104,102,100,98,96,94,92,
90,88,85,83,81,78,76,73,71,68,65,63,60,57,54,51,49,46,43,40,37,34,31,28,25,22,19,16,12,9,6,3,
0,-3,-6,-9,-12,-16,-19,-22,-25,-28,-31,-34,-37,-40,-43,-46,-49,-51,-54,-57,-60,-63,-65,-68,-71,-73,-76,-78,-81,-83,-85,-88,
-90,-92,-94,-96,-98,-100,-102,-104,-106,-107,-109,-111,-112,-113,-115,-116,-117,-118,-120,-121,-122,-122,-123,-124,-125,-125,-126,-126,-126,-127,-127,-127,
-127,-127,-127,-127,-126,-126,-126,-125,-125,-124,-123,-122,-122,-121,-120,-118,-117,-116,-115,-113,-112,-111,-109,-107,-106,-104,-102,-100,-98,-96,-94,-92,
-90,-88,-85,-83,-81,-78,-76,-73,-71,-68,-65,-63,-60,-57,-54,-51,-49,-46,-43,-40,-37,-34,-31,-28,-25,-22,-19,-16,-12,-9,-6,-3};
static inline INT8 SIN(UBYTE a){ return sine_wave[a]; }
static inline INT8 COS(UBYTE a){ return sine_wave[(UBYTE)(a + 64u)]; }
static void vm_sin_scale(SCRIPT_CTX * THIS, INT16 idx, INT16 idx_angle, UBYTE accuracy) {
    INT16 * res = I16P(idx); INT16 * angle = I16P(idx_angle);
    *res = (INT16)((*res * (SIN((UBYTE)*angle) >> (7 - accuracy))) >> accuracy);
}
static void vm_cos_scale(SCRIPT_CTX * THIS, INT16 idx, INT16 idx_angle, UBYTE accuracy) {
    INT16 * res = I16P(idx); INT16 * angle = I16P(idx_angle);
    *res = (INT16)((*res * (COS((UBYTE)*angle) >> (7 - accuracy))) >> accuracy);
}

// atan2 ported from GB Studio (src/core/math_atan2.c): CLAMP + quadrant table.
// Returns an 8-bit BRADS angle (0..255); the UBYTE intermediate gives GB's wrap.
static const UBYTE atan2_table[20][18] = {
{64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64},
{0,32,45,51,54,56,57,58,59,59,60,60,61,61,61,61,61,62},
{0,19,32,40,45,48,51,53,54,55,56,57,57,58,58,59,59,59},
{0,13,24,32,38,42,45,48,49,51,52,53,54,55,55,56,56,57},
{0,10,19,26,32,37,40,43,45,47,48,50,51,52,53,53,54,55},
{0,8,16,22,27,32,36,39,41,43,45,47,48,49,50,51,52,52},
{0,7,13,19,24,28,32,35,38,40,42,44,45,46,48,48,49,50},
{0,6,11,16,21,25,29,32,35,37,39,41,42,44,45,46,47,48},
{0,5,10,15,19,23,26,29,32,34,37,38,40,42,43,44,45,46},
{0,5,9,13,17,21,24,27,30,32,34,36,38,39,41,42,43,44},
{0,4,8,12,16,19,22,25,27,30,32,34,36,37,39,40,41,42},
{0,4,7,11,14,17,20,23,26,28,30,32,34,35,37,38,39,41},
{0,3,7,10,13,16,19,22,24,26,28,30,32,34,35,37,38,39},
{0,3,6,9,12,15,18,20,22,25,27,29,30,32,34,35,36,37},
{0,3,6,9,11,14,16,19,21,23,25,27,29,30,32,33,35,36},
{0,3,5,8,11,13,16,18,20,22,24,26,27,29,31,32,33,35},
{0,3,5,8,10,12,15,17,19,21,23,25,26,28,29,31,32,33},
{0,2,5,7,9,12,14,16,18,20,22,23,25,27,28,29,31,32},
{0,2,5,7,9,11,13,15,17,19,21,22,24,25,27,28,30,31},
{0,2,4,6,8,10,12,14,16,18,20,21,23,24,26,27,29,30}};
#define ATAN2_CLAMP(v,lo,hi) ((v)<(lo)?(lo):((v)>(hi)?(hi):(v)))
static WORD vm_atan2(WORD y, WORD x) {
    x = ATAN2_CLAMP(x,-19,19); y = ATAN2_CLAMP(y,-17,17);
    UBYTE r;
    if (x>=0 && y<=0)      r = (UBYTE)(64  - atan2_table[x][-y]);
    else if (x>=0 && y>=0) r = (UBYTE)(64  + atan2_table[x][y]);
    else if (x<=0 && y>=0) r = (UBYTE)(192 - atan2_table[-x][y]);
    else                   r = (UBYTE)(192 + atan2_table[-x][-y]);
    return (WORD)r;
}

// alignment-safe little-endian reads from the (byte-aligned) bytecode stream
static inline INT16 rd_i16(const UBYTE * p) { return (INT16)((UWORD)p[0] | ((UWORD)p[1] << 8)); }
static inline UWORD rd_u16(const UBYTE * p) { return (UWORD)((UWORD)p[0] | ((UWORD)p[1] << 8)); }
static inline uint32_t rd_u32(const UBYTE * p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---- control-flow opcodes -----------------------------------------------------

static void vm_jump(SCRIPT_CTX * THIS, UBYTE * pc) { THIS->PC = pc; }

static void vm_call(SCRIPT_CTX * THIS, UBYTE * pc) {
    *(THIS->ret_sp++) = THIS->PC;   // push return address onto the return stack
    THIS->PC = pc;
}
static void vm_ret(SCRIPT_CTX * THIS, UBYTE n) {
    THIS->PC = *(--THIS->ret_sp);   // pop return address
    if (n) THIS->stack_ptr -= n;    // clean up n data words
}
// "far" calls are identical to near calls on the GBA (no banks); bank is ignored.
static void vm_call_far(SCRIPT_CTX * THIS, UBYTE bank, UBYTE * pc) {
    (void)bank;
    *(THIS->ret_sp++) = THIS->PC;
    THIS->PC = pc;
}
static void vm_ret_far(SCRIPT_CTX * THIS, UBYTE n) {
    THIS->PC = *(--THIS->ret_sp);
    if (n) THIS->stack_ptr -= n;
}

static void vm_loop(SCRIPT_CTX * THIS, INT16 idx, UBYTE * pc, UBYTE n) {
    UWORD * counter = U16P(idx);
    if (*counter) { THIS->PC = pc; (*counter)--; }
    else { if (n) THIS->stack_ptr -= n; }
}

static void vm_switch(SCRIPT_CTX * THIS, INT16 idx, UBYTE size, UBYTE n) {
    INT16 value = *I16P(idx);
    if (n) THIS->stack_ptr -= n;                 // dispose values on the data stack
    const UBYTE * t = THIS->PC;                   // jump table follows the instruction
    while (size) {
        INT16 cval = (INT16)((UWORD)t[0] | ((UWORD)t[1] << 8));
        const UBYTE * target = (const UBYTE *)(uintptr_t)
            ((uint32_t)t[2] | ((uint32_t)t[3] << 8) | ((uint32_t)t[4] << 16) | ((uint32_t)t[5] << 24));
        if (value == cval) { THIS->PC = target; return; }
        t += 6;                                   // entry = value(2) + target(4)
        size--;
    }
    THIS->PC = t;                                 // fall through past the table
}

static UBYTE vm_compare(UBYTE condition, INT16 A, INT16 B) {
    switch (condition) {
        case VM_OP_EQ: return A == B;
        case VM_OP_LT: return A <  B;
        case VM_OP_LE: return A <= B;
        case VM_OP_GT: return A >  B;
        case VM_OP_GE: return A >= B;
        case VM_OP_NE: return A != B;
        default:       break;
    }
    return FALSE;
}
static void vm_if(SCRIPT_CTX * THIS, UBYTE condition, INT16 idxA, INT16 idxB, UBYTE * pc, UBYTE n) {
    if (vm_compare(condition, *I16P(idxA), *I16P(idxB))) THIS->PC = pc;
    if (n) THIS->stack_ptr -= n;
}
static void vm_if_const(SCRIPT_CTX * THIS, UBYTE condition, INT16 idxA, INT16 B, UBYTE * pc, UBYTE n) {
    if (vm_compare(condition, *I16P(idxA), B)) THIS->PC = pc;
    if (n) THIS->stack_ptr -= n;
}

static void vm_join(SCRIPT_CTX * THIS, INT16 idx) {
    UWORD * A = U16P(idx);
    if (!(*A >> 8)) { THIS->PC -= (INSTRUCTION_SIZE + 2); THIS->waitable = TRUE; }
}

static void vm_beginthread(SCRIPT_CTX * THIS, UBYTE bank, UBYTE * pc, INT16 idx, UBYTE nargs) {
    UWORD * A = U16P(idx);
    SCRIPT_CTX * ctx = script_execute(bank, pc, A, 0);
    if (!nargs) return;
    if (ctx) {
        for (UBYTE i = nargs; i != 0; i--) {
            INT16 a = rd_i16(THIS->PC);
            a = (a < 0) ? *(INT16 *)(THIS->stack_ptr + a) : (INT16)script_memory[a];
            *(ctx->stack_ptr++) = (UWORD)a;
            THIS->PC += 2;
        }
    }
}

// Calls a native C handler until it returns TRUE (blocking-wait pattern). The
// native bridge for hardware commands will be built on this in Task 5.
static void vm_invoke(SCRIPT_CTX * THIS, UBYTE bank, UBYTE * fn, UBYTE nparams, INT16 idx) {
    (void)bank;
    UWORD * stack_frame = U16P(idx);
    UBYTE start = (THIS->update_fn != (void *)fn) ? (THIS->update_fn = (void *)fn, (UBYTE)TRUE) : (UBYTE)FALSE;
    // Calling a handler whose address arrived as data is intentional in a bytecode VM.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    if (((SCRIPT_UPDATE_FN)(void *)fn)(THIS, start, stack_frame)) {
        if (nparams) THIS->stack_ptr -= nparams;
        THIS->update_fn = 0;
        return;
    }
#pragma GCC diagnostic pop
    THIS->PC -= (INSTRUCTION_SIZE + 8);   // re-execute next quant (1 + args_len(invoke)=8)
}

static void vm_get_far(SCRIPT_CTX * THIS, INT16 idxA, UBYTE size, UBYTE bank, UBYTE * addr) {
    (void)bank;
    UWORD * A = U16P(idxA);
    *A = (size == 0) ? *((UBYTE *)addr) : *((UINT16 *)addr);
}

static void vm_call_native(SCRIPT_CTX * THIS, UBYTE bank, const void * ptr) {
    (void)bank;
    // Native handler address arrives as a data pointer; the cast is intentional.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    ((void (*)(SCRIPT_CTX *))ptr)(THIS);
#pragma GCC diagnostic pop
}

static void vm_rate_limit_const(SCRIPT_CTX * THIS, UWORD n_frames, INT16 idxA, UBYTE * pc) {
    UWORD * A = U16P(idxA);
    if ((UWORD)(sys_time - *A) >= 0x8000u) THIS->PC = pc;
    else *A = (UWORD)(sys_time + n_frames);
}

static void vm_test_terminate(SCRIPT_CTX * THIS, UBYTE flags) {
    if (flags & 1) THIS->waitable = TRUE;   // yield to next frame (GB waited on vblank here)
}

// ---- data / arithmetic opcodes ------------------------------------------------

static void vm_push(SCRIPT_CTX * THIS, UWORD value) { *(THIS->stack_ptr++) = value; }

static UWORD vm_pop(SCRIPT_CTX * THIS, UBYTE n) {
    if (n) THIS->stack_ptr -= n;
    return *(THIS->stack_ptr);
}

static void vm_reserve(SCRIPT_CTX * THIS, INT8 ofs) { THIS->stack_ptr += ofs; }

static void vm_set(SCRIPT_CTX * THIS, INT16 idxA, INT16 idxB) {
    INT16 * A = I16P(idxA);
    INT16 * B = I16P(idxB);
    *A = *B;
}

static void vm_set_const(SCRIPT_CTX * THIS, INT16 idx, UWORD value) {
    UWORD * A = U16P(idx);
    *A = value;
}

static void vm_push_value(SCRIPT_CTX * THIS, INT16 idx) {
    *(THIS->stack_ptr) = *((idx < 0) ? (THIS->stack_ptr + idx) : (script_memory + idx));
    THIS->stack_ptr++;
}

static void vm_push_value_ind(SCRIPT_CTX * THIS, INT16 idx) {
    idx = (INT16)*((idx < 0) ? (THIS->stack_ptr + idx) : (script_memory + idx));
    *(THIS->stack_ptr) = *((idx < 0) ? (THIS->stack_ptr + idx) : (script_memory + idx));
    THIS->stack_ptr++;
}

// Push the absolute script_memory word-index of an operand (pointer subtraction
// yields the word index directly - the GBA-portable form of the original).
static void vm_push_reference(SCRIPT_CTX * THIS, INT16 idx) {
    *(THIS->stack_ptr) = (idx < 0) ? (UWORD)((THIS->stack_ptr - script_memory) + idx) : (UWORD)idx;
    THIS->stack_ptr++;
}

static void vm_get_tlocal(SCRIPT_CTX * THIS, INT16 idxA, INT16 idxB) {
    INT16 * A = I16P(idxA);
    INT16 * B = (INT16 *)((idxB < 0) ? THIS->stack_ptr + idxB : THIS->base_addr + idxB);
    *A = *B;
}

static void vm_set_indirect(SCRIPT_CTX * THIS, INT16 idxA, INT16 idxB) {
    INT16 * A = I16P(idxA);
    A = (INT16 *)((*A < 0) ? THIS->stack_ptr + *A : script_memory + *A);
    INT16 * B = I16P(idxB);
    *A = *B;
}

static void vm_get_indirect(SCRIPT_CTX * THIS, INT16 idxA, INT16 idxB) {
    INT16 * A = I16P(idxA);
    INT16 * B = I16P(idxB);
    B = (INT16 *)((*B < 0) ? THIS->stack_ptr + *B : script_memory + *B);
    *A = *B;
}

static void vm_idle(SCRIPT_CTX * THIS) { THIS->waitable = TRUE; }

static void vm_lock(SCRIPT_CTX * THIS) { THIS->lock_count++; vm_lock_state++; }

static void vm_unlock(SCRIPT_CTX * THIS) {
    if (THIS->lock_count == 0) return;
    THIS->lock_count--;
    vm_lock_state--;
}

static void vm_init_rng(SCRIPT_CTX * THIS, INT16 idx) { vm_initrand(*U16P(idx)); }

static void vm_rand(SCRIPT_CTX * THIS, INT16 idx, UINT16 min, UINT16 limit) {
    *U16P(idx) = (UWORD)((vm_randw() % limit) + min);
}

static void vm_poll_loaded(SCRIPT_CTX * THIS, INT16 idx) {
    *U16P(idx) = vm_loaded_state;
    vm_loaded_state = FALSE;
}

static void vm_terminate(SCRIPT_CTX * THIS, INT16 idx) { script_terminate((UBYTE)(*I16P(idx))); }

static void vm_memset(SCRIPT_CTX * THIS, INT16 idx, INT16 value, INT16 count) {
    INT16 * v = (INT16 *)VM_REF_TO_PTR(idx);
    for (INT16 i = 0; i != count; i++) *v++ = value;
}

static void vm_memcpy(SCRIPT_CTX * THIS, INT16 idxA, INT16 idxB, INT16 count) {
    memcpy(VM_REF_TO_PTR(idxA), VM_REF_TO_PTR(idxB), (size_t)(count << 1));
}

static void vm_raise(SCRIPT_CTX * THIS, UBYTE code, UBYTE size) {
    vm_exception_code = code;
    // EXCEPTION_CHANGE_SCENE carries a 2-byte scene index (gbavm-specific, emitted
    // by the editor's bridge); capture it before skipping the raise's inline data.
    if (size >= 2) vm_exception_param = rd_u16(THIS->PC);
    THIS->PC += size;
}

// Read the pending exception + its param (the main loop acts on EXCEPTION_*).
UBYTE vm_get_exception(void) { return vm_exception_code; }
UWORD vm_get_exception_param(void) { return vm_exception_param; }

// RPN expression evaluator: reads a stream of operators from the bytecode
// (terminated by 0) and evaluates it on the data stack.
static void vm_rpn(SCRIPT_CTX * THIS) {
    INT16 * MEM = (INT16 *)script_memory;
    INT16 * ARGS = (INT16 *)THIS->stack_ptr;   // stack frame fixed at entry
    INT16 idx;
    INT8 op;

    while (TRUE) {
        op = *((const INT8 *)(THIS->PC++));
        if (op < 0) {
            switch (op) {
                // Raw-memory ops address an engine symbol directly. On GBA the
                // address is a 32-bit pointer the loader relocated in (vs GB's 16-bit
                // RAM address); the type tag ('i'/'u'/'I') is the access width.
                case VM_OP_REF_MEM_SET: {   // write the top-of-stack value to *addr
                    UBYTE mem_type = (UBYTE)*((const INT8 *)(THIS->PC++));
                    void * addr = (void *)(uintptr_t)rd_u32(THIS->PC);
                    THIS->PC += 4;
                    INT16 val = *(--(THIS->stack_ptr));
                    if (mem_type == VM_OP_MEM_I16) *((INT16 *)addr) = val;
                    else                           *((INT8 *)addr) = (INT8)val;
                    continue;
                }
                case VM_OP_REF_MEM: {       // push *addr
                    UBYTE mem_type = (UBYTE)*((const INT8 *)(THIS->PC++));
                    void * addr = (void *)(uintptr_t)rd_u32(THIS->PC);
                    THIS->PC += 4;
                    if (mem_type == VM_OP_MEM_I16)     *(THIS->stack_ptr) = *((INT16 *)addr);
                    else if (mem_type == VM_OP_MEM_U8) *(THIS->stack_ptr) = *((UINT8 *)addr);
                    else                               *(THIS->stack_ptr) = *((INT8 *)addr);
                    break;
                }
                case VM_OP_REF_MEM_IND:     // indirect raw-memory read: TODO; consume + push 0
                    op = *((const INT8 *)(THIS->PC++));
                    *(THIS->stack_ptr) = 0;
                    THIS->PC += 4;
                    break;
                case VM_OP_REF_SET_IND:
                    idx = rd_i16(THIS->PC);
                    idx = *((idx < 0) ? ARGS + idx : MEM + idx);
                    *((idx < 0) ? ARGS + idx : MEM + idx) = *(--(THIS->stack_ptr));
                    THIS->PC += 2;
                    continue;
                case VM_OP_REF_SET:
                    idx = rd_i16(THIS->PC);
                    *((idx < 0) ? ARGS + idx : MEM + idx) = *(--(THIS->stack_ptr));
                    THIS->PC += 2;
                    continue;
                case VM_OP_REF_IND:
                    idx = rd_i16(THIS->PC);
                    idx = *((idx < 0) ? ARGS + idx : MEM + idx);
                    *(THIS->stack_ptr) = *((idx < 0) ? ARGS + idx : MEM + idx);
                    THIS->PC += 2;
                    break;
                case VM_OP_REF:
                    idx = rd_i16(THIS->PC);
                    *(THIS->stack_ptr) = *((idx < 0) ? ARGS + idx : MEM + idx);
                    THIS->PC += 2;
                    break;
                case VM_OP_INT16:
                    *(THIS->stack_ptr) = rd_u16(THIS->PC);
                    THIS->PC += 2;
                    break;
                case VM_OP_INT8:
                    op = *((const INT8 *)(THIS->PC++));
                    *(THIS->stack_ptr) = op;
                    break;
                default:
                    return;
            }
            THIS->stack_ptr++;
        } else {
            INT16 * A = (INT16 *)THIS->stack_ptr - 2;
            INT16 * B = A + 1;
            switch ((UINT8)op) {
                case VM_OP_EQ    : *A = (*A == *B); break;
                case VM_OP_LT    : *A = (*A <  *B); break;
                case VM_OP_LE    : *A = (*A <= *B); break;
                case VM_OP_GT    : *A = (*A >  *B); break;
                case VM_OP_GE    : *A = (*A >= *B); break;
                case VM_OP_NE    : *A = (*A != *B); break;
                case VM_OP_AND   : *A = ((*A != 0) && (*B != 0)); break;
                case VM_OP_OR    : *A = ((*A != 0) || (*B != 0)); break;
                case VM_OP_NOT   : *B = !(*B); continue;
                case VM_OP_ADD   : *A = *A + *B; break;
                case VM_OP_SUB   : *A = *A - *B; break;
                case VM_OP_MUL   : *A = *A * *B; break;
                case VM_OP_DIV   : *A = *A / *B; break;
                case VM_OP_MOD   : *A = *A % *B; break;
                case VM_OP_B_AND : *A = *A & *B; break;
                case VM_OP_B_OR  : *A = *A | *B; break;
                case VM_OP_B_XOR : *A = *A ^ *B; break;
                case VM_OP_SHL   : *A = (INT16)((UWORD)*A << (*B & 0x0f)); break;
                case VM_OP_SHR   : *A = (INT16)((UWORD)*A >> (*B & 0x0f)); break;
                case VM_OP_MIN   : *A = (*A < *B) ? *A : *B; break;
                case VM_OP_MAX   : *A = (*A > *B) ? *A : *B; break;
                case VM_OP_ATAN2 : *A = vm_atan2(*A, *B); break;
                case VM_OP_ABS   : *B = (INT16)abs(*B);             continue;
                case VM_OP_B_NOT : *B = ~(*B);                      continue;
                case VM_OP_NEG   : *B = -(*B);                      continue;
                case VM_OP_ISQRT : *B = (INT16)vm_isqrt((UWORD)*B); continue;
                case VM_OP_RND   : *B = (INT16)(vm_randw() % (UWORD)*B); continue;
                default:
                    return;
            }
            THIS->stack_ptr--;
        }
    }
}

// ---- fixed-argument lengths (bytes) per opcode (GBA sizes: 32-bit code pointers) ----
static const UBYTE vm_args_len[256] = {
    [0x01]=2, [0x02]=1, [0x04]=4, [0x05]=1, [0x06]=8, [0x07]=7, [0x08]=4, [0x09]=4,
    [0x0A]=5, [0x0B]=1, [0x0D]=8, [0x0E]=8, [0x0F]=10,[0x10]=2, [0x11]=2, [0x12]=1,
    [0x13]=4, [0x14]=4, [0x15]=0, [0x16]=2, [0x17]=2, [0x18]=0, [0x19]=4, [0x1A]=10,
    [0x1B]=0, [0x1C]=8, [0x23]=2, [0x24]=6, [0x25]=0, [0x26]=0, [0x27]=2, [0x28]=4,
    [0x29]=4, [0x2A]=1, [0x2B]=2, [0x2C]=2, [0x2D]=5, [0x76]=6, [0x77]=6,
    // hardware opcodes (Task 5)
    [0x31]=2, [0x33]=2, [0x35]=2, [0x3A]=2, [0x51]=1, [0x54]=3,
    // actor movement (M3b): MOVE_TO_INIT/X/Y/XY + attr, SET_DIR + dir, SET_DIR_X/Y,
    // SET_ANIM_MOVING, MOVE_CANCEL (all take an i16 actor-ref; the move/dir ops add u8)
    [0x32]=3, [0x34]=3, [0x36]=3, [0x37]=3, [0x38]=3, [0x39]=2, [0x3B]=2, [0x3C]=2, [0x3D]=2,
    // trig + actor-angle opcodes (P0): ACTOR_GET_ANGLE, SIN_SCALE, COS_SCALE
    [0x86]=4, [0x89]=5, [0x8A]=5,
    // scene-boot opcodes accepted as no-ops (no GBA equivalent / handled elsewhere)
    [0x57]=1, [0x5D]=1,
    // DMG music (M5a): MUSIC_PLAY track,loop; MUSIC_STOP. SFX_PLAY sfx (M5b)
    // M5c: SOUND_MASTERVOL vol
    [0x60]=2, [0x61]=0, [0x66]=1, [0x63]=1,
    // SRAM save (M6a): SAVE_PEEK res,dest,sour,count,slot (9b); SAVE_CLEAR slot
    [0x2E]=9, [0x2F]=1,
    // dialogue text (M4): VM_DISPLAY_TEXT/_EX carry their text inline (variable length)
    [0x90]=0, [0x95]=0,
    // dialogue overlay window box (M4d): MOVE_TO x,y,speed; SHOW x,y,color,options; HIDE
    // M4q: OVERLAY_WAIT modal,condition
    [0x91]=3, [0x92]=4, [0x93]=0, [0x94]=2,
    // timers (M6f): PREPARE ctx,bank,addr(ptr); SET ctx,interval; STOP ctx; RESET ctx
    [0x70]=6, [0x71]=2, [0x72]=1, [0x73]=1,
};

// little-endian fixed-argument readers
#define A_U8(o)  (a[o])
#define A_I8(o)  ((INT8)a[o])
#define A_U16(o) ((UWORD)((UWORD)a[o] | ((UWORD)a[(o)+1] << 8)))
#define A_I16(o) ((INT16)A_U16(o))
#define A_U32(o) ((uint32_t)((uint32_t)a[o] | ((uint32_t)a[(o)+1] << 8) | \
                             ((uint32_t)a[(o)+2] << 16) | ((uint32_t)a[(o)+3] << 24)))
#define A_PTR(o) ((UBYTE *)(uintptr_t)A_U32(o))

// Execute one instruction in THIS context. Returns 0 at VM_OP_STOP.
UBYTE VM_STEP(SCRIPT_CTX * THIS) {
    UBYTE op = *(THIS->PC);
    if (op == VM_OP_STOP) return 0;

    const UBYTE * a = THIS->PC + INSTRUCTION_SIZE;        // fixed args follow the opcode
    THIS->PC += INSTRUCTION_SIZE + vm_args_len[op];       // advance past opcode + fixed args

    switch (op) {
        // data / arithmetic
        case 0x01: vm_push(THIS, A_U16(0)); break;
        case 0x02: vm_pop(THIS, A_U8(0)); break;
        case 0x10: vm_push_value_ind(THIS, A_I16(0)); break;
        case 0x11: vm_push_value(THIS, A_I16(0)); break;
        case 0x12: vm_reserve(THIS, A_I8(0)); break;
        case 0x13: vm_set(THIS, A_I16(0), A_I16(2)); break;
        case 0x14: vm_set_const(THIS, A_I16(0), A_U16(2)); break;
        case 0x15: vm_rpn(THIS); break;                   // reads its stream from THIS->PC
        case 0x17: vm_terminate(THIS, A_I16(0)); break;
        case 0x18: vm_idle(THIS); break;
        case 0x19: vm_get_tlocal(THIS, A_I16(0), A_I16(2)); break;
        case 0x23: vm_init_rng(THIS, A_I16(0)); break;
        case 0x24: vm_rand(THIS, A_I16(0), A_U16(2), A_U16(4)); break;
        case 0x25: vm_lock(THIS); break;
        case 0x26: vm_unlock(THIS); break;
        case 0x27: vm_raise(THIS, A_U8(0), A_U8(1)); break;
        case 0x28: vm_set_indirect(THIS, A_I16(0), A_I16(2)); break;
        case 0x29: vm_get_indirect(THIS, A_I16(0), A_I16(2)); break;
        case 0x2B: vm_poll_loaded(THIS, A_I16(0)); break;
        case 0x2C: vm_push_reference(THIS, A_I16(0)); break;
        case 0x76: vm_memset(THIS, A_I16(0), A_I16(2), A_I16(4)); break;
        case 0x77: vm_memcpy(THIS, A_I16(0), A_I16(2), A_I16(4)); break;
        // control flow
        case 0x04: vm_call(THIS, A_PTR(0)); break;
        case 0x05: vm_ret(THIS, A_U8(0)); break;
        case 0x06: vm_get_far(THIS, A_I16(0), A_U8(2), A_U8(3), A_PTR(4)); break;
        case 0x07: vm_loop(THIS, A_I16(0), A_PTR(2), A_U8(6)); break;
        case 0x08: vm_switch(THIS, A_I16(0), A_U8(2), A_U8(3)); break;
        case 0x09: vm_jump(THIS, A_PTR(0)); break;
        case 0x0A: vm_call_far(THIS, A_U8(0), A_PTR(1)); break;
        case 0x0B: vm_ret_far(THIS, A_U8(0)); break;
        case 0x0D: vm_invoke(THIS, A_U8(0), A_PTR(1), A_U8(5), A_I16(6)); break;
        case 0x0E: vm_beginthread(THIS, A_U8(0), A_PTR(1), A_I16(5), A_U8(7)); break;
        case 0x0F: vm_if(THIS, A_U8(0), A_I16(1), A_I16(3), A_PTR(5), A_U8(9)); break;
        case 0x16: vm_join(THIS, A_I16(0)); break;
        case 0x1A: vm_if_const(THIS, A_U8(0), A_I16(1), A_I16(3), A_PTR(5), A_U8(9)); break;
        case 0x1C: vm_rate_limit_const(THIS, A_U16(0), A_I16(2), A_PTR(4)); break;
        case 0x2A: vm_test_terminate(THIS, A_U8(0)); break;
        case 0x2D: vm_call_native(THIS, A_U8(0), A_PTR(1)); break;
        // hardware opcodes (Task 5) -> Butano via hw.cpp
        // operand is a variable index holding the actor number (GB Studio semantics)
        case 0x31: hw_actor_activate(*(INT16 *)vm_resolve_ref(THIS, A_I16(0))); break;
        case 0x33: hw_actor_deactivate(*(INT16 *)vm_resolve_ref(THIS, A_I16(0))); break;
        case 0x35: hw_actor_set_pos((uint16_t *)vm_resolve_ref(THIS, A_I16(0))); break;
        case 0x3A: hw_actor_get_pos((uint16_t *)vm_resolve_ref(THIS, A_I16(0))); break;
        // actor movement (M3b). The i16 operand resolves to an {index, x, y} block;
        // MOVE_TO_X/Y/XY block (rewind past opcode + its 3 operand bytes) until arrival.
        case 0x32: { uint16_t *r = (uint16_t *)vm_resolve_ref(THIS, A_I16(0)); hw_actor_move_init((INT16)r[0], r[1], r[2]); break; }
        case 0x34: { uint16_t *r = (uint16_t *)vm_resolve_ref(THIS, A_I16(0)); hw_actor_set_dir((INT16)r[0], A_U8(2)); break; }
        case 0x36: { uint16_t *r = (uint16_t *)vm_resolve_ref(THIS, A_I16(0)); if (!hw_actor_move_step((INT16)r[0], 0)) { THIS->PC -= (INSTRUCTION_SIZE + 3); THIS->waitable = TRUE; } break; }
        case 0x37: { uint16_t *r = (uint16_t *)vm_resolve_ref(THIS, A_I16(0)); if (!hw_actor_move_step((INT16)r[0], 1)) { THIS->PC -= (INSTRUCTION_SIZE + 3); THIS->waitable = TRUE; } break; }
        case 0x38: { uint16_t *r = (uint16_t *)vm_resolve_ref(THIS, A_I16(0)); if (!hw_actor_move_step((INT16)r[0], 2)) { THIS->PC -= (INSTRUCTION_SIZE + 3); THIS->waitable = TRUE; } break; }
        case 0x39: { uint16_t *r = (uint16_t *)vm_resolve_ref(THIS, A_I16(0)); hw_actor_move_set_dir((INT16)r[0], 0); break; }
        case 0x3B: { uint16_t *r = (uint16_t *)vm_resolve_ref(THIS, A_I16(0)); hw_actor_move_set_dir((INT16)r[0], 1); break; }
        case 0x3C: { uint16_t *r = (uint16_t *)vm_resolve_ref(THIS, A_I16(0)); hw_actor_set_moving((INT16)r[0]); break; }
        case 0x3D: { uint16_t *r = (uint16_t *)vm_resolve_ref(THIS, A_I16(0)); hw_actor_move_cancel((INT16)r[0]); break; }
        case 0x51: hw_set_sprites_visible(A_U8(0)); break;
        case 0x54: hw_input_get((uint16_t *)vm_resolve_ref(THIS, A_I16(1)), A_U8(0)); break;
        // trig + actor-angle opcodes (P0)
        case 0x86: hw_actor_get_angle((uint16_t *)vm_resolve_ref(THIS, A_I16(0)), (int16_t *)vm_resolve_ref(THIS, A_I16(2))); break;
        case 0x89: vm_sin_scale(THIS, A_I16(0), A_I16(2), A_U8(4)); break;
        case 0x8A: vm_cos_scale(THIS, A_I16(0), A_I16(2), A_U8(4)); break;
        // VM_FADE: fade the screen out/in; blocks the thread until the fade ends
        // (rewind past opcode + its 1-byte flags operand, then yield).
        case 0x57: if (!hw_fade_step(A_U8(0))) { THIS->PC -= (INSTRUCTION_SIZE + 1); THIS->waitable = TRUE; } break;
        case 0x5D: /* VM_SET_SPRITE_MODE: Butano sets sprite size per-sprite; nothing global */ break;
        // DMG music (M5a): play the resolved track (loop per the op) / stop the music.
        case 0x60: hw_music_play(A_U8(0), A_U8(1)); break;
        case 0x61: hw_music_stop(); break;
        case 0x66: hw_sfx_play(A_U8(0)); break; // SFX_PLAY (M5b)
        case 0x63: hw_sound_mastervol(A_U8(0)); break; // SOUND_MASTERVOL (M5c)
        // VM_DISPLAY_TEXT (0x90) / VM_DISPLAY_TEXT_EX (0x95): reveal the inline dialogue
        // text; block (rewind) until fully revealed, then advance. The A-wait is now a
        // separate VM_OVERLAY_WAIT (M4q). 0x95 has a leading display-flag byte (bit 0 =
        // .DISPLAY_PRESERVE_POS = append the chunk for !W: waits).
        case 0x90:
        case 0x95: {
            const UBYTE *p = a;
            UBYTE preserve = 0;
            if (op == 0x95) preserve = (UBYTE)((*p++) & 1); // display flag (preserve-pos)
            // Payload: avatar byte (0xff = none, M4m), var-count, then count 16-bit
            // script_memory indices, then the null-terminated text. Resolve each
            // variable's value for the text's %d placeholders (M4i interpolation).
            UBYTE avatar = *p++;
            UBYTE nvars = *p++;
            INT16 vals[8];
            for (UBYTE i = 0; i < nvars; i++) {
                UWORD idx = (UWORD)((UWORD)p[0] | ((UWORD)p[1] << 8));
                p += 2;
                if (i < 8) vals[i] = (INT16)script_memory[idx];
            }
            const char *txt = (const char *)p;
            if (hw_text_step(txt, vals, nvars, avatar, preserve)) {
                UWORD len = 0; while (txt[len]) len++;
                // skip flag(0x95 only) + avatar + nvars + var indices + text + terminator
                THIS->PC += (UWORD)((op == 0x95 ? 3 : 2) + nvars * 2 + len + 1);
            } else { THIS->PC -= INSTRUCTION_SIZE; THIS->waitable = TRUE; }
            break;
        }
        // Dialogue overlay window box (M4d): MOVE_TO/SHOW/HIDE set the box target (the box
        // slides via hw_overlay_update each frame). VM_OVERLAY_WAIT (M4q) blocks until its
        // UI conditions; this is where dialogue waits for A.
        case 0x91: hw_overlay_move_to(A_U8(0), A_U8(1), A_I8(2)); break;
        case 0x92: hw_overlay_show(A_U8(0), A_U8(1), A_U8(2)); break;
        case 0x93: hw_overlay_hide(); break;
        case 0x94: if (!hw_overlay_wait(A_U8(1))) { THIS->PC -= (INSTRUCTION_SIZE + 2); THIS->waitable = TRUE; } break;
        // Timers (M6f): PREPARE stores a slot's script; SET arms it to fire every `interval`
        // ticks; STOP disables it; RESET restarts the countdown. timers_update() fires them.
        case 0x70: { UBYTE c = A_U8(0); if (c < VM_MAX_TIMERS) { vm_timers[c].bank = A_U8(1); vm_timers[c].pc = A_PTR(2); } break; }
        case 0x71: { UBYTE c = A_U8(0); if (c < VM_MAX_TIMERS) { UBYTE iv = A_U8(1); vm_timers[c].interval = iv; vm_timers[c].countdown = (UWORD)iv * TIMER_CYCLES; vm_timers[c].active = 1; } break; }
        case 0x72: { UBYTE c = A_U8(0); if (c < VM_MAX_TIMERS) vm_timers[c].active = 0; break; }
        case 0x73: { UBYTE c = A_U8(0); if (c < VM_MAX_TIMERS) vm_timers[c].countdown = (UWORD)vm_timers[c].interval * TIMER_CYCLES; break; }
        // Scene stack: push saves the current scene; pop/pop_all signal a scene
        // change (like VM_RAISE CHANGE_SCENE) to the popped scene.
        // SRAM save (M6a): SAVE_PEEK sets res = (a valid save exists); the COUNT>0
        // read-saved-vars form is a follow-up. SAVE_CLEAR invalidates the save. (Save/
        // load themselves go via VM_RAISE EXCEPTION_SAVE/LOAD -> the main loop.)
        case 0x2E: { UWORD *res = (UWORD *)vm_resolve_ref(THIS, A_I16(0)); *res = (UWORD)gba_save_peek(A_U8(8)); break; }
        case 0x2F: gba_save_clear(A_U8(0)); break;
        case 0x68: gba_scene_push(); break;
        case 0x69: vm_exception_code = EXCEPTION_CHANGE_SCENE; vm_exception_param = gba_scene_pop(); break;
        case 0x6A: vm_exception_code = EXCEPTION_CHANGE_SCENE; vm_exception_param = gba_scene_pop_all(); break;
        default:
            // remaining opcodes (other hardware commands, vm_asm) not implemented yet
            vm_last_unimplemented_op = op;
            return 0;
    }
    return 1;
}

// ---- thread scheduler ---------------------------------------------------------

void script_runner_init(UBYTE reset) {
    if (reset) {
        memset(script_memory, 0, sizeof(script_memory));
        memset(CTXS, 0, sizeof(CTXS));
        memset(vm_timers, 0, sizeof(vm_timers)); // M6f: timers are per-scene
    }
    UWORD * base_addr = &script_memory[VM_HEAP_SIZE];
    free_ctxs = CTXS; first_ctx = 0;
    SCRIPT_CTX * nxt = 0;
    SCRIPT_CTX * tmp = CTXS + (VM_MAX_CONTEXTS - 1);
    for (UBYTE i = VM_MAX_CONTEXTS; i != 0; i--) {
        tmp->next = nxt;
        tmp->base_addr = base_addr;
        tmp->ret_sp = ret_stacks[tmp - CTXS];
        tmp->ID = i;
        base_addr += VM_CONTEXT_STACK_SIZE;
        nxt = tmp--;
    }
    vm_lock_state = 0;
    vm_loaded_state = FALSE;
    old_executing_ctx = 0; executing_ctx = first_ctx;
}

SCRIPT_CTX * script_execute(UBYTE bank, UBYTE * pc, UWORD * handle, UBYTE nargs, ...) {
    if (free_ctxs == NULL) return NULL;

    SCRIPT_CTX * tmp = free_ctxs;
    free_ctxs = free_ctxs->next;

    tmp->PC = pc; tmp->bank = bank;
    tmp->stack_ptr = tmp->base_addr;
    tmp->ret_sp = ret_stacks[tmp - CTXS];
    tmp->hthread = handle;
    if (handle) *handle = tmp->ID;
    tmp->terminated = FALSE;
    tmp->lock_count = 0;
    tmp->flags = 0;
    tmp->update_fn = 0;
    tmp->next = NULL;

    if (first_ctx) {
        SCRIPT_CTX * idx = first_ctx;
        while (idx->next) idx = idx->next;
        idx->next = tmp;
    } else first_ctx = tmp;

    if (nargs) {
        va_list va;
        va_start(va, nargs);
        for (UBYTE i = nargs; i != 0; i--) *(tmp->stack_ptr++) = (UWORD)va_arg(va, int);
        va_end(va);
    }
    return tmp;
}

UBYTE script_terminate(UBYTE ID) {
    SCRIPT_CTX * ctx = first_ctx;
    while (ctx) {
        if (ctx->ID == ID) {
            if (ctx->hthread) { *(ctx->hthread) |= SCRIPT_TERMINATED; ctx->hthread = 0; }
            return ctx->terminated = TRUE;
        }
        ctx = ctx->next;
    }
    return FALSE;
}

// M6f: advance every armed timer one frame; when a slot's countdown elapses, run its
// prepared script as a new thread and restart the countdown (timers repeat).
void timers_update(void) {
    for (UBYTE c = 0; c < VM_MAX_TIMERS; c++) {
        VM_TIMER * t = &vm_timers[c];
        if (!t->active || t->interval == 0) continue;
        if (t->countdown) t->countdown--;
        if (t->countdown == 0) {
            script_execute(t->bank, t->pc, 0, 0);
            t->countdown = (UWORD)t->interval * TIMER_CYCLES;
        }
    }
}

UBYTE script_runner_update(void) {
    UBYTE waitable;
    UBYTE counter;

    if (!vm_lock_state) { old_executing_ctx = 0; executing_ctx = first_ctx; }

    waitable = TRUE;
    counter = INSTRUCTIONS_PER_QUANT;
    while (executing_ctx) {
        vm_exception_code = EXCEPTION_CODE_NONE;
        executing_ctx->waitable = FALSE;
        if ((executing_ctx->terminated != FALSE) || (!VM_STEP(executing_ctx))) {
            vm_lock_state -= executing_ctx->lock_count;
            if (executing_ctx->hthread) *(executing_ctx->hthread) |= SCRIPT_TERMINATED;
            if (old_executing_ctx) old_executing_ctx->next = executing_ctx->next;
            if (first_ctx == executing_ctx) first_ctx = executing_ctx->next;
            executing_ctx->next = free_ctxs; free_ctxs = executing_ctx;
            if (old_executing_ctx) executing_ctx = old_executing_ctx->next; else executing_ctx = first_ctx;
        } else {
            if (vm_exception_code) return RUNNER_EXCEPTION;
            if (!(executing_ctx->waitable) && (counter--)) continue;
            if (vm_lock_state) break;
            waitable &= executing_ctx->waitable;
            old_executing_ctx = executing_ctx; executing_ctx = executing_ctx->next;
            counter = INSTRUCTIONS_PER_QUANT;
        }
    }

    if (first_ctx == 0) return RUNNER_DONE;
    return waitable ? RUNNER_IDLE : RUNNER_BUSY;
}
