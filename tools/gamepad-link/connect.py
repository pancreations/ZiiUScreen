"""Bounded connection worker, launched by ziiuctl ap-connect with its handle.

This first transport milestone ends after a verified WPA message 4. It does not
yet provide encrypted IP/DHCP or video/input service. Never logs pairing keys.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import re
import struct
import time
from collections import Counter

from frames import Management, rx_frame, header
from wpa import Authenticator

ROOT = Path(__file__).resolve().parents[2]
LOCAL = ROOT / '.local' / 'gamepad-link'


def credentials():
    text = (ROOT / '.local/wiiu/ziiu-probe.txt').read_text()
    eeprom = bytearray()
    station = None
    drc = False
    for line in text.splitlines():
        if line.startswith('--- '):
            drc = 'DRC0' in line
        if drc and line.startswith('mac: r=0'):
            match = re.search(r'dev=2 ((?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2})', line)
            if match:
                station = bytes.fromhex(match[1].replace(':', ''))
        match = re.fullmatch(r'([0-9a-fA-F]{3}):((?: [0-9a-fA-F]{2})+)\s*', line)
        if match and int(match[1], 16) == len(eeprom):
            eeprom.extend(bytes.fromhex(match[2]))
    if station is None or len(eeprom) < 0x85 or not 1 <= eeprom[0x21] <= 32 or eeprom[0x65] != 64:
        raise ValueError('Saved pairing data is incomplete')
    return bytes(eeprom[0x7f:0x85]), station, bytes(eeprom[1:1 + eeprom[0x21]]), bytes.fromhex(eeprom[0x25:0x65].decode('ascii'))


class Radio:
    def __init__(self, handle):
        self.handle = wintypes.HANDLE(handle)
        self.call = ctypes.WinDLL('kernel32', use_last_error=True).DeviceIoControl
        self.call.argtypes = (wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                              ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p)
        self.call.restype = wintypes.BOOL
        self.seq = 0
        self.counts = Counter()
        self.last_receive_ns = None
        self.send_timings = []
        self.capture = (LOCAL / 'raw-rx.bin').open('wb')
        info = self.ioctl(0, b'', 36)
        if len(info) != 36 or struct.unpack_from('<I', info)[0] != 5:
            raise ValueError('Driver interface 5 required')

    def ioctl(self, number, payload=b'', capacity=0):
        src = ctypes.create_string_buffer(payload) if payload else None
        dst = ctypes.create_string_buffer(capacity) if capacity else None
        got = wintypes.DWORD()
        code = (0x22 << 16) | ((0x800 + number) << 2)
        if not self.call(self.handle, code, src, len(payload), dst, capacity, ctypes.byref(got), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return dst.raw[:got.value] if dst else b''

    def send(self, frame):
        started = time.perf_counter_ns()
        self.ioctl(10, struct.pack('<IIII', len(frame), 1, 0, 0) + frame)
        finished = time.perf_counter_ns()
        self.send_timings.append({'ioctl_ms': round((finished - started) / 1e6, 3),
                                 'after_receive_ms': round((started - self.last_receive_ns) / 1e6, 3)
                                 if self.last_receive_ns is not None else None})

    def tx_stats(self):
        fields = ('queued', 'released', 'busy_mask', 'reports', 'acked', 'last_status', 'last_packet_id')
        return dict(zip(fields, struct.unpack('<7I', self.ioctl(11, b'', 28))))

    def tx_ring(self):
        result = {}
        for name, address in (('cpu', 0xd4308), ('dma', 0xd430c), ('global', 0xd4208)):
            raw = self.ioctl(7, struct.pack('<IIII', address, 0, 0, 0), 8)
            result[name] = struct.unpack('<II', raw)[1]
        return result

    def rx_stats(self):
        values = struct.unpack('<290I', self.ioctl(12, b'', 1160))
        return {'types': {i: n for i, n in enumerate(values[:32]) if n},
                'events': {i: n for i, n in enumerate(values[32:288]) if n},
                'last_free_header': values[288], 'last_free_version': values[289]}

    def receive(self):
        started = time.perf_counter_ns()
        # Kernel KeDelayExecutionThread(1 ms) can sleep for a full ~15.6 ms
        # tick. Poll without holding its hardware lock across those sleeps;
        # Python on Windows uses a high-resolution waitable timer for sleep.
        raw = self.ioctl(8, struct.pack('<I', 0), 256 * 1024)
        self.last_receive_ns = time.perf_counter_ns()
        self.counts['rx_calls'] += 1
        self.counts['rx_call_us_total'] += (self.last_receive_ns - started) // 1000
        self.counts['rx_call_us_max'] = max(self.counts['rx_call_us_max'],
                                           (self.last_receive_ns - started) // 1000)
        if not raw:
            time.sleep(0.001)
            return
        at = 0
        while at + 4 <= len(raw):
            size = struct.unpack_from('<I', raw, at)[0]
            at += 4
            if size > len(raw) - at:
                raise ValueError('Truncated driver RX record')
            record = raw[at:at + size]
            self.counts['records'] += 1
            if size >= 4:
                self.counts['type_' + str(struct.unpack_from('<I', record)[0] >> 27)] += 1
            if self.capture.tell() < 8 * 1024 * 1024:
                self.capture.write(struct.pack('<I', size) + record)
                self.capture.flush()
            frame = rx_frame(record)
            if frame is not None:
                self.counts['decoded'] += 1
                yield frame
            at += (size + 3) & ~3

    def station(self, address):
        # Legacy 5 GHz peer, no RX header translation/aggregation. MT7925's
        # unified station update owns WTBL programming; no raw register writes.
        def tlv(tag, body):
            return struct.pack('<HH', tag, len(body) + 4) + body
        parts = [tlv(0, struct.pack('<IBBH6sH', 0x10001, 2, 0, 1, address, 3)),
                 tlv(0x15, struct.pack('<HBBBBBB', 0x15, 8, 0, 0, 0, 0, 0)),
                 tlv(1, struct.pack('<H', 0xff0) + bytes(10)),
                 tlv(7, bytes([1]) + bytes(11)),
                 # Even a non-MLO peer gets a single-link mapping in mt7925.
                 tlv(0x20, address + struct.pack('<HHH', 1, 0, 1) + bytes([1, 0, 0, 0])
                     + struct.pack('<HBB', 1, 0, 0) + bytes(4)),
                 tlv(0x2b, bytes([1, 0, 1, 0]))]
        payload = struct.pack('<BBHBBBB', 0, 1, len(parts), 1, 0, 0, 0) + b''.join(parts)
        self.seq = self.seq % 15 + 1
        message = bytearray(48 + len(payload))
        struct.pack_into('<II', message, 0, len(message) | (2 << 23) | (0x20 << 25), 1 << 14)
        struct.pack_into('<HH', message, 32, len(message) - 32, 3)
        message[37], message[39], message[43] = 0xa0, self.seq, 7
        message[48:] = payload
        result = self.ioctl(5, struct.pack('<IIII', 1, self.seq, 3000, len(message)) + message, 16384)
        if len(result) < 52 or struct.unpack_from('<I', result, 48)[0] != 0:
            raise ValueError('Firmware refused station setup')


def run(handle, seconds, channel, check_only=False, tx_check=False):
    LOCAL.mkdir(parents=True, exist_ok=True)
    with (LOCAL / 'connection-events.jsonl').open('w', encoding='utf-8', buffering=1) as log:
        def event(name, **details):
            log.write(json.dumps({'event': name, **details}) + '\n')
        try:
            ap, station, ssid, pmk = credentials()
            radio = Radio(handle)
            if check_only:
                event('transport_check_passed', driver_interface=5, credentials_valid=True)
                return 0
            radio.station(station)
            management = Management(ap, station, ssid, channel)
            if tx_check:
                # A single directed probe RESPONSE, no wildcard broadcast and no
                # association changes. DMA completion must happen even without ACK.
                probe = header(0x40, ap, station, ap, 0) + bytes([0, len(ssid)]) + ssid
                _, reply = management.receive(probe)
                radio.send(reply)
                end = time.monotonic() + min(seconds, 5)
                while time.monotonic() < end:
                    list(radio.receive())
                event('tx_check', ring=radio.tx_ring(), transmit=radio.tx_stats(), receive=dict(radio.counts), diagnostics=radio.rx_stats(), send_timings=radio.send_timings)
                return 0 if radio.tx_stats()['released'] else 1
            auth = None
            last_sent, retries, pending = 0, 0, None
            stale_resets, last_reset = 0, 0
            deadline = time.monotonic() + seconds
            report_at = time.monotonic() + 5
            event('ready', channel=channel, seconds=seconds)
            while time.monotonic() < deadline:
                for frame in radio.receive():
                    try:
                        if management.stale_session(frame) and stale_resets < 3 and time.monotonic() - last_reset >= 2:
                            radio.send(management.reset_stale_session())
                            stale_resets += 1
                            last_reset = time.monotonic()
                            event('stale_session_reset', attempt=stale_resets)
                        name, reply = management.receive(frame)
                        if reply:
                            radio.send(reply)
                        if name:
                            event(name)
                        if name == 'associated':
                            if auth is None:
                                auth = Authenticator(pmk, ap, station, station_rsn=management.station_rsn)
                                pending = auth.message1
                                last_sent, retries = 0, 0
                        elif name == 'disconnected':
                            auth = pending = None
                        raw = management.read_eapol(frame) if management.associated else None
                        if raw is not None and auth is not None:
                            pending = auth.receive(raw)
                            event('wpa', state=auth.state)
                            last_sent, retries = 0, 0
                            if auth.state == 'connected':
                                event('handshake_verified', encrypted_ip_available=False)
                                return 0
                    except ValueError as error:
                        event('rejected', reason=str(error))
                if time.monotonic() >= report_at:
                    event('radio_counts', receive=dict(radio.counts), transmit=radio.tx_stats(), diagnostics=radio.rx_stats())
                    report_at = time.monotonic() + 5
                if pending and time.monotonic() - last_sent >= 1:
                    if retries == 4:
                        event('handshake_timeout')
                        return 1
                    radio.send(management.eapol(pending))
                    last_sent = time.monotonic()
                    retries += 1
            event('timeout', phase=auth.state if auth else 'awaiting_authentication', receive_counts=dict(radio.counts), transmit=radio.tx_stats(), diagnostics=radio.rx_stats(), send_timings=radio.send_timings)
            return 1
        except Exception as error:
            event('error', reason=str(error))
            return 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--handle', type=int, required=True)
    parser.add_argument('--seconds', type=int, choices=range(1, 61), required=True)
    parser.add_argument('--channel', type=int, choices=(36, 40, 44, 48, 149, 153, 157, 161, 165), required=True)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--tx-check', action='store_true')
    args = parser.parse_args()
    raise SystemExit(run(args.handle, args.seconds, args.channel, args.check, args.tx_check))
