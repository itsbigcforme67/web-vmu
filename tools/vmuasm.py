#!/usr/bin/env python3
# web-vmu tools - a small assembler for the VMU's processor (Sanyo LC86K87), enough to write test games.
# Copyright (C) 2026 the web-vmu authors. GPL-3.0-or-later; see LICENSE. No warranty.
#
#   python3 tools/vmuasm.py demo/sketch.s -o web/demo/sketch.vms [--header esp32/src/demo_game.h]
#
# Syntax (close to the usual VMU assemblers):
#   label:            .local:  (local to the last ordinary label)        ; comment
#   name = value      numbers: 123  $7f  %1010     <label >label = low / high byte
#   .org $200         .byte 1,2,$ff      .text 16 "padded with spaces"
#   .icon "0123...f"  one row of a 32-pixel-wide icon, one hex digit (palette entry) per pixel
#   .pad $800         fill with zeros up to this address (a game file is a whole number of 512-byte blocks)
#   operands: #value   @R0..@R3   address or register name   (bit instructions: address, bit[, target])
import sys, re, argparse

SFR = dict(acc=0x100, psw=0x101, b=0x102, c=0x103, trl=0x104, trh=0x105, sp=0x106, pcon=0x107, ie=0x108, ip=0x109, ext=0x10D, ocr=0x10E,
           t0cnt=0x110, t0prr=0x111, t0l=0x112, t0lr=0x113, t0h=0x114, t0hr=0x115, t1cnt=0x118, t1lc=0x11A, t1l=0x11B, t1lr=0x11B, t1hc=0x11C,
           t1h=0x11D, t1hr=0x11D, mcr=0x120, stad=0x122, cnr=0x123, tdr=0x124, xbnk=0x125, vccr=0x127, scon0=0x130, sbuf0=0x131, sbr=0x132,
           scon1=0x134, sbuf1=0x135, p1=0x144, p1ddr=0x145, p1fcr=0x146, p3=0x14C, p3ddr=0x14D, p3int=0x14E, fpr=0x154, p7=0x15C,
           i01cr=0x15D, i23cr=0x15E, isl=0x15F, vsel=0x163, vrmad1=0x164, vrmad2=0x165, vtrbf=0x166, btcr=0x17F)
PLAIN = dict(nop=0x00, ret=0xA0, reti=0xB0, ror=0xC0, rorc=0xD0, rol=0xE0, rolc=0xF0, mul=0x30, div=0x40, ldf=0x50, stf=0x51, ldc=0xC1)
DATA = dict(ld=0x0, st=0x1, inc=0x6, dec=0x7, add=0x8, addc=0x9, sub=0xA, subc=0xB, xch=0xC, **{'or': 0xD, 'and': 0xE, 'xor': 0xF})
IMMEDIATE_OK = ('add', 'addc', 'sub', 'subc', 'or', 'and', 'xor')
BITS = dict(bpc=0x48, bp=0x68, bn=0x88, not1=0xA8, clr1=0xC8, set1=0xE8)

class AsmError(Exception): pass

def split_operands(s):
    out, cur, q = [], '', False
    for ch in s:
        if ch == '"': q = not q
        if ch == ',' and not q: out.append(cur.strip()); cur = ''
        else: cur += ch
    if cur.strip(): out.append(cur.strip())
    return out

def assemble(text):
    lines = []
    for n, raw in enumerate(text.split('\n'), 1):
        code, q = '', False
        for ch in raw:
            if ch == '"': q = not q
            if ch == ';' and not q: break
            code += ch
        if code.strip(): lines.append((n, code.strip()))

    syms = {k: v for k, v in SFR.items()}
    out = bytearray()

    def run(final):
        nonlocal out
        out = bytearray(); pc = 0; scope = ''; seen = set()
        def define(name, v, n):
            if name in seen or name in SFR: raise AsmError('line %d: the name "%s" is already used (names ignore upper and lower case)' % (n, name))
            seen.add(name); syms[name] = v
        def value(tok, n):
            tok = tok.strip()
            if tok[:1] in '<>': v = value(tok[1:], n); return (v & 0xFF) if tok[0] == '<' else (v >> 8) & 0xFF
            if re.fullmatch(r'\$[0-9a-fA-F]+', tok): return int(tok[1:], 16)
            if re.fullmatch(r'%[01]+', tok): return int(tok[1:], 2)
            if re.fullmatch(r'\d+', tok): return int(tok)
            m = re.fullmatch(r'(.+?)\s*([+-])\s*(\$?[0-9a-fA-F]+)', tok)
            if m and not re.fullmatch(r'[\w.]+', tok): return value(m.group(1), n) + (1 if m.group(2) == '+' else -1) * value(m.group(3), n)
            key = (scope + tok) if tok.startswith('.') else tok.lower()
            if key in syms: return syms[key]
            if final: raise AsmError('line %d: unknown name "%s"' % (n, tok))
            return 0
        def emit(*bs):
            nonlocal pc
            for b in bs:
                if final and not 0 <= b <= 255: raise AsmError('value out of range: %r' % (b,))
                if pc >= len(out): out.extend(b'\0' * (pc + 1 - len(out)))
                out[pc] = b & 0xFF; pc += 1
        def rel8(target, n, length):
            r = target - (pc + length)
            if final and not -128 <= r <= 127: raise AsmError('line %d: branch target is too far away (%d bytes)' % (n, r))
            return r & 0xFF
        for n, code in lines:
            m = re.match(r'^([.\w]+):\s*(.*)$', code)
            if m:
                name = m.group(1)
                if name.startswith('.'): define(scope + name, pc, n)
                else: scope = name.lower(); define(scope, pc, n)
                code = m.group(2)
                if not code: continue
            m = re.match(r'^(\w+)\s*=\s*(.+)$', code)
            if m: define(m.group(1).lower(), value(m.group(2), n), n); continue
            parts = code.split(None, 1)
            op = parts[0].lower(); args = split_operands(parts[1]) if len(parts) > 1 else []
            try:
                if op == '.org': pc = value(args[0], n)
                elif op == '.pad':
                    end = value(args[0], n)
                    if len(out) < end: out.extend(b'\0' * (end - len(out)))
                    pc = end
                elif op == '.byte': emit(*[value(a, n) for a in args])
                elif op == '.text':
                    m = re.match(r'^(\S+?)\s*,?\s*"(.*)"$', parts[1].strip())
                    if not m: raise AsmError('line %d: write  .text <width> "characters"' % n)
                    width = value(m.group(1), n); s = m.group(2)
                    if len(s) > width: raise AsmError('line %d: text is longer than %d characters' % (n, width))
                    emit(*s.ljust(width).encode('ascii'))
                elif op == '.icon':
                    s = args[0].strip('"')
                    if len(s) != 32: raise AsmError('line %d: an icon row is 32 hex digits' % n)
                    emit(*[int(s[i:i + 2], 16) for i in range(0, 32, 2)])
                elif op in PLAIN and not args: emit(PLAIN[op])
                elif op in ('br', 'bz', 'bnz'): emit({'br': 0x01, 'bz': 0x80, 'bnz': 0x90}[op], rel8(value(args[0], n), n, 2))
                elif op in ('brf', 'callr'):
                    r = (value(args[0], n) - pc - 2) & 0xFFFF
                    emit(0x11 if op == 'brf' else 0x10, r & 0xFF, r >> 8)
                elif op in ('jmpf', 'callf'): a = value(args[0], n); emit(0x21 if op == 'jmpf' else 0x20, (a >> 8) & 0xFF, a & 0xFF)
                elif op in ('jmp', 'call'):
                    a = value(args[0], n)
                    if final and (a & 0xF000) != ((pc + 2) & 0xF000): raise AsmError('line %d: %s target is outside this 4 KiB page (use %sf)' % (n, op, op))
                    emit((0x28 if op == 'jmp' else 0x08) | ((a >> 8) & 7) | (((a >> 11) & 1) << 4), a & 0xFF)
                elif op in ('push', 'pop'): a = value(args[0], n); emit((0x60 if op == 'push' else 0x70) | ((a >> 8) & 1), a & 0xFF)
                elif op in DATA:
                    a = args[0]; hi = DATA[op] << 4
                    if a.startswith('#'):
                        if op not in IMMEDIATE_OK: raise AsmError('line %d: %s cannot take a constant (use mov)' % (n, op))
                        emit(hi | 1, value(a[1:], n) & 0xFF)
                    elif a.upper() in ('@R0', '@R1', '@R2', '@R3'): emit(hi | 4 | int(a[2]))
                    else: v = value(a, n); emit(hi | 2 | ((v >> 8) & 1), v & 0xFF)
                elif op == 'mov':
                    if not args[0].startswith('#'): raise AsmError('line %d: mov needs a constant first: mov #value,where' % n)
                    i = value(args[0][1:], n) & 0xFF
                    if args[1].upper() in ('@R0', '@R1', '@R2', '@R3'): emit(0x24 | int(args[1][2]), i)
                    else: v = value(args[1], n); emit(0x22 | ((v >> 8) & 1), v & 0xFF, i)
                elif op in ('be', 'bne'):
                    hi = 0x30 if op == 'be' else 0x40
                    if args[0].startswith('#'): emit(hi | 1, value(args[0][1:], n) & 0xFF, rel8(value(args[1], n), n, 3))
                    elif args[0].upper() in ('@R0', '@R1', '@R2', '@R3'): emit(hi | 4 | int(args[0][2]), value(args[1][1:], n) & 0xFF, rel8(value(args[2], n), n, 3))
                    else: v = value(args[0], n); emit(hi | 2 | ((v >> 8) & 1), v & 0xFF, rel8(value(args[1], n), n, 3))
                elif op == 'dbnz':
                    if args[0].upper() in ('@R0', '@R1', '@R2', '@R3'): emit(0x54 | int(args[0][2]), rel8(value(args[1], n), n, 2))
                    else: v = value(args[0], n); emit(0x52 | ((v >> 8) & 1), v & 0xFF, rel8(value(args[1], n), n, 3))
                elif op in BITS:
                    v = value(args[0], n); bit = value(args[1], n)
                    if final and not 0 <= bit <= 7: raise AsmError('line %d: bit number must be 0-7' % n)
                    code0 = BITS[op] | (((v >> 8) & 1) << 4) | (bit & 7)
                    if op in ('bpc', 'bp', 'bn'): emit(code0, v & 0xFF, rel8(value(args[2], n), n, 3))
                    else: emit(code0, v & 0xFF)
                else: raise AsmError('line %d: unknown instruction "%s"' % (n, op))
            except IndexError:
                raise AsmError('line %d: missing operand in "%s"' % (n, code))
    run(False); run(False); run(True)
    return bytes(out), syms

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source'); ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--header', help='also write the program as a C array (for the ESP32 firmware)')
    ap.add_argument('--name', default='DEMO_GAME')
    a = ap.parse_args()
    try: data, _ = assemble(open(a.source).read())
    except AsmError as e: sys.exit(str(e))
    if len(data) % 512: data += b'\0' * (512 - len(data) % 512)
    open(a.output, 'wb').write(data)
    if a.header:
        rows = [', '.join('0x%02x' % b for b in data[i:i + 20]) for i in range(0, len(data), 20)]
        open(a.header, 'w').write('// Built by tools/vmuasm.py from %s - do not edit.\n#pragma once\n#include <stdint.h>\nstatic const uint8_t %s[%d] = {\n  %s\n};\n'
                                  % (a.source, a.name, len(data), ',\n  '.join(rows)))
    print('%s: %d bytes (%d blocks)' % (a.output, len(data), len(data) // 512))

if __name__ == '__main__': main()
