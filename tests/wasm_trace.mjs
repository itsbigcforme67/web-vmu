// web-vmu tests - the same trace as host_trace.cpp, produced by the built web/core/vmu.wasm in Node.
// Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
//   node tests/wasm_trace.mjs <card.bin> <script> <total cycles> <checkpoint every N steps>
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { Vmu, compileCore } from '../web/core/vmu.js';

const [cardPath, scriptPath, totalArg, everyArg] = process.argv.slice(2);
const total = Number(totalArg), every = Number(everyArg);
const here = dirname(fileURLToPath(import.meta.url));
const m = await Vmu.create(await compileCore(readFileSync(join(here, '..', 'web', 'core', 'vmu.wasm'))));
m.setCard(new Uint8Array(readFileSync(cardPath)));
m.audio(16000);
m.reset(new Date(2026, 0, 2, 3, 4, 5));
const evs = readFileSync(scriptPath, 'utf8').split('\n').map((l) => l.trim().split(/\s+/).map(Number)).filter((e) => e.length === 2 && !Number.isNaN(e[1]));

const hex = (v) => (v >>> 0).toString(16).padStart(8, '0');
const out = [];
let ei = 0, cyc = 0, step = 0, flow = 2166136261;
while (cyc < total) {
  while (ei < evs.length && evs[ei][0] <= cyc) m.buttons(evs[ei++][1]);
  const c = m.step();
  if (m.x.vmu_status() & 1) { out.push(`X ${step + 1} ${cyc + c}`); break; }
  cyc += c; step++;
  flow = Math.imul(flow ^ m.pc, 16777619);
  flow = Math.imul(flow ^ c, 16777619);
  if (step % every === 0) { out.push(`C ${step} ${cyc} ${hex(flow)} ${hex(m.stateHash())}`); m.readAudio(); }
}
out.push(`F ${hex(m.flashHash())}`);
process.stdout.write(out.join('\n') + '\n');
