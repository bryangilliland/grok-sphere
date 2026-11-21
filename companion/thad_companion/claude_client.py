"""Anthropic Claude API client for Thad voice responses."""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field

log = logging.getLogger("claude")


class ClaudeConfigError(RuntimeError):
    pass


class ClaudeAPIError(RuntimeError):
    pass


@dataclass
class ClaudeClient:
    api_key: str
    model: str = "claude-haiku-5-5"
    system_prompt: str = ""
    max_tokens: int = 220
    history_turns: int = 8
    history_reset_s: float = 600.0
    client_factory: object = None
    _history: list[dict[str, str]] = field(default_factory=list)
    _last_turn_at: float = 0.0
    _client: object = field(default=None, repr=False)

    def _get_client(self):
        if not self.api_key:
            raise ClaudeConfigError("API key missing")
        if self._client is None:
            if self.client_factory is not None:
                self._client = self.client_factory(self.api_key)
            else:
                try:
                    import anthropic  # type: ignore
                except ImportError as exc:
                    raise ClaudeAPIError("anthropic package not installed") from exc
                self._client = anthropic.Anthropic(api_key=self.api_key, timeout=30.0)
        return self._client

    def clear_history(self) -> None:
        self._history.clear()

    def ask(self, user_text: str, now: float | None = None) -> str:
        t = time.monotonic() if now is None else now
        if self._history and (t - self._last_turn_at) > self.history_reset_s:
            log.info("conversation idle timeout reached; resetting history")
            self._history.clear()

        client = self._get_client()
        messages = [*self._history, {"role": "user", "content": user_text}]

        try:
            resp = client.messages.create(
                model=self.model,
                max_tokens=self.max_tokens,
                system=self.system_prompt,
                messages=messages,
            )
        except Exception as exc:  # noqa: BLE001
            name = type(exc).__name__
            if "Authentication" in name or "Permission" in name:
                raise ClaudeConfigError("Invalid API key") from exc
            if "RateLimit" in name:
                raise ClaudeAPIError("Claude rate limit hit") from exc
            raise ClaudeAPIError(f"Claude error: {name}") from exc

        parts = []
        for block in getattr(resp, "content", []) or []:
            txt = getattr(block, "text", None)
            if txt:
                parts.append(txt.strip())
        reply = " ".join(parts).strip()
        if not reply:
            raise ClaudeAPIError("Empty reply from Claude")

        self._history.append({"role": "user", "content": user_text})
        self._history.append({"role": "assistant", "content": reply})
        max_msgs = max(2, self.history_turns * 2)
        if len(self._history) > max_msgs:
            self._history = self._history[-max_msgs:]
        self._last_turn_at = t
        return reply
