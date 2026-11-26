"""The Android APK's host-side checks (mtport.apk, platform.android_apk, ADR 0150, #356).

Synthetic APK-shaped zips only: the real MapleThief.apk is checked by CTest in an android-*
build (platform/android/android.cmake).
"""

from __future__ import annotations

import io
import zipfile

import pytest

from mtport import apk, appicon


def _entries(**override: bytes | None) -> dict[str, bytes]:
    entries = {
        "AndroidManifest.xml": b"manifest",
        "classes.dex": b"dex\n035\0",
        "resources.arsc": b"arsc",
        apk.MAIN_LIB: apk.make_elf(),
    }
    for name, payload in override.items():
        name = name.replace("__", "/")
        if payload is None:
            entries.pop(name, None)
        else:
            entries[name] = payload
    return entries


def test_a_good_apk_passes():
    data = apk.make_apk(_entries())
    assert apk.zip_problems(data) == []
    info = zipfile.ZipFile(io.BytesIO(data)).getinfo(apk.MAIN_LIB)
    assert apk._data_offset(data, info) % apk.PAGE_ALIGN == 0


def test_libmain_must_be_stored_and_16k_aligned():
    problems = apk.zip_problems(apk.make_apk(_entries(), lib_stored=False, lib_aligned=False))
    assert any("compressed" in p for p in problems)
    assert any("16 KiB boundary" in p for p in problems)


def test_another_abi_or_library_is_refused():
    other = "lib/armeabi-v7a/libmain.so"
    problems = apk.zip_problems(apk.make_apk({**_entries(), other: apk.make_elf()}))
    assert problems == [f"{other}: the APK holds libmain.so for arm64-v8a only (ADR 0150)"]


def test_missing_parts_are_named():
    problems = apk.zip_problems(
        apk.make_apk(
            _entries(**{"classes.dex": None}),
            signed=False,
        )
    )
    assert "no classes.dex" in problems
    assert "not signed with APK Signature Scheme v2 or later" in problems


def test_ipa_files_are_refused():
    problems = apk.zip_problems(apk.make_apk(_entries(**{"assets__game.ipa": b"x"})))
    assert problems == ["assets/game.ipa: a file of the IPA (ADR 0100)"]
    problems = apk.zip_problems(apk.make_apk(_entries(**{"Payload__a.png": b"x"})))
    assert problems == ["Payload/a.png: a file of the IPA (ADR 0100)"]


def test_elf_checks():
    assert apk.elf_problems(apk.make_elf()) == []
    assert apk.elf_problems(apk.make_elf(align=65536)) == []
    (problem,) = apk.elf_problems(apk.make_elf(align=4096))
    assert "aligned to 4096 bytes" in problem
    (problem,) = apk.elf_problems(apk.make_elf(machine=62))
    assert "not AArch64" in problem
    assert apk.elf_problems(b"not an elf") == ["libmain.so is not an ELF file"]


def test_not_a_zip():
    with pytest.raises(apk.ApkError):
        apk.zip_problems(b"PK not really")


def test_badging_problems():
    fields = {
        "package": "io.github.armaatus.maplethief",
        "label": "MapleThief",
        "version": "1.2.3",
        "min_sdk": "26",
        "target_sdk": "35",
        "native_code": "'arm64-v8a'",
    }
    expected = {"package": "io.github.armaatus.maplethief", "min_sdk": "26", "label": None}
    assert apk.badging_problems(fields, expected) == []
    assert apk.badging_problems({**fields, "min_sdk": "21"}, expected) == [
        "min_sdk is '21', expected '26'"
    ]
    assert apk.badging_problems({**fields, "native_code": "'arm64-v8a' 'x86_64'"}, {}) == [
        "native code is 'arm64-v8a' 'x86_64', expected 'arm64-v8a' alone"
    ]


def test_icons_must_be_the_ports_own():
    # Shortened names, as AGP gives a release APK's resources.
    icons, fields = {}, {}
    for short, density in zip("abcde", appicon.ANDROID_ICON_SIZES, strict=True):
        icons[f"res/{short}.png"] = appicon.android_icons()[f"mipmap-{density}/ic_launcher.png"]
        fields[f"icon_{apk.ICON_DPI[density]}"] = f"res/{short}.png"
    data = apk.make_apk({**_entries(), **icons})
    assert apk.icon_problems(data, fields) == []
    swapped = {**fields, "icon_160": "res/b.png"}
    assert apk.icon_problems(data, swapped) == [
        "the 160 dpi icon res/b.png is not the port's own 48 px icon"
    ]
    missing = {k: v for k, v in fields.items() if k != "icon_640"}
    assert apk.icon_problems(data, missing) == ["no launcher icon for 640 dpi (xxxhdpi)"]


def test_cli_check_and_info(tmp_path, capsys):
    good = tmp_path / "good.apk"
    good.write_bytes(apk.make_apk(_entries()))
    assert apk.main(["check", str(good)]) == 0
    assert "OK" in capsys.readouterr().out
    assert apk.main(["info", str(good)]) == 0
    assert "stored" in capsys.readouterr().out
    bad = tmp_path / "bad.apk"
    bad.write_bytes(apk.make_apk(_entries(), signed=False))
    assert apk.main(["check", str(bad)]) == 1
    assert "not signed" in capsys.readouterr().err
