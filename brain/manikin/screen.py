"""where things are on the users screen. monitors, captures and the coordinate chain

anything that crosses between processes is in physical desktop px, the virtual desktop over all
monitors with (0, 0) at the primary monitors top left (so a monitor left of it has negative x).
the overlay is per monitor DPI aware too (GLFW opts it in) so it just subtracts its window pos

    physical desktop px  --Frame.to_image-->  image px sent to the model
    image px from model  --Frame.to_desktop-> physical desktop px  -> overlay

call set_dpi_aware() before importing mss or anything else that touches the screen. the first
call that sets the DPI awareness wins and an unaware process gets scaled (logical) coords
"""

import math
from dataclasses import dataclass

# image limits for the model (Opus 5.5 and the computer use toolset), long edge up to
# 2576 px and at most 4784 visual tokens of 28x28 px each
MAX_EDGE = 2576
MAX_VISUAL_TOKENS = 4784
TOKEN_PX = 28


def set_dpi_aware() -> bool:
    """opt this process into per monitor v2 DPI awareness, False if it didnt take"""
    import ctypes
    try:
        # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):
            return True
    except (AttributeError, OSError):
        pass
    # already set (by us or an import) is fine as long as its per monitor
    try:
        user32 = ctypes.windll.user32
        user32.GetThreadDpiAwarenessContext.restype = ctypes.c_void_p
        user32.GetAwarenessFromDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        return user32.GetAwarenessFromDpiAwarenessContext(user32.GetThreadDpiAwarenessContext()) == 2
    except (AttributeError, OSError):
        return False


@dataclass(frozen=True)
class Rect:
    """physical desktop px, right and bottom are exclusive"""
    left: int
    top: int
    width: int
    height: int

    @property
    def right(self) -> int:
        return self.left + self.width

    @property
    def bottom(self) -> int:
        return self.top + self.height

    @property
    def center(self) -> tuple[float, float]:
        return self.left + self.width / 2, self.top + self.height / 2

    def contains(self, x: float, y: float) -> bool:
        return self.left <= x < self.right and self.top <= y < self.bottom

    def intersect(self, other: "Rect") -> "Rect | None":
        left, top = max(self.left, other.left), max(self.top, other.top)
        right, bottom = min(self.right, other.right), min(self.bottom, other.bottom)
        if right <= left or bottom <= top:
            return None
        return Rect(left, top, right - left, bottom - top)

    def distance(self, x: float, y: float) -> float:
        """how far a point is outside the rect, 0 if its inside"""
        dx = max(self.left - x, 0.0, x - (self.right - 1))
        dy = max(self.top - y, 0.0, y - (self.bottom - 1))
        return math.hypot(dx, dy)


@dataclass(frozen=True)
class Monitor:
    rect: Rect
    dpi: int = 96                        # 96 = 100% scaling, 144 = 150%
    primary: bool = False

    @property
    def scale(self) -> float:
        return self.dpi / 96


def monitor_at(monitors: list[Monitor], x: float, y: float) -> Monitor | None:
    for m in monitors:
        if m.rect.contains(x, y):
            return m
    return None


def logical_to_physical(monitors: list[Monitor], x: float, y: float) -> tuple[float, float]:
    """for coords from a DPI unaware source. windows scales those around each monitors origin
    so undo it on whichever monitor the logical point lands on"""
    for m in monitors:
        r, s = m.rect, m.scale
        lx, ly = r.left + (x - r.left) * s, r.top + (y - r.top) * s
        if r.contains(lx, ly):
            return lx, ly
    return x, y


def fit_size(width: int, height: int, max_edge: int = MAX_EDGE,
             max_tokens: int = MAX_VISUAL_TOKENS) -> tuple[int, int]:
    """biggest size with this aspect ratio that the model takes without rejecting it"""
    s = min(1.0, max_edge / max(width, height))
    while True:
        w, h = max(1, round(width * s)), max(1, round(height * s))
        if math.ceil(w / TOKEN_PX) * math.ceil(h / TOKEN_PX) <= max_tokens:
            return w, h
        s *= 0.99


@dataclass(frozen=True)
class Frame:
    """one capture, the desktop rect that got grabbed and the size of the image the model saw
    (just a scale per axis in between, nothing else)"""
    source: Rect
    image_w: int
    image_h: int

    @property
    def sx(self) -> float:
        return self.image_w / self.source.width

    @property
    def sy(self) -> float:
        return self.image_h / self.source.height

    # points are pixel indices (pixel N covers N..N+1 so its centre is N+0.5), that way the
    # models whole number answer lands on the centre of the desktop area it means
    def to_image(self, x: float, y: float) -> tuple[float, float]:
        return ((x + 0.5 - self.source.left) * self.sx - 0.5,
                (y + 0.5 - self.source.top) * self.sy - 0.5)

    def to_desktop(self, ix: float, iy: float) -> tuple[float, float]:
        return (self.source.left + (ix + 0.5) / self.sx - 0.5,
                self.source.top + (iy + 0.5) / self.sy - 0.5)

    def rect_to_image(self, r: Rect) -> tuple[float, float, float, float]:
        """this one works on edges instead of pixels. (x0, y0, x1, y1) in image px, x1/y1 exclusive"""
        return ((r.left - self.source.left) * self.sx, (r.top - self.source.top) * self.sy,
                (r.right - self.source.left) * self.sx, (r.bottom - self.source.top) * self.sy)

    def rect_to_desktop(self, x0: float, y0: float, x1: float, y1: float) -> Rect:
        left, top = self.source.left + x0 / self.sx, self.source.top + y0 / self.sy
        right, bottom = self.source.left + x1 / self.sx, self.source.top + y1 / self.sy
        return Rect(math.floor(left), math.floor(top),
                    math.ceil(right) - math.floor(left), math.ceil(bottom) - math.floor(top))

    def crop(self, region: Rect, image_w: int, image_h: int) -> "Frame":
        """zoomed in capture of part of the desktop, like for a second sharper look"""
        return Frame(region, image_w, image_h)


def to_window(x: float, y: float, window: Rect) -> tuple[int, int]:
    """desktop px -> the overlays own window px (same as what the C side does when it gets one)"""
    return round(x) - window.left, round(y) - window.top


def list_monitors() -> list[Monitor]:
    """the real monitors in physical px, needs set_dpi_aware() first"""
    return _win32_monitors()


def _win32_monitors() -> list[Monitor]:
    import ctypes
    from ctypes import wintypes as w

    class MonitorInfo(ctypes.Structure):
        _fields_ = [("cb", w.DWORD), ("rcMonitor", w.RECT), ("rcWork", w.RECT), ("dwFlags", w.DWORD)]

    found: list[Monitor] = []
    proc = ctypes.WINFUNCTYPE(w.BOOL, w.HMONITOR, w.HDC, ctypes.POINTER(w.RECT), w.LPARAM)

    def each(handle, _dc, _rect, _lp):
        info = MonitorInfo()
        info.cb = ctypes.sizeof(MonitorInfo)
        ctypes.windll.user32.GetMonitorInfoW(handle, ctypes.byref(info))
        dpi_x, dpi_y = w.UINT(96), w.UINT(96)
        try:
            ctypes.windll.shcore.GetDpiForMonitor(handle, 0, ctypes.byref(dpi_x), ctypes.byref(dpi_y))
        except (AttributeError, OSError):
            pass
        r = info.rcMonitor
        found.append(Monitor(Rect(r.left, r.top, r.right - r.left, r.bottom - r.top),
                             dpi_x.value, bool(info.dwFlags & 1)))
        return True

    ctypes.windll.user32.EnumDisplayMonitors(None, None, proc(each), 0)
    return found
