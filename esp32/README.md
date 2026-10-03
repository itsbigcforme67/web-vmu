# VMU for M5Stack CoreS3

The same emulator as the web version, for the **CoreS3 + Faces Bottom3 + Gamepad3**. `src/core_vmu.cpp` and `src/core_vmufs.cpp` pull in `../core`, so the firmware and the website run the very same code.

> **Status:** written while away from the hardware. It compiles and links with the real ESP32-S3 toolchain (about 650 KB of the 6.4 MB app partition; the storage partition has room for about 25 cards), and the emulator core passes its self-test on an emulated ESP32-S3. It has **not** been flashed to a real CoreS3 yet, so expect to adjust things on first run: screen layout, touch zones, sound level.

## Flashing

Plug the CoreS3 in with USB-C. In this `esp32` folder:

```bash
pio run -t upload -t monitor   # builds, flashes, then shows the serial log
```

The first build downloads the ESP32 toolchain, which takes a few minutes. If the upload can't find the port, hold the reset button for about 3 seconds until the green LED flashes (download mode), then run it again.

With nothing else on the device it creates one memory card holding the built-in test game, *Sketch* (D-pad draws, A wipes, B saves the picture to the card, MODE leaves).

### Your own games and cards

Put files in `data/cards/` and run:

```bash
pio run -t uploadfs
```

* `*.bin` / `*.vmu`: whole 128 KB cards, the same files the website downloads and Dreamcast emulators keep (`vmu_save_A1.bin`, …).
* `*.vms` / `*.dci`: single games or saves. At start-up each one is wrapped in a card of its own.

`uploadfs` **replaces everything on the device**, including snapshots and anything games saved to their cards, so copy cards off first once that matters. (Wi-Fi syncing, as on the PocketStation firmware, is the next step; see the main README.)

## Controls

| Gamepad3 | In a game | In the library / menus |
|---|---|---|
| D-pad | D-pad | move |
| A, B | A, B | A = play / select, B = back |
| SELECT | MODE | change volume |
| START | pause menu (resume, press SLEEP, restart, save & back, sound) | settings |

Without the Gamepad3, touch buttons appear under the screen (D-pad, A, B, SLEEP, MODE, MENU), and you tap games in the list to play them.

## Saving

* Leaving a game (START → *Save & back to library*) saves a snapshot, so the game resumes exactly where you were.
* When a game writes to its memory card, the card is saved on the device within a few seconds, and again when you leave.
* MODE ends the game, as on a real VMU; anything it saved is on its card.

## What the serial monitor shows

* At start-up: `speed test: … million VMU cycles per second`. A VMU runs at 0.15 million on its normal clock, so anything well above that is full speed.
* While playing, every 5 seconds: `emu load N%` (share of CPU core 0 the emulator used), the VMU clock in use, and whether the game is running, idle or asleep. If that ever goes above about 80%, tell me which game.

## Self-test (no screen needed)

```bash
pio run -e coretest -t upload -t monitor
```

Runs two programs with scripted button presses and compares where they end up (program flow, registers, RAM, screen, memory card) with what the PC build of the same core gets, then times 5 emulated seconds. Expected output:

```
PASS  sketch: 205000 instructions, flow b31f72cc, state 6888c657, card c6c9280c
PASS  random program: 59800 instructions, flow 2b2f212f, state 2180d282, card 4b039aaf
SPEED Sketch (RC clock, what games run on): 5 emulated seconds (…) took … us = …% of one core
DONE: this chip runs the emulator exactly like the PC build
```

The report repeats every 10 seconds in case the monitor attaches late. The two `PASS` lines were confirmed on an emulated ESP32-S3; the speed lines only mean something on a real chip. `tools/make_esp32_test.py` regenerates the expected values after a change to `core/`.

## Notes

* The emulator runs in its own task on CPU core 0 and only ever does `emu.runUs(time that passed)`; drawing, input and sound stay on core 1. That is the shape it needs to move into a Meshtastic build later.
* No BIOS is used. The firmware services games call are emulated.
* `src/demo_game.h` is built from `../demo/sketch.s` by `tools/vmuasm.py`.
* Games, saves and cards in `data/cards/` are not part of the repository.
