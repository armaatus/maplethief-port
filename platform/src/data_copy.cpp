// INTERIM (#263): a minimal copy_player_data for the Data location switch, replaced by #262's
// data_copy (the first-start copy, --migrate-from) when it lands; see data_copy.h.
#include "data_copy.h"

#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "data_location.h"
#include "save_tools.h"

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

unsigned long process_id() {
#if defined(_WIN32)
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::string u8(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool is_tmp(const fs::path &p) { return p.extension() == ".mt-tmp"; }

// `from` into `to` (created), byte for byte, *.mt-tmp left out.
bool copy_tree(const fs::path &from, const fs::path &to, const std::string &rel,
               DataCopyResult *r) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        r->error = u8(to) + ": " + ec.message();
        return false;
    }
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path name = it->path().filename();
        const std::string here = rel + "/" + u8(name);
        if (it->is_directory(ec)) {
            if (!copy_tree(it->path(), to / name, here, r)) {
                return false;
            }
        } else if (is_tmp(name)) {
            r->skipped.push_back(here);
        } else if (it->is_regular_file(ec)) {
            if (!fs::copy_file(it->path(), to / name, fs::copy_options::overwrite_existing, ec)) {
                r->error = u8(it->path()) + ": " + ec.message();
                return false;
            }
            r->copied.push_back(here);
        }
    }
    if (ec) {
        r->error = u8(from) + ": " + ec.message();
        return false;
    }
    return true;
}

// Every entry of `stage` into `dst` that `dst` lacks (folders merged); never overwrites.
void place(const fs::path &stage, const fs::path &dst, const std::string &rel, DataCopyResult *r) {
    std::error_code ec;
    for (fs::directory_iterator it(stage, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path target = dst / it->path().filename();
        const std::string here = rel + u8(it->path().filename());
        if (!fs::exists(target, ec)) {
            fs::rename(it->path(), target, ec);
        } else if (it->is_directory(ec) && fs::is_directory(target, ec)) {
            place(it->path(), target, here + "/", r);
        } else {
            r->skipped.push_back(here + " (already there)");
        }
    }
}

} // namespace

bool data_dir_has_player_data(const fs::path &dir) {
    std::error_code ec;
    if (fs::is_regular_file(dir / "settings.ini", ec)) {
        return true;
    }
    for (const char *name : {kSavesDirName, kOldDocumentsDirName}) {
        if (!read_save_dir(u8(dir / name)).empty()) {
            return true;
        }
    }
    return false;
}

DataCopyResult copy_player_data(const fs::path &src_dir, const fs::path &dst_dir,
                                const fs::path &dst_bundle_dir, bool force) {
    DataCopyResult r;
    std::error_code ec;
    if (!force && data_dir_has_player_data(dst_dir)) {
        r.error = u8(dst_dir) + " already has data";
        return r;
    }
    fs::path saves = src_dir / kSavesDirName;
    if (!fs::is_directory(saves, ec)) {
        saves = src_dir / kOldDocumentsDirName;
    }
    const SaveSet set = read_save_dir(u8(saves));
    if (const std::vector<SaveProblem> problems = check_save_set(set); !problems.empty()) {
        r.error = "the saves in " + u8(saves) + " fail the checks: " + problems.front().line();
        return r;
    }
    fs::path stage = dst_dir;
    stage += ".tmp-" + std::to_string(process_id());
    fs::remove_all(stage, ec);
    fs::create_directories(stage / kSavesDirName, ec);
    if (ec) {
        r.error = u8(stage) + ": " + ec.message();
        return r;
    }
    const auto fail = [&](std::string why) {
        std::error_code ignore;
        fs::remove_all(stage, ignore);
        r.copied.clear();
        r.error = std::move(why);
        return r;
    };
    for (const SaveFile &f : set) {
        std::ofstream out(stage / kSavesDirName / f.name, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(f.bytes.data()),
                  static_cast<std::streamsize>(f.bytes.size()));
        out.close();
        if (!out) {
            return fail("cannot write " + u8(stage / kSavesDirName / f.name));
        }
        r.copied.push_back(std::string(kSavesDirName) + "/" + f.name);
    }
    for (const fs::path &gc : {saves / kGameCenterFileName, src_dir / kGameCenterFileName}) {
        if (fs::is_regular_file(gc, ec)) {
            if (!fs::copy_file(gc, stage / kSavesDirName / kGameCenterFileName, ec)) {
                return fail(u8(gc) + ": " + ec.message());
            }
            r.copied.push_back(std::string(kSavesDirName) + "/" + kGameCenterFileName);
            break;
        }
    }
    for (const char *name : {"settings.ini", "bindings.ini", "window.ini"}) {
        if (fs::is_regular_file(src_dir / name, ec)) {
            if (!fs::copy_file(src_dir / name, stage / name, ec)) {
                return fail(u8(src_dir / name) + ": " + ec.message());
            }
            r.copied.push_back(name);
        }
    }
    for (const char *name :
         {kBackupsDirName, kStatesDirName, kScreenshotsDirName, kExportsDirName, kImportDirName}) {
        if (fs::is_directory(src_dir / name, ec) &&
            !copy_tree(src_dir / name, stage / name, name, &r)) {
            return fail(r.error);
        }
    }
    if (!dst_bundle_dir.empty() && fs::is_directory(src_dir / kBundleDirName, ec) &&
        !fs::exists(dst_bundle_dir, ec)) {
        fs::path bundle_stage = dst_bundle_dir;
        bundle_stage += ".tmp-" + std::to_string(process_id());
        fs::remove_all(bundle_stage, ec);
        if (!copy_tree(src_dir / kBundleDirName, bundle_stage, kBundleDirName, &r)) {
            fs::remove_all(bundle_stage, ec);
            return fail(r.error);
        }
        fs::rename(bundle_stage, dst_bundle_dir, ec);
        if (ec) {
            fs::remove_all(bundle_stage, ec);
            return fail(u8(dst_bundle_dir) + ": " + ec.message());
        }
    }
    if (!fs::exists(dst_dir, ec)) {
        fs::create_directories(dst_dir.parent_path(), ec);
        fs::rename(stage, dst_dir, ec);
        if (ec) {
            return fail(u8(dst_dir) + ": " + ec.message());
        }
    } else {
        place(stage, dst_dir, "", &r);
        fs::remove_all(stage, ec);
    }
    r.ok = true;
    return r;
}

} // namespace maplethief::frontend
