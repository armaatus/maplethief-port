// NSBundle and NSLocale (#49, ADR 0035).
//
// NSBundle is the main bundle only, on CoreFoundation's bundle index (#48, runtime/src/cf/
// bundle.cpp): resource paths are guest paths inside MT_CF_DEFAULT_BUNDLE_PATH (the sound
// loader turns them into file URLs), Info.plist values come localized from
// <language>.lproj/InfoPlist.strings first, as on iOS, and -localizedStringForKey:value:table:
// reads <language>.lproj/<table>.strings: every line of dialogue (LocalizedCString 0x6DD1,
// docs/formats/script-out.md). All of these files are binary plists (plist.h); each is parsed
// once per configuration.
//
// NSLocale answers from the configured language (mt_cf_language: "en" or "ko"): the game asks
// for the country code (Korea shows the terms of service) and the preferred languages.
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "foundation/plist.h"
#include "maplethief/cf.h"
#include "maplethief/hle/abi.h"

namespace mt_ns {

namespace {

#define MT_COUNT(a) static_cast<u32>(sizeof(a) / sizeof((a)[0]))

// Parsed plists by bundle-relative path, for one configuration (generation).
struct Plists {
    u32 generation = 0;
    std::map<std::string, PlistValue> files; // a failed parse is a null value
};
Plists g_plists;

// The plist at bundle-relative `rel` (a null value if it is missing or malformed: logged once).
const PlistValue &plist(mt_cpu *cpu, const std::string &rel) {
    const u32 generation = mt_cf::config().generation;
    if (g_plists.generation != generation) {
        g_plists = Plists{};
        g_plists.generation = generation;
    }
    auto it = g_plists.files.find(rel);
    if (it != g_plists.files.end()) {
        return it->second;
    }
    PlistValue v;
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!mt_cf::bundle_read(rel, bytes)) {
        log(cpu, "NSBundle: cannot read %s", rel.c_str());
    } else if (!plist_parse(bytes.data(), bytes.size(), &v, &error)) {
        log(cpu, "NSBundle: %s: %s", rel.c_str(), error.c_str());
        v = PlistValue{};
    }
    return g_plists.files.emplace(rel, std::move(v)).first->second;
}

std::u16string u16(const char *ascii) {
    std::u16string s;
    for (; *ascii != '\0'; ascii++) {
        s.push_back(static_cast<char16_t>(*ascii));
    }
    return s;
}

const std::u16string *text_of(mt_cpu *cpu, u32 str) {
    return str != 0 && mt_cf::is_string(cpu, str) ? mt_cf::string_chars(cpu, str) : nullptr;
}

std::string utf8_arg(mt_cpu *cpu, u32 str) { return str != 0 ? string_utf8(cpu, str) : ""; }

u32 autoreleased(mt_cpu *cpu, u32 obj) { return obj != 0 ? mt_objc_autorelease(cpu, obj) : 0u; }

// A plist value as a new guest object (retain count 1); 0 for null and dates (nothing the
// game reads has them).
u32 to_object(mt_cpu *cpu, const PlistValue &v) {
    switch (v.kind) {
    case PlistValue::string:
        return mt_cf::string_create(cpu, v.s, false);
    case PlistValue::boolean:
        return mt_objc_retain(cpu, mt_cf::boolean(cpu, v.b));
    case PlistValue::integer:
        return mt_cf::number_create(cpu, mt_cf::Number{4u, false, v.i, static_cast<double>(v.i)});
    case PlistValue::real:
        return mt_cf::number_create(cpu,
                                    mt_cf::Number{6u, true, static_cast<std::int64_t>(0), v.f});
    case PlistValue::data:
        return mt_cf::data_create(cpu, v.bytes.data(), static_cast<u32>(v.bytes.size()), false);
    case PlistValue::array: {
        std::vector<u32> items;
        for (const PlistValue &item : v.items) {
            const u32 obj = to_object(cpu, item);
            if (obj != 0) {
                items.push_back(obj);
            }
        }
        const u32 a = array_create(cpu, items.data(), static_cast<u32>(items.size()), false);
        for (const u32 obj : items) {
            mt_objc_release(cpu, obj);
        }
        return a;
    }
    case PlistValue::dict: {
        const u32 d = dictionary_create(cpu, false);
        for (const auto &e : v.entries) {
            const u32 key = mt_cf::string_create(cpu, e.first, false);
            const u32 value = to_object(cpu, e.second);
            if (d != 0 && key != 0 && value != 0) {
                dictionary_set(cpu, d, key, value);
            }
            mt_objc_release(cpu, key);
            mt_objc_release(cpu, value);
        }
        return d;
    }
    case PlistValue::null:
    case PlistValue::date:
        break;
    }
    return 0;
}

// --- NSBundle ------------------------------------------------------------------------------------

// +mainBundle
void bundle_main(mt_cpu *cpu) { cpu->r[0] = singleton(cpu, "NSBundle"); }

// -pathForResource:ofType:inDirectory: (CMSoundPlayer::LoadBGM, SoundPlayer_LoadEffects): the
// guest path of <directory>/<name>.<type>, in the bundle or its preferred localization; nil if
// there is none.
void bundle_path_for_resource(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 name = mt_arg_u32(&a), type = mt_arg_u32(&a), dir = mt_arg_u32(&a);
    const std::string n = utf8_arg(cpu, name), t = utf8_arg(cpu, type), d = utf8_arg(cpu, dir);
    const std::string rel = n.empty() ? std::string() : mt_cf::bundle_find(cpu, n, t, d, "");
    if (rel.empty()) {
        log(cpu, "-[NSBundle pathForResource:%s ofType:%s inDirectory:%s]: no such resource: nil",
            n.c_str(), t.c_str(), d.c_str());
        cpu->r[0] = 0;
        return;
    }
    cpu->r[0] = autoreleased(cpu, string_from_utf8(cpu, mt_cf::bundle_guest_path(rel)));
}

// -objectForInfoDictionaryKey: (CStateTitle asks for kCFBundleIdentifierKey): the localized
// value from InfoPlist.strings, else Info.plist's; nil if neither has the key.
void bundle_info_object(mt_cpu *cpu) {
    const u32 key = cpu->r[2];
    const std::u16string *k = text_of(cpu, key);
    if (k == nullptr) {
        cpu->r[0] = 0;
        return;
    }
    const std::u16string want = *k;
    const PlistValue *v = nullptr;
    const std::string strings = mt_cf::bundle_find(cpu, "InfoPlist", "strings", "", "");
    if (!strings.empty()) {
        v = plist(cpu, strings).find(want);
    }
    if (v == nullptr && mt_cf::bundle_has(cpu, "Info.plist")) {
        v = plist(cpu, "Info.plist").find(want);
    }
    cpu->r[0] = v != nullptr ? autoreleased(cpu, to_object(cpu, *v)) : 0u;
}

// -localizedStringForKey:value:table: the string of `key` in <language>.lproj/<table>.strings
// (table nil or empty: Localizable); else `value` if it is not empty, else `key`.
void bundle_localized_string(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 key = mt_arg_u32(&a), value = mt_arg_u32(&a), table = mt_arg_u32(&a);
    const std::u16string *k = text_of(cpu, key);
    const std::u16string *val = text_of(cpu, value);
    if (k == nullptr) {
        cpu->r[0] = value != 0 ? value : autoreleased(cpu, mt_cf::string_create(cpu, u"", false));
        return;
    }
    std::string t = utf8_arg(cpu, table);
    if (t.empty()) {
        t = "Localizable";
    }
    const std::string rel = mt_cf::bundle_find(cpu, t, "strings", "", "");
    const PlistValue *found = rel.empty() ? nullptr : plist(cpu, rel).find(*k);
    if (found != nullptr && found->kind == PlistValue::string) {
        cpu->r[0] = autoreleased(cpu, mt_cf::string_create(cpu, found->s, false));
        return;
    }
    cpu->r[0] = val != nullptr && !val->empty() ? value : key;
}

const mt_objc_host_method k_bundle_instance[] = {
    {"retain", singleton_retain},
    {"release", singleton_release},
    {"retainCount", singleton_retain_count},
    {"pathForResource:ofType:inDirectory:", bundle_path_for_resource},
    {"objectForInfoDictionaryKey:", bundle_info_object},
    {"localizedStringForKey:value:table:", bundle_localized_string},
};
const mt_objc_host_method k_bundle_class[] = {{"mainBundle", bundle_main}};

// --- NSLocale
// --------------------------------------------------------------------------------------

std::string language() { return mt_cf_language(); }

// +currentLocale
void locale_current(mt_cpu *cpu) { cpu->r[0] = singleton(cpu, "NSLocale"); }

// +preferredLanguages: the configured language, then English (the bundle's development
// region) if that is another.
void locale_preferred(mt_cpu *cpu) {
    const std::string lang = language();
    std::vector<u32> items;
    items.push_back(string_from_utf8(cpu, lang));
    if (lang != "en") {
        items.push_back(string_from_utf8(cpu, "en"));
    }
    const u32 a = array_create(cpu, items.data(), static_cast<u32>(items.size()), false);
    for (const u32 s : items) {
        mt_objc_release(cpu, s);
    }
    cpu->r[0] = autoreleased(cpu, a);
}

// -objectForKey: NSLocaleCountryCode ("KR" for Korean, else "US") and NSLocaleLanguageCode; nil
// for any other key.
void locale_object_for_key(mt_cpu *cpu) {
    const u32 key = cpu->r[2];
    const std::u16string *k = text_of(cpu, key);
    std::string answer;
    if (k != nullptr && *k == u16("kCFLocaleCountryCodeKey")) {
        answer = language() == "ko" ? "KR" : "US";
    } else if (k != nullptr && *k == u16("kCFLocaleLanguageCodeKey")) {
        answer = language();
    } else {
        log(cpu, "-[NSLocale objectForKey:%s]: not a key the port answers: nil",
            utf8_arg(cpu, key).c_str());
        cpu->r[0] = 0;
        return;
    }
    cpu->r[0] = autoreleased(cpu, string_from_utf8(cpu, answer));
}

const mt_objc_host_method k_locale_instance[] = {
    {"retain", singleton_retain},
    {"release", singleton_release},
    {"retainCount", singleton_retain_count},
    {"objectForKey:", locale_object_for_key},
};
const mt_objc_host_method k_locale_class[] = {{"currentLocale", locale_current},
                                              {"preferredLanguages", locale_preferred}};

} // namespace

const mt_objc_host_class k_nsbundle = {"NSBundle",
                                       "NSObject",
                                       4,
                                       k_bundle_instance,
                                       MT_COUNT(k_bundle_instance),
                                       k_bundle_class,
                                       MT_COUNT(k_bundle_class)};
const mt_objc_host_class k_nslocale = {"NSLocale",
                                       "NSObject",
                                       4,
                                       k_locale_instance,
                                       MT_COUNT(k_locale_instance),
                                       k_locale_class,
                                       MT_COUNT(k_locale_class)};

std::string home_guest_path() {
    std::string path = mt_cf::config().bundle_path;
    const std::size_t slash = path.find_last_of('/');
    path.resize(slash != std::string::npos && slash > 0 ? slash : 0);
    return path.empty() ? std::string("/") : path;
}

} // namespace mt_ns
