// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

// The tools of afterglow mcp that reuse what the user made: projects, which
// open into the server's own session and save from it, and the commands of
// the user's scripts, which the window that shows the session runs. No tool
// writes, enables or loads a script: a script runs later with the user's
// rights, so only the scripts that the user enabled run.

#include "mcp/McpTools.h"

#include "PaneLaunch.h"
#include "ProjectStore.h"
#include "ShownText.h"
#include "control/SessionLaunch.h"
#include "mcp/McpToolParts.h"

#include "MuxLayout.h"
#include "MuxProtocol.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/IpcServer.h>
#include <AfterglowProto/Json.h>
#include <AfterglowProto/Run.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Afterglow {

namespace {

using namespace McpParts;
using PhosphorMcp::McpResult;
using PhosphorMcp::McpTool;
namespace Run = AfterglowProto::Run;

/// A name of a project, a script or a command: 1 to 64 characters.
QJsonObject nameArgument(const QString& description)
{
    return {{L("type"), L("string")}, {L("minLength"), 1}, {L("maxLength"), 64}, {L("description"), description}};
}

QJsonObject listOf(const QJsonObject& items)
{
    return {{L("type"), L("array")}, {L("items"), items}};
}

McpTool openProjectTool()
{
    McpTool tool(QStringLiteral("open_project"));
    tool.setTitle(QStringLiteral("Open a project"))
        .setDescription(QStringLiteral(
            "Opens a saved project of the user in this server's session: its tabs come after the tabs of the "
            "session, and each pane runs the project's command in the project's directory, or the user's shell. "
            "It opens no window and changes no other session. Each split gives its new pane half of the room, and "
            "the titles of the project are not set. Returns the ID of each pane by its ID in the project. A name "
            "that is not saved gives the names of the saved projects."))
        .setInputSchema(schema({{L("name"), nameArgument(QStringLiteral("The project: letters, digits, _ and -."))}},
                               {QStringLiteral("name")}))
        .setOutputSchema(
            result({{L("name"), typed("string")},
                    {L("panes"), QJsonObject{{L("type"), L("object")}, {L("additionalProperties"), typed("integer")}}},
                    {L("warnings"), listOf(typed("string"))}},
                   {QStringLiteral("name"), QStringLiteral("panes"), QStringLiteral("warnings")}))
        .setHints({.readOnly = false, .destructive = false, .idempotent = false, .openWorld = false});
    return tool;
}

McpTool saveProjectTool()
{
    McpTool tool(QStringLiteral("save_project"));
    tool.setTitle(QStringLiteral("Save a project"))
        .setDescription(QStringLiteral(
            "Saves the tabs, splits, directories and pane commands of this server's session as a project of the "
            "user, so that open_project or the user can open it again. A project keeps only the commands that "
            "started panes (argv of open_pane, or the commands of a project), not what was typed in a shell. It "
            "needs no window. A saved project of the name is replaced only with overwrite true. A project has at "
            "most 16 tabs and 32 panes. The user can open the project later, which runs its commands with the "
            "user's rights."))
        .setInputSchema(
            schema({{L("name"), nameArgument(QStringLiteral("The project: letters, digits, _ and -."))},
                    {L("overwrite"),
                     QJsonObject{{L("type"), L("boolean")},
                                 {L("description"), QStringLiteral("Replace a saved project of the name.")}}}},
                   {QStringLiteral("name")}))
        .setOutputSchema(result(
            {{L("name"), typed("string")},
             {L("path"), typed("string")},
             {L("panes"), typed("integer")},
             {L("warnings"), listOf(typed("string"))}},
            {QStringLiteral("name"), QStringLiteral("path"), QStringLiteral("panes"), QStringLiteral("warnings")}))
        .setHints({.readOnly = false, .destructive = true, .idempotent = true, .openWorld = false});
    return tool;
}

McpTool listScriptCommandsTool()
{
    McpTool tool(QStringLiteral("list_script_commands"));
    tool.setTitle(QStringLiteral("List script commands"))
        .setDescription(QStringLiteral(
            "Lists the commands that the user's enabled scripts add, as the window that shows this server's session "
            "has them. It needs that window: the user opens one with afterglow mux attach and the session name. "
            "stdin is true for a command that reads standard input. The tools cannot add, enable or change a "
            "script."))
        .setInputSchema(schema({}))
        .setOutputSchema(result({{L("commands"),
                                  listOf(result({{L("script"), typed("string")},
                                                 {L("command"), typed("string")},
                                                 {L("summary"), typed("string")},
                                                 {L("usage"), typed("string")},
                                                 {L("stdin"), typed("boolean")}},
                                                {QStringLiteral("script"), QStringLiteral("command")}))}},
                                {QStringLiteral("commands")}))
        .setHints({.readOnly = true, .destructive = false, .idempotent = true, .openWorld = false});
    return tool;
}

McpTool runScriptCommandTool()
{
    McpTool tool(QStringLiteral("run_script_command"));
    tool.setTitle(QStringLiteral("Run a script command"))
        .setDescription(QStringLiteral(
            "Runs a command of an enabled script of the user, as aglow run runs it in pane: the command acts on "
            "that pane of this server's session, gets args, and gets stdin when it reads standard input. It needs "
            "the window that shows the session, which runs the script: else ask the user to run afterglow mux "
            "attach and the session name. Returns the exit status and the output. stdout and stderr together "
            "are at most the setting Mcp.outputLimit, cut from their start, and stderr takes at most half. At "
            "the timeout the server ends the run. A command can do all that its script can do."))
        .setInputSchema(
            schema({{L("pane"), paneArgument()},
                    {L("script"), nameArgument(QStringLiteral("The ID of the script, from list_script_commands."))},
                    {L("command"), nameArgument(QStringLiteral("The command, from list_script_commands."))},
                    {L("args"),
                     QJsonObject{{L("type"), L("array")},
                                 {L("items"), text(int(Run::MaxArgLength), {})},
                                 {L("maxItems"), qint64(Run::MaxArgs)},
                                 {L("description"), QStringLiteral("The arguments of the command.")}}},
                    {L("stdin"),
                     text(McpTools::MaxTextLength, QStringLiteral("The standard input, for a command that reads it."))},
                    {L("timeout_ms"), timeoutArgument()}},
                   {QStringLiteral("pane"), QStringLiteral("script"), QStringLiteral("command")}))
        .setOutputSchema(result({{L("exit_code"), typed("integer")},
                                 {L("stdout"), typed("string")},
                                 {L("stderr"), typed("string")},
                                 {L("truncated"), typed("boolean")}},
                                {QStringLiteral("exit_code"), QStringLiteral("stdout"), QStringLiteral("stderr"),
                                 QStringLiteral("truncated")}))
        .setHints({.readOnly = false, .destructive = true, .idempotent = false, .openWorld = false});
    return tool;
}

} // namespace

struct McpTools::ProjectOpen
{
    McpCall call;
    QString name;
    QList<ProjectStep> steps;
    QList<ProjectStore::Launch> launches;
    QStringList labels; ///< the ID of each pane in the project
    QList<qint64> ids; ///< the daemon's ID of each pane, 0 until it opened
    QList<bool> marks; ///< whether the shell of each pane marks its prompts
    /// The pane that has the focus in each tab, the active tab's last.
    QList<qsizetype> focus;
    qsizetype at = 0;
    /// A pane of the last tab: a new tab goes after it.
    qint64 beside = 0;
    QStringList warnings;
    bool cancelled = false;
};

struct McpTools::ScriptRun
{
    quint64 serial = 0;
    McpCall call;
    DaemonLink* link = nullptr;
    QTimer* timeout = nullptr;
    QString id; ///< the run ID, which the client makes up
    quint32 pane = 0;
    QString script;
    QString command;
    QJsonArray args;
    std::optional<std::string> input;
    QString out;
    QString err;
    bool truncated = false;
    /// The bytes of the output events since the last acknowledgement.
    qint64 unacknowledged = 0;
};

QList<McpTool> McpTools::workflowDefinitions()
{
    return {openProjectTool(), saveProjectTool(), listScriptCommandsTool(), runScriptCommandTool()};
}

void McpTools::openProject(McpCall call)
{
    const QString name = call.arguments().value(L("name")).toString();
    const ProjectStore store(m_session.projects);
    QJsonObject document;
    if (const QString error = store.read(name, document); !error.isEmpty()) {
        const QStringList names = store.names();
        call.fail(QStringLiteral("cannot open project %1: %2. The saved projects: %3")
                      .arg(name, error, names.isEmpty() ? QStringLiteral("none") : names.join(QStringLiteral(", "))));
        return;
    }
    auto open = std::make_shared<ProjectOpen>();
    open->name = name;
    // Programs without a slash are looked for in the PATH that the panes get.
    const QStringList searchPath = launchSearchPath(m_settings);
    for (const QJsonObject& leaf : ProjectStore::panes(document)) {
        open->launches.append(ProjectStore::launchOf(leaf, open->warnings, searchPath));
        open->labels.append(leaf.value(L("id")).toString());
    }
    open->ids = QList<qint64>(open->launches.size(), 0);
    open->marks = QList<bool>(open->launches.size(), false);
    qsizetype next = 0;
    qsizetype active = -1;
    for (const QJsonValue& value : document.value(L("tabs")).toArray()) {
        const QJsonObject tab = value.toObject();
        const qsizetype first = next;
        open->steps.append({first, -1, {}});
        planSplits(tab.value(L("tree")).toObject(), first, next, open->steps);
        qsizetype focused = first;
        for (qsizetype i = first; i < next; ++i) {
            if (open->labels.at(i) == tab.value(L("active_pane")).toString())
                focused = i;
        }
        if (tab.value(L("id")).toString() == document.value(L("active_tab")).toString())
            active = focused;
        else
            open->focus.append(focused);
    }
    if (active >= 0)
        open->focus.append(active);
    open->call = std::move(call);
    // The panes that opened stay: a cancel stops the ones after them.
    open->call.onCancelled([weak = std::weak_ptr<ProjectOpen>(open)] {
        if (const auto opening = weak.lock())
            opening->cancelled = true;
    });
    m_link.call(QStringLiteral("listPanes"), {}, [this, open](const QJsonValue& panes, const DaemonLink::Error* error) {
        if (error) {
            open->call.fail(failure(*error));
            return;
        }
        qint64 last = -1;
        for (const QJsonValue& pane : panes.toArray()) {
            const qint64 tab = pane.toObject().value(L("tab")).toInteger();
            if (tab >= last) {
                last = tab;
                open->beside = pane.toObject().value(L("id")).toInteger();
            }
        }
        openNext(open);
    });
}

void McpTools::openNext(const std::shared_ptr<ProjectOpen>& open)
{
    if (open->cancelled)
        return;
    if (!m_ended.isEmpty()) {
        open->call.fail(m_ended);
        return;
    }
    if (open->at < open->steps.size()) {
        const ProjectStep step = open->steps.at(open->at++);
        const ProjectStore::Launch& launch = open->launches.at(step.leaf);
        const QJsonObject prepared = sessionLaunch(m_settings, launch.cwd, launch.command,
                                                   m_session.environment ? &*m_session.environment : nullptr);
        open->marks[step.leaf] = marksPrompts(
            variableIn(prepared.value(L("environment")).toArray(), QStringLiteral("AFTERGLOW_SHELL_FEATURES")));
        // A pane of the project whose program ended at once has no room to
        // split: the pane that would split it opens in a tab of its own.
        const bool split = step.target >= 0 && !m_closed.contains(quint32(open->ids.at(step.target)));
        if (step.target >= 0 && !split) {
            open->warnings << QStringLiteral("Pane %1 opened in a tab of its own: pane %2, which it splits, ended")
                                  .arg(open->labels.at(step.leaf), open->labels.at(step.target));
        }
        const QJsonArray args{split ? open->ids.at(step.target) : open->beside, split ? step.direction : launch.cwd,
                              QJsonArray::fromStringList(launch.command), prepared};
        m_link.call(split ? QStringLiteral("split") : QStringLiteral("newTab"), args,
                    [this, open, step, split](const QJsonValue& value, const DaemonLink::Error* error) {
                        // The panes that opened stay, and the call is answered.
                        if (open->cancelled)
                            return;
                        if (error && split) {
                            // The pane to split ended while the call went: a tab.
                            // The daemon may answer the split before it tells
                            // of the end (paneClosed), so the pane list says.
                            const auto target = quint32(open->ids.at(step.target));
                            if (m_closed.contains(target)) {
                                --open->at;
                                return openNext(open);
                            }
                            m_link.call(QStringLiteral("listPanes"), {},
                                        [this, open, step, target, failed = *error](const QJsonValue& panes,
                                                                                    const DaemonLink::Error* error) {
                                            if (open->cancelled)
                                                return;
                                            if (!error && m_paneRuns.paneIn(panes, target).isEmpty()) {
                                                m_closed.insert(target);
                                                --open->at;
                                                return openNext(open);
                                            }
                                            failStep(open, step, failed);
                                        });
                            return;
                        }
                        if (error)
                            return failStep(open, step, *error);
                        const qint64 id = value.toInteger();
                        open->ids[step.leaf] = id;
                        // A shell with shell integration marks a prompt soon.
                        m_paneRuns.paneOpened(quint32(id), open->marks.at(step.leaf));
                        // The next tab goes after this one.
                        if (!split)
                            open->beside = id;
                        openNext(open);
                    });
        return;
    }
    if (!open->focus.isEmpty()) {
        const qint64 pane = open->ids.at(open->focus.takeFirst());
        // A pane whose program ended at once takes no focus: the project is
        // open all the same.
        m_link.call(QStringLiteral("focus"), {pane}, [this, open](const QJsonValue&, const DaemonLink::Error*) {
            openNext(open);
        });
        return;
    }
    QJsonObject panes;
    for (qsizetype i = 0; i < open->labels.size(); ++i)
        panes.insert(open->labels.at(i), open->ids.at(i));
    McpResult result = McpResult::structured(QJsonObject{{L("name"), open->name},
                                                         {L("panes"), panes},
                                                         {L("warnings"), QJsonArray::fromStringList(open->warnings)}});
    result.addText(QStringLiteral("Opened project %1 in session %2: %3.")
                       .arg(open->name, m_session.name, McpParts::panes(open->labels.size())));
    if (!open->warnings.isEmpty())
        result.addText(open->warnings.join(QLatin1Char('\n')));
    open->call.finish(result);
}

void McpTools::failStep(const std::shared_ptr<ProjectOpen>& open, const ProjectStep& step,
                        const DaemonLink::Error& error)
{
    const QString label = open->labels.at(step.leaf);
    // With no window, the panes have the size of a new window, and halving
    // it runs out.
    if (error.code == L(AfterglowMux::Error::NoRoom)) {
        return failOpen(open,
                        QStringLiteral("pane %1 of project %2 has no room for its split while no window shows "
                                       "session %3: ask the user to attach a window (afterglow mux attach %3), "
                                       "then open it again")
                            .arg(label, open->name, m_session.name));
    }
    failOpen(open, QStringLiteral("cannot open pane %1 of project %2: %3").arg(label, open->name, failure(error)));
}

void McpTools::failOpen(const std::shared_ptr<ProjectOpen>& open, const QString& text)
{
    // The session keeps no part of the project, but its last pane: a close of
    // that one would end the session, and the server with it.
    QList<qint64> panes;
    for (const qint64 pane : std::as_const(open->ids)) {
        if (pane != 0)
            panes.append(pane);
    }
    if (panes.isEmpty()) {
        open->call.fail(text);
        return;
    }
    auto left = std::make_shared<qsizetype>(panes.size());
    auto kept = std::make_shared<qint64>(0);
    for (const qint64 pane : std::as_const(panes)) {
        m_link.call(QStringLiteral("close"), {pane, QJsonObject{{L("endSession"), false}}},
                    [this, open, text, left, kept, pane](const QJsonValue&, const DaemonLink::Error* error) {
                        if (hasCode(error, AfterglowMux::Error::LastPane))
                            *kept = pane;
                        if (--*left > 0)
                            return;
                        if (*kept == 0) {
                            open->call.fail(text);
                            return;
                        }
                        open->call.fail(QStringLiteral("%1. Pane %2 of the project stays open: it is the last pane "
                                                       "of session %3")
                                            .arg(text)
                                            .arg(*kept)
                                            .arg(m_session.name));
                    });
    }
}

void McpTools::saveProject(McpCall call)
{
    const QString name = call.arguments().value(L("name")).toString();
    const bool overwrite = call.arguments().value(L("overwrite")).toBool();
    if (!ProjectStore::validName(name)) {
        call.fail(QStringLiteral("a project name has 1 to 64 letters, digits, _ and -"));
        return;
    }
    if (!overwrite && QFileInfo::exists(QDir(m_session.projects).filePath(name + QStringLiteral(".json")))) {
        call.fail(QStringLiteral("project %1 is saved already: give overwrite true to replace it, or another name")
                      .arg(name));
        return;
    }
    auto shared = std::make_shared<McpCall>(std::move(call));
    m_link.call(QStringLiteral("layout"), {},
                [this, shared, name, overwrite](const QJsonValue& layout, const DaemonLink::Error* error) {
                    if (error) {
                        shared->fail(failure(*error));
                        return;
                    }
                    m_link.call(QStringLiteral("listPanes"), {},
                                [this, shared, name, overwrite, layout](const QJsonValue& panes,
                                                                        const DaemonLink::Error* error) {
                                    if (error) {
                                        shared->fail(failure(*error));
                                        return;
                                    }
                                    // The directory that the shell of each pane reported, as a
                                    // window saves it. For the others, the daemon's directory of
                                    // their programs.
                                    auto directories = std::make_shared<QHash<quint32, QString>>();
                                    QList<quint32> unknown;
                                    for (const QJsonValue& value : panes.toArray()) {
                                        const quint32 id = quint32(value.toObject().value(L("id")).toInteger());
                                        const QString cwd = value.toObject().value(L("cwd")).toString();
                                        if (!cwd.isEmpty() && QFileInfo(cwd).isDir())
                                            directories->insert(id, cwd);
                                        else
                                            unknown.append(id);
                                    }
                                    if (unknown.isEmpty())
                                        return writeProject(shared, name, overwrite, layout, directories);
                                    auto left = std::make_shared<qsizetype>(unknown.size());
                                    for (const quint32 id : std::as_const(unknown)) {
                                        m_link.call(L(AfterglowMux::Target), L(AfterglowMux::Fn::PaneDirectory),
                                                    {m_session.name, qint64(id)},
                                                    [this, shared, name, overwrite, layout, directories, left,
                                                     id](const QJsonValue& value, const DaemonLink::Error* error) {
                                                        // Lost: the save would hold nothing true.
                                                        if (error && error->code.isEmpty()) {
                                                            shared->fail(failure(*error));
                                                            return;
                                                        }
                                                        const QString cwd = value.toString();
                                                        if (!error && !cwd.isEmpty() && QFileInfo(cwd).isDir())
                                                            directories->insert(id, cwd);
                                                        if (--*left == 0)
                                                            writeProject(shared, name, overwrite, layout, directories);
                                                    });
                                    }
                                });
                });
}

void McpTools::writeProject(const CallPtr& call, const QString& name, bool overwrite, const QJsonValue& layout,
                            const std::shared_ptr<QHash<quint32, QString>>& directories)
{
    std::string why;
    const auto parsed = AfterglowProto::Json::parse(
        QJsonDocument(layout.toObject()).toJson(QJsonDocument::Compact).toStdString(), &why);
    const auto read = parsed ? AfterglowMux::layoutFrom(*parsed, &why) : std::nullopt;
    if (!read) {
        call->fail(
            QStringLiteral("the mux daemon sent a layout that does not read: %1").arg(QString::fromStdString(why)));
        return;
    }
    QStringList warnings;
    qsizetype count = 0;
    // A pane as a window saves it: its directory, the title that the user
    // gave it, and the command that started it.
    const auto leaf = [&](std::uint32_t pane) {
        ++count;
        QString title;
        QStringList command;
        if (const auto values = read->values.find(pane); values != read->values.end()) {
            title = QString::fromStdString(values->second.title);
            for (const std::string& word : values->second.command)
                command << QString::fromStdString(word);
        }
        // A command that a project cannot keep would fail the whole save
        // (ProjectStore::validate): the pane saves as the shell.
        if (!ProjectStore::keepsCommand(command)) {
            warnings << QStringLiteral(
                            "Pane %1: a project keeps a command of at most %2 arguments of %3 bytes each. Saved "
                            "without its command")
                            .arg(pane)
                            .arg(ProjectStore::MaxCommandArgs)
                            .arg(ProjectStore::MaxCommandArgBytes);
            command.clear();
        }
        if (!directories->contains(pane)) {
            warnings << QStringLiteral("Pane %1: its directory is not known. Saved with your home directory").arg(pane);
        }
        return QJsonObject{{L("id"), QString::number(pane)},
                           {L("cwd"), directories->value(pane, QDir::homePath())},
                           {L("title"), textPrefix(title, ProjectStore::MaxTitle)},
                           {L("command"), QJsonArray::fromStringList(command)}};
    };
    QJsonArray tabs;
    for (std::size_t i = 0; i < read->tabs.size(); ++i) {
        const AfterglowMux::LayoutTab& tab = read->tabs[i];
        std::size_t at = 0;
        const QJsonObject tree = nested(tab.tree, at, leaf);
        tabs.append(QJsonObject{{L("id"), QString::number(i + 1)},
                                {L("title"), textPrefix(QString::fromStdString(tab.title), ProjectStore::MaxTitle)},
                                {L("active_pane"), QString::number(tab.focused)},
                                {L("tree"), tree}});
    }
    // A session of this computer: the project opens into a session, as the
    // save of a window that shows one writes it.
    const QJsonObject document{{L("version"), 1},
                               {L("active_tab"), QString::number(read->current + 1)},
                               {L("tabs"), tabs},
                               {L("session"), true}};
    const ProjectStore store(m_session.projects);
    if (const QString error = store.save(name, document, overwrite); !error.isEmpty()) {
        call->fail(QStringLiteral("cannot save project %1: %2").arg(name, error));
        return;
    }
    const QString path = QDir(m_session.projects).filePath(name + QStringLiteral(".json"));
    McpResult result = McpResult::structured(QJsonObject{{L("name"), name},
                                                         {L("path"), path},
                                                         {L("panes"), count},
                                                         {L("warnings"), QJsonArray::fromStringList(warnings)}});
    result.addText(QStringLiteral("Saved session %1 as project %2: %3.").arg(m_session.name, name, path));
    if (!warnings.isEmpty())
        result.addText(warnings.join(QLatin1Char('\n')));
    call->finish(result);
}

DaemonLink* McpTools::commandLink(McpCall& call)
{
    QString error;
    const int fd = m_session.connect ? m_session.connect(&error, nullptr) : -1;
    if (fd < 0) {
        call.fail(QStringLiteral("cannot reach the mux daemon: %1").arg(error));
        return nullptr;
    }
    return new DaemonLink(fd, this);
}

QString McpTools::commandsFailure(const DaemonLink::Error& error) const
{
    // The daemon ended the connection, and said why: the window that ran
    // the command closed or detached.
    if (error.code.isEmpty())
        return error.message;
    if (error.code == L(AfterglowMux::Error::NotAttached)) {
        return QStringLiteral(
                   "no window shows session %1, and only a window runs the commands of scripts. Ask the "
                   "user to run afterglow mux attach %1.")
            .arg(m_session.name);
    }
    if (error.code == L(Run::NoSuchCommand))
        return QStringLiteral("%1. list_script_commands lists the commands.").arg(error.message);
    if (error.code == L(AfterglowProto::IpcErrorCode::NoSuchTarget)) {
        return QStringLiteral(
                   "the window that shows session %1 runs no commands of scripts: Scripts.enabled or "
                   "Control.enabled is off there")
            .arg(m_session.name);
    }
    return failure(error);
}

void McpTools::listCommands(DaemonLink* link, std::function<void(const QString&)> failed,
                            std::function<void(const QJsonArray&)> next)
{
    link->call(L(AfterglowMux::Target), L(AfterglowMux::Fn::SelectSession),
               {AfterglowMux::ProtocolVersion, m_session.name},
               [this, link, failed, next](const QJsonValue&, const DaemonLink::Error* error) {
                   if (error)
                       return failed(failure(*error));
                   link->call(L(Run::Target), QStringLiteral("list"), {},
                              [this, failed, next](const QJsonValue& value, const DaemonLink::Error* error) {
                                  if (error)
                                      return failed(commandsFailure(*error));
                                  next(value.toArray());
                              });
               });
}

void McpTools::listScriptCommands(McpCall call)
{
    DaemonLink* const link = commandLink(call);
    if (!link)
        return;
    auto shared = std::make_shared<McpCall>(std::move(call));
    const QPointer<DaemonLink> guarded(link);
    // The connection goes with the call.
    shared->onCancelled([guarded] {
        if (guarded)
            guarded->deleteLater();
    });
    // A window that does not answer: the connection goes, as at a cancel. The
    // timer is the link's, and goes with it.
    auto* timeout = new QTimer(link);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, link, [this, shared, guarded] {
        shared->fail(QStringLiteral("the window that shows session %1 did not list its commands in %2 ms")
                         .arg(m_session.name)
                         .arg(m_settings.mcp.waitTimeout));
        if (guarded)
            guarded->deleteLater();
    });
    timeout->start(m_settings.mcp.waitTimeout);
    listCommands(
        link,
        [shared, guarded](const QString& text) {
            shared->fail(text);
            if (guarded)
                guarded->deleteLater();
        },
        [shared, guarded](const QJsonArray& list) {
            QJsonArray commands;
            QStringList lines;
            for (const QJsonValue& item : list) {
                const QJsonObject c = item.toObject();
                commands.append(QJsonObject{{L("script"), c.value(L("script"))},
                                            {L("command"), c.value(L("command"))},
                                            {L("summary"), c.value(L("summary"))},
                                            {L("usage"), c.value(L("usage"))},
                                            {L("stdin"), c.value(L("stdin")).toBool()}});
                lines << QStringLiteral("%1 %2: %3")
                             .arg(c.value(L("script")).toString(), c.value(L("command")).toString(),
                                  c.value(L("summary")).toString());
            }
            McpResult result = McpResult::structured(QJsonObject{{L("commands"), commands}});
            result.addText(lines.isEmpty() ? QStringLiteral("No enabled script adds commands.")
                                           : lines.join(QLatin1Char('\n')));
            shared->finish(result);
            if (guarded)
                guarded->deleteLater();
        });
}

void McpTools::runScriptCommand(McpCall call)
{
    const QJsonObject arguments = call.arguments();
    auto run = std::make_shared<ScriptRun>();
    run->pane = paneOf(call);
    run->script = arguments.value(L("script")).toString();
    run->command = arguments.value(L("command")).toString();
    run->args = arguments.value(L("args")).toArray();
    // The terminal counts UTF-16 units, which a schema does not.
    for (const QJsonValue& arg : std::as_const(run->args)) {
        if (arg.toString().size() > qsizetype(Run::MaxArgLength)) {
            call.fail(QStringLiteral("an argument has more than %1 UTF-16 units").arg(Run::MaxArgLength));
            return;
        }
    }
    if (arguments.contains(L("stdin")))
        run->input = arguments.value(L("stdin")).toString().toStdString();
    run->serial = ++m_nextScriptRun;
    run->id = QStringLiteral("mcp-%1").arg(run->serial);
    // The window puts its own ID of the pane in place of the daemon's.
    if (!DaemonLink::fits(QStringLiteral("run"),
                          {run->id, run->script, run->command, run->args, QString(), qint64(0xffffffff)},
                          L(Run::Target))) {
        call.fail(QStringLiteral("the arguments make a request over %1 MiB")
                      .arg(AfterglowProto::IpcConnection::MaxRequestBytes >> 20));
        return;
    }
    const int timeoutMs =
        std::min(arguments.value(L("timeout_ms")).toInt(m_settings.mcp.waitTimeout), m_settings.mcp.maxWaitTimeout);
    run->link = commandLink(call);
    if (!run->link)
        return;
    run->call = std::move(call);
    m_scriptRuns.emplace(run->serial, run);
    const std::weak_ptr<ScriptRun> weak = run;
    // Its connection closes, which ends the run in the window.
    run->call.onCancelled([this, weak] {
        if (const auto cancelled = weak.lock())
            dropRun(cancelled);
    });
    if (!m_scriptRuns.contains(run->serial))
        return;
    run->timeout = new QTimer(this);
    run->timeout->setSingleShot(true);
    connect(run->timeout, &QTimer::timeout, this, [this, weak, timeoutMs] {
        const auto late = weak.lock();
        if (!late)
            return;
        late->call.fail(QStringLiteral("script command %1 %2 ran longer than %3 ms, so the server ended it")
                            .arg(late->script, late->command)
                            .arg(timeoutMs));
        dropRun(late);
    });
    run->timeout->start(timeoutMs);
    connect(run->link, &DaemonLink::targetEvent, this,
            [this, weak](const QString& target, const QString& signal, const QJsonArray& args, qint64 bytes) {
                const auto running = weak.lock();
                if (!running || !m_scriptRuns.contains(running->serial) || target != L(Run::Target) || args.size() != 2
                    || args.at(0).toString() != running->id)
                    return;
                if (signal == L("stdout") || signal == L("stderr")) {
                    QString& text = signal == L("stdout") ? running->out : running->err;
                    text += args.at(1).toString();
                    // Only the end goes back: what is far before it goes now.
                    const qsizetype keep = m_settings.mcp.outputLimit;
                    if (text.size() > 2 * keep) {
                        keepEnd(text, keep);
                        running->truncated = true;
                    }
                }
                // The window sends a window of output ahead of what the
                // client acknowledged (Run::AckWindowBytes).
                running->unacknowledged += bytes;
                if (running->unacknowledged >= qint64(Run::AckBytes)) {
                    running->link->notify(L(Run::Target), QStringLiteral("ack"),
                                          {running->id, running->unacknowledged});
                    running->unacknowledged = 0;
                }
            });
    connect(run->link, &DaemonLink::closed, this, [this, weak](const QString& reason) {
        if (const auto lost = weak.lock(); lost && m_scriptRuns.contains(lost->serial)) {
            lost->call.fail(reason);
            dropRun(lost);
        }
    });
    listCommands(
        run->link,
        [this, run](const QString& text) {
            if (!m_scriptRuns.contains(run->serial))
                return;
            run->call.fail(text);
            dropRun(run);
        },
        [this, run](const QJsonArray& list) {
            if (!m_scriptRuns.contains(run->serial))
                return;
            std::optional<QJsonObject> found;
            for (const QJsonValue& item : list) {
                const QJsonObject c = item.toObject();
                if (c.value(L("script")).toString() == run->script && c.value(L("command")).toString() == run->command)
                    found = c;
            }
            if (!found) {
                run->call.fail(QStringLiteral("no enabled script %1 has a command %2 in the window of session %3: "
                                              "list_script_commands lists the commands")
                                   .arg(run->script, run->command, m_session.name));
                return dropRun(run);
            }
            if (run->input && !found->value(L("stdin")).toBool()) {
                run->call.fail(QStringLiteral("command %2 of script %1 reads no standard input: call it without stdin")
                                   .arg(run->script, run->command));
                return dropRun(run);
            }
            if (run->input)
                sendInput(run, 0);
            else
                startScriptRun(run);
        });
}

void McpTools::sendInput(const std::shared_ptr<ScriptRun>& run, qsizetype from)
{
    const std::string_view rest = std::string_view(*run->input).substr(std::size_t(from));
    // Parts that do not end inside a character, far below a request line.
    const std::size_t size = Run::inputPart(rest, true);
    const bool last = size == rest.size();
    const QString part = QString::fromUtf8(rest.data(), qsizetype(size));
    run->link->call(L(Run::Target), QStringLiteral("input"), {run->id, part, last},
                    [this, run, from, size, last](const QJsonValue&, const DaemonLink::Error* error) {
                        if (!m_scriptRuns.contains(run->serial))
                            return;
                        if (error) {
                            run->call.fail(commandsFailure(*error));
                            return dropRun(run);
                        }
                        if (last)
                            startScriptRun(run);
                        else
                            sendInput(run, from + qsizetype(size));
                    });
}

void McpTools::startScriptRun(const std::shared_ptr<ScriptRun>& run)
{
    // No directory: the command gets the pane's, as the window knows it.
    run->link->call(L(Run::Target), QStringLiteral("run"),
                    {run->id, run->script, run->command, run->args, QString(), qint64(run->pane)},
                    [this, run](const QJsonValue& value, const DaemonLink::Error* error) {
                        finishRun(run, value, error);
                    });
}

void McpTools::finishRun(const std::shared_ptr<ScriptRun>& run, const QJsonValue& value, const DaemonLink::Error* error)
{
    if (!m_scriptRuns.contains(run->serial))
        return;
    if (error) {
        run->call.fail(commandsFailure(*error));
        return dropRun(run);
    }
    // stderr takes at most half of the limit, and stdout what is left.
    const int limit = m_settings.mcp.outputLimit;
    const Output err = shape(run->err, 0, limit / 2, false);
    const Output out = shape(run->out, 0, limit - err.text.toUtf8().size(), false);
    const qint64 exit = exitStatus(value.toObject().value(L("exit")));
    McpResult result =
        McpResult::structured(QJsonObject{{L("exit_code"), exit},
                                          {L("stdout"), out.text},
                                          {L("stderr"), err.text},
                                          {L("truncated"), run->truncated || out.truncated || err.truncated}});
    result.addText(QStringLiteral("Script command %1 %2 in pane %3 exited with status %4.")
                       .arg(run->script, run->command)
                       .arg(run->pane)
                       .arg(exit));
    run->call.finish(result);
    dropRun(run);
}

void McpTools::endScriptRuns(const QString& text)
{
    const auto runs = std::exchange(m_scriptRuns, {});
    for (const auto& [serial, run] : runs) {
        run->call.fail(text);
        dropRun(run);
    }
}

void McpTools::dropRun(const std::shared_ptr<ScriptRun>& run)
{
    m_scriptRuns.erase(run->serial);
    if (run->timeout) {
        run->timeout->stop();
        run->timeout->deleteLater();
        run->timeout = nullptr;
    }
    // From the event loop: this can run inside the link's own answer.
    if (run->link) {
        disconnect(run->link, nullptr, this, nullptr);
        run->link->deleteLater();
        run->link = nullptr;
    }
}

} // namespace Afterglow
