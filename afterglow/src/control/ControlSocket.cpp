// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ControlSocket.h"

#include "../config/Settings.h"

#include <QLoggingCategory>

#include <algorithm>
#include <limits>

Q_LOGGING_CATEGORY(lcControl, "afterglow.control", QtInfoMsg)

namespace Afterglow {

namespace {

// Commands and user variables can carry secrets: they leave the process
// only with read access.
QString readable(const SettingsStore& settings, const QString& text)
{
    return settings.settings().control.allowReadText ? text : QString();
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

ControlSocket::ControlSocket(Control& control, SettingsStore& settings, PhosphorIpc::IpcRouter& router, QObject* parent)
    : QObject(parent)
    , m_control(control)
    , m_settings(settings)
    , m_api(control, settings)
    , m_router(router)
    , m_due(this)
{
    m_clock.start();
    m_due.setSingleShot(true);
    connect(&m_due, &QTimer::timeout, this, &ControlSocket::sendDue);

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
    m_router.unregisterTarget(QLatin1String(Target), &m_api);
}

void ControlSocket::applySettings()
{
    const bool want = m_settings.settings().control.enabled;
    if (want != m_enabled) {
        if (want) {
            m_enabled = m_router.registerTarget(QLatin1String(Target), &m_api);
            if (!m_enabled) qCWarning(lcControl) << "another object holds the control target";
        } else {
            // Ends the subscriptions too: a client must subscribe again
            // after control comes back.
            m_router.unregisterTarget(QLatin1String(Target), &m_api);
            m_enabled = false;
            m_merged.clear();
            m_due.stop();
        }
    }
    // A shorter interval can make waiting events due sooner.
    if (!m_merged.isEmpty()) scheduleDue();
}

void ControlSocket::send(const char* event, const QJsonArray& args)
{
    if (m_enabled) m_router.broadcastEvent(QLatin1String(Target), QLatin1String(event), args);
}

void ControlSocket::sendMerged(const char* event, quint32 pane, const QString& name, const QJsonArray& args)
{
    if (!m_enabled) return;
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
