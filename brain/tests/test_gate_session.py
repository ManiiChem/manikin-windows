"""session trust and confirm() in gate.py. only the users click on "yes for this session"
(on an ask that offered it) trusts a tool, nothing the model does can, and confirm() asks
every time anyway
"""

import asyncio
import sys
import tempfile
import unittest
from pathlib import Path

BRAIN_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRAIN_DIR))

from manikin import config                                       # noqa: E402
from manikin.gate import Gate                                    # noqa: E402
from manikin.notes import NoteVault                              # noqa: E402
from manikin.reminders import Reminders                          # noqa: E402
from manikin.tools import ToolContext, ToolError, build_tools    # noqa: E402

LEVELS = {"drive": "ask", "read_file": "ask", "point_at": "ask", "stop_trusting": "allow"}


class FakePipe:
    def __init__(self):
        self.sent = []

    def send(self, type_, **fields):
        self.sent.append({"type": type_, **fields})

    def of(self, type_):
        return [m for m in self.sent if m["type"] == type_]


class SessionTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.pipe = FakePipe()
        self.gate = Gate(self.pipe, dict(LEVELS), 1)

    async def ask(self, coro, *answer):
        """runs a check/confirm and answers its bubble (if one popped up) with *answer"""
        before = len(self.pipe.of("ask"))
        task = asyncio.create_task(coro)
        await asyncio.sleep(0)
        asked = len(self.pipe.of("ask")) > before
        if asked and answer:
            self.gate.answer(self.pipe.sent[-1]["id"], *answer)
        return await task, asked

    async def drive(self, *answer, session=True):
        return await self.ask(self.gate.check("drive", "Can I take the wheel?", session=session),
                              *answer)

    async def test_session_button_trusts_until_revoked(self):
        result, asked = await self.drive(True, "session")
        self.assertIsNone(result)
        self.assertTrue(asked)
        self.assertIs(self.pipe.of("ask")[-1]["session"], True)    # third button shown

        self.assertEqual(await self.drive(), (None, False))      # no bubble now
        self.gate.reset_turn()                                    # a new turn keeps it
        self.assertEqual(await self.drive(), (None, False))

        self.gate.revoke_session()
        result, asked = await self.drive(False)
        self.assertTrue(asked)
        self.assertIn("declined", result)

    async def test_plain_yes_never_trusts(self):
        self.assertEqual(await self.drive(True), (None, True))
        self.assertEqual(await self.drive(True), (None, True))    # asked again

    async def test_scope_on_an_ask_without_the_button(self):
        result, asked = await self.drive(True, "session", session=False)
        self.assertNotIn("session", self.pipe.of("ask")[-1])
        self.assertIsNone(result)                                  # the yes still counts
        self.assertTrue(asked)
        self.assertEqual(await self.drive(True), (None, True))    # but nothing was trusted

    async def test_session_scope_needs_an_explicit_yes(self):
        for allow in (False, "yes", 1, None):
            with self.subTest(allow=allow):
                result, _ = await self.drive(allow, "session")
                self.assertIn("declined", result)
        self.assertEqual(await self.drive(True), (None, True))

    async def test_only_the_exact_scope_counts(self):
        for scope in ("SESSION", "session ", True, 1, ["session"], {"session": True}):
            with self.subTest(scope=scope):
                self.assertEqual(await self.drive(True, scope), (None, True))
        self.assertEqual(await self.drive(True), (None, True))

    async def test_trust_is_for_that_tool_only(self):
        await self.drive(True, "session")
        result, asked = await self.ask(self.gate.check("read_file", "Can I read it?",
                                                       session=True), True)
        self.assertTrue(asked)
        self.assertEqual(await self.drive(), (None, False))

    async def test_trust_never_reaches_an_unprompted_remark(self):
        await self.drive(True, "session")
        result, asked = await self.ask(self.gate.check("drive", "?", proactive=True,
                                                       session=True))
        self.assertIn("unprompted", result)
        self.assertFalse(asked)

    async def test_trust_doesnt_unblock_denied_tools(self):
        self.gate = Gate(self.pipe, {"drive": "deny"}, 1)
        result, asked = await self.drive()
        self.assertIn("block", result)
        self.assertFalse(asked)

    async def test_forged_answers_grant_nothing(self):
        # the first ask is id 1 so a JSON true would hit it if bools counted as ints
        task = asyncio.create_task(self.gate.check("drive", "?", session=True))
        await asyncio.sleep(0)
        self.assertEqual(self.pipe.of("ask")[-1]["id"], 1)
        for forged in (True, 1.0, "1", None, 0, 2, 999, -1):
            self.gate.answer(forged, True, "session")
        await asyncio.sleep(0)
        self.assertFalse(task.done())                             # the real bubble still waits
        self.gate.cancel_all()
        self.assertIn("declined", await task)
        result, asked = await self.drive(False)
        self.assertTrue(asked)                                    # nothing was trusted

    async def test_late_answer_grants_nothing(self):
        gate = Gate(self.pipe, dict(LEVELS), 0.05)
        self.assertIn("didn't answer", await gate.check("drive", "?", session=True))
        ask_id = self.pipe.of("ask")[-1]["id"]
        self.assertEqual(self.pipe.sent[-1], {"type": "ask_cancel", "id": ask_id})
        gate.answer(ask_id, True, "session")                      # clicked after it was hidden
        self.gate = gate
        result, asked = await self.drive(False)
        self.assertTrue(asked)

    async def test_answered_twice(self):
        task = asyncio.create_task(self.gate.check("drive", "?", session=True))
        await asyncio.sleep(0)
        ask_id = self.pipe.of("ask")[-1]["id"]
        self.gate.answer(ask_id, True)
        self.gate.answer(ask_id, True, "session")      # second click on the same bubble
        self.assertIsNone(await task)
        self.assertEqual(await self.drive(True), (None, True))

    async def test_point_at_still_once_per_turn(self):
        self.assertEqual(await self.ask(self.gate.check("point_at", "?"), True), (None, True))
        self.assertEqual(await self.ask(self.gate.check("point_at", "?")), (None, False))
        self.gate.reset_turn()
        result, asked = await self.ask(self.gate.check("point_at", "?"), False)
        self.assertTrue(asked)
        self.assertIn("declined", result)


class ConfirmTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.pipe = FakePipe()
        self.gate = Gate(self.pipe, dict(LEVELS), 1)

    async def confirm(self, *answer, text="Press Submit?"):
        task = asyncio.create_task(self.gate.confirm(text))
        await asyncio.sleep(0)
        ask = self.pipe.sent[-1]
        self.assertEqual(ask["type"], "ask")
        self.assertEqual(ask["text"], text)
        self.assertNotIn("session", ask)              # confirm never offers trust
        if answer:
            self.gate.answer(ask["id"], *answer)
        return await task

    async def test_yes_no(self):
        self.assertIs(await self.confirm(True), True)
        self.assertIs(await self.confirm(False), False)
        self.assertIs(await self.confirm("yes"), False)          # only a real true counts
        self.assertIs(await self.confirm(1), False)

    async def test_asks_even_when_trusted(self):
        task = asyncio.create_task(self.gate.check("drive", "?", session=True))
        await asyncio.sleep(0)
        self.gate.answer(self.pipe.sent[-1]["id"], True, "session")
        self.assertIsNone(await task)
        self.assertIs(await self.confirm(True), True)            # still a bubble
        self.assertIs(await self.confirm(False), False)

    async def test_session_scope_on_a_confirm_grants_nothing(self):
        self.assertIs(await self.confirm(True, "session"), True)
        self.assertIs(await self.confirm(False), False)          # asked again
        task = asyncio.create_task(self.gate.check("drive", "?", session=True))
        await asyncio.sleep(0)
        self.assertEqual(self.pipe.sent[-1]["type"], "ask")       # drive isnt trusted
        self.gate.cancel_all()
        await task

    async def test_timeout_is_a_no(self):
        gate = Gate(self.pipe, dict(LEVELS), 0.05)
        self.assertIs(await gate.confirm("Carry on?"), False)
        ask_id = self.pipe.of("ask")[-1]["id"]
        self.assertEqual(self.pipe.sent[-1], {"type": "ask_cancel", "id": ask_id})
        gate.answer(ask_id, True)                                # too late, ignored

    async def test_cancel_all(self):
        first = asyncio.create_task(self.gate.confirm("Press Send?"))
        second = asyncio.create_task(self.gate.check("drive", "?", session=True))
        await asyncio.sleep(0)
        self.gate.cancel_all()
        self.assertIs(await first, False)
        self.assertIn("declined", await second)
        self.assertEqual([m["type"] for m in self.pipe.sent],
                         ["ask", "ask", "ask_cancel", "ask_cancel"])

    async def test_text_is_cleaned_for_display(self):
        task = asyncio.create_task(self.gate.confirm("It’s fine?"))
        await asyncio.sleep(0)
        self.assertEqual(self.pipe.sent[-1]["text"], "It's fine?")
        self.gate.cancel_all()
        await task


class ToolWiringTests(unittest.IsolatedAsyncioTestCase):
    """the model only reaches the gate through tools, and none of them can grant trust"""

    def tools(self, d, gate=None):
        ctx = ToolContext(Path(d), NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print),
                          gate=gate)
        return {t.name: t for t in build_tools(ctx)}

    async def test_stop_trusting_revokes(self):
        pipe = FakePipe()
        gate = Gate(pipe, dict(LEVELS), 1)
        task = asyncio.create_task(gate.check("drive", "?", session=True))
        await asyncio.sleep(0)
        gate.answer(pipe.sent[-1]["id"], True, "session")
        await task
        with tempfile.TemporaryDirectory() as d:
            result = self.tools(d, gate)["stop_trusting"].run({})
        self.assertIn("ask", result)
        task = asyncio.create_task(gate.check("drive", "?", session=True))
        await asyncio.sleep(0)
        self.assertEqual(pipe.sent[-1]["type"], "ask")           # asks again
        gate.cancel_all()
        await task

    def test_stop_trusting_without_a_gate(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(ToolError):
                self.tools(d)["stop_trusting"].run({})

    def test_only_the_plan_offers_trust(self):
        with tempfile.TemporaryDirectory() as d:
            tools = self.tools(d)
        self.assertEqual([name for name, t in tools.items() if t.session], ["drive"])
        self.assertEqual(config.DEFAULT_PERMISSIONS["drive"], "ask")
        self.assertEqual(config.DEFAULT_PERMISSIONS["stop_trusting"], "allow")
        self.assertEqual(tools["stop_trusting"].params, {})       # nothing to pass in


if __name__ == "__main__":
    unittest.main()
