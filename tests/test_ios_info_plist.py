"""The iOS app's Info.plist template (#78, ADR 0074): the app's Documents/, where the game keeps
its save files, is shared with the Files app and the Finder (UIFileSharingEnabled and
LSSupportsOpeningDocumentsInPlace), and platform/CMakeLists.txt uses the template for iOS.
Installing the app and copying a save in the Files app is a needs-human check on a device."""

from __future__ import annotations

import plistlib
import re

from repo_paths import REPO

TEMPLATE = REPO / "platform" / "ios" / "Info.plist.in"
MAC_TEMPLATE = REPO / "platform" / "macos" / "Info.plist.in"


def load(path):
    return plistlib.loads(path.read_bytes())


def test_ios_template_shares_documents_with_the_files_app() -> None:
    info = load(TEMPLATE)
    assert info["UIFileSharingEnabled"] is True
    assert info["LSSupportsOpeningDocumentsInPlace"] is True


def test_ios_template_is_an_ios_app_bundle() -> None:
    info = load(TEMPLATE)
    assert info["LSRequiresIPhoneOS"] is True
    assert info["CFBundlePackageType"] == "APPL"
    assert info["MinimumOSVersion"] == "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    # The game is played in landscape only (ADR 0055).
    assert set(info["UISupportedInterfaceOrientations"]) == {
        "UIInterfaceOrientationLandscapeLeft",
        "UIInterfaceOrientationLandscapeRight",
    }
    # The macOS-only keys stay in the macOS template.
    assert "LSMinimumSystemVersion" not in info and "NSPrincipalClass" not in info


def test_ios_template_has_the_input_audit_keys() -> None:
    """#73's audit (ADR 0055): both landscape orientations and nothing else, full screen, no
    status bar, the 120 Hz ProMotion key, and only an arm64 device."""
    info = load(TEMPLATE)
    assert info["UIRequiresFullScreen"] is True
    assert info["UIStatusBarHidden"] is True
    assert info["CADisableMinimumFrameDurationOnPhone"] is True
    assert info["UIRequiredDeviceCapabilities"] == ["arm64"]
    assert not any("Portrait" in o for o in info["UISupportedInterfaceOrientations"])


def test_ios_template_names_the_catalog_launch_screen() -> None:
    launch = load(TEMPLATE)["UILaunchScreen"]
    catalog = REPO / "platform" / "icon" / "Assets.xcassets"
    assert (catalog / f"{launch['UIImageName']}.imageset" / "Contents.json").is_file()
    assert (catalog / f"{launch['UIColorName']}.colorset" / "Contents.json").is_file()


def test_ios_template_fills_the_same_bundle_values_as_the_macos_one() -> None:
    ios, mac = load(TEMPLATE), load(MAC_TEMPLATE)
    for key in (
        "CFBundleExecutable",
        "CFBundleIdentifier",
        "CFBundleName",
        "CFBundleShortVersionString",
        "CFBundleVersion",
    ):
        assert ios[key] == mac[key], key


def test_cmake_uses_the_ios_template_for_ios() -> None:
    cmake = (REPO / "platform" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert re.search(
        r'if\(CMAKE_SYSTEM_NAME STREQUAL "iOS"\)\s*'
        r'set\(_maplethief_info_plist "\$\{CMAKE_CURRENT_SOURCE_DIR\}/ios/Info\.plist\.in"\)',
        cmake,
    )
    assert 'MACOSX_BUNDLE_INFO_PLIST "${_maplethief_info_plist}"' in cmake
