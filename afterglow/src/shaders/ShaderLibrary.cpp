// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderLibrary.h"

#include <PhosphorShaders/ShaderIncludeResolver.h>
#include <PhosphorShaders/ShaderRegistry.h>
#include <PhosphorShaders/ShaderPresetStore.h>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include <QImageReader>
#include <QBuffer>
#include <QUuid>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
            if (location.isEmpty()) continue;
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

QString expandWatched(const QString& source, const QString& dir, const QStringList& includes,
                      QStringList& watched, QString* error)
{
    QSet<QString> seen;
    collectIncludes(source, dir, includes, watched, seen);
    return PhosphorShaders::ShaderIncludeResolver::expandIncludes(source, dir, includes, error, &watched);
}

QString loadImageParameter(const QString& pack, const QString& value, QImage& image, QString& resolved)
{
    if (value.isEmpty()) { image = {}; resolved.clear(); return {}; }
    if (value.size() > 4096 || value.contains(QChar::Null)) return QStringLiteral("Invalid image path");
    const auto path = expandHome(value);
    if (QDir::isRelativePath(path) && (QDir::cleanPath(path) == QLatin1String("..")
        || QDir::cleanPath(path).startsWith(QLatin1String("../")))) return QStringLiteral("Relative images must stay inside the pack");
    resolved = QFileInfo(QDir(pack).absoluteFilePath(path)).absoluteFilePath();
    constexpr qint64 MaxBytes = 16 * 1024 * 1024;
    // Validate the opened descriptor: a path replacement must not turn a
    // regular asset into a blocking FIFO or device read on the GUI thread.
    const int fd = ::open(QFile::encodeName(resolved).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return QStringLiteral("Cannot open image: %1").arg(value);
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > MaxBytes) {
        ::close(fd);
        return QStringLiteral("Image must be a regular file of at most 16 MiB");
    }
    if (QDir::isRelativePath(path)) {
        const auto openedPath = QFileInfo(QStringLiteral("/proc/self/fd/%1").arg(fd)).canonicalFilePath();
        const auto packPath = QFileInfo(pack).canonicalFilePath() + QLatin1Char('/');
        if (openedPath.isEmpty() || !openedPath.startsWith(packPath)) {
            ::close(fd);
            return QStringLiteral("Relative images must stay inside the pack");
        }
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return QStringLiteral("Cannot read image: %1").arg(value);
    }
    auto bytes = file.read(MaxBytes + 1);
    if (file.error() != QFileDevice::NoError) return QStringLiteral("Cannot read image: %1").arg(value);
    if (bytes.size() > MaxBytes) return QStringLiteral("Image file exceeds 16 MiB");
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
    if (name.isEmpty() || name.size() > 4096 || name.contains(QChar::Null) || QDir::isAbsolutePath(name))
        return QStringLiteral("Geometry asset needs a relative pack path");
    path = QDir(pack).absoluteFilePath(name);
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return QStringLiteral("Cannot open geometry asset: %1").arg(name);
    struct stat st {};
    const auto opened = QFileInfo(QStringLiteral("/proc/self/fd/%1").arg(fd)).canonicalFilePath();
    const auto root = QFileInfo(pack).canonicalFilePath();
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 32 * 1024 * 1024
        || root.isEmpty() || !opened.startsWith(root + QLatin1Char('/'))) {
        ::close(fd);
        return QStringLiteral("Geometry assets must be regular files inside the pack, at most 32 MiB");
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd); return QStringLiteral("Cannot read geometry asset");
    }
    bytes = file.read(32 * 1024 * 1024 + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > 32 * 1024 * 1024)
        return QStringLiteral("Cannot read geometry asset within size limit");
    return {};
}

QString loadGeometryPasses(const QString& pack, AfterglowRender::ShaderProgramSource& source,
                          const QStringList& includes, QStringList& watched)
{
    QByteArray data;
    QString path;
    auto error = readGeometryAsset(pack, QStringLiteral("metadata.json"), data, path);
    watched << path;
    if (!error.isEmpty()) return error;
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(data, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return QStringLiteral("Invalid pack metadata");
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
            if (it.key() != QLatin1String("vertexShader") && it.key() != QLatin1String("mesh")
                && it.key() != QLatin1String("vertexCount") && it.key() != QLatin1String("instances") && it.key() != QLatin1String("samples"))
                return QStringLiteral("Unknown geometry setting: %1").arg(it.key());
        auto& geometry = source.buffers[i].geometry;
        error = readGeometryAsset(pack, object.value(QStringLiteral("vertexShader")).toString(), data, path);
        watched << path;
        if (!error.isEmpty()) return error;
        geometry.vertexCode = expandWatched(
            QString::fromUtf8(data), QFileInfo(path).absolutePath(), includes, watched, &error).toUtf8();
        if (!error.isEmpty()) return error;
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
        if (object.contains(QStringLiteral("mesh"))) {
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
    connect(&m_files, &QFileSystemWatcher::fileChanged, this, [this] {
        refreshWatches();
        m_cache.clear();
        m_failed.clear();
        Q_EMIT changed();
    });
    connect(&m_files, &QFileSystemWatcher::directoryChanged, this, [this] {
        // Unrelated writes beside a shader do not trigger recompilation.
        if (!refreshWatches()) return;
        m_cache.clear();
        m_failed.clear();
        Q_EMIT changed();
    });
}

ShaderLibrary::~ShaderLibrary() = default;

bool ShaderLibrary::refreshWatches()
{
    QSet<QString> files, directories;
    for (const auto& dependencies : m_dependencies) for (const auto& path : dependencies) {
        if (path.isEmpty()) continue;
        const QFileInfo info(path);
        if (info.isFile()) files.insert(info.absoluteFilePath());
        auto directory = info.absolutePath();
        while (!QFileInfo(directory).isDir()) {
            const auto parent = QFileInfo(directory).absolutePath();
            if (parent == directory) break;
            directory = parent;
        }
        if (QFileInfo(directory).isDir()) directories.insert(directory);
    }
    const auto oldFiles = m_files.files(), oldDirectories = m_files.directories();
    const QSet<QString> previousFiles(oldFiles.begin(), oldFiles.end());
    const QSet<QString> previousDirectories(oldDirectories.begin(), oldDirectories.end());
    const auto removed = (previousFiles - files) | (previousDirectories - directories);
    const auto added = (files - previousFiles) | (directories - previousDirectories);
    if (!removed.isEmpty()) m_files.removePaths(removed.values());
    if (!added.isEmpty()) m_files.addPaths(added.values());
    return !removed.isEmpty() || !added.isEmpty();
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
    for (const auto& chain : {refs.background, refs.text, refs.cursor, refs.pane, refs.window})
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
    AfterglowRender::ShaderProgramSource src;
    src.id = ref;
    QString path;
    QString dir;
    if (isFileReference(ref)) {
        // A raw Ghostty-style shader: mainImage and the shared uniforms, no metadata.
        path = QFileInfo(expandHome(ref)).absoluteFilePath();
        watched << path;
        dir = QFileInfo(path).absolutePath();
    } else {
        const PhosphorShaders::ShaderRegistry::ShaderInfo info = findPack(*m_registry, ref);
        if (!info.isValid()) {
            const QString error = QStringLiteral("no shader pack named \"%1\" (available: %2)")
                                      .arg(ref, packIds().join(QStringLiteral(", ")));
            qCWarning(lcShaders).noquote() << error;
            m_failed.insert(key, error);
            return nullptr;
        }
        if (info.bufferShaderPaths.size() > AfterglowRender::MaxShaderBuffers) {
            m_failed.insert(key, QStringLiteral("Too many buffer passes")); return nullptr;
        }
        watched << info.sourcePath << QDir(info.packDir).filePath(QStringLiteral("metadata.json"));
        src.useDepthBuffer = info.useDepthBuffer;
        if (!info.vertexShaderPath.isEmpty()) {
            watched << info.vertexShaderPath;
            QFile vertex(info.vertexShaderPath);
            QString error;
            if (!vertex.open(QIODevice::ReadOnly)) error = QStringLiteral("Cannot read vertex shader: %1").arg(info.vertexShaderPath);
            else src.vertexCode = expandWatched(
                QString::fromUtf8(vertex.readAll()), QFileInfo(info.vertexShaderPath).absolutePath(), m_includeDirs, watched, &error).toUtf8();
            if (!error.isEmpty()) { m_failed.insert(key, error); qCWarning(lcShaders) << error; return nullptr; }
        }
        for (int index = 0; index < info.bufferShaderPaths.size(); ++index) {
            const auto bufferPath = info.bufferShaderPaths[index];
            watched << bufferPath;
            QFile buffer(bufferPath);
            QString error;
            AfterglowRender::ShaderBufferSource pass;
            if (!buffer.open(QIODevice::ReadOnly)) error = QStringLiteral("Cannot read buffer shader: %1").arg(bufferPath);
            else pass.code = expandWatched(
                QString::fromUtf8(buffer.readAll()), QFileInfo(bufferPath).absolutePath(), m_includeDirs, watched, &error).toUtf8();
            if (!error.isEmpty()) { qCWarning(lcShaders) << error; m_failed.insert(key, error); return nullptr; }
            pass.feedback = info.bufferFeedback;
            pass.halfFloat = info.halfFloatBuffers;
            pass.scale = float(info.bufferScale);
            const auto filter = info.bufferFilters.value(index, info.bufferFilter);
            const auto wrap = info.bufferWraps.value(index, info.bufferWrap);
            if ((filter != QLatin1String("linear") && filter != QLatin1String("nearest"))
                || (wrap != QLatin1String("clamp") && wrap != QLatin1String("repeat") && wrap != QLatin1String("mirror"))) {
                m_failed.insert(key, QStringLiteral("Invalid buffer filter/wrap")); return nullptr;
            }
            pass.nearest = filter == QLatin1String("nearest");
            pass.wrap = wrap == QLatin1String("repeat") ? AfterglowRender::ShaderWrap::Repeat
                : wrap == QLatin1String("mirror") ? AfterglowRender::ShaderWrap::Mirror : AfterglowRender::ShaderWrap::Clamp;
            src.buffers.push_back(std::move(pass));
        }
        const auto geometryError = loadGeometryPasses(info.packDir, src, m_includeDirs, watched);
        if (!geometryError.isEmpty()) {
            m_failed.insert(key, geometryError); qCWarning(lcShaders) << ref << geometryError; return nullptr;
        }
        path = info.sourcePath;
        dir = info.packDir;
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
                if (!error.isEmpty()) { m_failed.insert(key, error); qCWarning(lcShaders) << error; return nullptr; }
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
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        const QString error = QStringLiteral("cannot read shader %1").arg(path);
        qCWarning(lcShaders).noquote() << error;
        m_failed.insert(key, error);
        return nullptr;
    }
    QString includeError;
    const QString code = expandWatched(
        QString::fromUtf8(f.readAll()), dir, m_includeDirs, watched, &includeError);
    if (!includeError.isEmpty()) {
        qCWarning(lcShaders).noquote() << ref << ":" << includeError;
        m_failed.insert(key, includeError);
        return nullptr;
    }
    src.code = code.toUtf8();

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
