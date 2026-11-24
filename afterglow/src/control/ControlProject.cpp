// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Control.h"
#include "../PaneLaunch.h"
#include "../ProjectStore.h"
#include "../TerminalWindow.h"
#include "../config/Settings.h"
#include "../mux/MuxClient.h"
#include "../mux/MuxSessions.h"

#include "MuxProtocol.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <unistd.h>

namespace Afterglow {
ProjectStore Control::projects() const
{
    return ProjectStore(ProjectStore::directoryBeside(m_settings.path()));
}
QStringList Control::projectNames() const
{
    return projects().names();
}
QString Control::saveProject(quint32 id, const QString& name, bool overwrite, QVariantMap& result)
{
    TerminalWindow* window = nullptr;
    if (!find(id, &window))
        return QStringLiteral("no pane %1").arg(id);
    // The commands that a project cannot keep save as the shell: the caller
    // hears which.
    QStringList warnings;
    const auto error = projects().save(name, window->projectLayout(&warnings), overwrite);
    if (!error.isEmpty())
        return error;
    window->setProjectName(name);
    result = {{QStringLiteral("name"), name}, {QStringLiteral("window_id"), window->id()}};
    if (!warnings.isEmpty())
        result.insert(QStringLiteral("warnings"), warnings);
    return {};
}
void Control::openProject(const QString& name, bool newInstance, const QString& mode, ProjectOpened done)
{
    QVariantMap result;
    const auto answer = [&](const QString& error) {
        done(error, error.isEmpty() ? result : QVariantMap{});
    };
    if (!ProjectStore::validName(name))
        return answer(QStringLiteral("invalid project name"));
    if (!mode.isEmpty() && mode != QLatin1String("session") && mode != QLatin1String("window"))
        return answer(QStringLiteral("a project opens into a session or a window"));
    // A window of the project that opens no more: it goes, and is no
    // window of the project.
    const auto usable = [](const QPointer<TerminalWindow>& window) {
        return window && !window->closing() && window->focusedPane();
    };
    // A window of the project comes to the front, of the kind that it
    // opens into when @p session says one.
    const auto reuse = [&](std::optional<bool> session) {
        if (newInstance)
            return false;
        for (const auto& window : m_windows) {
            if (!usable(window) || window->projectName() != name)
                continue;
            // A window of a session of another host saved no session: it is
            // a window of the project, as the file says.
            const bool shows = window->showsSession() && window->sessionHost().isEmpty();
            if (session && shows != *session)
                continue;
            focus(window->focusedPane()->id());
            result = {{QStringLiteral("name"), name},
                      {QStringLiteral("window_id"), window->id()},
                      {QStringLiteral("reused"), true}};
            if (shows)
                result.insert(QStringLiteral("session"), window->sessionName());
            return true;
        }
        return false;
    };
    QJsonObject document;
    if (const auto error = projects().read(name, document); !error.isEmpty()) {
        // A window keeps the name of a project whose file has gone, which
        // Switch project lists as running: it still comes to the front.
        if (mode.isEmpty() && reuse(std::nullopt))
            return answer({});
        return answer(error);
    }
    // A project whose name names no session opens into a window, unless
    // asked for a session, which then says why not.
    const bool session = mode.isEmpty()
        ? ProjectStore::opensSession(document) && AfterglowMux::validSessionName(name.toStdString())
        : mode == QLatin1String("session");
    if (reuse(session))
        return answer({});
    if (session)
        return openProjectSession(name, document, newInstance, std::move(done));
    auto* window = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!window)
        return answer(QStringLiteral("cannot open a window"));
    window->setProjectName(name);
    result = window->restoreProject(document);
    // A window that restored nothing stays hidden and keeps afterglow running.
    if (const QString error = result.value(QStringLiteral("error")).toString(); !error.isEmpty()) {
        window->deleteLater();
        return answer(error);
    }
    result.insert(QStringLiteral("name"), name);
    const auto warnings = result.value(QStringLiteral("warnings")).toStringList();
    if (!warnings.isEmpty() && window->focusedPane())
        notify(window->focusedPane()->id(), QStringLiteral("Project opened with warnings"),
               warnings.join(QLatin1Char('\n')));
    answer({});
}

TerminalWindow* Control::sessionWindow(const QString& session) const
{
    for (const auto& window : m_windows) {
        // One that closes shows the session no more.
        if (!window || window->closing())
            continue;
        for (Pane* pane : window->panes()) {
            if (pane->showsSession(session))
                return window;
        }
    }
    return nullptr;
}

void Control::openProjectSession(const QString& name, const QJsonObject& document, bool newInstance, ProjectOpened done)
{
    if (!AfterglowMux::validSessionName(name.toStdString())) {
        return done(QStringLiteral("project %1 cannot name a session: the name of a session must be %2")
                        .arg(name, QLatin1String(AfterglowMux::SessionNameRule)),
                    {});
    }
    if (!m_showSessionLink)
        return done(QStringLiteral("cannot open a window"), {});
    // One open of the project's session at a time: another one waits for it,
    // and then opens the project again, which finds its window. `--new`
    // starts a session of its own.
    if (!newInstance) {
        if (const auto opening = m_projectOpens.find(name); opening != m_projectOpens.end()) {
            opening->append([this, name, done = std::move(done)]() mutable {
                openProject(name, false, QStringLiteral("session"), std::move(done));
            });
            return;
        }
        m_projectOpens.insert(name, {});
    }
    // A window here that shows a session of the name: it comes to the front,
    // or it still shows a session that ended before it heard it.
    const bool shown = sessionWindow(name);
    // The name of the session, unless such a window, or another open, has
    // it: then the daemon names it.
    const bool named = !shown && !m_namingSessions.contains(name);
    if (named)
        m_namingSessions.insert(name);
    const auto finish = [this, name, newInstance, named, done = std::move(done)](const QString& error,
                                                                                 const QVariantMap& result) {
        if (named)
            m_namingSessions.remove(name);
        done(error, error.isEmpty() ? result : QVariantMap{});
        if (newInstance)
            return;
        for (const auto& next : m_projectOpens.take(name))
            next();
    };
    // A new session: its first pane is the project's first, and the daemon
    // starts the others in it too, before a window shows it, which then
    // lays them out. The launches come from this thread's settings, once:
    // the restore takes them with their warnings, and looks for no program
    // again.
    const Settings& settings = m_settings.settings();
    // The commands are looked for as a window's pane looks for them.
    const QStringList searchPath = launchSearchPath(settings);
    QList<ProjectStore::Launch> launches;
    QStringList warnings;
    for (const QJsonObject& leaf : ProjectStore::panes(document))
        launches.append(ProjectStore::launchOf(leaf, warnings, searchPath));
    // A project with no pane: what launchOf gives for none, as the window's.
    QStringList none;
    const ProjectStore::Launch first =
        launches.isEmpty() ? ProjectStore::launchOf({}, none, searchPath) : launches.first();
    AfterglowMux::SessionOptions options = localSessionOptions(settings, first.cwd, first.command, {});
    std::vector<AfterglowMux::Launch> others;
    for (qsizetype i = 1; i < launches.size(); ++i)
        others.push_back(localLaunch(settings, launches[i].cwd, launches[i].command));
    // No window measured the panes: the size of a new window, which then
    // resizes them. localSessionOptions measured it, with the fonts.
    const AfterglowMux::PaneSize size = options.size;
    const std::size_t eventQueue = settings.mux.eventQueue;
    const std::size_t history = settings.terminal.maxScrollback;
    // What the calls read from the process: the GUI thread changes its
    // environment (an activation token).
    const LocalDaemon daemon = localDaemon(settings);
    struct Started
    {
        bool runs = false;
        QString session;
        QString error;
        /// The daemon's IDs of the project's panes, in their order: the
        /// session's first pane, then those that it started for the others.
        std::vector<std::uint32_t> panes;
        /// The attach that the window takes.
        std::shared_ptr<SessionLink> link;
    };
    const QString unstarted = offThread<Started>(
        [name, newInstance, named, shown, daemon, others = std::move(others), size, eventQueue, history,
         closing = m_closing, options = std::move(options)]() mutable {
            Started started;
            AfterglowMux::Route route;
            route.sockets = daemon.sockets;
            const auto attach = [&](const QString& session) {
                // A session that another Afterglow shows stays there.
                if (auto link = attachSession(session, false, &started.error, route, eventQueue, history))
                    started.link = std::make_shared<SessionLink>(std::move(*link));
            };
            started.runs = localSessionRuns(daemon, name);
            if (closing->load())
                return started;
            if (!newInstance && started.runs) {
                if (!shown)
                    attach(name);
                return started;
            }
            // Another instance gets the daemon's next name, and so does a
            // project whose name another session has.
            options.name = started.runs || !named ? std::string() : name.toStdString();
            std::uint32_t first = 0;
            started.session = newLocalSession(daemon, options, &started.error, &first);
            if (started.session.isEmpty())
                return started;
            if (auto ids = startLocalPanes(daemon, started.session, others, size, *closing, &started.error)) {
                started.panes.push_back(first);
                started.panes.insert(started.panes.end(), ids->begin(), ids->end());
                attach(started.session);
            }
            // Afterglow quits: no window lays the session out. One that quits
            // after this look drops the answer, and the session runs on.
            if (closing->load())
                killLocalSession(daemon, started.session);
            return started;
        },
        [this, name, document, newInstance, daemon, finish, launches, warnings](Started started) {
            const auto opened = [this, name](TerminalWindow* window, const QString& session, bool reused,
                                             QVariantMap result) {
                window->setProjectName(name);
                // Its session keeps running when it closes, for the next open.
                window->keepSessionForProject();
                if (window->focusedPane())
                    focus(window->focusedPane()->id());
                result.insert(QStringLiteral("name"), name);
                result.insert(QStringLiteral("window_id"), window->id());
                result.insert(QStringLiteral("session"), session);
                result.insert(QStringLiteral("reused"), reused);
                return result;
            };
            // The session of the project runs: its window shows it, or a new one does.
            if (!newInstance && started.runs) {
                TerminalWindow* window = sessionWindow(name);
                // reused: a window here showed the session already. A new
                // window that shows the session that ran is no reuse.
                const bool reused = window != nullptr;
                if (!window && started.link) {
                    if (const QString error = m_showSessionLink(name, std::move(*started.link)); !error.isEmpty())
                        return finish(error, {});
                    window = sessionWindow(name);
                } else if (!window && started.error.isEmpty() && m_openSessionWindow) {
                    // The window that showed it closed meanwhile, which is
                    // rare: a new one attaches, off the GUI thread too.
                    m_openSessionWindow(
                        name, {}, [self = QPointer<Control>(this), name, finish, opened](const QString& error) {
                            // Afterglow quits: the caller hears nothing.
                            if (!self)
                                return;
                            if (!error.isEmpty())
                                return finish(error, {});
                            TerminalWindow* shown = self->sessionWindow(name);
                            if (!shown)
                                return finish(QStringLiteral("no window shows session %1").arg(name), {});
                            finish({}, opened(shown, name, false, {}));
                        });
                    return;
                }
                if (!window) {
                    return finish(started.error.isEmpty() ? QStringLiteral("no window shows session %1").arg(name)
                                                          : started.error,
                                  {});
                }
                return finish({}, opened(window, name, reused, {}));
            }
            // An empty error is success to finish: an error frame of the
            // daemon can come without a message.
            const QString failure =
                started.error.isEmpty() ? QStringLiteral("the mux daemon gave no reason") : started.error;
            if (started.session.isEmpty())
                return finish(failure, {});
            const QString session = started.session;
            // A session that no window lays out is ended, and the error says
            // so when it cannot be.
            const auto fail = [this, daemon, session, finish](const QString& why) {
                const QString unkilled = offThread<bool>(
                    [daemon, session] {
                        return killLocalSession(daemon, session);
                    },
                    [why, session, finish](bool killed) {
                        finish(killed ? why : QStringLiteral("%1, and session %2 still runs").arg(why, session), {});
                    });
                if (!unkilled.isEmpty())
                    finish(QStringLiteral("%1, and session %2 still runs").arg(why, session), {});
            };
            if (!started.link)
                return fail(failure);
            if (const QString why = m_showSessionLink(session, std::move(*started.link)); !why.isEmpty())
                return fail(why);
            TerminalWindow* window = sessionWindow(session);
            if (!window)
                return fail(QStringLiteral("no window shows session %1").arg(session));
            // The project's panes in their order, by the daemon's IDs: none
            // for a pane whose program ended before the window opened.
            QList<Pane*> panes;
            for (const std::uint32_t id : started.panes)
                panes.append(id ? window->sessionPane(id) : nullptr);
            const QVariantMap restored = window->restoreProject(document, panes, launches, warnings);
            // Its end closes the window.
            if (const QString why = restored.value(QStringLiteral("error")).toString(); !why.isEmpty())
                return fail(why);
            const QStringList warnings = restored.value(QStringLiteral("warnings")).toStringList();
            const QVariantMap result = opened(window, session, false, restored);
            if (!warnings.isEmpty() && window->focusedPane())
                notify(window->focusedPane()->id(), QStringLiteral("Project opened with warnings"),
                       warnings.join(QLatin1Char('\n')));
            finish({}, result);
        });
    if (!unstarted.isEmpty())
        finish(unstarted, {});
}

QString Control::openDirectory(const QString& requested, const QString& layout, QVariantMap& result)
{
    QString path = requested;
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/")))
        path = QDir::homePath() + path.mid(1);
    // Not QDir::isAbsolutePath: it takes ":dir" for a Qt resource.
    if (!path.startsWith(QLatin1Char('/')) || path.contains(QChar::Null) || path.size() > 4096)
        return QStringLiteral("project directory must be absolute or start with ~/");
    path = QFileInfo(path).canonicalFilePath();
    if (path.isEmpty() || !QFileInfo(path).isDir() || ::access(QFile::encodeName(path).constData(), X_OK) != 0)
        return QStringLiteral("project directory is unavailable");
    for (const auto& window : m_windows) {
        if (!window || window->projectDirectory() != path || !window->focusedPane())
            continue;
        focus(window->focusedPane()->id());
        result = {{QStringLiteral("window_id"), window->id()},
                  {QStringLiteral("pane_id"), window->focusedPane()->id()},
                  {QStringLiteral("cwd"), path},
                  {QStringLiteral("reused"), true}};
        return {};
    }
    QJsonObject document;
    if (!layout.isEmpty()) {
        if (const auto error = projects().read(layout, document); !error.isEmpty())
            return error;
        std::function<QJsonObject(QJsonObject)> relocate = [&](QJsonObject node) {
            if (node.contains(QStringLiteral("pane"))) {
                auto pane = node.value(QStringLiteral("pane")).toObject();
                pane.insert(QStringLiteral("cwd"), path);
                node.insert(QStringLiteral("pane"), pane);
            } else
                for (const auto& key : {QStringLiteral("first"), QStringLiteral("second")})
                    node.insert(key, relocate(node.value(key).toObject()));
            return node;
        };
        QJsonArray tabs;
        for (const auto& value : document.value(QStringLiteral("tabs")).toArray()) {
            auto tab = value.toObject();
            tab.insert(QStringLiteral("tree"), relocate(tab.value(QStringLiteral("tree")).toObject()));
            tabs.append(tab);
        }
        document.insert(QStringLiteral("tabs"), tabs);
    }
    auto* window = m_newWindow ? m_newWindow(path, {}, !layout.isEmpty()) : nullptr;
    if (!window)
        return QStringLiteral("cannot open a window");
    window->setProjectDirectory(path);
    if (layout.isEmpty())
        result = {{QStringLiteral("window_id"), window->id()}, {QStringLiteral("reused"), false}};
    else
        result = window->restoreProject(document);
    if (const QString error = result.value(QStringLiteral("error")).toString(); !error.isEmpty()) {
        window->deleteLater();
        result.clear();
        return error;
    }
    result.insert(QStringLiteral("cwd"), path);
    if (window->focusedPane()) {
        result.insert(QStringLiteral("pane_id"), window->focusedPane()->id());
        focus(window->focusedPane()->id());
        const auto warnings = result.value(QStringLiteral("warnings")).toStringList();
        if (!warnings.isEmpty())
            notify(window->focusedPane()->id(), QStringLiteral("Project opened with warnings"),
                   warnings.join(QLatin1Char('\n')));
    }
    return {};
}

void Control::projectDialog(Pane* pane, bool save)
{
    TerminalWindow* window = nullptr;
    if (!pane || !find(pane->id(), &window))
        return;
    const QPointer<Pane> guard(pane);
    const auto report = [this, guard](const QString& error) {
        if (guard && !error.isEmpty())
            notify(guard->id(), QStringLiteral("Project"), error);
    };
    const auto owner = QStringLiteral("@projects");
    if (save) {
        QVariantMap model{{QStringLiteral("kind"), QStringLiteral("prompt")},
                          {QStringLiteral("title"), QStringLiteral("Save window as project")},
                          {QStringLiteral("description"),
                           QStringLiteral("Letters, digits, underscores or hyphens. Saves tabs, splits, directories "
                                          "and explicit launch commands.")},
                          {QStringLiteral("text"), window->projectName()}};
        report(
            window->showScriptDialog(owner, pane, model, [this, guard, report, owner](QString error, QVariantMap data) {
                if (!guard || !error.isEmpty())
                    return;
                const auto name = data.value(QStringLiteral("text")).toString();
                if (!ProjectStore::validName(name)) {
                    report(QStringLiteral("Use 1–64 letters, digits, underscores or hyphens for the project name"));
                    return;
                }
                const auto write = [this, guard, name, report](bool overwrite) {
                    if (!guard)
                        return;
                    QVariantMap result;
                    const auto error = saveProject(guard->id(), name, overwrite, result);
                    if (!error.isEmpty())
                        report(error);
                    else
                        notify(guard->id(), QStringLiteral("Project saved"), name);
                };
                if (!projectNames().contains(name)) {
                    write(false);
                    return;
                }
                TerminalWindow* current = nullptr;
                if (!find(guard->id(), &current))
                    return;
                QVariantList items{QVariantMap{{QStringLiteral("id"), QStringLiteral("cancel")},
                                               {QStringLiteral("title"), QStringLiteral("Cancel")}},
                                   QVariantMap{{QStringLiteral("id"), QStringLiteral("replace")},
                                               {QStringLiteral("title"), QStringLiteral("Replace saved project")}}};
                report(current->showScriptDialog(
                    owner, guard,
                    {{QStringLiteral("kind"), QStringLiteral("select")},
                     {QStringLiteral("title"), QStringLiteral("Replace %1?").arg(name)},
                     {QStringLiteral("items"), items}},
                    [write](QString error, QVariantMap data) {
                        if (error.isEmpty() && data.value(QStringLiteral("id")) == QLatin1String("replace"))
                            write(true);
                    }));
            }));
    } else {
        QVariantList items;
        auto names = projectNames();
        for (const auto& w : m_windows)
            if (w && !w->projectName().isEmpty() && !names.contains(w->projectName()))
                names << w->projectName();
        names.sort();
        for (const auto& name : names) {
            bool live = false;
            for (const auto& w : m_windows)
                if (w && w->projectName() == name && w->focusedPane())
                    live = true;
            items << QVariantMap{
                {QStringLiteral("id"), name},
                {QStringLiteral("title"),
                 name + (live ? QStringLiteral(" (running)") : QStringLiteral(" (launch saved commands)"))}};
        }
        if (items.isEmpty()) {
            report(QStringLiteral("No saved projects. Use Save Project first."));
            return;
        }
        report(window->showScriptDialog(owner, pane,
                                        {{QStringLiteral("kind"), QStringLiteral("select")},
                                         {QStringLiteral("title"), QStringLiteral("Switch project")},
                                         {QStringLiteral("items"), items}},
                                        [this, guard, report](QString error, QVariantMap data) {
                                            if (!guard || !error.isEmpty())
                                                return;
                                            openProject(data.value(QStringLiteral("id")).toString(), false, {},
                                                        [report](const QString& error, const QVariantMap&) {
                                                            report(error);
                                                        });
                                        }));
    }
}
} // namespace Afterglow
