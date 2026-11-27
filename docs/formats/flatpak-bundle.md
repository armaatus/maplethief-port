# Flatpak single-file bundles (OSTree static deltas, GVariant)

**Status: specified** for the parts we read. This is not a game format. It is the container of the Linux player's Flatpak bundle our own build writes ([ADR 0193](../adr/0193-linux-glibc-2-35-appimage-and-flatpak-bundle.md), [#349](https://github.com/armaatus/maplethief-port/issues/349)). `flatpak build-bundle` writes it in the Linux build image, and we read it back so that the release guard sees every file it installs. The layout follows OSTree's sources (`src/libostree/ostree-repo-static-delta-private.h`, `ostree-repo-static-delta-processing.c`, `ostree-core.h`) and GLib's [GVariant specification](https://developer.gnome.org/documentation/specifications/gvariant-specification-1.0.html).

- **Parsers**: [`tools/mtport/gvariant.py`](../../tools/mtport/gvariant.py) (GVariant's normal form: basic types, strings, `v`, `m`, arrays, tuples and dict entries; `tuple_slices` for a member's raw bytes). [`tools/mtport/flatpak_bundle.py`](../../tools/mtport/flatpak_bundle.py) (`read`: the bundle's keys and every entry of its tree, each object checked). CLI: `python3 -m mtport.flatpak_bundle info|list BUNDLE`.
- **Tests**: [`tests/test_linux_packages.py`](../../tests/test_linux_packages.py), on `tests/fixtures/linux/fixture.flatpak`. That is a 12 KB bundle of our own synthetic files (`org.example.Fixture`), made by the build image's `flatpak build-export` and `build-bundle`. The tests cover the GVariant specification's examples, every object's checksum, a changed file, a changed commit and a cut-short bundle. Locally, `mtport.release guard` reads the real player bundle (`scripts/release build`).
- **Where it is used**: `mtport.release` expands a `*.flatpak` into the files of its tree.

## GVariant in brief

Fixed-size values are stored in the machine's byte order: little-endian here, and the bundle's `ostree.endianness` key says `l`. They are aligned to their size. Strings end in NUL. A variable-size array or tuple ends with *framing offsets*: little-endian integers of 1, 2, 4 or 8 bytes, the smallest that can address the container. An array has one per element. A tuple has one per variable-size member except the last, stored back to front from the end. A `v` is the value, a NUL and the type string. OSTree stores some numbers big-endian inside these little-endian containers: the timestamps, and the uid, gid and mode of files and directories.

## The superblock

The whole file is one GVariant of type `(a{sv}tayay(a{sv}aya(say)sstayay)aya(uayttay)a(yaytt))`:

| Member | Meaning |
|---|---|
| `a{sv}` | the keys. Flatpak's: `flatpak` (u), `ref` (`app/<id>/<arch>/<branch>`), `metadata` (the app's metadata file), `runtime-repo` (Flathub's `.flatpakrepo` URL), `collection-id`; OSTree's `ostree.endianness`; and per part, `deltas/<from>/<to>/<n>` → `(yay)`: the part inline |
| `t` | timestamp (big-endian) |
| `ay` | from-commit: empty, a delta from scratch (anything else is refused) |
| `ay` | to-commit checksum (32 bytes): SHA-256 of the next member's serialised bytes |
| `(a{sv}aya(say)sstayay)` | the commit: metadata, parent, related, subject, body, timestamp, root dirtree checksum, root dirmeta checksum |
| `aya(uayttay)` | dependencies, then per part: version, checksum, size, unpacked size, objects (33 bytes each: object type, checksum) |
| `a(yaytt)` | fallback objects: must be empty |

## Parts

A part is a compression byte (`x`: xz, `0`: none) and a payload of type `(a(uuu)aa(ayay)ayay)`: the modes (uid, gid and mode, big-endian), the xattr sets, the raw bytes and the operations. The unpacked payload is a little larger than the part's stated size (by up to 126 bytes in our bundles). The reader allows 64 KiB of slack and caps the total. The operations work through the part's objects in order. Their arguments are LEB128 unsigned integers:

| Op | Arguments | Effect |
|---|---|---|
| `S` | metadata object: size, offset | the object is `raw[offset:offset+size]` |
| `S` | content object: mode index, xattr index, size, offset | the same, with that mode |
| `o` | mode index, xattr index, size | open a content object |
| `w` | length, offset | append `raw[offset:offset+length]` |
| `c` | | close the open object |
| `r`, `R`, `B` | | read from another commit, bsdiff: refused |

Object types: 1 file (content), 2 dirtree `(a(say)a(sayay))` (file names to checksums; directory names to dirtree and dirmeta checksums), 3 dirmeta `(uuua(ayay))`, 4 commit. A metadata object's checksum is the SHA-256 of its bytes. A content object's is the SHA-256 of a big-endian u32 header size, four zero bytes, the header `(uuuusa(ayay))` (uid, gid, mode, 0, symlink target, xattrs), then the file's bytes (a symlink has none). The reader checks every object, and refuses files with xattrs. Our bundles have none. It then walks the tree from the commit's root, so a file the tree doesn't name is never listed, and a name without its object is an error.
