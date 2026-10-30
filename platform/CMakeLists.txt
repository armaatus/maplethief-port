# SDL3 frontend (ARCHITECTURE.md section 3). Shared C++ lives in src/; per-OS files go in
# macos/, windows/, ios/ and linux/ (Objective-C++ only under macos/ and ios/, ADR 0002).

set(MAPLETHIEF_APP_ID "io.github.armaatus.maplethief")

# The frontend's game and presentation code (#58, ADR 0042): booting the game from the bundle,
# the run loop's turn hook, the GL frame -> compositor -> SDL texture -> window chain, the audio
# device stream and the WAV capture (#61, ADR 0045), mouse and touch input (#65, ADR 0047),
# gamepads and the action bindings (#67, ADR 0048), the key pad's hiding (#71, ADR 0052). A
# library so that tests/cpp/platform_test.cpp drives it on a synthetic guest; it never sees the
# generated code (main.cpp hands it the program).
add_library(
    maplethief_frontend STATIC
    src/actions.cpp
    src/audio_out.cpp
    src/clock.cpp
    src/game.cpp
    src/gamepad.cpp
    src/input.cpp
    src/keypad_hide.cpp
    src/present.cpp
    src/scaling.cpp
    src/settings.cpp)
target_include_directories(maplethief_frontend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(maplethief_frontend PUBLIC runtime SDL3::SDL3)
maplethief_target_defaults(maplethief_frontend)
# The capture converts the mix to 16-bit the same way on every CPU (ADR 0045).
maplethief_strict_fp(src/audio_out.cpp)

add_executable(MapleThief src/main.cpp src/gfx_prototype.cpp)
# The prototype scene of --gfx-prototype (#55, ADR 0039) computes its vertices in floats, which
# must round as in its golden image (tests/cpp/gfx_test.cpp).
maplethief_strict_fp(src/gfx_prototype.cpp)
target_link_libraries(MapleThief PRIVATE maplethief_frontend runtime SDL3::SDL3)
# The game itself (ADR 0042): only when this build has the recompiler's generated code
# (maplethief_gen, cmake/GeneratedCode.cmake), i.e. a local build with work/gen. Without it (CI,
# a clean clone, every release kit: ADR 0024) MapleThief is the placeholder shell it was, and
# the release guard's check that no kit holds generated code stays true. The dev default bundle
# is the bootstrap's extraction next to work/gen.
if(TARGET maplethief_gen)
    target_link_libraries(MapleThief PRIVATE maplethief_gen)
    cmake_path(GET MAPLETHIEF_GEN_DIR PARENT_PATH _maplethief_work_dir)
    cmake_path(ABSOLUTE_PATH _maplethief_work_dir BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE)
    target_compile_definitions(
        MapleThief PRIVATE MT_HAVE_GENERATED_CODE=1
                           MT_DEV_BUNDLE_DIR="${_maplethief_work_dir}/ipa/Payload/MapleThief.app")
    message(STATUS "MapleThief: runs the game (linked with maplethief_gen)")
else()
    message(STATUS "MapleThief: the placeholder shell (no generated code)")
endif()
# MT_VERSION comes from the runtime target (PUBLIC).
target_compile_definitions(MapleThief PRIVATE MT_APP_ID="${MAPLETHIEF_APP_ID}")
maplethief_target_defaults(MapleThief)

if(APPLE)
    # build/<preset>/platform/MapleThief.app. CMake fills the template's
    # ${MACOSX_BUNDLE_*} placeholders from these properties.
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        message(FATAL_ERROR "CMAKE_OSX_DEPLOYMENT_TARGET is not set; use a mac-* preset")
    endif()
    set_target_properties(
        MapleThief
        PROPERTIES MACOSX_BUNDLE ON
                   MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
                   MACOSX_BUNDLE_BUNDLE_NAME "MapleThief"
                   MACOSX_BUNDLE_GUI_IDENTIFIER "${MAPLETHIEF_APP_ID}"
                   MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
                   MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
elseif(WIN32)
    # A GUI program (no console window). SDL_main.h supplies WinMain and passes the command
    # line through as argv.
    set_target_properties(MapleThief PROPERTIES WIN32_EXECUTABLE ON)
endif()

# Smoke test (ADR 0004), registered here next to the target it runs; the unit tests live in
# tests/ (ADR 0005). Starts headless with SDL's dummy video driver, presents a few frames,
# checks the clear colour, quits by itself and exits 0.
add_test(NAME platform.smoke COMMAND MapleThief --smoke-frames 3)
set_tests_properties(platform.smoke PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy" TIMEOUT 30)
# The rendering backend's prototype (#55, ADR 0039): renders a textured, alpha-blended quad with
# mt_gfx, shows it through an SDL streaming texture, reads the presented frame back headless and
# checks the quad's pixels against the scene; exits 0 only if they match.
add_test(NAME platform.gfx_prototype COMMAND MapleThief --gfx-prototype --smoke-frames 3)
set_tests_properties(platform.gfx_prototype PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
                                                       TIMEOUT 30)
# The audio output (#61, ADR 0045): a 440 Hz tone through the mixer and SDL's device stream, on
# SDL's dummy audio driver (no sound, but it pulls in real time); exits 0 only if a device
# opened and its callback pulled at least the tone's 1.5 s of frames. On a desktop,
# `MapleThief --audio-test-tone` without the variables plays it on the default device.
add_test(NAME platform.audio_tone COMMAND MapleThief --audio-test-tone)
set_tests_properties(platform.audio_tone PROPERTIES ENVIRONMENT
                                                    "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy"
                                                    TIMEOUT 30)
# `MapleThief --version` prints exactly "MapleThief <MAPLETHIEF_VERSION>" to stdout and exits 0
# (ADR 0024); scripts/release checks the unpacked kits the same way.
add_test(
    NAME platform.version
    COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:MapleThief>"
            "-DEXPECTED=MapleThief ${MAPLETHIEF_VERSION}" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/version_test.cmake")
set_tests_properties(platform.version PROPERTIES TIMEOUT 30)
# --seconds S (#124, the soak's time limit): every invalid value is a usage error (exit 2)
# before anything starts (platform/seconds_test.cmake; the game's own run is
# platform.game_seconds in tests/, with real assets).
add_test(
    NAME platform.seconds_usage
    COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:MapleThief>" -DMODE=usage -P
            "${CMAKE_CURRENT_SOURCE_DIR}/seconds_test.cmake")
set_tests_properties(platform.seconds_usage PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
                                                       TIMEOUT 30)
# --right-touch X,Y|off (#65, ADR 0047): invalid values are usage errors (exit 2), valid ones run
# (platform/input_test.cmake; the game's clicks are platform.mouse_clicks in tests/, with real
# assets).
add_test(
    NAME platform.input_usage
    COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:MapleThief>" -DMODE=usage -P
            "${CMAKE_CURRENT_SOURCE_DIR}/input_test.cmake")
set_tests_properties(platform.input_usage PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
                                                     TIMEOUT 60)

# Run targets: `cmake --build --preset <preset> --target run` starts the program, `--target
# smoke` runs the smoke test above by hand, outside CTest.
add_custom_target(
    run
    COMMAND MapleThief
    USES_TERMINAL VERBATIM)
add_custom_target(
    smoke
    COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEO_DRIVER=dummy "$<TARGET_FILE:MapleThief>"
            --smoke-frames 3
    USES_TERMINAL VERBATIM)
add_dependencies(smoke MapleThief)
