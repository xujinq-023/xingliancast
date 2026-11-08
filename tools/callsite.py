# Disassemble around a sender.exe RVA: find the call whose return address == target RVA,
# resolve the call target through IAT to an imported function name.
# Usage: python callsite.py <exe> <hex-rva-of-return-address>
import struct, sys

fn = sys.argv[1] if len(sys.argv) > 1 else r'D:\DSH-Workplace\projector\sender-pc\build\sender.exe'
TARGET = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x4137

d = open(fn, 'rb').read()
lf = struct.unpack_from('<I', d, 0x3C)[0]
coff = lf + 4
ns = struct.unpack_from('<H', d, coff + 2)[0]
szopt = struct.unpack_from('<H', d, coff + 16)[0]
opt = coff + 20
magic = struct.unpack_from('<H', d, opt)[0]
dd = opt + (112 if magic == 0x20B else 96)
base = struct.unpack_from('<Q', d, opt + 24)[0]
sec_off = opt + szopt
secs = []
for i in range(ns):
    so = sec_off + i * 40
    va, vsz, raw, rsz = struct.unpack_from('<IIII', d, so + 8)
    secs.append((va, vsz, raw, rsz))

def r2o(r):
    for va, vsz, raw, rsz in secs:
        if va <= r < va + max(vsz, rsz):
            return raw + (r - va)
    return None

# imports: map IAT slot RVA -> name (via import lookup table)
imports = {}
imp_rva, _ = struct.unpack_from('<II', d, dd + 8)
o = r2o(imp_rva) if imp_rva else None
while o:
    thrunk, _td, _fw, nameoff, iat2 = struct.unpack_from('<IIIII', d, o)
    if thrunk == 0 and nameoff == 0 and iat2 == 0:
        break
    dll_off = r2o(nameoff)
    if not dll_off:
        break
    dll = d[dll_off:d.index(b'\0', dll_off)].decode(errors='replace')
    lt = thrunk or iat2
    lo, io_ = r2o(lt), r2o(iat2)
    j = 0
    while lo and io_:
        e, = struct.unpack_from('<Q', d, lo + j * 8)
        if e == 0:
            break
        nm = None
        if e & (1 << 63):
            nm = f'#{e & 0xFFFF}'
        else:
            ho = r2o(e & 0x7FFFFFFF)
            if ho:
                nm = d[ho + 2:d.index(b'\0', ho + 2)].decode(errors='replace')
        if nm:
            imports[io_ + j * 8] = f'{dll}!{nm}'
        j += 1
    o += 20
    if o > len(d) - 20:
        break

to = r2o(TARGET)
print(f'{fn}  target RVA {TARGET:#x}  file {to:#x}  (base {base:#x})')
win = d[to - 24:to + 16]
print('bytes before target:', win[:24].hex(' '))
# scan backwards for E8 rel32 whose next-addr == TARGET
for back in range(1, 16):
    p = to - back - 5
    if d[p] == 0xE8:
        rel, = struct.unpack_from('<i', d, p + 1)
        tgt_rva = TARGET + rel
        slot = r2o(tgt_rva)
        nm = imports.get(slot)
        if nm:
            print(f'  CALL at {TARGET - back - 5:#x} -> IAT {tgt_rva:#x} = {nm}')
        else:
            print(f'  CALL at {TARGET - back - 5:#x} -> RVA {tgt_rva:#x} (direct)  bytes {d[tgt_rva and r2o(tgt_rva) or 0:][:8].hex()}')
# also show any recent RIP-relative call [rip+disp] FF 15
for back in range(1, 8):
    p = to - back - 6
    if d[p] == 0xFF and d[p + 1] == 0x15:
        disp, = struct.unpack_from('<i', d, p + 2)
        slot_rva = TARGET + disp
        slot = r2o(slot_rva)
        nm = imports.get(slot)
        print(f'  FF15 CALL at {p:#x} -> IAT {slot_rva:#x} = {nm}')
