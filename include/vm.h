// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
//
// Portions of this file are derived from GBVM (https://github.com/chrismaltby/gbvm),
// Copyright (c) 2020 Toxa, used under the MIT License. See the LICENSE file.

#ifndef GBAVM_VM_H
#define GBAVM_VM_H

// gbavm VM core - ported from GB Studio's GBVM (appData/engine/gbvm).
//
// Hardware-neutral bytecode virtual machine, retargeted from Z80/GBDK to portable
// C for the GBA (devkitARM / Butano).
//
// GBA bytecode conventions (differ from the GB original):
//   * Code targets (jump/call/loop/if/switch/... operands) are NATIVE 32-bit
//     pointers, not 16-bit addresses. When the compiler is ported it will emit
//     these as linker-resolved &label values.
//   * Return addresses live on a separate per-thread 32-bit return stack
//     (SCRIPT_CTX.ret_sp), so the 16-bit data stack stays identical to GB Studio's
//     and the compiler's stack-index math is unchanged.
//   * ROM bank-switching is gone (flat address space); bank operands are ignored.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- GBDK-compatible scalar typedefs, so ported handler bodies stay verbatim ---
typedef uint8_t  UBYTE;
typedef int8_t   BYTE;
typedef int8_t   INT8;
typedef uint8_t  UINT8;
typedef uint16_t UWORD;
typedef int16_t  WORD;
typedef int16_t  INT16;
typedef uint16_t UINT16;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

typedef void * SCRIPT_CMD_FN;

typedef struct SCRIPT_CMD {
    SCRIPT_CMD_FN fn;
    UBYTE fn_bank;   // unused on GBA (flat address space); kept for table parity
    UBYTE args_len;
} SCRIPT_CMD;

// Blocking-wait handler signature used by vm_invoke.
typedef UBYTE (*SCRIPT_UPDATE_FN)(void * THIS, UBYTE start, UWORD * stack_frame);

// Resolve a VM operand index to a pointer:
//   negative -> relative to the current thread's stack pointer
//   positive -> absolute index into shared script_memory[]
#define VM_REF_TO_PTR(idx) (void *)(((idx) < 0) ? THIS->stack_ptr + (idx) : script_memory + (idx))
#define VM_GLOBAL(idx) script_memory[(idx)]

typedef struct SCRIPT_CTX {
    const UBYTE * PC;          // native pointer into the bytecode blob (32-bit on GBA)
    UBYTE bank;                // unused on GBA
    struct SCRIPT_CTX * next;  // linked list of contexts for multitasking
    void * update_fn;          // blocking-wait handler (vm_invoke)
    const UBYTE ** ret_sp;     // per-thread 32-bit return-address stack pointer
    UWORD * stack_ptr;         // VM data stack pointer
    UWORD * base_addr;         // base of this thread's data stack frame
    UBYTE ID;                  // thread id (1..VM_MAX_CONTEXTS)
    UWORD * hthread;           // monitoring variable handle
    UBYTE terminated;
    UBYTE waitable;
    UBYTE lock_count;
    UBYTE flags;
} SCRIPT_CTX;

#define INSTRUCTION_SIZE       1
#define VM_MAX_CONTEXTS        16    // max concurrent VM threads
#define VM_CONTEXT_STACK_SIZE  64    // data stack words per thread
#define VM_RET_STACK_DEPTH     24    // return-address slots per thread
#define VM_HEAP_SIZE           768   // shared variables
#define INSTRUCTIONS_PER_QUANT 0x10
#define SCRIPT_TERMINATED      0x8000

#define VM_OP_STOP  0
// logical operators
#define VM_OP_EQ    1
#define VM_OP_LT    2
#define VM_OP_LE    3
#define VM_OP_GT    4
#define VM_OP_GE    5
#define VM_OP_NE    6
#define VM_OP_AND   7
#define VM_OP_OR    8
#define VM_OP_NOT   9
// math operators
#define VM_OP_ADD   10
#define VM_OP_SUB   11
#define VM_OP_MUL   12
#define VM_OP_DIV   13
#define VM_OP_MOD   14
#define VM_OP_B_AND 15
#define VM_OP_B_OR  16
#define VM_OP_B_XOR 17
#define VM_OP_SHL   18
#define VM_OP_SHR   19
#define VM_OP_MIN   20
#define VM_OP_MAX   21
#define VM_OP_ATAN2 22
#define VM_OP_ABS   23
#define VM_OP_B_NOT 24
#define VM_OP_NEG   25
#define VM_OP_ISQRT 26
#define VM_OP_RND   27
// memory operators (RPN)
#define VM_OP_INT8          -1
#define VM_OP_INT16         -2
#define VM_OP_REF           -3
#define VM_OP_REF_IND       -4
#define VM_OP_REF_SET       -5
#define VM_OP_REF_SET_IND   -6
#define VM_OP_REF_MEM       -7
#define VM_OP_REF_MEM_SET   -8
#define VM_OP_REF_MEM_IND   -9

#define VM_OP_MEM_I8        'i'
#define VM_OP_MEM_U8        'u'
#define VM_OP_MEM_I16       'I'

// shared script memory: heap (globals) followed by per-thread data stacks
extern UWORD script_memory[VM_HEAP_SIZE + (VM_MAX_CONTEXTS * VM_CONTEXT_STACK_SIZE)];

// last opcode VM_STEP hit that is not implemented yet (0 = none); for diagnostics
extern UBYTE vm_last_unimplemented_op;
// global frame counter (used by vm_rate_limit_const); advance once per frame
extern UWORD sys_time;

// runner state
#define RUNNER_DONE      0
#define RUNNER_IDLE      1
#define RUNNER_BUSY      2
#define RUNNER_EXCEPTION 3

// seed the VM PRNG at boot (GBA has no DIV register; pass a hardware-timer value)
void vm_boot_seed(UWORD seed);
// initialize / reset the script runner contexts
void script_runner_init(UBYTE reset);
// start a script in a newly allocated context
SCRIPT_CTX * script_execute(UBYTE bank, UBYTE * pc, UWORD * handle, UBYTE nargs, ...);
// terminate a script by thread id; returns FALSE if no such thread
UBYTE script_terminate(UBYTE ID);
// run one quant across all active contexts; returns a RUNNER_* code
UBYTE script_runner_update(void);
// scene-change exception (P2): the runner returns RUNNER_EXCEPTION; main.cpp reads
// the code + param to load the target scene. EXCEPTION_CHANGE_SCENE matches vm.i.
#define EXCEPTION_CHANGE_SCENE 2
UWORD vm_get_exception_code(void);
UWORD vm_get_exception_param(void);
void  vm_request_change_scene(UWORD index);
// execute a single instruction in the given context; returns 0 at VM_OP_STOP
UBYTE VM_STEP(SCRIPT_CTX * THIS);
// resolve a VM operand index to a pointer (used by the hardware bridge, hw.cpp)
void * vm_resolve_ref(SCRIPT_CTX * THIS, INT16 idx);

#ifdef __cplusplus
}
#endif

#endif // GBAVM_VM_H
