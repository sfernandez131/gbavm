# gbavm

**The Game Boy Advance engine behind [GBA Studio](https://github.com/sfernandez131/gba-studio).**

`gbavm` is a port of **GBVM** — the bytecode virtual machine from
[GB Studio](https://github.com/chrismaltby/gb-studio) — from its original Z80/GBDK
implementation to portable C/C++ running on the GBA via
[Butano](https://github.com/GValiente/butano) and devkitARM. It runs the same bytecode the
GBA Studio editor compiles, so no-code games authored in the editor execute natively on
Game Boy Advance hardware.

> ⚠️ **Experimental — work in progress.** Independent fork; not affiliated with or endorsed
> by GB Studio.

## What works

- **GBVM core**, ported to portable C: opcode dispatch, the full RPN expression evaluator,
  control flow, and the 16-thread scheduler *(verified on emulated GBA hardware)*.
- **Hardware bridge (core slice)** on Butano: actors/sprites, sprite visibility, and input.
- **Editor-emitted bytecode** runs end-to-end: the engine applies a relocation table at
  load (32-bit native code pointers, patched byte-wise for the ARM7TDMI) and executes the
  bytecode GBA Studio generates from a real editor scene.

GBA-specific conventions vs. the original GBVM: little-endian operands, 32-bit native code
pointers (with a load-time relocation table), a separate per-thread return stack, and no
ROM bank-switching (flat address space). See [`include/vm.h`](include/vm.h).

## Roadmap

See the [GBA Studio roadmap](https://github.com/sfernandez131/gba-studio#roadmap). Next up
here: more hardware command handlers (backgrounds, camera, actor movement/animation),
dialogue/UI, audio (Maxmod), and multi-script linking.

## Building

Requires [devkitPro](https://devkitpro.org/) (devkitARM) and
[Butano](https://github.com/GValiente/butano) checked out alongside this repo
(`../butano`).

```bash
make            # produces gbavm.gba
```

Run the result in [mGBA](https://mgba.io/) (or any GBA emulator / flashcart).
`game_script.c` holds the bytecode program; GBA Studio's build pipeline regenerates it from
the editor project.

## License

MIT. Portions are derived from **GBVM** (https://github.com/chrismaltby/gbvm), Copyright (c)
2020 Toxa, used under the MIT License — see [LICENSE](LICENSE). Builds on
[Butano](https://github.com/GValiente/butano) by Gustavo Valiente and the devkitPro
toolchain.
