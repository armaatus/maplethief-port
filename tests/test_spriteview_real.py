"""The sprite viewer over every shipped sprite project (issue #27).

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

import pytest

from mtport import spriteview as sv
from repo_paths import IPA_DIR

pytestmark = pytest.mark.real_assets

APP = IPA_DIR / "Payload" / "MapleThief.app"


def test_batch_renders_and_checks_all_138_projects_with_no_errors():
    # Every image decoded (palettes applied), every frame of every action rendered in both
    # facings, and the anchor check (sv.check_action) run on each.
    report = sv.batch(APP)
    assert report.errors == []
    assert (report.projects, report.empty, report.actions, report.frames) == (138, 17, 331, 1167)


@pytest.mark.parametrize(
    ("project", "actions", "grounded"),
    [
        ("hero/0", (3, 6, 7), True),  # walk and two attacks (inferred from the body images)
        ("mob/60", (0, 2), True),  # palette-swapped snail
        ("mob/80", (0, 2), True),
        ("mob/126", (0, 3), False),  # a boss that floats and bobs above its position
    ],
)
def test_visually_checked_actions_keep_one_anchor(project, actions, grounded):
    pid = sv.parse_id(project)
    rendered = sv.render_project(sv.load_project(APP, pid))
    assert rendered.errors == []
    for a in actions:
        r = rendered.actions[a]
        for flip in sv.FACINGS:
            assert {len(f) for f in r.frames[flip]} == {r.width * r.height * 4}
        if not grounded:
            continue
        # The feet stand on the anchor row: the lowest visible row of every frame is the
        # anchor's (the frame draw's y + 1 puts the images' bottom row there).
        for rgba in r.frames[False]:
            box = sv.visible_box(rgba[3::4], r.width, r.height)
            assert box is not None and box.y1 == r.anchor_y + 1, (project, a)
