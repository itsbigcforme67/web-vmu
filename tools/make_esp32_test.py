#!/usr/bin/env python3
# web-vmu tools - make the data for the on-chip self-test (esp32/src/coretest.cpp).
# Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
#
#   python3 tools/make_esp32_test.py        (writes esp32/src/coretest_data.h)
#
# The header holds a random test program, a script of button presses and the results the PC build
# of the core gets with them. The firmware runs the same thing and says whether it got the same.
import os, sys, subprocess, tempfile
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.join(HERE, '..')
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import compare

RUNS = [('sketch', 'SKETCH', os.path.join(ROOT, 'web', 'demo', 'sketch.vms'), 400000, 1000), ('random program', 'FUZZ', None, 60000, 100)]

def main():
    with tempfile.TemporaryDirectory() as tmp:
        host, card = compare.build(tmp)
        fuzz = os.path.join(tmp, 'FUZZ.vms'); compare.make_fuzz(1000, fuzz)
        script = os.path.join(tmp, 'script.txt'); compare.make_script(5, 400000, script)
        events = [l.split() for l in open(script) if l.strip()]
        data = bytearray(open(fuzz, 'rb').read()); data += b'\0' * (-len(data) % 512)
        out = ['// Built by tools/make_esp32_test.py - do not edit.', '#pragma once', '#include <stdint.h>',
               'static const uint8_t FUZZ_GAME[%d] = {%s};' % (len(data), ','.join(str(b) for b in data)),
               'static const uint32_t SCRIPT[%d][2] = {%s};' % (len(events), ','.join('{%s,%s}' % (a, b) for a, b in events)),
               '// what the PC build ends with: steps, cycles, program-flow hash, machine-state hash, card hash',
               'struct Expected { const char* label; const char* file; uint32_t total, every, steps, cycles, flow, state, card; };']
        rows = []
        for label, name, path, total, every in RUNS:
            src = os.path.join(tmp, name + '.vms')
            if path: open(src, 'wb').write(open(path, 'rb').read())
            img = os.path.join(tmp, name + '.bin')
            subprocess.check_call([card, 'new', img, '--game', src], env=dict(os.environ, VMUCARD_FIXED_DATE='1'))
            lines = compare.lines_of(subprocess.run([host, img, script, str(total), str(every)], capture_output=True, text=True, check=True).stdout)
            last = [l for l in lines if l.startswith('C ')][-1].split(); card_hash = [l for l in lines if l.startswith('F ')][-1].split()[1]
            rows.append('{"%s", "%s", %d, %d, %s, %s, 0x%s, 0x%s, 0x%s}' % (label, name, total, every, last[1], last[2], last[3], last[4], card_hash))
        out.append('static const Expected EXPECTED[%d] = {%s};' % (len(rows), ', '.join(rows)))
        dst = os.path.join(ROOT, 'esp32', 'src', 'coretest_data.h')
        open(dst, 'w').write('\n'.join(out) + '\n')
        print('wrote', os.path.relpath(dst, ROOT)); print('\n'.join(rows))

if __name__ == '__main__': main()
