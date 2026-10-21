"""offline pointing tests, no API calls, no screen, no desktop. the coordinator gets driven
like the brain drives it, the tool side from a thread and point_done and cancel from this
(the "event loop") side"""

import asyncio
import json
import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace as NS

BRAIN_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRAIN_DIR))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from manikin import config                                       # noqa: E402
from manikin.app import Brain                                    # noqa: E402
from manikin.backend import ClaudeBackend, EchoBackend           # noqa: E402
from manikin.gate import Gate                                    # noqa: E402
from manikin.initiative import Initiative                        # noqa: E402
from manikin.notes import NoteVault                              # noqa: E402
from manikin.pointer import (POINT_TOOL, Element, Outcome, Pointer, Sighting,  # noqa: E402
                             Snap, Spot, WindowInfo, app_name, ask_text, choose_box, clip_to_monitor,
                             describe, find_and_point, locate, own_pids, pick_window, point_line,
                             window_label)
from manikin.reminders import Reminders                          # noqa: E402
from manikin.screen import Frame, Monitor, Rect                  # noqa: E402
from manikin.tools import ToolContext, ToolError, build_tools    # noqa: E402
from test_brain import FakeClaude, _response, _text, _tool_call  # noqa: E402


class ThreadPipe:
    """records whats sent from any thread and lets a test wait for a message type"""

    def __init__(self):
        self.sent = []
        self._cond = threading.Condition()

    def send(self, type_, **fields):
        with self._cond:
            self.sent.append({"type": type_, **fields})
            self._cond.notify_all()

    def wait_for(self, type_, count=1, timeout=2.0) -> dict:
        with self._cond:
            ok = self._cond.wait_for(lambda: len(self.of(type_)) >= count, timeout)
        if not ok:
            raise AssertionError(f"no {type_} #{count} in {self.sent}")
        return self.of(type_)[count - 1]

    def of(self, type_):
        return [m for m in self.sent if m["type"] == type_]


SPOT = Spot(1210, 388, Rect(1180, 370, 60, 36), 132456, "Click this one")


def run_in_thread(fn, *args):
    out = {}
    t = threading.Thread(target=lambda: out.setdefault("value", fn(*args)), daemon=True)
    t.start()
    return t, out


class PointerTests(unittest.TestCase):
    def make(self, **kw):
        pipe = ThreadPipe()
        kw.setdefault("settle_s", 0)
        kw.setdefault("name_at", lambda x, y: "'Back' (Button)")
        return pipe, Pointer(pipe, **kw)

    def test_point_then_click_inside(self):
        pipe, pointer = self.make()
        with self.assertLogs("manikin.pointer", "INFO") as logs:
            t, out = run_in_thread(pointer.show, SPOT, True, {"q": "night light", "model": "m"})
            sent = pipe.wait_for("point")
            self.assertEqual(sent, {"type": "point", "id": 1, "x": 1210, "y": 388, "box": [1180, 370, 60, 36],
                                    "window": 132456, "text": "Click this one"})
            self.assertTrue(pointer.waiting)
            pointer.on_done({"type": "point_done", "id": 1, "outcome": "clicked", "x": 1215, "y": 390,
                             "inside": True})
            t.join(2)
        self.assertEqual(out["value"], Outcome("clicked", 1215, 390, True))
        self.assertEqual(describe(out["value"]), "The user clicked it.")
        self.assertEqual(pipe.of("point_clear"), [])            # the overlay ended it itself
        line = [l for l in logs.output if "point:" in l]
        self.assertEqual(len(line), 1)
        self.assertIn('point: id=1 outcome=clicked q="night light" model=m at=1210,388 box=1180,370,60,36 '
                      'wait=yes click=1215,390 inside=yes dist=0', line[0])
        self.assertFalse(pointer.waiting)

    def test_click_elsewhere_names_what_was_hit(self):
        pipe, pointer = self.make()
        with self.assertLogs("manikin.pointer", "INFO") as logs:
            t, out = run_in_thread(pointer.show, SPOT, True, {})
            pipe.wait_for("point")
            pointer.on_done({"type": "point_done", "id": 1, "outcome": "clicked", "x": 100, "y": 370,
                             "inside": False})
            t.join(2)
        self.assertEqual(describe(out["value"]), "The user clicked somewhere else (on 'Back' (Button)).")
        self.assertIn("inside=no dist=1080 clicked_on=\"'Back' (Button)\"", logs.output[-1])

    def test_waiting_point_times_out(self):
        pipe, pointer = self.make(wait_s=0.05)
        with self.assertLogs("manikin.pointer", "INFO") as logs:
            outcome = pointer.show(SPOT, True)
        self.assertEqual(outcome.kind, "timeout")
        self.assertEqual(pipe.of("point_clear"), [{"type": "point_clear", "id": 1}])
        self.assertIn("outcome=timeout", logs.output[-1])
        self.assertEqual(describe(outcome, 120), "They didn't click within 2 minutes.")

    def test_unwatched_point_is_cleared_after_a_while(self):
        pipe, pointer = self.make(linger_s=0.05)
        with self.assertLogs("manikin.pointer", "INFO") as logs:
            self.assertIsNone(pointer.show(SPOT, False))         # returns at once
            pipe.wait_for("point_clear")
        self.assertIn("outcome=timeout", logs.output[-1])
        self.assertIn("wait=no", logs.output[-1])

    def test_unwatched_click_is_still_logged(self):
        pipe, pointer = self.make()
        with self.assertLogs("manikin.pointer", "INFO") as logs:
            pointer.show(SPOT, False, {"q": "x"})
            pointer.on_done({"type": "point_done", "id": 1, "outcome": "clicked", "x": 1179, "y": 380,
                             "inside": True})
        self.assertIn("outcome=clicked", logs.output[-1])
        self.assertIn("inside=yes dist=1", logs.output[-1])
        self.assertEqual(pipe.of("point_clear"), [])

    def test_cancel_clears_and_wakes_the_tool(self):
        pipe, pointer = self.make()
        t, out = run_in_thread(pointer.show, SPOT, True)
        pipe.wait_for("point")
        pointer.cancel()
        t.join(2)
        self.assertEqual(out["value"].kind, "cancelled")
        self.assertEqual(pipe.of("point_clear"), [{"type": "point_clear", "id": 1}])
        pointer.cancel()                                         # nothing active, nothing sent
        self.assertEqual(len(pipe.of("point_clear")), 1)

    def test_interrupt_only_stops_a_waiting_point(self):
        pipe, pointer = self.make()
        pointer.show(SPOT, False)
        pointer.interrupt()
        self.assertEqual(pipe.of("point_clear"), [])             # not waited on so it stays up
        t, out = run_in_thread(pointer.show, SPOT, True)
        pipe.wait_for("point", 2)
        pointer.interrupt()
        t.join(2)
        self.assertEqual(out["value"].kind, "interrupted")

    def test_a_new_point_replaces_the_old_one(self):
        pipe, pointer = self.make()
        with self.assertLogs("manikin.pointer", "INFO") as logs:
            pointer.show(SPOT, False)
            pointer.show(SPOT, False)
        self.assertEqual([(m["type"], m["id"]) for m in pipe.sent],
                         [("point", 1), ("point_clear", 1), ("point", 2)])
        self.assertIn("id=1 outcome=replaced", logs.output[-1])
        pointer.cancel()

    def test_late_and_duplicate_point_done_are_ignored(self):
        pipe, pointer = self.make()
        with self.assertLogs("manikin.pointer", "INFO") as logs:
            pointer.show(SPOT, False)
            pointer.cancel()                                     # sends point_clear for 1
            pointer.on_done({"type": "point_done", "id": 1, "outcome": "cleared"})     # its echo
            pointer.on_done({"type": "point_done", "id": 1, "outcome": "clicked", "x": 1, "y": 1})
            pointer.on_done({"type": "point_done", "id": 99, "outcome": "moved"})
            pointer.on_done({"type": "point_done", "id": "1", "outcome": "moved"})
            pointer.on_done({"type": "point_done", "id": 1, "outcome": "exploded"})
        outcomes = [l for l in logs.output if "point: " in l]
        self.assertEqual(len(outcomes), 1)                       # exactly one outcome for id 1
        self.assertIn("outcome=cancelled", outcomes[0])

    def test_log_line_quotes_strings(self):
        self.assertEqual(point_line({"id": 3, "q": 'the "Save" button', "win": "Settings", "t": 1.5}),
                         'point: id=3 q="the \\"Save\\" button" win=Settings t=1.50')


class WindowPickTests(unittest.TestCase):
    def test_own_pids_reach_past_a_launcher(self):
        # a venv python.exe is a launcher so the overlay is our grandparent, every
        # ancestor counts as us
        import psutil
        pids = own_pids()
        self.assertIn(os.getpid(), pids)
        self.assertTrue({p.pid for p in psutil.Process().parents()} <= pids)

    def test_window_label_doesnt_say_window_twice(self):
        self.assertEqual(window_label(WindowInfo(1, "Pointer Test Window", Rect(0, 0, 9, 9))),
                         "your Pointer Test Window")
        self.assertEqual(window_label(WindowInfo(1, "Display - Settings", Rect(0, 0, 9, 9))),
                         "your Settings window")

    def win(self, handle, **kw):
        kw.setdefault("title", f"Window {handle}")
        kw.setdefault("rect", Rect(0, 0, 800, 600))
        kw.setdefault("pid", 10)
        return WindowInfo(handle, **kw)

    def test_filters(self):
        overlay = 4242
        windows = [
            self.win(1, pid=overlay),                            # the overlay itself
            self.win(2, cloaked=True),                           # another virtual desktop
            self.win(3, tool=True),
            self.win(4, title="  "),
            self.win(5, rect=Rect(0, 0, 80, 600)),               # too small
            self.win(6, minimized=True),
            self.win(7, cls="Shell_TrayWnd"),
            self.win(8, visible=False),
            self.win(9, topmost=True),                           # always on top, only a last resort
            self.win(10, title="Settings"),
            self.win(11),
        ]
        self.assertEqual(pick_window(windows, foreground=1, skip_pids={overlay}).handle, 10)
        self.assertEqual(pick_window(windows, foreground=11, skip_pids={overlay}).handle, 11)
        self.assertEqual(pick_window(windows[:9], foreground=0, skip_pids={overlay}).handle, 9)
        self.assertIsNone(pick_window(windows[:8], foreground=0, skip_pids={overlay}))

    def test_clip_to_monitor_and_names(self):
        monitors = [Monitor(Rect(0, 0, 2560, 1600), 144, True), Monitor(Rect(-1920, 0, 1920, 1080))]
        self.assertEqual(clip_to_monitor(Rect(-8, -8, 2576, 1544), monitors), Rect(0, 0, 2560, 1536))
        self.assertEqual(clip_to_monitor(Rect(-1900, 10, 1000, 500), monitors), Rect(-1900, 10, 1000, 500))
        self.assertEqual(app_name("GitHub - Mozilla Firefox"), "Mozilla Firefox")
        self.assertEqual(app_name("Settings"), "Settings")
        self.assertEqual(app_name("main.c - proj - Visual Studio Code"), "Visual Studio Code")


class SnapTests(unittest.TestCase):
    WINDOW = Rect(0, 0, 1200, 900)

    def test_accepts_a_control_and_keeps_the_point(self):
        e = Element(Rect(100, 100, 200, 40), "Display resolution", "ComboBox")
        self.assertEqual(choose_box(150, 120, self.WINDOW, [e]), Snap(e.rect, "Display resolution", "ComboBox"))

    def test_prefers_the_clickable_parent_of_a_label(self):
        label = Element(Rect(110, 110, 60, 20), "1920 x 1080", "Text")
        combo = Element(Rect(100, 100, 200, 40), "Display resolution", "ComboBox")
        self.assertEqual(choose_box(120, 115, self.WINDOW, [label, combo]).kind, "ComboBox")

    def test_what_the_smoke_test_ran_into(self):
        # settings resolution dropdown, the value shown inside it is a ListItem
        item = Element(Rect(1900, 873, 272, 28), "2560 x 1600 (Recommended)", "ListItem")
        combo = Element(Rect(1876, 858, 359, 60), "Display resolution", "ComboBox")
        self.assertEqual(choose_box(2054, 888, Rect(0, 0, 2560, 1528), [item, combo]).name, "Display resolution")
        # vs code Pull button, an unnamed icon Group inside the named button
        icon = Element(Rect(447, 798, 30, 29), "", "Group")
        pull = Element(Rect(443, 794, 37, 37), "Pull", "Button")
        self.assertEqual(choose_box(460, 814, Rect(0, 0, 2560, 1528), [icon, pull]).box, pull.rect)
        # a settings switch inside its row, should pick the switch
        switch = Element(Rect(2300, 500, 80, 40), "Night light", "Button")
        row = Element(Rect(400, 480, 2000, 80), "Night light", "ListItem")
        self.assertEqual(choose_box(2320, 520, Rect(0, 0, 2560, 1528), [switch, row]).box, switch.rect)

    def test_rejects_and_falls_back_to_a_box_around_the_point(self):
        default = Snap(Rect(126, 96, 48, 48))
        for bad in [Element(Rect(200, 200, 50, 50), "elsewhere", "Button"),      # doesnt hold the point
                    Element(Rect(140, 100, 5, 40), "sliver", "Button"),          # under 8 px
                    Element(Rect(0, 0, 1200, 900), "page", "Pane")]:             # a sixth of the window or more
            self.assertEqual(choose_box(150, 120, self.WINDOW, [bad]), default, bad.name)
        self.assertEqual(choose_box(150, 120, self.WINDOW, []), default)


class LocateTests(unittest.TestCase):
    FRAME = Frame(Rect(100, 50, 2560, 1600), 1280, 800)

    def client(self, *content):
        calls = []

        def create(**kw):
            calls.append(kw)
            return NS(content=list(content), usage=NS(input_tokens=1500, output_tokens=30))
        return NS(beta=NS(messages=NS(create=create))), calls

    def image(self):
        from PIL import Image
        return Image.new("RGB", (1280, 800), (30, 30, 30))

    def test_mouse_move_maps_to_the_desktop(self):
        client, calls = self.client(_text("There."), NS(type="tool_use", id="t", name="mouse_move",
                                                        input={"action": "mouse_move", "coordinate": [640, 400]}))
        seen = locate("Where's night light?", self.image(), self.FRAME, "Settings", client=client)
        self.assertEqual(seen.point, self.FRAME.to_desktop(640, 400))
        self.assertAlmostEqual(seen.point[0], 100 + 1280.5, places=6)
        self.assertEqual(seen.tokens, (1500, 30))
        request = calls[0]
        self.assertEqual(request["model"], config.POINT_MODEL)
        self.assertEqual(request["tools"], [POINT_TOOL])
        self.assertNotIn("mouse_move", POINT_TOOL["configs"])
        prompt, image = request["messages"][0]["content"]
        self.assertIn('"Where\'s night light?"', prompt["text"])
        self.assertIn("their Settings window", prompt["text"])
        self.assertEqual(image["type"], "image")

    def test_off_the_edge_is_pulled_onto_the_picture(self):
        client, _ = self.client(NS(type="tool_use", id="t", name="mouse_move", input={"coordinate": [1300, -4]}))
        seen = locate("x", self.image(), self.FRAME, client=client)
        self.assertEqual(seen.point, self.FRAME.to_desktop(1279, 0))

    def test_no_tool_call_means_not_visible(self):
        client, _ = self.client(_text("It isn't on screen; scroll down to Advanced."))
        seen = locate("x", self.image(), self.FRAME, client=client)
        self.assertIsNone(seen.point)
        self.assertEqual(seen.said, "It isn't on screen; scroll down to Advanced.")


class FindAndPointTests(unittest.TestCase):
    WINDOW = WindowInfo(77, "Display - Settings", Rect(0, 0, 2560, 1528), pid=5)

    def run_tool(self, seen: Sighting, wait=False, done=None):
        from PIL import Image
        pipe = ThreadPipe()
        pointer = Pointer(pipe, settle_s=0, name_at=lambda x, y: "")
        looked = {}

        def look(question, image, frame, window):
            looked.update(question=question, size=image.size, window=window)
            return seen
        if done:
            threading.Thread(target=lambda: (pipe.wait_for("point"), pointer.on_done(done)), daemon=True).start()
        result = find_and_point(
            pointer, "the night light switch", "Flip this", wait,
            find=lambda: self.WINDOW,
            capture=lambda w: (Image.new("RGB", (2560, 1528)), w.rect),
            look=look,
            refine=lambda x, y, r: Snap(Rect(1100, 600, 80, 40), "Night light", "Button"))
        pointer.cancel()
        return result, looked, pipe

    def test_points_and_reports_the_control(self):
        with self.assertLogs("manikin.pointer", "INFO"):
            result, looked, pipe = self.run_tool(Sighting((1130.4, 615.6), "", 1.2, "haiku"))
        self.assertEqual(result, "Pointing at 'Night light' (Button) in Settings.")
        self.assertEqual(looked, {"question": "the night light switch", "size": (1280, 764), "window": "Settings"})
        point = pipe.of("point")[0]
        self.assertEqual((point["x"], point["y"], point["box"], point["window"], point["text"]),
                         (1130, 616, [1100, 600, 80, 40], 77, "Flip this"))

    def test_wait_reports_the_click(self):
        with self.assertLogs("manikin.pointer", "INFO") as logs:
            result, _, _ = self.run_tool(Sighting((1130, 615), "", 1.2, "haiku"), wait=True,
                                         done={"id": 1, "outcome": "moved"})
        self.assertEqual(result, "Pointed at 'Night light' (Button) in Settings. The window changed before they clicked.")
        self.assertIn('el="Button:Night light"', logs.output[-1])

    def test_cancel_during_the_look_shows_nothing(self):
        from PIL import Image
        pipe = ThreadPipe()
        pointer = Pointer(pipe, settle_s=0, name_at=lambda x, y: "")

        def look(*_):
            pointer.cancel()                                     # Esc while Haiku is still looking
            return Sighting((10, 10), "", 1.0, "haiku")
        with self.assertLogs("manikin.pointer", "INFO") as logs:
            result = find_and_point(pointer, "x", "", False, find=lambda: self.WINDOW,
                                    capture=lambda w: (Image.new("RGB", (2560, 1528)), w.rect), look=look,
                                    refine=lambda x, y, r: Snap(Rect(0, 0, 48, 48)))
        self.assertEqual(pipe.sent, [])
        self.assertIn("The user cancelled.", result)
        self.assertIn("outcome=cancelled", logs.output[-1])

    def test_not_visible_passes_the_models_words_on(self):
        with self.assertLogs("manikin.pointer", "INFO") as logs:
            result, _, pipe = self.run_tool(Sighting(None, "It’s further down; scroll.", 1.0, "haiku"))
        self.assertEqual(result, "Couldn't see it in Settings: It's further down; scroll.")
        self.assertEqual(pipe.of("point"), [])
        self.assertIn("point: outcome=unseen", logs.output[-1])


class GateTurnTests(unittest.IsolatedAsyncioTestCase):
    async def test_one_yes_per_turn(self):
        pipe = ThreadPipe()
        gate = Gate(pipe, {"point_at": "ask", "read_file": "ask"}, 1)

        async def check(tool, answer=None, proactive=False):
            task = asyncio.create_task(gate.check(tool, "Can I?", proactive))
            await asyncio.sleep(0)
            if answer is not None:
                gate.answer(pipe.sent[-1]["id"], answer)
            return await task

        self.assertIn("declined", await check("point_at", False))
        self.assertIsNone(await check("point_at", True))
        asks = len(pipe.of("ask"))
        self.assertIsNone(await check("point_at"))               # granted, no bubble this time
        self.assertEqual(len(pipe.of("ask")), asks)
        self.assertIn("unprompted", await check("point_at", proactive=True))   # never in a remark
        self.assertIsNone(await check("read_file", True))
        self.assertIsNone(await check("read_file", True))        # others still ask every time
        self.assertEqual(len(pipe.of("ask")), asks + 2)
        gate.reset_turn()
        self.assertIn("declined", await check("point_at", False))   # a new turn asks again


class ToolTests(unittest.TestCase):
    def tools(self, d, pointer=None):
        ctx = ToolContext(Path(d), NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print),
                          pointer=pointer)
        return {t.name: t for t in build_tools(ctx)}

    def test_point_at_arguments(self):
        with tempfile.TemporaryDirectory() as d:
            tool = self.tools(d)["point_at"]
            schema = tool.schema()["input_schema"]
            self.assertEqual(schema["required"], ["target"])
            self.assertEqual(set(schema["properties"]), {"target", "say", "wait"})
            self.assertIsNotNone(tool.validate({}))
            self.assertIsNotNone(tool.validate({"target": "x", "wait": "yes"}))
            self.assertIsNotNone(tool.validate({"target": "x", "where": "here"}))
            self.assertIsNone(tool.validate({"target": "x", "say": "Click it", "wait": True}))
            with self.assertRaises(ToolError):
                tool.precheck({"target": "   "})
            with self.assertRaises(ToolError):
                tool.run({"target": "x"})                         # no pointer in this context
            self.assertIsNotNone(tool.ask)
            self.assertEqual(ask_text(lambda: WindowInfo(5, "Display - Settings", Rect(0, 0, 900, 700))),
                             "Can I look at your Settings window to find it?")
            self.assertEqual(ask_text(lambda: WindowInfo(0, "", Rect(0, 0, 900, 700))),
                             "Can I look at your screen to find it?")

    def test_old_permissions_file_gets_the_default(self):
        with tempfile.TemporaryDirectory() as d:
            paths = config.Paths(Path(d))
            paths.ensure()
            paths.permissions.write_text(json.dumps({"take_screenshot": "allow"}), encoding="utf-8")
            levels = config.load_permissions(paths)
            self.assertEqual(levels["point_at"], "ask")
            self.assertEqual(levels["take_screenshot"], "allow")


class FakeSettings:
    chattiness = "off"


class BrainRoutingTests(unittest.IsolatedAsyncioTestCase):
    async def test_point_done_and_cancel_reach_the_pointer(self):
        pipe = ThreadPipe()
        pointer = Pointer(pipe, settle_s=0, name_at=lambda x, y: "")
        brain = Brain(pipe, Gate(pipe, {}, 1), Initiative(FakeSettings()), pointer=pointer)
        with self.assertLogs("manikin.pointer", "INFO"):
            pointer.show(SPOT, False)
            brain.on_message({"type": "point_done", "id": 1, "outcome": "dismissed"})
            self.assertTrue(brain.inbox.empty())                 # bypasses the inbox
            pointer.show(SPOT, False)
            brain.cancel()
        self.assertEqual(pipe.of("point_clear"), [{"type": "point_clear", "id": 2}])

    async def test_echo_points_for_offline_testing(self):
        pipe = ThreadPipe()
        echo = EchoBackend(Pointer(pipe, settle_s=0, name_at=lambda x, y: ""))
        self.assertEqual(await echo.chat("point 1200 400"), "[happy] Pointing at 1200, 400.")
        self.assertEqual(pipe.of("point")[0], {"type": "point", "id": 1, "x": 1200, "y": 400,
                                               "box": [1170, 385, 60, 30], "window": 0, "text": "This one!"})
        task = asyncio.create_task(echo.chat("point -50 10 wait"))
        await asyncio.to_thread(pipe.wait_for, "point", 2)
        echo._pointer.on_done({"type": "point_done", "id": 2, "outcome": "clicked", "x": -50, "y": 10,
                               "inside": True})
        self.assertEqual(await task, "[happy] The user clicked it.")
        self.assertEqual(await echo.chat("pointless"), "pointless")
        echo._pointer.cancel()


class PointRoundsTests(unittest.IsolatedAsyncioTestCase):
    async def test_walkthrough_steps_have_their_own_allowance(self):
        os.environ.setdefault("ANTHROPIC_API_KEY", "placeholder-not-used")
        with tempfile.TemporaryDirectory() as d:
            pipe = ThreadPipe()
            gate = Gate(pipe, {"point_at": "allow", "get_time": "allow"}, 1)
            ctx = ToolContext(Path(d), NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print))
            backend = ClaudeBackend(pipe, gate, build_tools(ctx), "memory", ctx.chat)
            steps = config.MAX_TOOL_ROUNDS + 3
            fake = FakeClaude([_response("tool_use", _tool_call("point_at", target="x"))] * steps
                              + [_response("end_turn", _text("Done!"))])
            backend._client = NS(beta=NS(messages=fake))
            self.assertEqual(await backend.chat("walk me through it"), "Done!")   # point_at errors out, no pointer

            fake = FakeClaude([_response("tool_use", _tool_call("get_time"))] * (config.MAX_TOOL_ROUNDS + 1))
            backend._client = NS(beta=NS(messages=fake))
            self.assertIn("tangled", await backend.chat("what time is it, a lot"))
            self.assertEqual(len(fake.requests), config.MAX_TOOL_ROUNDS)


if __name__ == "__main__":
    unittest.main()
