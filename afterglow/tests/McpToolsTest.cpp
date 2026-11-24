// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// afterglow mcp with no daemon: the tool definitions (phosphor-mcp takes each
// schema, the list is the same at each call, and each tool says what it does
// to the session in all four hints), the helpers of the handlers, the
// framing of DaemonLink over a socket pair, and the handlers against a fake
// daemon that answers each call as a test says. The tools run against a real
// daemon in mcp_session.py.

#include "mcp/DaemonLink.h"
#include "mcp/McpToolParts.h"
#include "mcp/McpTools.h"

#include <AfterglowProto/IpcServer.h>
#include <PhosphorMcp/McpServer.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <memory>
#include <utility>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace Afterglow;
using namespace Afterglow::McpParts;

namespace {

QByteArray listJson(const QList<PhosphorMcp::McpTool>& tools)
{
    QJsonArray list;
    for (const PhosphorMcp::McpTool& tool : tools)
        list.append(tool.toJson());
    return QJsonDocument(list).toJson(QJsonDocument::Compact);
}

/// The application of the tests that run an event loop.
void ensureApplication()
{
    static int argc = 1;
    static char name[] = "afterglow_mcp_tests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication* app = QCoreApplication::instance() ? nullptr : new QCoreApplication(argc, argv);
    (void)app;
}

/// Runs the event loop until @p done, or for @p ms at most. @p each runs
/// at each turn.
bool pumpUntil(const std::function<bool()>& done, const std::function<void()>& each = {}, int ms = 5000)
{
    const QDeadlineTimer deadline(ms);
    while (true) {
        if (each)
            each();
        if (done())
            return true;
        if (deadline.hasExpired())
            return false;
        QEventLoop loop;
        QTimer::singleShot(2, &loop, &QEventLoop::quit);
        loop.exec();
    }
}

/// Reads the lines that wait on @p fd, without waiting.
QList<QJsonObject> readLines(int fd, QByteArray& buffer)
{
    char chunk[65536];
    while (true) {
        const ssize_t got = ::recv(fd, chunk, sizeof chunk, MSG_DONTWAIT);
        if (got <= 0)
            break;
        buffer.append(chunk, qsizetype(got));
    }
    QList<QJsonObject> lines;
    qsizetype end = 0;
    while ((end = buffer.indexOf('\n')) >= 0) {
        lines.append(QJsonDocument::fromJson(buffer.left(end)).object());
        buffer.remove(0, end + 1);
    }
    return lines;
}

void writeLine(int fd, const QJsonObject& object)
{
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    ASSERT_EQ(::send(fd, line.constData(), std::size_t(line.size()), MSG_NOSIGNAL), ssize_t(line.size()));
}

QJsonObject reply(const QJsonValue& result)
{
    return {{QStringLiteral("result"), result}};
}

QJsonObject error(const char* code, const QString& message)
{
    return {{QStringLiteral("code"), QLatin1String(code)}, {QStringLiteral("message"), message}};
}

/// The daemon's end of a socket pair: it answers each call with what
/// answer() gives, a reply ({"result": ...}) or an error ({"code", "message"}),
/// and takes each subscription.
struct FakeDaemon
{
    struct Call
    {
        QString fn;
        QJsonArray args;
    };
    int fd = -1;
    QByteArray buffer;
    QList<Call> calls;
    QHash<QString, qint64> subscriptions;
    std::function<QJsonObject(const QString& fn, const QJsonArray& args)> answer;

    void serve()
    {
        for (const QJsonObject& line : readLines(fd, buffer)) {
            const qint64 id = line.value(QLatin1String("id")).toInteger();
            if (line.value(QLatin1String("type")).toString() == QLatin1String("subscribe")) {
                subscriptions.insert(line.value(QLatin1String("signal")).toString(), id);
                writeLine(fd,
                          {{QStringLiteral("type"), QStringLiteral("reply")},
                           {QStringLiteral("id"), id},
                           {QStringLiteral("result"), QJsonValue()}});
                continue;
            }
            const QString fn = line.value(QLatin1String("fn")).toString();
            const QJsonArray args = line.value(QLatin1String("args")).toArray();
            calls.append({fn, args});
            const QJsonObject answered = answer(fn, args);
            if (answered.contains(QLatin1String("code"))) {
                QJsonObject out = answered;
                out.insert(QStringLiteral("type"), QStringLiteral("error"));
                out.insert(QStringLiteral("id"), id);
                writeLine(fd, out);
            } else {
                writeLine(fd,
                          {{QStringLiteral("type"), QStringLiteral("reply")},
                           {QStringLiteral("id"), id},
                           {QStringLiteral("result"), answered.value(QLatin1String("result"))}});
            }
        }
    }

    void event(const QString& signal, const QJsonArray& args)
    {
        writeLine(fd,
                  {{QStringLiteral("type"), QStringLiteral("event")},
                   {QStringLiteral("subscriptionId"), subscriptions.value(signal)},
                   {QStringLiteral("target"), QStringLiteral("control")},
                   {QStringLiteral("signal"), signal},
                   {QStringLiteral("args"), args}});
    }

    QList<Call> callsOf(const QString& fn) const
    {
        QList<Call> found;
        for (const Call& call : calls) {
            if (call.fn == fn)
                found.append(call);
        }
        return found;
    }
};

QJsonObject paneFacts(qint64 id, const QString& shell, bool input = false)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("tab"), 0},
            {QStringLiteral("shell"), shell},
            {QStringLiteral("input"), input},
            {QStringLiteral("last_exit"), QJsonValue()}};
}

QJsonObject region(qint64 id, const char* kind, bool open = false, const QJsonValue& exit = QJsonValue())
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("kind"), QLatin1String(kind)},
            {QStringLiteral("open"), open},
            {QStringLiteral("exit_code"), exit}};
}

/// McpTools between a fake daemon and an MCP client, each on a socket pair.
class Harness
{
public:
    struct Options
    {
        quint32 startingPane = 0;
        int eventInterval = 20;
        QString projects;
        /// A new connection to the daemon: with it, the session ends with
        /// the server (McpTools::Session::endsWithServer).
        std::function<int(QString*, bool*)> connect;
        /// The link and the fake daemon's end of it, instead of a socket pair.
        int linkFd = -1;
        int daemonFd = -1;
    };

    explicit Harness(quint32 startingPane = 0, int eventInterval = 20)
        : Harness(Options{startingPane, eventInterval, {}, {}})
    {
    }

    explicit Harness(const Options& options)
    {
        const quint32 startingPane = options.startingPane;
        const int eventInterval = options.eventInterval;
        ensureApplication();
        int link[2] = {options.linkFd, options.daemonFd};
        int mcp[2];
        if (link[0] < 0) {
            EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, link), 0);
        }
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, mcp), 0);
        daemon.fd = link[1];
        m_client = mcp[1];
        m_serverFd = mcp[0];
        m_settings.control.eventInterval = eventInterval;
        m_link = std::make_unique<DaemonLink>(link[0]);
        m_tools = std::make_unique<McpTools>(*m_link, m_settings,
                                             McpTools::Session{QStringLiteral("agent"), QStringLiteral("/tmp"),
                                                               std::nullopt, options.projects, options.connect,
                                                               startingPane, bool(options.connect)});
        m_server = std::make_unique<PhosphorMcp::McpServer>();
        QString problem;
        EXPECT_TRUE(m_tools->addTo(*m_server, &problem)) << problem.toStdString();
        EXPECT_TRUE(m_server->start(m_serverFd, m_serverFd));
        for (const QString& event : McpTools::events())
            m_link->subscribe(event, [](const QJsonValue&, const DaemonLink::Error*) { });
        EXPECT_TRUE(pumpUntil(
            [this] {
                return daemon.subscriptions.size() == McpTools::events().size();
            },
            [this] {
                daemon.serve();
            }));
    }

    ~Harness()
    {
        m_server.reset();
        m_tools.reset();
        m_link.reset();
        ::close(m_serverFd);
        ::close(m_client);
        ::close(daemon.fd);
    }

    qint64 start(const QString& tool, const QJsonObject& arguments)
    {
        const qint64 id = ++m_nextId;
        writeLine(m_client,
                  {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                   {QStringLiteral("id"), id},
                   {QStringLiteral("method"), QStringLiteral("tools/call")},
                   {QStringLiteral("params"),
                    QJsonObject{
                        {QStringLiteral("name"), tool},
                        {QStringLiteral("arguments"), arguments},
                        {QStringLiteral("_meta"),
                         QJsonObject{
                             {QStringLiteral("io.modelcontextprotocol/protocolVersion"), QStringLiteral("2026-07-28")},
                             {QStringLiteral("io.modelcontextprotocol/clientCapabilities"), QJsonObject()}}}}}});
        return id;
    }

    bool answered(qint64 id)
    {
        for (const QJsonObject& line : readLines(m_client, m_buffer)) {
            if (line.contains(QLatin1String("id")))
                m_replies.insert(line.value(QLatin1String("id")).toInteger(), line);
        }
        return m_replies.contains(id);
    }

    /// The result of call @p id, after the loop ran until it came.
    QJsonObject result(qint64 id, int ms = 5000)
    {
        const bool came = pumpUntil(
            [this, id] {
                return answered(id);
            },
            [this] {
                daemon.serve();
            },
            ms);
        EXPECT_TRUE(came) << "no answer to call " << id;
        return m_replies.value(id).value(QLatin1String("result")).toObject();
    }

    QJsonObject call(const QString& tool, const QJsonObject& arguments)
    {
        return result(start(tool, arguments));
    }

    /// Runs the loop for @p ms, with the daemon answering.
    void run(int ms)
    {
        pumpUntil(
            [] {
                return false;
            },
            [this] {
                daemon.serve();
            },
            ms);
    }

    FakeDaemon daemon;

private:
    Settings m_settings;
    int m_client = -1;
    int m_serverFd = -1;
    QByteArray m_buffer;
    QHash<qint64, QJsonObject> m_replies;
    qint64 m_nextId = 0;
    std::unique_ptr<DaemonLink> m_link;
    std::unique_ptr<McpTools> m_tools;
    std::unique_ptr<PhosphorMcp::McpServer> m_server;
};

QString textOf(const QJsonObject& result)
{
    return result.value(QLatin1String("content")).toArray().at(0).toObject().value(QLatin1String("text")).toString();
}

QJsonObject structured(const QJsonObject& result)
{
    return result.value(QLatin1String("structuredContent")).toObject();
}

} // namespace

TEST(McpTools, EachSchemaPassesTheChecksOfTheServer)
{
    PhosphorMcp::McpServer server;
    for (const PhosphorMcp::McpTool& tool : McpTools::definitions()) {
        QString error;
        EXPECT_TRUE(server.addTool(
            tool, [](PhosphorMcp::McpCall) { }, &error))
            << tool.name().toStdString() << ": " << error.toStdString();
    }
    EXPECT_TRUE(listJson(server.tools()) == listJson(McpTools::definitions()));
}

TEST(McpTools, TheListIsTheSameEachTime)
{
    const QList<PhosphorMcp::McpTool> first = McpTools::definitions();
    QStringList names;
    for (const PhosphorMcp::McpTool& tool : first)
        names << tool.name();
    EXPECT_EQ(names,
              (QStringList{QStringLiteral("list_panes"), QStringLiteral("open_pane"), QStringLiteral("run"),
                           QStringLiteral("wait"), QStringLiteral("send"), QStringLiteral("read"),
                           QStringLiteral("screenshot"), QStringLiteral("close_pane"), QStringLiteral("open_project"),
                           QStringLiteral("save_project"), QStringLiteral("list_script_commands"),
                           QStringLiteral("run_script_command")}));
    EXPECT_TRUE(listJson(first) == listJson(McpTools::definitions()));
}

// MCP gives a hint that a tool leaves out its least safe value: a client
// would ask before each new pane as before a destructive call.
TEST(McpTools, EachToolSetsTheFourHints)
{
    struct Hints
    {
        bool readOnly;
        bool destructive;
        bool idempotent;
    };
    const std::map<QString, Hints> expected{
        {QStringLiteral("list_panes"), {true, false, true}},
        {QStringLiteral("open_pane"), {false, false, false}},
        {QStringLiteral("run"), {false, true, false}},
        {QStringLiteral("wait"), {true, false, false}},
        {QStringLiteral("send"), {false, true, false}},
        {QStringLiteral("read"), {true, false, true}},
        {QStringLiteral("screenshot"), {true, false, true}},
        {QStringLiteral("close_pane"), {false, true, false}},
        // A project adds panes, and a save can replace a saved project.
        {QStringLiteral("open_project"), {false, false, false}},
        {QStringLiteral("save_project"), {false, true, true}},
        {QStringLiteral("list_script_commands"), {true, false, true}},
        // A script command can do all that its script can.
        {QStringLiteral("run_script_command"), {false, true, false}},
    };
    for (const PhosphorMcp::McpTool& tool : McpTools::definitions()) {
        SCOPED_TRACE(tool.name().toStdString());
        ASSERT_TRUE(tool.readOnlyHint() && tool.destructiveHint() && tool.idempotentHint() && tool.openWorldHint());
        const auto it = expected.find(tool.name());
        ASSERT_NE(it, expected.end());
        EXPECT_EQ(*tool.readOnlyHint(), it->second.readOnly);
        EXPECT_EQ(*tool.destructiveHint(), it->second.destructive);
        EXPECT_EQ(*tool.idempotentHint(), it->second.idempotent);
        // Each acts only on the server's own session.
        EXPECT_FALSE(*tool.openWorldHint());
        const QJsonObject annotations = tool.toJson().value(QLatin1String("annotations")).toObject();
        EXPECT_EQ(annotations.size(), 4);
    }
}

// The pane is required wherever a tool acts on one: never the focused pane.
TEST(McpTools, EveryToolOfAPaneRequiresIt)
{
    for (const PhosphorMcp::McpTool& tool : McpTools::definitions()) {
        const QJsonObject schema = tool.inputSchema();
        if (!schema.value(QLatin1String("properties")).toObject().contains(QLatin1String("pane")))
            continue;
        const bool required =
            schema.value(QLatin1String("required")).toArray().contains(QJsonValue(QStringLiteral("pane")));
        // open_pane takes a pane only for a split, which its handler checks.
        EXPECT_EQ(required, tool.name() != QLatin1String("open_pane")) << tool.name().toStdString();
    }
}

// The tools of projects and script commands take no pane where they act on
// the session as a whole, and their names have the limits of the files and
// the scripts that they name.
TEST(McpTools, TheWorkflowToolsTakeNamesWithinTheirLimits)
{
    std::map<QString, QJsonObject> tools;
    for (const PhosphorMcp::McpTool& tool : McpTools::definitions())
        tools[tool.name()] = tool.inputSchema();
    for (const char* name : {"open_project", "save_project"}) {
        const QJsonObject schema = tools.at(QLatin1String(name));
        EXPECT_EQ(schema.value(QLatin1String("required")).toArray(), QJsonArray{QStringLiteral("name")}) << name;
        const QJsonObject project =
            schema.value(QLatin1String("properties")).toObject().value(QLatin1String("name")).toObject();
        EXPECT_EQ(project.value(QLatin1String("maxLength")).toInt(), 64) << name;
    }
    EXPECT_TRUE(
        tools.at(QStringLiteral("list_script_commands")).value(QLatin1String("properties")).toObject().isEmpty());
    const QJsonArray required =
        tools.at(QStringLiteral("run_script_command")).value(QLatin1String("required")).toArray();
    EXPECT_EQ(required, (QJsonArray{QStringLiteral("pane"), QStringLiteral("script"), QStringLiteral("command")}));
}

// The helpers of the handlers.

TEST(McpParts, ShapeKeepsTheLastLines)
{
    const auto lines = [](const char* text, int count) {
        const Output out = shape(QString::fromUtf8(text), count, 1 << 20, false);
        return std::pair{out.text.toStdString(), out.truncated};
    };
    EXPECT_EQ(lines("a\nb\nc", 2), (std::pair{std::string("b\nc"), true}));
    EXPECT_EQ(lines("a\nb\nc", 3), (std::pair{std::string("a\nb\nc"), false}));
    EXPECT_EQ(lines("a\nb\nc", 9), (std::pair{std::string("a\nb\nc"), false}));
    // A line end at the very end ends the last line, and starts none.
    EXPECT_EQ(lines("a\nb\n", 1), (std::pair{std::string("b\n"), true}));
    EXPECT_EQ(lines("a\nb\n", 2), (std::pair{std::string("a\nb\n"), false}));
    // An empty first line counts as one.
    EXPECT_EQ(lines("\nabc", 1), (std::pair{std::string("abc"), true}));
    EXPECT_EQ(lines("\nabc", 2), (std::pair{std::string("\nabc"), false}));
    EXPECT_EQ(lines("", 1), (std::pair{std::string(), false}));
    EXPECT_EQ(lines("\n", 1), (std::pair{std::string("\n"), false}));
    EXPECT_EQ(lines("\n\n", 1), (std::pair{std::string("\n"), true}));
}

TEST(McpParts, ShapeCutsBytesAtACharacter)
{
    // "é" is two bytes: a cut inside it drops it whole.
    const Output out = shape(QString::fromUtf8("x\xc3\xa9yz"), 0, 3, false);
    EXPECT_EQ(out.text.toStdString(), "yz");
    EXPECT_TRUE(out.truncated);
    const Output whole = shape(QStringLiteral("abc"), 0, 3, false);
    EXPECT_EQ(whole.text.toStdString(), "abc");
    EXPECT_FALSE(whole.truncated);
    EXPECT_TRUE(shape(QStringLiteral("abc"), 0, 3, true).truncated);
}

TEST(McpParts, KeepEndNeverSplitsASurrogatePair)
{
    // U+1F600 is a surrogate pair in UTF-16.
    QString text = QStringLiteral("ab") + QString::fromUcs4(U"\U0001F600") + QStringLiteral("cd");
    keepEnd(text, 3);
    EXPECT_EQ(text, QStringLiteral("cd"));
    QString pair = QStringLiteral("ab") + QString::fromUcs4(U"\U0001F600") + QStringLiteral("cd");
    keepEnd(pair, 4);
    EXPECT_EQ(pair, QString::fromUcs4(U"\U0001F600") + QStringLiteral("cd"));
    QString short_ = QStringLiteral("ab");
    keepEnd(short_, 4);
    EXPECT_EQ(short_, QStringLiteral("ab"));
}

TEST(McpParts, ACommandEndsAtAPromptAfterItsOutput)
{
    const QJsonArray none;
    EXPECT_FALSE(commandEnded(none, false));
    // One line: a prompt ends it, with output or without (an empty line).
    EXPECT_TRUE(commandEnded({region(2, "output"), region(3, "prompt")}, false));
    EXPECT_TRUE(commandEnded({region(3, "prompt")}, false));
    EXPECT_FALSE(commandEnded({region(2, "output", true)}, false));
    // Several lines: a prompt that bash redraws before the output does not.
    EXPECT_FALSE(commandEnded({region(3, "prompt")}, true));
    EXPECT_FALSE(commandEnded({region(2, "prompt"), region(3, "output", true)}, true));
    EXPECT_TRUE(commandEnded({region(2, "prompt"), region(3, "output"), region(4, "prompt")}, true));
    // An output after the last prompt: the next line runs.
    EXPECT_FALSE(commandEnded({region(2, "output"), region(3, "prompt"), region(4, "output", true)}, true));
}

TEST(McpParts, ExitCodesAreThoseOfTheClosedOutputs)
{
    const QJsonArray regions{region(1, "prompt"), region(2, "output", false, 0), region(3, "prompt"),
                             region(4, "output", false, 4), region(5, "output", true)};
    EXPECT_EQ(exitCodes(regions), (QJsonArray{0, 4}));
    EXPECT_TRUE(exitCodes({}).isEmpty());
    // An output that the next prompt ended without a D has no code.
    EXPECT_EQ(exitCodes({region(2, "output")}), (QJsonArray{QJsonValue()}));
    EXPECT_TRUE(hasOutput(regions));
    EXPECT_FALSE(hasOutput({region(1, "prompt")}));
}

TEST(McpParts, ARunningCommandIsTheNewestOutput)
{
    EXPECT_EQ(newestRegion({region(3, "prompt"), region(7, "output", true)}), 7);
    EXPECT_EQ(runningSince({region(3, "prompt"), region(7, "output", true)}), 6);
    // It ended while the shell said that it runs: its output still counts.
    EXPECT_EQ(runningSince({region(3, "prompt"), region(7, "output"), region(8, "prompt")}), 6);
    EXPECT_EQ(runningSince({region(3, "prompt")}), 3);
    EXPECT_EQ(runningSince({}), 0);
}

// The version of serverInfo changes when a tool or the instructions change,
// and only then: a client that keys its cache on it reads the new list.
TEST(McpParts, TheServerVersionFollowsTheToolsAndTheInstructions)
{
    const QList<PhosphorMcp::McpTool> tools = McpTools::definitions();
    const QString version = serverVersion(QStringLiteral("0.1.0"), tools, QStringLiteral("text"));
    EXPECT_TRUE(QRegularExpression(QStringLiteral("^0\\.1\\.0\\+[0-9a-f]{8}$")).match(version).hasMatch())
        << version.toStdString();
    EXPECT_EQ(serverVersion(QStringLiteral("0.1.0"), McpTools::definitions(), QStringLiteral("text")), version);
    EXPECT_NE(serverVersion(QStringLiteral("0.1.0"), tools, QStringLiteral("other text")), version);
    QList<PhosphorMcp::McpTool> changed = tools;
    changed.first().setDescription(changed.first().description() + QStringLiteral(" More."));
    EXPECT_NE(serverVersion(QStringLiteral("0.1.0"), changed, QStringLiteral("text")), version);
    EXPECT_NE(serverVersion(QStringLiteral("0.1.0"), tools.mid(1), QStringLiteral("text")), version);
}

TEST(McpParts, SessionNamesKeepTheCharactersOfANameAndTheirLimit)
{
    EXPECT_EQ(sessionName(QStringLiteral("/home/u/my project.v2"), QStringLiteral("0a1b2c3d")),
              QStringLiteral("mcp-my_project_v2-0a1b2c3d"));
    EXPECT_EQ(sessionName(QStringLiteral("/"), QStringLiteral("0a1b2c3d")), QStringLiteral("mcp-0a1b2c3d"));
    const QString name =
        sessionName(QStringLiteral("/x/") + QString(200, QLatin1Char('d')), QStringLiteral("0a1b2c3d"));
    EXPECT_EQ(name.size(), qsizetype(AfterglowMux::MaxSessionName));
    EXPECT_TRUE(name.endsWith(QStringLiteral("-0a1b2c3d")));
    EXPECT_TRUE(AfterglowMux::validSessionName(name.toStdString()));
}

TEST(McpParts, ExitStatusIsThatOfAglowRun)
{
    EXPECT_EQ(exitStatus(QJsonValue(0)), 0);
    EXPECT_EQ(exitStatus(QJsonValue(255)), 255);
    EXPECT_EQ(exitStatus(QJsonValue(256)), 1);
    EXPECT_EQ(exitStatus(QJsonValue(-1)), 1);
    EXPECT_EQ(exitStatus(QJsonValue()), 1);
}

TEST(McpParts, OnlyAPreparedShellMarksPrompts)
{
    EXPECT_FALSE(marksPrompts(std::nullopt));
    EXPECT_TRUE(marksPrompts(QString()));
    EXPECT_TRUE(marksPrompts(QStringLiteral("no-cursor")));
    EXPECT_FALSE(marksPrompts(QStringLiteral("no-cursor no-prompt-mark")));
    const QJsonArray environment{QJsonArray{QStringLiteral("A"), QStringLiteral("1")},
                                 QJsonArray{QStringLiteral("AFTERGLOW_SHELL_FEATURES"), QStringLiteral("no-rc")}};
    EXPECT_TRUE(variableIn(environment, QStringLiteral("AFTERGLOW_SHELL_FEATURES")) == QStringLiteral("no-rc"));
    EXPECT_FALSE(variableIn(environment, QStringLiteral("B")).has_value());
}

namespace {

QJsonObject pane(const char* id)
{
    return {{QStringLiteral("pane"), QJsonObject{{QStringLiteral("id"), QLatin1String(id)}}}};
}

QJsonObject split(const char* direction, const QJsonObject& first, const QJsonObject& second)
{
    return {{QStringLiteral("direction"), QLatin1String(direction)},
            {QStringLiteral("ratio"), 0.5},
            {QStringLiteral("first"), first},
            {QStringLiteral("second"), second}};
}

} // namespace

// a | (b / d): b splits a, and d splits b, each in the room of its side.
TEST(McpParts, ASplitTreeOpensSplitBySplit)
{
    const QJsonObject tree = split("right", pane("a"), split("down", pane("b"), pane("d")));
    EXPECT_EQ(leaves(tree), 3);
    QList<ProjectStep> steps;
    qsizetype next = 0;
    planSplits(tree, 0, next, steps);
    EXPECT_EQ(next, 3);
    EXPECT_EQ(steps, (QList<ProjectStep>{{1, 0, QStringLiteral("right")}, {2, 1, QStringLiteral("down")}}));
    // (a / b) | (c / d): the right side splits its own first pane.
    const QJsonObject four = split("right", split("down", pane("a"), pane("b")), split("down", pane("c"), pane("d")));
    steps.clear();
    next = 0;
    planSplits(four, 0, next, steps);
    EXPECT_EQ(steps,
              (QList<ProjectStep>{{2, 0, QStringLiteral("right")},
                                  {1, 0, QStringLiteral("down")},
                                  {3, 2, QStringLiteral("down")}}));
}

TEST(McpParts, ALayoutTreeNestsInPreorder)
{
    const std::vector<AfterglowMux::LayoutNode> tree{{0, false, 0.25},
                                                     {7, false, 0.5},
                                                     {0, true, 0.5},
                                                     {8, false, 0.5},
                                                     {9, false, 0.5}};
    std::size_t at = 0;
    const QJsonObject nestedTree = nested(tree, at, [](std::uint32_t id) {
        return QJsonObject{{QStringLiteral("id"), QString::number(id)}};
    });
    EXPECT_EQ(at, tree.size());
    const auto id = [](const QJsonValue& node) {
        return node.toObject().value(QLatin1String("pane")).toObject().value(QLatin1String("id")).toString();
    };
    EXPECT_EQ(nestedTree.value(QLatin1String("direction")).toString(), QStringLiteral("right"));
    EXPECT_EQ(nestedTree.value(QLatin1String("ratio")).toDouble(), 0.25);
    EXPECT_EQ(id(nestedTree.value(QLatin1String("first"))), QStringLiteral("7"));
    const QJsonObject second = nestedTree.value(QLatin1String("second")).toObject();
    EXPECT_EQ(second.value(QLatin1String("direction")).toString(), QStringLiteral("down"));
    EXPECT_EQ(id(second.value(QLatin1String("first"))), QStringLiteral("8"));
    EXPECT_EQ(id(second.value(QLatin1String("second"))), QStringLiteral("9"));
}

// DaemonLink over a socket pair.

namespace {

struct LinkPair
{
    LinkPair()
    {
        ensureApplication();
        int fds[2];
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
        link = std::make_unique<DaemonLink>(fds[0]);
        peer = fds[1];
    }
    ~LinkPair()
    {
        link.reset();
        if (peer >= 0)
            ::close(peer);
    }
    QList<QJsonObject> lines()
    {
        return readLines(peer, buffer);
    }
    std::unique_ptr<DaemonLink> link;
    int peer = -1;
    QByteArray buffer;
};

} // namespace

TEST(DaemonLink, ACallIsOneLineAndItsReplyComesToItsAnswer)
{
    LinkPair pair;
    std::optional<QJsonValue> got;
    pair.link->call(QStringLiteral("listPanes"), {}, [&](const QJsonValue& value, const DaemonLink::Error* error) {
        EXPECT_EQ(error, nullptr);
        got = value;
    });
    QList<QJsonObject> sent;
    ASSERT_TRUE(pumpUntil([&] {
        sent += pair.lines();
        return !sent.isEmpty();
    }));
    ASSERT_EQ(sent.size(), 1);
    EXPECT_EQ(sent[0].value(QLatin1String("type")).toString(), QStringLiteral("call"));
    EXPECT_EQ(sent[0].value(QLatin1String("target")).toString(), QStringLiteral("control"));
    EXPECT_EQ(sent[0].value(QLatin1String("fn")).toString(), QStringLiteral("listPanes"));
    const qint64 id = sent[0].value(QLatin1String("id")).toInteger();
    // Two answers in one write, the first of no call: the second still comes.
    const QByteArray both = QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("reply")},
                                                      {QStringLiteral("id"), id + 100},
                                                      {QStringLiteral("result"), 1}})
                                .toJson(QJsonDocument::Compact)
        + '\n'
        + QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("reply")},
                                    {QStringLiteral("id"), id},
                                    {QStringLiteral("result"), QJsonArray{5}}})
              .toJson(QJsonDocument::Compact)
        + '\n';
    ASSERT_EQ(::send(pair.peer, both.constData(), std::size_t(both.size()), MSG_NOSIGNAL), ssize_t(both.size()));
    ASSERT_TRUE(pumpUntil([&] {
        return got.has_value();
    }));
    EXPECT_EQ(*got, QJsonValue(QJsonArray{5}));
    EXPECT_TRUE(pair.link->isOpen());
}

TEST(DaemonLink, AnErrorWithNoIdEndsTheConnectionAndEveryCall)
{
    LinkPair pair;
    std::optional<DaemonLink::Error> failed;
    std::optional<QString> closed;
    QObject::connect(pair.link.get(), &DaemonLink::closed, [&](const QString& reason) {
        closed = reason;
    });
    pair.link->call(QStringLiteral("listPanes"), {}, [&](const QJsonValue&, const DaemonLink::Error* error) {
        ASSERT_NE(error, nullptr);
        failed = *error;
    });
    writeLine(pair.peer,
              {{QStringLiteral("type"), QStringLiteral("error")},
               {QStringLiteral("code"), QStringLiteral("CALL_FAILED")},
               {QStringLiteral("message"), QStringLiteral("going away")}});
    ASSERT_TRUE(pumpUntil([&] {
        return failed && closed;
    }));
    EXPECT_TRUE(failed->code.isEmpty());
    EXPECT_EQ(failed->message, QStringLiteral("going away"));
    EXPECT_FALSE(pair.link->isOpen());
}

// A slot of an event that ends the connection (a write to a peer that
// closed) clears the subscriptions: the name that the slot got stays its own.
TEST(DaemonLink, AnEventSlotThatEndsTheConnectionKeepsItsName)
{
    LinkPair pair;
    pair.link->subscribe(QStringLiteral("textChanged"), [](const QJsonValue&, const DaemonLink::Error*) { });
    QList<QJsonObject> sent;
    ASSERT_TRUE(pumpUntil([&] {
        sent += pair.lines();
        return !sent.isEmpty();
    }));
    const qint64 id = sent[0].value(QLatin1String("id")).toInteger();
    std::optional<QString> heard;
    QObject::connect(pair.link.get(), &DaemonLink::event, [&](const QString& name, const QJsonArray&) {
        pair.link->call(QStringLiteral("mark"), {1}, [](const QJsonValue&, const DaemonLink::Error*) { });
        heard = name;
    });
    writeLine(pair.peer,
              {{QStringLiteral("type"), QStringLiteral("event")},
               {QStringLiteral("subscriptionId"), id},
               {QStringLiteral("signal"), QStringLiteral("textChanged")},
               {QStringLiteral("args"), QJsonArray{1}}});
    ::close(pair.peer);
    pair.peer = -1;
    ASSERT_TRUE(pumpUntil([&] {
        return heard.has_value() && !pair.link->isOpen();
    }));
    EXPECT_EQ(*heard, QStringLiteral("textChanged"));
}

// A daemon that reads nothing: the calls past MaxQueuedBytes fail, and the
// connection stays.
TEST(DaemonLink, TheQueueOfADaemonThatReadsNothingHasALimit)
{
    LinkPair pair;
    const QString big(1 << 19, QLatin1Char('x'));
    std::optional<DaemonLink::Error> refused;
    for (int i = 0; i < 2 * int(DaemonLink::MaxQueuedBytes >> 19) && !refused; ++i) {
        pair.link->call(QStringLiteral("sendText"), {1, big}, [&](const QJsonValue&, const DaemonLink::Error* error) {
            if (error && !refused)
                refused = *error;
        });
        QCoreApplication::processEvents();
    }
    ASSERT_TRUE(pumpUntil([&] {
        return refused.has_value();
    }));
    EXPECT_EQ(refused->code, QLatin1String(DaemonLink::QueueFull));
    EXPECT_TRUE(pair.link->isOpen());
}

// The handlers against a fake daemon.

namespace {

/// A daemon whose pane 1 is at an empty prompt, and whose run ends when
/// regions() says so.
QJsonObject promptDaemon(const QString& fn, const QJsonArray& args, const std::function<QJsonObject()>& regions)
{
    if (fn == QLatin1String("listPanes"))
        return reply(QJsonArray{paneFacts(1, QStringLiteral("prompt"))});
    if (fn == QLatin1String("mark"))
        return reply(rowMark(10, 1));
    if (fn == QLatin1String("shellRegions"))
        return args.size() == 1 ? reply(QJsonArray{region(5, "prompt")}) : regions();
    if (fn == QLatin1String("paste"))
        return reply(QJsonValue());
    if (fn == QLatin1String("getText"))
        return reply(QJsonObject{{QStringLiteral("text"), QStringLiteral("$ true\n$ ")},
                                 {QStringLiteral("truncated"), false},
                                 {QStringLiteral("trimmed"), false}});
    return error("NO_SUCH_FN", fn);
}

const QJsonArray Ended{region(6, "output", false, 0), region(7, "prompt")};

} // namespace

// A pane that decodes an image while a run waits: the check goes again, and
// never tells the model to run the command again.
TEST(McpToolsRun, TryAgainAfterThePasteChecksAgain)
{
    Harness h;
    int checks = 0;
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        return promptDaemon(fn, args, [&] {
            return ++checks == 1 ? error(AfterglowMux::Error::TryAgain, QStringLiteral("pane 1 decodes an image"))
                                 : reply(Ended);
        });
    };
    const QJsonObject result = h.call(
        QStringLiteral("run"), {{QStringLiteral("pane"), 1}, {QStringLiteral("command"), QStringLiteral("true")}});
    EXPECT_FALSE(result.value(QLatin1String("isError")).toBool()) << textOf(result).toStdString();
    EXPECT_EQ(structured(result).value(QLatin1String("status")).toString(), QStringLiteral("finished"));
    EXPECT_EQ(structured(result).value(QLatin1String("exit_code")).toInt(-1), 0);
    EXPECT_GE(checks, 2);
    // One paste, at an empty prompt and with its Enter, and no keys.
    const auto pastes = h.daemon.callsOf(QStringLiteral("paste"));
    ASSERT_EQ(pastes.size(), 1);
    EXPECT_EQ(pastes[0].args.at(2).toObject(),
              (QJsonObject{{QStringLiteral("prompt"), true}, {QStringLiteral("enter"), true}}));
    EXPECT_TRUE(h.daemon.callsOf(QStringLiteral("sendKeys")).isEmpty());
    // The output since the mark of the run.
    const auto texts = h.daemon.callsOf(QStringLiteral("getText"));
    ASSERT_FALSE(texts.isEmpty());
    EXPECT_EQ(texts.last().args.at(2).toObject().value(QLatin1String("mark")).toObject(), rowMark(10, 1));
}

TEST(McpToolsRun, APasteThatThePaneRefusedSendsNothingMore)
{
    Harness h;
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        if (fn == QLatin1String("paste"))
            return error(AfterglowMux::Error::NotAtPrompt, QStringLiteral("someone typed at the prompt of pane 1"));
        return promptDaemon(fn, args, [] {
            return reply(Ended);
        });
    };
    const QJsonObject result = h.call(
        QStringLiteral("run"), {{QStringLiteral("pane"), 1}, {QStringLiteral("command"), QStringLiteral("true")}});
    EXPECT_TRUE(result.value(QLatin1String("isError")).toBool());
    EXPECT_TRUE(textOf(result).contains(QLatin1String("did not go"))) << textOf(result).toStdString();
    EXPECT_FALSE(textOf(result).contains(QLatin1String("again")));
    EXPECT_EQ(h.daemon.callsOf(QStringLiteral("paste")).size(), 1);
}

// A reflow since the mark: the output of the command's region.
TEST(McpToolsRun, AStaleMarkReadsTheLastOutput)
{
    Harness h;
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        if (fn == QLatin1String("getText") && args.at(1).toString() == QLatin1String("since"))
            return error(AfterglowMux::Error::StaleMark,
                         QStringLiteral("the mark is from before the pane changed its width"));
        if (fn == QLatin1String("getText"))
            return reply(QJsonObject{{QStringLiteral("text"), QStringLiteral("the output")},
                                     {QStringLiteral("truncated"), false}});
        return promptDaemon(fn, args, [] {
            return reply(Ended);
        });
    };
    const QJsonObject result = h.call(
        QStringLiteral("run"), {{QStringLiteral("pane"), 1}, {QStringLiteral("command"), QStringLiteral("true")}});
    ASSERT_FALSE(result.value(QLatin1String("isError")).toBool()) << textOf(result).toStdString();
    EXPECT_EQ(structured(result).value(QLatin1String("output")).toString(), QStringLiteral("the output"));
    EXPECT_TRUE(structured(result).value(QLatin1String("truncated")).toBool());
    // In the structured result too, for a client that reads only that.
    const QJsonArray notes = structured(result).value(QLatin1String("notes")).toArray();
    ASSERT_EQ(notes.size(), 1);
    EXPECT_TRUE(notes.at(0).toString().contains(QLatin1String("changed its width")));
    EXPECT_EQ(h.daemon.callsOf(QStringLiteral("getText")).last().args.at(1).toString(), QStringLiteral("last-output"));
}

// The first pane of a new session starts its shell: run waits for its first
// prompt. Another pane with no prompt has no shell integration.
TEST(McpToolsRun, AStartingShellGetsItsFirstPrompt)
{
    Harness h(1);
    bool prompted = false;
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        if (fn == QLatin1String("listPanes")) {
            return reply(QJsonArray{paneFacts(1, prompted ? QStringLiteral("prompt") : QStringLiteral("none")),
                                    paneFacts(2, QStringLiteral("none"))});
        }
        return promptDaemon(fn, args, [] {
            return reply(Ended);
        });
    };
    const QJsonObject plain = h.call(
        QStringLiteral("run"), {{QStringLiteral("pane"), 2}, {QStringLiteral("command"), QStringLiteral("true")}});
    EXPECT_TRUE(textOf(plain).contains(QLatin1String("no shell integration"))) << textOf(plain).toStdString();
    const qint64 id = h.start(QStringLiteral("run"),
                              {{QStringLiteral("pane"), 1}, {QStringLiteral("command"), QStringLiteral("true")}});
    h.run(100);
    EXPECT_TRUE(h.daemon.callsOf(QStringLiteral("paste")).isEmpty());
    prompted = true;
    h.daemon.event(QStringLiteral("shellChanged"), {1, QStringLiteral("prompt")});
    const QJsonObject result = h.result(id);
    EXPECT_FALSE(result.value(QLatin1String("isError")).toBool()) << textOf(result).toStdString();
    EXPECT_EQ(structured(result).value(QLatin1String("status")).toString(), QStringLiteral("finished"));
}

TEST(McpToolsRun, AShellThatShowsNoPromptFailsAtTheTimeout)
{
    Harness h(1);
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        if (fn == QLatin1String("listPanes"))
            return reply(QJsonArray{paneFacts(1, QStringLiteral("none"))});
        return promptDaemon(fn, args, [] {
            return reply(Ended);
        });
    };
    const QJsonObject result = h.call(QStringLiteral("run"),
                                      {{QStringLiteral("pane"), 1},
                                       {QStringLiteral("command"), QStringLiteral("true")},
                                       {QStringLiteral("timeout_ms"), 200}});
    EXPECT_TRUE(textOf(result).contains(QLatin1String("showed no prompt in 200 ms"))) << textOf(result).toStdString();
    EXPECT_TRUE(h.daemon.callsOf(QStringLiteral("paste")).isEmpty());
    // The next call waits for no first prompt again.
    QElapsedTimer clock;
    clock.start();
    const QJsonObject again = h.call(QStringLiteral("run"),
                                     {{QStringLiteral("pane"), 1},
                                      {QStringLiteral("command"), QStringLiteral("true")},
                                      {QStringLiteral("timeout_ms"), 5000}});
    EXPECT_TRUE(textOf(again).contains(QLatin1String("no shell integration"))) << textOf(again).toStdString();
    EXPECT_LT(clock.elapsed(), 2000);
}

// A first prompt that comes when the time of the run is up sends no command.
TEST(McpToolsRun, AFirstPromptAtTheTimeoutSendsNoCommand)
{
    Harness h(1);
    bool prompted = false;
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        if (fn == QLatin1String("listPanes"))
            return reply(QJsonArray{paneFacts(1, prompted ? QStringLiteral("prompt") : QStringLiteral("none"))});
        // The mark comes when the time is up.
        if (fn == QLatin1String("mark") && prompted)
            QThread::msleep(400);
        return promptDaemon(fn, args, [] {
            return reply(Ended);
        });
    };
    const qint64 id = h.start(QStringLiteral("run"),
                              {{QStringLiteral("pane"), 1},
                               {QStringLiteral("command"), QStringLiteral("true")},
                               {QStringLiteral("timeout_ms"), 300}});
    h.run(100);
    prompted = true;
    h.daemon.event(QStringLiteral("shellChanged"), {1, QStringLiteral("prompt")});
    const QJsonObject result = h.result(id);
    EXPECT_TRUE(result.value(QLatin1String("isError")).toBool());
    EXPECT_TRUE(textOf(result).contains(QLatin1String("the command did not go"))) << textOf(result).toStdString();
    EXPECT_TRUE(h.daemon.callsOf(QStringLiteral("paste")).isEmpty());
}

// A pane that decodes an image takes the keys a moment later: the text that
// went before them goes once.
TEST(McpToolsSend, TryAgainSendsOnlyThePartThatDidNotGo)
{
    Harness h;
    int keys = 0;
    h.daemon.answer = [&](const QString& fn, const QJsonArray&) {
        if (fn == QLatin1String("sendKeys") && ++keys == 1)
            return error(AfterglowMux::Error::TryAgain, QStringLiteral("pane 1 decodes an image"));
        return reply(QJsonValue());
    };
    const QJsonObject result = h.call(QStringLiteral("send"),
                                      {{QStringLiteral("pane"), 1},
                                       {QStringLiteral("text"), QStringLiteral("abc")},
                                       {QStringLiteral("keys"), QJsonArray{QStringLiteral("enter")}}});
    EXPECT_FALSE(result.value(QLatin1String("isError")).toBool()) << textOf(result).toStdString();
    EXPECT_EQ(h.daemon.callsOf(QStringLiteral("sendText")).size(), 1);
    EXPECT_EQ(h.daemon.callsOf(QStringLiteral("sendKeys")).size(), 2);
}

TEST(McpToolsSend, AFailureAfterAPartSaysWhatWent)
{
    Harness h;
    h.daemon.answer = [&](const QString& fn, const QJsonArray&) {
        if (fn == QLatin1String("sendKeys"))
            return error("CALL_FAILED", QStringLiteral("program closed or input queue full"));
        return reply(QJsonValue());
    };
    const QJsonObject result = h.call(QStringLiteral("send"),
                                      {{QStringLiteral("pane"), 1},
                                       {QStringLiteral("text"), QStringLiteral("abc")},
                                       {QStringLiteral("keys"), QJsonArray{QStringLiteral("enter")}}});
    EXPECT_TRUE(result.value(QLatin1String("isError")).toBool());
    EXPECT_TRUE(textOf(result).contains(QLatin1String("took the text, but not the keys")))
        << textOf(result).toStdString();
}

// The daemon decides whether the pane is the last one, in the step of the
// close.
TEST(McpToolsClose, TheDaemonRefusesTheLastPane)
{
    Harness h;
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        if (fn == QLatin1String("close") && !args.at(1).toObject().value(QLatin1String("endSession")).toBool())
            return error(AfterglowMux::Error::LastPane, QStringLiteral("pane 1 is the last pane of session 'agent'"));
        return reply(QJsonObject{{QStringLiteral("sessionEnded"), true}});
    };
    const QJsonObject refused = h.call(QStringLiteral("close_pane"), {{QStringLiteral("pane"), 1}});
    EXPECT_TRUE(refused.value(QLatin1String("isError")).toBool());
    EXPECT_TRUE(textOf(refused).contains(QLatin1String("end_session true"))) << textOf(refused).toStdString();
    const QJsonObject ended =
        h.call(QStringLiteral("close_pane"), {{QStringLiteral("pane"), 1}, {QStringLiteral("end_session"), true}});
    EXPECT_FALSE(ended.value(QLatin1String("isError")).toBool());
    EXPECT_TRUE(textOf(ended).contains(QLatin1String("session agent ended"))) << textOf(ended).toStdString();
    // One call each, and no list of the panes before it.
    EXPECT_EQ(h.daemon.callsOf(QStringLiteral("close")).size(), 2);
    EXPECT_TRUE(h.daemon.callsOf(QStringLiteral("listPanes")).isEmpty());
}

// Text that came within the last event interval before the timeout: the
// last check reads it.
TEST(McpToolsWait, TextChecksOnceMoreAtTheTimeout)
{
    Harness h;
    bool written = false;
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        if (fn == QLatin1String("getText")) {
            return reply(QJsonObject{{QStringLiteral("text"), written ? QStringLiteral("DONE\n") : QString()},
                                     {QStringLiteral("truncated"), false}});
        }
        return promptDaemon(fn, args, [] {
            return reply(QJsonArray());
        });
    };
    const qint64 id = h.start(QStringLiteral("wait"),
                              {{QStringLiteral("pane"), 1},
                               {QStringLiteral("until"), QStringLiteral("text")},
                               {QStringLiteral("pattern"), QStringLiteral("DONE")},
                               {QStringLiteral("timeout_ms"), 300}});
    h.run(100);
    // No event: the text is there, and nothing says so before the timeout.
    written = true;
    const QJsonObject result = h.result(id);
    EXPECT_EQ(structured(result).value(QLatin1String("status")).toString(), QStringLiteral("matched"))
        << textOf(result).toStdString();
    EXPECT_EQ(structured(result).value(QLatin1String("match")).toString(), QStringLiteral("DONE"));
}

// The events of a pane come at most Control.eventInterval apart: a shorter
// quiet time waits two of them.
TEST(McpToolsWait, QuietWaitsAtLeastTwoEventIntervals)
{
    Harness h(0, 150);
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        return promptDaemon(fn, args, [] {
            return reply(QJsonArray());
        });
    };
    QElapsedTimer clock;
    clock.start();
    const QJsonObject result = h.call(QStringLiteral("wait"),
                                      {{QStringLiteral("pane"), 1},
                                       {QStringLiteral("until"), QStringLiteral("quiet")},
                                       {QStringLiteral("quiet_ms"), 1}});
    EXPECT_EQ(structured(result).value(QLatin1String("status")).toString(), QStringLiteral("quiet"));
    EXPECT_GE(clock.elapsed(), 300);
}

// A pane that closes before the wait has its pane list: closed, from its
// paneClosed (onEvent), and from the list that lacks a pane that the server
// saw (startWait). A pane that never was: an error.
TEST(McpToolsWait, ClosedCountsAPaneThatClosedBeforeTheList)
{
    Harness h(1);
    bool tell = true;
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        if (fn == QLatin1String("listPanes")) {
            if (std::exchange(tell, false))
                h.daemon.event(QStringLiteral("paneClosed"), {1});
            return reply(QJsonArray());
        }
        return promptDaemon(fn, args, [] {
            return reply(QJsonArray());
        });
    };
    const QJsonObject closed = h.call(
        QStringLiteral("wait"), {{QStringLiteral("pane"), 1}, {QStringLiteral("until"), QStringLiteral("closed")}});
    EXPECT_EQ(structured(closed).value(QLatin1String("status")).toString(), QStringLiteral("closed"))
        << textOf(closed).toStdString();
    // No event this time: the list says it.
    const QJsonObject listed = h.call(
        QStringLiteral("wait"), {{QStringLiteral("pane"), 1}, {QStringLiteral("until"), QStringLiteral("closed")}});
    EXPECT_EQ(structured(listed).value(QLatin1String("status")).toString(), QStringLiteral("closed"))
        << textOf(listed).toStdString();
    EXPECT_FALSE(tell);
    const QJsonObject never = h.call(
        QStringLiteral("wait"), {{QStringLiteral("pane"), 99}, {QStringLiteral("until"), QStringLiteral("closed")}});
    EXPECT_TRUE(never.value(QLatin1String("isError")).toBool());
    EXPECT_TRUE(textOf(never).contains(QLatin1String("no pane 99"))) << textOf(never).toStdString();
}

// After a send, the line in progress still shows the prompt that the input
// answered: the next wait until text reads from where the input went, and
// so does each later check while the cursor is on that row. A check on a
// later row reads from the start of the row.
TEST(McpToolsWait, TextAfterASendReadsFromWhereTheInputWent)
{
    Harness h;
    const QJsonObject sent{{QStringLiteral("row"), 10}, {QStringLiteral("col"), 4}, {QStringLiteral("epoch"), 1}};
    qint64 cursorRow = 10;
    int checks = 0;
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        if (fn == QLatin1String("mark")) {
            return reply(cursorRow == 10 ? sent
                                         : QJsonObject{{QStringLiteral("row"), cursorRow},
                                                       {QStringLiteral("col"), 4},
                                                       {QStringLiteral("epoch"), 1}});
        }
        if (fn == QLatin1String("getText")) {
            // The echo, and the cursor on a new row from the second check on.
            // Then the next prompt, at the fourth.
            if (++checks == 2)
                cursorRow = 12;
            return reply(
                QJsonObject{{QStringLiteral("text"), checks < 4 ? QStringLiteral("x") : QStringLiteral(">>> ")},
                            {QStringLiteral("truncated"), false}});
        }
        if (fn == QLatin1String("sendText"))
            return reply(QJsonValue());
        return promptDaemon(fn, args, [] {
            return reply(QJsonArray());
        });
    };
    const QJsonObject wrote =
        h.call(QStringLiteral("send"), {{QStringLiteral("pane"), 1}, {QStringLiteral("text"), QStringLiteral("x\r")}});
    ASSERT_FALSE(wrote.value(QLatin1String("isError")).toBool()) << textOf(wrote).toStdString();
    // The mark goes before the input.
    ASSERT_EQ(h.daemon.calls.size(), 2);
    EXPECT_EQ(h.daemon.calls.at(0).fn, QStringLiteral("mark"));
    EXPECT_EQ(h.daemon.calls.at(1).fn, QStringLiteral("sendText"));
    const qint64 id = h.start(QStringLiteral("wait"),
                              {{QStringLiteral("pane"), 1},
                               {QStringLiteral("until"), QStringLiteral("text")},
                               {QStringLiteral("pattern"), QStringLiteral("^>>> $")},
                               {QStringLiteral("timeout_ms"), 5000}});
    for (int i = 0; i < 4 && checks < 4; ++i) {
        h.run(50);
        h.daemon.event(QStringLiteral("textChanged"), {1});
    }
    const QJsonObject result = h.result(id);
    EXPECT_EQ(structured(result).value(QLatin1String("status")).toString(), QStringLiteral("matched"))
        << textOf(result).toStdString();
    const auto texts = h.daemon.callsOf(QStringLiteral("getText"));
    ASSERT_EQ(texts.size(), 4);
    const auto markOf = [&](qsizetype i) {
        return texts.at(i).args.at(2).toObject().value(QLatin1String("mark")).toObject();
    };
    // The row of the send: from where the input went, at each check.
    EXPECT_EQ(markOf(0), sent);
    EXPECT_EQ(markOf(1), sent);
    EXPECT_EQ(markOf(2), sent);
    // A later row: from its start.
    EXPECT_EQ(markOf(3), rowMark(12, 1));
}

// A send while a wait until text runs: that wait reads what the input makes,
// and the next wait reads from its own start, not from the send.
TEST(McpToolsWait, ASendDuringATextWaitLeavesNoMark)
{
    Harness h;
    const QJsonObject sent{{QStringLiteral("row"), 10}, {QStringLiteral("col"), 4}, {QStringLiteral("epoch"), 1}};
    bool answered = false;
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        if (fn == QLatin1String("mark"))
            return reply(sent);
        if (fn == QLatin1String("getText")) {
            return reply(QJsonObject{{QStringLiteral("text"), answered ? QStringLiteral("got y") : QString()},
                                     {QStringLiteral("truncated"), false}});
        }
        if (fn == QLatin1String("sendText")) {
            answered = true;
            return reply(QJsonValue());
        }
        return promptDaemon(fn, args, [] {
            return reply(QJsonArray());
        });
    };
    const qint64 id = h.start(QStringLiteral("wait"),
                              {{QStringLiteral("pane"), 1},
                               {QStringLiteral("until"), QStringLiteral("text")},
                               {QStringLiteral("pattern"), QStringLiteral("got y")},
                               {QStringLiteral("timeout_ms"), 5000}});
    h.run(50);
    h.call(QStringLiteral("send"), {{QStringLiteral("pane"), 1}, {QStringLiteral("text"), QStringLiteral("y\r")}});
    h.daemon.event(QStringLiteral("textChanged"), {1});
    EXPECT_EQ(structured(h.result(id)).value(QLatin1String("status")).toString(), QStringLiteral("matched"));
    h.daemon.calls.clear();
    h.start(QStringLiteral("wait"),
            {{QStringLiteral("pane"), 1},
             {QStringLiteral("until"), QStringLiteral("text")},
             {QStringLiteral("pattern"), QStringLiteral("never")},
             {QStringLiteral("timeout_ms"), 200}});
    h.run(100);
    const auto texts = h.daemon.callsOf(QStringLiteral("getText"));
    ASSERT_FALSE(texts.isEmpty());
    EXPECT_EQ(texts.first().args.at(2).toObject().value(QLatin1String("mark")).toObject().value(QLatin1String("col")),
              QJsonValue(0));
}

// A pane that decodes an image when a wait starts: its mark comes a moment
// later, and the wait goes on.
TEST(McpToolsWait, TheStartMarkTriesAgain)
{
    Harness h;
    int marks = 0;
    h.daemon.answer = [&](const QString& fn, const QJsonArray& args) {
        if (fn == QLatin1String("mark") && ++marks == 1)
            return error(AfterglowMux::Error::TryAgain, QStringLiteral("pane 1 decodes an image"));
        return promptDaemon(fn, args, [] {
            return reply(QJsonArray());
        });
    };
    const QJsonObject result = h.call(QStringLiteral("wait"),
                                      {{QStringLiteral("pane"), 1},
                                       {QStringLiteral("until"), QStringLiteral("text")},
                                       {QStringLiteral("pattern"), QStringLiteral("true")}});
    EXPECT_FALSE(result.value(QLatin1String("isError")).toBool()) << textOf(result).toStdString();
    EXPECT_EQ(structured(result).value(QLatin1String("status")).toString(), QStringLiteral("matched"));
    EXPECT_GE(marks, 2);
}

// The daemon closed the link of a server that read nothing, and lets the
// session run on: the server binds it again over a new connection and closes
// that, so the daemon ends it unless another server binds it too. It kills
// nothing, then says so.
TEST(McpToolsSession, ALostLinkBindsTheSessionAgainAndClosesTheNewLink)
{
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
    FakeDaemon other;
    other.fd = pair[1];
    other.answer = [](const QString&, const QJsonArray&) {
        return reply(QJsonValue());
    };
    int handed = pair[0];
    Harness h(Harness::Options{0, 20, {}, [&handed](QString*, bool*) {
                                   return std::exchange(handed, -1);
                               }});
    ::shutdown(h.daemon.fd, SHUT_RDWR);
    // The end of the new link: its read gives nothing more.
    const auto closed = [&] {
        char byte = 0;
        return ::recv(other.fd, &byte, 1, MSG_DONTWAIT | MSG_PEEK) == 0;
    };
    EXPECT_TRUE(pumpUntil(
        [&] {
            return !other.callsOf(QStringLiteral("bindSession")).isEmpty() && closed();
        },
        [&] {
            other.serve();
        }));
    ASSERT_EQ(other.callsOf(QStringLiteral("bindSession")).size(), 1);
    EXPECT_EQ(other.callsOf(QStringLiteral("bindSession")).first().args, QJsonArray{QStringLiteral("agent")});
    EXPECT_TRUE(other.callsOf(QStringLiteral("killSession")).isEmpty());
    QJsonObject listed;
    EXPECT_TRUE(pumpUntil(
        [&] {
            listed = h.call(QStringLiteral("list_panes"), {});
            return textOf(listed).contains(QLatin1String("ends unless another server binds it"));
        },
        [&] {
            other.serve();
        }))
        << textOf(listed).toStdString();
    EXPECT_TRUE(listed.value(QLatin1String("isError")).toBool());
    ::close(other.fd);
}

namespace {

/// A socket that another process listens on, as a daemon does: a connection
/// to it names that process (SO_PEERCRED). The child calls listen() on a
/// socket that this process made and keeps, so this process accepts, and the
/// child runs until end().
struct OtherListener
{
    QTemporaryDir dir;
    sockaddr_un address = {};
    int listener = -1;
    pid_t child = -1;

    OtherListener()
    {
        address.sun_family = AF_UNIX;
        const QByteArray path = QFile::encodeName(dir.filePath(QStringLiteral("socket")));
        EXPECT_LT(std::size_t(path.size()), sizeof address.sun_path);
        std::memcpy(address.sun_path, path.constData(), std::size_t(path.size()));
        listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        EXPECT_EQ(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof address), 0);
        int ready[2];
        EXPECT_EQ(::pipe2(ready, O_CLOEXEC), 0);
        child = ::fork();
        if (child == 0) {
            // Only system calls: the parent can have threads.
            if (::listen(listener, 4) != 0)
                ::_exit(1);
            const char byte = 1;
            (void)!::write(ready[1], &byte, 1);
            for (;;)
                ::pause();
        }
        EXPECT_GT(child, 0);
        ::close(ready[1]);
        char byte = 0;
        EXPECT_EQ(::read(ready[0], &byte, 1), 1);
        ::close(ready[0]);
    }

    ~OtherListener()
    {
        end();
        ::close(listener);
    }

    /// A connection: the client's end, and the end that this process accepted.
    std::pair<int, int> connect()
    {
        const int client = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        EXPECT_EQ(::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof address), 0);
        const int served = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        EXPECT_GE(served, 0);
        return {client, served};
    }

    /// The child ends, as a daemon that stopped.
    void end()
    {
        if (child <= 0)
            return;
        ::kill(child, SIGKILL);
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) { }
        child = -1;
    }
};

/// The text of list_panes after the link of @p h closed, once it has @p words.
QString listAfterTheLink(Harness& h, const char* words)
{
    QJsonObject listed;
    EXPECT_TRUE(pumpUntil(
        [&] {
            listed = h.call(QStringLiteral("list_panes"), {});
            return textOf(listed).contains(QLatin1String(words));
        },
        [] { }))
        << textOf(listed).toStdString();
    EXPECT_TRUE(listed.value(QLatin1String("isError")).toBool());
    return textOf(listed);
}

} // namespace

// The daemon stopped, and a later one started before the server connects
// again: a session of the same name there is another one, which a bind and
// a close would end. The server binds nothing there, and says that the
// session ended with the daemon that stopped.
TEST(McpToolsSession, ALostLinkToADaemonThatStoppedBindsNothingOnALaterDaemon)
{
    OtherListener stopped;
    OtherListener later;
    const auto [link, served] = stopped.connect();
    auto [handed, laterEnd] = later.connect();
    {
        Harness h(Harness::Options{0,
                                   20,
                                   {},
                                   [&handed](QString*, bool*) {
                                       return std::exchange(handed, -1);
                                   },
                                   link,
                                   served});
        stopped.end();
        ::shutdown(h.daemon.fd, SHUT_RDWR);
        listAfterTheLink(h, "That daemon stopped, and session agent ended with it");
        EXPECT_EQ(handed, -1);
    }
    // The server closed the connection to the later daemon with nothing sent.
    char byte = 0;
    EXPECT_EQ(::recv(laterEnd, &byte, 1, MSG_DONTWAIT), 0);
    ::close(laterEnd);
}

// The daemon that closed the link still runs, but the server reaches
// another one: it binds nothing there, and says that the session may run on.
TEST(McpToolsSession, ALostLinkToADaemonThatRunsOnBindsNothingOnAnother)
{
    OtherListener first;
    OtherListener other;
    const auto [link, served] = first.connect();
    auto [handed, otherEnd] = other.connect();
    {
        Harness h(Harness::Options{0,
                                   20,
                                   {},
                                   [&handed](QString*, bool*) {
                                       return std::exchange(handed, -1);
                                   },
                                   link,
                                   served});
        ::shutdown(h.daemon.fd, SHUT_RDWR);
        const QString words = QStringLiteral("may run on in the mux daemon of process %1").arg(first.child);
        listAfterTheLink(h, qPrintable(words));
    }
    char byte = 0;
    EXPECT_EQ(::recv(otherEnd, &byte, 1, MSG_DONTWAIT), 0);
    ::close(otherEnd);
}

// The link closed because the daemon ended: the session ended with it.
TEST(McpToolsSession, ALostLinkWithNoDaemonSaysTheSessionEnded)
{
    Harness h(Harness::Options{0, 20, {}, [](QString* error, bool* noDaemon) {
                                   *error = QStringLiteral("no mux daemon runs");
                                   if (noDaemon)
                                       *noDaemon = true;
                                   return -1;
                               }});
    ::shutdown(h.daemon.fd, SHUT_RDWR);
    QJsonObject listed;
    EXPECT_TRUE(pumpUntil(
        [&] {
            listed = h.call(QStringLiteral("list_panes"), {});
            return textOf(listed).contains(QLatin1String("ended with it"));
        },
        [] { }))
        << textOf(listed).toStdString();
    EXPECT_TRUE(listed.value(QLatin1String("isError")).toBool());
}

namespace {

/// A project of two panes, b splitting a, in a directory of its own.
QString writePairProject(const QTemporaryDir& dir)
{
    QFile file(QDir(dir.path()).filePath(QStringLiteral("pair.json")));
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(R"({"version": 1, "active_tab": "t", "tabs": [{"id": "t", "title": "", "active_pane": "a", "tree": {
        "direction": "right", "ratio": 0.5,
        "first": {"pane": {"id": "a", "cwd": "/tmp", "title": "", "command": []}},
        "second": {"pane": {"id": "b", "cwd": "/tmp", "title": "", "command": []}}}}]})");
    return dir.path();
}

} // namespace

// The pane to split ended, and the daemon answered the split before it told
// of the end: the pane list says so, and the pane opens in a tab.
TEST(McpToolsProject, ASplitOfAPaneThatEndedOpensATab)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Harness h(Harness::Options{0, 20, writePairProject(dir), {}});
    qint64 next = 10;
    h.daemon.answer = [&](const QString& fn, const QJsonArray&) {
        if (fn == QLatin1String("newTab"))
            return reply(next++);
        if (fn == QLatin1String("split"))
            return error(AfterglowProto::IpcErrorCode::CallFailed, QStringLiteral("no pane 10 in session 'agent'"));
        if (fn == QLatin1String("listPanes"))
            return reply(QJsonArray());
        return reply(QJsonValue());
    };
    const QJsonObject result =
        h.call(QStringLiteral("open_project"), {{QStringLiteral("name"), QStringLiteral("pair")}});
    ASSERT_FALSE(result.value(QLatin1String("isError")).toBool()) << textOf(result).toStdString();
    EXPECT_EQ(structured(result).value(QLatin1String("panes")).toObject().value(QLatin1String("b")).toInteger(), 11);
    QStringList warnings;
    for (const QJsonValue& warning : structured(result).value(QLatin1String("warnings")).toArray())
        warnings << warning.toString();
    EXPECT_TRUE(warnings.join(QLatin1Char('\n')).contains(QLatin1String("opened in a tab of its own")))
        << warnings.join(QLatin1Char('\n')).toStdString();
}

// An open that fails closes its panes, but never the last pane of the
// session: the call says which one stays.
TEST(McpToolsProject, AFailedOpenKeepsTheLastPaneOfTheSession)
{
    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Harness h(Harness::Options{0, 20, writePairProject(dir), {}});
    h.daemon.answer = [&](const QString& fn, const QJsonArray&) {
        if (fn == QLatin1String("newTab"))
            return reply(10);
        if (fn == QLatin1String("split"))
            return error(AfterglowMux::Error::NoRoom, QStringLiteral("no room"));
        if (fn == QLatin1String("close"))
            return error(AfterglowMux::Error::LastPane, QStringLiteral("pane 10 is the last pane"));
        // The pane to split is there: no room is no end.
        if (fn == QLatin1String("listPanes"))
            return reply(QJsonArray{paneFacts(10, QStringLiteral("prompt"))});
        return reply(QJsonValue());
    };
    const QJsonObject result =
        h.call(QStringLiteral("open_project"), {{QStringLiteral("name"), QStringLiteral("pair")}});
    EXPECT_TRUE(result.value(QLatin1String("isError")).toBool());
    EXPECT_TRUE(textOf(result).contains(QLatin1String("Pane 10 of the project stays open")))
        << textOf(result).toStdString();
    const auto closes = h.daemon.callsOf(QStringLiteral("close"));
    ASSERT_EQ(closes.size(), 1);
    EXPECT_EQ(closes.first().args.at(1).toObject().value(QLatin1String("endSession")), QJsonValue(false));
}
