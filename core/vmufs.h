// web-vmu core - the VMU's flash file system and the file formats memory cards travel in.
// Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
//
// A memory card is 256 blocks of 512 bytes: 200 user blocks, then system blocks at the top
// (root 255, allocation table 254, directory 253..241). The one mini-game a card can hold always
// sits at block 0 upward in one piece; ordinary save files are stored from block 199 downward.
// All of this works on the same 128 KiB image the emulator runs from. No heap, no libc.
#pragma once
#include "vmu.h"

namespace vmu {
namespace fs {

enum { BLOCK_SIZE = 512, BLOCKS = 256, USER_BLOCKS = 200, ROOT_BLOCK = 255, FAT_BLOCK = 254, DIR_BLOCK = 253, DIR_BLOCKS = 13, DIR_ENTRIES = 208 };
enum : uint8_t { TYPE_NONE = 0x00, TYPE_DATA = 0x33, TYPE_GAME = 0xCC };
enum { OK = 0, ERR_NOT_FORMATTED = -1, ERR_NO_SPACE = -2, ERR_HAS_GAME = -3, ERR_DIR_FULL = -4, ERR_BAD_FILE = -5, ERR_EXISTS = -6 };

struct FileInfo {
  int index;                // directory slot
  uint8_t type;             // TYPE_GAME or TYPE_DATA
  bool copyProtected;
  uint16_t firstBlock, blocks, headerBlock;
  char name[13];            // 12 characters, NUL-terminated here
  DateTime created;
};

struct VmiInfo {
  char description[33], copyright[33], resource[9], fileName[13];
  DateTime created;
  bool game, copyProtected;
  uint32_t size;
};

bool isFormatted(const uint8_t* flash);
void format(uint8_t* flash, const DateTime& now);
bool fileAt(const uint8_t* flash, int index, FileInfo* out);          // false if that directory slot is empty
bool findGame(const uint8_t* flash, FileInfo* out);
bool findFile(const uint8_t* flash, const char* name, FileInfo* out);
int freeBlocks(const uint8_t* flash);
// Stores a file. Returns the directory slot (>= 0) or an ERR_* value.
int addFile(uint8_t* flash, const char* name, uint8_t type, const uint8_t* data, size_t len, const DateTime& now, bool copyProtected = false);
bool removeFile(uint8_t* flash, int index);
size_t readFile(const uint8_t* flash, const FileInfo& f, uint8_t* out, size_t cap);

// Text from a file's VMS header: the 16-character name shown on the VMU and the 32-character one shown on the console.
void headerText(const uint8_t* flash, const FileInfo& f, char shortName[17], char longName[33]);
// Does this raw .VMS look like a mini-game (code first, header in its second block) rather than a save file?
bool looksLikeGame(const uint8_t* vms, size_t len);

// Container formats
bool parseVmi(const uint8_t* vmi, size_t len, VmiInfo* out);           // .VMI: the description that accompanies a .VMS
int addDci(uint8_t* flash, const uint8_t* dci, size_t len);            // .DCI: one file (directory entry + data, byte-swapped)
bool fromDcm(uint8_t* flash, const uint8_t* dcm, size_t len);          // .DCM: a whole card, byte-swapped

}  // namespace fs
}  // namespace vmu
