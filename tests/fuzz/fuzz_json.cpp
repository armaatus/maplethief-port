// Fuzz target: the JSON reader and writer of gamecenter.json (runtime/src/stubs/json.cpp, #323,
// ADR 0154). Oracles besides the sanitizers: a refusal has a reason; a text of valid UTF-8 gives
// strings of valid UTF-8 (a lone surrogate used to break that); and an accepted tree, written
// back with the writer, parses again to the same shape.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "fuzz2.h"
#include "stubs/json.h"

namespace {

using namespace mt_stubs_impl;

// Strict UTF-8: no overlong forms, no surrogates, nothing past U+10FFFF.
bool valid_utf8(const unsigned char *p, std::size_t n) {
    std::size_t i = 0;
    while (i < n) {
        const unsigned c = p[i];
        std::size_t len = 0;
        std::uint32_t cp = 0, min = 0;
        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            len = 2, cp = c & 0x1F, min = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3, cp = c & 0x0F, min = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4, cp = c & 0x07, min = 0x10000;
        } else {
            return false;
        }
        if (n - i < len) {
            return false;
        }
        for (std::size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) {
                return false;
            }
            cp = cp << 6 | (p[i + k] & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) {
            return false;
        }
        i += len;
    }
    return true;
}

bool strings_valid(const JsonValue &v) {
    if (v.string != nullptr &&
        !valid_utf8(reinterpret_cast<const unsigned char *>(v.string), std::strlen(v.string))) {
        return false;
    }
    for (std::uint32_t i = 0; i < v.count; i++) {
        if (v.keys != nullptr && !valid_utf8(reinterpret_cast<const unsigned char *>(v.keys[i]),
                                             std::strlen(v.keys[i]))) {
            return false;
        }
        if (!strings_valid(v.items[i])) {
            return false;
        }
    }
    return true;
}

void write(JsonOut *o, const JsonValue &v) {
    switch (v.type) {
    case JsonType::null:
        json_raw(o, "null");
        break;
    case JsonType::boolean:
        json_raw(o, v.boolean ? "true" : "false");
        break;
    case JsonType::number:
        if (v.is_integer) {
            json_int(o, v.integer);
        } else {
            json_double(o, v.number);
        }
        break;
    case JsonType::string:
        json_string(o, v.string);
        break;
    case JsonType::array:
    case JsonType::object: {
        const bool object = v.type == JsonType::object;
        json_raw(o, object ? "{" : "[");
        for (std::uint32_t i = 0; i < v.count; i++) {
            if (i > 0) {
                json_raw(o, ",");
            }
            if (object) {
                json_string(o, v.keys[i]);
                json_raw(o, ":");
            }
            write(o, v.items[i]);
        }
        json_raw(o, object ? "}" : "]");
        break;
    }
    }
}

// The same tree shape: types, counts, keys and strings (numbers may round).
bool same_shape(const JsonValue &a, const JsonValue &b) {
    if (a.type != b.type || a.count != b.count) {
        return false;
    }
    if (a.type == JsonType::boolean && a.boolean != b.boolean) {
        return false;
    }
    if (a.type == JsonType::string && std::strcmp(a.string, b.string) != 0) {
        return false;
    }
    for (std::uint32_t i = 0; i < a.count; i++) {
        if (a.type == JsonType::object && std::strcmp(a.keys[i], b.keys[i]) != 0) {
            return false;
        }
        if (!same_shape(a.items[i], b.items[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

void mt_fuzz2::init() {}

std::vector<mt_fuzz2::Seed> mt_fuzz2::seeds() {
    using mt_fuzz2::bytes_of;
    // gamecenter.json as gamekit.cpp writes it (ADR 0084), and the reader's other forms.
    return {
        {"gamecenter",
         bytes_of(R"({"format": 1, "player": "Local Player", "scores": [{"category": )"
                  R"("com.example.board", "value": 1200, "date": 1735689600.25}], )"
                  R"("achievements": [{"id": "first", "percent": 100, "date": 1735689600}]})")},
        {"escapes", bytes_of(R"(["x\"\\\/\b\f\n\r\t", "é😀", "A"])")},
        {"numbers", bytes_of("[0, -0, 1.5e300, -2.5E-3, 9223372036854775807, -9223372036854775808, "
                             "18446744073709551616, 1e999]")},
        {"literals", bytes_of(" {\"a\": true, \"b\": false, \"c\": null, \"d\": {}, \"e\": []} ")},
        {"nested", bytes_of(std::string(60, '[') + std::string(60, ']'))},
        {"utf8", bytes_of("{\"\xEB\xB8\x8C\xEB\xA3\xA8\xEC\x8A\xA4\": \"\xF0\x9F\x98\x80\"}")},
    };
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    JsonValue v{};
    const char *error = nullptr;
    const char *text = reinterpret_cast<const char *>(data);
    if (!json_parse(text, size, &v, &error)) {
        if (error == nullptr || *error == '\0') {
            mt_fuzz2::broken("JSON refused without a reason");
        }
        return 0;
    }
    if (valid_utf8(data, size) && !strings_valid(v)) {
        mt_fuzz2::broken("valid UTF-8 parsed into a string of invalid UTF-8");
    }
    (void)json_get(&v, "scores");
    JsonOut o{};
    write(&o, v);
    if (!o.failed) {
        JsonValue again{};
        if (!json_parse(o.data, o.size, &again, &error)) {
            mt_fuzz2::broken("the writer's output of an accepted tree does not parse");
        }
        if (!same_shape(v, again)) {
            mt_fuzz2::broken("the writer's output parses to another tree");
        }
        json_free(&again);
    }
    json_out_free(&o);
    json_free(&v);
    return 0;
}
