// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Capture.h"
#include "OverlayInputs.h"
#include "../SplitTree.h"
#include "../scripting/OverlaySpec.h"

#include <AfterglowProto/Remote.h>
#include <AfterglowVt/Terminal.h>

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QStringList>
#include <QVariantMap>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <optional>

namespace AfterglowVt {
enum class StatusState : std::uint8_t;
}

namespace Afterglow {

class Pane;
class ProjectStore;
class SettingsStore;
class TerminalWindow;
struct PaneLaunchOptions;
struct SessionLink;
struct ControlMailbox;

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
    /// Where the shell runs when not here as you ("root", "deploy@prod-1").
    QString context;
    quint32 tabId = 0; ///< stable within this process, unlike the tab's position (tab)
    /// The facts below are known: listPanes gathers them, with a read of
    /// /proc for each pane, and the other users of a pane's map do not.
    bool facts = false;
    /// The cursor is in a shell prompt or its input (OSC 133).
    bool atPrompt = false;
    /// The values of OSC 1337 SetUserVar.
    QMap<QString, QString> userVars;
    /// The leader of the foreground process group, for a pane whose program
    /// runs in this process: none for a session pane, whose daemon knows it.
    struct Foreground
    {
        qint64 pid = 0;
        QStringList cmdline;
        QString cwd;
    };
    std::optional<Foreground> foreground;
    /// The pane beside it in its tab, 0 for none: left, right, up, down.
    std::array<quint32, 4> neighbors{};
};

/// What afterglow lets other programs (the control socket) and scripts (Luau) do: one
/// layer, so both see the same model and the same rules. Operations return
/// an error message, empty on success.
class Control : public QObject
{
    Q_OBJECT

public:
    /// FirstOutputOnScreen and LastVisitedOutput as kitty's get-text extents
    /// first_cmd_output_on_screen and last_visited_cmd_output.
    enum class TextScope {
        Screen,
        Scrollback,
        Selection,
        LastOutput,
        FirstOutputOnScreen,
        LastVisitedOutput
    };
    using WindowFactory =
        std::function<TerminalWindow*(const QString& workingDirectory, const QStringList& command, bool empty)>;
    /// Starts a script command ("<script id>.<command>") with no arguments and
    /// no client, from @p source (ev.source). Returns an error, or empty.
    using ScriptCommandRunner = std::function<QString(const QString& command, quint32 pane, const QString& source)>;

    /// The end of a SessionWindowOpener: empty, else why no window shows the
    /// session.
    using SessionOpened = std::function<void(const QString& error)>;
    /// Opens a window that shows mux session @p session of @p host (empty:
    /// this computer), with the calls that wait for the daemon off the GUI
    /// thread: @p done gets the outcome once, before this returns or later,
    /// unless Afterglow quits first.
    using SessionWindowOpener = std::function<void(const QString& session, const QString& host, SessionOpened done)>;
    /// Opens a window that shows mux session @p session of this computer,
    /// on @p link, the connection that answered its attach call
    /// (attachSession): empty, else why not.
    using SessionLinkShower = std::function<QString(const QString& session, SessionLink&& link)>;
    /// The end of openProject(): its error, or empty and its result.
    using ProjectOpened = std::function<void(const QString& error, const QVariantMap& result)>;
    /// The end of movePane() and detachTab(): the error, or the pane where
    /// it went, as paneMap() describes it.
    using PaneMoved = std::function<void(const QString& error, const QVariantMap& result)>;
    /// The end of a split() or newTab() that can wait: the error, or the
    /// new pane.
    using PaneMade = std::function<void(const QString& error, quint32 created)>;

    Control(SettingsStore& settings, WindowFactory newWindow, QObject* parent = nullptr);
    /// Has the calls to the mux daemon that still run end the sessions that
    /// they start, and drops their answers: it does not wait for them,
    /// which a quit does only a bounded time (MuxClient::waitForCallThreads).
    /// A call that the quit cuts short can leave a project's session that no
    /// window shows (openProjectSession): that session runs on, as after a
    /// crash.
    ~Control() override;
    /// For a pane or a tab that moves to a new window from a session's
    /// window (a drag, a key, the menu, a script or afterglow ctl): it goes
    /// to a new session, which a new window shows. Also for a project whose
    /// session runs while the window that showed it closed (openProject).
    void setSessionWindowOpener(SessionWindowOpener opener)
    {
        m_openSessionWindow = std::move(opener);
    }
    /// For a project that opens into a session, whose attach waited for the
    /// daemon off the GUI thread.
    void setSessionLinkShower(SessionLinkShower shower)
    {
        m_showSessionLink = std::move(shower);
    }

    /// Follows a window's panes; every window registers on creation.
    void attachWindow(TerminalWindow* window);
    void setScriptCommandRunner(ScriptCommandRunner runner)
    {
        m_scriptCommands = std::move(runner);
    }

    /// With @p facts, each pane's PaneInfo::facts too, for listPanes: the
    /// command line of the foreground process only with @p cmdlines, which
    /// read access gives (Pane::foreground).
    QList<PaneInfo> panes(bool facts = false, bool cmdlines = true) const;
    std::optional<PaneInfo> paneInfo(quint32 id) const;
    bool hasPane(quint32 id) const
    {
        return find(id);
    }
    /// Sets what @p pane answers to `aglow run`'s query for its ssh
    /// session's socket (RemoteSockets). False when there is no such pane.
    bool setRemoteSocket(quint32 pane, const AfterglowProto::Remote::Answer& answer,
                         const std::optional<AfterglowProto::Remote::Answer>& overlay);
    /// The focused pane of the active window, else of the window that was
    /// active last: `afterglow ctl` run from another program's window.
    std::optional<quint32> focusedPane() const;
    /// Windows and tabs, including each window's active tab and pane.
    QVariantMap state() const;
    static QVariantMap paneMap(const PaneInfo& pane);
    static bool isStructuralAction(const QString& action);
    /// The name of a program status state (OSC 7501): idle, working, done, blocked or error.
    static QString statusStateName(AfterglowVt::StatusState state);

    QString openDirectory(const QString& cwd, const QString& layout, QVariantMap& result);
    void setScriptDiagnostics(std::function<QVariantList()> read)
    {
        m_scriptDiagnostics = std::move(read);
    }
    QVariantList scriptDiagnostics() const
    {
        return m_scriptDiagnostics ? m_scriptDiagnostics() : QVariantList{};
    }
    QStringList projectNames() const;
    QString saveProject(quint32 pane, const QString& name, bool overwrite, QVariantMap& result);
    /// Who owns the overlays and the inputs of @p client of the session that
    /// window @p window shows: the client numbers are its daemon's. A window
    /// shows one session for its life, and no two windows share an ID.
    /// Without @p client, the start of the owners of the window.
    static QString sessionClientOwner(quint32 window, std::optional<qint64> client = std::nullopt)
    {
        const QString prefix = QStringLiteral("session:%1:").arg(window);
        return client ? prefix + QString::number(*client) : prefix;
    }
    /// The overlays and their inputs of the clients of sessions that the mux
    /// daemon passes to the windows of this process (`afterglow ctl
    /// --session NAME overlay`), as the control socket keeps them for its
    /// own clients: @p owner is the client's (SessionControl).
    QString addSessionOverlayInput(const QString& owner, const QString& name, const QString& chunk, bool eof)
    {
        return m_sessionInputs.add(owner, name, chunk, eof);
    }
    QString takeSessionOverlayInput(const QString& owner, QVariantMap& options)
    {
        return m_sessionInputs.resolve(owner, options);
    }
    void sessionOverlayOpened(const QString& owner)
    {
        m_sessionOverlayOwners.insert(owner);
    }
    /// The client @p owner of a session went: its overlays close with
    /// @p message, and its inputs go.
    void endSessionClient(const QString& owner, const QString& message);
    /// The same for the clients of every session. Control does it itself when
    /// Control.enabled goes off.
    void endSessionClients(const QString& message);
    /// The answer to a screenshot for a client: {png, width, height}, or an
    /// error and its code.
    using ScreenshotReply = std::function<void(const QString& error, const QString& code, const QJsonObject& result)>;
    /// A screenshot of @p pane for a client of the control socket or of a
    /// session: capture(), then its PNG, encoded off the GUI thread one at a
    /// time. Read access and Control.enabled are checked again as the answer
    /// goes out. A refusal returns its error at once with @p code: denied,
    /// busy past MaxScreenshots, or no_pane. Else @p reply gets the answer
    /// once, also with the code not_shown, failed or cancelled.
    QString screenshot(quint32 pane, CaptureScope scope, ScreenshotReply reply, QString& code);
    /// Screenshots from the call to the answer, of every client: each holds
    /// a frame and its PNG. A client that goes does not stop its frame or
    /// encode, so the count drops only as they end.
    static constexpr int MaxScreenshots = 4;
    /// Calls of the mux daemon that wait on threads of their own (the opens
    /// of projects into sessions): past this, a call fails at once.
    static constexpr int MaxOffThreadCalls = 16;
    /// Whether process @p process runs in pane @p pane, or in its overlay with
    /// @p overlay: the program leads the terminal session of the process, or
    /// of a process above it, at most 64 up (a shell in an editor's
    /// terminal has a session of its own). By the processes, not by an
    /// AFTERGLOW_PANE_ID that a process of another terminal can carry.
    bool runsIn(quint32 pane, qint64 process, bool overlay) const;
    /// Opens project @p name: into a mux session with @p mode "session", into
    /// a window with "window", and as the project says with an empty mode.
    /// @p done gets the outcome once, before this returns, or later for a
    /// session, unless this object goes first: the calls to the mux daemon,
    /// which can start it, run off the GUI thread.
    void openProject(const QString& name, bool newInstance, const QString& mode, ProjectOpened done);
    /// Moves @p pane beside @p target, to a new tab, or to a new window. A
    /// new tab with a target is in the window of the target, else in the
    /// pane's own. @p done gets the outcome once, before this returns, or
    /// later for a pane of a mux session's window that goes to a new window
    /// or to the window of another session, unless this object goes first:
    /// the pane goes to a new session or joins that one, and the calls to
    /// its daemon, and the attach of the window that shows it, run off the
    /// GUI thread.
    void movePane(quint32 pane, quint32 target, const QString& placement, PaneMoved done);
    /// Moves @p pane's tab to a new window, as movePane() does.
    void detachTab(quint32 pane, PaneMoved done);
    QString sendText(quint32 pane, const QString& text);
    /// Honours Control.allowReadText.
    /// @p format: plain, or with styles, wrap markers or the cursor (not for
    /// a selection, which is plain text).
    QString readText(quint32 pane, TextScope scope, QString& out, const AfterglowVt::TextFormat& format = {});
    /// Bounded script query, subject to Scripts.allowReadText. With
    /// @p clientRun, for a command run whose output goes to a client
    /// (`aglow run`, a socket call), also to Control.allowReadText: the
    /// text would leave afterglow as control text does.
    QString readTextBounded(quint32 pane, const QString& scope, int limit, QVariantMap& result, bool clientRun);
    QString semanticRegions(quint32 pane, QVariantMap& result, bool clientRun);
    QString readRegion(quint32 pane, const QString& region, int limit, QVariantMap& result, bool clientRun);
    QString split(quint32 pane, const QString& direction, const QStringList& command, const QVariantMap& options,
                  quint32& created);
    /// split() that does not wait for the mux daemon (a script's): in the
    /// window of a session, the daemon starts the program off the GUI
    /// thread (TerminalWindow::startSessionPane). @p done gets the outcome
    /// once, before this returns, or later, also when the window closes
    /// first.
    void split(quint32 pane, const QString& direction, const QStringList& command, const QVariantMap& options,
               PaneMade done);
    QString newTab(quint32 pane, const QString& workingDirectory, const QStringList& command, quint32& created);
    QString newTab(quint32 pane, const QString& workingDirectory, const QStringList& command,
                   const QVariantMap& options, quint32& created);
    /// newTab() that does not wait for the mux daemon, as split() with
    /// @p done.
    void newTab(quint32 pane, const QString& workingDirectory, const QStringList& command, const QVariantMap& options,
                PaneMade done);
    QString newWindow(const QString& workingDirectory, const QStringList& command, quint32& created);
    QString newWindow(const QString& workingDirectory, const QStringList& command, const QVariantMap& options,
                      quint32& created);

    /// The side that @p name (left, right, up or down) names, for a split
    /// or a move.
    static std::optional<SplitTree::Direction> splitDirection(const QString& name);
    /// The longest name of a user variable, in bytes, as OSC 1337 takes it.
    static constexpr qsizetype MaxUserVarName = qsizetype(AfterglowVt::ITerminalHost::MaxUserVarName);
    /// Why @p name cannot name a user variable, as OSC 1337 SetUserVar
    /// takes one: 1 to MaxUserVarName bytes, and no =. Empty when it can.
    static QString userVarName(const QString& name);
    /// The refusal of a new user variable of pane @p id, which has
    /// ITerminalHost::MaxUserVars.
    static QString userVarsFull(quint32 id);
    /// split, newTab and newWindow for a control client, with the options
    /// of a script's launch (cwd, env, and size for a split) and those that
    /// kitten @ launch has besides: focus, hold, vars, stdin_text and
    /// stdin_ansi. The pane's text goes to the program only with
    /// Control.allowReadText, as getText, and only to a command: a shell
    /// would run it.
    QString clientSplit(quint32 pane, const QString& direction, const QStringList& command, const QVariantMap& options,
                        quint32& created);
    QString clientNewTab(quint32 pane, const QString& workingDirectory, const QStringList& command,
                         const QVariantMap& options, quint32& created);
    QString clientNewWindow(const QString& workingDirectory, const QStringList& command, const QVariantMap& options,
                            quint32& created);
    QString focus(quint32 pane);
    QString close(quint32 pane);
    QString setTitle(quint32 pane, const QString& title);
    /// Focuses the pane beside @p pane on @p direction, which @p focused
    /// gets. An error at the edge.
    QString focusNeighbor(quint32 pane, const QString& direction, quint32& focused);
    /// Moves a split edge of @p pane by @p cells toward @p direction, as
    /// the resize actions and tmux's resize-pane do: the edge on that side,
    /// else the one on the other side.
    /// @p cells, as the clients send numbers: a whole number from 1 to
    /// MaxResizeCells.
    QString resizePane(quint32 pane, const QString& direction, double cells);
    static constexpr int MaxResizeCells = 10000;
    /// A JSON number that is a whole number: the clients send numbers as
    /// doubles, and a conversion would wrap or round one that is not.
    static bool isWhole(double value);
    /// Sends the keys that @p keys name as typed keys: none when a name
    /// names no key.
    QString sendKeys(quint32 pane, const QStringList& keys);
    /// @p unit: lines, pages, top, bottom or prompt. A positive @p amount
    /// goes down, to the newest text: a whole number of at most
    /// MaxScrollAmount either way.
    QString scroll(quint32 pane, const QString& unit, double amount);
    static constexpr qint64 MaxScrollAmount = 1'000'000'000;
    QString setTabTitle(quint32 pane, const QString& title);
    /// The colours of @p pane by the names of Terminal::namedColors.
    QString colors(quint32 pane, QVariantMap& out);
    /// Sets the colours of @p colors (a name to a colour as a theme writes
    /// it) on @p pane as its program would, after the configured ones come
    /// back with @p reset. Not for a session pane, whose daemon has its
    /// terminal: that refusal sets @p code to SessionPaneCode.
    QString setColors(quint32 pane, const QVariantMap& colors, bool reset, QString* code = nullptr);
    /// setColors() refuses a session pane in these words.
    static const QString SessionPaneColors;
    /// The error code of that refusal, by which set-colors --all of
    /// afterglow ctl passes over session panes.
    static constexpr const char* SessionPaneCode = "session_pane";
    /// @p size: points, or "+N" and "-N" points more or fewer.
    QString setFontSize(quint32 pane, const QString& size);
    QString setOpacity(quint32 pane, double opacity);
    QString setUserVar(quint32 pane, const QString& name, const QString& value);
    /// @p signal: a name such as INT or SIGTERM, or a number.
    QString signalProgram(quint32 pane, const QString& signal);
    /// @p state: fullscreen, maximized, normal or empty; then @p cols x
    /// @p rows cells for one pane, whole numbers to MaxWindowCells, or 0
    /// and 0 for no size.
    QString setWindowState(quint32 pane, const QString& state, double cols, double rows);
    static constexpr int MaxWindowCells = 10000;
    /// Sets shader signal @p channel (0-7, uSignal in shaders) on the pane.
    QString signal(quint32 pane, double channel, double value);
    static constexpr int MaxSignalPanes = 128;
    /// Validates the complete batch before updating the anchor's pane/tab/window.
    QString signalsBatch(quint32 pane, const QString& scope, const QVariantList& values, QVariantList& updated);
    QString notify(quint32 pane, const QString& title, const QString& body);
    /// A script's notice (the end of a command run without a client) on
    /// @p pane, or for the window when it is 0 or has closed. It counts
    /// against Notifications.rateLimit: the pane's, or without a pane one
    /// limit for all the notices without a pane.
    void notice(quint32 pane, const QString& title, const QString& body);
    /// From now on notice() shows nothing: the application quits, and what
    /// quitting ends is no news.
    void endNotices()
    {
        m_noticesEnded = true;
    }
    /// Refuses text longer than MaxCopyLength UTF-16 units.
    QString copy(const QString& text);
    static constexpr qsizetype MaxCopyLength = 1 << 22;
    /// http, https and mailto only: a script cannot be asked first. Fails
    /// when the desktop refuses the request; success does not mean that a
    /// program opened the URL, which Qt cannot tell.
    QString openUrl(const QString& url);
    /// A keybind action by its config name (split_right, new_tab...), or a
    /// script command as "script:<script id>.<command>", which starts with no
    /// arguments and returns before the run ends. A keybind action
    /// shows the pane's tab first, except the tab actions that act on that
    /// tab where it is (TerminalWindow::runAction). A script command, as
    /// for `aglow run`, takes @p pane 0 for the focused pane, and runs
    /// with no pane when none has the focus.
    QString runAction(quint32 pane, const QString& action);
    /// runAction for a script: the actions that read pane text need
    /// Scripts.allowReadText, and script commands are refused.
    QString runScriptAction(quint32 pane, const QString& action);
    /// runAction for a client of the control socket or of a session: the
    /// actions that read pane text need Control.allowReadText.
    QString runClientAction(quint32 pane, const QString& action);
    /// @p pane, a paneMap, as it leaves for a client: commands can carry
    /// secrets, and leave only with Control.allowReadText.
    QVariantMap clientPaneMap(QVariantMap pane) const;
    /// Keybinds::readsPaneText for the action named @p action.
    static bool readsPaneText(const QString& action);

    using ScriptReply = std::function<void(QString, QVariantMap)>;
    QString scriptDialog(const QString& owner, quint32 pane, const QVariantMap& effect, ScriptReply reply);
    QString scriptAppearance(const QString& owner, quint32 pane, const QVariantMap& effect);
    /// A dock effect, in the window of @p pane. An undock acts there too when
    /// that window has the area; else, as when @p pane has moved or closed, in
    /// the window of the newest such area whose last dock named @p pane. With
    /// scope "pane", the effect acts on @p pane's own areas, which keep no
    /// record: a closed pane is an error.
    QString scriptDock(const QString& owner, quint32 pane, const QVariantMap& effect);
    /// Opens the tab menu of @p pane's tab (afterglow.tab_menu). Scripts
    /// only: no control socket call or ctl command opens it.
    QString tabMenu(quint32 pane);
    /// Moves @p pane's tab to @p index in its window, without showing it
    /// (afterglow.move_tab). An index outside 0 to count - 1 fails. Scripts
    /// only: the control socket and ctl move a tab one place with the move_tab_left and
    /// move_tab_right actions.
    QString moveTab(quint32 pane, int index);
    void clearScriptOwner(const QString& owner);
    /// Cancels the owner's dialogs, and the overlays it waits for, whose
    /// request IDs start with @p prefix.
    void closeRunWaits(const QString& owner, const QString& prefix);
    /// Runs @p spec over @p pane for @p owner (a script, or a control
    /// client), with the script's @p request ID. @p code tells a refusal
    /// apart: no_pane, busy or spawn_failed.
    QString overlay(const QString& owner, const QString& request, quint32 pane, const OverlaySpec& spec,
                    OverlayReply reply, QString& code);
    /// The overlay call of a client of the control socket or of a session,
    /// in two steps that both take, so that their rules cannot drift apart.
    /// The caller takes the client's waiting input into @p options first,
    /// and names the pane between the steps. This one reads @p argv with
    /// @p options: the spec, whose directory is one of this computer, or
    /// nothing and @p error, which has no code.
    static std::optional<OverlaySpec> clientOverlaySpec(const QStringList& argv, const QVariantMap& options,
                                                        QString& error);
    /// Then overlay() for @p owner with no request ID. The result's stdout
    /// leaves only with Control.allowReadText, checked as it goes out: read
    /// access can go while the program runs.
    QString clientOverlay(const QString& owner, quint32 pane, const OverlaySpec& spec, OverlayReply reply,
                          QString& code);
    /// Reads back the next frame of @p pane's window, as it shows on screen,
    /// cut to @p scope. It needs Control.allowReadText (see captureRefusal()).
    /// A refusal returns its error at once, with @p code denied or no_pane.
    /// Else @p reply gets the image, or not_shown or failed.
    QString capture(quint32 pane, CaptureScope scope, CaptureReply reply, QString& code);
    /// Why a screenshot is refused now, with @p code denied, or empty. A
    /// picture shows more than getText reads (an overlay over the pane, the
    /// chrome, a paste preview), and it takes the same permission.
    QString captureRefusal(QString& code) const;
    /// Cancels every overlay of @p owner (a control client that has gone).
    void closeOverlays(const QString& owner, const QString& message);
    QString scriptKeyMode(const QString& owner, quint32 pane, const QString& label);
    void endScriptKeyMode(const QString& owner);
    /// The overview for @p owner's script: see TerminalWindow::scriptOverview,
    /// which sets @p code "busy" for a refusal while another script holds
    /// the keyboard.
    QString overview(const QString& owner, quint32 pane, const QString& operation, QVariantMap& result,
                     QString* code = nullptr);
    QString overviewMove(quint32 pane, quint32 target, int step, QVariantMap& result);
    QString pulse(quint32 pane, int effect, int durationMs, int color);
    /// Where a pane split from: {"split_from": pane ID, "edge": 1-4}, empty when it did not.
    QVariantMap paneOrigin(quint32 pane) const;
    QString scriptShader(const QString& owner, quint32 pane, const QVariantMap& effect, QVariantMap& result);

    /// Expands a leading ~ of @p cwd. An error when it is not empty and
    /// names no existing absolute directory then: the rule of new-tab, split
    /// and the overlay call of the control socket.
    static QString localDirectory(QString& cwd);
    static QString scopeName(TextScope scope);
    /// The names of the text scopes, for the words of an error.
    static const QString TextScopeNames;
    static const QString BadTextScope;
    /// The format that the options of getText ask for: ansi, cursor and
    /// wrap_markers, each a boolean. An error for another key or type.
    static QString textFormat(const QVariantMap& options, AfterglowVt::TextFormat& format);
    static std::optional<TextScope> scopeFromName(const QString& name);
    /// The capture scope that @p name (pane, tab or window) names.
    static std::optional<CaptureScope> captureScopeFromName(const QString& name);

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
    /// A key for the script holding a window's key mode.
    void scriptKey(const QString& owner, quint32 pane, const QVariantMap& key);
    /// An event of a script's dock for the owner's @p handler; @p pane is a
    /// pane dock's pane, or the window's focused pane for a window dock.
    /// @p limited: the call counts toward the script's callback limit.
    void scriptDockEvent(const QString& owner, const QString& handler, quint32 pane, const QVariantMap& event,
                         bool limited);
    /// A window's current tab changed; @p pane is the new tab's focused pane.
    void tabSwitched(quint32 pane, quint32 fromTab, quint32 toTab, int direction);

private:
    static QString noPane(quint32 id);
    /// The refusal of a side that is not left, right, up or down.
    static QString badDirection();
    /// What a control client's launch asks for besides a script's.
    struct LaunchExtras;
    static QString readLaunchExtras(const QVariantMap& options, const QStringList& command, LaunchExtras& extras);
    /// The options of a new pane in @p window that @p input and @p extras
    /// ask for, with the text of pane @p source as its input when they ask.
    QString prepareLaunch(const TerminalWindow& window, quint32 source, const QString& cwd, const QStringList& command,
                          const QVariantMap& input, const LaunchExtras* extras, PaneLaunchOptions& options);
    /// What @p extras ask of the pane @p added once it runs: its user
    /// variables, and the focus back on @p before (a split: a tab without
    /// focus opens behind the current one, and passes none).
    static void afterLaunch(Pane* added, Pane* before, TerminalWindow* window, const LaunchExtras& extras);
    /// split, newTab and newWindow, with the @p extras of a control client
    /// or none.
    QString splitWith(quint32 pane, const QString& direction, const QStringList& command, const QVariantMap& input,
                      const LaunchExtras* extras, quint32& created);
    QString newTabWith(quint32 pane, const QString& workingDirectory, const QStringList& command,
                       const QVariantMap& input, const LaunchExtras* extras, quint32& created);
    QString newWindowWith(const QString& workingDirectory, const QStringList& command, const QVariantMap& input,
                          const LaunchExtras* extras, quint32& created);
    /// The window that shows mux session @p session of this computer, or nullptr.
    TerminalWindow* sessionWindow(const QString& session) const;
    /// openProject() into a mux session named @p name: its window, or a new
    /// window of the session that runs, or a new session.
    void openProjectSession(const QString& name, const QJsonObject& document, bool newInstance, ProjectOpened done);
    /// Runs @p work on a thread of its own, then @p then with its result on
    /// this object's thread, unless this object went first: @p then stays
    /// here, and @p work must capture nothing that this thread must free.
    /// An error when no thread starts, or MaxOffThreadCalls calls wait
    /// already, and then neither runs.
    template<typename Result>
    QString offThread(std::function<Result()> work, std::function<void(Result)> then);
    using OffThreadResult = std::shared_ptr<void>;
    QString startOffThread(std::function<OffThreadResult()> work, std::function<void(OffThreadResult)> then);
    void offThreadDone(quint64 id, OffThreadResult result);
    ProjectStore projects() const;
    void projectDialog(Pane* pane, bool save);
    void attachPane(Pane* pane);
    Pane* find(quint32 id, TerminalWindow** window = nullptr) const;
    static PaneInfo describe(TerminalWindow* window, Pane* pane, bool facts = false, bool cmdline = true);
    /// Shows a window made by m_newWindow as Window.startMode asks.
    void showNew(TerminalWindow* window) const;
    /// Encodes @p image as a PNG on m_encoder, then gives its base64 to
    /// @p done on this thread: empty when the PNG could not be written.
    void encodePng(QImage image, std::function<void(const QString& base64)> done);
    /// Moves @p pane of @p source, or its tab with @p tab, out of the session
    /// that @p source shows, to a new session in a new window, which takes
    /// what @p source kept for the panes. @p done gets the pane in the new
    /// window, which has another ID, once: a refusal at once, else later.
    /// An error when no window shows it.
    void breakOut(TerminalWindow* source, Pane* pane, bool tab, PaneMoved done);
    /// Moves @p pane of @p source, the window of a mux session, into
    /// @p destination, the window of another session: on @p placement of
    /// @p beside, a side or new-tab. The daemon moves its program
    /// (TerminalWindow::joinSession), and @p destination takes what @p source
    /// kept for the pane. @p done gets the pane in @p destination, which has
    /// another ID, once: a refusal at once, else later.
    void joinSession(TerminalWindow* source, Pane* pane, TerminalWindow* destination, Pane* beside,
                     const QString& placement, PaneMoved done);

    SettingsStore& m_settings;
    WindowFactory m_newWindow;
    SessionWindowOpener m_openSessionWindow;
    SessionLinkShower m_showSessionLink;
    ScriptCommandRunner m_scriptCommands;
    std::function<QVariantList()> m_scriptDiagnostics;
    bool m_noticesEnded = false;
    QList<QPointer<TerminalWindow>> m_windows;
    QPointer<TerminalWindow> m_lastActive;
    QHash<quint32, QPair<quint32, quint32>> m_closingPanes;
    QHash<quint32, QPair<QString, QString>> m_status; ///< the state and text last sent for each pane
    QHash<quint32, QString> m_context; ///< the context last sent for each pane
    /// A script's area in a window, and the pane it last docked it through:
    /// window docks belong to windows, and a pane can move or close. Pane
    /// docks go with their pane and have no record.
    struct DockRecord
    {
        QString owner;
        QString id;
        QPointer<TerminalWindow> window;
        quint32 pane = 0;
    };
    QList<DockRecord> m_docks;
    /// The projects whose session opens (openProjectSession), each with the
    /// opens of it that wait for that one to end.
    QHash<QString, QList<std::function<void()>>> m_projectOpens;
    /// The session names that opens ask the daemon for, until it answers.
    QSet<QString> m_namingSessions;
    /// The inputs of the overlays of clients of sessions, by their owners.
    OverlayInputs m_sessionInputs;
    /// The clients of sessions that opened an overlay.
    QSet<QString> m_sessionOverlayOwners;
    int m_screenshots = 0;
    /// Set as this object goes: a session that a call to the daemon starts
    /// then has no window to lay it out.
    std::shared_ptr<std::atomic<bool>> m_closing = std::make_shared<std::atomic<bool>>(false);
    /// Where the calls to the mux daemon of this computer, which run on
    /// threads of their own (offThread), give their results: this object
    /// while it lives.
    std::shared_ptr<ControlMailbox> m_mailbox;
    /// What runs with each result of offThread, by its call.
    QHash<quint64, std::function<void(OffThreadResult)>> m_offThread;
    quint64 m_lastOffThread = 0;
    /// The PNGs of screenshots, one at a time: a screenshot waits for its
    /// frame anyway, and MaxScreenshots bounds the queue. Away from the
    /// calls to the daemon, which can wait for seconds. Last: it waits for
    /// its work as it goes, while the rest of this object is still there.
    QThreadPool m_encoder;
};

template<typename Result>
QString Control::offThread(std::function<Result()> work, std::function<void(Result)> then)
{
    return startOffThread(
        [work = std::move(work)]() -> OffThreadResult {
            return std::make_shared<Result>(work());
        },
        [then = std::move(then)](OffThreadResult result) {
            then(std::move(*std::static_pointer_cast<Result>(result)));
        });
}

} // namespace Afterglow
