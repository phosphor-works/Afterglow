// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Control.h"
#include "../ProjectStore.h"
#include "../TerminalWindow.h"
#include "../config/Settings.h"
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
    if (!find(id, &window)) return QStringLiteral("Pane closed");
    const auto error = projects().save(name, window->projectLayout(), overwrite);
    if (!error.isEmpty()) return error;
    window->setProjectName(name);
    result = {{QStringLiteral("name"), name}, {QStringLiteral("window_id"), window->id()}};
    return {};
}
QString Control::openProject(const QString& name, bool newInstance, QVariantMap& result)
{
    if (!ProjectStore::validName(name)) return QStringLiteral("Invalid project name");
    if (!newInstance) for (const auto& window : m_windows) {
        if (!window || window->projectName() != name || !window->focusedPane()) continue;
        focus(window->focusedPane()->id());
        result = {{QStringLiteral("name"), name}, {QStringLiteral("window_id"), window->id()}, {QStringLiteral("reused"), true}};
        return {};
    }
    QJsonObject document;
    if (const auto error = projects().read(name, document); !error.isEmpty()) return error;
    auto* window = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!window) return QStringLiteral("Cannot create project window");
    window->setProjectName(name);
    result = window->restoreProject(document);
    result.insert(QStringLiteral("name"), name);
    const auto warnings = result.value(QStringLiteral("warnings")).toStringList();
    if (!warnings.isEmpty() && window->focusedPane())
        notify(window->focusedPane()->id(), QStringLiteral("Project opened with warnings"), warnings.join(QLatin1Char('\n')));
    return {};
}

QString Control::openDirectory(const QString& requested, const QString& layout, QVariantMap& result)
{
    QString path = requested;
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/"))) path = QDir::homePath() + path.mid(1);
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null) || path.size() > 4096)
        return QStringLiteral("Project directory must be absolute or start with ~/");
    path = QFileInfo(path).canonicalFilePath();
    if (path.isEmpty() || !QFileInfo(path).isDir() || ::access(QFile::encodeName(path).constData(), X_OK) != 0)
        return QStringLiteral("Project directory is unavailable");
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
    if (!window) return QStringLiteral("Cannot create project window");
    window->setProjectDirectory(path);
    if (layout.isEmpty()) result = {{QStringLiteral("window_id"), window->id()}, {QStringLiteral("reused"), false}};
    else result = window->restoreProject(document);
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
            if (!ProjectStore::validName(name)) { report(QStringLiteral("Invalid name: use 1–64 letters, digits, underscores or hyphens")); return; }
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
                report(openProject(data.value(QStringLiteral("id")).toString(), false, result));
            }));
    }
}
} // namespace Afterglow
