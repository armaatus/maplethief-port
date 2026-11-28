// MapleThiefTests.apk's JNI entry (#357): see android_test_host.h.
#include "android_test_host.h"

#include <android/log.h>
#include <jni.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

constexpr const char *k_tag = "MapleThiefTests";

std::string utf8(JNIEnv *env, jstring s) {
    if (s == nullptr) {
        return {};
    }
    const char *chars = env->GetStringUTFChars(s, nullptr);
    std::string out = chars != nullptr ? chars : "";
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

// Copies everything written to fd `from` to logcat (a line at a time) and to `results`.
void pump(int from, std::FILE *results) {
    std::string line;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(from, buf, sizeof buf);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        std::fwrite(buf, 1, static_cast<std::size_t>(n), results);
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                __android_log_write(ANDROID_LOG_INFO, k_tag, line.c_str());
                line.clear();
            } else {
                line.push_back(buf[i]);
            }
        }
    }
    if (!line.empty()) {
        __android_log_write(ANDROID_LOG_INFO, k_tag, line.c_str());
    }
    std::fflush(results);
}

std::string property(const char *name) {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}

} // namespace

extern "C" JNIEXPORT jint JNICALL Java_io_github_armaatus_maplethief_tests_TestsActivity_run(
    JNIEnv *env, jclass, jobjectArray args, jstring external_dir, jstring native_lib_dir) {
    namespace fs = std::filesystem;
    const std::string external = utf8(env, external_dir);
    const std::string libs = utf8(env, native_lib_dir);
    const fs::path tmp = fs::path(external) / "tmp";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    setenv("TMPDIR", tmp.c_str(), 1);
    const std::string child = libs + "/libcrash_child.so";
    setenv("MT_CRASH_CHILD_PATH", child.c_str(), 1);

    const std::string results_path = external + "/results.txt";
    std::FILE *results = std::fopen(results_path.c_str(), "w");
    if (results == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, k_tag, "cannot write %s: %s", results_path.c_str(),
                            std::strerror(errno));
        return 2;
    }
    // stdout and stderr into one pipe, read by the pump.
    int fds[2];
    if (pipe(fds) != 0) {
        std::fclose(results);
        return 2;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    const int saved_out = dup(1);
    const int saved_err = dup(2);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[1]);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::thread reader(pump, fds[0], results);

    std::printf("android: Android %s (API %s), %s, page size %ld\n",
                property("ro.build.version.release").c_str(),
                property("ro.build.version.sdk").c_str(), property("ro.product.model").c_str(),
                sysconf(_SC_PAGESIZE));
    std::printf("android: external files %s, TMPDIR %s\n", external.c_str(), tmp.c_str());
    std::printf("android: crash child %s (%s)\n", child.c_str(),
                access(child.c_str(), X_OK) == 0 ? "executable" : std::strerror(errno));

    std::vector<std::string> argv_text{"MapleThiefTests"};
    const jsize n = args != nullptr ? env->GetArrayLength(args) : 0;
    for (jsize i = 0; i < n; i++) {
        auto *s = static_cast<jstring>(env->GetObjectArrayElement(args, i));
        argv_text.push_back(utf8(env, s));
        env->DeleteLocalRef(s);
    }
    std::vector<char *> argv;
    for (std::string &a : argv_text) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    const int result = mt_android_doctest_main(static_cast<int>(argv_text.size()), argv.data());
    if (result == 0) {
        std::printf("android: all tests passed (exit status 0)\n");
    } else {
        std::printf("android: tests failed (exit status %d)\n", result);
    }
    std::fflush(stdout);
    std::fflush(stderr);
    dup2(saved_out, 1);
    dup2(saved_err, 2);
    close(saved_out);
    close(saved_err);
    reader.join();
    std::fclose(results);
    fs::remove_all(tmp, ec);
    return result;
}
