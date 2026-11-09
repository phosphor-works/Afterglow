// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "InstanceSocket.h"

#include <AfterglowProto/Lock.h>
#include <AfterglowProto/Socket.h>
#include <PhosphorIpc/IpcRouter.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QSet>

#include <string>

#include <sys/stat.h>
#include <unistd.h>

Q_DECLARE_LOGGING_CATEGORY(lcApp)

namespace Afterglow {

namespace {

QString g_processPath;

std::string lockPath(const QString& socket)
{
    return QFile::encodeName(socket + QStringLiteral(".lock")).toStdString();
}

// The lock on @p socket's lock file, as afterglow-mux takes the lock of its
// sockets: AfterglowProto::LockHeld while another holder has it,
// AfterglowProto::LockFailed when the file cannot be opened or locked.
int lockSocket(const QString& socket)
{
    std::string error;
    const int fd = AfterglowProto::lockFile(lockPath(socket), &error);
    if (fd == AfterglowProto::LockFailed) qCWarning(lcApp).noquote() << QString::fromStdString(error);
    return fd;
}

void unlockSocket(const QString& socket, int fd)
{
    AfterglowProto::unlockFile(lockPath(socket), fd);
}

enum class SocketFile { None, Ours, Other };

SocketFile socketFile(const QString& path)
{
    struct stat info{};
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0) return SocketFile::None;
    return S_ISSOCK(info.st_mode) && info.st_uid == ::getuid() ? SocketFile::Ours : SocketFile::Other;
}

// A listener without the lock (an older build) still answers: only a refused
// connection proves that none is there. A busy listener times out.
bool refusesConnections(const QString& path)
{
    QLocalSocket probe;
    probe.connectToServer(path);
    if (probe.waitForConnected(100)) {
        probe.abort();
        return false;
    }
    return probe.error() == QLocalSocket::ConnectionRefusedError;
}

} // namespace

InstanceSocket::InstanceSocket()
    : m_router(std::make_unique<PhosphorIpc::IpcRouter>())
{
}

InstanceSocket::~InstanceSocket()
{
    // The socket file goes first: while the lock is held, no other process
    // can take the path and lose its file to this one.
    m_router->stop();
    if (m_lock >= 0) unlockSocket(m_path, m_lock);
    if (g_processPath == m_path) g_processPath.clear();
}

InstanceSocket::Listen InstanceSocket::listen(const QString& path)
{
    if (path.isEmpty() || listening()) return Listen::Failed;
    const int lock = lockSocket(path);
    if (lock == AfterglowProto::LockHeld) return Listen::Held;
    if (lock == AfterglowProto::LockFailed) return Listen::Failed;
    // The last holder of the lock is gone. What it left is removed.
    switch (socketFile(path)) {
    case SocketFile::None:
        break;
    case SocketFile::Ours:
        if (refusesConnections(path)) {
            QFile::remove(path);
            break;
        }
        // A listener without the lock: an older build.
        unlockSocket(path, lock);
        return Listen::Held;
    case SocketFile::Other:
        qCWarning(lcApp) << path << "is not a socket of this user";
        unlockSocket(path, lock);
        return Listen::Failed;
    }
    if (!m_router->start(path)) {
        unlockSocket(path, lock);
        return Listen::Failed;
    }
    m_lock = lock;
    m_path = path;
    g_processPath = path;
    return Listen::Listening;
}

void InstanceSocket::removeStaleSockets()
{
    const QString runtime = runtimeDirectory();
    if (runtime.isEmpty()) return;
    const QDir dir(runtime);
    const QString lockSuffix = QStringLiteral(".lock");
    QSet<QString> sockets;
    const QStringList names = dir.entryList({QStringLiteral("afterglow-*.sock"), QStringLiteral("afterglow-*.sock.lock")},
                                            QDir::Files | QDir::System | QDir::Hidden);
    for (QString name : names) {
        if (name.endsWith(lockSuffix)) name.chop(lockSuffix.size());
        sockets.insert(dir.filePath(name));
    }
    for (const QString& path : std::as_const(sockets)) {
        const SocketFile file = socketFile(path);
        if (file == SocketFile::Other) continue;
        const int lock = lockSocket(path);
        if (lock < 0) continue;
        if (file == SocketFile::Ours && refusesConnections(path)) QFile::remove(path);
        unlockSocket(path, lock);
    }
}

QString InstanceSocket::runtimeDirectory()
{
    // A leading '/' as AfterglowProto::sharedSocketPath takes it:
    // QDir::isAbsolutePath takes a Qt resource path too.
    const QString directory = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return directory.startsWith(QLatin1Char('/')) ? directory : QString();
}

int InstanceSocket::lock(const QString& socket)
{
    return lockSocket(socket);
}

void InstanceSocket::unlock(const QString& socket, int fd)
{
    unlockSocket(socket, fd);
}

QString InstanceSocket::processPath()
{
    return g_processPath;
}

QString InstanceSocket::sharedPath()
{
    // The clients find the path through the same function (AfterglowProto::findSocket).
    return QFile::decodeName(QByteArray::fromStdString(AfterglowProto::sharedSocketPath()));
}

QString InstanceSocket::ownPath()
{
    return QFile::decodeName(QByteArray::fromStdString(AfterglowProto::ownSocketPath(QCoreApplication::applicationPid())));
}

} // namespace Afterglow
