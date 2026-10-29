// Every HLE implementation the runtime has, by import symbol (ADR 0019). mt_imports_link
// fills the import table from this list; an import that is not here gets the default
// unimplemented-import handler. M4 modules add their functions (and data-object init hooks)
// here, grouped by library.
#include "maplethief/hle/imports.h"

#include <cstdint>

#include "cf/cf_hle.h"             // mt_hle_CF*: CoreFoundation (#48)
#include "cg/cg_hle.h"             // mt_hle_CG*, mt_hle_UI*: CoreGraphics and UIKit drawing (#51)
#include "foundation/foundation.h" // mt_hle_NS*: Foundation (#49)
#include "hle/hle.h"
#include "maplethief/gcd.h"        // mt_hle_dispatch_*: GCD (#50)
#include "maplethief/hle/cxxabi.h" // the type_info vtable init hooks (#45)
#include "maplethief/objc/objc.h"  // mt_hle_objc_*, mt_hle_Block_*: the ObjC runtime (#47)
#ifndef MT_HLE_CORE_ONLY
#include "audio/audio_internal.h" // mt_hle_al*, mt_hle_Audio*, mt_hle_ExtAudioFile*: audio (#117)
#include "gles/gles_internal.h"   // mt_hle_gl*: OpenGL ES 1.1, recorded (#117)
#endif

namespace {

// One entry per line, grouped by library. The core entries come first; the C test harness
// of the recompiler (tests/recomp_harness.py) builds this file with MT_HLE_CORE_ONLY, which
// leaves out every module that needs the rest of the runtime (guest memory, ObjC, ...).
// clang-format off
constexpr mt_hle_entry k_registry[] = {
    // libgcc_s.1.dylib: the SjLj unwinder (runtime/src/sjlj.c, ADR 0020)
    {"__Unwind_SjLj_Register", mt_sjlj_hle_register, nullptr},
    {"__Unwind_SjLj_Resume", mt_sjlj_hle_resume, nullptr},
    {"__Unwind_SjLj_Unregister", mt_sjlj_hle_unregister, nullptr},
    // libstdc++.6.dylib
    {"__ZSt9terminatev", mt_sjlj_hle_terminate, nullptr},
    // libobjc.A.dylib: the catch helpers of Chartboost's two @catch blocks
    {"_objc_begin_catch", mt_sjlj_hle_objc_begin_catch, nullptr},
    {"_objc_end_catch", mt_sjlj_hle_objc_end_catch, nullptr},
    {"_objc_exception_rethrow", mt_sjlj_hle_objc_exception_rethrow, nullptr},
    // libgcc_s.1.dylib
    {"___divdi3", mt_hle_divdi3, nullptr},
    {"___divsi3", mt_hle_divsi3, nullptr},
    {"___floatdisf", mt_hle_floatdisf, nullptr},
    {"___modsi3", mt_hle_modsi3, nullptr},
    {"___udivdi3", mt_hle_udivdi3, nullptr},
    {"___udivsi3", mt_hle_udivsi3, nullptr},
    {"___umodsi3", mt_hle_umodsi3, nullptr},
    // Process start-up and exit (runtime/src/boot/, #43, ADR 0027): libSystem, UIKit
    {"___cxa_atexit", mt_hle_cxa_atexit, nullptr},
    {"_UIApplicationMain", mt_hle_UIApplicationMain, nullptr},
    {"_exit", mt_hle_exit, nullptr},
#ifndef MT_HLE_CORE_ONLY
    // libobjc.A.dylib: the Objective-C runtime (runtime/src/objc/, ADR 0026, ADR 0029)
    {"_objc_enumerationMutation", mt_hle_objc_enumerationMutation, nullptr},
    {"_objc_getProperty", mt_hle_objc_getProperty, nullptr},
    {"_objc_msgSend", mt_hle_objc_msgSend, nullptr},
    {"_objc_msgSendSuper2", mt_hle_objc_msgSendSuper2, nullptr},
    {"_objc_msgSend_stret", mt_hle_objc_msgSend_stret, nullptr},
    {"_objc_setProperty", mt_hle_objc_setProperty, nullptr},
    {"_objc_sync_enter", mt_hle_objc_sync_enter, nullptr},
    {"_objc_sync_exit", mt_hle_objc_sync_exit, nullptr},
    // libSystem.B.dylib: the blocks runtime (runtime/src/objc/blocks.cpp)
    {"__Block_copy", mt_hle_Block_copy, nullptr},
    {"__Block_object_assign", mt_hle_Block_object_assign, nullptr},
    {"__Block_object_dispose", mt_hle_Block_object_dispose, nullptr},
    {"__Block_release", mt_hle_Block_release, nullptr},
    // libSystem.B.dylib: GCD (runtime/src/gcd/, ADR 0033)
    {"__dispatch_main_q", nullptr, mt_hle_init_dispatch_main_q},
    {"_dispatch_after", mt_hle_dispatch_after, nullptr},
    {"_dispatch_async", mt_hle_dispatch_async, nullptr},
    {"_dispatch_get_global_queue", mt_hle_dispatch_get_global_queue, nullptr},
    {"_dispatch_group_async", mt_hle_dispatch_group_async, nullptr},
    {"_dispatch_group_create", mt_hle_dispatch_group_create, nullptr},
    {"_dispatch_group_wait", mt_hle_dispatch_group_wait, nullptr},
    {"_dispatch_once", mt_hle_dispatch_once, nullptr},
    {"_dispatch_release", mt_hle_dispatch_release, nullptr},
    {"_dispatch_sync", mt_hle_dispatch_sync, nullptr},
    {"_dispatch_time", mt_hle_dispatch_time, nullptr},
    // libSystem.B.dylib: the C library (libc.cpp, libc_sys.cpp, ADR 0028)
    {"___assert_rtn", mt_hle_assert_rtn, nullptr},
    {"___snprintf_chk", mt_hle_snprintf_chk, nullptr},
    {"___stderrp", nullptr, mt_hle_init_stderrp},
    {"___stdinp", nullptr, mt_hle_init_stdinp},
    {"___stdoutp", nullptr, mt_hle_init_stdoutp},
    {"_abort", mt_hle_abort, nullptr},
    {"_atoi", mt_hle_atoi, nullptr},
    {"_atol", mt_hle_atol, nullptr},
    {"_calloc", mt_hle_calloc, nullptr},
    {"_close", mt_hle_close, nullptr},
    {"_floor", mt_hle_floor, nullptr},
    {"_free", mt_hle_free, nullptr},
    {"_gethostbyname", mt_hle_gethostbyname, nullptr},
    {"_gettimeofday", mt_hle_gettimeofday, nullptr},
    {"_if_nametoindex", mt_hle_if_nametoindex, nullptr},
    {"_inet_addr", mt_hle_inet_addr, nullptr},
    {"_inet_ntoa", mt_hle_inet_ntoa, nullptr},
    {"_malloc", mt_hle_malloc, nullptr},
    {"_memcmp", mt_hle_memcmp, nullptr},
    {"_memcpy", mt_hle_memcpy, nullptr},
    {"_memset", mt_hle_memset, nullptr},
    {"_pow", mt_hle_pow, nullptr},
    {"_printf", mt_hle_printf, nullptr},
    {"_puts", mt_hle_puts, nullptr},
    {"_realloc", mt_hle_realloc, nullptr},
    {"_setsockopt", mt_hle_setsockopt, nullptr},
    {"_strcat", mt_hle_strcat, nullptr},
    {"_strcmp", mt_hle_strcmp, nullptr},
    {"_strcpy", mt_hle_strcpy, nullptr},
    {"_strlen", mt_hle_strlen, nullptr},
    {"_sysctl", mt_hle_sysctl, nullptr},
    {"_sysctlbyname", mt_hle_sysctlbyname, nullptr},
    {"_trunc", mt_hle_trunc, nullptr},
    {"_vfprintf", mt_hle_vfprintf, nullptr},
    {"_vsprintf", mt_hle_vsprintf, nullptr},
    // libstdc++.6.dylib: the C++ ABI (cxxabi.cpp, rtti.cpp, ADR 0031)
    {"__ZTVN10__cxxabiv117__class_type_infoE", nullptr, mt_cxx_init_class_type_info_vtable},
    {"__ZTVN10__cxxabiv120__si_class_type_infoE", nullptr, mt_cxx_init_si_class_type_info_vtable},
    {"__ZTVN10__cxxabiv121__vmi_class_type_infoE", nullptr, mt_cxx_init_vmi_class_type_info_vtable},
    {"__ZdaPv", mt_hle_ZdaPv, nullptr},
    {"__ZdlPv", mt_hle_ZdlPv, nullptr},
    {"__Znam", mt_hle_Znam, nullptr},
    {"__Znwm", mt_hle_Znwm, nullptr},
    {"___cxa_pure_virtual", mt_hle_cxa_pure_virtual, nullptr},
    // CoreFoundation (runtime/src/cf/, #48, ADR 0032); ___CFConstantStringClassReference is
    // bound by the ObjC start-up as the class object of __NSCFConstantString
    {"_CFAbsoluteTimeGetCurrent", mt_hle_CFAbsoluteTimeGetCurrent, nullptr},
    {"_CFBundleCopyResourceURLForLocalization", mt_hle_CFBundleCopyResourceURLForLocalization, nullptr},
    {"_CFBundleGetMainBundle", mt_hle_CFBundleGetMainBundle, nullptr},
    {"_CFDataCreate", mt_hle_CFDataCreate, nullptr},
    {"_CFDataGetBytePtr", mt_hle_CFDataGetBytePtr, nullptr},
    {"_CFDataGetBytes", mt_hle_CFDataGetBytes, nullptr},
    {"_CFDataGetLength", mt_hle_CFDataGetLength, nullptr},
    {"_CFRelease", mt_hle_CFRelease, nullptr},
    {"_CFRunLoopAddSource", mt_hle_CFRunLoopAddSource, nullptr},
    {"_CFRunLoopAddTimer", mt_hle_CFRunLoopAddTimer, nullptr},
    {"_CFRunLoopGetCurrent", mt_hle_CFRunLoopGetCurrent, nullptr},
    {"_CFRunLoopGetMain", mt_hle_CFRunLoopGetMain, nullptr},
    {"_CFRunLoopTimerCreate", mt_hle_CFRunLoopTimerCreate, nullptr},
    {"_CFRunLoopTimerSetNextFireDate", mt_hle_CFRunLoopTimerSetNextFireDate, nullptr},
    // CoreFoundation: CFSocket, offline (runtime/src/stubs/network.cpp, #52): connecting fails
    // at once
    {"_CFSocketConnectToAddress", mt_hle_CFSocketConnectToAddress, nullptr},
    {"_CFSocketCreate", mt_hle_CFSocketCreate, nullptr},
    {"_CFSocketCreateRunLoopSource", mt_hle_CFSocketCreateRunLoopSource, nullptr},
    {"_CFSocketGetNative", mt_hle_CFSocketGetNative, nullptr},
    {"_CFSocketInvalidate", mt_hle_CFSocketInvalidate, nullptr},
    {"_CFSocketSendData", mt_hle_CFSocketSendData, nullptr},
    // CoreFoundation (#48), continued
    {"_CFStringAppendCharacters", mt_hle_CFStringAppendCharacters, nullptr},
    {"_CFStringConvertEncodingToNSStringEncoding", mt_hle_CFStringConvertEncodingToNSStringEncoding, nullptr},
    {"_CFStringCreateWithCString", mt_hle_CFStringCreateWithCString, nullptr},
    {"_CFURLCreateDataAndPropertiesFromResource", mt_hle_CFURLCreateDataAndPropertiesFromResource, nullptr},
    {"_NSLocaleCountryCode", nullptr, mt_hle_init_NSLocaleCountryCode},
    {"_NSLocaleLanguageCode", nullptr, mt_hle_init_NSLocaleLanguageCode},
    {"_NSLocalizedDescriptionKey", nullptr, mt_hle_init_NSLocalizedDescriptionKey},
    {"_kCFAllocatorDefault", nullptr, mt_hle_init_kCFAllocatorDefault},
    {"_kCFBooleanFalse", nullptr, mt_hle_init_kCFBooleanFalse},
    {"_kCFBooleanTrue", nullptr, mt_hle_init_kCFBooleanTrue},
    {"_kCFBundleIdentifierKey", nullptr, mt_hle_init_kCFBundleIdentifierKey},
    {"_kCFBundleVersionKey", nullptr, mt_hle_init_kCFBundleVersionKey},
    {"_kCFNumberNaN", nullptr, mt_hle_init_kCFNumberNaN},
    {"_kCFNumberNegativeInfinity", nullptr, mt_hle_init_kCFNumberNegativeInfinity},
    {"_kCFNumberPositiveInfinity", nullptr, mt_hle_init_kCFNumberPositiveInfinity},
    {"_kCFRunLoopCommonModes", nullptr, mt_hle_init_kCFRunLoopCommonModes},
    {"_kCFRunLoopDefaultMode", nullptr, mt_hle_init_kCFRunLoopDefaultMode},
    // Foundation's functions (runtime/src/foundation/, #49, ADR 0035); NSLog and
    // NSSelectorFromString are Chartboost-only
    {"_NSClassFromString", mt_hle_NSClassFromString, nullptr},
    {"_NSHomeDirectory", mt_hle_NSHomeDirectory, nullptr},
    {"_NSSearchPathForDirectoriesInDomains", mt_hle_NSSearchPathForDirectoriesInDomains, nullptr},
    // CoreGraphics (runtime/src/cg/, #51, ADR 0036)
    {"_CGAffineTransformIdentity", nullptr, mt_hle_init_CGAffineTransformIdentity},
    {"_CGAffineTransformMakeRotation", mt_hle_CGAffineTransformMakeRotation, nullptr},
    {"_CGBitmapContextCreate", mt_hle_CGBitmapContextCreate, nullptr},
    {"_CGColorGetComponents", mt_hle_CGColorGetComponents, nullptr},
    {"_CGColorSpaceCreateDeviceRGB", mt_hle_CGColorSpaceCreateDeviceRGB, nullptr},
    {"_CGColorSpaceRelease", mt_hle_CGColorSpaceRelease, nullptr},
    {"_CGContextAddPath", mt_hle_CGContextAddPath, nullptr},
    {"_CGContextClip", mt_hle_CGContextClip, nullptr},
    {"_CGContextDrawImage", mt_hle_CGContextDrawImage, nullptr},
    {"_CGContextDrawLinearGradient", mt_hle_CGContextDrawLinearGradient, nullptr},
    {"_CGContextDrawRadialGradient", mt_hle_CGContextDrawRadialGradient, nullptr},
    {"_CGContextRelease", mt_hle_CGContextRelease, nullptr},
    {"_CGContextRestoreGState", mt_hle_CGContextRestoreGState, nullptr},
    {"_CGContextSaveGState", mt_hle_CGContextSaveGState, nullptr},
    {"_CGContextScaleCTM", mt_hle_CGContextScaleCTM, nullptr},
    {"_CGContextSetBlendMode", mt_hle_CGContextSetBlendMode, nullptr},
    {"_CGContextSetGrayFillColor", mt_hle_CGContextSetGrayFillColor, nullptr},
    {"_CGContextSetTextDrawingMode", mt_hle_CGContextSetTextDrawingMode, nullptr},
    {"_CGContextTranslateCTM", mt_hle_CGContextTranslateCTM, nullptr},
    {"_CGDataProviderCopyData", mt_hle_CGDataProviderCopyData, nullptr},
    {"_CGDataProviderCreateWithCFData", mt_hle_CGDataProviderCreateWithCFData, nullptr},
    {"_CGDataProviderRelease", mt_hle_CGDataProviderRelease, nullptr},
    {"_CGGradientCreateWithColors", mt_hle_CGGradientCreateWithColors, nullptr},
    {"_CGGradientRelease", mt_hle_CGGradientRelease, nullptr},
    {"_CGImageCreate", mt_hle_CGImageCreate, nullptr},
    {"_CGImageGetAlphaInfo", mt_hle_CGImageGetAlphaInfo, nullptr},
    {"_CGImageGetBitmapInfo", mt_hle_CGImageGetBitmapInfo, nullptr},
    {"_CGImageGetBitsPerComponent", mt_hle_CGImageGetBitsPerComponent, nullptr},
    {"_CGImageGetBitsPerPixel", mt_hle_CGImageGetBitsPerPixel, nullptr},
    {"_CGImageGetBytesPerRow", mt_hle_CGImageGetBytesPerRow, nullptr},
    {"_CGImageGetColorSpace", mt_hle_CGImageGetColorSpace, nullptr},
    {"_CGImageGetDataProvider", mt_hle_CGImageGetDataProvider, nullptr},
    {"_CGImageGetHeight", mt_hle_CGImageGetHeight, nullptr},
    {"_CGImageGetRenderingIntent", mt_hle_CGImageGetRenderingIntent, nullptr},
    {"_CGImageGetWidth", mt_hle_CGImageGetWidth, nullptr},
    {"_CGImageRelease", mt_hle_CGImageRelease, nullptr},
    {"_CGRectContainsPoint", mt_hle_CGRectContainsPoint, nullptr},
    {"_CGRectInset", mt_hle_CGRectInset, nullptr},
    {"_CGRectNull", nullptr, mt_hle_init_CGRectNull},
    {"_CGRectZero", nullptr, mt_hle_init_CGRectZero},
    // UIKit: the drawing functions (runtime/src/cg/uikit.cpp, #51)
    {"_UIGraphicsBeginImageContext", mt_hle_UIGraphicsBeginImageContext, nullptr},
    {"_UIGraphicsBeginImageContextWithOptions", mt_hle_UIGraphicsBeginImageContextWithOptions, nullptr},
    {"_UIGraphicsEndImageContext", mt_hle_UIGraphicsEndImageContext, nullptr},
    {"_UIGraphicsGetCurrentContext", mt_hle_UIGraphicsGetCurrentContext, nullptr},
    {"_UIGraphicsGetImageFromCurrentImageContext", mt_hle_UIGraphicsGetImageFromCurrentImageContext, nullptr},
    {"_UIGraphicsPopContext", mt_hle_UIGraphicsPopContext, nullptr},
    {"_UIGraphicsPushContext", mt_hle_UIGraphicsPushContext, nullptr},
    {"_UIImagePNGRepresentation", mt_hle_UIImagePNGRepresentation, nullptr},
    {"_UIRectFill", mt_hle_UIRectFill, nullptr},
    // SystemConfiguration (Chartboost only, #52): "not reachable"
    {"_SCNetworkReachabilityCreateWithAddress", mt_hle_SCNetworkReachabilityCreateWithAddress, nullptr},
    {"_SCNetworkReachabilityCreateWithName", mt_hle_SCNetworkReachabilityCreateWithName, nullptr},
    {"_SCNetworkReachabilityGetFlags", mt_hle_SCNetworkReachabilityGetFlags, nullptr},
    {"_SCNetworkReachabilityScheduleWithRunLoop", mt_hle_SCNetworkReachabilityScheduleWithRunLoop, nullptr},
    {"_SCNetworkReachabilitySetCallback", mt_hle_SCNetworkReachabilitySetCallback, nullptr},
    {"_SCNetworkReachabilityUnscheduleFromRunLoop", mt_hle_SCNetworkReachabilityUnscheduleFromRunLoop, nullptr},
    // libSystem.B.dylib: CommonCrypto's digests (commoncrypto.cpp, #117)
    {"_CC_MD5", mt_hle_CC_MD5, nullptr},
    {"_CC_SHA1", mt_hle_CC_SHA1, nullptr},
    // OpenGLES: GL ES 1.1, recorded (runtime/src/gles/, #117, ADR 0038); real rendering is M5
    // (#56, #58)
    {"_glBindFramebufferOES", mt_hle_glBindFramebufferOES, nullptr},
    {"_glBindRenderbufferOES", mt_hle_glBindRenderbufferOES, nullptr},
    {"_glBindTexture", mt_hle_glBindTexture, nullptr},
    {"_glBlendFunc", mt_hle_glBlendFunc, nullptr},
    {"_glCheckFramebufferStatusOES", mt_hle_glCheckFramebufferStatusOES, nullptr},
    {"_glColor4x", mt_hle_glColor4x, nullptr},
    {"_glDeleteTextures", mt_hle_glDeleteTextures, nullptr},
    {"_glDisable", mt_hle_glDisable, nullptr},
    {"_glDisableClientState", mt_hle_glDisableClientState, nullptr},
    {"_glDrawArrays", mt_hle_glDrawArrays, nullptr},
    {"_glEnable", mt_hle_glEnable, nullptr},
    {"_glEnableClientState", mt_hle_glEnableClientState, nullptr},
    {"_glFramebufferRenderbufferOES", mt_hle_glFramebufferRenderbufferOES, nullptr},
    {"_glGenFramebuffersOES", mt_hle_glGenFramebuffersOES, nullptr},
    {"_glGenRenderbuffersOES", mt_hle_glGenRenderbuffersOES, nullptr},
    {"_glGenTextures", mt_hle_glGenTextures, nullptr},
    {"_glGetRenderbufferParameterivOES", mt_hle_glGetRenderbufferParameterivOES, nullptr},
    {"_glIsTexture", mt_hle_glIsTexture, nullptr},
    {"_glLoadIdentity", mt_hle_glLoadIdentity, nullptr},
    {"_glMatrixMode", mt_hle_glMatrixMode, nullptr},
    {"_glOrthox", mt_hle_glOrthox, nullptr},
    {"_glPopMatrix", mt_hle_glPopMatrix, nullptr},
    {"_glPushMatrix", mt_hle_glPushMatrix, nullptr},
    {"_glRotatex", mt_hle_glRotatex, nullptr},
    {"_glScalef", mt_hle_glScalef, nullptr},
    {"_glTexCoordPointer", mt_hle_glTexCoordPointer, nullptr},
    {"_glTexImage2D", mt_hle_glTexImage2D, nullptr},
    {"_glTexParameteri", mt_hle_glTexParameteri, nullptr},
    {"_glTranslatex", mt_hle_glTranslatex, nullptr},
    {"_glVertexPointer", mt_hle_glVertexPointer, nullptr},
    {"_glViewport", mt_hle_glViewport, nullptr},
    // OpenGLES and AVFoundation string constants (#117): CF constants, made by mt_cf_start
    // like CoreFoundation's own
    {"_AVAudioSessionCategoryAmbient", nullptr, mt_hle_init_AVAudioSessionCategoryAmbient},
    {"_kEAGLColorFormatRGBA8", nullptr, mt_hle_init_kEAGLColorFormatRGBA8},
    {"_kEAGLDrawablePropertyColorFormat", nullptr, mt_hle_init_kEAGLDrawablePropertyColorFormat},
    {"_kEAGLDrawablePropertyRetainedBacking", nullptr, mt_hle_init_kEAGLDrawablePropertyRetainedBacking},
    // OpenAL: silent (runtime/src/audio/audio.cpp, #117, ADR 0038); sound is M6 (#61)
    {"_alBufferData", mt_hle_alBufferData, nullptr},
    {"_alDeleteBuffers", mt_hle_alDeleteBuffers, nullptr},
    {"_alDeleteSources", mt_hle_alDeleteSources, nullptr},
    {"_alGenBuffers", mt_hle_alGenBuffers, nullptr},
    {"_alGenSources", mt_hle_alGenSources, nullptr},
    {"_alGetError", mt_hle_alGetError, nullptr},
    {"_alListenerf", mt_hle_alListenerf, nullptr},
    {"_alSourcePlay", mt_hle_alSourcePlay, nullptr},
    {"_alSourcei", mt_hle_alSourcei, nullptr},
    {"_alcCloseDevice", mt_hle_alcCloseDevice, nullptr},
    {"_alcCreateContext", mt_hle_alcCreateContext, nullptr},
    {"_alcDestroyContext", mt_hle_alcDestroyContext, nullptr},
    {"_alcGetContextsDevice", mt_hle_alcGetContextsDevice, nullptr},
    {"_alcGetCurrentContext", mt_hle_alcGetCurrentContext, nullptr},
    {"_alcGetProcAddress", mt_hle_alcGetProcAddress, nullptr},
    {"_alcMakeContextCurrent", mt_hle_alcMakeContextCurrent, nullptr},
    {"_alcOpenDevice", mt_hle_alcOpenDevice, nullptr},
    {"_alcProcessContext", mt_hle_alcProcessContext, nullptr},
    {"_alcSuspendContext", mt_hle_alcSuspendContext, nullptr},
    // AudioToolbox: the audio session, system sounds and ExtAudioFile, silent (#117); M6 (#62,
    // #64)
    {"_AudioServicesPlaySystemSound", mt_hle_AudioServicesPlaySystemSound, nullptr},
    {"_AudioSessionInitialize", mt_hle_AudioSessionInitialize, nullptr},
    {"_AudioSessionSetActive", mt_hle_AudioSessionSetActive, nullptr},
    {"_AudioSessionSetProperty", mt_hle_AudioSessionSetProperty, nullptr},
    {"_ExtAudioFileDispose", mt_hle_ExtAudioFileDispose, nullptr},
    {"_ExtAudioFileGetProperty", mt_hle_ExtAudioFileGetProperty, nullptr},
    {"_ExtAudioFileOpenURL", mt_hle_ExtAudioFileOpenURL, nullptr},
    {"_ExtAudioFileRead", mt_hle_ExtAudioFileRead, nullptr},
    {"_ExtAudioFileSetProperty", mt_hle_ExtAudioFileSetProperty, nullptr},
#endif // MT_HLE_CORE_ONLY
};
// clang-format on

} // namespace

extern "C" const mt_hle_entry *mt_hle_registry(std::uint32_t *count) noexcept {
    *count = static_cast<std::uint32_t>(sizeof k_registry / sizeof k_registry[0]);
    return k_registry;
}
