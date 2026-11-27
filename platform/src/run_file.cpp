// The run file (#405, ADR 0156); see run_file.h and docs/formats/mtrun.md.
#include "run_file.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "maplethief/snapshot.h"

namespace maplethief::frontend {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

constexpr std::size_t kHeaderSize = 8 + 4 + 4 + 8 + 8 + 4;

// The sections of the body, in their order.
constexpr const char *kSections[] = {"BIND", "SETS", "CHGS", "FILE", "TAPE",
                                     "EVTS", "SPLT", "CHKS", "END_"};

class Writer {
  public:
    void u8v(u8 v) { b_.push_back(v); }
    void u32v(u32 v) {
        for (int i = 0; i < 4; i++) {
            b_.push_back(static_cast<u8>(v >> (8 * i)));
        }
    }
    void u64v(u64 v) {
        for (int i = 0; i < 8; i++) {
            b_.push_back(static_cast<u8>(v >> (8 * i)));
        }
    }
    void var(u64 v) {
        while (v >= 0x80) {
            b_.push_back(static_cast<u8>(v | 0x80));
            v >>= 7;
        }
        b_.push_back(static_cast<u8>(v));
    }
    void svar(i64 v) { var((static_cast<u64>(v) << 1) ^ static_cast<u64>(v >> 63)); }
    void bytes(const u8 *p, std::size_t n) { b_.insert(b_.end(), p, p + n); }
    void text(const std::string &s) {
        var(s.size());
        bytes(reinterpret_cast<const u8 *>(s.data()), s.size());
    }
    // A section: its tag, its length and `w`'s bytes.
    void section(const char *tag, const Writer &w) {
        bytes(reinterpret_cast<const u8 *>(tag), 4);
        u32v(static_cast<u32>(w.b_.size()));
        bytes(w.b_.data(), w.b_.size());
    }
    const std::vector<u8> &data() const { return b_; }

  private:
    std::vector<u8> b_;
};

// Reads a range of the body; the first failure sticks, with its offset (from `base`).
class Reader {
  public:
    Reader(const u8 *p, std::size_t n, std::size_t base) : p_(p), n_(n), base_(base) {}

    bool ok() const { return why_.empty(); }
    const std::string &why() const { return why_; }
    std::size_t left() const { return n_ - at_; }
    std::size_t offset() const { return base_ + at_; }
    bool done() {
        if (ok() && at_ != n_) {
            fail("bytes after the section's last field");
        }
        return ok();
    }
    bool fail(const std::string &what) {
        if (ok()) {
            why_ = "body byte " + std::to_string(offset()) + ": " + what;
        }
        return false;
    }

    bool u8v(u8 *v) {
        if (!need(1)) {
            return false;
        }
        *v = p_[at_++];
        return true;
    }
    bool u32v(u32 *v) {
        if (!need(4)) {
            return false;
        }
        *v = 0;
        for (int i = 0; i < 4; i++) {
            *v |= static_cast<u32>(p_[at_++]) << (8 * i);
        }
        return true;
    }
    bool u64v(u64 *v) {
        if (!need(8)) {
            return false;
        }
        *v = 0;
        for (int i = 0; i < 8; i++) {
            *v |= static_cast<u64>(p_[at_++]) << (8 * i);
        }
        return true;
    }
    bool var(u64 *v) {
        *v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            u8 b = 0;
            if (!u8v(&b)) {
                return false;
            }
            if (shift == 63 && b > 1) {
                return fail("a number longer than 64 bits");
            }
            *v |= static_cast<u64>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) {
                return true;
            }
        }
        return fail("a number longer than 64 bits");
    }
    bool svar(i64 *v) {
        u64 z = 0;
        if (!var(&z)) {
            return false;
        }
        *v = static_cast<i64>((z >> 1) ^ (~(z & 1) + 1));
        return true;
    }
    // A count of items that take at least `min_bytes` each: no more than the bytes left allow.
    bool count(u64 *n, std::size_t min_bytes, const char *what) {
        if (!var(n)) {
            return false;
        }
        if (*n > left() / min_bytes) {
            return fail(std::string("more ") + what + " (" + std::to_string(*n) +
                        ") than the section holds");
        }
        return true;
    }
    bool bytes(std::vector<u8> *out, u64 n, u64 limit, const char *what) {
        if (n > limit) {
            return fail(std::string(what) + " of " + std::to_string(n) + " bytes, over the limit");
        }
        if (!need(n)) {
            return false;
        }
        out->assign(p_ + at_, p_ + at_ + n);
        at_ += static_cast<std::size_t>(n);
        return true;
    }
    bool text(std::string *s, u64 limit = kRunMaxText) {
        u64 n = 0;
        if (!var(&n)) {
            return false;
        }
        std::vector<u8> b;
        if (!bytes(&b, n, limit, "a string")) {
            return false;
        }
        s->assign(b.begin(), b.end());
        return true;
    }

  private:
    bool need(u64 n) {
        if (!ok()) {
            return false;
        }
        if (n > n_ - at_) {
            return fail("the section ends inside a field");
        }
        return true;
    }

    const u8 *p_;
    std::size_t n_;
    std::size_t base_;
    std::size_t at_ = 0;
    std::string why_;
};

void put_u32(std::vector<u8> &b, u32 v) {
    for (int i = 0; i < 4; i++) {
        b.push_back(static_cast<u8>(v >> (8 * i)));
    }
}
void put_u64(std::vector<u8> &b, u64 v) {
    for (int i = 0; i < 8; i++) {
        b.push_back(static_cast<u8>(v >> (8 * i)));
    }
}
u32 get_u32(const u8 *p) {
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
           static_cast<u32>(p[3]) << 24;
}
u64 get_u64(const u8 *p) { return get_u32(p) | static_cast<u64>(get_u32(p + 4)) << 32; }

bool event_fields_ok(const mt_runtape_event &e) {
    return e.kind >= MT_RUNTAPE_PRESS && e.kind <= MT_RUNTAPE_KIND_LAST;
}

void write_event(Writer &w, const mt_runtape_event &e) {
    w.u8v(static_cast<u8>(e.kind));
    switch (e.kind) {
    case MT_RUNTAPE_PRESS:
    case MT_RUNTAPE_RELEASE:
    case MT_RUNTAPE_LIFECYCLE:
        w.var(e.a);
        break;
    case MT_RUNTAPE_TOUCH: {
        w.var(e.a);
        w.u8v(static_cast<u8>(e.b));
        u32 x = 0, y = 0;
        std::memcpy(&x, &e.x, 4);
        std::memcpy(&y, &e.y, 4);
        w.u32v(x);
        w.u32v(y);
        break;
    }
    case MT_RUNTAPE_FIRE_TOLERANCE:
        w.u64v(e.value);
        break;
    case MT_RUNTAPE_POKE:
        w.u32v(e.a);
        w.u8v(static_cast<u8>(e.b));
        for (u32 i = 0; i < e.b && i < 8; i++) {
            w.u8v(static_cast<u8>(e.value >> (8 * i)));
        }
        break;
    default: // RELEASE_ALL
        break;
    }
}

bool read_event(Reader &r, mt_runtape_event *e) {
    u8 kind = 0;
    if (!r.u8v(&kind)) {
        return false;
    }
    e->kind = kind;
    if (!event_fields_ok(*e)) {
        return r.fail("an event of unknown kind " + std::to_string(kind));
    }
    u64 v = 0;
    switch (kind) {
    case MT_RUNTAPE_PRESS:
    case MT_RUNTAPE_RELEASE:
    case MT_RUNTAPE_LIFECYCLE:
        if (!r.var(&v)) {
            return false;
        }
        if (v > 0xFFFFFFFFu) {
            return r.fail("an event's value over 32 bits");
        }
        e->a = static_cast<u32>(v);
        return true;
    case MT_RUNTAPE_TOUCH: {
        u8 phase = 0;
        u32 x = 0, y = 0;
        if (!r.var(&v) || !r.u8v(&phase) || !r.u32v(&x) || !r.u32v(&y)) {
            return false;
        }
        if (v > 0xFFFFFFFFu || phase > 3) {
            return r.fail("a touch with a bad finger or phase");
        }
        e->a = static_cast<u32>(v);
        e->b = phase;
        std::memcpy(&e->x, &x, 4);
        std::memcpy(&e->y, &y, 4);
        return true;
    }
    case MT_RUNTAPE_FIRE_TOLERANCE:
        return r.u64v(&e->value);
    case MT_RUNTAPE_POKE: {
        u8 len = 0;
        if (!r.u32v(&e->a) || !r.u8v(&len)) {
            return false;
        }
        if (len == 0 || len > 8) {
            return r.fail("a memory write of " + std::to_string(len) + " bytes (1 to 8)");
        }
        e->b = len;
        e->value = 0;
        for (u32 i = 0; i < len; i++) {
            u8 b = 0;
            if (!r.u8v(&b)) {
                return false;
            }
            e->value |= static_cast<u64>(b) << (8 * i);
        }
        return true;
    }
    default: // RELEASE_ALL
        return true;
    }
}

bool read_body(const u8 *p, std::size_t n, RunFile *out, std::string *why) {
    std::size_t at = 0;
    for (const char *tag : kSections) {
        if (n - at < 8) {
            *why = "body byte " + std::to_string(at) + ": the body ends before section " + tag;
            return false;
        }
        if (std::memcmp(p + at, tag, 4) != 0) {
            *why = "body byte " + std::to_string(at) + ": section " + tag + " expected";
            return false;
        }
        const u32 len = get_u32(p + at + 4);
        if (len > n - at - 8) {
            *why = "body byte " + std::to_string(at + 4) + ": section " + tag + " of " +
                   std::to_string(len) + " bytes runs past the body";
            return false;
        }
        Reader r(p + at + 8, len, at + 8);
        const std::string t(tag);
        if (t == "BIND") {
            std::vector<u8> sha;
            if (r.bytes(&sha, 32, 32, "the hash")) {
                std::memcpy(out->binary_sha256.data(), sha.data(), 32);
            }
            r.text(&out->port_version, 256);
        } else if (t == "SETS") {
            r.text(&out->settings);
        } else if (t == "CHGS") {
            u64 count = 0;
            u64 turn = 0;
            if (r.count(&count, 3, "setting changes")) {
                out->changes.resize(count);
                for (RunSettingChange &c : out->changes) {
                    u64 d = 0;
                    if (!r.var(&d) || !r.text(&c.key, 64) || !r.text(&c.value, 4096)) {
                        break;
                    }
                    turn += d;
                    c.turn = turn;
                }
            }
        } else if (t == "FILE") {
            u64 count = 0;
            if (r.count(&count, 2, "files") && count > kRunMaxFiles) {
                r.fail(std::to_string(count) + " boot files, over the limit of " +
                       std::to_string(kRunMaxFiles));
            }
            if (r.ok()) {
                out->files.resize(count);
                for (RunBootFile &f : out->files) {
                    u64 size = 0;
                    if (!r.text(&f.name, 64) || !r.var(&size) ||
                        !r.bytes(&f.bytes, size, kRunMaxFileBytes, "a boot file")) {
                        break;
                    }
                    if (f.name.empty() || f.name.find_first_of("/\\:") != std::string::npos ||
                        f.name == "." || f.name == "..") {
                        r.fail("a boot file named '" + f.name + "' (a plain file name expected)");
                        break;
                    }
                }
            }
        } else if (t == "TAPE") {
            for (std::vector<mt_runtape_run> &s : out->streams) {
                u64 count = 0;
                if (!r.count(&count, 2, "runs")) {
                    break;
                }
                s.resize(count);
                i64 prev = 0;
                for (mt_runtape_run &run : s) {
                    i64 d = 0;
                    if (!r.svar(&d) || !r.var(&run.count)) {
                        break;
                    }
                    if (run.count == 0) {
                        r.fail("a run of no reads");
                        break;
                    }
                    prev = static_cast<i64>(static_cast<u64>(prev) + static_cast<u64>(d));
                    run.value = static_cast<u64>(prev);
                }
                if (!r.ok()) {
                    break;
                }
            }
        } else if (t == "EVTS") {
            u64 count = 0;
            u64 turn = 0;
            if (r.count(&count, 2, "events")) {
                out->events.resize(count);
                for (mt_runtape_event &e : out->events) {
                    u64 d = 0;
                    e = mt_runtape_event{};
                    if (!r.var(&d) || !read_event(r, &e)) {
                        break;
                    }
                    turn += d;
                    e.turn = turn;
                }
            }
        } else if (t == "SPLT") {
            u64 count = 0;
            if (r.count(&count, 2, "categories") && count > kRunMaxCategories) {
                r.fail(std::to_string(count) + " categories, over the limit of " +
                       std::to_string(kRunMaxCategories));
            }
            if (r.ok()) {
                out->categories.resize(count);
                for (RunFileCategory &c : out->categories) {
                    if (!r.text(&c.id, 64) || !r.text(&c.text, kRunMaxCategoryText)) {
                        break;
                    }
                    if (c.id.empty() || c.id.find_first_of("/\\:.") != std::string::npos) {
                        r.fail("a category named '" + c.id + "' (a plain name expected)");
                        break;
                    }
                }
            }
            if (r.ok() && r.count(&count, 4, "splits")) {
                out->splits.resize(count);
                for (RunFileSplit &s : out->splits) {
                    if (!r.text(&s.name, 256) || !r.var(&s.turn) || !r.var(&s.rta_us) ||
                        !r.var(&s.ticks)) {
                        break;
                    }
                }
            }
        } else if (t == "CHKS") {
            u64 count = 0;
            if (r.count(&count, 11, "checkpoints")) {
                out->checkpoints.resize(count);
                for (RunCheckpoint &c : out->checkpoints) {
                    if (!r.var(&c.turn) || !r.var(&c.tick) || !r.u64v(&c.digest) ||
                        !r.text(&c.state, 1024)) {
                        break;
                    }
                }
            }
        } else { // END_
            u64 reason = 0;
            if (r.var(&reason) && reason > static_cast<u64>(RunEnd::kFailed)) {
                r.fail("an end of unknown kind " + std::to_string(reason));
            }
            if (r.ok()) {
                out->end = static_cast<RunEnd>(reason);
                r.text(&out->end_detail, 1024);
                r.var(&out->turns);
            }
        }
        if (!r.done()) {
            *why = r.why();
            return false;
        }
        at += 8 + len;
    }
    if (at != n) {
        *why = "body byte " + std::to_string(at) + ": bytes after the last section";
        return false;
    }
    return true;
}

} // namespace

const char *run_end_name(RunEnd end) {
    switch (end) {
    case RunEnd::kQuit:
        return "quit";
    case RunEnd::kRefused:
        return "refused";
    case RunEnd::kFailed:
        return "failed";
    }
    return "?";
}

bool RunFile::operator==(const RunFile &o) const {
    const auto same_events = [](const std::vector<mt_runtape_event> &a,
                                const std::vector<mt_runtape_event> &b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); i++) {
            const mt_runtape_event &x = a[i], &y = b[i];
            if (x.turn != y.turn || x.kind != y.kind || x.a != y.a || x.b != y.b ||
                std::bit_cast<std::uint32_t>(x.x) != std::bit_cast<std::uint32_t>(y.x) ||
                std::bit_cast<std::uint32_t>(x.y) != std::bit_cast<std::uint32_t>(y.y) ||
                x.value != y.value) {
                return false;
            }
        }
        return true;
    };
    for (std::size_t s = 0; s < streams.size(); s++) {
        if (streams[s].size() != o.streams[s].size()) {
            return false;
        }
        for (std::size_t i = 0; i < streams[s].size(); i++) {
            if (streams[s][i].value != o.streams[s][i].value ||
                streams[s][i].count != o.streams[s][i].count) {
                return false;
            }
        }
    }
    return binary_sha256 == o.binary_sha256 && port_version == o.port_version &&
           settings == o.settings && changes == o.changes && files == o.files &&
           same_events(events, o.events) && categories == o.categories && splits == o.splits &&
           checkpoints == o.checkpoints && end == o.end && end_detail == o.end_detail &&
           turns == o.turns;
}

std::vector<std::uint8_t> write_run_file(const RunFile &run) {
    Writer body;
    {
        Writer w;
        w.bytes(run.binary_sha256.data(), 32);
        w.text(run.port_version);
        body.section("BIND", w);
    }
    {
        Writer w;
        w.text(run.settings);
        body.section("SETS", w);
    }
    {
        Writer w;
        w.var(run.changes.size());
        u64 turn = 0;
        for (const RunSettingChange &c : run.changes) {
            w.var(c.turn - turn);
            turn = c.turn;
            w.text(c.key);
            w.text(c.value);
        }
        body.section("CHGS", w);
    }
    {
        Writer w;
        w.var(run.files.size());
        for (const RunBootFile &f : run.files) {
            w.text(f.name);
            w.var(f.bytes.size());
            w.bytes(f.bytes.data(), f.bytes.size());
        }
        body.section("FILE", w);
    }
    {
        Writer w;
        for (const std::vector<mt_runtape_run> &s : run.streams) {
            w.var(s.size());
            u64 prev = 0;
            for (const mt_runtape_run &r : s) {
                w.svar(static_cast<i64>(r.value - prev));
                prev = r.value;
                w.var(r.count);
            }
        }
        body.section("TAPE", w);
    }
    {
        Writer w;
        w.var(run.events.size());
        u64 turn = 0;
        for (const mt_runtape_event &e : run.events) {
            w.var(e.turn - turn);
            turn = e.turn;
            write_event(w, e);
        }
        body.section("EVTS", w);
    }
    {
        Writer w;
        w.var(run.categories.size());
        for (const RunFileCategory &c : run.categories) {
            w.text(c.id);
            w.text(c.text);
        }
        w.var(run.splits.size());
        for (const RunFileSplit &s : run.splits) {
            w.text(s.name);
            w.var(s.turn);
            w.var(s.rta_us);
            w.var(s.ticks);
        }
        body.section("SPLT", w);
    }
    {
        Writer w;
        w.var(run.checkpoints.size());
        for (const RunCheckpoint &c : run.checkpoints) {
            w.var(c.turn);
            w.var(c.tick);
            w.u64v(c.digest);
            w.text(c.state);
        }
        body.section("CHKS", w);
    }
    {
        Writer w;
        w.var(static_cast<u64>(run.end));
        w.text(run.end_detail);
        w.var(run.turns);
        body.section("END_", w);
    }
    u8 *packed = nullptr;
    std::size_t packed_size = 0;
    if (mt_zlib_deflate(body.data().data(), body.data().size(), &packed, &packed_size) != 0) {
        return {};
    }
    std::vector<u8> out(kRunMagic, kRunMagic + 8);
    put_u32(out, kRunFormat);
    put_u32(out, 0); // flags
    put_u64(out, body.data().size());
    put_u64(out, packed_size);
    put_u32(out, mt_crc32(0, packed, packed_size));
    out.insert(out.end(), packed, packed + packed_size);
    std::free(packed);
    return out;
}

bool read_run_file(const std::uint8_t *bytes, std::size_t size, RunFile *out, std::string *why) {
    *out = RunFile{};
    if (size < kHeaderSize) {
        *why = "byte " + std::to_string(size) + ": the file ends inside its header (" +
               std::to_string(kHeaderSize) + " bytes)";
        return false;
    }
    if (std::memcmp(bytes, kRunMagic, 8) != 0) {
        *why = "byte 0: not a run file (no MTRUN magic)";
        return false;
    }
    const u32 format = get_u32(bytes + 8);
    if (format != kRunFormat) {
        *why = "byte 8: run file format " + std::to_string(format) + ", this build reads " +
               std::to_string(kRunFormat);
        return false;
    }
    if (get_u32(bytes + 12) != 0) {
        *why = "byte 12: unknown flags";
        return false;
    }
    const u64 body_size = get_u64(bytes + 16);
    if (body_size > kRunMaxBody) {
        *why = "byte 16: a body of " + std::to_string(body_size) + " bytes, over the limit";
        return false;
    }
    const u64 packed_size = get_u64(bytes + 24);
    if (packed_size != size - kHeaderSize) {
        *why = "byte 24: " + std::to_string(packed_size) + " packed bytes, the file holds " +
               std::to_string(size - kHeaderSize);
        return false;
    }
    if (mt_crc32(0, bytes + kHeaderSize, packed_size) != get_u32(bytes + 32)) {
        *why = "byte 32: the packed body's CRC-32 does not match";
        return false;
    }
    u8 *body = nullptr;
    if (mt_zlib_inflate(bytes + kHeaderSize, packed_size, body_size, &body) != 0) {
        *why = "byte " + std::to_string(kHeaderSize) + ": the body does not inflate to " +
               std::to_string(body_size) + " bytes";
        return false;
    }
    const bool ok = read_body(body, body_size, out, why);
    std::free(body);
    return ok;
}

std::string run_file_name(std::int64_t t) {
    const std::time_t seconds = static_cast<std::time_t>(t);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    char text[64];
    std::snprintf(text, sizeof text, "%04d%02d%02d-%02d%02d%02d.mtrun", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return text;
}

std::uint64_t run_tape_span_us(const RunFile &run) {
    const std::vector<mt_runtape_run> &s = run.streams[MT_RUNTAPE_CLOCK];
    if (s.empty()) {
        return 0;
    }
    u64 lo = s.front().value, hi = s.front().value;
    for (const mt_runtape_run &r : s) {
        lo = r.value < lo ? r.value : lo;
        hi = r.value > hi ? r.value : hi;
    }
    return hi - lo;
}

} // namespace maplethief::frontend
