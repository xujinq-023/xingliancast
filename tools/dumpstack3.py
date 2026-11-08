import sys, io, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
from minidump.minidumpfile import MinidumpFile

mf = MinidumpFile.parse(sys.argv[1] if len(sys.argv) > 1 else r'D:\DSH-Workplace\sender.exe_261107_202414.dmp')
reader = mf.get_reader()

mods = sorted(mf.modules.modules, key=lambda m: m.baseaddress)
def mod_of(a):
    for m in mods:
        if m.baseaddress <= a < m.baseaddress + m.size:
            return f'{m.name}+0x{a - m.baseaddress:x}'
    return None

def read_mem(addr, n):
    for fn in (lambda: reader.read(addr, n), lambda: reader.get_memory(addr, n), lambda: reader.get_memory(addr, n).read()):
        try:
            b = fn()
            if b:
                if hasattr(b, 'read'):
                    b = b.read()
                return bytes(b)
        except Exception:
            continue
    return None

er = mf.exception.exception_records[0].ExceptionRecord
tid = mf.exception.exception_records[0].ThreadId
code = getattr(er, 'ExceptionCode', None)
addr = getattr(er, 'ExceptionAddress', 0)
params = getattr(er, 'ExceptionInformation', None)
print(f'=== EXCEPTION tid={tid} code={code} addr={mod_of(addr)} params={params}')

def ctx_regs(t):
    c = t.ContextObject
    rip = getattr(c, 'Rip', 0) or 0
    rsp = getattr(c, 'Rsp', 0) or 0
    rbp = getattr(c, 'Rbp', 0) or 0
    return rip, rsp, rbp

def ctx_extra(t):
    c = t.ContextObject
    g = lambda n: getattr(c, n, 0) or 0
    return {n: g(n) for n in ('Rax', 'Rbx', 'Rcx', 'Rdx', 'Rsi', 'Rdi', 'R8', 'R9', 'R10', 'R11')}

INTEREST = ('sender', 'ucrtbase', 'mfplat', 'mfcore', 'mf', 'msmpeg', 'audiodg', 'audiocore', 'audioeng',
            'ntdll', 'kernelbase', 'kernel32', 'combase', 'twinapi', 'windows.media', 'mfaudio',
            'msaud', 'ksproxy', 'coremsg', 'rtworkq', 'winmmbase', 'wdmaud', 'swenum', 'mmdevapi')

def scan(rsp, limit=8000, top=16, boundary=None):
    sp = rsp
    seen = []
    for _ in range(limit):
        if boundary and sp >= boundary:
            break
        b = read_mem(sp, 8)
        if not b or len(b) < 8:
            break
        v, = struct.unpack('<Q', b)
        m = mod_of(v)
        if m and any(x in m.lower() for x in INTEREST):
            if not seen or seen[-1] != m:
                seen.append(m)
        sp += 8
    return seen

# collect all thread rsps to bound each scan (stacks are allocated adjacently)
ALL_RSP = sorted((getattr(t.ContextObject, 'Rsp', 0) or 0) for t in mf.threads.threads if getattr(t.ContextObject, 'Rsp', 0))

def next_rsp(rsp):
    for r in ALL_RSP:
        if r > rsp:
            return r
    return None

crash = None
print('\n=== THREADS ===')
for t in mf.threads.threads:
    rip, rsp, rbp = ctx_regs(t)
    m = mod_of(rip) or f'{rip:#x}'
    mark = ''
    if t.ThreadId == tid:
        mark = ' <== CRASH'
        crash = (rip, rsp, rbp)
    print(f'  tid={t.ThreadId:#08x} rsp={rsp:#x} rip={m}{mark}')

if crash:
    rip, rsp, rbp = crash
    print(f'\n=== CRASH THREAD rip={mod_of(rip)} rsp={rsp:#x} rbp={rbp:#x} ===')
    try:
        cr = next(t for t in mf.threads.threads if t.ThreadId == tid)
        ex = ctx_extra(cr)
        print('  regs: ' + ' '.join(f'{k}={v:#x}' + (f'~{mod_of(v)}' if mod_of(v) else '') for k, v in ex.items()))
    except Exception as e:
        print('  regs: ?', e)
    for i, m in enumerate(scan(rsp, boundary=next_rsp(rsp))):
        print(f'  #{i:2d}: {m}')

print('\n=== ALL THREADS first code addrs on stack ===')
for t in mf.threads.threads:
    rip, rsp, rbp = ctx_regs(t)
    if not rsp or t.ThreadId == tid:
        continue
    seen = scan(rsp, limit=4000, top=5, boundary=next_rsp(rsp))
    if seen:
        print(f'  tid={t.ThreadId:#08x}: ' + ' | '.join(seen[:5]))

# ---- caller-context analysis for _purecall chain ----
print('\n=== CALLER CONTEXT ANALYSIS ===')
# disasm around ucrtbase call-site (+0x6e660..+0x6e720)
m_ucrt = next((m for m in mods if m.name.lower().startswith('ucrtbase')), None)
if m_ucrt and crash:
    base = m_ucrt.baseaddress
    lo, hi = base + 0x6e660, base + 0x6e710
    blob = read_mem(lo, hi - lo)
    if blob:
        print(f'--- ucrtbase 0x6e660..0x6e70f (raw; call e8 targets = RVA 0x6e70a+t+1):')
        i = 0
        while i < len(blob):
            if blob[i] == 0xE8 and i + 5 <= len(blob):
                t32, = struct.unpack('<i', blob[i + 1:i + 5])
                nxt = lo + i + 5
                tgt = (base + 0x6e700) + (nxt - (base + 0x6e700)) + t32  # rip-rel from nxt
                print(f'  0x6e{hex(lo + i - base)[2:]}: call -> RVA {tgt - base:#x}')
            i += 1
        print(f'  raw tail: {blob[-40:].hex()}')
    # full 256B window above RSP
    win = read_mem(crash[1], 256)
    if win:
        print(f'--- crash RSP+0..256 window:')
        for j in range(0, len(win), 8):
            v, = struct.unpack('<Q', win[j:j + 8])
            if v > 0x10000:
                m = mod_of(v)
                tag = f' => {m}' if m else (' => ' + repr(bytes(win[j:j + 8][::-1].rstrip(b'\0')).strip(b"b'").decode('latin1')) if 0x20 <= (v & 0xFF) < 0x7F and v < (1 << 48) else '')
                print(f'  RSP+0x{j:02x}: {v:#018x}{tag}')

