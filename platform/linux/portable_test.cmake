# CTest platform.linux_portable (platform/linux/linux.cmake, ADR 0065). It fails unless the
# Linux executable PROGRAM
#   - lists only glibc's own libraries as NEEDED (no libstdc++, libgcc_s, SDL or X11), and
#   - needs no GLIBC symbol version newer than GLIBC_FLOOR, and no GLIBCXX, CXXABI or GCC
#     version (those would mean a dynamic libstdc++ or libgcc_s).
#
#   cmake -DPROGRAM=<MapleThief> -DREADELF=<readelf> -DGLIBC_FLOOR=2.39 -P portable_test.cmake

set(allowed_needed
    libc.so.6
    libm.so.6
    libpthread.so.0
    libdl.so.2
    librt.so.1
    ld-linux-x86-64.so.2
    ld-linux-aarch64.so.1)

execute_process(
    COMMAND "${READELF}" --wide --dynamic "${PROGRAM}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE dynamic
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${READELF} --dynamic ${PROGRAM} failed (${result}): ${error}")
endif()
string(REGEX MATCHALL "\\(NEEDED\\)[^\n]*\\[[^]\n]+\\]" needed_lines "${dynamic}")
set(needed "")
set(bad "")
foreach(line IN LISTS needed_lines)
    string(REGEX REPLACE ".*\\[([^]]+)\\]$" "\\1" lib "${line}")
    list(APPEND needed "${lib}")
    if(NOT lib IN_LIST allowed_needed)
        list(APPEND bad "NEEDED ${lib}")
    endif()
endforeach()
if(needed STREQUAL "")
    message(FATAL_ERROR "${PROGRAM}: no NEEDED entries found; is it a dynamic ELF executable?")
endif()

execute_process(
    COMMAND "${READELF}" --wide --version-info "${PROGRAM}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE versions
    ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${READELF} --version-info ${PROGRAM} failed (${result}): ${error}")
endif()
# The version needs (.gnu.version_r) list one "Name: <VERSION>" per needed version.
string(REGEX MATCHALL "Name: [A-Za-z_]+[0-9.]*" names "${versions}")
set(newest "0")
foreach(entry IN LISTS names)
    string(REGEX REPLACE "^Name: " "" name "${entry}")
    if(name MATCHES "^GLIBC_([0-9]+(\\.[0-9]+)*)$")
        set(v "${CMAKE_MATCH_1}")
        if(v VERSION_GREATER newest)
            set(newest "${v}")
        endif()
        if(v VERSION_GREATER GLIBC_FLOOR)
            list(APPEND bad "${name} (newer than GLIBC_${GLIBC_FLOOR})")
        endif()
    elseif(name MATCHES "^(GLIBCXX|CXXABI|GCC)_")
        list(APPEND bad "${name} (a dynamic libstdc++ or libgcc_s)")
    endif()
endforeach()
list(REMOVE_DUPLICATES bad)

list(JOIN needed ", " needed_text)
if(bad)
    list(JOIN bad "\n  " bad_text)
    message(FATAL_ERROR "${PROGRAM} is not portable (ADR 0065):\n  ${bad_text}\n"
                        "NEEDED: ${needed_text}")
endif()
message(STATUS "${PROGRAM}: NEEDED ${needed_text}; newest GLIBC_${newest} "
               "(floor GLIBC_${GLIBC_FLOOR})")
