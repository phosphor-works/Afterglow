// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "XkbKeyboard.h"

#include <QGuiApplication>
#include <QLoggingCategory>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <sys/mman.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcXkb, "afterglow.xkb", QtInfoMsg)

namespace Afterglow {

namespace {

void onEnter(void*, wl_keyboard*, std::uint32_t, wl_surface*, wl_array*) { }
void onLeave(void*, wl_keyboard*, std::uint32_t, wl_surface*) { }
void onKey(void*, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) { }
void onRepeatInfo(void*, wl_keyboard*, std::int32_t, std::int32_t) { }

} // namespace

XkbKeyboard& XkbKeyboard::instance()
{
    static XkbKeyboard keyboard;
    return keyboard;
}

XkbKeyboard::XkbKeyboard()
{
    auto* wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!wayland || !wayland->seat()) {
        qCWarning(lcXkb) << "no Wayland seat; kitty keyboard reports use Qt's key codes";
        return;
    }
    m_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    m_keyboard = wl_seat_get_keyboard(wayland->seat());
    static const wl_keyboard_listener listener = {
        &XkbKeyboard::onKeymap, onEnter, onLeave, onKey, &XkbKeyboard::onModifiers, onRepeatInfo,
    };
    wl_keyboard_add_listener(m_keyboard, &listener, this);
    // The keymap arrives through Qt's own event dispatch shortly after; a
    // roundtrip here would race Qt's Wayland event thread.
}

XkbKeyboard::~XkbKeyboard()
{
    // The Wayland connection is torn down with QGuiApplication, after which
    // releasing the proxy would touch freed memory; only the xkb side is ours.
    if (m_state) {
        xkb_state_unref(m_state);
    }
    if (m_keymap) {
        xkb_keymap_unref(m_keymap);
    }
    if (m_context) {
        xkb_context_unref(m_context);
    }
}

void XkbKeyboard::onKeymap(void* data, wl_keyboard*, std::uint32_t format, int fd, std::uint32_t size)
{
    auto* self = static_cast<XkbKeyboard*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        return;
    }
    xkb_keymap* keymap = xkb_keymap_new_from_buffer(self->m_context, static_cast<const char*>(map), size - 1,
                                                    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!keymap) {
        qCWarning(lcXkb) << "failed to compile the compositor's keymap";
        return;
    }
    if (self->m_state) {
        xkb_state_unref(self->m_state);
    }
    if (self->m_keymap) {
        xkb_keymap_unref(self->m_keymap);
    }
    self->m_keymap = keymap;
    self->m_state = xkb_state_new(keymap);
    qCInfo(lcXkb) << "keymap loaded," << xkb_keymap_num_layouts(keymap) << "layout(s)";
}

void XkbKeyboard::onModifiers(void* data, wl_keyboard*, std::uint32_t, std::uint32_t depressed,
                              std::uint32_t latched, std::uint32_t locked, std::uint32_t group)
{
    auto* self = static_cast<XkbKeyboard*>(data);
    if (self->m_state) {
        xkb_state_update_mask(self->m_state, depressed, latched, locked, 0, 0, group);
    }
}

char32_t XkbKeyboard::unshiftedCodepoint(std::uint32_t keycode) const
{
    if (!m_state) {
        return 0;
    }
    const xkb_layout_index_t group = xkb_state_key_get_layout(m_state, keycode);
    if (group == XKB_LAYOUT_INVALID) {
        return 0;
    }
    const xkb_keysym_t* syms = nullptr;
    if (xkb_keymap_key_get_syms_by_level(m_keymap, keycode, group, 0, &syms) < 1) {
        return 0;
    }
    return char32_t(xkb_keysym_to_utf32(syms[0]));
}

bool XkbKeyboard::capsLock() const
{
    return m_state && xkb_state_mod_name_is_active(m_state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0;
}

bool XkbKeyboard::numLock() const
{
    return m_state && xkb_state_mod_name_is_active(m_state, XKB_MOD_NAME_NUM, XKB_STATE_MODS_LOCKED) > 0;
}

} // namespace Afterglow
