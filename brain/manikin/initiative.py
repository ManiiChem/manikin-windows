"""when Manikin talks first. no I/O in here, the app feeds in presence updates, wake ups and
watcher observations and gets back Events to queue as turns. the clock and rng get passed in
so tests can replay any schedule exactly. every remark but the startup greeting sticks to the
chattiness levels hourly cap and min gap, and only remarks he actually said count ("[quiet]" doesnt)
"""

import logging
import random
import re
from collections import deque
from dataclasses import dataclass
from datetime import datetime

log = logging.getLogger(__name__)

PREFIX = "[Event, not typed by the user]"
ASK_QUIET = "Say one short line in character, or reply with exactly [quiet] to stay silent."
AWAY_AFTER_S = 120          # pc_idle at or above this means the user stepped away
HOUR = 3600.0


@dataclass(frozen=True)
class Level:
    per_hour: int                       # most remarks in any rolling hour
    gap_s: float                        # min time between two remarks
    quiet_wait_s: tuple[float, float]   # the quiet stretch wait gets picked from this range


LEVELS = {
    "often": Level(6, 5 * 60, (8 * 60, 15 * 60)),
    "sometimes": Level(3, 12 * 60, (15 * 60, 25 * 60)),
    "rarely": Level(1, 30 * 60, (30 * 60, 45 * 60)),
    "off": Level(0, 0, (0, 0)),
}


@dataclass(frozen=True)
class Event:
    kind: str      # "greeting" | "woke" | "quiet" | an Observation kind ("cpu_high", ...)
    prompt: str    # user role text for backend.chat(prompt, proactive=True)


@dataclass
class Presence:
    pet_idle: float = 0.0     # secs since the user last touched Manikin
    pc_idle: float = -1.0     # system wide idle secs, -1 = unknown
    typing: bool = False      # the chat box has text in it
    asleep: bool = False
    at: float = 0.0           # when we got it, to age pet_idle between updates

    @property
    def present(self) -> bool:
        return self.pc_idle < AWAY_AFTER_S     # unknown (-1) counts as present


_QUIET_IDEAS = (
    "Make a short offhand remark, the kind a friend sitting nearby might make.",
    "Ask the user one light question about themselves or what they're up to.",
    "Share one short fun fact, ideally about something the user is into (check your notes "
    "and our conversation), otherwise about computers or retro tech.",
    "Say something that shows you remember a detail about the user from your notes or "
    "from earlier in our conversation.",
)

_OBSERVATION_HINTS = {
    "cpu_high": "If it fits, tease or sympathise about the busy processor.",
    "ram_high": "If it fits, mention the memory squeeze; maybe suggest closing something.",
    "gpu_high": "If it fits, react to the graphics card working hard.",
    "battery_low": "A gentle nudge to plug in is welcome here.",
    "late_night": "If it fits, gently suggest getting some rest, without nagging.",
}


def is_quiet(text: str) -> bool:
    """True for a reply that means say nothing, so empty, "..." or [quiet] (mood already split off)"""
    t = text.strip().lower()
    return t in ("", "...") or re.fullmatch(r"\[quiet\][\s.!]*", t) is not None


def duration(seconds: float) -> str:
    minutes = round(seconds / 60)
    if minutes < 1:
        return "less than a minute"
    if minutes < 60:
        return f"about {minutes} minute{'s' if minutes != 1 else ''}"
    hours, rest = divmod(minutes, 60)
    text = f"about {hours} hour{'s' if hours != 1 else ''}"
    return f"{text} and {rest} minutes" if rest >= 5 else text


def _part_of_day(hour: int) -> str:
    if 5 <= hour < 12:
        return "morning"
    if 12 <= hour < 17:
        return "afternoon"
    if 17 <= hour < 22:
        return "evening"
    return "night"


class Initiative:
    def __init__(self, settings, rng: random.Random | None = None):
        self._settings = settings                # anything with a .chattiness str
        self._rng = rng or random.Random()
        self.presence = Presence()
        self._said: deque[float] = deque()       # when remarks were said, for the cap
        self._greeted = False
        self._quiet_since: float | None = None   # quiet stretch clock restarts here
        self._quiet_wait: float | None = None
        self._quiet_level: str | None = None     # level the wait was drawn for
        self._idea = -1
        self._tried: dict[str, float] = {}       # observation kind -> last attempt

    @property
    def level(self) -> Level:
        return LEVELS.get(self._settings.chattiness, LEVELS["often"])

    def update_presence(self, msg: dict, now: float) -> None:
        def num(key: str, default: float) -> float:
            v = msg.get(key)
            return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else default
        self.presence = Presence(num("pet_idle", 0.0), num("pc_idle", -1.0),
                                 msg.get("typing") is True, msg.get("asleep") is True, now)

    def greeting(self, now: float) -> Event | None:
        """once per run right after startup, ignores the cap and gap"""
        if self._greeted:
            return None
        self._greeted = True
        if self._settings.chattiness == "off":
            log.info("initiative: greeting skipped (chattiness off)")
            return None
        t = datetime.fromtimestamp(now)
        log.info("initiative: greeting fired")
        return Event("greeting", f"{PREFIX} It's {t:%H:%M} on a {t:%A} {_part_of_day(t.hour)}. "
                     "The user just started you up, so you're waking into a new session. Greet "
                     "them, fitting the time of day; if you remember something about them, you can "
                     f"use it. {ASK_QUIET}")

    def woke(self, slept_s: float, now: float) -> Event | None:
        if reason := self._blocked(now):
            log.info("initiative: woke skipped (%s)", reason)
            return None
        t = datetime.fromtimestamp(now)
        log.info("initiative: woke fired (slept %.0f s)", slept_s)
        return Event("woke", f"{PREFIX} It's {t:%H:%M}. You just woke up from a nap "
                     f"({duration(slept_s)}) because the user poked you. {ASK_QUIET}")

    def quiet_check(self, now: float, busy: bool) -> Event | None:
        """a remark after a quiet stretch, called on every presence update. the stretch counts from
        the users last touch or his own last try (whichever is later) so he doesnt fire again every
        min gap while being ignored, and a [quiet] reply doesnt get retried 5 s later"""
        p = self.presence
        if self._settings.chattiness == "off":
            return None
        if self._quiet_level != self._settings.chattiness:
            self._reroll()
        idle = p.pet_idle + max(0.0, now - p.at)
        stretch = idle if self._quiet_since is None else min(idle, now - self._quiet_since)
        if stretch < self._quiet_wait:
            return None          # the common case, not logged or itd flood the log every 5 s
        why = ("busy with a turn" if busy else "user away" if not p.present
               else "user typing" if p.typing else self._blocked(now))
        if why:
            log.info("initiative: quiet remark skipped (%s; quiet %.0f s, asleep=%s)",
                     why, stretch, p.asleep)
            return None
        self._quiet_since = now
        self._reroll()
        self._idea = (self._idea + 1 + self._rng.randrange(len(_QUIET_IDEAS) - 1)) % len(_QUIET_IDEAS)
        t = datetime.fromtimestamp(now)
        log.info("initiative: quiet remark fired (quiet %.0f s, asleep=%s, next wait %.0f s)",
                 stretch, p.asleep, self._quiet_wait)
        return Event("quiet", f"{PREFIX} It's {t:%H:%M}. The user is at the computer but hasn't "
                     f"talked to you for {duration(idle)}. {_QUIET_IDEAS[self._idea]} "
                     f"{ASK_QUIET}")

    def observation(self, obs, now: float, busy: bool) -> Event | None:
        """something the watcher noticed on the PC (obs has .kind and .text)"""
        last = self._tried.get(obs.kind)
        why = ("busy with a turn" if busy else "user away" if not self.presence.present
               else self._blocked(now) or (
                   "same kind tried recently" if last is not None and now - last < self.level.gap_s
                   else None))
        if why:
            log.info("initiative: %s skipped (%s)", obs.kind, why)
            return None
        # even if he answers [quiet] dont ask about the same thing again right away
        self._tried[obs.kind] = now
        t = datetime.fromtimestamp(now)
        log.info("initiative: %s fired: %s", obs.kind, obs.text)
        hint = _OBSERVATION_HINTS.get(obs.kind, "")
        return Event(obs.kind, f"{PREFIX} It's {t:%H:%M}. You noticed this on the user's PC: "
                     f"{obs.text}. {hint} {ASK_QUIET}".replace("  ", " "))

    def still_wanted(self, event: Event, user_waiting: bool) -> bool:
        """rechecked right before a queued event turn runs, user messages win"""
        why = ("user message waiting" if user_waiting else "user typing" if self.presence.typing
               else "chattiness off" if self._settings.chattiness == "off" else None)
        if why:
            log.info("initiative: %s dropped before running (%s)", event.kind, why)
        return why is None

    def said(self, event: Event, now: float) -> None:
        """he actually said it so it counts toward the cap and gap (the greeting doesnt)"""
        if event.kind != "greeting":
            self._said.append(now)
        log.info("initiative: %s said", event.kind)

    def _blocked(self, now: float) -> str | None:
        """why the cap or gap blocks a remark right now, or None"""
        level = self.level
        if level.per_hour == 0:
            return "chattiness off"
        while self._said and now - self._said[0] >= HOUR:
            self._said.popleft()
        if len(self._said) >= level.per_hour:
            return f"hourly cap of {level.per_hour} reached"
        if self._said and now - self._said[-1] < level.gap_s:
            return f"min gap {level.gap_s / 60:.0f} min not over"
        return None

    def _reroll(self) -> None:
        self._quiet_level = self._settings.chattiness
        lo, hi = self.level.quiet_wait_s
        self._quiet_wait = self._rng.uniform(lo, hi)
