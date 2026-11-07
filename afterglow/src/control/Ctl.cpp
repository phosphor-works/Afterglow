// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ctl.h"

#include "ControlSocket.h"
#include "IpcClient.h"
#include "../config/Keybinds.h"
#include "../scripting/OverlaySpec.h"

#include <AfterglowProto/Run.h>
#include <AfterglowProto/Socket.h>
#include <AfterglowProto/Text.h>
#include <AfterglowVt/Unicode.h>
#include <PhosphorIpc/IpcProtocol.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <poll.h>
#include <unistd.h>

namespace Afterglow {

namespace {

const char* const Usage = R"(Usage: afterglow ctl <command> [options]

Drives a running Afterglow (Control.enabled in the config). Commands act on
--pane N, else $AFTERGLOW_PANE_ID (set in every pane), else the focused pane
of the active window or of the window that was active last.

  ls [--json]                           list panes
  send-text [--raw] TEXT...             type TEXT (\n, \r, \t, \e, \\ and \xHH for U+00HH unless --raw)
  send-text --stdin                     type standard input (UTF-8 text)
  get-text [--screen|--scrollback|--selection|--last-output]
                                        print pane text (needs Control.allowReadText)
  split [--left|--right|--up|--down] [-- CMD...]
                                        split, run CMD or the shell, and print the new pane
  move-pane TARGET PLACEMENT            move beside pane TARGET (left, right, up or down)
                                        or to a new-tab or new-window with TARGET 0
  detach-tab                            move the pane's whole tab to a new window
  new-tab [--cwd DIR] [-- CMD...]       open a tab and print its pane
  new-window [--cwd DIR] [-- CMD...]    open a window and print its pane
  focus | close                         focus or close the pane
  set-title TITLE...                    set the pane's title ("" to clear)
  signal CHANNEL VALUE                  set a shader signal (uSignal[CHANNEL], 0-7)
  notify TITLE [BODY...]                desktop notification from the pane
  scripts                               show script and package diagnostics as JSON
  open-directory PATH [--layout NAME]   open a directory, optionally with a saved layout
  projects                              list saved projects
  save-project NAME [--overwrite]       save the pane's window layout
  open-project NAME [--new]             switch to a project or launch its layout
  action NAME                           run a keybind action (split_right, new_tab...)
                                        or start a script command (script:ID.COMMAND)
  overlay [--capture stdout|file|none] [--cwd DIR] [--title TEXT] [-n|--no-stdin] -- CMD...
                                        run CMD over the pane until it exits, print its
                                        result and exit with its status. Standard input,
                                        unless it is a terminal or -n is given, goes to
                                        CMD (at most 768 KiB)
  watch                                 print events as JSON lines until interrupted

Put text that starts with -- after a "--": send-text -- --stdin.
)";

// The events that `watch` subscribes to: the signals of ControlApi.
QStringList events()
{
    QStringList names;
    const QMetaObject& api = ControlApi::staticMetaObject;
    for (int i = QObject::staticMetaObject.methodCount(); i < api.methodCount(); ++i) {
        if (api.method(i).methodType() == QMetaMethod::Signal) names << QString::fromLatin1(api.method(i).name());
    }
    return names;
}

// Not a setting: a call that answers in a moment, which a client can repeat.
// Its end tells the user that afterglow hangs, not that the call failed. The
// clients of the socket wait as long as one another. The overlay call waits
// for the user instead, with no limit.
constexpr int ReplyTimeoutMs = AfterglowProto::Run::ReplyTimeoutMs;

int fail(const QString& message, int status = 1)
{
    std::fprintf(stderr, "afterglow ctl: %s\n", qPrintable(message));
    return status;
}

/// The control target of the afterglow that runs this pane, else of the main
/// one on this display. Connects on the first use and reports its own errors.
class Client
{
public:
    /// @p timeoutMs: -1 waits as long as the call takes (an overlay waits for the user).
    std::optional<QJsonValue> call(const QString& fn, const QJsonArray& args, int timeoutMs = ReplyTimeoutMs)
    {
        if (!connected()) return std::nullopt;
        IpcClient::Error error;
        auto result = m_client.call(QLatin1String(ControlSocket::Target), fn, args, timeoutMs, &error);
        if (!result) report(error);
        return result;
    }

    bool subscribe(const QString& event)
    {
        if (!connected()) return false;
        IpcClient::Error error;
        if (m_client.subscribe(QLatin1String(ControlSocket::Target), event, ReplyTimeoutMs, &error)) return true;
        report(error);
        return false;
    }

    QString eventName(qint64 subscription) const { return m_client.eventName(subscription); }

    /// $AFTERGLOW_PANE_ID when $AFTERGLOW_SOCKET is set, else 0
    /// (AfterglowProto::findSocket). The lookup cannot tell the socket of
    /// the pane's afterglow from that of another one: a user who points
    /// $AFTERGLOW_SOCKET elsewhere clears the pane ID or gives --pane.
    /// Empty after it reported why it cannot connect.
    std::optional<qint64> ownPane()
    {
        if (!connected()) return std::nullopt;
        return m_pane;
    }

    /// The next event, or nothing after it reported why the stream ended.
    std::optional<QJsonObject> readEvent()
    {
        IpcClient::Error error;
        auto frame = m_client.readEvent(&error);
        if (frame) return frame;
        // The target also goes when afterglow quits, which then closes the
        // connection: only a socket that still answers had control turned off.
        IpcClient::Error closed;
        if (error.code == QLatin1String(PhosphorIpc::ErrorCode::NoSuchTarget) && !m_client.targets(ReplyTimeoutMs, &closed)) {
            fail(QStringLiteral("Afterglow quit"));
        } else {
            report(error);
        }
        return std::nullopt;
    }

private:
    bool connected()
    {
        if (m_client.connected()) return true;
        // The afterglow that runs this pane, else the main one on the display.
        const AfterglowProto::SocketLookup socket = AfterglowProto::findSocket();
        if (socket.path.empty()) {
            fail(QString::fromStdString(socket.error));
            return false;
        }
        const QString path = QFile::decodeName(QByteArray::fromStdString(socket.path));
        m_own = socket.own;
        m_pane = socket.pane;
        IpcClient::Error error;
        if (m_client.connectTo(path, ReplyTimeoutMs, &error)) return true;
        // In the words of aglow run. A queue of new connections that stays
        // full: there is an Afterglow, and it does not take the connection.
        if (error.timedOut) {
            report(error);
        } else if (error.noServer) {
            const QString who = !m_own ? QStringLiteral("no running Afterglow on this display")
                : m_pane               ? QStringLiteral("the Afterglow that runs this pane has exited")
                                       : QStringLiteral("no Afterglow at $AFTERGLOW_SOCKET");
            fail(QStringLiteral("%1 (%2)").arg(who, error.message));
        } else {
            fail(QStringLiteral("cannot connect to %1: %2")
                     .arg(m_own ? owner() : QStringLiteral("the Afterglow of this display"), error.message));
        }
        return false;
    }

    /// The afterglow at $AFTERGLOW_SOCKET, for messages: it runs this pane
    /// only when $AFTERGLOW_PANE_ID names one.
    QString owner() const
    {
        return m_pane ? QStringLiteral("the Afterglow that runs this pane") : QStringLiteral("the Afterglow at $AFTERGLOW_SOCKET");
    }

    void report(const IpcClient::Error& error) const
    {
        // The socket stays open for launches: the control target is what
        // Control.enabled takes away, also from a running watch.
        if (error.code == QLatin1String(PhosphorIpc::ErrorCode::NoSuchTarget)) {
            fail(QStringLiteral("Control.enabled is off in %1").arg(m_own ? owner() : QStringLiteral("the running Afterglow")));
        } else if (error.tooLong) {
            fail(QStringLiteral("the request is over what one call carries (%1 MiB)").arg(IpcClient::MaxLineBytes >> 20));
        } else if (error.timedOut) {
            // A slow afterglow may still do what was asked.
            fail(QStringLiteral("Afterglow did not answer in %1 seconds").arg(ReplyTimeoutMs / 1000));
        } else if (error.code.isEmpty()) {
            fail(QStringLiteral("lost Afterglow: %1").arg(error.message));
        } else {
            fail(error.message);
        }
    }

    IpcClient m_client;
    bool m_own = false; ///< the socket of $AFTERGLOW_SOCKET
    qint64 m_pane = 0;
};

QString unescape(const QString& s)
{
    QString out;
    for (qsizetype i = 0; i < s.size(); ++i) {
        if (s[i] != QLatin1Char('\\') || i + 1 == s.size()) {
            out += s[i];
            continue;
        }
        const QChar c = s[++i];
        if (c == QLatin1Char('n')) {
            out += QLatin1Char('\n');
        } else if (c == QLatin1Char('r')) {
            out += QLatin1Char('\r');
        } else if (c == QLatin1Char('t')) {
            out += QLatin1Char('\t');
        } else if (c == QLatin1Char('e')) {
            out += QChar(0x1b);
        } else if (c == QLatin1Char('x')) {
            // Exactly two hex digits, else the escape stays as typed.
            const auto hex = [&s](qsizetype at) { return at < s.size() && QStringView(u"0123456789abcdefABCDEF").contains(s[at]); };
            if (hex(i + 1) && hex(i + 2)) {
                out += QChar(s.mid(i + 1, 2).toUShort(nullptr, 16));
                i += 2;
            } else {
                out += QLatin1String("\\x");
            }
        } else {
            out += c;
        }
    }
    return out;
}

int usage(const QString& message)
{
    std::fprintf(stderr, "afterglow ctl: %s\n\n%s", qPrintable(message), Usage);
    return 2;
}

// Titles and directories come from programs: the characters that a
// terminal acts on (controls, bidi controls, line separators) would reach
// this terminal. The filter of aglow run.
QString printable(const QString& text)
{
    return QString::fromStdString(AfterglowProto::printable(text.toStdString()));
}

// Qt's JSON writer escapes only C0 controls. The other characters that
// printable() changes are escaped here, so that they reach the terminal as
// text. They occur only in strings.
QByteArray terminalSafe(const QByteArray& json)
{
    const std::string_view text(json.constData(), std::size_t(json.size()));
    const auto escape = [](QByteArray& out, char32_t unit) {
        out += "\\u" + QByteArray::number(uint(unit), 16).rightJustified(4, '0');
    };
    QByteArray out;
    out.reserve(json.size());
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t at = i;
        char32_t cp = 0;
        // ASCII below DEL is Qt's: escaped in strings, layout outside them.
        if (static_cast<unsigned char>(text[at]) < 0x7f || !AfterglowProto::nextUtf8(text, i, cp)) {
            out += text[at];
            i = at + 1;
            continue;
        }
        const std::string_view character = text.substr(at, i - at);
        if (AfterglowProto::printable(character) == character) {
            out.append(character.data(), qsizetype(character.size()));
        } else if (cp > 0xffff) {
            escape(out, 0xd800 + ((cp - 0x10000) >> 10));
            escape(out, 0xdc00 + ((cp - 0x10000) & 0x3ff));
        } else {
            escape(out, cp);
        }
    }
    return out;
}

void printJson(const QJsonDocument& document, QJsonDocument::JsonFormat format)
{
    // Indented JSON ends with a newline already, compact JSON does not.
    QByteArray out = terminalSafe(document.toJson(format));
    if (!out.endsWith('\n')) out += '\n';
    std::fwrite(out.constData(), 1, std::size_t(out.size()), stdout);
}

// @p text cut and padded to @p cells terminal cells: printf pads by bytes,
// and a cut by UTF-16 units can split a character.
QString column(const QString& text, int cells)
{
    QString out;
    int used = 0;
    for (qsizetype i = 0; i < text.size();) {
        char32_t cp = text[i].unicode();
        qsizetype length = 1;
        if (text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate()) {
            cp = QChar::surrogateToUcs4(text[i], text[i + 1]);
            length = 2;
        }
        const int width = AfterglowVt::Unicode::props(cp).width;
        if (used + width > cells) break;
        out += QStringView(text).mid(i, length);
        used += width;
        i += length;
    }
    return out + QString(cells - used, QLatin1Char(' '));
}

// A pane ID as aglow run reads one: decimal digits only, no sign and no
// space, which QString::toUInt would take.
std::optional<quint32> paneNumber(const QString& text)
{
    const bool digits = std::all_of(text.cbegin(), text.cend(), [](QChar c) { return c.unicode() >= u'0' && c.unicode() <= u'9'; });
    bool ok = false;
    const uint id = digits && !text.isEmpty() ? text.toUInt(&ok) : 0;
    return ok ? std::optional<quint32>(id) : std::nullopt;
}

bool isUtf8(const QByteArray& bytes)
{
    const std::string_view text(bytes.constData(), std::size_t(bytes.size()));
    char32_t cp = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (!AfterglowProto::nextUtf8(text, i, cp)) return false;
    }
    return true;
}

/// Standard input as text, or nothing after it said why in @p status. Input
/// over @p limit bytes or not UTF-8 is refused as aglow run refuses its
/// input, with the status of a usage error and the message @p tooLong.
std::optional<QString> readStdin(qsizetype limit, const QString& tooLong, int& status)
{
    // One byte past the limit tells that the input is over it.
    // read() tells an error from the end, which QFile does not.
    QByteArray bytes;
    char buffer[64 * 1024];
    while (bytes.size() <= limit) {
        const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof buffer);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // A descriptor that another program left non-blocking: the input
            // has not come yet, which is no error.
            pollfd in{STDIN_FILENO, POLLIN, 0};
            if (::poll(&in, 1, -1) >= 0 || errno == EINTR) continue;
        }
        if (got < 0) {
            status = fail(QStringLiteral("cannot read standard input: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
            return std::nullopt;
        }
        if (got == 0) break;
        bytes.append(buffer, qsizetype(got));
    }
    if (bytes.size() > limit) {
        status = fail(tooLong, AfterglowProto::Run::UsageError);
        return std::nullopt;
    }
    // Decoding would put U+FFFD in place of the bytes.
    if (!isUtf8(bytes)) {
        status = fail(QStringLiteral("standard input is not UTF-8 text"), AfterglowProto::Run::UsageError);
        return std::nullopt;
    }
    return QString::fromUtf8(bytes);
}

// The running afterglow resolves directories itself: send it absolute ones.
QString absolute(const QString& path)
{
    // Not QDir::isAbsolutePath: it takes ":dir" for a Qt resource.
    if (path.isEmpty() || path.startsWith(QLatin1Char('~')) || path.startsWith(QLatin1Char('/'))) return path;
    // A directory that was removed has no path: the running afterglow then
    // refuses the relative one, rather than starting in /.
    const QString here = QDir::currentPath();
    if (here.isEmpty()) return path;
    return QDir::cleanPath(here + QLatin1Char('/') + path);
}

const QStringList Commands = {
    QStringLiteral("ls"), QStringLiteral("send-text"), QStringLiteral("get-text"), QStringLiteral("split"),
    QStringLiteral("move-pane"), QStringLiteral("detach-tab"), QStringLiteral("new-tab"), QStringLiteral("new-window"),
    QStringLiteral("focus"), QStringLiteral("close"), QStringLiteral("set-title"), QStringLiteral("signal"),
    QStringLiteral("notify"), QStringLiteral("scripts"), QStringLiteral("open-directory"), QStringLiteral("projects"),
    QStringLiteral("save-project"), QStringLiteral("open-project"), QStringLiteral("action"), QStringLiteral("watch"),
    QStringLiteral("overlay"),
};

// The standard input that `overlay` sends with the call, which goes in one
// request line (IpcClient::MaxLineBytes): room for the rest of the call,
// and for JSON escapes, which most text does not need.
constexpr qsizetype CtlOverlayInput = 768 * 1024;

} // namespace

int runCtl(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    if (args.isEmpty() || args[0] == QLatin1String("-h") || args[0] == QLatin1String("--help")) {
        std::fputs(Usage, args.isEmpty() ? stderr : stdout);
        return args.isEmpty() ? 2 : 0;
    }
    const QString command = args.takeFirst();
    if (!Commands.contains(command)) return usage(QStringLiteral("unknown command %1").arg(command));

    // Options before "--". What follows is a command to run, or more text.
    // The text joins the words only after the options are taken, so that
    // words after "--" are never read as options.
    QStringList trailing;
    if (const qsizetype dash = args.indexOf(QStringLiteral("--")); dash >= 0) {
        trailing = args.mid(dash + 1);
        args = args.mid(0, dash);
    }
    // Only these read words after "--": elsewhere they would be dropped unseen.
    static const QStringList TakesTrailing = {QStringLiteral("new-window"), QStringLiteral("split"), QStringLiteral("new-tab"),
                                              QStringLiteral("send-text"), QStringLiteral("set-title"), QStringLiteral("notify"),
                                              QStringLiteral("overlay")};
    if (!trailing.isEmpty() && !TakesTrailing.contains(command)) {
        return usage(QStringLiteral("unexpected argument %1").arg(trailing.first()));
    }
    auto takeFlag = [&args](const QString& flag) { return args.removeAll(flag) > 0; };
    // An option that needs a value and came last: a usage error of its own,
    // not an unknown option.
    std::optional<QString> missingValue;
    auto takeValue = [&args, &missingValue](const QString& flag) -> std::optional<QString> {
        const qsizetype i = args.indexOf(flag);
        if (i < 0) return std::nullopt;
        if (i + 1 >= args.size()) {
            missingValue = flag;
            args.removeAt(i);
            return std::nullopt;
        }
        const QString v = args[i + 1];
        args.remove(i, 2);
        return v;
    };

    Client client;
    // Options left after a command took its own are typing errors.
    auto extra = [&args, &missingValue](qsizetype positional) -> std::optional<QString> {
        if (missingValue) return QStringLiteral("%1 needs a value").arg(*missingValue);
        for (const QString& arg : std::as_const(args)) {
            if (arg.startsWith(QLatin1String("--"))) return QStringLiteral("unknown option %1").arg(arg);
        }
        if (positional >= 0 && args.size() > positional) return QStringLiteral("unexpected argument %1").arg(args.value(positional));
        return std::nullopt;
    };

    const std::optional<QString> paneArg = takeValue(QStringLiteral("--pane"));
    if (missingValue) return usage(QStringLiteral("--pane needs a pane ID"));
    // These act on no pane: a --pane given to them is a mistake.
    static const QStringList Paneless = {QStringLiteral("ls"), QStringLiteral("watch"), QStringLiteral("scripts"),
                                         QStringLiteral("open-directory"), QStringLiteral("projects"),
                                         QStringLiteral("open-project"), QStringLiteral("new-window")};
    if (paneArg && Paneless.contains(command)) return usage(QStringLiteral("%1 takes no --pane").arg(command));
    // A pane ID as `aglow run --pane` takes it: 0 names no pane.
    const std::optional<quint32> paneId = paneArg ? paneNumber(*paneArg) : std::nullopt;
    if (paneArg && (!paneId || *paneId == 0)) return usage(QStringLiteral("--pane takes a pane ID from 1"));
    // @p focused: for none, ask afterglow for its focused pane. Else 0 goes,
    // which a script command takes for the focused pane or none.
    auto targetPane = [&](bool focused) -> std::optional<qint64> {
        if (paneId) return *paneId;
        // The lookup's: $AFTERGLOW_PANE_ID whenever $AFTERGLOW_SOCKET is
        // set, whichever afterglow listens there, else 0.
        const auto own = client.ownPane();
        if (!own) return std::nullopt;
        if (*own || !focused) return *own;
        const auto result = client.call(QStringLiteral("focusedPane"), {});
        if (!result) return std::nullopt;
        return result->toInteger();
    };
    auto run = [&client](const QString& fn, const QJsonArray& callArgs) { return client.call(fn, callArgs) ? 0 : 1; };
    auto printPane = [&client](const QString& fn, const QJsonArray& callArgs) {
        const auto result = client.call(fn, callArgs);
        if (!result) return 1;
        std::printf("%lld\n", result->toInteger());
        return 0;
    };
    auto printObject = [&client](const QString& fn, const QJsonArray& callArgs) {
        const auto result = client.call(fn, callArgs);
        if (!result) return 1;
        printJson(QJsonDocument(result->toObject()), QJsonDocument::Compact);
        return 0;
    };

    if (command == QLatin1String("ls")) {
        const bool json = takeFlag(QStringLiteral("--json"));
        if (const auto error = extra(0)) return usage(*error);
        const auto result = client.call(QStringLiteral("listPanes"), {});
        if (!result) return 1;
        const QJsonArray panes = result->toArray();
        if (json) {
            printJson(QJsonDocument(panes), QJsonDocument::Indented);
            return 0;
        }
        // TAB is the tab's position from 0, as in --json.
        std::printf("%-5s %-4s %-4s %-30s %s\n", "PANE", "WIN", "TAB", "TITLE", "DIRECTORY");
        for (const QJsonValue& value : panes) {
            const QJsonObject p = value.toObject();
            const QString id = QString::number(p.value(QLatin1String("id")).toInteger())
                + (p.value(QLatin1String("focused")).toBool() ? QStringLiteral("*") : QString());
            std::printf("%-5s %-4lld %-4lld %s %s\n", qPrintable(id), p.value(QLatin1String("window")).toInteger(),
                        p.value(QLatin1String("tab")).toInteger(),
                        qPrintable(column(printable(p.value(QLatin1String("title")).toString()), 30)),
                        qPrintable(printable(p.value(QLatin1String("cwd")).toString())));
        }
        return 0;
    }
    if (command == QLatin1String("watch")) {
        if (const auto error = extra(0)) return usage(*error);
        for (const QString& event : events()) {
            if (!client.subscribe(event)) return 1;
        }
        while (const auto frame = client.readEvent()) {
            // An event for this connection alone (another target's output)
            // has no subscription, and no name here.
            if (!frame->contains(QLatin1String(PhosphorIpc::Field::SubscriptionId))) continue;
            const QJsonObject line{
                {QStringLiteral("event"), client.eventName(frame->value(QLatin1String(PhosphorIpc::Field::SubscriptionId)).toInteger())},
                {QStringLiteral("args"), frame->value(QLatin1String(PhosphorIpc::Field::Args))}};
            printJson(QJsonDocument(line), QJsonDocument::Compact);
            std::fflush(stdout);
        }
        return 1;
    }
    if (command == QLatin1String("scripts")) {
        if (const auto error = extra(0)) return usage(*error);
        const auto result = client.call(QStringLiteral("scriptDiagnostics"), {});
        if (!result) return 1;
        printJson(QJsonDocument(result->toArray()), QJsonDocument::Compact);
        return 0;
    }
    if (command == QLatin1String("open-directory")) {
        const auto layout = takeValue(QStringLiteral("--layout")).value_or(QString());
        if (const auto error = extra(1)) return usage(*error);
        if (args.size() != 1) return usage(QStringLiteral("open-directory takes PATH [--layout NAME]"));
        return printObject(QStringLiteral("openDirectory"), {absolute(args.first()), layout});
    }
    if (command == QLatin1String("projects")) {
        if (const auto error = extra(0)) return usage(*error);
        const auto result = client.call(QStringLiteral("projects"), {});
        if (!result) return 1;
        for (const QJsonValue& name : result->toArray()) {
            std::printf("%s\n", qPrintable(name.toString()));
        }
        return 0;
    }
    if (command == QLatin1String("open-project")) {
        const bool fresh = takeFlag(QStringLiteral("--new"));
        if (const auto error = extra(1)) return usage(*error);
        if (args.size() != 1) return usage(QStringLiteral("open-project takes NAME [--new]"));
        return printObject(QStringLiteral("openProject"), {args.first(), fresh});
    }
    if (command == QLatin1String("new-window")) {
        const QString cwd = takeValue(QStringLiteral("--cwd")).value_or(QString());
        if (const auto error = extra(0)) return usage(*error);
        return printPane(QStringLiteral("newWindow"), {absolute(cwd), QJsonArray::fromStringList(trailing)});
    }

    // In parts, each whole: a surrogate pair never splits. Text over
    // CtlMaxText was refused before.
    auto sendText = [&run](qint64 pane, const QString& text) {
        qsizetype at = 0;
        do {
            qsizetype size = std::min(CtlTextPart, text.size() - at);
            if (at + size < text.size() && text[at + size - 1].isHighSurrogate()) --size;
            if (run(QStringLiteral("sendText"), {pane, text.mid(at, size)}) != 0) return 1;
            at += size;
        } while (at < text.size());
        return 0;
    };

    // The commands on a pane check their arguments before the pane is
    // looked up, and send-text --stdin reads its input then: a usage error
    // or refused input never waits for a connection, nor fails as one. Each
    // leaves what it does with the pane in act.
    std::function<int(qint64 pane)> act;
    const bool scriptAction = command == QLatin1String("action") && Keybinds::isScriptTarget(args.value(0));
    if (command == QLatin1String("save-project")) {
        const bool overwrite = takeFlag(QStringLiteral("--overwrite"));
        if (const auto error = extra(1)) return usage(*error);
        if (args.size() != 1) return usage(QStringLiteral("save-project takes NAME [--overwrite]"));
        act = [&, name = args.first(), overwrite](qint64 pane) {
            return printObject(QStringLiteral("saveProject"), {pane, name, overwrite});
        };
    } else if (command == QLatin1String("send-text")) {
        QString text;
        // Refused as aglow run refuses its input, with the status of a
        // usage error.
        const QString tooLong = QStringLiteral("text over %1 MiB").arg(CtlMaxText >> 20);
        if (takeFlag(QStringLiteral("--stdin"))) {
            if (const auto error = extra(0)) return usage(*error);
            if (!trailing.isEmpty()) return usage(QStringLiteral("unexpected argument %1").arg(trailing.first()));
            int status = 0;
            const std::optional<QString> input = readStdin(CtlMaxText, tooLong, status);
            if (!input) return status;
            text = *input;
        } else {
            const bool raw = takeFlag(QStringLiteral("--raw"));
            if (const auto error = extra(-1)) return usage(*error);
            text = (args + trailing).join(QLatin1Char(' '));
            if (!raw) {
                text = unescape(text);
            }
            if (text.toUtf8().size() > CtlMaxText) return fail(tooLong, AfterglowProto::Run::UsageError);
        }
        act = [&sendText, text](qint64 pane) { return sendText(pane, text); };
    } else if (command == QLatin1String("get-text")) {
        QString scope = QStringLiteral("screen");
        int scopes = 0;
        for (const char* s : {"scrollback", "selection", "last-output", "screen"}) {
            if (takeFlag(QStringLiteral("--") + QLatin1String(s))) {
                scope = QLatin1String(s);
                ++scopes;
            }
        }
        if (scopes > 1) return usage(QStringLiteral("get-text takes one scope"));
        if (const auto error = extra(0)) return usage(*error);
        act = [&client, scope](qint64 pane) {
            const auto result = client.call(QStringLiteral("getText"), {pane, scope});
            if (!result) return 1;
            const QByteArray text = result->toString().toUtf8();
            std::fwrite(text.constData(), 1, std::size_t(text.size()), stdout);
            if (!text.endsWith('\n')) {
                std::fputc('\n', stdout);
            }
            return 0;
        };
    } else if (command == QLatin1String("split")) {
        QString dir = QStringLiteral("right");
        int directions = 0;
        for (const char* direction : {"left", "right", "up", "down"}) {
            if (takeFlag(QStringLiteral("--") + QLatin1String(direction))) {
                dir = QLatin1String(direction);
                ++directions;
            }
        }
        if (directions > 1) return usage(QStringLiteral("split takes one direction"));
        if (const auto error = extra(0)) return usage(*error);
        act = [&, dir](qint64 pane) { return printPane(QStringLiteral("split"), {pane, dir, QJsonArray::fromStringList(trailing)}); };
    } else if (command == QLatin1String("new-tab")) {
        const QString cwd = takeValue(QStringLiteral("--cwd")).value_or(QString());
        if (const auto error = extra(0)) return usage(*error);
        act = [&, cwd](qint64 pane) {
            return printPane(QStringLiteral("newTab"), {pane, absolute(cwd), QJsonArray::fromStringList(trailing)});
        };
    } else if (command == QLatin1String("move-pane")) {
        const std::optional<quint32> target = paneNumber(args.value(0));
        if (const auto error = extra(2)) return usage(*error);
        if (!target || args.size() != 2) return usage(QStringLiteral("move-pane needs TARGET and PLACEMENT"));
        act = [&, to = *target, placement = args[1]](qint64 pane) {
            return run(QStringLiteral("movePane"), {pane, qint64(to), placement});
        };
    } else if (command == QLatin1String("detach-tab") || command == QLatin1String("focus") || command == QLatin1String("close")) {
        if (const auto error = extra(0)) return usage(*error);
        // focus and close have the names of their calls.
        const QString fn = command == QLatin1String("detach-tab") ? QStringLiteral("detachTab") : command;
        act = [&, fn](qint64 pane) { return run(fn, {pane}); };
    } else if (command == QLatin1String("set-title")) {
        if (const auto error = extra(-1)) return usage(*error);
        act = [&, title = (args + trailing).join(QLatin1Char(' '))](qint64 pane) { return run(QStringLiteral("setTitle"), {pane, title}); };
    } else if (command == QLatin1String("signal")) {
        bool okChannel = false;
        bool okValue = false;
        const int channel = args.value(0).toInt(&okChannel);
        const double value = args.value(1).toDouble(&okValue);
        if (const auto error = extra(2)) return usage(*error);
        if (!okChannel || !okValue || !std::isfinite(value)) {
            return usage(QStringLiteral("signal needs a channel and a number"));
        }
        act = [&, channel, value](qint64 pane) { return run(QStringLiteral("signal"), {pane, channel, value}); };
    } else if (command == QLatin1String("notify")) {
        if (const auto error = extra(-1)) return usage(*error);
        const QStringList words = args + trailing;
        if (words.isEmpty()) {
            return usage(QStringLiteral("notify needs a title"));
        }
        act = [&, title = words.value(0), body = words.mid(1).join(QLatin1Char(' '))](qint64 pane) {
            return run(QStringLiteral("notify"), {pane, title, body});
        };
    } else if (command == QLatin1String("overlay")) {
        const QString capture = takeValue(QStringLiteral("--capture")).value_or(QStringLiteral("stdout"));
        const QString cwd = takeValue(QStringLiteral("--cwd")).value_or(QDir::currentPath());
        const std::optional<QString> title = takeValue(QStringLiteral("--title"));
        const bool noInput = takeFlag(QStringLiteral("-n")) | takeFlag(QStringLiteral("--no-stdin"));
        if (const auto error = extra(0)) return usage(*error);
        if (trailing.isEmpty()) return usage(QStringLiteral("overlay needs a command after --"));
        if (capture != QLatin1String("stdout") && capture != QLatin1String("file") && capture != QLatin1String("none"))
            return usage(QStringLiteral("--capture takes stdout, file or none"));
        QJsonObject options{{QStringLiteral("capture"), capture}, {QStringLiteral("cwd"), absolute(cwd)}};
        if (title) options.insert(QStringLiteral("title"), *title);
        // A list piped in, as for fzf. A terminal is no list: the program
        // reads the pane instead, and the call does not wait for an end of
        // file that never comes.
        if (!noInput && !isatty(STDIN_FILENO)) {
            int status = 0;
            const std::optional<QString> input = readStdin(
                CtlOverlayInput, QStringLiteral("standard input over %1 KiB: use a script for a longer list").arg(CtlOverlayInput >> 10),
                status);
            if (!input) return status;
            options.insert(QStringLiteral("stdin"), *input);
        }
        act = [&, options](qint64 pane) {
            const auto result = client.call(QStringLiteral("overlay"), {pane, QJsonArray::fromStringList(trailing), options}, -1);
            if (!result) return 1;
            const QJsonObject done = result->toObject();
            const QByteArray out = done.value(QLatin1String("stdout")).toString().toUtf8();
            std::fwrite(out.constData(), 1, std::size_t(out.size()), stdout);
            // Without read access the reply has no stdout, and nothing was cut here.
            if (done.contains(QLatin1String("stdout")) && done.value(QLatin1String("truncated")).toBool())
                fail(QStringLiteral("the result was cut at %1 MiB").arg(OverlaySpec::MaxOutput >> 20));
            const qint64 status = done.value(QLatin1String("exit_code")).toInteger(-1);
            return status >= 0 && status <= 255 ? int(status) : 1;
        };
    } else if (command == QLatin1String("action")) {
        if (const auto error = extra(1)) return usage(*error);
        if (args.isEmpty()) {
            return usage(QStringLiteral("action needs a name"));
        }
        act = [&, name = args.value(0)](qint64 pane) { return run(QStringLiteral("runAction"), {pane, name}); };
    }
    if (!act) {
        std::fputs(Usage, stderr);
        return 2;
    }
    const std::optional<qint64> pane = targetPane(!scriptAction);
    if (!pane) {
        return 1;
    }
    return act(*pane);
}

} // namespace Afterglow
