// web-vmu core - Sega Dreamcast Visual Memory Unit (VMU/VMS) emulator.
// Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
// See vmu.h. Register and timing behaviour follows libElysianVMU; where the two differ on purpose
// it is noted here and in tests/README.
#include "vmu.h"

namespace vmu {

// ---- special function registers (offsets from 0x100)
enum : uint8_t {
  ACC = 0x00, PSW = 0x01, B = 0x02, C = 0x03, TRL = 0x04, TRH = 0x05, SP = 0x06, PCON = 0x07, IE = 0x08, IP = 0x09,
  EXT = 0x0D, OCR = 0x0E, T0CNT = 0x10, T0PRR = 0x11, T0L = 0x12, T0LR = 0x13, T0H = 0x14, T0HR = 0x15,
  T1CNT = 0x18, T1LC = 0x1A, T1LR = 0x1B, T1HC = 0x1C, T1HR = 0x1D, MCR = 0x20, STAD = 0x22, CNR = 0x23, TDR = 0x24,
  XBNK = 0x25, VCCR = 0x27, SCON0 = 0x30, SBUF0 = 0x31, SBR = 0x32, SCON1 = 0x34, SBUF1 = 0x35,
  P1 = 0x44, P1DDR = 0x45, P1FCR = 0x46, P3 = 0x4C, P3DDR = 0x4D, P3INT = 0x4E, FPR = 0x54, P7 = 0x5C,
  I01CR = 0x5D, I23CR = 0x5E, ISL = 0x5F, VSEL = 0x63, VRMAD1 = 0x64, VRMAD2 = 0x65, VTRBF = 0x66, BTCR = 0x7F
};
enum : uint8_t { PSW_CY = 0x80, PSW_AC = 0x40, PSW_IRBK1 = 0x10, PSW_IRBK0 = 0x08, PSW_OV = 0x04, PSW_RAMBK0 = 0x02, PSW_P = 0x01 };
enum : uint8_t { PCON_HALT = 0x01, PCON_HOLD = 0x02 };
enum { IRQ_INT0 = 1, IRQ_INT1 = 2, IRQ_INT2_T0L = 3, IRQ_INT3_BT = 4, IRQ_T0H = 5, IRQ_T1 = 6, IRQ_SIO0 = 7, IRQ_SIO1 = 8, IRQ_RFB = 9, IRQ_P3 = 10 };
enum { PRI_LOW = 0, PRI_HIGH = 1, PRI_HIGHEST = 2 };

static const uint16_t IRQ_VECTOR[16] = {
  0x0000, 0x0003, 0x000B, 0x0013, 0x001B, 0x0023, 0x002B, 0x0033, 0x003B, 0x0043, 0x004B, 0x004F, 0x0052, 0x0055, 0x005A, 0x005D
};

static const uint32_t OSC_QUARTZ = 32768, OSC_RC = 879236, OSC_CF = 6000000;

// firmware entry points a game jumps to (from its own code at the same addresses) and where they return
enum : uint16_t { FM_WRT_EX = 0x100, FM_WRTA_EX = 0x108, FM_VRF_EX = 0x110, FM_PRD_EX = 0x120, TIMER_EX = 0x130, SLEEP_EX = 0x140, EXIT_EX = 0x1F0 };
// system variables the firmware keeps in RAM bank 0
enum : uint8_t { SYS_YEAR_HI_BCD = 0x10, SYS_YEAR_LO_BCD, SYS_MONTH_BCD, SYS_DAY_BCD, SYS_HOUR_BCD, SYS_MINUTE_BCD, SYS_SEC_BCD,
                 SYS_YEAR_HI, SYS_YEAR_LO, SYS_MONTH, SYS_DAY, SYS_HOUR, SYS_MINUTE, SYS_SEC, SYS_HALF_SEC, SYS_LEAP, SYS_DATE_SET = 0x31 };

// cycles and length per opcode
static uint8_t opCycles(uint8_t op) {
  switch (op) {
    case 0x10: case 0x11: return 4;                       // CALLR, BRF
    case 0x30: case 0x40: return 7;                       // MUL, DIV
    case 0x22: case 0x23: return 2;                       // MOV #i8,d9
    case 0x20: case 0x21: case 0x31: case 0x41: case 0x50: case 0x51: case 0xA0: case 0xB0: case 0xC1: case 0x01: case 0x80: case 0x90: return 2;
  }
  uint8_t lo = op & 0x0F, hi = op >> 4;
  if (lo >= 8) return (hi == 0xA || hi == 0xB || hi >= 0xC) ? 1 : 2;   // NOT1/CLR1/SET1 take 1; CALL/JMP/BPC/BP/BN take 2
  if (hi == 3 || hi == 4 || hi == 5 || hi == 6 || hi == 7) {
    if (hi == 6 || hi == 7) return (lo == 0 || lo == 1) ? 2 : 1;        // PUSH/POP 2, INC/DEC 1
    return 2;                                                           // BE, BNE, DBNZ
  }
  return 1;
}

static uint8_t g_cycles[256];                           // opCycles() as a table, filled in by init()

static inline uint8_t parity(uint8_t v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return v & 1; }
static inline uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static bool leapYear(unsigned y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static unsigned daysInMonth(unsigned y, unsigned m) {
  static const uint8_t d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (m == 2 && leapYear(y)) ? 29 : d[(m - 1) % 12];
}
static uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
  return ~c;
}

// ------------------------------------------------------------------------------------------ setup
void Vmu::init(uint8_t* flash, const uint8_t* bios) {
  flash_ = flash; bios_ = bios;
  for (int i = 0; i < 256; i++) g_cycles[i] = opCycles((uint8_t)i);
  sampleRate_ = 0; ring_ = nullptr; ringSize_ = 0; ringW_ = 0; ringR_ = 0;
}

void Vmu::reset(const DateTime& now) {
  for (int i = 0; i < 0x80; i++) sfr_[i] = 0;
  for (int b = 0; b < 2; b++) for (int i = 0; i < 0x100; i++) ram_[b][i] = 0;
  for (int b = 0; b < 3; b++) for (int i = 0; i < 0x80; i++) xram_[b][i] = 0;
  for (int i = 0; i < 0x200; i++) wram_[i] = 0;
  xbank_ = 0; pc_ = 0;
  t0l_ = t0h_ = t1l_ = t1h_ = 0; t0Base_ = 0; t0Scale_ = 256; t0Delay_ = t1Delay_ = btDelay_ = 0; btCounter_ = 0; btRemainder_ = 0;
  intReq_ = 0; intStack_[0] = intStack_[1] = intStack_[2] = 0; irqSkip_ = false; prevIntPriority_ = 0;
  prevCycles_ = 1; curCycles_ = 1; flashUnlock_ = 0; exited_ = false; cycleBudget_ = 0; usRemainder_ = 0; totalCycles = 0;
  toneT1lr_ = toneT1lc_ = 0; tonePeriod_ = toneLow_ = 0; toneOn_ = false; tonePhase_ = 0; audioAcc_ = 0;

  sfr_[SP] = 0x7F;
  sfr_[P3] = 0xFF;
  sfr_[P7] = 0x02;                                      // battery fine, not plugged into a controller
  sfr_[IE] = 0xFF;
  sfr_[P1FCR] = 0xBF; sfr_[P3INT] = 0xFD; sfr_[ISL] = 0xC0; sfr_[VSEL] = 0xFC;

  if (bios_) {
    // The real firmware starts at address 0 of the ROM and sets everything else up itself.
    sfr_[EXT] = 0x08;
    setDateTime(now);
    // Known BIOS versions can be entered just after their "set the clock" screen, with the registers as that code leaves them.
    const uint32_t crc = crc32(bios_, BIOS_SIZE);
    if (crc == 0xC825003Au || crc == 0x8E0F867Au) {
      static const uint8_t afterSetup[0x80] = {
        0x00, 0x00, 0xff, 0x00, 0x66, 0x19, 0x7f, 0x00, 0x83, 0x02, 0x00, 0x00, 0x00, 0x08, 0xa3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x05, 0x20, 0x01, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x80, 0xbf, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x06, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0xc0,
        0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79
      };
      for (int i = 0; i < 0x80; i++) writeMem((uint16_t)(0x100 + i), afterSetup[i]);
      pc_ = 0x2E1;
    }
  } else {
    // No BIOS: leave the machine the way the firmware does when it starts a game.
    sfr_[PSW] = PSW_RAMBK0;                             // games run with RAM bank 1
    sfr_[EXT] = 0x09;                                   // program comes from flash
    setDateTime(now);
    xram_[2][2] = 0x10;                                 // "game" icon
    sfr_[P1DDR] = 0xFF; sfr_[P3DDR] = 0x00;
    sfr_[BTCR] = 0x41;                                  // base timer running: clock tick twice a second
    sfr_[OCR] = 0xA3;                                   // quartz oscillator, 1/6 divider, RC and CF stopped
    sfr_[VCCR] = 0x80; sfr_[MCR] = 0x08;                // LCD on and refreshing
    // a formatted card without a game has nothing to run
    if (gameEnd() == 0) exited_ = true;
  }
  clockChanged();
}

DateTime Vmu::dateTime() const {
  const uint8_t* s = ram_[0];
  DateTime d;
  d.year = (uint16_t)((s[SYS_YEAR_HI] << 8) | s[SYS_YEAR_LO]);
  d.month = s[SYS_MONTH]; d.day = s[SYS_DAY]; d.hour = s[SYS_HOUR]; d.minute = s[SYS_MINUTE]; d.second = s[SYS_SEC];
  return d;
}

void Vmu::setDateTime(const DateTime& d) {
  uint8_t* s = ram_[0];
  s[SYS_YEAR_HI_BCD] = bcd(d.year / 100); s[SYS_YEAR_LO_BCD] = bcd(d.year % 100);
  s[SYS_MONTH_BCD] = bcd(d.month); s[SYS_DAY_BCD] = bcd(d.day);
  s[SYS_HOUR_BCD] = bcd(d.hour); s[SYS_MINUTE_BCD] = bcd(d.minute); s[SYS_SEC_BCD] = bcd(d.second);
  s[SYS_YEAR_HI] = (uint8_t)(d.year >> 8); s[SYS_YEAR_LO] = (uint8_t)d.year;
  s[SYS_MONTH] = d.month; s[SYS_DAY] = d.day; s[SYS_HOUR] = d.hour; s[SYS_MINUTE] = d.minute; s[SYS_SEC] = d.second;
  s[SYS_HALF_SEC] = 0;
  s[SYS_LEAP] = leapYear(d.year);
  s[SYS_DATE_SET] = 0xFF;
}

// one second of the firmware's clock
void Vmu::tickClock() {
  DateTime d = dateTime();
  if (d.month < 1 || d.month > 12) d.month = 1;
  if (++d.second >= 60) {
    d.second = 0;
    if (++d.minute >= 60) {
      d.minute = 0;
      if (++d.hour >= 24) {
        d.hour = 0;
        if (++d.day > daysInMonth(d.year, d.month)) {
          d.day = 1;
          if (++d.month > 12) { d.month = 1; d.year++; }
        }
      }
    }
  }
  setDateTime(d);
}

bool Vmu::sleeping() const { return (sfr_[PCON] & PCON_HOLD) != 0; }
bool Vmu::halted() const { return (sfr_[PCON] & PCON_HALT) != 0; }

void Vmu::clockChanged() {
  const uint8_t ocr = sfr_[OCR];
  const uint32_t osc = (ocr & 0x10) ? OSC_CF : (ocr & 0x20) ? OSC_QUARTZ : OSC_RC;
  const uint32_t div = (ocr & 0x80) ? 6 : 12;
  if (osc != osc_ || div != clkDiv_) {
    // time runUs() has already granted is worth a different number of cycles on the new clock
    if (cycleBudget_ > 0) cycleBudget_ = (int32_t)(((uint64_t)cycleBudget_ * osc * clkDiv_) / ((uint64_t)osc_ * div));
    usRemainder_ = 0; audioAcc_ = 0;
  }
  osc_ = osc; clkDiv_ = div;
  btInc_ = OSC_QUARTZ * div;                            // quartz ticks per CPU cycle, times the oscillator frequency
  audioInc_ = sampleRate_ * div;                        // samples per CPU cycle, times the oscillator frequency
  toneStepQ16_ = sampleRate_ ? (uint32_t)((((uint64_t)osc << 16) / div) / sampleRate_) : 0;   // timer ticks per sample
}

uint32_t Vmu::cyclesPerSecond() const {
  const uint8_t ocr = sfr_[OCR];
  const uint32_t osc = (ocr & 0x10) ? OSC_CF : (ocr & 0x20) ? OSC_QUARTZ : OSC_RC;
  return osc / ((ocr & 0x80) ? 6 : 12);
}

// ------------------------------------------------------------------------------------------ memory
inline uint8_t Vmu::rd(uint16_t a) { return a < 0x100 ? ram_[(sfr_[PSW] >> 1) & 1][a] : readMem(a); }
inline uint8_t Vmu::rdl(uint16_t a) { return a < 0x100 ? ram_[(sfr_[PSW] >> 1) & 1][a] : readLatch(a); }
inline void Vmu::wr(uint16_t a, uint8_t v) { if (a < 0x100) ram_[(sfr_[PSW] >> 1) & 1][a] = v; else writeMem(a, v); }
// what setAcc(v) does: the parity flag always follows the accumulator
inline void Vmu::setAcc(uint8_t v) {
  if (sfr_[ACC] != v) { sfr_[PSW] = (uint8_t)((sfr_[PSW] & 0xFE) | parity(v)); sfr_[ACC] = v; }
}

uint8_t Vmu::prog(uint16_t a) const {
  if (sfr_[EXT] & 1) return flash_[a];                  // flash bank 0: the game
  return bios_ ? bios_[a] : 0;
}

// Port 3 as the program sees it: a pressed button pulls its pin low when the pin is an input with its pull-up on.
uint8_t Vmu::port3() const {
  return (uint8_t)(sfr_[P3DDR] | (uint8_t)~sfr_[P3] | (uint8_t)~buttons_);
}

uint16_t Vmu::indirect(uint8_t mode) {
  const uint8_t reg = (uint8_t)(mode | ((sfr_[PSW] & (PSW_IRBK0 | PSW_IRBK1)) >> 1));
  return (uint16_t)(ram_[(sfr_[PSW] & PSW_RAMBK0) ? 1 : 0][reg] | ((mode & 2) << 7));
}

uint8_t Vmu::peek(uint16_t a) const {
  a &= 0x1FF;
  if (a < 0x100) return ram_[(sfr_[PSW] & PSW_RAMBK0) ? 1 : 0][a];
  if (a < 0x180) return sfr_[a - 0x100];
  return xram_[xbank_][a - 0x180];
}

uint8_t Vmu::readMem(uint16_t a) {
  a &= 0x1FF;
  if (a < 0x100) return ram_[(sfr_[PSW] & PSW_RAMBK0) ? 1 : 0][a];
  if (a >= 0x180) return xram_[xbank_][a - 0x180];
  const uint8_t r = (uint8_t)(a - 0x100);
  switch (r) {
    case P1DDR: case P1FCR: case P3DDR: case MCR: case VCCR: return 0xFF;      // write-only
    case VTRBF: {
      const uint8_t v = wram_[((sfr_[VRMAD2] & 1) << 8) | sfr_[VRMAD1]];
      if (sfr_[VSEL] & 0x10) { if (!++sfr_[VRMAD1]) sfr_[VRMAD2] ^= 1; }
      return v;
    }
    case T0L: return t0l_;
    case T0H: return t0h_;
    case T1LR: return t1l_;                                                     // reads give the counter, writes set the reload value
    case T1HR: return t1h_;
    case VRMAD2: return (uint8_t)(0xFE | (sfr_[VRMAD2] & 1));
    case P1: return 0;
    case P3: return port3();
    case P7: return (uint8_t)(0xF0 | sfr_[P7]);
    case OCR: return (uint8_t)(0x4C | sfr_[OCR]);
  }
  return sfr_[r];
}

// Read-modify-write instructions use the output latch, not the pin, so they do not disturb other bits.
uint8_t Vmu::readLatch(uint16_t a) {
  a &= 0x1FF;
  switch (a) {
    case 0x100 + T1LR: case 0x100 + T1HR: case 0x100 + P1: case 0x100 + P3: case 0x100 + P7: return sfr_[a - 0x100];
  }
  return readMem(a);
}

void Vmu::writeMem(uint16_t a, uint8_t v) {
  a &= 0x1FF;
  if (a < 0x100) { ram_[(sfr_[PSW] & PSW_RAMBK0) ? 1 : 0][a] = v; return; }
  if (a >= 0x180) {
    if (a > 0x1FB || !(sfr_[VCCR] & 0x40)) xram_[xbank_][a - 0x180] = v;       // VCCR bit 6 locks the display RAM (0x180-0x1FB)
    return;
  }
  const uint8_t r = (uint8_t)(a - 0x100);
  switch (r) {
    case ACC:
      if (sfr_[ACC] != v) sfr_[PSW] = (uint8_t)((sfr_[PSW] & 0xFE) | parity(v));
      break;
    case VTRBF:
      wram_[((sfr_[VRMAD2] & 1) << 8) | sfr_[VRMAD1]] = v;
      if (sfr_[VSEL] & 0x10) { if (!++sfr_[VRMAD1]) sfr_[VRMAD2] ^= 1; }
      return;
    case VRMAD2: v &= 1; break;
    case EXT:
      if ((sfr_[EXT] ^ v) & 1) {
        // The program source changes under the CPU's feet, so the next instruction is always a JMPF: take it now.
        if (prog(pc_) == 0x21) pc_ = (uint16_t)((prog((uint16_t)(pc_ + 1)) << 8) | prog((uint16_t)(pc_ + 2)));
      }
      break;
    case XBNK:
      if (v > 2) { if (v != sfr_[XBNK]) return; }      // there are only three banks; anything else is ignored
      else xbank_ = v;
      break;
    case T0PRR:
      t0Scale_ = 256 - v; t0Base_ = 0; btRemainder_ = 0;
      break;
    case T0CNT: {
      const uint8_t was = sfr_[T0CNT] & 0xC0, now = v & 0xC0;
      if (!was && now) t0Delay_ = 1; else if (!now) t0Delay_ = 0;
      if (!(v & 0x40)) t0l_ = sfr_[T0LR];
      if (!(v & 0x80)) t0h_ = sfr_[T0HR];
      break;
    }
    case T0LR: case T0L: if (!(sfr_[T0CNT] & 0x40)) t0l_ = v; break;
    case T0HR: case T0H: if (!(sfr_[T0CNT] & 0x80)) t0h_ = v; break;
    case T1CNT: {
      const uint8_t was = sfr_[T1CNT] & 0xC0, now = v & 0xC0;
      if (!was && now) t1Delay_ = 1; else if (!now) t1Delay_ = 0;
      if (!(v & 0x40)) t1l_ = sfr_[T1LR];
      if (!(v & 0x80)) t1h_ = sfr_[T1HR];
      break;
    }
    case T1LR: if (!(sfr_[T1CNT] & 0x40)) t1l_ = v; break;
    case T1HR: if (!(sfr_[T1CNT] & 0x80)) t1h_ = v; break;
    case BTCR: {
      const bool was = (sfr_[BTCR] & 0x40) != 0, now = (v & 0x40) != 0;
      if (!was && now) btDelay_ = curCycles_; else if (!now) btDelay_ = 0;
      if (!now) btCounter_ = 0;
      break;
    }
    case ISL: btRemainder_ = 0; break;
  }
  sfr_[r] = v;
  if (r == OCR) { btRemainder_ = 0; clockChanged(); }
  buzzerSink(a);
}

// ------------------------------------------------------------------------------------------ interrupts
uint16_t Vmu::irqsEnabled(int priority) const {
  const uint8_t ie = sfr_[IE], ip = sfr_[IP];
  uint16_t mask = 0;
  if (priority == PRI_HIGHEST) {
    if (!(ie & 1)) mask |= 1u << IRQ_INT0;
    if (!(ie & 2) && !(ie & 1)) mask |= 1u << IRQ_INT1;
    return mask;
  }
  if (priority == PRI_HIGH || (ie & 0x80)) {
    const bool high = priority == PRI_HIGH;
    // interrupt priority register: one bit per source, set = high priority
    static const struct { uint8_t bit; uint8_t irq; } MAP[7] = {
      {0x80, IRQ_P3}, {0x20, IRQ_SIO1}, {0x10, IRQ_SIO0}, {0x08, IRQ_T1}, {0x04, IRQ_T0H}, {0x02, IRQ_INT3_BT}, {0x01, IRQ_INT2_T0L}
    };
    for (int i = 0; i < 7; i++) if (((ip & MAP[i].bit) != 0) == high) mask |= (uint16_t)(1u << MAP[i].irq);
    if (!high) {
      if (ie & 1) mask |= (1u << IRQ_INT0) | (1u << IRQ_INT1);
      if (ie & 2) mask |= 1u << IRQ_INT1;
    }
  }
  return mask;
}

bool Vmu::picCheck(int priority) {
  const uint16_t pending = irqsEnabled(priority) & intReq_;
  if (!pending) return false;
  for (int i = 0; i < 16; i++) {
    const uint16_t bit = (uint16_t)(1u << i);
    if (!(pending & bit)) continue;
    intReq_ &= (uint16_t)~bit;
    intStack_[priority] = bit;
    push((uint8_t)pc_); push((uint8_t)(pc_ >> 8));
    sfr_[PCON] &= (uint8_t)~PCON_HALT;                 // any interrupt ends HALT
    pc_ = IRQ_VECTOR[i];
    return true;
  }
  return false;
}

void Vmu::picUpdate() {
  if (irqSkip_) { irqSkip_ = false; return; }          // one instruction of the interrupted code always runs after RETI
  if (!intReq_) return;                                 // nothing waiting (the usual case)
  if (!(intStack_[0] | intStack_[1] | intStack_[2])) {
    // nothing being serviced: after returning from a level, lower levels get the first chance
    for (int p = (int)prevIntPriority_ - 1; p >= PRI_LOW; --p) if (picCheck(p)) return;
    for (int p = PRI_HIGH; p >= (int)prevIntPriority_; --p) if (picCheck(p)) return;
  } else {
    for (int p = PRI_HIGHEST; p >= PRI_LOW; --p) {
      if (intStack_[p]) break;                          // only levels above the one being serviced may interrupt it
      if (picCheck(p)) return;
    }
  }
}

// ------------------------------------------------------------------------------------------ timers
static inline uint32_t takeDelay(uint32_t& delay, uint32_t ticks) {
  const uint32_t skipped = delay < ticks ? delay : ticks;
  delay -= skipped;
  return ticks - skipped;
}

// Advance an up-counter that reloads on overflow; returns how many times it overflowed.
static uint32_t advanceCounter(uint32_t& counter, uint32_t ticks, uint32_t modulus, uint32_t reload) {
  const uint32_t start = counter % modulus;
  if (!ticks) { counter = start; return 0; }
  const uint32_t toOverflow = modulus - start;
  if (ticks < toOverflow) { counter = start + ticks; return 0; }
  ticks -= toOverflow;
  const uint32_t period = modulus - reload;
  counter = reload + ticks % period;
  return 1 + ticks / period;
}
static uint32_t advance8(uint8_t& c, uint32_t ticks, uint8_t reload) {
  uint32_t v = c;
  const uint32_t n = advanceCounter(v, ticks, 0x100, reload);
  c = (uint8_t)v;
  return n;
}

void Vmu::timersUpdate(uint32_t cycles) {
  // ---- base timer: 14-bit counter normally fed by the 32768 Hz crystal, whatever the CPU runs from
  {
    const uint8_t btcr = sfr_[BTCR];
    uint32_t elapsed;
    switch ((sfr_[ISL] >> 4) & 3) {
      case 1: elapsed = takeDelay(btDelay_, cycles); break;                                    // cycle clock
      case 3: {                                                                                // timer 0 prescaler
        btDelay_ = 0;
        const uint32_t n = btRemainder_ + cycles;
        elapsed = n / (uint32_t)t0Scale_; btRemainder_ = n % (uint32_t)t0Scale_;
        break;
      }
      default: {
        btDelay_ = 0;
        // quartz ticks in this many CPU cycles = cycles * 32768 * divider / oscillator. All of it fits in 32 bits
        // (the remainder is below the oscillator frequency), which matters on a microcontroller.
        uint32_t n = btRemainder_ + cycles * btInc_;
        elapsed = 0;
        if (n >= osc_) { elapsed = n / osc_; n -= elapsed * osc_; }
        btRemainder_ = n;
      }
    }
    if ((btcr & 0x40) && elapsed) {
      const uint32_t cur = btCounter_, next = cur + elapsed;
      const uint32_t rate0 = (btcr & 0x80) ? 0x40 : 0x4000;
      uint32_t rate1 = 0;
      switch (btcr & 0xB0) {
        case 0x00: case 0x80: rate1 = 0x20; break;
        case 0x10: case 0x90: rate1 = 0x80; break;
        case 0x20: rate1 = 0x200; break;
        case 0x30: rate1 = 0x800; break;
        case 0xA0: rate1 = 0x2; break;
        case 0xB0: rate1 = 0x8; break;
      }
      // the rates are powers of two, so "passed a multiple of the rate" is a change in the bits above it
      if (rate1 && ((cur ^ next) & ~(rate1 - 1))) { sfr_[BTCR] |= 0x08; if (btcr & 0x04) raiseIrq(IRQ_INT3_BT); }
      if ((cur ^ next) & ~(rate0 - 1)) { sfr_[BTCR] |= 0x02; if (btcr & 0x01) raiseIrq(IRQ_INT3_BT); }
      btCounter_ = (uint16_t)(next & 0x3FFF);
    }
  }

  // ---- timer 0: 8-bit prescaler in front of two 8-bit counters (or one 16-bit)
  if (sfr_[T0CNT] & 0xC0) {
    const uint32_t gated = takeDelay(t0Delay_, cycles);
    uint32_t ticks = 0;
    t0Base_ += (int32_t)gated;
    if (t0Base_ >= t0Scale_) { ticks = (uint32_t)(t0Base_ / t0Scale_); t0Base_ %= t0Scale_; }
    if (ticks) {
      if ((sfr_[T0CNT] & 0xE0) == 0xE0) {
        uint32_t v = ((uint32_t)t0h_ << 8) | t0l_;
        const uint32_t n = advanceCounter(v, ticks, 0x10000, ((uint32_t)sfr_[T0HR] << 8) | sfr_[T0LR]);
        t0l_ = (uint8_t)v; t0h_ = (uint8_t)(v >> 8);
        if (n) { sfr_[T0CNT] |= 0x0A; if (sfr_[T0CNT] & 0x04) raiseIrq(IRQ_T0H); }
      } else {
        if (sfr_[T0CNT] & 0x40) {
          if (advance8(t0l_, ticks, sfr_[T0LR])) { sfr_[T0CNT] |= 0x02; if (sfr_[T0CNT] & 0x01) raiseIrq(IRQ_INT2_T0L); }
        }
        if (sfr_[T0CNT] & 0x80) {
          if (advance8(t0h_, ticks, sfr_[T0HR])) { sfr_[T0CNT] |= 0x08; if (sfr_[T0CNT] & 0x04) raiseIrq(IRQ_T0H); }
        }
      }
    }
  }

  // ---- timer 1: counts CPU cycles; its low half also drives the buzzer
  {
    const uint32_t ticks = takeDelay(t1Delay_, cycles);
    if (ticks && (sfr_[T1CNT] & 0xC0)) {
      if ((sfr_[T1CNT] & 0xE0) == 0xE0) {
        const uint32_t lowOverflows = advance8(t1l_, ticks, sfr_[T1LR]);
        if (advance8(t1h_, lowOverflows, sfr_[T1HR])) { sfr_[T1CNT] |= 0x08; if (sfr_[T1CNT] & 0x04) raiseIrq(IRQ_T1); }
        if (lowOverflows) sfr_[T1CNT] |= 0x02;
      } else {
        if (sfr_[T1CNT] & 0x40) {
          if (advance8(t1l_, ticks, sfr_[T1LR])) {
            sfr_[T1CNT] |= 0x02;
            if (!(sfr_[T1CNT] & 0x20)) { buzzerLatch((sfr_[T1CNT] & 0x10) != 0); if (buzzerConfigured()) buzzerUpdate(); }
            if (sfr_[T1CNT] & 0x01) raiseIrq(IRQ_T1);
          }
        }
        if (sfr_[T1CNT] & 0x80) {
          if (advance8(t1h_, ticks, sfr_[T1HR])) { sfr_[T1CNT] |= 0x08; if (sfr_[T1CNT] & 0x04) raiseIrq(IRQ_T1); }
        }
      }
    }
  }
}

// ------------------------------------------------------------------------------------------ buzzer
// Timer 1's low counter is a pulse generator on pin P1.7: low from reload until the counter
// reaches the compare value, then high until it overflows.
bool Vmu::buzzerConfigured() const {
  return (sfr_[P1DDR] & 0x80) && (sfr_[P1FCR] & 0x80) && !(sfr_[P1] & 0x80) && !(sfr_[T1CNT] & 0x20);
}

void Vmu::buzzerLatch(bool compare) {
  toneT1lr_ = sfr_[T1LR];
  if (compare) toneT1lc_ = sfr_[T1LC];
}

void Vmu::buzzerUpdate() {
  uint16_t period = (uint16_t)(256 - toneT1lr_);
  const uint16_t low = (uint8_t)(toneT1lc_ - toneT1lr_);
  if (period < low) period = low;
  tonePeriod_ = period; toneLow_ = low;
}

void Vmu::buzzerSink(uint16_t a) {
  switch (a) {
    case 0x100 + T1LR: case 0x100 + T1LC: case 0x100 + T1CNT: case 0x100 + P1DDR: case 0x100 + P1FCR: case 0x100 + P1: break;
    default: return;
  }
  const bool running = (sfr_[T1CNT] & 0x40) != 0;
  if (!running) buzzerLatch(true);
  if (!buzzerConfigured() || !running) { toneOn_ = false; return; }
  // reload/compare written while the timer runs only take effect at the next overflow
  if (a != 0x100 + T1LR && a != 0x100 + T1LC) buzzerUpdate();
  if (!toneOn_) { toneOn_ = true; if (!tonePeriod_) buzzerUpdate(); }
}

void Vmu::audioInit(uint32_t sampleRate, int16_t* ring, uint32_t ringSize) {
  sampleRate_ = ring && ringSize > 1 ? sampleRate : 0; ring_ = ring; ringSize_ = sampleRate_ ? ringSize : 0;
  ringW_ = 0; ringR_ = 0; audioAcc_ = 0; tonePhase_ = 0;
  clockChanged();
}

// One output sample: the pulse wave timer 1 is making, or silence.
void Vmu::audioSample() {
  int16_t s = 0;
  if (toneOn_ && tonePeriod_ && toneLow_ && toneLow_ < tonePeriod_) {      // a pulse that never goes low, or never high, is silence
    tonePhase_ += toneStepQ16_;
    const uint32_t period = (uint32_t)tonePeriod_ << 16;
    if (tonePhase_ >= period) tonePhase_ %= period;
    s = (int16_t)((tonePhase_ >> 16) < toneLow_ ? -audioLevel : audioLevel);
  }
  pushSample(s);
}

uint32_t Vmu::audioRead(int16_t* dst, uint32_t max) {
  uint32_t n = 0, r = ringR_;
  const uint32_t w = ringW_;
  while (n < max && r != w) { dst[n++] = ring_[r]; if (++r == ringSize_) r = 0; }
  ringR_ = r;
  return n;
}

// ------------------------------------------------------------------------------------------ screen
bool Vmu::lcdOn() const {
  return (sfr_[VCCR] & 0x80) && (sfr_[MCR] & 0x08) && !(sfr_[PCON] & PCON_HOLD);
}

void Vmu::frame(uint8_t out[LCD_H * LCD_ROW_BYTES]) const {
  // display RAM: bank 0 holds rows 0-15, bank 1 rows 16-31; each 16-byte line holds two rows of 6 bytes
  int p = sfr_[STAD];
  if (p >= 0x83) p -= 0x83;
  int bank = p >> 6;
  p = (p & 0x3F) * 2;
  for (int i = 0; i < LCD_H * LCD_ROW_BYTES; i++) {
    out[i] = bank < 3 ? xram_[bank][p & 0x7F] : 0;
    p++;
    if (i % LCD_ROW_BYTES == LCD_ROW_BYTES - 1) {
      if ((p & 0xF) >= 12) p += 4;
      if (p >= 128) { bank++; p -= 128; }
      if (bank == 2 && p >= 6) { bank = 0; p -= 6; }
    }
  }
}

uint8_t Vmu::icons() const {
  if (!lcdOn()) return 0;
  return (uint8_t)(((xram_[2][1] & 0x40) ? ICON_FILE : 0) | ((xram_[2][2] & 0x10) ? ICON_GAME : 0) |
                   ((xram_[2][3] & 0x04) ? ICON_CLOCK : 0) | ((xram_[2][4] & 0x01) ? ICON_FLASH : 0));
}

// ------------------------------------------------------------------------------------------ buttons
void Vmu::setButtons(uint8_t pressed) {
  const uint8_t newlyDown = (uint8_t)(pressed & ~buttons_);
  buttons_ = pressed;
  // Port 3 interrupt: a button going down while a pin is pulled low. It also ends HOLD (sleep).
  if (newlyDown && (uint8_t)~port3() && (sfr_[P3INT] & 0x04)) {
    sfr_[P3INT] |= 0x02;
    sfr_[PCON] &= (uint8_t)~PCON_HOLD;
    if (sfr_[P3INT] & 0x01) raiseIrq(IRQ_P3);
  }
}

// ------------------------------------------------------------------------------------------ firmware emulation
// Where the game on this card ends, from the card's directory (0 = no game).
uint32_t Vmu::gameEnd() const {
  const uint8_t* root = flash_ + 255 * 512;
  for (int i = 0; i < 16; i++) if (root[i] != 0x55) return 0;
  uint32_t dirBlock = root[0x4A] | (root[0x4B] << 8), fatBlock = root[0x46] | (root[0x47] << 8);
  if (fatBlock > 255) return 0;
  const uint8_t* fat = flash_ + fatBlock * 512;
  for (int guard = 0; guard < 32 && dirBlock < 256; guard++) {
    const uint8_t* dir = flash_ + dirBlock * 512;
    for (int e = 0; e < 16; e++) {
      const uint8_t* d = dir + e * 32;
      if (d[0] == 0xCC) {
        const uint32_t first = d[2] | (d[3] << 8), blocks = d[0x18] | (d[0x19] << 8);
        if (first != 0 || blocks == 0 || blocks > 200) return 0;      // a game always starts at block 0
        return blocks * 512;
      }
    }
    dirBlock = fat[dirBlock * 2] | (fat[dirBlock * 2 + 1] << 8);
  }
  return 0;
}

// The CPU has just jumped into the (absent) BIOS: do what the routine at that address does and go back.
void Vmu::firmwareCall() {
  uint8_t* user = ram_[1];
  const uint32_t a = (((uint32_t)user[0x7D] << 16) | ((uint32_t)user[0x7E] << 8) | user[0x7F]) & 0x1FFFF;
  uint16_t back = 0;
  switch (pc_) {
    case FM_WRT_EX: case FM_WRTA_EX: {                  // write the 128 bytes at RAM 0x80 to a flash page of the game's own file
      const uint32_t end = gameEnd();
      if (!end || a + 128 > end) setAcc(0xFF);
      else {
        setAcc(0x00);
        for (uint32_t i = 0; i < 128; i++) flash_[(a & ~0xFFu) | ((a + i) & 0xFF)] = user[0x80 + i];
        flashDirty = true;
        if (onFlashWrite) onFlashWrite(cbCtx, a);
      }
      back = pc_ == FM_WRT_EX ? 0x105 : 0x10B;
      break;
    }
    case FM_VRF_EX: {                                   // compare a flash page with RAM
      uint8_t r = 0;
      for (uint32_t i = 0; i < 128; i++) if ((r = flash_[(a & ~0xFFu) | ((a + i) & 0xFF)] ^ user[0x80 + i]) != 0) break;
      setAcc(r);
      back = 0x115;
      break;
    }
    case FM_PRD_EX:                                     // read a flash page into RAM
      for (uint32_t i = 0; i < 128; i++) user[0x80 + i] = flash_[(a & ~0xFFu) | ((a + i) & 0xFF)];
      back = 0x125;
      break;
    case TIMER_EX:                                      // called from the game's base-timer interrupt, twice a second
      if (!((ram_[0][SYS_HALF_SEC] ^= 1) & 1)) tickClock();
      back = 0x139;
      break;
    default:                                            // EXIT_EX (MODE pressed), SLEEP_EX, or anything else: leave the game
      exited_ = true;
      return;
  }
  pc_ = back;
  sfr_[EXT] |= 1;
}

// Flash programming as the real BIOS does it: unlock with three magic writes, then store a 128-byte page.
void Vmu::flashStore() {
  uint32_t addr = ((uint32_t)sfr_[TRH] << 8) | sfr_[TRL];
  const uint8_t acc = sfr_[ACC], fpr = sfr_[FPR];
  if (sfr_[EXT] & 1) return;                            // only system code may write flash
  if (fpr & 0x02) {
    switch (flashUnlock_) {
      case 0: if (addr == 0x5555 && acc == 0xAA) flashUnlock_ = 1; break;
      case 1: flashUnlock_ = (addr == 0x2AAA && acc == 0x55) ? 2 : 0; break;
      case 2: flashUnlock_ = (addr == 0x5555 && acc == 0xA0) ? 3 : 0; break;
      default: flashUnlock_ = 0;
    }
  } else if (flashUnlock_ < 3) {
    flashUnlock_ = 0;
  } else {
    addr |= (uint32_t)(fpr & 1) << 16;
    if (flashUnlock_ == 3 && (addr & 0x7F)) { flashUnlock_ = 0; return; }
    flash_[addr] = acc;
    flashDirty = true;
    if (onFlashWrite) onFlashWrite(cbCtx, addr);
    if (++flashUnlock_ == 3 + 128) flashUnlock_ = 0;
  }
}

// ------------------------------------------------------------------------------------------ CPU
void Vmu::arith(uint8_t b, bool carryIn, bool subtract) {
  const int a = sfr_[ACC];
  const int c = carryIn ? (sfr_[PSW] >> 7) : 0;
  const int bc = b + c;
  uint8_t psw = sfr_[PSW] & (uint8_t)~(PSW_CY | PSW_AC | PSW_OV);
  uint8_t r;
  if (!subtract) {
    r = (uint8_t)(a + b + c);
    if (a + b + c > 255) psw |= PSW_CY;
    if ((a & 0xF) + (bc & 0xF) > 0xF) psw |= PSW_AC;
    if (0x80 & (~a ^ bc) & (bc ^ (a + bc))) psw |= PSW_OV;
  } else {
    r = (uint8_t)(a - b - c);
    if (a - b - c < 0) psw |= PSW_CY;
    if ((a & 0xF) - (b & 0xF) - c < 0) psw |= PSW_AC;
    if ((int8_t)(a ^ bc) < 0 && (int8_t)(bc ^ (a - bc)) >= 0) psw |= PSW_OV;
  }
  sfr_[PSW] = psw;
  setAcc(r);                                   // parity follows the new accumulator
}

int Vmu::execute() {
  const uint8_t* code = (sfr_[EXT] & 1) ? flash_ : bios_;
  if (!code) { pc_ += 1; curCycles_ = 1; return 1; }    // no program mapped in reads as NOPs (step() never lets this run)
  const uint8_t op = code[pc_];
  const uint8_t lo = op & 0x0F, hi = op >> 4;
  curCycles_ = g_cycles[op];

  // operand bytes
  const uint8_t b1 = code[(uint16_t)(pc_ + 1)], b2 = code[(uint16_t)(pc_ + 2)];
#define REL8(x) ((int8_t)(x))
#define BRANCH(cond, rel) do { if (cond) pc_ = (uint16_t)(pc_ + REL8(rel)); } while (0)

  if (lo >= 8) {
    if (hi <= 3) {                                      // CALL / JMP a12
      const uint16_t a12 = (uint16_t)(((op & 0x10) << 7) | ((op & 0x07) << 8) | b1);
      pc_ += 2;
      if (hi <= 1) { push((uint8_t)pc_); push((uint8_t)(pc_ >> 8)); }
      pc_ = (uint16_t)((pc_ & 0xF000) | a12);
      return curCycles_;
    }
    // bit instructions: d9 with its top bit in opcode bit 4, bit number in the low 3 bits
    const uint16_t d9 = (uint16_t)(((op & 0x10) << 4) | b1);
    const uint8_t mask = (uint8_t)(1u << (op & 7));
    switch (hi & 0xE) {
      case 0x4: {                                       // BPC d9,b3,r8: branch if set, and clear
        pc_ += 3;
        const uint8_t v = rdl(d9);
        if (v & mask) { wr(d9, v & (uint8_t)~mask); pc_ = (uint16_t)(pc_ + REL8(b2)); }
        break;
      }
      case 0x6: pc_ += 3; BRANCH(rd(d9) & mask, b2); break;          // BP
      case 0x8: pc_ += 3; BRANCH(!(rd(d9) & mask), b2); break;       // BN
      case 0xA: pc_ += 2; wr(d9, rdl(d9) ^ mask); break;      // NOT1
      case 0xC: pc_ += 2; wr(d9, rdl(d9) & (uint8_t)~mask); break;   // CLR1
      case 0xE: pc_ += 2; wr(d9, rdl(d9) | mask); break;      // SET1
    }
    return curCycles_;
  }

  if (lo >= 2) {
    // one data operand: d9 (opcode bit 0 is its top bit, one more byte) or @Ri (no extra byte)
    const bool ind = lo >= 4;
    const uint16_t addr = ind ? indirect(op & 3) : (uint16_t)(((op & 1) << 8) | b1);
    pc_ = (uint16_t)(pc_ + (ind ? 1 : 2));
    const uint8_t n1 = code[pc_], n2 = code[(uint16_t)(pc_ + 1)];      // the bytes after the operand, if the instruction has any
    switch (hi) {
      case 0x0: setAcc(rd(addr)); break;                    // LD
      case 0x1: wr(addr, sfr_[ACC]); break;                         // ST
      case 0x2: pc_ += 1; wr(addr, n1); break;                      // MOV #i8
      case 0x3: case 0x4: {                                               // BE / BNE
        uint8_t v1, v2, rel;
        if (ind) { v1 = rd(addr); v2 = n1; rel = n2; pc_ += 2; }     // @Ri,#i8,r8
        else { v1 = sfr_[ACC]; v2 = rd(addr); rel = n1; pc_ += 1; }   // d9,r8
        sfr_[PSW] = (uint8_t)((sfr_[PSW] & ~PSW_CY) | (v1 < v2 ? PSW_CY : 0));
        BRANCH(hi == 3 ? v1 == v2 : v1 != v2, rel);
        break;
      }
      case 0x5: {                                                         // DBNZ
        pc_ += 1;
        const uint8_t v = (uint8_t)(rdl(addr) - 1);
        wr(addr, v);
        BRANCH(v != 0, n1);
        break;
      }
      case 0x6: wr(addr, (uint8_t)(rdl(addr) + 1)); break;    // INC
      case 0x7: wr(addr, (uint8_t)(rdl(addr) - 1)); break;    // DEC
      case 0x8: arith(rd(addr), false, false); break;                // ADD
      case 0x9: arith(rd(addr), true, false); break;                 // ADDC
      case 0xA: arith(rd(addr), false, true); break;                 // SUB
      case 0xB: arith(rd(addr), true, true); break;                  // SUBC
      case 0xC: { const uint8_t a = sfr_[ACC], m = rd(addr); setAcc(m); wr(addr, a); break; }   // XCH
      case 0xD: setAcc(sfr_[ACC] | rd(addr)); break;   // OR
      case 0xE: setAcc(sfr_[ACC] & rd(addr)); break;   // AND
      case 0xF: setAcc(sfr_[ACC] ^ rd(addr)); break;   // XOR
    }
    return curCycles_;
  }

  if (lo == 1) {
    switch (hi) {
      case 0x0: pc_ += 2; pc_ = (uint16_t)(pc_ + REL8(b1)); break;                      // BR
      case 0x1: pc_ += 3; pc_ = (uint16_t)(pc_ + (uint16_t)(b1 | (b2 << 8)) - 1); break;   // BRF
      case 0x2: pc_ = (uint16_t)((b1 << 8) | b2); break;                                // JMPF
      case 0x3: case 0x4: {                                                             // BE / BNE #i8,r8
        pc_ += 3;
        const uint8_t a = sfr_[ACC];
        sfr_[PSW] = (uint8_t)((sfr_[PSW] & ~PSW_CY) | (a < b1 ? PSW_CY : 0));
        BRANCH(hi == 3 ? a == b1 : a != b1, b2);
        break;
      }
      case 0x5: pc_ += 1; flashStore(); break;                                          // STF
      case 0x6: pc_ += 2; push(readMem((uint16_t)(0x100 | b1))); break;                 // PUSH d9 (0x1xx)
      case 0x7: pc_ += 2; writeMem((uint16_t)(0x100 | b1), pop()); break;               // POP d9
      case 0x8: pc_ += 2; arith(b1, false, false); break;
      case 0x9: pc_ += 2; arith(b1, true, false); break;
      case 0xA: pc_ += 2; arith(b1, false, true); break;
      case 0xB: pc_ += 2; arith(b1, true, true); break;
      case 0xC: {                                                                       // LDC: table lookup in program memory
        pc_ += 1;
        const uint32_t a = (uint32_t)sfr_[ACC] + (uint32_t)(sfr_[TRL] | (sfr_[TRH] << 8));
        setAcc((sfr_[EXT] & 1) ? flash_[a & 0x1FFFF] : prog((uint16_t)a));   // in flash the table may run past 64 KiB
        break;
      }
      case 0xD: pc_ += 2; setAcc(sfr_[ACC] | b1); break;
      case 0xE: pc_ += 2; setAcc(sfr_[ACC] & b1); break;
      case 0xF: pc_ += 2; setAcc(sfr_[ACC] ^ b1); break;
    }
    return curCycles_;
  }

  // lo == 0
  switch (hi) {
    case 0x0: pc_ += 1; break;                                                          // NOP
    case 0x1: pc_ += 3; push((uint8_t)pc_); push((uint8_t)(pc_ >> 8)); pc_ = (uint16_t)(pc_ + (uint16_t)(b1 | (b2 << 8)) - 1); break;   // CALLR
    case 0x2: pc_ += 3; push((uint8_t)pc_); push((uint8_t)(pc_ >> 8)); pc_ = (uint16_t)((b1 << 8) | b2); break;                        // CALLF
    case 0x3: {                                                                         // MUL: (ACC:C) * B -> B:ACC:C
      pc_ += 1;
      const uint32_t t = (uint32_t)(sfr_[C] | (sfr_[ACC] << 8)) * sfr_[B];
      writeMem(0x100 + C, (uint8_t)t); setAcc((uint8_t)(t >> 8)); writeMem(0x100 + B, (uint8_t)(t >> 16));
      sfr_[PSW] = (uint8_t)((sfr_[PSW] & ~(PSW_CY | PSW_OV)) | (t > 65535 ? PSW_OV : 0));
      break;
    }
    case 0x4: {                                                                         // DIV: (ACC:C) / B -> ACC:C, remainder B
      pc_ += 1;
      const uint32_t d = sfr_[B];
      if (d) {
        const uint32_t v = (uint32_t)(sfr_[C] | (sfr_[ACC] << 8));
        writeMem(0x100 + B, (uint8_t)(v % d)); writeMem(0x100 + C, (uint8_t)(v / d)); setAcc((uint8_t)((v / d) >> 8));
      } else setAcc(0xFF);
      sfr_[PSW] = (uint8_t)((sfr_[PSW] & ~(PSW_CY | PSW_OV)) | (d ? 0 : PSW_OV));
      break;
    }
    case 0x5: pc_ += 1; setAcc(flash_[((uint32_t)(sfr_[FPR] & 1) << 16) | ((uint32_t)sfr_[TRH] << 8) | sfr_[TRL]]); break;   // LDF
    case 0x6: pc_ += 2; push(readMem(b1)); break;                                       // PUSH d9 (0x0xx)
    case 0x7: pc_ += 2; writeMem(b1, pop()); break;                                     // POP d9
    case 0x8: pc_ += 2; BRANCH(sfr_[ACC] == 0, b1); break;                         // BZ
    case 0x9: pc_ += 2; BRANCH(sfr_[ACC] != 0, b1); break;                         // BNZ
    case 0xA: { const uint16_t h = (uint16_t)(pop() << 8); pc_ = (uint16_t)(h | pop()); break; }   // RET
    case 0xB: {                                                                         // RETI
      const uint16_t h = (uint16_t)(pop() << 8); pc_ = (uint16_t)(h | pop());
      irqSkip_ = true;
      for (int p = PRI_HIGHEST; p >= PRI_LOW; --p) if (intStack_[p]) { prevIntPriority_ = (uint8_t)p; intStack_[p] = 0; break; }
      break;
    }
    case 0xC: { pc_ += 1; const uint8_t v = sfr_[ACC]; setAcc((uint8_t)((v << 7) | (v >> 1))); break; }   // ROR
    case 0xD: {                                                                         // RORC
      pc_ += 1;
      const uint8_t v = sfr_[ACC], psw = sfr_[PSW];
      sfr_[PSW] = (uint8_t)((psw & ~PSW_CY) | ((v & 1) << 7));
      setAcc((uint8_t)((v >> 1) | (psw & PSW_CY)));
      break;
    }
    case 0xE: { pc_ += 1; const uint8_t v = sfr_[ACC]; setAcc((uint8_t)((v << 1) | (v >> 7))); break; }   // ROL
    case 0xF: {                                                                         // ROLC
      pc_ += 1;
      const uint8_t v = sfr_[ACC], psw = sfr_[PSW];
      sfr_[PSW] = (uint8_t)((psw & ~PSW_CY) | (v & 0x80));
      setAcc((uint8_t)((v << 1) | (psw >> 7)));
      break;
    }
  }
#undef REL8
#undef BRANCH
  return curCycles_;
}

int Vmu::step() {
  if (exited_) return 1;
  const uint8_t pcon = sfr_[PCON];
  const bool hold = (pcon & PCON_HOLD) != 0, halt = (pcon & PCON_HALT) != 0;
  if (!hold) {
    if (irqSkip_) irqSkip_ = false;                     // one instruction of the interrupted code always runs after RETI
    else if (intReq_) picUpdate();
    timersUpdate(prevCycles_);                          // the timers see the time the previous step took
  }
  int cycles = 1;                                       // HALT and HOLD idle one cycle at a time
  if (!halt && !hold) {
    cycles = execute();
    if (!(sfr_[EXT] & 1) && !bios_ && !exited_) firmwareCall();
  }
  prevCycles_ = (uint8_t)cycles;
  totalCycles += (uint32_t)cycles;
  if (audioInc_) {
    audioAcc_ += (uint32_t)cycles * audioInc_;
    while (audioAcc_ >= osc_) { audioAcc_ -= osc_; audioSample(); }
  }
  return cycles;
}

void Vmu::runUs(uint32_t us) {
  if (us > 1000000) us = 1000000;
  // cycles = microseconds * oscillator / (divider * 1,000,000); the remainder is carried, so no time is lost between calls
  const uint64_t n = usRemainder_ + (uint64_t)us * osc_;
  const uint32_t per = clkDiv_ * 1000000u;
  cycleBudget_ += (int32_t)(n / per);
  usRemainder_ = n % per;
  const int32_t most = (int32_t)(osc_ / clkDiv_ / 2);   // never try to catch up more than half a second
  if (cycleBudget_ > most) cycleBudget_ = most;
  while (cycleBudget_ > 0) {
    if (exited_ || (sfr_[PCON] & PCON_HOLD)) {          // nothing happens until a button is pressed: let the time pass in silence
      if (audioInc_) {
        uint32_t samples = (uint32_t)(((uint64_t)cycleBudget_ * audioInc_ + audioAcc_) / osc_);
        audioAcc_ = 0;
        while (samples--) pushSample(0);
      }
      cycleBudget_ = 0;
      return;
    }
    cycleBudget_ -= step();
  }
}

// ------------------------------------------------------------------------------------------ save states
size_t Vmu::sync(State& s) {
  uint32_t magic = 0x31554D56u /* "VMU1" */, version = STATE_VERSION;
  s.v(magic); s.v(version);
  if (s.loading() && (magic != 0x31554D56u || version != STATE_VERSION)) return 0;
  s.bytes(sfr_, 0x80); s.bytes(ram_[0], 0x100); s.bytes(ram_[1], 0x100);
  s.bytes(xram_[0], 0x80); s.bytes(xram_[1], 0x80); s.bytes(xram_[2], 0x80); s.bytes(wram_, 0x200);
  s.v(xbank_); s.v(pc_);
  s.v(t0l_); s.v(t0h_); s.v(t1l_); s.v(t1h_); s.v(t0Base_); s.v(t0Scale_); s.v(t0Delay_); s.v(t1Delay_); s.v(btDelay_); s.v(btCounter_); s.v(btRemainder_);
  s.v(intReq_); s.v(intStack_[0]); s.v(intStack_[1]); s.v(intStack_[2]); s.b(irqSkip_); s.v(prevIntPriority_);
  s.v(prevCycles_); s.v(curCycles_); s.v(buttons_); s.v(flashUnlock_); s.b(exited_); s.v(totalCycles);
  s.v(toneT1lr_); s.v(toneT1lc_); s.v(tonePeriod_); s.v(toneLow_); s.b(toneOn_);
  if (s.loading()) { if (xbank_ > 2) xbank_ = 0; if (t0Scale_ < 1 || t0Scale_ > 256) t0Scale_ = 256; if (btRemainder_ >= OSC_CF) btRemainder_ = 0; if (prevCycles_ < 1 || prevCycles_ > 7) prevCycles_ = 1; cycleBudget_ = 0; usRemainder_ = 0; audioAcc_ = 0; tonePhase_ = 0; clockChanged(); }
  return s.ok() ? s.size() : 0;
}

size_t Vmu::saveState(uint8_t* out, size_t cap) {
  State s(out ? State::Save : State::Measure, out, cap);
  return sync(s);
}

bool Vmu::loadState(const uint8_t* in, size_t len) {
  State s(State::Load, const_cast<uint8_t*>(in), len);
  return sync(s) != 0;
}

size_t Vmu::debugRegs(uint32_t* out, size_t cap) const {
  size_t n = 0;
#define PUT(x) do { if (n < cap) out[n] = (uint32_t)(x); n++; } while (0)
  static const uint8_t REGS[] = { ACC, PSW, B, C, TRL, TRH, SP, PCON, IE, IP, OCR, T0CNT, T0PRR, T0LR, T0HR, T1CNT, T1LC, T1LR, T1HC, T1HR,
                                  MCR, STAD, XBNK, VCCR, P1, P1DDR, P1FCR, P3, P3DDR, P3INT, I01CR, I23CR, ISL, VSEL, VRMAD1, VRMAD2, BTCR };
  PUT(pc_);
  for (size_t i = 0; i < sizeof REGS; i++) PUT(sfr_[REGS[i]]);
  PUT(sfr_[EXT]); PUT(t0l_); PUT(t0h_); PUT(t1l_); PUT(t1h_); PUT(btCounter_);
  for (int b = 0; b < 2; b++) for (int i = 0; i < 0x100; i++) PUT(ram_[b][i]);
  for (int b = 0; b < 3; b++) for (int i = 0; i < 0x80; i++) PUT(xram_[b][i]);
#undef PUT
  return n;
}

}  // namespace vmu
