"""finding things on the users screen and pointing at them. find_and_point at the bottom is
the whole flow (runs in a tool thread). everything is physical desktop px (screen.py), app.py
makes the process DPI aware before any of this runs and uiautomation is imported lazily since
it would set DPI awareness itself if it got imported first. each point ends in one "point:"
line in brain.log (where he pointed, the box, where the user clicked next) to check accuracy"""

import base64
import io
import json
import logging
import os
import threading
import time
from dataclasses import dataclass, field, replace
from typing import Any, Callable

import psutil

from . import config
from .drive_types import Facts
from .protocol import to_display
from .screen import Frame, Monitor, Rect, fit_size, list_monitors

log = logging.getLogger(__name__)

COARSE_W = 1280                 # one look at 1280 px wide
POINT_EFFORT = "low"            # a pointing pet has to feel quick
POINT_API_TIMEOUT_S = 20.0
LINGER_S = 30.0                 # a point nobody waits on gets cleared after this
WAIT_S = 120.0                  # a waiting point gives up after this
SETTLE_S = 0.6                  # after a click let menus and dialogs open before the next look
SNAP_TIMEOUT_S = 0.3
DEFAULT_BOX = 48                # box when nothing better is known, centred on the point
MIN_WINDOW = 100                # smaller top level windows are popups, not where the user works
SHELL_CLASSES = {"Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd"}

# the computer-use toolset with everything but mouse_move off, so the model can only
# answer by moving the mouse or by not calling it at all (it isnt on screen)
MEMBERS = ["screenshot", "zoom", "left_click", "right_click", "middle_click", "double_click",
           "triple_click", "left_click_drag", "mouse_move", "left_mouse_down", "left_mouse_up",
           "cursor_position", "scroll", "type", "key", "hold_key", "wait"]
POINT_TOOL = {"type": "computer_toolset_20260801",
              "configs": {m: {"enabled": False} for m in MEMBERS if m != "mouse_move"}}

# careful editing this one, the wording is tuned for accuracy
ASK_POINT = """\
You are a desktop companion that points at things on the user's screen for them. \
The user asked: "{question}"

Below is a screenshot of their {window} window. Move the mouse onto the exact control \
they should click or use for this (the control itself, e.g. the switch or the dropdown, \
not just its label). Call mouse_move once. If it isn't on screen, don't call any tool \
and say so in one sentence."""

# control types worth snapping to when the element under the point is just a label
CLICKABLE = {"Button", "CheckBox", "ComboBox", "ListItem", "Hyperlink", "Slider", "Edit",
             "TabItem", "MenuItem", "TreeItem", "SplitButton", "RadioButton", "MenuBarItem",
             "DataItem", "Spinner"}
COMPOSITE = {"ComboBox", "SplitButton", "Spinner"}      # controls with clickable parts inside


# the target window

@dataclass(frozen=True)
class WindowInfo:
    """a top level window. rect is the visible frame without the invisible resize
    borders, find_target_window clips it to its monitor"""
    handle: int
    title: str
    rect: Rect
    pid: int = 0
    cls: str = ""
    visible: bool = True
    minimized: bool = False
    cloaked: bool = False            # on another virtual desktop, or a suspended UWP app
    tool: bool = False               # WS_EX_TOOLWINDOW, palettes, tray popups and so on
    topmost: bool = False


def usable(w: WindowInfo, skip_pids: set[int], min_size: int = MIN_WINDOW) -> bool:
    return (w.visible and not w.minimized and not w.cloaked and not w.tool
            and bool(w.title.strip()) and w.rect.width >= min_size and w.rect.height >= min_size
            and w.pid not in skip_pids and w.cls not in SHELL_CLASSES)


def pick_window(windows: list[WindowInfo], foreground: int, skip_pids: set[int]) -> WindowInfo | None:
    """the window the user is working in, from windows in z-order (top first). the foreground
    one if it qualifies (usually not, the user just typed into the overlay or clicked its yes
    button), else the topmost usable one that isnt always-on-top (a pinned mini player isnt
    where anyone works)"""
    ok = [w for w in windows if usable(w, skip_pids)]
    for w in ok:
        if w.handle == foreground:
            return w
    return next((w for w in ok if not w.topmost), ok[0] if ok else None)


def clip_to_monitor(rect: Rect, monitors: list[Monitor]) -> Rect:
    """the part of the window on the monitor its mostly on. a maximised window hangs a
    few px off each edge and the model should only see what the user sees"""
    best, area = rect, 0
    for m in monitors:
        part = rect.intersect(m.rect)
        if part and part.width * part.height > area:
            best, area = part, part.width * part.height
    return best


def app_name(title: str) -> str:
    """what to call a window, 'GitHub - Mozilla Firefox' -> 'Mozilla Firefox'"""
    for sep in (" - ", " — ", " – ", " | "):
        if sep in title:
            title = title.rsplit(sep, 1)[1]
    title = title.strip()
    return title if len(title) <= 30 else title[:28] + ".."


def window_label(window: WindowInfo | None) -> str:
    """for sentences, 'your Settings window' or 'your screen' when theres no window"""
    if window is None or not window.handle:
        return "your screen"
    name = app_name(window.title) or "active"
    return f"your {name}" if name.lower().endswith("window") else f"your {name} window"


def ask_text(find: Callable[[], "WindowInfo | None"] | None = None) -> str:
    """the permission question naming the window hed look at (one EnumWindows pass, a few ms)"""
    try:
        window = (find or find_target_window)()
    except Exception:
        log.exception("couldn't find the target window")
        window = None
    return f"Can I look at {window_label(window)} to find it?"


def own_pids() -> set[int]:
    """this process and its ancestors. a venv python.exe is a launcher that starts the
    real interpreter as its child, so the overlay can be our grandparent"""
    pids = {os.getpid(), os.getppid()}
    try:
        pids.update(p.pid for p in psutil.Process().parents())
    except psutil.Error:
        pass
    return pids


def find_target_window() -> WindowInfo | None:
    """the window the user is working in, clipped to its monitor. if the windows cant be
    listed its the primary monitor with handle 0 (the overlay then just waits for a click)"""
    monitors = list_monitors()
    try:
        windows, foreground = _win32_windows()
    except (AttributeError, OSError):
        log.exception("couldn't list windows")
    else:
        w = pick_window(windows, foreground, own_pids())
        if w is not None:
            return replace(w, rect=clip_to_monitor(w.rect, monitors))
        return None
    primary = next((m for m in monitors if m.primary), monitors[0] if monitors else None)
    return WindowInfo(0, "", primary.rect) if primary else None


def _win32_dlls():
    """private WinDLL instances so the argtypes set here dont leak into other modules"""
    import ctypes
    from ctypes import wintypes as w
    user32, dwm, gdi32 = ctypes.WinDLL("user32"), ctypes.WinDLL("dwmapi"), ctypes.WinDLL("gdi32")
    H = ctypes.c_void_p
    for name, args, res in [
        ("IsWindowVisible", [H], w.BOOL), ("IsIconic", [H], w.BOOL),
        ("GetWindowTextLengthW", [H], ctypes.c_int),
        ("GetWindowTextW", [H, w.LPWSTR, ctypes.c_int], ctypes.c_int),
        ("GetClassNameW", [H, w.LPWSTR, ctypes.c_int], ctypes.c_int),
        ("GetWindowThreadProcessId", [H, ctypes.POINTER(w.DWORD)], w.DWORD),
        ("GetWindowLongW", [H, ctypes.c_int], w.LONG),
        ("GetWindowRect", [H, ctypes.POINTER(w.RECT)], w.BOOL),
        ("GetForegroundWindow", [], H),
        ("GetWindowDC", [H], H), ("ReleaseDC", [H, H], ctypes.c_int),
        ("PrintWindow", [H, H, w.UINT], w.BOOL),
    ]:
        fn = getattr(user32, name)
        fn.argtypes, fn.restype = args, res
    dwm.DwmGetWindowAttribute.argtypes = [H, w.DWORD, ctypes.c_void_p, w.DWORD]
    dwm.DwmGetWindowAttribute.restype = ctypes.c_long
    for name, args, res in [
        ("CreateCompatibleDC", [H], H), ("CreateCompatibleBitmap", [H, ctypes.c_int, ctypes.c_int], H),
        ("SelectObject", [H, H], H), ("DeleteObject", [H], w.BOOL), ("DeleteDC", [H], w.BOOL),
        ("GetDIBits", [H, H, w.UINT, w.UINT, ctypes.c_void_p, ctypes.c_void_p, w.UINT], ctypes.c_int),
    ]:
        fn = getattr(gdi32, name)
        fn.argtypes, fn.restype = args, res
    return user32, dwm, gdi32


DWMWA_EXTENDED_FRAME_BOUNDS = 9
DWMWA_CLOAKED = 14
GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x8
WS_EX_TOOLWINDOW = 0x80


def _frame_rect(user32, dwm, hwnd) -> Rect:
    """the visible frame, GetWindowRect would include the invisible resize borders"""
    import ctypes
    from ctypes import wintypes as w
    r = w.RECT()
    if dwm.DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, ctypes.byref(r), ctypes.sizeof(r)) != 0:
        user32.GetWindowRect(hwnd, ctypes.byref(r))
    return Rect(r.left, r.top, r.right - r.left, r.bottom - r.top)


def _win32_windows() -> tuple[list[WindowInfo], int]:
    import ctypes
    from ctypes import wintypes as w
    user32, dwm, _ = _win32_dlls()
    handles: list[int] = []
    proc = ctypes.WINFUNCTYPE(w.BOOL, ctypes.c_void_p, w.LPARAM)

    def each(hwnd, _lp):
        handles.append(hwnd or 0)
        return True

    user32.EnumWindows.argtypes = [proc, w.LPARAM]
    user32.EnumWindows(proc(each), 0)               # top level windows in z-order, top first
    out = []
    for hwnd in handles:
        if not user32.IsWindowVisible(hwnd):
            continue                                 # most of them, skip the rest of the queries
        n = user32.GetWindowTextLengthW(hwnd)
        title = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, title, n + 1)
        cls = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, cls, 256)
        pid = w.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        cloaked = w.DWORD(0)
        dwm.DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, ctypes.byref(cloaked), ctypes.sizeof(cloaked))
        ex = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
        out.append(WindowInfo(hwnd, title.value, _frame_rect(user32, dwm, hwnd), pid.value, cls.value,
                              True, bool(user32.IsIconic(hwnd)), bool(cloaked.value),
                              bool(ex & WS_EX_TOOLWINDOW), bool(ex & WS_EX_TOPMOST)))
    return out, user32.GetForegroundWindow() or 0


# capturing it

PW_RENDERFULLCONTENT = 2


def capture_window(window: WindowInfo):
    """(PIL image, desktop Rect it shows) of the windows visible part. PrintWindow has the
    window draw itself so Manikin never shows up, with a screen grab as the fallback if that
    fails, is blank (some apps dont draw for it) or mostly disagrees with the screen. pretty
    much only the overlay can cover the target so a mismatch is the bitmaps fault (suspended
    Store apps hand back a splash screen)"""
    grab = _grab(window.rect)
    if window.handle:
        try:
            img = _print_window(window.handle, window.rect)
        except (AttributeError, OSError):
            log.exception("PrintWindow failed")
            img = None
        if img is not None and not looks_blank(img) and agrees(img, grab):
            return img, window.rect
        log.info("PrintWindow gave nothing usable for %r; using the screen", app_name(window.title))
    return grab, window.rect


def agrees(a, b, share: float = 0.5) -> bool:
    """at least `share` of the two images is the same (compared at a quarter size)"""
    from PIL import ImageChops
    if a.size != b.size:
        return False
    diff = ImageChops.difference(a.reduce(4), b.reduce(4)).convert("L").histogram()
    return sum(diff[:16]) >= share * sum(diff)


def looks_blank(img) -> bool:
    """one flat colour or a pure black middle, what apps that dont draw for PrintWindow give back"""
    if all(lo == hi for lo, hi in img.getextrema()):
        return True
    w, h = img.size
    middle = img.crop((w // 4, h // 4, max(w * 3 // 4, w // 4 + 1), max(h * 3 // 4, h // 4 + 1)))
    return all(hi == 0 for _, hi in middle.getextrema())


def _print_window(hwnd: int, visible: Rect):
    import ctypes
    from ctypes import wintypes as w
    from PIL import Image
    user32, _, gdi32 = _win32_dlls()
    wr = w.RECT()
    if not user32.GetWindowRect(hwnd, ctypes.byref(wr)):
        return None
    full = Rect(wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top)
    if full.width <= 0 or full.height <= 0:
        return None

    class BitmapInfoHeader(ctypes.Structure):
        _fields_ = [("biSize", w.DWORD), ("biWidth", w.LONG), ("biHeight", w.LONG), ("biPlanes", w.WORD),
                    ("biBitCount", w.WORD), ("biCompression", w.DWORD), ("biSizeImage", w.DWORD),
                    ("biXPelsPerMeter", w.LONG), ("biYPelsPerMeter", w.LONG), ("biClrUsed", w.DWORD),
                    ("biClrImportant", w.DWORD)]

    header = BitmapInfoHeader(ctypes.sizeof(BitmapInfoHeader), full.width, -full.height, 1, 32, 0)  # top-down
    pixels = ctypes.create_string_buffer(full.width * full.height * 4)
    dc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(dc)
    bmp = gdi32.CreateCompatibleBitmap(dc, full.width, full.height)
    old = gdi32.SelectObject(mem, bmp)
    try:
        ok = user32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)
        rows = gdi32.GetDIBits(mem, bmp, 0, full.height, pixels, ctypes.byref(header), 0) if ok else 0
    finally:
        gdi32.SelectObject(mem, old)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mem)
        user32.ReleaseDC(hwnd, dc)
    if rows != full.height:
        return None
    img = Image.frombuffer("RGB", (full.width, full.height), pixels, "raw", "BGRX", 0, 1)
    # the bitmap covers the whole window rect, invisible borders and off screen bits too
    part = visible.intersect(full)
    if part is None or part != visible:
        return None                      # the window moved in the meantime, screen grab is safer
    return img.crop((part.left - full.left, part.top - full.top,
                     part.right - full.left, part.bottom - full.top))


def _grab(rect: Rect):
    import mss
    from PIL import Image
    with mss.MSS() as grabber:
        shot = grabber.grab({"left": rect.left, "top": rect.top, "width": rect.width, "height": rect.height})
    return Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")


def grab(rect: Rect):
    """PIL image of that part of the screen, Manikin and all (for driving)"""
    return _grab(rect)


def cursor_pos() -> tuple[int, int] | None:
    """where the mouse pointer is in desktop px, None if unknown"""
    import ctypes
    from ctypes import wintypes as w
    p = w.POINT()
    try:
        if ctypes.WinDLL("user32").GetCursorPos(ctypes.byref(p)):
            return p.x, p.y
    except (AttributeError, OSError):
        pass
    return None


def foreground_pid() -> int:
    """pid of the process that owns the foreground window, 0 if unknown"""
    import ctypes
    from ctypes import wintypes as w
    try:
        user32, _, _ = _win32_dlls()
        hwnd = user32.GetForegroundWindow()
        pid = w.DWORD(0)
        if hwnd:
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        return pid.value
    except (AttributeError, OSError):
        return 0


# asking the model where it is

def look_size(width: int, height: int) -> tuple[int, int]:
    """1280 px wide, never upscaled, always within the models image limits"""
    w = min(COARSE_W, width)
    return fit_size(w, max(1, round(height * w / width)))


def prepare(image, source: Rect):
    """the image to send and the Frame that maps its answer back to the desktop"""
    w, h = look_size(*image.size)
    return image.resize((w, h)) if (w, h) != image.size else image, Frame(source, w, h)


def encode_bytes(img) -> tuple[bytes, str]:
    """(file bytes, media type). PNG, or JPEG if the PNG would go over the size cap"""
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    data, media = buf.getvalue(), "image/png"
    if len(data) > 4_500_000:                   # stay under the per image size cap
        buf = io.BytesIO()
        img.convert("RGB").save(buf, format="JPEG", quality=90)
        data, media = buf.getvalue(), "image/jpeg"
    return data, media


def image_block(data: bytes, media: str) -> dict:
    return {"type": "image", "source": {"type": "base64", "media_type": media,
                                        "data": base64.standard_b64encode(data).decode("ascii")}}


def encode(img) -> dict:
    return image_block(*encode_bytes(img))


@dataclass
class Sighting:
    """what the pointing model answered. point is in desktop px, None if it said no"""
    point: tuple[float, float] | None
    said: str
    seconds: float
    model: str
    tokens: tuple[int, int] = (0, 0)


_client = None
_client_lock = threading.Lock()


def _default_client():
    global _client
    with _client_lock:
        if _client is None:
            import anthropic
            _client = anthropic.Anthropic(timeout=POINT_API_TIMEOUT_S, max_retries=1)
        return _client


def locate(question: str, image, frame: Frame, window: str = "app", client=None) -> Sighting:
    """one look, where should the user click for `question`? blocks (tool thread). the image
    is already frame sized (prepare), no mouse_move means it isnt visible and the reply says why"""
    client = client or _default_client()
    prompt = ASK_POINT.format(question=question, window=window)
    t0 = time.perf_counter()
    response = client.beta.messages.create(
        model=config.POINT_MODEL, max_tokens=2000, output_config={"effort": POINT_EFFORT},
        tools=[POINT_TOOL], messages=[{"role": "user", "content": [
            {"type": "text", "text": prompt}, encode(image)]}])
    return read_sighting(response, frame, time.perf_counter() - t0)


def read_sighting(response, frame: Frame, seconds: float) -> Sighting:
    usage = getattr(response, "usage", None)
    tokens = (getattr(usage, "input_tokens", 0) or 0, getattr(usage, "output_tokens", 0) or 0)
    said = " ".join(b.text for b in response.content if b.type == "text").strip()
    for block in response.content:
        if block.type != "tool_use" or block.name != "mouse_move":
            continue
        coord = (block.input or {}).get("coordinate") if isinstance(block.input, dict) else None
        if (isinstance(coord, (list, tuple)) and len(coord) == 2
                and all(isinstance(c, (int, float)) and not isinstance(c, bool) for c in coord)):
            # clamp onto the picture, a point just off the edge is still meant for the window
            ix = min(max(coord[0], 0), frame.image_w - 1)
            iy = min(max(coord[1], 0), frame.image_h - 1)
            return Sighting(frame.to_desktop(ix, iy), said, seconds, config.POINT_MODEL, tokens)
    return Sighting(None, said[:300], seconds, config.POINT_MODEL, tokens)


# snapping to the real control

@dataclass(frozen=True)
class Element:
    """a UI Automation element, its desktop box, accessible name and control type"""
    rect: Rect
    name: str
    kind: str


@dataclass(frozen=True)
class Snap:
    box: Rect
    name: str = ""
    kind: str = ""                   # "" = not snapped, the default box around the point


def choose_box(x: float, y: float, window: Rect, elements: list[Element]) -> Snap:
    """the box to show from the element under the point and its parents (innermost first).
    an element counts if it holds the point, is 8+ px each way and under a sixth of the window.
    pick a dropdown or split button around the point (not the list item showing its value),
    else the innermost named clickable (the button around an icon, the switch in a settings
    row), else the innermost clickable, else whatever is innermost"""
    def fits(e: Element) -> bool:
        return (e.rect.contains(x, y) and e.rect.width >= 8 and e.rect.height >= 8
                and e.rect.width * e.rect.height < window.width * window.height / 6)
    good = [e for e in elements if fits(e)]
    clickable = [e for e in good if e.kind in CLICKABLE]
    pick = next((e for e in clickable if e.kind in COMPOSITE),
                next((e for e in clickable if e.name.strip()),
                     clickable[0] if clickable else good[0] if good else None))
    if pick is None:
        half = DEFAULT_BOX // 2
        return Snap(Rect(round(x) - half, round(y) - half, DEFAULT_BOX, DEFAULT_BOX))
    return Snap(pick.rect, pick.name.strip()[:80], pick.kind)


def snap(x: float, y: float, window: Rect,
         lookup: Callable[[float, float], list[Element]] | None = None) -> Snap:
    """turn the models point into a control box. the point itself never moves, the box only
    counts if it holds the point and the model picked that spot on purpose (the switch at the
    end of a wide row), the centre would only help a sloppy point on a tiny control"""
    found = (lookup or elements_at)(x, y)
    return choose_box(x, y, window, found)


def elements_at(x: float, y: float, timeout: float = SNAP_TIMEOUT_S) -> list[Element]:
    """the element under (x, y) plus its parent and grandparent. one ControlFromPoint, never a
    tree walk (seconds in browsers), on a helper thread since COM calls cant time out. empty on
    failure or for the overlays own element. not checked against the target windows process,
    Store app controls live in another process than their frame and only the overlay can be
    on top of it anyway"""
    try:
        import uiautomation as auto      # out here so the first import doesnt eat the timeout
    except ImportError:
        return []
    found: list[Element] = []

    def query():
        try:
            with auto.UIAutomationInitializerInThread():
                control = auto.ControlFromPoint(int(round(x)), int(round(y)))
                if control is None or control.ProcessId in own_pids():
                    return
                for _ in range(3):
                    r = control.BoundingRectangle
                    kind = control.ControlTypeName.removesuffix("Control")
                    found.append(Element(Rect(r.left, r.top, r.width(), r.height()), control.Name or "", kind))
                    control = control.GetParentControl()
                    if control is None:
                        break
        except Exception:
            log.debug("ControlFromPoint failed", exc_info=True)

    worker = threading.Thread(target=query, name="uia-lookup", daemon=True)
    worker.start()
    worker.join(timeout)
    if worker.is_alive():
        log.info("accessibility lookup took over %.1f s; not snapping", timeout)
        return []
    return list(found)


def element_name_at(x: float, y: float) -> str:
    """'Name' (Type) of whatevers under a point, or '' (for a click that missed)"""
    found = elements_at(x, y)
    for e in found:
        if e.name.strip():
            return f"'{e.name.strip()[:60]}' ({e.kind})"
    return ""


# what the safety rules get to know (driving)

FACTS_TIMEOUT_S = 0.5           # a bit longer than snapping, a fact lookup walks up to the window
FACT_WALK = 25                  # parents walked looking for the top level window
FACT_ANCESTORS = 8              # enclosing names kept, nearest first
GWL_STYLE = -16
ES_MULTILINE = 0x4


def uia_query(query: Callable[[Any], Any], timeout: float, what: str) -> Any:
    """query(uiautomation) on a helper thread like elements_at, None on failure or timeout"""
    try:
        import uiautomation as auto
    except ImportError:
        return None
    out = []

    def work():
        try:
            with auto.UIAutomationInitializerInThread():
                out.append(query(auto))
        except Exception:
            log.debug("%s failed", what, exc_info=True)

    worker = threading.Thread(target=work, name="uia-facts", daemon=True)
    worker.start()
    worker.join(timeout)
    if worker.is_alive():
        log.info("%s took over %.1f s; no facts", what, timeout)
        return None
    return out[0] if out else None


def facts_at(x: float, y: float, timeout: float = FACTS_TIMEOUT_S) -> Facts | None:
    """what the safety rules get to know about the control under a desktop point, or None
    (nothing known, or its the overlays own). a label inside a named button counts as it"""
    def query(auto):
        control = auto.ControlFromPoint(int(round(x)), int(round(y)))
        return _facts(auto, control, snap_up=True)
    return uia_query(query, timeout, "accessibility lookup at a point")


def focused_facts(timeout: float = FACTS_TIMEOUT_S) -> Facts | None:
    """same thing for the element with keyboard focus, or None"""
    return uia_query(lambda auto: _facts(auto, auto.GetFocusedControl(), snap_up=False),
                     timeout, "focus lookup")


def _kind(control) -> str:
    return control.ControlTypeName.removesuffix("Control")


def _facts(auto, control, snap_up: bool) -> Facts | None:
    if control is None or control.ProcessId in own_pids():
        return None
    chain, parent, at_root = [control], control, False
    for _ in range(FACT_WALK):
        parent = parent.GetParentControl()
        if parent is None:
            at_root = True
            break
        chain.append(parent)
    # if we hit the root the last element is the desktop and the one before it the window
    top = chain[-2] if at_root and len(chain) >= 2 else None
    pick = 0
    if snap_up:                          # text inside a button, the button is what gets clicked
        pick = next((i for i, c in enumerate(chain[:3])
                     if _kind(c) in CLICKABLE and (c.Name or "").strip()), 0)
    el = chain[pick]
    kind = _kind(el)
    value = el.GetPattern(auto.PatternId.ValuePattern)
    read_only = value is not None and value.IsReadOnly
    editable = not read_only if kind == "Edit" else (value is not None and not read_only)
    above = chain[pick + 1:-1] if at_root else chain[pick + 1:]
    names = [(c.Name or "").strip()[:80] for c in above[:FACT_ANCESTORS * 2]]
    multiline = kind == "Edit" and (_native_multiline(el.NativeWindowHandle)
                                    or "multiline=true" in (el.AriaProperties or "").lower())
    return Facts(
        name=(el.Name or "").strip()[:200], kind=kind, password=bool(el.IsPassword),
        editable=editable, multiline=multiline, in_form=_in_form(above),
        window_title=(top.Name or "").strip() if top else "",
        window_class=(top.ClassName or "") if top else "",
        process=_process_name(el.ProcessId),
        ancestors=tuple(n for n in names if n)[:FACT_ANCESTORS])


def _in_form(above: list) -> bool:
    """inside a web form as far as UIA can tell, so a "form" landmark or anywhere in a page
    (electron apps too, not the browsers toolbar). browsers only expose named <form>s and
    lots of sites have none, so any page counts and Enter asks a bit more than it needs to"""
    for c in above:
        if _kind(c) == "Document" or (c.LocalizedControlType or "").lower() == "form":
            return True
    return False


def _native_multiline(hwnd: int) -> bool:
    """a classic win32 edit box with ES_MULTILINE (notepads for example). browsers and
    other self drawn apps dont say, so this only says yes when its sure"""
    if not hwnd:
        return False
    import ctypes
    user32, _, _ = _win32_dlls()
    cls = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, cls, 256)
    if "edit" not in cls.value.lower():
        return False
    return bool(user32.GetWindowLongW(hwnd, GWL_STYLE) & ES_MULTILINE)


def _process_name(pid: int) -> str:
    try:
        return psutil.Process(pid).name().lower()
    except (psutil.Error, ValueError):
        return ""


# the coordinator

@dataclass(frozen=True)
class Spot:
    """what the overlay gets told to point at"""
    x: int
    y: int
    box: Rect
    window: int = 0
    text: str = ""


@dataclass(frozen=True)
class Outcome:
    """how a point ended. from the overlay: clicked, moved, dismissed, cleared. from the
    brain: timeout, cancelled, replaced, interrupted (a new chat message)"""
    kind: str
    x: int = 0
    y: int = 0
    inside: bool = False
    clicked_on: str = ""             # click outside the box, 'Name' (Type) of what got hit


OVERLAY_OUTCOMES = ("clicked", "moved", "dismissed", "cleared")


@dataclass
class _Point:
    id: int
    spot: Spot
    wait: bool
    info: dict
    done: threading.Event = field(default_factory=threading.Event)
    outcome: Outcome | None = None
    timer: threading.Timer | None = None


class Pointer:
    """shared by the tool thread and the event loop (point_done, cancel), the Pipe is thread
    safe. one outcome per id, whichever of point_done, timeout, cancel or a newer point comes
    first wins (calling a point off sends point_clear and the overlays "cleared" comes late)"""

    def __init__(self, pipe, linger_s: float = LINGER_S, wait_s: float = WAIT_S,
                 settle_s: float = SETTLE_S, name_at: Callable[[float, float], str] | None = None):
        self._pipe = pipe
        self._linger_s = linger_s
        self._wait_s = wait_s
        self._settle_s = settle_s
        self._name_at = name_at if name_at is not None else element_name_at
        self._lock = threading.Lock()
        self._active: _Point | None = None
        self._next_id = 0
        self._cancels = 0            # bumped by cancel() so a look still in flight cant point after it

    @property
    def wait_s(self) -> float:
        return self._wait_s

    @property
    def epoch(self) -> int:
        """take this before a slow look and pass it to show() so a cancel in between wins"""
        return self._cancels

    @property
    def waiting(self) -> bool:
        """a turn is blocked until the user clicks the current point"""
        with self._lock:
            return self._active is not None and self._active.wait

    def show(self, spot: Spot, wait: bool = False, info: dict | None = None,
             epoch: int | None = None) -> Outcome | None:
        """point at spot. wait blocks until it ends and returns how (else None right away),
        info is extra log fields, epoch is from before the look (nothing shows if cancelled since)"""
        with self._lock:
            if epoch is not None and epoch != self._cancels:
                log.info(point_line({"outcome": "cancelled", **(info or {}), "shown": "no"}))
                return Outcome("cancelled")
            self._next_id += 1
            point = _Point(self._next_id, spot, wait, dict(info or {}))
            old, self._active = self._active, point
        if old is not None:
            self._finish(old, Outcome("replaced"), clear=True)
        self._pipe.send("point", id=point.id, x=spot.x, y=spot.y,
                        box=[spot.box.left, spot.box.top, spot.box.width, spot.box.height],
                        window=spot.window, text=to_display(spot.text))
        if not wait:
            point.timer = threading.Timer(self._linger_s, self._expire, (point,))
            point.timer.daemon = True
            point.timer.start()
            if point.outcome is not None:            # finished before the timer existed
                point.timer.cancel()
            return None
        if not point.done.wait(self._wait_s):
            self._finish(point, Outcome("timeout"), clear=True)
        outcome = point.outcome
        clicked_on = ""
        if outcome.kind == "clicked":
            if not outcome.inside:
                clicked_on = self._name_at(outcome.x, outcome.y)
            time.sleep(self._settle_s)               # the next look should see the menu it opened
        self._log(point, clicked_on)
        return replace(outcome, clicked_on=clicked_on) if clicked_on else outcome

    def on_done(self, msg: dict) -> None:
        """the overlays point_done (from the event loop)"""
        pid, kind = msg.get("id"), msg.get("outcome")
        if not isinstance(pid, int) or isinstance(pid, bool) or kind not in OVERLAY_OUTCOMES:
            log.warning("ignoring malformed point_done")
            return
        with self._lock:
            point = self._active if self._active is not None and self._active.id == pid else None
        if point is None:
            log.info("point %s: late %s ignored", pid, kind)
            return
        x, y = msg.get("x"), msg.get("y")
        self._finish(point, Outcome(kind, x if _is_int(x) else 0, y if _is_int(y) else 0,
                                    msg.get("inside") is True))

    def cancel(self) -> None:
        """user pressed Esc on the turn, stop pointing"""
        with self._lock:
            self._cancels += 1
        self._end_active(Outcome("cancelled"))

    def interrupt(self) -> None:
        """a new chat message came in while a turn waits on a click, stop waiting"""
        with self._lock:
            point = self._active if self._active is not None and self._active.wait else None
        if point is not None:
            self._finish(point, Outcome("interrupted"), clear=True)

    # internals

    def _end_active(self, outcome: Outcome) -> None:
        with self._lock:
            point = self._active
        if point is not None:
            self._finish(point, outcome, clear=True)

    def _expire(self, point: _Point) -> None:
        self._finish(point, Outcome("timeout"), clear=True)

    def _finish(self, point: _Point, outcome: Outcome, clear: bool = False) -> bool:
        with self._lock:
            if point.outcome is not None:
                return False                         # it already ended
            point.outcome = outcome
            if self._active is point:
                self._active = None
        if point.timer is not None:
            point.timer.cancel()
        if clear:
            self._pipe.send("point_clear", id=point.id)
        if not point.wait:
            self._log(point, "")                     # nobody waits on it so log now
        point.done.set()
        return True

    def _log(self, point: _Point, clicked_on: str) -> None:
        o, spot = point.outcome, point.spot
        fields = {"id": point.id, "outcome": o.kind, **point.info,
                  "at": f"{spot.x},{spot.y}",
                  "box": f"{spot.box.left},{spot.box.top},{spot.box.width},{spot.box.height}",
                  "wait": "yes" if point.wait else "no"}
        if o.kind == "clicked":
            fields.update(click=f"{o.x},{o.y}", inside="yes" if o.inside else "no",
                          dist=round(spot.box.distance(o.x, o.y)))
            if clicked_on:
                fields["clicked_on"] = clicked_on
        log.info(point_line(fields))


def _is_int(v: Any) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def point_line(fields: dict, prefix: str = "point") -> str:
    """'point: id=4 outcome=clicked q="..." ...' on one line, strings JSON quoted"""
    def value(v):
        if isinstance(v, float):
            return f"{v:.2f}"
        if isinstance(v, str) and (not v or any(c in v for c in ' ="')):
            return json.dumps(v, ensure_ascii=True)
        return str(v)
    return f"{prefix}: " + " ".join(f"{k}={value(v)}" for k, v in fields.items())


# the whole point_at tool

_OUTCOME_TEXT = {
    "moved": "The window changed before they clicked.",
    "dismissed": "The user waved you off.",
    "cleared": "The point was called off.",
    "cancelled": "The user cancelled.",
    "replaced": "Another point replaced it.",
    "interrupted": "The user typed a new message instead of clicking; it comes next.",
}


def describe(outcome: Outcome, wait_s: float = WAIT_S) -> str:
    if outcome.kind == "clicked":
        if outcome.inside:
            return "The user clicked it."
        if outcome.clicked_on:
            return f"The user clicked somewhere else (on {outcome.clicked_on})."
        return "The user clicked somewhere else."
    if outcome.kind == "timeout":
        return f"They didn't click within {round(wait_s / 60)} minutes."
    return _OUTCOME_TEXT.get(outcome.kind, f"It ended: {outcome.kind}.")


def find_and_point(pointer: Pointer, target: str, say: str = "", wait: bool = False, *,
                   find: Callable[[], WindowInfo | None] = find_target_window,
                   capture: Callable = capture_window, look: Callable = locate,
                   refine: Callable = snap) -> str:
    """the point_at tool, look, point, maybe wait. returns the result for the model and
    raises RuntimeError with a short reason when it cant look at all"""
    t0 = time.perf_counter()
    epoch = pointer.epoch            # so a cancel during the look doesnt leave him pointing
    window = find()
    if window is None:
        raise RuntimeError("there's no window to look at")
    name = app_name(window.title) if window.handle else "your screen"
    where = f"in {name}" if window.handle else "on your screen"
    try:
        image, source = capture(window)
    except Exception:
        log.exception("capture failed")
        raise RuntimeError(f"couldn't capture {window_label(window)}")
    small, frame = prepare(image, source)
    try:
        seen = look(target, small, frame, name if window.handle else "screen")
    except Exception as e:
        log.exception("pointing model failed")
        raise RuntimeError(f"couldn't take a look right now ({type(e).__name__})")
    info = {"q": target[:100], "win": (window.title or "screen")[:40], "model": seen.model,
            "t": round(seen.seconds, 2), "img": f"{frame.image_w}x{frame.image_h}",
            "tok": f"{seen.tokens[0]}/{seen.tokens[1]}"}
    if seen.point is None:
        said = to_display(seen.said) or "it isn't visible"
        log.info(point_line({"outcome": "unseen", **info, "said": said[:200]}))
        return f"Couldn't see it {where}: {said}"

    x, y = seen.point
    got = refine(x, y, window.rect)
    info.update(total=round(time.perf_counter() - t0, 2))
    if got.kind:
        info["el"] = f"{got.kind}:{got.name}"
    spot = Spot(round(x), round(y), got.box, window.handle, say)
    what = f"'{got.name}' ({got.kind})" if got.kind and got.name else "it"
    outcome = pointer.show(spot, wait, info, epoch)
    if outcome is None:
        return f"Pointing at {what} {where}."
    return f"Pointed at {what} {where}. {describe(outcome, pointer.wait_s)}"
