import asyncio
import json
import unittest
from pathlib import Path
import numpy as np

from thad_companion.config import Config
from thad_companion.hub import BridgeHub
from thad_companion.session import Engines, Session
from thad_companion.wakeword import HeyThadVadWake
from thad_companion.ws_transport import is_lan_address


class FakeSTT:
    def __init__(self, outputs):
        self.outputs = list(outputs)
        self.calls = 0

    def transcribe(self, samples, initial_prompt=None):
        self.calls += 1
        return self.outputs.pop(0) if self.outputs else ""


class FakeTTS:
    def __init__(self):
        self.spoken = []

    def synth(self, text):
        self.spoken.append(text)
        return b"\x01\x00" * 1600  # 100 ms of PCM


class FakeClaude:
    def __init__(self, reply="It is sunny and 72 in Fort Worth."):
        self.reply = reply
        self.prompts = []

    def ask(self, text):
        self.prompts.append(text)
        return self.reply


class TestSessionAndHub(unittest.IsolatedAsyncioTestCase):
    def _make_session(self, stt_outputs, wake=None):
        cfg = Config(root_dir=Path("."), chime=False, end_silence_s=0.2, no_speech_timeout_s=2.0)
        stt = FakeSTT(stt_outputs)
        tts = FakeTTS()
        claude = FakeClaude()
        sent_text = []
        sent_bytes = []

        async def st(t):
            sent_text.append(json.loads(t))
            if json.loads(t).get("type") == "audio_end":
                # Simulate sphere confirming playback shortly after audio_end
                asyncio.get_running_loop().call_later(0.02, sess.playback_done.set)

        async def sb(b):
            sent_bytes.append(b)

        sess = Session(cfg, Engines(wake=wake, stt=stt, tts=tts, claude=claude), st, sb)
        return sess, stt, tts, claude, sent_text, sent_bytes

    async def test_ptt_conversation_flow(self):
        sess, stt, tts, claude, sent_text, sent_bytes = self._make_session(["What is the weather?"])
        await sess.on_connect()
        await sess.on_text('{"type":"touch","action":"ptt_start"}')
        speech = (np.ones(16000, dtype=np.int16) * 2000).tobytes()
        await sess.on_audio(speech)
        await sess.on_text('{"type":"touch","action":"ptt_stop"}')
        await sess.task

        self.assertEqual(claude.prompts, ["What is the weather?"])
        self.assertEqual(tts.spoken, ["It is sunny and 72 in Fort Worth."])
        self.assertTrue(len(sent_bytes) > 0)
        states = [m["state"] for m in sent_text if m["type"] == "state"]
        self.assertEqual(states, ["idle", "listening", "thinking", "speaking", "idle"])

    async def test_hey_thad_one_breath_wake(self):
        wake_stt = FakeSTT(["Hey Thad, what time is it?"])
        wake = HeyThadVadWake(wake_stt, end_silence_s=0.16, min_speech_s=0.08)
        sess, _stt, tts, claude, _sent_text, _sent_bytes = self._make_session([], wake=wake)
        await sess.on_connect()

        # Feed 240ms of speech then 240ms of silence
        speech = (np.ones(1280 * 3, dtype=np.int16) * 2000).tobytes()
        quiet = np.zeros(1280 * 3, dtype=np.int16).tobytes()
        await sess.on_audio(speech)
        await sess.on_audio(quiet)
        if sess.task:
            await sess.task

        self.assertEqual(claude.prompts, ["what time is it?"])
        self.assertEqual(tts.spoken, ["It is sunny and 72 in Fort Worth."])

    async def test_tap_cancels_speaking(self):
        sess, _stt, _tts, _claude, sent_text, _sent_bytes = self._make_session([])
        await sess.on_connect()
        await sess.set_state("speaking", "Thad", "Speaking")
        await sess.on_text('{"type":"touch","action":"tap"}')
        types = [m["type"] for m in sent_text]
        self.assertIn("stop", types)
        self.assertEqual(sess.state, "idle")

    async def test_hub_usb_priority_and_wifi_fallback(self):
        cfg = Config(root_dir=Path("."))
        hub = BridgeHub(cfg, lambda: Engines(None, FakeSTT([]), FakeTTS(), FakeClaude()))

        usb_msgs = []
        wifi_msgs = []

        async def usb_st(t): usb_msgs.append(t)
        async def usb_sb(b): pass
        async def wifi_st(t): wifi_msgs.append(t)
        async def wifi_sb(b): pass

        s_usb = await hub.attach("usb", usb_st, usb_sb)
        self.assertIsNotNone(s_usb)
        self.assertEqual(hub.active_kind, "usb")

        # Wi-Fi refused while USB is active
        s_wifi = await hub.attach("wifi", wifi_st, wifi_sb)
        self.assertIsNone(s_wifi)

        # Detach USB -> Wi-Fi attaches cleanly
        await hub.detach(s_usb)
        s_wifi2 = await hub.attach("wifi", wifi_st, wifi_sb)
        self.assertIsNotNone(s_wifi2)
        self.assertEqual(hub.active_kind, "wifi")

        self.assertTrue(is_lan_address("192.168.1.42"))
        self.assertFalse(is_lan_address("8.8.8.8"))


if __name__ == "__main__":
    unittest.main()
