# Raw-stack window dumper for a minidump crash thread.
# Usage: python dumpstack4.py <dmp> [start_hex [len_dec]]
import struct, sys
from minidump.minidumpfile import MinidumpFile

mf = MinidumpFile.parse(sys.argv[1])
reader = mf.get_reader()

def read_mem(virt, size):
    for fn in (lambda: reader.read(virt, size), lambda: reader.get_memory(virt, size),
               lambda: reader.get_memory(virt, size).read()):
        try:
            b = fn()
            if b:
                if hasattr(b, 'read'):
                    b = b.read()
                return bytes(b)
        except Exception:
            continue
    return None

mods = []
try:
    for m in mf.modules.modules:
        mods.append((m.baseaddress, m.baseaddress + m.size, m.name.split('\\')[-1]))
except Exception:
    pass

def mod_of(v):
    for b, e, n in mods:
        if b <= v < e:
            return f'{n}+{v - b:#x}'
    return None

# crash thread = exception record thread, else first
er = mf.exception.exception_records[0]
ctid = er.ThreadId

ctx = None
for t in mf.threads.threads:
    if t.ThreadId == ctid:
        ctx = t.ContextObject
        break
rsp, rbp, rip = ctx.Rsp, ctx.Rbp, ctx.Rip
print(f'crash tid={ctid:#x} rip={mod_of(rip)} rsp={rsp:#x} rbp={rbp:#x}')

start = int(sys.argv[2], 16) if len(sys.argv) > 2 else rsp
n = int(sys.argv[3]) if len(sys.argv) > 3 else 0x280
b = read_mem(start, n)
if not b:
    print('cannot read stack window')
    sys.exit(1)
for i in range(0, len(b) - 7, 8):
    v, = struct.unpack_from('<Q', b, i)
    a = start + i
    m = mod_of(v)
    tag = ''
    if m:
        tag = '  <== ' + m
    print(f'{a:#018x}  {v:016x}{tag}')
