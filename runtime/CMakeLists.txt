# HLE runtime (ARCHITECTURE.md section 2): guest memory (runtime/src/mem/, ADR 0025), the import
# table at run time and the HLE implementations (runtime/src/hle/, ADR 0019), the Objective-C
# runtime over the guest's metadata (runtime/src/objc/, ADR 0026), the offline service stubs
# (runtime/src/stubs/, #52, ADR 0034), process start-up
# (runtime/src/boot/, ADR 0027), diagnostics: trace, report, symbols and the crash reporter
# (runtime/src/diag/, ADR 0030), CoreFoundation (runtime/src/cf/, ADR 0032), GCD's queues and worker
# threads (runtime/src/gcd/, ADR 0033), CoreGraphics and UIKit drawing (runtime/src/cg/,
# ADR 0036), Foundation (runtime/src/foundation/, ADR 0035), UIKit (runtime/src/uikit/, ADR
# 0037), OpenGL ES 1.1 and EAGL recorded and audio silent (runtime/src/gles/, runtime/src/audio/,
# ADR 0038); M4 grows it into the full runtime.

add_library(
    runtime STATIC
    src/runtime.cpp
    src/sjlj.c
    src/audio/audio.cpp
    src/audio/avfoundation.cpp
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
    src/gcd/gcd.cpp
    src/gles/eagl.cpp
    src/gles/gles.cpp
    src/hle/commoncrypto.cpp
    src/hle/cxxabi.cpp
    src/hle/format.cpp
    src/hle/imports.cpp
    src/hle/libc.cpp
    src/hle/libc_sys.cpp
    src/hle/libgcc.cpp
    src/hle/registry.cpp
    src/hle/rtti.cpp
    src/mem/heap.cpp
    src/mem/memory.cpp
    src/mem/vm.cpp
    src/objc/blocks.cpp
    src/objc/classes.cpp
    src/objc/libobjc.cpp
    src/objc/nsobject.cpp
    src/objc/objc.cpp
    src/stubs/gamekit.cpp
    src/stubs/json.cpp
    src/stubs/network.cpp
    src/stubs/storekit.cpp
    src/stubs/stubs.cpp
    src/uikit/app.cpp
    src/uikit/compositor.cpp
    src/uikit/controls.cpp
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
# dladdr names the generated function at a hardware fault's host PC (diag/crash.cpp).
target_link_libraries(runtime PRIVATE ${CMAKE_DL_LIBS})
# GCD's worker threads (src/gcd/gcd.cpp).
find_package(Threads REQUIRED)
target_link_libraries(runtime PUBLIC Threads::Threads)
# CoreGraphics and UIKit drawing (src/cg/): PNG and glyph decoding with stb, the bundled font
# embedded as a generated source (ADR 0036).
maplethief_use_stb()
maplethief_use_font(font_source)
target_sources(runtime PRIVATE "${font_source}")
target_link_libraries(runtime PRIVATE maplethief::stb)
maplethief_target_defaults(runtime)
