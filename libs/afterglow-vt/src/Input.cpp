// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Input.h>
#include <AfterglowVt/Terminal.h>

#include <algorithm>
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

std::string ss3Key(char final, std::uint8_t mods)
{
    if (mods == 0) {
        return std::string("\x1bO") + final;
    }
    return "\x1b[1;" + std::to_string(modParam(mods)) + final;
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

void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

} // namespace

namespace {

std::string encodeLegacyKey(const KeyEvent& e, const Modes& modes)
{
    const std::uint8_t m = e.mods & ~Mod::Locks;
    switch (e.key) {
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
        return ss3Key('P', m);
    case Key::F2:
        return ss3Key('Q', m);
    case Key::F3:
        return ss3Key('R', m);
    case Key::F4:
        return ss3Key('S', m);
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
        return withAlt(modes.newline ? "\r\n" : "\r", m);
    case Key::Tab:
        if (m & Mod::Shift) {
            return "\x1b[Z";
        }
        return withAlt("\t", m);
    case Key::Backspace: {
        // DECBKM swaps which of BS and DEL the key and Ctrl+key send.
        const bool bs = ((m & Mod::Ctrl) != 0) != modes.backarrowSendsBs;
        return withAlt(bs ? "\x08" : "\x7f", m);
    }
    case Key::Escape:
        return withAlt("\x1b", m);
    case Key::KeypadEnter:
        if (modes.keypadApp) {
            return ss3Key('M', m);
        }
        return withAlt(modes.newline ? "\r\n" : "\r", m);
    case Key::Text:
        break;
    default: {
        // Remaining keypad keys.
        if (modes.keypadApp) {
            static constexpr char Finals[] = "pqrstuvwxynojmkX";
            const int idx = int(e.key) - int(Key::Keypad0);
            if (idx >= 0 && idx < 16) {
                return ss3Key(Finals[idx], m);
            }
        }
        return withAlt(std::string(e.text), m);
    }
    }

    // Plain text keys.
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
        appendUtf8(s, e.unshifted);
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
    default:
        break;
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

void appendTextCodepoints(std::string& out, std::string_view utf8)
{
    bool first = true;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto b0 = static_cast<unsigned char>(utf8[i]);
        const std::size_t len = b0 < 0x80 ? 1 : b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : 2;
        const char32_t cp = firstCodepoint(utf8.substr(i, len));
        i += len;
        if (cp < 0x20 || cp == 0x7F) {
            continue;
        }
        if (!first) {
            out += ':';
        }
        out += std::to_string(cp);
        first = false;
    }
}

std::string encodeKittyKey(const KeyEvent& e, std::uint8_t flags)
{
    const bool reportAll = flags & KittyFlag::ReportAllKeys;
    if (e.action == KeyAction::Release && !(flags & KittyFlag::ReportEvents)) {
        return {};
    }
    if (isModifierKey(e.key) && !reportAll) {
        return {};
    }

    // Lock states are only reported alongside "report all keys".
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
    const bool keypadText = e.key >= Key::Keypad0 && e.key <= Key::KeypadEqual && e.key != Key::KeypadEnter
                            && producesText;
    if (!reportAll && (!functional || keypadText) && (activeMods & ~Mod::Shift) == 0) {
        return e.action == KeyAction::Release ? std::string() : std::string(e.text);
    }
    // Enter, Tab and Backspace keep their legacy bytes when unmodified, so a
    // shell stays usable after a program exits without restoring the flags.
    if (!reportAll && activeMods == 0
        && (e.key == Key::Enter || e.key == Key::Tab || e.key == Key::Backspace)) {
        if (e.action == KeyAction::Release) {
            return {};
        }
        return e.key == Key::Enter ? "\r" : e.key == Key::Tab ? "\t" : "\x7f";
    }

    const int modParam = 1 + mods;
    const int event = e.action == KeyAction::Repeat ? 2 : e.action == KeyAction::Release ? 3 : 1;
    const bool withEvent = (flags & KittyFlag::ReportEvents) && event != 1;

    std::string text;
    if ((flags & KittyFlag::ReportText) && reportAll && producesText && e.action != KeyAction::Release) {
        appendTextCodepoints(text, e.text);
    }

    std::string out = "\x1b[";
    const bool letterFinal = kk.final != 'u' && kk.final != '~';
    if (!(letterFinal && kk.number == 1 && modParam == 1 && !withEvent)) {
        out += std::to_string(kk.number);
    }
    if (kk.final == 'u' && (flags & KittyFlag::ReportAlternates) && !functional && (mods & Mod::Shift) && e.shifted != 0
        && std::uint32_t(e.shifted) != kk.number) {
        out += ':' + std::to_string(std::uint32_t(e.shifted));
    }
    if (modParam != 1 || withEvent || !text.empty()) {
        out += ';' + std::to_string(modParam);
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
        return encodeKittyKey(e, modes.kittyKeyboard);
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

    const bool release = e.action == MouseAction::Release;
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
        n = std::snprintf(buf, sizeof buf, "\x1b[%d;%d;%dM", (release ? 3 : cb) + 32, e.col + 1, e.row + 1);
        return std::string(buf, std::size_t(n));
    case MouseFormat::Utf8: {
        const int x = e.col + 1 + 32;
        const int y = e.row + 1 + 32;
        if (x > 2047 || y > 2047) {
            return {};
        }
        std::string s = "\x1b[M";
        s.push_back(char((release ? 3 : cb) + 32));
        appendUtf8(s, char32_t(x));
        appendUtf8(s, char32_t(y));
        return s;
    }
    case MouseFormat::X10: {
        const int x = e.col + 1 + 32;
        const int y = e.row + 1 + 32;
        if (x > 255 || y > 255) {
            return {};
        }
        std::string s = "\x1b[M";
        s.push_back(char((release ? 3 : cb) + 32));
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
        const char c = text[i];
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue;
        }
        body.push_back(c == '\n' ? '\r' : c);
    }
    if (!modes.bracketedPaste) {
        return body;
    }
    constexpr std::string_view End = "\x1b[201~";
    for (auto pos = body.find(End); pos != std::string::npos; pos = body.find(End, pos)) {
        body.erase(pos, End.size());
    }
    return "\x1b[200~" + body + std::string(End);
}

std::string encodeFocus(bool focused, const Modes& modes)
{
    if (!modes.focusEvents) {
        return {};
    }
    return focused ? "\x1b[I" : "\x1b[O";
}

} // namespace AfterglowVt
