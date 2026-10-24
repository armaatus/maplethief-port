# HLE runtime (ARCHITECTURE.md section 2). A placeholder until M4: one source file, so
# that the platform layer links a real library from the start.

add_library(runtime STATIC src/runtime.cpp)
target_include_directories(runtime PUBLIC include)
target_compile_definitions(runtime PRIVATE MT_VERSION="${PROJECT_VERSION}")
maplethief_target_defaults(runtime)
