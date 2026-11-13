// A small JSON reader and writer for the stubs' files (gamecenter.json), in the plain C++ of
// stubs_internal.h. The reader builds a tree of malloc'd values; the writer appends to a
// growing buffer. RFC 8259 syntax; numbers keep a 64-bit integer when they have no fraction or
// exponent; strings are UTF-8 (\u escapes, surrogate pairs included, are decoded; a lone
// surrogate is refused, #323).
#ifndef MAPLETHIEF_RUNTIME_STUBS_JSON_H
#define MAPLETHIEF_RUNTIME_STUBS_JSON_H

#include <cstddef>
#include <cstdint>

namespace mt_stubs_impl {

enum class JsonType { null = 0, boolean, number, string, array, object };

// Zero-initialize (`JsonValue v{}`): the reader memsets and reallocs these, so they have no
// default member initializers.
struct JsonValue {
    JsonType type; // JsonType::null is 0
    bool boolean;
    double number;
    std::int64_t integer; // the number as an integer, if is_integer
    bool is_integer;
    char *string;     // string: its text, NUL-terminated
    JsonValue *items; // array elements, object member values
    char **keys;      // object member names (keys[i] belongs to items[i])
    std::uint32_t count;
};

// Parses `text` (`size` bytes) into *out. Returns false, with `error` set to a short reason
// and *out freed, if it is not exactly one JSON value (whitespace around it allowed) or nests
// deeper than 64 levels.
bool json_parse(const char *text, std::size_t size, JsonValue *out, const char **error);
void json_free(JsonValue *v);

// The member `key` of an object, or NULL (also for a non-object).
const JsonValue *json_get(const JsonValue *object, const char *key);

// A growing output buffer; zero-initialize it (`JsonOut o{}`).
struct JsonOut {
    char *data;
    std::size_t size, cap;
    bool failed; // out of memory: everything after is dropped
};
void json_raw(JsonOut *o, const char *text);
void json_string(JsonOut *o, const char *text); // quoted and escaped
void json_int(JsonOut *o, std::int64_t v);
void json_double(JsonOut *o, double v); // round-trips; non-finite values are written as 0
void json_out_free(JsonOut *o);

} // namespace mt_stubs_impl

#endif // MAPLETHIEF_RUNTIME_STUBS_JSON_H
