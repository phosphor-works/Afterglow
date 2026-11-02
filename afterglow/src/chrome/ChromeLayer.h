// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCursor>
#include <QObject>
#include <QSize>
#include <QUrl>

#include <cstdint>
#include <memory>

class QEvent;
class QInputMethodQueryEvent;
class QAccessibleInterface;
class QPointF;
class QQmlComponent;
class QQmlEngine;
class QQuickItem;
class QQuickWindow;
class QRhiRenderPassDescriptor;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QWindow;

namespace AfterglowRender {
class GpuDevice;
}

namespace Afterglow {

class ChromeController;
class ChromeIncubator;
class ChromeRenderControl;

/// The window chrome: a QML scene rendered by QQuickRenderControl into a
/// texture on the shared GpuDevice, which the window composites over the
/// terminal. No second GPU device or context is involved.
///
/// The scene is made in the background: the application loads the type on
/// the QML loader thread once, and each layer incubates its objects in short
/// slices of the event loop from the moment its window is made. The first
/// frame that shows chrome then only renders it. A frame that needs the
/// chrome before it is made finishes it.
class ChromeLayer : public QObject
{
    Q_OBJECT

public:
    /// @p component is the chrome type, shared by every window of the
    /// engine, which the application loads.
    ChromeLayer(AfterglowRender::GpuDevice& device, QWindow* host, QQmlComponent& component, ChromeController* controller);
    ~ChromeLayer() override;

    /// The chrome scene, by its URL in the module's resources. A component
    /// loaded from the module by name is created as a C++ type when its
    /// load ended before an incubator asks for it, and Qt 6.12 then makes no
    /// object and crashes; the document's URL always loads as QML.
    static QUrl sceneUrl();

    /// Re-renders the scene if it changed or the size did. Must be called
    /// outside any QRhi frame: Qt Quick records its own offscreen frame.
    void renderIfNeeded(const QSize& pixelSize);

    QRhiTexture* texture() const;
    /// Changes whenever the texture object is replaced.
    std::uint64_t serial() const noexcept { return m_serial; }

    /// True if the chrome handles the pointer at @p pos (window coordinates).
    /// @p event, when given, lets the chrome leave wheel events to the panes
    /// where it takes only presses (the history scrollbar).
    bool wantsPointer(const QPointF& pos, const QEvent* event = nullptr) const;
    /// Sends an input event to the QML scene.
    void deliver(QEvent* event);
    void queryInputMethod(QInputMethodQueryEvent* event) const;
    QQuickItem* rootItem() const { return m_root; }
    QQuickItem* focusedItem() const;
    /// The pointer the chrome asks for at @p pos (logical pixels): the
    /// cursor of the topmost item there that sets one, an arrow otherwise.
    QCursor cursorAt(const QPointF& pos) const;

Q_SIGNALS:
    /// The scene needs rendering; the host schedules a frame.
    void updateRequested();

private:
    /// The component loaded (or failed): starts incubating the scene.
    void componentStatusChanged();
    /// The incubator finished (or failed): puts the scene in its window.
    void sceneCreated(bool ready);
    /// Gives the scene the keyboard focus, once it has a window.
    void focusScene();
    /// Finishes the scene now, for a frame that shows chrome before the
    /// background made it.
    void completeScene();
    /// Gives the scene its GPU device. Needs the scene and the device.
    bool initialize();
    /// Frees the render target and the texture: after a failed allocation,
    /// and from the destructor once the scene graph is invalidated.
    void releaseResources();

    AfterglowRender::GpuDevice& m_device;
    QWindow* m_host;
    QQmlComponent& m_component;
    QQmlEngine& m_engine;
    ChromeController* m_controller;

    std::unique_ptr<ChromeIncubator> m_incubator;
    std::unique_ptr<ChromeRenderControl> m_control;
    std::unique_ptr<QQuickWindow> m_window;
    QQuickItem* m_root = nullptr;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPass;
    std::unique_ptr<QRhiTextureRenderTarget> m_target;
    QSize m_size;
    QSize m_failedSize;
    qreal m_dpr = 0;
    bool m_dirty = true;
    bool m_wanted = false; ///< a frame needed the chrome before it was made
    bool m_initialized = false;
    bool m_failed = false;
    std::uint64_t m_serial = 0;
};

} // namespace Afterglow
