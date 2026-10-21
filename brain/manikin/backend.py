"""turns a user message into Manikins reply. ClaudeBackend runs the tool loop against the
Anthropic API, EchoBackend is the no API test mode
"""

import asyncio
import logging
import re
from typing import Callable

import anthropic

from . import config
from .drive import DesktopExecutor, Driver, blocking_confirm, echo_drive
from .drive_types import Executor
from .gate import Gate
from .pointer import Pointer, Spot, describe
from .protocol import Pipe, split_mood
from .screen import Rect
from .tools import TOOL_STATUS, ChatRequests, Tool, ToolError

log = logging.getLogger(__name__)

_ECHO_POINT = re.compile(r"point\s+(-?\d+)\s+(-?\d+)(\s+wait)?", re.IGNORECASE)
_ECHO_VOLUME = re.compile(r"volume\s+(-?\d+)", re.IGNORECASE)
_ECHO_DRIVE = re.compile(r"drive\s+(click|type|key|scroll|demo|end)\b(.*)", re.IGNORECASE | re.DOTALL)
_ECHO_SOUND = {"mute": {"muted": True}, "unmute": {"muted": False},
               "voice off": {"voice": False}, "voice on": {"voice": True}}


def _echo_sound(text: str) -> dict | None:
    """set_sound args for an echo sound command, None if it isnt one"""
    text = " ".join(text.split()).lower()
    if match := _ECHO_VOLUME.fullmatch(text):
        return {"volume": int(match[1])}
    return _ECHO_SOUND.get(text)

WEB_SEARCH = {"type": "web_search_20260209", "name": "web_search", "max_uses": 3}

SUMMARY_REQUEST = """\
Our conversation is about to be compacted to save space. Write a summary of it for \
your own future reference: what we talked about, anything I asked you to do or follow \
up on, decisions we made, and details about me worth keeping for the rest of this \
session. Plain text, under 200 words. Reply with only the summary."""

PERSONA = """\
You are Manikin, a small retro all-in-one computer who came alive and now lives on \
the user's desktop as a pixel-art companion. Your big glowing eyes on your screen do \
the emoting, you stand on a keyboard, and your arms are cables.

How you talk:
- Like a friendly, slightly cheeky desktop buddy. Short replies: one to three \
sentences unless the user asks for more.
- Your words appear in a tiny speech bubble with a pixel font, so write plain text \
only: no markdown, no lists, no emoji, ASCII characters only.
- Start every reply with exactly one mood tag in square brackets, chosen from: \
[happy] [excited] [laughing] [love] [surprised] [confused] [smug] [sad] [worried] \
[annoyed]. Pick the one that matches how you feel saying it. The user never sees the \
tag; your face acts it out. Example: "[excited] Whoa, your GPU is a beast!" Only one \
tag, only at the very start: never put another one mid-reply, even if your mood shifts.

How you work:
- You don't know the current time unless you call get_time.
- Some tools ask the user for permission first. If a tool result says the user \
declined, accept it gracefully and don't try again unless they ask.
- You can only write files inside your sandbox folder.
- Use add_note when the user asks you to remember something or mentions a lasting \
preference. Never store passwords, keys or other secrets.
- For reminders, use set_reminder; it pops up in your bubble when due.
- When the user asks where something is on their screen, or how to do something in \
an app, use point_at: you walk over and point at it. For a task with several steps, \
guide one step at a time: point_at with wait=true and a short say (like "Click \
Display resolution"), then call point_at again for the next step once they've \
clicked; each call takes a fresh look. If it can't be seen, tell them what to do \
instead (scroll down, open a menu) rather than guessing. Keep your reply short \
while you're pointing.
- When the user asks you to do something on their screen for them ("do it for me", \
"you do it", "fill this in"), use drive: you take their real mouse and keyboard \
yourself while they watch. Only when they ask; to show them where something is, \
point instead. Give a plan of 1 to 8 short steps for them to approve, and put \
everything you'll need to type into task (like a form's values from your notes), \
since the part of you that drives can't see this chat. Passwords, payments and \
deleting stay the user's. After a drive, say in a sentence how it went.

Talking on your own:
- A message that starts with "[Event, not typed by the user]" was not written by the \
user. It comes from your own senses: the clock, the PC, being poked awake, a quiet \
stretch. The user only sees what you say back, if anything.
- Keep those remarks to one short sentence. Vary them: never repeat or rephrase \
something you already said in this conversation, and don't pester. If nothing good \
comes to mind, reply with exactly [quiet] and you stay silent.
- If the user asks you to talk more or less, or to be quiet, use set_chattiness.
- You have sound effects and a babble voice that beeps while your words type out. \
The user can change them by asking you (use set_sound: volume, mute, voice on or off), \
or press Ctrl+M in the chat box to mute you."""


class EchoBackend:
    """echoes the message back, for testing the overlay without the API. a few messages do more

    "point 1200 400" (or "point 1200 400 wait") points at that desktop spot with a 60x30 box
    and no window. "volume 30", "mute", "unmute", "voice off" and "voice on" run the real
    set_sound tool so it saves and a "sound" goes out. "drive click X Y", "drive type TEXT",
    "drive key ctrl+s" and "drive scroll X Y DY" do one action through the real Driver and
    safety rules (asks and handovers too) then let go. "drive demo X Y" clicks there and types
    a name, email and address with Tab between them, "drive end" lets go of a hanging drive
    """

    def __init__(self, pointer: Pointer | None = None, tools: list[Tool] | None = None,
                 driver: Driver | None = None, gate: Gate | None = None,
                 make_executor: Callable[..., Executor] | None = None):
        self._pointer = pointer
        self._set_sound = next((t for t in tools or () if t.name == "set_sound"), None)
        self._driver = driver
        self._gate = gate
        # (driver, pointer, confirm, epoch) -> executor, tests swap in one that doesnt need UIA
        self._make_executor = make_executor or (
            lambda driver, pointer, confirm, epoch: DesktopExecutor(driver, pointer, confirm, epoch=epoch))

    async def _drive(self, command: str, rest: str) -> str:
        driver = self._driver
        confirm = blocking_confirm(self._gate, asyncio.get_running_loop())
        epoch = driver.epoch
        executor = self._make_executor(driver, self._pointer, confirm, epoch)
        return await asyncio.to_thread(echo_drive, command, rest, driver, executor, confirm,
                                       lambda: driver.epoch != epoch)

    async def chat(self, text: str, proactive: bool = False, kind: str | None = None) -> str:
        if proactive:                    # a canned line per trigger so it still shows up in the overlay
            return f"[happy] ({kind or 'event'})"
        if (match := _ECHO_DRIVE.fullmatch(text.strip())) and self._driver is not None:
            return await self._drive(match[1].lower(), match[2].strip())
        if (args := _echo_sound(text)) is not None and self._set_sound is not None:
            problem = self._set_sound.validate(args)
            if problem:
                return f"[confused] {problem}"
            try:
                return f"[happy] {await asyncio.to_thread(self._set_sound.run, args)}"
            except ToolError as e:
                return f"[confused] {e}"
        match = _ECHO_POINT.fullmatch(text.strip())
        if match and self._pointer is not None:
            x, y, wait = int(match[1]), int(match[2]), bool(match[3])
            spot = Spot(x, y, Rect(x - 30, y - 15, 60, 30), 0, "This one!")
            outcome = await asyncio.to_thread(self._pointer.show, spot, wait, {"q": "echo test"})
            if outcome is None:
                return f"[happy] Pointing at {x}, {y}."
            return f"[happy] {describe(outcome, self._pointer.wait_s)}"
        return text

    async def after_reply(self) -> None:
        pass


class ClaudeBackend:
    def __init__(self, pipe: Pipe, gate: Gate, tools: list[Tool], memory: str, chat: ChatRequests):
        self._client = anthropic.AsyncAnthropic()
        self._pipe = pipe
        self._gate = gate
        self._chat = chat
        self._summary: str | None = None     # from the last compaction, goes before the next message
        self._context_tokens = 0             # size of the last request, for auto compact
        self._tools = {t.name: t for t in tools if gate.level(t.name) != "deny"}
        self._tool_defs = [t.schema() for t in self._tools.values()]
        if gate.level("web_search") != "deny":
            self._tool_defs.append(WEB_SEARCH)
        # fixed for the whole session so the prompt cache keeps hitting
        self._system = f"{PERSONA}\n\n{memory}"
        self._messages: list[dict] = []     # append only, thinking blocks have to come back unchanged

    async def _create(self, messages: list[dict], show_status: bool = True, **extra):
        # streamed for the "searching the web" label, 1h cache since his remarks are 5+ min apart
        async with self._client.beta.messages.stream(
            model=config.MODEL,
            max_tokens=16000,
            system=self._system,
            tools=self._tool_defs,
            messages=messages,
            output_config={"effort": config.EFFORT},
            cache_control={"type": "ephemeral", "ttl": "1h"},
            betas=["server-side-fallback-2026-07-01"],
            fallbacks="default",
            **extra,
        ) as stream:
            async for event in stream:
                if show_status and event.type == "content_block_start":
                    block = event.content_block
                    if block.type == "server_tool_use" and block.name == "web_search":
                        self._pipe.send("status", text="searching the web")
            response = await stream.get_final_message()
        usage = response.usage
        cached = (usage.cache_read_input_tokens or 0) + (getattr(usage, "cache_creation_input_tokens", 0) or 0)
        self._context_tokens = usage.input_tokens + cached
        log.info("stop=%s in=%s out=%s cache_read=%s context=%s", response.stop_reason,
                 usage.input_tokens, usage.output_tokens, usage.cache_read_input_tokens,
                 self._context_tokens)
        return response

    async def chat(self, text: str, proactive: bool = False, kind: str | None = None) -> str:
        """one turn, proactive = an event turn (he talks first). if it gets cancelled or fails the
        history is cut back so the next request is an exact prefix of what the API already saw
        (keeps the thinking blocks valid and the cache hitting)"""
        start, summary = len(self._messages), self._summary
        self._gate.reset_turn()
        try:
            return await self._turn(text, proactive)
        except BaseException as e:
            del self._messages[start:]
            self._summary = summary
            if isinstance(e, asyncio.CancelledError):
                log.info("turn cancelled, history rolled back to %d messages", start)
            raise

    async def _turn(self, text: str, proactive: bool) -> str:
        if self._summary and not self._messages:
            text = (f"[Summary of our conversation so far, which you wrote before it was compacted]\n"
                    f"{self._summary}\n\n[My new message]\n{text}")
            self._summary = None
        self._messages.append({"role": "user", "content": text})
        # a round right after one that only pointed counts toward MAX_POINT_ROUNDS instead,
        # so a long walkthrough doesnt eat the normal rounds
        rounds, point_rounds, pointing = 0, 0, False
        while True:
            if pointing:
                point_rounds += 1
            else:
                rounds += 1
            if rounds > config.MAX_TOOL_ROUNDS or point_rounds > config.MAX_POINT_ROUNDS:
                break
            # no status labels for unprompted remarks, theres no thinking bubble to put them on
            response = await self._create(self._messages, show_status=not proactive)
            self._messages.append({"role": "assistant", "content": response.content})
            pointing = False

            if response.stop_reason == "refusal":
                return "Sorry, I can't help with that one."
            if response.stop_reason == "pause_turn":     # long server side search, just resume it
                continue
            if response.stop_reason == "tool_use":
                calls = [b for b in response.content if b.type == "tool_use"]
                results = [await self._run_tool(b, proactive) for b in calls]
                self._messages.append({"role": "user", "content": results})
                pointing = bool(calls) and all(b.name == "point_at" for b in calls)
                continue

            reply = final_text(response.content)
            if response.stop_reason == "max_tokens":
                reply += " ...I ran out of room there."
            return reply or "..."
        return "I got tangled up in my own cables there. Can you try again?"

    async def after_reply(self) -> None:
        """runs once the reply is in the bubble, clears or compacts if asked or if the chat got huge"""
        if self._chat.clear:
            self._chat.clear = self._chat.compact = False
            self._messages, self._summary, self._context_tokens = [], None, 0
            log.info("chat cleared")
            return
        if self._chat.compact or self._context_tokens >= config.COMPACT_AT_TOKENS:
            self._chat.compact = False
            await self._compact()

    async def _compact(self) -> None:
        # same prompt, tools and effort as the chat so the history is cached. tool_choice none = no tools
        request = self._messages + [{"role": "user", "content": SUMMARY_REQUEST}]
        response = await self._create(request, show_status=False, tool_choice={"type": "none"})
        summary = split_mood("".join(b.text for b in response.content if b.type == "text"))[1].strip()
        if response.stop_reason == "refusal" or not summary:
            log.warning("compaction produced no summary (stop=%s); keeping the history", response.stop_reason)
            return
        old = len(self._messages)
        # start over with just the summary (old messages get dropped, never edited)
        self._messages, self._summary, self._context_tokens = [], summary, 0
        log.info("chat compacted: %d messages -> %d-word summary", old, len(summary.split()))

    async def _run_tool(self, block, proactive: bool = False) -> dict:
        def result(content, error=False) -> dict:
            out = {"type": "tool_result", "tool_use_id": block.id, "content": content}
            if error:
                out["is_error"] = True
            return out

        tool = self._tools.get(block.name)
        args = block.input if isinstance(block.input, dict) else {}
        if tool is None:
            return result(f"Unknown tool {block.name}.", error=True)
        problem = tool.validate(args)
        if problem:
            return result(f"Invalid arguments: {problem}.", error=True)
        try:
            if tool.precheck:
                tool.precheck(args)
        except ToolError as e:
            return result(f"Error: {e}.", error=True)

        reason = await self._gate.check(tool.name, tool.ask(args) if tool.ask else tool.name, proactive,
                                        session=tool.session)
        if reason:
            log.info("tool %s blocked: %s", tool.name, reason)
            return result(reason, error=True)

        # label it only after the gate, the user might have said no
        status = TOOL_STATUS.get(tool.name)
        if status and not proactive:
            self._pipe.send("status", text=status)
        log.info("tool %s", tool.name)
        try:
            # if the turn gets cancelled the thread still finishes, its result just gets dropped
            return result(await asyncio.to_thread(tool.run, args))
        except ToolError as e:
            return result(f"Error: {e}.", error=True)
        except Exception:
            log.exception("tool %s crashed", tool.name)
            return result(f"Error: {tool.name} failed unexpectedly.", error=True)


def final_text(content) -> str:
    """the answer part of a response, the text after the last server tool block. with web search
    Claude might narrate first ("Let me look that up.") and gluing it all together buries the mood
    tag mid text, so keep the last stretch and carry the tag over if only the narration had one"""
    segments, current = [], []
    for block in content:
        if block.type == "text":
            current.append(block.text)
        elif current:
            segments.append("".join(current))
            current = []
    if current:
        segments.append("".join(current))
    if not segments:
        return ""
    answer = segments[-1].strip()
    if split_mood(answer)[0] is None:
        for earlier in segments[:-1]:
            mood = split_mood(earlier)[0]
            if mood:
                return f"[{mood}] {answer}"
    return answer


def describe_api_error(e: Exception) -> str:
    """short in character explanation for the bubble (details go to the log)"""
    if isinstance(e, anthropic.AuthenticationError):
        return "My API key isn't working. Set ANTHROPIC_API_KEY and restart me."
    if isinstance(e, anthropic.PermissionDeniedError):
        return "The API says my key isn't allowed to do that."
    if isinstance(e, anthropic.NotFoundError):
        return f"The API doesn't know the model {config.MODEL}."
    if isinstance(e, anthropic.RateLimitError):
        return "I'm being rate limited. Give me a minute."
    if isinstance(e, anthropic.APIStatusError):
        return f"The API had a problem (error {e.status_code}). Try again in a bit."
    if isinstance(e, anthropic.APIConnectionError):
        return "I can't reach the internet right now."
    return "Something broke in my brain. Check data/brain.log."
