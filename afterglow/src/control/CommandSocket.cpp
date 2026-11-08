// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CommandSocket.h"

#include "Control.h"
#include "RemoteSockets.h"
#include "../config/Settings.h"

#include <AfterglowProto/Random.h>
#include <AfterglowProto/Remote.h>

#include <PhosphorIpc/IpcProtocol.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPointer>
#include <QRegularExpression>
// Private, as the renderer's QRhi is: only a metaobject built at run time
// gives each command an entry of its own in the schema. Qt6::CorePrivate is
// linked for it.
#include <QtCore/private/qmetaobjectbuilder_p.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>

Q_LOGGING_CATEGORY(lcCommands, "afterglow.commands", QtInfoMsg)

namespace Afterglow {

namespace {

// The client refuses what the terminal would, before it connects.
static_assert(ScriptHost::MaxCommandArgs == int(AfterglowProto::Run::MaxArgs)
              && ScriptHost::MaxCommandArgLength == int(AfterglowProto::Run::MaxArgLength));

/// The exit status of a run that its client cancelled, as for SIGINT.
constexpr int Cancelled = 130;
/// The UTF-16 units of one stdout or stderr event: a run's text in parts, so
/// that no event line comes near the client's frame limit.
constexpr qsizetype EventText = 1024 * 1024;

/// The protocol's rule for a run ID, which the client makes up. Not
/// validScriptId: the rules for script IDs are the scripts' own.
std::optional<QString> runIdError(const QString& id)
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    if (re.match(id).hasMatch()) return std::nullopt;
    return QStringLiteral("a run ID is 1 to 64 letters, digits, _ and -");
}

/// The target with an entry of its own for each command, "<script>.<command>"
/// taking the arguments, so that the schema lists the commands. The entries
/// are methods of a metaobject built at run time; a call of one comes to
/// qt_metacall, past the methods that moc made. The metaobject also declares
/// the events, ahead of the entries, as Qt wants a class's signals first.
class CommandTarget final : public CommandApi
{
public:
    using CommandApi::CommandApi;
    ~CommandTarget() override { std::free(m_meta); }

    void setCommands(const QList<ScriptCommand>& commands)
    {
        QStringList tokens;
        for (const ScriptCommand& c : commands) tokens << c.token();
        if (m_meta && tokens == m_tokens) return;
        QMetaObjectBuilder builder;
        builder.setClassName("Afterglow::CommandTarget");
        builder.setSuperClass(&CommandApi::staticMetaObject);
        for (const char* event : Events) {
            QMetaMethodBuilder signal = builder.addSignal(QByteArray(event) + "(QString,QString)");
            signal.setParameterNames({QByteArrayLiteral("run"), QByteArrayLiteral("text")});
            signal.setAccess(QMetaMethod::Public);
        }
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
        constexpr int events = int(std::size(Events));
        // An event: nothing emits one through Qt (IpcRouter::sendEvent).
        if (id < events) return -1;
        if (id - events < m_tokens.size()) {
            runEntry(m_tokens[id - events], *static_cast<const QStringList*>(argv[1]));
            return -1;
        }
        return id - events - int(m_tokens.size());
    }

private:
    static constexpr const char* Events[] = {"stdout", "stderr"};

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
        // A remote host sees only the commands that declare remote.
        if (m_socket.m_remote && !c.remote) continue;
        out.append(QVariantMap{{QStringLiteral("script"), c.script}, {QStringLiteral("command"), c.name},
                               {QStringLiteral("summary"), c.summary}, {QStringLiteral("usage"), c.usage},
                               {QStringLiteral("palette"), c.palette}, {QStringLiteral("stdin"), c.input},
                               {QStringLiteral("remote"), c.remote}});
    }
    return out;
}

void CommandApi::fail(const char* code, const QString& message)
{
    if (!code) {
        sendErrorReply(message);
        return;
    }
    // Deferred and failed at once: a pending reply carries a code.
    PhosphorIpc::IpcPendingReply reply = deferReply();
    if (reply.isValid()) reply.fail(QString::fromLatin1(code), message);
    else if (!connectionId()) sendErrorReply(message); // past the router's limit of waiting calls, the router fails it
}

bool CommandApi::input(const QString& run, const QString& chunk, bool eof)
{
    if (const auto refusal = m_socket.input(connectionId(), run, chunk, eof)) {
        fail(refusal->code, refusal->message);
        return false;
    }
    return true;
}

void CommandApi::run(const QString& run, const QString& script, const QString& command, const QStringList& args,
                     const QString& cwd, double pane)
{
    if (const auto error = runIdError(run)) {
        sendErrorReply(*error);
        return;
    }
    const quint64 connection = connectionId();
    if (const auto entry = m_socket.find(connection, run); entry && entry->started) {
        sendErrorReply(QStringLiteral("run ID %1 is in use on this connection").arg(run));
        return;
    } else if (entry && entry->hasInput && !entry->eof) {
        // A part of the input would look whole to the command. The input
        // stays: the client can send the rest and run again.
        sendErrorReply(QStringLiteral("the input of run %1 has not ended").arg(run));
        return;
    }
    QString error;
    const auto target = m_socket.pane(pane, error);
    if (!target) {
        sendErrorReply(error);
        return;
    }
    // The client's own directory: a relative one would name a directory of
    // the terminal's. A '/' and not QDir::isAbsolutePath, which takes a Qt
    // resource path (":/..."). On a remote host it is a directory there,
    // which the handler gets as text.
    if (cwd.contains(QChar::Null) || (!cwd.isEmpty() && !cwd.startsWith(QLatin1Char('/')))) {
        sendErrorReply(QStringLiteral("cwd must be empty or an absolute path without NUL"));
        return;
    }
    PhosphorIpc::IpcPendingReply reply = deferReply();
    if (!reply.isValid()) {
        // A call from no connection has nobody to answer later; past the
        // router's limit of waiting calls, the router fails it.
        if (!connection) sendErrorReply(QStringLiteral("a run needs a client connection"));
        return;
    }
    const auto& remote = m_socket.m_remote;
    m_socket.start(connection, run,
                   {script, command, args, cwd, *target, std::nullopt,
                    remote ? QStringLiteral("remote") : QStringLiteral("shell"), remote ? remote->host : QString()},
                   std::move(reply), true);
}

void CommandApi::ack(const QString& run, double bytes)
{
    if (const auto refusal = m_socket.ack(connectionId(), run, bytes)) fail(refusal->code, refusal->message);
}

void CommandApi::cancel(const QString& run)
{
    if (const auto refusal = m_socket.cancel(connectionId(), run)) fail(refusal->code, refusal->message);
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
                   {token.left(dot), token.mid(dot + 1), args, {}, *target, std::nullopt, QStringLiteral("control"), {}},
                   std::move(reply), false);
}

CommandSocket::CommandSocket(Control& control, ScriptHost& scripts, SettingsStore& settings, PhosphorIpc::IpcRouter& router,
                             std::optional<RemoteSession> remote, std::shared_ptr<CommandBudget> budget, QObject* parent)
    : QObject(parent)
    , m_control(control)
    , m_scripts(scripts)
    , m_settings(settings)
    , m_router(router)
    , m_remote(std::move(remote))
    , m_budget(budget ? std::move(budget) : std::make_shared<CommandBudget>())
    , m_api(std::make_unique<CommandTarget>(*this))
{
    auto* target = static_cast<CommandTarget*>(m_api.get());
    // The commands' own entries run with no client of their own. A remote
    // host has the calls of a run (list, input, run, ack and cancel), and
    // no SCRIPT.COMMAND entries.
    target->setCommands(m_remote ? QList<ScriptCommand>{} : scripts.commands());
    // Queued: set_commands can change the list inside a call of this target.
    if (!m_remote) {
        connect(&scripts, &ScriptHost::commandsChanged, this, [this, target] { target->setCommands(m_scripts.commands()); },
                Qt::QueuedConnection);
    }
    connect(&router, &PhosphorIpc::IpcRouter::connectionClosed, this, &CommandSocket::connectionClosed);
    connect(&settings, &SettingsStore::changed, this, &CommandSocket::applySettings);
    applySettings();
}

CommandSocket::~CommandSocket()
{
    m_router.unregisterTarget(target(), m_api.get());
    dropAll(QStringLiteral("afterglow is quitting"), false);
}

void CommandSocket::applySettings()
{
    const Settings& s = m_settings.settings();
    const QString remoteOff = m_remote ? remoteCommandsOff(s) : QString();
    const bool want = m_remote ? remoteOff.isEmpty() : s.control.enabled && s.scripts;
    if (want == m_enabled) return;
    if (want) {
        m_enabled = m_router.registerTarget(target(), m_api.get());
        if (!m_enabled) qCWarning(lcCommands) << "another object holds the" << target() << "target";
        return;
    }
    m_router.unregisterTarget(target(), m_api.get());
    m_enabled = false;
    dropAll(m_remote ? remoteOff + QStringLiteral(" in the running afterglow")
                     : QStringLiteral("commands are off in the running afterglow (Control.enabled or Scripts.enabled)"),
            true);
}

void CommandSocket::dropAll(const QString& message, bool cancel)
{
    const auto all = std::exchange(m_connections, {});
    std::deque<Output> output;
    for (const Connection& connection : all) {
        for (const auto& run : connection.runs) {
            // This socket's share of the budget, which other sockets keep using.
            releaseInput(*run);
            m_budget->outputBytes -= run->outputBytes;
            run->outputBytes = 0;
            run->reply.fail(message);
            if (!cancel) continue;
            std::move(run->output.begin(), run->output.end(), std::back_inserter(output));
            run->output.clear();
            if (run->handle && !run->exit) m_scripts.cancelCommand(run->handle, Cancelled);
        }
    }
    dropped(output);
}

void CommandSocket::connectionClosed(quint64 connection)
{
    // Nobody reads the output any more, and what waited for a run is dropped.
    const Connection books = m_connections.take(connection);
    std::deque<Output> output;
    for (const auto& run : books.runs) {
        releaseInput(*run);
        std::deque<Output> part = takeOutput(*run);
        std::move(part.begin(), part.end(), std::back_inserter(output));
    }
    for (const auto& run : books.runs) {
        if (run->handle && !run->exit) m_scripts.cancelCommand(run->handle, Cancelled);
    }
    dropped(output);
}

std::optional<CommandSocket::Refusal> CommandSocket::input(quint64 connection, const QString& id, const QString& chunk, bool eof)
{
    if (const auto error = runIdError(id)) return Refusal{nullptr, *error};
    const std::shared_ptr<Run> entry = find(connection, id);
    if (entry && entry->started) return Refusal{nullptr, QStringLiteral("run %1 has started: its input is complete").arg(id)};
    if (entry && entry->eof) return Refusal{nullptr, QStringLiteral("the input of run %1 has ended").arg(id)};
    if (!entry) {
        const Runs runs = m_connections.value(connection).runs;
        if (std::count_if(runs.cbegin(), runs.cend(), [](const auto& r) { return !r->started; }) >= MaxWaitingInputs)
            return Refusal{nullptr, QStringLiteral("%1 runs wait for their input already").arg(MaxWaitingInputs)};
    }
    const QByteArray bytes = chunk.toUtf8();
    const qint64 limit = m_scripts.commandLimits().inputBytes;
    if ((entry ? entry->input.size() : 0) + bytes.size() > limit) {
        // Refused, not cut: a command that got a part of its input could not tell.
        if (entry) remove(connection, id);
        return Refusal{InputTooLarge, QStringLiteral("Standard input is over %1 KiB (Scripts.commandStdinLimit)").arg(limit >> 10)};
    }
    // The input that waited stays: a cancel or a closed connection gives the room back.
    if (m_budget->inputBytes + bytes.size() > MaxWaitingInputBytes) {
        return Refusal{nullptr, QStringLiteral("afterglow keeps at most %1 MiB of input for runs that have not started")
                                    .arg(MaxWaitingInputBytes >> 20)};
    }
    Run& run = entry ? *entry : *m_connections[connection].runs.insert(id, std::make_shared<Run>()).value();
    run.input += bytes;
    m_budget->inputBytes += bytes.size();
    run.hasInput = true;
    run.eof = eof;
    return std::nullopt;
}

std::optional<CommandSocket::Refusal> CommandSocket::ack(quint64 connection, const QString& id, double bytes)
{
    const auto run = find(connection, id);
    if (!run || !run->started) return Refusal{nullptr, QStringLiteral("no run %1 on this connection").arg(id)};
    if (!std::isfinite(bytes) || bytes < 0) return Refusal{nullptr, QStringLiteral("ack takes a number of bytes")};
    // No more than the run sent: an ack cannot open the window for the others.
    const qint64 acknowledged = std::min(run->unacknowledged, qint64(std::min(bytes, 9.0e15)));
    run->unacknowledged -= acknowledged;
    m_connections[connection].unacknowledged -= acknowledged;
    pumpAll(connection);
    return std::nullopt;
}

std::optional<CommandSocket::Refusal> CommandSocket::cancel(quint64 connection, const QString& id)
{
    const auto run = find(connection, id);
    if (!run) return Refusal{nullptr, QStringLiteral("no run %1 on this connection").arg(id)};
    if (!run->started) {
        remove(connection, id); // only its input waited
        return std::nullopt;
    }
    // The client wants none of the output that waits.
    std::deque<Output> output = takeOutput(*run);
    // A run that ended answers with its status. Else the scripts end it,
    // and finish() answers with 130.
    if (run->exit) pump(connection, id);
    else m_scripts.cancelCommand(run->handle, Cancelled);
    dropped(output);
    return std::nullopt;
}

void CommandSocket::releaseInput(Run& run)
{
    if (run.started) return;
    m_budget->inputBytes -= run.input.size();
    run.input.clear();
}

std::deque<CommandSocket::Output> CommandSocket::takeOutput(Run& run)
{
    m_budget->outputBytes -= run.outputBytes;
    run.outputBytes = 0;
    return std::exchange(run.output, {});
}

void CommandSocket::dropped(std::deque<Output>& output)
{
    for (Output& out : output) {
        if (out.sent) out.sent(false);
    }
    output.clear();
}

void CommandSocket::remove(quint64 connection, const QString& id)
{
    const auto books = m_connections.find(connection);
    if (books == m_connections.end()) return;
    const std::shared_ptr<Run> run = books->runs.take(id);
    if (!run) return;
    releaseInput(*run);
    books->unacknowledged -= run->unacknowledged;
    run->unacknowledged = 0;
}

void CommandSocket::start(quint64 connection, const QString& id, ScriptHost::CommandRequest request,
                          PhosphorIpc::IpcPendingReply reply, bool credit)
{
    Runs& runs = m_connections[connection].runs;
    std::shared_ptr<Run> entry = runs.value(id);
    if (!entry) entry = runs.insert(id, std::make_shared<Run>()).value();
    // Out of the books first, and converted once: the script gets it as it came.
    const QByteArray input = std::exchange(entry->input, {});
    m_budget->inputBytes -= input.size();
    if (entry->hasInput) request.input = QString::fromUtf8(input);
    entry->started = true;
    entry->credit = credit;
    entry->reply = std::move(reply);
    // The runs can outlive this socket: the scripts end them as they go, after it.
    ScriptHost::CommandSink sink;
    const QPointer<CommandSocket> socket(this);
    sink.output = [socket, connection, id](const QString& text, std::function<void(bool)> sent) {
        return socket && socket->send(connection, "stdout", id, text, std::move(sent));
    };
    sink.error = [socket, connection, id](const QString& text, std::function<void(bool)> sent) {
        return socket && socket->send(connection, "stderr", id, text, std::move(sent));
    };
    sink.finished = [socket, connection, id](int status) {
        if (socket) socket->finish(connection, id, status);
    };
    QString error;
    bool unknown = false;
    const quint64 handle = m_scripts.startCommand(request, std::move(sink), &error, &unknown);
    if (!handle) {
        if (const auto run = find(connection, id)) run->reply.fail(unknown ? QString::fromLatin1(NoSuchCommand) : QString(), error);
        remove(connection, id);
        return;
    }
    // The run can have ended inside startCommand.
    if (const auto run = find(connection, id)) run->handle = handle;
}

bool CommandSocket::send(quint64 connection, const char* event, const QString& id, const QString& text,
                         std::function<void(bool)> sent)
{
    const auto run = find(connection, id);
    if (!run) return false;
    if (text.isEmpty()) {
        // Nothing to send: it has gone once the output before it has. That
        // output's own callback stays.
        if (run->output.empty()) {
            if (sent) sent(true);
        } else if (sent) {
            Output& last = run->output.back();
            last.sent = [before = std::move(last.sent), sent = std::move(sent)](bool delivered) {
                if (before) before(delivered);
                sent(delivered);
            };
        }
        return true;
    }
    std::vector<qsizetype> parts;
    for (qsizetype at = 0; at < text.size();) {
        qsizetype size = std::min(EventText, text.size() - at);
        if (at + size < text.size() && text[at + size - 1].isHighSurrogate()) --size;
        parts.push_back(size);
        at += size;
    }
    const qint64 bytes = qint64(parts.size()) * OutputPartBytes + text.size() * qint64(sizeof(QChar));
    if (m_budget->outputBytes + bytes > m_budget->maxOutputBytes) {
        // Before the reply, past the limit: the client learns why its run ends.
        const Output why{"stderr",
                         QStringLiteral("The output that waits for slow clients is over %1 MiB, afterglow's limit\n")
                             .arg(m_budget->maxOutputBytes >> 20),
                         {}};
        run->outputBytes += cost(why);
        m_budget->outputBytes += cost(why);
        run->output.push_back(why);
        return false;
    }
    qsizetype at = 0;
    for (const qsizetype size : parts) {
        run->output.push_back({event, text.mid(at, size), {}});
        at += size;
    }
    run->outputBytes += bytes;
    m_budget->outputBytes += bytes;
    run->output.back().sent = std::move(sent);
    // A client that has gone has its connectionClosed next, which ends the run.
    pump(connection, id);
    return true;
}

bool CommandSocket::pump(quint64 connection, const QString& id)
{
    const auto run = find(connection, id);
    if (!run) return false;
    // A callback can come back here: the books are looked up again each time.
    while (!run->output.empty() && (!run->credit || unacknowledged(connection) < AckWindow)) {
        Output out = std::move(run->output.front());
        run->output.pop_front();
        run->outputBytes -= cost(out);
        m_budget->outputBytes -= cost(out);
        const QJsonArray args{id, out.text};
        // The client counts the lines it reads: the same bytes.
        if (run->credit) {
            const qint64 line = eventLineBytes(target(), out.event, id, out.text);
            run->unacknowledged += line;
            if (const auto books = m_connections.find(connection); books != m_connections.end()) books->unacknowledged += line;
        }
        // False: the client has gone, or fell more than 16 MiB behind.
        if (!m_router.sendEvent(connection, target(), QLatin1String(out.event), args)) {
            std::deque<Output> rest = takeOutput(*run);
            if (out.sent) out.sent(false);
            dropped(rest);
            return false;
        }
        if (out.sent) out.sent(true);
    }
    if (run->output.empty() && run->exit && find(connection, id) == run) {
        const int status = *run->exit;
        const bool window = run->unacknowledged > 0;
        remove(connection, id);
        run->reply.finish(QJsonObject{{QStringLiteral("exit"), status}});
        // Its share of the window is free for the others.
        if (window) pumpAll(connection);
    }
    return true;
}

qint64 CommandSocket::unacknowledged(quint64 connection) const
{
    const auto books = m_connections.constFind(connection);
    return books == m_connections.cend() ? 0 : books->unacknowledged;
}

void CommandSocket::pumpAll(quint64 connection)
{
    const QStringList ids = m_connections.value(connection).runs.keys();
    for (const QString& id : ids) pump(connection, id);
}

void CommandSocket::finish(quint64 connection, const QString& id, int status)
{
    const auto run = find(connection, id);
    if (!run) return;
    // The reply goes after the output that waits for the client.
    run->exit = status;
    pump(connection, id);
}

QLatin1String CommandSocket::target() const
{
    return QLatin1String(m_remote ? AfterglowProto::Remote::Target : Target);
}

qint64 CommandSocket::waitingInputBytes() const
{
    return m_budget->inputBytes;
}

qint64 CommandSocket::waitingOutputBytes() const
{
    return m_budget->outputBytes;
}

void CommandSocket::setMaxWaitingOutputBytes(qint64 bytes)
{
    m_budget->maxOutputBytes = bytes;
}

qint64 CommandSocket::eventLineBytes(QLatin1String target, const char* event, const QString& id, const QString& text)
{
    // The frame around an empty text, and then the text as Qt's JSON writer
    // escapes it: \" \\ \b \f \n \r \t, \u00XX for the other C0 controls,
    // UTF-8 for the rest, and \uXXXX for a surrogate without its pair.
    // A test holds this to PhosphorIpc::writeLine for every UTF-16 unit.
    qint64 bytes = PhosphorIpc::writeLine(PhosphorIpc::buildEvent(target, QLatin1String(event), QJsonArray{id, QString()})).size();
    const qsizetype size = text.size();
    for (qsizetype i = 0; i < size; ++i) {
        const char16_t u = text[i].unicode();
        if (u < 0x80) {
            if (u == '"' || u == '\\' || u == '\b' || u == '\f' || u == '\n' || u == '\r' || u == '\t') bytes += 2;
            else bytes += u < 0x20 ? 6 : 1;
        } else if (u < 0x800) {
            bytes += 2;
        } else if (QChar::isHighSurrogate(u) && i + 1 < size && text[i + 1].isLowSurrogate()) {
            bytes += 4;
            ++i;
        } else {
            bytes += QChar::isSurrogate(u) ? 6 : 3;
        }
    }
    return bytes;
}

int CommandSocket::waitingInputs() const
{
    int count = 0;
    for (const Connection& c : m_connections)
        count += int(std::count_if(c.runs.cbegin(), c.runs.cend(), [](const auto& r) { return !r->started; }));
    return count;
}

std::shared_ptr<CommandSocket::Run> CommandSocket::find(quint64 connection, const QString& id) const
{
    const auto books = m_connections.constFind(connection);
    return books == m_connections.cend() ? nullptr : books->runs.value(id);
}

std::optional<quint32> CommandSocket::pane(double pane, QString& error) const
{
    if (!std::isfinite(pane) || pane != std::floor(pane) || pane < 0 || pane > double(std::numeric_limits<quint32>::max())) {
        error = QStringLiteral("a pane ID is a whole number from 0 to %1").arg(std::numeric_limits<quint32>::max());
        return std::nullopt;
    }
    // A remote host reaches the pane of its ssh session only.
    if (m_remote) {
        if (pane == 0 || quint32(pane) == m_remote->pane) return m_remote->pane;
        error = QStringLiteral("a run from a remote host acts on the pane of its ssh session, %1").arg(m_remote->pane);
        return std::nullopt;
    }
    if (pane == 0) return m_control.focusedPane().value_or(0);
    if (!m_control.hasPane(quint32(pane))) {
        error = QStringLiteral("no pane %1").arg(quint32(pane));
        return std::nullopt;
    }
    return quint32(pane);
}

} // namespace Afterglow
