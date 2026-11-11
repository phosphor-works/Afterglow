// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Theme.h"

#include "app/BuildTree.h"

#include <AfterglowProto/Color.h>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

namespace Afterglow {

bool hasGuiApplication()
{
    return qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr;
}

} // namespace Afterglow

namespace Afterglow::Themes {

namespace {

std::optional<AfterglowVt::Rgb> color(QString value)
{
    value = value.trimmed();
    // Ghostty and kitty files often leave out the '#'.
    static const QRegularExpression BareHex(QStringLiteral("^[0-9a-fA-F]{6}$"));
    if (BareHex.match(value).hasMatch()) {
        value.prepend(QLatin1Char('#'));
    }
    if (!isColorName(value)) {
        return std::nullopt;
    }
    const QColor c = colorFromName(value);
    return AfterglowVt::Rgb{std::uint8_t(c.red()), std::uint8_t(c.green()), std::uint8_t(c.blue())};
}

enum class Assigned { Unused, Invalid, Set };

/// Sets the colour @p key names (in any of the three spellings) to @p value.
/// Unused for keys that are not colours afterglow uses, Invalid for one of
/// its keys with a value that is not a colour.
Assigned assign(Theme& theme, QString key, const QString& value)
{
    auto set = [](std::optional<AfterglowVt::Rgb>& target, const QString& text) {
        target = color(text);
        return target ? Assigned::Set : Assigned::Invalid;
    };
    key = key.toLower().replace(QLatin1Char('-'), QLatin1Char('_'));
    if (key == QLatin1String("palette")) {
        // Ghostty: "palette = 3=#c5a332".
        const qsizetype eq = value.indexOf(QLatin1Char('='));
        bool ok = false;
        const int index = value.left(eq).trimmed().toInt(&ok);
        if (eq < 0 || !ok) {
            return Assigned::Invalid;
        }
        // Entries 16 to 255 are the program's, not a theme's.
        if (index < 0 || index >= 16) {
            return Assigned::Unused;
        }
        return set(theme.palette[std::size_t(index)], value.mid(eq + 1));
    }
    if (key.startsWith(QLatin1String("color"))) {
        // kitty: "color3 #c5a332".
        bool ok = false;
        const int index = key.mid(5).toInt(&ok);
        if (!ok || index < 0 || index >= 16) {
            return Assigned::Unused;
        }
        return set(theme.palette[std::size_t(index)], value);
    }
    std::optional<AfterglowVt::Rgb>* target = nullptr;
    if (key == QLatin1String("foreground")) {
        target = &theme.foreground;
    } else if (key == QLatin1String("background")) {
        target = &theme.background;
    } else if (key == QLatin1String("cursor") || key == QLatin1String("cursor_color")) {
        target = &theme.cursor;
    } else if (key == QLatin1String("cursortext") || key == QLatin1String("cursor_text")
               || key == QLatin1String("cursor_text_color")) {
        target = &theme.cursorText;
    } else if (key == QLatin1String("selectionforeground") || key == QLatin1String("selection_foreground")) {
        target = &theme.selectionForeground;
    } else if (key == QLatin1String("selectionbackground") || key == QLatin1String("selection_background")) {
        target = &theme.selectionBackground;
    } else if (key == QLatin1String("divider")) {
        target = &theme.divider;
    } else if (key == QLatin1String("popupborder") || key == QLatin1String("popup_border")) {
        target = &theme.popupBorder;
    } else if (key == QLatin1String("searchmatch") || key == QLatin1String("search_match")) {
        target = &theme.searchMatch;
    } else if (key == QLatin1String("searchcurrentmatch") || key == QLatin1String("search_current_match")) {
        target = &theme.searchCurrentMatch;
    } else if (key == QLatin1String("searchmatchtext") || key == QLatin1String("search_match_text")) {
        target = &theme.searchMatchText;
    } else if (key == QLatin1String("visualbell") || key == QLatin1String("visual_bell")
               || key == QLatin1String("visual_bell_color")) {
        target = &theme.visualBell;
    } else {
        return Assigned::Unused;
    }
    return set(*target, value);
}

QByteArray withoutByteOrderMark(const QByteArray& data)
{
    // Some editors save UTF-8 with a byte-order mark.
    return data.startsWith("\xEF\xBB\xBF") ? data.mid(3) : data;
}

/// A colour written as KDE writes it, "r,g,b" with 0-255 parts (Konsole
/// schemes and kdeglobals), or any form color() reads.
std::optional<AfterglowVt::Rgb> kdeColor(const QString& value)
{
    const QStringList parts = value.split(QLatin1Char(','));
    if (parts.size() != 3 && parts.size() != 4) { // kdeglobals may add an alpha
        return color(value);
    }
    std::array<int, 3> rgb{};
    for (std::size_t i = 0; i < 3; ++i) {
        bool ok = false;
        rgb[i] = parts[qsizetype(i)].trimmed().toInt(&ok);
        if (!ok || rgb[i] < 0 || rgb[i] > 255) {
            return std::nullopt;
        }
    }
    return AfterglowVt::Rgb{std::uint8_t(rgb[0]), std::uint8_t(rgb[1]), std::uint8_t(rgb[2])};
}

/// The "[Section]" a line opens, or an empty string.
QString sectionOf(const QString& line)
{
    return line.size() > 2 && line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))
        ? line.mid(1, line.size() - 2).trimmed()
        : QString();
}

/// Where a Konsole section's colour goes: Foreground, Background, ColorN
/// (0-7) and ColorNIntense (the bright 8-15). nullptr for the sections that
/// Afterglow has no counterpart for (faint and intense text, General).
std::optional<AfterglowVt::Rgb>* konsoleTarget(Theme& theme, const QString& section)
{
    if (section == QLatin1String("Foreground")) {
        return &theme.foreground;
    }
    if (section == QLatin1String("Background")) {
        return &theme.background;
    }
    static const QRegularExpression Numbered(QStringLiteral("^Color([0-7])(Intense)?$"));
    if (const auto m = Numbered.match(section); m.hasMatch()) {
        return &theme.palette[std::size_t(m.captured(1).toInt() + (m.captured(2).isEmpty() ? 0 : 8))];
    }
    return nullptr;
}

} // namespace

QColor colorFromName(const QString& name)
{
    // Qt reads no form with a parenthesis.
    if (!name.contains(QLatin1Char('('))) return QColor::fromString(name);
    const QByteArray utf8 = name.toUtf8();
    std::uint8_t r = 0, g = 0, b = 0;
    if (!AfterglowProto::parseWideGamutColor(std::string_view(utf8.constData(), std::size_t(utf8.size())), r, g, b)) {
        return {};
    }
    return QColor(r, g, b);
}

bool isColorName(const QString& name)
{
    static const QRegularExpression EightDigits(QStringLiteral("^#[0-9a-fA-F]{8}$"));
    if (name.contains(QLatin1Char('('))) return colorFromName(name).isValid(); // OKLCH and LAB have no alpha
    // Transparent by its alpha, not by its spelling: Qt drops the spaces and
    // tabs of a name, so "trans parent" is transparent too.
    return QColor::isValidColorName(name) && !EightDigits.match(name).hasMatch()
        && QColor::fromString(name).alpha() == 255;
}

QString pick(const QString& spec, bool dark)
{
    // Each comma part is looked at alone, so a half may be a path.
    QString light;
    QString darkName;
    bool halves = false;
    for (const QString& part : spec.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const qsizetype colon = part.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            continue;
        }
        const QString which = part.left(colon).trimmed().toLower();
        const QString name = part.mid(colon + 1).trimmed();
        if (which == QLatin1String("light")) {
            light = name;
            halves = true;
        } else if (which == QLatin1String("dark")) {
            darkName = name;
            halves = true;
        }
    }
    if (!halves) {
        return spec.trimmed();
    }
    if (dark) {
        return darkName.isEmpty() ? light : darkName;
    }
    return light.isEmpty() ? darkName : light;
}

namespace {

QString bundledDirectory()
{
#if defined(AFTERGLOW_INSTALLED_THEME_DIR)
    // A build-tree binary reads the checkout; an installed one its own prefix,
    // which need not be in XDG_DATA_DIRS.
    return runningFromBuildTree() ? QStringLiteral(AFTERGLOW_SOURCE_THEME_DIR) : QStringLiteral(AFTERGLOW_INSTALLED_THEME_DIR);
#elif defined(AFTERGLOW_SOURCE_THEME_DIR)
    return QStringLiteral(AFTERGLOW_SOURCE_THEME_DIR);
#else
    return {};
#endif
}

/// @p sub in each XDG data directory, also where it does not exist yet:
/// SettingsStore watches these paths, and a theme saved into a directory
/// made later must still count (QStandardPaths::locateAll lists only
/// directories that exist).
QStringList dataDirs(const QString& sub)
{
    QStringList dirs;
    for (const QString& data : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        dirs << data + QLatin1Char('/') + sub;
    }
    return dirs;
}

} // namespace

QStringList searchDirs(const QString& configDirectory)
{
    // Beside the config file, as its scripts are: afterglow --config
    // DIR/config.json takes DIR/themes in place of the user's own.
    QStringList dirs{(configDirectory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                                                      + QStringLiteral("/afterglow")
                                                : configDirectory)
                     + QStringLiteral("/themes")};
    const QString bundled = bundledDirectory();
    // A build-tree binary puts the checkout above the data directories, so
    // that an installed copy does not shadow the themes that it is testing.
    const bool checkout = runningFromBuildTree();
    if (checkout && !bundled.isEmpty()) {
        dirs << bundled;
    }
    dirs << dataDirs(QStringLiteral("afterglow/themes"));
    if (!checkout && !bundled.isEmpty()) {
        dirs << bundled;
    }
    // The themes other terminals installed, after Afterglow's own, so that an
    // Afterglow theme of the same name wins: Ghostty's (its user directory and
    // share/ghostty/themes, where it ships its own), Kitty's custom themes and
    // Konsole's colour schemes.
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    dirs << config + QStringLiteral("/ghostty/themes");
    dirs << dataDirs(QStringLiteral("ghostty/themes"));
    dirs << config + QStringLiteral("/kitty/themes");
    dirs << dataDirs(QStringLiteral("konsole"));
    dirs.removeDuplicates();
    return dirs;
}

QStringList candidates(const QString& name, const QStringList& dirs, const QString& base)
{
    if (name.isEmpty()) {
        return {};
    }
    if (name.contains(QLatin1Char('/'))) {
        QString path = name.startsWith(QLatin1String("~/")) ? QDir::homePath() + name.mid(1) : name;
        // Relative to the config file, not to wherever Afterglow was started.
        if (QDir::isRelativePath(path) && !base.isEmpty()) {
            path = QDir(base).absoluteFilePath(path);
        }
        return {QDir::cleanPath(path)};
    }
    QStringList out;
    for (const QString& dir : dirs) {
        for (const QString& file : {name + QStringLiteral(".json"), name + QStringLiteral(".conf"),
                                    name + QStringLiteral(".colorscheme"), name}) {
            out << dir + QLatin1Char('/') + file;
        }
    }
    return out;
}

QString locate(const QString& name, const QStringList& dirs, const QString& base)
{
    for (const QString& path : candidates(name, dirs, base)) {
        if (QFileInfo(path).isFile()) {
            return path;
        }
    }
    return {};
}

bool isJson(const QByteArray& data)
{
    return withoutByteOrderMark(data).trimmed().startsWith('{');
}

bool isKonsole(const QByteArray& data)
{
    static const QRegularExpression Section(QStringLiteral("^\\[(Foreground|Background|Color[0-7](Intense)?)\\]\\s*$"),
                                            QRegularExpression::MultilineOption);
    return !isJson(data) && Section.match(QString::fromUtf8(withoutByteOrderMark(data))).hasMatch();
}

std::optional<Theme> parse(const QByteArray& data, QString* error, QStringList* ignored, QStringList* invalid)
{
    Theme theme;
    int found = 0;
    const QByteArray text = withoutByteOrderMark(data);
    auto count = [&](Assigned assigned, const QString& key) {
        if (assigned == Assigned::Set) {
            ++found;
        } else if (QStringList* list = assigned == Assigned::Unused ? ignored : invalid) {
            list->append(key);
        }
    };
    if (isJson(text)) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(text, &err);
        if (err.error != QJsonParseError::NoError) {
            if (error) {
                *error = err.errorString();
            }
            return std::nullopt;
        }
        const QJsonObject o = doc.object();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.key().compare(QLatin1String("palette"), Qt::CaseInsensitive) == 0) {
                if (!it.value().isArray()) {
                    if (invalid) {
                        invalid->append(it.key());
                    }
                    continue;
                }
                const QJsonArray palette = it.value().toArray();
                for (int i = 0; i < 16 && i < palette.size(); ++i) {
                    theme.palette[std::size_t(i)] = color(palette[i].toString());
                    count(theme.palette[std::size_t(i)] ? Assigned::Set : Assigned::Invalid,
                          QStringLiteral("palette[%1]").arg(i));
                }
            } else if (it.key().compare(QLatin1String("visualBellIntensity"), Qt::CaseInsensitive) == 0) {
                // A number, not a colour: it alone does not make a theme.
                if (const double v = it.value().toDouble(-1); v >= 0 && v <= 1) {
                    theme.visualBellIntensity = v;
                } else if (invalid) {
                    invalid->append(it.key());
                }
            } else {
                count(assign(theme, it.key(), it.value().toString()), it.key());
            }
        }
    } else if (isKonsole(text)) {
        QString section;
        for (const QByteArray& raw : text.split('\n')) {
            const QString line = QString::fromUtf8(raw).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
                continue;
            }
            if (const QString opened = sectionOf(line); !opened.isEmpty()) {
                section = opened;
                continue;
            }
            const qsizetype eq = line.indexOf(QLatin1Char('='));
            const QString key = line.left(eq).trimmed();
            if (eq < 0 || key != QLatin1String("Color")) {
                continue; // Description, Opacity, MaxRandomHue and the like
            }
            std::optional<AfterglowVt::Rgb>* target = konsoleTarget(theme, section);
            if (!target) {
                count(Assigned::Unused, section);
                continue;
            }
            *target = kdeColor(line.mid(eq + 1));
            count(*target ? Assigned::Set : Assigned::Invalid, section);
        }
    } else {
        // Ghostty writes "key = value"; kitty "key value", where a value can
        // be "oklch(0.7 0.1 200)" with spaces.
        static const QRegularExpression Ghostty(QStringLiteral("^([\\w-]+)\\s*=\\s*(.*)$"));
        static const QRegularExpression Kitty(QStringLiteral("^([\\w-]+)\\s+(\\w+\\([^)]*\\)|\\S+)"));
        for (const QByteArray& raw : text.split('\n')) {
            const QString line = QString::fromUtf8(raw).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
                continue;
            }
            QRegularExpressionMatch m = Ghostty.match(line);
            if (!m.hasMatch()) {
                m = Kitty.match(line);
            }
            if (!m.hasMatch()) {
                continue;
            }
            count(assign(theme, m.captured(1), m.captured(2).trimmed()), m.captured(1));
        }
    }
    if (found == 0) {
        if (error) {
            *error = QStringLiteral("no colors found");
        }
        return std::nullopt;
    }
    return theme;
}

bool isPlasma(const QString& name, QString* base)
{
    static const QLatin1String Prefix("plasma");
    // In any case, as the light: and dark: of pick().
    if (name.compare(Prefix, Qt::CaseInsensitive) != 0 && !name.startsWith(Prefix + QLatin1Char(':'), Qt::CaseInsensitive)) {
        return false;
    }
    if (base) {
        *base = name.mid(Prefix.size() + 1).trimmed();
    }
    return true;
}

QStringList plasmaSources()
{
    // locateAll lists the user's file first; KDE reads the system's first.
    QStringList files = QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation, QStringLiteral("kdeglobals"));
    const QString user = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/kdeglobals");
    files.removeAll(user);
    std::reverse(files.begin(), files.end());
    files << user;
    return files;
}

Theme plasmaColors(const QStringList& sources)
{
    std::optional<AfterglowVt::Rgb> background, foreground, accent, selectionBackground, selectionForeground;
    for (const QString& path : sources) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        QString section;
        for (const QByteArray& raw : file.readAll().split('\n')) {
            const QString line = QString::fromUtf8(raw).trimmed();
            if (const QString opened = sectionOf(line); !opened.isEmpty()) {
                section = opened;
                continue;
            }
            const qsizetype eq = line.indexOf(QLatin1Char('='));
            if (eq < 0 || (section != QLatin1String("Colors:View") && section != QLatin1String("Colors:Selection"))) {
                continue;
            }
            const QString key = line.left(eq).trimmed();
            const std::optional<AfterglowVt::Rgb> value = kdeColor(line.mid(eq + 1));
            if (!value) {
                continue;
            }
            const bool view = section == QLatin1String("Colors:View");
            if (key == QLatin1String("BackgroundNormal")) {
                (view ? background : selectionBackground) = value;
            } else if (key == QLatin1String("ForegroundNormal")) {
                (view ? foreground : selectionForeground) = value;
            } else if (view && key == QLatin1String("DecorationFocus")) {
                accent = value;
            }
        }
    }
    // Without kdeglobals (another desktop, or a scheme that leaves a colour
    // out) the platform theme's palette says the same. Without a GUI
    // application (afterglow mux new) there is no palette of the desktop:
    // a colour that the scheme leaves out stays unset, and the theme under
    // it keeps its own.
    const bool gui = hasGuiApplication();
    const QPalette palette = gui ? QGuiApplication::palette() : QPalette();
    auto from = [&](const std::optional<AfterglowVt::Rgb>& set, QPalette::ColorRole role) -> std::optional<AfterglowVt::Rgb> {
        if (set || !gui) return set;
        const QColor c = palette.color(QPalette::Active, role);
        return AfterglowVt::Rgb{std::uint8_t(c.red()), std::uint8_t(c.green()), std::uint8_t(c.blue())};
    };
    Theme theme;
    theme.background = from(background, QPalette::Base);
    theme.foreground = from(foreground, QPalette::Text);
    theme.selectionBackground = from(selectionBackground, QPalette::Highlight);
    theme.selectionForeground = from(selectionForeground, QPalette::HighlightedText);
    theme.cursor = from(accent, QPalette::Accent);
    theme.cursorText = theme.background;
    return theme;
}

} // namespace Afterglow::Themes
