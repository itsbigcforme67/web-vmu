// Web VMU - M5Stack CoreS3 firmware
// Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
// Hardware: CoreS3 + Faces Bottom3 + Gamepad3 (falls back to on-screen touch buttons).
//
// Controls (Gamepad3):  D-pad = D-pad, A = A, B = B, SELECT = MODE, START = menu (which also has SLEEP).
//
// The emulator is ../../core (the same source the website's vmu.wasm is built from). It runs in its
// own task on CPU core 0 and is purely passive: the task tells it how much time has passed and it
// catches up. Drawing, input and sound stay on core 1.
#include <M5Unified.h>
#include <LittleFS.h>
#include <vector>
#include "../../core/vmu.h"
#include "../../core/vmufs.h"
#include "demo_game.h"

using vmu::Vmu;
namespace vfs = vmu::fs;

// ------------------------------------------------------------------ pad input
enum : uint8_t { P_UP = 0x01, P_DOWN = 0x02, P_LEFT = 0x04, P_RIGHT = 0x08, P_A = 0x10, P_B = 0x20, P_SELECT = 0x40, P_START = 0x80 };
static const uint8_t GP_ADDR = 0x08, GP_REG_KEYS = 0x00;
static bool gpPresent = false;
static uint8_t gpLastRaw = 0xFF, gpState = 0;
static uint8_t pad = 0, padPrev = 0, padPressed = 0;
static uint32_t padRepeatAt = 0;

static bool gpReadRaw(uint8_t& raw) { return M5.In_I2C.readRegister(GP_ADDR, GP_REG_KEYS, &raw, 1, 400000); }
static void gpInit() {
  uint8_t raw = 0xFF;
  gpPresent = gpReadRaw(raw);
  // The panel reports the key byte as an event: keep the first value as a baseline and
  // only act on changes (avoids phantom presses at power-on).
  if (gpPresent) gpLastRaw = raw;
  gpState = 0;
  Serial.printf("Gamepad3 %s (first byte %02x)\n", gpPresent ? "found" : "not found - using touch controls", raw);
}
static void gpPoll() {
  uint8_t raw;
  if (!gpPresent || !gpReadRaw(raw)) return;
  if (raw != gpLastRaw) { gpLastRaw = raw; gpState = (uint8_t)~raw; }
}

// Touch fallback (and touch shortcuts in the menus). VMU-only buttons get their own bits above the pad's.
enum : uint16_t { T_MODE = 0x100, T_SLEEP = 0x200 };
struct Zone { int16_t x, y, w, h; uint16_t bit; };
static std::vector<Zone> touchZones;
static uint16_t touchBits = 0;
static uint16_t touchPad() {
  uint16_t m = 0;
  int n = M5.Touch.getCount();
  for (int i = 0; i < n; i++) {
    auto t = M5.Touch.getDetail(i);
    if (!t.isPressed()) continue;
    for (auto& z : touchZones) if (t.x >= z.x && t.x < z.x + z.w && t.y >= z.y && t.y < z.y + z.h) m |= z.bit;
  }
  return m;
}
static void readInput() {
  gpPoll();
  padPrev = pad;
  touchBits = touchPad();
  pad = gpState | (uint8_t)touchBits;
  padPressed = pad & ~padPrev;
  // key repeat for menu navigation
  if (pad & (P_UP | P_DOWN)) {
    if (padPressed & (P_UP | P_DOWN)) padRepeatAt = millis() + 380;
    else if (millis() > padRepeatAt) { padPressed |= pad & (P_UP | P_DOWN); padRepeatAt = millis() + 90; }
  }
}

// ------------------------------------------------------------------ clock
static vmu::DateTime nowDT() {
  vmu::DateTime t = {2026, 1, 1, 12, 0, 0};
  if (M5.Rtc.isEnabled()) {
    auto d = M5.Rtc.getDateTime();
    if (d.date.year >= 2024 && d.date.year < 2100) {
      t.year = (uint16_t)d.date.year; t.month = (uint8_t)d.date.month; t.day = (uint8_t)d.date.date;
      t.hour = (uint8_t)d.time.hours; t.minute = (uint8_t)d.time.minutes; t.second = (uint8_t)d.time.seconds;
    }
  }
  return t;
}

// ------------------------------------------------------------------ graphics helpers
static M5Canvas canvas(&M5.Display);
static const uint16_t C_BG = 0x10A3, C_PANEL = 0x2124, C_HI = 0x3A8F, C_TEXT = 0xEF7D, C_MUTED = 0x9CF3;
static uint16_t LCD_BG, LCD_INK, LCD_GAP, LCD_FAINT;

static void toast(const String& msg, uint32_t ms = 1800) {
  canvas.fillRoundRect(16, 92, 288, 56, 10, C_PANEL);
  canvas.drawRoundRect(16, 92, 288, 56, 10, C_HI);
  canvas.setTextColor(C_TEXT);
  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_center);
  canvas.setTextWrap(false);
  String m = msg;
  if (m.length() > 40) {   // two lines
    int cut = m.lastIndexOf(' ', 40); if (cut < 10) cut = 40;
    canvas.drawString(m.substring(0, cut), 160, 110);
    canvas.drawString(m.substring(cut + 1), 160, 130);
  } else canvas.drawString(m, 160, 120);
  canvas.pushSprite(0, 0);
  if (ms) delay(ms);
}
static void status(const char* msg) { toast(msg, 0); }

// ------------------------------------------------------------------ memory cards on the device
// /cards/<name>.bin   whole 128 KiB cards, the same files the website downloads and emulators use
// /states/<name>.st   where each card's game was left
struct Entry {
  String path, name, title, detail;
  bool game = false;
  int freeBlocks = 0;
  std::vector<uint16_t> icon;     // 32 x 32, RGB565, 0 = see-through
};
static std::vector<Entry> lib;
static uint8_t* cardBuf = nullptr;      // 128 KiB, the running card (the emulated flash)
static uint8_t* cardShadow = nullptr;   // copy written to storage while the game keeps running

static String baseOf(const String& path) {
  int a = path.lastIndexOf('/'), d = path.lastIndexOf('.');
  return path.substring(a + 1, d > a ? d : path.length());
}
static String statePath(const Entry& e) { return "/states/" + e.name + ".st"; }

static bool loadCard(const String& path, uint8_t* buf) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  const bool ok = f.size() == vmu::FLASH_SIZE && f.read(buf, vmu::FLASH_SIZE) == vmu::FLASH_SIZE;
  f.close();
  return ok;
}
static bool saveCard(const String& path, const uint8_t* buf) {
  const String tmp = path + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  const bool ok = f.write(buf, vmu::FLASH_SIZE) == vmu::FLASH_SIZE;
  f.close();
  if (!ok) { LittleFS.remove(tmp); return false; }
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}

// A loose game or save file (.vms, .dci) dropped into /cards becomes a card of its own.
static void wrapLooseFile(const String& path) {
  File f = LittleFS.open(path, "r");
  if (!f) return;
  const size_t len = f.size();
  uint8_t* data = (len && len <= 200 * 512 + 32) ? (uint8_t*)ps_malloc(len) : nullptr;
  if (data && f.read(data, len) != len) { free(data); data = nullptr; }
  f.close();
  if (!data) return;
  vfs::format(cardBuf, nowDT());
  String lower = path; lower.toLowerCase();
  String name = baseOf(path); name.toUpperCase();
  int r;
  if (lower.endsWith(".dci")) r = vfs::addDci(cardBuf, data, len);
  else r = vfs::addFile(cardBuf, name.substring(0, 12).c_str(), vfs::looksLikeGame(data, len) ? vfs::TYPE_GAME : vfs::TYPE_DATA, data, len, nowDT());
  free(data);
  if (r >= 0 && saveCard("/cards/" + baseOf(path) + ".bin", cardBuf)) LittleFS.remove(path);
  Serial.printf("%s -> card: %s\n", path.c_str(), r >= 0 ? "ok" : "not a VMU file");
}

static bool makeDemoCard() {
  vfs::format(cardBuf, nowDT());
  if (vfs::addFile(cardBuf, "SKETCH", vfs::TYPE_GAME, DEMO_GAME, sizeof DEMO_GAME, nowDT()) < 0) return false;
  return saveCard("/cards/Sketch.bin", cardBuf);
}

static void describe(Entry& e) {
  e.game = false; e.title = e.name; e.detail = ""; e.icon.clear();
  if (!vfs::isFormatted(cardBuf)) { e.detail = "not a formatted card"; e.freeBlocks = 0; return; }
  e.freeBlocks = vfs::freeBlocks(cardBuf);
  vfs::FileInfo g;
  if (!vfs::findGame(cardBuf, &g)) { e.detail = "no game on this card"; return; }
  char s[17], l[33];
  vfs::headerText(cardBuf, g, s, l);
  e.game = true;
  e.title = s[0] ? String(s) : String(g.name);
  e.detail = l;
  // the game's first icon: 16 colours (4 bits each of alpha, red, green, blue), then 32 x 32 dots, 4 bits each
  const uint8_t* h = cardBuf + 0x200;
  const unsigned count = h[0x40] | (h[0x41] << 8);
  if (count >= 1 && count <= 64) {
    uint16_t pal[16];
    for (int i = 0; i < 16; i++) {
      const uint16_t v = (uint16_t)(h[0x60 + i * 2] | (h[0x61 + i * 2] << 8));
      const uint8_t a = v >> 12, r = (v >> 8) & 15, gg = (v >> 4) & 15, b = v & 15;
      const uint16_t c = M5.Display.color565(r * 17, gg * 17, b * 17);
      pal[i] = a < 8 ? 0 : (c ? c : 0x0001);
    }
    e.icon.resize(32 * 32);
    for (int i = 0; i < 512; i++) { e.icon[i * 2] = pal[h[0x80 + i] >> 4]; e.icon[i * 2 + 1] = pal[h[0x80 + i] & 15]; }
  }
}

static void scan() {
  lib.clear();
  LittleFS.mkdir("/cards"); LittleFS.mkdir("/states");
  std::vector<String> loose, cards;
  File dir = LittleFS.open("/cards");
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String n = String(f.name()); String lower = n; lower.toLowerCase();
    const String path = n.startsWith("/") ? n : "/cards/" + n;
    if (lower.endsWith(".vms") || lower.endsWith(".dci")) loose.push_back(path);
    else if (lower.endsWith(".bin") || lower.endsWith(".vmu")) cards.push_back(path);
  }
  dir.close();
  for (auto& p : loose) { wrapLooseFile(p); cards.push_back("/cards/" + baseOf(p) + ".bin"); }
  if (cards.empty() && makeDemoCard()) cards.push_back("/cards/Sketch.bin");
  for (auto& p : cards) {
    bool seen = false;
    for (auto& e : lib) if (e.path == p) seen = true;
    if (seen || !loadCard(p, cardBuf)) continue;
    Entry e; e.path = p; e.name = baseOf(p);
    describe(e);
    lib.push_back(e);
  }
  Serial.printf("%u memory card(s)\n", (unsigned)lib.size());
}

// ------------------------------------------------------------------ emulator session
static Vmu emu;
static uint8_t* stateBuf = nullptr;
static size_t stateCap = 0;
static int16_t audioRing[8192];
static int16_t audioChunks[4][512];
static int audioChunk = 0;
static volatile bool cardDirty = false;
static volatile uint32_t lastFlashWrite = 0;
static int curGame = -1;
static uint8_t frameBuf[vmu::LCD_H * vmu::LCD_ROW_BYTES], shownBuf[vmu::LCD_H * vmu::LCD_ROW_BYTES];
static uint8_t shownIcons = 0xFF;
static bool lcdFull = true;
static uint32_t lastDraw = 0, sleepUntil = 0;
static bool muted = false;
static int volume = 128;

enum Mode { M_LIBRARY, M_GAME, M_PAUSE, M_SYSMENU };
static Mode mode = M_LIBRARY;
static int libSel = 0, libTop = 0, menuSel = 0;

static void onFlashWrite(void*, uint32_t) { cardDirty = true; lastFlashWrite = millis(); }

// ---- the emulator runs in its own task on CPU core 0; drawing, input and audio stay on core 1
static TaskHandle_t emuTask = nullptr;
static volatile bool emuWanted = false, emuIdle = true;
static volatile uint8_t emuButtons = 0;
static volatile uint32_t emuLoadPct10 = 0;

static void emuTaskFn(void*) {
  uint32_t last = micros(), busy = 0, winStart = millis();
  for (;;) {
    if (!emuWanted) { emuIdle = true; vTaskDelay(pdMS_TO_TICKS(5)); last = micros(); busy = 0; winStart = millis(); continue; }
    emuIdle = false;
    emu.setButtons(emuButtons);
    uint32_t now = micros(), dt = now - last;
    last = now;
    if (dt > 100000) dt = 100000;
    if (dt) emu.runUs(dt);
    busy += micros() - now;
    if (millis() - winStart >= 5000) {
      emuLoadPct10 = (uint32_t)((uint64_t)busy * 1000 / ((millis() - winStart) * 1000ull));
      busy = 0; winStart = millis();
    }
    vTaskDelay(1);  // always yield (keeps the core-0 watchdog happy); time lost here is made up next round
  }
}
static void emuStart() { emuWanted = true; }
static void emuStop() { emuWanted = false; while (!emuIdle) delay(1); }

static void saveCardIfDirty(bool force) {
  if (!cardDirty || curGame < 0) return;
  if (!force && millis() - lastFlashWrite < 3000) return;
  const bool running = emuWanted;
  if (running) emuStop();
  memcpy(cardShadow, cardBuf, vmu::FLASH_SIZE);   // ~1 ms; then write the copy while the game keeps running
  cardDirty = false;
  if (running) emuStart();
  if (!saveCard(lib[curGame].path, cardShadow)) Serial.println("could not write the card");
}
static void saveState() {
  if (curGame < 0) return;
  const size_t n = emu.saveState(stateBuf, stateCap);
  if (!n) return;
  File f = LittleFS.open(statePath(lib[curGame]), "w");
  if (f) { f.write(stateBuf, n); f.close(); }
}

// LCD geometry: 6x scale in gamepad mode (288 x 192), 4x with touch controls underneath
static int lcdScale = 6, lcdX = 16, lcdY = 4;

// the four symbols under the dot matrix: file, game, clock, flash
static void drawVmuIcon(int which, int x, int y, uint16_t c) {
  auto& d = M5.Display;
  switch (which) {
    case 0: d.drawRect(x + 3, y + 1, 10, 14, c); d.drawFastHLine(x + 5, y + 6, 6, c); d.drawFastHLine(x + 5, y + 9, 6, c); d.drawFastHLine(x + 5, y + 12, 6, c); break;
    case 1: d.drawRoundRect(x + 3, y + 1, 10, 14, 2, c); d.fillTriangle(x + 8, y + 4, x + 5, y + 8, x + 11, y + 8, c); d.fillTriangle(x + 8, y + 12, x + 5, y + 8, x + 11, y + 8, c); break;
    case 2: d.drawCircle(x + 8, y + 8, 6, c); d.drawFastVLine(x + 8, y + 4, 5, c); d.drawFastHLine(x + 8, y + 8, 4, c); break;
    default: d.fillTriangle(x + 9, y + 1, x + 4, y + 9, x + 8, y + 9, c); d.fillTriangle(x + 7, y + 15, x + 12, y + 7, x + 8, y + 7, c); break;
  }
}

static void drawGameChrome() {
  auto& d = M5.Display;
  d.fillScreen(C_BG);
  lcdScale = gpPresent ? 6 : 4;
  const int w = vmu::LCD_W * lcdScale, h = vmu::LCD_H * lcdScale;
  lcdX = (320 - w) / 2;
  lcdY = 6;
  d.fillRoundRect(lcdX - 8, lcdY - 4, w + 16, h + 8 + 22, 8, 0x5ACB);
  d.fillRect(lcdX - 2, lcdY - 1, w + 4, h + 2 + 20, LCD_BG);
  d.setFont(&fonts::Font2);
  auto& e = lib[curGame];
  String t = e.title; if (t.length() > 26) t = t.substring(0, 26);
  touchZones.clear();
  if (gpPresent) {
    d.setTextColor(C_MUTED, C_BG);
    d.setTextDatum(top_left);
    d.drawString(t, 6, 224);
    d.setTextDatum(top_right);
    d.drawString("SELECT: mode  START: menu", 314, 224);
  } else {
    // on-screen controls under the LCD
    const int y0 = lcdY + h + 28, cy = y0 + 30, cx = 46, b = 24;
    const char* lbl[4] = {"^", "v", "<", ">"};
    const int bx[4] = {cx - b / 2, cx - b / 2, cx - b / 2 - b, cx + b / 2};
    const int by[4] = {cy - b / 2 - b + 4, cy + b / 2 - 4, cy - b / 2, cy - b / 2};
    const uint8_t bits[4] = {P_UP, P_DOWN, P_LEFT, P_RIGHT};
    d.setTextDatum(middle_center);
    for (int i = 0; i < 4; i++) {
      d.fillRoundRect(bx[i], by[i], b, b, 5, 0x5ACB);
      d.setTextColor(C_TEXT);
      d.drawString(lbl[i], bx[i] + b / 2, by[i] + b / 2);
      touchZones.push_back({(int16_t)(bx[i] - 5), (int16_t)(by[i] - 3), (int16_t)(b + 10), (int16_t)(b + 6), bits[i]});
    }
    d.fillCircle(246, cy + 10, 17, 0x5ACB); d.drawString("A", 246, cy + 10);
    touchZones.push_back({224, (int16_t)(cy - 10), 44, 44, P_A});
    d.fillCircle(292, cy - 8, 17, 0x5ACB); d.drawString("B", 292, cy - 8);
    touchZones.push_back({272, (int16_t)(cy - 30), 48, 44, P_B});
    const char* mid[3] = {"SLEEP", "MODE", "MENU"};
    const uint16_t midBits[3] = {T_SLEEP, T_MODE, P_START};
    for (int i = 0; i < 3; i++) {
      const int x = 96 + i * 42;
      d.fillRoundRect(x, cy - 10, 38, 22, 11, i == 2 ? C_HI : 0x7BEF);
      d.setFont(&fonts::Font0);
      d.drawString(mid[i], x + 19, cy + 1);
      d.setFont(&fonts::Font2);
      touchZones.push_back({(int16_t)(x - 2), (int16_t)(cy - 18), 42, 38, midBits[i]});
    }
  }
  lcdFull = true;
  shownIcons = 0xFF;
}

static M5Canvas lcdStrip(&M5.Display);   // one LCD row (48*scale x scale), rendered in RAM

static void drawLcd() {
  const bool on = emu.lcdOn();
  if (on) emu.frame(frameBuf); else memset(frameBuf, 0, sizeof frameBuf);
  const int s = lcdScale, w = vmu::LCD_W * s;
  if (lcdStrip.width() != w || lcdStrip.height() != s) { lcdStrip.deleteSprite(); lcdStrip.setColorDepth(16); lcdStrip.createSprite(w, s); }
  M5.Display.startWrite();
  for (int y = 0; y < vmu::LCD_H; y++) {
    const uint8_t* row = frameBuf + y * vmu::LCD_ROW_BYTES;
    if (!lcdFull && !memcmp(row, shownBuf + y * vmu::LCD_ROW_BYTES, vmu::LCD_ROW_BYTES)) continue;
    lcdStrip.fillScreen(LCD_GAP);
    for (int x = 0; x < vmu::LCD_W; x++) lcdStrip.fillRect(x * s, 0, s - 1, s - 1, ((row[x >> 3] >> (7 - (x & 7))) & 1) ? LCD_INK : LCD_BG);
    lcdStrip.pushSprite(&M5.Display, lcdX, lcdY + y * s);
    memcpy(shownBuf + y * vmu::LCD_ROW_BYTES, row, vmu::LCD_ROW_BYTES);
  }
  const uint8_t icons = on ? emu.icons() : 0;
  if (icons != shownIcons || lcdFull) {
    const int y = lcdY + vmu::LCD_H * s + 3, step = vmu::LCD_W * s / 4;
    for (int i = 0; i < 4; i++) {
      const int x = lcdX + step * i + step / 2 - 8;
      M5.Display.fillRect(x, y, 16, 16, LCD_BG);
      drawVmuIcon(i, x, y, (icons & (1 << i)) ? LCD_INK : LCD_FAINT);
    }
    shownIcons = icons;
  }
  M5.Display.endWrite();
  lcdFull = false;
}

static void pumpAudio() {
  while (emu.audioAvailable() >= 512) {
    if (!muted && M5.Speaker.isPlaying(0) >= 2) break;
    int16_t* out = audioChunks[audioChunk];
    emu.audioRead(out, 512);
    if (!muted) { M5.Speaker.playRaw(out, 512, 16000, false, 1, 0); audioChunk = (audioChunk + 1) & 3; }
  }
}

static void drawLibrary();

static void startGame(int gi) {
  emuStop();
  auto& e = lib[gi];
  if (!e.game) { toast("There is no game on this card"); drawLibrary(); return; }
  if (!loadCard(e.path, cardBuf)) { toast("Couldn't read that card"); drawLibrary(); return; }
  curGame = gi;
  cardDirty = false;
  emu.init(cardBuf, nullptr);          // no BIOS: the firmware services games call are emulated
  emu.onFlashWrite = onFlashWrite;
  emu.audioLevel = 6000;
  emu.audioInit(16000, audioRing, 8192);
  emu.reset(nowDT());
  bool resumed = false;
  File f = LittleFS.open(statePath(e), "r");
  if (f) {
    const size_t n = f.read(stateBuf, stateCap);
    f.close();
    resumed = emu.loadState(stateBuf, n);
    if (resumed) emu.setDateTime(nowDT()); else emu.reset(nowDT());
  }
  if (emu.exited()) { toast("This game could not be started"); curGame = -1; drawLibrary(); return; }
  memset(shownBuf, 0, sizeof shownBuf);
  drawGameChrome();
  mode = M_GAME;
  emuButtons = 0;
  emuStart();
  Serial.printf("Started %s %s\n", e.title.c_str(), resumed ? "from its snapshot" : "fresh");
}

static void quitGame(bool keepState) {
  emuStop();
  if (curGame >= 0) {
    if (keepState) saveState(); else LittleFS.remove(statePath(lib[curGame]));
    saveCardIfDirty(true);
  }
  M5.Speaker.stop();
  curGame = -1;
  mode = M_LIBRARY;
  touchZones.clear();
  drawLibrary();
}

// ------------------------------------------------------------------ library screen
static const int ROW_H = 46, LIST_Y = 34, ROWS = 4;

static void drawLibrary() {
  touchZones.clear();
  canvas.fillScreen(C_BG);
  canvas.setTextDatum(middle_left);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextColor(C_TEXT);
  canvas.drawString("VMU", 10, 16);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(C_MUTED);
  canvas.setTextDatum(middle_right);
  canvas.drawString(String(M5.Power.getBatteryLevel()) + "%", 312, 16);
  if (lib.empty()) {
    canvas.setTextDatum(middle_center);
    canvas.drawString("No memory cards found.", 160, 100);
    canvas.drawString("Put card images or .vms files in data/cards", 160, 124);
    canvas.drawString("and run: pio run -t uploadfs", 160, 144);
  }
  if (libSel >= (int)lib.size()) libSel = lib.size() ? lib.size() - 1 : 0;
  if (libSel < libTop) libTop = libSel;
  if (libSel >= libTop + ROWS) libTop = libSel - ROWS + 1;
  for (int r = 0; r < ROWS && libTop + r < (int)lib.size(); r++) {
    const int i = libTop + r;
    auto& e = lib[i];
    const int y = LIST_Y + r * ROW_H;
    canvas.fillRoundRect(6, y + 2, 308, ROW_H - 4, 8, i == libSel ? C_HI : C_PANEL);
    canvas.fillRoundRect(12, y + 5, 36, 36, 5, LCD_BG);
    if (e.icon.size() == 32 * 32) {
      for (int py = 0; py < 32; py++) for (int px = 0; px < 32; px++) { const uint16_t c = e.icon[py * 32 + px]; if (c) canvas.drawPixel(14 + px, y + 7 + py, c); }
    }
    canvas.setTextDatum(top_left);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    canvas.setTextColor(e.game ? C_TEXT : C_MUTED);
    String t = e.title; if (t.length() > 24) t = t.substring(0, 24);
    canvas.drawString(t, 56, y + 6);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_MUTED);
    String sub = e.game ? e.name + ".bin  -  " + String(e.freeBlocks) + " blocks free" : e.name + ".bin  -  " + e.detail;
    canvas.drawString(sub, 56, y + 28);
  }
  canvas.setTextColor(C_MUTED);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::Font2);
  if (libTop > 0) canvas.drawString("^", 160, LIST_Y - 2);
  canvas.fillRect(0, 218, 320, 22, C_PANEL);
  canvas.setFont(&fonts::Font0);
  canvas.drawString(gpPresent ? "A: play    START: settings" : "Tap a game to play   Tap the top bar: settings", 160, 229);
  canvas.pushSprite(0, 0);
}

static int libraryTapRow() {
  static bool wasDown = false;
  bool down = false; int ty = -1;
  if (M5.Touch.getCount()) { auto t = M5.Touch.getDetail(0); if (t.isPressed()) { down = true; ty = t.y; } }
  int r = -1;
  if (down && !wasDown) {
    if (ty < 30) r = -2;  // top bar -> settings
    else if (ty >= LIST_Y && ty < LIST_Y + ROWS * ROW_H) r = libTop + (ty - LIST_Y) / ROW_H;
    else if (ty >= 210) r = -3;  // footer -> scroll down
  }
  wasDown = down;
  return r;
}

// ------------------------------------------------------------------ menus
static void drawMenu(const char* title, const std::vector<String>& items, int sel) {
  canvas.fillScreen(C_BG);
  canvas.setTextDatum(middle_left);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(title, 12, 18);
  for (size_t i = 0; i < items.size(); i++) {
    const int y = 38 + i * 34;
    canvas.fillRoundRect(8, y, 304, 30, 7, (int)i == sel ? C_HI : C_PANEL);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(items[i], 20, y + 15);
  }
  canvas.setFont(&fonts::Font0);
  canvas.setTextColor(C_MUTED);
  canvas.setTextDatum(middle_center);
  canvas.drawString(gpPresent ? "UP/DOWN choose   A select   B back" : "Tap an option", 160, 229);
  canvas.pushSprite(0, 0);
}
static int menuTap(int count) {
  static bool wasDown = false;
  bool down = false; int ty = -1;
  if (M5.Touch.getCount()) { auto t = M5.Touch.getDetail(0); if (t.isPressed()) { down = true; ty = t.y; } }
  int r = -1;
  if (down && !wasDown && ty >= 38) { const int i = (ty - 38) / 34; if (i < count) r = i; }
  wasDown = down;
  return r;
}

static String soundLabel() { return String("Sound: ") + (muted ? String("off") : String(volume * 100 / 255) + "%"); }
static std::vector<String> pauseItems() { return {"Resume", "Press the VMU's SLEEP button", "Restart game", "Save & back to library", soundLabel()}; }
static std::vector<String> sysItems() { return {"Back", soundLabel(), "Add the Sketch test game again", "Look for new cards"}; }

static void cycleVolume() {
  if (muted) { muted = false; volume = 64; }
  else if (volume < 100) volume = 128;
  else if (volume < 180) volume = 220;
  else muted = true;
  M5.Speaker.setVolume(muted ? 0 : volume);
}

// How fast can this chip run the emulator? (Printed once at start-up, on the serial monitor.)
static void speedTest() {
  vfs::format(cardBuf, nowDT());
  if (vfs::addFile(cardBuf, "SKETCH", vfs::TYPE_GAME, DEMO_GAME, sizeof DEMO_GAME, nowDT()) < 0) return;
  emu.init(cardBuf, nullptr);
  emu.audioInit(16000, audioRing, 8192);
  emu.reset(nowDT());
  const uint32_t t0 = micros();
  for (int i = 0; i < 20; i++) { emu.runUs(100000); emu.audioRead(audioChunks[0], 512); while (emu.audioAvailable()) emu.audioRead(audioChunks[0], 512); }   // 2 emulated seconds
  const uint32_t us = micros() - t0;
  const double cps = (double)emu.totalCycles * 1e6 / (us ? us : 1);
  Serial.printf("speed test: %.2f million VMU cycles per second (a VMU runs at 0.15, and at most 1.0, million): %.0fx real time for this game\n",
                cps / 1e6, 2e6 / (us ? us : 1));
}

// ------------------------------------------------------------------ setup / loop
static void fatal(const String& a, const String& b) {
  M5.Display.fillScreen(C_BG);
  M5.Display.setTextColor(C_TEXT);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(a, 160, 100);
  M5.Display.setTextColor(C_MUTED);
  M5.Display.drawString(b, 160, 130);
  Serial.println(a + " - " + b);
  while (true) delay(1000);
}

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_spk = true;
  cfg.internal_mic = false;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(160);
  LCD_BG = M5.Display.color565(176, 189, 164);
  LCD_INK = M5.Display.color565(27, 37, 24);
  LCD_GAP = M5.Display.color565(183, 196, 171);
  LCD_FAINT = M5.Display.color565(160, 173, 149);
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  canvas.createSprite(320, 240);
  status("VMU starting...");

  cardBuf = (uint8_t*)heap_caps_malloc(vmu::FLASH_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!cardBuf) cardBuf = (uint8_t*)ps_malloc(vmu::FLASH_SIZE);
  cardShadow = (uint8_t*)ps_malloc(vmu::FLASH_SIZE);
  if (!cardShadow) cardShadow = (uint8_t*)malloc(vmu::FLASH_SIZE);
  stateCap = emu.saveState(nullptr, 0) + 64;
  stateBuf = (uint8_t*)malloc(stateCap);
  if (!cardBuf || !stateBuf || !cardShadow) fatal("Out of memory", "");
  Serial.printf("card buffer at %p, free heap %u, free PSRAM %u\n", cardBuf, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

  if (!LittleFS.begin(true)) fatal("Storage error", "Could not mount or format LittleFS");
  M5.Speaker.begin();
  M5.Speaker.setVolume(volume);
  gpInit();
  speedTest();
  status("Reading memory cards...");
  scan();
  xTaskCreatePinnedToCore(emuTaskFn, "vmu", 8192, nullptr, 2, &emuTask, 0);
  drawLibrary();
}

void loop() {
  M5.update();
  readInput();

  switch (mode) {
    case M_LIBRARY: {
      const int n = lib.size();
      const int tap = libraryTapRow();
      bool redraw = false;
      if ((padPressed & P_DOWN) || tap == -3) { if (libSel < n - 1) { libSel++; redraw = true; } }
      if (padPressed & P_UP) { if (libSel > 0) { libSel--; redraw = true; } }
      if (padPressed & P_RIGHT) { libSel = min(n - 1, libSel + ROWS); redraw = true; }
      if (padPressed & P_LEFT) { libSel = max(0, libSel - ROWS); redraw = true; }
      if (tap >= 0 && tap < n) { libSel = tap; startGame(libSel); break; }
      if ((padPressed & (P_A | P_B)) && n) { startGame(libSel); break; }
      if ((padPressed & P_START) || tap == -2) { mode = M_SYSMENU; menuSel = 0; drawMenu("Settings", sysItems(), menuSel); break; }
      if (padPressed & P_SELECT) { cycleVolume(); redraw = true; }
      if (redraw) drawLibrary();
      delay(10);
      break;
    }

    case M_GAME: {
      if (emu.exited()) { quitGame(false); toast("The game ended (MODE). Its saves are on the card.", 1400); drawLibrary(); break; }
      if (padPressed & P_START) { emuStop(); saveCardIfDirty(true); M5.Speaker.stop(); mode = M_PAUSE; menuSel = 0; touchZones.clear(); drawMenu("Paused", pauseItems(), menuSel); break; }
      uint8_t btn = 0;
      if (pad & P_UP) btn |= vmu::BTN_UP;
      if (pad & P_DOWN) btn |= vmu::BTN_DOWN;
      if (pad & P_LEFT) btn |= vmu::BTN_LEFT;
      if (pad & P_RIGHT) btn |= vmu::BTN_RIGHT;
      if (pad & P_A) btn |= vmu::BTN_A;
      if (pad & P_B) btn |= vmu::BTN_B;
      if ((gpState & P_SELECT) || (touchBits & T_MODE)) btn |= vmu::BTN_MODE;
      if ((touchBits & T_SLEEP) || millis() < sleepUntil) btn |= vmu::BTN_SLEEP;
      emuButtons = btn;
      pumpAudio();
      if (millis() - lastDraw >= 33) {
        lastDraw = millis();
        drawLcd();
      }
      saveCardIfDirty(false);
      static uint32_t lastReport = 0;
      if (millis() - lastReport > 5000) {
        lastReport = millis();
        Serial.printf("emu load %u.%u%%  %u cycles/s  %s\n", (unsigned)(emuLoadPct10 / 10), (unsigned)(emuLoadPct10 % 10), (unsigned)emu.cyclesPerSecond(), emu.sleeping() ? "asleep" : emu.halted() ? "idle" : "running");
      }
      delay(4);
      break;
    }

    case M_PAUSE: {
      auto items = pauseItems();
      const int tap = menuTap(items.size());
      if (padPressed & P_DOWN) menuSel = (menuSel + 1) % items.size();
      if (padPressed & P_UP) menuSel = (menuSel + items.size() - 1) % items.size();
      int choice = -1;
      if (padPressed & P_A) choice = menuSel;
      if (tap >= 0) choice = tap;
      if (padPressed & (P_B | P_START)) choice = 0;
      if (choice == 0 || choice == 1) {
        if (choice == 1) sleepUntil = millis() + 250;
        drawGameChrome(); mode = M_GAME; emuStart(); break;
      }
      if (choice == 2) {
        LittleFS.remove(statePath(lib[curGame]));
        emu.reset(nowDT());
        drawGameChrome(); mode = M_GAME; emuStart(); break;
      }
      if (choice == 3) { quitGame(true); break; }
      if (choice == 4) cycleVolume();
      if (padPressed || tap >= 0) drawMenu("Paused", pauseItems(), menuSel);
      delay(10);
      break;
    }

    case M_SYSMENU: {
      auto items = sysItems();
      const int tap = menuTap(items.size());
      if (padPressed & P_DOWN) menuSel = (menuSel + 1) % items.size();
      if (padPressed & P_UP) menuSel = (menuSel + items.size() - 1) % items.size();
      int choice = -1;
      if (padPressed & P_A) choice = menuSel;
      if (tap >= 0) choice = tap;
      if (padPressed & (P_B | P_START)) choice = 0;
      if (choice == 0) { mode = M_LIBRARY; drawLibrary(); break; }
      if (choice == 1) cycleVolume();
      if (choice == 2 || choice == 3) {
        if (choice == 2) { LittleFS.remove("/states/Sketch.st"); status("Adding Sketch..."); if (!makeDemoCard()) toast("Could not write the card"); }
        status("Reading memory cards...");
        scan();
        mode = M_LIBRARY; drawLibrary(); break;
      }
      if (padPressed || tap >= 0) drawMenu("Settings", sysItems(), menuSel);
      delay(10);
      break;
    }
  }
}
