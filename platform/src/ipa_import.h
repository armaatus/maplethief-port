// The first-launch IPA import (#182, ADR 0100): the player's own MapleThief-1.5.ipa is checked
// and its bundle, Payload/MapleThief.app/ (the executable included), is extracted into
// <data dir>/game/, where the bundle lookup (game.h's find_bundle) finds it.
//
//   check_ipa     the file's size and SHA-256 against the 1.5 IPA's (original/SHA256SUMS,
//                 tools/mtport/bootstrap.py's IPA_SIZE; tests/test_ipa_import.py keeps them
//                 equal). A file that does not match is looked into for the reason: not a zip,
//                 no Payload/<name>.app, another app, encrypted zip entries, an encrypted
//                 executable (LC_ENCRYPTION_INFO with cryptid != 0 in its armv7 slice), another
//                 version (CFBundleVersion or CFBundleShortVersionString of Info.plist, binary
//                 or XML), or version 1.5 with other bytes.
//   import_ipa    check_ipa, then bootstrap.py's safe-extraction rules on every entry (no
//                 absolute paths, '.', '..' or empty components, no backslashes, drive letters,
//                 control or reserved characters, no names Windows cannot store, no duplicates
//                 or case-only collisions, no file that is also a folder, no links or other
//                 non-regular entries, nothing encrypted), extraction into game.tmp-<pid>/ next
//                 to game/, the executable's SHA-256, then the rename into place: an old game/
//                 goes to game.old-<pid>/ first and comes back if the rename fails, and is
//                 removed after. Leftover game.tmp-* and game.old-* folders (an interrupted
//                 import) are removed at the start of the next one, so an interrupted import
//                 never leaves a half-extracted game/.
//
// No SDL, no UI: paths and bytes in, a result with its reason out, so that every frontend
// calls it: the import screen (import_screen.h), --import-ipa FILE (main.cpp) and the Switch
// build's importer from its SD-card folder (#243). The IPA is untrusted data: read, checked
// and copied, never executed.
#ifndef MAPLETHIEF_PLATFORM_IPA_IMPORT_H
#define MAPLETHIEF_PLATFORM_IPA_IMPORT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace maplethief::frontend {

// The 1.5 IPA: original/SHA256SUMS and IPA_SIZE in tools/mtport/bootstrap.py.
inline constexpr std::uint64_t kIpaSize = 17872861;
inline constexpr std::string_view kIpaSha256 =
    "a6a06f050aa21af9576dbf8af3c832dcbbf38f453886ef1690447f934cb3e387";
// Payload/MapleThief.app/MapleThief (original/SHA256SUMS' work/ipa/... line).
inline constexpr std::string_view kExecutableSha256 =
    "1267cf3e048113c26c8bc354087194da8f9e8ed830fcaa766da170e8e6e32066";
inline constexpr std::string_view kIpaVersion = "1.5";
inline constexpr std::string_view kIpaFileName = "MapleThief-1.5.ipa";

// The folder of the data directory that holds the imported bundle.
inline constexpr std::string_view kGameDirName = "game";
// Files larger than this are refused before they are read (the 1.5 IPA is 17 MB).
inline constexpr std::uint64_t kMaxIpaBytes = 256ull << 20;
// No entry of the IPA is larger than this (its largest is 2.7 MB).
inline constexpr std::size_t kMaxIpaEntryBytes = 64u << 20;

using Sha256 = std::array<std::uint8_t, 32>;

// What the file must be. ipa_expected() is the 1.5 IPA; tests pass their own.
struct IpaExpected {
    std::uint64_t size = 0;
    Sha256 ipa_sha256{};
    Sha256 executable_sha256{};
};
IpaExpected ipa_expected();

// Why a file was refused (kNone: it is the IPA, or it was imported).
enum class IpaProblem {
    kNone,
    kUnreadable,      // the file cannot be opened or read, or is too large
    kNotZip,          // not a zip file at all
    kNotIpa,          // a zip without Payload/<name>.app/
    kOtherApp,        // an IPA of another app
    kEncryptedZip,    // entries encrypted with a zip password
    kEncryptedApp,    // the executable is encrypted (cryptid != 0)
    kOtherVersion,    // MapleThief, but not version 1.5
    kModified,        // version 1.5, but not the 1.5 IPA's bytes (damaged or changed)
    kUnsafeEntry,     // an entry that is not safe to extract
    kWrongExecutable, // the extracted executable is not 1.5's
    kWriteFailed,     // the data directory could not be written
    kInterrupted,     // the progress callback stopped the import
};
// A stable name for logs and tests ("not_zip", "other_version", ...).
const char *ipa_problem_name(IpaProblem problem);

struct IpaResult {
    IpaProblem problem = IpaProblem::kNone;
    std::string why;      // a sentence for the player ("" when kNone)
    std::string version;  // the Info.plist version, when one was read
    std::string game_dir; // import_ipa: <data dir>/game, once imported
    bool ok() const { return problem == IpaProblem::kNone; }
};

// Checks the IPA's bytes against `expected` (see above). Never throws.
IpaResult check_ipa_bytes(std::span<const std::uint8_t> ipa, const IpaExpected &expected);
// Reads the file at `path` (UTF-8) and checks it. `bytes` (if not null) gets its contents.
IpaResult check_ipa(const std::string &path, const IpaExpected &expected,
                    std::vector<std::uint8_t> *bytes = nullptr);

// Called after each extracted entry with (bytes written so far, total bytes to write); return
// false to stop the import (it then ends with kInterrupted and leaves nothing behind). May be
// empty. Called on the importing thread.
using IpaProgress = std::function<bool(std::uint64_t done, std::uint64_t total)>;

// Checks the IPA at `path` and extracts its bundle into <data_dir>/game/ (created; an existing
// game/ is replaced only once the new one is complete). result.game_dir is the folder.
IpaResult import_ipa(const std::string &path, const std::string &data_dir,
                     const IpaExpected &expected, const IpaProgress &progress = {});
// The same from bytes already read (`path` is only named in messages).
IpaResult import_ipa_bytes(std::span<const std::uint8_t> ipa, const std::string &data_dir,
                           const IpaExpected &expected, const IpaProgress &progress = {});

// <data_dir>/game ("" without a data directory).
std::string imported_game_dir(const std::string &data_dir);
// Whether <data_dir>/game holds an imported bundle (a folder with its MapleThief executable).
bool has_imported_game(const std::string &data_dir);

// --- the pieces, for tests ------------------------------------------------------------------

// The SHA-256 of `bytes`; "a6a06f05..." (64 lower-case hex digits) and back (false if not
// 64 hex digits).
Sha256 sha256_of(std::span<const std::uint8_t> bytes);
std::string sha256_hex(const Sha256 &digest);
bool parse_sha256(std::string_view hex, Sha256 *digest);

// bootstrap.py's entry_parts: the components of a zip entry name, or false with `why` if the
// name is unsafe to extract (see above). A trailing '/' (a folder) is dropped first.
bool ipa_entry_parts(std::string_view name, std::vector<std::string> *parts, std::string *why);

// The string value of `key` in the top dictionary of a property list (binary "bplist00" or
// XML); false if it is neither, has no such key, or its value is not a string. The file is
// untrusted: every offset, count and size is checked.
bool plist_string(std::span<const std::uint8_t> plist, std::string_view key, std::string *value);

// The cryptid of the LC_ENCRYPTION_INFO command of a thin 32-bit Mach-O (0 without one);
// false if its load commands run outside it.
bool macho_cryptid(std::span<const std::uint8_t> macho, std::uint32_t *cryptid);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_IPA_IMPORT_H
