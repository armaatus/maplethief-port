// A reader for binary property lists ("bplist00", Apple's CFBinaryPList.c format): the bundle's
// Info.plist and its *.lproj/*.strings tables are binary plists (#49, ADR 0035). The bytes come
// from the extracted IPA and are untrusted: every offset, count and reference is checked, and
// a reference cycle or a nesting deeper than 64 fails the parse.
//
// Supported objects: null, booleans, integers (1-8 bytes; 16-byte ones keep their low 64 bits),
// reals (4 or 8 bytes), dates (as their double), data, ASCII and UTF-16 strings, UIDs (as
// integers), arrays, sets (as arrays) and dictionaries with string keys.
#ifndef MAPLETHIEF_RUNTIME_FOUNDATION_PLIST_H
#define MAPLETHIEF_RUNTIME_FOUNDATION_PLIST_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mt_ns {

struct PlistValue {
    enum Kind { null, boolean, integer, real, date, data, string, array, dict };
    Kind kind = null;
    bool b = false;
    std::int64_t i = 0;
    double f = 0.0;
    std::u16string s;                                           // string
    std::vector<std::uint8_t> bytes;                            // data
    std::vector<PlistValue> items;                              // array
    std::vector<std::pair<std::u16string, PlistValue>> entries; // dict, in file order

    // The value of a dictionary entry, or nullptr.
    const PlistValue *find(const std::u16string &key) const;
};

// Parses a binary plist; false (with a reason in *error) if it is not one or is malformed.
bool plist_parse(const std::uint8_t *bytes, std::size_t n, PlistValue *out, std::string *error);

} // namespace mt_ns

#endif // MAPLETHIEF_RUNTIME_FOUNDATION_PLIST_H
