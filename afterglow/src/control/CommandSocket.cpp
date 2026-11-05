// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CommandSocket.h"

#include "Control.h"
#include "../config/Settings.h"
#include "../scripting/ScriptFiles.h"

#include <AfterglowProto/Random.h>

#include <PhosphorIpc/IpcProtocol.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QtCore/private/qmetaobjectbuilder_p.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

Q_LOGGING_CATEGORY(lcCommands, "afterglow.commands", QtInfoMsg)

namespace Afterglow {

namespace {

/// The exit status of a run that its client cancelled, as for SIGINT.
constexpr int Cancelled = 130;
/// The UTF-16 units of one stdout or stderr event: a run's text in parts, so
/// that no event line comes near the client's frame limit.
constexpr qsizetype EventText = 1024 * 1024;

bool validRun(const QString& id)
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    return re.match(id).hasMatch();
}

/// The target with an entry of its own for each command, "<script>.<command>"
/// taking the arguments, so that the schema lists the commands. The entries
/// are methods of a metaobject built at run time; a call of one comes to
/// qt_metacall, past the methods that moc made.
class CommandTarget final : public CommandApi
{
public:
    using CommandApi::CommandApi;
    ~CommandTarget() override { std::free(m_meta); }

    void setCommands(const QList<ScriptCommand>& commands)
    {
        QStringList tokens;
        for (const ScriptCommand& c : commands) tokens << c.token();
        if (tokens == m_tokens) return;
        QMetaObjectBuilder builder;
        builder.setClassName("Afterglow::CommandTarget");
        builder.setSuperClass(&CommandApi::staticMetaObject);
        for (const QString& token : std::as_const(tokens)) {
            QMetaMethodBuilder method = builder.addMethod(token.toUtf8() + "(QStringList)");
            method.setParameterNames({QByteArrayLiteral("args")});
            method.setAccess(QMetaMethod::Public);
        }
        // Built from the event loop (a queued connection), never inside a
        // call that the old metaobject runs.
        std::free(std::exchange(m_meta, builder.toMetaObject()));
        m_tokens = tokens;
    }

    const QMetaObject* metaObject() const override { return m_meta ? m_meta : &CommandApi::staticMetaObject; }

    int qt_metacall(QMetaObject::Call call, int id, void** argv) override
    {
        id = CommandApi::qt_metacall(call, id, argv);
        if (id < 0 || call != QMetaObject::InvokeMetaMethod) return id;
        if (id < m_tokens.size()) {
            runEntry(m_tokens[id], *static_cast<const QStringList*>(argv[1]));
            return -1;
        }
        return id - int(m_tokens.size());
    }

private:
    QMetaObject* m_meta = nullptr;
    QStringList m_tokens;
};

} // namespace

CommandApi::CommandApi(CommandSocket& socket)
    : m_socket(socket)
{
}

QVariantList CommandApi::list()
{
    QVariantList out;
    for (const ScriptCommand& c : m_socket.m_scripts.commands()) {
        out.append(QVariantMap{{QStringLiteral("script"), c.script}, {QStringLiteral("command"), c.name},
                               {QStringLiteral("summary"), c.summary}, {QStringLiteral("usage"), c.usage},
                               {QStringLiteral("palette"), c.palette}, {QStringLiteral("stdin"), c.input}});
    }
    return out;
}

bool CommandApi::input(const QString& run, const QString& chunk, bool eof)
{
    if (!validRun(run)) {
        sendErrorReply(QStringLiteral("a run ID is 1 to 64 letters, digits, _ and -"));
        return false;
    }
    auto& runs = m_socket.m_runs[connectionId()];
    std::shared_ptr<CommandSocket::Run> entry = runs.value(run);
    if (entry && entry->started) {
        sendErrorReply(QStringLiteral("run %1 has started: its input is complete").arg(run));
        return false;
    }
    if (!entry) {
        const auto waiting = std::count_if(runs.cbegin(), runs.cend(), [](const auto& r) { return !r->started; });
        if (waiting >= CommandSocket::MaxWaitingInputs) {
            sendErrorReply(QStringLiteral("%1 runs wait for their input already").arg(CommandSocket::MaxWaitingInputs));
            return false;
        }
        entry = std::make_shared<CommandSocket::Run>();
        runs.insert(run, entry);
    }
    if (entry->eof) {
        sendErrorReply(QStringLiteral("the input of run %1 has ended").arg(run));
        return false;
    }
    // Cut at the limit, on a whole character: what is past it is dropped.
    const qint64 limit = m_socket.m_scripts.commandLimits().inputBytes;
    const QByteArray bytes = chunk.toUtf8();
    const QByteArray kept = utf8Prefix(bytes, limit - entry->input.size());
    if (m_socket.m_waitingInputBytes + kept.size() > CommandSocket::MaxWaitingInputBytes) {
        sendErrorReply(QStringLiteral("afterglow keeps at most %1 MiB of input for runs that have not started")
                           .arg(CommandSocket::MaxWaitingInputBytes >> 20));
        return false;
    }
    entry->input += kept;
    m_socket.m_waitingInputBytes += kept.size();
    entry->hasInput = true;
    entry->eof = eof || kept.size() < bytes.size() || entry->input.size() >= limit;
    return !entry->eof;
}

void CommandApi::run(const QString& run, const QString& script, const QString& command, const QStringList& args,
                     const QString& cwd, double pane)
{
    if (!validRun(run)) {
        sendErrorReply(QStringLiteral("a run ID is 1 to 64 letters, digits, _ and -"));
        return;
    }
    const quint64 connection = connectionId();
    if (const auto entry = m_socket.find(connection, run); entry && entry->started) {
        sendErrorReply(QStringLiteral("run ID %1 is in use on this connection").arg(run));
        return;
    }
    QString error;
    const auto target = m_socket.pane(pane, error);
    if (!target) {
        sendErrorReply(error);
        return;
    }
    if (cwd.contains(QChar::Null)) {
        sendErrorReply(QStringLiteral("cwd must be a path without NUL"));
        return;
    }
    PhosphorIpc::IpcPendingReply reply = deferReply();
    if (!reply.isValid()) {
        // A call from no connection has nobody to answer later; past the
        // router's limit of waiting calls, the router fails it.
        if (!connection) sendErrorReply(QStringLiteral("a run needs a client connection"));
        return;
    }
    m_socket.start(connection, run, {script, command, args, cwd, *target, std::nullopt, QStringLiteral("shell")}, std::move(reply),
                   true);
}

void CommandApi::ack(const QString& run, double bytes)
{
    const quint64 connection = connectionId();
    const auto entry = m_socket.find(connection, run);
    if (!entry || !entry->started) {
        sendErrorReply(QStringLiteral("no run %1 on this connection").arg(run));
        return;
    }
    if (!std::isfinite(bytes) || bytes < 0) {
        sendErrorReply(QStringLiteral("ack takes a number of bytes"));
        return;
    }
    entry->unacknowledged = std::max<qint64>(0, entry->unacknowledged - qint64(std::min(bytes, 9.0e15)));
    m_socket.pump(connection, run);
}

void CommandApi::cancel(const QString& run)
{
    const quint64 connection = connectionId();
    const auto entry = m_socket.find(connection, run);
    if (!entry) {
        sendErrorReply(QStringLiteral("no run %1 on this connection").arg(run));
        return;
    }
    if (!entry->started) {
        m_socket.releaseInput(*entry);
        m_socket.m_runs[connection].remove(run); // only its input waited
        return;
    }
    if (entry->exit) {
        // The run ended and its output waits: the client wants none of it.
        entry->output.clear();
        m_socket.pump(connection, run);
        return;
    }
    m_socket.m_scripts.cancelCommand(entry->handle, Cancelled);
}

void CommandApi::runEntry(const QString& token, const QStringList& args)
{
    const auto id = AfterglowProto::randomHex(16);
    QString error;
    const auto target = m_socket.pane(0, error);
    if (!id || !target) {
        sendErrorReply(id ? error : QStringLiteral("no random bytes for a run ID"));
        return;
    }
    PhosphorIpc::IpcPendingReply reply = deferReply();
    if (!reply.isValid()) {
        if (!connectionId()) sendErrorReply(QStringLiteral("a run needs a client connection"));
        return;
    }
    const auto dot = token.indexOf(QLatin1Char('.'));
    // A client of the schema entry (phosphorctl) does not acknowledge.
    m_socket.start(connectionId(), QString::fromStdString(*id),
                   {token.left(dot), token.mid(dot + 1), args, {}, *target, std::nullopt, QStringLiteral("control")},
                   std::move(reply), false);
}

CommandSocket::CommandSocket(Control& control, ScriptHost& scripts, SettingsStore& settings, PhosphorIpc::IpcRouter& router,
                             QObject* parent)
    : QObject(parent)
    , m_control(control)
    , m_scripts(scripts)
    , m_settings(settings)
    , m_router(router)
    , m_api(std::make_unique<CommandTarget>(*this))
{
    auto* target = static_cast<CommandTarget*>(m_api.get());
    target->setCommands(scripts.commands());
    // Queued: set_commands can change the list inside a call of this target.
    connect(&scripts, &ScriptHost::commandsChanged, this, [this, target] { target->setCommands(m_scripts.commands()); },
            Qt::QueuedConnection);
    connect(&router, &PhosphorIpc::IpcRouter::connectionClosed, this, &CommandSocket::connectionClosed);
    connect(&settings, &SettingsStore::changed, this, &CommandSocket::applySettings);
    applySettings();
}

CommandSocket::~CommandSocket()
{
    m_router.unregisterTarget(QLatin1String(Target), m_api.get());
    dropAll(QStringLiteral("afterglow is quitting"));
}

void CommandSocket::applySettings()
{
    const Settings& s = m_settings.settings();
    const bool want = s.control.enabled && s.scripts;
    if (want == m_enabled) return;
    if (want) {
        m_enabled = m_router.registerTarget(QLatin1String(Target), m_api.get());
        if (!m_enabled) qCWarning(lcCommands) << "another object holds the commands target";
        return;
    }
    m_router.unregisterTarget(QLatin1String(Target), m_api.get());
    m_enabled = false;
    dropAll(QStringLiteral("commands are off in the running afterglow (Control.enabled or Scripts.enabled)"));
}

void CommandSocket::dropAll(const QString& message)
{
    const auto all = std::exchange(m_runs, {});
    m_waitingInputBytes = 0;
    for (const Runs& runs : all) {
        for (const auto& run : runs) {
            run->reply.fail(message);
            if (run->handle) m_scripts.cancelCommand(run->handle, Cancelled);
        }
    }
}

void CommandSocket::connectionClosed(quint64 connection)
{
    // Nobody reads the output any more, and what waited for a run is dropped.
    const Runs runs = m_runs.take(connection);
    for (const auto& run : runs) {
        releaseInput(*run);
        if (run->handle) m_scripts.cancelCommand(run->handle, Cancelled);
    }
}

void CommandSocket::releaseInput(Run& run)
{
    if (run.started) return;
    m_waitingInputBytes -= run.input.size();
    run.input.clear();
}

void CommandSocket::start(quint64 connection, const QString& id, ScriptHost::CommandRequest request,
                          PhosphorIpc::IpcPendingReply reply, bool credit)
{
    Runs& runs = m_runs[connection];
    std::shared_ptr<Run> entry = runs.value(id);
    if (!entry) entry = runs.insert(id, std::make_shared<Run>()).value();
    if (entry->hasInput) request.input = QString::fromUtf8(entry->input);
    releaseInput(*entry);
    entry->started = true;
    entry->credit = credit;
    entry->reply = std::move(reply);
    ScriptHost::CommandSink sink;
    sink.output = [this, connection, id](const QString& text, std::function<void()> sent) {
        return send(connection, "stdout", id, text, std::move(sent));
    };
    sink.error = [this, connection, id](const QString& text, std::function<void()> sent) {
        return send(connection, "stderr", id, text, std::move(sent));
    };
    sink.finished = [this, connection, id](int status) { finish(connection, id, status); };
    QString error;
    bool unknown = false;
    const quint64 handle = m_scripts.startCommand(request, std::move(sink), &error, &unknown);
    if (!handle) {
        if (const auto run = find(connection, id)) run->reply.fail(unknown ? QString::fromLatin1(NoSuchCommand) : QString(), error);
        m_runs[connection].remove(id);
        return;
    }
    // The run can have ended inside startCommand.
    if (const auto run = find(connection, id)) run->handle = handle;
}

bool CommandSocket::send(quint64 connection, const char* event, const QString& id, const QString& text, std::function<void()> sent)
{
    const auto run = find(connection, id);
    if (!run) return false;
    for (qsizetype at = 0; at < text.size();) {
        qsizetype size = std::min(EventText, text.size() - at);
        if (at + size < text.size() && text[at + size - 1].isHighSurrogate()) --size;
        run->output.push_back({event, text.mid(at, size), {}});
        at += size;
    }
    if (run->output.empty()) {
        if (sent) sent();
        return true;
    }
    run->output.back().sent = std::move(sent);
    return pump(connection, id);
}

bool CommandSocket::pump(quint64 connection, const QString& id)
{
    const auto run = find(connection, id);
    if (!run) return false;
    while (!run->output.empty() && (!run->credit || run->unacknowledged < AckWindow)) {
        Output out = std::move(run->output.front());
        run->output.pop_front();
        const QJsonArray args{id, out.text};
        // The client counts the lines it reads: the same bytes.
        if (run->credit) {
            run->unacknowledged += PhosphorIpc::writeLine(PhosphorIpc::buildEvent(QLatin1String(Target), QLatin1String(out.event), args)).size();
        }
        // False: the client has gone, or fell more than 16 MiB behind.
        if (!m_router.sendEvent(connection, QLatin1String(Target), QLatin1String(out.event), args)) {
            run->output.clear();
            return false;
        }
        if (out.sent) out.sent();
    }
    if (run->output.empty() && run->exit) {
        const int status = *run->exit;
        m_runs[connection].remove(id);
        run->reply.finish(QJsonObject{{QStringLiteral("exit"), status}});
    }
    return true;
}

void CommandSocket::finish(quint64 connection, const QString& id, int status)
{
    const auto run = find(connection, id);
    if (!run) return;
    // The reply goes after the output that waits for the client.
    run->exit = status;
    pump(connection, id);
}

int CommandSocket::waitingInputs() const
{
    int count = 0;
    for (const Runs& runs : m_runs) count += int(std::count_if(runs.cbegin(), runs.cend(), [](const auto& r) { return !r->started; }));
    return count;
}

std::shared_ptr<CommandSocket::Run> CommandSocket::find(quint64 connection, const QString& id) const
{
    return m_runs.value(connection).value(id);
}

std::optional<quint32> CommandSocket::pane(double pane, QString& error) const
{
    if (!std::isfinite(pane) || pane != std::floor(pane) || pane < 0 || pane > double(std::numeric_limits<quint32>::max())) {
        error = QStringLiteral("a pane ID is a whole number from 0 to %1").arg(std::numeric_limits<quint32>::max());
        return std::nullopt;
    }
    if (pane == 0) return m_control.focusedPane().value_or(0);
    if (!m_control.paneInfo(quint32(pane))) {
        error = QStringLiteral("no pane %1").arg(quint32(pane));
        return std::nullopt;
    }
    return quint32(pane);
}

} // namespace Afterglow
