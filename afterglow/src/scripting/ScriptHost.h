// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <PhosphorFsLoader/IScanStrategy.h>

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <vector>

namespace PhosphorScripting {
class LuauEngine;
class LuauWatchdog;
} // namespace PhosphorScripting

namespace PhosphorFsLoader {
class WatchedDirectorySet;
}

namespace Afterglow {

class ScriptTasks;
struct ScriptPackage;

/// Luau scripts that react to terminal events and add actions. Each script
/// runs in its own sandboxed engine with a heap cap and a time limit per
/// call. Handlers do not touch the terminal: they return effects, which the
/// host checks, rate limits and hands to @ref Callbacks::apply.
///
/// Scripts load from the search path, lowest priority first; a script with
/// the same id in a later directory replaces the earlier one. Files ending in
/// .d.luau are type definitions and are skipped. Edits reload on the fly.
class ScriptHost : public QObject, private PhosphorFsLoader::IScanStrategy
{
    Q_OBJECT

public:
    struct EffectResult
    {
        QString error; ///< empty on success
        QVariantMap data;
        QString code; ///< stable error category, empty on success
        bool deferred = false; ///< completion is delivered asynchronously
    };

    struct Callbacks
    {
        /// The pane as scripts see it, empty if there is no such pane.
        std::function<QVariantMap(quint32 pane)> paneInfo;
        /// A detached snapshot of terminal metadata, without terminal text.
        std::function<QVariantMap()> state;
        /// Resolves generic actions to the same budget class as direct effects.
        std::function<bool(const QString& action)> structuralAction;
        /// Carries out a checked effect, preserving created object identities.
        std::function<EffectResult(const QVariantMap& effect)> apply;
        /// Owned UI/override operations. Completion is used only when deferred=true.
        std::function<EffectResult(const QString& owner, const QVariantMap&, std::function<void(EffectResult)>)> applyOwned;
        std::function<void(const QString& owner)> cleanup;
        /// A handler of the owner failed: whatever it holds must not trap the user.
        std::function<void(const QString& owner)> failed;
    };

    struct ScriptAction
    {
        QString token; ///< "<script id>.<action id>"
        QString title;
    };

    static constexpr int EventTimeoutMs = 20;
    static constexpr int ActionTimeoutMs = 100;
    static constexpr int MaxFailures = 5; ///< in a row, before a script is disabled
    static constexpr int MaxEffectsPerSecond = 20;
    static constexpr int MaxStructuralPer10s = 3; ///< splits, tabs, windows and closes
    static constexpr int MaxEffectsPerCall = 64;
    static constexpr int MaxPendingResults = 64;
    static constexpr int MaxResultsPerSecond = 64;
    static constexpr int StateVersion = 1;
    static constexpr std::size_t MemoryCapBytes = 16u * 1024 * 1024;

    explicit ScriptHost(Callbacks callbacks, QObject* parent = nullptr);
    ~ScriptHost() override;

    /// The system data directories, then ~/.config/afterglow/scripts.
    static QStringList defaultSearchPaths();
    void setStateDirectory(const QString& directory);
    void setSearchPaths(const QStringList& dirs);
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }
    /// Rescans the search path now instead of waiting for a file change.
    void reload();

    QStringList scriptIds() const;
    QVariantList diagnostics() const;
    /// Whether a script stopped running after repeated failures.
    bool isDisabled(const QString& id) const;
    QList<ScriptAction> actions() const;
    /// The shader directories of loaded packages, by package ID.
    QMap<QString, QString> packageShaders() const { return m_packageShaders; }
    /// Shader settings loaded packages suggest: {package, key, ref}.
    QVariantList packageSuggestions() const;

    /// Runs a script action with @p pane (0 for none) as its event's pane.
    /// False if there is no such action.
    bool runAction(const QString& token, quint32 pane);
    /// Calls @p handler (on_command_finished...) in every script that has it.
    /// The event's "pane" is filled in from @p pane.
    void dispatch(const QString& handler, quint32 pane, QVariantMap event = {});
    /// Calls @p handler in the script instance @p owner only (on_key).
    void dispatchTo(const QString& owner, const QString& handler, quint32 pane, QVariantMap event = {});

Q_SIGNALS:
    void actionsChanged();
    /// Loaded packages' shader directories changed (packageShaders()).
    void packageShadersChanged(const QMap<QString, QString>& directories);
    /// After every effect, with the error if it was refused or failed.
    void effectApplied(const QString& script, const QVariantMap& effect, const QString& error);

private:
    struct Script;
    using ScriptPtr = std::shared_ptr<Script>;

    ScriptPtr load(const QString& path, const QByteArray& source, const ScriptPackage* package = nullptr) const;
    void call(const ScriptPtr& script, const QString& function, const QVariantList& args, int timeoutMs,
              quint32 eventPane);
    void applyResult(const ScriptPtr& script, const QVariant& result, quint32 eventPane);
    void applyEffect(const ScriptPtr& script, const QVariantMap& effect, quint32 eventPane);
    QVariantMap state() const;
    void queueResult(const ScriptPtr& script, const QString& request, const QString& kind,
                     const EffectResult& result, quint32 eventPane, int timeoutMs);
    bool withinBudget(Script& script, bool structural);
    /// Loads the scripts in @p directories, lowest priority first, and
    /// returns the files and package folders to watch.
    QStringList performScan(const QStringList& directories) override;
    void retire(const ScriptPtr& script);
    void emitOwned(const ScriptPtr&, const QString& handler, QVariantMap event, quint32 pane);

    Callbacks m_callbacks;
    std::unique_ptr<ScriptTasks> m_tasks;
    std::shared_ptr<PhosphorScripting::LuauWatchdog> m_watchdog;
    QByteArray m_prelude;
    QStringList m_searchPaths;
    QString m_stateDirectory;
    mutable QMap<QString, QString> m_loadErrors;
    bool m_enabled = false;
    std::vector<ScriptPtr> m_scripts;
    QMap<QString, QString> m_packageShaders;
    QElapsedTimer m_clock;
    // Last, so a pending rescan cannot run into a partly destroyed host.
    std::unique_ptr<PhosphorFsLoader::WatchedDirectorySet> m_directories; ///< while enabled
};

} // namespace Afterglow
