// The run file, .mtrun (#405, ADR 0156, platform/src/run_file.h, docs/formats/mtrun.md) on
// synthetic runs: the round trip, malformed files refused with the byte where they go wrong,
// and mutations of a good file (every byte flipped, every length cut, random bytes) refused or
// read without a crash. The real runs are recorded and replayed locally (MapleThief
// --record-runs on, --play-run; real assets).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "maplethief/snapshot.h"
#include "run_census.h"
#include "run_file.h"

namespace {

namespace fe = maplethief::frontend;

mt_runtape_event event(std::uint64_t turn, std::uint32_t kind, std::uint32_t a = 0,
                       std::uint32_t b = 0) {
    mt_runtape_event e{};
    e.turn = turn;
    e.kind = kind;
    e.a = a;
    e.b = b;
    return e;
}

fe::RunFile sample() {
    fe::RunFile r;
    for (std::size_t i = 0; i < r.binary_sha256.size(); i++) {
        r.binary_sha256[i] = static_cast<std::uint8_t>(i * 7 + 1);
    }
    r.port_version = "1.2.3";
    r.settings = "preset = custom\nhd_text = on\n";
    r.changes = {{120, "game_speed", "40"}, {120, "hd_text", "off"}, {5000, "flashes", "50"}};
    r.files = {{"config", {1, 2, 3}}, {"theif0", std::vector<std::uint8_t>(1124, 0x5A)}};
    // A clock that moves 1 ms every 20 reads, a GCD clock that moves every read, the music's.
    std::uint64_t us = 1735689600ull * 1000000ull;
    for (int i = 0; i < 300; i++, us += 1000) {
        r.streams[MT_RUNTAPE_CLOCK].push_back({us, 20});
    }
    r.streams[MT_RUNTAPE_CLOCK].push_back({us - 5, 1}); // a step back is a negative delta
    for (std::uint64_t ns = 1000000000ull; ns < 1000000000ull + 50 * 333; ns += 333) {
        r.streams[MT_RUNTAPE_GCD].push_back({ns, 1});
    }
    r.streams[MT_RUNTAPE_MUSIC] = {{1, 400}, {0, 2}, {1, 7}};
    r.events = {event(0, MT_RUNTAPE_FIRE_TOLERANCE), event(3, MT_RUNTAPE_PRESS, 5),
                event(3, MT_RUNTAPE_RELEASE, 5),     event(9, MT_RUNTAPE_RELEASE_ALL),
                event(9, MT_RUNTAPE_LIFECYCLE, 2),   event(44, MT_RUNTAPE_TOUCH, 0x1001, 3)};
    double tol = 0.0005;
    std::memcpy(&r.events[0].value, &tol, sizeof tol);
    r.events[5].x = 240.5f;
    r.events[5].y = -3.25f;
    mt_runtape_event poke = event(50, MT_RUNTAPE_POKE, 0x11A478, 8);
    poke.value = 0x0102030405060708ull;
    r.events.push_back(poke);
    mt_runtape_event poke1 = event(50, MT_RUNTAPE_POKE, 0x1000, 1);
    poke1.value = 0xAB;
    r.events.push_back(poke1);
    r.categories = {{"chapter_1", "title = Chapter 1\nsplit chapter_end 1 = Orbis\n"}};
    r.splits = {{"Chapter 1", 900, 61000000, 1200}};
    r.checkpoints = {{1074, 1000, 0xaf08dab41ff8995cull, "ticks=1000 mode=0 map=2.8"},
                     {3604, 2392, 0x0d0a5a6b0779d1dcull, "ticks=2392 mode=0 map=2.8"}};
    r.end = fe::RunEnd::kRefused;
    r.end_detail = "a save state's load";
    r.turns = 3604;
    return r;
}

std::string read_error(const std::vector<std::uint8_t> &bytes) {
    fe::RunFile out;
    std::string why;
    return fe::read_run_file(bytes.data(), bytes.size(), &out, &why) ? std::string() : why;
}

// The file's bytes with its body replaced by `body` (deflated, the header's sizes and CRC set).
std::vector<std::uint8_t> with_body(const std::vector<std::uint8_t> &body) {
    std::uint8_t *packed = nullptr;
    std::size_t n = 0;
    REQUIRE(mt_zlib_deflate(body.data(), body.size(), &packed, &n) == 0);
    std::vector<std::uint8_t> out(fe::kRunMagic, fe::kRunMagic + 8);
    const auto u32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; i++) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    const auto u64 = [&](std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v));
        u32(static_cast<std::uint32_t>(v >> 32));
    };
    u32(fe::kRunFormat);
    u32(0);
    u64(body.size());
    u64(n);
    u32(mt_crc32(0, packed, n));
    out.insert(out.end(), packed, packed + n);
    std::free(packed);
    return out;
}

// The inflated body of a good file.
std::vector<std::uint8_t> body_of(const std::vector<std::uint8_t> &file) {
    std::uint64_t size = 0;
    for (int i = 0; i < 8; i++) {
        size |= static_cast<std::uint64_t>(file[16 + i]) << (8 * i);
    }
    std::uint8_t *raw = nullptr;
    REQUIRE(mt_zlib_inflate(file.data() + 36, file.size() - 36, size, &raw) == 0);
    std::vector<std::uint8_t> body(raw, raw + size);
    std::free(raw);
    return body;
}

// A minimal body: every section empty but the ones that need fields.
std::vector<std::uint8_t> minimal_body() { return body_of(fe::write_run_file(fe::RunFile{})); }

} // namespace

TEST_CASE("run_file: a run reads back as it was written") {
    const fe::RunFile r = sample();
    const std::vector<std::uint8_t> bytes = fe::write_run_file(r);
    REQUIRE(!bytes.empty());
    CHECK(std::memcmp(bytes.data(), "MTRUN\0\r\n", 8) == 0);
    fe::RunFile back;
    std::string why;
    REQUIRE_MESSAGE(fe::read_run_file(bytes.data(), bytes.size(), &back, &why), why);
    CHECK(back == r);
    // An empty run too.
    const std::vector<std::uint8_t> empty = fe::write_run_file(fe::RunFile{});
    REQUIRE(fe::read_run_file(empty.data(), empty.size(), &back, &why));
    CHECK(back == fe::RunFile{});
}

TEST_CASE("run_file: run-length coding and deflate keep a display-paced tape small") {
    // An hour at 60 Hz, locked: the guest time moves at each refresh, read ~30 times between.
    fe::RunFile r;
    std::uint64_t us = 1735689600ull * 1000000ull;
    for (int i = 0; i < 60 * 3600; i++, us += 16667) {
        r.streams[MT_RUNTAPE_CLOCK].push_back({us, 30});
    }
    const std::vector<std::uint8_t> bytes = fe::write_run_file(r);
    CHECK(bytes.size() < 64 * 1024); // the refresh period repeats: deflate takes it
    CHECK(fe::run_tape_span_us(r) == 16667ull * (60 * 3600 - 1));
}

TEST_CASE("run_file: a malformed header is refused with its byte") {
    const std::vector<std::uint8_t> good = fe::write_run_file(sample());
    CHECK(read_error({}) == "byte 0: the file ends inside its header (36 bytes)");
    CHECK(read_error(std::vector<std::uint8_t>(good.begin(), good.begin() + 20)) ==
          "byte 20: the file ends inside its header (36 bytes)");
    std::vector<std::uint8_t> b = good;
    b[0] = 'X';
    CHECK(read_error(b) == "byte 0: not a run file (no MTRUN magic)");
    b = good;
    b[8] = 2;
    CHECK(read_error(b) == "byte 8: run file format 2, this build reads 1");
    b = good;
    b[12] = 1;
    CHECK(read_error(b) == "byte 12: unknown flags");
    b = good;
    b[23] = 0x40; // a body of 2^62 bytes
    CHECK(read_error(b).rfind("byte 16: a body of ", 0) == 0);
    b = good;
    b.push_back(0);
    CHECK(read_error(b).rfind("byte 24: ", 0) == 0);
    b = good;
    b.back() ^= 1;
    CHECK(read_error(b) == "byte 32: the packed body's CRC-32 does not match");
    b = good;
    b[16] ^= 1; // the body's length is one off: the packed body inflates to another
    CHECK(read_error(b) == "byte 36: the body does not inflate to " +
                               std::to_string(body_of(good).size() ^ 1) + " bytes");
}

TEST_CASE("run_file: a malformed body is refused with its byte in the body") {
    const std::vector<std::uint8_t> body = minimal_body();
    // The minimal body: BIND (32 + 1), SETS (1), CHGS (1), FILE (1), TAPE (3), EVTS (1), SPLT
    // (2: no category, no split), CHKS (1), END_ (3).
    REQUIRE(body.size() == 9 * 8 + 33 + 1 + 1 + 1 + 3 + 1 + 2 + 1 + 3);
    CHECK(read_error(with_body(body)).empty());

    std::vector<std::uint8_t> b = body;
    b[0] = 'X';
    CHECK(read_error(with_body(b)) == "body byte 0: section BIND expected");
    b = body;
    b.resize(b.size() - 3);
    CHECK(read_error(with_body(b)) == "body byte " + std::to_string(body.size() - 7) +
                                          ": section END_ of 3 bytes runs past "
                                          "the body");
    b = body;
    b.push_back(0);
    CHECK(read_error(with_body(b)) ==
          "body byte " + std::to_string(body.size()) + ": bytes after the last section");
    // Sections out of order.
    b = body;
    std::memcpy(b.data() + 41, "CHGS", 4);
    CHECK(read_error(with_body(b)) == "body byte 41: section SETS expected");

    // A field that runs past its section: SETS's string says 5 bytes, the section has none.
    const std::size_t sets = 8 + 33; // SETS's tag
    b = body;
    b[sets + 8] = 5;
    CHECK(read_error(with_body(b)) ==
          "body byte " + std::to_string(sets + 9) + ": the section ends inside a field");

    // TAPE: a count larger than the section can hold, then a run of no reads.
    const std::size_t tape = sets + 9 + 9 + 9; // after SETS, CHGS, FILE
    b = body;
    b[tape + 8] = 0x7F;
    CHECK(read_error(with_body(b)) ==
          "body byte " + std::to_string(tape + 9) + ": more runs (127) than the section holds");
    {
        fe::RunFile r;
        r.streams[MT_RUNTAPE_CLOCK] = {{5, 1}};
        std::vector<std::uint8_t> t = body_of(fe::write_run_file(r));
        // TAPE's first run: count 1 at tape + 8 + 2 (after its count and the value's delta).
        REQUIRE(t[tape + 10] == 1);
        t[tape + 10] = 0;
        CHECK(read_error(with_body(t)) ==
              "body byte " + std::to_string(tape + 11) + ": a run of no reads");
    }
    // EVTS: an unknown kind, a touch's phase, a write of 9 bytes.
    {
        fe::RunFile r;
        r.events = {event(1, MT_RUNTAPE_PRESS, 3)};
        std::vector<std::uint8_t> t = body_of(fe::write_run_file(r));
        const std::size_t evts = tape + 11;
        REQUIRE(std::memcmp(t.data() + evts, "EVTS", 4) == 0);
        t[evts + 10] = 9; // the kind, after the count and the turn's delta
        CHECK(read_error(with_body(t)) ==
              "body byte " + std::to_string(evts + 11) + ": an event of unknown kind 9");
        r.events = {event(1, MT_RUNTAPE_TOUCH, 1, 1)};
        t = body_of(fe::write_run_file(r));
        t[evts + 12] = 4; // the phase
        CHECK(read_error(with_body(t)) ==
              "body byte " + std::to_string(evts + 21) + ": a touch with a bad finger or phase");
        r.events = {event(1, MT_RUNTAPE_POKE, 0x1000, 2)};
        t = body_of(fe::write_run_file(r));
        t[evts + 15] = 9; // the length
        CHECK(read_error(with_body(t)) ==
              "body byte " + std::to_string(evts + 16) + ": a memory write of 9 bytes (1 to 8)");
    }
    // FILE: a name with a path in it, too many files.
    {
        fe::RunFile r;
        r.files = {{"theif0", {1}}};
        std::vector<std::uint8_t> t = body_of(fe::write_run_file(r));
        const std::size_t file = sets + 9 + 9;
        REQUIRE(std::memcmp(t.data() + file, "FILE", 4) == 0);
        t[file + 10] = '/'; // the name's first byte
        CHECK(read_error(with_body(t)) ==
              "body byte " + std::to_string(file + 18) +
                  ": a boot file named '/heif0' (a plain file name expected)");
        r.files.assign(fe::kRunMaxFiles + 1, {"config", {}});
        t = body_of(fe::write_run_file(r));
        CHECK(read_error(with_body(t)) ==
              "body byte " + std::to_string(file + 9) + ": 17 boot files, over the limit of 16");
    }
    // SPLT (#408): a category named with a path, too many categories.
    {
        fe::RunFile r;
        r.categories = {{"any", "title = Any%\n"}};
        std::vector<std::uint8_t> t = body_of(fe::write_run_file(r));
        const std::size_t splt = tape + 11 + 9; // after EVTS (1 byte)
        REQUIRE(std::memcmp(t.data() + splt, "SPLT", 4) == 0);
        // The id's first byte, after the count and its length; refused once id and text read.
        t[splt + 10] = '.';
        CHECK(read_error(with_body(t)) == "body byte " + std::to_string(splt + 27) +
                                              ": a category named '.ny' (a plain name expected)");
        r.categories.assign(fe::kRunMaxCategories + 1, {"any", ""});
        t = body_of(fe::write_run_file(r));
        CHECK(read_error(with_body(t)) ==
              "body byte " + std::to_string(splt + 9) + ": 17 categories, over the limit of 16");
    }
    // END_: an unknown end, a number longer than 64 bits.
    b = body;
    const std::size_t end = body.size() - 11;
    REQUIRE(std::memcmp(b.data() + end, "END_", 4) == 0);
    b[end + 8] = 3;
    CHECK(read_error(with_body(b)) ==
          "body byte " + std::to_string(end + 9) + ": an end of unknown kind 3");
    b = body;
    b.resize(end);
    const std::uint8_t long_var[] = {'E',  'N',  'D',  '_',  11,   0,    0,    0,    0xFF, 0xFF,
                                     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0,    0};
    b.insert(b.end(), long_var, long_var + sizeof long_var);
    CHECK(read_error(with_body(b)) ==
          "body byte " + std::to_string(end + 18) + ": a number longer than 64 bits");
}

TEST_CASE("run_file: mutations of a good file are refused or read, never crash") {
    const std::vector<std::uint8_t> good = fe::write_run_file(sample());
    const std::vector<std::uint8_t> body = body_of(good);
    std::mt19937 rng(405);
    std::size_t refused = 0, read = 0;
    // Every byte of the body flipped, through a file with a good CRC (so the body is parsed).
    for (std::size_t i = 0; i < body.size(); i++) {
        std::vector<std::uint8_t> b = body;
        b[i] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
        (read_error(with_body(b)).empty() ? read : refused)++;
    }
    // Every cut of the body.
    for (std::size_t n = 1; n < body.size(); n += 3) {
        const std::vector<std::uint8_t> b(body.begin(), body.begin() + static_cast<long>(n));
        const std::string why = read_error(with_body(b));
        CHECK_MESSAGE(why.rfind("body byte ", 0) == 0, n);
        refused++;
    }
    // Random bodies, and random bytes after a good header.
    for (int k = 0; k < 300; k++) {
        std::vector<std::uint8_t> b(rng() % 400);
        for (std::uint8_t &x : b) {
            x = static_cast<std::uint8_t>(rng());
        }
        (read_error(with_body(b)).empty() ? read : refused)++;
        std::vector<std::uint8_t> f(good.begin(), good.begin() + 36);
        f.insert(f.end(), b.begin(), b.end());
        CHECK(!read_error(f).empty());
    }
    CHECK(refused > 0);
    MESSAGE("mutations: " << refused << " refused, " << read << " read");
}

TEST_CASE("run_file: names and ends") {
    CHECK(fe::run_file_name(0) == "19700101-000000.mtrun");
    CHECK(fe::run_file_name(1795725457) == "20261126-203737.mtrun");
    CHECK(std::string(fe::run_end_name(fe::RunEnd::kQuit)) == "quit");
    CHECK(std::string(fe::run_end_name(fe::RunEnd::kRefused)) == "refused");
    CHECK(std::string(fe::run_end_name(fe::RunEnd::kFailed)) == "failed");
}

TEST_CASE("run_census: every site is named once, and every refused one has its name") {
    const auto sites = fe::run_census();
    REQUIRE(!sites.empty());
    for (std::size_t i = 0; i < sites.size(); i++) {
        CAPTURE(sites[i].file);
        CAPTURE(sites[i].call);
        CHECK((sites[i].kind == fe::CensusKind::kRefused) == (sites[i].refuse != nullptr));
        CHECK(sites[i].why != nullptr);
        for (std::size_t j = i + 1; j < sites.size(); j++) {
            CHECK(!(std::string(sites[i].file) == sites[j].file &&
                    std::string(sites[i].call) == sites[j].call));
        }
    }
    CHECK(std::string(fe::census_kind_name(fe::CensusKind::kPure)) == "pure");
}
