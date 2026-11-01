// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PaneLaunchOptions.h"

#include "Pane.h"
#include "SplitTree.h"
#include "animation/FrameClock.h"
#include "animation/PaneMotion.h"
#include "app/Services.h"
#include "config/Settings.h"
#include "shaders/ShaderControls.h"

#include <AfterglowRender/GridRenderer.h>

#include <QClipboard>
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

class QQmlEngine;

namespace AfterglowRender {
class GpuDevice;
}

namespace Afterglow {

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
};

class ChromeController;
class ChromeLayer;
class Pane;
class ScriptHost;
class ShaderLibrary;

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
    QRect accessiblePaneRect(Pane*) const;
    QRect accessibleCellRect(Pane*, AfterglowVt::SelectionPoint) const;
    /// Switches to @p pane's tab, focuses it and raises the window.
    void revealPane(Pane* pane);

    // For the control interface and scripts.
    quint32 id() const noexcept { return m_id; }
    /// Developer diagnostics; collection is off unless explicitly enabled.
    void setGpuTimingEnabled(bool enabled) { m_renderer->setGpuTimingEnabled(enabled); }
    double completedGpuSeconds() const { return m_renderer->completedGpuSeconds(); }
    std::uint64_t submittedFrames() const { return m_renderer->submittedFrames(); }

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
    /// Opens a tab using @p options; returns its pane. With @p from, the tab
    /// opens beside that pane's tab and inherits its directory; otherwise
    /// beside the current tab, from the focused pane.
    Pane* openTab(const PaneLaunchOptions& options, QString* error, Pane* from = nullptr);
    void requestClosePane(Pane* pane) { closePane(pane); }
    /// Runs a keybind action as if it were pressed in @p pane.
    bool runAction(Pane* pane, Action action);
    using ScriptReply = std::function<void(QString, QVariantMap)>;
    QString showScriptDialog(const QString& owner, Pane*, const QVariantMap&, ScriptReply);
    QString setScriptAppearance(const QString& owner, Pane*, const QVariantMap&);
    void clearScriptOwner(const QString& owner);
    QString scriptShader(const QString& owner, Pane*, const QVariantMap&, QVariantMap&);
    /// A script's key mode: the window's keys go to the script's on_key until
    /// it ends the mode, fails or unloads. One script at a time.
    QString startScriptKeyMode(const QString& owner, Pane* pane, const QString& label);
    void endScriptKeyMode(const QString& owner);
    /// Scripts drive the pane overview: "open", "close" (back to where it
    /// was) or "choose" (the highlighted pane); and move its highlight to
    /// @p target, or by @p step cards when no target is given.
    QString scriptOverview(Pane* pane, const QString& operation, QVariantMap& result);
    QString scriptOverviewMove(Pane* pane, Pane* target, int step, QVariantMap& result);
    /// The overview for scripts: open, highlighted and its cards, by pane ID.
    QVariantMap overviewState() const;
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
    /// Creates a pane and starts its shell sized for @p area (pane rect, pixels).
    Pane* createPane(const QString& workingDirectory, const QRect& area, const QStringList& command = {}, QString* failure = nullptr, const QVariantMap& environment = {}, bool fallback = false);
    void bindPane(Pane* pane);
    void releasePaneUi(Pane* pane);
    /// Closes the prompts and modes that belong to @p pane, which is going
    /// away: its clipboard and paste prompts, title prompt, menu, URL mode
    /// and selection drag. Prompts about other panes stay.
    void dropPanePrompts(Pane* pane);
    void reparentPane(Pane* pane, TerminalWindow* destination);
    /// After a swap or rotation: shows every pane again when the tab was
    /// zoomed (@p wasZoomed), or lets the panes ease to their new places.
    void settleRearrangement(bool wasZoomed);
    void removeTabShell(int index);
    /// Shows the current tab after panes or tabs arrived or left. The tab
    /// that was current before (@p previousTab, by id, at @p previousIndex)
    /// counts as left when another one shows now.
    void finishTransfer(quint32 previousTab = 0, int previousIndex = -1);
    Pane* splitFocused(SplitTree::Direction direction, const QStringList& command = {}, const PaneLaunchOptions& options = {}, QString* error = nullptr);
    /// Opens a tab at @p at (beside the current tab when negative).
    Pane* newTab(const QString& workingDirectory, const QStringList& command = {}, const QVariantMap& environment = {}, QString* error = nullptr, int at = -1);
    void switchTab(int index);
    void closeTab(int index);
    /// Closes tabs from the tab bar, menus and keys: asks first where a program runs.
    void requestCloseTabs(const QList<int>& indices);
    /// Runs @p close, first asking when confirmClose is on and a program
    /// runs in one of @p panes. @p what names what closes: "this tab".
    void confirmClose(const QList<Pane*>& panes, const QString& what, std::function<void()> close);
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
    /// count when negative); 0 when hidden by its configured policy.
    int tabBarHeight(int tabCount = -1) const;
    /// Where the current tab's panes are laid out, in pixels, with
    /// @p tabCount tabs (the current count when negative).
    QRect paneArea(int tabCount = -1) const;
    /// The smallest pane that splits, resizes and divider drags leave, in pixels.
    QSize minimumPaneSize() const;
    /// @p pane is drawn this frame: visible in the current tab, a card of
    /// the overview, or in the tab a tab switch is leaving.
    bool paneDrawn(const Pane* pane) const;
    void closePane(Pane* pane);
    void setFocusedPane(Pane* pane);
    void scheduleRender();
    void renderNow();
    void relayout();
    void setPaneZoom(Pane* pane);
    bool resizeFocused(SplitTree::Neighbour direction);
    bool handleShortcut(QKeyEvent* e);
    void updateKeyMode();
    bool copyModeKey(QKeyEvent* e);
    /// Hands the key to the script holding the key mode, if one does.
    bool scriptKeyModeKey(QKeyEvent* e);
    void stopCopyMode();
    QPointer<Pane> m_copyPane;
    struct ScriptDialog { QString owner; QPointer<Pane> pane; ScriptReply reply; };
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
    QVariantMap scriptOverrides(Pane* pane) const;
    void cancelKeyMode();
    KeyModes::State m_keyMode;
    QTimer m_keyModeTimer;
    QElapsedTimer m_keyModeClock;
    /// Presses the window took, by physical key, so their releases stay
    /// here too. True when a shortcut took it: its auto-repeats run the
    /// shortcut again. A mode or the chrome took the others, and their
    /// auto-repeats go nowhere once the mode has closed.
    QHash<quint32, bool> m_consumedKeys;
    /// Presses that reached a pane's program, by physical key: the release
    /// goes to that pane whatever has the keyboard by then.
    QHash<quint32, QPointer<Pane>> m_sentKeys;
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
    bool m_inputMethodDirty = false; ///< the focused pane's text changed since the last query
    bool performAction(Action action);
    void updateChrome();
    /// The command palette's list: every action and the scripts' actions.
    void updatePaletteActions();
    bool chromeVisible() const;
    /// Routes a pointer event to the chrome when it is over it (or the chrome
    /// holds a press). Returns true if the chrome took it.
    bool routePointerToChrome(QEvent* e, const QPointF& pos, bool press, bool release);
    void runSearch(const QString& query, int direction);
    /// Finds the link under the pointer while the link modifier is held and underlines it.
    void updateLinkHover();
    /// The pointer for where it is: a link hand, the program's shape, or the default.
    void applyPointerShape();
    /// The cursor cell in window coordinates, for the input method's popup.
    QRectF inputMethodCursorRect() const;
    QRectF m_reportedImeRect;
    void ringBell(Pane* pane);
    /// Plays Terminal.bellSound, at most a few times a second.
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
    /// Moves the quick terminal @p offset pixels out from its screen edge
    /// (negative is off screen).
    void setQuickOffset(int offset);
    QMetaObject::Connection m_quickScreenWatch; ///< the quick terminal's screen changing size
    /// Encodes @p e for @p pane (the focused pane when null) and sends it.
    void sendKey(QKeyEvent* e, Pane* pane = nullptr);
    void paste(QClipboard::Mode mode);

    // Clipboard protocol (OSC 5522)
    void clipboardRequest(Pane* pane, const AfterglowVt::ClipboardRequest& request);
    void answerClipboardRead(Pane* pane, const AfterglowVt::ClipboardRequest& request, std::string_view oneTimePassword = {});
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
    /// Shows what the config file could not use as a notice, once for each
    /// new list, or @p again after a reload the user asked for.
    static void reportConfigProblems(const QStringList& problems, bool again = false);

    QRect contentRect(const QRect& paneRect) const;
    /// Cell under a window position, clamped to the pane's grid.
    QPoint cellAt(Pane* pane, const QPointF& pos) const;
    /// Cell row under @p pos without clamping; negative above, >= rows below.
    int rawRowAt(Pane* pane, const QPointF& pos) const;
    void updateSelectionDrag();
    void copySelection(bool toClipboard);
    /// Copies @p pane's new selection where Terminal.copyOnSelect says.
    void copyOnSelect(Pane* pane);
    bool sendMouse(Pane* pane, const QPointF& pos, int button, int action, Qt::KeyboardModifiers mods);

    AfterglowRender::GpuDevice& m_device;
    SettingsStore& m_settings;
    QQmlEngine& m_engine;
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
    } m_audioUse;
    bool m_audioCapturing = false; ///< a chain that shows now reads audio
    /// Starts or stops audio capture for the chains that show now.
    void updateAudioCapture();
    bool m_readsMouse = false; ///< a chain reads the pointer: it moving is a frame
    QStringList m_paletteScriptActions; ///< palette id ScriptActionBase + i runs entry i
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
    /// What the tab bar, scripts and D-Bus show for @p tab, whose shown pane
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
    PhosphorAnimation::AnimatedValue<qreal> m_slide;
    std::unordered_map<const Pane*, std::unique_ptr<PaneMotion>> m_motion;
    /// Where a pane just created by a split grows from.
    std::unordered_map<const Pane*, QRectF> m_growFrom;
    /// Rows each pane's history has taken in, at the last frame: a view
    /// that moved by as many rows was kept on its text, not scrolled.
    std::unordered_map<const Pane*, std::uint64_t> m_historyRows;

    QTimer m_blinkTimer;
    QTimer m_textBlinkTimer; ///< SGR 5 blinking, running only while some shows
    bool m_textBlinkOn = true;
    /// Tells each pane whether it can be seen (mode 2033 reports).
    void updatePaneVisibility();
    bool m_blinkOn = true;
    double m_wheelAccumulator = 0;
    QPointer<Pane> m_wheelPane; ///< the pane m_wheelAccumulator belongs to
    Qt::MouseButtons m_reportedButtons;
    QPointer<Pane> m_reportedPane; ///< the pane whose program got the reported press
    bool m_overviewSwallowedPress = false; ///< the overview took a press: it takes its release

    // Links under the pointer (while the link modifier is held).
    QPointF m_pointer;
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
        /// A card in another tab was chosen: its tab shows once the close
        /// has finished, so the cards never move with the current tab.
        QPointer<Pane> reveal;
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
    /// The unfocused dim (when @p dim) and the dividers of a pane drawn at
    /// @p shown, as the window draws them for the current tab.
    void addPaneOverlays(std::vector<std::pair<QRect, std::uint32_t>>& overlays, const QRect& shown, bool dim,
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
    void overviewKey(QKeyEvent* e);
    PhosphorAnimation::MotionSpec<qreal> overviewSpec();
    /// Advances the overview's motion. Closes it when its panes went away.
    /// Sets @p animating while it moves.
    void advanceOverview(bool& animating);

    // Unicode input: a codepoint typed in hex.
    std::optional<QString> m_unicodeInput;
    void showUnicodeInput();
    void unicodeInputKey(QKeyEvent* e);

    struct LinkHover
    {
        QPointer<Pane> pane;
        std::uint32_t link = 0; ///< snapshot link index (OSC 8)
        int row = -1; ///< a detected URL's cells
        int colStart = 0;
        int colEnd = -1;
        QString url;
    } m_hover;
    /// The link (OSC 8 or a URL in the text) under @p pos in @p pane, if any.
    LinkHover linkAt(Pane* pane, const QPointF& pos) const;
    /// The right-click menu, at @p pos (logical pixels).
    void openContextMenu(const QPointF& pos);
    /// Opens @p url with Links.openWith, or the desktop's handler.
    void openLink(const QString& url);
    /// The link modifier (Links.modifier) is held, or none is needed.
    bool linkModifierHeld() const;
    QString m_menuLink; ///< the link the menu was opened on
    QStringList m_menuScriptActions; ///< menu id MenuScriptBase + i runs entry i
    /// The pane the context menu was opened on: its items act on it, even
    /// when the focus moved while the menu was open.
    QPointer<Pane> m_menuPane;
    const Tab* m_menuTab = nullptr; ///< the tab a tab menu was opened on
    /// What the open title prompt renames: a pane, or (pane null) a tab.
    QPointer<Pane> m_titlePane;
    const Tab* m_titleTab = nullptr;
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
    QPointF m_dragPos;
    QTimer m_autoScroll;
    int m_clickCount = 0;
    QElapsedTimer m_lastClick;
    QPoint m_lastClickCell{-1, -1};
    QPointer<Pane> m_lastClickPane; ///< the pane m_lastClickCell is in
};

} // namespace Afterglow
