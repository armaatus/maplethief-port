# Golden-image helper (ADR 0005): load and write RGBA PNGs, compare an image against a
# golden PNG with a per-channel tolerance and a maximum ratio of differing pixels, and write
# a diff PNG into the test output directory on failure. Any C++ test that renders frames
# links maplethief_golden.
#
# Committed goldens under data/ are synthetic test patterns only. Goldens of real game
# frames are derived from the IPA, so they stay local in work/golden/ (never committed).

maplethief_use_stb()

add_library(maplethief_golden STATIC golden.cpp)
target_include_directories(maplethief_golden PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(maplethief_golden PRIVATE maplethief::stb)
target_compile_definitions(maplethief_golden
                           PRIVATE MT_TEST_OUTPUT_DIR="${MAPLETHIEF_TEST_OUTPUT_DIR}")
maplethief_target_defaults(maplethief_golden)

maplethief_add_cpp_test(
    golden_test
    AREA golden
    SOURCES golden_test.cpp
    LIBRARIES maplethief_golden)
target_compile_definitions(golden_test PRIVATE MT_GOLDEN_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
