// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Settings.h"
#include "Theme.h"

#include <AfterglowRender/Snapshot.h>

#include <PhosphorConfig/JsonBackend.h>
#include <PhosphorConfig/Schema.h>
#include <PhosphorConfig/Store.h>

#include <QColor>
#include <limits>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QStyleHints>

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

QVariant clampDouble(const QVariant& v, double lo, double hi)
{
    return std::clamp(v.toDouble(), lo, hi);
}

QVariant validColor(const QVariant& v, const QString& fallback)
{
    const QString s = v.toString();
    return (s.isEmpty() || QColor::isValidColorName(s)) ? QVariant(s) : QVariant(fallback);
}

KeyDef intKey(const QString& key, int def, int lo, int hi, const QString& description)
{
    return {key, def, QMetaType::Int, description, [lo, hi](const QVariant& v) { return clampInt(v, lo, hi); }};
}

KeyDef doubleKey(const QString& key, double def, double lo, double hi, const QString& description)
{
    return {key, def, QMetaType::Double, description,
            [lo, hi](const QVariant& v) { return clampDouble(v, lo, hi); }};
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

KeyDef colorKey(const QString& key, const QString& def, const QString& description)
{
    return {key, def, QMetaType::QString, description, [def](const QVariant& v) { return validColor(v, def); }};
}

QStringList defaultPalette()
{
    AfterglowVt::TerminalOptions o;
    QStringList out;
    for (const AfterglowVt::Rgb& c : o.basePalette) {
        out.append(QColor(c.r, c.g, c.b).name());
    }
    return out;
}

PhosphorConfig::Schema buildSchema()
{
    const AfterglowRender::FontSpec font;
    QStringList features;
    for (const std::string& f : font.features) {
        features.append(QString::fromStdString(f));
    }

    const QString wordSeparators = QString::fromStdU32String(std::u32string(AfterglowVt::DefaultWordBoundaries));

    PhosphorConfig::Schema schema;
    schema.version = 1;
    schema.groups[Font] = {
        stringKey(QStringLiteral("family"), QString::fromStdString(font.regular.family), QStringLiteral("Regular text font family")),
        stringKey(QStringLiteral("boldFamily"), {}, QStringLiteral("Bold font family. An empty value uses family.")),
        stringKey(QStringLiteral("italicFamily"), {}, QStringLiteral("Italic font family. An empty value uses family.")),
        stringKey(QStringLiteral("boldItalicFamily"), {}, QStringLiteral("Bold italic font family. An empty value uses the italic family.")),
        doubleKey(QStringLiteral("size"), font.sizePoints, 4, 96, QStringLiteral("Size in points")),
        intKey(QStringLiteral("weight"), 400, 100, 900, QStringLiteral("Regular font weight. Used for static font matching and variable font axes.")),
        intKey(QStringLiteral("boldWeight"), 700, 100, 900, QStringLiteral("Bold font weight. Used for static font matching and variable font axes.")),
        doubleKey(QStringLiteral("width"), 100, 50, 200, QStringLiteral("Width axis (Monaspace: 100-125)")),
        doubleKey(QStringLiteral("italicSlant"), -11, -90, 90, QStringLiteral("Slant of italic text (variable fonts)")),
        {QStringLiteral("features"), features, QMetaType::QVariantList,
         QStringLiteral("OpenType features in HarfBuzz syntax: calt, liga, ss01, cv01=2, -liga")},
        {QStringLiteral("fallbackFamilies"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Fallback families in priority order. These supply missing characters before system fallback.")},
        doubleKey(QStringLiteral("cellHeight"), 1.0, 0.5, 3.0,
                  QStringLiteral("Row-height multiplier relative to font metrics. A value of 1.2 adds 20 percent to the row height.")),
        doubleKey(QStringLiteral("cellWidth"), 1.0, 0.5, 3.0, QStringLiteral("Column-width multiplier relative to font metrics.")),
        {QStringLiteral("symbolMap"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Preferred families for codepoint ranges. Example: { \"U+E000-U+F8FF,U+F0000-U+FFFFD\": \"Symbols Nerd "
                                 "Font Mono\" }. Afterglow draws box and powerline shapes itself.")},
        enumKey(QStringLiteral("hinting"), QStringLiteral("light"),
                QStringLiteral("Glyph hinting mode. The none mode preserves outlines. The light mode aligns vertical positions only. "
                                 "The full mode aligns both axes."),
                {QStringLiteral("none"), QStringLiteral("light"), QStringLiteral("full")}),
        enumKey(QStringLiteral("antialiasing"), QStringLiteral("grayscale"),
                QStringLiteral("Font antialiasing mode. Subpixel modes use grayscale when transparency or shaders are active."),
                {QStringLiteral("grayscale"), QStringLiteral("none"), QStringLiteral("rgb"), QStringLiteral("bgr")}),
        doubleKey(QStringLiteral("gamma"), 1.0, 0.5, 3.0,
                  QStringLiteral("Glyph coverage gamma. A value of 1 preserves coverage. Higher values increase edge coverage.")),
    };
    AfterglowVt::TerminalOptions term;
    const auto hex = [](AfterglowVt::Rgb c) { return QColor(c.r, c.g, c.b).name(); };
    schema.groups[Colors] = {
        stringKey(QStringLiteral("theme"), {},
                  QStringLiteral("Theme name from ~/.config/afterglow/themes or bundled themes: afterglow-dark and afterglow-light. Use "
                                 "light:NAME,dark:NAME to follow the desktop. Theme colors replace explicit colors. Supports Afterglow "
                                 "JSON, Ghostty and Kitty themes.")),
        colorKey(QStringLiteral("foreground"), hex(term.defaultForeground), QStringLiteral("Default text color.")),
        colorKey(QStringLiteral("background"), hex(term.defaultBackground), QStringLiteral("Default background color.")),
        colorKey(QStringLiteral("cursor"), {}, QStringLiteral("Cursor color. An empty value uses the foreground.")),
        colorKey(QStringLiteral("cursorText"), {}, QStringLiteral("Text color under a block cursor. An empty value uses the background.")),
        colorKey(QStringLiteral("selectionForeground"), {}, QStringLiteral("Selected text color. An empty value reverses the cell colors.")),
        colorKey(QStringLiteral("selectionBackground"), {}, QStringLiteral("Selection background color. An empty value reverses the cell colors.")),
        {QStringLiteral("palette"), defaultPalette(), QMetaType::QVariantList,
         QStringLiteral("The 16 ANSI colors, from black to bright white.")},
        colorKey(QStringLiteral("divider"), QStringLiteral("#3a3a42"), QStringLiteral("Split divider color.")),
        doubleKey(QStringLiteral("unfocusedDim"), 0.19, 0, 1, QStringLiteral("Darkening amount for unfocused splits.")),
        {QStringLiteral("boldIsBright"), false, QMetaType::Bool,
         QStringLiteral("Use bright colors 8–15 for bold text with colors 0–7.")},
        doubleKey(QStringLiteral("minimumContrast"), 1.0, 1.0, 21.0,
                  QStringLiteral("Minimum text contrast ratio. Adjust text below this ratio. A value of 1 preserves colors. 4.5 is the "
                                 "WCAG AA normal-text threshold.")),
    };
    schema.groups[Terminal] = {
        enumKey(QStringLiteral("bidi"), QStringLiteral("off"),
            QStringLiteral("Bidirectional paragraph layout mode. Application grids retain logical order."),
            {QStringLiteral("off"), QStringLiteral("auto"), QStringLiteral("ltr"), QStringLiteral("rtl")}),
        intKey(QStringLiteral("scrollback"), 10000, 0, 1000000, QStringLiteral("Lines of history per pane")),
        {QStringLiteral("shellIntegration"), true, QMetaType::Bool, QStringLiteral("Automatic Bash, Zsh and Fish prompt integration")},
        {QStringLiteral("sshIntegration"), true, QMetaType::Bool,
         QStringLiteral("Run ssh through afterglow ssh in integrated shells: label the pane with the host and offer COLORTERM and TERM_PROGRAM")},
        enumKey(QStringLiteral("sshTerminfo"), QStringLiteral("install"),
            QStringLiteral("When TERM is afterglow, install its terminfo entry on each ssh host, or use xterm-256color there instead"),
            {QStringLiteral("install"), QStringLiteral("fallback")}),
        stringKey(QStringLiteral("pager"), QStringLiteral("less -R"), QStringLiteral("Command for viewing the last command output. Shell expansion is disabled.")),
        stringKey(QStringLiteral("shell"), {}, QStringLiteral("Program to run. An empty value starts your login shell.")),
        stringKey(QStringLiteral("term"), QStringLiteral("xterm-256color"), QStringLiteral("TERM for child programs")),
        {QStringLiteral("cursorShape"), QStringLiteral("block"), QMetaType::QString, QStringLiteral("block, underline or bar"),
         [](const QVariant& v) {
             const QString s = v.toString();
             return (s == QLatin1String("underline") || s == QLatin1String("bar")) ? QVariant(s) : QVariant(QStringLiteral("block"));
         },
         {{QStringLiteral("block"), QStringLiteral("block")},
          {QStringLiteral("underline"), QStringLiteral("underline")},
          {QStringLiteral("bar"), QStringLiteral("bar")}}},
        {QStringLiteral("cursorBlink"), true, QMetaType::Bool, QStringLiteral("Blink the cursor")},
        {QStringLiteral("copyOnSelect"), QStringLiteral("primary"), QMetaType::QString,
         QStringLiteral("Copy new selections to primary, both primary and clipboard, or neither with off."),
         [](const QVariant& v) {
             // true and false from older configs: primary and off.
             const QString s = v.typeId() == QMetaType::Bool ? (v.toBool() ? QStringLiteral("primary") : QStringLiteral("off"))
                                                             : v.toString().toLower();
             if (s == QLatin1String("clipboard")) {
                 return QVariant(QStringLiteral("clipboard"));
             }
             if (s == QLatin1String("off") || s == QLatin1String("false") || s == QLatin1String("none")) {
                 return QVariant(QStringLiteral("off"));
             }
             return QVariant(QStringLiteral("primary"));
         },
         {{QStringLiteral("primary"), QStringLiteral("primary")},
          {QStringLiteral("clipboard"), QStringLiteral("clipboard")},
          {QStringLiteral("off"), QStringLiteral("off")}}},
        {QStringLiteral("bell"), QStringLiteral("attention"), QMetaType::QString,
         QStringLiteral("none, attention (mark the window urgent), visual (flash the pane) or both"),
         [](const QVariant& v) {
             const QString s = v.toString();
             return (s == QLatin1String("none") || s == QLatin1String("visual") || s == QLatin1String("both"))
                 ? QVariant(s)
                 : QVariant(QStringLiteral("attention"));
         },
         {{QStringLiteral("none"), QStringLiteral("none")},
          {QStringLiteral("attention"), QStringLiteral("attention")},
          {QStringLiteral("visual"), QStringLiteral("visual")},
          {QStringLiteral("both"), QStringLiteral("both")}}},
        {QStringLiteral("notifications"), true, QMetaType::Bool,
         QStringLiteral("Show desktop notifications programs send (OSC 9, OSC 777)")},
        {QStringLiteral("confirmClose"), true, QMetaType::Bool,
         QStringLiteral("Ask before closing a split, tab or window where a program is running")},
        {QStringLiteral("hideMouseWhileTyping"), false, QMetaType::Bool,
         QStringLiteral("Hide the pointer while typing. Pointer movement shows it again.")},
        intKey(QStringLiteral("scrollLines"), 3, 1, 50, QStringLiteral("Number of rows scrolled for each mouse-wheel notch.")),
        intKey(QStringLiteral("cursorBlinkInterval"), 530, 100, 5000, QStringLiteral("Duration in milliseconds for each visible and hidden cursor phase.")),
        {QStringLiteral("scrollOnOutput"), false, QMetaType::Bool,
         QStringLiteral("Scroll to the bottom when new output arrives.")},
        {QStringLiteral("scrollOnInput"), true, QMetaType::Bool, QStringLiteral("Scroll to the bottom on typing or paste.")},
        {QStringLiteral("ambiguousWide"), false, QMetaType::Bool,
         QStringLiteral("Use two cells for East Asian Ambiguous characters, including Greek, Cyrillic and some symbols.")},
        stringKey(QStringLiteral("bellSound"), {},
                  QStringLiteral("Bell sound. An empty value disables sound. Use default for the desktop bell, or specify a sound file.")),
        stringKey(QStringLiteral("wordSeparators"), wordSeparators,
                  QStringLiteral("Characters that end a word for double-click selection, besides spaces")),
        {QStringLiteral("env"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Variables added to the shell's environment, e.g. { \"EDITOR\": \"nvim\" }")},
        {QStringLiteral("confirmUnsafePaste"), true, QMetaType::Bool,
         QStringLiteral("Ask before pasting several lines into a program that would run them")},
        stringKey(QStringLiteral("fileTransferPassword"), {}, QStringLiteral("Shared secret for Kitty file-transfer approval. Empty always asks for approval.")),
        {QStringLiteral("clipboardRead"), QStringLiteral("ask"), QMetaType::QString,
         QStringLiteral("Programs reading the clipboard (OSC 52): ask, allow or deny"),
         [](const QVariant& v) {
             const QString s = v.toString();
             return (s == QLatin1String("allow") || s == QLatin1String("deny")) ? QVariant(s) : QVariant(QStringLiteral("ask"));
         },
         {{QStringLiteral("ask"), QStringLiteral("ask")},
          {QStringLiteral("allow"), QStringLiteral("allow")},
          {QStringLiteral("deny"), QStringLiteral("deny")}}},
    };
    schema.groups[Window] = {
        intKey(QStringLiteral("padding"), 6, 0, 200, QStringLiteral("Space around the grid in logical pixels")),
        intKey(QStringLiteral("initialColumns"), 100, 10, 1000, QStringLiteral("Columns of a new window")),
        intKey(QStringLiteral("initialRows"), 30, 4, 500, QStringLiteral("Rows of a new window")),
        doubleKey(QStringLiteral("backgroundOpacity"), 1.0, 0.05, 1.0,
                  QStringLiteral("Opacity of the default background. Text stays opaque")),
        {QStringLiteral("blur"), false, QMetaType::Bool,
         QStringLiteral("Blur content behind translucent backgrounds with KWin.")},
        {QStringLiteral("paneTitles"), false, QMetaType::Bool,
         QStringLiteral("Show pane titles in tabs with multiple splits.")},
        stringKey(QStringLiteral("workingDirectory"), QStringLiteral("inherit"),
                  QStringLiteral("Starting directory for new tabs, splits and windows. Use inherit, home or a path. Inherit uses the "
                                 "focused pane's directory or launcher's directory. The afterglow -d option overrides this setting.")),
        enumKey(QStringLiteral("startMode"), QStringLiteral("normal"), QStringLiteral("How new windows open: normal, maximized or fullscreen"),
                {QStringLiteral("normal"), QStringLiteral("maximized"), QStringLiteral("fullscreen")}),
        {QStringLiteral("decorations"), true, QMetaType::Bool,
         QStringLiteral("The compositor's title bar and borders (new windows)")},
        {QStringLiteral("balancePadding"), false, QMetaType::Bool,
         QStringLiteral("Distribute unused cell space between opposite edges. Otherwise, space remains at the right and "
                                 "bottom.")},
        enumKey(QStringLiteral("tabBar"), QStringLiteral("auto"),
                QStringLiteral("Tab-bar visibility. The auto value shows the bar when multiple tabs exist."),
                {QStringLiteral("auto"), QStringLiteral("always"), QStringLiteral("never")}),
        enumKey(QStringLiteral("scrollbar"), QStringLiteral("auto"),
                QStringLiteral("History scrollbar visibility. The auto value shows the scrollbar while the view scrolls or the "
                               "pointer is over it, then fades it out."),
                {QStringLiteral("auto"), QStringLiteral("always"), QStringLiteral("never")}),
        intKey(QStringLiteral("scrollbarHideDelay"), 1200, 0, 60000,
               QStringLiteral("Milliseconds an auto scrollbar stays visible after the last scroll or after the pointer leaves")),
    };
    schema.groups[MouseGroup] = {
        enumKey(QStringLiteral("rightClick"), QStringLiteral("menu"),
                QStringLiteral("Right-click action. The copy-or-paste action copies an existing selection or pastes otherwise. The "
                                 "extend action extends the selection."),
                {QStringLiteral("menu"), QStringLiteral("paste"), QStringLiteral("copy-or-paste"), QStringLiteral("extend")}),
        enumKey(QStringLiteral("middleClick"), QStringLiteral("primary"),
                QStringLiteral("Middle-click action. Paste the primary selection, paste the clipboard, or do nothing."),
                {QStringLiteral("primary"), QStringLiteral("clipboard"), QStringLiteral("none")}),
    };
    schema.groups[LinksGroup] = {
        {QStringLiteral("detectUrls"), true, QMetaType::Bool,
         QStringLiteral("Detect plain-text URLs in addition to application-provided OSC 8 links.")},
        stringKey(QStringLiteral("openWith"), {},
                  QStringLiteral("Command for opening links. The URL is appended. An empty value uses the desktop browser.")),
        enumKey(QStringLiteral("modifier"), QStringLiteral("ctrl"),
                QStringLiteral("Modifier for link highlighting and activation. With none, a plain click opens the link."),
                {QStringLiteral("ctrl"), QStringLiteral("shift"), QStringLiteral("alt"), QStringLiteral("none")}),
    };
    schema.groups[Quick] = {
        doubleKey(QStringLiteral("height"), 0.4, 0.1, 1.0, QStringLiteral("Height as a fraction of the screen")),
        stringKey(QStringLiteral("shortcut"), QStringLiteral("Meta+`"),
                  QStringLiteral("Global shortcut to show or hide the quick terminal. An empty value disables the shortcut.")),
        {QStringLiteral("animate"), true, QMetaType::Bool, QStringLiteral("Animate quick-terminal opening and closing.")},
    };
    schema.groups[ControlGroup] = {
        {QStringLiteral("enabled"), true, QMetaType::Bool,
         QStringLiteral("Allow other programs to control Afterglow through D-Bus and afterglow ctl.")},
        {QStringLiteral("allowReadText"), false, QMetaType::Bool,
         QStringLiteral("Allow D-Bus clients to read pane text, including scrollback.")},
    };
    schema.groups[ScriptsGroup] = {
        {QStringLiteral("allowReadText"), true, QMetaType::Bool,
         QStringLiteral("Allow local Luau scripts to query pane text, selections and semantic regions")},
        {QStringLiteral("enabled"), true, QMetaType::Bool,
         QStringLiteral("Run Luau scripts from ~/.config/afterglow/scripts")},
    };
    schema.groups[AudioGroup] = {
        {QStringLiteral("enabled"), true, QMetaType::Bool,
         QStringLiteral("Analyze system playback when a visible shader requests audio. Requires cava.")},
        enumKey(QStringLiteral("inputMethod"), QStringLiteral("auto"), QStringLiteral("Audio capture backend"),
                {QStringLiteral("auto"), QStringLiteral("pipewire"), QStringLiteral("pulse")}),
        stringKey(QStringLiteral("inputSource"), QStringLiteral("auto"),
                  QStringLiteral("Playback source for capture. The auto value follows the default output.")),
    };
    schema.groups[Animation] = {
        {QStringLiteral("cursor"), true, QMetaType::Bool, QStringLiteral("Animate cursor movement between cells.")},
        {QStringLiteral("scroll"), true, QMetaType::Bool, QStringLiteral("Animate scrolling between rows.")},
        {QStringLiteral("splits"), true, QMetaType::Bool, QStringLiteral("Animate split opening, closing and resizing.")},
        stringKey(QStringLiteral("splitCurve"), QStringLiteral("spring:22,1"),
                  QStringLiteral("Curve of split animations, as a Phosphor curve spec: a spring such as spring:22,1 (angular frequency, "
                                 "damping ratio), a cubic Bezier such as 0.2,0,0,1, or a named easing such as elastic-out. An "
                                 "invalid spec uses the default.")),
        intKey(QStringLiteral("splitDuration"), 250, 16, 5000,
               QStringLiteral("Duration of split animations in milliseconds for curves other than springs, which settle by themselves")),
    };
    schema.groups[Shaders] = {
        {QStringLiteral("background"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane's background layer alone: cell backgrounds and images under the text")},
        {QStringLiteral("text"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane's text layer alone: glyphs, decorations and images over the text")},
        {QStringLiteral("cursor"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane's cursor layer alone")},
        {QStringLiteral("pane"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane: shader pack ids, such as cursor-trail and bloom")},
        {QStringLiteral("window"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Shaders run over the whole window, across splits, after the pane shaders")},
        {QStringLiteral("params"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Per shader parameter values, e.g. { \"bloom\": { \"strength\": 0.5 } }")},
    };
    schema.groups[QStringLiteral("Hints")] = {
        {QStringLiteral("patterns"), QVariantList{QVariantMap{
            {QStringLiteral("pattern"), QStringLiteral(R"((?:~?/|\.{1,2}/)?[\w.@+-]+(?:/[\w.@+-]+)*(?:\.[\w+-]+|:\d+)(?::\d+){0,2})")},
            {QStringLiteral("action"), QStringLiteral("copy")}}}, QMetaType::QVariantList,
         QStringLiteral("Up to 16 regular-expression hints with copy, open or command actions. A command uses an argv list.")},
    };
    schema.groups[Keys] = {
        stringKey(QStringLiteral("leader"), {}, QStringLiteral("Prefix key used by Leader sequences")),
        intKey(QStringLiteral("sequenceTimeout"), 1000, 100, 10000, QStringLiteral("Milliseconds before an incomplete sequence is cancelled")),
        intKey(QStringLiteral("tableTimeout"), 5000, 100, 60000, QStringLiteral("Idle milliseconds before leaving a key table")),
        {QStringLiteral("tables"), QVariantMap{}, QMetaType::QVariantMap, QStringLiteral("Named action-to-key maps, activated with table:name")},
        {QStringLiteral("bindings"), Keybinds::defaultMap(), QMetaType::QVariantMap,
         QStringLiteral("Map each action to a key combination or list. An empty string removes the binding.")},
    };
    return schema;
}

AfterglowVt::Rgb toRgb(const QString& name, AfterglowVt::Rgb fallback)
{
    const QColor c(name);
    return c.isValid() ? AfterglowVt::Rgb{std::uint8_t(c.red()), std::uint8_t(c.green()), std::uint8_t(c.blue())}
                       : fallback;
}

} // namespace

QString SettingsStore::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/afterglow/config.json");
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

    if (!QFile::exists(path)) {
        // A complete file with every default, so all options are discoverable.
        if (!PhosphorConfig::JsonBackend::writeJsonAtomically(path, m_store->defaultsToJson())) {
            qCWarning(lcConfig).noquote() << "cannot write" << path << "- running with defaults";
        }
    }
    m_backend->reparseConfiguration();
    m_settings = read();

    connect(&m_files, &PhosphorFsLoader::WatchedFileSet::filesChanged, this, &SettingsStore::reload);
    if (qGuiApp) {
        // "light:a,dark:b" themes follow the desktop.
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &SettingsStore::reload);
    }
    watch();
}

SettingsStore::~SettingsStore() = default;

bool SettingsStore::addOverride(QJsonObject& overrides, const QString& assignment)
{
    const qsizetype eq = assignment.indexOf(QLatin1Char('='));
    const QString path = assignment.left(eq).trimmed();
    const qsizetype dot = path.indexOf(QLatin1Char('.'));
    if (eq < 0 || dot <= 0 || dot == path.size() - 1) {
        return false;
    }
    const QString raw = assignment.mid(eq + 1);
    // JSON when it parses as a value (numbers, booleans, lists, objects), else a string.
    const QJsonDocument doc = QJsonDocument::fromJson(QStringLiteral("[%1]").arg(raw).toUtf8());
    const QJsonValue value = doc.isArray() && doc.array().size() == 1 ? doc.array().first() : QJsonValue(raw);
    QJsonObject group = overrides.value(path.left(dot)).toObject();
    group.insert(path.mid(dot + 1), value);
    overrides.insert(path.left(dot), group);
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
    const auto name = Themes::pick(spec, darkScheme());
    if (!name.isEmpty()) {
        if (name.contains(QLatin1Char('/'))) {
            paths << (name.startsWith(QLatin1String("~/")) ? QDir::homePath() + name.mid(1) : name);
        } else {
            for (const auto& directory : Themes::searchDirs())
                for (const auto& suffix : {QStringLiteral(".json"), QStringLiteral(".conf"), QString()})
                    paths << directory + QLatin1Char('/') + name + suffix;
        }
    }
    m_files.setPaths(paths);
}

void SettingsStore::reload()
{
    QFile f(m_path);
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError err{};
        QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError) {
            qCWarning(lcConfig).noquote() << m_path << "does not parse:" << err.errorString() << "at offset"
                                          << err.offset << "- keeping the current settings";
            return;
        }
    }
    m_backend->reparseConfiguration();
    const Settings previous = m_settings;
    m_settings = read();
    watch(); // the theme file may have changed
    qCInfo(lcConfig).noquote() << "reloaded" << m_path;
    Q_EMIT changed(previous);
}

Settings SettingsStore::read() const
{
    const PhosphorConfig::Store& st = *m_store;
    Settings s;

    // Font
    auto& f = s.font;
    const QString family = st.read<QString>(Font, QStringLiteral("family"));
    const QString bold = st.read<QString>(Font, QStringLiteral("boldFamily"));
    const QString italic = st.read<QString>(Font, QStringLiteral("italicFamily"));
    const QString boldItalic = st.read<QString>(Font, QStringLiteral("boldItalicFamily"));
    const float weight = float(st.read<int>(Font, QStringLiteral("weight")));
    const float boldWeight = float(st.read<int>(Font, QStringLiteral("boldWeight")));
    const float width = float(st.read<double>(Font, QStringLiteral("width")));
    const float slant = float(st.read<double>(Font, QStringLiteral("italicSlant")));
    const std::string fam = family.toStdString();
    const std::string italicFam = italic.isEmpty() ? fam : italic.toStdString();
    f.sizePoints = float(st.read<double>(Font, QStringLiteral("size")));
    f.regular = {fam, weight, width, 0};
    f.bold = {bold.isEmpty() ? fam : bold.toStdString(), boldWeight, width, 0};
    f.italic = {italicFam, weight, width, slant};
    f.boldItalic = {boldItalic.isEmpty() ? italicFam : boldItalic.toStdString(), boldWeight, width, slant};
    f.features.clear();
    for (const QVariant& v : st.read<QVariantList>(Font, QStringLiteral("features"))) {
        f.features.push_back(v.toString().toStdString());
    }
    for (const QVariant& v : st.read<QVariantList>(Font, QStringLiteral("fallbackFamilies"))) {
        if (const QString family = v.toString().trimmed(); !family.isEmpty()) {
            f.fallbackFamilies.push_back(family.toStdString());
        }
    }
    const QVariantMap symbolMap = st.read<QVariantMap>(Font, QStringLiteral("symbolMap"));
    for (auto it = symbolMap.cbegin(); it != symbolMap.cend(); ++it) {
        const QString family = it.value().toString().trimmed();
        const auto ranges = parseCodepointRanges(it.key());
        if (!ranges || family.isEmpty()) {
            qCWarning(lcConfig).noquote() << "symbolMap: ignoring" << it.key() << "- expected U+XXXX or U+XXXX-U+YYYY ranges and a family";
            continue;
        }
        for (const auto& [first, last] : *ranges) {
            f.symbolMap.push_back({first, last, family.toStdString()});
        }
    }
    const QString hinting = st.read<QString>(Font, QStringLiteral("hinting"));
    f.hinting = hinting == QLatin1String("none")   ? AfterglowRender::Hinting::None
                : hinting == QLatin1String("full") ? AfterglowRender::Hinting::Full
                                                   : AfterglowRender::Hinting::Light;
    const QString aa = st.read<QString>(Font, QStringLiteral("antialiasing"));
    f.antialiasing = aa == QLatin1String("none") ? AfterglowRender::Antialiasing::None
                    : aa == QLatin1String("rgb") ? AfterglowRender::Antialiasing::Rgb
                    : aa == QLatin1String("bgr") ? AfterglowRender::Antialiasing::Bgr
                                                 : AfterglowRender::Antialiasing::Grayscale;
    f.gamma = float(st.read<double>(Font, QStringLiteral("gamma")));
    f.cellHeightScale = float(st.read<double>(Font, QStringLiteral("cellHeight")));
    f.cellWidthScale = float(st.read<double>(Font, QStringLiteral("cellWidth")));

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
    AfterglowVt::Rgb divider = toRgb(st.read<QString>(Colors, QStringLiteral("divider")), {0x3a, 0x3a, 0x42});

    // A theme's colours replace the ones above.
    if (const QString spec = st.read<QString>(Colors, QStringLiteral("theme")).trimmed(); !spec.isEmpty()) {
        const QString name = Themes::pick(spec, darkScheme());
        const QString path = Themes::locate(name, Themes::searchDirs());
        QFile file(path);
        QString error;
        std::optional<Theme> theme;
        if (path.isEmpty()) {
            error = QStringLiteral("not found in %1").arg(Themes::searchDirs().join(QStringLiteral(", ")));
        } else if (!file.open(QIODevice::ReadOnly)) {
            error = file.errorString();
        } else {
            theme = Themes::parse(file.readAll(), &error);
        }
        if (!theme) {
            qCWarning(lcConfig).noquote() << "theme" << name << "-" << error;
        } else {
            s.themePath = path;
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
            // Themes from other terminals have no divider: a fifth of the way
            // from the background to the text suits light and dark alike.
            auto mix = [](std::uint8_t a, std::uint8_t b) { return std::uint8_t(a + (b - a) / 5); };
            const AfterglowVt::Rgb& bg = t.defaultBackground;
            const AfterglowVt::Rgb& fg = t.defaultForeground;
            divider = theme->divider.value_or(AfterglowVt::Rgb{mix(bg.r, fg.r), mix(bg.g, fg.g), mix(bg.b, fg.b)});
        }
    }
    s.window.dividerColor = AfterglowRender::packRgba(divider);
    s.window.unfocusedDim = st.read<double>(Colors, QStringLiteral("unfocusedDim"));
    t.boldIsBright = st.read<bool>(Colors, QStringLiteral("boldIsBright"));
    t.minimumContrast = st.read<double>(Colors, QStringLiteral("minimumContrast"));

    // Terminal
    t.maxScrollback = std::size_t(st.read<int>(Terminal, QStringLiteral("scrollback")));
    const QString shape = st.read<QString>(Terminal, QStringLiteral("cursorShape"));
    t.cursorShape = shape == QLatin1String("bar")         ? AfterglowVt::CursorShape::Bar
                    : shape == QLatin1String("underline") ? AfterglowVt::CursorShape::Underline
                                                          : AfterglowVt::CursorShape::Block;
    t.cursorBlink = st.read<bool>(Terminal, QStringLiteral("cursorBlink"));
    s.pager = st.read<QString>(Terminal, QStringLiteral("pager"));
    s.shellIntegration = st.read<bool>(Terminal, QStringLiteral("shellIntegration"));
    s.sshIntegration = st.read<bool>(Terminal, QStringLiteral("sshIntegration"));
    s.sshTerminfo = st.read<QString>(Terminal, QStringLiteral("sshTerminfo"));
    s.shell = st.read<QString>(Terminal, QStringLiteral("shell"));
    s.term = st.read<QString>(Terminal, QStringLiteral("term"));
    {
        const QString copy = st.read<QString>(Terminal, QStringLiteral("copyOnSelect"));
        s.copyOnSelect = copy == QLatin1String("clipboard") ? Settings::CopyOnSelect::Clipboard
            : copy == QLatin1String("off")                  ? Settings::CopyOnSelect::Off
                                                            : Settings::CopyOnSelect::Primary;
    }
    const QString bell = st.read<QString>(Terminal, QStringLiteral("bell"));
    s.bell = bell == QLatin1String("none")     ? Settings::Bell::None
             : bell == QLatin1String("visual") ? Settings::Bell::Visual
             : bell == QLatin1String("both")   ? Settings::Bell::Both
                                               : Settings::Bell::Attention;
    s.notifications = st.read<bool>(Terminal, QStringLiteral("notifications"));
    s.confirmUnsafePaste = st.read<bool>(Terminal, QStringLiteral("confirmUnsafePaste"));
    s.confirmClose = st.read<bool>(Terminal, QStringLiteral("confirmClose"));
    s.hideMouseWhileTyping = st.read<bool>(Terminal, QStringLiteral("hideMouseWhileTyping"));
    s.scrollLines = st.read<int>(Terminal, QStringLiteral("scrollLines"));
    s.cursorBlinkInterval = st.read<int>(Terminal, QStringLiteral("cursorBlinkInterval"));
    s.scrollOnOutput = st.read<bool>(Terminal, QStringLiteral("scrollOnOutput"));
    s.scrollOnInput = st.read<bool>(Terminal, QStringLiteral("scrollOnInput"));
    t.ambiguousWide = st.read<bool>(Terminal, QStringLiteral("ambiguousWide"));
    s.bellSound = st.read<QString>(Terminal, QStringLiteral("bellSound")).trimmed();
    t.wordBoundaries = st.read<QString>(Terminal, QStringLiteral("wordSeparators")).toStdU32String();
    const QVariantMap env = st.read<QVariantMap>(Terminal, QStringLiteral("env"));
    for (auto it = env.cbegin(); it != env.cend(); ++it) {
        if (!it.key().isEmpty() && !it.key().contains(QLatin1Char('='))) {
            s.environment.emplace_back(it.key().toStdString(), it.value().toString().toStdString());
        }
    }
    s.fileTransferPassword = st.read<QString>(Terminal, QStringLiteral("fileTransferPassword"));
    s.clipboardRead = st.read<QString>(Terminal, QStringLiteral("clipboardRead"));

    // Window
    s.window.padding = st.read<int>(Window, QStringLiteral("padding"));
    s.window.initialColumns = st.read<int>(Window, QStringLiteral("initialColumns"));
    s.window.initialRows = st.read<int>(Window, QStringLiteral("initialRows"));

    s.window.backgroundOpacity = st.read<double>(Window, QStringLiteral("backgroundOpacity"));
    s.window.blur = st.read<bool>(Window, QStringLiteral("blur"));
    s.window.paneTitles = st.read<bool>(Window, QStringLiteral("paneTitles"));
    s.window.workingDirectory = st.read<QString>(Window, QStringLiteral("workingDirectory")).trimmed();
    const QString startMode = st.read<QString>(Window, QStringLiteral("startMode"));
    s.window.startMode = startMode == QLatin1String("maximized")    ? WindowSettings::StartMode::Maximized
                         : startMode == QLatin1String("fullscreen") ? WindowSettings::StartMode::Fullscreen
                                                                    : WindowSettings::StartMode::Normal;
    s.window.decorations = st.read<bool>(Window, QStringLiteral("decorations"));
    s.window.balancePadding = st.read<bool>(Window, QStringLiteral("balancePadding"));
    const QString right = st.read<QString>(MouseGroup, QStringLiteral("rightClick"));
    s.mouse.rightClick = right == QLatin1String("paste")           ? MouseSettings::RightClick::Paste
                         : right == QLatin1String("copy-or-paste") ? MouseSettings::RightClick::CopyOrPaste
                         : right == QLatin1String("extend")        ? MouseSettings::RightClick::Extend
                                                                   : MouseSettings::RightClick::Menu;
    const QString middle = st.read<QString>(MouseGroup, QStringLiteral("middleClick"));
    s.mouse.middleClick = middle == QLatin1String("clipboard") ? MouseSettings::MiddleClick::Clipboard
                          : middle == QLatin1String("none")    ? MouseSettings::MiddleClick::None
                                                               : MouseSettings::MiddleClick::Primary;
    s.links.detectUrls = st.read<bool>(LinksGroup, QStringLiteral("detectUrls"));
    s.links.openWith = st.read<QString>(LinksGroup, QStringLiteral("openWith")).trimmed();
    const QString modifier = st.read<QString>(LinksGroup, QStringLiteral("modifier"));
    s.links.modifier = modifier == QLatin1String("shift") ? Qt::ShiftModifier
                       : modifier == QLatin1String("alt") ? Qt::AltModifier
                       : modifier == QLatin1String("none") ? Qt::NoModifier
                                                           : Qt::ControlModifier;
    const QString tabBar = st.read<QString>(Window, QStringLiteral("tabBar"));
    s.window.tabBar = tabBar == QLatin1String("always") ? WindowSettings::TabBar::Always
                      : tabBar == QLatin1String("never") ? WindowSettings::TabBar::Never
                                                         : WindowSettings::TabBar::Auto;
    const QString scrollbar = st.read<QString>(Window, QStringLiteral("scrollbar"));
    s.window.scrollbar = scrollbar == QLatin1String("always") ? WindowSettings::Scrollbar::Always
                         : scrollbar == QLatin1String("never") ? WindowSettings::Scrollbar::Never
                                                               : WindowSettings::Scrollbar::Auto;
    s.window.scrollbarHideDelay = st.read<int>(Window, QStringLiteral("scrollbarHideDelay"));
    s.quickTerminal.height = st.read<double>(Quick, QStringLiteral("height"));
    s.quickTerminal.shortcut = st.read<QString>(Quick, QStringLiteral("shortcut"));
    s.quickTerminal.animate = st.read<bool>(Quick, QStringLiteral("animate"));
    s.control.enabled = st.read<bool>(ControlGroup, QStringLiteral("enabled"));
    s.control.allowReadText = st.read<bool>(ControlGroup, QStringLiteral("allowReadText"));
    s.scripts = st.read<bool>(ScriptsGroup, QStringLiteral("enabled"));
    s.scriptsAllowReadText = st.read<bool>(ScriptsGroup, QStringLiteral("allowReadText"));
    const auto bidi = st.read<QString>(Terminal, QStringLiteral("bidi"));
    s.terminal.bidi = bidi == QLatin1String("auto") ? AfterglowVt::BidiMode::Auto
        : bidi == QLatin1String("ltr") ? AfterglowVt::BidiMode::LeftToRight
        : bidi == QLatin1String("rtl") ? AfterglowVt::BidiMode::RightToLeft : AfterglowVt::BidiMode::Off;

    s.audio.enabled = st.read<bool>(AudioGroup, QStringLiteral("enabled"));
    s.audio.inputMethod = st.read<QString>(AudioGroup, QStringLiteral("inputMethod"));
    s.audio.inputSource = st.read<QString>(AudioGroup, QStringLiteral("inputSource"));

    // Animation
    s.animation.cursor = st.read<bool>(Animation, QStringLiteral("cursor"));
    s.animation.scroll = st.read<bool>(Animation, QStringLiteral("scroll"));
    s.animation.splits = st.read<bool>(Animation, QStringLiteral("splits"));
    s.animation.splitCurve = st.read<QString>(Animation, QStringLiteral("splitCurve")).trimmed();
    s.animation.splitDuration = st.read<int>(Animation, QStringLiteral("splitDuration"));

    // Shaders
    auto shaderList = [&st](const char* key) {
        QStringList refs;
        for (const QVariant& v : st.read<QVariantList>(Shaders, QLatin1String(key))) {
            refs << v.toString();
        }
        return refs;
    };
    s.shaders = {shaderList("background"), shaderList("text"), shaderList("cursor"), shaderList("pane"),
                 shaderList("window")};
    s.shaderParams = st.read<QVariantMap>(Shaders, QStringLiteral("params"));

    s.hintPatterns = st.read<QVariantList>(QStringLiteral("Hints"), QStringLiteral("patterns")).mid(0, 16);
    // Keybinds
    QStringList errors;
    s.keybinds = Keybinds::fromMap(st.read<QVariantMap>(Keys, QStringLiteral("bindings")), &errors);
    s.keyModes = KeyModes::fromConfig(st.read<QVariantMap>(Keys, QStringLiteral("bindings")),
        st.read<QVariantMap>(Keys, QStringLiteral("tables")), st.read<QString>(Keys, QStringLiteral("leader")),
        st.read<int>(Keys, QStringLiteral("sequenceTimeout")), st.read<int>(Keys, QStringLiteral("tableTimeout")), s.keybinds, &errors);
    for (const QString& e : errors) {
        qCWarning(lcConfig).noquote() << "keybinds:" << e;
    }
    return s;
}

} // namespace Afterglow

namespace Afterglow {
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
}
