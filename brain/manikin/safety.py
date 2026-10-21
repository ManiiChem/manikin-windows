"""the rules for what he can do while driving. plain code like gate.py so the model never
decides, it only hears afterwards what happened

handover (he points, the user does it): passwords, payments, deleting. ask (even when the
user trusts him for the session): sending or posting, submitting, closing unsaved work and
the button that finishes an install. everything else goes, strictest match wins

names match as whole english words, any case ("Display" doesnt trigger "pay"). when UIA
knows nothing about an element (None, games, apps that draw their own UI) only the text
and key rules apply and the users plan approval is the guard
"""

import re

from .drive_types import GO, Action, Decision, Facts
from .protocol import MODIFIER_KEYS

# what names mean

# a field with one of these in its name counts as a password box
PASSWORD_WORDS = ("password", "passwords", "passcode", "passphrase", "pin code")

# buttons and links that spend money (no plain "subscribe", on youtube thats a channel)
PAYMENT_WORDS = (
    "pay", "pay now", "buy", "buy now", "purchase", "checkout",
    "place order", "place your order", "complete purchase", "complete order",
    "confirm order", "confirm purchase", "confirm payment", "submit payment",
    "make payment", "make a payment", "proceed to payment", "order now",
    "subscribe and pay", "pay and subscribe", "donate", "paypal", "gpay",
)

# card or bank fields, anything typed in them counts (a lone CVV or expiry isnt a card number)
CARD_FIELD_WORDS = ("card number", "credit card", "debit card", "cvv", "cvc", "csc",
                    "security code", "expiration date", "expiry date", "iban",
                    "routing number")

# buttons and menu items that throw stuff away for good (or nearly)
DELETE_WORDS = (
    "delete", "remove", "trash", "erase", "wipe", "permanently delete",
    "delete forever", "discard permanently", "move to trash", "move to bin",
    "move to recycle bin", "empty trash", "empty bin", "empty recycle bin",
    "clear browsing data", "clear history", "clear all history", "factory reset",
    "reset this pc",
)

# navigation that only looks like the above, cut out of names before matching so
# "Purchase history" stays a page and "Delete purchase history" still deletes
NOT_ACTIONS = ("purchase history", "order history", "where to buy", "how to buy")

# buttons that send something to other people
SEND_WORDS = ("send", "post", "publish", "reply", "tweet", "retweet", "repost",
              "comment", "share")

# buttons that hand in a form, anywhere
SUBMIT_WORDS = ("submit", "sign up", "signup", "register", "confirm",
                "create account", "create my account")

# only when the button is in a form, anywhere else "Continue" is just the next page
FORM_SUBMIT_WORDS = ("continue",)

# a button whose whole name is one of these closes the window or tab (title bar Close,
# File > Exit). "Close <window title>" counts too, Store apps use "Close Settings" etc
CLOSE_NAMES = ("close", "close window", "close tab", "close all tabs",
               "close other tabs", "exit", "quit")

# save prompts and such, these throw away unsaved work
DISCARD_WORDS = ("don't save", "dont save", "do not save", "discard",
                 "close without saving", "quit without saving")

# what an installers finalizing button is called (the whole first line of its name).
# Next, Back, I Agree and Cancel are free, one ask per install at the very end
INSTALL_FINAL_NAMES = ("install", "install now", "uninstall", "uninstall now",
                       "finish", "remove")

# signs a window is an installer. process names match as substrings since they run
# together ("chromesetup.exe", "unins000.exe"), titles match as whole words
INSTALLER_PROCESSES = ("msiexec", "setup", "install", "unins")
INSTALLER_TITLE_WORDS = ("setup", "installer", "install", "installation",
                         "uninstall", "uninstaller", "installshield", "setup wizard")
INSTALLER_CLASSES = ("#32770",)    # the classic dialog every old style wizard is made of

# search boxes, Enter there only searches even inside a web form (googles is one)
SEARCH_WORDS = ("search", "find")

# chat apps, Enter in any of their text boxes sends
CHAT_APPS = ("discord.exe", "slack.exe", "ms-teams.exe", "teams.exe", "whatsapp.exe",
             "telegram.exe", "signal.exe", "messenger.exe")

# text boxes anywhere where Enter sends. kept tight on purpose cause a contact forms
# "Message" box is a textarea, but "Message <channel or person>" ("Message #general") counts
CHAT_BOX_WORDS = ("type a message", "type a new message", "write a message",
                  "send a message", "write a comment", "write a public comment",
                  "write a reply", "reply to", "chat", "post text", "tweet text")

# mail apps, their big text box is the email body where Enter is a new line and
# Ctrl+Enter sends. bodies in a browser (gmails "Message Body") go by the word "body"
MAIL_APPS = ("outlook.exe", "olk.exe", "thunderbird.exe")
BODY_WORDS = ("body",)

# UIA types that act when clicked, their names are commands so a keyword anywhere counts
BUTTON_KINDS = ("Button", "SplitButton", "MenuItem", "")      # "" = kind unknown
# links and the text or image inside one (browsers often report the text leaf). their names
# are often prose so the keyword has to lead a short name, "How to delete your profile" goes
LINK_KINDS = ("Hyperlink", "Text", "Image")
LINK_MAX_WORDS = 5
# any other kind (ListItem, Edit, CheckBox...) is content, a file called "delete me.txt" is just a file

# where the Delete key throws away the selected item instead of a character
LIST_KINDS = ("ListItem", "DataItem", "TreeItem", "List", "Tree", "DataGrid")
FILE_VIEW_CLASSES = ("CabinetWClass", "Progman", "WorkerW")   # Explorer, the desktop

# keys that leave a password box without typing in it
LEAVING_KEYS = ("tab", "escape")

# key combos that close the window or tab, as (modifiers that have to be held, key)
CLOSE_KEYS = ((("alt",), "f4"), (("ctrl",), "w"), (("ctrl",), "q"), (("ctrl",), "f4"))

# what he says

PASSWORD_REASON = "That's a password box. Your turn!"
CARD_FIELD_REASON = "That's a card field. Your turn!"
CARD_TEXT_REASON = "That looks like a card number. You type that one!"
PAYMENT_REASON = "That's a payment button. You do that one."
DELETE_REASON = "That deletes stuff. You do that one!"
DELETE_KEY_REASON = "Delete would throw that away. You do that one!"
# asks are yes/no questions, the driver hands them to gate.confirm
CHAT_REASON = "Send that message?"
ENTER_REASON = "Press Enter to submit?"
NEWLINE_REASON = "That Enter might submit something. Go ahead?"
CLOSE_REASON = "Unsaved stuff could vanish. Close it?"
DISCARD_REASON = "Throw away the changes?"


def check(action: Action, target: Facts | None, focused: Facts | None) -> Decision:
    """target is the element at the actions point (move, click, drag start, scroll),
    focused is where typed text and keys go. either can be None (nothing known)"""
    if action.kind == "click":
        if target is None or action.button != "left":    # a right click opens a menu
            return GO
        return _press(target)
    if action.kind == "type":
        return _type(action.text, focused)
    if action.kind == "key":
        return _key(action.keys, focused)
    return GO       # move, drag and scroll dont press anything


# clicks

def _press(t: Facts) -> Decision:
    """clicking t, or pressing Enter or Space while it has focus"""
    if _says(t, PAYMENT_WORDS):
        return Decision("handover", "payment", PAYMENT_REASON)
    # before the delete rule since an installers "Remove" is its finish button and asks once
    # like Install. only in a window that says its an installer, in credential manager it deletes
    final = _first_line(t.name)
    if _acts(t) and final in INSTALL_FINAL_NAMES and _installer(t, any_dialog=final != "remove"):
        return Decision("ask", "install", _press_reason(t, "Finish the install?"))
    if _says(t, DELETE_WORDS):
        return Decision("handover", "delete", DELETE_REASON)
    if _says(t, SEND_WORDS):
        return Decision("ask", "send", _press_reason(t, "Send it?"))
    if _says(t, SUBMIT_WORDS) or (t.in_form and _says(t, FORM_SUBMIT_WORDS)):
        return Decision("ask", "submit", _press_reason(t, "Submit it?"))
    if _closes(t):
        return Decision("ask", "close", CLOSE_REASON)
    if _says(t, DISCARD_WORDS):
        return Decision("ask", "close", DISCARD_REASON)
    return GO


def _says(t: Facts, phrases: tuple[str, ...]) -> bool:
    """t acts when clicked and its name says one of the phrases"""
    words = _words(t.name)
    for skip in NOT_ACTIONS:
        words = words.replace(f" {skip} ", " ")
    if t.kind in BUTTON_KINDS:
        return any(f" {p} " in words for p in phrases)
    if t.kind in LINK_KINDS:
        return (len(words.split()) <= LINK_MAX_WORDS
                and any(words.startswith(f" {p} ") for p in phrases))
    return False


def _closes(t: Facts) -> bool:
    """a Close button (or Exit, Quit) for the window or tab"""
    if not _acts(t):
        return False
    words = _words(t.name).strip()
    title = _words(t.window_title).strip()
    return words in CLOSE_NAMES or (bool(title) and words == f"close {title}")


def _acts(t: Facts) -> bool:
    return t.kind in BUTTON_KINDS or t.kind in LINK_KINDS


def _installer(t: Facts, any_dialog: bool = True) -> bool:
    """t's window looks like an (un)installer. any_dialog lets a bare #32770 dialog count"""
    process = t.process.lower()
    return (any(hint in process for hint in INSTALLER_PROCESSES)
            or _has(t.window_title, INSTALLER_TITLE_WORDS)
            or (any_dialog and t.window_class in INSTALLER_CLASSES))


def _press_reason(t: Facts, fallback: str) -> str:
    """makes "Press Submit?" out of the buttons own name"""
    name = " ".join(t.name.split("\n", 1)[0].split())
    if not name:
        return fallback
    if len(name) > 30:
        name = name[:27].rstrip() + "..."
    return f"Press {name}?"


# typing

def _type(text: str, f: Facts | None) -> Decision:
    if f is not None and _password_box(f):
        return Decision("handover", "password", PASSWORD_REASON)
    if f is not None and _card_field(f):
        return Decision("handover", "payment", CARD_FIELD_REASON)
    if has_card_number(text):
        return Decision("handover", "payment", CARD_TEXT_REASON)
    if "\n" in text or "\r" in text:
        # a typed newline is an Enter press, with no focus info it might be a form field so ask
        if f is None:
            return Decision("ask", "submit", NEWLINE_REASON)
        return _enter(set(), f)
    if " " in text and f is not None and _pressable(f):
        return _press(f)          # space presses a focused button
    return GO


def has_card_number(text: str) -> bool:
    """text has a run of 13-19 digits that passes Luhn. single spaces or dashes can split
    it (one kind per number, like "4242 4242 ..."), and the run can start or stop at any
    split so "4242 4242 4242 4242 12/28" still counts but two phone numbers dont"""
    for run in re.findall(r"\d+(?:[ -]\d+)*", text):
        parts = re.split(r"([ -])", run)     # digits, separator, digits, ...
        groups, seps = parts[::2], parts[1::2]
        for start in range(len(groups)):
            digits = ""
            for end in range(start, len(groups)):
                if end > start and seps[end - 1] != seps[start]:
                    break                    # a space after dashes means another number
                digits += groups[end]
                if len(digits) > 19:
                    break
                if len(digits) >= 13 and _luhn(digits):
                    return True
    return False


def _luhn(digits: str) -> bool:
    total = 0
    for i, ch in enumerate(reversed(digits)):
        n = int(ch)
        if i % 2:
            n = n * 2 - 9 if n > 4 else n * 2
        total += n
    return total % 10 == 0


# keys

def _key(keys: tuple[str, ...], f: Facts | None) -> Decision:
    mods = {k for k in keys if k in MODIFIER_KEYS}
    main = {k for k in keys if k not in MODIFIER_KEYS}
    types_in = not (main <= set(LEAVING_KEYS) or "alt" in mods or "super" in mods)
    if f is not None and types_in and _password_box(f):
        return Decision("handover", "password", PASSWORD_REASON)
    if f is not None and types_in and _card_field(f):
        return Decision("handover", "payment", CARD_FIELD_REASON)
    if "delete" in main:
        # shift+delete skips the bin, with no focus info thats likelier than cutting text
        if (f is None and "shift" in mods) or (f is not None and _file_view(f)):
            return Decision("handover", "delete", DELETE_KEY_REASON)
    if (main == {"d"} and mods == {"ctrl"} and f is not None and not _editable(f)
            and f.window_class in FILE_VIEW_CLASSES):
        return Decision("handover", "delete", DELETE_KEY_REASON)   # explorers ctrl+d deletes too
    if "enter" in main:
        if f is None:
            return GO
        return _enter(mods, f)
    if main == {"space"} and not mods and f is not None and _pressable(f):
        return _press(f)
    for needed, key in CLOSE_KEYS:
        if key in main and set(needed) <= mods:
            return Decision("ask", "close", CLOSE_REASON)
    return GO


def _enter(mods: set[str], f: Facts) -> Decision:
    """Enter (with these modifiers held) at the focus f"""
    if _pressable(f):
        return _press(f)          # enter presses a focused button or link
    if not _editable(f) or mods - {"ctrl"}:
        return GO                 # shift+enter, alt+enter, a new line or something local
    ctrl = "ctrl" in mods
    if _chat_box(f) or (ctrl and _mail_body(f)):
        return Decision("ask", "send", CHAT_REASON)
    if f.in_form and not _has(f.name, SEARCH_WORDS) and (ctrl or not f.multiline):
        return Decision("ask", "submit", ENTER_REASON)
    return GO


def _password_box(f: Facts) -> bool:
    """a password box by its IsPassword flag, or a field (not a page) by its name"""
    return f.password or (_editable(f) and _has(f.name, PASSWORD_WORDS))


def _card_field(f: Facts) -> bool:
    return _editable(f) and _has(f.name, CARD_FIELD_WORDS)


def _pressable(f: Facts) -> bool:
    """a focused control that Enter (or Space) would press"""
    return f.kind in ("Button", "SplitButton", "MenuItem", "Hyperlink") and not f.editable


def _editable(f: Facts) -> bool:
    """f takes typed text, a UIA Edit always counts no matter what the flag says"""
    return f.editable or f.kind == "Edit"


def _file_view(f: Facts) -> bool:
    """the focus is a list of things (files, mails) where Delete throws one away"""
    return not _editable(f) and (f.kind in LIST_KINDS or f.window_class in FILE_VIEW_CLASSES)


def _mail_body(f: Facts) -> bool:
    """an email body, Enter is a new line there and Ctrl+Enter sends"""
    return (_editable(f) and f.multiline
            and (f.process.lower() in MAIL_APPS or _has(f.name, BODY_WORDS)))


def _chat_box(f: Facts) -> bool:
    """a text box where Enter sends the message"""
    if not _editable(f) or _has(f.name, SEARCH_WORDS) or _mail_body(f):
        return False
    if f.process.lower() in CHAT_APPS:
        return True
    words = _words(f.name)
    return _has(f.name, CHAT_BOX_WORDS) or (words.startswith(" message ") and len(words.split()) > 1)


# words

def _words(text: str) -> str:
    """'Don't Save (Ctrl+D)' -> " don't save " for whole word matching. curly apostrophes
    go straight and bracketed hints (shortcuts, counts) get dropped"""
    text = re.sub(r"\([^)]*\)", " ", text.lower().replace("’", "'"))
    return " " + " ".join(re.findall(r"[^\W_]+(?:'[^\W_]+)*", text)) + " "


def _has(text: str, phrases: tuple[str, ...]) -> bool:
    words = _words(text)
    return any(f" {p} " in words for p in phrases)


def _first_line(name: str) -> str:
    """first line of a name as plain words ("Install Now" of a command link)"""
    return _words(name.split("\n", 1)[0]).strip()
