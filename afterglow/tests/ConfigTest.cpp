// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/Settings.h"
#include "config/Theme.h"

#include <AfterglowRender/Snapshot.h>

#include <QDir>
#include <QKeyEvent>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Afterglow;
using AfterglowVt::Rgb;

namespace {

bool operator==(const std::optional<Rgb>& a, Rgb b)
{
    return a && a->r == b.r && a->g == b.g && a->b == b.b;
}

bool same(Rgb a, Rgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

/// A config directory of its own, so the user's themes stay out of it.
class ConfigTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_home.isValid());
        qputenv("XDG_CONFIG_HOME", m_home.path().toUtf8());
    }

    QString write(const QString& name, const QByteArray& content)
    {
        const QString path = m_home.path() + QLatin1Char('/') + name;
        QFile f(path);
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(content);
        return path;
    }

    QTemporaryDir m_home;
};

} // namespace

TEST(Themes, PicksTheHalfForTheColourScheme)
{
    EXPECT_EQ(Themes::pick(QStringLiteral("nord"), true), QStringLiteral("nord"));
    EXPECT_EQ(Themes::pick(QStringLiteral("light:paper,dark:night"), true), QStringLiteral("night"));
    EXPECT_EQ(Themes::pick(QStringLiteral("light:paper,dark:night"), false), QStringLiteral("paper"));
    EXPECT_EQ(Themes::pick(QStringLiteral("dark: night , light: paper"), false), QStringLiteral("paper"));
    // A missing half falls back to the other.
    EXPECT_EQ(Themes::pick(QStringLiteral("light:paper"), true), QStringLiteral("paper"));
    // Paths are names, colons and all.
    EXPECT_EQ(Themes::pick(QStringLiteral("~/themes/a:b"), true), QStringLiteral("~/themes/a:b"));
}

TEST(Themes, ReadsGhosttyThemes)
{
    const auto theme = Themes::parse("# a comment\n"
                                     "palette = 0=#1d1f21\n"
                                     "palette = 12=81a2be\n"
                                     "background = 1d1f21\n"
                                     "foreground = #c5c8c6\n"
                                     "cursor-color = #c5c8c6\n"
                                     "cursor-text = #1d1f21\n"
                                     "selection-background = #373b41\n"
                                     "selection-foreground = #c5c8c6\n");
    ASSERT_TRUE(theme);
    EXPECT_TRUE(theme->palette[0] == Rgb(0x1d, 0x1f, 0x21));
    EXPECT_TRUE(theme->palette[12] == Rgb(0x81, 0xa2, 0xbe));
    EXPECT_FALSE(theme->palette[1].has_value());
    EXPECT_TRUE(theme->background == Rgb(0x1d, 0x1f, 0x21));
    EXPECT_TRUE(theme->foreground == Rgb(0xc5, 0xc8, 0xc6));
    EXPECT_TRUE(theme->cursor == Rgb(0xc5, 0xc8, 0xc6));
    EXPECT_TRUE(theme->cursorText == Rgb(0x1d, 0x1f, 0x21));
    EXPECT_TRUE(theme->selectionBackground == Rgb(0x37, 0x3b, 0x41));
    EXPECT_TRUE(theme->selectionForeground == Rgb(0xc5, 0xc8, 0xc6));
}

TEST(Themes, ReadsKittyThemes)
{
    const auto theme = Themes::parse("## name: Example\n"
                                     "foreground            #c0caf5\n"
                                     "background #1a1b26\n"
                                     "cursor_text_color #1a1b26\n"
                                     "selection_background #283457\n"
                                     "color1 #f7768e\n"
                                     "color15 #c0caf5\n"
                                     "url_color #73daca\n");
    ASSERT_TRUE(theme);
    EXPECT_TRUE(theme->foreground == Rgb(0xc0, 0xca, 0xf5));
    EXPECT_TRUE(theme->background == Rgb(0x1a, 0x1b, 0x26));
    EXPECT_TRUE(theme->cursorText == Rgb(0x1a, 0x1b, 0x26));
    EXPECT_TRUE(theme->selectionBackground == Rgb(0x28, 0x34, 0x57));
    EXPECT_TRUE(theme->palette[1] == Rgb(0xf7, 0x76, 0x8e));
    EXPECT_TRUE(theme->palette[15] == Rgb(0xc0, 0xca, 0xf5));
}

TEST(Themes, ReadsJsonThemes)
{
    const auto theme = Themes::parse(R"({"background": "#101010", "selectionBackground": "#334455",
                                         "divider": "#222222", "palette": ["#000000", "#ff0000"]})");
    ASSERT_TRUE(theme);
    EXPECT_TRUE(theme->background == Rgb(0x10, 0x10, 0x10));
    EXPECT_TRUE(theme->selectionBackground == Rgb(0x33, 0x44, 0x55));
    EXPECT_TRUE(theme->divider == Rgb(0x22, 0x22, 0x22));
    EXPECT_TRUE(theme->palette[1] == Rgb(0xff, 0x00, 0x00));
}

TEST(Themes, RejectsFilesWithoutColours)
{
    QString error;
    EXPECT_FALSE(Themes::parse("font-size = 12\n", &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(Themes::parse("{ not json", &error));
}

TEST(Themes, BundledThemesLoad)
{
    for (const char* name : {"afterglow-dark", "afterglow-light"}) {
        const QString path = Themes::locate(QLatin1String(name), {QStringLiteral(AFTERGLOW_THEME_DIR)});
        ASSERT_FALSE(path.isEmpty()) << name;
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        const auto theme = Themes::parse(f.readAll());
        ASSERT_TRUE(theme) << name;
        for (const auto& c : theme->palette) {
            EXPECT_TRUE(c.has_value()) << name;
        }
    }
}

TEST_F(ConfigTest, ThemeColoursReplaceTheConfigs)
{
    const QString config = write(QStringLiteral("config.json"), R"({"Colors": {"theme": "afterglow-light", "background": "#000000"}})");
    SettingsStore store(config);
    const Settings& s = store.settings();
    EXPECT_TRUE(same(s.terminal.defaultBackground, {0xf6, 0xf5, 0xf2}));
    EXPECT_TRUE(s.terminal.selectionBackground == Rgb(0xc9, 0xd6, 0xee));
    EXPECT_TRUE(s.themePath.endsWith(QStringLiteral("afterglow-light.json")));
}

TEST_F(ConfigTest, UserThemesComeFirstAndDeriveADivider)
{
    QDir().mkpath(m_home.path() + QStringLiteral("/afterglow/themes"));
    write(QStringLiteral("afterglow/themes/mine"), "background = #000000\nforeground = #ffffff\n");
    const QString config = write(QStringLiteral("config.json"), R"({"Colors": {"theme": "mine"}})");
    SettingsStore store(config);
    const Settings& s = store.settings();
    EXPECT_TRUE(same(s.terminal.defaultBackground, {0, 0, 0}));
    // A fifth of the way from the background to the text.
    EXPECT_EQ(s.window.dividerColor, AfterglowRender::packRgba(0x33, 0x33, 0x33));
}

TEST_F(ConfigTest, MissingThemeKeepsTheConfigsColours)
{
    const QString config = write(QStringLiteral("config.json"), R"({"Colors": {"theme": "no-such-theme", "background": "#123456"}})");
    SettingsStore store(config);
    EXPECT_TRUE(same(store.settings().terminal.defaultBackground, {0x12, 0x34, 0x56}));
    EXPECT_TRUE(store.settings().themePath.isEmpty());
}

TEST_F(ConfigTest, ReadsTheNewerSettings)
{
    const QString config = write(QStringLiteral("config.json"), R"({
        "Font": {"fallbackFamilies": ["Symbols Nerd Font", ""], "cellHeight": 1.2, "cellWidth": 9},
        "Colors": {"cursorText": "#010203", "selectionForeground": "#ffffff", "boldIsBright": true, "minimumContrast": 4.5},
        "Terminal": {"env": {"EDITOR": "nvim", "BAD=KEY": "x"}, "wordSeparators": " ,", "scrollLines": 5,
                     "confirmClose": false, "hideMouseWhileTyping": true},
        "Window": {"workingDirectory": "home", "startMode": "MAXIMIZED", "decorations": false, "tabBar": "sometimes"}
    })");
    SettingsStore store(config);
    const Settings& s = store.settings();
    ASSERT_EQ(s.font.fallbackFamilies.size(), 1u);
    EXPECT_EQ(s.font.fallbackFamilies[0], "Symbols Nerd Font");
    EXPECT_FLOAT_EQ(s.font.cellHeightScale, 1.2f);
    EXPECT_FLOAT_EQ(s.font.cellWidthScale, 3.0f); // clamped
    EXPECT_TRUE(s.terminal.cursorTextColor == Rgb(1, 2, 3));
    EXPECT_TRUE(s.terminal.selectionForeground == Rgb(0xff, 0xff, 0xff));
    EXPECT_FALSE(s.terminal.selectionBackground.has_value());
    EXPECT_TRUE(s.terminal.boldIsBright);
    EXPECT_DOUBLE_EQ(s.terminal.minimumContrast, 4.5);
    ASSERT_EQ(s.environment.size(), 1u);
    EXPECT_EQ(s.environment[0].first, "EDITOR");
    EXPECT_EQ(s.environment[0].second, "nvim");
    EXPECT_EQ(s.terminal.wordBoundaries, U" ,");
    EXPECT_EQ(s.scrollLines, 5);
    EXPECT_FALSE(s.confirmClose);
    EXPECT_TRUE(s.hideMouseWhileTyping);
    EXPECT_EQ(s.window.workingDirectory, QStringLiteral("home"));
    EXPECT_EQ(s.window.startMode, WindowSettings::StartMode::Maximized);
    EXPECT_FALSE(s.window.decorations);
    EXPECT_EQ(s.window.tabBar, WindowSettings::TabBar::Auto); // not a choice: the default
}

TEST(Keybinds, TextBindingsSendTheirBytes)
{
    EXPECT_EQ(Keybinds::unescape(QStringLiteral("\\x1b\\r")), QByteArray("\x1b\r"));
    EXPECT_EQ(Keybinds::unescape(QStringLiteral("a\\e[A\\t\\\\\\u00e9")), QByteArray("a\x1b[A\t\\\xc3\xa9"));
    EXPECT_TRUE(Keybinds::unescape(QStringLiteral("\\q")).isEmpty());
    EXPECT_TRUE(Keybinds::unescape(QStringLiteral("\\x1")).isEmpty());

    QStringList errors;
    const Keybinds kb = Keybinds::fromMap({{QStringLiteral("text:\\x1b\\r"), QStringLiteral("Shift+Return")},
                                           {QStringLiteral("text:\\z"), QStringLiteral("Ctrl+Return")}},
                                          &errors);
    EXPECT_EQ(errors.size(), 1); // the malformed one
    const QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier);
    EXPECT_EQ(kb.matchText(&press), QByteArray("\x1b\r"));
    EXPECT_EQ(kb.match(&press), Action::None);
}

TEST_F(ConfigTest, ReadsTheTierTwoSettings)
{
    const QString config = write(QStringLiteral("config.json"), R"({
        "Font": {"symbolMap": {"U+E000-U+F8FF,U+2500": "Symbols Nerd Font Mono", "U+zz": "Broken", "U+41": ""},
                 "hinting": "full"},
        "Terminal": {"cursorBlinkInterval": 20, "scrollOnOutput": true, "scrollOnInput": false,
                     "ambiguousWide": true, "bellSound": " default "},
        "Window": {"balancePadding": true},
        "Mouse": {"rightClick": "copy-or-paste", "middleClick": "clipboard"},
        "Links": {"detectUrls": false, "openWith": "firefox --new-tab", "modifier": "none"}
    })");
    SettingsStore store(config);
    const Settings& s = store.settings();
    ASSERT_EQ(s.font.symbolMap.size(), 2u); // the broken and empty entries are skipped
    EXPECT_EQ(s.font.symbolMap[0].first, 0xE000u);
    EXPECT_EQ(s.font.symbolMap[0].last, 0xF8FFu);
    EXPECT_EQ(s.font.symbolMap[1].first, 0x2500u);
    EXPECT_EQ(s.font.symbolMap[1].last, 0x2500u);
    EXPECT_EQ(s.font.symbolMap[1].family, "Symbols Nerd Font Mono");
    EXPECT_EQ(s.font.hinting, AfterglowRender::Hinting::Full);
    EXPECT_EQ(s.cursorBlinkInterval, 100); // clamped
    EXPECT_TRUE(s.scrollOnOutput);
    EXPECT_FALSE(s.scrollOnInput);
    EXPECT_TRUE(s.terminal.ambiguousWide);
    EXPECT_EQ(s.bellSound, QStringLiteral("default"));
    EXPECT_TRUE(s.window.balancePadding);
    EXPECT_EQ(s.mouse.rightClick, MouseSettings::RightClick::CopyOrPaste);
    EXPECT_EQ(s.mouse.middleClick, MouseSettings::MiddleClick::Clipboard);
    EXPECT_FALSE(s.links.detectUrls);
    EXPECT_EQ(s.links.openWith, QStringLiteral("firefox --new-tab"));
    EXPECT_EQ(s.links.modifier, Qt::KeyboardModifiers(Qt::NoModifier));
}

TEST(Overrides, ParseAssignments)
{
    QJsonObject o;
    EXPECT_TRUE(SettingsStore::addOverride(o, QStringLiteral("Font.size=14")));
    EXPECT_TRUE(SettingsStore::addOverride(o, QStringLiteral("Font.family=Monaspace Radon Var")));
    EXPECT_TRUE(SettingsStore::addOverride(o, QStringLiteral("Window.decorations=false")));
    EXPECT_TRUE(SettingsStore::addOverride(o, QStringLiteral("Font.features=[\"calt\"]")));
    EXPECT_TRUE(SettingsStore::addOverride(o, QStringLiteral("Colors.theme=light:a,dark:b")));
    EXPECT_FALSE(SettingsStore::addOverride(o, QStringLiteral("size=14")));
    EXPECT_FALSE(SettingsStore::addOverride(o, QStringLiteral("Font.size")));
    EXPECT_FALSE(SettingsStore::addOverride(o, QStringLiteral(".size=1")));
    const QJsonObject font = o.value(QStringLiteral("Font")).toObject();
    EXPECT_EQ(font.value(QStringLiteral("size")).toDouble(), 14.0);
    EXPECT_EQ(font.value(QStringLiteral("family")).toString(), QStringLiteral("Monaspace Radon Var"));
    EXPECT_TRUE(font.value(QStringLiteral("features")).isArray());
    EXPECT_EQ(o.value(QStringLiteral("Window")).toObject().value(QStringLiteral("decorations")), QJsonValue(false));
    EXPECT_EQ(o.value(QStringLiteral("Colors")).toObject().value(QStringLiteral("theme")).toString(),
              QStringLiteral("light:a,dark:b"));
}

TEST_F(ConfigTest, OverridesWinOverTheFileAndLeaveItAlone)
{
    const QByteArray original = R"({"Font": {"size": 11, "family": "Monaspace Krypton Var"}})";
    const QString config = write(QStringLiteral("config.json"), original);
    QJsonObject o;
    SettingsStore::addOverride(o, QStringLiteral("Font.size=14"));
    {
        SettingsStore store(config, o);
        EXPECT_FLOAT_EQ(store.settings().font.sizePoints, 14.0f);
        EXPECT_EQ(store.settings().font.regular.family, "Monaspace Krypton Var"); // the rest from the file
    }
    QFile f(config);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), original);
}

TEST_F(ConfigTest, AudioDefaultsAndOverrides)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    EXPECT_EQ(defaults.settings().audio, AudioSettings{});
    SettingsStore custom(write(QStringLiteral("audio.json"), R"({"Audio": {
        "enabled": false, "inputMethod": "pulse", "inputSource": "speakers.monitor"
    }})"));
    EXPECT_FALSE(custom.settings().audio.enabled);
    EXPECT_EQ(custom.settings().audio.inputMethod, QStringLiteral("pulse"));
    EXPECT_EQ(custom.settings().audio.inputSource, QStringLiteral("speakers.monitor"));
}
