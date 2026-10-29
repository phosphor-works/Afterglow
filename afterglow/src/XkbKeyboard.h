// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

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

    bool available() const noexcept { return m_state != nullptr; }
    /// The unshifted character for an xkb keycode in the active layout group, or 0.
    char32_t unshiftedCodepoint(std::uint32_t keycode) const;
    bool capsLock() const;
    bool numLock() const;

    XkbKeyboard(const XkbKeyboard&) = delete;
    XkbKeyboard& operator=(const XkbKeyboard&) = delete;

private:
    XkbKeyboard();
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
