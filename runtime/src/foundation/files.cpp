// The save files (#49, ADR 0035): NSFileManager, NSFileHandle, NSHomeDirectory and
// NSSearchPathForDirectoriesInDomains, as the game uses them (docs/formats/save.md):
//
//   SaveIO_DocumentsPath 0x6EF1   [[NSHomeDirectory() stringByAppendingPathComponent:@"Documents"]
//                                  stringByAppendingPathComponent:name]
//   SaveIO_Open 0x6F65            fileHandleForReadingAtPath: / ForWritingAtPath: (+
//                                 truncateFileAtOffset:0) / createFileAtPath:contents:attributes:
//                                 if missing, then fileHandleForUpdatingAtPath:
//   write 0xB729                  writeData:, synchronizeFile, closeFile (at offset 0, no truncate)
//   read 0xB7C9                   availableData.length for the size, readDataOfLength: + getBytes:
//   delete a slot 0x9462D         NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, ...)[0],
//                                 removeItemAtPath:error:
//
// Guest paths map to host paths: the app's home directory (the directory that holds the bundle,
// NSHomeDirectory()) is the data directory of the offline stubs (mt_stubs_data_dir,
// MAPLETHIEF_DATA_DIR, ADR 0034), and <home>/Documents is mt_saves_documents_dir() (by default
// <data dir>/Documents; on iOS the app's own Documents/), so <home>/Documents/theif0 is
// <documents>/theif0 and a device's Documents/ can be copied in as is. The bundle is readable,
// nothing else is reachable.
//
// Saving never leaves half a file (#78, ADR 0074). A file handle holds the file's bytes in
// memory from the moment it opens; -writeData: and -truncateFileAtOffset: change that copy at the
// handle's offset (the game's write at offset 0 without truncating keeps the old tail, as on
// the device), and -synchronizeFile, -closeFile or the handle's end replace the file with it in
// one step (mt_saves_replace_file: a temporary file renamed over it). A file that
// -createFileAtPath:contents:nil makes is only remembered until a handle on it saves: SaveIO_Open
// 0x6F65 (the only caller) creates the file and then writes it, so a crash in between leaves no
// empty slot file behind. The bytes a handle reads of a save file go through mt_saves_sanitise
// (config's game speed, maplethief/saves.h).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <share.h> // _SH_DENYNO
#endif

#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "maplethief/saves.h"
#include "maplethief/stubs.h"

namespace mt_ns {

namespace {

namespace fs = std::filesystem;

#define MT_COUNT(a) static_cast<u32>(sizeof(a) / sizeof((a)[0]))

// NSSearchPathDirectory and NSSearchPathDomainMask (NSPathUtilities.h).
constexpr u32 k_document_directory = 9;
constexpr u32 k_user_domain_mask = 1;
// NSCocoaErrorDomain codes (FoundationErrors.h).
constexpr std::int32_t k_file_no_such_file = 4;
constexpr std::int32_t k_file_write_unknown = 512;

fs::path to_path(const std::string &utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::FILE *open_file(const std::string &host, const char *mode) {
#if defined(_WIN32)
    // Shared like a POSIX open: _wfopen_s denies other opens, so a second handle on a file
    // (reading while a writing handle is still open, as iOS allows) would fail.
    std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
    return _wfsopen(to_path(host).c_str(), wmode.c_str(), _SH_DENYNO);
#else
    return std::fopen(host.c_str(), mode);
#endif
}

std::string path_arg(mt_cpu *cpu, u32 path, const char *method) {
    if (path == 0 || !mt_cf::is_string(cpu, path)) {
        raise(cpu, method, "nil or not an NSString path", path);
    }
    return string_utf8(cpu, path);
}

u32 autoreleased(mt_cpu *cpu, u32 obj) { return obj != 0 ? mt_objc_autorelease(cpu, obj) : 0u; }

// *error = an autoreleased NSError in NSCocoaErrorDomain (if `error` is not NULL).
void set_error(mt_cpu *cpu, u32 error, std::int32_t code) {
    if (error == 0) {
        return;
    }
    mt_g2h(cpu, error, 4);
    const u32 domain = string_from_utf8(cpu, "NSCocoaErrorDomain");
    const u32 args[3] = {domain, static_cast<u32>(code), 0u};
    const u32 err =
        mt_objc_msg(cpu, class_named(cpu, "NSError"),
                    mt_objc_sel(runtime(cpu), "errorWithDomain:code:userInfo:"), args, 3);
    mt_objc_release(cpu, domain);
    std::memcpy(mt_g2h(cpu, error, 4), &err, 4);
}

// --- the files' bytes ----------------------------------------------------------------------------

// Files created by -createFileAtPath:contents:nil that no handle has saved yet (host paths), per
// runtime (a new process starts without them). They exist for the guest (fileExistsAtPath:, the
// handles) but not yet on the disk.
struct PendingCreates {
    mt_objc *rt = nullptr;
    u32 binding = 0;
    std::set<std::string> paths;
};
PendingCreates g_pending;

std::set<std::string> &pending_creates(mt_cpu *cpu) {
    mt_objc *rt = runtime(cpu);
    if (g_pending.rt != rt || g_pending.binding != mt_cf::binding()) {
        g_pending = PendingCreates{rt, mt_cf::binding(), {}};
    }
    return g_pending.paths;
}

bool pending(mt_cpu *cpu, const std::string &host) { return pending_creates(cpu).count(host) != 0; }

// The whole file at `host`; false if it cannot be read.
bool read_whole(const std::string &host, std::vector<std::uint8_t> *out) {
    std::FILE *f = open_file(host, "rb");
    if (f == nullptr) {
        return false;
    }
    out->clear();
    // 64 KiB at a time, straight into `out`. No large buffer on the stack: a raise below this
    // frame is a longjmp, and MSVC's unwinding longjmp crashed through a 64 KiB frame (Release).
    bool ok = true;
    for (;;) {
        const std::size_t at = out->size();
        out->resize(at + 65536);
        const std::size_t got = std::fread(out->data() + at, 1, 65536, f);
        out->resize(at + got);
        if (got < 65536) {
            ok = std::ferror(f) == 0;
            break;
        }
    }
    std::fclose(f);
    return ok;
}

// The save file's name if `host` is a file directly in the Documents directory, else "".
std::string save_name(const std::string &host) {
    const std::string docs = mt_saves_documents_dir();
    if (docs.empty() || host.size() <= docs.size() + 1 || host.compare(0, docs.size(), docs) != 0 ||
        host[docs.size()] != '/') {
        return std::string();
    }
    const std::string name = host.substr(docs.size() + 1);
    return name.find('/') == std::string::npos ? name : std::string();
}

// Replaces the file with `bytes` in one step; false (logged) if it could not be written, and the
// file keeps its last saved bytes.
bool save_whole(mt_cpu *cpu, const std::string &host, const std::vector<std::uint8_t> &bytes,
                const char *method) {
    if (mt_saves_replace_file(host.c_str(), bytes.data(), bytes.size()) != 0) {
        log(cpu, "%s: cannot save %s: it keeps its last saved bytes", method, host.c_str());
        return false;
    }
    pending_creates(cpu).erase(host);
    return true;
}

// --- NSFileHandle --------------------------------------------------------------------------------

// An open handle's file: its bytes since the handle opened (with the handle's changes), the
// handle's offset, and whether the file does not have the changes yet.
struct OpenFile {
    std::string host;
    std::vector<std::uint8_t> bytes;
    std::size_t offset = 0;
    bool can_read = false, can_write = false, dirty = false;
};

struct HandleState {
    OpenFile *file; // nullptr once closed
};

// The handle's changes reach the file (nothing to do without changes).
bool save_changes(mt_cpu *cpu, OpenFile &f, const char *method) {
    if (!f.dirty) {
        return true;
    }
    if (!save_whole(cpu, f.host, f.bytes, method)) {
        return false;
    }
    f.dirty = false;
    return true;
}

void handle_destroy(mt_cpu *cpu, u32, void *p) {
    auto *s = static_cast<HandleState *>(p);
    if (s->file != nullptr) {
        save_changes(cpu, *s->file, "-[NSFileHandle dealloc]");
        delete s->file;
        s->file = nullptr;
    }
}
const mt_objc_state_type k_handle_state = {"NSFileHandle", sizeof(HandleState), handle_destroy};

// An autoreleased NSFileHandle on `guest`, or nil if the file cannot be opened that way.
void open_handle(mt_cpu *cpu, const char *method, bool read, bool write) {
    const u32 cls = cpu->r[0];
    const std::string guest = path_arg(cpu, cpu->r[2], method);
    const std::string host = host_path(guest, write);
    auto *f = new OpenFile{host, {}, 0, read, write, false};
    bool ok = !host.empty();
    if (ok && pending(cpu, host)) {
        f->dirty = write; // created, not saved yet: empty, and saved when this handle saves
    } else if (ok) {
        std::error_code ec;
        ok = fs::is_regular_file(to_path(host), ec) && read_whole(host, &f->bytes);
    }
    if (!ok) {
        if (host.empty()) {
            log(cpu, "%s: %s is outside the app's home directory: nil", method, guest.c_str());
        }
        delete f;
        cpu->r[0] = 0;
        return;
    }
    const std::string name = save_name(host);
    if (!name.empty() && mt_saves_sanitise(name.c_str(), f->bytes.data(), f->bytes.size()) != 0) {
        log(cpu, "%s: %s holds values past the game's tables: read as the nearest valid ones",
            method, name.c_str());
    }
    mt_objc *rt = runtime(cpu);
    const u32 h = mt_objc_create_instance(rt, cls);
    auto *s = h != 0 ? static_cast<HandleState *>(mt_objc_state(rt, h, &k_handle_state)) : nullptr;
    if (s == nullptr) {
        delete f;
        cpu->r[0] = 0;
        return;
    }
    s->file = f;
    cpu->r[0] = mt_objc_autorelease(cpu, h);
}

void handle_for_reading(mt_cpu *cpu) {
    open_handle(cpu, "+[NSFileHandle fileHandleForReadingAtPath:]", true, false);
}
void handle_for_writing(mt_cpu *cpu) {
    open_handle(cpu, "+[NSFileHandle fileHandleForWritingAtPath:]", false, true);
}
void handle_for_updating(mt_cpu *cpu) {
    open_handle(cpu, "+[NSFileHandle fileHandleForUpdatingAtPath:]", true, true);
}

OpenFile &handle_of(mt_cpu *cpu, u32 obj, const char *method, bool write) {
    mt_objc *rt = runtime(cpu);
    auto *s = obj != 0 ? static_cast<HandleState *>(mt_objc_find_state(rt, obj, &k_handle_state))
                       : nullptr;
    if (s == nullptr || s->file == nullptr) {
        raise(cpu, method, "a closed or invalid file handle", obj);
    }
    if (write ? !s->file->can_write : !s->file->can_read) {
        raise(cpu, method, write ? "not open for writing" : "not open for reading", obj);
    }
    return *s->file;
}

// Reads up to `max` bytes at the handle's offset into an autoreleased NSData.
void read_data(mt_cpu *cpu, const char *method, std::uint64_t max) {
    OpenFile &f = handle_of(cpu, cpu->r[0], method, false);
    const std::size_t at = std::min(f.offset, f.bytes.size());
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(max, f.bytes.size() - at));
    f.offset = at + n;
    cpu->r[0] =
        autoreleased(cpu, mt_cf::data_create(cpu, f.bytes.data() + at, static_cast<u32>(n), false));
}

// -availableData: the rest of the file.
void handle_available_data(mt_cpu *cpu) {
    read_data(cpu, "-[NSFileHandle availableData]", 0xFFFFFFFFu);
}

// -readDataOfLength:
void handle_read_data(mt_cpu *cpu) {
    read_data(cpu, "-[NSFileHandle readDataOfLength:]", cpu->r[2]);
}

// -writeData: at the handle's offset, over the bytes there and past the end (never truncating).
void handle_write_data(mt_cpu *cpu) {
    const u32 self = cpu->r[0], data = cpu->r[2];
    u32 bytes = 0, length = 0;
    if (data == 0 || !mt_cf::data_bytes(cpu, data, &bytes, &length)) {
        raise(cpu, "-[NSFileHandle writeData:]", "nil or not an NSData", data);
    }
    OpenFile &f = handle_of(cpu, self, "-[NSFileHandle writeData:]", true);
    if (f.bytes.size() < f.offset + length) {
        f.bytes.resize(f.offset + length);
    }
    if (length != 0) {
        std::memcpy(f.bytes.data() + f.offset, cpu->mem + bytes, length);
    }
    f.offset += length;
    f.dirty = true;
}

// -synchronizeFile: the handle's changes reach the file and the disk, all of them in one step.
void handle_synchronize(mt_cpu *cpu) {
    OpenFile &f = handle_of(cpu, cpu->r[0], "-[NSFileHandle synchronizeFile]", true);
    save_changes(cpu, f, "-[NSFileHandle synchronizeFile]");
}

// -truncateFileAtOffset:(unsigned long long): the file ends there (zeros if that is past its
// end), and the offset moves there.
void handle_truncate(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const std::uint64_t offset = mt_arg_u64(&a);
    OpenFile &f = handle_of(cpu, cpu->r[0], "-[NSFileHandle truncateFileAtOffset:]", true);
    if (offset > 0xFFFFFFFFu) {
        raise(cpu, "-[NSFileHandle truncateFileAtOffset:]", "cannot truncate", cpu->r[0]);
    }
    f.bytes.resize(static_cast<std::size_t>(offset));
    f.offset = static_cast<std::size_t>(offset);
    f.dirty = true;
}

// -closeFile: the handle's changes are saved, and it does nothing more.
void handle_close(mt_cpu *cpu) {
    mt_objc *rt = runtime(cpu);
    auto *s = static_cast<HandleState *>(mt_objc_find_state(rt, cpu->r[0], &k_handle_state));
    if (s == nullptr || s->file == nullptr) {
        raise(cpu, "-[NSFileHandle closeFile]", "a closed or invalid file handle", cpu->r[0]);
    }
    save_changes(cpu, *s->file, "-[NSFileHandle closeFile]");
    delete s->file;
    s->file = nullptr;
}

const mt_objc_host_method k_handle_instance[] = {
    {"availableData", handle_available_data},   {"readDataOfLength:", handle_read_data},
    {"writeData:", handle_write_data},          {"synchronizeFile", handle_synchronize},
    {"truncateFileAtOffset:", handle_truncate}, {"closeFile", handle_close},
};
const mt_objc_host_method k_handle_class[] = {
    {"fileHandleForReadingAtPath:", handle_for_reading},
    {"fileHandleForWritingAtPath:", handle_for_writing},
    {"fileHandleForUpdatingAtPath:", handle_for_updating},
};

// --- NSFileManager -------------------------------------------------------------------------------

// +defaultManager
void manager_default(mt_cpu *cpu) { cpu->r[0] = singleton(cpu, "NSFileManager"); }

// -fileExistsAtPath: (a file or a directory)
void manager_exists(mt_cpu *cpu) {
    const std::string host =
        host_path(path_arg(cpu, cpu->r[2], "-[NSFileManager fileExistsAtPath:]"), false);
    std::error_code ec;
    cpu->r[0] = !host.empty() && (pending(cpu, host) || fs::exists(to_path(host), ec)) ? 1u : 0u;
}

// -createFileAtPath:contents:attributes: a new file (or a replaced one) with the bytes of the
// NSData (nil: empty), in one step; the attributes are ignored. NO if it cannot be written. A
// missing file created empty is only remembered until a handle on it saves (the header).
void manager_create_file(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 path = mt_arg_u32(&a), data = mt_arg_u32(&a);
    const char *method = "-[NSFileManager createFileAtPath:contents:attributes:]";
    const std::string guest = path_arg(cpu, path, method);
    u32 bytes = 0, length = 0;
    if (data != 0 && !mt_cf::data_bytes(cpu, data, &bytes, &length)) {
        raise(cpu, method, "not an NSData", data);
    }
    const std::string host = host_path(guest, true);
    std::error_code ec;
    bool ok = !host.empty();
    if (ok && length == 0 && !fs::exists(to_path(host), ec)) {
        pending_creates(cpu).insert(host);
    } else if (ok) {
        const std::uint8_t *p = cpu->mem + bytes;
        ok = save_whole(cpu, host, std::vector<std::uint8_t>(p, p + length), method);
    }
    if (!ok) {
        log(cpu, "-[NSFileManager createFileAtPath:%s ...]: cannot write %s: NO", guest.c_str(),
            host.empty() ? "(outside the home directory)" : host.c_str());
    }
    cpu->r[0] = ok ? 1u : 0u;
}

// -removeItemAtPath:error: (a file, or a directory with its contents)
void manager_remove(mt_cpu *cpu) {
    const u32 path = cpu->r[2], error = cpu->r[3];
    const std::string guest = path_arg(cpu, path, "-[NSFileManager removeItemAtPath:error:]");
    const std::string host = host_path(guest, true);
    std::error_code ec;
    if (!host.empty() && pending_creates(cpu).erase(host) != 0 && !fs::exists(to_path(host), ec)) {
        cpu->r[0] = 1u; // created and never saved: nothing on the disk
        return;
    }
    const bool exists = !host.empty() && fs::exists(to_path(host), ec);
    const bool ok =
        exists && fs::remove_all(to_path(host), ec) != static_cast<std::uintmax_t>(-1) && !ec;
    if (!ok) {
        set_error(cpu, error, exists ? k_file_write_unknown : k_file_no_such_file);
    }
    cpu->r[0] = ok ? 1u : 0u;
}

const mt_objc_host_method k_manager_instance[] = {
    {"retain", singleton_retain},
    {"release", singleton_release},
    {"retainCount", singleton_retain_count},
    {"fileExistsAtPath:", manager_exists},
    {"createFileAtPath:contents:attributes:", manager_create_file},
    {"removeItemAtPath:error:", manager_remove},
};
const mt_objc_host_method k_manager_class[] = {{"defaultManager", manager_default}};

bool starts_with_dir(const std::string &path, const std::string &dir, std::string *rest) {
    if (path == dir) {
        rest->clear();
        return true;
    }
    if (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
        path[dir.size()] == '/') {
        *rest = path.substr(dir.size() + 1);
        return true;
    }
    return false;
}

} // namespace

const mt_objc_host_class k_nsfilehandle = {"NSFileHandle",
                                           "NSObject",
                                           4,
                                           k_handle_instance,
                                           MT_COUNT(k_handle_instance),
                                           k_handle_class,
                                           MT_COUNT(k_handle_class)};
const mt_objc_host_class k_nsfilemanager = {"NSFileManager",
                                            "NSObject",
                                            4,
                                            k_manager_instance,
                                            MT_COUNT(k_manager_instance),
                                            k_manager_class,
                                            MT_COUNT(k_manager_class)};

std::string host_path(const std::string &guest, bool for_writing) {
    // Split into components: no "." or "..", empty ones ("//", a trailing '/') dropped.
    if (guest.empty() || guest[0] != '/') {
        return std::string();
    }
    std::string norm;
    std::size_t i = 1;
    while (i <= guest.size()) {
        const std::size_t j = std::min(guest.find('/', i), guest.size());
        const std::string part = guest.substr(i, j - i);
        if (part == "." || part == "..") {
            return std::string();
        }
        if (!part.empty()) {
            norm += "/" + part;
        }
        i = j + 1;
    }
    std::string rest;
    if (starts_with_dir(norm, mt_cf::config().bundle_path, &rest)) {
        return for_writing || mt_cf::config().bundle_dir.empty() ? std::string()
                                                                 : mt_cf::bundle_host_path(rest);
    }
    const std::string data_dir = mt_stubs_data_dir();
    if (data_dir.empty() || !starts_with_dir(norm, home_guest_path(), &rest)) {
        return std::string();
    }
    std::string in_documents;
    if (starts_with_dir("/" + rest, "/Documents", &in_documents)) {
        const std::string docs = mt_saves_documents_dir();
        if (docs.empty()) {
            return std::string();
        }
        return in_documents.empty() ? docs : docs + "/" + in_documents;
    }
    return rest.empty() ? data_dir : data_dir + "/" + rest;
}

} // namespace mt_ns

using namespace mt_ns;

// NSString *NSHomeDirectory(void)
extern "C" void mt_hle_NSHomeDirectory(mt_cpu *cpu, std::uint32_t) noexcept {
    cpu->r[0] = autoreleased(cpu, string_from_utf8(cpu, home_guest_path()));
}

// NSArray *NSSearchPathForDirectoriesInDomains(NSSearchPathDirectory, NSSearchPathDomainMask,
//                                              BOOL expandTilde): [<home>/Documents] for the
// user's document directory (the game's slot deletion); empty for anything else (logged).
extern "C" void mt_hle_NSSearchPathForDirectoriesInDomains(mt_cpu *cpu, std::uint32_t) noexcept {
    const u32 directory = cpu->r[0], domains = cpu->r[1];
    std::vector<u32> items;
    if (directory == k_document_directory && (domains & k_user_domain_mask) != 0) {
        items.push_back(string_from_utf8(cpu, home_guest_path() + "/Documents"));
    } else {
        log(cpu, "NSSearchPathForDirectoriesInDomains(%u, %u): not a directory the port has: ()",
            static_cast<unsigned>(directory), static_cast<unsigned>(domains));
    }
    const u32 a = array_create(cpu, items.data(), static_cast<u32>(items.size()), false);
    for (const u32 s : items) {
        mt_objc_release(cpu, s);
    }
    cpu->r[0] = autoreleased(cpu, a);
}
