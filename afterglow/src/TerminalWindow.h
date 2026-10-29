// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "SplitTree.h"
#include "config/Settings.h"

#include <AfterglowRender/GridRenderer.h>

#include <QClipboard>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWindow>

#include <memory>

namespace AfterglowRender {
class GpuDevice;
}

namespace Afterglow {

class Pane;

/// A top-level terminal window: a split tree of panes drawn in one render
/// pass on the shared GpuDevice.
class TerminalWindow : public QWindow
{
    Q_OBJECT

public:
    TerminalWindow(AfterglowRender::GpuDevice& device, SettingsStore& settings, const QString& workingDirectory = {});
    ~TerminalWindow() override;

    static QList<TerminalWindow*> windows();

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
    Pane* createPane(const QString& workingDirectory, const QRect& area);
    void splitFocused(SplitTree::Direction direction);
    void closePane(Pane* pane);
    void setFocusedPane(Pane* pane);
    void scheduleRender();
    void renderNow();
    void relayout();
    bool handleShortcut(QKeyEvent* e);
    bool performAction(Action action);
    void applySettings(const Settings& previous);
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
    std::unique_ptr<AfterglowRender::WindowRenderer> m_renderer;
    std::unique_ptr<SplitTree> m_splits;
    QPointer<Pane> m_focused;
    QSize m_layoutSize;
    double m_layoutDpr = 0;
    std::uint64_t m_layoutFontEpoch = 0;
    QPoint m_lastMouseCell{-1, -1};
    bool m_windowFocused = false;

    QTimer m_blinkTimer;
    bool m_blinkOn = true;
    double m_wheelAccumulator = 0;
    Qt::MouseButtons m_reportedButtons;

    // Selection dragging.
    QPointer<Pane> m_selectionPane;
    QPointF m_dragPos;
    QTimer m_autoScroll;
    int m_clickCount = 0;
    QElapsedTimer m_lastClick;
    QPoint m_lastClickCell{-1, -1};
};

} // namespace Afterglow
