// M6's exit criteria through the game's own code (#124, ADR 0045, ADR 0046): the recompiled
// game boots from work/ (as runtime.launch_real) to its main menu, which plays track 31, and
// from the run loop's turn hook this test calls the game's sound functions with mt_call_guest
// (docs/re/audio.md): CMSoundPlayer::GetInstance 0x1E099, CMSoundPlayer::Play 0x1D3B9 (this,
// id, resume), CMSoundPlayer::StopAllBGM 0x1CF1D and CStateGame::PlayMapBGM 0x81031. The test
// is the mixer's only reader, so it pulls the mix with mt_audio_render: exact and
// deterministic, no device. The "audio" trace channel (alSourcePlay, -[AVAudioPlayer
// initWithContentsOfURL:error:], -play, -stop, -setCurrentTime:) is captured and parsed.
// Local only: it needs work/gen (python -m recomp all), the slice and the extracted bundle,
// and exits 77 (skipped) without them.
//
//   game_audio_real_test sounds     (audio.game_sounds)
//     For every id 0-43: StopAllBGM, every voice quiet, then Play(id, 0), as the game plays
//     its sounds. An effect (Sound_IsBGM false) must reach alSourcePlay(g_soundSources[id])
//     and play exactly ceil(frames * 2^32 / step) output frames (ADR 0045's 32.32 rule, the
//     WAV's length resampled to 44,100 Hz), not silent. A music id must make the player of
//     sound/<id>.mp3 rewind (setCurrentTime:0) and play; its output is not silent, it still
//     plays a second after the end of its pass (frames x 44,100 / rate output frames), and the
//     second pass's first second equals the first pass's, sample for sample (the loop). The
//     loop point (every track; 31 is the main menu's): the silent run across it (frames that
//     round to 0 in 16 bits) is the pass's own trailing silence, which ends in ADR 0046's 529
//     silent tail frames, plus the pass's own leading silence (the same at the start of both
//     passes), and on macOS it is the silence of Apple's own decode of the file (ExtAudioFile,
//     apple_decode.h: its zero frames at the end and the start) within one MPEG frame. Most
//     files end in seconds of digital silence of their own (31.mp3: 3.2 s), which the device
//     looped too; the port adds nothing to it. One line per id. 42 has no call site in the
//     game (docs/re/audio.md): it is played here through Play like the others.
//
//   game_audio_real_test gapless    (audio.game_gapless_music, #125, ADR 0062)
//     The same for the 18 music ids, with mt_audio_set_gapless_music on before the boot (the
//     settings.ini option gapless_music): every player's trace shows its trim, and the silent
//     run across every loop point is at most one MPEG frame (1,152 output frames: 1,152 at
//     44,100 Hz, 576 resampled at 22,050 Hz), the second pass still identical to the first. On
//     macOS, the frames the trim drops are Apple's own decode's zero frames at the file's end
//     and start, within one MPEG frame.
//
//   game_audio_real_test maps       (audio.game_map_music)
//     For every map file in the bundle's map/ (90, docs/formats/map.md names), in area and map
//     order: area +0x84 and map +0x88 of a game-state block (PlayMapBGM reads only those and
//     clears the flag +0x3FC, per the generated code), then PlayMapBGM, then 0.25 s of mix.
//     From the trace: the one track that plays and whether PlayMapBGM restarted it
//     (setCurrentTime:0) or resumed it, compared with the generated maps table of
//     docs/re/audio.md (mtport.audio's TOWN_BGM / BOSS_MAPS / FIELD_BGM transcription, kept
//     current by tests/test_audio_real.py); a resumed track plays on from the frame of its
//     last stop. Also area 11 (not shipped): effect 9 instead of music.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(MT_HAVE_GENERATED_CODE)

#include "audio/audio_internal.h"
#include "audio/mixer.h"
#include "maplethief/audio.h"
#include "maplethief/boot.h"
#include "maplethief/cf.h"
#include "maplethief/diag.h"
#include "maplethief/gles.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"
#include "maplethief/recomp/dispatch.h"
#include "maplethief/stubs.h"
#include "maplethief/uikit.h"

#include "apple_decode.h"

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace fs = std::filesystem;

// The game's sound code (docs/re/audio.md).
constexpr u32 k_get_instance = 0x1E099;  // CMSoundPlayer::GetInstance()
constexpr u32 k_play = 0x1D3B9;          // CMSoundPlayer::Play(this, id, resume)
constexpr u32 k_stop_all_bgm = 0x1CF1D;  // CMSoundPlayer::StopAllBGM(this)
constexpr u32 k_play_map_bgm = 0x81031;  // CStateGame::PlayMapBGM(this)
constexpr u32 k_sound_player = 0x11A460; // g_soundPlayer
constexpr u32 k_sources = 0x11AAA0;      // g_soundSources[44]
constexpr u32 k_buffers = 0x11AB50;      // g_soundBuffers[44]

// ADR 0046: a pass ends in this many silent frames (of the file's rate).
constexpr u64 k_tail = 529;

int g_failures = 0;
void check(bool ok, const std::string &what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    g_failures += ok ? 0 : 1;
}

std::vector<std::string> g_log;
void capture(void *, const char *line) { g_log.emplace_back(line); }

u32 rd(const mt_cpu *cpu, u32 addr) {
    u32 v;
    std::memcpy(&v, cpu->mem + addr, 4);
    return v;
}
void wr(mt_cpu *cpu, u32 addr, u32 v) { std::memcpy(cpu->mem + addr, &v, 4); }

std::string commas(u64 v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<std::size_t>(i), ",");
    }
    return s;
}

std::string fmt(const char *f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

// A guest function called from the turn hook, as the game calls it; the registers the host's
// caller may still hold are put back.
u32 call(mt_cpu *cpu, u32 fn, std::initializer_list<u32> args) {
    u32 saved[16];
    std::memcpy(saved, cpu->r, sizeof saved);
    const u32 r0 = mt_call_guest(cpu, fn, args.begin(), static_cast<u32>(args.size()), fn);
    std::memcpy(cpu->r, saved, sizeof saved);
    return r0;
}

bool is_bgm(int id) { return id <= 6 || id == 30 || id == 31 || (id >= 34 && id <= 42); }

// --- the trace ----------------------------------------------------------------------------

// The players the trace has seen: player -> (id, rate, frames of a pass).
struct PlayerInfo {
    int id = -1;
    u32 rate = 0;
    u64 frames = 0;
    bool gapless = false;    // the trace shows the gapless trim (#125)
    u64 start = 0, full = 0; // gapless: Apple frames start .. start + frames of full
};

// The gapless mode (mt_audio_set_gapless_music on before the boot).
bool g_gapless = false;
std::map<u32, PlayerInfo> g_players;

// The number (base 10, or 16 with its 0x) right after the first `key` in `l` from `from` on;
// false if there is none.
bool number_after(const std::string &l, const std::string &key, int base, u64 *out,
                  std::size_t from = 0) {
    const auto at = l.find(key, from);
    if (at == std::string::npos) {
        return false;
    }
    const char *start = l.c_str() + at + key.size();
    char *end = nullptr;
    *out = std::strtoull(start, &end, base);
    return end != start;
}

// Parses the trace lines from `from` on: new players, and returns those lines.
std::vector<std::string> lines_since(std::size_t from) {
    std::vector<std::string> out;
    for (std::size_t i = from; i < g_log.size(); i++) {
        const std::string &l = g_log[i];
        // "-[AVAudioPlayer initWithContentsOfURL:<path>/sound/<id>.mp3 error:] -> 0x<player>:
        // <rate> Hz, <channels> channels, <frames> frames"
        u64 id = 0, player = 0, rate = 0, frames = 0;
        if (l.find("-[AVAudioPlayer initWithContentsOfURL:") != std::string::npos &&
            number_after(l, "/sound/", 10, &id) && number_after(l, " error:] -> ", 16, &player) &&
            number_after(l, ": ", 10, &rate, l.find(" error:] -> ") + 12) &&
            number_after(l, " channels, ", 10, &frames)) {
            PlayerInfo info{static_cast<int>(id), static_cast<u32>(rate), frames};
            // "..., <frames> frames, gapless: frames <start> to <end> of <full> (...)"
            const auto g = l.find(", gapless: frames ");
            u64 end = 0;
            info.gapless = g != std::string::npos &&
                           number_after(l, ", gapless: frames ", 10, &info.start, g) &&
                           number_after(l, " to ", 10, &end, g) &&
                           number_after(l, " of ", 10, &info.full, g) &&
                           end == info.start + frames && end <= info.full;
            g_players[static_cast<u32>(player)] = info;
        }
        out.push_back(l);
    }
    return out;
}

// "<prefix>0x%08X..." lines: the players (and the number after "frame ", if any) in order.
std::vector<std::pair<u32, u64>> player_lines(const std::vector<std::string> &lines,
                                              const std::string &prefix) {
    std::vector<std::pair<u32, u64>> out;
    for (const std::string &l : lines) {
        u64 player = 0, n = 0;
        if (!number_after(l, prefix, 16, &player)) {
            continue;
        }
        number_after(l, "frame ", 10, &n, l.find(prefix));
        out.emplace_back(static_cast<u32>(player), n);
    }
    return out;
}

std::vector<u32> source_plays(const std::vector<std::string> &lines) {
    std::vector<u32> out;
    for (const std::string &l : lines) {
        u64 source = 0;
        if (number_after(l, "audio: alSourcePlay(", 10, &source)) {
            out.push_back(static_cast<u32>(source));
        }
    }
    return out;
}

// --- the mix ------------------------------------------------------------------------------

// A frame that a 16-bit capture writes as 0 (ADR 0045: lround(x * 32767)).
bool silent(const float *frame) {
    return std::fabs(frame[0]) * 32767.0f < 0.5f && std::fabs(frame[1]) * 32767.0f < 0.5f;
}

// Renders and drops frames until no voice plays (at most `seconds`); true if it got there.
bool quiesce(double seconds) {
    std::vector<float> buf(2 * 4096);
    for (u64 done = 0; done < static_cast<u64>(seconds * MT_AUDIO_RATE); done += 4096) {
        const mt_audio_output_stats st = mt_audio_get_output_stats();
        if (st.effects_playing == 0 && st.music_playing == 0) {
            return true;
        }
        mt_audio_render(buf.data(), 4096);
    }
    const mt_audio_output_stats st = mt_audio_get_output_stats();
    return st.effects_playing == 0 && st.music_playing == 0;
}

double dbfs(float peak) { return peak > 0 ? 20.0 * std::log10(peak) : -999.0; }

// --- the all-44 run -----------------------------------------------------------------------

struct Totals {
    int effects = 0, effects_ok = 0, music = 0, music_ok = 0;
    u64 effect_frames = 0, music_frames = 0;
};

// An effect id: alSourcePlay of its source, exactly the resampled length, not silent.
void effect(mt_cpu *cpu, u32 sp, int id, Totals &t) {
    std::vector<std::string> bad;
    const auto need = [&bad](bool ok, const std::string &what) {
        if (!ok) {
            bad.push_back(what);
        }
    };
    const u32 source = rd(cpu, k_sources + 4u * static_cast<u32>(id));
    const u32 buffer = rd(cpu, k_buffers + 4u * static_cast<u32>(id));
    const mt_audio::mix::PcmPtr pcm = mt_audio::buffer_pcm(buffer);
    const std::size_t mark = g_log.size();
    call(cpu, k_play, {sp, static_cast<u32>(id), 0});
    const std::vector<std::string> lines = lines_since(mark);
    const std::vector<u32> plays = source_plays(lines);
    need(plays.size() == 1 && plays[0] == source,
         "the trace does not show alSourcePlay(" + std::to_string(source) + ") alone");
    need(player_lines(lines, "-[AVAudioPlayer play] ").empty(), "a player played");
    need(pcm != nullptr && pcm->frames != 0, "its AL buffer holds no samples");
    if (pcm == nullptr || pcm->frames == 0) {
        check(false, std::to_string(id) + ": effect: no samples");
        t.effects++;
        return;
    }
    const u64 step = mt_audio::mix::step_for(pcm->rate);
    const u64 expected = ((static_cast<u64>(pcm->frames) << 32) + step - 1) / step;
    std::vector<float> out(2 * expected + 2);
    mt_audio_render(out.data(), static_cast<u32>(expected));
    const bool playing_at_end = mt_audio::mix::effect_playing(source);
    mt_audio_render(out.data() + 2 * expected, 1);
    const bool playing_after = mt_audio::mix::effect_playing(source);
    float peak = 0;
    u64 sounding = 0, last_sound = 0;
    for (u64 i = 0; i < expected; i++) {
        peak = std::max({peak, std::fabs(out[2 * i]), std::fabs(out[2 * i + 1])});
        if (!silent(&out[2 * i])) {
            sounding++;
            last_sound = i;
        }
    }
    need(playing_at_end && !playing_after && silent(&out[2 * expected]),
         "the voice did not play exactly " + std::to_string(expected) + " frames");
    need(sounding > expected / 4, "mostly silent (" + std::to_string(sounding) + " frames sound)");
    const mt_audio_output_stats st = mt_audio_get_output_stats();
    need(st.music_playing == 0, "music plays");
    t.effects++;
    t.effects_ok += bad.empty() ? 1 : 0;
    t.effect_frames += expected;
    std::string what = std::to_string(id) + ": effect " + std::to_string(id) + ".wav (" +
                       std::to_string(pcm->rate) + " Hz, " + std::to_string(pcm->channels) +
                       " ch, " + commas(pcm->frames) + " frames) -> alSourcePlay(" +
                       std::to_string(source) + "): " + commas(expected) + " output frames (" +
                       fmt("%.3f", static_cast<double>(expected) / MT_AUDIO_RATE) +
                       " s) as the WAV resampled, then silent; " + commas(sounding) +
                       " frames sound, the last at " + commas(last_sound) + ", peak " +
                       fmt("%.1f", dbfs(peak)) + " dBFS";
    for (const std::string &b : bad) {
        what += "; " + b;
    }
    check(bad.empty(), what);
}

// A music id: the player of <id>.mp3 rewound and played; sounds; loops onto its first pass.
void music(mt_cpu *cpu, u32 sp, int id, Totals &t) {
    std::vector<std::string> bad;
    const auto need = [&bad](bool ok, const std::string &what) {
        if (!ok) {
            bad.push_back(what);
        }
    };
    const std::size_t mark = g_log.size();
    call(cpu, k_play, {sp, static_cast<u32>(id), 0});
    const std::vector<std::string> lines = lines_since(mark);
    const auto plays = player_lines(lines, "-[AVAudioPlayer play] ");
    const auto rewinds = player_lines(lines, "-[AVAudioPlayer setCurrentTime:0] ");
    need(source_plays(lines).empty(), "alSourcePlay was called");
    const u32 player = plays.size() == 1 ? plays[0].first : 0;
    const auto info = g_players.find(player);
    need(plays.size() == 1 && info != g_players.end() && info->second.id == id,
         "the trace does not show one -play of the player of " + std::to_string(id) + ".mp3");
    need(rewinds.size() == 1 && rewinds[0].first == player && plays.size() == 1 &&
             plays[0].second == 0,
         "not rewound to frame 0 (setCurrentTime:0) before -play");
    need(rd(cpu, sp + 4) == static_cast<u32>(id), "CMSoundPlayer +0x04 is not the id");
    if (info == g_players.end() || info->second.rate == 0) {
        check(false, std::to_string(id) + ": music: no player");
        t.music++;
        return;
    }
    const PlayerInfo p = info->second;
    const u64 ratio = MT_AUDIO_RATE / p.rate; // 1 or 2: 44,100 or 22,050 Hz
    const u64 pass = p.frames * ratio;        // output frames of a pass
    const u64 second = MT_AUDIO_RATE;
    std::vector<float> first(2 * second), buf(2 * 4096);
    float peak = 0;
    u64 sounding = 0, lead = 0, trail = 0, lead2 = 0, differ = 0;
    bool in_lead = true, in_lead2 = true;
    for (u64 at = 0; at < pass + second;) {
        const u32 n = static_cast<u32>(std::min<u64>(4096, pass + second - at));
        mt_audio_render(buf.data(), n);
        for (u32 i = 0; i < n; i++, at++) {
            const float *f = &buf[2 * i];
            const bool quiet = silent(f);
            if (at < pass) {
                if (at < second) {
                    first[2 * at] = f[0];
                    first[2 * at + 1] = f[1];
                }
                peak = std::max({peak, std::fabs(f[0]), std::fabs(f[1])});
                sounding += quiet ? 0 : 1;
                in_lead = in_lead && quiet;
                lead += in_lead ? 1 : 0;
                trail = quiet ? trail + 1 : 0;
            } else {
                const u64 k = at - pass;
                differ += f[0] != first[2 * k] || f[1] != first[2 * k + 1] ? 1 : 0;
                in_lead2 = in_lead2 && quiet;
                lead2 += in_lead2 ? 1 : 0;
            }
        }
    }
    const mt_audio_output_stats st = mt_audio_get_output_stats();
    need(sounding > pass / 2, "mostly silent (" + commas(sounding) + " frames sound)");
    need(st.music_playing == 1 && mt_audio::mix::music_playing(player),
         "it stopped at the end of its pass");
    need(differ == 0, commas(differ) + " frames of the second pass's first second differ");
    // The loop point: the pass's own trailing silence (ending in the tail) and its own leading
    // silence, and nothing else. Gapless: at most one MPEG frame of silence in all.
    const u64 gap = trail + lead2;
    const u64 frame = (p.rate == MT_AUDIO_RATE ? 1152u : 576u) * ratio; // one MPEG frame
    if (g_gapless) {
        need(p.gapless, "the trace shows no gapless trim");
        need(gap <= frame, "a silent run of " + commas(gap) +
                               " frames across the loop point, more than one MPEG frame (" +
                               commas(frame) + ")");
    } else {
        need(trail >= k_tail * ratio, "fewer than 529 silent frames end the pass");
    }
    need(lead2 == lead, "the second pass starts with other silence than the first");
    const u64 dropped_start = p.start, dropped_end = p.full - p.start - p.frames;
    std::string reference;
#if defined(__APPLE__)
    // The same silence in Apple's own decode of the file (the device's decoder family): its
    // zero frames at the end (the tail included) and at the start, resampled, within one MPEG
    // frame (dr_mp3 and Apple differ by up to 4 LSB, mp3_real_test, so a quiet edge may be 0
    // in one and 1 in the other).
    std::vector<std::int16_t> apple;
    u32 channels = 0;
    const fs::path file = fs::path(MT_BUNDLE_DIR) / "sound" / (std::to_string(id) + ".mp3");
    if (apple_decode::decode(file, &apple, &channels) && channels != 0) {
        const u64 n = apple.size() / channels;
        const auto zero = [&](u64 i) {
            for (u32 c = 0; c < channels; c++) {
                if (apple[i * channels + c] != 0) {
                    return false;
                }
            }
            return true;
        };
        u64 a_trail = 0, a_lead = 0;
        while (a_trail < n && zero(n - 1 - a_trail)) {
            a_trail++;
        }
        while (a_lead < n && zero(a_lead)) {
            a_lead++;
        }
        const u64 expect = (a_trail + a_lead) * ratio;
        if (g_gapless) {
            // what the trim dropped is the file's own silence in Apple's decode, at each end
            const u64 mpeg = frame / ratio;
            const auto near = [mpeg](u64 a, u64 b) { return a + mpeg >= b && a <= b + mpeg; };
            need(n == p.full && a_trail >= k_tail, "Apple's decode has another length or no tail");
            need(near(dropped_end, a_trail) && near(dropped_start, a_lead),
                 "the trim dropped " + commas(dropped_start) + " + " + commas(dropped_end) +
                     " frames, not the file's own silence (Apple: " + commas(a_lead) +
                     " zero frames at the start, " + commas(a_trail) + " at the end)");
        } else {
            need(n == p.frames && a_trail >= k_tail,
                 "Apple's decode has another length or no tail");
            need(gap + frame >= expect && gap <= expect + frame,
                 "the loop point's silence is not the file's own (Apple: " + commas(a_trail) +
                     " zero frames at the end, " + commas(a_lead) + " at the start)");
        }
        reference = "; Apple's decode: " + commas(a_trail) + " + " + commas(a_lead) +
                    " zero frames = " +
                    fmt("%.1f", 1000.0 * static_cast<double>(a_trail + a_lead) / p.rate) + " ms";
    } else {
        need(false, "ExtAudioFile cannot decode " + file.string());
    }
#endif
    t.music++;
    t.music_ok += bad.empty() ? 1 : 0;
    t.music_frames += pass + second;
    char addr[16];
    std::snprintf(addr, sizeof addr, "0x%08X", static_cast<unsigned>(player));
    std::string what =
        std::to_string(id) + ": music " + std::to_string(id) + ".mp3 (" + std::to_string(p.rate) +
        " Hz, pass " + commas(p.frames) +
        " frames = " + fmt("%.2f", static_cast<double>(p.frames) / p.rate) +
        " s) -> setCurrentTime:0, -play " + "on player " + addr + "; " + commas(sounding) +
        " of the pass's frames sound, peak " + fmt("%.1f", dbfs(peak)) +
        " dBFS; still playing 1 s past the end, the second pass's first second identical; loop " +
        "point: " + commas(trail) + " silent frames before" +
        (g_gapless ? std::string() : " (the tail " + std::to_string(k_tail * ratio) + ")") + " + " +
        commas(lead2) +
        " after = " + fmt("%.1f", 1000.0 * static_cast<double>(gap) / MT_AUDIO_RATE) + " ms" +
        reference +
        (g_gapless
             ? "; gapless: " + commas(dropped_start) + " silent frames dropped at the start, " +
                   commas(dropped_end) + " at the end (" +
                   fmt("%.1f", 1000.0 * static_cast<double>(dropped_start + dropped_end) / p.rate) +
                   " ms)"
             : std::string()) +
        (id == 42 ? " (no call site in the game: played directly)" : "");
    for (const std::string &b : bad) {
        what += "; " + b;
    }
    check(bad.empty(), what);
}

void all_sounds(mt_cpu *cpu) {
    const u32 sp = call(cpu, k_get_instance, {});
    check(sp != 0 && sp == rd(cpu, k_sound_player),
          "CMSoundPlayer::GetInstance answers the singleton g_soundPlayer");
    Totals t;
    std::set<int> sounded;
    for (int id = 0; id < 44; id++) {
        call(cpu, k_stop_all_bgm, {sp});
        if (!quiesce(30)) {
            check(false, std::to_string(id) + ": a voice still plays after StopAllBGM");
            continue;
        }
        const int before = g_failures;
        if (is_bgm(id)) {
            music(cpu, sp, id, t);
        } else if (g_gapless) {
            continue; // the gapless run plays the music only
        } else {
            effect(cpu, sp, id, t);
        }
        if (g_failures == before) {
            sounded.insert(id);
        }
    }
    call(cpu, k_stop_all_bgm, {sp});
    check(quiesce(30), "StopAllBGM stops the last track");
    if (!g_gapless) {
        check(t.effects == 26 && t.effects_ok == 26,
              std::to_string(t.effects_ok) + " of " + std::to_string(t.effects) +
                  " effects play their whole WAV through alSourcePlay (" +
                  fmt("%.2f", static_cast<double>(t.effect_frames) / MT_AUDIO_RATE) + " s)");
    }
    check(t.music == 18 && t.music_ok == 18,
          std::to_string(t.music_ok) + " of " + std::to_string(t.music) +
              " music tracks play through AVAudioPlayer and loop" +
              (g_gapless ? " gapless (at most one MPEG frame of silence at the loop point)" : "") +
              " (" + fmt("%.2f", static_cast<double>(t.music_frames) / MT_AUDIO_RATE) +
              " s rendered)");
    const std::size_t ids = g_gapless ? 18 : 44;
    check(sounded.size() == ids, std::to_string(sounded.size()) + " of " + std::to_string(ids) +
                                     " ids sound through CMSoundPlayer::Play");
}

// --- the per-map run ----------------------------------------------------------------------

struct Expect {
    int id = -1;
    bool restart = false;
};

// The generated maps table of docs/re/audio.md: (area, map) -> (id, restart).
std::map<std::pair<int, int>, Expect> doc_maps(const fs::path &doc) {
    std::map<std::pair<int, int>, Expect> out;
    std::ifstream in(doc);
    std::string line;
    bool inside = false;
    while (std::getline(in, line)) {
        if (line.rfind("<!-- BEGIN generated:maps", 0) == 0) {
            inside = true;
        } else if (line.rfind("<!-- END generated", 0) == 0) {
            inside = false;
        } else if (inside && line.size() > 2 && line[0] == '|' && line[2] >= '0' &&
                   line[2] <= '9') {
            const int area = std::atoi(line.c_str() + 2);
            const auto last = line.rfind('|', line.size() - 2);
            std::stringstream cells(line.substr(last + 1));
            std::string cell;
            while (std::getline(cells, cell, ',')) {
                // "<map>: <id>" or "<map>: <id> r" (the last one followed by " |")
                std::istringstream parts(cell);
                int map = -1, id = -1;
                char colon = 0;
                std::string flag;
                if (parts >> map >> colon >> id && colon == ':') {
                    parts >> flag;
                    out[{area, map}] = Expect{id, flag == "r"};
                }
            }
        }
    }
    return out;
}

// (area, map) of a map/<area><map>.map name (docs/formats/map.md, mtport.audio's
// split_map_name): a 3-digit name starting with 10 is area 10.
bool split_map_name(const std::string &stem, std::pair<int, int> *out) {
    if ((stem.size() != 2 && stem.size() != 3) ||
        !std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    if (stem.size() == 3 && stem.rfind("10", 0) == 0) {
        *out = {10, stem[2] - '0'};
    } else {
        *out = {stem[0] - '0', std::atoi(stem.c_str() + 1)};
    }
    return true;
}

void map_music(mt_cpu *cpu) {
    const auto expected = doc_maps(MT_AUDIO_DOC);
    std::set<std::pair<int, int>> shipped;
    bool names_ok = true;
    for (const auto &e : fs::directory_iterator(fs::path(MT_BUNDLE_DIR) / "map")) {
        if (e.path().extension() == ".map") {
            std::pair<int, int> am;
            names_ok = split_map_name(e.path().stem().string(), &am) && names_ok;
            shipped.insert(am);
        }
    }
    std::set<std::pair<int, int>> documented;
    for (const auto &[am, x] : expected) {
        documented.insert(am);
    }
    check(names_ok && shipped == documented && shipped.size() == 90,
          std::to_string(shipped.size()) + " map files in the bundle's map/, the " +
              std::to_string(documented.size()) + " maps of docs/re/audio.md's generated table");

    const u32 sp = call(cpu, k_get_instance, {});
    mt_mem *mem = mt_process_mem(mt_process_current());
    const u32 state = mt_heap_calloc(mem, 1, 0x400); // the CStateGame fields PlayMapBGM reads
    call(cpu, k_stop_all_bgm, {sp});
    check(quiesce(30), "StopAllBGM: nothing plays before the first map");
    std::map<u32, u64> stopped_at; // player -> stream frame at its last -stop
    std::vector<float> buf(2 * 4096);
    int matched = 0, restarts = 0, resumes = 0, resumed_on = 0;
    std::map<int, int> per_track;
    for (const auto &am : shipped) {
        const auto x = expected.find(am);
        wr(cpu, state + 0x84, static_cast<u32>(am.first));
        wr(cpu, state + 0x88, static_cast<u32>(am.second));
        cpu->mem[state + 0x3FC] = 1;
        const std::size_t mark = g_log.size();
        call(cpu, k_play_map_bgm, {state});
        const std::vector<std::string> lines = lines_since(mark);
        for (const auto &[player, frame] : player_lines(lines, "-[AVAudioPlayer stop] ")) {
            stopped_at[player] = frame;
        }
        const auto plays = player_lines(lines, "-[AVAudioPlayer play] ");
        const auto rewinds = player_lines(lines, "-[AVAudioPlayer setCurrentTime:0] ");
        const u32 player = plays.size() == 1 ? plays[0].first : 0;
        const auto info = g_players.find(player);
        const int got = info != g_players.end() ? info->second.id : -1;
        const bool restart = !rewinds.empty();
        std::vector<std::string> bad;
        if (plays.size() != 1 || !source_plays(lines).empty()) {
            bad.push_back(std::to_string(plays.size()) + " -play, " +
                          std::to_string(source_plays(lines).size()) + " alSourcePlay");
        }
        if (x == expected.end() || got != x->second.id || restart != x->second.restart) {
            bad.push_back("expected " +
                          (x == expected.end()
                               ? std::string("?")
                               : std::to_string(x->second.id) +
                                     (x->second.restart ? " restarted" : " resumed")));
        }
        if (restart &&
            (rewinds.size() != 1 || rewinds[0].first != player || plays[0].second != 0)) {
            bad.push_back("the restart is not setCurrentTime:0 then -play at frame 0");
        }
        if (!restart && plays.size() == 1) {
            const auto s = stopped_at.find(player);
            if (s != stopped_at.end()) {
                resumed_on++;
                if (plays[0].second != s->second) {
                    bad.push_back("resumed at frame " + std::to_string(plays[0].second) +
                                  ", stopped at " + std::to_string(s->second));
                }
            }
        }
        if (cpu->mem[state + 0x3FC] != 0) {
            bad.push_back("+0x3FC not cleared");
        }
        // A quarter of a second of the map's music, so a resumed track has moved on.
        for (u32 done = 0; done < MT_AUDIO_RATE / 4; done += 4096) {
            mt_audio_render(buf.data(), std::min<u32>(4096, MT_AUDIO_RATE / 4 - done));
        }
        const mt_audio_output_stats st = mt_audio_get_output_stats();
        if (st.music_playing != 1 || !mt_audio::mix::music_playing(player)) {
            bad.push_back(std::to_string(st.music_playing) + " music voices play");
        }
        std::string what = "map (" + std::to_string(am.first) + ", " + std::to_string(am.second) +
                           "): " + std::to_string(got) + (restart ? " restarted" : " resumed");
        if (plays.size() == 1) {
            what += " (-play at frame " + commas(plays[0].second) + ")";
        }
        for (const std::string &b : bad) {
            what += "; " + b;
        }
        check(bad.empty(), what);
        matched += bad.empty() ? 1 : 0;
        restarts += restart ? 1 : 0;
        resumes += restart ? 0 : 1;
        per_track[got]++;
    }
    std::string tracks;
    for (const auto &[id, n] : per_track) {
        tracks += (tracks.empty() ? "" : ", ") + std::to_string(id) + " x" + std::to_string(n);
    }
    check(matched == 90, std::to_string(matched) +
                             " of 90 maps play the transcription's track, restarted or resumed "
                             "as it says (" +
                             std::to_string(restarts) + " restarts, " + std::to_string(resumes) +
                             " resumes, " + std::to_string(resumed_on) +
                             " of them on from the frame of the track's last stop; tracks " +
                             tracks + ")");

    // An area outside 0-10 (none ships): effect 9, the warp sound, instead of music.
    wr(cpu, state + 0x84, 11);
    wr(cpu, state + 0x88, 0);
    const std::size_t mark = g_log.size();
    call(cpu, k_play_map_bgm, {state});
    const std::vector<std::string> lines = lines_since(mark);
    const std::vector<u32> sources = source_plays(lines);
    check(sources.size() == 1 && sources[0] == rd(cpu, k_sources + 4 * 9) &&
              player_lines(lines, "-[AVAudioPlayer play] ").empty(),
          "area 11 (not shipped), map 0: alSourcePlay of effect 9's source, no music");
    call(cpu, k_stop_all_bgm, {sp});
    mt_heap_free(mem, state);
}

// --- the boot and the turn hook -----------------------------------------------------------

std::string g_mode;
int g_turns = 0;
double g_first = 0;
bool g_done = false, g_menu_music = false;

// After every turn of the main run loop: once the main menu plays its track 31 (or after 20 s
// of it), the run's checks, then the end of the run.
int turn(void *, mt_cpu *cpu, double now, double next) {
    if (g_turns++ == 0) {
        g_first = now;
    }
    bool menu = false;
    for (const auto &[player, frame] : player_lines(lines_since(0), "-[AVAudioPlayer play] ")) {
        const auto p = g_players.find(player);
        menu = menu || (p != g_players.end() && p->second.id == 31);
    }
    if (!menu && now - g_first < 20) {
        const double wait = std::min(std::max(0.0, next - mt_cf_absolute_time()), 0.02);
        std::this_thread::sleep_for(std::chrono::duration<double>(wait));
        return 0;
    }
    g_menu_music = menu;
    check(menu, "the main menu plays track 31 (the game's own CMSoundPlayer::Play, " +
                    fmt("%.1f", now - g_first) + " s after the first turn)");
    const mt_audio_output_stats st = mt_audio_get_output_stats();
    check(st.frames == 0, "nothing pulled the mixer before the test (it is the only reader)");
    if (g_mode == "sounds" || g_mode == "gapless") {
        all_sounds(cpu);
    } else {
        map_music(cpu);
    }
    g_done = true;
    return 1;
}

int run(const std::vector<std::uint8_t> &slice) {
    mt_cf_config cf = mt_cf_config_default();
    cf.bundle_dir = MT_BUNDLE_DIR;
    mt_cf_configure(&cf);
    const fs::path data = fs::temp_directory_path() / ("maplethief-game-audio-" + g_mode);
    fs::remove_all(data);
    fs::create_directories(data);
    mt_stubs_set_data_dir(data.string().c_str());
    mt_diag_config diag = mt_diag_config_default();
    diag.report_path = nullptr;
    diag.trace = "audio";
    mt_diag_configure(&diag);
    mt_gles_config gl{};
    mt_gles_configure(&gl);
    mt_uikit_config ui = mt_uikit_config_default();
    ui.turn = turn;
    mt_boot_config cfg = mt_boot_config_default();
    cfg.reservation = MT_MEM_RESERVE_FOR_BUILD;
    cfg.imports.unimplemented = MT_UNIMPLEMENTED_STOP;
    cfg.imports.log = capture;
    cfg.app_main = mt_uikit_app_main;
    cfg.app_user = &ui;
    mt_program prog = MT_PROGRAM_GENERATED;
    mt_audio_reset_output_stats();
    g_gapless = g_mode == "gapless";
    mt_audio_set_gapless_music(g_gapless); // before the game's LoadBGM opens its players
    mt_process *p = mt_process_load(&cfg, &prog, slice.data(), slice.size());
    if (p == nullptr) {
        std::printf("FAIL: the boot could not load the slice\n");
        return 1;
    }
    const auto started = std::chrono::steady_clock::now();
    const std::uint32_t outcome = mt_process_run(p);
    const mt_process_result *r = mt_process_result_of(p);
    if (outcome == MT_PROCESS_TRAPPED) {
        std::printf("trap kind %u at pc 0x%08X detail 0x%08X\n", r->trap_kind, r->trap_pc,
                    r->trap_detail);
    }
    check(
        outcome == MT_PROCESS_STOPPED && g_done,
        "the run reached the main menu and its checks, then ended from the turn hook (" +
            fmt("%.1f",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()) +
            " s)");
    const mt_audio_output_stats st = mt_audio_get_output_stats();
    std::printf("mixer: %s frames rendered in %s calls, longest %u us\n", commas(st.frames).c_str(),
                commas(st.renders).c_str(), st.longest_render_us);
    mt_process_destroy(p);
    mt_gles_configure(nullptr);
    mt_cf_configure(nullptr);
    mt_stubs_set_data_dir(nullptr);
    mt_audio_set_gapless_music(false);
    fs::remove_all(data);
    return 0;
}

std::vector<std::uint8_t> read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

int main(int argc, char **argv) {
    g_mode = argc > 1 ? argv[1] : "";
    if (g_mode != "sounds" && g_mode != "maps" && g_mode != "gapless") {
        std::printf("usage: game_audio_real_test sounds|maps|gapless\n");
        return 2;
    }
    if (!fs::is_regular_file(MT_ARMV7_SLICE) ||
        !fs::is_directory(fs::path(MT_BUNDLE_DIR) / "sound")) {
        std::printf("skipped: %s or %s absent (scripts/bootstrap)\n", MT_ARMV7_SLICE,
                    MT_BUNDLE_DIR);
        return 77;
    }
    const std::vector<std::uint8_t> slice = read_file(MT_ARMV7_SLICE);
    if (run(slice) != 0) {
        return 1;
    }
    std::printf("%s\n", g_failures == 0 ? "all ok" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}

#else

int main() {
    std::printf("skipped: built without generated code (work/gen: python -m recomp all)\n");
    return 77;
}

#endif
