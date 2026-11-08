// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ScriptPackage.h"

#include <PhosphorFsLoader/IScanStrategy.h>

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <optional>
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

/// Luau scripts that react to terminal events and add commands. Each script
/// runs in its own sandboxed engine with a heap cap and a time limit per
/// call. Handlers do not touch the terminal: they return effects, which the
/// host checks and rate limits. It keeps state, sleeps, logs, commands and
/// their runs itself, gives timers, processes and file reads to its tasks, and hands
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
        /// Closes the dialogs and the overlays that the owner opened with
        /// request IDs that start with @p prefix: the effects that an ended
        /// run waited for.
        std::function<void(const QString& owner, const QString& prefix)> closeRunWaits;
        /// Afterglow's own notice: on @p pane, or a desktop notification that
        /// belongs to no pane when it is 0 or has closed.
        std::function<void(quint32 pane, const QString& title, const QString& body)> notice;
    };

    /// Where a command run's output goes. A run with no client (the palette,
    /// a key, the menu, `afterglow ctl action`) has no finished function: its
    /// stdout is dropped, and its stderr and a non-zero status show as a
    /// notification.
    struct CommandSink
    {
        /// Takes @p text for stdout or stderr, and calls @p sent once: with
        /// true once the text has gone to the client, which can be later (a
        /// client that reads slowly holds it back), or with false when the
        /// text is dropped (the client went, or cancelled). Returns false
        /// when the client has gone: the run is cancelled.
        using Write = std::function<bool(const QString& text, std::function<void(bool delivered)> sent)>;
        Write output;
        Write error;
        std::function<void(int status)> finished;
        /// Whether text for stderr (@p error) or stdout goes to a client: a
        /// run with no client drops its stdout and keeps its stderr.
        bool toClient(bool error) const { return finished && (error ? this->error : output); }
    };

    struct CommandRequest
    {
        QString script;
        QString command;
        QStringList args;
        QString cwd; ///< empty: the pane's directory
        quint32 pane = 0;
        /// The client's stdin, for a command with stdin. CommandSocket
        /// refuses more than Scripts.commandStdinLimit: this is never cut.
        std::optional<QString> input;
        QString source; ///< shell, palette, key, menu, control or remote
        /// For source "remote": the host of the ssh session, ev.remote.host.
        /// Only a command that declares remote runs from there.
        QString remoteHost;
    };

    /// Scripts.command*: see their descriptions in Settings.cpp. The
    /// defaults are those of Settings, which ConfigTest checks.
    struct CommandLimits
    {
        int timeoutMs = 600000;
        qint64 inputBytes = 1024 * 1024;
        int runs = 16;
        qint64 outputBytes = 16 * 1024 * 1024;
    };

    static constexpr int EventTimeoutMs = 20;
    /// Each part of a command handler, a package's on_load and migrate.
    static constexpr int CommandCallTimeoutMs = 100;
    static constexpr int MaxFailures = 5; ///< in a row, before a script is disabled
    static constexpr int MaxEffectsPerSecond = 20;
    /// dock effects, which have a budget of their own: a dock that follows
    /// the pointer or a clock must not use up the budget of the rest.
    static constexpr int MaxDockEffectsPerSecond = 60;
    /// Effects that create, close or move panes, tabs and windows (Kinds[]
    /// marks them), and the generic actions that do.
    static constexpr int MaxStructuralPer10s = 3;
    static constexpr int MaxEffectsPerCall = 64;
    static constexpr int MaxCommandArgs = 256;
    static constexpr int MaxCommandArgLength = 4096; ///< UTF-16 units of one argument
    /// The exit status of a run that ran out of time.
    static constexpr int CommandTimedOut = 124;
    /// Results, queued or each second, disable a script past these: a
    /// handler that answers each result with a new request loops without
    /// end. A script that requests every effect its budgets let it apply,
    /// in one pass of the event loop, must stay under them. The output of
    /// a run with a client that went out to it is not counted each second:
    /// the run's output limit and time limit bound it, and a command prints
    /// line by line. Output that no client takes is counted.
    static constexpr int MaxPendingResults = 128;
    static constexpr int MaxResultsPerSecond = 128;
    static_assert(MaxPendingResults > MaxEffectsPerSecond + MaxDockEffectsPerSecond);
    static_assert(MaxResultsPerSecond > MaxEffectsPerSecond + MaxDockEffectsPerSecond);
    /// Handler calls for one script each second, once for events and once
    /// for its timers and watchers: a title storm must not stop its timers.
    /// Keys, pointer clicks on docks, dock hovers, dock layouts and on_load
    /// are not counted: dropping one would lose a state that no later call
    /// tells again. Wheel turns and accessibility presses on docks count.
    static constexpr int MaxCallbacksPerSecond = 64;
    static constexpr int MaxSleeping = 16;
    static constexpr int MaxSleepMs = 600000;
    static constexpr int MaxLogCharacters = 2048;
    static constexpr qint64 MaxScriptBytes = 1024 * 1024; ///< a single-file script
    /// The highest max_bytes of read_text and read_region, which Control also takes.
    static constexpr int MaxReadTextBytes = 1024 * 1024;
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
    /// The commands of the running scripts: by script, and in a script
    /// the manifest's by name, then those of set_commands by name.
    QList<ScriptCommand> commands() const;
    void setCommandLimits(const CommandLimits& limits) { m_limits = limits; }
    const CommandLimits& commandLimits() const { return m_limits; }
    /// The shader directories of loaded packages, by package ID.
    QMap<QString, QString> packageShaders() const { return m_packageShaders; }
    /// Shader settings loaded packages suggest: {package, key, ref}.
    QVariantList packageSuggestions() const;

    /// Starts a run of a command. Returns its handle, or 0 with @p error
    /// set; @p unknown says that there is no such script or command. The run
    /// can end, and @p sink finish, before this returns.
    quint64 startCommand(const CommandRequest& request, CommandSink sink, QString* error, bool* unknown = nullptr);
    /// Starts @p token ("<script id>.<command>") with no arguments and no
    /// client, as the palette, a key, the menu and `afterglow ctl action` do.
    /// Returns an error, or empty.
    QString runCommand(const QString& token, quint32 pane, const QString& source);
    /// Ends a run with @p status, and stops what its handler waits for. A
    /// handle that has ended already is ignored.
    void cancelCommand(quint64 run, int status, const QString& message = {});
    /// Calls @p handler (on_command_finished...) in every script that has it.
    /// The event's "pane" is filled in from @p pane.
    void dispatch(const QString& handler, quint32 pane, QVariantMap event = {});
    /// Calls @p handler in the script instance @p owner only (on_key). With
    /// @p limited, the call counts toward the callback limit like any event.
    void dispatchTo(const QString& owner, const QString& handler, quint32 pane, QVariantMap event = {}, bool limited = false);

Q_SIGNALS:
    void commandsChanged();
    /// Loaded packages' shader directories changed (packageShaders()).
    void packageShadersChanged(const QMap<QString, QString>& directories);
    /// After every effect, with the error if it was refused or failed. Tests
    /// observe effects through it.
    void effectApplied(const QString& script, const QVariantMap& effect, const QString& error);

private:
    struct Script;
    using ScriptPtr = std::shared_ptr<Script>;

    ScriptPtr load(const QString& path, const QByteArray& source, const ScriptPackage* package = nullptr) const;
    /// The call that runs now, which its effects belong to.
    struct CallContext
    {
        const Script* script = nullptr;
        quint64 run = 0; ///< the command run whose handler it is, 0 for none
        bool ended = false; ///< that run had ended before the call
        bool done = false; ///< the run's handler has returned
        /// The flows that the call registered: (flowsBefore, flowsAfter].
        qint64 flowsBefore = 0, flowsAfter = 0;
    };
    /// Runs a handler. @p run is the command run whose handler it is, 0 for none.
    void call(const ScriptPtr& script, const QString& function, const QVariantList& args, int timeoutMs,
              quint32 eventPane, quint64 run = 0);
    void applyResult(const ScriptPtr& script, const QVariant& result, quint32 eventPane);
    void applyEffect(const ScriptPtr& script, const QVariantMap& effect, quint32 eventPane);
    QVariantMap state() const;
    /// @p counted: whether it counts toward MaxResultsPerSecond.
    void queueResult(const ScriptPtr& script, const QString& request, const QString& kind,
                     const EffectResult& result, quint32 eventPane, int timeoutMs, bool counted = true);
    /// Where a deferred effect's completion goes: counted, then queued as its result.
    std::function<void(EffectResult)> completion(const ScriptPtr& script, const QString& request,
                                                 const QString& kind, quint32 eventPane);
    bool withinBudget(Script& script, const QString& kind, bool structural);
    /// Counts a handler call against the script's callback budget: the one
    /// for events, or for @p task the one for its timers and watchers.
    bool admitCallback(Script& script, bool task = false);
    /// The event's pane: its record, or only its id once it has closed.
    QVariantMap eventPane(quint32 pane) const;
    /// Stops a script at run time and withdraws what it contributes.
    /// @p reason ends its runs.
    void disable(const ScriptPtr& script, const QString& reason);
    void announceCommands(const QList<ScriptCommand>& before);
    /// Ends the run @p handle of @p script. @p message goes to its stderr.
    /// @p waiting: its handler may still wait. What it waits for then
    /// stops, and it resumes once with "cancelled", in a later pass of the
    /// event loop: the run can end inside an effect of a handler.
    void finishRun(const ScriptPtr& script, quint64 handle, int status, const QString& message, bool waiting);
    /// Writes to a run's stdout or stderr, within Scripts.commandOutputLimit.
    /// The result is deferred when @p sent will report it: once the text
    /// has gone, or was dropped.
    EffectResult writeRun(const ScriptPtr& script, quint64 handle, const QString& text, bool error,
                          std::function<void(bool delivered)> sent);
    /// Stops the sleeps of @p script whose request IDs start with @p prefix.
    void stopSleeps(Script& script, const QString& prefix);
    /// An ended run's handler that still runs: it resumed with "cancelled".
    bool endedRun(const Script& script, quint64 run) const;
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
    CommandLimits m_limits;
    quint64 m_nextRun = 0;
    QHash<quint64, std::weak_ptr<Script>> m_runScripts; ///< the script of each live run
    CallContext m_call;
    // Last, so a pending rescan cannot run into a partly destroyed host.
    std::unique_ptr<PhosphorFsLoader::WatchedDirectorySet> m_directories; ///< while enabled
};

} // namespace Afterglow
