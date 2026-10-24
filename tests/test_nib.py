"""NIBArchive reader/writer (mtport.nib) on synthetic archives."""

from __future__ import annotations

import struct

import pytest

from mtport import nib
from mtport.bytereader import FormatError


def _sample() -> nib.NibArchive:
    """A tiny MainWindow-like graph: root, a window, an app delegate, one outlet."""
    classes = [
        nib.NibClass(n)
        for n in (
            "NSObject",
            "NSArray",
            "NSString",
            "UIWindow",
            "UIClassSwapper",
            "UIRuntimeOutletConnection",
        )
    ]
    keys = [
        "UINibConnectionsKey",
        "UINibEncoderEmptyKey",
        "NS.bytes",
        "UIClassName",
        "UILabel",
        "UISource",
        "UIDestination",
        "UIBounds",
        "UIOpaque",
        "UITag",
        "UIAlpha",
        "UIScale",
        "UIBig",
        "UISmall",
        "UINothing",
    ]
    v = nib.Value
    objs = [
        nib.NibObject(0, 0, [v("UINibConnectionsKey", nib.OBJECT, nib.Ref(1))]),
        nib.NibObject(1, 1, [v("UINibEncoderEmptyKey", nib.OBJECT, nib.Ref(2))]),
        nib.NibObject(
            2,
            5,
            [
                v("UILabel", nib.OBJECT, nib.Ref(3)),
                v("UISource", nib.OBJECT, nib.Ref(4)),
                v("UIDestination", nib.OBJECT, nib.Ref(6)),
            ],
        ),
        nib.NibObject(3, 2, [v("NS.bytes", nib.DATA, b"window")]),
        nib.NibObject(4, 4, [v("UIClassName", nib.OBJECT, nib.Ref(5))]),
        nib.NibObject(5, 2, [v("NS.bytes", nib.DATA, b"AppDelegate")]),
        nib.NibObject(
            6,
            3,
            [
                v("UIBounds", nib.DATA, b"\x06" + struct.pack("<4f", 0, 0, 320, 480)),
                v("UIOpaque", nib.FALSE, False),
                v("UITag", nib.I8, -3),
                v("UIBig", nib.I64, 1 << 40),
                v("UISmall", nib.I16, -300),
                v("UIScale", nib.I32, 70000),
                v("UIAlpha", nib.F32, 0.5),
                v("UINothing", nib.NIL, None),
                v("UIOpaque", nib.TRUE, True),
                v("UIAlpha", nib.F64, 0.25),
            ],
        ),
    ]
    return nib.NibArchive(1, 9, objs, keys, classes)


def test_round_trip():
    a = _sample()
    data = nib.encode(a)
    assert data.startswith(b"NIBArchive")
    b = nib.parse(data)
    assert nib.encode(b) == data
    assert [o.values for o in b.objects] == [o.values for o in a.objects]
    assert b.keys == a.keys and b.classes == a.classes


def test_accessors_and_outlets():
    a = nib.parse(nib.encode(_sample()))
    assert a.class_name(6) == "UIWindow"
    assert a.get(6, "UITag") == -3 and a.get(6, "UIBig") == 1 << 40
    assert a.get(6, "UIOpaque") is False  # the first value of a repeated key
    bounds = next(v for v in a.objects[6].values if v.key == "UIBounds")
    assert bounds.floats() == (0.0, 0.0, 320.0, 480.0)
    assert a.label(a.objects[4]) == "AppDelegate"
    ((name, src, dst),) = a.outlets()
    assert (name, a.label(src), a.label(dst)) == ("window", "AppDelegate", "UIWindow")
    assert "    UIBounds = floats 0, 0, 320, 480" in nib.dump(a)
    with pytest.raises(KeyError):
        a.get(6, "UIMissing")


def test_varints_cross_the_byte_boundary():
    a = _sample()
    a.objects[3].values[0] = nib.Value("NS.bytes", nib.DATA, b"x" * 300)  # length 300: 2 bytes
    data = nib.encode(a)
    assert nib.parse(data).string(3) == "x" * 300
    out = bytearray()
    nib._put_varint(out, 300)
    assert bytes(out) == bytes([300 & 0x7F, 0x80 | (300 >> 7)])


def _header(data: bytes) -> list[int]:
    return list(struct.unpack_from("<10I", data, 10))


def _with_header(data: bytes, fields: list[int]) -> bytes:
    return data[:10] + struct.pack("<10I", *fields) + data[50:]


def test_rejects_bad_input():
    data = nib.encode(_sample())
    with pytest.raises(FormatError, match="bad magic"):
        nib.parse(b"NIBArchivX" + data[10:])
    with pytest.raises(FormatError):
        nib.parse(data[:-1])  # truncated class name
    with pytest.raises(FormatError, match="trailing|after"):
        nib.parse(data + b"\0")
    h = _header(data)
    h[3] += 1  # objects offset no longer right after the header
    with pytest.raises(FormatError, match="contiguous"):
        nib.parse(_with_header(data, h))
    h = _header(data)
    h[2] -= 1  # one object fewer: its values belong to nobody (and the keys move)
    with pytest.raises(FormatError):
        nib.parse(_with_header(data, h))


def test_rejects_out_of_range_indices():
    a = _sample()
    a.objects[0].values[0] = nib.Value("UINibConnectionsKey", nib.OBJECT, nib.Ref(99))
    with pytest.raises(FormatError, match="object index 99"):
        nib.parse(nib.encode(a))
    b = _sample()
    b.objects[1].cls = 42
    with pytest.raises(FormatError, match="class index 42"):
        nib.parse(nib.encode(b))
    c = _sample()
    c.classes[0] = nib.NibClass("NSObject", (17,))
    with pytest.raises(FormatError, match="fallback 17"):
        nib.parse(nib.encode(c))


def test_rejects_unknown_value_type():
    data = bytearray(nib.encode(_sample()))
    values_offset = _header(bytes(data))[7]
    assert data[values_offset + 1] == nib.OBJECT  # key varint is one byte
    data[values_offset + 1] = 11
    with pytest.raises(FormatError, match="unknown type 11"):
        nib.parse(bytes(data))
