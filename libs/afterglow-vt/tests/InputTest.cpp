// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Input.h>
#include <AfterglowVt/Terminal.h>

#include <gtest/gtest.h>

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

MouseEvent mouse(MouseButton button, MouseAction action, int col, int row, std::uint8_t mods = 0)
{
    MouseEvent e;
    e.button = button;
    e.action = action;
    e.col = col;
    e.row = row;
    e.mods = mods;
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

TEST(Input, ApplicationKeypadCoversEveryKey)
{
    Modes modes;
    modes.keypadApp = true;
    const std::pair<Key, char> cases[] = {
        {Key::Keypad0, 'p'}, {Key::Keypad1, 'q'}, {Key::Keypad2, 'r'}, {Key::Keypad3, 's'},
        {Key::Keypad4, 't'}, {Key::Keypad5, 'u'}, {Key::Keypad6, 'v'}, {Key::Keypad7, 'w'},
        {Key::Keypad8, 'x'}, {Key::Keypad9, 'y'}, {Key::KeypadDecimal, 'n'}, {Key::KeypadDivide, 'o'},
        {Key::KeypadMultiply, 'j'}, {Key::KeypadSubtract, 'm'}, {Key::KeypadAdd, 'k'},
        {Key::KeypadEnter, 'M'}, {Key::KeypadEqual, 'X'}, {Key::KeypadSeparator, 'l'},
    };
    for (const auto& [code, final] : cases) {
        EXPECT_EQ(encodeKey(key(code), modes), std::string("\x1bO") + final);
    }
    EXPECT_EQ(encodeKey(key(Key::KeypadEqual, Mod::Ctrl, "="), modes), "\x1b[1;5X");
    modes.keypadApp = false;
    EXPECT_EQ(encodeKey(key(Key::KeypadEqual, 0, "="), modes), "=");
    EXPECT_EQ(encodeKey(key(Key::KeypadSeparator, 0, ","), modes), ",");
}

TEST(Input, TildeAndFunctionKeys)
{
    Modes m;
    EXPECT_EQ(encodeKey(key(Key::Delete), m), "\x1b[3~");
    EXPECT_EQ(encodeKey(key(Key::PageUp, Mod::Shift), m), "\x1b[5;2~");
    EXPECT_EQ(encodeKey(key(Key::F1), m), "\x1bOP");
    EXPECT_EQ(encodeKey(key(Key::F5, Mod::Alt), m), "\x1b[15;3~");
}

TEST(Input, FunctionKeysBeyondTwelve)
{
    Modes m;
    // xterm's terminfo: F13 to F24 are Shift+F1 to F12, F25 to F35 Ctrl+F1 to F11.
    EXPECT_EQ(encodeKey(key(Key::F13), m), "\x1b[1;2P");
    EXPECT_EQ(encodeKey(key(Key::F17), m), "\x1b[15;2~");
    EXPECT_EQ(encodeKey(key(Key::F24), m), "\x1b[24;2~");
    EXPECT_EQ(encodeKey(key(Key::F25), m), "\x1b[1;5P");
    EXPECT_EQ(encodeKey(key(Key::F35), m), "\x1b[23;5~");
    EXPECT_EQ(encodeKey(key(Key::F13, Mod::Ctrl), m), "\x1b[1;6P"); // modifiers add up
    // The kitty protocol has a code for each: 57376 for F13 to 57398 for F35.
    m.kittyKeyboard = KittyFlag::Disambiguate;
    EXPECT_EQ(encodeKey(key(Key::F13), m), "\x1b[57376u");
    EXPECT_EQ(encodeKey(key(Key::F35, Mod::Shift), m), "\x1b[57398;2u");
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
    MouseEvent e = mouse(MouseButton::Left, MouseAction::Press, 4, 2);
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

TEST(Input, MouseLegacyFormats)
{
    Modes m;
    m.mouseTracking = MouseTracking::Normal;
    m.mouseFormat = MouseFormat::X10;
    EXPECT_EQ(encodeMouse(mouse(MouseButton::Left, MouseAction::Press, 222, 0), m), "\x1b[M \xff!");
    EXPECT_EQ(encodeMouse(mouse(MouseButton::Left, MouseAction::Press, 223, 0), m), "");
    // A release keeps its modifiers (xterm's BtnCode): 3 + Ctrl (16), + 32.
    EXPECT_EQ(encodeMouse(mouse(MouseButton::Left, MouseAction::Release, 0, 0, Mod::Ctrl), m), "\x1b[M3!!");
    m.mouseFormat = MouseFormat::Utf8;
    EXPECT_EQ(encodeMouse(mouse(MouseButton::Left, MouseAction::Press, 2014, 0), m), "\x1b[M \xdf\xbf!");
    EXPECT_EQ(encodeMouse(mouse(MouseButton::Left, MouseAction::Press, 2015, 0), m), "");
    EXPECT_EQ(encodeMouse(mouse(MouseButton::Left, MouseAction::Release, 0, 0, Mod::Shift), m), "\x1b[M'!!");
    m.mouseFormat = MouseFormat::Urxvt;
    EXPECT_EQ(encodeMouse(mouse(MouseButton::Right, MouseAction::Press, 9, 4), m), "\x1b[34;10;5M");
    EXPECT_EQ(encodeMouse(mouse(MouseButton::Right, MouseAction::Release, 9, 4), m), "\x1b[35;10;5M");
    EXPECT_EQ(encodeMouse(mouse(MouseButton::Right, MouseAction::Release, 9, 4, Mod::Alt), m), "\x1b[43;10;5M");
    m.mouseFormat = MouseFormat::SgrPixels;
    MouseEvent pixels = mouse(MouseButton::Left, MouseAction::Press, 9, 4);
    pixels.pixelX = 99;
    pixels.pixelY = 41;
    EXPECT_EQ(encodeMouse(pixels, m), "\x1b[<0;100;42M");
}

TEST(Input, PasteFoldsNewlines)
{
    Modes m;
    EXPECT_EQ(encodePaste("a\nb", m), "a\rb");
    EXPECT_EQ(encodePaste("a\r\nb\rc", m), "a\rb\rc");
    m.bracketedPaste = true;
    EXPECT_EQ(encodePaste("a\r\nb", m), "\x1b[200~a\rb\x1b[201~");
}

TEST(Input, PasteDropsControlCharacters)
{
    Modes m;
    // C-o would run the line in bash without a newline for the prompt to see.
    EXPECT_EQ(encodePaste("echo pwned\x0f", m), "echo pwned");
    EXPECT_EQ(encodePaste("a\tb\x7f\x03\x1b[A", m), "a\tb[A");
    // C1 in UTF-8 (U+0085) goes, other text stays.
    EXPECT_EQ(encodePaste("x\xc2\x85y\xc3\xa9\xc2\xa0", m), "xy\xc3\xa9\xc2\xa0");
    // A dropped control does not join its neighbours into a C1 control.
    EXPECT_EQ(encodePaste("x\xc2\x01\x85y\xc2\x7f\x9b", m), "xy");
    m.bracketedPaste = true;
    // Without ESC the end marker cannot appear in the text.
    EXPECT_EQ(encodePaste("x\x1b[201~y", m), "\x1b[200~x[201~y\x1b[201~");
}

TEST(Input, FocusEventsOnlyWhenEnabled)
{
    Modes m;
    EXPECT_EQ(encodeFocus(true, m), "");
    m.focusEvents = true;
    EXPECT_EQ(encodeFocus(true, m), "\x1b[I");
    EXPECT_EQ(encodeFocus(false, m), "\x1b[O");
}

TEST(Input, LegacyMenuAndAltShiftTab)
{
    Modes m;
    EXPECT_EQ(encodeKey(key(Key::Menu), m), "\x1b[29~");
    EXPECT_EQ(encodeKey(key(Key::Menu, Mod::Ctrl), m), "\x1b[29;5~");
    EXPECT_EQ(encodeKey(key(Key::Tab, Mod::Shift | Mod::Alt), m), "\x1b\x1b[Z");
}

TEST(Input, Vt52KeypadAndFunctionKeys)
{
    Modes m;
    m.vt52 = true;
    EXPECT_EQ(encodeKey(key(Key::F1), m), "\x1bP");
    EXPECT_EQ(encodeKey(key(Key::F4), m), "\x1bS");
    m.keypadApp = true;
    EXPECT_EQ(encodeKey(key(Key::Keypad0, 0, "0"), m), "\x1b?p");
    EXPECT_EQ(encodeKey(key(Key::KeypadEnter), m), "\x1b?M");
}

TEST(Input, KeypadNavigationKeys)
{
    Modes m;
    // Legacy: the main keys' bytes.
    EXPECT_EQ(encodeKey(key(Key::KeypadLeft), m), "\x1b[D");
    EXPECT_EQ(encodeKey(key(Key::KeypadDelete), m), "\x1b[3~");
    EXPECT_EQ(encodeKey(key(Key::KeypadBegin), m), "\x1b[E");
    // Kitty: keys of their own.
    m.kittyKeyboard = KittyFlag::Disambiguate;
    EXPECT_EQ(encodeKey(key(Key::KeypadLeft), m), "\x1b[57417u");
    EXPECT_EQ(encodeKey(key(Key::KeypadDelete), m), "\x1b[57426u");
    EXPECT_EQ(encodeKey(key(Key::KeypadSeparator, 0, ","), m), ",");
    EXPECT_EQ(encodeKey(key(Key::KeypadBegin, Mod::Shift), m), "\x1b[1;2E");
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
    // Enter, Tab and Backspace have none either, even when modified.
    EXPECT_EQ(encodeKey(key(Key::Enter, Mod::Shift), m), "\x1b[13;2u");
    EXPECT_EQ(encodeKey(key(Key::Enter, Mod::Shift, {}, 0, KeyAction::Release), m), "");
    EXPECT_EQ(encodeKey(key(Key::Tab, Mod::Ctrl, {}, 0, KeyAction::Release), m), "");
    EXPECT_EQ(encodeKey(key(Key::Backspace, Mod::Alt, {}, 0, KeyAction::Release), m), "");
    m.kittyKeyboard |= KittyFlag::ReportAllKeys;
    EXPECT_EQ(encodeKey(key(Key::Enter, Mod::Shift, {}, 0, KeyAction::Release), m), "\x1b[13;2:3u");
}

TEST(Input, KittyUnmodifiedEnterAndBackspaceFollowLnmAndDecbkm)
{
    Modes m;
    m.kittyKeyboard = KittyFlag::Disambiguate;
    m.newline = true;
    EXPECT_EQ(encodeKey(key(Key::Enter), m), "\r\n");
    m.backarrowSendsBs = true;
    EXPECT_EQ(encodeKey(key(Key::Backspace), m), "\x08");
}

TEST(Input, KittyAssociatedTextHasNoControls)
{
    Modes m;
    m.kittyKeyboard = KittyFlag::Disambiguate | KittyFlag::ReportAllKeys | KittyFlag::ReportText;
    // U+0085 (NEL) is a C1 control: the event keeps its key code, without text.
    EXPECT_EQ(encodeKey(key(Key::Text, 0, "\xC2\x85", U'x'), m), "\x1b[120u");
    EXPECT_EQ(encodeKey(key(Key::Text, 0, "\xC3\xA9", 0xE9), m), "\x1b[233;1;233u");
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
    // A key with no unshifted character uses the code point of its text.
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Ctrl, "\xC3\xA9", 0), m), "\x1b[27;5;233~");
    m.modifyOtherKeys = 2;
    // Level 2: every modified key, except Shift alone on a printable key.
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Ctrl, "a", U'a'), m), "\x1b[27;5;97~");
    EXPECT_EQ(encodeKey(key(Key::Text, Mod::Alt, "x", U'x'), m), "\x1b[27;3;120~");
    KeyEvent shiftA = key(Key::Text, Mod::Shift, "A", U'a');
    shiftA.shifted = U'A';
    EXPECT_EQ(encodeKey(shiftA, m), "A");
    KeyEvent shiftComma = key(Key::Text, Mod::Shift, "<", U',');
    shiftComma.shifted = U'<';
    EXPECT_EQ(encodeKey(shiftComma, m), "<");
    KeyEvent shiftSpace = key(Key::Text, Mod::Shift, " ", U' ');
    shiftSpace.shifted = U' ';
    EXPECT_EQ(encodeKey(shiftSpace, m), "\x1b[27;2;32~");
    EXPECT_EQ(encodeKey(key(Key::Text, 0, "a", U'a'), m), "a");
}

TEST(Input, ModifyOtherKeysCoversReturnTabBackspaceAndEscape)
{
    Modes m;
    m.modifyOtherKeys = 1;
    EXPECT_EQ(encodeKey(key(Key::Enter, Mod::Shift), m), "\x1b[27;2;13~");
    EXPECT_EQ(encodeKey(key(Key::Enter, Mod::Ctrl), m), "\x1b[27;5;13~");
    EXPECT_EQ(encodeKey(key(Key::Enter, Mod::Alt), m), "\x1b\r"); // Alt keeps its ESC prefix
    EXPECT_EQ(encodeKey(key(Key::Tab, Mod::Ctrl), m), "\x1b[27;5;9~");
    EXPECT_EQ(encodeKey(key(Key::Tab, Mod::Shift), m), "\x1b[Z");
    EXPECT_EQ(encodeKey(key(Key::Backspace, Mod::Shift), m), "\x7f");
    EXPECT_EQ(encodeKey(key(Key::Escape, Mod::Ctrl), m), "\x1b");
    EXPECT_EQ(encodeKey(key(Key::Escape, Mod::Ctrl | Mod::Shift), m), "\x1b[27;6;27~");
    m.modifyOtherKeys = 2;
    EXPECT_EQ(encodeKey(key(Key::Enter, Mod::Alt), m), "\x1b[27;3;13~");
    EXPECT_EQ(encodeKey(key(Key::Tab, Mod::Shift), m), "\x1b[Z");
    EXPECT_EQ(encodeKey(key(Key::Tab, Mod::Ctrl | Mod::Shift), m), "\x1b[27;6;9~");
    EXPECT_EQ(encodeKey(key(Key::Backspace, Mod::Ctrl), m), "\x08");
    EXPECT_EQ(encodeKey(key(Key::Backspace, Mod::Shift), m), "\x1b[27;2;127~");
    EXPECT_EQ(encodeKey(key(Key::Escape, Mod::Ctrl), m), "\x1b[27;5;27~");
    EXPECT_EQ(encodeKey(key(Key::Enter), m), "\r");
}

TEST(Input, NestedPasteTerminatorsCannotForm)
{
    Modes modes;
    modes.bracketedPaste = true;
    EXPECT_EQ(encodePaste("a\x1b[20\x1b[201~1~b", modes), "\x1b[200~a[20[201~1~b\x1b[201~");
    std::string nested;
    std::string expected;
    for (int i = 0; i < 10000; ++i) {
        nested += "\x1b[20";
        expected += "[20";
    }
    for (int i = 0; i < 10000; ++i) {
        nested += "1~";
        expected += "1~";
    }
    EXPECT_EQ(encodePaste(nested, modes), "\x1b[200~" + expected + "\x1b[201~");
}
