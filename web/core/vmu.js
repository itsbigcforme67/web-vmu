// web-vmu - Copyright (C) 2026 the web-vmu authors.
// Free software under the GNU General Public License, version 3 or later; see LICENSE. No warranty.
// JavaScript wrapper around core/vmu.wasm (the C++ core in ../../core, built by build.sh).
// No DOM use: works in browsers and in Node (tests/).

export const CARD_SIZE = 0x20000, BIOS_SIZE = 0x10000, LCD_W = 48, LCD_H = 32;
export const BTN = { up: 1, down: 2, left: 4, right: 8, a: 16, b: 32, mode: 64, sleep: 128 };
export const ICON = { file: 1, game: 2, clock: 4, flash: 8 };
export const TYPE_GAME = 0xCC, TYPE_DATA = 0x33;
export const FS_ERRORS = { '-1': 'the card is not formatted', '-2': 'not enough free space on the card', '-3': 'the card already holds a mini-game (a VMU has room for one)',
  '-4': 'the card\'s directory is full', '-5': 'not a file a VMU can hold', '-6': 'a file with that name is already on the card' };

let compiled;
/** Fetch and compile vmu.wasm once. */
export function loadCore(url = new URL('./vmu.wasm', import.meta.url)) {
  if (!compiled) compiled = fetch(url).then((r) => { if (!r.ok) throw new Error('vmu.wasm: ' + r.status); return r.arrayBuffer(); }).then((b) => WebAssembly.compile(b));
  return compiled;
}
export const compileCore = (bytes) => WebAssembly.compile(bytes);

const dateArgs = (d = new Date()) => [d.getFullYear(), d.getMonth() + 1, d.getDate(), d.getHours(), d.getMinutes(), d.getSeconds()];
const cstr = (m, p, n) => { let s = ''; for (let i = 0; i < n && m[p + i]; i++) s += String.fromCharCode(m[p + i]); return s; };

/** One VMU with one memory card in it. */
export class Vmu {
  static async create(module) {
    const inst = await WebAssembly.instantiate(module, {});
    return new Vmu(inst.exports);
  }

  constructor(x) {
    this.x = x;
    x.vmu_init();
    const alloc = (n) => { const p = x.vmu_alloc(n); if (!p) throw new Error('emulator memory is full'); return p; };
    this.flashPtr = x.vmu_flash();
    this.biosPtr = x.vmu_bios();
    this.framePtr = alloc(LCD_H * 6);
    this.audioPtr = alloc(8192 * 2);
    this.infoPtr = alloc(96);
    this.namePtr = alloc(16);
    this.stateCap = x.vmu_state_save(0, 0) + 64;
    this.statePtr = alloc(this.stateCap);
    this.filePtr = alloc(CARD_SIZE);
    this.pixels = new Uint8Array(LCD_W * LCD_H);
  }
  get mem() { return new Uint8Array(this.x.memory.buffer); }

  // ---- the card
  /** The 128 KiB card image the machine runs from (a live view: copy it before keeping it). */
  get card() { return new Uint8Array(this.x.memory.buffer, this.flashPtr, CARD_SIZE); }
  setCard(bytes) { if (bytes.length !== CARD_SIZE) throw new Error('a memory card image is exactly 128 KiB'); this.mem.set(bytes, this.flashPtr); this.x.vmu_clear_dirty(); }
  getCard() { return this.card.slice(); }
  /** True once the game has written to the card since the last call. */
  takeDirty() { const d = (this.x.vmu_status() & 8) !== 0; if (d) this.x.vmu_clear_dirty(); return d; }

  /** Optional real firmware (64 KiB). Without it, the firmware's services are emulated. */
  setBios(bytes) {
    if (bytes && bytes.length !== BIOS_SIZE) throw new Error('a VMU BIOS is exactly 64 KiB');
    if (bytes) this.mem.set(bytes, this.biosPtr);
    this.x.vmu_use_bios(bytes ? 1 : 0);
  }

  // ---- running
  reset(date) { this.x.vmu_reset(...dateArgs(date)); }
  setTime(date) { this.x.vmu_set_time(...dateArgs(date)); }
  audio(sampleRate, level = 6000) { this.x.vmu_audio(sampleRate, level); }
  buttons(mask) { this.x.vmu_buttons(mask); }
  /** Run the machine for this many microseconds of its own time. */
  runUs(us) { this.x.vmu_run_us(us); }
  step() { return this.x.vmu_step(); }
  get pc() { return this.x.vmu_pc() >>> 0; }
  get status() { const s = this.x.vmu_status(); return { exited: !!(s & 1), sleeping: !!(s & 2), halted: !!(s & 4), dirty: !!(s & 8), bios: !!(s & 16) }; }
  stateHash() { return this.x.vmu_state_hash() >>> 0; }
  flashHash() { return this.x.vmu_flash_hash() >>> 0; }

  /** The screen: { on, icons (ICON bits), pixels: Uint8Array(48*32) of 0/1, reused between calls }. */
  frame() {
    const r = this.x.vmu_frame(this.framePtr), m = this.mem, px = this.pixels;
    for (let y = 0, o = 0; y < LCD_H; y++) for (let b = 0; b < 6; b++) { const v = m[this.framePtr + y * 6 + b]; for (let k = 0; k < 8; k++) px[o++] = (v >> (7 - k)) & 1; }
    return { on: !!(r & 1), icons: (r >> 8) & 15, pixels: px };
  }
  /** Sound produced since the last call: Int16Array view, valid until the next call. */
  readAudio() { const n = this.x.vmu_audio_read(this.audioPtr, 8192); return new Int16Array(this.x.memory.buffer, this.audioPtr, n); }

  saveState() { const n = this.x.vmu_state_save(this.statePtr, this.stateCap); return n ? this.mem.slice(this.statePtr, this.statePtr + n) : null; }
  /** Restore a snapshot (the card is not part of it). Returns false if it is not one of ours. */
  loadState(bytes) {
    if (!bytes || bytes.length > this.stateCap) return false;
    this.mem.set(bytes, this.statePtr);
    return !!this.x.vmu_state_load(this.statePtr, bytes.length);
  }

  // ---- files on the card
  get formatted() { return !!this.x.vmu_fs_formatted(); }
  format(date) { this.x.vmu_fs_format(...dateArgs(date)); }
  get freeBlocks() { return this.x.vmu_fs_free(); }
  /** Everything on the card: [{ index, type, game, copyProtected, firstBlock, blocks, name, title, longTitle, date }]. */
  files() {
    const out = [];
    for (let i = 0; i < 208; i++) {
      if (!this.x.vmu_fs_file(i, this.infoPtr)) continue;
      const m = this.mem, p = this.infoPtr;
      out.push({ index: i, type: m[p], game: m[p] === TYPE_GAME, copyProtected: !!m[p + 1], firstBlock: m[p + 2] | (m[p + 3] << 8), blocks: m[p + 4] | (m[p + 5] << 8),
        date: new Date(m[p + 6] | (m[p + 7] << 8), Math.max(0, m[p + 8] - 1), m[p + 9] || 1, m[p + 10], m[p + 11]),
        name: cstr(m, p + 12, 13), title: cstr(m, p + 25, 17).trim(), longTitle: cstr(m, p + 42, 33).trim() });
    }
    return out;
  }
  get game() { return this.files().find((f) => f.game) || null; }
  _put(bytes) { if (bytes.length > CARD_SIZE) throw new Error('too big for a memory card'); this.mem.set(bytes, this.filePtr); return this.filePtr; }
  _check(r) { if (r < 0) throw new Error(FS_ERRORS[r] || 'card error ' + r); return r; }
  /** Store a raw .VMS on the card. Returns its directory slot; throws with a readable message if it does not fit. */
  addFile(name, bytes, { game = this.looksLikeGame(bytes), date, copyProtected = false } = {}) {
    const m = this.mem, clean = String(name).toUpperCase().replace(/[^A-Z0-9_.\-]/g, '_').slice(0, 12) || 'FILE';
    for (let i = 0; i < 13; i++) m[this.namePtr + i] = i < clean.length ? clean.charCodeAt(i) : 0;
    return this._check(this.x.vmu_fs_add(this.namePtr, game ? TYPE_GAME : TYPE_DATA, this._put(bytes), bytes.length, ...dateArgs(date), copyProtected ? 1 : 0));
  }
  addDci(bytes) { return this._check(this.x.vmu_fs_add_dci(this._put(bytes), bytes.length)); }
  /** Replace the whole card with a .DCM image. Returns false (card untouched in JS terms: reload it) if it is not one. */
  setCardFromDcm(bytes) { if (bytes.length !== CARD_SIZE) return false; const ok = !!this.x.vmu_fs_from_dcm(this._put(bytes), bytes.length); this.x.vmu_clear_dirty(); return ok; }
  removeFile(index) { return !!this.x.vmu_fs_remove(index); }
  readFile(index) { const n = this.x.vmu_fs_read(index, this.filePtr, CARD_SIZE); return this.mem.slice(this.filePtr, this.filePtr + n); }
  looksLikeGame(bytes) { return !!this.x.vmu_fs_looks_like_game(this._put(bytes), bytes.length); }
  /** Read a .VMI description file: { fileName, description, game, copyProtected } or null. */
  parseVmi(bytes) {
    const r = this.x.vmu_fs_parse_vmi(this._put(bytes), bytes.length, this.infoPtr);
    if (!(r & 1)) return null;
    const m = this.mem;
    return { fileName: cstr(m, this.infoPtr, 13), description: cstr(m, this.infoPtr + 13, 33).trim(), game: !!(r & 2), copyProtected: !!(r & 4) };
  }
}
