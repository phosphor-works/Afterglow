// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScriptState.h"
#include "ScriptFiles.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <cmath>
#include <limits>

namespace Afterglow {
namespace {
constexpr int MaxBytes = 256 * 1024;
bool validId(const QString& id)
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    return re.match(id).hasMatch();
}
bool validValue(const QVariant& value, int depth, int& count)
{
    if (depth > 16 || ++count > 8192) return false;
    switch (value.typeId()) {
    case QMetaType::QVariantMap: {
        const auto map = value.toMap();
        for (auto it = map.begin(); it != map.end(); ++it)
            if (it.key().size() > 256 || it.key().contains(QChar::Null) || !validValue(it.value(), depth + 1, count)) return false;
        return true;
    }
    case QMetaType::QVariantList:
        for (const auto& item : value.toList()) if (!validValue(item, depth + 1, count)) return false;
        return true;
    case QMetaType::QString: return value.toString().size() <= 65536;
    case QMetaType::Bool: return true;
    case QMetaType::Int: case QMetaType::LongLong: case QMetaType::Double:
        return std::isfinite(value.toDouble()) && std::abs(value.toDouble()) <= 9007199254740991.;
    default: return false;
    }
}
}
QString ScriptState::validate(const QVariantMap& value)
{
    int count = 0;
    if (!validValue(value, 0, count)) return QStringLiteral("State needs JSON-compatible values within depth/size limits");
    if (QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact).size() > MaxBytes - 1024)
        return QStringLiteral("State exceeds 256 KiB");
    return {};
}
QString ScriptState::read(const QString& id, Snapshot& result) const
{
    if (!validId(id)) return QStringLiteral("Invalid script identity");
    const auto path = QDir(m_directory).filePath(id + QStringLiteral(".json"));
    const QFileInfo info(path);
    if (info.isSymLink()) return QStringLiteral("State symlinks are not supported");
    if (!info.exists()) { result = {}; return {}; }
    if (!info.isFile() || info.size() > MaxBytes) return QStringLiteral("State is not a regular bounded file");
    QByteArray bytes;
    if (const auto error = readScriptFile(path, MaxBytes, bytes, {}, true); !error.isEmpty()) return error;
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    const auto object = document.object();
    const auto schema = object.value(QStringLiteral("version"));
    const auto revision = object.value(QStringLiteral("revision"));
    if (parse.error != QJsonParseError::NoError || object.value(QStringLiteral("format")) != QJsonValue(1)
        || !object.value(QStringLiteral("value")).isObject() || schema.toInt() < 1 || schema.toInt() > 10000
        || schema.toDouble() != schema.toInt() || revision.toInt() < 1 || revision.toDouble() != revision.toInt())
        return QStringLiteral("Invalid state file. Original file preserved");
    const auto value = object.value(QStringLiteral("value")).toObject().toVariantMap();
    if (const auto error = validate(value); !error.isEmpty()) return error;
    result = {schema.toInt(), revision.toInt(), value};
    return {};
}
QString ScriptState::write(const QString& id, int version, int expectedRevision, const QVariantMap& value, Snapshot& result) const
{
    if (!validId(id) || version < 1 || version > 10000 || expectedRevision < 0 || expectedRevision == std::numeric_limits<int>::max())
        return QStringLiteral("Invalid state identity/version/revision");
    if (const auto error = validate(value); !error.isEmpty()) return error;
    if (!QDir().mkpath(m_directory)) return QStringLiteral("Cannot create state directory");
    QLockFile lock(QDir(m_directory).filePath(id + QStringLiteral(".lock")));
    if (!lock.tryLock(0)) return QStringLiteral("State is busy. Retry after reading it again");
    Snapshot current;
    if (const auto error = read(id, current); !error.isEmpty()) return error;
    if (current.revision != expectedRevision) return QStringLiteral("State revision conflict. Read again before saving");
    if (current.version > version) return QStringLiteral("Cannot overwrite a newer state version");
    QSaveFile file(QDir(m_directory).filePath(id + QStringLiteral(".json")));
    if (!file.open(QIODevice::WriteOnly)) return file.errorString();
    const QJsonObject object{{QStringLiteral("format"), 1}, {QStringLiteral("version"), version},
                             {QStringLiteral("revision"), current.revision + 1}, {QStringLiteral("value"), QJsonObject::fromVariantMap(value)}};
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) return file.errorString();
    result = {version, current.revision + 1, value};
    return {};
}
}
