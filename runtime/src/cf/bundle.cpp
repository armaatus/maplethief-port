// CFBundle and CFURL (#48, ADR 0032): the main bundle is the extracted app directory
// (mt_cf_config.bundle_dir, e.g. work/ipa/Payload/MapleThief.app), and resource lookup is
// how the game reads every one of its files: FUN_0000a2c0 calls
// CFBundleCopyResourceURLForLocalization(main, name, NULL, subdir, CFSTR("English")) and
// CFURLCreateDataAndPropertiesFromResource, then copies the CFData out.
//
// The bundle is indexed once (every file, bundle-relative, exact case) and looked up in the
// index, so that a name matches the way it does on the device's case-sensitive file system
// on every host, and a guest path can never leave the bundle ("..", absolute paths).
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <unordered_set>
#include <vector>

#include "cf/cf_hle.h"
#include "cf/cf_objects.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"

namespace mt_cf {

namespace {

constexpr std::int32_t k_resource_not_found = -12; // kCFURLResourceNotFoundError
constexpr u32 k_max_file = 256u << 20;             // larger files are no game resource

struct Index {
    u32 generation = 0xFFFFFFFFu;
    std::unordered_set<std::string> files;
    std::vector<std::string> sorted;
};
Index g_index;
u32 g_main_bundle = 0;

struct UrlState {
    std::string *rel; // bundle-relative path
};
void destroy_url(mt_cpu *, u32, void *p) { delete static_cast<UrlState *>(p)->rel; }
const mt_objc_state_type k_url_state = {"CFURL", sizeof(UrlState), destroy_url};
const mt_objc_state_type k_bundle_state = {"CFBundle", 4, nullptr};

std::filesystem::path host_path(const std::string &utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8_of(const std::filesystem::path &p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

const Index &index(mt_cpu *cpu) {
    const Config &c = config();
    if (g_index.generation == c.generation) {
        return g_index;
    }
    g_index = Index{};
    g_index.generation = c.generation;
    if (c.bundle_dir.empty()) {
        log(cpu, "no bundle directory configured (%s): every resource lookup fails",
            MT_CF_BUNDLE_DIR_ENV);
        return g_index;
    }
    const std::filesystem::path root = host_path(c.bundle_dir);
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root, ec), end;
    if (ec) {
        log(cpu, "bundle directory %s: %s", c.bundle_dir.c_str(), ec.message().c_str());
        return g_index;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            log(cpu, "bundle directory %s: %s", c.bundle_dir.c_str(), ec.message().c_str());
            break;
        }
        if (it->is_regular_file(ec)) {
            std::string rel = utf8_of(it->path().lexically_relative(root));
            g_index.files.insert(rel);
            g_index.sorted.push_back(std::move(rel));
        }
    }
    std::sort(g_index.sorted.begin(), g_index.sorted.end());
    return g_index;
}

// Appends the '/'-separated components of `part` to `out`; false for "..".
bool append_components(std::string &out, const std::string &part) {
    std::size_t i = 0;
    while (i <= part.size()) {
        std::size_t j = part.find('/', i);
        if (j == std::string::npos) {
            j = part.size();
        }
        const std::string c = part.substr(i, j - i);
        if (c == "..") {
            return false;
        }
        if (!c.empty() && c != ".") {
            out += out.empty() ? c : "/" + c;
        }
        i = j + 1;
    }
    return true;
}

// The localization's .lproj names: itself and its legacy/ISO alias.
void lproj_names(const std::string &loc, std::vector<std::string> &out) {
    static const char *const k_aliases[][2] = {
        {"English", "en"}, {"Korean", "ko"}, {"Japanese", "ja"}};
    auto add = [&out](const std::string &name) {
        if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
    };
    add(loc);
    for (const auto &a : k_aliases) {
        if (loc == a[0]) {
            add(a[1]);
        } else if (loc == a[1]) {
            add(a[0]);
        }
    }
}

std::string utf8_string(mt_cpu *cpu, u32 obj) {
    const std::u16string *s = string_chars(cpu, obj);
    return s != nullptr ? to_utf8(*s) : std::string();
}

u32 main_bundle(mt_cpu *cpu) {
    if (g_main_bundle == 0) {
        g_main_bundle = create(cpu, classes(cpu).type);
        if (g_main_bundle != 0) {
            mt_objc_state(runtime(cpu), g_main_bundle, &k_bundle_state);
            make_immortal(cpu, g_main_bundle);
        }
    }
    return g_main_bundle;
}

} // namespace

void bundle_forget() noexcept { g_main_bundle = 0; }

std::string bundle_find(mt_cpu *cpu, const std::string &name, const std::string &type,
                        const std::string &subdir, const std::string &localization) {
    std::string file = name;
    if (!type.empty()) {
        file += type[0] == '.' ? type : "." + type;
    }
    std::string rel;
    if (file.empty() || !append_components(rel, subdir) || !append_components(rel, file)) {
        return {};
    }
    const Index &ix = index(cpu);
    if (ix.files.count(rel) != 0) {
        return rel;
    }
    std::vector<std::string> lprojs;
    if (!localization.empty()) {
        lproj_names(localization, lprojs);
    } else {
        lproj_names(config().language, lprojs);
        lproj_names("English", lprojs); // the development region
    }
    for (const std::string &l : lprojs) {
        const std::string candidate = l + ".lproj/" + rel;
        if (ix.files.count(candidate) != 0) {
            return candidate;
        }
    }
    return {};
}

std::string bundle_host_path(const std::string &rel) { return config().bundle_dir + "/" + rel; }

std::string bundle_guest_path(const std::string &rel) {
    return rel.empty() ? config().bundle_path : config().bundle_path + "/" + rel;
}

bool bundle_has(mt_cpu *cpu, const std::string &rel) { return index(cpu).files.count(rel) != 0; }

bool bundle_read(const std::string &rel, std::vector<std::uint8_t> &out) {
    std::ifstream f(host_path(bundle_host_path(rel)), std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > k_max_file) {
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    return size == 0 || static_cast<bool>(f.read(reinterpret_cast<char *>(out.data()), size));
}

std::vector<std::string> bundle_files(mt_cpu *cpu) { return index(cpu).sorted; }

u32 url_create(mt_cpu *cpu, const std::string &rel) {
    const u32 obj = create(cpu, classes(cpu).type);
    auto *s = obj != 0 ? static_cast<UrlState *>(mt_objc_state(runtime(cpu), obj, &k_url_state))
                       : nullptr;
    if (s == nullptr) {
        return 0;
    }
    s->rel = new (std::nothrow) std::string(rel);
    return obj;
}

bool url_bundle_path(mt_cpu *cpu, u32 url, std::string *rel) {
    mt_objc *rt = runtime(cpu);
    const auto *s = rt != nullptr && url != 0
                        ? static_cast<const UrlState *>(mt_objc_find_state(rt, url, &k_url_state))
                        : nullptr;
    if (s == nullptr || s->rel == nullptr) {
        return false;
    }
    *rel = *s->rel;
    return true;
}

} // namespace mt_cf

using namespace mt_cf;

// CFBundleRef CFBundleGetMainBundle(void): not owned by the caller (immortal).
extern "C" void mt_hle_CFBundleGetMainBundle(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, main_bundle(cpu));
}

// CFURLRef CFBundleCopyResourceURLForLocalization(CFBundleRef, CFStringRef resourceName,
//     CFStringRef resourceType, CFStringRef subDirName, CFStringRef localizationName)
extern "C" void mt_hle_CFBundleCopyResourceURLForLocalization(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 bundle = mt_arg_u32(&a);
    const u32 name = mt_arg_u32(&a), type = mt_arg_u32(&a), subdir = mt_arg_u32(&a);
    const u32 loc = mt_arg_u32(&a);
    if (bundle == 0 || bundle != main_bundle(cpu)) {
        misuse(cpu, "CFBundleCopyResourceURLForLocalization", "not the main bundle", bundle);
    }
    for (const u32 s : {name, type, subdir, loc}) {
        if (s != 0 && !is_string(cpu, s)) {
            misuse(cpu, "CFBundleCopyResourceURLForLocalization", "not a CFString", s);
        }
    }
    const std::string n = utf8_string(cpu, name), t = utf8_string(cpu, type);
    const std::string d = utf8_string(cpu, subdir), l = utf8_string(cpu, loc);
    const std::string rel = bundle_find(cpu, n, t, d, l);
    if (rel.empty()) {
        log(cpu,
            "CFBundleCopyResourceURLForLocalization: no resource \"%s\" (type \"%s\") in "
            "\"%s\" for \"%s\" (from 0x%08X): NULL",
            n.c_str(), t.c_str(), d.c_str(), l.c_str(), static_cast<unsigned>(cpu->r[MT_LR]));
        mt_ret_u32(cpu, 0);
        return;
    }
    mt_ret_u32(cpu, url_create(cpu, rel));
}

// Boolean CFURLCreateDataAndPropertiesFromResource(CFAllocatorRef, CFURLRef url,
//     CFDataRef *resourceData, CFDictionaryRef *properties, CFArrayRef desiredProperties,
//     SInt32 *errorCode)
extern "C" void mt_hle_CFURLCreateDataAndPropertiesFromResource(mt_cpu *cpu,
                                                                std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    (void)mt_arg_u32(&a);
    const u32 url = mt_arg_u32(&a);
    const u32 data_out = mt_arg_u32(&a), props_out = mt_arg_u32(&a);
    (void)mt_arg_u32(&a); // desiredProperties: NULL (all) at both call sites
    const u32 error_out = mt_arg_u32(&a);
    std::string rel;
    if (url == 0 || !url_bundle_path(cpu, url, &rel)) {
        misuse(cpu, "CFURLCreateDataAndPropertiesFromResource", "not a CFURL", url);
    }
    // Check every out pointer before anything is created (a bad one stops the guest).
    for (const u32 p : {data_out, props_out, error_out}) {
        if (p != 0) {
            mt_g2h(cpu, p, 4);
        }
    }
    std::vector<std::uint8_t> bytes;
    const bool ok = bundle_read(rel, bytes);
    u32 data = 0, props = 0;
    if (ok && data_out != 0) {
        data = data_create(cpu, bytes.data(), static_cast<u32>(bytes.size()), false);
    }
    if (ok && props_out != 0) {
        // The properties are a CFDictionary the game only releases: an empty NSDictionary
        // (Foundation, #49), made the toll-free way, by messages.
        mt_objc *rt = runtime(cpu);
        const u32 cls = rt != nullptr ? mt_objc_class_named(rt, "NSDictionary") : 0;
        if (cls != 0) {
            props = mt_objc_send0(cpu, mt_objc_send0(cpu, cls, "alloc"), "init");
        } else {
            log(cpu, "CFURLCreateDataAndPropertiesFromResource: no NSDictionary class: "
                     "properties NULL");
        }
    }
    if (!ok) {
        log(cpu, "CFURLCreateDataAndPropertiesFromResource: cannot read %s",
            bundle_host_path(rel).c_str());
    }
    const std::int32_t error = ok ? 0 : k_resource_not_found;
    if (data_out != 0) {
        std::memcpy(mt_g2h(cpu, data_out, 4), &data, 4);
    }
    if (props_out != 0) {
        std::memcpy(mt_g2h(cpu, props_out, 4), &props, 4);
    }
    if (error_out != 0) {
        std::memcpy(mt_g2h(cpu, error_out, 4), &error, 4);
    }
    mt_ret_u32(cpu, ok ? 1u : 0u);
}
