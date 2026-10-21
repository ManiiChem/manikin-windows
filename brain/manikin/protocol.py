"""the pipe between the overlay and the brain. JSON lines over stdin/stdout, one object per
line, each one has a "type"

overlay -> brain
  {"type": "user", "text": "..."}                  a chat message
  {"type": "answer", "id": 3, "allow": true}       reply to an "ask"
  {"type": "answer", "id": 3, "allow": true, "scope": "session"}
      the third button on an ask that offered it ("session": true), means yes and dont ask
      again this session. only ever comes from a real click
  {"type": "password", "text": "..."}              reply to a "password" prompt
  {"type": "password", "skip": true}               no password, memory stays locked
  {"type": "cancel"}                               Esc while waiting on a reply. stops the turn,
                                                   drops queued messages and closes asks
  {"type": "presence", "pet_idle": 42.0, "pc_idle": 3.5, "typing": false, "asleep": false}
      every 5 s. pet_idle = secs since the user last touched Manikin, pc_idle = system wide
      idle secs or -1 if unknown (counts as present), typing = the chat box has text in it
  {"type": "event", "name": "woke", "slept_s": 720}
      the user poked him awake
  {"type": "point_done", "id": 4, "outcome": "clicked", "x": 1215, "y": 390, "inside": true}
      a "point" ended, always exactly one per point. outcome is one of
        clicked    the users first click after he got there (x, y = where, inside = in the
                   box give or take 6 px)
        moved      the target window moved, resized, closed, minimized or lost the foreground
        dismissed  the user grabbed or poked him, or hit Esc
        cleared    after point_clear, or a newer point replaced it
      x, y and inside are 0/false unless its clicked
  {"type": "sound_set", "muted": true}             the user toggled mute (Ctrl+M), remember it
  {"type": "drive_done", "id": 7, "outcome": "done", "x": 1215, "y": 390}
      a "drive" action ended, exactly one per id. outcome is one of
        done       he did it (x, y = where the pointer is now)
        grabbed    the user moved the mouse or clicked so he let go mid action
        cancelled  Esc, anywhere (a "cancel" follows)
        failed     Windows refused the input (an admin window say) or he wouldve typed
                   into his own window

brain -> overlay
  {"type": "sound", "volume": 30, "muted": false, "voice": true}
      sound settings. volume 0-100, muted, voice (his babble while a line types). goes out
      first before anything else, then after every change. the overlay reads the same
      values from data/settings.json at launch, before the brain is up
  {"type": "password", "mode": "create" | "confirm" | "unlock", "error": "..." | null}
  {"type": "unlocked", "ok": true | false}
  {"type": "ready"}                                accepting chat messages now
  {"type": "thinking"}                             working on a reply (never for proactive turns)
  {"type": "status", "text": "searching the web"}  short lowercase label for the thinking
                                                   bubble, what hes doing right now
  {"type": "ask", "id": 3, "text": "Can I ...?"}   show yes/no buttons
  {"type": "ask", "id": 3, "text": "...", "session": true}
      ...plus a third one, "yes for this session"
  {"type": "ask_cancel", "id": 3}                  the prompt timed out or got cancelled, hide it
  {"type": "say", "text": "...", "mood": "happy" | null, "proactive": true}
      reply for the speech bubble. mood is one of MOODS for the face to act out. proactive
      is only there when its true, he said it on his own so it isnt answering anything
  {"type": "cancelled"}                            a "cancel" is done (sent even if nothing ran)
  {"type": "remind", "text": "..."}                a reminder fired
  {"type": "error", "text": "..."}                 something went wrong
  {"type": "point", "id": 4, "x": 1210, "y": 388, "box": [1180, 370, 60, 36],
   "window": 132456, "text": "Click this one"}
      walk over and point at (x, y) in physical desktop px (the virtual desktop, so x can be
      negative). box = the controls bounds [left, top, width, height] with (x, y) inside it.
      window = native handle of the window its in so the overlay can watch it (0 = unknown).
      text = a line to keep in the bubble while pointing ("" = none). a new point replaces
      the current one
  {"type": "point_clear", "id": 4}                 stop pointing at that id and walk home
  {"type": "drive", "id": 7, "do": "click", ...}
      he takes the wheel. one action with the real pointer or keyboard, walking the pointer
      there on his cable first. one at a time, the next one comes after its "drive_done".
      coords are desktop px
      do: move   x, y            (hover there)
          click  x, y, button ("left" | "right"), count (1-3), mods (key names held)
          drag   x, y, to_x, to_y, mods
          scroll x, y, dx, dy  (wheel notches, dy > 0 is down, dx > 0 is right)
          type   text  (at the focus, 500 chars max, anything longer comes split up)
          key    keys (KEY_NAMES, modifiers first, pressed together), repeat (1-100)
  {"type": "drive_end", "result": "done"}
      the drive is over, let go of the pointer and walk home. result is done (victory lap)
      | stopped (cancelled, declined or handed over, sheepish) | failed
  {"type": "narrate", "text": "...", "mood": "happy" | null}
      a passing line while he drives ("Settings... aha!"), shown like "say" but it doesnt
      end the turn

"key" and "mods" can only use KEY_NAMES (below). the brain maps the models xdotool style
names ("Return", "ctrl+s") onto them and platform_win32.c maps those to real keys
"""

# keys a "drive" action can press. modifiers, named keys and any one printable ASCII char
# except space (thats "space"). chars go through the users keyboard layout so shortcuts use
# lowercase ("ctrl" + "s"), keep in sync with the key table in src/platform_win32.c
MODIFIER_KEYS = ("ctrl", "shift", "alt", "super")
NAMED_KEYS = ("enter", "tab", "escape", "backspace", "delete", "insert", "space",
              "up", "down", "left", "right", "home", "end", "pageup", "pagedown",
              *(f"f{n}" for n in range(1, 13)))
KEY_NAMES = (*MODIFIER_KEYS, *NAMED_KEYS)


def is_key_name(name: str) -> bool:
    """a modifier, a named key or one printable ASCII char"""
    return name in KEY_NAMES or (len(name) == 1 and 32 < ord(name) < 127)

import asyncio
import json
import logging
import re
import sys
import threading
from typing import Any, Callable

log = logging.getLogger(__name__)

_TYPOGRAPHY = str.maketrans({
    "‘": "'", "’": "'", "“": '"', "”": '"',
    "–": "-", "—": "-", "…": "...", " ": " ", "•": "*",
})


def to_display(text: str) -> str:
    """the bubble font is printable ASCII only, swap fancy quotes/dashes for plain ones"""
    text = text.translate(_TYPOGRAPHY).replace("\t", " ")
    return "".join(ch for ch in text if ch == "\n" or 32 <= ord(ch) < 127).strip()


# moods his face can act out, keep in sync with Emotion in src/anim_emotion.h
MOODS = ("happy", "excited", "laughing", "love", "surprised",
         "confused", "smug", "sad", "worried", "annoyed")
_MOOD_TAG = re.compile(r"^\s*\[(\w+)\]\s*")
_ANY_TAG = re.compile(r"\[(\w+)\][ \t]*")


def split_mood(text: str) -> tuple[str | None, str]:
    """pulls the [mood] tag off a reply, '[happy] Hi!' -> ('happy', 'Hi!')

    the persona asks for one tag up front but Claude sometimes slips another one in mid reply
    ("...wow. [love] You built me"). every known tag gets stripped so none shows in the bubble,
    the leading one wins, else the first one found. unknown tags like "[note]" are left alone
    """
    mood = None
    match = _MOOD_TAG.match(text)
    if match and match.group(1).lower() in MOODS:
        mood, text = match.group(1).lower(), text[match.end():]

    def strip(m: re.Match) -> str:
        nonlocal mood
        name = m.group(1).lower()
        if name not in MOODS:
            return m.group(0)
        mood = mood or name
        return ""
    text = _ANY_TAG.sub(strip, text)
    return mood, re.sub(r"[ \t]{2,}", " ", text).strip()


class Pipe:
    def __init__(self, loop: asyncio.AbstractEventLoop, on_message: Callable[[dict], None]):
        self._loop = loop
        self._on_message = on_message
        self._lock = threading.Lock()
        self.closed = asyncio.Event()

        sys.stdin.reconfigure(encoding="utf-8")
        sys.stdout.reconfigure(encoding="utf-8", newline="\n")
        # real stdout is for protocol lines only, a stray print() lands on stderr instead
        self._out = sys.stdout
        sys.stdout = sys.stderr

    def start(self) -> None:
        # plain thread because the asyncio loop on windows cant read an anonymous stdin pipe
        threading.Thread(target=self._read_loop, name="stdin-reader", daemon=True).start()

    def _read_loop(self) -> None:
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                log.warning("ignoring malformed line from the overlay")
                continue
            if isinstance(msg, dict):
                self._loop.call_soon_threadsafe(self._on_message, msg)
        self._loop.call_soon_threadsafe(self.closed.set)   # overlay closed our stdin

    def send(self, type_: str, **fields: Any) -> None:
        line = json.dumps({"type": type_, **fields}, ensure_ascii=True) + "\n"
        with self._lock:
            try:
                self._out.write(line)
                self._out.flush()
            except OSError:
                self._loop.call_soon_threadsafe(self.closed.set)
