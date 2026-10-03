// web-vmu tests - run the C++ core on the PC and print a trace in the same format as
// tests/ref_trace.c (libElysianVMU), optionally with a save/load round trip at every checkpoint.
// Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
//
//   host_trace <card.bin> <script> <total cycles> <checkpoint every N steps> [--state-test]
#include "../core/vmu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace vmu;

static uint32_t stateHash(const Vmu& m) {
  static uint32_t regs[2048];
  uint32_t h = 2166136261u;
  const size_t n = m.debugRegs(regs, 2048);
  for (size_t i = 0; i < n; i++) h = (h ^ regs[i]) * 16777619u;
  return h;
}

int main(int argc, char** argv) {
  if (argc < 5) { fprintf(stderr, "usage: host_trace <card.bin> <script> <cycles> <every> [--state-test]\n"); return 2; }
  const unsigned long long total = strtoull(argv[3], nullptr, 10);
  const unsigned every = (unsigned)atoi(argv[4]);
  const bool stateTest = argc > 5 && !strcmp(argv[5], "--state-test");

  static uint8_t flash[FLASH_SIZE];
  FILE* f = fopen(argv[1], "rb");
  if (!f || fread(flash, 1, FLASH_SIZE, f) != FLASH_SIZE) { fprintf(stderr, "cannot read card image\n"); return 2; }
  fclose(f);
  struct Ev { unsigned long long cyc; int mask; };
  std::vector<Ev> evs;
  f = fopen(argv[2], "r");
  if (f) { Ev e; while (fscanf(f, "%llu %d", &e.cyc, &e.mask) == 2) evs.push_back(e); fclose(f); }

  static int16_t ring[4096], sink[4096];
  Vmu* m = new Vmu();
  m->init(flash, nullptr);
  m->audioInit(16000, ring, 4096);
  const DateTime now = {2026, 1, 2, 3, 4, 5};
  m->reset(now);

  std::vector<uint8_t> snap;
  int restores = 0;
  size_t ei = 0; unsigned long long cyc = 0; unsigned step = 0; uint32_t flow = 2166136261u;
  while (cyc < total) {
    while (ei < evs.size() && evs[ei].cyc <= cyc) m->setButtons((uint8_t)evs[ei++].mask);
    const int c = m->step();
    if (m->exited()) { printf("X %u %llu\n", step + 1, cyc + c); break; }
    cyc += (unsigned long long)c; step++;
    flow = (flow ^ m->pc()) * 16777619u;
    flow = (flow ^ (uint32_t)c) * 16777619u;
    if (step % every == 0) {
      printf("C %u %llu %08x %08x\n", step, cyc, flow, stateHash(*m));
      if (getenv("TRACE_DUMP")) {
        static uint32_t r[2048]; m->debugRegs(r, 2048);
        printf("D pc=%04x", r[0]);
        for (int i = 1; i <= 37; i++) printf(" %02x", r[i]);
        printf(" | %x %02x %02x %02x %02x %04x", r[38], r[39], r[40], r[41], r[42], r[43]);
        uint32_t h = 0; for (int i = 0; i < 0x200; i++) h = (h ^ r[44 + i]) * 16777619u;
        uint32_t x = 0; for (int i = 0; i < 0x180; i++) x = (x ^ r[44 + 0x200 + i]) * 16777619u;
        printf(" ram=%08x xram=%08x\n", h, x);
      }
      m->audioRead(sink, 4096);
      if (stateTest) {
        // snapshot, throw the machine away, build a fresh one, restore, and carry on with that one
        snap.resize(m->saveState(nullptr, 0));
        if (snap.empty() || m->saveState(snap.data(), snap.size()) != snap.size()) { fprintf(stderr, "saveState failed\n"); return 1; }
        Vmu* twin = new Vmu();
        twin->init(flash, nullptr);
        twin->audioInit(16000, ring, 4096);
        twin->reset(now);
        if (!twin->loadState(snap.data(), snap.size())) { fprintf(stderr, "loadState failed\n"); return 1; }
        if (stateHash(*twin) != stateHash(*m)) { fprintf(stderr, "state differs right after load at step %u\n", step); return 1; }
        delete m; m = twin; restores++;
      }
    }
  }
  { uint32_t h = 2166136261u; for (uint32_t i = 0; i < FLASH_SIZE; i++) h = (h ^ flash[i]) * 16777619u; printf("F %08x\n", h); }   // what ended up on the card
  if (stateTest) fprintf(stderr, "state test: %d save/load round trips\n", restores);
  return 0;
}
