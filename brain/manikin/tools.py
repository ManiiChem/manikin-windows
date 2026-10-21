"""client side tools, each one runs in a worker thread once the permission gate says yes.
returns a string or a list of content blocks (the screenshot returns an image), raise
ToolError for problems the model should hear about
"""

import base64
import io
import os
import re
import subprocess
import time
import webbrowser
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Callable
from urllib.parse import urlparse

import psutil

from .config import CHATTINESS, Settings
from .drive import Driver, drive_tool
from .gate import Gate
from .notes import NoteVault
from .pointer import Pointer, ask_text, find_and_point
from .reminders import Reminders

MAX_READ_BYTES = 200_000
MAX_WRITE_BYTES = 1_000_000
MAX_CLIPBOARD_CHARS = 8_000
SCREENSHOT_MAX_EDGE = 1568


class ToolError(Exception):
    pass


@dataclass
class Tool:
    name: str
    description: str
    params: dict[str, dict]
    run: Callable[[dict], Any]
    required: list[str] = field(default_factory=list)
    ask: Callable[[dict], str] | None = None    # question for the yes/no bubble
    precheck: Callable[[dict], None] | None = None   # raises ToolError before we bother the user
    session: bool = False       # the ask offers "yes for this session" too (gate.check)

    def schema(self) -> dict:
        return {
            "name": self.name,
            "description": self.description,
            "input_schema": {
                "type": "object",
                "properties": self.params,
                "required": self.required,
                "additionalProperties": False,
            },
        }

    def validate(self, args: dict) -> str | None:
        """checks the models arguments against params, returns an error or None"""
        types = {"string": str, "integer": int, "number": (int, float), "boolean": bool}
        for key in self.required:
            if key not in args:
                return f"missing argument {key!r}"
        for key, value in args.items():
            spec = self.params.get(key)
            if spec is None:
                return f"unknown argument {key!r}"
            kind = spec.get("type", "")
            expected = types.get(kind)
            if expected is None:
                continue
            # bool is a subclass of int in python but JSON true/false is only ever a boolean
            ok = isinstance(value, bool) if kind == "boolean" else (
                isinstance(value, expected) and not isinstance(value, bool))
            if not ok:
                return f"argument {key!r} should be a {kind}"
            if "enum" in spec and value not in spec["enum"]:
                return f"argument {key!r} should be one of {', '.join(map(str, spec['enum']))}"
        return None


@dataclass
class ChatRequests:
    """what the user asked to happen to the chat once the current reply is done. a tool cant
    touch the history mid turn (it has to stay intact till the model finishes) so these are
    flags the backend acts on after replying"""
    clear: bool = False
    compact: bool = False


@dataclass
class ToolContext:
    sandbox: Path
    vault: NoteVault
    reminders: Reminders
    chat: ChatRequests = field(default_factory=ChatRequests)
    settings: Settings | None = None     # these 4 are None in tests that dont need them
    pointer: Pointer | None = None
    driver: Driver | None = None
    gate: Gate | None = None


# thinking bubble labels while a tool runs, the missing ones are so quick a label would flicker
TOOL_STATUS = {
    "take_screenshot": "looking at your screen",
    "point_at": "looking for it",
    "drive": "taking the wheel",
    "system_info": "checking your PC",
    "list_apps": "looking through your apps",
    "read_file": "reading a file",
    "write_file": "writing a file",
    "read_clipboard": "reading your clipboard",
    "open_app_or_file": "opening it",
    "open_url": "opening it",
    "list_sandbox": "looking in my sandbox",
    "add_note": "writing that down",
    "list_notes": "checking my notes",
    "delete_note": "crossing that out",
    "set_reminder": "setting a reminder",
    "list_reminders": "checking my reminders",
    "cancel_reminder": "cancelling a reminder",
}


def get_time(_: dict) -> str:
    now = datetime.now().astimezone()
    offset = now.strftime("%z")
    return f"{now:%A %d %B %Y, %H:%M:%S} local time (UTC{offset[:3]}:{offset[3:]}, {now.tzname()})"


def _gb(n: float) -> str:
    return f"{n / 2**30:.1f} GB"


def _gpu_lines(names: dict[int, str]) -> list[str]:
    """whatever NVML will say about each NVIDIA GPU. every reading is best effort since an idle
    laptop dGPU powers down and then some queries fail (NVMLError_Unknown) while others still
    answer. a GPU that wont talk shouldnt cost us the CPU, RAM and disk lines"""
    try:
        import pynvml
        pynvml.nvmlInit()
    except Exception:
        return ["GPU: no NVIDIA GPU info available"]

    def ask(query, *args):
        try:
            return query(*args)
        except pynvml.NVMLError:
            return None

    lines = []
    try:
        for i in range(ask(pynvml.nvmlDeviceGetCount) or 0):
            h = ask(pynvml.nvmlDeviceGetHandleByIndex, i)
            if h is None:
                lines.append(f"GPU {i}: not answering right now")
                continue
            parts = [ask(pynvml.nvmlDeviceGetName, h) or "NVIDIA GPU"]
            util = ask(pynvml.nvmlDeviceGetUtilizationRates, h)
            temp = ask(pynvml.nvmlDeviceGetTemperature, h, pynvml.NVML_TEMPERATURE_GPU)
            mem = ask(pynvml.nvmlDeviceGetMemoryInfo, h)
            if util is not None:
                parts.append(f"{util.gpu}% load")
            if temp is not None:
                parts.append(f"{temp} C")
            if mem is not None:
                parts.append(f"VRAM {_gb(mem.used)} / {_gb(mem.total)}")
            if util is None and temp is None and mem is None:
                parts.append("asleep or not reporting (normal for an idle laptop GPU)")
            elif util is None or temp is None:
                parts.append("some readings unavailable (the GPU may be powered down)")
            lines.append(f"GPU {i}: " + ", ".join(parts))
            procs = [*(ask(pynvml.nvmlDeviceGetComputeRunningProcesses, h) or []),
                     *(ask(pynvml.nvmlDeviceGetGraphicsRunningProcesses, h) or [])]
            usage = {p.pid: p.usedGpuMemory for p in procs if p.usedGpuMemory}
            if usage:
                top = sorted(usage.items(), key=lambda kv: kv[1], reverse=True)[:5]
                lines.append("  VRAM by process: " + ", ".join(
                    f"{names.get(pid, pid)} {_gb(used)}" for pid, used in top))
            elif mem is not None:
                lines.append("  (the driver doesn't report VRAM per process on this system)")
    finally:
        try:
            pynvml.nvmlShutdown()
        except pynvml.NVMLError:
            pass
    return lines


def system_info(_: dict) -> str:
    cores = psutil.cpu_count() or 1
    procs = []
    for p in psutil.process_iter(["name", "memory_info"]):
        try:
            p.cpu_percent(None)      # first call primes the counter
            procs.append(p)
        except psutil.Error:
            pass
    psutil.cpu_percent(None)
    time.sleep(0.5)
    total_cpu = psutil.cpu_percent(None)

    rows, names = [], {}
    for p in procs:
        try:
            if p.pid == 0:           # the "System Idle Process"
                continue
            name = p.info["name"] or str(p.pid)
            names[p.pid] = name
            rss = p.info["memory_info"].rss if p.info["memory_info"] else 0
            rows.append((name, p.cpu_percent(None) / cores, rss))
        except psutil.Error:
            pass

    vm = psutil.virtual_memory()
    lines = [
        f"CPU: {total_cpu:.0f}% overall across {cores} threads",
        f"RAM: {_gb(vm.used)} / {_gb(vm.total)} ({vm.percent:.0f}%)",
        "Top CPU: " + ", ".join(f"{n} {c:.0f}%" for n, c, _ in
                                sorted(rows, key=lambda r: r[1], reverse=True)[:5]),
        "Top RAM: " + ", ".join(f"{n} {_gb(m)}" for n, _, m in
                                sorted(rows, key=lambda r: r[2], reverse=True)[:5]),
    ]
    for part in psutil.disk_partitions(all=False):
        try:
            du = psutil.disk_usage(part.mountpoint)
            lines.append(f"Disk {part.mountpoint}: {_gb(du.free)} free of {_gb(du.total)}")
        except OSError:
            pass
    battery = psutil.sensors_battery()
    if battery is not None:
        lines.append(f"Battery: {battery.percent:.0f}%, "
                     f"{'plugged in' if battery.power_plugged else 'on battery'}")
    lines += _gpu_lines(names)
    return "\n".join(lines)


def open_url(args: dict) -> str:
    url = args["url"].strip()
    if urlparse(url).scheme not in ("http", "https"):
        raise ToolError("only http and https links can be opened")
    webbrowser.open(url)
    return f"Opened {url} in the browser."


def _vault(ctx: ToolContext) -> NoteVault:
    if not ctx.vault.unlocked:
        raise ToolError("memory is locked this session (the user skipped the password)")
    return ctx.vault


def add_note(ctx: ToolContext, args: dict) -> str:
    note = _vault(ctx).add(args["text"].strip())
    return f"Saved note {note['id']}."


def list_notes(ctx: ToolContext, _: dict) -> str:
    notes = _vault(ctx).all()
    if not notes:
        return "No notes yet."
    return "\n".join(f"[{n['id']}] {n['text']} (saved {n['saved']})" for n in notes)


def delete_note(ctx: ToolContext, args: dict) -> str:
    if not _vault(ctx).delete(args["id"]):
        raise ToolError(f"there is no note {args['id']}")
    return f"Deleted note {args['id']}."


def _local(ts: float) -> str:
    return datetime.fromtimestamp(ts).astimezone().strftime("%a %d %b %H:%M")


def set_reminder(ctx: ToolContext, args: dict) -> str:
    minutes, at = args.get("minutes_from_now"), args.get("at_local_time")
    if (minutes is None) == (at is None):
        raise ToolError("give exactly one of minutes_from_now or at_local_time")
    if minutes is not None:
        if not 0 < minutes <= 60 * 24 * 365:
            raise ToolError("minutes_from_now must be between 0 and a year")
        due = time.time() + minutes * 60
    else:
        try:
            due = datetime.fromisoformat(at).astimezone().timestamp()   # naive = local time
        except ValueError:
            raise ToolError("at_local_time must be an ISO date-time like 2026-10-19T17:30")
        if due <= time.time():
            raise ToolError("that time is already in the past")
    item = ctx.reminders.add(args["text"].strip(), due)
    return f"Reminder {item['id']} set for {_local(due)}."


def list_reminders(ctx: ToolContext, _: dict) -> str:
    items = ctx.reminders.all()
    if not items:
        return "No reminders set."
    return "\n".join(f"[{r['id']}] {_local(r['due'])}: {r['text']}" for r in items)


def cancel_reminder(ctx: ToolContext, args: dict) -> str:
    if not ctx.reminders.cancel(args["id"]):
        raise ToolError(f"there is no reminder {args['id']}")
    return f"Cancelled reminder {args['id']}."


MAX_APPS_LISTED = 150
APP_CACHE_SECONDS = 60

# start menu links that arent apps (uninstallers, docs, web links etc)
_JUNK_LINK = re.compile(
    r"\b(uninst\w*|readme|read me|help|documentation|module docs|manuals?|user guide|"
    r"licen[cs]e|release notes|changelog|what'?s new|website|web site|on the web|faq|support|"
    r"report a problem)\b",
    re.IGNORECASE)


def windows_app_roots() -> list[Path]:
    roots = []
    for var in ("ProgramData", "AppData"):
        base = os.environ.get(var)
        if base:
            roots.append(Path(base) / "Microsoft" / "Windows" / "Start Menu" / "Programs")
    return roots


def scan_windows_apps(roots: list[Path]) -> list[tuple[str, str]]:
    """(display name, target) for each start menu shortcut, minus junk and duplicates"""
    apps, seen = [], set()
    for root in roots:
        if not root.is_dir():
            continue
        for link in sorted(root.rglob("*.lnk")):
            name = link.stem.strip()
            if not name or _JUNK_LINK.search(name) or name.lower() in seen:
                continue
            seen.add(name.lower())
            apps.append((name, str(link)))
    return apps


def scan_windows_store_apps() -> list[tuple[str, str]]:
    """packaged (Microsoft Store) apps like Calculator or Notepad, they dont have a .lnk file.
    Get-StartApps lists the whole start menu, we only keep entries with a package AppID
    (Name_publisher!App) since the shortcut scan covers the rest"""
    import json
    script = ("[Console]::OutputEncoding=[Text.Encoding]::UTF8; "
              "Get-StartApps | Select-Object Name,AppID | ConvertTo-Json -Compress")
    try:
        done = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                              capture_output=True, timeout=15,
                              creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        rows = json.loads(done.stdout.decode("utf-8-sig", errors="replace") or "[]")
    except (OSError, subprocess.SubprocessError, ValueError):
        return []
    return store_apps_from_start_apps(rows)


def store_apps_from_start_apps(rows) -> list[tuple[str, str]]:
    """keeps the packaged apps from Get-StartApps JSON (one object or a list of them)"""
    if isinstance(rows, dict):
        rows = [rows]
    if not isinstance(rows, list):
        return []
    apps = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        name, app_id = str(row.get("Name") or "").strip(), str(row.get("AppID") or "").strip()
        # Name_publisher!App ids are packages, paths and plain .exe ids are the .lnk scans job
        if (name and "!" in app_id and "\\" not in app_id and "/" not in app_id
                and not _JUNK_LINK.search(name)):
            apps.append((name, "shell:AppsFolder\\" + app_id))
    return apps


_app_cache: dict[tuple | None, tuple[float, list[tuple[str, str]]]] = {}


def installed_apps(roots: list[Path] | None = None, use_cache: bool = True) -> list[tuple[str, str]]:
    """every installed app as (display name, target for open_app_or_file), sorted by name"""
    key = tuple(str(r) for r in roots) if roots is not None else None
    hit = _app_cache.get(key)
    if use_cache and hit and time.monotonic() - hit[0] < APP_CACHE_SECONDS:
        return hit[1]
    apps = scan_windows_apps(windows_app_roots() if roots is None else roots)
    if roots is None:
        known = {name.lower() for name, _ in apps}
        apps += [a for a in scan_windows_store_apps() if a[0].lower() not in known]
    apps.sort(key=lambda a: a[0].lower())
    _app_cache[key] = (time.monotonic(), apps)
    return apps


def list_apps(args: dict, roots: list[Path] | None = None) -> str:
    query = (args.get("query") or "").strip().lower()
    apps = installed_apps(roots)
    if query:
        apps = [a for a in apps if query in a[0].lower() or query in Path(a[1]).stem.lower()]
        if not apps:
            return f"No installed app matches {query!r}. Try a shorter or different query."
    head = (f"{len(apps)} installed app{'s' * (len(apps) != 1)}" +(f" matching {query!r}" if query else "") +
            ". Pass the text after '->' exactly as target to open_app_or_file:")
    lines = [head] + [f"{name} -> {target}" for name, target in apps[:MAX_APPS_LISTED]]
    if len(apps) > MAX_APPS_LISTED:
        lines.append(f"[{len(apps) - MAX_APPS_LISTED} more not shown; "
                     "call list_apps with a query to narrow it down]")
    return "\n".join(lines)


def app_label(target: str) -> str:
    """the name list_apps showed for a target (for the yes/no bubble), else the target itself"""
    for _, apps in list(_app_cache.values()):
        for name, app_target in apps:
            if app_target == target:
                return name
    return target


def open_app_or_file(args: dict) -> str:
    target = os.path.expanduser(args["target"].strip())
    try:
        os.startfile(target)
    except OSError as e:
        raise ToolError(f"couldn't open {target}: {e.strerror or e}")
    return f"Opened {target}."


def read_clipboard(_: dict) -> str:
    import pyperclip
    try:
        text = pyperclip.paste()
    except pyperclip.PyperclipException as e:
        raise ToolError(f"clipboard unavailable: {e}")
    if not text:
        return "The clipboard is empty (or holds something that isn't text)."
    if len(text) > MAX_CLIPBOARD_CHARS:
        return text[:MAX_CLIPBOARD_CHARS] + f"\n[truncated, {len(text)} chars total]"
    return text


def read_file(args: dict) -> str:
    path = Path(os.path.expanduser(args["path"].strip()))
    if not path.is_file():
        raise ToolError(f"{path} is not a file")
    data = path.read_bytes()[:MAX_READ_BYTES + 1]
    if b"\x00" in data[:8192]:
        raise ToolError(f"{path.name} looks like a binary file")
    text = data[:MAX_READ_BYTES].decode("utf-8", errors="replace")
    if len(data) > MAX_READ_BYTES:
        text += f"\n[truncated after {MAX_READ_BYTES} bytes]"
    return text


def _resolve_in_sandbox(sandbox: Path, name: str, allow_root: bool) -> Path:
    root = sandbox.resolve()
    if not name.strip() or Path(name).is_absolute() or Path(name).drive:
        raise ToolError("use a relative file name inside the sandbox")
    target = (root / name).resolve()
    if (target == root and not allow_root) or not target.is_relative_to(root):
        raise ToolError("that path is outside the sandbox")
    return target


def sandbox_path(sandbox: Path, name: str) -> Path:
    """resolves name inside the sandbox following symlinks/junctions, or raises"""
    return _resolve_in_sandbox(sandbox, name, allow_root=False)


def write_file(ctx: ToolContext, args: dict) -> str:
    target = sandbox_path(ctx.sandbox, args["name"])
    data = args["content"].encode("utf-8")
    if len(data) > MAX_WRITE_BYTES:
        raise ToolError("file too large (1 MB max)")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return f"Wrote {len(data)} bytes to sandbox/{target.relative_to(ctx.sandbox.resolve()).as_posix()}."


MAX_LISTING_LINES = 400


def human_size(n: int) -> str:
    """like ls -h, so 512B 1.5K 23K 4.0M"""
    if n < 1024:
        return f"{n}B"
    for unit in "KMGT":
        n /= 1024
        if n < 1024 or unit == "T":
            return f"{n:.1f}{unit}" if n < 10 else f"{n:.0f}{unit}"
    return f"{n:.0f}T"


_IO_REPARSE_TAG_MOUNT_POINT = 0xA0000003      # a Windows junction


def _is_link(entry: os.DirEntry) -> bool:
    """symlink or windows junction, on any python version. only the junction reparse tag counts
    since OneDrive placeholders are reparse points too, and to the listing those are just files"""
    if entry.is_symlink():
        return True
    if hasattr(entry, "is_junction"):                # Python 3.12+
        return entry.is_junction()
    try:
        tag = getattr(entry.stat(follow_symlinks=False), "st_reparse_tag", 0)
    except OSError:
        return False
    return tag == _IO_REPARSE_TAG_MOUNT_POINT


def list_sandbox(ctx: ToolContext, args: dict) -> str:
    root = ctx.sandbox.resolve()
    rel = (args.get("path") or "").strip().replace("\\", "/")
    if len(rel) > 1:
        rel = rel.rstrip("/")             # a leading / stays so absolute paths still get refused
    if rel in ("sandbox",) or rel.startswith("sandbox/"):       # the model echoing our own labels
        if not (root / rel).exists():
            rel = rel[len("sandbox"):].lstrip("/")
    start = root if rel in ("", ".") else _resolve_in_sandbox(ctx.sandbox, rel, allow_root=True)
    if not start.exists():
        raise ToolError(f"sandbox/{rel} doesn't exist")

    def label(p: Path) -> str:
        r = p.relative_to(root).as_posix()
        return "sandbox" if r == "." else f"sandbox/{r}"

    def row(kind: str, size: str, mtime: float, name: str) -> str:
        when = datetime.fromtimestamp(mtime).strftime("%Y-%m-%d %H:%M")
        return f"{kind} {size:>6} {when} {name}"

    if start.is_file():
        st = start.stat()
        return row("-", human_size(st.st_size), st.st_mtime, label(start))

    lines, n_dirs, n_files, n_bytes, truncated = [], 0, 0, 0, False
    pending = [start]
    while pending:
        folder = pending.pop(0)
        try:
            with os.scandir(folder) as it:
                entries = list(it)
        except OSError as e:
            lines.append(f"{label(folder)}: can't read ({e.strerror or e})")
            continue
        dirs, files = [], []
        for e in entries:
            try:
                st = e.stat(follow_symlinks=False)
            except OSError:
                continue
            if _is_link(e):
                try:
                    dest = Path(e.path).resolve()
                    where = label(dest) if dest.is_relative_to(root) else "outside the sandbox"
                except (OSError, RuntimeError):
                    where = "unresolvable"
                files.append((e.name, row("l", "-", st.st_mtime, f"{e.name} -> {where} (not followed)")))
            elif e.is_dir(follow_symlinks=False):
                dirs.append((e.name, row("d", "-", st.st_mtime, e.name + "/")))
                n_dirs += 1
            else:
                files.append((e.name, row("-", human_size(st.st_size), st.st_mtime, e.name)))
                n_files += 1
                n_bytes += st.st_size
        dirs.sort(key=lambda d: d[0].lower())
        files.sort(key=lambda f: f[0].lower())
        if args.get("recursive"):
            pending[:0] = [folder / name for name, _ in dirs]    # depth first, in order
        if lines:
            lines.append("")
        lines.append(f"{label(folder)}:")
        lines += [r for _, r in dirs] + [r for _, r in files] or ["(empty)"]
        if len(lines) > MAX_LISTING_LINES:
            truncated = True
            break
    if truncated:
        lines = lines[:MAX_LISTING_LINES]
        lines.append(f"[listing cut off after {MAX_LISTING_LINES} lines; "
                     "list a subfolder with path to see more]")
    lines.append(f"total: {n_dirs} folder{'s' * (n_dirs != 1)}, {n_files} file{'s' * (n_files != 1)}, "
                 f"{human_size(n_bytes)}" + (" (so far)" if truncated else ""))
    return "\n".join(lines)


def take_screenshot(_: dict) -> list[dict]:
    import mss
    from PIL import Image
    try:
        with mss.MSS() as grabber:
            shot = grabber.grab(grabber.monitors[0])     # all monitors as one image
    except Exception as e:
        raise ToolError(f"couldn't capture the screen: {e}")
    img = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
    img.thumbnail((SCREENSHOT_MAX_EDGE, SCREENSHOT_MAX_EDGE))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=80)
    return [
        {"type": "image", "source": {"type": "base64", "media_type": "image/jpeg",
                                     "data": base64.standard_b64encode(buf.getvalue()).decode("ascii")}},
        {"type": "text", "text": f"Screenshot of the user's screen ({shot.size.width}x{shot.size.height}, "
                                 f"scaled to {img.width}x{img.height})."},
    ]


MAX_SAY_CHARS = 120


def _point_target(args: dict) -> str:
    target = args["target"].strip()
    if not target:
        raise ToolError("target is empty; say plainly what to find")
    return target


def point_at(ctx: ToolContext, args: dict) -> str:
    if ctx.pointer is None:
        raise ToolError("pointing isn't available right now")
    say = (args.get("say") or "").strip()[:MAX_SAY_CHARS]
    try:
        return find_and_point(ctx.pointer, _point_target(args), say, args.get("wait", False))
    except RuntimeError as e:
        raise ToolError(str(e))


def clear_chat(ctx: ToolContext, _: dict) -> str:
    ctx.chat.clear = True
    return "The conversation will be wiped right after this reply. Notes are kept."


def compact_chat(ctx: ToolContext, _: dict) -> str:
    ctx.chat.compact = True
    return "The conversation will be shrunk to a summary right after this reply."


_CHATTINESS_DONE = {
    "often": "Done: you'll pipe up on your own fairly often now.",
    "sometimes": "Done: you'll pipe up on your own now and then.",
    "rarely": "Done: you'll only rarely pipe up on your own.",
    "off": "Done: you won't speak unless spoken to.",
}


def set_chattiness(ctx: ToolContext, args: dict) -> str:
    if ctx.settings is None:
        raise ToolError("settings aren't available right now")
    level = args["level"]
    if level not in CHATTINESS:
        raise ToolError(f"level must be one of {', '.join(CHATTINESS)}")
    ctx.settings.set_chattiness(level)
    return _CHATTINESS_DONE[level]


def describe_sound(sound: dict) -> str:
    """the state for confirmations, like 'volume 60%, sound on, babble voice on'"""
    return (f"volume {sound['volume']}%, sound {'muted' if sound['muted'] else 'on'}, "
            f"babble voice {'on' if sound['voice'] else 'off'}")


def set_sound(ctx: ToolContext, args: dict) -> str:
    if ctx.settings is None:
        raise ToolError("settings aren't available right now")
    if not any(key in args for key in ("volume", "muted", "voice")):
        # include the current state so a model that doesnt know the level can still pick one
        raise ToolError(f"give at least one of volume, muted or voice (now: {describe_sound(ctx.settings.sound)})")
    volume = args.get("volume")
    # validate() only checks the type, the range gets checked here
    if volume is not None and not 0 <= volume <= 100:
        raise ToolError("volume must be from 0 to 100")
    try:
        sound = ctx.settings.set_sound(volume, args.get("muted"), args.get("voice"))
    except ValueError as e:                  # wrong type, if validate() got skipped
        raise ToolError(str(e))
    return f"Done: {describe_sound(sound)}."


MAX_PLAN_STEPS = 8


def _drive_plan(args: dict) -> list[str]:
    """the plan the user approves, 1 to MAX_PLAN_STEPS short non empty lines"""
    plan = args.get("plan")
    if not isinstance(plan, list) or not plan or len(plan) > MAX_PLAN_STEPS:
        raise ToolError(f"plan must be a list of 1 to {MAX_PLAN_STEPS} short steps")
    if not all(isinstance(step, str) and step.strip() for step in plan):
        raise ToolError("every plan step must be a non-empty string")
    if not str(args.get("task", "")).strip():
        raise ToolError("task is empty; say what to do and with what details")
    return [step.strip() for step in plan]


def drive_ask(args: dict) -> str:
    """text for the plan bubble"""
    try:
        plan = _drive_plan(args)
    except ToolError:
        return "Can I take the wheel?"
    return "Can I take the wheel? I'll:\n" + "\n".join(f"{i}. {step}" for i, step in enumerate(plan, 1))


def stop_trusting(ctx: ToolContext, _: dict) -> str:
    if ctx.gate is None:
        raise ToolError("there's nothing to take back right now")
    ctx.gate.revoke_session()
    return "Done: I'll ask before taking the wheel again."


def build_tools(ctx: ToolContext) -> list[Tool]:
    def with_ctx(fn):
        return lambda args: fn(ctx, args)

    text = {"type": "string"}
    return [
        Tool("get_time", "Get the current local date, time and timezone. You don't know the time otherwise.",
             {}, get_time),
        Tool("system_info",
             "Check the computer's health: CPU and RAM load, the top processes by CPU and RAM, "
             "disk space, battery, and NVIDIA GPU load, temperature and VRAM.",
             {}, system_info),
        Tool("open_url", "Open an http(s) link in the user's default browser.",
             {"url": text}, open_url, ["url"]),
        Tool("add_note",
             "Save a note to your encrypted long-term memory (survives restarts). Use for things the "
             "user asks you to remember or lasting preferences. Never store passwords or secrets.",
             {"text": text}, with_ctx(add_note), ["text"]),
        Tool("list_notes", "List everything in your long-term memory, with note ids.",
             {}, with_ctx(list_notes)),
        Tool("delete_note", "Delete a note from your long-term memory by id.",
             {"id": {"type": "integer"}}, with_ctx(delete_note), ["id"]),
        Tool("set_reminder",
             "Set a reminder that pops up in your speech bubble when it's due. Give exactly one of "
             "minutes_from_now or at_local_time (ISO, e.g. 2026-10-19T17:30).",
             {"text": text, "minutes_from_now": {"type": "number"}, "at_local_time": text},
             with_ctx(set_reminder), ["text"]),
        Tool("list_reminders", "List pending reminders with their ids and due times.",
             {}, with_ctx(list_reminders)),
        Tool("cancel_reminder", "Cancel a pending reminder by id.",
             {"id": {"type": "integer"}}, with_ctx(cancel_reminder), ["id"]),
        Tool("open_app_or_file",
             "Open an application, file or folder on the user's computer, by name or path. "
             "For apps, prefer a target from list_apps. The user is asked first.",
             {"target": text}, open_app_or_file, ["target"],
             ask=lambda a: f"Can I open {app_label(a.get('target', '?'))}?"),
        Tool("read_clipboard", "Read the text on the user's clipboard. The user is asked first.",
             {}, read_clipboard, ask=lambda a: "Can I read your clipboard?"),
        Tool("read_file", "Read a text file the user points you to, by path. The user is asked first.",
             {"path": text}, read_file, ["path"],
             ask=lambda a: f"Can I read {a.get('path', '?')}?"),
        Tool("write_file",
             "Create or overwrite a text file in your sandbox folder (the only place you can write). "
             "name is relative to the sandbox, e.g. notes/todo.txt. The user is asked first.",
             {"name": text, "content": text}, with_ctx(write_file), ["name", "content"],
             ask=lambda a: f"Can I write sandbox/{a.get('name', '?')}?",
             precheck=lambda a: sandbox_path(ctx.sandbox, a["name"]) and None),
        Tool("list_sandbox",
             "List your sandbox folder like ls -l: type (d folder, - file, l link), size, "
             "last modified time and name, folders first. path is an optional subfolder relative "
             "to the sandbox (e.g. notes; default is the whole sandbox); set recursive to true to "
             "include everything inside subfolders too.",
             {"path": text, "recursive": {"type": "boolean"}}, with_ctx(list_sandbox)),
        Tool("list_apps",
             "List the applications installed on the user's computer, each with the exact target "
             "to pass to open_app_or_file. Use it before opening an app by name so you open "
             "something that really exists. query filters by name (case-insensitive substring, "
             "e.g. spotify); use one whenever you know roughly what you're looking for.",
             {"query": text}, list_apps),
        Tool("take_screenshot", "Take a screenshot of the user's screen so you can see it. "
             "The user is asked first.",
             {}, take_screenshot, ask=lambda a: "Can I take a screenshot of your screen?"),
        Tool("point_at",
             "Find something in the window the user is working in, then walk over and point at "
             "it. Use it when they ask where something is on their screen or how to do something "
             "in an app. target: what to find, plainly (e.g. \"the Display resolution dropdown\"). "
             "say: an optional short line kept in your bubble while pointing, for a step of "
             "instructions (e.g. \"Click Display resolution\"). wait: true to wait until the user "
             "clicks before returning (for step-by-step walkthroughs); the result then says what "
             "they did. Every call takes a fresh look. The user is asked first.",
             {"target": text, "say": text, "wait": {"type": "boolean"}},
             with_ctx(point_at), ["target"], ask=lambda a: ask_text(),
             precheck=lambda a: _point_target(a) and None),
        Tool("drive",
             "Take the wheel: drive the user's real mouse and keyboard yourself, clicking and "
             "typing while they watch. Only when they ask you to do something for them (\"do it "
             "for me\", \"you do it\"), never on your own. task: what to do, with every detail "
             "needed (e.g. the values to type into a form). plan: 1 to 8 short steps the user "
             "approves first, e.g. [\"Open Settings\", \"Display > Night light\", \"Turn it on\"]. "
             "The part of you that drives sees only the screen, task and plan, not this chat. "
             "Passwords, payments and deleting are never yours: you point and the user does "
             "them. Sending, submitting, closing unsaved work and finishing installs are asked "
             "about on the spot. To open a link or an app, open_url and open_app_or_file are "
             "quicker. The result says how it went.",
             {"task": text, "plan": {"type": "array", "items": text}},
             with_ctx(drive_tool), ["task", "plan"], ask=drive_ask,
             precheck=lambda a: _drive_plan(a) and None, session=True),
        Tool("stop_trusting",
             "The user wants to be asked before you take the wheel again (\"ask me first\", "
             "\"stop doing things without asking\"): forget their \"yes for this session\".",
             {}, with_ctx(stop_trusting)),
        Tool("clear_chat",
             "Forget this whole conversation and start fresh (long-term notes are kept). Use when "
             "the user asks to clear, reset or start a new chat. Takes effect after your reply.",
             {}, with_ctx(clear_chat)),
        Tool("compact_chat",
             "Shrink this conversation into a short summary to save tokens, keeping what matters. "
             "Use when the user asks to compact or summarize the chat. Takes effect after your reply.",
             {}, with_ctx(compact_chat)),
        Tool("set_chattiness",
             "Set how often you talk on your own, without being spoken to: often, sometimes, "
             "rarely, or off (only answer when spoken to). Use it when the user asks you to "
             "talk more or less, or to be quiet. Saved across restarts.",
             {"level": {"type": "string", "enum": list(CHATTINESS)}},
             with_ctx(set_chattiness), ["level"]),
        Tool("set_sound",
             "Change your sound effects and your babble voice (the beeps while your words type "
             "out). volume: 0 to 100, default 30, for \"be quieter\", \"turn it down\" or \"turn "
             "it up\" (go about 10 up or down unless they say how much; the result tells you the "
             "new level, so you know it next time). muted: true for \"mute yourself\", "
             "\"no sounds\"; false for \"unmute\", \"sound back on\". voice: false for \"stop "
             "babbling\", \"stop beeping when you talk\"; true for \"turn your voice back on\". "
             "Give only what should change, at least one. Saved across restarts. This is your "
             "sound, not how often you talk: for that use set_chattiness.",
             {"volume": {"type": "integer"}, "muted": {"type": "boolean"}, "voice": {"type": "boolean"}},
             with_ctx(set_sound)),
    ]
