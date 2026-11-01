// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ShaderRefs.h"

#include <AfterglowRender/PostChain.h>
#include <AfterglowRender/ShaderProgram.h>

#include <PhosphorFsLoader/WatchedFileSet.h>

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

#include <array>
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
/// data directories and the bundled packs, and from loaded script packages'
/// shaders/ directories as "<package>.<pack>". When two packs answer to one
/// name, the user's wins over the system's, and the system's over the bundled.
/// A pack compiles once and is cached until its files change; the config's
/// parameter values are applied over a copy, so changing them does not
/// compile it again.
class ShaderLibrary : public QObject
{
    Q_OBJECT

public:
    /// Decoded images that the configured chains, or the script overrides of
    /// one window, may hold at once.
    static constexpr qint64 MaxDecodedImageBytes = 256LL * 1024 * 1024;

    explicit ShaderLibrary(QObject* parent = nullptr);
    ~ShaderLibrary() override;

    using Refs = ShaderRefs;
    /// Compiles the configured chains. Shaders that fail are logged and skipped.
    /// @p params maps a reference to its parameter overrides.
    AfterglowRender::ShaderChains build(const Refs& refs, const QVariantMap& params);
    /// Script packages' shader directories, by package ID. Their packs are
    /// referenced by "<package>.<pack>" only, while the package is loaded.
    void setPackageShaders(const QMap<QString, QString>& directories);

    QStringList packIds() const;
    QVariantList presets(const QString& ref) const;
    QString presetParameters(const QString& ref, const QString& id, QVariantMap& values) const;
    QString savePreset(const QString& ref, const QString& name, const QVariantMap& values, QVariantMap& result);
    QVariantList parameters(const QString& ref) const;
    /// Checks values that a script or a preset sends. Image paths must stay
    /// inside the pack: only the config file can name an image elsewhere.
    QString validateParameters(const QString& ref, const QVariantMap& values,
                               AfterglowRender::ShaderParameterValues& packed) const;
    /// Curves for parameter transitions.
    const PhosphorAnimation::CurveRegistry& curves() const { return *m_curves; }
    /// What the last build() could not use, one line each: Shaders.params
    /// entries that apply nowhere, and configured packs that do not draw
    /// (no such pack, a compile error, an image, the image budget). Each is
    /// also logged, with the full compile log.
    QStringList problems() const { return m_problems; }
    /// The files whose change rebuilds: the configured packs' stages,
    /// includes and images, found or not yet created.
    QStringList watchedPaths() const { return m_files.paths(); }

Q_SIGNALS:
    /// A configured pack or one of its files changed on disk, or script
    /// packages loaded or unloaded: chains should be rebuilt. Rebuilding
    /// retires every script override.
    void changed();
    /// problems() changed.
    void problemsChanged(const QStringList& problems);

private:
    struct PackIndex;

    /// A pack compiled once, with its own defaults, for every set of
    /// configured values: a Shaders.params change does not compile again.
    struct Compiled
    {
        AfterglowRender::ShaderProgramPtr program; ///< null when it failed
        QString error;
        QStringList watched;   ///< resolved and missing candidate paths of its files
        QByteArray signature;  ///< their state when it compiled
        QString packDir;       ///< the pack its reference named
        /// Why a default image did not load, by slot. The program has no
        /// image there, and only a configured image for that slot can use it.
        std::array<QString, AfterglowRender::MaxShaderImages> imageErrors;
    };

    /// The pack @p ref names with the config's values @p params over its defaults.
    AfterglowRender::ShaderProgramPtr load(const QString& ref, const QVariantMap& params);
    Compiled compile(const QString& ref);
    AfterglowRender::ShaderProgramPtr configure(const QString& key, const QString& ref, const QVariantMap& params,
                                                QStringList& watched);
    /// The shared/ directories a pack's includes search, most important first:
    /// its own package's, then m_baseDirectories from the user's down.
    QStringList includeDirs(const QString& packDir) const;
    /// Drops cached and failed programs whose files changed or whose
    /// reference now names another pack. True if it dropped any.
    bool invalidate();
    void reindex();

    void seedPresets();
    void refreshWatches();
    std::unique_ptr<PhosphorShaders::ShaderRegistry> m_registry;
    /// Bundled, system and user packs, lowest priority first. A build-tree
    /// binary puts the checkout's packs above the system ones.
    QStringList m_baseDirectories;
    QMap<QString, QString> m_packageShaders; ///< script packages' pack directories, by package ID
    /// A registry over the base directories and the packages' directories.
    void buildRegistry();
    std::unique_ptr<PackIndex> m_index; ///< references to packs, by priority
    std::unique_ptr<PhosphorShaders::ShaderPresetStore> m_presets;
    std::unique_ptr<PhosphorAnimation::CurveRegistry> m_curves;
    PhosphorFsLoader::WatchedFileSet m_files;
    QHash<QString, Compiled> m_compiled; // reference -> its pack, compiled with the pack's defaults
    QHash<QString, QStringList> m_dependencies; // cache key -> resolved and missing candidate paths
    QHash<QString, QByteArray> m_signatures; // cache key -> the state of its dependencies when loaded
    QHash<QString, QString> m_resolved; // cache key -> the pack directory its reference named
    QHash<QString, AfterglowRender::ShaderProgramPtr> m_cache;
    QHash<QString, QString> m_failed; // cache key -> error, so a broken shader is reported once
    QSet<QString> m_desired; // cache keys of the last build
    QSet<QString> m_overBudget; // keys of m_desired dropped for the image budget, not loaded again
    QString m_paramsChecked; // the Shaders.params and references last checked for unused entries
    QStringList m_paramsProblems; // what that check found
    QStringList m_problems;
};

} // namespace Afterglow
