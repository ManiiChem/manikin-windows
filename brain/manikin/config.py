import json
import logging
import os
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

log = logging.getLogger(__name__)

MODEL = os.environ.get("MANIKIN_MODEL", "claude-opus-5-5")
EFFORT = os.environ.get("MANIKIN_EFFORT", "medium")   # low | medium | high | xhigh | max
MAX_TOOL_ROUNDS = 12        # model calls per user message before giving up
# rounds that only point get their own allowance (a walkthrough is one point_at per step).
# each is a cached Opus call (a cent at most) + a $0.0004 Haiku look, 30 is a few tens of cents tops
MAX_POINT_ROUNDS = 30
POINT_MODEL = os.environ.get("MANIKIN_POINT_MODEL", "claude-haiku-5-5")   # finds things on screen
# drives when he takes the wheel, one screenshot at a time (drive.py). the chat model just plans
DRIVE_MODEL = os.environ.get("MANIKIN_DRIVE_MODEL", "claude-haiku-5-5")
DRIVE_EFFORT = os.environ.get("MANIKIN_DRIVE_EFFORT", "medium")
MAX_DRIVE_STEPS = 40        # model calls per drive before he gives up
ASK_TIMEOUT_S = 120         # an unanswered permission prompt counts as "no"
COMPACT_AT_TOKENS = 150_000 # auto compact once a requests context gets this big
CHATTINESS = ("often", "sometimes", "rarely", "off")   # how often he talks on his own
DEFAULT_CHATTINESS = "often"
DEFAULT_VOLUME = 30          # 0-100


@dataclass(frozen=True)
class Paths:
    home: Path

    @property
    def data(self) -> Path:
        return self.home / "data"

    @property
    def sandbox(self) -> Path:
        return self.home / "sandbox"

    @property
    def notes(self) -> Path:
        return self.data / "notes.enc"

    @property
    def reminders(self) -> Path:
        return self.data / "reminders.json"

    @property
    def permissions(self) -> Path:
        return self.data / "permissions.json"

    @property
    def settings(self) -> Path:
        return self.data / "settings.json"

    @property
    def log(self) -> Path:
        return self.data / "brain.log"

    def ensure(self) -> None:
        self.data.mkdir(parents=True, exist_ok=True)
        self.sandbox.mkdir(parents=True, exist_ok=True)


# allow = runs without asking, ask = yes/no bubble first, deny = never offered to the model.
# web_search runs on Anthropics servers so its allow or deny only
DEFAULT_PERMISSIONS = {
    "get_time": "allow",
    "system_info": "allow",
    "open_url": "allow",
    "add_note": "allow",
    "list_notes": "allow",
    "delete_note": "allow",
    "set_reminder": "allow",
    "list_reminders": "allow",
    "cancel_reminder": "allow",
    "web_search": "allow",
    "list_sandbox": "allow",
    "list_apps": "allow",
    "open_app_or_file": "ask",
    "read_clipboard": "ask",
    "read_file": "ask",
    "write_file": "ask",
    "take_screenshot": "ask",
    "point_at": "ask",
    "drive": "ask",              # the plan bubble (once, for this session, or no)
    "stop_trusting": "allow",    # only ever takes trust away
    "clear_chat": "allow",
    "compact_chat": "allow",
    "set_chattiness": "allow",
    "set_sound": "allow",
}
LEVELS = ("allow", "ask", "deny")


def load_permissions(paths: Paths) -> dict[str, str]:
    """defaults with data/permissions.json on top (written if missing), unknown tools/levels ignored"""
    levels = dict(DEFAULT_PERMISSIONS)
    if not paths.permissions.exists():
        paths.permissions.write_text(json.dumps(levels, indent=2) + "\n", encoding="utf-8")
        return levels
    try:
        overrides = json.loads(paths.permissions.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        log.warning("ignoring unreadable %s: %s", paths.permissions, e)
        return levels
    for tool, level in overrides.items() if isinstance(overrides, dict) else ():
        if tool not in levels or level not in LEVELS:
            log.warning("ignoring permission %r: %r", tool, level)
        elif tool == "web_search" and level == "ask":
            log.warning("web_search can't ask (it runs server-side); treating as allow")
        else:
            levels[tool] = level
    return levels


def _is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)    # JSON true isnt a number


class Settings:
    """stuff the user changes by asking Manikin, saved in data/settings.json like

        {"chattiness": "often", "sound": {"volume": 60, "muted": false, "voice": true}}

    the overlay reads "sound" from the same file at launch so keep that shape.
    changes come from tool threads (set_chattiness, set_sound) and the event loop (sound_set)
    so every change + save holds a lock and writes the whole file. on_sound_change gets a
    snapshot after a sound change is saved, its called under the lock so they go out in order
    (and it cant change settings itself, the lock isnt reentrant)
    """

    def __init__(self, path: Path):
        self.path = path
        self.chattiness = DEFAULT_CHATTINESS
        self.volume = DEFAULT_VOLUME
        self.muted = False
        self.voice = True               # his babble while a line types
        self.on_sound_change: Callable[[dict], None] | None = None
        self._lock = threading.Lock()
        if not path.exists():
            return
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            log.warning("ignoring unreadable %s: %s", path, e)
            return
        if not isinstance(data, dict):
            log.warning("ignoring %s: not an object", path)
            return
        level = data.get("chattiness")
        if level in CHATTINESS:
            self.chattiness = level
        elif level is not None:
            log.warning("ignoring chattiness %r", level)
        self._load_sound(data.get("sound"))

    def _load_sound(self, sound) -> None:
        """takes each valid field, anything missing or the wrong type keeps its default"""
        if sound is None:
            return
        if not isinstance(sound, dict):
            log.warning("ignoring sound %r", sound)
            return
        volume = sound.get("volume")
        if _is_int(volume):
            self.volume = min(max(volume, 0), 100)
        elif volume is not None:
            log.warning("ignoring sound volume %r", volume)
        for key in ("muted", "voice"):
            value = sound.get(key)
            if isinstance(value, bool):
                setattr(self, key, value)
            elif value is not None:
                log.warning("ignoring sound %s %r", key, value)

    @property
    def sound(self) -> dict:
        """the sound settings, same shape as the "sound" message and the "sound" key in the file"""
        return {"volume": self.volume, "muted": self.muted, "voice": self.voice}

    def set_chattiness(self, level: str) -> None:
        if level not in CHATTINESS:
            raise ValueError(f"unknown chattiness {level!r}")
        with self._lock:
            self.chattiness = level
            self._save()

    def set_sound(self, volume: int | None = None, muted: bool | None = None,
                  voice: bool | None = None) -> dict:
        """change any of the sound settings (None = leave it), returns the new ones"""
        if volume is not None and not (_is_int(volume) and 0 <= volume <= 100):
            raise ValueError(f"volume must be a whole number from 0 to 100, not {volume!r}")
        for name, value in (("muted", muted), ("voice", voice)):
            if value is not None and not isinstance(value, bool):
                raise ValueError(f"{name} must be true or false, not {value!r}")
        with self._lock:
            if volume is not None:
                self.volume = volume
            if muted is not None:
                self.muted = muted
            if voice is not None:
                self.voice = voice
            self._save()
            sound = self.sound
            if self.on_sound_change is not None:
                self.on_sound_change(sound)
        return sound

    def _save(self) -> None:
        # whole file every time, via a temp file swapped in so a crash never leaves half a file
        data = {"chattiness": self.chattiness, "sound": self.sound}
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        os.replace(tmp, self.path)
