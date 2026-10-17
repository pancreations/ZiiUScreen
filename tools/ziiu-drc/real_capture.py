"""Decrypt and analyse a receive-only capture of the owner's real Wii U and GamePad.

The reference for ziiu-drc's protocol details: what the real console sends, byte for
byte, and how it encodes video. Nothing here transmits.

  1. Record (Wii U on, GamePad connected; power-cycle the GamePad during the capture
     so the 4-way handshake is in it):
       ziiuctl sniff wiiu-lock 150 .local\\wiiu\\real-session.pcap
  2. python tools/ziiu-drc/real_capture.py decrypt   -> .local/wiiu/real-session-udp.pkl
  3. python tools/ziiu-drc/real_capture.py protocol  (startup commands, packet layouts,
                                                      timing, audio, IP marking)
  4. python tools/ziiu-drc/real_capture.py video <ffmpeg.exe>
                                                     (rebuilds the H.264 with the GamePad
                                                      SPS/PPS, decodes, saves every 300th
                                                      frame to .local/wiiu/real-video)

Key material: the PMK is read from the gitignored .local/gamepad-link/hostapd.conf
(written by `ziiuctl ap-hostapd`), the GamePad and console MACs come from the
capture and .local/wiiu/ziiu-probe.txt. Nothing secret is printed or written; the
cache holds decrypted UDP payloads only (gitignored .local).

PTK derivation: drc-hostap CONFIG_TENDONIN (standard PRF, output rotated left by 3
bytes). The derived key is accepted only if it reproduces message 2's MIC.
"""
import bisect
import collections
import hashlib
import hmac
import os
import pickle
import re
import struct
import subprocess
import sys

from cryptography.hazmat.primitives.ciphers.aead import AESCCM

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOCAL = os.path.join(REPO, '.local')
PCAP = os.path.join(LOCAL, 'wiiu', 'real-session.pcap')
CACHE = os.path.join(LOCAL, 'wiiu', 'real-session-udp.pkl')
VIDEO_DIR = os.path.join(LOCAL, 'wiiu', 'real-video')
SPS = bytes([0x67, 0x64, 0x00, 0x20, 0xac, 0x2b, 0x40, 0x6c, 0x1e, 0xf3, 0x68])
PPS = bytes([0x68, 0xee, 0x06, 0x0c, 0xe8])


# --- capture parsing --------------------------------------------------------------

def pcap_frames(path):
    with open(path, 'rb') as f:
        gh = f.read(24)
        e = '<' if struct.unpack('<I', gh[:4])[0] in (0xa1b2c3d4, 0xa1b23c4d) else '>'
        while True:
            h = f.read(16)
            if len(h) < 16:
                return
            ts, tu, incl, _ = struct.unpack(e + 'IIII', h)
            yield ts + tu / 1e6, f.read(incl)


def wlan(d):
    """802.11 frame from a radiotap record (ziiuctl writes no FCS)."""
    return d[struct.unpack('<H', d[2:4])[0]:]


def gamepad_mac():
    text = open(os.path.join(LOCAL, 'wiiu', 'ziiu-probe.txt'), encoding='utf-8', errors='replace').read()
    m = re.search(r'mac: r=0 dev=2 ([0-9a-fA-F:]{17})', text)
    if not m:
        sys.exit('GamePad MAC not found in .local/wiiu/ziiu-probe.txt')
    return bytes.fromhex(m.group(1).replace(':', ''))


def pmk():
    for line in open(os.path.join(LOCAL, 'gamepad-link', 'hostapd.conf')):
        if line.startswith('wpa_psk='):
            return bytes.fromhex(line.split('=', 1)[1].strip())
    sys.exit('no wpa_psk in .local/gamepad-link/hostapd.conf (run ziiuctl ap-hostapd once)')


# --- Nintendo WPA2 + CCMP -----------------------------------------------------------

def prf(key, label, data, n):
    out, i = b'', 0
    while len(out) < n:
        out += hmac.new(key, label + b'\0' + data + bytes([i]), hashlib.sha1).digest()
        i += 1
    return out[:n]


def ptk(pmk_, aa, spa, an, sn):
    t = prf(pmk_, b'Pairwise key expansion', min(aa, spa) + max(aa, spa) + min(an, sn) + max(an, sn), 48)
    return t[3:] + t[:3]   # CONFIG_TENDONIN: rotate left by 3 bytes


def ccmp_decrypt(tk, w):
    fc = w[0] | w[1] << 8
    qos = bool(fc & 0x80) and ((fc >> 2) & 3) == 2
    hdr = 26 if qos else 24
    if len(w) < hdr + 16:
        return None
    iv = w[hdr:hdr + 8]
    pn = bytes([iv[7], iv[6], iv[5], iv[4], iv[1], iv[0]])
    fcm = (fc & ~(0x0070 | 0x0800 | 0x1000 | 0x2000)) | 0x4000
    if qos:
        fcm &= ~0x8000
    aad = struct.pack('<H', fcm) + w[4:22] + struct.pack('<H', (w[22] | w[23] << 8) & 0x000f)
    prio = 0
    if qos:
        aad += bytes([w[24] & 0x0f, 0])
        prio = w[24] & 0x0f
    try:
        return AESCCM(tk, tag_length=8).decrypt(bytes([prio]) + w[10:16] + pn, bytes(w[hdr + 8:]), aad)
    except Exception:
        return None


def cmd_decrypt():
    pad = gamepad_mac()
    key = pmk()
    console = None
    eapol, frames, beacons = [], [], []
    for t, d in pcap_frames(PCAP):
        w = wlan(d)
        if len(w) < 24:
            continue
        fc = w[0] | w[1] << 8
        ftype, sub = (fc >> 2) & 3, (fc >> 4) & 15
        if ftype == 0 and sub == 8 and console and w[10:16] == console:
            beacons.append((t, struct.unpack('<Q', w[24:32])[0]))
            continue
        if ftype != 2 or pad not in (w[4:10], w[10:16]):
            continue
        peer = w[10:16] if w[4:10] == pad else w[4:10]
        console = console or peer
        if peer != console:
            continue
        hdr = 26 if sub & 8 else 24
        if not fc & 0x4000:
            if w[hdr:hdr + 8] == bytes.fromhex('aaaa03000000888e'):
                eapol.append((t, w[10:16] == console, w[hdr + 8:]))
            continue
        frames.append((t, w[10:16] == console, bytes(w)))
    keys = []
    for i, (t1, ap1, e1) in enumerate(eapol):
        ki1 = struct.unpack('>H', e1[5:7])[0]
        if not ap1 or not ki1 & 0x80 or ki1 & 0x100:
            continue
        for t2, ap2, e2 in eapol[i + 1:]:
            ki2 = struct.unpack('>H', e2[5:7])[0]
            if ap2 or not ki2 & 0x100 or ki2 & 0x40:
                continue
            p = ptk(key, console, pad, e1[17:49], e2[17:49])
            frm = bytearray(e2[:struct.unpack('>H', e2[2:4])[0] + 4])
            mic = bytes(frm[81:97])
            frm[81:97] = bytes(16)
            if hmac.new(p[:16], bytes(frm), hashlib.sha1).digest()[:16] == mic:
                keys.append((t2, p[32:48]))
            break
    print('console beacons %d, EAPOL frames %d, protected data %d, handshakes verified by MIC: %d' % (
        len(beacons), len(eapol), len(frames), len(keys)))
    if not keys:
        sys.exit('no verified handshake: power-cycle the GamePad while capturing')
    udp, bad = [], 0
    for t, down, w in frames:
        tk = None
        for kt, k in keys:
            if kt <= t + 1:
                tk = k
        if tk is None:
            continue
        plain = ccmp_decrypt(tk, w)
        if plain is None:
            bad += 1
            continue
        if plain[:8] != bytes.fromhex('aaaa030000000800'):
            continue
        ip = plain[8:]
        if len(ip) < 28 or ip[9] != 17:
            continue
        ihl = (ip[0] & 15) * 4
        sport, dport = struct.unpack('>HH', ip[ihl:ihl + 4])
        udp.append((t, down, sport, dport, bytes(ip[ihl + 8:]), bytes(ip[:ihl])))
    with open(CACHE, 'wb') as f:
        pickle.dump({'udp': udp, 'beacons': beacons}, f)
    print('UDP packets %d, decrypt failures %d -> %s' % (len(udp), bad, CACHE))


# --- analysis -----------------------------------------------------------------------

def load():
    return pickle.load(open(CACHE, 'rb'))


def cmd_protocol():
    d = load()
    udp, beacons = d['udp'], d['beacons']
    bt = [b[0] for b in beacons]
    t0 = udp[0][0]

    def tsf_at(t):
        i = bisect.bisect_right(bt, t) - 1
        return None if i < 0 else beacons[i][1] + int((t - beacons[i][0]) * 1e6)

    flows = collections.Counter((s, dp, down) for t, down, s, dp, p, ih in udp)
    print('flows (sport, dport, console->GamePad):', flows.most_common())
    print('IP TOS per destination port:',
          sorted(collections.Counter((dp, ih[1]) for t, down, s, dp, p, ih in udp if down).items()))
    print('== command exchange (first 40 packets on 50123/50023)')
    n = 0
    for t, down, s, dp, p, ih in udp:
        if dp in (50123, 50023) and n < 40:
            print('  +%.4f %s %s' % (t - t0, 'C->P' if down else 'P->C', p[:28].hex(' ')))
            n += 1
    uvc = collections.Counter(p[8:].hex(' ') for t, down, s, dp, p, ih in udp if down and dp == 50123 and p[0] == 0 and p[2] == 1)
    print('UVC/UAC state payloads:', uvc.most_common(3))
    st = [p for t, down, s, dp, p, ih in udp if dp == 50125]
    print('50125 messages: %d, distinct bodies %d, body %s' % (len(st), len({p[8:] for p in st}), st[0][8:].hex() if st else '-'))
    vf = collections.Counter((p[:8].hex(' '), p[12:].hex(' ')) for t, down, s, dp, p, ih in udp
                             if down and dp == 50121 and (p[0] >> 2) & 1)
    print('video-format packets (header, payload after ts):', vf.most_common(3))
    opts = collections.Counter(p[8:16].hex(' ') for t, down, s, dp, p, ih in udp if dp == 50120)
    print('vstrm option bytes:', opts.most_common(3))
    au = [p for t, down, s, dp, p, ih in udp if down and dp == 50121 and not (p[0] >> 2) & 1]
    print('audio: %d packets, sizes %s, byte0&0xfc %s' % (
        len(au), collections.Counter(len(p) for p in au).most_common(2),
        collections.Counter(p[0] & 0xfc for p in au).most_common(2)))
    for name, port, first in (('vstrm first packet', 50120, True), ('vstrm last packet', 50120, False)):
        v = []
        for t, down, s, dp, p, ih in udp:
            if dp == port and (p[2] & 0x40 if first else p[2] & 0x10):
                e = tsf_at(t)
                if e is not None:
                    v.append(((e & 0xffffffff) - struct.unpack('>I', p[4:8])[0] + 2**31) % 2**32 - 2**31)
        v.sort()
        if v:
            print('%s: capture TSF - stamp (us, includes capture jitter): p10 %d median %d p90 %d' % (
                name, v[len(v) // 10], v[len(v) // 2], v[len(v) * 9 // 10]))
    print('msg (GamePad -> 50010):', [(round(t - t0, 2), p.hex()) for t, down, s, dp, p, ih in udp if dp == 50010])


def cmd_video(ffmpeg):
    d = load()
    frames, cur, prev, gaps = [], None, None, 0
    for t, down, s, dp, p, ih in d['udp']:
        if dp != 50120:
            continue
        seq = ((p[0] & 3) << 8) | p[1]
        if prev is not None and seq != (prev + 1) % 1024:
            gaps += 1
            cur = None      # a packet the capture missed: drop that frame
        prev = seq
        if p[2] & 0x40:
            cur = {'idr': p[8] == 0x80, 'chunks': [b''], 'ends': 0}
        if cur is None:
            continue
        cur['chunks'][-1] += p[16:]
        if p[2] & 0x20:
            cur['ends'] += 1
            if not p[2] & 0x10:
                cur['chunks'].append(b'')
        if p[2] & 0x10:
            frames.append(cur)
            cur = None
    sizes = sorted(sum(len(c) for c in f['chunks']) for f in frames)
    n = len(sizes)
    print('frames %d (capture gaps %d), keyframes %d, bytes p10 %d median %d p90 %d max %d' % (
        n, gaps, sum(f['idr'] for f in frames), sizes[n // 10], sizes[n // 2], sizes[n * 9 // 10], sizes[-1]))
    os.makedirs(VIDEO_DIR, exist_ok=True)
    start = next((i for i, f in enumerate(frames) if f['idr']), 0)
    h264 = os.path.join(VIDEO_DIR, 'real.h264')
    with open(h264, 'wb') as f:
        f.write(b'\0\0\0\1' + SPS + b'\0\0\0\1' + PPS)
        fn = 0
        for fr in frames[start:]:
            if fr['idr']:
                fn, hdr = 0, bytes([0x25, 0xb8, 0x04, 0xff])
            else:
                fn = (fn + 1) & 0xff
                hdr = bytes([0x21, 0xe0 | (fn >> 3), 0x03 | ((fn << 5) & 0xff), 0xff])
            raw = hdr + b''.join(fr['chunks'])
            out = bytearray(raw[:2])
            for b in raw[2:]:
                if b <= 3 and out[-2] == 0 and out[-1] == 0:
                    out.append(3)
                out.append(b)
            f.write(b'\0\0\0\1' + bytes(out))
    r = subprocess.run([ffmpeg, '-v', 'error', '-y', '-i', h264, '-vf', 'select=not(mod(n' + chr(92) + ',300))',
                        '-vsync', '0', os.path.join(VIDEO_DIR, 'frame_%03d.png')], capture_output=True, text=True)
    errs = [l for l in r.stderr.splitlines() if l.strip()]
    print('ffmpeg exit %d, decoder messages %d; frames in %s' % (r.returncode, len(errs), VIDEO_DIR))


if __name__ == '__main__':
    what = sys.argv[1] if len(sys.argv) > 1 else ''
    if what == 'decrypt':
        cmd_decrypt()
    elif what == 'protocol':
        cmd_protocol()
    elif what == 'video' and len(sys.argv) > 2:
        cmd_video(sys.argv[2])
    else:
        print(__doc__)
