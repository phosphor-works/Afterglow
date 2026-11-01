// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderLibrary.h"

#include "app/BuildTree.h"

#include <PhosphorAnimation/CurveRegistry.h>
#include <PhosphorFsLoader/ContainedFileReader.h>
#include <PhosphorShaders/ShaderIncludeResolver.h>
#include <PhosphorShaders/ShaderRegistry.h>
#include <PhosphorShaders/ShaderPresetStore.h>

#include <QBuffer>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QImageReader>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

Q_LOGGING_CATEGORY(lcShaders, "afterglow.shaders", QtInfoMsg)

namespace Afterglow {

namespace {

using PhosphorFsLoader::AbsolutePathPolicy;
using PhosphorShaders::ShaderIncludeResolver;

/// At most @p limit bytes of @p path; false if it cannot be read or is longer.
bool readCapped(const QString& path, qint64 limit, QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    bytes = file.read(limit + 1);
    return bytes.size() <= limit;
}

// Mirror the resolver's include search, retaining missing candidates as well
// as resolved paths so a broken shader can recover without a config change.
void collectIncludes(const QString& source, const QString& dir, const QStringList& includeDirs,
                     QStringList& watched, QSet<QString>& seen, int depth = 0)
{
    if (depth > ShaderIncludeResolver::MaxIncludeDepth) return;
    static const QRegularExpression pattern(QStringLiteral(R"(^\s*#include\s+(["<])([^">]+)[">]\s*$)"));
    for (const auto& line : source.split(QLatin1Char('\n'))) {
        if (!line.contains(QLatin1String("#include"))) continue;
        const auto match = pattern.match(line);
        if (!match.hasMatch()) continue;
        const auto name = match.captured(2).trimmed();
        // The resolver refuses absolute names, so a pack cannot read any file it likes.
        if (name.isEmpty() || QDir::isAbsolutePath(name)
            || QFileInfo(name).fileName() == QLatin1String(ShaderIncludeResolver::GeneratedPreambleInclude)) continue;
        QStringList search;
        if (match.captured(1) == QLatin1String("\"")) search << dir;
        search << includeDirs;
        for (const auto& location : search) {
            if (location.isEmpty()) continue;
            // The contract is built in and never changes; a hit there shadows the disk.
            if (location.startsWith(QLatin1Char(':'))) {
                if (QFileInfo(QDir(location).filePath(name)).isFile()) break;
                continue;
            }
            const auto candidate = QDir::cleanPath(QDir(location).absoluteFilePath(name));
            watched << candidate;
            const QFileInfo info(candidate);
            if (!info.exists() || !info.isFile()) continue;
            const auto resolved = info.canonicalFilePath();
            watched << resolved;
            if (!seen.contains(resolved)) {
                seen.insert(resolved);
                // The resolver reports an oversized include; only watch it here.
                QByteArray bytes;
                if (readCapped(resolved, ShaderIncludeResolver::MaxIncludeBytes, bytes))
                    collectIncludes(QString::fromUtf8(bytes), QFileInfo(resolved).absolutePath(), includeDirs, watched, seen, depth + 1);
            }
            break;
        }
    }
}

/// Records what a stage includes, resolved or not yet created, for watching.
/// The renderer completes and expands the stage itself.
void watchIncludes(const QByteArray& code, const QString& path, const QStringList& includes, QStringList& watched)
{
    QSet<QString> seen;
    collectIncludes(QString::fromUtf8(code), QFileInfo(path).absolutePath(),
                    QStringList{AfterglowRender::ShaderProgram::contractDirectory()} + includes, watched, seen);
}

QString readStage(const QString& path, const QStringList& includes, QStringList& watched, QByteArray& code)
{
    watched << path;
    // Package packs are third-party files, read on the GUI thread.
    if (!readCapped(path, ShaderIncludeResolver::MaxIncludeBytes, code))
        return QStringLiteral("Cannot read shader, or it exceeds %1 KiB: %2")
            .arg(ShaderIncludeResolver::MaxIncludeBytes / 1024)
            .arg(path);
    watchIncludes(code, path, includes, watched);
    return {};
}

/// An image parameter's file. Under Reject (pack files, scripts, presets) the
/// path must stay inside the pack; under Trust (the user's own config) an
/// absolute or ~/ path may point anywhere.
QString loadImageParameter(const QString& pack, const QString& value, AbsolutePathPolicy policy, QImage& image,
                           QString& resolved)
{
    if (value.isEmpty()) { image = {}; resolved.clear(); return {}; }
    const bool trusted = policy == AbsolutePathPolicy::Trust;
    const auto path = trusted && value.startsWith(QLatin1String("~/")) ? QDir::homePath() + value.mid(1) : value;
    // Watched even when unreadable, so fixing the file recovers the shader.
    resolved = QFileInfo(QDir(pack).absoluteFilePath(path)).absoluteFilePath();
    constexpr qint64 MaxBytes = 16 * 1024 * 1024;
    QString error;
    const auto file = PhosphorFsLoader::readFileWithinDirectory(path, pack, policy, MaxBytes, &error);
    if (!file) {
        return trusted ? QStringLiteral("Image must be a regular file of at most 16 MiB, inside the pack when relative: %1").arg(error)
                       : QStringLiteral("Image must be a regular file of at most 16 MiB inside the pack: %1").arg(error);
    }
    auto bytes = file->bytes;
    QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto size = reader.size();
    if (size.isEmpty() || size.width() > 4096 || size.height() > 4096
        || qint64(size.width()) * size.height() > 4 * 1024 * 1024)
        return QStringLiteral("Image must fit 4096 pixels per side and 4 megapixels");
    image = reader.read();
    if (image.isNull() || image.size() != size) return QStringLiteral("Cannot decode image: %1").arg(value);
    image = image.convertToFormat(QImage::Format_RGBA8888);
    return image.isNull() ? QStringLiteral("Cannot allocate image") : QString();
}

QString readGeometryAsset(const QString& pack, const QString& name, QByteArray& bytes, QString& path)
{
    path = QDir(pack).absoluteFilePath(name);
    QString error;
    const auto file = PhosphorFsLoader::readFileWithinDirectory(name, pack, PhosphorFsLoader::AbsolutePathPolicy::Reject,
                                                                32 * 1024 * 1024, &error);
    if (!file) return QStringLiteral("Geometry assets must be regular files inside the pack, at most 32 MiB: %1").arg(error);
    bytes = file->bytes;
    return {};
}

/// A mesh pass's `objects`: each object's meshes are appended to the pass's
/// vertices. Transform components are numbers or "$id" for a scalar parameter.
QString loadSceneObjects(const QString& pack, const QJsonArray& list, const QHash<QString, int>& scalarSlots,
                         AfterglowRender::ShaderGeometry& geometry, QStringList& watched)
{
    if (list.isEmpty() || list.size() > AfterglowRender::MaxSceneObjects)
        return QStringLiteral("objects needs 1 to %1 entries").arg(AfterglowRender::MaxSceneObjects);
    QHash<QString, int> ids;
    for (int i = 0; i < list.size(); ++i) {
        if (!list[i].isObject()) return QStringLiteral("Scene object %1 must be an object").arg(i);
        const auto entry = list[i].toObject();
        for (auto it = entry.begin(); it != entry.end(); ++it)
            if (!QStringList{QStringLiteral("id"), QStringLiteral("parent"), QStringLiteral("mesh"), QStringLiteral("translate"),
                             QStringLiteral("rotate"), QStringLiteral("scale"), QStringLiteral("spin")}.contains(it.key()))
                return QStringLiteral("Unknown scene object setting: %1").arg(it.key());
        const QString id = entry.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || ids.contains(id)) return QStringLiteral("Scene object %1 needs a unique id").arg(i);
        AfterglowRender::ShaderSceneObject object;
        if (entry.contains(QStringLiteral("parent"))) {
            const QString parent = entry.value(QStringLiteral("parent")).toString();
            if (!ids.contains(parent)) return QStringLiteral("Scene object %1 needs an earlier parent").arg(id);
            object.parent = ids.value(parent);
        }
        auto value = [&](const QJsonValue& v, AfterglowRender::ShaderSceneValue& out) {
            if (v.isDouble() && std::isfinite(v.toDouble())) { out = {float(v.toDouble()), -1}; return true; }
            const QString text = v.toString();
            if (!text.startsWith(QLatin1Char('$')) || !scalarSlots.contains(text.mid(1))) return false;
            out = {0.0f, scalarSlots.value(text.mid(1))};
            return true;
        };
        auto vector = [&](const char* key, std::array<AfterglowRender::ShaderSceneValue, 3>& out, bool uniform) {
            const auto v = entry.value(QLatin1String(key));
            if (v.isUndefined()) return true;
            if (uniform && !v.isArray()) {
                if (!value(v, out[0])) return false;
                out[1] = out[2] = out[0];
                return true;
            }
            const auto parts = v.toArray();
            if (parts.size() != 3) return false;
            for (int axis = 0; axis < 3; ++axis) if (!value(parts[axis], out[axis])) return false;
            return true;
        };
        if (!vector("translate", object.translate, false) || !vector("rotate", object.rotate, false)
            || !vector("scale", object.scale, true) || !vector("spin", object.spin, false))
            return QStringLiteral("Scene object %1 has an invalid transform; use numbers or \"$parameter\"").arg(id);
        if (entry.contains(QStringLiteral("mesh"))) {
            QByteArray data;
            QString path;
            auto error = readGeometryAsset(pack, entry.value(QStringLiteral("mesh")).toString(), data, path);
            watched << path;
            if (!error.isEmpty()) return error;
            AfterglowRender::ShaderGeometry mesh;
            if (!AfterglowRender::ShaderProgram::loadObj(data, mesh, &error)) return error;
            if (geometry.vertices.size() + mesh.vertices.size() > std::size_t(3 * 1024 * 1024))
                return QStringLiteral("Scene meshes exceed the triangle budget");
            object.firstVertex = int(geometry.vertices.size());
            object.vertexCount = int(mesh.vertices.size());
            geometry.vertices.insert(geometry.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        }
        ids.insert(id, i);
        geometry.objects.push_back(object);
    }
    if (geometry.vertices.empty()) return QStringLiteral("A scene needs at least one object with a mesh");
    geometry.vertexCount = int(geometry.vertices.size());
    return {};
}

/// Afterglow's own metadata keys: the scene flags, the settle time and the mesh passes.
QString loadPackMetadata(const QString& pack, AfterglowRender::ShaderProgramSource& source,
                         const QStringList& includes, const QHash<QString, int>& scalarSlots, QStringList& watched)
{
    QByteArray data;
    QString path;
    auto error = readGeometryAsset(pack, QStringLiteral("metadata.json"), data, path);
    watched << path;
    if (!error.isEmpty()) return error;
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(data, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return QStringLiteral("Invalid pack metadata");
    for (const auto& [key, flag] : {std::pair{QStringLiteral("highDynamicRange"), &source.highDynamicRange},
                                    std::pair{QStringLiteral("windowBackground"), &source.windowBackground}}) {
        const auto value = document.object().value(key);
        if (!value.isUndefined() && !value.isBool()) return QStringLiteral("%1 must be true or false").arg(key);
        *flag = value.toBool(false);
    }
    if (const auto settle = document.object().value(QStringLiteral("settleSeconds")); !settle.isUndefined()) {
        const double seconds = settle.toDouble(-1);
        if (!settle.isDouble() || !(seconds > 0 && seconds <= 600))
            return QStringLiteral("settleSeconds must be a number above 0, at most 600");
        source.settleSeconds = float(seconds);
    }
    const auto value = document.object().value(QStringLiteral("geometryPasses"));
    if (value.isUndefined()) return {};
    if (!value.isArray() || value.toArray().size() != qsizetype(source.buffers.size()))
        return QStringLiteral("geometryPasses must have one entry per buffer shader (null for fullscreen)");
    const auto passes = value.toArray();
    qint64 totalBytes = 0;
    for (int i = 0; i < passes.size(); ++i) {
        if (passes[i].isNull()) continue;
        if (!passes[i].isObject()) return QStringLiteral("Geometry pass must be an object or null");
        const auto object = passes[i].toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            if (it.key() != QLatin1String("vertexShader") && it.key() != QLatin1String("mesh") && it.key() != QLatin1String("objects")
                && it.key() != QLatin1String("vertexCount") && it.key() != QLatin1String("instances") && it.key() != QLatin1String("samples"))
                return QStringLiteral("Unknown geometry setting: %1").arg(it.key());
        auto& geometry = source.buffers[i].geometry;
        error = readGeometryAsset(pack, object.value(QStringLiteral("vertexShader")).toString(), data, path);
        watched << path;
        if (!error.isEmpty()) return error;
        watchIncludes(data, path, includes, watched);
        geometry.vertexCode = data;
        geometry.directory = QFileInfo(path).absolutePath();
        if (geometry.vertexCode.trimmed().isEmpty()) return QStringLiteral("Empty mesh vertex shader");
        auto integer = [&](const char* key, int fallback, int& out) {
            const auto v = object.value(QLatin1String(key));
            if (v.isUndefined()) { out = fallback; return true; }
            const double n = v.toDouble(-1);
            if (!v.isDouble() || n < 1 || n > 3 * 1024 * 1024 || n != std::floor(n)) return false;
            out = int(n); return true;
        };
        if (!integer("instances", 1, geometry.instances)) return QStringLiteral("Invalid geometry instance count");
        if (!integer("samples", 1, geometry.samples)) return QStringLiteral("Invalid geometry sample count");
        if (object.contains(QStringLiteral("objects"))) {
            if (object.contains(QStringLiteral("mesh")) || object.contains(QStringLiteral("vertexCount"))
                || object.contains(QStringLiteral("instances")))
                return QStringLiteral("Use objects, or use mesh or vertexCount with instances, not both");
            error = loadSceneObjects(pack, object.value(QStringLiteral("objects")).toArray(), scalarSlots, geometry, watched);
            if (!error.isEmpty()) return error;
        } else if (object.contains(QStringLiteral("mesh"))) {
            if (object.contains(QStringLiteral("vertexCount"))) return QStringLiteral("Use mesh or vertexCount, not both");
            error = readGeometryAsset(pack, object.value(QStringLiteral("mesh")).toString(), data, path);
            watched << path;
            if (!error.isEmpty()) return error;
            if (!AfterglowRender::ShaderProgram::loadObj(data, geometry, &error)) return error;
        } else if (!integer("vertexCount", 0, geometry.vertexCount)) return QStringLiteral("Invalid geometry vertex count");
        totalBytes += qint64(geometry.vertices.size() * sizeof(AfterglowRender::ShaderMeshVertex));
        if (totalBytes > 96 * 1024 * 1024) return QStringLiteral("Pack geometry exceeds 96 MiB");
    }
    return {};
}

bool isFileReference(const QString& ref)
{
    return ref.contains(QLatin1Char('/')) || ref.endsWith(QLatin1String(".glsl"))
        || ref.endsWith(QLatin1String(".frag"));
}

QString bundledDirectory()
{
#if defined(AFTERGLOW_INSTALLED_SHADER_DIR)
    // A build-tree binary reads the checkout; an installed one its own prefix,
    // which need not be in XDG_DATA_DIRS.
    return runningFromBuildTree() ? QStringLiteral(AFTERGLOW_SOURCE_SHADER_DIR) : QStringLiteral(AFTERGLOW_INSTALLED_SHADER_DIR);
#elif defined(AFTERGLOW_SOURCE_SHADER_DIR)
    return QStringLiteral(AFTERGLOW_SOURCE_SHADER_DIR);
#else
    return {};
#endif
}

QString cacheKey(const QString& ref, const QVariantMap& overrides)
{
    return ref + QLatin1Char('\x1f') + QString::fromUtf8(QJsonDocument::fromVariant(overrides).toJson(QJsonDocument::Compact));
}

/// What a cached program was built from: each dependency's size and times, or
/// its absence. Two change signals for one edit then rebuild once.
QByteArray signature(const QStringList& paths)
{
    QByteArray out;
    QSet<QString> seen;
    for (const QString& path : paths) {
        if (seen.contains(path)) continue;
        seen.insert(path);
        const QFileInfo info(path);
        out += path.toUtf8() + '\0';
        if (info.exists()) {
            out += QByteArray::number(info.size()) + ':' + QByteArray::number(info.lastModified().toMSecsSinceEpoch()) + ':'
                + QByteArray::number(info.metadataChangeTime().toMSecsSinceEpoch());
        }
        out += '\n';
    }
    return out;
}

using ShaderInfo = PhosphorShaders::ShaderRegistry::ShaderInfo;

/// "<package>.<pack>" for a pack from a script package, else empty.
QString packagedRef(const ShaderInfo& info, const QMap<QString, QString>& packages)
{
    const QString parent = QFileInfo(info.packDir).absolutePath();
    for (auto it = packages.cbegin(); it != packages.cend(); ++it) {
        if (QFileInfo(it.value()).absoluteFilePath() == parent) return it.key() + QLatin1Char('.') + QDir(info.packDir).dirName();
    }
    return {};
}

} // namespace

/// The registry keys packs by a UUID derived from their id; configs name them
/// by directory (normally the declared id), display name or that UUID. A
/// package's packs answer only to "<package>.<pack>", the id the package
/// loader required them to declare.
struct ShaderLibrary::PackIndex
{
    QList<ShaderInfo> packs; ///< lowest priority first
    QHash<QString, qsizetype> byRef; ///< directory name, UUID or "<package>.<pack>"
    QHash<QString, qsizetype> byName; ///< case-folded display name

    ShaderInfo find(const QString& ref) const
    {
        auto it = byRef.constFind(ref);
        if (it == byRef.constEnd()) {
            it = byName.constFind(ref.toCaseFolded());
            if (it == byName.constEnd()) return {};
        }
        return packs.at(*it);
    }
};

ShaderLibrary::ShaderLibrary(QObject* parent)
    : QObject(parent)
    , m_registry(std::make_unique<PhosphorShaders::ShaderRegistry>())
    , m_presets(std::make_unique<PhosphorShaders::ShaderPresetStore>())
    , m_curves(std::make_unique<PhosphorAnimation::CurveRegistry>())
{
    // Lowest priority first: bundled, then system, then the user's own packs.
    // A build-tree binary puts the checkout above the system packs, so that
    // an installed copy does not shadow the packs that it is testing.
    QStringList dirs;
    const QString bundled = bundledDirectory();
    const bool checkout = runningFromBuildTree();
    if (!checkout && !bundled.isEmpty()) {
        dirs << bundled;
    }
    const QStringList system =
        QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("afterglow/shaders"),
                                  QStandardPaths::LocateDirectory);
    for (auto it = system.crbegin(); it != system.crend(); ++it) {
        dirs << *it;
    }
    if (checkout && !bundled.isEmpty()) {
        dirs.removeAll(bundled);
        dirs << bundled;
    }
    const QString user = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/afterglow/shaders");
    QDir().mkpath(user);
    dirs << user;
    dirs.removeDuplicates();
    for (const QString& d : dirs) {
        if (QDir(d).exists()) m_baseDirectories << d;
    }
    m_presets->load(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/afterglow/shader-presets"), {PhosphorShaders::ShaderFamily::Surface});
    buildRegistry();
    // The registry watches the packs too: whichever reports an edit first
    // rebuilds, and the other finds nothing left to drop.
    connect(&m_files, &PhosphorFsLoader::WatchedFileSet::filesChanged, this, [this] {
        if (invalidate()) Q_EMIT changed();
    });
}

ShaderLibrary::~ShaderLibrary() = default;

void ShaderLibrary::buildRegistry()
{
    // The registry cannot drop a search path, so a change of packages builds a new one.
    m_registry = std::make_unique<PhosphorShaders::ShaderRegistry>();
    for (const QString& d : m_baseDirectories) {
        m_registry->addSearchPath(d);
    }
    for (const QString& d : std::as_const(m_packageShaders)) {
        m_registry->addSearchPath(d);
    }
    m_registry->refresh();
    seedPresets();
    reindex();
    connect(m_registry.get(), &PhosphorShaders::ShaderRegistry::shadersChanged, this, [this] {
        seedPresets();
        reindex();
        if (invalidate()) Q_EMIT changed();
    });
}

void ShaderLibrary::reindex()
{
    auto index = std::make_unique<PackIndex>();
    auto priority = [this](const ShaderInfo& info) {
        const QString parent = QFileInfo(info.packDir).absolutePath();
        for (qsizetype i = 0; i < m_baseDirectories.size(); ++i) {
            if (QFileInfo(m_baseDirectories[i]).absoluteFilePath() == parent) return i;
        }
        // Package packs: their namespaced references never meet a base pack's.
        return m_baseDirectories.size();
    };
    auto packs = m_registry->availableShaders();
    std::stable_sort(packs.begin(), packs.end(),
                     [&](const ShaderInfo& a, const ShaderInfo& b) { return priority(a) < priority(b); });
    // Later, more important packs overwrite the names that earlier ones took.
    for (const ShaderInfo& info : std::as_const(packs)) {
        const qsizetype i = index->packs.size();
        index->packs << info;
        if (const QString packaged = packagedRef(info, m_packageShaders); !packaged.isEmpty()) {
            index->byRef.insert(packaged, i);
            continue;
        }
        index->byRef.insert(QDir(info.packDir).dirName(), i);
        index->byRef.insert(info.id, i);
        index->byName.insert(info.name.toCaseFolded(), i);
    }
    m_index = std::move(index);
}

void ShaderLibrary::setPackageShaders(const QMap<QString, QString>& directories)
{
    if (directories == m_packageShaders) return;
    m_packageShaders = directories;
    buildRegistry();
    m_compiled.clear();
    m_cache.clear();
    m_failed.clear();
    m_valueProblems.clear();
    m_overBudget.clear();
    Q_EMIT changed();
}

void ShaderLibrary::refreshWatches()
{
    QStringList paths;
    for (const auto& dependencies : m_dependencies) paths << dependencies;
    m_files.setPaths(paths);
}

bool ShaderLibrary::invalidate()
{
    bool dropped = false;
    // A pack whose files changed, or whose reference now names another pack,
    // compiles again, and every configuration of it goes with it.
    QSet<QString> stale;
    for (auto it = m_compiled.begin(); it != m_compiled.end();) {
        if (signature(it->watched) == it->signature && m_index->find(it.key()).packDir == it->packDir) {
            ++it;
            continue;
        }
        stale.insert(it.key());
        it = m_compiled.erase(it);
    }
    const auto keys = m_dependencies.keys();
    for (const QString& key : keys) {
        const QString ref = key.section(QLatin1Char('\x1f'), 0, 0);
        if (!stale.contains(ref) && signature(m_dependencies.value(key)) == m_signatures.value(key)
            && m_index->find(ref).packDir == m_resolved.value(key)) continue;
        // Bitwise: each must be dropped, whichever held it.
        dropped |= m_cache.remove(key) | m_failed.remove(key) | m_overBudget.remove(key);
        m_valueProblems.remove(key);
        m_signatures.remove(key);
    }
    return dropped;
}

QStringList ShaderLibrary::includeDirs(const QString& packDir) const
{
    QStringList dirs;
    const QString parent = QFileInfo(packDir).absolutePath();
    for (const QString& d : m_packageShaders) {
        if (QFileInfo(d).absoluteFilePath() == parent) dirs << d + QStringLiteral("/shared");
    }
    for (auto it = m_baseDirectories.crbegin(); it != m_baseDirectories.crend(); ++it) {
        dirs << *it + QStringLiteral("/shared");
    }
    return dirs;
}

QStringList ShaderLibrary::packIds() const
{
    QStringList ids;
    for (const auto& info : m_index->packs) {
        const QString packaged = packagedRef(info, m_packageShaders);
        ids << (packaged.isEmpty() ? QDir(info.packDir).dirName() : packaged);
    }
    ids.removeDuplicates();
    ids.sort();
    return ids;
}

void ShaderLibrary::seedPresets()
{
    PhosphorShaders::seedPackPresets(m_presets->registry(), PhosphorShaders::ShaderFamily::Surface, m_registry->availableShaders());
}

QVariantList ShaderLibrary::presets(const QString& ref) const
{
    const auto info = m_index->find(ref);
    QVariantList result;
    if (!info.isValid()) return result;
    result.append(QVariantMap{{QStringLiteral("id"), QStringLiteral("@defaults")}, {QStringLiteral("name"), QStringLiteral("Pack defaults")}, {QStringLiteral("read_only"), true}});
    for (const auto& preset : m_presets->registry().presetsFor(PhosphorShaders::ShaderFamily::Surface, info.id))
        result.append(QVariantMap{{QStringLiteral("id"), preset.id}, {QStringLiteral("name"), preset.name}, {QStringLiteral("read_only"), preset.readOnly}});
    return result;
}
QString ShaderLibrary::presetParameters(const QString& ref, const QString& id, QVariantMap& values) const
{
    const auto info = m_index->find(ref);
    if (!info.isValid()) return QStringLiteral("Shader has no preset metadata");
    QVariantMap candidate;
    for (const auto& p : parameters(ref)) {
        const auto metadata = p.toMap();
        if (metadata.value(QStringLiteral("supported")).toBool())
            candidate.insert(metadata.value(QStringLiteral("id")).toString(), metadata.value(QStringLiteral("default")));
    }
    if (id != QLatin1String("@defaults")) {
        const auto preset = m_presets->registry().preset(PhosphorShaders::ShaderFamily::Surface, info.id, id);
        if (!preset.isValid()) return QStringLiteral("Unknown shader preset");
        const auto resolved = m_presets->registry().resolveParams(PhosphorShaders::ShaderFamily::Surface, info.id, id, {});
        for (auto it = resolved.begin(); it != resolved.end(); ++it) {
            if (!candidate.contains(it.key())) return QStringLiteral("Preset contains an unknown or unsupported parameter");
            candidate.insert(it.key(), it.value());
        }
    }
    AfterglowRender::ShaderParameterValues packed;
    const auto error = validateParameters(ref, candidate, packed);
    if (error.isEmpty()) values = candidate;
    return error;
}
QString ShaderLibrary::savePreset(const QString& ref, const QString& name, const QVariantMap& values, QVariantMap& result)
{
    if (name.trimmed().isEmpty() || name.size() > 128) return QStringLiteral("Preset name needs 1 to 128 characters");
    AfterglowRender::ShaderParameterValues packed;
    const auto error = validateParameters(ref, values, packed);
    if (!error.isEmpty()) return error;
    const auto info = m_index->find(ref);
    // Images inside the pack are stored relative to it, so the preset still
    // works when the pack moves (a rebuilt checkout, a reinstalled package).
    QVariantMap stored = values;
    for (const auto& p : info.parameters) {
        if (p.type != QLatin1String("image") && p.type != QLatin1String("texture")) continue;
        const QString path = stored.value(p.id).toString();
        if (!QDir::isAbsolutePath(path)) continue;
        const QString relative = QDir(info.packDir).relativeFilePath(path);
        if (!relative.startsWith(QLatin1String(".."))) stored.insert(p.id, relative);
    }
    const auto directory = m_presets->directoryFor(PhosphorShaders::ShaderFamily::Surface);
    if (QDir(directory).entryList({QStringLiteral("*.json")}, QDir::Files).size() >= 128)
        return QStringLiteral("At most 128 user presets can be stored");
    PhosphorShaders::ShaderPreset preset;
    preset.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    preset.name = name.trimmed(); preset.packId = info.id; preset.params = stored;
    if (!QDir().mkpath(directory)) return QStringLiteral("Cannot create preset directory");
    QSaveFile file(directory + QLatin1Char('/') + preset.id + QStringLiteral(".json"));
    const auto bytes = QJsonDocument(preset.toJson()).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return QStringLiteral("Cannot save shader preset: %1").arg(file.errorString());
    m_presets->rescanNow(PhosphorShaders::ShaderFamily::Surface);
    result = {{QStringLiteral("id"), preset.id}, {QStringLiteral("name"), preset.name}};
    return {};
}

QVariantList ShaderLibrary::parameters(const QString& ref) const
{
    QVariantList result;
    for (const auto& p : m_index->find(ref).parameters) {
        const bool supported = p.slot >= 0 && (((p.type == QLatin1String("image") || p.type == QLatin1String("texture")) && p.slot < AfterglowRender::MaxShaderImages)
            || (p.type == QLatin1String("color") && p.slot < 16)
            || ((p.type == QLatin1String("float") || p.type == QLatin1String("int") || p.type == QLatin1String("bool")) && p.slot < 32));
        QVariantMap entry{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.name},
            {QStringLiteral("type"), p.type}, {QStringLiteral("default"), p.defaultValue},
            {QStringLiteral("slot"), p.slot}, {QStringLiteral("supported"), supported}, {QStringLiteral("unit"), p.unit}};
        if (p.minValue.isValid()) entry.insert(QStringLiteral("min"), p.minValue);
        if (p.maxValue.isValid()) entry.insert(QStringLiteral("max"), p.maxValue);
        result.append(entry);
    }
    return result;
}

QString ShaderLibrary::validateParameters(const QString& ref, const QVariantMap& values,
                                         AfterglowRender::ShaderParameterValues& packed) const
{
    const auto info = m_index->find(ref);
    if (!info.isValid()) return QStringLiteral("Shader has no parameter metadata");
    AfterglowRender::ShaderParameterValues candidate;
    for (auto it = values.begin(); it != values.end(); ++it) {
        const auto p = std::find_if(info.parameters.begin(), info.parameters.end(), [&](const auto& p) { return p.id == it.key(); });
        if (p == info.parameters.end()) return QStringLiteral("Unknown shader parameter: %1").arg(it.key());
        const QVariant& v = it.value();
        const auto type = v.metaType().id();
        if (p->type == QLatin1String("float") || p->type == QLatin1String("int")) {
            const double n = v.toDouble();
            if ((type != QMetaType::Double && type != QMetaType::Int && type != QMetaType::LongLong)
                || !std::isfinite(n) || std::abs(n) > std::numeric_limits<float>::max()
                || (p->type == QLatin1String("int") && (n != std::floor(n) || n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max())))
                return QStringLiteral("Parameter %1 needs a finite %2").arg(it.key(), p->type);
        } else if (p->type == QLatin1String("bool")) {
            if (type != QMetaType::Bool) return QStringLiteral("Parameter %1 needs a boolean").arg(it.key());
        } else if (p->type == QLatin1String("color")) {
            if (type != QMetaType::QString || !QColor(v.toString()).isValid()) return QStringLiteral("Parameter %1 needs a color string").arg(it.key());
        } else if (p->type == QLatin1String("image") || p->type == QLatin1String("texture")) {
            if (type != QMetaType::QString) return QStringLiteral("Image parameter needs a path string");
        } else return QStringLiteral("Unsupported parameter type: %1").arg(p->type);
        const bool image = p->type == QLatin1String("image") || p->type == QLatin1String("texture");
        if (p->slot < 0 || p->slot >= (image ? AfterglowRender::MaxShaderImages : p->type == QLatin1String("color") ? 16 : 32))
            return QStringLiteral("Parameter has no supported uniform slot");
    }
    if (!m_registry->validateParams(info.id, values)) return QStringLiteral("Shader parameter outside its declared range");
    const auto validated = m_registry->validateAndCoerceParams(info.id, values);
    for (const auto& p : info.parameters) {
        if (!values.contains(p.id)) continue;
        const auto v = validated.value(p.id);
        if (p.type == QLatin1String("image") || p.type == QLatin1String("texture")) {
            // Scripts, presets and pack defaults reach only the pack's own files.
            const auto error = loadImageParameter(info.packDir, v.toString(), AbsolutePathPolicy::Reject,
                                                  candidate.images[p.slot], candidate.imagePaths[p.slot]);
            if (!error.isEmpty()) return error;
            candidate.imageMask |= std::uint8_t(1u << p.slot);
        } else if (p.type == QLatin1String("color")) {
            const QColor c(v.toString());
            candidate.colorMask |= std::uint16_t(1u << p.slot);
            auto* out = candidate.colors.data() + p.slot * 4;
            out[0] = c.redF(); out[1] = c.greenF(); out[2] = c.blueF(); out[3] = c.alphaF();
        } else {
            candidate.paramMask |= std::uint32_t(1) << p.slot;
            candidate.params[p.slot] = p.type == QLatin1String("bool") ? (v.toBool() ? 1.f : 0.f) : v.toFloat();
        }
    }
    packed = candidate;
    return {};
}

AfterglowRender::ShaderChains ShaderLibrary::build(const Refs& refs, const QVariantMap& params)
{
    AfterglowRender::ShaderChains chains;
    QSet<QString> desired;
    QSet<QString> named;
    const QStringList overview = refs.overview.isEmpty() ? QStringList{} : QStringList{refs.overview};
    const QStringList tabTransition = refs.tabTransition.isEmpty() ? QStringList{} : QStringList{refs.tabTransition};
    const QStringList paneEffects = refs.paneEffects.isEmpty() ? QStringList{} : QStringList{refs.paneEffects};
    for (const auto& chain : {refs.background, refs.text, refs.cursor, refs.pane, refs.window, overview, tabTransition, paneEffects})
        for (const auto& ref : chain) {
            desired.insert(cacheKey(ref, params.value(ref).toMap()));
            named.insert(ref);
        }
    // Each window builds the same chains: say what the config gets wrong once.
    if (const QString check = QString::fromUtf8(QJsonDocument::fromVariant(params).toJson(QJsonDocument::Compact))
            + QStringList(named.values()).join(QLatin1Char('\x1f'));
        check != m_paramsChecked) {
        m_paramsChecked = check;
        m_paramsProblems.clear();
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (it.value().typeId() != QMetaType::QVariantMap)
                m_paramsProblems << QStringLiteral("Shaders.params.%1 must be an object of parameter values").arg(it.key());
            else if (!named.contains(it.key()))
                m_paramsProblems << QStringLiteral("Shaders.params.%1 names no shader in any chain, so its values apply nowhere")
                                        .arg(it.key());
        }
        for (const QString& problem : std::as_const(m_paramsProblems)) qCWarning(lcShaders).noquote() << problem;
    }
    // Configuration changes must release old decoded assets and compiled packs.
    m_compiled.removeIf([&](const auto& item) { return !named.contains(item.key()); });
    m_cache.removeIf([&](const auto& item) { return !desired.contains(item.key()); });
    m_failed.removeIf([&](const auto& item) { return !desired.contains(item.key()); });
    m_valueProblems.removeIf([&](const auto& item) { return !desired.contains(item.key()); });
    m_dependencies.removeIf([&](const auto& item) { return !desired.contains(item.key()); });
    m_signatures.removeIf([&](const auto& item) { return !desired.contains(item.key()); });
    m_resolved.removeIf([&](const auto& item) { return !desired.contains(item.key()); });
    // Budget pressure is transient: a new set of packs may fit, so try again.
    if (desired != m_desired) {
        m_desired = desired;
        m_overBudget.clear();
    }
    QSet<qint64> images;
    qint64 imageBytes = 0;
    auto add = [&](const QStringList& from, std::vector<AfterglowRender::ShaderProgramPtr>& to) {
        for (const QString& ref : from) {
            const QVariantMap overrides = params.value(ref).toMap();
            const QString key = cacheKey(ref, overrides);
            if (m_overBudget.contains(key)) continue;
            if (auto p = load(ref, overrides)) {
                auto candidate = images;
                qint64 bytes = imageBytes;
                for (const auto& image : p->images()) if (!image.isNull() && !candidate.contains(image.cacheKey())) {
                    candidate.insert(image.cacheKey()); bytes += image.sizeInBytes();
                }
                if (bytes > MaxDecodedImageBytes) {
                    qCWarning(lcShaders).noquote() << ref << ": configured images exceed the" << (MaxDecodedImageBytes >> 20)
                                                   << "MiB decoded-image budget";
                    // Not compiled again for every window and change until the set of packs changes.
                    m_cache.remove(key);
                    m_overBudget.insert(key);
                    continue;
                }
                images = std::move(candidate); imageBytes = bytes;
                to.push_back(std::move(p));
            }
        }
    };
    add(refs.background, chains.background);
    add(refs.text, chains.text);
    add(refs.cursor, chains.cursor);
    add(refs.pane, chains.pane);
    add(refs.window, chains.window);
    add(overview, chains.overview);
    add(tabTransition, chains.tabTransition);
    add(paneEffects, chains.paneEffects);
    refreshWatches();
    // What the user sees in a notice: each configured pack that does not
    // draw, and why, in a line. Compile logs stay in the afterglow.shaders log.
    QStringList problems = m_paramsProblems;
    QStringList packs;
    for (const QString& key : std::as_const(m_desired)) {
        const QString ref = key.section(QLatin1Char('\x1f'), 0, 0);
        if (const auto failed = m_failed.constFind(key); failed != m_failed.constEnd()) {
            packs << QStringLiteral("shader %1: %2").arg(ref, failed->section(QLatin1Char('\n'), 0, 0).trimmed());
        } else if (m_overBudget.contains(key)) {
            packs << QStringLiteral("shader %1: its images exceed the %2 MiB decoded-image budget")
                         .arg(ref)
                         .arg(MaxDecodedImageBytes >> 20);
        }
        packs << m_valueProblems.value(key);
    }
    packs.sort();
    packs.removeDuplicates();
    problems << packs;
    if (problems != m_problems) {
        m_problems = problems;
        Q_EMIT problemsChanged(m_problems);
    }
    return chains;
}

AfterglowRender::ShaderProgramPtr ShaderLibrary::load(const QString& ref, const QVariantMap& overrides)
{
    const QString key = cacheKey(ref, overrides);
    if (auto it = m_cache.constFind(key); it != m_cache.constEnd()) {
        return *it;
    }
    if (m_failed.contains(key)) {
        return nullptr;
    }
    auto& watched = m_dependencies[key];
    watched.clear();
    auto program = configure(key, ref, overrides, watched);
    m_signatures.insert(key, signature(watched));
    m_resolved.insert(key, m_index->find(ref).packDir);
    return program;
}

AfterglowRender::ShaderProgramPtr ShaderLibrary::configure(const QString& key, const QString& ref,
                                                           const QVariantMap& overrides, QStringList& watched)
{
    auto compiled = m_compiled.find(ref);
    if (compiled == m_compiled.end()) compiled = m_compiled.insert(ref, compile(ref));
    watched = compiled->watched;
    m_valueProblems.remove(key);
    if (!compiled->program) {
        // compile() said why, once for the pack.
        m_failed.insert(key, compiled->error);
        return nullptr;
    }
    auto fail = [&](const QString& error) {
        qCWarning(lcShaders).noquote() << ref << ":" << error;
        m_failed.insert(key, error);
        return nullptr;
    };
    const ShaderInfo info = m_index->find(ref);
    // A default image that did not load fails the pack, unless the config names another one.
    // Images, colours and scalars number their slots apart: only an image owns an image slot.
    for (const auto& p : info.parameters) {
        if ((p.type != QLatin1String("image") && p.type != QLatin1String("texture")) || p.slot < 0
            || p.slot >= AfterglowRender::MaxShaderImages || overrides.contains(p.id))
            continue;
        if (const QString& error = compiled->imageErrors[std::size_t(p.slot)]; !error.isEmpty()) return fail(error);
    }
    if (overrides.isEmpty()) {
        m_cache.insert(key, compiled->program);
        return compiled->program;
    }
    // Values that apply nowhere: build() lists them with the other problems.
    // The result is cached, so they are kept by key.
    QStringList notes;
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        if (std::none_of(info.parameters.begin(), info.parameters.end(), [&](const auto& p) { return p.id == it.key(); }))
            notes << QStringLiteral("Shaders.params.%1.%2: the pack has no such parameter").arg(ref, it.key());
    }
    // Phosphor decides which values the pack accepts (in range, a colour, a truth value).
    // Each value is asked alone, so that one wrong value does not reject the others.
    // A rejected value gives the pack's default.
    QSet<QString> rejected;
    for (const auto& p : info.parameters) {
        if (p.type == QLatin1String("image") || p.type == QLatin1String("texture") || !overrides.contains(p.id)) continue;
        if (m_registry->validateParams(info.id, QVariantMap{{p.id, overrides.value(p.id)}})) continue;
        rejected.insert(p.id);
        notes << QStringLiteral("Shaders.params.%1.%2: the pack does not accept this value, so it uses its default")
                     .arg(ref, p.id);
    }
    // The config's values over the pack's defaults, in phosphor's slot layout.
    AfterglowRender::ShaderParameterValues values;
    for (const auto& p : info.parameters) {
        if (p.slot < 0 || !overrides.contains(p.id)) {
            continue;
        }
        const QVariant v = rejected.contains(p.id) ? p.defaultValue : overrides.value(p.id);
        if (p.type == QLatin1String("image") || p.type == QLatin1String("texture")) {
            if (p.slot >= AfterglowRender::MaxShaderImages) continue;
            // The user's config may name any image; the pack's own default stays inside it.
            // A value that is not a path would turn into the pack's default here, read
            // as trusted, so it fails instead.
            const QVariant path = overrides.value(p.id);
            if (path.typeId() != QMetaType::QString)
                return fail(QStringLiteral("Shaders.params sets %1 to a value that is not an image path").arg(p.id));
            const auto error = loadImageParameter(info.packDir, path.toString(), AbsolutePathPolicy::Trust,
                                                  values.images[std::size_t(p.slot)], values.imagePaths[std::size_t(p.slot)]);
            watched << values.imagePaths[std::size_t(p.slot)];
            if (!error.isEmpty()) return fail(error);
            values.imageMask |= std::uint8_t(1u << p.slot);
        } else if (p.type == QLatin1String("color")) {
            // A colour that does not parse was rejected above and gave the pack's default.
            const QColor c(v.toString());
            if (p.slot < 16 && c.isValid()) {
                float* out = &values.colors[std::size_t(p.slot) * 4];
                out[0] = float(c.redF());
                out[1] = float(c.greenF());
                out[2] = float(c.blueF());
                out[3] = float(c.alphaF());
                values.colorMask |= std::uint16_t(1u << p.slot);
            }
        } else if (p.slot < 32) {
            values.params[std::size_t(p.slot)] = p.type == QLatin1String("bool") ? (v.toBool() ? 1.0f : 0.0f) : v.toFloat();
            values.paramMask |= std::uint32_t(1) << p.slot;
        }
    }
    // Only a pack that draws lists them. A pack that failed above has its one line.
    for (const QString& note : std::as_const(notes)) qCWarning(lcShaders).noquote() << note;
    if (!notes.isEmpty()) m_valueProblems.insert(key, notes);
    auto program = compiled->program->withValues(values);
    m_cache.insert(key, program);
    return program;
}

ShaderLibrary::Compiled ShaderLibrary::compile(const QString& ref)
{
    Compiled result;
    const ShaderInfo info = m_index->find(ref);
    result.packDir = info.packDir;
    QStringList& watched = result.watched;
    auto done = [&] {
        result.signature = signature(watched);
        return result;
    };
    auto fail = [&](const QString& error) {
        qCWarning(lcShaders).noquote() << ref << ":" << error;
        result.error = error;
        return done();
    };
    if (!info.isValid()) {
        // A package pack may be named "<package>.glsl", so only a reference
        // that names no pack is taken for a file.
        if (isFileReference(ref)) {
            return fail(QStringLiteral("shaders are packs, referenced by id: put the shader in a directory with a "
                                       "metadata.json under ~/.config/afterglow/shaders"));
        }
        // The list goes to the log only: in a notice it would fill the body.
        qCInfo(lcShaders).noquote() << "available shader packs:" << packIds().join(QStringLiteral(", "));
        return fail(QStringLiteral("no shader pack named \"%1\"").arg(ref));
    }
    if (info.bufferShaderPaths.size() > AfterglowRender::MaxShaderBuffers) return fail(QStringLiteral("Too many buffer passes"));
    const QStringList includes = includeDirs(info.packDir);
    AfterglowRender::ShaderProgramSource src;
    src.id = ref;
    src.directory = info.packDir;
    src.includeDirs = includes;
    src.useDepthBuffer = info.useDepthBuffer;
    watched << QDir(info.packDir).filePath(QStringLiteral("metadata.json"));
    if (const auto error = readStage(info.sourcePath, includes, watched, src.code); !error.isEmpty()) return fail(error);
    if (!info.vertexShaderPath.isEmpty()) {
        if (const auto error = readStage(info.vertexShaderPath, includes, watched, src.vertexCode); !error.isEmpty())
            return fail(error);
    }
    for (int index = 0; index < info.bufferShaderPaths.size(); ++index) {
        const auto bufferPath = info.bufferShaderPaths[index];
        AfterglowRender::ShaderBufferSource pass;
        if (const auto error = readStage(bufferPath, includes, watched, pass.code); !error.isEmpty()) return fail(error);
        pass.directory = QFileInfo(bufferPath).absolutePath();
        pass.feedback = info.bufferFeedback;
        pass.halfFloat = info.halfFloatBuffers;
        pass.scale = float(info.bufferScale);
        const auto filter = info.bufferFilters.value(index, info.bufferFilter);
        const auto wrap = info.bufferWraps.value(index, info.bufferWrap);
        if ((filter != QLatin1String("linear") && filter != QLatin1String("nearest"))
            || (wrap != QLatin1String("clamp") && wrap != QLatin1String("repeat") && wrap != QLatin1String("mirror"))) {
            return fail(QStringLiteral("Invalid buffer filter/wrap"));
        }
        pass.nearest = filter == QLatin1String("nearest");
        pass.wrap = wrap == QLatin1String("repeat") ? AfterglowRender::ShaderWrap::Repeat
            : wrap == QLatin1String("mirror") ? AfterglowRender::ShaderWrap::Mirror : AfterglowRender::ShaderWrap::Clamp;
        src.buffers.push_back(std::move(pass));
    }
    QHash<QString, int> scalarSlots;
    for (const auto& p : info.parameters)
        if (p.slot >= 0 && p.slot < 32 && (p.type == QLatin1String("float") || p.type == QLatin1String("int")))
            scalarSlots.insert(p.id, p.slot);
    if (const auto error = loadPackMetadata(info.packDir, src, includes, scalarSlots, watched); !error.isEmpty())
        return fail(error);
    src.parameterPreamble = PhosphorShaders::ShaderRegistry::paramPreamble(info).toUtf8();
    // The pack's defaults, in phosphor's slot layout. configure() lays the config's values over them.
    const auto defaults = m_registry->validateAndCoerceParams(info.id, {});
    for (const auto& p : info.parameters) {
        if (p.slot < 0) {
            continue;
        }
        const QVariant v = defaults.value(p.id, p.defaultValue);
        if (p.type == QLatin1String("image") || p.type == QLatin1String("texture")) {
            if (p.slot >= AfterglowRender::MaxShaderImages) continue;
            const auto error = loadImageParameter(info.packDir, v.toString(), AbsolutePathPolicy::Reject, src.images[p.slot],
                                                  src.imagePaths[p.slot]);
            watched << src.imagePaths[p.slot];
            // The pack still compiles: a configured image in this slot can stand in for it (configure()).
            if (!error.isEmpty()) {
                result.imageErrors[std::size_t(p.slot)] = error;
                src.images[p.slot] = {};
            }
        } else if (p.type == QLatin1String("color")) {
            const QColor c(v.toString());
            if (p.slot < 16 && c.isValid()) {
                float* out = &src.colors[std::size_t(p.slot) * 4];
                out[0] = float(c.redF());
                out[1] = float(c.greenF());
                out[2] = float(c.blueF());
                out[3] = float(c.alphaF());
            }
        } else if (p.slot < 32) {
            src.params[std::size_t(p.slot)] = p.type == QLatin1String("bool") ? (v.toBool() ? 1.0f : 0.0f)
                                                                               : v.toFloat();
        }
    }

    QString error;
    auto program = AfterglowRender::ShaderProgram::compile(src, &error);
    if (!program) {
        qCWarning(lcShaders).noquote() << "shader" << ref << "failed to compile:\n" << error;
        result.error = error;
        return done();
    }
    qCInfo(lcShaders).noquote() << "loaded shader" << ref
                                << (program->animated() ? "(animated)" : program->eventAges() ? "(animated after events)" : "");
    result.program = std::move(program);
    return done();
}

} // namespace Afterglow
