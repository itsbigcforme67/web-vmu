// web-vmu core - the VMU's flash file system and card file formats. See vmufs.h.
// Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
#include "vmufs.h"

namespace vmu {
namespace fs {

static const uint16_t FAT_FREE = 0xFFFC, FAT_END = 0xFFFA;

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static inline unsigned unbcd(uint8_t v) { return (unsigned)((v >> 4) * 10 + (v & 15)); }

static void putStamp(uint8_t* p, const DateTime& d) {
  p[0] = bcd(d.year / 100); p[1] = bcd(d.year % 100); p[2] = bcd(d.month); p[3] = bcd(d.day);
  p[4] = bcd(d.hour); p[5] = bcd(d.minute); p[6] = bcd(d.second);
  // day of week, Monday = 0 (Sakamoto's method)
  static const uint8_t t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  unsigned y = d.year, m = d.month ? d.month : 1;
  if (m < 3) y--;
  const unsigned sunday0 = (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d.day) % 7;
  p[7] = (uint8_t)((sunday0 + 6) % 7);
}
static DateTime getStamp(const uint8_t* p) {
  DateTime d;
  d.year = (uint16_t)(unbcd(p[0]) * 100 + unbcd(p[1])); d.month = (uint8_t)unbcd(p[2]); d.day = (uint8_t)unbcd(p[3]);
  d.hour = (uint8_t)unbcd(p[4]); d.minute = (uint8_t)unbcd(p[5]); d.second = (uint8_t)unbcd(p[6]);
  return d;
}

bool isFormatted(const uint8_t* flash) {
  const uint8_t* root = flash + ROOT_BLOCK * BLOCK_SIZE;
  for (int i = 0; i < 16; i++) if (root[i] != 0x55) return false;
  return rd16(root + 0x46) < BLOCKS && rd16(root + 0x4A) < BLOCKS;
}

static const uint8_t* fatOf(const uint8_t* flash) { return flash + rd16(flash + ROOT_BLOCK * BLOCK_SIZE + 0x46) * BLOCK_SIZE; }
static uint8_t* fatOf(uint8_t* flash) { return flash + rd16(flash + ROOT_BLOCK * BLOCK_SIZE + 0x46) * BLOCK_SIZE; }
static inline uint16_t fatGet(const uint8_t* flash, unsigned b) { return rd16(fatOf(flash) + b * 2); }
static inline void fatSet(uint8_t* flash, unsigned b, uint16_t v) { wr16(fatOf(flash) + b * 2, v); }

// address of directory slot `index`, following the directory's block chain
static const uint8_t* dirSlot(const uint8_t* flash, int index) {
  if (index < 0 || !isFormatted(flash)) return nullptr;
  unsigned block = rd16(flash + ROOT_BLOCK * BLOCK_SIZE + 0x4A);
  for (int hop = index / 16; hop > 0; hop--) {
    block = fatGet(flash, block);
    if (block >= BLOCKS) return nullptr;
  }
  return flash + block * BLOCK_SIZE + (index % 16) * 32;
}

void format(uint8_t* flash, const DateTime& now) {
  for (uint32_t i = 0; i < FLASH_SIZE; i++) flash[i] = 0;
  uint8_t* root = flash + ROOT_BLOCK * BLOCK_SIZE;
  for (int i = 0; i < 16; i++) root[i] = 0x55;
  root[0x11] = root[0x12] = root[0x13] = root[0x14] = 0xFF;   // shell colour (unused while byte 0x10 is 0)
  putStamp(root + 0x30, now);
  wr16(root + 0x40, 255);              // last block
  wr16(root + 0x44, ROOT_BLOCK);
  wr16(root + 0x46, FAT_BLOCK); wr16(root + 0x48, 1);
  wr16(root + 0x4A, DIR_BLOCK); wr16(root + 0x4C, DIR_BLOCKS);
  wr16(root + 0x50, USER_BLOCKS);      // where the hidden area starts = number of user blocks
  wr16(root + 0x52, 31);               // its size, as the console formats it
  wr16(root + 0x54, 0); wr16(root + 0x56, 128);   // where a game goes, and its largest size
  for (unsigned b = 0; b < BLOCKS; b++) fatSet(flash, b, FAT_FREE);
  for (unsigned b = DIR_BLOCK; b > DIR_BLOCK - DIR_BLOCKS + 1; b--) fatSet(flash, b, (uint16_t)(b - 1));
  fatSet(flash, DIR_BLOCK - DIR_BLOCKS + 1, FAT_END);
  fatSet(flash, FAT_BLOCK, FAT_END);
  fatSet(flash, ROOT_BLOCK, FAT_END);
}

bool fileAt(const uint8_t* flash, int index, FileInfo* out) {
  const uint8_t* d = dirSlot(flash, index);
  if (!d || (d[0] != TYPE_GAME && d[0] != TYPE_DATA)) return false;
  if (out) {
    out->index = index; out->type = d[0]; out->copyProtected = d[1] == 0xFF;
    out->firstBlock = rd16(d + 2); out->blocks = rd16(d + 0x18); out->headerBlock = rd16(d + 0x1A);
    for (int i = 0; i < 12; i++) out->name[i] = (char)d[4 + i];
    out->name[12] = 0;
    for (int i = 11; i >= 0 && (out->name[i] == ' ' || out->name[i] == 0); i--) out->name[i] = 0;
    out->created = getStamp(d + 0x10);
  }
  return true;
}

bool findGame(const uint8_t* flash, FileInfo* out) {
  FileInfo f;
  for (int i = 0; i < DIR_ENTRIES; i++) if (fileAt(flash, i, &f) && f.type == TYPE_GAME) { if (out) *out = f; return true; }
  return false;
}

static bool nameEq(const char* a, const char* b) {
  for (int i = 0; i < 12; i++) {
    const char x = a[i] == ' ' ? 0 : a[i], y = b[i] == ' ' ? 0 : b[i];
    if (x != y) return false;
    if (!x) return true;
  }
  return true;
}

bool findFile(const uint8_t* flash, const char* name, FileInfo* out) {
  FileInfo f;
  for (int i = 0; i < DIR_ENTRIES; i++) if (fileAt(flash, i, &f) && nameEq(f.name, name)) { if (out) *out = f; return true; }
  return false;
}

int freeBlocks(const uint8_t* flash) {
  if (!isFormatted(flash)) return 0;
  int n = 0;
  for (unsigned b = 0; b < USER_BLOCKS; b++) if (fatGet(flash, b) == FAT_FREE) n++;
  return n;
}

int addFile(uint8_t* flash, const char* name, uint8_t type, const uint8_t* data, size_t len, const DateTime& now, bool copyProtected) {
  if (!isFormatted(flash)) return ERR_NOT_FORMATTED;
  if (!len || (type != TYPE_GAME && type != TYPE_DATA)) return ERR_BAD_FILE;
  const unsigned blocks = (unsigned)((len + BLOCK_SIZE - 1) / BLOCK_SIZE);
  if (blocks > USER_BLOCKS) return ERR_NO_SPACE;
  if (findFile(flash, name, nullptr)) return ERR_EXISTS;
  if (type == TYPE_GAME && findGame(flash, nullptr)) return ERR_HAS_GAME;

  int slot = -1;
  for (int i = 0; i < DIR_ENTRIES && slot < 0; i++) { const uint8_t* d = dirSlot(flash, i); if (d && d[0] == TYPE_NONE) slot = i; }
  if (slot < 0) return ERR_DIR_FULL;

  // choose the blocks: a game needs 0..n-1, a save takes free blocks from the top down
  uint16_t chain[USER_BLOCKS];
  unsigned n = 0;
  if (type == TYPE_GAME) {
    for (unsigned b = 0; b < blocks; b++) { if (fatGet(flash, b) != FAT_FREE) return ERR_NO_SPACE; chain[n++] = (uint16_t)b; }
  } else {
    for (int b = USER_BLOCKS - 1; b >= 0 && n < blocks; b--) if (fatGet(flash, (unsigned)b) == FAT_FREE) chain[n++] = (uint16_t)b;
    if (n < blocks) return ERR_NO_SPACE;
  }
  for (unsigned i = 0; i < n; i++) {
    fatSet(flash, chain[i], i + 1 < n ? chain[i + 1] : FAT_END);
    uint8_t* dst = flash + chain[i] * BLOCK_SIZE;
    for (unsigned k = 0; k < BLOCK_SIZE; k++) { const size_t src = (size_t)i * BLOCK_SIZE + k; dst[k] = src < len ? data[src] : 0; }
  }
  uint8_t* d = const_cast<uint8_t*>(dirSlot(flash, slot));
  for (int i = 0; i < 32; i++) d[i] = 0;
  d[0] = type; d[1] = copyProtected ? 0xFF : 0x00;
  wr16(d + 2, chain[0]);
  bool ended = false;
  for (int i = 0; i < 12; i++) { if (!name[i]) ended = true; d[4 + i] = ended ? ' ' : (uint8_t)name[i]; }
  putStamp(d + 0x10, now);
  wr16(d + 0x18, (uint16_t)blocks);
  wr16(d + 0x1A, type == TYPE_GAME ? 1 : 0);
  return slot;
}

bool removeFile(uint8_t* flash, int index) {
  FileInfo f;
  if (!fileAt(flash, index, &f)) return false;
  unsigned b = f.firstBlock;
  for (int guard = 0; guard < BLOCKS && b < USER_BLOCKS; guard++) {
    const unsigned next = fatGet(flash, b);
    fatSet(flash, b, FAT_FREE);
    b = next;
  }
  uint8_t* d = const_cast<uint8_t*>(dirSlot(flash, index));
  for (int i = 0; i < 32; i++) d[i] = 0;
  return true;
}

size_t readFile(const uint8_t* flash, const FileInfo& f, uint8_t* out, size_t cap) {
  size_t n = 0;
  unsigned b = f.firstBlock;
  for (unsigned i = 0; i < f.blocks && b < BLOCKS; i++) {
    for (unsigned k = 0; k < BLOCK_SIZE && n < cap; k++) out[n++] = flash[b * BLOCK_SIZE + k];
    b = fatGet(flash, b);
  }
  return n;
}

static void cleanText(const uint8_t* src, int len, char* out) {
  int end = 0;
  for (int i = 0; i < len; i++) {
    const uint8_t c = src[i];
    out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : (c == 0 ? ' ' : '?');
    if (out[i] != ' ') end = i + 1;
  }
  out[end] = 0;
}

void headerText(const uint8_t* flash, const FileInfo& f, char shortName[17], char longName[33]) {
  // the header sits at the start of the file's block number `headerBlock` (0 for saves, 1 for games)
  unsigned b = f.firstBlock;
  for (unsigned i = 0; i < f.headerBlock && b < BLOCKS; i++) b = fatGet(flash, b);
  shortName[0] = longName[0] = 0;
  if (b >= BLOCKS) return;
  const uint8_t* h = flash + b * BLOCK_SIZE;
  cleanText(h, 16, shortName);
  cleanText(h + 0x10, 32, longName);
}

bool looksLikeGame(const uint8_t* vms, size_t len) {
  if (len < 0x280) return false;
  // a game's interrupt vectors are code: jumps, RETI or padding, never text
  auto text = [](const uint8_t* p, int n) { int ok = 0; for (int i = 0; i < n; i++) if ((p[i] >= 0x20 && p[i] < 0x7F) || p[i] >= 0x80) ok++; return ok; };
  const bool codeAtStart = vms[0] == 0x21 || vms[0] == 0x28 || vms[0] == 0x29 || (vms[0] & 0xEF) == 0x01 || vms[0] == 0x00;
  return codeAtStart && text(vms + 0x200, 16) >= 12 && text(vms, 16) < 12;
}

bool parseVmi(const uint8_t* v, size_t len, VmiInfo* out) {
  if (len < 108) return false;
  cleanText(v + 0x04, 32, out->description);
  cleanText(v + 0x24, 32, out->copyright);
  cleanText(v + 0x50, 8, out->resource);
  cleanText(v + 0x58, 12, out->fileName);
  out->created.year = rd16(v + 0x44); out->created.month = v[0x46]; out->created.day = v[0x47];
  out->created.hour = v[0x48]; out->created.minute = v[0x49]; out->created.second = v[0x4A];
  const uint16_t mode = rd16(v + 0x64);
  out->copyProtected = (mode & 1) != 0;
  out->game = (mode & 2) != 0;
  out->size = (uint32_t)v[0x68] | ((uint32_t)v[0x69] << 8) | ((uint32_t)v[0x6A] << 16) | ((uint32_t)v[0x6B] << 24);
  return out->fileName[0] != 0;
}

int addDci(uint8_t* flash, const uint8_t* dci, size_t len) {
  if (len < 32 + BLOCK_SIZE || ((len - 32) % BLOCK_SIZE) != 0) return ERR_BAD_FILE;
  if (dci[0] != TYPE_GAME && dci[0] != TYPE_DATA) return ERR_BAD_FILE;
  const size_t bodyLen = len - 32;
  if (bodyLen > (size_t)USER_BLOCKS * BLOCK_SIZE) return ERR_NO_SPACE;
  char name[13];
  for (int i = 0; i < 12; i++) name[i] = (char)dci[4 + i];
  name[12] = 0;
  // Store first with the bytes as they are, then undo the format's 4-byte swap in place.
  const DateTime when = getStamp(dci + 0x10);
  const int slot = addFile(flash, name, dci[0], dci + 32, bodyLen, when, dci[1] == 0xFF);
  if (slot < 0) return slot;
  FileInfo f;
  fileAt(flash, slot, &f);
  unsigned b = f.firstBlock;
  for (unsigned i = 0; i < f.blocks && b < BLOCKS; i++) {
    uint8_t* p = flash + b * BLOCK_SIZE;
    for (unsigned k = 0; k < BLOCK_SIZE; k += 4) { uint8_t t = p[k]; p[k] = p[k + 3]; p[k + 3] = t; t = p[k + 1]; p[k + 1] = p[k + 2]; p[k + 2] = t; }
    b = fatGet(flash, b);
  }
  uint8_t* d = const_cast<uint8_t*>(dirSlot(flash, slot));
  d[0x17] = dci[0x17];                                   // keep the original day of week
  wr16(d + 0x1A, rd16(dci + 0x1A));
  return slot;
}

bool fromDcm(uint8_t* flash, const uint8_t* dcm, size_t len) {
  if (len != FLASH_SIZE) return false;
  for (uint32_t k = 0; k < FLASH_SIZE; k += 4) { flash[k] = dcm[k + 3]; flash[k + 1] = dcm[k + 2]; flash[k + 2] = dcm[k + 1]; flash[k + 3] = dcm[k]; }
  return isFormatted(flash);
}

}  // namespace fs
}  // namespace vmu
