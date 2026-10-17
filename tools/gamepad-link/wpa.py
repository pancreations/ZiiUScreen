"""Nintendo WPA2-PSK/CCMP primitives and a bounded authenticator.

Wire behavior follows drc-hostap's CONFIG_TENDONIN (three-byte PTK rotation).
No radio access here. Keys are never logged. cryptography supplies AES key wrap.
"""
import hmac
import secrets
import struct
from dataclasses import dataclass

RSN = bytes.fromhex('30140100a4c0e1040100a4c0e1040100a4c0e1020800')
PAIRWISE, INSTALL, ACK, MIC, SECURE, ENCRYPTED = 8, 64, 128, 256, 512, 4096


def valid_station_rsn(rsn):
    # Same selected suites; replay-counter capability bits may differ from AP.
    # No PMF/preauthentication/GTK-negotiation extensions in this implementation.
    return (len(rsn) in (22, 24) and rsn[0] == 48 and rsn[1] == len(rsn) - 2
            and rsn[2:20] == RSN[2:20]
            and int.from_bytes(rsn[20:22], 'little') & ~0x003c == 0
            and (len(rsn) == 22 or rsn[22:] == b'\x00\x00'))


def derive_ptk(pmk, ap, station, anonce, snonce, *, nintendo=True):
    if tuple(map(len, (pmk, ap, station, anonce, snonce))) != (32, 6, 6, 32, 32):
        raise ValueError('Invalid key, address or nonce length')
    data = min(ap, station) + max(ap, station) + min(anonce, snonce) + max(anonce, snonce)
    prefix = b'Pairwise key expansion\0' + data
    ptk = b''.join(hmac.digest(pmk, prefix + bytes([i]), 'sha1') for i in range(3))[:48]
    return ptk[3:] + ptk[:3] if nintendo else ptk


@dataclass(frozen=True, repr=False)
class KeyFrame:
    raw: bytes
    info: int
    replay: int
    nonce: bytes
    data: bytes

    @classmethod
    def parse(cls, raw):
        raw = bytes(raw)
        if len(raw) < 99 or raw[0] not in (1, 2) or raw[1] != 3 or raw[4] != 2:
            raise ValueError('Not a supported EAPOL-Key frame')
        length = int.from_bytes(raw[2:4], 'big')
        data_len = int.from_bytes(raw[97:99], 'big')
        if length != len(raw) - 4 or data_len != len(raw) - 99:
            raise ValueError('Invalid EAPOL lengths')
        info = int.from_bytes(raw[5:7], 'big')
        if info & 7 != 2:
            raise ValueError('Only HMAC-SHA1/AES WPA2 key descriptor v2 is supported')
        return cls(raw, info, int.from_bytes(raw[9:17], 'big'), raw[17:49], raw[99:])

    def valid_mic(self, kck):
        unsigned = self.raw[:81] + bytes(16) + self.raw[97:]
        return hmac.compare_digest(self.raw[81:97], hmac.digest(kck, unsigned, 'sha1')[:16])


def key_frame(info, replay, nonce, data=b'', kck=None, version=2):
    if len(nonce) != 32 or not 0 <= replay < 2**64 or len(data) > 4096:
        raise ValueError('Invalid EAPOL-Key fields')
    raw = bytearray(99 + len(data))
    raw[:5] = bytes([version, 3]) + struct.pack('>H', len(raw) - 4) + b'\x02'
    struct.pack_into('>HHQ', raw, 5, info, 16, replay)
    raw[17:49] = nonce
    struct.pack_into('>H', raw, 97, len(data))
    raw[99:] = data
    if kck is not None:
        raw[81:97] = hmac.digest(kck, raw, 'sha1')[:16]
    return bytes(raw)


class Authenticator:
    """One station, one association. Caller owns retries/timeouts and radio TX.

    Pairwise key is available for installation only after verified message 4.
    Repeated message 2 returns the identical message 3; it never rotates keys.
    """
    def __init__(self, pmk, ap, station, *, nonce=None, gtk=None, replay=1, station_rsn=RSN):
        self._pmk, self.ap, self.station = pmk, ap, station
        self._anonce = nonce if nonce is not None else secrets.token_bytes(32)
        self._gtk = gtk if gtk is not None else secrets.token_bytes(16)
        if len(self._gtk) != 16 or not 1 <= replay < 2**64 - 1:
            raise ValueError('Invalid GTK or initial replay counter')
        derive_ptk(pmk, ap, station, self._anonce, bytes(32))  # validate lengths
        self.replay = replay
        if not valid_station_rsn(station_rsn):
            raise ValueError('Unsupported station RSN')
        self._station_rsn = station_rsn
        self.state = 'awaiting_m2'
        self._ptk = self._m2 = self._m3 = None
        self.message1 = key_frame(2 | PAIRWISE | ACK, replay, self._anonce)

    def receive(self, raw):
        msg = KeyFrame.parse(raw)
        if self.state in ('awaiting_m4', 'connected') and raw == self._m2:
            return self._m3 if self.state == 'awaiting_m4' else None
        if self.state == 'awaiting_m2':
            if msg.info != (2 | PAIRWISE | MIC) or msg.replay != self.replay:
                raise ValueError('Unexpected message 2 flags/replay')
            if msg.nonce == bytes(32) or msg.data != self._station_rsn:
                raise ValueError('Invalid station nonce or mismatched Nintendo RSN')
            ptk = derive_ptk(self._pmk, self.ap, self.station, self._anonce, msg.nonce)
            if not msg.valid_mic(ptk[:16]):
                raise ValueError('Message 2 MIC mismatch')
            from cryptography.hazmat.primitives.keywrap import aes_key_wrap
            # Standard GTK KDE OUI is unchanged by CONFIG_TENDONIN.
            plain = RSN + bytes.fromhex('dd16000fac010100') + self._gtk
            if len(plain) % 8:
                plain += b'\xdd' + bytes((-len(plain) - 1) % 8)
            encrypted = aes_key_wrap(ptk[16:32], plain)
            self._m3 = key_frame(2 | PAIRWISE | INSTALL | ACK | MIC | SECURE | ENCRYPTED,
                                 self.replay + 1, self._anonce, encrypted, ptk[:16], msg.raw[0])
            self._ptk, self._m2 = ptk, bytes(raw)
            self.state = 'awaiting_m4'
            return self._m3
        if self.state == 'awaiting_m4':
            if msg.info != (2 | PAIRWISE | MIC | SECURE) or msg.replay != self.replay + 1:
                raise ValueError('Unexpected message 4 flags/replay')
            if msg.data or msg.nonce != bytes(32) or not msg.valid_mic(self._ptk[:16]):
                raise ValueError('Invalid message 4')
            self.state = 'connected'
            return None
        raise ValueError('Handshake already completed')

    def keys_for_install(self):
        if self.state != 'connected':
            raise ValueError('Station has not proved possession of the key')
        return self._ptk[32:48], self._gtk
