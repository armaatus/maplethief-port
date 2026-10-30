# C/C++ tests (ADR 0005). Each area gets one doctest executable; every doctest test case is
# registered with CTest as "<area>.<test case name>", next to platform.smoke. The Python
# tests in this directory run through pytest, not CTest (scripts/test runs both).

maplethief_use_doctest()

# Where tests write files for a human to look at, e.g. golden-image diffs. The presets point
# it at build/test-output/<preset>; the MAPLETHIEF_TEST_OUTPUT_DIR environment variable
# overrides it when a test runs.
set(MAPLETHIEF_TEST_OUTPUT_DIR
    "${CMAKE_BINARY_DIR}/test-output"
    CACHE PATH "Directory for test output files such as golden-image diffs (ADR 0005)")

# One main() for every doctest executable.
add_library(maplethief_doctest_main OBJECT cpp/doctest_main.cpp)
target_link_libraries(maplethief_doctest_main PUBLIC doctest::doctest)
# Let doctest include <ostream> instead of forward-declaring std::basic_ostream itself. Without
# it, MSVC's STL (VS 2026, MSVC 14.51) fails to compile any CHECK that prints a
# std::string_view ("use of undefined type std::basic_ostream"). PUBLIC, so that every test
# translation unit sees the same doctest configuration as the main().
target_compile_definitions(maplethief_doctest_main PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS)
maplethief_target_defaults(maplethief_doctest_main)

# maplethief_add_cpp_test(<target> AREA <area> SOURCES <src>... [LIBRARIES <lib>...])
#
# Builds <target> from the sources plus the shared doctest main, and registers each of its
# test cases with CTest as "<area>.<test case name>".
function(maplethief_add_cpp_test target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "AREA" "SOURCES;LIBRARIES")
    if(NOT arg_AREA OR NOT arg_SOURCES)
        message(FATAL_ERROR "maplethief_add_cpp_test(${target}): AREA and SOURCES are required")
    endif()
    add_executable(${target} ${arg_SOURCES})
    target_link_libraries(${target} PRIVATE maplethief_doctest_main ${arg_LIBRARIES})
    maplethief_target_defaults(${target})
    doctest_discover_tests(
        ${target}
        TEST_PREFIX "${arg_AREA}."
        PROPERTIES LABELS cpp TIMEOUT 60)
endfunction()

maplethief_add_cpp_test(
    runtime_test
    AREA runtime
    SOURCES cpp/runtime_test.cpp
            cpp/recomp_headers_test.cpp
            cpp/imports_test.cpp
            cpp/memory_test.cpp
            cpp/diag_test.cpp
            cpp/libc_test.cpp
            cpp/cxxabi_test.cpp
            cpp/trace_entry_fixture.c
            cpp/guest_trap.cpp
    LIBRARIES runtime)
# libc_test.cpp reaches the runtime's internal HLE headers (runtime/src/hle/format.h, hle.h).
target_include_directories(runtime_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")

# The guest that crashes on purpose (ADR 0030): diag_test.cpp runs it as a child process to
# see the crash reporter's output for faults that kill the process. ENABLE_EXPORTS: dladdr
# finds its f_XXXXXXXX function from the host PC, as in the game executable.
add_executable(crash_child cpp/crash_child.cpp)
target_link_libraries(crash_child PRIVATE runtime)
set_target_properties(crash_child PROPERTIES ENABLE_EXPORTS ON)
maplethief_target_defaults(crash_child)
add_dependencies(runtime_test crash_child)
target_compile_definitions(runtime_test PRIVATE MT_CRASH_CHILD="$<TARGET_FILE:crash_child>")

# The real image in guest memory (ADR 0025): maps work/slices/MapleThief.armv7 when
# scripts/bootstrap has written it, and is reported as skipped otherwise (always in CI).
add_executable(memory_real_test cpp/memory_real_test.cpp cpp/guest_trap.cpp)
target_link_libraries(memory_real_test PRIVATE runtime)
target_compile_definitions(
    memory_real_test PRIVATE MT_ARMV7_SLICE="${PROJECT_SOURCE_DIR}/work/slices/MapleThief.armv7")
maplethief_target_defaults(memory_real_test)
add_test(NAME runtime.memory_real_image COMMAND memory_real_test)
set_tests_properties(runtime.memory_real_image PROPERTIES LABELS "cpp;real_assets" SKIP_RETURN_CODE
                                                          77 TIMEOUT 60)

# The Objective-C runtime (ADR 0026) defines its own mt_trap / mt_dispatch test doubles, so it
# is a separate executable. The offline service stubs (#52) are host classes of that runtime and
# are tested with it; stubs_test.cpp also tests their JSON code (runtime/src/stubs/json.h). So are
# CoreFoundation (#48), Foundation (#49) and UIKit (#54).
maplethief_add_cpp_test(
    objc_test
    AREA objc
    SOURCES cpp/objc_test.cpp
            cpp/objc_services_test.cpp
            cpp/stubs_test.cpp
            cpp/cf_test.cpp
            cpp/cg_test.cpp
            cpp/foundation_test.cpp
            cpp/uikit_test.cpp
            cpp/host_surface_test.cpp
            cpp/gles_test.cpp
            cpp/gles_fixed_test.cpp
            cpp/audio_test.cpp
            cpp/extaudiofile_test.cpp
            cpp/mp3_test.cpp
    LIBRARIES runtime maplethief_golden maplethief::dr_mp3)
# stubs_test.cpp (#52), cf_test.cpp (#48), cg_test.cpp (#51), foundation_test.cpp (#49),
# uikit_test.cpp (#54), gles_test.cpp (#117), gles_fixed_test.cpp (#56), audio_test.cpp (#61,
# OpenAL through the mixer, sample-exact on synthetic PCM), extaudiofile_test.cpp (#62, WAV
# decoding on synthetic files) and mp3_test.cpp (#63, AVAudioPlayer on the synthetic MP3s of
# tests/cpp/data/audio/, decoded by the runtime's dr_mp3, whose header it includes for the
# reference decode) reach the runtime's
# internal headers (runtime/src/); cg_test.cpp, uikit_test.cpp and gles_fixed_test.cpp check
# golden images of synthetic drawings; host_surface_test.cpp reads the committed host-surface
# table.
target_include_directories(objc_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(
    objc_test PRIVATE MT_GOLDEN_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/golden/data"
                      MT_HOST_SURFACE_DOC="${PROJECT_SOURCE_DIR}/docs/re/host-surface.md"
                      MT_AUDIO_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/cpp/data/audio")
# The GL pipeline's goldens are exact on every CPU (ADR 0039, 0040): no contraction there either;
# nor in the mixer's expected samples (ADR 0045).
maplethief_strict_fp(cpp/gles_fixed_test.cpp cpp/audio_test.cpp cpp/mp3_test.cpp)

# mt_gfx, the rendering backend (#55, ADR 0039): rasterization rules and golden images of
# synthetic scenes, exact on every platform. It also builds the frontend's prototype scene
# (platform/src/gfx_prototype.cpp, no SDL in it) to compare it with its golden image, and reaches
# the runtime's internal gfx header (runtime/src/gfx/gfx_internal.h).
maplethief_add_cpp_test(
    gfx_test
    AREA gfx
    SOURCES cpp/gfx_test.cpp "${PROJECT_SOURCE_DIR}/platform/src/gfx_prototype.cpp"
    LIBRARIES runtime maplethief_golden)
target_include_directories(gfx_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src"
                                            "${PROJECT_SOURCE_DIR}/platform/src")
target_compile_definitions(gfx_test PRIVATE MT_GOLDEN_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/golden/data")
# The scenes' vertices are computed in floats: the same on every CPU only without contraction.
maplethief_strict_fp(cpp/gfx_test.cpp "${PROJECT_SOURCE_DIR}/platform/src/gfx_prototype.cpp")

# mt_gfx's GPU backend (#120, ADR 0060) against the software rasterizer: gfx_test's golden scenes
# on the GPU within ADR 0060's tolerance, internal scale 4, the sync points, batching and a busy
# frame's GPU time. Local only: without a GPU device (CI, the Linux container) and on GitHub
# Actions every case prints "gfx_gpu: skipped", which CTest reports as skipped. One label only:
# doctest_discover_tests flattens a list value into the property arguments.
add_executable(gfx_gpu_test cpp/gfx_gpu_test.cpp)
target_link_libraries(gfx_gpu_test PRIVATE maplethief_doctest_main maplethief_gfx_gpu
                                           maplethief_golden)
target_sources(gfx_gpu_test PRIVATE "${PROJECT_SOURCE_DIR}/platform/src/gfx_prototype.cpp")
target_include_directories(gfx_gpu_test PRIVATE "${PROJECT_SOURCE_DIR}/platform/src")
target_compile_definitions(gfx_gpu_test PRIVATE MT_GOLDEN_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/golden/data")
maplethief_strict_fp(cpp/gfx_gpu_test.cpp)
maplethief_target_defaults(gfx_gpu_test)
doctest_discover_tests(
    gfx_gpu_test
    TEST_PREFIX "gfx_gpu."
    PROPERTIES LABELS gpu TIMEOUT 120 SKIP_REGULAR_EXPRESSION "gfx_gpu: skipped")

# The GPU shaders' committed header is up to date with its HLSL sources (#120, ADR 0060):
# scripts/build-shaders --check, without the container.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter) # the top level's, found later
add_test(NAME shaders.stamps
         COMMAND "${Python3_EXECUTABLE}" -I -B "${PROJECT_SOURCE_DIR}/tools/mtport/shaderbuild.py"
                 check --repo "${PROJECT_SOURCE_DIR}")
set_tests_properties(shaders.stamps PROPERTIES TIMEOUT 30)

# The frontend (#58, ADR 0042): the fit rectangle, the armv7 slice of the bundle's executable,
# the bundle's precedence, and the presentation chain on a synthetic guest (GL -> the present
# hook -> the compositor -> an SDL texture -> a dummy-driver window, read back). Scaling (#59,
# ADR 0043, scaling_test.cpp): every mode's placement, the mouse inverse, settings.ini, and
# read-backs at 1080p, 1440p and a Retina-sized drawable. The audio output (#61, ADR 0045,
# audio_out_test.cpp): the capture's frames and bytes, late callbacks, the dummy-driver device
# stream. Touch input (#65, ADR 0047, input_test.cpp): SDL mouse and finger events pushed on a
# dummy-driver window, through Input into a synthetic main menu and key pad. Gamepads and
# actions (#67, ADR 0048, gamepad_test.cpp): SDL3 virtual joysticks of the Xbox 360, PS4 and
# PS5 (mapped) kinds through Gamepads and Actions, and the runtime's action delivery into a
# synthetic EAGLView, TouchKeyPadView and SkyCanvas. The keyboard (#68, keyboard_test.cpp):
# synthetic SDL key events through Keyboard and Actions, against docs/controls/keyboard.md's
# table. The key pad's hiding (#71, ADR 0052, keypad_hide_test.cpp): the modes, every input
# source, a hide waiting for a touch to end, the fade, hide_keypad, the opacity on the pad's
# panel 0. Controller navigation (#69, ADR 0051, focus_test.cpp): spatial navigation, the
# highlight, Focus on scripted targets, and mt_uikit_controls on a synthetic view tree. Reaches
# the runtime's internal UIKit and GL headers to set up the guest's window and GL calls.
maplethief_add_cpp_test(
    platform_test
    AREA platform
    SOURCES cpp/platform_test.cpp cpp/scaling_test.cpp cpp/clock_test.cpp cpp/audio_out_test.cpp
            cpp/input_test.cpp cpp/gamepad_test.cpp cpp/keyboard_test.cpp
            cpp/keypad_hide_test.cpp cpp/focus_test.cpp cpp/lifecycle_test.cpp
    LIBRARIES maplethief_frontend runtime SDL3::SDL3)
# The audio output's expected capture samples round as the capture does (ADR 0045).
maplethief_strict_fp(cpp/audio_out_test.cpp)
target_include_directories(platform_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(platform_test PRIVATE MT_REPO_DIR="${PROJECT_SOURCE_DIR}")

# CoreFoundation on the real bundle and slice (#48, ADR 0032): every bundle file through the
# game's resource calls, every CFString literal; skipped without work/ipa (always in CI).
add_executable(cf_real_test cpp/cf_real_test.cpp cpp/guest_trap.cpp)
target_link_libraries(cf_real_test PRIVATE runtime)
target_include_directories(cf_real_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(
    cf_real_test PRIVATE MT_ARMV7_SLICE="${PROJECT_SOURCE_DIR}/work/slices/MapleThief.armv7"
                         MT_BUNDLE_DIR="${PROJECT_SOURCE_DIR}/work/ipa/Payload/MapleThief.app")
maplethief_target_defaults(cf_real_test)
add_test(NAME cf.real_bundle COMMAND cf_real_test)
set_tests_properties(cf.real_bundle PROPERTIES LABELS "cpp;real_assets" SKIP_RETURN_CODE 77
                                               TIMEOUT 120)

# CoreGraphics / UIKit drawing on the real bundle (#51, ADR 0036): every shipped PNG (CgBI
# included) and every PNG of the png/**/*.dat packs through UIImage; skipped without work/ipa
# (always in CI).
add_executable(cg_real_test cpp/cg_real_test.cpp cpp/guest_trap.cpp)
target_link_libraries(cg_real_test PRIVATE runtime)
target_include_directories(cg_real_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(
    cg_real_test PRIVATE MT_BUNDLE_DIR="${PROJECT_SOURCE_DIR}/work/ipa/Payload/MapleThief.app")
maplethief_target_defaults(cg_real_test)
add_test(NAME cg.real_bundle_images COMMAND cg_real_test)
set_tests_properties(cg.real_bundle_images PROPERTIES LABELS "cpp;real_assets" SKIP_RETURN_CODE
                                                      77 TIMEOUT 120)

# AVAudioPlayer's MP3 music on the real bundle (#63, ADR 0046): all 18 MP3s open with the
# frame walk's length, stream to their end, loop past it, and the menu track mixes exactly across
# its loop point; on macOS, every file and the synthetic fixtures also against Apple's own decoder
# (ExtAudioFile). Skipped without work/ipa (always in CI).
add_executable(mp3_real_test cpp/mp3_real_test.cpp)
target_link_libraries(mp3_real_test PRIVATE runtime)
if(APPLE)
    target_link_libraries(mp3_real_test PRIVATE "-framework AudioToolbox"
                                                "-framework CoreFoundation")
endif()
target_include_directories(mp3_real_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(
    mp3_real_test PRIVATE MT_BUNDLE_DIR="${PROJECT_SOURCE_DIR}/work/ipa/Payload/MapleThief.app"
                          MT_AUDIO_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/cpp/data/audio")
maplethief_target_defaults(mp3_real_test)
add_test(NAME audio.real_mp3 COMMAND mp3_real_test)
set_tests_properties(audio.real_mp3 PROPERTIES LABELS "cpp;real_assets" SKIP_RETURN_CODE 77
                                               TIMEOUT 300)

# Apple's CoreGraphics as the reference for the texture loader's premultiplied context (#57,
# ADR 0041), macOS only: synthetic images always (CI), every shipped PNG when work/ipa is there.
if(APPLE)
    add_executable(cg_apple_test cpp/cg_apple_test.cpp cpp/guest_trap.cpp)
    target_link_libraries(cg_apple_test PRIVATE runtime "-framework CoreFoundation"
                                                "-framework CoreGraphics" "-framework ImageIO")
    target_include_directories(cg_apple_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
    target_compile_definitions(
        cg_apple_test PRIVATE MT_BUNDLE_DIR="${PROJECT_SOURCE_DIR}/work/ipa/Payload/MapleThief.app")
    maplethief_target_defaults(cg_apple_test)
    add_test(NAME cg.apple_reference COMMAND cg_apple_test)
    set_tests_properties(cg.apple_reference PROPERTIES LABELS cpp TIMEOUT 300)
endif()

# Foundation on the real bundle (#49, ADR 0035): every binary plist parses, every localized
# string answers through NSBundle; skipped without work/ipa (always in CI).
add_executable(foundation_real_test cpp/foundation_real_test.cpp cpp/guest_trap.cpp)
target_link_libraries(foundation_real_test PRIVATE runtime)
target_include_directories(foundation_real_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(
    foundation_real_test
    PRIVATE MT_BUNDLE_DIR="${PROJECT_SOURCE_DIR}/work/ipa/Payload/MapleThief.app")
maplethief_target_defaults(foundation_real_test)
add_test(NAME foundation.real_bundle COMMAND foundation_real_test)
set_tests_properties(foundation.real_bundle PROPERTIES LABELS "cpp;real_assets"
                                                       SKIP_RETURN_CODE 77 TIMEOUT 120)

# ExtAudioFile on the real bundle (#62, ADR 0045): every shipped WAV through the game's
# MyGetOpenALAudioData calls and alBufferData, against a strict reference reader and the
# generated table of docs/re/audio.md; skipped without work/ipa (always in CI).
add_executable(audio_real_test cpp/audio_real_test.cpp cpp/guest_trap.cpp)
target_link_libraries(audio_real_test PRIVATE runtime)
target_include_directories(audio_real_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(
    audio_real_test PRIVATE MT_BUNDLE_DIR="${PROJECT_SOURCE_DIR}/work/ipa/Payload/MapleThief.app"
                            MT_AUDIO_DOC="${PROJECT_SOURCE_DIR}/docs/re/audio.md")
maplethief_target_defaults(audio_real_test)
add_test(NAME audio.real_wavs COMMAND audio_real_test)
set_tests_properties(audio.real_wavs PROPERTIES LABELS "cpp;real_assets" SKIP_RETURN_CODE 77
                                                TIMEOUT 120)

# The ObjC runtime running recompiled methods of the real image (#47, ADR 0029). Only when the
# recompiler's output is there (target maplethief_gen, which also compiles
# runtime/src/recomp/dispatch.c; never in CI); skipped without the slice.
if(TARGET maplethief_gen)
    add_executable(objc_real_test cpp/objc_real_test.cpp cpp/guest_trap.cpp)
    target_link_libraries(objc_real_test PRIVATE maplethief_gen runtime)
    target_compile_definitions(
        objc_real_test PRIVATE MT_ARMV7_SLICE="${PROJECT_SOURCE_DIR}/work/slices/MapleThief.armv7")
    maplethief_target_defaults(objc_real_test)
    add_test(NAME objc.real_image_app_delegate COMMAND objc_real_test)
    set_tests_properties(objc.real_image_app_delegate PROPERTIES LABELS "cpp;real_assets"
                                                                 SKIP_RETURN_CODE 77 TIMEOUT 60)

    # The offline stubs (#52, ADR 0034) under the game's recompiled GameCenterManager and
    # InAppPurchaseManager.
    add_executable(stubs_real_test cpp/stubs_real_test.cpp cpp/guest_trap.cpp)
    target_link_libraries(stubs_real_test PRIVATE maplethief_gen runtime)
    # Foundation's host API (runtime/src/foundation/foundation.h) makes the test's strings.
    target_include_directories(stubs_real_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
    target_compile_definitions(
        stubs_real_test PRIVATE MT_ARMV7_SLICE="${PROJECT_SOURCE_DIR}/work/slices/MapleThief.armv7")
    maplethief_target_defaults(stubs_real_test)
    add_test(NAME stubs.real_image_game_center_and_store COMMAND stubs_real_test)
    set_tests_properties(stubs.real_image_game_center_and_store
                         PROPERTIES LABELS "cpp;real_assets" SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

# GCD (ADR 0033): queues, worker threads, groups, timers, dispatch_once. It defines its own
# mt_trap / mt_dispatch test doubles, so it is a separate executable.
maplethief_add_cpp_test(
    gcd_test
    AREA gcd
    SOURCES cpp/gcd_test.cpp
    LIBRARIES runtime)

# Process start-up (ADR 0027) on a synthetic Mach-O. It links the runtime's own mt_trap and
# defines an mt_dispatch test double, so it is a separate executable.
maplethief_add_cpp_test(
    boot_test
    AREA boot
    SOURCES cpp/boot_test.cpp
    LIBRARIES runtime)

# The real boot (ADR 0027), launch (#54, ADR 0037) and first GL frame (#117, ADR 0038): the
# generated program (maplethief_gen) and the runtime start the armv7 slice, load MainWindow.nib
# and run the main run loop: runtime.boot_real until the first GL frame (M4's exit criterion),
# runtime.launch_real for a few seconds of the main menu with taps. Needs work/gen (python -m
# recomp all), the slice and work/ipa; without them the test is built without the generated code
# and reports itself skipped (CI). Its renders of the
# screen go to the test output directory (build/, never committed).
add_executable(boot_real_test cpp/boot_real_test.cpp)
target_link_libraries(boot_real_test PRIVATE runtime maplethief_golden)
target_include_directories(boot_real_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(
    boot_real_test
    PRIVATE MT_ARMV7_SLICE="${PROJECT_SOURCE_DIR}/work/slices/MapleThief.armv7"
            MT_BUNDLE_DIR="${PROJECT_SOURCE_DIR}/work/ipa/Payload/MapleThief.app")
if(TARGET maplethief_gen)
    target_link_libraries(boot_real_test PRIVATE maplethief_gen)
    target_compile_definitions(boot_real_test PRIVATE MT_HAVE_GENERATED_CODE=1)
endif()
maplethief_target_defaults(boot_real_test)
add_test(NAME runtime.boot_real COMMAND boot_real_test first-frame)
set_tests_properties(runtime.boot_real PROPERTIES LABELS "cpp;real_assets" SKIP_RETURN_CODE 77
                                                  TIMEOUT 120)
add_test(NAME runtime.launch_real COMMAND boot_real_test launch)
set_tests_properties(runtime.launch_real PROPERTIES LABELS "cpp;real_assets" SKIP_RETURN_CODE 77
                                                    TIMEOUT 120 RUN_SERIAL TRUE)

# The gamepad in the real game (#67, ADR 0048): the game boots fresh with the virtual clock
# (MapleThief --fixed-step 1), and the frontend's own input layer (Input, Actions, Gamepads) is
# driven from the turn hook with SDL3 virtual joysticks: input.gamepad_play walks, jumps,
# attacks, uses a skill and drinks potions with an Xbox 360, a PS4 and a PS5 pad hot-plugged in
# turn and checks the hero in guest memory; input.gamepad_hold holds the d-pad and attack once
# as key pad touches and once as the pad, on the same turns, and the hero's per-tick x, y and
# state must be identical (platform/gamepad_test.cmake). Skipped (77) without the generated
# code, the slice or the bundle. Output to the test output directory (build/, never committed).
add_executable(gamepad_real_test cpp/gamepad_real_test.cpp)
target_link_libraries(gamepad_real_test PRIVATE maplethief_frontend runtime SDL3::SDL3)
target_include_directories(gamepad_real_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(
    gamepad_real_test
    PRIVATE MT_ARMV7_SLICE="${PROJECT_SOURCE_DIR}/work/slices/MapleThief.armv7"
            MT_BUNDLE_DIR="${PROJECT_SOURCE_DIR}/work/ipa/Payload/MapleThief.app")
if(TARGET maplethief_gen)
    target_link_libraries(gamepad_real_test PRIVATE maplethief_gen)
    target_compile_definitions(gamepad_real_test PRIVATE MT_HAVE_GENERATED_CODE=1)
endif()
maplethief_target_defaults(gamepad_real_test)
add_test(NAME input.gamepad_play COMMAND gamepad_real_test play
                                         "${MAPLETHIEF_TEST_OUTPUT_DIR}/gamepad_play")
set_tests_properties(input.gamepad_play PROPERTIES LABELS "cpp;real_assets" SKIP_RETURN_CODE 77
                                                   TIMEOUT 300 RUN_SERIAL TRUE)
add_test(
    NAME input.gamepad_hold
    COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:gamepad_real_test>"
            "-DOUT=${MAPLETHIEF_TEST_OUTPUT_DIR}/gamepad_hold" -P
            "${PROJECT_SOURCE_DIR}/platform/gamepad_test.cmake")
set_tests_properties(input.gamepad_hold PROPERTIES LABELS "cpp;real_assets" SKIP_REGULAR_EXPRESSION
                                                   "skipped:" TIMEOUT 300 RUN_SERIAL TRUE)
# The key pad's hiding (#71, ADR 0052): the same program's keypad run (auto, always and never on
# one input stream; the first map hidden vs shown differs only inside the key pad's rects) and
# its hold run with the pad hidden, whose hero and GL frames must match the shown runs'
# (platform/keypad_hide_test.cmake).
add_test(
    NAME input.keypad_hide
    COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:gamepad_real_test>"
            "-DOUT=${MAPLETHIEF_TEST_OUTPUT_DIR}/input_keypad_hide" -P
            "${PROJECT_SOURCE_DIR}/platform/keypad_hide_test.cmake")
set_tests_properties(input.keypad_hide PROPERTIES LABELS "cpp;real_assets" SKIP_REGULAR_EXPRESSION
                                                  "skipped:" TIMEOUT 900 RUN_SERIAL TRUE)

# The keyboard in the real game (#68, ADR 0048's addendum): as the gamepad's, from the turn hook
# with synthetic SDL key events (and the held keys' repeats) through Input, Keyboard and Actions:
# input.keyboard_play plays the first map by keys and checks the hero in guest memory;
# input.keyboard_hold holds Left, Z and Right once as key pad touches and once as keys, on the
# same turns, and the per-tick hero must be identical; input.keyboard_menus goes through the main
# menu and the character screens by keys alone (platform/keyboard_test.cmake). Skipped (77)
# without the generated code, the slice or the bundle. Output to the test output directory.
add_executable(keyboard_real_test cpp/keyboard_real_test.cpp)
target_link_libraries(keyboard_real_test PRIVATE maplethief_frontend runtime SDL3::SDL3)
target_include_directories(keyboard_real_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(
    keyboard_real_test
    PRIVATE MT_ARMV7_SLICE="${PROJECT_SOURCE_DIR}/work/slices/MapleThief.armv7"
            MT_BUNDLE_DIR="${PROJECT_SOURCE_DIR}/work/ipa/Payload/MapleThief.app")
if(TARGET maplethief_gen)
    target_link_libraries(keyboard_real_test PRIVATE maplethief_gen)
    target_compile_definitions(keyboard_real_test PRIVATE MT_HAVE_GENERATED_CODE=1)
endif()
maplethief_target_defaults(keyboard_real_test)
add_test(NAME input.keyboard_play COMMAND keyboard_real_test play
                                          "${MAPLETHIEF_TEST_OUTPUT_DIR}/keyboard_play")
set_tests_properties(input.keyboard_play PROPERTIES LABELS "cpp;real_assets" SKIP_RETURN_CODE 77
                                                    TIMEOUT 300 RUN_SERIAL TRUE)
foreach(what hold menus)
    add_test(
        NAME input.keyboard_${what}
        COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:keyboard_real_test>" -DWHAT=${what}
                "-DOUT=${MAPLETHIEF_TEST_OUTPUT_DIR}/keyboard_${what}" -P
                "${PROJECT_SOURCE_DIR}/platform/keyboard_test.cmake")
    set_tests_properties(input.keyboard_${what} PROPERTIES LABELS "cpp;real_assets"
                         SKIP_REGULAR_EXPRESSION "skipped:" TIMEOUT 300 RUN_SERIAL TRUE)
endforeach()

# Controller navigation in the real game (#69, ADR 0051): every menu, screen and popup done with
# a virtual Xbox 360 pad only (no taps), through the frontend's Gamepads, Actions and Focus:
# input.menu_main (the main menu, Help, About and its links, Game Center, More Games, Play),
# input.menu_terms (the Korean terms and the 2-page help), input.menu_early (A on Play at 0.05 s,
# before the engine's main menu: dropped, #129; then Play), input.menu_characters (create,
# select and delete, platform/menu_nav_test.cmake running the flows on one data directory) and
# input.menu_game (NPC chat, the popup menu and its shortcut panel, the shops). Checks the uikit
# trace's button actions, the save files and guest memory. Skipped (77) without the generated
# code, the slice or the bundle. Output to the test output directory (build/, never committed).
add_executable(menu_nav_real_test cpp/menu_nav_real_test.cpp)
target_link_libraries(menu_nav_real_test PRIVATE maplethief_frontend runtime SDL3::SDL3)
target_include_directories(menu_nav_real_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(
    menu_nav_real_test
    PRIVATE MT_ARMV7_SLICE="${PROJECT_SOURCE_DIR}/work/slices/MapleThief.armv7"
            MT_BUNDLE_DIR="${PROJECT_SOURCE_DIR}/work/ipa/Payload/MapleThief.app")
if(TARGET maplethief_gen)
    target_link_libraries(menu_nav_real_test PRIVATE maplethief_gen)
    target_compile_definitions(menu_nav_real_test PRIVATE MT_HAVE_GENERATED_CODE=1)
endif()
maplethief_target_defaults(menu_nav_real_test)
foreach(flow main terms early characters game)
    add_test(
        NAME input.menu_${flow}
        COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:menu_nav_real_test>" "-DFLOW=${flow}"
                "-DOUT=${MAPLETHIEF_TEST_OUTPUT_DIR}/menu_${flow}" -P
                "${PROJECT_SOURCE_DIR}/platform/menu_nav_test.cmake")
    set_tests_properties(input.menu_${flow} PROPERTIES LABELS "cpp;real_assets"
                                                       SKIP_REGULAR_EXPRESSION "skipped:"
                                                       TIMEOUT 600 RUN_SERIAL TRUE)
endforeach()

# M6's exit criteria through the game's own sound code (#124, ADR 0045, 0046): the real game
# boots to its main menu, then the test calls CMSoundPlayer::Play for every id 0-43
# (audio.game_sounds: each effect's exact length, each track's loop) and CStateGame::PlayMapBGM
# for every shipped map (audio.game_map_music: against docs/re/audio.md's generated maps
# table), pulling the mix itself; audio.game_gapless_music plays every track with the gapless
# option on (#125, ADR 0062: at most one MPEG frame of silence at each loop point). Skipped (77)
# without the generated code, the slice or the bundle.
add_executable(game_audio_real_test cpp/game_audio_real_test.cpp)
target_link_libraries(game_audio_real_test PRIVATE runtime)
if(APPLE) # Apple's decode of the MP3s, the loop points' reference (apple_decode.h)
    target_link_libraries(game_audio_real_test PRIVATE "-framework AudioToolbox"
                                                       "-framework CoreFoundation")
endif()
target_include_directories(game_audio_real_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(
    game_audio_real_test
    PRIVATE MT_ARMV7_SLICE="${PROJECT_SOURCE_DIR}/work/slices/MapleThief.armv7"
            MT_BUNDLE_DIR="${PROJECT_SOURCE_DIR}/work/ipa/Payload/MapleThief.app"
            MT_AUDIO_DOC="${PROJECT_SOURCE_DIR}/docs/re/audio.md")
if(TARGET maplethief_gen)
    target_link_libraries(game_audio_real_test PRIVATE maplethief_gen)
    target_compile_definitions(game_audio_real_test PRIVATE MT_HAVE_GENERATED_CODE=1)
endif()
maplethief_target_defaults(game_audio_real_test)
add_test(NAME audio.game_sounds COMMAND game_audio_real_test sounds)
add_test(NAME audio.game_map_music COMMAND game_audio_real_test maps)
add_test(NAME audio.game_gapless_music COMMAND game_audio_real_test gapless)
set_tests_properties(audio.game_sounds audio.game_map_music audio.game_gapless_music
                     PROPERTIES LABELS "cpp;real_assets" SKIP_RETURN_CODE 77 TIMEOUT 300)

# The frontend running the real game (#58, ADR 0042): MapleThief, linked with maplethief_gen,
# boots the game from work/ipa headless (SDL's dummy video and audio drivers), taps Play on the
# main menu at 3 s (a scripted tap) and runs for 150 window frames, into the first map. The last
# frame is read
# back and must show the composited screen, not black (exit status 3 otherwise). The read-backs
# (the first frame, the first that is not black, the first with a GL frame that is not black,
# the last) go to the test output directory (build/, never committed), and so does the data
# directory, emptied first so that Play starts a new game. Only with generated code, so CI never
# registers it.
if(TARGET maplethief_gen)
    set(_game_boot_out "${MAPLETHIEF_TEST_OUTPUT_DIR}/game_boot")
    add_test(NAME platform.game_boot_clean COMMAND "${CMAKE_COMMAND}" -E rm -rf
                                                   "${_game_boot_out}")
    set_tests_properties(platform.game_boot_clean PROPERTIES FIXTURES_SETUP game_boot_dir)
    add_test(NAME platform.game_boot COMMAND MapleThief --frames 150 --tap 3,106,165 --capture
                                             "${_game_boot_out}")
    set_tests_properties(
        platform.game_boot
        PROPERTIES LABELS "real_assets" TIMEOUT 120 RUN_SERIAL TRUE FIXTURES_REQUIRED game_boot_dir
                   ENVIRONMENT
                   "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;MAPLETHIEF_DATA_DIR=${_game_boot_out}/data"
    )

    # The deterministic audio capture (#61, ADR 0045): the same scripted --fixed-step run twice,
    # each with --audio-capture, must write the same WAV byte for byte
    # (platform/audio_capture_test.cmake). The captures and data directories go to the test
    # output directory (build/, never committed).
    add_test(
        NAME platform.audio_capture
        COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:MapleThief>"
                "-DOUT=${MAPLETHIEF_TEST_OUTPUT_DIR}/audio_capture" -P
                "${PROJECT_SOURCE_DIR}/platform/audio_capture_test.cmake")
    set_tests_properties(platform.audio_capture PROPERTIES LABELS "real_assets" TIMEOUT 300
                                                           RUN_SERIAL TRUE)

    # --seconds S (#124, the soak's time limit): the game ends by itself at 2 s of virtual time
    # (--fixed-step) and after 3 s of real time with the device stream on SDL's dummy driver,
    # logging the audio output's numbers (platform/seconds_test.cmake).
    add_test(
        NAME platform.game_seconds
        COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:MapleThief>" -DMODE=game
                "-DOUT=${MAPLETHIEF_TEST_OUTPUT_DIR}/game_seconds" -P
                "${PROJECT_SOURCE_DIR}/platform/seconds_test.cmake")
    set_tests_properties(platform.game_seconds PROPERTIES LABELS "real_assets" TIMEOUT 120
                                                          RUN_SERIAL TRUE)

    # The app lifecycle (#128, ADR 0063): a scripted minimise and restore (fixed-step, twice:
    # the same capture) shows Clet_Pause (the music stops, silence and no present while
    # minimised), Clet_Resume (the music again), a tap on Play after it and the quit's
    # applicationWillTerminate:; SDL's iOS home-gesture sequence and a real-time minimise send
    # each message once, in order (platform/lifecycle_test.cmake). Logs, captures and data
    # directories go to the test output directory (build/, never committed).
    add_test(
        NAME platform.game_lifecycle
        COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:MapleThief>" -DMODE=game
                "-DOUT=${MAPLETHIEF_TEST_OUTPUT_DIR}/game_lifecycle" -P
                "${PROJECT_SOURCE_DIR}/platform/lifecycle_test.cmake")
    set_tests_properties(platform.game_lifecycle PROPERTIES LABELS "real_assets" TIMEOUT 600
                                                            RUN_SERIAL TRUE)

    # Mouse clicks into the game (#65, ADR 0047): --tap clicks (the left mouse button through
    # the frontend's Input, as a real click) through the device goldens' fresh run up to the key
    # pad's cash button; the uikit trace must show -[MainMenuView clickPlay:] and the key pad's
    # clickCashButton: / releaseCashButton: (platform/input_test.cmake). Log and data directory
    # go to the test output directory (build/, never committed).
    add_test(
        NAME platform.mouse_clicks
        COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:MapleThief>" -DMODE=game
                "-DOUT=${MAPLETHIEF_TEST_OUTPUT_DIR}/mouse_clicks" -P
                "${PROJECT_SOURCE_DIR}/platform/input_test.cmake")
    set_tests_properties(platform.mouse_clicks PROPERTIES LABELS "real_assets" TIMEOUT 600
                                                          RUN_SERIAL TRUE)

    # A tap on Play before the engine's main menu (#122, #129): --tap 0.1,106,165 under
    # --fixed-step 1 still ends the game with the device's exception (exit status 5); the
    # controller focus's guard leaves touches alone (platform/input_test.cmake).
    add_test(
        NAME platform.early_tap
        COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:MapleThief>" -DMODE=early_tap
                "-DOUT=${MAPLETHIEF_TEST_OUTPUT_DIR}/early_tap" -P
                "${PROJECT_SOURCE_DIR}/platform/input_test.cmake")
    set_tests_properties(platform.early_tap PROPERTIES LABELS "real_assets" TIMEOUT 120
                                                       RUN_SERIAL TRUE)

    # The title for a minute (#74): mtport.titlerun drives MapleThief --fixed-step 16.667
    # --seconds 60 headless from a fresh data directory, and fails unless the run ends cleanly
    # at 60 s with no crash, trap or GL misuse, the unimplemented report reads "none", the
    # frame loop presents GL frames for the whole minute, the slide-in's shots all differ and
    # the still title's shots (2.5 s to 60 s) are identical and not black. Log, report, shots
    # and data directory go to the test output directory (build/, never committed).
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter) # the top level's, found later
    add_test(
        NAME platform.title_60s
        COMMAND
            "${Python3_EXECUTABLE}" -X "pycache_prefix=${CMAKE_BINARY_DIR}/pycache" -m
            mtport.titlerun soak --binary "$<TARGET_FILE:MapleThief>" --out
            "${MAPLETHIEF_TEST_OUTPUT_DIR}/title_60s")
    set_tests_properties(
        platform.title_60s
        PROPERTIES LABELS "real_assets" TIMEOUT 900 RUN_SERIAL TRUE SKIP_RETURN_CODE 77
                   ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/tools")

    # The menus before the game (#75): mtport.menuflows clicks MapleThief --fixed-step headless
    # through one flow each (help and about; the Korean terms and the help swipe; the online
    # buttons; a new game; New, choose + Start and Delete on the character select) and checks
    # the uikit trace (actions, openURL: refused, the leaderboard dismissed), the save files in
    # the data directory and the shots by hash (a closed screen leaves the menu as it was; the
    # loading screen shows). Skipped (77) when the game cannot start. Logs, shots and data
    # directories go to the test output directory (build/, never committed).
    foreach(_flow help_about terms online new_game create select delete)
        add_test(
            NAME menus.${_flow}
            COMMAND
                "${Python3_EXECUTABLE}" -X "pycache_prefix=${CMAKE_BINARY_DIR}/pycache" -m
                mtport.menuflows --binary "$<TARGET_FILE:MapleThief>" --flow ${_flow} --out
                "${MAPLETHIEF_TEST_OUTPUT_DIR}/menus")
        set_tests_properties(
            menus.${_flow}
            PROPERTIES LABELS "real_assets" TIMEOUT 900 RUN_SERIAL TRUE SKIP_RETURN_CODE 77
                       ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/tools")
    endforeach()

    # The device golden screens (#60, ADR 0044): mtport.devicegolden drives MapleThief
    # --fixed-step headless through the runs of tests/golden/device_screens.json (fresh install,
    # then a second launch on its saves) and compares each screen with our recorded render
    # (work/golden/ours/, exact; MAPLETHIEF_UPDATE_GOLDEN=1 records it) and with the device
    # screenshot (work/golden/device/, normalised, masks and tolerance from the list). A missing
    # golden skips its case; the test is skipped (77) when the game cannot start or every case
    # was skipped. Shots, diffs and the runs' data directories go to the test output directory.
    cmake_path(GET MAPLETHIEF_GEN_DIR PARENT_PATH _device_golden_work)
    cmake_path(ABSOLUTE_PATH _device_golden_work BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE)
    add_test(
        NAME platform.device_golden
        COMMAND
            "${Python3_EXECUTABLE}" -X "pycache_prefix=${CMAKE_BINARY_DIR}/pycache" -m
            mtport.devicegolden --binary "$<TARGET_FILE:MapleThief>" --screens
            "${PROJECT_SOURCE_DIR}/tests/golden/device_screens.json" --golden-dir
            "${_device_golden_work}/golden" --out "${MAPLETHIEF_TEST_OUTPUT_DIR}/device_golden")
    set_tests_properties(
        platform.device_golden
        PROPERTIES LABELS "real_assets" TIMEOUT 1800 RUN_SERIAL TRUE SKIP_RETURN_CODE 77
                   ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/tools")

    # The key pad's hiding through the same capture path (#71, ADR 0052): mtport.keypadhide runs
    # the device golden list's fresh run with --hide-keypad never and always (--shot, --shot-gl):
    # the GL frames are identical, the screens differ only inside the key pad's rects, the shown
    # run matches the device golden's render. Skipped (77) when the game cannot start.
    add_test(
        NAME platform.keypad_hide
        COMMAND
            "${Python3_EXECUTABLE}" -X "pycache_prefix=${CMAKE_BINARY_DIR}/pycache" -m
            mtport.keypadhide --binary "$<TARGET_FILE:MapleThief>" --screens
            "${PROJECT_SOURCE_DIR}/tests/golden/device_screens.json" --golden-dir
            "${_device_golden_work}/golden" --out
            "${MAPLETHIEF_TEST_OUTPUT_DIR}/platform_keypad_hide")
    set_tests_properties(
        platform.keypad_hide
        PROPERTIES LABELS "real_assets" TIMEOUT 1800 RUN_SERIAL TRUE SKIP_RETURN_CODE 77
                   ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/tools")

    # The GPU backend against the software rasterizer on the game's frames (#120, ADR 0060):
    # mtport.gpucompare runs the fresh run with --gfx-backend software and gpu, at render scale 1
    # and 4 (--shot, --shot-gl): every GL frame (and the screens at 1x) within ADR 0060's
    # tolerance, and the GPU time per frame at 4x under 2 ms. The GPU runs open a window (SDL GPU
    # has no device under the dummy video driver). Skipped (77) without the game or a GPU.
    add_test(
        NAME platform.gpu_compare
        COMMAND
            "${Python3_EXECUTABLE}" -X "pycache_prefix=${CMAKE_BINARY_DIR}/pycache" -m
            mtport.gpucompare --binary "$<TARGET_FILE:MapleThief>" --screens
            "${PROJECT_SOURCE_DIR}/tests/golden/device_screens.json" --out
            "${MAPLETHIEF_TEST_OUTPUT_DIR}/gpu_compare")
    set_tests_properties(
        platform.gpu_compare
        PROPERTIES LABELS "real_assets;gpu" TIMEOUT 3600 RUN_SERIAL TRUE SKIP_RETURN_CODE 77
                   ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/tools")
endif()

add_subdirectory(golden)
