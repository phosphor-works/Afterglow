// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "KeyMapping.h"

#include "XkbKeyboard.h"

#include <QKeyEvent>

namespace Afterglow {

using AfterglowVt::Key;

std::uint8_t mapModifiers(Qt::KeyboardModifiers mods)
{
    std::uint8_t m = 0;
    if (mods & Qt::ShiftModifier) {
        m |= AfterglowVt::Mod::Shift;
    }
    if (mods & Qt::AltModifier) {
        m |= AfterglowVt::Mod::Alt;
    }
    if (mods & Qt::ControlModifier) {
        m |= AfterglowVt::Mod::Ctrl;
    }
    if (mods & Qt::MetaModifier) {
        m |= AfterglowVt::Mod::Super;
    }
    return m;
}

namespace {

Key keypadKey(int qtKey)
{
    switch (qtKey) {
    case Qt::Key_0:
    case Qt::Key_Insert:
        return Key::Keypad0;
    case Qt::Key_1:
    case Qt::Key_End:
        return Key::Keypad1;
    case Qt::Key_2:
    case Qt::Key_Down:
        return Key::Keypad2;
    case Qt::Key_3:
    case Qt::Key_PageDown:
        return Key::Keypad3;
    case Qt::Key_4:
    case Qt::Key_Left:
        return Key::Keypad4;
    case Qt::Key_5:
    case Qt::Key_Clear:
        return Key::Keypad5;
    case Qt::Key_6:
    case Qt::Key_Right:
        return Key::Keypad6;
    case Qt::Key_7:
    case Qt::Key_Home:
        return Key::Keypad7;
    case Qt::Key_8:
    case Qt::Key_Up:
        return Key::Keypad8;
    case Qt::Key_9:
    case Qt::Key_PageUp:
        return Key::Keypad9;
    case Qt::Key_Period:
    case Qt::Key_Comma:
    case Qt::Key_Delete:
        return Key::KeypadDecimal;
    case Qt::Key_Slash:
        return Key::KeypadDivide;
    case Qt::Key_Asterisk:
        return Key::KeypadMultiply;
    case Qt::Key_Minus:
        return Key::KeypadSubtract;
    case Qt::Key_Plus:
        return Key::KeypadAdd;
    case Qt::Key_Enter:
        return Key::KeypadEnter;
    case Qt::Key_Equal:
        return Key::KeypadEqual;
    default:
        return Key::Text;
    }
}

} // namespace

namespace {

/// Modifier and lock keys by xkb keycode (evdev code + 8), so left and right
/// are told apart.
Key modifierKey(quint32 keycode)
{
    switch (keycode) {
    case 50:
        return Key::LeftShift;
    case 62:
        return Key::RightShift;
    case 37:
        return Key::LeftControl;
    case 105:
        return Key::RightControl;
    case 64:
        return Key::LeftAlt;
    case 108:
        return Key::RightAlt;
    case 133:
        return Key::LeftSuper;
    case 134:
        return Key::RightSuper;
    case 66:
        return Key::CapsLock;
    case 77:
        return Key::NumLock;
    case 78:
        return Key::ScrollLock;
    case 107:
        return Key::PrintScreen;
    case 127:
        return Key::Pause;
    case 135:
        return Key::Menu;
    default:
        return Key::Text;
    }
}

char32_t firstCodepoint(const QString& s)
{
    if (s.isEmpty()) {
        return 0;
    }
    const char32_t cp = s.at(0).isHighSurrogate() && s.size() > 1 ? QChar::surrogateToUcs4(s.at(0), s.at(1))
                                                                     : char32_t(s.at(0).unicode());
    return cp < 0x20 || cp == 0x7F ? 0 : cp;
}

} // namespace

AfterglowVt::KeyEvent mapKeyEvent(const QKeyEvent* e, QByteArray& textStorage)
{
    AfterglowVt::KeyEvent out;
    out.mods = mapModifiers(e->modifiers());
    if (e->type() == QEvent::KeyRelease) {
        out.action = AfterglowVt::KeyAction::Release;
    } else if (e->isAutoRepeat()) {
        out.action = AfterglowVt::KeyAction::Repeat;
    }
    const XkbKeyboard& xkb = XkbKeyboard::instance();
    if (xkb.capsLock()) {
        out.mods |= AfterglowVt::Mod::CapsLock;
    }
    if (xkb.numLock()) {
        out.mods |= AfterglowVt::Mod::NumLock;
    }
    const int k = e->key();

    if (const Key mk = modifierKey(e->nativeScanCode()); mk != Key::Text) {
        out.key = mk;
        return out;
    }

    // Text with control characters stripped: Ctrl combos are encoded from the key.
    const QString text = e->text();
    bool printable = !text.isEmpty();
    for (QChar c : text) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7F) {
            printable = false;
        }
    }
    textStorage = printable ? text.toUtf8() : QByteArray();
    out.text = std::string_view(textStorage.constData(), std::size_t(textStorage.size()));

    if (e->modifiers() & Qt::KeypadModifier) {
        const Key kp = keypadKey(k);
        // With NumLock off the keypad sends navigation keys and no text; those
        // fall through to the regular mapping below.
        if (kp != Key::Text && (!textStorage.isEmpty() || kp == Key::KeypadEnter)) {
            out.key = kp;
            return out;
        }
    }

    switch (k) {
    case Qt::Key_Return:
        out.key = Key::Enter;
        break;
    case Qt::Key_Enter:
        out.key = Key::KeypadEnter;
        break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        out.key = Key::Tab;
        if (k == Qt::Key_Backtab) {
            out.mods |= AfterglowVt::Mod::Shift;
        }
        break;
    case Qt::Key_Backspace:
        out.key = Key::Backspace;
        break;
    case Qt::Key_Escape:
        out.key = Key::Escape;
        break;
    case Qt::Key_Up:
        out.key = Key::Up;
        break;
    case Qt::Key_Down:
        out.key = Key::Down;
        break;
    case Qt::Key_Left:
        out.key = Key::Left;
        break;
    case Qt::Key_Right:
        out.key = Key::Right;
        break;
    case Qt::Key_Home:
        out.key = Key::Home;
        break;
    case Qt::Key_End:
        out.key = Key::End;
        break;
    case Qt::Key_Insert:
        out.key = Key::Insert;
        break;
    case Qt::Key_Delete:
        out.key = Key::Delete;
        break;
    case Qt::Key_PageUp:
        out.key = Key::PageUp;
        break;
    case Qt::Key_PageDown:
        out.key = Key::PageDown;
        break;
    default:
        if (k >= Qt::Key_F1 && k <= Qt::Key_F12) {
            out.key = Key(int(Key::F1) + (k - Qt::Key_F1));
        } else {
            out.key = Key::Text;
            // The layout's unshifted character; Qt's key() is the shifted
            // symbol for punctuation (Shift+1 reports '!').
            out.unshifted = xkb.unshiftedCodepoint(e->nativeScanCode());
            if (out.unshifted == 0 && k >= 0x20 && k < 0x7F) {
                out.unshifted = (k >= 'A' && k <= 'Z') ? char32_t(k - 'A' + 'a') : char32_t(k);
            }
            if (e->modifiers() & Qt::ShiftModifier) {
                out.shifted = firstCodepoint(text);
            }
        }
        break;
    }
    return out;
}

} // namespace Afterglow
