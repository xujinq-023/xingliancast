# Capture PJS video AUs from :45690 into an Annex-B .264 file for 12 s.
# Run while the TV receiver is force-stopped (port contention).
import socket, struct, sys, time

PORT = 45690
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', PORT))
s.settimeout(1.0)

H = 16  # 'PJ' type codec seq(4) ts_lo(4) flags kind len(2)
FIRST, LAST = 1, 2
buf = bytearray()
cur_ts = -1
n_au = 0
n_au_bytes = 0
stats = {'video': 0, 'audio': 0, 'meta': 0, 'other': 0, 'seqgaps': 0, 'short': 0}
last_seq = [-1]

def flush(f):
    global n_au, n_au_bytes, cur_ts
    if len(buf) > 4:
        f.write(bytes(buf)); f.flush()
        n_au += 1; n_au_bytes += len(buf)

t0 = time.time()
fn = r'C:\Users\Administrator\AppData\Local\Temp\pjs_dump.264'
with open(fn, 'wb') as f:
    while time.time() - t0 < 12:
        try:
            d, _ = s.recvfrom(2048)
        except socket.timeout:
            continue
        if len(d) < H or d[:2] != b'PJ':
            stats['short'] += 1
            continue
        typ, codec = d[2], d[3]
        seq, tslo, flags, kind, plen = struct.unpack('>IIIBB H'.replace(' ', ''), d[4:16])
        if len(d) < H + plen:
            stats['short'] += 1
            continue
        if typ == 0:
            if last_seq[0] >= 0 and seq - last_seq[0] not in (0, 1):
                stats['seqgaps'] += 1
            last_seq[0] = seq
            stats['video'] += 1
            if (flags & 1) and cur_ts != tslo:
                if len(buf) > 4:
                    f.write(bytes(buf)); f.flush(); n_au += 1; n_au_bytes += len(buf)
                buf.clear(); cur_ts = tslo
            buf += d[H:H + plen]
            if flags & LAST:
                if len(buf) > 4:
                    f.write(bytes(buf)); f.flush(); n_au += 1; n_au_bytes += len(buf)
                buf.clear(); cur_ts = -1
        elif typ == 1:
            stats['audio'] += 1
        elif typ == 2:
            stats['meta'] += 1
        else:
            stats['other'] += 1

print('WROTE', fn, 'AUs=', n_au, 'bytes=', n_au_bytes, 'avg=', (n_au_bytes // max(n_au, 1)))
print('stats', stats)
