// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Afterglow {
/// Module, asset, pulse, shader pack and dock names: 1 to 64 letters,
/// digits, _ and -.
inline bool validName(const QString& name)
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    return re.match(name).hasMatch();
}

/// Script and package ids, dependency names (the ids of packages) and
/// command names: names that do not start with -, which `afterglow run`
/// would read as an option.
inline bool validScriptId(const QString& id)
{
    return validName(id) && !id.startsWith(QLatin1Char('-'));
}
/// The rule of validScriptId, for error messages.
inline constexpr QLatin1StringView ScriptIdRule{"1 to 64 letters, digits, _ and -, and must not start with -"};

/// The longest prefix of @p bytes that has at most @p limit bytes and ends on
/// a whole UTF-8 character. A character that ends exactly at the limit stays.
inline QByteArray utf8Prefix(QByteArray bytes, qsizetype limit)
{
    if (limit < 0) limit = 0;
    if (bytes.size() <= limit) return bytes;
    bytes.truncate(limit);
    // The lead byte of the last sequence, at most three continuation bytes back.
    qsizetype lead = bytes.size();
    while (lead > 0 && bytes.size() - lead < 3 && (static_cast<unsigned char>(bytes[lead - 1]) & 0xC0) == 0x80) --lead;
    if (lead == 0) return bytes; // continuation bytes only: not text to keep whole
    const auto b = static_cast<unsigned char>(bytes[lead - 1]);
    const qsizetype length = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
    // The sequence that the cut runs through goes whole.
    if (lead - 1 + length > bytes.size()) bytes.truncate(lead - 1);
    return bytes;
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
