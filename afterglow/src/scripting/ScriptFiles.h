// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Afterglow {
/// Script, package, module, action and pulse names: letters, digits, _ and -.
inline bool validScriptId(const QString& id)
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    return re.match(id).hasMatch();
}

// Read the descriptor that was validated, with a byte bound even if the file
// grows. O_NONBLOCK prevents a replaced path from hanging the GUI on a FIFO.
inline QString readScriptFile(const QString& path, qint64 limit, QByteArray& bytes,
                              const QString& root = {}, bool noFollow = false)
{
    const int fd = ::open(QFile::encodeName(path).constData(),
        O_RDONLY | O_CLOEXEC | O_NONBLOCK | (noFollow ? O_NOFOLLOW : 0));
    if (fd < 0) return QStringLiteral("Cannot open file: %1").arg(path);
    struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
    struct stat info {};
    if (::fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > limit)
        return QStringLiteral("File must be regular and within its byte limit: %1").arg(path);
    if (!root.isEmpty()) {
        const auto opened = QFileInfo(QStringLiteral("/proc/self/fd/%1").arg(fd)).canonicalFilePath();
        if (!opened.startsWith(root + QLatin1Char('/')))
            return QStringLiteral("Package file escapes its root: %1").arg(path);
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) return file.errorString();
    auto contents = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError) return file.errorString();
    if (contents.size() > limit) return QStringLiteral("File exceeds its byte limit: %1").arg(path);
    bytes = std::move(contents);
    return {};
}
}
