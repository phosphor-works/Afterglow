// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Settings.h"
#include "Theme.h"

#include <AfterglowRender/Snapshot.h>

#include <PhosphorAudio/AudioDefaults.h>
#include <PhosphorConfig/JsonBackend.h>
#include <PhosphorConfig/Schema.h>
#include <PhosphorConfig/Store.h>

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QStyleHints>

#include <cmath>
#include <limits>

Q_LOGGING_CATEGORY(lcConfig, "afterglow.config", QtInfoMsg)

namespace Afterglow {

namespace {

using PhosphorConfig::KeyDef;

const QString Font = QStringLiteral("Font");
const QString Colors = QStringLiteral("Colors");
const QString Terminal = QStringLiteral("Terminal");
const QString Window = QStringLiteral("Window");
const QString Keys = QStringLiteral("Keybinds");
const QString Shaders = QStringLiteral("Shaders");
const QString Animation = QStringLiteral("Animation");
const QString Quick = QStringLiteral("QuickTerminal");
const QString ControlGroup = QStringLiteral("Control");
const QString AudioGroup = QStringLiteral("Audio");
const QString ScriptsGroup = QStringLiteral("Scripts");
const QString MouseGroup = QStringLiteral("Mouse");
const QString LinksGroup = QStringLiteral("Links");
const QString GraphicsGroup = QStringLiteral("Graphics");
const QString NotificationsGroup = QStringLiteral("Notifications");
const QString HintsGroup = QStringLiteral("Hints");

constexpr qsizetype MaxHintPatterns = 16;
constexpr qsizetype MaxHintCommandArguments = 32;

/// One spelling of an enum setting. Each enum has one table, which gives
/// both the schema's choices and what read() turns them into.
template<typename E>
struct Choice
{
    const char* name;
    E value;
};

using AfterglowRender::Antialiasing;
using AfterglowRender::Hinting;

const Choice<Hinting> HintingChoices[] = {{"none", Hinting::None}, {"light", Hinting::Light}, {"full", Hinting::Full}};
const Choice<Antialiasing> AntialiasingChoices[] = {{"grayscale", Antialiasing::Grayscale},
                                                    {"none", Antialiasing::None},
                                                    {"rgb", Antialiasing::Rgb},
                                                    {"bgr", Antialiasing::Bgr}};
const Choice<AfterglowVt::BidiMode> BidiChoices[] = {{"off", AfterglowVt::BidiMode::Off},
                                                     {"auto", AfterglowVt::BidiMode::Auto},
                                                     {"ltr", AfterglowVt::BidiMode::LeftToRight},
                                                     {"rtl", AfterglowVt::BidiMode::RightToLeft}};
const Choice<AfterglowVt::CursorShape> CursorShapeChoices[] = {{"block", AfterglowVt::CursorShape::Block},
                                                               {"underline", AfterglowVt::CursorShape::Underline},
                                                               {"bar", AfterglowVt::CursorShape::Bar}};
const Choice<Settings::CopyOnSelect> CopyOnSelectChoices[] = {{"primary", Settings::CopyOnSelect::Primary},
                                                              {"clipboard", Settings::CopyOnSelect::Clipboard},
                                                              {"off", Settings::CopyOnSelect::Off}};
const Choice<Settings::Bell> BellChoices[] = {{"none", Settings::Bell::None},
                                              {"attention", Settings::Bell::Attention},
                                              {"visual", Settings::Bell::Visual},
                                              {"both", Settings::Bell::Both}};
const Choice<Settings::Permission> ClipboardWriteChoices[] = {{"allow", Settings::Permission::Allow},
                                                              {"ask", Settings::Permission::Ask},
                                                              {"deny", Settings::Permission::Deny}};
const Choice<Settings::Permission> GraphicsTransferChoices[] = {{"allow", Settings::Permission::Allow},
                                                                {"deny", Settings::Permission::Deny}};
const Choice<WindowSettings::StartMode> StartModeChoices[] = {{"normal", WindowSettings::StartMode::Normal},
                                                              {"maximized", WindowSettings::StartMode::Maximized},
                                                              {"fullscreen", WindowSettings::StartMode::Fullscreen}};
const Choice<WindowSettings::TabBar> TabBarChoices[] = {{"auto", WindowSettings::TabBar::Auto},
                                                        {"always", WindowSettings::TabBar::Always},
                                                        {"never", WindowSettings::TabBar::Never}};
const Choice<WindowSettings::Scrollbar> ScrollbarChoices[] = {{"auto", WindowSettings::Scrollbar::Auto},
                                                              {"always", WindowSettings::Scrollbar::Always},
                                                              {"never", WindowSettings::Scrollbar::Never}};
const Choice<MouseSettings::RightClick> RightClickChoices[] = {{"menu", MouseSettings::RightClick::Menu},
                                                               {"paste", MouseSettings::RightClick::Paste},
                                                               {"copy-or-paste", MouseSettings::RightClick::CopyOrPaste},
                                                               {"extend", MouseSettings::RightClick::Extend}};
const Choice<MouseSettings::MiddleClick> MiddleClickChoices[] = {{"primary", MouseSettings::MiddleClick::Primary},
                                                                 {"clipboard", MouseSettings::MiddleClick::Clipboard},
                                                                 {"none", MouseSettings::MiddleClick::None}};
const Choice<Qt::KeyboardModifiers> ModifierChoices[] = {{"ctrl", Qt::ControlModifier},
                                                         {"shift", Qt::ShiftModifier},
                                                         {"alt", Qt::AltModifier},
                                                         {"none", Qt::NoModifier}};
const Choice<QuickTerminalSettings::Position> QuickPositionChoices[] = {{"top", QuickTerminalSettings::Position::Top},
                                                                        {"bottom", QuickTerminalSettings::Position::Bottom}};

/// "U+E000-U+F8FF,U+2500" as inclusive ranges; nullopt if any part is malformed.
std::optional<std::vector<std::pair<char32_t, char32_t>>> parseCodepointRanges(const QString& spec)
{
    std::vector<std::pair<char32_t, char32_t>> out;
    auto codepoint = [](QString s) -> std::optional<char32_t> {
        s = s.trimmed();
        if (s.startsWith(QLatin1String("U+"), Qt::CaseInsensitive)) {
            s = s.mid(2);
        }
        bool ok = false;
        const uint v = s.toUInt(&ok, 16);
        return ok && v < 0x110000 ? std::optional<char32_t>(char32_t(v)) : std::nullopt;
    };
    for (const QString& part : spec.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList ends = part.split(QLatin1Char('-'));
        const auto first = codepoint(ends.value(0));
        const auto last = ends.size() == 2 ? codepoint(ends[1]) : first;
        if (!first || !last || ends.size() > 2 || *last < *first) {
            return std::nullopt;
        }
        out.emplace_back(*first, *last);
    }
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

QVariant clampInt(const QVariant& v, int lo, int hi)
{
    return std::clamp(v.toInt(), lo, hi);
}

/// NaN and infinities become @p def: std::clamp would pass NaN through.
QVariant clampDouble(const QVariant& v, double def, double lo, double hi)
{
    const double d = v.toDouble();
    return std::isfinite(d) ? std::clamp(d, lo, hi) : def;
}

QVariant validColor(const QVariant& v, const QString& fallback)
{
    const QString s = v.toString();
    return (s.isEmpty() || Themes::isColorName(s)) ? QVariant(s) : QVariant(fallback);
}

KeyDef intKey(const QString& key, int def, int lo, int hi, const QString& description)
{
    return {key, def, QMetaType::Int, description, [lo, hi](const QVariant& v) { return clampInt(v, lo, hi); }};
}

KeyDef doubleKey(const QString& key, double def, double lo, double hi, const QString& description)
{
    return {key, def, QMetaType::Double, description,
            [def, lo, hi](const QVariant& v) { return clampDouble(v, def, lo, hi); }};
}

KeyDef stringKey(const QString& key, const QString& def, const QString& description)
{
    return {key, def, QMetaType::QString, description};
}

/// A string limited to @p choices; anything else becomes @p def.
KeyDef enumKey(const QString& key, const QString& def, const QString& description, const QStringList& choices)
{
    KeyDef k{key, def, QMetaType::QString, description, [def, choices](const QVariant& v) {
                 const QString s = v.toString().toLower();
                 return QVariant(choices.contains(s) ? s : def);
             }};
    for (const QString& c : choices) {
        k.choices.append({c, c});
    }
    return k;
}

template<typename E, std::size_t N>
KeyDef enumKey(const QString& key, const Choice<E> (&choices)[N], E def, const QString& description)
{
    QStringList names;
    QString defName;
    for (const Choice<E>& c : choices) {
        names << QLatin1String(c.name);
        if (c.value == def) {
            defName = QLatin1String(c.name);
        }
    }
    return enumKey(key, defName, description, names);
}

/// The value of an enum key. The schema has already turned anything else into the default.
template<typename E, std::size_t N>
E readEnum(const PhosphorConfig::Store& st, const QString& group, const QString& key, const Choice<E> (&choices)[N], E def)
{
    const QString name = st.read<QString>(group, key);
    for (const Choice<E>& c : choices) {
        if (name == QLatin1String(c.name)) {
            return c.value;
        }
    }
    return def;
}

KeyDef colorKey(const QString& key, const QString& def, const QString& description)
{
    return {key, def, QMetaType::QString, description, [def](const QVariant& v) { return validColor(v, def); }};
}

QVariantList stringList(const QStringList& strings)
{
    QVariantList out;
    for (const QString& s : strings) {
        out.append(s);
    }
    return out;
}

/// The desktop's fixed-width font, for Font.family "" and Font.size 0.
/// Without a GUI application (or a platform theme that names one), fontconfig's
/// monospace alias at 11 points.
std::pair<QString, double> desktopFixedFont()
{
    QString family;
    double size = 0;
    if (qGuiApp) {
        const QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        family = font.family().trimmed();
        size = font.pointSizeF();
    }
    return {family.isEmpty() ? QStringLiteral("monospace") : family,
            std::isfinite(size) && size > 0 ? size : AfterglowRender::FontSpec{}.sizePoints};
}

QVariantList defaultPalette()
{
    AfterglowVt::TerminalOptions o;
    QVariantList out;
    for (const AfterglowVt::Rgb& c : o.basePalette) {
        out.append(QColor(c.r, c.g, c.b).name());
    }
    return out;
}

PhosphorConfig::Schema buildSchema()
{
    // The struct defaults are the one source; the schema repeats none of them.
    const Settings d;
    const AfterglowRender::FontSpec& font = d.font;
    QVariantList features;
    for (const std::string& f : font.features) {
        features.append(QString::fromStdString(f));
    }
    const AfterglowVt::TerminalOptions& term = d.terminal;
    const auto hex = [](AfterglowVt::Rgb c) { return QColor(c.r, c.g, c.b).name(); };
    const QString wordSeparators = QString::fromStdU32String(std::u32string(AfterglowVt::DefaultWordBoundaries));

    PhosphorConfig::Schema schema;
    schema.version = 1;
    schema.groups[Font] = {
        stringKey(QStringLiteral("family"), {},
                  QStringLiteral("Font family of regular text. An empty value uses the desktop's fixed-width font.")),
        stringKey(QStringLiteral("boldFamily"), {}, QStringLiteral("Font family of bold text. An empty value uses family.")),
        stringKey(QStringLiteral("italicFamily"), {}, QStringLiteral("Font family of italic text. An empty value uses family.")),
        stringKey(QStringLiteral("boldItalicFamily"), {},
                  QStringLiteral("Font family of bold italic text. An empty value uses the italic family.")),
        {QStringLiteral("size"), 0.0, QMetaType::Double,
         QStringLiteral("Size in points, from %1 to %2. A value of 0 uses the size of the desktop's fixed-width font.")
             .arg(AfterglowRender::FontSpec::MinPoints)
             .arg(AfterglowRender::FontSpec::MaxPoints),
         [](const QVariant& v) {
             const double size = v.toDouble();
             return QVariant(!std::isfinite(size) || size <= 0
                                 ? 0.0
                                 : std::clamp(size, double(AfterglowRender::FontSpec::MinPoints),
                                              double(AfterglowRender::FontSpec::MaxPoints)));
         }},
        doubleKey(QStringLiteral("sizeStep"), d.fontSizeStep, 0.1, 12.0,
                  QStringLiteral("Points that the font_bigger and font_smaller actions add or remove.")),
        intKey(QStringLiteral("weight"), int(font.regular.weight), 100, 900,
               QStringLiteral("Weight of regular text. It selects a static font and sets the weight axis of a variable font.")),
        intKey(QStringLiteral("boldWeight"), int(font.bold.weight), 100, 900,
               QStringLiteral("Weight of bold text. It selects a static font and sets the weight axis of a variable font.")),
        doubleKey(QStringLiteral("width"), font.regular.width, 50, 200,
                  QStringLiteral("Width axis of variable fonts. A value of 100 is the normal width. Each font limits it to its own "
                                 "range.")),
        doubleKey(QStringLiteral("italicSlant"), font.italic.slant, -90, 90,
                  QStringLiteral("Slant of italic text in degrees, for variable fonts with a slant axis.")),
        {QStringLiteral("features"), features, QMetaType::QVariantList,
         QStringLiteral("OpenType features in HarfBuzz syntax, such as calt, liga, ss01, cv01=2 or -liga.")},
        {QStringLiteral("fallbackFamilies"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Fallback families in priority order. These supply missing characters before system fallback.")},
        doubleKey(QStringLiteral("cellHeight"), font.cellHeightScale, 0.5, 3.0,
                  QStringLiteral("Row-height multiplier relative to font metrics. A value of 1.2 adds 20 percent to the row height.")),
        doubleKey(QStringLiteral("cellWidth"), font.cellWidthScale, 0.5, 3.0,
                  QStringLiteral("Column-width multiplier relative to font metrics.")),
        {QStringLiteral("symbolMap"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Preferred families for codepoint ranges, such as { \"U+E000-U+F8FF,U+F0000-U+FFFFD\": \"Symbols Nerd "
                        "Font Mono\" }. Afterglow draws box drawing, block elements, Braille and the Powerline separators "
                        "U+E0B0 to U+E0BF itself, so this setting does not change them.")},
        enumKey(QStringLiteral("hinting"), HintingChoices, font.hinting,
                QStringLiteral("Glyph hinting mode. The none mode preserves outlines. The light mode aligns vertical positions only. "
                               "The full mode aligns both axes.")),
        enumKey(QStringLiteral("antialiasing"), AntialiasingChoices, font.antialiasing,
                QStringLiteral("Font antialiasing mode. Subpixel modes use grayscale when transparency or shaders are active. Pane "
                               "effects are shaders, and they run while a pane is remote or in another context.")),
        doubleKey(QStringLiteral("gamma"), font.gamma, 0.5, 3.0,
                  QStringLiteral("Glyph coverage gamma. A value of 1 preserves coverage. Higher values increase edge coverage.")),
    };
    const HighlightColors& highlights = d.colors;
    schema.groups[Colors] = {
        stringKey(QStringLiteral("theme"), {},
                  QStringLiteral("Theme name or file path. Afterglow searches ~/.config/afterglow/themes, then afterglow/themes in "
                                 "each XDG data directory, then the 33 bundled themes, such as nord and afterglow-light. Use "
                                 "light:NAME,dark:NAME to follow the desktop. A theme replaces each color that it sets. Afterglow "
                                 "reads its own JSON themes, Ghostty themes and kitty themes. A relative path starts at the "
                                 "directory of the config file.")),
        colorKey(QStringLiteral("foreground"), hex(term.defaultForeground), QStringLiteral("Default text color.")),
        colorKey(QStringLiteral("background"), hex(term.defaultBackground), QStringLiteral("Default background color.")),
        colorKey(QStringLiteral("cursor"), {}, QStringLiteral("Cursor color. An empty value uses the foreground.")),
        colorKey(QStringLiteral("cursorText"), {}, QStringLiteral("Text color under a block cursor. An empty value uses the background.")),
        colorKey(QStringLiteral("selectionForeground"), {}, QStringLiteral("Selected text color. An empty value reverses the cell colors.")),
        colorKey(QStringLiteral("selectionBackground"), {}, QStringLiteral("Selection background color. An empty value reverses the cell colors.")),
        {QStringLiteral("palette"), defaultPalette(), QMetaType::QVariantList,
         QStringLiteral("The 16 ANSI colors, from black to bright white.")},
        colorKey(QStringLiteral("divider"), hex(WindowSettings::DefaultDivider),
                 QStringLiteral("Split divider color. A theme without a divider color gets one between its background and text "
                                "colors.")),
        doubleKey(QStringLiteral("unfocusedDim"), d.window.unfocusedDim, 0, 1, QStringLiteral("Darkening amount for unfocused splits.")),
        {QStringLiteral("boldIsBright"), term.boldIsBright, QMetaType::Bool,
         QStringLiteral("Use bright colors 8–15 for bold text with colors 0–7.")},
        doubleKey(QStringLiteral("minimumContrast"), term.minimumContrast, 1.0, 21.0,
                  QStringLiteral("Minimum text contrast ratio. Adjust text below this ratio. A value of 1 preserves colors. 4.5 is the "
                                 "WCAG AA normal-text threshold.")),
        colorKey(QStringLiteral("searchMatch"), hex(highlights.searchMatch), QStringLiteral("Background color of search matches.")),
        colorKey(QStringLiteral("searchCurrentMatch"), hex(highlights.searchCurrentMatch),
                 QStringLiteral("Background color of the current search match.")),
        colorKey(QStringLiteral("searchMatchText"), hex(highlights.searchMatchText), QStringLiteral("Text color of search matches.")),
        colorKey(QStringLiteral("visualBell"), hex(highlights.visualBell),
                 QStringLiteral("Color of the visual bell flash. A color that a program sets with OSC 21 replaces it.")),
        doubleKey(QStringLiteral("visualBellIntensity"), highlights.visualBellIntensity, 0, 1,
                  QStringLiteral("Opacity of the visual bell flash when it starts. A value of 0 hides the flash.")),
    };
    schema.groups[Terminal] = {
        enumKey(QStringLiteral("bidi"), BidiChoices, term.bidi,
                QStringLiteral("Bidirectional paragraph layout mode. Application grids retain logical order.")),
        intKey(QStringLiteral("scrollback"), int(term.maxScrollback), 0, 1000000, QStringLiteral("Lines of history that each pane keeps.")),
        {QStringLiteral("shellIntegration"), d.shellIntegration, QMetaType::Bool,
         QStringLiteral("Integrate Bash, Zsh and Fish prompts in new panes automatically.")},
        {QStringLiteral("sshIntegration"), d.sshIntegration, QMetaType::Bool,
         QStringLiteral("Run ssh through afterglow ssh in integrated shells. The pane label then shows the host, and the session "
                        "offers COLORTERM and TERM_PROGRAM to the server.")},
        enumKey(QStringLiteral("sshTerminfo"), d.sshTerminfo,
                QStringLiteral("What to do when TERM is afterglow: install its terminfo entry on each ssh host, or use "
                               "xterm-256color there instead."),
                {QStringLiteral("install"), QStringLiteral("fallback")}),
        stringKey(QStringLiteral("pager"), d.pager,
                  QStringLiteral("Command for viewing the last command output. Shell expansion is disabled.")),
        stringKey(QStringLiteral("shell"), {}, QStringLiteral("Program to run. An empty value starts your login shell.")),
        stringKey(QStringLiteral("term"), d.term, QStringLiteral("Value of TERM for the programs in the panes.")),
        enumKey(QStringLiteral("cursorShape"), CursorShapeChoices, term.cursorShape, QStringLiteral("Cursor shape.")),
        {QStringLiteral("cursorBlink"), term.cursorBlink, QMetaType::Bool, QStringLiteral("Make the cursor blink.")},
        enumKey(QStringLiteral("copyOnSelect"), CopyOnSelectChoices, d.copyOnSelect,
                QStringLiteral("Where new selections are copied: the primary selection, the clipboard too, or nowhere with off.")),
        enumKey(QStringLiteral("bell"), BellChoices, d.bell,
                QStringLiteral("Response to the bell. The attention value marks the window urgent. The visual value flashes the "
                               "pane. The both value does both.")),
        {QStringLiteral("notifications"), d.notifications, QMetaType::Bool,
         QStringLiteral("Show desktop notifications that programs send with OSC 9, OSC 99 and OSC 777, and program status "
                        "notifications (OSC 7501).")},
        {QStringLiteral("confirmClose"), d.confirmClose, QMetaType::Bool,
         QStringLiteral("Ask before closing a split, tab or window where a program is running.")},
        {QStringLiteral("hideMouseWhileTyping"), d.hideMouseWhileTyping, QMetaType::Bool,
         QStringLiteral("Hide the pointer while typing. Pointer movement shows it again.")},
        intKey(QStringLiteral("scrollLines"), d.scrollLines, 1, 50, QStringLiteral("Number of rows scrolled for each mouse-wheel notch.")),
        intKey(QStringLiteral("cursorBlinkInterval"), d.cursorBlinkInterval, 100, 5000,
               QStringLiteral("Duration in milliseconds for each visible and hidden cursor phase.")),
        intKey(QStringLiteral("textBlinkInterval"), d.textBlinkInterval, 100, 5000,
               QStringLiteral("Duration in milliseconds for each visible and hidden phase of blinking text (SGR 5).")),
        {QStringLiteral("scrollOnOutput"), d.scrollOnOutput, QMetaType::Bool,
         QStringLiteral("Scroll to the bottom when new output arrives.")},
        {QStringLiteral("scrollOnInput"), d.scrollOnInput, QMetaType::Bool, QStringLiteral("Scroll to the bottom on typing or paste.")},
        {QStringLiteral("ambiguousWide"), term.ambiguousWide, QMetaType::Bool,
         QStringLiteral("Use two cells for East Asian Ambiguous characters, including Greek, Cyrillic and some symbols.")},
        stringKey(QStringLiteral("bellSound"), {},
                  QStringLiteral("Bell sound. An empty value disables sound. Use default for the desktop bell, or specify a sound file.")),
        stringKey(QStringLiteral("wordSeparators"), wordSeparators,
                  QStringLiteral("Characters that end a word for double-click selection, besides spaces.")),
        {QStringLiteral("env"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Variables added to the shell's environment, such as { \"EDITOR\": \"nvim\" }. Each value must be a "
                        "string.")},
        {QStringLiteral("confirmUnsafePaste"), d.confirmUnsafePaste, QMetaType::Bool,
         QStringLiteral("Ask before pasting text that holds a line break, unless the program uses bracketed paste. A line "
                        "break could run a command.")},
        stringKey(QStringLiteral("fileTransferPassword"), {},
                  QStringLiteral("Shared secret for kitty file-transfer approval. An empty value always asks for approval.")),
        enumKey(QStringLiteral("clipboardRead"), d.clipboardRead,
                QStringLiteral("Programs reading the clipboard (OSC 52, OSC 5522). The ask value shows a prompt for each read."),
                {QStringLiteral("ask"), QStringLiteral("allow"), QStringLiteral("deny")}),
        enumKey(QStringLiteral("clipboardWrite"), ClipboardWriteChoices, d.clipboardWrite,
                QStringLiteral("Programs writing the clipboard (OSC 52, OSC 5522). The ask value shows a prompt for each write.")),
        enumKey(QStringLiteral("graphicsFileTransfer"), GraphicsTransferChoices, d.graphicsFileTransfer,
                QStringLiteral("Images that programs send with the kitty graphics protocol as a file, a temporary file or shared "
                               "memory. Images sent inline always work.")),
        {QStringLiteral("allowScreenReadback"), d.allowScreenReadback, QMetaType::Bool,
         QStringLiteral("Answer DECRQCRA checksum requests. These let any program in the pane read the screen back.")},
    };
    schema.groups[Window] = {
        intKey(QStringLiteral("padding"), d.window.padding, 0, 200, QStringLiteral("Space around the grid in logical pixels.")),
        intKey(QStringLiteral("initialColumns"), d.window.initialColumns, 10, 1000, QStringLiteral("Columns of a new window.")),
        intKey(QStringLiteral("initialRows"), d.window.initialRows, 4, 500, QStringLiteral("Rows of a new window.")),
        doubleKey(QStringLiteral("backgroundOpacity"), d.window.backgroundOpacity, 0.05, 1.0,
                  QStringLiteral("Opacity of the default background. Text stays opaque.")),
        {QStringLiteral("blur"), d.window.blur, QMetaType::Bool,
         QStringLiteral("Blur content behind translucent backgrounds with KWin.")},
        {QStringLiteral("paneTitles"), d.window.paneTitles, QMetaType::Bool,
         QStringLiteral("Show pane titles in tabs with multiple splits.")},
        stringKey(QStringLiteral("workingDirectory"), d.window.workingDirectory,
                  QStringLiteral("Starting directory for new tabs, splits and windows. Use inherit, home or a path. Inherit uses the "
                                 "focused pane's directory or launcher's directory. The afterglow -d option overrides this setting.")),
        enumKey(QStringLiteral("startMode"), StartModeChoices, d.window.startMode, QStringLiteral("How new windows open.")),
        {QStringLiteral("decorations"), d.window.decorations, QMetaType::Bool,
         QStringLiteral("Show the compositor's title bar and borders on new windows.")},
        {QStringLiteral("balancePadding"), d.window.balancePadding, QMetaType::Bool,
         QStringLiteral("Distribute unused cell space between opposite edges. Otherwise, space remains at the right and "
                        "bottom.")},
        enumKey(QStringLiteral("tabBar"), TabBarChoices, d.window.tabBar,
                QStringLiteral("Tab-bar visibility. The auto value shows the bar when multiple tabs exist.")),
        enumKey(QStringLiteral("scrollbar"), ScrollbarChoices, d.window.scrollbar,
                QStringLiteral("History scrollbar visibility. The auto value shows the scrollbar while the view scrolls or the "
                               "pointer is over it, then fades it out.")),
        intKey(QStringLiteral("scrollbarHideDelay"), d.window.scrollbarHideDelay, 0, 60000,
               QStringLiteral("Milliseconds an auto scrollbar stays visible after the last scroll or after the pointer leaves.")),
    };
    schema.groups[MouseGroup] = {
        enumKey(QStringLiteral("rightClick"), RightClickChoices, d.mouse.rightClick,
                QStringLiteral("Right-click action. The copy-or-paste action copies an existing selection or pastes otherwise. The "
                               "extend action extends the selection.")),
        enumKey(QStringLiteral("middleClick"), MiddleClickChoices, d.mouse.middleClick,
                QStringLiteral("Middle-click action. Paste the primary selection, paste the clipboard, or do nothing.")),
    };
    schema.groups[LinksGroup] = {
        {QStringLiteral("detectUrls"), d.links.detectUrls, QMetaType::Bool,
         QStringLiteral("Detect plain-text URLs in addition to application-provided OSC 8 links.")},
        stringKey(QStringLiteral("openWith"), {},
                  QStringLiteral("Command for opening links. The URL is appended. An empty value uses the desktop browser.")),
        enumKey(QStringLiteral("modifier"), ModifierChoices, d.links.modifier,
                QStringLiteral("Modifier for link highlighting and activation. With none, a plain click opens the link.")),
        {QStringLiteral("schemes"), stringList(d.links.schemes), QMetaType::QVariantList,
         QStringLiteral("URL schemes that open without a prompt. Links with other schemes ask first.")},
        {QStringLiteral("hintAlphabet"), d.links.hintAlphabet, QMetaType::QString,
         QStringLiteral("Letters that label hints, the easiest to reach first. It needs two or more letters, each one time. "
                        "Other values use the default."),
         [def = d.links.hintAlphabet](const QVariant& v) {
             const QString letters = v.toString().toLower().remove(QLatin1Char(' '));
             for (qsizetype i = 0; i < letters.size(); ++i) {
                 if (!letters[i].isLetter() || letters[i].isSurrogate() || letters.indexOf(letters[i]) != i) {
                     return QVariant(def);
                 }
             }
             return QVariant(letters.size() >= 2 ? letters : def);
         }},
    };
    schema.groups[Quick] = {
        doubleKey(QStringLiteral("height"), d.quickTerminal.height, 0.1, 1.0, QStringLiteral("Height as a fraction of the screen.")),
        stringKey(QStringLiteral("shortcut"), d.quickTerminal.shortcut,
                  QStringLiteral("Global shortcut to show or hide the quick terminal. An empty value disables the shortcut.")),
        {QStringLiteral("animate"), d.quickTerminal.animate, QMetaType::Bool,
         QStringLiteral("Animate quick-terminal opening and closing.")},
        stringKey(QStringLiteral("curve"), d.quickTerminal.curve,
                  QStringLiteral("Quick terminal slide: a Phosphor curve spec, as for Animation.splitCurve.")),
        intKey(QStringLiteral("duration"), d.quickTerminal.duration, 16, 5000,
               QStringLiteral("Duration of the quick terminal slide in milliseconds for curves other than springs.")),
        enumKey(QStringLiteral("position"), QuickPositionChoices, d.quickTerminal.position,
                QStringLiteral("Screen edge the quick terminal slides from.")),
        stringKey(QStringLiteral("screen"), {},
                  QStringLiteral("Output that shows the quick terminal, such as DP-1. An empty or unknown name uses the primary "
                                 "screen.")),
    };
    schema.groups[ControlGroup] = {
        {QStringLiteral("enabled"), d.control.enabled, QMetaType::Bool,
         QStringLiteral("Allow other programs to control Afterglow through D-Bus and afterglow ctl.")},
        {QStringLiteral("allowReadText"), d.control.allowReadText, QMetaType::Bool,
         QStringLiteral("Allow D-Bus clients to read pane text, including scrollback.")},
    };
    schema.groups[ScriptsGroup] = {
        {QStringLiteral("allowReadText"), d.scriptsAllowReadText, QMetaType::Bool,
         QStringLiteral("Allow local Luau scripts to query pane text, selections and semantic regions.")},
        {QStringLiteral("enabled"), d.scripts, QMetaType::Bool,
         QStringLiteral("Run Luau scripts from ~/.config/afterglow/scripts and from afterglow/scripts in each XDG data "
                        "directory.")},
    };
    const AudioSettings& audio = d.audio;
    schema.groups[AudioGroup] = {
        {QStringLiteral("enabled"), d.audio.enabled, QMetaType::Bool,
         QStringLiteral("Analyze system playback when a visible shader requests audio. Requires cava.")},
        enumKey(QStringLiteral("inputMethod"), d.audio.inputMethod, QStringLiteral("Audio capture backend."),
                {QStringLiteral("auto"), QStringLiteral("pipewire"), QStringLiteral("pulse")}),
        stringKey(QStringLiteral("inputSource"), d.audio.inputSource,
                  QStringLiteral("Playback source for capture. The auto value follows the default output.")),
        intKey(QStringLiteral("framerate"), audio.framerate, PhosphorAudio::Defaults::MinFramerate,
               PhosphorAudio::Defaults::MaxFramerate, QStringLiteral("Spectrum updates per second.")),
        intKey(QStringLiteral("smoothing"), audio.smoothing, PhosphorAudio::Defaults::MinNoiseReduction,
               PhosphorAudio::Defaults::MaxNoiseReduction,
               QStringLiteral("Noise reduction of the spectrum analyzer. A value of 0 is fast and jumpy. 100 is slow and smooth.")),
        doubleKey(QStringLiteral("extraSmoothing"), audio.extraSmoothing, PhosphorAudio::Defaults::MinExtraSmoothing,
                  PhosphorAudio::Defaults::MaxExtraSmoothing,
                  QStringLiteral("Part of the previous spectrum kept on each update, after noise reduction. A value of 0 turns it "
                                 "off.")),
        intKey(QStringLiteral("lowerCutoff"), audio.lowerCutoff, PhosphorAudio::Defaults::MinLowerCutoffHz,
               PhosphorAudio::Defaults::MaxLowerCutoffHz, QStringLiteral("Lowest analyzed frequency in hertz.")),
        intKey(QStringLiteral("higherCutoff"), audio.higherCutoff, PhosphorAudio::Defaults::MinHigherCutoffHz,
               PhosphorAudio::Defaults::MaxHigherCutoffHz, QStringLiteral("Highest analyzed frequency in hertz.")),
    };
    const AnimationSettings& anim = d.animation;
    schema.groups[Animation] = {
        {QStringLiteral("cursor"), anim.cursor, QMetaType::Bool, QStringLiteral("Animate cursor movement between cells.")},
        {QStringLiteral("scroll"), anim.scroll, QMetaType::Bool, QStringLiteral("Animate scrolling between rows.")},
        {QStringLiteral("splits"), anim.splits, QMetaType::Bool, QStringLiteral("Animate split opening, closing and resizing.")},
        stringKey(QStringLiteral("splitCurve"), anim.splitCurve,
                  QStringLiteral("Curve of split animations, as a Phosphor curve spec: a spring such as spring:22,1 (angular frequency, "
                                 "damping ratio), a cubic Bezier such as 0.2,0,0,1, or a named easing such as elastic-out. An "
                                 "invalid spec uses the default.")),
        intKey(QStringLiteral("splitDuration"), anim.splitDuration, 16, 5000,
               QStringLiteral("Duration of split animations in milliseconds for curves other than springs, which settle by "
                              "themselves.")),
        stringKey(QStringLiteral("overviewCurve"), anim.overviewCurve,
                  QStringLiteral("Pane overview motion: a Phosphor curve spec, as for splitCurve. The default spring overshoots "
                                 "slightly.")),
        intKey(QStringLiteral("overviewDuration"), anim.overviewDuration, 16, 5000,
               QStringLiteral("Duration of pane overview motion in milliseconds for curves other than springs.")),
        stringKey(QStringLiteral("tabCurve"), anim.tabCurve,
                  QStringLiteral("Tab switch motion (Shaders.tabTransition): a Phosphor curve spec, as for splitCurve.")),
        intKey(QStringLiteral("tabDuration"), anim.tabDuration, 16, 5000,
               QStringLiteral("Duration of tab switches in milliseconds for curves other than springs.")),
        intKey(QStringLiteral("paneEffectDuration"), anim.paneEffectDuration, 16, 5000,
               QStringLiteral("Milliseconds a pane takes to open or close in Shaders.paneEffects.")),
        stringKey(QStringLiteral("cursorCurve"), anim.cursorCurve,
                  QStringLiteral("Cursor motion between cells: a Phosphor curve spec, as for splitCurve.")),
        intKey(QStringLiteral("cursorDuration"), anim.cursorDuration, 16, 5000,
               QStringLiteral("Duration of cursor motion in milliseconds for curves other than springs.")),
        stringKey(QStringLiteral("scrollCurve"), anim.scrollCurve,
                  QStringLiteral("Scroll motion: a Phosphor curve spec, as for splitCurve.")),
        intKey(QStringLiteral("scrollDuration"), anim.scrollDuration, 16, 5000,
               QStringLiteral("Duration of scroll motion in milliseconds for curves other than springs.")),
        stringKey(QStringLiteral("bellCurve"), anim.bellCurve,
                  QStringLiteral("Fade of the visual bell flash: a Phosphor curve spec, as for splitCurve.")),
        intKey(QStringLiteral("bellDuration"), anim.bellDuration, 16, 5000,
               QStringLiteral("Duration of the visual bell fade in milliseconds for curves other than springs.")),
    };
    schema.groups[GraphicsGroup] = {
        intKey(QStringLiteral("quota"), int(d.graphicsQuota >> 20), 16, 4096,
               QStringLiteral("Mebibytes of kitty graphics images that each pane keeps. The oldest images go first when it is "
                              "full.")),
    };
    schema.groups[NotificationsGroup] = {
        intKey(QStringLiteral("rateLimit"), d.notificationRateLimit, 0, 1000,
               QStringLiteral("Desktop notifications that one pane can show in 10 seconds. A value of 0 removes the limit. "
                              "Notices from Afterglow itself do not count.")),
    };
    schema.groups[Shaders] = {
        {QStringLiteral("background"), stringList(d.shaders.background), QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane's background layer alone: cell backgrounds and images under the text.")},
        {QStringLiteral("text"), stringList(d.shaders.text), QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane's text layer alone: glyphs, decorations and images over the text.")},
        {QStringLiteral("cursor"), stringList(d.shaders.cursor), QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane's cursor layer alone.")},
        {QStringLiteral("pane"), stringList(d.shaders.pane), QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane. Name a pack by its directory name, which is normally its id, such as "
                        "cursor-trail or bloom.")},
        {QStringLiteral("window"), stringList(d.shaders.window), QMetaType::QVariantList,
         QStringLiteral("Shaders run over the whole window, across splits, after the pane shaders.")},
        stringKey(QStringLiteral("overview"), d.shaders.overview,
                  QStringLiteral("Shader that draws the pane overview (pane_overview). An empty value turns the overview off.")),
        stringKey(QStringLiteral("tabTransition"), d.shaders.tabTransition,
                  QStringLiteral("Shader that animates switching tabs. An empty value switches at once.")),
        stringKey(QStringLiteral("paneEffects"), d.shaders.paneEffects,
                  QStringLiteral("Shader that animates panes as they split open and close. An empty value turns it off.")),
        {QStringLiteral("params"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Parameter values for each shader reference, such as { \"bloom\": { \"strength\": 0.5 } }.")},
    };
    schema.groups[HintsGroup] = {
        {QStringLiteral("patterns"), d.hintPatterns, QMetaType::QVariantList,
         QStringLiteral("Up to 16 hints. Each is an object with a regular expression in pattern and an action: copy, open or "
                        "command. A command action also needs command, a list of 1 to 32 arguments.")},
    };
    schema.groups[Keys] = {
        stringKey(QStringLiteral("leader"), {}, QStringLiteral("Key that Leader stands for in sequences. It has no default.")),
        intKey(QStringLiteral("sequenceTimeout"), 1000, 100, 10000,
               QStringLiteral("Milliseconds before an incomplete sequence is cancelled.")),
        intKey(QStringLiteral("tableTimeout"), 5000, 100, 60000, QStringLiteral("Idle milliseconds before leaving a key table.")),
        {QStringLiteral("tables"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Named action-to-key maps, activated with table:NAME.")},
        {QStringLiteral("bindings"), Keybinds::defaultMap(), QMetaType::QVariantMap,
         QStringLiteral("Map each action to a key combination or list. An empty string removes the binding.")},
    };
    return schema;
}

AfterglowVt::Rgb toRgb(const QString& name, AfterglowVt::Rgb fallback)
{
    if (!Themes::isColorName(name)) return fallback;
    const QColor c = QColor::fromString(name);
    return AfterglowVt::Rgb{std::uint8_t(c.red()), std::uint8_t(c.green()), std::uint8_t(c.blue())};
}

/// Values the validators replaced with a default, and keys the schema does
/// not know. The validators stay silent, since every read and the reference
/// run them.
QStringList malformedValues(const PhosphorConfig::Schema& schema, const QJsonObject& file, const QJsonObject& overrides)
{
    QStringList out;
    QSet<QString> groups;
    for (const QJsonObject& source : {file, overrides}) {
        for (auto group = source.begin(); group != source.end(); ++group) {
            if (group.key() == schema.versionKey) continue;
            if (!schema.groups.contains(group.key())) {
                out << QStringLiteral("unknown setting group %1").arg(group.key());
                continue;
            }
            const QJsonObject keys = group.value().toObject();
            for (auto key = keys.begin(); key != keys.end(); ++key) {
                if (!schema.findKey(group.key(), key.key())) out << QStringLiteral("unknown setting %1.%2").arg(group.key(), key.key());
            }
            groups.insert(group.key());
        }
    }
    for (const QString& group : std::as_const(groups)) {
        for (const KeyDef& def : schema.groups.value(group)) {
            // An override wins over the file, so it is the value that counts.
            QJsonValue raw = overrides.value(group).toObject().value(def.key);
            if (raw.isUndefined()) raw = file.value(group).toObject().value(def.key);
            if (raw.isUndefined()) continue;
            const QString name = group + QLatin1Char('.') + def.key;
            if (def.expectedType == QMetaType::QVariantList && !raw.isArray()) {
                out << QStringLiteral("%1 must be a list. Using the default").arg(name);
            } else if (def.expectedType == QMetaType::QVariantMap && !raw.isObject()) {
                out << QStringLiteral("%1 must be an object. Using the default").arg(name);
            } else if (!def.choices.isEmpty()) {
                QStringList names;
                for (const auto& choice : def.choices) names << choice.value.toString();
                if (!names.contains(raw.toString().toLower())) {
                    out << QStringLiteral("%1 must be one of %2. Using the default").arg(name, names.join(QStringLiteral(", ")));
                }
            } else if (group == Colors && def.expectedType == QMetaType::QString && def.key != QLatin1String("theme")) {
                if (const QString color = raw.toString(); !color.isEmpty() && !Themes::isColorName(color)) {
                    out << QStringLiteral("%1 is not a color. Using the default").arg(name);
                }
            } else if (group == Colors && def.key == QLatin1String("palette")) {
                const QJsonArray palette = raw.toArray();
                for (qsizetype i = 0; i < palette.size(); ++i) {
                    if (!Themes::isColorName(palette[i].toString())) {
                        out << QStringLiteral("Colors.palette[%1] is not a color. Using the default").arg(i);
                    }
                }
            }
        }
    }
    return out;
}

} // namespace

QVariantList defaultHintPatterns()
{
    return {QVariantMap{
        {QStringLiteral("pattern"), QStringLiteral(R"((?:~?/|\.{1,2}/)?[\w.@+-]+(?:/[\w.@+-]+)*(?:\.[\w+-]+|:\d+)(?::\d+){0,2})")},
        {QStringLiteral("action"), QStringLiteral("copy")}}};
}

QString SettingsStore::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/afterglow/config.json");
}

PhosphorConfig::Schema SettingsStore::schema()
{
    return buildSchema();
}

SettingsStore::SettingsStore(QObject* parent)
    : SettingsStore(defaultPath(), parent)
{
}

SettingsStore::SettingsStore(const QString& path, QObject* parent)
    : SettingsStore(path, QJsonObject{}, parent)
{
}

SettingsStore::SettingsStore(const QString& path, const QJsonObject& overrides, QObject* parent)
    : QObject(parent)
    , m_path(path)
    , m_overrides(overrides)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    m_backend = std::make_unique<PhosphorConfig::JsonBackend>(path);
    // Command-line overrides are read over the file and never written to it.
    m_backend->setOverlay(m_overrides);
    m_store = std::make_unique<PhosphorConfig::Store>(m_backend.get(), buildSchema());
    // No file means every default. The file holds only what differs, so a
    // later change of a default reaches the user (PhosphorConfig's sparse
    // persistence); write() creates it.
    loadFile();
    m_settings = read();

    connect(&m_files, &PhosphorFsLoader::WatchedFileSet::filesChanged, this, &SettingsStore::reload);
    if (qGuiApp) {
        // "light:a,dark:b" themes follow the desktop, also while the file does not parse.
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &SettingsStore::apply);
    }
    watch();
}

SettingsStore::~SettingsStore() = default;

bool SettingsStore::loadFile()
{
    // One read and one parse: the document checked is the document used.
    QFile file(m_path);
    QString problem;
    if (!file.exists()) {
        // Deleted: every setting returns to its default.
        m_backend->replaceRoot({});
    } else if (!file.open(QIODevice::ReadOnly)) {
        problem = QStringLiteral("%1 cannot be read: %2. Keeping the current settings").arg(m_path, file.errorString());
    } else {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
        if (err.error != QJsonParseError::NoError) {
            problem = QStringLiteral("%1 does not parse: %2 at offset %3. Keeping the current settings")
                          .arg(m_path, err.errorString())
                          .arg(err.offset);
        } else if (!doc.isObject()) {
            problem = QStringLiteral("%1 does not hold a JSON object. Keeping the current settings").arg(m_path);
        } else {
            m_backend->replaceRoot(doc.object());
        }
    }
    if (!problem.isEmpty()) {
        qCWarning(lcConfig).noquote() << problem;
        setProblems(&m_fileProblems, {problem});
        return false;
    }
    // The backend now holds what the file holds.
    m_backend->clearDirty();
    setProblems(&m_fileProblems, {});
    return true;
}

void SettingsStore::setProblems(QStringList* list, const QStringList& problems)
{
    if (*list == problems) return;
    *list = problems;
    Q_EMIT problemsChanged(this->problems());
}

bool SettingsStore::write(const QString& group, const QString& key, const QVariant& value)
{
    if (!m_store->schema().findKey(group, key)) {
        qCWarning(lcConfig).noquote() << "cannot write" << group + QLatin1Char('.') + key << "- no such setting";
        return false;
    }
    // Start from the file as it is now: edits not reloaded yet stay, and a
    // file saved half-written is not replaced by an older copy.
    if (!loadFile()) {
        qCWarning(lcConfig).noquote() << "not writing" << group + QLatin1Char('.') + key << "until" << m_path << "parses";
        return false;
    }
    if (!QFile::exists(m_path)) {
        // The file can come to hold Terminal.fileTransferPassword. Create it
        // private before the first save, which keeps the mode it finds.
        QFile file(m_path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner)
            || file.write("{}\n") < 0) {
            qCWarning(lcConfig).noquote() << "cannot create" << m_path << "-" << file.errorString();
            return false;
        }
    }
    m_store->write(group, key, value);
    if (!m_backend->commit()) {
        qCWarning(lcConfig).noquote() << "cannot write" << m_path;
        return false;
    }
    apply();
    return true;
}

QVariant SettingsStore::persisted(const QString& group, const QString& key) const
{
    const PhosphorConfig::KeyDef* def = m_store->schema().findKey(group, key);
    if (!def) return {};
    const auto file = m_backend->persistedGroup(group);
    QVariant value = file->hasKey(key) ? file->readJson(key).toVariant() : def->defaultValue;
    return def->validator ? def->validator(value) : value;
}

bool SettingsStore::addOverride(QJsonObject& overrides, const QString& assignment)
{
    const qsizetype eq = assignment.indexOf(QLatin1Char('='));
    const QString path = assignment.left(eq).trimmed();
    const qsizetype dot = path.indexOf(QLatin1Char('.'));
    if (eq < 0 || dot <= 0 || dot == path.size() - 1) {
        return false;
    }
    const QString group = path.left(dot);
    const QString key = path.mid(dot + 1);
    // A misspelt key would otherwise be ignored without a word.
    if (!buildSchema().findKey(group, key)) {
        return false;
    }
    const QString raw = assignment.mid(eq + 1);
    // JSON when it parses as a value (numbers, booleans, lists, objects), else a string.
    const QJsonDocument doc = QJsonDocument::fromJson(QStringLiteral("[%1]").arg(raw).toUtf8());
    const QJsonValue value = doc.isArray() && doc.array().size() == 1 ? doc.array().first() : QJsonValue(raw);
    QJsonObject entries = overrides.value(group).toObject();
    entries.insert(key, value);
    overrides.insert(group, entries);
    return true;
}

bool SettingsStore::darkScheme() const
{
    // Unknown (no desktop preference) counts as dark, afterglow's own default.
    return !qGuiApp || QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Light;
}

void SettingsStore::watch()
{
    // Every place the theme could come from, so creating a closer one counts.
    QStringList paths{m_path};
    const auto spec = m_store->read<QString>(Colors, QStringLiteral("theme")).trimmed();
    paths << Themes::candidates(Themes::pick(spec, darkScheme()), Themes::searchDirs(), QFileInfo(m_path).absolutePath());
    m_files.setPaths(paths);
}

void SettingsStore::reload()
{
    if (loadFile()) {
        apply();
    }
}

void SettingsStore::apply()
{
    const Settings previous = m_settings;
    m_settings = read();
    watch(); // the theme file may have changed
    // A save that changes nothing, or our own write coming back through the
    // watcher, must not reset key modes or wake every script.
    if (m_settings == previous) {
        return;
    }
    qCInfo(lcConfig).noquote() << "reloaded" << m_path;
    Q_EMIT changed(previous);
}

Settings SettingsStore::read()
{
    const PhosphorConfig::Store& st = *m_store;
    Settings s;
    QStringList problems = malformedValues(st.schema(), m_backend->jsonRootSnapshot(), m_overrides);
    auto problem = [&problems](const QString& text) { problems << text; };

    // Font
    auto& f = s.font;
    QString family = st.read<QString>(Font, QStringLiteral("family")).trimmed();
    double size = st.read<double>(Font, QStringLiteral("size"));
    if (family.isEmpty() || size <= 0) {
        const auto [desktopFamily, desktopSize] = desktopFixedFont();
        if (family.isEmpty()) {
            family = desktopFamily;
        }
        if (size <= 0) {
            size = desktopSize;
        }
    }
    const QString bold = st.read<QString>(Font, QStringLiteral("boldFamily")).trimmed();
    const QString italic = st.read<QString>(Font, QStringLiteral("italicFamily")).trimmed();
    const QString boldItalic = st.read<QString>(Font, QStringLiteral("boldItalicFamily")).trimmed();
    const float weight = float(st.read<int>(Font, QStringLiteral("weight")));
    const float boldWeight = float(st.read<int>(Font, QStringLiteral("boldWeight")));
    const float width = float(st.read<double>(Font, QStringLiteral("width")));
    const float slant = float(st.read<double>(Font, QStringLiteral("italicSlant")));
    const std::string fam = family.toStdString();
    const std::string italicFam = italic.isEmpty() ? fam : italic.toStdString();
    f.sizePoints = float(size);
    f.regular = {fam, weight, width, 0};
    f.bold = {bold.isEmpty() ? fam : bold.toStdString(), boldWeight, width, 0};
    f.italic = {italicFam, weight, width, slant};
    f.boldItalic = {boldItalic.isEmpty() ? italicFam : boldItalic.toStdString(), boldWeight, width, slant};
    f.features.clear();
    for (const QVariant& v : st.read<QVariantList>(Font, QStringLiteral("features"))) {
        if (const QString feature = v.toString().trimmed(); !feature.isEmpty()) {
            f.features.push_back(feature.toStdString());
        }
    }
    for (const QVariant& v : st.read<QVariantList>(Font, QStringLiteral("fallbackFamilies"))) {
        if (const QString fallback = v.toString().trimmed(); !fallback.isEmpty()) {
            f.fallbackFamilies.push_back(fallback.toStdString());
        }
    }
    const QVariantMap symbolMap = st.read<QVariantMap>(Font, QStringLiteral("symbolMap"));
    for (auto it = symbolMap.cbegin(); it != symbolMap.cend(); ++it) {
        const QString symbolFamily = it.value().toString().trimmed();
        const auto ranges = parseCodepointRanges(it.key());
        if (!ranges || symbolFamily.isEmpty()) {
            problem(QStringLiteral("Font.symbolMap: ignoring %1. Use U+XXXX or U+XXXX-U+YYYY ranges and a family").arg(it.key()));
            continue;
        }
        for (const auto& [first, last] : *ranges) {
            f.symbolMap.push_back({first, last, symbolFamily.toStdString()});
        }
    }
    f.hinting = readEnum(st, Font, QStringLiteral("hinting"), HintingChoices, f.hinting);
    f.antialiasing = readEnum(st, Font, QStringLiteral("antialiasing"), AntialiasingChoices, f.antialiasing);
    f.gamma = float(st.read<double>(Font, QStringLiteral("gamma")));
    f.cellHeightScale = float(st.read<double>(Font, QStringLiteral("cellHeight")));
    f.cellWidthScale = float(st.read<double>(Font, QStringLiteral("cellWidth")));
    s.fontSizeStep = st.read<double>(Font, QStringLiteral("sizeStep"));

    // Colours
    auto& t = s.terminal;
    t.defaultForeground = toRgb(st.read<QString>(Colors, QStringLiteral("foreground")), t.defaultForeground);
    t.defaultBackground = toRgb(st.read<QString>(Colors, QStringLiteral("background")), t.defaultBackground);
    const QString cursor = st.read<QString>(Colors, QStringLiteral("cursor"));
    if (!cursor.isEmpty()) {
        t.cursorColor = toRgb(cursor, t.defaultForeground);
    }
    const QVariantList palette = st.read<QVariantList>(Colors, QStringLiteral("palette"));
    for (int i = 0; i < 16 && i < palette.size(); ++i) {
        t.basePalette[std::size_t(i)] = toRgb(palette[i].toString(), t.basePalette[std::size_t(i)]);
    }
    auto optionalColor = [&st](const char* key) -> std::optional<AfterglowVt::Rgb> {
        const QString name = st.read<QString>(Colors, QLatin1String(key));
        if (name.isEmpty()) {
            return std::nullopt;
        }
        return toRgb(name, {});
    };
    t.cursorTextColor = optionalColor("cursorText");
    t.selectionForeground = optionalColor("selectionForeground");
    t.selectionBackground = optionalColor("selectionBackground");
    AfterglowVt::Rgb divider = toRgb(st.read<QString>(Colors, QStringLiteral("divider")), WindowSettings::DefaultDivider);
    auto& hl = s.colors;
    hl.searchMatch = toRgb(st.read<QString>(Colors, QStringLiteral("searchMatch")), hl.searchMatch);
    hl.searchCurrentMatch = toRgb(st.read<QString>(Colors, QStringLiteral("searchCurrentMatch")), hl.searchCurrentMatch);
    hl.searchMatchText = toRgb(st.read<QString>(Colors, QStringLiteral("searchMatchText")), hl.searchMatchText);
    hl.visualBell = toRgb(st.read<QString>(Colors, QStringLiteral("visualBell")), hl.visualBell);
    hl.visualBellIntensity = st.read<double>(Colors, QStringLiteral("visualBellIntensity"));

    // A theme's colours replace the ones above.
    std::shared_ptr<const Theme> theme;
    if (const QString spec = st.read<QString>(Colors, QStringLiteral("theme")).trimmed(); !spec.isEmpty()) {
        const QString name = Themes::pick(spec, darkScheme());
        const QStringList dirs = Themes::searchDirs();
        const QString path = Themes::locate(name, dirs, QFileInfo(m_path).absolutePath());
        if (name.isEmpty()) {
            problem(QStringLiteral("Colors.theme \"%1\" names no theme").arg(spec));
        } else if (path.isEmpty()) {
            problem(name.contains(QLatin1Char('/'))
                        ? QStringLiteral("theme file %1 not found").arg(name)
                        : QStringLiteral("theme %1 not found in %2").arg(name, dirs.join(QStringLiteral(", "))));
        } else {
            QFile file(path);
            QString error;
            QStringList ignored;
            std::optional<Theme> parsed;
            if (!file.open(QIODevice::ReadOnly)) {
                error = file.errorString();
            } else {
                parsed = Themes::parse(file.readAll(), &error, &ignored);
            }
            if (parsed) {
                theme = std::make_shared<const Theme>(*parsed);
                m_themeFile = path;
                m_theme = theme;
                // Ghostty and kitty files hold much else; Afterglow's own should not.
                if (path.endsWith(QLatin1String(".json")) && !ignored.isEmpty()) {
                    problem(QStringLiteral("theme %1 has keys Afterglow does not use: %2").arg(name, ignored.join(QStringLiteral(", "))));
                }
            } else if (path == m_themeFile && m_theme) {
                // Mid-save: keep its colours rather than flash the config's.
                theme = m_theme;
                problem(QStringLiteral("theme %1: %2. Keeping its last colors").arg(name, error));
            } else {
                problem(QStringLiteral("theme %1: %2").arg(name, error));
            }
        }
    }
    if (theme) {
        t.defaultForeground = theme->foreground.value_or(t.defaultForeground);
        t.defaultBackground = theme->background.value_or(t.defaultBackground);
        for (std::size_t i = 0; i < 16; ++i) {
            t.basePalette[i] = theme->palette[i].value_or(t.basePalette[i]);
        }
        if (theme->cursor) {
            t.cursorColor = theme->cursor;
        }
        if (theme->cursorText) {
            t.cursorTextColor = theme->cursorText;
        }
        if (theme->selectionForeground) {
            t.selectionForeground = theme->selectionForeground;
        }
        if (theme->selectionBackground) {
            t.selectionBackground = theme->selectionBackground;
        }
        hl.searchMatch = theme->searchMatch.value_or(hl.searchMatch);
        hl.searchCurrentMatch = theme->searchCurrentMatch.value_or(hl.searchCurrentMatch);
        hl.searchMatchText = theme->searchMatchText.value_or(hl.searchMatchText);
        hl.visualBell = theme->visualBell.value_or(hl.visualBell);
        hl.visualBellIntensity = theme->visualBellIntensity.value_or(hl.visualBellIntensity);
        // Themes from other terminals have no divider: a fifth of the way
        // from the background to the text suits light and dark alike. The
        // config's divider is not used, since its default is a dark one.
        auto mix = [](std::uint8_t a, std::uint8_t b) { return std::uint8_t(a + (b - a) / 5); };
        const AfterglowVt::Rgb& bg = t.defaultBackground;
        const AfterglowVt::Rgb& fg = t.defaultForeground;
        divider = theme->divider.value_or(AfterglowVt::Rgb{mix(bg.r, fg.r), mix(bg.g, fg.g), mix(bg.b, fg.b)});
    }
    s.window.dividerColor = AfterglowRender::packRgba(divider);
    s.window.unfocusedDim = st.read<double>(Colors, QStringLiteral("unfocusedDim"));
    t.boldIsBright = st.read<bool>(Colors, QStringLiteral("boldIsBright"));
    t.minimumContrast = st.read<double>(Colors, QStringLiteral("minimumContrast"));

    // Terminal
    t.maxScrollback = std::size_t(st.read<int>(Terminal, QStringLiteral("scrollback")));
    t.cursorShape = readEnum(st, Terminal, QStringLiteral("cursorShape"), CursorShapeChoices, t.cursorShape);
    t.cursorBlink = st.read<bool>(Terminal, QStringLiteral("cursorBlink"));
    t.bidi = readEnum(st, Terminal, QStringLiteral("bidi"), BidiChoices, t.bidi);
    s.pager = st.read<QString>(Terminal, QStringLiteral("pager"));
    s.shellIntegration = st.read<bool>(Terminal, QStringLiteral("shellIntegration"));
    s.sshIntegration = st.read<bool>(Terminal, QStringLiteral("sshIntegration"));
    s.sshTerminfo = st.read<QString>(Terminal, QStringLiteral("sshTerminfo"));
    s.shell = st.read<QString>(Terminal, QStringLiteral("shell"));
    s.term = st.read<QString>(Terminal, QStringLiteral("term"));
    s.copyOnSelect = readEnum(st, Terminal, QStringLiteral("copyOnSelect"), CopyOnSelectChoices, s.copyOnSelect);
    s.bell = readEnum(st, Terminal, QStringLiteral("bell"), BellChoices, s.bell);
    s.notifications = st.read<bool>(Terminal, QStringLiteral("notifications"));
    s.confirmUnsafePaste = st.read<bool>(Terminal, QStringLiteral("confirmUnsafePaste"));
    s.confirmClose = st.read<bool>(Terminal, QStringLiteral("confirmClose"));
    s.hideMouseWhileTyping = st.read<bool>(Terminal, QStringLiteral("hideMouseWhileTyping"));
    s.scrollLines = st.read<int>(Terminal, QStringLiteral("scrollLines"));
    s.cursorBlinkInterval = st.read<int>(Terminal, QStringLiteral("cursorBlinkInterval"));
    s.textBlinkInterval = st.read<int>(Terminal, QStringLiteral("textBlinkInterval"));
    s.scrollOnOutput = st.read<bool>(Terminal, QStringLiteral("scrollOnOutput"));
    s.scrollOnInput = st.read<bool>(Terminal, QStringLiteral("scrollOnInput"));
    t.ambiguousWide = st.read<bool>(Terminal, QStringLiteral("ambiguousWide"));
    s.bellSound = st.read<QString>(Terminal, QStringLiteral("bellSound")).trimmed();
    t.wordBoundaries = st.read<QString>(Terminal, QStringLiteral("wordSeparators")).toStdU32String();
    const QVariantMap env = st.read<QVariantMap>(Terminal, QStringLiteral("env"));
    for (auto it = env.cbegin(); it != env.cend(); ++it) {
        if (it.key().isEmpty() || it.key().contains(QLatin1Char('='))) {
            problem(QStringLiteral("Terminal.env: \"%1\" is not a variable name").arg(it.key()));
        } else if (it.value().typeId() != QMetaType::QString) {
            // null, a number or a list would otherwise turn into text the user did not write.
            problem(QStringLiteral("Terminal.env.%1 must be a string").arg(it.key()));
        } else {
            s.environment.emplace_back(it.key().toStdString(), it.value().toString().toStdString());
        }
    }
    s.fileTransferPassword = st.read<QString>(Terminal, QStringLiteral("fileTransferPassword"));
    s.clipboardRead = st.read<QString>(Terminal, QStringLiteral("clipboardRead"));
    s.clipboardWrite = readEnum(st, Terminal, QStringLiteral("clipboardWrite"), ClipboardWriteChoices, s.clipboardWrite);
    s.graphicsFileTransfer =
        readEnum(st, Terminal, QStringLiteral("graphicsFileTransfer"), GraphicsTransferChoices, s.graphicsFileTransfer);
    s.allowScreenReadback = st.read<bool>(Terminal, QStringLiteral("allowScreenReadback"));
    s.graphicsQuota = std::size_t(st.read<int>(GraphicsGroup, QStringLiteral("quota"))) << 20;
    // The terminal core enforces these three itself.
    t.allowScreenReadback = s.allowScreenReadback;
    t.graphicsQuota = s.graphicsQuota;
    t.graphicsFileTransfer = s.graphicsFileTransfer != Settings::Permission::Deny;
    s.notificationRateLimit = st.read<int>(NotificationsGroup, QStringLiteral("rateLimit"));

    // Window
    s.window.padding = st.read<int>(Window, QStringLiteral("padding"));
    s.window.initialColumns = st.read<int>(Window, QStringLiteral("initialColumns"));
    s.window.initialRows = st.read<int>(Window, QStringLiteral("initialRows"));
    s.window.backgroundOpacity = st.read<double>(Window, QStringLiteral("backgroundOpacity"));
    s.window.blur = st.read<bool>(Window, QStringLiteral("blur"));
    s.window.paneTitles = st.read<bool>(Window, QStringLiteral("paneTitles"));
    s.window.workingDirectory = st.read<QString>(Window, QStringLiteral("workingDirectory")).trimmed();
    s.window.startMode = readEnum(st, Window, QStringLiteral("startMode"), StartModeChoices, s.window.startMode);
    s.window.decorations = st.read<bool>(Window, QStringLiteral("decorations"));
    s.window.balancePadding = st.read<bool>(Window, QStringLiteral("balancePadding"));
    s.window.tabBar = readEnum(st, Window, QStringLiteral("tabBar"), TabBarChoices, s.window.tabBar);
    s.window.scrollbar = readEnum(st, Window, QStringLiteral("scrollbar"), ScrollbarChoices, s.window.scrollbar);
    s.window.scrollbarHideDelay = st.read<int>(Window, QStringLiteral("scrollbarHideDelay"));
    s.mouse.rightClick = readEnum(st, MouseGroup, QStringLiteral("rightClick"), RightClickChoices, s.mouse.rightClick);
    s.mouse.middleClick = readEnum(st, MouseGroup, QStringLiteral("middleClick"), MiddleClickChoices, s.mouse.middleClick);
    s.links.detectUrls = st.read<bool>(LinksGroup, QStringLiteral("detectUrls"));
    s.links.openWith = st.read<QString>(LinksGroup, QStringLiteral("openWith")).trimmed();
    s.links.modifier = readEnum(st, LinksGroup, QStringLiteral("modifier"), ModifierChoices, s.links.modifier);
    s.links.schemes.clear();
    for (const QVariant& v : st.read<QVariantList>(LinksGroup, QStringLiteral("schemes"))) {
        // "https:" and "HTTPS" name the same scheme as "https".
        QString scheme = v.toString().trimmed().toLower();
        if (scheme.endsWith(QLatin1Char(':'))) {
            scheme.chop(1);
        }
        if (!scheme.isEmpty() && !s.links.schemes.contains(scheme)) {
            s.links.schemes.append(scheme);
        }
    }
    s.links.hintAlphabet = st.read<QString>(LinksGroup, QStringLiteral("hintAlphabet"));
    s.quickTerminal.height = st.read<double>(Quick, QStringLiteral("height"));
    s.quickTerminal.shortcut = st.read<QString>(Quick, QStringLiteral("shortcut"));
    s.quickTerminal.animate = st.read<bool>(Quick, QStringLiteral("animate"));
    s.quickTerminal.curve = st.read<QString>(Quick, QStringLiteral("curve")).trimmed();
    s.quickTerminal.duration = st.read<int>(Quick, QStringLiteral("duration"));
    s.quickTerminal.position = readEnum(st, Quick, QStringLiteral("position"), QuickPositionChoices, s.quickTerminal.position);
    s.quickTerminal.screen = st.read<QString>(Quick, QStringLiteral("screen")).trimmed();
    s.control.enabled = st.read<bool>(ControlGroup, QStringLiteral("enabled"));
    s.control.allowReadText = st.read<bool>(ControlGroup, QStringLiteral("allowReadText"));
    s.scripts = st.read<bool>(ScriptsGroup, QStringLiteral("enabled"));
    s.scriptsAllowReadText = st.read<bool>(ScriptsGroup, QStringLiteral("allowReadText"));

    s.audio.enabled = st.read<bool>(AudioGroup, QStringLiteral("enabled"));
    s.audio.inputMethod = st.read<QString>(AudioGroup, QStringLiteral("inputMethod"));
    s.audio.inputSource = st.read<QString>(AudioGroup, QStringLiteral("inputSource"));
    s.audio.framerate = st.read<int>(AudioGroup, QStringLiteral("framerate"));
    s.audio.smoothing = st.read<int>(AudioGroup, QStringLiteral("smoothing"));
    s.audio.extraSmoothing = st.read<double>(AudioGroup, QStringLiteral("extraSmoothing"));
    s.audio.lowerCutoff = st.read<int>(AudioGroup, QStringLiteral("lowerCutoff"));
    s.audio.higherCutoff = st.read<int>(AudioGroup, QStringLiteral("higherCutoff"));

    // Animation
    s.animation.cursor = st.read<bool>(Animation, QStringLiteral("cursor"));
    s.animation.scroll = st.read<bool>(Animation, QStringLiteral("scroll"));
    s.animation.splits = st.read<bool>(Animation, QStringLiteral("splits"));
    s.animation.splitCurve = st.read<QString>(Animation, QStringLiteral("splitCurve")).trimmed();
    s.animation.splitDuration = st.read<int>(Animation, QStringLiteral("splitDuration"));
    s.animation.overviewCurve = st.read<QString>(Animation, QStringLiteral("overviewCurve")).trimmed();
    s.animation.overviewDuration = st.read<int>(Animation, QStringLiteral("overviewDuration"));
    s.animation.tabCurve = st.read<QString>(Animation, QStringLiteral("tabCurve")).trimmed();
    s.animation.tabDuration = st.read<int>(Animation, QStringLiteral("tabDuration"));
    s.animation.paneEffectDuration = st.read<int>(Animation, QStringLiteral("paneEffectDuration"));
    s.animation.cursorCurve = st.read<QString>(Animation, QStringLiteral("cursorCurve")).trimmed();
    s.animation.cursorDuration = st.read<int>(Animation, QStringLiteral("cursorDuration"));
    s.animation.scrollCurve = st.read<QString>(Animation, QStringLiteral("scrollCurve")).trimmed();
    s.animation.scrollDuration = st.read<int>(Animation, QStringLiteral("scrollDuration"));
    s.animation.bellCurve = st.read<QString>(Animation, QStringLiteral("bellCurve")).trimmed();
    s.animation.bellDuration = st.read<int>(Animation, QStringLiteral("bellDuration"));

    // Shaders
    auto shaderList = [&st](const char* key) {
        QStringList refs;
        for (const QVariant& v : st.read<QVariantList>(Shaders, QLatin1String(key))) {
            if (const QString ref = v.toString().trimmed(); !ref.isEmpty()) {
                refs << ref;
            }
        }
        return refs;
    };
    s.shaders = {shaderList("background"), shaderList("text"), shaderList("cursor"), shaderList("pane"),
                 shaderList("window"), st.read<QString>(Shaders, QStringLiteral("overview")).trimmed(),
                 st.read<QString>(Shaders, QStringLiteral("tabTransition")).trimmed(),
                 st.read<QString>(Shaders, QStringLiteral("paneEffects")).trimmed()};
    s.shaderParams = st.read<QVariantMap>(Shaders, QStringLiteral("params"));

    // Hints: checked once here, not on every show_hints.
    const QVariantList patterns = st.read<QVariantList>(HintsGroup, QStringLiteral("patterns"));
    if (patterns.size() > MaxHintPatterns) {
        problem(QStringLiteral("Hints.patterns has %1 entries. Afterglow uses the first %2").arg(patterns.size()).arg(MaxHintPatterns));
    }
    s.hintPatterns.clear();
    for (qsizetype i = 0; i < patterns.size() && i < MaxHintPatterns; ++i) {
        const QString where = QStringLiteral("Hints.patterns[%1]").arg(i);
        if (patterns[i].typeId() != QMetaType::QVariantMap) {
            problem(where + QStringLiteral(" must be an object"));
            continue;
        }
        const QVariantMap entry = patterns[i].toMap();
        if (entry.value(QStringLiteral("pattern")).typeId() != QMetaType::QString
            || entry.value(QStringLiteral("pattern")).toString().isEmpty()) {
            problem(where + QStringLiteral(" needs a pattern"));
            continue;
        }
        const QString action = entry.value(QStringLiteral("action"), QStringLiteral("copy")).toString();
        if (action != QLatin1String("copy") && action != QLatin1String("open") && action != QLatin1String("command")) {
            problem(where + QStringLiteral(": unknown action \"%1\". Use copy, open or command").arg(action));
            continue;
        }
        if (action == QLatin1String("command")) {
            const QVariant command = entry.value(QStringLiteral("command"));
            const QVariantList arguments = command.toList();
            const bool strings = std::all_of(arguments.begin(), arguments.end(),
                                             [](const QVariant& a) { return a.typeId() == QMetaType::QString; });
            if (command.typeId() != QMetaType::QVariantList || arguments.isEmpty() || arguments.size() > MaxHintCommandArguments
                || !strings) {
                problem(where + QStringLiteral(": command must be a list of 1 to %1 strings").arg(MaxHintCommandArguments));
                continue;
            }
        }
        s.hintPatterns << entry;
    }

    // Keybinds
    QStringList errors;
    s.keybinds = Keybinds::fromMap(st.read<QVariantMap>(Keys, QStringLiteral("bindings")), &errors);
    s.keyModes = KeyModes::fromConfig(st.read<QVariantMap>(Keys, QStringLiteral("bindings")),
        st.read<QVariantMap>(Keys, QStringLiteral("tables")), st.read<QString>(Keys, QStringLiteral("leader")),
        st.read<int>(Keys, QStringLiteral("sequenceTimeout")), st.read<int>(Keys, QStringLiteral("tableTimeout")), s.keybinds, &errors);
    for (const QString& e : std::as_const(errors)) {
        problem(QStringLiteral("keybinds: ") + e);
    }

    for (const QString& p : std::as_const(problems)) {
        qCWarning(lcConfig).noquote() << p;
    }
    setProblems(&m_readProblems, problems);
    return s;
}

QJsonObject SettingsStore::reference()
{
    const auto schema = buildSchema();
    QJsonObject groups;
    for (auto group = schema.groups.cbegin(); group != schema.groups.cend(); ++group) {
        QJsonObject keys;
        for (const auto& key : group.value()) {
            QJsonObject entry{{QStringLiteral("default"), QJsonValue::fromVariant(key.defaultValue)},
                              {QStringLiteral("description"), key.description}};
            QJsonArray choices;
            for (const auto& choice : key.choices) choices.append(QJsonValue::fromVariant(choice.value));
            if (!choices.isEmpty()) entry.insert(QStringLiteral("choices"), choices);
            if (key.validator && (key.expectedType == QMetaType::Int || key.expectedType == QMetaType::Double)) {
                const QVariant lo = key.expectedType == QMetaType::Int ? QVariant(std::numeric_limits<int>::min()) : QVariant(-1e100);
                const QVariant hi = key.expectedType == QMetaType::Int ? QVariant(std::numeric_limits<int>::max()) : QVariant(1e100);
                entry.insert(QStringLiteral("minimum"), QJsonValue::fromVariant(key.validator(lo)));
                entry.insert(QStringLiteral("maximum"), QJsonValue::fromVariant(key.validator(hi)));
            }
            keys.insert(key.key, entry);
        }
        groups.insert(group.key(), keys);
    }
    return {{QStringLiteral("schema_version"), schema.version}, {QStringLiteral("groups"), groups}};
}

} // namespace Afterglow
