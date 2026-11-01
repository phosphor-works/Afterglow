// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/Settings.h"
#include "config/Theme.h"

#include <AfterglowRender/Snapshot.h>

#include <PhosphorConfig/Schema.h>

#include <QDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QSaveFile>
#include <QKeyEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

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

/// Config and data directories of its own, so the user's themes stay out of it.
class ConfigTest : public ::testing::Test
{
protected:
    static constexpr const char* Variables[] = {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_DATA_DIRS"};

    void SetUp() override
    {
        ASSERT_TRUE(m_home.isValid());
        for (const char* name : Variables) {
            m_saved.append({name, qEnvironmentVariableIsSet(name), qgetenv(name)});
        }
        qputenv("XDG_CONFIG_HOME", m_home.path().toUtf8());
        qputenv("XDG_DATA_HOME", m_home.filePath(QStringLiteral("data-home")).toUtf8());
        qputenv("XDG_DATA_DIRS", m_home.filePath(QStringLiteral("data-dirs")).toUtf8());
    }

    void TearDown() override
    {
        for (const auto& [name, wasSet, value] : m_saved) {
            if (wasSet) qputenv(name, value);
            else qunsetenv(name);
        }
    }

    struct Saved
    {
        const char* name;
        bool wasSet;
        QByteArray value;
    };
    QList<Saved> m_saved;

    QString write(const QString& name, const QByteArray& content)
    {
        const QString path = m_home.path() + QLatin1Char('/') + name;
        QFile f(path);
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(content);
        return path;
    }

    QByteArray read(const QString& path)
    {
        QFile f(path);
        EXPECT_TRUE(f.open(QIODevice::ReadOnly));
        return f.readAll();
    }

    QTemporaryDir m_home;
};

bool mentions(const QStringList& problems, const QString& text)
{
    return std::any_of(problems.begin(), problems.end(), [&](const QString& p) { return p.contains(text); });
}

QKeyEvent press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool autoRepeat = false)
{
    return QKeyEvent(QEvent::KeyPress, key, modifiers, QString(), autoRepeat);
}

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
    // Either half may be a path.
    EXPECT_EQ(Themes::pick(QStringLiteral("light:~/t/a.conf,dark:nord"), false), QStringLiteral("~/t/a.conf"));
    EXPECT_EQ(Themes::pick(QStringLiteral("light:~/t/a.conf,dark:nord"), true), QStringLiteral("nord"));
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

TEST(Themes, EightDigitHexIsNotAColour)
{
    // Qt reads eight digits as #AARRGGBB; these colours have no alpha.
    EXPECT_FALSE(Themes::isColorName(QStringLiteral("#1e1e2eff")));
    EXPECT_TRUE(Themes::isColorName(QStringLiteral("#1e1e2e")));
    EXPECT_TRUE(Themes::isColorName(QStringLiteral("#fff")));
    EXPECT_TRUE(Themes::isColorName(QStringLiteral("red")));
    QStringList ignored;
    QStringList invalid;
    const auto theme = Themes::parse(R"({"background": "#101010", "foreground": "#1e1e2eff"})", nullptr, &ignored, &invalid);
    ASSERT_TRUE(theme);
    EXPECT_FALSE(theme->foreground);
    // A key Afterglow uses, with a value it cannot: not an unused key.
    EXPECT_TRUE(ignored.isEmpty());
    EXPECT_EQ(invalid, QStringList{QStringLiteral("foreground")});
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
    const QStringList names = QDir(QStringLiteral(AFTERGLOW_THEME_DIR)).entryList({QStringLiteral("*.json")}, QDir::Files);
    ASSERT_GE(names.size(), 33);
    for (const QString& file : names) {
        const QString name = QFileInfo(file).completeBaseName();
        const QString path = Themes::locate(name, {QStringLiteral(AFTERGLOW_THEME_DIR)});
        ASSERT_FALSE(path.isEmpty()) << name.toStdString();
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        QStringList ignored;
        QStringList invalid;
        const auto theme = Themes::parse(f.readAll(), nullptr, &ignored, &invalid);
        ASSERT_TRUE(theme) << name.toStdString();
        // A misspelt key would be dropped without a word.
        EXPECT_TRUE(ignored.isEmpty()) << name.toStdString() << ": " << ignored.join(QLatin1Char(' ')).toStdString();
        EXPECT_TRUE(invalid.isEmpty()) << name.toStdString() << ": " << invalid.join(QLatin1Char(' ')).toStdString();
        EXPECT_TRUE(theme->foreground && theme->background)
            << name.toStdString();
        for (const auto& c : theme->palette) {
            EXPECT_TRUE(c.has_value()) << name.toStdString();
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
    EXPECT_TRUE(store.problems().isEmpty()) << store.problems().join(QLatin1Char('\n')).toStdString();
}

TEST_F(ConfigTest, UserThemesComeFirstAndDeriveADivider)
{
    // The same name as a bundled theme, whose background is #1c1c1c.
    QDir().mkpath(m_home.path() + QStringLiteral("/afterglow/themes"));
    write(QStringLiteral("afterglow/themes/afterglow-dark"), "background = #000000\nforeground = #ffffff\n");
    const QString config = write(QStringLiteral("config.json"), R"({"Colors": {"theme": "afterglow-dark"}})");
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
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("no-such-theme")));
}

TEST_F(ConfigTest, ReadsFontColorTerminalAndWindowSettings)
{
    const QString config = write(QStringLiteral("config.json"), R"({
        "Font": {"fallbackFamilies": ["Symbols Nerd Font", ""], "cellHeight": 1.2, "cellWidth": 9},
        "Colors": {"cursorText": "#010203", "selectionForeground": "#ffffff", "boldIsBright": true, "minimumContrast": 4.5},
        "Terminal": {"env": {"EDITOR": "nvim", "BAD=KEY": "x"}, "wordSeparators": " ,", "scrollLines": 5,
                     "confirmClose": false, "hideMouseWhileTyping": true},
        "Window": {"workingDirectory": "home", "startMode": "MAXIMIZED", "decorations": false, "tabBar": "sometimes",
                   "scrollbar": "always", "scrollbarHideDelay": 90000},
        "Animation": {"splitCurve": " 0.2,0,0,1 ", "splitDuration": 5}
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
    EXPECT_EQ(s.window.scrollbar, WindowSettings::Scrollbar::Always);
    EXPECT_EQ(s.window.scrollbarHideDelay, 60000); // clamped
    EXPECT_EQ(s.animation.splitCurve, QStringLiteral("0.2,0,0,1"));
    EXPECT_EQ(s.animation.splitDuration, 16); // clamped
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

TEST_F(ConfigTest, ReadsSymbolMapMouseAndLinkSettings)
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

TEST_F(ConfigTest, StructuralActionClassification)
{
    for (Action action : {Action::SplitRight, Action::SplitLeft, Action::SplitDown, Action::SplitUp,
                          Action::NewTab, Action::NewWindow, Action::ClosePane, Action::CloseTab, Action::CloseWindow}) {
        EXPECT_TRUE(Keybinds::isStructural(action));
    }
    for (Action action : {Action::Copy, Action::NextTab, Action::MoveTabLeft, Action::RenamePane, Action::EqualizeSplits}) {
        EXPECT_FALSE(Keybinds::isStructural(action));
    }
}

TEST_F(ConfigTest, FontRenderingDefaultsNormalizationAndReload)
{
    const QString path = write(QStringLiteral("fonts.json"), "{}");
    SettingsStore store(path);
    EXPECT_EQ(store.settings().font.antialiasing, AfterglowRender::Antialiasing::Grayscale);
    EXPECT_FLOAT_EQ(store.settings().font.gamma, 1.0f);
    const std::pair<const char*, AfterglowRender::Antialiasing> modes[] = {
        {"RGB", AfterglowRender::Antialiasing::Rgb}, {"bgr", AfterglowRender::Antialiasing::Bgr},
        {"none", AfterglowRender::Antialiasing::None}, {"invalid", AfterglowRender::Antialiasing::Grayscale},
    };
    for (const auto& [name, aa] : modes) {
        write(QStringLiteral("fonts.json"), QByteArray("{\"Font\":{\"antialiasing\":\"") + name + "\",\"gamma\":1.4}}");
        store.reload();
        EXPECT_EQ(store.settings().font.antialiasing, aa);
        EXPECT_FLOAT_EQ(store.settings().font.gamma, 1.4f);
    }
    for (const auto& [value, expected] : {std::pair{-5, 0.5f}, std::pair{10, 3.0f}}) {
        write(QStringLiteral("fonts.json"), QByteArray("{\"Font\":{\"gamma\":") + QByteArray::number(value) + "}}");
        store.reload();
        EXPECT_FLOAT_EQ(store.settings().font.gamma, expected);
    }
    QJsonObject overrides;
    SettingsStore::addOverride(overrides, QStringLiteral("Font.antialiasing=bgr"));
    SettingsStore::addOverride(overrides, QStringLiteral("Font.gamma=1.2"));
    SettingsStore overridden(path, overrides);
    EXPECT_EQ(overridden.settings().font.antialiasing, AfterglowRender::Antialiasing::Bgr);
    EXPECT_FLOAT_EQ(overridden.settings().font.gamma, 1.2f);
}

TEST(KeyModes, LeaderSequenceAndMismatchFallback)
{
    QVariantMap bindings{{QStringLiteral("split_right"), QStringLiteral("Leader, E")},
                         {QStringLiteral("split_down"), QStringLiteral("Leader, O")}};
    QStringList errors;
    const auto singles = Keybinds::fromMap(bindings, &errors);
    const auto modes = KeyModes::fromConfig(bindings, {}, QStringLiteral("Ctrl+Space"), 1000, 5000, singles, &errors);
    EXPECT_TRUE(errors.isEmpty()) << errors.join('\n').toStdString();
    KeyModes::State state;
    QKeyEvent leader(QEvent::KeyPress, Qt::Key_Space, Qt::ControlModifier);
    QKeyEvent e(QEvent::KeyPress, Qt::Key_E, Qt::NoModifier);
    QKeyEvent x(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier);
    EXPECT_TRUE(modes.press(&leader, state, 0).consumed);
    EXPECT_TRUE(state.active());
    EXPECT_EQ(modes.press(&e, state, 100).target, QStringLiteral("split_right"));
    EXPECT_FALSE(state.active());
    modes.press(&leader, state, 200);
    EXPECT_FALSE(modes.press(&x, state, 300).consumed);
    EXPECT_FALSE(state.active());
    modes.press(&leader, state, 400);
    EXPECT_FALSE(modes.press(&e, state, 1400).consumed);
    EXPECT_FALSE(state.active());
}

TEST(KeyModes, TablesCancelExpireAndDoNotTrapUnmatchedInput)
{
    QVariantMap bindings{{QStringLiteral("table:resize"), QStringLiteral("Ctrl+Space, R")}};
    QVariantMap resize{{QStringLiteral("resize_left"), QStringLiteral("H")},
                       {QStringLiteral("resize_right"), QStringLiteral("L")},
                       {QStringLiteral("pop_key_table"), QStringLiteral("Return")}};
    QStringList errors;
    const auto modes = KeyModes::fromConfig(bindings, {{QStringLiteral("resize"), resize}}, {}, 1000, 5000,
        Keybinds::fromMap(bindings, &errors), &errors);
    EXPECT_TRUE(errors.isEmpty());
    KeyModes::State state;
    QKeyEvent leader(QEvent::KeyPress, Qt::Key_Space, Qt::ControlModifier);
    QKeyEvent r(QEvent::KeyPress, Qt::Key_R, Qt::NoModifier);
    QKeyEvent h(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QKeyEvent x(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier);
    auto enter = [&] { modes.press(&leader, state, 0); modes.press(&r, state, 100); };
    enter();
    EXPECT_EQ(state.table, QStringLiteral("resize"));
    EXPECT_EQ(modes.press(&h, state, 200).target, QStringLiteral("resize_left"));
    EXPECT_TRUE(state.active());
    EXPECT_TRUE(modes.press(&escape, state, 300).consumed);
    EXPECT_FALSE(state.active());
    enter();
    EXPECT_FALSE(modes.press(&x, state, 300).consumed);
    EXPECT_FALSE(state.active());
    enter();
    KeyModes::expire(state, 5100);
    EXPECT_FALSE(state.active());
}

TEST(KeyModes, TablesMatchTheCallersBaseKey)
{
    // On a Russian layout R and H report the Cyrillic К and Р. The caller
    // gives the Latin key of the same position, and the table matches it.
    QVariantMap bindings{{QStringLiteral("table:resize"), QStringLiteral("Ctrl+Space, R")}};
    QVariantMap resize{{QStringLiteral("resize_left"), QStringLiteral("H")}};
    QStringList errors;
    const auto modes = KeyModes::fromConfig(bindings, {{QStringLiteral("resize"), resize}}, {}, 1000, 5000,
        Keybinds::fromMap(bindings, &errors), &errors);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
    KeyModes::State state;
    const QKeyEvent leader = press(Qt::Key_Space, Qt::ControlModifier);
    const QKeyEvent ka = press(0x041A);
    const QKeyEvent er = press(0x0420);
    modes.press(&leader, state, 0);
    EXPECT_TRUE(modes.press(&ka, state, 10, U'r').consumed);
    EXPECT_EQ(state.table, QStringLiteral("resize"));
    EXPECT_EQ(modes.press(&er, state, 20, U'h').target, QStringLiteral("resize_left"));
    // Without the base key the Cyrillic letter matches nothing and leaves the table.
    EXPECT_FALSE(modes.press(&er, state, 30).consumed);
    EXPECT_FALSE(state.active());
}

TEST(KeyModes, RejectsAmbiguousPrefixesAndPreservesExistingSingles)
{
    QStringList errors;
    QVariantMap bindings{{QStringLiteral("split_right"), QStringLiteral("Ctrl+Shift+C, E")},
                         {QStringLiteral("split_down"), QStringLiteral("Ctrl+Space, X")},
                         {QStringLiteral("new_tab"), QStringLiteral("Ctrl+Space, X, T")}};
    const auto singles = Keybinds::fromMap(bindings, &errors);
    const auto modes = KeyModes::fromConfig(bindings, {}, {}, 1000, 5000, singles, &errors);
    EXPECT_EQ(errors.size(), 2);
    KeyModes::State state;
    QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_FALSE(modes.press(&copy, state, 0).consumed);
    EXPECT_EQ(singles.match(&copy), Action::Copy);
}

TEST(Config, ReferenceUsesRuntimeDefaultsAndChoices)
{
    const auto groups = Afterglow::SettingsStore::reference().value(QStringLiteral("groups")).toObject();
    const auto font = groups.value(QStringLiteral("Font")).toObject();
    EXPECT_EQ(font.value(QStringLiteral("antialiasing")).toObject().value(QStringLiteral("default")).toString(), QStringLiteral("grayscale"));
    // 0 is the desktop's size; other sizes run from 4 to 96, the limits font_bigger keeps to.
    EXPECT_EQ(font.value(QStringLiteral("size")).toObject().value(QStringLiteral("minimum")).toInt(), 0);
    EXPECT_EQ(font.value(QStringLiteral("size")).toObject().value(QStringLiteral("maximum")).toInt(), 96);
    EXPECT_EQ(AfterglowRender::FontSpec::MaxPoints, 96.0f);
    EXPECT_TRUE(font.value(QStringLiteral("size")).toObject().value(QStringLiteral("description")).toString().contains(QStringLiteral("from 4 to 96")));
    EXPECT_TRUE(font.value(QStringLiteral("family")).toObject().value(QStringLiteral("default")).toString().isEmpty());
    EXPECT_TRUE(groups.value(QStringLiteral("Keybinds")).toObject().value(QStringLiteral("bindings")).toObject().value(QStringLiteral("default")).isObject());
}

TEST(Keybinds, SupplementaryUnicodeSurvivesLiteralAndEscapedBindings)
{
    EXPECT_EQ(Keybinds::unescape(QString::fromUtf8("a🙂é\\n")), QByteArray("a🙂é\n"));
    EXPECT_EQ(Keybinds::unescape(QStringLiteral("\\uD83D\\uDE42")), QByteArray("🙂"));
    EXPECT_TRUE(Keybinds::unescape(QStringLiteral("\\uD83D")).isEmpty());
    EXPECT_TRUE(Keybinds::unescape(QStringLiteral("\\uDE42")).isEmpty());
    EXPECT_TRUE(Keybinds::unescape(QStringLiteral("\\uD83D\\u0041")).isEmpty());
}

TEST_F(ConfigTest, BidiAndScriptReadPolicyHaveExplicitDefaultsAndOverrides)
{
    const auto path = write(QStringLiteral("options.json"), "{}");
    SettingsStore defaults(path);
    EXPECT_EQ(defaults.settings().terminal.bidi, AfterglowVt::BidiMode::Off);
    EXPECT_TRUE(defaults.settings().scriptsAllowReadText);
    SettingsStore configured(path, QJsonObject{
        {QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("bidi"), QStringLiteral("rtl")}}},
        {QStringLiteral("Scripts"), QJsonObject{{QStringLiteral("allowReadText"), false}}}});
    EXPECT_EQ(configured.settings().terminal.bidi, AfterglowVt::BidiMode::RightToLeft);
    EXPECT_FALSE(configured.settings().scriptsAllowReadText);
}

TEST_F(ConfigTest, ExternalThemeWatchRecoversCreationReplacementAndDeletion)
{
    int argc = 1;
    char name[] = "config-test";
    char* argv[] = {name, nullptr};
    std::unique_ptr<QCoreApplication> app;
    if (!QCoreApplication::instance()) app = std::make_unique<QCoreApplication>(argc, argv);
    const auto theme = m_home.filePath(QStringLiteral("new/sub/theme.json"));
    const auto config = write(QStringLiteral("watch.json"), QJsonDocument(QJsonObject{
        {QStringLiteral("Colors"), QJsonObject{{QStringLiteral("theme"), theme}, {QStringLiteral("foreground"), QStringLiteral("#112233")}}}
    }).toJson());
    SettingsStore store(config);
    auto waitFor = [&](Rgb color) {
        QElapsedTimer timer;
        timer.start();
        do {
            QCoreApplication::processEvents();
            QThread::msleep(5);
            if (same(store.settings().terminal.defaultForeground, color)) return true;
        } while (timer.elapsed() < 2500);
        return false;
    };
    auto replace = [&](const QByteArray& value) {
        QSaveFile file(theme);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        EXPECT_EQ(file.write(value), value.size());
        EXPECT_TRUE(file.commit());
    };
    ASSERT_TRUE(QDir().mkpath(QFileInfo(theme).absolutePath()));
    replace("{\"foreground\":\"#445566\"}");
    ASSERT_TRUE(waitFor({0x44, 0x55, 0x66}));
    replace("{\"foreground\":\"#778899\"}");
    ASSERT_TRUE(waitFor({0x77, 0x88, 0x99}));
    ASSERT_TRUE(QFile::remove(theme));
    ASSERT_TRUE(waitFor({0x11, 0x22, 0x33}));
    replace("{\"foreground\":\"#aabbcc\"}");
    ASSERT_TRUE(waitFor({0xaa, 0xbb, 0xcc}));
    const auto other = write(QStringLiteral("other.json"), "{\"foreground\":\"#123456\"}");
    QFile file(config);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("Colors"), QJsonObject{{QStringLiteral("theme"), other}}}}).toJson());
    file.close();
    ASSERT_TRUE(waitFor({0x12, 0x34, 0x56}));
    // After the switch only the new theme is watched. A reload that changes
    // nothing emits nothing, so the watched set is what shows a stale watch.
    const QStringList watched = store.watchedPaths();
    EXPECT_TRUE(watched.contains(config));
    EXPECT_TRUE(watched.contains(other));
    EXPECT_FALSE(watched.contains(theme));
}

TEST_F(ConfigTest, WritesOneKeyAndKeepsTheRest)
{
    const QString path = write(QStringLiteral("config.json"), R"({"Font": {"size": 15}, "Shaders": {"window": ["bloom"]}})");
    SettingsStore store(path);
    ASSERT_TRUE(store.write(QStringLiteral("Shaders"), QStringLiteral("overview"), QStringLiteral("scripted-overview.wall")));
    EXPECT_EQ(store.settings().shaders.overview, QStringLiteral("scripted-overview.wall"));
    EXPECT_EQ(store.settings().shaders.window, QStringList{QStringLiteral("bloom")});
    EXPECT_EQ(store.settings().font.sizePoints, 15.0f);
    SettingsStore reread(path);
    EXPECT_EQ(reread.settings().shaders.overview, QStringLiteral("scripted-overview.wall"));
}

TEST_F(ConfigTest, SecurityPoliciesDefaultAndOverride)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    const Settings& d = defaults.settings();
    EXPECT_FALSE(d.allowScreenReadback);
    EXPECT_EQ(d.graphicsFileTransfer, Settings::Permission::Allow);
    EXPECT_EQ(d.clipboardWrite, Settings::Permission::Allow);
    EXPECT_EQ(d.links.schemes, (QStringList{QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("mailto"),
                                            QStringLiteral("ftp"), QStringLiteral("file")}));

    SettingsStore custom(write(QStringLiteral("custom.json"), R"({
        "Terminal": {"allowScreenReadback": true, "graphicsFileTransfer": "DENY", "clipboardWrite": "ask"},
        "Links": {"schemes": [" HTTPS ", "gemini:", "", "https"]}
    })"));
    const Settings& c = custom.settings();
    EXPECT_TRUE(c.allowScreenReadback);
    EXPECT_EQ(c.graphicsFileTransfer, Settings::Permission::Deny);
    // The terminal core enforces them, so they reach its options.
    EXPECT_TRUE(c.terminal.allowScreenReadback);
    EXPECT_FALSE(c.terminal.graphicsFileTransfer);
    EXPECT_FALSE(d.terminal.allowScreenReadback);
    EXPECT_TRUE(d.terminal.graphicsFileTransfer);
    EXPECT_EQ(c.clipboardWrite, Settings::Permission::Ask);
    // Trimmed, lower case, without the colon or repeats.
    EXPECT_EQ(c.links.schemes, (QStringList{QStringLiteral("https"), QStringLiteral("gemini")}));

    // Not a choice: the default. graphicsFileTransfer has no ask.
    SettingsStore invalid(write(QStringLiteral("invalid.json"), R"({"Terminal": {"graphicsFileTransfer": "ask", "clipboardWrite": "maybe"}})"));
    EXPECT_EQ(invalid.settings().graphicsFileTransfer, Settings::Permission::Allow);
    EXPECT_EQ(invalid.settings().clipboardWrite, Settings::Permission::Allow);
}

TEST(Keybinds, EndScriptKeyModeHasADefaultChord)
{
    const Keybinds kb = Keybinds::defaults();
    EXPECT_EQ(Keybinds::token(Action::EndScriptKeyMode), QStringLiteral("end_script_key_mode"));
    const QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(kb.match(&press), Action::EndScriptKeyMode);
    const Keybinds moved = Keybinds::fromMap({{QStringLiteral("end_script_key_mode"), QStringLiteral("Ctrl+Alt+Q")}});
    EXPECT_EQ(moved.match(&press), Action::None);
    const QKeyEvent other(QEvent::KeyPress, Qt::Key_Q, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_EQ(moved.match(&other), Action::EndScriptKeyMode);
}

TEST_F(ConfigTest, FontFollowsTheDesktopUntilSet)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    const AfterglowRender::FontSpec& d = defaults.settings().font;
    EXPECT_FALSE(d.regular.family.empty());
    EXPECT_EQ(d.bold.family, d.regular.family);
    EXPECT_GT(d.sizePoints, 0.0f);
    if (!qGuiApp) {
        // No platform theme to ask: fontconfig's alias at 11 points.
        EXPECT_EQ(d.regular.family, "monospace");
        EXPECT_FLOAT_EQ(d.sizePoints, 11.0f);
    }
    EXPECT_EQ(d.features, (std::vector<std::string>{"calt", "liga"}));
    EXPECT_DOUBLE_EQ(defaults.settings().fontSizeStep, 1.0);

    SettingsStore custom(write(QStringLiteral("custom.json"), R"({"Font": {"family": " Iosevka ", "size": 13.5, "sizeStep": 0.5,
                                                                           "features": ["-liga"]}})"));
    const AfterglowRender::FontSpec& c = custom.settings().font;
    EXPECT_EQ(c.regular.family, "Iosevka");
    EXPECT_EQ(c.boldItalic.family, "Iosevka");
    EXPECT_FLOAT_EQ(c.sizePoints, 13.5f);
    EXPECT_EQ(c.features, std::vector<std::string>{"-liga"});
    EXPECT_DOUBLE_EQ(custom.settings().fontSizeStep, 0.5);

    // Below the range clamps to 4. Negative sizes mean the desktop's, as 0 does.
    SettingsStore small(write(QStringLiteral("small.json"), R"({"Font": {"size": 2, "sizeStep": 50}})"));
    EXPECT_FLOAT_EQ(small.settings().font.sizePoints, 4.0f);
    EXPECT_DOUBLE_EQ(small.settings().fontSizeStep, 12.0);
    SettingsStore negative(write(QStringLiteral("negative.json"), R"({"Font": {"size": -3}})"));
    EXPECT_FLOAT_EQ(negative.settings().font.sizePoints, d.sizePoints);
}

TEST(Config, DoubleKeysRejectNonFiniteValues)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("config.json"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{}");
    file.close();
    // Overrides are not parsed from text, so they can carry what JSON cannot.
    const QJsonObject overrides{{QStringLiteral("Font"),
                                 QJsonObject{{QStringLiteral("size"), std::numeric_limits<double>::quiet_NaN()},
                                             {QStringLiteral("gamma"), std::numeric_limits<double>::infinity()}}}};
    SettingsStore store(path, overrides);
    EXPECT_TRUE(std::isfinite(store.settings().font.sizePoints));
    EXPECT_GT(store.settings().font.sizePoints, 0.0f);
    EXPECT_FLOAT_EQ(store.settings().font.gamma, 1.0f); // the default, not the maximum
}

TEST_F(ConfigTest, AnimationCurvesAndTextBlinkDefaultAndOverride)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    const Settings& d = defaults.settings();
    EXPECT_EQ(d.animation.cursorCurve, QStringLiteral("spring:48,1"));
    EXPECT_EQ(d.animation.scrollCurve, QStringLiteral("spring:26,1"));
    EXPECT_EQ(d.animation.bellCurve, QStringLiteral("spring:14,1"));
    EXPECT_EQ(d.animation.cursorDuration, 150);
    EXPECT_EQ(d.quickTerminal.curve, QStringLiteral("spring:22,1"));
    EXPECT_EQ(d.quickTerminal.duration, 150);
    EXPECT_EQ(d.textBlinkInterval, 500);

    SettingsStore custom(write(QStringLiteral("custom.json"), R"({
        "Animation": {"cursorCurve": " 0.2,0,0,1 ", "cursorDuration": 90, "scrollCurve": "spring:10,0.8",
                      "bellCurve": "elastic-out", "bellDuration": 99999, "scrollDuration": 1},
        "QuickTerminal": {"curve": "spring:30,1", "duration": 300},
        "Terminal": {"textBlinkInterval": 10}
    })"));
    const Settings& c = custom.settings();
    EXPECT_EQ(c.animation.cursorCurve, QStringLiteral("0.2,0,0,1"));
    EXPECT_EQ(c.animation.cursorDuration, 90);
    EXPECT_EQ(c.animation.scrollCurve, QStringLiteral("spring:10,0.8"));
    EXPECT_EQ(c.animation.scrollDuration, 16); // clamped
    EXPECT_EQ(c.animation.bellCurve, QStringLiteral("elastic-out"));
    EXPECT_EQ(c.animation.bellDuration, 5000); // clamped
    EXPECT_EQ(c.quickTerminal.curve, QStringLiteral("spring:30,1"));
    EXPECT_EQ(c.quickTerminal.duration, 300);
    EXPECT_EQ(c.textBlinkInterval, 100); // clamped
}

TEST_F(ConfigTest, LastOutputLimitIsKibibytesWithAClamp)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    EXPECT_EQ(defaults.settings().lastOutputLimit, std::size_t(1024) << 10);

    SettingsStore custom(write(QStringLiteral("custom.json"), R"({"Terminal": {"lastOutputLimit": 4096}})"));
    EXPECT_EQ(custom.settings().lastOutputLimit, std::size_t(4096) << 10);

    SettingsStore low(write(QStringLiteral("low.json"), R"({"Terminal": {"lastOutputLimit": 0}})"));
    EXPECT_EQ(low.settings().lastOutputLimit, std::size_t(1) << 10);
    SettingsStore high(write(QStringLiteral("high.json"), R"({"Terminal": {"lastOutputLimit": 999999}})"));
    EXPECT_EQ(high.settings().lastOutputLimit, std::size_t(65536) << 10);
}

TEST_F(ConfigTest, HighlightColoursComeFromTheConfigAndTheTheme)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    const HighlightColors& d = defaults.settings().colors;
    EXPECT_TRUE(same(d.searchMatch, {0xe5, 0xc0, 0x7b}));
    EXPECT_TRUE(same(d.searchCurrentMatch, {0xff, 0x8c, 0x42}));
    EXPECT_TRUE(same(d.searchMatchText, {0x10, 0x10, 0x10}));
    EXPECT_TRUE(same(d.visualBell, {0xff, 0xff, 0xff}));
    EXPECT_DOUBLE_EQ(d.visualBellIntensity, 0.18);

    SettingsStore custom(write(QStringLiteral("custom.json"), R"({"Colors": {
        "searchMatch": "#010203", "searchCurrentMatch": "#040506", "searchMatchText": "not a colour",
        "visualBell": "#ff0000", "visualBellIntensity": 3}})"));
    const HighlightColors& c = custom.settings().colors;
    EXPECT_TRUE(same(c.searchMatch, {1, 2, 3}));
    EXPECT_TRUE(same(c.searchCurrentMatch, {4, 5, 6}));
    EXPECT_TRUE(same(c.searchMatchText, {0x10, 0x10, 0x10})); // invalid: the default
    EXPECT_TRUE(same(c.visualBell, {0xff, 0, 0}));
    EXPECT_DOUBLE_EQ(c.visualBellIntensity, 1.0); // clamped

    // A theme's colours replace the config's; what it leaves out stays.
    QDir().mkpath(m_home.path() + QStringLiteral("/afterglow/themes"));
    write(QStringLiteral("afterglow/themes/marks.json"),
          R"({"background": "#000000", "searchMatch": "#111111", "visualBell": "#222222", "visualBellIntensity": 0.5})");
    SettingsStore themed(write(QStringLiteral("themed.json"),
                               R"({"Colors": {"theme": "marks", "searchMatch": "#010203", "searchCurrentMatch": "#040506"}})"));
    const HighlightColors& t = themed.settings().colors;
    EXPECT_TRUE(same(t.searchMatch, {0x11, 0x11, 0x11}));
    EXPECT_TRUE(same(t.searchCurrentMatch, {4, 5, 6}));
    EXPECT_TRUE(same(t.visualBell, {0x22, 0x22, 0x22}));
    EXPECT_DOUBLE_EQ(t.visualBellIntensity, 0.5);
}

TEST(Themes, ReadsHighlightColours)
{
    const auto json = Themes::parse(R"({"searchCurrentMatch": "#123456", "searchMatchText": "#000000",
                                        "visualBellIntensity": 2})");
    ASSERT_TRUE(json);
    EXPECT_TRUE(json->searchCurrentMatch == Rgb(0x12, 0x34, 0x56));
    EXPECT_TRUE(json->searchMatchText == Rgb(0, 0, 0));
    EXPECT_FALSE(json->visualBellIntensity.has_value()); // out of range
    const auto kitty = Themes::parse("visual_bell_color #abcdef\n");
    ASSERT_TRUE(kitty);
    EXPECT_TRUE(kitty->visualBell == Rgb(0xab, 0xcd, 0xef));
    // An intensity alone is not a theme.
    EXPECT_FALSE(Themes::parse(R"({"visualBellIntensity": 0.4})"));
}

TEST_F(ConfigTest, LimitsDefaultAndOverride)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    const Settings& d = defaults.settings();
    EXPECT_EQ(d.graphicsQuota, std::size_t(320) << 20);
    EXPECT_EQ(d.notificationRateLimit, 20);
    EXPECT_EQ(d.hintAlphabet, QStringLiteral("asdfjklghqweruiopzxcvbnmty"));
    EXPECT_EQ(d.quickTerminal.position, QuickTerminalSettings::Position::Top);
    EXPECT_TRUE(d.quickTerminal.screen.isEmpty());
    EXPECT_EQ(d.audio, AudioSettings{});
    EXPECT_EQ(d.audio.framerate, 60);
    EXPECT_EQ(d.audio.smoothing, 65);
    EXPECT_DOUBLE_EQ(d.audio.extraSmoothing, 0.2);
    EXPECT_EQ(d.audio.lowerCutoff, 50);
    EXPECT_EQ(d.audio.higherCutoff, 10000);

    SettingsStore custom(write(QStringLiteral("custom.json"), R"({
        "Graphics": {"quota": 64},
        "Notifications": {"rateLimit": 0},
        "Hints": {"alphabet": "Q W E R"},
        "QuickTerminal": {"position": "bottom", "screen": " DP-1 "},
        "Audio": {"framerate": 500, "smoothing": 30, "extraSmoothing": 0.99, "lowerCutoff": 5, "higherCutoff": 12000}
    })"));
    const Settings& c = custom.settings();
    EXPECT_EQ(c.graphicsQuota, std::size_t(64) << 20);
    EXPECT_EQ(c.terminal.graphicsQuota, std::size_t(64) << 20);
    EXPECT_EQ(c.notificationRateLimit, 0);
    EXPECT_EQ(c.hintAlphabet, QStringLiteral("qwer"));
    EXPECT_TRUE(custom.problems().filter(QStringLiteral("Hints.alphabet")).isEmpty());
    EXPECT_EQ(c.quickTerminal.position, QuickTerminalSettings::Position::Bottom);
    EXPECT_EQ(c.quickTerminal.screen, QStringLiteral("DP-1"));
    EXPECT_EQ(c.audio.framerate, 144); // clamped
    EXPECT_EQ(c.audio.smoothing, 30);
    EXPECT_DOUBLE_EQ(c.audio.extraSmoothing, 0.95); // clamped
    EXPECT_EQ(c.audio.lowerCutoff, 20); // clamped
    EXPECT_EQ(c.audio.higherCutoff, 12000);

    // Repeated letters, a single letter and non-letters fall back to the
    // default, and the user hears of it.
    for (const char* alphabet : {"aab", "a", "ab1"}) {
        SettingsStore bad(write(QStringLiteral("bad.json"), QByteArray(R"({"Hints": {"alphabet": ")") + alphabet + "\"}}"));
        EXPECT_EQ(bad.settings().hintAlphabet, QStringLiteral("asdfjklghqweruiopzxcvbnmty")) << alphabet;
        EXPECT_TRUE(mentions(bad.problems(), QStringLiteral("Hints.alphabet"))) << alphabet;
    }
    SettingsStore quotaFloor(write(QStringLiteral("quota.json"), R"({"Graphics": {"quota": 1}, "Notifications": {"rateLimit": -4}})"));
    EXPECT_EQ(quotaFloor.settings().graphicsQuota, std::size_t(16) << 20);
    EXPECT_EQ(quotaFloor.settings().notificationRateLimit, 0);
}

TEST(Config, SchemaDefaultsHaveTheirDeclaredTypes)
{
    // PhosphorConfig::Store asserts this in debug builds; release builds would not notice.
    const PhosphorConfig::Schema schema = SettingsStore::schema();
    for (auto group = schema.groups.cbegin(); group != schema.groups.cend(); ++group) {
        for (const auto& key : group.value()) {
            if (!key.defaultValue.isValid() || key.expectedType == QMetaType::UnknownType) continue;
            EXPECT_EQ(key.defaultValue.typeId(), int(key.expectedType)) << (group.key() + QLatin1Char('.') + key.key).toStdString();
        }
    }
}

TEST_F(ConfigTest, StructDefaultsAgreeWithTheSchema)
{
    SettingsStore store(write(QStringLiteral("default.json"), "{}"));
    const Settings defaults;
    EXPECT_EQ(store.settings().shaders, defaults.shaders);
    EXPECT_EQ(store.settings().hintPatterns, defaults.hintPatterns);
    EXPECT_EQ(store.settings().keybinds, defaults.keybinds);
    EXPECT_EQ(store.settings().window, defaults.window);
    EXPECT_EQ(store.settings().terminal.cursorShape, defaults.terminal.cursorShape);
    EXPECT_TRUE(store.problems().isEmpty()) << store.problems().join(QLatin1Char('\n')).toStdString();
}

TEST_F(ConfigTest, ReloadThatChangesNothingEmitsNothing)
{
    const QString path = write(QStringLiteral("config.json"), R"({"Window": {"padding": 9}})");
    SettingsStore store(path);
    int changes = 0;
    QObject::connect(&store, &SettingsStore::changed, &store, [&] { ++changes; });
    store.reload();
    EXPECT_EQ(changes, 0);
    // A write emits once; the watcher's reload of the same file adds nothing.
    ASSERT_TRUE(store.write(QStringLiteral("Window"), QStringLiteral("padding"), 12));
    store.reload();
    EXPECT_EQ(changes, 1);
    EXPECT_EQ(store.settings().window.padding, 12);
}

TEST_F(ConfigTest, WriteKeepsAFileThatDoesNotParse)
{
    const QString path = write(QStringLiteral("config.json"), R"({"Window": {"padding": 9}})");
    SettingsStore store(path);
    write(QStringLiteral("config.json"), R"({"Window": {"padding": 10)");
    EXPECT_FALSE(store.write(QStringLiteral("Shaders"), QStringLiteral("overview"), QStringLiteral("pane-deck")));
    EXPECT_EQ(read(path), QByteArray(R"({"Window": {"padding": 10)"));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("does not parse")));
}

TEST_F(ConfigTest, WriteKeepsEditsNotReloadedYetAndLeavesOverridesOut)
{
    const QString path = write(QStringLiteral("config.json"), R"({"Font": {"size": 15}})");
    QJsonObject overrides;
    ASSERT_TRUE(SettingsStore::addOverride(overrides, QStringLiteral("Window.initialRows=40")));
    SettingsStore store(path, overrides);
    // Saved within the watcher's debounce: not reloaded yet.
    write(QStringLiteral("config.json"), R"({"Font": {"size": 16}})");
    ASSERT_TRUE(store.write(QStringLiteral("Window"), QStringLiteral("padding"), 9));
    const QJsonObject file = QJsonDocument::fromJson(read(path)).object();
    EXPECT_EQ(file.value(QStringLiteral("Font")).toObject().value(QStringLiteral("size")).toInt(), 16);
    EXPECT_EQ(file.value(QStringLiteral("Window")).toObject().value(QStringLiteral("padding")).toInt(), 9);
    EXPECT_FALSE(file.value(QStringLiteral("Window")).toObject().contains(QStringLiteral("initialRows")));
    EXPECT_EQ(store.settings().window.initialRows, 40);
    EXPECT_FLOAT_EQ(store.settings().font.sizePoints, 16.0f);
}

TEST_F(ConfigTest, PersistedReadsTheFileWithoutOverrides)
{
    const QString path = write(QStringLiteral("config.json"), R"({"Shaders": {"window": ["bloom"]}})");
    QJsonObject overrides;
    ASSERT_TRUE(SettingsStore::addOverride(overrides, QStringLiteral(R"(Shaders.window=["crt"])")));
    ASSERT_TRUE(SettingsStore::addOverride(overrides, QStringLiteral("Window.padding=30")));
    SettingsStore store(path, overrides);
    EXPECT_EQ(store.settings().shaders.window, QStringList{QStringLiteral("crt")});
    EXPECT_EQ(store.persisted(QStringLiteral("Shaders"), QStringLiteral("window")).toStringList(), QStringList{QStringLiteral("bloom")});
    // Not in the file: the default, not the override.
    EXPECT_EQ(store.persisted(QStringLiteral("Window"), QStringLiteral("padding")).toInt(), WindowSettings{}.padding);
    EXPECT_FALSE(store.persisted(QStringLiteral("Window"), QStringLiteral("nosuchkey")).isValid());
}

TEST_F(ConfigTest, WriteRefusesUndeclaredKeys)
{
    const QString path = write(QStringLiteral("config.json"), "{}");
    SettingsStore store(path);
    EXPECT_FALSE(store.write(QStringLiteral("Font"), QStringLiteral("sise"), 14));
    EXPECT_EQ(read(path), QByteArray("{}"));
}

TEST_F(ConfigTest, ReloadKeepsSettingsUnlessTheFileIsGone)
{
    const QString path = write(QStringLiteral("config.json"), R"({"Window": {"padding": 9}})");
    SettingsStore store(path);
    write(QStringLiteral("config.json"), "[]");
    store.reload();
    EXPECT_EQ(store.settings().window.padding, 9);
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("JSON object")));
    // A later write must not turn the file into one key.
    EXPECT_FALSE(store.write(QStringLiteral("Window"), QStringLiteral("initialRows"), 40));
    EXPECT_EQ(read(path), QByteArray("[]"));
    // Deleted: the defaults.
    ASSERT_TRUE(QFile::remove(path));
    store.reload();
    EXPECT_EQ(store.settings().window.padding, WindowSettings{}.padding);
    EXPECT_TRUE(store.problems().isEmpty());
}

TEST_F(ConfigTest, EnsureFileCreatesAnEmptyPrivateFileOnce)
{
    const QString path = m_home.filePath(QStringLiteral("fresh/config.json"));
    SettingsStore store(path);
    const Settings before = store.settings();
    ASSERT_FALSE(QFile::exists(path));
    // Open Config needs a file for the editor to open.
    ASSERT_TRUE(store.ensureFile());
    ASSERT_TRUE(QFile::exists(path));
    const auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther;
    EXPECT_EQ(QFile::permissions(path) & others, QFileDevice::Permissions{});
    EXPECT_TRUE(QJsonDocument::fromJson(read(path)).object().isEmpty());
    store.reload();
    EXPECT_TRUE(store.settings() == before);
    EXPECT_TRUE(store.problems().isEmpty()) << store.problems().join(QLatin1Char('\n')).toStdString();
    // An existing file is left as it is.
    write(QStringLiteral("fresh/config.json"), R"({"Window": {"padding": 9}})");
    ASSERT_TRUE(store.ensureFile());
    EXPECT_EQ(read(path), QByteArray(R"({"Window": {"padding": 9}})"));
}

TEST_F(ConfigTest, ThemeEditsApplyWhileTheConfigDoesNotParse)
{
    const QString theme = write(QStringLiteral("theme.json"), R"({"foreground": "#445566"})");
    const QString config = write(QStringLiteral("config.json"),
                                 QJsonDocument(QJsonObject{{QStringLiteral("Colors"), QJsonObject{{QStringLiteral("theme"), theme}}},
                                                           {QStringLiteral("Window"), QJsonObject{{QStringLiteral("padding"), 9}}}})
                                     .toJson());
    SettingsStore store(config);
    ASSERT_TRUE(same(store.settings().terminal.defaultForeground, {0x44, 0x55, 0x66}));
    write(QStringLiteral("config.json"), "{ \"Window\": ");
    store.reload();
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("does not parse")));
    // The last good config stays, and its theme still follows its file.
    write(QStringLiteral("theme.json"), R"({"foreground": "#778899"})");
    store.reload();
    EXPECT_TRUE(same(store.settings().terminal.defaultForeground, {0x77, 0x88, 0x99}));
    EXPECT_EQ(store.settings().window.padding, 9);
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("does not parse")));
}

TEST_F(ConfigTest, BuildTreeThemesComeBeforeTheDataDirectories)
{
    // An installed copy in ~/.local/share must not shadow the checkout's
    // theme that a build-tree binary is testing. This test runs from the
    // build tree.
    ASSERT_TRUE(QDir().mkpath(m_home.filePath(QStringLiteral("data-home/afterglow/themes"))));
    write(QStringLiteral("data-home/afterglow/themes/nord.json"), R"({"background": "#010101", "foreground": "#ffffff"})");
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Colors": {"theme": "nord"}})"));
    EXPECT_TRUE(same(store.settings().terminal.defaultBackground, {0x2e, 0x34, 0x40}));
    const QStringList dirs = Themes::searchDirs();
    ASSERT_GE(dirs.size(), 3);
    // The user's own config directory still comes first.
    EXPECT_EQ(dirs.first(), m_home.path() + QStringLiteral("/afterglow/themes"));
}

TEST_F(ConfigTest, FirstRunWritesNoFile)
{
    const QString path = m_home.filePath(QStringLiteral("fresh/config.json"));
    SettingsStore store(path);
    // No file means every default; a later change of a default reaches the user.
    EXPECT_FALSE(QFile::exists(path));
    SettingsStore empty(write(QStringLiteral("empty.json"), "{}"));
    EXPECT_TRUE(store.settings() == empty.settings());
    EXPECT_TRUE(store.problems().isEmpty()) << store.problems().join(QLatin1Char('\n')).toStdString();
}

TEST_F(ConfigTest, FirstWriteCreatesAPrivateFileWithOnlyThatKey)
{
    const QString path = m_home.filePath(QStringLiteral("fresh/config.json"));
    SettingsStore store(path);
    ASSERT_TRUE(store.write(QStringLiteral("Window"), QStringLiteral("padding"), 11));
    ASSERT_TRUE(QFile::exists(path));
    const auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther;
    EXPECT_EQ(QFile::permissions(path) & others, QFileDevice::Permissions{});
    const QJsonObject file = QJsonDocument::fromJson(read(path)).object();
    // The changed key, and the schema version PhosphorConfig stamps on every file.
    EXPECT_EQ(file.keys(), (QStringList{QStringLiteral("Window"), QStringLiteral("_version")}));
    EXPECT_EQ(file.value(QStringLiteral("Window")).toObject(), (QJsonObject{{QStringLiteral("padding"), 11}}));
    EXPECT_EQ(store.settings().window.padding, 11);
    // A later save keeps the mode.
    ASSERT_TRUE(store.write(QStringLiteral("Window"), QStringLiteral("padding"), 12));
    EXPECT_EQ(QFile::permissions(path) & others, QFileDevice::Permissions{});
}

TEST_F(ConfigTest, EnumKeysTakeOnlyTheirChoicesInAnyCase)
{
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Terminal": {"cursorShape": "BAR", "bell": "Visual",
                                                                              "copyOnSelect": "none", "clipboardRead": "DENY"}})"));
    const Settings& s = store.settings();
    EXPECT_EQ(s.terminal.cursorShape, AfterglowVt::CursorShape::Bar);
    EXPECT_EQ(s.bell, Settings::Bell::Visual);
    // No aliases: "none" is not a choice, so the default.
    EXPECT_EQ(s.copyOnSelect, Settings::CopyOnSelect::Primary);
    EXPECT_EQ(s.clipboardRead, QStringLiteral("deny"));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Terminal.copyOnSelect must be one of")));
}

TEST_F(ConfigTest, MalformedValuesAndUnknownKeysAreReported)
{
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Shaders": {"pane": "bloom"}, "Window": {"tabBar": "sometimes"},
        "Colors": {"divider": "nope", "palette": ["#000000", "nope"]}, "Font": {"sise": 3}, "Fonts": {}, "Mouse": 5,
        "Animation": {"cursorCurve": "not-a-curve", "scrollCurve": " spring:30,1 "}, "QuickTerminal": {"curve": 5}})"));
    const QStringList problems = store.problems();
    EXPECT_TRUE(mentions(problems, QStringLiteral("Mouse must be an object")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Animation.cursorCurve is not a curve")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("QuickTerminal.curve is not a curve")));
    EXPECT_FALSE(mentions(problems, QStringLiteral("Animation.scrollCurve")));
    EXPECT_EQ(store.settings().animation.cursorCurve, AnimationSettings{}.cursorCurve);
    EXPECT_EQ(store.settings().animation.scrollCurve, QStringLiteral("spring:30,1"));
    EXPECT_EQ(store.settings().quickTerminal.curve, QuickTerminalSettings{}.curve);
    EXPECT_TRUE(mentions(problems, QStringLiteral("Shaders.pane must be a list")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Window.tabBar must be one of")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Colors.divider is not a color")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Colors.palette[1] is not a color")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("unknown setting Font.sise")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("unknown setting group Fonts")));
    EXPECT_TRUE(store.settings().shaders.pane.isEmpty());
}

TEST_F(ConfigTest, EightDigitColorsAreReportedAndNotUsed)
{
    SettingsStore store(write(QStringLiteral("config.json"),
                              R"({"Colors": {"foreground": "#1e1e2eff", "palette": ["#000000", "#ff0000ff"]}})"));
    const QStringList problems = store.problems();
    EXPECT_TRUE(mentions(problems, QStringLiteral("Colors.foreground is not a color")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Colors.palette[1] is not a color")));
    EXPECT_TRUE(same(store.settings().terminal.defaultForeground, Settings{}.terminal.defaultForeground));
}

TEST_F(ConfigTest, HintPatternsAreCheckedWhenRead)
{
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Hints": {"patterns": [
        {"pattern": "a+", "action": "copy"},
        {"pattern": ""},
        {"pattern": "b+", "action": "run"},
        {"pattern": "c+", "action": "command", "command": []},
        {"pattern": "d+", "action": "command", "command": ["kate", "{path}"]},
        "e+"]}})"));
    const QVariantList patterns = store.settings().hintPatterns;
    ASSERT_EQ(patterns.size(), 2);
    EXPECT_EQ(patterns[0].toMap().value(QStringLiteral("pattern")).toString(), QStringLiteral("a+"));
    EXPECT_EQ(patterns[1].toMap().value(QStringLiteral("pattern")).toString(), QStringLiteral("d+"));
    for (int i : {1, 2, 3, 5}) {
        EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Hints.patterns[%1]").arg(i))) << i;
    }

    QJsonArray many;
    for (int i = 0; i < 17; ++i) many.append(QJsonObject{{QStringLiteral("pattern"), QStringLiteral("x%1").arg(i)}});
    SettingsStore capped(write(QStringLiteral("many.json"),
                               QJsonDocument(QJsonObject{{QStringLiteral("Hints"), QJsonObject{{QStringLiteral("patterns"), many}}}}).toJson()));
    EXPECT_EQ(capped.settings().hintPatterns.size(), 16);
    EXPECT_TRUE(mentions(capped.problems(), QStringLiteral("first 16")));
}

TEST_F(ConfigTest, EnvironmentValuesMustBeStrings)
{
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Terminal": {"env": {"EDITOR": "nvim", "N": 5, "Z": null}}})"));
    ASSERT_EQ(store.settings().environment.size(), 1u);
    EXPECT_EQ(store.settings().environment[0].first, "EDITOR");
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Terminal.env.N")));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Terminal.env.Z")));
}

TEST_F(ConfigTest, FamiliesAndShaderReferencesAreTrimmed)
{
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Font": {"family": "Iosevka", "boldFamily": " ", "italicFamily": " Mono "},
                                                                 "Shaders": {"pane": [" bloom", ""]}})"));
    const Settings& s = store.settings();
    EXPECT_EQ(s.font.bold.family, "Iosevka");
    EXPECT_EQ(s.font.italic.family, "Mono");
    EXPECT_EQ(s.shaders.pane, QStringList{QStringLiteral("bloom")});
}

TEST(Overrides, RejectUnknownKeys)
{
    QJsonObject o;
    EXPECT_FALSE(SettingsStore::addOverride(o, QStringLiteral("Font.sise=14")));
    EXPECT_FALSE(SettingsStore::addOverride(o, QStringLiteral("Fonts.size=14")));
    EXPECT_TRUE(o.isEmpty());
}

TEST_F(ConfigTest, RelativeThemePathsStartAtTheConfigDirectory)
{
    QDir().mkpath(m_home.filePath(QStringLiteral("mine")));
    write(QStringLiteral("mine/theme.json"), R"({"foreground": "#010203"})");
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Colors": {"theme": "mine/theme.json"}})"));
    EXPECT_TRUE(same(store.settings().terminal.defaultForeground, {1, 2, 3}));
    EXPECT_EQ(Themes::candidates(QStringLiteral("mine/theme.json"), {}, m_home.path()),
              QStringList{m_home.filePath(QStringLiteral("mine/theme.json"))});
}

TEST_F(ConfigTest, ThemeThatStopsParsingKeepsItsColours)
{
    const QString theme = write(QStringLiteral("theme.json"), R"({"foreground": "#445566"})");
    const QString config = write(QStringLiteral("config.json"),
                                 QJsonDocument(QJsonObject{{QStringLiteral("Colors"), QJsonObject{{QStringLiteral("theme"), theme},
                                     {QStringLiteral("foreground"), QStringLiteral("#112233")}}}}).toJson());
    SettingsStore store(config);
    ASSERT_TRUE(same(store.settings().terminal.defaultForeground, {0x44, 0x55, 0x66}));
    write(QStringLiteral("theme.json"), R"({"foreground": )");
    store.reload();
    EXPECT_TRUE(same(store.settings().terminal.defaultForeground, {0x44, 0x55, 0x66}));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Keeping its last colors")));
    // Gone is gone: the config's own colours.
    ASSERT_TRUE(QFile::remove(theme));
    store.reload();
    EXPECT_TRUE(same(store.settings().terminal.defaultForeground, {0x11, 0x22, 0x33}));
}

TEST_F(ConfigTest, ThemeValuesThatAreNotColorsAreNotCalledUnusedKeys)
{
    const QString theme = write(QStringLiteral("theme.json"), R"({"foreground": "#1e1e2eff", "background": "#101010",
                                                                  "palette": ["#000000", "nope"]})");
    SettingsStore store(write(QStringLiteral("config.json"),
                              QJsonDocument(QJsonObject{{QStringLiteral("Colors"), QJsonObject{{QStringLiteral("theme"), theme}}}})
                                  .toJson()));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("values Afterglow cannot use: foreground, palette[1]")));
    EXPECT_FALSE(mentions(store.problems(), QStringLiteral("does not use")));
    EXPECT_TRUE(same(store.settings().terminal.defaultBackground, {0x10, 0x10, 0x10}));
}

TEST_F(ConfigTest, JsonThemeWithoutTheJsonExtensionReportsItsKeys)
{
    // locate() finds a bare "mine" too; the content says it is JSON.
    const QString theme = write(QStringLiteral("mine"), R"({"foreground": "#445566", "selectionBackgroud": "#111111"})");
    SettingsStore store(write(QStringLiteral("config.json"),
                              QJsonDocument(QJsonObject{{QStringLiteral("Colors"), QJsonObject{{QStringLiteral("theme"), theme}}}})
                                  .toJson()));
    EXPECT_TRUE(same(store.settings().terminal.defaultForeground, {0x44, 0x55, 0x66}));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("keys Afterglow does not use: selectionBackgroud")));
}

TEST(Themes, JsonKeysMatchInAnyCaseAndAPaletteThatIsNotAListIsInvalid)
{
    QStringList ignored;
    QStringList invalid;
    const auto theme = Themes::parse(R"({"foreground": "#000000", "VisualBellIntensity": 0.25, "palette": "#ffffff"})",
                                     nullptr, &ignored, &invalid);
    ASSERT_TRUE(theme);
    EXPECT_EQ(theme->visualBellIntensity.value_or(-1), 0.25);
    EXPECT_TRUE(ignored.isEmpty());
    EXPECT_EQ(invalid, QStringList{QStringLiteral("palette")});
    EXPECT_TRUE(Themes::isJson("\xEF\xBB\xBF  {}"));
    EXPECT_FALSE(Themes::isJson("foreground = #000000\n"));
}

TEST(Themes, ReadsByteOrderMarksAnyCasePaletteAndReportsIgnoredKeys)
{
    const auto bom = Themes::parse("\xEF\xBB\xBF{\"foreground\": \"#010203\"}");
    ASSERT_TRUE(bom);
    EXPECT_TRUE(bom->foreground == Rgb(1, 2, 3));
    const auto palette = Themes::parse(R"({"Palette": ["#0a0b0c"]})");
    ASSERT_TRUE(palette);
    EXPECT_TRUE(palette->palette[0] == Rgb(0x0a, 0x0b, 0x0c));
    QStringList ignored;
    ASSERT_TRUE(Themes::parse(R"({"foreground": "#000000", "selectionBackgroud": "#111111"})", nullptr, &ignored));
    EXPECT_EQ(ignored, QStringList{QStringLiteral("selectionBackgroud")});
}

TEST(Keybinds, ExplicitBindingsTakeKeysFromDefaults)
{
    QStringList errors;
    const Keybinds kb = Keybinds::fromMap({{QStringLiteral("split_right"), QStringLiteral("Ctrl+Shift+C")}}, &errors);
    EXPECT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
    const QKeyEvent copy = press(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(kb.match(&copy), Action::SplitRight);
    EXPECT_TRUE(kb.bindingsOf(Action::Copy).isEmpty());
    // A text binding wins over a default action too.
    const Keybinds text = Keybinds::fromMap({{QStringLiteral("text:\\x1b\\r"), QStringLiteral("Ctrl+Shift+C")}});
    EXPECT_EQ(text.match(&copy), Action::None);
    EXPECT_EQ(text.matchText(&copy), QByteArray("\x1b\r"));
}

TEST(Keybinds, TwoExplicitBindingsOfOneKeyAreAnError)
{
    QStringList errors;
    const Keybinds kb = Keybinds::fromMap({{QStringLiteral("split_right"), QStringLiteral("Ctrl+Shift+C")},
                                           {QStringLiteral("copy"), QStringLiteral("Ctrl+Shift+C")}},
                                          &errors);
    EXPECT_EQ(errors.size(), 1);
    // The first in key order keeps it.
    const QKeyEvent copy = press(Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(kb.match(&copy), Action::Copy);
}

TEST(Keybinds, BindingsMustBeStringsOrLists)
{
    QStringList errors;
    const Keybinds kb = Keybinds::fromMap({{QStringLiteral("copy"), 5}}, &errors);
    EXPECT_EQ(errors.size(), 1);
    EXPECT_TRUE(kb.bindingsOf(Action::Copy).isEmpty());
}

TEST(Keybinds, HexEscapesNeedExactlyTheirDigits)
{
    EXPECT_EQ(Keybinds::unescape(QStringLiteral("\\x41\\u0042")), QByteArray("AB"));
    EXPECT_TRUE(Keybinds::unescape(QStringLiteral("\\u0x41")).isEmpty());
    EXPECT_TRUE(Keybinds::unescape(QStringLiteral("\\x+1")).isEmpty());
    EXPECT_TRUE(Keybinds::unescape(QStringLiteral("\\x 1")).isEmpty());
}

TEST(Keybinds, ShiftTabMatchesBacktab)
{
    // Qt reports Shift+Tab as Backtab.
    const QKeyEvent backtab = press(Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(Keybinds::defaults().match(&backtab), Action::PreviousTab);
}

TEST(Keybinds, ShiftedSymbolsMatchBindingsWithoutShift)
{
    // Ctrl++ on a US layout is Ctrl+Shift+=, which Qt reports as Ctrl+Shift+Plus.
    const Keybinds kb = Keybinds::defaults();
    const QKeyEvent plus = press(Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(kb.match(&plus), Action::FontBigger);
    const QKeyEvent equalize = press(Qt::Key_Equal, Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    EXPECT_EQ(kb.match(&equalize), Action::EqualizeSplits);
    // Letters keep their Shift: Ctrl+Shift+A is not Ctrl+A.
    const Keybinds letters = Keybinds::fromMap({{QStringLiteral("new_tab"), QStringLiteral("Ctrl+A")}});
    const QKeyEvent shiftA = press(Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_NE(letters.match(&shiftA), Action::NewTab);
}

TEST(Keybinds, BindingsOfKeepTheConfigOrder)
{
    const Keybinds kb = Keybinds::fromMap({{QStringLiteral("font_bigger"), QStringList{QStringLiteral("Ctrl+Z"), QStringLiteral("Ctrl+A")}}});
    const QStringList expected{QKeySequence(QStringLiteral("Ctrl+Z")).toString(QKeySequence::NativeText),
                               QKeySequence(QStringLiteral("Ctrl+A")).toString(QKeySequence::NativeText)};
    EXPECT_EQ(kb.bindingsOf(Action::FontBigger), expected);
}

namespace {
KeyModes modesFor(const QVariantMap& bindings, const QVariantMap& tables, const QString& leader, QStringList* errors)
{
    return KeyModes::fromConfig(bindings, tables, leader, 1000, 5000, Keybinds::fromMap(bindings, errors), errors);
}
} // namespace

TEST(KeyModes, TableOutlivesAPrefixThatTimedOut)
{
    QStringList errors;
    const auto modes = modesFor({{QStringLiteral("table:resize"), QStringLiteral("Ctrl+Space, R")}},
                                {{QStringLiteral("resize"), QVariantMap{{QStringLiteral("split_right"), QStringLiteral("G, G")}}}}, {}, &errors);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
    KeyModes::State state;
    const QKeyEvent leader = press(Qt::Key_Space, Qt::ControlModifier);
    const QKeyEvent r = press(Qt::Key_R);
    const QKeyEvent g = press(Qt::Key_G);
    modes.press(&leader, state, 0);
    modes.press(&r, state, 100);
    EXPECT_TRUE(modes.press(&g, state, 200).consumed);
    EXPECT_EQ(state.deadline, 1200); // the prefix runs out first
    KeyModes::expire(state, 1300);
    EXPECT_TRUE(state.prefix.isEmpty());
    EXPECT_EQ(state.table, QStringLiteral("resize"));
    EXPECT_EQ(state.deadline, 5200);
    modes.press(&g, state, 1400);
    EXPECT_EQ(modes.press(&g, state, 1500).target, QStringLiteral("split_right"));
    EXPECT_EQ(state.table, QStringLiteral("resize"));
}

TEST(KeyModes, ModifierAndLockKeysKeepAPendingSequence)
{
    QStringList errors;
    const auto modes = modesFor({{QStringLiteral("split_right"), QStringLiteral("Leader, E")}}, {}, QStringLiteral("Ctrl+Space"), &errors);
    KeyModes::State state;
    const QKeyEvent leader = press(Qt::Key_Space, Qt::ControlModifier);
    modes.press(&leader, state, 0);
    for (int key : {Qt::Key_AltGr, Qt::Key_Super_L, Qt::Key_CapsLock, Qt::Key_NumLock}) {
        const QKeyEvent modifier = press(key);
        EXPECT_TRUE(modes.press(&modifier, state, 10).consumed) << key;
        EXPECT_TRUE(state.active()) << key;
    }
    const QKeyEvent e = press(Qt::Key_E);
    EXPECT_EQ(modes.press(&e, state, 20).target, QStringLiteral("split_right"));
}

TEST(KeyModes, ShiftedSymbolsAndKeypadKeysMatchSequences)
{
    QStringList errors;
    const auto modes = modesFor({{QStringLiteral("split_right"), QStringLiteral("Leader, %")},
                                 {QStringLiteral("split_down"), QStringLiteral("Leader, Num+1")}},
                                {}, QStringLiteral("Ctrl+Space"), &errors);
    EXPECT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
    KeyModes::State state;
    const QKeyEvent leader = press(Qt::Key_Space, Qt::ControlModifier);
    const QKeyEvent percent = press(Qt::Key_Percent, Qt::ShiftModifier);
    const QKeyEvent one = press(Qt::Key_1, Qt::KeypadModifier);
    modes.press(&leader, state, 0);
    EXPECT_EQ(modes.press(&percent, state, 10).target, QStringLiteral("split_right"));
    modes.press(&leader, state, 20);
    EXPECT_EQ(modes.press(&one, state, 30).target, QStringLiteral("split_down"));
}

TEST(KeyModes, PopKeyTableOutsideATableIsAnError)
{
    QStringList errors;
    const auto modes = modesFor({{QStringLiteral("pop_key_table"), QStringLiteral("Escape")}}, {}, {}, &errors);
    EXPECT_TRUE(mentions(errors, QStringLiteral("pop_key_table works only inside a key table")));
    KeyModes::State state;
    const QKeyEvent escape = press(Qt::Key_Escape);
    EXPECT_FALSE(modes.press(&escape, state, 0).consumed);
}

TEST(KeyModes, BareLeaderInAnyCaseBindsAnAction)
{
    QStringList errors;
    const auto modes = modesFor({{QStringLiteral("command_palette"), QStringLiteral("leader")},
                                 {QStringLiteral("split_right"), QStringLiteral(" LEADER, E")}},
                                {}, QStringLiteral("Ctrl+B"), &errors);
    // A bare Leader and a Leader sequence share their first key.
    EXPECT_TRUE(mentions(errors, QStringLiteral("ambiguous")));
    QStringList single;
    const auto alone = modesFor({{QStringLiteral("command_palette"), QStringLiteral("Leader")}}, {}, QStringLiteral("Ctrl+B"), &single);
    EXPECT_TRUE(single.isEmpty()) << single.join(QLatin1Char('\n')).toStdString();
    KeyModes::State state;
    const QKeyEvent leader = press(Qt::Key_B, Qt::ControlModifier);
    EXPECT_EQ(alone.press(&leader, state, 0).target, QStringLiteral("command_palette"));

    QStringList lower;
    const auto sequence = modesFor({{QStringLiteral("split_right"), QStringLiteral(" leader, E")}}, {}, QStringLiteral("Ctrl+B"), &lower);
    EXPECT_TRUE(lower.isEmpty()) << lower.join(QLatin1Char('\n')).toStdString();
    const QKeyEvent e = press(Qt::Key_E);
    sequence.press(&leader, state, 0);
    EXPECT_EQ(sequence.press(&e, state, 10).target, QStringLiteral("split_right"));
    EXPECT_EQ(sequence.bindingsOf(QStringLiteral("split_right")),
              QStringList{QKeySequence(QStringLiteral("Ctrl+B, E")).toString(QKeySequence::NativeText)});
}

TEST(KeyModes, SequencesOfMoreThanFourKeysAreRejected)
{
    QStringList errors;
    modesFor({{QStringLiteral("split_right"), QStringLiteral("Ctrl+A, B, C, D, E")},
              {QStringLiteral("split_down"), QStringLiteral("Ctrl+A, B, C, Ctrl+,")}},
             {}, {}, &errors);
    ASSERT_EQ(errors.size(), 1) << errors.join(QLatin1Char('\n')).toStdString();
    EXPECT_TRUE(errors[0].contains(QStringLiteral("more than 4 keys")));
}

TEST(KeyModes, RootMistakesAreReportedOnce)
{
    for (const QVariantMap& bindings : {QVariantMap{{QStringLiteral("no_such_action"), QStringLiteral("Ctrl+X")}},
                                        QVariantMap{{QStringLiteral("copy"), QStringLiteral("Ctrl+Nope")}},
                                        QVariantMap{{QStringLiteral("text:\\z"), QStringLiteral("Ctrl+Return")}}}) {
        QStringList errors;
        modesFor(bindings, {}, {}, &errors);
        EXPECT_EQ(errors.size(), 1) << errors.join(QLatin1Char('\n')).toStdString();
    }
}

TEST(KeyModes, TablesCanBindEscapeAndHoldingTheirKeyTypesNothing)
{
    QStringList errors;
    const auto modes = modesFor({{QStringLiteral("table:resize"), QStringLiteral("Ctrl+Space, R")}},
                                {{QStringLiteral("resize"), QVariantMap{{QStringLiteral("split_right"), QStringLiteral("Escape")},
                                                                        {QStringLiteral("resize_left"), QStringLiteral("H")}}}},
                                {}, &errors);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
    KeyModes::State state;
    const QKeyEvent leader = press(Qt::Key_Space, Qt::ControlModifier);
    const QKeyEvent r = press(Qt::Key_R);
    const QKeyEvent heldR = press(Qt::Key_R, Qt::NoModifier, true);
    const QKeyEvent escape = press(Qt::Key_Escape);
    const QKeyEvent heldH = press(Qt::Key_H, Qt::NoModifier, true);
    modes.press(&leader, state, 0);
    modes.press(&r, state, 10);
    EXPECT_TRUE(modes.press(&heldR, state, 20).consumed);
    EXPECT_EQ(state.table, QStringLiteral("resize"));
    // A held table key repeats its action.
    EXPECT_EQ(modes.press(&heldH, state, 30).target, QStringLiteral("resize_left"));
    EXPECT_EQ(modes.press(&escape, state, 40).target, QStringLiteral("split_right"));
    EXPECT_EQ(state.table, QStringLiteral("resize"));
}

TEST(Keybinds, TheCallerGivesTheKeyOnTheLatinLayout)
{
    // Ctrl+Shift+С on a Russian layout: Qt reports the Cyrillic letter, and
    // the caller, which reads the keymap, gives the Latin c of that key.
    const QKeyEvent cyrillic = press(0x0421, Qt::ControlModifier | Qt::ShiftModifier);
    const Keybinds kb = Keybinds::defaults();
    EXPECT_EQ(kb.match(&cyrillic), Action::None);
    EXPECT_EQ(kb.match(&cyrillic, U'c'), Action::Copy);
    EXPECT_TRUE(Keybinds::combinations(&cyrillic, U'c').contains(
        QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_C).toCombined()));
}
