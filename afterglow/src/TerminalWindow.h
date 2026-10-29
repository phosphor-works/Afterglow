// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "SplitTree.h"
#include "animation/FrameClock.h"
#include "animation/PaneMotion.h"
#include "app/Services.h"
#include "config/Settings.h"

#include <AfterglowRender/GridRenderer.h>

#include <QClipboard>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWindow>

#include <memory>
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
};

class ChromeController;
class ChromeLayer;
class Pane;
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

    /// Slides the quick terminal in or out (quick windows only).
    void toggleQuick();

Q_SIGNALS:
    /// The tab list, the current tab or a tab title changed.
    void tabsChanged();
    /// The new-window action, for the application to fulfil.
    void newWindowRequested(const QString& workingDirectory);

protected:
    bool event(QEvent* e) override;
    void exposeEvent(QExposeEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;

private:
    /// Creates a pane and starts its shell sized for @p area (pane rect, pixels).
    Pane* createPane(const QString& workingDirectory, const QRect& area, const QStringList& command = {});
    void splitFocused(SplitTree::Direction direction);
    void newTab(const QString& workingDirectory, const QStringList& command = {});
    void switchTab(int index);
    void closeTab(int index);
    int tabOf(const Pane* pane) const;
    /// Height of the tab bar in pixels; 0 while there is a single tab.
    int tabBarHeight() const;
    /// Where the current tab's panes are laid out, in pixels.
    QRect paneArea() const;
    void closePane(Pane* pane);
    void setFocusedPane(Pane* pane);
    void scheduleRender();
    void renderNow();
    void relayout();
    bool handleShortcut(QKeyEvent* e);
    bool performAction(Action action);
    void updateChrome();
    bool chromeVisible() const;
    /// Routes a pointer event to the chrome when it is over it (or the chrome
    /// holds a press). Returns true if the chrome took it.
    bool routePointerToChrome(QEvent* e, const QPointF& pos, bool press, bool release);
    void runSearch(const QString& query, int direction);
    /// Finds the link under the pointer while Ctrl is held and underlines it.
    void updateLinkHover();
    /// The pointer for where it is: a link hand, the program's shape, or the default.
    void applyPointerShape();
    void sendNotification(const QString& title, const QString& body);
    void ringBell(Pane* pane);
    void pasteText(Pane* pane, const QByteArray& text);

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
    void applyShaders();
    void applyWindowEffects();
    void setUpQuickTerminal();
    void sendKey(QKeyEvent* e);
    void paste(QClipboard::Mode mode);
    void updateTitle();
    void restartBlink();

    QRect contentRect(const QRect& paneRect) const;
    /// Cell under a window position, clamped to the pane's grid.
    QPoint cellAt(Pane* pane, const QPointF& pos) const;
    /// Cell row under @p pos without clamping; negative above, >= rows below.
    int rawRowAt(Pane* pane, const QPointF& pos) const;
    void updateSelectionDrag();
    void copySelection(bool toClipboard);
    bool sendMouse(Pane* pane, const QPointF& pos, int button, int action, Qt::KeyboardModifiers mods);

    AfterglowRender::GpuDevice& m_device;
    SettingsStore& m_settings;
    QQmlEngine& m_engine;
    ShaderLibrary& m_shaders;
    ChromeController* m_controller = nullptr;
    ChromeLayer* m_chrome = nullptr;
    bool m_chromeGrab = false;
    std::unique_ptr<AfterglowRender::WindowRenderer> m_renderer;
    struct Tab
    {
        std::unique_ptr<SplitTree> splits;
        QPointer<Pane> focused;
    };
    std::vector<std::unique_ptr<Tab>> m_tabs;
    int m_currentTab = 0;
    // The current tab's tree and focused pane (focus is saved back on switch).
    SplitTree* m_splits = nullptr;
    QPointer<Pane> m_focused;
    QSize m_layoutSize;
    double m_layoutDpr = 0;
    std::uint64_t m_layoutFontEpoch = 0;
    QPoint m_lastMouseCell{-1, -1};
    bool m_windowFocused = false;

    std::unique_ptr<FrameClock> m_clock;
    bool m_quick = false;
    bool m_effectsApplied = false;
    bool m_quickShown = false;
    int m_quickHeight = 0;
    PhosphorAnimation::AnimatedValue<qreal> m_slide;
    std::unordered_map<const Pane*, std::unique_ptr<PaneMotion>> m_motion;
    /// Where a pane just created by a split grows from.
    std::unordered_map<const Pane*, QRectF> m_growFrom;

    QTimer m_blinkTimer;
    bool m_blinkOn = true;
    double m_wheelAccumulator = 0;
    Qt::MouseButtons m_reportedButtons;

    // Links under the pointer (Ctrl held).
    QPointF m_pointer;
    struct LinkHover
    {
        QPointer<Pane> pane;
        std::uint32_t link = 0; ///< snapshot link index (OSC 8)
        int row = -1; ///< a detected URL's cells
        int colStart = 0;
        int colEnd = -1;
        QString url;
    } m_hover;
    QByteArray m_pendingPaste;

    // Selection dragging.
    QPointer<Pane> m_selectionPane;
    QPointF m_dragPos;
    QTimer m_autoScroll;
    int m_clickCount = 0;
    QElapsedTimer m_lastClick;
    QPoint m_lastClickCell{-1, -1};
};

} // namespace Afterglow
