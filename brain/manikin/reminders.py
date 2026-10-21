"""reminders that survive restarts. due times are absolute UTC timestamps so closing the app,
a timezone change or daylight saving cant shift them. a ticker checks the wall clock once a
second (one long sleep() would drift if the laptop goes to sleep)
"""

import asyncio
import json
import logging
import os
import time
from pathlib import Path
from typing import Callable

log = logging.getLogger(__name__)


def _valid(r) -> bool:
    due = r.get("due") if isinstance(r, dict) else None
    return (isinstance(r, dict) and type(r.get("id")) is int and isinstance(r.get("text"), str)
            and isinstance(due, (int, float)) and not isinstance(due, bool))


class Reminders:
    def __init__(self, path: Path, on_fire: Callable[[dict], None]):
        self.path = path
        self._on_fire = on_fire
        self._items: list[dict] = []
        if path.exists():
            try:
                loaded = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as e:
                log.warning("ignoring unreadable %s: %s", path, e)
            else:
                # a hand edited or broken file shouldnt crash him at startup, keep the good ones
                self._items = [r for r in loaded if _valid(r)] if isinstance(loaded, list) else []
                if not isinstance(loaded, list) or len(self._items) != len(loaded):
                    log.warning("skipped malformed reminders in %s", path)

    def take_missed(self, now: float | None = None) -> list[dict]:
        """reminders that came due while the app was closed, removes them too"""
        now = time.time() if now is None else now
        missed = [r for r in self._items if r["due"] <= now]
        if missed:
            self._items = [r for r in self._items if r["due"] > now]
            self._save()
        return missed

    async def run(self) -> None:
        while True:
            await asyncio.sleep(1.0)
            self.fire_due()

    def fire_due(self, now: float | None = None) -> None:
        now = time.time() if now is None else now
        due = [r for r in self._items if r["due"] <= now]
        if not due:
            return
        self._items = [r for r in self._items if r["due"] > now]
        self._save()
        for r in due:
            self._on_fire(r)

    def add(self, text: str, due: float) -> dict:
        item = {"id": max((r["id"] for r in self._items), default=0) + 1, "text": text, "due": due}
        self._items.append(item)
        self._save()
        return item

    def cancel(self, reminder_id: int) -> bool:
        before = len(self._items)
        self._items = [r for r in self._items if r["id"] != reminder_id]
        if len(self._items) == before:
            return False
        self._save()
        return True

    def all(self) -> list[dict]:
        return sorted(self._items, key=lambda r: r["due"])

    def _save(self) -> None:
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self._items, indent=2), encoding="utf-8")
        os.replace(tmp, self.path)
