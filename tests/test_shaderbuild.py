"""The GPU shaders' build path (mtport.shaderbuild, #120, ADR 0060) without the shader tools:
source naming, the header's stamps and the up-to-date check, the descriptor-set check on
SPIRV-Cross reflection, and the committed header being up to date with the committed sources."""

from __future__ import annotations

from pathlib import Path

import pytest

from mtport import shaderbuild as sb
from repo_paths import REPO


def write_sources(d: Path, files: dict[str, str]) -> None:
    d.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        (d / name).write_text(text, encoding="utf-8")


def compiled(src: sb.Source, samplers: int = 1) -> sb.Compiled:
    return sb.Compiled(
        src, sb.sha256(src.path), b"\x03\x02", b"DXBC", b"#include", sb.Resources(samplers)
    )


def test_sources_are_named_by_stage_and_sorted(tmp_path) -> None:
    write_sources(tmp_path, {"b.frag.hlsl": "x", "a.vert.hlsl": "y", "notes.txt": "z"})
    found = sb.sources(tmp_path)
    assert [(s.file, s.name, s.stage, s.ident) for s in found] == [
        ("a.vert.hlsl", "a", "vert", "a_vert"),
        ("b.frag.hlsl", "b", "frag", "b_frag"),
    ]
    with pytest.raises(sb.BuildError, match="no shaders"):
        sb.sources(tmp_path / "empty")
    write_sources(tmp_path, {"Bad.pixel.hlsl": ""})
    with pytest.raises(sb.BuildError, match="not named"):
        sb.sources(tmp_path)


def test_a_rendered_header_is_up_to_date_until_a_source_changes(tmp_path) -> None:
    src_dir, header = tmp_path / "shaders", tmp_path / "shaders_compiled.h"
    write_sources(src_dir, {"gfx.vert.hlsl": "v", "gfx.frag.hlsl": "f"})
    text = sb.render_header([compiled(s) for s in sb.sources(src_dir)], ["dxc 1.0"])
    assert "// source gfx.frag.hlsl sha256 " in text
    # Generated: clang-format leaves it alone (scripts/lint checks every tracked .h).
    assert text.startswith("// clang-format off\n") and text.endswith("// clang-format on\n")
    assert '{"gfx.frag", MT_GFX_GPU_STAGE_FRAGMENT, k_gfx_frag_spirv,' in text
    assert (
        "static const unsigned char k_gfx_vert_dxil[4] = {\n    0x44, 0x58, 0x42, 0x43,\n};" in text
    )
    header.write_text(text, encoding="utf-8")
    assert set(sb.stamps(text)) == {"gfx.vert.hlsl", "gfx.frag.hlsl"}
    assert sb.check(src_dir, header) == []
    # CR LF line endings (a Windows checkout) are the same source.
    (src_dir / "gfx.vert.hlsl").write_bytes(b"line\r\nv")
    first = sb.sha256(src_dir / "gfx.vert.hlsl")
    (src_dir / "gfx.vert.hlsl").write_bytes(b"line\nv")
    assert sb.sha256(src_dir / "gfx.vert.hlsl") == first
    (src_dir / "gfx.vert.hlsl").write_text("v", encoding="utf-8")
    # An edit, a new source and a removed one are each reported.
    (src_dir / "gfx.frag.hlsl").write_text("f2", encoding="utf-8")
    (src_dir / "blur.frag.hlsl").write_text("b", encoding="utf-8")
    (src_dir / "gfx.vert.hlsl").unlink()
    problems = sb.check(src_dir, header)
    assert problems == [
        f"blur.frag.hlsl: not in {header} (run scripts/build-shaders)",
        f"gfx.frag.hlsl: changed since {header} was built (run scripts/build-shaders)",
        f"gfx.vert.hlsl: in {header} but no longer a source (run scripts/build-shaders)",
    ]
    assert ": cannot read: " in sb.check(src_dir, tmp_path / "missing.h")[0]


def test_reflection_counts_resources_and_checks_sdls_descriptor_sets() -> None:
    frag = {"textures": [{"name": "tex", "set": 2}], "ubos": [{"name": "params", "set": 3}]}
    assert sb.resources_from_reflection(frag, "frag", "f.frag.hlsl") == sb.Resources(1, 0, 0, 1)
    assert sb.resources_from_reflection({}, "vert", "v.vert.hlsl") == sb.Resources()
    vert = {"ubos": [{"name": "m", "set": 1}]}
    assert sb.resources_from_reflection(vert, "vert", "v.vert.hlsl").uniform_buffers == 1
    with pytest.raises(sb.BuildError, match="descriptor set 0, SDL wants set 2"):
        sb.resources_from_reflection({"textures": [{"name": "t", "set": 0}]}, "frag", "f")
    with pytest.raises(sb.BuildError, match="combinedImageSampler"):
        sb.resources_from_reflection({"separate_images": [{"name": "t", "set": 2}]}, "frag", "f")


def test_the_committed_header_matches_the_committed_sources() -> None:
    assert sb.check(REPO / sb.SOURCE_DIR, REPO / sb.HEADER) == []
    assert sb.main(["check", "--repo", str(REPO)]) == 0
