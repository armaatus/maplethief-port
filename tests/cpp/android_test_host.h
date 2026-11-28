// The test APK's host on Android (#357; doctest_main.cpp and the JNI entry of
// android_test_host.cpp, nothing elsewhere). MapleThiefTests.apk's activity
// (platform/android/tests/) calls the JNI entry on a thread of its own with the doctest arguments
// and the app's folders. stdout and stderr go, line by line, to logcat (tag MapleThiefTests) and
// to results.txt in the app's external files folder; the first lines say how the program runs
// (the Android release, the page size, the folders), the last one
// "android: all tests passed (exit status 0)" or "android: tests failed (exit status N)".
// TMPDIR is <external files>/tmp, so the tests' scratch files sit on the data folder's file
// system (ADR 0251); MT_CRASH_CHILD_PATH is libcrash_child.so in the native library folder.
#ifndef MAPLETHIEF_TESTS_CPP_ANDROID_TEST_HOST_H
#define MAPLETHIEF_TESTS_CPP_ANDROID_TEST_HOST_H

// The doctest run (doctest_main.cpp's main() elsewhere); its exit status.
int mt_android_doctest_main(int argc, char **argv);

#endif // MAPLETHIEF_TESTS_CPP_ANDROID_TEST_HOST_H
