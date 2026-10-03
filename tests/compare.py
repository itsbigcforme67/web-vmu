#!/usr/bin/env python3
# web-vmu tests - check the C++ core against libElysianVMU, instruction for instruction.
# Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
#
#   bash tests/build_ref.sh                      (once: builds tests/ref_trace from libevmu)
#   python3 tests/compare.py game.vms [more.vms ...] [--cycles 2000000] [--seeds 1,2,3] [--every 1000]
#
# For each game and each seed: makes a memory card holding the game, makes a random button script,
# runs both emulators with it and compares, every N instructions, the program flow (every program
# counter and cycle count so far) and the whole machine state (registers, timers, both RAM banks,
# display RAM). Then repeats the run with a save/load round trip at every checkpoint, and with the
# built web/core/vmu.wasm in Node; neither may change anything.
import sys, os, subprocess, random, argparse, tempfile, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
CORE = [os.path.join(HERE, '..', 'core', f) for f in ('vmu.cpp', 'vmufs.cpp')]

def build(tmp):
    cxx = os.environ.get('CXX', 'g++')
    host = os.path.join(tmp, 'host_trace'); card = os.path.join(tmp, 'vmucard')
    subprocess.check_call([cxx, '-std=c++11', '-O2', '-Wall', '-Wextra', '-o', host, os.path.join(HERE, 'host_trace.cpp')] + CORE)
    subprocess.check_call([cxx, '-std=c++11', '-O2', '-Wall', '-Wextra', '-o', card, os.path.join(HERE, '..', 'tools', 'vmucard.cpp')] + CORE)
    return host, card

def make_script(seed, cycles, path):
    """Button events as 'cycle mask' lines. Seed 0 presses nothing. MODE and SLEEP (which end most games) only appear late and rarely."""
    rnd = random.Random(seed)
    lines = []; t = 0; mask = 0
    if seed:
        while t < cycles:
            t += rnd.choice([40, 300, 1500, 6000, 20000])
            pool = [1, 2, 4, 8, 16, 32] * 6 + ([64, 128] if t > cycles * 0.8 and seed % 3 == 0 else [])
            b = rnd.choice(pool)
            mask ^= b if rnd.random() < 0.7 else 0
            if rnd.random() < 0.3: mask = 0
            lines.append('%d %d' % (t, mask))
    with open(path, 'w') as f: f.write('\n'.join(lines) + '\n')
    return len(lines)

SFRS = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0E, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x18, 0x1A, 0x1B, 0x1C, 0x1D,
        0x20, 0x22, 0x25, 0x27, 0x44, 0x45, 0x46, 0x4C, 0x4D, 0x4E, 0x54, 0x5D, 0x5E, 0x5F, 0x63, 0x64, 0x65, 0x66, 0x7F]

def make_fuzz(seed, path):
    """A random but well-formed program: every kind of instruction, with extra pokes at the hardware registers."""
    rnd = random.Random(seed)
    out = bytearray()
    def d9(): return rnd.choice(SFRS) | 0x100 if rnd.random() < 0.45 else rnd.randrange(0x200)
    def one():
        r = rnd.random()
        if r < 0.22:                                   # MOV #i8,sfr
            return [0x23, rnd.choice(SFRS), rnd.choice([0, 1, 2, 0x40, 0x41, 0x45, 0x80, 0xC0, 0xC5, 0xE5, 0xFF, rnd.randrange(256)])]
        if r < 0.30:                                   # bit instruction on a register
            a = d9(); op = rnd.choice([0x48, 0x68, 0x88, 0xA8, 0xC8, 0xE8]) | (0x10 if a & 0x100 else 0) | rnd.randrange(8)
            return [op, a & 0xFF] + ([rnd.randrange(2, 12)] if op < 0xA0 else [])
        if r < 0.32: return [rnd.choice([0x30, 0x40])]                    # MUL / DIV
        if r < 0.36: return [rnd.choice([0x08, 0x28, 0x08, 0x28, 0x18, 0x38]) | rnd.randrange(8), rnd.randrange(256)]   # CALL / JMP within the 4 KiB page
        if r < 0.38: return [rnd.choice([0xA0, 0xB0])]                    # RET / RETI
        if r < 0.40: return [rnd.choice([0x50, 0xC1, 0x50, 0xC1, 0x51])]        # LDF / LDC / STF
        if r < 0.415:                                  # far jumps and calls into the program
            t = rnd.randrange(0x60, 5900); op = rnd.choice([0x20, 0x21, 0x10, 0x11])
            if op in (0x20, 0x21): return [op, t >> 8, t & 0xFF]
            rel = (t - (len(out) + 3) + 1) & 0xFFFF
            return [op, rel & 0xFF, rel >> 8]
        if r < 0.43:                                   # firmware call: point at a flash page of the game, then NOT1 EXT,0 + JMPF entry
            # (not the clock tick at 0x130: random programs scribble over the date, and how a nonsense date rolls over is not worth matching)
            entry = rnd.choice([0x100, 0x100, 0x108, 0x110, 0x120, 0x120] + ([0x1F0, 0x140] if rnd.random() < 0.02 else []))
            return [0x22, 0x7D, rnd.choice([0, 0, 0, 1]), 0x22, 0x7E, rnd.randrange(0x1C), 0x22, 0x7F, rnd.choice([0, 0x80, rnd.randrange(256)]), 0xB8, 0x0D, 0x21, entry >> 8, entry & 0xFF]
        if r < 0.44: return [0x23, 0x01, rnd.choice([0x02, 0x02, 0x00, 0x0A, 0x12])]     # choose the RAM bank
        op = rnd.randrange(256)
        lo = op & 15; hi = op >> 4
        if lo >= 8 or op in (0x10, 0x11, 0x20, 0x21, 0x51): return one()  # far jumps and flash writes: leave to chance elsewhere
        if lo in (2, 3): a = d9(); op = (op & 0xFE) | (a >> 8); body = [a & 0xFF]
        elif lo >= 4: body = []
        elif lo == 1: body = [] if op in (0xC1,) else [rnd.randrange(256)]
        else: body = [rnd.randrange(256)] if hi in (6, 7, 8, 9) else []
        if hi in (3, 4) and lo >= 1: body += ([rnd.randrange(256)] if lo >= 4 else []) + [rnd.randrange(2, 10)]
        if hi == 5 and lo >= 2: body += [rnd.randrange(2, 10)]
        if hi == 2 and lo >= 2: body += [rnd.randrange(256)]
        if hi in (0, 8, 9) and lo in (0, 1) and op not in (0x00,): body = [rnd.randrange(2, 12)] if op in (0x01, 0x80, 0x90) else body
        if hi in (6, 7) and lo == 1: body = [rnd.choice(SFRS)]
        return [op] + body
    # vectors: every interrupt entry holds a short routine that returns
    while len(out) < 0x60: out += bytes(one()) if rnd.random() < 0.5 else bytes([0xB0])
    while len(out) < 6000: out += bytes(one())
    out += bytes([0x28, 0x60])                         # and round again
    out = out[:0x200] + bytearray(b'FUZZ PROGRAM    ' + b' ' * 32) + out[0x230:]
    with open(path, 'wb') as f: f.write(bytes(out))

def lines_of(out): return [l for l in out.split('\n') if l[:2] in ('C ', 'X ', 'F ')]

def diff(ref, got):
    for i in range(max(len(ref), len(got))):
        a = ref[i] if i < len(ref) else '<end>'; b = got[i] if i < len(got) else '<end>'
        if a != b: return 'checkpoint %d\n   libevmu:  %s\n   this core: %s' % (i + 1, a, b)
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('games', nargs='*'); ap.add_argument('--cycles', type=int, default=2000000)
    ap.add_argument('--fuzz', type=int, default=0, help='also run this many random programs, comparing after every instruction')
    ap.add_argument('--seeds', default='0,1,2,3'); ap.add_argument('--every', type=int, default=1000)
    a = ap.parse_args()
    ref = os.path.join(HERE, 'ref_trace')
    if not os.path.exists(ref): sys.exit('tests/ref_trace is missing: run  bash tests/build_ref.sh  first')
    wasm = os.path.join(HERE, '..', 'web', 'core', 'vmu.wasm')
    node = shutil.which('node') if os.path.exists(wasm) else None
    if not node: print('(node or web/core/vmu.wasm not found: skipping the WebAssembly check)')
    failed = 0; ran = 0
    with tempfile.TemporaryDirectory() as tmp:
        host, card = build(tmp)
        env = dict(os.environ, VMUCARD_FIXED_DATE='1')
        jobs = [(g, [int(x) for x in a.seeds.split(',')], a.cycles, a.every, False) for g in a.games]
        for n in range(a.fuzz):
            fz = os.path.join(tmp, 'fuzz%d.vms' % n)
            make_fuzz(1000 + n, fz)
            jobs.append((fz, [n % 7], 60000, 1, True))
        fuzz_ok = 0
        for game, seeds, cycles_, every_, quiet in jobs:
            img = os.path.join(tmp, 'card.bin')
            subprocess.check_call([card, 'new', img, '--game', game], env=env)
            for seed in seeds:
                sf = os.path.join(tmp, 'script.txt')
                nev = make_script(seed, cycles_, sf)
                args = [img, sf, str(cycles_), str(every_)]
                r = lines_of(subprocess.run([ref] + args, capture_output=True, text=True).stdout)
                m = subprocess.run([host] + args, capture_output=True, text=True)
                s = subprocess.run([host] + args + ['--state-test'], capture_output=True, text=True)
                problem = diff(r, lines_of(m.stdout)) if m.returncode == 0 else 'run failed: ' + m.stderr
                if problem is None: problem = (diff(r, lines_of(s.stdout)) and 'with save/load round trips: ' + diff(r, lines_of(s.stdout))) if s.returncode == 0 else 'state test failed: ' + s.stderr
                if problem is None and node:
                    w = subprocess.run([node, os.path.join(HERE, 'wasm_trace.mjs')] + args, capture_output=True, text=True)
                    problem = (diff(r, lines_of(w.stdout)) and 'vmu.wasm: ' + diff(r, lines_of(w.stdout))) if w.returncode == 0 else 'WebAssembly run failed: ' + w.stderr[-300:]
                ran += 1
                name = '%s seed %d' % (os.path.basename(game), seed)
                ended = ' (game exited at %s)' % r[-1].split()[2] if r and r[-1].startswith('X') else ''
                if problem: failed += 1; print('%-28s FAIL %s' % (name, problem))
                elif quiet: fuzz_ok += 1
                else: print('%-28s OK   %d checkpoints, %d button events%s%s' % (name, len(r), nev, ended, ', save/load and vmu.wasm identical' if node else ', save/load identical'))
        if a.fuzz: print('%d of %d random programs identical after every instruction' % (fuzz_ok, a.fuzz))
    print('%d run(s) compared, %d failed' % (ran, failed))
    sys.exit(1 if failed else 0)

if __name__ == "__main__": main()
