"""Zip files built in code for the zip and IPA seeds (synthetic: nothing from the game)."""

from __future__ import annotations

import io
import zipfile

# A fixed time, so that the seeds are the same bytes on every run.
WHEN = (2010, 6, 1, 12, 0, 0)


def make_zip(
    entries: list[tuple[str, bytes]],
    *,
    deflate: bool = False,
    unix: bool = False,
    mode: int = 0o644,
) -> bytes:
    """A zip of (name, bytes) in order; a name ending in '/' is a folder."""
    buf = io.BytesIO()
    method = zipfile.ZIP_DEFLATED if deflate else zipfile.ZIP_STORED
    with zipfile.ZipFile(buf, "w", method) as z:
        for name, data in entries:
            info = zipfile.ZipInfo(name, WHEN)
            info.compress_type = method
            if unix:
                info.create_system = 3
                info.external_attr = (0o40755 if name.endswith("/") else 0o100000 | mode) << 16
            z.writestr(info, data)
    return buf.getvalue()
