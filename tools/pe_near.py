# Locate which exported function contains <RVA> in a PE file.
# Usage: python pe_near.py <dll-path> <hex-rva> [hex-rva2 ...]
import struct, sys

fn = sys.argv[1] if len(sys.argv) > 1 else r'C:\Windows\System32\ucrtbase.dll'
RVAS = [int(x, 16) for x in (sys.argv[2:] or ['a527e'])]

d = open(fn, 'rb').read()
e_lfanew = struct.unpack_from('<I', d, 0x3C)[0]
assert d[e_lfanew:e_lfanew+4] == b'PE\0\0'
coff = e_lfanew + 4
num_sec = struct.unpack_from('<H', d, coff+2)[0]
size_opt = struct.unpack_from('<H', d, coff+16)[0]
opt = coff + 20
magic = struct.unpack_from('<H', d, opt)[0]
dd_off = opt + (112 if magic == 0x20B else 96)
exp_rva, exp_size = struct.unpack_from('<II', d, dd_off)
sec_off = opt + size_opt
secs = []
for i in range(num_sec):
    so = sec_off + i*40
    name = d[so:so+8].rstrip(b'\0').decode(errors='replace')
    va_size, va, raw_size, raw = struct.unpack_from('<IIII', d, so+8)
    secs.append((name, va, va_size, raw, raw_size))

def rva_to_off(rva):
    for name, va, vs, raw, rs in secs:
        if va <= rva < va + max(vs, rs):
            return raw + (rva - va)
    return None

imgbase = struct.unpack_from('<Q', d, opt+24)[0] if magic == 0x20B else struct.unpack_from('<I', d, opt+28)[0]

exps = []  # (rva, name)
def parse_dir(eo):
    ob, nfuncs, nnames = struct.unpack_from('<III', d, eo+12)[0:3]
    af, an, ao = struct.unpack_from('<III', d, eo+24)[0:3]
    if not (ob in (0, 1) and 0x40 < nfuncs < 0x20000 and 0 < nnames <= nfuncs):
        return None
    def norm(x):   # some linkers store export subtables as VAs
        return x - imgbase if x >= imgbase else x
    fo, no_, oo = rva_to_off(norm(af)), rva_to_off(norm(an)), rva_to_off(norm(ao))
    if not (fo and no_ and oo):
        return None
    out = []
    for i in range(nnames):
        name_off = rva_to_off(struct.unpack_from('<I', d, no_+i*4)[0])
        if not name_off:
            continue
        nm = d[name_off:d.index(b'\0', name_off)].decode(errors='replace')
        ordn = struct.unpack_from('<H', d, oo+i*2)[0]
        frva = norm(struct.unpack_from('<I', d, fo+ordn*4)[0])
        if exp_rva <= frva < exp_rva+exp_size:
            offo = rva_to_off(frva)
            if offo:
                nm = '->' + d[offo:d.index(b'\0', offo)].decode(errors='replace')
        out.append((frva, nm))
    print(f'  dir at {nfuncs} funcs, {nnames} names')
    return out

exps = None
if exp_rva:
    eo = rva_to_off(exp_rva)
    if eo:
        exps = parse_dir(eo)
        if exps is None:
            exps = []
    if not exps:  # data-directory slot bogus (ucrtbase): scan for OrdinalBase=1 signature
        for (_, _, _, raw, rs) in sorted(secs, key=lambda s: s[3]):
            for off in range(raw, raw+rs-48):
                ob, nf, nn, af, an, ao = struct.unpack_from('<6I', d, off+12)
                if ob in (0, 1) and 0x40 < nf < 0x20000 and 0 < nn <= nf and \
                   all(rva_to_off(x) for x in (af, an, ao)):
                    got = parse_dir(off-12)
                    if got:
                        exps = got
                        print(f'  (fallback: real dir at raw {off-12:#x})')
                        break
            if exps:
                break
if exps:
    print(f'{fn}: {len(exps)} exports, base={imgbase:#x}')

# also collect section starts for context
for RVA in RVAS:
    hits = [(RVA - r, r, n) for (r, n) in exps if r <= RVA and not n.startswith('->')]
    hits.sort()
    print(f'RVA 0x{RVA:x}:')
    for delta, r, n in hits[:4]:
        print(f'  {n}  (RVA 0x{r:08x}, +0x{delta:x})')
