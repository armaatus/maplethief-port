"""The compendium site as one bundle, and the guard that keeps it out of git (issue #226,
ADR 0121).

- :func:`site_files` lists what a deploy publishes from ``build/compendium/site/``: every
  file but hidden ones (the generator's marker), sorted. :func:`bundle_problems` refuses
  anything the site must never carry (ADR 0121): a file type outside :data:`PUBLISHED_TYPES`
  (so no audio, no raw bundle files, no executables), a symlink, a missing home or 404 page.
- :func:`pack` writes them as one ``.tar.gz``, deterministic (sorted, no dates, no owners), so
  the same site gives the same archive and the same SHA-256.
- :func:`size_report` is the size budget: the total, per directory, and the largest files.
- :func:`guard` lists the tracked files of a checkout that look like compendium output: under
  ``build/``, ``work/`` or ``assets-out/``, a site marker, search index, image manifest,
  site or inputs bundle (or its metadata), a JSON file shaped like the knowledge base, an
  exported page (HTML with the site's footer), an image in the images step's layout
  (``img/monster/...``) or any image outside :data:`IMAGE_DIRS`.
- :func:`find_release` picks the site's draft release out of the releases API's list: a draft
  is never found by its tag, only by listing (ADR 0121).
"""

from __future__ import annotations

import gzip
import hashlib
import io
import json
import re
import subprocess
import tarfile
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from mtport.compendium import site as sitegen

BUNDLE_NAME = "compendium-site.tar.gz"
# The inputs bundle (KB and images) and its metadata, uploaded beside it (inputs.py, #248).
INPUTS_NAME = "compendium-inputs.tar.gz"
INPUTS_META = "compendium-inputs.json"
RELEASE_TAG = "compendium-site"
PAGES_LIMIT = 1_000_000_000  # GitHub Pages: a published site may be at most 1 GB

# File types a deploy may publish. Everything else (audio, .dat tables, .ipa, binaries) is
# refused: ADR 0121 lists what the site never carries.
PUBLISHED_TYPES = frozenset({".html", ".css", ".js", ".png", ".svg", ".json", ".txt", ".xml"})

# Tracked paths that can only be derived output (all three are git-ignored).
DERIVED_DIRS = ("build/", "work/", "assets-out/")
# The only directories that may track images: synthetic golden images, our own drawings and
# the port's own icon (ADR 0125).
IMAGE_DIRS = ("tests/golden/data/", "docs/controls/", "platform/icon/")
IMAGE_TYPES = frozenset(
    {".png", ".apng", ".gif", ".jpg", ".jpeg", ".webp", ".avif", ".bmp", ".svg", ".ico", ".tif"}
)
SITE_FILES = frozenset({sitegen.MARKER, sitegen.SEARCH_INDEX})
EXPORT_LAYOUT = re.compile(r"(^|/)img/(monster|npc|item|skill|map)/")


# --- the bundle ---------------------------------------------------------------------------------


def site_files(site: Path) -> list[tuple[str, Path]]:
    """``(published path, file)`` for every file a deploy publishes, sorted. Hidden files and
    directories (names starting with a dot) are left out, as the Pages artifact leaves them."""
    site = Path(site)
    out = []
    for p in sorted(site.rglob("*")):
        rel = p.relative_to(site)
        if any(part.startswith(".") for part in rel.parts):
            continue
        if p.is_symlink() or p.is_file():
            out.append((rel.as_posix(), p))
    return out


def bundle_problems(files: Sequence[tuple[str, Path]]) -> list[str]:
    """Why ``files`` must not be published, one line each; empty when it may."""
    problems = []
    names = {rel for rel, _ in files}
    for need in ("index.html", sitegen.NOT_FOUND):
        if need not in names:
            problems.append(f"{need}: missing (run scripts/compendium build)")
    for rel, path in files:
        if path.is_symlink():
            problems.append(f"{rel}: a symlink; the bundle holds plain files only")
        elif PurePosixPath(rel).suffix.lower() not in PUBLISHED_TYPES:
            problems.append(
                f"{rel}: file type not published by the site (allowed: "
                + " ".join(sorted(PUBLISHED_TYPES))
                + ")"
            )
    return problems


def pack(files: Sequence[tuple[str, Path]]) -> bytes:
    """The ``.tar.gz`` of ``files``: sorted, mode 0644, owner 0, dated 1970, so the same files
    give the same bytes."""
    return pack_bytes([(rel, path.read_bytes()) for rel, path in files])


def pack_bytes(members: Sequence[tuple[str, bytes]]) -> bytes:
    """The deterministic ``.tar.gz`` of ``(name, bytes)`` pairs, as :func:`pack` writes it."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for rel, data in sorted(members):
            info = tarfile.TarInfo(rel)
            info.size = len(data)
            info.mtime = 0
            info.mode = 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            tar.addfile(info, io.BytesIO(data))
    out = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=out, mtime=0, compresslevel=9) as gz:
        gz.write(raw.getvalue())
    return out.getvalue()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def human(n: int) -> str:
    size = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1000 or unit == "GB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1000
    raise AssertionError("unreachable")


@dataclass
class SizeReport:
    total: int
    count: int
    by_dir: list[tuple[str, int, int]]  # (directory, bytes, files), largest first
    largest: list[tuple[str, int]]

    def lines(self) -> list[str]:
        out = [f"size: {self.count} files, {human(self.total)} ({self.total} bytes)"]
        out += [f"size:   {d:<16} {human(b):>9}  {n} files" for d, b, n in self.by_dir]
        out.append("size: largest files")
        out += [f"size:   {human(b):>9}  {rel}" for rel, b in self.largest]
        if self.total > PAGES_LIMIT:
            out.append(f"size: over the GitHub Pages limit of {human(PAGES_LIMIT)}")
        return out


def _group(rel: str) -> str:
    parts = rel.split("/")
    if parts[0] == sitegen.IMG_DIR and len(parts) > 2:
        return f"{parts[0]}/{parts[1]}/"
    return parts[0] + "/" if len(parts) > 1 else "(top level)"


def size_report(files: Sequence[tuple[str, Path]], top: int = 10) -> SizeReport:
    sizes = [(rel, path.stat().st_size) for rel, path in files]
    groups: dict[str, list[int]] = {}
    for rel, b in sizes:
        g = groups.setdefault(_group(rel), [0, 0])
        g[0] += b
        g[1] += 1
    by_dir = sorted(((d, b, n) for d, (b, n) in groups.items()), key=lambda x: (-x[1], x[0]))
    largest = sorted(sizes, key=lambda x: (-x[1], x[0]))[:top]
    return SizeReport(sum(b for _, b in sizes), len(sizes), by_dir, largest)


# --- the guard ----------------------------------------------------------------------------------


def tracked_files(repo: Path) -> list[str]:
    res = subprocess.run(
        ["git", "-C", str(repo), "ls-files", "-z"], capture_output=True, check=True
    )
    return sorted(p for p in res.stdout.decode("utf-8", "surrogateescape").split("\0") if p)


def looks_like_kb(data: bytes | None) -> bool:
    """A JSON object shaped like a knowledge-base file as kb.py writes it
    (docs/compendium/kb.md): ``schema`` with ``kind`` and ``entities``, or the index's
    ``schema`` with ``kinds`` and ``relations``."""
    if not data:
        return False
    try:
        doc = json.loads(data)
    except (ValueError, UnicodeDecodeError, RecursionError):
        return False
    if not isinstance(doc, dict) or "schema" not in doc:
        return False
    return {"kind", "entities"} <= doc.keys() or {"kinds", "relations"} <= doc.keys()


def guard_problem(rel: str, read: Callable[[str], bytes | None] | None = None) -> str | None:
    """Why the tracked path ``rel`` looks like compendium output, or ``None``. ``read`` returns
    the file's bytes (for the exported-page and knowledge-base tests); without it the
    content is not looked at."""
    p = PurePosixPath(rel)
    name, suffix = p.name, p.suffix.lower()
    if rel.startswith(DERIVED_DIRS):
        return "under a git-ignored output directory (build/, work/, assets-out/)"
    if name in SITE_FILES:
        return "a compendium site file"
    if name.endswith(BUNDLE_NAME):
        return "a compendium site bundle"
    if name.endswith((INPUTS_NAME, INPUTS_META)):
        return "a compendium inputs bundle or its metadata"
    if name == "manifest.json" and p.parent.name == sitegen.IMG_DIR:
        return "a compendium image manifest"
    if EXPORT_LAYOUT.search(rel):
        return "in the compendium images layout (img/<kind>/...)"
    if suffix in IMAGE_TYPES and not rel.startswith(IMAGE_DIRS):
        return "an image outside " + ", ".join(IMAGE_DIRS) + " (exported game art?)"
    if suffix == ".json" and read is not None and looks_like_kb(read(rel)):
        return "a compendium knowledge-base file"
    if suffix in (".html", ".htm") and read is not None:
        data = read(rel)
        if data is not None and sitegen.DISCLAIMER.encode("utf-8") in data:
            return "an exported compendium page (it carries the site's footer)"
    return None


def guard(repo: Path, paths: Iterable[str] | None = None) -> list[str]:
    """One line per tracked file of the checkout ``repo`` that looks like compendium output."""
    repo = Path(repo)

    def read(rel: str) -> bytes | None:
        f = repo / rel
        return f.read_bytes() if f.is_file() else None

    out = []
    for rel in tracked_files(repo) if paths is None else sorted(paths):
        why = guard_problem(rel, read)
        if why:
            out.append(f"{rel}: {why}")
    return out


# --- the release --------------------------------------------------------------------------------


class ReleaseError(Exception):
    pass


def find_release(releases: Iterable[Mapping], tag: str = RELEASE_TAG) -> Mapping | None:
    """The draft release for the site among ``releases`` (the releases API's objects), or
    ``None``. A *published* release with the tag is an error: the bundle would be public."""
    hits = [r for r in releases if r.get("tag_name") == tag]
    published = [r for r in hits if not r.get("draft")]
    if published:
        raise ReleaseError(
            f"release {tag!r} is published (id {published[0].get('id')}); the site bundle must"
            " sit on a draft release. Turn it back into a draft or delete it."
        )
    if len(hits) > 1:
        ids = ", ".join(str(r.get("id")) for r in hits)
        raise ReleaseError(f"{len(hits)} draft releases are tagged {tag!r} ({ids}); keep one")
    return hits[0] if hits else None
