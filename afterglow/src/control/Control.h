// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <optional>

namespace Afterglow {

class Pane;
class ProjectStore;
class SettingsStore;
class TerminalWindow;

/// One pane as the control interface and scripts see it.
struct PaneInfo
{
    quint32 id = 0;
    quint32 window = 0;
    int tab = 0;
    QString title;
    QString workingDirectory;
    QString command; ///< running now (OSC 133), empty at the prompt
    int cols = 0;
    int rows = 0;
    bool focused = false; ///< the focused pane of the active window
    /// Where the shell runs when not here as you ("root", "deploy@prod-1");
    /// not part of the D-Bus pane structure.
    QString context;
    quint32 tabId = 0; ///< stable within this process; tab remains the legacy index
};

/// What afterglow lets other programs (D-Bus) and scripts (Luau) do: one
/// layer, so both see the same model and the same rules. Operations return
/// an error message, empty on success.
class Control : public QObject
{
    Q_OBJECT

public:
    enum class TextScope { Screen, Scrollback, Selection, LastOutput };
    using WindowFactory = std::function<TerminalWindow*(const QString& workingDirectory, const QStringList& command, bool empty)>;
    /// Runs a script action ("<script id>.<action id>"); false if there is none.
    using ScriptActionRunner = std::function<bool(const QString& action, quint32 pane)>;

    Control(SettingsStore& settings, WindowFactory newWindow, QObject* parent = nullptr);

    /// Follows a window's panes; every window registers on creation.
    void attachWindow(TerminalWindow* window);
    void setScriptActionRunner(ScriptActionRunner runner) { m_scriptActions = std::move(runner); }

    QList<PaneInfo> panes() const;
    std::optional<PaneInfo> paneInfo(quint32 id) const;
    /// Windows and tabs, including each window's active tab and pane.
    QVariantMap state() const;
    static QVariantMap paneMap(const PaneInfo& pane);
    static bool isStructuralAction(const QString& action);

    QString openDirectory(const QString& cwd, const QString& layout, QVariantMap& result);
    void setScriptDiagnostics(std::function<QVariantList()> read) { m_scriptDiagnostics = std::move(read); }
    QVariantList scriptDiagnostics() const { return m_scriptDiagnostics ? m_scriptDiagnostics() : QVariantList{}; }
    QStringList projectNames() const;
    QString saveProject(quint32 pane, const QString& name, bool overwrite, QVariantMap& result);
    QString openProject(const QString& name, bool newInstance, QVariantMap& result);
    QString movePane(quint32 pane, quint32 target, const QString& placement, QVariantMap& result);
    QString detachTab(quint32 pane, QVariantMap& result);
    QString sendText(quint32 pane, const QString& text);
    /// Honours Control.allowReadText unless @p trusted (afterglow itself).
    QString readText(quint32 pane, TextScope scope, QString& out, bool trusted = false);
    /// Bounded script query, subject to Scripts.allowReadText.
    QString readTextBounded(quint32 pane, const QString& scope, int limit, QVariantMap& result);
    QString semanticRegions(quint32 pane, QVariantMap& result);
    QString readRegion(quint32 pane, const QString& region, int limit, QVariantMap& result);
    QString split(quint32 pane, bool right, const QStringList& command, quint32& created);
    QString split(quint32 pane, const QString& direction, const QStringList& command, const QVariantMap& options, quint32& created);
    QString newTab(quint32 pane, const QString& workingDirectory, const QStringList& command, quint32& created);
    QString newTab(quint32 pane, const QString& workingDirectory, const QStringList& command, const QVariantMap& options, quint32& created);
    QString newWindow(const QString& workingDirectory, const QStringList& command, quint32& created);
    QString newWindow(const QString& workingDirectory, const QStringList& command, const QVariantMap& options, quint32& created);
    QString focus(quint32 pane);
    QString close(quint32 pane);
    QString setTitle(quint32 pane, const QString& title);
    /// Sets shader signal @p channel (0-7, iSignal in shaders) on the pane.
    QString signal(quint32 pane, int channel, double value);
    static constexpr int MaxSignalPanes = 128;
    /// Validates the complete batch before updating the anchor's pane/tab/window.
    QString signalsBatch(quint32 pane, const QString& scope, const QVariantList& values, QVariantList& updated);
    QString notify(quint32 pane, const QString& title, const QString& body);
    QString copy(const QString& text);
    /// http, https and mailto only.
    QString openUrl(const QString& url);
    /// A keybind action by its config name (split_right, new_tab...), or a
    /// script action as "script:<script id>.<action id>".
    QString runAction(quint32 pane, const QString& action);

    using ScriptReply = std::function<void(QString, QVariantMap)>;
    QString scriptDialog(const QString& owner, quint32 pane, const QVariantMap& effect, ScriptReply reply);
    QString scriptAppearance(const QString& owner, quint32 pane, const QVariantMap& effect);
    void clearScriptOwner(const QString& owner);
    QString scriptShader(const QString& owner, quint32 pane, const QVariantMap& effect, QVariantMap& result);

    static QString scopeName(TextScope scope);
    static std::optional<TextScope> scopeFromName(const QString& name);

Q_SIGNALS:
    void paneCreated(quint32 pane);
    void paneMoved(quint32 pane);
    void paneClosed(quint32 pane);
    void paneClosedDetailed(quint32 pane, quint32 tab, quint32 window);
    void titleChanged(quint32 pane, const QString& title);
    void bell(quint32 pane);
    void commandStarted(quint32 pane, const QString& command);
    void commandFinished(quint32 pane, const QString& command, int exitCode, qint64 durationMs);
    /// Program status (OSC 7501): the pane's newest status record.
    void statusChanged(quint32 pane, const QString& state, const QString& text);
    void focusChanged(quint32 pane);
    /// A program set an iTerm2 user variable (OSC 1337 SetUserVar).
    void userVarChanged(quint32 pane, const QString& name, const QString& value);
    /// Where the pane's shell runs changed (OSC 3008, RemoteHost).
    void contextChanged(quint32 pane, const QString& context);

private:
    ProjectStore projects() const;
    void projectDialog(Pane* pane, bool save);
    void attachPane(TerminalWindow* window, Pane* pane);
    Pane* find(quint32 id, TerminalWindow** window = nullptr) const;

    SettingsStore& m_settings;
    WindowFactory m_newWindow;
    ScriptActionRunner m_scriptActions;
    std::function<QVariantList()> m_scriptDiagnostics;
    QList<QPointer<TerminalWindow>> m_windows;
    QHash<quint32, QPair<quint32, quint32>> m_closingPanes;
};

} // namespace Afterglow
