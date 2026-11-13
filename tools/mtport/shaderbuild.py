"""The GPU shaders' build path (#120, ADR 0060; DKSH #242, ADR 0143): HLSL in, committed C headers
out.

mt_gfx's GPU backend and the frontend's present stage run on SDL3 GPU. SDL3 GPU takes SPIR-V
(Vulkan), DXIL (D3D12), MSL source (Metal) and, on the Nintendo Switch, DKSH (deko3d, as
``SDL_GPU_SHADERFORMAT_PRIVATE``). Each shader is written once, in HLSL, under
``runtime/src/gfx/gpu/shaders/`` and named ``<name>.<stage>.hlsl`` (stage ``vert`` or ``frag``).
``scripts/build-shaders`` runs ``compile`` in a pinned container
(``tools/containers/shaders/Dockerfile``), which does this for every source:

- HLSL -> DXIL with DXC (shader model 6.0, signed with libdxil);
- HLSL -> SPIR-V with DXC (Vulkan 1.0), then ``spirv-val``;
- SPIR-V -> MSL source with SPIRV-Cross (entry point ``main0``);
- SPIR-V -> GLSL 4.60 with SPIRV-Cross, then GLSL -> DKSH with UAM (deko3d's compiler);
- a binding check from SPIRV-Cross's reflection: a fragment shader's samplers are combined
  image samplers in descriptor set 2 and its uniform buffers sit in set 3, as SDL wants them;
  and in the GLSL, each resource kind is bound from slot 0 (``layout(binding = slot)``, no set),
  as sdl3-nx's deko3d backend binds them.

The result is ``runtime/src/gfx/gpu/shaders_compiled.h``: every shader's four blobs as byte
arrays, the resource counts SDL_CreateGPUShader needs, and the SHA-256 of the HLSL it came from.

SDL's own GPU renderer (``SDL_CreateGPURenderer``) needs its shaders as DKSH too, which the
pinned sdl3-nx (ADR 0138) does not carry. ``compile --sdl-archive`` builds them from that
archive's HLSL (``src/render/gpu/shaders/``) through the same chain into
``cmake/patches/sdl3-nx-render-shaders.h``, stamped with the archive's SHA-256;
``cmake/patches/sdl3-nx-gpu-renderer.cmake`` puts it into SDL's source at configure time.

Both headers are committed, so a build needs no shader tools (CI has none). ``check`` (CTest
``shaders.stamps``, pytest ``tests/test_shaderbuild.py``) fails when a source's hash differs from
the header's, or the SDL header's archive is not the one ``cmake/Dependencies.cmake`` pins, so
that an edited shader or a new SDL pin without rebuilt headers can't merge.

This module is stdlib-only. The container runs it as a plain script (``python3 -I -B``), so it
imports nothing from ``mtport``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import tarfile
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

SOURCE_DIR = Path("runtime/src/gfx/gpu/shaders")
HEADER = Path("runtime/src/gfx/gpu/shaders_compiled.h")
SDL_HEADER = Path("cmake/patches/sdl3-nx-render-shaders.h")
DEPENDENCIES = Path("cmake/Dependencies.cmake")
STAGES = {
    "vert": ("vs_6_0", "MT_GFX_GPU_STAGE_VERTEX"),
    "frag": ("ps_6_0", "MT_GFX_GPU_STAGE_FRAGMENT"),
}
NAME_RE = re.compile(r"^([a-z][a-z0-9_]*)\.(vert|frag)\.hlsl$")
STAMP_RE = re.compile(r"^// source ([a-z0-9_]+\.(?:vert|frag)\.hlsl) sha256 ([0-9a-f]{64})$", re.M)
ARCHIVE_STAMP_RE = re.compile(r"^// archive sdl3-nx sha256 ([0-9a-f]{64})$", re.M)
PINNED_SDL_RE = re.compile(r"^set\(MAPLETHIEF_SDL3_SWITCH_SHA256 ([0-9a-f]{64})\)$", re.M)
# The pinned SDL's render shaders and the file that lists them (its counts are checked).
SDL_SHADER_DIR = PurePosixPath("src/render/gpu/shaders")
SDL_SHADER_LIST = PurePosixPath("src/render/gpu/SDL_shaders_gpu.c")


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


def sha256_bytes(data: bytes) -> str:
    """The SHA-256 of a text's bytes with CR LF read as LF, so a Windows checkout
    (core.autocrlf) has the same stamps."""
    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()


def sha256(path: Path) -> str:
    """A source's SHA-256 (CR LF read as LF)."""
    return sha256_bytes(path.read_bytes())


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


def pinned_sdl_sha256(dependencies: Path) -> str:
    """The SHA-256 of the Switch's SDL3 archive that cmake/Dependencies.cmake pins."""
    m = PINNED_SDL_RE.search(dependencies.read_text(encoding="utf-8"))
    if m is None:
        raise BuildError(f"{dependencies}: no MAPLETHIEF_SDL3_SWITCH_SHA256")
    return m.group(1)


def check_sdl(header: Path, dependencies: Path) -> list[str]:
    """The problems between the SDL renderer's DKSH header and the pinned sdl3-nx archive."""
    try:
        text = header.read_text(encoding="utf-8")
        want = pinned_sdl_sha256(dependencies)
    except OSError as e:
        return [f"cannot read: {e}"]
    except BuildError as e:
        return [str(e)]
    m = ARCHIVE_STAMP_RE.search(text)
    if m is None:
        return [f"{header}: no archive stamp (run scripts/build-shaders)"]
    if m.group(1) != want:
        return [
            f"{header}: built from sdl3-nx archive {m.group(1)[:12]}, {dependencies} pins "
            f"{want[:12]} (run scripts/build-shaders)"
        ]
    return []


# --- resources and bindings --------------------------------------------------------------------


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


# A GLSL resource declaration as SPIRV-Cross writes it: the layout, then the storage, then the
# type (sampler2D, image2D) or a uniform or storage block's name.
_GLSL_MEMORY = r"(?:(?:readonly|writeonly|restrict|coherent|volatile)\s+)*"
GLSL_RESOURCE_RE = re.compile(
    rf"^layout\(([^)]*)\)\s+{_GLSL_MEMORY}(uniform|buffer)\s+{_GLSL_MEMORY}(\w+)", re.M
)
GLSL_BINDING_RE = re.compile(r"\bbinding\s*=\s*(\d+)")


def glsl_bindings(glsl: str, file: str) -> dict[str, list[int]]:
    """The bindings of a GLSL shader's resources, by kind ("sampler", "storage texture",
    "storage buffer", "uniform buffer"), as the deko3d backend binds them: per kind, each
    binding is SDL's slot. A resource without a binding, or with a descriptor set (Vulkan GLSL,
    which UAM doesn't take), is an error."""
    found: dict[str, list[int]] = {}
    for m in GLSL_RESOURCE_RE.finditer(glsl):
        layout, storage, word = m.groups()
        if re.search(r"\bset\s*=", layout):
            raise BuildError(f"{file}: GLSL {word} has a descriptor set ({layout}): not for UAM")
        binding = GLSL_BINDING_RE.search(layout)
        if binding is None:
            raise BuildError(f"{file}: GLSL {word} has no binding ({layout})")
        if storage == "buffer":
            kind = "storage buffer"
        elif word.startswith(("sampler", "isampler", "usampler")):
            kind = "sampler"
        elif word.startswith(("image", "iimage", "uimage")):
            kind = "storage texture"
        else:
            kind = "uniform buffer"
        found.setdefault(kind, []).append(int(binding.group(1)))
    return {k: sorted(v) for k, v in found.items()}


def check_glsl_bindings(glsl: str, resources: Resources, file: str) -> None:
    """Each resource kind's bindings in the GLSL are SDL's slots 0..n-1, n being the count SDL is
    told (SDL_GPUShaderCreateInfo): what sdl3-nx's deko3d backend binds (ADR 0143). Two samplers
    may share a slot (SPIRV-Cross combines one texture with two samplers that way: the texture's
    slot, as SDL binds a texture and its sampler together)."""
    have = glsl_bindings(glsl, file)
    for kind, count in (
        ("sampler", resources.samplers),
        ("storage texture", resources.storage_textures),
        ("storage buffer", resources.storage_buffers),
        ("uniform buffer", resources.uniform_buffers),
    ):
        slots = sorted(set(have.get(kind, [])))
        if slots != list(range(count)):
            raise BuildError(
                f"{file}: GLSL {kind} bindings {slots}, the deko3d backend binds slots "
                f"{list(range(count))} (each resource kind from slot 0)"
            )


# --- DKSH --------------------------------------------------------------------------------------

DKSH_MAGIC = 0x48534B44  # "DKSH"
DKSH_PROGRAM_TYPES = {"vert": 0, "frag": 1}  # deko3d's DkshProgramType (source/dksh.h)
# sdl3-nx's backend gives each warp this much scratch memory (DKO_PER_WARP_SCRATCH); a program
# that needs more traps its draws.
DKSH_MAX_SCRATCH = 64 * 1024


def dksh_problems(data: bytes, stage: str) -> list[str]:
    """What is wrong with a DKSH as sdl3-nx's backend loads it (DKO_LoadShader): the header
    (magic, header size, control size, code size, programs offset, program count), its sizes
    within the file, and one program of the expected stage that fits the backend's per-warp
    scratch memory."""
    if len(data) < 24:
        return [f"{len(data)} bytes: shorter than a DKSH header"]
    magic, header_size, control_size, code_size, programs_offset, count = struct.unpack_from(
        "<6I", data
    )
    problems = []
    if magic != DKSH_MAGIC:
        problems.append(f"magic {magic:#010x}, not DKSH")
    if control_size + code_size > len(data) or header_size > control_size:
        problems.append(f"control {control_size} + code {code_size} > {len(data)} bytes")
    if count != 1:
        problems.append(f"{count} programs, not 1")
    elif programs_offset + 24 > control_size:
        problems.append(f"programs at {programs_offset}, past the control section")
    else:
        kind, _, _, _, _, scratch = struct.unpack_from("<6I", data, programs_offset)
        if kind != DKSH_PROGRAM_TYPES[stage]:
            problems.append(f"a program of type {kind}, not {stage} ({DKSH_PROGRAM_TYPES[stage]})")
        if scratch > DKSH_MAX_SCRATCH:
            problems.append(
                f"{scratch} bytes of scratch per warp, the backend has {DKSH_MAX_SCRATCH}"
            )
    return problems


# --- compiling (in the container) --------------------------------------------------------------


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
    dksh: bytes = b""


def to_spirv(path: Path, stage: str, out: Path) -> None:
    run(
        [
            "dxc",
            "-spirv",
            "-fspv-target-env=vulkan1.0",
            "-T",
            STAGES[stage][0],
            "-E",
            "main",
            "-O3",
            "-Fo",
            str(out),
            str(path),
        ]
    )
    run(["spirv-val", "--target-env", "vulkan1.0", str(out)])


def reflect(spv: Path) -> dict:
    reflected = subprocess.run(
        ["spirv-cross", str(spv), "--reflect"], capture_output=True, text=True, check=False
    )
    if reflected.returncode != 0:
        raise BuildError(f"spirv-cross --reflect {spv} failed:\n{reflected.stderr}")
    return json.loads(reflected.stdout)


def to_dksh(spv: Path, stage: str, resources: Resources, file: str, work: Path) -> bytes:
    """SPIR-V -> GLSL 4.60 (SPIRV-Cross; desktop GLSL without Vulkan semantics, so bindings
    without sets, and separate textures and samplers combined on the texture's binding) -> DKSH
    (UAM). The GLSL's bindings are checked against SDL's slots, the DKSH's header against what
    the backend loads."""
    glsl = work / f"{spv.stem}.glsl"
    dksh = work / f"{spv.stem}.dksh"
    run(
        [
            "spirv-cross",
            str(spv),
            "--version",
            "460",
            "--no-es",
            "--combined-samplers-inherit-bindings",
            "--output",
            str(glsl),
        ]
    )
    check_glsl_bindings(glsl.read_text(encoding="utf-8"), resources, file)
    run(["uam", "--stage", stage, "--out", str(dksh), str(glsl)])
    data = dksh.read_bytes()
    problems = dksh_problems(data, stage)
    if problems:
        raise BuildError(f"{file}: UAM's DKSH: {'; '.join(problems)}")
    return data


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
    to_spirv(src.path, src.stage, spv)
    run(["spirv-cross", str(spv), "--msl", "--msl-version", "20000", "--output", str(msl)])
    resources = resources_from_reflection(reflect(spv), src.stage, src.file)
    dksh = to_dksh(spv, src.stage, resources, src.file, work)
    return Compiled(
        src,
        sha256(src.path),
        spv.read_bytes(),
        dxil.read_bytes(),
        msl.read_bytes(),
        resources,
        dksh,
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
        out.append(c_bytes(f"k_{c.source.ident}_dksh", c.dksh))
        out.append("")
    out.append("inline constexpr ShaderBlob k_shaders[] = {")
    for c in compiled:
        r = c.resources
        i = c.source.ident
        out.append(
            f'    {{"{c.source.name}.{c.source.stage}", {STAGES[c.source.stage][1]}, '
            f"k_{i}_spirv, sizeof k_{i}_spirv, "
            f"k_{i}_dxil, sizeof k_{i}_dxil, "
            f"k_{i}_msl, sizeof k_{i}_msl, "
            f"k_{i}_dksh, sizeof k_{i}_dksh, "
            f"{r.samplers}, {r.storage_textures}, {r.storage_buffers}, {r.uniform_buffers}}},"
        )
    out += ["};", "", "} // namespace mt_gfx_gpu::compiled", "", "#endif", "// clang-format on", ""]
    return "\n".join(out)


# --- SDL's renderer shaders (from the pinned sdl3-nx archive) ----------------------------------

# One shader in SDL_shaders_gpu.c's tables: its counts, then SHADER_PRIVATE(<ident>_private).
SDL_TABLE_RE = re.compile(
    r"\.num_samplers\s*=\s*(\d+),\s*\.num_uniform_buffers\s*=\s*(\d+),\s*"
    r"SHADER_PRIVATE\((\w+)_private\)"
)


def sdl_shader_counts(shader_list: str) -> dict[str, Resources]:
    """SDL_shaders_gpu.c's shaders (ident "tri_texture_vert") and the counts SDL creates them
    with."""
    found = {
        m.group(3): Resources(samplers=int(m.group(1)), uniform_buffers=int(m.group(2)))
        for m in SDL_TABLE_RE.finditer(shader_list)
    }
    if not found:
        raise BuildError(f"{SDL_SHADER_LIST}: no SHADER_PRIVATE entries")
    return found


def extract_sdl_shaders(archive: Path, into: Path) -> tuple[Path, str]:
    """Extracts SDL's render shaders (src/render/gpu/shaders/*.hlsl, *.hlsli) and
    SDL_shaders_gpu.c from the sdl3-nx archive into `into`. Members are only read, by name,
    never extracted with their own paths. Returns the shader directory and SDL_shaders_gpu.c's
    text."""
    shader_dir = into / "shaders"
    shader_dir.mkdir(parents=True, exist_ok=True)
    shader_list = None
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            if not member.isfile():
                continue
            parts = PurePosixPath(member.name).parts
            if len(parts) < 2:
                continue
            inner = PurePosixPath(*parts[1:])  # without the archive's top folder
            data = tar.extractfile(member)
            if data is None:
                continue
            if inner.parent == SDL_SHADER_DIR and inner.suffix in (".hlsl", ".hlsli"):
                (shader_dir / inner.name).write_bytes(data.read())
            elif inner == SDL_SHADER_LIST:
                shader_list = data.read().decode("utf-8")
    if shader_list is None:
        raise BuildError(f"{archive}: no {SDL_SHADER_LIST}")
    return shader_dir, shader_list


@dataclass(frozen=True)
class SdlCompiled:
    ident: str  # "tri_texture_vert", SDL's name without "_private"
    file: str
    digest: str
    dksh: bytes


def compile_sdl(archive: Path, dependencies: Path, work: Path) -> tuple[str, list[SdlCompiled]]:
    """SDL's render shaders from the pinned sdl3-nx archive, as DKSH. The archive must be the
    one cmake/Dependencies.cmake pins; every shader SDL_shaders_gpu.c lists is built, with the
    bindings checked against the counts SDL gives SDL_CreateGPUShader."""
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    want = pinned_sdl_sha256(dependencies)
    if digest != want:
        raise BuildError(f"{archive}: sha256 {digest}, {dependencies} pins {want}")
    shader_dir, shader_list = extract_sdl_shaders(archive, work / "sdl")
    counts = sdl_shader_counts(shader_list)
    compiled = []
    for ident, resources in sorted(counts.items()):
        name, _, stage = ident.rpartition("_")
        if stage not in STAGES:
            raise BuildError(f"{SDL_SHADER_LIST}: {ident}_private is not a vert or frag shader")
        path = shader_dir / f"{name}.{stage}.hlsl"
        if not path.is_file():
            raise BuildError(f"{archive}: no {SDL_SHADER_DIR / path.name}")
        spv = work / f"sdl_{ident}.spv"
        to_spirv(path, stage, spv)
        dksh = to_dksh(spv, stage, resources, path.name, work)
        compiled.append(SdlCompiled(ident, path.name, sha256(path), dksh))
    return digest, compiled


def render_sdl_header(
    archive_sha256: str, compiled: Sequence[SdlCompiled], tools: Sequence[str]
) -> str:
    out = [
        "// clang-format off",
        "// Generated by scripts/build-shaders (tools/mtport/shaderbuild.py, #242, ADR 0143)",
        "// from the HLSL of SDL's GPU renderer (src/render/gpu/shaders/, zlib licence,",
        "// (C) Sam Lantinga) in the sdl3-nx archive that cmake/Dependencies.cmake pins,",
        "// compiled to DKSH for SDL_GPU on deko3d. cmake/patches/sdl3-nx-gpu-renderer.cmake",
        "// copies it into SDL's source as src/render/gpu/SDL_render_private_shaders.h.",
        "// Do not edit: run the script again.",
        "// Tools:",
    ]
    out += [f"//   {t}" for t in tools]
    out.append(f"// archive sdl3-nx sha256 {archive_sha256}")
    for c in compiled:
        out.append(f"// sdl source {c.file} sha256 {c.digest}")
    out += [
        "#ifndef MAPLETHIEF_SDL3_NX_RENDER_SHADERS_H",
        "#define MAPLETHIEF_SDL3_NX_RENDER_SHADERS_H",
        "",
    ]
    for c in compiled:
        out.append(c_bytes(f"{c.ident}_private", c.dksh))
        out.append("")
    out += ["#endif", "// clang-format on", ""]
    return "\n".join(out)


def compile_all(source_dir: Path, header: Path, sdl: tuple[Path, Path, Path] | None = None) -> int:
    """Compiles the library into `header`; with `sdl` (archive, dependencies, SDL header) SDL's
    renderer shaders into the SDL header too."""
    srcs = sources(source_dir)
    with tempfile.TemporaryDirectory() as tmp:
        compiled = []
        for src in srcs:
            c = compile_one(src, Path(tmp))
            print(
                f"{src.file}: SPIR-V {len(c.spirv)} B, DXIL {len(c.dxil)} B, MSL {len(c.msl)} B, "
                f"DKSH {len(c.dksh)} B, {c.resources.samplers} samplers, "
                f"{c.resources.uniform_buffers} uniform buffers"
            )
            compiled.append(c)
        sdl_compiled = None
        if sdl is not None:
            archive, dependencies, _ = sdl
            sdl_compiled = compile_sdl(archive, dependencies, Path(tmp))
            for s in sdl_compiled[1]:
                print(f"SDL {s.file}: DKSH {len(s.dksh)} B ({s.ident}_private)")
    header.write_text(render_header(compiled, tool_versions()), encoding="utf-8")
    print(f"wrote {header}")
    if sdl is not None and sdl_compiled is not None:
        sdl[2].write_text(render_sdl_header(*sdl_compiled, tool_versions()), encoding="utf-8")
        print(f"wrote {sdl[2]}")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="shaderbuild", description=__doc__.split("\n\n")[0])
    parser.add_argument("command", choices=("compile", "check"))
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument(
        "--sdl-archive",
        type=Path,
        help="compile: the pinned sdl3-nx archive, to build SDL's renderer shaders as DKSH too",
    )
    args = parser.parse_args(argv)
    source_dir, header = args.repo / SOURCE_DIR, args.repo / HEADER
    sdl_header, dependencies = args.repo / SDL_HEADER, args.repo / DEPENDENCIES
    try:
        if args.command == "compile":
            sdl = (args.sdl_archive, dependencies, sdl_header) if args.sdl_archive else None
            return compile_all(source_dir, header, sdl)
        problems = check(source_dir, header) + check_sdl(sdl_header, dependencies)
    except BuildError as e:
        print(f"shaderbuild: {e}", file=sys.stderr)
        return 1
    for p in problems:
        print(f"shaderbuild: {p}", file=sys.stderr)
    if not problems:
        print(f"shaderbuild: {header} and {sdl_header} are up to date")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
