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
    void approve(const QString& token, bool allowed);
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
    /// after the reply() signals of what the worker sent while it handled the
    /// command. A reset or an abort answers the commands that it dropped.
    void answering();
    void answered();
    /// @p paths holds at most the first 100 of the @p total requested paths,
    /// for display: control characters, line separators, bidirectional
    /// controls and backslashes in them are written as \uXXXX.
    void permissionRequested(const QString& token, bool writing, const QStringList& paths, int total);

private:
    struct Worker;
    std::unique_ptr<Worker> m_worker;
};
} // namespace Afterglow
