"""offline tests for talking first, cancel and chattiness. no API calls and no real watcher"""

import asyncio
import json
import os
import random
import sys
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace as NS

BRAIN_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRAIN_DIR))

from manikin.app import Brain                                   # noqa: E402
from manikin.backend import ClaudeBackend, EchoBackend          # noqa: E402
from manikin.config import Settings                             # noqa: E402
from manikin.gate import Gate                                   # noqa: E402
from manikin.initiative import LEVELS, PREFIX, Initiative, is_quiet   # noqa: E402
from manikin.notes import NoteVault                             # noqa: E402
from manikin.reminders import Reminders                         # noqa: E402
from manikin.tools import ToolContext, ToolError, build_tools   # noqa: E402

from test_brain import FakeClaude, FakePipe, _response, _text   # noqa: E402

T0 = 1_800_000_000.0


@dataclass
class FakeSettings:
    chattiness: str = "often"


@dataclass(frozen=True)
class Obs:                       # same shape as watcher.Observation
    kind: str
    text: str


def initiative(level="often", seed=1) -> Initiative:
    return Initiative(FakeSettings(level), random.Random(seed))


def presence(ini, now, pet_idle=0.0, pc_idle=-1.0, typing=False, asleep=False):
    ini.update_presence({"type": "presence", "pet_idle": pet_idle, "pc_idle": pc_idle,
                         "typing": typing, "asleep": asleep}, now)


class CapAndGapTests(unittest.TestCase):
    def test_each_level(self):
        for name, level in LEVELS.items():
            if name == "off":
                continue
            with self.subTest(level=name):
                ini, t = initiative(name), T0
                for _ in range(level.per_hour):
                    event = ini.woke(60, t)
                    self.assertIsNotNone(event)
                    ini.said(event, t)
                    self.assertIsNone(ini.woke(60, t + level.gap_s - 1))     # min gap
                    t += level.gap_s
                self.assertIsNone(ini.woke(60, t))                           # hourly cap
                self.assertIsNotNone(ini.woke(60, T0 + 3600))                # first one aged out

    def test_only_said_remarks_count(self):
        ini = initiative("rarely")
        self.assertIsNotNone(ini.woke(60, T0))          # fired but stayed [quiet] so it wasnt said
        self.assertIsNotNone(ini.woke(60, T0 + 1))

    def test_greeting_once_and_exempt(self):
        ini = initiative("rarely")
        ini.said(ini.woke(60, T0), T0)                  # cap of 1 now used up
        greeting = ini.greeting(T0 + 10)
        self.assertIsNotNone(greeting)
        self.assertTrue(greeting.prompt.startswith(PREFIX))
        self.assertIn("It's ", greeting.prompt)
        ini.said(greeting, T0 + 10)
        self.assertIsNone(ini.greeting(T0 + 20))         # only once
        ini2 = initiative("rarely")
        ini2.said(ini2.greeting(T0), T0)
        self.assertIsNotNone(ini2.woke(60, T0 + 1))      # greeting didnt start the gap

    def test_off_silences_everything(self):
        ini = initiative("off")
        presence(ini, T0, pet_idle=10_000)
        self.assertIsNone(ini.greeting(T0))
        self.assertIsNone(ini.woke(600, T0))
        self.assertIsNone(ini.quiet_check(T0, busy=False))
        self.assertIsNone(ini.observation(Obs("cpu_high", "CPU busy"), T0, busy=False))

    def test_level_change_applies_live(self):
        settings = FakeSettings("often")
        ini = Initiative(settings, random.Random(1))
        self.assertIsNotNone(ini.woke(60, T0))
        settings.chattiness = "off"
        self.assertIsNone(ini.woke(60, T0 + 1))


class WokeTests(unittest.TestCase):
    def test_prompt_mentions_the_nap(self):
        event = initiative().woke(12 * 60, T0)
        self.assertEqual(event.kind, "woke")
        self.assertTrue(event.prompt.startswith(PREFIX + " It's "))
        self.assertIn("about 12 minutes", event.prompt)
        self.assertIn("[quiet]", event.prompt)


class QuietStretchTests(unittest.TestCase):
    LONG = 15 * 60 + 1          # longer than any "often" wait

    def test_fires_after_a_quiet_stretch(self):
        ini = initiative()
        presence(ini, T0, pet_idle=60)
        self.assertIsNone(ini.quiet_check(T0, busy=False))           # too soon
        presence(ini, T0, pet_idle=self.LONG)
        event = ini.quiet_check(T0, busy=False)
        self.assertEqual(event.kind, "quiet")
        self.assertIn("[quiet]", event.prompt)

    def test_unknown_pc_idle_counts_as_present(self):
        ini = initiative()
        presence(ini, T0, pet_idle=self.LONG, pc_idle=-1)
        self.assertIsNotNone(ini.quiet_check(T0, busy=False))

    def test_presence_gating(self):
        for kwargs, busy in (({"pc_idle": 120}, False), ({"pc_idle": 999}, False),
                             ({"typing": True}, False), ({}, True)):
            with self.subTest(**kwargs, busy=busy):
                ini = initiative()
                presence(ini, T0, pet_idle=self.LONG, **kwargs)
                self.assertIsNone(ini.quiet_check(T0, busy=busy))

    def test_waits_again_after_each_attempt(self):
        ini = initiative()
        presence(ini, T0, pet_idle=self.LONG)
        ini.said(ini.quiet_check(T0, busy=False), T0)
        presence(ini, T0 + 400, pet_idle=self.LONG + 400)   # past the gap, not past a new wait
        self.assertIsNone(ini.quiet_check(T0 + 400, busy=False))
        t = T0 + self.LONG
        presence(ini, t, pet_idle=self.LONG * 2)
        self.assertIsNotNone(ini.quiet_check(t, busy=False))

    def test_ideas_vary(self):
        ini, t, prompts = initiative(), T0, []
        for _ in range(4):
            presence(ini, t, pet_idle=10_000)
            prompts.append(ini.quiet_check(t, busy=False).prompt)
            t += 3600
        self.assertTrue(all(a != b for a, b in zip(prompts, prompts[1:])))


class ObservationTests(unittest.TestCase):
    def test_gated_by_presence(self):
        ini = initiative()
        presence(ini, T0, pc_idle=300)
        self.assertIsNone(ini.observation(Obs("cpu_high", "CPU at 94%"), T0, busy=False))
        presence(ini, T0, pc_idle=5)
        event = ini.observation(Obs("cpu_high", "CPU at 94%"), T0, busy=False)
        self.assertEqual(event.kind, "cpu_high")
        self.assertIn("CPU at 94%", event.prompt)
        # answered [quiet], the same thing isnt raised again straight away
        self.assertIsNone(ini.observation(Obs("cpu_high", "CPU at 95%"), T0 + 10, busy=False))
        self.assertIsNone(ini.observation(Obs("ram_high", "RAM full"), T0, busy=True))


class QuietReplyTests(unittest.TestCase):
    def test_is_quiet(self):
        for text in ("[quiet]", " [QUIET] ", "[quiet].", "", "..."):
            self.assertTrue(is_quiet(text), text)
        for text in ("Hi!", "[quiet] but actually hello"):
            self.assertFalse(is_quiet(text), text)


class GateProactiveTests(unittest.IsolatedAsyncioTestCase):
    async def test_ask_tools_are_auto_denied(self):
        pipe = FakePipe()
        gate = Gate(pipe, {"a": "allow", "t": "ask"}, 5)
        self.assertIn("unprompted", await gate.check("t", "Can I?", proactive=True))
        self.assertIsNone(await gate.check("a", "?", proactive=True))
        self.assertEqual(pipe.sent, [])                  # no bubble popped up

    async def test_cancel_all(self):
        pipe = FakePipe()
        gate = Gate(pipe, {"t": "ask"}, 5)
        task = asyncio.create_task(gate.check("t", "Can I?"))
        await asyncio.sleep(0)
        gate.cancel_all()
        self.assertIn("declined", await task)
        self.assertEqual([m["type"] for m in pipe.sent], ["ask", "ask_cancel"])


class SettingsTests(unittest.TestCase):
    def test_set_chattiness_persists(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "settings.json"
            settings = Settings(path)
            self.assertEqual(settings.chattiness, "often")          # default, no file yet
            ctx = ToolContext(Path(d), NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print),
                              settings=settings)
            tool = {t.name: t for t in build_tools(ctx)}["set_chattiness"]
            self.assertIsNotNone(tool.validate({"level": "loud"}))   # enum enforced
            self.assertIsNone(tool.validate({"level": "rarely"}))
            self.assertIn("rarely", tool.run({"level": "rarely"}))
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")),
                             {"chattiness": "rarely", "sound": {"volume": 30, "muted": False, "voice": True}})
            self.assertEqual(Settings(path).chattiness, "rarely")    # survives a restart
            self.assertFalse((Path(d) / "settings.tmp").exists())

    def test_bad_file_falls_back(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "settings.json"
            path.write_text('{"chattiness": "shouty"}', encoding="utf-8")
            self.assertEqual(Settings(path).chattiness, "often")


class SoundSettingsTests(unittest.TestCase):
    DEFAULT = {"volume": 30, "muted": False, "voice": True}

    def load(self, text):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "settings.json"
            path.write_text(text, encoding="utf-8")
            return Settings(path)

    def test_defaults(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(Settings(Path(d) / "settings.json").sound, self.DEFAULT)     # no file
        self.assertEqual(self.load("not json").sound, self.DEFAULT)
        self.assertEqual(self.load('{"chattiness": "rarely"}').sound, self.DEFAULT)    # older file
        self.assertEqual(self.load('{"sound": [1, 2]}').sound, self.DEFAULT)

    def test_bad_values_are_ignored_or_clamped(self):
        for sound, expected in (
                ({"volume": 150}, {"volume": 100}),            # an int out of range is clamped
                ({"volume": -5}, {"volume": 0}),
                ({"volume": True}, {}),                         # a bool isnt a volume
                ({"volume": 30.5}, {}),
                ({"volume": "40"}, {}),
                ({"muted": 1, "voice": "no"}, {}),
                ({"muted": True}, {"muted": True}),             # the rest keep their defaults
                ({"volume": 20, "voice": False, "extra": 1}, {"volume": 20, "voice": False})):
            with self.subTest(sound=sound):
                settings = self.load(json.dumps({"chattiness": "sometimes", "sound": sound}))
                self.assertEqual(settings.sound, {**self.DEFAULT, **expected})
                self.assertEqual(settings.chattiness, "sometimes")

    def test_round_trip_keeps_everything(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "settings.json"
            settings = Settings(path)
            settings.set_chattiness("rarely")
            self.assertEqual(settings.set_sound(volume=35, voice=False),
                             {"volume": 35, "muted": False, "voice": False})
            saved = {"chattiness": "rarely", "sound": {"volume": 35, "muted": False, "voice": False}}
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), saved)
            settings.set_sound(muted=True)                       # partial, the rest stay
            again = Settings(path)
            self.assertEqual(again.chattiness, "rarely")
            self.assertEqual(again.sound, {"volume": 35, "muted": True, "voice": False})
            again.set_chattiness("off")                          # keeps the sound settings
            self.assertEqual(Settings(path).sound, {"volume": 35, "muted": True, "voice": False})
            self.assertFalse((Path(d) / "settings.tmp").exists())

    def test_set_sound_rejects_bad_values(self):
        with tempfile.TemporaryDirectory() as d:
            settings = Settings(Path(d) / "settings.json")
            for kwargs in ({"volume": 101}, {"volume": -1}, {"volume": True}, {"muted": "yes"}, {"voice": 0}):
                with self.subTest(**kwargs), self.assertRaises(ValueError):
                    settings.set_sound(**kwargs)
            self.assertEqual(settings.sound, self.DEFAULT)
            self.assertFalse((Path(d) / "settings.json").exists())   # nothing was saved

    def test_hook_runs_after_each_saved_sound_change(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "settings.json"
            settings = Settings(path)
            seen = []
            settings.on_sound_change = lambda sound: seen.append(
                (sound, json.loads(path.read_text(encoding="utf-8"))["sound"]))
            settings.set_sound(volume=10)
            settings.set_chattiness("rarely")                    # not a sound change
            settings.set_sound(muted=True)
            self.assertEqual(seen, [
                ({"volume": 10, "muted": False, "voice": True},) * 2,     # saved before the hook
                ({"volume": 10, "muted": True, "voice": True},) * 2,
            ])

    def test_tool(self):
        with tempfile.TemporaryDirectory() as d:
            settings = Settings(Path(d) / "settings.json")
            settings.set_chattiness("rarely")
            ctx = ToolContext(Path(d), NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print),
                              settings=settings)
            tool = {t.name: t for t in build_tools(ctx)}["set_sound"]
            self.assertEqual(tool.schema()["input_schema"]["required"], [])
            self.assertIsNone(tool.validate({}))                  # the handler insists on one
            with self.assertRaises(ToolError):
                tool.run({})
            self.assertIsNotNone(tool.validate({"volume": True}))
            self.assertIsNotNone(tool.validate({"muted": "yes"}))
            self.assertIsNotNone(tool.validate({"loud": True}))
            for volume in (101, -1):
                self.assertIsNone(tool.validate({"volume": volume}))   # the type is fine...
                with self.assertRaises(ToolError):                     # ...the range isnt
                    tool.run({"volume": volume})
            with self.assertRaises(ToolError):
                tool.run({"volume": True})                        # even if validate was skipped
            self.assertEqual(settings.sound, SoundSettingsTests.DEFAULT)

            self.assertEqual(tool.run({"volume": 40}), "Done: volume 40%, sound on, babble voice on.")
            self.assertEqual(tool.run({"voice": False}), "Done: volume 40%, sound on, babble voice off.")
            self.assertEqual(tool.run({"muted": True, "volume": 0}),
                             "Done: volume 0%, sound muted, babble voice off.")
            self.assertEqual(Settings(settings.path).chattiness, "rarely")      # set_sound keeps it

    def test_tool_is_allowed_by_default(self):
        from manikin.config import DEFAULT_PERMISSIONS
        self.assertEqual(DEFAULT_PERMISSIONS["set_sound"], "allow")


class EchoSoundTests(unittest.IsolatedAsyncioTestCase):
    async def test_commands(self):
        with tempfile.TemporaryDirectory() as d:
            settings = Settings(Path(d) / "settings.json")
            sent = []
            settings.on_sound_change = sent.append
            ctx = ToolContext(Path(d), NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print),
                              settings=settings)
            echo = EchoBackend(None, build_tools(ctx))
            self.assertEqual(await echo.chat("volume 30"), "[happy] Done: volume 30%, sound on, babble voice on.")
            self.assertIn("muted", await echo.chat("Mute"))
            self.assertIn("sound on", await echo.chat("unmute"))
            self.assertIn("voice off", await echo.chat("voice  off"))
            self.assertIn("voice on", await echo.chat("voice on"))
            self.assertEqual(await echo.chat("volume 300"), "[confused] volume must be from 0 to 100")
            self.assertEqual(await echo.chat("mute please"), "mute please")     # not a command, just echoed
            self.assertEqual([s["volume"] for s in sent], [30] * 5)
            self.assertEqual([s["muted"] for s in sent], [False, True, False, False, False])
            self.assertEqual([s["voice"] for s in sent], [True, True, True, False, True])
            self.assertEqual(await EchoBackend().chat("mute"), "mute")         # no tools, plain echo


class LoopPipe(FakePipe):
    def __init__(self):
        super().__init__()
        self.closed = asyncio.Event()


class ScriptedBackend:
    def __init__(self, *replies):
        self.replies = list(replies)
        self.calls = []
        self.after = 0

    async def chat(self, text, proactive=False, kind=None):
        self.calls.append((text, proactive))
        return self.replies.pop(0)

    async def after_reply(self):
        self.after += 1


async def until(cond, what="condition"):
    for _ in range(500):
        if cond():
            return
        await asyncio.sleep(0.01)
    raise AssertionError(f"timed out waiting for {what}")


class BrainTests(unittest.IsolatedAsyncioTestCase):
    def make(self, backend, level="often", greet=True):
        pipe = LoopPipe()
        brain = Brain(pipe, Gate(pipe, {}, 5), initiative(level), clock=lambda: T0)
        brain.initiative._greeted = not greet
        brain.start(backend)
        return pipe, brain

    async def serve(self, brain, pipe):
        task = asyncio.create_task(brain.serve())
        await until(lambda: brain.inbox.empty() and not brain.busy, "an idle brain")
        pipe.closed.set()
        await task

    async def test_greeting_is_said_proactively(self):
        backend = ScriptedBackend("[happy] Morning!")
        pipe, brain = self.make(backend)
        await self.serve(brain, pipe)
        self.assertEqual(pipe.sent, [{"type": "say", "text": "Morning!", "mood": "happy", "proactive": True}])
        self.assertTrue(backend.calls[0][1])                 # chat(..., proactive=True)
        self.assertEqual(backend.after, 1)                   # after_reply still runs

    async def test_quiet_reply_sends_nothing(self):
        backend = ScriptedBackend("[smug] [quiet]", "[happy] Nap over!")
        pipe, brain = self.make(backend)
        brain.on_message({"type": "event", "name": "woke", "slept_s": 300})
        await self.serve(brain, pipe)
        self.assertEqual([m["text"] for m in pipe.sent], ["Nap over!"])   # greeting stayed quiet
        self.assertNotIn("thinking", [m["type"] for m in pipe.sent])
        self.assertEqual(backend.after, 2)

    async def test_event_dropped_when_typing_or_user_waiting(self):
        backend = ScriptedBackend("[happy] hi user")
        pipe, brain = self.make(backend)                     # greeting queued
        brain.on_message({"type": "user", "text": "hello"})  # user message waiting, greeting dropped
        await self.serve(brain, pipe)
        self.assertEqual(backend.calls, [("hello", False)])

        backend = ScriptedBackend()
        pipe, brain = self.make(backend)
        brain.on_message({"type": "presence", "pet_idle": 0, "pc_idle": 0, "typing": True, "asleep": False})
        await self.serve(brain, pipe)
        self.assertEqual(backend.calls, [])                  # typing now, greeting dropped

    async def test_watcher_observation_becomes_a_turn(self):
        class FakeWatcher:
            def __init__(self):
                self.batches = [[Obs("battery_low", "Battery at 8%")]]

            def poll(self, now=None):
                return self.batches.pop(0) if self.batches else []

        backend = ScriptedBackend("[worried] Plug me in!")
        pipe, brain = self.make(backend, level="sometimes", greet=False)
        watch = asyncio.create_task(brain.watch(FakeWatcher(), every=0.01))
        serve = asyncio.create_task(brain.serve())
        await until(lambda: pipe.sent, "a say")
        watch.cancel()
        pipe.closed.set()
        await serve
        self.assertEqual(pipe.sent[0]["text"], "Plug me in!")
        self.assertIn("Battery at 8%", backend.calls[0][0])

    async def test_sound_set_saves_and_echoes_without_a_turn(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "settings.json"
            settings = Settings(path)
            settings.set_sound(volume=45)
            backend = ScriptedBackend()
            pipe = LoopPipe()
            settings.on_sound_change = lambda sound: pipe.send("sound", **sound)
            brain = Brain(pipe, Gate(pipe, {}, 5), initiative("off"), clock=lambda: T0, settings=settings)
            brain.start(backend)
            brain.on_message({"type": "sound_set", "muted": True})
            for bad in ({"type": "sound_set"}, {"type": "sound_set", "muted": 1}):
                with self.assertLogs("manikin", "WARNING"):
                    brain.on_message(bad)                         # malformed, ignored
            self.assertTrue(brain.inbox.empty())
            self.assertFalse(brain.busy)
            await self.serve(brain, pipe)
            self.assertEqual(pipe.sent, [{"type": "sound", "volume": 45, "muted": True, "voice": True}])
            self.assertEqual(backend.calls, [])
            self.assertTrue(Settings(path).muted)

    async def test_cancel_with_nothing_running(self):
        pipe, brain = self.make(ScriptedBackend(), level="off")
        brain.on_message({"type": "cancel"})
        self.assertEqual(pipe.sent, [{"type": "cancelled"}])

    async def test_cancel_rolls_back_and_cleans_up(self):
        os.environ.setdefault("ANTHROPIC_API_KEY", "placeholder-not-used")
        with tempfile.TemporaryDirectory() as d:
            pipe = LoopPipe()
            gate = Gate(pipe, {"write_file": "ask"}, 5)
            box = Path(d) / "sandbox"
            box.mkdir()
            ctx = ToolContext(box, NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print))
            backend = ClaudeBackend(pipe, gate, build_tools(ctx), "memory", ctx.chat)
            backend._client = NS(beta=NS(messages=FakeClaude([
                _response("end_turn", _text("[happy] Earlier reply.")),
                _response("tool_use", NS(type="tool_use", id="t1", name="write_file",
                                         input={"name": "hi.txt", "content": "hi"})),
            ])))
            brain = Brain(pipe, gate, initiative("off"), clock=lambda: T0)
            brain.start(backend)
            serve = asyncio.create_task(brain.serve())

            brain.on_message({"type": "user", "text": "first"})
            await until(lambda: any(m["type"] == "say" for m in pipe.sent), "the first reply")
            backend._summary = "a summary waiting to be used"   # as if compaction just ran
            backend._messages.clear()

            brain.on_message({"type": "user", "text": "write hi.txt"})
            brain.on_message({"type": "user", "text": "and this one is queued"})
            await until(lambda: pipe.sent[-1]["type"] == "ask", "the permission ask")
            self.assertTrue(brain.inbox.has_user())
            brain.on_message({"type": "cancel"})
            brain.on_message({"type": "cancel"})                 # Esc twice, still one reply
            await until(lambda: pipe.sent[-1]["type"] == "cancelled", "cancelled")
            await asyncio.sleep(0.05)

            self.assertEqual([m["type"] for m in pipe.sent[-4:]], ["thinking", "ask", "ask_cancel", "cancelled"])
            self.assertEqual(backend._messages, [])              # back to the length before the turn
            self.assertEqual(backend._summary, "a summary waiting to be used")
            self.assertTrue(brain.inbox.empty())                 # queued message dropped
            self.assertFalse((box / "hi.txt").exists())
            pipe.closed.set()
            await serve

    async def test_cancel_mid_stream_truncates_to_prefix(self):
        os.environ.setdefault("ANTHROPIC_API_KEY", "placeholder-not-used")
        with tempfile.TemporaryDirectory() as d:
            pipe = LoopPipe()
            gate = Gate(pipe, {}, 5)
            ctx = ToolContext(Path(d), NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print))
            backend = ClaudeBackend(pipe, gate, build_tools(ctx), "memory", ctx.chat)
            fake = FakeClaude([_response("end_turn", _text("one")),
                               _response("end_turn", _text("slow"), _text("reply"))])
            backend._client = NS(beta=NS(messages=fake))
            await backend.chat("hi")
            self.assertEqual(len(backend._messages), 2)
            fake.delay = 5                                     # the next stream hangs
            task = asyncio.create_task(backend.chat("tell me a long story"))
            await until(lambda: len(fake.requests) == 2, "the second request")
            task.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await task
            self.assertEqual(len(backend._messages), 2)
            self.assertEqual(backend._messages[0]["content"], "hi")


if __name__ == "__main__":
    unittest.main()
