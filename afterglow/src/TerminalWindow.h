// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PaneLaunchOptions.h"

#include "Pane.h"
#include "PaneOverlay.h"
#include "SplitTree.h"
#include "ShownText.h"
#include "animation/FrameClock.h"
#include "animation/PaneMotion.h"
#include "app/Services.h"
#include "config/Settings.h"
#include "control/Capture.h"
#include "shaders/ShaderControls.h"

#include <AfterglowRender/GridRenderer.h>

#include <QClipboard>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWindow>

#include <functional>
#include <memory>
#include <QHash>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <vector>

namespace AfterglowRender {
class GpuDevice;
}

class QDialog;

namespace Afterglow {

class MuxClient;

struct WindowOptions
{
    QString workingDirectory;
    /// Program and arguments for the first pane; empty runs the shell.
    QStringList command;
    /// The quick terminal: a layer surface along the top of the screen.
    bool quick = false;
    /// @p workingDirectory was asked for (afterglow -d); otherwise it is only
    /// where the request came from, and the workingDirectory setting decides.
    bool directoryGiven = false;
    /// A fixed window title, instead of the programs' (afterglow --title).
    QString title;
    /// Keep the first pane open after its program exits (afterglow --hold).
    bool hold = false;
    bool empty = false; ///< Internal: receive live panes or a restored project before showing.
    QVariantMap environment = {};
    /// A mux session that the window shows in its first tab, in place of a
    /// program of its own (afterglow mux attach).
    std::shared_ptr<MuxClient> session = {};
};

class ChromeController;
class ChromeLayer;
class DockArea;
class Pane;
class ScriptHost;
class ShaderLibrary;
struct ScriptCommand;

/// How a popup overlay looks with @p settings: the popup border colour for
/// its border line (when @p border) with Window.popupCorners, the text
/// colour for @p title (UTF-8) in Terminal.bidi's order, and Colors.popupDim
/// over the pane around it.
AfterglowRender::WindowRenderer::Pane::Overlay::Popup popupLook(const Settings& settings, bool border, const std::string& title);

/// A top-level terminal window: a split tree of panes drawn in one render
/// pass on the shared GpuDevice.
class TerminalWindow : public QWindow
{
    Q_OBJECT

public:
    TerminalWindow(Services& services, const WindowOptions& options);
    ~TerminalWindow() override;

    static QList<TerminalWindow*> windows();
    QAccessibleInterface* accessibleRoot() const override;
    QAccessibleInterface* accessibleChrome() const;
    QAccessibleInterface* accessibleChromeFocus() const;
    /// A prompt, a menu, the palette or the search field takes the keys.
    bool chromeHasKeyboard() const;
    QRect accessiblePaneRect(Pane*) const;
    QRect accessibleCellRect(Pane*, AfterglowVt::SelectionPoint) const;
    /// The cell of @p pane under @p global (global logical pixels), if any.
    std::optional<AfterglowVt::SelectionPoint> accessibleCellAt(Pane*, const QPoint& global) const;
    /// The scripts' docks that show, and where (global logical pixels).
    QList<const DockArea*> accessibleDocks() const;
    QRect accessibleDockRect(const DockArea*) const;
    /// Where item @p index of @p dock shows (global logical pixels).
    QRect accessibleDockItemRect(const DockArea*, std::size_t index) const;
    /// Whether an assistive technology may act on the window now: as for
    /// keys, not while a prompt, the palette or a mode holds the keyboard.
    bool acceptsAccessibleActions() const;
    /// Item @p index of a dock that shows sends its script the click a left
    /// press on it sends, unless the chrome covers the item.
    void pressDockItem(const DockArea*, std::size_t index);
    /// Shows @p pane (its window, its tab) and gives it the focus, then
    /// runs @p action there, as its keys would. False when the window does
    /// not accept accessible actions now or the action did nothing.
    bool runAccessibleAction(Pane* pane, Action action);
    /// Shows @p pane and opens the context menu at its cursor.
    void openPaneMenu(Pane* pane);
    /// The keys that run @p action, as the command palette shows them.
    QStringList actionShortcuts(Action action) const;
    /// Switches to @p pane's tab, focuses it and raises the window.
    void revealPane(Pane* pane);

    // For the control interface and scripts.
    quint32 id() const noexcept { return m_id; }
    /// Developer diagnostics; collection is off unless explicitly enabled.
    void setGpuTimingEnabled(bool enabled) { m_renderer->setGpuTimingEnabled(enabled); }
    double completedGpuSeconds() const { return m_renderer->completedGpuSeconds(); }
    std::uint64_t submittedFrames() const { return m_renderer->submittedFrames(); }
    /// The size of the frames that the renderer draws, in device pixels:
    /// the size of a window screenshot. For the tests.
    QSize framePixels() const { return m_renderer->pixelSize(); }
    /// @p pane moves to its place (a split, a resize). For the tests.
    bool paneMoving(const Pane* pane) const
    {
        const auto motion = m_motion.find(pane);
        return motion != m_motion.end() && motion->second->rect.isAnimating();
    }

    /// Every pane, tab by tab.
    QList<Pane*> panes() const;
    int tabIndexOf(const Pane* pane) const { return tabOf(pane); }
    quint32 tabIdOf(const Pane* pane) const;
    struct TabInfo
    {
        quint32 id;
        int index;
        quint32 activePane;
        QString title;
    };
    QList<TabInfo> tabs() const;
    quint32 activeTabId() const;
    Pane* focusedPane() const { return m_focused; }
    /// Splits @p pane using @p options (empty command: the shell); returns the new pane.
    Pane* splitPane(Pane* pane, SplitTree::Direction direction, const PaneLaunchOptions& options, QString* error);
    /// The error of a split that has no room for another pane, as a notice
    /// shows it: afterglow ctl and scripts give their own text for it.
    static QString noRoomForPane() { return tr("There is not enough room for another pane."); }
    /// Opens a tab using @p options; returns its pane. With @p from, the tab
    /// opens beside that pane's tab and inherits its directory; otherwise
    /// beside the current tab, from the focused pane.
    Pane* openTab(const PaneLaunchOptions& options, QString* error, Pane* from = nullptr);
    void requestClosePane(Pane* pane) { closePane(pane); }
    /// Runs a keybind action as if it were pressed in @p pane. The tab
    /// actions of a pane in another tab (close, rename, move, detach, and a
    /// new tab beside it) act on that tab without showing it, as its tab menu
    /// does. False, and nothing
    /// done, while a prompt, a menu, the palette, the search or a script
    /// dialog has the keyboard.
    bool runAction(Pane* pane, Action action);
    using ScriptReply = std::function<void(QString, QVariantMap)>;
    QString showScriptDialog(const QString& owner, Pane*, const QVariantMap&, ScriptReply);
    QString setScriptAppearance(const QString& owner, Pane*, const QVariantMap&);
    /// A script's dock or undock effect, for this window. With scope "pane",
    /// for the docks at the edges of @p pane, which must be in this window.
    QString setScriptDock(const QString& owner, const QVariantMap& effect, Pane* pane = nullptr);
    /// The tab menu of @p pane's tab, for a script: at the dock click that
    /// came before it, else at the pointer, else at the window's top left
    /// corner. Refused while a prompt, a script dialog or the palette is
    /// open: the menu would answer for the prompt.
    QString openScriptTabMenu(Pane* pane);
    /// Moves @p pane's tab to @p index, without showing it.
    QString moveTabOf(Pane* pane, int index);
    /// The cells that fit in the window: a top or bottom dock is as wide.
    QSize gridSize() const;
    void clearScriptOwner(const QString& owner);
    /// Cancels the owner's dialog, and the owner's overlays that a run waits
    /// for, when their request IDs start with @p prefix.
    void closeRunWaits(const QString& owner, const QString& prefix);
    QString scriptShader(const QString& owner, Pane*, const QVariantMap&, QVariantMap&);
    /// A script's key mode: the window's keys go to the script's on_key until
    /// it ends the mode, fails or unloads. One script at a time.
    QString startScriptKeyMode(const QString& owner, Pane* pane, const QString& label);
    void endScriptKeyMode(const QString& owner);
    /// Scripts drive the pane overview: "open", "close" (back to where it
    /// was) or "choose" (the highlighted pane); and move its highlight to
    /// @p target, or by @p step cards when no target is given. "open" is
    /// refused while a script other than @p owner holds a key mode.
    QString scriptOverview(const QString& owner, Pane* pane, const QString& operation, QVariantMap& result);
    QString scriptOverviewMove(Pane* pane, Pane* target, int step, QVariantMap& result);
    /// The overview for scripts: open, highlighted and its cards, by pane ID.
    QVariantMap overviewState() const;
    /// Runs @p spec over @p pane (an overlay): it shows in the pane's place,
    /// or as a popup over a part of it with the pane showing around it, with
    /// the pane's tab and the focus, until the program exits. Refused
    /// while the pane has one, a script dialog is open in this window,
    /// another owner's key mode holds the keyboard, or a prompt, a menu or
    /// the command palette is open. An open search closes.
    /// @p code tells the refusal apart: busy, no_pane or spawn_failed.
    /// @p reply gets the result, or "cancelled" when the overlay closes first.
    QString openOverlay(const QString& owner, const QString& request, Pane* pane, const OverlaySpec& spec,
                        OverlayReply reply, QString& code);
    /// Cancels @p owner's overlays whose request IDs start with @p prefix.
    void closeOverlays(const QString& owner, const QString& prefix, const QString& message);
    /// How far the overlay of @p pane has faded in, 0 to 1, and whether the
    /// fade still runs. For the tests.
    std::pair<qreal, bool> overlayFade(const Pane* pane) const;
    /// Draws a frame and reads it back as the window shows it, cut to
    /// @p scope around @p pane: shader packs, an overlay over the pane and
    /// the chrome over it are in the image. A translucent background stays
    /// translucent, without what is behind the window. @p reply gets
    /// not_shown at once when the window is not on screen, and for the pane
    /// and tab scopes when the pane is in a tab in the background. In the
    /// frame, those two scopes are not_shown while the overview shows, and
    /// for a pane that closed, left the tab or whose tab went to the
    /// background; the pane scope also for a pane that a zoom hides or that
    /// has no area. They wait for the end of a tab switch, and the pane
    /// scope for the end of a move of the pane (a split, a resize), with no
    /// readback while they wait. A window that leaves the screen or closes
    /// first gives not_shown. A frame that cannot be read gives failed, and
    /// so does no frame for CaptureTimeoutMs (each frame of a wait restarts it).
    void captureFrame(Pane* pane, CaptureScope scope, CaptureReply reply);
    /// Plays effect @p effect over the pane in Shaders.paneEffects.
    QString pulsePane(Pane* pane, int effect, int durationMs, int color);
    /// Where a pane split from, and the edge it shares with that pane
    /// (1 left, 2 top, 3 right, 4 bottom); zeros otherwise.
    struct PaneOrigin
    {
        quint32 from = 0;
        int edge = 0;
    };
    PaneOrigin paneOrigin(const Pane* pane) const { return m_paneOrigin.value(pane); }

    QJsonObject projectLayout() const;
    QVariantMap restoreProject(const QJsonObject& document);
    QString projectDirectory() const { return m_projectDirectory; }
    void setProjectDirectory(const QString& path) { m_projectDirectory = path; }
    QString projectName() const { return m_projectName; }
    void setProjectName(const QString& name) { m_projectName = name; }
    QString movePaneTo(Pane* pane, TerminalWindow* destination, Pane* beside, SplitTree::Direction direction);
    QString detachTabTo(Pane* pane, TerminalWindow* destination);
    bool swapPane(Pane* pane, SplitTree::Neighbour direction);
    bool rotatePanes(bool clockwise);
    /// The pane overview: every tab's panes as live cards, drawn by Shaders.overview.
    bool openOverview();
    /// Whether the overview can open with @p pane highlighted, apart from
    /// it being open already.
    bool overviewOpensOn(const Pane* pane) const;
    /// Lists the shader settings loaded script packages suggest and writes
    /// the chosen one to the config file.
    bool usePackageShaders();
    bool overviewOpen() const { return m_overview.open; }
    /// The highlighted pane, while the overview is open.
    Pane* overviewHighlight() const { return m_overview.open ? m_overview.highlighted.data() : nullptr; }
    /// A tab switch is animating (Shaders.tabTransition).
    bool tabSwitching() const { return m_tabTransition.progress.isAnimating(); }
    /// The overview deck's position and where it heads, in cards.
    std::pair<qreal, qreal> overviewPosition() const { return {m_overview.position.value(), m_overview.positionTarget}; }
    /// Slides the quick terminal in or out (quick windows only).
    void toggleQuick();
    /// Whether the quick terminal shows or slides in (quick windows only).
    bool quickShown() const { return m_quickShown; }

Q_SIGNALS:
    /// The tab list, the current tab or a tab title changed.
    void tabsChanged();
    /// The new-window action, for the application to fulfil.
    void newWindowRequested(const QString& workingDirectory);
    void paneCreated(Pane* pane);
    void paneClosing(quint32 pane, quint32 tab, quint32 window);
    void paneMoved(Pane* pane);
    void transferRequested(Pane* pane, bool wholeTab);
    void projectRequested(Pane* pane, bool save);
    void focusedPaneChanged(Pane* pane);
    /// A key for the script that holds the key mode.
    void scriptKey(const QString& owner, quint32 pane, const QVariantMap& key);
    /// An event of a script's dock for its owner's @p handler: on_dock_click,
    /// on_dock_hover, on_dock_wheel, on_dock_drag or on_dock_layout. @p pane
    /// is a pane dock's pane, or the window's focused pane for a window
    /// dock. @p limited: the call counts toward the callback limit.
    void scriptDockEvent(const QString& owner, const QString& handler, quint32 pane, const QVariantMap& event, bool limited);
    /// The current tab changed; @p direction is 1 toward the right.
    void tabSwitched(quint32 fromTab, quint32 toTab, int direction);

protected:
    bool event(QEvent* e) override;
    void exposeEvent(QExposeEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    /// At a prompt that asked for it, moves the shell's cursor to a click.
    void moveCursorToClick(Pane* pane, const QPointF& pos);
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    struct Tab;
    /// Creates a pane and starts its shell sized for @p area (pane rect, pixels).
    Pane* createPane(const QString& workingDirectory, const QRect& area, const QStringList& command = {}, QString* failure = nullptr, const QVariantMap& environment = {}, bool fallback = false);
    /// A pane that shows pane @p id of @p session: no program of its own.
    Pane* createSessionPane(const std::shared_ptr<MuxClient>& session, std::uint32_t id);
    /// Puts @p first in a new tab at @p at (-1: beside the current one).
    Pane* addTab(Pane* first, int at);
    void bindPane(Pane* pane);
    /// Connects the overlay that runs over @p host (bindPane does it).
    void bindOverlay(Pane* host);
    /// The overlay over @p host ended: it goes off screen with a fade, and
    /// the host takes the input again. A host that closes (@p hostCloses)
    /// gets neither: closePane shows the overlay's last frame.
    void overlayEnded(Pane* host, PaneOverlay* overlay, bool hostCloses = false);
    /// Closes what the terminal of an ended or departing overlay
    /// (@p program) had here: its prompts, copy mode, link hover, click
    /// count, search, and, when its host has the focus, the search bar,
    /// Unicode input and the composition.
    void dropOverlayUi(Pane* program);
    /// The pane whose program gets @p pane's input: its overlay's while one
    /// runs over it.
    static Pane* inputPane(Pane* pane);
    /// The pane whose program the pointer at @p pos (logical) reaches over
    /// @p pane: inputPane(), or none where a popup leaves the pane showing.
    /// A click there focuses the pane, reaches no program and leaves the
    /// popup open.
    Pane* pointerTarget(Pane* pane, const QPointF& pos) const;
    /// The pane in the tab for @p pane: the host of an overlay's pane, else
    /// @p pane itself.
    static Pane* hostOf(Pane* pane) { return pane && pane->overlayHost() ? pane->overlayHost() : pane; }
    /// @p pane still takes the input it got before: it is in a tab, or the
    /// overlay that still runs over a pane in a tab.
    bool takesInput(const Pane* pane) const
    {
        if (pane && pane->overlayHost()) return inputPane(pane->overlayHost()) == pane && tabOf(pane->overlayHost()) >= 0;
        return pane && tabOf(pane) >= 0;
    }
    /// The title that shows for @p pane (window, tab, and the split label
    /// under a full-pane overlay): its overlay's while that has one, else
    /// its own. A popup's split label keeps the pane's own title.
    static QString shownTitle(const Pane* pane);
    /// Tells @p pane's own program that it got or lost the focus, when it
    /// asked to hear it (mode 1004). Not its overlay's: callers choose.
    static void reportFocus(Pane* pane, bool focused);
    /// The panes in the tabs and the panes of the overlays over them: every
    /// terminal of this window that a program runs in.
    QList<Pane*> terminalPanes() const;
    void releasePaneUi(Pane* pane);
    /// Closes the prompts and modes that belong to @p pane, which is going
    /// away: its clipboard and paste prompts, title prompt, menu, URL mode,
    /// selection drag, held press inside its selection, reported mouse
    /// buttons, a drag on or the pointer over its docks, and a close
    /// confirmation about it alone. Prompts about other panes stay.
    void dropPanePrompts(Pane* pane);
    /// Cancels the title prompt and closes the tab menu when they belong to
    /// tab @p tabId, which is going away: they would have nothing to act on.
    void dropTabPrompts(quint32 tabId);
    /// Gives @p pane, out of its tab already, to @p destination with what
    /// this window kept for it.
    void reparentPane(Pane* pane, TerminalWindow* destination);
    /// A dialog of @p pane is about to show over the quick terminal. A
    /// layer surface is no parent for it: it would open under the quick
    /// terminal and block its keys. The quick terminal slides out instead
    /// (settleQuickDialogs).
    void quickDialogOpened(Pane* pane, QDialog* dialog);
    /// Forgets the quick terminal's dialogs that closed, or whose pane closed
    /// or went to another window. A dialog that came since the last look slides the quick
    /// terminal out. Once none is left, it slides in again when it slid out
    /// for them, unless a dialog went with its pane to another window.
    void settleQuickDialogs();
    /// After a swap or rotation: shows every pane again when the tab was
    /// zoomed (@p wasZoomed), or lets the panes ease to their new places.
    void settleRearrangement(bool wasZoomed);
    /// Takes tab @p index out of this window and returns it, with its panes.
    std::unique_ptr<Tab> removeTabShell(int index);
    /// Shows the current tab after panes or tabs arrived or left. The tab
    /// that was current before (@p previousTab, by id, at @p previousIndex)
    /// counts as left when another one shows now.
    void finishTransfer(quint32 previousTab = 0, int previousIndex = -1);
    Pane* splitFocused(SplitTree::Direction direction, const QStringList& command = {}, const PaneLaunchOptions& options = {}, QString* error = nullptr);
    /// Opens a tab at @p at (beside the current tab when negative).
    Pane* newTab(const QString& workingDirectory, const QStringList& command = {}, const QVariantMap& environment = {}, QString* error = nullptr, int at = -1);
    /// A tab that shows the first pane of @p session. Null when the session
    /// names no pane: the window then has no tab, and Application deletes it.
    Pane* newSessionTab(const std::shared_ptr<MuxClient>& session);
    /// Opens a shell tab beside @p from's tab, in its directory (beside the
    /// current tab with none), without showing @p from's tab first: one tab
    /// switch. A failure is a notice.
    void openShellTab(Pane* from);
    void switchTab(int index);
    void closeTab(int index);
    /// Closes tabs from the tab bar, menus and keys: asks first where a program runs.
    void requestCloseTabs(const QList<int>& indices);
    /// Runs @p close, first asking when confirmClose is on and a program
    /// runs in one of @p panes. @p what names what closes: "this tab".
    void confirmClose(const QList<Pane*>& panes, const QString& what, std::function<void()> close);
    /// confirmClose with the @p jobs that closingJobs gave for @p panes.
    void confirmClose(const QStringList& jobs, const QList<Pane*>& panes, const QString& what, std::function<void()> close);
    /// The jobs that confirmClose names for @p panes, as it shows them (an
    /// overlay's program too); empty when confirmClose is off or none runs.
    QStringList closingJobs(const QList<Pane*>& panes) const;
    /// Where a new tab, split or window starts, given the directory it
    /// would inherit (the workingDirectory setting).
    QString startDirectory(const QString& inherited) const;
    QString focusedDirectory() const;
    /// Moves the tab at @p from to @p to; the current tab stays current.
    void moveTab(int from, int to);
    void detachTab(int index);
    void renameTab(int index);
    /// The menu of the tab at @p index, at @p pos (logical pixels).
    void openTabMenu(int index, const QPointF& pos);
    void runTabMenuItem(int id);
    int tabOf(const Pane* pane) const;
    /// Height of the tab bar in pixels with @p tabCount tabs (the current
    /// count when negative); 0 when hidden by its configured policy or
    /// replaced by a script's tab_bar dock.
    int tabBarHeight(int tabCount = -1) const;
    /// The window in pixels, before the renderer has a size too.
    QSize windowPixels() const;
    /// The window below the tab bar, in pixels, with @p tabCount tabs (the
    /// current count when negative): what the docks and panes share.
    QRect windowArea(int tabCount = -1) const;
    /// Where the current tab's panes are laid out, in pixels, with
    /// @p tabCount tabs (the current count when negative).
    QRect paneArea(int tabCount = -1) const;
    /// The smallest pane that splits, resizes and divider drags leave, in pixels.
    QSize minimumPaneSize() const;
    /// The width of a divider between panes, in pixels: one logical pixel.
    int dividerPixels() const;
    /// @p pane is drawn this frame: visible in the current tab, a card of
    /// the overview, or in the tab a tab switch is leaving.
    bool paneDrawn(const Pane* pane) const;
    /// @p pane (or the host of an overlay) has a place in the current tab's
    /// layout: not in another tab, not hidden by a zoom, laid out.
    bool paneLaidOut(const Pane* pane) const;
    void closePane(Pane* pane);
    void setFocusedPane(Pane* pane);
    void scheduleRender();
    void renderNow();
    void relayout();
    /// The panes' rectangles are from another window size, scale, font or
    /// padding than now, or from before a split or a tab switch.
    bool layoutStale() const;
    /// Lays the panes out now when a dock moved or the layout is stale, as
    /// the next frame would: for a caller that measures them before it.
    void ensureLayout();
    void setPaneZoom(Pane* pane);
    bool resizeFocused(SplitTree::Neighbour direction);
    bool handleShortcut(QKeyEvent* e);
    void updateKeyMode();
    bool copyModeKey(QKeyEvent* e);
    /// Hands the key to the script holding the key mode, if one does.
    /// @p shortcut is set when a binding of the user's took it instead
    /// (cancel_overlay), so that its repeats are swallowed as a shortcut's.
    bool scriptKeyModeKey(QKeyEvent* e, bool& shortcut);
    void stopCopyMode();
    QPointer<Pane> m_copyPane;
    struct ScriptDialog { QString owner; QPointer<Pane> pane; ScriptReply reply; QString request; };
    std::optional<ScriptDialog> m_scriptDialog;
    void finishScriptDialog(bool accepted, const QString& value);
    struct ScriptAppearance {
        QString owner;
        QPointer<Pane> pane;
        QVariantMap values;
        QString status, tab;
        quint64 order = 0;
    };
    std::vector<ScriptAppearance> m_scriptAppearance;
    quint64 m_scriptAppearanceOrder = 0;
    /// Scripts' docked areas at the window's edges.
    std::vector<std::unique_ptr<DockArea>> m_docks;
    /// The buttons pressed on a dock: their releases, and the moves while
    /// only they are held, are the dock's too.
    Qt::MouseButtons m_dockButtons;
    /// What is left of @p area once each dock that fits takes its edge;
    /// with @p rects, where each dock goes (empty when it does not fit);
    /// with @p tabBarPlaced, whether a tab_bar dock fits.
    QRect layoutDocks(QRect area, std::vector<QRect>* rects, bool* tabBarPlaced = nullptr) const;
    /// Places the docks for the window's size: returns the panes' area.
    QRect placeDocks();
    /// The panes' terminal options, for the docks too.
    void setDockOptions(const AfterglowVt::TerminalOptions& options);
    /// The dock at @p pixel: a window's dock, or a shown pane's dock.
    DockArea* dockAt(const QPoint& pixel) const;
    /// A plain left press at @p pixel grabs a divider whose grab reaches
    /// into a pane's dock there, not the dock.
    bool dividerTakesDock(const QPoint& pixel) const;
    /// The pane whose own cells are at window logical point @p logical: not
    /// one whose dock is there.
    Pane* paneUnder(const QPointF& logical) const;
    /// How far from a divider a press still grabs it, pixels.
    int dividerTolerance() const;
    /// The framebuffer pixel that holds window logical point @p logical.
    QPoint dockPixel(const QPointF& logical) const;
    /// The cell of @p dock at @p pixel, also past its edges.
    QPoint dockCell(const DockArea& dock, const QPoint& pixel) const;
    /// A press on a dock: sends its item's click. False when no dock is there.
    bool pressDock(QMouseEvent* e);
    /// @p limited: the click counts toward the script's callback limit.
    /// @p at, window logical pixels, is where a tab menu opens after it.
    void emitDockClick(const DockArea& dock, const QString& item, const QString& button, bool limited, const QPointF& at);
    void emitDockEvent(const DockArea& dock, const QString& handler, QVariantMap event, bool limited);
    /// The wheel over a dock: whole steps go to its script.
    void wheelDock(DockArea& dock, const QPoint& pixel, QWheelEvent* e);
    /// A dock by owner, id and pane (0 for the window's), or null when it
    /// is not in this window.
    DockArea* findDock(const QString& owner, const QString& id, quint32 pane) const;
    /// The first dock of the current tab's panes that shows, while no
    /// overview draws them as cards, for which @p match is true.
    template<typename Match>
    DockArea* firstShownPaneDock(Match&& match) const;
    /// The dock item under the pointer, by owner and id: a dock can go
    /// while the pointer is over it.
    struct DockHover
    {
        QString owner, dock, item;
        quint32 pane = 0; ///< the pane whose dock it is, or 0
        bool operator==(const DockHover&) const = default;
    };
    DockHover m_dockHover;
    /// Tells the scripts when the item under the pointer changes.
    void updateDockHover();
    /// The pointer left the hovered item: its script hears of it.
    void leaveDockHover();
    /// The docks of @p pane (of every pane and the window when null) go or
    /// stop showing here: their drag ends with "cancel", and their hovered
    /// item hears that the pointer left it.
    void dropDockPointer(const Pane* pane);
    /// Wheel travel over m_wheelDock that is not yet a whole step.
    QPoint m_dockWheel;
    DockHover m_wheelDock;
    /// A left-button drag that began on a dock item. It starts once the
    /// pointer has gone the drag distance from the press.
    struct DockDrag
    {
        QString owner, dock, item;
        quint32 pane = 0;
        QPointF from; ///< the press, window logical pixels
        bool started = false;
        /// What the last event told: the item and the cell under the pointer.
        QString over;
        QPoint cell;
    };
    std::optional<DockDrag> m_dockDrag;
    /// The pointer at @p pos while a dock drag is held: starts the drag,
    /// or tells its script of a new item or cell under the pointer.
    void moveDockDrag(const QPointF& pos);
    /// Ends the dock drag. A drag that started tells its script: "end" for
    /// a release at @p pos, "cancel" when the drag was cut off.
    void endDockDrag(bool released, const QPointF& pos = {});
    /// Where the last dock click was, window logical pixels: a tab menu
    /// that a script opens for it opens there. A new pointer gesture or the
    /// menu takes it.
    std::optional<QPointF> m_dockClickAt;
    /// Tells each dock's script where it shows, when that changed.
    void reportDockLayouts();
    void reportDockLayout(DockArea& dock);
    /// Whether @p dock is one of this window's and shows.
    bool showsDock(const DockArea* dock) const;
    /// Framebuffer pixels to global logical ones.
    QRect globalRect(const QRect& pixels) const;
    /// Removes @p owner's docks.
    void eraseDocks(const QString& owner);
    /// A dock came, went or moved: the next frame lays out the panes and
    /// the chrome again (finishDockMoves), once for every change before it.
    bool m_docksMoved = false;
    void finishDockMoves();
    /// A tab_bar dock shows, in place of the tab bar.
    bool tabBarReplaced() const;
    QVariantMap scriptOverrides(Pane* pane) const;
    void cancelKeyMode();
    KeyModes::State m_keyMode;
    QTimer m_keyModeTimer;
    QElapsedTimer m_keyModeClock;
    /// Presses the window took, by physical key, so their releases stay
    /// here too. True when a shortcut took it: its auto-repeats run the
    /// shortcut again, but for a script command and the actions of
    /// onceWhileHeld (handleShortcut), which run once for each press. A
    /// mode or the chrome took the others, and their auto-repeats go
    /// nowhere once the mode has closed.
    QHash<quint32, bool> m_consumedKeys;
    /// A press that reached a pane's program, with what its release needs.
    struct SentKey {
        QPointer<Pane> pane;
        int key = 0;
        QString text;
        quint32 nativeScanCode = 0;
        std::uint64_t order = 0; ///< later presses have higher numbers
    };
    /// Presses that reached a pane's program, by physical key: the release
    /// goes to that pane whatever has the keyboard by then.
    QHash<quint32, SentKey> m_sentKeys;
    std::uint64_t m_sentKeyOrder = 0;
    /// Sends the release of every key in m_sentKeys to its pane now, and
    /// takes the keys: a drag takes the keyboard (QDrag::exec), and their
    /// releases would go to it.
    void releaseSentKeys();
    /// A mode holds the keyboard: the overview, URL mode, Unicode input,
    /// copy mode or a script's key mode. Input-method text does not reach
    /// the pane meanwhile.
    bool keyboardModeActive() const;
    /// Tells the input method to query the window again, after the keyboard
    /// owner or the terminal's text changed.
    void refreshInputMethod(Qt::InputMethodQueries queries = Qt::ImQueryAll);
    /// Tells the input method when a mode starts or stops holding the keyboard.
    void syncInputMethodEnabled();
    bool m_modeHeldKeyboard = false; ///< keyboardModeActive() as the input method last heard
    bool m_inputMethodDirty = false; ///< the focused pane's input pane's text changed since the last query
    bool performAction(Action action);
    void updateChrome();
    /// The command palette's list: every action but the palette itself and
    /// the end of a script key mode, then the script commands that have a
    /// palette title.
    void updatePaletteActions();
    /// Starts a script command from the palette, a key or the menu, with no
    /// arguments. @p token by value: the list it comes from can change
    /// while the run starts.
    void runScriptCommand(QString token, Pane* pane, const QString& source);
    /// The script commands with a palette title, which the palette and the
    /// context menu list.
    QList<ScriptCommand> listedScriptCommands() const;
    /// The command that @p token ("<script>.<name>") names, if a script has it.
    std::optional<ScriptCommand> scriptCommand(const QString& token) const;
    /// The keys that run script command @p command, as the palette shows them.
    QStringList scriptCommandShortcuts(const QString& command) const;
    /// runAction for the window's own menu, which closed before its item
    /// runs: no refusal for an open prompt.
    bool actOnPane(Pane* pane, Action action);
    /// A prompt, a menu or a script dialog waits for its answer. The
    /// palette and the search are not prompts.
    bool chromePromptOpen() const;
    bool chromeVisible() const;
    /// Routes a pointer event to the chrome when it is over it (or the chrome
    /// holds a press). Returns true if the chrome took it.
    bool routePointerToChrome(QEvent* e, const QPointF& pos, bool press, bool release);
    void runSearch(const QString& query, int direction);
    /// Finds the link under the pointer while the link modifier is held and underlines it.
    void updateLinkHover();
    /// The pointer for where it is: a link hand, the program's shape, or the default.
    void applyPointerShape();
    /// Shows @p pane for an assistive technology's action and lays its tab
    /// out. False when the pane is not this window's or the window does not
    /// accept accessible actions now.
    bool revealForAccessibility(Pane* pane);
    /// The chrome held the keyboard when it last changed: when it lets go,
    /// assistive technology hears that the focused pane has the focus again.
    bool m_chromeHeldKeyboard = false;
    bool m_accessibleExposed = false; ///< isExposed() as assistive technology last heard
    /// The cursor cell in window coordinates, for the input method's popup.
    QRectF inputMethodCursorRect() const;
    /// @p pane's cursor cell, window logical pixels.
    QRectF cursorCellRect(Pane* pane) const;
    QRectF m_reportedImeRect;
    void ringBell(Pane* pane);
    /// Plays Terminal.bellSound, at most once in Terminal.bellSoundInterval.
    void playBellSound();
    /// Starts the visual bell's flash over @p pane.
    void flashPane(Pane* pane);
    /// A key or input-method text went to the focused pane: the cursor
    /// stops blinking for a moment and the pointer may hide.
    void noteTyping();

    // Program status (OSC 7501)
    struct StatusView
    {
        QList<PaneStatusRecord> records; ///< cached, so drawing never locks the pane
        quint64 acknowledged = 0; ///< done and error records up to this serial were seen
        QHash<QString, int> lastStates; ///< id -> state, to notice changes worth a notification
        QElapsedTimer lastNotification; ///< steady, so a clock change cannot silence it
    };
    std::unordered_map<const Pane*, StatusView> m_statusViews;
    void paneStatusChanged(Pane* pane);
    /// The focused pane of an active window has been seen: its done and
    /// error records stop showing.
    void acknowledgeStatus();
    /// The most pressing record of @p pane: { state, progress, text }, or
    /// an empty map when nothing is worth showing.
    QVariantMap statusSummary(const Pane* pane) const;
    /// Taskbar progress and urgency across all windows.
    static void publishLauncherStatus();

    // Animation
    PaneMotion& motion(const Pane* pane);
    /// Scrolls a pane's view by @p rows (positive is back in history),
    /// eased when @p animate and scroll animation is on.
    void scrollPane(Pane* pane, double rows, bool animate = true);
    /// Takes a move of @p pane's view by the terminal (output, typing) into
    /// @p m: the easing follows it.
    void followViewport(Pane* pane, PaneMotion& m);
    /// Shows the rows of @p m's scroll position in @p pane, sets its pixel
    /// shift and refreshes the snapshot. False while the pane holds its frame.
    bool applyScroll(Pane* pane, PaneMotion& m);
    /// Ends @p pane's scroll easing at its target now, with the snapshot of
    /// that place, for a mode that reads the rows as drawn.
    void settleScroll(Pane* pane);
    /// Advances a pane's motion and returns how to draw it; refreshes its
    /// snapshot. Sets @p animating when something is still moving.
    void preparePane(Pane* pane, const QRect& layoutRect, AfterglowRender::FrameBuilder::PaneParams& params,
                     bool& holding, bool& animating);
    void applySettings(const Settings& previous);
    /// The search highlight colours (Colors.searchMatch and the rest) for @p pane.
    void applySearchColors(Pane* pane);
    void applyShaders();
    void applyWindowEffects();
    void setUpQuickTerminal();
    /// Moves the quick terminal @p offset pixels out from its screen edge: 0
    /// shows all of it, and a negative offset puts that many pixel rows of it
    /// off screen.
    void setQuickOffset(int offset);
    /// The offset of the slide's frame now, with one pixel row on screen at least.
    int slideOffset() const;
    QMetaObject::Connection m_quickScreenWatch; ///< the quick terminal's screen changing size
    /// Encodes @p e for @p pane (the focused pane's input pane when null) and sends it.
    void sendKey(QKeyEvent* e, Pane* pane = nullptr);
    void paste(QClipboard::Mode mode);
    /// Pastes @p text into @p target, with the size limit and the question
    /// before an unsafe paste (Terminal.confirmUnsafePaste).
    void pasteText(Pane* target, const QByteArray& text);

    // Clipboard protocol (OSC 5522)
    void clipboardRequest(Pane* pane, const AfterglowVt::ClipboardRequest& request);
    void answerClipboardRead(Pane* pane, const AfterglowVt::ClipboardRequest& request, std::string_view oneTimePassword = {});
    /// Who asks in a clipboard prompt, in words the program cannot disguise
    /// (@p name, else the pane's title), and where the pane is when it is
    /// not the one the user looks at.
    QString clipboardAsker(Pane* pane, const QString& name = {}) const;
    /// A program's clipboard write (OSC 52 or 5522), as Terminal.clipboardWrite says.
    void clipboardWrite(Pane* pane, const AfterglowVt::ClipboardRequest& request);
    void writeClipboard(Pane* pane, const AfterglowVt::ClipboardRequest& request);
    /// Pastes as OSC 5522 when the program asked for paste events.
    bool sendPasteEvent(QClipboard::Mode mode);
    struct ClipboardGrant
    {
        bool oneTime = false;
        /// A paste's grant reads only the buffer that was pasted.
        std::optional<bool> primary;
        /// A paste's grant expires: the program reads right after the paste.
        std::optional<std::chrono::steady_clock::time_point> expires;
    };
    /// Per pane: passwords the user allowed to read the clipboard.
    std::unordered_map<const Pane*, QHash<QByteArray, ClipboardGrant>> m_clipboardGrants;
    struct PendingClipboardRead
    {
        QPointer<Pane> pane;
        AfterglowVt::ClipboardRequest request;
        bool write = false; ///< a write that Terminal.clipboardWrite "ask" holds
    };
    std::optional<PendingClipboardRead> m_pendingClipboardRead;
    void updateTitle();
    void restartBlink();
    enum class ProblemSource { Config, Shaders };
    /// Shows what the config file (or the shader library) could not use as
    /// a notice, once for each new list, or @p again after a reload the user
    /// asked for.
    static void reportProblems(ProblemSource source, const QStringList& problems, bool again = false);

    QRect contentRect(const QRect& paneRect) const;
    /// The grid of @p pane drawn at @p paneRect: what its docks leave, less
    /// the padding.
    QRect contentRect(const Pane* pane, const QRect& paneRect) const;
    /// What @p pane's docks that show leave of @p paneRect, with them at the
    /// sizes that the last layout gave them, or what is left of @p paneRect. With @p rects, where each of
    /// them goes (empty when it does not show), in the order of docks().
    QRect paneDockArea(const Pane* pane, const QRect& paneRect, std::vector<QRect>* rects = nullptr) const;
    /// Lays out @p pane's docks in its layout rectangle @p paneRect, and
    /// tells their scripts. Returns what they leave.
    QRect placePaneDocks(Pane* pane, const QRect& paneRect);
    /// Lays out @p pane at @p paneRect: its docks, then its grid and its
    /// shell's size. An empty @p paneRect (a zoom hides the pane) hides its
    /// docks and keeps the size.
    void layoutPane(Pane* pane, const QRect& paneRect);
    /// layoutPane for a pane in a tab that does not show, in its tab's last
    /// layout.
    void layoutBackgroundPane(Pane* pane);
    /// @p pane's docks, drawn at @p shown, into @p out.
    void addPaneDocks(Pane* pane, const QRect& shown, std::vector<AfterglowRender::WindowRenderer::Pane::Dock>& out);
    /// placePaneDocks for each pane of tab @p tab, as its tree lays it out.
    void placeTabDocks(int tab);
    /// The pane docks' rectangles of placePaneDocks and addPaneDocks, kept
    /// from frame to frame.
    std::vector<QRect> m_paneDockRects;
    /// The scroll shift of @p pane's last frame (PaneMotion::scrollShift).
    float drawnScrollShift(const Pane* pane) const;
    /// Where window position @p pos (logical) is on @p pane's grid as the
    /// last frame drew it, in pixels from the grid's top left corner.
    QPointF gridPixel(Pane* pane, const QPointF& pos) const;
    /// Cell under a window position, as the last frame drew the cells,
    /// clamped to the pane's grid.
    QPoint cellAt(Pane* pane, const QPointF& pos) const;
    /// Cell row under @p pos without clamping; negative above, >= rows below.
    int rawRowAt(Pane* pane, const QPointF& pos) const;
    void updateSelectionDrag();
    /// Starts a selection at @p pos of @p pane, as a press there does.
    void beginSelection(Pane* pane, const QPointF& pos, AfterglowVt::SelectionMode mode);
    /// Drags the selected text of @p pane out (QDrag, text/plain), and returns
    /// when the drop ends. False, with no drag, when the selection has no text.
    bool dragSelectionText(Pane* pane);
    void copySelection(bool toClipboard);
    /// Copies @p pane's new selection where Terminal.copyOnSelect says.
    void copyOnSelect(Pane* pane);
    bool sendMouse(Pane* pane, const QPointF& pos, int button, int action, Qt::KeyboardModifiers mods);

    AfterglowRender::GpuDevice& m_device;
    SettingsStore& m_settings;
    ShaderLibrary& m_shaders;
    ShaderControls m_shaderControls;
    QVariantMap m_sceneSample;
    qint64 m_sceneSampleMs = -1000;
    ScriptHost& m_scripts;
    AudioController& m_audio;
    /// Which chains read audio. An event chain counts only while it shows.
    struct AudioUse
    {
        bool alwaysOn = false; ///< layers, panes, window
        bool overview = false;
        bool tabTransition = false;
        bool paneEffects = false;
        bool overlayTransition = false;
    } m_audioUse;
    bool m_audioCapturing = false; ///< a chain that shows now reads audio
    /// Starts or stops audio capture for the chains that show now.
    void updateAudioCapture();
    bool m_readsMouse = false; ///< a chain reads the pointer: it moving is a frame
    QStringList m_paletteCommands; ///< the token of the command that palette id ScriptCommandBase + i runs
    ChromeController* m_controller = nullptr;
    ChromeLayer* m_chrome = nullptr;
    bool m_chromeGrab = false;
    std::unique_ptr<AfterglowRender::WindowRenderer> m_renderer;
    struct Tab
    {
        Tab() { static quint32 next = 1; id = next++; }
        quint32 id = 0; ///< process-lifetime identity, independent of tab position
        std::unique_ptr<SplitTree> splits;
        QPointer<Pane> focused;
        QString title; ///< set by the user; empty follows the focused pane
    };
    /// What the tab bar, scripts and control clients show for @p tab, whose shown pane
    /// is @p focused: the user's title or the pane's, with script suffixes.
    QString tabTitle(const Tab& tab, const Pane* focused) const;
    QString m_projectName;
    QString m_projectDirectory;
    std::vector<std::unique_ptr<Tab>> m_tabs;
    int m_currentTab = 0;
    // The current tab's tree and focused pane (focus is saved back on switch).
    SplitTree* m_splits = nullptr;
    QPointer<Pane> m_focused;
    const quint32 m_id;
    QSize m_layoutSize;
    double m_layoutDpr = 0;
    std::uint64_t m_layoutFontEpoch = 0;
    QPoint m_lastMouseCell{-1, -1};
    QPointer<Pane> m_lastMousePane; ///< the pane m_lastMouseCell is in
    bool m_windowFocused = false;
    /// Some pane drew a translucent background (a cell colour or a script's
    /// opacity). It stays set, so content cannot rebuild the swapchain over and over.
    bool m_contentTranslucent = false;

    std::unique_ptr<FrameClock> m_clock;
    bool m_quick = false;
    bool m_effectsApplied = false;
    bool m_quickShown = false;
    int m_quickHeight = 0;
    /// The dialogs of the quick terminal's panes that are open (keepDialogOverPane),
    /// with the pane each one asks for.
    std::vector<std::pair<QPointer<QDialog>, QPointer<Pane>>> m_quickDialogs;
    bool m_quickOutForDialogs = false; ///< the quick terminal slid out for m_quickDialogs
    bool m_quickDialogAdded = false; ///< a dialog came to m_quickDialogs since settleQuickDialogs looked
    PhosphorAnimation::AnimatedValue<qreal> m_slide;
    std::unordered_map<const Pane*, std::unique_ptr<PaneMotion>> m_motion;
    /// A screenshot that waits for its frame (captureFrame).
    struct PendingCapture
    {
        CaptureScope scope = CaptureScope::Pane;
        QPointer<Pane> target;
        CaptureReply reply;
        std::uint64_t ticket = 0; ///< the renderer's, once the frame is asked for
        QDeadlineTimer deadline; ///< for the next frame
    };
    /// A bound on a failure, not a value to tune: a frame takes milliseconds,
    /// and a window that cannot draw (its surface is lost, the compositor
    /// sends it no frames) gives an error after this.
    static constexpr int CaptureTimeoutMs = 2000;
    std::vector<std::shared_ptr<PendingCapture>> m_pendingCaptures;
    /// Fires at the first deadline of m_pendingCaptures.
    QTimer m_captureTimer;
    /// Before a frame: asks the renderer to read it back for the captures
    /// that wait no more, and restarts the deadline of those that wait.
    void prepareCaptures();
    /// @p capture waits for the end of a tab switch, or of a move of its pane.
    bool captureWaits(const PendingCapture& capture) const;
    /// Cuts @p image, the frame drawn for @p capture.
    void captureDrawn(const std::shared_ptr<PendingCapture>& capture, const QImage& image);
    /// Gives @p capture its answer and forgets it.
    void finishCapture(std::shared_ptr<PendingCapture> capture, const QString& error, const QString& code,
                       const QImage& image = {});
    /// Answers every pending capture with @p error (not_shown).
    void abortCaptures(const QString& error);
    /// Answers the captures whose deadline has passed with failed.
    void expireCaptures();
    /// Starts m_captureTimer for the first deadline, or stops it.
    void armCaptureTimer();
    /// Where a pane just created by a split grows from.
    std::unordered_map<const Pane*, QRectF> m_growFrom;
    /// Rows each pane's history has taken in, at the last frame: a view
    /// that moved by as many rows was kept on its text, not scrolled.
    std::unordered_map<const Pane*, std::uint64_t> m_historyRows;

    QTimer m_blinkTimer;
    QTimer m_textBlinkTimer; ///< SGR 5 blinking, running only while some shows
    QTimer m_holdRetry; ///< a frame again when a pane's held frame times out
    qint64 m_holdWait = 0; ///< this frame: the milliseconds until the first held frame times out
    bool m_textBlinkOn = true;
    /// Tells each pane whether it can be seen (mode 2033 reports).
    void updatePaneVisibility();
    bool m_blinkOn = true;
    double m_wheelAccumulator = 0;
    QPointer<Pane> m_wheelPane; ///< the pane m_wheelAccumulator belongs to
    Qt::MouseButtons m_reportedButtons;
    QPointer<Pane> m_reportedPane; ///< the pane whose program got the reported press
    Qt::MouseButtons m_overviewButtons; ///< the overview took their presses: it takes their releases

    // Links under the pointer (while the link modifier is held). Outside
    // the window, as before the pointer first enters it, it is (-1, -1).
    QPointF m_pointer{-1, -1};
    /// A link on one row: an OSC 8 hyperlink run or a URL in the text.
    struct UrlSpan
    {
        int colStart = 0;
        int colEnd = -1; ///< inclusive
        std::uint32_t link = 0; ///< snapshot link index, 0 for detected text
        QString url;
    };
    static std::vector<UrlSpan> findUrls(const AfterglowRender::PaneSnapshot& snap, int row);

    // URL mode: every link on screen gets a label to type.
    struct UrlHint
    {
        QString label;
        QString url;
        int row = 0;
        int col = 0;
        QString action = {};
        QStringList command = {};
    };
    struct UrlMode
    {
        bool active = false;
        bool copy = false;
        QPointer<Pane> pane;
        std::vector<UrlHint> hints;
        QString typed;
        std::uint64_t generation = 0;
        /// Labels found on a held frame (DEC 2026, a prompt redraw), older
        /// than the terminal: the next frame taken closes them.
        bool held = false;
    } m_urlMode;
    std::shared_ptr<std::atomic<bool>> m_hintCancel;
    void startHints();
    void activateHints(std::vector<UrlHint> hints, bool copy);
    void startUrlMode(bool copy);
    void stopUrlMode();
    void showUrlHints();
    void urlModeKey(QKeyEvent* e);

    struct Overview
    {
        bool open = false;
        QPointer<Pane> highlighted;
        QPointer<Pane> original; ///< focused when the overview opened
        /// Each tab's zoomed pane when the overview opened: the overview
        /// shows every pane, and closing zooms these tabs again.
        std::vector<QPointer<Pane>> zoomed;
        PhosphorAnimation::AnimatedValue<qreal> amount; ///< 0 flat, 1 open
        PhosphorAnimation::AnimatedValue<qreal> position; ///< the highlight, counting on past the last card
        qreal positionTarget = 0; ///< where it heads: equals the highlighted index modulo the cards
        std::optional<std::chrono::steady_clock::time_point> changedAt;
        std::optional<std::chrono::steady_clock::time_point> movedAt;
        bool wasShowing = false; ///< showing at the last frame
    } m_overview;
    /// A curve resolved from its setting once per spec.
    struct CurveCache
    {
        QString spec;
        std::shared_ptr<const PhosphorAnimation::Curve> curve;
    };
    CurveCache m_overviewCurve;
    CurveCache m_splitCurve;
    CurveCache m_cursorCurve;
    CurveCache m_scrollCurve;
    CurveCache m_bellCurve;
    CurveCache m_quickCurve;
    /// @p fallback is the setting's default, used when @p spec is not a curve.
    PhosphorAnimation::MotionSpec<qreal> motionSpec(const QString& spec, int durationMs, CurveCache& cache, const char* setting,
                                                    const QString& fallback = {});
    /// Another tab's panes for the renderer, prepared as visible panes are;
    /// only those in @p only when it is given.
    AfterglowRender::WindowRenderer::OverviewTab prepareOtherTab(int tab, const std::vector<Pane*>* only,
                                                                  std::uint8_t backgroundAlpha, bool& holding, bool& animating);
    // A tab switch (Shaders.tabTransition): the tab being left keeps drawing
    // until the new one has arrived.
    struct TabTransition
    {
        quint32 from = 0; ///< the tab being left, by id
        float direction = 1;
        PhosphorAnimation::AnimatedValue<qreal> progress;
        std::optional<std::chrono::steady_clock::time_point> startedAt;
    } m_tabTransition;
    CurveCache m_tabCurve;
    bool m_tabTransitionAvailable = false; ///< Shaders.tabTransition compiled
    /// Shaders.overlayTransition compiled: without it an overlay switches at once.
    bool m_overlayTransitionAvailable = false;
    /// Starts the fade of @p host's overlay toward @p target (1 in, 0 out),
    /// from where a fade under way is; none without the transition.
    void startOverlayFade(Pane* host, qreal target);
    void startTabTransition(int from, int to);
    // Panes opening and closing (Shaders.paneEffects).
    struct PaneOpening
    {
        std::chrono::steady_clock::time_point at;
        int edge = 0; ///< 1 left, 2 top, 3 right, 4 bottom
    };
    QHash<const Pane*, PaneOpening> m_paneOpening;
    struct ClosingPane
    {
        std::shared_ptr<AfterglowRender::PaneSnapshot> snapshot; ///< its last frame
        QRect rect;
        QPoint contentOrigin;
        /// Its docks' last frames, and where they were drawn.
        std::vector<std::pair<std::shared_ptr<AfterglowRender::PaneSnapshot>, QRect>> docks;
        std::chrono::steady_clock::time_point at;
        quint32 tab = 0; ///< the tab it closed in, by id
        std::uint8_t backgroundAlpha = 255; ///< with the pane's script override
    };
    std::optional<ClosingPane> m_closingPane;
    struct PaneContext
    {
        int kind = 0; ///< 0 here, 1 remote, 2 elevated, 3 a container
        std::chrono::steady_clock::time_point at; ///< when it last changed
        int previous = 0; ///< the kind before that change
    };
    QHash<const Pane*, PaneContext> m_paneContext;
    QHash<const Pane*, PaneOrigin> m_paneOrigin;
    struct PanePulse
    {
        std::chrono::steady_clock::time_point at;
        int durationMs = 0;
        int effect = 0;
        int color = 0;
    };
    QHash<const Pane*, PanePulse> m_panePulse;
    struct ScriptKeyMode
    {
        QString owner;
        QString label;
        QPointer<Pane> pane;
        /// Started while the overview was open: it drives the overview, and
        /// closing the overview (a click, a script) ends it.
        bool withOverview = false;
    };
    std::optional<ScriptKeyMode> m_scriptKeyMode;
    bool m_anyPaneAway = false; ///< this frame: some visible pane is not here
    bool m_paneEffectsAvailable = false; ///< Shaders.paneEffects compiled
    bool paneEffectsAllowed() const;
    /// How far an effect that started at @p at has run, 0 to 1.
    float paneEffectProgress(std::chrono::steady_clock::time_point at) const;
    /// Hands opening and closing panes to the renderer.
    void advancePaneEffects(bool& animating);
    bool m_paneEffectsShowing = false; ///< the last frame ran the pane effects chain
    /// Fills @p params with @p pane's context and pulse, for any pane drawn
    /// (the current tab, overview cards, the tab being left).
    void applyPaneEffectState(Pane* pane, AfterglowRender::FrameBuilder::PaneParams& params, bool& animating);
    /// The overlay over @p pane as it shows now, into @p wp: its grid fading
    /// in, or the last frame of one that ended fading out.
    void prepareOverlay(Pane* pane, const QRect& layoutRect, AfterglowRender::WindowRenderer::Pane& wp, bool& holding,
                        bool& animating);
    /// An overlay's fade, by its host: in after it opens, out after it ends.
    struct OverlayFade
    {
        PhosphorAnimation::AnimatedValue<qreal> progress;
        /// The last frame of an overlay that ended, while it fades out, and
        /// where it was when it was a popup.
        std::shared_ptr<AfterglowRender::PaneSnapshot> last;
        std::optional<OverlaySpec::Placement> lastPopup;
        std::string lastTitle; ///< UTF-8, as the border shows it
        /// prepareOverlay advanced it in the frame that is being built.
        bool advanced = false;
    };
    /// A popup's grid at @p placement in its host's grid, drawn at @p origin
    /// (the host's cell 0,0), with @p title (UTF-8) in its border, into
    /// @p shown.
    void preparePopup(const OverlaySpec::Placement& placement, const std::string& title, QPoint origin,
                      AfterglowRender::WindowRenderer::Pane::Overlay& shown) const;
    std::unordered_map<const Pane*, OverlayFade> m_overlayFades;
    CurveCache m_overlayCurve;
    /// The unfocused dim (when @p dim) and the dividers of a pane drawn at
    /// @p shown, as the window draws them for the current tab: colour
    /// rectangles for the renderer, not a PaneOverlay.
    void addPaneShading(std::vector<std::pair<QRect, std::uint32_t>>& shading, const QRect& shown, bool dim,
                        const QVariantMap& overrides) const;
    /// Advances the tab switch and hands it to the renderer.
    void advanceTabTransition(std::uint8_t backgroundAlpha, bool& holding, bool& animating);
    bool m_overviewAvailable = false; ///< Shaders.overview compiled
    int m_overviewWheel = 0; ///< wheel travel not yet a whole step
    /// The overview's cards: while it shows, every tab's panes in tab order,
    /// then layout order; otherwise the current tab's.
    std::vector<Pane*> overviewPanes() const;
    /// True while the overview is open or still easing open or shut.
    bool overviewShowing() const;
    /// Applies the pane's script overrides (backgroundOpacity, cursorBlink)
    /// over the window's @p backgroundAlpha, and returns all of them.
    QVariantMap applyScriptOverrides(Pane* pane, AfterglowRender::FrameBuilder::PaneParams& params, std::uint8_t backgroundAlpha) const;
    void layoutOtherTabs();
    /// Focuses @p chosen and closes; nullptr cancels back to the original pane.
    void closeOverview(Pane* chosen);
    /// Zooms the tabs that were zoomed when the overview opened again, a
    /// tab that holds @p target on the target.
    void restoreOverviewZooms(Pane* target);
    /// Highlights card @p index, the short way round, or in @p direction (1
    /// forward, -1 back) when it is given.
    void moveOverview(int index, int direction = 0);
    /// The cards were @p before and changed order (a tab moved): the deck
    /// counts its position in the new order, so the card in front stays.
    void rebaseOverview(const std::vector<Pane*>& before);
    void overviewKey(QKeyEvent* e);
    PhosphorAnimation::MotionSpec<qreal> overviewSpec();
    /// Advances the overview's motion. Closes it when its panes went away.
    /// Sets @p animating while it moves.
    void advanceOverview(bool& animating);

    // Unicode input: a codepoint typed in hex.
    std::optional<QString> m_unicodeInput;
    void showUnicodeInput();
    /// Ends Unicode input, when it is open, without inserting anything.
    void stopUnicodeInput();
    void unicodeInputKey(QKeyEvent* e);

    struct LinkHover
    {
        QPointer<Pane> pane;
        std::uint32_t link = 0; ///< snapshot link index (OSC 8)
        int row = -1; ///< a detected URL's cells
        int colStart = 0;
        int colEnd = -1;
        QString url;
        /// The version of the snapshot it was found in: output or a scroll
        /// since moves the text under it (refreshLinkHover).
        std::uint64_t version = 0;
    } m_hover;
    /// Finds the link under the pointer again when @p pane's snapshot is
    /// not the one m_hover was found in. After the pane's refreshSnapshot.
    void refreshLinkHover(Pane* pane);
    /// The link (OSC 8 or a URL in the text) under @p pos in @p pane, if any.
    LinkHover linkAt(Pane* pane, const QPointF& pos) const;
    /// The right-click menu, at @p pos (logical pixels).
    void openContextMenu(const QPointF& pos);
    /// Opens @p url with Links.openWith, or the desktop's handler.
    void openLink(const QString& url);
    /// The link modifier (Links.modifier) is held, or none is needed.
    bool linkModifierHeld() const;
    QString m_menuLink; ///< the link the menu was opened on
    QStringList m_menuCommands; ///< the token of the command that menu id MenuScriptBase + i runs
    /// The pane the context menu was opened on: its items act on it, even
    /// when the focus moved while the menu was open.
    QPointer<Pane> m_menuPane;
    /// Tab::id of the tab the last menu was opened on (0: a pane menu, or
    /// none). A dismissed tab menu leaves it set, which is harmless: closing a
    /// closed menu does nothing. An id, not an address: a tab created after a
    /// move can reuse a freed one.
    quint32 m_menuTab = 0;
    /// What the open title prompt renames: a pane, or (pane null) the tab
    /// with this Tab::id (0: none).
    QPointer<Pane> m_titlePane;
    quint32 m_titleTab = 0;
    /// Panes that notify when their next command finishes.
    QSet<const Pane*> m_notifyOnFinish;
    QByteArray m_pendingPaste;
    QPointer<Pane> m_pendingPastePane;
    QString m_fixedTitle; ///< afterglow --title
    bool m_holdNextPane = false; ///< afterglow --hold, for the first pane
    std::function<void()> m_pendingConfirm; ///< what a confirm prompt goes ahead with
    QList<QPointer<Pane>> m_pendingConfirmPanes; ///< the panes the confirm prompt is about
    bool m_closeConfirmed = false; ///< the window may close without asking
    bool m_pointerHidden = false; ///< hidden while typing
    QElapsedTimer m_lastBellSound;
    std::optional<SplitTree::Divider> m_dividerDrag;

    // Selection dragging.
    QPointer<Pane> m_selectionPane;
    /// A press inside the selection (Mouse.dragSelection), held back: a move
    /// drags the text out, a release clicks where the press was.
    QPointer<Pane> m_textDragPane;
    QPointF m_textDragFrom;
    /// The pane whose selection a drag carries while it runs: no drop there.
    QPointer<Pane> m_textDragSource;
    bool m_textDragging = false; ///< dragSelectionText() is in QDrag::exec()
    /// The selection's press moved to another cell: its release is no click,
    /// even when output cleared the selection meanwhile.
    bool m_selectionDragged = false;
    QPointF m_dragPos;
    QTimer m_autoScroll;
    int m_clickCount = 0;
    QElapsedTimer m_lastClick;
    QPoint m_lastClickCell{-1, -1};
    QPointer<Pane> m_lastClickPane; ///< the pane m_lastClickCell is in
};

} // namespace Afterglow
