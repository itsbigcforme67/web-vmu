// Web VMU - self-test of the emulator core on the chip itself (no screen or buttons needed).
// Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
//
//   pio run -e coretest -t upload -t monitor
//
// 1. Runs two programs with scripted button presses and compares where they end up (program flow,
//    every register, RAM, the screen, the memory card) with what the PC build of the same core gets.
//    The numbers to match are in coretest_data.h (tools/make_esp32_test.py).
// 2. Times 5 emulated seconds, to show how much of the processor the emulator needs.
#include <Arduino.h>
#include <stdarg.h>
#include "../../core/vmu.h"
#include "../../core/vmufs.h"
#include "demo_game.h"
#include "coretest_data.h"

using namespace vmu;

static uint8_t* flash;
static Vmu emu;
static int16_t ring[4096], sink[512];
static const DateTime NOW = {2026, 1, 2, 3, 4, 5};

// everything printed is kept, and repeated every 10 seconds, so a serial monitor that attaches late still sees it
static char report[1400];
static size_t reportLen = 0;
static void say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(report + reportLen, sizeof report - reportLen, fmt, ap);
  va_end(ap);
  if (n > 0) { Serial.print(report + reportLen); reportLen += (size_t)n < sizeof report - reportLen ? (size_t)n : sizeof report - reportLen - 1; }
}

static uint32_t stateHash() {
  static uint32_t regs[2048];
  uint32_t h = 2166136261u;
  const size_t n = emu.debugRegs(regs, 2048);
  for (size_t i = 0; i < n; i++) h = (h ^ regs[i]) * 16777619u;
  return h;
}

static void makeCard(const char* name, const uint8_t* game, size_t len) {
  fs::format(flash, NOW);
  fs::addFile(flash, name, fs::TYPE_GAME, game, len, NOW);
  emu.init(flash, nullptr);
  emu.audioInit(16000, ring, 4096);
  emu.reset(NOW);
}

static bool check(const Expected& x, const uint8_t* game, size_t len) {
  makeCard(x.file, game, len);
  const size_t nev = sizeof SCRIPT / sizeof SCRIPT[0];
  size_t ei = 0; uint32_t cyc = 0, step = 0, flow = 2166136261u, lastStep = 0, lastCyc = 0, lastFlow = 0, lastState = 0;
  while (cyc < x.total) {
    while (ei < nev && SCRIPT[ei][0] <= cyc) emu.setButtons((uint8_t)SCRIPT[ei++][1]);
    const int c = emu.step();
    if (emu.exited()) break;
    cyc += (uint32_t)c; step++;
    flow = (flow ^ emu.pc()) * 16777619u;
    flow = (flow ^ (uint32_t)c) * 16777619u;
    if (step % x.every == 0) {
      lastStep = step; lastCyc = cyc; lastFlow = flow; lastState = stateHash();
      while (emu.audioAvailable()) emu.audioRead(sink, 512);
    }
  }
  uint32_t card = 2166136261u;
  for (uint32_t i = 0; i < FLASH_SIZE; i++) card = (card ^ flash[i]) * 16777619u;
  const bool ok = lastStep == x.steps && lastCyc == x.cycles && lastFlow == x.flow && lastState == x.state && card == x.card;
  say("%s  %s: %u instructions, flow %08x, state %08x, card %08x%s\n", ok ? "PASS" : "FAIL", x.label, (unsigned)lastStep, (unsigned)lastFlow, (unsigned)lastState, (unsigned)card,
                ok ? "" : "  <- differs from the PC build");
  return ok;
}

static void speed(const char* label, const char* name, const uint8_t* game, size_t len) {
  makeCard(name, game, len);
  const uint32_t t0 = micros();
  for (int i = 0; i < 50 && !emu.exited(); i++) { emu.runUs(100000); while (emu.audioAvailable()) emu.audioRead(sink, 512); }
  const uint32_t us = micros() - t0;
  say("SPEED %s: 5 emulated seconds (%u VMU cycles at %u per second) took %u us = %u.%u%% of one core\n", label, (unsigned)emu.totalCycles, (unsigned)emu.cyclesPerSecond(),
                (unsigned)us, (unsigned)(us / 50000), (unsigned)(us / 5000 % 10));
}

void setup() {
  Serial.begin(115200);
  delay(2500);                       // time for the serial monitor to attach
  flash = (uint8_t*)malloc(FLASH_SIZE);
  say("\nweb-vmu core self-test (the emulator itself uses %u bytes of RAM plus the 128 KiB card)\n", (unsigned)sizeof(Vmu));
  if (!flash) { say("FAIL  no memory for the card\n"); return; }
  bool ok = check(EXPECTED[0], DEMO_GAME, sizeof DEMO_GAME);
  ok = check(EXPECTED[1], FUZZ_GAME, sizeof FUZZ_GAME) && ok;
  speed("Sketch (RC clock, what games run on)", "SKETCH", DEMO_GAME, sizeof DEMO_GAME);
  speed("random program (32 kHz clock)", "FUZZ", FUZZ_GAME, sizeof FUZZ_GAME);
  say(ok ? "DONE: this chip runs the emulator exactly like the PC build\n" : "DONE: FAILED\n");
}
void loop() { delay(10000); Serial.print("\n(again)\n"); Serial.print(report); }
