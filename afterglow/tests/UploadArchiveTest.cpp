// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/Settings.h"
#include "desktop/UploadArchive.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowPty/Pty.h>

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <sys/time.h>

using namespace Afterglow;

namespace {

void write(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly)) << path.toStdString();
    file.write(data);
}

bool haveTar()
{
    // GNU tar runs gzip for -z.
    return !QStandardPaths::findExecutable(QStringLiteral("tar")).isEmpty()
        && !QStandardPaths::findExecutable(QStringLiteral("gzip")).isEmpty();
}

/// The output of tar with @p args, given @p archive as its input.
QString runTar(const std::string& archive, const QStringList& args)
{
    QProcess tar;
    tar.start(QStringLiteral("tar"), args);
    tar.write(archive.data(), qint64(archive.size()));
    tar.closeWriteChannel();
    if (!tar.waitForFinished(30000) || tar.exitCode() != 0) {
        ADD_FAILURE() << "tar failed: " << tar.readAllStandardError().toStdString();
        return {};
    }
    return QString::fromUtf8(tar.readAllStandardOutput());
}

/// What tar unpacks from @p archive in @p into: each path, with '/' after a
/// directory, sorted. Read from the files, not from tar's own listing.
QStringList unpack(const std::string& archive, const QString& into)
{
    QDir().mkpath(into);
    runTar(archive, {QStringLiteral("-xzf"), QStringLiteral("-"), QStringLiteral("-C"), into});
    QStringList names;
    QDirIterator it(into, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo info = it.nextFileInfo();
        const QString name = QDir(into).relativeFilePath(info.filePath());
        names.append(info.isDir() && !info.isSymLink() ? name + QLatin1Char('/') : name);
    }
    names.sort();
    return names;
}

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

/// Bytes that gzip cannot shrink (xorshift32).
QByteArray noise(int size)
{
    QByteArray bytes;
    std::uint32_t x = 2463534242u;
    for (int i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        bytes.append(char(x >> 24));
    }
    return bytes;
}

} // namespace

// As iTerm2 makes it: each path below the deepest directory that holds them
// all, written as ./path; a directory chosen alone below its parent. tar
// reads it back whole: long names and link targets (pax), links, binary data
// and modes. A pipe in the directory is left out.
TEST(UploadArchive, TarReadsWhatItulGets)
{
    if (!haveTar()) GTEST_SKIP() << "no tar or gzip";
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
    ASSERT_TRUE(QFile::link(QStringLiteral("sub/") + longName, root + QStringLiteral("/longlink")));
    ASSERT_TRUE(QFile::link(QStringLiteral("sub"), root + QStringLiteral("/dirlink")));
    ASSERT_EQ(::mkfifo(QFile::encodeName(root + QStringLiteral("/fifo")).constData(), 0600), 0);
    ASSERT_TRUE(QFile::setPermissions(root + QStringLiteral("/a.txt"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    QString error;
    auto archive = UploadArchive::build({root}, 16 << 20, error);
    ASSERT_TRUE(archive) << error.toStdString();
    const QString out = temp.filePath(QStringLiteral("out"));
    const QStringList names = unpack(*archive, out);
    EXPECT_EQ(names, (QStringList{QStringLiteral("project/"), QStringLiteral("project/a.txt"), QStringLiteral("project/dirlink"),
                                  QStringLiteral("project/link"), QStringLiteral("project/longlink"), QStringLiteral("project/sub/"),
                                  QStringLiteral("project/sub/b.bin"), QStringLiteral("project/sub/") + longName}))
        << names.join(QLatin1Char(' ')).toStdString();
    EXPECT_EQ(read(out + QStringLiteral("/project/a.txt")), QByteArray("hello\n"));
    EXPECT_TRUE(read(out + QStringLiteral("/project/sub/b.bin")) == binary);
    EXPECT_EQ(read(out + QStringLiteral("/project/sub/") + longName), QByteArray("long\n"));
    EXPECT_EQ(QFile::symLinkTarget(out + QStringLiteral("/project/link")), out + QStringLiteral("/project/a.txt"));
    EXPECT_EQ(QFile::symLinkTarget(out + QStringLiteral("/project/longlink")), out + QStringLiteral("/project/sub/") + longName);
    EXPECT_EQ(QFile::symLinkTarget(out + QStringLiteral("/project/dirlink")), out + QStringLiteral("/project/sub"));
    EXPECT_TRUE(QFile::permissions(out + QStringLiteral("/project/a.txt")) & QFile::ExeOwner);

    // "project/" and a file in it are "project" alone, never "./.".
    archive = UploadArchive::build({root + QStringLiteral("/"), root + QStringLiteral("/a.txt")}, 16 << 20, error);
    ASSERT_TRUE(archive) << error.toStdString();
    EXPECT_EQ(unpack(*archive, temp.filePath(QStringLiteral("slash"))), names);

    // Files of two directories: below the directory that holds both.
    archive = UploadArchive::build({root + QStringLiteral("/a.txt"), root + QStringLiteral("/sub/b.bin")}, 16 << 20, error);
    ASSERT_TRUE(archive) << error.toStdString();
    EXPECT_EQ(unpack(*archive, temp.filePath(QStringLiteral("two"))), (QStringList{QStringLiteral("a.txt"), QStringLiteral("sub/"),
                                                                                    QStringLiteral("sub/b.bin")}));

    // A file in a chosen directory, the directory, then a file elsewhere:
    // the last one stays.
    write(temp.filePath(QStringLiteral("other/c.txt")), "c\n");
    archive = UploadArchive::build({root + QStringLiteral("/a.txt"), root, temp.filePath(QStringLiteral("other/c.txt"))}, 16 << 20, error);
    ASSERT_TRUE(archive) << error.toStdString();
    const QStringList both = unpack(*archive, temp.filePath(QStringLiteral("both")));
    EXPECT_TRUE(both.contains(QStringLiteral("other/c.txt"))) << both.join(QLatin1Char(' ')).toStdString();
    EXPECT_TRUE(both.contains(QStringLiteral("project/sub/b.bin"))) << both.join(QLatin1Char(' ')).toStdString();
    EXPECT_FALSE(both.contains(QStringLiteral("a.txt"))) << both.join(QLatin1Char(' ')).toStdString();
}

// A chosen link is what it points at, under the link's name: a link alone
// would point at nothing where the program unpacks it.
TEST(UploadArchive, FollowsAChosenLink)
{
    if (!haveTar()) GTEST_SKIP() << "no tar or gzip";
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    write(temp.filePath(QStringLiteral("real/dir/f.txt")), "in dir\n");
    write(temp.filePath(QStringLiteral("real/file.txt")), "file\n");
    QDir().mkpath(temp.filePath(QStringLiteral("chosen")));
    ASSERT_TRUE(QFile::link(temp.filePath(QStringLiteral("real/dir")), temp.filePath(QStringLiteral("chosen/dirlink"))));
    ASSERT_TRUE(QFile::link(temp.filePath(QStringLiteral("real/file.txt")), temp.filePath(QStringLiteral("chosen/filelink"))));

    QString error;
    auto archive = UploadArchive::build({temp.filePath(QStringLiteral("chosen/dirlink"))}, 1 << 20, error);
    ASSERT_TRUE(archive) << error.toStdString();
    const QString out = temp.filePath(QStringLiteral("out"));
    EXPECT_EQ(unpack(*archive, out), (QStringList{QStringLiteral("dirlink/"), QStringLiteral("dirlink/f.txt")}));
    EXPECT_EQ(read(out + QStringLiteral("/dirlink/f.txt")), QByteArray("in dir\n"));

    archive = UploadArchive::build({temp.filePath(QStringLiteral("chosen/filelink"))}, 1 << 20, error);
    ASSERT_TRUE(archive) << error.toStdString();
    const QString out2 = temp.filePath(QStringLiteral("out2"));
    EXPECT_EQ(unpack(*archive, out2), QStringList{QStringLiteral("filelink")});
    EXPECT_FALSE(QFileInfo(out2 + QStringLiteral("/filelink")).isSymLink());
    EXPECT_EQ(read(out2 + QStringLiteral("/filelink")), QByteArray("file\n"));
}

// Times before 1970 and past the eleven octal digits of ustar go in a pax
// header. The setuid and setgid bits stay out: tar run as root restores them.
TEST(UploadArchive, KeepsTimesAndDropsSetuid)
{
    if (!haveTar()) GTEST_SKIP() << "no tar or gzip";
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString root = temp.filePath(QStringLiteral("t"));
    write(root + QStringLiteral("/old"), "old\n");
    write(root + QStringLiteral("/future"), "future\n");
    write(root + QStringLiteral("/setuid"), "#!/bin/sh\n");
    const auto setTime = [](const QString& path, std::int64_t seconds) {
        const timeval times[2] = {{time_t(seconds), 0}, {time_t(seconds), 0}};
        return ::utimes(QFile::encodeName(path).constData(), times) == 0;
    };
    const std::int64_t future = (std::int64_t(1) << 33) + 1000; // just past 8^11 seconds, in 2242
    if (!setTime(root + QStringLiteral("/old"), -86400) || !setTime(root + QStringLiteral("/future"), future)
        || QFileInfo(root + QStringLiteral("/old")).lastModified().toSecsSinceEpoch() != -86400
        || QFileInfo(root + QStringLiteral("/future")).lastModified().toSecsSinceEpoch() != future) {
        GTEST_SKIP() << "the file system keeps no such times";
    }
    ASSERT_EQ(::chmod(QFile::encodeName(root + QStringLiteral("/setuid")).constData(), 06755), 0);

    QString error;
    const auto archive = UploadArchive::build({root}, 1 << 20, error);
    ASSERT_TRUE(archive) << error.toStdString();
    const QString out = temp.filePath(QStringLiteral("out"));
    unpack(*archive, out);
    EXPECT_EQ(QFileInfo(out + QStringLiteral("/t/old")).lastModified().toSecsSinceEpoch(), -86400);
    EXPECT_EQ(QFileInfo(out + QStringLiteral("/t/future")).lastModified().toSecsSinceEpoch(), future);
    const QString listing = runTar(*archive, {QStringLiteral("-tvzf"), QStringLiteral("-")});
    bool seen = false;
    for (const QString& line : listing.split(QLatin1Char('\n'))) {
        if (!line.endsWith(QStringLiteral("/setuid"))) continue;
        seen = true;
        EXPECT_TRUE(line.startsWith(QStringLiteral("-rwxr-xr-x"))) << line.toStdString();
    }
    EXPECT_TRUE(seen) << listing.toStdString();
}

TEST(UploadArchive, RefusesWhatItCannotSendWhole)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    write(temp.filePath(QStringLiteral("noise.bin")), noise(300000));
    QString error;
    EXPECT_FALSE(UploadArchive::build({temp.filePath(QStringLiteral("noise.bin"))}, 100000, error));
    EXPECT_EQ(error, QStringLiteral("The files are larger than Terminal.uploadLimit lets through. Send them with aglow transfer."));
    EXPECT_FALSE(UploadArchive::build({temp.filePath(QStringLiteral("gone"))}, 1 << 20, error));
    EXPECT_EQ(error, temp.filePath(QStringLiteral("gone")) + QStringLiteral(" does not exist."));
    EXPECT_FALSE(UploadArchive::build({}, 1 << 20, error));
    EXPECT_EQ(error, QStringLiteral("No file was chosen."));
    // A pipe chosen alone: refused, never opened.
    const QString fifo = temp.filePath(QStringLiteral("fifo"));
    ASSERT_EQ(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
    EXPECT_FALSE(UploadArchive::build({fifo}, 1 << 20, error));
    EXPECT_EQ(error, fifo + QStringLiteral(" is not a file, a directory or a link."));

    // The limit is on the gzip data: 4 MiB of zeros is far larger than the
    // limit, and its archive is far smaller.
    write(temp.filePath(QStringLiteral("zeros.bin")), QByteArray(4 << 20, '\0'));
    const auto archive = UploadArchive::build({temp.filePath(QStringLiteral("zeros.bin"))}, 100000, error);
    ASSERT_TRUE(archive) << error.toStdString();
    EXPECT_LE(archive->size(), 100000u);
    if (haveTar()) {
        const QString out = temp.filePath(QStringLiteral("out"));
        EXPECT_EQ(unpack(*archive, out), QStringList{QStringLiteral("zeros.bin")});
        EXPECT_TRUE(read(out + QStringLiteral("/zeros.bin")) == QByteArray(4 << 20, '\0'));
    }
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
    EXPECT_LE(UploadArchive::answerSize(std::size_t(Settings::MaxUploadLimit) << 20), AfterglowPty::Pty::MaxPendingInput);
}
