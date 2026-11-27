// Pack checks (#371, ADR 0205, maplethief/packcheck.h): what the three kinds' checks share. The
// report's lines and counters, and the manifest every pack may have, pack.strings (ADR 0149).
#ifndef MAPLETHIEF_RUNTIME_TEXTPACK_PACKCHECK_INTERNAL_H
#define MAPLETHIEF_RUNTIME_TEXTPACK_PACKCHECK_INTERNAL_H

#include <filesystem>
#include <map>
#include <string>

#include "maplethief/packcheck.h"

namespace mt_packcheck {

// The lines of one check, to `fn`, and its counters.
struct Reporter {
    mt_packcheck_fn fn = nullptr;
    void *user = nullptr;
    mt_packcheck_report report{};

    Reporter(mt_packcheck_fn f, void *u, const char *kind) : fn(f), user(u) {
        report.opens = 1;
        report.kind = kind;
    }
    void line(int level, const std::string &text) const {
        if (fn != nullptr) {
            fn(user, level, text.c_str());
        }
    }
    void note(const std::string &text) {
        ++report.notes;
        line(MT_PACKCHECK_NOTE, text);
    }
    void ignored(const std::string &text) {
        ++report.ignored;
        line(MT_PACKCHECK_IGNORED, text);
    }
    void rejected(const std::string &text) {
        ++report.rejected;
        line(MT_PACKCHECK_REJECTED, text);
    }
    // The whole pack is refused: the game opens none of it.
    void refused(const std::string &text) {
        report.opens = 0;
        rejected(text);
    }
};

// The manifest's file name (ADR 0149) and a text pack's older one (ADR 0108).
inline constexpr const char *k_manifest = "pack.strings";
inline constexpr const char *k_text_manifest = "textpack.strings";

// Reads `dir`/pack.strings, if there, by ADR 0149's rules for a pack of `kind` ("texture",
// "audio", "text"). Its entries into *out (empty without one). Refuses the pack in `r` when the
// file breaks a rule; a note when there is none. Returns whether the file is there.
bool check_manifest(const std::filesystem::path &dir, const char *kind, Reporter &r,
                    std::map<std::string, std::string> *out);

// The numeric X.Y.Z of a port version ("1.4.0", "1.4.0-rc.2"): false if it is not one.
bool parse_version(const std::string &v, unsigned out[3]);

std::filesystem::path host_path(const std::string &utf8);
std::string utf8_of(const std::filesystem::path &p);

} // namespace mt_packcheck

#endif // MAPLETHIEF_RUNTIME_TEXTPACK_PACKCHECK_INTERNAL_H
