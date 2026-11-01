// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Input.h>
#include <AfterglowVt/Terminal.h>

#include "Utf8.h"

#include <cstdio>

namespace AfterglowVt {

namespace {

int modParam(std::uint8_t mods)
{
    int m = 1;
    if (mods & Mod::Shift) {
        m += 1;
    }
    if (mods & Mod::Alt) {
        m += 2;
    }
    if (mods & Mod::Ctrl) {
        m += 4;
    }
    if (mods & Mod::Super) {
        m += 8;
    }
    return m;
}

std::string cursorKey(char final, std::uint8_t mods, const Modes& modes)
{
    if (modes.vt52) {
        return std::string("\x1b") + final;
    }
    if (mods == 0) {
        return std::string(modes.cursorKeysApp ? "\x1bO" : "\x1b[") + final;
    }
    return "\x1b[1;" + std::to_string(modParam(mods)) + final;
}

std::string tildeKey(int number, std::uint8_t mods)
{
    if (mods == 0) {
        return "\x1b[" + std::to_string(number) + "~";
    }
    return "\x1b[" + std::to_string(number) + ";" + std::to_string(modParam(mods)) + "~";
}

std::string ss3Key(char final, std::uint8_t mods, const Modes& modes)
{
    if (modes.vt52) {
        // VT52: ESC alone for PF1-PF4, ESC ? for the application keypad.
        return std::string(final >= 'P' && final <= 'S' ? "\x1b" : "\x1b?") + final;
    }
    if (mods == 0) {
        return std::string("\x1bO") + final;
    }
    return "\x1b[1;" + std::to_string(modParam(mods)) + final;
}

/// xterm's modifyOtherKeys form: CSI 27 ; modifiers ; code ~.
std::string otherKey(char32_t code, std::uint8_t mods)
{
    return "\x1b[27;" + std::to_string(modParam(mods)) + ";" + std::to_string(std::uint32_t(code)) + "~";
}

std::string withAlt(std::string s, std::uint8_t mods)
{
    if ((mods & Mod::Alt) && !s.empty()) {
        s.insert(s.begin(), '\x1b');
    }
    return s;
}

/// Legacy Ctrl+key mapping to C0, or -1 when the key has none.
int ctrlCode(char32_t c)
{
    if (c >= 'a' && c <= 'z') {
        return int(c - 'a' + 1);
    }
    if (c >= 'A' && c <= 'Z') {
        return int(c - 'A' + 1);
    }
    switch (c) {
    case ' ':
    case '@':
    case '2':
        return 0x00;
    case '[':
    case '3':
        return 0x1B;
    case '\\':
    case '4':
        return 0x1C;
    case ']':
    case '5':
        return 0x1D;
    case '^':
    case '6':
    case '~':
        return 0x1E;
    case '_':
    case '7':
    case '/':
        return 0x1F;
    case '8':
    case '?':
        return 0x7F;
    default:
        return -1;
    }
}

char32_t firstCodepoint(std::string_view utf8)
{
    if (utf8.empty()) {
        return 0;
    }
    const auto b0 = static_cast<unsigned char>(utf8[0]);
    if (b0 < 0x80) {
        return b0;
    }
    const int len = b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : 2;
    if (int(utf8.size()) < len) {
        return 0;
    }
    char32_t cp = b0 & (0x7F >> len);
    for (int i = 1; i < len; ++i) {
        cp = (cp << 6) | (static_cast<unsigned char>(utf8[std::size_t(i)]) & 0x3F);
    }
    return cp;
}

/// The main key a keypad navigation key stands for in legacy encodings.
Key withoutKeypad(Key k)
{
    switch (k) {
    case Key::KeypadLeft:
        return Key::Left;
    case Key::KeypadRight:
        return Key::Right;
    case Key::KeypadUp:
        return Key::Up;
    case Key::KeypadDown:
        return Key::Down;
    case Key::KeypadPageUp:
        return Key::PageUp;
    case Key::KeypadPageDown:
        return Key::PageDown;
    case Key::KeypadHome:
        return Key::Home;
    case Key::KeypadEnd:
        return Key::End;
    case Key::KeypadInsert:
        return Key::Insert;
    case Key::KeypadDelete:
        return Key::Delete;
    default:
        return k;
    }
}

} // namespace

namespace {

std::string encodeLegacyKey(const KeyEvent& e, const Modes& modes)
{
    if (e.key >= Key::F13 && e.key <= Key::F35) {
        // As xterm's terminfo (kf13 to kf35): F13 to F24 are Shift+F1 to F12,
        // F25 to F35 Ctrl+F1 to F11. Other modifiers add to that one.
        const int n = int(e.key) - int(Key::F13);
        KeyEvent base = e;
        base.key = Key(int(Key::F1) + n % 12);
        base.mods = std::uint8_t(e.mods | (n < 12 ? Mod::Shift : Mod::Ctrl));
        return encodeLegacyKey(base, modes);
    }
    const std::uint8_t m = e.mods & ~Mod::Locks;
    // modifyOtherKeys for Return, Tab, Backspace and Escape follows xterm's
    // ModifyOtherKeys (input.c): level 1 leaves Alt to the ESC prefix and the
    // modifiers these keys already have a legacy form for.
    const int other = modes.modifyOtherKeys;
    const std::uint8_t level1 = m & ~Mod::Alt;
    switch (withoutKeypad(e.key)) {
    case Key::Up:
        return cursorKey('A', m, modes);
    case Key::Down:
        return cursorKey('B', m, modes);
    case Key::Right:
        return cursorKey('C', m, modes);
    case Key::Left:
        return cursorKey('D', m, modes);
    case Key::Home:
        return cursorKey('H', m, modes);
    case Key::End:
        return cursorKey('F', m, modes);
    case Key::Insert:
        return tildeKey(2, m);
    case Key::Delete:
        return tildeKey(3, m);
    case Key::PageUp:
        return tildeKey(5, m);
    case Key::PageDown:
        return tildeKey(6, m);
    case Key::F1:
        return ss3Key('P', m, modes);
    case Key::F2:
        return ss3Key('Q', m, modes);
    case Key::F3:
        return ss3Key('R', m, modes);
    case Key::F4:
        return ss3Key('S', m, modes);
    case Key::F5:
        return tildeKey(15, m);
    case Key::F6:
        return tildeKey(17, m);
    case Key::F7:
        return tildeKey(18, m);
    case Key::F8:
        return tildeKey(19, m);
    case Key::F9:
        return tildeKey(20, m);
    case Key::F10:
        return tildeKey(21, m);
    case Key::F11:
        return tildeKey(23, m);
    case Key::F12:
        return tildeKey(24, m);
    case Key::Enter:
        if ((other == 1 && level1 != 0) || (other == 2 && m != 0)) {
            return otherKey(13, m);
        }
        return withAlt(modes.newline ? "\r\n" : "\r", m);
    case Key::Tab:
        // Shift+Tab has its own legacy form, CSI Z.
        if ((other == 1 && (level1 & ~Mod::Shift) != 0) || (other == 2 && (m & ~Mod::Shift) != 0)) {
            return otherKey(9, m);
        }
        if (m & Mod::Shift) {
            return withAlt("\x1b[Z", m);
        }
        return withAlt("\t", m);
    case Key::Backspace: {
        // DECBKM swaps which of BS and DEL the key and Ctrl+key send.
        const bool bs = ((m & Mod::Ctrl) != 0) != modes.backarrowSendsBs;
        if (other == 2 && (m & ~Mod::Ctrl) != 0) {
            return otherKey(bs ? 8 : 127, m);
        }
        return withAlt(bs ? "\x08" : "\x7f", m);
    }
    case Key::Escape:
        if ((other == 1 && level1 != 0 && level1 != Mod::Ctrl && level1 != Mod::Shift) || (other == 2 && m != 0)) {
            return otherKey(27, m);
        }
        return withAlt("\x1b", m);
    case Key::Menu:
        return tildeKey(29, m);
    case Key::KeypadBegin:
        return cursorKey('E', m, modes);
    case Key::KeypadEnter:
        if (modes.keypadApp) {
            return ss3Key('M', m, modes);
        }
        return withAlt(modes.newline ? "\r\n" : "\r", m);
    case Key::Text:
        break;
    default: {
        // Remaining keypad keys.
        if (modes.keypadApp) {
            static constexpr char Finals[] = "pqrstuvwxynojmkMX";
            const int idx = int(e.key) - int(Key::Keypad0);
            if (idx >= 0 && idx < int(sizeof Finals) - 1) {
                return ss3Key(Finals[idx], m, modes);
            }
            if (e.key == Key::KeypadSeparator) {
                return ss3Key('l', m, modes); // xterm's KP_Separator
            }
        }
        return withAlt(std::string(e.text), m);
    }
    }

    // Plain text keys. With modifyOtherKeys, modified keys that have no
    // legacy encoding (level 1), or any modified key (level 2), are sent as
    // CSI 27 ; mods ; code ~ (xterm's format, as foot does it).
    if (modes.modifyOtherKeys != 0 && (m & (Mod::Shift | Mod::Alt | Mod::Ctrl | Mod::Super)) != 0) {
        char32_t ch = (m & Mod::Shift) && e.shifted ? e.shifted : e.unshifted;
        if (ch == 0) {
            ch = firstCodepoint(e.text);
        }
        const bool ctrl = (m & Mod::Ctrl) != 0;
        const char32_t base = e.unshifted ? e.unshifted : ch;
        const bool legacyCtrl = ctrl && ctrlCode(base) >= 0;
        bool level2 = false;
        if (modes.modifyOtherKeys == 2) {
            // Anything beyond Shift. Shift alone already shows in the
            // character it types, except on space (xterm).
            level2 = (m & ~Mod::Shift) != 0 || base == U' ';
        }
        if (ch != 0 && (level2 || (ctrl && !legacyCtrl))) {
            return otherKey(ch, m);
        }
    }
    if (m & Mod::Ctrl) {
        char32_t base = e.unshifted;
        if (base == 0 && e.text.size() == 1) {
            base = char32_t(static_cast<unsigned char>(e.text[0]));
        }
        const int code = ctrlCode(base);
        if (code >= 0) {
            std::string s(1, char(code));
            return withAlt(std::move(s), m);
        }
    }
    if (!e.text.empty()) {
        return withAlt(std::string(e.text), m);
    }
    if ((m & Mod::Alt) && e.unshifted != 0) {
        std::string s;
        detail::appendUtf8(s, e.unshifted);
        return withAlt(std::move(s), m);
    }
    return {};
}

// --- kitty keyboard protocol -------------------------------------------------

/// How a key is written in the kitty protocol: CSI number ; mods <final>.
struct KittyKey
{
    std::uint32_t number;
    char final;
};

bool kittyFunctional(Key k, KittyKey& out)
{
    switch (k) {
    case Key::Enter:
        out = {13, 'u'};
        return true;
    case Key::Tab:
        out = {9, 'u'};
        return true;
    case Key::Backspace:
        out = {127, 'u'};
        return true;
    case Key::Escape:
        out = {27, 'u'};
        return true;
    case Key::Up:
        out = {1, 'A'};
        return true;
    case Key::Down:
        out = {1, 'B'};
        return true;
    case Key::Right:
        out = {1, 'C'};
        return true;
    case Key::Left:
        out = {1, 'D'};
        return true;
    case Key::Home:
        out = {1, 'H'};
        return true;
    case Key::End:
        out = {1, 'F'};
        return true;
    case Key::Insert:
        out = {2, '~'};
        return true;
    case Key::Delete:
        out = {3, '~'};
        return true;
    case Key::PageUp:
        out = {5, '~'};
        return true;
    case Key::PageDown:
        out = {6, '~'};
        return true;
    case Key::F1:
        out = {1, 'P'};
        return true;
    case Key::F2:
        out = {1, 'Q'};
        return true;
    case Key::F3:
        out = {13, '~'}; // not CSI R, which would read as a cursor position report
        return true;
    case Key::F4:
        out = {1, 'S'};
        return true;
    case Key::F5:
        out = {15, '~'};
        return true;
    case Key::F6:
        out = {17, '~'};
        return true;
    case Key::F7:
        out = {18, '~'};
        return true;
    case Key::F8:
        out = {19, '~'};
        return true;
    case Key::F9:
        out = {20, '~'};
        return true;
    case Key::F10:
        out = {21, '~'};
        return true;
    case Key::F11:
        out = {23, '~'};
        return true;
    case Key::F12:
        out = {24, '~'};
        return true;
    case Key::CapsLock:
        out = {57358, 'u'};
        return true;
    case Key::ScrollLock:
        out = {57359, 'u'};
        return true;
    case Key::NumLock:
        out = {57360, 'u'};
        return true;
    case Key::PrintScreen:
        out = {57361, 'u'};
        return true;
    case Key::Pause:
        out = {57362, 'u'};
        return true;
    case Key::Menu:
        out = {57363, 'u'};
        return true;
    case Key::LeftShift:
        out = {57441, 'u'};
        return true;
    case Key::LeftControl:
        out = {57442, 'u'};
        return true;
    case Key::LeftAlt:
        out = {57443, 'u'};
        return true;
    case Key::LeftSuper:
        out = {57444, 'u'};
        return true;
    case Key::RightShift:
        out = {57447, 'u'};
        return true;
    case Key::RightControl:
        out = {57448, 'u'};
        return true;
    case Key::RightAlt:
        out = {57449, 'u'};
        return true;
    case Key::RightSuper:
        out = {57450, 'u'};
        return true;
    case Key::KeypadBegin:
        out = {1, 'E'};
        return true;
    default:
        break;
    }
    if (k >= Key::F13 && k <= Key::F35) {
        out = {57376u + std::uint32_t(int(k) - int(Key::F13)), 'u'};
        return true;
    }
    if (k >= Key::KeypadSeparator && k <= Key::KeypadDelete) {
        out = {57416u + std::uint32_t(int(k) - int(Key::KeypadSeparator)), 'u'};
        return true;
    }
    if (k >= Key::Keypad0 && k <= Key::KeypadEqual) {
        out = {57399u + std::uint32_t(int(k) - int(Key::Keypad0)), 'u'};
        return true;
    }
    return false;
}

bool isModifierKey(Key k)
{
    return k >= Key::CapsLock && k <= Key::RightSuper && k != Key::PrintScreen && k != Key::Pause && k != Key::Menu
        && k != Key::ScrollLock;
}

void appendTextCodepoints(std::string& out, std::string_view utf8)
{
    bool first = true;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto b0 = static_cast<unsigned char>(utf8[i]);
        const std::size_t len = b0 < 0x80 ? 1 : b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : 2;
        const char32_t cp = firstCodepoint(utf8.substr(i, len));
        i += len;
        if (detail::isControl(cp)) {
            continue; // associated text holds no C0 or C1 control (the spec)
        }
        if (!first) {
            out += ':';
        }
        out += std::to_string(cp);
        first = false;
    }
}

std::string encodeKittyKey(const KeyEvent& e, const Modes& modes)
{
    const std::uint8_t flags = modes.kittyKeyboard;
    const bool reportAll = flags & KittyFlag::ReportAllKeys;
    if (e.action == KeyAction::Release && !(flags & KittyFlag::ReportEvents)) {
        return {};
    }
    if (isModifierKey(e.key) && !reportAll) {
        return {};
    }

    // Lock states are only reported alongside "report all keys". The spec
    // reports them for keys that make no text in disambiguate mode too; they
    // are left out there on purpose, since programs that only want to tell
    // keys apart would read NumLock+Up as a different key from Up.
    const std::uint8_t mods = reportAll ? e.mods : std::uint8_t(e.mods & ~Mod::Locks);
    const std::uint8_t activeMods = mods & ~Mod::Locks;
    const bool producesText = !e.text.empty() && e.key != Key::Enter && e.key != Key::Tab && e.key != Key::Backspace;

    KittyKey kk{0, 'u'};
    const bool functional = kittyFunctional(e.key, kk);
    if (!functional) {
        // A text key: its code is the unshifted character.
        char32_t code = e.unshifted;
        if (code == 0) {
            code = firstCodepoint(e.text);
        }
        if (code == 0) {
            return {};
        }
        kk = {std::uint32_t(code), 'u'};
    }

    // Plain typing stays plain text unless every key is to be reported.
    const bool keypadText = ((e.key >= Key::Keypad0 && e.key <= Key::KeypadEqual) || e.key == Key::KeypadSeparator)
                            && e.key != Key::KeypadEnter && producesText;
    if (!reportAll && (!functional || keypadText) && (activeMods & ~Mod::Shift) == 0) {
        return e.action == KeyAction::Release ? std::string() : std::string(e.text);
    }
    // Enter, Tab and Backspace have no release events without "report all
    // keys", modified or not. Unmodified, they keep their legacy bytes (LNM
    // and DECBKM included), so a shell stays usable after a program exits
    // without restoring the flags.
    const bool legacyKey = e.key == Key::Enter || e.key == Key::Tab || e.key == Key::Backspace;
    if (!reportAll && legacyKey && e.action == KeyAction::Release) {
        return {};
    }
    if (!reportAll && legacyKey && activeMods == 0) {
        return encodeLegacyKey(e, modes);
    }

    const int modifierParam = 1 + mods;
    const int event = e.action == KeyAction::Repeat ? 2 : e.action == KeyAction::Release ? 3 : 1;
    const bool withEvent = (flags & KittyFlag::ReportEvents) && event != 1;

    std::string text;
    if ((flags & KittyFlag::ReportText) && reportAll && producesText && e.action != KeyAction::Release) {
        appendTextCodepoints(text, e.text);
    }

    std::string out = "\x1b[";
    const bool letterFinal = kk.final != 'u' && kk.final != '~';
    if (!(letterFinal && kk.number == 1 && modifierParam == 1 && !withEvent)) {
        out += std::to_string(kk.number);
    }
    if (kk.final == 'u' && (flags & KittyFlag::ReportAlternates) && !functional && (mods & Mod::Shift) && e.shifted != 0
        && std::uint32_t(e.shifted) != kk.number) {
        out += ':' + std::to_string(std::uint32_t(e.shifted));
    }
    if (modifierParam != 1 || withEvent || !text.empty()) {
        out += ';' + std::to_string(modifierParam);
        if (withEvent) {
            out += ':' + std::to_string(event);
        }
    }
    if (!text.empty()) {
        out += ';' + text;
    }
    out += kk.final;
    return out;
}

} // namespace

std::string encodeKey(const KeyEvent& e, const Modes& modes)
{
    if (modes.kittyKeyboard != 0) {
        return encodeKittyKey(e, modes);
    }
    if (e.action == KeyAction::Release || isModifierKey(e.key)) {
        return {};
    }
    return encodeLegacyKey(e, modes);
}

std::string encodeMouse(const MouseEvent& e, const Modes& modes)
{
    const MouseTracking t = modes.mouseTracking;
    if (t == MouseTracking::None) {
        return {};
    }
    const bool wheel = e.button >= MouseButton::WheelUp;
    if (t == MouseTracking::X10 && (e.action != MouseAction::Press || wheel)) {
        return {};
    }
    if (e.action == MouseAction::Motion) {
        if (t == MouseTracking::Normal || t == MouseTracking::X10) {
            return {};
        }
        if (t == MouseTracking::Button && !e.buttonHeld) {
            return {};
        }
    }
    if (wheel && e.action == MouseAction::Release) {
        return {};
    }

    int cb = 0;
    switch (e.button) {
    case MouseButton::Left:
        cb = 0;
        break;
    case MouseButton::Middle:
        cb = 1;
        break;
    case MouseButton::Right:
        cb = 2;
        break;
    case MouseButton::None:
        cb = 3;
        break;
    case MouseButton::WheelUp:
        cb = 64;
        break;
    case MouseButton::WheelDown:
        cb = 65;
        break;
    case MouseButton::WheelLeft:
        cb = 66;
        break;
    case MouseButton::WheelRight:
        cb = 67;
        break;
    }
    if (e.action == MouseAction::Motion) {
        cb += 32;
    }
    if (t != MouseTracking::X10) {
        if (e.mods & Mod::Shift) {
            cb += 4;
        }
        if (e.mods & Mod::Alt) {
            cb += 8;
        }
        if (e.mods & Mod::Ctrl) {
            cb += 16;
        }
    }

    // A release reports button 3 with the modifiers still in it (xterm's
    // BtnCode); SGR names the button and ends with 'm' instead.
    const bool release = e.action == MouseAction::Release;
    const int releaseCode = 3 | (cb & 0x1C);
    char buf[64];
    int n = 0;
    switch (modes.mouseFormat) {
    case MouseFormat::Sgr:
        n = std::snprintf(buf, sizeof buf, "\x1b[<%d;%d;%d%c", cb, e.col + 1, e.row + 1, release ? 'm' : 'M');
        return std::string(buf, std::size_t(n));
    case MouseFormat::SgrPixels:
        n = std::snprintf(buf, sizeof buf, "\x1b[<%d;%d;%d%c", cb, e.pixelX + 1, e.pixelY + 1, release ? 'm' : 'M');
        return std::string(buf, std::size_t(n));
    case MouseFormat::Urxvt:
        n = std::snprintf(buf, sizeof buf, "\x1b[%d;%d;%dM", (release ? releaseCode : cb) + 32, e.col + 1, e.row + 1);
        return std::string(buf, std::size_t(n));
    case MouseFormat::Utf8: {
        const int x = e.col + 1 + 32;
        const int y = e.row + 1 + 32;
        if (x > 2047 || y > 2047) {
            return {};
        }
        std::string s = "\x1b[M";
        s.push_back(char((release ? releaseCode : cb) + 32));
        detail::appendUtf8(s, char32_t(x));
        detail::appendUtf8(s, char32_t(y));
        return s;
    }
    case MouseFormat::X10: {
        const int x = e.col + 1 + 32;
        const int y = e.row + 1 + 32;
        if (x > 255 || y > 255) {
            return {};
        }
        std::string s = "\x1b[M";
        s.push_back(char((release ? releaseCode : cb) + 32));
        s.push_back(char(x));
        s.push_back(char(y));
        return s;
    }
    }
    return {};
}

std::string encodePaste(std::string_view text, const Modes& modes)
{
    std::string body;
    body.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue;
        }
        if (c == '\n' || c == '\r') {
            body.push_back('\r');
            continue;
        }
        // Other controls are dropped: one could run a command in a line
        // editor (bash binds C-o to operate-and-get-next) with no newline for
        // the unsafe-paste prompt to catch. C1 controls in UTF-8
        // (U+0080-U+009F) are C2 80 to C2 9F.
        const bool c1 = c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) >= 0x80
            && static_cast<unsigned char>(text[i + 1]) <= 0x9F;
        // A dropped control must not join the bytes around it into one: C2,
        // 01, 85 would otherwise leave C2 85.
        const bool joinsC1 = c >= 0x80 && c <= 0x9F && !body.empty() && static_cast<unsigned char>(body.back()) == 0xC2;
        if (c1) {
            ++i;
        } else if (joinsC1) {
            body.pop_back();
        } else if (c == '\t' || (c >= 0x20 && c != 0x7F)) {
            body.push_back(char(c));
        }
    }
    if (!modes.bracketedPaste) {
        return body;
    }
    // Without ESC the text cannot hold the end marker.
    return "\x1b[200~" + body + "\x1b[201~";
}

std::string encodeFocus(bool focused, const Modes& modes)
{
    if (!modes.focusEvents) {
        return {};
    }
    return focused ? "\x1b[I" : "\x1b[O";
}

} // namespace AfterglowVt
