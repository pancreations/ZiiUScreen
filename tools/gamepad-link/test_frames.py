import struct
import unittest
from frames import Management, header, elements, rx_frame, LLC_EAPOL
from wpa import RSN

AP, STA, OTHER = (bytes.fromhex(x) for x in ('020000000001', '020000000002', '020000000003'))
SSID = b'WiiUtest'


class FrameTests(unittest.TestCase):
    def test_only_saved_peer_and_directed_probes(self):
        m = Management(AP, STA, SSID, 157)
        for peer, name in ((OTHER, SSID), (STA, b''), (STA, b'wrong')):
            frame = header(0x40, b'\xff' * 6, peer, b'\xff' * 6, 0) + bytes([0, len(name)]) + name
            self.assertEqual(m.receive(frame), (None, None))
        frame = header(0x40, b'\xff' * 6, STA, b'\xff' * 6, 0) + bytes([0, len(SSID)]) + SSID
        event, reply = m.receive(frame)
        self.assertEqual(event, 'probe')
        self.assertEqual(reply[4:10], STA)

    def test_auth_before_association_and_rsn_match(self):
        m = Management(AP, STA, SSID, 157)
        assoc = header(0, AP, STA, AP, 0) + bytes(4) + bytes([0, len(SSID)]) + SSID + RSN
        self.assertEqual(m.receive(assoc)[0], 'association-rejected')
        auth = header(0xb0, AP, STA, AP, 1) + struct.pack('<HHH', 0, 1, 0)
        self.assertEqual(m.receive(auth)[0], 'auth')
        self.assertEqual(m.receive(assoc)[0], 'associated')
        self.assertTrue(m.associated)
        m.receive(header(0xc0, AP, STA, AP, 2) + bytes(2))
        self.assertFalse(m.authenticated)
        self.assertFalse(m.associated)

    def test_eapol_qos_and_direction(self):
        m = Management(AP, STA, SSID, 157)
        data = bytes.fromhex('02000000')
        raw = header(0x0188, AP, STA, AP, 0) + bytes(2) + LLC_EAPOL + data
        self.assertEqual(m.read_eapol(raw), data)
        self.assertIsNone(m.read_eapol(header(0x0288, AP, STA, AP, 0) + raw[24:]))
        self.assertIsNone(m.read_eapol(header(0x4188, AP, STA, AP, 0) + raw[24:]))

    def test_rx_bounds_and_groups(self):
        f = header(0xb0, AP, STA, AP, 0)
        record = bytearray(32 + 16 + 4 + len(f))
        struct.pack_into('<III', record, 0, len(record) | (2 << 27), 1 << 19, 2 << 13)
        record[-len(f):] = f
        self.assertEqual(rx_frame(record), f)
        for n in range(len(record)):
            self.assertIsNone(rx_frame(record[:n]))

    def test_invalid_information_elements(self):
        for raw in (b'\x00', b'\x00\x02x', b'\x00\x00\x00\x00'):
            with self.assertRaises(ValueError):
                elements(raw)

    def test_reset_only_saved_stale_peer(self):
        m = Management(AP, STA, SSID, 36)
        stale = header(0x4188, AP, STA, AP, 0) + bytes(20)
        self.assertTrue(m.stale_session(stale))
        self.assertFalse(m.stale_session(header(0x4188, AP, OTHER, AP, 0) + bytes(20)))
        reply = m.reset_stale_session()
        self.assertEqual(reply[0], 0xc0)
        self.assertEqual(reply[4:10], STA)
        self.assertEqual(reply[24:], b'\x07\x00')
        m.associated = True
        self.assertFalse(m.stale_session(stale))


if __name__ == '__main__':
    unittest.main()
