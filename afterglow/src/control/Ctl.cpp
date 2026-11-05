// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ctl.h"

#include "ControlSocket.h"
#include "IpcClient.h"

#include <AfterglowProto/Socket.h>
#include <PhosphorIpc/IpcProtocol.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace Afterglow {

namespace {

const char* const Usage = R"(usage: afterglow ctl <command> [options]

Drives a running afterglow (Control.enabled in the config). Commands act on
--pane N, else $AFTERGLOW_PANE_ID (set in every pane), else the focused pane
of the active window or of the window that was active last.

  ls [--json]                           list panes
  send-text [--raw] TEXT...             type TEXT (\n, \r, \t, \e, \\ and \xHH for U+00HH unless --raw)
  send-text --stdin                     type standard input
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

constexpr int ReplyTimeoutMs = 10000;

int fail(const QString& message)
{
    std::fprintf(stderr, "afterglow ctl: %s\n", qPrintable(message));
    return 1;
}

/// The control target of the afterglow that runs this pane, else of the main
/// one on this display. Connects on the first use and reports its own errors.
class Client
{
public:
    std::optional<QJsonValue> call(const QString& fn, const QJsonArray& args)
    {
        if (!connected()) return std::nullopt;
        IpcClient::Error error;
        auto result = m_client.call(QLatin1String(ControlSocket::Target), fn, args, ReplyTimeoutMs, &error);
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
            fail(QStringLiteral("afterglow quit"));
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
        m_owner = socket.own ? path : QString();
        IpcClient::Error error;
        if (m_client.connectTo(path, ReplyTimeoutMs, &error)) return true;
        fail(QStringLiteral("%1 (%2)")
                 .arg(m_owner.isEmpty() ? QStringLiteral("no running afterglow on this display")
                                        : QStringLiteral("the afterglow that runs this pane has exited"),
                      error.message));
        return false;
    }

    void report(const IpcClient::Error& error) const
    {
        // The socket stays open for launches: the control target is what
        // Control.enabled takes away, also from a running watch.
        if (error.code == QLatin1String(PhosphorIpc::ErrorCode::NoSuchTarget)) {
            fail(m_owner.isEmpty() ? QStringLiteral("Control.enabled is off in the running afterglow")
                                   : QStringLiteral("Control.enabled is off in the afterglow that runs this pane"));
        } else if (error.timedOut) {
            // A slow afterglow may still do what was asked.
            fail(QStringLiteral("afterglow did not answer in %1 seconds").arg(ReplyTimeoutMs / 1000));
        } else if (error.code.isEmpty()) {
            fail(QStringLiteral("lost afterglow: %1").arg(error.message));
        } else {
            fail(error.message);
        }
    }

    IpcClient m_client;
    QString m_owner;
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

// Titles and directories come from programs: control characters in them
// would reach this terminal as escape sequences.
QString printable(const QString& text)
{
    QString out = text;
    for (QChar& c : out) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7f || (u >= 0x80 && u <= 0x9f)) c = QLatin1Char('?');
    }
    return out;
}

// Qt's JSON writer escapes only C0 controls: DEL and the C1 controls, which
// a terminal can also act on, are escaped here. They occur only in strings.
QByteArray terminalSafe(const QByteArray& json)
{
    QByteArray out;
    out.reserve(json.size());
    for (qsizetype i = 0; i < json.size(); ++i) {
        const auto byte = static_cast<unsigned char>(json[i]);
        if (byte == 0x7f) {
            out += "\\u007f";
        } else if (byte == 0xc2 && i + 1 < json.size() && static_cast<unsigned char>(json[i + 1]) >= 0x80
                   && static_cast<unsigned char>(json[i + 1]) <= 0x9f) {
            out += "\\u00" + QByteArray::number(static_cast<unsigned char>(json[++i]), 16);
        } else {
            out += char(byte);
        }
    }
    return out;
}

void printJson(const QJsonDocument& document, QJsonDocument::JsonFormat format)
{
    std::printf("%s\n", terminalSafe(document.toJson(format)).constData());
}

// The running afterglow resolves directories itself: send it absolute ones.
QString absolute(const QString& path)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char('~')) || QDir::isAbsolutePath(path)) return path;
    return QDir::cleanPath(QDir::current().absoluteFilePath(path));
}

const QStringList Commands = {
    QStringLiteral("ls"), QStringLiteral("send-text"), QStringLiteral("get-text"), QStringLiteral("split"),
    QStringLiteral("move-pane"), QStringLiteral("detach-tab"), QStringLiteral("new-tab"), QStringLiteral("new-window"),
    QStringLiteral("focus"), QStringLiteral("close"), QStringLiteral("set-title"), QStringLiteral("signal"),
    QStringLiteral("notify"), QStringLiteral("scripts"), QStringLiteral("open-directory"), QStringLiteral("projects"),
    QStringLiteral("save-project"), QStringLiteral("open-project"), QStringLiteral("action"), QStringLiteral("watch"),
};

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
                                              QStringLiteral("send-text"), QStringLiteral("set-title"), QStringLiteral("notify")};
    if (!trailing.isEmpty() && !TakesTrailing.contains(command)) {
        return usage(QStringLiteral("unexpected argument %1").arg(trailing.first()));
    }
    auto takeFlag = [&args](const QString& flag) { return args.removeAll(flag) > 0; };
    auto takeValue = [&args](const QString& flag) -> std::optional<QString> {
        const qsizetype i = args.indexOf(flag);
        if (i < 0 || i + 1 >= args.size()) {
            return std::nullopt;
        }
        const QString v = args[i + 1];
        args.remove(i, 2);
        return v;
    };

    Client client;
    // Options left after a command took its own are typing errors.
    auto extra = [&args](qsizetype positional) -> std::optional<QString> {
        for (const QString& arg : std::as_const(args)) {
            if (arg.startsWith(QLatin1String("--"))) return QStringLiteral("unknown option %1").arg(arg);
        }
        if (positional >= 0 && args.size() > positional) return QStringLiteral("unexpected argument %1").arg(args.value(positional));
        return std::nullopt;
    };

    const std::optional<QString> paneArg = takeValue(QStringLiteral("--pane"));
    // These act on no pane: a --pane given to them is a mistake.
    static const QStringList Paneless = {QStringLiteral("ls"), QStringLiteral("watch"), QStringLiteral("scripts"),
                                         QStringLiteral("open-directory"), QStringLiteral("projects"),
                                         QStringLiteral("open-project"), QStringLiteral("new-window")};
    if (paneArg && Paneless.contains(command)) return usage(QStringLiteral("%1 takes no --pane").arg(command));
    auto targetPane = [&]() -> std::optional<qint64> {
        bool ok = false;
        if (paneArg) {
            const uint id = paneArg->toUInt(&ok);
            if (!ok) {
                fail(QStringLiteral("--pane takes a number"));
                return std::nullopt;
            }
            return id;
        }
        if (const uint env = qEnvironmentVariable("AFTERGLOW_PANE_ID").toUInt(&ok); ok) {
            return env;
        }
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
            std::printf("%-5s %-4lld %-4lld %-30s %s\n", qPrintable(id), p.value(QLatin1String("window")).toInteger(),
                        p.value(QLatin1String("tab")).toInteger(),
                        qPrintable(printable(p.value(QLatin1String("title")).toString()).left(30)),
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
    // --cwd without a value stays in args and is reported as unknown.
    if (command == QLatin1String("new-window")) {
        const QString cwd = takeValue(QStringLiteral("--cwd")).value_or(QString());
        if (const auto error = extra(0)) return usage(*error);
        return printPane(QStringLiteral("newWindow"), {absolute(cwd), QJsonArray::fromStringList(trailing)});
    }

    const std::optional<qint64> pane = targetPane();
    if (!pane) {
        return 1;
    }
    if (command == QLatin1String("save-project")) {
        const bool overwrite = takeFlag(QStringLiteral("--overwrite"));
        if (const auto error = extra(1)) return usage(*error);
        if (args.size() != 1) return usage(QStringLiteral("save-project takes NAME [--overwrite]"));
        return printObject(QStringLiteral("saveProject"), {*pane, args.first(), overwrite});
    }
    if (command == QLatin1String("send-text")) {
        QString text;
        if (takeFlag(QStringLiteral("--stdin"))) {
            if (const auto error = extra(0)) return usage(*error);
            if (!trailing.isEmpty()) return usage(QStringLiteral("unexpected argument %1").arg(trailing.first()));
            QFile in;
            if (!in.open(stdin, QIODevice::ReadOnly)) {
                return fail(QStringLiteral("cannot read standard input"));
            }
            text = QString::fromUtf8(in.readAll());
        } else {
            const bool raw = takeFlag(QStringLiteral("--raw"));
            if (const auto error = extra(-1)) return usage(*error);
            text = (args + trailing).join(QLatin1Char(' '));
            if (!raw) {
                text = unescape(text);
            }
        }
        if (text.toUtf8().size() > CtlMaxText) return fail(QStringLiteral("text over %1 MiB").arg(CtlMaxText >> 20));
        // In parts, each whole: a surrogate pair never splits.
        qsizetype at = 0;
        do {
            qsizetype size = std::min(CtlTextPart, text.size() - at);
            if (at + size < text.size() && text[at + size - 1].isHighSurrogate()) --size;
            if (run(QStringLiteral("sendText"), {*pane, text.mid(at, size)}) != 0) return 1;
            at += size;
        } while (at < text.size());
        return 0;
    }
    if (command == QLatin1String("get-text")) {
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
        const auto result = client.call(QStringLiteral("getText"), {*pane, scope});
        if (!result) return 1;
        const QByteArray text = result->toString().toUtf8();
        std::fwrite(text.constData(), 1, std::size_t(text.size()), stdout);
        if (!text.endsWith('\n')) {
            std::fputc('\n', stdout);
        }
        return 0;
    }
    if (command == QLatin1String("split")) {
        QString dir = QStringLiteral("right");
        for (const auto* direction : {"left", "right", "up", "down"})
            if (takeFlag(QStringLiteral("--") + QLatin1String(direction))) dir = QLatin1String(direction);
        if (const auto error = extra(0)) return usage(*error);
        return printPane(QStringLiteral("split"), {*pane, dir, QJsonArray::fromStringList(trailing)});
    }
    if (command == QLatin1String("new-tab")) {
        const QString cwd = takeValue(QStringLiteral("--cwd")).value_or(QString());
        if (const auto error = extra(0)) return usage(*error);
        return printPane(QStringLiteral("newTab"), {*pane, absolute(cwd), QJsonArray::fromStringList(trailing)});
    }
    if (command == QLatin1String("move-pane")) {
        bool valid = false;
        const uint target = args.value(0).toUInt(&valid);
        if (const auto error = extra(2)) return usage(*error);
        if (!valid || args.size() != 2) return usage(QStringLiteral("move-pane needs TARGET and PLACEMENT"));
        return run(QStringLiteral("movePane"), {*pane, qint64(target), args[1]});
    }
    if (command == QLatin1String("detach-tab") || command == QLatin1String("focus") || command == QLatin1String("close")) {
        if (const auto error = extra(0)) return usage(*error);
    }
    if (command == QLatin1String("detach-tab")) return run(QStringLiteral("detachTab"), {*pane});
    if (command == QLatin1String("focus")) return run(QStringLiteral("focus"), {*pane});
    if (command == QLatin1String("close")) return run(QStringLiteral("close"), {*pane});
    if (command == QLatin1String("set-title")) {
        if (const auto error = extra(-1)) return usage(*error);
        return run(QStringLiteral("setTitle"), {*pane, (args + trailing).join(QLatin1Char(' '))});
    }
    if (command == QLatin1String("signal")) {
        bool okChannel = false;
        bool okValue = false;
        const int channel = args.value(0).toInt(&okChannel);
        const double value = args.value(1).toDouble(&okValue);
        if (const auto error = extra(2)) return usage(*error);
        if (!okChannel || !okValue || !std::isfinite(value)) {
            return usage(QStringLiteral("signal needs a channel and a number"));
        }
        return run(QStringLiteral("signal"), {*pane, channel, value});
    }
    if (command == QLatin1String("notify")) {
        if (const auto error = extra(-1)) return usage(*error);
        const QStringList words = args + trailing;
        if (words.isEmpty()) {
            return usage(QStringLiteral("notify needs a title"));
        }
        return run(QStringLiteral("notify"), {*pane, words.value(0), words.mid(1).join(QLatin1Char(' '))});
    }
    if (command == QLatin1String("action")) {
        if (const auto error = extra(1)) return usage(*error);
        if (args.isEmpty()) {
            return usage(QStringLiteral("action needs a name"));
        }
        return run(QStringLiteral("runAction"), {*pane, args.value(0)});
    }
    std::fputs(Usage, stderr);
    return 2;
}

} // namespace Afterglow

