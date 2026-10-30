# Linux-only settings of the MapleThief executable (#84, ADR 0065), included by
# platform/CMakeLists.txt on Linux.
#
# A portable binary. A build from the Linux container (Ubuntu 26.04, GCC 15) must also run on
# distributions with an older C++ runtime, SteamOS among them. So libstdc++ and libgcc are linked
# statically. SDL3 is static already (cmake/Dependencies.cmake) and loads X11, Wayland, ALSA,
# PulseAudio, PipeWire, libdecor and udev at run time with dlopen, so the executable needs
# nothing but glibc. platform.linux_portable checks that it stays that way: glibc's own
# libraries as its only NEEDED entries, and no GLIBC symbol version newer than
# MAPLETHIEF_LINUX_GLIBC_FLOOR.

set(MAPLETHIEF_LINUX_GLIBC_FLOOR
    "2.39"
    CACHE STRING "Newest GLIBC symbol version MapleThief may need on Linux (ADR 0065)")

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
