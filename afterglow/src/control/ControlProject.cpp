// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Control.h"
#include "../ProjectStore.h"
#include "../TerminalWindow.h"
#include "../config/Settings.h"
#include "../mux/MuxSessions.h"

#include "MuxProtocol.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <unistd.h>

namespace Afterglow {
ProjectStore Control::projects() const
{
    return ProjectStore(QDir(QFileInfo(m_settings.path()).absolutePath()).filePath(QStringLiteral("projects")));
}
QStringList Control::projectNames() const { return projects().names(); }
QString Control::saveProject(quint32 id, const QString& name, bool overwrite, QVariantMap& result)
{
    TerminalWindow* window = nullptr;
    if (!find(id, &window)) return QStringLiteral("no pane %1").arg(id);
    const auto error = projects().save(name, window->projectLayout(), overwrite);
    if (!error.isEmpty()) return error;
    window->setProjectName(name);
    result = {{QStringLiteral("name"), name}, {QStringLiteral("window_id"), window->id()}};
    return {};
}
void Control::openProject(const QString& name, bool newInstance, const QString& mode, ProjectOpened done)
{
    QVariantMap result;
    const auto answer = [&](const QString& error) { done(error, error.isEmpty() ? result : QVariantMap{}); };
    if (!ProjectStore::validName(name)) return answer(QStringLiteral("invalid project name"));
    if (!mode.isEmpty() && mode != QLatin1String("session") && mode != QLatin1String("window"))
        return answer(QStringLiteral("a project opens into a session or a window"));
    QJsonObject document;
    if (const auto error = projects().read(name, document); !error.isEmpty()) return answer(error);
    // A project whose name names no session opens into a window, unless
    // asked for a session, which then says why not.
    const bool session = mode.isEmpty() ? ProjectStore::opensSession(document) && AfterglowMux::validSessionName(name.toStdString())
                                        : mode == QLatin1String("session");
    // A window of the project, of the kind that it opens into.
    if (!newInstance) for (const auto& window : m_windows) {
        if (!window || window->projectName() != name || !window->focusedPane() || window->showsSession() != session) continue;
        focus(window->focusedPane()->id());
        result = {{QStringLiteral("name"), name}, {QStringLiteral("window_id"), window->id()}, {QStringLiteral("reused"), true}};
        if (session) result.insert(QStringLiteral("session"), window->sessionName());
        return answer({});
    }
    if (session) return openProjectSession(name, document, newInstance, std::move(done));
    auto* window = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!window) return answer(QStringLiteral("cannot open a window"));
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
        notify(window->focusedPane()->id(), QStringLiteral("Project opened with warnings"), warnings.join(QLatin1Char('\n')));
    answer({});
}

template<typename Result>
void Control::offThread(std::function<Result()> work, std::function<void(Result)> then)
{
    // The answer is posted to this object: an object that went first drops
    // it, and m_daemonCalls waits for the work before then.
    m_daemonCalls.start([this, work = std::move(work), then = std::move(then)]() mutable {
        Result result = work();
        QMetaObject::invokeMethod(
            this, [then = std::move(then), result = std::move(result)]() mutable { then(std::move(result)); }, Qt::QueuedConnection);
    });
}

TerminalWindow* Control::sessionWindow(const QString& session) const
{
    for (const auto& window : m_windows) {
        if (!window) continue;
        for (Pane* pane : window->panes()) {
            if (pane->showsSession(session)) return window;
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
    if (!m_openSessionWindow) return done(QStringLiteral("cannot open a window"), {});
    // One open of the project's session at a time: another one waits for it,
    // and then opens the project again, which finds its window. `--new`
    // starts a session of its own.
    if (!newInstance) {
        if (const auto opening = m_projectOpens.find(name); opening != m_projectOpens.end()) {
            opening->append([this, name, done = std::move(done)]() mutable { openProject(name, false, QStringLiteral("session"), std::move(done)); });
            return;
        }
        m_projectOpens.insert(name, {});
    }
    // The name of the session, unless a window here still shows a session
    // of the name, which ended before the window heard it, or another open
    // asks for it: then the daemon names it.
    const bool named = !sessionWindow(name) && !m_namingSessions.contains(name);
    if (named) m_namingSessions.insert(name);
    const auto finish = [this, name, newInstance, named, done = std::move(done)](const QString& error, const QVariantMap& result) {
        if (named) m_namingSessions.remove(name);
        done(error, error.isEmpty() ? result : QVariantMap{});
        if (newInstance) return;
        for (const auto& next : m_projectOpens.take(name)) next();
    };
    // A new session: the project's first pane starts it, and its window
    // starts the others in it. The restore gives the warnings, the first
    // pane's too. The options come from this thread's settings.
    QStringList ignored;
    const ProjectStore::Launch first = ProjectStore::launchOf(ProjectStore::firstPane(document), ignored);
    AfterglowMux::SessionOptions options = localSessionOptions(m_settings.settings(), first.cwd, first.command, {});
    // What the calls read from the process: the GUI thread changes its
    // environment (an activation token).
    const LocalDaemon daemon = localDaemon();
    struct Started
    {
        bool runs = false;
        QString session;
        QString error;
    };
    offThread<Started>(
        [name, newInstance, named, daemon, closing = m_closing, options = std::move(options)]() mutable {
            Started started;
            started.runs = localSessionRuns(daemon, name);
            if ((!newInstance && started.runs) || closing->load()) return started;
            // Another instance gets the daemon's next name, and so does a
            // project whose name another session has.
            options.name = started.runs || !named ? std::string() : name.toStdString();
            started.session = newLocalSession(daemon, options, &started.error);
            // Afterglow quits: no window lays the session out.
            if (!started.session.isEmpty() && closing->load()) killLocalSession(daemon, started.session);
            return started;
        },
        [this, name, document, newInstance, daemon, finish](Started started) {
            const auto opened = [&](TerminalWindow* window, const QString& session, bool reused, QVariantMap result) {
                window->setProjectName(name);
                if (window->focusedPane()) focus(window->focusedPane()->id());
                result.insert(QStringLiteral("name"), name);
                result.insert(QStringLiteral("window_id"), window->id());
                result.insert(QStringLiteral("session"), session);
                result.insert(QStringLiteral("reused"), reused);
                return result;
            };
            // The session of the project runs: its window shows it, or a new one does.
            if (!newInstance && started.runs) {
                TerminalWindow* window = sessionWindow(name);
                if (!window) {
                    if (const QString error = m_openSessionWindow(name, {}); !error.isEmpty()) return finish(error, {});
                    window = sessionWindow(name);
                }
                if (!window) return finish(QStringLiteral("no window shows session %1").arg(name), {});
                return finish({}, opened(window, name, true, {}));
            }
            if (started.session.isEmpty()) return finish(started.error, {});
            const QString session = started.session;
            // A session that no window lays out is ended, and the error says
            // so when it cannot be.
            const auto fail = [this, daemon, session, finish](const QString& why) {
                offThread<bool>([daemon, session] { return killLocalSession(daemon, session); }, [why, session, finish](bool killed) {
                    finish(killed ? why : QStringLiteral("%1, and session %2 still runs").arg(why, session), {});
                });
            };
            if (const QString why = m_openSessionWindow(session, {}); !why.isEmpty()) return fail(why);
            TerminalWindow* window = sessionWindow(session);
            if (!window || !window->focusedPane()) return fail(QStringLiteral("no window shows session %1").arg(session));
            const QVariantMap restored = window->restoreProject(document, window->focusedPane());
            // Its end closes the window.
            if (const QString why = restored.value(QStringLiteral("error")).toString(); !why.isEmpty()) return fail(why);
            const QStringList warnings = restored.value(QStringLiteral("warnings")).toStringList();
            const QVariantMap result = opened(window, session, false, restored);
            if (!warnings.isEmpty() && window->focusedPane())
                notify(window->focusedPane()->id(), QStringLiteral("Project opened with warnings"), warnings.join(QLatin1Char('\n')));
            finish({}, result);
        });
}

QString Control::openDirectory(const QString& requested, const QString& layout, QVariantMap& result)
{
    QString path = requested;
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/"))) path = QDir::homePath() + path.mid(1);
    // Not QDir::isAbsolutePath: it takes ":dir" for a Qt resource.
    if (!path.startsWith(QLatin1Char('/')) || path.contains(QChar::Null) || path.size() > 4096)
        return QStringLiteral("project directory must be absolute or start with ~/");
    path = QFileInfo(path).canonicalFilePath();
    if (path.isEmpty() || !QFileInfo(path).isDir() || ::access(QFile::encodeName(path).constData(), X_OK) != 0)
        return QStringLiteral("project directory is unavailable");
    for (const auto& window : m_windows) {
        if (!window || window->projectDirectory() != path || !window->focusedPane()) continue;
        focus(window->focusedPane()->id());
        result = {{QStringLiteral("window_id"), window->id()}, {QStringLiteral("pane_id"), window->focusedPane()->id()},
                  {QStringLiteral("cwd"), path}, {QStringLiteral("reused"), true}};
        return {};
    }
    QJsonObject document;
    if (!layout.isEmpty()) {
        if (const auto error = projects().read(layout, document); !error.isEmpty()) return error;
        std::function<QJsonObject(QJsonObject)> relocate = [&](QJsonObject node) {
            if (node.contains(QStringLiteral("pane"))) {
                auto pane = node.value(QStringLiteral("pane")).toObject();
                pane.insert(QStringLiteral("cwd"), path);
                node.insert(QStringLiteral("pane"), pane);
            } else for (const auto& key : {QStringLiteral("first"), QStringLiteral("second")}) node.insert(key, relocate(node.value(key).toObject()));
            return node;
        };
        QJsonArray tabs;
        for (const auto& value : document.value(QStringLiteral("tabs")).toArray()) {
            auto tab = value.toObject(); tab.insert(QStringLiteral("tree"), relocate(tab.value(QStringLiteral("tree")).toObject())); tabs.append(tab);
        }
        document.insert(QStringLiteral("tabs"), tabs);
    }
    auto* window = m_newWindow ? m_newWindow(path, {}, !layout.isEmpty()) : nullptr;
    if (!window) return QStringLiteral("cannot open a window");
    window->setProjectDirectory(path);
    if (layout.isEmpty()) result = {{QStringLiteral("window_id"), window->id()}, {QStringLiteral("reused"), false}};
    else result = window->restoreProject(document);
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
        if (!warnings.isEmpty()) notify(window->focusedPane()->id(), QStringLiteral("Project opened with warnings"), warnings.join(QLatin1Char('\n')));
    }
    return {};
}

void Control::projectDialog(Pane* pane, bool save)
{
    TerminalWindow* window = nullptr;
    if (!pane || !find(pane->id(), &window)) return;
    const QPointer<Pane> guard(pane);
    const auto report = [this, guard](const QString& error) {
        if (guard && !error.isEmpty()) notify(guard->id(), QStringLiteral("Project"), error);
    };
    const auto owner = QStringLiteral("@projects");
    if (save) {
        QVariantMap model{{QStringLiteral("kind"), QStringLiteral("prompt")}, {QStringLiteral("title"), QStringLiteral("Save window as project")},
                          {QStringLiteral("description"), QStringLiteral("Letters, digits, underscores or hyphens. Saves tabs, splits, directories and explicit launch commands.")},
                          {QStringLiteral("text"), window->projectName()}};
        report(window->showScriptDialog(owner, pane, model, [this, guard, report, owner](QString error, QVariantMap data) {
            if (!guard || !error.isEmpty()) return;
            const auto name = data.value(QStringLiteral("text")).toString();
            if (!ProjectStore::validName(name)) { report(QStringLiteral("Use 1–64 letters, digits, underscores or hyphens for the project name")); return; }
            const auto write = [this, guard, name, report](bool overwrite) {
                if (!guard) return;
                QVariantMap result;
                const auto error = saveProject(guard->id(), name, overwrite, result);
                if (!error.isEmpty()) report(error);
                else notify(guard->id(), QStringLiteral("Project saved"), name);
            };
            if (!projectNames().contains(name)) { write(false); return; }
            TerminalWindow* current = nullptr;
            if (!find(guard->id(), &current)) return;
            QVariantList items{QVariantMap{{QStringLiteral("id"), QStringLiteral("cancel")}, {QStringLiteral("title"), QStringLiteral("Cancel")}},
                               QVariantMap{{QStringLiteral("id"), QStringLiteral("replace")}, {QStringLiteral("title"), QStringLiteral("Replace saved project")}}};
            report(current->showScriptDialog(owner, guard, {{QStringLiteral("kind"), QStringLiteral("select")},
                {QStringLiteral("title"), QStringLiteral("Replace %1?").arg(name)}, {QStringLiteral("items"), items}},
                [write](QString error, QVariantMap data) { if (error.isEmpty() && data.value(QStringLiteral("id")) == QLatin1String("replace")) write(true); }));
        }));
    } else {
        QVariantList items;
        auto names = projectNames();
        for (const auto& w : m_windows) if (w && !w->projectName().isEmpty() && !names.contains(w->projectName())) names << w->projectName();
        names.sort();
        for (const auto& name : names) {
            bool live = false;
            for (const auto& w : m_windows) if (w && w->projectName() == name && w->focusedPane()) live = true;
            items << QVariantMap{{QStringLiteral("id"), name}, {QStringLiteral("title"), name + (live ? QStringLiteral(" (running)") : QStringLiteral(" (launch saved commands)"))}};
        }
        if (items.isEmpty()) { report(QStringLiteral("No saved projects. Use Save Project first.")); return; }
        report(window->showScriptDialog(owner, pane, {{QStringLiteral("kind"), QStringLiteral("select")},
            {QStringLiteral("title"), QStringLiteral("Switch project")}, {QStringLiteral("items"), items}},
            [this, guard, report](QString error, QVariantMap data) {
                if (!guard || !error.isEmpty()) return;
                openProject(data.value(QStringLiteral("id")).toString(), false, {}, [report](const QString& error, const QVariantMap&) { report(error); });
            }));
    }
}
} // namespace Afterglow
