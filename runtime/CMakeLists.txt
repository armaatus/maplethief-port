# HLE runtime (ARCHITECTURE.md section 2): guest memory (runtime/src/mem/, ADR 0025), the import
# table at run time and the HLE implementations (runtime/src/hle/, ADR 0019), the Objective-C
# runtime over the guest's metadata (runtime/src/objc/, ADR 0026), the offline service stubs
# (runtime/src/stubs/, #52, ADR 0034), process start-up
# (runtime/src/boot/, ADR 0027), diagnostics: trace, report, symbols and the crash reporter
# (runtime/src/diag/, ADR 0030), CoreFoundation (runtime/src/cf/, ADR 0032), GCD's queues and worker
# threads (runtime/src/gcd/, ADR 0033), CoreGraphics and UIKit drawing (runtime/src/cg/,
# ADR 0036), Foundation (runtime/src/foundation/, ADR 0035; the save files, ADR 0074), UIKit
# (runtime/src/uikit/, ADR 0037), OpenGL ES 1.1 and EAGL (runtime/src/gles/, ADR 0038, the fixed-function pipeline ADR
# 0040), audio: OpenAL and AVAudioPlayer's MP3s (dr_mp3) through the mixer (runtime/src/audio/,
# ADR 0045; the rest silent, ADR 0038; audio packs, ADR 0106), the rendering backend mt_gfx (runtime/src/gfx/, ADR 0039), game
# actions into the key pad and Clet_HandleEvent (runtime/src/input/, ADR 0048), game events seen at
# observed guest functions (runtime/src/events/, ADR 0053), guest function hooks
# (runtime/src/hooks/, ADR 0069), texture packs and dumps (runtime/src/texpack/, ADR 0081), the
# offline polish: the dead online UI hidden and the local achievements (runtime/src/offline/, ADR
# 0084), save states (runtime/src/snapshot/, ADR 0105); M4 grows it into the full runtime.

add_library(
    runtime STATIC
    src/runtime.cpp
    src/sjlj.c
    src/audio/audio.cpp
    src/audio/audio_pack.cpp
    src/audio/avfoundation.cpp
    src/audio/mixer.cpp
    src/audio/mp3_stream.cpp
    src/audio/wav.cpp
    src/boot/boot.c
    src/boot/image.c
    src/boot/objc_boot.c
    src/boot/process.c
    src/boot/trap.c
    src/cf/bridged_classes.cpp
    src/cf/bundle.cpp
    src/cf/cf.cpp
    src/cf/data.cpp
    src/cf/encoding.cpp
    src/cf/runloop.cpp
    src/cf/string.cpp
    src/cg/codec.cpp
    src/cg/hires_text.cpp
    src/cg/objects.cpp
    src/cg/raster.cpp
    src/cg/text.cpp
    src/cg/uikit.cpp
    src/diag/crash.cpp
    src/diag/diag.cpp
    src/diag/symbols.cpp
    src/foundation/collections.cpp
    src/foundation/files.cpp
    src/foundation/misc.cpp
    src/foundation/ns_bundle.cpp
    src/foundation/ns_data.cpp
    src/foundation/ns_string.cpp
    src/foundation/plist.cpp
    src/foundation/saves.cpp
    src/gcd/gcd.cpp
    src/gfx/gfx.cpp
    src/gfx/raster.cpp
    src/gfx/texture.cpp
    src/gles/eagl.cpp
    src/gles/fixed.cpp
    src/gles/gles.cpp
    src/gles/gles_snapshot.cpp
    src/hle/commoncrypto.cpp
    src/hle/cxxabi.cpp
    src/hle/format.cpp
    src/hle/imports.cpp
    src/hle/libc.cpp
    src/hle/libc_sys.cpp
    src/hle/libgcc.cpp
    src/hle/registry.cpp
    src/hle/rtti.cpp
    src/cashshop/cashshop.cpp
    src/cashshop/pricing.cpp
    src/events/events.cpp
    src/hooks/builtin.cpp
    src/hooks/flashes.cpp
    src/hooks/hooks.cpp
    src/hooks/widescreen.cpp
    src/hooks/widescreen_menus.cpp
    src/input/input.cpp
    src/mem/heap.cpp
    src/mem/memory.cpp
    src/mem/vm.cpp
    src/objc/blocks.cpp
    src/objc/classes.cpp
    src/objc/libobjc.cpp
    src/objc/nsobject.cpp
    src/objc/objc.cpp
    src/offline/achievements.cpp
    src/offline/offline_ui.cpp
    src/snapshot/hashes.cpp
    src/snapshot/registry.cpp
    src/snapshot/snapshot.cpp
    src/snapshot/state_file.cpp
    src/stubs/gamekit.cpp
    src/stubs/json.cpp
    src/stubs/network.cpp
    src/stubs/storekit.cpp
    src/stubs/stubs.cpp
    src/texpack/texpack.cpp
    src/textpack/strings_file.cpp
    src/textpack/textpack.cpp
    src/uikit/app.cpp
    src/uikit/compositor.cpp
    src/uikit/controls.cpp
    src/uikit/focus.cpp
    src/uikit/nib.cpp
    src/uikit/touch.cpp
    src/uikit/view.cpp)
target_include_directories(
    runtime
    PUBLIC include
    PRIVATE src)
# MT_VERSION is the full version (MAPLETHIEF_VERSION, e.g. 0.0.1-rc.1, ADR 0024) as a string
# literal, defined once here; PUBLIC, so that everything linking the runtime (the platform
# layer, the tests) sees the same value.
target_compile_definitions(runtime PUBLIC MT_VERSION="${MAPLETHIEF_VERSION}")
# dladdr names the generated function at a hardware fault's host PC (diag/crash.cpp). Horizon
# has no libdl (the Switch's crash.cpp has no dladdr yet, #236).
if(NOT NINTENDO_SWITCH)
    target_link_libraries(runtime PRIVATE ${CMAKE_DL_LIBS})
endif()
# GCD's worker threads (src/gcd/gcd.cpp).
find_package(Threads REQUIRED)
target_link_libraries(runtime PUBLIC Threads::Threads)
# CoreGraphics and UIKit drawing (src/cg/): PNG and glyph decoding with stb, the bundled font
# embedded as a generated source (ADR 0036).
maplethief_use_stb()
maplethief_use_font(font_source)
target_sources(runtime PRIVATE "${font_source}")
# The fallback font for the characters NanumGothic lacks (text packs, #189, ADR 0108).
maplethief_use_fallback_font(fallback_font_source)
target_sources(runtime PRIVATE "${fallback_font_source}")
target_link_libraries(runtime PRIVATE maplethief::stb)
# AVAudioPlayer's MP3 music (#63): dr_mp3, implemented in src/audio/mp3_stream.cpp only.
maplethief_use_dr_mp3()
target_link_libraries(runtime PRIVATE maplethief::dr_mp3)
# Audio packs' FLAC files (#188, ADR 0106): dr_flac, implemented in src/audio/mp3_stream.cpp too.
maplethief_use_dr_flac()
target_link_libraries(runtime PRIVATE maplethief::dr_flac)
# What a frame costs is here: mt_gfx, the CoreGraphics drawing and the UIKit compositor run
# optimised in Debug too (#136, ADR 0134); the rest of the runtime stays at -O0 for stepping.
maplethief_debug_opt(
    runtime
    src/cg/codec.cpp
    src/cg/hires_text.cpp
    src/cg/objects.cpp
    src/cg/raster.cpp
    src/cg/text.cpp
    src/cg/uikit.cpp
    src/gfx/gfx.cpp
    src/gfx/raster.cpp
    src/gfx/texture.cpp
    src/uikit/compositor.cpp)
maplethief_target_defaults(runtime)
# mt_gfx and the GL ES pipeline that feeds it render the same bytes on every OS and CPU (ADR 0039,
# ADR 0040): no floating-point contraction.
maplethief_strict_fp(src/gfx/gfx.cpp src/gfx/raster.cpp src/gfx/texture.cpp src/gles/fixed.cpp)
# The audio mixer's output is the same bytes on every OS and CPU (ADR 0045): no contraction.
maplethief_strict_fp(src/audio/mixer.cpp)
# So is the MP3 decode (#63): dr_mp3's scalar float path (DR_MP3_NO_SIMD in the file), without
# contraction, gives the same samples on arm64 and x86_64.
maplethief_strict_fp(src/audio/mp3_stream.cpp)

# mt_gfx's GPU backend (#120, ADR 0060): the draws on SDL3 GPU (Metal, D3D12, Vulkan), installed
# by the frontend when settings.ini says gfx_backend = gpu. It is a library of its own, so the
# runtime itself never links SDL. Its shaders are compiled ahead of time (scripts/build-shaders)
# into the committed src/gfx/gpu/shaders_compiled.h, so building it needs no shader tools.
add_library(maplethief_gfx_gpu STATIC src/gfx/gpu/gpu_backend.cpp)
target_include_directories(maplethief_gfx_gpu PRIVATE src)
target_link_libraries(maplethief_gfx_gpu PUBLIC runtime SDL3::SDL3)
maplethief_debug_opt(maplethief_gfx_gpu) # mt_gfx too (ADR 0134)
maplethief_target_defaults(maplethief_gfx_gpu)
