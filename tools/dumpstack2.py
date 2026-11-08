import sys, io, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

path = r'D:\DSH-Workplace\sender.exe_261107_202414.dmp'
d = open(path, 'rb').read()
print(f'dump {len(d)/1e6:.1f}MB')

# MINIDUMP_HEADER: sig(4) version(4) nstreams(4) dir_off(4@+12?)
assert d[:4] == b'MDMP'
n_streams, dir_off = struct.unpack_from('<II', d, 8)
streams = {}
for i in range(n_streams):
    stype, dsize, rva = struct.unpack_from('<III', d, dir_off + i * 12)
    streams[stype] = (rva, dsize)  # rva is a file offset in this file-backed dump

def rd(addr, n):
    # Memory64ListStream=7: base_rva(8) then n*{va(8),size(8)} descriptors
    rva, size = streams[7]
    cnt = struct.unpack_from('<Q', d, rva)[0]
    desc = rva + 8
    for i in range(cnt):
        va, sz = struct.unpack_from('<QQ', d, desc + i * 16)
        off = struct.unpack_from('<Q', d, desc + cnt * 16 + sum(struct.unpack_from('<Q', d, desc + j*16+8) for j in range(i)))[0] if False else None
    # simpler: precompute cumulative once
    return None

# build memory map: MINIDUMP_MEMORY64_LIST = BaseRva(8) Count(8) then Count * {Va(8) Rva64(8) Size64(8)}
rva, _ = streams[7]
base_rva = struct.unpack_from('<Q', d, rva)[0]
cnt = struct.unpack_from('<Q', d, rva + 8)[0]
desc = rva + 16
regions = []
for i in range(cnt):
    va, rr, sz = struct.unpack_from('<QQQ', d, desc + i * 24)
    regions.append((va, sz, base_rva + rr))
print(f'{cnt} memory regions')

import bisect
starts = [r[0] for r in regions]
def read_mem(addr, n):
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0: return None
    va, sz, off = regions[i]
    if addr + n > va + sz: return None
    return d[off + (addr - va): off + (addr - va) + n]

# modules (stream 4): count(4) then MINIDUMP_MODULE entries
rva, _ = streams[4]
nmod = struct.unpack_from('<I', d, rva)[0]
mods = []
for i in range(nmod):
    e = rva + 4 + i * 108
    base, size = struct.unpack_from('<QQ', d, e)
    name_rva, name_len = struct.unpack_from('<II', d, e + 96)
    nm = d[name_rva: name_rva + name_len * 2].decode('utf-16-le', errors='replace')
    mods.append((base, size, nm.split('\\')[-1]))
mods.sort()
def mod_of(a):
    i = bisect.bisect_right([m[0] for m in mods], a) - 1
    if i >= 0 and a < mods[i][0] + mods[i][1]:
        return f'{mods[i][2]}+0x{a - mods[i][0]:x}'
    return f'{a:#018x}'

# exception (stream 6): threadid(4) align(4) EXC_RECORD: code(4) flags(4) rec(8) addr(8) nparams(4) pad(4) params(8*n) CONTEXT located AFTER record per stream layout: exception record then context location descriptor
rva, _ = streams[6]
tid, = struct.unpack_from('<I', d, rva)
code, flags = struct.unpack_from('<II', d, rva + 8)
addr, = struct.unpack_from('<Q', d, rva + 24)
npar, = struct.unpack_from('<I', d, rva + 32)
par_off = rva + 40
params = struct.unpack_from(f'<{npar}Q', d, par_off)
# MINIDUMP_EXCEPTION_STREAM: tid(4)+pad(4)+exception(152)+context(MINIDUMP_MEMORY_LOCATION 16)
ctx_rva, ctx_sz = struct.unpack_from('<II', d, rva + 8 + 152)
print(f'\n=== EXCEPTION tid={tid} code={code:#x} addr={mod_of(addr)} params={params}')

# parse CONTEXT x64 from ctx_rva: rip at offset 0xF8 in MCDR? CONTEXT64: rip at +0xF8, rsp at +0xC8, rbp at +0xE8
c = d[ctx_rva: ctx_rva + ctx_sz]
rsp = struct.unpack_from('<Q', c, 0xC8)[0]
rbp = struct.unpack_from('<Q', c, 0xE8)[0]
rip = struct.unpack_from('<Q', c, 0xF8)[0]
print(f'  rip={mod_of(rip)} rsp={rsp:#x} rbp={rbp:#x}')

# all threads (stream 3): count(4) then entries 48B: tid(4)susp(4)prio(2)x(2) startaddr(8) teb(8) stack(start,memsize...) stack_start(8) stack_size(8) ctx_rva ctx_sz...
rva, _ = streams[3]
nt = struct.unpack_from('<I', d, rva)[0]
# auto-detect stride: 48 vs 56 (InstructionOffset present in newer dumps)
stride = 48
cr_rva, cr_sz = struct.unpack_from('<II', d, rva + 4 + 40)
if not (0 < cr_sz <= 2000):
    cr2_rva, cr2_sz = struct.unpack_from('<II', d, rva + 4 + 48)
    if 0 < cr2_sz <= 2000: stride = 56
print(f'\n=== THREADS ({nt}, stride={stride}) ===')
print(f'exception ctx loc: {ctx_rva:#x}/{ctx_sz}')
for i in range(nt):
    e = rva + 4 + i * stride
    t, = struct.unpack_from('<I', d, e)
    cx_rva, cx_sz = struct.unpack_from('<II', d, e + (40 if stride == 48 else 48))
    cc = d[cx_rva: cx_rva + cx_sz] if cx_rva else b'\0' * 256
    r = struct.unpack_from('<Q', cc, 0xF8)[0] if len(cc) > 0xF8 else 0
    sp_ = struct.unpack_from('<Q', cc, 0xC8)[0] if len(cc) > 0xC8 else 0
    m = mod_of(r)
    mark = ' <== CRASH' if t == tid else ''
    print(f'  tid={t:#x} rip={m} rsp={sp_:#x}{mark}')
    if t == tid:
        rsp, rip = sp_, r

# scan crash-thread stack for return addresses in interesting modules
print(f'\n=== CRASH STACK SCAN (from rsp={rsp:#x}) ===')
interest = ('sender', 'ucrtbase', 'mf', 'mspro', 'audio', 'ntdll', 'kernel', 'twin', 'combase', 'windows', 'msacm', 'audiocore')
seen = []
sp = rsp
for k in range(4096):
    b = read_mem(sp, 8)
    if not b or len(b) < 8: break
    v, = struct.unpack('<Q', b)
    m = mod_of(v)
    if '+' in m:
        ml = m.lower()
        if any(x in ml for x in interest):
            if not seen or seen[-1] != m:
                seen.append(m)
    sp += 8
for i, m in enumerate(seen[:30]):
    print(f'  #{i}: {m}')
