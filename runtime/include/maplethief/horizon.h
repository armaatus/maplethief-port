/* What Horizon, the Nintendo Switch's system, does not have yet in this port (#235, ADR 0138).
 *
 * devkitA64's newlib has no mmap, no POSIX signals, no dladdr and no child processes. Where our
 * code needs one, its __SWITCH__ branch is a stub that says so at run time, loudly, instead of
 * pretending to work: MT_HORIZON_MISSING prints the reason to stderr (nxlink's console with
 * scripts/switch-run) and the caller then fails the way it would on any error;
 * MT_HORIZON_FATAL prints it and aborts, for the paths that cannot fail softly. #236 (the runtime
 * on Horizon) replaced every stub #235 left with libnx's equivalents, so nothing uses these
 * now; a later gap does the same, and lists itself on its issue.
 *
 * C and C++. Only meaningful in __SWITCH__ code; empty elsewhere. */
#ifndef MAPLETHIEF_HORIZON_H
#define MAPLETHIEF_HORIZON_H

#if defined(__SWITCH__)
#include <stdio.h>
#include <stdlib.h>

#define MT_HORIZON_MESSAGE(what)                                                                   \
    fprintf(stderr, "MapleThief: not on the Nintendo Switch yet: %s (%s:%d)\n", (what), __FILE__,  \
            __LINE__)
#define MT_HORIZON_MISSING(what) ((void)MT_HORIZON_MESSAGE(what))
#define MT_HORIZON_FATAL(what) ((void)MT_HORIZON_MESSAGE(what), abort())
#endif

#endif /* MAPLETHIEF_HORIZON_H */
