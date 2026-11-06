"""The compendium's inputs bundle: what a site build needs from the game, and the fingerprint
of the code that made it (issue #248, ADR 0121 addendum).

The site step needs only the knowledge base (``build/compendium/kb/``) and the exported
images (``build/compendium/site/img/``); it reads nothing else derived from the game. Both
come from the builder's own IPA, which CI never sees (ADR 0100). ``scripts/compendium deploy``
therefore also packs them as ``compendium-inputs.tar.gz`` and uploads it, with its metadata
``compendium-inputs.json``, to the same draft release as the site. When a change to the
generator, the sections, the templates or the prose is pushed to ``main``, the deploy
workflow downloads the bundle, builds the site with the pushed code and deploys it.

That is only right while the pushed code would extract the same knowledge base and images.
:func:`fingerprint` hashes the code that shapes them: every module of ``tools/mtport`` that
the knowledge base's and the images' entry points (:data:`ROOTS`) import, directly or
through each other, found by reading the import statements (also those inside functions).
The bundle carries the fingerprint of the code that built it; a push whose code has another
fingerprint skips the deploy until the builder runs ``scripts/compendium deploy`` again.

- :func:`source_hashes` / :func:`fingerprint`: the files and their combined hash.
- :func:`pack` / :func:`unpack`: the bundle, deterministic like the site bundle; unpacking
  accepts plain files at the paths a bundle holds and nothing else (it is untrusted data).
- :func:`metadata`: the small JSON uploaded beside the bundle (its SHA-256, size and
  fingerprint), which the workflow checks the download against.
"""

from __future__ import annotations

import ast
import hashlib
import io
import json
import re
import tarfile
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from mtport.compendium import bundle

INPUTS_NAME = bundle.INPUTS_NAME
INPUTS_META = bundle.INPUTS_META
INFO = "inputs.json"  # inside the bundle: the fingerprint and the hashed files
SCHEMA = 1
# Bumped when the fingerprint is computed differently, so old bundles stop matching.
FINGERPRINT_VERSION = "compendium-inputs/1"

TOOLS = Path(__file__).resolve().parents[2]  # tools/, the root of the mtport package
# The entry points of the knowledge base (all its stages) and the images step.
ROOTS = ("mtport.compendium.kb", "mtport.compendium.images")

KB_PREFIX = "kb/"
IMG_PREFIX = "img/"
# The only members a bundle may hold: the info file, the KB's JSON and the images step's
# output (PNG and APNG, SVG overlays, the manifest).
_MEMBER = re.compile(r"(kb/[A-Za-z0-9_.-]+\.json|img/[A-Za-z0-9_./@-]+\.(png|svg|json))")
MAX_MEMBERS = 200_000


class InputsError(Exception):
    """A bundle that is malformed, unsafe or does not match its metadata."""


# --- the fingerprint ----------------------------------------------------------------------------


def _module_file(tools: Path, module: str) -> Path | None:
    base = tools.joinpath(*module.split("."))
    for p in (base.with_suffix(".py"), base / "__init__.py"):
        if p.is_file():
            return p
    return None


def _imports(path: Path, module: str) -> list[str]:
    """Every module ``path`` names in an import statement, anywhere in the file; a
    ``from a import b`` names both ``a`` and ``a.b`` (``b`` may be a module)."""
    package = module if path.name == "__init__.py" else module.rpartition(".")[0]
    out = []
    for node in ast.walk(ast.parse(path.read_bytes(), filename=str(path))):
        if isinstance(node, ast.Import):
            out += [a.name for a in node.names]
        elif isinstance(node, ast.ImportFrom):
            base = node.module or ""
            if node.level:
                parts = package.split(".")
                parts = parts[: len(parts) - (node.level - 1)] if node.level > 1 else parts
                base = ".".join(parts + ([base] if base else []))
            if base:
                out.append(base)
            out += [f"{base}.{a.name}" if base else a.name for a in node.names]
    return out


def source_files(tools: Path = TOOLS, roots: Sequence[str] = ROOTS) -> list[Path]:
    """The files of the ``mtport`` package that :data:`ROOTS` import, transitively, with the
    packages' ``__init__.py`` files; sorted."""
    seen: dict[str, Path] = {}
    todo = list(roots)
    while todo:
        mod = todo.pop()
        if mod in seen or not (mod == "mtport" or mod.startswith("mtport.")):
            continue
        path = _module_file(tools, mod)
        if path is None:
            continue
        seen[mod] = path
        parts = mod.split(".")
        todo += [".".join(parts[:i]) for i in range(1, len(parts))]  # parent packages
        todo += _imports(path, mod)
    return sorted(set(seen.values()))


def source_hashes(tools: Path = TOOLS, roots: Sequence[str] = ROOTS) -> dict[str, str]:
    """``{path relative to the repository: SHA-256}`` of :func:`source_files`. Line endings
    are normalised to LF first, so a checkout with CRLF hashes the same."""
    repo = tools.parent
    out = {}
    for p in source_files(tools, roots):
        data = p.read_bytes().replace(b"\r\n", b"\n")
        out[p.relative_to(repo).as_posix()] = hashlib.sha256(data).hexdigest()
    return out


def combine(hashes: Mapping[str, str]) -> str:
    """The fingerprint of ``hashes``: one SHA-256 over the version and every (path, hash)."""
    text = FINGERPRINT_VERSION + "\n" + "".join(f"{h}  {p}\n" for p, h in sorted(hashes.items()))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def fingerprint(tools: Path = TOOLS) -> str:
    return combine(source_hashes(tools))


def changed_files(old: Mapping[str, str], new: Mapping[str, str]) -> list[str]:
    """One line per file that differs between two :func:`source_hashes` maps."""
    out = []
    for p in sorted(set(old) | set(new)):
        if p not in old:
            out.append(f"{p}: new")
        elif p not in new:
            out.append(f"{p}: no longer used")
        elif old[p] != new[p]:
            out.append(f"{p}: changed")
    return out


# --- the bundle ---------------------------------------------------------------------------------


def input_files(kb_dir: Path, img_dir: Path) -> list[tuple[str, Path]]:
    """``(bundle path, file)`` for the knowledge base's JSON and every file of ``img/``."""
    files = [(KB_PREFIX + p.name, p) for p in sorted(Path(kb_dir).glob("*.json")) if p.is_file()]
    img_dir = Path(img_dir)
    for p in sorted(img_dir.rglob("*")):
        rel = p.relative_to(img_dir)
        if any(part.startswith(".") for part in rel.parts):
            continue
        if p.is_symlink() or p.is_file():
            files.append((IMG_PREFIX + rel.as_posix(), p))
    return files


def files_problems(files: Sequence[tuple[str, Path]]) -> list[str]:
    """Why ``files`` cannot be packed, one line each; empty when they can."""
    problems = []
    names = {rel for rel, _ in files}
    if KB_PREFIX + "index.json" not in names:
        problems.append("kb/index.json: missing (run scripts/compendium build)")
    if IMG_PREFIX + "manifest.json" not in names:
        problems.append("img/manifest.json: missing (run scripts/compendium build)")
    for rel, path in files:
        if path.is_symlink():
            problems.append(f"{rel}: a symlink; the bundle holds plain files only")
        elif not _MEMBER.fullmatch(rel) or ".." in rel.split("/"):
            problems.append(f"{rel}: not a file the inputs bundle may hold")
    return problems


def info_bytes(hashes: Mapping[str, str]) -> bytes:
    info = {
        "schema": SCHEMA,
        "fingerprint": combine(hashes),
        "sources": dict(sorted(hashes.items())),
    }
    return (json.dumps(info, indent=1, sort_keys=True) + "\n").encode("utf-8")


def pack(files: Sequence[tuple[str, Path]], hashes: Mapping[str, str]) -> bytes:
    """The bundle: :data:`INFO` (the fingerprint of ``hashes``), then ``files``. Deterministic
    as :func:`bundle.pack` is."""
    problems = files_problems(files)
    if problems:
        raise InputsError("\n".join(problems))
    members = [(rel, path.read_bytes()) for rel, path in files]
    members.append((INFO, info_bytes(hashes)))
    return bundle.pack_bytes(members)


def metadata(data: bytes, hashes: Mapping[str, str]) -> dict:
    """What is uploaded beside the bundle as :data:`INPUTS_META`."""
    return {
        "schema": SCHEMA,
        "asset": INPUTS_NAME,
        "sha256": bundle.sha256(data),
        "size": len(data),
        "fingerprint": combine(hashes),
    }


def metadata_bytes(meta: Mapping) -> bytes:
    return (json.dumps(meta, indent=1, sort_keys=True) + "\n").encode("utf-8")


@dataclass
class Unpacked:
    fingerprint: str
    sources: dict[str, str]
    kb_files: int
    img_files: int


def _info(raw: bytes) -> tuple[str, dict[str, str]]:
    try:
        info = json.loads(raw.decode("utf-8"))
        fp, sources = info["fingerprint"], info["sources"]
    except (UnicodeDecodeError, ValueError, KeyError, TypeError) as exc:
        raise InputsError(f"{INFO}: unreadable ({exc})") from None
    if info.get("schema") != SCHEMA:
        raise InputsError(f"{INFO}: schema {info.get('schema')!r}, this code reads {SCHEMA}")
    if not isinstance(sources, dict) or combine(sources) != fp:
        raise InputsError(f"{INFO}: the fingerprint does not match its own source list")
    return fp, sources


def check_metadata(data: bytes, meta: Mapping) -> str:
    """The bundle's fingerprint, after checking ``data`` against ``meta`` (its SHA-256, size
    and fingerprint); raises :class:`InputsError` on any difference."""
    if meta.get("schema") != SCHEMA:
        raise InputsError(f"{INPUTS_META}: schema {meta.get('schema')!r}, this code reads {SCHEMA}")
    digest = bundle.sha256(data)
    if digest != meta.get("sha256") or len(data) != meta.get("size"):
        raise InputsError(
            f"{INPUTS_NAME}: SHA-256 {digest} ({len(data)} bytes) is not the one in"
            f" {INPUTS_META} ({meta.get('sha256')}, {meta.get('size')} bytes): a partial upload?"
        )
    fp, _ = read_info(data)
    if fp != meta.get("fingerprint"):
        raise InputsError(f"{INPUTS_NAME}: its fingerprint is not the one in {INPUTS_META}")
    return fp


def _open(data: bytes) -> tarfile.TarFile:
    try:
        return tarfile.open(fileobj=io.BytesIO(data), mode="r:gz")
    except (tarfile.TarError, OSError, EOFError) as exc:
        raise InputsError(f"{INPUTS_NAME}: not a .tar.gz ({exc})") from None


def read_info(data: bytes) -> tuple[str, dict[str, str]]:
    """The fingerprint and source hashes recorded in the bundle ``data``."""
    with _open(data) as tar:
        try:
            member = tar.getmember(INFO)
        except KeyError:
            raise InputsError(f"{INPUTS_NAME}: no {INFO}") from None
        f = tar.extractfile(member) if member.isfile() else None
        if f is None:
            raise InputsError(f"{INPUTS_NAME}: {INFO} is not a plain file")
        return _info(f.read())


def unpack(data: bytes, kb_dir: Path, img_dir: Path) -> Unpacked:
    """Write the bundle ``data`` into ``kb_dir`` and ``img_dir``, both of which must not exist
    yet or be empty. Every member is checked before anything is written: plain files only, at
    the paths :data:`_MEMBER` allows, no duplicates."""
    kb_dir, img_dir = Path(kb_dir), Path(img_dir)
    for d in (kb_dir, img_dir):
        if d.exists() and any(d.iterdir()):
            raise InputsError(f"{d}: not empty; unpack into a fresh directory")
    with _open(data) as tar:
        members = tar.getmembers()
        if len(members) > MAX_MEMBERS:
            raise InputsError(f"{INPUTS_NAME}: {len(members)} members, more than any build makes")
        seen: set[str] = set()
        for m in members:
            name = m.name
            if not m.isfile():
                raise InputsError(f"{name}: not a plain file")
            if name in seen:
                raise InputsError(f"{name}: twice in the bundle")
            seen.add(name)
            if name == INFO:
                continue
            p = PurePosixPath(name)
            if p.is_absolute() or ".." in p.parts or not _MEMBER.fullmatch(name):
                raise InputsError(f"{name}: not a file an inputs bundle holds")
        if INFO not in seen:
            raise InputsError(f"{INPUTS_NAME}: no {INFO}")
        fp, sources = _info(tar.extractfile(INFO).read())
        counts = {KB_PREFIX: 0, IMG_PREFIX: 0}
        for m in members:
            if m.name == INFO:
                continue
            prefix = KB_PREFIX if m.name.startswith(KB_PREFIX) else IMG_PREFIX
            dest = (kb_dir if prefix == KB_PREFIX else img_dir).joinpath(
                *m.name[len(prefix) :].split("/")
            )
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(tar.extractfile(m).read())
            counts[prefix] += 1
    return Unpacked(fp, sources, counts[KB_PREFIX], counts[IMG_PREFIX])
