// Adding a pack from anywhere on a desktop (#422, ADR 0149, 0154): the packs screen's "Add a
// pack..." row (a folder picker) and a folder or zip dropped on the window while the screen is
// open. No SDL: paths and bytes in, a verdict out (packs.cpp drives it from the menu).
//
//   kind     the pack's pack.strings names it (mt_pack_manifest_kind); else a textpack.strings
//            makes it text; else its files tell: a .png texture, a .wav / .mp3 / .flac audio.
//            None of these: refused ("give it a pack.strings with its kind").
//   folder   added by its path: checked as the start checks it (read_pack), then appended to its
//            kind's list (on, last). Nothing is written to the folder.
//   zip      unpacked into <base>/<kind's pack folder>/<name>/, then listed (off). <name> is the
//            zip's one top folder when every entry is below it, else the zip's file name without
//            ".zip". Checked whole before anything is written, as untrusted input (ADR 0149):
//            - every entry name safe (ipa_entry_parts: no absolute path, '..', drive letter,
//              backslash or control character), no two that differ only in case, no file that
//              is also a folder
//            - no link or other special file (a Unix host's mode), no encrypted entry
//            - at most the kind's files (MT_TEXPACK_MAX_FILES, MT_AUDIO_PACK_MAX_FILES; the zip
//              reader's kMaxZipEntries caps every kind) and the kind's bytes per file
//              (MT_TEXPACK_MAX_FILE, MT_AUDIO_PACK_MAX_FILE, MT_TEXTPACK_MAX_FILE)
//            - at most 1 GiB in all (the entries' sizes, and the zip file itself)
//            - a <name> that is a safe folder name without '|', not taken in the pack folder
//            The first breach refuses it whole. Then it unpacks into a hidden staging folder
//            beside <name> (each entry's CRC-32 checked), which is checked as a pack (read_pack)
//            and renamed to <name>; a refusal or a failed write removes the staging folder.
//            A Finder zip's __MACOSX/ folder and .DS_Store files are left out.
#ifndef MAPLETHIEF_PLATFORM_PACK_ADD_H
#define MAPLETHIEF_PLATFORM_PACK_ADD_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "packs.h"
#include "settings.h"

namespace maplethief::frontend {

inline constexpr std::uint64_t kMaxPackZipBytes = std::uint64_t{1} << 30; // 1 GiB

struct PackAdd {
    bool ok = false;
    PackKind kind = PackKind::kTexture;
    std::string path;  // the pack's folder: the one added, or where the zip went
    std::string title; // pack_title of its manifest
    std::string why;   // !ok: why it was refused, a short English clause
};

// The kind of the pack in folder `dir`, or nullopt with `why`.
std::optional<PackKind> pack_folder_kind(const std::string &dir, std::string *why);

// The folder `dir` as a pack to add by its path: its kind and the manifest's verdict.
PackAdd check_pack_folder(const std::string &dir, const std::string &port_version);

// The zip `zip` (its file's name `zip_name`, for <name>) unpacked into `base`'s pack folder of
// its kind under the rules above; `max_total` bytes in all at most (tests pass less).
PackAdd unpack_pack_zip(std::span<const std::uint8_t> zip, const std::string &zip_name,
                        const std::string &base, const std::string &port_version,
                        std::uint64_t max_total = kMaxPackZipBytes);
// The same of the zip file at `path`, refused unread when larger than `max_total`.
PackAdd unpack_pack_zip_file(const std::string &path, const std::string &base,
                             const std::string &port_version,
                             std::uint64_t max_total = kMaxPackZipBytes);

// A path chosen in the folder picker or dropped on the packs screen: a folder is checked
// (check_pack_folder) and appended to its kind's list in `s`; a file ending in .zip is unpacked
// (unpack_pack_zip_file) and listed. The change to apply (key "" for none), the status line and
// the kind's pack it is now; `base` "" (no data folder): a zip is refused.
struct PackAddChange {
    std::string key, value;
    std::string status;
    PackAdd added;
};
PackAddChange add_pack_path(const std::string &path, const Settings &s, const std::string &base,
                            const std::string &port_version);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_PACK_ADD_H
