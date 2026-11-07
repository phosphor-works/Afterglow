// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/Settings.h"
#include "config/Theme.h"
#include "desktop/DragDrop.h"
#include "desktop/FileTransfer.h"
#include "scripting/ScriptHost.h"

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
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include <unistd.h>

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
    static constexpr const char* Variables[] = {"XDG_CONFIG_HOME", "XDG_CONFIG_DIRS", "XDG_DATA_HOME", "XDG_DATA_DIRS"};

    void SetUp() override
    {
        ASSERT_TRUE(m_home.isValid());
        for (const char* name : Variables) {
            m_saved.append({name, qEnvironmentVariableIsSet(name), qgetenv(name)});
        }
        qputenv("XDG_CONFIG_HOME", m_home.path().toUtf8());
        // No system kdeglobals: a plasma theme reads only what a test writes.
        qputenv("XDG_CONFIG_DIRS", m_home.filePath(QStringLiteral("config-dirs")).toUtf8());
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
        EXPECT_TRUE(f.open(QIODevice::WriteOnly)) << path.toStdString();
        // A short write would leave the test reading a file it did not mean.
        EXPECT_EQ(f.write(content), content.size()) << path.toStdString();
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

TEST(Themes, ReadsKonsoleColorSchemes)
{
    const QByteArray scheme = "[Background]\nColor=35,38,39\n\n"
                              "[BackgroundFaint]\nColor=49,54,59\n\n"
                              "[Color0]\nColor=35,38,39\n\n"
                              "[Color1]\nColor=237,21,21\n\n"
                              "[Color1Intense]\nColor=192,57,43\n\n"
                              "[Color7Intense]\nColor=255,255,255\n\n"
                              "[Foreground]\nColor=252,252,252\n\n"
                              "[ForegroundIntense]\nColor=255,255,255\n\n"
                              "[General]\nDescription=Breeze\nOpacity=1\nWallpaper=\n";
    EXPECT_TRUE(Themes::isKonsole(scheme));
    EXPECT_FALSE(Themes::isKonsole("background = #000000\n"));
    QStringList ignored;
    const auto theme = Themes::parse(scheme, nullptr, &ignored);
    ASSERT_TRUE(theme);
    EXPECT_TRUE(theme->background == Rgb(35, 38, 39));
    EXPECT_TRUE(theme->foreground == Rgb(252, 252, 252));
    EXPECT_TRUE(theme->palette[0] == Rgb(35, 38, 39));
    EXPECT_TRUE(theme->palette[1] == Rgb(237, 21, 21));
    // ColorNIntense is the bright colour N + 8.
    EXPECT_TRUE(theme->palette[9] == Rgb(192, 57, 43));
    EXPECT_TRUE(theme->palette[15] == Rgb(255, 255, 255));
    EXPECT_FALSE(theme->palette[2].has_value());
    EXPECT_FALSE(theme->cursor.has_value());
    // Faint and intense text colours have no Afterglow counterpart.
    EXPECT_TRUE(ignored.contains(QStringLiteral("BackgroundFaint")));
    EXPECT_TRUE(ignored.contains(QStringLiteral("ForegroundIntense")));
    QStringList invalid;
    EXPECT_FALSE(Themes::parse("[Color3]\nColor=300,0,0\n", nullptr, nullptr, &invalid));
    EXPECT_EQ(invalid, QStringList{QStringLiteral("Color3")});
}

TEST(Themes, PlasmaNamesTakeAnOptionalBaseTheme)
{
    QString base = QStringLiteral("unset");
    EXPECT_TRUE(Themes::isPlasma(QStringLiteral("plasma"), &base));
    EXPECT_TRUE(base.isEmpty());
    EXPECT_TRUE(Themes::isPlasma(QStringLiteral("plasma:nord"), &base));
    EXPECT_EQ(base, QStringLiteral("nord"));
    // In any case, as light: and dark: are.
    EXPECT_TRUE(Themes::isPlasma(QStringLiteral("Plasma:Nord"), &base));
    EXPECT_EQ(base, QStringLiteral("Nord"));
    EXPECT_TRUE(Themes::isPlasma(QStringLiteral("PLASMA")));
    EXPECT_FALSE(Themes::isPlasma(QStringLiteral("plasmatic")));
    EXPECT_FALSE(Themes::isPlasma(QStringLiteral("nord")));
    // Each half of a light:/dark: spec may be a plasma theme of its own.
    EXPECT_EQ(Themes::pick(QStringLiteral("light:plasma:paper,dark:plasma"), false), QStringLiteral("plasma:paper"));
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

namespace {
std::optional<Theme> bundled(const char* name)
{
    QFile file(Themes::locate(QString::fromLatin1(name), Themes::searchDirs()));
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    return Themes::parse(file.readAll());
}
const QByteArray DarkPlasma = "[Colors:View]\nBackgroundNormal=20,22,24\nForegroundNormal=230,231,232\n"
                              "DecorationFocus=61,174,233\n\n[Colors:Selection]\nBackgroundNormal=30,90,150\n"
                              "ForegroundNormal=250,251,252\n";
} // namespace

TEST_F(ConfigTest, KonsoleColorSchemesAreFoundByNameAfterAfterglowThemes)
{
    QDir().mkpath(m_home.filePath(QStringLiteral("data-home/konsole")));
    write(QStringLiteral("data-home/konsole/Breezy.colorscheme"), "[Background]\nColor=1,2,3\n[Foreground]\nColor=4,5,6\n");
    const QString config = write(QStringLiteral("config.json"), R"({"Colors": {"theme": "Breezy"}})");
    {
        SettingsStore store(config);
        EXPECT_TRUE(same(store.settings().terminal.defaultBackground, {1, 2, 3}));
        EXPECT_TRUE(store.problems().isEmpty()) << store.problems().join(QLatin1Char('\n')).toStdString();
    }
    // An Afterglow theme of the same name comes first.
    QDir().mkpath(m_home.filePath(QStringLiteral("afterglow/themes")));
    write(QStringLiteral("afterglow/themes/Breezy.json"), R"({"background": "#0a0b0c"})");
    SettingsStore store(config);
    EXPECT_TRUE(same(store.settings().terminal.defaultBackground, {0x0a, 0x0b, 0x0c}));
}

TEST_F(ConfigTest, GhosttyAndKittyThemesAreFoundInTheirOwnDirectories)
{
    // Ghostty's user themes, the ones it ships (share/ghostty/themes, no
    // extension) and Kitty's custom themes (NAME.conf).
    QDir().mkpath(m_home.filePath(QStringLiteral("ghostty/themes")));
    QDir().mkpath(m_home.filePath(QStringLiteral("data-dirs/ghostty/themes")));
    QDir().mkpath(m_home.filePath(QStringLiteral("kitty/themes")));
    write(QStringLiteral("ghostty/themes/Mine"), "background = #010203\n");
    write(QStringLiteral("data-dirs/ghostty/themes/Shipped Night"), "background = #040506\n");
    write(QStringLiteral("kitty/themes/Paper.conf"), "background #070809\n");
    auto backgroundOf = [&](const char* theme) {
        const QString config = write(QStringLiteral("config.json"),
                                     QJsonDocument(QJsonObject{{QStringLiteral("Colors"),
                                                                QJsonObject{{QStringLiteral("theme"), QString::fromUtf8(theme)}}}})
                                         .toJson());
        SettingsStore store(config);
        EXPECT_TRUE(store.problems().isEmpty()) << theme << ": " << store.problems().join(QLatin1Char('\n')).toStdString();
        return store.settings().terminal.defaultBackground;
    };
    EXPECT_TRUE(same(backgroundOf("Mine"), {1, 2, 3}));
    EXPECT_TRUE(same(backgroundOf("Shipped Night"), {4, 5, 6}));
    EXPECT_TRUE(same(backgroundOf("Paper"), {7, 8, 9}));
}

TEST_F(ConfigTest, PlasmaThemeTakesTheDesktopColorsOverTheNamedTheme)
{
    write(QStringLiteral("kdeglobals"), DarkPlasma);
    const QString config = write(QStringLiteral("config.json"), R"({"Colors": {"theme": "plasma:afterglow-light"}})");
    SettingsStore store(config);
    const Settings& s = store.settings();
    EXPECT_TRUE(same(s.terminal.defaultBackground, {20, 22, 24}));
    EXPECT_TRUE(same(s.terminal.defaultForeground, {230, 231, 232}));
    EXPECT_TRUE(s.terminal.selectionBackground == Rgb(30, 90, 150));
    EXPECT_TRUE(s.terminal.selectionForeground == Rgb(250, 251, 252));
    EXPECT_TRUE(s.terminal.cursorColor == Rgb(61, 174, 233));
    EXPECT_TRUE(s.terminal.cursorTextColor == Rgb(20, 22, 24));
    // The 16 colours are the named theme's.
    const auto light = bundled("afterglow-light");
    ASSERT_TRUE(light && light->palette[1]);
    EXPECT_TRUE(same(s.terminal.basePalette[1], *light->palette[1]));
    // The divider comes from the desktop's background and text.
    auto mix = [](int a, int b) { return a + (b - a) / 5; };
    EXPECT_EQ(s.window.dividerColor, AfterglowRender::packRgba(mix(20, 230), mix(22, 231), mix(24, 232)));
    EXPECT_TRUE(store.problems().isEmpty()) << store.problems().join(QLatin1Char('\n')).toStdString();
    EXPECT_TRUE(store.watchedPaths().contains(m_home.filePath(QStringLiteral("kdeglobals"))));
}

TEST_F(ConfigTest, PlasmaColorsReadHexValuesTheViewSectionAndTheSystemFile)
{
    // As Plasma writes it today: hex values, and the same keys in other sections.
    write(QStringLiteral("kdeglobals"), "[Colors:Button]\nBackgroundNormal=#ff0000\nForegroundNormal=#00ff00\n\n"
                                        "[Colors:View]\nBackgroundAlternate=#071C33\nBackgroundNormal=#2E4159\n"
                                        "DecorationFocus=#80B9FF\nForegroundNormal=#e2e2e6\n");
    // The system's file under the user's: it supplies what the user's leaves out.
    QDir().mkpath(m_home.filePath(QStringLiteral("config-dirs")));
    write(QStringLiteral("config-dirs/kdeglobals"), "[Colors:View]\nBackgroundNormal=1,1,1\n\n"
                                                    "[Colors:Selection]\nBackgroundNormal=61,174,233,255\nForegroundNormal=252,252,252\n");
    const QStringList sources = Themes::plasmaSources();
    ASSERT_EQ(sources.size(), 2);
    EXPECT_EQ(sources.last(), m_home.filePath(QStringLiteral("kdeglobals")));
    const Theme theme = Themes::plasmaColors(sources);
    EXPECT_TRUE(theme.background == Rgb(0x2e, 0x41, 0x59));
    EXPECT_TRUE(theme.foreground == Rgb(0xe2, 0xe2, 0xe6));
    EXPECT_TRUE(theme.cursor == Rgb(0x80, 0xb9, 0xff));
    EXPECT_TRUE(theme.selectionBackground == Rgb(61, 174, 233));
    EXPECT_TRUE(theme.selectionForeground == Rgb(252, 252, 252));
}

TEST_F(ConfigTest, PlasmaThemeAloneTakesTheColorsOfTheSchemesBrightness)
{
    const QString config = write(QStringLiteral("config.json"), R"({"Colors": {"theme": "plasma"}})");
    const auto dark = bundled("afterglow-dark");
    const auto light = bundled("afterglow-light");
    ASSERT_TRUE(dark && light && dark->palette[4] && light->palette[4]);
    write(QStringLiteral("kdeglobals"), DarkPlasma);
    {
        SettingsStore store(config);
        EXPECT_TRUE(same(store.settings().terminal.basePalette[4], *dark->palette[4]));
    }
    write(QStringLiteral("kdeglobals"), "[Colors:View]\nBackgroundNormal=252,252,252\nForegroundNormal=35,38,41\n");
    SettingsStore store(config);
    EXPECT_TRUE(same(store.settings().terminal.basePalette[4], *light->palette[4]));
    EXPECT_TRUE(same(store.settings().terminal.defaultBackground, {252, 252, 252}));
    // The watch follows the theme that was read, not another read of kdeglobals.
    const QStringList watched = store.watchedPaths();
    auto watches = [&](const QString& file) {
        return std::any_of(watched.begin(), watched.end(), [&](const QString& p) { return p.endsWith(file); });
    };
    EXPECT_TRUE(watches(QStringLiteral("/afterglow-light.json")));
    EXPECT_FALSE(watches(QStringLiteral("/afterglow-dark.json")));
    EXPECT_TRUE(watched.contains(m_home.filePath(QStringLiteral("kdeglobals"))));
}

TEST_F(ConfigTest, PlasmaThemeFollowsAChangedColorScheme)
{
    int argc = 1;
    char name[] = "config-test";
    char* argv[] = {name, nullptr};
    std::unique_ptr<QCoreApplication> app;
    if (!QCoreApplication::instance()) app = std::make_unique<QCoreApplication>(argc, argv);
    write(QStringLiteral("kdeglobals"), DarkPlasma);
    const QString config = write(QStringLiteral("config.json"), R"({"Colors": {"theme": "plasma:afterglow-dark"}})");
    SettingsStore store(config);
    ASSERT_TRUE(same(store.settings().terminal.defaultBackground, {20, 22, 24}));
    // Plasma rewrites kdeglobals when the user picks another scheme.
    QSaveFile file(m_home.filePath(QStringLiteral("kdeglobals")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("[Colors:View]\nBackgroundNormal=1,2,3\nForegroundNormal=200,200,200\n");
    ASSERT_TRUE(file.commit());
    QElapsedTimer timer;
    timer.start();
    while (!same(store.settings().terminal.defaultBackground, {1, 2, 3}) && timer.elapsed() < 2500) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    EXPECT_TRUE(same(store.settings().terminal.defaultBackground, {1, 2, 3}));
}

TEST_F(ConfigTest, PlasmaThemeRereadsTheColorsOnANewApplicationPalette)
{
    int argc = 1;
    char name[] = "config-test";
    char* argv[] = {name, nullptr};
    std::unique_ptr<QCoreApplication> app;
    if (!QCoreApplication::instance()) app = std::make_unique<QCoreApplication>(argc, argv);
    write(QStringLiteral("kdeglobals"), DarkPlasma);
    const QString config = write(QStringLiteral("config.json"), R"({"Colors": {"theme": "plasma:afterglow-dark"}})");
    SettingsStore store(config);
    ASSERT_TRUE(same(store.settings().terminal.defaultBackground, {20, 22, 24}));
    // Before the file watcher's debounce: only the palette event reads it now.
    write(QStringLiteral("kdeglobals"), "[Colors:View]\nBackgroundNormal=7,8,9\nForegroundNormal=200,200,200\n");
    QEvent event(QEvent::ApplicationPaletteChange);
    QCoreApplication::sendEvent(QCoreApplication::instance(), &event);
    QCoreApplication::processEvents();
    EXPECT_TRUE(same(store.settings().terminal.defaultBackground, {7, 8, 9}));
}

TEST_F(ConfigTest, TheDesktopEventsOfOneChangeReadOnce)
{
    int argc = 1;
    char name[] = "config-test";
    char* argv[] = {name, nullptr};
    std::unique_ptr<QCoreApplication> app;
    if (!QCoreApplication::instance()) app = std::make_unique<QCoreApplication>(argc, argv);
    write(QStringLiteral("kdeglobals"), DarkPlasma);
    // A plasma theme and the desktop font: both kinds of event pass the filter.
    const QString config = write(QStringLiteral("config.json"), R"({"Colors": {"theme": "plasma:afterglow-dark"}})");
    SettingsStore store(config);
    ASSERT_TRUE(same(store.settings().terminal.defaultBackground, {20, 22, 24}));
    int changes = 0;
    QObject::connect(&store, &SettingsStore::changed, &store, [&] { ++changes; });
    QEvent palette(QEvent::ApplicationPaletteChange);
    QEvent font(QEvent::ApplicationFontChange);
    // Qt delivers a desktop change as several events in a row. The read
    // waits until they are all delivered.
    write(QStringLiteral("kdeglobals"), "[Colors:View]\nBackgroundNormal=7,8,9\nForegroundNormal=200,200,200\n");
    QCoreApplication::sendEvent(QCoreApplication::instance(), &palette);
    EXPECT_TRUE(same(store.settings().terminal.defaultBackground, {20, 22, 24}));
    write(QStringLiteral("kdeglobals"), "[Colors:View]\nBackgroundNormal=1,2,3\nForegroundNormal=200,200,200\n");
    QCoreApplication::sendEvent(QCoreApplication::instance(), &font);
    QCoreApplication::sendEvent(QCoreApplication::instance(), &palette);
    QCoreApplication::processEvents();
    EXPECT_TRUE(same(store.settings().terminal.defaultBackground, {1, 2, 3}));
    EXPECT_EQ(changes, 1);
    // An event after the read queues the next one.
    write(QStringLiteral("kdeglobals"), "[Colors:View]\nBackgroundNormal=4,5,6\nForegroundNormal=200,200,200\n");
    QCoreApplication::sendEvent(QCoreApplication::instance(), &palette);
    QCoreApplication::processEvents();
    EXPECT_TRUE(same(store.settings().terminal.defaultBackground, {4, 5, 6}));
    EXPECT_EQ(changes, 2);
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
    EXPECT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
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
    // The choices are the ones that the store reads, in their order. A key
    // with a narrower set than its type (graphicsFileTransfer has no ask)
    // lists only its own.
    EXPECT_EQ(font.value(QStringLiteral("antialiasing")).toObject().value(QStringLiteral("choices")).toArray(),
              (QJsonArray{QStringLiteral("grayscale"), QStringLiteral("none"), QStringLiteral("rgb"), QStringLiteral("bgr")}));
    const auto transfer = groups.value(QStringLiteral("Terminal")).toObject().value(QStringLiteral("graphicsFileTransfer")).toObject();
    EXPECT_EQ(transfer.value(QStringLiteral("choices")).toArray(), (QJsonArray{QStringLiteral("allow"), QStringLiteral("deny")}));
    // A key without choices has no list.
    EXPECT_FALSE(font.value(QStringLiteral("family")).toObject().contains(QStringLiteral("choices")));
    // 0 is the desktop's size; other sizes are clamped to 4–96, the limits
    // font_bigger keeps to. The range shows what the clamp keeps to, and the
    // description names 0.
    EXPECT_EQ(font.value(QStringLiteral("size")).toObject().value(QStringLiteral("minimum")).toInt(), 4);
    EXPECT_EQ(font.value(QStringLiteral("size")).toObject().value(QStringLiteral("maximum")).toInt(), 96);
    EXPECT_EQ(AfterglowRender::FontSpec::MinPoints, 4.0f);
    EXPECT_EQ(AfterglowRender::FontSpec::MaxPoints, 96.0f);
    EXPECT_TRUE(font.value(QStringLiteral("size")).toObject().value(QStringLiteral("description")).toString().contains(QStringLiteral("clamps other values to 4–96")));
    EXPECT_TRUE(font.value(QStringLiteral("family")).toObject().value(QStringLiteral("default")).toString().isEmpty());
    EXPECT_TRUE(groups.value(QStringLiteral("Keybinds")).toObject().value(QStringLiteral("bindings")).toObject().value(QStringLiteral("default")).isObject());
}

// The reference says that an integer key drops the fraction. The JSON
// backend truncates before the clamp sees the value, so 6.7 is 6, not 7 as
// a rounding conversion would give. A fraction in a string is no whole
// number: it is reported and uses the default.
TEST_F(ConfigTest, AnIntegerKeyDropsTheFractionOfANumber)
{
    SettingsStore up(write(QStringLiteral("up.json"), R"({"Notifications": {"statusInterval": 6.7}})"));
    EXPECT_EQ(up.settings().statusNotificationInterval, 6);
    EXPECT_FALSE(mentions(up.problems(), QStringLiteral("Notifications.statusInterval")));

    SettingsStore text(write(QStringLiteral("text.json"), R"({"Notifications": {"statusInterval": "6.7"}})"));
    EXPECT_EQ(text.settings().statusNotificationInterval, Settings{}.statusNotificationInterval);
    EXPECT_TRUE(mentions(text.problems(), QStringLiteral("Notifications.statusInterval must be a whole number")));
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

TEST_F(ConfigTest, ScriptCommandLimitsHaveDefaultsOverridesAndRanges)
{
    // The two byte limits are KiB in the file and bytes in Settings, as
    // Terminal.lastOutputLimit.
    const auto path = write(QStringLiteral("commands.json"), "{}");
    const Settings defaults = SettingsStore(path).settings();
    EXPECT_EQ(defaults.scriptCommandTimeout, 600000);
    EXPECT_EQ(defaults.scriptCommandStdinLimit, qint64(1024) << 10);
    EXPECT_EQ(defaults.scriptCommandRunLimit, 16);
    EXPECT_EQ(defaults.scriptCommandOutputLimit, qint64(16384) << 10);
    const auto scripts = SettingsStore::reference().value(QStringLiteral("groups")).toObject().value(QStringLiteral("Scripts")).toObject();
    EXPECT_EQ(scripts.value(QStringLiteral("commandStdinLimit")).toObject().value(QStringLiteral("default")).toInt(), 1024);
    EXPECT_EQ(scripts.value(QStringLiteral("commandOutputLimit")).toObject().value(QStringLiteral("default")).toInt(), 16384);
    const Settings set = SettingsStore(path, QJsonObject{{QStringLiteral("Scripts"), QJsonObject{
        {QStringLiteral("commandTimeout"), 5000}, {QStringLiteral("commandStdinLimit"), 0},
        {QStringLiteral("commandRunLimit"), 2}, {QStringLiteral("commandOutputLimit"), 100}}}}).settings();
    EXPECT_EQ(set.scriptCommandTimeout, 5000);
    EXPECT_EQ(set.scriptCommandStdinLimit, 0); // refuses any input
    EXPECT_EQ(set.scriptCommandRunLimit, 2);
    EXPECT_EQ(set.scriptCommandOutputLimit, qint64(100) << 10);
    const Settings low = SettingsStore(path, QJsonObject{{QStringLiteral("Scripts"), QJsonObject{
        {QStringLiteral("commandTimeout"), 10}, {QStringLiteral("commandStdinLimit"), -5},
        {QStringLiteral("commandRunLimit"), 0}, {QStringLiteral("commandOutputLimit"), 1}}}}).settings();
    EXPECT_EQ(low.scriptCommandTimeout, 1000);
    EXPECT_EQ(low.scriptCommandStdinLimit, 0);
    EXPECT_EQ(low.scriptCommandRunLimit, 1);
    EXPECT_EQ(low.scriptCommandOutputLimit, qint64(64) << 10);
    // Stdin stays well below the 16 MiB heap of a script, and runs at the
    // 16 waiting handlers of a script.
    const Settings high = SettingsStore(path, QJsonObject{{QStringLiteral("Scripts"), QJsonObject{
        {QStringLiteral("commandTimeout"), 1 << 30}, {QStringLiteral("commandStdinLimit"), 1 << 20},
        {QStringLiteral("commandRunLimit"), 256}, {QStringLiteral("commandOutputLimit"), 1 << 30}}}}).settings();
    EXPECT_EQ(high.scriptCommandTimeout, 3600000);
    EXPECT_EQ(high.scriptCommandStdinLimit, qint64(4096) << 10);
    EXPECT_EQ(high.scriptCommandRunLimit, 16);
    EXPECT_EQ(high.scriptCommandOutputLimit, qint64(262144) << 10);
    // A value of the wrong type is reported and uses the default.
    SettingsStore wrong(path, QJsonObject{{QStringLiteral("Scripts"), QJsonObject{
        {QStringLiteral("commandStdinLimit"), QStringLiteral("lots")}, {QStringLiteral("commandRunLimit"), true}}}});
    EXPECT_EQ(wrong.settings().scriptCommandStdinLimit, defaults.scriptCommandStdinLimit);
    EXPECT_TRUE(mentions(wrong.problems(), QStringLiteral("Scripts.commandStdinLimit must be a whole number")));
}

TEST(Config, ScriptCommandLimitsOfTheHostMatchTheSettingDefaults)
{
    // ScriptHost starts with its own limits until Application hands it the
    // settings: both must be the documented defaults.
    const ScriptHost::CommandLimits limits;
    const Settings settings;
    EXPECT_EQ(limits.timeoutMs, settings.scriptCommandTimeout);
    EXPECT_EQ(limits.inputBytes, settings.scriptCommandStdinLimit);
    EXPECT_EQ(limits.runs, settings.scriptCommandRunLimit);
    EXPECT_EQ(limits.outputBytes, settings.scriptCommandOutputLimit);
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

TEST(Keybinds, CancelOverlayHasADefaultChord)
{
    const Keybinds kb = Keybinds::defaults();
    EXPECT_EQ(Keybinds::token(Action::CancelOverlay), QStringLiteral("cancel_overlay"));
    EXPECT_EQ(Keybinds::defaultMap().value(QStringLiteral("cancel_overlay")).toString(), QStringLiteral("Ctrl+Shift+Alt+Escape"));
    const QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    EXPECT_EQ(kb.match(&press), Action::CancelOverlay);
    // The chord without Alt still ends a script's key mode.
    const QKeyEvent endMode(QEvent::KeyPress, Qt::Key_Escape, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(kb.match(&endMode), Action::EndScriptKeyMode);
    const Keybinds moved = Keybinds::fromMap({{QStringLiteral("cancel_overlay"), QStringLiteral("Ctrl+Alt+Q")}});
    EXPECT_EQ(moved.match(&press), Action::None);
    const QKeyEvent other(QEvent::KeyPress, Qt::Key_Q, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_EQ(moved.match(&other), Action::CancelOverlay);
}

TEST_F(ConfigTest, FontFollowsTheDesktopUntilSet)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    const AfterglowRender::FontSpec& d = defaults.settings().font;
    EXPECT_FALSE(d.regular.family.empty());
    EXPECT_EQ(d.bold.family, d.regular.family);
    EXPECT_GT(d.sizePoints, 0.0f);
    // qGuiApp is a cast that a QCoreApplication passes too.
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
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

TEST_F(ConfigTest, DoubleKeysRejectNonFiniteValues)
{
    const QString path = write(QStringLiteral("config.json"), "{}");
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

TEST_F(ConfigTest, OverlayTransitionAndFadeDefaultsOverridesAndClamps)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    EXPECT_EQ(defaults.settings().shaders.overlayTransition, QStringLiteral("overlay-fade"));
    EXPECT_EQ(defaults.settings().animation.overlayCurve, QStringLiteral("0.4,0,0.2,1"));
    EXPECT_EQ(defaults.settings().animation.overlayDuration, 180);

    SettingsStore custom(write(QStringLiteral("custom.json"), R"({"Animation": {"overlayCurve": " spring:20,1 ", "overlayDuration": 600}})"));
    EXPECT_EQ(custom.settings().animation.overlayCurve, QStringLiteral("spring:20,1"));
    EXPECT_EQ(custom.settings().animation.overlayDuration, 600);

    // An invalid curve is the default, not a curve that the fade cannot run.
    SettingsStore invalid(write(QStringLiteral("invalid.json"), R"({"Animation": {"overlayCurve": "not-a-curve"}})"));
    EXPECT_EQ(invalid.settings().animation.overlayCurve, QStringLiteral("0.4,0,0.2,1"));
    EXPECT_TRUE(mentions(invalid.problems(), QStringLiteral("Animation.overlayCurve is not a curve")));

    // The setting keeps an empty reference and a package's <package>.<pack>
    // as written (ShaderControlsTest resolves them into the chain).
    SettingsStore none(write(QStringLiteral("none.json"), R"({"Shaders": {"overlayTransition": ""}})"));
    EXPECT_TRUE(none.settings().shaders.overlayTransition.isEmpty());
    SettingsStore packaged(write(QStringLiteral("packaged.json"), R"({"Shaders": {"overlayTransition": " my-kit.dissolve "}})"));
    EXPECT_EQ(packaged.settings().shaders.overlayTransition, QStringLiteral("my-kit.dissolve"));

    SettingsStore low(write(QStringLiteral("low.json"), R"({"Animation": {"overlayDuration": 1}})"));
    EXPECT_EQ(low.settings().animation.overlayDuration, 16);
    SettingsStore high(write(QStringLiteral("high.json"), R"({"Animation": {"overlayDuration": 99999}})"));
    EXPECT_EQ(high.settings().animation.overlayDuration, 5000);
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

TEST_F(ConfigTest, FileTransferDeltaLimitIsGibibytesWithAClamp)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    EXPECT_EQ(defaults.settings().fileTransferDeltaLimit, qint64(64) << 30);
    // The transfer service keeps the same default for a worker that gets no setting.
    EXPECT_EQ(Settings{}.fileTransferDeltaLimit, FileTransfer::DefaultDeltaLimit);

    SettingsStore custom(write(QStringLiteral("custom.json"), R"({"Terminal": {"fileTransferDeltaLimit": 2}})"));
    EXPECT_EQ(custom.settings().fileTransferDeltaLimit, qint64(2) << 30);

    SettingsStore off(write(QStringLiteral("off.json"), R"({"Terminal": {"fileTransferDeltaLimit": 0}})"));
    EXPECT_EQ(off.settings().fileTransferDeltaLimit, 0);
    SettingsStore high(write(QStringLiteral("high.json"), R"({"Terminal": {"fileTransferDeltaLimit": 99999999}})"));
    EXPECT_EQ(high.settings().fileTransferDeltaLimit, qint64(1048576) << 30);
}

TEST_F(ConfigTest, DragAnswerWaitIsMillisecondsWithAClamp)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    EXPECT_EQ(defaults.settings().dragAnswerWait, 100);
    // The drag and drop handler keeps the same default for a pane that gets no setting.
    EXPECT_EQ(Settings{}.dragAnswerWait, DragDrop::DefaultAnswerWaitMs);
    SettingsStore custom(write(QStringLiteral("custom.json"), R"({"Terminal": {"dragAnswerWait": 250}})"));
    EXPECT_EQ(custom.settings().dragAnswerWait, 250);
    SettingsStore off(write(QStringLiteral("off.json"), R"({"Terminal": {"dragAnswerWait": 0}})"));
    EXPECT_EQ(off.settings().dragAnswerWait, 0);
    SettingsStore high(write(QStringLiteral("high.json"), R"({"Terminal": {"dragAnswerWait": 99999}})"));
    EXPECT_EQ(high.settings().dragAnswerWait, 1000);
}

TEST_F(ConfigTest, DragTreeLifetimeIsMinutesWithAClamp)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    EXPECT_EQ(defaults.settings().dragTreeLifetime, 10);
    // The trees keep the same default in a process that applies no setting.
    EXPECT_EQ(Settings{}.dragTreeLifetime, DragTrees::DefaultLifetime.count());
    SettingsStore custom(write(QStringLiteral("custom.json"), R"({"Terminal": {"dragTreeLifetime": 60}})"));
    EXPECT_EQ(custom.settings().dragTreeLifetime, 60);
    SettingsStore low(write(QStringLiteral("low.json"), R"({"Terminal": {"dragTreeLifetime": 0}})"));
    EXPECT_EQ(low.settings().dragTreeLifetime, 1);
    SettingsStore high(write(QStringLiteral("high.json"), R"({"Terminal": {"dragTreeLifetime": 99999}})"));
    EXPECT_EQ(high.settings().dragTreeLifetime, 1440);
}

TEST_F(ConfigTest, IntervalsOfBellsStatusPasteReadsAndAutoScrollAreSettings)
{
    SettingsStore defaults(write(QStringLiteral("default.json"), "{}"));
    EXPECT_EQ(defaults.settings().bellSoundInterval, 250);
    EXPECT_EQ(defaults.settings().statusNotificationInterval, 3000);
    EXPECT_EQ(defaults.settings().pasteReadTimeout, 10);
    EXPECT_EQ(defaults.settings().mouse.autoScrollInterval, 50);
    EXPECT_EQ(defaults.settings().control.eventInterval, 100);

    SettingsStore custom(write(QStringLiteral("custom.json"), R"({
        "Terminal": {"bellSoundInterval": 0, "pasteReadTimeout": 30},
        "Notifications": {"statusInterval": 0},
        "Mouse": {"autoScrollInterval": 120},
        "Control": {"eventInterval": 0}
    })"));
    EXPECT_EQ(custom.settings().control.eventInterval, 0);
    EXPECT_EQ(custom.settings().bellSoundInterval, 0);
    EXPECT_EQ(custom.settings().statusNotificationInterval, 0);
    EXPECT_EQ(custom.settings().pasteReadTimeout, 30);
    EXPECT_EQ(custom.settings().mouse.autoScrollInterval, 120);

    SettingsStore clamped(write(QStringLiteral("clamped.json"), R"({
        "Terminal": {"bellSoundInterval": 99999, "pasteReadTimeout": 0},
        "Notifications": {"statusInterval": -5},
        "Mouse": {"autoScrollInterval": 1},
        "Control": {"eventInterval": 99999}
    })"));
    EXPECT_EQ(clamped.settings().control.eventInterval, 10000);
    SettingsStore negative(write(QStringLiteral("negative.json"), R"({"Control": {"eventInterval": -5}})"));
    EXPECT_EQ(negative.settings().control.eventInterval, 0);
    EXPECT_EQ(clamped.settings().bellSoundInterval, 10000);
    EXPECT_EQ(clamped.settings().statusNotificationInterval, 0);
    EXPECT_EQ(clamped.settings().pasteReadTimeout, 1);
    EXPECT_EQ(clamped.settings().mouse.autoScrollInterval, 10);
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
    SettingsStore quotaFloor(write(QStringLiteral("quota.json"),
                                   R"({"Graphics": {"quota": 1}, "Notifications": {"rateLimit": -4}})"));
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

// ShaderRefs.h names the Shaders keys for the package manifest, the script
// API and the benchmark. A key added to the schema alone would be one that
// they do not know.
TEST(Config, ShaderKeysOfTheSchemaAreTheSharedLists)
{
    const PhosphorConfig::Schema schema = SettingsStore::schema();
    ASSERT_TRUE(schema.groups.contains(QStringLiteral("Shaders")));
    QStringList keys;
    for (const auto& key : schema.groups.value(QStringLiteral("Shaders"))) {
        keys << key.key;
        if (ShaderChainKeys.contains(key.key)) {
            EXPECT_EQ(key.expectedType, QMetaType::QVariantList) << key.key.toStdString();
        } else if (ShaderEventKeys.contains(key.key)) {
            EXPECT_EQ(key.expectedType, QMetaType::QString) << key.key.toStdString();
        }
    }
    QStringList expected = ShaderChainKeys + ShaderEventKeys + QStringList{QStringLiteral("params")};
    keys.sort();
    expected.sort();
    EXPECT_EQ(keys, expected);
}

TEST_F(ConfigTest, StructDefaultsAgreeWithTheSchema)
{
    // Every field, read from an empty file, is its struct default: the
    // schema's default and its read of each key agree with Settings.h.
    SettingsStore store(write(QStringLiteral("default.json"), "{}"));
    const Settings& read = store.settings();
    Settings expected;
    // Font.family "" and Font.size 0 come from the desktop.
    for (auto face : {&AfterglowRender::FontSpec::regular, &AfterglowRender::FontSpec::bold, &AfterglowRender::FontSpec::italic,
                      &AfterglowRender::FontSpec::boldItalic}) {
        (expected.font.*face).family = (read.font.*face).family;
    }
    expected.font.sizePoints = read.font.sizePoints;
    // The key modes are built from the bindings, which a default Settings holds unbuilt.
    expected.keyModes = read.keyModes;
    EXPECT_TRUE(read.font == expected.font);
    EXPECT_TRUE(read.terminal == expected.terminal);
    EXPECT_EQ(read.window, expected.window);
    EXPECT_TRUE(read.animation == expected.animation);
    EXPECT_TRUE(read.audio == expected.audio);
    EXPECT_TRUE(read.quickTerminal == expected.quickTerminal);
    EXPECT_TRUE(read.control == expected.control);
    EXPECT_TRUE(read.mouse == expected.mouse);
    EXPECT_TRUE(read.links == expected.links);
    EXPECT_TRUE(read.colors == expected.colors);
    EXPECT_EQ(read.shaders, expected.shaders);
    EXPECT_EQ(read.hintPatterns, expected.hintPatterns);
    EXPECT_EQ(read.keybinds, expected.keybinds);
    EXPECT_TRUE(read == expected);
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

// Started on a file that does not parse, every save used to be refused until
// a restart: the backend's own load had marked the file unreadable, and a
// reload that handed it a good document never ended that refusal.
TEST_F(ConfigTest, AStoreThatStartedOnABrokenFileSavesOnceItIsFixed)
{
    const QString path = write(QStringLiteral("config.json"), R"({"Window": {"padding": )");
    SettingsStore store(path);
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("does not parse")));
    write(QStringLiteral("config.json"), R"({"Window": {"padding": 9}})");
    store.reload();
    EXPECT_TRUE(store.problems().isEmpty());
    EXPECT_EQ(store.settings().window.padding, 9);
    ASSERT_TRUE(store.write(QStringLiteral("Window"), QStringLiteral("padding"), 12));
    const QJsonObject file = QJsonDocument::fromJson(read(path)).object();
    EXPECT_EQ(file.value(QStringLiteral("Window")).toObject().value(QStringLiteral("padding")).toInt(), 12);
}

// An empty file (a touch) holds nothing to keep: it is every default, and a
// save writes it.
TEST_F(ConfigTest, AnEmptyFileIsEveryDefaultAndTakesASave)
{
    const QString path = write(QStringLiteral("config.json"), "\n");
    SettingsStore store(path);
    EXPECT_TRUE(store.problems().isEmpty());
    EXPECT_EQ(store.settings().window.padding, WindowSettings{}.padding);
    ASSERT_TRUE(store.write(QStringLiteral("Window"), QStringLiteral("padding"), 12));
    const QJsonObject file = QJsonDocument::fromJson(read(path)).object();
    EXPECT_EQ(file.value(QStringLiteral("Window")).toObject().value(QStringLiteral("padding")).toInt(), 12);
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
    // Nor a directory: the first write makes both.
    EXPECT_FALSE(QFileInfo::exists(m_home.filePath(QStringLiteral("fresh"))));
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

TEST_F(ConfigTest, ValuesOfTheWrongTypeAreReported)
{
    SettingsStore store(write(QStringLiteral("config.json"), R"({
        "Window": {"blur": "maybe", "paneTitles": "yes"},
        "Terminal": {"scrollback": "lots", "scrollLines": "7", "term": "  "},
        "Font": {"size": "12pt", "gamma": "1.5"},
        "Colors": {"background": 123}
    })"));
    const QStringList problems = store.problems();
    EXPECT_TRUE(mentions(problems, QStringLiteral("Window.blur must be true or false")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Terminal.scrollback must be a whole number")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Font.size must be a number")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Colors.background must be a string")));
    // What PhosphorConfig reads as the type is fine.
    EXPECT_FALSE(mentions(problems, QStringLiteral("Window.paneTitles")));
    EXPECT_FALSE(mentions(problems, QStringLiteral("Terminal.scrollLines")));
    EXPECT_FALSE(mentions(problems, QStringLiteral("Font.gamma")));
    EXPECT_EQ(store.settings().scrollLines, 7);
    // An empty TERM would reach every program.
    EXPECT_EQ(store.settings().term, Settings{}.term);
}

TEST_F(ConfigTest, HintPatternsThatDoNotCompileAreReported)
{
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Hints": {"patterns": [
        {"pattern": "(unclosed", "action": "copy"},
        {"pattern": "[a-z]+", "action": "copy"}
    ]}})"));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Hints.patterns[0]: the pattern does not compile")));
    ASSERT_EQ(store.settings().hintPatterns.size(), 1);
    EXPECT_EQ(store.settings().hintPatterns.first().toMap().value(QStringLiteral("pattern")).toString(), QStringLiteral("[a-z]+"));
}

TEST_F(ConfigTest, AWriteThatCannotBeSavedChangesNothing)
{
    const QString directory = m_home.filePath(QStringLiteral("locked"));
    ASSERT_TRUE(QDir().mkpath(directory));
    const QString path = directory + QStringLiteral("/config.json");
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(R"({"Terminal": {"scrollLines": 5}})");
    }
    if (::geteuid() == 0) GTEST_SKIP() << "root writes anyway";
    {
        SettingsStore store(path);
        ASSERT_TRUE(QFile::setPermissions(path, QFileDevice::ReadOwner));
        ASSERT_TRUE(QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const bool written = store.write(QStringLiteral("Terminal"), QStringLiteral("scrollLines"), 9);
        QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        EXPECT_FALSE(written);
        EXPECT_EQ(store.persisted(QStringLiteral("Terminal"), QStringLiteral("scrollLines")).toInt(), 5);
        // The store closes with the file writable again: it must not save the
        // lost change then, over what the user has in the file.
    }
    EXPECT_TRUE(read(path).contains("\"scrollLines\": 5")) << read(path).toStdString();
}

TEST_F(ConfigTest, AWriteThatCannotBeSavedAppliesWhatTheFileHolds)
{
    const QString directory = m_home.filePath(QStringLiteral("locked"));
    ASSERT_TRUE(QDir().mkpath(directory));
    const QString path = write(QStringLiteral("locked/config.json"), R"({"Terminal": {"scrollLines": 5}})");
    if (::geteuid() == 0) GTEST_SKIP() << "root writes anyway";
    SettingsStore store(path);
    QStringList published;
    QObject::connect(&store, &SettingsStore::problemsChanged, &store,
                     [&](const QStringList& problems) { published << problems.join(QLatin1Char('\n')); });
    // Saved within the watcher's debounce: the write reads it first.
    write(QStringLiteral("locked/config.json"), R"({"Terminal": {"scrollLines": 6}, "Window": {"tabBar": "sometimes"}})");
    ASSERT_TRUE(QFile::setPermissions(path, QFileDevice::ReadOwner));
    ASSERT_TRUE(QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const bool written = store.write(QStringLiteral("Terminal"), QStringLiteral("scrollLines"), 9);
    QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    EXPECT_FALSE(written);
    // The problems of the file that was read, with its settings.
    EXPECT_EQ(store.settings().scrollLines, 6);
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Window.tabBar must be one of")));
    ASSERT_EQ(published.size(), 1);
    EXPECT_EQ(published.first(), store.problems().join(QLatin1Char('\n')));
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
    EXPECT_EQ(s.clipboardRead, Settings::Permission::Deny);
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
    EXPECT_TRUE(mentions(problems, QStringLiteral("QuickTerminal.curve must be a string")));
    EXPECT_FALSE(mentions(problems, QStringLiteral("Animation.scrollCurve")));
    EXPECT_EQ(store.settings().animation.cursorCurve, AnimationSettings{}.cursorCurve);
    EXPECT_EQ(store.settings().animation.scrollCurve, QStringLiteral("spring:30,1"));
    EXPECT_EQ(store.settings().quickTerminal.curve, QuickTerminalSettings{}.curve);
    EXPECT_TRUE(mentions(problems, QStringLiteral("Shaders.pane must be a list")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Window.tabBar must be one of")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Colors.divider is not a color")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Colors.palette[1] is not a color")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Unknown setting Font.sise")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Unknown setting group Fonts")));
    EXPECT_TRUE(mentions(problems, QStringLiteral("Colors.palette needs 16 colors, not 2")));
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
    for (qsizetype i = 0; i <= MaxHintPatterns; ++i) many.append(QJsonObject{{QStringLiteral("pattern"), QStringLiteral("x%1").arg(i)}});
    SettingsStore capped(write(QStringLiteral("many.json"),
                               QJsonDocument(QJsonObject{{QStringLiteral("Hints"), QJsonObject{{QStringLiteral("patterns"), many}}}}).toJson()));
    EXPECT_EQ(capped.settings().hintPatterns.size(), MaxHintPatterns);
    EXPECT_TRUE(mentions(capped.problems(), QStringLiteral("first %1").arg(MaxHintPatterns)));
    const QString description = SettingsStore::reference().value(QStringLiteral("groups")).toObject().value(QStringLiteral("Hints"))
                                    .toObject().value(QStringLiteral("patterns")).toObject().value(QStringLiteral("description")).toString();
    EXPECT_TRUE(description.contains(QStringLiteral("Up to %1 hints").arg(MaxHintPatterns))) << description.toStdString();
    EXPECT_TRUE(description.contains(QStringLiteral("1 to %1 arguments").arg(MaxHintCommandArguments))) << description.toStdString();
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
                                                                 "Shaders": {"pane": [" bloom", ""]},
                                                                 "Audio": {"inputSource": " auto "}})"));
    const Settings& s = store.settings();
    EXPECT_EQ(s.audio.inputSource, QStringLiteral("auto"));
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
    // The search directories hold a bare "mine" too; the content says it is JSON.
    QDir().mkpath(m_home.path() + QStringLiteral("/afterglow/themes"));
    write(QStringLiteral("afterglow/themes/mine"), R"({"foreground": "#445566", "selectionBackgroud": "#111111"})");
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Colors": {"theme": "mine"}})"));
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
    // select_all's default Ctrl+Shift+A would match first, whatever Shift does.
    const Keybinds letters = Keybinds::fromMap({{QStringLiteral("new_tab"), QStringLiteral("Ctrl+A")},
                                                {QStringLiteral("select_all"), QString()}});
    const QKeyEvent shiftA = press(Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(letters.match(&shiftA), Action::None);
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

TEST(Keybinds, ScriptCommandsBindKeysAndMalformedTargetsAreReported)
{
    QStringList errors;
    const Keybinds kb = Keybinds::fromMap(
        {{QStringLiteral("script:projects.switch"), QStringList{QStringLiteral("Ctrl+Alt+P"), QStringLiteral("Ctrl+Alt+O")}},
         {QStringLiteral("script:projects"), QStringLiteral("Ctrl+Alt+X")},
         {QStringLiteral("script:.switch"), QStringLiteral("Ctrl+Alt+Y")},
         {QStringLiteral("script:projects."), QStringLiteral("Ctrl+Alt+Z")}},
        &errors);
    const QKeyEvent p = press(Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_EQ(kb.matchScript(&p), QStringLiteral("projects.switch"));
    EXPECT_EQ(kb.match(&p), Action::None);
    const QStringList expected{QKeySequence(QStringLiteral("Ctrl+Alt+P")).toString(QKeySequence::NativeText),
                               QKeySequence(QStringLiteral("Ctrl+Alt+O")).toString(QKeySequence::NativeText)};
    EXPECT_EQ(kb.bindingsOfScript(QStringLiteral("projects.switch")), expected);
    // A target without a script or a command binds no key.
    for (Qt::Key key : {Qt::Key_X, Qt::Key_Y, Qt::Key_Z}) {
        const QKeyEvent e = press(key, Qt::ControlModifier | Qt::AltModifier);
        EXPECT_TRUE(kb.matchScript(&e).isEmpty()) << key;
        EXPECT_FALSE(kb.isBound(QKeyCombination(Qt::ControlModifier | Qt::AltModifier, key).toCombined())) << key;
    }
    EXPECT_EQ(errors.size(), 3) << errors.join(QLatin1Char('\n')).toStdString();
    for (const char* target : {"script:projects", "script:.switch", "script:projects."}) {
        EXPECT_TRUE(mentions(errors, QStringLiteral("\"%1\" names no script command").arg(QLatin1String(target)))) << target;
    }
    EXPECT_TRUE(Keybinds::isScriptTarget(QStringLiteral("script:a.b.c")));
    EXPECT_FALSE(Keybinds::isScriptTarget(QStringLiteral("text:a.b")));
}

TEST(KeyModes, AMalformedScriptTargetInATableIsReported)
{
    QStringList errors;
    const auto modes = modesFor({{QStringLiteral("table:run"), QStringLiteral("Ctrl+Space, R")}},
                                {{QStringLiteral("run"), QVariantMap{{QStringLiteral("script:nodot"), QStringLiteral("N")},
                                                                     {QStringLiteral("script:a.go"), QStringLiteral("G")}}}},
                                {}, &errors);
    EXPECT_TRUE(mentions(errors, QStringLiteral("\"script:nodot\" names no script command")));
    KeyModes::State state;
    const QKeyEvent space = press(Qt::Key_Space, Qt::ControlModifier);
    const QKeyEvent r = press(Qt::Key_R);
    const QKeyEvent n = press(Qt::Key_N);
    const QKeyEvent g = press(Qt::Key_G);
    modes.press(&space, state, 0);
    modes.press(&r, state, 10);
    EXPECT_EQ(state.table, QStringLiteral("run"));
    EXPECT_TRUE(modes.press(&n, state, 20).target.isEmpty());
    modes.press(&space, state, 30);
    modes.press(&r, state, 40);
    EXPECT_EQ(modes.press(&g, state, 50).target, QStringLiteral("script:a.go"));
}

TEST_F(ConfigTest, StringSettingsOfAnotherJsonTypeUseTheDefault)
{
    // PhosphorConfig reads false as "false": the pager would run false.
    SettingsStore store(write(QStringLiteral("config.json"),
                              R"({"Terminal": {"pager": false, "term": 5, "shell": " /bin/zsh "}, "Font": {"family": ["Iosevka"]}})"));
    const Settings defaults;
    EXPECT_EQ(store.settings().pager, defaults.pager);
    EXPECT_EQ(store.settings().term, defaults.term);
    EXPECT_EQ(store.settings().shell, QStringLiteral("/bin/zsh"));
    EXPECT_NE(store.settings().font.regular.family, "[\"Iosevka\"]");
    for (const char* key : {"Terminal.pager", "Terminal.term", "Font.family"}) {
        EXPECT_TRUE(mentions(store.problems(), QStringLiteral("%1 must be a string").arg(QLatin1String(key)))) << key;
    }
    EXPECT_FALSE(mentions(store.problems(), QStringLiteral("Terminal.shell")));
    // An override of a string setting is the text as written.
    QJsonObject o;
    ASSERT_TRUE(SettingsStore::addOverride(o, QStringLiteral("Terminal.term=5")));
    ASSERT_TRUE(SettingsStore::addOverride(o, QStringLiteral("Terminal.pager=\"most\"")));
    SettingsStore overridden(store.path(), o);
    EXPECT_EQ(overridden.settings().term, QStringLiteral("5"));
    EXPECT_EQ(overridden.settings().pager, QStringLiteral("most"));
    // The override replaces the file's false, so the file's problem is gone.
    EXPECT_FALSE(mentions(overridden.problems(), QStringLiteral("Terminal.pager")));
}

TEST_F(ConfigTest, ListEntriesThatAreNotStringsAreReported)
{
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Font": {"features": ["-liga", 5]},
        "Links": {"schemes": ["HTTPS:", {}]}, "Shaders": {"pane": [true, "bloom"]}})"));
    EXPECT_EQ(store.settings().font.features, (std::vector<std::string>{"-liga"}));
    EXPECT_EQ(store.settings().links.schemes, QStringList{QStringLiteral("https")});
    EXPECT_EQ(store.settings().shaders.pane, QStringList{QStringLiteral("bloom")});
    for (const char* entry : {"Font.features[1]", "Links.schemes[1]", "Shaders.pane[0]"}) {
        EXPECT_TRUE(mentions(store.problems(), QStringLiteral("%1 must be a string").arg(QLatin1String(entry)))) << entry;
    }
}

TEST_F(ConfigTest, PaletteSizeAndOutOfRangeNumbersAreReported)
{
    QJsonArray seventeen;
    for (int i = 0; i < 17; ++i) seventeen.append(QStringLiteral("#000000"));
    SettingsStore store(write(QStringLiteral("config.json"),
                              QJsonDocument(QJsonObject{{QStringLiteral("Colors"), QJsonObject{{QStringLiteral("palette"), seventeen}}},
                                                        {QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("scrollback"), 1e12}}}})
                                  .toJson()));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Colors.palette needs 16 colors, not 17. Afterglow uses the first 16")));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Terminal.scrollback is too large or too small")));
    EXPECT_FALSE(mentions(store.problems(), QStringLiteral("Terminal.scrollback must be a whole number")));
    EXPECT_EQ(store.settings().terminal.maxScrollback, Settings{}.terminal.maxScrollback);
    SettingsStore one(write(QStringLiteral("one.json"), R"({"Colors": {"palette": ["#ff0000"]}})"));
    EXPECT_TRUE(mentions(one.problems(), QStringLiteral("Colors.palette needs 16 colors, not 1. The others use the default")));
    EXPECT_TRUE(same(one.settings().terminal.basePalette[0], {0xff, 0x00, 0x00}));
}

TEST_F(ConfigTest, AReloadPublishesItsProblemsOnce)
{
    const QString path = write(QStringLiteral("config.json"), R"({"Window": {"padding": 9}})");
    SettingsStore store(path);
    write(QStringLiteral("config.json"), "{ not json");
    store.reload();
    ASSERT_EQ(store.problems().size(), 1);
    QList<QStringList> published;
    QObject::connect(&store, &SettingsStore::problemsChanged, &store,
                     [&published](const QStringList& problems) { published.append(problems); });
    // The parse problem goes and an unknown key comes in the same reload:
    // one signal with both changes, not one with the file's half first.
    write(QStringLiteral("config.json"), R"({"Window": {"padding": 9, "nope": 1}})");
    store.reload();
    ASSERT_EQ(published.size(), 1);
    EXPECT_EQ(published.first(), store.problems());
    EXPECT_TRUE(mentions(published.first(), QStringLiteral("Unknown setting Window.nope")));
    // A reload that keeps the same problems publishes nothing.
    store.reload();
    EXPECT_EQ(published.size(), 1);
}

namespace {
int g_configWarnings = 0;
QtMessageHandler g_previousHandler = nullptr;
void countConfigWarnings(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg && context.category && qstrcmp(context.category, "afterglow.config") == 0) ++g_configWarnings;
    if (g_previousHandler) g_previousHandler(type, context, message);
}
} // namespace

TEST_F(ConfigTest, AProblemIsLoggedWhenItAppears)
{
    const QString path = write(QStringLiteral("config.json"), R"({"Window": {"nope": 1}})");
    g_configWarnings = 0;
    g_previousHandler = qInstallMessageHandler(countConfigWarnings);
    {
        SettingsStore store(path);
        store.reload();
        store.reload();
        write(QStringLiteral("config.json"), "{ not json");
        store.reload();
        store.reload();
    }
    qInstallMessageHandler(g_previousHandler);
    // The unknown key once, the parse problem once.
    EXPECT_EQ(g_configWarnings, 2);
}

TEST_F(ConfigTest, ThemeDirectoriesThatDoNotExistYetAreWatched)
{
    // A theme saved into ~/.local/share/afterglow/themes, made after the
    // start, must reload: the watch lists the directory before it exists.
    const QString dir = m_home.filePath(QStringLiteral("data-home/afterglow/themes"));
    ASSERT_FALSE(QFileInfo::exists(dir));
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Colors": {"theme": "later"}})"));
    EXPECT_TRUE(store.watchedPaths().contains(dir + QStringLiteral("/later.json")));
    EXPECT_TRUE(Themes::searchDirs().contains(dir));
}

TEST_F(ConfigTest, EverySettingReachesTheSettings)
{
    // A key in the schema that read() forgets would take any value and do
    // nothing. Each key gets a valid value other than its default.
    const QString path = write(QStringLiteral("config.json"), "{}");
    const Settings defaults = SettingsStore(path).settings();
    const QJsonObject reference = SettingsStore::reference().value(QStringLiteral("groups")).toObject();
    // These two change only the bindings that use them (see the KeyModes tests).
    const QStringList unused{QStringLiteral("Keybinds.leader"), QStringLiteral("Keybinds.tables")};
    const PhosphorConfig::Schema schema = SettingsStore::schema();
    for (auto group = schema.groups.cbegin(); group != schema.groups.cend(); ++group) {
        for (const auto& def : group.value()) {
            const QString name = group.key() + QLatin1Char('.') + def.key;
            if (unused.contains(name)) continue;
            const QJsonObject entry = reference.value(group.key()).toObject().value(def.key).toObject();
            const QJsonValue current = QJsonValue::fromVariant(def.defaultValue);
            QJsonValue value;
            if (!def.choices.isEmpty()) {
                for (const auto& choice : def.choices) {
                    if (choice.value != def.defaultValue) value = QJsonValue::fromVariant(choice.value);
                }
            } else if (def.expectedType == QMetaType::Bool) {
                value = !def.defaultValue.toBool();
            } else if (def.expectedType == QMetaType::Int || def.expectedType == QMetaType::Double) {
                const QJsonValue low = entry.value(QStringLiteral("minimum"));
                value = low == current ? entry.value(QStringLiteral("maximum")) : low;
            } else if (name == QLatin1String("Colors.theme")) {
                value = QStringLiteral("nord");
            } else if (name == QLatin1String("Colors.palette")) {
                QJsonArray palette;
                for (int i = 0; i < 16; ++i) palette.append(QStringLiteral("#123456"));
                value = palette;
            } else if (group.key() == QLatin1String("Colors") && def.expectedType == QMetaType::QString) {
                value = QStringLiteral("#123456");
            } else if (def.key.endsWith(QLatin1String("Curve")) || name == QLatin1String("QuickTerminal.curve")) {
                value = QStringLiteral("spring:30,1");
            } else if (name == QLatin1String("Hints.alphabet")) {
                value = QStringLiteral("qwer");
            } else if (name == QLatin1String("Font.features")) {
                value = QJsonArray{QStringLiteral("ss01")};
            } else if (name == QLatin1String("QuickTerminal.shortcut")) {
                value = QStringLiteral("Meta+F12");
            } else if (name == QLatin1String("Font.symbolMap")) {
                value = QJsonObject{{QStringLiteral("U+E000"), QStringLiteral("Afterglow Test")}};
            } else if (name == QLatin1String("Hints.patterns")) {
                value = QJsonArray{
                    QJsonObject{{QStringLiteral("pattern"), QStringLiteral("x+")}, {QStringLiteral("action"), QStringLiteral("open")}}};
            } else if (name == QLatin1String("Keybinds.bindings")) {
                value = QJsonObject{{QStringLiteral("new_tab"), QStringLiteral("Ctrl+Alt+F12")}};
            } else if (def.expectedType == QMetaType::QVariantMap) {
                value = QJsonObject{{QStringLiteral("AFTERGLOW_TEST"), QStringLiteral("1")}};
            } else if (def.expectedType == QMetaType::QVariantList) {
                value = QJsonArray{QStringLiteral("afterglow-test")};
            } else {
                value = QStringLiteral("afterglow-test");
            }
            ASSERT_FALSE(value.isUndefined()) << name.toStdString();
            SettingsStore store(path, QJsonObject{{group.key(), QJsonObject{{def.key, value}}}});
            EXPECT_FALSE(store.settings() == defaults) << name.toStdString();
            // A value that a check refused would reach the settings as the
            // default, or as the part that passed, and prove nothing.
            EXPECT_TRUE(store.problems().isEmpty())
                << name.toStdString() << ": " << store.problems().join(QLatin1Char('\n')).toStdString();
        }
    }
}

TEST_F(ConfigTest, AStringKeyOfAnotherJsonTypeIsReportedBeforeItsOwnCheck)
{
    // PhosphorConfig would read true as "true", four distinct letters.
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Hints": {"alphabet": true}, "Terminal": {"bell": 1}})"));
    EXPECT_EQ(store.settings().hintAlphabet, Settings{}.hintAlphabet);
    EXPECT_EQ(store.settings().bell, Settings{}.bell);
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Hints.alphabet must be a string. Using the default")));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Terminal.bell must be a string. Using the default")));
}

TEST_F(ConfigTest, ListsAndMapsOfAnotherJsonTypeUseTheDefault)
{
    // PhosphorConfig reads them as empty, or as the JSON that a string holds.
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Font": {"features": "[\"ss01\"]"},
        "Links": {"schemes": 5}, "Hints": {"patterns": {}}, "Terminal": {"env": "{\"A\": \"1\"}"}})"));
    const Settings defaults;
    EXPECT_EQ(store.settings().font.features, defaults.font.features);
    EXPECT_EQ(store.settings().links.schemes, defaults.links.schemes);
    EXPECT_EQ(store.settings().hintPatterns, defaults.hintPatterns);
    EXPECT_TRUE(store.settings().environment.empty());
    for (const char* key : {"Font.features must be a list", "Links.schemes must be a list", "Hints.patterns must be a list",
                            "Terminal.env must be an object"}) {
        EXPECT_TRUE(mentions(store.problems(), QString::fromLatin1(key))) << key;
    }
    // An override of the wrong type gives way to the default too, not to the file.
    const QString path = write(QStringLiteral("links.json"), R"({"Links": {"schemes": ["gopher"]}})");
    QJsonObject o;
    ASSERT_TRUE(SettingsStore::addOverride(o, QStringLiteral("Links.schemes=https")));
    SettingsStore overridden(path, o);
    EXPECT_EQ(overridden.settings().links.schemes, defaults.links.schemes);
    EXPECT_TRUE(mentions(overridden.problems(), QStringLiteral("Links.schemes must be a list")));
}

TEST_F(ConfigTest, NumbersThatTheValidatorsReplaceAreReported)
{
    SettingsStore store(write(QStringLiteral("config.json"),
                              R"({"Font": {"gamma": "nan", "cellHeight": "inf"}, "Terminal": {"scrollback": "99999999999"}})"));
    EXPECT_FLOAT_EQ(store.settings().font.gamma, Settings{}.font.gamma);
    EXPECT_FLOAT_EQ(store.settings().font.cellHeightScale, Settings{}.font.cellHeightScale);
    EXPECT_EQ(store.settings().terminal.maxScrollback, Settings{}.terminal.maxScrollback);
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Font.gamma must be a number")));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Font.cellHeight must be a number")));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Terminal.scrollback is too large or too small")));
    EXPECT_FALSE(mentions(store.problems(), QStringLiteral("Terminal.scrollback must be a whole number")));
}

TEST_F(ConfigTest, ProblemsComeInTheOrderOfTheSchema)
{
    SettingsStore store(write(QStringLiteral("config.json"),
                              R"({"Window": {"blur": "x"}, "Terminal": {"cursorBlink": "x"}, "Animation": {"cursor": "x"}})"));
    const QStringList problems = store.problems();
    ASSERT_EQ(problems.size(), 3) << problems.join(QLatin1Char('\n')).toStdString();
    EXPECT_TRUE(problems[0].startsWith(QLatin1String("Animation.cursor")));
    EXPECT_TRUE(problems[1].startsWith(QLatin1String("Terminal.cursorBlink")));
    EXPECT_TRUE(problems[2].startsWith(QLatin1String("Window.blur")));
}

TEST_F(ConfigTest, EnumValuesAreTrimmed)
{
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Terminal": {"cursorShape": " bar "}})"));
    EXPECT_EQ(store.settings().terminal.cursorShape, AfterglowVt::CursorShape::Bar);
    EXPECT_TRUE(store.problems().isEmpty()) << store.problems().join(QLatin1Char('\n')).toStdString();
}

TEST_F(ConfigTest, TransparentIsNotAColor)
{
    EXPECT_FALSE(Themes::isColorName(QStringLiteral("transparent")));
    EXPECT_FALSE(Themes::isColorName(QStringLiteral("Transparent")));
    // Qt drops the spaces and tabs of a colour name.
    EXPECT_FALSE(Themes::isColorName(QStringLiteral("trans parent")));
    EXPECT_FALSE(Themes::isColorName(QStringLiteral(" Transparent\t")));
    EXPECT_TRUE(Themes::isColorName(QStringLiteral("r e d")));
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Colors": {"background": "transparent"}})"));
    EXPECT_TRUE(same(store.settings().terminal.defaultBackground, Settings{}.terminal.defaultBackground));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Colors.background is not a color")));
}

TEST_F(ConfigTest, FontFeaturesAndSymbolFamiliesAreChecked)
{
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Font": {"features": ["liga", "cv01=2", "not-a-feature"],
        "symbolMap": {"U+E000": 5, "U+E001": "Symbols"}}})"));
    EXPECT_EQ(store.settings().font.features, (std::vector<std::string>{"liga", "cv01=2"}));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Font.features: \"not-a-feature\" is not an OpenType feature")));
    ASSERT_EQ(store.settings().font.symbolMap.size(), 1u);
    EXPECT_EQ(store.settings().font.symbolMap[0].family, "Symbols");
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Font.symbolMap: ignoring U+E000")));
}

TEST_F(ConfigTest, QuickTerminalShortcutIsTrimmedAndChecked)
{
    SettingsStore store(write(QStringLiteral("config.json"), R"({"QuickTerminal": {"shortcut": " Meta+F12 "}})"));
    EXPECT_EQ(store.settings().quickTerminal.shortcut, QStringLiteral("Meta+F12"));
    EXPECT_TRUE(store.problems().isEmpty()) << store.problems().join(QLatin1Char('\n')).toStdString();
    write(QStringLiteral("config.json"), R"({"QuickTerminal": {"shortcut": "Meta+Nothing"}})");
    store.reload();
    EXPECT_EQ(store.settings().quickTerminal.shortcut, QuickTerminalSettings{}.shortcut);
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("QuickTerminal.shortcut \"Meta+Nothing\" is not a key combination")));
    write(QStringLiteral("config.json"), R"({"QuickTerminal": {"shortcut": " "}})");
    store.reload();
    EXPECT_TRUE(store.settings().quickTerminal.shortcut.isEmpty());
    EXPECT_TRUE(store.problems().isEmpty()) << store.problems().join(QLatin1Char('\n')).toStdString();
}

TEST_F(ConfigTest, AHintCommandNeedsAProgram)
{
    SettingsStore store(write(QStringLiteral("config.json"), R"({"Hints": {"patterns": [
        {"pattern": "a+", "action": "command", "command": [" ", "{match}"]},
        {"pattern": "b+", "action": "command", "command": ["{match}"]},
        {"pattern": "c+"}
    ]}})"));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Hints.patterns[0]: the first argument of command must name a program")));
    ASSERT_EQ(store.settings().hintPatterns.size(), 2);
    EXPECT_EQ(store.settings().hintPatterns[0].toMap().value(QStringLiteral("pattern")).toString(), QStringLiteral("b+"));
    // Without an action, a hint copies.
    EXPECT_EQ(store.settings().hintPatterns[1].toMap().value(QStringLiteral("pattern")).toString(), QStringLiteral("c+"));
}

TEST_F(ConfigTest, EnvironmentWithANulIsRefused)
{
    // An override carries the NULs without a JSON parser in between.
    const QJsonObject env{{QStringLiteral("CUT"), QString(QStringLiteral("a") + QChar(u'\0') + QStringLiteral("b"))},
                          {QString(QStringLiteral("BAD") + QChar(u'\0') + QStringLiteral("NAME")), QStringLiteral("1")},
                          {QStringLiteral("KEPT"), QStringLiteral("1")}};
    SettingsStore store(write(QStringLiteral("config.json"), "{}"),
                        QJsonObject{{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("env"), env}}}});
    ASSERT_EQ(store.settings().environment.size(), 1u);
    EXPECT_EQ(store.settings().environment[0].first, "KEPT");
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("Terminal.env.CUT must not hold a NUL character")));
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("is not a variable name")));
}

TEST_F(ConfigTest, AnOverrideReplacesAMapAsAWhole)
{
    const QString path = write(QStringLiteral("config.json"), R"({"Terminal": {"env": {"A": "1", "B": "2"}},
        "Shaders": {"params": {"bloom": {"strength": 0.5}, "crt": {"curve": 1}}}})");
    QJsonObject o;
    ASSERT_TRUE(SettingsStore::addOverride(o, QStringLiteral(R"(Terminal.env={"C": "3"})")));
    ASSERT_TRUE(SettingsStore::addOverride(o, QStringLiteral(R"(Shaders.params={"bloom": {"radius": 2}})")));
    SettingsStore store(path, o);
    ASSERT_EQ(store.settings().environment.size(), 1u);
    EXPECT_EQ(store.settings().environment[0], (std::pair<std::string, std::string>{"C", "3"}));
    const QVariantMap params = store.settings().shaderParams;
    EXPECT_EQ(params.keys(), QStringList{QStringLiteral("bloom")});
    EXPECT_EQ(params.value(QStringLiteral("bloom")).toMap().keys(), QStringList{QStringLiteral("radius")});
    // The file keeps its own maps.
    EXPECT_EQ(store.persisted(QStringLiteral("Terminal"), QStringLiteral("env")).toMap().size(), 2);
}

TEST_F(ConfigTest, PersistedReadsAnEditThatIsNotReloadedYet)
{
    const QString path = write(QStringLiteral("config.json"), R"({"Shaders": {"window": ["bloom"]}})");
    SettingsStore store(path);
    // Saved within the watcher's debounce: a chain built on the old file
    // would drop the new pack when it is written.
    write(QStringLiteral("config.json"), R"({"Shaders": {"window": ["bloom", "crt"]}})");
    EXPECT_EQ(store.persisted(QStringLiteral("Shaders"), QStringLiteral("window")).toStringList(),
              (QStringList{QStringLiteral("bloom"), QStringLiteral("crt")}));
    // Emptied to white space after it held settings: a save in progress, as
    // loadFile() takes it, so the last document stands.
    write(QStringLiteral("config.json"), " \n");
    EXPECT_EQ(store.persisted(QStringLiteral("Shaders"), QStringLiteral("window")).toStringList(),
              QStringList{QStringLiteral("bloom")});
    // Deleted: the default.
    ASSERT_TRUE(QFile::remove(path));
    EXPECT_TRUE(store.persisted(QStringLiteral("Shaders"), QStringLiteral("window")).toList().isEmpty());
}

// An editor that saves in place empties the file, then writes it. The
// settings must not flash to the defaults between the two, and a save in the
// meantime must not replace the file that is being written.
TEST_F(ConfigTest, AFileThatAnEditorEmptiesKeepsItsSettingsUntilItsTextComes)
{
    const QString path = write(QStringLiteral("config.json"), R"({"Window": {"padding": 9}})");
    SettingsStore store(path);
    int changes = 0;
    QObject::connect(&store, &SettingsStore::changed, &store, [&] { ++changes; });
    write(QStringLiteral("config.json"), "");
    store.reload();
    EXPECT_EQ(store.settings().window.padding, 9);
    EXPECT_EQ(changes, 0);
    EXPECT_TRUE(mentions(store.problems(), QStringLiteral("is empty")));
    EXPECT_FALSE(store.write(QStringLiteral("Window"), QStringLiteral("padding"), 12));
    EXPECT_TRUE(read(path).isEmpty());
    write(QStringLiteral("config.json"), R"({"Window": {"padding": 11}})");
    store.reload();
    EXPECT_EQ(store.settings().window.padding, 11);
    EXPECT_TRUE(store.problems().isEmpty());
    // Deleted, the file is every default.
    ASSERT_TRUE(QFile::remove(path));
    store.reload();
    EXPECT_EQ(store.settings().window.padding, WindowSettings{}.padding);
}

TEST_F(ConfigTest, AWriteRemakesAConfigDirectoryThatWasRemoved)
{
    const QString directory = m_home.filePath(QStringLiteral("gone"));
    ASSERT_TRUE(QDir().mkpath(directory));
    const QString path = write(QStringLiteral("gone/config.json"), R"({"Window": {"padding": 9}})");
    SettingsStore store(path);
    ASSERT_TRUE(QDir(directory).removeRecursively());
    ASSERT_TRUE(store.write(QStringLiteral("Window"), QStringLiteral("padding"), 11));
    EXPECT_EQ(QJsonDocument::fromJson(read(path)).object().value(QStringLiteral("Window")).toObject(),
              (QJsonObject{{QStringLiteral("padding"), 11}}));
}
