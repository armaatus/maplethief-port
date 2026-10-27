# HLE runtime (ARCHITECTURE.md section 2): the import table at run time and the HLE
# implementations (runtime/src/hle/, ADR 0019); M4 grows it into the full runtime.

add_library(runtime STATIC src/runtime.cpp src/sjlj.c src/hle/imports.cpp src/hle/libgcc.cpp
                           src/hle/registry.cpp)
target_include_directories(
    runtime
    PUBLIC include
    PRIVATE src)
# MT_VERSION is the full version (MAPLETHIEF_VERSION, e.g. 0.0.1-rc.1, ADR 0024) as a string
# literal, defined once here; PUBLIC, so that everything linking the runtime (the platform
# layer, the tests) sees the same value.
target_compile_definitions(runtime PUBLIC MT_VERSION="${MAPLETHIEF_VERSION}")
maplethief_target_defaults(runtime)
