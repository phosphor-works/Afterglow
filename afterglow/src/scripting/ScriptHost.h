// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <PhosphorFsLoader/IScanStrategy.h>

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
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
/// host checks and rate limits. It keeps state, sleeps, logs and actions
/// itself, gives timers, processes and file reads to its tasks, and hands
/// the rest to @ref Callbacks::applyOwned or @ref Callbacks::apply.
///
/// The search path lists directories lowest priority first; a script with
/// the same id in a later directory, a package beside a file with the same
/// id, or a file whose name sorts later in the same directory replaces the
/// other one. Files ending in .d.luau are type definitions and are skipped.
/// Edits reload on the fly.
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
    /// Effects that create, close or move panes, tabs and windows (Kinds[]
    /// marks them), and the generic actions that do.
    static constexpr int MaxStructuralPer10s = 3;
    static constexpr int MaxEffectsPerCall = 64;
    static constexpr int MaxActions = 32; ///< palette actions of one script
    static constexpr int MaxPendingResults = 64;
    static constexpr int MaxResultsPerSecond = 64;
    /// Handler calls for one script each second: events, timers and watchers.
    static constexpr int MaxCallbacksPerSecond = 64;
    static constexpr int MaxSleeping = 16;
    static constexpr int MaxSleepMs = 600000;
    static constexpr int MaxLogCharacters = 2048;
    static constexpr qint64 MaxScriptBytes = 1024 * 1024; ///< a single-file script
    /// The version of the terminal snapshot in ev.terminal, not of package state.
    static constexpr int SnapshotVersion = 1;
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
    /// A test hook: the application relies on the file watches.
    void reload();

    /// The loaded scripts' ids. A test hook; users see diagnostics().
    QStringList scriptIds() const;
    QVariantList diagnostics() const;
    /// Whether a script stopped running after repeated failures. A test hook.
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
    /// After every effect, with the error if it was refused or failed. Tests
    /// observe effects through it.
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
    /// Where a deferred effect's completion goes: counted, then queued as its result.
    std::function<void(EffectResult)> completion(const ScriptPtr& script, const QString& request,
                                                 const QString& kind, quint32 eventPane);
    bool withinBudget(Script& script, bool structural);
    /// Counts a handler call against the script's callback budget.
    bool admitCallback(Script& script);
    /// The event's pane: its record, or only its id once it has closed.
    QVariantMap eventPane(quint32 pane) const;
    /// Stops a script at run time and withdraws what it contributes.
    void disable(const ScriptPtr& script);
    void announceActions(const QList<ScriptAction>& before);
    void publishPackageShaders();
    /// Creates or drops the directory set to match m_enabled.
    void applyEnabled();
    /// Loads the scripts in @p directories, which are ordered lowest priority
    /// first, and returns the files and package folders to watch. The scan
    /// visits candidates highest priority first: the first to claim an id keeps it.
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
    /// What the last scan learned about a source it did not keep: why it
    /// failed, or which id it has when another script took that id. A rescan
    /// does not run the same source again.
    struct Memo
    {
        QByteArray stamp;
        QString id;
        QString error;
        bool state = false; ///< the error came from the stored state, whose file is watched
    };
    QHash<QString, Memo> m_memos;
    QMap<QString, QString> m_superseded; ///< path -> the path of the script that took its id
    mutable QSet<QString> m_stateFailures; ///< packages that failed on their stored state
    bool m_enabled = false;
    bool m_scanning = false;
    std::vector<ScriptPtr> m_scripts;
    QMap<QString, QString> m_packageShaders;
    QElapsedTimer m_clock;
    // Last, so a pending rescan cannot run into a partly destroyed host.
    std::unique_ptr<PhosphorFsLoader::WatchedDirectorySet> m_directories; ///< while enabled
};

} // namespace Afterglow
