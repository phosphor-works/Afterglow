// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ChromeLayer.h"

#include "ChromeController.h"

#include <AfterglowRender/GpuDevice.h>

#include <QCoreApplication>
#include <QFocusEvent>
#include <QLoggingCategory>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <rhi/qrhi.h>

Q_LOGGING_CATEGORY(lcChrome, "afterglow.chrome", QtInfoMsg)

namespace Afterglow {

/// Reports the real window, so the scene uses its device pixel ratio and
/// positions popups relative to it.
class ChromeRenderControl : public QQuickRenderControl
{
public:
    explicit ChromeRenderControl(QWindow* host)
        : m_host(host)
    {
    }
    QWindow* renderWindow(QPoint* offset) override
    {
        if (offset) {
            *offset = QPoint(0, 0);
        }
        return m_host;
    }

private:
    QWindow* m_host;
};

ChromeLayer::ChromeLayer(AfterglowRender::GpuDevice& device, QWindow* host, QQmlEngine& engine,
                         ChromeController* controller)
    : QObject(host)
    , m_device(device)
    , m_host(host)
    , m_engine(engine)
    , m_controller(controller)
{
}

ChromeLayer::~ChromeLayer()
{
    releaseResources();
    delete m_root;
    m_window.reset();
    m_control.reset();
}

bool ChromeLayer::initialize()
{
    if (m_initialized || m_failed) {
        return m_initialized;
    }
    QRhi* rhi = m_device.rhi(m_host);
    m_control = std::make_unique<ChromeRenderControl>(m_host);
    m_window = std::make_unique<QQuickWindow>(m_control.get());
    m_window->setVulkanInstance(m_device.vulkanInstance());
    m_window->setGraphicsDevice(QQuickGraphicsDevice::fromRhi(rhi));
    m_window->setColor(Qt::transparent);
    if (!m_control->initialize()) {
        qCWarning(lcChrome) << "Qt Quick could not initialise on the shared device; running without chrome";
        m_failed = true;
        return false;
    }

    QQmlComponent component(&m_engine);
    component.loadFromModule("Afterglow.Chrome", "Chrome");
    QObject* obj = component.createWithInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(m_controller)}});
    m_root = qobject_cast<QQuickItem*>(obj);
    if (!m_root) {
        qCWarning(lcChrome).noquote() << "chrome QML failed to load:" << component.errorString();
        delete obj;
        m_failed = true;
        return false;
    }
    m_root->setParentItem(m_window->contentItem());

    auto markDirty = [this] {
        m_dirty = true;
        Q_EMIT updateRequested();
    };
    connect(m_control.get(), &QQuickRenderControl::renderRequested, this, markDirty);
    connect(m_control.get(), &QQuickRenderControl::sceneChanged, this, markDirty);

    // The offscreen window never gets real focus; tell it it has it so
    // text fields accept keys.
    QFocusEvent focusIn(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
    QCoreApplication::sendEvent(m_window.get(), &focusIn);

    m_initialized = true;
    return true;
}

void ChromeLayer::renderIfNeeded(const QSize& pixelSize)
{
    if (pixelSize.isEmpty() || !initialize()) {
        return;
    }
    if (pixelSize != m_size || !m_texture) {
        QRhi* rhi = m_device.rhi();
        m_target.reset();
        m_renderPass.reset();
        m_texture.reset(rhi->newTexture(QRhiTexture::RGBA8, pixelSize, 1, QRhiTexture::RenderTarget));
        m_texture->create();
        m_target.reset(rhi->newTextureRenderTarget({m_texture.get()}));
        m_renderPass.reset(m_target->newCompatibleRenderPassDescriptor());
        m_target->setRenderPassDescriptor(m_renderPass.get());
        m_target->create();
        m_window->setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(m_target.get()));
        const qreal dpr = m_host->devicePixelRatio();
        m_window->setGeometry(0, 0, int(std::lround(pixelSize.width() / dpr)),
                              int(std::lround(pixelSize.height() / dpr)));
        m_root->setSize(m_window->size());
        m_size = pixelSize;
        ++m_serial;
        m_dirty = true;
    }
    if (!m_dirty) {
        return;
    }
    m_dirty = false;
    qCDebug(lcChrome) << "rendering chrome at" << pixelSize;
    m_control->polishItems();
    m_control->beginFrame();
    m_control->sync();
    m_control->render();
    m_control->endFrame();
}

QRhiTexture* ChromeLayer::texture() const
{
    return m_initialized ? m_texture.get() : nullptr;
}

bool ChromeLayer::wantsPointer(const QPointF& pos) const
{
    if (!m_root) {
        return false;
    }
    // Chrome.qml declares wantsPointer(x: real, y: real): bool; the types must match.
    bool result = false;
    QMetaObject::invokeMethod(m_root, "wantsPointer", Q_RETURN_ARG(bool, result), Q_ARG(double, pos.x()),
                              Q_ARG(double, pos.y()));
    return result;
}

void ChromeLayer::deliver(QEvent* event)
{
    if (m_window) {
        QCoreApplication::sendEvent(m_window.get(), event);
    }
}

void ChromeLayer::releaseResources()
{
    m_target.reset();
    m_renderPass.reset();
    m_texture.reset();
    m_size = QSize();
}

} // namespace Afterglow
