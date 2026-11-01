// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProjectStore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <cmath>

namespace Afterglow {
namespace {
constexpr qint64 MaxBytes = 1024 * 1024;
bool string(const QJsonValue& value, int limit = 4096)
{
    return value.isString() && value.toString().size() <= limit && !value.toString().contains(QChar::Null);
}
bool identity(const QJsonValue& value)
{
    return string(value, 64) && !value.toString().isEmpty();
}
/// Checks one split tree. Returns why it is refused, or nothing.
QString tree(const QJsonValue& value, QSet<QString>& all, QSet<QString>& local, int depth)
{
    if (depth > 16) return ProjectStore::tr("A split tree is more than 16 levels deep");
    if (!value.isObject()) return ProjectStore::tr("A split tree entry is not an object");
    const auto node = value.toObject();
    if (node.contains(QStringLiteral("pane"))) {
        if (node.size() != 1 || !node.value(QStringLiteral("pane")).isObject())
            return ProjectStore::tr("A pane entry holds only a pane object");
        const auto pane = node.value(QStringLiteral("pane")).toObject();
        const auto id = pane.value(QStringLiteral("id"));
        if (!identity(id)) return ProjectStore::tr("A pane ID must be text of 1–64 characters");
        if (all.contains(id.toString())) return ProjectStore::tr("The pane ID \"%1\" is used more than once").arg(id.toString());
        if (all.size() >= 32) return ProjectStore::tr("A project holds at most 32 panes");
        const auto cwd = pane.value(QStringLiteral("cwd"));
        if (!string(cwd)) return ProjectStore::tr("A pane directory must be text of at most 4,096 characters");
        const auto path = cwd.toString();
        if (!QDir::isAbsolutePath(path) && path != QLatin1String("~") && !path.startsWith(QLatin1String("~/")))
            return ProjectStore::tr("A pane directory must be an absolute path or start with ~");
        if (!string(pane.value(QStringLiteral("title")), 256)) return ProjectStore::tr("A pane title must be text of at most 256 characters");
        const auto command = pane.value(QStringLiteral("command"));
        if (!command.isArray()) return ProjectStore::tr("A pane command must be a list of arguments");
        if (command.toArray().size() > 64) return ProjectStore::tr("A pane command has more than 64 arguments");
        for (const auto arg : command.toArray())
            if (!string(arg)) return ProjectStore::tr("A command argument must be text of at most 4,096 characters");
        if (!command.toArray().isEmpty() && command.toArray().first().toString().isEmpty())
            return ProjectStore::tr("A pane command starts with an empty program name");
        all.insert(id.toString()); local.insert(id.toString());
        return {};
    }
    if (node.size() != 4) return ProjectStore::tr("A split holds direction, ratio, first and second");
    const auto direction = node.value(QStringLiteral("direction")).toString();
    if (direction != QLatin1String("right") && direction != QLatin1String("down"))
        return ProjectStore::tr("A split direction must be right or down");
    const auto ratio = node.value(QStringLiteral("ratio"));
    if (!ratio.isDouble() || !std::isfinite(ratio.toDouble()) || ratio.toDouble() <= 0 || ratio.toDouble() >= 1)
        return ProjectStore::tr("A split ratio must be greater than 0 and less than 1");
    if (const auto error = tree(node.value(QStringLiteral("first")), all, local, depth + 1); !error.isEmpty()) return error;
    return tree(node.value(QStringLiteral("second")), all, local, depth + 1);
}
}
bool ProjectStore::validName(const QString& name)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    return pattern.match(name).hasMatch();
}
QString ProjectStore::validate(const QJsonObject& document)
{
    if (document.value(QStringLiteral("version")) != QJsonValue(1)) return tr("Unsupported project version (expected 1)");
    const auto tabsValue = document.value(QStringLiteral("tabs"));
    if (!tabsValue.isArray() || tabsValue.toArray().isEmpty() || tabsValue.toArray().size() > 16)
        return tr("A project needs 1–16 tabs");
    QSet<QString> tabs, panes;
    for (const auto value : tabsValue.toArray()) {
        const auto tab = value.toObject();
        const auto id = tab.value(QStringLiteral("id"));
        if (!identity(id)) return tr("A tab ID must be text of 1–64 characters");
        if (tabs.contains(id.toString())) return tr("The tab ID \"%1\" is used more than once").arg(id.toString());
        if (!string(tab.value(QStringLiteral("title")), 256)) return tr("A tab title must be text of at most 256 characters");
        QSet<QString> local;
        if (const auto error = tree(tab.value(QStringLiteral("tree")), panes, local, 0); !error.isEmpty()) return error;
        if (!local.contains(tab.value(QStringLiteral("active_pane")).toString()))
            return tr("The active pane of the tab \"%1\" is not in its split tree").arg(id.toString());
        tabs.insert(id.toString());
    }
    if (!tabs.contains(document.value(QStringLiteral("active_tab")).toString())) return tr("Invalid active project tab");
    if (QJsonDocument(document).toJson(QJsonDocument::Compact).size() > MaxBytes) return tr("Project exceeds 1 MiB");
    return {};
}
QStringList ProjectStore::names() const
{
    QStringList result;
    const auto files = QDir(m_directory).entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const auto& file : files) if (validName(file.completeBaseName())) result << file.completeBaseName();
    return result;
}
QString ProjectStore::read(const QString& name, QJsonObject& document) const
{
    if (!validName(name)) return tr("Use 1–64 letters, digits, underscores or hyphens for the project name");
    const auto path = QDir(m_directory).filePath(name + QStringLiteral(".json"));
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile() || info.size() > MaxBytes) return tr("Project is missing, not a regular file, or exceeds 1 MiB");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return file.errorString();
    const auto bytes = file.read(MaxBytes + 1);
    if (bytes.size() > MaxBytes) return tr("Project exceeds 1 MiB");
    QJsonParseError parse;
    const auto json = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError) return tr("Invalid project JSON: %1").arg(parse.errorString());
    if (!json.isObject()) return tr("The project file does not hold a JSON object");
    const auto error = validate(json.object());
    if (error.isEmpty()) document = json.object();
    return error;
}
QString ProjectStore::save(const QString& name, const QJsonObject& document, bool overwrite) const
{
    if (!validName(name)) return tr("Use 1–64 letters, digits, underscores or hyphens for the project name");
    if (const auto error = validate(document); !error.isEmpty()) return error;
    if (!QDir().mkpath(m_directory)) return tr("Cannot create project directory");
    QLockFile lock(QDir(m_directory).filePath(QStringLiteral(".save.lock")));
    if (!lock.tryLock(0)) return tr("Another project save is in progress");
    const auto path = QDir(m_directory).filePath(name + QStringLiteral(".json"));
    if (QFileInfo(path).isSymLink()) return tr("Cannot replace a project symlink");
    if (!overwrite && QFileInfo::exists(path)) return tr("Project already exists. Confirm replacement or choose another name");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return file.errorString();
    const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) return file.errorString();
    return {};
}
} // namespace Afterglow
