// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/afterglowvt_export.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace AfterglowVt {

struct Modes;

namespace Mod {
inline constexpr std::uint8_t Shift = 1 << 0;
inline constexpr std::uint8_t Alt = 1 << 1;
inline constexpr std::uint8_t Ctrl = 1 << 2;
inline constexpr std::uint8_t Super = 1 << 3;
// Lock states, reported only by the kitty protocol.
inline constexpr std::uint8_t CapsLock = 1 << 6;
inline constexpr std::uint8_t NumLock = 1 << 7;
inline constexpr std::uint8_t Locks = CapsLock | NumLock;
} // namespace Mod

/// Kitty keyboard protocol enhancement flags (CSI > flags u).
namespace KittyFlag {
inline constexpr std::uint8_t Disambiguate = 1;
inline constexpr std::uint8_t ReportEvents = 2;
inline constexpr std::uint8_t ReportAlternates = 4;
inline constexpr std::uint8_t ReportAllKeys = 8;
inline constexpr std::uint8_t ReportText = 16;
inline constexpr std::uint8_t All = 31;
} // namespace KittyFlag

/// Keys that are not plain text. Text keys use Key::Text with KeyEvent::text.
enum class Key : std::uint8_t {
    Text,
    Enter,
    Tab,
    Backspace,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    Insert,
    Delete,
    PageUp,
    PageDown,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Keypad0, Keypad1, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
    KeypadDecimal,
    KeypadDivide,
    KeypadMultiply,
    KeypadSubtract,
    KeypadAdd,
    KeypadEnter,
    KeypadEqual,
    CapsLock,
    ScrollLock,
    NumLock,
    PrintScreen,
    Pause,
    Menu,
    LeftShift,
    LeftControl,
    LeftAlt,
    LeftSuper,
    RightShift,
    RightControl,
    RightAlt,
    RightSuper,
    // The keypad's other keys: the separator, and with NumLock off the
    // navigation keys. Legacy encodings send them as the main keys; the kitty
    // protocol tells them apart.
    KeypadSeparator,
    KeypadLeft,
    KeypadRight,
    KeypadUp,
    KeypadDown,
    KeypadPageUp,
    KeypadPageDown,
    KeypadHome,
    KeypadEnd,
    KeypadInsert,
    KeypadDelete,
    KeypadBegin,
};

enum class KeyAction : std::uint8_t { Press, Repeat, Release };

struct KeyEvent
{
    Key key = Key::Text;
    std::uint8_t mods = 0;
    KeyAction action = KeyAction::Press;
    /// UTF-8 text the key produces with the current layout (Key::Text, keypad digits).
    std::string_view text;
    /// The unshifted character of the key for Key::Text (Ctrl combos, kitty key codes).
    char32_t unshifted = 0;
    /// The character with Shift applied, when Shift is held (kitty alternate keys).
    char32_t shifted = 0;
};

/// Encodes a key event: the kitty keyboard protocol when the application
/// enabled it (Modes::kittyKeyboard), the legacy xterm encoding otherwise.
/// Returns an empty string for events the active protocol does not report.
AFTERGLOWVT_EXPORT std::string encodeKey(const KeyEvent& event, const Modes& modes);

enum class MouseButton : std::uint8_t { Left, Middle, Right, None, WheelUp, WheelDown, WheelLeft, WheelRight };
enum class MouseAction : std::uint8_t { Press, Release, Motion };

struct MouseEvent
{
    MouseButton button = MouseButton::None;
    MouseAction action = MouseAction::Press;
    std::uint8_t mods = 0;
    int col = 0; ///< 0-based cell
    int row = 0;
    int pixelX = 0; ///< for SGR-pixels (1016)
    int pixelY = 0;
    /// A button is held during motion (needed for 1002).
    bool buttonHeld = false;
};

/// Encodes a mouse event for the current tracking mode, or returns an empty
/// string when the mode does not report it.
AFTERGLOWVT_EXPORT std::string encodeMouse(const MouseEvent& event, const Modes& modes);

/// Prepares pasted text for the terminal. CRLF and LF become CR. Control
/// characters other than HT, CR and LF (C0, DEL and C1) are dropped, so a
/// paste cannot run a line editor command or end a bracketed paste early.
/// Bracketed paste markers surround the text when the mode is on.
AFTERGLOWVT_EXPORT std::string encodePaste(std::string_view text, const Modes& modes);

AFTERGLOWVT_EXPORT std::string encodeFocus(bool focused, const Modes& modes);

} // namespace AfterglowVt
