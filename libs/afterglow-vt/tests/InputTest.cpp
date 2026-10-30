// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Input.h>
#include <AfterglowVt/Terminal.h>

#include <gtest/gtest.h>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

using namespace AfterglowVt;

namespace {
KeyEvent key(Key k, std::uint8_t mods = 0, std::string_view text = {}, char32_t unshifted = 0,
             KeyAction action = KeyAction::Press)
{
    KeyEvent e;
    e.key = k;
    e.mods = mods;
    e.text = text;
    e.unshifted = unshifted;
    e.action = action;
    return e;
}
} // namespace

TEST(Input, CursorKeys)
{
    Modes m;
    EXPECT_EQ(encodeKey(key(Key::Up), m), "\x1b[A");
    m.cursorKeysApp = true;
    EXPECT_EQ(encodeKey(key(Key::Up), m), "\x1bOA");
    EXPECT_EQ(encodeKey(key(Key::Left, Mod::Ctrl), m), "\x1b[1;5D");
}

TEST(Input, TildeAndFunctionKeys)
{
    Modes m;
    EXPECT_EQ(encodeKey(key(Key::Delete), m), "\x1b[3~");
    EXPECT_EQ(encodeKey(key(Key::PageUp, Mod::Shift), m), "\x1b[5;2~");
    EXPECT_EQ(encodeKey(key(Key::F1), m), "\x1bOP");
    EXPECT_EQ(encodeKey(key(Key::F5, Mod::Alt), m), "\x1b[15;3~");
}

TEST(Input, ControlAndAltText)
{
    Modes m;
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Ctrl, "c", U'c'), m), "\x03");
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Ctrl, "", U'['), m), "\x1b");
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Alt, "x", U'x'), m), "\x1bx");
    EXPECT_EQ(encodeKey(key(Key::Text, 0, "\xC3\xA9", 0), m), "\xC3\xA9");
    EXPECT_EQ(encodeKey(key(Key::Backspace), m), "\x7f");
    EXPECT_EQ(encodeKey(key(Key::Tab, Mod::Shift), m), "\x1b[Z");
}

TEST(Input, MouseSgr)
{
    Modes m;
    MouseEvent e{MouseButton::Left, MouseAction::Press, 0, 4, 2};
    EXPECT_EQ(encodeMouse(e, m), "");
    m.mouseTracking = MouseTracking::Normal;
    m.mouseFormat = MouseFormat::Sgr;
    EXPECT_EQ(encodeMouse(e, m), "\x1b[<0;5;3M");
    e.action = MouseAction::Release;
    EXPECT_EQ(encodeMouse(e, m), "\x1b[<0;5;3m");
    e.action = MouseAction::Motion;
    EXPECT_EQ(encodeMouse(e, m), "");
    m.mouseTracking = MouseTracking::Any;
    e.button = MouseButton::None;
    EXPECT_EQ(encodeMouse(e, m), "\x1b[<35;5;3M");
}

TEST(Input, PasteIsBracketedAndSanitized)
{
    Modes m;
    EXPECT_EQ(encodePaste("a\nb", m), "a\rb");
    m.bracketedPaste = true;
    EXPECT_EQ(encodePaste("x\x1b[201~y", m), "\x1b[200~xy\x1b[201~");
}

TEST(Input, ReleasesAndModifierKeysAreSilentInLegacyMode)
{
    Modes m;
    EXPECT_EQ(encodeKey(key(Key::Text, 0, "a", U'a', KeyAction::Release), m), "");
    EXPECT_EQ(encodeKey(key(Key::LeftShift, Mod::Shift), m), "");
    EXPECT_EQ(encodeKey(key(Key::Up, Mod::CapsLock), m), "\x1b[A"); // locks never leak into legacy
}

TEST(Input, KittyDisambiguate)
{
    Modes m;
    m.kittyKeyboard = KittyFlag::Disambiguate;
    EXPECT_EQ(encodeKey(key(Key::Text, 0, "a", U'a'), m), "a");
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Shift, "A", U'a'), m), "A");
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Ctrl, "", U'a'), m), "\x1b[97;5u");
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Alt, "x", U'x'), m), "\x1b[120;3u");
    EXPECT_EQ(encodeKey(key(Key::Escape), m), "\x1b[27u");
    EXPECT_EQ(encodeKey(key(Key::Enter), m), "\r");
    EXPECT_EQ(encodeKey(key(Key::Enter, Mod::Shift), m), "\x1b[13;2u");
    EXPECT_EQ(encodeKey(key(Key::Up), m), "\x1b[A");
    EXPECT_EQ(encodeKey(key(Key::Up, Mod::Ctrl), m), "\x1b[1;5A");
    EXPECT_EQ(encodeKey(key(Key::F3), m), "\x1b[13~");
    EXPECT_EQ(encodeKey(key(Key::KeypadEnter), m), "\x1b[57414u");
    EXPECT_EQ(encodeKey(key(Key::Keypad5, 0, "5"), m), "5");
    EXPECT_EQ(encodeKey(key(Key::Text, 0, "a", U'a', KeyAction::Release), m), "");
}

TEST(Input, KittyEventTypes)
{
    Modes m;
    m.kittyKeyboard = KittyFlag::Disambiguate | KittyFlag::ReportEvents;
    EXPECT_EQ(encodeKey(key(Key::Escape, 0, {}, 0, KeyAction::Release), m), "\x1b[27;1:3u");
    EXPECT_EQ(encodeKey(key(Key::Up, 0, {}, 0, KeyAction::Repeat), m), "\x1b[1;1:2A");
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Ctrl, "", U'c', KeyAction::Release), m), "\x1b[99;5:3u");
    // plain text has no release event without "report all keys"
    EXPECT_EQ(encodeKey(key(Key::Text, 0, "a", U'a', KeyAction::Release), m), "");
}

TEST(Input, KittyReportAllKeysWithText)
{
    Modes m;
    m.kittyKeyboard = KittyFlag::Disambiguate | KittyFlag::ReportAllKeys | KittyFlag::ReportText
                      | KittyFlag::ReportAlternates;
    EXPECT_EQ(encodeKey(key(Key::Text, 0, "a", U'a'), m), "\x1b[97;1;97u");
    KeyEvent shifted = key(Key::Text, Mod::Shift, "A", U'a');
    shifted.shifted = U'A';
    EXPECT_EQ(encodeKey(shifted, m), "\x1b[97:65;2;65u");
    EXPECT_EQ(encodeKey(key(Key::Enter), m), "\x1b[13u");
    EXPECT_EQ(encodeKey(key(Key::LeftShift, Mod::Shift), m), "\x1b[57441;2u");
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::CapsLock, "A", U'a'), m), "\x1b[97;65;65u");
}

TEST(Input, ModifyOtherKeys)
{
    Modes m;
    // Off: legacy bytes.
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Ctrl, "1", U'1'), m), "1");
    m.modifyOtherKeys = 1;
    // Level 1: Ctrl+a still has its control code; Ctrl+1 and Ctrl+; get sequences.
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Ctrl, "a", U'a'), m), "\x01");
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Ctrl, "1", U'1'), m), "\x1b[27;5;49~");
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Ctrl, ";", U';'), m), "\x1b[27;5;59~");
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Alt, "x", U'x'), m), "\x1bx");
    m.modifyOtherKeys = 2;
    // Level 2: every modified key except Shift with a non-letter.
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Ctrl, "a", U'a'), m), "\x1b[27;5;97~");
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Alt, "x", U'x'), m), "\x1b[27;3;120~");
    KeyEvent shiftA = key(Key::Text, Mod::Shift, "A", U'a');
    shiftA.shifted = U'A';
    EXPECT_EQ(encodeKey(shiftA, m), "\x1b[27;2;65~");
    KeyEvent shiftComma = key(Key::Text, Mod::Shift, "<", U',');
    shiftComma.shifted = U'<';
    EXPECT_EQ(encodeKey(shiftComma, m), "<");
    EXPECT_EQ(encodeKey(key(Key::Text, 0, "a", U'a'), m), "a");
}

TEST(Input, NestedPasteTerminatorsCannotReappearAfterRemoval)
{
    Modes modes;
    modes.bracketedPaste = true;
    EXPECT_EQ(encodePaste("a\x1b[20\x1b[201~1~b", modes), "\x1b[200~ab\x1b[201~");
    std::string nested;
    for (int i = 0; i < 10000; ++i) nested += "\x1b[20";
    for (int i = 0; i < 10000; ++i) nested += "1~";
    EXPECT_EQ(encodePaste(nested, modes), "\x1b[200~\x1b[201~");
}
