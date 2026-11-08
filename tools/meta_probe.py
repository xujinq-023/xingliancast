# PJS host-side META/SPS probe: run a sender pointed at 127.0.0.1:<port>,
# reassemble META groups (ts_hi + Annex-B SPS/PPS), parse SPS size in pure
# python (independent impl) and compare against what the TV receiver computes.
import socket, struct, sys, time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 45999
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', port))
s.settimeout(0.5)

FIRST, LAST = 1, 2
meta_groups = {}   # key: tslo or seq-of-LAST -> (payload, complete)
metas = []         # complete meta payloads
vid_first = vid_last = 0
vid_idr = vid_key = 0
vid_gaps = 0
seq_seen = {}

t0 = time.time()
while time.time() - t0 < secs:
    try:
        pkt, _ = s.recvfrom(65535)
    except socket.timeout:
        continue
    if len(pkt) < 16 or pkt[:2] != b'PJ':
        continue
    magic, typ, codec, seq, tslo, flags, kind, plen = struct.unpack_from('>HBBIIbbH', pkt, 0)
    payload = pkt[16:]
    if typ == 2:  # META
        if flags & FIRST:
            meta_groups[tslo] = bytearray(payload)
        elif tslo in meta_groups:
            meta_groups[tslo] += payload
        if flags & LAST and tslo in meta_groups:
            metas.append(bytes(meta_groups.pop(tslo)))
    elif typ == 0:  # VIDEO
        vseq = seq
        prev = seq_seen.get('v')
        if prev is not None and vseq != prev + 1:
            vid_gaps += 1
        seq_seen['v'] = vseq
        if flags & 1: vid_first += 1
        if flags & 2: vid_last += 1
        if flags & 4: vid_idr += 1          # IDR-flagged packets
        elif flags & 8: vid_key += 1        # KEY-flagged (non-IDR)

print(f'metas={len(metas)} vid_first={vid_first} vid_last={vid_last} seq_gaps={vid_gaps} vid_idr_pkts={vid_idr} vid_key_pkts={vid_key}')
if not metas:
    print('NO META PACKETS!'); sys.exit(1)

# ---- independent SPS parser (H.264 7.3.2.1.1) ----
def deblock(b):
    out = bytearray(); zeros = 0
    for x in b:
        if zeros >= 2 and x == 3:
            zeros = 0; continue
        zeros = zeros + 1 if x == 0 else 0
        out.append(x)
    return bytes(out)

class Br:
    def __init__(self, d): self.d, self.pos = d, 0
    def bit(self):
        i = self.pos >> 3
        if i >= len(self.d): raise RuntimeError('overrun')
        v = (self.d[i] >> (7 - (self.pos & 7))) & 1
        self.pos += 1
        return v
    def u(self, n):
        v = 0
        for _ in range(n): v = (v << 1) | self.bit()
        return v
    def ue(self):
        z = 0
        while self.bit() == 0:
            z += 1
            assert z < 32
        return (1 << z) - 1 + self.u(z)
    def se(self):
        k = self.ue()
        return (k + 1) >> 1 if k & 1 else -(k >> 1)

def scaling_list(br, size):
    last = next_ = 8
    for _ in range(size):
        if next_ != 16:
            next_ = (last + br.se() + 256) & 0xFF
        if next_ != 0: last = next_

def parse_sps(nal):  # nal incl 1-byte header
    br = Br(deblock(nal[1:]))
    prof = br.u(8); br.u(8); br.u(8); br.ue()
    chroma = 1
    if prof in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        chroma = br.ue()
        if chroma == 3: br.u(1)
        br.ue(); br.ue(); br.u(1)
        if br.u(1):
            for i in range(12 if prof not in (66, 77, 88) else 8):
                if br.u(1): scaling_list(br, 16 if i < 6 else 64)
    br.ue()
    poc = br.ue()
    if poc == 0: br.ue()
    elif poc == 1:
        br.u(1); br.se(); br.se()
        for _ in range(br.ue()): br.se()
    br.ue(); br.u(1)
    pw = br.ue(); ph = br.ue(); fmo = br.u(1)
    if not fmo: br.u(1)
    br.u(1)
    cw = 1 if chroma == 3 else 2
    ch = (1 if chroma == 0 else 2) * (1 if fmo else 2)
    w = (pw + 1) * 16; h = (ph + 1) * 16 * (1 if fmo else 2)
    if br.u(1):
        l, r, t, b = br.ue(), br.ue(), br.ue(), br.ue()
        w -= (l + r) * cw; h -= (t + b) * ch
    return prof, w, h

def annexb_scan(b):  # yields (type, nal_without_startcode)
    pos, nals = [], []
    i = 0
    while i < len(b) - 3:
        if b[i] == 0 and b[i+1] == 0 and (b[i+2] == 1 or (b[i+2] == 0 and i < len(b)-4 and b[i+3] == 1)):
            pos.append(i + (4 if (b[i+2] == 0 and i < len(b)-4 and b[i+3] == 1) else 3))
            i += 3
        else: i += 1
    pos.append(len(b))
    for j in range(len(pos) - 1):
        nal = b[pos[j]:pos[j+1]]
        if nal: nals.append((nal[0] & 0x1F, nal))
    return nals

for mi, full in enumerate(metas[:3]):
    ts_hi, = struct.unpack_from('>H', full, 0)
    body = full[2:]
    nals = annexb_scan(body)
    print(f'meta#{mi}: len={len(full)} ts_hi={ts_hi} nals={[t for t,_ in nals]}')
    for t, nal in nals:
        print(f'  NAL t={t} full_hex={nal.hex()}')
        if t == 7:
            try:
                prof, w, h = parse_sps(nal)
                print(f'  SPS: profile={prof} {w}x{h} (hex head: {nal[:12].hex()})')
            except Exception as e:
                print(f'  SPS parse FAIL: {e} (hex head: {nal[:12].hex()})')
