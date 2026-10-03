// web-vmu core - WebAssembly interface (one VMU per module instance).
// Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
// Built with plain clang (no Emscripten, no C library); see build.sh. web/core/vmu.js wraps it.
#include "vmu.h"
#include "vmufs.h"

using namespace vmu;

#define EXPORT(name) __attribute__((export_name(#name))) name

extern "C" {
// the compiler may emit calls to these even though nothing here includes a C library
void* memset(void* d, int c, size_t n) { uint8_t* p = (uint8_t*)d; while (n--) *p++ = (uint8_t)c; return d; }
void* memcpy(void* d, const void* s, size_t n) { uint8_t* p = (uint8_t*)d; const uint8_t* q = (const uint8_t*)s; while (n--) *p++ = *q++; return d; }
void* memmove(void* d, const void* s, size_t n) {
  uint8_t* p = (uint8_t*)d; const uint8_t* q = (const uint8_t*)s;
  if (p < q) { while (n--) *p++ = *q++; } else { p += n; q += n; while (n--) *--p = *--q; }
  return d;
}
void __cxa_pure_virtual() { __builtin_trap(); }
}

struct PlaceTag {};
inline void* operator new(size_t, PlaceTag, void* p) noexcept { return p; }

static const uint32_t ARENA_SIZE = 384 * 1024, RING_SIZE = 16384;
alignas(16) static uint8_t g_arena[ARENA_SIZE];
alignas(16) static uint8_t g_flash[FLASH_SIZE];
alignas(16) static uint8_t g_bios[BIOS_SIZE];
alignas(16) static uint8_t g_mem[sizeof(Vmu)];
static int16_t g_ring[RING_SIZE];
static uint32_t g_arenaUsed = 0;
static Vmu* g_v = nullptr;
static bool g_useBios = false;

static DateTime date(int y, int mo, int d, int h, int mi, int s) {
  DateTime t; t.year = (uint16_t)y; t.month = (uint8_t)mo; t.day = (uint8_t)d; t.hour = (uint8_t)h; t.minute = (uint8_t)mi; t.second = (uint8_t)s;
  return t;
}

extern "C" {

// Must be called first. Returns the interface version.
int EXPORT(vmu_init)() {
  g_arenaUsed = 0;
  g_v = new (PlaceTag(), g_mem) Vmu();
  g_useBios = false;
  g_v->init(g_flash, nullptr);
  return 1;
}

// Scratch memory for passing files and save states in and out.
void* EXPORT(vmu_alloc)(uint32_t n) {
  n = (n + 7) & ~7u;
  if (g_arenaUsed + n > ARENA_SIZE) return nullptr;
  void* p = g_arena + g_arenaUsed;
  g_arenaUsed += n;
  return p;
}

uint8_t* EXPORT(vmu_flash)() { return g_flash; }          // the 128 KiB memory card the machine runs from
uint8_t* EXPORT(vmu_bios)() { return g_bios; }            // 64 KiB; fill it, then vmu_use_bios(1)
void EXPORT(vmu_use_bios)(int on) { g_useBios = on != 0; if (g_v) g_v->init(g_flash, g_useBios ? g_bios : nullptr); }

void EXPORT(vmu_reset)(int y, int mo, int d, int h, int mi, int s) { if (g_v) g_v->reset(date(y, mo, d, h, mi, s)); }
void EXPORT(vmu_set_time)(int y, int mo, int d, int h, int mi, int s) { if (g_v) g_v->setDateTime(date(y, mo, d, h, mi, s)); }
void EXPORT(vmu_audio)(uint32_t sampleRate, int level) { if (g_v) { g_v->audioLevel = level; g_v->audioInit(sampleRate, g_ring, RING_SIZE); } }
void EXPORT(vmu_buttons)(int mask) { if (g_v) g_v->setButtons((uint8_t)mask); }
void EXPORT(vmu_run_us)(uint32_t us) { if (g_v) g_v->runUs(us); }
int EXPORT(vmu_step)() { return g_v ? g_v->step() : 0; }
uint32_t EXPORT(vmu_pc)() { return g_v ? g_v->pc() : 0; }
uint32_t EXPORT(vmu_cps)() { return g_v ? g_v->cyclesPerSecond() : 0; }

// Writes 32 rows of 6 bytes (bit 7 = leftmost pixel). Returns: bit 0 = LCD on, bits 8-11 = icons (file, game, clock, flash).
uint32_t EXPORT(vmu_frame)(uint8_t* out) {
  if (!g_v) return 0;
  g_v->frame(out);
  return (g_v->lcdOn() ? 1u : 0u) | ((uint32_t)g_v->icons() << 8);
}
uint32_t EXPORT(vmu_audio_read)(int16_t* out, uint32_t max) { return g_v ? g_v->audioRead(out, max) : 0; }

// bit 0 = game left for the system menu, 1 = asleep (HOLD), 2 = halted, 3 = card written since vmu_clear_dirty(), 4 = BIOS in use
uint32_t EXPORT(vmu_status)() {
  if (!g_v) return 0;
  return (g_v->exited() ? 1u : 0) | (g_v->sleeping() ? 2u : 0) | (g_v->halted() ? 4u : 0) | (g_v->flashDirty ? 8u : 0) | (g_v->hasBios() ? 16u : 0);
}
void EXPORT(vmu_clear_dirty)() { if (g_v) g_v->flashDirty = false; }

uint32_t EXPORT(vmu_state_save)(uint8_t* out, uint32_t cap) { return g_v ? (uint32_t)g_v->saveState(out, cap) : 0; }
int EXPORT(vmu_state_load)(const uint8_t* in, uint32_t len) { return g_v && g_v->loadState(in, len); }

// FNV-1a over the registers, timers, RAM and display RAM: the number tests/ref_trace.c prints for libElysianVMU.
uint32_t EXPORT(vmu_state_hash)() {
  if (!g_v) return 0;
  static uint32_t regs[2048];
  uint32_t h = 2166136261u;
  const size_t n = g_v->debugRegs(regs, 2048);
  for (size_t i = 0; i < n && i < 2048; i++) h = (h ^ regs[i]) * 16777619u;
  return h;
}
uint32_t EXPORT(vmu_flash_hash)() { uint32_t h = 2166136261u; for (uint32_t i = 0; i < FLASH_SIZE; i++) h = (h ^ g_flash[i]) * 16777619u; return h; }

// ---- the card's file system (all of these work on the image at vmu_flash())
int EXPORT(vmu_fs_formatted)() { return fs::isFormatted(g_flash); }
void EXPORT(vmu_fs_format)(int y, int mo, int d, int h, int mi, int s) { fs::format(g_flash, date(y, mo, d, h, mi, s)); }
int EXPORT(vmu_fs_free)() { return fs::freeBlocks(g_flash); }

// Describes directory slot `index` in 96 bytes at `out`: type, copy-protected, first block (2), blocks (2), year (2),
// month, day, hour, minute, then name[13], short title[17], long title[33], NUL-terminated. Returns 0 if the slot is empty.
int EXPORT(vmu_fs_file)(int index, uint8_t* out) {
  fs::FileInfo f;
  if (!fs::fileAt(g_flash, index, &f)) return 0;
  char s[17], l[33];
  fs::headerText(g_flash, f, s, l);
  for (int i = 0; i < 96; i++) out[i] = 0;
  out[0] = f.type; out[1] = f.copyProtected; out[2] = (uint8_t)f.firstBlock; out[3] = (uint8_t)(f.firstBlock >> 8);
  out[4] = (uint8_t)f.blocks; out[5] = (uint8_t)(f.blocks >> 8); out[6] = (uint8_t)f.created.year; out[7] = (uint8_t)(f.created.year >> 8);
  out[8] = f.created.month; out[9] = f.created.day; out[10] = f.created.hour; out[11] = f.created.minute;
  for (int i = 0; i < 13; i++) out[12 + i] = (uint8_t)f.name[i];
  for (int i = 0; i < 17; i++) out[25 + i] = (uint8_t)s[i];
  for (int i = 0; i < 33; i++) out[42 + i] = (uint8_t)l[i];
  return 1;
}
int EXPORT(vmu_fs_add)(const char* name, int type, const uint8_t* data, uint32_t len, int y, int mo, int d, int h, int mi, int s, int copyProtected) {
  return fs::addFile(g_flash, name, (uint8_t)type, data, len, date(y, mo, d, h, mi, s), copyProtected != 0);
}
int EXPORT(vmu_fs_remove)(int index) { return fs::removeFile(g_flash, index); }
uint32_t EXPORT(vmu_fs_read)(int index, uint8_t* out, uint32_t cap) {
  fs::FileInfo f;
  return fs::fileAt(g_flash, index, &f) ? (uint32_t)fs::readFile(g_flash, f, out, cap) : 0;
}
int EXPORT(vmu_fs_add_dci)(const uint8_t* dci, uint32_t len) { return fs::addDci(g_flash, dci, len); }
int EXPORT(vmu_fs_from_dcm)(const uint8_t* dcm, uint32_t len) { return fs::fromDcm(g_flash, dcm, len); }
int EXPORT(vmu_fs_looks_like_game)(const uint8_t* vms, uint32_t len) { return fs::looksLikeGame(vms, len); }
// Reads a .VMI: writes file name[13] then description[33] at `out`; returns bit 0 = valid, bit 1 = game, bit 2 = copy-protected.
int EXPORT(vmu_fs_parse_vmi)(const uint8_t* vmi, uint32_t len, uint8_t* out) {
  fs::VmiInfo v;
  if (!fs::parseVmi(vmi, len, &v)) return 0;
  for (int i = 0; i < 13; i++) out[i] = (uint8_t)v.fileName[i];
  for (int i = 0; i < 33; i++) out[13 + i] = (uint8_t)v.description[i];
  return 1 | (v.game ? 2 : 0) | (v.copyProtected ? 4 : 0);
}

}  // extern "C"
