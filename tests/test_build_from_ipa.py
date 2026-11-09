"""Building from your own copy of the IPA (#85, ADR 0135): which files are accepted and what each
refusal says, how the file reaches original/, and paths with spaces through every script. All on
synthetic zips and repositories; the run on the real IPA is an acceptance step."""

from __future__ import annotations

import hashlib
import json
import os
import plistlib
import shlex
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest

from machofixtures import (
    ARMV6_THIN,
    ARMV7_THIN,
    CPU_SUBTYPE_ARM_V6,
    CPU_SUBTYPE_ARM_V7,
    CPU_TYPE_ARM,
    MH_MAGIC,
    SAMPLE_FAT,
    fat_macho,
)
from mtport import bootstrap as bs
from mtport import build as bd
from repo_paths import REPO

needs_bash = pytest.mark.skipif(os.name == "nt" or not shutil.which("bash"), reason="needs bash")
needs_pwsh = pytest.mark.skipif(not shutil.which("pwsh"), reason="needs pwsh (PowerShell 7+)")
needs_just = pytest.mark.skipif(not shutil.which("just"), reason="needs just")

PLIST_15 = plistlib.dumps({"CFBundleVersion": "1.5"})


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def make_zip(path: Path, entries: dict[str, bytes]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries.items():
            z.writestr(name, data)
    return path


def game_entries(binary: bytes = SAMPLE_FAT, plist: bytes = PLIST_15) -> dict[str, bytes]:
    return {bs.BINARY_ENTRY: binary, bs.PLIST_ENTRY: plist, "iTunesArtwork": b"art"}


def encrypted_thin(subtype: int, cryptid: int) -> bytes:
    command = struct.pack("<IIIII", 0x21, 20, 0x1000, 0x100, cryptid)  # LC_ENCRYPTION_INFO
    return struct.pack("<IiiIIII", MH_MAGIC, CPU_TYPE_ARM, subtype, 2, 1, len(command), 0) + command


def fat_of(armv6: bytes, armv7: bytes) -> bytes:
    return fat_macho(
        [(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V6, armv6), (CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, armv7)]
    )


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    """A checkout's original/ without the IPA, whose SHA256SUMS expects the synthetic game."""
    root = tmp_path / "repo"
    (root / "original").mkdir(parents=True)
    sums = {
        bs.IPA: sha(b"the exact file, which no test here has"),
        bs.BINARY: sha(SAMPLE_FAT),
        bs.slice_path("armv6"): sha(ARMV6_THIN),
        bs.slice_path("armv7"): sha(ARMV7_THIN),
    }
    text = "".join(f"{digest}  {path}\n" for path, digest in sums.items())
    (root / bs.SUMS).write_text(text, encoding="ascii")
    return root


def check(repo: Path, path: Path) -> tuple[int, str]:
    return bs.check_ipa_file(path, bs.load_sums(repo), "my.ipa")


def assert_refused(repo: Path, path: Path, *needles: str) -> None:
    with pytest.raises(bs.BootstrapError) as e:
        check(repo, path)
    message = str(e.value)
    for needle in needles:
        assert needle in message
    assert "docs/play/your-ipa.md" in message
    for word in ("decrypt", "download", "http"):
        assert word not in message.lower(), message


# --- Which files are accepted ----------------------------------------------------------------


def test_a_repacked_copy_is_accepted(repo, tmp_path):
    path = make_zip(tmp_path / "a.ipa", game_entries())
    assert check(repo, path) == (path.stat().st_size, sha(path.read_bytes()))


def test_a_repacked_copy_is_accepted_whatever_its_plist_says(repo, tmp_path):
    # The binary decides: the hash can't match on anything but the game.
    path = make_zip(tmp_path / "a.ipa", game_entries(plist=b"not a plist"))
    check(repo, path)


def test_the_exact_file_passes_without_being_opened(repo, tmp_path, monkeypatch):
    path = tmp_path / "exact.ipa"
    path.write_bytes(b"x" * 16)
    sums = {bs.IPA: sha(b"x" * 16), bs.BINARY: "0" * 64}
    monkeypatch.setattr(bs, "IPA_SIZE", 16)
    assert bs.check_ipa_file(path, sums, "my.ipa") == (16, sha(b"x" * 16))


def test_not_a_zip(repo, tmp_path):
    path = tmp_path / "a.ipa"
    path.write_bytes(b"this is no zip")
    assert_refused(repo, path, "my.ipa:", "not an IPA", "not a zip file")


def test_a_zip_that_is_no_ipa(repo, tmp_path):
    path = make_zip(tmp_path / "a.ipa", {"hello.txt": b"x"})
    assert_refused(repo, path, "not an iPhone app")


def test_another_app(repo, tmp_path):
    path = make_zip(tmp_path / "a.ipa", {"Payload/Other.app/Other": b"x"})
    assert_refused(repo, path, "another app (Other.app)")


def test_no_main_binary(repo, tmp_path):
    path = make_zip(tmp_path / "a.ipa", {bs.PLIST_ENTRY: PLIST_15})
    assert_refused(repo, path, "no main binary", bs.BINARY_ENTRY)


def test_zip_password(repo, tmp_path):
    path = make_zip(tmp_path / "a.ipa", game_entries())
    data = bytearray(path.read_bytes())
    at = data.find(b"PK\1\2")  # the first central directory record's flags, at +8
    data[at + 8] |= 1
    path.write_bytes(data)
    assert_refused(repo, path, "encrypted with a zip password")


@pytest.mark.parametrize(
    "binary",
    [
        fat_of(encrypted_thin(CPU_SUBTYPE_ARM_V6, 1), encrypted_thin(CPU_SUBTYPE_ARM_V7, 0)),
        fat_of(encrypted_thin(CPU_SUBTYPE_ARM_V6, 0), encrypted_thin(CPU_SUBTYPE_ARM_V7, 1)),
        encrypted_thin(CPU_SUBTYPE_ARM_V7, 1),
    ],
    ids=["first slice", "last slice", "thin"],
)
def test_cryptid_in_any_slice(repo, tmp_path, binary):
    path = make_zip(tmp_path / "a.ipa", game_entries(binary))
    assert_refused(repo, path, "encrypted (cryptid 1)", "can't be used")


def test_cryptid_0_is_not_encrypted(repo, tmp_path):
    binary = fat_of(encrypted_thin(CPU_SUBTYPE_ARM_V6, 0), encrypted_thin(CPU_SUBTYPE_ARM_V7, 0))
    path = make_zip(tmp_path / "a.ipa", game_entries(binary))
    assert_refused(repo, path, "is not the known one")


@pytest.mark.parametrize("fmt", [plistlib.FMT_XML, plistlib.FMT_BINARY], ids=["xml", "binary"])
def test_wrong_version(repo, tmp_path, fmt):
    plist = plistlib.dumps({"CFBundleVersion": "1.3"}, fmt=fmt)
    path = make_zip(tmp_path / "a.ipa", game_entries(b"another binary", plist))
    assert_refused(repo, path, "MapleThief 1.3", "needs 1.5")


@pytest.mark.parametrize("plist", [b"not a plist", plistlib.dumps({"Other": "x"})])
def test_no_readable_version(repo, tmp_path, plist):
    path = make_zip(tmp_path / "a.ipa", game_entries(b"another binary", plist))
    assert_refused(repo, path, "gives no version")


def test_version_from_the_short_version_string(repo, tmp_path):
    plist = plistlib.dumps({"CFBundleShortVersionString": "1.4"})
    path = make_zip(tmp_path / "a.ipa", game_entries(b"another binary", plist))
    assert_refused(repo, path, "MapleThief 1.4")


def test_right_version_changed_binary(repo, tmp_path):
    path = make_zip(tmp_path / "a.ipa", game_entries(b"another binary"))
    assert_refused(repo, path, "MapleThief 1.5", "damaged or changed")


def test_a_right_binary_in_an_unsafe_zip_is_refused(repo, tmp_path):
    entries = {**game_entries(), "Payload/MapleThief.app/../../evil": b"x"}
    path = make_zip(tmp_path / "a.ipa", entries)
    with pytest.raises(bs.BootstrapError, match="my.ipa: unsafe entry"):
        check(repo, path)


# --- bootstrap on a repacked copy ------------------------------------------------------------


def test_bootstrap_takes_a_repacked_copy_and_its_stamp_notices_another(repo, tmp_path):
    (repo / "tools").mkdir()
    make_zip(repo / bs.IPA, game_entries())
    log: list[str] = []
    doc = bs.bootstrap(repo, lipo_check=False, log=log.append)
    assert doc["ipa"]["sha256"] == sha((repo / bs.IPA).read_bytes())
    assert (repo / bs.slice_path("armv7")).read_bytes() == ARMV7_THIN
    assert any("repacked" in line for line in log)
    assert bd.bootstrap_fresh(repo)

    make_zip(repo / bs.IPA, {**game_entries(), "extra.txt": b"repacked again"})
    assert not bd.bootstrap_fresh(repo)


def test_doctor_says_the_copy_is_repacked(repo, monkeypatch):
    make_zip(repo / bs.IPA, game_entries())
    ipa = next(c for c in bd.doctor_checks(repo) if c.name == "The IPA")
    assert ipa.ok
    assert "repacked" in ipa.detail


# --- --ipa -----------------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("argv", "platform", "config", "ipa"),
    [
        (["mac", "--ipa", "/a b/x.ipa"], "mac", "release", "/a b/x.ipa"),
        (["--ipa", "x.ipa", "mac", "debug"], "mac", "debug", "x.ipa"),
        (["mac", "--ipa", "x.ipa", "debug"], "mac", "debug", "x.ipa"),
        (["mac", "--ipa=/a b/x.ipa"], "mac", "release", "/a b/x.ipa"),
        (["--ipa", "Release"], None, "release", "Release"),  # a path, not the word
        (["--ipa", "-linux"], None, "release", "-linux"),
    ],
)
def test_parse_ipa(argv, platform, config, ipa):
    opts = bd.parse_args(argv)
    assert (opts.command, opts.platform, opts.config, opts.ipa) == ("build", platform, config, ipa)


def test_parse_ipa_bootstrap():
    assert bd.parse_args(["bootstrap", "--ipa", "x"]).ipa == "x"


@pytest.mark.parametrize("argv", [["run", "--ipa", "x"], ["doctor", "--ipa", "x"], ["--ipa", ""]])
def test_parse_ipa_rejected(argv):
    with pytest.raises(bd.BuildError):
        bd.parse_args(argv)


def test_ipa_without_a_path_is_an_error():
    with pytest.raises(SystemExit):
        bd.parse_args(["--ipa"])


def test_needs_msvc_with_ipa(monkeypatch):
    monkeypatch.setattr(bd, "host_platform", lambda: "win")
    assert bd.needs_msvc(bd.parse_args(["win", "--ipa", "C:\\a b\\x.ipa"]))


def test_adopt_copies_a_good_file_and_leaves_no_temporary(repo, tmp_path):
    source = make_zip(tmp_path / "my copy.ipa", game_entries())
    bd.adopt_ipa(repo, str(source))
    assert (repo / bs.IPA).read_bytes() == source.read_bytes()
    assert sorted(p.name for p in (repo / "original").iterdir()) == [
        "MapleThief-1.5.ipa",
        "SHA256SUMS",
    ]


def test_adopt_refuses_without_copying(repo, tmp_path):
    source = tmp_path / "bad.ipa"
    source.write_bytes(b"not a zip")
    with pytest.raises(bd.BuildError, match="not a zip file"):
        bd.adopt_ipa(repo, str(source))
    assert not (repo / bs.IPA).exists()


def test_adopt_missing_path(repo, tmp_path):
    with pytest.raises(bd.BuildError, match="no file at this path.*your-ipa.md") as e:
        bd.adopt_ipa(repo, str(tmp_path / "nothing here.ipa"))
    assert "nothing here.ipa" in str(e.value)
    assert not (repo / bs.IPA).exists()


def test_adopt_a_folder(repo, tmp_path):
    with pytest.raises(bd.BuildError, match="is a folder"):
        bd.adopt_ipa(repo, str(tmp_path))


def test_adopt_reads_a_relative_path_from_the_callers_folder(repo, tmp_path, monkeypatch):
    make_zip(tmp_path / "caller" / "a.ipa", game_entries())
    monkeypatch.setenv(bd.CALLER_DIR_ENV, str(tmp_path / "caller"))
    bd.adopt_ipa(repo, "a.ipa")
    assert (repo / bs.IPA).is_file()


def test_adopt_refuses_to_replace_a_different_file(repo, tmp_path):
    source = make_zip(tmp_path / "a.ipa", game_entries())
    (repo / bs.IPA).write_bytes(b"something of the user's")
    with pytest.raises(bd.BuildError, match="already there and is not this game.*Move it away"):
        bd.adopt_ipa(repo, str(source))
    assert (repo / bs.IPA).read_bytes() == b"something of the user's"


def test_adopt_refuses_a_dangling_link_in_the_way(repo, tmp_path):
    source = make_zip(tmp_path / "a.ipa", game_entries())
    try:
        os.symlink(tmp_path / "gone", repo / bs.IPA)
    except OSError:
        pytest.skip("this system can't make symbolic links")
    with pytest.raises(bd.BuildError, match="already there"):
        bd.adopt_ipa(repo, str(source))
    assert (repo / bs.IPA).is_symlink()


def test_adopt_keeps_the_same_game_already_there(repo, tmp_path, capsys):
    source = make_zip(tmp_path / "a.ipa", {**game_entries(), "extra": b"repacked"})
    make_zip(repo / bs.IPA, game_entries())
    before = (repo / bs.IPA).read_bytes()
    assert source.read_bytes() != before
    bd.adopt_ipa(repo, str(source))
    assert (repo / bs.IPA).read_bytes() == before
    assert "already holds this game" in capsys.readouterr().out


def test_adopt_the_file_already_in_place(repo):
    make_zip(repo / bs.IPA, game_entries())
    before = (repo / bs.IPA).read_bytes()
    bd.adopt_ipa(repo, str(repo / bs.IPA))
    assert (repo / bs.IPA).read_bytes() == before


# --- Missing tools ---------------------------------------------------------------------------


def _checks() -> list[bd.Check]:
    return [
        bd.Check("Python", True, "3.12"),
        bd.check_version("CMake", "cmake", (3, 28), "brew install cmake", version=(3, 1)),
        bd.Check("Docker", False, "not found", "get docker", False, ("linux",)),
        bd.Check("The IPA", False, "missing", "see the docs", needed_for=()),
    ]


def test_check_tools_names_the_missing_tool_and_its_fix(repo, monkeypatch):
    monkeypatch.setattr(bd, "doctor_checks", lambda r: _checks())
    with pytest.raises(bd.BuildError) as e:
        bd.check_tools(repo, "mac")
    message = str(e.value)
    assert "CMake" in message and "fix: brew install cmake" in message
    assert "Docker" not in message  # not what a macOS build needs
    assert "The IPA" not in message  # not in place yet: --ipa checks its own file


def test_check_tools_linux_needs_docker_not_cmake(repo, monkeypatch):
    monkeypatch.setattr(bd, "doctor_checks", lambda r: _checks())
    with pytest.raises(bd.BuildError, match="fix: get docker") as e:
        bd.check_tools(repo, "linux")
    assert "CMake" not in str(e.value)


def test_check_tools_passes_when_all_is_there(repo, monkeypatch):
    monkeypatch.setattr(bd, "doctor_checks", lambda r: [bd.Check("Python", True, "3.12")])
    bd.check_tools(repo, "mac")


def test_build_stops_on_missing_tools_before_copying(repo, tmp_path, monkeypatch):
    source = make_zip(tmp_path / "a.ipa", game_entries())
    monkeypatch.setattr(bd, "doctor_checks", lambda r: _checks())
    monkeypatch.setattr(bd, "host_platform", lambda: "mac")
    with pytest.raises(bd.BuildError, match="Nothing was copied"):
        bd.cmd_build(repo, bd.parse_args(["mac", "--ipa", str(source)]))
    assert not (repo / bs.IPA).exists()


# --- The scripts, with a space in the path -----------------------------------------------------


@pytest.fixture
def script_repo(repo: Path) -> Path:
    shutil.copytree(REPO / "scripts", repo / "scripts")
    shutil.copytree(
        REPO / "tools" / "mtport",
        repo / "tools" / "mtport",
        ignore=shutil.ignore_patterns("__pycache__"),
    )
    return repo


def script_env() -> dict[str, str]:
    env = {**os.environ, "PYTHON": sys.executable}
    env.pop("PYTHONPATH", None)
    return env


def run_script(argv: list[str], cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        argv, cwd=cwd, env=script_env(), capture_output=True, text=True, timeout=120
    )


def assert_bootstrapped(repo: Path, out: subprocess.CompletedProcess) -> None:
    assert out.returncode == 0, out.stdout + out.stderr
    assert (repo / bs.IPA).is_file()
    assert (repo / bs.slice_path("armv7")).read_bytes() == ARMV7_THIN
    doc = json.loads((repo / bs.BOOTSTRAP_JSON).read_text(encoding="utf-8"))
    assert doc["ipa"]["sha256"] == sha((repo / bs.IPA).read_bytes())


def spaced_ipa(tmp_path: Path) -> Path:
    return make_zip(tmp_path / "my games" / "the copy.ipa", game_entries())


@needs_bash
def test_scripts_build_with_a_space(script_repo, tmp_path):
    ipa = spaced_ipa(tmp_path)
    out = run_script(["bash", "scripts/build", "bootstrap", "--ipa", str(ipa)], script_repo)
    assert_bootstrapped(script_repo, out)


@needs_bash
def test_scripts_build_from_ipa_with_a_space(script_repo, tmp_path):
    ipa = spaced_ipa(tmp_path)
    out = run_script(["bash", "scripts/build-from-ipa", str(ipa), "bootstrap"], script_repo)
    assert_bootstrapped(script_repo, out)


@needs_bash
def test_scripts_build_from_ipa_reads_a_relative_path_from_the_callers_folder(
    script_repo, tmp_path
):
    ipa = spaced_ipa(tmp_path)
    script = str(script_repo / "scripts" / "build-from-ipa")
    out = run_script(["bash", script, ipa.name, "bootstrap"], ipa.parent)
    assert_bootstrapped(script_repo, out)


@needs_bash
def test_scripts_build_from_ipa_refusal_is_the_modules(script_repo, tmp_path):
    missing = tmp_path / "no such" / "file.ipa"
    out = run_script(["bash", "scripts/build-from-ipa", str(missing), "bootstrap"], script_repo)
    assert out.returncode == 1
    assert f"scripts/build: {missing}: there is no file at this path" in out.stderr
    assert not (script_repo / bs.IPA).exists()


@needs_bash
def test_scripts_build_from_ipa_needs_a_path():
    out = subprocess.run(
        ["bash", "scripts/build-from-ipa"], cwd=REPO, capture_output=True, text=True, timeout=60
    )
    assert out.returncode == 2
    assert "usage: scripts/build-from-ipa PATH" in out.stderr


@needs_pwsh
@pytest.mark.parametrize(
    "script", ["scripts/build.ps1", "scripts/build-from-ipa.ps1"], ids=["build", "from-ipa"]
)
def test_ps1_scripts_parse(script):
    command = (
        "$errors = $null; "
        "[System.Management.Automation.Language.Parser]::ParseFile("
        f"'{script}', [ref]$null, [ref]$errors) | Out-Null; "
        "if ($errors) { $errors; exit 1 }"
    )
    out = subprocess.run(
        ["pwsh", "-NoProfile", "-NonInteractive", "-Command", command],
        cwd=REPO,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert out.returncode == 0, out.stdout + out.stderr


PWSH = ["pwsh", "-NoProfile", "-NonInteractive", "-File"]


@needs_pwsh
def test_build_ps1_with_a_space(script_repo, tmp_path):
    ipa = spaced_ipa(tmp_path)
    out = run_script([*PWSH, "scripts/build.ps1", "bootstrap", "--ipa", str(ipa)], script_repo)
    assert_bootstrapped(script_repo, out)


@needs_pwsh
def test_build_from_ipa_ps1_with_a_space(script_repo, tmp_path):
    ipa = spaced_ipa(tmp_path)
    out = run_script([*PWSH, "scripts/build-from-ipa.ps1", str(ipa), "bootstrap"], script_repo)
    assert_bootstrapped(script_repo, out)


@needs_pwsh
def test_build_ps1_reads_a_relative_path_from_the_callers_folder(script_repo, tmp_path):
    ipa = spaced_ipa(tmp_path)
    script = str(script_repo / "scripts" / "build-from-ipa.ps1")
    out = run_script([*PWSH, script, ipa.name, "bootstrap"], ipa.parent)
    assert_bootstrapped(script_repo, out)


@needs_just
@pytest.mark.skipif(os.name == "nt", reason="the POSIX spellings")
def test_just_needs_inner_quotes_for_a_space():
    # The documented form: `just build mac --ipa "'/my games/x.ipa'"`.
    out = subprocess.run(
        ["just", "--dry-run", "build", "mac", "--ipa", "'/my games/x.ipa'"],
        cwd=REPO,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert out.returncode == 0, out.stderr
    line = (out.stdout + out.stderr).strip()
    assert shlex.split(line) == ["scripts/build", "build", "mac", "--ipa", "/my games/x.ipa"]
