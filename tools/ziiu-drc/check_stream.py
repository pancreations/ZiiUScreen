"""Offline check of ziiu-drc's output, no radio or GamePad.

Plays the bridge: binds 127.0.0.1:50120/50121/50123, runs ziiu-drc for a few
seconds, then checks
  - vstrm headers (magic, 10-bit sequence, init/frame/chunk flags, timestamps,
    extension options) and 5 chunks per frame,
  - astrm video-format and audio packets, command queries,
  - the video itself: frames are rebuilt into a plain H.264 stream with the
    GamePad's SPS/PPS and slice headers (as libdrc's debug dump does), decoded
    with ffmpeg and compared with the test pattern's colour bars.

  python tools/ziiu-drc/check_stream.py [ffmpeg.exe]

With ZIIU_CAPTURE set (desktop capture), the decoded frames are instead compared
with the source frames ziiu-drc dumps (ZIIU_CAPTURE_DUMP, frames 120..120+N): a
decoder/encoder mismatch shows as PSNR falling frame after frame (drift), encoder
loss as a steady level. ZIIU_CHECK_SECONDS sets the run length (default 4).
"""
import os
import shutil

import numpy as np
import socket
import select
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.environ.get('ZIIU_CHECK_EXE', os.path.join(REPO, '.local', 'build', 'ziiu-drc', 'ziiu-drc.exe'))
PORT_OFFSET = int(os.environ.get('ZIIU_TEST_PORT_OFFSET', '0'))
W, H = 864, 480

# libdrc h264-encoder.cpp DumpH264Headers / DumpH264Frame
SPS = bytes([0x67, 0x64, 0x00, 0x20, 0xac, 0x2b, 0x40, 0x6c, 0x1e, 0xf3, 0x68])
PPS = bytes([0x68, 0xee, 0x06, 0x0c, 0xe8])
START = b'\x00\x00\x00\x01'


def nal_escape(src):
    out = bytearray(src[:2])
    for b in src[2:]:
        if b <= 3 and out[-2] == 0 and out[-1] == 0:
            out.append(3)
        out.append(b)
    return bytes(out)


def fail(msg):
    print('FAIL:', msg)
    sys.exit(1)


def main():
    ffmpeg = sys.argv[1] if len(sys.argv) > 1 else shutil.which('ffmpeg')
    socks = {}
    for port in (50120, 50121, 50123, 50125):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)   # keyframe bursts
        s.bind(('127.0.0.1', port + PORT_OFFSET))
        s.setblocking(False)
        socks[port] = s
    secs = int(os.environ.get('ZIIU_CHECK_SECONDS', '4'))
    pkts = {p: [] for p in socks}
    # A pipe can fill during longer diagnostics and block the encoder. Also drain
    # every ready socket through process exit, rather than losing the final burst.
    with tempfile.TemporaryFile() as log:
        proc = subprocess.Popen([EXE, str(secs), os.environ.get('ZIIU_CHECK_PRESET', 'slow')], stdout=log, stderr=subprocess.STDOUT)
        end = time.monotonic() + secs + 5
        resync_every = float(os.environ.get('ZIIU_CHECK_RESYNC_EVERY', '0'))
        next_resync = time.monotonic() + 2
        try:
            while time.monotonic() < end:
                if resync_every > 0 and time.monotonic() >= next_resync and proc.poll() is None:
                    socks[50120].sendto(b'\x01\x00\x00\x00', ('127.0.0.1', 50010 + PORT_OFFSET))
                    next_resync += resync_every
                ready, _, _ = select.select(list(socks.values()), [], [], 0.05)
                if not ready and proc.poll() is not None:
                    break
                for port, s in socks.items():
                    if s not in ready:
                        continue
                    try:
                        while True:
                            pkts[port].append(s.recv(4096))
                    except (BlockingIOError, ConnectionResetError):
                        pass
            proc.wait(timeout=2)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            for s in socks.values():
                s.close()
        log.seek(0)
        out = log.read().decode(errors='replace')
    print(out.strip())
    if os.environ.get('ZIIU_CHECK_DIR'):
        os.makedirs(os.environ['ZIIU_CHECK_DIR'], exist_ok=True)
        with open(os.path.join(os.environ['ZIIU_CHECK_DIR'], 'encoder.log'), 'w', encoding='utf-8') as f:
            f.write(out)
    if proc.returncode:
        fail('ziiu-drc exited with code %d' % proc.returncode)

    vid, aud, cmd = pkts[50120], pkts[50121], pkts[50123]
    print('captured: vstrm=%d astrm=%d cmd=%d' % (len(vid), len(aud), len(cmd)))
    if len(vid) < 50:
        fail('too few video packets')

    # vstrm headers and frame assembly
    frames, cur, prev_seq, ts = [], None, None, None
    for i, p in enumerate(vid):
        if len(p) < 16 or p[0] >> 4 != 0xF or not (p[2] & 0x08):
            fail('bad vstrm header at %d' % i)
        seq = ((p[0] & 3) << 8) | p[1]
        if prev_seq is not None and seq != (prev_seq + 1) % 1024:
            fail('vstrm sequence gap %d -> %d' % (prev_seq, seq))
        prev_seq = seq
        size = ((p[2] & 7) << 8) | p[3]
        if size != len(p) - 16 or size > 1400:
            fail('vstrm size %d vs %d' % (size, len(p) - 16))
        init, begin, chunk_end, frame_end = (p[2] >> 7) & 1, (p[2] >> 6) & 1, (p[2] >> 5) & 1, (p[2] >> 4) & 1
        if init != (i == 0 or (cur is None and not frames) or (frames == [] and cur is not None and cur['init'])):
            pass  # checked per frame below
        opts = p[8:16]   # the Wii U's order: [80 (IDR)] 82 00 83 85 06
        idr = opts[0] == 0x80
        if opts[int(idr):int(idr) + 5] != bytes([0x82, 0, 0x83, 0x85, 6]):
            fail('vstrm options %s' % opts.hex())
        t = int.from_bytes(p[4:8], 'big')
        if begin:
            if cur is not None:
                fail('frame begin inside a frame')
            cur = {'ts': t, 'idr': idr, 'init': init, 'chunks': [b''], 'ends': 0}
        if cur is None:
            continue   # capture started mid-frame
        if t != cur['ts'] or idr != cur['idr'] or init != cur['init']:
            fail('header fields change inside a frame')
        cur['chunks'][-1] += p[16:]
        if chunk_end:
            cur['ends'] += 1
            if not frame_end:
                cur['chunks'].append(b'')
        if frame_end:
            if cur['ends'] != 5:
                fail('frame has %d chunks' % cur['ends'])
            frames.append(cur)
            cur = None
    if not frames or not frames[0]['idr'] or not frames[0]['init']:
        fail('first frame must be IDR with the init flag')
    if any(f['init'] for f in frames[1:]):
        fail('init flag after the first frame')
    if any(not 0 < ((b['ts'] - a['ts']) & 0xffffffff) < 0x80000000 for a, b in zip(frames, frames[1:])):
        fail('timestamps not increasing')
    print('vstrm OK: %d frames, first IDR, 5 chunks each, avg %d bytes/frame' %
          (len(frames), sum(len(b''.join(f['chunks'])) for f in frames) // len(frames)))

    # astrm
    vf = [p for p in aud if (p[0] >> 2) & 1]
    au = [p for p in aud if not (p[0] >> 2) & 1]
    vf_tail = bytes.fromhex('803e0000803e00000852000008520000') + bytes(4)
    if not vf or any(len(p) != 32 or p[:4] != bytes([4, 0, 0, 0]) or p[4:8] != bytes([0, 0, 1, 0]) or
                     p[12:] != vf_tail for p in vf):
        fail('astrm video-format packets')
    if not au or any(len(p) != 8 + 1664 or (p[0] >> 5) != 1 for p in au):
        fail('astrm audio packets')
    print('astrm OK: %d video-format, %d audio' % (len(vf), len(au)))
    if au:
        samples = np.frombuffer(b''.join(p[8:] for p in au), dtype='<i2').astype(np.int32)
        print('audio payload: peak %d/32767, nonzero samples %d/%d' % (
            np.abs(samples).max(), np.count_nonzero(samples), samples.size))
        print('audio waveform: min %d max %d mean %.1f stddev %.1f' % (
            samples.min(), samples.max(), samples.mean(), samples.std()))

    # cmd queries
    # Without a GamePad each query is retried ten times before the next starts.
    # Long optimization runs legitimately advance beyond the first time query.
    first = cmd[0] if cmd else b''
    if len(first) != 16 or first[0] != 0 or first[2] != 2 or first[4] != 8:
        fail('first command should be the time query, got %s' % first[:16].hex())
    previous, attempts = first, 0
    for p in cmd:
        if len(p) < 8 or p[0] != 0:
            fail('unexpected command packet')
        if p[6:8] != previous[6:8]:
            if attempts != 10 or int.from_bytes(p[6:8], 'little') != (int.from_bytes(previous[6:8], 'little') + 1) % 65536:
                fail('next query sent before the previous query exhausted its retries')
            previous, attempts = p, 0
        if p != previous:
            fail('query changed during a retry')
        attempts += 1
    status = pkts[50125]
    if not status or any(len(p) != 96 or p[8:10] != bytes([0x5a, 0xa5]) for p in status):
        fail('50125 status messages')
    print('cmd OK: time query first, queries serialized with ten tries, %d status messages' % len(status))

    # decode
    if not ffmpeg:
        print('ffmpeg not found: skipped decode check')
        return
    tmp = os.environ.get('ZIIU_CHECK_DIR') or tempfile.mkdtemp()
    os.makedirs(tmp, exist_ok=True)
    h264 = os.path.join(tmp, 'drc.h264')
    with open(h264, 'wb') as f:
        f.write(START + SPS + START + PPS)
        fn = 0
        for fr in frames:
            if fr['idr']:
                fn = 0
                hdr = bytes([0x25, 0xb8, 0x04, 0xff])
            else:
                fn = (fn + 1) & 0xff
                hdr = bytes([0x21, 0xe0 | (fn >> 3), 0x03 | ((fn << 5) & 0xff), 0xff])
            f.write(START + nal_escape(hdr + b''.join(fr['chunks'])))
    raw = os.path.join(tmp, 'out.yuv')
    r = subprocess.run([ffmpeg, '-v', 'error', '-y', '-i', h264, '-f', 'rawvideo', '-pix_fmt', 'yuv420p', raw],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(raw):
        fail('ffmpeg decode: %s' % r.stderr.strip()[:400])
    errors = [l for l in r.stderr.splitlines() if l.strip()]
    probe = subprocess.run([os.path.join(os.path.dirname(ffmpeg), 'ffprobe'), '-v', 'error',
                            '-show_entries', 'stream=width,height', '-of', 'csv=p=0', h264],
                           capture_output=True, text=True).stdout.strip()
    w, h = [int(x) for x in probe.split(',')[:2]]
    data = np.fromfile(raw, dtype=np.uint8)
    fsize = w * h * 3 // 2
    n = len(data) // fsize
    print('ffmpeg: %d frames %dx%d (SPS crops 864 -> %d), %d decoder messages' % (n, w, h, w, len(errors)))
    for l in errors[:6]:
        print('   ', l)
    if errors:
        fail('ffmpeg reported decoder errors')
    if n < len(frames) - 2:
        fail('decoded %d of %d frames' % (n, len(frames)))
    if os.environ.get('ZIIU_CAPTURE') or os.environ.get('ZIIU_REPLAY'):
        compare_capture(data, fsize, w, h, n)
        return
    psnr = []
    for k in range(n):
        y = data[k * fsize:k * fsize + w * h].reshape(h, w).astype(np.float64)
        mse = np.mean((y - reference(k)[:, :w]) ** 2)
        psnr.append(99.0 if mse == 0 else 10 * np.log10(255 * 255 / mse))
    bad = [k for k, v in enumerate(psnr) if v < 30]
    print('luma PSNR vs test pattern: min %.1f dB, median %.1f dB, frames below 30 dB: %d %s' %
          (min(psnr), sorted(psnr)[n // 2], len(bad), bad[:20]))
    if bad:
        fail('%d decoded frames do not match the test pattern' % len(bad))
    print('PASS: stream decodes with the GamePad SPS/PPS and matches the test pattern')


def compare_capture(data, fsize, w, h, n):
    """Decoded frames vs the source frames ziiu-drc dumped (frames 120.. on)."""
    src_path = os.environ.get('ZIIU_CAPTURE_DUMP')
    if not src_path or not os.path.exists(src_path):
        fail('set ZIIU_CAPTURE_DUMP (and ZIIU_CAPTURE_DUMP_FRAMES) for a capture check')
    src = np.fromfile(src_path, dtype=np.uint8)
    ssize = W * H * 3 // 2
    m = min(len(src) // ssize, n - 120)
    psnr = []
    for k in range(m):
        s = src[k * ssize:k * ssize + W * H].reshape(H, W)[:, :w].astype(np.float64)
        d = data[(120 + k) * fsize:(120 + k) * fsize + w * h].reshape(h, w).astype(np.float64)
        mse = np.mean((d - s) ** 2)
        psnr.append(99.0 if mse == 0 else 10 * np.log10(255 * 255 / mse))
    if not psnr:
        fail('no overlapping frames')
    print('luma PSNR decoded vs source, frames 120..%d:' % (119 + m))
    for k in range(0, m, max(1, m // 12)):
        print('  frame %d: %.1f dB' % (120 + k, psnr[k]))
    print('min %.1f dB, median %.1f dB, first %.1f, last %.1f' % (min(psnr), sorted(psnr)[m // 2], psnr[0], psnr[-1]))
    # Chroma: an encoder/decoder chroma QP mismatch only shows here.
    cw, ch = w // 2, h // 2
    for name, so, do in (('U', W * H, w * h), ('V', W * H * 5 // 4, w * h * 5 // 4)):
        cp = []
        for k in range(m):
            s = src[k * ssize + so:k * ssize + so + (W // 2) * (H // 2)].reshape(H // 2, W // 2)[:, :cw].astype(np.float64)
            d = data[(120 + k) * fsize + do:(120 + k) * fsize + do + cw * ch].reshape(ch, cw).astype(np.float64)
            mse = np.mean((d - s) ** 2)
            cp.append(99.0 if mse == 0 else 10 * np.log10(255 * 255 / mse))
        print('%s PSNR: min %.1f dB, median %.1f dB, first %.1f, last %.1f' % (name, min(cp), sorted(cp)[m // 2], cp[0], cp[-1]))
    np.save(os.path.join(os.path.dirname(src_path), 'decoded.npy'),
            data[120 * fsize:(120 + m) * fsize].reshape(m, fsize)[:, :w * h].reshape(m, h, w))


def reference(frame):
    """ziiu_drc.cpp DrawPattern, luma only."""
    x = np.arange(W)
    y = np.zeros((H, W))
    bars = np.array([235, 210, 170, 145, 106, 81, 41, 16])
    y[:320, :] = bars[x * 8 // W]
    y[320:, :] = 16 + x * 219 // W
    sq = (frame * 4) % (W - 64)
    y[360:424, sq:sq + 64] = 235
    return y


if __name__ == '__main__':
    main()
