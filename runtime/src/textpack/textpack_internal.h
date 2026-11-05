// Text packs (#189, ADR 0108, maplethief/textpack.h): what the runtime's other parts ask.
//
//   CFURLCreateDataAndPropertiesFromResource (cf/bundle.cpp) passes each bundle file it reads
//   through patch_table, which writes the pack's Tables.strings into <Language>/data/*.dat.
//   -[NSBundle localizedStringForKey:value:table:] (foundation/ns_bundle.cpp) asks
//   mt_textpack_lookup first. -[NSString drawAtPoint:withFont:] (cg/text.cpp) reports a text
//   wider than its bitmap context with note_overflow.
#ifndef MAPLETHIEF_RUNTIME_TEXTPACK_TEXTPACK_INTERNAL_H
#define MAPLETHIEF_RUNTIME_TEXTPACK_TEXTPACK_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mt_textpack {

// A text column of the data tables (docs/formats/data-tables.md): the pack's
// "<table>.<row>.<column>" keys name these.
struct TextColumn {
    const char *table;  // the file's stem: "item_etc"
    const char *column; // "name", "description", "detail"
    std::uint32_t offset, width;
    std::uint32_t columns, stride; // the table's header, checked before a field is written
};
// All of them (10), in the format document's order.
const TextColumn *text_columns(std::size_t *count);

// Writes the pack's values into a bundle file read at bundle-relative path `rel`, if it is a
// language's data table (English/data/item_etc.dat, Korean/..., Japanese/...) with the layout
// the game reads. Returns the fields written (0 without a pack). A value longer than its field
// (width - 1 bytes: the field keeps its NUL) is cut at a whole character and logged once.
std::uint32_t patch_table(const std::string &rel, std::vector<std::uint8_t> &bytes);

// A text `width` points wide drawn into a bitmap `limit` points wide: logged once per text
// while a pack is open (the game's text textures are 512 points wide).
void note_overflow(const std::string &utf8, float width, float limit);

// The steps of the two Text_Wrap hooks, on host bytes: the characters of `s` by their UTF-8
// lengths (utf8_length), and the bytes of characters [from, to) (stopping at the end).
std::uint32_t char_count(const std::string &s);
std::string sub_chars(const std::string &s, std::int32_t from, std::int32_t to);

} // namespace mt_textpack

#endif // MAPLETHIEF_RUNTIME_TEXTPACK_TEXTPACK_INTERNAL_H
