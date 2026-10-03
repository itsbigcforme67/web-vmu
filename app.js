// web-vmu - Copyright (C) 2026 the web-vmu authors.
// Free software under the GNU General Public License, version 3 or later; see LICENSE. No warranty.
// Phone/desktop UI: library of memory cards, importing games, the VMU on screen, sound, saves.
// The machine itself is core/vmu.wasm (C++ in ../core).
import { loadCore, Vmu, BTN, CARD_SIZE, BIOS_SIZE, LCD_W, LCD_H } from './core/vmu.js';
import { isZip, unzip } from './zip.js';

const $ = (s) => document.querySelector(s);
const STATE_FORMAT = 1;

// ------------------------------------------------------------------ storage (IndexedDB)
const db = (() => {
  let p;
  const open = () => p || (p = new Promise((res, rej) => {
    const r = indexedDB.open('web-vmu', 1);
    r.onupgradeneeded = () => r.result.createObjectStore('kv');
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  }));
  const tx = async (mode, fn) => {
    const d = await open();
    return new Promise((res, rej) => {
      const t = d.transaction('kv', mode);
      const req = fn(t.objectStore('kv'));
      t.oncomplete = () => res(req && req.result);
      t.onerror = () => rej(t.error);
    });
  };
  return {
    get: (k) => tx('readonly', (s) => s.get(k)),
    set: (k, v) => tx('readwrite', (s) => s.put(v, k)),
    del: (k) => tx('readwrite', (s) => s.delete(k)),
    keys: () => tx('readonly', (s) => s.getAllKeys()),
  };
})();

const prefs = {
  get(k, d) { try { const v = localStorage.getItem('wvm.' + k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem('wvm.' + k, JSON.stringify(v)); } catch { /* private mode */ } },
};

async function sha1(bytes) {
  if (!(globalThis.crypto && crypto.subtle)) return '';      // plain http on a LAN address: no hashing available
  const h = await crypto.subtle.digest('SHA-1', bytes);
  return Array.from(new Uint8Array(h), (b) => b.toString(16).padStart(2, '0')).join('');
}

// ------------------------------------------------------------------ UI helpers
function toast(msg, ms = 2600) {
  const t = $('#toast');
  t.textContent = msg; t.classList.remove('hidden');
  clearTimeout(toast.timer); toast.timer = setTimeout(() => t.classList.add('hidden'), ms);
}
function sheet(title, bodyHTML, actions, onOpen) {
  return new Promise((resolve) => {
    $('#sheet-title').textContent = title;
    $('#sheet-body').innerHTML = bodyHTML || '';
    const box = $('#sheet-actions');
    box.innerHTML = '';
    const s = $('#sheet');
    const close = (v) => { s.classList.add('hidden'); s.onclick = null; resolve(v); };
    for (const a of actions) {
      const b = document.createElement('button');
      b.className = 'btn ' + (a.cls || '');
      b.textContent = a.label;
      b.onclick = () => close(typeof a.value === 'function' ? a.value() : a.value);
      box.appendChild(b);
    }
    s.onclick = (e) => { if (e.target === s) close(null); };
    s.classList.remove('hidden');
    if (onOpen) onOpen($('#sheet-body'), close);
  });
}
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
function showView(id) {
  document.querySelectorAll('.view').forEach((v) => v.classList.toggle('active', v.id === id));
  document.documentElement.classList.toggle('in-player', id === 'player');
}
function ago(t) {
  const s = Math.round((Date.now() - t) / 1000);
  if (s < 60) return 'just now';
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  if (s < 86400) return `${Math.round(s / 3600)} h ago`;
  return new Date(t).toLocaleDateString();
}
function download(name, bytes) {
  const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
  const a = document.createElement('a');
  a.href = url; a.download = name; document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 5000);
  window.webVmu.lastDownload = { name, bytes };
}
const baseName = (n) => n.split(/[\\/]/).pop().replace(/\.[^.]*$/, '');
const extOf = (n) => (/\.([^.\\/]+)$/.exec(n) || [, ''])[1].toLowerCase();
const plural = (n, word) => `${n} ${word}${n === 1 ? '' : 's'}`;

// ------------------------------------------------------------------ memory cards
// Each card is kept whole, as the 128 KiB image a real VMU (or an emulator's vmu_save_*.bin) holds:
//   card:<id>  -> Uint8Array(131072)
//   meta:<id>  -> { id, name, created, modified, sha1, free, files: [...], game: { name, title, detail, icon } | null }
//   state:<id> -> { format, data, saved, game, bios }   where the game was left (not the card)
let cards = [];            // every meta, sorted by name
let saved = {};            // card id -> time of its snapshot
let tool = null;           // a spare VMU used only to look inside cards and change their files
async function getTool() { return tool || (tool = await Vmu.create(await loadCore())); }

/** The first icon of a VMU file as a PNG data URL ('' if it has none). `at` is where the file's header starts. */
function iconOf(bytes, at) {
  if (bytes.length < at + 0x80 + 512) return '';
  const count = bytes[at + 0x40] | (bytes[at + 0x41] << 8);
  if (count < 1 || count > 64) return '';
  const pal = [];
  for (let i = 0; i < 16; i++) { const v = bytes[at + 0x60 + i * 2] | (bytes[at + 0x61 + i * 2] << 8); pal.push([((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 15) * 17]); }
  const c = document.createElement('canvas'); c.width = c.height = 32;
  const g = c.getContext('2d'), img = g.createImageData(32, 32);
  let any = false;
  for (let i = 0; i < 512; i++) {
    const b = bytes[at + 0x80 + i];
    for (const [k, n] of [[0, b >> 4], [1, b & 15]]) { img.data.set(pal[n], (i * 2 + k) * 4); if (pal[n][3]) any = true; }
  }
  if (!any) return '';
  g.putImageData(img, 0, 0);
  return c.toDataURL();
}

/** What is on a card image: null if it is not a formatted VMU card. */
async function describe(bytes) {
  const t = await getTool();
  t.setCard(bytes);
  if (!t.formatted) return null;
  const files = t.files();
  const out = { free: t.freeBlocks, files: files.map((f) => ({ name: f.name, game: f.game, blocks: f.blocks, title: f.title, detail: f.longTitle })), game: null };
  const g = files.find((f) => f.game);
  if (g) out.game = { name: g.name, title: g.title || g.name, detail: g.longTitle, icon: iconOf(t.readFile(g.index), 0x200) };
  return out;
}

async function putCard(meta, bytes) {
  const d = await describe(bytes);
  if (!d) throw new Error('not a formatted VMU memory card');
  Object.assign(meta, d, { modified: Date.now(), sha1: await sha1(bytes) });
  await db.set('card:' + meta.id, bytes);
  await db.set('meta:' + meta.id, meta);
  return meta;
}
const newMeta = (name) => ({ id: Array.from(crypto.getRandomValues(new Uint8Array(6)), (b) => b.toString(16).padStart(2, '0')).join(''), name, created: Date.now() });
function freeName(want) {
  const taken = new Set(cards.map((c) => c.name.toLowerCase()));
  let name = want.trim() || 'Memory card';
  for (let n = 2; taken.has(name.toLowerCase()); n++) name = `${want} (${n})`;
  return name;
}
async function newCard(name, bytes) {
  if (!bytes) { const t = await getTool(); t.format(new Date()); bytes = t.getCard(); }
  const meta = await putCard(newMeta(freeName(name)), bytes);
  cards.push(meta);
  return meta;
}
async function deleteCard(meta) {
  await db.del('card:' + meta.id); await db.del('meta:' + meta.id); await db.del('state:' + meta.id);
  cards = cards.filter((c) => c !== meta); delete saved[meta.id];
}

async function refreshStore() {
  const keys = (await db.keys()).filter((k) => typeof k === 'string');
  cards = [];
  for (const k of keys) if (k.startsWith('meta:')) { const m = await db.get(k); if (m) cards.push(m); }
  saved = {};
  for (const k of keys) if (k.startsWith('state:')) { const s = await db.get(k); if (s) saved[k.slice(6)] = s.saved; }
}

// ------------------------------------------------------------------ importing
const CARD_FILE = /\.(vms|vmi|dci|dcm|bin|vmu|vmc|sav|srm)$/i;

/** Add one .VMS or .DCI to a card. Throws with a readable reason if it does not fit. */
async function addToCard(meta, item) {
  const t = await getTool();
  t.setCard(await db.get('card:' + meta.id));
  if (item.kind === 'dci') t.addDci(item.data);
  else t.addFile(item.fileName, item.data, { game: item.game, copyProtected: item.copyProtected });
  if (item.game) { await db.del('state:' + meta.id); delete saved[meta.id]; }
  await putCard(meta, t.getCard());
}

/** Take whatever the user chose (games, saves, whole cards, zips of them). `target` = add to this card only. */
async function importFiles(fileList, target) {
  const t = await getTool();
  const files = [];
  for (const f of fileList) {
    const buf = f.data || new Uint8Array(await f.arrayBuffer());
    if (isZip(buf)) {
      try { files.push(...await unzip(buf, (name) => CARD_FILE.test(name))); }
      catch (e) { toast(`${f.name}: ${e.message}`); }
    } else files.push({ name: f.name, data: buf });
  }
  // .VMI files only describe the .VMS that goes with them
  const vmis = new Map();
  for (const f of files) if (extOf(f.name) === 'vmi') { const v = t.parseVmi(f.data); if (v) vmis.set(baseName(f.name).toLowerCase(), v); }

  const done = { cards: 0, games: 0, saves: 0 }, problems = [];
  const items = [];
  for (const f of files) {
    const ext = extOf(f.name), base = baseName(f.name);
    if (ext === 'vmi') continue;
    if (f.data.length === CARD_SIZE) {
      // a whole card: raw (.bin, .vmu, emulator saves) or byte-swapped (.dcm)
      let bytes = f.data, ok = false;
      if (ext !== 'dcm') { t.setCard(bytes); ok = t.formatted; }
      if (!ok) { ok = t.setCardFromDcm(f.data); bytes = t.getCard(); }
      if (!ok) { problems.push(`${f.name} is not a formatted VMU card`); continue; }
      if (target) { problems.push(`${f.name} is a whole card: import it from the library screen`); continue; }
      await newCard(base, bytes); done.cards++;
      continue;
    }
    if (ext === 'dci') {
      if (f.data.length < 32 + 512 || (f.data.length - 32) % 512) { problems.push(`${f.name} is not a .dci file`); continue; }
      items.push({ kind: 'dci', data: f.data, game: f.data[0] === 0xCC, title: base, src: f.name });
      continue;
    }
    if (ext !== 'vms') { problems.push(`${f.name} is not a memory card (those are exactly 128 KB) or a .vms / .dci file`); continue; }
    if (!f.data.length || f.data.length > 200 * 512) { problems.push(`${f.name} is not a VMU file`); continue; }
    const vmi = vmis.get(base.toLowerCase());
    items.push({ kind: 'vms', data: f.data, game: vmi ? vmi.game : t.looksLikeGame(f.data), fileName: (vmi && vmi.fileName) || base, copyProtected: !!(vmi && vmi.copyProtected), title: base, src: f.name });
  }

  // games get a card each (a VMU holds one game); other files go where the user says
  let dest = target || null;
  for (const it of items.filter((i) => i.game)) {
    try {
      if (target) await addToCard(target, it);
      else { const meta = await newCard(it.title); await addToCard(meta, it); if (meta.game) { meta.name = ''; meta.name = freeName(meta.game.title); await db.set('meta:' + meta.id, meta); } dest = dest || meta; }
      done.games++;
    } catch (e) { problems.push(`${it.src}: ${e.message}`); }
  }
  const rest = items.filter((i) => !i.game);
  if (rest.length) {
    if (!dest || (!target && done.games !== 1)) dest = await chooseCard(`Where should ${rest.length === 1 ? rest[0].src : plural(rest.length, 'save file')} go?`);
    if (dest) for (const it of rest) { try { await addToCard(dest, it); done.saves++; } catch (e) { problems.push(`${it.src}: ${e.message}`); } }
  }
  await refreshStore(); renderLibrary();
  const parts = [];
  if (done.cards) parts.push(plural(done.cards, 'card'));
  if (done.games) parts.push(plural(done.games, 'game'));
  if (done.saves) parts.push(plural(done.saves, 'save file'));
  if (problems.length) sheet(parts.length ? `Added ${parts.join(', ')}` : 'Nothing was added', `<p>${problems.map(esc).join('</p><p>')}</p>`, [{ label: 'OK', value: null }]);
  else toast(parts.length ? `Added ${parts.join(', ')}` : 'No VMU files were found in that selection', 3200);
  return done;
}

async function chooseCard(question) {
  const acts = cards.map((c) => ({ label: `${c.name} (${c.free} blocks free)`, value: c }));
  acts.push({ label: 'A new card', value: 'new', cls: 'primary' }, { label: 'Cancel', value: null });
  const v = await sheet('Choose a memory card', `<p>${esc(question)}</p>`, acts);
  return v === 'new' ? newCard('Memory card') : v;
}

async function addDemo() {
  try {
    const r = await fetch('demo/sketch.vms');
    if (!r.ok) throw new Error(r.status);
    await importFiles([{ name: 'Sketch.vms', data: new Uint8Array(await r.arrayBuffer()) }]);
  } catch (e) { toast('Could not load the test game: ' + e.message); }
}

// ------------------------------------------------------------------ library
function renderLibrary() {
  cards.sort((a, b) => a.name.localeCompare(b.name));
  const grid = $('#game-grid');
  grid.innerHTML = '';
  const withGame = cards.filter((c) => c.game);
  for (const c of withGame) {
    const el = document.createElement('div');
    el.className = 'game';
    el.setAttribute('role', 'button'); el.tabIndex = 0;
    const g = c.game;
    el.innerHTML = `<div class="thumb">${g.icon ? `<img src="${g.icon}" alt="">` : `<div class="noicon">${esc((g.title || '?').trim().charAt(0).toUpperCase())}</div>`}</div>
      <div class="name">${esc(g.title)}</div>
      <div class="meta">${saved[c.id] ? `<span class="tag good">saved ${esc(ago(saved[c.id]))}</span>` : ''}<span>${esc(g.detail && g.detail !== g.title ? g.detail : c.name)}</span></div>
      <button class="more" aria-label="Options for ${esc(g.title)}">⋯</button>`;
    el.onclick = (e) => { if (e.target.closest('.more')) cardMenu(c); else openCard(c); };
    el.onkeydown = (e) => { if (e.key === 'Enter') el.click(); };
    grid.appendChild(el);
  }
  $('#no-games').classList.toggle('hidden', withGame.length > 0 || cards.length === 0);
  const list = $('#card-list');
  list.innerHTML = '';
  for (const c of cards) {
    const el = document.createElement('div');
    el.className = 'card';
    el.setAttribute('role', 'button'); el.tabIndex = 0;
    const n = c.files.length;
    el.innerHTML = `<span class="chip" aria-hidden="true"></span>
      <div class="info"><div class="name">${esc(c.name)}</div><div class="meta">${n ? plural(n, 'file') : 'empty'} · ${c.free} of 200 blocks free${c.game ? ' · ' + esc(c.game.title) : ''}</div></div>
      <button class="more" aria-label="Options for ${esc(c.name)}">⋯</button>`;
    el.onclick = () => cardMenu(c);
    el.onkeydown = (e) => { if (e.key === 'Enter') el.click(); };
    list.appendChild(el);
  }
  $('#lib-status').textContent = cards.length ? `${plural(withGame.length, 'game')} on ${plural(cards.length, 'memory card')}` : 'Games and saves are stored on this device only';
  $('#empty-banner').classList.toggle('hidden', cards.length > 0);
  document.querySelectorAll('#lib-content .section-title, #card-list, .import-row').forEach((e) => e.classList.toggle('hidden', cards.length === 0));
}

async function cardMenu(c) {
  const acts = [];
  if (c.game || prefs.get('useBios', false)) acts.push({ label: c.game ? (saved[c.id] ? `Continue ${c.game.title}` : `Play ${c.game.title}`) : 'Open in the VMU\'s own menu', value: 'play', cls: 'primary' });
  acts.push({ label: 'Files on this card…', value: 'files' }, { label: 'Add files to this card…', value: 'add' }, { label: 'Download the card (.bin)', value: 'save' }, { label: 'Rename…', value: 'rename' });
  if (saved[c.id]) acts.push({ label: 'Start the game from the beginning next time', value: 'fresh' });
  acts.push({ label: 'Delete this card', value: 'delete', cls: 'danger' }, { label: 'Close', value: null });
  const v = await sheet(c.name, `<p>${c.files.length ? plural(c.files.length, 'file') : 'Empty'}, ${c.free} of 200 blocks free. Changed ${esc(ago(c.modified))}.</p>`, acts);
  if (v === 'play') openCard(c);
  if (v === 'files') filesSheet(c);
  if (v === 'add') { const inp = $('#add-input'); inp.value = ''; inp.onchange = async () => { const f = [...inp.files]; if (f.length) await importFiles(f, c); }; inp.click(); }
  if (v === 'save') download(c.name.replace(/[^\w.\- ()]+/g, '_') + '.bin', await db.get('card:' + c.id));
  if (v === 'fresh') { await db.del('state:' + c.id); delete saved[c.id]; renderLibrary(); toast('It will start from the beginning'); }
  if (v === 'rename') {
    const name = await sheet('Rename card', `<input type="text" id="rename" maxlength="40" value="${esc(c.name)}">`, [{ label: 'Save', value: () => $('#rename').value, cls: 'primary' }, { label: 'Cancel', value: null }]);
    if (name && name.trim() && name.trim() !== c.name) { c.name = ''; c.name = freeName(name.trim()); await db.set('meta:' + c.id, c); renderLibrary(); }
  }
  if (v === 'delete') {
    const ok = await sheet(`Delete ${c.name}?`, '<p>The card, everything saved on it and its snapshot are removed from this device. Download it first if you want a copy.</p>', [{ label: 'Delete', value: true, cls: 'danger' }, { label: 'Cancel', value: false }]);
    if (ok) { await deleteCard(c); renderLibrary(); }
  }
}

async function filesSheet(c) {
  const t = await getTool();
  const bytes = await db.get('card:' + c.id);
  t.setCard(bytes);
  const files = t.files();
  const rows = files.map((f, i) => {
    const icon = iconOf(t.readFile(f.index), f.game ? 0x200 : 0);
    return `<tr><td>${icon ? `<img src="${icon}" alt="">` : ''}</td><td><div style="color:var(--text);font-size:14px">${esc(f.title || f.name)}</div>${esc(f.name)} · ${f.game ? 'game' : 'save'} · ${plural(f.blocks, 'block')}</td>
      <td><button class="mini" data-get="${i}">Download</button> <button class="mini danger" data-del="${i}">Delete</button></td></tr>`;
  }).join('');
  const again = await sheet(c.name, files.length ? `<table class="files">${rows}</table>` : '<p>This card is empty.</p>', [{ label: 'Close', value: false }], (el, close) => {
    el.querySelectorAll('[data-get]').forEach((b) => { b.onclick = () => { const f = files[+b.dataset.get]; t.setCard(bytes); download(f.name + '.vms', t.readFile(f.index)); }; });
    el.querySelectorAll('[data-del]').forEach((b) => { b.onclick = async () => {
      const f = files[+b.dataset.del];
      t.setCard(bytes); t.removeFile(f.index);
      if (f.game) { await db.del('state:' + c.id); delete saved[c.id]; }
      await putCard(c, t.getCard());
      renderLibrary(); close(true);
    }; });
  });
  if (again) filesSheet(c);
}

// ------------------------------------------------------------------ player
const player = { meta: null, m: null, raf: 0, last: 0, audio: null, muted: prefs.get('muted', false), pad: {}, opening: false, over: false,
  mask: 0, held: {}, cur: new Float32Array(LCD_W * LCD_H), shown: new Int8Array(LCD_W * LCD_H).fill(-1), cell: 0, cardDirty: false, cardTimer: 0, saving: null, asleep: false };

async function openCard(c) {
  if (player.opening) return;
  player.opening = true;
  try {
    const useBios = prefs.get('useBios', false);
    $('#game-title').textContent = c.game ? c.game.title : c.name;
    $('#game-sub').textContent = !c.game ? 'VMU menu' : c.name !== c.game.title ? c.name : c.game.detail && c.game.detail !== c.game.title ? c.game.detail : 'Memory card';
    showView('player');
    setupAudio();
    const [module, bytes, snap, bios] = await Promise.all([loadCore(), db.get('card:' + c.id), db.get('state:' + c.id), useBios ? db.get('bios') : null]);
    if (!bytes) throw new Error('the card is missing from this device');
    const m = await Vmu.create(module);
    m.setCard(bytes);
    if (bios) m.setBios(bios);
    m.audio(player.audio ? player.audio.ctx.sampleRate : 0, 9000);
    m.reset(new Date());
    let resumed = false;
    if (snap && snap.format === STATE_FORMAT && !!snap.bios === !!bios && snap.data) {
      resumed = m.loadState(snap.data);
      if (resumed) m.setTime(new Date()); else m.reset(new Date());      // the VMU's clock catches up with the real one
    }
    if (m.status.exited) throw new Error('there is no mini-game on this card');
    Object.assign(player, { meta: c, m, last: performance.now(), pad: {}, over: false, cardDirty: false, asleep: false });
    player.cur.fill(0); player.shown.fill(-1);
    sizeLcd();
    applySound();
    note('');
    cancelAnimationFrame(player.raf);
    player.raf = requestAnimationFrame(frame);
    if (resumed) toast('Continued where you left off', 1600);
  } catch (e) {
    console.error(e);
    toast('Could not start: ' + e.message, 4000);
    showView('library');
  } finally { player.opening = false; }
}

function note(text) { const n = $('#player-note'); n.textContent = text; n.classList.toggle('hidden', !text); }

// ---- the screen: 48 x 32 dots that fade in and out the way the real liquid crystal does
const lcd = $('#lcd'), lcdCtx = lcd.getContext('2d');
let shades = [];
function sizeLcd() {
  const css = lcd.getBoundingClientRect().width || 240;
  const cell = Math.max(2, Math.min(24, Math.round(css * (window.devicePixelRatio || 1) / LCD_W)));
  const cs = getComputedStyle(document.documentElement);
  const hex = (name, d) => { const v = (cs.getPropertyValue(name) || d).trim(); return [1, 3, 5].map((i) => parseInt(v.slice(i, i + 2), 16)); };
  const bg = hex('--lcd-bg', '#b7c4ab'), ink = hex('--lcd-ink', '#1b2518');
  shades = [];
  for (let i = 0; i <= 32; i++) { const k = 0.045 + 0.955 * (i / 32); shades.push(`rgb(${bg.map((b, j) => Math.round(b + (ink[j] - b) * k)).join(',')})`); }
  if (cell !== player.cell || lcd.width !== cell * LCD_W) { player.cell = cell; lcd.width = cell * LCD_W; lcd.height = cell * LCD_H; }
  lcdCtx.fillStyle = `rgb(${bg.join(',')})`;
  lcdCtx.fillRect(0, 0, lcd.width, lcd.height);
  player.shown.fill(-1);
}
function drawLcd(snap) {
  const f = player.m.frame();
  const k = snap || !prefs.get('lcdSlow', true) ? 1 : 0.42;
  const cell = player.cell, gap = cell >= 4 && prefs.get('lcdGrid', true) ? 1 : 0, cur = player.cur, shown = player.shown;
  for (let i = 0; i < LCD_W * LCD_H; i++) {
    const target = f.on ? f.pixels[i] : 0;
    let v = cur[i];
    if (v !== target) { v += k * (target - v); if (Math.abs(v - target) < 0.02) v = target; cur[i] = v; }
    const level = Math.round(v * 32);
    if (level !== shown[i]) {
      shown[i] = level;
      lcdCtx.fillStyle = shades[level];
      lcdCtx.fillRect((i % LCD_W) * cell, ((i / LCD_W) | 0) * cell, cell - gap, cell - gap);
    }
  }
  const icons = f.on ? f.icons : 0;
  if (icons !== player.icons) {
    player.icons = icons;
    ['ic-file', 'ic-game', 'ic-clock', 'ic-flash'].forEach((id, i) => document.getElementById(id).classList.toggle('on', !!(icons & (1 << i))));
  }
}

function frame(now) {
  const dt = Math.min(Math.max(now - player.last, 0), 100);   // after a pause (tab hidden) do not try to catch up
  player.last = now;
  const m = player.m;
  pollGamepad();
  m.buttons(player.mask);
  m.runUs(Math.round(dt * 1000));
  pumpAudio();
  drawLcd(false);
  const st = m.x.vmu_status();
  if (st & 8) { m.x.vmu_clear_dirty(); player.cardDirty = true; clearTimeout(player.cardTimer); player.cardTimer = setTimeout(saveCardNow, 1200); }
  if (!!(st & 2) !== player.asleep) { player.asleep = !!(st & 2); note(player.asleep ? 'The VMU is asleep. Press a button to wake it.' : ''); }
  if (st & 1) { gameOver(); return; }
  player.raf = requestAnimationFrame(frame);
}

/** The game handed control back to the VMU's own menu (MODE), which only exists with a real BIOS. */
async function gameOver() {
  player.over = true;
  releaseEverything();
  await saveCardNow();
  await db.del('state:' + player.meta.id); delete saved[player.meta.id];
  const v = await sheet('The game ended', '<p>It handed control back to the VMU (that is what MODE does in a game). Anything it saved is on the memory card.</p>',
    [{ label: 'Play again', value: 'again', cls: 'primary' }, { label: 'Back to the library', value: 'back' }]);
  if (v === 'again' && player.m) { player.m.reset(new Date()); player.over = false; player.last = performance.now(); player.raf = requestAnimationFrame(frame); }
  else closePlayer();
}

function saveCardNow() {
  clearTimeout(player.cardTimer);
  if (!player.cardDirty || !player.m || !player.meta) return player.saving || Promise.resolve();
  player.cardDirty = false;
  const meta = player.meta, bytes = player.m.getCard();
  player.saving = (player.saving || Promise.resolve()).then(() => putCard(meta, bytes)).catch((e) => console.error(e));
  return player.saving;
}
async function saveNow() {
  if (!player.m || !player.meta) return;
  const id = player.meta.id;
  if (player.m.x.vmu_status() & 8) { player.m.x.vmu_clear_dirty(); player.cardDirty = true; }
  const card = saveCardNow();
  if (!player.over) {
    const data = player.m.saveState();
    if (data) { await db.set('state:' + id, { format: STATE_FORMAT, data, saved: Date.now(), game: player.meta.game ? player.meta.game.name : '', bios: player.m.status.bios }); saved[id] = Date.now(); }
  }
  await card;
}

async function closePlayer() {
  cancelAnimationFrame(player.raf); player.raf = 0;
  releaseEverything();
  await saveNow();
  player.m = null; player.meta = null;
  if (player.audio) player.audio.w = player.audio.r = 0;
  await refreshStore();
  renderLibrary();
  showView('library');
}

// ---- buttons: a button can be held by a finger, a key and a gamepad at once; it is released when the last one lets go.
// A very quick tap is stretched to MIN_HOLD_MS so a game that polls the buttons once a frame cannot miss it.
const MIN_HOLD_MS = 70;
function setButton(bit, down, source) {
  const h = player.held[bit] || (player.held[bit] = { by: new Set(), on: false, since: 0, timer: 0 });
  if (down) h.by.add(source); else h.by.delete(source);
  const want = h.by.size > 0;
  if (want === h.on) return;
  const off = () => { h.timer = 0; if (h.by.size || !h.on) return; h.on = false; player.mask &= ~bit; push(); show(bit, false); };
  const push = () => { if (player.m && !player.over) player.m.buttons(player.mask); };
  if (want) { h.on = true; h.since = performance.now(); player.mask |= bit; push(); show(bit, true); }
  else if (!h.timer) { const wait = MIN_HOLD_MS - (performance.now() - h.since); if (wait > 0) h.timer = setTimeout(off, wait); else off(); }
}
const DIR_CLASS = { [BTN.up]: 'up', [BTN.down]: 'down', [BTN.left]: 'left', [BTN.right]: 'right' };
function show(bit, on) {
  if (DIR_CLASS[bit]) $('#dpad').classList.toggle(DIR_CLASS[bit], on);
  else { const b = document.querySelector(`.pbtn[data-btn="${bit}"]`); if (b) b.classList.toggle('pressed', on); }
}
function releaseEverything() {
  for (const [bit, h] of Object.entries(player.held)) { h.by.clear(); clearTimeout(h.timer); h.timer = 0; if (h.on) { h.on = false; show(+bit, false); } }
  player.mask = 0;
  if (player.m) player.m.buttons(0);
}

// touch: the round and oval buttons
for (const b of document.querySelectorAll('.pbtn')) {
  const bit = +b.dataset.btn;
  const down = (e) => { e.preventDefault(); try { b.setPointerCapture(e.pointerId); } catch { /* old browser */ } setButton(bit, true, 'touch' + e.pointerId); resumeAudio(); };
  const up = (e) => { e.preventDefault(); setButton(bit, false, 'touch' + e.pointerId); };
  b.addEventListener('pointerdown', down); b.addEventListener('pointerup', up); b.addEventListener('pointercancel', up);
  b.addEventListener('contextmenu', (e) => e.preventDefault());
}
// touch: the direction pad is one piece, so a thumb can roll from one direction to the next (and press diagonals)
{
  const pad = $('#dpad');
  const dirs = (e) => {
    const r = pad.getBoundingClientRect();
    const x = (e.clientX - r.left) / r.width - 0.5, y = (e.clientY - r.top) / r.height - 0.5;
    if (Math.hypot(x, y) < 0.1) return 0;
    let bits = 0;
    if (Math.abs(x) > Math.abs(y) * 0.45) bits |= x > 0 ? BTN.right : BTN.left;
    if (Math.abs(y) > Math.abs(x) * 0.45) bits |= y > 0 ? BTN.down : BTN.up;
    return bits;
  };
  const apply = (e, bits) => { for (const bit of [BTN.up, BTN.down, BTN.left, BTN.right]) setButton(bit, !!(bits & bit), 'pad' + e.pointerId); };
  pad.addEventListener('pointerdown', (e) => { e.preventDefault(); try { pad.setPointerCapture(e.pointerId); } catch { /* old browser */ } apply(e, dirs(e)); resumeAudio(); });
  pad.addEventListener('pointermove', (e) => { if (e.buttons || e.pointerType === 'touch') apply(e, dirs(e)); });
  pad.addEventListener('pointerup', (e) => { e.preventDefault(); apply(e, 0); });
  pad.addEventListener('pointercancel', (e) => apply(e, 0));
  pad.addEventListener('contextmenu', (e) => e.preventDefault());
}

// ---- sound
function setupAudio() {
  if (player.audio) return;
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return;
  const ctx = new AC();
  const ring = new Float32Array(16384);
  const a = { ctx, ring, r: 0, w: 0 };
  const node = ctx.createScriptProcessor(1024, 0, 1);
  node.onaudioprocess = (e) => {
    const out = e.outputBuffer.getChannelData(0);
    const avail = (a.w - a.r + ring.length) % ring.length;
    for (let i = 0; i < out.length; i++) {
      if (i < avail) { out[i] = ring[a.r]; a.r = (a.r + 1) % ring.length; } else out[i] = 0;
    }
    const left = (a.w - a.r + ring.length) % ring.length;
    if (left > 6000) a.r = (a.w - 2048 + ring.length) % ring.length;   // keep latency bounded
  };
  const gain = ctx.createGain(); gain.gain.value = 0.3;
  node.connect(gain); gain.connect(ctx.destination);
  player.audio = a;
}
function resumeAudio() { if (player.audio && !player.muted && player.audio.ctx.state !== 'running') player.audio.ctx.resume().catch(() => {}); }
function applySound() {
  $('#btn-sound').textContent = player.muted ? '🔇' : '🔈';
  if (player.audio) { if (player.muted) player.audio.w = player.audio.r = 0; else resumeAudio(); }
}
function pumpAudio() {
  const a = player.audio;
  for (let guard = 0; guard < 4; guard++) {
    const s = player.m.readAudio();
    if (!s.length) break;
    if (a && !player.muted) for (let i = 0; i < s.length; i++) { a.ring[a.w] = s[i] / 32768; a.w = (a.w + 1) % a.ring.length; }
    if (s.length < 8192) break;
  }
}

// ---- keyboard and gamepad
const KEYS = { ArrowUp: BTN.up, ArrowDown: BTN.down, ArrowLeft: BTN.left, ArrowRight: BTN.right, z: BTN.a, ' ': BTN.a, x: BTN.b, Enter: BTN.mode, m: BTN.mode, Backspace: BTN.sleep, s: BTN.sleep };
function onKey(e, down) {
  if (!player.m || !$('#sheet').classList.contains('hidden')) return;
  if (e.key === 'Escape') { if (down) closePlayer(); return; }
  const bit = KEYS[e.key.length === 1 ? e.key.toLowerCase() : e.key];
  if (!bit) return;
  e.preventDefault();
  if (e.repeat) return;
  setButton(bit, down, 'key' + e.key.toLowerCase());
  resumeAudio();
}
const PAD_BUTTONS = [[BTN.a, [0]], [BTN.b, [1, 2]], [BTN.sleep, [8]], [BTN.mode, [9]], [BTN.up, [12]], [BTN.down, [13]], [BTN.left, [14]], [BTN.right, [15]]];
function pollGamepad() {
  if (!navigator.getGamepads) return;
  const now = {};
  for (const gp of navigator.getGamepads()) {
    if (!gp || !gp.connected) continue;
    for (const [bit, idxs] of PAD_BUTTONS) if (idxs.some((i) => gp.buttons[i] && gp.buttons[i].pressed)) now[bit] = true;
    if (gp.axes[0] < -0.5) now[BTN.left] = true; if (gp.axes[0] > 0.5) now[BTN.right] = true;
    if (gp.axes[1] < -0.5) now[BTN.up] = true; if (gp.axes[1] > 0.5) now[BTN.down] = true;
  }
  for (const [bit] of PAD_BUTTONS) if (!!now[bit] !== !!player.pad[bit]) { setButton(bit, !!now[bit], 'gamepad'); if (now[bit]) resumeAudio(); }
  player.pad = now;
}

async function playerMenu() {
  const rows = [['Direction pad', '<kbd>←</kbd> <kbd>↑</kbd> <kbd>→</kbd> <kbd>↓</kbd> · pad d-pad or stick'], ['A', '<kbd>Z</kbd> <kbd>Space</kbd> · pad A'], ['B', '<kbd>X</kbd> · pad B'],
    ['MODE', '<kbd>Enter</kbd> <kbd>M</kbd> · pad Start'], ['SLEEP', '<kbd>S</kbd> <kbd>Backspace</kbd> · pad Select'], ['Back to the library', '<kbd>Esc</kbd>']]
    .map(([a, b]) => `<tr><td>${a}</td><td>${b}</td></tr>`).join('');
  const box = (id, label, key) => `<label class="field"><input type="checkbox" id="${id}" ${prefs.get(key, true) ? 'checked' : ''}><span>${label}</span></label>`;
  const body = `<p>Tap the VMU's buttons, or use a keyboard or gamepad:</p><table class="keys">${rows}</table>
    <p style="margin-top:14px">Screen</p>
    ${box('fx-slow', 'Slow liquid crystal (dots fade in and out, as on the real screen)', 'lcdSlow')}
    ${box('fx-grid', 'Show the gaps between the dots', 'lcdGrid')}`;
  const v = await sheet(player.meta.game ? player.meta.game.title : player.meta.name, body, [{ label: 'Restart the game', value: 'reset', cls: 'danger' }, { label: 'Close', value: null }], (el) => {
    el.querySelector('#fx-slow').onchange = (e) => prefs.set('lcdSlow', e.target.checked);
    el.querySelector('#fx-grid').onchange = (e) => { prefs.set('lcdGrid', e.target.checked); sizeLcd(); };
  });
  if (v === 'reset' && player.m) { releaseEverything(); player.m.reset(new Date()); await db.del('state:' + player.meta.id); delete saved[player.meta.id]; toast('Restarted. What the game saved to the card is still there.'); }
  player.last = performance.now();
}

// ------------------------------------------------------------------ settings
function siteLinks() {
  const m = location.hostname.match(/^([a-z0-9-]+)\.github\.io$/i);
  const repo = location.pathname.split('/').filter(Boolean)[0];
  return { source: m && repo ? `https://github.com/${m[1]}/${repo}` : '', home: prefs.get('home', '') };
}
// Arrived from a console-picker page on the same site? Remember it so Settings can link back.
(function rememberHome() {
  try {
    const r = document.referrer && new URL(document.referrer);
    const here = new URL('.', location.href).pathname;
    if (r && r.origin === location.origin && !r.pathname.startsWith(here)) prefs.set('home', r.origin + r.pathname);
  } catch { /* no referrer */ }
})();

async function settingsMenu() {
  const { source, home } = siteLinks();
  const hasBios = !!(await db.get('bios')), useBios = hasBios && prefs.get('useBios', false);
  const body = `<p>A Dreamcast Visual Memory Unit in your browser: it runs VMU mini-games from memory card images. Cards, saves and settings are kept in this browser's storage on this device; clearing the browser's site data erases them, so download the cards you care about.</p>
    <p>No BIOS is needed: the handful of system services a game calls are built in. ${useBios ? 'A real BIOS file is in use (experimental).' : ''}</p>
    <p class="about">The emulator was written for this project and checked instruction by instruction against <a href="https://github.com/gyrovorbis/libevmu" target="_blank" rel="noopener">libElysianVMU</a>.${
      source ? ` It is free software (GPL-3.0): <a href="${esc(source)}" target="_blank" rel="noopener">source code</a>.` : ''} Unofficial; not affiliated with SEGA.</p>`;
  const acts = [{ label: 'Import game or card…', value: 'import', cls: 'primary' }, { label: 'New empty card', value: 'new' }, { label: 'Add the built-in test game (Sketch)', value: 'demo' }];
  acts.push(useBios ? { label: 'Stop using the BIOS file', value: 'nobios' } : { label: 'Use a real VMU BIOS file (experimental)…', value: 'bios' });
  if (home) acts.push({ label: '‹ All consoles', value: 'home' });
  acts.push({ label: 'Erase everything on this device', value: 'wipe', cls: 'danger' }, { label: 'Close', value: null });
  const v = await sheet('Settings', body, acts);
  if (v === 'import') document.querySelector('.import-input').click();
  if (v === 'new') { await newCard('Memory card'); renderLibrary(); }
  if (v === 'demo') addDemo();
  if (v === 'home') location.href = home;
  if (v === 'nobios') { prefs.set('useBios', false); for (const k of await db.keys()) if (String(k).startsWith('state:')) await db.del(k); await refreshStore(); renderLibrary(); toast('Back to the built-in system services'); }
  if (v === 'bios') {
    const ok = await sheet('Use a real BIOS?', '<p>With a BIOS dump (a 64 KB file) the VMU starts in its own menu, with the clock and the file viewer, and MODE switches between them and the game. <strong>This path has not been tested</strong>: no BIOS was available while this was written. Games do not need it.</p>',
      [{ label: 'Choose the BIOS file…', value: true, cls: 'primary' }, { label: 'Cancel', value: false }]);
    if (ok) $('#bios-input').click();
  }
  if (v === 'wipe') {
    const ok = await sheet('Erase all local data?', '<p>Removes every memory card, save and snapshot stored in this browser.</p>', [{ label: 'Erase', value: true, cls: 'danger' }, { label: 'Cancel', value: false }]);
    if (ok) { for (const k of await db.keys()) await db.del(k); location.reload(); }
  }
}

// ------------------------------------------------------------------ start
async function boot() {
  try { await loadCore(); } catch (e) { $('#lib-status').textContent = 'Could not load the emulator (' + e.message + ')'; return; }
  await refreshStore();
  renderLibrary();
}

$('#btn-menu').onclick = settingsMenu;
$('#btn-demo').onclick = addDemo;
$('#btn-new-card').onclick = async () => { await newCard('Memory card'); renderLibrary(); };
document.querySelectorAll('.import-input').forEach((inp) => { inp.onchange = async (e) => { const f = [...e.target.files]; e.target.value = ''; if (f.length) await importFiles(f); }; });
$('#bios-input').onchange = async (e) => {
  const f = e.target.files[0]; e.target.value = '';
  if (!f) return;
  let data = new Uint8Array(await f.arrayBuffer());
  if (isZip(data)) { const inner = await unzip(data).catch(() => []); const hit = inner.find((x) => x.data.length === BIOS_SIZE); if (hit) data = hit.data; }
  if (data.length !== BIOS_SIZE) { toast(`That file is ${data.length} bytes; a VMU BIOS is ${BIOS_SIZE}`); return; }
  await db.set('bios', data); prefs.set('useBios', true);
  for (const k of await db.keys()) if (String(k).startsWith('state:')) await db.del(k);
  await refreshStore(); renderLibrary();
  toast('BIOS stored on this device. Open a card to start the VMU with it.', 3600);
};
$('#btn-back').onclick = closePlayer;
$('#btn-pmenu').onclick = playerMenu;
$('#btn-sound').onclick = () => { player.muted = !player.muted; prefs.set('muted', player.muted); applySound(); };
window.addEventListener('keydown', (e) => onKey(e, true));
window.addEventListener('keyup', (e) => onKey(e, false));
window.addEventListener('blur', releaseEverything);
window.addEventListener('resize', () => { if (player.m) sizeLcd(); });
document.addEventListener('visibilitychange', () => {
  if (document.hidden) saveNow();
  else { player.last = performance.now(); if (player.m && !player.over) player.m.setTime(new Date()); }
});
window.addEventListener('pagehide', () => { saveNow(); });
document.addEventListener('dragover', (e) => e.preventDefault());
document.addEventListener('drop', (e) => { e.preventDefault(); if (!player.m && e.dataTransfer && e.dataTransfer.files.length) importFiles([...e.dataTransfer.files]); });

if ('serviceWorker' in navigator && (location.protocol === 'https:' || location.hostname === 'localhost')) {
  navigator.serviceWorker.register('sw.js').catch(() => {});
}
window.webVmu = { player, prefs, db, get cards() { return cards; }, importFiles };   // for tests and the browser console
boot();
