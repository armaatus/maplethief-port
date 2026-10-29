"""The host-surface generator (tools/mtport/hostsurface.py) on synthetic analysis documents."""

from __future__ import annotations

import pytest

from mtport import hostsurface as hs


def send(
    fn,
    selector,
    *,
    cls="game",
    implemented_by=None,
    in_image=False,
    class_method=False,
    how=None,
    receiver=None,
):
    return {
        "function": fn,
        "selector": selector,
        "class": cls,
        "implemented_by": implemented_by,
        "in_image": in_image,
        "class_method": class_method,
        "how": how,
        "receiver": receiver if receiver is not None else implemented_by,
    }


def imports_doc(sends):
    return {
        "imports": [
            {"name": "_OBJC_CLASS_$_NSString", "framework": "Foundation"},
            {"name": "_OBJC_CLASS_$_NSArray", "framework": "Foundation"},
            {"name": "_OBJC_CLASS_$_UIView", "framework": "UIKit"},
            {"name": "_objc_msgSend", "framework": "libobjc"},
        ],
        "functions": [
            {"function": "0x1001", "name": "-[EAGLView layoutStuff]"},
            {"function": "0x2001", "name": "-[EAGLView startAppEnv]"},
            {"function": "0x3001", "name": "-[MoviePlayerController play]"},
            {"function": "0x4001", "name": "CGame::Run"},
        ],
        "sends": sends,
    }


OBJC_DOC = {
    "classes": [
        {
            "name": "EAGLView",
            "superclass": {"name": "UIView"},
            "group": "app",
            "instance_methods": [{"selector": "layoutStuff"}, {"selector": "isOpaque"}],
            "class_methods": [],
        },
        {
            "name": "CBView",
            "group": "chartboost",
            "instance_methods": [{"selector": "count"}],
            "class_methods": [],
        },
    ],
    "categories": [
        {"name": "NSString(Extra)", "instance_methods": [{"selector": "trim"}], "class_methods": []}
    ],
}


@pytest.fixture
def receivers(monkeypatch):
    table = {"count": ("NSArray",), "length": ("NSString",), "alloc": ("+NSArray",)}
    monkeypatch.setattr(hs, "RECEIVERS", table)
    return table


def test_typed_and_untyped_rows(receivers):
    rows = hs.surface(
        imports_doc(
            [
                send(
                    "0x1001", "stringWithUTF8String:", implemented_by="NSString", class_method=True
                ),
                send(
                    "0x4001", "stringWithUTF8String:", implemented_by="NSString", class_method=True
                ),
                send("0x1001", "setNeedsDisplay", implemented_by="UIView"),
                send("0x1001", "count"),  # untyped: Chartboost's CBView implements it, no app class
                send("0x4001", "length"),
                send("0x4001", "length", implemented_by="NSString"),  # both kinds
                send("0x4001", "alloc"),
            ]
        ),
        OBJC_DOC,
    )
    assert [(r.area, r.cls, r.selector, r.how, r.sends) for r in rows] == [
        ("Foundation", "NSArray", "+alloc", "untyped", 1),
        ("Foundation", "NSArray", "-count", "untyped", 1),
        ("Foundation", "NSString", "+stringWithUTF8String:", "typed", 2),
        ("Foundation", "NSString", "-length", "typed, untyped", 2),
        ("UIKit", "UIView", "-setNeedsDisplay", "typed", 1),
    ]


def test_left_out_sends(receivers):
    rows = hs.surface(
        imports_doc(
            [
                send("0x1001", "count"),
                send("0x1001", "length"),
                send("0x1001", "alloc"),
                send("0x2001", "setNeedsDisplay", implemented_by="UIView"),  # dead function
                send("0x3001", "setNeedsDisplay", implemented_by="UIView"),  # dead class
                send("0x1001", "setFrame:", cls="chartboost", implemented_by="UIView"),
                send("0x1001", "isOpaque", in_image=True),  # an app class implements it
                send("0x1001", "trim", in_image=True),  # a category does
                send("0x1001", "layoutStuff", implemented_by="EAGLView", in_image=True),
                send("0x1001", None),  # selector unknown
            ]
        ),
        OBJC_DOC,
    )
    assert {r.selector for r in rows} == {"-count", "-length", "+alloc"}


def test_receivers_must_be_exact(receivers):
    with pytest.raises(ValueError, match="without a receiver.*: frame"):
        hs.surface(
            imports_doc([send("0x1001", s) for s in ("count", "length", "alloc", "frame")]),
            OBJC_DOC,
        )
    with pytest.raises(ValueError, match="no live game code sends untyped: alloc"):
        hs.surface(imports_doc([send("0x1001", "count"), send("0x1001", "length")]), OBJC_DOC)


def test_unknown_class_and_area_overrides(receivers, monkeypatch):
    with pytest.raises(ValueError, match="no framework known for class NSFoo"):
        hs.surface(
            imports_doc(
                [send("0x1001", s) for s in ("count", "length", "alloc")]
                + [send("0x1001", "bar", implemented_by="NSFoo")]
            ),
            OBJC_DOC,
        )
    monkeypatch.setattr(hs, "AREA_OVERRIDES", {("NSString", "sizeWithFont:"): "UIKit"})
    rows = hs.surface(
        imports_doc(
            [send("0x1001", s) for s in ("count", "length", "alloc")]
            + [send("0x1001", "sizeWithFont:", implemented_by="NSString")]
        ),
        OBJC_DOC,
    )
    assert ("UIKit", "NSString", "-sizeWithFont:") in {(r.area, r.cls, r.selector) for r in rows}


def test_markdown_tables():
    rows = [
        hs.Row("Foundation", "NSArray", "-count", "untyped", 3),
        hs.Row("Foundation", "NSString", "+string", "typed", 1),
        hs.Row("UIKit", "UIView", "-frame", "typed, untyped", 2),
    ]
    md = hs.to_markdown(rows)
    assert md.startswith("# Host surface\n")
    assert "| Foundation | 2 | 2 | #49 |" in md
    assert "| UIKit | 1 | 1 | #54 (drawing: #51) |" in md
    # The row format tests/cpp/host_surface_test.cpp parses: | `Class` | `±selector` | ...
    foundation = md.split("## Foundation\n")[1].split("## UIKit")[0]
    assert "| `NSArray` | `-count` | 3 | untyped |" in foundation
    assert "| `NSString` | `+string` | 1 | typed |" in foundation
    assert md.endswith("| `UIView` | `-frame` | 2 | typed, untyped |\n")


def test_dead_functions():
    assert hs.is_dead("-[EAGLView startAppEnv]")
    assert hs.is_dead("-[InAppPurchaseManager makePurchase:]")
    assert hs.is_dead("-[MoviePlayerController initWithMovieFile:]")
    assert not hs.is_dead("-[EAGLView layoutSubviews]")
    assert not hs.is_dead("")


def test_cli_reports_missing_inputs(tmp_path, capsys):
    assert hs.main(["generate", "--repo", str(tmp_path)]) == 1
    assert "mtport.imports generate" in capsys.readouterr().err


def test_self_send_after_super_init_reaches_the_app_method(receivers):
    # In -[EAGLView layoutStuff], self = [super init...] is typed UIView ("init"): a send of a
    # selector EAGLView implements reaches EAGLView's method (no row), any other one UIView's.
    # The result of another class's init is no self: its row stays.
    rows = hs.surface(
        imports_doc(
            [
                send("0x1001", "layoutStuff", implemented_by="UIView", how="init"),
                send("0x1001", "setNeedsDisplay", implemented_by="UIView", how="init"),
                send("0x1001", "isOpaque", implemented_by="NSString", how="init"),
                send("0x1001", "isOpaque", implemented_by="UIView", how="super"),
                send("0x1001", "count"),
                send("0x4001", "length"),
                send("0x4001", "alloc", class_method=True),
            ]
        ),
        OBJC_DOC,
    )
    got = {(r.cls, r.selector) for r in rows}
    assert ("UIView", "-layoutStuff") not in got
    assert ("UIView", "-setNeedsDisplay") in got
    assert ("UIView", "-isOpaque") in got  # [super isOpaque] reaches UIView's
    assert ("NSString", "-isOpaque") in got  # [[NSString alloc] init...] is no self


def test_app_class_selectors_follow_app_superclasses():
    doc = {
        "classes": [
            {
                "name": "Base",
                "group": "app",
                "superclass": {"name": "UIView"},
                "instance_methods": [{"selector": "a"}],
                "class_methods": [],
            },
            {
                "name": "Sub",
                "group": "app",
                "superclass": {"name": "Base"},
                "instance_methods": [{"selector": "b"}],
                "class_methods": [],
            },
        ],
        "categories": [],
    }
    assert hs.app_class_selectors(doc) == {
        "Base": ("UIView", {"a"}),
        "Sub": ("UIView", {"a", "b"}),
    }
    assert hs.sending_class("-[Sub b]") == "Sub"
    assert hs.sending_class("+[Sub b]") is None
    assert hs.sending_class("CGame::Run") is None
