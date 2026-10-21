"""notes Manikin remembers between sessions, encrypted on disk. the key comes from the users
password through Argon2id (slow and memory hungry on purpose so guessing is expensive) and is
never stored, the notes are sealed with Fernet (AES + HMAC so tampering shows)

notes.enc = {"version": 1, "kdf": {...argon2id params...}, "salt": b64, "token": fernet token}
"""

import base64
import json
import os
from datetime import datetime
from pathlib import Path

from cryptography.fernet import Fernet, InvalidToken
from cryptography.hazmat.primitives.kdf.argon2 import Argon2id

KDF_PARAMS = {"iterations": 4, "lanes": 4, "memory_kib": 128 * 1024}
MIN_PASSWORD = 6


def _derive_key(password: str, salt: bytes, params: dict) -> bytes:
    # clamp the params from disk so a tampered file cant make us allocate gigabytes
    iterations = min(max(int(params["iterations"]), 1), 64)
    lanes = min(max(int(params["lanes"]), 1), 16)
    memory_kib = min(max(int(params["memory_kib"]), 8 * lanes), 1024 * 1024)
    raw = Argon2id(salt=salt, length=32, iterations=iterations, lanes=lanes,
                   memory_cost=memory_kib).derive(password.encode("utf-8"))
    return base64.urlsafe_b64encode(raw)


class NoteVault:
    def __init__(self, path: Path):
        self.path = path
        self._fernet: Fernet | None = None
        self._salt = b""
        self._params: dict = {}
        self._notes: list[dict] = []

    @property
    def exists(self) -> bool:
        return self.path.exists()

    @property
    def unlocked(self) -> bool:
        return self._fernet is not None

    def create(self, password: str) -> None:
        self._salt = os.urandom(16)
        self._params = dict(KDF_PARAMS)
        self._fernet = Fernet(_derive_key(password, self._salt, self._params))
        self._notes = []
        self._save()

    def unlock(self, password: str) -> bool:
        doc = json.loads(self.path.read_text(encoding="utf-8"))
        salt = base64.b64decode(doc["salt"])
        fernet = Fernet(_derive_key(password, salt, doc["kdf"]))
        try:
            notes = json.loads(fernet.decrypt(doc["token"].encode("ascii")))
        except InvalidToken:
            return False
        self._fernet, self._salt, self._params, self._notes = fernet, salt, doc["kdf"], notes
        return True

    def all(self) -> list[dict]:
        return list(self._notes)

    def add(self, text: str) -> dict:
        note = {
            "id": max((n["id"] for n in self._notes), default=0) + 1,
            "text": text,
            "saved": datetime.now().astimezone().strftime("%Y-%m-%d"),
        }
        self._notes.append(note)
        self._save()
        return note

    def delete(self, note_id: int) -> bool:
        before = len(self._notes)
        self._notes = [n for n in self._notes if n["id"] != note_id]
        if len(self._notes) == before:
            return False
        self._save()
        return True

    def _save(self) -> None:
        assert self._fernet is not None
        doc = {
            "version": 1,
            "kdf": self._params,
            "salt": base64.b64encode(self._salt).decode("ascii"),
            "token": self._fernet.encrypt(json.dumps(self._notes).encode("utf-8")).decode("ascii"),
        }
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(doc), encoding="utf-8")
        os.replace(tmp, self.path)    # atomic, a crash never leaves half a file
