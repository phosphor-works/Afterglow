// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <memory>

namespace Afterglow {
/// OSC 5113 sessions. Filesystem and compression work runs off the GUI thread.
class FileTransfer : public QObject {
    Q_OBJECT
public:
    explicit FileTransfer(QObject* parent = nullptr);
    ~FileTransfer() override;
    void handle(const QByteArray& command);
    void approve(const QString& token, bool allowed);
    void reset();
    void abort();
    void setPassword(const QString& password);
Q_SIGNALS:
    void sessionClosed(const QString& token);
    void reply(const QByteArray& bytes);
    void permissionRequested(const QString& token, bool writing, const QStringList& paths);

private:
    struct Worker;
    std::unique_ptr<Worker> m_worker;
};
} // namespace Afterglow
