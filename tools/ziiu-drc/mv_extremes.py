"""Signed motion vector extremes and percentiles per direction (is the GamePad's
reference window symmetric?).

  .local/tools/av-venv/Scripts/python tools/ziiu-drc/mv_extremes.py <stream.h264> [frames]
"""
import sys

import av
import numpy as np

c = av.open(sys.argv[1])
nframes = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
st = c.streams.video[0]
st.codec_context.options = {'flags2': '+export_mvs'}
st.thread_type = 'NONE'
xs, ys = [], []
for i, fr in enumerate(c.decode(st)):
    if i >= nframes:
        break
    sd = fr.side_data.get('MOTION_VECTORS')
    if sd is None:
        continue
    for v in sd:
        if v.source == -1 and (v.motion_x or v.motion_y):
            # motion_x/y point from the block to its reference (ffmpeg: src = dst + motion)
            xs.append(v.motion_x / v.motion_scale)
            ys.append(v.motion_y / v.motion_scale)
xs, ys = np.array(xs), np.array(ys)
for name, a in (('x (reference to the right +)', xs), ('y (reference below +)', ys)):
    print('%s: min %.2f p0.1 %.2f p1 %.2f p99 %.2f p99.9 %.2f max %.2f' % (
        name, a.min(), np.percentile(a, 0.1), np.percentile(a, 1), np.percentile(a, 99), np.percentile(a, 99.9), a.max()))
