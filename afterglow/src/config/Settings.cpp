// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Settings.h"
#include "Theme.h"

#include <AfterglowRender/Snapshot.h>

#include <PhosphorConfig/JsonBackend.h>
#include <PhosphorConfig/Schema.h>
#include <PhosphorConfig/Store.h>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStyleHints>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QStandardPaths>

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
const QString ScriptsGroup = QStringLiteral("Scripts");

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
        stringKey(QStringLiteral("boldFamily"), {}, QStringLiteral("Bold font family; empty uses family")),
        stringKey(QStringLiteral("italicFamily"), {}, QStringLiteral("Italic font family; empty uses family")),
        stringKey(QStringLiteral("boldItalicFamily"), {}, QStringLiteral("Bold italic font family; empty uses the italic family")),
        doubleKey(QStringLiteral("size"), font.sizePoints, 4, 96, QStringLiteral("Size in points")),
        intKey(QStringLiteral("weight"), 400, 100, 900, QStringLiteral("Weight of regular text (variable fonts)")),
        intKey(QStringLiteral("boldWeight"), 700, 100, 900, QStringLiteral("Weight of bold text (variable fonts)")),
        doubleKey(QStringLiteral("width"), 100, 50, 200, QStringLiteral("Width axis (Monaspace: 100-125)")),
        doubleKey(QStringLiteral("italicSlant"), -11, -90, 90, QStringLiteral("Slant of italic text (variable fonts)")),
        {QStringLiteral("features"), features, QMetaType::QVariantList,
         QStringLiteral("OpenType features in HarfBuzz syntax: calt, liga, ss01, cv01=2, -liga")},
        {QStringLiteral("fallbackFamilies"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Families tried, in order, for characters the font lacks (symbols, CJK), before the system's choice")},
        doubleKey(QStringLiteral("cellHeight"), 1.0, 0.5, 3.0,
                  QStringLiteral("Row height against the font's own: 1.2 adds a fifth of a line between rows")),
        doubleKey(QStringLiteral("cellWidth"), 1.0, 0.5, 3.0, QStringLiteral("Column width against the font's own")),
    };
    AfterglowVt::TerminalOptions term;
    const auto hex = [](AfterglowVt::Rgb c) { return QColor(c.r, c.g, c.b).name(); };
    schema.groups[Colors] = {
        stringKey(QStringLiteral("theme"), {},
                  QStringLiteral("A theme from ~/.config/afterglow/themes or the bundled ones (afterglow-dark, afterglow-light), "
                                 "or light:NAME,dark:NAME to follow the desktop. Its colours replace the ones below. "
                                 "Reads afterglow JSON, Ghostty and kitty theme files")),
        colorKey(QStringLiteral("foreground"), hex(term.defaultForeground), QStringLiteral("Default text colour")),
        colorKey(QStringLiteral("background"), hex(term.defaultBackground), QStringLiteral("Default background colour")),
        colorKey(QStringLiteral("cursor"), {}, QStringLiteral("Cursor colour; empty follows the foreground")),
        colorKey(QStringLiteral("cursorText"), {}, QStringLiteral("Text under a block cursor; empty uses the background")),
        colorKey(QStringLiteral("selectionForeground"), {}, QStringLiteral("Selected text; empty inverts the cell")),
        colorKey(QStringLiteral("selectionBackground"), {}, QStringLiteral("Selection background; empty inverts the cell")),
        {QStringLiteral("palette"), defaultPalette(), QMetaType::QVariantList,
         QStringLiteral("The 16 ANSI colours, black to bright white")},
        colorKey(QStringLiteral("divider"), QStringLiteral("#3a3a42"), QStringLiteral("Split divider colour")),
        doubleKey(QStringLiteral("unfocusedDim"), 0.19, 0, 1, QStringLiteral("Darkening over unfocused splits")),
        {QStringLiteral("boldIsBright"), false, QMetaType::Bool,
         QStringLiteral("Bold text in colours 0-7 uses their bright versions, 8-15")},
        doubleKey(QStringLiteral("minimumContrast"), 1.0, 1.0, 21.0,
                  QStringLiteral("Lowest contrast ratio between text and its background; text below it is lightened or "
                                 "darkened. 1 leaves colours alone, 4.5 is WCAG AA")),
    };
    schema.groups[Terminal] = {
        intKey(QStringLiteral("scrollback"), 10000, 0, 1000000, QStringLiteral("Lines of history per pane")),
        stringKey(QStringLiteral("shell"), {}, QStringLiteral("Program to run; empty runs your login shell")),
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
         QStringLiteral("Copy a selection when it is made: primary (middle-click paste), clipboard (both), or off"),
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
         QStringLiteral("Hide the mouse pointer while typing; moving it shows it again")},
        intKey(QStringLiteral("scrollLines"), 3, 1, 50, QStringLiteral("Lines a mouse wheel notch scrolls")),
        stringKey(QStringLiteral("wordSeparators"), wordSeparators,
                  QStringLiteral("Characters that end a word for double-click selection, besides spaces")),
        {QStringLiteral("env"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Variables added to the shell's environment, e.g. { \"EDITOR\": \"nvim\" }")},
        {QStringLiteral("confirmUnsafePaste"), true, QMetaType::Bool,
         QStringLiteral("Ask before pasting several lines into a program that would run them")},
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
                  QStringLiteral("Opacity of the default background; text stays opaque")),
        {QStringLiteral("blur"), false, QMetaType::Bool,
         QStringLiteral("Blur what shows through a translucent background (KWin)")},
        {QStringLiteral("paneTitles"), false, QMetaType::Bool,
         QStringLiteral("Label each split with its title while a tab has several")},
        stringKey(QStringLiteral("workingDirectory"), QStringLiteral("inherit"),
                  QStringLiteral("Where new tabs, splits and windows start: inherit (the focused pane's directory, or "
                                 "the launcher's), home, or a path. afterglow -d overrides it")),
        enumKey(QStringLiteral("startMode"), QStringLiteral("normal"), QStringLiteral("How new windows open: normal, maximized or fullscreen"),
                {QStringLiteral("normal"), QStringLiteral("maximized"), QStringLiteral("fullscreen")}),
        {QStringLiteral("decorations"), true, QMetaType::Bool,
         QStringLiteral("The compositor's title bar and borders (new windows)")},
        enumKey(QStringLiteral("tabBar"), QStringLiteral("auto"),
                QStringLiteral("Show the tab bar: auto (while there are several tabs), always or never"),
                {QStringLiteral("auto"), QStringLiteral("always"), QStringLiteral("never")}),
    };
    schema.groups[Quick] = {
        doubleKey(QStringLiteral("height"), 0.4, 0.1, 1.0, QStringLiteral("Height as a fraction of the screen")),
        stringKey(QStringLiteral("shortcut"), QStringLiteral("Meta+`"),
                  QStringLiteral("Global shortcut that shows and hides it; empty for none")),
        {QStringLiteral("animate"), true, QMetaType::Bool, QStringLiteral("Slide in and out")},
    };
    schema.groups[ControlGroup] = {
        {QStringLiteral("enabled"), true, QMetaType::Bool,
         QStringLiteral("Let other programs control afterglow over D-Bus (afterglow ctl)")},
        {QStringLiteral("allowReadText"), false, QMetaType::Bool,
         QStringLiteral("Let them read what panes show, scrollback included")},
    };
    schema.groups[ScriptsGroup] = {
        {QStringLiteral("enabled"), true, QMetaType::Bool,
         QStringLiteral("Run Luau scripts from ~/.config/afterglow/scripts")},
    };
    schema.groups[Animation] = {
        {QStringLiteral("cursor"), true, QMetaType::Bool, QStringLiteral("The cursor glides between cells")},
        {QStringLiteral("scroll"), true, QMetaType::Bool, QStringLiteral("Scrolling eases instead of jumping by rows")},
        {QStringLiteral("splits"), true, QMetaType::Bool, QStringLiteral("Splits open, close and resize smoothly")},
    };
    schema.groups[Shaders] = {
        {QStringLiteral("background"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane's background layer alone: cell backgrounds and images under the text")},
        {QStringLiteral("text"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane's text layer alone: glyphs, decorations and images over the text")},
        {QStringLiteral("cursor"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane's cursor layer alone")},
        {QStringLiteral("pane"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane: pack ids (cursor-trail, bloom) or paths to Ghostty shader files")},
        {QStringLiteral("window"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Shaders run over the whole window, across splits, after the pane shaders")},
        {QStringLiteral("params"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Per shader parameter values, e.g. { \"bloom\": { \"strength\": 0.5 } }")},
    };
    schema.groups[Keys] = {
        {QStringLiteral("bindings"), Keybinds::defaultMap(), QMetaType::QVariantMap,
         QStringLiteral("Action to key combination, or a list of them; an empty string unbinds")},
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
    : QObject(parent)
    , m_path(path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    m_backend = std::make_unique<PhosphorConfig::JsonBackend>(path);
    m_store = std::make_unique<PhosphorConfig::Store>(m_backend.get(), buildSchema());

    if (!QFile::exists(path)) {
        // A complete file with every default, so all options are discoverable.
        if (PhosphorConfig::JsonBackend::writeJsonAtomically(path, m_store->defaultsToJson())) {
            m_backend->reparseConfiguration();
        } else {
            qCWarning(lcConfig).noquote() << "cannot write" << path << "- running with defaults";
        }
    }
    m_settings = read();

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(120);
    connect(&m_debounce, &QTimer::timeout, this, &SettingsStore::reload);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    if (qGuiApp) {
        // "light:a,dark:b" themes follow the desktop.
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] { m_debounce.start(); });
    }
    watch();
}

SettingsStore::~SettingsStore() = default;

bool SettingsStore::darkScheme() const
{
    // Unknown (no desktop preference) counts as dark, afterglow's own default.
    return !qGuiApp || QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Light;
}

void SettingsStore::watch()
{
    // Editors often replace the file instead of writing it, which drops the
    // file watch; watching the directory catches that and re-adds the file.
    const QString dir = QFileInfo(m_path).absolutePath();
    if (!m_watcher.directories().contains(dir)) {
        m_watcher.addPath(dir);
    }
    if (QFile::exists(m_path) && !m_watcher.files().contains(m_path)) {
        m_watcher.addPath(m_path);
    }
    // The theme in use, and the user's themes folder for themes added later.
    const QString themes = Themes::searchDirs().constFirst();
    if (QFileInfo(themes).isDir() && !m_watcher.directories().contains(themes)) {
        m_watcher.addPath(themes);
    }
    const QString theme = m_settings.themePath;
    if (!theme.isEmpty() && QFile::exists(theme) && !m_watcher.files().contains(theme)) {
        m_watcher.addPath(theme);
    }
}

void SettingsStore::reload()
{
    watch();
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
    t.wordBoundaries = st.read<QString>(Terminal, QStringLiteral("wordSeparators")).toStdU32String();
    const QVariantMap env = st.read<QVariantMap>(Terminal, QStringLiteral("env"));
    for (auto it = env.cbegin(); it != env.cend(); ++it) {
        if (!it.key().isEmpty() && !it.key().contains(QLatin1Char('='))) {
            s.environment.emplace_back(it.key().toStdString(), it.value().toString().toStdString());
        }
    }
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
    const QString tabBar = st.read<QString>(Window, QStringLiteral("tabBar"));
    s.window.tabBar = tabBar == QLatin1String("always") ? WindowSettings::TabBar::Always
                      : tabBar == QLatin1String("never") ? WindowSettings::TabBar::Never
                                                         : WindowSettings::TabBar::Auto;
    s.quickTerminal.height = st.read<double>(Quick, QStringLiteral("height"));
    s.quickTerminal.shortcut = st.read<QString>(Quick, QStringLiteral("shortcut"));
    s.quickTerminal.animate = st.read<bool>(Quick, QStringLiteral("animate"));
    s.control.enabled = st.read<bool>(ControlGroup, QStringLiteral("enabled"));
    s.control.allowReadText = st.read<bool>(ControlGroup, QStringLiteral("allowReadText"));
    s.scripts = st.read<bool>(ScriptsGroup, QStringLiteral("enabled"));

    // Animation
    s.animation.cursor = st.read<bool>(Animation, QStringLiteral("cursor"));
    s.animation.scroll = st.read<bool>(Animation, QStringLiteral("scroll"));
    s.animation.splits = st.read<bool>(Animation, QStringLiteral("splits"));

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

    // Keybinds
    QStringList errors;
    s.keybinds = Keybinds::fromMap(st.read<QVariantMap>(Keys, QStringLiteral("bindings")), &errors);
    for (const QString& e : errors) {
        qCWarning(lcConfig).noquote() << "keybinds:" << e;
    }
    return s;
}

} // namespace Afterglow
