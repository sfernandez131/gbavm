// gbavm - GBA Studio engine
// Copyright (c) 2026 Scott Fernandez
// MIT License - see the LICENSE file.
//
// Whole-project link loader (Milestone M1). Walks the generated gba_procs[]
// manifest and patches each proc's bytecode in place: local relocations resolve
// to an offset within the same proc, symbolic relocations to another proc's
// array (script -> script). Generalizes the single hand-patched jump, then the
// per-blob apply_relocations(), into one project-wide pass.

#include "gba_link.h"

#include <cstdint>

namespace
{
    // Write a 32-bit native pointer into the (possibly unaligned) 4-byte field at
    // code[at], byte-wise - unaligned 32-bit stores are unsafe on the ARM7TDMI.
    inline void patch_ptr(unsigned char * code, unsigned int at, uintptr_t v)
    {
        code[at + 0] = static_cast<unsigned char>(v);
        code[at + 1] = static_cast<unsigned char>(v >> 8);
        code[at + 2] = static_cast<unsigned char>(v >> 16);
        code[at + 3] = static_cast<unsigned char>(v >> 24);
    }
}

void gba_link_apply(void)
{
    for(unsigned int i = 0; i < gba_procs_count; ++i)
    {
        const GbaProc & p = gba_procs[i];

        // Local relocations: flat {field, target} pairs -> code + target.
        for(unsigned int r = 0; r < p.relocs_count; ++r)
        {
            const unsigned int field = p.relocs[r * 2];
            const unsigned int target = p.relocs[r * 2 + 1];
            patch_ptr(p.code, field, reinterpret_cast<uintptr_t>(p.code + target));
        }

        // Symbolic relocations: {field, &target_proc} -> the target proc's address.
        for(unsigned int s = 0; s < p.symrelocs_count; ++s)
        {
            patch_ptr(p.code, p.symrelocs[s].at,
                      reinterpret_cast<uintptr_t>(p.symrelocs[s].target));
        }
    }
}
