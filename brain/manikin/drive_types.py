"""the pieces "he takes the wheel" is built from, shared by drive.py and safety.py

the chat model calls the `drive` tool with a task and a short plan and the user okays the
plan (gate.py). drive.run_drive then asks a computer-use model for actions one screenshot
at a time, safety.check judges each one from the Facts the executor reports about the
element it touches, and the executor does whatever passes (on the desktop thats the
overlay walking the real pointer over on his cable)
"""

from dataclasses import dataclass, field
from typing import Any, Literal, Protocol

from .screen import Frame

ActionKind = Literal["move", "click", "drag", "scroll", "type", "key"]
Verdict = Literal["go", "ask", "handover"]
Outcome = Literal["done", "grabbed", "cancelled", "failed"]


@dataclass(frozen=True)
class Shot:
    """a screenshot as the model sees it, plus how its pixels map back to the desktop"""
    image: bytes                 # encoded image
    media_type: str              # "image/png" or "image/jpeg"
    frame: Frame                 # image px <-> physical desktop px
    # the capture before it got scaled down (a PIL image of frame.source) so a zoom can
    # show more detail. None means a zoom just crops `image`
    full: Any = field(default=None, compare=False, repr=False)


@dataclass(frozen=True)
class Facts:
    """what UI Automation says about one element. unknown fields stay empty, the rules
    only act on whats known"""
    name: str = ""               # accessible name, label or button text
    kind: str = ""               # UIA control type: "Button", "Edit", "Hyperlink",
                                 # "CheckBox", "MenuItem", "ListItem", "Document", ...
    password: bool = False       # a password field (UIA IsPassword, <input type=password> on a page)
    editable: bool = False       # takes typed text
    multiline: bool = False      # Enter makes a new line instead of sending/submitting
    in_form: bool = False        # inside a web form so Enter might submit it
    window_title: str = ""
    window_class: str = ""
    process: str = ""            # exe name in lowercase: "msiexec.exe", "firefox.exe"
    ancestors: tuple[str, ...] = ()   # names of the enclosing elements, nearest first


@dataclass(frozen=True)
class Action:
    """one thing to do with the real pointer or keyboard, in physical desktop px.
    turns into one "drive" message (protocol.py)"""
    kind: ActionKind
    x: int = 0
    y: int = 0
    to_x: int = 0                # drag end
    to_y: int = 0
    button: Literal["left", "right"] = "left"
    count: int = 1               # click: 1-3
    mods: tuple[str, ...] = ()   # protocol.KEY_NAMES held down through a click or drag
    dx: int = 0                  # scroll: wheel notches, dy > 0 is down, dx > 0 is right
    dy: int = 0
    text: str = ""               # type: 500 chars max
    keys: tuple[str, ...] = ()   # key: protocol.KEY_NAMES pressed together, modifiers first
    repeat: int = 1              # key: 1-100

    def message(self) -> dict:
        """the "drive" message fields for this action (minus type and id)"""
        if self.kind == "type":
            return {"do": "type", "text": self.text}
        if self.kind == "key":
            return {"do": "key", "keys": list(self.keys), "repeat": self.repeat}
        msg = {"do": self.kind, "x": self.x, "y": self.y}
        if self.kind == "click":
            msg.update(button=self.button, count=self.count, mods=list(self.mods))
        elif self.kind == "drag":
            msg.update(to_x=self.to_x, to_y=self.to_y, mods=list(self.mods))
        elif self.kind == "scroll":
            msg.update(dx=self.dx, dy=self.dy)
        return msg


@dataclass(frozen=True)
class Decision:
    """safety.check's verdict on one action. go = do it, ask = only with the users yes
    (even when they trust him for the session), handover = never his, he points at it
    and the user does it"""
    verdict: Verdict
    category: str = ""           # "" | "password" | "payment" | "delete" | "send" |
                                 # "submit" | "close" | "install"
    reason: str = ""             # for the bubble, like "That's a password box. Your turn!"


GO = Decision("go")


@dataclass
class DriveResult:
    """how a drive went. summary goes to the chat model, the rest is for logs and tests"""
    result: Literal["done", "stopped", "failed"]
    summary: str                 # what happened, goes in the chat models tool result
    steps: int = 0               # model calls
    actions: int = 0             # actions actually carried out
    input_tokens: int = 0        # uncached input
    output_tokens: int = 0
    asked: list[str] = field(default_factory=list)       # categories it asked about
    handed_over: list[str] = field(default_factory=list) # categories it handed over
    cache_read_tokens: int = 0   # input read from the prompt cache (most of a drives input)
    cache_write_tokens: int = 0  # input written to the prompt cache


class Executor(Protocol):
    """where a drive happens, the real desktop (drive.DesktopExecutor) or the tests
    FakeExecutor. every method blocks till its done"""

    def screenshot(self) -> Shot: ...

    def facts_at(self, x: int, y: int) -> Facts | None:
        """the element at a desktop point, None when nothing is known"""

    def focused(self) -> Facts | None:
        """the element typed text and keys would go to, None when nothing is known"""

    def run(self, action: Action) -> Outcome: ...

    def handover(self, action: Action, decision: Decision) -> bool:
        """the user does it instead (he points at it). True means they did, carry on"""
