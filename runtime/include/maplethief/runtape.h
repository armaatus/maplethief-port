/* The run tape (#405, ADR 0156): what reaches the guest from outside, recorded at the point it
 * reaches it, so that a run played in real time can be played again exactly. The game is
 * single-threaded and its clock at boot is its only seed (docs/re/timing-threading.md), so a run
 * is a function of its starting files, the values its clocks returned and the input the host gave
 * it. The tape holds the last two:
 *
 *   streams   every value a guest clock hook returned, in the order it was read, run-length
 *             coded (under display pacing the guest's time only moves at refreshes, so a value
 *             repeats for every read in between):
 *               MT_RUNTAPE_CLOCK   mt_libc_config.clock (gettimeofday, CFAbsoluteTime, UIKit's
 *                                  clock), Unix microseconds
 *               MT_RUNTAPE_GCD     mt_gcd_config.clock (dispatch_time), nanoseconds
 *               MT_RUNTAPE_MUSIC   -[AVAudioPlayer isPlaying]'s answer (0 or 1): a player plays
 *                                  out on the audio device's thread, in real time
 *   events    the host's calls at the guest's boundary, at the run-loop turn they came in
 *             (UIApplicationMain's loop counts the turns from 1; 0 is before the first) and in
 *             their order: mt_input_press / release / release_all, mt_uikit_touch,
 *             mt_uikit_lifecycle_event, mt_runloop_set_fire_tolerance, and the port's own writes
 *             into guest memory (mt_runtape_poke).
 *
 * Recording (mt_runtape_record) wraps whatever clocks are configured: install the platform's
 * clocks first (mt_libc_configure, mt_gcd_configure), then this, before the process loads.
 * Playing (mt_runtape_play) makes the tape the clock: every read takes the stream's next value
 * (past the end, the last one, and mt_runtape_overrun counts it), GCD's sleep returns at once,
 * and the boundary functions above take only the tape's events: the host's own calls are dropped
 * (counted in mt_runtape_stats.dropped). The tape delivers the events of a turn at
 * mt_runtape_deliver, which UIApplicationMain's loop calls before the platform's turn hook.
 *
 * The host's own reads of the time (its waits, its pacing, its overlays) go through
 * mt_runtape_host_time, which is not taped: a replay does not have to make them.
 *
 * Single guest thread, except that GCD's workers read the GCD clock while they hold its baton.
 * C17 that also compiles as C++20; the implementation is runtime/src/runtape/. */
#ifndef MAPLETHIEF_RUNTAPE_H
#define MAPLETHIEF_RUNTAPE_H

#include <stddef.h>
#include <stdint.h>

#include "maplethief/recomp/recomp.h"

MT_EXTERN_C_BEGIN

typedef enum mt_runtape_mode {
    MT_RUNTAPE_OFF = 0,
    MT_RUNTAPE_RECORD = 1,
    MT_RUNTAPE_PLAY = 2
} mt_runtape_mode;

typedef enum mt_runtape_stream {
    MT_RUNTAPE_CLOCK = 0,
    MT_RUNTAPE_GCD = 1,
    MT_RUNTAPE_MUSIC = 2,
    MT_RUNTAPE_STREAMS = 3
} mt_runtape_stream;

/* `count` reads in a row that returned `value`. */
typedef struct mt_runtape_run {
    uint64_t value;
    uint64_t count;
} mt_runtape_run;

typedef enum mt_runtape_kind {
    MT_RUNTAPE_PRESS = 1,          /* a = action */
    MT_RUNTAPE_RELEASE = 2,        /* a = action */
    MT_RUNTAPE_RELEASE_ALL = 3,    /* - */
    MT_RUNTAPE_TOUCH = 4,          /* a = finger, b = phase, x, y (screen points) */
    MT_RUNTAPE_LIFECYCLE = 5,      /* a = event (mt_uikit_lifecycle_event) */
    MT_RUNTAPE_FIRE_TOLERANCE = 6, /* value = the double's bits (seconds) */
    MT_RUNTAPE_POKE = 7,           /* a = guest address, b = length (1-8), value = the bytes (LE) */
    MT_RUNTAPE_KIND_LAST = MT_RUNTAPE_POKE
} mt_runtape_kind;

typedef struct mt_runtape_event {
    uint64_t turn;
    uint32_t kind;
    uint32_t a;
    uint32_t b;
    float x;
    float y;
    uint64_t value;
} mt_runtape_event;

typedef struct mt_runtape_stats {
    uint64_t turn;                      /* the current run-loop turn */
    uint64_t reads[MT_RUNTAPE_STREAMS]; /* reads so far (recorded or played) */
    uint64_t overrun;                   /* reads past a stream's end (play) */
    uint64_t events;                    /* events recorded, or delivered (play) */
    uint64_t dropped;                   /* the host's boundary calls dropped while playing */
} mt_runtape_stats;

/* Starts recording a fresh tape around the clocks configured now. Returns 0. */
int mt_runtape_record(void) MT_NOEXCEPT;
/* Starts playing a tape: the streams' runs and the events (in turn order), copied. Returns 0,
 * or -1 if the events are out of turn order or a kind is unknown. */
int mt_runtape_play(const mt_runtape_run *const runs[MT_RUNTAPE_STREAMS],
                    const size_t run_counts[MT_RUNTAPE_STREAMS], const mt_runtape_event *events,
                    size_t event_count) MT_NOEXCEPT;
/* Back to off: the clocks the recording wrapped are configured again (play: the defaults). The
 * tape stays readable until the next record or play. */
void mt_runtape_stop(void) MT_NOEXCEPT;
mt_runtape_mode mt_runtape_get_mode(void) MT_NOEXCEPT;

/* The recorded (or played) tape: a stream's runs and the events. Valid until the tape changes. */
const mt_runtape_run *mt_runtape_runs(uint32_t stream, size_t *count) MT_NOEXCEPT;
const mt_runtape_event *mt_runtape_events(size_t *count) MT_NOEXCEPT;
mt_runtape_stats mt_runtape_get_stats(void) MT_NOEXCEPT;

/* A new run-loop turn (UIApplicationMain's loop, before it reads the time). */
void mt_runtape_turn(void) MT_NOEXCEPT;
/* Playing: delivers the tape's events up to the current turn (UIApplicationMain's loop, before
 * the turn hook). Otherwise nothing. */
void mt_runtape_deliver(mt_cpu *cpu) MT_NOEXCEPT;

/* The boundary functions' check: 1 if the call goes ahead (off, recording: then it is recorded;
 * playing: only the tape's own delivery), 0 if it is dropped. */
int mt_runtape_admit(const mt_runtape_event *event) MT_NOEXCEPT;
/* -[AVAudioPlayer isPlaying]'s state: recording, `live` taped; playing, the tape's. */
int mt_runtape_music_playing(int live) MT_NOEXCEPT;

/* A port write of `length` (1 to 8) bytes into guest memory at `addr`: done and recorded, or,
 * while playing, dropped (the tape's own is done at its turn). Returns 1 if written. */
int mt_runtape_poke(mt_cpu *cpu, uint32_t addr, const void *bytes, uint32_t length) MT_NOEXCEPT;

/* The guest's CFAbsoluteTime as the host reads it, not taped: the wrapped clock while recording,
 * the last value played while playing, else mt_cf_absolute_time. */
double mt_runtape_host_time(void) MT_NOEXCEPT;
/* The last value the guest read from the MT_RUNTAPE_CLOCK stream (Unix microseconds), recorded
 * or played; 0 when off or before the first read. A recording and its replay see the same value
 * at the same point, so a time measured on it replays exactly (the speedrun timer, #408). */
uint64_t mt_runtape_guest_clock_us(void) MT_NOEXCEPT;

MT_EXTERN_C_END

#endif /* MAPLETHIEF_RUNTAPE_H */
