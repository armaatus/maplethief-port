# SDL3 frontend (ARCHITECTURE.md section 3). Shared C++ lives in src/; per-OS files go in
# macos/, windows/, ios/ and linux/ (Objective-C++ only under macos/ and ios/, ADR 0002).

set(MAPLETHIEF_APP_ID "io.github.armaatus.maplethief")

add_executable(MapleThief src/main.cpp)
target_link_libraries(MapleThief PRIVATE runtime SDL3::SDL3)
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
