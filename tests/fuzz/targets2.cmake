# The fuzz targets of #323 (ADR 0154, ADR 0159): NIBArchive, binary plist, JSON
# (gamecenter.json), the save files and the state file with every section's load function.
# Each is fuzz_<name> from fuzz_<name>.cpp and the shared driver fuzz2_driver.cpp (fuzz2.h),
# linked against the runtime; its seed corpus is made in code (no binary seeds in git, none
# from the IPA).
#
# With MAPLETHIEF_FUZZ (clang, libFuzzer) the targets link -fsanitize=fuzzer; the runtime gets
# its coverage from the fuzz preset's -fsanitize=fuzzer-no-link. Without it, each target is a
# replayer, and CTest fuzz.<name>.seeds replays its seeds on every build that runs the tests
# (the ASan job included). The targets' names are appended to the global property
# MAPLETHIEF_FUZZ_TARGETS for the smoke and long runs.
#
# Included from the tests' CMakeLists, so that paths below are this file's own.

set(_mt_fuzz2_dir "${CMAKE_CURRENT_LIST_DIR}")
foreach(_mt_fuzz2_name IN ITEMS nib plist json saves state)
    set(_mt_fuzz2_target fuzz_${_mt_fuzz2_name})
    add_executable(${_mt_fuzz2_target} "${_mt_fuzz2_dir}/fuzz_${_mt_fuzz2_name}.cpp"
                                       "${_mt_fuzz2_dir}/fuzz2_driver.cpp")
    target_include_directories(${_mt_fuzz2_target} PRIVATE "${_mt_fuzz2_dir}"
                                                           "${PROJECT_SOURCE_DIR}/runtime/src")
    target_link_libraries(${_mt_fuzz2_target} PRIVATE runtime)
    maplethief_target_defaults(${_mt_fuzz2_target})
    set_property(GLOBAL APPEND PROPERTY MAPLETHIEF_FUZZ_TARGETS ${_mt_fuzz2_target})
    if(MAPLETHIEF_FUZZ)
        target_compile_definitions(${_mt_fuzz2_target} PRIVATE MT_FUZZ_LIBFUZZER=1)
        target_link_options(${_mt_fuzz2_target} PRIVATE -fsanitize=fuzzer)
    elseif(NOT NINTENDO_SWITCH)
        add_test(NAME fuzz.${_mt_fuzz2_name}.seeds COMMAND ${_mt_fuzz2_target} --seeds)
        set_tests_properties(fuzz.${_mt_fuzz2_name}.seeds PROPERTIES LABELS "cpp;fuzz" TIMEOUT 120)
    endif()
endforeach()
