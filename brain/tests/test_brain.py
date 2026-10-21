"""offline tests, nothing in here calls the Anthropic API

run from brain/:  .venv/Scripts/python -m unittest discover tests
"""

import asyncio
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

BRAIN_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRAIN_DIR))

from manikin.backend import ClaudeBackend          # noqa: E402
from manikin.gate import Gate                      # noqa: E402
from manikin.notes import NoteVault                # noqa: E402
from manikin.protocol import to_display            # noqa: E402
from manikin.reminders import Reminders            # noqa: E402
from manikin.tools import (Tool, ToolContext, ToolError, build_tools,  # noqa: E402
                           sandbox_path, set_reminder)


class FakePipe:
    def __init__(self):
        self.sent = []

    def send(self, type_, **fields):
        self.sent.append({"type": type_, **fields})


class GateTests(unittest.IsolatedAsyncioTestCase):
    async def test_levels(self):
        gate = Gate(FakePipe(), {"a": "allow", "d": "deny"}, 1)
        self.assertIsNone(await gate.check("a", "?"))
        self.assertIn("block", await gate.check("d", "?"))
        self.assertIn("block", await gate.check("unknown", "?"))   # unknown = deny

    async def test_ask_yes_no_timeout(self):
        pipe = FakePipe()
        gate = Gate(pipe, {"t": "ask"}, 0.2)

        task = asyncio.create_task(gate.check("t", "Can I?"))
        await asyncio.sleep(0)
        ask = pipe.sent[-1]
        self.assertEqual(ask["type"], "ask")
        gate.answer(ask["id"], True)
        self.assertIsNone(await task)

        task = asyncio.create_task(gate.check("t", "Can I?"))
        await asyncio.sleep(0)
        gate.answer(pipe.sent[-1]["id"], "yes")      # only a real true counts
        self.assertIn("declined", await task)

        self.assertIn("didn't answer", await gate.check("t", "Can I?"))
        self.assertEqual(pipe.sent[-1]["type"], "ask_cancel")


class VaultTests(unittest.TestCase):
    def test_roundtrip_and_wrong_password(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "notes.enc"
            vault = NoteVault(path)
            vault.create("correct horse")
            vault.add("likes pixel art")
            self.assertNotIn(b"pixel", path.read_bytes())      # encrypted at rest

            self.assertFalse(NoteVault(path).unlock("wrong password"))
            again = NoteVault(path)
            self.assertTrue(again.unlock("correct horse"))
            self.assertEqual([n["text"] for n in again.all()], ["likes pixel art"])

    def test_create_that_cant_save_stays_locked(self):
        with tempfile.TemporaryDirectory() as d:
            vault = NoteVault(Path(d) / "missing folder" / "notes.enc")
            with self.assertRaises(OSError):
                vault.create("correct horse")
            self.assertFalse(vault.unlocked)


class ReminderTests(unittest.TestCase):
    def test_persist_fire_and_missed(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "reminders.json"
            fired = []
            r = Reminders(path, fired.append)
            r.add("past", time.time() - 600)
            r.add("soon", time.time() + 5)
            r.add("later", time.time() + 3600)

            reopened = Reminders(path, fired.append)        # as if the app restarted
            self.assertEqual([m["text"] for m in reopened.take_missed()], ["past"])
            reopened.fire_due(now=time.time() + 10)
            self.assertEqual([f["text"] for f in fired], ["soon"])
            self.assertEqual([x["text"] for x in Reminders(path, fired.append).all()], ["later"])

    def test_malformed_file_is_skipped(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "reminders.json"
            path.write_text(json.dumps({"not": "a list"}), encoding="utf-8")
            self.assertEqual(Reminders(path, print).all(), [])
            path.write_text(json.dumps([{"id": 1, "text": "ok", "due": time.time() - 5},
                                        {"id": "2", "text": "id is a string", "due": 0},
                                        "junk", {"id": 3, "text": "no due"}]), encoding="utf-8")
            self.assertEqual([m["text"] for m in Reminders(path, print).take_missed()], ["ok"])

    def test_set_reminder_validation(self):
        with tempfile.TemporaryDirectory() as d:
            ctx = ToolContext(Path(d), NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print))
            with self.assertRaises(ToolError):
                set_reminder(ctx, {"text": "x"})                                   # neither
            with self.assertRaises(ToolError):
                set_reminder(ctx, {"text": "x", "minutes_from_now": 5, "at_local_time": "2030-01-01T10:00"})
            with self.assertRaises(ToolError):
                set_reminder(ctx, {"text": "x", "at_local_time": "2001-01-01T10:00"})  # past
            self.assertIn("set for", set_reminder(ctx, {"text": "x", "minutes_from_now": 20}))


class SandboxTests(unittest.TestCase):
    def test_escapes_are_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            box = Path(d) / "sandbox"
            box.mkdir()
            self.assertEqual(sandbox_path(box, "a/b.txt"), (box / "a" / "b.txt").resolve())
            for bad in ["../evil.txt", "a/../../evil.txt", str(Path(d) / "abs.txt"), "", "."]:
                with self.assertRaises(ToolError, msg=bad):
                    sandbox_path(box, bad)

    def test_symlink_escape_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            box = Path(d) / "sandbox"
            box.mkdir()
            outside = Path(d) / "outside"
            outside.mkdir()
            try:
                os.symlink(outside, box / "link", target_is_directory=True)
            except OSError:
                # symlinks need developer mode on windows, a junction does the same trick without it
                subprocess.run(["cmd", "/c", "mklink", "/J", str(box / "link"), str(outside)],
                               check=True, capture_output=True)
            with self.assertRaises(ToolError):
                sandbox_path(box, "link/evil.txt")


class ToolSchemaTests(unittest.TestCase):
    def test_schemas_and_validation(self):
        with tempfile.TemporaryDirectory() as d:
            ctx = ToolContext(Path(d), NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print))
            tools = {t.name: t for t in build_tools(ctx)}
            for t in tools.values():
                json.dumps(t.schema())
            self.assertIsNotNone(tools["open_url"].validate({}))
            self.assertIsNotNone(tools["delete_note"].validate({"id": "3"}))
            self.assertIsNotNone(tools["delete_note"].validate({"id": True}))
            self.assertIsNone(tools["delete_note"].validate({"id": 3}))
            with self.assertRaises(ToolError):
                tools["add_note"].run({"text": "x"})          # vault locked
            with self.assertRaises(ToolError):
                tools["open_url"].run({"url": "file:///etc/passwd"})


class FakeStream:
    """fake SDK message stream, one content_block_start per block"""

    def __init__(self, response, delay=0.0):
        self.response = response
        self.delay = delay

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False

    async def __aiter__(self):
        from types import SimpleNamespace as NS
        for block in self.response.content:
            await asyncio.sleep(self.delay)
            yield NS(type="content_block_start", content_block=block)

    async def get_final_message(self):
        return self.response


class FakeClaude:
    """fake client.beta.messages, stream() plays back scripted responses"""

    def __init__(self, responses, delay=0.0):
        self.responses = list(responses)
        self.requests = []
        self.delay = delay

    def stream(self, **kwargs):
        self.requests.append(kwargs)
        return FakeStream(self.responses.pop(0), self.delay)


def _response(stop_reason, *content, context=1):
    from types import SimpleNamespace as NS
    return NS(stop_reason=stop_reason, content=list(content),
              usage=NS(input_tokens=context, output_tokens=1, cache_read_input_tokens=0,
                       cache_creation_input_tokens=0))


def _text(text):
    from types import SimpleNamespace as NS
    return NS(type="text", text=text)


def _tool_call(name, **args):
    from types import SimpleNamespace as NS
    return NS(type="tool_use", id=f"call_{name}", name=name, input=args)


class FinalTextTests(unittest.TestCase):
    """narration before a web search shouldnt get glued onto the answer"""

    def _search(self):
        from types import SimpleNamespace as NS
        return [NS(type="server_tool_use"), NS(type="web_search_tool_result")]

    def test_keeps_text_after_the_last_search(self):
        from manikin.backend import final_text
        content = [_text("Let me look."), *self._search(), _text("Results are poor."),
                   *self._search(), _text("[happy] Found it: "), _text("chips are up.")]
        self.assertEqual(final_text(content), "[happy] Found it: chips are up.")

    def test_carries_the_mood_over_from_the_narration(self):
        from manikin.backend import final_text
        content = [_text("[excited] Ooh, checking!"), *self._search(), _text("It's sunny.")]
        self.assertEqual(final_text(content), "[excited] It's sunny.")

    def test_plain_reply_unchanged(self):
        from manikin.backend import final_text
        self.assertEqual(final_text([_text("[smug] Easy.")]), "[smug] Easy.")
        self.assertEqual(final_text([]), "")


class ClaudeLoopTests(unittest.IsolatedAsyncioTestCase):
    """the tool loop and permission gate with a fake model (no API calls)"""

    async def run_write_file(self, allow: bool):
        from types import SimpleNamespace as NS
        os.environ.setdefault("ANTHROPIC_API_KEY", "placeholder-not-used")

        with tempfile.TemporaryDirectory() as d:
            loop = asyncio.get_running_loop()
            pipe = FakePipe()
            gate = Gate(pipe, {"write_file": "ask", "web_search": "allow"}, 5)
            real_send = pipe.send

            def send(type_, **fields):           # the "user" clicks as soon as the bubble appears
                real_send(type_, **fields)
                if type_ == "ask":
                    loop.call_soon(gate.answer, fields["id"], allow)
            pipe.send = send

            box = Path(d) / "sandbox"
            box.mkdir()
            ctx = ToolContext(box, NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print))
            backend = ClaudeBackend(pipe, gate, build_tools(ctx), "Your long-term memory is empty so far.", ctx.chat)
            fake = FakeClaude([
                _response("pause_turn", NS(type="server_tool_use", id="s1", name="web_search")),
                _response("tool_use", NS(type="tool_use", id="t1", name="write_file",
                                         input={"name": "hi.txt", "content": "hi!"})),
                _response("end_turn", NS(type="text", text="All done.")),
            ])
            backend._client = NS(beta=NS(messages=fake))

            reply = await backend.chat("write hi.txt please")
            written = (box / "hi.txt").exists()
            return reply, written, pipe.sent, fake.requests, backend._messages

    async def test_yes_runs_the_tool(self):
        reply, written, sent, requests, messages = await self.run_write_file(True)
        self.assertEqual(reply, "All done.")
        self.assertTrue(written)
        self.assertEqual(sent, [
            {"type": "status", "text": "searching the web"},      # from the stream
            {"type": "ask", "id": 1, "text": "Can I write sandbox/hi.txt?"},
            {"type": "status", "text": "writing a file"},         # only once allowed
        ])
        self.assertEqual(requests[0]["cache_control"], {"type": "ephemeral", "ttl": "1h"})
        self.assertEqual(requests[0]["fallbacks"], "default")
        self.assertEqual(len(requests), 3)                     # pause_turn was resumed
        self.assertEqual([m["role"] for m in messages], ["user", "assistant", "assistant", "user", "assistant"])
        result = messages[3]["content"][0]
        self.assertEqual(result["tool_use_id"], "t1")
        self.assertNotIn("is_error", result)
        self.assertIn({"type": "web_search_20260209", "name": "web_search", "max_uses": 3}, requests[0]["tools"])

    async def test_no_blocks_the_tool(self):
        reply, written, sent, _, messages = await self.run_write_file(False)
        self.assertFalse(written)
        self.assertEqual([m["type"] for m in sent], ["status", "ask"])   # no tool status, it never ran
        result = messages[3]["content"][0]
        self.assertTrue(result["is_error"])
        self.assertIn("declined", result["content"])


class ChatHousekeepingTests(unittest.IsolatedAsyncioTestCase):
    """clear_chat/compact_chat kick in after the reply, never mid turn"""

    def make_backend(self, d, responses):
        from types import SimpleNamespace as NS
        os.environ.setdefault("ANTHROPIC_API_KEY", "placeholder-not-used")
        ctx = ToolContext(Path(d), NoteVault(Path(d) / "n"), Reminders(Path(d) / "r.json", print))
        gate = Gate(FakePipe(), {"clear_chat": "allow", "compact_chat": "allow"}, 5)
        backend = ClaudeBackend(FakePipe(), gate, build_tools(ctx), "memory", ctx.chat)
        fake = FakeClaude(responses)
        backend._client = NS(beta=NS(messages=fake))
        return backend, fake

    async def test_clear(self):
        with tempfile.TemporaryDirectory() as d:
            backend, fake = self.make_backend(d, [
                _response("end_turn", _text("Noted.")),
                _response("tool_use", _tool_call("clear_chat")),
                _response("end_turn", _text("Wiped!")),
                _response("end_turn", _text("Hi again.")),
            ])
            await backend.chat("my name is Mani")
            self.assertEqual(await backend.chat("clear the chat"), "Wiped!")
            self.assertEqual(len(backend._messages), 6)      # still intact until the reply is out
            await backend.after_reply()
            self.assertEqual(backend._messages, [])
            await backend.chat("hello")
            self.assertEqual(fake.requests[-1]["messages"][0]["content"], "hello")   # fresh start

    async def test_compact_on_request(self):
        from manikin.backend import SUMMARY_REQUEST
        with tempfile.TemporaryDirectory() as d:
            backend, fake = self.make_backend(d, [
                _response("tool_use", _tool_call("compact_chat")),
                _response("end_turn", _text("Compacting!")),
                _response("end_turn", _text("We talked about tea.")),      # the summary
                _response("end_turn", _text("Sure.")),
            ])
            await backend.chat("compact our chat please")
            await backend.after_reply()
            summary_request = fake.requests[2]
            self.assertEqual(summary_request["tool_choice"], {"type": "none"})
            self.assertEqual(summary_request["messages"][-1]["content"], SUMMARY_REQUEST)
            self.assertEqual(backend._messages, [])

            await backend.chat("what next?")
            first = fake.requests[-1]["messages"][0]["content"]
            self.assertIn("We talked about tea.", first)
            self.assertIn("what next?", first)

    async def test_auto_compact_when_huge(self):
        with tempfile.TemporaryDirectory() as d:
            backend, fake = self.make_backend(d, [
                _response("end_turn", _text("ok"), context=200_000),
                _response("end_turn", _text("summary")),
            ])
            await backend.chat("hi")
            await backend.after_reply()
            self.assertEqual(len(fake.requests), 2)
            self.assertEqual(backend._summary, "summary")

    async def test_no_compact_when_small(self):
        with tempfile.TemporaryDirectory() as d:
            backend, fake = self.make_backend(d, [_response("end_turn", _text("ok"))])
            await backend.chat("hi")
            await backend.after_reply()
            self.assertEqual(len(fake.requests), 1)
            self.assertEqual(len(backend._messages), 2)


class DisplayTests(unittest.TestCase):
    def test_split_mood(self):
        from manikin.protocol import split_mood
        self.assertEqual(split_mood("[happy] Hi!"), ("happy", "Hi!"))
        self.assertEqual(split_mood("  [Worried]\nUh oh"), ("worried", "Uh oh"))
        self.assertEqual(split_mood("[note] keep me"), (None, "[note] keep me"))   # unknown tag stays
        self.assertEqual(split_mood("no tag here"), (None, "no tag here"))
        # a second tag mid reply goes too, the leading one still wins
        self.assertEqual(split_mood("[surprised] Wait, that's ME! [love] You built me."),
                         ("surprised", "Wait, that's ME! You built me."))
        self.assertEqual(split_mood("on the right.\n[love] You built me"), ("love", "on the right.\nYou built me"))
        self.assertEqual(split_mood("Hi [happy] there [note]"), ("happy", "Hi there [note]"))

    def test_to_display(self):
        self.assertEqual(to_display("It’s “fine” — ok… \U0001F600"),
                         "It's \"fine\" - ok...")


class EndToEndTests(unittest.TestCase):
    """runs the real brain process with the echo backend like the overlay does"""

    def test_password_then_chat(self):
        with tempfile.TemporaryDirectory() as home:
            proc = subprocess.Popen(
                [sys.executable, "-m", "manikin", "--home", home, "--backend", "echo"],
                cwd=BRAIN_DIR, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")

            def send(**msg):
                proc.stdin.write(json.dumps(msg) + "\n")
                proc.stdin.flush()

            def read():
                return json.loads(proc.stdout.readline())

            try:
                # sound settings come first, before the password prompt
                self.assertEqual(read(), {"type": "sound", "volume": 30, "muted": False, "voice": True})
                self.assertEqual(read(), {"type": "password", "mode": "create", "error": None})
                send(type="password", text="hunter22")
                self.assertEqual(read()["mode"], "confirm")
                send(type="password", text="hunter22")
                self.assertEqual(read(), {"type": "unlocked", "ok": True})
                self.assertEqual(read(), {"type": "ready"})
                # he greets first on his own, no "thinking" before it
                self.assertEqual(read(), {"type": "say", "text": "(greeting)", "mood": "happy",
                                          "proactive": True})
                send(type="presence", pet_idle=1, pc_idle=0.5, typing=False, asleep=False)
                send(type="cancel")                         # nothing running, still gets an answer
                self.assertEqual(read(), {"type": "cancelled"})
                send(type="user", text="hello ’there’")
                self.assertEqual(read(), {"type": "thinking"})
                self.assertEqual(read(), {"type": "say", "text": "hello 'there'", "mood": None})
                send(type="user", text="[sad] the echo gets a face too")
                self.assertEqual(read(), {"type": "thinking"})
                self.assertEqual(read(), {"type": "say", "text": "the echo gets a face too", "mood": "sad"})
                send(type="event", name="woke", slept_s=600)
                self.assertEqual(read(), {"type": "say", "text": "(woke)", "mood": "happy",
                                          "proactive": True})
            finally:
                proc.stdin.close()
                proc.wait(timeout=10)
                proc.stdout.close()
            self.assertEqual(proc.returncode, 0)
            self.assertTrue((Path(home) / "data" / "notes.enc").exists())
            self.assertTrue((Path(home) / "data" / "permissions.json").exists())

    def test_chattiness_off_means_no_greeting(self):
        with tempfile.TemporaryDirectory() as home:
            (Path(home) / "data").mkdir()
            (Path(home) / "data" / "settings.json").write_text('{"chattiness": "off"}', encoding="utf-8")
            proc = subprocess.Popen(
                [sys.executable, "-m", "manikin", "--home", home, "--backend", "echo"],
                cwd=BRAIN_DIR, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")
            try:
                for msg in ({"type": "password", "skip": True}, {"type": "user", "text": "hi"}):
                    proc.stdin.write(json.dumps(msg) + "\n")
                proc.stdin.flush()
                lines = [json.loads(proc.stdout.readline()) for _ in range(6)]
            finally:
                proc.stdin.close()
                proc.wait(timeout=10)
                proc.stdout.close()
            self.assertEqual([m["type"] for m in lines],
                             ["sound", "password", "unlocked", "ready", "thinking", "say"])
            self.assertEqual(lines[-1]["text"], "hi")

    def test_sound_settings(self):
        """saved sound settings go out first, sound_set and the echo commands change them"""
        with tempfile.TemporaryDirectory() as home:
            data = Path(home) / "data"
            data.mkdir()
            (data / "settings.json").write_text(
                '{"chattiness": "off", "sound": {"volume": 25, "muted": false, "voice": true}}',
                encoding="utf-8")
            proc = subprocess.Popen(
                [sys.executable, "-m", "manikin", "--home", home, "--backend", "echo"],
                cwd=BRAIN_DIR, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")

            def send(**msg):
                proc.stdin.write(json.dumps(msg) + "\n")
                proc.stdin.flush()

            def read():
                return json.loads(proc.stdout.readline())

            def sound(volume, muted, voice):
                return {"type": "sound", "volume": volume, "muted": muted, "voice": voice}

            try:
                self.assertEqual(read(), sound(25, False, True))
                self.assertEqual(read()["type"], "password")
                send(type="sound_set", muted=True)          # works even at the password prompt
                self.assertEqual(read(), sound(25, True, True))
                send(type="password", skip=True)
                self.assertEqual(read()["type"], "unlocked")
                self.assertEqual(read(), {"type": "ready"})
                send(type="sound_set", muted="yes")         # malformed, ignored
                send(type="user", text="unmute")
                self.assertEqual(read(), {"type": "thinking"})   # no turn from either sound_set
                self.assertEqual(read(), sound(25, False, True))
                self.assertEqual(read()["text"], "Done: volume 25%, sound on, babble voice on.")
                for text, expected in (("volume 80", sound(80, False, True)),
                                       ("voice off", sound(80, False, False)),
                                       ("voice on", sound(80, False, True)),
                                       ("mute", sound(80, True, True))):
                    send(type="user", text=text)
                    self.assertEqual(read(), {"type": "thinking"})
                    self.assertEqual(read(), expected)
                    self.assertEqual(read()["type"], "say")
                send(type="user", text="volume 101")        # refused, nothing changes
                self.assertEqual(read(), {"type": "thinking"})
                self.assertEqual(read(), {"type": "say", "text": "volume must be from 0 to 100",
                                          "mood": "confused"})
            finally:
                proc.stdin.close()
                proc.wait(timeout=10)
                proc.stdout.close()
            self.assertEqual(proc.returncode, 0)
            self.assertEqual(json.loads((data / "settings.json").read_text(encoding="utf-8")),
                             {"chattiness": "off", "sound": {"volume": 80, "muted": True, "voice": True}})


if __name__ == "__main__":
    unittest.main()
