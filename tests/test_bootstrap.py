"""Tests for the bootstrap on synthetic repos (no IPA needed).

A SyntheticRepo is a temporary directory laid out like the repo, whose original/ holds a
synthetic stand-in for the IPA: a zip padded to the real IPA's size whose main binary is a
synthetic fat Mach-O, with a matching SHA256SUMS. The module tests run mtport.bootstrap on
it directly; the script tests copy scripts/ and tools/mtport/ in and run scripts/bootstrap,
scripts/verify-original.sh (and scripts/bootstrap.ps1 when pwsh is installed) end to end.
"""

from __future__ import annotations

import functools
import hashlib
import io
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path

from machofixtures import ARMV6_THIN, ARMV7_THIN, SAMPLE_FAT
from mtport import bootstrap as bs
from repo_paths import REPO

LFS_POINTER = (
    b"version https://git-lfs.github.com/spec/v1\n"
    b"oid sha256:a6a06f050aa21af9576dbf8af3c832dcbbf38f453886ef1690447f934cb3e387\n"
    b"size %d\n" % bs.IPA_SIZE
)
BINARY_ENTRY = "Payload/MapleThief.app/MapleThief"
ZIP_TIME = (2010, 1, 2, 3, 4, 6)
# (name, data, Unix mode or 0 for an MS-DOS entry like the real IPA's)
Entry = tuple[str, bytes, int]
DEFAULT_ENTRIES: tuple[Entry, ...] = (
    (BINARY_ENTRY, SAMPLE_FAT, 0),
    ("Payload/MapleThief.app/Info.plist", b"<plist/>", 0),
    ("iTunesArtwork", b"not really a PNG", 0),
)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@functools.cache
def synthetic_ipa(entries: tuple[Entry, ...] = DEFAULT_ENTRIES, size: int = bs.IPA_SIZE) -> bytes:
    """A zip of ``entries`` plus padding.bin, padded to exactly ``size`` bytes."""

    def build(pad: int) -> bytes:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            for name, data, mode in (*entries, ("padding.bin", b"\0" * pad, 0)):
                info = zipfile.ZipInfo(name, date_time=ZIP_TIME)
                info.compress_type = zipfile.ZIP_STORED
                if mode:
                    info.create_system = 3  # Unix
                    info.external_attr = mode << 16
                z.writestr(info, data)
        return buf.getvalue()

    data = build(size - len(build(0)))
    assert len(data) == size
    return data


def sums_text(entries: dict[str, bytes]) -> str:
    return "".join(f"{sha(data)}  {path}\n" for path, data in entries.items())


def tree_digest(root: Path) -> dict[str, str]:
    """``{relative path: sha256}`` for every file under ``root`` (contents only)."""
    return {
        p.relative_to(root).as_posix(): sha(p.read_bytes())
        for p in sorted(root.rglob("*"))
        if p.is_file()
    }


class SyntheticRepo:
    """A temp dir laid out like the repo, with a synthetic IPA."""

    def __init__(self, tmp: str, ipa: bytes | None = None, with_scripts: bool = False):
        self.root = Path(tmp)
        (self.root / "original").mkdir()
        self.set_ipa(synthetic_ipa() if ipa is None else ipa)
        if with_scripts:
            shutil.copytree(REPO / "scripts", self.root / "scripts")
            shutil.copytree(
                REPO / "tools" / "mtport",
                self.root / "tools" / "mtport",
                ignore=shutil.ignore_patterns("__pycache__"),
            )

    def set_ipa(self, ipa: bytes, binary: bytes = SAMPLE_FAT) -> None:
        """Install ``ipa`` with a SHA256SUMS that expects it and ``binary`` inside it."""
        self.ipa = ipa
        (self.root / bs.IPA).write_bytes(ipa)
        self.write_sums(
            {
                bs.IPA: ipa,
                bs.BINARY: binary,
                bs.slice_path("armv6"): ARMV6_THIN,
                bs.slice_path("armv7"): ARMV7_THIN,
            }
        )

    def write_sums(self, entries: dict[str, bytes]) -> None:
        (self.root / bs.SUMS).write_text(sums_text(entries), encoding="ascii")


def quiet(*_args) -> None:
    pass


class BootstrapModuleTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = SyntheticRepo(self._tmp.name)
        self.root = self.repo.root

    def tearDown(self):
        self._tmp.cleanup()

    def run_bootstrap(self, until: str = bs.SPLIT):
        return bs.bootstrap(self.root, lipo_check=False, log=quiet, until=until)

    def assertNoTempLeft(self):
        work = self.root / "work"
        left = (
            sorted(p.name for p in work.iterdir() if p.name.startswith("."))
            if work.exists()
            else []
        )
        self.assertEqual(left, [])

    def test_writes_slices_sums_and_json(self):
        doc = self.run_bootstrap()
        self.assertEqual((self.root / "work/slices/MapleThief.armv6").read_bytes(), ARMV6_THIN)
        self.assertEqual((self.root / "work/slices/MapleThief.armv7").read_bytes(), ARMV7_THIN)
        self.assertEqual(
            (self.root / bs.SLICE_SUMS).read_text(encoding="utf-8"),
            f"{sha(ARMV6_THIN)}  work/slices/MapleThief.armv6\n"
            f"{sha(ARMV7_THIN)}  work/slices/MapleThief.armv7\n",
        )
        on_disk = json.loads((self.root / bs.BOOTSTRAP_JSON).read_text(encoding="utf-8"))
        self.assertEqual(on_disk, doc)
        self.assertEqual(doc["schema_version"], 1)
        self.assertEqual(doc["tool"]["name"], "mtport.bootstrap")
        self.assertEqual(doc["primary_slice"], "armv7")
        self.assertEqual(
            doc["ipa"], {"path": bs.IPA, "size": bs.IPA_SIZE, "sha256": sha(self.repo.ipa)}
        )
        self.assertEqual(
            doc["binary"], {"path": bs.BINARY, "size": len(SAMPLE_FAT), "sha256": sha(SAMPLE_FAT)}
        )
        self.assertEqual(doc["slices"]["armv6"]["fat_offset"], 4096)
        self.assertEqual(doc["slices"]["armv7"]["size"], len(ARMV7_THIN))
        self.assertEqual(doc["slices"]["armv7"]["cpusubtype"], 9)

    def test_extracts_every_entry_with_its_time(self):
        self.assertIsNone(self.run_bootstrap(until=bs.EXTRACT))
        ipa_dir = self.root / bs.IPA_DIR
        with zipfile.ZipFile(io.BytesIO(self.repo.ipa)) as z:
            want = {i.filename: sha(z.read(i)) for i in z.infolist()}
        self.assertEqual(tree_digest(ipa_dir), want)
        mtime = time.mktime((*ZIP_TIME, 0, 0, -1))
        self.assertEqual((ipa_dir / BINARY_ENTRY).stat().st_mtime, mtime)
        self.assertFalse((self.root / bs.SLICES_DIR).exists())
        self.assertFalse((self.root / bs.BOOTSTRAP_JSON).exists())
        self.assertNoTempLeft()

    def test_extraction_replaces_old_tree(self):
        stale = self.root / bs.IPA_DIR / "stale.txt"
        stale.parent.mkdir(parents=True)
        stale.write_text("from an earlier run", encoding="utf-8")
        self.run_bootstrap(until=bs.EXTRACT)
        self.assertFalse(stale.exists())
        self.assertTrue((self.root / bs.BINARY).is_file())

    def test_verify_only_writes_nothing(self):
        self.assertIsNone(self.run_bootstrap(until=bs.VERIFY))
        self.assertFalse((self.root / "work").exists())

    def test_two_runs_give_identical_outputs(self):
        self.run_bootstrap()
        first = tree_digest(self.root / "work")
        self.run_bootstrap()
        self.assertEqual(tree_digest(self.root / "work"), first)
        self.assertIn("bootstrap.json", first)
        self.assertIn("ipa/" + BINARY_ENTRY, first)
        self.assertNoTempLeft()

    def test_slice_hash_mismatch_writes_nothing(self):
        self.repo.write_sums(
            {
                bs.IPA: self.repo.ipa,
                bs.BINARY: SAMPLE_FAT,
                bs.slice_path("armv6"): ARMV6_THIN,
                bs.slice_path("armv7"): b"something else",
            }
        )
        with self.assertRaisesRegex(bs.BootstrapError, "MapleThief.armv7: SHA-256 mismatch"):
            self.run_bootstrap()
        self.assertFalse((self.root / bs.SLICES_DIR).exists())
        self.assertFalse((self.root / bs.BOOTSTRAP_JSON).exists())

    def test_missing_slice_entry(self):
        self.repo.write_sums({bs.IPA: self.repo.ipa, bs.BINARY: SAMPLE_FAT})
        with self.assertRaisesRegex(bs.BootstrapError, "no entry for work/slices/MapleThief.armv6"):
            self.run_bootstrap()

    def test_binary_hash_mismatch_keeps_the_old_tree(self):
        self.run_bootstrap(until=bs.EXTRACT)
        before = tree_digest(self.root / "work")
        self.repo.set_ipa(self.repo.ipa, binary=SAMPLE_FAT + b"\0")
        with self.assertRaisesRegex(bs.BootstrapError, "MapleThief: SHA-256 mismatch"):
            self.run_bootstrap()
        self.assertEqual(tree_digest(self.root / "work"), before)
        self.assertNoTempLeft()

    def test_ipa_without_binary(self):
        self.repo.set_ipa(synthetic_ipa((("Payload/Other.app/Other", b"x", 0),)))
        with self.assertRaisesRegex(bs.BootstrapError, f"extraction did not produce {bs.BINARY}"):
            self.run_bootstrap()
        self.assertFalse((self.root / bs.IPA_DIR).exists())
        self.assertNoTempLeft()

    def check_rejected(self, entries: tuple[Entry, ...], pattern: str):
        self.repo.set_ipa(synthetic_ipa((*DEFAULT_ENTRIES, *entries)))
        with self.assertRaisesRegex(bs.BootstrapError, pattern):
            self.run_bootstrap()
        # Every entry is checked before anything is written.
        self.assertFalse((self.root / bs.IPA_DIR).exists())
        self.assertNoTempLeft()
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ["original", "work"])

    def test_rejects_unsafe_names(self):
        names = [
            "../evil",
            "Payload/../../evil",
            "/tmp/evil",
            "./evil",
            "Payload//evil",
            "C:evil",
            "Payload/AUX.txt",
            "Payload/trailing.",
            "Payload/bad?name",
        ]
        if os.name != "nt":  # zipfile on Windows can't write a name with a backslash
            names.append("..\\evil")
        for name in names:
            with self.subTest(name=name):
                self.check_rejected(((name, b"x", 0),), "unsafe entry")

    def test_rejects_symlink(self):
        link = ("Payload/MapleThief.app/link", b"../../../../etc/passwd", stat.S_IFLNK | 0o777)
        self.check_rejected((link,), "unsafe entry .*symbolic link")

    def test_rejects_special_file(self):
        self.check_rejected((("Payload/fifo", b"", stat.S_IFIFO | 0o644),), "not a regular file")

    def test_rejects_case_collision(self):
        self.check_rejected((("Payload/MapleThief.app/info.plist", b"y", 0),), "name the same file")

    def test_rejects_file_and_directory_clash(self):
        self.check_rejected(
            (("Payload/MapleThief.app/Info.plist/x", b"y", 0),), "both a file and a directory"
        )

    def test_lfs_pointer(self):
        (self.root / bs.IPA).write_bytes(LFS_POINTER)
        with self.assertRaisesRegex(bs.BootstrapError, "Git LFS pointer.*git lfs pull"):
            self.run_bootstrap()

    def test_wrong_size(self):
        (self.root / bs.IPA).write_bytes(b"PK\3\4 short")
        with self.assertRaisesRegex(bs.BootstrapError, f"size 10, expected {bs.IPA_SIZE}"):
            self.run_bootstrap()

    def test_not_a_zip(self):
        self.repo.set_ipa(b"\0" * bs.IPA_SIZE)
        with self.assertRaisesRegex(bs.BootstrapError, "cannot extract"):
            self.run_bootstrap()
        self.assertNoTempLeft()

    def test_main_reports_errors(self):
        (self.root / bs.IPA).write_bytes(LFS_POINTER)
        err = io.StringIO()
        old, sys.stderr = sys.stderr, err
        try:
            code = bs.main(["--repo", str(self.root), "--no-lipo-check"])
        finally:
            sys.stderr = old
        self.assertEqual(code, 1)
        self.assertIn("git lfs pull", err.getvalue())

    def test_read_sums_formats(self):
        path = self.root / "sums"
        path.write_text(f"# comment\n\n{'A' * 64}  a/b\n{'b' * 64} *c d\n", encoding="ascii")
        self.assertEqual(bs.read_sums(path), {"a/b": "a" * 64, "c d": "b" * 64})
        path.write_text("nothex  x\n", encoding="ascii")
        with self.assertRaisesRegex(bs.BootstrapError, "malformed"):
            bs.read_sums(path)


def have(*tools: str) -> bool:
    return all(shutil.which(t) for t in tools)


class ScriptTestBase(unittest.TestCase):
    """Runs a bootstrap script inside a SyntheticRepo."""

    ipa: bytes | None = None

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = SyntheticRepo(self._tmp.name, ipa=self.ipa, with_scripts=True)
        self.root = self.repo.root

    def tearDown(self):
        self._tmp.cleanup()

    def command(self) -> list[str]:
        raise NotImplementedError

    def run_script(self, *args: str, **env) -> subprocess.CompletedProcess:
        full_env = {**os.environ, "PYTHON": sys.executable, **env}
        full_env.pop("PYTHONPATH", None)
        return subprocess.run(
            [*self.command(), *args], cwd=self.root, env=full_env, capture_output=True, text=True
        )

    def assertFails(self, proc: subprocess.CompletedProcess, *needles: str):
        self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        for needle in needles:
            self.assertIn(needle, proc.stderr)

    def check_end_to_end(self):
        first = self.run_script()
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertEqual((self.root / "work/slices/MapleThief.armv7").read_bytes(), ARMV7_THIN)
        self.assertEqual((self.root / "work/slices/MapleThief.armv6").read_bytes(), ARMV6_THIN)
        self.assertTrue((self.root / bs.BINARY).is_file())
        digest = tree_digest(self.root / "work")
        second = self.run_script()
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(tree_digest(self.root / "work"), digest)
        self.assertEqual(second.stdout, first.stdout)
        # Nothing written into the copied tools/ (python -B).
        self.assertEqual(list((self.root / "tools").rglob("__pycache__")), [])

    def check_lfs_pointer(self):
        (self.root / bs.IPA).write_bytes(LFS_POINTER)
        proc = self.run_script()
        self.assertFails(
            proc,
            "error: original/MapleThief-1.5.ipa is a Git LFS pointer; "
            "run 'git lfs install && git lfs pull' first",
        )
        self.assertFalse((self.root / "work").exists())

    def check_wrong_size(self):
        (self.root / bs.IPA).write_bytes(b"PK\3\4 not the real IPA")
        self.assertFails(self.run_script(), f"size 21, expected {bs.IPA_SIZE}")

    def check_wrong_hash(self):
        self.repo.write_sums({bs.IPA: b"other"})
        self.assertFails(self.run_script(), "SHA-256 mismatch")

    def check_missing_python(self):
        proc = self.run_script(PYTHON=str(self.root / "no-such-python"))
        self.assertFails(proc, "need Python 3.10+", "uv python install", "python.org", "Homebrew")

    def check_old_python(self):
        """An interpreter that fails the version probe, like macOS CLT python3 3.9."""
        fake = self.root / "old-python"
        fake.write_text("#!/bin/sh\necho 3.9.6\nexit 1\n", encoding="ascii")
        fake.chmod(0o755)
        proc = self.run_script(PYTHON=str(fake))
        self.assertFails(proc, "need Python 3.10+", "uv python install")
        self.assertFalse((self.root / "work").exists())


@unittest.skipUnless(os.name == "posix" and have("bash"), "needs bash")
class PosixScriptTest(ScriptTestBase):
    def command(self):
        return ["bash", "scripts/bootstrap", "--no-lipo-check"]

    def test_end_to_end_twice_identical(self):
        self.check_end_to_end()

    def test_lfs_pointer(self):
        self.check_lfs_pointer()

    def test_wrong_size(self):
        self.check_wrong_size()

    def test_wrong_hash(self):
        self.check_wrong_hash()

    def test_missing_python(self):
        self.check_missing_python()

    def test_old_python(self):
        self.check_old_python()


@unittest.skipUnless(os.name == "posix" and have("bash"), "needs bash")
class VerifyOriginalScriptTest(ScriptTestBase):
    """scripts/verify-original.sh: the verify and extract stages of scripts/bootstrap."""

    def command(self):
        return ["bash", "scripts/verify-original.sh"]

    def test_verifies_and_extracts(self):
        proc = self.run_script()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(
            proc.stdout,
            "ok  original/MapleThief-1.5.ipa\n"
            "ok  work/ipa/Payload/MapleThief.app/MapleThief\n"
            "extracted to work/ipa/\n",
        )
        self.assertEqual((self.root / bs.BINARY).read_bytes(), SAMPLE_FAT)
        self.assertFalse((self.root / bs.SLICES_DIR).exists())

    def test_no_extract_writes_nothing(self):
        proc = self.run_script("--no-extract")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(proc.stdout, "ok  original/MapleThief-1.5.ipa\n")
        self.assertFalse((self.root / "work").exists())

    def test_unknown_argument(self):
        proc = self.run_script("--bogus")
        self.assertEqual(proc.returncode, 2)
        self.assertIn("usage: scripts/verify-original.sh [--no-extract]", proc.stderr)

    def test_lfs_pointer(self):
        self.check_lfs_pointer()

    def test_wrong_size(self):
        self.check_wrong_size()


@unittest.skipUnless(have("pwsh"), "needs pwsh (PowerShell 7+)")
class PowerShellScriptTest(ScriptTestBase):
    def command(self):
        return [
            "pwsh",
            "-NoProfile",
            "-NonInteractive",
            "-File",
            "scripts/bootstrap.ps1",
            "-NoLipoCheck",
        ]

    def test_end_to_end_twice_identical(self):
        self.check_end_to_end()

    def test_lfs_pointer(self):
        self.check_lfs_pointer()

    def test_wrong_size(self):
        self.check_wrong_size()

    def test_wrong_hash(self):
        self.check_wrong_hash()

    def test_missing_python(self):
        self.check_missing_python()

    @unittest.skipIf(os.name == "nt", "the fake interpreter is a shell script")
    def test_old_python(self):
        self.check_old_python()


if __name__ == "__main__":
    unittest.main()
