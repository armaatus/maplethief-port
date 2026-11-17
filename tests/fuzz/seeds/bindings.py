"""Seeds for fuzz_bindings: bindings.ini text written here, good lines and bad."""

from __future__ import annotations

MAX_LEN = 16384


def seeds() -> dict[str, bytes]:
    texts = {
        "remap.ini": "\ufeff# remapped\r\nnintendo_buttons = Label\r\n\r\n[field]\r\n"
        "  Attack = key K , button leftstick\r\njump = key Space, button a\n[MENU]\n"
        "ok = key Return, button a\n",
        "chords.ini": "[field]\nattack = none\nslot1 = key Q\nfast_forward = hold rightshoulder\n"
        "[menu]\nback = button b\n[window]\nsize_1x = key Ctrl+1\n"
        "size_3x = key Alt+Shift+3, none\n",
        "triggers.ini": "[field]\nattack = trigger righttrigger\njump = trigger lefttrigger\n"
        "map = key M, key N\n",
        "bad.ini": "attack = key K\n[field]\natack = key K\njump = key Nonsense\n"
        "slot1 = key Q, button z, key W\nslot2 key E\nslot3 = button dpup\n"
        "slot4 = key F1, key T\nmap = key M, , key N\n[options]\nattack = key L\n[field\n"
        "nintendo_buttons = sideways\n[menu]\nback = trigger leftx, key Backspace\n",
    }
    out = {name: text.encode("utf-8") for name, text in texts.items()}
    out["junk.ini"] = b"\x00\xff[\n=\n[field]\n=key\n\xc3\x28 = \x80\n"
    return out
