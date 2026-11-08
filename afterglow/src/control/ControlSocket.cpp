// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ControlSocket.h"

#include "RemoteSockets.h"
#include "../config/Settings.h"

#include <QJsonObject>
#include <QLoggingCategory>
#include <QPointer>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

Q_LOGGING_CATEGORY(lcControl, "afterglow.control", QtInfoMsg)

namespace Afterglow {

namespace {

// Commands and user variables can carry secrets: they leave the process
// only with read access.
QString readable(const SettingsStore& settings, const QString& text)
{
    return settings.settings().control.allowReadText ? text : QString();
}

bool isWhole(double value)
{
    return std::isfinite(value) && std::floor(value) == value;
}

QJsonValue paneValue(quint32 id)
{
    return QJsonValue(qint64(id));
}

} // namespace

ControlApi::ControlApi(Control& control, SettingsStore& settings, QObject* parent)
    : QObject(parent)
    , m_control(control)
    , m_settings(settings)
{
}

bool ControlApi::failed(const QString& error)
{
    if (error.isEmpty()) return false;
    sendErrorReply(error);
    return true;
}

std::optional<quint32> ControlApi::paneId(double value)
{
    // JSON numbers arrive as doubles. A conversion to uint would wrap -1 and
    // 2^32 + 1 onto real panes, and round 1.6 to pane 2.
    if (isWhole(value) && value >= 0 && value <= double(std::numeric_limits<quint32>::max())) return quint32(value);
    failed(QStringLiteral("a pane ID is a whole number from 0 to %1").arg(std::numeric_limits<quint32>::max()));
    return std::nullopt;
}

QVariantList ControlApi::listPanes()
{
    QVariantList panes;
    for (PaneInfo& p : m_control.panes()) {
        p.command = readable(m_settings, p.command);
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

void ControlApi::sendText(double pane, const QString& text)
{
    if (const auto id = paneId(pane)) failed(m_control.sendText(*id, text));
}

QString ControlApi::getText(double pane, const QString& scope)
{
    const auto id = paneId(pane);
    if (!id) return {};
    const auto s = Control::scopeFromName(scope);
    if (!s) {
        failed(QStringLiteral("scope is screen, scrollback, selection or last-output"));
        return {};
    }
    QString text;
    failed(m_control.readText(*id, *s, text));
    return text;
}

uint ControlApi::split(double pane, const QString& direction, const QStringList& command)
{
    quint32 created = 0;
    if (const auto id = paneId(pane)) failed(m_control.split(*id, direction, command, {}, created));
    return created;
}

uint ControlApi::newTab(double pane, const QString& workingDirectory, const QStringList& command)
{
    quint32 created = 0;
    if (const auto id = paneId(pane)) failed(m_control.newTab(*id, workingDirectory, command, created));
    return created;
}

uint ControlApi::newWindow(const QString& workingDirectory, const QStringList& command)
{
    quint32 created = 0;
    failed(m_control.newWindow(workingDirectory, command, created));
    return created;
}

void ControlApi::focus(double pane)
{
    if (const auto id = paneId(pane)) failed(m_control.focus(*id));
}

void ControlApi::close(double pane)
{
    if (const auto id = paneId(pane)) failed(m_control.close(*id));
}

void ControlApi::setTitle(double pane, const QString& title)
{
    if (const auto id = paneId(pane)) failed(m_control.setTitle(*id, title));
}

void ControlApi::signal(double pane, double channel, double value)
{
    const auto id = paneId(pane);
    if (!id) return;
    // Control names the valid channels for any value outside them.
    const int index = isWhole(channel) && channel >= 0 && channel <= 1024 ? int(channel) : -1;
    failed(m_control.signal(*id, index, value));
}

void ControlApi::notify(double pane, const QString& title, const QString& body)
{
    if (const auto id = paneId(pane)) failed(m_control.notify(*id, title, body));
}

void ControlApi::runAction(double pane, const QString& action)
{
    const auto id = paneId(pane);
    if (!id) return;
    // These move pane text to the clipboard, into other panes or into a pager
    // file: the same reading that getText needs permission for.
    if (!m_settings.settings().control.allowReadText && Control::readsPaneText(action)) {
        failed(QStringLiteral("action %1 reads pane text, which is off (Control.allowReadText)").arg(action));
        return;
    }
    failed(m_control.runAction(*id, action));
}

QVariantMap ControlApi::movePane(double pane, double target, const QString& placement)
{
    QVariantMap result;
    const auto id = paneId(pane);
    if (!id) return result;
    const auto to = paneId(target);
    if (!to) return result;
    failed(m_control.movePane(*id, *to, placement, result));
    return result;
}

QVariantMap ControlApi::detachTab(double pane)
{
    QVariantMap result;
    if (const auto id = paneId(pane)) failed(m_control.detachTab(*id, result));
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

QVariantMap ControlApi::saveProject(double pane, const QString& name, bool overwrite)
{
    QVariantMap result;
    if (const auto id = paneId(pane)) failed(m_control.saveProject(*id, name, overwrite, result));
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

bool ControlApi::remoteCommandsEnabled()
{
    return m_remote && m_remote->enabled();
}

QVariantMap ControlApi::openRemoteSocket(double pane, double pid, const QString& host, const QString& directory,
                                         double protocol, const QStringList& ssh, const QString& destination)
{
    const auto id = paneId(pane);
    if (!id) return {};
    if (!m_remote) {
        failed(QStringLiteral("this afterglow has no remote sockets"));
        return {};
    }
    if (!isWhole(pid) || !isWhole(protocol) || std::abs(protocol) > 1e9) {
        failed(QStringLiteral("the process ID and the protocol are whole numbers"));
        return {};
    }
    RemoteSockets::Opened opened;
    if (failed(m_remote->open({*id, qint64(std::clamp(pid, -1.0, 1e12)), host, directory, int(protocol), ssh, destination},
                              opened)))
        return {};
    QVariantMap result{{QStringLiteral("token"), opened.token}};
    if (!opened.error.isEmpty()) {
        result.insert(QStringLiteral("error"), opened.error);
        return result;
    }
    result.insert(QStringLiteral("socket"), opened.socket);
    result.insert(QStringLiteral("path"), opened.path);
    return result;
}

void ControlApi::remoteSocketForwarded(const QString& token)
{
    if (!m_remote) {
        failed(QStringLiteral("this afterglow has no remote sockets"));
        return;
    }
    failed(m_remote->forwarded(token));
}

void ControlApi::remoteSocketFailed(const QString& token, const QString& reason)
{
    if (!m_remote) {
        failed(QStringLiteral("this afterglow has no remote sockets"));
        return;
    }
    failed(m_remote->failed(token, reason));
}

bool ControlApi::overlayInput(const QString& input, const QString& chunk, bool eof)
{
    static const QRegularExpression rule(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    if (!rule.match(input).hasMatch()) return !failed(QStringLiteral("an input name is 1 to 64 letters, digits, _ and -"));
    const quint64 connection = connectionId();
    if (!connection) return !failed(QStringLiteral("an input needs a client connection"));
    // Looked up, not made: a refused first part leaves no books behind.
    const auto books = m_inputs.constFind(connection);
    const qsizetype waitingInputs = books == m_inputs.cend() ? 0 : books->size();
    const auto known = books == m_inputs.cend() ? nullptr : &*books;
    const auto entry = known ? known->constFind(input) : QHash<QString, WaitingInput>::const_iterator();
    const bool exists = known && entry != known->cend();
    if (!exists && waitingInputs >= MaxWaitingInputs)
        return !failed(QStringLiteral("%1 inputs wait for their overlay already").arg(MaxWaitingInputs));
    if (exists && entry->eof) return !failed(QStringLiteral("the input %1 has ended").arg(input));
    const QByteArray bytes = chunk.toUtf8();
    const qsizetype size = exists ? entry->bytes.size() : 0;
    if (size + bytes.size() > OverlaySpec::MaxInput) {
        dropInput(connection, input);
        return !failed(QStringLiteral("the overlay's stdin is over %1 MiB").arg(OverlaySpec::MaxInput >> 20));
    }
    if (m_inputBytes + bytes.size() > MaxWaitingInputBytes)
        return !failed(QStringLiteral("afterglow keeps at most %1 MiB of input for overlays that have not started")
                           .arg(MaxWaitingInputBytes >> 20));
    WaitingInput& waiting = m_inputs[connection][input];
    waiting.bytes += bytes;
    waiting.eof = eof;
    m_inputBytes += bytes.size();
    return true;
}

void ControlApi::dropInput(quint64 connection, const QString& name)
{
    const auto inputs = m_inputs.find(connection);
    if (inputs == m_inputs.end()) return;
    m_inputBytes -= inputs->take(name).bytes.size();
    if (inputs->isEmpty()) m_inputs.erase(inputs);
}

void ControlApi::overlay(double pane, const QStringList& argv, const QVariantMap& given)
{
    QString error;
    // Standard input that came in parts: the call takes it, whatever it
    // decides, also for a pane ID that it refuses.
    QVariantMap options = given;
    if (options.contains(QStringLiteral("input"))) {
        const QString name = options.take(QStringLiteral("input")).toString();
        QByteArray bytes;
        bool complete = false;
        if (const auto inputs = m_inputs.constFind(connectionId()); inputs != m_inputs.cend()) {
            if (const auto input = inputs->constFind(name); input != inputs->cend() && input->eof) {
                bytes = input->bytes;
                complete = true;
            }
        }
        dropInput(connectionId(), name);
        if (options.contains(QStringLiteral("stdin"))) {
            failed(QStringLiteral("the overlay options stdin and input exclude each other"));
            return;
        }
        if (!complete) {
            failed(QStringLiteral("no complete input %1 on this connection").arg(name));
            return;
        }
        options.insert(QStringLiteral("stdin"), QString::fromUtf8(bytes));
    }
    const auto id = paneId(pane);
    if (!id) return;
    auto spec = OverlaySpec::read(argv, options, &error);
    // A client names a directory of this machine: one that is missing is
    // its mistake, where a script's remote path starts in $HOME.
    if (spec) error = Control::localDirectory(spec->cwd);
    if (failed(error)) return;
    const quint64 connection = connectionId();
    auto reply = std::make_shared<PhosphorIpc::IpcPendingReply>(deferReply());
    if (!reply->isValid()) {
        // A call from no connection has nobody to answer later; past the
        // router's limit of waiting calls, the router fails it.
        if (!connection) failed(QStringLiteral("an overlay needs a client connection"));
        return;
    }
    // From here on a failure has a code of its own, which only the pending
    // reply sends.
    quint32 target = *id;
    if (target == 0) {
        const auto focused = m_control.focusedPane();
        if (!focused) {
            reply->fail(QStringLiteral("no_pane"), QStringLiteral("no focused pane"));
            return;
        }
        target = *focused;
    }
    QString code;
    error = m_control.overlay(overlayOwner(connection), {}, target, *spec,
        [reply, api = QPointer<ControlApi>(this)](const QString& error, const QString& code, const QVariantMap& result) {
            if (!error.isEmpty()) {
                reply->fail(code, error);
                return;
            }
            // Gated as it goes out, as events are: read access can go while
            // the program runs.
            QVariantMap answer = result;
            if (!api || !api->m_settings.settings().control.allowReadText) answer.remove(QStringLiteral("stdout"));
            reply->finish(QJsonObject::fromVariantMap(answer));
        }, code);
    if (!error.isEmpty()) {
        reply->fail(code, error);
        return;
    }
    m_overlayClients.insert(connection);
}

void ControlApi::cancelOverlays(const QString& message)
{
    const QSet<quint64> clients = std::exchange(m_overlayClients, {});
    for (const quint64 connection : clients) m_control.closeOverlays(overlayOwner(connection), message);
}

void ControlApi::dropInputs()
{
    m_inputs.clear();
    m_inputBytes = 0;
}

void ControlApi::connectionClosed(quint64 connection)
{
    for (const WaitingInput& input : m_inputs.take(connection)) m_inputBytes -= input.bytes.size();
    if (m_overlayClients.remove(connection)) m_control.closeOverlays(overlayOwner(connection), QStringLiteral("The client went away"));
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
    m_due.setObjectName(QStringLiteral("eventIntervalTimer"));
    m_due.setSingleShot(true);
    connect(&m_due, &QTimer::timeout, this, &ControlSocket::sendDue);

    // A client that goes takes its overlays with it: nobody waits for them.
    connect(&router, &PhosphorIpc::IpcRouter::connectionClosed, this, [this](quint64 connection) { m_api.connectionClosed(connection); });
    connect(&control, &Control::paneCreated, this, [this](quint32 p) { sendNow("paneCreated", p, {paneValue(p)}); });
    connect(&control, &Control::paneMoved, this, [this](quint32 p) { sendNow("paneMoved", p, {paneValue(p)}); });
    connect(&control, &Control::paneClosed, this, [this](quint32 p) {
        sendNow("paneClosed", p, {paneValue(p)});
        // Nothing more comes for the pane.
        forgetPane(p);
    });
    connect(&control, &Control::focusChanged, this, [this](quint32 p) { sendNow("focusChanged", p, {paneValue(p)}); });
    connect(&control, &Control::commandStarted, this, [this](quint32 p, const QString& c) {
        sendNow("commandStarted", p, {paneValue(p), readable(m_settings, c)});
    });
    connect(&control, &Control::commandFinished, this, [this](quint32 p, const QString& c, int code, qint64 ms) {
        sendNow("commandFinished", p, {paneValue(p), readable(m_settings, c), code, ms});
    });
    connect(&control, &Control::titleChanged, this, [this](quint32 p, const QString& title) {
        sendMerged("titleChanged", p, {}, {paneValue(p), title});
    });
    connect(&control, &Control::bell, this, [this](quint32 p) { sendMerged("bell", p, {}, {paneValue(p)}); });
    connect(&control, &Control::statusChanged, this, [this](quint32 p, const QString& state, const QString& text) {
        sendMerged("statusChanged", p, {}, {paneValue(p), state, text});
    });
    connect(&control, &Control::contextChanged, this, [this](quint32 p, const QString& context) {
        sendMerged("contextChanged", p, {}, {paneValue(p), context});
    });
    // The value is gated when it goes out: a value that waits must not
    // leave after read access was taken away.
    connect(&control, &Control::userVarChanged, this, [this](quint32 p, const QString& name, const QString& value) {
        sendMerged("userVarChanged", p, name, {paneValue(p), name, value});
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
            // Its waiting overlay calls would still answer, and the overlays
            // hold the keyboard for clients that control no longer serves.
            m_api.cancelOverlays(QStringLiteral("Control.enabled is off"));
            // No overlay call can take them now: a client sends them again.
            m_api.dropInputs();
        }
    }
    if (m_merged.isEmpty()) return;
    if (m_settings.settings().control.eventInterval <= 0) {
        // Every later event goes at once: what waits must go before it.
        for (const Merged& m : std::as_const(m_merged)) {
            if (m.waiting) send(m.event, m.args);
        }
        m_merged.clear();
        m_due.stop();
        return;
    }
    // A shorter interval can make waiting events due sooner.
    scheduleDue();
}

void ControlSocket::send(const char* event, QJsonArray args)
{
    if (!m_enabled) return;
    if (std::strcmp(event, "userVarChanged") == 0) args[2] = readable(m_settings, args[2].toString());
    m_router.broadcastEvent(QLatin1String(Target), QLatin1String(event), args);
}

void ControlSocket::sendNow(const char* event, quint32 pane, const QJsonArray& args)
{
    // A client reads the pane's state in order: what waits for the pane goes
    // before an event that is never merged.
    const qint64 now = m_clock.elapsed();
    for (Merged& m : m_merged) {
        if (m.pane != pane || !m.waiting) continue;
        m.waiting = false;
        m.sentAt = now;
        send(m.event, m.args);
    }
    send(event, args);
}

void ControlSocket::sendMerged(const char* event, quint32 pane, const QString& name, const QJsonArray& args)
{
    if (!m_enabled) return;
    if (m_settings.settings().control.eventInterval <= 0) {
        // What still waits from before the interval became 0 goes first.
        sendNow(event, pane, args);
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

void ControlSocket::forgetPane(quint32 pane)
{
    for (auto it = m_merged.begin(); it != m_merged.end();) {
        if (it->pane == pane) {
            it = m_merged.erase(it);
        } else {
            ++it;
        }
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
