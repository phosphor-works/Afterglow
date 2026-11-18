// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "XkbKeyboard.h"

#include <QGuiApplication>
#include <QLoggingCategory>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcXkb, "afterglow.xkb", QtInfoMsg)

namespace Afterglow {

namespace {

void onRepeatInfo(void*, wl_keyboard*, std::int32_t, std::int32_t) { }

} // namespace

XkbKeyboard& XkbKeyboard::instance()
{
    static XkbKeyboard keyboard;
    return keyboard;
}

XkbKeyboard::XkbKeyboard()
{
    // kitty's base layout keys come from the US layout whatever the
    // compositor's: compiled meanwhile, as the first key would wait for it.
    m_baseKeymapReady = std::async(std::launch::async, [] {
        xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!context) return static_cast<xkb_keymap*>(nullptr);
        const xkb_rule_names names{"evdev", "pc105", "us", "", ""};
        xkb_keymap* keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        xkb_context_unref(context); // the keymap keeps its own reference
        return keymap;
    });
    // Tests and tools call this without a QGuiApplication.
    auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    m_wayland = gui ? gui->nativeInterface<QNativeInterface::QWaylandApplication>() : nullptr;
    if (!m_wayland || !m_wayland->seat()) {
        qCWarning(lcXkb) << "no Wayland seat; kitty keyboard reports use Qt's key codes";
        m_wayland = nullptr;
        return;
    }
    if (!m_wayland->keyboard()) qCWarning(lcXkb) << "the Wayland seat has no keyboard; kitty keyboard reports use Qt's key codes";
    // The seat, its capabilities and the keymap arrive through Qt's own
    // event dispatch shortly after; a roundtrip here would race Qt's Wayland
    // event thread.
    static const wl_registry_listener registryListener = {&XkbKeyboard::onGlobal, &XkbKeyboard::onGlobalRemove};
    m_registry = wl_display_get_registry(m_wayland->display());
    if (m_registry) wl_registry_add_listener(m_registry, &registryListener, this);
}

void XkbKeyboard::onGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version)
{
    auto* self = static_cast<XkbKeyboard*>(data);
    if (self->m_seat || std::strcmp(interface, wl_seat_interface.name) != 0) return;
    // The version of Qt's seat: our keyboard gets the events that Qt's gets
    // (repeated keys from version 10).
    const std::uint32_t qt = wl_proxy_get_version(reinterpret_cast<wl_proxy*>(self->m_wayland->seat()));
    const std::uint32_t bound = std::min({version, qt, std::uint32_t(wl_seat_interface.version)});
    self->m_seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, bound));
    if (!self->m_seat) return;
    self->m_seatName = name;
    static const wl_seat_listener seatListener = {&XkbKeyboard::onCapabilities, [](void*, wl_seat*, const char*) {}};
    wl_seat_add_listener(self->m_seat, &seatListener, self);
}

void XkbKeyboard::onGlobalRemove(void* data, wl_registry*, std::uint32_t name)
{
    auto* self = static_cast<XkbKeyboard*>(data);
    if (!self->m_seat || name != self->m_seatName) return;
    self->setKeyboardCapability(false);
    if (wl_seat_get_version(self->m_seat) >= WL_SEAT_RELEASE_SINCE_VERSION) wl_seat_release(self->m_seat);
    else wl_seat_destroy(self->m_seat);
    self->m_seat = nullptr;
}

void XkbKeyboard::onCapabilities(void* data, wl_seat*, std::uint32_t capabilities)
{
    static_cast<XkbKeyboard*>(data)->setKeyboardCapability(capabilities & WL_SEAT_CAPABILITY_KEYBOARD);
}

void XkbKeyboard::setKeyboardCapability(bool keyboard)
{
    // A keyboard that goes and comes back is another: the old one gets no
    // events any more. Each change of the capability comes as an event.
    if (keyboard == (m_keyboard != nullptr)) return;
    if (m_keyboard) {
        if (wl_keyboard_get_version(m_keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION) wl_keyboard_release(m_keyboard);
        else wl_keyboard_destroy(m_keyboard);
        m_keyboard = nullptr;
        m_held.clear();
        return;
    }
    if (!m_context && !(m_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) return;
    m_keyboard = wl_seat_get_keyboard(m_seat);
    static const wl_keyboard_listener listener = {
        &XkbKeyboard::onKeymap, &XkbKeyboard::onEnter, &XkbKeyboard::onLeave, &XkbKeyboard::onKey,
        &XkbKeyboard::onModifiers, onRepeatInfo,
    };
    wl_keyboard_add_listener(m_keyboard, &listener, this);
}

XkbKeyboard::~XkbKeyboard()
{
    // The Wayland connection is torn down with QGuiApplication, after which
    // releasing the proxy would touch freed memory; only the xkb side is ours.
    if (m_baseKeymapReady.valid()) m_baseKeymap = m_baseKeymapReady.get();
    if (m_scratch) {
        xkb_state_unref(m_scratch);
    }
    if (m_eventState) {
        xkb_state_unref(m_eventState);
    }
    if (m_state) {
        xkb_state_unref(m_state);
    }
    if (m_keymap) {
        xkb_keymap_unref(m_keymap);
    }
    if (m_compositorKeymap) {
        xkb_keymap_unref(m_compositorKeymap);
    }
    if (m_baseKeymap) {
        xkb_keymap_unref(m_baseKeymap);
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
    if (self->m_testKeymap) {
        // A test's keymap is in use: it stays until the test puts this back.
        if (self->m_compositorKeymap) xkb_keymap_unref(self->m_compositorKeymap);
        self->m_compositorKeymap = keymap;
        return;
    }
    self->install(keymap);
    qCInfo(lcXkb) << "keymap loaded," << xkb_keymap_num_layouts(keymap) << "layout(s)";
}

void XkbKeyboard::install(xkb_keymap* keymap)
{
    if (m_scratch) {
        xkb_state_unref(m_scratch);
        m_scratch = nullptr;
    }
    if (m_eventState) {
        xkb_state_unref(m_eventState);
        m_eventState = nullptr;
    }
    // Their states are of the keymap that goes.
    m_recent.clear();
    if (m_state) {
        xkb_state_unref(m_state);
    }
    if (m_keymap) {
        xkb_keymap_unref(m_keymap);
    }
    m_keymap = keymap;
    m_state = keymap ? xkb_state_new(keymap) : nullptr;
}

void XkbKeyboard::onEnter(void* data, wl_keyboard*, std::uint32_t, wl_surface*, wl_array* keys)
{
    auto* self = static_cast<XkbKeyboard*>(data);
    self->m_held.clear();
    const auto* key = static_cast<const std::uint32_t*>(keys->data);
    for (std::size_t i = 0; i < keys->size / sizeof(std::uint32_t); ++i) {
        self->m_held.push_back(key[i] + 8); // evdev code to xkb keycode
    }
}

void XkbKeyboard::onLeave(void* data, wl_keyboard*, std::uint32_t, wl_surface*)
{
    // The records stay: Qt delivers the key events that came in the same
    // read as the leave after it, and they look up their own.
    static_cast<XkbKeyboard*>(data)->m_held.clear();
}

void XkbKeyboard::onKey(void* data, wl_keyboard*, std::uint32_t, std::uint32_t time, std::uint32_t key,
                        std::uint32_t state)
{
    auto* self = static_cast<XkbKeyboard*>(data);
    const std::uint32_t keycode = key + 8;
    // Pressed, or repeated (version 10): down either way.
    const bool pressed = state != WL_KEYBOARD_KEY_STATE_RELEASED;
    std::erase(self->m_held, keycode);
    if (pressed) self->m_held.push_back(keycode);
    // Enough for the events of one read, which Qt delivers after it.
    constexpr std::size_t Kept = 32;
    if (self->m_recent.size() == Kept) self->m_recent.erase(self->m_recent.begin());
    // The state before the key's own modifiers event, which the compositor
    // sends after it: the one that the key's layout and levels come from.
    KeyRecord record{time, keycode, pressed, self->m_held, self->m_state != nullptr, {}};
    if (self->m_state) record.masks = masksOf(self->m_state);
    self->m_recent.push_back(std::move(record));
}

const XkbKeyboard::KeyRecord* XkbKeyboard::recordOf(const Event& event) const
{
    // The oldest match: Qt delivers the events in their order, and two of
    // the same key and state within one millisecond do not come from hands.
    for (const KeyRecord& record : m_recent) {
        if (record.time == event.time && record.keycode == event.keycode && record.pressed == event.pressed) return &record;
    }
    return nullptr;
}

std::vector<std::uint32_t> XkbKeyboard::heldKeysAfter(std::uint32_t time, std::uint32_t keycode, bool pressed) const
{
    const KeyRecord* record = recordOf({time, keycode, pressed});
    return record ? record->held : m_held;
}

XkbKeyboard::Masks XkbKeyboard::masksOf(xkb_state* state)
{
    return {xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED), xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED),
            xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED), xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_DEPRESSED),
            xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_LATCHED), xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_LOCKED)};
}

void XkbKeyboard::applyMasks(xkb_state* state, const Masks& m)
{
    xkb_state_update_mask(state, m.depressed, m.latched, m.locked, m.depressedLayout, m.latchedLayout, m.lockedLayout);
}

xkb_state* XkbKeyboard::stateAt(const Event& event) const
{
    if (!m_state) return nullptr;
    const KeyRecord* record = recordOf(event);
    if (!record || !record->hasMasks) return m_state;
    if (!m_eventState && !(m_eventState = xkb_state_new(m_keymap))) return m_state;
    applyMasks(m_eventState, record->masks);
    return m_eventState;
}

bool XkbKeyboard::useLayoutsForTest(const char* layouts, std::uint32_t group)
{
    if (!layouts) {
        if (m_testKeymap) {
            install(m_compositorKeymap);
            m_compositorKeymap = nullptr;
            m_testKeymap = false;
            if (m_state) applyMasks(m_state, m_compositorMasks);
        }
        return true;
    }
    if (!m_context && !(m_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) return false;
    const xkb_rule_names names{"evdev", "pc105", layouts, "", ""};
    xkb_keymap* keymap = xkb_keymap_new_from_names(m_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) return false;
    if (!m_testKeymap) {
        m_compositorKeymap = m_keymap ? xkb_keymap_ref(m_keymap) : nullptr;
        m_testKeymap = true;
    }
    install(keymap);
    xkb_state_update_mask(m_state, 0, 0, 0, 0, 0, group);
    return true;
}

char32_t XkbKeyboard::baseLayoutCodepoint(std::uint32_t keycode) const
{
    if (m_baseKeymapReady.valid()) {
        m_baseKeymap = m_baseKeymapReady.get();
        if (!m_baseKeymap) qCWarning(lcXkb) << "no US keymap from xkeyboard-config: no base layout keys";
    }
    const xkb_keysym_t* syms = nullptr;
    if (!m_baseKeymap || xkb_keymap_key_get_syms_by_level(m_baseKeymap, keycode, 0, 0, &syms) < 1) {
        return 0;
    }
    return char32_t(xkb_keysym_to_utf32(syms[0]));
}

void XkbKeyboard::onModifiers(void* data, wl_keyboard*, std::uint32_t, std::uint32_t depressed,
                              std::uint32_t latched, std::uint32_t locked, std::uint32_t group)
{
    auto* self = static_cast<XkbKeyboard*>(data);
    // wl_keyboard sends the effective layout as the locked one.
    self->m_compositorMasks = {depressed, latched, locked, 0, 0, group};
    // A test's keymap has modifiers of its own: the compositor's get theirs
    // back with its keymap (useLayoutsForTest).
    if (self->m_state && !self->m_testKeymap) applyMasks(self->m_state, self->m_compositorMasks);
}

char32_t XkbKeyboard::codepoint(std::uint32_t keycode, std::uint32_t layout, std::uint32_t level) const
{
    const xkb_keysym_t* syms = nullptr;
    if (!m_keymap || xkb_keymap_key_get_syms_by_level(m_keymap, keycode, layout, level, &syms) < 1) {
        return 0;
    }
    return char32_t(xkb_keysym_to_utf32(syms[0]));
}

char32_t XkbKeyboard::unshiftedCodepoint(const Event& event) const
{
    xkb_state* state = stateAt(event);
    if (!state) {
        return 0;
    }
    const xkb_layout_index_t group = xkb_state_key_get_layout(state, event.keycode);
    return group == XKB_LAYOUT_INVALID ? 0 : codepoint(event.keycode, group, 0);
}

char32_t XkbKeyboard::shiftedCodepoint(const Event& event) const
{
    xkb_state* state = stateAt(event);
    if (!state) {
        return 0;
    }
    const xkb_layout_index_t group = xkb_state_key_get_layout(state, event.keycode);
    return group == XKB_LAYOUT_INVALID ? 0 : codepoint(event.keycode, group, 1);
}

std::pair<char32_t, char32_t> XkbKeyboard::latinCodepoints(std::uint32_t keycode) const
{
    if (!m_keymap) {
        return {0, 0};
    }
    const xkb_layout_index_t layouts = xkb_keymap_num_layouts_for_key(m_keymap, keycode);
    for (xkb_layout_index_t layout = 0; layout < layouts; ++layout) {
        const char32_t base = codepoint(keycode, layout, 0);
        if (base >= 0x20 && base < 0x7F) return {base, codepoint(keycode, layout, 1)};
    }
    return {0, 0};
}

std::uint32_t XkbKeyboard::keysym(std::uint32_t keycode) const
{
    return keysymIn(m_state, keycode);
}

std::uint32_t XkbKeyboard::keysym(const Event& event) const
{
    return keysymIn(stateAt(event), event.keycode);
}

std::uint32_t XkbKeyboard::keysymIn(xkb_state* state, std::uint32_t keycode) const
{
    if (!state) {
        return XKB_KEY_NoSymbol;
    }
    const xkb_layout_index_t group = xkb_state_key_get_layout(state, keycode);
    const xkb_keysym_t* syms = nullptr;
    if (group == XKB_LAYOUT_INVALID || xkb_keymap_key_get_syms_by_level(m_keymap, keycode, group, 0, &syms) < 1) {
        return XKB_KEY_NoSymbol;
    }
    return syms[0];
}

xkb_state* XkbKeyboard::stateWith(HeldModifiers held) const
{
    if (!m_state) {
        return nullptr;
    }
    if (!m_scratch && !(m_scratch = xkb_state_new(m_keymap))) {
        return nullptr;
    }
    xkb_mod_mask_t four = 0;
    xkb_mod_mask_t down = 0;
    const std::pair<const char*, bool> mods[] = {{XKB_MOD_NAME_SHIFT, held.shift},
                                                 {XKB_MOD_NAME_CTRL, held.control},
                                                 {XKB_MOD_NAME_ALT, held.alt},
                                                 {XKB_MOD_NAME_LOGO, held.super}};
    for (const auto& [name, on] : mods) {
        const xkb_mod_index_t index = xkb_keymap_mod_get_index(m_keymap, name);
        if (index == XKB_MOD_INVALID) continue;
        four |= xkb_mod_mask_t(1) << index;
        if (on) down |= xkb_mod_mask_t(1) << index;
    }
    xkb_state_update_mask(m_scratch, (xkb_state_serialize_mods(m_state, XKB_STATE_MODS_DEPRESSED) & ~four) | down,
                          xkb_state_serialize_mods(m_state, XKB_STATE_MODS_LATCHED) & ~four,
                          xkb_state_serialize_mods(m_state, XKB_STATE_MODS_LOCKED) & ~four, 0, 0,
                          xkb_state_serialize_layout(m_state, XKB_STATE_LAYOUT_EFFECTIVE));
    return m_scratch;
}

bool XkbKeyboard::capsLock(const Event& event) const
{
    xkb_state* state = stateAt(event);
    return state && xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0;
}

bool XkbKeyboard::numLock(const Event& event) const
{
    xkb_state* state = stateAt(event);
    return state && xkb_state_mod_name_is_active(state, XKB_MOD_NAME_NUM, XKB_STATE_MODS_LOCKED) > 0;
}

} // namespace Afterglow
