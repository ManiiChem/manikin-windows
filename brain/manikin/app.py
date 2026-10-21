import asyncio
import logging
import time
from collections import deque
from pathlib import Path
from typing import Any, Callable, Coroutine

from . import config
from .drive import Driver
from .gate import Gate
from .initiative import Event, Initiative, is_quiet
from .notes import MIN_PASSWORD, NoteVault
from .pointer import Pointer
from .protocol import Pipe, split_mood, to_display
from .reminders import Reminders
from .tools import ToolContext, build_tools

log = logging.getLogger("manikin")

WATCH_EVERY_S = 10.0


class Inbox:
    """FIFO of overlay messages and event turns (a deque so cancel and the recheck can filter it)"""

    def __init__(self):
        self._items: deque[dict] = deque()
        self._waiting = asyncio.Event()

    def put(self, msg: dict) -> None:
        self._items.append(msg)
        self._waiting.set()

    async def get(self) -> dict:
        while not self._items:
            self._waiting.clear()
            await self._waiting.wait()
        return self._items.popleft()

    def empty(self) -> bool:
        return not self._items

    def has_user(self) -> bool:
        return any(m.get("type") == "user" for m in self._items)

    def drop_users(self) -> int:
        kept = [m for m in self._items if m.get("type") != "user"]
        dropped = len(self._items) - len(kept)
        self._items = deque(kept)
        return dropped


class Brain:
    """routes overlay messages and runs turns one at a time. user messages and event turns share
    one inbox so they never overlap. answers, cancel, presence, wake ups, point_done, drive_done
    and sound_set skip it since they have to work mid turn"""

    def __init__(self, pipe: Pipe, gate: Gate, initiative: Initiative,
                 clock: Callable[[], float] = time.time, pointer: Pointer | None = None,
                 settings: config.Settings | None = None, driver: Driver | None = None):
        self.pipe = pipe
        self.gate = gate
        self.initiative = initiative
        self.clock = clock
        self.pointer = pointer
        self.driver = driver
        self.settings = settings
        self.inbox = Inbox()
        self.backend = None             # set by start() once memory is sorted out
        self._turn: asyncio.Task | None = None

    @property
    def busy(self) -> bool:
        return (self._turn is not None and not self._turn.done()) or not self.inbox.empty()

    def on_message(self, msg: dict) -> None:
        kind = msg.get("type")
        if kind == "answer":
            self.gate.answer(msg.get("id"), msg.get("allow"), msg.get("scope"))
        elif kind == "point_done":
            if self.pointer is not None:
                self.pointer.on_done(msg)
        elif kind == "drive_done":
            if self.driver is not None:
                self.driver.on_done(msg)
        elif kind == "cancel":
            self.cancel()
        elif kind == "presence":
            self._presence(msg)
        elif kind == "event":
            self._event(msg)
        elif kind == "sound_set":
            self._sound_set(msg)
        else:
            if kind == "user" and self.pointer is not None:
                self.pointer.interrupt()     # or a turn waiting on a click holds this for minutes
            self.inbox.put(msg)

    def start(self, backend) -> None:
        """memory is unlocked (or skipped) and "ready" went out, turns can run now"""
        self.backend = backend
        if event := self.initiative.greeting(self.clock()):
            self._enqueue(event)

    def cancel(self) -> None:
        # asks first, once the task is cancelled check() forgets its pending ask before we can hide it
        self.gate.cancel_all()
        if self.pointer is not None:
            self.pointer.cancel()
        if self.driver is not None:
            self.driver.cancel()
        dropped = self.inbox.drop_users()
        turn = self._turn
        running = turn is not None and not turn.done()
        log.info("cancel: turn %s, dropped %d queued message(s)",
                 "running" if running else "not running", dropped)
        if not running:
            self.pipe.send("cancelled")
            return
        if turn.cancelling():
            return                       # Esc pressed twice, still one "cancelled" per turn
        turn.cancel()
        # sent once the turn unwinds so the history is already rolled back
        turn.add_done_callback(lambda _: self.pipe.send("cancelled"))

    async def serve(self) -> None:
        """runs turns until the overlay closes the pipe"""
        while True:
            msg = await _next(self.pipe, self.inbox)
            if msg is None:
                return
            if msg.get("type") == "user" and isinstance(msg.get("text"), str):
                await self._run(self._user_turn(msg["text"]))
            elif msg.get("type") == "_event" and isinstance(event := msg.get("event"), Event):
                if self.initiative.still_wanted(event, self.inbox.has_user()):
                    await self._run(self._event_turn(event))

    async def watch(self, watcher, every: float = WATCH_EVERY_S) -> None:
        """polls the PC watcher, an observation becomes an event turn if the user is around"""
        while True:
            await asyncio.sleep(every)
            try:
                observations = await asyncio.to_thread(watcher.poll)
            except Exception:
                log.exception("watcher poll failed")
                continue
            for obs in observations:
                if event := self.initiative.observation(obs, self.clock(), self.busy):
                    self._enqueue(event)

    def _presence(self, msg: dict) -> None:
        now = self.clock()
        self.initiative.update_presence(msg, now)
        if self.backend is not None:
            if event := self.initiative.quiet_check(now, self.busy):
                self._enqueue(event)

    def _event(self, msg: dict) -> None:
        if msg.get("name") != "woke" or self.backend is None:
            return
        slept = msg.get("slept_s")
        slept = float(slept) if isinstance(slept, (int, float)) and not isinstance(slept, bool) else 0.0
        if event := self.initiative.woke(slept, self.clock()):
            self._enqueue(event)

    def _sound_set(self, msg: dict) -> None:
        """Ctrl+M in the overlay, save it (the settings hook echoes "sound" back)"""
        muted = msg.get("muted")
        if not isinstance(muted, bool):
            log.warning("ignoring malformed sound_set: %r", msg)
            return
        if self.settings is None:
            return
        try:
            self.settings.set_sound(muted=muted)
        except OSError:
            log.exception("couldn't save the sound settings")

    def _enqueue(self, event: Event) -> None:
        self.inbox.put({"type": "_event", "event": event})

    async def _run(self, turn: Coroutine[Any, Any, bool]) -> None:
        # a task so cancel() can stop it, asyncio.wait so its CancelledError doesnt look like ours
        self._turn = asyncio.create_task(turn)
        await asyncio.wait({self._turn})
        if self._turn.cancelled():
            log.info("turn cancelled")
            return
        if self._turn.result():
            try:
                await self.backend.after_reply()     # clear/compact while you read the bubble
            except Exception:
                log.exception("after_reply failed")
                self.pipe.send("error", text="I tried to tidy up our chat but something went wrong.")

    async def _user_turn(self, text: str) -> bool:
        self.pipe.send("thinking")
        try:
            reply = await self.backend.chat(text)
        except Exception as e:
            log.exception("reply failed")
            from .backend import describe_api_error
            self.pipe.send("error", text=describe_api_error(e))
            return False
        mood, reply = split_mood(reply)
        self.pipe.send("say", text=to_display(reply), mood=mood)
        return True

    async def _event_turn(self, event: Event) -> bool:
        # no "thinking" here, a bubble out of nowhere that might end in [quiet] would be kinda weird
        try:
            reply = await self.backend.chat(event.prompt, proactive=True, kind=event.kind)
        except Exception:
            log.exception("%s remark failed", event.kind)     # he wasnt asked so no error bubble
            return False
        mood, text = split_mood(reply)
        text = to_display(text)
        if is_quiet(text):
            log.info("initiative: %s stayed quiet", event.kind)
        else:
            self.pipe.send("say", text=text, mood=mood, proactive=True)
            self.initiative.said(event, self.clock())
        return True


async def run(home: Path, backend_name: str, watcher=None) -> None:
    paths = config.Paths(home)
    paths.ensure()
    logging.basicConfig(filename=paths.log, level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    log.info("brain starting, home=%s backend=%s model=%s", home, backend_name, config.MODEL)

    loop = asyncio.get_running_loop()
    settings = config.Settings(paths.settings)
    # the lambda only looks up brain when a message comes in, which is after pipe.start()
    pipe = Pipe(loop, lambda msg: brain.on_message(msg))
    # sound settings go out first, then again after every change
    settings.on_sound_change = lambda sound: pipe.send("sound", **sound)
    pipe.send("sound", **settings.sound)
    gate = Gate(pipe, config.load_permissions(paths), config.ASK_TIMEOUT_S)
    pointer = Pointer(pipe)
    driver = Driver(pipe)
    brain = Brain(pipe, gate, Initiative(settings), pointer=pointer, settings=settings, driver=driver)
    pipe.start()

    def fire(reminder: dict) -> None:
        pipe.send("remind", text=to_display(f"Reminder: {reminder['text']}"))

    reminders = Reminders(paths.reminders, fire)
    missed = reminders.take_missed()
    tasks = [asyncio.create_task(reminders.run())]

    vault = NoteVault(paths.notes)
    if not await _password_phase(pipe, brain.inbox, vault):
        return

    ctx = ToolContext(paths.sandbox, vault, reminders, settings=settings, pointer=pointer,
                      driver=driver, gate=gate)
    backend = _make_backend(backend_name, pipe, gate, ctx)
    pipe.send("ready")
    for r in missed:
        late = _ago(time.time() - r["due"])
        pipe.send("remind", text=to_display(f"You missed a reminder while I was off ({late} ago): {r['text']}"))
    brain.start(backend)                         # queues the greeting

    watcher = watcher or _make_watcher()
    if watcher is not None:
        tasks.append(asyncio.create_task(brain.watch(watcher)))

    await brain.serve()
    for task in tasks:
        task.cancel()
    log.info("overlay closed the pipe, exiting")


def _make_watcher():
    """the PC watcher, or None if its missing or wont start (its a bonus anyway)"""
    try:
        from .watcher import Watcher
    except ImportError:
        log.info("no PC watcher module; no remarks about the PC this session")
        return None
    try:
        return Watcher()
    except Exception:
        log.exception("PC watcher unavailable; no remarks about the PC this session")
        return None


async def _next(pipe: Pipe, inbox: Inbox) -> dict | None:
    """next message from the overlay, or None once its closed the pipe"""
    if pipe.closed.is_set() and inbox.empty():
        return None
    get = asyncio.ensure_future(inbox.get())
    closed = asyncio.ensure_future(pipe.closed.wait())
    done, _ = await asyncio.wait({get, closed}, return_when=asyncio.FIRST_COMPLETED)
    if get in done:
        closed.cancel()
        return get.result()
    get.cancel()
    return None


async def _ask_password(pipe: Pipe, inbox: Inbox, mode: str, error: str | None) -> str | None | bool:
    """asks once, returns the password, None if they skipped, False if the overlay closed"""
    pipe.send("password", mode=mode, error=error)
    while True:
        msg = await _next(pipe, inbox)
        if msg is None:
            return False
        if msg.get("type") != "password":
            continue                     # chat is off while the password prompt is up
        if msg.get("skip") is True:
            return None
        if isinstance(msg.get("text"), str):
            return msg["text"]


async def _password_phase(pipe: Pipe, inbox: Inbox, vault: NoteVault) -> bool:
    """unlock (or create) the notes vault, False if the overlay went away"""
    error = None
    while True:
        if vault.exists:
            password = await _ask_password(pipe, inbox, "unlock", error)
            if password is False:
                return False
            if password is not None:
                try:
                    ok = await asyncio.to_thread(vault.unlock, password)
                except Exception:
                    log.exception("couldn't read %s", vault.path)
                    pipe.send("error", text="My notes file looks damaged, so memory stays off. See data/brain.log.")
                    ok = True       # stop asking, the vault just stays locked
                if not ok:
                    error = "Wrong password. Try again?"
                    continue
        else:
            password = await _ask_password(pipe, inbox, "create", error)
            if password is False:
                return False
            if password is not None:
                if len(password) < MIN_PASSWORD:
                    error = f"Too short, use at least {MIN_PASSWORD} characters."
                    continue
                again = await _ask_password(pipe, inbox, "confirm", None)
                if again is False:
                    return False
                if again != password:
                    error = "Those didn't match. Let's start over."
                    continue
                try:
                    await asyncio.to_thread(vault.create, password)
                except Exception:
                    log.exception("couldn't create %s", vault.path)
                    pipe.send("error", text="I couldn't save my notes file, so memory stays off this time. "
                                            "See data/brain.log.")
        pipe.send("unlocked", ok=vault.unlocked)
        log.info("memory %s", "unlocked" if vault.unlocked else "skipped")
        return True


def _make_backend(name: str, pipe: Pipe, gate: Gate, ctx: ToolContext):
    from .backend import ClaudeBackend, EchoBackend
    if name == "echo":
        return EchoBackend(ctx.pointer, build_tools(ctx), driver=ctx.driver, gate=gate)
    if not ctx.vault.unlocked:
        memory = "Your long-term memory is locked this session (the user skipped the password)."
    elif notes := ctx.vault.all():
        memory = "Things you remember about the user (from your encrypted notes):\n" + \
                 "\n".join(f"- [{n['id']}] {n['text']}" for n in notes)
    else:
        memory = "Your long-term memory is empty so far."
    return ClaudeBackend(pipe, gate, build_tools(ctx), memory, ctx.chat)


def _ago(seconds: float) -> str:
    minutes = int(seconds // 60)
    if minutes < 60:
        return f"{max(minutes, 1)} min"
    hours = minutes // 60
    return f"{hours} h {minutes % 60} min" if hours < 24 else f"{hours // 24} days"
