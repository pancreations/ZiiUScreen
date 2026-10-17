import unittest
from cryptography.hazmat.primitives.keywrap import aes_key_unwrap
from wpa import Authenticator, KeyFrame, derive_ptk, key_frame, RSN, valid_station_rsn

PMK = bytes(range(32))
AP = bytes.fromhex('020000000001')
STA = bytes.fromhex('020000000002')
ANONCE, SNONCE = bytes(range(32, 64)), bytes(range(64, 96))
PTK = bytes.fromhex('7c0c18ae4392b1831fa7b4c47113691431e82fe693967325c35c5547178ec6663d093eb76401bdfc77636464999be6b0')


class HandshakeTests(unittest.TestCase):
    def auth(self):
        return Authenticator(PMK, AP, STA, nonce=ANONCE, gtk=bytes(range(16)))

    def message2(self, **kwargs):
        return key_frame(kwargs.get('info', 0x010a), kwargs.get('replay', 1),
                         SNONCE, kwargs.get('data', RSN), kwargs.get('kck', PTK[:16]))

    def test_derivation_and_rotation(self):
        self.assertEqual(derive_ptk(PMK, AP, STA, ANONCE, SNONCE), PTK)
        self.assertEqual(derive_ptk(PMK, STA, AP, SNONCE, ANONCE), PTK)
        standard = derive_ptk(PMK, AP, STA, ANONCE, SNONCE, nintendo=False)
        self.assertEqual(standard[3:] + standard[:3], PTK)

    def test_complete_handshake_and_wrapped_gtk(self):
        auth = self.auth()
        m1 = KeyFrame.parse(auth.message1)
        self.assertEqual((m1.info, m1.replay, m1.nonce), (0x008a, 1, ANONCE))
        with self.assertRaises(ValueError):
            auth.keys_for_install()
        m2 = self.message2()
        raw3 = auth.receive(m2)
        m3 = KeyFrame.parse(raw3)
        self.assertEqual((m3.info, m3.replay), (0x13ca, 2))
        self.assertTrue(m3.valid_mic(PTK[:16]))
        plain = aes_key_unwrap(PTK[16:32], m3.data)
        self.assertEqual(plain, RSN + bytes.fromhex('dd16000fac010100') + bytes(range(16)) + b'\xdd\x00')
        self.assertEqual(auth.receive(m2), raw3)
        with self.assertRaises(ValueError):
            auth.keys_for_install()
        auth.receive(key_frame(0x030a, 2, bytes(32), kck=PTK[:16]))
        self.assertEqual(auth.keys_for_install(), (PTK[32:], bytes(range(16))))
        self.assertIsNone(auth.receive(m2))

    def test_bad_mic_replay_and_cipher_never_advance(self):
        for packet in (self.message2(kck=bytes(16)), self.message2(replay=0),
                       self.message2(info=0x018a), self.message2(data=RSN.replace(b'\xa4\xc0\xe1', b'\x00\x0f\xac'))):
            auth = self.auth()
            with self.assertRaises(ValueError):
                auth.receive(packet)
            self.assertEqual(auth.state, 'awaiting_m2')

    def test_bad_message4_does_not_authorize(self):
        for packet in (key_frame(0x030a, 1, bytes(32), kck=PTK[:16]),
                       key_frame(0x030a, 2, bytes(32), kck=bytes(16)),
                       key_frame(0x030a, 2, SNONCE, kck=PTK[:16])):
            auth = self.auth()
            auth.receive(self.message2())
            with self.assertRaises(ValueError):
                auth.receive(packet)
            self.assertEqual(auth.state, 'awaiting_m4')

    def test_truncated_and_trailing_frames(self):
        raw = self.message2()
        for i in range(len(raw)):
            with self.assertRaises(ValueError):
                KeyFrame.parse(raw[:i])
        with self.assertRaises(ValueError):
            KeyFrame.parse(raw + b'\x00')

    def test_station_capabilities_bound_to_message2(self):
        station_rsn = RSN[:-2] + b'\x00\x00'
        self.assertTrue(valid_station_rsn(station_rsn))
        self.assertFalse(valid_station_rsn(RSN[:-2] + b'\x40\x00'))
        auth = Authenticator(PMK, AP, STA, nonce=ANONCE, station_rsn=station_rsn)
        with self.assertRaises(ValueError):
            auth.receive(self.message2())
        reply = auth.receive(self.message2(data=station_rsn))
        self.assertTrue(KeyFrame.parse(reply).valid_mic(PTK[:16]))


if __name__ == '__main__':
    unittest.main()
