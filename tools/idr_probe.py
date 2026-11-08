# -*- coding: utf-8 -*-
# idr_probe.py — 10 秒 UDP 抓包：统计 PJS 视频包 flags(IDR/KEY/FIRST/LAST) 分布
import socket, struct, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 45690
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", PORT))
s.settimeout(1.0)

t_end = time.time() + 10
n_v = n_idr = n_key = n_first = n_last = n_meta = n_aud = 0
seq_v_min = seq_v_max = None
gaps = 0
prev_v = None
idr_examples = []
while time.time() < t_end:
    try:
        d, _ = s.recvfrom(2048)
    except socket.timeout:
        continue
    if len(d) < 16 or d[:2] != b"PJ":
        continue
    typ = d[2]
    codec = d[3]
    seq = struct.unpack_from(">I", d, 4)[0]
    flags = d[12]
    kind = d[13]
    if typ == 1:  # video
        n_v += 1
        if flags & 4:
            n_idr += 1
            if len(idr_examples) < 3:
                idr_examples.append((seq, flags, d[14:16].hex(), d[16] if len(d) > 16 else None))
        if flags & 8: n_key += 1
        if flags & 1: n_first += 1
        if flags & 2: n_last += 1
        if prev_v is not None and seq != prev_v + 1:
            gaps += 1
        prev_v = seq
    elif typ == 3:
        n_meta += 1
    elif typ == 2:
        n_aud += 1

print(f"video={n_v} idr={n_idr} key={n_key} first={n_first} last={n_last} meta={n_meta} audio={n_aud} seqgaps={gaps}")
for e in idr_examples:
    print("IDR pkt seq=%d flags=0x%02x len_b=%s nal0=0x%02x" % e)
