// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OverlaySpec.h"

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

/// Cells as an integer, or a percentage as a string: "80%". JSON and Luau
/// give numbers as doubles, JSON integers also as qlonglong.
std::optional<OverlaySpec::Extent> extent(const QVariant& v)
{
    if (v.typeId() == QMetaType::QString) {
        static const QRegularExpression Percent(QStringLiteral("^([0-9]{1,3})%$"));
        const QRegularExpressionMatch match = Percent.match(v.toString());
        const int percent = match.hasMatch() ? match.captured(1).toInt() : 0;
        if (percent < 1 || percent > 100) return std::nullopt;
        return OverlaySpec::Extent{percent, true};
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

std::optional<OverlaySpec> OverlaySpec::read(const QStringList& argv, const QVariantMap& options, QString* error)
{
    auto refuse = [error](const QString& message) -> std::optional<OverlaySpec> {
        if (error) *error = message;
        return std::nullopt;
    };
    auto text = [&options](const char* key) { return options.value(QLatin1String(key)); };
    auto isText = [](const QVariant& v) { return !v.isValid() || v.isNull() || v.typeId() == QMetaType::QString; };
    OverlaySpec spec;
    if (argv.isEmpty() || argv.size() > MaxArguments)
        return refuse(QStringLiteral("An overlay needs an argv list of 1 to %1 strings").arg(MaxArguments));
    for (const QString& arg : argv) {
        if (arg.size() > MaxArgument || arg.contains(QChar::Null)) return refuse(QStringLiteral("Invalid command argument"));
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
        if (!isText(text(key))) return refuse(QStringLiteral("The overlay option %1 must be a string").arg(QLatin1String(key)));
    }
    if (const QVariant input = text("stdin"); input.isValid() && !input.isNull()) {
        QByteArray bytes = input.toString().toUtf8();
        if (bytes.size() > MaxInput) return refuse(QStringLiteral("The overlay's stdin is over %1 MiB").arg(MaxInput >> 20));
        spec.input = std::move(bytes);
    }
    const QString capture = text("capture").toString();
    if (capture.isEmpty() || capture == QLatin1String("stdout")) spec.capture = Capture::Stdout;
    else if (capture == QLatin1String("file")) spec.capture = Capture::File;
    else if (capture == QLatin1String("none")) spec.capture = Capture::None;
    else return refuse(QStringLiteral("capture must be stdout, file or none"));
    spec.cwd = text("cwd").toString();
    if (spec.cwd.contains(QChar::Null)) return refuse(QStringLiteral("cwd must be a string without NUL"));
    spec.title = text("title").toString();
    if (spec.title.size() > MaxTitle) return refuse(QStringLiteral("The overlay title is over %1 characters").arg(MaxTitle));
    const QVariant env = text("env");
    if (env.isValid() && !env.isNull() && env.typeId() != QMetaType::QVariantMap
        && !(env.typeId() == QMetaType::QVariantList && env.toList().isEmpty()))
        return refuse(QStringLiteral("env must be an object"));
    spec.env = env.toMap();
    qsizetype bytes = 0;
    for (auto it = spec.env.cbegin(); it != spec.env.cend(); ++it) {
        const QVariant& v = it.value();
        bytes += it.key().toUtf8().size() + v.toString().toUtf8().size();
        if (it.key().isEmpty() || it.key().contains(QLatin1Char('=')) || it.key().contains(QChar::Null)
            || !((v.typeId() == QMetaType::QString && !v.toString().contains(QChar::Null))
                 || (v.typeId() == QMetaType::Bool && !v.toBool())))
            return refuse(QStringLiteral("env needs valid variable names and string values or false to remove"));
    }
    if (spec.env.size() > 256 || bytes > 65536) return refuse(QStringLiteral("Environment overrides exceed their limit"));
    // Unset and null both leave an option at its default.
    auto given = [&options](const char* key) {
        const QVariant v = options.value(QLatin1String(key));
        return v.isValid() && !v.isNull() ? std::optional<QVariant>(v) : std::nullopt;
    };
    for (const char* key : {"width", "height"}) {
        const auto value = given(key);
        if (!value) continue;
        const auto size = extent(*value);
        if (!size)
            return refuse(QStringLiteral("%1 must be a number of cells from 1 to %2, or a percentage from \"1%\" to \"100%\"")
                              .arg(QLatin1String(key))
                              .arg(MaxCells));
        (qstrcmp(key, "width") == 0 ? spec.width : spec.height) = size;
    }
    if (const auto value = given("position")) {
        const QString name = value->typeId() == QMetaType::QString ? value->toString() : QString();
        const auto known = std::find_if(std::begin(Positions), std::end(Positions),
                                        [&name](const auto& p) { return name == QLatin1String(p.first); });
        if (known == std::end(Positions))
            return refuse(QStringLiteral("position must be center, top, bottom, left, right, top-left, top-right, bottom-left or "
                                         "bottom-right"));
        spec.position = known->second;
    }
    if (const auto value = given("border")) {
        if (value->typeId() != QMetaType::Bool) return refuse(QStringLiteral("border must be true or false"));
        spec.border = value->toBool();
    }
    return spec;
}

} // namespace Afterglow
