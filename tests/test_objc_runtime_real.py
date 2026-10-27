"""The Objective-C runtime (ADR 0026) on the game's own metadata: the armv7 slice mapped into
guest memory with its binds applied, every imported class a host class, and a lookup of
every method of every class, inherited ones included, checked against ``mtport.objc``'s
independent parse. This is the #46 prototype's measurement: ``objc_msgSend`` lookup over guest
metadata works for all 75 classes and 1,064 IMPs with no per-class code.

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

from __future__ import annotations

import pytest

from mtport import bootstrap as bs
from mtport import macho, objc
from objc_harness import HostClass, guest_memory, run
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / bs.slice_path("armv7")

# NSObject's host methods for this test (the real ones come with #47 and #49).
NSOBJECT_INSTANCE = ("release", "retain", "autorelease", "dealloc", "init", "class")
NSOBJECT_CLASS = ("alloc", "allocWithZone:", "new")
# Guest instance sizes of the SDK the game was built against, as its subclasses' ivars show:
# MovieOverlayView (no ivars) has instanceStart = instanceSize = 46 = UIView's size; every
# UIView subclass with ivars starts at 48 (pointer alignment); UIViewController subclasses at
# 152; NSObject subclasses at 4 (isa).
SDK_SIZES = {"NSObject": 4, "UIView": 46, "UIViewController": 152}
# The image-class override of runtime/src/objc/classes.cpp. +[GameCenterManager
# IsGameCenterAvailable] keeps its own IMP: GameKit is emulated offline (#52, ADR 0029).
OVERRIDES = [("Chartboost", True, "sharedChartboost")]


def _chain(classes: dict[int, objc.ObjCClass], cls: objc.ObjCClass) -> list[objc.ObjCClass]:
    out = []
    while cls is not None:
        out.append(cls)
        sup = cls.superclass
        cls = classes.get(sup.address) if sup is not None and sup.address is not None else None
    return out


def test_every_method_of_every_class_resolves_from_guest_metadata(tmp_path):
    image = macho.MachO(SLICE.read_bytes())
    meta = objc.parse(image)
    objects = guest_memory(image).host_class_objects()
    nsobject, nsobject_meta = objects["NSObject"]
    hosts = [HostClass("NSObject", None, 4, NSOBJECT_INSTANCE, NSOBJECT_CLASS)]
    hosts += [
        HostClass(name, "NSObject", SDK_SIZES.get(name, 0))
        for name in sorted(objects)
        if name != "NSObject"
    ]
    classes = {c.address: c for c in meta.classes}
    overridden = {(name, sel) for name, _, sel in OVERRIDES}

    # The oracle: the first class up the image chain that lists the selector, then NSObject.
    expected: dict[tuple[int, str], tuple[str, int, int] | None] = {}
    for cls in meta.classes:
        chain = _chain(classes, cls)
        for kind in ("instance", "class"):
            obj = cls.address if kind == "instance" else cls.metaclass
            lists = [
                (c, c.instance_methods if kind == "instance" else c.class_methods) for c in chain
            ]
            for c, methods in lists:
                sels = [m.selector for m in methods]
                assert len(sels) == len(set(sels)), f"{c.name} lists a selector twice"
            selectors = {m.selector for _, ms in lists for m in ms}
            selectors |= set(NSOBJECT_INSTANCE)
            if kind == "class":
                selectors |= set(NSOBJECT_CLASS)
            for sel in sorted(selectors):
                found = None
                for c, methods in lists:
                    hit = next((m for m in methods if m.selector == sel), None)
                    if hit is not None:
                        owner = c.address if kind == "instance" else c.metaclass
                        found = ("image", hit.imp, owner)
                        break
                if kind == "class" and (cls.name, sel) in overridden:
                    found = ("override", 0, cls.metaclass)
                if found is None and kind == "class" and sel in NSOBJECT_CLASS:
                    found = ("host", 0, nsobject_meta)
                if found is None and sel in NSOBJECT_INSTANCE:
                    found = ("host", 0, nsobject)  # a root metaclass inherits from its class
                expected[(obj, sel)] = found
    named = []
    expected_named = {}
    for cat in meta.categories:
        for kind, methods in (("-", cat.instance_methods), ("+", cat.class_methods)):
            for m in methods:
                named.append((cat.cls.name, kind, m.selector))
                expected_named[(cat.cls.name, kind, m.selector)] = ("category", m.imp)
    eagl = next(c for c in meta.classes if c.name == "EAGLView")

    _, result = run(
        tmp_path,
        image,
        hosts,
        list(expected),
        overrides=OVERRIDES,
        named=named,
        bench=(eagl.address, "release", 2_000_000),
    )
    print(
        f"objc runtime on the armv7 slice: {result.load}, {len(expected)} class lookups, "
        f"{len(named)} category lookups, {result.bench_ns} ns per cached lookup"
    )
    assert result.logs == []
    assert result.load["problems"] == 0
    assert result.load["image_classes"] == 75
    assert result.load["classes"] == 75 + len(objects)
    assert result.load["categories"] == 4
    assert result.load["instance_start_problems"] == 0
    # The linker coalesced __objc_methname: every selref already is the canonical SEL.
    assert result.load["selrefs_changed"] == 0
    mismatches = [
        (k, v, result.lookups.get(k)) for k, v in expected.items() if result.lookups.get(k) != v
    ]
    assert not mismatches, mismatches[:20]
    for key, (origin, imp) in expected_named.items():
        got = result.named[key]
        assert got is not None and got[:2] == (origin, imp), (key, got)

    # Every IMP of the image is reachable by a lookup, except the one the override replaces.
    imps = {v[1] for v in result.lookups.values() if v is not None and v[0] == "image"}
    imps |= {v[1] for v in result.named.values() if v is not None}
    replaced = {
        m.imp for c in meta.classes for m in c.class_methods if (c.name, m.selector) in overridden
    }
    assert len(replaced) == 1
    assert len(imps) == 1063
    assert imps | replaced == set(objc.imp_names(meta))
