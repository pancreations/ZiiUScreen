"""Motion vector features of a GamePad H.264 stream: sub-pixel precision, size, and
references outside the picture. Compare the real Wii U's stream with ziiu-drc's to
find what the GamePad's decoder never sees from a real console.

  .local/tools/av-venv/Scripts/python tools/ziiu-drc/mv_stats.py <stream.h264> [frames]
"""
import collections
import sys

import av

path = sys.argv[1]
nframes = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
W, H = 864, 480
c = av.open(path)
st = c.streams.video[0]
st.codec_context.options = {'flags2': '+export_mvs'}
st.thread_type = 'NONE'
frac = collections.Counter()      # (x frac, y frac) in quarter pels
sizes = collections.Counter()
outside = collections.Counter()
maxmv = [0, 0]
overhang = collections.Counter()   # px beyond the picture edge, max per side
hist = collections.Counter()
n = 0
for i, fr in enumerate(c.decode(st)):
    if i >= nframes:
        break
    sd = fr.side_data.get('MOTION_VECTORS')
    if sd is None:
        continue
    for v in sd:
        if v.source != -1 or (v.motion_x == 0 and v.motion_y == 0):
            continue
        n += 1
        s = v.motion_scale
        fx, fy = (v.motion_x * 4 // s) % 4, (v.motion_y * 4 // s) % 4
        frac[('full' if fx == 0 else 'half' if fx == 2 else 'qpel',
              'full' if fy == 0 else 'half' if fy == 2 else 'qpel')] += 1
        sizes['%dx%d' % (v.w, v.h)] += 1
        mx, my = v.motion_x / s, v.motion_y / s
        maxmv[0] = max(maxmv[0], abs(mx))
        maxmv[1] = max(maxmv[1], abs(my))
        x0, y0 = v.dst_x - v.w / 2 + mx, v.dst_y - v.h / 2 + my     # reference block top-left
        if x0 < 0 or y0 < 0 or x0 + v.w > W or y0 + v.h > H:
            outside['left' if x0 < 0 else 'right' if x0 + v.w > W else 'top' if y0 < 0 else 'bottom'] += 1
            for side, d in (('left', -x0), ('right', x0 + v.w - W), ('top', -y0), ('bottom', y0 + v.h - H)):
                if d > 0:
                    overhang[side] = max(overhang[side], d)
                    hist[min(int(d) // 4 * 4, 64)] += 1
print('non-zero vectors: %d' % n)
print('precision (x, y): ' + ', '.join('%s/%s %.1f%%' % (a, b, 100.0 * k / n) for (a, b), k in frac.most_common()))
print('block sizes:', dict(sizes))
print('max |mv|: x %.2f y %.2f px' % tuple(maxmv))
print('reference outside the picture: %d (%.2f%%) %s' % (sum(outside.values()), 100.0 * sum(outside.values()) / max(n, 1),
                                                         dict(outside)))
print('max overhang past the edge (px):', {k: round(v, 2) for k, v in overhang.items()})
print('overhang histogram (px bucket: count):', dict(sorted(hist.items())))
