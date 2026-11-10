// Commit-on-demand bookkeeping for a host without page protection on reserved memory: the
// Nintendo Switch (ADR 0025's Horizon addendum, #236). Plain C++ on every host, so that the
// desktop tests run it against a fake backend; on Horizon, vm.cpp drives it with libnx.
//
// A reservation is address space that nothing is mapped into. Committing a range maps memory
// into the pages of it that are not mapped yet: for each run of such pages (cut into pieces of
// at most `max_block` bytes) one block is taken from the host's heap (`alloc`, page-aligned)
// and mapped at the range (`map`: svcMapMemory on Horizon, which makes the heap block itself
// inaccessible until it is unmapped). The pages are then zeroed, so that fresh memory reads as
// zero whatever the heap block held. Pages already committed keep their bytes. Decommitting
// unmaps the committed pages of a range (`unmap`) and gives each heap block back (`free`) once
// none of its pages is mapped any more; a piece cut in two by a partial decommit keeps its
// block until both halves are gone. A commit that fails part way undoes what it mapped, and
// the range is as it was.
//
// Not thread-safe: the caller locks (vm.cpp holds one mutex around every call).
#ifndef MAPLETHIEF_RUNTIME_MEM_COMMIT_MAP_H
#define MAPLETHIEF_RUNTIME_MEM_COMMIT_MAP_H

#include <cstddef>
#include <cstdint>
#include <map>

namespace mt::vm {

// The host calls behind the bookkeeping: a seam, so that tests can fake them.
struct CommitOps {
    void *user = nullptr;
    // `size` bytes from the host heap, aligned to the page size; nullptr if there are none.
    void *(*alloc)(void *user, std::size_t size) = nullptr;
    void (*free)(void *user, void *block) = nullptr;
    // Maps (unmaps) the `size` bytes at host address `src` to (from) host address `dst`.
    bool (*map)(void *user, std::uintptr_t dst, std::uintptr_t src, std::size_t size) = nullptr;
    bool (*unmap)(void *user, std::uintptr_t dst, std::uintptr_t src, std::size_t size) = nullptr;
};

class CommitMap {
  public:
    CommitMap(const CommitOps &ops, std::size_t page, std::size_t max_block) noexcept;

    // Commits [addr, addr + size) (page-aligned): maps and zeroes the pages not committed yet.
    // False, with nothing changed, if the heap or a mapping refuses.
    bool commit(std::uintptr_t addr, std::size_t size) noexcept;

    // Decommits the committed pages in [addr, addr + size); pages not committed are skipped.
    // False if an unmapping is refused (what was unmapped before it stays unmapped).
    bool decommit(std::uintptr_t addr, std::size_t size) noexcept;

    // Whether the page at `addr` is committed.
    bool is_committed(std::uintptr_t addr) const noexcept;

    // Committed bytes, mapped pieces and live heap blocks (for tests and diagnostics).
    std::size_t committed_bytes() const noexcept;
    std::size_t pieces() const noexcept { return pieces_.size(); }
    std::size_t blocks() const noexcept { return refs_.size(); }

  private:
    struct Piece {
        std::size_t size;
        std::uintptr_t src; // the heap address mapped at the piece's start
        void *block;        // the heap block it belongs to
    };

    CommitOps ops_;
    std::size_t max_block_;                  // a multiple of the page size
    std::map<std::uintptr_t, Piece> pieces_; // by host address
    std::map<void *, unsigned> refs_;        // heap block -> pieces still mapped from it
};

} // namespace mt::vm

#endif // MAPLETHIEF_RUNTIME_MEM_COMMIT_MAP_H
