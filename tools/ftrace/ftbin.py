#!/usr/bin/python3 -I
# Decodes raw pstore function records (ip, parent ip, stamp << 8 | cpu), as stock's pmsg zone shows them,
# against the crashing boot's symbol table. Usage: ftbin.py RAW KALLSYMS [LAST] [PERCPU]
import bisect, struct, sys
raw = open(sys.argv[1], 'rb').read()
last = int(sys.argv[3]) if len(sys.argv) > 3 else 60
percpu = int(sys.argv[4]) if len(sys.argv) > 4 else 12
addr, name = [], []
for l in open(sys.argv[2], errors='replace'):
    p = l.split()
    if len(p) >= 3:
        addr.append(int(p[0], 16)); name.append(p[2] + (' ' + p[3] if len(p) > 3 else ''))
order = sorted(range(len(addr)), key=addr.__getitem__)
addr = [addr[i] for i in order]; name = [name[i] for i in order]
def sym(a):
    if a >= 0xffff800080000000:
        a += 0x80000
    i = bisect.bisect_right(addr, a) - 1
    return name[i] if i >= 0 else hex(a)
best, bestn = 0, -1
base = len(raw) - 24 * 3000
for off in range(24):
    n = 0
    for i in range(base + off, len(raw) - 48, 24):
        ip, pip, ts = struct.unpack_from('<QQQ', raw, i)
        ts2 = struct.unpack_from('<Q', raw, i + 40)[0]
        if ip >> 48 == 0xffff and pip >> 48 == 0xffff and (ts & 0xff) < 8 and 0 <= (ts2 >> 8) - (ts >> 8) < 64:
            n += 1
    if n > bestn:
        best, bestn = off, n
best = (best + base) % 24
recs = [struct.unpack_from('<QQQ', raw, i) for i in range(best, len(raw) - 23, 24)]
print(f'{len(raw)} bytes, alignment {best} ({bestn} of 3000 look right), {len(recs)} records')
per = {}
for r in recs:
    per.setdefault(r[2] & 0xff, []).append(r)
print('per CPU counts:', {c: len(v) for c, v in sorted(per.items()) if c < 8}, 'bad', sum(len(v) for c, v in per.items() if c >= 8))
good = [r for r in recs if r[2] & 0xff < 8 and r[0] >> 48 == 0xffff]
print(f'stamps {good[0][2] >> 8} .. {good[-1][2] >> 8}')
print('--- newest records overall, oldest first: stamp cpu function <- caller')
end = good[-1][2] >> 8
for ip, pip, ts in good[-last:]:
    print(f'{(ts >> 8) - end:>7} {ts & 0xff} {sym(ip):<44} <- {sym(pip)}')
print('--- last records of each CPU')
for c, v in sorted(per.items()):
    if c >= 8:
        continue
    print(f'cpu{c}: newest is {(v[-1][2] >> 8) - end} from the end')
    for ip, pip, ts in v[-percpu:]:
        print(f'{(ts >> 8) - end:>7} {sym(ip):<44} <- {sym(pip)}')
