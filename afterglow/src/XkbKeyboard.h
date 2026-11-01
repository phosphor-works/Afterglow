// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <utility>

struct wl_keyboard;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace Afterglow {

/// The compositor's live keyboard layout and lock state.
///
/// Qt reports the shifted symbol for punctuation and does not expose the
/// keymap or Caps/Num Lock, which the kitty keyboard protocol needs. This
/// binds a second wl_keyboard on Qt's own Wayland connection and follows the
/// keymap and modifier events the compositor sends to it.
class XkbKeyboard
{
public:
    static XkbKeyboard& instance();

    /// The unshifted character for an xkb keycode in the active layout group, or 0.
    char32_t unshiftedCodepoint(std::uint32_t keycode) const;
    /// The character with Shift (shift level 1) in the active layout group, or 0.
    char32_t shiftedCodepoint(std::uint32_t keycode) const;
    /// The unshifted and shifted characters in the first layout group where
    /// the key gives an ASCII character, or zeros. Ctrl combinations use it
    /// while a non-Latin layout is active, as Qt does for shortcuts.
    std::pair<char32_t, char32_t> latinCodepoints(std::uint32_t keycode) const;
    /// The unshifted keysym in the active layout group, or 0 (XKB_KEY_NoSymbol)
    /// without a keymap. Remapped keys (caps:escape) report what they became.
    std::uint32_t keysym(std::uint32_t keycode) const;
    bool capsLock() const;
    bool numLock() const;

    XkbKeyboard(const XkbKeyboard&) = delete;
    XkbKeyboard& operator=(const XkbKeyboard&) = delete;

private:
    XkbKeyboard();
    char32_t codepoint(std::uint32_t keycode, std::uint32_t layout, std::uint32_t level) const;
    ~XkbKeyboard();

    static void onKeymap(void* data, wl_keyboard*, std::uint32_t format, int fd, std::uint32_t size);
    static void onModifiers(void* data, wl_keyboard*, std::uint32_t serial, std::uint32_t depressed,
                            std::uint32_t latched, std::uint32_t locked, std::uint32_t group);

    wl_keyboard* m_keyboard = nullptr;
    xkb_context* m_context = nullptr;
    xkb_keymap* m_keymap = nullptr;
    xkb_state* m_state = nullptr;
};

} // namespace Afterglow
