// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

struct wl_array;
struct wl_keyboard;
struct wl_surface;
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
    /// The unshifted character of the key in the PC-101 US layout, or 0:
    /// kitty's "base layout key". It does not depend on the user's layouts.
    char32_t baseLayoutCodepoint(std::uint32_t keycode) const;
    /// The keys held down right after the key event of @p keycode at
    /// @p time, as xkb keycodes, while an Afterglow window has the keyboard
    /// focus; the keys held now for an event it did not see. Qt's modifiers
    /// cannot tell that the other Ctrl key is still down when one is
    /// released. Qt delivers its key events after it has read every event
    /// that came with them, so the keys held now can be those of a later event.
    std::vector<std::uint32_t> heldKeysAfter(std::uint32_t time, std::uint32_t keycode, bool pressed) const;
    /// The unshifted keysym in the active layout group, or 0 (XKB_KEY_NoSymbol)
    /// without a keymap. Remapped keys (caps:escape) report what they became.
    std::uint32_t keysym(std::uint32_t keycode) const;
    /// The modifiers that stateWith() puts down.
    struct HeldModifiers
    {
        bool shift = false;
        bool control = false;
        bool alt = false;
        bool super = false;
    };
    /// The keyboard as it is now, with @p held in place of the Shift, Control,
    /// Alt (Mod1) and Super (Mod4) that are down, latched or locked. The
    /// layout, Caps Lock, Num Lock and the level keys (AltGr) stay. Null
    /// without a keymap. The state is valid until the next call or the next
    /// keymap.
    xkb_state* stateWith(HeldModifiers held) const;
    bool capsLock() const;
    bool numLock() const;

    /// For tests: the layouts of xkeyboard-config (such as "us,ru") with
    /// @p group active, in place of the compositor's keymap, and the keys
    /// held. A null @p layouts puts the compositor's keymap back.
    bool useLayoutsForTest(const char* layouts, std::uint32_t group);
    void setHeldKeysForTest(std::vector<std::uint32_t> keys)
    {
        m_held = std::move(keys);
        m_recent.clear();
    }

    XkbKeyboard(const XkbKeyboard&) = delete;
    XkbKeyboard& operator=(const XkbKeyboard&) = delete;

private:
    XkbKeyboard();
    char32_t codepoint(std::uint32_t keycode, std::uint32_t layout, std::uint32_t level) const;
    ~XkbKeyboard();
    /// Takes @p keymap (a reference) as the active keymap.
    void install(xkb_keymap* keymap);

    static void onKeymap(void* data, wl_keyboard*, std::uint32_t format, int fd, std::uint32_t size);
    static void onModifiers(void* data, wl_keyboard*, std::uint32_t serial, std::uint32_t depressed,
                            std::uint32_t latched, std::uint32_t locked, std::uint32_t group);
    static void onEnter(void* data, wl_keyboard*, std::uint32_t serial, wl_surface*, wl_array* keys);
    static void onLeave(void* data, wl_keyboard*, std::uint32_t serial, wl_surface*);
    static void onKey(void* data, wl_keyboard*, std::uint32_t serial, std::uint32_t time, std::uint32_t key,
                      std::uint32_t state);

    wl_keyboard* m_keyboard = nullptr;
    xkb_context* m_context = nullptr;
    xkb_keymap* m_keymap = nullptr;
    xkb_state* m_state = nullptr;
    /// stateWith()'s answer, kept from one call to the next.
    mutable xkb_state* m_scratch = nullptr;
    /// The US layout of baseLayoutCodepoint(), made at its first call.
    mutable xkb_keymap* m_baseKeymap = nullptr;
    mutable bool m_baseKeymapTried = false;
    /// The compositor's keymap while a test's keymap is in use.
    xkb_keymap* m_compositorKeymap = nullptr;
    bool m_testKeymap = false;
    std::vector<std::uint32_t> m_held;
    /// The last key events, with the keys held right after each.
    struct KeyRecord
    {
        std::uint32_t time = 0;
        std::uint32_t keycode = 0;
        bool pressed = false;
        std::vector<std::uint32_t> held;
    };
    std::vector<KeyRecord> m_recent;
};

} // namespace Afterglow
