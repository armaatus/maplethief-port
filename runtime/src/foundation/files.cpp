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
// MAPLETHIEF_DATA_DIR, ADR 0034), so <home>/Documents/theif0 is <data dir>/Documents/theif0 and a
// device's Documents/ can be copied in as is. The bundle is readable, nothing else is reachable.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <io.h> // _chsize_s, _fileno
#else
#include <unistd.h> // ftruncate, fileno
#endif

#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
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
    std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
    std::FILE *f = nullptr;
    return _wfopen_s(&f, to_path(host).c_str(), wmode.c_str()) == 0 ? f : nullptr;
#else
    return std::fopen(host.c_str(), mode);
#endif
}

bool make_parent_dirs(const std::string &host) {
    std::error_code ec;
    const fs::path parent = to_path(host).parent_path();
    return parent.empty() || fs::create_directories(parent, ec) || fs::is_directory(parent, ec);
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

// --- NSFileHandle --------------------------------------------------------------------------------

struct HandleState {
    std::FILE *f; // nullptr once closed
    bool can_read, can_write;
};

void handle_destroy(mt_cpu *, u32, void *p) {
    auto *s = static_cast<HandleState *>(p);
    if (s->f != nullptr) {
        std::fclose(s->f);
        s->f = nullptr;
    }
}
const mt_objc_state_type k_handle_state = {"NSFileHandle", sizeof(HandleState), handle_destroy};

// An autoreleased NSFileHandle on `guest`, or nil if the file cannot be opened that way.
void open_handle(mt_cpu *cpu, const char *method, bool read, bool write) {
    const u32 cls = cpu->r[0];
    const std::string guest = path_arg(cpu, cpu->r[2], method);
    const std::string host = host_path(guest, write);
    std::FILE *f = host.empty() ? nullptr : open_file(host, write ? "r+b" : "rb");
    if (f == nullptr) {
        if (host.empty()) {
            log(cpu, "%s: %s is outside the app's home directory: nil", method, guest.c_str());
        }
        cpu->r[0] = 0;
        return;
    }
    mt_objc *rt = runtime(cpu);
    const u32 h = mt_objc_create_instance(rt, cls);
    auto *s = h != 0 ? static_cast<HandleState *>(mt_objc_state(rt, h, &k_handle_state)) : nullptr;
    if (s == nullptr) {
        std::fclose(f);
        cpu->r[0] = 0;
        return;
    }
    *s = HandleState{f, read, write};
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

HandleState &handle_of(mt_cpu *cpu, u32 obj, const char *method, bool write) {
    mt_objc *rt = runtime(cpu);
    auto *s = obj != 0 ? static_cast<HandleState *>(mt_objc_find_state(rt, obj, &k_handle_state))
                       : nullptr;
    if (s == nullptr || s->f == nullptr) {
        raise(cpu, method, "a closed or invalid file handle", obj);
    }
    if (write ? !s->can_write : !s->can_read) {
        raise(cpu, method, write ? "not open for writing" : "not open for reading", obj);
    }
    // A read after a write (or the reverse) on one stream needs a seek in between (C11 7.21.5.3).
    std::fseek(s->f, 0, SEEK_CUR);
    return *s;
}

// Reads up to `max` bytes at the handle's offset into an autoreleased NSData.
void read_data(mt_cpu *cpu, const char *method, std::uint64_t max) {
    HandleState &s = handle_of(cpu, cpu->r[0], method, false);
    std::vector<std::uint8_t> bytes;
    std::uint8_t buf[65536];
    while (bytes.size() < max) {
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(sizeof buf, max - bytes.size()));
        const std::size_t got = std::fread(buf, 1, want, s.f);
        bytes.insert(bytes.end(), buf, buf + got);
        if (got < want) {
            break;
        }
    }
    if (std::ferror(s.f) != 0) {
        raise(cpu, method, "read error", cpu->r[0]);
    }
    cpu->r[0] = autoreleased(
        cpu, mt_cf::data_create(cpu, bytes.data(), static_cast<u32>(bytes.size()), false));
}

// -availableData: the rest of the file.
void handle_available_data(mt_cpu *cpu) {
    read_data(cpu, "-[NSFileHandle availableData]", 0xFFFFFFFFu);
}

// -readDataOfLength:
void handle_read_data(mt_cpu *cpu) {
    read_data(cpu, "-[NSFileHandle readDataOfLength:]", cpu->r[2]);
}

// -writeData: at the handle's offset (an error raises NSFileHandleOperationException).
void handle_write_data(mt_cpu *cpu) {
    const u32 self = cpu->r[0], data = cpu->r[2];
    u32 bytes = 0, length = 0;
    if (data == 0 || !mt_cf::data_bytes(cpu, data, &bytes, &length)) {
        raise(cpu, "-[NSFileHandle writeData:]", "nil or not an NSData", data);
    }
    HandleState &s = handle_of(cpu, self, "-[NSFileHandle writeData:]", true);
    if (length != 0 && std::fwrite(cpu->mem + bytes, 1, length, s.f) != length) {
        raise(cpu, "-[NSFileHandle writeData:]", "write error", self);
    }
}

// -synchronizeFile: the bytes reach the file.
void handle_synchronize(mt_cpu *cpu) {
    HandleState &s = handle_of(cpu, cpu->r[0], "-[NSFileHandle synchronizeFile]", true);
    std::fflush(s.f);
}

// -truncateFileAtOffset:(unsigned long long): the file ends there, and the offset moves there.
void handle_truncate(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const std::uint64_t offset = mt_arg_u64(&a);
    HandleState &s = handle_of(cpu, cpu->r[0], "-[NSFileHandle truncateFileAtOffset:]", true);
    std::fflush(s.f);
#if defined(_WIN32)
    const bool ok = _chsize_s(_fileno(s.f), static_cast<long long>(offset)) == 0;
#else
    const bool ok = ftruncate(fileno(s.f), static_cast<off_t>(offset)) == 0;
#endif
    if (!ok || std::fseek(s.f, static_cast<long>(offset), SEEK_SET) != 0) {
        raise(cpu, "-[NSFileHandle truncateFileAtOffset:]", "cannot truncate", cpu->r[0]);
    }
}

// -closeFile
void handle_close(mt_cpu *cpu) {
    mt_objc *rt = runtime(cpu);
    auto *s = static_cast<HandleState *>(mt_objc_find_state(rt, cpu->r[0], &k_handle_state));
    if (s == nullptr || s->f == nullptr) {
        raise(cpu, "-[NSFileHandle closeFile]", "a closed or invalid file handle", cpu->r[0]);
    }
    std::fclose(s->f);
    s->f = nullptr;
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
    cpu->r[0] = !host.empty() && fs::exists(to_path(host), ec) ? 1u : 0u;
}

// -createFileAtPath:contents:attributes: a new file (or a replaced one) with the bytes of the
// NSData (nil: empty); the attributes are ignored. NO if it cannot be written.
void manager_create_file(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 path = mt_arg_u32(&a), data = mt_arg_u32(&a);
    const std::string guest =
        path_arg(cpu, path, "-[NSFileManager createFileAtPath:contents:attributes:]");
    u32 bytes = 0, length = 0;
    if (data != 0 && !mt_cf::data_bytes(cpu, data, &bytes, &length)) {
        raise(cpu, "-[NSFileManager createFileAtPath:contents:attributes:]", "not an NSData", data);
    }
    const std::string host = host_path(guest, true);
    std::FILE *f = !host.empty() && make_parent_dirs(host) ? open_file(host, "wb") : nullptr;
    bool ok = f != nullptr;
    if (ok && length != 0) {
        ok = std::fwrite(cpu->mem + bytes, 1, length, f) == length;
    }
    if (f != nullptr) {
        ok = std::fclose(f) == 0 && ok;
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
