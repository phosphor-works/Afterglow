// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowRender/PostChain.h>
#include <AfterglowRender/ShaderProgram.h>

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <memory>
#include <vector>

namespace PhosphorShaders {
class ShaderRegistry;
}

namespace Afterglow {

/// Terminal shaders by reference: a phosphor shader pack id (a directory with
/// metadata.json and its fragment shader) or a path to a raw Ghostty-style
/// shader file. Packs come from ~/.config/afterglow/shaders, the system data
/// directories and the bundled packs. Compiled programs are cached and rebuilt
/// when their files change.
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
        bool operator==(const Refs&) const = default;
    };
    /// Compiles the configured chains. Shaders that fail are logged and skipped.
    /// @p params maps a reference to its parameter overrides.
    AfterglowRender::ShaderChains build(const Refs& refs, const QVariantMap& params);

    QStringList packIds() const;
    QVariantList parameters(const QString& ref) const;
    QString validateParameters(const QString& ref, const QVariantMap& values,
                               AfterglowRender::ShaderParameterValues& packed) const;

Q_SIGNALS:
    /// A pack or shader file changed on disk; chains should be rebuilt.
    void changed();

private:
    AfterglowRender::ShaderProgramPtr load(const QString& ref, const QVariantMap& params);

    std::unique_ptr<PhosphorShaders::ShaderRegistry> m_registry;
    QStringList m_includeDirs;
    QFileSystemWatcher m_files;
    QHash<QString, AfterglowRender::ShaderProgramPtr> m_cache;
    QHash<QString, QString> m_failed; // cache key -> error, so a broken shader is reported once
};

} // namespace Afterglow
