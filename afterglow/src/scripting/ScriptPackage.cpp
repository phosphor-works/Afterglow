// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScriptPackage.h"
#include "ScriptFiles.h"
#include "ScriptState.h"
#include "../shaders/ShaderRefs.h"
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
constexpr int MaxPackages = 16; ///< in one package tree, the package itself included
constexpr int MaxModules = 64; ///< in one package tree
/// The lines of the code that opens each module's factory, before its source.
constexpr int FactoryLines = 2;

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
/// A name from a manifest, for an error, before it was checked: bounded.
QString shown(QString name)
{
    if (name.size() <= 64) return name;
    name.truncate(64);
    if (name.back().isHighSurrogate()) name.chop(1);
    return name + QStringLiteral("…");
}
/// What a command's text must not hold: controls, the line and paragraph
/// separators, and the bidirectional controls. They could break or reorder
/// a help line, a palette entry or the terminal line that shows them.
bool unprintable(QChar c)
{
    const char16_t u = c.unicode();
    return u < 0x20 || (u >= 0x7f && u < 0xa0) || u == 0x061c || u == 0x200e || u == 0x200f || u == 0x2028 || u == 0x2029
        || (u >= 0x202a && u <= 0x202e) || (u >= 0x2066 && u <= 0x2069);
}
struct Builder
{
    ScriptPackage& out;
    QString root; ///< the canonical package directory, which every path stays in
    QString top; ///< the package directory as given, for module file names
    QString error; ///< the first error: it stands
    QString checking; ///< the manifest that is being checked, as its errors name it
    int bytes = 0;
    int modules = 0;
    int packages = 0;
    int lines = 0; ///< the line breaks in factories
    QSet<QString> loading; ///< the canonical directories of the packages that are being read
    QByteArray factories;

    /// An error of a rule of the manifest that is being checked, named by it.
    void refuse(const QString& why) { error = checking + QStringLiteral(": ") + why; }
    QString path(const QString& base, const QString& relative)
    {
        if (!error.isEmpty()) return {};
        const auto clean = QDir::cleanPath(relative);
        if (relative.isEmpty() || QDir::isAbsolutePath(relative) || clean == QLatin1String("..") || clean.startsWith(QLatin1String("../"))) {
            refuse(QStringLiteral("the path %1 must remain inside its package").arg(shown(relative))); return {};
        }
        const auto absolute = QDir(base).absoluteFilePath(clean);
        // Watched also when it is refused: the fix loads the package again.
        out.watched << QFileInfo(absolute).absolutePath() << absolute;
        const auto canonical = QFileInfo(absolute).canonicalFilePath();
        // Also reject symlinked parents escaping the complete package tree.
        if (!canonical.isEmpty() && !canonical.startsWith(root + QLatin1Char('/'))) {
            refuse(QStringLiteral("the path %1 escapes its root").arg(shown(relative))); return {};
        }
        return absolute;
    }
    QByteArray read(const QString& file, int limit)
    {
        if (!error.isEmpty()) return {};
        QByteArray contents;
        // It holds the file to its own limit.
        error = readScriptFile(file, limit, contents, root);
        if (!error.isEmpty()) return {};
        bytes += contents.size();
        if (bytes > 1024 * 1024) error = QStringLiteral("Package exceeds its source limit (1 MiB)");
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
        static const QStringList Keys = ShaderChainKeys + ShaderEventKeys;
        const auto pulses = manifest.value(QStringLiteral("pulses"));
        if ((!pulses.isUndefined() && !pulses.isObject()) || pulses.toObject().size() > 32) {
            refuse(QStringLiteral("pulses must name at most 32 effects")); return false;
        }
        const auto named = pulses.toObject();
        for (auto it = named.begin(); it != named.end(); ++it) {
            const double number = it.value().toDouble(-1);
            if (!validName(it.key()) || number < 4 || number > 255 || number != std::floor(number)) {
                refuse(QStringLiteral("a pulse name maps to an effect number from 4 to 255")); return false;
            }
            // The built-in names and plain numbers already select effects.
            static const QRegularExpression digits(QStringLiteral("\\A[0-9]+\\z"));
            if (it.key() == QLatin1String("portal") || it.key() == QLatin1String("flash")
                || it.key() == QLatin1String("shake") || digits.match(it.key()).hasMatch()) {
                refuse(QStringLiteral("pulse name %1 is reserved").arg(it.key())); return false;
            }
            out.pulses.insert(it.key(), int(number));
        }
        const auto shaders = manifest.value(QStringLiteral("shaders"));
        if (shaders.isUndefined()) return true;
        if (!shaders.isObject()) { refuse(QStringLiteral("shaders must be an object")); return false; }
        const auto declared = shaders.toObject();
        const auto declaredPath = declared.value(QStringLiteral("path"));
        if (!declaredPath.isUndefined() && !declaredPath.isString()) { refuse(QStringLiteral("shaders.path must be a string")); return false; }
        const auto directory = path(base, declaredPath.toString(QStringLiteral("shaders")));
        if (!error.isEmpty()) return false;
        if (!QFileInfo(directory).isDir()) { refuse(QStringLiteral("shaders.path is not a directory")); return false; }
        // Each pack carries the package's name, so packages cannot collide
        // with each other or replace a bundled pack. The shader loader
        // follows symlinked packs, so they pass the same checks.
        for (const auto& pack : QDir(directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QString name = pack.fileName();
            if (name == QLatin1String("shared")) { // the packs' include directory
                path(directory, name);
                if (!error.isEmpty()) return false;
                continue;
            }
            // The name comes first: the pack's id and its reference are made
            // from it. The shader library leaves out the same directories.
            if (!packageShaderPack(name)) {
                refuse(validName(name) ? QStringLiteral("shader pack name none is reserved")
                                       : QStringLiteral("shader pack directory %1 must be 1 to 64 letters, digits, _ and -").arg(shown(name)));
                return false;
            }
            path(directory, name);
            const auto metadata = json(path(directory, name + QStringLiteral("/metadata.json")));
            if (!error.isEmpty()) return false;
            const QString expected = id + QLatin1Char('.') + name;
            if (metadata.value(QStringLiteral("id")).toString() != expected) {
                refuse(QStringLiteral("shader pack %1 must declare the id \"%2\"").arg(name, expected)); return false;
            }
            out.shaderPacks << name;
        }
        if (out.shaderPacks.isEmpty() || out.shaderPacks.size() > 16) {
            refuse(QStringLiteral("shaders must hold 1 to 16 packs")); return false;
        }
        out.shaderDirectory = directory;
        const auto suggests = declared.value(QStringLiteral("suggests"));
        if (!suggests.isUndefined() && !suggests.isObject()) { refuse(QStringLiteral("suggests must be an object")); return false; }
        const auto wanted = suggests.toObject();
        for (auto it = wanted.begin(); it != wanted.end(); ++it) {
            if (!Keys.contains(it.key()) || !out.shaderPacks.contains(it.value().toString())) {
                refuse(QStringLiteral("suggests maps a Shaders key to one of the package's packs")); return false;
            }
            out.suggests.insert(it.key(), it.value().toString());
        }
        return true;
    }
    /// @p alias and @p expected: the name and the version that the
    /// package that depends on this one gives it.
    QByteArray package(const QString& base, const QString& prefix, const QString& alias = {}, const QString& expected = {})
    {
        const QString name = QDir(top).relativeFilePath(QDir(base).filePath(QStringLiteral("package.json")));
        // By where it is: a symlink can lead back into a package that is being read.
        const QString where = QFileInfo(base).canonicalFilePath();
        if (!where.isEmpty() && loading.contains(where)) { error = QStringLiteral("Dependency cycle at %1").arg(name); return {}; }
        if (++packages > MaxPackages) { error = QStringLiteral("A package tree has at most %1 packages").arg(MaxPackages); return {}; }
        loading.insert(where);
        checking = name;
        const auto manifest = json(path(base, QStringLiteral("package.json")));
        const auto id = manifest.value(QStringLiteral("id")).toString();
        const auto release = manifest.value(QStringLiteral("version")).toString();
        const auto entry = manifest.value(QStringLiteral("entry")).toString();
        const auto declared = manifest.value(QStringLiteral("modules"));
        const auto dependencies = manifest.value(QStringLiteral("dependencies"));
        if (!error.isEmpty()) return {}; // the manifest itself could not be read
        // Each rule its own message, named by the manifest that breaks it.
        auto refused = [&](const QString& why) {
            refuse(why);
            return QByteArray();
        };
        if (!validScriptId(id)) return refused(QStringLiteral("id must be %1").arg(ScriptIdRule));
        if (!version(release)) return refused(QStringLiteral("version must be X.Y.Z: three numbers of 1 to 6 digits, with no leading zero"));
        if (!expected.isEmpty() && release != expected)
            return refused(QStringLiteral("version is %1, but the package that depends on it declares %2").arg(release, expected));
        if (manifest.value(QStringLiteral("api_version")) != QJsonValue(1)) return refused(QStringLiteral("api_version must be 1"));
        if (!declared.isObject()) return refused(QStringLiteral("modules must map module names to files"));
        if (!validName(entry)) return refused(QStringLiteral("entry must name a module: 1 to 64 letters, digits, _ and -"));
        if (!dependencies.isUndefined() && !dependencies.isObject())
            return refused(QStringLiteral("dependencies must map package ids to a path and a version"));
        // require("@utils") names the package with the id utils, as a
        // package directory names its package.
        if (!alias.isEmpty() && id != alias)
            return refused(QStringLiteral("id is %1, but the package that depends on it names it %2").arg(id, alias));
        if (prefix == QLatin1String("root")) {
            out.id = id; out.version = release;
            const auto schema = manifest.value(QStringLiteral("state_version"));
            out.stateVersion = schema.isUndefined() ? 1 : schema.toInt(0);
            if (out.stateVersion < 1 || out.stateVersion > ScriptState::MaxVersion
                || (!schema.isUndefined() && schema.toDouble() != out.stateVersion)) {
                return refused(QStringLiteral("state_version must be an integer from 1 to %1").arg(ScriptState::MaxVersion));
            }
            if (manifest.contains(QStringLiteral("config")) && !manifest.value(QStringLiteral("config")).isObject()) {
                return refused(QStringLiteral("config must be an object"));
            }
            out.config = manifest.value(QStringLiteral("config")).toObject().toVariantMap();
            const auto assets = manifest.value(QStringLiteral("assets"));
            if ((!assets.isUndefined() && !assets.isObject()) || assets.toObject().size() > 32) {
                return refused(QStringLiteral("assets must declare at most 32 files"));
            }
            qint64 assetBytes = 0;
            const auto declaredAssets = assets.toObject();
            for (auto it = declaredAssets.begin(); it != declaredAssets.end(); ++it) {
                if (!it.value().isString()) return refused(QStringLiteral("asset %1 must be a path in the package").arg(shown(it.key())));
                const auto file = path(base, it.value().toString());
                if (!error.isEmpty()) return {};
                const QFileInfo info(file);
                assetBytes += info.size();
                if (!validName(it.key()) || !info.isFile() || info.size() > 4 * 1024 * 1024 || assetBytes > 16 * 1024 * 1024) {
                    return refused(QStringLiteral("asset %1 must have a name of 1 to 64 letters, digits, _ and -, and be a file "
                                                  "of at most 4 MiB. All assets together have at most 16 MiB")
                                       .arg(shown(it.key())));
                }
                out.assets.insert(it.key(), file);
            }
            if (!shaders(base, id, manifest)) return {};
            if (const auto commands = manifest.value(QStringLiteral("commands")); !commands.isUndefined()) {
                if (const QString why = readCommands(commands.toVariant(), out.commands); !why.isEmpty()) return refused(why);
            }
            const auto configPath = path(base, QStringLiteral("config.json"));
            if (!error.isEmpty()) return {};
            if (QFileInfo::exists(configPath)) {
                const auto overrides = json(configPath).toVariantMap();
                if (!error.isEmpty()) return {};
                for (auto it = overrides.begin(); it != overrides.end(); ++it) out.config.insert(it.key(), it.value());
            }
        }
        QMap<QString, QByteArray> aliases;
        const auto deps = dependencies.toObject();
        for (auto it = deps.begin(); it != deps.end(); ++it) {
            const auto dep = it.value().toObject();
            if (!validScriptId(it.key()) || !dep.value(QStringLiteral("path")).isString()
                || !version(dep.value(QStringLiteral("version")).toString())) {
                return refused(QStringLiteral("dependency %1 needs a package id as its name, a local path and an exact version")
                                  .arg(shown(it.key())));
            }
            const auto directory = path(base, dep.value(QStringLiteral("path")).toString());
            if (!error.isEmpty()) return {};
            const QByteArray loaded = package(directory, prefix + QLatin1Char('/') + it.key(), it.key(),
                                              dep.value(QStringLiteral("version")).toString());
            // The dependency's own error stands: nothing after it may replace it.
            if (!error.isEmpty()) return {};
            checking = name;
            aliases.insert(QLatin1Char('@') + it.key(), loaded);
        }
        const auto files = declared.toObject();
        if (!files.contains(entry)) return refused(QStringLiteral("the entry module %1 is not in modules").arg(entry));
        for (auto it = files.begin(); it != files.end(); ++it) {
            if (!validName(it.key()) || !it.value().isString())
                return refused(QStringLiteral("module %1 needs a name of 1 to 64 letters, digits, _ and -, and a file").arg(shown(it.key())));
            if (++modules > MaxModules) {
                error = QStringLiteral("A package tree has at most %1 modules").arg(MaxModules); return {};
            }
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
            const QByteArray header = "factories[" + aliases[it.key()] + "]=function(...)\nlocal require=resolver(" + mapping
                + ") local factories, cache, loading, load, resolver\n";
            // Every name in it passed validName() or validScriptId(), and quoted() escapes
            // any byte below 32 as well: no build can add a line to it.
            Q_ASSERT(header.count('\n') == FactoryLines);
            factories += header;
            lines += FactoryLines;
            // Counted in the module alone: counting the whole chunk again for
            // each module would grow with the square of its size.
            const int breaks = int(source.count('\n'));
            out.modules.append({lines + 1, breaks + (source.endsWith('\n') ? 0 : 1), QDir(top).relativeFilePath(filename),
                                factories.size(), source.size()});
            factories += source + "\nend\n";
            lines += breaks + 2;
        }
        loading.remove(where);
        return aliases[entry];
    }
};

/// Where line @p line of the generated chunk comes from, as "file:line", or
/// empty for the loader's own code. The code that opens a module's factory
/// counts as its first line, and the code that closes it as its last.
QString origin(const QList<ScriptPackage::Module>& modules, int line)
{
    for (qsizetype i = 0; i < modules.size(); ++i) {
        const ScriptPackage::Module& m = modules[i];
        if (line >= m.line - FactoryLines && line < m.line) return QStringLiteral("%1:1").arg(m.file);
        if (line >= m.line && line < m.line + m.lines) return QStringLiteral("%1:%2").arg(m.file).arg(line - m.line + 1);
        const bool last = i + 1 == modules.size();
        if (line >= m.line + m.lines && (last || line < modules[i + 1].line - FactoryLines))
            return QStringLiteral("%1:%2").arg(m.file).arg(m.lines);
    }
    return {};
}
}
QString readCommand(const QString& name, const QVariantMap& fields, ScriptCommand& out)
{
    out = {};
    out.name = name;
    // Not from a dash: `aglow run ID --help` must stay an option.
    if (!validScriptId(name))
        return QStringLiteral("Command name %1 must be %2").arg(shown(name), ScriptIdRule);
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        const QString& key = it.key();
        const QVariant& value = it.value();
        if (key == QLatin1String("stdin") || key == QLatin1String("remote")) {
            if (value.typeId() != QMetaType::Bool) return QStringLiteral("Command %1: %2 must be a boolean").arg(name, key);
            (key == QLatin1String("stdin") ? out.input : out.remote) = value.toBool();
            continue;
        }
        QString* text = key == QLatin1String("summary") ? &out.summary
            : key == QLatin1String("usage")             ? &out.usage
            : key == QLatin1String("palette")           ? &out.palette
                                                        : nullptr;
        if (!text) return QStringLiteral("Command %1 has an unknown field %2").arg(name, shown(key));
        const QString string = value.toString();
        if (value.typeId() != QMetaType::QString || string.size() > MaxCommandText || std::any_of(string.begin(), string.end(), unprintable))
            return QStringLiteral("Command %1: %2 must be text of at most %3 UTF-16 units, with no control characters, "
                                  "line or paragraph separators or bidirectional controls")
                .arg(name, key).arg(MaxCommandText);
        *text = string;
    }
    // Blank text would show as an empty help line or palette entry.
    if (out.summary.trimmed().isEmpty()) return QStringLiteral("Command %1 needs a summary").arg(name);
    if (!out.palette.isEmpty() && out.palette.trimmed().isEmpty())
        return QStringLiteral("Command %1: palette must not be blank").arg(name);
    return {};
}

QString readCommands(const QVariant& table, QList<ScriptCommand>& out)
{
    out.clear();
    // An empty Luau table arrives as an empty map.
    if (table.typeId() != QMetaType::QVariantMap) return QStringLiteral("commands must map command names to their fields");
    const QVariantMap commands = table.toMap();
    if (commands.size() > MaxScriptCommands) return QStringLiteral("A script has at most %1 commands").arg(MaxScriptCommands);
    for (auto it = commands.begin(); it != commands.end(); ++it) {
        if (it.value().typeId() != QMetaType::QVariantMap)
            return QStringLiteral("Command %1 must be a table of fields, with a summary").arg(shown(it.key()));
        ScriptCommand command;
        if (const QString error = readCommand(it.key(), it.value().toMap(), command); !error.isEmpty()) {
            out.clear();
            return error;
        }
        out.append(command);
    }
    return {};
}

QString ScriptPackage::read(const QString& manifest, ScriptPackage& result)
{
    result = {};
    const auto root = QFileInfo(manifest).absolutePath();
    const auto canonical = QFileInfo(root).canonicalFilePath();
    // With no root, every path would pass the containment checks.
    if (canonical.isEmpty()) return QStringLiteral("Cannot resolve the package directory %1").arg(root);
    Builder builder{result, canonical, root, {}, {}, 0, 0, 0, 0, {}, {}};
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
    if not ok then error(id .. ": " .. tostring(value), 0) end
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
    for (auto& module : result.modules) {
        module.line += offset;
        module.offset += loader.size();
    }
    result.watched.removeDuplicates();
    return {};
}
QString ScriptPackage::locate(const QList<Module>& modules, const QString& message)
{
    // The chunk is named after the manifest; name the module file instead.
    // Only its own positions: ScriptHost parses each module alone first, so
    // a parse error, which names lines in its text too, comes from the chunk
    // only when the modules together pass a compiler limit, and then it starts
    // with a position this maps. The rest of a script's own error stays as it
    // wrote it.
    static const QRegularExpression position(QStringLiteral("package\\.json:(\\d+):"));
    QString out;
    qsizetype done = 0;
    for (auto it = position.globalMatch(message); it.hasNext();) {
        const auto match = it.next();
        const QString where = origin(modules, match.captured(1).toInt());
        if (where.isEmpty()) continue;
        out += QStringView(message).mid(done, match.capturedStart() - done);
        out += where + QLatin1Char(':');
        done = match.capturedEnd();
    }
    return out + QStringView(message).mid(done);
}
}
