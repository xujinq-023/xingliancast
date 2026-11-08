# 分析 WER full dump: 从 crash 线程 CONTEXT 恢复寄存器 → 追对象 vtable → 定位 purecall 槽
import sys, io, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
from minidump.minidumpfile import MinidumpFile

PATH = sys.argv[1] if len(sys.argv) > 1 else r'D:\DSH-Workplace\wdumps\sender.exe.18840.dmp'
mf = MinidumpFile.parse(PATH)
reader = mf.get_reader()
mods = sorted(mf.modules.modules, key=lambda m: m.baseaddress)

def mod_of(a):
    for m in mods:
        if m.baseaddress <= a < m.baseaddress + m.size:
            return f'{m.name.rsplit(".",1)[0]}+0x{a - m.baseaddress:x}'
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

er0 = mf.exception.exception_records[0]
tid = er0.ThreadId
ct = next(t.ContextObject for t in mf.threads.threads if t.ThreadId == tid)
rip, rsp, rbp = ct.Rip, ct.Rsp, ct.Rbp
rbx, r14 = getattr(ct, 'Rbx', 0) or 0, getattr(ct, 'R14', 0) or 0
print(f'crash tid={tid:#x} rip={mod_of(rip)} rsp={rsp:#x} rbp={rbp:#x} rbx={rbx:#x} r14={r14:#x}')

# sender 基址
smod = next(m for m in mods if 'sender' in m.name.lower())
SB = smod.baseaddress
print(f'sender base={SB:#x}')

# rip 所在处指令字节
ib = read_mem(rip - 12, 24)
print(f'bytes @rip-12..rip+11: {ib.hex() if ib else None}')

# rsp 窗口 + 对象特征扫描
print('\n--- RSP window (deref+vtbl heuristic) ---')
win = read_mem(rsp, 0x400)
cands = []
for j in range(0, len(win) - 7, 8):
    v, = struct.unpack('<Q', win[j:j + 8])
    if v and v < (1 << 48) and (v & 7) == 0:
        b8 = read_mem(v, 8)
        if b8:
            vtv, = struct.unpack('<Q', b8)
            mm = mod_of(vtv)
            if mm and ('sender' in mm.lower() or 'mf' in mm.lower() or 'combase' in mm.lower()):
                cands.append((j, v, vtv, mm))
                print(f'  RSP+0x{j:03x}: obj={v:#x} vtbl={vtv:#x} => {mm}')

# rbx / r14 对象
for name, v in (('rbx', rbx), ('r14', r14)):
    if v and v < (1 << 48):
        b8 = read_mem(v, 8)
        if b8:
            vtv, = struct.unpack('<Q', b8)
            print(f'\n{name}={v:#x} vtbl={vtv:#x} => {mod_of(vtv)}')
            bts = read_mem(vtv, 0xC0)
            if bts:
                for k in range(0, 0xC0, 8):
                    tv, = struct.unpack('<Q', bts[k:k + 8])
                    mm = mod_of(tv)
                    if mm:
                        print(f'    slot[{k//8}] = {tv:#x} => {mm}')

# 崩溃点前指令：sender+0x5EC7 处 call [rax+10h]，对象 = [rbp-0x61]
OBJ = rbp - 0x61
print(f'\n=== TARGET OBJ=[rbp-0x61]={OBJ:#x} ===')
b8 = read_mem(OBJ, 8)
if b8:
    obj, = struct.unpack('<Q', b8)
    print(f'obj = {obj:#x}')
    vb = read_mem(obj, 0x140)
    if vb:
        vtv, = struct.unpack('<Q', vb[:8])
        print(f'vtbl = {vtv:#x} => {mod_of(vtv)}')
        for k in range(0, 0x140, 8):
            tv, = struct.unpack('<Q', vb[k:k + 8])
            mm = mod_of(tv)
            print(f'  slot[{k//8:2d}] = {tv:#x} {"=> " + mm if mm else ""}')
cb = read_mem(SB + 0x5EA0, 0x40)
if cb:
    print(f'\n--- sender+0x5EA0..0x5EDF raw ---\n{cb.hex()}')

# ---- 纯调用桩：ucrtbase 导出 RVA 0xb2f90 = _purecall ----
ucrt = next((m for m in mods if 'ucrtbase' in m.name.lower()), None)
PURE = ucrt.baseaddress + 0xb2f90 if ucrt else 0
print(f'\n_purecall @ {PURE:#x}')

# ---- 堆扫描：vtable 位于已加载模块 rdata，且 slot2(Release)==_purecall 的对象 ----
import time
mod_lo = sorted(m.baseaddress for m in mods)
def in_mod(a):
    for m in mods:
        if m.baseaddress <= a < m.baseaddress + m.size:
            return m
    return None

t0 = time.time()
hits = []
scan_lo, scan_hi = 0x122000000000, 0x130000000000   # 二级堆区
PAGE = 0x10000
a0 = scan_lo
pages = 0
while a0 < scan_hi and time.time() - t0 < 160:
    blob = read_mem(a0, PAGE)
    if blob:
        pages += 1
        for j in range(0, len(blob) - 7, 8):
            vp, = struct.unpack('<Q', blob[j:j + 8])
            mm = in_mod(vp)
            if mm and (vp & 7) == 0:
                vb = read_mem(vp, 40)
                if vb and len(vb) >= 24:
                    s2, = struct.unpack('<Q', vb[16:24])
                    if s2 == PURE:
                        objaddr = a0 + j
                        on_stack = 0x10168f000 <= objaddr < 0x101690000
                        print(f'HIT obj={objaddr:#x} vtbl={vp:#x} [{mm.name.rsplit(".",1)[0]}+0x{vp-mm.baseaddress:x}] stack={on_stack}')
                        print(f'    slots(rel): ' + ' '.join(f'{struct.unpack("<Q", vb[k:k+8])[0]-vp:+#x}' for k in range(0, 40, 8)))
                        hits.append(objaddr)
                        if len(hits) >= 20:
                            break
        if len(hits) >= 20:
            break
    a0 += PAGE
print(f'scanned pages={pages} hits={len(hits)} elapsed={time.time()-t0:.0f}s')


# purecall 桩地址（ucrtbase 导出 RVA 0xb2f90）
ucrt = next((m for m in mods if 'ucrtbase' in m.name.lower()), None)
PURE = ucrt.baseaddress + 0xb2f90 if ucrt else 0
print(f'\n_purecall @ {PURE:#x}')

# 堆对象扫描：vtable 指针落在 sender rdata，且 vtable[2](Release) == _purecall
smod_e = SB + smod.size
lo, hi = SB + 0x100000, SB + 0x1000000
hits = []
step = 0x40000
for a0 in range(lo, hi, step):
    blob = read_mem(a0, min(step, hi - a0))
    if not blob:
        continue
    for j in range(0, len(blob) - 15, 8):
        vp, = struct.unpack('<Q', blob[j:j + 8])
        if SB + 0x1000 <= vp < smod_e and (vp & 7) == 0:
            vb = read_mem(vp, 40)
            if vb:
                s2, = struct.unpack('<Q', vb[16:24])
                if s2 == PURE:
                    objaddr = a0 + j
                    vt_rva = vp - SB
                    obj_rva = objaddr - SB
                    in_stack = 0x10168f000 <= objaddr < 0x101690000
                    print(f'  HIT obj={objaddr:#x} (sender+{obj_rva:#x}, stack={in_stack}) vtbl=sender+{vt_rva:#x}')
                    print(f'      slots: ' + ' '.join(f'{struct.unpack("<Q", vb[k:k+8])[0] - SB:+#x}' for k in (0, 8, 16, 24, 32)))
                    hits.append((objaddr, vt_rva))
                    if len(hits) >= 12:
                        break
    if len(hits) >= 12:
        break
print(f'total hits: {len(hits)}')

