"""The GPU shaders' build path (mtport.shaderbuild, #120, ADR 0060; DKSH #242, ADR 0143) without
the shader tools: source naming, the header's stamps and the up-to-date check, the descriptor-set
check on SPIRV-Cross reflection, the GLSL binding check for deko3d (each resource kind from slot
0), the DKSH header check, SDL's renderer shaders from the pinned sdl3-nx archive, and the
committed headers being up to date with the committed sources and the SDL pin."""

from __future__ import annotations

import io
import os
import shutil
import struct
import subprocess
import tarfile
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
        src,
        sb.sha256(src.path),
        b"\x03\x02",
        b"DXBC",
        b"#include",
        sb.Resources(samplers),
        b"DKSH",
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
    assert "k_gfx_frag_dksh, sizeof k_gfx_frag_dksh, 1, 0, 0, 0}," in text
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
    assert sb.check_sdl(REPO / sb.SDL_HEADER, REPO / sb.DEPENDENCIES) == []
    assert sb.main(["check", "--repo", str(REPO)]) == 0


# --- deko3d: GLSL bindings, DKSH, SDL's renderer shaders (#242, ADR 0143) ----------------------

# GLSL as SPIRV-Cross writes it from the library's SPIR-V (--version 460 --no-es): a filter's
# uniform block and its texture, both from slot 0 although SPIR-V had them in sets 3 and 2.
FILTER_GLSL = """#version 460

layout(binding = 0, std140) uniform type_Sizes
{
    vec4 source_size;
} Sizes;

layout(binding = 0) uniform sampler2D u_texture;

layout(location = 0) in vec4 in_var_COLOR0;
"""


def test_glsl_bindings_are_read_per_resource_kind() -> None:
    assert sb.glsl_bindings(FILTER_GLSL, "f") == {"uniform buffer": [0], "sampler": [0]}
    storage = (
        "layout(binding = 1, std430) readonly buffer B { float x[]; } b;\n"
        "layout(binding = 0, rgba8) uniform writeonly image2D img;\n"
        "layout(binding = 2) uniform usampler2D lut;\n"
    )
    assert sb.glsl_bindings(storage, "s") == {
        "storage buffer": [1],
        "storage texture": [0],
        "sampler": [2],
    }
    assert sb.glsl_bindings("layout(location = 0) in vec4 v;\n", "v") == {}
    with pytest.raises(sb.BuildError, match="descriptor set"):
        sb.glsl_bindings("layout(set = 2, binding = 0) uniform sampler2D t;\n", "f")
    with pytest.raises(sb.BuildError, match="no binding"):
        sb.glsl_bindings("layout(std140) uniform U { vec4 v; } u;\n", "f")


def test_glsl_bindings_must_be_sdls_slots_from_zero() -> None:
    sb.check_glsl_bindings(FILTER_GLSL, sb.Resources(samplers=1, uniform_buffers=1), "f")
    # SDL's texture_advanced: texture 1 with two samplers is two sampler2Ds on slot 1.
    advanced = "".join(
        f"layout(binding = {b}) uniform sampler2D s{i};\n" for i, b in enumerate((0, 1, 1, 2))
    )
    sb.check_glsl_bindings(advanced, sb.Resources(samplers=3), "a")
    with pytest.raises(sb.BuildError, match=r"uniform buffer bindings \[0\], .* slots \[\]"):
        sb.check_glsl_bindings(FILTER_GLSL, sb.Resources(samplers=1), "f")
    shifted = FILTER_GLSL.replace(
        "binding = 0) uniform sampler2D", "binding = 1) uniform sampler2D"
    )
    with pytest.raises(sb.BuildError, match=r"sampler bindings \[1\], .* slots \[0\]"):
        sb.check_glsl_bindings(shifted, sb.Resources(samplers=1, uniform_buffers=1), "f")


def dksh(program_type: int, scratch: int = 0, programs: int = 1, code: int = 256) -> bytes:
    header = struct.pack("<6I", sb.DKSH_MAGIC, 24, 256, code, 24, programs)
    program = struct.pack("<6I", program_type, 0, 8, 0, 0, scratch)
    return (header + program).ljust(256, b"\0") + b"\0" * code


def test_a_dksh_is_checked_as_the_deko3d_backend_loads_it() -> None:
    assert sb.dksh_problems(dksh(0), "vert") == []
    assert sb.dksh_problems(dksh(1), "frag") == []
    assert sb.dksh_problems(dksh(0), "frag") == ["a program of type 0, not frag (1)"]
    assert sb.dksh_problems(dksh(1, scratch=65 * 1024), "frag") == [
        "66560 bytes of scratch per warp, the backend has 65536"
    ]
    assert sb.dksh_problems(dksh(1, programs=2), "frag") == ["2 programs, not 1"]
    assert sb.dksh_problems(dksh(1)[:300], "frag") == ["control 256 + code 256 > 300 bytes"]
    assert sb.dksh_problems(b"DXBC" + dksh(1)[4:], "frag") == ["magic 0x43425844, not DKSH"]
    assert "shorter than a DKSH header" in sb.dksh_problems(b"DKSH", "vert")[0]


def test_the_committed_library_has_a_dksh_for_every_shader() -> None:
    text = (REPO / sb.HEADER).read_text(encoding="utf-8")
    for src in sb.sources(REPO / sb.SOURCE_DIR):
        assert f"k_{src.ident}_dksh, sizeof k_{src.ident}_dksh," in text
    assert "//   uam 1.1.0 (devkitpro/devkita64, ADR 0138)" in text


SDL_TABLE = """
static const GPU_ShaderSources vert_shader_sources[NUM_VERT_SHADERS] = {
    [VERT_SHADER_TRI_TEXTURE] = {
        .num_samplers = 0,
        .num_uniform_buffers = 1,
        SHADER_PRIVATE(tri_texture_vert_private)
        SHADER_SPIRV(tri_texture_vert_spv)
    },
};
static const GPU_ShaderSources frag_shader_sources[NUM_FRAG_SHADERS] = {
    [FRAG_SHADER_TEXTURE_ADVANCED] = {
        .num_samplers = 3,
        .num_uniform_buffers = 1,
        SHADER_PRIVATE(texture_advanced_frag_private)
    },
};
"""


def test_sdls_renderer_shaders_and_counts_come_from_its_table() -> None:
    assert sb.sdl_shader_counts(SDL_TABLE) == {
        "tri_texture_vert": sb.Resources(uniform_buffers=1),
        "texture_advanced_frag": sb.Resources(samplers=3, uniform_buffers=1),
    }
    with pytest.raises(sb.BuildError, match="no SHADER_PRIVATE"):
        sb.sdl_shader_counts("static int x;")


def add_member(tar: tarfile.TarFile, name: str, data: bytes) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    tar.addfile(info, io.BytesIO(data))


def test_sdls_shaders_are_read_from_the_archive_by_name_only(tmp_path) -> None:
    archive = tmp_path / "sdl3-nx.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        add_member(tar, "sdl3-nx-1234/src/render/gpu/shaders/color.frag.hlsl", b"frag")
        add_member(tar, "sdl3-nx-1234/src/render/gpu/shaders/common.frag.hlsli", b"common")
        add_member(tar, "sdl3-nx-1234/src/render/gpu/shaders/build-shaders.sh", b"#!/bin/sh")
        add_member(tar, "sdl3-nx-1234/src/render/gpu/SDL_shaders_gpu.c", SDL_TABLE.encode())
        add_member(tar, "sdl3-nx-1234/../../escape.hlsl", b"no")
        add_member(tar, "sdl3-nx-1234/src/render/gpu/shaders/../../../../x.hlsl", b"no")
    shader_dir, table = sb.extract_sdl_shaders(archive, tmp_path / "out")
    assert sorted(p.name for p in shader_dir.iterdir()) == ["color.frag.hlsl", "common.frag.hlsli"]
    assert table == SDL_TABLE
    assert sorted(p.name for p in tmp_path.iterdir()) == ["out", "sdl3-nx.tar.gz"]
    bare = tmp_path / "bare.tar.gz"
    with tarfile.open(bare, "w:gz") as tar:
        add_member(tar, "sdl3-nx-1234/README.md", b"")
    with pytest.raises(sb.BuildError, match="no src/render/gpu/SDL_shaders_gpu.c"):
        sb.extract_sdl_shaders(bare, tmp_path / "bare")


def test_the_sdl_header_is_stamped_with_the_pinned_archive(tmp_path) -> None:
    pin = "a" * 64
    deps = tmp_path / "Dependencies.cmake"
    deps.write_text(f"set(MAPLETHIEF_SDL3_SWITCH_SHA256 {pin})\n", encoding="utf-8")
    assert sb.pinned_sdl_sha256(deps) == pin
    header = tmp_path / "render.h"
    shader = sb.SdlCompiled("color_frag", "color.frag.hlsl", "b" * 64, b"DKSH")
    text = sb.render_sdl_header(pin, [shader], ["uam 1.1.0"])
    assert text.startswith("// clang-format off\n") and text.endswith("// clang-format on\n")
    assert "static const unsigned char color_frag_private[4] = {" in text
    header.write_text(text, encoding="utf-8")
    assert sb.check_sdl(header, deps) == []
    deps.write_text(f"set(MAPLETHIEF_SDL3_SWITCH_SHA256 {'c' * 64})\n", encoding="utf-8")
    assert sb.check_sdl(header, deps) == [
        f"{header}: built from sdl3-nx archive aaaaaaaaaaaa, {deps} pins cccccccccccc "
        "(run scripts/build-shaders)"
    ]
    header.write_text("// clang-format off\n", encoding="utf-8")
    assert "no archive stamp" in sb.check_sdl(header, deps)[0]
    assert "cannot read" in sb.check_sdl(tmp_path / "missing.h", deps)[0]
    deps.write_text("", encoding="utf-8")
    with pytest.raises(sb.BuildError, match="no MAPLETHIEF_SDL3_SWITCH_SHA256"):
        sb.pinned_sdl_sha256(deps)


def pinned_archive() -> Path | None:
    """The configure step's download of the pinned sdl3-nx archive, if this machine has it."""
    root = Path(os.environ.get("MAPLETHIEF_DOWNLOAD_DIR", REPO / "build" / "_downloads"))
    archive = root / "sdl3-nx" / "sdl3-nx-07ea26a3.tar.gz"
    return archive if archive.is_file() else None


@pytest.mark.skipif(pinned_archive() is None, reason="no sdl3-nx archive (a Switch configure)")
@pytest.mark.skipif(shutil.which("cmake") is None, reason="no cmake")
def test_the_sdl3_nx_patches_apply_to_the_pinned_source_once(tmp_path) -> None:
    """The configure step's patches (#238, #242) on the real pinned source: both apply, the
    renderer gets the DKSH header, a second run changes nothing."""
    archive = pinned_archive()
    assert archive is not None
    with tarfile.open(archive) as tar:
        tar.extractall(tmp_path, filter="data")
    (source,) = tmp_path.iterdir()
    patches = [
        REPO / "cmake/patches/sdl3-nx-window-size.cmake",
        REPO / "cmake/patches/sdl3-nx-gpu-renderer.cmake",
    ]
    files = [
        "src/video/switch/SDL_switchvideo.c",
        "src/render/gpu/SDL_shaders_gpu.c",
        "src/gpu/deko3d/SDL_gpu_deko3d.c",
        "src/render/gpu/SDL_render_private_shaders.h",
    ]
    for patch in patches:
        subprocess.run(["cmake", "-P", str(patch)], cwd=source, check=True, capture_output=True)
    first = {f: (source / f).read_bytes() for f in files}
    assert first[files[3]] == (REPO / sb.SDL_HEADER).read_bytes()
    assert b"defined(SDL_GPU_DEKO3D) /* MapleThief #242" in first[files[1]]
    assert first[files[2]].count(b"MapleThief #242") == 2
    for patch in patches:
        subprocess.run(["cmake", "-P", str(patch)], cwd=source, check=True, capture_output=True)
    assert {f: (source / f).read_bytes() for f in files} == first
