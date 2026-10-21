"""the driving safety rules (safety.py) against Facts written the way UI Automation reports
them for real apps (Settings, Firefox forms, Explorer, Discord, a checkout page, installers,
title bars). all hand written, nothing read off a live desktop

run from brain/:  .venv/Scripts/python -m unittest discover tests
"""

import sys
import unittest
from pathlib import Path

BRAIN_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRAIN_DIR))

from manikin import safety                              # noqa: E402
from manikin.drive_types import GO, Action, Facts      # noqa: E402
from manikin.safety import check, has_card_number      # noqa: E402

CATEGORIES = {"password", "payment", "delete", "send", "submit", "close", "install"}


def click(button="left", count=1):
    return Action("click", 640, 400, button=button, count=count)


def key(*keys):
    return Action("key", keys=keys)


def type_(text):
    return Action("type", text=text)


# the apps

SETTINGS = dict(window_title="Settings", window_class="ApplicationFrameWindow",
                process="systemsettings.exe")
NIGHT_LIGHT = Facts("Night light", "Button", **SETTINGS, ancestors=("Night light", "Display"))
DISPLAY = Facts("Display", "ListItem", **SETTINGS, ancestors=("System",))
DISPLAY_BUTTON = Facts("Display", "Button", **SETTINGS)
FIND_SETTING = Facts("Find a setting", "Edit", editable=True, **SETTINGS)
REMOVE_DEVICE = Facts("Remove device", "Button", **SETTINGS, ancestors=("WH-1000XM4",))
SETTINGS_UNINSTALL = Facts("Uninstall", "MenuItem", **SETTINGS, ancestors=("More options",))
CLOSE_SETTINGS = Facts("Close Settings", "Button", **SETTINGS, ancestors=("Settings",))
MINIMIZE_SETTINGS = Facts("Minimize Settings", "Button", **SETTINGS, ancestors=("Settings",))

FIREFOX = dict(window_title="Contact us — Mozilla Firefox", window_class="MozillaWindowClass",
               process="firefox.exe")
FORM = ("Contact form", "Contact us")
ADDRESS_BAR = Facts("Search with Google or enter address", "Edit", editable=True, **FIREFOX,
                    ancestors=("Navigation",))
FULL_NAME = Facts("Full name", "Edit", editable=True, in_form=True, **FIREFOX, ancestors=FORM)
EMAIL = Facts("Email", "Edit", editable=True, in_form=True, **FIREFOX, ancestors=FORM)
MESSAGE_AREA = Facts("Message", "Edit", editable=True, multiline=True, in_form=True, **FIREFOX,
                     ancestors=FORM)
PASSWORD = Facts("Password", "Edit", password=True, editable=True, in_form=True, **FIREFOX,
                 ancestors=FORM)
# some sites use a plain text box with a show/hide toggle, only the name gives it away
CONFIRM_PASSWORD = Facts("Confirm your password", "Edit", editable=True, in_form=True,
                         **FIREFOX, ancestors=FORM)
SHOW_PASSWORD = Facts("Show password", "Button", in_form=True, **FIREFOX, ancestors=FORM)
RESET_PAGE = Facts("Reset your password", "Document", **FIREFOX)    # nothing in it focused
UNFLAGGED_PASSWORD = Facts("New password", "Edit", in_form=True, **FIREFOX, ancestors=FORM)
FORGOT_PASSWORD = Facts("Forgot password?", "Hyperlink", **FIREFOX, ancestors=FORM)
TERMS = Facts("I agree to the terms and conditions", "CheckBox", in_form=True, **FIREFOX,
              ancestors=FORM)
SUBMIT = Facts("Submit", "Button", in_form=True, **FIREFOX, ancestors=FORM)
SIGN_UP = Facts("Sign up", "Button", in_form=True, **FIREFOX, ancestors=FORM)
CONTINUE = Facts("Continue", "Button", in_form=True, **FIREFOX, ancestors=FORM)
CONTINUE_READING = Facts("Continue reading", "Hyperlink", **FIREFOX)
GOOGLE_SEARCH = Facts("Search", "ComboBox", editable=True, in_form=True,
                      window_title="Google — Mozilla Firefox", window_class="MozillaWindowClass",
                      process="firefox.exe")
HOW_TO_DELETE = Facts("How to delete your Firefox profile without losing bookmarks",
                      "Hyperlink", **FIREFOX)
HOW_TO_DELETE_TEXT = Facts("How to delete your Firefox profile without losing bookmarks",
                           "Text", **FIREFOX,
                           ancestors=("How to delete your Firefox profile without losing bookmarks",))
DELETE_LINK = Facts("Delete", "Hyperlink", **FIREFOX, ancestors=("Row 3",))
REMOVE_TEXT_LEAF = Facts("Remove", "Text", **FIREFOX, ancestors=("Remove", "Cart item"))
CLOSE_TAB = Facts("Close Tab", "Button", **FIREFOX, ancestors=("Contact us",))
FIREFOX_CLOSE = Facts("Close", "Button", **FIREFOX, ancestors=("", "Contact us — Mozilla Firefox"))
COOKIE_CLOSE = Facts("Close", "Button", **FIREFOX, ancestors=("Cookie preferences", "Contact us"))
COOKIE_ACCEPT = Facts("Accept all cookies", "Button", **FIREFOX, ancestors=("Cookie preferences",))

SHOP = dict(window_title="Checkout — Mozilla Firefox", window_class="MozillaWindowClass",
            process="firefox.exe")
PAY_FORM = ("Payment", "Checkout")
PLACE_ORDER = Facts("Place your order", "Button", in_form=True, **SHOP, ancestors=PAY_FORM)
PAY = Facts("Pay $24.99", "Button", in_form=True, **SHOP, ancestors=PAY_FORM)
PAYPAL = Facts("PayPal", "Button", **SHOP)
BUY_NOW = Facts("Buy Now", "Button", **SHOP)
CHECKOUT_LINK = Facts("Checkout", "Hyperlink", **SHOP)
ADD_TO_CART = Facts("Add to cart", "Button", **SHOP)
REMOVE_FROM_CART = Facts("Remove from cart", "Button", **SHOP)
PURCHASE_HISTORY = Facts("Purchase history", "Hyperlink", **SHOP)
CLEAR_PURCHASE_HISTORY = Facts("Delete purchase history", "Button", **SHOP)
CARD_NUMBER = Facts("Card number", "Edit", editable=True, in_form=True, **SHOP, ancestors=PAY_FORM)
SECURITY_CODE = Facts("Security code", "Edit", editable=True, in_form=True, **SHOP,
                      ancestors=PAY_FORM)
CARD_NAME = Facts("Name on card", "Edit", editable=True, in_form=True, **SHOP, ancestors=PAY_FORM)
SUBSCRIBE = Facts("Subscribe", "Button", window_title="Lofi beats - YouTube — Mozilla Firefox",
                  window_class="MozillaWindowClass", process="firefox.exe")

GMAIL = dict(window_title="Inbox - Gmail — Mozilla Firefox", window_class="MozillaWindowClass",
             process="firefox.exe")
GMAIL_BODY = Facts("Message Body", "Edit", editable=True, multiline=True, **GMAIL)
GMAIL_SUBJECT = Facts("Subject", "Edit", editable=True, **GMAIL)
GMAIL_SEND = Facts("Send ‪(Ctrl-Enter)‬", "Button", **GMAIL)
GMAIL_TRASH_FOLDER = Facts("Trash", "Hyperlink", **GMAIL)
WEB_CHAT = Facts("Type a message", "Edit", editable=True, **FIREFOX)

EXPLORER = dict(window_title="Downloads - File Explorer", window_class="CabinetWClass",
                process="explorer.exe")
FILE_ITEM = Facts("delete me.txt", "ListItem", **EXPLORER, ancestors=("Items View",))
FILE_DATA_ITEM = Facts("Name", "DataItem", **EXPLORER, ancestors=("delete me.txt",))
NAV_TREE = Facts("Downloads", "TreeItem", **EXPLORER, ancestors=("Navigation Pane",))
RENAME_BOX = Facts("delete me.txt", "Edit", editable=True, **EXPLORER, ancestors=("Items View",))
EXPLORER_ADDRESS = Facts("Address", "Edit", editable=True, **EXPLORER)
DELETE_BUTTON = Facts("Delete", "Button", **EXPLORER, ancestors=("Command Bar",))
DELETE_MENU = Facts("Delete", "MenuItem", **EXPLORER, ancestors=("Context",))
DESKTOP_ICON = Facts("Recycle Bin", "ListItem", window_title="", window_class="Progman",
                     process="explorer.exe", ancestors=("Desktop",))
EMPTY_BIN = Facts("Empty Recycle Bin", "MenuItem", window_title="", window_class="Progman",
                  process="explorer.exe")

NOTEPAD = dict(window_title="notes.txt - Notepad", window_class="Notepad", process="notepad.exe")
NOTEPAD_TEXT = Facts("Text editor", "Document", editable=True, multiline=True, **NOTEPAD)
NOTEPAD_CLOSE = Facts("Close", "Button", **NOTEPAD, ancestors=("notes.txt - Notepad",))
NOTEPAD_MINIMIZE = Facts("Minimize", "Button", **NOTEPAD, ancestors=("notes.txt - Notepad",))
NOTEPAD_MAXIMIZE = Facts("Maximize", "Button", **NOTEPAD, ancestors=("notes.txt - Notepad",))
NOTEPAD_EXIT = Facts("Exit", "MenuItem", **NOTEPAD, ancestors=("File",))
DONT_SAVE = Facts("Don't save", "Button", **NOTEPAD, ancestors=("Do you want to save changes?",))
SAVE = Facts("Save", "Button", **NOTEPAD, ancestors=("Do you want to save changes?",))
CANCEL = Facts("Cancel", "Button", **NOTEPAD, ancestors=("Do you want to save changes?",))
VSCODE_DISCARD = Facts("Discard Changes", "Button", window_title="app.py - Visual Studio Code",
                       window_class="Chrome_WidgetWin_1", process="code.exe")
VSCODE_TAB_CLOSE = Facts("Close (Ctrl+F4)", "Button", window_title="app.py - Visual Studio Code",
                         window_class="Chrome_WidgetWin_1", process="code.exe")

DISCORD = dict(window_title="#general | Manikin fans - Discord", window_class="Chrome_WidgetWin_1",
               process="discord.exe")
DISCORD_BOX = Facts("Message #general", "Edit", editable=True, multiline=True, **DISCORD)
DISCORD_SEARCH = Facts("Search", "Edit", editable=True, **DISCORD)
DISCORD_DELETE = Facts("Delete Message", "MenuItem", **DISCORD)
DISCORD_REPLY = Facts("Reply", "MenuItem", **DISCORD)

NSIS = dict(window_title="VLC media player 3.0.21 Setup", window_class="#32770",
            process="vlc-3.0.21-win64.exe")
MSI = dict(window_title="Node.js Setup", window_class="MsiDialogCloseClass", process="msiexec.exe")
INNO = dict(window_title="Setup - Notepad++", window_class="TWizardForm", process="is-q3v1k.tmp")
UNINSTALLER = dict(window_title="Notepad++ Uninstall", window_class="#32770",
                   process="unins000.exe")
PYTHON_INSTALL = Facts("Install Now\nC:\\Users\\Mani\\AppData\\Local\\Programs\\Python\\Python313"
                       "\n\nIncludes IDLE, pip and documentation", "Button",
                       window_title="Python 3.13.0 (64-bit) Setup", window_class="WixStdBA",
                       process="python-3.13.0-amd64.exe")
CHROME_INSTALL = Facts("Install", "Button", window_title="Google Chrome", window_class="#32770",
                       process="chromesetup.exe")
STORE_INSTALL = Facts("Install", "Button", window_title="Microsoft Store",
                      window_class="ApplicationFrameWindow", process="winstore.app.exe")

PASSWORD_HANDOVER = ("handover", "password")
PAYMENT_HANDOVER = ("handover", "payment")
DELETE_HANDOVER = ("handover", "delete")
ASK_SEND = ("ask", "send")
ASK_SUBMIT = ("ask", "submit")
ASK_CLOSE = ("ask", "close")
ASK_INSTALL = ("ask", "install")
GO_ = ("go", "")

# (what, action, target, focused, (verdict, category))
CASES = [
    # settings, toggles and pages just go ("Display" isnt "pay")
    ("settings toggle", click(), NIGHT_LIGHT, None, GO_),
    ("settings nav item", click(), DISPLAY, None, GO_),
    ("display button is not pay", click(), DISPLAY_BUTTON, None, GO_),
    ("settings search enter", key("enter"), None, FIND_SETTING, GO_),
    ("settings search typed newline", type_("night light\n"), None, FIND_SETTING, GO_),
    ("remove bluetooth device", click(), REMOVE_DEVICE, None, DELETE_HANDOVER),
    # settings own Uninstall isnt in an installer window so it goes, the uninstaller it
    # opens asks at its own last button
    ("settings uninstall", click(), SETTINGS_UNINSTALL, None, GO_),
    ("settings close", click(), CLOSE_SETTINGS, None, ASK_CLOSE),
    ("settings minimize", click(), MINIMIZE_SETTINGS, None, GO_),

    # firefox web form
    ("click into a field", click(), FULL_NAME, FULL_NAME, GO_),
    ("type into a field", type_("Mani"), None, FULL_NAME, GO_),
    ("type an email", type_("mani@example.com"), None, EMAIL, GO_),
    ("tab between fields", key("tab"), None, FULL_NAME, GO_),
    ("enter in a single-line form field", key("enter"), None, FULL_NAME, ASK_SUBMIT),
    ("ctrl+enter in a form field", key("ctrl", "enter"), None, EMAIL, ASK_SUBMIT),
    ("typed newline in a form field", type_("Mani\n"), None, FULL_NAME, ASK_SUBMIT),
    ("typed newline mid-text", type_("Mani\nGh"), None, FULL_NAME, ASK_SUBMIT),
    ("enter in a textarea is a new line", key("enter"), None, MESSAGE_AREA, GO_),
    ("typed paragraphs in a textarea", type_("Hi,\n\nThanks!"), None, MESSAGE_AREA, GO_),
    ("shift+enter in a form field", key("shift", "enter"), None, FULL_NAME, GO_),
    ("ctrl+enter in a textarea submits", key("ctrl", "enter"), None, MESSAGE_AREA, ASK_SUBMIT),
    ("terms checkbox", click(), TERMS, None, GO_),
    ("space on the terms checkbox", key("space"), None, TERMS, GO_),
    ("submit button", click(), SUBMIT, None, ASK_SUBMIT),
    ("double-click submit", click(count=2), SUBMIT, None, ASK_SUBMIT),
    ("sign up button", click(), SIGN_UP, None, ASK_SUBMIT),
    ("continue in a form", click(), CONTINUE, None, ASK_SUBMIT),
    ("continue reading link", click(), CONTINUE_READING, None, GO_),
    ("enter on a focused submit button", key("enter"), None, SUBMIT, ASK_SUBMIT),
    ("space on a focused submit button", key("space"), None, SUBMIT, ASK_SUBMIT),
    ("typed space on a focused submit button", type_(" "), None, SUBMIT, ASK_SUBMIT),
    ("typed newline on a focused submit button", type_("\n"), None, SUBMIT, ASK_SUBMIT),
    ("address bar enter", key("enter"), None, ADDRESS_BAR, GO_),
    ("address bar typed url", type_("example.com"), None, ADDRESS_BAR, GO_),
    ("address bar typed url and newline", type_("example.com\n"), None, ADDRESS_BAR, GO_),
    ("google search enter", key("enter"), None, GOOGLE_SEARCH, GO_),
    ("delete inside long link text", click(), HOW_TO_DELETE, None, GO_),
    ("delete inside long link text leaf", click(), HOW_TO_DELETE_TEXT, None, GO_),
    ("a bare delete link", click(), DELETE_LINK, None, DELETE_HANDOVER),
    ("enter on a focused delete link", key("enter"), None, DELETE_LINK, DELETE_HANDOVER),
    ("text leaf of a remove link", click(), REMOVE_TEXT_LEAF, None, DELETE_HANDOVER),
    ("close tab button", click(), CLOSE_TAB, None, ASK_CLOSE),
    ("firefox title bar close", click(), FIREFOX_CLOSE, None, ASK_CLOSE),
    # a web dialogs X named "Close" asks too, UIA cant tell it apart from the title bar one
    ("web dialog close", click(), COOKIE_CLOSE, None, ASK_CLOSE),
    ("cookie accept", click(), COOKIE_ACCEPT, None, GO_),

    # passwords, typing or keys into the box (just clicking it is fine)
    ("click into the password box", click(), PASSWORD, None, GO_),
    ("type a password", type_("hunter2"), None, PASSWORD, PASSWORD_HANDOVER),
    ("paste into the password box", key("ctrl", "v"), None, PASSWORD, PASSWORD_HANDOVER),
    ("a letter key in the password box", key("a"), None, PASSWORD, PASSWORD_HANDOVER),
    ("enter in the password box", key("enter"), None, PASSWORD, PASSWORD_HANDOVER),
    ("backspace in the password box", key("backspace"), None, PASSWORD, PASSWORD_HANDOVER),
    ("tab past the password box", key("tab"), None, PASSWORD, GO_),
    ("shift+tab past the password box", key("shift", "tab"), None, PASSWORD, GO_),
    ("escape in the password box", key("escape"), None, PASSWORD, GO_),
    ("alt+f4 from the password box", key("alt", "f4"), None, PASSWORD, ASK_CLOSE),
    ("password box by name only", type_("hunter2"), None, CONFIRM_PASSWORD, PASSWORD_HANDOVER),
    ("show password button", click(), SHOW_PASSWORD, None, GO_),
    ("forgot password link", click(), FORGOT_PASSWORD, None, GO_),
    ("a page about passwords isn't a password box", key("ctrl", "l"), None, RESET_PAGE, GO_),
    ("an Edit counts as a field even unflagged", type_("hunter2"), None, UNFLAGGED_PASSWORD,
     PASSWORD_HANDOVER),

    # payments
    ("place your order", click(), PLACE_ORDER, None, PAYMENT_HANDOVER),
    ("pay with an amount", click(), PAY, None, PAYMENT_HANDOVER),
    ("paypal button", click(), PAYPAL, None, PAYMENT_HANDOVER),
    ("buy now", click(), BUY_NOW, None, PAYMENT_HANDOVER),
    ("checkout link", click(), CHECKOUT_LINK, None, PAYMENT_HANDOVER),
    ("enter on a focused pay button", key("enter"), None, PAY, PAYMENT_HANDOVER),
    ("add to cart", click(), ADD_TO_CART, None, GO_),
    ("purchase history link", click(), PURCHASE_HISTORY, None, GO_),
    ("delete purchase history", click(), CLEAR_PURCHASE_HISTORY, None, DELETE_HANDOVER),
    ("remove from cart", click(), REMOVE_FROM_CART, None, DELETE_HANDOVER),
    ("youtube subscribe", click(), SUBSCRIBE, None, GO_),
    ("card number in a card field", type_("4242 4242 4242 4242"), None, CARD_NUMBER,
     PAYMENT_HANDOVER),
    ("card number in any field", type_("4242424242424242"), None, FULL_NAME, PAYMENT_HANDOVER),
    ("card number with expiry", type_("4242-4242-4242-4242 12/28"), None, MESSAGE_AREA,
     PAYMENT_HANDOVER),
    ("security code", type_("123"), None, SECURITY_CODE, PAYMENT_HANDOVER),
    ("paste into the card field", key("ctrl", "v"), None, CARD_NUMBER, PAYMENT_HANDOVER),
    ("name on card", type_("Mani Gh"), None, CARD_NAME, GO_),
    ("not a card (fails luhn)", type_("4242 4242 4242 4241"), None, FULL_NAME, GO_),
    ("phone numbers", type_("555-123-4567 555-987-6543"), None, MESSAGE_AREA, GO_),

    # explorer
    ("click a file named delete", click(), FILE_ITEM, None, GO_),
    ("double-click a file", click(count=2), FILE_ITEM, None, GO_),
    ("delete key on a file", key("delete"), None, FILE_ITEM, DELETE_HANDOVER),
    ("shift+delete on a file", key("shift", "delete"), None, FILE_ITEM, DELETE_HANDOVER),
    ("delete key in details view", key("delete"), None, FILE_DATA_ITEM, DELETE_HANDOVER),
    ("delete key in the folder tree", key("delete"), None, NAV_TREE, DELETE_HANDOVER),
    ("ctrl+d in explorer", key("ctrl", "d"), None, FILE_ITEM, DELETE_HANDOVER),
    ("delete key on the desktop", key("delete"), None, DESKTOP_ICON, DELETE_HANDOVER),
    ("delete key while renaming", key("delete"), None, RENAME_BOX, GO_),
    ("ctrl+d while renaming", key("ctrl", "d"), None, RENAME_BOX, GO_),
    ("delete key in the address box", key("delete"), None, EXPLORER_ADDRESS, GO_),
    ("enter opens a file", key("enter"), None, FILE_ITEM, GO_),
    ("copy a file", key("ctrl", "c"), None, FILE_ITEM, GO_),
    ("backspace goes back", key("backspace"), None, FILE_ITEM, GO_),
    ("delete button", click(), DELETE_BUTTON, None, DELETE_HANDOVER),
    ("delete in the context menu", click(), DELETE_MENU, None, DELETE_HANDOVER),
    ("empty recycle bin", click(), EMPTY_BIN, None, DELETE_HANDOVER),
    ("right-click delete only opens a menu", click(button="right"), DELETE_BUTTON, None, GO_),
    ("hover over delete", Action("move", 640, 400), DELETE_BUTTON, None, GO_),
    ("scroll over delete", Action("scroll", 640, 400, dy=3), DELETE_BUTTON, None, GO_),
    ("drag a file", Action("drag", 640, 400, to_x=900, to_y=500), FILE_ITEM, None, GO_),

    # notepad, save prompts, title bars, close keys
    ("type into notepad", type_("line one\nline two\n"), None, NOTEPAD_TEXT, GO_),
    ("enter in notepad", key("enter"), None, NOTEPAD_TEXT, GO_),
    ("delete key in notepad", key("delete"), None, NOTEPAD_TEXT, GO_),
    ("save in notepad", key("ctrl", "s"), None, NOTEPAD_TEXT, GO_),
    ("notepad title bar close", click(), NOTEPAD_CLOSE, None, ASK_CLOSE),
    ("notepad minimize", click(), NOTEPAD_MINIMIZE, None, GO_),
    ("notepad maximize", click(), NOTEPAD_MAXIMIZE, None, GO_),
    ("file exit", click(), NOTEPAD_EXIT, None, ASK_CLOSE),
    ("don't save", click(), DONT_SAVE, None, ASK_CLOSE),
    ("save in the save prompt", click(), SAVE, None, GO_),
    ("cancel in the save prompt", click(), CANCEL, None, GO_),
    ("discard changes", click(), VSCODE_DISCARD, None, ASK_CLOSE),
    ("editor tab close with shortcut hint", click(), VSCODE_TAB_CLOSE, None, ASK_CLOSE),
    ("alt+f4", key("alt", "f4"), None, NOTEPAD_TEXT, ASK_CLOSE),
    ("ctrl+w", key("ctrl", "w"), None, ADDRESS_BAR, ASK_CLOSE),
    ("ctrl+shift+w", key("ctrl", "shift", "w"), None, ADDRESS_BAR, ASK_CLOSE),
    ("ctrl+q", key("ctrl", "q"), None, NOTEPAD_TEXT, ASK_CLOSE),
    ("ctrl+f4", key("ctrl", "f4"), None, NOTEPAD_TEXT, ASK_CLOSE),
    ("alt+tab", key("alt", "tab"), None, NOTEPAD_TEXT, GO_),
    ("plain w", key("w"), None, NOTEPAD_TEXT, GO_),

    # chat and mail
    ("discord enter sends", key("enter"), None, DISCORD_BOX, ASK_SEND),
    ("discord ctrl+enter sends", key("ctrl", "enter"), None, DISCORD_BOX, ASK_SEND),
    ("discord shift+enter is a new line", key("shift", "enter"), None, DISCORD_BOX, GO_),
    ("discord typing", type_("hi everyone"), None, DISCORD_BOX, GO_),
    ("discord typed newline sends", type_("hi everyone\n"), None, DISCORD_BOX, ASK_SEND),
    ("discord search enter", key("enter"), None, DISCORD_SEARCH, GO_),
    ("discord delete message", click(), DISCORD_DELETE, None, DELETE_HANDOVER),
    ("discord reply", click(), DISCORD_REPLY, None, ASK_SEND),
    ("web chat enter", key("enter"), None, WEB_CHAT, ASK_SEND),
    ("gmail body enter is a new line", key("enter"), None, GMAIL_BODY, GO_),
    ("gmail body paragraphs", type_("Hi Bob,\n\nSee you Friday."), None, GMAIL_BODY, GO_),
    ("gmail body ctrl+enter sends", key("ctrl", "enter"), None, GMAIL_BODY, ASK_SEND),
    ("gmail subject enter", key("enter"), None, GMAIL_SUBJECT, GO_),
    ("gmail send button", click(), GMAIL_SEND, None, ASK_SEND),
    # gmails Trash folder link is a bare "Trash" so it looks like a trash button
    ("gmail trash folder", click(), GMAIL_TRASH_FOLDER, None, DELETE_HANDOVER),

    # installers, one ask at the finalizing button
    ("nsis next", click(), Facts("Next >", "Button", **NSIS), None, GO_),
    ("nsis back", click(), Facts("< Back", "Button", **NSIS), None, GO_),
    ("nsis license agree", click(), Facts("I Agree", "Button", **NSIS), None, GO_),
    ("nsis cancel", click(), Facts("Cancel", "Button", **NSIS), None, GO_),
    ("nsis browse", click(), Facts("Browse...", "Button", **NSIS), None, GO_),
    ("nsis component checkbox", click(), Facts("Delete preferences and cache", "CheckBox", **NSIS),
     None, GO_),
    ("nsis install", click(), Facts("Install", "Button", **NSIS), None, ASK_INSTALL),
    ("nsis finish", click(), Facts("Finish", "Button", **NSIS), None, ASK_INSTALL),
    ("enter on a focused install button", key("enter"), None, Facts("Install", "Button", **NSIS),
     ASK_INSTALL),
    ("msi next", click(), Facts("Next", "Button", **MSI), None, GO_),
    ("msi install", click(), Facts("Install", "Button", **MSI), None, ASK_INSTALL),
    ("msi remove asks rather than hands over", click(), Facts("Remove", "Button", **MSI), None,
     ASK_INSTALL),
    ("inno next", click(), Facts("Next", "Button", **INNO), None, GO_),
    ("inno install", click(), Facts("Install", "Button", **INNO), None, ASK_INSTALL),
    ("uninstaller uninstall", click(), Facts("Uninstall", "Button", **UNINSTALLER), None,
     ASK_INSTALL),
    ("python install now", click(), PYTHON_INSTALL, None, ASK_INSTALL),
    ("python customize", click(), Facts("Customize installation", "Button",
                                        window_title="Python 3.13.0 (64-bit) Setup",
                                        window_class="WixStdBA",
                                        process="python-3.13.0-amd64.exe"), None, GO_),
    ("chrome setup by process", click(), CHROME_INSTALL, None, ASK_INSTALL),
    ("finish in a bare classic wizard", click(), Facts("Finish", "Button", window_title="Add a device",
                                                       window_class="#32770",
                                                       process="devicepairingwizard.exe"),
     None, ASK_INSTALL),
    # a classic dialog that isnt an installer, its "Remove" still deletes
    ("remove in credential manager", click(), Facts("Remove", "Button",
                                                    window_title="Credential Manager",
                                                    window_class="#32770",
                                                    process="rundll32.exe"), None,
     DELETE_HANDOVER),
    # not an installer window so store installs ride on the plan approval
    ("store install", click(), STORE_INSTALL, None, GO_),
    ("finish outside an installer", click(), Facts("Finish", "Button", **NOTEPAD), None, GO_),

    # nothing known (games, apps that draw their own UI), only the text and key rules
    ("click on nothing known", click(), None, None, GO_),
    ("type with nothing known", type_("hello"), None, None, GO_),
    ("card number with nothing known", type_("4111 1111 1111 1111"), None, None,
     PAYMENT_HANDOVER),
    ("typed newline with nothing known", type_("hello\n"), None, None, ASK_SUBMIT),
    ("enter with nothing known", key("enter"), None, None, GO_),
    ("delete with nothing known", key("delete"), None, None, GO_),
    ("shift+delete with nothing known", key("shift", "delete"), None, None, DELETE_HANDOVER),
    ("alt+f4 with nothing known", key("alt", "f4"), None, None, ASK_CLOSE),
    ("ctrl+w with nothing known", key("ctrl", "w"), None, None, ASK_CLOSE),
    ("arrow keys with nothing known", key("left"), None, None, GO_),
    ("type ignores a stray target", type_("hello"), DELETE_BUTTON, FULL_NAME, GO_),
]


class RuleTableTests(unittest.TestCase):
    def test_cases(self):
        for what, action, target, focused, (verdict, category) in CASES:
            with self.subTest(what):
                got = check(action, target, focused)
                self.assertEqual((got.verdict, got.category), (verdict, category), got.reason)

    def test_every_decision_is_well_formed(self):
        for what, action, target, focused, _ in CASES:
            with self.subTest(what):
                got = check(action, target, focused)
                if got.verdict == "go":
                    self.assertEqual(got, GO)
                    continue
                self.assertIn(got.category, CATEGORIES)
                self.assertTrue(got.reason)
                if got.verdict == "ask":            # a yes/no question for confirm()
                    self.assertTrue(got.reason.endswith("?"), got.reason)

    def test_deterministic(self):
        for what, action, target, focused, _ in CASES:
            self.assertEqual(check(action, target, focused), check(action, target, focused))


class KeywordListTests(unittest.TestCase):
    """every entry in every list does what its list says, as a plain web button"""

    def button(self, name, **kw):
        return Facts(name.title(), "Button", **FIREFOX, **kw)

    def judge(self, facts):
        got = check(click(), facts, None)
        return got.verdict, got.category

    def test_lists(self):
        lists = [(safety.PAYMENT_WORDS, PAYMENT_HANDOVER), (safety.DELETE_WORDS, DELETE_HANDOVER),
                 (safety.SEND_WORDS, ASK_SEND), (safety.SUBMIT_WORDS, ASK_SUBMIT),
                 (safety.CLOSE_NAMES, ASK_CLOSE), (safety.DISCARD_WORDS, ASK_CLOSE)]
        for words, expected in lists:
            for word in words:
                with self.subTest(word):
                    self.assertEqual(self.judge(self.button(word)), expected)

    def test_form_only_words(self):
        for word in safety.FORM_SUBMIT_WORDS:
            with self.subTest(word):
                self.assertEqual(self.judge(self.button(word, in_form=True)), ASK_SUBMIT)
                self.assertEqual(self.judge(self.button(word)), GO_)

    def test_install_names_only_in_installers(self):
        for word in safety.INSTALL_FINAL_NAMES:
            with self.subTest(word):
                self.assertEqual(self.judge(Facts(word.title(), "Button", **MSI)), ASK_INSTALL)
                self.assertEqual(self.judge(Facts(word.title(), "Button", **NSIS)), ASK_INSTALL)
                self.assertNotEqual(self.judge(self.button(word))[0], "ask")

    def test_password_and_card_fields(self):
        for words, expected in ((safety.PASSWORD_WORDS, PASSWORD_HANDOVER),
                                (safety.CARD_FIELD_WORDS, PAYMENT_HANDOVER)):
            for word in words:
                with self.subTest(word):
                    field = Facts(word.title(), "Edit", editable=True, in_form=True, **FIREFOX)
                    got = check(type_("abc"), None, field)
                    self.assertEqual((got.verdict, got.category), expected)

    def test_chat_apps(self):
        for process in safety.CHAT_APPS:
            with self.subTest(process):
                box = Facts("Message", "Edit", editable=True, multiline=True, process=process)
                self.assertEqual(check(key("enter"), None, box).verdict, "ask")

    def test_whole_words_only(self):
        for name in ("Display", "Repay later", "Payload", "Removable storage", "Sender details",
                     "Posted by", "Shared with me", "Deleted Items", "Closed captions",
                     "Disclose", "Commentary", "Republish date", "Submitted", "Subscribe",
                     "Purchased", "Erased"):
            with self.subTest(name):
                self.assertEqual(self.judge(self.button(name)), GO_)

    def test_only_controls_that_act(self):
        for kind in ("ListItem", "DataItem", "TreeItem", "Edit", "Document", "CheckBox",
                     "RadioButton", "TabItem", "Pane", "Group", "Window", "ComboBox"):
            with self.subTest(kind):
                self.assertEqual(self.judge(Facts("Delete", kind, **FIREFOX)), GO_)
        for kind in ("Button", "SplitButton", "MenuItem", "Hyperlink", "Text", "Image", ""):
            with self.subTest(kind):
                self.assertEqual(self.judge(Facts("Delete", kind, **FIREFOX)), DELETE_HANDOVER)

    def test_link_names_must_lead_and_be_short(self):
        for name, expected in (("Remove item", DELETE_HANDOVER),
                               ("Delete this comment now please", DELETE_HANDOVER),
                               ("Delete this old comment now please", GO_),
                               ("Learn how to remove malware", GO_),
                               ("Where to buy", GO_),
                               ("Buy it now", PAYMENT_HANDOVER)):
            with self.subTest(name):
                self.assertEqual(self.judge(Facts(name, "Hyperlink", **FIREFOX)), expected)


class ReasonTests(unittest.TestCase):
    def test_his_lines(self):
        self.assertEqual(check(type_("x"), None, PASSWORD).reason,
                         "That's a password box. Your turn!")
        self.assertEqual(check(click(), PAY, None).reason,
                         "That's a payment button. You do that one.")
        self.assertEqual(check(click(), SUBMIT, None).reason, "Press Submit?")
        self.assertEqual(check(click(), SIGN_UP, None).reason, "Press Sign up?")
        self.assertEqual(check(click(), Facts("Install", "Button", **MSI), None).reason,
                         "Press Install?")
        self.assertEqual(check(click(), PYTHON_INSTALL, None).reason, "Press Install Now?")
        self.assertEqual(check(key("enter"), None, DISCORD_BOX).reason, "Send that message?")
        self.assertEqual(check(click(), DONT_SAVE, None).reason, "Throw away the changes?")

    def test_long_names_are_cut(self):
        long = Facts("Submit this application form for review by the committee", "Button")
        reason = check(click(), long, None).reason
        self.assertTrue(reason.startswith("Press Submit this"), reason)
        self.assertTrue(reason.endswith("...?"), reason)
        self.assertLessEqual(len(reason), 40)

    def test_nameless_button_has_a_fallback(self):
        self.assertEqual(safety._press_reason(Facts("", "Button"), "Submit it?"), "Submit it?")


class CardNumberTests(unittest.TestCase):
    def test_cards(self):
        for text in ("4242424242424242", "4242 4242 4242 4242", "4242-4242-4242-4242",
                     "4111 1111 1111 1111", "3782 822463 10005",       # Amex, 15
                     "4222222222222",                                  # 13
                     "6011000000000000001",                            # 19
                     "card: 4242424242424242.", "4242 4242 4242 4242 12/28 123",
                     "my card is 4111-1111-1111-1111 thanks"):
            with self.subTest(text):
                self.assertTrue(has_card_number(text))

    def test_not_cards(self):
        for text in ("4242 4242 4242 4241",             # fails Luhn
                     "424242424242",                    # 12 digits, passes Luhn
                     "42424242424242424242",            # 20 digits
                     "555-123-4567 555-987-6543",       # two phone numbers
                     "4242  4242  4242  4242",          # double spaces split it up
                     "Order 12345, call 555-0100", "", "no digits here"):
            with self.subTest(text):
                self.assertFalse(has_card_number(text))


if __name__ == "__main__":
    unittest.main()
