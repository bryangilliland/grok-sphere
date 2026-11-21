"""Turn a chat reply into something pleasant to hear."""

from __future__ import annotations

import re

_LINK = re.compile(r"\[([^\]]+)\]\((?:[^)]+)\)")
_URL = re.compile(r"https?://\S+")
_CODE_BLOCK = re.compile(r"```.*?```", re.S)
_INLINE_CODE = re.compile(r"`([^`]*)`")
_EMPH = re.compile(r"(\*\*|__|\*|_|~~)(?=\S)(.+?)(?<=\S)\1")
_HEADER = re.compile(r"^\s{0,3}#{1,6}\s*", re.M)
_BULLET = re.compile(r"^\s*(?:[-*+•]|\d+[.)])\s+", re.M)
_TABLE_RULE = re.compile(r"^\s*\|?\s*:?-{3,}.*$", re.M)


def clean_for_speech(text: str, max_chars: int = 600) -> str:
    t = _CODE_BLOCK.sub(" (code in the chat) ", text)
    t = _LINK.sub(r"\1", t)
    t = _URL.sub("a link", t)
    t = _INLINE_CODE.sub(r"\1", t)
    t = _EMPH.sub(r"\2", t)
    t = _HEADER.sub("", t)
    t = _TABLE_RULE.sub("", t)
    t = _BULLET.sub("", t)
    t = t.replace("|", ", ")
    lines = [ln.strip() for ln in t.splitlines() if ln.strip()]
    t = " ".join(ln if ln.endswith((".", "?", "!", ":", ";", ",")) else ln + "." for ln in lines)
    t = re.sub(r"\s+", " ", t).strip()
    t = re.sub(r"\.\s*\.", ".", t)
    if len(t) > max_chars:
        cut = t.rfind(". ", 0, max_chars)
        cut = cut + 1 if cut > max_chars // 2 else max_chars
        t = t[:cut].rstrip() + " The rest is in the chat."
    return t
