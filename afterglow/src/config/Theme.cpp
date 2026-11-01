// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Theme.h"

#include "app/BuildTree.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

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
    const QColor c = QColor::fromString(value);
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

} // namespace

bool isColorName(const QString& name)
{
    static const QRegularExpression EightDigits(QStringLiteral("^#[0-9a-fA-F]{8}$"));
    return QColor::isValidColorName(name) && !EightDigits.match(name).hasMatch();
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

} // namespace

QStringList searchDirs()
{
    QStringList dirs{QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                     + QStringLiteral("/afterglow/themes")};
    const QString bundled = bundledDirectory();
    // A build-tree binary puts the checkout above the data directories, so
    // that an installed copy does not shadow the themes that it is testing.
    const bool checkout = runningFromBuildTree();
    if (checkout && !bundled.isEmpty()) {
        dirs << bundled;
    }
    dirs << QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("afterglow/themes"),
                                      QStandardPaths::LocateDirectory);
    if (!checkout && !bundled.isEmpty()) {
        dirs << bundled;
    }
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
        for (const QString& file : {name + QStringLiteral(".json"), name + QStringLiteral(".conf"), name}) {
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
    } else {
        // Ghostty writes "key = value"; kitty "key value".
        static const QRegularExpression Ghostty(QStringLiteral("^([\\w-]+)\\s*=\\s*(.*)$"));
        static const QRegularExpression Kitty(QStringLiteral("^([\\w-]+)\\s+(\\S+)"));
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

} // namespace Afterglow::Themes
