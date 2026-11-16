// The stubs' JSON reader and writer (json.h).
#include "stubs/json.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mt_stubs_impl {
namespace {

constexpr int k_max_depth = 64;
static_assert(k_json_max_elements == 4096, "the error message names the limit");

struct Parser {
    const char *p;
    const char *end;
    const char *error;
    std::uint32_t elements; // array items and object members so far (k_json_max_elements)
};

void skip_space(Parser &ps) {
    while (ps.p < ps.end && (*ps.p == ' ' || *ps.p == '\t' || *ps.p == '\n' || *ps.p == '\r')) {
        ps.p++;
    }
}

bool fail(Parser &ps, const char *why) {
    if (ps.error == nullptr) {
        ps.error = why;
    }
    return false;
}

// Growable byte buffer for decoded strings.
struct Bytes {
    char *data;
    std::size_t size, cap;
};

bool put(Bytes &b, char c) {
    if (b.size + 1 >= b.cap) {
        const std::size_t grown = b.cap != 0 ? b.cap * 2 : 32;
        char *p = static_cast<char *>(std::realloc(b.data, grown));
        if (p == nullptr) {
            return false;
        }
        b.data = p;
        b.cap = grown;
    }
    b.data[b.size++] = c;
    return true;
}

bool put_utf8(Bytes &b, std::uint32_t cp) {
    if (cp < 0x80) {
        return put(b, static_cast<char>(cp));
    }
    if (cp < 0x800) {
        return put(b, static_cast<char>(0xC0 | (cp >> 6))) &&
               put(b, static_cast<char>(0x80 | (cp & 0x3F)));
    }
    if (cp < 0x10000) {
        return put(b, static_cast<char>(0xE0 | (cp >> 12))) &&
               put(b, static_cast<char>(0x80 | ((cp >> 6) & 0x3F))) &&
               put(b, static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return put(b, static_cast<char>(0xF0 | (cp >> 18))) &&
           put(b, static_cast<char>(0x80 | ((cp >> 12) & 0x3F))) &&
           put(b, static_cast<char>(0x80 | ((cp >> 6) & 0x3F))) &&
           put(b, static_cast<char>(0x80 | (cp & 0x3F)));
}

bool hex4(Parser &ps, std::uint32_t *out) {
    if (ps.end - ps.p < 4) {
        return fail(ps, "truncated \\u escape");
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        const char c = *ps.p++;
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            return fail(ps, "bad \\u escape");
        }
    }
    *out = v;
    return true;
}

// A string starting at the opening quote; *out is malloc'd.
bool parse_string(Parser &ps, char **out) {
    *out = nullptr;
    ps.p++; // the opening quote
    Bytes b{nullptr, 0, 0};
    while (true) {
        if (ps.p >= ps.end) {
            std::free(b.data);
            return fail(ps, "unterminated string");
        }
        const unsigned char c = static_cast<unsigned char>(*ps.p++);
        bool ok = true;
        if (c == '"') {
            break;
        }
        if (c < 0x20) {
            std::free(b.data);
            return fail(ps, "control character in string");
        }
        if (c != '\\') {
            ok = put(b, static_cast<char>(c));
        } else {
            if (ps.p >= ps.end) {
                std::free(b.data);
                return fail(ps, "unterminated string");
            }
            const char e = *ps.p++;
            switch (e) {
            case '"':
                ok = put(b, '"');
                break;
            case '\\':
                ok = put(b, '\\');
                break;
            case '/':
                ok = put(b, '/');
                break;
            case 'b':
                ok = put(b, '\b');
                break;
            case 'f':
                ok = put(b, '\f');
                break;
            case 'n':
                ok = put(b, '\n');
                break;
            case 'r':
                ok = put(b, '\r');
                break;
            case 't':
                ok = put(b, '\t');
                break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(ps, &cp)) {
                    std::free(b.data);
                    return false;
                }
                if (cp >= 0xD800 && cp < 0xDC00 && ps.end - ps.p >= 6 && ps.p[0] == '\\' &&
                    ps.p[1] == 'u') {
                    const char *save = ps.p;
                    ps.p += 2;
                    std::uint32_t lo = 0;
                    if (hex4(ps, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        ps.p = save;
                        ps.error = nullptr;
                    }
                }
                if (cp == 0) {
                    // It would cut the C string every reader of the value sees (#413).
                    std::free(b.data);
                    return fail(ps, "\\u0000 in string");
                }
                ok = put_utf8(b, cp);
                break;
            }
            default:
                std::free(b.data);
                return fail(ps, "bad escape");
            }
        }
        if (!ok) {
            std::free(b.data);
            return fail(ps, "out of memory");
        }
    }
    if (!put(b, '\0')) {
        std::free(b.data);
        return fail(ps, "out of memory");
    }
    *out = b.data;
    return true;
}

bool parse_value(Parser &ps, JsonValue *v, int depth);

bool literal(Parser &ps, const char *word) {
    const std::size_t n = std::strlen(word);
    if (static_cast<std::size_t>(ps.end - ps.p) < n || std::memcmp(ps.p, word, n) != 0) {
        return fail(ps, "unexpected token");
    }
    ps.p += n;
    return true;
}

bool parse_number(Parser &ps, JsonValue *v) {
    const char *start = ps.p;
    bool integer = true;
    if (ps.p < ps.end && *ps.p == '-') {
        ps.p++;
    }
    if (ps.p >= ps.end || *ps.p < '0' || *ps.p > '9') {
        return fail(ps, "bad number");
    }
    while (ps.p < ps.end && ((*ps.p >= '0' && *ps.p <= '9') || *ps.p == '.' || *ps.p == 'e' ||
                             *ps.p == 'E' || *ps.p == '+' || *ps.p == '-')) {
        if (*ps.p == '.' || *ps.p == 'e' || *ps.p == 'E') {
            integer = false;
        }
        ps.p++;
    }
    char buf[64];
    const std::size_t n = static_cast<std::size_t>(ps.p - start);
    if (n >= sizeof buf) {
        return fail(ps, "number too long");
    }
    std::memcpy(buf, start, n);
    buf[n] = '\0';
    char *stop = nullptr;
    v->type = JsonType::number;
    v->number = std::strtod(buf, &stop);
    if (stop != buf + n) {
        return fail(ps, "bad number");
    }
    if (integer) {
        v->integer = std::strtoll(buf, &stop, 10);
        v->is_integer = stop == buf + n;
    }
    return true;
}

// Appends a zeroed element to v's items (and keys, for an object); returns it or NULL.
JsonValue *add_item(JsonValue *v, char *key) {
    const std::uint32_t n = v->count + 1;
    auto *items = static_cast<JsonValue *>(std::realloc(v->items, sizeof(JsonValue) * n));
    if (items == nullptr) {
        return nullptr;
    }
    v->items = items;
    if (v->type == JsonType::object) {
        auto *keys = static_cast<char **>(std::realloc(v->keys, sizeof(char *) * n));
        if (keys == nullptr) {
            return nullptr;
        }
        v->keys = keys;
        v->keys[v->count] = key;
    }
    std::memset(&v->items[v->count], 0, sizeof(JsonValue));
    v->count = n;
    return &v->items[n - 1];
}

bool parse_container(Parser &ps, JsonValue *v, int depth, bool object) {
    v->type = object ? JsonType::object : JsonType::array;
    const char close = object ? '}' : ']';
    ps.p++;
    skip_space(ps);
    if (ps.p < ps.end && *ps.p == close) {
        ps.p++;
        return true;
    }
    while (true) {
        char *key = nullptr;
        if (object) {
            skip_space(ps);
            if (ps.p >= ps.end || *ps.p != '"') {
                return fail(ps, "expected a member name");
            }
            if (!parse_string(ps, &key)) {
                return false;
            }
            skip_space(ps);
            if (ps.p >= ps.end || *ps.p != ':') {
                std::free(key);
                return fail(ps, "expected ':'");
            }
            ps.p++;
        }
        if (++ps.elements > k_json_max_elements) {
            std::free(key);
            return fail(ps, "more than 4096 elements");
        }
        JsonValue *item = add_item(v, key);
        if (item == nullptr) {
            std::free(key);
            return fail(ps, "out of memory");
        }
        if (!parse_value(ps, item, depth + 1)) {
            return false;
        }
        skip_space(ps);
        if (ps.p < ps.end && *ps.p == ',') {
            ps.p++;
            continue;
        }
        if (ps.p < ps.end && *ps.p == close) {
            ps.p++;
            return true;
        }
        return fail(ps, object ? "expected ',' or '}'" : "expected ',' or ']'");
    }
}

bool parse_value(Parser &ps, JsonValue *v, int depth) {
    if (depth > k_max_depth) {
        return fail(ps, "nested too deeply");
    }
    skip_space(ps);
    if (ps.p >= ps.end) {
        return fail(ps, "unexpected end");
    }
    switch (*ps.p) {
    case '{':
        return parse_container(ps, v, depth, true);
    case '[':
        return parse_container(ps, v, depth, false);
    case '"':
        v->type = JsonType::string;
        return parse_string(ps, &v->string);
    case 't':
        v->type = JsonType::boolean;
        v->boolean = true;
        return literal(ps, "true");
    case 'f':
        v->type = JsonType::boolean;
        return literal(ps, "false");
    case 'n':
        return literal(ps, "null");
    default:
        return parse_number(ps, v);
    }
}

void reserve(JsonOut *o, std::size_t more) {
    if (o->failed || o->size + more + 1 <= o->cap) {
        return;
    }
    std::size_t cap = o->cap != 0 ? o->cap : 256;
    while (cap < o->size + more + 1) {
        cap *= 2;
    }
    char *p = static_cast<char *>(std::realloc(o->data, cap));
    if (p == nullptr) {
        o->failed = true;
        return;
    }
    o->data = p;
    o->cap = cap;
}

void append(JsonOut *o, const char *text, std::size_t n) {
    reserve(o, n);
    if (o->failed) {
        return;
    }
    std::memcpy(o->data + o->size, text, n);
    o->size += n;
    o->data[o->size] = '\0';
}

} // namespace

bool json_parse(const char *text, std::size_t size, JsonValue *out, const char **error) {
    std::memset(out, 0, sizeof *out);
    Parser ps{text, text + size, nullptr, 0};
    bool ok = parse_value(ps, out, 0);
    if (ok) {
        skip_space(ps);
        if (ps.p != ps.end) {
            ok = fail(ps, "text after the value");
        }
    }
    if (!ok) {
        json_free(out);
        if (error != nullptr) {
            *error = ps.error != nullptr ? ps.error : "invalid JSON";
        }
    }
    return ok;
}

void json_free(JsonValue *v) {
    if (v == nullptr) {
        return;
    }
    for (std::uint32_t i = 0; i < v->count; i++) {
        json_free(&v->items[i]);
        if (v->keys != nullptr) {
            std::free(v->keys[i]);
        }
    }
    std::free(v->items);
    std::free(v->keys);
    std::free(v->string);
    std::memset(v, 0, sizeof *v);
}

const JsonValue *json_get(const JsonValue *object, const char *key) {
    if (object == nullptr || object->type != JsonType::object) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < object->count; i++) {
        if (object->keys[i] != nullptr && std::strcmp(object->keys[i], key) == 0) {
            return &object->items[i];
        }
    }
    return nullptr;
}

void json_raw(JsonOut *o, const char *text) { append(o, text, std::strlen(text)); }

void json_string(JsonOut *o, const char *text) {
    append(o, "\"", 1);
    for (const char *p = text; *p != '\0'; p++) {
        const unsigned char c = static_cast<unsigned char>(*p);
        char esc[8];
        switch (c) {
        case '"':
            append(o, "\\\"", 2);
            break;
        case '\\':
            append(o, "\\\\", 2);
            break;
        case '\n':
            append(o, "\\n", 2);
            break;
        case '\r':
            append(o, "\\r", 2);
            break;
        case '\t':
            append(o, "\\t", 2);
            break;
        default:
            if (c < 0x20) {
                std::snprintf(esc, sizeof esc, "\\u%04x", c);
                append(o, esc, 6);
            } else {
                append(o, p, 1);
            }
        }
    }
    append(o, "\"", 1);
}

void json_int(JsonOut *o, std::int64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%" PRId64, v);
    json_raw(o, buf);
}

void json_double(JsonOut *o, double v) {
    char buf[40];
    if (!std::isfinite(v)) {
        v = 0.0;
    }
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        // whole numbers as integers ("100", not %g's "1e+02"); -0 as 0
        std::snprintf(buf, sizeof buf, "%.0f", v == 0.0 ? 0.0 : v);
        json_raw(o, buf);
        return;
    }
    std::snprintf(buf, sizeof buf, "%.17g", v);
    // %.17g round-trips; shorten when a shorter form reads back the same value.
    for (int digits = 1; digits < 17; digits++) {
        char shorter[40];
        std::snprintf(shorter, sizeof shorter, "%.*g", digits, v);
        if (std::strtod(shorter, nullptr) == v) {
            std::snprintf(buf, sizeof buf, "%s", shorter);
            break;
        }
    }
    json_raw(o, buf);
}

void json_out_free(JsonOut *o) {
    std::free(o->data);
    o->data = nullptr;
    o->size = o->cap = 0;
    o->failed = false;
}

} // namespace mt_stubs_impl
