// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShaderLibrary.h"

#include <PhosphorShaders/ShaderIncludeResolver.h>
#include <PhosphorShaders/ShaderRegistry.h>

#include <QColor>
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
        for (const auto& p : info.parameters) {
            if (p.slot < 0) {
                continue;
            }
            const QVariant v = overrides.contains(p.id) ? overrides.value(p.id) : p.defaultValue;
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
