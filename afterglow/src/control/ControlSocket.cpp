// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ControlSocket.h"

#include "../config/Settings.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QLoggingCategory>

#include <algorithm>
#include <limits>

#include <sys/stat.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcControl, "afterglow.control", QtInfoMsg)

namespace Afterglow {

namespace {

QString g_processPath;

// Commands and user variables can carry secrets: they leave the process
// only with read access.
QString readable(const SettingsStore& settings, const QString& text)
{
    return settings.settings().control.allowReadText ? text : QString();
}

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

QString defaultPath()
{
    const QDBusConnection bus = QDBusConnection::sessionBus();
    const QDBusConnectionInterface* interface = bus.interface();
    const bool owner = interface && !bus.baseService().isEmpty()
        && interface->serviceOwner(QStringLiteral(AFTERGLOW_APP_ID)).value() == bus.baseService();
    return owner ? ControlSocket::sharedPath() : ControlSocket::ownPath();
}

} // namespace

ControlApi::ControlApi(Control& control, SettingsStore& settings, QObject* parent)
    : QObject(parent)
    , m_control(control)
    , m_settings(settings)
{
}

QString ControlApi::readable(const QString& text) const
{
    return Afterglow::readable(m_settings, text);
}

bool ControlApi::failed(const QString& error)
{
    if (error.isEmpty()) return false;
    sendErrorReply(error);
    return true;
}

QVariantList ControlApi::listPanes()
{
    QVariantList panes;
    for (PaneInfo& p : m_control.panes()) {
        p.command = readable(p.command);
        panes.append(Control::paneMap(p));
    }
    return panes;
}

uint ControlApi::focusedPane()
{
    if (const auto pane = m_control.focusedPane()) return *pane;
    failed(QStringLiteral("no focused pane"));
    return 0;
}

void ControlApi::sendText(uint pane, const QString& text)
{
    failed(m_control.sendText(pane, text));
}

QString ControlApi::getText(uint pane, const QString& scope)
{
    const auto s = Control::scopeFromName(scope);
    if (!s) {
        failed(QStringLiteral("scope is screen, scrollback, selection or last-output"));
        return {};
    }
    QString text;
    failed(m_control.readText(pane, *s, text));
    return text;
}

uint ControlApi::split(uint pane, const QString& direction, const QStringList& command)
{
    quint32 created = 0;
    failed(m_control.split(pane, direction, command, {}, created));
    return created;
}

uint ControlApi::newTab(uint pane, const QString& workingDirectory, const QStringList& command)
{
    quint32 created = 0;
    failed(m_control.newTab(pane, workingDirectory, command, created));
    return created;
}

uint ControlApi::newWindow(const QString& workingDirectory, const QStringList& command)
{
    quint32 created = 0;
    failed(m_control.newWindow(workingDirectory, command, created));
    return created;
}

void ControlApi::focus(uint pane)
{
    failed(m_control.focus(pane));
}

void ControlApi::close(uint pane)
{
    failed(m_control.close(pane));
}

void ControlApi::setTitle(uint pane, const QString& title)
{
    failed(m_control.setTitle(pane, title));
}

void ControlApi::signal(uint pane, int channel, double value)
{
    failed(m_control.signal(pane, channel, value));
}

void ControlApi::notify(uint pane, const QString& title, const QString& body)
{
    failed(m_control.notify(pane, title, body));
}

void ControlApi::runAction(uint pane, const QString& action)
{
    // These move pane text to the clipboard, into other panes or into a pager
    // file: the same reading that getText needs permission for.
    if (!m_settings.settings().control.allowReadText && Control::readsPaneText(action)) {
        failed(QStringLiteral("action %1 reads pane text, which is off (Control.allowReadText)").arg(action));
        return;
    }
    failed(m_control.runAction(pane, action));
}

QVariantMap ControlApi::movePane(uint pane, uint target, const QString& placement)
{
    QVariantMap result;
    failed(m_control.movePane(pane, target, placement, result));
    return result;
}

QVariantMap ControlApi::detachTab(uint pane)
{
    QVariantMap result;
    failed(m_control.detachTab(pane, result));
    return result;
}

QVariantMap ControlApi::openDirectory(const QString& directory, const QString& layout)
{
    QVariantMap result;
    failed(m_control.openDirectory(directory, layout, result));
    return result;
}

QStringList ControlApi::projects()
{
    return m_control.projectNames();
}

QVariantMap ControlApi::saveProject(uint pane, const QString& name, bool overwrite)
{
    QVariantMap result;
    failed(m_control.saveProject(pane, name, overwrite, result));
    return result;
}

QVariantMap ControlApi::openProject(const QString& name, bool newInstance)
{
    QVariantMap result;
    failed(m_control.openProject(name, newInstance, result));
    return result;
}

QVariantList ControlApi::scriptDiagnostics()
{
    return m_control.scriptDiagnostics();
}

ControlSocket::ControlSocket(Control& control, SettingsStore& settings, const QString& path, QObject* parent)
    : QObject(parent)
    , m_control(control)
    , m_settings(settings)
    , m_api(control, settings)
    , m_path(path.isEmpty() ? defaultPath() : path)
    , m_due(this)
{
    g_processPath = m_path;
    if (const QString runtime = runtimeDirectory(); !runtime.isEmpty()) removeStaleSockets(runtime);
    m_clock.start();
    m_due.setSingleShot(true);
    connect(&m_due, &QTimer::timeout, this, &ControlSocket::sendDue);
    if (!m_router.registerTarget(QLatin1String(Target), &m_api)) {
        qCWarning(lcControl) << "cannot register the control target";
    }

    const auto pane = [](quint32 id) { return QJsonValue(qint64(id)); };
    connect(&control, &Control::paneCreated, this, [this, pane](quint32 p) { send("paneCreated", {pane(p)}); });
    connect(&control, &Control::paneMoved, this, [this, pane](quint32 p) { send("paneMoved", {pane(p)}); });
    connect(&control, &Control::paneClosed, this, [this, pane](quint32 p) {
        // A client hears the pane's last title and status before it closes.
        flushPane(p);
        send("paneClosed", {pane(p)});
    });
    connect(&control, &Control::focusChanged, this, [this, pane](quint32 p) { send("focusChanged", {pane(p)}); });
    connect(&control, &Control::commandStarted, this, [this, pane](quint32 p, const QString& c) {
        send("commandStarted", {pane(p), readable(m_settings, c)});
    });
    connect(&control, &Control::commandFinished, this, [this, pane](quint32 p, const QString& c, int code, qint64 ms) {
        send("commandFinished", {pane(p), readable(m_settings, c), code, ms});
    });
    connect(&control, &Control::titleChanged, this, [this, pane](quint32 p, const QString& title) {
        sendMerged("titleChanged", p, {}, {pane(p), title});
    });
    connect(&control, &Control::bell, this, [this, pane](quint32 p) { sendMerged("bell", p, {}, {pane(p)}); });
    connect(&control, &Control::statusChanged, this, [this, pane](quint32 p, const QString& state, const QString& text) {
        sendMerged("statusChanged", p, {}, {pane(p), state, text});
    });
    connect(&control, &Control::contextChanged, this, [this, pane](quint32 p, const QString& context) {
        sendMerged("contextChanged", p, {}, {pane(p), context});
    });
    connect(&control, &Control::userVarChanged, this, [this, pane](quint32 p, const QString& name, const QString& value) {
        sendMerged("userVarChanged", p, name, {pane(p), name, readable(m_settings, value)});
    });
    connect(&settings, &SettingsStore::changed, this, &ControlSocket::applySettings);
    applySettings();
}

ControlSocket::~ControlSocket()
{
    m_router.stop();
    m_router.unregisterTarget(QLatin1String(Target), &m_api);
    if (g_processPath == m_path) g_processPath.clear();
}

QString ControlSocket::processPath()
{
    return g_processPath;
}

QString ControlSocket::sharedPath()
{
    const QString runtime = runtimeDirectory();
    if (runtime.isEmpty()) return {};
    return QStringLiteral("%1/afterglow-%2.sock").arg(runtime, displayName());
}

QString ControlSocket::ownPath()
{
    const QString runtime = runtimeDirectory();
    if (runtime.isEmpty()) return {};
    return QStringLiteral("%1/afterglow-%2-%3.sock").arg(runtime, displayName()).arg(QCoreApplication::applicationPid());
}

void ControlSocket::applySettings()
{
    const bool want = m_settings.settings().control.enabled;
    if (want && !m_listening) {
        if (m_path.isEmpty()) {
            qCWarning(lcControl) << "no control socket: XDG_RUNTIME_DIR is not set";
            return;
        }
        m_listening = m_router.start(m_path);
        if (!m_listening) {
            qCWarning(lcControl).noquote() << "cannot listen on" << m_path
                                           << "(another process listens there, or the directory is not writable)";
        }
    } else if (!want && m_listening) {
        m_router.stop();
        m_listening = false;
        m_merged.clear();
        m_due.stop();
    }
    // A shorter interval can make waiting events due sooner.
    if (!m_merged.isEmpty()) scheduleDue();
}

void ControlSocket::send(const char* event, const QJsonArray& args)
{
    if (m_listening) m_router.broadcastEvent(QLatin1String(Target), QLatin1String(event), args);
}

void ControlSocket::sendMerged(const char* event, quint32 pane, const QString& name, const QJsonArray& args)
{
    if (!m_listening) return;
    if (m_settings.settings().control.eventInterval <= 0) {
        send(event, args);
        return;
    }
    const QString key = QString::number(pane) + QLatin1Char('/') + QLatin1String(event) + QLatin1Char('/') + name;
    const qint64 now = m_clock.elapsed();
    const auto it = m_merged.find(key);
    if (it == m_merged.end()) {
        m_merged.insert(key, {event, pane, {}, now, false});
        send(event, args);
    } else {
        // An entry is younger than the interval: sendDue removes it when it
        // is due and nothing waits.
        it->args = args;
        it->waiting = true;
    }
    // A new entry is due last, after everything that the timer waits for.
    if (!m_due.isActive()) scheduleDue();
}

void ControlSocket::sendDue()
{
    const qint64 interval = m_settings.settings().control.eventInterval;
    const qint64 now = m_clock.elapsed();
    for (auto it = m_merged.begin(); it != m_merged.end();) {
        if (now - it->sentAt < interval) {
            ++it;
        } else if (it->waiting) {
            it->waiting = false;
            it->sentAt = now;
            send(it->event, it->args);
            ++it;
        } else {
            it = m_merged.erase(it);
        }
    }
    scheduleDue();
}

void ControlSocket::flushPane(quint32 pane)
{
    for (auto it = m_merged.begin(); it != m_merged.end();) {
        if (it->pane != pane) {
            ++it;
            continue;
        }
        if (it->waiting) send(it->event, it->args);
        it = m_merged.erase(it);
    }
    scheduleDue();
}

void ControlSocket::scheduleDue()
{
    if (m_merged.isEmpty()) {
        m_due.stop();
        return;
    }
    const qint64 interval = m_settings.settings().control.eventInterval;
    qint64 next = std::numeric_limits<qint64>::max();
    for (const Merged& m : std::as_const(m_merged)) next = std::min(next, m.sentAt + interval);
    m_due.start(int(std::clamp<qint64>(next - m_clock.elapsed(), 0, interval)));
}

} // namespace Afterglow
