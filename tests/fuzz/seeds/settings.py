"""Seeds for fuzz_settings: settings.ini text written here, good lines and bad."""

from __future__ import annotations

MAX_LEN = 16384


def seeds() -> dict[str, bytes]:
    texts = {
        "video.ini": "# video\nscale_mode = fit\nborder_color = #102030\nrender_scale = auto\n"
        "gfx_backend = software\nfilter = crt\nhd_text = on\n",
        "audio.ini": "master_volume = 80\nmusic_volume=0\neffects_volume = 100\n"
        "gapless_music = on\naudio_device = default\naudio_mono = off\n",
        "preset.ini": "preset = enhanced\nhd_text = off\nwidescreen = on\n",
        "custom.ini": "preset = custom\nright_touch = 100,200\nhide_keypad = auto\n"
        "rumble_strength = 50\nsave_backups = 3\ngame_speed = 1.5\nbrightness = 10\n",
        "paths.ini": "save_dir = /tmp/saves\ntexture_pack = packs/hd\ntext_pack = \n"
        "border_image = b\u00f6rder.png\n",
        "crlf-bom.ini": "\ufeffscale_mode = sharp\r\nvsync = off\r\n\r\n; not a comment\r\n",
        "bad.ini": "scael_mode = fit\n= 3\nmaster_volume = 101\nfullscreen\nvsync = maybe\n",
    }
    return {name: text.encode("utf-8") for name, text in texts.items()}
