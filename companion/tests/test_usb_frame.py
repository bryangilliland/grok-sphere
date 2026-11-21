import unittest
from thad_companion.usb_frame import FRAME_JSON, FRAME_PCM, MAX_PAYLOAD, UsbFrameRx, crc16_ccitt, encode_frame


class TestUsbFrame(unittest.TestCase):
    def test_round_trip_and_byte_streaming(self):
        payload = b'{"type":"ping"}'
        raw = encode_frame(FRAME_JSON, payload)
        self.assertEqual(raw[:4], b"GST1")
        self.assertEqual(len(raw), len(payload) + 10)

        rx = UsbFrameRx()
        got = []
        noisy = b"ESP-ROM:boot log\r\nGSx" + raw
        for i in range(len(noisy)):
            rx.feed(noisy[i : i + 1], lambda ft, pl: got.append((ft, pl)))
        self.assertEqual(got, [(FRAME_JSON, payload)])

    def test_bad_crc_rejected_and_resyncs(self):
        good = encode_frame(FRAME_PCM, b"\x01\x02\x03\x04")
        bad = bytearray(good)
        bad[-1] ^= 0xFF

        rx = UsbFrameRx()
        got = []
        rx.feed(bytes(bad) + good, lambda ft, pl: got.append((ft, pl)))
        self.assertEqual(got, [(FRAME_PCM, b"\x01\x02\x03\x04")])

    def test_bounds(self):
        with self.assertRaises(ValueError):
            encode_frame(99, b"abc")
        with self.assertRaises(ValueError):
            encode_frame(FRAME_PCM, b"x" * (MAX_PAYLOAD + 1))
        self.assertEqual(crc16_ccitt(b"123456789"), 0x29B1)


if __name__ == "__main__":
    unittest.main()
