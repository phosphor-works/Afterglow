// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "desktop/UploadArchive.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowPty/Pty.h>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Afterglow;

namespace {

void write(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly)) << path.toStdString();
    file.write(data);
}

/// The names in @p archive, as GNU tar lists them, and its files unpacked in @p into.
QStringList unpack(const std::string& archive, const QString& into)
{
    QDir().mkpath(into);
    QProcess tar;
    tar.start(QStringLiteral("tar"), {QStringLiteral("-xzvf"), QStringLiteral("-"), QStringLiteral("-C"), into});
    tar.write(archive.data(), qint64(archive.size()));
    tar.closeWriteChannel();
    if (!tar.waitForFinished(30000) || tar.exitCode() != 0) {
        ADD_FAILURE() << "tar failed: " << tar.readAllStandardError().toStdString();
        return {};
    }
    QStringList names = QString::fromUtf8(tar.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    names.sort();
    return names;
}

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

} // namespace

// As iTerm2 makes it: each path below the deepest directory that holds them
// all, written as ./path; a directory chosen alone below its parent. GNU tar
// reads it back whole: long names (pax), links, binary data and modes.
TEST(UploadArchive, TarReadsWhatItulGets)
{
    if (QStandardPaths::findExecutable(QStringLiteral("tar")).isEmpty()) GTEST_SKIP() << "no tar";
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString root = temp.filePath(QStringLiteral("project"));
    QByteArray binary;
    for (int i = 0; i < 70000; ++i) binary.append(char(i * 7 % 256));
    write(root + QStringLiteral("/a.txt"), "hello\n");
    write(root + QStringLiteral("/sub/b.bin"), binary);
    const QString longName = QString(150, QLatin1Char('n')) + QStringLiteral(".txt");
    write(root + QStringLiteral("/sub/") + longName, "long\n");
    ASSERT_TRUE(QFile::link(QStringLiteral("a.txt"), root + QStringLiteral("/link")));
    QFile::setPermissions(root + QStringLiteral("/a.txt"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    QString error;
    auto archive = UploadArchive::build({root}, 16 << 20, error);
    ASSERT_TRUE(archive) << error.toStdString();
    const QString out = temp.filePath(QStringLiteral("out"));
    const QStringList names = unpack(*archive, out);
    EXPECT_EQ(names, (QStringList{QStringLiteral("./project/"), QStringLiteral("./project/a.txt"), QStringLiteral("./project/link"),
                                  QStringLiteral("./project/sub/"), QStringLiteral("./project/sub/b.bin"),
                                  QStringLiteral("./project/sub/") + longName}))
        << names.join(QLatin1Char(' ')).toStdString();
    EXPECT_EQ(read(out + QStringLiteral("/project/a.txt")), QByteArray("hello\n"));
    EXPECT_TRUE(read(out + QStringLiteral("/project/sub/b.bin")) == binary);
    EXPECT_EQ(read(out + QStringLiteral("/project/sub/") + longName), QByteArray("long\n"));
    EXPECT_EQ(QFile::symLinkTarget(out + QStringLiteral("/project/link")), out + QStringLiteral("/project/a.txt"));
    EXPECT_TRUE(QFile::permissions(out + QStringLiteral("/project/a.txt")) & QFile::ExeOwner);

    // Files of two directories: below the directory that holds both.
    archive = UploadArchive::build({root + QStringLiteral("/a.txt"), root + QStringLiteral("/sub/b.bin")}, 16 << 20, error);
    ASSERT_TRUE(archive) << error.toStdString();
    EXPECT_EQ(unpack(*archive, temp.filePath(QStringLiteral("two"))), (QStringList{QStringLiteral("./a.txt"), QStringLiteral("./sub/b.bin")}));
}

TEST(UploadArchive, RefusesWhatItCannotSendWhole)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    // Bytes that gzip cannot shrink (xorshift32).
    QByteArray noise;
    std::uint32_t x = 2463534242u;
    for (int i = 0; i < 300000; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        noise.append(char(x >> 24));
    }
    write(temp.filePath(QStringLiteral("noise.bin")), noise);
    QString error;
    EXPECT_FALSE(UploadArchive::build({temp.filePath(QStringLiteral("noise.bin"))}, 100000, error));
    EXPECT_EQ(error, QStringLiteral("the files are larger than Terminal.uploadLimit lets through: send them with aglow transfer"));
    EXPECT_FALSE(UploadArchive::build({temp.filePath(QStringLiteral("gone"))}, 1 << 20, error));
    EXPECT_EQ(error, temp.filePath(QStringLiteral("gone")) + QStringLiteral(" does not exist"));
    EXPECT_FALSE(UploadArchive::build({}, 1 << 20, error));
}

// iTerm2's answer: ok, base64 lines of 76 characters joined by CR, and an
// empty line. The largest answer fits the program's input in one write.
TEST(UploadArchive, TheAnswerIsWhatItulReads)
{
    std::string archive;
    for (int i = 0; i < 1000; ++i) archive += char(i % 251);
    const std::string answer = UploadArchive::answer(archive);
    ASSERT_TRUE(answer.starts_with("ok\n"));
    ASSERT_TRUE(answer.ends_with("\n\n"));
    const std::string body = answer.substr(3, answer.size() - 5);
    std::string joined;
    std::size_t at = 0;
    while (at <= body.size()) {
        const std::size_t end = std::min(body.find('\r', at), body.size());
        EXPECT_LE(end - at, 76u);
        if (end < body.size()) {
            EXPECT_EQ(end - at, 76u);
        }
        joined += body.substr(at, end - at);
        at = end + 1;
    }
    std::string decoded;
    ASSERT_TRUE(AfterglowProto::base64DecodeAppend(decoded, joined, AfterglowProto::Base64Check::Strict));
    EXPECT_EQ(decoded, archive);
    EXPECT_EQ(UploadArchive::answerSize(archive.size()), answer.size());
    EXPECT_EQ(UploadArchive::answerSize(0), UploadArchive::answer({}).size());
    EXPECT_LE(UploadArchive::answerSize(std::size_t(23) << 20), AfterglowPty::Pty::MaxPendingInput);
}
