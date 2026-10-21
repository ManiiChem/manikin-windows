"""permission gate, every tool call goes through check() before it runs.

plain python on purpose so the model never gets a say in whether its allowed, it just
finds out from the tool result

an ask can offer a third button, "yes for this session" (the drive plan does). only the users
click on that button, on an ask that actually offered it, trusts that one tool until
revoke_session() or a restart. the only way in is answer() and only the overlays "answer"
messages reach it, so nothing the model sends can grant it. confirm() is for the asks inside
a drive (Submit, Send, "carry on?") and asks every time no matter how much you trust him
"""

import asyncio

from .protocol import Pipe, to_display

# point_at asks once per turn since a walkthrough points every step. the other ask tools
# ask every call because each one can touch something different (another file say)
PER_TURN = frozenset({"point_at"})       # one yes covers the rest of the turn


class Gate:
    def __init__(self, pipe: Pipe, levels: dict[str, str], timeout_s: float,
                 per_turn: frozenset[str] = PER_TURN):
        self._pipe = pipe
        self._levels = levels
        self._timeout_s = timeout_s
        self._per_turn = per_turn
        self._granted: set[str] = set()      # allowed earlier in this turn
        self._trusted: set[str] = set()      # "yes for this session"
        self._pending: dict[int, asyncio.Future] = {}
        self._offers: dict[int, str] = {}    # ask id -> tool, for open asks showing the session button
        self._next_id = 0

    def level(self, tool: str) -> str:
        return self._levels.get(tool, "deny")     # unknown tools are never allowed

    def reset_turn(self) -> None:
        self._granted.clear()

    async def check(self, tool: str, question: str, proactive: bool = False,
                    session: bool = False) -> str | None:
        """None if the tool can run, otherwise why not (that goes back to the model)

        proactive = hes making a remark on his own, a permission bubble out of nowhere would be
        alarming so "ask" tools just get refused then, trusted or not. session = offer the third
        "yes for this session" button, which stops the asking until revoke_session() or a restart
        """
        level = self.level(tool)
        if level == "allow":
            return None
        if level != "ask":
            return f"The user's permission settings block {tool}."
        if proactive:
            return (f"{tool} is not allowed during an unprompted remark (it needs the user's "
                    "permission). Don't mention it; just say your line without it.")
        if tool in self._granted or tool in self._trusted:
            return None

        allowed = await self._ask(question, offer=tool if session else None)
        if allowed is None:
            return "The user didn't answer the permission prompt, so the tool did not run."
        if not allowed:
            return "The user declined, so the tool did not run."
        if tool in self._per_turn:
            self._granted.add(tool)
        return None

    async def confirm(self, text: str) -> bool:
        """always asks yes/no whatever was granted, for the asks inside a drive (pressing
        Submit, "carry on?"). True only on an explicit yes, a timeout is a no"""
        return await self._ask(text) is True

    def revoke_session(self) -> None:
        """forget every "yes for this session" (the stop_trusting tool)"""
        self._trusted.clear()

    def answer(self, ask_id: object, allow: object, scope: object = None) -> None:
        """scope is "session" when the user clicked the third button on an ask that offered it"""
        if not isinstance(ask_id, int) or isinstance(ask_id, bool):
            return
        future = self._pending.get(ask_id)
        if future is None or future.done():
            return
        allowed = allow is True              # only an explicit yes counts
        tool = self._offers.get(ask_id)      # None if this ask had no session button
        if allowed and scope == "session" and tool is not None:
            self._trusted.add(tool)
        future.set_result(allowed)

    def cancel_all(self) -> None:
        """the user cancelled the turn, hide every open prompt and treat it as a no"""
        for ask_id, future in list(self._pending.items()):
            if not future.done():        # done = already answered or cancelled
                self._pipe.send("ask_cancel", id=ask_id)
                future.set_result(False)

    async def _ask(self, question: str, offer: str | None = None) -> bool | None:
        """shows a yes/no bubble and waits. True/False, or None if nobody answered in time
        (the bubble gets hidden then). offer = the tool a "yes for this session" would
        trust, the bubble only shows that third button when its set"""
        self._next_id += 1
        ask_id = self._next_id
        future = asyncio.get_running_loop().create_future()
        self._pending[ask_id] = future
        fields = {"id": ask_id, "text": to_display(question)}
        if offer is not None:
            self._offers[ask_id] = offer
            fields["session"] = True
        self._pipe.send("ask", **fields)
        try:
            return await asyncio.wait_for(future, self._timeout_s)
        except TimeoutError:
            self._pipe.send("ask_cancel", id=ask_id)
            return None
        finally:
            self._pending.pop(ask_id, None)
            self._offers.pop(ask_id, None)
