// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ctl.h"

#include "ControlBus.h"

#include <PhosphorDBus/Client.h>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDir>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <cstdio>
#include <optional>

namespace Afterglow {

namespace {

const char* const Usage = R"(usage: afterglow ctl <command> [options]

Drives a running afterglow (Control.enabled in the config). Commands act on
--pane N, else $AFTERGLOW_PANE_ID (set in every pane), else the focused pane
of the active window or of the window that was active last.

  ls [--json]                           list panes
  send-text [--raw] TEXT...             type TEXT (\n, \r, \t, \e and \xHH for U+00HH unless --raw)
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
                                        or a script action (script:ID.ACTION)
  watch                                 print events as JSON lines until interrupted

Put text that starts with -- after a "--": send-text -- --stdin.
)";

const QString Service = QStringLiteral(AFTERGLOW_APP_ID);

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

/// Prints the bus's events as JSON lines.
class Watcher : public QObject
{
    Q_OBJECT
public Q_SLOTS:
    void onSignal(const QDBusMessage& msg)
    {
        QJsonObject o{{QStringLiteral("event"), msg.member()}};
        QJsonArray args;
        for (const QVariant& v : msg.arguments()) {
            args.append(QJsonValue::fromVariant(v));
        }
        o.insert(QStringLiteral("args"), args);
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        std::fflush(stdout);
    }
};

QJsonValue jsonValue(QVariant value)
{
    if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
        const auto arg = value.value<QDBusArgument>();
        if (arg.currentType() == QDBusArgument::MapType) value = qdbus_cast<QVariantMap>(arg);
        else if (arg.currentType() == QDBusArgument::ArrayType) value = qdbus_cast<QStringList>(arg);
    }
    if (value.metaType() == QMetaType::fromType<QVariantMap>()) {
        QJsonObject out;
        const auto map = value.toMap();
        for (auto it = map.begin(); it != map.end(); ++it) out.insert(it.key(), jsonValue(it.value()));
        return out;
    }
    return QJsonValue::fromVariant(value);
}
int printProject(const QDBusMessage& reply)
{
    std::printf("%s\n", QJsonDocument(jsonValue(reply.arguments().value(0)).toObject()).toJson(QJsonDocument::Compact).constData());
    return 0;
}

int fail(const QString& message)
{
    std::fprintf(stderr, "afterglow ctl: %s\n", qPrintable(message));
    return 1;
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
    qDBusRegisterMetaType<PaneInfo>();
    qDBusRegisterMetaType<QList<PaneInfo>>();
    QStringList args = app.arguments().mid(1);
    if (args.isEmpty() || args[0] == QLatin1String("-h") || args[0] == QLatin1String("--help")) {
        std::fputs(Usage, args.isEmpty() ? stderr : stdout);
        return args.isEmpty() ? 2 : 0;
    }
    const QString command = args.takeFirst();
    if (!Commands.contains(command)) return usage(QStringLiteral("unknown command %1").arg(command));

    // Options before "--". What follows is a command to run, or more text.
    QStringList trailing;
    if (const qsizetype dash = args.indexOf(QStringLiteral("--")); dash >= 0) {
        trailing = args.mid(dash + 1);
        args = args.mid(0, dash);
    }
    if (command == QLatin1String("send-text") || command == QLatin1String("set-title") || command == QLatin1String("notify")) {
        args += trailing;
        trailing.clear();
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

    // Each pane names the afterglow that runs it. Another one (a separate
    // build, or one started with --config) numbers its panes from 1 too.
    const QString owner = qEnvironmentVariable("AFTERGLOW_BUS_NAME");
    const QString service = owner.isEmpty() ? Service : owner;
    const PhosphorDBus::Client client(QDBusConnection::sessionBus(), service, QLatin1String(ControlBus::ObjectPath));
    const QString iface = QLatin1String(ControlBus::Interface);
    auto call = [&](const QString& method, const QVariantList& callArgs, QDBusMessage& reply) -> bool {
        reply = client.syncCall(iface, method, callArgs, 10000);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            const QString name = reply.errorName();
            if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown") && !owner.isEmpty()) {
                fail(QStringLiteral("the afterglow that runs this pane has exited"));
            } else if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
                       || name == QLatin1String("org.freedesktop.DBus.Error.UnknownObject")) {
                fail(QStringLiteral("no running afterglow with Control.enabled"));
            } else {
                fail(reply.errorMessage());
            }
            return false;
        }
        return true;
    };
    // Options left after a command took its own are typing errors.
    auto extra = [&args](qsizetype positional) -> std::optional<QString> {
        for (const QString& arg : std::as_const(args)) {
            if (arg.startsWith(QLatin1String("--"))) return QStringLiteral("unknown option %1").arg(arg);
        }
        if (positional >= 0 && args.size() > positional) return QStringLiteral("unexpected argument %1").arg(args.value(positional));
        return std::nullopt;
    };

    const std::optional<QString> paneArg = takeValue(QStringLiteral("--pane"));
    auto targetPane = [&]() -> std::optional<uint> {
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
        QDBusMessage reply;
        if (!call(QStringLiteral("FocusedPane"), {}, reply)) {
            return std::nullopt;
        }
        return reply.arguments().value(0).toUInt();
    };
    auto printPane = [](const QDBusMessage& reply) {
        std::printf("%u\n", reply.arguments().value(0).toUInt());
        return 0;
    };

    QDBusMessage reply;
    if (command == QLatin1String("ls")) {
        const bool json = takeFlag(QStringLiteral("--json"));
        if (const auto error = extra(0)) return usage(*error);
        if (!call(QStringLiteral("ListPanes"), {}, reply)) {
            return 1;
        }
        const auto panes = qdbus_cast<QList<PaneInfo>>(reply.arguments().value(0));
        if (json) {
            QJsonArray list;
            for (const PaneInfo& p : panes) {
                list.append(QJsonObject{{QStringLiteral("id"), qint64(p.id)}, {QStringLiteral("window"), qint64(p.window)},
                                        {QStringLiteral("tab"), p.tab}, {QStringLiteral("title"), p.title},
                                        {QStringLiteral("cwd"), p.workingDirectory}, {QStringLiteral("command"), p.command},
                                        {QStringLiteral("cols"), p.cols}, {QStringLiteral("rows"), p.rows},
                                        {QStringLiteral("focused"), p.focused}});
            }
            std::printf("%s\n", QJsonDocument(list).toJson(QJsonDocument::Indented).constData());
            return 0;
        }
        // TAB is the tab's position from 0, as in --json and the D-Bus PaneInfo.
        std::printf("%-5s %-4s %-4s %-30s %s\n", "PANE", "WIN", "TAB", "TITLE", "DIRECTORY");
        for (const PaneInfo& p : panes) {
            std::printf("%-5s %-4u %-4d %-30s %s\n", qPrintable(QString::number(p.id) + (p.focused ? QStringLiteral("*") : QString())),
                        p.window, p.tab, qPrintable(printable(p.title).left(30)), qPrintable(printable(p.workingDirectory)));
        }
        return 0;
    }
    if (command == QLatin1String("watch")) {
        if (const auto error = extra(0)) return usage(*error);
        // Like every other command, fail at once when nothing would ever send events.
        if (!call(QStringLiteral("Projects"), {}, reply)) return 1;
        Watcher watcher;
        QDBusConnection bus = QDBusConnection::sessionBus();
        for (const char* name : {"PaneCreated", "PaneMoved", "PaneClosed", "TitleChanged", "Bell", "CommandStarted",
                                 "CommandFinished", "StatusChanged", "FocusChanged", "UserVarChanged", "ContextChanged"}) {
            if (!bus.connect(service, QLatin1String(ControlBus::ObjectPath), iface, QLatin1String(name), &watcher,
                             SLOT(onSignal(QDBusMessage)))) {
                return fail(QStringLiteral("cannot watch %1: %2").arg(QLatin1String(name), bus.lastError().message()));
            }
        }
        return app.exec();
    }
    if (command == QLatin1String("scripts")) {
        if (const auto error = extra(0)) return usage(*error);
        if (!call(QStringLiteral("ScriptDiagnostics"), {}, reply)) return 1;
        std::printf("%s\n", qPrintable(reply.arguments().value(0).toString()));
        return 0;
    }
    if (command == QLatin1String("open-directory")) {
        const auto layout = takeValue(QStringLiteral("--layout")).value_or(QString());
        if (const auto error = extra(1)) return usage(*error);
        if (args.size() != 1) return usage(QStringLiteral("open-directory takes PATH [--layout NAME]"));
        return call(QStringLiteral("OpenDirectory"), {absolute(args.first()), layout}, reply) ? printProject(reply) : 1;
    }
    if (command == QLatin1String("projects")) {
        if (const auto error = extra(0)) return usage(*error);
        if (!call(QStringLiteral("Projects"), {}, reply)) return 1;
        for (const auto& name : qdbus_cast<QStringList>(reply.arguments().value(0))) std::printf("%s\n", qPrintable(name));
        return 0;
    }
    if (command == QLatin1String("open-project")) {
        const bool fresh = takeFlag(QStringLiteral("--new"));
        if (const auto error = extra(1)) return usage(*error);
        if (args.size() != 1) return usage(QStringLiteral("open-project takes NAME [--new]"));
        return call(QStringLiteral("OpenProject"), {args.first(), fresh}, reply) ? printProject(reply) : 1;
    }
    // --cwd without a value stays in args and is reported as unknown.
    if (command == QLatin1String("new-window")) {
        const QString cwd = takeValue(QStringLiteral("--cwd")).value_or(QString());
        if (const auto error = extra(0)) return usage(*error);
        return call(QStringLiteral("CreateWindow"), {absolute(cwd), trailing}, reply) ? printPane(reply) : 1;
    }

    const std::optional<uint> pane = targetPane();
    if (!pane) {
        return 1;
    }
    if (command == QLatin1String("save-project")) {
        const bool overwrite = takeFlag(QStringLiteral("--overwrite"));
        if (const auto error = extra(1)) return usage(*error);
        if (args.size() != 1) return usage(QStringLiteral("save-project takes NAME [--overwrite]"));
        return call(QStringLiteral("SaveProject"), {*pane, args.first(), overwrite}, reply) ? printProject(reply) : 1;
    }
    if (command == QLatin1String("send-text")) {
        QString text;
        if (takeFlag(QStringLiteral("--stdin"))) {
            if (const auto error = extra(0)) return usage(*error);
            QFile in;
            if (!in.open(stdin, QIODevice::ReadOnly)) {
                return fail(QStringLiteral("cannot read standard input"));
            }
            text = QString::fromUtf8(in.readAll());
        } else {
            const bool raw = takeFlag(QStringLiteral("--raw"));
            if (const auto error = extra(-1)) return usage(*error);
            text = args.join(QLatin1Char(' '));
            if (!raw) {
                text = unescape(text);
            }
        }
        return call(QStringLiteral("SendText"), {*pane, text}, reply) ? 0 : 1;
    }
    if (command == QLatin1String("get-text")) {
        QString scope = QStringLiteral("screen");
        for (const char* s : {"scrollback", "selection", "last-output", "screen"}) {
            if (takeFlag(QStringLiteral("--") + QLatin1String(s))) {
                scope = QLatin1String(s);
            }
        }
        if (const auto error = extra(0)) return usage(*error);
        if (!call(QStringLiteral("GetText"), {*pane, scope}, reply)) {
            return 1;
        }
        const QByteArray text = reply.arguments().value(0).toString().toUtf8();
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
        return call(QStringLiteral("Split"), {*pane, dir, trailing}, reply) ? printPane(reply) : 1;
    }
    if (command == QLatin1String("new-tab")) {
        const QString cwd = takeValue(QStringLiteral("--cwd")).value_or(QString());
        if (const auto error = extra(0)) return usage(*error);
        return call(QStringLiteral("NewTab"), {*pane, absolute(cwd), trailing}, reply) ? printPane(reply) : 1;
    }
    if (command == QLatin1String("move-pane")) {
        bool valid = false;
        const uint target = args.value(0).toUInt(&valid);
        if (const auto error = extra(2)) return usage(*error);
        if (!valid || args.size() != 2) return usage(QStringLiteral("move-pane needs TARGET and PLACEMENT"));
        return call(QStringLiteral("MovePane"), {*pane, target, args[1]}, reply) ? 0 : 1;
    }
    if (command == QLatin1String("detach-tab") || command == QLatin1String("focus") || command == QLatin1String("close")) {
        if (const auto error = extra(0)) return usage(*error);
    }
    if (command == QLatin1String("detach-tab"))
        return call(QStringLiteral("DetachTab"), {*pane}, reply) ? 0 : 1;
    if (command == QLatin1String("focus")) {
        return call(QStringLiteral("Focus"), {*pane}, reply) ? 0 : 1;
    }
    if (command == QLatin1String("close")) {
        return call(QStringLiteral("Close"), {*pane}, reply) ? 0 : 1;
    }
    if (command == QLatin1String("set-title")) {
        if (const auto error = extra(-1)) return usage(*error);
        return call(QStringLiteral("SetTitle"), {*pane, args.join(QLatin1Char(' '))}, reply) ? 0 : 1;
    }
    if (command == QLatin1String("signal")) {
        bool okChannel = false;
        bool okValue = false;
        const int channel = args.value(0).toInt(&okChannel);
        const double value = args.value(1).toDouble(&okValue);
        if (const auto error = extra(2)) return usage(*error);
        if (!okChannel || !okValue) {
            return usage(QStringLiteral("signal needs a channel and a number"));
        }
        return call(QStringLiteral("Signal"), {*pane, channel, value}, reply) ? 0 : 1;
    }
    if (command == QLatin1String("notify")) {
        if (const auto error = extra(-1)) return usage(*error);
        if (args.isEmpty()) {
            return usage(QStringLiteral("notify needs a title"));
        }
        return call(QStringLiteral("Notify"), {*pane, args.value(0), args.mid(1).join(QLatin1Char(' '))}, reply) ? 0 : 1;
    }
    if (command == QLatin1String("action")) {
        if (const auto error = extra(1)) return usage(*error);
        if (args.isEmpty()) {
            return usage(QStringLiteral("action needs a name"));
        }
        return call(QStringLiteral("RunAction"), {*pane, args.value(0)}, reply) ? 0 : 1;
    }
    std::fputs(Usage, stderr);
    return 2;
}

} // namespace Afterglow

#include "Ctl.moc"
