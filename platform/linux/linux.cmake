# Linux-only settings of the MapleThief executable (#84, ADR 0065), included by
# platform/CMakeLists.txt on Linux.
#
# A portable binary. libstdc++ and libgcc are linked statically, so the executable does not
# need the build machine's C++ runtime (SteamOS has its own). SDL3 is static already
# (cmake/Dependencies.cmake) and loads X11, Wayland, ALSA, PulseAudio, PipeWire, libdecor and
# udev at run time with dlopen, so the executable needs nothing but glibc.
#
# platform.linux_portable checks that it stays that way: glibc's own libraries as its only
# NEEDED entries, no GLIBCXX/CXXABI/GCC symbol version (a dynamic libstdc++ or libgcc_s) and,
# when MAPLETHIEF_LINUX_GLIBC_FLOOR is set, no GLIBC symbol version newer than it. glibc only
# runs programs linked against it or an older one, so the floor holds only for a build on a
# distribution with that glibc or an older one: scripts/linux-game builds the game folder in
# the Ubuntu 24.04 image of tools/containers/linux-game/ with the floor 2.39. The CI image
# (Ubuntu 26.04, glibc 2.43) leaves it empty.

set(MAPLETHIEF_LINUX_GLIBC_FLOOR
    ""
    CACHE STRING
          "Newest GLIBC symbol version MapleThief may need on Linux, e.g. 2.39; empty: unchecked (ADR 0065)")

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_options(MapleThief PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(CMAKE_READELF)
    add_test(
        NAME platform.linux_portable
        COMMAND
            "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:MapleThief>" "-DREADELF=${CMAKE_READELF}"
            "-DGLIBC_FLOOR=${MAPLETHIEF_LINUX_GLIBC_FLOOR}" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/linux/portable_test.cmake")
    set_tests_properties(platform.linux_portable PROPERTIES TIMEOUT 60)
else()
    message(WARNING "No readelf found: platform.linux_portable is not registered")
endif()
