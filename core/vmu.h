// web-vmu core - Sega Dreamcast Visual Memory Unit (VMU/VMS) emulator.
// Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
//
// Sanyo LC86K87 "Potato" CPU, timers, interrupts, LCD, buzzer, flash and work RAM.
// Hardware behaviour follows libElysianVMU (Falco Girgis, MIT) and MAME's lc8670 core
// (Sandro Ronco, BSD-3-Clause); tests/ compares this core with libElysianVMU instruction by instruction.
//
// Portable C++11 with no heap, no libc and no floating point: the same source builds as
// WebAssembly for the website and inside ESP32 firmware. The host owns the 128 KiB flash image
// (that is the memory card file) and, optionally, a 64 KiB BIOS image. Without a BIOS the
// firmware routines games call (flash read/write, clock tick, exit) are emulated, which is all a
// mini-game needs.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace vmu {

enum : uint8_t { BTN_UP = 0x01, BTN_DOWN = 0x02, BTN_LEFT = 0x04, BTN_RIGHT = 0x08, BTN_A = 0x10, BTN_B = 0x20, BTN_MODE = 0x40, BTN_SLEEP = 0x80 };
enum : uint8_t { ICON_FILE = 1, ICON_GAME = 2, ICON_CLOCK = 4, ICON_FLASH = 8 };

static const uint32_t FLASH_SIZE = 0x20000, BIOS_SIZE = 0x10000;
static const int LCD_W = 48, LCD_H = 32, LCD_ROW_BYTES = 6;
static const uint32_t STATE_VERSION = 1;

struct DateTime { uint16_t year; uint8_t month, day, hour, minute, second; };

// ---- save states: one sync() both saves and loads, so the two cannot drift apart
class State {
 public:
  enum Mode { Measure, Save, Load };
  State(Mode m, uint8_t* buf, size_t cap) : mode_(m), buf_(buf), cap_(cap), n_(0), ok_(true) {}
  template <typename T> void v(T& x) {
    uint64_t u = mode_ == Load ? 0 : (uint64_t)x;
    for (size_t i = 0; i < sizeof(T); i++) {
      if (mode_ == Load) { if (n_ < cap_) u |= (uint64_t)buf_[n_] << (8 * i); else ok_ = false; }
      else if (mode_ == Save) { if (n_ < cap_) buf_[n_] = (uint8_t)(u >> (8 * i)); else ok_ = false; }
      n_++;
    }
    if (mode_ == Load) x = (T)u;
  }
  void b(bool& x) { uint8_t t = x; v(t); x = t != 0; }
  void bytes(uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) v(p[i]); }
  bool loading() const { return mode_ == Load; }
  size_t size() const { return n_; }
  bool ok() const { return ok_; }
 private:
  Mode mode_; uint8_t* buf_; size_t cap_, n_; bool ok_;
};

class Vmu {
 public:
  // ---- setup
  // flash: 128 KiB memory-card image owned by the caller (written to when a game saves).
  // bios:  64 KiB BIOS image, or nullptr to emulate the firmware calls instead.
  void init(uint8_t* flash, const uint8_t* bios);
  // Power on. Without a BIOS this starts the game stored at the beginning of the flash.
  void reset(const DateTime& now);

  // ---- running
  void runUs(uint32_t microseconds);              // run for this much of the VMU's own time
  int step();                                     // one instruction, or one idle cycle; returns CPU cycles
  void setButtons(uint8_t pressed);               // BTN_* mask of the buttons currently held
  uint8_t buttons() const { return buttons_; }

  // ---- screen
  bool lcdOn() const;
  void frame(uint8_t out[LCD_H * LCD_ROW_BYTES]) const;   // 32 rows of 6 bytes, bit 7 = leftmost pixel, 1 = dark
  uint8_t icons() const;                          // ICON_* mask

  // ---- sound: signed 16-bit samples into a ring the host drains
  void audioInit(uint32_t sampleRate, int16_t* ring, uint32_t ringSize);
  uint32_t audioRead(int16_t* dst, uint32_t max);
  uint32_t audioAvailable() const { return ringSize_ ? (ringW_ + ringSize_ - ringR_) % ringSize_ : 0; }   // samples waiting to be read
  int32_t audioLevel = 6000;                      // square-wave amplitude (of 32767)

  // ---- status
  bool exited() const { return exited_; }         // the game handed control back to the system menu (no BIOS to show one)
  bool sleeping() const;                          // HOLD: everything stopped until a button is pressed
  bool halted() const;                            // HALT: CPU waiting for an interrupt
  bool hasBios() const { return bios_ != nullptr; }
  bool flashDirty = false;                        // set when the flash image was written; the host clears it after saving
  void (*onFlashWrite)(void* ctx, uint32_t address) = nullptr;
  void* cbCtx = nullptr;
  DateTime dateTime() const;                      // the VMU's clock (system variables kept by the firmware)
  void setDateTime(const DateTime& d);
  uint32_t cyclesPerSecond() const;               // current CPU speed (depends on the oscillator the game selected)

  // ---- save states (fixed size; the flash image is not included, it is the memory card)
  size_t saveState(uint8_t* out, size_t cap);     // out = nullptr measures
  bool loadState(const uint8_t* in, size_t len);

  // ---- debugging / tests
  uint16_t pc() const { return pc_; }
  size_t debugRegs(uint32_t* out, size_t cap) const;
  uint8_t peek(uint16_t address) const;           // data memory without side effects
  const uint8_t* ram(int bank) const { return ram_[bank & 1]; }
  uint64_t totalCycles = 0;

 private:
  // memory
  uint8_t* flash_ = nullptr;
  const uint8_t* bios_ = nullptr;
  uint8_t sfr_[0x80];
  uint8_t ram_[2][0x100];
  uint8_t xram_[3][0x80];
  uint8_t wram_[0x200];
  uint8_t xbank_ = 0;
  uint16_t pc_ = 0;

  // timers
  uint8_t t0l_ = 0, t0h_ = 0, t1l_ = 0, t1h_ = 0;
  int32_t t0Base_ = 0, t0Scale_ = 256;
  uint32_t t0Delay_ = 0, t1Delay_ = 0, btDelay_ = 0;
  uint16_t btCounter_ = 0;
  uint32_t btRemainder_ = 0;

  // interrupts
  uint16_t intReq_ = 0, intStack_[3] = {0, 0, 0};
  bool irqSkip_ = false;                          // no interrupt is taken right after RETI
  uint8_t prevIntPriority_ = 0;

  // execution
  uint8_t prevCycles_ = 0, curCycles_ = 0;
  uint8_t buttons_ = 0;
  uint8_t flashUnlock_ = 0;                       // BIOS flash-programming sequence state
  bool exited_ = false;
  // time is counted in CPU cycles of whichever clock is selected (oscillator / divider); all 32-bit on purpose
  uint32_t osc_ = 879236, clkDiv_ = 12, btInc_ = 0;
  int32_t cycleBudget_ = 0;                       // cycles runUs() still owes
  uint64_t usRemainder_ = 0;                      // part of a cycle carried from one runUs() call to the next

  // buzzer
  uint8_t toneT1lr_ = 0, toneT1lc_ = 0;
  uint16_t tonePeriod_ = 0, toneLow_ = 0;
  bool toneOn_ = false;
  uint32_t tonePhase_ = 0, toneStepQ16_ = 0;      // in timer ticks << 16
  uint32_t sampleRate_ = 0;
  uint32_t audioInc_ = 0, audioAcc_ = 0;          // a sample is due each time the accumulator passes the oscillator frequency
  int16_t* ring_ = nullptr;
  uint32_t ringSize_ = 0, ringW_ = 0;
  volatile uint32_t ringR_ = 0;

  // CPU helpers
  uint8_t prog(uint16_t a) const;
  uint8_t readMem(uint16_t a);
  uint8_t readLatch(uint16_t a);
  void writeMem(uint16_t a, uint8_t v);
  uint8_t rd(uint16_t a);                         // the same three, with the common case (plain RAM) inline
  uint8_t rdl(uint16_t a);
  void wr(uint16_t a, uint8_t v);
  void setAcc(uint8_t v);
  uint16_t indirect(uint8_t mode);
  void push(uint8_t v) { ram_[0][++sfr_[0x06]] = v; }
  uint8_t pop() { return ram_[0][sfr_[0x06]--]; }
  int execute();
  void arith(uint8_t b, bool carryIn, bool subtract);
  void flashStore();
  // peripherals
  void picUpdate();
  bool picCheck(int priority);
  uint16_t irqsEnabled(int priority) const;
  void raiseIrq(int irq) { intReq_ |= (uint16_t)(1u << irq); }
  void timersUpdate(uint32_t cycles);
  void clockChanged();
  void buzzerSink(uint16_t addr);
  void audioSample();
  void pushSample(int16_t s) { uint32_t nw = ringW_ + 1; if (nw == ringSize_) nw = 0; if (nw != ringR_) { ring_[ringW_] = s; ringW_ = nw; } }
  void buzzerLatch(bool compare);
  void buzzerUpdate();
  bool buzzerConfigured() const;
  uint8_t port3() const;
  // firmware emulation
  void firmwareCall();
  void tickClock();
  uint32_t gameEnd() const;
  size_t sync(State& s);
};

}  // namespace vmu
