"""MainWindow.nib, the app's only nib (docs/formats/nibarchive.md, docs/re/program-flow.md).

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

import plistlib

import pytest

from mtport import nib

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module")
def app(ipa_dir):
    return ipa_dir / "Payload" / "MapleThief.app"


@pytest.fixture(scope="module")
def main_nib(app):
    info = plistlib.loads((app / "Info.plist").read_bytes())
    assert info["NSMainNibFile"] == "MainWindow"
    return app / "MainWindow.nib"


def test_every_shipped_nib_parses_and_reencodes(app, main_nib):
    nibs = sorted(app.rglob("*.nib"))
    assert nibs == [main_nib]  # no EAGLViewController.nib, though the delegate names one
    data = main_nib.read_bytes()
    a = nib.parse(data, main_nib.name)
    assert nib.encode(a) == data
    assert (a.format_version, a.coder_version, len(a.objects)) == (1, 9, 23)


def test_main_window_objects(main_nib):
    a = nib.load(main_nib)
    objects = [a.label(o) for o in a.array(a.deref(a.get(a.root(), "UINibObjectsKey")))]
    assert objects == ["IBFilesOwner", "IBFirstResponder", "MapleThiefAppDelegate", "UIWindow",
                       "EAGLView"]
    window = next(o for o in a.objects if a.class_name(o) == "UIWindow")
    (view,) = a.array(a.deref(a.get(window, "UISubviews")))
    assert a.label(view) == "EAGLView"
    assert a.string(a.deref(a.get(view, "UIOriginalClassName"))) == "UIView"
    for o in (window, view):
        bounds = next(v for v in o.values if v.key == "UIBounds")
        assert bounds.floats() == (0.0, 0.0, 320.0, 480.0)  # portrait; createGLes rotates
    visible = a.array(a.deref(a.get(a.root(), "UINibVisibleWindowsKey")))
    assert visible == [window]


def test_main_window_outlets(main_nib):
    a = nib.load(main_nib)
    got = sorted((a.label(s), name, a.label(d)) for name, s, d in a.outlets())
    # Only these two: the delegate's glView and m_ViewController outlets are not connected.
    assert got == [
        ("IBFilesOwner", "delegate", "MapleThiefAppDelegate"),
        ("MapleThiefAppDelegate", "window", "UIWindow"),
    ]
