# The Android toolchain file of the android-* presets (ADR 0150, ADR 0163, issue #356): finds the
# pinned SDK and includes its NDK's own toolchain file (build/cmake/android.toolchain.cmake).
#
# The SDK root is MAPLETHIEF_ANDROID_SDK from the environment (the container sets it to
# /opt/android-sdk), else build/_android-sdk/ of this checkout, where scripts/android-sdk
# installs it on the Mac. The NDK's version comes from tools/containers/android/pins.env, the
# one file of the pins. The presets set ANDROID_ABI, ANDROID_PLATFORM and ANDROID_STL.
#
# A toolchain file runs again for every try_compile, in another build tree: it reads only the
# environment and paths relative to itself.

include_guard(GLOBAL)

if(DEFINED ENV{MAPLETHIEF_ANDROID_SDK} AND NOT "$ENV{MAPLETHIEF_ANDROID_SDK}" STREQUAL "")
    set(MAPLETHIEF_ANDROID_SDK "$ENV{MAPLETHIEF_ANDROID_SDK}")
else()
    get_filename_component(MAPLETHIEF_ANDROID_SDK "${CMAKE_CURRENT_LIST_DIR}/../build/_android-sdk"
                           ABSOLUTE)
endif()
set(MAPLETHIEF_ANDROID_PINS "${CMAKE_CURRENT_LIST_DIR}/../tools/containers/android/pins.env")
file(STRINGS "${MAPLETHIEF_ANDROID_PINS}" _maplethief_ndk_line REGEX "^ANDROID_NDK_PATH=")
string(REGEX REPLACE "^ANDROID_NDK_PATH=" "" _maplethief_ndk_path "${_maplethief_ndk_line}")
set(MAPLETHIEF_ANDROID_NDK "${MAPLETHIEF_ANDROID_SDK}/${_maplethief_ndk_path}")
if(NOT EXISTS "${MAPLETHIEF_ANDROID_NDK}/build/cmake/android.toolchain.cmake")
    message(
        FATAL_ERROR
            "No NDK at ${MAPLETHIEF_ANDROID_NDK}. On a Mac: scripts/android-sdk install (the pins of "
            "tools/containers/android/pins.env into build/_android-sdk/); elsewhere: "
            "scripts/build-android, which builds in the container.")
endif()
# Seen by the NDK's toolchain file and by platform/android/android.cmake.
set(ANDROID_NDK "${MAPLETHIEF_ANDROID_NDK}")
include("${MAPLETHIEF_ANDROID_NDK}/build/cmake/android.toolchain.cmake")
