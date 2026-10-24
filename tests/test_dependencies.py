"""The Python test environment matches the pins (ADR 0003) and Capstone decodes armv7."""

import importlib.metadata
import re

import pytest

from repo_paths import REPO


def third_party_version(name: str) -> str:
    """The Version column of ``name``'s row in THIRD_PARTY.md."""
    text = (REPO / "THIRD_PARTY.md").read_text(encoding="utf-8")
    match = re.search(rf"^\| \*\*{re.escape(name)}\*\*[^|]*\| ([^ |]+) \|", text, re.MULTILINE)
    assert match, f"no row for {name} in THIRD_PARTY.md"
    return match.group(1)


@pytest.mark.parametrize(("dist", "row"), [("pytest", "pytest"), ("capstone", "Capstone")])
def test_installed_version_matches_third_party_md(dist, row):
    # capstone.__version__ reports 5.0.7 in the 5.0.9 wheel (ADR 0003); use the metadata.
    assert importlib.metadata.version(dist) == third_party_version(row)


def test_capstone_decodes_thumb2_and_vfp():
    import capstone

    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    code = bytes.fromhex("2de9f0402ded0a8b")  # push.w {r4-r7, lr}; vpush {d8-d12}
    insns = [(i.mnemonic, i.op_str) for i in md.disasm(code, 0x2A3C0)]
    assert insns == [("push.w", "{r4, r5, r6, r7, lr}"), ("vpush", "{d8, d9, d10, d11, d12}")]
