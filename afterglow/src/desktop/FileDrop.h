// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QMimeData>
#include <QUrl>
namespace Afterglow {
/// Local files only; shell quoting works in bash, zsh and fish. Never adds Enter.
inline QString droppedFiles(const QMimeData* mime)
{
    if (!mime || !mime->hasUrls()) return {};
    const auto urls = mime->urls();
    if (urls.size() > 256) return {};
    QStringList paths;
    qsizetype size = 0;
    for (const QUrl& url : urls) {
        if (!url.isLocalFile()) return {};
        QString path = url.toLocalFile();
        if (!path.startsWith(QLatin1Char('/'))) return {};
        for (const QChar c : path) if (c.unicode() < 32 || c.unicode() == 127) return {};
        // Both POSIX shells and fish understand adjacent quoted/unquoted fragments.
        path.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        path = QLatin1Char('\'') + path + QLatin1Char('\'');
        size += path.size();
        if (size > 1024 * 1024) return {};
        paths.append(path);
    }
    return paths.join(QLatin1Char(' '));
}
}
