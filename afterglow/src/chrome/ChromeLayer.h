// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QSize>

#include <cstdint>
#include <memory>

class QEvent;
class QPointF;
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
class ChromeRenderControl;

/// The window chrome: a QML scene rendered by QQuickRenderControl into a
/// texture on the shared GpuDevice, which the window composites over the
/// terminal. No second GPU device or context is involved.
class ChromeLayer : public QObject
{
    Q_OBJECT

public:
    ChromeLayer(AfterglowRender::GpuDevice& device, QWindow* host, QQmlEngine& engine, ChromeController* controller);
    ~ChromeLayer() override;

    /// Re-renders the scene if it changed or the size did. Must be called
    /// outside any QRhi frame: Qt Quick records its own offscreen frame.
    void renderIfNeeded(const QSize& pixelSize);

    QRhiTexture* texture() const;
    /// Changes whenever the texture object is replaced.
    std::uint64_t serial() const noexcept { return m_serial; }

    /// True if the chrome handles the pointer at @p pos (window coordinates).
    bool wantsPointer(const QPointF& pos) const;
    /// Sends an input event to the QML scene.
    void deliver(QEvent* event);
    /// Releases GPU resources before the window's surface goes away.
    void releaseResources();

Q_SIGNALS:
    /// The scene needs rendering; the host schedules a frame.
    void updateRequested();

private:
    bool initialize();

    AfterglowRender::GpuDevice& m_device;
    QWindow* m_host;
    QQmlEngine& m_engine;
    ChromeController* m_controller;

    std::unique_ptr<ChromeRenderControl> m_control;
    std::unique_ptr<QQuickWindow> m_window;
    QQuickItem* m_root = nullptr;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPass;
    std::unique_ptr<QRhiTextureRenderTarget> m_target;
    QSize m_size;
    bool m_dirty = true;
    bool m_initialized = false;
    bool m_failed = false;
    std::uint64_t m_serial = 0;
};

} // namespace Afterglow
