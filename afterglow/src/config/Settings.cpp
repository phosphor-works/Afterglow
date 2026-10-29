// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Settings.h"

#include <AfterglowRender/Snapshot.h>

#include <PhosphorConfig/JsonBackend.h>
#include <PhosphorConfig/Schema.h>
#include <PhosphorConfig/Store.h>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    };
    AfterglowVt::TerminalOptions term;
    const auto hex = [](AfterglowVt::Rgb c) { return QColor(c.r, c.g, c.b).name(); };
    schema.groups[Colors] = {
        colorKey(QStringLiteral("foreground"), hex(term.defaultForeground), QStringLiteral("Default text colour")),
        colorKey(QStringLiteral("background"), hex(term.defaultBackground), QStringLiteral("Default background colour")),
        colorKey(QStringLiteral("cursor"), {}, QStringLiteral("Cursor colour; empty follows the foreground")),
        {QStringLiteral("palette"), defaultPalette(), QMetaType::QVariantList,
         QStringLiteral("The 16 ANSI colours, black to bright white")},
        colorKey(QStringLiteral("divider"), QStringLiteral("#3a3a42"), QStringLiteral("Split divider colour")),
        doubleKey(QStringLiteral("unfocusedDim"), 0.19, 0, 1, QStringLiteral("Darkening over unfocused splits")),
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
        {QStringLiteral("copyOnSelect"), true, QMetaType::Bool, QStringLiteral("Copy selections to the primary selection")},
    };
    schema.groups[Window] = {
        intKey(QStringLiteral("padding"), 6, 0, 200, QStringLiteral("Space around the grid in logical pixels")),
        intKey(QStringLiteral("initialColumns"), 100, 10, 1000, QStringLiteral("Columns of a new window")),
        intKey(QStringLiteral("initialRows"), 30, 4, 500, QStringLiteral("Rows of a new window")),
    };
    schema.groups[Shaders] = {
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
    watch();
}

SettingsStore::~SettingsStore() = default;

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
    const AfterglowVt::Rgb divider = toRgb(st.read<QString>(Colors, QStringLiteral("divider")), {0x3a, 0x3a, 0x42});
    s.window.dividerColor = AfterglowRender::packRgba(divider);
    s.window.unfocusedDim = st.read<double>(Colors, QStringLiteral("unfocusedDim"));

    // Terminal
    t.maxScrollback = std::size_t(st.read<int>(Terminal, QStringLiteral("scrollback")));
    const QString shape = st.read<QString>(Terminal, QStringLiteral("cursorShape"));
    t.cursorShape = shape == QLatin1String("bar")         ? AfterglowVt::CursorShape::Bar
                    : shape == QLatin1String("underline") ? AfterglowVt::CursorShape::Underline
                                                          : AfterglowVt::CursorShape::Block;
    t.cursorBlink = st.read<bool>(Terminal, QStringLiteral("cursorBlink"));
    s.shell = st.read<QString>(Terminal, QStringLiteral("shell"));
    s.term = st.read<QString>(Terminal, QStringLiteral("term"));
    s.copyOnSelect = st.read<bool>(Terminal, QStringLiteral("copyOnSelect"));

    // Window
    s.window.padding = st.read<int>(Window, QStringLiteral("padding"));
    s.window.initialColumns = st.read<int>(Window, QStringLiteral("initialColumns"));
    s.window.initialRows = st.read<int>(Window, QStringLiteral("initialRows"));

    // Shaders
    for (const QVariant& v : st.read<QVariantList>(Shaders, QStringLiteral("pane"))) {
        s.paneShaders << v.toString();
    }
    for (const QVariant& v : st.read<QVariantList>(Shaders, QStringLiteral("window"))) {
        s.windowShaders << v.toString();
    }
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
