"""The Android APK's host-side checks (ADR 0150, issue #356).

An APK is a zip archive: the binary manifest, the Java classes (``classes.dex``), the compiled
resources and the native libraries below ``lib/<abi>/``, signed with APK Signature Scheme v2 or
later in a block just before the zip's central directory. The Android build checks its own
``MapleThief.apk`` with this module (CTest ``platform.android_apk``,
platform/android/android.cmake):

- ``lib/arm64-v8a/libmain.so`` is the only native library and arm64-v8a the only ABI (ADR 0150:
  no 32-bit and no x86 build);
- it is stored uncompressed and starts at a 16 KiB boundary of the file, so the system maps it
  in place on a device with 4 KiB or 16 KiB pages, and it is a 64-bit little-endian AArch64
  shared object whose loadable segments are aligned to 16 KiB or more (Android 15's 16 KiB page
  devices refuse anything less);
- the archive is signed (v2 block), and no entry is an IPA or a file from an IPA's ``Payload/``
  (ADR 0100);
- with ``--aapt2`` (the pinned build tools' ``aapt2 dump badging``), the package name, the label,
  the version name and the API levels are the expected ones, and the launcher icon at each of
  the five densities decodes to exactly the port's own (mtport.appicon, ADR 0125). A release
  build shortens resource paths (``res/9w.png``), so only the badging can say which file the
  icon is.

It never runs the APK's code. :func:`make_apk` and :func:`make_elf` build small ones for the
tests.

CLI::

    python3 -m mtport.apk info APK
    python3 -m mtport.apk check APK [--package P] [--label L] [--version V] [--min-sdk N]
                                    [--target-sdk N] [--aapt2 PATH]
"""

from __future__ import annotations

import argparse
import io
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

from mtport import appicon, png

ABI = "arm64-v8a"
MAIN_LIB = f"lib/{ABI}/libmain.so"
PAGE_ALIGN = 16384  # 16 KiB: the largest page size an arm64 Android device uses
SIG_BLOCK_MAGIC = b"APK Sig Block 42"
# The launcher densities' dpi, as aapt2's badging names them (application-icon-<dpi>).
ICON_DPI = {"mdpi": 160, "hdpi": 240, "xhdpi": 320, "xxhdpi": 480, "xxxhdpi": 640}

ELF_MAGIC = b"\x7fELF"
ELFCLASS64 = 2
ELFDATA2LSB = 1
ET_DYN = 3
EM_AARCH64 = 183
PT_LOAD = 1


class ApkError(Exception):
    """The file is not an APK this module can read."""


def elf_problems(data: bytes) -> list[str]:
    """What is wrong with ``data`` as libmain.so: an AArch64 shared object, 16 KiB segments."""
    if len(data) < 64 or data[:4] != ELF_MAGIC:
        return ["libmain.so is not an ELF file"]
    if data[4] != ELFCLASS64 or data[5] != ELFDATA2LSB:
        return ["libmain.so is not a 64-bit little-endian ELF file"]
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    problems = []
    if e_type != ET_DYN:
        problems.append(f"libmain.so is ELF type {e_type}, not a shared object ({ET_DYN})")
    if e_machine != EM_AARCH64:
        problems.append(f"libmain.so is for ELF machine {e_machine}, not AArch64 ({EM_AARCH64})")
    (e_phoff,) = struct.unpack_from("<Q", data, 32)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)
    if e_phentsize < 56 or e_phoff + e_phnum * e_phentsize > len(data):
        return problems + ["libmain.so's program headers are outside the file"]
    loads = 0
    for i in range(e_phnum):
        p_type = struct.unpack_from("<I", data, e_phoff + i * e_phentsize)[0]
        (p_align,) = struct.unpack_from("<Q", data, e_phoff + i * e_phentsize + 48)
        if p_type != PT_LOAD:
            continue
        loads += 1
        if p_align < PAGE_ALIGN or p_align % PAGE_ALIGN:
            problems.append(
                f"libmain.so's LOAD segment {i} is aligned to {p_align} bytes, not a multiple "
                f"of {PAGE_ALIGN} (16 KiB page devices refuse it)"
            )
    if loads == 0:
        problems.append("libmain.so has no LOAD segment")
    return problems


def _data_offset(data: bytes, info: zipfile.ZipInfo) -> int:
    """Where the entry's bytes start: after its local header, whose extra field may differ."""
    name_len, extra_len = struct.unpack_from("<HH", data, info.header_offset + 26)
    return info.header_offset + 30 + name_len + extra_len


def _signed(data: bytes) -> bool:
    """Whether an APK Signing Block (v2 and later) sits just before the central directory."""
    eocd = data.rfind(b"PK\x05\x06")
    if eocd < 0 or eocd + 22 > len(data):
        return False
    (cd_offset,) = struct.unpack_from("<I", data, eocd + 16)
    return cd_offset >= 24 and data[cd_offset - 16 : cd_offset] == SIG_BLOCK_MAGIC


def zip_problems(data: bytes) -> list[str]:
    """Every problem of the APK's archive itself (no aapt2 needed)."""
    try:
        archive = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile as e:
        raise ApkError(f"not a zip archive: {e}") from e
    infos = archive.infolist()
    names = [i.filename for i in infos]
    problems = []
    for required in ("AndroidManifest.xml", "classes.dex", "resources.arsc"):
        if required not in names:
            problems.append(f"no {required}")
    libs = [n for n in names if n.startswith("lib/") and not n.endswith("/")]
    if MAIN_LIB not in libs:
        problems.append(f"no {MAIN_LIB}")
    for other in sorted(set(libs) - {MAIN_LIB}):
        problems.append(f"{other}: the APK holds libmain.so for {ABI} only (ADR 0150)")
    if MAIN_LIB in libs:
        info = archive.getinfo(MAIN_LIB)
        if info.compress_type != zipfile.ZIP_STORED:
            problems.append(f"{MAIN_LIB} is compressed; it must be stored to be mapped in place")
        offset = _data_offset(data, info)
        if offset % PAGE_ALIGN:
            problems.append(f"{MAIN_LIB} starts at byte {offset}, not at a 16 KiB boundary")
        problems += elf_problems(archive.read(MAIN_LIB))
    for name in names:
        if name.lower().endswith(".ipa") or name.startswith("Payload/"):
            problems.append(f"{name}: a file of the IPA (ADR 0100)")
    if not _signed(data):
        problems.append("not signed with APK Signature Scheme v2 or later")
    return problems


def badging(aapt2: Path, apk: Path) -> dict[str, str]:
    """The fields of ``aapt2 dump badging`` this module checks."""
    out = subprocess.run(
        [str(aapt2), "dump", "badging", str(apk)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    fields: dict[str, str] = {}
    package = re.search(r"^package: name='([^']*)'.*?versionName='([^']*)'", out, re.M)
    if package:
        fields["package"], fields["version"] = package.group(1), package.group(2)
    for key, pattern in (
        ("label", r"^application-label:'([^']*)'"),
        ("min_sdk", r"^(?:minSdkVersion|sdkVersion):'([^']*)'"),
        ("target_sdk", r"^targetSdkVersion:'([^']*)'"),
        ("native_code", r"^native-code: (.*)$"),
    ):
        match = re.search(pattern, out, re.M)
        if match:
            fields[key] = match.group(1)
    for dpi, path in re.findall(r"^application-icon-(\d+):'([^']*)'", out, re.M):
        fields[f"icon_{dpi}"] = path
    return fields


def icon_problems(data: bytes, fields: dict[str, str]) -> list[str]:
    """Whether the badging's launcher icon at each density is the port's own, pixel for pixel."""
    archive = zipfile.ZipFile(io.BytesIO(data))
    problems = []
    for density, size in appicon.ANDROID_ICON_SIZES.items():
        dpi = ICON_DPI[density]
        path = fields.get(f"icon_{dpi}")
        if path is None:
            problems.append(f"no launcher icon for {dpi} dpi ({density})")
            continue
        try:
            img = png.decode(archive.read(path), path)
        except (KeyError, png.PngError) as e:
            problems.append(f"the {dpi} dpi icon {path} can't be read: {e}")
            continue
        if (img.width, img.height) != (size, size) or img.rgba != appicon.render(size).rgba:
            problems.append(f"the {dpi} dpi icon {path} is not the port's own {size} px icon")
    return problems


def badging_problems(fields: dict[str, str], expected: dict[str, str | None]) -> list[str]:
    """Each expected badging field (None: not checked) that ``fields`` doesn't match."""
    problems = []
    for key, want in expected.items():
        if want is not None and fields.get(key) != want:
            problems.append(f"{key} is {fields.get(key)!r}, expected {want!r}")
    if fields.get("native_code", f"'{ABI}'") != f"'{ABI}'":
        problems.append(f"native code is {fields['native_code']}, expected '{ABI}' alone")
    return problems


def describe(data: bytes) -> str:
    archive = zipfile.ZipFile(io.BytesIO(data))
    lines = [f"{len(data)} bytes, {len(archive.infolist())} entries"]
    if MAIN_LIB in archive.namelist():
        info = archive.getinfo(MAIN_LIB)
        lines.append(
            f"{MAIN_LIB}: {info.file_size} bytes at offset {_data_offset(data, info)}"
            f" ({'stored' if info.compress_type == zipfile.ZIP_STORED else 'compressed'})"
        )
    lines.append(f"signed (v2+): {'yes' if _signed(data) else 'no'}")
    return "\n".join(lines)


def make_elf(align: int = PAGE_ALIGN, machine: int = EM_AARCH64) -> bytes:
    """A minimal 64-bit ELF shared object with one LOAD segment aligned to ``align``."""
    header = bytearray(64)
    header[:4] = ELF_MAGIC
    header[4], header[5], header[6] = ELFCLASS64, ELFDATA2LSB, 1
    struct.pack_into("<HHI", header, 16, ET_DYN, machine, 1)
    struct.pack_into("<Q", header, 32, 64)  # e_phoff
    struct.pack_into("<HHHH", header, 52, 64, 56, 1, 0)  # ehsize, phentsize, phnum, shentsize
    phdr = struct.pack("<IIQQQQQQ", PT_LOAD, 5, 0, 0, 0, 120, 120, align)
    return bytes(header) + phdr


def make_apk(
    entries: dict[str, bytes],
    *,
    lib_aligned: bool = True,
    lib_stored: bool = True,
    signed: bool = True,
) -> bytes:
    """A small APK-shaped zip: ``entries`` in order, libraries stored and 16 KiB-aligned unless
    told otherwise, and a stand-in signing block (its magic only) when ``signed``."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as archive:
        for name, payload in entries.items():
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            is_lib = name.startswith("lib/")
            info.compress_type = (
                zipfile.ZIP_STORED if (is_lib and lib_stored) else zipfile.ZIP_DEFLATED
            )
            if is_lib and lib_aligned:
                start = buf.tell() + 30 + len(name.encode())
                pad = (-(start + 4)) % PAGE_ALIGN
                info.extra = struct.pack("<HH", 0xD935, pad) + b"\0" * pad
            archive.writestr(info, payload)
    data = buf.getvalue()
    if not signed:
        return data
    eocd = data.rfind(b"PK\x05\x06")
    (cd_offset,) = struct.unpack_from("<I", data, eocd + 16)
    block = struct.pack("<Q", 0) + SIG_BLOCK_MAGIC
    out = bytearray(data[:cd_offset] + block + data[cd_offset:])
    struct.pack_into("<I", out, eocd + len(block) + 16, cd_offset + len(block))
    return bytes(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.apk", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info", help="print the entries' count, libmain.so's place, the signature")
    p.add_argument("apk", type=Path)
    p = sub.add_parser("check", help="exit 1 unless the APK matches the expectations")
    p.add_argument("apk", type=Path)
    p.add_argument("--package")
    p.add_argument("--label")
    p.add_argument("--version")
    p.add_argument("--min-sdk")
    p.add_argument("--target-sdk")
    p.add_argument("--aapt2", type=Path, help="the pinned build tools' aapt2, for the badging")
    args = parser.parse_args(argv)
    data = args.apk.read_bytes()
    try:
        if args.cmd == "info":
            print(describe(data))
            return 0
        problems = zip_problems(data)
    except ApkError as e:
        print(f"apk: {args.apk}: {e}", file=sys.stderr)
        return 1
    if args.aapt2:
        expected = {
            "package": args.package,
            "label": args.label,
            "version": args.version,
            "min_sdk": args.min_sdk,
            "target_sdk": args.target_sdk,
        }
        fields = badging(args.aapt2, args.apk)
        problems += badging_problems(fields, expected) + icon_problems(data, fields)
    for problem in problems:
        print(f"apk: {args.apk}: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"apk: {args.apk}: OK\n{describe(data)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
