// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScriptPackage.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <functional>

namespace Afterglow {
namespace {
bool identifier(const QString& name)
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    return re.match(name).hasMatch();
}
bool version(const QString& name)
{
    static const QRegularExpression re(QStringLiteral("\\A(0|[1-9][0-9]{0,5})\\.(0|[1-9][0-9]{0,5})\\.(0|[1-9][0-9]{0,5})\\z"));
    return re.match(name).hasMatch();
}
QByteArray quoted(const QString& value)
{
    QByteArray result("\"");
    for (const unsigned char c : value.toUtf8()) {
        if (c >= 32 && c < 127 && c != '\\' && c != '"') result += char(c);
        else result += '\\' + QByteArray::number(c).rightJustified(3, '0');
    }
    return result + '"';
}
struct Builder
{
    ScriptPackage& out;
    QString root;
    QString error;
    int bytes = 0;
    int modules = 0;
    int packages = 0;
    QSet<QString> loading;
    QByteArray factories;

    QString path(const QString& base, const QString& relative)
    {
        const auto clean = QDir::cleanPath(relative);
        if (relative.isEmpty() || QDir::isAbsolutePath(relative) || clean == QLatin1String("..") || clean.startsWith(QLatin1String("../"))) {
            error = QStringLiteral("Package paths must remain inside their package"); return {};
        }
        const auto absolute = QDir(base).absoluteFilePath(clean);
        const auto canonical = QFileInfo(absolute).canonicalFilePath();
        // Also reject symlinked parents escaping the complete package tree.
        if (!canonical.isEmpty() && !canonical.startsWith(root + QLatin1Char('/'))) {
            error = QStringLiteral("Package path escapes its root"); return {};
        }
        out.watched << QFileInfo(absolute).absolutePath() << absolute;
        return absolute;
    }
    QByteArray read(const QString& file, int limit)
    {
        if (!error.isEmpty()) return {};
        const QFileInfo info(file);
        if (!info.isFile() || info.size() > limit) { error = QStringLiteral("Missing or oversized package file: %1").arg(file); return {}; }
        QFile input(file);
        if (!input.open(QIODevice::ReadOnly)) { error = input.errorString(); return {}; }
        auto contents = input.read(limit + 1);
        bytes += contents.size();
        if (contents.size() > limit || bytes > 1024 * 1024) error = QStringLiteral("Package exceeds its source limit (1 MiB)");
        return contents;
    }
    QJsonObject json(const QString& file)
    {
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(read(file, 65536), &parse);
        if (error.isEmpty() && (parse.error != QJsonParseError::NoError || !doc.isObject())) error = QStringLiteral("Invalid package JSON: %1").arg(file);
        return doc.object();
    }
    QByteArray package(const QString& base, const QString& prefix, const QString& expected = {})
    {
        if (++packages > 16 || loading.contains(base)) { error = QStringLiteral("Too many dependencies or dependency cycle"); return {}; }
        loading.insert(base);
        const auto manifest = json(path(base, QStringLiteral("package.json")));
        const auto id = manifest.value(QStringLiteral("id")).toString();
        const auto release = manifest.value(QStringLiteral("version")).toString();
        const auto entry = manifest.value(QStringLiteral("entry")).toString();
        const auto declared = manifest.value(QStringLiteral("modules"));
        const auto dependencies = manifest.value(QStringLiteral("dependencies"));
        if (!identifier(id) || !version(release) || (!expected.isEmpty() && release != expected)
            || manifest.value(QStringLiteral("api_version")) != QJsonValue(1) || !identifier(entry) || !declared.isObject()
            || (dependencies.isUndefined() ? false : !dependencies.isObject())) {
            error = QStringLiteral("Invalid manifest, incompatible API or dependency version: %1").arg(base); return {};
        }
        if (prefix == QLatin1String("root")) {
            out.id = id; out.version = release;
            const auto schema = manifest.value(QStringLiteral("state_version"));
            out.stateVersion = schema.isUndefined() ? 1 : schema.toInt(0);
            if (out.stateVersion < 1 || out.stateVersion > 10000 || (!schema.isUndefined() && schema.toDouble() != out.stateVersion)) {
                error = QStringLiteral("state_version must be an integer from 1 to 10000"); return {};
            }
            if (manifest.contains(QStringLiteral("config")) && !manifest.value(QStringLiteral("config")).isObject()) {
                error = QStringLiteral("config must be an object"); return {};
            }
            out.config = manifest.value(QStringLiteral("config")).toObject().toVariantMap();
            const auto assets = manifest.value(QStringLiteral("assets"));
            if ((!assets.isUndefined() && !assets.isObject()) || assets.toObject().size() > 32) {
                error = QStringLiteral("assets must declare at most 32 files"); return {};
            }
            qint64 assetBytes = 0;
            const auto declaredAssets = assets.toObject();
            for (auto it = declaredAssets.begin(); it != declaredAssets.end(); ++it) {
                const auto file = path(base, it.value().toString());
                const QFileInfo info(file);
                assetBytes += info.size();
                if (!error.isEmpty() || !identifier(it.key()) || !info.isFile() || info.size() > 4 * 1024 * 1024 || assetBytes > 16 * 1024 * 1024) {
                    error = QStringLiteral("Invalid asset or asset size limit exceeded"); return {};
                }
                out.assets.insert(it.key(), file);
            }
            const auto configPath = path(base, QStringLiteral("config.json"));
            if (QFileInfo::exists(configPath)) {
                const auto overrides = json(configPath).toVariantMap();
                for (auto it = overrides.begin(); it != overrides.end(); ++it) out.config.insert(it.key(), it.value());
            }
        }
        QMap<QString, QByteArray> aliases;
        const auto deps = dependencies.toObject();
        for (auto it = deps.begin(); it != deps.end(); ++it) {
            const auto dep = it.value().toObject();
            if (!identifier(it.key()) || !version(dep.value(QStringLiteral("version")).toString())) {
                error = QStringLiteral("Dependency requires a name, local path and exact version"); return {};
            }
            const auto directory = path(base, dep.value(QStringLiteral("path")).toString());
            if (!error.isEmpty()) return {};
            aliases.insert(QLatin1Char('@') + it.key(), package(directory, prefix + QLatin1Char('/') + it.key(), dep.value(QStringLiteral("version")).toString()));
        }
        const auto files = declared.toObject();
        if (!files.contains(entry)) { error = QStringLiteral("Entry module is not declared"); return {}; }
        for (auto it = files.begin(); it != files.end(); ++it) {
            if (!identifier(it.key()) || !it.value().isString() || ++modules > 64) { error = QStringLiteral("Invalid module name or too many modules (64)"); return {}; }
            aliases.insert(it.key(), quoted(prefix + QLatin1Char(':') + it.key()));
        }
        QByteArray mapping("{");
        for (auto it = aliases.begin(); it != aliases.end(); ++it) mapping += '[' + quoted(it.key()) + "]=" + it.value() + ',';
        mapping += '}';
        for (auto it = files.begin(); it != files.end(); ++it) {
            const auto filename = path(base, it.value().toString());
            const auto source = read(filename, 256 * 1024);
            if (!error.isEmpty()) return {};
            factories += "factories[" + aliases[it.key()] + "]=function()\nlocal require=resolver(" + mapping + ")\n" + source + "\nend\n";
        }
        loading.remove(base);
        return aliases[entry];
    }
};
}
QString ScriptPackage::read(const QString& manifest, ScriptPackage& result)
{
    result = {};
    const auto root = QFileInfo(manifest).absolutePath();
    Builder builder{result, QFileInfo(root).canonicalFilePath(), {}, 0, 0, 0, {}, {}};
    const auto entry = builder.package(root, QStringLiteral("root"));
    if (!builder.error.isEmpty()) return builder.error;
    result.source = R"(
local factories, cache, loading = {}, {}, {}
local function load(id)
    if cache[id] ~= nil then return cache[id] end
    assert(not loading[id], "Module dependency cycle: " .. id)
    assert(factories[id], "Unknown module: " .. id)
    loading[id] = true
    local ok, value = pcall(factories[id])
    loading[id] = nil
    if not ok then error(id .. ": " .. tostring(value)) end
    assert(type(value) == "table", "Module must return a table: " .. id)
    cache[id] = value
    return value
end
local function resolver(aliases)
    return function(name)
        assert(type(name) == "string" and aliases[name], "Undeclared module: " .. tostring(name))
        return load(aliases[name])
    end
end
)" + builder.factories + "\nreturn load(" + entry + ")\n";
    result.watched.removeDuplicates();
    return {};
}
}
