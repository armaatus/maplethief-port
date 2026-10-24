"""A synthetic Objective-C 2 image built in code (nothing derived from the IPA).

Contents (all addresses are fixed by :class:`machofixtures.ImageBuilder`'s layout):

- ``Widget : NSObject <WidgetDelegate>``: NSObject is imported (bound by dyld at Widget+4,
  and the metaclass's isa/superclass at its +0/+4); two ivars, one property, three instance
  methods (the method list's entsize carries flag bits 0b11) and one class method. Its
  ``data`` pointer has the low bit set, which the parser must mask off.
- ``CBThing : Widget``: an image superclass (Chartboost group by name), no metadata lists.
- ``WidgetDelegate <NSObject>`` in ``__objc_protolist``; ``NSObject`` (the protocol) is only
  reachable through WidgetDelegate's protocol list.
- ``NSString (Extras)``: a category on an imported class, one instance method that shares
  its IMP with ``-[Widget size]`` and one class method.
- selrefs (``size``, ``initWithFrame:``), classrefs (Widget, imported NSArray), superrefs
  (Widget's metaclass, Widget), protorefs (WidgetDelegate).
"""

from __future__ import annotations

from machofixtures import ImageBuilder, bind_stream

CONST = ("__DATA", "__objc_const")
DATA = ("__DATA", "__objc_data")

# IMPs (Thumb bit set) inside __text.
IMP_INIT = 0x1801
IMP_SIZE = 0x1811
IMP_SET_SIZE = 0x1821
IMP_SHARED = 0x1831
IMP_CBTHING = 0x1841
IMP_CAT_CLASS = 0x1851


def _methods(b: ImageBuilder, methods: list[tuple[str, str, int]], flags: int = 0) -> int:
    body = []
    for sel, types, imp in methods:
        body += [b.cstring("__objc_methname", sel), b.cstring("__objc_methtype", types), imp]
    return b.words(CONST, 12 | flags, len(methods), *body)


def _props(b: ImageBuilder, props: list[tuple[str, str]]) -> int:
    body = []
    for name, attrs in props:
        body += [b.cstring("__cstring", name), b.cstring("__cstring", attrs)]
    return b.words(CONST, 8, len(props), *body)


def _ivars(b: ImageBuilder, ivars: list[tuple[int, str, str]]) -> int:
    """ivar_list_t of 4-byte, 4-aligned ivars: ``(offset variable address, name, type)``."""
    body = []
    for offset_ptr, name, type_ in ivars:
        name_addr = b.cstring("__objc_methname", name)
        body += [offset_ptr, name_addr, b.cstring("__objc_methtype", type_), 2, 4]
    return b.words(CONST, 20, len(ivars), *body)


def _ro(b: ImageBuilder, flags, start, size, name, methods=0, protocols=0, ivars=0, props=0):
    return b.words(CONST, flags, start, size, 0, name, methods, protocols, ivars, 0, props)


def _protocol(
    b: ImageBuilder,
    name: str,
    protocols: int = 0,
    instance: int = 0,
    optional_instance: int = 0,
    props: int = 0,
) -> int:
    """protocol_t { isa, name, protocols, inst, cls, opt inst, opt cls, props, size, flags }."""
    name_addr = b.cstring("__objc_classname", name)
    fields = (0, name_addr, protocols, instance, 0, optional_instance, 0, props, 40, 0)
    return b.words(("__DATA", "__data"), *fields)


def build() -> tuple[bytes, dict[str, int]]:
    """The image bytes and a map of the interesting addresses."""
    b = ImageBuilder()
    b.put(("__TEXT", "__text"), b"\x70\x47" * 0x40)  # bx lr
    a: dict[str, int] = {}

    # Protocols (protocol_t lives in __data).
    a["proto_nsobject"] = _protocol(
        b, "NSObject", instance=_methods(b, [("description", "@8@0:4", 0)])
    )
    a["proto_widget"] = _protocol(
        b,
        "WidgetDelegate",
        protocols=b.words(CONST, 1, a["proto_nsobject"]),
        optional_instance=_methods(b, [("widgetDidChange:", "v12@0:4@8", 0)]),
        props=_props(b, [("delegate", 'T@"WidgetDelegate",&,N')]),
    )
    proto_list_widget = b.words(CONST, 1, a["proto_widget"])

    # Widget's ivars, offsets in __objc_ivar.
    a["ivar_size"] = b.words(("__DATA", "__objc_ivar"), 4)
    a["ivar_name"] = b.words(("__DATA", "__objc_ivar"), 8)
    ivars = _ivars(b, [(a["ivar_size"], "_size", "i"), (a["ivar_name"], "_name", '@"NSString"')])

    widget_name = b.cstring("__objc_classname", "Widget")
    instance = _methods(
        b,
        [
            ("init", "@8@0:4", IMP_INIT),
            ("size", "i8@0:4", IMP_SHARED),
            ("setSize:", "v12@0:4i8", IMP_SET_SIZE),
        ],
        flags=3,
    )
    klass = _methods(b, [("widgetWithSize:", "@12@0:4i8", IMP_SIZE)])
    props = _props(b, [("size", "Ti,N,V_size")])
    ro_widget = _ro(b, 0, 4, 12, widget_name, instance, proto_list_widget, ivars, props)
    ro_widget_meta = _ro(b, 1, 20, 20, widget_name, klass)

    cbthing_name = b.cstring("__objc_classname", "CBThing")
    cb_methods = _methods(b, [("run", "v8@0:4", IMP_CBTHING)])
    ro_cbthing = _ro(b, 0, 12, 12, cbthing_name, cb_methods)
    ro_cbthing_meta = _ro(b, 1, 20, 20, cbthing_name)

    # class_t: { isa, superclass, cache, vtable, data }
    a["widget"] = b.words(DATA, 0, 0, 0, 0, ro_widget | 1)
    a["widget_meta"] = b.words(DATA, 0, 0, 0, 0, ro_widget_meta)
    b.patch(a["widget"], a["widget_meta"])
    a["cbthing"] = b.words(DATA, 0, a["widget"], 0, 0, ro_cbthing)
    a["cbthing_meta"] = b.words(DATA, 0, a["widget_meta"], 0, 0, ro_cbthing_meta)
    b.patch(a["cbthing"], a["cbthing_meta"])
    b.words(("__DATA", "__objc_classlist"), a["widget"], a["cbthing"])

    # Category NSString (Extras): { name, cls, instanceMethods, classMethods, protocols, props }
    cat_inst = _methods(b, [("widgetSize", "i8@0:4", IMP_SHARED)])
    cat_cls = _methods(b, [("stringWithWidget:", "@12@0:4@8", IMP_CAT_CLASS)])
    a["category"] = b.words(
        CONST, b.cstring("__objc_classname", "Extras"), 0, cat_inst, cat_cls, 0, 0
    )
    b.words(("__DATA", "__objc_catlist"), a["category"])
    b.words(("__DATA", "__objc_protolist"), a["proto_widget"])
    a["protoref"] = b.words(("__DATA", "__objc_protorefs"), a["proto_widget"])

    a["selref_size"] = b.words(("__DATA", "__objc_selrefs"), b.cstring("__objc_methname", "size"))
    a["selref_init"] = b.words(
        ("__DATA", "__objc_selrefs"), b.cstring("__objc_methname", "initWithFrame:")
    )
    a["classref_widget"] = b.words(("__DATA", "__objc_classrefs"), a["widget"])
    a["classref_nsarray"] = b.words(("__DATA", "__objc_classrefs"), 0)
    a["superref_meta"] = b.words(("__DATA", "__objc_superrefs"), a["widget_meta"])
    a["superref_widget"] = b.words(("__DATA", "__objc_superrefs"), a["widget"])

    binds = bind_stream(
        [
            (a["widget"] + 4, "_OBJC_CLASS_$_NSObject"),
            (a["widget_meta"], "_OBJC_METACLASS_$_NSObject"),
            (a["widget_meta"] + 4, "_OBJC_METACLASS_$_NSObject"),
            (a["category"] + 4, "_OBJC_CLASS_$_NSString"),
            (a["classref_nsarray"], "_OBJC_CLASS_$_NSArray"),
        ]
    )
    return b.build(bind=binds), a


IMAGE, ADDRS = build()
