// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/KeyNames.h>
#include <AfterglowVt/Terminal.h>

#include <gtest/gtest.h>

using namespace AfterglowVt;

namespace {

/// The bytes of the key that @p name gives, or "?" for no key.
std::string bytes(std::string_view name, const Modes& modes = {})
{
    const auto key = parseKeyName(name);
    return key ? encodeNamedKey(*key, modes) : std::string("?");
}

} // namespace

// The names of the bindings and of kitty's send-key, in either case.
TEST(KeyNames, NamesOfTheBindingsAndOfKitty)
{
    EXPECT_EQ(bytes("ctrl+c"), "\x03");
    EXPECT_EQ(bytes("Ctrl+C"), "\x03");
    EXPECT_EQ(bytes("Return"), "\r");
    EXPECT_EQ(bytes("enter"), "\r");
    EXPECT_EQ(bytes("Escape"), "\x1b");
    EXPECT_EQ(bytes("esc"), "\x1b");
    EXPECT_EQ(bytes("Tab"), "\t");
    EXPECT_EQ(bytes("backspace"), "\x7f");
    EXPECT_EQ(bytes("F5"), "\x1b[15~");
    EXPECT_EQ(bytes("f12"), "\x1b[24~");
    EXPECT_EQ(bytes("page_up"), "\x1b[5~");
    EXPECT_EQ(bytes("PageDown"), "\x1b[6~");
    EXPECT_EQ(bytes("Up"), "\x1b[A");
    EXPECT_EQ(bytes("shift+up"), "\x1b[1;2A");
    EXPECT_EQ(bytes("ctrl+left"), "\x1b[1;5D");
    // Alt types the character after an escape; Shift gives the character
    // of the US layout.
    EXPECT_EQ(bytes("alt+x"), "\x1bx");
    EXPECT_EQ(bytes("shift+a"), "A");
    EXPECT_EQ(bytes("a"), "a");
    EXPECT_EQ(bytes("A"), "a");
    EXPECT_EQ(bytes("shift+1"), "!");
    EXPECT_EQ(bytes("space"), " ");
    EXPECT_EQ(bytes("ctrl+space"), std::string(1, '\0'));
    // Characters beyond ASCII type themselves.
    EXPECT_EQ(bytes("é"), "é");
    // The keys + and -, in words or at the end.
    EXPECT_EQ(bytes("+"), "+");
    EXPECT_EQ(bytes("plus"), "+");
    EXPECT_EQ(bytes("alt++"), "\x1b+");
    EXPECT_EQ(bytes("-"), "-");
    EXPECT_EQ(bytes("alt+minus"), "\x1b-");
    EXPECT_EQ(bytes("alt+-"), "\x1b-");
}

TEST(KeyNames, RefusesWhatNamesNoKey)
{
    for (const std::string_view name : {"", "ctrl+", "hyper+c", "ctrl++c", "ctrl+foo", "ab", "f0", "f36", "f05", "\x01", "ctrl+\x7f",
                                        "\xff", "+c", "shift+", "++"}) {
        EXPECT_FALSE(parseKeyName(name)) << name;
    }
}

// In the kitty keyboard protocol a key also reports its release when the
// program asked for events, and a key that types text only when it also
// asked for every key as an escape code.
TEST(KeyNames, FollowTheProgramsKeyboardProtocol)
{
    Modes kitty;
    kitty.kittyKeyboard = KittyFlag::Disambiguate;
    EXPECT_EQ(bytes("ctrl+c", kitty), "\x1b[99;5u");
    EXPECT_EQ(bytes("escape", kitty), "\x1b[27u");
    EXPECT_EQ(bytes("a", kitty), "a");
    kitty.kittyKeyboard = KittyFlag::Disambiguate | KittyFlag::ReportEvents | KittyFlag::ReportAllKeys;
    EXPECT_EQ(bytes("a", kitty), "\x1b[97u\x1b[97;1:3u");
    Modes application;
    application.cursorKeysApp = true;
    EXPECT_EQ(bytes("up", application), "\x1bOA");
}

// The keypad types its characters, as a typed keypad key does: in the legacy
// encoding, and in kitty's until every key is reported. kitty's other names.
TEST(KeyNames, KeypadKeysTypeTheirCharacters)
{
    EXPECT_EQ(bytes("kp5"), "5");
    EXPECT_EQ(bytes("KP_0"), "0");
    EXPECT_EQ(bytes("kp_decimal"), ".");
    EXPECT_EQ(bytes("kp_divide"), "/");
    EXPECT_EQ(bytes("kp_multiply"), "*");
    EXPECT_EQ(bytes("kp_subtract"), "-");
    EXPECT_EQ(bytes("kp_add"), "+");
    EXPECT_EQ(bytes("kp_equal"), "=");
    EXPECT_EQ(bytes("kp_separator"), ",");
    Modes kitty;
    kitty.kittyKeyboard = KittyFlag::Disambiguate;
    EXPECT_EQ(bytes("kp7", kitty), "7");
    kitty.kittyKeyboard = KittyFlag::Disambiguate | KittyFlag::ReportAllKeys;
    EXPECT_EQ(bytes("kp7", kitty), "\x1b[57406u");
    Modes application;
    application.keypadApp = true;
    EXPECT_EQ(bytes("kp1", application), "\x1bOq");
    for (const std::string_view name : {"media_reverse", "media_record", "iso_level3_shift", "iso_level5_shift"}) {
        EXPECT_TRUE(parseKeyName(name)) << name;
    }
    kitty.kittyKeyboard = KittyFlag::Disambiguate | KittyFlag::ReportAllKeys;
    EXPECT_EQ(bytes("media_record", kitty), "\x1b[57437u");
}
