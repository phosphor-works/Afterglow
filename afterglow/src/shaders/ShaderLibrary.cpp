// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderLibrary.h"

#include <PhosphorShaders/ShaderIncludeResolver.h>
#include <PhosphorShaders/ShaderRegistry.h>

#include <QColor>
#include <algorithm>
#include <cmath>
#include <limits>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QStandardPaths>

Q_LOGGING_CATEGORY(lcShaders, "afterglow.shaders", QtInfoMsg)

namespace Afterglow {

namespace {

QString expandHome(const QString& path)
{
    return path.startsWith(QLatin1String("~/")) ? QDir::homePath() + path.mid(1) : path;
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

    connect(m_registry.get(), &PhosphorShaders::ShaderRegistry::shadersChanged, this, [this] {
        m_cache.clear();
        m_failed.clear();
        Q_EMIT changed();
    });
    connect(&m_files, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        // Editors replace files: watch the new one.
        if (QFile::exists(path) && !m_files.files().contains(path)) {
            m_files.addPath(path);
        }
        m_cache.clear();
        m_failed.clear();
        Q_EMIT changed();
    });
}

ShaderLibrary::~ShaderLibrary() = default;

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

QVariantList ShaderLibrary::parameters(const QString& ref) const
{
    QVariantList result;
    for (const auto& p : findPack(*m_registry, ref).parameters) {
        const bool supported = p.slot >= 0 && ((p.type == QLatin1String("color") && p.slot < 16)
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
        } else return QStringLiteral("Unsupported parameter type: %1").arg(p->type);
        if (p->slot < 0 || p->slot >= (p->type == QLatin1String("color") ? 16 : 32))
            return QStringLiteral("Parameter has no supported uniform slot");
    }
    if (!m_registry->validateParams(info.id, values)) return QStringLiteral("Shader parameter outside its declared range");
    const auto validated = m_registry->validateAndCoerceParams(info.id, values);
    for (const auto& p : info.parameters) {
        if (!values.contains(p.id)) continue;
        const auto v = validated.value(p.id);
        if (p.type == QLatin1String("color")) {
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
    auto add = [&](const QStringList& from, std::vector<AfterglowRender::ShaderProgramPtr>& to) {
        for (const QString& ref : from) {
            if (auto p = load(ref, params.value(ref).toMap())) {
                to.push_back(std::move(p));
            }
        }
    };
    add(refs.background, chains.background);
    add(refs.text, chains.text);
    add(refs.cursor, chains.cursor);
    add(refs.pane, chains.pane);
    add(refs.window, chains.window);
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

    AfterglowRender::ShaderProgramSource src;
    src.id = ref;
    QString path;
    QString dir;
    if (isFileReference(ref)) {
        // A raw Ghostty-style shader: mainImage and the shared uniforms, no metadata.
        path = expandHome(ref);
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
            if (p.type == QLatin1String("color")) {
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
    const QString code = PhosphorShaders::ShaderIncludeResolver::expandIncludes(
        QString::fromUtf8(f.readAll()), dir, m_includeDirs, &includeError);
    if (!includeError.isEmpty()) {
        qCWarning(lcShaders).noquote() << ref << ":" << includeError;
        m_failed.insert(key, includeError);
        return nullptr;
    }
    src.code = code.toUtf8();
    if (!m_files.files().contains(path)) {
        m_files.addPath(path);
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
