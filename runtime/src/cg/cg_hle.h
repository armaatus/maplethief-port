// The CoreGraphics and UIKit drawing HLE functions and data-import hooks (#51, ADR 0036),
// listed by import name in runtime/src/hle/registry.cpp. Every CoreGraphics import of the game
// and every UIKit C function except UIApplicationMain (process start-up, #43) is here.
#ifndef MAPLETHIEF_RUNTIME_CG_CG_HLE_H
#define MAPLETHIEF_RUNTIME_CG_CG_HLE_H

#include <cstdint>

#include "maplethief/recomp/recomp.h"

extern "C" {

// objects.cpp: CoreGraphics
void mt_hle_CGAffineTransformMakeRotation(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGBitmapContextCreate(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGColorGetComponents(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGColorSpaceCreateDeviceRGB(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGColorSpaceRelease(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextAddPath(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextClip(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextDrawImage(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextDrawLinearGradient(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextDrawRadialGradient(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextRelease(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextRestoreGState(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextSaveGState(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextScaleCTM(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextSetBlendMode(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextSetGrayFillColor(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextSetTextDrawingMode(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGContextTranslateCTM(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGDataProviderCopyData(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGDataProviderCreateWithCFData(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGDataProviderRelease(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGGradientCreateWithColors(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGGradientRelease(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGImageCreate(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGImageGetAlphaInfo(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGImageGetBitmapInfo(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGImageGetBitsPerComponent(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGImageGetBitsPerPixel(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGImageGetBytesPerRow(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGImageGetColorSpace(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGImageGetDataProvider(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGImageGetHeight(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGImageGetRenderingIntent(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGImageGetWidth(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGImageRelease(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGRectContainsPoint(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_CGRectInset(mt_cpu *cpu, std::uint32_t id) noexcept;
// The data imports: CGAffineTransform (24 bytes) and CGRect (16 bytes) constants.
void mt_hle_init_CGAffineTransformIdentity(mt_cpu *cpu, std::uint32_t id,
                                           std::uint32_t addr) noexcept;
void mt_hle_init_CGRectNull(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;
void mt_hle_init_CGRectZero(mt_cpu *cpu, std::uint32_t id, std::uint32_t addr) noexcept;

// uikit.cpp: UIKit's C drawing functions
void mt_hle_UIGraphicsBeginImageContext(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_UIGraphicsBeginImageContextWithOptions(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_UIGraphicsEndImageContext(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_UIGraphicsGetCurrentContext(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_UIGraphicsGetImageFromCurrentImageContext(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_UIGraphicsPopContext(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_UIGraphicsPushContext(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_UIImagePNGRepresentation(mt_cpu *cpu, std::uint32_t id) noexcept;
void mt_hle_UIRectFill(mt_cpu *cpu, std::uint32_t id) noexcept;

} // extern "C"

#endif // MAPLETHIEF_RUNTIME_CG_CG_HLE_H
