# Nintendo Switch settings of the MapleThief executable (#235, ADR 0138), included by
# platform/CMakeLists.txt when devkitPro's Switch.cmake is the toolchain (NINTENDO_SWITCH).
#
# The build turns the MapleThief ELF into MapleThief.nro, the homebrew the Homebrew Menu starts,
# at the top of the build tree (build/<preset>/MapleThief.nro), with:
#   - its NACP (control.nacp): the name "MapleThief", the author "armaatus" and the version
#     MAPLETHIEF_VERSION (ADR 0024), written by devkitPro's nacptool;
#   - its icon: the port's own icon (tools/mtport/appicon.py, ADR 0125) as a 256 x 256 baseline
#     JPEG, drawn into the build tree by `python3 -m mtport.appicon switch-icon`; never the
#     IPA's. Its SHA-256 is in the release guard's allowlist (mtport.release.ICON_ALLOWLIST).
#
# Without the generated code MapleThief.nro is the hello path (switch/hello.cpp): the version on
# the screen and over nxlink. The runtime on Horizon is #236's, the game's start #238's.
#
# Host-side checks, the only tests of a Switch build that CTest runs (the switch-* test presets
# select them; the doctest programs become NROs of their own in tests/ and run on a console):
#   platform.switch_nro    mtport.nro reads MapleThief.nro: NACP name, author and version, and
#                          an icon that is a 256 x 256 baseline JPEG (docs/formats/nro.md)
#   platform.switch_guard  the release guard's checks on MapleThief.nro (mtport.release)

set(MAPLETHIEF_SWITCH_AUTHOR "armaatus")

target_sources(MapleThief PRIVATE switch/hello.cpp)
target_include_directories(MapleThief PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/switch")

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
# `-m mtport...` from tools/: not -I, which would leave the working directory off sys.path.
set(_maplethief_switch_python "${Python3_EXECUTABLE}" -E -B)

set(MAPLETHIEF_SWITCH_ICON "${CMAKE_CURRENT_BINARY_DIR}/switch/icon.jpg")
add_custom_command(
    OUTPUT "${MAPLETHIEF_SWITCH_ICON}"
    COMMAND ${_maplethief_switch_python} -m mtport.appicon switch-icon "${MAPLETHIEF_SWITCH_ICON}"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/tools"
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/mtport/appicon.py" "${PROJECT_SOURCE_DIR}/tools/mtport/jpeg.py"
    COMMENT "Drawing the Switch icon (256 x 256 JPEG, the port's own)"
    VERBATIM)

set(MAPLETHIEF_SWITCH_NACP "${CMAKE_CURRENT_BINARY_DIR}/switch/MapleThief.nacp")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/switch")
nx_generate_nacp(
    OUTPUT "${MAPLETHIEF_SWITCH_NACP}"
    NAME "MapleThief"
    AUTHOR "${MAPLETHIEF_SWITCH_AUTHOR}"
    VERSION "${MAPLETHIEF_VERSION}")

# Cached (internal) for cmake/Packaging.cmake, which installs it into the kit.
set(MAPLETHIEF_SWITCH_NRO "${CMAKE_BINARY_DIR}/MapleThief.nro" CACHE INTERNAL "MapleThief.nro")
nx_create_nro(
    MapleThief_nro
    TARGET MapleThief
    OUTPUT "${MAPLETHIEF_SWITCH_NRO}"
    ICON "${MAPLETHIEF_SWITCH_ICON}"
    NACP "${MAPLETHIEF_SWITCH_NACP}")

add_test(
    NAME platform.switch_nro
    COMMAND
        ${_maplethief_switch_python} -m mtport.nro check "${MAPLETHIEF_SWITCH_NRO}" --name
        MapleThief --author "${MAPLETHIEF_SWITCH_AUTHOR}" --version "${MAPLETHIEF_VERSION}"
        --icon-size 256
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/tools")
set_tests_properties(platform.switch_nro PROPERTIES TIMEOUT 60)
add_test(
    NAME platform.switch_guard
    COMMAND ${_maplethief_switch_python} -m mtport.release guard "${MAPLETHIEF_SWITCH_NRO}"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/tools")
set_tests_properties(platform.switch_guard PROPERTIES TIMEOUT 120)
message(STATUS "MapleThief: ${MAPLETHIEF_SWITCH_NRO} (Nintendo Switch homebrew)")
