"""Do P_Skip macroblocks carry a non-zero (predicted) motion vector?

H.264 P_Skip blocks inherit the predicted motion vector of their neighbours; a
decoder that treats skip as "copy the co-located block" would leave scrolled areas
behind. This joins ffmpeg's per-macroblock types (-debug mb_type) with the motion
vectors it exports (PyAV, export_mvs) and counts skip blocks by their vector.

  .local/tools/av-venv/Scripts/python tools/ziiu-drc/skip_mv.py <ffmpeg.exe> <stream.h264> [frames]
"""
import collections
import re
import subprocess
import sys

import av

ffmpeg, path = sys.argv[1], sys.argv[2]
nframes = int(sys.argv[3]) if len(sys.argv) > 3 else 1800
MBW = 54

# Macroblock types per frame, in decode order.
p = subprocess.run([ffmpeg, '-hide_banner', '-threads', '1', '-debug', 'mb_type', '-i', path,
                    '-frames:v', str(nframes), '-f', 'null', '-'], capture_output=True, text=True, errors='replace')
types, cur = [], None
cell = re.compile(r'([A-Za-z<>])([+|-]?)')
for line in p.stderr.splitlines():
    if 'New frame, type:' in line:
        cur = []
        types.append(cur)
        continue
    m = re.match(r'\[h264 @ [0-9a-fx]+\]\s+(\d+) (.*)$', line)
    if cur is not None and m and not m.group(2).strip().startswith('64'):
        row = [k for k, _ in cell.findall(m.group(2))]
        if len(row) == MBW:
            cur.extend(row)

# Motion vectors per frame.
c = av.open(path)
st = c.streams.video[0]
st.codec_context.options = {'flags2': '+export_mvs'}
st.thread_type = 'NONE'
skip = collections.Counter()
inter = collections.Counter()
examples = []
for i, fr in enumerate(c.decode(st)):
    if i >= len(types) or i >= nframes:
        break
    mbs = types[i]
    if len(mbs) != MBW * 30:
        continue
    mv = {}
    sd = fr.side_data.get('MOTION_VECTORS') if hasattr(fr.side_data, 'get') else None
    if sd is not None:
        for v in sd:
            if v.source != -1:
                continue
            mbx, mby = (v.dst_x - v.w // 2) // 16, (v.dst_y - v.h // 2) // 16
            nz = v.motion_x != 0 or v.motion_y != 0
            mv[(mbx, mby)] = mv.get((mbx, mby), False) or nz
    for idx, k in enumerate(mbs):
        key = (idx % MBW, idx // MBW)
        if k == 'S':
            nz = mv.get(key, False)
            skip['nonzero' if nz else 'zero'] += 1
            if nz and len(examples) < 5:
                examples.append((i, key))
        elif k == '>':
            inter['nonzero' if mv.get(key, False) else 'zero'] += 1
print('P_Skip macroblocks by inherited vector:', dict(skip))
print('inter macroblocks by vector:', dict(inter))
print('first skip blocks with a non-zero vector (frame, (mb x, mb y)):', examples)
