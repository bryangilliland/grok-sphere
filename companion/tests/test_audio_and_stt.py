import unittest
import numpy as np
from thad_companion.audio import Endpointer, Rechunker, int16_to_pcm, pcm_to_int16, split_frames
from thad_companion.speech_text import clean_for_speech
from thad_companion.stt import clean_transcript, match_wake_phrase


class TestAudioAndStt(unittest.TestCase):
    def test_endpointer_speech_then_silence(self):
        ep = Endpointer(end_silence_s=0.5, max_seconds=5.0, no_speech_timeout_s=2.0)
        speech = (np.ones(1600, dtype=np.int16) * 2000)
        quiet = np.zeros(1600, dtype=np.int16)

        for _ in range(3):
            res = ep.push(speech)
            self.assertFalse(res.done)
        self.assertTrue(ep.heard)

        done = False
        for _ in range(6):
            res = ep.push(quiet)
            if res.done:
                done = True
                self.assertEqual(res.reason, "silence")
                break
        self.assertTrue(done)

    def test_endpointer_no_speech_timeout(self):
        ep = Endpointer(end_silence_s=0.5, max_seconds=5.0, no_speech_timeout_s=0.4)
        quiet = np.zeros(1600, dtype=np.int16)
        reasons = [ep.push(quiet).reason for _ in range(5)]
        self.assertIn("no_speech", reasons)

    def test_rechunker_and_split_frames(self):
        rc = Rechunker(1280)
        out = rc.push(np.arange(3000, dtype=np.int16))
        self.assertEqual(len(out), 2)
        self.assertEqual(len(out[0]), 1280)

        pcm = int16_to_pcm(np.arange(1000, dtype=np.int16))
        self.assertEqual(pcm_to_int16(pcm).size, 1000)
        frames = list(split_frames(pcm, 800))
        self.assertEqual([len(f) for f in frames], [800, 800, 400])

    def test_wake_phrase_matching(self):
        self.assertEqual(match_wake_phrase("Hey Thad, what's the weather?"), (True, "what's the weather?"))
        self.assertEqual(match_wake_phrase("Hey Thad."), (True, ""))
        self.assertEqual(match_wake_phrase("Hi dad, set a timer"), (True, "set a timer"))
        self.assertEqual(match_wake_phrase("Let's grab lunch"), (False, "Let's grab lunch"))
        self.assertEqual(clean_transcript("[BLANK_AUDIO] Hello (cough) world"), "Hello world")

    def test_clean_for_speech(self):
        txt = "**Bold** answer:\n- Check https://example.com\n- Ready!"
        cleaned = clean_for_speech(txt, 300)
        self.assertNotIn("**", cleaned)
        self.assertNotIn("https://", cleaned)


if __name__ == "__main__":
    unittest.main()
