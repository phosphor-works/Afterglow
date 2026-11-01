// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScriptPackage.h"
#include "ScriptFiles.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace Afterglow {
namespace {
bool identifier(const QString& name) { return validScriptId(name); }
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
    QString top; ///< the package directory as given, for module file names
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
        QByteArray contents;
        error = readScriptFile(file, limit, contents, root);
        if (!error.isEmpty()) return {};
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
    /// The package's shader packs, the settings it suggests and its pulse names.
    bool shaders(const QString& base, const QString& id, const QJsonObject& manifest)
    {
        static const QStringList Keys{QStringLiteral("background"), QStringLiteral("text"), QStringLiteral("cursor"),
            QStringLiteral("pane"), QStringLiteral("window"), QStringLiteral("overview"), QStringLiteral("tabTransition"),
            QStringLiteral("paneEffects")};
        const auto pulses = manifest.value(QStringLiteral("pulses"));
        if ((!pulses.isUndefined() && !pulses.isObject()) || pulses.toObject().size() > 32) {
            error = QStringLiteral("pulses must name at most 32 effects"); return false;
        }
        const auto named = pulses.toObject();
        for (auto it = named.begin(); it != named.end(); ++it) {
            const double number = it.value().toDouble(-1);
            if (!identifier(it.key()) || number < 4 || number > 255 || number != std::floor(number)) {
                error = QStringLiteral("A pulse name maps to an effect number from 4 to 255"); return false;
            }
            // The built-in names and plain numbers already select effects.
            static const QRegularExpression digits(QStringLiteral("\\A[0-9]+\\z"));
            if (it.key() == QLatin1String("portal") || it.key() == QLatin1String("flash")
                || it.key() == QLatin1String("shake") || digits.match(it.key()).hasMatch()) {
                error = QStringLiteral("Pulse name %1 is reserved").arg(it.key()); return false;
            }
            out.pulses.insert(it.key(), int(number));
        }
        const auto shaders = manifest.value(QStringLiteral("shaders"));
        if (shaders.isUndefined()) return true;
        if (!shaders.isObject()) { error = QStringLiteral("shaders must be an object"); return false; }
        const auto declared = shaders.toObject();
        const auto declaredPath = declared.value(QStringLiteral("path"));
        if (!declaredPath.isUndefined() && !declaredPath.isString()) { error = QStringLiteral("shaders.path must be a string"); return false; }
        const auto directory = path(base, declaredPath.toString(QStringLiteral("shaders")));
        if (!error.isEmpty()) return false;
        if (!QFileInfo(directory).isDir()) { error = QStringLiteral("The shaders path is not a directory"); return false; }
        // Each pack carries the package's name, so packages cannot collide
        // with each other or replace a bundled pack. The shader loader
        // follows symlinked packs, so they pass the same checks.
        for (const auto& pack : QDir(directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (pack.fileName() == QLatin1String("shared")) { // the packs' include directory
                path(directory, pack.fileName());
                if (!error.isEmpty()) return false;
                continue;
            }
            if (pack.fileName() == QLatin1String("none")) {
                error = QStringLiteral("Shader pack name none is reserved"); return false;
            }
            path(directory, pack.fileName());
            const auto metadata = json(path(directory, pack.fileName() + QStringLiteral("/metadata.json")));
            if (!error.isEmpty()) return false;
            const QString expected = id + QLatin1Char('.') + pack.fileName();
            if (!identifier(pack.fileName()) || metadata.value(QStringLiteral("id")).toString() != expected) {
                error = QStringLiteral("Shader pack %1 must declare the id \"%2\"").arg(pack.fileName(), expected); return false;
            }
            out.shaderPacks << pack.fileName();
        }
        if (out.shaderPacks.isEmpty() || out.shaderPacks.size() > 16) {
            error = QStringLiteral("shaders must hold 1 to 16 packs"); return false;
        }
        out.shaderDirectory = directory;
        const auto suggests = declared.value(QStringLiteral("suggests"));
        if (!suggests.isUndefined() && !suggests.isObject()) { error = QStringLiteral("suggests must be an object"); return false; }
        const auto wanted = suggests.toObject();
        for (auto it = wanted.begin(); it != wanted.end(); ++it) {
            if (!Keys.contains(it.key()) || !out.shaderPacks.contains(it.value().toString())) {
                error = QStringLiteral("suggests maps a Shaders key to one of the package's packs"); return false;
            }
            out.suggests.insert(it.key(), it.value().toString());
        }
        return true;
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
        if (!error.isEmpty()) return {}; // the manifest itself could not be read
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
                if (!error.isEmpty()) return {};
                const QFileInfo info(file);
                assetBytes += info.size();
                if (!identifier(it.key()) || !info.isFile() || info.size() > 4 * 1024 * 1024 || assetBytes > 16 * 1024 * 1024) {
                    error = QStringLiteral("Invalid asset or asset size limit exceeded"); return {};
                }
                out.assets.insert(it.key(), file);
            }
            if (!shaders(base, id, manifest)) return {};
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
            // Locals named like the loader's own keep a module from replacing
            // them by mistake. Each module starts on a line of its own, so its
            // errors can be traced back to its file.
            factories += "factories[" + aliases[it.key()] + "]=function(...)\nlocal require=resolver(" + mapping
                + ") local factories, cache, loading, load, resolver\n";
            const int lines = int(source.count('\n')) + (source.endsWith('\n') ? 0 : 1);
            out.modules.append({int(factories.count('\n')) + 1, lines, QDir(top).relativeFilePath(filename)});
            factories += source + "\nend\n";
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
    Builder builder{result, QFileInfo(root).canonicalFilePath(), root, {}, 0, 0, 0, {}, {}};
    const auto entry = builder.package(root, QStringLiteral("root"));
    if (!builder.error.isEmpty()) return builder.error;
    const QByteArray loader = R"(
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
)";
    result.source = loader + builder.factories + "\nreturn load(" + entry + ")\n";
    const int offset = int(loader.count('\n'));
    for (auto& module : result.modules) module.line += offset;
    result.watched.removeDuplicates();
    return {};
}
QString ScriptPackage::locate(const QList<Module>& modules, const QString& message)
{
    // The chunk is named after the manifest; name the module file instead.
    static const QRegularExpression position(QStringLiteral("package\\.json:(\\d+):"));
    QString out;
    qsizetype done = 0;
    for (auto it = position.globalMatch(message); it.hasNext();) {
        const auto match = it.next();
        const int line = match.captured(1).toInt();
        const auto module = std::find_if(modules.cbegin(), modules.cend(),
            [line](const Module& m) { return line >= m.line && line < m.line + m.lines; });
        if (module == modules.cend()) continue;
        out += QStringView(message).mid(done, match.capturedStart() - done);
        out += QStringLiteral("%1:%2:").arg(module->file).arg(line - module->line + 1);
        done = match.capturedEnd();
    }
    return out + QStringView(message).mid(done);
}
}
