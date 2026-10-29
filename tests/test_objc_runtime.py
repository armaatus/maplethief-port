"""The Objective-C runtime (ADR 0026) on a synthetic Mach-O image (``objcfixtures``): the
start-up path from a real image layout (sections, binds) to method lookup, compiled with the
host C compiler as the recompiled program will be. tests/cpp/objc_test.cpp covers sending;
this covers loading from the Mach-O's own metadata. Runs in CI."""

from __future__ import annotations

import objcfixtures as fx
from mtport import macho
from objc_harness import HostClass, guest_memory, run

NSOBJECT = HostClass(
    "NSObject",
    None,
    4,
    instance_methods=("release", "retain", "init"),
    class_methods=("alloc",),
)
HOSTS = [NSOBJECT, HostClass("NSString", "NSObject", 0), HostClass("NSArray", "NSObject", 0)]


def test_lookup_on_a_synthetic_image(tmp_path):
    image = macho.MachO(fx.IMAGE)
    a = fx.ADDRS
    objects = guest_memory(image).host_class_objects()
    nsobject, nsobject_meta = objects["NSObject"]
    nsstring, nsstring_meta = objects["NSString"]
    # The image binds NSString's class object only: the runtime places its metaclass.
    assert nsstring and nsstring_meta == 0
    lookups = [
        (a["widget"], "init"),
        (a["widget"], "size"),
        (a["widget"], "setSize:"),
        (a["widget"], "release"),
        (a["widget_meta"], "widgetWithSize:"),
        (a["widget_meta"], "alloc"),
        (a["widget_meta"], "release"),  # the root metaclass inherits NSObject's methods
        (a["cbthing"], "run"),
        (a["cbthing"], "size"),  # inherited from Widget
        (a["cbthing"], "init"),
        (a["cbthing_meta"], "widgetWithSize:"),
        (a["cbthing_meta"], "shared"),  # the override
        (a["widget"], "run"),  # a subclass's method: a miss
        (a["widget"], "widgetWithSize:"),  # a class method: a miss on the class
        (nsstring, "widgetSize"),  # the category NSString (Extras)
    ]
    named = [("NSString", "+", "stringWithWidget:"), ("NSArray", "-", "widgetSize")]
    _, result = run(
        tmp_path, image, HOSTS, lookups, overrides=[("CBThing", True, "shared")], named=named
    )
    assert result.logs == []
    assert result.load["problems"] == 0
    assert result.load["image_classes"] == 2
    assert result.load["classes"] == 5  # NSObject, NSString, NSArray, Widget, CBThing
    assert result.load["categories"] == 1
    assert result.load["selrefs_changed"] == 0  # the fixture's strings are deduplicated
    assert result.load["instance_start_problems"] == 0
    lk = result.lookups
    assert lk[(a["widget"], "init")] == ("image", fx.IMP_INIT, a["widget"])
    assert lk[(a["widget"], "size")] == ("image", fx.IMP_SHARED, a["widget"])
    assert lk[(a["widget"], "setSize:")] == ("image", fx.IMP_SET_SIZE, a["widget"])
    assert lk[(a["widget"], "release")] == ("host", 0, nsobject)
    assert lk[(a["widget_meta"], "widgetWithSize:")] == ("image", fx.IMP_SIZE, a["widget_meta"])
    assert lk[(a["widget_meta"], "alloc")] == ("host", 0, nsobject_meta)
    assert lk[(a["widget_meta"], "release")] == ("host", 0, nsobject)
    assert lk[(a["cbthing"], "run")] == ("image", fx.IMP_CBTHING, a["cbthing"])
    assert lk[(a["cbthing"], "size")] == ("image", fx.IMP_SHARED, a["widget"])
    assert lk[(a["cbthing"], "init")] == ("image", fx.IMP_INIT, a["widget"])
    assert lk[(a["cbthing_meta"], "widgetWithSize:")] == (
        "image",
        fx.IMP_SIZE,
        a["widget_meta"],
    )
    assert lk[(a["cbthing_meta"], "shared")] == ("override", 0, a["cbthing_meta"])
    assert lk[(a["widget"], "run")] is None
    assert lk[(a["widget"], "widgetWithSize:")] is None
    assert lk[(nsstring, "widgetSize")] == ("category", fx.IMP_SHARED, nsstring)
    origin, imp, owner = result.named[("NSString", "+", "stringWithWidget:")]
    assert (origin, imp) == ("category", fx.IMP_CAT_CLASS)
    assert owner >= guest_memory(image).heap  # the metaclass object the runtime placed
    assert result.named[("NSArray", "-", "widgetSize")] is None


def test_a_missing_host_superclass_is_a_problem(tmp_path):
    # Without NSObject, Widget's superclass word points at an object no class owns.
    image = macho.MachO(fx.IMAGE)
    a = fx.ADDRS
    _, result = run(tmp_path, image, [HostClass("NSString", None, 0)], [(a["widget"], "init")])
    assert result.load["image_classes"] == 0
    assert result.lookups[(a["widget"], "init")] is None
    assert any("of Widget is neither an image class nor a host class" in s for s in result.logs)
    assert any("of CBThing could not be realized" in s for s in result.logs)
