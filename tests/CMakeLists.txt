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
    LIBRARIES runtime maplethief_golden)
# stubs_test.cpp (#52), cf_test.cpp (#48), cg_test.cpp (#51), foundation_test.cpp (#49),
# uikit_test.cpp (#54), gles_test.cpp (#117) and gles_fixed_test.cpp (#56) reach the runtime's
# internal headers (runtime/src/); cg_test.cpp, uikit_test.cpp and gles_fixed_test.cpp check
# golden images of synthetic drawings; host_surface_test.cpp reads the committed host-surface
# table.
target_include_directories(objc_test PRIVATE "${PROJECT_SOURCE_DIR}/runtime/src")
target_compile_definitions(
    objc_test PRIVATE MT_GOLDEN_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/golden/data"
                      MT_HOST_SURFACE_DOC="${PROJECT_SOURCE_DIR}/docs/re/host-surface.md")
# The GL pipeline's goldens are exact on every CPU (ADR 0039, 0040): no contraction there either.
maplethief_strict_fp(cpp/gles_fixed_test.cpp)

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

# The frontend (#58, ADR 0042): the fit rectangle, the armv7 slice of the bundle's executable,
# the bundle's precedence, and the presentation chain on a synthetic guest (GL -> the present
# hook -> the compositor -> an SDL texture -> a dummy-driver window, read back). Scaling (#59,
# ADR 0043, scaling_test.cpp): every mode's placement, the mouse inverse, settings.ini, and
# read-backs at 1080p, 1440p and a Retina-sized drawable. Reaches the
# runtime's internal UIKit and GL headers to set up the guest's window and GL calls.
maplethief_add_cpp_test(
    platform_test
    AREA platform
    SOURCES cpp/platform_test.cpp cpp/scaling_test.cpp cpp/clock_test.cpp
    LIBRARIES maplethief_frontend runtime SDL3::SDL3)
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

# The frontend running the real game (#58, ADR 0042): MapleThief, linked with maplethief_gen,
# boots the game from work/ipa headless (SDL's dummy video driver), taps Play on the main menu at
# 3 s (a scripted tap) and runs for 150 window frames, into the first map. The last frame is read
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
                   ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;MAPLETHIEF_DATA_DIR=${_game_boot_out}/data")

    # The device golden screens (#60, ADR 0044): mtport.devicegolden drives MapleThief
    # --fixed-step headless through the runs of tests/golden/device_screens.json (fresh install,
    # then a second launch on its saves) and compares each screen with our recorded render
    # (work/golden/ours/, exact; MAPLETHIEF_UPDATE_GOLDEN=1 records it) and with the device
    # screenshot (work/golden/device/, normalised, masks and tolerance from the list). A missing
    # golden skips its case; the test is skipped (77) when the game cannot start or every case
    # was skipped. Shots, diffs and the runs' data directories go to the test output directory.
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter) # the top level's, found later
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
endif()

add_subdirectory(golden)
