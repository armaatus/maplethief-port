# libFuzzer targets for the parsers of player-supplied files (#322, #323, ADR 0158; the threat
# model is ADR 0154). Built only by the linux-fuzz preset (MAPLETHIEF_FUZZ), with clang, ASan and
# UBSan. How to add a target: README.md in this directory.
#
# Each target <name> is
#   <name>_fuzz.cpp          LLVMFuzzerTestOneInput, built as fuzz_<name>
#   seeds/<name>.py          its seed corpus, generated (synthetic only, never a game file)
#   <name>.dict              optional: a libFuzzer dictionary (fuzz_run.py passes it)
# and CTest fuzz.<name> runs it for MAPLETHIEF_FUZZ_SMOKE_SECONDS from those seeds (the CI
# smoke). scripts/fuzz runs the same programs for hours.

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter) # make_corpus.py, fuzz_run.py

set(MAPLETHIEF_FUZZ_SMOKE_SECONDS
    30
    CACHE STRING "How long CTest's fuzz.<name> runs each target, in seconds (ADR 0158)")

# Every fuzz target, and the seed corpora: `cmake --build --preset linux-fuzz` builds this.
add_custom_target(maplethief_fuzz)

# maplethief_add_fuzz_target(<name> LIBRARIES <lib>... [SOURCES <src>...] [OWN_DISPATCH])
#
# Builds fuzz_<name> from <name>_fuzz.cpp (plus SOURCES and no_guest.cpp's mt_dispatch, which
# aborts; OWN_DISPATCH: the target defines its own), linked with -fsanitize=fuzzer and the
# LIBRARIES; generates its seeds from seeds/<name>.py into <build>/tests/fuzz/seeds/<name>/;
# and registers CTest fuzz.<name>, which runs it for MAPLETHIEF_FUZZ_SMOKE_SECONDS through
# fuzz_run.py (which takes the input size limit, MAX_LEN, from seeds/<name>.py).
function(maplethief_add_fuzz_target name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "OWN_DISPATCH" "" "SOURCES;LIBRARIES")
    set(target fuzz_${name})
    add_executable(${target} ${name}_fuzz.cpp ${arg_SOURCES})
    if(NOT arg_OWN_DISPATCH)
        target_sources(${target} PRIVATE no_guest.cpp)
    endif()
    target_link_libraries(${target} PRIVATE ${arg_LIBRARIES})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                                 "${PROJECT_SOURCE_DIR}/runtime/src")
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
    maplethief_target_defaults(${target})

    set(seeds "${CMAKE_CURRENT_BINARY_DIR}/seeds/${name}")
    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/seeds/${name}.stamp")
    file(GLOB seed_helpers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/seeds/_*.py")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/make_corpus.py" ${name}
                "${seeds}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/make_corpus.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/seeds/${name}.py" ${seed_helpers}
        COMMENT "Seed corpus for fuzz_${name}"
        VERBATIM)
    add_custom_target(${target}_seeds DEPENDS "${stamp}")
    add_dependencies(maplethief_fuzz ${target} ${target}_seeds)

    set(out "${MAPLETHIEF_TEST_OUTPUT_DIR}/fuzz/${name}")
    add_test(
        NAME fuzz.${name}
        COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/fuzz_run.py" smoke
                ${name} --binary "$<TARGET_FILE:${target}>" --seeds "${seeds}" --out "${out}"
                --seconds ${MAPLETHIEF_FUZZ_SMOKE_SECONDS})
    math(EXPR timeout "${MAPLETHIEF_FUZZ_SMOKE_SECONDS} * 3 + 60")
    set_tests_properties(fuzz.${name} PROPERTIES LABELS fuzz TIMEOUT ${timeout})
endfunction()

# --- #322: the files a downloaded pack or a save export reaches first ---------------------------

# The save export's zip reader and the IPA import (platform/src/save_zip.cpp, ipa_import.cpp).
maplethief_add_fuzz_target(zip LIBRARIES maplethief_frontend runtime)
maplethief_add_fuzz_target(ipa LIBRARIES maplethief_frontend runtime)
maplethief_add_fuzz_target(ipa_parts LIBRARIES maplethief_frontend runtime)
# PNG through stb_image (runtime/src/cg/codec.cpp), a texture pack's files (texpack.cpp), the
# border's image (platform/src/border.cpp).
maplethief_add_fuzz_target(png LIBRARIES runtime)
maplethief_add_fuzz_target(texpack LIBRARIES runtime)
maplethief_add_fuzz_target(border LIBRARIES maplethief_frontend runtime)
# Audio packs' WAV, MP3 and FLAC (runtime/src/audio/wav.cpp, mp3_stream.cpp).
maplethief_add_fuzz_target(wav LIBRARIES runtime)
maplethief_add_fuzz_target(mp3 LIBRARIES runtime)
maplethief_add_fuzz_target(flac LIBRARIES runtime)
# A text pack's .strings (runtime/src/textpack/strings_file.cpp).
maplethief_add_fuzz_target(strings LIBRARIES runtime)
# settings.ini and bindings.ini (platform/src/settings.cpp, bindings.cpp).
maplethief_add_fuzz_target(settings LIBRARIES maplethief_frontend runtime)
maplethief_add_fuzz_target(bindings LIBRARIES maplethief_frontend runtime)

# --- #323: the structured formats and the save-state file ---------------------------------------
