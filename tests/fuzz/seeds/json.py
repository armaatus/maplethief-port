"""Seeds for fuzz_json: gamecenter.json as gamekit.cpp writes it, and the reader's other forms."""

from __future__ import annotations

MAX_LEN = 16384


def seeds() -> dict[str, bytes]:
    texts = {
        "gamecenter.json": '{"format": 1, "player": "Local Player", "scores": [{"category": '
        '"com.example.board", "value": 1200, "date": 1735689600.25}], '
        '"achievements": [{"id": "first", "percent": 100, "date": 1735689600}]}',
        "escapes.json": '["x\\"\\\\\\/\\b\\f\\n\\r\\t", "\\u00e9\\ud83d\\ude00", "\\u0041"]',
        "numbers.json": "[0, -0, 1.5e300, -2.5E-3, 9223372036854775807, -9223372036854775808, "
        "18446744073709551616, 1e999]",
        "literals.json": ' {"a": true, "b": false, "c": null, "d": {}, "e": []} ',
        "nested.json": "[" * 60 + "]" * 60,
        "utf8.json": '{"브루스": "\U0001f600"}',
        "lone-surrogate.json": '["\\ud800", "\\udc00"]',
    }
    return {name: text.encode("utf-8") for name, text in texts.items()}
