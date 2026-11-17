"""Seeds for fuzz_strings: .strings text in Apple's syntax, written here."""

from __future__ import annotations

MAX_LEN = 16384


def seeds() -> dict[str, bytes]:
    texts = {
        "plain.strings": '"hello" = "Hallo";\n"bye" = "Tsch\u00fcss";\n',
        "comments.strings": '/* block */\n// line\n"a" = "b"; // after\n/* x */ "c" = "d";\n',
        "escapes.strings": '"esc" = "q\\" b\\\\ s\\\' n\\n t\\t r\\r u\\u00e9 U\\U0001";\n'
        '"pair" = "\\uD83D\\uDE00";\n',
        "multiline.strings": '"poem" = "line one\nline two";\n"dup" = "1";\n"dup" = "2";\n',
        "bom.strings": '\ufeff"k" = "v";',
        "korean.strings": '"title" = "\uba54\uc774\ud50c";\n',
        "bad-escape.strings": '"a" = "\\x";\n',
        "lone-surrogate.strings": '"a" = "\\uD800";\n',
        "empty.strings": "",
    }
    return {name: text.encode("utf-8") for name, text in texts.items()}
