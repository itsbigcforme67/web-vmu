// web-vmu tools - build and inspect memory-card images on the PC with the same code the emulator uses.
// Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
//
//   vmucard new  <card.bin> [--game] [file.vms|file.dci ...]   make a formatted card holding these files
//                                                      (--game: store the following .vms as a mini-game whatever it looks like)
//   vmucard add  <card.bin> <file.vms|file.dci> [NAME] add a file to an existing card
//   vmucard list <card.bin>                            show what is on a card (.bin or .dcm)
#include "../core/vmufs.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>
#include <cctype>

using namespace vmu;

static std::vector<uint8_t> readFile(const char* p) {
  std::vector<uint8_t> v; FILE* f = fopen(p, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", p); exit(2); }
  uint8_t b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
  fclose(f); return v;
}
static DateTime nowDate() {
  if (getenv("VMUCARD_FIXED_DATE")) return DateTime{2026, 1, 2, 3, 4, 5};
  time_t t = time(nullptr); tm* l = localtime(&t);
  return DateTime{(uint16_t)(l->tm_year + 1900), (uint8_t)(l->tm_mon + 1), (uint8_t)l->tm_mday, (uint8_t)l->tm_hour, (uint8_t)l->tm_min, (uint8_t)l->tm_sec};
}
static std::string baseName(const char* path) {
  std::string s(path); size_t a = s.find_last_of("/\\"); if (a != std::string::npos) s = s.substr(a + 1);
  size_t d = s.find_last_of('.'); if (d != std::string::npos) s = s.substr(0, d);
  std::string out; for (char c : s) if (out.size() < 12) out += (char)toupper((unsigned char)(isalnum((unsigned char)c) ? c : '_'));
  return out.empty() ? "GAME" : out;
}
static bool endsWith(const char* s, const char* ext) {
  size_t a = strlen(s), b = strlen(ext); if (a < b) return false;
  for (size_t i = 0; i < b; i++) if (tolower((unsigned char)s[a - b + i]) != ext[i]) return false;
  return true;
}
static bool forceGame = false;
static int addOne(uint8_t* flash, const char* path, const char* name) {
  std::vector<uint8_t> d = readFile(path);
  if (endsWith(path, ".dci")) return fs::addDci(flash, d.data(), d.size());
  const bool game = forceGame || fs::looksLikeGame(d.data(), d.size());
  return fs::addFile(flash, name ? name : baseName(path).c_str(), game ? fs::TYPE_GAME : fs::TYPE_DATA, d.data(), d.size(), nowDate());
}
static void save(const char* path, const uint8_t* flash) {
  FILE* f = fopen(path, "wb"); if (!f || fwrite(flash, 1, FLASH_SIZE, f) != FLASH_SIZE) { fprintf(stderr, "cannot write %s\n", path); exit(2); } fclose(f);
}

int main(int argc, char** argv) {
  static uint8_t flash[FLASH_SIZE];
  if (argc >= 3 && !strcmp(argv[1], "new")) {
    fs::format(flash, nowDate());
    for (int i = 3; i < argc; i++) { if (!strcmp(argv[i], "--game")) { forceGame = true; continue; } int r = addOne(flash, argv[i], nullptr); if (r < 0) { fprintf(stderr, "%s: error %d\n", argv[i], r); return 1; } }
    save(argv[2], flash); return 0;
  }
  if (argc >= 4 && !strcmp(argv[1], "add")) {
    std::vector<uint8_t> c = readFile(argv[2]); if (c.size() != FLASH_SIZE) { fprintf(stderr, "not a 128 KiB card image\n"); return 1; }
    memcpy(flash, c.data(), FLASH_SIZE);
    int r = addOne(flash, argv[3], argc > 4 ? argv[4] : nullptr); if (r < 0) { fprintf(stderr, "error %d\n", r); return 1; }
    save(argv[2], flash); return 0;
  }
  if (argc >= 3 && !strcmp(argv[1], "list")) {
    std::vector<uint8_t> c = readFile(argv[2]); if (c.size() != FLASH_SIZE) { fprintf(stderr, "not a 128 KiB card image\n"); return 1; }
    if (endsWith(argv[2], ".dcm")) fs::fromDcm(flash, c.data(), c.size()); else memcpy(flash, c.data(), FLASH_SIZE);
    if (!fs::isFormatted(flash)) { printf("not formatted\n"); return 1; }
    fs::FileInfo f; char s[17], l[33];
    for (int i = 0; i < fs::DIR_ENTRIES; i++) if (fs::fileAt(flash, i, &f)) {
      fs::headerText(flash, f, s, l);
      printf("%-4s %-12s %3u blocks from %3u  %04u-%02u-%02u  \"%s\" / \"%s\"\n", f.type == fs::TYPE_GAME ? "GAME" : "DATA", f.name, f.blocks, f.firstBlock, f.created.year, f.created.month, f.created.day, s, l);
    }
    printf("%d of %d blocks free\n", fs::freeBlocks(flash), fs::USER_BLOCKS);
    return 0;
  }
  fprintf(stderr, "usage: vmucard new|add|list <card.bin> ...\n"); return 2;
}
