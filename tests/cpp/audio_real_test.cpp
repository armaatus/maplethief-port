// ExtAudioFile on the real bundle (#62), local only: skipped (exit 77) without work/ipa/
// (scripts/bootstrap).
//
// Every sound/<id>.wav goes through the calls of the game's MyGetOpenALAudioData (0xA0045,
// docs/re/audio.md) as CMSoundPlayer makes them for ids 0-43: -[NSBundle
// pathForResource:@"<id>" ofType:@"wav" inDirectory:@"sound"], -[NSURL initFileURLWithPath:],
// ExtAudioFileOpenURL, 'ffmt', 'cfmt' (16-bit signed packed at the file's channels and rate),
// '#frm', one ExtAudioFileRead of all the frames, ExtAudioFileDispose, then alBufferData with
// AL_FORMAT_MONO16 / STEREO16 into one of 44 buffers. Checks, per file: the frame count ('#frm'
// and the frames read) and the decoded bytes equal the data chunk of a strict, independent RIFF
// walk (wav_reference.h, mtport.audio's rules); the format, duration and file size agree with
// mtport.audio's generated table in docs/re/audio.md; a second read is the end; the AL buffer
// holds the same samples, with no AL error. Over all: the 26 effect ids are exactly the ids
// with a WAV, and 13 files carry chunks after "data".
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "audio/audio_internal.h"
#include "audio/mixer.h"
#include "foundation/foundation.h"
#include "maplethief/audio.h"
#include "maplethief/cf.h"
#include "maplethief/hle/imports.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"

#include "wav_reference.h"

namespace {

using u32 = std::uint32_t;
namespace fs = std::filesystem;

int g_failures = 0;
void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
}

void ignore_log(void *, const char *) {}

struct World {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 pool = 0;
    World() {
        mem = mt_mem_create(nullptr);
        mt_mem_attach(mem, &cpu);
        mt_stack_create(mem, 0, &stack);
        cpu.r[MT_SP] = stack.top;
        cpu.r[MT_LR] = 0x2345u;
        mt_import_config cfg = mt_import_config_default();
        cfg.log = ignore_log;
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        mt_objc_add_host_classes(rt);
        mt_cf_start(&cpu, rt);
        pool = mt_objc_pool_push(rt);
    }
    ~World() {
        mt_objc_pool_pop(rt, pool);
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
    }
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    u32 send(u32 obj, const char *sel, std::initializer_list<u32> args = {}) {
        return mt_objc_msg(&cpu, obj, mt_objc_sel(rt, sel), args.begin(),
                           static_cast<u32>(args.size()));
    }
    u32 str(const std::string &s) { return mt_ns::string_from_utf8(&cpu, s); }
    // An HLE import with word arguments (those past r3 on the guest stack).
    u32 call(mt_import_fn fn, std::initializer_list<u32> args) {
        const u32 sp = cpu.r[MT_SP];
        const u32 stacked = args.size() > 4 ? static_cast<u32>(args.size()) - 4u : 0u;
        cpu.r[MT_SP] = sp - 8u * ((stacked + 1u) / 2u);
        u32 i = 0;
        for (u32 a : args) {
            if (i < 4) {
                cpu.r[i] = a;
            } else {
                std::memcpy(cpu.mem + cpu.r[MT_SP] + 4u * (i - 4u), &a, 4);
            }
            i++;
        }
        fn(&cpu, 0);
        cpu.r[MT_SP] = sp;
        return cpu.r[0];
    }
    u32 alloc(u32 bytes) { return mt_heap_calloc(mem, 1, bytes); }
    void free(u32 p) { mt_heap_free(mem, p); }
    u32 get32(u32 addr) const {
        u32 v;
        std::memcpy(&v, cpu.mem + addr, 4);
        return v;
    }
    void put32(u32 addr, u32 v) { std::memcpy(cpu.mem + addr, &v, 4); }
};

// One row of the generated files table of docs/re/audio.md (mtport.audio generate).
struct DocRow {
    std::string file, klass, format, rate, channels, bits, duration, bytes;
};
std::map<int, DocRow> doc_rows(const fs::path &doc) {
    std::map<int, DocRow> rows;
    std::ifstream in(doc);
    std::string line;
    bool inside = false;
    while (std::getline(in, line)) {
        if (line.rfind("<!-- BEGIN generated:files", 0) == 0) {
            inside = true;
        } else if (line.rfind("<!-- END generated", 0) == 0) {
            inside = false;
        } else if (inside && line.size() > 2 && line[0] == '|' && line[2] >= '0' &&
                   line[2] <= '9') {
            std::vector<std::string> cells;
            std::stringstream ss(line.substr(1));
            std::string cell;
            while (std::getline(ss, cell, '|')) {
                const auto a = cell.find_first_not_of(' '), b = cell.find_last_not_of(' ');
                cells.push_back(a == std::string::npos ? "" : cell.substr(a, b - a + 1));
            }
            if (cells.size() >= 9) {
                rows[std::stoi(cells[0])] = {cells[1], cells[2], cells[3], cells[4],
                                             cells[5], cells[6], cells[7], cells[8]};
            }
        }
    }
    return rows;
}

std::string with_commas(std::uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<std::size_t>(i), ",");
    }
    return s;
}

constexpr u32 k_ffmt = 0x66666D74u, k_cfmt = 0x63666D74u, k_frm = 0x2366726Du;
constexpr u32 k_lpcm = 0x6C70636Du;
constexpr u32 k_al_buffer_mono16 = 0x1101u, k_al_buffer_stereo16 = 0x1103u;

bool is_bgm(int id) { return id <= 6 || id == 30 || id == 31 || (id >= 34 && id <= 42); }

struct Totals {
    int files = 0, decoded = 0, trailing = 0;
    std::uint64_t frames = 0;
};

// MyGetOpenALAudioData and alBufferData for one id; true if every check holds.
bool decode(World &w, const fs::path &app, int id, const DocRow *row, u32 buffer, Totals *totals) {
    const std::string name = std::to_string(id);
    const wav_reference::Wav ref = wav_reference::read(app / "sound" / (name + ".wav"));
    std::vector<std::string> bad;
    const auto need = [&bad](bool ok, const std::string &what) {
        if (!ok) {
            bad.push_back(what);
        }
    };
    need(ref.error.empty(), "the reference reader: " + ref.error);
    need(ref.tag == 1 && ref.bits == 16, "the file is no 16-bit PCM WAV");
    bool after_data = false;
    for (std::size_t i = 0; i + 1 < ref.chunks.size(); i++) {
        after_data = after_data || ref.chunks[i] == "data";
    }
    totals->trailing += after_data ? 1 : 0;

    const u32 pool = mt_objc_pool_push(w.rt);
    const u32 bundle = w.send(mt_objc_class_named(w.rt, "NSBundle"), "mainBundle");
    const u32 path = w.send(
        bundle, "pathForResource:ofType:inDirectory:", {w.str(name), w.str("wav"), w.str("sound")});
    need(path != 0, "pathForResource:ofType:inDirectory: answered nil");
    const u32 url =
        w.send(w.send(mt_objc_class_named(w.rt, "NSURL"), "alloc"), "initFileURLWithPath:", {path});
    const u32 out = w.alloc(4), size = w.alloc(4), asbd = w.alloc(40), frames64 = w.alloc(8),
              list = w.alloc(16), io = w.alloc(4);
    u32 status = w.call(mt_hle_ExtAudioFileOpenURL, {url, out});
    need(status == 0, "ExtAudioFileOpenURL: " + std::to_string(static_cast<std::int32_t>(status)));
    const u32 file = w.get32(out);
    // the file format
    w.put32(size, 40);
    status = w.call(mt_hle_ExtAudioFileGetProperty, {file, k_ffmt, size, asbd});
    double rate = 0;
    std::memcpy(&rate, w.cpu.mem + asbd, 8);
    const u32 channels = w.get32(asbd + 28);
    need(status == 0 && w.get32(asbd + 8) == k_lpcm && rate == ref.rate &&
             channels == ref.channels && w.get32(asbd + 32) == 16,
         "'ffmt' is not the file's format");
    // the client format: lpcm, flags 0xC, 16 bits, the file's channels and rate
    w.put32(asbd + 12, 0xC);
    w.put32(asbd + 16, 2 * channels);
    w.put32(asbd + 20, 1);
    w.put32(asbd + 24, 2 * channels);
    w.put32(asbd + 32, 16);
    status = w.call(mt_hle_ExtAudioFileSetProperty, {file, k_cfmt, 40, asbd});
    need(status == 0, "'cfmt': " + std::to_string(static_cast<std::int32_t>(status)));
    // the length, then one read of it all
    w.put32(size, 8);
    status = w.call(mt_hle_ExtAudioFileGetProperty, {file, k_frm, size, frames64});
    std::uint64_t length = 0;
    std::memcpy(&length, w.cpu.mem + frames64, 8);
    need(status == 0 && length == ref.frames(), "'#frm' " + std::to_string(length) +
                                                    ", the data chunk has " +
                                                    std::to_string(ref.frames()) + " frames");
    const u32 bytes = static_cast<u32>(length) * 2u * channels;
    const u32 pcm = w.alloc(bytes + 2);
    w.put32(list, 1);
    w.put32(list + 4, channels);
    w.put32(list + 8, bytes);
    w.put32(list + 12, pcm);
    w.put32(io, static_cast<u32>(length));
    status = w.call(mt_hle_ExtAudioFileRead, {file, io, list});
    const u32 got = w.get32(io);
    need(status == 0 && got == ref.frames() && w.get32(list + 8) == bytes,
         "ExtAudioFileRead: " + std::to_string(got) + " frames, " +
             std::to_string(w.get32(list + 8)) + " bytes");
    need(bytes <= ref.data.size() && std::memcmp(w.cpu.mem + pcm, ref.data.data(), bytes) == 0,
         "the decoded bytes are not the data chunk's");
    w.put32(io, 1000);
    w.put32(list + 8, bytes);
    status = w.call(mt_hle_ExtAudioFileRead, {file, io, list});
    need(status == 0 && w.get32(io) == 0 && w.get32(list + 8) == 0,
         "a read past the end gave frames");
    need(w.call(mt_hle_ExtAudioFileDispose, {file}) == 0, "ExtAudioFileDispose");
    // alBufferData, as SoundPlayer_LoadEffects
    w.call(mt_hle_alBufferData, {buffer, channels == 1 ? k_al_buffer_mono16 : k_al_buffer_stereo16,
                                 pcm, bytes, static_cast<u32>(rate)});
    need(w.call(mt_hle_alGetError, {}) == 0, "an AL error after alBufferData");
    const mt_audio::mix::PcmPtr held = mt_audio::buffer_pcm(buffer);
    need(held != nullptr && held->frames == ref.frames() && held->channels == channels &&
             held->rate == ref.rate &&
             std::memcmp(held->samples.data(), ref.data.data(), bytes) == 0,
         "the AL buffer does not hold the decoded samples");
    // the generated table of docs/re/audio.md (mtport.audio)
    char duration[32];
    std::snprintf(duration, sizeof duration, "%.2f",
                  static_cast<double>(ref.frames()) / static_cast<double>(ref.rate));
    need(row != nullptr && row->file == "`" + name + ".wav`" && row->klass == "SFX" &&
             row->format == "WAV PCM" && row->rate == std::to_string(ref.rate) &&
             row->channels == std::to_string(ref.channels) && row->bits == "16-bit" &&
             row->duration == duration && row->bytes == with_commas(ref.file_size),
         "docs/re/audio.md's row differs");
    for (u32 p : {out, size, asbd, frames64, list, io, pcm}) {
        w.free(p);
    }
    mt_objc_pool_pop(w.rt, pool);

    totals->files++;
    totals->frames += got;
    totals->decoded += bad.empty() ? 1 : 0;
    std::string what = name + ".wav: " + std::to_string(ref.rate) + " Hz, " +
                       std::to_string(ref.channels) + " ch, " + std::to_string(got) + " frames (" +
                       duration + " s) decoded, '#frm' " + std::to_string(length) + ", AL buffer " +
                       std::to_string(buffer) + (after_data ? ", chunks after data" : "");
    for (const std::string &b : bad) {
        what += "; " + b;
    }
    check(bad.empty(), what);
    return bad.empty();
}

} // namespace

// No guest code runs here.
extern "C" mt_guest_fn mt_dispatch(mt_cpu *cpu, std::uint32_t target, std::uint32_t site) noexcept {
    mt_trap(cpu, MT_TRAP_NO_FUNCTION, site, target);
}

int main() {
    const fs::path app = MT_BUNDLE_DIR;
    if (!fs::is_directory(app / "sound")) {
        std::printf("skipped: %s absent (run scripts/bootstrap)\n", MT_BUNDLE_DIR);
        return 77;
    }
    const std::map<int, DocRow> rows = doc_rows(MT_AUDIO_DOC);
    check(rows.size() == 44, "docs/re/audio.md: " + std::to_string(rows.size()) +
                                 " rows in the generated files table");
    mt_cf_config cf = mt_cf_config_default();
    const std::string dir = app.string();
    cf.bundle_dir = dir.c_str();
    mt_cf_configure(&cf);
    {
        World w;
        // SoundPlayer_InitOpenAL: a device, a context, 44 buffers
        const u32 device = w.call(mt_hle_alcOpenDevice, {0});
        const u32 context = w.call(mt_hle_alcCreateContext, {device, 0});
        w.call(mt_hle_alcMakeContextCurrent, {context});
        const u32 names = w.alloc(4 * 44);
        w.call(mt_hle_alGenBuffers, {44, names});
        check(w.call(mt_hle_alGetError, {}) == 0, "44 AL buffers");
        Totals totals;
        std::vector<int> wav_ids, sfx_ids;
        for (int id = 0; id < 44; id++) {
            if (!is_bgm(id)) {
                sfx_ids.push_back(id);
            }
            if (fs::is_regular_file(app / "sound" / (std::to_string(id) + ".wav"))) {
                wav_ids.push_back(id);
                const auto row = rows.find(id);
                decode(w, app, id, row == rows.end() ? nullptr : &row->second,
                       w.get32(names + 4u * static_cast<u32>(id)), &totals);
            }
        }
        check(wav_ids == sfx_ids && wav_ids.size() == 26,
              std::to_string(wav_ids.size()) +
                  " WAVs, at exactly the ids Sound_IsBGM calls effects");
        check(totals.decoded == 26 && totals.files == 26,
              std::to_string(totals.decoded) + " of " + std::to_string(totals.files) +
                  " WAVs decoded with the right frame counts and bytes, " +
                  std::to_string(totals.frames) + " frames in all");
        check(totals.trailing == 13,
              std::to_string(totals.trailing) + " WAVs carry chunks after data (ignored)");
        const mt_audio_stats st = mt_audio_get_stats();
        check(st.files_opened == 26 && st.files_failed == 0 && st.buffers_filled == 26,
              "mt_audio stats: " + std::to_string(st.files_opened) + " opened, " +
                  std::to_string(st.files_failed) + " failed, " +
                  std::to_string(st.buffers_filled) + " buffers filled");
    }
    mt_cf_configure(nullptr);
    std::printf("%s\n", g_failures == 0 ? "all ok" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
