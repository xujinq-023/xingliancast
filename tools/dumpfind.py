# 在页堆 dump 中定位 fdk AACENCODER 句柄并读出崩溃时的真实字段
# 锚点: handle+0xd8 处 aacConfig.sampleRate==48000 且 aacConfig+0x10(aot)==2
import struct, sys
from minidump.minidumpfile import MinidumpFile

path = r'D:\DSH-Workplace\wdumps\sender.exe.9808.dmp'
mf = MinidumpFile.parse(path)
reader = mf.get_reader()

def read_mem(addr, n):
    try:
        b = mf.get_reader().read(addr, n)
        if b and len(b) == n:
            return bytes(b)
    except Exception:
        pass
    return None

# 模块基址
mods = sorted(mf.modules.modules, key=lambda m: m.baseaddress)
def mod_of(a):
    for m in mods:
        if m.baseaddress <= a < m.baseaddress + m.sizeofimage:
            return m.name + f'+0x{a-m.baseaddress:x}'
    return None

# 用 memory info 列表拿 region 范围 (仅 MEM_COMMIT)
regions = []
try:
    for i in mf.memory_info.infos:
        st = getattr(i.State, 'value', i.State)
        if st == 0x1000:
            regions.append((i.BaseAddress, i.RegionSize))
except Exception as e:
    print('memory_info fail:', e)
print(f'regions: {len(regions)}')

# 大 region 分块读入搜索模式
PAT_SR = struct.pack('<I', 48000)   # 0xBB80
hits = []
CHUNK = 4 << 20
for base, size in regions:
    if size == 0 or size > 0x20000000:
        continue
    off = 0
    while off < size:
        n = min(CHUNK, size - off)
        b = read_mem(base + off, n)
        if not b:
            # 二分降级读
            n2 = n // 2
            if n2 >= 0x1000:
                b = read_mem(base + off, n2)
                n = n2 if b else n
            if not b:
                off += max(n, 0x1000)
                continue
        pos = 0
        while True:
            i = b.find(PAT_SR, pos)
            if i < 0: break
            if (i & 7) == 0:
                # 校验 handle+0xd8 锚点：region 内 i-0xd8 处即 handle
                hb = i - 0xd8
                if hb >= 0 and len(b) - hb >= 0x300:
                    aot = struct.unpack_from('<I', b, i + 0x10)[0]
                    if aot in (1, 2, 5, 29, 39):
                        hits.append((base + off + hb, b[hb:hb+0x300]))
            pos = i + 4
print(f'candidate handles: {len(hits)}')
for h, blob in hits[:6]:
    g = lambda o: struct.unpack_from('<I', blob, o)[0]
    print(f'--- handle@{h:#x} (module={mod_of(h)})')
    print(f'  InitFlags@+0x2c8 = {g(0x2c8):#x}')
    print(f'  aacConfig: sampleRate={g(0xd8)} aot={g(0xd8+0x10)} nCh?+0x1c={g(0xd8+0x1c)} channelMode@+0x28={g(0xd8+0x28)}')
    print(f'  extParam@+0x140: userAOT={g(0x140)} userSR={g(0x140+4)} downscale@+0x48={g(0x140+0x48)} userChMode@+0xc={g(0x140+0xc)}')
    print(f'  extParam raw[0..0x60]: {blob[0x140:0x160].hex()} | {blob[0x160:0x180].hex()}')
