// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <functional>
#include <memory>

namespace Afterglow {
/// OSC 5113 sessions. Filesystem and compression work runs off the GUI thread.
class FileTransfer : public QObject {
    Q_OBJECT
public:
    /// The delta limit in bytes until setDeltaLimit() is called. It matches
    /// the default of Terminal.fileTransferDeltaLimit.
    static constexpr qint64 DefaultDeltaLimit = qint64(64) << 30;

    explicit FileTransfer(QObject* parent = nullptr);
    ~FileTransfer() override;
    void handle(const QByteArray& command);
    /// The answer to permissionRequested. A Yes approves the paths that it
    /// gave, and no others.
    void approve(const QString& token, bool allowed);
    /// The prompt of a permission request from @p asker: the question, with
    /// the first paths, a count of the others and a count of the paths that
    /// the answer does not cover, and the detail, with every path that
    /// permissionRequested gives.
    struct Question {
        QString text;
        QString detail;
    };
    static Question question(bool writing, const QStringList& paths, int total, const QString& asker = { });
    /// Paths that the question shows in its text, and the characters of each.
    static constexpr int QuestionPaths = 10;
    static constexpr int QuestionLength = 160;
    void reset();
    void abort();
    void setPassword(const QString& password);
    /// The most bytes that a delta upload without a declared size can write.
    /// Copy operations let a few bytes of input write a whole old file again,
    /// so such an upload has no other bound. 0 sets no limit.
    void setDeltaLimit(qint64 bytes);
    /// Reports the bytes that wait in the PTY input queue. Called on the GUI thread.
    /// Outgoing data pauses while it is large.
    void setInputBacklog(std::function<std::size_t()> backlog);
Q_SIGNALS:
    void sessionClosed(const QString& token);
    void reply(const QByteArray& bytes);
    /// handle() took a command. answered() follows once for each, in order,
    /// after the reply() of each packet that the worker queued until it had
    /// handled the command. A packet that a reset, an abort, an overflow or a
    /// cancel drops is not waited for.
    void answering();
    void answered();
    /// @p paths holds at most the first 100 of the @p total paths that wait,
    /// for display: control characters, line separators, invisible format
    /// characters and backslashes in them are written as \uXXXX (\UXXXXXXXX
    /// above U+FFFF). A path that links lead elsewhere names where. For a
    /// read, the files that the program asks for, a directory with a slash:
    /// the program reads all of it. For a write, the destinations of the
    /// files that the program named: a send session asks once the worker has
    /// handled what came with its first file. The files wait until the
    /// answer. The paths stay as they are: what comes later waits for a
    /// request of its own, and a Yes approves only @p paths.
    void permissionRequested(const QString& token, bool writing, const QStringList& paths, int total);

private:
    struct Worker;
    std::unique_ptr<Worker> m_worker;
};
} // namespace Afterglow
