"""offline tests for list_apps, the ls style list_sandbox and system_info with a sleepy GPU.
doesnt depend on whats installed, every app tree is faked in a temp folder and passed to
the scanners as their roots
"""

import os
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

BRAIN_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRAIN_DIR))

from manikin import tools                                  # noqa: E402
from manikin.notes import NoteVault                        # noqa: E402
from manikin.reminders import Reminders                    # noqa: E402
from manikin.tools import (ToolContext, ToolError, build_tools,  # noqa: E402
                           human_size, installed_apps, list_apps, list_sandbox,
                           scan_windows_apps, store_apps_from_start_apps)


def touch(path: Path, text: str = "") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def make_link(link: Path, target: Path) -> None:
    """directory symlink, or a junction when symlinks arent allowed (no admin needed)"""
    try:
        os.symlink(target, link, target_is_directory=True)
    except OSError:
        subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                       check=True, capture_output=True)


class WindowsScanTests(unittest.TestCase):
    def test_shortcuts_filtered_and_deduplicated(self):
        with tempfile.TemporaryDirectory() as d:
            machine, user = Path(d) / "ProgramData", Path(d) / "AppData"
            touch(machine / "Spotify.lnk")
            touch(machine / "Games" / "Hollow Knight.lnk")
            touch(machine / "Foo" / "Uninstall Foo.lnk")
            touch(machine / "Foo" / "Foo Readme.lnk")
            touch(machine / "Foo" / "Foo Help.lnk")
            touch(machine / "Foo" / "Foo on the Web.lnk")
            touch(machine / "Foo" / "Foo Manuals.lnk")
            touch(machine / "Foo" / "Foo.lnk")
            touch(machine / "Foo" / "Foo Website.url")       # not a .lnk
            touch(user / "spotify.lnk")                      # duplicate name, other root
            touch(user / "Google Docs.lnk")                  # "Docs" alone is a real app
            apps = scan_windows_apps([machine, user, Path(d) / "missing"])
            names = sorted(n for n, _ in apps)
            self.assertEqual(names, ["Foo", "Google Docs", "Hollow Knight", "Spotify"])
            target = dict(apps)["Hollow Knight"]
            self.assertEqual(Path(target), machine / "Games" / "Hollow Knight.lnk")

    def test_list_apps_query_cap_and_cache(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / "Programs"
            for i in range(tools.MAX_APPS_LISTED + 10):
                touch(root / f"App {i:03}.lnk")
            touch(root / "Spotify.lnk")
            out = list_apps({}, roots=[root])
            lines = out.splitlines()
            self.assertTrue(lines[0].startswith(f"{tools.MAX_APPS_LISTED + 11} installed apps"))
            self.assertEqual(len(lines), 1 + tools.MAX_APPS_LISTED + 1)
            self.assertIn("11 more not shown", lines[-1])
            self.assertIn("query", lines[-1])
            self.assertEqual(lines[1], f"App 000 -> {root / 'App 000.lnk'}")

            hit = list_apps({"query": "SPOT"}, roots=[root])
            self.assertEqual(hit.splitlines()[1:], [f"Spotify -> {root / 'Spotify.lnk'}"])
            self.assertIn("1 installed app matching", hit)
            self.assertIn("No installed app", list_apps({"query": "zzz"},
                                                        roots=[root]))

            touch(root / "Newcomer.lnk")                     # cached for a minute...
            self.assertNotIn("Newcomer", list_apps({"query": "new"}, roots=[root]))
            fresh = installed_apps([root], use_cache=False)
            self.assertIn("Newcomer", [n for n, _ in fresh])

    def test_store_apps_from_start_apps(self):
        rows = [
            {"Name": "Calculator", "AppID": "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"},
            {"Name": "Firefox", "AppID": "308046B0AF4A39CB"},                       # .lnk backed
            {"Name": "Notepad++", "AppID": r"C:\Program Files\Notepad++\notepad++.exe"},
            {"Name": "Uninstall Thing", "AppID": "Thing_abc!Uninstall"},
            {"Name": "", "AppID": "Blank_abc!App"},
            "garbage",
        ]
        self.assertEqual(store_apps_from_start_apps(rows), [
            ("Calculator", r"shell:AppsFolder\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App")])
        single = {"Name": "Claude", "AppID": "Claude_pzs8sxrjxfjjc!Claude"}   # ConvertTo-Json of one row
        self.assertEqual(store_apps_from_start_apps(single),
                         [("Claude", r"shell:AppsFolder\Claude_pzs8sxrjxfjjc!Claude")])
        self.assertEqual(store_apps_from_start_apps(None), [])

    def test_permission_bubble_names_the_app(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d) / "Programs"
            link = touch(root / "Some Game.lnk")
            installed_apps([root], use_cache=False)
            ctx = ToolContext(Path(d), NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print))
            opener = {t.name: t for t in build_tools(ctx)}["open_app_or_file"]
            self.assertEqual(opener.ask({"target": str(link)}), "Can I open Some Game?")
            self.assertEqual(opener.ask({"target": "notepad"}), "Can I open notepad?")


class SandboxListingTests(unittest.TestCase):
    STAMP = datetime(2026, 3, 4, 5, 6).timestamp()

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.box = self.tmp / "sandbox"
        self.box.mkdir()
        self.ctx = ToolContext(self.box, NoteVault(self.tmp / "n"),
                               Reminders(self.tmp / "r.json", print))

    def tearDown(self):
        self._tmp.cleanup()

    def put(self, rel: str, size: int = 0) -> Path:
        p = self.box / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"x" * size)
        os.utime(p, (self.STAMP, self.STAMP))
        return p

    def when(self) -> str:
        return datetime.fromtimestamp(self.STAMP).strftime("%Y-%m-%d %H:%M")

    def test_human_size(self):
        self.assertEqual([human_size(n) for n in (0, 1023, 1536, 20 * 1024, 5 * 2**20)],
                         ["0B", "1023B", "1.5K", "20K", "5.0M"])

    def test_ls_format_order_and_total(self):
        self.put("zeta.txt", 10)
        self.put("Alpha.txt", 2048)
        self.put("notes/todo.txt", 5)
        self.put("art/pic.png", 1)
        for folder in ("notes", "art"):
            os.utime(self.box / folder, (self.STAMP, self.STAMP))
        out = list_sandbox(self.ctx, {})
        w = self.when()
        self.assertEqual(out.splitlines(), [
            "sandbox:",
            f"d      - {w} art/",
            f"d      - {w} notes/",
            f"-   2.0K {w} Alpha.txt",
            f"-    10B {w} zeta.txt",
            "total: 2 folders, 2 files, 2.0K",
        ])
        for path in ("notes", "notes/", "sandbox/notes", "notes\\"):
            sub = list_sandbox(self.ctx, {"path": path})
            self.assertEqual(sub.splitlines()[0], "sandbox/notes:", path)
            self.assertIn(f"-     5B {w} todo.txt", sub)
            self.assertTrue(sub.endswith("total: 0 folders, 1 file, 5B"))
        self.assertEqual(list_sandbox(self.ctx, {"path": "notes/todo.txt"}),
                         f"-     5B {w} sandbox/notes/todo.txt")

    def test_recursive(self):
        self.put("a.txt", 1)
        self.put("notes/b.txt", 2)
        self.put("notes/deep/c.txt", 3)
        (self.box / "empty").mkdir()
        flat = list_sandbox(self.ctx, {"recursive": False})
        self.assertNotIn("b.txt", flat)
        out = list_sandbox(self.ctx, {"recursive": True})
        headers = [line for line in out.splitlines() if line.endswith(":")]
        self.assertEqual(headers, ["sandbox:", "sandbox/empty:", "sandbox/notes:",
                                   "sandbox/notes/deep:"])
        self.assertIn("sandbox/empty:\n(empty)", out)
        self.assertTrue(out.endswith("total: 3 folders, 3 files, 6B"))

    def test_empty_sandbox(self):
        self.assertEqual(list_sandbox(self.ctx, {}), "sandbox:\n(empty)\ntotal: 0 folders, 0 files, 0B")

    def test_escapes_rejected(self):
        outside = self.tmp / "outside"
        outside.mkdir()
        touch(outside / "secret.txt", "top secret")
        for bad in ["..", "../outside", "notes/../../outside", str(outside), "/", "sandbox/../.."]:
            with self.assertRaises(ToolError, msg=bad):
                list_sandbox(self.ctx, {"path": bad})
        with self.assertRaises(ToolError):
            list_sandbox(self.ctx, {"path": "does-not-exist"})

    def test_links_are_not_followed(self):
        outside = self.tmp / "outside"
        outside.mkdir()
        touch(outside / "secret.txt", "top secret")
        self.put("real/inner.txt", 1)
        make_link(self.box / "escape", outside)
        make_link(self.box / "shortcut", self.box / "real")
        out = list_sandbox(self.ctx, {"recursive": True})
        self.assertNotIn("secret", out)
        self.assertNotIn(str(outside), out)
        self.assertRegex(out, r"(?m)^l .* escape -> outside the sandbox \(not followed\)$")
        self.assertRegex(out, r"(?m)^l .* shortcut -> sandbox/real \(not followed\)$")
        self.assertEqual(out.count("inner.txt"), 1)          # only via the real folder
        with self.assertRaises(ToolError):
            list_sandbox(self.ctx, {"path": "escape"})
        with self.assertRaises(ToolError):
            list_sandbox(self.ctx, {"path": "escape/secret.txt"})

    def test_long_listing_is_capped(self):
        for i in range(tools.MAX_LISTING_LINES + 50):
            self.put(f"f{i:04}.txt")
        out = list_sandbox(self.ctx, {}).splitlines()
        self.assertLessEqual(len(out), tools.MAX_LISTING_LINES + 2)
        self.assertIn("cut off", out[-2])
        self.assertTrue(out[-1].endswith("(so far)"))


def fake_nvml(fail: set[str]):
    """fake pynvml with one GPU, the queries named in `fail` raise like a powered down
    laptop GPU answering some NVML calls with NVMLError_Unknown"""
    class NVMLError(Exception):
        pass

    def query(name, value):
        def call(*_):
            if name in fail:
                raise NVMLError("Unknown Error")
            return value
        return call

    nv = SimpleNamespace(NVMLError=NVMLError, NVML_TEMPERATURE_GPU=0,
                         nvmlInit=lambda: None, nvmlShutdown=lambda: None)
    for name, value in {
        "nvmlDeviceGetCount": 1, "nvmlDeviceGetHandleByIndex": object(),
        "nvmlDeviceGetName": "RTX Test", "nvmlDeviceGetUtilizationRates": SimpleNamespace(gpu=37),
        "nvmlDeviceGetTemperature": 61, "nvmlDeviceGetMemoryInfo": SimpleNamespace(used=2 << 30, total=12 << 30),
        "nvmlDeviceGetComputeRunningProcesses": [], "nvmlDeviceGetGraphicsRunningProcesses": [],
    }.items():
        setattr(nv, name, query(name, value))
    return nv


class SystemInfoGpuTests(unittest.TestCase):
    def gpu(self, fail=()):
        with mock.patch.dict(sys.modules, {"pynvml": fake_nvml(set(fail))}):
            return "\n".join(tools._gpu_lines({}))

    def test_awake_gpu(self):
        out = self.gpu()
        self.assertIn("GPU 0: RTX Test, 37% load, 61 C, VRAM", out)

    def test_some_readings_fail(self):
        # this used to crash the whole tool on a laptop, load and temp fail but the rest answers
        out = self.gpu({"nvmlDeviceGetUtilizationRates", "nvmlDeviceGetTemperature"})
        self.assertIn("GPU 0: RTX Test, VRAM", out)
        self.assertIn("some readings unavailable", out)

    def test_nothing_answers(self):
        out = self.gpu({"nvmlDeviceGetUtilizationRates", "nvmlDeviceGetTemperature",
                        "nvmlDeviceGetMemoryInfo", "nvmlDeviceGetComputeRunningProcesses",
                        "nvmlDeviceGetGraphicsRunningProcesses"})
        self.assertIn("asleep or not reporting", out)
        self.assertNotIn("VRAM", out)

    def test_count_fails(self):
        self.assertEqual(self.gpu({"nvmlDeviceGetCount"}), "")

    def test_system_info_survives_a_sleeping_gpu(self):
        with mock.patch.dict(sys.modules, {"pynvml": fake_nvml({"nvmlDeviceGetUtilizationRates"})}):
            out = tools.system_info({})
        self.assertIn("CPU:", out)
        self.assertIn("RAM:", out)
        self.assertIn("GPU 0: RTX Test", out)


class ValidationTests(unittest.TestCase):
    def test_boolean_arguments(self):
        with tempfile.TemporaryDirectory() as d:
            ctx = ToolContext(Path(d), NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print))
            t = {tool.name: tool for tool in build_tools(ctx)}
            ls = t["list_sandbox"]
            self.assertIsNone(ls.validate({}))
            self.assertIsNone(ls.validate({"path": "notes", "recursive": True}))
            self.assertIsNone(ls.validate({"recursive": False}))
            for bad in ("true", 1, 0, None):
                self.assertIsNotNone(ls.validate({"recursive": bad}), bad)
            self.assertIsNotNone(t["delete_note"].validate({"id": True}))     # still not an int
            self.assertIsNotNone(t["set_reminder"].validate({"text": "x", "minutes_from_now": False}))
            self.assertIsNone(t["list_apps"].validate({"query": "spot"}))
            self.assertIsNotNone(t["list_apps"].validate({"query": 3}))

    def test_list_apps_has_a_default_permission(self):
        from manikin.config import DEFAULT_PERMISSIONS
        self.assertEqual(DEFAULT_PERMISSIONS["list_apps"], "allow")
        self.assertEqual(DEFAULT_PERMISSIONS["list_sandbox"], "allow")


if __name__ == "__main__":
    unittest.main()
