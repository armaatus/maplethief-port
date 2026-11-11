// Commit-on-demand bookkeeping (commit_map.h).
//
// The host heap that the blocks come from is also where the bookkeeping's own nodes come from,
// and a commit may take its last bytes. So every allocation of the bookkeeping happens before
// the mapping it records, into maps of their own that are spliced in afterwards (std::map::merge
// moves nodes and allocates nothing): running out of memory part way fails the call cleanly.
#include "mem/commit_map.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <new>
#include <vector>

namespace mt::vm {

CommitMap::CommitMap(const CommitOps &ops, std::size_t page, std::size_t max_block) noexcept
    : ops_(ops), max_block_(max_block < page ? page : max_block / page * page) {}

bool CommitMap::commit(std::uintptr_t addr, std::size_t size) noexcept {
    if (size == 0u) {
        return true;
    }
    const std::uintptr_t end = addr + size;
    // The runs of pages in the range that no piece covers, cut to at most max_block_ each.
    struct Run {
        std::uintptr_t at;
        std::size_t size;
    };
    std::vector<Run> runs;
    std::map<std::uintptr_t, Piece> fresh;
    std::map<void *, unsigned> fresh_refs;
    try {
        const auto add_gap = [&](std::uintptr_t lo, std::uintptr_t hi) {
            for (std::uintptr_t a = lo; a < hi;) {
                const std::size_t n = std::min<std::size_t>(hi - a, max_block_);
                runs.push_back(Run{a, n});
                a += n;
            }
        };
        std::uintptr_t at = addr;
        auto it = pieces_.upper_bound(addr);
        if (it != pieces_.begin()) {
            const auto before = std::prev(it);
            at = std::max(at, before->first + before->second.size);
        }
        for (; at < end && it != pieces_.end() && it->first < end; ++it) {
            if (it->first > at) {
                add_gap(at, it->first);
            }
            at = std::max(at, it->first + it->second.size);
        }
        if (at < end) {
            add_gap(at, end);
        }
    } catch (const std::bad_alloc &) {
        return false;
    }

    // Map every run; on a refusal, undo the ones mapped so far.
    bool ok = true;
    for (const Run &r : runs) {
        void *block = ops_.alloc(ops_.user, r.size);
        if (block == nullptr) {
            ok = false;
            break;
        }
        const auto src = reinterpret_cast<std::uintptr_t>(block);
        try {
            fresh.emplace(r.at, Piece{r.size, src, block});
            fresh_refs.emplace(block, 1u);
        } catch (const std::bad_alloc &) {
            fresh.erase(r.at);
            ops_.free(ops_.user, block);
            ok = false;
            break;
        }
        if (!ops_.map(ops_.user, r.at, src, r.size)) {
            fresh.erase(r.at);
            fresh_refs.erase(block);
            ops_.free(ops_.user, block);
            ok = false;
            break;
        }
    }
    if (!ok) {
        for (const auto &f : fresh) {
            ops_.unmap(ops_.user, f.first, f.second.src, f.second.size);
            ops_.free(ops_.user, f.second.block);
        }
        return false;
    }
    for (const auto &f : fresh) {
        std::memset(reinterpret_cast<void *>(f.first), 0, f.second.size); // fresh reads zero
    }
    pieces_.merge(fresh);
    refs_.merge(fresh_refs);
    return true;
}

bool CommitMap::decommit(std::uintptr_t addr, std::size_t size) noexcept {
    const std::uintptr_t end = addr + size;
    auto it = pieces_.upper_bound(addr);
    if (it != pieces_.begin()) {
        --it;
    }
    while (it != pieces_.end() && it->first < end) {
        const std::uintptr_t ps = it->first;
        const Piece p = it->second;
        const std::uintptr_t pe = ps + p.size;
        if (pe <= addr) {
            ++it;
            continue;
        }
        const std::uintptr_t a = std::max(ps, addr);
        const std::uintptr_t b = std::min(pe, end);
        // The parts of the piece outside the range stay mapped, as pieces of their own.
        std::map<std::uintptr_t, Piece> rest;
        try {
            if (a > ps) {
                rest.emplace(ps, Piece{a - ps, p.src, p.block});
            }
            if (b < pe) {
                rest.emplace(b, Piece{pe - b, p.src + (b - ps), p.block});
            }
        } catch (const std::bad_alloc &) {
            return false;
        }
        if (!ops_.unmap(ops_.user, a, p.src + (a - ps), b - a)) {
            return false;
        }
        it = pieces_.erase(it);
        unsigned &refs = refs_[p.block]; // present: the piece held a reference
        refs = refs - 1u + static_cast<unsigned>(rest.size());
        pieces_.merge(rest); // what follows `it` is unchanged: a split after b ends the loop
        if (refs == 0u) {
            refs_.erase(p.block);
            ops_.free(ops_.user, p.block);
        }
    }
    return true;
}

bool CommitMap::is_committed(std::uintptr_t addr) const noexcept {
    auto it = pieces_.upper_bound(addr);
    if (it == pieces_.begin()) {
        return false;
    }
    --it;
    return addr - it->first < it->second.size;
}

std::size_t CommitMap::committed_bytes() const noexcept {
    std::size_t n = 0;
    for (const auto &p : pieces_) {
        n += p.second.size;
    }
    return n;
}

} // namespace mt::vm
