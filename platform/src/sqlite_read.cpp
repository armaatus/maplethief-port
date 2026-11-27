// A read-only SQLite 3 table scanner (#374); see sqlite_read.h.
#include "sqlite_read.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <utility>

namespace maplethief::frontend {

namespace {

namespace fs = std::filesystem;
using u8 = std::uint8_t;
using u64 = std::uint64_t;

constexpr char kMagic[] = "SQLite format 3"; // and its NUL: 16 bytes
constexpr std::size_t kHeaderBytes = 100;
constexpr u8 kInteriorTable = 5, kLeafTable = 13;

u64 be(const u8 *p, std::size_t n) {
    u64 v = 0;
    for (std::size_t i = 0; i < n; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

bool fail(std::string *why, const std::string &what) {
    if (why != nullptr) {
        *why = what;
    }
    return false;
}

// A varint at p[*at] (at most 9 bytes, before `end`); false if it runs past `end`.
bool varint(const u8 *p, std::size_t end, std::size_t *at, u64 *out) {
    u64 v = 0;
    for (int i = 0; i < 9; i++) {
        if (*at >= end) {
            return false;
        }
        const u8 b = p[(*at)++];
        if (i == 8) {
            *out = (v << 8) | b;
            return true;
        }
        v = (v << 7) | (b & 0x7F);
        if ((b & 0x80) == 0) {
            *out = v;
            return true;
        }
    }
    return false;
}

// The bytes a serial type takes in a record's body.
u64 serial_size(u64 type) {
    static constexpr u8 kSizes[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
    if (type < 12) {
        return kSizes[type];
    }
    return (type - 12) / 2;
}

bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); i++) {
        const auto lower = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

std::string page_text(std::uint32_t number) { return "page " + std::to_string(number); }

} // namespace

bool sqlite_record(std::span<const u8> payload, SqliteRow *row) {
    row->clear();
    const u8 *p = payload.data();
    const std::size_t n = payload.size();
    std::size_t at = 0;
    u64 header = 0;
    if (!varint(p, n, &at, &header) || header < at || header > n) {
        return false;
    }
    std::vector<u64> types;
    while (at < header) {
        u64 t = 0;
        if (!varint(p, static_cast<std::size_t>(header), &at, &t) || t == 10 || t == 11) {
            return false;
        }
        types.push_back(t);
    }
    std::size_t body = static_cast<std::size_t>(header);
    for (const u64 t : types) {
        const u64 size = serial_size(t);
        if (size > n - body) {
            return false;
        }
        SqliteValue v;
        const u8 *b = p + body;
        if (t == 0) {
            v.type = SqliteValue::Type::kNull;
        } else if (t <= 6) {
            v.type = SqliteValue::Type::kInteger;
            u64 raw = be(b, static_cast<std::size_t>(size));
            const unsigned bits = static_cast<unsigned>(size) * 8;
            if (bits < 64 && (raw >> (bits - 1)) != 0) {
                raw |= ~u64{0} << bits; // sign-extend
            }
            v.integer = static_cast<std::int64_t>(raw);
        } else if (t == 7) {
            v.type = SqliteValue::Type::kFloat;
            const u64 raw = be(b, 8);
            std::memcpy(&v.real, &raw, sizeof v.real);
        } else if (t == 8 || t == 9) {
            v.type = SqliteValue::Type::kInteger;
            v.integer = t == 9 ? 1 : 0;
        } else {
            v.type = (t & 1) != 0 ? SqliteValue::Type::kText : SqliteValue::Type::kBlob;
            v.bytes.assign(reinterpret_cast<const char *>(b), static_cast<std::size_t>(size));
        }
        body += static_cast<std::size_t>(size);
        row->push_back(std::move(v));
    }
    return true;
}

std::vector<std::string> sqlite_create_columns(std::string_view sql) {
    const std::size_t open = sql.find('(');
    const std::size_t close = sql.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
        return {};
    }
    // The definitions between the outer parentheses, split at commas outside quotes and
    // parentheses.
    std::vector<std::string_view> parts;
    int depth = 0;
    char quote = 0;
    std::size_t start = open + 1;
    for (std::size_t i = open + 1; i < close; i++) {
        const char c = sql[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
        } else if (c == '"' || c == '\'' || c == '`') {
            quote = c;
        } else if (c == '[') {
            quote = ']';
        } else if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
        } else if (c == ',' && depth == 0) {
            parts.push_back(sql.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(sql.substr(start, close - start));
    std::vector<std::string> out;
    for (std::string_view part : parts) {
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t' ||
                                 part.front() == '\n' || part.front() == '\r')) {
            part.remove_prefix(1);
        }
        if (part.empty()) {
            continue;
        }
        std::string name;
        const char q = part.front();
        if (q == '"' || q == '`' || q == '[') {
            const char end = q == '[' ? ']' : q;
            const std::size_t e = part.find(end, 1);
            name = std::string(part.substr(1, e == std::string_view::npos ? e : e - 1));
        } else {
            std::size_t e = 0;
            while (e < part.size() && part[e] != ' ' && part[e] != '\t' && part[e] != '\n' &&
                   part[e] != '\r' && part[e] != '(') {
                e++;
            }
            name = std::string(part.substr(0, e));
            bool constraint = false;
            for (const char *word : {"CONSTRAINT", "PRIMARY", "UNIQUE", "CHECK", "FOREIGN"}) {
                constraint = constraint || same_name(name, word);
            }
            if (constraint) {
                continue;
            }
        }
        out.push_back(std::move(name));
    }
    return out;
}

bool SqliteReader::open(ReadAt read, std::uint64_t size, std::string *why) {
    read_ = std::move(read);
    size_ = size;
    page_size_ = usable_ = page_count_ = 0;
    u8 h[kHeaderBytes];
    if (size < kHeaderBytes || !read_(0, kHeaderBytes, h)) {
        return fail(why, "too short for an SQLite database");
    }
    if (std::memcmp(h, kMagic, sizeof kMagic) != 0) {
        return fail(why, "not an SQLite database (no \"SQLite format 3\" header)");
    }
    std::uint32_t ps = static_cast<std::uint32_t>(be(h + 16, 2));
    if (ps == 1) {
        ps = 65536;
    }
    if (ps < 512 || ps > 65536 || (ps & (ps - 1)) != 0) {
        return fail(why, "an SQLite page size of " + std::to_string(ps) + " is not valid");
    }
    const std::uint32_t reserved = h[20];
    if (ps - reserved < 480) {
        return fail(why, "an SQLite page leaves fewer than 480 usable bytes");
    }
    const u64 encoding = be(h + 56, 4);
    if (encoding > 1) {
        return fail(why, "the database's text is UTF-16, which is not read");
    }
    if (size < ps) {
        return fail(why, "the database is shorter than one page");
    }
    page_size_ = ps;
    usable_ = ps - reserved;
    const u64 pages = size / ps;
    page_count_ = static_cast<std::uint32_t>(pages > 0xFFFFFFFEu ? 0xFFFFFFFEu : pages);
    return true;
}

bool SqliteReader::open(std::span<const u8> bytes, std::string *why) {
    return open(
        [bytes](u64 offset, std::size_t n, u8 *out) {
            if (offset > bytes.size() || n > bytes.size() - offset) {
                return false;
            }
            std::memcpy(out, bytes.data() + offset, n);
            return true;
        },
        bytes.size(), why);
}

bool SqliteReader::open_file(const std::string &path, std::string *why) {
    const fs::path p(std::u8string(path.begin(), path.end()));
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(p, ec);
    if (ec) {
        return fail(why, "cannot read " + path + ": " + ec.message());
    }
    auto in = std::make_shared<std::ifstream>(p, std::ios::binary);
    if (!*in) {
        return fail(why, "cannot open " + path);
    }
    return open(
        [in](u64 offset, std::size_t n, u8 *out) {
            in->clear();
            in->seekg(static_cast<std::streamoff>(offset));
            in->read(reinterpret_cast<char *>(out), static_cast<std::streamsize>(n));
            return static_cast<std::size_t>(in->gcount()) == n;
        },
        size, why);
}

bool SqliteReader::page(std::uint32_t number, std::vector<u8> *out, std::string *why) const {
    if (number == 0 || number > page_count_) {
        return fail(why, "the database points to " + page_text(number) + ", past its " +
                             std::to_string(page_count_) + " pages");
    }
    out->resize(page_size_);
    if (!read_(u64{number - 1} * page_size_, page_size_, out->data())) {
        return fail(why, "cannot read the database's " + page_text(number));
    }
    return true;
}

// The payload of `size` bytes whose local part starts at page[at] (a table leaf cell), with
// its overflow chain.
bool SqliteReader::payload(const std::vector<u8> &pg, std::size_t at, std::uint32_t number,
                           std::uint64_t size, std::vector<u8> *out, std::string *why) const {
    if (size > kMaxRecordBytes) {
        return fail(why, "a record on " + page_text(number) + " has " + std::to_string(size) +
                             " bytes, over the limit of " + std::to_string(kMaxRecordBytes));
    }
    const u64 u = usable_;
    const u64 x = u - 35;
    u64 local = size;
    if (size > x) {
        const u64 m = ((u - 12) * 32 / 255) - 23;
        const u64 k = m + ((size - m) % (u - 4));
        local = k <= x ? k : m;
    }
    const bool spills = local < size;
    if (at > usable_ || local + (spills ? 4 : 0) > usable_ - at) {
        return fail(why, "a cell on " + page_text(number) + " runs past the page");
    }
    out->assign(pg.begin() + static_cast<std::ptrdiff_t>(at),
                pg.begin() + static_cast<std::ptrdiff_t>(at + local));
    if (!spills) {
        return true;
    }
    std::uint32_t next = static_cast<std::uint32_t>(be(pg.data() + at + local, 4));
    std::vector<u8> over;
    std::uint32_t hops = 0;
    while (out->size() < size) {
        if (next == 0 || ++hops > page_count_) {
            return fail(why, "a record's overflow chain from " + page_text(number) +
                                 " ends early or loops");
        }
        if (!page(next, &over, why)) {
            return false;
        }
        const u64 take = std::min<u64>(size - out->size(), u - 4);
        out->insert(out->end(), over.begin() + 4, over.begin() + 4 + static_cast<std::ptrdiff_t>(take));
        next = static_cast<std::uint32_t>(be(over.data(), 4));
    }
    return true;
}

bool SqliteReader::scan(std::uint32_t root,
                        const std::function<bool(std::int64_t, const SqliteRow &)> &row,
                        std::string *why) {
    if (page_size_ == 0) {
        return fail(why, "the database is not open");
    }
    std::vector<bool> seen(static_cast<std::size_t>(page_count_) + 1, false);
    bool stopped = false;
    std::vector<u8> cell;
    SqliteRow values;
    // Depth-first, children in key order; returns false on a broken tree.
    const std::function<bool(std::uint32_t, std::uint32_t)> walk = [&](std::uint32_t number,
                                                                       std::uint32_t depth) {
        if (depth > kMaxDepth) {
            return fail(why, "the table's tree is over " + std::to_string(kMaxDepth) +
                                 " levels deep at " + page_text(number));
        }
        if (number == 0 || number > page_count_) {
            return fail(why, "the database points to " + page_text(number) + ", past its " +
                                 std::to_string(page_count_) + " pages");
        }
        if (seen[number]) {
            return fail(why, "the table's tree reaches " + page_text(number) + " twice");
        }
        seen[number] = true;
        std::vector<u8> pg;
        if (!page(number, &pg, why)) {
            return false;
        }
        const std::size_t h = number == 1 ? kHeaderBytes : 0;
        const u8 type = pg[h];
        if (type != kInteriorTable && type != kLeafTable) {
            return fail(why, page_text(number) + " is not a table page (type " +
                                 std::to_string(type) + ")");
        }
        const std::size_t cells = static_cast<std::size_t>(be(pg.data() + h + 3, 2));
        const std::size_t array = h + (type == kLeafTable ? 8 : 12);
        if (array + 2 * cells > usable_) {
            return fail(why, page_text(number) + " lists more cells than it can hold");
        }
        for (std::size_t i = 0; i < cells && !stopped; i++) {
            std::size_t at = static_cast<std::size_t>(be(pg.data() + array + 2 * i, 2));
            if (at < array + 2 * cells || at >= usable_) {
                return fail(why, "a cell of " + page_text(number) + " points outside it");
            }
            if (type == kInteriorTable) {
                if (at + 4 > usable_) {
                    return fail(why, "a cell of " + page_text(number) + " runs past the page");
                }
                const auto child = static_cast<std::uint32_t>(be(pg.data() + at, 4));
                if (!walk(child, depth + 1)) {
                    return false;
                }
                continue;
            }
            u64 size = 0, rowid = 0;
            if (!varint(pg.data(), usable_, &at, &size) ||
                !varint(pg.data(), usable_, &at, &rowid)) {
                return fail(why, "a cell of " + page_text(number) + " runs past the page");
            }
            if (!payload(pg, at, number, size, &cell, why)) {
                return false;
            }
            if (!sqlite_record(cell, &values)) {
                return fail(why, "a record on " + page_text(number) + " is malformed");
            }
            if (!row(static_cast<std::int64_t>(rowid), values)) {
                stopped = true;
            }
        }
        if (type == kInteriorTable && !stopped) {
            return walk(static_cast<std::uint32_t>(be(pg.data() + h + 8, 4)), depth + 1);
        }
        return true;
    };
    return walk(root, 0);
}

bool SqliteReader::table(std::string_view name, std::uint32_t *root,
                         std::vector<std::string> *columns, std::string *why) {
    bool found = false;
    // sqlite_schema(type, name, tbl_name, rootpage, sql), rooted at page 1.
    const bool ok = scan(
        1,
        [&](std::int64_t, const SqliteRow &r) {
            if (r.size() < 5 || !r[0].is_text() || r[0].bytes != "table" || !r[1].is_text() ||
                !same_name(r[1].bytes, name)) {
                return true;
            }
            if (!r[3].is_integer() || r[3].integer < 1 || r[3].integer > page_count_ ||
                !r[4].is_text()) {
                return true; // not a table this reader can scan; reported below
            }
            *root = static_cast<std::uint32_t>(r[3].integer);
            *columns = sqlite_create_columns(r[4].bytes);
            found = true;
            return false;
        },
        why);
    if (!ok) {
        return false;
    }
    if (!found) {
        return fail(why, "the database has no table " + std::string(name));
    }
    return true;
}

} // namespace maplethief::frontend
