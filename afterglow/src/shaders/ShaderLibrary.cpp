// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderLibrary.h"

#include <PhosphorAnimation/CurveRegistry.h>
#include <PhosphorFsLoader/ContainedFileReader.h>
#include <PhosphorShaders/ShaderIncludeResolver.h>
#include <PhosphorShaders/ShaderRegistry.h>
#include <PhosphorShaders/ShaderPresetStore.h>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include <QImageReader>
#include <QBuffer>
#include <QUuid>

#include <QColor>
#include <algorithm>
#include <cmath>
#include <limits>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QStandardPaths>

Q_LOGGING_CATEGORY(lcShaders, "afterglow.shaders", QtInfoMsg)

namespace Afterglow {

namespace {

QString expandHome(const QString& path)
{
    return path.startsWith(QLatin1String("~/")) ? QDir::homePath() + path.mid(1) : path;
}

// Mirror the resolver's include search, retaining missing candidates as well
// as resolved paths so a broken shader can recover without a config change.
void collectIncludes(const QString& source, const QString& dir, const QStringList& includeDirs,
                     QStringList& watched, QSet<QString>& seen, int depth = 0)
{
    if (depth > PhosphorShaders::ShaderIncludeResolver::MaxIncludeDepth) return;
    static const QRegularExpression pattern(QStringLiteral(R"(^\s*#include\s+(["<])([^">]+)[">]\s*$)"));
    for (const auto& line : source.split(QLatin1Char('\n'))) {
        if (!line.contains(QLatin1String("#include"))) continue;
        const auto match = pattern.match(line);
        if (!match.hasMatch()) continue;
        const auto name = match.captured(2).trimmed();
        if (name.isEmpty() || QFileInfo(name).fileName() == QLatin1String(PhosphorShaders::ShaderIncludeResolver::GeneratedPreambleInclude)) continue;
        QStringList search;
        if (match.captured(1) == QLatin1String("\"")) search << dir;
        search << includeDirs;
        for (const auto& location : search) {
            // The contract is built in and never changes.
            if (location.isEmpty() || location.startsWith(QLatin1Char(':'))) continue;
            const auto candidate = QDir::cleanPath(QDir(location).absoluteFilePath(name));
            watched << candidate;
            const QFileInfo info(candidate);
            if (!info.exists() || !info.isFile()) continue;
            const auto resolved = info.canonicalFilePath();
            watched << resolved;
            if (!seen.contains(resolved)) {
                seen.insert(resolved);
                QFile file(resolved);
                if (file.open(QIODevice::ReadOnly | QIODevice::Text))
                    collectIncludes(QString::fromUtf8(file.readAll()), QFileInfo(resolved).absolutePath(), includeDirs, watched, seen, depth + 1);
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
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("Cannot read shader: %1").arg(path);
    code = file.readAll();
    watchIncludes(code, path, includes, watched);
    return {};
}

QString loadImageParameter(const QString& pack, const QString& value, QImage& image, QString& resolved)
{
    if (value.isEmpty()) { image = {}; resolved.clear(); return {}; }
    const auto path = expandHome(value);
    // Watched even when unreadable, so fixing the file recovers the shader.
    resolved = QFileInfo(QDir(pack).absoluteFilePath(path)).absoluteFilePath();
    constexpr qint64 MaxBytes = 16 * 1024 * 1024;
    // Relative paths stay inside the pack; absolute ones come from the user.
    QString error;
    const auto file = PhosphorFsLoader::readFileWithinDirectory(path, pack, PhosphorFsLoader::AbsolutePathPolicy::Trust,
                                                                MaxBytes, &error);
    if (!file) return QStringLiteral("Image must be a regular file of at most 16 MiB, inside the pack when relative: %1").arg(error);
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

/// Afterglow's own metadata keys: the scene flags and the mesh passes.
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
                return QStringLiteral("Use objects, or mesh and vertexCount with instances, not both");
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

} // namespace

ShaderLibrary::ShaderLibrary(QObject* parent)
    : QObject(parent)
    , m_registry(std::make_unique<PhosphorShaders::ShaderRegistry>())
    , m_presets(std::make_unique<PhosphorShaders::ShaderPresetStore>())
    , m_curves(std::make_unique<PhosphorAnimation::CurveRegistry>())
{
    // Lowest priority first: bundled, then system, then the user's own packs.
    QStringList dirs;
#ifdef AFTERGLOW_SOURCE_SHADER_DIR
    dirs << QStringLiteral(AFTERGLOW_SOURCE_SHADER_DIR);
#endif
    const QStringList system =
        QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("afterglow/shaders"),
                                  QStandardPaths::LocateDirectory);
    for (auto it = system.crbegin(); it != system.crend(); ++it) {
        dirs << *it;
    }
    const QString user = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/afterglow/shaders");
    QDir().mkpath(user);
    dirs << user;
    dirs.removeDuplicates();

    for (const QString& d : dirs) {
        if (QDir(d).exists()) {
            m_registry->addSearchPath(d);
            m_includeDirs << d + QStringLiteral("/shared");
        }
    }
    m_registry->refresh();
    m_presets->load(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/afterglow/shader-presets"), {PhosphorShaders::ShaderFamily::Surface});
    seedPresets();

    connect(m_registry.get(), &PhosphorShaders::ShaderRegistry::shadersChanged, this, [this] {
        seedPresets();
        m_cache.clear();
        m_failed.clear();
        Q_EMIT changed();
    });
    connect(&m_files, &PhosphorFsLoader::WatchedFileSet::filesChanged, this, [this] {
        m_cache.clear();
        m_failed.clear();
        Q_EMIT changed();
    });
}

ShaderLibrary::~ShaderLibrary() = default;

void ShaderLibrary::refreshWatches()
{
    QStringList paths;
    for (const auto& dependencies : m_dependencies) paths << dependencies;
    m_files.setPaths(paths);
}

QStringList ShaderLibrary::packIds() const
{
    QStringList ids;
    for (const auto& info : m_registry->availableShaders()) {
        ids << QDir(info.packDir).dirName();
    }
    ids.sort();
    return ids;
}

namespace {
/// The registry keys packs by a UUID derived from their id; configs name them
/// by directory (normally the declared id), display name or that UUID.
PhosphorShaders::ShaderRegistry::ShaderInfo findPack(const PhosphorShaders::ShaderRegistry& registry, const QString& ref)
{
    for (const auto& info : registry.availableShaders()) {
        if (QDir(info.packDir).dirName() == ref || info.id == ref
            || info.name.compare(ref, Qt::CaseInsensitive) == 0) {
            return info;
        }
    }
    return {};
}
} // namespace

void ShaderLibrary::seedPresets()
{
    PhosphorShaders::seedPackPresets(m_presets->registry(), PhosphorShaders::ShaderFamily::Surface, m_registry->availableShaders());
}

QVariantList ShaderLibrary::presets(const QString& ref) const
{
    const auto info = findPack(*m_registry, ref);
    QVariantList result;
    if (!info.isValid()) return result;
    result.append(QVariantMap{{QStringLiteral("id"), QStringLiteral("@defaults")}, {QStringLiteral("name"), QStringLiteral("Pack defaults")}, {QStringLiteral("read_only"), true}});
    for (const auto& preset : m_presets->registry().presetsFor(PhosphorShaders::ShaderFamily::Surface, info.id))
        result.append(QVariantMap{{QStringLiteral("id"), preset.id}, {QStringLiteral("name"), preset.name}, {QStringLiteral("read_only"), preset.readOnly}});
    return result;
}
QString ShaderLibrary::presetParameters(const QString& ref, const QString& id, QVariantMap& values) const
{
    const auto info = findPack(*m_registry, ref);
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
    const auto info = findPack(*m_registry, ref);
    const auto directory = m_presets->directoryFor(PhosphorShaders::ShaderFamily::Surface);
    if (QDir(directory).entryList({QStringLiteral("*.json")}, QDir::Files).size() >= 128)
        return QStringLiteral("At most 128 user presets can be stored");
    PhosphorShaders::ShaderPreset preset;
    preset.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    preset.name = name.trimmed(); preset.packId = info.id; preset.params = values;
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
    for (const auto& p : findPack(*m_registry, ref).parameters) {
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
    const auto info = findPack(*m_registry, ref);
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
            const auto error = loadImageParameter(info.packDir, v.toString(), candidate.images[p.slot], candidate.imagePaths[p.slot]);
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
    auto cacheKey = [&](const QString& ref) {
        return ref + QLatin1Char('\x1f')
            + QString::fromUtf8(QJsonDocument::fromVariant(params.value(ref).toMap()).toJson(QJsonDocument::Compact));
    };
    QSet<QString> desired;
    const QStringList overview = refs.overview.isEmpty() ? QStringList{} : QStringList{refs.overview};
    const QStringList tabTransition = refs.tabTransition.isEmpty() ? QStringList{} : QStringList{refs.tabTransition};
    const QStringList paneEffects = refs.paneEffects.isEmpty() ? QStringList{} : QStringList{refs.paneEffects};
    for (const auto& chain : {refs.background, refs.text, refs.cursor, refs.pane, refs.window, overview, tabTransition, paneEffects})
        for (const auto& ref : chain) desired.insert(cacheKey(ref));
    // Configuration changes must release old decoded assets and compiled packs.
    m_cache.removeIf([&](const auto& item) { return !desired.contains(item.key()); });
    m_failed.removeIf([&](const auto& item) { return !desired.contains(item.key()); });
    m_dependencies.removeIf([&](const auto& item) { return !desired.contains(item.key()); });
    QSet<qint64> images;
    qint64 imageBytes = 0;
    auto add = [&](const QStringList& from, std::vector<AfterglowRender::ShaderProgramPtr>& to) {
        for (const QString& ref : from) {
            if (auto p = load(ref, params.value(ref).toMap())) {
                auto candidate = images;
                qint64 bytes = imageBytes;
                for (const auto& image : p->images()) if (!image.isNull() && !candidate.contains(image.cacheKey())) {
                    candidate.insert(image.cacheKey()); bytes += image.sizeInBytes();
                }
                if (bytes > 256LL * 1024 * 1024) {
                    const auto error = QStringLiteral("Configured images exceed the 256 MiB decoded-image budget");
                    qCWarning(lcShaders) << ref << error;
                    // Budget pressure is transient: retry after other packs are removed.
                    m_cache.remove(cacheKey(ref));
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
    return chains;
}

AfterglowRender::ShaderProgramPtr ShaderLibrary::load(const QString& ref, const QVariantMap& overrides)
{
    const QString key = ref + QLatin1Char('\x1f')
        + QString::fromUtf8(QJsonDocument::fromVariant(overrides).toJson(QJsonDocument::Compact));
    if (auto it = m_cache.constFind(key); it != m_cache.constEnd()) {
        return *it;
    }
    if (m_failed.contains(key)) {
        return nullptr;
    }

    auto& watched = m_dependencies[key];
    watched.clear();
    auto fail = [&](const QString& error) {
        qCWarning(lcShaders).noquote() << ref << ":" << error;
        m_failed.insert(key, error);
        return nullptr;
    };
    if (isFileReference(ref)) {
        return fail(QStringLiteral("shaders are packs, referenced by id: put the shader in a directory with a "
                                   "metadata.json under ~/.config/afterglow/shaders"));
    }
    const PhosphorShaders::ShaderRegistry::ShaderInfo info = findPack(*m_registry, ref);
    if (!info.isValid()) {
        return fail(QStringLiteral("no shader pack named \"%1\" (available: %2)")
                        .arg(ref, packIds().join(QStringLiteral(", "))));
    }
    if (info.bufferShaderPaths.size() > AfterglowRender::MaxShaderBuffers) return fail(QStringLiteral("Too many buffer passes"));
    AfterglowRender::ShaderProgramSource src;
    src.id = ref;
    src.directory = info.packDir;
    src.includeDirs = m_includeDirs;
    src.useDepthBuffer = info.useDepthBuffer;
    watched << QDir(info.packDir).filePath(QStringLiteral("metadata.json"));
    if (const auto error = readStage(info.sourcePath, m_includeDirs, watched, src.code); !error.isEmpty()) return fail(error);
    if (!info.vertexShaderPath.isEmpty()) {
        if (const auto error = readStage(info.vertexShaderPath, m_includeDirs, watched, src.vertexCode); !error.isEmpty())
            return fail(error);
    }
    for (int index = 0; index < info.bufferShaderPaths.size(); ++index) {
        const auto bufferPath = info.bufferShaderPaths[index];
        AfterglowRender::ShaderBufferSource pass;
        if (const auto error = readStage(bufferPath, m_includeDirs, watched, pass.code); !error.isEmpty()) return fail(error);
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
    if (const auto error = loadPackMetadata(info.packDir, src, m_includeDirs, scalarSlots, watched); !error.isEmpty())
        return fail(error);
    src.parameterPreamble = PhosphorShaders::ShaderRegistry::paramPreamble(info).toUtf8();
    // Defaults, overlaid with the config's values, in phosphor's slot layout.
    const auto validated = m_registry->validateAndCoerceParams(info.id, overrides);
    for (const auto& p : info.parameters) {
        if (p.slot < 0) {
            continue;
        }
        const QVariant v = validated.value(p.id, p.defaultValue);
        if (p.type == QLatin1String("image") || p.type == QLatin1String("texture")) {
            if (p.slot >= AfterglowRender::MaxShaderImages) continue;
            const auto error = loadImageParameter(info.packDir, v.toString(), src.images[p.slot], src.imagePaths[p.slot]);
            watched << src.imagePaths[p.slot];
            if (!error.isEmpty()) return fail(error);
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
        m_failed.insert(key, error);
        return nullptr;
    }
    qCInfo(lcShaders).noquote() << "loaded shader" << ref << (program->animated() ? "(animated)" : "");
    m_cache.insert(key, program);
    return program;
}

} // namespace Afterglow
