// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowRender/PostChain.h>
#include <AfterglowRender/ShaderProgram.h>

#include <PhosphorFsLoader/WatchedFileSet.h>

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <memory>
#include <vector>

namespace PhosphorAnimation {
class CurveRegistry;
}

namespace PhosphorShaders {
class ShaderRegistry;
class ShaderPresetStore;
}

namespace Afterglow {

/// Terminal shader packs by id: Phosphor shader packs (a directory with
/// metadata.json and its stages) written against the terminal contract in
/// data/shaders/shared. Packs come from ~/.config/afterglow/shaders, the system
/// data directories and the bundled packs. Compiled programs are cached and
/// rebuilt when their files change.
class ShaderLibrary : public QObject
{
    Q_OBJECT

public:
    explicit ShaderLibrary(QObject* parent = nullptr);
    ~ShaderLibrary() override;

    /// Shader references for each chain (see AfterglowRender::ShaderChains).
    struct Refs
    {
        QStringList background;
        QStringList text;
        QStringList cursor;
        QStringList pane;
        QStringList window;
        QString overview; ///< the pane overview; empty for none
        bool operator==(const Refs&) const = default;
    };
    /// Compiles the configured chains. Shaders that fail are logged and skipped.
    /// @p params maps a reference to its parameter overrides.
    AfterglowRender::ShaderChains build(const Refs& refs, const QVariantMap& params);

    QStringList packIds() const;
    QVariantList presets(const QString& ref) const;
    QString presetParameters(const QString& ref, const QString& id, QVariantMap& values) const;
    QString savePreset(const QString& ref, const QString& name, const QVariantMap& values, QVariantMap& result);
    QVariantList parameters(const QString& ref) const;
    QString validateParameters(const QString& ref, const QVariantMap& values,
                               AfterglowRender::ShaderParameterValues& packed) const;
    /// Curves for parameter transitions.
    const PhosphorAnimation::CurveRegistry& curves() const { return *m_curves; }

Q_SIGNALS:
    /// A pack or shader file changed on disk; chains should be rebuilt.
    void changed();

private:
    AfterglowRender::ShaderProgramPtr load(const QString& ref, const QVariantMap& params);

    void seedPresets();
    void refreshWatches();
    std::unique_ptr<PhosphorShaders::ShaderRegistry> m_registry;
    std::unique_ptr<PhosphorShaders::ShaderPresetStore> m_presets;
    std::unique_ptr<PhosphorAnimation::CurveRegistry> m_curves;
    QStringList m_includeDirs;
    PhosphorFsLoader::WatchedFileSet m_files;
    QHash<QString, QStringList> m_dependencies; // cache key -> resolved and missing candidate paths
    QHash<QString, AfterglowRender::ShaderProgramPtr> m_cache;
    QHash<QString, QString> m_failed; // cache key -> error, so a broken shader is reported once
};

} // namespace Afterglow
