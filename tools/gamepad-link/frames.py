"""Bounded 802.11 management and MT7925 RX parsing; no hardware access."""
import struct
from wpa import RSN, valid_station_rsn

RATES = bytes.fromhex('01088c129824b048606c')
LLC_EAPOL = bytes.fromhex('aaaa03000000888e')


def elements(data):
    result = {}
    while data:
        if len(data) < 2 or data[1] + 2 > len(data):
            raise ValueError('Truncated information element')
        tag, size = data[:2]
        if tag in result and tag in (0, 1, 48):
            raise ValueError('Duplicate required information element')
        result[tag] = data[2:2 + size]
        data = data[2 + size:]
    return result


def rx_frame(record):
    if len(record) < 32:
        return None
    r0, r1, r2, _ = struct.unpack_from('<IIII', record)
    length = r0 & 0xffff
    if length > len(record) or length < 32 or (r0 >> 27) != 2 or r2 & (1 << 7):
        return None
    offset = 32
    for bit, size in ((19, 16), (16, 16), (17, 16), (18, 16), (20, 96)):
        if r1 & (1 << bit):
            offset += size
    offset += 2 * ((r2 >> 13) & 7)
    if offset + 24 > length or r1 & ((1 << 25) | (1 << 26)):
        return None
    return record[offset:length]


def header(fc, receiver, sender, bssid, sequence):
    return struct.pack('<HH', fc, 0) + receiver + sender + bssid + struct.pack('<H', (sequence & 4095) << 4)


class Management:
    def __init__(self, ap, station, ssid, channel):
        if len(ap) != 6 or len(station) != 6 or not 1 <= len(ssid) <= 32:
            raise ValueError('Invalid network identity')
        self.ap, self.station, self.ssid, self.channel = ap, station, ssid, channel
        self.sequence = 0
        self.authenticated = self.associated = False
        self.station_rsn = None

    def packet(self, fc, body):
        self.sequence = (self.sequence + 1) & 4095
        return header(fc, self.station, self.ap, self.ap, self.sequence) + body

    def receive(self, frame):
        if len(frame) < 24 or frame[10:16] != self.station:
            return None, None
        fc = int.from_bytes(frame[:2], 'little')
        if fc & 0x040f or fc & 0xc000 or frame[22] & 15:
            return None, None
        subtype = (fc >> 4) & 15
        if subtype == 4:
            if frame[4:10] not in (self.ap, b'\xff' * 6):
                return None, None
            ie = elements(frame[24:])
            if ie.get(0) != self.ssid:  # hidden network: ignore wildcard probes
                return None, None
            body = bytes(8) + struct.pack('<HH', 100, 0x11) + bytes([0, len(self.ssid)]) + self.ssid + RATES + RSN
            return 'probe', self.packet(0x50, body)
        if frame[4:10] != self.ap or frame[16:22] != self.ap:
            return None, None
        if subtype == 11 and len(frame) >= 30:
            algorithm, transaction, status = struct.unpack_from('<HHH', frame, 24)
            if transaction != 1 or status:
                return None, None
            accepted = algorithm == 0
            if accepted:
                self.authenticated = True
            return 'auth' if accepted else 'unsupported-auth', self.packet(0xb0, struct.pack('<HHH', algorithm, 2, 0 if accepted else 13))
        if subtype in (0, 2):
            fixed = 4 if subtype == 0 else 10
            if len(frame) < 24 + fixed:
                raise ValueError('Truncated association request')
            ie = elements(frame[24 + fixed:])
            station_rsn = bytes([48, len(ie.get(48, b''))]) + ie.get(48, b'')
            accepted = self.authenticated and ie.get(0) == self.ssid and valid_station_rsn(station_rsn)
            self.associated = accepted
            self.station_rsn = station_rsn if accepted else None
            body = struct.pack('<HHH', 0x11, 0 if accepted else 13, 0xc001 if accepted else 0) + RATES
            return 'associated' if accepted else 'association-rejected', self.packet(0x10 if subtype == 0 else 0x30, body)
        if subtype in (10, 12):
            self.associated = False
            if subtype == 12:
                self.authenticated = False
            return 'disconnected', None
        return None, None

    def eapol(self, payload):
        return self.packet(0x0208, LLC_EAPOL + payload)

    def stale_session(self, frame):
        if len(frame) < 24 or self.associated:
            return False
        fc = int.from_bytes(frame[:2], 'little')
        return (fc & 0x030f == 0x0108 and frame[4:10] == self.ap
                and frame[10:16] == self.station)

    def reset_stale_session(self):
        self.authenticated = self.associated = False
        return self.packet(0xc0, struct.pack('<H', 7))  # class-3 frame from nonassociated STA

    def read_eapol(self, frame):
        if len(frame) < 32 or frame[4:10] != self.ap or frame[10:16] != self.station:
            return None
        fc = int.from_bytes(frame[:2], 'little')
        if fc & 0x430f != 0x0108 or fc & 0x8400 or frame[22] & 15:
            return None
        offset = 26 if fc & 0x80 else 24
        if frame[offset:offset + 8] != LLC_EAPOL:
            return None
        raw = frame[offset + 8:]
        if len(raw) < 4:
            raise ValueError('Truncated EAPOL')
        length = int.from_bytes(raw[2:4], 'big') + 4
        if length > len(raw):
            raise ValueError('Truncated EAPOL payload')
        return raw[:length]
