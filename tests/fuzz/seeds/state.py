"""Seeds for fuzz_state: an edit of the synthetic guest's own state file (state_fuzz.cpp).

The target builds a synthetic guest and saves its state once (the baseline); an input names
what it edits (the whole file, one section, or one host state serializer's payload) and how
(state_fuzz.cpp's header). A seed per name with no edit is the baseline's own bytes, so every
section's reader and every serializer's load starts from valid input. The names are the
stable ones written into state files (ADR 0105): renaming one breaks every saved state.
"""

from __future__ import annotations

import struct

MAX_LEN = 16384

# snapshot.cpp's sections, then each module's (runtime/src/snapshot/registry.cpp).
SECTIONS = (
    "cpu sjlj image heap heap.book stacks objc objc.states process "
    "cf cf.bundle cf.runloop ns ns.files libc.stdio libc.ntoa cg.uikit gfx gles uikit "
    "uikit.touch stubs gamekit storekit cashshop audio input"
).split()

# Every host state serializer (StateOps' names, registry.cpp).
SERIALIZERS = (
    "CFString",
    "CFNumber",
    "CFData",
    "CFURL",
    "CFBundle",
    "CFRunLoopTimer",
    "CFRunLoop",
    "NSArray",
    "NSSet",
    "NSDictionary",
    "NSURL",
    "NSError",
    "CGColorSpace",
    "CGColor",
    "CGDataProvider",
    "CGImage",
    "CGGradient",
    "CGPath",
    "UIImage",
    "UIColor",
    "UIBezierPath",
    "UIFont",
    "UIView",
    "CALayer",
    "UIViewController",
    "EAGLContext",
    "AVAudioSession",
    "AVAudioPlayer",
    "GKScore",
    "GKAchievement",
    "GKLeaderboard",
    "GKLeaderboardViewController",
    "SKProductsRequest",
    "SKProductsResponse",
    "SKProduct",
    "SKPayment",
    "SKPaymentTransaction",
    "CFSocket (offline)",
    "CFRunLoopSource of a CFSocket (offline)",
)


def edit(name: str, mode: int = 1, offset: int = 0, data: bytes = b"") -> bytes:
    return name.encode("ascii") + b"\0" + bytes([mode]) + struct.pack("<I", offset) + data


def file_name(name: str) -> str:
    return "".join(c if c.isalnum() or c in "-." else "_" for c in name)


def seeds() -> dict[str, bytes]:
    out = {
        "file": b"file\0",
        "file-header-flipped": edit("file", 1, 8, b"\x02"),
        "file-cut": edit("file", 4, 200),
        "raw-magic": b"MTSTATE\x1a" + b"\x01" * 120,
    }
    for s in SECTIONS:
        out[f"section-{file_name(s)}"] = s.encode("ascii") + b"\0"
    out["section-heap.book-top"] = edit("heap.book", 1, 0, b"\xff\xff\xff\xff")
    out["section-objc.states-count"] = edit("objc.states", 1, 0, b"\xff\xff\x00\x00")
    out["section-stacks-cut"] = edit("stacks", 4, 6)
    for s in SERIALIZERS:
        out[f"state-{file_name(s)}"] = f"state:{s}".encode("ascii") + b"\0"
    return out
