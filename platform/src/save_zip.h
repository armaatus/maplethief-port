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
// No SDL, no file I/O: bytes in, bytes out (save_tools.cpp reads and writes the files).
#ifndef MAPLETHIEF_PLATFORM_SAVE_ZIP_H
#define MAPLETHIEF_PLATFORM_SAVE_ZIP_H

#include <cstddef>
#include <cstdint>
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
// `max_entry_bytes` is refused before it is inflated.
bool zip_read(std::span<const std::uint8_t> zip, std::size_t max_entry_bytes,
              std::vector<ZipEntry> *entries, std::string *why);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SAVE_ZIP_H
