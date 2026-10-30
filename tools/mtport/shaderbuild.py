"""The GPU shaders' build path (#120, ADR 0060): HLSL in, a committed C header out.

mt_gfx's GPU backend and the frontend's present stage run on SDL3 GPU. SDL3 GPU takes one of
three shader formats: SPIR-V (Vulkan), DXIL (D3D12) and MSL source (Metal). Each shader is
written once, in HLSL, under ``runtime/src/gfx/gpu/shaders/`` and named ``<name>.<stage>.hlsl``
(stage ``vert`` or ``frag``). ``scripts/build-shaders`` runs ``compile`` in a pinned container
(``tools/containers/shaders/Dockerfile``), which does this for every source:

- HLSL -> DXIL with DXC (shader model 6.0, signed with libdxil);
- HLSL -> SPIR-V with DXC (Vulkan 1.0), then ``spirv-val``;
- SPIR-V -> MSL source with SPIRV-Cross (entry point ``main0``);
- a binding check from SPIRV-Cross's reflection: a fragment shader's samplers are combined
  image samplers in descriptor set 2 and its uniform buffers sit in set 3, as SDL wants them.

The result is ``runtime/src/gfx/gpu/shaders_compiled.h``: every shader's three blobs as byte
arrays, the resource counts SDL_CreateGPUShader needs, and the SHA-256 of the HLSL it came from.
That header is committed, so a build needs no shader tools (CI has none). ``check`` (CTest
``shaders.stamps``, pytest ``tests/test_shaderbuild.py``) fails when a source's hash differs from
the header's, so that an edited shader without a rebuilt header can't merge.

This module is stdlib-only. The container runs it as a plain script (``python3 -I -B``), so it
imports nothing from ``mtport``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

SOURCE_DIR = Path("runtime/src/gfx/gpu/shaders")
HEADER = Path("runtime/src/gfx/gpu/shaders_compiled.h")
STAGES = {
    "vert": ("vs_6_0", "MT_GFX_GPU_STAGE_VERTEX"),
    "frag": ("ps_6_0", "MT_GFX_GPU_STAGE_FRAGMENT"),
}
NAME_RE = re.compile(r"^([a-z][a-z0-9_]*)\.(vert|frag)\.hlsl$")
STAMP_RE = re.compile(r"^// source ([a-z0-9_]+\.(?:vert|frag)\.hlsl) sha256 ([0-9a-f]{64})$", re.M)


class BuildError(Exception):
    pass


@dataclass(frozen=True)
class Source:
    path: Path
    name: str  # "gfx"
    stage: str  # "vert" | "frag"

    @property
    def ident(self) -> str:  # "gfx_vert"
        return f"{self.name}_{self.stage}"

    @property
    def file(self) -> str:  # "gfx.vert.hlsl"
        return self.path.name


def sources(source_dir: Path) -> list[Source]:
    """The shader sources, sorted by file name. A .hlsl file with a bad name is an error."""
    found: list[Source] = []
    for path in sorted(source_dir.glob("*.hlsl")):
        m = NAME_RE.match(path.name)
        if m is None:
            raise BuildError(f"{path}: not named <name>.vert.hlsl or <name>.frag.hlsl")
        found.append(Source(path, m.group(1), m.group(2)))
    if not found:
        raise BuildError(f"{source_dir}: no shaders")
    return found


def sha256(path: Path) -> str:
    """A source's SHA-256 with CR LF read as LF, so a Windows checkout (core.autocrlf) has the
    same stamps."""
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def stamps(header_text: str) -> dict[str, str]:
    """The header's source stamps: file name -> SHA-256."""
    return {m.group(1): m.group(2) for m in STAMP_RE.finditer(header_text)}


def check(source_dir: Path, header: Path) -> list[str]:
    """The problems between the sources and the committed header (empty: it is up to date)."""
    try:
        text = header.read_text(encoding="utf-8")
    except OSError as e:
        return [f"{header}: cannot read: {e}"]
    have = stamps(text)
    try:
        want = {s.file: sha256(s.path) for s in sources(source_dir)}
    except BuildError as e:
        return [str(e)]
    problems = []
    for file, digest in sorted(want.items()):
        if file not in have:
            problems.append(f"{file}: not in {header} (run scripts/build-shaders)")
        elif have[file] != digest:
            problems.append(f"{file}: changed since {header} was built (run scripts/build-shaders)")
    for file in sorted(set(have) - set(want)):
        problems.append(f"{file}: in {header} but no longer a source (run scripts/build-shaders)")
    return problems


# --- compiling (in the container) --------------------------------------------------------------


@dataclass(frozen=True)
class Resources:
    """What SDL_GPUShaderCreateInfo needs besides the code."""

    samplers: int = 0
    storage_textures: int = 0
    storage_buffers: int = 0
    uniform_buffers: int = 0


def resources_from_reflection(reflection: dict, stage: str, file: str) -> Resources:
    """Counts the resources from SPIRV-Cross's JSON reflection and checks their descriptor sets
    against SDL3 GPU's layout (SDL_CreateGPUShader): vertex shaders use sets 0 (textures,
    storage) and 1 (uniforms), fragment shaders sets 2 and 3."""
    resource_set, uniform_set = (0, 1) if stage == "vert" else (2, 3)
    samplers = reflection.get("textures", [])
    separate = reflection.get("separate_images", []) + reflection.get("separate_samplers", [])
    if separate:
        raise BuildError(
            f"{file}: separate images or samplers ({[r.get('name') for r in separate]}): "
            "mark them [[vk::combinedImageSampler]] with the same binding"
        )
    storage_textures = reflection.get("images", [])
    storage_buffers = reflection.get("ssbos", [])
    uniforms = reflection.get("ubos", [])
    for kind, items, want_set in (
        ("sampler", samplers, resource_set),
        ("storage texture", storage_textures, resource_set),
        ("storage buffer", storage_buffers, resource_set),
        ("uniform buffer", uniforms, uniform_set),
    ):
        for item in items:
            if item.get("set") != want_set:
                raise BuildError(
                    f"{file}: {kind} {item.get('name')!r} is in descriptor set {item.get('set')}, "
                    f"SDL wants set {want_set} (register spaces: vertex 0/1, fragment 2/3)"
                )
    return Resources(len(samplers), len(storage_textures), len(storage_buffers), len(uniforms))


def run(cmd: Sequence[str]) -> None:
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise BuildError(f"{' '.join(cmd)} failed:\n{result.stdout}{result.stderr}")


def tool_versions() -> list[str]:
    try:
        return Path("/etc/maplethief-shader-tools").read_text(encoding="utf-8").splitlines()
    except OSError:
        return ["(unknown tools: not run in tools/containers/shaders)"]


@dataclass(frozen=True)
class Compiled:
    source: Source
    digest: str
    spirv: bytes
    dxil: bytes
    msl: bytes
    resources: Resources


def compile_one(src: Source, work: Path) -> Compiled:
    profile = STAGES[src.stage][0]
    spv = work / f"{src.ident}.spv"
    dxil = work / f"{src.ident}.dxil"
    msl = work / f"{src.ident}.msl"
    run(
        [
            "dxc",
            "-T",
            profile,
            "-E",
            "main",
            "-O3",
            "-Qstrip_debug",
            "-Fo",
            str(dxil),
            str(src.path),
        ]
    )
    run(
        [
            "dxc",
            "-spirv",
            "-fspv-target-env=vulkan1.0",
            "-T",
            profile,
            "-E",
            "main",
            "-O3",
            "-Fo",
            str(spv),
            str(src.path),
        ]
    )
    run(["spirv-val", "--target-env", "vulkan1.0", str(spv)])
    run(["spirv-cross", str(spv), "--msl", "--msl-version", "20000", "--output", str(msl)])
    reflected = subprocess.run(
        ["spirv-cross", str(spv), "--reflect"], capture_output=True, text=True, check=False
    )
    if reflected.returncode != 0:
        raise BuildError(f"spirv-cross --reflect {spv} failed:\n{reflected.stderr}")
    resources = resources_from_reflection(json.loads(reflected.stdout), src.stage, src.file)
    return Compiled(
        src, sha256(src.path), spv.read_bytes(), dxil.read_bytes(), msl.read_bytes(), resources
    )


def c_bytes(name: str, data: bytes) -> str:
    lines = [f"static const unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i : i + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def render_header(compiled: Sequence[Compiled], tools: Sequence[str]) -> str:
    out = [
        "// clang-format off",
        "// Generated by scripts/build-shaders (tools/mtport/shaderbuild.py, #120, ADR 0060)",
        f"// from {SOURCE_DIR.as_posix()}/*.hlsl. Do not edit: change a source and run the",
        "// script again.",
        "// Tools:",
    ]
    out += [f"//   {t}" for t in tools]
    for c in compiled:
        out.append(f"// source {c.source.file} sha256 {c.digest}")
    out += [
        "#ifndef MAPLETHIEF_GFX_GPU_SHADERS_COMPILED_H",
        "#define MAPLETHIEF_GFX_GPU_SHADERS_COMPILED_H",
        "",
        '#include "gfx/gpu/shader_library.h"',
        "",
        "namespace mt_gfx_gpu::compiled {",
        "",
    ]
    for c in compiled:
        out.append(c_bytes(f"k_{c.source.ident}_spirv", c.spirv))
        out.append(c_bytes(f"k_{c.source.ident}_dxil", c.dxil))
        out.append(c_bytes(f"k_{c.source.ident}_msl", c.msl))
        out.append("")
    out.append("inline constexpr ShaderBlob k_shaders[] = {")
    for c in compiled:
        r = c.resources
        out.append(
            f'    {{"{c.source.name}.{c.source.stage}", {STAGES[c.source.stage][1]}, '
            f"k_{c.source.ident}_spirv, sizeof k_{c.source.ident}_spirv, "
            f"k_{c.source.ident}_dxil, sizeof k_{c.source.ident}_dxil, "
            f"k_{c.source.ident}_msl, sizeof k_{c.source.ident}_msl, "
            f"{r.samplers}, {r.storage_textures}, {r.storage_buffers}, {r.uniform_buffers}}},"
        )
    out += ["};", "", "} // namespace mt_gfx_gpu::compiled", "", "#endif", "// clang-format on", ""]
    return "\n".join(out)


def compile_all(source_dir: Path, header: Path) -> int:
    srcs = sources(source_dir)
    with tempfile.TemporaryDirectory() as tmp:
        compiled = []
        for src in srcs:
            c = compile_one(src, Path(tmp))
            print(
                f"{src.file}: SPIR-V {len(c.spirv)} B, DXIL {len(c.dxil)} B, MSL {len(c.msl)} B, "
                f"{c.resources.samplers} samplers, {c.resources.uniform_buffers} uniform buffers"
            )
            compiled.append(c)
    header.write_text(render_header(compiled, tool_versions()), encoding="utf-8")
    print(f"wrote {header}")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="shaderbuild", description=__doc__.split("\n\n")[0])
    parser.add_argument("command", choices=("compile", "check"))
    parser.add_argument("--repo", type=Path, default=Path("."))
    args = parser.parse_args(argv)
    source_dir, header = args.repo / SOURCE_DIR, args.repo / HEADER
    try:
        if args.command == "compile":
            return compile_all(source_dir, header)
        problems = check(source_dir, header)
    except BuildError as e:
        print(f"shaderbuild: {e}", file=sys.stderr)
        return 1
    for p in problems:
        print(f"shaderbuild: {p}", file=sys.stderr)
    if not problems:
        print(f"shaderbuild: {header} is up to date with {source_dir}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
