// A text pack's .strings files (#189, ADR 0108, docs/formats/text-pack.md): Apple's text syntax
// in strict UTF-8, parsed as untrusted data. tools/mtport/textpack.py parses by the same rules
// (tests/test_textpack.py and textpack_test.cpp check the same cases).
//
//   file     an optional UTF-8 byte order mark, then entries, white space and comments
//   entry    "key" = "value" ;
//   comment  /* ... */ (not nested) or // to the end of the line
//   string   between double quotes; escapes \" \\ \' \n \t \r and \Uxxxx / \uxxxx (four hex
//            digits, UTF-16: a surrogate pair is two escapes); a line break may be in it
//
// Refused, whole: more than MT_TEXTPACK_MAX_FILE bytes or MT_TEXTPACK_MAX_ENTRIES entries,
// bytes that are not UTF-8 (overlong forms, surrogates and code points past U+10FFFF
// included), a syntax error, an unknown escape, a lone surrogate, a NUL character, an empty
// key, a key longer than MT_TEXTPACK_MAX_KEY bytes or a value longer than MT_TEXTPACK_MAX_VALUE
// bytes. The reason names the line (1-based). A key given twice keeps its last value (counted).
//
// Also the UTF-8 helpers the pack's hooks and encodings use.
#ifndef MAPLETHIEF_RUNTIME_TEXTPACK_STRINGS_FILE_H
#define MAPLETHIEF_RUNTIME_TEXTPACK_STRINGS_FILE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace mt_textpack {

struct StringsFile {
    std::map<std::string, std::string> entries; // UTF-8 key -> UTF-8 value
    std::uint32_t duplicates = 0;               // keys given more than once (the last kept)
};

// Parses `n` bytes; false with `*why` ("line 3: ...") if they are refused.
bool parse_strings(const std::uint8_t *bytes, std::size_t n, StringsFile *out, std::string *why);

// Whether bytes [p, p + n) are strict UTF-8 (true for an empty range). *bad: the offset of the
// first byte that is not, when false.
bool valid_utf8(const std::uint8_t *p, std::size_t n, std::size_t *bad = nullptr);

// The length of the UTF-8 character that starts with `lead`: 1 for ASCII and for a byte that
// cannot start one (a continuation byte or 0xF8-0xFF: stepped over one at a time), 2, 3 or 4.
unsigned utf8_length(std::uint8_t lead);

// Cuts UTF-8 text to at most `max` bytes at a character boundary (never inside a character).
std::string utf8_cut(const std::string &s, std::size_t max);

} // namespace mt_textpack

#endif // MAPLETHIEF_RUNTIME_TEXTPACK_STRINGS_FILE_H
