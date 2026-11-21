import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from thad_companion.claude_client import ClaudeClient, ClaudeConfigError
from thad_companion.config import load_config


class _FakeMessages:
    def __init__(self, replies):
        self.replies = list(replies)
        self.calls = []

    def create(self, **kwargs):
        self.calls.append(kwargs)
        txt = self.replies.pop(0)
        return SimpleNamespace(content=[SimpleNamespace(text=txt)])


class TestClaudeAndConfig(unittest.TestCase):
    def test_load_config_reads_dotenv_and_ignores_placeholder(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / "config.yaml").write_text("claude_model: claude-haiku-5-5\nvolume: 80\n", encoding="utf-8")
            (p / ".env").write_text("ANTHROPIC_API_KEY=sk-ant-YOUR_KEY_HERE\n", encoding="utf-8")
            old = os.environ.pop("ANTHROPIC_API_KEY", None)
            try:
                cfg = load_config(p / "config.yaml")
                self.assertEqual(cfg.anthropic_api_key, "")
                self.assertEqual(cfg.volume, 80)

                os.environ.pop("ANTHROPIC_API_KEY", None)
                (p / ".env").write_text("ANTHROPIC_API_KEY=sk-ant-test-123\n", encoding="utf-8")
                cfg = load_config(p / "config.yaml")
                self.assertEqual(cfg.anthropic_api_key, "sk-ant-test-123")
            finally:
                os.environ.pop("ANTHROPIC_API_KEY", None)
                if old is not None:
                    os.environ["ANTHROPIC_API_KEY"] = old

    def test_claude_missing_key_and_history_reset(self):
        c_empty = ClaudeClient(api_key="")
        with self.assertRaises(ClaudeConfigError):
            c_empty.ask("hello")

        fake_msgs = _FakeMessages(["First answer.", "Second answer.", "Fresh answer."])
        client = ClaudeClient(
            api_key="sk-ant-fake",
            system_prompt="Be brief.",
            history_turns=4,
            history_reset_s=60.0,
            client_factory=lambda _k: SimpleNamespace(messages=fake_msgs),
        )
        self.assertEqual(client.ask("Hi", now=10.0), "First answer.")
        self.assertEqual(client.ask("Follow up", now=20.0), "Second answer.")
        self.assertEqual(len(fake_msgs.calls[1]["messages"]), 3)

        # Idle past 60s clears history
        self.assertEqual(client.ask("New topic", now=100.0), "Fresh answer.")
        self.assertEqual(len(fake_msgs.calls[2]["messages"]), 1)


if __name__ == "__main__":
    unittest.main()
