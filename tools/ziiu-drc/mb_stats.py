"""Per-macroblock statistics of a GamePad H.264 stream (ffmpeg -debug mb_type+qp).

Answers how an encoder uses the stream: QP per macroblock, how many macroblocks are
intra or skipped in P frames, and whether intra macroblocks in P frames sweep the
picture in columns (x264 intra refresh) or follow the content.

  python tools/ziiu-drc/mb_stats.py <ffmpeg.exe> <stream.h264> [frames]
"""
import collections
import re
import subprocess
import sys

ffmpeg, path = sys.argv[1], sys.argv[2]
nframes = int(sys.argv[3]) if len(sys.argv) > 3 else 1800
p = subprocess.Popen([ffmpeg, '-hide_banner', '-threads', '1', '-debug', 'mb_type+qp', '-i', path,
                      '-frames:v', str(nframes), '-f', 'null', '-'], stderr=subprocess.PIPE, text=True,
                     errors='replace')
# ffmpeg mb_type: S skip, I/i/A intra, P PCM, > forward inter, then partition +-| or space
cell = re.compile(r'(\d+)([A-Za-z<>])([+|-]?)')
frames = []          # (type, [[(qp, kind)]])
cur = None
for line in p.stderr:
    m = re.search(r'New frame, type: (\w)', line)
    if m:
        cur = (m.group(1), [])
        frames.append(cur)
        continue
    if cur is None:
        continue
    m = re.match(r'\[h264 @ [0-9a-fx]+\]\s+(\d+) (.*)$', line.rstrip('\n'))
    if m and not m.group(2).strip().startswith('64'):
        row = [(int(q), k + part) for q, k, part in cell.findall(m.group(2))]
        if len(row) >= 50:
            cur[1].append(row)
p.wait()

def kind(k):
    if k[0] == 'S': return 'skip'
    if k[0] in 'IiAP': return 'intra'
    return 'inter'


parts = collections.Counter()

tot = collections.Counter()
qp_by = {'intra': collections.Counter(), 'inter': collections.Counter()}
idr_qp = collections.Counter()
intra_cols = []      # per P frame: set of columns with intra MBs
types = collections.Counter(f[0] for f in frames)
dqp = collections.Counter()   # |QP - previous coded QP| per coded macroblock (raster order, slice QP 32)
wraps = 0
i16 = collections.Counter()
for t, rows in frames:
    prev = 32
    for row in rows:
        for q, k in row:
            if k[0] == 'S':
                continue
            d = q - prev
            if d < -26 or d > 25:
                wraps += 1
            dqp[min(abs(d), 30) // 5 * 5] += 1
            prev = q
            if k[0] in 'Ii':
                i16['16x16' if k[0] == 'I' else '4x4'] += 1
for t, rows in frames:
    cols = set()
    for row in rows:
        for x, (q, k) in enumerate(row):
            kk = kind(k)
            if t == 'I':
                idr_qp[q] += 1
                continue
            tot[kk] += 1
            if kk == 'inter':
                parts[{'': '16x16', '-': '16x8', '|': '8x16', '+': '8x8'}[k[1:]]] += 1
            if kk != 'skip':
                qp_by[kk][q] += 1
            if kk == 'intra':
                cols.add(x)
    if t != 'I':
        intra_cols.append(cols)

print('frames:', dict(types), 'rows/frame:', len(frames[0][1]) if frames else 0)
n = sum(tot.values()) or 1
print('inter partitions:', dict(parts))
print('P-frame macroblocks: ' + ', '.join('%s %.1f%%' % (k, 100.0 * v / n) for k, v in tot.most_common()))
for k in ('inter', 'intra'):
    c = qp_by[k]
    s = sum(c.values()) or 1
    print('%s QP: ' % k + ' '.join('%d:%.0f%%' % (q, 100.0 * v / s) for q, v in sorted(c.items()) if v / s >= 0.005))
s = sum(idr_qp.values()) or 1
print('I-frame QP: ' + ' '.join('%d:%.0f%%' % (q, 100.0 * v / s) for q, v in sorted(idr_qp.items()) if v / s >= 0.005))
with_intra = [c for c in intra_cols if c]
print('P frames with intra MBs: %d of %d' % (len(with_intra), len(intra_cols)))
# Intra refresh signature: intra MBs confined to a narrow column band that advances.
narrow = sum(1 for c in with_intra if max(c) - min(c) <= 3)
print('  of those, intra confined to <=4 adjacent columns: %d' % narrow)
print('  first 12 (column ranges):', [(min(c), max(c)) for c in with_intra[:12]])
print('QP change between coded macroblocks (bucket: count):', dict(sorted(dqp.items())))
print('changes outside -26..+25 (need the modulo-52 wrap): %d' % wraps)
print('intra prediction size:', dict(i16))
