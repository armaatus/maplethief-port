"""Unit tests for the .sprprj parser (synthetic fixtures only; spec: docs/formats/sprprj.md)."""

import struct
import tempfile
import unittest
from pathlib import Path

from mtport import sprprj
from mtport.sprprj import Action, ActionPlayer, Frame, Image, Layer, SpriteProject, SprprjError

# Hand-written bytes: two images (the second with a 2-colour palette), one action with two
# frames. Frame 0 has one layer, frame 1 two layers (one flipped and translucent, one hidden).
SAMPLE = bytes.fromhex(
    "02"  # image_count
    "05" + b"a.png".hex() + "00000000"  # image 0, no palette
    "05" + b"b.png".hex() + "06000000" + "102030405060"  # image 1, palette_len 6
    "01"  # action_count
    "02"  # frame_count
    "030001"  # frame 0: delay 3, 1 layer
    "00ff0001f6ff1400"  # image 0, alpha 255, no flip, visible, x -10, y 20
    "010002"  # frame 1: delay 1, 2 layers
    "018001010500fbff"  # image 1, alpha 128, hflip, visible, x 5, y -5
    "00ff000064006400"  # image 0, alpha 255, hidden, x 100, y 100
)

SAMPLE_PROJECT = SpriteProject(
    images=(Image("a.png"), Image("b.png", bytes.fromhex("102030405060"))),
    actions=(
        Action(
            (
                Frame(3, (Layer(0, 255, False, True, -10, 20),)),
                Frame(
                    1,
                    (
                        Layer(1, 128, True, True, 5, -5),
                        Layer(0, 255, False, False, 100, 100),
                    ),
                ),
            )
        ),
    ),
)


def action(*delays: int) -> Action:
    return Action(tuple(Frame(d, ()) for d in delays))


class ParseTest(unittest.TestCase):
    def test_sample(self):
        self.assertEqual(sprprj.parse(SAMPLE), SAMPLE_PROJECT)

    def test_build_round_trips(self):
        self.assertEqual(sprprj.build(SAMPLE_PROJECT), SAMPLE)
        self.assertEqual(sprprj.parse(sprprj.build(SAMPLE_PROJECT)), SAMPLE_PROJECT)

    def test_empty_project(self):
        # 17 shipped mob projects are exactly these two bytes.
        self.assertEqual(sprprj.parse(b"\0\0"), SpriteProject((), ()))

    def test_frames_iterates_every_frame(self):
        self.assertEqual([(a, f) for a, f, _ in SAMPLE_PROJECT.frames()], [(0, 0), (0, 1)])

    def test_signed_fields(self):
        layer = Layer(0, 0, False, True, -32768, 32767)
        project = SpriteProject((Image("x.png"),), (Action((Frame(-1, (layer,)),)),))
        self.assertEqual(sprprj.parse(sprprj.build(project)), project)

    def test_nonzero_flag_bytes_are_true(self):
        data = bytearray(SAMPLE)
        layer0 = SAMPLE.index(bytes.fromhex("00ff0001f6ff1400"))
        data[layer0 + 2] = 7  # hflip byte
        data[layer0 + 3] = 0x80  # visible byte
        layer = sprprj.parse(bytes(data)).actions[0].frames[0].layers[0]
        self.assertTrue(layer.hflip)
        self.assertTrue(layer.visible)

    def test_every_truncation_is_an_error(self):
        for end in range(len(SAMPLE)):
            with self.subTest(end=end), self.assertRaises(SprprjError):
                sprprj.parse(SAMPLE[:end])

    def test_trailing_bytes_are_an_error(self):
        with self.assertRaisesRegex(SprprjError, "1 trailing bytes"):
            sprprj.parse(SAMPLE + b"\0")

    def test_image_index_out_of_range(self):
        data = bytearray(SAMPLE)
        data[SAMPLE.index(bytes.fromhex("00ff0001f6ff1400"))] = 2
        with self.assertRaisesRegex(SprprjError, "image 2 out of range"):
            sprprj.parse(bytes(data))

    def test_huge_palette_length_is_rejected_without_allocating(self):
        data = b"\x01\x05a.png" + struct.pack("<I", 0xFFFFFFFF) + b"\0"
        with self.assertRaisesRegex(SprprjError, "palette"):
            sprprj.parse(data)

    def test_unsafe_names(self):
        for name in (b"../x.png", b"a/b.png", b"a\\b.png", b"..", b"", b"a\0b", b"\xff.png"):
            data = bytes([1, len(name)]) + name + b"\0\0\0\0\0"
            with self.subTest(name=name), self.assertRaises(SprprjError):
                sprprj.parse(data)

    def test_build_rejects_counts_over_255(self):
        project = SpriteProject(tuple(Image(f"{i}.png") for i in range(256)), ())
        with self.assertRaises(SprprjError):
            sprprj.build(project)


class TimingTest(unittest.TestCase):
    """CMAction update semantics (0x1E585, clamp 0x1E509)."""

    def play(self, act: Action, ticks: int, **kw) -> list[tuple[int, bool]]:
        player = ActionPlayer(act, **kw)
        out = [(player.frame, player.ended)]
        for _ in range(ticks):
            player.update()
            out.append((player.frame, player.ended))
        return out

    def test_delay_is_ticks_per_frame(self):
        frames = [f for f, _ in self.play(action(2, 1, 3), 6)]
        self.assertEqual(frames, [0, 0, 1, 2, 2, 2, 2])

    def test_delay_below_two_is_one_tick(self):
        frames = [f for f, _ in self.play(action(0, -5, 1, 1), 4)]
        self.assertEqual(frames, [0, 1, 2, 3, 3])

    def test_last_frame_holds_and_sets_ended(self):
        # ended is set by the update that tries to step past the last frame.
        states = self.play(action(1, 1), 3)
        self.assertEqual(states, [(0, False), (1, False), (1, True), (1, True)])
        states = self.play(action(1, 3), 5)
        self.assertEqual([e for _, e in states], [False, False, False, False, True, True])

    def test_single_frame_action_starts_ended(self):
        self.assertEqual(self.play(action(4), 1), [(0, True), (0, True)])

    def test_without_delays_every_frame_is_one_tick(self):
        frames = [f for f, _ in self.play(action(5, 5, 5), 3, use_delay=False)]
        self.assertEqual(frames, [0, 1, 2, 2])

    def test_reset_restarts(self):
        player = ActionPlayer(action(1, 1))
        player.update()
        player.reset()
        self.assertEqual((player.frame, player.ticks, player.ended), (0, 0, False))

    def test_action_ticks_matches_player(self):
        for delays in [(1,), (1, 1), (2, 1, 3), (0, 4, 2, 2), (20, 10, 5)]:
            act = action(*delays)
            player = ActionPlayer(act)
            n = 0
            while not player.ended:
                player.update()
                n += 1
            with self.subTest(delays=delays):
                self.assertEqual(sprprj.action_ticks(act), n)

    def test_empty_action_cannot_play(self):
        with self.assertRaises(ValueError):
            ActionPlayer(Action(()))


class BoundsTest(unittest.TestCase):
    def test_visible_layers_only(self):
        sizes = [(10, 20), (4, 4)]
        b = sprprj.frame_bounds(SAMPLE_PROJECT.actions[0].frames[1], sizes)
        self.assertEqual((b.min_x, b.max_x, b.min_y, b.max_y), (5, 9, -5, -1))
        self.assertEqual((b.width, b.height), (4, 4))

    def test_union(self):
        frame = Frame(1, (Layer(0, 255, False, True, -10, 20), Layer(1, 255, True, True, 5, -5)))
        b = sprprj.frame_bounds(frame, [(10, 20), (4, 4)])
        self.assertEqual((b.min_x, b.max_x, b.min_y, b.max_y), (-10, 9, -5, 40))

    def test_no_visible_layer_keeps_sentinels(self):
        b = sprprj.frame_bounds(Frame(1, ()), [])
        self.assertEqual((b.min_x, b.max_x, b.min_y, b.max_y), (10000, -10000, 10000, -10000))


class CliTest(unittest.TestCase):
    def test_check_reports_missing_png(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "mob"
            (root / "res").mkdir(parents=True)
            (root / "0.sprprj").write_bytes(SAMPLE)
            (root / "res" / "a.png").write_bytes(b"")
            self.assertEqual(sprprj.main(["check", tmp]), 1)
            self.assertEqual(sprprj.missing_images(SAMPLE_PROJECT, root / "res"), ["b.png"])
            (root / "res" / "b.png").write_bytes(b"")
            self.assertEqual(sprprj.main(["check", tmp]), 0)


if __name__ == "__main__":
    unittest.main()
