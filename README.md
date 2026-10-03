# Web VMU

A Sega Dreamcast **Visual Memory Unit** emulator that runs in a phone or desktop browser and on an ESP32-S3. It installs as an app, works offline, and has touch buttons, sound, the four LCD symbols and automatic resume. A sibling of Web PocketStation, Web Pokémon mini and Web Brick.

**No BIOS is needed and no games are included.** The few system services a mini-game calls (save to flash, read flash, clock tick, exit) are built into the emulator. You import your own games and memory cards; they are stored in your browser and never uploaded anywhere. One small test game written for this project, *Sketch*, is built in so there is something to run straight away.

One emulator, two targets:

* `core/` is the whole machine in portable C++ (no heap, no C library, no floating point).
* `web/` is the app. It runs `core/` compiled to a 36 KB WebAssembly file.
* `esp32/` is firmware for the M5Stack CoreS3 (+ Gamepad3). It compiles the same `core/` files.

## Try it

```bash
bash run.sh          # then open http://localhost:8768 (or the address it prints, from a phone on the same Wi-Fi)
```

Tap **Try the built-in test game**, or **Import game or card…**. *Add to Home Screen* in the browser menu makes it full-screen like an app.

To publish your own copy as a website: `bash publish.sh` (see *Publishing* below).

## What you can import

| File | What it is | What happens |
|---|---|---|
| `.vms` (with or without its `.vmi`) | one game or one save | a game gets a memory card of its own; a save goes onto the card you choose |
| `.dci` | one file, Nexus/DreamExplorer format | same |
| `.bin`, `.vmu` (exactly 128 KB) | a whole memory card, as Dreamcast emulators keep them (`vmu_save_A1.bin`, …) | added as a card |
| `.dcm` | a whole card, byte-swapped | added as a card |
| `.zip` | any of the above | unpacked |

A VMU holds one mini-game at a time, at the start of its memory, plus saves. That is why each imported game becomes its own card.

## Using it

| | |
|---|---|
| Tap a game | Starts it. The VMU's clock is set from your device. |
| ‹ back | Saves a snapshot; next time the game continues exactly where you left it. |
| MODE | In a real VMU this leaves the game for the clock and file screens. Those live in the BIOS, so here the game simply ends (anything it saved is on its card) and you can play again or go back. |
| Card ⋯ | Files on the card (download or delete each), add files, **download the card** as a `.bin`, rename, delete. |
| Player ⋯ | Controls, screen options (slow liquid crystal, dot grid), restart. |
| Keyboard | Arrows, `Z`/Space = A, `X` = B, Enter/`M` = MODE, `S`/Backspace = SLEEP, Esc = back. A gamepad works too. |

When a game writes to its card, the card is stored in the browser about a second later. Clearing the browser's site data erases everything, so download cards you care about.

**Real BIOS (experimental):** Settings can load a 64 KB VMU BIOS dump, which should give the VMU's own menus. That path has never been run: no BIOS was available while this was written. Games do not need it.

## Save syncing (planned, not built yet)

Everything is already stored the way syncing needs it:

* The unit is the **whole 128 KB card image**, byte for byte what Flycast, Redream and other emulators keep as `vmu_save_*.bin`, what a real VMU holds, and what the ESP32 firmware keeps in `/cards/*.bin`. Download a card here and an emulator can use it; import an emulator's card and it plays here.
* Each card has an id, a name, a modified time and a SHA-1, so a companion can tell which side changed.
* Snapshots (where a game was left) are separate from cards, so replacing a card never corrupts one; the snapshot is just dropped when its game changes.

The plan is the PocketSync shape again: a small program on the PC that watches the emulator's VMU files, keeps a history of every version, serves this app, and offers `GET /api/cards`, `GET`/`PUT /api/cards/<id>/data` with conflict detection; the ESP32 firmware pulls at power-on and pushes on leaving a game.

## ESP32

See [`esp32/README.md`](esp32/README.md). In short: `cd esp32 && pio run -t upload -t monitor`. The firmware has not been run on a real CoreS3 yet (it was written while away from the hardware); what was checked is listed below.

Inside another firmware (the goal is a task on the second core of a Meshtastic build), the core is passive: call `emu.runUs(microseconds that passed)` from a low-priority task and it catches up and returns. It never allocates, blocks or touches hardware. It needs 1.7 KB of RAM, 18 KB of code and the 128 KB card image; a snapshot is 1.6 KB.

## How it was tested

* **Against a reference emulator.** `tests/compare.py` runs this core and [libElysianVMU](https://github.com/gyrovorbis/libevmu) side by side with the same scripted button presses and compares the program flow (every program counter and cycle count) and the whole machine state (registers, timers, both RAM banks, display memory) and, at the end, the memory card. Identical for four homebrew programs plus Sketch (several button scripts each, up to 2 million cycles), and for 600 random programs compared after *every* instruction. Each run is repeated with a save/load round trip at every checkpoint and with the built `vmu.wasm` in Node.
* **Clock.** `tests/clock_test.cpp`: the firmware clock service against the C library's calendar for 400 days from six starting dates (month ends, leap years, 2100).
* **Browser.** `tests/e2e.py` drives the real app in a phone-sized Chromium: empty library, the test game, keyboard and touch, sound, saving to the card, snapshots, MODE, every import format, card download, deleting files, reload.
* **ESP32-S3.** Both firmware targets were compiled and linked with the real ESP32-S3 compiler against Arduino-ESP32 2.0.17 and M5Unified. The self-test target (`pio run -e coretest`) was run on an emulated ESP32-S3 (Espressif's QEMU): it gets exactly the PC build's results. Counting instructions there, a game on the VMU's normal clock needs about 16.5 million ESP32 instructions per second, roughly a tenth of one 240 MHz core.
* **Speed in a browser.** About 350 times real time in Node on a PC, so a phone has plenty to spare.

Not tested: real hardware (CoreS3 screen, sound, buttons, storage), the real-BIOS path, commercial games (none were available; only open-source homebrew and Sketch), and the 6 MHz clock at full speed on the ESP32 (a real VMU only has that clock while plugged into a controller; by the instruction count it is borderline for one core).

Known difference from a real VMU: behaviour follows libElysianVMU, including where it simplifies (for example the port 3 interrupt).

## Running the tests

```bash
bash tests/build_ref.sh                                   # once: fetches and builds libElysianVMU + tests/ref_trace
python3 tests/compare.py web/demo/sketch.vms --fuzz 100   # add your own .vms files to the list
g++ -std=c++11 -O2 -o clock_test tests/clock_test.cpp core/vmu.cpp core/vmufs.cpp && ./clock_test
bash run.sh &  python3 tests/e2e.py                       # needs: pip install playwright
```

## Layout

| | |
|---|---|
| `core/vmu.h`, `vmu.cpp` | the machine: Sanyo LC86K87 CPU, timers, interrupts, LCD, buzzer, flash, the emulated firmware services |
| `core/vmufs.h`, `vmufs.cpp` | the card's file system and the `.vmi` / `.dci` / `.dcm` formats |
| `core/wasm_api.cpp`, `build.sh` | the WebAssembly interface; `bash build.sh` rebuilds `web/core/vmu.wasm` (needs `clang` and `lld`; only after changing `core/`) |
| `web/` | the app: plain JavaScript, no dependencies, no build step |
| `esp32/` | PlatformIO project for the CoreS3, plus the on-chip self-test |
| `demo/sketch.s`, `tools/vmuasm.py` | the test game and the small assembler that builds it |
| `tools/vmucard.cpp` | make and list card images on the PC |
| `tests/` | everything under *How it was tested* |

## Publishing

`bash publish.sh` creates a public GitHub repo from this folder, pushes the code to `main`, pushes `web/` to a `gh-pages` branch and switches on GitHub Pages (needs `git` and the GitHub CLI `gh`, logged in). To publish a change later: `git add -A && git commit -m "what changed" && bash publish.sh`.

`.gitignore` and `publish.sh` keep games, saves, memory cards and BIOS dumps out of the repository (the only game file allowed is this project's own `web/demo/sketch.vms`). Do not add them.

## Licence and credits

Web VMU is free software under the **GNU General Public License, version 3 or later** (see `LICENSE`). It comes with no warranty.

The emulator was written for this project. Hardware behaviour was learned from, and checked against, [libElysianVMU](https://github.com/gyrovorbis/libevmu) by Falco Girgis (MIT), with MAME's `lc8670` core by Sandro Ronco (BSD-3-Clause) as a second reference; neither project's code is included. Thanks to the authors of the homebrew used for testing (Tiny 3D Engine and Sound Demo by Rockin'-B, ADVM audio test by jvsTSX), which is not included either.

This is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by SEGA. "Dreamcast" and "Visual Memory Unit" are trademarks of their owner and are used here only to say what the emulator is compatible with.
