// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProjectStore.h"
#include "scripting/ScriptNames.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <functional>

namespace Afterglow {

namespace {

constexpr qint64 MaxBytes = 1024 * 1024;

bool string(const QJsonValue& value, qsizetype limit = 4096)
{
    return value.isString() && value.toString().size() <= limit && !value.toString().contains(QChar::Null);
}

bool identity(const QJsonValue& value)
{
    return string(value, 64) && !value.toString().isEmpty();
}

/// Why a project would not keep @p command, or nothing. The bytes are those
/// that a session's layout counts (sessionCommand).
QString commandProblem(const QStringList& command)
{
    if (command.isEmpty())
        return {};
    if (command.size() > ProjectStore::MaxCommandArgs)
        return ProjectStore::tr("A pane command holds at most %1 arguments, and this one has %2")
            .arg(ProjectStore::MaxCommandArgs)
            .arg(command.size());
    for (const QString& arg : command) {
        if (arg.contains(QChar::Null) || arg.toUtf8().size() > ProjectStore::MaxCommandArgBytes)
            return ProjectStore::tr("A command argument must be text of at most %L1 bytes")
                .arg(ProjectStore::MaxCommandArgBytes);
    }
    if (command.first().isEmpty())
        return ProjectStore::tr("A pane command starts with an empty program name");
    return {};
}

/// How deep the splits of a tree nest: 0 for a pane. The JSON parser bounds
/// the nesting of a file that a project reads.
int levels(const QJsonValue& value)
{
    const auto node = value.toObject();
    if (!node.contains(QStringLiteral("first")))
        return 0;
    return 1 + std::max(levels(node.value(QStringLiteral("first"))), levels(node.value(QStringLiteral("second"))));
}

/// Checks one split tree, which validate found no deeper than a project
/// allows. Returns why it is refused, or nothing.
QString tree(const QJsonValue& value, QSet<QString>& all, QSet<QString>& local)
{
    if (!value.isObject())
        return ProjectStore::tr("A split tree entry is not an object");
    const auto node = value.toObject();
    if (node.contains(QStringLiteral("pane"))) {
        if (node.size() != 1 || !node.value(QStringLiteral("pane")).isObject())
            return ProjectStore::tr("A pane entry holds only a pane object");
        const auto pane = node.value(QStringLiteral("pane")).toObject();
        const auto id = pane.value(QStringLiteral("id"));
        if (!identity(id))
            return ProjectStore::tr("A pane ID must be text of 1–64 characters");
        if (all.contains(id.toString()))
            return ProjectStore::tr("The pane ID \"%1\" is used more than once").arg(id.toString());
        const auto cwd = pane.value(QStringLiteral("cwd"));
        if (!string(cwd))
            return ProjectStore::tr("A pane directory must be text of at most 4,096 characters");
        const auto path = cwd.toString();
        // Not QDir::isAbsolutePath: it takes ":dir" for a Qt resource.
        if (!path.startsWith(QLatin1Char('/')) && path != QLatin1String("~") && !path.startsWith(QLatin1String("~/")))
            return ProjectStore::tr("A pane directory must be an absolute path or start with ~");
        if (!string(pane.value(QStringLiteral("title")), ProjectStore::MaxTitle))
            return ProjectStore::tr("A pane title must be text of at most %1 characters").arg(ProjectStore::MaxTitle);
        const auto command = pane.value(QStringLiteral("command"));
        if (!command.isArray())
            return ProjectStore::tr("A pane command must be a list of arguments");
        QStringList words;
        for (const auto arg : command.toArray()) {
            if (!arg.isString())
                return ProjectStore::tr("A command argument must be text of at most 4,096 bytes");
            words << arg.toString();
        }
        if (const auto error = commandProblem(words); !error.isEmpty())
            return error;
        all.insert(id.toString());
        local.insert(id.toString());
        return {};
    }
    if (node.size() != 4)
        return ProjectStore::tr("A split holds direction, ratio, first and second");
    const auto direction = node.value(QStringLiteral("direction")).toString();
    if (direction != QLatin1String("right") && direction != QLatin1String("down"))
        return ProjectStore::tr("A split direction must be right or down");
    const auto ratio = node.value(QStringLiteral("ratio"));
    if (!ratio.isDouble() || !std::isfinite(ratio.toDouble()) || ratio.toDouble() <= 0 || ratio.toDouble() >= 1)
        return ProjectStore::tr("A split ratio must be greater than 0 and less than 1");
    if (const auto error = tree(node.value(QStringLiteral("first")), all, local); !error.isEmpty())
        return error;
    return tree(node.value(QStringLiteral("second")), all, local);
}

} // namespace

bool ProjectStore::keepsCommand(const QStringList& command)
{
    return commandProblem(command).isEmpty();
}

QString ProjectStore::directoryBeside(const QString& configPath)
{
    return QDir(QFileInfo(configPath).absolutePath()).filePath(QStringLiteral("projects"));
}

bool ProjectStore::validName(const QString& name)
{
    return Afterglow::validName(name);
}

QString ProjectStore::validate(const QJsonObject& document)
{
    if (document.value(QStringLiteral("version")) != QJsonValue(1))
        return tr("Unsupported project version (expected 1)");
    const auto tabsValue = document.value(QStringLiteral("tabs"));
    if (!tabsValue.isArray() || tabsValue.toArray().isEmpty())
        return tr("A project needs 1–16 tabs");
    // A window saved as a project can hold more than a project does: the
    // error says by how much.
    if (tabsValue.toArray().size() > 16)
        return tr("A project needs 1–16 tabs, and this one has %1").arg(tabsValue.toArray().size());
    QSet<QString> tabs, panes;
    for (const auto value : tabsValue.toArray()) {
        const auto tab = value.toObject();
        const auto id = tab.value(QStringLiteral("id"));
        if (!identity(id))
            return tr("A tab ID must be text of 1–64 characters");
        if (tabs.contains(id.toString()))
            return tr("The tab ID \"%1\" is used more than once").arg(id.toString());
        if (!string(tab.value(QStringLiteral("title")), MaxTitle))
            return tr("A tab title must be text of at most %1 characters").arg(MaxTitle);
        const int depth = levels(tab.value(QStringLiteral("tree")));
        if (depth > 16)
            return tr("A split tree is %1 levels deep, and a project holds at most 16 levels").arg(depth);
        QSet<QString> local;
        if (const auto error = tree(tab.value(QStringLiteral("tree")), panes, local); !error.isEmpty())
            return error;
        if (!local.contains(tab.value(QStringLiteral("active_pane")).toString()))
            return tr("The active pane of the tab \"%1\" is not in its split tree").arg(id.toString());
        tabs.insert(id.toString());
    }
    if (panes.size() > 32)
        return tr("A project holds at most 32 panes, and this one has %1").arg(panes.size());
    if (!tabs.contains(document.value(QStringLiteral("active_tab")).toString()))
        return tr("Invalid active project tab");
    if (document.contains(QStringLiteral("session")) && !document.value(QStringLiteral("session")).isBool())
        return tr("The session of a project is true or false");
    if (QJsonDocument(document).toJson(QJsonDocument::Compact).size() > MaxBytes)
        return tr("Project exceeds 1 MiB");
    return {};
}

ProjectStore::Launch ProjectStore::launchOf(const QJsonObject& pane, QStringList& warnings,
                                            const QStringList& searchPath)
{
    Launch launch;
    const auto savedId = pane.value(QStringLiteral("id")).toString();
    launch.cwd = pane.value(QStringLiteral("cwd")).toString();
    if (launch.cwd == QLatin1String("~"))
        launch.cwd = QDir::homePath();
    else if (launch.cwd.startsWith(QLatin1String("~/")))
        launch.cwd = QDir::homePath() + launch.cwd.mid(1);
    if (!QFileInfo(launch.cwd).isDir()) {
        warnings << tr("Pane %1: directory missing (%2). Using home").arg(savedId, launch.cwd);
        launch.cwd = QDir::homePath();
    }
    for (const auto arg : pane.value(QStringLiteral("command")).toArray())
        launch.command << arg.toString();
    launch.configured = launch.command;
    if (!launch.command.isEmpty()) {
        const auto program = launch.command.first();
        // A relative entry of the PATH is one in the pane's directory, as
        // the pane's program looks for it.
        QStringList directories;
        for (const QString& entry : searchPath)
            directories << QDir(launch.cwd).absoluteFilePath(entry);
        const auto resolved = program.contains(QLatin1Char('/')) ? QDir(launch.cwd).absoluteFilePath(program)
                                                                 : QStandardPaths::findExecutable(program, directories);
        if (resolved.isEmpty() || !QFileInfo(resolved).isFile() || !QFileInfo(resolved).isExecutable()) {
            warnings << tr("Pane %1: command unavailable (%2). Starting a shell").arg(savedId, program);
            launch.command.clear();
        }
    }
    return launch;
}

QList<QJsonObject> ProjectStore::panes(const QJsonObject& document)
{
    QList<QJsonObject> out;
    // A valid tree nests no deeper than validate allows.
    std::function<void(const QJsonObject&)> walk = [&](const QJsonObject& node) {
        if (node.contains(QStringLiteral("pane"))) {
            out.append(node.value(QStringLiteral("pane")).toObject());
            return;
        }
        if (!node.contains(QStringLiteral("first")))
            return;
        walk(node.value(QStringLiteral("first")).toObject());
        walk(node.value(QStringLiteral("second")).toObject());
    };
    for (const QJsonValue& tab : document.value(QStringLiteral("tabs")).toArray())
        walk(tab.toObject().value(QStringLiteral("tree")).toObject());
    return out;
}

bool ProjectStore::opensSession(const QJsonObject& document)
{
    return document.value(QStringLiteral("session")).toBool(false);
}

QStringList ProjectStore::names() const
{
    QStringList result;
    const auto files =
        QDir(m_directory).entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const auto& file : files)
        if (validName(file.completeBaseName()))
            result << file.completeBaseName();
    return result;
}

QString ProjectStore::read(const QString& name, QJsonObject& document) const
{
    if (!validName(name))
        return tr("Use 1–64 letters, digits, underscores or hyphens for the project name");
    const auto path = QDir(m_directory).filePath(name + QStringLiteral(".json"));
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile() || info.size() > MaxBytes)
        return tr("Project is missing, not a regular file, or exceeds 1 MiB");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return file.errorString();
    const auto bytes = file.read(MaxBytes + 1);
    if (bytes.size() > MaxBytes)
        return tr("Project exceeds 1 MiB");
    QJsonParseError parse;
    const auto json = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError)
        return tr("Invalid project JSON: %1").arg(parse.errorString());
    if (!json.isObject())
        return tr("The project file does not hold a JSON object");
    const auto error = validate(json.object());
    if (error.isEmpty())
        document = json.object();
    return error;
}

QString ProjectStore::save(const QString& name, const QJsonObject& document, bool overwrite) const
{
    if (!validName(name))
        return tr("Use 1–64 letters, digits, underscores or hyphens for the project name");
    if (const auto error = validate(document); !error.isEmpty())
        return error;
    if (!QDir().mkpath(m_directory))
        return tr("Cannot create project directory");
    QLockFile lock(QDir(m_directory).filePath(QStringLiteral(".save.lock")));
    if (!lock.tryLock(0))
        return tr("Another project save is in progress");
    const auto path = QDir(m_directory).filePath(name + QStringLiteral(".json"));
    if (QFileInfo(path).isSymLink())
        return tr("Cannot replace a project symlink");
    if (!overwrite && QFileInfo::exists(path))
        return tr("Project already exists. Confirm replacement or choose another name");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return file.errorString();
    const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return file.errorString();
    return {};
}

} // namespace Afterglow
