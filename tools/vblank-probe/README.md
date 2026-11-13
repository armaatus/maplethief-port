# vblank-probe

What macOS tells an app about the display's real vblanks, measured outside the game (#366). It draws a clear colour, never the game, so it needs no IPA.

```sh
xcrun swiftc -O tools/vblank-probe/vblank_probe.swift -o build/vblank-probe
build/vblank-probe 60   # seconds per phase; about 4 minutes in all
```

A small window opens. Keep the session unlocked, the display awake and the window uncovered: otherwise no frame reaches the display and the two tick phases print "no numbers".

It prints one block per phase:

- **info**: the screen's rate facts (NSScreen, the modes' refresh rates, CVDisplayLink's nominal period).
- **cvlink**, **calink**: CVDisplayLink and CADisplayLink (`NSView.displayLink`, macOS 14+). The period fitted to their timestamps, against 60 Hz in ppm, the jitter of the timestamps, how late each callback runs, and the phase between the two links.
- **grid**: ADR 0075's host grid: a 20 Hz tick with 1–9 ms of random work, presented 10 ms into the tick on a grid at the nominal 60 Hz with an arbitrary phase. Each frame's `CAMetalDrawable.presentedTime` gives the refreshes it was really shown for.
- **vblank**: the same tick on a grid taken from CVDisplayLink's period and phase, re-fitted every second.

#366 has the numbers from a locked session; #411 asks for an unlocked and a high-refresh run.
