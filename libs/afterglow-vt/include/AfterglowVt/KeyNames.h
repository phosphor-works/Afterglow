// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Input.h>
#include <AfterglowVt/afterglowvt_export.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace AfterglowVt {

/// A key that a name gives, as `afterglow ctl send-key` and `kitten @
/// send-key` take them: modifiers and a key joined with '+', such as
/// "ctrl+c", "Ctrl+Shift+T", "alt+x", "Return", "page_up" or "f5". A name
/// with no layout behind it: a character key is the key of the US layout
/// that types it.
struct NamedKey
{
    Key key = Key::Text;
    std::uint8_t mods = 0;
    /// What the key types: for Key::Text its character, empty with Ctrl,
    /// which types a control; for a keypad key its character ("kp5": "5").
    std::string text;
    char32_t unshifted = 0;
    char32_t shifted = 0;

    /// The event of the key, which points into text: valid while this lives.
    KeyEvent event(KeyAction action = KeyAction::Press) const;
};

/// The most keys that one call sends (`afterglow ctl send-key`).
inline constexpr std::size_t MaxKeysAtOnce = 4096;

/// The key that @p name gives, else nullopt: an unknown modifier or key
/// name, an empty name, a character that is a control, or text that is not
/// one UTF-8 character. Names and modifiers ignore case, '_' and '-'; a
/// letter key ignores case too, so "ctrl+C" is Ctrl with the C key, and
/// "shift+a" types A. The keys "+" and "-" are "plus" and "minus", or a '+'
/// or '-' at the end after a modifier ("ctrl++"): "++" names no key.
AFTERGLOWVT_EXPORT std::optional<NamedKey> parseKeyName(std::string_view name);

/// The bytes that a press and a release of @p key send to a program in
/// @p modes, as a typed key does: the kitty keyboard protocol when the
/// program enabled it, the legacy encoding otherwise.
AFTERGLOWVT_EXPORT std::string encodeNamedKey(const NamedKey& key, const Modes& modes);

} // namespace AfterglowVt
