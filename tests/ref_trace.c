// web-vmu tests - reference trace from libElysianVMU (https://github.com/gyrovorbis/libevmu, MIT).
// Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
//
//   ref_trace <card.bin> <script> <total cycles> <checkpoint every N steps>
//
// Drives libevmu one instruction at a time with the same loop as tests/host_trace.cpp and prints
// the same lines, so the two outputs can be compared. Uses libevmu's private headers (built by
// tests/build_ref.sh, which also applies tests/libevmu-parity.patch).
#include <evmu/evmu_api.h>
#include <evmu/hw/evmu_device.h>
#include <evmu/hw/evmu_cpu.h>
#include <evmu/hw/evmu_rom.h>
#include <evmu/hw/evmu_pic.h>
#include <evmu/hw/evmu_timers.h>
#include <evmu/hw/evmu_gamepad.h>
#include <evmu/hw/evmu_sfr.h>
#include <evmu/hw/evmu_address_space.h>
#include <gimbal/utils/gimbal_date_time.h>
#include "hw/evmu_device_.h"
#include "hw/evmu_cpu_.h"
#include "hw/evmu_ram_.h"
#include "hw/evmu_timers_.h"
#include "hw/evmu_flash_.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t REGS[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0E, 0x10, 0x11, 0x13, 0x15, 0x18, 0x1A, 0x1B, 0x1C, 0x1D,
                                0x20, 0x22, 0x25, 0x27, 0x44, 0x45, 0x46, 0x4C, 0x4D, 0x4E, 0x5D, 0x5E, 0x5F, 0x63, 0x64, 0x65, 0x7F };

static uint32_t stateHash(EvmuDevice* dev, EvmuDevice_* d) {
  uint32_t h = 2166136261u;
#define PUT(x) h = (h ^ (uint32_t)(x)) * 16777619u
  PUT(EvmuCpu_pc(dev->pCpu));
  for (size_t i = 0; i < sizeof REGS; i++) PUT(d->pRam->sfr[REGS[i]]);
  PUT(d->pRam->sfr[0x0D]);
  PUT(d->pTimers->timer0.base.tl & 0xff); PUT(d->pTimers->timer0.base.th & 0xff);
  PUT(d->pTimers->timer1.base.tl & 0xff); PUT(d->pTimers->timer1.base.th & 0xff);
  PUT(d->pTimers->baseTimer.counter);
  for (int b = 0; b < 2; b++) for (int i = 0; i < 0x100; i++) PUT(d->pRam->ram[b][i]);
  for (int b = 0; b < 3; b++) for (int i = 0; i < 0x80; i++) PUT(d->pRam->xram[b][i]);
#undef PUT
  return h;
}

static void setCycles(EvmuCpu_* cpu, int c) {     // what EvmuCpu_cycles() will report to the timers
  cpu->curInstr.encoded.bytes[0] = c >= 7 ? 0x30 : c >= 4 ? 0x10 : c >= 2 ? 0x01 : 0x00;   // MUL, CALLR, BR, NOP
}

int main(int argc, char** argv) {
  if (argc < 5) { fprintf(stderr, "usage: ref_trace <card.bin> <script> <cycles> <every>\n"); return 2; }
  const unsigned long long total = strtoull(argv[3], NULL, 10);
  const unsigned every = (unsigned)atoi(argv[4]);

  EvmuDevice* dev = EvmuDevice_create();
  EvmuDevice_* d = EVMU_DEVICE_(dev);
  FILE* f = fopen(argv[1], "rb");
  if (!f || fread(d->pFlash->pStorage->pData, 1, 0x20000, f) != 0x20000) { fprintf(stderr, "cannot read card image\n"); return 2; }
  fclose(f);
  EvmuIBehavior_reset(EVMU_IBEHAVIOR(dev));
  GblDateTime dt = { .date = { .year = 2026, .month = 1, .day = 2 }, .time = { .hours = 3, .minutes = 4, .seconds = 5 } };
  EvmuRom_setDateTime(dev->pRom, &dt);
  d->pRam->sfr[0x0D] |= 0x08;        // EXT as the real firmware leaves it when it starts a game (libevmu's shortcut start has 0x01)

  unsigned long long evCyc[4096]; int evMask[4096]; int nev = 0, ei = 0;
  f = fopen(argv[2], "r");
  if (f) { while (nev < 4096 && fscanf(f, "%llu %d", &evCyc[nev], &evMask[nev]) == 2) nev++; fclose(f); }

  EvmuCpu_* cpu = EVMU_CPU_(dev->pCpu);
  unsigned long long cyc = 0; unsigned step = 0; uint32_t flow = 2166136261u; int prev = 0, mask = 0;
  setCycles(cpu, 1);
  while (cyc < total) {
    while (ei < nev && evCyc[ei] <= cyc) {
      const int m = evMask[ei++];
      EvmuGamepad* g = dev->pGamepad;
      g->up = m & 1; g->down = (m >> 1) & 1; g->left = (m >> 2) & 1; g->right = (m >> 3) & 1;
      g->a = (m >> 4) & 1; g->b = (m >> 5) & 1; g->mode = (m >> 6) & 1; g->sleep = (m >> 7) & 1;
      if (m & ~mask) EvmuIBehavior_update(EVMU_IBEHAVIOR(g), 0);       // a button went down: port 3 interrupt
      mask = m;
    }
    const uint8_t pcon = d->pRam->sfr[0x07];
    const int hold = (pcon & 2) != 0, halt = (pcon & 1) != 0;
    if (!hold) {
      EvmuPic_update(dev->pPic);
      const uint8_t keep = cpu->curInstr.encoded.bytes[0];
      setCycles(cpu, prev ? prev : 1);
      EvmuTimers_update(dev->pTimers);
      cpu->curInstr.encoded.bytes[0] = keep;
    }
    int c = 1;
    if (!halt && !hold) {
      EvmuCpu_runNext(dev->pCpu);
      c = (int)EvmuCpu_cycles(dev->pCpu);
      if (EvmuRom_biosActive(dev->pRom)) { printf("X %u %llu\n", step + 1, cyc + c); break; }   // the game left for the system menu
    }
    prev = c; cyc += (unsigned long long)c; step++;
    flow = (flow ^ EvmuCpu_pc(dev->pCpu)) * 16777619u;
    flow = (flow ^ (uint32_t)c) * 16777619u;
    if (step % every == 0) printf("C %u %llu %08x %08x\n", step, cyc, flow, stateHash(dev, d));
    if (getenv("TRACE_DUMP") && step % every == 0) {
      printf("D pc=%04x", EvmuCpu_pc(dev->pCpu));
      for (size_t i = 0; i < sizeof REGS; i++) printf(" %02x", d->pRam->sfr[REGS[i]]);
      printf(" | %x %02x %02x %02x %02x %04x", d->pRam->sfr[0x0D], d->pTimers->timer0.base.tl & 0xff, d->pTimers->timer0.base.th & 0xff, d->pTimers->timer1.base.tl & 0xff, d->pTimers->timer1.base.th & 0xff, d->pTimers->baseTimer.counter);
      uint32_t h = 0; for (int b = 0; b < 2; b++) for (int i = 0; i < 0x100; i++) h = (h ^ d->pRam->ram[b][i]) * 16777619u;
      uint32_t x = 0; for (int b = 0; b < 3; b++) for (int i = 0; i < 0x80; i++) x = (x ^ d->pRam->xram[b][i]) * 16777619u;
      printf(" ram=%08x xram=%08x\n", h, x);
    }
  }
  { uint32_t h = 2166136261u; for (uint32_t i = 0; i < 0x20000; i++) h = (h ^ d->pFlash->pStorage->pData[i]) * 16777619u; printf("F %08x\n", h); }
  return 0;
}
