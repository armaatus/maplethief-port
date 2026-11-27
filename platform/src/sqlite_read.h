// A read-only reader of SQLite 3 database files, just enough to scan one table (#374, ADR 0078's
// addendum): an iPhone backup's Manifest.db (docs/formats/ios-backup.md) lists the device's
// files in its table Files, and the game reads that list without the SQLite library.
//
// What it reads (https://www.sqlite.org/fileformat2.html): the 100-byte header (page size,
// reserved bytes, text encoding), the schema table on page 1 (a table's root page and the column
// names of its CREATE TABLE), and a rowid table's b-tree: interior (5) and leaf (13) table pages,
// cell payloads with their overflow chains, and records (NULL, integers, floats, text, blobs).
// Not read: indexes, WITHOUT ROWID tables, UTF-16 databases, the write-ahead log (a "-wal" file
// beside the database is ignored, as SQLite's immutable mode does; a finished backup has none).
//
// The file is untrusted: every read is bounds-checked, a page is visited at most once in a scan
// (a cycle is an error), the tree is at most kMaxDepth deep and a record at most
// kMaxRecordBytes. A broken file gives false and a reason naming the page, never a crash.
#ifndef MAPLETHIEF_PLATFORM_SQLITE_READ_H
#define MAPLETHIEF_PLATFORM_SQLITE_READ_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace maplethief::frontend {

struct SqliteValue {
    enum class Type { kNull, kInteger, kFloat, kText, kBlob };
    Type type = Type::kNull;
    std::int64_t integer = 0;
    double real = 0;
    std::string bytes; // text (UTF-8) or blob

    bool is_text() const { return type == Type::kText; }
    bool is_integer() const { return type == Type::kInteger; }
};
using SqliteRow = std::vector<SqliteValue>;

class SqliteReader {
  public:
    static constexpr std::uint32_t kMaxDepth = 20;
    static constexpr std::size_t kMaxRecordBytes = 4u * 1024u * 1024u;

    // Reads `n` bytes at `offset` into `out`; false if it cannot.
    using ReadAt = std::function<bool(std::uint64_t offset, std::size_t n, std::uint8_t *out)>;

    // Opens a database of `size` bytes read through `read`; false (with `why`) if it is not an
    // SQLite 3 file this reader takes.
    bool open(ReadAt read, std::uint64_t size, std::string *why);
    // A database held in memory (tests). The bytes must outlive the reader.
    bool open(std::span<const std::uint8_t> bytes, std::string *why);
    // A database file (UTF-8 path), read page by page as needed.
    bool open_file(const std::string &path, std::string *why);

    // The table `name`'s root page and its columns' names, from the schema; false (with `why`)
    // if there is no such table or the schema cannot be read.
    bool table(std::string_view name, std::uint32_t *root, std::vector<std::string> *columns,
               std::string *why);
    // Calls `row` with each row of the rowid table rooted at `root`, in rowid order, until it
    // returns false. The row holds the values the record has: a table that gained columns later
    // has shorter records, whose missing values read as NULL. False (with `why`) if the tree is
    // broken; rows already handed over stay handed over.
    bool scan(std::uint32_t root,
              const std::function<bool(std::int64_t rowid, const SqliteRow &row)> &row,
              std::string *why);

    std::uint32_t page_size() const { return page_size_; }
    std::uint32_t page_count() const { return page_count_; }

  private:
    bool page(std::uint32_t number, std::vector<std::uint8_t> *out, std::string *why) const;
    bool payload(const std::vector<std::uint8_t> &page, std::size_t at, std::uint32_t number,
                 std::uint64_t size, std::vector<std::uint8_t> *out, std::string *why) const;

    ReadAt read_;
    std::uint64_t size_ = 0;
    std::uint32_t page_size_ = 0;
    std::uint32_t usable_ = 0;
    std::uint32_t page_count_ = 0;
};

// The values of a record (the payload of a table cell); false if it is malformed.
bool sqlite_record(std::span<const std::uint8_t> payload, SqliteRow *row);
// The column names of a CREATE TABLE statement, in order (constraints such as PRIMARY KEY (...)
// left out); empty if it is not one.
std::vector<std::string> sqlite_create_columns(std::string_view sql);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SQLITE_READ_H
