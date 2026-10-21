"""offline driving tests, no API calls, no screen capture, no real input. the model is a
scripted fake client, the desktop a fake executor, and the Driver runs from a thread with
drive_done and cancel coming from this side (like test_pointer does for points)"""

import asyncio
import base64
import io
import os
import sys
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace as NS
from unittest import mock

BRAIN_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRAIN_DIR))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from manikin import config, drive                                 # noqa: E402
from manikin.app import Brain                                     # noqa: E402
from manikin.backend import PERSONA, EchoBackend                  # noqa: E402
from manikin.drive import (ASK_USER_TOOL, CARRY_ON, DRIVE_TOOL, ENABLED, HAND_OVER_TOOL,  # noqa: E402
                           NOT_AFTER_USER, NOT_EXECUTED, RESUMED, BadInput, DesktopExecutor, Driver,
                           DriveTimeout, blocking_confirm, chunks, drive_tool, map_key, parse_combo,
                           parse_keys, run_drive, to_actions)
from manikin.drive_types import Action, Decision, Facts, Shot     # noqa: E402
from manikin.gate import Gate                                     # noqa: E402
from manikin.initiative import Initiative                         # noqa: E402
from manikin.protocol import is_key_name                          # noqa: E402
from manikin.screen import Frame, Rect                            # noqa: E402
from manikin.tools import ToolError                               # noqa: E402
from test_pointer import FakeSettings, ThreadPipe, run_in_thread  # noqa: E402

FRAME = Frame(Rect(100, 50, 2560, 1600), 1280, 800)       # a 2560x1600 monitor seen at 1280 wide


def png(w=1280, h=800) -> bytes:
    from PIL import Image
    buf = io.BytesIO()
    Image.new("RGB", (w, h), (20, 40, 60)).save(buf, "PNG")
    return buf.getvalue()


PNG = png()


def desktop(ix, iy, frame=FRAME):
    x, y = frame.to_desktop(ix, iy)
    return round(x), round(y)


class FakeExecutor:
    """the desktop, scripted. run() answers from `outcomes` and after that with done"""

    def __init__(self, outcomes=(), facts=None, focus=None, handover=True, full=None, frames=()):
        self.outcomes = list(outcomes)
        self.facts = facts or {}                 # (x, y) -> Facts
        self.focus = focus
        self.handover_answer = handover
        self.full = full
        self.frames = list(frames)               # frames for the screenshots after the first
        self.ran, self.handed, self.shots = [], [], 0
        self.on_run = None

    def screenshot(self):
        frame = self.frames.pop(0) if self.shots and self.frames else FRAME
        self.shots += 1
        return Shot(PNG, "image/png", frame, full=self.full)

    def facts_at(self, x, y):
        return self.facts.get((x, y))

    def focused(self):
        return self.focus

    def run(self, action):
        self.ran.append(action)
        if self.on_run:
            self.on_run(action)
        return self.outcomes.pop(0) if self.outcomes else "done"

    def handover(self, action, decision):
        self.handed.append((action, decision))
        return self.handover_answer


class FakeClient:
    """client.beta.messages.create playing back scripted responses"""

    def __init__(self, *responses):
        self.responses = list(responses)
        self.requests = []
        self.beta = NS(messages=NS(create=self.create))

    def create(self, **kw):
        self.requests.append({**kw, "messages": list(kw["messages"])})   # as it was then
        item = self.responses.pop(0)
        if isinstance(item, Exception):
            raise item
        return item


def call(name, id_, **inp):
    return NS(type="tool_use", id=id_, name=name, input=inp, toolset_name="computer")


def text(t):
    return NS(type="text", text=t)


def resp(stop, *content, tokens=(100, 10, 0, 0)):
    i, o, read, write = tokens
    return NS(stop_reason=stop, content=list(content),
              usage=NS(input_tokens=i, output_tokens=o, cache_read_input_tokens=read,
                       cache_creation_input_tokens=write))


def end(line="[happy] All done!"):
    return resp("end_turn", text(line))


class Run:
    """one run_drive with fakes, keeps what it narrated and asked"""

    def __init__(self, client, executor=None, answers=(), **kw):
        self.client, self.executor = client, executor or FakeExecutor()
        self.narrated, self.asked, self.answers = [], [], list(answers)
        self.result = run_drive("Fill in the form: name Mani", ["Click Name", "Type it"],
                                self.executor, client, narrate=lambda t, m: self.narrated.append((t, m)),
                                ask=self.ask, **kw)

    def ask(self, question):
        self.asked.append(question)
        return self.answers.pop(0) if self.answers else False

    def results(self, request=1):
        """the tool_results sent in the given request (its last user message)"""
        return [b for b in self.client.requests[request]["messages"][-1]["content"]
                if b["type"] == "tool_result"]


class Go:
    """safety.check says go unless a test patches it, the real rules get tested in test_safety"""

    def setUp(self):
        super().setUp()
        patcher = mock.patch.object(drive.safety, "check", return_value=Decision("go"))
        patcher.start()
        self.addCleanup(patcher.stop)


def check_returning(*decisions):
    """safety.check patched to give these answers in turn (then go), recording what it saw"""
    seen, answers = [], list(decisions)

    def check(action, target, focused):
        seen.append((action, target, focused))
        return answers.pop(0) if answers else Decision("go")
    return mock.patch.object(drive.safety, "check", side_effect=check), seen


class RequestShapeTests(Go, unittest.TestCase):
    def test_the_toolset_entry(self):
        self.assertEqual(DRIVE_TOOL["type"], "computer_toolset_20260801")
        self.assertEqual(set(DRIVE_TOOL), {"type", "configs"})          # no name, no display size
        on = {m for m, c in DRIVE_TOOL["configs"].items() if c["enabled"]}
        off = {m for m, c in DRIVE_TOOL["configs"].items() if not c["enabled"]}
        self.assertEqual(on, {"screenshot", "zoom", "left_click", "right_click", "double_click",
                              "triple_click", "left_click_drag", "mouse_move", "scroll", "type", "key",
                              "wait"})
        self.assertEqual(off, {"middle_click", "left_mouse_down", "left_mouse_up", "hold_key",
                               "cursor_position"})
        self.assertEqual(on, set(ENABLED))

    def test_requests_and_results(self):
        thinking = NS(type="thinking", thinking="", signature="sig")
        first = resp("tool_use", thinking, text("[excited] Name field!"),
                     call("left_click", "a", coordinate=[640, 400]), call("type", "b", text="Mani"))
        run = Run(FakeClient(first, resp("tool_use", call("screenshot", "c")), end()))
        req = run.client.requests[0]
        self.assertEqual(req["model"], config.DRIVE_MODEL)
        self.assertEqual(req["output_config"], {"effort": config.DRIVE_EFFORT})
        self.assertEqual(req["tools"], [DRIVE_TOOL, ASK_USER_TOOL, HAND_OVER_TOOL])
        self.assertEqual(req["cache_control"], {"type": "ephemeral"})
        self.assertNotIn("thinking", req)
        self.assertIn("is you", req["system"])
        self.assertIn("never yours", req["system"])
        intro, image = req["messages"][0]["content"]
        self.assertIn("name Mani", intro["text"])
        self.assertIn("1. Click Name\n2. Type it", intro["text"])
        self.assertEqual(image["type"], "image")
        self.assertEqual(base64.b64decode(image["source"]["data"]), PNG)

        # the assistant turn goes back exactly as it came, thinking block and all
        self.assertIs(run.client.requests[1]["messages"][1]["content"], first.content)
        results = run.results(1)
        self.assertEqual([r["tool_use_id"] for r in results], ["a", "b"])
        for r in results:
            self.assertEqual(r["toolset_name"], "computer")
            self.assertEqual(r["content"], "OK")
            self.assertNotIn("is_error", r)
        shot = run.results(2)[0]
        self.assertEqual(shot["toolset_name"], "computer")
        self.assertEqual(shot["content"][0]["type"], "image")
        # append only, each request starts with the whole previous one
        self.assertEqual(run.client.requests[2]["messages"][:3], run.client.requests[1]["messages"])

        r = run.result
        self.assertEqual((r.result, r.steps, r.actions), ("done", 3, 2))
        self.assertEqual((r.input_tokens, r.output_tokens), (300, 30))
        self.assertIn("Drive finished (2 actions in 3 looks)", r.summary)
        self.assertIn("All done!", r.summary)
        self.assertEqual([a.kind for a in run.executor.ran], ["click", "type"])


class BatchTests(Go, unittest.TestCase):
    def test_in_order_and_nothing_after_the_first_failure(self):
        executor = FakeExecutor(outcomes=["done", "failed"])
        batch = resp("tool_use", call("left_click", "a", coordinate=[10, 10]), call("type", "b", text="hi"),
                     call("key", "c", text="Return"), call("screenshot", "d"))
        run = Run(FakeClient(batch, end()), executor)
        a, b, c, d = run.results(1)
        self.assertEqual(a["content"], "OK")
        self.assertTrue(b["is_error"])
        self.assertTrue(b["content"].startswith("Failed: "))
        self.assertIn("administrator", b["content"])
        for r in (c, d):
            self.assertEqual(r["content"], NOT_EXECUTED)
            self.assertTrue(r["is_error"])
            self.assertEqual(r["toolset_name"], "computer")
        self.assertEqual([x.kind for x in executor.ran], ["click", "type"])
        self.assertEqual(executor.shots, 1)                       # d never ran
        self.assertEqual(run.result.result, "done")                # the model carries on from there

    def test_the_executor_can_say_why_it_failed(self):
        executor = FakeExecutor(outcomes=["failed"])
        executor.failure_reason = lambda action: "your own chat box had the keyboard focus."
        run = Run(FakeClient(resp("tool_use", call("type", "a", text="hi")), end()), executor)
        self.assertEqual(run.results(1)[0]["content"], "Failed: your own chat box had the keyboard focus.")

    def test_bad_calls_are_errors_too(self):
        stranger = NS(type="tool_use", id="x", name="get_time", input={}, toolset_name=None)
        batch = resp("tool_use", call("key", "a", text="XF86AudioPlay"), call("left_click", "b", coordinate=[1, 1]))
        run = Run(FakeClient(batch, resp("tool_use", call("middle_click", "c", coordinate=[1, 1])),
                             resp("tool_use", stranger), resp("tool_use", call("left_click", "d")), end()))
        a, b = run.results(1)
        self.assertTrue(a["is_error"])
        self.assertIn("Unknown key 'XF86AudioPlay'", a["content"])
        self.assertEqual(b["content"], NOT_EXECUTED)
        self.assertIn("switched off", run.results(2)[0]["content"])
        self.assertNotIn("toolset_name", run.results(3)[0])        # not ours, answered but not echoed
        self.assertTrue(run.results(3)[0]["is_error"])
        self.assertIn("coordinate", run.results(4)[0]["content"])  # a click with nowhere to go
        self.assertEqual(run.executor.ran, [])

    def test_a_screenshot_mid_turn_doesnt_move_the_turns_coordinates(self):
        moved = Frame(Rect(-1920, 0, 1920, 1080), 1280, 720)      # the next look is another monitor
        executor = FakeExecutor(frames=[moved])
        run = Run(FakeClient(resp("tool_use", call("screenshot", "a"), call("left_click", "b", coordinate=[640, 400])),
                             resp("tool_use", call("left_click", "c", coordinate=[640, 400])), end()), executor)
        first, second = executor.ran
        self.assertEqual((first.x, first.y), desktop(640, 400))     # written against the old look
        self.assertEqual((second.x, second.y), desktop(640, 400, moved))


class MappingTests(Go, unittest.TestCase):
    def test_coordinates(self):
        click, = to_actions("left_click", {"coordinate": [640, 400]}, FRAME, None)
        self.assertEqual((click.kind, click.x, click.y, click.button, click.count), ("click", *desktop(640, 400), "left", 1))
        edge, = to_actions("left_click", {"coordinate": [1300, -4]}, FRAME, None)
        self.assertEqual((edge.x, edge.y), desktop(1279, 0))         # kept on the picture
        self.assertEqual(to_actions("left_click", {}, FRAME, (5, 6))[0].x, 5)   # no coordinate = where it is
        with self.assertRaises(BadInput):
            to_actions("left_click", {"coordinate": [True, 3]}, FRAME, None)

    def test_clicks_moves_and_drags(self):
        def one(name, **inp):
            action, = to_actions(name, inp, FRAME, None)
            return action
        self.assertEqual(one("right_click", coordinate=[1, 1]).button, "right")
        self.assertEqual(one("double_click", coordinate=[1, 1]).count, 2)
        self.assertEqual(one("triple_click", coordinate=[1, 1]).count, 3)
        self.assertEqual(one("left_click", coordinate=[1, 1], text="shift").mods, ("shift",))
        self.assertEqual(one("left_click", coordinate=[1, 1], text="ctrl+shift").mods, ("ctrl", "shift"))
        with self.assertRaises(BadInput):
            one("left_click", coordinate=[1, 1], text="a")
        move = one("mouse_move", coordinate=[100, 200])
        self.assertEqual((move.kind, move.x, move.y), ("move", *desktop(100, 200)))
        drag = one("left_click_drag", start_coordinate=[10, 20], coordinate=[300, 400])
        self.assertEqual((drag.kind, drag.x, drag.y, drag.to_x, drag.to_y),
                         ("drag", *desktop(10, 20), *desktop(300, 400)))
        self.assertEqual(drag.message()["do"], "drag")

    def test_scroll(self):
        def scroll(direction, amount=3):
            a, = to_actions("scroll", {"coordinate": [5, 5], "scroll_direction": direction,
                                       "scroll_amount": amount}, FRAME, None)
            return a.dx, a.dy
        self.assertEqual(scroll("down"), (0, 3))
        self.assertEqual(scroll("up", 2), (0, -2))
        self.assertEqual(scroll("left", 4), (-4, 0))
        self.assertEqual(scroll("right", 1), (1, 0))
        self.assertEqual(scroll("down", 500), (0, 25))
        for bad in [("sideways", 3), ("down", 0), ("down", "lots")]:
            with self.assertRaises(BadInput):
                scroll(*bad)

    def test_key_names(self):
        self.assertEqual(map_key("Return"), "enter")
        self.assertEqual(map_key("BackSpace"), "backspace")
        self.assertEqual(map_key("Page_Down"), "pagedown")
        self.assertEqual(map_key("Prior"), "pageup")
        self.assertEqual(map_key("KP_Enter"), "enter")
        self.assertEqual(map_key("A"), "A")
        self.assertEqual(map_key("comma"), ",")
        self.assertIsNone(map_key("XF86AudioPlay"))
        self.assertEqual(parse_combo("ctrl+shift+t"), ("ctrl", "shift", "t"))
        self.assertEqual(parse_combo("t+ctrl"), ("ctrl", "t"))          # modifiers first
        self.assertEqual(parse_combo("alt+F4"), ("alt", "f4"))
        self.assertEqual(parse_combo("Escape"), ("escape",))
        for name in ("super", "cmd", "win", "Super_L", "command"):
            self.assertEqual(parse_combo(f"{name}+e"), ("super", "e"))
        self.assertEqual(parse_combo("ctrl++"), ("ctrl", "+"))
        self.assertEqual(parse_combo("ctrl+plus"), ("ctrl", "+"))
        self.assertEqual(parse_combo("+"), ("+",))
        self.assertEqual(parse_keys("Tab Tab Tab"), [(("tab",), 3)])
        self.assertEqual(parse_keys("ctrl+a BackSpace"), [(("ctrl", "a"), 1), (("backspace",), 1)])
        self.assertEqual(parse_keys("ctrl + s"), [(("ctrl", "s"), 1)])
        for keys, _ in parse_keys("ctrl+shift+Page_Up Return space KP_5 F12 quotedbl"):
            self.assertTrue(all(is_key_name(k) for k in keys), keys)
        for bad in ("", "ctrl+", "Hyper_L", None):
            with self.assertRaises(BadInput):
                parse_keys(bad)

    def test_key_actions(self):
        tab, = to_actions("key", {"text": "Tab Tab"}, FRAME, None)
        self.assertEqual(tab.message(), {"do": "key", "keys": ["tab"], "repeat": 2})
        save, enter = to_actions("key", {"text": "ctrl+s Return"}, FRAME, None)
        self.assertEqual((save.keys, enter.keys), (("ctrl", "s"), ("enter",)))
        tab, = to_actions("key", {"text": "Tab", "repeat": 4}, FRAME, None)       # repeat given
        self.assertEqual((tab.keys, tab.repeat), (("tab",), 4))
        presses = to_actions("key", {"text": "Down Return", "repeat": 2}, FRAME, None)
        self.assertEqual([(a.keys, a.repeat) for a in presses],
                         [(("down",), 1), (("enter",), 1), (("down",), 1), (("enter",), 1)])
        self.assertEqual(to_actions("key", {"text": "ctrl+a", "repeat": None}, FRAME, None)[0].repeat, 1)
        for bad in (0, 101, 2.5, True):
            with self.assertRaises(BadInput):
                to_actions("key", {"text": "Tab", "repeat": bad}, FRAME, None)

    def test_long_text_is_typed_in_chunks(self):
        long = "".join(chr(ord("a") + i % 26) for i in range(1200))
        parts = to_actions("type", {"text": long}, FRAME, None)
        self.assertEqual([len(p.text) for p in parts], [500, 500, 200])
        self.assertEqual("".join(p.text for p in parts), long)
        self.assertEqual(chunks("ab\n", 2), ["ab", "\n"])
        patch, seen = check_returning()
        with patch:
            run = Run(FakeClient(resp("tool_use", call("type", "a", text=long)), end()))
        self.assertEqual(len(seen), 3)                               # every chunk gets checked
        self.assertEqual([len(a.text) for a in run.executor.ran], [500, 500, 200])
        self.assertEqual(run.results(1)[0]["content"], "OK")

    def test_tabs_and_line_breaks_are_their_own_presses(self):
        parts = to_actions("type", {"text": "Mani\tmani@example.com\r\nhi"}, FRAME, None)
        self.assertEqual([(a.kind, a.text or a.keys) for a in parts],
                         [("type", "Mani"), ("key", ("tab",)), ("type", "mani@example.com"),
                          ("type", "\n"), ("type", "hi")])
        name, email = Facts(name="Name", kind="Edit"), Facts(name="Email", kind="Edit")
        executor = FakeExecutor()
        focus = iter([name, name, email, email, email])
        executor.focused = lambda: next(focus)                     # the focus moves with the Tab
        patch, seen = check_returning()
        with patch:
            Run(FakeClient(resp("tool_use", call("type", "a", text="Mani\tmani@example.com\n")), end()), executor)
        self.assertEqual([(a.kind, a.text, f.name) for a, _, f in seen],
                         [("type", "Mani", "Name"), ("key", "", "Name"), ("type", "mani@example.com", "Email"),
                          ("type", "\n", "Email")])                 # judged as a typed newline...
        self.assertEqual([(a.kind, a.text or a.keys) for a in executor.ran],
                         [("type", "Mani"), ("key", ("tab",)), ("type", "mani@example.com"),
                          ("key", ("enter",))])                      # ...and pressed as Enter


class DecisionTests(Go, unittest.TestCase):
    SUBMIT = Facts(name="Submit", kind="Button")

    def click_run(self, decision, answers=(), executor=None, more=()):
        executor = executor or FakeExecutor(facts={desktop(640, 400): self.SUBMIT})
        patch, seen = check_returning(decision)
        with patch:
            run = Run(FakeClient(resp("tool_use", call("left_click", "a", coordinate=[640, 400]), *more), end()),
                      executor, answers)
        return run, seen

    def test_ask_yes_runs_it(self):
        run, seen = self.click_run(Decision("ask", "submit", "Press Submit?"), [True])
        self.assertEqual(run.asked, ["Press Submit?"])
        self.assertEqual(len(run.executor.ran), 1)
        self.assertEqual(run.results(1)[0]["content"], "OK")
        self.assertEqual(run.result.asked, ["submit"])
        self.assertIn("asked about submit (they said yes)", run.result.summary)
        action, target, focused = seen[0]
        self.assertEqual((action.kind, target), ("click", self.SUBMIT))   # the facts at the mapped point

    def test_ask_no_stops_the_drive(self):
        run, _ = self.click_run(Decision("ask", "submit", "Press Submit?"), [False],
                                more=[call("key", "b", text="Return")])
        self.assertEqual(run.executor.ran, [])
        self.assertEqual(len(run.client.requests), 1)                # no more looks
        self.assertEqual(run.result.result, "stopped")
        self.assertEqual(run.result.asked, ["submit"])
        self.assertIn("said no", run.result.summary)
        self.assertIn("asked about submit (they said no)", run.result.summary)

    def two_clicks(self, decisions, answers):
        patch, _ = check_returning(*decisions)
        clicks = [resp("tool_use", call("left_click", f"c{i}", coordinate=[640, 400])) for i in range(2)]
        with patch:
            return Run(FakeClient(*clicks, end()), FakeExecutor(), answers)

    def test_an_install_asks_once(self):
        install, finish = Decision("ask", "install", "Press Install?"), Decision("ask", "install", "Press Finish?")
        run = self.two_clicks([install, finish], [True])
        self.assertEqual(run.asked, ["Press Install?"])              # Finish just goes
        self.assertEqual(len(run.executor.ran), 2)
        self.assertEqual(run.result.asked, ["install"])
        submit = Decision("ask", "submit", "Press Submit?")
        run = self.two_clicks([submit, submit], [True, True])
        self.assertEqual(run.asked, ["Press Submit?", "Press Submit?"])   # everything else asks every time

    def test_an_ask_without_words_gets_a_question(self):
        run, _ = self.click_run(Decision("ask", "send"), [True])
        self.assertEqual(run.asked, ["Can I click 'Submit'?"])

    def test_handover_done_by_the_user(self):
        run, _ = self.click_run(Decision("handover", "password", "That's a password box: your turn."))
        self.assertEqual(len(run.executor.handed), 1)
        self.assertEqual(run.executor.ran, [])                      # never his
        result = run.results(1)[0]
        self.assertTrue(result["is_error"])
        self.assertIn("did it themselves", result["content"])
        self.assertEqual(run.result.handed_over, ["password"])
        self.assertEqual(run.result.result, "done")

    def test_handover_stop_ends_the_drive(self):
        executor = FakeExecutor(handover=False)
        run, _ = self.click_run(Decision("handover", "payment", "Paying is yours."), executor=executor)
        self.assertEqual(len(run.client.requests), 1)
        self.assertEqual(run.result.result, "stopped")
        self.assertIn("chose to stop", run.result.summary)
        self.assertEqual(run.result.handed_over, ["payment"])

    def test_an_unknown_verdict_is_a_handover(self):
        run, _ = self.click_run(Decision("maybe", "odd"))
        self.assertEqual(len(run.executor.handed), 1)
        self.assertEqual(run.executor.ran, [])

    def test_typing_is_judged_by_the_focus(self):
        field = Facts(name="Email", kind="Edit", editable=True)
        patch, seen = check_returning()
        with patch:
            Run(FakeClient(resp("tool_use", call("type", "a", text="x")), end()), FakeExecutor(focus=field))
        action, target, focused = seen[0]
        self.assertEqual((action.kind, target, focused), ("type", None, field))

    def test_a_drag_is_judged_at_both_ends(self):
        start, drop = desktop(10, 10), desktop(500, 500)
        executor = FakeExecutor(facts={start: Facts(name="notes.txt", kind="ListItem"),
                                       drop: Facts(name="Recycle Bin", kind="ListItem")})

        def check(action, target, focused):
            if target is not None and target.name == "Recycle Bin":
                return Decision("handover", "delete", "Binning things is yours.")
            return Decision("go")
        with mock.patch.object(drive.safety, "check", side_effect=check):
            run = Run(FakeClient(resp("tool_use", call("left_click_drag", "a", start_coordinate=[10, 10],
                                                       coordinate=[500, 500])), end()), executor)
        self.assertEqual(run.executor.ran, [])
        self.assertEqual(run.result.handed_over, ["delete"])


def own(name, id_, **inp):
    """a call to one of his own tools, plain tool_use with no toolset_name"""
    return NS(type="tool_use", id=id_, name=name, input=inp)


class ModelToolsTests(Go, unittest.TestCase):
    """ask_user and hand_over, the models own ways to bring the user in mid drive"""

    def test_offered_and_explained(self):
        self.assertEqual([ASK_USER_TOOL["name"], HAND_OVER_TOOL["name"]], ["ask_user", "hand_over"])
        self.assertEqual(ASK_USER_TOOL["input_schema"]["required"], ["question"])
        self.assertEqual(HAND_OVER_TOOL["input_schema"]["properties"]["kind"]["enum"],
                         ["password", "payment", "delete", "other"])
        self.assertEqual(set(HAND_OVER_TOOL["input_schema"]["required"]), {"coordinate", "say", "kind"})
        for line in ("call hand_over", "ask_user only for a real decision", "never end it with a question",
                     "including accepting a license or terms"):
            self.assertIn(line, " ".join(drive.DRIVE_SYSTEM.split()))

    def test_ask_user_yes_carries_on(self):
        batch = resp("tool_use", own("ask_user", "q", question="Use the  work\nemail?"),
                     call("left_click", "a", coordinate=[1, 1]))
        run = Run(FakeClient(batch, end()), answers=[True])
        q, a = run.results(1)
        self.assertEqual(q, {"type": "tool_result", "tool_use_id": "q", "content": "The user said yes."})
        self.assertEqual((a["content"], a["toolset_name"]), ("OK", "computer"))
        self.assertEqual(run.asked, ["Use the work email?"])
        self.assertEqual(len(run.executor.ran), 1)
        self.assertEqual((run.result.result, run.result.asked), ("done", ["model"]))
        self.assertIn('asked the user "Use the work email?" (they said yes)', run.result.summary)

    def test_ask_user_no_lets_the_model_decide(self):
        batch = resp("tool_use", own("ask_user", "q", question="Tick the newsletter box?"),
                     call("left_click", "a", coordinate=[1, 1]))
        run = Run(FakeClient(batch, end("[happy] Left the newsletter off.")), answers=[False])
        q, a = run.results(1)
        self.assertEqual(q["content"], "The user said no.")
        self.assertNotIn("is_error", q)
        self.assertEqual((a["content"], a["is_error"]), (NOT_AFTER_USER, True))   # the click it planned
        self.assertEqual(run.executor.ran, [])
        self.assertEqual(len(run.client.requests), 2)                # not a stop, the model keeps going
        self.assertEqual((run.result.result, run.result.asked), ("done", ["model"]))

    def test_hand_over_done_by_the_user(self):
        executor = FakeExecutor(handover=True)
        batch = resp("tool_use", own("hand_over", "h", coordinate=[640, 400], say="Your password goes here!",
                                     kind="password"), call("key", "k", text="Tab"))
        run = Run(FakeClient(batch, resp("tool_use", call("left_click", "c", coordinate=[2, 2])), end()),
                  executor)
        action, decision = executor.handed[0]
        self.assertEqual((action.kind, action.x, action.y), ("click", *desktop(640, 400)))   # through the frame
        self.assertEqual(decision, Decision("handover", "password", "Your password goes here!"))
        h, k = run.results(1)
        self.assertIn("The user did it.", h["content"])
        self.assertNotIn("is_error", h)
        self.assertEqual(k["content"], NOT_AFTER_USER)
        self.assertEqual([a.kind for a in executor.ran], ["click"])   # only the next turns
        self.assertEqual((run.result.result, run.result.handed_over), ("done", ["password"]))
        self.assertIn("handed password to the user (they did it)", run.result.summary)

    def test_a_declined_hand_over_lets_the_model_decide(self):
        executor = FakeExecutor(handover=False)
        run = Run(FakeClient(resp("tool_use", own("hand_over", "h", coordinate=[10, 10], say=" ", kind="payment")),
                             end("[sad] You'll pay later.")), executor)
        self.assertEqual(run.results(1)[0]["content"], "The user didn't do it.")
        self.assertEqual(executor.handed[0][1].reason, "Your turn!")   # no line given so the default
        self.assertEqual(len(run.client.requests), 2)
        self.assertEqual((run.result.result, run.result.handed_over), ("done", ["payment"]))

    def test_bad_calls_are_errors(self):
        run = Run(FakeClient(
            resp("tool_use", own("hand_over", "h", coordinate=[1], say="x", kind="password"),
                 call("left_click", "a", coordinate=[1, 1])),
            resp("tool_use", own("hand_over", "h2", coordinate=[1, 1], say="x", kind="secret")),
            resp("tool_use", own("ask_user", "q", question="  ")), end()))
        h, a = run.results(1)
        self.assertTrue(h["is_error"])
        self.assertIn("coordinate", h["content"])
        self.assertEqual(a["content"], NOT_EXECUTED)
        self.assertIn("kind must be one of", run.results(2)[0]["content"])
        self.assertIn("needs a question", run.results(3)[0]["content"])
        self.assertEqual((run.executor.handed, run.asked, run.result.asked), ([], [], []))

    def test_the_safety_rules_still_judge_what_he_does(self):
        patch, _ = check_returning(Decision("ask", "submit", "Press Submit?"))
        batch = resp("tool_use", own("ask_user", "q", question="Submit with the old address?"),
                     call("left_click", "a", coordinate=[1, 1]))
        with patch:
            run = Run(FakeClient(batch, end()), answers=[True, True])
        self.assertEqual(run.asked, ["Submit with the old address?", "Press Submit?"])   # his question doesnt cover the safety ask
        self.assertEqual(run.result.asked, ["model", "submit"])


class GrabTests(Go, unittest.TestCase):
    def batch(self):
        return resp("tool_use", call("left_click", "a", coordinate=[10, 10]), call("type", "b", text="Mani"))

    def test_carry_on_resumes_with_a_note_and_a_fresh_look(self):
        executor = FakeExecutor(outcomes=["grabbed"])
        run = Run(FakeClient(self.batch(), end()), executor, answers=[True])
        self.assertEqual(run.asked, [CARRY_ON])
        content = run.client.requests[1]["messages"][-1]["content"]
        self.assertEqual([b["type"] for b in content], ["tool_result", "tool_result", "text", "image"])
        self.assertTrue(content[0]["is_error"])
        self.assertIn("grabbed the mouse", content[0]["content"])
        self.assertEqual(content[1]["content"], NOT_EXECUTED)
        self.assertEqual(content[2]["text"], RESUMED)               # after the results, not in one
        self.assertEqual(executor.shots, 2)
        self.assertEqual(run.result.result, "done")

    def test_no_stops(self):
        run = Run(FakeClient(self.batch(), end()), FakeExecutor(outcomes=["grabbed"]), answers=[False])
        self.assertEqual(len(run.client.requests), 1)
        self.assertEqual(run.result.result, "stopped")
        self.assertIn("took the mouse", run.result.summary)


class StopTests(Go, unittest.TestCase):
    def test_cancelled_between_actions(self):
        flag = threading.Event()
        executor = FakeExecutor()
        executor.on_run = lambda action: flag.set()
        batch = resp("tool_use", call("left_click", "a", coordinate=[1, 1]), call("type", "b", text="x"))
        run = Run(FakeClient(batch, end()), executor, cancelled=flag.is_set)
        self.assertEqual(len(executor.ran), 1)
        self.assertEqual(len(run.client.requests), 1)
        self.assertEqual(run.result.result, "stopped")
        self.assertIn("Esc", run.result.summary)

    def test_cancelled_by_the_overlay(self):
        run = Run(FakeClient(resp("tool_use", call("left_click", "a", coordinate=[1, 1])), end()),
                  FakeExecutor(outcomes=["cancelled"]))
        self.assertEqual(run.result.result, "stopped")
        self.assertEqual(len(run.client.requests), 1)

    def test_cancelled_before_it_starts(self):
        run = Run(FakeClient(end()), cancelled=lambda: True)
        self.assertEqual(run.client.requests, [])
        self.assertEqual(run.result.result, "stopped")

    def test_step_cap(self):
        looks = [resp("tool_use", call("screenshot", f"s{i}")) for i in range(5)]
        run = Run(FakeClient(*looks), max_steps=3)
        self.assertEqual(len(run.client.requests), 3)
        self.assertEqual((run.result.result, run.result.steps), ("stopped", 3))
        self.assertIn("limit of 3 looks", run.result.summary)

    def test_token_budget(self):
        looks = [resp("tool_use", call("screenshot", f"s{i}"), tokens=(200, 50, 600, 150)) for i in range(5)]
        run = Run(FakeClient(*looks), budget=2000)
        self.assertEqual(len(run.client.requests), 2)               # 1000 a look, stops at 2000
        r = run.result
        self.assertEqual((r.input_tokens, r.output_tokens, r.cache_read_tokens, r.cache_write_tokens),
                         (400, 100, 1200, 300))
        self.assertEqual(r.result, "stopped")
        self.assertIn("budget", r.summary)

    def test_api_failure_and_refusal(self):
        with self.assertLogs("manikin.drive", "ERROR"):
            run = Run(FakeClient(ConnectionError("offline")))
        self.assertEqual(run.result.result, "failed")
        self.assertIn("couldn't be reached (ConnectionError)", run.result.summary)
        run = Run(FakeClient(resp("refusal")))
        self.assertEqual(run.result.result, "failed")

    def test_the_model_finishing_is_done_whatever_it_says(self):
        run = Run(FakeClient(end("[sad] I couldn't find the form.")))
        self.assertEqual(run.result.result, "done")                 # its words tell the chat
        self.assertIn("I couldn't find the form.", run.result.summary)
        run = Run(FakeClient(resp("max_tokens")))
        self.assertEqual(run.result.result, "failed")

    def test_the_overlay_never_answering_ends_it(self):
        executor = FakeExecutor()
        executor.run = mock.Mock(side_effect=DriveTimeout("the overlay didn't answer action 1 (click)"))
        with self.assertLogs("manikin.drive", "ERROR"):
            run = Run(FakeClient(resp("tool_use", call("left_click", "a", coordinate=[1, 1])), end()), executor)
        self.assertEqual(run.result.result, "failed")
        self.assertIn("didn't answer", run.result.summary)

    def test_wait_is_short_and_cancellable(self):
        run = Run(FakeClient(resp("tool_use", call("wait", "a", duration=0.05)), end()))
        self.assertEqual(run.results(1)[0]["content"], "OK")
        self.assertLess(drive.MAX_WAIT_S, 10)
        with mock.patch.object(drive, "MAX_WAIT_S", 0.05):
            run = Run(FakeClient(resp("tool_use", call("wait", "a", duration=60)), end()))
        self.assertEqual(run.results(1)[0]["content"], "OK")         # capped, not a minute
        looks = []                                                   # Esc on the third look
        run = Run(FakeClient(resp("tool_use", call("wait", "a", duration=3)), end()),
                  cancelled=lambda: looks.append(1) or len(looks) > 2)
        self.assertEqual(run.result.result, "stopped")               # Esc cuts it short
        self.assertEqual(len(run.client.requests), 1)
        self.assertIn("Esc", run.result.summary)

    def test_zoom_crops_the_full_capture(self):
        from PIL import Image
        full = Image.new("RGB", (2560, 1600), (0, 0, 0))
        zoom = resp("tool_use", call("zoom", "a", region=[0, 0, 640, 400]),
                    call("left_click", "b", coordinate=[640, 400]))
        for capture, size in ((full, (1280, 800)), (None, (640, 400))):
            run = Run(FakeClient(zoom, end()), FakeExecutor(full=capture))
            image = run.results(1)[0]["content"][0]
            got = Image.open(io.BytesIO(base64.b64decode(image["source"]["data"])))
            self.assertEqual(got.size, size)                        # sharper with the full capture
            click = run.executor.ran[0]
            self.assertEqual((click.x, click.y), desktop(640, 400))  # still the screenshots px
        run = Run(FakeClient(resp("tool_use", call("zoom", "a", region=[5, 5])), end()))
        self.assertIn("region", run.results(1)[0]["content"])


class NarrationTests(Go, unittest.TestCase):
    def test_text_blocks_become_bubble_lines(self):
        batch = resp("tool_use", text("[excited] Name field!"), call("left_click", "a", coordinate=[1, 1]),
                     text("Tabbing on"), call("key", "b", text="Tab"))
        run = Run(FakeClient(batch, end("[happy] Filled in.")))
        self.assertEqual(run.narrated, [("Name field!", "excited"), ("Tabbing on", None)])

    def test_nothing_after_a_failure(self):
        batch = resp("tool_use", call("left_click", "a", coordinate=[1, 1]), text("[smug] Nailed it"),
                     call("key", "b", text="Tab"))
        run = Run(FakeClient(batch, end()), FakeExecutor(outcomes=["failed"]))
        self.assertEqual(run.narrated, [])


def run_action(driver, action, epoch=None):
    return run_in_thread(lambda: driver.run(action, epoch))


class DriverTests(unittest.TestCase):
    CLICK = Action("click", 1210, 388)

    def test_one_outcome_per_id(self):
        pipe = ThreadPipe()
        driver = Driver(pipe)
        with self.assertLogs("manikin.drive", "INFO") as logs:
            t, out = run_action(driver, self.CLICK, driver.start())
            sent = pipe.wait_for("drive")
            self.assertEqual(sent, {"type": "drive", "id": 1, "do": "click", "x": 1210, "y": 388,
                                    "button": "left", "count": 1, "mods": []})
            driver.on_done({"type": "drive_done", "id": 2, "outcome": "done"})       # not this one
            driver.on_done({"type": "drive_done", "id": "1", "outcome": "done"})     # malformed
            driver.on_done({"type": "drive_done", "id": 1, "outcome": "exploded"})   # malformed
            self.assertTrue(t.is_alive())
            driver.on_done({"type": "drive_done", "id": 1, "outcome": "grabbed", "x": 1300, "y": 400})
            t.join(2)
            driver.on_done({"type": "drive_done", "id": 1, "outcome": "done"})       # late duplicate
        self.assertEqual(out["value"], "grabbed")
        self.assertEqual(driver.position, (1300, 400))
        self.assertEqual(len([l for l in logs.output if "outcome=" in l]), 1)
        self.assertIn("drive: id=1 do=click at=1210,388 outcome=grabbed", [l for l in logs.output if "outcome=" in l][0])

    def test_timeout(self):
        pipe = ThreadPipe()
        driver = Driver(pipe, timeout_s=0.05)
        with self.assertLogs("manikin.drive", "INFO") as logs:
            with self.assertRaises(DriveTimeout):
                driver.run(self.CLICK)
            driver.on_done({"type": "drive_done", "id": 1, "outcome": "done"})       # too late
        self.assertIn("outcome=timeout", logs.output[0])
        self.assertTrue(any("late done ignored" in l for l in logs.output))
        self.assertAlmostEqual(Driver(pipe, timeout_s=10).timeout_for(Action("type", text="x" * 500)), 60)

    def test_cancel(self):
        pipe = ThreadPipe()
        driver = Driver(pipe)
        epoch = driver.start()
        t, out = run_action(driver, self.CLICK, epoch)
        pipe.wait_for("drive")
        driver.cancel()
        t.join(2)
        self.assertEqual(out["value"], "cancelled")
        self.assertEqual(pipe.of("drive_end"), [{"type": "drive_end", "result": "stopped"}])
        self.assertEqual(driver.run(self.CLICK, epoch), "cancelled")             # stale, nothing sent
        self.assertEqual(len(pipe.of("drive")), 1)
        driver.cancel()
        driver.end("done")
        self.assertEqual(len(pipe.of("drive_end")), 1)                         # one end per drive

    def test_narrate(self):
        pipe = ThreadPipe()
        driver = Driver(pipe)
        driver.narrate("Name… field", "excited")
        driver.narrate("hm", "grumpy")
        driver.narrate("   ")
        driver.narrate("x" * 200)
        sent = pipe.of("narrate")
        self.assertEqual(sent[:2], [{"type": "narrate", "text": "Name... field", "mood": "excited"},
                                    {"type": "narrate", "text": "hm", "mood": None}])
        self.assertEqual(len(sent), 3)
        self.assertLessEqual(len(sent[2]["text"]), drive.NARRATE_MAX)


class FakeGate:
    def __init__(self, answer=True):
        self.answer, self.asked = answer, []

    async def confirm(self, text):
        self.asked.append(text)
        return self.answer


class DesktopExecutorTests(unittest.TestCase):
    def make(self, confirm=None):
        pipe = ThreadPipe()
        driver = Driver(pipe)
        return pipe, driver, DesktopExecutor(driver, None, confirm, epoch=driver.start(),
                                             lookup=lambda x, y: None, focus=lambda: None, settle_s=0)

    def test_runs_through_the_driver_with_its_epoch(self):
        pipe, driver, executor = self.make()
        t, out = run_in_thread(executor.run, Action("key", keys=("ctrl", "s")))
        sent = pipe.wait_for("drive")
        self.assertEqual(sent["keys"], ["ctrl", "s"])
        driver.on_done({"type": "drive_done", "id": sent["id"], "outcome": "done", "x": 5, "y": 6})
        t.join(2)
        self.assertEqual(out["value"], "done")
        driver.cancel()
        self.assertEqual(executor.run(Action("type", text="x")), "cancelled")   # nothing sent after Esc
        self.assertEqual(len(pipe.of("drive")), 1)

    def test_handover_without_a_pointer_still_asks(self):
        asked = []
        _, _, executor = self.make(lambda text: asked.append(text) or True)
        self.assertTrue(executor.handover(Action("type", text="hunter2"), Decision("handover", "password")))
        self.assertEqual(asked, ["Done?"])

    def test_handover_points_then_asks(self):
        from manikin.pointer import Pointer, Snap
        for outcome, asked_after in (("clicked", ["Done?"]), ("dismissed", ["Done?"]), ("cancelled", [])):
            pipe, asked = ThreadPipe(), []
            pointer = Pointer(pipe, settle_s=0, name_at=lambda x, y: "")
            driver = Driver(pipe)
            executor = DesktopExecutor(driver, pointer, lambda text: asked.append(text) or True,
                                       epoch=driver.start(), lookup=lambda x, y: None, focus=lambda: None)
            decision = Decision("handover", "password", "That's a password box. Your turn!")
            with mock.patch.object(drive, "snap", return_value=Snap(Rect(600, 380, 200, 40), "Password", "Edit")), \
                    self.assertLogs("manikin.pointer", "INFO"):
                t, out = run_in_thread(executor.handover, Action("click", 640, 400), decision)
                point = pipe.wait_for("point")
                self.assertEqual(asked, [])                         # nothing asked while he points
                if outcome == "cancelled":
                    pointer.cancel()                                 # Esc
                else:
                    pointer.on_done({"type": "point_done", "id": point["id"], "outcome": outcome,
                                     "x": 650, "y": 400, "inside": True})
                t.join(2)
            self.assertEqual((point["x"], point["y"], point["box"], point["window"], point["text"]),
                             (640, 400, [600, 380, 200, 40], 0, "That's a password box. Your turn!"))
            self.assertEqual(asked, asked_after, outcome)
            self.assertEqual(out["value"], outcome != "cancelled")

    def test_a_typing_handover_points_where_the_pointer_is(self):
        from manikin.pointer import Pointer, Snap
        pipe = ThreadPipe()
        pointer = Pointer(pipe, settle_s=0, name_at=lambda x, y: "")
        driver = Driver(pipe)
        driver.position = (900, 300)                                 # from the last drive_done
        executor = DesktopExecutor(driver, pointer, lambda text: False, epoch=driver.start(),
                                   lookup=lambda x, y: None, focus=lambda: None)
        with mock.patch.object(drive, "snap", return_value=Snap(Rect(880, 290, 40, 20))), \
                self.assertLogs("manikin.pointer", "INFO"):
            t, out = run_in_thread(executor.handover, Action("type", text="hunter2"), Decision("handover"))
            point = pipe.wait_for("point")
            pointer.on_done({"type": "point_done", "id": point["id"], "outcome": "clicked", "x": 900, "y": 300,
                             "inside": True})
            t.join(2)
        self.assertEqual((point["x"], point["y"], point["text"]), (900, 300, "Your turn!"))
        self.assertFalse(out["value"])                               # they clicked no on "Done?"

    def test_failure_reason_for_his_own_window(self):
        _, _, executor = self.make()
        with mock.patch.object(drive, "foreground_pid", return_value=os.getpid()):
            self.assertIn("own chat box", executor.failure_reason(Action("type", text="x")))


class FakeControl:
    """a UI Automation element the way uiautomation shows it"""

    def __init__(self, kind, name="", parent=None, pid=999_999, password=False, value=None, local="",
                 cls="", aria=""):
        self.ControlTypeName, self.Name, self.parent = f"{kind}Control", name, parent
        self.ProcessId, self.IsPassword, self.value = pid, password, value
        self.LocalizedControlType, self.ClassName, self.AriaProperties = local, cls, aria
        self.NativeWindowHandle = 0

    def GetParentControl(self):
        return self.parent

    def GetPattern(self, pattern_id):
        return None if self.value is None else NS(IsReadOnly=self.value == "read-only")


class FactsTests(unittest.TestCase):
    AUTO = NS(PatternId=NS(ValuePattern=10002))

    def facts(self, control, snap_up=True):
        from manikin import pointer
        with mock.patch.object(pointer, "_process_name", return_value="firefox.exe"):
            return pointer._facts(self.AUTO, control, snap_up)

    def test_process_names_are_lowercase(self):
        from manikin import pointer
        name = pointer._process_name(os.getpid())
        self.assertTrue(name.endswith(".exe"), name)
        self.assertEqual(name, name.lower())
        self.assertEqual(pointer._process_name(-5), "")

    def page(self):
        desktop_root = FakeControl("Pane", "Desktop 1")
        window = FakeControl("Window", "Sign up - Mozilla Firefox", desktop_root, cls="MozillaWindowClass")
        return FakeControl("Document", "Sign up", window)

    def test_a_button_on_a_web_page(self):
        button = FakeControl("Button", "Continue", FakeControl("Group", "", self.page()))
        label = FakeControl("Text", "Continue", button)
        f = self.facts(label)
        self.assertEqual((f.name, f.kind, f.in_form, f.editable), ("Continue", "Button", True, False))
        self.assertEqual((f.window_title, f.window_class, f.process),
                         ("Sign up - Mozilla Firefox", "MozillaWindowClass", "firefox.exe"))
        self.assertEqual(f.ancestors, ("Sign up", "Sign up - Mozilla Firefox"))   # not the desktop
        self.assertEqual(self.facts(label, snap_up=False).kind, "Text")          # the focus is what it is

    def test_fields(self):
        password = self.facts(FakeControl("Edit", "Password", self.page(), password=True, value="rw"))
        self.assertTrue(password.password and password.editable and password.in_form)
        self.assertFalse(password.multiline)
        self.assertTrue(self.facts(FakeControl("Edit", "Message", self.page(), aria="multiline=true")).multiline)
        self.assertTrue(self.facts(FakeControl("Edit", "x", self.page())).editable)        # an Edit types
        self.assertFalse(self.facts(FakeControl("Edit", "x", self.page(), value="read-only")).editable)
        self.assertTrue(self.facts(FakeControl("ComboBox", "To", self.page(), value="rw")).editable)
        app = FakeControl("Edit", "Name", FakeControl("Window", "Setup", FakeControl("Pane", "Desktop")))
        self.assertFalse(self.facts(app).in_form)                                # not on a page
        self.assertTrue(self.facts(FakeControl("Edit", "Name", FakeControl("Group", "", app, local="form"))).in_form)

    def test_his_own_window_tells_nothing(self):
        self.assertIsNone(self.facts(FakeControl("Pane", "Manikin", pid=os.getpid())))
        self.assertIsNone(self.facts(None))


class DriveToolTests(unittest.IsolatedAsyncioTestCase):
    async def test_the_tool_end_to_end(self):
        pipe = ThreadPipe()
        driver = Driver(pipe, loop=asyncio.get_running_loop())
        gate = FakeGate(answer=True)
        executor = FakeExecutor()
        ctx = NS(driver=driver, gate=gate, pointer=None)
        client = FakeClient(resp("tool_use", text("Clicking!"), call("left_click", "a", coordinate=[1, 1])), end())
        ask = Decision("ask", "submit", "Press Submit?")
        with mock.patch.object(drive, "default_client", return_value=client), \
                mock.patch.object(drive, "DesktopExecutor", return_value=executor), \
                mock.patch.object(drive.safety, "check", return_value=ask):
            summary = await asyncio.to_thread(drive_tool, ctx, {"task": "press submit", "plan": [" Press it "]})
        self.assertIn("Drive finished", summary)
        self.assertEqual(gate.asked, ["Press Submit?"])               # through the event loop
        self.assertEqual(pipe.of("narrate"), [{"type": "narrate", "text": "Clicking!", "mood": None}])
        self.assertEqual(pipe.of("drive_end"), [{"type": "drive_end", "result": "done"}])
        self.assertIn("1. Press it\n", client.requests[0]["messages"][0]["content"][0]["text"])

    async def test_a_crash_still_lets_go(self):
        pipe = ThreadPipe()
        driver = Driver(pipe, loop=asyncio.get_running_loop())
        ctx = NS(driver=driver, gate=FakeGate(), pointer=None)
        with mock.patch.object(drive, "default_client", side_effect=RuntimeError("no key")):
            with self.assertLogs("manikin.drive", "ERROR"):
                summary = await asyncio.to_thread(drive_tool, ctx, {"task": "x", "plan": ["y"]})
        self.assertIn("failed", summary)
        self.assertEqual(pipe.of("drive_end"), [{"type": "drive_end", "result": "failed"}])

    async def test_needs_a_driver(self):
        with self.assertRaises(ToolError):
            drive_tool(NS(driver=None, gate=None, pointer=None), {"task": "x", "plan": ["y"]})

    async def test_blocking_confirm(self):
        loop = asyncio.get_running_loop()
        self.assertTrue(await asyncio.to_thread(blocking_confirm(FakeGate(True), loop), "Done?"))
        self.assertFalse(await asyncio.to_thread(blocking_confirm(FakeGate(False), loop), "Done?"))
        with self.assertLogs("manikin.drive", "WARNING"):
            self.assertFalse(blocking_confirm(None, loop)("Done?"))

        class BrokenGate:
            async def confirm(self, text):
                raise RuntimeError("no overlay")
        with self.assertLogs("manikin.drive", "ERROR"):              # a broken ask is a no
            self.assertFalse(await asyncio.to_thread(blocking_confirm(BrokenGate(), loop), "?"))


class AnsweringPipe(ThreadPipe):
    """the overlay, answering every "drive" right away (from another thread)"""

    def __init__(self):
        super().__init__()
        self.driver = None
        self.outcomes = {}               # id -> outcome, else done

    def send(self, type_, **fields):
        super().send(type_, **fields)
        if type_ == "drive" and self.driver is not None:
            msg = {"type": "drive_done", "id": fields["id"], "outcome": self.outcomes.get(fields["id"], "done"),
                   "x": fields.get("x", 0), "y": fields.get("y", 0)}
            threading.Thread(target=self.driver.on_done, args=(msg,), daemon=True).start()


class EchoDriveTests(Go, unittest.IsolatedAsyncioTestCase):
    def make(self):
        pipe = AnsweringPipe()
        pipe.driver = Driver(pipe)
        echo = EchoBackend(driver=pipe.driver, make_executor=lambda d, p, confirm, epoch: DesktopExecutor(
            d, p, confirm, epoch=epoch, lookup=lambda x, y: None, focus=lambda: None, settle_s=0))
        return pipe, echo

    def drives(self, pipe):
        return [{k: v for k, v in m.items() if k not in ("type", "id")} for m in pipe.of("drive")]

    async def test_single_commands(self):
        pipe, echo = self.make()
        self.assertEqual(await echo.chat("drive click 1200 400"), "[happy] Done: clicked 1200, 400.")
        self.assertEqual(await echo.chat("drive type Hello there"), "[happy] Done: typed 11 characters.")
        self.assertEqual(await echo.chat("drive key ctrl+s"), "[happy] Done: pressed ctrl+s.")
        self.assertEqual(await echo.chat("drive scroll 10 20 -3"), "[happy] Done: scrolled -3 at 10, 20.")
        self.assertEqual(self.drives(pipe), [
            {"do": "click", "x": 1200, "y": 400, "button": "left", "count": 1, "mods": []},
            {"do": "type", "text": "Hello there"},
            {"do": "key", "keys": ["ctrl", "s"], "repeat": 1},
            {"do": "scroll", "x": 10, "y": 20, "dx": 0, "dy": -3}])
        self.assertEqual([m["id"] for m in pipe.of("drive")], [1, 2, 3, 4])
        # each one is a whole drive, let go of before the reply since a "say" ends the overlays drive
        self.assertEqual([m["type"] for m in pipe.sent if m["type"] != "narrate"],
                         ["drive", "drive_end"] * 4)
        self.assertEqual({m["result"] for m in pipe.of("drive_end")}, {"done"})
        self.assertIn("all yours", await echo.chat("drive end"))
        self.assertEqual(len(pipe.of("drive_end")), 4)                 # nothing left to let go of
        self.assertIn("usage: drive click X Y", await echo.chat("drive click here"))
        self.assertIn("Unknown key", await echo.chat("drive key Hyper_L"))
        self.assertEqual(len(pipe.of("drive")), 4)
        self.assertEqual(await echo.chat("drive me home"), "drive me home")   # not a command

    async def test_demo(self):
        pipe, echo = self.make()
        self.assertEqual(await echo.chat("drive demo 300 200"), "[happy] All filled in!")
        self.assertEqual(self.drives(pipe), [
            {"do": "click", "x": 300, "y": 200, "button": "left", "count": 1, "mods": []},
            {"do": "type", "text": "Mani"},
            {"do": "key", "keys": ["tab"], "repeat": 1},
            {"do": "type", "text": "mani@example.com"},
            {"do": "key", "keys": ["tab"], "repeat": 1},
            {"do": "type", "text": "1 Pixel Lane"}])
        self.assertEqual([(m["text"], m["mood"]) for m in pipe.of("narrate")],
                         [("Name first!", "excited"), ("Email next...", None), ("And the address!", None)])
        self.assertEqual(pipe.of("drive_end"), [{"type": "drive_end", "result": "done"}])

    async def test_demo_grabbed_lets_go(self):
        pipe, echo = self.make()                                      # no gate so "carry on?" is a no
        pipe.outcomes[2] = "grabbed"
        reply = await echo.chat("drive demo 300 200")
        self.assertIn("all yours", reply)
        self.assertNotIn("Interrupted", reply)                        # the models wording stays inside
        self.assertEqual(len(pipe.of("drive")), 2)
        self.assertEqual(pipe.of("drive_end"), [{"type": "drive_end", "result": "stopped"}])

    async def test_demo_grabbed_carries_on(self):
        class YesGate:
            asked: list[str] = []

            async def confirm(self, text):
                self.asked.append(text)
                return True

        pipe, echo = self.make()
        gate = YesGate()
        echo._gate = gate
        pipe.outcomes[2] = "grabbed"
        self.assertEqual(await echo.chat("drive demo 300 200"), "[happy] All filled in!")
        self.assertEqual(gate.asked, [drive.CARRY_ON])
        self.assertEqual(len(pipe.of("drive")), 6)                    # the interrupted step doesnt get redone
        self.assertEqual(pipe.of("drive_end"), [{"type": "drive_end", "result": "done"}])

    async def test_the_safety_rules_still_apply(self):
        pipe, echo = self.make()                                      # no gate so every ask is a no
        with mock.patch.object(drive.safety, "check", return_value=Decision("ask", "submit", "Press Submit?")):
            with self.assertLogs("manikin.drive", "WARNING"):
                reply = await echo.chat("drive click 10 10")
        self.assertIn("said no", reply)
        self.assertEqual(pipe.of("drive"), [])


class RoutingTests(unittest.IsolatedAsyncioTestCase):
    async def test_drive_done_and_cancel_reach_the_driver(self):
        pipe = ThreadPipe()
        driver = Driver(pipe)
        brain = Brain(pipe, Gate(pipe, {}, 1), Initiative(FakeSettings()), driver=driver)
        t, out = run_action(driver, Action("move", 5, 5), driver.start())
        pipe.wait_for("drive")
        brain.on_message({"type": "drive_done", "id": 1, "outcome": "done", "x": 5, "y": 5})
        t.join(2)
        self.assertEqual(out["value"], "done")
        self.assertTrue(brain.inbox.empty())
        brain.cancel()
        self.assertEqual(pipe.of("drive_end"), [{"type": "drive_end", "result": "stopped"}])

    def test_the_persona_knows_when_to_drive(self):
        self.assertIn("use drive", PERSONA)
        self.assertIn("into task", PERSONA)


if __name__ == "__main__":
    unittest.main()
