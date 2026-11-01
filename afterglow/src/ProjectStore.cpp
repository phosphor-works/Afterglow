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
bool tree(const QJsonValue& value, QSet<QString>& all, QSet<QString>& local, int depth)
{
    if (!value.isObject() || depth > 16) return false;
    const auto node = value.toObject();
    if (node.contains(QStringLiteral("pane"))) {
        if (node.size() != 1 || !node.value(QStringLiteral("pane")).isObject()) return false;
        const auto pane = node.value(QStringLiteral("pane")).toObject();
        const auto id = pane.value(QStringLiteral("id"));
        const auto cwd = pane.value(QStringLiteral("cwd"));
        if (!identity(id) || all.contains(id.toString()) || all.size() >= 32 || !string(cwd)
            || !string(pane.value(QStringLiteral("title")), 256)) return false;
        const auto path = cwd.toString();
        if (!QDir::isAbsolutePath(path) && path != QLatin1String("~") && !path.startsWith(QLatin1String("~/"))) return false;
        const auto command = pane.value(QStringLiteral("command"));
        if (!command.isArray() || command.toArray().size() > 64) return false;
        for (const auto arg : command.toArray()) if (!string(arg)) return false;
        if (!command.toArray().isEmpty() && command.toArray().first().toString().isEmpty()) return false;
        all.insert(id.toString()); local.insert(id.toString());
        return true;
    }
    const auto direction = node.value(QStringLiteral("direction")).toString();
    const auto ratio = node.value(QStringLiteral("ratio"));
    return node.size() == 4 && (direction == QLatin1String("right") || direction == QLatin1String("down"))
        && ratio.isDouble() && std::isfinite(ratio.toDouble()) && ratio.toDouble() > 0 && ratio.toDouble() < 1
        && tree(node.value(QStringLiteral("first")), all, local, depth + 1)
        && tree(node.value(QStringLiteral("second")), all, local, depth + 1);
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
        QSet<QString> local;
        if (!identity(id) || tabs.contains(id.toString()) || !string(tab.value(QStringLiteral("title")), 256)
            || !tree(tab.value(QStringLiteral("tree")), panes, local, 0)
            || !local.contains(tab.value(QStringLiteral("active_pane")).toString()))
            return tr("Invalid tab or split tree, a duplicate ID, or more than 32 panes or depth 16");
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
