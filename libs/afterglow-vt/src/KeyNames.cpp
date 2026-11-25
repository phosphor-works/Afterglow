// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/KeyNames.h>

#include "Utf8.h"

#include <utility>

namespace AfterglowVt {

namespace {

/// @p word in lower case, without '_' and '-': "Page_Up" and "pageup" are
/// one name.
std::string normalized(std::string_view word)
{
    std::string out;
    out.reserve(word.size());
    for (const char c : word) {
        if (c == '_' || c == '-')
            continue;
        out += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    }
    return out;
}

std::optional<std::uint8_t> modifierNamed(const std::string& name)
{
    if (name == "ctrl" || name == "control")
        return Mod::Ctrl;
    if (name == "shift")
        return Mod::Shift;
    if (name == "alt" || name == "opt" || name == "option")
        return Mod::Alt;
    // Qt and the bindings call the Super key Meta.
    if (name == "super" || name == "meta" || name == "cmd" || name == "command")
        return Mod::Super;
    return std::nullopt;
}

std::optional<Key> keyNamed(const std::string& name)
{
    static constexpr std::pair<std::string_view, Key> Names[]{
        {"enter", Key::Enter},
        {"return", Key::Enter},
        {"tab", Key::Tab},
        {"backspace", Key::Backspace},
        {"escape", Key::Escape},
        {"esc", Key::Escape},
        {"up", Key::Up},
        {"down", Key::Down},
        {"left", Key::Left},
        {"right", Key::Right},
        {"home", Key::Home},
        {"end", Key::End},
        {"insert", Key::Insert},
        {"ins", Key::Insert},
        {"delete", Key::Delete},
        {"del", Key::Delete},
        {"pageup", Key::PageUp},
        {"pgup", Key::PageUp},
        {"pagedown", Key::PageDown},
        {"pgdn", Key::PageDown},
        {"pgdown", Key::PageDown},
        {"kp0", Key::Keypad0},
        {"kp1", Key::Keypad1},
        {"kp2", Key::Keypad2},
        {"kp3", Key::Keypad3},
        {"kp4", Key::Keypad4},
        {"kp5", Key::Keypad5},
        {"kp6", Key::Keypad6},
        {"kp7", Key::Keypad7},
        {"kp8", Key::Keypad8},
        {"kp9", Key::Keypad9},
        {"kpdecimal", Key::KeypadDecimal},
        {"kpdivide", Key::KeypadDivide},
        {"kpmultiply", Key::KeypadMultiply},
        {"kpsubtract", Key::KeypadSubtract},
        {"kpadd", Key::KeypadAdd},
        {"kpenter", Key::KeypadEnter},
        {"kpequal", Key::KeypadEqual},
        {"kpseparator", Key::KeypadSeparator},
        {"kpleft", Key::KeypadLeft},
        {"kpright", Key::KeypadRight},
        {"kpup", Key::KeypadUp},
        {"kpdown", Key::KeypadDown},
        {"kppageup", Key::KeypadPageUp},
        {"kppagedown", Key::KeypadPageDown},
        {"kphome", Key::KeypadHome},
        {"kpend", Key::KeypadEnd},
        {"kpinsert", Key::KeypadInsert},
        {"kpdelete", Key::KeypadDelete},
        {"kpbegin", Key::KeypadBegin},
        {"capslock", Key::CapsLock},
        {"scrolllock", Key::ScrollLock},
        {"numlock", Key::NumLock},
        {"printscreen", Key::PrintScreen},
        {"print", Key::PrintScreen},
        {"pause", Key::Pause},
        {"menu", Key::Menu},
        {"leftshift", Key::LeftShift},
        {"leftcontrol", Key::LeftControl},
        {"leftalt", Key::LeftAlt},
        {"leftsuper", Key::LeftSuper},
        {"rightshift", Key::RightShift},
        {"rightcontrol", Key::RightControl},
        {"rightalt", Key::RightAlt},
        {"rightsuper", Key::RightSuper},
        {"isolevel3shift", Key::IsoLevel3Shift},
        {"isolevel5shift", Key::IsoLevel5Shift},
        {"lefthyper", Key::LeftHyper},
        {"leftmeta", Key::LeftMeta},
        {"righthyper", Key::RightHyper},
        {"rightmeta", Key::RightMeta},
        {"mediaplay", Key::MediaPlay},
        {"mediapause", Key::MediaPause},
        {"mediaplaypause", Key::MediaPlayPause},
        {"mediareverse", Key::MediaReverse},
        {"mediastop", Key::MediaStop},
        {"mediafastforward", Key::MediaFastForward},
        {"mediarewind", Key::MediaRewind},
        {"mediatracknext", Key::MediaTrackNext},
        {"mediatrackprevious", Key::MediaTrackPrevious},
        {"mediarecord", Key::MediaRecord},
        {"lowervolume", Key::LowerVolume},
        {"raisevolume", Key::RaiseVolume},
        {"mutevolume", Key::MuteVolume},
    };
    for (const auto& [n, key] : Names) {
        if (name == n)
            return key;
    }
    // F1 to F35.
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'f') {
        int number = 0;
        for (std::size_t i = 1; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9')
                return std::nullopt;
            number = number * 10 + (name[i] - '0');
        }
        if (name[1] != '0' && number >= 1 && number <= 35)
            return Key(int(Key::F1) + number - 1);
    }
    return std::nullopt;
}

/// What a keypad key types with NumLock on, as a typed one does: its text
/// goes in the legacy encoding and in kitty's unless every key is reported.
/// Empty for another key.
std::string_view keypadText(Key key)
{
    if (key >= Key::Keypad0 && key <= Key::Keypad9) {
        static constexpr std::string_view Digits = "0123456789";
        return Digits.substr(std::size_t(int(key) - int(Key::Keypad0)), 1);
    }
    switch (key) {
    case Key::KeypadDecimal:
        return ".";
    case Key::KeypadDivide:
        return "/";
    case Key::KeypadMultiply:
        return "*";
    case Key::KeypadSubtract:
        return "-";
    case Key::KeypadAdd:
        return "+";
    case Key::KeypadEqual:
        return "=";
    case Key::KeypadSeparator:
        return ",";
    default:
        return {};
    }
}

/// The character of a character key named in words.
char32_t characterNamed(const std::string& name)
{
    if (name == "space")
        return U' ';
    if (name == "plus")
        return U'+';
    if (name == "minus")
        return U'-';
    return 0;
}

/// What Shift gives on the key of @p c in the US layout, else @p c.
char32_t usShifted(char32_t c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 'A';
    constexpr std::string_view Plain = "1234567890-=[]\\;',./`";
    constexpr std::string_view Shifted = "!@#$%^&*()_+{}|:\"<>?~";
    if (c < 0x80) {
        if (const auto at = Plain.find(char(c)); at != std::string_view::npos)
            return char32_t(Shifted[at]);
    }
    return c;
}

} // namespace

KeyEvent NamedKey::event(KeyAction action) const
{
    KeyEvent out;
    out.key = key;
    out.mods = mods;
    out.action = action;
    out.text = text;
    out.unshifted = unshifted;
    out.shifted = shifted;
    return out;
}

std::optional<NamedKey> parseKeyName(std::string_view name)
{
    if (name.empty())
        return std::nullopt;
    NamedKey out;
    // The key is the last word. A '+' or a '-' at the end is a key of its
    // own: "+" alone, or after the '+' that ends the modifiers ("ctrl++").
    std::string_view keyWord;
    std::string_view modifiers;
    if (name == "+") {
        keyWord = name;
    } else if (name.ends_with("++")) {
        keyWord = name.substr(name.size() - 1);
        modifiers = name.substr(0, name.size() - 2);
        // "++": a '+' that ends no modifier.
        if (modifiers.empty())
            return std::nullopt;
    } else {
        const std::size_t plus = name.rfind('+');
        keyWord = plus == std::string_view::npos ? name : name.substr(plus + 1);
        modifiers = plus == std::string_view::npos ? std::string_view() : name.substr(0, plus);
        // "+c": a '+' with no modifier before it.
        if (plus == 0)
            return std::nullopt;
    }
    if (keyWord.empty())
        return std::nullopt;
    while (!modifiers.empty()) {
        const std::size_t plus = modifiers.find('+');
        const std::string word = normalized(modifiers.substr(0, plus));
        const auto mod = modifierNamed(word);
        if (!mod)
            return std::nullopt;
        out.mods |= *mod;
        if (plus == std::string_view::npos)
            break;
        modifiers.remove_prefix(plus + 1);
        // "ctrl+" before a key: an empty word between two '+' is a typo.
        if (modifiers.empty())
            return std::nullopt;
    }

    char32_t character = 0;
    std::size_t at = 0;
    if (detail::nextUtf8(keyWord, at, character) && at == keyWord.size()) {
        // One character: itself, a letter in either case.
        if (detail::isControl(character))
            return std::nullopt;
        if (character >= 'A' && character <= 'Z')
            character = character - 'A' + 'a';
    } else {
        const std::string word = normalized(keyWord);
        character = characterNamed(word);
        if (character == 0) {
            const auto key = keyNamed(word);
            if (!key)
                return std::nullopt;
            out.key = *key;
            out.text = keypadText(*key);
            return out;
        }
    }
    out.unshifted = character;
    const char32_t typed = (out.mods & Mod::Shift) ? usShifted(character) : character;
    if (out.mods & Mod::Shift)
        out.shifted = typed;
    // With Ctrl the key types a control character: the encoders take the key.
    if (!(out.mods & Mod::Ctrl))
        detail::appendUtf8(out.text, typed);
    return out;
}

std::string encodeNamedKey(const NamedKey& key, const Modes& modes)
{
    return encodeKey(key.event(KeyAction::Press), modes) + encodeKey(key.event(KeyAction::Release), modes);
}

} // namespace AfterglowVt
