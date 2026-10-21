"""offline tests for the coordinate chain on made up monitor layouts"""

import math
import random
import sys
import unittest
from pathlib import Path

BRAIN_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRAIN_DIR))

from manikin.screen import (MAX_EDGE, MAX_VISUAL_TOKENS, TOKEN_PX, Frame, Monitor, Rect,  # noqa: E402
                            fit_size, logical_to_physical, monitor_at, to_window)

# the laptop, one 2560x1600 panel at 150%
LAPTOP = [Monitor(Rect(0, 0, 2560, 1600), 144, primary=True)]
# a 100% monitor left of a 150% primary and a 125% one above it, so negative coords
MIXED = [
    Monitor(Rect(0, 0, 2560, 1600), 144, primary=True),
    Monitor(Rect(-1920, 200, 1920, 1080), 96),
    Monitor(Rect(300, -1440, 2560, 1440), 120),
]


def tokens(w: int, h: int) -> int:
    return math.ceil(w / TOKEN_PX) * math.ceil(h / TOKEN_PX)


class FitSizeTests(unittest.TestCase):
    def test_laptop_screen_shrinks_to_fit_the_token_budget(self):
        w, h = fit_size(2560, 1600)
        self.assertLessEqual(tokens(w, h), MAX_VISUAL_TOKENS)
        self.assertAlmostEqual(w / h, 2560 / 1600, places=2)
        self.assertGreater(tokens(w, h), MAX_VISUAL_TOKENS * 0.9)    # not wastefully small

    def test_small_images_are_left_alone(self):
        self.assertEqual(fit_size(800, 600), (800, 600))

    def test_long_edge_cap(self):
        w, h = fit_size(4000, 200)
        self.assertLessEqual(max(w, h), MAX_EDGE)

    def test_any_size_fits(self):
        rng = random.Random(1)
        for _ in range(200):
            w, h = fit_size(rng.randint(1, 8000), rng.randint(1, 8000))
            self.assertLessEqual(tokens(w, h), MAX_VISUAL_TOKENS)
            self.assertLessEqual(max(w, h), MAX_EDGE)


class FrameTests(unittest.TestCase):
    def frames(self):
        for m in MIXED:
            w, h = fit_size(m.rect.width, m.rect.height)
            yield Frame(m.rect, w, h)
            yield Frame(m.rect, 1280, round(1280 * m.rect.height / m.rect.width))
        window = Rect(-1500, 400, 900, 700)                     # a window on the left monitor
        yield Frame(window, *fit_size(window.width, window.height))
        yield Frame(window, 900, 700)                           # unscaled

    def test_round_trip_is_exact(self):
        rng = random.Random(2)
        for f in self.frames():
            for _ in range(100):
                x = rng.uniform(f.source.left, f.source.right)
                y = rng.uniform(f.source.top, f.source.bottom)
                bx, by = f.to_desktop(*f.to_image(x, y))
                self.assertAlmostEqual(bx, x, places=6)
                self.assertAlmostEqual(by, y, places=6)

    def test_integer_answers_land_within_one_source_pixel_block(self):
        # whole image px answers cost at most half an image px, so half a source px times the downscale
        rng = random.Random(3)
        for f in self.frames():
            for _ in range(100):
                x = rng.uniform(f.source.left, f.source.right - 1)
                y = rng.uniform(f.source.top, f.source.bottom - 1)
                ix, iy = f.to_image(x, y)
                bx, by = f.to_desktop(round(ix), round(iy))
                self.assertLessEqual(abs(bx - x), 0.5 / f.sx + 1e-9)
                self.assertLessEqual(abs(by - y), 0.5 / f.sy + 1e-9)

    def test_corners(self):
        f = Frame(Rect(-1920, 200, 1920, 1080), 1280, 720)
        ix, iy = f.to_image(-1920, 200)                 # centre of the first pixel
        self.assertAlmostEqual(ix, 0.5 * 2 / 3 - 0.5)
        self.assertAlmostEqual(iy, 0.5 * 2 / 3 - 0.5)
        x, y = f.to_desktop(1279, 719)
        self.assertTrue(f.source.contains(x, y))
        x, y = f.to_desktop(0, 0)
        self.assertTrue(f.source.contains(x, y))

    def test_rect_round_trip_covers_the_original(self):
        f = Frame(Rect(300, -1440, 2560, 1440), *fit_size(2560, 1440))
        r = Rect(1000, -900, 37, 19)
        back = f.rect_to_desktop(*f.rect_to_image(r))
        self.assertIsNotNone(back.intersect(r))
        self.assertLessEqual(abs(back.left - r.left), 1)
        self.assertLessEqual(abs(back.right - r.right), 1)

    def test_centre_of_a_target_maps_inside_it(self):
        f = Frame(LAPTOP[0].rect, *fit_size(2560, 1600))
        button = Rect(2400, 10, 46, 30)                 # a title bar button
        ix, iy = f.to_image(*button.center)
        self.assertTrue(button.contains(*f.to_desktop(round(ix), round(iy))))


class MonitorTests(unittest.TestCase):
    def test_monitor_at_handles_negative_space(self):
        self.assertEqual(monitor_at(MIXED, -10, 500).dpi, 96)
        self.assertEqual(monitor_at(MIXED, 400, -10).dpi, 120)
        self.assertTrue(monitor_at(MIXED, 0, 0).primary)
        self.assertIsNone(monitor_at(MIXED, -10, 0))       # the gap above the left monitor

    def test_logical_to_physical(self):
        self.assertEqual(logical_to_physical(LAPTOP, 100, 200), (150, 300))
        self.assertEqual(logical_to_physical(MIXED, -100, 300), (-100, 300))   # 100% monitor

    def test_to_window_subtracts_the_overlay_position(self):
        # the overlay where it sits on the laptop, bottom right above the taskbar
        overlay = Rect(2124, 946, 420, 582)
        self.assertEqual(to_window(2124, 946, overlay), (0, 0))
        self.assertEqual(to_window(100.4, 1527.6, overlay), (-2024, 582))


class RectTests(unittest.TestCase):
    def test_distance(self):
        r = Rect(10, 10, 10, 10)
        self.assertEqual(r.distance(15, 15), 0)
        self.assertEqual(r.distance(19, 19), 0)
        self.assertEqual(r.distance(22, 19), 3)
        self.assertAlmostEqual(r.distance(6, 6), math.hypot(4, 4))


if __name__ == "__main__":
    unittest.main()
