// Commit on demand for the Nintendo Switch's guest memory (runtime/src/mem/commit_map.h, ADR
// 0025's Horizon addendum, #236), on every desktop against a fake of Horizon's calls: heap blocks
// that hold garbage, and a "map" that moves a block's bytes into the reservation and leaves the
// block unreadable (filled with 0xCD) until it is unmapped, as svcMapMemory does. The real calls
// run in MapleThiefTests.nro on a console.
#include "mem/commit_map.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace {

constexpr std::size_t k_page = 0x1000;

struct Fake {
    std::vector<std::uint8_t> space;         // the reservation
    std::vector<std::uintptr_t> mapped_from; // per page of it: the heap address mapped, or 0
    std::map<std::uintptr_t, std::pair<std::unique_ptr<std::uint8_t[]>, std::size_t>> blocks;
    int allocs = 0, frees = 0, maps = 0, unmaps = 0;
    int fail_alloc_at = -1, fail_map_at = -1; // the n-th call (0-based) is refused
    bool error = false;                       // a call the real kernel would refuse

    explicit Fake(std::size_t size)
        : space(size + k_page), mapped_from((size + k_page) / k_page, 0u) {}

    std::uintptr_t base() const {
        const auto p = reinterpret_cast<std::uintptr_t>(space.data());
        return (p + k_page - 1u) & ~static_cast<std::uintptr_t>(k_page - 1u);
    }
    std::size_t page_of(std::uintptr_t a) const { return (a - base()) / k_page; }
    std::uint8_t *at(std::size_t off) { return reinterpret_cast<std::uint8_t *>(base() + off); }

    mt::vm::CommitOps ops() {
        mt::vm::CommitOps o;
        o.user = this;
        o.alloc = [](void *u, std::size_t size) -> void * {
            auto *f = static_cast<Fake *>(u);
            if (f->allocs++ == f->fail_alloc_at) {
                return nullptr;
            }
            auto raw = std::make_unique<std::uint8_t[]>(size + k_page);
            const auto p = (reinterpret_cast<std::uintptr_t>(raw.get()) + k_page - 1u) &
                           ~static_cast<std::uintptr_t>(k_page - 1u);
            std::memset(reinterpret_cast<void *>(p), 0xAB, size); // the heap's old bytes
            f->blocks.emplace(p, std::make_pair(std::move(raw), size));
            return reinterpret_cast<void *>(p);
        };
        o.free = [](void *u, void *block) {
            auto *f = static_cast<Fake *>(u);
            f->frees++;
            const auto p = reinterpret_cast<std::uintptr_t>(block);
            const auto it = f->blocks.find(p);
            if (it == f->blocks.end()) {
                f->error = true;
                return;
            }
            for (std::uintptr_t src : f->mapped_from) { // freed while still mapped?
                if (src >= p && src < p + it->second.second) {
                    f->error = true;
                }
            }
            f->blocks.erase(it);
        };
        o.map = [](void *u, std::uintptr_t dst, std::uintptr_t src, std::size_t size) {
            auto *f = static_cast<Fake *>(u);
            if (f->maps++ == f->fail_map_at) {
                return false;
            }
            if (dst % k_page != 0u || src % k_page != 0u || size % k_page != 0u) {
                f->error = true;
                return false;
            }
            for (std::size_t i = 0; i < size / k_page; i++) {
                std::uintptr_t &slot = f->mapped_from[f->page_of(dst) + i];
                if (slot != 0u) {
                    f->error = true; // mapped twice
                }
                slot = src + i * k_page;
            }
            std::memcpy(reinterpret_cast<void *>(dst), reinterpret_cast<void *>(src), size);
            std::memset(reinterpret_cast<void *>(src), 0xCD, size);
            return true;
        };
        o.unmap = [](void *u, std::uintptr_t dst, std::uintptr_t src, std::size_t size) {
            auto *f = static_cast<Fake *>(u);
            f->unmaps++;
            for (std::size_t i = 0; i < size / k_page; i++) {
                std::uintptr_t &slot = f->mapped_from[f->page_of(dst) + i];
                if (slot != src + i * k_page) {
                    f->error = true; // not mapped from there
                }
                slot = 0u;
            }
            std::memcpy(reinterpret_cast<void *>(src), reinterpret_cast<void *>(dst), size);
            std::memset(reinterpret_cast<void *>(dst), 0xEE, size); // unmapped: not ours
            return true;
        };
        return o;
    }
};

bool all(const std::uint8_t *p, std::size_t n, std::uint8_t v) {
    for (std::size_t i = 0; i < n; i++) {
        if (p[i] != v) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("commit map: fresh pages read zero; a second commit maps only the gaps") {
    Fake f(64 * k_page);
    mt::vm::CommitMap m(f.ops(), k_page, 16 * k_page);
    const std::uintptr_t b = f.base();
    REQUIRE(m.commit(b + 4 * k_page, 4 * k_page));
    CHECK(all(f.at(4 * k_page), 4 * k_page, 0u)); // not the heap block's 0xAB
    CHECK(m.is_committed(b + 4 * k_page));
    CHECK_FALSE(m.is_committed(b + 3 * k_page));
    CHECK_FALSE(m.is_committed(b + 8 * k_page));
    std::memset(f.at(4 * k_page), 0x11, 4 * k_page);

    // Overlapping both sides: the old pages keep their bytes, the new ones read zero.
    REQUIRE(m.commit(b + 2 * k_page, 8 * k_page));
    CHECK(all(f.at(2 * k_page), 2 * k_page, 0u));
    CHECK(all(f.at(4 * k_page), 4 * k_page, 0x11u));
    CHECK(all(f.at(8 * k_page), 2 * k_page, 0u));
    CHECK(m.committed_bytes() == 8 * k_page);
    CHECK(m.pieces() == 3u);
    CHECK(m.blocks() == 3u);
    REQUIRE(m.commit(b + 4 * k_page, 2 * k_page)); // all committed: nothing to do
    CHECK(f.maps == 3);
    CHECK_FALSE(f.error);
}

TEST_CASE("commit map: 64 MiB committed, written, decommitted and committed again reads zero") {
    constexpr std::size_t k_size = std::size_t{64} << 20;
    Fake f(k_size);
    mt::vm::CommitMap m(f.ops(), k_page, std::size_t{8} << 20);
    const std::uintptr_t b = f.base();
    for (int round = 0; round < 3; round++) {
        REQUIRE(m.commit(b, k_size));
        CHECK(m.blocks() == 8u); // cut into blocks of at most 8 MiB
        CHECK(all(f.at(0), k_size, 0u));
        std::memset(f.at(0), 0xA5, k_size);
        REQUIRE(m.decommit(b, k_size));
        CHECK(m.committed_bytes() == 0u);
        CHECK(m.blocks() == 0u);
        CHECK(f.blocks.empty()); // every heap block given back
    }
    CHECK(f.allocs == f.frees);
    CHECK_FALSE(f.error);
}

TEST_CASE("commit map: a partial decommit keeps the block until all of its pages are gone") {
    Fake f(32 * k_page);
    mt::vm::CommitMap m(f.ops(), k_page, 32 * k_page);
    const std::uintptr_t b = f.base();
    REQUIRE(m.commit(b, 8 * k_page));
    std::memset(f.at(0), 0x22, 8 * k_page);
    REQUIRE(m.decommit(b + 3 * k_page, 2 * k_page)); // the middle
    CHECK(m.pieces() == 2u);
    CHECK(m.blocks() == 1u);
    CHECK(f.frees == 0);
    CHECK(all(f.at(0), 3 * k_page, 0x22u));
    CHECK(all(f.at(5 * k_page), 3 * k_page, 0x22u));
    CHECK_FALSE(m.is_committed(b + 3 * k_page));
    REQUIRE(m.commit(b + 3 * k_page, k_page)); // a new block for the hole, zeroed
    CHECK(all(f.at(3 * k_page), k_page, 0u));
    REQUIRE(m.decommit(b, 8 * k_page)); // uncommitted pages in the range are skipped
    CHECK(m.pieces() == 0u);
    CHECK(f.blocks.empty());
    CHECK(f.allocs == f.frees);
    CHECK_FALSE(f.error);
}

TEST_CASE("commit map: a refused allocation or mapping part way changes nothing") {
    Fake f(64 * k_page);
    mt::vm::CommitMap m(f.ops(), k_page, 4 * k_page);
    const std::uintptr_t b = f.base();
    f.fail_alloc_at = 2; // the third block of four
    CHECK_FALSE(m.commit(b, 16 * k_page));
    CHECK(m.pieces() == 0u);
    CHECK(f.blocks.empty());
    f.fail_map_at = f.maps + 1; // the second mapping of the next commit
    f.fail_alloc_at = -1;
    CHECK_FALSE(m.commit(b, 16 * k_page));
    CHECK(m.pieces() == 0u);
    CHECK(f.blocks.empty());
    for (std::uintptr_t src : f.mapped_from) {
        CHECK(src == 0u);
    }
    f.fail_map_at = -1;
    REQUIRE(m.commit(b, 16 * k_page)); // and then it works
    CHECK(m.blocks() == 4u);
    REQUIRE(m.decommit(b, 16 * k_page));
    CHECK(f.allocs == f.frees + 1); // the refused allocation returned nothing to free
    CHECK_FALSE(f.error);
}
