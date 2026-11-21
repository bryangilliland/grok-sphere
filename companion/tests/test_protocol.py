import json
import unittest
from thad_companion import protocol as P


class TestProtocol(unittest.TestCase):
    def test_state_and_clipping(self):
        s = json.loads(P.state("listening", "Thad", "x" * 150))
        self.assertEqual(s["type"], "state")
        self.assertEqual(s["state"], "listening")
        self.assertEqual(s["label"], "Thad")
        self.assertLessEqual(len(s["text"].encode("utf-8")), P.MAX_TEXT)

        with self.assertRaises(P.ProtocolError):
            P.state("offline")

    def test_board_messages(self):
        m = P.parse_board_message('{"type":"hello","sample_rate":16000,"channels":1}')
        self.assertEqual(m["type"], "hello")
        m = P.parse_board_message('{"type":"touch","action":"ptt_start"}')
        self.assertEqual(m["action"], "ptt_start")

        with self.assertRaises(P.ProtocolError):
            P.parse_board_message('{"type":"touch","action":"swipe"}')
        with self.assertRaises(P.ProtocolError):
            P.parse_board_message('{"type":"hello","sample_rate":44100}')


if __name__ == "__main__":
    unittest.main()
