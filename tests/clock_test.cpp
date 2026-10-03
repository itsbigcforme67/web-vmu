// web-vmu tests - the emulated firmware clock (the service at 0x130 that games call twice a second)
// against the C library's calendar, across minute, day, month, year and leap-year boundaries.
// Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
//   g++ -std=c++11 -O2 -o clock_test tests/clock_test.cpp core/vmu.cpp core/vmufs.cpp && ./clock_test
#include "../core/vmu.h"
#include "../core/vmufs.h"
#include <cstdio>
#include <ctime>
#include <cstring>

using namespace vmu;

int main() {
  // A game that does nothing but call the clock service: NOT1 EXT,0 / JMPF 0x130, which returns to 0x139.
  static uint8_t game[1024], flash[FLASH_SIZE];
  memset(game, 0xB0, 0x100);                                  // every interrupt vector: RETI
  const uint8_t start[] = {0x21, 0x01, 0x2A};                 // JMPF 0x12A
  const uint8_t call[] = {0xB8, 0x0D, 0x21, 0x01, 0x30};      // NOT1 EXT,0 ; JMPF 0x130
  memcpy(game, start, 3); memcpy(game + 0x12A, call, 5); memcpy(game + 0x139, start, 3);
  memcpy(game + 0x200, "CLOCK TEST      ", 16);

  const DateTime starts[] = {{1999, 12, 31, 23, 59, 50}, {2024, 2, 28, 23, 59, 0}, {2026, 2, 28, 12, 0, 0}, {2100, 2, 28, 23, 0, 0}, {2000, 2, 28, 22, 30, 30}, {2026, 10, 2, 8, 15, 0}};
  int failed = 0; long checked = 0;
  for (const DateTime& s : starts) {
    fs::format(flash, s);
    if (fs::addFile(flash, "CLOCK", fs::TYPE_GAME, game, sizeof game, s) < 0) { printf("could not make the test card\n"); return 1; }
    static Vmu m;
    m.init(flash, nullptr);
    m.reset(s);
    tm t0; memset(&t0, 0, sizeof t0);
    t0.tm_year = s.year - 1900; t0.tm_mon = s.month - 1; t0.tm_mday = s.day; t0.tm_hour = s.hour; t0.tm_min = s.minute; t0.tm_sec = s.second;
    const time_t base = timegm(&t0);
    const long seconds = 400L * 86400;                        // 400 days: every month end, one February of each kind
    long calls = 0;
    for (long sec = 1; sec <= seconds && !failed; sec++) {
      for (; calls < sec * 2; ) { const uint16_t before = m.pc(); m.step(); if (before == 0x12A && m.pc() == 0x139) calls++; if (m.exited()) { printf("the test game exited\n"); return 1; } }
      const DateTime d = m.dateTime();
      const time_t want = base + sec; tm w; gmtime_r(&want, &w);
      checked++;
      if (d.year != w.tm_year + 1900 || d.month != w.tm_mon + 1 || d.day != w.tm_mday || d.hour != w.tm_hour || d.minute != w.tm_min || d.second != w.tm_sec) {
        printf("FAIL from %04u-%02u-%02u after %ld s: clock says %04u-%02u-%02u %02u:%02u:%02u, calendar says %04d-%02d-%02d %02d:%02d:%02d\n", s.year, s.month, s.day, sec,
               d.year, d.month, d.day, d.hour, d.minute, d.second, w.tm_year + 1900, w.tm_mon + 1, w.tm_mday, w.tm_hour, w.tm_min, w.tm_sec);
        failed++;
      }
    }
  }
  printf("%ld seconds of clock checked from %d starting dates, %d failed\n", checked, (int)(sizeof starts / sizeof starts[0]), failed);
  return failed ? 1 : 0;
}
