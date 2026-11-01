// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QUrl>
#include <sys/stat.h>
namespace Afterglow {
/// The absolute path that a file URL names on this computer, or an empty string.
/// A file URL with another host names a network share, not a local file.
inline QString localFilePath(const QUrl& url)
{
    if (!url.isLocalFile()) return {};
    QUrl local = url;
    if (!local.host().isEmpty()) {
        if (local.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) != 0) return {};
        local.setHost(QString());
    }
    const QString path = local.toLocalFile();
    return path.startsWith(QLatin1Char('/')) ? path : QString();
}
/// Local files only; shell quoting works in bash, zsh and fish. Never adds Enter.
inline QString droppedFiles(const QMimeData* mime)
{
    if (!mime || !mime->hasUrls()) return {};
    const auto urls = mime->urls();
    if (urls.size() > 256) return {};
    QStringList paths;
    qsizetype size = 0;
    for (const QUrl& url : urls) {
        const QString path = localFilePath(url);
        if (path.isEmpty()) return {};
        // Controls and format characters (bidi overrides) can disguise the command line.
        for (const char32_t c : path.toUcs4())
            if (c < 32 || (c >= 127 && c < 160) || QChar::category(c) == QChar::Other_Format) return {};
        // Fish reads \' and \\ as escapes inside single quotes, so neither
        // character may appear there. Both are escaped outside the quotes,
        // which bash, zsh and fish read the same way.
        QString quoted = QStringLiteral("'");
        for (const QChar c : path) {
            if (c == QLatin1Char('\'')) quoted += QStringLiteral("'\\''");
            else if (c == QLatin1Char('\\')) quoted += QStringLiteral("'\\\\'");
            else quoted += c;
        }
        quoted += QLatin1Char('\'');
        size += quoted.toUtf8().size() + (paths.isEmpty() ? 0 : 1);
        if (size > 1024 * 1024) return {};
        paths.append(quoted);
    }
    return paths.join(QLatin1Char(' '));
}
/// Whether a relative symbolic link at `link` resolves inside `root`.
/// A ".." after a link or a missing component could leave the tree once that
/// component exists, so ".." may only leave a real directory.
inline bool linkStaysInside(const QString& link, const QByteArray& target, const QString& root)
{
    QString current = QFileInfo(link).absolutePath();
    for (const auto& part : QString::fromUtf8(target).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String(".")) continue;
        if (part == QLatin1String("..")) {
            struct stat st {};
            if (::lstat(QFile::encodeName(current).constData(), &st) || !S_ISDIR(st.st_mode)) return false;
            current = QFileInfo(current).absolutePath();
            if (current != root && !current.startsWith(root + QLatin1Char('/'))) return false;
            continue;
        }
        current += QLatin1Char('/') + part;
    }
    return current.startsWith(root + QLatin1Char('/'));
}
}
