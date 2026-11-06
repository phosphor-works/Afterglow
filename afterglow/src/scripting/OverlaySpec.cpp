// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OverlaySpec.h"

namespace Afterglow {

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
        static const QStringList Known = {QStringLiteral("stdin"), QStringLiteral("capture"), QStringLiteral("cwd"),
                                          QStringLiteral("env"), QStringLiteral("title")};
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
    return spec;
}

} // namespace Afterglow
