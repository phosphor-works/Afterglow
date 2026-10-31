// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Ctl.h"

#include "ControlBus.h"

#include <PhosphorDBus/Client.h>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <cstdio>

namespace Afterglow {

namespace {

const char* const Usage = R"(usage: afterglow ctl <command> [options]

Drives a running afterglow (Control.enabled in the config). Commands act on
--pane N, else $AFTERGLOW_PANE_ID (set in every pane), else the focused pane.

  ls [--json]                         list panes
  send-text [--raw] TEXT...           type TEXT (\n, \t, \e, \xHH unless --raw)
  send-text --stdin                   type standard input
  get-text [--screen|--scrollback|--selection|--last-output]
                                      print pane text (needs Control.allowReadText)
  split [--left|--right|--up|--down] [-- CMD...]  split, running CMD or the shell; prints the new pane
  move-pane TARGET PLACEMENT        move beside pane (left/right/up/down); TARGET 0 for new-tab/new-window
  detach-tab                        detach the selected pane's whole tab
  new-tab [--cwd DIR] [-- CMD...]     open a tab; prints its pane
  new-window [--cwd DIR] [-- CMD...]  open a window; prints its pane
  focus | close                       focus or close the pane
  set-title TITLE                     set the pane's title ("" to clear)
  signal CHANNEL VALUE                set a shader signal (uSignal[CHANNEL], 0-7)
  notify TITLE [BODY]                 desktop notification from the pane
  scripts                             show script/package diagnostics as JSON
  open-directory PATH [--layout NAME]  open a repository with a saved layout
  projects                            list saved projects
  save-project NAME [--overwrite]      save the target pane's window layout
  open-project NAME [--new]            switch to a project or launch its layout
  action NAME                         run a keybind action (split_right, new_tab...)
                                      or a script action (script:ID.ACTION)
  watch                               print events as JSON lines until interrupted
)";

const QString Service = QStringLiteral("com.phosphorworks.Afterglow");

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
        } else if (c == QLatin1Char('x') && i + 2 < s.size()) {
            bool ok = false;
            const int v = s.mid(i + 1, 2).toInt(&ok, 16);
            if (ok) {
                out += QChar(v);
                i += 2;
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

    // Options before "--"; what follows is a command to run.
    QStringList trailing;
    if (const qsizetype dash = args.indexOf(QStringLiteral("--")); dash >= 0) {
        trailing = args.mid(dash + 1);
        args = args.mid(0, dash);
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

    const PhosphorDBus::Client client(QDBusConnection::sessionBus(), Service, QLatin1String(ControlBus::ObjectPath));
    const QString iface = QLatin1String(ControlBus::Interface);
    auto call = [&](const QString& method, const QVariantList& callArgs, QDBusMessage& reply) -> bool {
        reply = client.syncCall(iface, method, callArgs, 10000);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            const QString name = reply.errorName();
            if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
                || name == QLatin1String("org.freedesktop.DBus.Error.UnknownObject")
                || name == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod")) {
                fail(QStringLiteral("no running afterglow with Control.enabled"));
            } else {
                fail(reply.errorMessage());
            }
            return false;
        }
        return true;
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
        std::printf("%-5s %-4s %-4s %-30s %s\n", "PANE", "WIN", "TAB", "TITLE", "DIRECTORY");
        for (const PaneInfo& p : panes) {
            std::printf("%-5s %-4u %-4d %-30s %s\n", qPrintable(QString::number(p.id) + (p.focused ? QStringLiteral("*") : QString())),
                        p.window, p.tab + 1, qPrintable(p.title.left(30)), qPrintable(p.workingDirectory));
        }
        return 0;
    }
    if (command == QLatin1String("watch")) {
        Watcher watcher;
        QDBusConnection bus = QDBusConnection::sessionBus();
        for (const char* name : {"PaneCreated", "PaneMoved", "PaneClosed", "TitleChanged", "Bell", "CommandStarted",
                                 "CommandFinished", "StatusChanged", "FocusChanged", "UserVarChanged", "ContextChanged"}) {
            bus.connect(Service, QLatin1String(ControlBus::ObjectPath), iface, QLatin1String(name), &watcher,
                        SLOT(onSignal(QDBusMessage)));
        }
        return app.exec();
    }
    if (command == QLatin1String("scripts")) {
        if (!call(QStringLiteral("ScriptDiagnostics"), {}, reply)) return 1;
        std::printf("%s\n", qPrintable(reply.arguments().value(0).toString()));
        return 0;
    }
    if (command == QLatin1String("open-directory")) {
        const auto layout = takeValue(QStringLiteral("--layout")).value_or(QString());
        if (args.size() != 1) return fail(QStringLiteral("open-directory takes PATH [--layout NAME]"));
        return call(QStringLiteral("OpenDirectory"), {args.first(), layout}, reply) ? printProject(reply) : 1;
    }
    if (command == QLatin1String("projects")) {
        if (!call(QStringLiteral("Projects"), {}, reply)) return 1;
        for (const auto& name : qdbus_cast<QStringList>(reply.arguments().value(0))) std::printf("%s\n", qPrintable(name));
        return 0;
    }
    if (command == QLatin1String("open-project")) {
        const bool fresh = takeFlag(QStringLiteral("--new"));
        if (args.size() != 1) return fail(QStringLiteral("open-project takes NAME [--new]"));
        return call(QStringLiteral("OpenProject"), {args.first(), fresh}, reply) ? printProject(reply) : 1;
    }
    if (command == QLatin1String("new-window")) {
        const QString cwd = takeValue(QStringLiteral("--cwd")).value_or(QString());
        return call(QStringLiteral("CreateWindow"), {cwd, trailing}, reply) ? printPane(reply) : 1;
    }

    const std::optional<uint> pane = targetPane();
    if (!pane) {
        return 1;
    }
    if (command == QLatin1String("save-project")) {
        const bool overwrite = takeFlag(QStringLiteral("--overwrite"));
        if (args.size() != 1) return fail(QStringLiteral("save-project takes NAME [--overwrite]"));
        return call(QStringLiteral("SaveProject"), {*pane, args.first(), overwrite}, reply) ? printProject(reply) : 1;
    }
    if (command == QLatin1String("send-text")) {
        QString text;
        if (takeFlag(QStringLiteral("--stdin"))) {
            QFile in;
            if (!in.open(stdin, QIODevice::ReadOnly)) {
                return fail(QStringLiteral("cannot read standard input"));
            }
            text = QString::fromUtf8(in.readAll());
        } else {
            const bool raw = takeFlag(QStringLiteral("--raw"));
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
        return call(QStringLiteral("Split"), {*pane, dir, trailing}, reply) ? printPane(reply) : 1;
    }
    if (command == QLatin1String("new-tab")) {
        const QString cwd = takeValue(QStringLiteral("--cwd")).value_or(QString());
        return call(QStringLiteral("NewTab"), {*pane, cwd, trailing}, reply) ? printPane(reply) : 1;
    }
    if (command == QLatin1String("move-pane")) {
        bool valid = false;
        const uint target = args.value(0).toUInt(&valid);
        if (!valid || args.size() != 2) return fail(QStringLiteral("move-pane needs TARGET and PLACEMENT"));
        return call(QStringLiteral("MovePane"), {*pane, target, args[1]}, reply) ? 0 : 1;
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
        return call(QStringLiteral("SetTitle"), {*pane, args.value(0)}, reply) ? 0 : 1;
    }
    if (command == QLatin1String("signal")) {
        bool okChannel = false;
        bool okValue = false;
        const int channel = args.value(0).toInt(&okChannel);
        const double value = args.value(1).toDouble(&okValue);
        if (!okChannel || !okValue) {
            return fail(QStringLiteral("signal needs a channel and a number"));
        }
        return call(QStringLiteral("Signal"), {*pane, channel, value}, reply) ? 0 : 1;
    }
    if (command == QLatin1String("notify")) {
        if (args.isEmpty()) {
            return fail(QStringLiteral("notify needs a title"));
        }
        return call(QStringLiteral("Notify"), {*pane, args.value(0), args.mid(1).join(QLatin1Char(' '))}, reply) ? 0 : 1;
    }
    if (command == QLatin1String("action")) {
        if (args.isEmpty()) {
            return fail(QStringLiteral("action needs a name"));
        }
        return call(QStringLiteral("RunAction"), {*pane, args.value(0)}, reply) ? 0 : 1;
    }
    std::fputs(Usage, stderr);
    return 2;
}

} // namespace Afterglow

#include "Ctl.moc"
