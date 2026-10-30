// GPU presentation (#130, ADR 0068): the game's GL frame stays on the GPU from draw to window.
//
// With gfx_backend = gpu, mt_gfx draws into its targets' GPU textures, and the window's renderer
// is SDL's GPU renderer on the same device. The read-back path (Screen, present.h) downloads
// each GL frame, composites UIKit's views over it on the CPU and uploads the result again. At
// 4x that is about 10 MB down and 10 MB up a frame, with a fence wait in between. GpuScreen keeps
// the frame on the GPU instead:
//
//   -[EAGLContext presentRenderbuffer:]  ->  GpuScreen::take_gl_frame
//       mt_gfx_gpu_target_texture(target), copied on the GPU into a texture of the window's
//       renderer (the copy Screen::take_gl_frame makes, so later draws cannot tear the frame)
//   the UIKit turn hook                  ->  GpuScreen::compose_uikit
//       mt_uikit_composite(OVERLAY) on the CPU: UIKit's views, transparent in the GL slot
//       (ADR 0037), uploaded as a second texture when it changed
//                                        ->  GpuScreen::compose
//       into the Presenter's screen texture (a render target): opaque black, the GL frame in the
//       GL view's rectangle (its colour; alpha stays 255, as the opaque CAEAGLLayer shows it),
//       the overlay over it (premultiplied "over")
//                                        ->  Presenter::draw
//       as before: the scale mode, a shader filter's pass (#88), the bars
//
// The result equals the FULL composite up to the GPU's rounding of the blend (ADR 0060's
// tolerance): (views over transparent) over GL rather than views over GL, layer by layer.
//
// Nothing is read back per frame. The downloads (read_gl, read_screen) are for the test aids
// only: --shot (the composited screen), --shot-gl (the GL frame), --capture (the first-lit
// frames) and the --frames end check. They are the frontend's own and do not count in mt_gfx's
// GPU stats (readbacks stays 0).
#ifndef MAPLETHIEF_PLATFORM_GPU_SCREEN_H
#define MAPLETHIEF_PLATFORM_GPU_SCREEN_H

#include <cstdint>
#include <vector>

#include "maplethief/gles.h"
#include "maplethief/recomp/recomp.h"
#include "scaling.h"

struct SDL_GPUDevice;
struct SDL_GPUTexture;
struct SDL_Renderer;
struct SDL_Texture;

namespace maplethief::frontend {

class GpuScreen {
  public:
    GpuScreen() = default;
    GpuScreen(const GpuScreen &) = delete;
    GpuScreen &operator=(const GpuScreen &) = delete;
    ~GpuScreen() { stop(); }

    // `renderer`: SDL's GPU renderer on the device that mt_gfx's GPU backend draws with.
    // `screen`: the Presenter's screen texture (Presenter::start with a render target),
    // width x height. Returns false (logged) if the renderer is not a GPU renderer or a texture
    // cannot be made.
    bool start(SDL_Renderer *renderer, SDL_Texture *screen, std::uint32_t width,
               std::uint32_t height);
    // Frees the textures (before the renderer goes).
    void stop();
    bool running() const { return renderer_ != nullptr; }

    // The present hook's half: the frame's target as it is now, copied on the GPU. Returns false
    // if the frame has no target or the copy cannot be made (logged).
    bool take_gl_frame(const mt_gles_frame &frame);
    // The index of the last GL frame taken (mt_gles_frame.index), 0 if none.
    std::uint32_t gl_frame() const { return gl_index_; }
    std::uint32_t gl_width() const { return gl_width_; }
    std::uint32_t gl_height() const { return gl_height_; }

    // Composites UIKit's views at `now` in overlay mode (with the key pad's opacity as the
    // caller set it) and composes the screen texture with the GL frame in the GL view's
    // rectangle (mt_uikit_gl_rect). Returns false if UIKit has nothing to composite (no
    // application or window) or drawing failed.
    bool compose_uikit(mt_cpu *cpu, double now);
    // Composes the screen texture from an overlay (width x height premultiplied RGBA8, top row
    // first) and the last GL frame taken, drawn into `gl_rect` (screen pixels; nullptr: no GL
    // view, nothing of the GL frame shows). The overlay is uploaded only when it differs from
    // the last one. The renderer's target is restored afterwards.
    bool compose(const std::uint8_t *overlay, std::uint32_t stride, const Rect *gl_rect);
    // The overlay of the last compose (width x height RGBA8).
    const std::vector<std::uint8_t> &overlay() const { return uploaded_; }

    // Downloads the last GL frame taken (gl_width x gl_height RGBA8, top row first, alpha 255 as
    // the opaque layer shows it). Waits for the GPU. False (logged) if there is none.
    bool read_gl(std::vector<std::uint8_t> *rgba);
    // Downloads the composed screen texture (width x height RGBA8, top row first, opaque).
    // Waits for the GPU.
    bool read_screen(std::vector<std::uint8_t> *rgba);

    struct Stats {
        std::uint64_t frames_taken = 0;    // GL frames copied on the GPU
        std::uint64_t composes = 0;        // screen textures composed
        std::uint64_t overlay_uploads = 0; // overlays uploaded (the rest were unchanged)
        std::uint64_t downloads = 0;       // read_gl and read_screen calls that downloaded
        std::uint64_t download_bytes = 0;
    };
    const Stats &stats() const { return stats_; }

  private:
    bool ensure_gl_texture(std::uint32_t w, std::uint32_t h);

    SDL_Renderer *renderer_ = nullptr;
    SDL_GPUDevice *device_ = nullptr;
    SDL_Texture *screen_ = nullptr;    // the Presenter's (not owned)
    SDL_Texture *gl_ = nullptr;        // the copy of the last GL frame
    SDL_GPUTexture *gl_gpu_ = nullptr; // its GPU texture (the renderer's)
    SDL_Texture *overlay_tex_ = nullptr;
    std::uint32_t width_ = 0, height_ = 0;
    std::uint32_t gl_index_ = 0, gl_width_ = 0, gl_height_ = 0;
    bool have_gl_ = false;
    std::vector<std::uint8_t> scratch_, uploaded_; // the composite and the last one uploaded
    bool uploaded_valid_ = false;
    Stats stats_;
};

// How far the GPU's composed screen is from the CPU's FULL composite, RGB only (the window has
// no alpha): ADR 0060's tolerance, a channel off by more than 2 in at most 0.5% of the pixels.
struct ScreenDiff {
    static constexpr int kChannel = 2;
    static constexpr double kRatio = 0.005;
    std::uint64_t pixels = 0;
    std::uint64_t beyond = 0; // pixels with a channel off by more than kChannel
    int max_delta = 0;
    bool within() const {
        return static_cast<double>(beyond) <= kRatio * static_cast<double>(pixels);
    }
};
// Two width x height RGBA8 images (rows of width * 4 bytes).
ScreenDiff diff_screens(const std::uint8_t *a, const std::uint8_t *b, std::uint32_t width,
                        std::uint32_t height);

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_GPU_SCREEN_H
