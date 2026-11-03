"""mtport.texpack (#153, ADR 0081): the scalers and the pack builder on synthetic dumps."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from mtport import png, texpack

R = (255, 0, 0, 255)
G = (0, 255, 0, 255)
T = (0, 0, 0, 0)


def image(rows: list[list[tuple[int, int, int, int]]]) -> png.Image:
    data = bytes(c for row in rows for px in row for c in px)
    return png.Image(len(rows[0]), len(rows), data)


def pixels(img: png.Image) -> list[list[tuple[int, int, int, int]]]:
    return [[img.pixel(x, y) for x in range(img.width)] for y in range(img.height)]


def test_nearest_copies_each_pixel_k_by_k() -> None:
    out = texpack.upscale(image([[R, G], [T, R]]), 3, "nearest")
    assert (out.width, out.height) == (6, 6)
    rows = pixels(out)
    assert rows[0] == [R, R, R, G, G, G]
    assert rows[5] == [T, T, T, R, R, R]


def test_scale2x_fills_the_corners_along_a_diagonal() -> None:
    # A diagonal of red on transparent. Worked by hand from the rules (E0 = D if D == B, E1 = F
    # if B == F, E2 = D if D == H, E3 = F if H == F, when B != H and D != F): the red pixels'
    # outer corners and the transparent pixels' corners towards the diagonal meet, so the 2x
    # line is a staircase of single pixels instead of 2 x 2 blocks.
    src = image([[R, T, T], [T, R, T], [T, T, R]])
    rows = pixels(texpack.upscale(src, 2))
    assert rows[0] == [R, R, T, T, T, T]
    assert rows[1] == [R, T, R, T, T, T]
    assert rows[2] == [T, R, R, R, T, T]
    assert rows[3] == [T, T, R, R, R, T]


def test_scale2x_leaves_flat_areas_and_straight_edges_as_blocks() -> None:
    src = image([[R, R, G, G]] * 3)
    assert pixels(texpack.upscale(src, 2)) == pixels(texpack.upscale(src, 2, "nearest"))


def test_scale3x_rules_on_a_diagonal() -> None:
    src = image([[R, T, T], [T, R, T], [T, T, R]])
    rows = pixels(texpack.upscale(src, 3))
    # The red centre has equal neighbours above and below (B == H): a solid block.
    assert [row[3:6] for row in rows[3:6]] == [[R, R, R]] * 3
    # The transparent pixel above it (A = R, B = T, D = R, F = T, H = R, I = T, by hand): E3 and
    # E6 take D, the rest stay E.
    assert [row[3:6] for row in rows[0:3]] == [[T, T, T], [R, T, T], [R, T, T]]


@pytest.mark.parametrize(
    ("k", "steps"),
    [(1, []), (2, [2]), (3, [3]), (4, [2, 2]), (6, [2, 3]), (8, [2, 2, 2]), (5, [-5]), (7, [-7])],
)
def test_scale_steps(k: int, steps: list[int]) -> None:
    assert texpack.scale_steps(k, "scale") == steps
    assert texpack.scale_steps(k, "nearest") == ([] if k == 1 else [-k])


@pytest.mark.parametrize("k", [0, 9, -1])
def test_scale_out_of_range(k: int) -> None:
    with pytest.raises(texpack.TexpackError):
        texpack.scale_steps(k, "scale")


def test_upscale_refuses_more_than_4096() -> None:
    with pytest.raises(texpack.TexpackError, match="4096"):
        texpack.upscale(png.Image(1024, 1, bytes(4096)), 8)


def test_alpha_is_compared_like_colour() -> None:
    half = (255, 0, 0, 128)
    rows = pixels(texpack.upscale(image([[half, R], [R, half]]), 2))
    assert {px for row in rows for px in row} == {half, R}


def write_dump(dump: Path, files: dict[str, png.Image], extra_lines: list[str] = ()) -> None:
    dump.mkdir(parents=True, exist_ok=True)
    lines = []
    for name, img in files.items():
        path = dump.joinpath(*name.split("/"))
        path.parent.mkdir(parents=True, exist_ok=True)
        png.save(path, img)
        record = {"file": name, "width": img.width, "height": img.height}
        if name.startswith("ui/"):
            record["ui"] = name[3:]
        else:
            record.update({"hash": name[:16], "format": "0x1908", "type": "0x1401"})
        lines.append(json.dumps(record))
    (dump / "manifest.jsonl").write_text(
        "\n".join(lines + list(extra_lines)) + "\n", encoding="utf-8"
    )


def test_build_writes_every_file_k_times_under_its_name(tmp_path: Path) -> None:
    a = image([[R, G], [G, R]])
    b = image([[T, R, T, R]])
    write_dump(
        tmp_path / "dump",
        {
            "0123456789abcdef_2x2.png": a,
            "fedcba9876543210_4x1.raw.png": b,
            "ui/png/Menu.png": image([[G]]),
        },
        extra_lines=[
            "not json",
            json.dumps({"file": "../escape.png", "width": 1, "height": 1}),
            json.dumps({"file": "ui/../x.png", "width": 1, "height": 1}),
            json.dumps({"file": "0123456789abcdef_2x2.png", "width": 4, "height": 2}),  # wrong size
            json.dumps({"file": "0123456789abcdef_2x2.png", "width": 2, "height": 2}),  # again
        ],
    )
    written, problems = texpack.build(tmp_path / "dump", tmp_path / "pack", 4, jobs=1)
    assert written == 3
    assert len(problems) == 4 and all("skipped" in p for p in problems)
    out = png.load(tmp_path / "pack" / "0123456789abcdef_2x2.png")
    assert (out.width, out.height) == (8, 8)
    assert pixels(out) == pixels(texpack.upscale(a, 4))
    assert png.load(tmp_path / "pack" / "fedcba9876543210_4x1.raw.png").width == 16
    assert png.load(tmp_path / "pack" / "ui" / "png" / "Menu.png").width == 4
    assert not (tmp_path / "escape.png").exists()
    # --no-ui
    written, _ = texpack.build(tmp_path / "dump", tmp_path / "pack2", 2, jobs=1, ui=False)
    assert written == 2
    assert not (tmp_path / "pack2" / "ui").exists()


def test_build_reports_a_file_that_does_not_match_its_manifest(tmp_path: Path) -> None:
    write_dump(tmp_path / "dump", {"0123456789abcdef_2x2.png": image([[R, G], [G, R]])})
    (tmp_path / "dump" / "0123456789abcdef_2x2.png").write_bytes(b"not a png")
    written, problems = texpack.build(tmp_path / "dump", tmp_path / "pack", 2, jobs=1)
    assert written == 0
    assert len(problems) == 1 and problems[0].startswith("0123456789abcdef_2x2.png: ")


def test_build_in_parallel_matches_one_process(tmp_path: Path) -> None:
    files = {f"{i:016x}_2x2.png": image([[R, G], [T, (i, i, i, 255)]]) for i in range(6)}
    write_dump(tmp_path / "dump", files)
    texpack.build(tmp_path / "dump", tmp_path / "one", 2, jobs=1)
    texpack.build(tmp_path / "dump", tmp_path / "many", 2, jobs=2)
    for name in files:
        assert (tmp_path / "one" / name).read_bytes() == (tmp_path / "many" / name).read_bytes()


def test_build_needs_a_dump_and_another_directory(tmp_path: Path) -> None:
    with pytest.raises(texpack.TexpackError, match="manifest"):
        texpack.build(tmp_path, tmp_path / "pack", 2)
    write_dump(tmp_path / "dump", {})
    with pytest.raises(texpack.TexpackError, match="another directory"):
        texpack.build(tmp_path / "dump", tmp_path / "dump", 2)


def test_main(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    write_dump(tmp_path / "dump", {"0123456789abcdef_2x2.png": image([[R, G], [G, R]])})
    assert (
        texpack.main(
            ["build", str(tmp_path / "dump"), str(tmp_path / "pack"), "--scale", "3", "--jobs", "1"]
        )
        == 0
    )
    assert "1 files at 3x (scale)" in capsys.readouterr().out
    assert texpack.main(["build", str(tmp_path / "dump"), str(tmp_path / "p"), "--scale", "9"]) == 2


def test_main_skips_without_a_dump_when_asked(tmp_path: Path) -> None:
    args = ["build", str(tmp_path / "none"), str(tmp_path / "pack")]
    assert texpack.main([*args, "--skip-without-dump"]) == texpack.SKIPPED
    assert texpack.main(args) == 2
    assert not (tmp_path / "pack").exists()
