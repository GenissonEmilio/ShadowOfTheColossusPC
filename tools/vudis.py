"""Minimal VU micro-mode disassembler. Usage: vudis.py image.bin [start end]"""
import struct, sys

def dest(i):
    d = (i >> 21) & 0xF
    return '.' + ''.join(c for c, b in zip('xyzw', (8, 4, 2, 1)) if d & b) if d != 0xF else ''

def bc(i): return 'xyzw'[i & 3]

UP = {}
for b, n in ((0, 'ADD'), (4, 'SUB'), (8, 'MADD'), (12, 'MSUB'), (16, 'MAX'), (20, 'MINI'), (24, 'MUL')):
    for k in range(4): UP[b + k] = (n, 'bc')
UP.update({0x1C: ('MULq', 'q'), 0x1D: ('MAXi', 'i'), 0x1E: ('MULi', 'i'), 0x1F: ('MINIi', 'i'),
           0x20: ('ADDq', 'q'), 0x21: ('MADDq', 'q'), 0x22: ('ADDi', 'i'), 0x23: ('MADDi', 'i'),
           0x24: ('SUBq', 'q'), 0x25: ('MSUBq', 'q'), 0x26: ('SUBi', 'i'), 0x27: ('MSUBi', 'i'),
           0x28: ('ADD', 'v'), 0x29: ('MADD', 'v'), 0x2A: ('MUL', 'v'), 0x2B: ('MAX', 'v'),
           0x2C: ('SUB', 'v'), 0x2D: ('MSUB', 'v'), 0x2E: ('OPMSUB', 'v'), 0x2F: ('MINI', 'v')})
US = {}
for b, n in ((0, 'ADDA'), (4, 'SUBA'), (8, 'MADDA'), (12, 'MSUBA'), (24, 'MULA')):
    for k in range(4): US[b + k] = (n, 'bc')
US.update({0x10: ('ITOF0', 't'), 0x11: ('ITOF4', 't'), 0x12: ('ITOF12', 't'), 0x13: ('ITOF15', 't'),
           0x14: ('FTOI0', 't'), 0x15: ('FTOI4', 't'), 0x16: ('FTOI12', 't'), 0x17: ('FTOI15', 't'),
           0x1C: ('MULAq', 'q'), 0x1D: ('ABS', 't'), 0x1E: ('MULAi', 'i'), 0x1F: ('CLIP', 'clip'),
           0x20: ('ADDAq', 'q'), 0x21: ('MADDAq', 'q'), 0x22: ('ADDAi', 'i'), 0x23: ('MADDAi', 'i'),
           0x24: ('SUBAq', 'q'), 0x25: ('MSUBAq', 'q'), 0x26: ('SUBAi', 'i'), 0x27: ('MSUBAi', 'i'),
           0x28: ('ADDA', 'v'), 0x29: ('MADDA', 'v'), 0x2A: ('MULA', 'v'), 0x2C: ('SUBA', 'v'),
           0x2D: ('MSUBA', 'v'), 0x2E: ('OPMULA', 'v'), 0x2F: ('NOP', 'n'), 0x30: ('NOP', 'n')})

def upper(i):
    ft, fs, fd = (i >> 16) & 31, (i >> 11) & 31, (i >> 6) & 31
    op = i & 0x3F
    flags = ''.join(f for f, b in (('I', 31), ('E', 30), ('M', 29), ('D', 28), ('T', 27)) if i >> b & 1)
    flags = f'[{flags}]' if flags else ''
    if op < 0x3C:
        n, k = UP.get(op, ('?up%02x' % op, 'n'))
        if k == 'bc': s = f'{n}{bc(i)}{dest(i)} vf{fd}, vf{fs}, vf{ft}{bc(i)}'
        elif k in 'qi': s = f'{n}{dest(i)} vf{fd}, vf{fs}, {k}'
        else: s = f'{n}{dest(i)} vf{fd}, vf{fs}, vf{ft}'
    else:
        sp = (i & 3) | ((i >> 4) & 0x7C)
        n, k = US.get(sp, ('?us%02x' % sp, 'n'))
        if k == 'bc': s = f'{n}{bc(i)}{dest(i)} ACC, vf{fs}, vf{ft}{bc(i)}'
        elif k in 'qi': s = f'{n}{dest(i)} ACC, vf{fs}, {k}'
        elif k == 'v': s = f'{n}{dest(i)} ACC, vf{fs}, vf{ft}'
        elif k == 't': s = f'{n}{dest(i)} vf{ft}, vf{fs}'
        elif k == 'clip': s = f'CLIPw.xyz vf{fs}, vf{ft}w'
        else: s = 'NOP'
    return s + flags

def imm11(i):
    v = i & 0x7FF
    return v - 0x800 if v & 0x400 else v

def imm15(i):
    v = (i & 0x7FF) | (((i >> 21) & 0xF) << 11)
    return v - 0x8000 if v & 0x4000 else v

LS = {0x30: 'MOVE', 0x31: 'MR32', 0x34: 'LQI', 0x35: 'SQI', 0x36: 'LQD', 0x37: 'SQD', 0x38: 'DIV', 0x39: 'SQRT',
      0x3A: 'RSQRT', 0x3B: 'WAITQ', 0x3C: 'MTIR', 0x3D: 'MFIR', 0x3E: 'ILWR', 0x3F: 'ISWR', 0x40: 'RNEXT', 0x41: 'RGET',
      0x42: 'RINIT', 0x43: 'RXOR', 0x64: 'MFP', 0x68: 'XTOP', 0x69: 'XITOP', 0x6C: 'XGKICK', 0x70: 'ESADD',
      0x71: 'ERSADD', 0x72: 'ELENG', 0x73: 'ERLENG', 0x74: 'EATANxy', 0x75: 'EATANxz', 0x76: 'ESUM', 0x78: 'ESQRT',
      0x79: 'ERSQRT', 0x7A: 'ERCPR', 0x7B: 'WAITP', 0x7C: 'ESIN', 0x7D: 'EATAN', 0x7E: 'EEXP'}

def lower(i, pc):
    if i in (0, 0x8000033C): return 'NOP'
    op = i >> 25
    it, is_, id_ = (i >> 16) & 15, (i >> 11) & 15, (i >> 6) & 15
    ft, fs = (i >> 16) & 31, (i >> 11) & 31
    fsf, ftf = 'xyzw'[(i >> 21) & 3], 'xyzw'[(i >> 23) & 3]
    tgt = lambda: '0x%04x' % ((pc + 8 + imm11(i) * 8) & 0x3FFF)
    if op == 0x40:
        f = i & 0x3F
        if f < 0x3C:
            return {0x30: f'IADD vi{id_}, vi{is_}, vi{it}', 0x31: f'ISUB vi{id_}, vi{is_}, vi{it}',
                    0x32: f'IADDI vi{it}, vi{is_}, {((i >> 6) & 31) - 32 if (i >> 6) & 16 else (i >> 6) & 31}',
                    0x34: f'IAND vi{id_}, vi{is_}, vi{it}', 0x35: f'IOR vi{id_}, vi{is_}, vi{it}',
                    0x38: 'VCALLMS', 0x39: 'VCALLMSR'}.get(f, '?lo40.%02x' % f)
        sp = (i & 3) | ((i >> 4) & 0x7C)
        n = LS.get(sp, '?ls%02x' % sp)
        if n in ('MOVE', 'MR32'): return f'{n}{dest(i)} vf{ft}, vf{fs}'
        if n in ('LQI', 'LQD'): return f'{n}{dest(i)} vf{ft}, (vi{is_}{"++" if n == "LQI" else "--"})'
        if n in ('SQI', 'SQD'): return f'{n}{dest(i)} vf{fs}, (vi{it}{"++" if n == "SQI" else "--"})'
        if n in ('DIV', 'RSQRT'): return f'{n} Q, vf{fs}{fsf}, vf{ft}{ftf}'
        if n == 'SQRT': return f'SQRT Q, vf{ft}{ftf}'
        if n == 'MTIR': return f'MTIR vi{it}, vf{fs}{fsf}'
        if n == 'MFIR': return f'MFIR{dest(i)} vf{ft}, vi{is_}'
        if n in ('ILWR', 'ISWR'): return f'{n}{dest(i)} vi{it}, (vi{is_})'
        if n == 'XGKICK': return f'XGKICK vi{is_}'
        if n in ('XTOP', 'XITOP'): return f'{n} vi{it}'
        if n == 'MFP': return f'MFP{dest(i)} vf{ft}, P'
        if n.startswith('E'): return f'{n} P, vf{fs}{fsf}'
        return n
    m = {0x00: lambda: f'LQ{dest(i)} vf{ft}, {imm11(i)}(vi{is_})', 0x01: lambda: f'SQ{dest(i)} vf{fs}, {imm11(i)}(vi{it})',
         0x04: lambda: f'ILW{dest(i)} vi{it}, {imm11(i)}(vi{is_})', 0x05: lambda: f'ISW{dest(i)} vi{it}, {imm11(i)}(vi{is_})',
         0x08: lambda: f'IADDIU vi{it}, vi{is_}, {imm15(i) & 0x7FFF}', 0x09: lambda: f'ISUBIU vi{it}, vi{is_}, {imm15(i) & 0x7FFF}',
         0x10: lambda: f'FCEQ vi1, 0x{i & 0xFFFFFF:x}', 0x11: lambda: f'FCSET 0x{i & 0xFFFFFF:x}',
         0x12: lambda: f'FCAND vi1, 0x{i & 0xFFFFFF:x}', 0x13: lambda: f'FCOR vi1, 0x{i & 0xFFFFFF:x}',
         0x14: lambda: f'FSEQ vi{it}, 0x{(((i >> 21) & 1) << 11) | (i & 0x7FF):x}',
         0x15: lambda: f'FSSET 0x{(((i >> 21) & 1) << 11) | (i & 0x7FF):x}',
         0x16: lambda: f'FSAND vi{it}, 0x{(((i >> 21) & 1) << 11) | (i & 0x7FF):x}',
         0x17: lambda: f'FSOR vi{it}, 0x{(((i >> 21) & 1) << 11) | (i & 0x7FF):x}',
         0x18: lambda: f'FMEQ vi{it}, vi{is_}', 0x1A: lambda: f'FMAND vi{it}, vi{is_}', 0x1B: lambda: f'FMOR vi{it}, vi{is_}',
         0x1C: lambda: f'FCGET vi{it}', 0x20: lambda: f'B {tgt()}', 0x21: lambda: f'BAL vi{it}, {tgt()}',
         0x24: lambda: f'JR vi{is_}', 0x25: lambda: f'JALR vi{it}, vi{is_}',
         0x28: lambda: f'IBEQ vi{it}, vi{is_}, {tgt()}', 0x29: lambda: f'IBNE vi{it}, vi{is_}, {tgt()}',
         0x2C: lambda: f'IBLTZ vi{is_}, {tgt()}', 0x2D: lambda: f'IBGTZ vi{is_}, {tgt()}',
         0x2E: lambda: f'IBLEZ vi{is_}, {tgt()}', 0x2F: lambda: f'IBGEZ vi{is_}, {tgt()}'}
    return m[op]() if op in m else '?lo%02x' % op

def main():
    d = open(sys.argv[1], 'rb').read()
    a = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    b = int(sys.argv[3], 0) if len(sys.argv) > 3 else len(d)
    for pc in range(a, b, 8):
        lo, up = struct.unpack_from('<II', d, pc)
        if up & 0x80000000:
            low = 'LOI %g' % struct.unpack('<f', struct.pack('<I', lo))[0]
        else:
            low = lower(lo, pc)
        print(f'{pc:04x}: {upper(up):42s} {low}')

if __name__ == '__main__':
    main()
