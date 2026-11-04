// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "InstanceSocket.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QLoggingCategory>

#include <sys/stat.h>
#include <unistd.h>

Q_DECLARE_LOGGING_CATEGORY(lcApp)

namespace Afterglow {

namespace {

QString g_processPath;

// Qt's own default, so that the application and `afterglow ctl` agree when the
// variable is unset. A display can be an absolute path: only its name counts.
QString displayName()
{
    QString name = QFileInfo(qEnvironmentVariable("WAYLAND_DISPLAY")).fileName();
    for (QChar& c : name) {
        const char16_t u = c.unicode();
        const bool plain = (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9') || u == u'-'
            || u == u'_' || u == u'.';
        if (!plain) c = QLatin1Char('_');
    }
    return name.isEmpty() ? QStringLiteral("wayland-0") : name;
}

QString runtimeDirectory()
{
    return qEnvironmentVariable("XDG_RUNTIME_DIR");
}

// A process that ends without its destructors (a crash, SIGKILL) leaves its
// socket file. Separate processes and nested displays use names that no later
// process takes again, so nothing would remove them. A socket that refuses a
// connection has no listener: a busy process still accepts one.
void removeStaleSockets(const QString& runtime)
{
    const QDir dir(runtime);
    for (const QString& name : dir.entryList({QStringLiteral("afterglow-*.sock")}, QDir::System | QDir::Hidden)) {
        const QString path = dir.filePath(name);
        struct stat info{};
        if (::lstat(QFile::encodeName(path).constData(), &info) != 0 || !S_ISSOCK(info.st_mode) || info.st_uid != ::getuid()) {
            continue;
        }
        QLocalSocket probe;
        probe.connectToServer(path);
        if (probe.waitForConnected(100)) {
            probe.abort();
            continue;
        }
        if (probe.error() == QLocalSocket::ConnectionRefusedError) {
            QFile::remove(path);
        }
    }
}

} // namespace

InstanceSocket::InstanceSocket()
{
    if (const QString runtime = runtimeDirectory(); !runtime.isEmpty()) removeStaleSockets(runtime);
}

InstanceSocket::~InstanceSocket()
{
    m_router.stop();
    if (g_processPath == m_path) g_processPath.clear();
}

bool InstanceSocket::listen(const QString& path)
{
    if (path.isEmpty()) {
        qCWarning(lcApp) << "no socket for launches and control: XDG_RUNTIME_DIR is not set";
        return false;
    }
    if (!m_router.start(path)) return false;
    m_path = path;
    g_processPath = path;
    return true;
}

QString InstanceSocket::processPath()
{
    return g_processPath;
}

QString InstanceSocket::sharedPath()
{
    const QString runtime = runtimeDirectory();
    if (runtime.isEmpty()) return {};
    return QStringLiteral("%1/afterglow-%2.sock").arg(runtime, displayName());
}

QString InstanceSocket::ownPath()
{
    const QString runtime = runtimeDirectory();
    if (runtime.isEmpty()) return {};
    return QStringLiteral("%1/afterglow-%2-%3.sock").arg(runtime, displayName()).arg(QCoreApplication::applicationPid());
}

} // namespace Afterglow
