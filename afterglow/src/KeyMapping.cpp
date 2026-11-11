// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "KeyMapping.h"

#include "XkbKeyboard.h"
#include "config/ModifierRemap.h"

#include <QKeyEvent>

#include <xkbcommon/xkbcommon-keysyms.h>

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

/// A keypad key with Qt::KeypadModifier. With NumLock off the keypad gives
/// navigation keys (Qt reports Home, Down…): they stay keypad keys, which the
/// kitty protocol tells apart and legacy encodings send as the main keys.
Key keypadKey(int qtKey)
{
    switch (qtKey) {
    case Qt::Key_0:
        return Key::Keypad0;
    case Qt::Key_1:
        return Key::Keypad1;
    case Qt::Key_2:
        return Key::Keypad2;
    case Qt::Key_3:
        return Key::Keypad3;
    case Qt::Key_4:
        return Key::Keypad4;
    case Qt::Key_5:
        return Key::Keypad5;
    case Qt::Key_6:
        return Key::Keypad6;
    case Qt::Key_7:
        return Key::Keypad7;
    case Qt::Key_8:
        return Key::Keypad8;
    case Qt::Key_9:
        return Key::Keypad9;
    case Qt::Key_Insert:
        return Key::KeypadInsert;
    case Qt::Key_End:
        return Key::KeypadEnd;
    case Qt::Key_Down:
        return Key::KeypadDown;
    case Qt::Key_PageDown:
        return Key::KeypadPageDown;
    case Qt::Key_Left:
        return Key::KeypadLeft;
    case Qt::Key_Clear:
        return Key::KeypadBegin;
    case Qt::Key_Right:
        return Key::KeypadRight;
    case Qt::Key_Home:
        return Key::KeypadHome;
    case Qt::Key_Up:
        return Key::KeypadUp;
    case Qt::Key_PageUp:
        return Key::KeypadPageUp;
    case Qt::Key_Delete:
        return Key::KeypadDelete;
    case Qt::Key_Period:
        return Key::KeypadDecimal;
    case Qt::Key_Comma:
        // KP_Separator: the decimal key of layouts with a decimal comma.
        return Key::KeypadSeparator;
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

/// Modifier and lock keys by the keysym the active layout gives the key, so
/// a remapped key (caps:escape, ctrl:nocaps) reports what it became.
Key modifierKeyFromKeysym(std::uint32_t keysym)
{
    switch (keysym) {
    case XKB_KEY_Shift_L:
        return Key::LeftShift;
    case XKB_KEY_Shift_R:
        return Key::RightShift;
    case XKB_KEY_Control_L:
        return Key::LeftControl;
    case XKB_KEY_Control_R:
        return Key::RightControl;
    case XKB_KEY_Alt_L:
    case XKB_KEY_Meta_L:
        return Key::LeftAlt;
    case XKB_KEY_Alt_R:
    case XKB_KEY_Meta_R:
    case XKB_KEY_ISO_Level3_Shift: // AltGr, as the keycode table had it
        return Key::RightAlt;
    case XKB_KEY_Super_L:
        return Key::LeftSuper;
    case XKB_KEY_Super_R:
        return Key::RightSuper;
    case XKB_KEY_Caps_Lock:
        return Key::CapsLock;
    case XKB_KEY_Num_Lock:
        return Key::NumLock;
    case XKB_KEY_Scroll_Lock:
        return Key::ScrollLock;
    case XKB_KEY_Print:
        return Key::PrintScreen;
    case XKB_KEY_Pause:
        return Key::Pause;
    case XKB_KEY_Menu:
        return Key::Menu;
    default:
        return Key::Text;
    }
}

/// The same keys by xkb keycode (evdev code + 8), for when there is no keymap.
Key modifierKeyFromKeycode(quint32 keycode)
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

/// Whether Qt's key agrees that @p key is the modifier or lock key it names.
bool qtKeyMatches(Key key, int qtKey)
{
    switch (key) {
    case Key::LeftShift:
    case Key::RightShift:
        return qtKey == Qt::Key_Shift;
    case Key::LeftControl:
    case Key::RightControl:
        return qtKey == Qt::Key_Control;
    case Key::LeftAlt:
    case Key::RightAlt:
        return qtKey == Qt::Key_Alt || qtKey == Qt::Key_AltGr || qtKey == Qt::Key_Meta;
    case Key::LeftSuper:
    case Key::RightSuper:
        return qtKey == Qt::Key_Super_L || qtKey == Qt::Key_Super_R || qtKey == Qt::Key_Meta;
    case Key::CapsLock:
        return qtKey == Qt::Key_CapsLock;
    case Key::NumLock:
        return qtKey == Qt::Key_NumLock;
    case Key::ScrollLock:
        return qtKey == Qt::Key_ScrollLock;
    case Key::PrintScreen:
        return qtKey == Qt::Key_Print;
    case Key::Pause:
        return qtKey == Qt::Key_Pause;
    case Key::Menu:
        return qtKey == Qt::Key_Menu;
    default:
        return false;
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

char32_t bindingBaseKey(const QKeyEvent* event)
{
    const XkbKeyboard& xkb = XkbKeyboard::instance();
    char32_t base = xkb.unshiftedCodepoint(event->nativeScanCode());
    // "H" in a binding means the key that types h on the first Latin layout.
    if (base < 0x20 || base >= 0x7F) base = xkb.latinCodepoints(event->nativeScanCode()).first;
    return base;
}

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

    // The layout decides what a key is: a keycode table would call a Caps
    // Lock remapped to Escape Caps Lock, and send nothing for it.
    const std::uint32_t keysym = xkb.keysym(e->nativeScanCode());
    const Key mk = keysym != XKB_KEY_NoSymbol ? modifierKeyFromKeysym(keysym)
        : qtKeyMatches(modifierKeyFromKeycode(e->nativeScanCode()), k) ? modifierKeyFromKeycode(e->nativeScanCode())
                                                                        : Key::Text;
    if (mk != Key::Text) {
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
        if (const Key kp = keypadKey(k); kp != Key::Text) {
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
        if (k >= Qt::Key_F1 && k <= Qt::Key_F35) {
            out.key = Key(int(Key::F1) + (k - Qt::Key_F1));
        } else {
            out.key = Key::Text;
            // The layout's unshifted character; Qt's key() is the shifted
            // symbol for punctuation (Shift+1 reports '!').
            out.unshifted = xkb.unshiftedCodepoint(e->nativeScanCode());
            char32_t shifted = xkb.shiftedCodepoint(e->nativeScanCode());
            // Ctrl+C under a Cyrillic or Greek layout is still ^C: control
            // characters come from the Latin layout, as Qt's shortcuts do.
            if ((e->modifiers() & Qt::ControlModifier) && out.unshifted >= 0x80) {
                if (const auto latin = xkb.latinCodepoints(e->nativeScanCode()); latin.first != 0) {
                    out.unshifted = latin.first;
                    shifted = latin.second;
                }
            }
            if (out.unshifted == 0 && k >= 0x20 && k < 0x7F) {
                out.unshifted = (k >= 'A' && k <= 'Z') ? char32_t(k - 'A' + 'a') : char32_t(k);
            }
            if (e->modifiers() & Qt::ShiftModifier) {
                // With Ctrl the text is a control character: take the shifted
                // character from the layout, or from Qt's key.
                out.shifted = firstCodepoint(text);
                if (!out.shifted || (e->modifiers() & Qt::ControlModifier)) {
                    out.shifted = shifted >= 0x20 && shifted != 0x7F ? shifted
                        : k >= 0x20 && k < 0x7F ? char32_t(k) : out.shifted;
                }
            }
        }
        break;
    }
    return out;
}

namespace {

/// The modifier that a Qt modifier key is. Qt names Super Meta.
Qt::KeyboardModifier modifierOfQtKey(int key)
{
    switch (key) {
    case Qt::Key_Shift:
        return Qt::ShiftModifier;
    case Qt::Key_Control:
        return Qt::ControlModifier;
    case Qt::Key_Alt:
        return Qt::AltModifier;
    case Qt::Key_Meta:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
        return Qt::MetaModifier;
    default:
        return Qt::NoModifier;
    }
}

/// The modifier that QKeyEvent::modifiers() flips for @p key: for a modifier
/// key it reports the state after the key, not the one stored.
Qt::KeyboardModifier flippedModifier(int key)
{
    switch (key) {
    case Qt::Key_Shift:
        return Qt::ShiftModifier;
    case Qt::Key_Control:
        return Qt::ControlModifier;
    case Qt::Key_Alt:
        return Qt::AltModifier;
    case Qt::Key_Meta:
        return Qt::MetaModifier;
    case Qt::Key_AltGr:
        return Qt::GroupSwitchModifier;
    default:
        return Qt::NoModifier;
    }
}

int qtKeyOfModifier(Qt::KeyboardModifier modifier, bool right)
{
    switch (modifier) {
    case Qt::ShiftModifier:
        return Qt::Key_Shift;
    case Qt::ControlModifier:
        return Qt::Key_Control;
    case Qt::AltModifier:
        return Qt::Key_Alt;
    default:
        return right ? Qt::Key_Super_R : Qt::Key_Super_L;
    }
}

} // namespace

std::unique_ptr<QKeyEvent> remappedKeyEvent(const QKeyEvent& event, const ModifierRemap& remap)
{
    if (remap.isEmpty()) return nullptr;
    const Qt::KeyboardModifiers before = event.modifiers();
    const Qt::KeyboardModifiers after = remap.apply(before);
    int key = event.key();
    if (const Qt::KeyboardModifier own = modifierOfQtKey(key); own != Qt::NoModifier) {
        // The key's own press and release name the modifier it becomes, as
        // in kitty, when that is one modifier.
        if (const Qt::KeyboardModifier target = remap.singleTarget(own); target != Qt::NoModifier) {
            const std::uint32_t keysym = XkbKeyboard::instance().keysym(event.nativeScanCode());
            const bool right = keysym == XKB_KEY_Shift_R || keysym == XKB_KEY_Control_R || keysym == XKB_KEY_Alt_R
                || keysym == XKB_KEY_Meta_R || keysym == XKB_KEY_Super_R;
            key = qtKeyOfModifier(target, right);
        }
    }
    if (after == before && key == event.key()) return nullptr;
    // The text is the one of the modifiers that the key had. With Ctrl it
    // goes: the encoder makes control characters from the key. Without Ctrl,
    // and with Ctrl or Shift changed, it is the layout's character for the
    // new Shift state.
    QString text = event.text();
    if (after & Qt::ControlModifier) {
        text.clear();
    } else if ((before & Qt::ControlModifier) || ((before ^ after) & Qt::ShiftModifier)) {
        const XkbKeyboard& xkb = XkbKeyboard::instance();
        const char32_t c = (after & Qt::ShiftModifier) ? xkb.shiftedCodepoint(event.nativeScanCode())
                                                       : xkb.unshiftedCodepoint(event.nativeScanCode());
        if (c >= 0x20 && c != 0x7F) text = QString::fromUcs4(&c, 1);
    }
    // Stored so that the new event's modifiers() reads as the remapped ones.
    return std::make_unique<QKeyEvent>(event.type(), key, after ^ flippedModifier(key), event.nativeScanCode(),
                                       event.nativeVirtualKey(), event.nativeModifiers(), text, event.isAutoRepeat(),
                                       event.count(), event.device());
}

AfterglowVt::Key remappedModifierKey(Key key, const ModifierRemap& remap)
{
    struct Side
    {
        Key key;
        Qt::KeyboardModifier modifier;
        bool right;
    };
    static constexpr Side Keys[] = {
        {Key::LeftShift, Qt::ShiftModifier, false},    {Key::LeftControl, Qt::ControlModifier, false},
        {Key::LeftAlt, Qt::AltModifier, false},        {Key::LeftSuper, Qt::MetaModifier, false},
        {Key::RightShift, Qt::ShiftModifier, true},    {Key::RightControl, Qt::ControlModifier, true},
        {Key::RightAlt, Qt::AltModifier, true},        {Key::RightSuper, Qt::MetaModifier, true},
    };
    for (const Side& from : Keys) {
        if (from.key != key) continue;
        const Qt::KeyboardModifier target = remap.singleTarget(from.modifier);
        if (target == Qt::NoModifier) return key;
        for (const Side& to : Keys) {
            if (to.modifier == target && to.right == from.right) return to.key;
        }
    }
    return key;
}

} // namespace Afterglow
