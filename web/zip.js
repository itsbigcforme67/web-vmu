// web-vmu - Copyright (C) 2026 the web-vmu authors (from Web Pokémon mini, plus the optional name filter).
// Free software under the GNU General Public License, version 3 or later; see LICENSE. No warranty.
// Minimal ZIP reader (stored + deflate entries) using the browser's DecompressionStream.
// unzip(buf, want) returns [{ name, data: Uint8Array }] for the entries whose name want(name) accepts. Enough for ROM zips and GitHub source zips; no encryption, no zip64.

export function isZip(buf) {
  return buf.length > 22 && buf[0] === 0x50 && buf[1] === 0x4B && buf[2] === 0x03 && buf[3] === 0x04;
}

export async function unzip(buf, want = () => true) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  // find the end-of-central-directory record
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 65557); i--) {
    if (dv.getUint32(i, true) === 0x06054B50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error('not a zip file');
  const count = dv.getUint16(eocd + 10, true);
  let p = dv.getUint32(eocd + 16, true);
  const out = [];
  for (let n = 0; n < count; n++) {
    if (dv.getUint32(p, true) !== 0x02014B50) break;
    const method = dv.getUint16(p + 10, true);
    const csize = dv.getUint32(p + 20, true);
    const nameLen = dv.getUint16(p + 28, true), extraLen = dv.getUint16(p + 30, true), commentLen = dv.getUint16(p + 32, true);
    const local = dv.getUint32(p + 42, true);
    const name = new TextDecoder().decode(buf.subarray(p + 46, p + 46 + nameLen));
    p += 46 + nameLen + extraLen + commentLen;
    if (name.endsWith('/') || !want(name)) continue;
    const lNameLen = dv.getUint16(local + 26, true), lExtraLen = dv.getUint16(local + 28, true);
    const start = local + 30 + lNameLen + lExtraLen;
    const raw = buf.subarray(start, start + csize);
    let data;
    if (method === 0) data = raw.slice();
    else if (method === 8) data = await inflateRaw(raw);
    else continue;
    out.push({ name, data });
  }
  return out;
}

async function inflateRaw(raw) {
  if (typeof DecompressionStream === 'undefined') throw new Error('this browser cannot unzip; extract the file first');
  const stream = new Blob([raw]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}
