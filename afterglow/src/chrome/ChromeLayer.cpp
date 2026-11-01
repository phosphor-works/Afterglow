// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ChromeLayer.h"
#include "../desktop/Accessibility.h"
#include <QInputMethodEvent>

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

#include <algorithm>
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
    // The scene graph and render control still refer to our render target.
    // Invalidate them before releasing it, while the shared QRhi is alive.
    if (m_control) m_control->invalidate();
    releaseResources();
    delete m_root;
    m_root = nullptr;
    // QQuickRenderControl detaches itself from the window on destruction.
    m_control.reset();
    m_window.reset();
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
    installChromeAccessibility(m_root, m_host);

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
    // A size that failed is not tried again until the window changes size.
    if (pixelSize.isEmpty() || pixelSize == m_failedSize || !initialize()) {
        return;
    }
    if (pixelSize != m_size || !m_texture) {
        QRhi* rhi = m_device.rhi();
        m_window->setRenderTarget(QQuickRenderTarget());
        m_target.reset();
        m_renderPass.reset();
        m_texture.reset(rhi->newTexture(QRhiTexture::RGBA8, pixelSize, 1, QRhiTexture::RenderTarget));
        if (!m_texture->create()) {
            qCWarning(lcChrome) << "could not create the chrome texture at" << pixelSize << "; drawing no chrome";
            releaseResources();
            m_failedSize = pixelSize;
            return;
        }
        m_target.reset(rhi->newTextureRenderTarget({m_texture.get()}));
        m_renderPass.reset(m_target->newCompatibleRenderPassDescriptor());
        m_target->setRenderPassDescriptor(m_renderPass.get());
        if (!m_target->create()) {
            qCWarning(lcChrome) << "could not create the chrome render target at" << pixelSize << "; drawing no chrome";
            releaseResources();
            m_failedSize = pixelSize;
            return;
        }
        m_window->setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(m_target.get()));
        m_size = pixelSize;
        m_failedSize = QSize();
        ++m_serial;
        m_dirty = true;
    }
    const qreal dpr = m_host->devicePixelRatio();
    const QSize logicalSize(int(std::lround(pixelSize.width() / dpr)), int(std::lround(pixelSize.height() / dpr)));
    if (m_dpr != dpr || m_window->size() != logicalSize) {
        m_dpr = dpr;
        m_window->setGeometry(QRect(QPoint(), logicalSize));
        m_root->setSize(logicalSize);
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

bool ChromeLayer::wantsPointer(const QPointF& pos, const QEvent* event) const
{
    if (!m_root) {
        return false;
    }
    // Chrome.qml declares wantsPointer(x: real, y: real, wheel: bool): bool; the types must match.
    const bool wheel = event && event->type() == QEvent::Wheel;
    bool result = false;
    QMetaObject::invokeMethod(m_root, "wantsPointer", Q_RETURN_ARG(bool, result), Q_ARG(double, pos.x()),
                              Q_ARG(double, pos.y()), Q_ARG(bool, wheel));
    return result;
}

void ChromeLayer::deliver(QEvent* event)
{
    if (event->type() == QEvent::InputMethod && focusedItem()) {
        QCoreApplication::sendEvent(focusedItem(), event);
    } else if (m_window) {
        QCoreApplication::sendEvent(m_window.get(), event);
    }
}

namespace {

/// Finds the cursor at @p scenePos under @p item, topmost first. An enabled
/// mouse area without a cursor of its own (a backdrop) hides what is below.
bool cursorUnder(const QQuickItem* item, const QPointF& scenePos, QCursor& out)
{
    if (!item->isVisible() || item->opacity() <= 0.0) {
        return false;
    }
    QList<QQuickItem*> children = item->childItems();
    std::stable_sort(children.begin(), children.end(), [](const QQuickItem* a, const QQuickItem* b) { return a->z() < b->z(); });
    for (auto it = children.crbegin(); it != children.crend(); ++it) {
        if (cursorUnder(*it, scenePos, out)) {
            return true;
        }
    }
    if (!item->contains(item->mapFromScene(scenePos))) {
        return false;
    }
    if (item->cursor().shape() != Qt::ArrowCursor) {
        out = item->cursor();
        return true;
    }
    if (item->isEnabled() && item->inherits("QQuickMouseArea")) {
        out = QCursor(Qt::ArrowCursor);
        return true;
    }
    return false;
}

} // namespace

QCursor ChromeLayer::cursorAt(const QPointF& pos) const
{
    QCursor cursor(Qt::ArrowCursor);
    if (m_root) {
        cursorUnder(m_root, pos, cursor);
    }
    return cursor;
}

void ChromeLayer::releaseResources()
{
    if (m_window) m_window->setRenderTarget(QQuickRenderTarget());
    m_target.reset();
    m_renderPass.reset();
    m_texture.reset();
    m_size = QSize();
}

} // namespace Afterglow

void Afterglow::ChromeLayer::queryInputMethod(QInputMethodQueryEvent* event) const
{
    auto* focus = focusedItem();
    if (!focus) { event->setValue(Qt::ImEnabled, false); event->accept(); return; }
    for (unsigned bit = 1; bit != 0; bit <<= 1) {
        const auto query = static_cast<Qt::InputMethodQuery>(bit);
        if (!(event->queries() & query)) continue;
        QVariant value = focus->inputMethodQuery(query);
        if (query == Qt::ImCursorRectangle || query == Qt::ImAnchorRectangle || query == Qt::ImInputItemClipRectangle)
            value = focus->mapRectToScene(value.toRectF());
        event->setValue(query, value);
    }
    event->accept();
}

QQuickItem* Afterglow::ChromeLayer::focusedItem() const
{
    if (!m_window) return nullptr;
    auto* item = m_window->activeFocusItem();
    if (!item) item = m_window->contentItem();
    while (auto* child = item->scopedFocusItem()) {
        if (child == item) break;
        item = child;
    }
    if (item->flags().testFlag(QQuickItem::ItemAcceptsInputMethod)) return item;
    // Render-control windows have no platform focus object. A visible focus
    // scope can retain its editor focus while the offscreen root is active.
    const auto find = [](auto&& self, QQuickItem* parent) -> QQuickItem* {
        if (!parent->isVisible() || !parent->isEnabled()) return nullptr;
        for (auto* child : parent->childItems()) if (auto* focus = self(self, child)) return focus;
        return parent->hasFocus() && parent->flags().testFlag(QQuickItem::ItemAcceptsInputMethod) ? parent : nullptr;
    };
    if (auto* editor = find(find, m_window->contentItem())) return editor;
    return item;
}
