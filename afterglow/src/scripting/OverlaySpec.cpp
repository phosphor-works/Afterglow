// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OverlaySpec.h"

#include <QDir>

namespace Afterglow {

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
    if (!spec.cwd.isEmpty() && !QDir::isAbsolutePath(spec.cwd) && spec.cwd != QLatin1String("~")
        && !spec.cwd.startsWith(QLatin1String("~/")))
        return refuse(QStringLiteral("cwd must be an absolute path, ~ or a path that starts with ~/"));
    spec.title = option("title").toString();
    if (spec.title.size() > MaxTitle) return refuse(QStringLiteral("The overlay title is over %1 characters").arg(MaxTitle));
    if (const QString problem = EnvironmentOverrides::read(option("env"), spec.env); !problem.isEmpty()) return refuse(problem);
    return spec;
}

} // namespace Afterglow
