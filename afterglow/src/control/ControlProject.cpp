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
QString Control::openProject(const QString& name, bool newInstance, const QString& mode, QVariantMap& result)
{
    if (!ProjectStore::validName(name)) return QStringLiteral("invalid project name");
    if (!mode.isEmpty() && mode != QLatin1String("session") && mode != QLatin1String("window"))
        return QStringLiteral("a project opens into a session or a window");
    QJsonObject document;
    if (const auto error = projects().read(name, document); !error.isEmpty()) return error;
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
        return {};
    }
    if (session) return openProjectSession(name, document, newInstance, result);
    auto* window = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!window) return QStringLiteral("cannot open a window");
    window->setProjectName(name);
    result = window->restoreProject(document);
    // A window that restored nothing stays hidden and keeps afterglow running.
    if (const QString error = result.value(QStringLiteral("error")).toString(); !error.isEmpty()) {
        window->deleteLater();
        result.clear();
        return error;
    }
    result.insert(QStringLiteral("name"), name);
    const auto warnings = result.value(QStringLiteral("warnings")).toStringList();
    if (!warnings.isEmpty() && window->focusedPane())
        notify(window->focusedPane()->id(), QStringLiteral("Project opened with warnings"), warnings.join(QLatin1Char('\n')));
    return {};
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

QString Control::openProjectSession(const QString& name, const QJsonObject& document, bool newInstance, QVariantMap& result)
{
    if (!AfterglowMux::validSessionName(name.toStdString()))
        return QStringLiteral("project %1 cannot name a session: the name of a session must be %2").arg(name, QLatin1String(AfterglowMux::SessionNameRule));
    if (!m_openSessionWindow) return QStringLiteral("cannot open a window");
    const auto opened = [&](TerminalWindow* window, const QString& session, bool reused) {
        window->setProjectName(name);
        if (window->focusedPane()) focus(window->focusedPane()->id());
        result.insert(QStringLiteral("name"), name);
        result.insert(QStringLiteral("window_id"), window->id());
        result.insert(QStringLiteral("session"), session);
        result.insert(QStringLiteral("reused"), reused);
    };
    // The session of the project runs: its window shows it, or a new one does.
    const bool runs = localSessionRuns(name);
    if (!newInstance && runs) {
        if (TerminalWindow* shown = sessionWindow(name)) {
            opened(shown, name, true);
            return {};
        }
        if (const QString error = m_openSessionWindow(name, {}); !error.isEmpty()) return error;
        TerminalWindow* window = sessionWindow(name);
        if (!window) return QStringLiteral("no window shows session %1").arg(name);
        opened(window, name, true);
        return {};
    }
    // A new session: the project's first pane starts it, and its window
    // starts the others in it. Another instance gets the daemon's next name,
    // and so does a project whose last session ended while a window here
    // has not heard it yet: that window still shows a session of the name.
    // The restore gives the warnings, the first pane's too.
    QStringList ignored;
    const ProjectStore::Launch first = ProjectStore::launchOf(ProjectStore::firstPane(document), ignored);
    const bool taken = runs || sessionWindow(name);
    QString error;
    const QString session = newLocalSession(localSessionOptions(m_settings.settings(), first.cwd, first.command, taken ? QString() : name), &error);
    if (session.isEmpty()) return error;
    // A session that no window lays out is ended, and the error says so when it cannot be.
    const auto fail = [&session](const QString& why) {
        return killLocalSession(session) ? why : QStringLiteral("%1, and session %2 still runs").arg(why, session);
    };
    if (const QString why = m_openSessionWindow(session, {}); !why.isEmpty()) return fail(why);
    TerminalWindow* window = sessionWindow(session);
    if (!window || !window->focusedPane()) return fail(QStringLiteral("no window shows session %1").arg(session));
    result = window->restoreProject(document, window->focusedPane());
    if (const QString why = result.value(QStringLiteral("error")).toString(); !why.isEmpty()) {
        result.clear();
        // Its end closes the window.
        return fail(why);
    }
    const QStringList warnings = result.value(QStringLiteral("warnings")).toStringList();
    opened(window, session, false);
    if (!warnings.isEmpty() && window->focusedPane())
        notify(window->focusedPane()->id(), QStringLiteral("Project opened with warnings"), warnings.join(QLatin1Char('\n')));
    return {};
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
                QVariantMap result;
                report(openProject(data.value(QStringLiteral("id")).toString(), false, {}, result));
            }));
    }
}
} // namespace Afterglow
