// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <future>
#include <utility>
#include <vector>

struct wl_array;
struct wl_keyboard;
struct wl_registry;
struct wl_seat;
struct wl_surface;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace QNativeInterface {
struct QWaylandApplication;
}

namespace Afterglow {

/// The compositor's keyboard layout and lock state, and the keys held.
///
/// Qt reports the shifted symbol for punctuation and does not expose the
/// keymap or Caps/Num Lock, which the kitty keyboard protocol needs. This
/// binds a second wl_keyboard on Qt's own Wayland connection and follows the
/// events the compositor sends to it: the keymap, the modifiers and the
/// layout, the keys (with the state that each found) and the focus (enter,
/// with the keys held then, and leave). It binds again when the seat's
/// keyboard comes back after it went (instance()).
///
/// Qt delivers its key events after it has read every event that came with
/// them: the state now can be that of a later key. The lookups for a key
/// event take the state that the compositor's key event found (Event).
class XkbKeyboard
{
public:
    /// Also follows the seat: a keyboard that Qt bound anew (the seat lost
    /// the capability, and has it again) gets a second wl_keyboard too.
    static XkbKeyboard& instance();

    /// A key event as Qt delivers it: its time, its xkb keycode, and whether
    /// it is a press. The lookups take the state as it was at that key, or
    /// the state now for a key that the compositor did not send here (a test's).
    struct Event
    {
        std::uint32_t time = 0;
        std::uint32_t keycode = 0;
        bool pressed = true;
    };

    /// The unshifted character of the key in its layout group, or 0.
    char32_t unshiftedCodepoint(const Event& event) const;
    /// The character with Shift (shift level 1) in its layout group, or 0.
    char32_t shiftedCodepoint(const Event& event) const;
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
    /// keysym() in the layout group of @p event.
    std::uint32_t keysym(const Event& event) const;
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
    /// Caps Lock and Num Lock as they were at @p event.
    bool capsLock(const Event& event) const;
    bool numLock(const Event& event) const;

    /// For tests: the layouts of xkeyboard-config (such as "us,ru") with
    /// @p group active, in place of the compositor's keymap, and the keys
    /// held. A null @p layouts puts the compositor's keymap back, with the
    /// modifiers, locks and layout that the compositor sent meanwhile.
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
    /// Binds a wl_keyboard of our own while the seat has the keyboard
    /// capability (@p keyboard), and releases it without: asking a seat
    /// without it for one is a protocol error that ends the connection.
    void setKeyboardCapability(bool keyboard);
    /// The state of @p event, or m_state. Null without a keymap.
    xkb_state* stateAt(const Event& event) const;
    /// keysym() of @p keycode in @p state, which may be null.
    std::uint32_t keysymIn(xkb_state* state, std::uint32_t keycode) const;
    /// The modifiers and the layout of a state, as wl_keyboard sends them.
    struct Masks
    {
        std::uint32_t depressed = 0;
        std::uint32_t latched = 0;
        std::uint32_t locked = 0;
        std::uint32_t depressedLayout = 0;
        std::uint32_t latchedLayout = 0;
        std::uint32_t lockedLayout = 0;
    };
    static Masks masksOf(xkb_state* state);
    static void applyMasks(xkb_state* state, const Masks& masks);

    static void onKeymap(void* data, wl_keyboard*, std::uint32_t format, int fd, std::uint32_t size);
    static void onModifiers(void* data, wl_keyboard*, std::uint32_t serial, std::uint32_t depressed,
                            std::uint32_t latched, std::uint32_t locked, std::uint32_t group);
    static void onEnter(void* data, wl_keyboard*, std::uint32_t serial, wl_surface*, wl_array* keys);
    static void onLeave(void* data, wl_keyboard*, std::uint32_t serial, wl_surface*);
    static void onKey(void* data, wl_keyboard*, std::uint32_t serial, std::uint32_t time, std::uint32_t key,
                      std::uint32_t state);
    static void onGlobal(void* data, wl_registry*, std::uint32_t name, const char* interface, std::uint32_t version);
    static void onGlobalRemove(void* data, wl_registry*, std::uint32_t name);
    static void onCapabilities(void* data, wl_seat*, std::uint32_t capabilities);

    QNativeInterface::QWaylandApplication* m_wayland = nullptr;
    /// A seat of our own for its capability events, which Qt's seat gives
    /// only to Qt: the first, as Qt's default seat is. Neither Qt's keyboard
    /// proxy nor its ID tells a keyboard that came back, as both are reused.
    wl_registry* m_registry = nullptr;
    wl_seat* m_seat = nullptr;
    std::uint32_t m_seatName = 0;
    wl_keyboard* m_keyboard = nullptr;
    xkb_context* m_context = nullptr;
    xkb_keymap* m_keymap = nullptr;
    xkb_state* m_state = nullptr;
    /// stateWith()'s answer, kept from one call to the next.
    mutable xkb_state* m_scratch = nullptr;
    /// stateAt()'s answer, kept from one call to the next.
    mutable xkb_state* m_eventState = nullptr;
    /// The US layout of baseLayoutCodepoint(), which a thread compiles from
    /// the start, with an xkb context of its own: no key waits for it, and
    /// it needs no Wayland seat.
    mutable std::future<xkb_keymap*> m_baseKeymapReady;
    mutable xkb_keymap* m_baseKeymap = nullptr;
    /// The compositor's keymap while a test's keymap is in use.
    xkb_keymap* m_compositorKeymap = nullptr;
    bool m_testKeymap = false;
    /// The modifiers and the layout that the compositor sent last: a test's
    /// keymap does not take them, and the compositor's gets them back.
    Masks m_compositorMasks;
    std::vector<std::uint32_t> m_held;
    /// The last key events, with the keys held right after each.
    struct KeyRecord
    {
        std::uint32_t time = 0;
        std::uint32_t keycode = 0;
        bool pressed = false;
        std::vector<std::uint32_t> held;
        /// The state that the key found: the modifiers and the layout that
        /// the compositor sent before it.
        bool hasMasks = false;
        Masks masks;
    };
    /// The record of @p event, or null.
    const KeyRecord* recordOf(const Event& event) const;
    std::vector<KeyRecord> m_recent;
};

} // namespace Afterglow
