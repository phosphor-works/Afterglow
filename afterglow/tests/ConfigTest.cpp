// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/Settings.h"
#include "config/Theme.h"

#include <AfterglowRender/Snapshot.h>

#include <QDir>
#include <QFile>
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
