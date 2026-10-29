// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Theme.h"

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
    const QColor c = QColor::fromString(value);
    if (!c.isValid()) {
        return std::nullopt;
    }
    return AfterglowVt::Rgb{std::uint8_t(c.red()), std::uint8_t(c.green()), std::uint8_t(c.blue())};
}

/// Sets the colour @p key names (in any of the three spellings) to @p value.
/// Returns false for keys that are not colours afterglow uses.
bool assign(Theme& theme, QString key, const QString& value)
{
    key = key.toLower().replace(QLatin1Char('-'), QLatin1Char('_'));
    if (key == QLatin1String("palette")) {
        // Ghostty: "palette = 3=#c5a332".
        const qsizetype eq = value.indexOf(QLatin1Char('='));
        bool ok = false;
        const int index = value.left(eq).trimmed().toInt(&ok);
        if (eq < 0 || !ok || index < 0 || index >= 16) {
            return false;
        }
        theme.palette[std::size_t(index)] = color(value.mid(eq + 1));
        return theme.palette[std::size_t(index)].has_value();
    }
    if (key.startsWith(QLatin1String("color"))) {
        // kitty: "color3 #c5a332".
        bool ok = false;
        const int index = key.mid(5).toInt(&ok);
        if (!ok || index < 0 || index >= 16) {
            return false;
        }
        theme.palette[std::size_t(index)] = color(value);
        return theme.palette[std::size_t(index)].has_value();
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
    } else {
        return false;
    }
    *target = color(value);
    return target->has_value();
}

} // namespace

QString pick(const QString& spec, bool dark)
{
    if (!spec.contains(QLatin1Char(':')) || spec.contains(QLatin1Char('/'))) {
        return spec.trimmed();
    }
    QString light;
    QString darkName;
    for (const QString& part : spec.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const qsizetype colon = part.indexOf(QLatin1Char(':'));
        const QString which = part.left(colon).trimmed().toLower();
        const QString name = part.mid(colon + 1).trimmed();
        if (which == QLatin1String("light")) {
            light = name;
        } else if (which == QLatin1String("dark")) {
            darkName = name;
        }
    }
    if (dark) {
        return darkName.isEmpty() ? light : darkName;
    }
    return light.isEmpty() ? darkName : light;
}

QStringList searchDirs()
{
    QStringList dirs{QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                     + QStringLiteral("/afterglow/themes")};
    dirs << QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("afterglow/themes"),
                                      QStandardPaths::LocateDirectory);
#ifdef AFTERGLOW_SOURCE_THEME_DIR
    dirs << QStringLiteral(AFTERGLOW_SOURCE_THEME_DIR);
#endif
    dirs.removeDuplicates();
    return dirs;
}

QString locate(const QString& name, const QStringList& dirs)
{
    if (name.isEmpty()) {
        return {};
    }
    if (name.contains(QLatin1Char('/'))) {
        QString path = name;
        if (path.startsWith(QLatin1String("~/"))) {
            path = QDir::homePath() + path.mid(1);
        }
        return QFileInfo(path).isFile() ? path : QString();
    }
    for (const QString& dir : dirs) {
        for (const QString& file : {name + QStringLiteral(".json"), name + QStringLiteral(".conf"), name}) {
            const QString path = dir + QLatin1Char('/') + file;
            if (QFileInfo(path).isFile()) {
                return path;
            }
        }
    }
    return {};
}

std::optional<Theme> parse(const QByteArray& data, QString* error)
{
    Theme theme;
    int found = 0;
    if (data.trimmed().startsWith('{')) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (err.error != QJsonParseError::NoError) {
            if (error) {
                *error = err.errorString();
            }
            return std::nullopt;
        }
        const QJsonObject o = doc.object();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.key() == QLatin1String("palette")) {
                const QJsonArray palette = it.value().toArray();
                for (int i = 0; i < 16 && i < palette.size(); ++i) {
                    theme.palette[std::size_t(i)] = color(palette[i].toString());
                    found += theme.palette[std::size_t(i)].has_value();
                }
            } else {
                found += assign(theme, it.key(), it.value().toString());
            }
        }
    } else {
        // Ghostty writes "key = value"; kitty "key value".
        static const QRegularExpression Ghostty(QStringLiteral("^([\\w-]+)\\s*=\\s*(.*)$"));
        static const QRegularExpression Kitty(QStringLiteral("^([\\w-]+)\\s+(\\S+)"));
        for (const QByteArray& raw : data.split('\n')) {
            const QString line = QString::fromUtf8(raw).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
                continue;
            }
            QRegularExpressionMatch m = Ghostty.match(line);
            if (!m.hasMatch()) {
                m = Kitty.match(line);
            }
            if (m.hasMatch()) {
                found += assign(theme, m.captured(1), m.captured(2).trimmed());
            }
        }
    }
    if (found == 0) {
        if (error) {
            *error = QStringLiteral("no colours found");
        }
        return std::nullopt;
    }
    return theme;
}

} // namespace Afterglow::Themes
