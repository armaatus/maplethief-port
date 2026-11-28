# Android settings of the MapleThief library (ADR 0150, ADR 0163, issue #356), included by
# platform/CMakeLists.txt when the NDK's toolchain is in use (ANDROID, cmake/AndroidToolchain.cmake).
#
# MapleThief is libmain.so here, with SDL3 linked in statically; SDL's Java activity calls its
# SDL_main. The build then packages it, with Gradle, into MapleThief.apk at the top of the build
# tree (build/<preset>/MapleThief.apk), the target MapleThief_apk:
#   - the icon: the port's own (tools/mtport/appicon.py, ADR 0125) at the five launcher
#     densities, drawn into the build tree by `python3 -m mtport.appicon android-icons`;
#   - SDL's Java sources: android-project/app/src/main/java of the pinned SDL archive, read where
#     FetchContent unpacked it, never copied into git;
#   - the signing key: a debug key that keytool generates once in the build tree (ADR 0150);
#   - Gradle 8.12 and the JDK of tools/containers/android/pins.env, from the SDK root, with
#     GRADLE_USER_HOME below MAPLETHIEF_DOWNLOAD_DIR (the Android Gradle Plugin's own downloads)
#     and ANDROID_USER_HOME in the build tree, so nothing is written to the home folder.
#
# Without the generated code MapleThief.apk is the placeholder shell, as everywhere (ADR 0024).
#
# Host-side check, the only test of an Android build that CTest runs (the android-* test presets
# select it; scripts/android-run installs and starts the APK on the emulator):
#   platform.android_apk   mtport.apk reads MapleThief.apk: the package, its label, the API
#                          levels, libmain.so alone for arm64-v8a, stored uncompressed at a
#                          16 KiB boundary with 16 KiB-aligned segments, the icon, no IPA file

set_target_properties(MapleThief PROPERTIES OUTPUT_NAME main)
# SDL's Java side calls SDL_main and the JNI entry points SDL defines; the NDK's libraries.
target_link_libraries(MapleThief PRIVATE android log)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
# `-m mtport...` from tools/: not -I, which would leave the working directory off sys.path.
set(_maplethief_android_python "${Python3_EXECUTABLE}" -E -B)

file(STRINGS "${MAPLETHIEF_ANDROID_PINS}" _maplethief_android_pins REGEX "^ANDROID_[A-Z_]+=")
foreach(_maplethief_pin IN LISTS _maplethief_android_pins)
    string(REGEX MATCH "^([A-Z_]+)=(.*)$" _ "${_maplethief_pin}")
    set(_maplethief_pin_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()
set(_maplethief_android_sdk "${MAPLETHIEF_ANDROID_SDK}")
set(_maplethief_android_jdk "${_maplethief_android_sdk}/${_maplethief_pin_ANDROID_JDK_PATH}")
if(EXISTS "${_maplethief_android_jdk}/Contents/Home")
    string(APPEND _maplethief_android_jdk "/Contents/Home") # the macOS JDK's layout
endif()
set(_maplethief_android_gradle
    "${_maplethief_android_sdk}/${_maplethief_pin_ANDROID_GRADLE_PATH}/bin/gradle")
set(_maplethief_android_build_tools
    "${_maplethief_android_sdk}/${_maplethief_pin_ANDROID_BUILD_TOOLS_PATH}")
foreach(_maplethief_tool "${_maplethief_android_jdk}/bin/keytool" "${_maplethief_android_gradle}"
                         "${_maplethief_android_build_tools}/aapt2")
    if(NOT EXISTS "${_maplethief_tool}")
        message(FATAL_ERROR "${_maplethief_tool} is missing: scripts/android-sdk install "
                            "(tools/containers/android/pins.env)")
    endif()
endforeach()

include(FetchContent)
FetchContent_GetProperties(SDL3 SOURCE_DIR _maplethief_sdl3_source_dir)
set(_maplethief_android_dir "${CMAKE_CURRENT_BINARY_DIR}/android")
set(_maplethief_android_res "${_maplethief_android_dir}/res")
set(_maplethief_android_jni "${_maplethief_android_dir}/jniLibs")
set(_maplethief_android_keystore "${_maplethief_android_dir}/debug.keystore")
set(MAPLETHIEF_ANDROID_GRADLE_HOME
    "${MAPLETHIEF_DOWNLOAD_DIR}/android-gradle-home"
    CACHE PATH "GRADLE_USER_HOME of the APK's Gradle run: the Android Gradle Plugin's downloads")

set(_maplethief_android_icon_stamp "${_maplethief_android_res}/.icons")
add_custom_command(
    OUTPUT "${_maplethief_android_icon_stamp}"
    COMMAND ${_maplethief_android_python} -m mtport.appicon android-icons
            "${_maplethief_android_res}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_maplethief_android_icon_stamp}"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/tools"
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/mtport/appicon.py" "${PROJECT_SOURCE_DIR}/tools/mtport/png.py"
    COMMENT "Drawing the Android launcher icons (the port's own)"
    VERBATIM)

add_custom_command(
    OUTPUT "${_maplethief_android_keystore}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_maplethief_android_dir}"
    COMMAND
        "${_maplethief_android_jdk}/bin/keytool" -genkeypair -noprompt -keystore
        "${_maplethief_android_keystore}" -storetype PKCS12 -storepass android -keypass android
        -alias maplethief-debug -keyalg RSA -keysize 2048 -validity 10000 -dname
        "CN=MapleThief debug key"
    COMMENT "Generating the build tree's debug signing key (never a release key, ADR 0150)"
    VERBATIM)

# The NDK's toolchain compiles with -g in every build type. A Debug APK keeps the symbols for
# lldb; the others pack a stripped copy (Gradle keeps libmain.so as it gets it).
set(_maplethief_android_strip)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_maplethief_android_variant debug)
    set(_maplethief_android_task assembleDebug)
else()
    set(_maplethief_android_variant release)
    set(_maplethief_android_task assembleRelease)
    set(_maplethief_android_strip COMMAND "${CMAKE_STRIP}" --strip-unneeded
                                  "${_maplethief_android_jni}/arm64-v8a/libmain.so")
endif()
set(_maplethief_android_gradle_out "${_maplethief_android_dir}/gradle")
set(_maplethief_android_built
    "${_maplethief_android_gradle_out}/outputs/apk/${_maplethief_android_variant}/MapleThief-${_maplethief_android_variant}.apk"
)
# Cached (internal) for cmake/Packaging.cmake, which installs it into the kit.
set(MAPLETHIEF_ANDROID_APK "${CMAKE_BINARY_DIR}/MapleThief.apk" CACHE INTERNAL "MapleThief.apk")
file(GLOB _maplethief_android_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/android/*.gradle"
     "${CMAKE_CURRENT_SOURCE_DIR}/android/src/*/*/*/*/*.java")
add_custom_command(
    OUTPUT "${MAPLETHIEF_ANDROID_APK}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_maplethief_android_jni}/arm64-v8a"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:MapleThief>"
            "${_maplethief_android_jni}/arm64-v8a/libmain.so"
    ${_maplethief_android_strip}
    COMMAND
        "${CMAKE_COMMAND}" -E env "JAVA_HOME=${_maplethief_android_jdk}"
        "ANDROID_HOME=${_maplethief_android_sdk}" --unset=ANDROID_SDK_ROOT
        "ANDROID_USER_HOME=${_maplethief_android_dir}/user-home"
        "GRADLE_USER_HOME=${MAPLETHIEF_ANDROID_GRADLE_HOME}" "${_maplethief_android_gradle}"
        --no-daemon --quiet --console=plain -p "${CMAKE_CURRENT_SOURCE_DIR}/android"
        --project-cache-dir "${_maplethief_android_dir}/gradle-project-cache"
        "-Pmaplethief.buildDir=${_maplethief_android_gradle_out}"
        "-Pmaplethief.jniLibs=${_maplethief_android_jni}"
        "-Pmaplethief.res=${_maplethief_android_res}"
        "-Pmaplethief.sdlJava=${_maplethief_sdl3_source_dir}/android-project/app/src/main/java"
        "-Pmaplethief.keystore=${_maplethief_android_keystore}"
        "-Pmaplethief.version=${MAPLETHIEF_VERSION}" ${_maplethief_android_task}
    COMMAND "${CMAKE_COMMAND}" -E copy "${_maplethief_android_built}" "${MAPLETHIEF_ANDROID_APK}"
    DEPENDS MapleThief
            "${_maplethief_android_icon_stamp}"
            "${_maplethief_android_keystore}"
            "${CMAKE_CURRENT_SOURCE_DIR}/android/AndroidManifest.xml"
            "${CMAKE_CURRENT_SOURCE_DIR}/android/gradle.properties"
            "${MAPLETHIEF_ANDROID_PINS}"
            ${_maplethief_android_sources}
    COMMENT "Packaging MapleThief.apk with Gradle (${_maplethief_android_task})"
    USES_TERMINAL VERBATIM)
add_custom_target(MapleThief_apk ALL DEPENDS "${MAPLETHIEF_ANDROID_APK}")

# What tests/CMakeLists.txt needs to package MapleThiefTests.apk (#357) with the same tools and
# key: the Gradle command line up to its own properties, and the key's file.
set(MAPLETHIEF_ANDROID_GRADLE_COMMAND
    "${CMAKE_COMMAND}" -E env "JAVA_HOME=${_maplethief_android_jdk}"
    "ANDROID_HOME=${_maplethief_android_sdk}" --unset=ANDROID_SDK_ROOT
    "ANDROID_USER_HOME=${_maplethief_android_dir}/user-home"
    "GRADLE_USER_HOME=${MAPLETHIEF_ANDROID_GRADLE_HOME}" "${_maplethief_android_gradle}"
    --no-daemon --quiet --console=plain
    CACHE INTERNAL "The pinned Gradle with its environment (ADR 0163)")
set(MAPLETHIEF_ANDROID_KEYSTORE "${_maplethief_android_keystore}"
    CACHE INTERNAL "The build tree's debug key")
set(MAPLETHIEF_ANDROID_VARIANT "${_maplethief_android_variant}"
    CACHE INTERNAL "debug or release: the Gradle variant of this build type")
add_custom_target(maplethief_android_keystore DEPENDS "${_maplethief_android_keystore}")

add_test(
    NAME platform.android_apk
    COMMAND
        ${_maplethief_android_python} -m mtport.apk check "${MAPLETHIEF_ANDROID_APK}" --package
        io.github.armaatus.maplethief --label MapleThief --version "${MAPLETHIEF_VERSION}"
        --min-sdk "${_maplethief_pin_ANDROID_MIN_API}" --target-sdk
        "${_maplethief_pin_ANDROID_COMPILE_API}" --aapt2 "${_maplethief_android_build_tools}/aapt2"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/tools")
set_tests_properties(platform.android_apk PROPERTIES TIMEOUT 120)
message(STATUS "MapleThief: ${MAPLETHIEF_ANDROID_APK} (Android, arm64-v8a)")
