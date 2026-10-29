// The NIBArchive reader (#54, docs/formats/nibarchive.md): Apple's compiled-nib format, of
// which the app ships one file, MainWindow.nib. The file is untrusted data: parse() checks
// every count, offset, varint, index and length against the file, and refuses gaps, overlaps
// and trailing bytes between the four tables, with the same rules as the Python parser
// (tools/mtport/nib.py). Nothing here runs guest code; app.cpp turns an Archive into objects.
#ifndef MAPLETHIEF_RUNTIME_UIKIT_NIB_H
#define MAPLETHIEF_RUNTIME_UIKIT_NIB_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mt_ui::nib {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

enum Type : u8 {
    k_i8 = 0,
    k_i16 = 1,
    k_i32 = 2,
    k_i64 = 3,
    k_true = 4,
    k_false = 5,
    k_f32 = 6,
    k_f64 = 7,
    k_data = 8,
    k_nil = 9,
    k_object = 10,
};

struct Value {
    u32 key = 0; // index into Archive::keys
    u8 type = k_nil;
    std::int64_t integer = 0; // k_i8..k_i64; 1/0 for k_true/k_false
    double real = 0;          // k_f32, k_f64
    u32 data_offset = 0;      // k_data: the bytes in Archive::bytes
    u32 data_length = 0;
    u32 object = 0; // k_object: index into Archive::objects
};

struct Object {
    u32 cls = 0;   // index into Archive::classes
    u32 first = 0; // its values: values[first, first + count)
    u32 count = 0;
};

struct Archive {
    u32 format_version = 0, coder_version = 0;
    std::vector<Object> objects;
    std::vector<std::string> keys;
    std::vector<Value> values;
    std::vector<std::string> classes; // without the NUL
    std::vector<u8> bytes;            // the file

    // The class name of object `index` ("" if out of range).
    const std::string &class_name(u32 index) const;
    // The first value of object `index` with key `key`, or nullptr.
    const Value *find(u32 index, const char *key) const;
    // The objects an array object refers to: its UINibEncoderEmptyKey values of type
    // k_object, in order.
    std::vector<u32> elements(u32 index) const;
    // The bytes of a k_data value.
    const u8 *data(const Value &v) const;
    // A geometry value (observed, not documented): k_data whose first byte is 0x06, then
    // `count` little-endian f32s. False for anything else.
    bool floats(const Value &v, float *out, u32 count) const;
    // A number value (k_i8..k_f64, k_true/k_false) as a double; false for other types.
    bool number(const Value &v, double *out) const;
};

// Parses a NIBArchive file. Returns false, with a one-line reason in *error, if any check
// fails; *out is then unspecified.
bool parse(const u8 *bytes, std::size_t size, Archive *out, std::string *error);

} // namespace mt_ui::nib

#endif // MAPLETHIEF_RUNTIME_UIKIT_NIB_H
