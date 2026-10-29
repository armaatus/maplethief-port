# 40. GL ES 1.1 fixed-function pipeline: built-in execution on mt_gfx, renderbuffer targets, error flag and GL trace file

- **Status:** Accepted; amended by #121 (the game's `GL_LINES` are drawn: only `GL_POINTS` still count as `draws_skipped`)
- **Date:** 2026-10-29
- **Issue:** #56
- **Builds on:** [ADR 0038](0038-gl-recording-eagl-silent-audio-boot-to-first-frame.md) (decode, state tracker, command and present hooks), [ADR 0039](0039-rendering-backend-software-rasterizer.md) (`mt_gfx`, the draw interface, determinism), [ADR 0030](0030-runtime-diagnostics.md) (trace channels)

## Context

M4 left OpenGL ES recorded: every one of the 31 GL imports is decoded into an `mt_gles_cmd`, a state tracker answers the game's queries (names, `glIsTexture`, framebuffer status, renderbuffer size), and a command hook (`mt_gles_config.command`) was planned as the place where #56 executes the commands ([ADR 0038](0038-gl-recording-eagl-silent-audio-boot-to-first-frame.md)). [ADR 0039](0039-rendering-backend-software-rasterizer.md) chose the backend: `mt_gfx`, a deterministic software rasterizer that takes one self-contained draw (clip-space vertices with s, t, a constant colour, a texture handle, blend factors, a viewport) per `glDrawArrays`.

#56 is the half in between: the fixed-function state (matrices, client arrays, enables, colour, blending, textures), the draws, GL's error semantics and a trace for offline debugging. #58 presents the frames next; it needs a target per renderbuffer and a frame that names it.

What the game does on the real boot and launch path, from the GL trace of `runtime.launch_real` (6 s: the boot, the main menu, Play, the loading screen, the first map; 139 presents, 2,244 draws; [ANALYSIS.md → Rendering](../ANALYSIS.md#rendering)):

- One projection, `glOrthox(0, 480, 0, 320, -1, 1)` (y up), set once. Around every draw: `glLoadIdentity`, `glPushMatrix`, `glScalef(1, 1, 1)`, `glPopMatrix`. No `glRotatex` or `glTranslatex` on this path (the engine positions sprites in the vertex arrays).
- Every draw is `glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)`: vertices `2 × GL_SHORT`, stride 0, often on the guest stack; texture coordinates `2 × GL_FIXED`, stride 0, enabled and disabled around each textured draw.
- `GL_BLEND` is enabled and disabled around every draw, `GL_TEXTURE_2D` and `GL_ALPHA_TEST` around every textured one (6,464 `glEnable` and as many `glDisable` calls). The game imports no `glAlphaFunc`, so the alpha test keeps its initial function `GL_ALWAYS` and passes every fragment.
- One blend function: `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`.
- 565 uploads, all `GL_RGBA`/`GL_UNSIGNED_BYTE` at level 0, all powers of two (1 × 1 to 512 × 512); every texture gets `GL_LINEAR` for both filters and keeps `GL_REPEAT`.
- `glColor4x` with the 8-bit colour shifted left by 8: full intensity is `0xFF00`, which GL's conversion makes 254, not 255.

## Options considered

1. **Execution behind the command hook, installed by the host** (ADR 0038's plan).
   - Pro: the recorder stays a recorder; a host could swap executors.
   - Con: every host (the platform, each real-asset test, a future headless runner) must install the same executor, or the game draws nothing. The hook runs after the tracker has already changed its state, so a rejected call is hard to tell from an accepted one, and texture deletion has already forgotten the texture's handle.
   - Con: executing needs the tracker's objects (the bound texture, the colour attachment's renderbuffer). Through a public hook it would have to keep a second copy of them.
2. **Execution built into the runtime, always on; the hook stays as an observer.**
   - Pro: one GL implementation for every host. The pipeline runs only for calls the tracker accepted, and it shares the tracker's objects: a texture object carries its `mt_gfx` texture, a renderbuffer its `mt_gfx` target, freed with them.
   - Pro: traces and tests keep the hook. They see each command after it was executed, with the guest memory it points at still valid.
   - Con: no way to switch execution off. The cost is that of `mt_gfx` itself (see Evidence), and a recording-only run has no use.
3. **Batching the game's draws** (merging consecutive quads with the same state into one `mt_gfx` draw).
   - Pro: fewer submits.
   - Con: the game changes state around every draw (enables, colour, texture), so there is little to merge. `mt_gfx` costs per pixel, not per submit, and batching would change the order in which the trace and the statistics see draws. Not now; ADR 0039 already lets `mt_gfx_submit` batch internally if a GPU backend needs it.

## Decision

- **Option 2.** The fixed-function pipeline is part of the runtime (`runtime/src/gles/fixed.cpp`), next to the tracker (`gles.cpp`), and always on. Each import runs: decode → tracker → pipeline (only if the tracker raised no misuse) → trace → `mt_gles_config.command` (an observer). Compiled without floating-point contraction, like `mt_gfx` (`maplethief_strict_fp`).
- **Matrices.** Three stacks with values: modelview 16 deep, projection 2, texture 2 (the depths stay the tracker's). `glLoadIdentity`, `glPushMatrix` (copies the top), `glPopMatrix`, `glOrthox`, `glTranslatex`, `glRotatex` and `glScalef` multiply the top of the current stack on the right, as GL ES 1.1 section 2.10.2 defines them. Matrices are float, column-major. Each operation's matrix is built in double from its arguments and rounded once to float. `GLfixed` is `value / 65536`, rounded once. `glOrthox` with `l = r`, `b = t` or `n = f` is `GL_INVALID_VALUE`. `glRotatex` normalises its axis (`sqrt`, which IEEE requires to be correctly rounded, so it is the same everywhere). It takes sine and cosine from an exact integer reduction of the 16.16 angle to a quadrant and an offset of at most 45°, then Taylor series in double. That way rotations are bit-identical on every CPU, with no libm call (ADR 0039), and exact at multiples of 90°. A zero axis leaves the matrix as it is (GL ES leaves it undefined) and is logged once.
- **Client arrays.** `glVertexPointer`/`glTexCoordPointer` take sizes 2 to 4 and `GL_BYTE`, `GL_SHORT`, `GL_FIXED`, `GL_FLOAT` with a stride (0 means packed); anything else is `GL_INVALID_VALUE` or `GL_INVALID_ENUM`. `GL_VERTEX_ARRAY` and `GL_TEXTURE_COORD_ARRAY` are executed. `GL_NORMAL_ARRAY`, `GL_COLOR_ARRAY` and `GL_POINT_SIZE_ARRAY_OES` are kept but have no pointer function in the game, so a draw with one enabled is logged once. At `glDrawArrays` the elements `first .. first + count − 1` are read from guest memory **after checking the whole range**: every page of it must lie in an image segment, the heap below its top, or a guest stack (`mt_mem_describe`). An array outside mapped memory is a misuse (logged, no GL error, the draw skipped), never a fault. Missing components are z = 0, w = 1 and r = 0, q = 1. Without a texture coordinate array, every vertex has the current coordinates (0, 0, 0, 1); the game imports no `glMultiTexCoord`.
- **The draw.** Positions are transformed by projection × modelview to clip space. Texture coordinates go through the texture matrix (skipped when it is the identity) and are divided by q when q is neither 1 nor 0. Then one `mt_gfx_draw` with the primitive as given (points and lines are `mt_gfx`'s to count; since #121 it draws lines), the constant colour, the bound texture if `GL_TEXTURE_2D` is enabled and the texture is complete, the blend enable and factors, and the tracker's viewport. It goes into the target of the bound framebuffer's colour attachment. Without a complete framebuffer the draw is `GL_INVALID_FRAMEBUFFER_OPERATION_OES` and skipped. Without a vertex array, or with a count of 0, it draws nothing and sets no error, as in GL. `glDrawArrays` with an unknown mode is `GL_INVALID_ENUM`, a negative first or count `GL_INVALID_VALUE`.
- **State.**
  - `glEnable`/`glDisable` take every GL ES 1.1 cap (with GL's initial values: only `GL_DITHER` and `GL_MULTISAMPLE` start on); any other value is `GL_INVALID_ENUM`. `GL_TEXTURE_2D` and `GL_BLEND` are executed. Of the rest, only `GL_CULL_FACE`, `GL_LIGHTING` and `GL_FOG` would change the game's pixels (and, since #121 draws lines, `GL_LINE_SMOOTH` on a line draw, logged the same way; not enabled on the launch-to-dialogue path). None of the three is on the boot or launch path; a draw with one on is logged once and drawn without it. The others change nothing here: there is no depth, stencil or multisample buffer, no point or line is drawn, and the game imports no `glAlphaFunc`, `glScissor`, `glLogicOp` or `glClipPlane`, so those tests keep initial values that pass everything.
  - `glColor4x` clamps each component to [0, 1] and converts it as GL does, `round(v × 255)`, i.e. `(v × 255 + 32768) >> 16` on the 16.16 value. The game's `0xFF00` is therefore 254; #60's device screenshots will show whether the device rounds the same.
  - `glBlendFunc` takes the source and destination factor lists of GL ES 1.1 section 4.1.7; any other factor is `GL_INVALID_ENUM`.
  - Textures:
    - A texture name becomes an `mt_gfx` texture when it is first bound, when it becomes a texture object; the default texture 0 gets one at its first upload or parameter. `glDeleteTextures` destroys it.
    - `glTexParameteri` checks the parameter and value (`GL_TEXTURE_MIN_FILTER`, `MAG_FILTER`, `WRAP_S`, `WRAP_T`, `GL_GENERATE_MIPMAP`) and passes the filters and wraps to `mt_gfx_texture_parameter`.
    - `glTexImage2D` gets GL ES 1.1's format rules in the tracker: `internalformat` equals `format`, and a packed type goes with its format. Level 0 goes to `mt_gfx_texture_image`, with the pixels range-checked like the arrays.
    - A format/type pair that `mt_gfx` does not convert yet keeps the tracker's size, leaves the `mt_gfx` texture empty (drawn untextured) and is logged once per pair. #57 adds the conversions.
    - Non-power-of-two sizes are accepted and logged once, as on devices with `APPLE_texture_2D_limited_npot`. Levels above 0 are logged once and dropped.
  - **Texture completeness** is GL's: a mipmapped minification filter with only level 0 makes a texture incomplete, unless the image is 1 × 1 or `GL_GENERATE_MIPMAP` is on. Then the draw is untextured, as on the device, and logged once. With `GL_GENERATE_MIPMAP` no mipmaps are made; `mt_gfx` samples level 0 (logged once).
- **Render targets (shared with #58).** When EAGL gives a renderbuffer storage (`-renderbufferStorage:fromDrawable:`), it gets an `mt_gfx` target of that logical size at the render scale `mt_gles_config.render_scale` (0 means 1, at most `MT_GFX_MAX_SCALE`). New storage replaces the old target. GL and EAGL keep seeing the logical size. `-presentRenderbuffer:` calls `mt_gfx_end_frame()`, then the present hook gets the frame with `mt_gles_frame.target`: the presented renderbuffer's target, valid until that renderbuffer gets new storage, `mt_gles_reset` or a new process. A new process (a new ObjC runtime) and `mt_gles_reset` free the old state's textures and targets.
- **Errors.**
  - GL's error flag is kept although the game imports no `glGetError`. Every call the device would reject records its error code if the flag is clear; the first error sticks until `mt_gles_get_error()` reads it. `mt_gles_stats.error` shows it without clearing it. Each such call changes nothing, as on the device.
  - Every misuse is counted, GL error or not (a client array outside memory, an EAGL call without a renderbuffer). It is logged the first time its kind (`"<function>: <what>"`) occurs, with details, then again at its 10th, 100th, 1,000th … occurrence, so a mistake repeated every frame cannot flood the log.
  - Gaps in the emulation (an unconverted format, an enable that is not executed) are logged once per kind, not counted as misuse.
  - The statistics add `draws_drawn`, `draws_skipped` and `vertices`.
- **Trace.**
  - The `gl` trace channel (`MAPLETHIEF_TRACE=gl`, ADR 0030) logs every call with its arguments decoded: enum names, `GLfixed` and float values, guest addresses in hex, query results. Each `glDrawArrays` gets its outcome (`"4 vertices, drawn"`, `"skipped: count 0"`), and presents and storage are logged too.
  - **The GL trace file**: `MAPLETHIEF_GL_TRACE_FILE=<path>` (or `mt_gles_config.trace_file`, which overrides the environment; `""` turns it off) writes the same lines to a text file, with log lines as `# …` comments. The file is opened, truncated, at the first GL call after `mt_gles_configure`, kept open across processes (each new GL state starts with `# new GL state`) and flushed at every present.
  - It is for local debugging only: the 6 s launch path writes 1.3 MB. Analysing the file answered this ADR's questions about what the game uses.

## Evidence

- **Tests** (`tests/cpp/gles_fixed_test.cpp`, in `objc_test`, CI; synthetic only, every call through the HLE functions on a guest address space):
  - conversions and matrices: `GLfixed` conversions; `glColor4x` clamping and rounding over the whole range; `glOrthox` (y up, y down, degenerate), `glTranslatex`, `glScalef` and their composition order against hand-computed matrices; `glRotatex` exact at multiples of 90° (also wrapped and negative angles, unnormalised axes), 30° about z, 120° about (1, 1, 1), 45° about y; the sine and cosine within 1e-14 of libm over ±720°; the zero axis;
  - push and pop on all three stacks, overflow and underflow with their errors;
  - client arrays: `GL_BYTE`/`SHORT`/`FIXED`/`FLOAT`, sizes 2–4, packed, padded and interleaved strides, q division, the texture matrix, the current texture coordinates, projection × modelview; bad pointer arguments; arrays in page zero, unmapped image space, above the heap's top, beyond guest memory and on the texture coordinate side, each a logged misuse with the draw skipped;
  - draws: no vertex array, count 0, unknown mode, negative first, lines, no framebuffer;
  - enables, client states and blend functions with GL ES 1.1's lists; the once-only notes;
  - textures: the lifecycle from the first bind to the delete, parameters onto `mt_gfx`, the format rules, unconverted pairs (logged once per pair), pixels outside memory, completeness and `GL_GENERATE_MIPMAP`;
  - targets, frames, errors and trace: the target at render scale 2 and replaced by new storage, `mt_gles_frame.target`, `mt_gfx_end_frame` at present; the first error sticking and logging at the 1st and 10th time; the trace file's lines.
- **Golden images** (`tests/golden/data/`, tolerance 0), each drawn by GL call sequences of the game's kind (`GL_SHORT` strips, `GL_FIXED` texture coordinates, a y-up ortho):
  - `gles_ortho_quad.png`: a checkerboard magnified 32× nearest, repeated, bilinear and clamped; a 16 × 16 sprite opaque, blended, tinted and mirrored by `glScalef(-1, 1, 1)`; a bar on half-pixel `GL_FIXED` edges.
  - `gles_rotated_sprite.png`: sprites turned by `glRotatex` about their centres at 0°, 30°, 90°, −135°, 45°, 200°, −15° and 780°, one turned by the texture matrix, one turned about x and clipped by the depth range.
  - `gles_blend_modes.png`: ten factor pairs over an alpha ramp, and blending off.
  - They match on macOS arm64 and in the Linux x86_64 container (`scripts/local-ci`).
- **Real assets (local, `runtime.boot_real`, `runtime.launch_real`)**:
  - Every `glDrawArrays` of the boot and the 6 s launch is drawn by `mt_gfx`: 33 at the first frame, 2,244 in 139 frames, none skipped, no misuse, no GL error.
  - The GL targets are written to `build/test-output/<preset>/gles_real/` and were looked at:
    - Frame 1 is black: it ends with the fade-in's black untextured quad at alpha 254 over the screen.
    - The main-menu frames draw only that quad; the menu itself is UIKit.
    - The loading screen after Play shows the skull pattern, the spinner around the pig, "Loading…" and the tip text, upright and readable.
    - The first map shows Henesys with "Peaceful Henesys" over it, sky at the top, ground at the bottom, between letterbox bars.
  - **Speed** (Apple M4, arm64 Release, `mt_gfx`'s own timing). A frame has 16–24 draws and 235,000 shaded pixels on average.
    - With the main thread kept busy (a local measurement in which the launch run's turn hook spins instead of sleeping), the costs per frame are: 0.14 ms for a main-menu frame (one full-screen blended quad), 1.4 ms for the loading screen (58 draws, 177,000 pixels), 2.4–2.5 ms for the first map (6 draws, 366,000 pixels, 2.4× overdraw), 1.27 ms on average. That is under ADR 0039's 4 ms threshold for a map frame.
    - As the test runs it, the run loop sleeps between frames and macOS often moves the thread to an efficiency core. The same full-screen quad then takes 0.15 ms in one frame and 1.1 ms in another: 6.1 ms a frame on average, up to 17 ms for a map frame.
    - A Debug build takes 10.2 ms a frame on average.

## Consequences

- **#57** adds rows to `mt_gfx`'s conversion table. The pipeline already passes every valid pair through and logs the ones that are missing. On the boot and launch path that list is empty: every upload is `GL_RGBA`/`GL_UNSIGNED_BYTE`. #57 should look at map and UI paths further into the game (with the GL trace file) for other pairs and decide about premultiplied alpha.
- **#58** gets the presented target in `mt_gles_frame.target` and sets `mt_gles_config.render_scale`; it does not need a command hook. M10's HD modes reuse the same field.
- **#60** compares the targets with device screenshots. Open question for it: the game's `0xFF00` colours give 254 under GL's rounding. A device that truncates `x >> 8` would give 255, a difference of one level in alpha-blended sprites.
- **Debugging** a rendering problem starts with `MAPLETHIEF_GL_TRACE_FILE`, which records the whole GL stream with each draw's outcome.
- **How we'd know this was wrong:**
  - A host needs GL without drawing (a fast headless run). Then a config flag switches execution off; the hook order stays.
  - The device's results differ from GL's rules as written here (colour rounding, completeness, NPOT). Then the rule changes in `fixed.cpp`, once, for every platform.
  - Later game states use rotations, other primitives or other blend functions that look wrong. The trace file shows which calls they are, and the goldens get a case for each.
