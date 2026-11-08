// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OverlaySpec.h"
#include "ScriptNames.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Afterglow {

namespace {

constexpr std::pair<const char*, OverlaySpec::Position> Positions[] = {
    {"center", OverlaySpec::Position::Center},         {"top", OverlaySpec::Position::Top},
    {"bottom", OverlaySpec::Position::Bottom},         {"left", OverlaySpec::Position::Left},
    {"right", OverlaySpec::Position::Right},           {"top-left", OverlaySpec::Position::TopLeft},
    {"top-right", OverlaySpec::Position::TopRight},    {"bottom-left", OverlaySpec::Position::BottomLeft},
    {"bottom-right", OverlaySpec::Position::BottomRight},
};

/// Cells as an integer, or a percentage as a string: "80%". JSON integers and
/// Luau whole numbers arrive as qlonglong, other numbers as double.
std::optional<OverlaySpec::Extent> extent(const QVariant& v)
{
    if (v.typeId() == QMetaType::QString) {
        // 1 to 100, written as such: no sign, no leading zero, nothing after.
        static const QRegularExpression Percent(QStringLiteral("\\A(100|[1-9][0-9]?)%\\z"));
        const QRegularExpressionMatch match = Percent.match(v.toString());
        if (!match.hasMatch()) return std::nullopt;
        return OverlaySpec::Extent{match.captured(1).toInt(), true};
    }
    switch (v.typeId()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
    case QMetaType::Float:
        break;
    default:
        return std::nullopt;
    }
    const double cells = v.toDouble();
    if (!std::isfinite(cells) || cells != std::floor(cells) || cells < 1 || cells > OverlaySpec::MaxCells) return std::nullopt;
    return OverlaySpec::Extent{int(cells), false};
}

} // namespace

OverlaySpec::Placement OverlaySpec::place(int cols, int rows) const
{
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    auto along = [](const std::optional<Extent>& size, int whole) {
        if (!size) return whole;
        return size->percent ? int(qint64(whole) * size->value / 100) : size->value;
    };
    // The program's least grid, and the border's cell on each side, when
    // the host has room for both.
    const bool border = framed() && cols >= 4 && rows >= 3;
    const int edge = border ? 1 : 0;
    const int w = std::clamp(along(width, cols), std::min(cols, 2 + 2 * edge), cols);
    const int h = std::clamp(along(height, rows), 1 + 2 * edge, rows);
    int x = (cols - w) / 2;
    int y = (rows - h) / 2;
    switch (position) {
    case Position::Center: break;
    case Position::Top: y = 0; break;
    case Position::Bottom: y = rows - h; break;
    case Position::Left: x = 0; break;
    case Position::Right: x = cols - w; break;
    case Position::TopLeft: x = 0, y = 0; break;
    case Position::TopRight: x = cols - w, y = 0; break;
    case Position::BottomLeft: x = 0, y = rows - h; break;
    case Position::BottomRight: x = cols - w, y = rows - h; break;
    }
    return {QRect(x + edge, y + edge, w - 2 * edge, h - 2 * edge), border};
}

QString EnvironmentOverrides::read(const QVariant& value, QVariantMap& env)
{
    env.clear();
    if (!value.isValid() || value.isNull()) return {};
    if (value.typeId() == QMetaType::QVariantList && value.toList().isEmpty()) return {};
    if (value.typeId() != QMetaType::QVariantMap) return QStringLiteral("env must be an object");
    const QVariantMap variables = value.toMap();
    if (variables.size() > MaxVariables)
        return QStringLiteral("env sets at most %1 variables").arg(MaxVariables);
    qsizetype bytes = 0;
    for (auto it = variables.cbegin(); it != variables.cend(); ++it) {
        const QVariant& v = it.value();
        bytes += it.key().toUtf8().size() + v.toString().toUtf8().size();
        if (it.key().isEmpty() || it.key().contains(QLatin1Char('=')) || it.key().contains(QChar::Null)
            || !((v.typeId() == QMetaType::QString && !v.toString().contains(QChar::Null))
                 || (v.typeId() == QMetaType::Bool && !v.toBool())))
            return QStringLiteral("env needs valid variable names and string values or false to remove");
    }
    if (bytes > MaxBytes) return QStringLiteral("env is over %1 KiB of names and values").arg(MaxBytes >> 10);
    env = variables;
    return {};
}

std::optional<OverlaySpec> OverlaySpec::read(const QStringList& argv, const QVariantMap& options, QString* error)
{
    auto refuse = [error](const QString& message) -> std::optional<OverlaySpec> {
        if (error) *error = message;
        return std::nullopt;
    };
    auto option = [&options](const char* key) { return options.value(QLatin1String(key)); };
    auto given = [](const QVariant& v) { return v.isValid() && !v.isNull(); };
    OverlaySpec spec;
    if (argv.isEmpty() || argv.size() > MaxArguments)
        return refuse(QStringLiteral("An overlay needs an argv list of 1 to %1 strings").arg(MaxArguments));
    for (const QString& arg : argv) {
        if (arg.size() > MaxArgument || arg.contains(QChar::Null))
            return refuse(QStringLiteral("A command argument is over %1 characters or has a NUL character").arg(MaxArgument));
    }
    if (argv.first().isEmpty()) return refuse(QStringLiteral("An overlay needs a program to run"));
    spec.argv = argv;
    for (auto it = options.cbegin(); it != options.cend(); ++it) {
        static const QStringList Known = {QStringLiteral("stdin"),  QStringLiteral("capture"), QStringLiteral("cwd"),
                                          QStringLiteral("env"),    QStringLiteral("title"),   QStringLiteral("width"),
                                          QStringLiteral("height"), QStringLiteral("position"), QStringLiteral("border")};
        if (!Known.contains(it.key())) return refuse(QStringLiteral("Unknown overlay option %1").arg(it.key()));
    }
    for (const char* key : {"stdin", "capture", "cwd", "title"}) {
        const QVariant v = option(key);
        if (given(v) && v.typeId() != QMetaType::QString)
            return refuse(QStringLiteral("The overlay option %1 must be a string").arg(QLatin1String(key)));
    }
    if (const QVariant input = option("stdin"); given(input)) {
        QByteArray bytes = input.toString().toUtf8();
        if (bytes.size() > MaxInput) return refuse(QStringLiteral("The overlay's stdin is over %1 MiB").arg(MaxInput >> 20));
        spec.input = std::move(bytes);
    }
    // Left out, the program's stdout; an empty string is no choice.
    if (const QVariant capture = option("capture"); !given(capture) || capture.toString() == QLatin1String("stdout"))
        spec.capture = Capture::Stdout;
    else if (capture.toString() == QLatin1String("file")) spec.capture = Capture::File;
    else if (capture.toString() == QLatin1String("none")) spec.capture = Capture::None;
    else return refuse(QStringLiteral("capture must be stdout, file or none"));
    // A relative path has no directory to start from: the window's own
    // directory means nothing to the script or the client.
    spec.cwd = option("cwd").toString();
    if (spec.cwd.contains(QChar::Null)) return refuse(QStringLiteral("cwd must be a string without NUL"));
    // A leading slash, not QDir::isAbsolutePath: that takes ":/x", a Qt
    // resource, for absolute, and no program can start there.
    if (!spec.cwd.isEmpty() && !spec.cwd.startsWith(QLatin1Char('/')) && spec.cwd != QLatin1String("~")
        && !spec.cwd.startsWith(QLatin1String("~/")))
        return refuse(QStringLiteral("cwd must be an absolute path, ~ or a path that starts with ~/"));
    spec.title = option("title").toString();
    if (spec.title.size() > MaxTitle) return refuse(QStringLiteral("The overlay title is over %1 characters").arg(MaxTitle));
    if (std::any_of(spec.title.begin(), spec.title.end(), unprintable))
        return refuse(QStringLiteral("The overlay title has a control character, a line break or a bidirectional control"));
    if (const QString problem = EnvironmentOverrides::read(option("env"), spec.env); !problem.isEmpty()) return refuse(problem);
    // Unset and null both leave an option at its default.
    for (const char* key : {"width", "height"}) {
        const QVariant value = option(key);
        if (!given(value)) continue;
        const auto size = extent(value);
        if (!size)
            return refuse(QStringLiteral("%1 must be a number of cells from 1 to %2, or a percentage from \"1%\" to \"100%\"")
                              .arg(QLatin1String(key))
                              .arg(MaxCells));
        (qstrcmp(key, "width") == 0 ? spec.width : spec.height) = size;
    }
    if (const QVariant value = option("position"); given(value)) {
        const QString name = value.typeId() == QMetaType::QString ? value.toString() : QString();
        const auto known = std::find_if(std::begin(Positions), std::end(Positions),
                                        [&name](const auto& p) { return name == QLatin1String(p.first); });
        if (known == std::end(Positions)) {
            QStringList names;
            for (const auto& [each, position] : Positions) names.append(QLatin1String(each));
            const QString last = names.takeLast();
            return refuse(QStringLiteral("position must be %1 or %2").arg(names.join(QStringLiteral(", ")), last));
        }
        spec.position = known->second;
    }
    if (const QVariant value = option("border"); given(value)) {
        if (value.typeId() != QMetaType::Bool) return refuse(QStringLiteral("border must be true or false"));
        spec.border = value.toBool();
    }
    return spec;
}

} // namespace Afterglow
