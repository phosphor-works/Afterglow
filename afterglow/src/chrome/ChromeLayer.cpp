// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ChromeLayer.h"
#include "../desktop/Accessibility.h"
#include <QInputMethodEvent>

#include "ChromeController.h"

#include <AfterglowRender/GpuDevice.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQmlIncubationController>
#include <QQmlIncubator>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>

#include <algorithm>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QPointer>
#include <QTimer>
#include <rhi/qrhi.h>

#include <functional>

Q_LOGGING_CATEGORY(lcChrome, "afterglow.chrome", QtInfoMsg)

namespace Afterglow {

namespace {

/// The longest piece of a chrome scene that one event-loop turn makes: a
/// quarter of a 60 Hz frame, so a scene made in the background never holds
/// a frame back noticeably.
constexpr int IncubationSliceMs = 4;

/// Drives the engine's asynchronous incubators from the event loop, one
/// slice for each turn while any are pending. Without a controller the
/// engine makes every incubated scene synchronously. One for each engine:
/// the engine detaches it when it is destroyed, and it is deleted after.
class ChromeIncubation : public QObject, public QQmlIncubationController
{
public:
    explicit ChromeIncubation(QQmlEngine& engine)
    {
        m_timer.setInterval(0);
        connect(&m_timer, &QTimer::timeout, this, [this] { incubateFor(IncubationSliceMs); });
        connect(&engine, &QObject::destroyed, this, &QObject::deleteLater);
        engine.setIncubationController(this);
    }

protected:
    void incubatingObjectCountChanged(int count) override
    {
        if (count > 0) m_timer.start();
        else m_timer.stop();
    }

private:
    QTimer m_timer;
};

} // namespace

/// Reports the end of the scene's incubation to the layer.
class ChromeIncubator : public QQmlIncubator
{
public:
    explicit ChromeIncubator(std::function<void(bool)> done)
        : QQmlIncubator(QQmlIncubator::Asynchronous)
        , m_done(std::move(done))
    {
    }

    /// Stops an unfinished incubation and deletes what it made so far.
    /// Destroying a QQmlIncubator alone does not stop it: the engine keeps
    /// the incubation and finishes it with nobody to report to, and its
    /// bindings then run against a controller that is gone.
    void abandon()
    {
        if (!isLoading()) {
            return;
        }
        // clear() only schedules the root for deletion, and in the last
        // phase its bindings are live: delete it now, before the controller.
        const QPointer<QObject> partial = m_partial;
        clear();
        delete partial.data();
    }

protected:
    void setInitialState(QObject* object) override { m_partial = object; }

    void statusChanged(Status status) override
    {
        if (status == Ready || status == Error) m_done(status == Ready);
    }

private:
    std::function<void(bool)> m_done;
    QPointer<QObject> m_partial;
};

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
    if (!m_engine.incubationController()) new ChromeIncubation(m_engine);
    m_component = std::make_unique<QQmlComponent>(&m_engine);
    connect(m_component.get(), &QQmlComponent::statusChanged, this, &ChromeLayer::componentStatusChanged);
    // The type, its imports and their plugins load on the QML loader thread.
    m_component->loadFromModule("Afterglow.Chrome", "Chrome", QQmlComponent::Asynchronous);
    componentStatusChanged();
    // The scene is made before the compositor activates the host, and Qt
    // Quick ignores the focus while another window has it: focus it again
    // when the host gets the focus.
    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow* window) {
        if (window == m_host) focusScene();
    });
}

ChromeLayer::~ChromeLayer()
{
    // The scene graph and render control still refer to our render target.
    // Invalidate them before releasing it, while the shared QRhi is alive.
    if (m_control) m_control->invalidate();
    releaseResources();
    // A scene still incubating is abandoned with its objects; a made one is ours.
    if (m_incubator) m_incubator->abandon();
    m_incubator.reset();
    delete m_root;
    m_root = nullptr;
    m_component.reset();
    // QQuickRenderControl detaches itself from the window on destruction.
    m_control.reset();
    m_window.reset();
}

void ChromeLayer::componentStatusChanged()
{
    if (m_incubator || m_failed) {
        return;
    }
    if (m_component->isError()) {
        qCWarning(lcChrome).noquote() << "chrome QML failed to load:" << m_component->errorString();
        m_failed = true;
        return;
    }
    if (!m_component->isReady()) {
        return;
    }
    m_incubator = std::make_unique<ChromeIncubator>([this](bool ready) { sceneCreated(ready); });
    m_incubator->setInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(m_controller)}});
    m_component->create(*m_incubator);
    if (m_wanted && m_incubator->isLoading()) {
        m_incubator->forceCompletion();
    }
}

void ChromeLayer::sceneCreated(bool ready)
{
    QObject* obj = ready ? m_incubator->object() : nullptr;
    m_root = qobject_cast<QQuickItem*>(obj);
    if (!m_root) {
        QStringList errors;
        for (const auto& error : m_incubator->errors()) errors << error.toString();
        if (obj) errors << QStringLiteral("the root object is not an Item");
        qCWarning(lcChrome).noquote() << "chrome QML failed to load:" << errors.join(QLatin1Char('\n'));
        delete obj;
        m_failed = true;
        return;
    }
    m_control = std::make_unique<ChromeRenderControl>(m_host);
    m_window = std::make_unique<QQuickWindow>(m_control.get());
    m_window->setColor(Qt::transparent);
    m_root->setParentItem(m_window->contentItem());
    // At the window's size now, so that the layout settles here and not on
    // the first frame; renderIfNeeded follows later changes.
    m_window->setGeometry(QRect(QPoint(), m_host->size()));
    m_root->setSize(m_host->size());
    installChromeAccessibility(m_root, m_host);

    auto markDirty = [this] {
        m_dirty = true;
        Q_EMIT updateRequested();
    };
    connect(m_control.get(), &QQuickRenderControl::renderRequested, this, markDirty);
    connect(m_control.get(), &QQuickRenderControl::sceneChanged, this, markDirty);

    focusScene();

    // The device exists once any window has drawn: take it now, not on the
    // frame that first shows chrome.
    if (m_device.rhi()) {
        initialize();
    }
    Q_EMIT updateRequested();
}

void ChromeLayer::focusScene()
{
    if (!m_window) {
        return;
    }
    // The offscreen window never gets real focus; tell it it has it so
    // text fields accept keys. Qt Quick takes it only while the host is the
    // application's focus window, or while no window is.
    QFocusEvent focusIn(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
    QCoreApplication::sendEvent(m_window.get(), &focusIn);
}

void ChromeLayer::completeScene()
{
    m_wanted = true;
    QElapsedTimer timer;
    timer.start();
    // Loading again synchronously waits for the loader thread's load in
    // progress; the status change then creates the scene, at once.
    if (m_component->isLoading()) {
        m_component->loadFromModule("Afterglow.Chrome", "Chrome", QQmlComponent::PreferSynchronous);
    }
    if (m_incubator && m_incubator->isLoading()) {
        m_incubator->forceCompletion();
    }
    qCDebug(lcChrome) << "chrome scene finished on demand in" << timer.elapsed() << "ms";
}

bool ChromeLayer::initialize()
{
    if (!m_root && !m_failed) {
        completeScene();
    }
    // completeScene may have initialised it already, through sceneCreated.
    if (m_initialized || m_failed || !m_root) {
        return m_initialized;
    }
    QRhi* rhi = m_device.rhi(m_host);
    m_window->setVulkanInstance(m_device.vulkanInstance());
    m_window->setGraphicsDevice(QQuickGraphicsDevice::fromRhi(rhi));
    if (!m_control->initialize()) {
        qCWarning(lcChrome) << "Qt Quick could not initialise on the shared device; running without chrome";
        // Nothing will draw the scene: drop it, so that the pointer and clicks
        // over where the chrome would be go to the panes.
        delete m_root;
        m_root = nullptr;
        m_failed = true;
        return false;
    }
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
    QElapsedTimer timer;
    timer.start();
    m_control->polishItems();
    m_control->beginFrame();
    m_control->sync();
    m_control->render();
    m_control->endFrame();
    qCDebug(lcChrome) << "rendered chrome at" << pixelSize << "in" << timer.nsecsElapsed() / 1000 << "us";
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
