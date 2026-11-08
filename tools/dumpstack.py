import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
from minidump.minidumpfile import MinidumpFile

path = r'D:\DSH-Workplace\sender.exe_261107_202414.dmp'
mf = MinidumpFile.parse(path)

mods = mf.modules.modules
def mod_of(addr):
    for m in mods:
        if m.baseaddr <= addr < m.baseaddr + m.size:
            return f'{m.name}+0x{addr - m.baseaddr:x}'
    return f'{addr:#x}'

exc = mf.exception
print('=== EXCEPTION ===')
exc0 = exc
print('  ', exc0)
tid = 0x3f8c

tl = mf.thread_list
crash = None
for t in tl.threads:
    if t.thread_id == tid:
        crash = t
print(f'\n=== {len(tl.threads)} THREADS ===')
for t in tl.threads:
    rip = t.context.rip if hasattr(t.context, 'rip') else 0
    mark = ' <== CRASH' if t is crash else ''
    print(f'  tid={t.thread_id} rip={mod_of(rip)}{mark}')

if crash is not None:
    print('\n=== CRASH THREAD STACK ===')
    try:
        cs = crash.callstacks
        print('callstacks attr:', cs)
    except Exception as e:
        print('callstacks failed:', e)
    # manual: stack memory scan for return addrs inside modules
    import struct
    ctx = crash.context
    rsp, rip = ctx.rsp, ctx.rip
    print(f'  rip = {mod_of(rip)}')
    print(f'  rsp = {rsp:#x} rbp = {ctx.rbp:#x}')
    # read stack memory
    mem = mf.meminfo
    depth = 0
    print('\n  scan stack for code addrs:')
    segs = [m for m in mods]
    addr = rsp
    hits = []
    while depth < 0x4000:
        data = mem.read(addr, 8)
        if data is None or len(data) < 8:
            break
        v = struct.unpack('<Q', data)[0]
        mo = mod_of(v)
        if mo != f'{v:#x}' and ('.exe' in mo or '.dll' in mo or '.pyd' in mo):
            # only exe/dll frames
            low = mo.split('+')[0].lower()
            if any(x in low for x in ('sender', 'ucrtbase', 'mf', 'msaac', 'audio', 'kernel32', 'ntdll', 'windows.media', 'wintypes')):
                hits.append((addr, mo))
        addr += 8
        depth += 8
    for a, mo in hits[:40]:
        print(f'    [{a:#x}] {mo}')
