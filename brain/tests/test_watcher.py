"""offline tests for the PC watcher with a fake sampler, clock and local time"""

import sys
import time
import unittest
from pathlib import Path

BRAIN_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRAIN_DIR))

from manikin.watcher import COOLDOWN, Sample, Watcher     # noqa: E402

T0 = 1_000_000.0      # arbitrary, local time comes from the fake below and not the real timezone
STEP = 10.0           # the app polls about every 10 s


def fake_localtime(now: float) -> time.struct_time:
    """hour of day = whole hours since epoch mod 24, same every run and no timezone"""
    secs = int(now)
    hour, minute = (secs // 3600) % 24, (secs // 60) % 60
    return time.struct_time((2026, 10, 19, hour, minute, 0, 0, 292, -1))


def noon(_: float) -> time.struct_time:
    return time.struct_time((2026, 10, 19, 12, 0, 0, 0, 292, -1))


class FakeSampler:
    def __init__(self, sample: Sample = Sample()):
        self.sample = sample

    def __call__(self) -> Sample:
        return self.sample


class Harness:
    """a Watcher plus a clock that moves STEP secs per poll"""

    def __init__(self, sample: Sample = Sample(), localtime=noon, start: float = T0):
        self.sampler = FakeSampler(sample)
        self.watcher = Watcher(self.sampler, localtime=localtime)
        self.now = start

    def run(self, sample: Sample | None = None, seconds: float = STEP) -> list:
        """polls every STEP s for `seconds` and returns every observation seen"""
        if sample is not None:
            self.sampler.sample = sample
        seen = []
        for _ in range(max(1, int(seconds // STEP))):
            seen += self.watcher.poll(self.now)
            self.now += STEP
        return seen


def kinds(obs: list) -> list[str]:
    return [o.kind for o in obs]


class CpuTests(unittest.TestCase):
    def test_fires_once_after_a_minute_and_not_again(self):
        h = Harness(Sample(cpu=95.0))
        # 6 polls cover 50 s, the first five dont fire and the sixth does
        self.assertEqual(h.run(seconds=50), [])
        obs = h.run()
        self.assertEqual(kinds(obs), ["cpu_high"])
        self.assertIn("95%", obs[0].text)
        self.assertTrue(obs[0].text.isascii())
        self.assertEqual(h.run(seconds=3 * COOLDOWN), [])     # still high, no repeats

    def test_rearms_after_clear_and_cooldown(self):
        h = Harness(Sample(cpu=95.0))
        self.assertEqual(kinds(h.run(seconds=60)), ["cpu_high"])
        h.run(Sample(cpu=10.0), seconds=COOLDOWN + 60)         # clear for the whole cooldown
        self.assertEqual(kinds(h.run(Sample(cpu=95.0), seconds=120)), ["cpu_high"])

    def test_clearing_during_cooldown_is_not_enough(self):
        h = Harness(Sample(cpu=95.0))
        h.run(seconds=60)
        h.run(Sample(cpu=10.0), seconds=300)                    # clears early...
        # ...but its high again when the cooldown ends so it stays quiet till it clears again
        self.assertEqual(h.run(Sample(cpu=95.0), seconds=COOLDOWN), [])
        h.run(Sample(cpu=10.0), seconds=120)
        self.assertEqual(kinds(h.run(Sample(cpu=95.0), seconds=120)), ["cpu_high"])

    def test_hysteresis(self):
        h = Harness(Sample(cpu=95.0))
        h.run(seconds=60)
        # dipping to 80% (under the trigger but over the 75% clear level) never rearms
        h.run(Sample(cpu=80.0), seconds=COOLDOWN + 600)
        self.assertEqual(h.run(Sample(cpu=95.0), seconds=120), [])
        h.run(Sample(cpu=70.0), seconds=120)
        self.assertEqual(kinds(h.run(Sample(cpu=95.0), seconds=120)), ["cpu_high"])

    def test_average_not_instant(self):
        h = Harness()
        # alternating 100/70 averages 85%, spiky but not a steady 90%
        seen = []
        for i in range(30):
            seen += h.run(Sample(cpu=100.0 if i % 2 else 70.0))
        self.assertEqual(seen, [])

    def test_old_samples_drop_out_of_the_window(self):
        h = Harness(Sample(cpu=20.0))
        h.run(seconds=600)
        # 60 s in the window still has one 20% reading (avg 84%) so not yet
        self.assertEqual(h.run(Sample(cpu=95.0), seconds=60), [])
        # 10 s later its dropped out and the window is all high
        self.assertEqual(kinds(h.run()), ["cpu_high"])

    def test_gap_in_samples_needs_a_new_warmup(self):
        h = Harness(Sample(cpu=95.0))
        h.run(seconds=40)
        h.run(Sample(), seconds=120)                            # no CPU readings for 2 min
        self.assertEqual(h.run(Sample(cpu=95.0), seconds=50), [])
        self.assertEqual(kinds(h.run()), ["cpu_high"])

    def test_clock_jump_gives_no_false_average(self):
        h = Harness(Sample(cpu=95.0))
        h.run(seconds=40)
        h.now += 3600                                           # laptop slept for an hour
        self.assertEqual(h.run(seconds=50), [])


class GpuTests(unittest.TestCase):
    def test_fires_once_and_rearms(self):
        h = Harness(Sample(gpu=99.0))
        obs = h.run(seconds=60)
        self.assertEqual(kinds(obs), ["gpu_high"])
        self.assertIn("99%", obs[0].text)
        self.assertEqual(h.run(seconds=COOLDOWN), [])
        h.run(Sample(gpu=50.0), seconds=COOLDOWN)
        self.assertEqual(kinds(h.run(Sample(gpu=99.0), seconds=120)), ["gpu_high"])

    def test_hysteresis(self):
        h = Harness(Sample(gpu=99.0))
        h.run(seconds=60)
        h.run(Sample(gpu=80.0), seconds=COOLDOWN + 600)
        self.assertEqual(h.run(Sample(gpu=99.0), seconds=120), [])

    def test_gpu_going_dark_rearms(self):
        # a laptop dGPU that powers down after a game reads None, that cant leave the trigger stuck
        h = Harness(Sample(gpu=99.0))
        h.run(seconds=60)
        h.run(Sample(gpu=None), seconds=COOLDOWN + 60)
        self.assertEqual(kinds(h.run(Sample(gpu=99.0), seconds=120)), ["gpu_high"])

    def test_short_gap_does_not_rearm(self):
        h = Harness(Sample(gpu=99.0))
        h.run(seconds=COOLDOWN + 60)
        h.run(Sample(gpu=None), seconds=30)          # a few failed reads mid game
        self.assertEqual(h.run(Sample(gpu=99.0), seconds=120), [])

    def test_crossing_on_a_missing_reading_fires_on_the_next_one(self):
        # the old 0% ages out on a poll with no reading of its own. the average is 99% now
        # but the observation waits for a real reading instead of getting lost
        h = Harness(Sample(gpu=0.0))
        h.run()
        self.assertEqual(h.run(Sample(gpu=99.0), seconds=60), [])
        self.assertEqual(h.run(Sample(gpu=None)), [])
        obs = h.run(Sample(gpu=99.0))
        self.assertEqual(kinds(obs), ["gpu_high"])
        self.assertIn("99% now", obs[0].text)

    def test_no_gpu_never_fires(self):
        h = Harness(Sample(cpu=10.0, gpu=None))
        self.assertEqual(h.run(seconds=600), [])


class RamTests(unittest.TestCase):
    def test_fires_immediately_once(self):
        h = Harness(Sample(ram=93.0, ram_used_gb=14.9, ram_total_gb=16.0))
        obs = h.run()
        self.assertEqual(kinds(obs), ["ram_high"])
        self.assertEqual(obs[0].text, "RAM is 93% full (14.9 GB of 16.0 GB used)")
        self.assertEqual(h.run(seconds=COOLDOWN * 2), [])

    def test_hysteresis_and_rearm(self):
        h = Harness(Sample(ram=93.0))
        self.assertEqual(kinds(h.run()), ["ram_high"])
        h.run(Sample(ram=85.0), seconds=COOLDOWN + 60)          # below 90 but not below 80
        self.assertEqual(h.run(Sample(ram=93.0)), [])
        h.run(Sample(ram=60.0), seconds=60)
        obs = h.run(Sample(ram=91.0))
        self.assertEqual(kinds(obs), ["ram_high"])
        self.assertEqual(obs[0].text, "RAM is 91% full")        # no GB figures given

    def test_missing_data_does_not_rearm(self):
        h = Harness(Sample(ram=93.0))
        h.run()
        h.run(Sample(), seconds=COOLDOWN * 2)
        self.assertEqual(h.run(Sample(ram=93.0)), [])


class BatteryTests(unittest.TestCase):
    def test_low_then_critical_once_each(self):
        h = Harness(Sample(battery=14.0, plugged=False))
        obs = h.run()
        self.assertEqual(kinds(obs), ["battery_low"])
        self.assertNotIn("critical", obs[0].text)
        self.assertIn("14%", obs[0].text)
        self.assertEqual(h.run(Sample(battery=10.0, plugged=False), seconds=60), [])
        obs = h.run(Sample(battery=4.0, plugged=False))
        self.assertEqual(kinds(obs), ["battery_low"])
        self.assertIn("critical", obs[0].text)
        self.assertEqual(h.run(Sample(battery=3.0, plugged=False), seconds=600), [])

    def test_starting_critical_is_one_observation(self):
        h = Harness(Sample(battery=4.0, plugged=False))
        obs = h.run()
        self.assertEqual(len(obs), 1)
        self.assertIn("critical", obs[0].text)
        self.assertEqual(h.run(Sample(battery=12.0, plugged=False), seconds=600), [])

    def test_plugged_in_never_fires(self):
        h = Harness(Sample(battery=3.0, plugged=True))
        self.assertEqual(h.run(seconds=600), [])

    def test_rearms_after_charging_and_cooldown(self):
        h = Harness(Sample(battery=14.0, plugged=False))
        h.run()
        h.run(Sample(battery=14.0, plugged=True), seconds=COOLDOWN)
        self.assertEqual(kinds(h.run(Sample(battery=14.0, plugged=False))), ["battery_low"])

    def test_hysteresis(self):
        h = Harness(Sample(battery=14.0, plugged=False))
        h.run()
        h.run(Sample(battery=17.0, plugged=False), seconds=COOLDOWN + 60)   # not above 20%
        self.assertEqual(h.run(Sample(battery=14.0, plugged=False)), [])
        h.run(Sample(battery=25.0, plugged=False))
        self.assertEqual(kinds(h.run(Sample(battery=14.0, plugged=False))), ["battery_low"])

    def test_desktop_without_battery(self):
        h = Harness(Sample(battery=None, plugged=None))
        self.assertEqual(h.run(seconds=600), [])


class NightTests(unittest.TestCase):
    def at(self, day: int, hour: int, minute: int = 0) -> float:
        return float(day * 86400 + hour * 3600 + minute * 60)

    def test_once_per_night_and_resets(self):
        h = Harness(localtime=fake_localtime, start=self.at(10, 0, 50))
        self.assertEqual(h.run(seconds=600), [])                # 00:50 to 01:00
        obs = h.run()                                           # 01:00
        self.assertEqual(kinds(obs), ["late_night"])
        self.assertEqual(obs[0].text, "It is 01:00 local time, late at night")
        self.assertEqual(h.run(seconds=4 * 3600), [])           # the rest of the night
        h.now = self.at(10, 12)
        self.assertEqual(h.run(seconds=600), [])                # midday
        h.now = self.at(11, 3, 30)
        obs = h.run()
        self.assertEqual(kinds(obs), ["late_night"])            # next night
        self.assertIn("03:30", obs[0].text)

    def test_window_edges(self):
        h = Harness(localtime=fake_localtime, start=self.at(10, 4, 59))
        self.assertEqual(kinds(h.run()), ["late_night"])        # 04:59 counts
        h = Harness(localtime=fake_localtime, start=self.at(10, 5, 0))
        self.assertEqual(h.run(seconds=6 * 3600), [])           # 05:00 to 11:00 doesnt

    def test_real_localtime_from_mktime(self):
        # same check against the real time.localtime with a timestamp built in local time
        two_am = time.mktime((2026, 1, 15, 2, 0, 0, 0, 0, -1))
        h = Harness(start=two_am, localtime=time.localtime)
        self.assertEqual(kinds(h.run()), ["late_night"])


class RobustnessTests(unittest.TestCase):
    def test_several_kinds_in_one_poll(self):
        h = Harness(Sample(cpu=95.0, gpu=95.0, ram=95.0, battery=10.0, plugged=False))
        seen = h.run(seconds=60)
        self.assertEqual(sorted(kinds(seen)), ["battery_low", "cpu_high", "gpu_high", "ram_high"])

    def test_sampler_crash_does_not_raise(self):
        def broken() -> Sample:
            raise RuntimeError("sensor exploded")
        w = Watcher(broken, localtime=noon)
        with self.assertLogs("manikin.watcher", "WARNING"):
            self.assertEqual(w.poll(T0), [])

    def test_default_now(self):
        w = Watcher(FakeSampler(Sample(ram=95.0)), localtime=noon)
        self.assertEqual(kinds(w.poll()), ["ram_high"])


if __name__ == "__main__":
    unittest.main()
