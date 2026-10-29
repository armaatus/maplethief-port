# 38. Boot to the first GL frame: OpenGL ES recorded through a state tracker, EAGLContext, silent audio, real CommonCrypto digests

- **Status:** Accepted
- **Date:** 2026-10-29
- **Issue:** #117
- **Builds on:** [ADR 0026](0026-objective-c-strategy.md) (host classes), [ADR 0027](0027-process-start-up.md) (boot milestones, `mt_process_stop`), [ADR 0030](0030-runtime-diagnostics.md) (the unimplemented report, trace channels), [ADR 0035](0035-foundation-subset.md) (NSURL, the guest home), [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (`CAEAGLLayer`, the GL slot, the run loop's turn hook)

## Context

M4's exit criterion: the recompiled game boots through `UIApplicationMain` and the app delegate, creates its EAGL context and issues its **first GL frame**, with no unimplemented import or selector on the way. Until now `runtime.boot_real` reached the main menu only with test stand-ins for everything that is not M4's: EAGL, the GL imports, audio and `CC_MD5`.

What the boot path reaches ([re/imports.md](../re/imports.md), [re/host-surface.md](../re/host-surface.md), [re/audio.md](../re/audio.md)):

- **OpenGL ES 1.1:** 31 functions (24 core, 7 `GL_OES_framebuffer_object`). `-[EAGLView createGLes]` makes one framebuffer and one colour renderbuffer, checks `glCheckFramebufferStatusOES` and reads the renderbuffer's size back with `glGetRenderbufferParameterivOES`. Textures are made with `glGenTextures`, and the text cache asks `glIsTexture` before it draws a cached string texture. No `glClear`: every frame overdraws the screen.
- **EAGL:** `EAGLContext` `+alloc`, `-initWithAPI:` (ES 1), `+setCurrentContext:`, `-renderbufferStorage:fromDrawable:` (the view's `CAEAGLLayer`, `drawableProperties` RGBA8 with no retained backing), `-presentRenderbuffer:` once per frame from `-[EAGLView flushLCD]`; the three `kEAGL*` string constants.
- **Audio:** `CMSoundPlayer` opens an OpenAL device and context, makes 44 buffers and 44 sources, decodes the 26 WAV effects with `ExtAudioFile` into the buffers and checks `alGetError` after each step: any error at start-up ends the app with `exit(1)`. `AudioSessionInitialize`/`SetProperty`/`SetActive`, `AVAudioSession` (`sharedInstance`, `setCategory:error:`), one `AVAudioPlayer` per music track, `AudioServicesPlaySystemSound` (vibrate).
- **`CC_MD5`** hashes the MAC address for the device id; `CC_SHA1` is Chartboost's.

Real GL rendering is M5 (#56 fixed-function emulation onto the backend #55 picks, #58 EAGL framebuffer and presentation). Real sound is M6 (#61 OpenAL, #62 WAV decoding, #63 MP3, #64 session and volume).

## Options considered

**GL.**

1. **A recording layer with a state tracker.** Each GL import decodes its arguments into a command. A tracker applies it to the state the game reads back (names, bindings, texture sizes, renderbuffer storage, framebuffer attachments, matrix-stack depth) and answers queries from that state. Then a sink sees the command. The sink is where #56 executes it.
2. **Constant stubs.** Every function returns 0 or a fixed "good" answer. This is not enough. With `glIsTexture` faked, the text cache took the "texture disappeared" path (the game logged `text exist draw direct But texture disapear`), so the boot would be checked against behaviour the device never shows.
3. **Real GL now.** That depends on #55's backend choice and is M5's work.

**Audio.**

1. **Silent implementations that keep OpenAL's and ExtAudioFile's state:** handles, names, the error rules, WAV headers parsed for format and length, reads returning silence of the right length, player play state.
2. **The default handler** (return 0). The default handler is what the unimplemented report flags, and `alcOpenDevice` returning NULL sends the game into its `exit(1)` path.

**When the first frame is.** A first `presentRenderbuffer:` can come before anything is drawn. The game has no `glClear`, so "after GL draw/clear calls" in practice means "after at least one `glDrawArrays`".

## Decision

- **GL is recorded (option 1)** in `runtime/src/gles/` behind `maplethief/gles.h`, as three layers:
  - **Decode:** one HLE function per import (the X-macro `MT_GLES_OPS`) reads the guest's argument words into an `mt_gles_cmd` (op, argc, args as passed: `GLfixed` and enums as integers, `GLfloat` as bits, pointers as guest addresses).
  - **State:** the tracker keeps texture, framebuffer and renderbuffer names (`glGen*` skips names in use), the bound framebuffer, renderbuffer and 2D texture, each texture's level-0 size and bytes, each renderbuffer's storage, the attachments, the matrix mode and stack depths (16 modelview, 2 projection and texture, the iPhone's limits), and the viewport. It answers `glGen*`, `glIsTexture` (true once bound, until deleted, GL ES 1.1 §3.8.13), `glCheckFramebufferStatusOES` and `glGetRenderbufferParameterivOES` truthfully. A call the device would reject with a GL error is counted as a misuse, logged (`gl: ...`) and ignored. It is never fatal.
  - **Sink:** `mt_gles_config.command` sees each command after the tracker, inside the guest's call, while the guest memory it points at (client arrays, pixels) is valid. The recorder installs none.
  - The state is per process: a new current ObjC runtime, or CF's rebinding to one at the same address, starts it from scratch.
- **`EAGLContext` is a host class** (`runtime/src/gles/eagl.cpp`) with the game's selectors only. `-initWithAPI:` accepts `kEAGLRenderingAPIOpenGLES1` and answers nil for anything else; it records the boot milestone `eagl-context`. `+setCurrentContext:` retains the current context. `-renderbufferStorage:fromDrawable:` takes a `CAEAGLLayer` only, and gives the bound renderbuffer the layer's bounds (scale 1: 480 × 320 for the turned `EAGLView`), the colour format and retained backing of its `drawableProperties` (`kEAGLColorFormatRGBA8` unless `RGB565`). `-presentRenderbuffer:` ends a frame: `mt_gles_frame` (index, size, format, retained backing, the frame's calls, draws and uploads) goes to `mt_gles_config.present`. The `kEAGL*` constants are CF strings made at link time, like the other string data imports.
- **The first GL frame** is the first `presentRenderbuffer:` after at least one `glDrawArrays` (the boot milestone `first-frame`, value = frame index). A present hook that returns nonzero ends the run there (`mt_process_stop`, "presentRenderbuffer: the host ended the run"). That is how `runtime.boot_real` stops.
- **Audio is silent (option 1)** in `runtime/src/audio/` behind `maplethief/audio.h`. OpenAL keeps guest-heap device and context handles, buffer and source names, and the error state as OpenAL keeps it: the first error stays until `alGetError`, and every call without a current context sets `AL_INVALID_OPERATION`. `alBufferData` keeps format, size and rate. The audio session is initialised once (`'init'` the second time) and keeps its category; its interruption listener is never called. `ExtAudioFileOpenURL` parses the WAV with a bounds-checked RIFF chunk walk (PCM only; a truncated last data chunk counts up to the end of the file). Reads return silence of the right length, converted to the client format's rate. `AVAudioSession` is a singleton. `AVAudioPlayer` exists for a file that exists (nil otherwise) and keeps play state, volume, loops and time. Trace channel `audio`. M6's issues replace these files behind the same imports and classes.
- **`CC_MD5` and `CC_SHA1` are real** (RFC 1321, FIPS 180-4), so derived values (the device id in the hello packet) are what the device computes.
- **`runtime.boot_real` has no stand-ins any more.** It runs `boot_real_test first-frame` with the unimplemented policy "stop" and checks the whole boot trace, an empty unimplemented report, and the GL and audio state at the first frame. `runtime.launch_real` (`boot_real_test launch`) runs #54's main-menu checks and the epic's rechecks.

## Consequences

- **Evidence, real assets (local).** The boot reaches `main` `0x280D` → `UIApplicationMain` `0x2849` → `eagl-context` → `nib` (23 objects) → `app-delegate` → `first-frame` 1 → `stop` in about 0.7 s. The report is empty. Frame 1 is 480 × 320 RGBA8 with no retained backing: 1,229 GL calls, 33 draws, 132 texture uploads, 133 textures alive (1,489,604 bytes), framebuffer 1 complete, viewport (0, 0, 480, 320), no misuse. Audio: 44 buffers (26 filled), 44 sources, 26 WAVs opened, none failed. Eight function imports stay on the default handler, and none of them is called on this path (among them `NSLog`, `NSSelectorFromString` and the three `CATransform3D*`, all Chartboost's).
- **Evidence, CI:** `tests/cpp/gles_test.cpp` (in `objc_test`) covers the tracker (names, `glIsTexture`, sizes, deletion, framebuffer status, renderbuffer queries, stacks, misuse), both hooks, `EAGLContext` on a synthetic `CAEAGLLayer` view (RGBA8 and RGB565, other APIs, other drawables, the first frame), OpenAL's error rules, the audio session, `ExtAudioFile` on a synthetic WAV and on hostile ones, `AVAudioPlayer`, and the digests against the RFC 1321 and FIPS 180 vectors. `objc.host_surface` now checks the AVFoundation and OpenGLES areas too.
- **Interface for #56 and #58:** decode and state stay. #56 installs the command hook and executes commands on its backend (the client-array pointers are valid only during the call). #58 installs the present hook to show the renderbuffer, which is 480 × 320 RGBA8 with no retained backing, never cleared by the game. The tracker's answers stay the source of truth for queries, so the backend never needs to read state back.
- **Interface for M6:** #61–#64 replace `runtime/src/audio/` behind the same imports and classes, and keep its state rules (the game's `exit(1)` on any start-up AL error).
- **The game's own bugs are kept.** `ResumeBGM` with no music sends `alSourcePlay(-1)` and gets `AL_INVALID_NAME`, as on the device ([re/audio.md](../re/audio.md)).
- **How we'd know this was wrong:**
  - **#56 needs state the tracker does not keep**, or a query answer differs from what real GL gives on the same calls. Then the tracker grows, or answers move to the backend with the tracker as a cross-check.
  - **A misuse appears in a long play session** (`gl:` lines). Then either the game does misuse GL as on the device (keep it, document it) or a tracker rule is wrong.
  - **Music or effects need timing the silent stubs do not give** (a game wait on `isPlaying` turning NO). The looping music never ends, so this is not expected before M6.
