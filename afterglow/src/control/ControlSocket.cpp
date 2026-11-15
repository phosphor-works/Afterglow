// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ControlSocket.h"

#include "RemoteSockets.h"
#include "../config/Settings.h"

#include <QBuffer>
#include <QImage>
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
    // One at a time: a screenshot waits for its frame anyway, and the cap
    // of MaxScreenshots bounds the queue.
    m_encoder.setMaxThreadCount(1);
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

ControlApi::PendingReply ControlApi::deferCall(const QString& noClient)
{
    auto reply = std::make_shared<PhosphorIpc::IpcPendingReply>(deferReply());
    if (reply->isValid()) return reply;
    // A call from no connection has nobody to answer later; past the
    // router's limit of waiting calls, the router fails it.
    if (!connectionId()) failed(noClient);
    return nullptr;
}

std::optional<quint32> ControlApi::targetPane(PhosphorIpc::IpcPendingReply& reply, quint32 id)
{
    if (id != 0) return id;
    if (const auto focused = m_control.focusedPane()) return *focused;
    reply.fail(QStringLiteral("no_pane"), QStringLiteral("no focused pane"));
    return std::nullopt;
}

void ControlApi::screenshot(double pane, const QString& scope)
{
    const auto id = paneId(pane);
    if (!id) return;
    const auto captureScope = Control::captureScopeFromName(scope);
    if (!captureScope) {
        failed(QStringLiteral("scope is pane, tab or window"));
        return;
    }
    const PendingReply reply = deferCall(QStringLiteral("a screenshot needs a client connection"));
    if (!reply) return;
    // From here on a failure has a code of its own, which only the pending
    // reply sends. The permission goes first: it holds for every pane.
    QString code;
    if (const QString refused = m_control.captureRefusal(code); !refused.isEmpty()) {
        reply->fail(code, refused);
        return;
    }
    if (m_screenshots >= MaxScreenshots) {
        reply->fail(QStringLiteral("busy"), QStringLiteral("%1 screenshots are in progress").arg(MaxScreenshots));
        return;
    }
    const auto target = targetPane(*reply, *id);
    if (!target) return;
    ++m_screenshots;
    const QString error = m_control.capture(*target, *captureScope,
        [reply, api = QPointer<ControlApi>(this)](const QString& error, const QString& code, const QImage& image) {
            if (!api) {
                reply->fail(QStringLiteral("failed"), QStringLiteral("the control socket closed"));
                return;
            }
            if (!error.isEmpty()) {
                --api->m_screenshots;
                reply->fail(code, error);
                return;
            }
            // A 4K PNG takes a moment to encode: not on the thread that draws
            // the windows. The answer comes back to this object's thread,
            // the router's.
            ControlApi* self = api.data();
            // The reply moves on to the posted call: the pool keeps no copy,
            // so it ends on the router's thread, as IpcPendingReply wants.
            self->m_encoder.start([reply, image, self]() mutable {
                QByteArray png;
                QBuffer buffer(&png);
                buffer.open(QIODevice::WriteOnly);
                const QString base64 = image.save(&buffer, "PNG") ? QString::fromLatin1(png.toBase64()) : QString();
                buffer.close();
                png = {};
                QMetaObject::invokeMethod(self, [reply = std::move(reply), base64, size = image.size(), self] {
                    self->finishScreenshot(*reply, base64, size);
                });
            });
        },
        code);
    if (!error.isEmpty()) {
        --m_screenshots;
        reply->fail(code, error);
    }
}

void ControlApi::finishScreenshot(PhosphorIpc::IpcPendingReply& reply, const QString& base64, const QSize& size)
{
    --m_screenshots;
    // Checked again as the picture goes out: read access, or control
    // itself, can go off while the frame is drawn and encoded.
    QString code;
    if (const QString refused = m_control.captureRefusal(code); !refused.isEmpty()) {
        reply.fail(code, refused);
    } else if (!m_settings.settings().control.enabled) {
        reply.fail(QStringLiteral("cancelled"), QStringLiteral("Control.enabled is off"));
    } else if (base64.isEmpty()) {
        reply.fail(QStringLiteral("failed"), QStringLiteral("the PNG could not be written"));
    } else {
        reply.finish(QJsonObject{{QStringLiteral("png"), base64},
                                 {QStringLiteral("width"), size.width()},
                                 {QStringLiteral("height"), size.height()}});
    }
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

void ControlApi::openProject(const QString& name, bool newInstance, const QString& mode)
{
    // A session waits for the mux daemon off the GUI thread: the call is
    // answered then. Taken first: a call that cannot wait opens nothing.
    const PendingReply reply = deferCall(QStringLiteral("opening a project needs a client connection"));
    if (!reply) return;
    m_control.openProject(name, newInstance, mode, [reply](const QString& error, const QVariantMap& result) {
        if (error.isEmpty()) reply->finish(QJsonObject::fromVariantMap(result));
        else reply->fail(error);
    });
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
                                         double protocol, const QStringList& ssh, const QString& destination, bool overlay)
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
    // The pane is the one that the caller and the process it names run in:
    // a process of another terminal that names the pane gets no socket that
    // runs remote commands for it.
    const auto runsIn = [&](qint64 process) {
        return m_paneProcesses ? m_paneProcesses(*id, process, overlay) : m_control.runsIn(*id, process, overlay);
    };
    if (!runsIn(connectionPid()) || !runsIn(qint64(std::clamp(pid, -1.0, 1e12)))) {
        failed(QStringLiteral("only `afterglow ssh` in pane %1 opens a remote socket for it").arg(*id));
        return {};
    }
    RemoteSockets::Opened opened;
    if (failed(m_remote->open({*id, qint64(std::clamp(pid, -1.0, 1e12)), host, directory, int(protocol), ssh, destination, overlay},
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
    if (!OverlayInputs::validName(input)) return !failed(QStringLiteral("an input name is 1 to 64 letters, digits, _ and -"));
    const quint64 connection = connectionId();
    if (!connection) return !failed(QStringLiteral("an input needs a client connection"));
    return !failed(m_inputs.add(QString::number(connection), input, chunk, eof));
}

void ControlApi::overlay(double pane, const QStringList& argv, const QVariantMap& given)
{
    QString error;
    // Standard input that came in parts: the call takes it, whatever it
    // decides, also for a pane ID that it refuses.
    QVariantMap options = given;
    if (failed(m_inputs.resolve(QString::number(connectionId()), options))) return;
    const auto id = paneId(pane);
    if (!id) return;
    auto spec = OverlaySpec::read(argv, options, &error);
    // A client names a directory of this machine: one that is missing is
    // its mistake, where a script's remote path starts in $HOME.
    if (spec) error = Control::localDirectory(spec->cwd);
    if (failed(error)) return;
    const quint64 connection = connectionId();
    const PendingReply reply = deferCall(QStringLiteral("an overlay needs a client connection"));
    if (!reply) return;
    // From here on a failure has a code of its own, which only the pending
    // reply sends.
    const auto target = targetPane(*reply, *id);
    if (!target) return;
    QString code;
    error = m_control.overlay(overlayOwner(connection), {}, *target, *spec,
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
}

void ControlApi::connectionClosed(quint64 connection)
{
    m_inputs.drop(QString::number(connection));
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
            // Those of the clients of sessions, which the windows answer too.
            m_control.endSessionClients(QStringLiteral("Control.enabled is off"));
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
