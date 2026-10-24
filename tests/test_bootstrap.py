"""Tests for the bootstrap step on synthetic repos (no IPA needed).

The script-level tests copy scripts/ and tools/mtport/ into a temporary repo
whose original/ holds a synthetic stand-in for the IPA: a zip padded to the
real IPA's size whose main binary is a synthetic fat Mach-O, with a matching
SHA256SUMS. That runs scripts/bootstrap (and scripts/bootstrap.ps1 when pwsh is
installed) end to end in CI.
"""

from __future__ import annotations

import functools
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

from machofixtures import ARMV6_THIN, ARMV7_THIN, SAMPLE_FAT
from mtport import bootstrap as bs

REPO = Path(__file__).resolve().parents[1]
IPA_SIZE = 17872861  # the real IPA's size, hard-coded in scripts/verify-original.sh
LFS_POINTER = (
    b"version https://git-lfs.github.com/spec/v1\n"
    b"oid sha256:a6a06f050aa21af9576dbf8af3c832dcbbf38f453886ef1690447f934cb3e387\n"
    b"size 17872861\n"
)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@functools.cache
def synthetic_ipa(binary: bytes, size: int = IPA_SIZE) -> bytes:
    """A zip with Payload/MapleThief.app/MapleThief, padded to exactly ``size`` bytes."""

    def build(pad: int) -> bytes:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            for name, data in (
                ("Payload/MapleThief.app/MapleThief", binary),
                ("padding.bin", b"\0" * pad),
            ):
                info = zipfile.ZipInfo(name, date_time=(2010, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_STORED
                z.writestr(info, data)
        return buf.getvalue()

    data = build(size - len(build(0)))
    assert len(data) == size
    return data


def sums_text(entries: dict[str, bytes]) -> str:
    return "".join(f"{sha(data)}  {path}\n" for path, data in entries.items())


def tree_digest(root: Path) -> dict[str, str]:
    """``{relative path: sha256}`` for every file under ``root``."""
    return {
        p.relative_to(root).as_posix(): sha(p.read_bytes())
        for p in sorted(root.rglob("*"))
        if p.is_file()
    }


class SyntheticRepo:
    """A temp dir laid out like the repo, with a synthetic IPA."""

    def __init__(self, tmp: str, ipa: bytes | None = None, with_scripts: bool = False):
        self.root = Path(tmp)
        self.ipa = synthetic_ipa(SAMPLE_FAT) if ipa is None else ipa
        (self.root / "original").mkdir()
        (self.root / bs.IPA).write_bytes(self.ipa)
        self.write_sums(
            {
                bs.IPA: self.ipa,
                bs.BINARY: SAMPLE_FAT,
                bs.slice_path("armv6"): ARMV6_THIN,
                bs.slice_path("armv7"): ARMV7_THIN,
            }
        )
        if with_scripts:
            shutil.copytree(REPO / "scripts", self.root / "scripts")
            shutil.copytree(
                REPO / "tools" / "mtport",
                self.root / "tools" / "mtport",
                ignore=shutil.ignore_patterns("__pycache__"),
            )

    def write_sums(self, entries: dict[str, bytes]) -> None:
        (self.root / bs.SUMS).write_text(sums_text(entries), encoding="ascii")

    def extract(self) -> None:
        """What verify-original.sh does: put the binary in work/ipa/."""
        binary = self.root / bs.BINARY
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(SAMPLE_FAT)


def quiet(*_args) -> None:
    pass


class BootstrapModuleTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = SyntheticRepo(self._tmp.name)
        self.root = self.repo.root

    def tearDown(self):
        self._tmp.cleanup()

    def run_bootstrap(self):
        return bs.bootstrap(self.root, lipo_check=False, log=quiet)

    def test_writes_slices_sums_and_json(self):
        self.repo.extract()
        doc = self.run_bootstrap()
        self.assertEqual((self.root / "work/slices/MapleThief.armv6").read_bytes(), ARMV6_THIN)
        self.assertEqual((self.root / "work/slices/MapleThief.armv7").read_bytes(), ARMV7_THIN)
        self.assertEqual(
            (self.root / bs.SLICE_SUMS).read_text(),
            f"{sha(ARMV6_THIN)}  work/slices/MapleThief.armv6\n"
            f"{sha(ARMV7_THIN)}  work/slices/MapleThief.armv7\n",
        )
        on_disk = json.loads((self.root / bs.BOOTSTRAP_JSON).read_text())
        self.assertEqual(on_disk, doc)
        self.assertEqual(doc["schema_version"], 1)
        self.assertEqual(doc["tool"]["name"], "mtport.bootstrap")
        self.assertEqual(doc["primary_slice"], "armv7")
        self.assertEqual(
            doc["ipa"], {"path": bs.IPA, "size": IPA_SIZE, "sha256": sha(self.repo.ipa)}
        )
        self.assertEqual(doc["binary"]["sha256"], sha(SAMPLE_FAT))
        self.assertEqual(doc["slices"]["armv6"]["fat_offset"], 4096)
        self.assertEqual(doc["slices"]["armv7"]["size"], len(ARMV7_THIN))
        self.assertEqual(doc["slices"]["armv7"]["cpusubtype"], 9)

    def test_two_runs_give_identical_outputs(self):
        self.repo.extract()
        self.run_bootstrap()
        first = tree_digest(self.root / "work")
        self.run_bootstrap()
        self.assertEqual(tree_digest(self.root / "work"), first)
        self.assertIn("bootstrap.json", first)

    def test_slice_hash_mismatch_writes_nothing(self):
        self.repo.extract()
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
        self.repo.extract()
        self.repo.write_sums({bs.IPA: self.repo.ipa, bs.BINARY: SAMPLE_FAT})
        with self.assertRaisesRegex(bs.BootstrapError, "no entry for work/slices/MapleThief.armv6"):
            self.run_bootstrap()

    def test_binary_hash_mismatch(self):
        self.repo.extract()
        (self.root / bs.BINARY).write_bytes(SAMPLE_FAT + b"\0")
        with self.assertRaisesRegex(bs.BootstrapError, "MapleThief: SHA-256 mismatch"):
            self.run_bootstrap()

    def test_not_extracted_yet(self):
        with self.assertRaisesRegex(bs.BootstrapError, "extract the IPA first"):
            self.run_bootstrap()

    def test_lfs_pointer(self):
        (self.root / bs.IPA).write_bytes(LFS_POINTER)
        with self.assertRaisesRegex(bs.BootstrapError, "Git LFS pointer.*git lfs pull"):
            self.run_bootstrap()

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

    def run_script(self, **env) -> subprocess.CompletedProcess:
        full_env = {**os.environ, "PYTHON": sys.executable, **env}
        full_env.pop("PYTHONPATH", None)
        return subprocess.run(
            self.command(), cwd=self.root, env=full_env, capture_output=True, text=True
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
        digest = tree_digest(self.root / "work")
        second = self.run_script()
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(tree_digest(self.root / "work"), digest)

    def check_lfs_pointer(self):
        (self.root / bs.IPA).write_bytes(LFS_POINTER)
        self.assertFails(self.run_script(), "Git LFS pointer", "git lfs install && git lfs pull")

    def check_wrong_size(self):
        (self.root / bs.IPA).write_bytes(b"PK\3\4 not the real IPA")
        self.assertFails(self.run_script(), "size 21, expected 17872861")

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


@unittest.skipUnless(os.name == "posix" and have("bash", "unzip"), "needs bash and unzip")
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
