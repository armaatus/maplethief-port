// Zip files for the save export and import (#148, ADR 0078): just enough of the format
// (PKWARE APPNOTE 6.3) to write a set of save files and to read one back, or one that the
// Finder, Windows' "Send to > Compressed folder" or `zip` made.
//
//   writing  every entry stored (method 0, no compression: the saves are ~25 KB in all), UTF-8
//            names (flag bit 11), one central directory, no zip64, no comment.
//   reading  stored and deflated (method 8, through the pinned stb_image's raw inflate, ADR
//            0003: no new dependency) entries, found through the central directory. The input
//            is untrusted: every offset and size is checked, the CRC-32 of each entry too.
//            Refused: zip64, encryption, spanned archives, other methods, more than
//            kMaxZipEntries entries, an entry over the caller's size limit.
//
// No SDL, no file I/O: bytes in, bytes out (save_tools.cpp reads and writes the files). The IPA
// import (#182, ipa_import.h) reads the player's IPA through zip_list and zip_extract.
#ifndef MAPLETHIEF_PLATFORM_SAVE_ZIP_H
#define MAPLETHIEF_PLATFORM_SAVE_ZIP_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace maplethief::frontend {

inline constexpr std::size_t kMaxZipEntries = 4096;

struct ZipEntry {
    std::string name; // as stored: "Documents/theif0" ('/' separated, never '\')
    std::vector<std::uint8_t> bytes;
};

// The CRC-32 (IEEE 802.3, the zip format's) of `bytes`.
std::uint32_t crc32(std::span<const std::uint8_t> bytes);

// MS-DOS date and time fields (local headers and the central directory), from a calendar date
// (year 1980-2107) and time; seconds are stored in 2-second steps.
struct DosTime {
    std::uint16_t time = 0;
    std::uint16_t date = (1 << 5) | 1; // 1980-01-01
};
DosTime dos_time(int year, int month, int day, int hour, int minute, int second);

// A zip file holding `entries`, in order, each stored. The names must be unique.
std::vector<std::uint8_t> zip_store(std::span<const ZipEntry> entries, DosTime when = {});

// The files in the zip `zip` (directories left out), in central-directory order. False, with
// `why` in plain words, if it is not a zip this reader can trust; an entry larger than
// `max_entry_bytes` is refused before it is inflated. zip_list + zip_extract underneath.
bool zip_read(std::span<const std::uint8_t> zip, std::size_t max_entry_bytes,
              std::vector<ZipEntry> *entries, std::string *why);
// The same, of only the files `keep` takes by name ('\' made '/'): every name is checked
// first, then the kept files are inflated, and more than `max_total_bytes` in them together is
// refused before any is (#324).
bool zip_read(std::span<const std::uint8_t> zip, std::size_t max_entry_bytes,
              std::size_t max_total_bytes, const std::function<bool(const std::string &)> &keep,
              std::vector<ZipEntry> *entries, std::string *why);

// One entry of the central directory as stored, for a caller that checks the entries itself
// before it reads any (the IPA import, #182, ipa_import.h): every entry, folders included.
struct ZipItem {
    std::string name;          // as stored: '\' kept, a folder's ends in '/'
    std::uint16_t made_by = 0; // "version made by" (high byte: the host system, 3 = Unix)
    std::uint16_t flags = 0;   // general purpose flags (bit 0: encrypted)
    std::uint16_t method = 0;  // 0 stored, 8 deflated
    std::uint32_t crc = 0;
    std::uint32_t packed = 0;   // compressed size
    std::uint32_t size = 0;     // uncompressed size
    std::uint32_t external = 0; // external attributes (a Unix host's mode in the high 16 bits)
    std::uint32_t local_at = 0; // the local header's offset
    bool is_dir() const { return !name.empty() && name.back() == '/'; }
    bool encrypted() const { return (flags & 1) != 0; }
};

// The central directory of `zip`, in order. False (with `why`) if it is not a zip, a spanned
// or zip64 one, has more than kMaxZipEntries entries, or a central entry runs outside it. The
// entries' names, methods and data are not checked: zip_extract checks an entry's.
bool zip_list(std::span<const std::uint8_t> zip, std::vector<ZipItem> *items, std::string *why);

// The bytes of `item` (zip_list's, of the same `zip`). False (with `why`) if it is encrypted,
// neither stored nor deflated, larger than `max_entry_bytes` (refused before it is inflated),
// its local header or data lie outside the zip, its data does not inflate to its size or its
// CRC-32 does not match.
bool zip_extract(std::span<const std::uint8_t> zip, const ZipItem &item,
                 std::size_t max_entry_bytes, std::vector<std::uint8_t> *bytes, std::string *why);

// Test seam (#413): while on, zip_extract's allocation for an entry's bytes throws
// std::bad_alloc, as one the host cannot satisfy would. Off by default; tests only.
void zip_fail_alloc_for_test(bool on);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SAVE_ZIP_H
