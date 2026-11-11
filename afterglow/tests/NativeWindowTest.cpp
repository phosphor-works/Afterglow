// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Application.h"
#include "app/InstanceSocket.h"
#include "app/Startup.h"
#include "desktop/DragDrop.h"
#include "desktop/FileTransfer.h"
#include <QAbstractButton>
#include <QFileDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QMessageBox>
#include <chrono>
#include <csignal>
#include <optional>
#include <vector>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include "chrome/ChromeController.h"
#include "chrome/ChromeLayer.h"
#include "config/Keybinds.h"
#include "KeyMapping.h"
#include "DockArea.h"
#include "control/Control.h"
#include "control/ControlSocket.h"
#include "scripting/ScriptControl.h"
#include "TerminalWindow.h"
#include "config/ModifierRemap.h"
#include "config/Settings.h"
#include <AfterglowRender/FontSet.h>
#include <AfterglowRender/Snapshot.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QInputMethodEvent>
#include <QDrag>
#include <QPainter>
#include <QProcess>
#include <QRasterWindow>
#include <PhosphorWayland/LayerSurface.h>
#include <QMimeData>
#include <QMouseEvent>
#include <QStyleHints>
#include <QFile>
#include <QFontMetricsF>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <sys/stat.h>
#include <dlfcn.h>
#include <unistd.h>
#include <QAccessible>
#include "app/NotificationCenter.h"
#include <QDBusConnection>
#include <QDBusArgument>
#include <QBuffer>
#include <QClipboard>
#include <QImage>
#include <QQuickWindow>
#include <QQuickItem>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <AfterglowProto/Clipboard.h>
#include <limits>
#include <QThread>
#include <QThreadPool>
#include <QtGui/qtestsupport_gui.h>
#include <qpa/qwindowsysteminterface.h>
#include <QImageReader>
#include <QImageWriter>
#include <QTimer>
#include <QScopeGuard>
#include <QScreen>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include "QStringPrinter.h" // IWYU pragma: keep (gtest finds PrintTo(QString) by ADL)
#include "SavedEnvironment.h"
#include "TestDaemon.h"
#include "mux/MuxClient.h"
#include <gtest/gtest.h>
using namespace Afterglow;
using Afterglow::Test::Saved;
namespace {

/// The tests' Applications share a socket that listens nowhere: they take no
/// launches, and never touch the sockets of the user's display.
InstanceSocket& testSocket()
{
    static InstanceSocket socket;
    return socket;
}


void drain(int ms = 100) {
    QElapsedTimer timer; timer.start();
    do { QCoreApplication::processEvents(); QThread::msleep(2); } while (timer.elapsed() < ms);
}
// Deletes what a test's Application left with deleteLater, then drains what
// those deletes post. processEvents() never delivers a DeferredDelete posted
// outside an event loop, as the test body and a TearDown are: only an
// explicit send does.
void drainDeferredDeletes() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    drain();
}
// Waits until @p condition holds or @p ms pass, and returns the condition.
// A PTY echo or a search has no fixed time.
bool waitFor(const std::function<bool()>& condition, int ms = 3000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < ms) drain(10);
    return condition();
}
// Like waitFor, but deletes what was left with deleteLater on each look:
// a test body runs outside an event loop, where nothing else does.
bool waitDeleting(const std::function<bool()>& condition, int ms = 3000) {
    return waitFor([&] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return condition();
    }, ms);
}
// Waits until @p object is deleted.
template <class T>
bool waitDeleted(const QPointer<T>& object) {
    return waitDeleting([&] { return object.isNull(); });
}
// The visible top-level dialog of type @p T whose title starts with
// @p title, once one shows. Null when none shows in @p ms.
template <class T>
QPointer<T> waitForDialog(const QString& title, int ms = 3000) {
    QPointer<T> found;
    waitFor([&] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* dialog = qobject_cast<T*>(widget); dialog && dialog->isVisible() && dialog->windowTitle().startsWith(title)) found = dialog;
        return !found.isNull();
    }, ms);
    return found;
}
// Feeds @p bytes to @p pane's terminal as its program's output, from a
// thread that stands in for the PTY reader. The terminal's callbacks then
// run off the GUI thread, and each delivery crosses to the GUI thread as it
// does in use, which a feed on the GUI thread would not show.
void feedFromReader(Pane* pane, std::string_view bytes) {
    std::thread reader([pane, bytes] { pane->withTerminal([bytes](AfterglowVt::Terminal& t) { t.feed(bytes); }); });
    reader.join();
}
// A program's request to read the clipboard (OSC 52).
void requestClipboardRead(Pane* pane) { feedFromReader(pane, "\x1b]52;c;?\x07"); }
// The text on @p pane's screen, without its history.
QString screenOf(Pane* pane) { return pane->text(AfterglowVt::Terminal::TextScope::Screen); }
// Runs the event loop until every call posted before this one has run:
// queued calls and zero timers run in the order they were posted. A call
// that one of them posts in turn runs in the drain that follows.
bool settleQueued() {
    // The timer goes with its context when the wait gives up before it ran,
    // so that it never writes to this frame after the return.
    QObject context;
    bool ran = false;
    QTimer::singleShot(0, &context, [&ran] { ran = true; });
    return waitFor([&ran] { return ran; });
}
// Waits until @p window renders no frame for 300 ms, and fails when it
// still renders after @p deadlineMs. Springs advance at most 100 ms of
// motion per frame, so a loaded machine that gives few frames stretches
// them in wall time. The deadline allows for that; an animation that never
// stops still fails. No wait for a first frame comes before: a coast asks
// for frames from the event that starts it.
void waitIdle(TerminalWindow* window, int deadlineMs = 10000) {
    QElapsedTimer deadline;
    deadline.start();
    for (;;) {
        const auto frames = window->submittedFrames();
        drain(300);
        if (window->submittedFrames() == frames) return;
        if (deadline.elapsed() > deadlineMs) {
            ADD_FAILURE() << "the window still renders with nothing moving";
            return;
        }
    }
}
// Expects the window to stop asking for frames once nothing moves, after a
// change whose motion can start some frames later: 1.5 s pass first, so
// that a quiet spell before the motion starts is not taken for its end.
void expectIdle(TerminalWindow* window) {
    drain(1500);
    waitIdle(window, 5000);
}
// Renders a frame and waits until it is submitted. A check that nothing
// changed then sees the layout and the chrome of a frame after the change.
bool nextFrame(TerminalWindow* window) {
    const auto frames = window->submittedFrames();
    QEvent update(QEvent::UpdateRequest);
    QCoreApplication::sendEvent(window, &update);
    return waitFor([&] { return window->submittedFrames() > frames; });
}
// QML errors print a warning and leave the test green. The handler keeps the
// warnings of a QML file that report a TypeError, a ReferenceError or an
// assignment that failed ("Cannot assign", "Unable to assign"), and every
// test fails that produced one. Other QML warnings pass.
std::mutex qmlErrorsMutex;
QStringList qmlErrors;
QtMessageHandler previousHandler = nullptr;
void recordQmlErrors(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const bool fromQml = message.contains(QLatin1String(".qml:"))
        || (context.file && QByteArray(context.file).endsWith(".qml"));
    if (type == QtWarningMsg && fromQml
        && (message.contains(QLatin1String("TypeError")) || message.contains(QLatin1String("ReferenceError"))
            || message.contains(QLatin1String("Cannot assign")) || message.contains(QLatin1String("Unable to assign")))) {
        std::lock_guard lock(qmlErrorsMutex);
        qmlErrors.append(message);
    }
    if (previousHandler) previousHandler(type, context, message);
}
QStringList takeQmlErrors() {
    std::lock_guard lock(qmlErrorsMutex);
    return std::exchange(qmlErrors, {});
}
// Fails the running test when it produced a QML error. Each fixture calls it
// in its TearDown, so that the next test's SetUp does not drop the errors.
void expectNoQmlErrors() {
    const auto errors = takeQmlErrors();
    EXPECT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
}
// The drag and drop tests need real compositor input. A test of a client's
// drag needs that client too: kitten, or the aglow of the build.
#define REQUIRE_NATIVE_DND(ready)                                                                                 \
    const QString ready = qEnvironmentVariable("AFTERGLOW_NATIVE_DND_READY");                                    \
    if (ready.isEmpty()) GTEST_SKIP() << "Run through native_window_acceptance.py for real compositor input"
#define REQUIRE_KITTEN()                                                                                          \
    if (QStandardPaths::findExecutable(QStringLiteral("kitten")).isEmpty()) GTEST_SKIP() << "kitten is not installed"
#ifdef AFTERGLOW_AGLOW
#define AGLOW_BINARY QStringLiteral(AFTERGLOW_AGLOW)
#else
#define AGLOW_BINARY QString()
#endif
#define REQUIRE_AGLOW(aglow)                                                                                      \
    const QString aglow = AGLOW_BINARY;                                                                           \
    if (aglow.isEmpty()) GTEST_SKIP() << "aglow is not built (AFTERGLOW_BUILD_AGLOW=OFF)"
QList<QQuickItem*> visualDescendants(QQuickItem* root) {
    QList<QQuickItem*> result;
    for (auto* child : root->childItems()) {
        result.append(child);
        result.append(visualDescendants(child));
    }
    return result;
}
// A chrome panel takes the keyboard one event-loop turn after it shows. The
// chrome is made in the background and finished by the first frame that
// needs it, if the background has not finished it yet. The chrome root holds
// the keys after a panel closed, until the next one takes them.
bool chromeFocusReady(ChromeLayer* layer) {
    auto* focus = layer->focusedItem();
    return focus && focus->hasActiveFocus() && focus->isVisible() && focus != focus->window()->contentItem()
        && focus != layer->rootItem();
}
// Mouse input at @p at, window logical pixels, as the compositor sends it.
void mouseAt(QWindow* window, QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons held) {
    QMouseEvent event(type, at, window->mapToGlobal(at), button, held, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &event);
}
void pressLeft(QWindow* window, const QPointF& at) { mouseAt(window, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton); }
void dragLeft(QWindow* window, const QPointF& at) { mouseAt(window, QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton); }
void releaseLeft(QWindow* window, const QPointF& at) { mouseAt(window, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton); }
void hoverAt(QWindow* window, const QPointF& at) { mouseAt(window, QEvent::MouseMove, at, Qt::NoButton, Qt::NoButton); }
void clickLeft(QWindow* window, const QPointF& at) {
    pressLeft(window, at);
    releaseLeft(window, at);
}
// Where @p pane's grid is drawn, in @p window's logical pixels: a popup's
// own cells for an overlay's pane.
QRectF gridOf(TerminalWindow* window, Pane* pane) {
    const QRect global = window->accessiblePaneRect(pane);
    return QRectF(window->mapFromGlobal(global.topLeft()), global.size());
}
void clickRight(QWindow* window, const QPointF& at) {
    mouseAt(window, QEvent::MouseButtonPress, at, Qt::RightButton, Qt::RightButton);
    mouseAt(window, QEvent::MouseButtonRelease, at, Qt::RightButton, Qt::NoButton);
}
// A run of a dock's line: a click on it reports @p item, when it has one.
QVariant dockRun(const QString& text, const QString& item = {}) {
    QVariantMap run{{QStringLiteral("text"), text}};
    if (!item.isEmpty()) run.insert(QStringLiteral("item"), item);
    return run;
}
// A checked dock effect, as ScriptHost passes it on: one line of @p runs.
QVariantMap dockEffect(const QString& id, const QString& edge, int size, const QVariantList& runs,
                       const QString& scope = QStringLiteral("window")) {
    QVariantList lines;
    lines.append(QVariant(runs));
    return {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), id}, {QStringLiteral("scope"), scope},
            {QStringLiteral("edge"), edge}, {QStringLiteral("size"), size}, {QStringLiteral("lines"), lines}};
}
QVariantMap undockEffect(const QString& id, const QString& scope = QStringLiteral("window")) {
    return {{QStringLiteral("kind"), QStringLiteral("undock")}, {QStringLiteral("id"), id}, {QStringLiteral("scope"), scope}};
}
// A dock of @p window that shows: a window's dock, or pane @p pane's.
const DockArea* shownDock(const TerminalWindow* window, const char* id, quint32 pane = 0) {
    for (const DockArea* dock : window->accessibleDocks())
        if (dock->id() == QLatin1String(id) && dock->pane() == pane) return dock;
    return nullptr;
}
const DockArea* paneDock(const Pane* pane, const char* id) {
    for (const auto& dock : pane->docks())
        if (dock->id() == QLatin1String(id)) return dock.get();
    return nullptr;
}
void sendKey(TerminalWindow* window, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
             const QString& text = {}, bool autoRepeat = false) {
    QKeyEvent down(QEvent::KeyPress, key, modifiers, text, autoRepeat);
    QCoreApplication::sendEvent(window, &down);
    QKeyEvent up(QEvent::KeyRelease, key, modifiers, text, autoRepeat);
    QCoreApplication::sendEvent(window, &up);
}
// As sendKey, then runs the calls that the key queued. False when they did
// not run.
[[nodiscard]] bool pressKey(TerminalWindow* window, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                            const QString& text = {}) {
    sendKey(window, key, modifiers, text);
    return settleQueued();
}
// The keys of one drag and drop packet: ESC ] 72 ; keys ; payload. A payload
// can hold any of the keys' text.
QList<QByteArray> dndKeys(const QByteArray& packet) {
    return packet.split(';').value(1).split(':');
}
class NotificationServer : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    int count = 0, closes = 0;
    uint next = 0;
    QList<QSize> images;
    QString appName, appIcon, desktopEntry;
    QStringList titles, bodies;
public Q_SLOTS:
    uint Notify(const QString& name, uint replaces, const QString& icon, const QString& title, const QString& body,
                const QStringList&, const QVariantMap& hints, int) {
        ++count;
        titles.append(title);
        bodies.append(body);
        appName = name;
        appIcon = icon;
        desktopEntry = hints.value(QStringLiteral("desktop-entry")).toString();
        QSize size;
        if (hints.contains(QStringLiteral("image-data"))) {
            const auto arg = hints.value(QStringLiteral("image-data")).value<QDBusArgument>();
            int width = 0, height = 0, stride = 0, bits = 0, channels = 0; bool alpha = false; QByteArray bytes;
            arg.beginStructure(); arg >> width >> height >> stride >> alpha >> bits >> channels >> bytes; arg.endStructure();
            size = {width, height};
        }
        images.append(size);
        return replaces ? replaces : ++next;
    }
    void CloseNotification(uint) { ++closes; }
};
// A NotificationServer on the session bus, as the desktop's own would be.
class RegisteredNotificationServer {
public:
    NotificationServer server;
    bool registered = false;
    RegisteredNotificationServer() {
        auto bus = QDBusConnection::sessionBus();
        registered = bus.registerService(QStringLiteral("org.freedesktop.Notifications"))
            && bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &server, QDBusConnection::ExportAllSlots);
    }
    ~RegisteredNotificationServer() {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QStringLiteral("/org/freedesktop/Notifications"));
        bus.unregisterService(QStringLiteral("org.freedesktop.Notifications"));
    }
};
// The dock events that windows send one script, in order.
struct DockEvent {
    QString handler;
    quint32 pane = 0;
    QVariantMap event;
    bool limited = false;
};
class DockEvents {
public:
    explicit DockEvents(TerminalWindow* window, QString owner = QStringLiteral("a@1")) : m_owner(std::move(owner)) { watch(window); }
    void watch(TerminalWindow* window) {
        QObject::connect(window, &TerminalWindow::scriptDockEvent, &m_context,
                         [this](const QString& owner, const QString& handler, quint32 pane, const QVariantMap& event, bool limited) {
                             if (owner == m_owner) all.append({handler, pane, event, limited});
                         });
    }
    QList<DockEvent> of(const char* handler) const {
        QList<DockEvent> found;
        for (const DockEvent& e : all) if (e.handler == QLatin1String(handler)) found.append(e);
        return found;
    }
    // The last event for @p handler, of dock @p id when one is given; an
    // empty one when none came.
    DockEvent last(const char* handler, const char* id = nullptr) const {
        DockEvent found;
        for (const DockEvent& e : all)
            if (e.handler == QLatin1String(handler) && (!id || e.event.value(QStringLiteral("dock")) == QLatin1String(id))) found = e;
        return found;
    }
    QList<DockEvent> all;
private:
    QString m_owner;
    QObject m_context;
};
// The settings of a test's Application, with @p overrides over them by
// group. No cursor blink, so a window with nothing moving renders no frames.
// No scripts, so that the scripts installed for the user do not run in the
// tests: a test that needs scripts turns them on. A pane with no command
// runs /bin/sh, not the developer's login shell with its startup files.
QJsonObject testSettings(const QJsonObject& overrides = {}) {
    QJsonObject settings{{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("cursorBlink"), false},
                                                                  {QStringLiteral("shell"), QStringLiteral("/bin/sh")}}},
                         {QStringLiteral("Scripts"), QJsonObject{{QStringLiteral("enabled"), false}}}};
    for (auto group = overrides.begin(); group != overrides.end(); ++group) {
        QJsonObject merged = settings.value(group.key()).toObject();
        const QJsonObject values = group.value().toObject();
        for (auto value = values.begin(); value != values.end(); ++value) merged.insert(value.key(), value.value());
        settings.insert(group.key(), merged);
    }
    return settings;
}
// Two client panes side by side, and what each side of the protocol sent
// through a drag from the left one to the right one.
struct ClientDrag {
    Pane* source = nullptr;
    Pane* receiver = nullptr;
    bool complete = false;
    // The keys of the terminal's reply that ended the drag (t=e, x=4).
    QList<QByteArray> end;
    // The terminal's requests for the tree of a remote source.
    int treeRequests = 0;
    QByteArray protocol; // the source's side
    QByteArray dropped;  // the receiver's side
    // Made after the members that its connections write to, so that the
    // connections end before them: the window outlives the test body.
    QObject listener;
};
class NativeWindow : public ::testing::Test {
protected:
    void openDragClients(const QStringList& sourceCommand, const QStringList& receiverCommand, ClientDrag& drag);
    void runClientDrag(const QString& ready, ClientDrag& drag, const std::function<bool()>& accepted = {});
    void dragAndDropClient(const QString& client, const QString& ready);
    void closeOverviewOntoTheFirstTab(Pane*& first, Pane*& second);
    void remoteDragDirectory(const QString& client, const QString& ready, bool move = false);
    void remoteDragToWaylandTarget(const QString& client, const QString& ready, Qt::DropAction action, bool refuse = false);
    void overlaySwitchesAtOnce(const QString& transition);
    QTemporaryDir directory;
    std::unique_ptr<Application> application;
    TerminalWindow* window = nullptr;
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        takeQmlErrors();
        application = std::make_unique<Application>(testSocket(), directory.filePath(QStringLiteral("afterglow.conf")),
            testSettings({{QStringLiteral("Window"), QJsonObject{{QStringLiteral("paneTitles"), true}}}}));
        openWindow({QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    }
    // Replaces the application with one that reads @p config beside the
    // fixture's, with @p settings, and opens its window on @p command.
    void restartApplication(const QString& config, const QJsonObject& settings, const QStringList& command) {
        application.reset();
        window = nullptr;
        // What the old application left with deleteLater goes before the
        // new one starts, as between two tests.
        drainDeferredDeletes();
        application = std::make_unique<Application>(testSocket(), directory.filePath(config), settings);
        openWindow(command);
    }
    // The fixture's settings with Mouse.dragSelection on, and @p overrides
    // over them by group, in a new application whose window runs @p command.
    void restartWithDragSelection(QJsonObject overrides = {},
                                  const QStringList& command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                                                QStringLiteral("stty -echo; cat")}) {
        QJsonObject mouse = overrides.value(QStringLiteral("Mouse")).toObject();
        mouse.insert(QStringLiteral("dragSelection"), true);
        overrides.insert(QStringLiteral("Mouse"), mouse);
        QJsonObject windowSettings = overrides.value(QStringLiteral("Window")).toObject();
        windowSettings.insert(QStringLiteral("paneTitles"), true);
        overrides.insert(QStringLiteral("Window"), windowSettings);
        restartApplication(QStringLiteral("drag.conf"), testSettings(overrides), command);
    }
    void openWindow(const QStringList& command) {
        WindowOptions options;
        options.command = command;
        window = application->newWindow(options);
        ASSERT_NE(window, nullptr);
        window->resize(900, 600);
        window->show();
        ASSERT_TRUE(waitFor([this] { return window->isExposed() && window->submittedFrames() > 0; }));
    }
    void TearDown() override {
        application.reset();
        drainDeferredDeletes();
        expectNoQmlErrors();
    }
};
TEST_F(NativeWindow, TabDragOutPreservesLiveSplitAndMenuTargetsBackgroundTab) {
    auto* original = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* split = window->splitPane(original, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(split, nullptr);
    const auto tabId = window->tabIdOf(original);
    auto* other = window->openTab(launch, nullptr);
    ASSERT_NE(other, nullptr);
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(waitFor([&] { return chrome->tabBarHeight() > 0; }));
    // The tab bar takes the drag only once the chrome scene, made in the
    // background, exists and has drawn the two tabs. Before that the first
    // drag below would pass for having detached nothing.
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(waitFor([&] { return layer->rootItem() != nullptr; }));
    ASSERT_TRUE(nextFrame(window));
    const QPointF start(50, chrome->tabBarHeight()/2.0), outside(50, 150);
    pressLeft(window, start);
    dragLeft(window, outside);
    dragLeft(window, start);
    releaseLeft(window, start);
    // A detach would run from a call that the release queued.
    ASSERT_TRUE(settleQueued());
    ASSERT_EQ(original->parent(), window);
    ASSERT_EQ(window->tabs().size(), 2);
    pressLeft(window, start);
    dragLeft(window, outside);
    releaseLeft(window, outside);
    ASSERT_TRUE(waitFor([&] { return original->parent() != window; }));
    auto* detached = qobject_cast<TerminalWindow*>(original->parent());
    ASSERT_NE(detached, nullptr);
    EXPECT_EQ(split->parent(), detached);
    EXPECT_EQ(detached->tabIdOf(original), tabId);
    EXPECT_EQ(window->panes(), QList<Pane*>{other});
    // The pane runs cat with no echo: only the program's output shows.
    ASSERT_TRUE(original->sendInput("detached-process-alive\n"));
    EXPECT_TRUE(waitFor([&] {
        return screenOf(original).contains(QStringLiteral("detached-process-alive"));
    }));
    auto* background = window->openTab(launch, nullptr);
    ASSERT_NE(background, nullptr);
    window->revealPane(other);
    chrome->tabMenu(1, 50, 15);
    const auto items = chrome->contextMenu().value(QStringLiteral("items")).toList();
    int detachId = -1;
    for (const auto& item : items) {
        const auto map = item.toMap();
        if (map.value(QStringLiteral("title")).toString() == QStringLiteral("Detach Tab to New Window"))
            detachId = map.value(QStringLiteral("id")).toInt();
    }
    ASSERT_GE(detachId, 0);
    chrome->activateMenuItem(detachId);
    EXPECT_TRUE(waitFor([&] { return background->parent() != window; }));
    EXPECT_EQ(other->parent(), window);

    chrome = nullptr;
    restartApplication(QStringLiteral("single-tab.conf"),
        testSettings({{QStringLiteral("Window"), QJsonObject{{QStringLiteral("tabBar"), QStringLiteral("always")}}}}),
        {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(waitFor([&] { return chrome->tabBarHeight() > 0; }));
    // The new window's tab bar takes the drag only once its own chrome scene
    // exists and has drawn the tab, as for the first window above.
    layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(waitFor([&] { return layer->rootItem() != nullptr; }));
    ASSERT_TRUE(nextFrame(window));
    QPointer<Pane> lastPane = window->focusedPane();
    QPointer<TerminalWindow> source = window;
    pressLeft(window, start);
    dragLeft(window, QPointF(-50, 150));
    releaseLeft(window, QPointF(-50, 150));
    // The window that the last tab left goes with deleteLater, which only
    // a running event loop or an explicit send delivers.
    EXPECT_TRUE(waitDeleted(source));
    window = source.data();
    ASSERT_FALSE(lastPane.isNull());
    ASSERT_NE(qobject_cast<TerminalWindow*>(lastPane->parent()), nullptr);
    EXPECT_FALSE(lastPane->isHeld());
}

class NativeRenderer : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override { takeQmlErrors(); }
    // The test's Application is gone by now: drain its deferred deletes.
    void TearDown() override {
        drainDeferredDeletes();
        expectNoQmlErrors();
    }
};
TEST_P(NativeRenderer, SurvivesWindowTransferResizeAndSourceDestruction) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    Application application(testSocket(), directory.filePath(QStringLiteral("mesh.conf")),
        testSettings({{QStringLiteral("Shaders"), QJsonObject{{QStringLiteral("background"), GetParam() ? QJsonArray{QStringLiteral("mesh-reactor")} : QJsonArray{}}}},
                      {QStringLiteral("Audio"), QJsonObject{{QStringLiteral("enabled"), false}}},
                      {QStringLiteral("Window"), QJsonObject{{QStringLiteral("backgroundOpacity"), 0.9}}}}));
    WindowOptions options;
    options.command = {QStringLiteral("/bin/cat")};
    QPointer<TerminalWindow> source = application.newWindow(options);
    ASSERT_TRUE(source);
    source->resize(400, 300);
    auto* survivor = application.newWindow(options);
    ASSERT_TRUE(survivor);
    survivor->resize(400, 300);
    ASSERT_TRUE(waitFor([&] { return source->submittedFrames() > 0 && survivor->submittedFrames() > 0; }));
    QPointer<Pane> pane = source->focusedPane();
    ASSERT_TRUE(pane);
    Q_EMIT source->transferRequested(pane, true);
    ASSERT_TRUE(waitDeleted(source));
    ASSERT_TRUE(pane);
    auto* destination = qobject_cast<TerminalWindow*>(pane->parent());
    ASSERT_TRUE(destination);
    ASSERT_TRUE(waitFor([&] { return destination->submittedFrames() > 0; }));
    EXPECT_TRUE(destination->isExposed());
    EXPECT_TRUE(survivor->isExposed());
    for (int i = 0; i < 8; ++i) {
        destination->resize(i % 2 ? QSize(500, 350) : QSize(300, 200));
        QEvent update(QEvent::UpdateRequest);
        QCoreApplication::sendEvent(destination, &update);
        QCoreApplication::sendEvent(survivor, &update);
        drain(50);
    }
    // Both windows still render after the resizes: a frame that each one
    // is asked for now is submitted. A frame of the loop would not count.
    EXPECT_TRUE(nextFrame(destination));
    EXPECT_TRUE(nextFrame(survivor));
}

INSTANTIATE_TEST_SUITE_P(Shaders, NativeRenderer, ::testing::Bool());

// A screenshot, as Control::capture answers it.
struct Shot {
    bool done = false;
    QString error, code;
    QImage image;
};
// Asks @p control for a screenshot of @p pane, and waits for the answer.
Shot captureShot(Control& control, Pane* pane, CaptureScope scope) {
    auto shot = std::make_shared<Shot>();
    QString code;
    const QString error = control.capture(pane->id(), scope,
        [shot](const QString& e, const QString& c, const QImage& image) { *shot = Shot{true, e, c, image}; }, code);
    if (!error.isEmpty()) return Shot{true, error, code, {}};
    // Longer than a default tab switch and the window's bound on a frame
    // together: an answer always comes.
    EXPECT_TRUE(waitFor([shot] { return shot->done; }));
    return *shot;
}
// Mostly red, green or blue, whatever a pane effect does to the shade.
bool reddish(QRgb c) { return qRed(c) > 120 && qRed(c) > 2 * qGreen(c) && qRed(c) > 2 * qBlue(c); }
bool greenish(QRgb c) { return qGreen(c) > 120 && qGreen(c) > 2 * qRed(c) && qGreen(c) > 2 * qBlue(c); }
bool bluish(QRgb c) { return qBlue(c) > 120 && qBlue(c) > 2 * qRed(c) && qBlue(c) > 2 * qGreen(c); }
// The pixel at the fractions @p x and @p y of @p image.
QRgb pixelAt(const QImage& image, double x, double y) {
    return image.pixel(int(x * image.width()), int(y * image.height()));
}

TEST_F(NativeWindow, ScreenshotsShowThePaneItsTabAndItsWindow) {
    SettingsStore settings(directory.filePath(QStringLiteral("capture.conf")),
                           QJsonObject{{QStringLiteral("Control"), QJsonObject{{QStringLiteral("allowReadText"), true}}}});
    Control control(settings, {});
    control.attachWindow(window);
    // Two panes side by side, each filled with its own background colour.
    Pane* left = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")};
    Pane* right = window->splitPane(left, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(right, nullptr);
    expectIdle(window);
    left->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?25l\x1b[48;2;220;20;20m\x1b[2J"); });
    right->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?25l\x1b[48;2;20;200;20m\x1b[2J"); });
    ASSERT_TRUE(nextFrame(window));
    // From an idle window: the capture asks for its own frame.
    expectIdle(window);
    const QSize windowPixels = window->framePixels();
    ASSERT_FALSE(windowPixels.isEmpty());

    const Shot whole = captureShot(control, left, CaptureScope::Window);
    ASSERT_TRUE(whole.error.isEmpty()) << qPrintable(whole.error);
    EXPECT_TRUE(whole.code.isEmpty());
    EXPECT_EQ(whole.image.size(), windowPixels);
    EXPECT_TRUE(reddish(pixelAt(whole.image, 0.25, 0.5)));
    EXPECT_TRUE(greenish(pixelAt(whole.image, 0.75, 0.5)));
    const Shot tab = captureShot(control, right, CaptureScope::Tab);
    ASSERT_TRUE(tab.error.isEmpty()) << qPrintable(tab.error);
    EXPECT_TRUE(tab.code.isEmpty());
    // One tab: no tab bar (Window.tabBar auto), so the tab is the window.
    EXPECT_EQ(tab.image.size(), windowPixels);
    EXPECT_TRUE(reddish(pixelAt(tab.image, 0.25, 0.5)));
    EXPECT_TRUE(greenish(pixelAt(tab.image, 0.75, 0.5)));
    const Shot leftShot = captureShot(control, left, CaptureScope::Pane);
    const Shot rightShot = captureShot(control, right, CaptureScope::Pane);
    ASSERT_TRUE(leftShot.error.isEmpty()) << qPrintable(leftShot.error);
    ASSERT_TRUE(rightShot.error.isEmpty()) << qPrintable(rightShot.error);
    EXPECT_TRUE(leftShot.code.isEmpty());
    EXPECT_TRUE(rightShot.code.isEmpty());
    // Each pane as it shows, with its padding: the two side by side fill
    // the tab, less the divider between them.
    EXPECT_LT(leftShot.image.width(), tab.image.width() * 3 / 4);
    EXPECT_LT(rightShot.image.width(), tab.image.width() * 3 / 4);
    EXPECT_GE(leftShot.image.width() + rightShot.image.width(), tab.image.width() - 16);
    EXPECT_LE(leftShot.image.width() + rightShot.image.width(), tab.image.width());
    // The cells: only red in the left pane, only green in the right one.
    for (const double at : {0.25, 0.5, 0.75}) {
        EXPECT_TRUE(reddish(pixelAt(leftShot.image, at, at))) << at;
        EXPECT_TRUE(greenish(pixelAt(rightShot.image, at, at))) << at;
    }
    // A capture lets the window go idle again.
    expectIdle(window);

    // A pane that a split makes is cut once it has grown to its place.
    Pane* below = window->splitPane(right, SplitTree::Direction::Down, launch, nullptr);
    ASSERT_NE(below, nullptr);
    // Blue once its terminal has the size of its place, which the first
    // frame gives it. The pane then still grows: at the top of its place,
    // a frame of the way shows the green pane above.
    ASSERT_TRUE(nextFrame(window));
    below->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?25l\x1b[48;2;20;20;220m\x1b[2J"); });
    const Shot fresh = captureShot(control, below, CaptureScope::Pane);
    ASSERT_TRUE(fresh.error.isEmpty()) << qPrintable(fresh.error);
    EXPECT_EQ(fresh.image.width(), rightShot.image.width());
    EXPECT_LT(fresh.image.height(), rightShot.image.height() * 3 / 4);
    EXPECT_TRUE(bluish(pixelAt(fresh.image, 0.5, 0.1)));
    EXPECT_TRUE(bluish(pixelAt(fresh.image, 0.5, 0.5)));
    // A pane that closes before its frame has no picture: not the window.
    auto closing = std::make_shared<Shot>();
    QString code;
    ASSERT_TRUE(control.capture(below->id(), CaptureScope::Pane,
        [closing](const QString& e, const QString& c, const QImage& image) { *closing = Shot{true, e, c, image}; }, code).isEmpty());
    window->requestClosePane(below);
    ASSERT_TRUE(waitFor([closing] { return closing->done; }));
    EXPECT_EQ(closing->code, QStringLiteral("not_shown"));
    EXPECT_TRUE(closing->image.isNull());
    // The right pane grows back. A zoom that hides it on the way leaves it
    // a motion that never advances: its picture is not_shown, not a wait.
    ASSERT_TRUE(window->paneMoving(right));
    ASSERT_TRUE(window->runAction(left, Action::TogglePaneZoom));
    EXPECT_EQ(captureShot(control, right, CaptureScope::Pane).code, QStringLiteral("not_shown"));
    ASSERT_TRUE(window->runAction(left, Action::TogglePaneZoom));
    expectIdle(window);

    // A zoom hides the other pane: it has no picture, but its tab has, which
    // shows the zoomed pane.
    ASSERT_TRUE(window->runAction(left, Action::TogglePaneZoom));
    expectIdle(window);
    // Red again at the new size: the columns that the zoom added are blank.
    left->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[48;2;220;20;20m\x1b[2J"); });
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(captureShot(control, right, CaptureScope::Pane).code, QStringLiteral("not_shown"));
    const Shot zoomed = captureShot(control, right, CaptureScope::Tab);
    ASSERT_TRUE(zoomed.error.isEmpty()) << qPrintable(zoomed.error);
    EXPECT_TRUE(reddish(pixelAt(zoomed.image, 0.75, 0.5)));
    ASSERT_TRUE(window->runAction(left, Action::TogglePaneZoom));
    expectIdle(window);
    // The zoom's end reflows the red rows: red again at the old size.
    left->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[48;2;220;20;20m\x1b[2J"); });
    ASSERT_TRUE(nextFrame(window));

    // A second tab shows the tab bar, which only the window has.
    Pane* other = window->openTab(launch, nullptr);
    ASSERT_NE(other, nullptr);
    other->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?25l\x1b[48;2;20;20;220m\x1b[2J"); });
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(waitFor([&] { return chrome->tabBarHeight() > 0 && !window->tabSwitching(); }));
    expectIdle(window);
    const int bar = int(std::lround(chrome->tabBarHeight() * window->devicePixelRatio()));
    const Shot shown = captureShot(control, left, CaptureScope::Window);
    ASSERT_TRUE(shown.error.isEmpty()) << qPrintable(shown.error);
    // The window shows its tab, which is not the tab of the pane.
    EXPECT_EQ(shown.image.size(), windowPixels);
    EXPECT_TRUE(bluish(shown.image.pixel(shown.image.width() / 2, bar + (shown.image.height() - bar) / 2)));
    const Shot otherTab = captureShot(control, other, CaptureScope::Tab);
    ASSERT_TRUE(otherTab.error.isEmpty()) << qPrintable(otherTab.error);
    EXPECT_EQ(otherTab.image.size(), QSize(windowPixels.width(), windowPixels.height() - bar));
    EXPECT_TRUE(bluish(pixelAt(otherTab.image, 0.5, 0.5)));
    // A pane in a tab in the background has no picture, and its tab neither.
    EXPECT_EQ(captureShot(control, left, CaptureScope::Pane).code, QStringLiteral("not_shown"));
    EXPECT_EQ(captureShot(control, left, CaptureScope::Tab).code, QStringLiteral("not_shown"));

    // During a tab switch the pane waits for the end of the switch.
    window->revealPane(left);
    ASSERT_TRUE(window->tabSwitching());
    const Shot afterSwitch = captureShot(control, left, CaptureScope::Pane);
    ASSERT_TRUE(afterSwitch.error.isEmpty()) << qPrintable(afterSwitch.error);
    EXPECT_FALSE(window->tabSwitching());
    EXPECT_TRUE(reddish(pixelAt(afterSwitch.image, 0.5, 0.5)));
    expectIdle(window);

    // The overview draws cards: a pane or a tab has no picture, the window has.
    ASSERT_TRUE(window->openOverview());
    EXPECT_EQ(captureShot(control, left, CaptureScope::Pane).code, QStringLiteral("not_shown"));
    EXPECT_EQ(captureShot(control, left, CaptureScope::Tab).code, QStringLiteral("not_shown"));
    EXPECT_TRUE(captureShot(control, left, CaptureScope::Window).error.isEmpty());

    // Without Control.allowReadText, a refusal at once, and no answer later.
    SettingsStore closed(directory.filePath(QStringLiteral("closed.conf")),
                         QJsonObject{{QStringLiteral("Control"), QJsonObject{{QStringLiteral("allowReadText"), false}}}});
    Control denied(closed, {});
    denied.attachWindow(window);
    bool answered = false;
    EXPECT_FALSE(denied.capture(left->id(), CaptureScope::Window,
        [&answered](const QString&, const QString&, const QImage&) { answered = true; }, code).isEmpty());
    EXPECT_EQ(code, QStringLiteral("denied"));
    ASSERT_TRUE(nextFrame(window));
    EXPECT_FALSE(answered);

    // A window that leaves the screen before its frame answers not_shown,
    // and one off screen answers at once.
    auto hiding = std::make_shared<Shot>();
    ASSERT_TRUE(control.capture(left->id(), CaptureScope::Window,
        [hiding](const QString& e, const QString& c, const QImage& image) { *hiding = Shot{true, e, c, image}; }, code).isEmpty());
    ASSERT_FALSE(hiding->done) << "the capture waits for its frame";
    window->hide();
    ASSERT_TRUE(waitFor([hiding] { return hiding->done; }));
    EXPECT_EQ(hiding->code, QStringLiteral("not_shown"));
    auto hidden = std::make_shared<Shot>();
    ASSERT_TRUE(control.capture(left->id(), CaptureScope::Window,
        [hidden](const QString& e, const QString& c, const QImage& image) { *hidden = Shot{true, e, c, image}; }, code).isEmpty());
    EXPECT_TRUE(hidden->done) << "answered at once";
    EXPECT_EQ(hidden->code, QStringLiteral("not_shown"));

    // Shown again (a new toplevel): one that is minimized before its frame
    // answers not_shown too.
    window->show();
    ASSERT_TRUE(waitFor([&] { return window->isExposed(); }));
    ASSERT_TRUE(nextFrame(window));
    auto minimizing = std::make_shared<Shot>();
    ASSERT_TRUE(control.capture(left->id(), CaptureScope::Window,
        [minimizing](const QString& e, const QString& c, const QImage& image) { *minimizing = Shot{true, e, c, image}; }, code).isEmpty());
    ASSERT_FALSE(minimizing->done) << "the capture waits for its frame";
    window->showMinimized();
    ASSERT_TRUE(waitFor([minimizing] { return minimizing->done; }));
    EXPECT_EQ(minimizing->code, QStringLiteral("not_shown"));
}

TEST_F(NativeWindow, FailedLaunchDoesNotAddPaneOrTab) {
    const int panes = window->panes().size(), tabs = window->tabs().size();
    PaneLaunchOptions options; options.command = {QStringLiteral("/nonexistent/afterglow-test-program")};
    QString tabError;
    EXPECT_EQ(window->openTab(options, &tabError), nullptr);
    EXPECT_FALSE(tabError.isEmpty());
    EXPECT_EQ(window->tabs().size(), tabs);
    QString splitError;
    EXPECT_EQ(window->splitPane(window->focusedPane(), SplitTree::Direction::Left, options, &splitError), nullptr);
    EXPECT_FALSE(splitError.isEmpty());
    EXPECT_EQ(window->panes().size(), panes);
}
TEST_F(NativeWindow, LaunchOptionsAndClosedIdentity) {
    SettingsStore settings(directory.filePath(QStringLiteral("control.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    auto* original = window->focusedPane();
    quint32 created = 0;
    // A launch cannot give a pane another pane's identity or socket.
    const QVariantMap env{{QStringLiteral("AFTERGLOW_LAUNCH_TEST"), QStringLiteral("native-options")},
                          {QStringLiteral("AFTERGLOW_REMOVE_TEST"), false},
                          {QStringLiteral("AFTERGLOW_PANE_ID"), QStringLiteral("1")},
                          {QStringLiteral("AFTERGLOW_SOCKET"), QStringLiteral("/launch")}};
    const QVariantMap options{{QStringLiteral("cwd"), directory.path()}, {QStringLiteral("env"), env},
                              {QStringLiteral("size"), .25}};
    QString error;
    {
        const Saved removed("AFTERGLOW_REMOVE_TEST", "inherited");
        // These tests listen on no socket: an inherited one must not reach the
        // pane, where afterglow ctl, afterglow run and aglow run would act on the
        // Afterglow that it names.
        const Saved socket("AFTERGLOW_SOCKET", "/inherited");
        const QStringList command{QStringLiteral("/bin/sh"), QStringLiteral("-c"),
            QStringLiteral("printf '%s|%s|%s|%s\\n' \"$AFTERGLOW_LAUNCH_TEST\" \"${AFTERGLOW_REMOVE_TEST-unset}\" "
                           "\"${AFTERGLOW_SOCKET-unset}\" \"$AFTERGLOW_PANE_ID\"; exec cat")};
        error = control.split(original->id(), QStringLiteral("left"), command, options, created);
    }
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_NE(created, 0u);
    auto* added = window->focusedPane();
    EXPECT_EQ(added->id(), created);
    const QString expected = QStringLiteral("native-options|unset|unset|%1").arg(created);
    EXPECT_TRUE(waitFor([&] { return screenOf(added).contains(expected); }))
        << screenOf(added).toStdString();
    EXPECT_EQ(added->workingDirectory(), directory.path());
    // The window lays the split out at its next frame, and the shell can
    // print before that frame.
    ASSERT_TRUE(waitFor([&] { return !window->accessiblePaneRect(added).isEmpty(); }));
    EXPECT_LT(window->accessiblePaneRect(added).width(), window->accessiblePaneRect(original).width() / 2);
    const quint32 tab = window->tabIdOf(added), win = window->id();
    QVariantList closed;
    QObject::connect(&control, &Control::paneClosedDetailed, &control, [&](quint32 p, quint32 t, quint32 w) { closed = {p, t, w}; });
    window->requestClosePane(added);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(closed, (QVariantList{created, tab, win}));
    const int count = window->panes().size();
    EXPECT_FALSE(control.split(original->id(), QStringLiteral("up"), {}, {{QStringLiteral("size"), 1.0}}, created).isEmpty());
    EXPECT_EQ(window->panes().size(), count);
}
TEST_F(NativeWindow, ZoomedPaneTitlesRenderEveryPanePosition) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    first->setUserTitle(QStringLiteral("first title"));
    second->setUserTitle(QStringLiteral("second title"));
    for (auto* pane : {first, second}) {
        window->revealPane(pane);
        ASSERT_TRUE(window->runAction(pane, Action::TogglePaneZoom));
        auto* chrome = window->findChild<ChromeController*>();
        ASSERT_NE(chrome, nullptr);
        // The zoom animates, stretched in wall time on a loaded machine:
        // wait for the label to reach the zoomed size. The zoomed pane fills
        // the window, the label's rectangle with it.
        // The labels change at the next frame: until then they are the last
        // zoom's, which also had one label as wide as the window.
        ASSERT_TRUE(waitFor([&] {
            return chrome->paneTitles().size() == 1
                && chrome->paneTitles().front().toMap().value(QStringLiteral("title")).toString() == pane->title()
                && chrome->paneTitles().front().toMap().value(QStringLiteral("width")).toDouble() > window->width() * 0.9;
        })) << pane->title().toStdString();
        ASSERT_TRUE(window->runAction(pane, Action::TogglePaneZoom));
    }
}
TEST_F(NativeWindow, PasteConfirmationRetainsOriginalPane) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    window->revealPane(first);
    QGuiApplication::clipboard()->setText(QStringLiteral("original-pane-line-one\noriginal-pane-line-two\n"));
    ASSERT_TRUE(window->runAction(first, Action::Paste));
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(chrome->pasteConfirmOpen());
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    // Return pastes only once the panel holds the keyboard.
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    window->revealPane(second);
    QKeyEvent accept(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &accept);
    EXPECT_FALSE(chrome->pasteConfirmOpen());
    EXPECT_TRUE(waitFor([&] {
        return screenOf(first).contains(QStringLiteral("original-pane-line-one"));
    }));
    EXPECT_FALSE(screenOf(second).contains(QStringLiteral("original-pane-line-one")));
}
TEST_F(NativeWindow, PasteConfirmsRawLineTerminatorsAndHonorsBracketedMode) {
    auto* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    for (const auto& text : {QStringLiteral("command\n"), QStringLiteral("command\r"),
                            QStringLiteral("first\rsecond"), QStringLiteral("command\r\n")}) {
        SCOPED_TRACE(text.toStdString());
        QGuiApplication::clipboard()->setText(text);
        const bool asked = window->runAction(pane, Action::Paste);
        EXPECT_TRUE(asked);
        if (!asked) continue;
        EXPECT_TRUE(chrome->pasteConfirmOpen());
        chrome->answerPaste(false);
        EXPECT_FALSE(chrome->pasteConfirmOpen());
    }
    // What the shell gets but the preview cannot show as it is shows as its code point.
    QGuiApplication::clipboard()->setText(QStringLiteral("safe\u202Edm- fr\u2028\u200Bnext\n"));
    ASSERT_TRUE(window->runAction(pane, Action::Paste));
    ASSERT_TRUE(chrome->pasteConfirmOpen());
    EXPECT_TRUE(chrome->pastePreview() == QStringLiteral("safe<U+202E>dm- fr<U+2028><U+200B>next\n")) << chrome->pastePreview().toStdString();
    chrome->answerPaste(false);
    // In bracketed paste mode the text goes at once, between the brackets.
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat -v")};
    Pane* shown = window->splitPane(pane, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(shown, nullptr);
    ASSERT_TRUE(settleQueued());
    shown->withTerminal([](AfterglowVt::Terminal& terminal) { terminal.feed("\x1b[?2004h"); });
    QGuiApplication::clipboard()->setText(QStringLiteral("bracketed\n"));
    ASSERT_TRUE(window->runAction(shown, Action::Paste));
    EXPECT_FALSE(chrome->pasteConfirmOpen());
    EXPECT_TRUE(waitFor([&] {
        return screenOf(shown).contains(QStringLiteral("^[[200~bracketed"));
    })) << screenOf(shown).toStdString();
}

TEST_F(NativeWindow, ChromeTreatsTitlesAndMenuLabelsLiterally) {
    const QString title = QStringLiteral("<b>A&B</b><img src='http://127.0.0.1:9/a'>");
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    ASSERT_NE(window->splitPane(window->focusedPane(), SplitTree::Direction::Right, launch, nullptr), nullptr);
    window->focusedPane()->setUserTitle(title);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    // The pane titles reach the labels at the next frame.
    QList<QQuickItem*> labels;
    ASSERT_TRUE(waitFor([&] {
        if (!layer->rootItem()) return false;
        labels.clear();
        for (auto* item : visualDescendants(layer->rootItem()))
            if (item->property("text").toString() == title && item->property("textFormat").isValid()) labels.append(item);
        return !labels.isEmpty();
    }));
    // What is drawn, not only the property: as markup the title would lay
    // out as a short bold "A&B" and an image, far narrower than its source.
    // A menu style that does not strip the mnemonic "&" would draw the extra
    // "&" of each escaped entity, wider than the source.
    const auto drawsLiterally = [&title](const QQuickItem* item) {
        const QFont font = item->property("font").value<QFont>();
        const qreal advance = QFontMetricsF(font).horizontalAdvance(title);
        return std::abs(item->property("implicitWidth").toDouble() - advance) <= 0.1 * advance;
    };
    for (auto* item : labels) {
        EXPECT_EQ(item->property("textFormat").toInt(), 0); // Text.PlainText
        EXPECT_TRUE(drawsLiterally(item));
    }
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    chrome->openContextMenu({10, 10}, {QVariantMap{{QStringLiteral("id"), 1}, {QStringLiteral("title"), title}, {QStringLiteral("icon"), QStringLiteral("document-open")}}});
    const auto menuRows = [&] {
        QList<QQuickItem*> rows;
        for (auto* item : visualDescendants(layer->rootItem()))
            if (item->property("literalTitle").toString() == title) rows.append(item);
        return rows;
    };
    ASSERT_TRUE(waitFor([&] { return !menuRows().isEmpty(); }));
    for (auto* item : menuRows()) {
        // Each entity carries an extra "&" for the label's mnemonic step.
        QString escaped = title;
        escaped.replace(QLatin1Char('&'), QStringLiteral("&&amp;"))
            .replace(QLatin1Char('<'), QStringLiteral("&&lt;"))
            .replace(QLatin1Char('>'), QStringLiteral("&&gt;"));
        EXPECT_EQ(item->property("text").toString(), QStringLiteral("<span>") + escaped + QStringLiteral("</span>"));
        auto* accessible = QAccessible::queryAccessibleInterface(item);
        EXPECT_NE(accessible, nullptr);
        if (!accessible) continue;
        EXPECT_EQ(accessible->text(QAccessible::Name), title);
        // The style's own text item inside the menu row draws the escaped
        // title at the width of the literal string.
        bool drawn = false;
        for (auto* text : visualDescendants(item)) {
            if (text->property("textFormat").isValid() && !text->property("text").toString().isEmpty())
                drawn = drawn || drawsLiterally(text);
        }
        EXPECT_TRUE(drawn);
    }
}

TEST_F(NativeWindow, FocusUpdatesTabTitleAndRetiresPreviousSearch) {
    auto* first = window->focusedPane();
    first->setUserTitle(QStringLiteral("first"));
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    second->setUserTitle(QStringLiteral("second"));
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    window->revealPane(first);
    EXPECT_EQ(chrome->tabTitles().front(), QStringLiteral("first"));
    // Finish split layout before capturing a search epoch.
    ASSERT_TRUE(waitFor([&] { return !window->accessiblePaneRect(second).isEmpty(); }));
    first->withTerminal([](AfterglowVt::Terminal& terminal) { terminal.feed("\x1b[2J\x1b[Hneedle"); });
    first->refreshSnapshot();
    const auto original = first->snapshot().at(0, 0).bg;
    chrome->setSearchOpen(true);
    chrome->search(QStringLiteral("needle"), 0);
    ASSERT_TRUE(waitFor([&] { return chrome->searchStatus() == QStringLiteral("1 of 1"); })) << chrome->searchStatus().toStdString();
    first->refreshSnapshot();
    EXPECT_NE(first->snapshot().at(0, 0).bg, original);
    window->revealPane(second);
    EXPECT_EQ(chrome->tabTitles().front(), QStringLiteral("second"));
    EXPECT_FALSE(chrome->searchOpen());
    EXPECT_TRUE(chrome->searchStatus().isEmpty());
    EXPECT_TRUE(waitFor([&] {
        first->refreshSnapshot();
        return first->snapshot().at(0, 0).bg == original;
    }));
    // A search that has not completed must also remain retired after its callback.
    window->revealPane(first);
    chrome->setSearchOpen(true);
    chrome->search(QStringLiteral("needle"), 0);
    window->revealPane(second);
    // Let the search finish and its watcher report, then check. The watcher
    // reports on the GUI thread, after the pool has finished the search.
    ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(2000));
    ASSERT_TRUE(waitFor([&] {
        const auto watchers = first->findChildren<QFutureWatcherBase*>();
        return std::all_of(watchers.begin(), watchers.end(), [](const QFutureWatcherBase* w) { return w->isFinished(); });
    }));
    first->refreshSnapshot();
    EXPECT_EQ(first->snapshot().at(0, 0).bg, original);
    EXPECT_TRUE(chrome->searchStatus().isEmpty());
}

TEST_F(NativeWindow, SwapFailurePreservesZoomAndSuccessRestoresVisiblePanes) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    ASSERT_NE(window->splitPane(first, SplitTree::Direction::Right, launch, nullptr), nullptr);
    ASSERT_TRUE(window->runAction(first, Action::TogglePaneZoom));
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    // The titles follow the next frame, which has no fixed time.
    ASSERT_TRUE(waitFor([&] { return chrome->paneTitles().size() == 1; }));
    const auto zoomed = window->accessiblePaneRect(first);
    EXPECT_FALSE(window->swapPane(first, SplitTree::Neighbour::Left));
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(chrome->paneTitles().size(), 1);
    EXPECT_EQ(window->accessiblePaneRect(first), zoomed);
    ASSERT_TRUE(window->swapPane(first, SplitTree::Neighbour::Right));
    EXPECT_TRUE(waitFor([&] { return chrome->paneTitles().size() == 2; }));
    EXPECT_LT(window->accessiblePaneRect(first).width(), zoomed.width());
}

TEST_F(NativeWindow, RotationMovesLivePanesAndClearsZoom) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    auto* third = window->splitPane(second, SplitTree::Direction::Down, launch, nullptr);
    ASSERT_NE(third, nullptr);
    // The panes have their slots from the next frame's layout.
    const auto laidOut = [&] {
        const QRect a = window->accessiblePaneRect(first), b = window->accessiblePaneRect(second),
                    c = window->accessiblePaneRect(third);
        return !a.isEmpty() && !b.isEmpty() && !c.isEmpty() && a != b && b != c && a != c;
    };
    ASSERT_TRUE(waitFor(laidOut));
    const QRect slots[] = {window->accessiblePaneRect(first), window->accessiblePaneRect(second),
                           window->accessiblePaneRect(third)};
    ASSERT_TRUE(window->runAction(first, Action::TogglePaneZoom));
    ASSERT_TRUE(waitFor([&] { return window->accessiblePaneRect(second).isEmpty(); }));
    ASSERT_TRUE(window->runAction(first, Action::RotatePanesClockwise));
    EXPECT_TRUE(waitFor([&] {
        return laidOut() && window->accessiblePaneRect(third) == slots[0] && window->accessiblePaneRect(first) == slots[1]
            && window->accessiblePaneRect(second) == slots[2];
    }));
    EXPECT_EQ(window->accessiblePaneRect(third), slots[0]);
    EXPECT_EQ(window->accessiblePaneRect(first), slots[1]);
    EXPECT_EQ(window->accessiblePaneRect(second), slots[2]);
    auto* titles = window->findChild<ChromeController*>();
    ASSERT_NE(titles, nullptr);
    EXPECT_TRUE(waitFor([&] { return titles->paneTitles().size() == 3; }));
    EXPECT_EQ(window->focusedPane(), first);
    ASSERT_TRUE(window->runAction(first, Action::RotatePanesCounterclockwise));
    EXPECT_TRUE(waitFor([&] { return window->accessiblePaneRect(first) == slots[0]; }));
}

TEST_F(NativeWindow, InitialMenuUpSelectsLastItem) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    QVariantList entries;
    for (int i = 0; i < 3; ++i)
        entries.append(QVariantMap{{QStringLiteral("id"), 40000 + i}, {QStringLiteral("title"), QString::number(i)}});
    chrome->openContextMenu({10, 10}, entries);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &up);
    ASSERT_NE(layer->rootItem(), nullptr);
    QString selected;
    for (auto* item : visualDescendants(layer->rootItem()))
        if (item->property("literalTitle").isValid() && item->property("highlighted").toBool())
            selected = item->property("literalTitle").toString();
    EXPECT_EQ(selected, QStringLiteral("2"));
}

TEST_F(NativeWindow, LazyChromeInitializesPanelFocusAndTitle) {
    // Each panel opens after the scene is made, and while it is still being
    // made in the background: the panel is then open when the scene completes.
    for (const bool settled : {true, false})
    for (const auto& kind : {QStringLiteral("title"), QStringLiteral("palette"), QStringLiteral("search"),
                             QStringLiteral("paste"), QStringLiteral("clipboard"), QStringLiteral("confirm"),
                             QStringLiteral("script")}) {
        SCOPED_TRACE(kind.toStdString() + (settled ? " after the scene is made" : " while the scene is made"));
        WindowOptions options;
        options.command = {QStringLiteral("/bin/cat")};
        std::unique_ptr<TerminalWindow> fresh(application->newWindow(options));
        EXPECT_TRUE(fresh != nullptr);
        if (!fresh) continue;
        auto* layer = fresh->findChild<ChromeLayer*>();
        EXPECT_NE(layer, nullptr);
        if (!layer) continue;
        if (settled) {
            EXPECT_TRUE(waitFor([&] { return layer->rootItem() != nullptr; }));
        }
        auto* chrome = fresh->findChild<ChromeController*>();
        EXPECT_NE(chrome, nullptr);
        if (!chrome) continue;
        if (kind == QLatin1String("title")) chrome->askTitle(QStringLiteral("Title"), QStringLiteral("existing title"));
        else if (kind == QLatin1String("palette")) chrome->setPaletteOpen(true);
        else if (kind == QLatin1String("search")) chrome->setSearchOpen(true);
        else if (kind == QLatin1String("paste")) chrome->askPaste(QStringLiteral("one\ntwo"), 2);
        else if (kind == QLatin1String("clipboard")) chrome->askClipboard(QStringLiteral("Read clipboard"), QString());
        else if (kind == QLatin1String("script"))
            chrome->setScriptDialog({{QStringLiteral("open"), true}, {QStringLiteral("kind"), QStringLiteral("prompt")},
                                     {QStringLiteral("title"), QStringLiteral("Script")}});
        else chrome->askConfirm(QStringLiteral("Confirm"), QStringLiteral("Body"), QStringLiteral("Accept"));
        QElapsedTimer deadline;
        deadline.start();
        while (!chromeFocusReady(layer) && deadline.elapsed() < 2000) {
            // Run queued QML initialization and focus callbacks on real event-loop turns.
            QEventLoop loop;
            QTimer::singleShot(20, &loop, &QEventLoop::quit);
            loop.exec();
        }
        EXPECT_TRUE(chromeFocusReady(layer));
        if (!chromeFocusReady(layer)) continue;
        if (kind == QLatin1String("title")) {
            EXPECT_EQ(layer->focusedItem()->property("text").toString(), QStringLiteral("existing title"));
        }
        if (kind == QLatin1String("palette") || kind == QLatin1String("search")) {
            EXPECT_TRUE(layer->focusedItem()->property("placeholderText").isValid());
        }
        if (kind == QLatin1String("script")) {
            // The window, which closes a script dialog, did not open this one.
            chrome->setScriptDialog({});
        } else {
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(fresh.get(), &escape);
        }
        EXPECT_FALSE(chrome->wantsKeyboard());
    }
}

class LauncherObserver : public QObject {
    Q_OBJECT
public:
    QList<QVariantMap> updates;
public Q_SLOTS:
    void update(const QString&, const QVariantMap& properties) { updates.append(properties); }
};
TEST_F(NativeWindow, WindowsDeletedWhileTheirChromeLoadsLeaveTheNextChromeSound) {
    // The application loads the chrome type in the background, and each
    // window incubates its scene from it. A window that goes before its
    // scene is made, or before the load ends, leaves the next window's
    // chrome sound, also across an engine that goes.
    WindowOptions options;
    options.command = {QStringLiteral("/bin/cat")};
    for (int i = 0; i < 20; ++i) {
        delete application->newWindow(options);
        if (i % 4 == 0) drain(5);
    }
    drain(200);
    auto* next = application->newWindow(options);
    ASSERT_NE(next, nullptr);
    auto* layer = next->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    EXPECT_TRUE(waitFor([&] { return layer->rootItem() != nullptr; }));
    delete next;
    // The same while the engine goes too, as between two tests, and while a
    // new application's load still runs.
    for (int i = 0; i < 8; ++i) delete application->newWindow(options);
    application.reset();
    application = std::make_unique<Application>(testSocket(), directory.filePath(QStringLiteral("afterglow.conf")), testSettings());
    for (int i = 0; i < 8; ++i) delete application->newWindow(options);
    auto* fresh = application->newWindow(options);
    ASSERT_NE(fresh, nullptr);
    auto* freshLayer = fresh->findChild<ChromeLayer*>();
    ASSERT_NE(freshLayer, nullptr);
    EXPECT_TRUE(waitFor([&] { return freshLayer->rootItem() != nullptr; }));
    window = nullptr;
}

TEST_F(NativeWindow, ClosingStatusWindowRepublishesLauncherAggregate) {
    LauncherObserver observer;
    auto bus = QDBusConnection::sessionBus();
    ASSERT_TRUE(bus.connect(QString(), QStringLiteral("/com/phosphorworks/Afterglow"),
        QStringLiteral("com.canonical.Unity.LauncherEntry"), QStringLiteral("Update"),
        &observer, SLOT(update(QString,QVariantMap))));
    WindowOptions options;
    options.command = {QStringLiteral("/bin/cat")};
    auto* contributor = application->newWindow(options);
    ASSERT_NE(contributor, nullptr);
    // Terminal status comes from the PTY reader thread and crosses to the GUI thread.
    feedFromReader(contributor->focusedPane(), "\x1b]7501;state=working:progress=50\x1b\\");
    // A window that closed in an earlier test can still have an update on
    // the bus: wait for the one that this progress sends.
    const auto lastShowsProgress = [&] {
        return !observer.updates.isEmpty() && observer.updates.last().value(QStringLiteral("progress-visible")).toBool();
    };
    ASSERT_TRUE(waitFor(lastShowsProgress));
    observer.updates.clear();
    delete contributor;
    EXPECT_TRUE(waitFor([&] { return !observer.updates.isEmpty() && !lastShowsProgress(); }));
    ASSERT_FALSE(observer.updates.isEmpty());
    EXPECT_FALSE(observer.updates.last().value(QStringLiteral("urgent")).toBool());
}

TEST_F(NativeWindow, InputMethodPreservesGraphemesAndCommitsToPty) {
    auto* pane = window->focusedPane();
    const QString preedit = QString::fromUtf8("🙂é👩‍💻");
    QInputMethodEvent event(preedit, {QInputMethodEvent::Attribute(QInputMethodEvent::Cursor, 2, 1, {})});
    QCoreApplication::sendEvent(window, &event);
    pane->refreshSnapshot();
    const auto& snapshot = pane->snapshot();
    EXPECT_EQ(snapshot.cursorX, 2);
    // A cell's text is its codepoint and then the extra ones in graphemes, as
    // the renderer reads it: the first codepoint must not repeat.
    const auto cellText = [&](const AfterglowRender::SnapshotCell& cell) {
        std::u32string text(1, cell.cp);
        if (cell.grapheme > 0 && cell.grapheme <= snapshot.graphemes.size()) text += snapshot.graphemes[cell.grapheme - 1];
        return text;
    };
    const auto shows = [&](std::u32string_view wanted) {
        return std::any_of(snapshot.cells.begin(), snapshot.cells.end(), [&](const auto& cell) { return cell.cp && cellText(cell) == wanted; });
    };
    EXPECT_TRUE(shows(U"e\u0301"));
    EXPECT_TRUE(shows(U"\U0001F469\u200D\U0001F4BB"));
    EXPECT_FALSE(shows(U"ee\u0301"));
    QInputMethodEvent commit; commit.setCommitString(QStringLiteral("native-ime-accepted\n"));
    QCoreApplication::sendEvent(window, &commit);
    EXPECT_TRUE(waitFor([&] {
        return screenOf(pane).contains(QStringLiteral("native-ime-accepted"));
    }));
}
TEST_F(NativeWindow, InputMethodTextTakesTheCellsOfPrintedText) {
    // The composition takes the cells that the terminal gives the same text:
    // Hindi KA AA two (a spacing mark widens it), a watch with VS15 one.
    auto* pane = window->focusedPane();
    const QString text = QString::fromUtf8("\u0915\u093E\u231A\uFE0E");
    QInputMethodEvent event(text, {QInputMethodEvent::Attribute(QInputMethodEvent::Cursor, int(text.size()), 1, {})});
    QCoreApplication::sendEvent(window, &event);
    pane->refreshSnapshot();
    const auto& snapshot = pane->snapshot();
    const auto widthOf = [&](char32_t cp) {
        const auto cell = std::find_if(snapshot.cells.begin(), snapshot.cells.end(), [&](const auto& c) { return c.cp == cp; });
        return cell == snapshot.cells.end() ? -1 : int(cell->width);
    };
    EXPECT_EQ(widthOf(U'\u0915'), 2);
    EXPECT_EQ(widthOf(U'\u231A'), 1);
    QInputMethodEvent clear;
    QCoreApplication::sendEvent(window, &clear);
}

TEST_F(NativeWindow, InputMethodQueriesFollowChromeEditor) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    chrome->setSearchOpen(true);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    QInputMethodQueryEvent query(Qt::ImEnabled | Qt::ImCursorRectangle | Qt::ImSurroundingText);
    QCoreApplication::sendEvent(window, &query);
    EXPECT_TRUE(query.value(Qt::ImEnabled).toBool());
    EXPECT_FALSE(query.value(Qt::ImCursorRectangle).toRectF().isEmpty());
    QInputMethodEvent commit; commit.setCommitString(QString::fromUtf8("検索"));
    QCoreApplication::sendEvent(window, &commit);
    const auto surrounding = [&] {
        QInputMethodQueryEvent text(Qt::ImSurroundingText);
        QCoreApplication::sendEvent(window, &text);
        return text.value(Qt::ImSurroundingText).toString();
    };
    EXPECT_TRUE(waitFor([&] { return surrounding() == QString::fromUtf8("検索"); })) << surrounding().toStdString();
}
TEST_F(NativeWindow, ChromeButtonsReceiveKeyReleases) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    chrome->setSearchOpen(true);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    // The search field takes the keys first: the button takes them after.
    ASSERT_TRUE(waitFor([&] { return layer->rootItem() != nullptr && chromeFocusReady(layer); }));
    QQuickItem* button = nullptr;
    for (auto* item : layer->rootItem()->findChildren<QQuickItem*>())
        if (item->property("text").toString() == QStringLiteral(".*")) { button = item; break; }
    ASSERT_NE(button, nullptr);
    button->forceActiveFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    QCoreApplication::sendEvent(window, &press);
    EXPECT_TRUE(button->property("pressed").toBool());
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    QCoreApplication::sendEvent(window, &release);
    EXPECT_FALSE(button->property("pressed").toBool());
    EXPECT_TRUE(chrome->searchRegex());
}
TEST_F(NativeWindow, AccessibleChromeExposesFocusedEditor) {
    QAccessible::setActive(true);
    // Later tests must not run with accessibility on.
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    chrome->setSearchOpen(true);
    // The chrome scene is made in the background and may not be ready
    // yet, which on a new window can take longer than a fixed wait.
    ASSERT_TRUE(waitFor([&] { return window->accessibleChrome() != nullptr; }));
    auto* root = window->accessibleRoot();
    ASSERT_NE(root, nullptr);
    // The search field takes the focus one event-loop turn after it shows.
    QAccessibleInterface* focus = nullptr;
    ASSERT_TRUE(waitFor([&] {
        focus = root->focusChild();
        return focus != nullptr && focus->state().editable;
    }));
    EXPECT_GE(root->childCount(), 2);
    EXPECT_EQ(window->accessibleChrome()->parent(), root);
    EXPECT_GT(window->accessibleChrome()->childCount(), 0);
    EXPECT_TRUE(root->rect().contains(focus->rect().center()));
}
// What the accessibility layer tells clients, as Qt's AT-SPI bridge hears it.
struct AccessibleEvents {
    struct Event { QAccessible::Event type; QAccessibleInterface* iface; bool valid; };
    QList<Event> events;
    // The bridge names a removed button's parent by its id, which registers
    // the parent when the cache no longer holds it.
    QList<QAccessible::Id> parentsOfRemoved;
    static AccessibleEvents*& current() { static AccessibleEvents* events = nullptr; return events; }
    // The handler that was there before, which goes back when this one goes.
    QAccessible::UpdateHandler previous = nullptr;
    AccessibleEvents() {
        current() = this;
        previous = QAccessible::installUpdateHandler([](QAccessibleEvent* event) {
            QAccessibleInterface* iface = event->accessibleInterface();
            current()->events.append({event->type(), iface, iface && iface->isValid()});
            if (event->type() == QAccessible::ObjectDestroyed && iface && iface->isValid() && iface->role() == QAccessible::Button)
                if (QAccessibleInterface* parent = iface->parent()) current()->parentsOfRemoved.append(QAccessible::uniqueId(parent));
        });
    }
    ~AccessibleEvents() { QAccessible::installUpdateHandler(previous); current() = nullptr; }
    int count(QAccessible::Event type, QAccessibleInterface* iface) const {
        return int(std::count_if(events.cbegin(), events.cend(), [&](const Event& e) { return e.type == type && e.iface == iface; }));
    }
};
TEST_F(NativeWindow, AnEndedRunClosesOnlyItsOwnDialog) {
    Pane* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    // Shared with the reply: after a failed check the test returns, and the
    // window answers the dialog later, when the fixture closes it.
    const auto answer = std::make_shared<std::optional<QString>>();
    const QVariantMap prompt{{QStringLiteral("kind"), QStringLiteral("prompt")}, {QStringLiteral("title"), QStringLiteral("Name")},
                             {QStringLiteral("request_id"), QStringLiteral("afterglow.await:31:1")}};
    ASSERT_TRUE(window->showScriptDialog(QStringLiteral("a@1"), pane, prompt,
                                         [answer](const QString& error, const QVariantMap&) { *answer = error; }).isEmpty());
    ASSERT_TRUE(chrome->scriptDialog().value(QStringLiteral("open")).toBool());
    // Another flow, one whose number starts with the same digit, and another
    // owner leave it open.
    window->closeRunWaits(QStringLiteral("a@1"), QStringLiteral("afterglow.await:4:"));
    window->closeRunWaits(QStringLiteral("a@1"), QStringLiteral("afterglow.await:3:"));
    window->closeRunWaits(QStringLiteral("b@2"), QStringLiteral("afterglow.await:31:"));
    EXPECT_FALSE(answer->has_value());
    window->closeRunWaits(QStringLiteral("a@1"), QStringLiteral("afterglow.await:31:"));
    ASSERT_TRUE(answer->has_value());
    EXPECT_EQ(**answer, QStringLiteral("Dialog canceled"));
    EXPECT_FALSE(chrome->scriptDialog().value(QStringLiteral("open")).toBool());
}
// Sends a press of @p key, @p repeats auto-repeats of it as a held key
// sends them (release and press pairs), and its release.
void holdKey(TerminalWindow* window, int key, Qt::KeyboardModifiers modifiers, const QString& text, int repeats) {
    QKeyEvent down(QEvent::KeyPress, key, modifiers, text);
    QCoreApplication::sendEvent(window, &down);
    for (int i = 0; i < repeats; ++i) {
        QKeyEvent up(QEvent::KeyRelease, key, modifiers, text, true);
        QCoreApplication::sendEvent(window, &up);
        QKeyEvent again(QEvent::KeyPress, key, modifiers, text, true);
        QCoreApplication::sendEvent(window, &again);
    }
    QKeyEvent up(QEvent::KeyRelease, key, modifiers, text);
    QCoreApplication::sendEvent(window, &up);
}
TEST_F(NativeWindow, PaletteMenuAndKeyRunScriptCommands) {
    // A script beside the config: one command in the palette, one only for
    // keys. Each run has its number, so that a run too many shows.
    ASSERT_TRUE(QDir().mkpath(directory.filePath(QStringLiteral("scripts"))));
    QFile script(directory.filePath(QStringLiteral("scripts/marker.luau")));
    ASSERT_TRUE(script.open(QIODevice::WriteOnly));
    script.write(R"(
local runs = 0
return {
    metadata = { commands = {
        mark = { summary = "Title the pane", palette = "Mark this pane" },
        keyed = { summary = "Not in the palette" },
    } },
    run_command = function(command, ev)
        runs += 1
        local run = runs
        afterglow.sleep(1) -- what a run returns after a wait still applies
        return { afterglow.set_title(ev.pane, command .. " " .. run .. " from " .. ev.source .. " with " .. #ev.args),
                 afterglow.output(ev, "dropped") }
    end,
}
)");
    script.close();
    RegisteredNotificationServer notices;
    ASSERT_TRUE(notices.registered);
    // The fixture turns scripts off.
    const QJsonObject marks{{QStringLiteral("script:marker.keyed"), QStringLiteral("K")},
                            {QStringLiteral("script:marker.missing"), QStringLiteral("J")}};
    restartApplication(QStringLiteral("commands.conf"),
        testSettings({{QStringLiteral("Scripts"), QJsonObject{{QStringLiteral("enabled"), true}}},
                      {QStringLiteral("Keybinds"), QJsonObject{
                          {QStringLiteral("bindings"), QJsonObject{{QStringLiteral("script:marker.keyed"), QStringLiteral("Ctrl+Alt+M")},
                                                                   {QStringLiteral("table:marks"), QStringLiteral("Ctrl+Alt+T")}}},
                          {QStringLiteral("tables"), QJsonObject{{QStringLiteral("marks"), marks}}}}}}),
        {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    // Two panes: each entry point names the pane it runs on.
    Pane* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(window->focusedPane(), second);
    const auto title = [](const QVariant& entry) { return entry.toMap().value(QStringLiteral("title")).toString(); };

    // The palette lists only the command with a palette title, after the
    // id of its script, and runs it on the focused pane.
    const auto scriptEntries = [&] {
        QVariantList found;
        for (const QVariant& entry : chrome->actions())
            if (title(entry).startsWith(QLatin1String("marker: "))) found.append(entry);
        return found;
    };
    ASSERT_TRUE(waitFor([&] { return !scriptEntries().isEmpty(); }));
    const QVariantList entries = scriptEntries();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(title(entries[0]), QStringLiteral("marker: Mark this pane"));
    chrome->runAction(entries[0].toMap().value(QStringLiteral("id")).toInt());
    EXPECT_TRUE(waitFor([&] { return second->userTitle() == QLatin1String("mark 1 from palette with 0"); }))
        << second->userTitle().toStdString();

    // A key runs the command that the palette does not list: once for a
    // press, also when the key is held and repeats. A run that a repeat
    // started would take a number, which the titles of the runs after it
    // show.
    holdKey(window, Qt::Key_M, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("m"), 4);
    EXPECT_TRUE(waitFor([&] { return second->userTitle() == QLatin1String("keyed 2 from key with 0"); }))
        << second->userTitle().toStdString();

    // A key table runs it too, also once for a held key.
    sendKey(window, Qt::Key_T, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("t"));
    holdKey(window, Qt::Key_K, Qt::NoModifier, QStringLiteral("k"), 4);
    EXPECT_TRUE(waitFor([&] { return second->userTitle() == QLatin1String("keyed 3 from key with 0"); }))
        << second->userTitle().toStdString();
    // The table took the key of a command that no script has: the refusal
    // shows, with the command's title.
    sendKey(window, Qt::Key_J, Qt::NoModifier, QStringLiteral("j"));
    EXPECT_TRUE(waitFor([&] { return notices.server.count >= 1; }));
    EXPECT_EQ(notices.server.titles.value(0), QStringLiteral("Could not run marker missing"));
    sendKey(window, Qt::Key_Escape);
    EXPECT_TRUE(chrome->keyMode().isEmpty()) << chrome->keyMode().toStdString();
    EXPECT_TRUE(first->userTitle().isEmpty()) << first->userTitle().toStdString();

    // The context menu lists the palette command, alone, in its Scripts
    // submenu. It runs on the pane that the menu opened on, also when the
    // focus moved since.
    window->openPaneMenu(first);
    QVariantList scripts;
    int submenus = 0;
    for (const QVariant& item : chrome->contextMenu().value(QStringLiteral("items")).toList()) {
        if (title(item) != QLatin1String("Scripts")) continue;
        ++submenus;
        scripts = item.toMap().value(QStringLiteral("items")).toList();
    }
    ASSERT_EQ(submenus, 1);
    ASSERT_EQ(scripts.size(), 1);
    EXPECT_EQ(title(scripts[0]), QStringLiteral("Mark this pane"));
    window->revealPane(second);
    chrome->activateMenuItem(scripts[0].toMap().value(QStringLiteral("id")).toInt());
    EXPECT_TRUE(waitFor([&] { return first->userTitle() == QLatin1String("mark 4 from menu with 0"); }))
        << first->userTitle().toStdString();
    EXPECT_EQ(second->userTitle(), QStringLiteral("keyed 3 from key with 0"));

    // The runs wrote only to stdout, which no client reads: no notice but
    // the refusal. The notices arrive in the order sent: all of them are in
    // once a marker sent after them is.
    ASSERT_TRUE(settleQueued());
    NotificationCenter::instance().showNotice(nullptr, QStringLiteral("Test"), QStringLiteral("last"));
    ASSERT_TRUE(waitFor([&] { return notices.server.bodies.contains(QStringLiteral("last")); }));
    EXPECT_EQ(notices.server.titles, (QStringList{QStringLiteral("Could not run marker missing"), QStringLiteral("Test")}));
}
TEST_F(NativeWindow, AnOpenPromptKeepsItsAnswerFromOutsideActionsAndChords) {
    Pane* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    SettingsStore settings(directory.filePath(QStringLiteral("control.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    const auto chord = [&](int key) { sendKey(window, key, Qt::ControlModifier | Qt::ShiftModifier); };
    // A paste that asks first.
    QList<bool> pasted;
    QObject listener;
    QObject::connect(chrome, &ChromeController::pasteAnswered, &listener, [&](bool paste) { pasted << paste; });
    QGuiApplication::clipboard()->setText(QStringLiteral("prompt-line-one\nprompt-line-two\n"));
    ASSERT_TRUE(window->runAction(pane, Action::Paste));
    ASSERT_TRUE(chrome->pasteConfirmOpen());
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    // afterglow ctl and afterglow.action act as a key would: not now.
    EXPECT_FALSE(window->runAction(pane, Action::SplitRight));
    EXPECT_FALSE(control.runAction(pane->id(), QStringLiteral("split_right")).isEmpty());
    EXPECT_FALSE(control.runAction(pane->id(), QStringLiteral("search")).isEmpty());
    EXPECT_EQ(window->panes().size(), 1);
    // The palette and search chords do not open over the prompt, where an
    // action from the palette would answer for it.
    chord(Qt::Key_P);
    chord(Qt::Key_F);
    EXPECT_FALSE(chrome->paletteOpen());
    EXPECT_FALSE(chrome->searchOpen());
    EXPECT_TRUE(chrome->pasteConfirmOpen());
    EXPECT_TRUE(pasted.isEmpty());
    chrome->answerPaste(false);
    EXPECT_EQ(pasted, QList<bool>{false});
    // With the prompt answered, they act again. Over the search, the
    // palette chord opens and closes the palette.
    EXPECT_TRUE(control.runAction(pane->id(), QStringLiteral("search")).isEmpty());
    ASSERT_TRUE(chrome->searchOpen());
    chord(Qt::Key_P);
    EXPECT_TRUE(chrome->paletteOpen());
    chord(Qt::Key_P);
    EXPECT_FALSE(chrome->paletteOpen());
    chrome->setSearchOpen(false);

    // A script's dialog: the same.
    const auto answer = std::make_shared<std::optional<QString>>();
    const QVariantMap prompt{{QStringLiteral("kind"), QStringLiteral("prompt")}, {QStringLiteral("title"), QStringLiteral("Name")},
                             {QStringLiteral("request_id"), QStringLiteral("afterglow.await:1:1")}};
    ASSERT_TRUE(window->showScriptDialog(QStringLiteral("a@1"), pane, prompt,
                                         [answer](const QString& error, const QVariantMap&) { *answer = error; }).isEmpty());
    EXPECT_FALSE(window->runAction(pane, Action::NewTab));
    EXPECT_EQ(window->tabs().size(), 1u);
    chord(Qt::Key_P);
    EXPECT_FALSE(chrome->paletteOpen());
    EXPECT_FALSE(answer->has_value());
    window->closeRunWaits(QStringLiteral("a@1"), QStringLiteral("afterglow.await:1:"));
    ASSERT_TRUE(answer->has_value());

    // A close that asks first keeps its question: the window's own close,
    // which would ask too, does not take over the answer.
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* background = window->openTab(launch, nullptr);
    ASSERT_NE(background, nullptr);
    window->revealPane(pane);
    const quint32 backgroundTab = window->tabIdOf(background);
    ASSERT_TRUE(window->runAction(background, Action::CloseTab));
    ASSERT_TRUE(chrome->confirmPromptOpen());
    const QString heading = chrome->confirmPrompt().value(QStringLiteral("heading")).toString();
    QPointer<TerminalWindow> alive = window;
    QCloseEvent close;
    QCoreApplication::sendEvent(window, &close);
    EXPECT_TRUE(chrome->confirmPromptOpen());
    EXPECT_EQ(chrome->confirmPrompt().value(QStringLiteral("heading")).toString(), heading);
    EXPECT_FALSE(window->runAction(pane, Action::CloseWindow));
    chrome->answerConfirm(true);
    ASSERT_TRUE(waitFor([&] { return window->tabs().size() == 1u; }));
    EXPECT_NE(window->tabs()[0].id, backgroundTab);
    ASSERT_TRUE(settleQueued());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_FALSE(alive.isNull());
}
TEST_F(NativeWindow, AWindowWhoseOnlyPaneClosesGoesAfterThePaneAndItsDock) {
    // The last tab's panes go with deleteLater before their window does.
    // The window must not reach them through the tab, which still holds
    // their addresses: a pane dock's accessible area is what it would read.
    // Only a build with AddressSanitizer (the debug preset) can show that
    // read of freed memory: elsewhere this test mostly passes without the fix.
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QPointer<Pane> pane = window->focusedPane();
    ASSERT_TRUE(pane);
    ASSERT_EQ(window->tabs().size(), 1u);
    ASSERT_EQ(window->panes().size(), 1);
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("title"), QStringLiteral("top"), 1,
        {dockRun(QStringLiteral("OnlyTitle"), QStringLiteral("t"))}, QStringLiteral("pane")), pane).isEmpty());
    QAccessible::Id area = 0;
    ASSERT_TRUE(waitFor([&] {
        QAccessibleInterface* root = window->accessibleRoot();
        for (int i = 0; root && i < root->childCount(); ++i) {
            QAccessibleInterface* child = root->child(i);
            if (child && child->object() == nullptr && child->text(QAccessible::Name).contains(QLatin1String("OnlyTitle")))
                area = QAccessible::uniqueId(child);
        }
        return area != 0;
    }));
    QPointer<TerminalWindow> closing = window;
    window->requestClosePane(pane);
    ASSERT_TRUE(waitDeleted(closing));
    window = nullptr;
    EXPECT_TRUE(pane.isNull());
    EXPECT_EQ(QAccessible::accessibleInterface(area), nullptr);
}
TEST_F(NativeWindow, APanelThatTakesTheKeyboardClearsThePanesComposition) {
    Pane* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    const auto drawn = [&] {
        pane->refreshSnapshot();
        QString text;
        for (const auto& cell : pane->snapshot().cells)
            if (cell.cp) text += QString::fromUcs4(&cell.cp, 1);
        return text;
    };
    QInputMethodEvent compose(QStringLiteral("zqxcomposed"), {});
    QCoreApplication::sendEvent(window, &compose);
    ASSERT_TRUE(drawn().contains(QLatin1String("zqxcomposed")));
    // The input method's own clearing event would go to the search field.
    chrome->setSearchOpen(true);
    EXPECT_FALSE(drawn().contains(QLatin1String("zqxcomposed")));
    chrome->setSearchOpen(false);
}
TEST_F(NativeWindow, CopyModeAndUnicodeInputEndEachOtherAndWaitForAScriptsKeyMode) {
    Pane* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    const auto unicodeOpen = [&] { return chrome->unicodeInput().value(QStringLiteral("active")).toBool(); };
    const auto copying = [&] { return chrome->keyMode().startsWith(QLatin1String("Copy")); };
    ASSERT_TRUE(window->runAction(pane, Action::CopyMode));
    ASSERT_TRUE(copying()) << chrome->keyMode().toStdString();
    ASSERT_TRUE(window->runAction(pane, Action::UnicodeInput));
    EXPECT_TRUE(unicodeOpen());
    EXPECT_FALSE(copying()) << chrome->keyMode().toStdString();
    ASSERT_TRUE(window->runAction(pane, Action::CopyMode));
    EXPECT_FALSE(unicodeOpen());
    EXPECT_TRUE(copying()) << chrome->keyMode().toStdString();
    ASSERT_TRUE(window->runAction(pane, Action::CopyMode)); // and out again
    EXPECT_FALSE(copying());
    // Hints end Unicode input too.
    ASSERT_TRUE(window->runAction(pane, Action::UnicodeInput));
    ASSERT_TRUE(unicodeOpen());
    ASSERT_TRUE(window->runAction(pane, Action::ShowHints));
    EXPECT_FALSE(unicodeOpen());
    // A script's key mode keeps its keys: copy mode's, Unicode input's and
    // the labels' would come before them.
    ASSERT_TRUE(window->startScriptKeyMode(QStringLiteral("script@1"), pane, QStringLiteral("Script keys")).isEmpty());
    EXPECT_FALSE(window->runAction(pane, Action::CopyMode));
    EXPECT_FALSE(window->runAction(pane, Action::UnicodeInput));
    EXPECT_FALSE(window->runAction(pane, Action::ShowHints));
    EXPECT_FALSE(window->runAction(pane, Action::UrlsOpen));
    EXPECT_FALSE(window->runAction(pane, Action::UrlsCopy));
    EXPECT_TRUE(chrome->urlHints().isEmpty());
    EXPECT_EQ(chrome->keyMode(), QStringLiteral("Script keys"));
    EXPECT_FALSE(unicodeOpen());
    window->endScriptKeyMode(QStringLiteral("script@1"));
}
TEST_F(NativeWindow, AccessiblePaneRunsThePaneActionsOfThePalette) {
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto* original = window->focusedPane();
    auto* accessible = QAccessible::queryAccessibleInterface(original);
    ASSERT_NE(accessible, nullptr);
    auto* actions = accessible->actionInterface();
    ASSERT_NE(actions, nullptr);
    // The menu first, the palette's pane actions in its order, the focus last.
    // The list is the one docs/DESKTOP.md gives.
    const QList<Action> paneActions{
        Action::Copy, Action::Paste, Action::SelectAll, Action::Search, Action::ClearScreen, Action::ResetTerminal,
        Action::RenamePane, Action::NotifyOnFinish, Action::ShowLastOutput, Action::CopyMode, Action::ShowHints,
        Action::UrlsOpen, Action::UrlsCopy, Action::UnicodeInput, Action::ScrollPageUp, Action::ScrollPageDown,
        Action::ScrollTop, Action::ScrollBottom, Action::JumpPromptUp, Action::JumpPromptDown, Action::SplitRight,
        Action::SplitDown, Action::SplitLeft, Action::SplitUp, Action::ClosePane, Action::TogglePaneZoom,
        Action::ResizeLeft, Action::ResizeRight, Action::ResizeUp, Action::ResizeDown, Action::SwapLeft,
        Action::SwapRight, Action::SwapUp, Action::SwapDown, Action::FocusLeft, Action::FocusRight, Action::FocusUp,
        Action::FocusDown, Action::MovePaneToTab, Action::MovePaneToWindow, Action::NewTab, Action::NewWindow};
    QStringList expected{QAccessibleActionInterface::showMenuAction()};
    for (Action action : Keybinds::allActions())
        if (paneActions.contains(action)) expected.append(Keybinds::title(action));
    expected.append(QAccessibleActionInterface::setFocusAction());
    EXPECT_EQ(expected.size(), 44);
    EXPECT_EQ(actions->actionNames(), expected);
    EXPECT_EQ(actions->keyBindingsForAction(QStringLiteral("Copy")), QStringList{QStringLiteral("Ctrl+Shift+C")});
    // Titles only: the token of a pane action, and the title of an action
    // that is not a pane's, name nothing.
    EXPECT_TRUE(actions->keyBindingsForAction(QStringLiteral("copy")).isEmpty());
    EXPECT_TRUE(actions->keyBindingsForAction(Keybinds::title(Action::NextTab)).isEmpty());

    // A structural action, as its keys run it.
    actions->doAction(QStringLiteral("Split Right"));
    ASSERT_TRUE(waitFor([&] { return window->panes().size() == 2; }));
    Pane* other = window->focusedPane();
    EXPECT_NE(other, original);
    // The split is laid out at the next frame: the cursor rectangles below
    // come from that layout.
    ASSERT_TRUE(nextFrame(window));

    // Copy runs in the pane it was asked of, not in the one with the focus.
    original->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[2J\x1b[Hreadable\r\n"); });
    original->startSelection(original->selectionPointAt(0, 0), AfterglowVt::SelectionMode::Normal, true);
    original->extendSelection(original->selectionPointAt(7, 0));
    ASSERT_EQ(original->selectedText(), QStringLiteral("readable"));
    QGuiApplication::clipboard()->setText(QStringLiteral("before"));
    ASSERT_TRUE(waitFor([] { return QGuiApplication::clipboard()->text() == QStringLiteral("before"); }));
    actions->doAction(QStringLiteral("Copy"));
    EXPECT_TRUE(waitFor([] { return QGuiApplication::clipboard()->text() == QStringLiteral("readable"); }))
        << QGuiApplication::clipboard()->text().toStdString();
    EXPECT_EQ(window->focusedPane(), original);

    // The pane's focus state below follows the window's: only an active
    // window gives one.
    window->requestActivate();
    if (!waitFor([&] { return window->isActive(); }, 2000)) GTEST_SKIP() << "the compositor did not activate the window";
    // The menu opens at the pane's cursor, which a pointer need not be near.
    window->revealPane(other);
    ASSERT_TRUE(nextFrame(window));
    ASSERT_FALSE(chrome->contextMenuOpen());
    actions->doAction(QAccessibleActionInterface::showMenuAction());
    ASSERT_TRUE(chrome->contextMenuOpen());
    EXPECT_EQ(window->focusedPane(), original);
    const QPointF at(chrome->contextMenu().value(QStringLiteral("x")).toDouble(), chrome->contextMenu().value(QStringLiteral("y")).toDouble());
    // While the menu holds the keyboard, no action runs: not another
    // menu, not a split, not a focus change. Another menu would open at the
    // other pane's cursor, with items of its own.
    const QVariantMap menu = chrome->contextMenu();
    auto* otherAccessible = QAccessible::queryAccessibleInterface(other);
    ASSERT_NE(otherAccessible, nullptr);
    auto* otherActions = otherAccessible->actionInterface();
    ASSERT_NE(otherActions, nullptr);
    otherActions->doAction(QAccessibleActionInterface::showMenuAction());
    otherActions->doAction(QStringLiteral("Split Down"));
    otherActions->doAction(QAccessibleActionInterface::setFocusAction());
    ASSERT_TRUE(settleQueued());
    EXPECT_TRUE(chrome->contextMenuOpen());
    EXPECT_TRUE(chrome->contextMenu() == menu);
    EXPECT_EQ(window->panes().size(), 2);
    EXPECT_EQ(window->focusedPane(), original);
    EXPECT_FALSE(accessible->state().focused); // the menu has the focus
    chrome->closeContextMenu();
    // The open menu answers the input method itself.
    ASSERT_TRUE(waitFor([&] { return !chrome->wantsKeyboard(); }));
    // The pane has the focus again: it was the menu that held it.
    EXPECT_TRUE(accessible->state().focused);
    QInputMethodQueryEvent query(Qt::ImCursorRectangle);
    QCoreApplication::sendEvent(window, &query);
    const QRectF cursor = query.value(Qt::ImCursorRectangle).toRectF();
    EXPECT_TRUE(cursor.contains(at)) << at.x() << "," << at.y() << " outside " << cursor.x() << "," << cursor.y() << " "
                                     << cursor.width() << "x" << cursor.height();

    // The focus action moves the keyboard to the pane.
    otherActions->doAction(QAccessibleActionInterface::setFocusAction());
    EXPECT_EQ(window->focusedPane(), other);

    // Tokens and the palette's other titles run nothing: not the window's
    // close, not the tab's, not the palette, not the overview.
    // One at a time: a close confirmation from one would make the rest
    // refuse for another reason.
    const QPointer<TerminalWindow> alive = window;
    for (const QString& name : {QStringLiteral("close_window"), QStringLiteral("split_right"), Keybinds::title(Action::CloseWindow), Keybinds::title(Action::CloseTab),
                                Keybinds::title(Action::CommandPalette), Keybinds::title(Action::PaneOverview)}) {
        SCOPED_TRACE(name.toStdString());
        actions->doAction(name);
        ASSERT_TRUE(settleQueued());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ASSERT_TRUE(alive);
        EXPECT_FALSE(chrome->wantsKeyboard());
        // The overview is the window's, not the chrome's: it takes no keyboard there.
        EXPECT_FALSE(window->overviewOpen());
        EXPECT_TRUE(window->isVisible());
        EXPECT_EQ(window->panes().size(), 2);
        EXPECT_EQ(window->tabs().size(), 1);
    }
}
TEST_F(NativeWindow, AccessibleDockItemsAreButtonsThatSendTheirClick) {
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    auto dock = [&](const QVariantList& runs) {
        return window->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("bar"), QStringLiteral("bottom"), 1, runs));
    };
    // Two runs of one item make one button.
    ASSERT_TRUE(dock({dockRun(QStringLiteral("["), QStringLiteral("build")), dockRun(QStringLiteral("Build]"), QStringLiteral("build")),
                      dockRun(QStringLiteral(" | ")), dockRun(QStringLiteral("Deploy"), QStringLiteral("deploy"))}).isEmpty());
    auto* root = window->accessibleRoot();
    ASSERT_NE(root, nullptr);
    auto findDock = [&]() -> QAccessibleInterface* {
        for (int i = 0; i < root->childCount(); ++i)
            if (auto* c = root->child(i); c && c->object() == nullptr && c->text(QAccessible::Name).contains(QLatin1String("Deploy"))) return c;
        return nullptr;
    };
    // The dock shows from the next frame's layout.
    ASSERT_TRUE(waitFor([&] { return findDock() != nullptr; }));
    QAccessibleInterface* area = findDock();
    ASSERT_NE(area, nullptr);
    // An area with buttons is a toolbar, which screen readers look inside.
    EXPECT_EQ(area->role(), QAccessible::ToolBar);
    ASSERT_EQ(area->childCount(), 2);
    QAccessibleInterface* build = area->child(0);
    QAccessibleInterface* deploy = area->child(1);
    ASSERT_NE(build, nullptr);
    ASSERT_NE(deploy, nullptr);
    EXPECT_EQ(build->role(), QAccessible::Button);
    EXPECT_EQ(deploy->role(), QAccessible::Button);
    EXPECT_EQ(build->text(QAccessible::Name), QStringLiteral("[Build]"));
    EXPECT_EQ(deploy->text(QAccessible::Name), QStringLiteral("Deploy"));
    // Pressed, not focused: the keyboard stays with the panes.
    EXPECT_FALSE(deploy->state().focusable);
    EXPECT_FALSE(deploy->state().invisible);
    EXPECT_EQ(deploy->parent(), area);
    EXPECT_EQ(area->indexOfChild(deploy), 1);
    EXPECT_EQ(area->child(1), deploy); // the same object each time
    // Each button covers its own cells: "[Build]" and " | " come first.
    EXPECT_TRUE(area->rect().contains(deploy->rect()));
    EXPECT_GT(deploy->rect().left(), build->rect().right());
    // Six cells against seven, within the rounding of a fractional scale.
    EXPECT_NEAR(deploy->rect().width() * 7.0, build->rect().width() * 6.0, 7.0);
    EXPECT_EQ(area->childAt(deploy->rect().center().x(), deploy->rect().center().y()), deploy);
    EXPECT_EQ(root->childAt(deploy->rect().center().x(), deploy->rect().center().y()), area);

    struct Click { QVariantMap click; bool limited; };
    QList<Click> clicks;
    QObject listener;
    QObject::connect(window, &TerminalWindow::scriptDockEvent, &listener,
                     [&](const QString& owner, const QString& handler, quint32, const QVariantMap& click, bool limited) {
                         if (owner == QLatin1String("a@1") && handler == QLatin1String("on_dock_click")) clicks.append({click, limited});
                     });
    ASSERT_NE(deploy->actionInterface(), nullptr);
    EXPECT_EQ(deploy->actionInterface()->actionNames(), QStringList{QAccessibleActionInterface::pressAction()});
    deploy->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
    ASSERT_EQ(clicks.size(), 1);
    EXPECT_EQ(clicks[0].click[QStringLiteral("dock")], QStringLiteral("bar"));
    EXPECT_EQ(clicks[0].click[QStringLiteral("item")], QStringLiteral("deploy"));
    EXPECT_EQ(clicks[0].click[QStringLiteral("button")], QStringLiteral("left"));
    EXPECT_EQ(clicks[0].click[QStringLiteral("window")].toUInt(), window->id());
    // A client can press without end: its presses count toward the limit.
    EXPECT_TRUE(clicks[0].limited);

    AccessibleEvents heard;
    // New text for the same items renames the buttons and keeps them.
    const QAccessible::Id areaId = QAccessible::uniqueId(area);
    const QAccessible::Id buildId = QAccessible::uniqueId(build), deployId = QAccessible::uniqueId(deploy);
    ASSERT_TRUE(dock({dockRun(QStringLiteral("Building"), QStringLiteral("build")), dockRun(QStringLiteral(" | ")),
                      dockRun(QStringLiteral("Deploy"), QStringLiteral("deploy"))}).isEmpty());
    ASSERT_EQ(area->childCount(), 2);
    EXPECT_EQ(area->child(0), build);
    EXPECT_EQ(build->text(QAccessible::Name), QStringLiteral("Building"));
    EXPECT_EQ(heard.count(QAccessible::NameChanged, build), 1);
    EXPECT_EQ(heard.count(QAccessible::NameChanged, deploy), 0);
    // "Deploy" moved right, after the longer label.
    EXPECT_EQ(heard.count(QAccessible::LocationChanged, deploy), 1);
    EXPECT_EQ(heard.count(QAccessible::ObjectReorder, area), 0);
    heard.events.clear();

    // Items in a new order are new buttons: clients hear the old ones go,
    // while they still name the area, then the area's new children.
    ASSERT_TRUE(dock({dockRun(QStringLiteral("Deploy"), QStringLiteral("deploy"))}).isEmpty());
    EXPECT_EQ(QAccessible::accessibleInterface(buildId), nullptr);
    EXPECT_EQ(QAccessible::accessibleInterface(deployId), nullptr);
    const auto removed = std::count_if(heard.events.cbegin(), heard.events.cend(), [](const auto& e) {
        return e.type == QAccessible::ObjectDestroyed && e.valid;
    });
    EXPECT_EQ(removed, 2);
    EXPECT_EQ(heard.count(QAccessible::ObjectReorder, area), 1);
    ASSERT_EQ(area->childCount(), 1);
    deploy = area->child(0);
    ASSERT_NE(deploy, nullptr);
    EXPECT_EQ(deploy->text(QAccessible::Name), QStringLiteral("Deploy"));
    ASSERT_NE(deploy->actionInterface(), nullptr);
    deploy->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
    ASSERT_EQ(clicks.size(), 2);
    EXPECT_EQ(clicks[1].click[QStringLiteral("item")], QStringLiteral("deploy"));

    // Without items, the area is text again.
    heard.events.clear();
    ASSERT_TRUE(dock({dockRun(QStringLiteral("Deploy done"))}).isEmpty());
    EXPECT_EQ(area->role(), QAccessible::StaticText);
    EXPECT_EQ(heard.count(QAccessible::RoleChanged, area), 1);
    ASSERT_TRUE(dock({dockRun(QStringLiteral("Deploy"), QStringLiteral("deploy"))}).isEmpty());
    ASSERT_EQ(area->childCount(), 1);
    const QAccessible::Id lastId = QAccessible::uniqueId(area->child(0));

    // The buttons go with their dock, and before it: their removal names
    // the area that the cache still holds, so no freed area is registered
    // again under a new id.
    heard.events.clear();
    heard.parentsOfRemoved.clear();
    window->clearScriptOwner(QStringLiteral("a@1"));
    EXPECT_EQ(QAccessible::accessibleInterface(lastId), nullptr);
    EXPECT_EQ(QAccessible::accessibleInterface(areaId), nullptr);
    ASSERT_FALSE(heard.parentsOfRemoved.isEmpty());
    for (const QAccessible::Id id : heard.parentsOfRemoved) {
        EXPECT_EQ(id, areaId);
        EXPECT_EQ(QAccessible::accessibleInterface(id), nullptr);
    }
    // At once: the window's children are fewer before any frame.
    EXPECT_EQ(heard.count(QAccessible::ObjectReorder, root), 1);
    EXPECT_EQ(findDock(), nullptr);
}
TEST_F(NativeWindow, NotificationReplacementAndPaneScopedImageCache) {
    RegisteredNotificationServer notices;
    ASSERT_TRUE(notices.registered);
    auto& server = notices.server;
    auto* first = window->focusedPane();
    AfterglowVt::NotificationRequest request;
    request.id = "replacement";
    request.expireMs = 200;
    request.iconCacheId = "shared";
    QImage image(1, 1024, QImage::Format_RGBA8888); image.fill(Qt::red);
    QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); ASSERT_TRUE(image.save(&buffer, "PNG"));
    request.iconData.assign(png.constData(), std::size_t(png.size()));
    auto& center = NotificationCenter::instance();
    center.handle(first, request);
    ASSERT_TRUE(waitFor([&] { return server.count == 1; }));
    EXPECT_EQ(server.appName, QStringLiteral("Afterglow"));
    EXPECT_EQ(server.appIcon, QStringLiteral(AFTERGLOW_APP_ID));
    EXPECT_EQ(server.desktopEntry, QStringLiteral(AFTERGLOW_APP_ID));
    EXPECT_FALSE(server.images.last().isEmpty());
    EXPECT_LE(server.images.last().height(), 512);
    request.expireMs = 2000;
    request.iconData.clear();
    center.handle(first, request);
    ASSERT_TRUE(waitFor([&] { return server.count == 2; }));
    EXPECT_FALSE(server.images.last().isEmpty());
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Down, launch, nullptr);
    ASSERT_NE(second, nullptr);
    request.id = "another-pane";
    center.handle(second, request);
    ASSERT_TRUE(waitFor([&] { return server.count == 3; }));
    EXPECT_TRUE(server.images.last().isEmpty());
    // Past the 200 ms expiry of the notice that was replaced. A close would
    // come before the next notice: the calls of one connection arrive in
    // the order sent.
    drain(250);
    request.id = "barrier";
    center.handle(second, request);
    ASSERT_TRUE(waitFor([&] { return server.count == 4; }));
    EXPECT_EQ(server.closes, 0);
}
// Shows @p window full screen for the runner's compositor input, whose
// pointer goes to screen positions: a Wayland client does not know where its
// window is. The first drag of a process lists the image formats that Qt can
// convert, and that loads every image plugin: seconds with a cold page cache,
// on the GUI thread, while the drag runs. They load here, before it.
[[nodiscard]] bool showFullScreenForDrag(QWindow* window) {
    window->showFullScreen();
    if (!waitFor([&] { return window->isExposed() && window->size() == window->screen()->size(); })) return false;
    QImageReader::supportedMimeTypes();
    QImageWriter::supportedMimeTypes();
    return true;
}
// Asks the runner (native_window_acceptance.py) for a drag from @p from to
// @p to, positions in @p window. With @p awaitHover, the runner releases
// only once the test creates @p ready + ".hovered", as a person waits for
// the cursor: a slow first frame must not race the release.
[[nodiscard]] bool writeDragMarker(const QString& ready, QWindow* window, QPointF from, QPointF to, bool awaitHover) {
    // The runner's pointer goes to screen positions: the window must fill the screen.
    if (window->size() != window->screen()->size()) return false;
    QFile marker(ready);
    if (!marker.open(QIODevice::WriteOnly)) return false;
    const QPoint a = window->mapToGlobal(from.toPoint()), b = window->mapToGlobal(to.toPoint());
    marker.write(QByteArray::number(a.x()) + " " + QByteArray::number(a.y()) + " " + QByteArray::number(b.x()) + " "
                 + QByteArray::number(b.y()) + (awaitHover ? " 1" : ""));
    return true;
}
class DragSender : public QObject {
public:
    QUrl url;
    QString pressedMarker;
    bool started = false, completed = false;
    Qt::DropAction result = Qt::IgnoreAction;
    bool eventFilter(QObject* target, QEvent* event) override {
        if (!started && event->type() == QEvent::MouseButtonPress) {
            started = true;
            QFile marker(pressedMarker);
            if (marker.open(QIODevice::WriteOnly)) marker.close();
            QDrag drag(target);
            auto* mime = new QMimeData;
            mime->setUrls({url});
            drag.setMimeData(mime);
            result = drag.exec(Qt::CopyAction);
            completed = true;
            return true;
        }
        return false;
    }
};
TEST_F(NativeWindow, NativeWaylandFileDrop) {
    REQUIRE_NATIVE_DND(ready);
    ASSERT_TRUE(showFullScreenForDrag(window));
    const QString path = directory.filePath(QString::fromUtf8("drag's café.txt"));
    QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.close();
    DragSender sender; sender.url = QUrl::fromLocalFile(path);
    sender.pressedMarker = ready + QStringLiteral(".pressed");
    window->installEventFilter(&sender);
    ASSERT_TRUE(writeDragMarker(ready, window, QPoint(window->width() / 4, window->height() / 3),
                                QPoint(window->width() * 3 / 4, window->height() * 2 / 3), false));
    waitFor([&] { return sender.completed; }, 15000);
    window->removeEventFilter(&sender);
    EXPECT_TRUE(sender.started);
    ASSERT_TRUE(sender.completed);
    EXPECT_EQ(sender.result, Qt::CopyAction);
    ASSERT_TRUE(window->focusedPane()->sendInput("\n"));
    waitFor([&] { return screenOf(window->focusedPane()).contains(QString::fromUtf8("café.txt")); });
    const auto text = screenOf(window->focusedPane());
    EXPECT_TRUE(text.contains(QString::fromUtf8("café.txt"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("'\\''"))) << text.toStdString();
}

class PressMarker : public QObject {
public:
    QString path;
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::MouseButtonPress) { QFile marker(path); if (marker.open(QIODevice::WriteOnly)) marker.close(); }
        return false;
    }
};
TEST_F(NativeWindow, FileTransferPermissionFollowsMovedPane) {
    auto* original = window->focusedPane();
    PaneLaunchOptions launch; launch.command = {QStringLiteral("/bin/cat")};
    ASSERT_NE(window->openTab(launch, nullptr), nullptr);
    WindowOptions options; options.command = {QStringLiteral("/bin/cat")};
    auto* destination = application->newWindow(options); ASSERT_NE(destination, nullptr);
    destination->show();
    ASSERT_TRUE(window->movePaneTo(original, destination, destination->focusedPane(), SplitTree::Direction::Right).isEmpty());
    // A send asks at its first destination, which the prompt lists.
    const QString destinationPath = directory.filePath(QStringLiteral("sent"));
    original->fileTransfers()->handle("ac=send;id=moved");
    // A directory: an approval would make it at once, so its absence shows that nothing ran before the answer.
    original->fileTransfers()->handle("ac=file;id=moved;fid=f;ft=directory;n=" + destinationPath.toUtf8().toBase64());
    QList<QPointer<QMessageBox>> prompts;
    const auto rejectAll = qScopeGuard([&prompts] {
        for (const auto& box : prompts)
            if (box && box->testAttribute(Qt::WA_DeleteOnClose)) box->reject();
        drainDeferredDeletes();
    });
    waitFor([&] {
        prompts.clear();
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible() && box->windowTitle() == QStringLiteral("File transfer")) prompts.append(box);
        return !prompts.isEmpty();
    });
    ASSERT_EQ(prompts.size(), 1);
    ASSERT_NE(prompts.front()->windowHandle(), nullptr);
    EXPECT_EQ(prompts.front()->windowHandle()->transientParent(), destination);
    EXPECT_TRUE(prompts.front()->text().contains(destinationPath)) << qPrintable(prompts.front()->text());
    drain(50);
    EXPECT_FALSE(QFileInfo::exists(destinationPath));
    // The window's Yes reaches the transfer, which makes the directory it approved.
    ASSERT_TRUE(prompts.front()) << "the prompt went";
    prompts.front()->button(QMessageBox::Yes)->click();
    EXPECT_TRUE(waitFor([&] { return QFileInfo(destinationPath).isDir(); }));
}

// The drag and drop tests of kitten dnd and their twins with aglow dnd, which
// takes the same options. All of them need the real compositor input of
// native_window_acceptance.py. The kitten tests skip without kitten, and the
// aglow twins skip in a build without aglow.

// Opens a tab with @p sourceCommand and a split to its right with
// @p receiverCommand, shows the window full screen and waits until each
// client has sent its first command.
void NativeWindow::openDragClients(const QStringList& sourceCommand, const QStringList& receiverCommand, ClientDrag& drag) {
    PaneLaunchOptions options;
    options.command = sourceCommand;
    // kitten makes a temporary directory where it runs, and aglow stages a
    // drop in its drop directory: keep both in the fixture's directory.
    options.cwd = directory.path();
    drag.source = window->openTab(options, nullptr);
    ASSERT_NE(drag.source, nullptr);
    PaneLaunchOptions receiverOptions;
    receiverOptions.command = receiverCommand;
    receiverOptions.cwd = directory.path();
    drag.receiver = window->splitPane(drag.source, SplitTree::Direction::Right, receiverOptions, nullptr);
    ASSERT_NE(drag.receiver, nullptr);
    // The receiver's side from its first command, for a drop that fails after the drag completed.
    QObject::connect(drag.receiver->dragDrop(), &DragDrop::reply, &drag.listener, [&drag](const QByteArray& bytes) { drag.dropped += bytes; });
    QObject::connect(drag.receiver, &Pane::terminalProtocolRequested, &drag.listener, [&drag](int code, const QByteArray& bytes) {
        if (code == 72) drag.dropped += "RECEIVER: " + bytes + "\n";
    });
    ASSERT_TRUE(showFullScreenForDrag(window));
    // Both clients start at once: wait for each one's command.
    waitFor([&] { return drag.source->dragDrop()->offersDrags() && drag.receiver->dragDrop()->acceptsDrops(); }, 5000);
    ASSERT_TRUE(drag.source->dragDrop()->offersDrags()) << screenOf(drag.source).toStdString();
    ASSERT_TRUE(drag.receiver->dragDrop()->acceptsDrops()) << drag.dropped.toStdString()
        << screenOf(drag.receiver).toStdString();
}

// Drags with the runner's compositor input from the left quarter of the
// window, in the source pane, to the right quarter, in the receiver, and
// waits until the source's drag ends. The runner releases when @p accepted
// is true, by default when the receiver accepts the drag.
void NativeWindow::runClientDrag(const QString& ready, ClientDrag& drag, const std::function<bool()>& accepted) {
    QObject::connect(drag.source->dragDrop(), &DragDrop::reply, &drag.listener, [&drag](const QByteArray& bytes) {
        drag.protocol += bytes;
        const auto keys = dndKeys(bytes);
        if (keys.contains("t=e") && keys.contains("x=4")) {
            drag.complete = true;
            drag.end = keys;
        }
        if (keys.contains("t=k")) ++drag.treeRequests;
    });
    QObject::connect(drag.source, &Pane::terminalProtocolRequested, &drag.listener, [&drag](int code, const QByteArray& bytes) {
        if (code == 72) drag.protocol += "CLIENT: " + bytes + "\n";
    });
    PressMarker input; input.path = ready + QStringLiteral(".pressed"); window->installEventFilter(&input);
    const auto removeInput = qScopeGuard([&] { window->removeEventFilter(&input); });
    // The runner releases only after the receiver's program has accepted the drag.
    ASSERT_TRUE(writeDragMarker(ready, window, QPoint(window->width() / 4, window->height() / 3),
                                QPoint(window->width() * 3 / 4, window->height() * 2 / 3), true));
    // A timer, because the drag runs in QDrag::exec's own event loop.
    QTimer hover;
    QObject::connect(&hover, &QTimer::timeout, &drag.listener, [&] {
        if (accepted ? !accepted() : !drag.receiver->dragDrop()->acceptsDrag()) return;
        QFile hovered(ready + QStringLiteral(".hovered"));
        if (hovered.open(QIODevice::WriteOnly)) hover.stop();
    });
    hover.start(5);
    waitFor([&] { return drag.complete; }, 15000);
    hover.stop();
    EXPECT_TRUE(drag.complete) << drag.protocol.toStdString() << screenOf(drag.source).toStdString();
}

QByteArray fileContent(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// CRT Curve's bend at curvature @p k, as its effect.frag has it: where the
// content shown at @p at on @p canvas comes from (both logical pixels).
QPointF crtSource(const QRectF& canvas, const QPointF& at, double k) {
    const double cx = (at.x() - canvas.left()) / canvas.width() * 2 - 1, cy = (at.y() - canvas.top()) / canvas.height() * 2 - 1;
    const double f = 1 + k * (cx * cx + cy * cy);
    return {canvas.left() + (cx * f * 0.5 + 0.5) * canvas.width(), canvas.top() + (cy * f * 0.5 + 0.5) * canvas.height()};
}
// A program that turns on SGR mouse reports and writes what it gets to @p file.
QStringList mouseReporter(const QString& file) {
    return {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
            QStringLiteral("stty raw -echo; printf '\\033[?1000h\\033[?1006h'; exec cat > \"$0\""), file};
}
bool reportsMouse(Pane* pane) {
    return pane->withTerminal([](AfterglowVt::Terminal& t) { return t.modes().mouseTracking != AfterglowVt::MouseTracking::None; });
}
// A left click at @p at, and the 1-based column of the press that the
// program writing @p reports got: after its release has come too, so that
// the next click's reports start after both.
int reportedColumn(QWindow* window, const QString& reports, const QPointF& at) {
    const qsizetype before = fileContent(reports).size();
    clickLeft(window, at);
    const bool released = waitFor([&] { return fileContent(reports).mid(before).endsWith('m'); });
    EXPECT_TRUE(released) << "no release report";
    const QByteArray got = fileContent(reports).mid(before);
    const qsizetype press = got.indexOf("\x1b[<0;");
    if (press < 0) return -1;
    const auto parts = got.mid(press + 4).split(';');
    return parts.size() >= 2 ? parts[1].toInt() : -1;
}

TEST_F(NativeWindow, AClickOnABentPaneReachesTheCellDrawnThere) {
    // CRT Curve bends each pane: content near the edges is drawn closer to
    // the centre, so a click there must reach a column further out. Its
    // pointer map (pPointerMap) moves the click the same way.
    constexpr double Curvature = 0.3;
    const QString reports = directory.filePath(QStringLiteral("reports"));
    restartApplication(QStringLiteral("crt.conf"),
        testSettings({{QStringLiteral("Shaders"), QJsonObject{{QStringLiteral("pane"), QJsonArray{QStringLiteral("crt-curve")}},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("crt-curve"), QJsonObject{{QStringLiteral("curvature"), Curvature}}}}}}}}),
        mouseReporter(reports));
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    ASSERT_NE(pane, nullptr);
    ASSERT_TRUE(waitFor([&] { return reportsMouse(pane); }));
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    // One pane fills the panes' area: the pane chain's canvas.
    expectIdle(window);
    const QRectF canvas = chrome->paneArea();
    const QRectF grid = gridOf(window, pane);
    const QRectF cell = window->accessibleCellRect(pane, {0, 0});
    ASSERT_GT(cell.width(), 0);
    auto column = [&](double x) { return int(std::floor((x - grid.left()) / cell.width())) + 1; };
    // The centre does not move. Towards the right edge, the drawn column is
    // the bent one, well past the one that the layout has there.
    for (const double share : {0.5, 0.8, 0.88}) {
        const QPointF at(grid.left() + grid.width() * share, grid.center().y());
        const int drawn = reportedColumn(window, reports, at);
        const int expected = std::min(column(crtSource(canvas, at, Curvature).x()), pane->cols());
        EXPECT_NEAR(drawn, expected, 1) << "at " << share;
        if (share > 0.8) {
            EXPECT_GT(drawn, column(at.x()) + 2);
        }
    }
}

// Where CRT Curve at curvature @p k draws the content of @p source on
// @p canvas: the inverse of crtSource(), by fixed-point iteration.
QPointF crtShown(const QRectF& canvas, const QPointF& source, double k) {
    const double sx = (source.x() - canvas.left()) / canvas.width() * 2 - 1, sy = (source.y() - canvas.top()) / canvas.height() * 2 - 1;
    double cx = sx, cy = sy;
    for (int i = 0; i < 100; ++i) {
        const double f = 1 + k * (cx * cx + cy * cy);
        cx = sx / f;
        cy = sy / f;
    }
    return {canvas.left() + (cx * 0.5 + 0.5) * canvas.width(), canvas.top() + (cy * 0.5 + 0.5) * canvas.height()};
}

TEST_F(NativeWindow, AClickThroughABentWindowReachesThePaneDrawnThere) {
    // CRT Curve over the whole window (the window chain) bends the panes
    // together: a pane's content can show where the layout has its
    // neighbour. A click goes to the pane drawn under it, at the drawn cell.
    constexpr double Curvature = 0.3;
    const QString leftReports = directory.filePath(QStringLiteral("left"));
    restartApplication(QStringLiteral("crt-window.conf"),
        testSettings({{QStringLiteral("Shaders"), QJsonObject{{QStringLiteral("window"), QJsonArray{QStringLiteral("crt-curve")}},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("crt-curve"), QJsonObject{{QStringLiteral("curvature"), Curvature}}}}}}}}),
        mouseReporter(leftReports));
    if (HasFatalFailure()) return;
    Pane* left = window->focusedPane();
    ASSERT_NE(left, nullptr);
    // Three panes: the left half, and two quarters at the right, so the
    // middle divider is off the centre, where the bend moves it.
    const QString middleReports = directory.filePath(QStringLiteral("middle"));
    const QString rightReports = directory.filePath(QStringLiteral("right"));
    PaneLaunchOptions launch;
    launch.command = mouseReporter(middleReports);
    Pane* middle = window->splitPane(left, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(middle, nullptr);
    launch.command = mouseReporter(rightReports);
    Pane* right = window->splitPane(middle, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_TRUE(waitFor([&] { return reportsMouse(left) && reportsMouse(middle) && reportsMouse(right); }));
    expectIdle(window);
    // The window chain's canvas is the whole window.
    const QRectF canvas(QPointF(0, 0), QSizeF(window->size()));
    const QRectF middleGrid = gridOf(window, middle), rightGrid = gridOf(window, right);
    const QRectF cell = window->accessibleCellRect(right, {0, 0});
    ASSERT_GT(cell.width(), 0);
    // In the middle pane's layout, near the divider, high up: the bend
    // shows the right pane's left columns there.
    const QPointF at(middleGrid.right() - cell.width(), canvas.top() + canvas.height() * 0.2);
    const QPointF source = crtSource(canvas, at, Curvature);
    ASSERT_GT(source.x(), rightGrid.left() + cell.width()) << "the bend must reach past the divider";
    const qsizetype middleBefore = fileContent(middleReports).size();
    const int drawn = reportedColumn(window, rightReports, at);
    EXPECT_NEAR(drawn, int(std::floor((source.x() - rightGrid.left()) / cell.width())) + 1, 1);
    EXPECT_EQ(window->focusedPane(), right);
    // The middle pane got nothing: its program echoes a marker afterwards.
    ASSERT_TRUE(middle->sendInput("\x1b[<0;0;0Mdone"));
    ASSERT_TRUE(waitFor([&] { return fileContent(middleReports).mid(middleBefore).contains("done"); }));
    EXPECT_EQ(fileContent(middleReports).mid(middleBefore), QByteArray("\x1b[<0;0;0Mdone"));
}

TEST_F(NativeWindow, APaneDockIsClickedWhereTheWindowChainDrawsIt) {
    // A pane's dock is drawn through the window chain: under CRT Curve, the
    // dock at the top of the pane shows well inside the window, and a click
    // there reaches its item.
    constexpr double Curvature = 0.3;
    restartApplication(QStringLiteral("crt-dock.conf"),
        testSettings({{QStringLiteral("Shaders"), QJsonObject{{QStringLiteral("window"), QJsonArray{QStringLiteral("crt-curve")}},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("crt-curve"), QJsonObject{{QStringLiteral("curvature"), Curvature}}}}}}}}),
        {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    ASSERT_NE(pane, nullptr);
    DockEvents events(window);
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"),
        dockEffect(QStringLiteral("title"), QStringLiteral("top"), 1, {dockRun(QStringLiteral("title"), QStringLiteral("t"))},
                   QStringLiteral("pane")), pane).isEmpty());
    const DockArea* title = nullptr;
    ASSERT_TRUE(waitFor([&] {
        title = shownDock(window, "title", pane->id());
        return title && !title->items().empty();
    }));
    expectIdle(window);
    const qreal dpr = window->devicePixelRatio();
    const QRectF canvas(QPointF(0, 0), QSizeF(window->size()));
    const QPointF item = QRectF(title->itemRect(0)).center() / dpr;
    const QPointF drawn = crtShown(canvas, item, Curvature);
    // Far from the layout's place: the layout has the pane's text there.
    ASSERT_FALSE(QRectF(title->rect()).contains(drawn * dpr)) << "the bend must move the dock out of its own place";
    clickLeft(window, drawn);
    ASSERT_EQ(events.of("on_dock_click").size(), 1);
    EXPECT_EQ(events.of("on_dock_click")[0].event[QStringLiteral("item")], QStringLiteral("t"));
    EXPECT_FALSE(pane->hasSelection());
}

// One notch of the wheel at @p at (logical), away from the user.
void wheelAt(QWindow* window, const QPointF& at) {
    QWheelEvent event(at, window->mapToGlobal(at), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window, &event);
}

// A text drag from one client's pane to another client's pane.
void NativeWindow::dragAndDropClient(const QString& client, const QString& ready) {
    const QString source = directory.filePath(QStringLiteral("source.txt"));
    const QString destination = directory.filePath(QStringLiteral("dropped.txt"));
    // The source takes drops too, into a file of its own: a drop that the
    // wrong pane took writes that file.
    const QString sourceDrop = directory.filePath(QStringLiteral("dropped-on-source.txt"));
    const QByteArray text("Native Kitty drag and drop\n");
    QFile file(source); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.write(text); file.close();
    ClientDrag drag;
    openDragClients({client, QStringLiteral("dnd"), QStringLiteral("--drag"), QStringLiteral("text/plain:") + source,
                     QStringLiteral("--drop"), QStringLiteral("text/plain:") + sourceDrop, QStringLiteral("--drop-anywhere=copy")},
                    {client, QStringLiteral("dnd"), QStringLiteral("--drop"), QStringLiteral("text/plain:") + destination,
                     QStringLiteral("--drop-anywhere=copy")}, drag);
    if (HasFatalFailure()) return;
    runClientDrag(ready, drag);
    if (HasFatalFailure()) return;
    // The file exists before the receiver has written all of it.
    EXPECT_TRUE(waitFor([&] { return fileContent(destination) == text; }, 5000))
        << drag.protocol.toStdString() << "\n" << drag.dropped.toStdString()
        << screenOf(drag.receiver).toStdString();
    EXPECT_FALSE(QFileInfo::exists(sourceDrop)) << "the source pane took the drop";
}

// A directory dragged from a client that the terminal takes for a remote
// one, so that the tree crosses the protocol (t=k), to another client.
void NativeWindow::remoteDragDirectory(const QString& client, const QString& ready, bool move) {
    const QString source = directory.filePath(QStringLiteral("remote"));
    const QString destination = directory.filePath(QStringLiteral("received"));
    ASSERT_TRUE(QDir().mkpath(source + QStringLiteral("/nested")));
    ASSERT_TRUE(QDir().mkpath(destination));
    const QByteArray data("remote file data");
    QFile file(source + QStringLiteral("/nested/file")); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.write(data); file.close();
    QFile empty(source + QStringLiteral("/empty")); ASSERT_TRUE(empty.open(QIODevice::WriteOnly)); empty.close();
    ASSERT_EQ(::symlink("nested/file", QFile::encodeName(source + QStringLiteral("/link")).constData()), 0);
    ClientDrag drag;
    openDragClients({client, QStringLiteral("dnd"), source},
                    {client, QStringLiteral("dnd"), QStringLiteral("--drop-dest"), destination,
                     move ? QStringLiteral("--drop-anywhere=move") : QStringLiteral("--drop-anywhere=copy")},
                    drag);
    if (HasFatalFailure()) return;
    // A machine id that is not this computer's: the source is remote.
    drag.source->dragDrop()->handle("t=o:x=1;1:remote-test-machine");
    runClientDrag(ready, drag);
    if (HasFatalFailure()) return;
    // A source that the terminal took for a local one gives no requests,
    // and its drop of the local path passes the checks below as well.
    EXPECT_GT(drag.treeRequests, 0) << drag.protocol.toStdString();
    const QString root = destination + QStringLiteral("/remote");
    // The receiver writes the tree in steps: wait for every part of it.
    EXPECT_TRUE(waitFor([&] {
        return fileContent(root + QStringLiteral("/nested/file")) == data && QFileInfo::exists(root + QStringLiteral("/empty"))
            && QFileInfo(root + QStringLiteral("/link")).isSymLink();
    }, 5000)) << drag.protocol.toStdString() << "\n" << drag.dropped.toStdString()
              << screenOf(drag.receiver).toStdString();
    EXPECT_EQ(fileContent(root + QStringLiteral("/nested/file")), data);
    EXPECT_EQ(QFileInfo(root + QStringLiteral("/empty")).size(), 0);
    EXPECT_EQ(QFileInfo(root + QStringLiteral("/link")).readSymLink(), QStringLiteral("nested/file"));
    if (!move) return;
    // A move between two panes stays a move: this process took the drop, so
    // the terminal knows its action, and the source removes its files.
    EXPECT_TRUE(drag.end.contains("o=2") && drag.end.contains("y=0")) << drag.protocol.toStdString();
    EXPECT_TRUE(waitFor([&] { return !QFileInfo::exists(source + QStringLiteral("/nested/file")); }, 5000))
        << "the client kept its files after a move between panes";
}

// A drop target in a process of its own. Qt gives a drop between two windows
// of one process to the target inside that process, so the drag tests above
// never see the action that the compositor negotiates. A drop on another
// client does. The target is a layer surface on the right half of the output,
// above the full-screen window, where runClientDrag drops. It takes every
// drag with one action and reads no data: Qt would read text/uri-list through
// the document portal, which the private session bus would start. With
// @p refuse it accepts the hover with the action and refuses the drop.
class DropTarget : public QRasterWindow {
public:
    DropTarget(Qt::DropAction action, bool refuse, QString report)
        : m_action(action), m_refuse(refuse), m_report(std::move(report)) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter(this).fillRect(QRect(QPoint(), size()), QColor(40, 90, 120));
        QFile shown(m_report + QStringLiteral(".shown"));
        if (shown.open(QIODevice::WriteOnly)) shown.close();
    }
    bool event(QEvent* event) override {
        switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            // The preferred action of the offer: the compositor picks it,
            // because the source allows it.
            auto* move = static_cast<QDragMoveEvent*>(event);
            move->setDropAction(m_action);
            move->accept();
            return true;
        }
        case QEvent::Drop: {
            auto* drop = static_cast<QDropEvent*>(event);
            const Qt::DropAction taken = m_refuse ? Qt::IgnoreAction : m_action;
            if (m_refuse) {
                drop->ignore();
            } else {
                drop->setDropAction(m_action);
                drop->accept();
            }
            QFile dropped(m_report + QStringLiteral(".dropped"));
            if (dropped.open(QIODevice::WriteOnly)) dropped.write(QByteArray::number(int(taken)));
            return true;
        }
        default:
            return QRasterWindow::event(event);
        }
    }

private:
    Qt::DropAction m_action;
    bool m_refuse = false;
    QString m_report;
};

// The drop target process: --drop-target copy|move|refuse-move REPORT. It
// writes REPORT.shown when it is on the screen, and REPORT.dropped, with the
// action that it took (0 for a refused drop), when a drop comes.
int runDropTarget(const QStringList& arguments) {
    const QStringList modes {QStringLiteral("copy"), QStringLiteral("move"), QStringLiteral("refuse-move")};
    if (arguments.size() != 4 || !modes.contains(arguments[2])) {
        std::fprintf(stderr, "usage: --drop-target copy|move|refuse-move REPORT\n");
        return 2;
    }
    using PhosphorWayland::LayerSurface;
    const bool copy = arguments[2] == QLatin1String("copy");
    DropTarget target(copy ? Qt::CopyAction : Qt::MoveAction, arguments[2] == QLatin1String("refuse-move"), arguments[3]);
    const QSize output = QGuiApplication::primaryScreen()->size();
    auto* surface = LayerSurface::get(&target);
    surface->setLayer(LayerSurface::LayerOverlay);
    surface->setAnchors(LayerSurface::Anchors(LayerSurface::AnchorTop | LayerSurface::AnchorBottom | LayerSurface::AnchorRight));
    surface->setKeyboardInteractivity(LayerSurface::KeyboardInteractivityNone);
    surface->setScope(QStringLiteral("afterglow-native-drop-target"));
    surface->setExclusiveZone(0);
    surface->setDesiredSize(QSize(output.width() / 2, 0));
    target.resize(output.width() / 2, output.height());
    target.show();
    return QCoreApplication::exec();
}

// A directory dragged from a client that the terminal takes for a remote one
// to the drop target of another process, which takes it with @p action. The
// terminal reports a copy to the client, which keeps its files, whatever the
// target did: KWin reports a drop that the target refused as finished, so a
// move cannot be told from it. With @p refuse the target accepts the hover as
// a move and refuses the drop.
void NativeWindow::remoteDragToWaylandTarget(const QString& client, const QString& ready, Qt::DropAction action, bool refuse) {
    const QString source = directory.filePath(QStringLiteral("remote"));
    ASSERT_TRUE(QDir().mkpath(source + QStringLiteral("/nested")));
    const QByteArray data("remote file data");
    QFile file(source + QStringLiteral("/nested/file")); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.write(data); file.close();
    const QString report = directory.filePath(QStringLiteral("drop-target"));
    QProcess target;
    target.setProcessChannelMode(QProcess::ForwardedChannels);
    target.start(QCoreApplication::applicationFilePath(),
                 {QStringLiteral("--drop-target"),
                  refuse ? QStringLiteral("refuse-move") : action == Qt::MoveAction ? QStringLiteral("move") : QStringLiteral("copy"),
                  report});
    const auto stopTarget = qScopeGuard([&target] {
        target.terminate();
        if (!target.waitForFinished(5000)) target.kill();
    });
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(report + QStringLiteral(".shown")); }, 10000))
        << "the drop target did not show: " << target.errorString().toStdString();
    // The client exits after a completed drag, which is after it removed the
    // files of a move. The marker follows its exit.
    const QString exited = directory.filePath(QStringLiteral("client-exited"));
    // The terminal's last x=2 action. Made before the drag, whose listener
    // writes it.
    QByteArray operation;
    ClientDrag drag;
    PaneLaunchOptions options;
    options.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                       QStringLiteral("\"$0\" dnd --exit-on=drag-finish \"$1\"; : > \"$2\"; exec cat"), client, source, exited};
    options.cwd = directory.path();
    drag.source = window->openTab(options, nullptr);
    ASSERT_NE(drag.source, nullptr);
    ASSERT_TRUE(showFullScreenForDrag(window));
    waitFor([&] { return drag.source->dragDrop()->offersDrags(); }, 5000);
    ASSERT_TRUE(drag.source->dragDrop()->offersDrags()) << screenOf(drag.source).toStdString();
    // A machine id that is not this computer's: the source is remote.
    drag.source->dragDrop()->handle("t=o:x=1;1:remote-test-machine");
    QObject::connect(drag.source->dragDrop(), &DragDrop::reply, &drag.listener, [&](const QByteArray& bytes) {
        const auto keys = dndKeys(bytes);
        if (keys.contains("t=e") && keys.contains("x=2"))
            for (const auto& key : keys)
                if (key.startsWith("o=")) operation = key.mid(2);
    });
    const QByteArray offered = QByteArray::number(int(action));
    // What the target did, and what the terminal reports: a move to another
    // client is a copy for the source, because KWin reports a refused drop
    // as finished.
    const QByteArray taken = refuse ? QByteArray("0") : offered;
    const QByteArray reported = QByteArray::number(int(Qt::CopyAction));
    // Release only after the compositor gave the source the target's action.
    runClientDrag(ready, drag, [&] { return operation == offered; });
    if (HasFatalFailure()) return;
    EXPECT_GT(drag.treeRequests, 0) << drag.protocol.toStdString();
    EXPECT_TRUE(drag.end.contains("y=0")) << drag.protocol.toStdString();
    EXPECT_TRUE(drag.end.contains("o=" + reported)) << drag.protocol.toStdString();
    EXPECT_TRUE(waitFor([&] { return QFileInfo::exists(report + QStringLiteral(".dropped")); }, 5000))
        << "the drop target got no drop";
    EXPECT_EQ(fileContent(report + QStringLiteral(".dropped")), taken);
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(exited); }, 5000))
        << drag.protocol.toStdString() << screenOf(drag.source).toStdString();
    EXPECT_EQ(fileContent(source + QStringLiteral("/nested/file")), data) << "the client removed its files";
}
TEST_F(NativeWindow, KittyDragAndDropClient) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_KITTEN();
    dragAndDropClient(QStringLiteral("kitten"), ready);
}
TEST_F(NativeWindow, AglowDragAndDropClient) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_AGLOW(aglow);
    dragAndDropClient(aglow, ready);
}
TEST_F(NativeWindow, KittyRemoteDragDirectory) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_KITTEN();
    remoteDragDirectory(QStringLiteral("kitten"), ready);
}
TEST_F(NativeWindow, AglowRemoteDragDirectory) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_AGLOW(aglow);
    remoteDragDirectory(aglow, ready);
}
TEST_F(NativeWindow, KittyRemoteDragDirectoryMovesBetweenPanes) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_KITTEN();
    remoteDragDirectory(QStringLiteral("kitten"), ready, true);
}
TEST_F(NativeWindow, AglowRemoteDragDirectoryMovesBetweenPanes) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_AGLOW(aglow);
    remoteDragDirectory(aglow, ready, true);
}
TEST_F(NativeWindow, KittyRemoteDragMoveToWaylandTarget) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_KITTEN();
    remoteDragToWaylandTarget(QStringLiteral("kitten"), ready, Qt::MoveAction);
}
TEST_F(NativeWindow, KittyRemoteDragCopyToWaylandTarget) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_KITTEN();
    remoteDragToWaylandTarget(QStringLiteral("kitten"), ready, Qt::CopyAction);
}
TEST_F(NativeWindow, AglowRemoteDragMoveToWaylandTarget) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_AGLOW(aglow);
    remoteDragToWaylandTarget(aglow, ready, Qt::MoveAction);
}
TEST_F(NativeWindow, AglowRemoteDragCopyToWaylandTarget) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_AGLOW(aglow);
    remoteDragToWaylandTarget(aglow, ready, Qt::CopyAction);
}
TEST_F(NativeWindow, KittyRemoteDragRefusedByAWaylandTargetKeepsTheFiles) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_KITTEN();
    remoteDragToWaylandTarget(QStringLiteral("kitten"), ready, Qt::MoveAction, true);
}
TEST_F(NativeWindow, AglowRemoteDragRefusedByAWaylandTargetKeepsTheFiles) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_AGLOW(aglow);
    remoteDragToWaylandTarget(aglow, ready, Qt::MoveAction, true);
}

// kitten clipboard, the client that kitty users bring, against the window's
// answers to OSC 5522: the desktop's clipboard, several MIME types, the read
// prompt and the size limit of a read. They need no compositor input.

// What a run of kitten clipboard left: its exit status and its standard error.
struct KittenRun
{
    int status = -1;
    QByteArray errors;
};

// Starts `kitten clipboard ARGUMENTS` in a split of @p window, in @p cwd,
// with @p input on its standard input. finishKitten waits for its end.
Pane* startKittenClipboard(TerminalWindow* window, const QString& cwd, const QStringList& arguments, const QByteArray& input = {}) {
    QFile in(cwd + QStringLiteral("/kitten.in"));
    if (!in.open(QIODevice::WriteOnly | QIODevice::Truncate) || in.write(input) != input.size()) return nullptr;
    in.close();
    QFile::remove(cwd + QStringLiteral("/kitten.status"));
    PaneLaunchOptions launch;
    launch.command = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                 QStringLiteral("kitten clipboard \"$@\" <kitten.in 2>kitten.err; echo $? >kitten.status.new; "
                                                "mv kitten.status.new kitten.status"),
                                 QStringLiteral("sh")} + arguments;
    launch.cwd = cwd;
    return window->splitPane(window->focusedPane(), SplitTree::Direction::Right, launch, nullptr);
}
KittenRun finishKitten(const QString& cwd) {
    KittenRun run;
    const QString status = cwd + QStringLiteral("/kitten.status");
    if (!waitFor([&] { return QFileInfo::exists(status); }, 10000)) return run;
    bool ok = false;
    run.status = fileContent(status).trimmed().toInt(&ok);
    if (!ok) run.status = -1;
    run.errors = fileContent(cwd + QStringLiteral("/kitten.err"));
    return run;
}

TEST_F(NativeWindow, KittenClipboardWritesTextAndSeveralTypes) {
    REQUIRE_KITTEN();
    const QString cwd = directory.path();
    // Text from its standard input.
    QGuiApplication::clipboard()->setText(QStringLiteral("before"));
    ASSERT_TRUE(waitFor([] { return QGuiApplication::clipboard()->text() == QStringLiteral("before"); }));
    ASSERT_NE(startKittenClipboard(window, cwd, {}, "hello from kitten\n"), nullptr);
    KittenRun run = finishKitten(cwd);
    ASSERT_EQ(run.status, 0) << run.errors.toStdString();
    EXPECT_TRUE(waitFor([] { return QGuiApplication::clipboard()->text() == QStringLiteral("hello from kitten\n"); }))
        << QGuiApplication::clipboard()->text().toStdString();
    // Files, each with the type that its name gives.
    const QByteArray note = "a note\n";
    QByteArray image("\x89PNG\r\n\x1a\n", 8);
    for (int i = 0; i < 300; ++i) image += char(i);
    QFile noteFile(cwd + QStringLiteral("/note.txt"));
    QFile imageFile(cwd + QStringLiteral("/picture.png"));
    ASSERT_TRUE(noteFile.open(QIODevice::WriteOnly) && noteFile.write(note) == note.size());
    ASSERT_TRUE(imageFile.open(QIODevice::WriteOnly) && imageFile.write(image) == image.size());
    noteFile.close();
    imageFile.close();
    ASSERT_NE(startKittenClipboard(window, cwd, {QStringLiteral("note.txt"), QStringLiteral("picture.png")}), nullptr);
    run = finishKitten(cwd);
    ASSERT_EQ(run.status, 0) << run.errors.toStdString();
    ASSERT_TRUE(waitFor([] {
        const QMimeData* data = QGuiApplication::clipboard()->mimeData();
        return data && data->hasFormat(QStringLiteral("image/png"));
    }));
    const QMimeData* data = QGuiApplication::clipboard()->mimeData();
    EXPECT_EQ(data->data(QStringLiteral("text/plain")), note);
    EXPECT_EQ(data->data(QStringLiteral("image/png")), image);
}

TEST_F(NativeWindow, KittenClipboardReadsAfterThePromptAllowsIt) {
    REQUIRE_KITTEN();
    const QString cwd = directory.path();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto* offered = new QMimeData;
    offered->setText(QStringLiteral("from the desktop"));
    offered->setHtml(QStringLiteral("<b>bold</b>"));
    QGuiApplication::clipboard()->setMimeData(offered);
    ASSERT_TRUE(waitFor([] { return QGuiApplication::clipboard()->text() == QStringLiteral("from the desktop"); }));
    // Allowed once: kitten writes each type to the file whose name gives it.
    ASSERT_NE(startKittenClipboard(window, cwd, {QStringLiteral("--get-clipboard"), QStringLiteral("out.txt"), QStringLiteral("out.html")}),
              nullptr);
    ASSERT_TRUE(waitFor([&] { return chrome->clipboardPromptOpen(); }, 10000));
    EXPECT_TRUE(chrome->clipboardPromptText().contains(QStringLiteral("text/html"))) << chrome->clipboardPromptText().toStdString();
    chrome->answerClipboard(1);
    KittenRun run = finishKitten(cwd);
    ASSERT_EQ(run.status, 0) << run.errors.toStdString();
    EXPECT_EQ(fileContent(cwd + QStringLiteral("/out.txt")), "from the desktop");
    EXPECT_EQ(fileContent(cwd + QStringLiteral("/out.html")), "<b>bold</b>");
    // Denied: kitten fails and writes nothing.
    ASSERT_NE(startKittenClipboard(window, cwd, {QStringLiteral("--get-clipboard"), QStringLiteral("denied.txt")}), nullptr);
    ASSERT_TRUE(waitFor([&] { return chrome->clipboardPromptOpen(); }, 10000));
    chrome->answerClipboard(0);
    run = finishKitten(cwd);
    EXPECT_NE(run.status, 0);
    // kitten words the EPERM of the answer.
    EXPECT_TRUE(run.errors.contains("permission denied")) << run.errors.toStdString();
    EXPECT_TRUE(fileContent(cwd + QStringLiteral("/denied.txt")).isEmpty());
}

TEST_F(NativeWindow, KittenClipboardReportsAReadOverTheLimit) {
    REQUIRE_KITTEN();
    const QString cwd = directory.path();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    // One byte over the 8 MiB of a reply.
    const QString large(8 * 1024 * 1024 + 1, QLatin1Char('x'));
    QGuiApplication::clipboard()->setText(large);
    ASSERT_TRUE(waitFor([&] { return QGuiApplication::clipboard()->text().size() == large.size(); }));
    ASSERT_NE(startKittenClipboard(window, cwd, {QStringLiteral("--get-clipboard"), QStringLiteral("large.txt")}), nullptr);
    ASSERT_TRUE(waitFor([&] { return chrome->clipboardPromptOpen(); }, 10000));
    chrome->answerClipboard(1);
    const KittenRun run = finishKitten(cwd);
    EXPECT_NE(run.status, 0);
    EXPECT_TRUE(run.errors.contains("EFBIG")) << run.errors.toStdString();
}

// Selected text that drags out of a pane (Mouse.dragSelection), and text that
// drops on a pane. Only ASelectionDragsIntoAnotherPaneAndNotIntoItsOwn needs
// the real compositor input of native_window_acceptance.py. The others send
// their events to the window. A drag here ends soon, as the compositor
// refuses a drag that no real press started: a zero timer acts in QDrag::exec's
// event loop before it does.

// Writes @p line into every row of @p pane.
void fillRows(Pane* pane, const QByteArray& line) {
    QByteArray text = "\x1b[2J\x1b[H";
    for (int row = 0; row < pane->rows(); ++row) text += line + (row + 1 < pane->rows() ? "\r\n" : "");
    pane->withTerminal([&text](AfterglowVt::Terminal& t) { t.feed(std::string_view(text.constData(), std::size_t(text.size()))); });
}
// Selects from cell (@p fromCol, @p fromRow) to cell (@p toCol, @p toRow) of @p pane, as a drag does.
void selectCells(Pane* pane, int fromCol, int fromRow, int toCol, int toRow) {
    pane->startSelection(pane->selectionPointAt(fromCol, fromRow), AfterglowVt::SelectionMode::Normal, true);
    pane->extendSelection(pane->selectionPointAt(toCol, toRow));
}
// One row: a drop of it asks no paste question.
void selectRow(Pane* pane, int row) { selectCells(pane, 0, row, pane->cols() - 1, row); }
void selectWholeScreen(Pane* pane) { selectCells(pane, 0, 0, pane->cols() - 1, pane->rows() - 1); }
// The centre of cell (@p col, @p row) of @p pane, in window logical pixels.
QPointF cellCentre(TerminalWindow* window, Pane* pane, int col, int row) {
    const QRectF grid = gridOf(window, pane);
    return grid.topLeft() + QPointF((col + 0.5) * grid.width() / pane->cols(), (row + 0.5) * grid.height() / pane->rows());
}
// A command for a pane that writes what its program gets to @p path, byte for byte.
QStringList catInto(const QString& path) {
    return {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty raw -echo; exec cat > \"$0\""), path};
}
// A left press with @p modifiers, from @p source.
void pressLeftWith(QWindow* window, const QPointF& at, Qt::KeyboardModifiers modifiers,
                   Qt::MouseEventSource source = Qt::MouseEventNotSynthesized) {
    QMouseEvent event(QEvent::MouseButtonPress, at, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, modifiers, source);
    QCoreApplication::sendEvent(window, &event);
}
// A distance that starts a drag, in logical pixels.
qreal dragDistance() { return 4 * QGuiApplication::styleHints()->startDragDistance(); }

} // namespace

// Keybinds.remapModifiers, as kitty's remap_modifiers: the event that the
// window reads, and the terminal's name of a modifier key.
TEST(RemappedKeys, TheEventCarriesTheNewModifiersAndText) {
    const auto remap = ModifierRemap::parse(QStringLiteral("ctrl:super super:ctrl alt:ctrl+shift"));
    ASSERT_TRUE(remap);
    // Super+C becomes Ctrl+C. Its text goes: the encoder takes ^C from the key.
    QKeyEvent superC(QEvent::KeyPress, Qt::Key_C, Qt::MetaModifier, QStringLiteral("c"));
    auto event = remappedKeyEvent(superC, *remap);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->modifiers(), Qt::KeyboardModifiers(Qt::ControlModifier));
    EXPECT_EQ(event->key(), int(Qt::Key_C));
    EXPECT_TRUE(event->text().isEmpty());
    // The press of the Ctrl key itself is the Super key's, as in kitty. Qt
    // stores the state before a modifier key and reports the one after it.
    QKeyEvent ctrl(QEvent::KeyPress, Qt::Key_Control, Qt::NoModifier);
    ASSERT_EQ(ctrl.modifiers(), Qt::KeyboardModifiers(Qt::ControlModifier));
    event = remappedKeyEvent(ctrl, *remap);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->key(), int(Qt::Key_Super_L));
    EXPECT_EQ(event->modifiers(), Qt::KeyboardModifiers(Qt::MetaModifier));
    // Alt becomes two modifiers: its key stays Alt.
    QKeyEvent alt(QEvent::KeyPress, Qt::Key_Alt, Qt::NoModifier);
    event = remappedKeyEvent(alt, *remap);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->key(), int(Qt::Key_Alt));
    EXPECT_EQ(event->modifiers(), Qt::ControlModifier | Qt::ShiftModifier);
    // A key with no remapped modifier is not made again.
    QKeyEvent shiftA(QEvent::KeyPress, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A"));
    EXPECT_FALSE(remappedKeyEvent(shiftA, *remap));
    EXPECT_FALSE(remappedKeyEvent(superC, ModifierRemap{}));
    // The terminal's modifier keys keep their side.
    using AfterglowVt::Key;
    EXPECT_EQ(remappedModifierKey(Key::LeftControl, *remap), Key::LeftSuper);
    EXPECT_EQ(remappedModifierKey(Key::RightSuper, *remap), Key::RightControl);
    EXPECT_EQ(remappedModifierKey(Key::LeftAlt, *remap), Key::LeftAlt);
    EXPECT_EQ(remappedModifierKey(Key::RightShift, *remap), Key::RightShift);
    EXPECT_EQ(remappedModifierKey(Key::Text, *remap), Key::Text);
}

// The remap acts before anything reads an event: a program gets Ctrl+C from
// the Super key, a click with Super reports Ctrl, and a binding on
// Ctrl+Shift fires from Super+Shift and not from Ctrl+Shift.
TEST_F(NativeWindow, RemappedModifiersReachProgramsTheMouseAndBindings) {
    const QString received = directory.filePath(QStringLiteral("received"));
    restartApplication(QStringLiteral("remap.conf"),
        testSettings({{QStringLiteral("Keybinds"), QJsonObject{{QStringLiteral("remapModifiers"), QStringLiteral("ctrl:super super:ctrl")}}}}),
        mouseReporter(received));
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(received) && reportsMouse(pane); }));
    sendKey(window, Qt::Key_C, Qt::MetaModifier, QStringLiteral("c"));
    // Ctrl+C is Super+C now: no ^C.
    sendKey(window, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03"));
    sendKey(window, Qt::Key_X, Qt::NoModifier, QStringLiteral("x"));
    ASSERT_TRUE(waitFor([&] { return fileContent(received).endsWith('x'); })) << fileContent(received).toStdString();
    EXPECT_EQ(fileContent(received), QByteArray("\x03x"));
    // A press with Super is reported with the Ctrl bit (16).
    const QPointF at = cellCentre(window, pane, 2, 1);
    pressLeftWith(window, at, Qt::MetaModifier);
    ASSERT_TRUE(waitFor([&] { return fileContent(received).contains("\x1b[<"); })) << fileContent(received).toStdString();
    EXPECT_TRUE(fileContent(received).contains("\x1b[<16;3;2M")) << fileContent(received).toStdString();
    QMouseEvent release(QEvent::MouseButtonRelease, at, at, window->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::MetaModifier);
    QCoreApplication::sendEvent(window, &release);
    // New Tab is on Ctrl+Shift+T.
    const auto tabs = window->tabs().size();
    sendKey(window, Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("\x14"));
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(window->tabs().size(), tabs);
    sendKey(window, Qt::Key_T, Qt::MetaModifier | Qt::ShiftModifier, QStringLiteral("T"));
    EXPECT_TRUE(waitFor([&] { return window->tabs().size() == tabs + 1; }));
}

namespace {
// Fails the test when @p window starts a drag, which it cancels so that the
// test goes on. A drag that ended leaves its QDrag: its deleteLater came from
// no event loop, so only an explicit send of the deferred deletes takes it.
class NoDrag : public QObject {
public:
    explicit NoDrag(QWindow* window) : m_window(window) {
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] {
            if (!m_window->findChild<QDrag*>()) return;
            m_started = true;
            QDrag::cancel();
        });
        m_timer.start(0);
    }
    ~NoDrag() override { EXPECT_FALSE(m_started || m_window->findChild<QDrag*>()) << "a drag started"; }
private:
    QWindow* m_window;
    QTimer m_timer;
    bool m_started = false;
};
// --- Finger scrolls and touchscreens -----------------------------------------

// @p lines rows of output in @p pane, which go into its history.
void fillHistory(Pane* pane, int lines) {
    std::string text;
    for (int i = 0; i < lines; ++i) text += "line " + std::to_string(i) + "\r\n";
    pane->withTerminal([&text](AfterglowVt::Terminal& t) { t.feed(text); });
}
std::size_t viewportOffset(Pane* pane) {
    return pane->withTerminal([](AfterglowVt::Terminal& t) { return t.screen().viewportOffset(); });
}
std::size_t historySize(Pane* pane) {
    return pane->withTerminal([](AfterglowVt::Terminal& t) { return t.screen().historySize(); });
}
// The height of a row of @p pane, in window logical pixels.
double rowPitch(TerminalWindow* window, Pane* pane) {
    return window->accessibleCellRect(pane, pane->selectionPointAt(0, 0)).height();
}
// Rows of @p pixels of finger travel in @p pane.
double rowsOf(TerminalWindow* window, Pane* pane, double pixels) { return pixels / rowPitch(window, pane); }
// A touchpad's finger scroll, through Qt's input processing in the form
// QtWayland gives it: a begin, @p updates of @p pixels 8 ms apart (an angle
// 12 times as large), and the end when the fingers lift.
void fingerScroll(QWindow* window, const QPointF& at, int pixels, int updates) {
    auto wheel = [&](Qt::ScrollPhase phase, int dy) {
        const auto source = phase == Qt::ScrollBegin ? Qt::MouseEventNotSynthesized : Qt::MouseEventSynthesizedBySystem;
        QWindowSystemInterface::handleWheelEvent(window, at, window->mapToGlobal(at), QPoint(0, dy), QPoint(0, dy * 12),
                                                 Qt::NoModifier, phase, source);
        QWindowSystemInterface::flushWindowSystemEvents();
    };
    wheel(Qt::ScrollBegin, 0);
    for (int i = 0; i < updates; ++i) {
        wheel(Qt::ScrollUpdate, pixels);
        QThread::msleep(8);
    }
    wheel(Qt::ScrollEnd, 0);
}
// Writes @p group.@p key = @p value to the fixture's settings and reloads
// them, as a user's edit does.
void setLive(TerminalWindow* window, const QString& config, const QString& group, const QString& key, const QJsonValue& value) {
    SettingsStore file(config);
    ASSERT_TRUE(file.write(group, key, value.toVariant()));
    ASSERT_TRUE(window->runAction(window->focusedPane(), Action::ReloadConfig));
}
// The offset of @p pane's view once its window renders no more frames:
// a coast has ended then, or stopped.
std::size_t settledOffset(TerminalWindow* window, Pane* pane) {
    waitIdle(window);
    return viewportOffset(pane);
}
// A touchscreen as Qt knows one, for every test.
QPointingDevice* touchscreen() {
    static QPointingDevice* device = QTest::createTouchDevice();
    return device;
}
// Touch points through Qt's input processing, as the compositor's touch
// comes: Qt tracks the points and makes mouse events of a touch that a
// window does not take.
struct TouchSequence : QTest::QTouchEventSequence {
    explicit TouchSequence(QWindow* window) : QTouchEventSequence(window, touchscreen(), false) {}
};
// One finger on a touchscreen.
struct Finger {
    QWindow* window;
    TouchSequence touch{window};
    void down(const QPointF& at, int id = 1) { touch.press(id, at.toPoint()).commit(); }
    void move(const QPointF& at, int id = 1) { touch.move(id, at.toPoint()).commit(); }
    void up(const QPointF& at, int id = 1) { touch.release(id, at.toPoint()).commit(); }
    void cancel() { touch.cancel(); }
    // From @p from by @p by in @p steps moves 8 ms apart, and the lift.
    void stroke(const QPointF& from, const QPointF& by, int steps = 10) {
        down(from);
        for (int i = 1; i <= steps; ++i) {
            move(from + by * double(i) / steps);
            QThread::msleep(8);
        }
        up(from + by);
    }
};
// A program that turns on alternate scroll on the alternate screen and
// writes what it gets to @p file.
QStringList alternateScreenReader(const QString& file) {
    return {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
            QStringLiteral("stty raw -echo; printf '\\033[?1049h\\033[?1007h'; exec cat > \"$0\""), file};
}
// Up keys in @p bytes, in either cursor key mode.
qsizetype upKeys(const QByteArray& bytes) { return bytes.count("\x1b[A") + bytes.count("\x1bOA"); }

TEST_F(NativeWindow, AFingerScrollCoastsAfterTheFingersLiftUntilAPressStopsIt) {
    Pane* pane = window->focusedPane();
    fillHistory(pane, 400);
    waitIdle(window);
    const QPointF at = gridOf(window, pane).center();
    // 180 pixels of finger travel, then the coast: it goes on past them,
    // slows down and stops (kitty's 0.96 each 8 ms). About 24 pixels a step
    // at the lift coast about 570 pixels in all.
    const double travel = rowsOf(window, pane, 180);
    ASSERT_GT(travel, 2);
    fingerScroll(window, at, 30, 6);
    const std::size_t coasted = settledOffset(window, pane);
    EXPECT_GT(double(coasted), travel + 5);
    EXPECT_LT(double(coasted), travel + rowsOf(window, pane, 700));
    // A press stops a coast that moves: once it is past the fingers'
    // distance, the view stays where the press found it.
    fingerScroll(window, at, -30, 6);
    ASSERT_TRUE(waitFor([&] { return double(viewportOffset(pane)) < double(coasted) - travel - 2; })) << "no coast";
    const std::size_t pressed = viewportOffset(pane);
    clickLeft(window, at);
    EXPECT_LE(std::abs(double(settledOffset(window, pane)) - double(pressed)), 1.0) << pressed;
}

TEST_F(NativeWindow, ACoastEndsWithTheHistoryAKeyTheFocusATouchOrAnActionAndAWheelGoesOn) {
    // Mouse.momentumScroll 1, set while Afterglow runs: the coast never slows,
    // so only what stops it ends it.
    setLive(window, directory.filePath(QStringLiteral("afterglow.conf")), QStringLiteral("Mouse"), QStringLiteral("momentumScroll"), 1);
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    fillHistory(pane, 30);
    waitIdle(window);
    const QPointF at = gridOf(window, pane).center();
    // Fewer rows of history than it coasts: it ends at the top, and the
    // window stops rendering.
    fingerScroll(window, at, 30, 6);
    EXPECT_EQ(settledOffset(window, pane), historySize(pane));
    fillHistory(pane, 5000);
    ASSERT_TRUE(window->runAction(pane, Action::ScrollBottom));
    waitIdle(window);
    const auto stops = [&](const char* what, const std::function<void()>& stop) {
        SCOPED_TRACE(what);
        const std::size_t from = viewportOffset(pane);
        fingerScroll(window, at, 30, 6);
        ASSERT_TRUE(waitFor([&] { return viewportOffset(pane) > from + 20; })) << "no coast";
        stop();
        const std::size_t stopped = viewportOffset(pane);
        EXPECT_LE(std::abs(double(settledOffset(window, pane)) - double(stopped)), 1.0) << stopped;
    };
    stops("a key", [&] {
        QKeyEvent shift(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        QCoreApplication::sendEvent(window, &shift);
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &up);
    });
    stops("the focus", [&] {
        QFocusEvent out(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
        QCoreApplication::sendEvent(window, &out);
        QFocusEvent in(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
        QCoreApplication::sendEvent(window, &in);
    });
    stops("a touch", [&] {
        Finger finger{window};
        finger.down(at);
        finger.up(at);
    });
    // An action from a key, a script or afterglow ctl: the view goes where
    // the action puts it, and stays.
    {
        SCOPED_TRACE("an action");
        fingerScroll(window, at, 30, 6);
        ASSERT_TRUE(waitFor([&] { return viewportOffset(pane) > 20; })) << "no coast";
        ASSERT_TRUE(window->runAction(pane, Action::ScrollBottom));
        EXPECT_EQ(settledOffset(window, pane), 0u);
    }
    // As in kitty, a wheel turn scrolls on top of the coast and does not
    // end it.
    fingerScroll(window, at, 30, 6);
    ASSERT_TRUE(waitFor([&] { return viewportOffset(pane) > 20; })) << "no coast";
    wheelAt(window, at);
    const std::size_t wheeled = viewportOffset(pane);
    EXPECT_TRUE(waitFor([&] { return viewportOffset(pane) > wheeled + 20; })) << "the wheel ended the coast";
    clickLeft(window, at);
    waitIdle(window);
}

TEST_F(NativeWindow, AFingerScrollGivesAProgramOneWheelReportOrKeyForEachRow) {
    // No coast: the fingers' distance only. 300 pixels of travel.
    const auto scrollIn = [&](const QStringList& command, const QString& file) {
        restartApplication(QStringLiteral("program.conf"),
            testSettings({{QStringLiteral("Mouse"), QJsonObject{{QStringLiteral("momentumScroll"), 0}}}}), command);
        if (HasFatalFailure()) return 0.0;
        Pane* pane = window->focusedPane();
        EXPECT_TRUE(waitFor([&] { return QFileInfo::exists(file) && !gridOf(window, pane).isEmpty(); }));
        waitIdle(window);
        fingerScroll(window, gridOf(window, pane).center(), 30, 10);
        return rowsOf(window, pane, 300);
    };
    // The reports and the pane's input share the PTY's write queue: once a
    // marker sent after the scroll is in the file, every report is too.
    const auto allIn = [&](const QString& file) {
        if (!window->focusedPane()->sendInput("@m@")) return false;
        return waitFor([&] { return fileContent(file).endsWith("@m@"); });
    };
    const QString reports = directory.filePath(QStringLiteral("reports"));
    const double rows = scrollIn(mouseReporter(reports), reports);
    if (HasFatalFailure()) return;
    ASSERT_TRUE(allIn(reports)) << fileContent(reports).toStdString();
    EXPECT_LE(std::abs(double(fileContent(reports).count("\x1b[<64;")) - rows), 1.0) << fileContent(reports).toStdString();
    // On the alternate screen with alternate scroll, a row is one Up key.
    // A wheel notch is Terminal.scrollLines of them.
    const QString keys = directory.filePath(QStringLiteral("keys"));
    const double keyRows = scrollIn(alternateScreenReader(keys), keys);
    if (HasFatalFailure()) return;
    ASSERT_TRUE(allIn(keys)) << fileContent(keys).toStdString();
    const qsizetype fromFinger = upKeys(fileContent(keys));
    EXPECT_LE(std::abs(double(fromFinger) - keyRows), 1.0) << fromFinger;
    wheelAt(window, gridOf(window, window->focusedPane()).center());
    EXPECT_TRUE(waitFor([&] { return upKeys(fileContent(keys)) == fromFinger + 3; })) << upKeys(fileContent(keys));
}

TEST_F(NativeWindow, ACoastEndsWhenItsPaneGoesToTheAlternateScreen) {
    const QString keys = directory.filePath(QStringLiteral("keys"));
    restartApplication(QStringLiteral("screen.conf"),
        testSettings({{QStringLiteral("Mouse"), QJsonObject{{QStringLiteral("momentumScroll"), 1}}}}), catInto(keys));
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(keys) && !gridOf(window, pane).isEmpty(); }));
    fillHistory(pane, 5000);
    waitIdle(window);
    fingerScroll(window, gridOf(window, pane).center(), 30, 6);
    ASSERT_TRUE(waitFor([&] { return viewportOffset(pane) > 20; })) << "no coast";
    // A program starts on the alternate screen, with alternate scroll: the
    // rest of a coast through the history is no arrow keys for it.
    feedFromReader(pane, "\x1b[?1049h\x1b[?1007h");
    waitIdle(window);
    EXPECT_EQ(upKeys(fileContent(keys)), 0) << fileContent(keys).toStdString();
}

TEST_F(NativeWindow, OutputKeepsAFingerScrollsPartOfARow) {
    restartApplication(QStringLiteral("part.conf"),
        testSettings({{QStringLiteral("Window"), QJsonObject{{QStringLiteral("paneTitles"), true}}},
                      {QStringLiteral("Mouse"), QJsonObject{{QStringLiteral("momentumScroll"), 0}}}}),
        {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    fillHistory(pane, 200);
    waitIdle(window);
    // Two and a half rows back: the view shows three rows back, drawn down
    // by half a row.
    const double pitch = rowPitch(window, pane);
    fingerScroll(window, gridOf(window, pane).center(), int(std::lround(pitch * 2.5)), 1);
    waitIdle(window);
    ASSERT_EQ(viewportOffset(pane), 3u);
    const int shifted = window->accessibleCellRect(pane, pane->selectionPointAt(0, 0)).y();
    // Output enters the history: the view stays on its text, with its half
    // row.
    feedFromReader(pane, "out 1\r\nout 2\r\nout 3\r\n");
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(viewportOffset(pane), 6u);
    EXPECT_LE(std::abs(window->accessibleCellRect(pane, pane->selectionPointAt(0, 0)).y() - shifted), 1);
}

TEST_F(NativeWindow, ASelectionBeingDraggedTakesInTheRowsThatTheWheelScrolls) {
    Pane* pane = window->focusedPane();
    fillHistory(pane, 200);
    waitIdle(window);
    const QPointF at = cellCentre(window, pane, 3, 5);
    pressLeft(window, at);
    dragLeft(window, cellCentre(window, pane, 6, 5));
    ASSERT_TRUE(pane->hasSelection());
    ASSERT_EQ(pane->selectedText().count(QLatin1Char('\n')), 0);
    // The wheel scrolls Terminal.scrollLines rows back under the pointer,
    // which stays still: the selection reaches them.
    wheelAt(window, at);
    waitIdle(window);
    EXPECT_EQ(pane->selectedText().count(QLatin1Char('\n')), 3) << pane->selectedText().toStdString();
    releaseLeft(window, cellCentre(window, pane, 6, 5));
}

TEST_F(NativeWindow, AFingerOnATouchscreenTapsScrollsAndSelects) {
    Pane* pane = window->focusedPane();
    fillHistory(pane, 400);
    waitIdle(window);
    Finger finger{window};
    const QRectF grid = gridOf(window, pane);
    const QPointF at = grid.center();
    // A tap is a click: it clears a selection.
    selectCells(pane, 0, 0, 5, 0);
    ASSERT_TRUE(pane->hasSelection());
    finger.down(at);
    finger.move(at + QPointF(3, 3)); // within the slop
    finger.up(at + QPointF(3, 3));
    EXPECT_FALSE(pane->hasSelection());
    // A finger that moves at once scrolls: down shows older rows, and it
    // coasts.
    finger.stroke(at - QPointF(0, 100), QPointF(0, 200));
    const std::size_t scrolled = settledOffset(window, pane);
    EXPECT_GT(double(scrolled), rowsOf(window, pane, 200) + 5);
    EXPECT_FALSE(pane->hasSelection());
    // Held still for Mouse.touchSelectHold (400 ms), then moved: a selection
    // from where it landed, from column 1 to column 6 of a "line NNN" row.
    // The button is down meanwhile, for a program's drag that reads it.
    const QPointF from = cellCentre(window, pane, 1, 3);
    finger.down(from);
    drain(450);
    finger.move(cellCentre(window, pane, 6, 3));
    EXPECT_EQ(QGuiApplication::mouseButtons(), Qt::LeftButton);
    finger.up(cellCentre(window, pane, 6, 3));
    EXPECT_EQ(QGuiApplication::mouseButtons(), Qt::NoButton);
    EXPECT_EQ(pane->selectedText().size(), 6) << pane->selectedText().toStdString();
    EXPECT_TRUE(pane->selectedText().startsWith(QStringLiteral("ine "))) << pane->selectedText().toStdString();
    // Two fingers at once are no gesture of these: no scroll, and no tap
    // that clears the selection.
    const std::size_t before = settledOffset(window, pane);
    finger.touch.press(1, at.toPoint()).press(2, (at + QPointF(40, 0)).toPoint()).commit();
    finger.touch.move(1, (at + QPointF(0, 120)).toPoint()).move(2, (at + QPointF(40, 120)).toPoint()).commit();
    finger.touch.release(1, (at + QPointF(0, 120)).toPoint()).release(2, (at + QPointF(40, 120)).toPoint()).commit();
    EXPECT_EQ(settledOffset(window, pane), before);
    EXPECT_TRUE(pane->hasSelection());
    // A second finger before the first moves ends the gesture too. As
    // QtWayland does, each event has the fingers that did not move as
    // stationary.
    finger.down(at);
    finger.touch.stationary(1).press(2, (at + QPointF(40, 0)).toPoint()).commit();
    finger.touch.move(1, (at + QPointF(0, 120)).toPoint()).stationary(2).commit();
    finger.touch.release(1, (at + QPointF(0, 120)).toPoint()).release(2, (at + QPointF(40, 0)).toPoint()).commit();
    EXPECT_EQ(settledOffset(window, pane), before);
    EXPECT_TRUE(pane->hasSelection());
    // One that lands after the first moved is ignored: the scroll goes on.
    finger.down(at);
    finger.move(at + QPointF(0, 60));
    finger.touch.stationary(1).press(2, (at + QPointF(40, 0)).toPoint()).commit();
    finger.touch.move(1, (at + QPointF(0, 120)).toPoint()).stationary(2).commit();
    finger.touch.release(1, (at + QPointF(0, 120)).toPoint()).release(2, (at + QPointF(40, 0)).toPoint()).commit();
    EXPECT_GT(double(settledOffset(window, pane)), double(before) + rowsOf(window, pane, 120) - 1);
}

TEST_F(NativeWindow, ACancelledTouchNeitherCoastsNorHoldsItsButton) {
    Pane* pane = window->focusedPane();
    fillHistory(pane, 400);
    waitIdle(window);
    Finger finger{window};
    const QPointF at = gridOf(window, pane).center();
    // The compositor takes a scroll (an edge gesture): the view stays at the
    // fingers' distance, with no coast.
    finger.down(at - QPointF(0, 100));
    for (int i = 1; i <= 10; ++i) {
        finger.move(at - QPointF(0, 100) + QPointF(0, 20 * i));
        QThread::msleep(8);
    }
    finger.cancel();
    EXPECT_LE(std::abs(double(settledOffset(window, pane)) - rowsOf(window, pane, 200)), 1.0);
    // And a selection: its button goes up.
    const QPointF from = cellCentre(window, pane, 1, 3);
    finger.down(from);
    drain(450);
    finger.move(cellCentre(window, pane, 6, 3));
    ASSERT_EQ(QGuiApplication::mouseButtons(), Qt::LeftButton);
    finger.cancel();
    EXPECT_EQ(QGuiApplication::mouseButtons(), Qt::NoButton);
    EXPECT_TRUE(pane->hasSelection());
}

TEST_F(NativeWindow, AFlickThatEndsOverAnotherPaneCoastsInItsOwn) {
    Pane* top = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")};
    Pane* bottom = window->splitPane(top, SplitTree::Direction::Down, launch, nullptr);
    ASSERT_NE(bottom, nullptr);
    fillHistory(top, 400);
    fillHistory(bottom, 400);
    waitIdle(window);
    // From the top pane's middle down past its bottom edge.
    const QRectF upper = gridOf(window, top);
    const QPointF from = upper.center();
    Finger finger{window};
    finger.stroke(from, QPointF(0, gridOf(window, bottom).center().y() - from.y()));
    EXPECT_GT(double(settledOffset(window, top)), rowsOf(window, top, gridOf(window, bottom).center().y() - from.y()) + 5);
    EXPECT_EQ(viewportOffset(bottom), 0u);
}

TEST_F(NativeWindow, AFingerDragsADividerAsAMouseDoes) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    fillHistory(first, 400);
    waitIdle(window);
    const QPoint left = window->mapFromGlobal(window->accessiblePaneRect(first).topRight());
    const QPoint right = window->mapFromGlobal(window->accessiblePaneRect(second).topLeft());
    const QPointF divider((left.x() + right.x()) / 2.0, window->mapFromGlobal(window->accessiblePaneRect(first).center()).y());
    const int cols = first->cols();
    // A move at once, which in a pane would scroll, drags the divider.
    Finger finger{window};
    finger.down(divider);
    finger.move(divider + QPointF(30, 0));
    ASSERT_TRUE(nextFrame(window));
    finger.move(divider + QPointF(60, 0));
    EXPECT_TRUE(waitFor([&] { return first->cols() > cols; })) << first->cols();
    finger.up(divider + QPointF(60, 0));
    EXPECT_EQ(settledOffset(window, first), 0u);
}

TEST_F(NativeWindow, APressInsideTheSelectionWaitsForAMoveOrClicks) {
    restartWithDragSelection();
    if (HasFatalFailure()) return;
    NoDrag noDrag(window);
    Pane* pane = window->focusedPane();
    fillRows(pane, "select me here");
    selectWholeScreen(pane);
    ASSERT_TRUE(pane->hasSelection());
    // A click inside the selection clears it, as any click does. The press
    // only waits: a press that started a selection would end this one.
    const QPointF at = cellCentre(window, pane, 3, 0);
    pressLeft(window, at);
    EXPECT_TRUE(pane->hasSelection());
    releaseLeft(window, at);
    EXPECT_FALSE(pane->hasSelection());
    // A move shorter than the drag distance does not drag. Another cell, so
    // that this press is no double click.
    selectWholeScreen(pane);
    const QPointF other = cellCentre(window, pane, 4, 2);
    const QPointF near = other + QPointF(QGuiApplication::styleHints()->startDragDistance() - 1, 0);
    pressLeft(window, other);
    dragLeft(window, near);
    EXPECT_TRUE(pane->hasSelection());
    releaseLeft(window, near);
    EXPECT_FALSE(pane->hasSelection());
}
TEST_F(NativeWindow, AFingerInsideTheSelectionSelectsAndDoesNotDragIt) {
    restartWithDragSelection();
    if (HasFatalFailure()) return;
    NoDrag noDrag(window);
    Pane* pane = window->focusedPane();
    fillRows(pane, "select me here");
    selectWholeScreen(pane);
    // Held, then moved, inside the selection: a new selection, as anywhere.
    Finger finger{window};
    finger.down(cellCentre(window, pane, 0, 1));
    drain(450);
    finger.move(cellCentre(window, pane, 5, 1));
    finger.up(cellCentre(window, pane, 5, 1));
    EXPECT_EQ(pane->selectedText(), QStringLiteral("select"));
}
TEST_F(NativeWindow, TheClickThatAPressInsideTheSelectionHeldMovesTheShellCursor) {
    const QString received = directory.filePath(QStringLiteral("received.txt"));
    restartWithDragSelection({}, catInto(received));
    if (HasFatalFailure()) return;
    NoDrag noDrag(window);
    Pane* pane = window->focusedPane();
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(received) && !gridOf(window, pane).isEmpty(); }));
    // A prompt that takes clicks (OSC 133 click_events), with the cursor
    // after its command, in column 13.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b]133;A;click_events=1\x07$ \x1b]133;B\x07hello world"); });
    selectCells(pane, 2, 0, 12, 0);
    // A release on another cell, nearer than the drag distance, is a drag
    // that selects nothing: it moves no cursor.
    const QRectF grid = gridOf(window, pane);
    const QPointF edge = grid.topLeft() + QPointF(5 * grid.width() / pane->cols(), 0.5 * grid.height() / pane->rows());
    pressLeft(window, edge - QPointF(1, 0));
    releaseLeft(window, edge + QPointF(1, 0));
    EXPECT_FALSE(pane->hasSelection());
    // A click on column 6 moves the cursor 7 characters to the left.
    selectCells(pane, 2, 0, 12, 0);
    clickLeft(window, cellCentre(window, pane, 6, 0));
    EXPECT_FALSE(pane->hasSelection());
    QByteArray left;
    for (int i = 0; i < 7; ++i) left += "\x1b[D";
    EXPECT_TRUE(waitFor([&] { return fileContent(received) == left; }))
        << QString::fromLatin1(fileContent(received).toPercentEncoding()).toStdString();
}
TEST_F(NativeWindow, AProgramThatReportsTheMouseGetsAPressInsideTheSelection) {
    const QString received = directory.filePath(QStringLiteral("received.txt"));
    restartWithDragSelection({}, catInto(received));
    if (HasFatalFailure()) return;
    NoDrag noDrag(window);
    Pane* pane = window->focusedPane();
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(received) && !gridOf(window, pane).isEmpty(); }));
    fillRows(pane, "select me here");
    selectWholeScreen(pane);
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?1000h\x1b[?1006h"); });
    const QPointF at = cellCentre(window, pane, 3, 1);
    pressLeft(window, at);
    releaseLeft(window, at);
    EXPECT_TRUE(waitFor([&] { return fileContent(received).contains("M") && fileContent(received).endsWith("m"); }))
        << QString::fromLatin1(fileContent(received).toPercentEncoding()).toStdString();
    EXPECT_TRUE(pane->hasSelection());
    // With Shift the window takes the press, which waits inside the selection.
    pressLeftWith(window, at, Qt::ShiftModifier);
    EXPECT_TRUE(pane->hasSelection());
    releaseLeft(window, at);
    EXPECT_FALSE(pane->hasSelection());
}
TEST_F(NativeWindow, ClicksInsideTheSelectionSelectAWordALineOrABlock) {
    restartWithDragSelection();
    if (HasFatalFailure()) return;
    NoDrag noDrag(window);
    Pane* pane = window->focusedPane();
    fillRows(pane, "select me here");
    selectWholeScreen(pane);
    // The press held back inside the selection is the first click of a
    // series: a double click selects a word, and a third press inside that
    // word selects the line.
    const QPointF at = cellCentre(window, pane, 8, 1);
    clickLeft(window, at);
    clickLeft(window, at);
    EXPECT_EQ(pane->selectedText(), QStringLiteral("me"));
    clickLeft(window, at);
    EXPECT_EQ(pane->selectedText(), QStringLiteral("select me here"));
    // The fourth press of the series counts as one again, inside the line:
    // it starts a selection, which a move extends.
    pressLeft(window, at);
    EXPECT_FALSE(pane->hasSelection());
    dragLeft(window, cellCentre(window, pane, 11, 1));
    releaseLeft(window, cellCentre(window, pane, 11, 1));
    EXPECT_EQ(pane->selectedText(), QStringLiteral("e he"));
    // An Alt press starts a block selection, and a press that a finger makes
    // (touchMouse, with the source of Qt's own synthesis) starts a
    // selection: neither waits to drag.
    selectWholeScreen(pane);
    const QPointF block = cellCentre(window, pane, 2, 3);
    pressLeftWith(window, block, Qt::AltModifier);
    EXPECT_FALSE(pane->hasSelection());
    releaseLeft(window, block);
    selectWholeScreen(pane);
    const QPointF touch = cellCentre(window, pane, 5, 4);
    pressLeftWith(window, touch, Qt::NoModifier, Qt::MouseEventSynthesizedByQt);
    EXPECT_FALSE(pane->hasSelection());
    releaseLeft(window, touch);
}
TEST_F(NativeWindow, APressOutsideTheSelectionOrOnBlanksSelects) {
    restartWithDragSelection();
    if (HasFatalFailure()) return;
    NoDrag noDrag(window);
    Pane* pane = window->focusedPane();
    fillRows(pane, "abcdef");
    // Outside the selection, a press selects from where it is.
    selectRow(pane, 0);
    pressLeft(window, cellCentre(window, pane, 1, 2));
    EXPECT_FALSE(pane->hasSelection());
    dragLeft(window, cellCentre(window, pane, 4, 2));
    releaseLeft(window, cellCentre(window, pane, 4, 2));
    EXPECT_EQ(pane->selectedText(), QStringLiteral("bcde"));
    // Blanks after the end of a line hold no text: a press on them waits,
    // and its move selects from the press as any press does.
    selectCells(pane, 10, 0, 20, 0);
    ASSERT_TRUE(pane->hasSelection());
    ASSERT_TRUE(pane->selectedText().isEmpty());
    pressLeft(window, cellCentre(window, pane, 15, 0));
    EXPECT_TRUE(pane->hasSelection());
    dragLeft(window, cellCentre(window, pane, 3, 1));
    releaseLeft(window, cellCentre(window, pane, 3, 1));
    EXPECT_TRUE(pane->selectedText().endsWith(QStringLiteral("abcd"))) << pane->selectedText().toStdString();
    // A press whose release went elsewhere ends at the next move without
    // the button. A move with the button after it drags nothing.
    selectRow(pane, 0);
    const QPointF held = cellCentre(window, pane, 2, 0);
    pressLeft(window, held);
    hoverAt(window, held);
    dragLeft(window, held + QPointF(dragDistance(), 0));
    releaseLeft(window, held + QPointF(dragDistance(), 0));
    EXPECT_TRUE(pane->hasSelection());
}
TEST_F(NativeWindow, WithoutDragSelectionAPressInsideTheSelectionSelectsAgain) {
    // The default, as in Kitty.
    NoDrag noDrag(window);
    Pane* pane = window->focusedPane();
    fillRows(pane, "select me here");
    selectWholeScreen(pane);
    // Away from where the old selection starts, so that the old selection
    // extended to the move would show.
    pressLeft(window, cellCentre(window, pane, 2, 1));
    ASSERT_FALSE(pane->hasSelection());
    dragLeft(window, cellCentre(window, pane, 5, 1));
    releaseLeft(window, cellCentre(window, pane, 5, 1));
    EXPECT_EQ(pane->selectedText(), QStringLiteral("lect"));
}
TEST_F(NativeWindow, AProgramsDragComesBeforeTheDragOfTheSelection) {
    restartWithDragSelection();
    if (HasFatalFailure()) return;
    NoDrag noDrag(window);
    Pane* pane = window->focusedPane();
    fillRows(pane, "select me here");
    selectWholeScreen(pane);
    // The program offers drags (OSC 72), as kitten dnd does.
    pane->dragDrop()->handle("t=o:x=1;");
    ASSERT_TRUE(pane->dragDrop()->offersDrags());
    QByteArray replies;
    QObject context;
    QObject::connect(pane->dragDrop(), &DragDrop::reply, &context, [&replies](const QByteArray& bytes) { replies += bytes; });
    const QPointF at = cellCentre(window, pane, 3, 1);
    pressLeft(window, at);
    dragLeft(window, at + QPointF(dragDistance(), 0));
    releaseLeft(window, at + QPointF(dragDistance(), 0));
    // The program hears that its drag starts, and the selection stays.
    EXPECT_TRUE(dndKeys(replies).contains("t=o")) << replies.toStdString();
    EXPECT_TRUE(pane->hasSelection());
}
TEST_F(NativeWindow, ASelectionDropsOnAnotherPaneAndNotOnItsOwn) {
    restartWithDragSelection();
    if (HasFatalFailure()) return;
    const QString received = directory.filePath(QStringLiteral("received.txt"));
    Pane* left = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = catInto(received);
    Pane* right = window->splitPane(left, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_TRUE(waitFor([&] { return !gridOf(window, right).isEmpty() && QFileInfo::exists(received); }));
    fillRows(left, "dragged words");
    selectRow(left, 1);
    // While the drag runs, the drops that the compositor would send. The
    // drop event finds the drag's source through Qt.
    bool dragging = false, ownTaken = true, otherTaken = false;
    QByteArray toProgram;
    // The timer's context dies with the locals that it writes, before them.
    QObject context;
    QTimer::singleShot(0, &context, [&] {
        auto* drag = window->findChild<QDrag*>();
        dragging = drag;
        if (drag) {
            const auto dropAt = [&](Pane* pane, QEvent::Type type) {
                QDropEvent event(gridOf(window, pane).center(), Qt::CopyAction, drag->mimeData(), Qt::NoButton, Qt::NoModifier, type);
                QCoreApplication::sendEvent(window, &event);
                return event.isAccepted();
            };
            ownTaken = dropAt(left, QEvent::Drop);
            // A program that accepts drops (OSC 72) hears nothing of its
            // own pane's selection either.
            left->dragDrop()->handle("t=a;");
            QObject::connect(left->dragDrop(), &DragDrop::reply, &context, [&](const QByteArray& bytes) { toProgram += bytes; });
            dropAt(left, QEvent::DragMove);
            ownTaken = dropAt(left, QEvent::Drop) || ownTaken;
            otherTaken = dropAt(right, QEvent::Drop);
        }
        QDrag::cancel();
    });
    const QPointF at = cellCentre(window, left, 2, 1);
    pressLeft(window, at);
    dragLeft(window, at + QPointF(dragDistance(), 0));
    releaseLeft(window, at + QPointF(dragDistance(), 0));
    ASSERT_TRUE(dragging);
    EXPECT_FALSE(ownTaken);
    EXPECT_TRUE(toProgram.isEmpty()) << toProgram.toStdString();
    EXPECT_TRUE(otherTaken);
    EXPECT_TRUE(waitFor([&] { return fileContent(received) == "dragged words"; })) << fileContent(received).toStdString();
    EXPECT_TRUE(left->hasSelection()); // copied, not moved
}
TEST_F(NativeWindow, AWindowThatClosesDuringASelectionDragGoesAfterIt) {
    restartWithDragSelection({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("confirmClose"), false}}}});
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    fillRows(pane, "dragged words");
    selectRow(pane, 0);
    // The drag runs this close in its event loop, with its QDrag a child of
    // the window: the window goes after the drag.
    const QPointer<TerminalWindow> closing = window;
    bool dragging = false, moveReturned = false, deletedInTheDrag = false;
    QObject context;
    QObject::connect(window, &QObject::destroyed, &context, [&] { deletedInTheDrag = !moveReturned; });
    // A drag of a person runs on after the close, and its event loop deletes
    // what the close left with deleteLater. Here the compositor ends the
    // drag at once: the timer sends those deletes as that loop would.
    QTimer::singleShot(0, window, [&] {
        dragging = closing->findChild<QDrag*>();
        closing->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    const QPointF at = cellCentre(window, pane, 2, 0);
    pressLeft(window, at);
    dragLeft(window, at + QPointF(dragDistance(), 0));
    moveReturned = true;
    EXPECT_TRUE(dragging);
    EXPECT_FALSE(deletedInTheDrag);
    window = nullptr;
    // The window went with deleteLater once the drag returned.
    drainDeferredDeletes();
    EXPECT_TRUE(closing.isNull());
}
TEST_F(NativeWindow, AKeyHeldIntoASelectionDragIsReleasedToItsProgram) {
    const QString received = directory.filePath(QStringLiteral("received.txt"));
    restartWithDragSelection({}, catInto(received));
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(received); }));
    // The program asks for key releases (Kitty keyboard flags 1, 2 and 8).
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[>11u"); });
    // A key press clears the selection, as typing does: the selection comes after it.
    QKeyEvent down(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QCoreApplication::sendEvent(window, &down);
    fillRows(pane, "dragged words");
    selectRow(pane, 0);
    // The key comes up during the drag, which takes the key events: the
    // program has its release from when the drag started, and only that one.
    bool dragging = false;
    // The timer's context dies with the locals that it writes, before them.
    QObject context;
    QTimer::singleShot(0, &context, [&] {
        dragging = window->findChild<QDrag*>();
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
        QCoreApplication::sendEvent(window, &up);
        QDrag::cancel();
    });
    const QPointF at = cellCentre(window, pane, 2, 0);
    pressLeft(window, at);
    dragLeft(window, at + QPointF(dragDistance(), 0));
    releaseLeft(window, at + QPointF(dragDistance(), 0));
    ASSERT_TRUE(dragging);
    // Input after the drag reaches the program after any release that the drag sent.
    ASSERT_TRUE(pane->sendInput("|end"));
    EXPECT_TRUE(waitFor([&] { return fileContent(received).endsWith("|end"); }))
        << QString::fromLatin1(fileContent(received).toPercentEncoding()).toStdString();
    EXPECT_EQ(fileContent(received).count("\x1b[97;1:3u"), 1);
}
TEST_F(NativeWindow, AProgramsDragStartReleasesTheKeysThatItsWindowSent) {
    const QString received = directory.filePath(QStringLiteral("received.txt"));
    restartWithDragSelection({}, catInto(received));
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(received); }));
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[>11u"); });
    QKeyEvent down(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QCoreApplication::sendEvent(window, &down);
    // DragDrop::start() says this before QDrag::exec(), which takes the key events.
    Q_EMIT pane->dragDrop()->dragStarting();
    ASSERT_TRUE(pane->sendInput("|end"));
    EXPECT_TRUE(waitFor([&] { return fileContent(received).endsWith("|end"); }))
        << QString::fromLatin1(fileContent(received).toPercentEncoding()).toStdString();
    EXPECT_EQ(fileContent(received).count("\x1b[97;1:3u"), 1);
    // The key is taken: its release after the drag sends nothing more.
    QKeyEvent up(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    QCoreApplication::sendEvent(window, &up);
    ASSERT_TRUE(pane->sendInput("|after"));
    EXPECT_TRUE(waitFor([&] { return fileContent(received).endsWith("|after"); }));
    EXPECT_EQ(fileContent(received).count("\x1b[97;1:3u"), 1);
}
TEST_F(NativeWindow, DroppedTextPastesIntoThePane) {
    const QString received = directory.filePath(QStringLiteral("received.txt"));
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = catInto(received);
    Pane* right = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_TRUE(waitFor([&] { return !gridOf(window, right).isEmpty() && QFileInfo::exists(received); }));
    // The program uses bracketed paste: the text arrives in its brackets.
    right->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?2004h"); });
    window->revealPane(first);
    ASSERT_EQ(window->focusedPane(), first);
    QMimeData mime;
    mime.setText(QStringLiteral("dropped words"));
    // The desktop drops only where a move of the drag was accepted.
    QDragMoveEvent move(gridOf(window, right).center(), Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &move);
    EXPECT_TRUE(move.isAccepted());
    QDropEvent drop(gridOf(window, right).center(), Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier, QEvent::Drop);
    QCoreApplication::sendEvent(window, &drop);
    EXPECT_TRUE(drop.isAccepted());
    EXPECT_TRUE(waitFor([&] { return fileContent(received) == "\x1b[200~dropped words\x1b[201~"; }))
        << fileContent(received).toStdString();
    EXPECT_EQ(window->focusedPane(), right);
    // A link from a browser holds no file: its text pastes, as in Kitty.
    QMimeData link;
    link.setUrls({QUrl(QStringLiteral("https://example.com/page"))});
    link.setText(QStringLiteral("https://example.com/page"));
    QDropEvent linkDrop(gridOf(window, right).center(), Qt::CopyAction, &link, Qt::NoButton, Qt::NoModifier, QEvent::Drop);
    QCoreApplication::sendEvent(window, &linkDrop);
    EXPECT_TRUE(linkDrop.isAccepted());
    EXPECT_TRUE(waitFor([&] {
        return fileContent(received) == "\x1b[200~dropped words\x1b[201~\x1b[200~https://example.com/page\x1b[201~";
    })) << fileContent(received).toStdString();
}
TEST_F(NativeWindow, DroppedLinesAskBeforeTheyPaste) {
    const QString received = directory.filePath(QStringLiteral("received.txt"));
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = catInto(received);
    Pane* right = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_TRUE(waitFor([&] { return !gridOf(window, right).isEmpty() && QFileInfo::exists(received); }));
    window->revealPane(first);
    QMimeData mime;
    mime.setText(QStringLiteral("one\ntwo\n"));
    QDropEvent drop(gridOf(window, right).center(), Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier, QEvent::Drop);
    QCoreApplication::sendEvent(window, &drop);
    EXPECT_TRUE(drop.isAccepted());
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(chrome->pasteConfirmOpen());
    EXPECT_EQ(window->focusedPane(), right);
    // The question takes the drops until it has its answer.
    QMimeData other;
    other.setText(QStringLiteral("other"));
    QDropEvent second(gridOf(window, right).center(), Qt::CopyAction, &other, Qt::NoButton, Qt::NoModifier, QEvent::Drop);
    QCoreApplication::sendEvent(window, &second);
    EXPECT_FALSE(second.isAccepted());
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    EXPECT_TRUE(fileContent(received).isEmpty());
    QKeyEvent accept(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &accept);
    EXPECT_FALSE(chrome->pasteConfirmOpen());
    // A paste sends line feeds as carriage returns, as Enter does.
    EXPECT_TRUE(waitFor([&] { return fileContent(received) == "one\rtwo\r"; })) << fileContent(received).toStdString();
}
TEST_F(NativeWindow, DropsThatAPaneRefusesPasteNothing) {
    const QString received = directory.filePath(QStringLiteral("received.txt"));
    PaneLaunchOptions launch;
    launch.command = catInto(received);
    Pane* right = window->splitPane(window->focusedPane(), SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_TRUE(waitFor([&] { return !gridOf(window, right).isEmpty() && QFileInfo::exists(received); }));
    const auto dropOnRight = [&](const QMimeData& mime, Qt::DropActions actions) {
        QDropEvent drop(gridOf(window, right).center(), actions, &mime, Qt::NoButton, Qt::NoModifier, QEvent::Drop);
        QCoreApplication::sendEvent(window, &drop);
        return drop.isAccepted();
    };
    // A file on another host is no local file. Qt gives the URL as the
    // drop's text, which does not paste either.
    QMimeData remote;
    remote.setUrls({QUrl(QStringLiteral("file://example.com/share/file.txt"))});
    ASSERT_TRUE(remote.hasText());
    EXPECT_FALSE(dropOnRight(remote, Qt::CopyAction));
    // A name with a bidirectional override, with text of its own as well.
    QMimeData disguised;
    disguised.setUrls({QUrl::fromLocalFile(directory.filePath(QStringLiteral("name") + QChar(0x202E) + QStringLiteral("txt.sh")))});
    disguised.setText(QStringLiteral("disguised"));
    EXPECT_FALSE(dropOnRight(disguised, Qt::CopyAction));
    // A local file beside a link: the link is no local file, so the whole
    // file drop is refused, and its text does not paste either.
    QMimeData mixed;
    mixed.setUrls({QUrl::fromLocalFile(directory.filePath(QStringLiteral("a"))), QUrl(QStringLiteral("https://example.com/mixed"))});
    mixed.setText(QStringLiteral("https://example.com/mixed"));
    EXPECT_FALSE(dropOnRight(mixed, Qt::CopyAction));
    // A drag with neither text nor URLs: its move is refused on its types.
    QMimeData picture;
    picture.setData(QStringLiteral("image/png"), QByteArray("not read"));
    QDragMoveEvent overRight(gridOf(window, right).center(), Qt::CopyAction, &picture, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &overRight);
    EXPECT_FALSE(overRight.isAccepted());
    EXPECT_FALSE(dropOnRight(picture, Qt::CopyAction));
    // A drag that offers only a move: a paste copies.
    QMimeData moved;
    moved.setText(QStringLiteral("moved"));
    EXPECT_FALSE(dropOnRight(moved, Qt::MoveAction));
    // A pane held open after its program ended takes no drop.
    PaneLaunchOptions ending;
    ending.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("read line")};
    Pane* held = window->splitPane(right, SplitTree::Direction::Down, ending, nullptr);
    ASSERT_NE(held, nullptr);
    held->setHold(true);
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(held->sendInput("\n"));
    ASSERT_TRUE(waitFor([&] { return held->isHeld() && !gridOf(window, held).isEmpty(); }));
    QDropEvent onHeld(gridOf(window, held).center(), Qt::CopyAction, &moved, Qt::NoButton, Qt::NoModifier, QEvent::Drop);
    QCoreApplication::sendEvent(window, &onHeld);
    EXPECT_FALSE(onHeld.isAccepted());
    // The program accepts drops (OSC 72): the drop is the program's, not a
    // paste. It chose no action for the drop's types, so the drop is
    // refused and the program hears the drag leave (t=m, x=-1).
    right->dragDrop()->handle("t=a;");
    ASSERT_TRUE(right->dragDrop()->acceptsDrops());
    QList<QByteArray> toProgram;
    QObject listener;
    QObject::connect(right->dragDrop(), &DragDrop::reply, &listener, [&](const QByteArray& bytes) { toProgram.append(bytes); });
    QMimeData forProgram;
    forProgram.setText(QStringLiteral("for the program"));
    EXPECT_FALSE(dropOnRight(forProgram, Qt::CopyAction));
    EXPECT_TRUE(std::any_of(toProgram.cbegin(), toProgram.cend(), [](const QByteArray& packet) {
        const auto keys = dndKeys(packet);
        return keys.contains("t=m") && keys.contains("x=-1");
    })) << toProgram.join('|').toStdString();
    right->dragDrop()->reset();
    // A drop that pastes, after them: only its text arrives.
    QMimeData marker;
    marker.setText(QStringLiteral("marker"));
    EXPECT_TRUE(dropOnRight(marker, Qt::CopyAction));
    EXPECT_TRUE(waitFor([&] { return fileContent(received).endsWith("marker"); })) << fileContent(received).toStdString();
    EXPECT_FALSE(fileContent(received).contains("for the program"));
    EXPECT_FALSE(fileContent(received).contains("disguised"));
    EXPECT_FALSE(fileContent(received).contains("example.com"));
    EXPECT_FALSE(fileContent(received).contains("moved"));
}
// Drag events that reach a window, also in QDrag::exec's own event loop.
class DragEvents : public QObject {
public:
    QRectF over;     ///< where moves count
    QString hovered; ///< made at the first move over `over`, for the runner
    int moves = 0;
    bool ended = false; ///< a drop or a leave after a move
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            if (over.contains(static_cast<QDropEvent*>(event)->position()) && ++moves == 1 && !hovered.isEmpty()) {
                QFile marker(hovered);
                if (marker.open(QIODevice::WriteOnly)) marker.close();
            }
        } else if ((event->type() == QEvent::Drop || event->type() == QEvent::DragLeave) && moves > 0) {
            ended = true;
        }
        return false;
    }
};
// Selected text dragged with real compositor input: into another pane it
// pastes, back onto its own pane it does not.
TEST_F(NativeWindow, ASelectionDragsIntoAnotherPaneAndNotIntoItsOwn) {
    REQUIRE_NATIVE_DND(ready);
    restartWithDragSelection();
    if (HasFatalFailure()) return;
    const QString received = directory.filePath(QStringLiteral("received.txt"));
    Pane* left = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = catInto(received);
    Pane* right = window->splitPane(left, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_TRUE(showFullScreenForDrag(window));
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(received) && !gridOf(window, right).isEmpty(); }));
    // The full screen layout and the compositor settle first: a width that
    // changes ends a selection, and a press while the surface still changes
    // is lost.
    expectIdle(window);
    PressMarker input;
    input.path = ready + QStringLiteral(".pressed");
    window->installEventFilter(&input);
    const auto removeInput = qScopeGuard([&] { window->removeEventFilter(&input); });
    DragEvents events;
    events.hovered = ready + QStringLiteral(".hovered");
    window->installEventFilter(&events);
    const auto removeEvents = qScopeGuard([&] { window->removeEventFilter(&events); });
    // Presses about a third down and in the middle of a row, as the other
    // drags press: clear of the pane titles and of the window's border, where
    // the compositor takes the press.
    const int leftRow = left->rows() / 3, rightRow = right->rows() / 3;
    fillRows(left, "dragged words");
    selectRow(left, leftRow);
    ASSERT_TRUE(left->hasSelection());
    events.over = gridOf(window, right);
    ASSERT_TRUE(writeDragMarker(ready, window, cellCentre(window, left, left->cols() / 2, leftRow),
                                gridOf(window, right).center(), true));
    EXPECT_TRUE(waitFor([&] { return fileContent(received) == "dragged words"; }, 15000)) << fileContent(received).toStdString();
    EXPECT_TRUE(left->hasSelection()); // copied, not moved
    // From the right pane back onto itself: the drag moves over the pane and
    // ends with no paste. The selection stays, so the press waited and
    // started no selection.
    fillRows(right, "own words");
    selectRow(right, rightRow);
    ASSERT_TRUE(right->hasSelection());
    events.moves = 0;
    events.ended = false;
    ASSERT_TRUE(writeDragMarker(ready, window, cellCentre(window, right, right->cols() / 2, rightRow),
                                cellCentre(window, right, right->cols() / 2, right->rows() * 2 / 3), true));
    ASSERT_TRUE(waitFor([&] { return events.ended; }, 15000)) << "moves over the pane: " << events.moves;
    // Input after the drop reaches the program after a paste of the drop would.
    ASSERT_TRUE(right->sendInput("|end"));
    EXPECT_TRUE(waitFor([&] { return fileContent(received).endsWith("|end"); })) << fileContent(received).toStdString();
    EXPECT_EQ(fileContent(received), "dragged words|end");
    EXPECT_EQ(right->selectedText(), QStringLiteral("own words"));
}

TEST_F(NativeWindow, ScriptQueriesAreBoundedAndRespectTextPolicy) {
    SettingsStore settings(directory.filePath(QStringLiteral("query.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    auto* pane = window->focusedPane();
    pane->withTerminal([](AfterglowVt::Terminal& terminal) {
        terminal.feed("\x1b[2J\x1b[H\x1b]133;A\x1b\\$ \x1b]133;B\x1b\\echo test\x1b]133;C\x1b\\result\x1b]133;D;0\x1b\\");
    });
    pane->startSelection(pane->selectionPointAt(0, 0), AfterglowVt::SelectionMode::Normal, true);
    pane->extendSelection(pane->selectionPointAt(5, 0));
    QVariantMap data;
    ASSERT_TRUE(control.readTextBounded(pane->id(), QStringLiteral("selection"), 3, data, false).isEmpty());
    EXPECT_TRUE(data[QStringLiteral("available")].toBool());
    EXPECT_TRUE(data[QStringLiteral("truncated")].toBool());
    EXPECT_LE(data[QStringLiteral("text")].toString().toUtf8().size(), 3);
    ASSERT_TRUE(control.semanticRegions(pane->id(), data, false).isEmpty());
    const auto regions = data[QStringLiteral("regions")].toList();
    ASSERT_GE(regions.size(), 3);
    QString output;
    for (const auto& item : regions)
        if (item.toMap()[QStringLiteral("kind")] == QStringLiteral("output")) output = item.toMap()[QStringLiteral("id")].toString();
    ASSERT_FALSE(output.isEmpty());
    ASSERT_TRUE(control.readRegion(pane->id(), output, 3, data, false).isEmpty());
    EXPECT_EQ(data[QStringLiteral("text")].toString(), QStringLiteral("res"));
    EXPECT_TRUE(data[QStringLiteral("truncated")].toBool());

    // With Scripts.allowReadText off, no read runs, and the refusal names it.
    const QString scriptsOff = QStringLiteral("reading pane text is off (Scripts.allowReadText)");
    SettingsStore denied(directory.filePath(QStringLiteral("denied.conf")),
        QJsonObject{{QStringLiteral("Scripts"), QJsonObject{{QStringLiteral("allowReadText"), false}}}});
    Control blocked(denied, {});
    blocked.attachWindow(window);
    EXPECT_EQ(blocked.readTextBounded(pane->id(), QStringLiteral("screen"), 100, data, false), scriptsOff);
    EXPECT_EQ(blocked.readTextBounded(pane->id(), QStringLiteral("selection"), 100, data, false), scriptsOff);
    EXPECT_EQ(blocked.semanticRegions(pane->id(), data, false), scriptsOff);
    EXPECT_EQ(blocked.readRegion(pane->id(), output, 100, data, false), scriptsOff);

    // A command that a client runs (aglow run, afterglow run) reads pane
    // text only with Control.allowReadText too.
    const QString clientsOff =
        QStringLiteral("reading pane text for a command run with a client is off (Control.allowReadText)");
    SettingsStore local(directory.filePath(QStringLiteral("local.conf")),
        QJsonObject{{QStringLiteral("Scripts"), QJsonObject{{QStringLiteral("allowReadText"), true}}},
                    {QStringLiteral("Control"), QJsonObject{{QStringLiteral("allowReadText"), false}}}});
    Control scriptsOnly(local, {});
    scriptsOnly.attachWindow(window);
    EXPECT_TRUE(scriptsOnly.readTextBounded(pane->id(), QStringLiteral("screen"), 100, data, false).isEmpty());
    EXPECT_EQ(scriptsOnly.readTextBounded(pane->id(), QStringLiteral("screen"), 100, data, true), clientsOff);
    EXPECT_EQ(scriptsOnly.semanticRegions(pane->id(), data, true), clientsOff);
    EXPECT_EQ(scriptsOnly.readRegion(pane->id(), output, 100, data, true), clientsOff);

    // With both on, a client's command reads as any other does.
    SettingsStore both(directory.filePath(QStringLiteral("both.conf")),
        QJsonObject{{QStringLiteral("Scripts"), QJsonObject{{QStringLiteral("allowReadText"), true}}},
                    {QStringLiteral("Control"), QJsonObject{{QStringLiteral("allowReadText"), true}}}});
    Control clients(both, {});
    clients.attachWindow(window);
    data.clear();
    ASSERT_TRUE(clients.readTextBounded(pane->id(), QStringLiteral("selection"), 3, data, true).isEmpty());
    EXPECT_TRUE(data[QStringLiteral("available")].toBool());
    EXPECT_TRUE(data[QStringLiteral("truncated")].toBool());
    data.clear();
    ASSERT_TRUE(clients.semanticRegions(pane->id(), data, true).isEmpty());
    EXPECT_EQ(data[QStringLiteral("regions")].toList().size(), regions.size());
    data.clear();
    ASSERT_TRUE(clients.readRegion(pane->id(), output, 3, data, true).isEmpty());
    EXPECT_EQ(data[QStringLiteral("text")].toString(), QStringLiteral("res"));
}

TEST_F(NativeWindow, BidiPointerSelectionAndAccessibleGeometryUseVisualCells) {
    auto* pane=window->focusedPane();
    auto options=pane->withTerminal([](AfterglowVt::Terminal& terminal){return terminal.options();});
    options.bidi=AfterglowVt::BidiMode::Auto; pane->setOptions(options);
    pane->withTerminal([](AfterglowVt::Terminal& terminal){terminal.feed("\x1b[2J\x1b[Hאבג");});
    pane->refreshSnapshot();
    const auto first=pane->selectionPointAt(0,0), second=pane->selectionPointAt(1,0);
    const auto firstRect=window->accessibleCellRect(pane,first), secondRect=window->accessibleCellRect(pane,second);
    EXPECT_GT(firstRect.x(),secondRect.x());
    const QPointF a=window->mapFromGlobal(firstRect.center()), b=window->mapFromGlobal(secondRect.center());
    QMouseEvent press(QEvent::MouseButtonPress,a,QPointF(firstRect.center()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QCoreApplication::sendEvent(window,&press);
    QMouseEvent move(QEvent::MouseMove,b,QPointF(secondRect.center()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QCoreApplication::sendEvent(window,&move);
    QMouseEvent release(QEvent::MouseButtonRelease,b,QPointF(secondRect.center()),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QCoreApplication::sendEvent(window,&release);
    EXPECT_EQ(pane->selectedText(),QString::fromUtf8("אב"));
}


TEST_F(NativeWindow, PaneOverviewChoosesWithKeysAndKeepsZoom) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    auto* third = window->splitPane(second, SplitTree::Direction::Down, launch, nullptr);
    ASSERT_NE(third, nullptr);
    window->revealPane(first);
    ASSERT_TRUE(settleQueued());
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(window->runAction(first, Action::TogglePaneZoom));
    ASSERT_TRUE(settleQueued());
    // The zoomed tab shows one title, so that the empty titles below are the
    // overview's and not those of a frame that has not drawn yet.
    ASSERT_TRUE(waitFor([&] { return chrome->paneTitles().size() == 1; }));
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(window->overviewOpen());
    EXPECT_EQ(window->overviewHighlight(), first);
    EXPECT_TRUE(chrome->keyMode().startsWith(QStringLiteral("Panes")));
    // The titles follow the frame that draws the cards, which has no fixed time.
    EXPECT_TRUE(waitFor([&] { return chrome->paneTitles().isEmpty(); }));
    // Every pane is a card while the overview is open, zoom or not.
    EXPECT_FALSE(window->accessiblePaneRect(third).isEmpty());
    ASSERT_TRUE(pressKey(window, Qt::Key_Right));
    EXPECT_EQ(window->overviewHighlight(), second);
    ASSERT_TRUE(pressKey(window, Qt::Key_Right));
    ASSERT_TRUE(pressKey(window, Qt::Key_Right));
    EXPECT_EQ(window->overviewHighlight(), first); // wraps
    ASSERT_TRUE(pressKey(window, Qt::Key_Left));
    EXPECT_EQ(window->overviewHighlight(), third);
    // Keys never reach the shells while the overview is open. A key that got
    // through would go to the focused pane, the first, whose cat holds it
    // until a line ends.
    ASSERT_TRUE(pressKey(window, Qt::Key_X, Qt::NoModifier, QStringLiteral("x")));
    ASSERT_TRUE(pressKey(window, Qt::Key_Return));
    EXPECT_FALSE(window->overviewOpen());
    EXPECT_EQ(window->focusedPane(), third);
    // A line through the same input ends what it held: an "x" shows with it.
    ASSERT_TRUE(first->sendInput("overview-closed\n"));
    ASSERT_TRUE(waitFor([&] {
        return screenOf(first).contains(QStringLiteral("overview-closed"));
    }));
    EXPECT_FALSE(screenOf(first).contains(QLatin1Char('x')))
        << screenOf(first).toStdString();
    // The tab was zoomed: it returns zoomed, on the chosen pane.
    EXPECT_TRUE(waitFor([&] { return window->accessiblePaneRect(first).isEmpty(); }));
    EXPECT_FALSE(window->accessiblePaneRect(third).isEmpty());
    EXPECT_TRUE(chrome->keyMode().isEmpty());
    expectIdle(window);
    // Escape cancels back to the focused pane, also from another card; a
    // digit chooses directly.
    ASSERT_TRUE(window->runAction(third, Action::PaneOverview));
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(window->overviewOpen());
    EXPECT_EQ(window->overviewHighlight(), third);
    ASSERT_TRUE(pressKey(window, Qt::Key_Right));
    EXPECT_EQ(window->overviewHighlight(), first); // wraps
    ASSERT_TRUE(pressKey(window, Qt::Key_Escape));
    EXPECT_FALSE(window->overviewOpen());
    EXPECT_EQ(window->focusedPane(), third);
    ASSERT_TRUE(window->runAction(third, Action::PaneOverview));
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(window->overviewOpen());
    ASSERT_TRUE(pressKey(window, Qt::Key_2, Qt::NoModifier, QStringLiteral("2")));
    EXPECT_FALSE(window->overviewOpen());
    EXPECT_EQ(window->focusedPane(), second);
}

TEST_F(NativeWindow, PaneOverviewSpansTabsAndSwitchesToTheChosenTab) {
    auto* first = window->focusedPane();
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    auto* second = window->focusedPane();
    ASSERT_NE(second, first);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* third = window->splitPane(second, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(third, nullptr);
    window->revealPane(second);
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(window->runAction(second, Action::PaneOverview));
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(window->overviewHighlight(), second);
    // Cards run in tab order: Left from the second tab's first pane reaches the first tab.
    ASSERT_TRUE(pressKey(window, Qt::Key_Left));
    EXPECT_EQ(window->overviewHighlight(), first);
    ASSERT_TRUE(pressKey(window, Qt::Key_Right));
    ASSERT_TRUE(pressKey(window, Qt::Key_Right));
    EXPECT_EQ(window->overviewHighlight(), third);
    // Up and Down jump to the first pane of the previous or next tab.
    ASSERT_TRUE(pressKey(window, Qt::Key_Up));
    EXPECT_EQ(window->overviewHighlight(), first);
    ASSERT_TRUE(pressKey(window, Qt::Key_Down));
    EXPECT_EQ(window->overviewHighlight(), second);
    ASSERT_TRUE(pressKey(window, Qt::Key_Up));
    // The chosen tab shows while the close runs: the cards close onto its
    // layout, and the current tab never changes back first. Every switch
    // until the close has settled goes to it.
    const quint32 chosen = window->tabIdOf(first);
    QObject listener;
    std::vector<quint32> switches;
    QObject::connect(window, &TerminalWindow::tabSwitched, &listener,
                     [&switches](quint32, quint32 to, int) { switches.push_back(to); });
    ASSERT_TRUE(pressKey(window, Qt::Key_Return));
    EXPECT_FALSE(window->overviewOpen());
    EXPECT_EQ(window->activeTabId(), chosen);
    EXPECT_TRUE(waitFor([&] { return window->focusedPane() == first; }));
    EXPECT_FALSE(window->accessiblePaneRect(first).isEmpty());
    expectIdle(window);
    EXPECT_EQ(window->activeTabId(), chosen);
    ASSERT_FALSE(switches.empty());
    for (const quint32 to : switches) EXPECT_EQ(to, chosen);
}

TEST_F(NativeWindow, TabSwitchAnimatesAndSettles) {
    auto* first = window->focusedPane();
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    EXPECT_TRUE(window->tabSwitching());
    auto* second = window->focusedPane();
    EXPECT_NE(second, first);
    EXPECT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    EXPECT_EQ(window->focusedPane(), second);
    expectIdle(window);
    // Switching back runs again; the overview suppresses it.
    window->revealPane(first);
    EXPECT_TRUE(window->tabSwitching());
    EXPECT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    expectIdle(window);
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    ASSERT_TRUE(window->overviewOpen());
    window->revealPane(second);
    // The tab switched, with no slide under the overview.
    EXPECT_EQ(window->activeTabId(), window->tabIdOf(second));
    EXPECT_EQ(window->focusedPane(), second);
    EXPECT_FALSE(window->tabSwitching());
}

TEST_F(NativeWindow, ScriptKeyModeOverviewTabSwitchAndPulse) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(window->paneOrigin(second).from, first->id());
    EXPECT_EQ(window->paneOrigin(second).edge, 1);
    ASSERT_TRUE(settleQueued());
    QList<QVariantMap> keys;
    QList<std::pair<quint32, quint32>> switches;
    QObject listener;
    QObject::connect(window, &TerminalWindow::scriptKey, &listener, [&](const QString& owner, quint32, const QVariantMap& key) {
        if (owner == QLatin1String("script@1")) keys << key;
    });
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(window->startScriptKeyMode(QStringLiteral("script@1"), second, QStringLiteral("Script keys")).isEmpty());
    EXPECT_EQ(chrome->keyMode(), QStringLiteral("Script keys"));
    EXPECT_FALSE(window->startScriptKeyMode(QStringLiteral("other@2"), second, {}).isEmpty());
    ASSERT_TRUE(pressKey(window, Qt::Key_Left));
    ASSERT_TRUE(pressKey(window, Qt::Key_A, Qt::ControlModifier, QStringLiteral("\x01")));
    ASSERT_TRUE(pressKey(window, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q")));
    ASSERT_EQ(keys.size(), 3);
    EXPECT_EQ(keys[0].value(QStringLiteral("key")), QStringLiteral("Left"));
    EXPECT_EQ(keys[1].value(QStringLiteral("key")), QStringLiteral("a"));
    EXPECT_TRUE(keys[1].value(QStringLiteral("ctrl")).toBool());
    EXPECT_EQ(keys[1].value(QStringLiteral("text")), QString());
    EXPECT_EQ(keys[2].value(QStringLiteral("text")), QStringLiteral("q"));
    // The script drives the overview while it holds the keys. Another
    // script cannot open it then, nor move the focus for it: the keys would
    // not reach the overview.
    QVariantMap state;
    ASSERT_EQ(window->focusedPane(), second);
    EXPECT_FALSE(window->scriptOverview(QStringLiteral("other@2"), first, QStringLiteral("open"), state).isEmpty());
    EXPECT_FALSE(window->overviewOpen());
    EXPECT_EQ(window->focusedPane(), second);
    ASSERT_TRUE(window->scriptOverview(QStringLiteral("script@1"), second, QStringLiteral("open"), state).isEmpty());
    EXPECT_TRUE(state.value(QStringLiteral("open")).toBool());
    EXPECT_EQ(state.value(QStringLiteral("cards")).toList().size(), 2);
    ASSERT_TRUE(pressKey(window, Qt::Key_Right)); // to the script, not the overview
    EXPECT_EQ(window->overviewHighlight(), second);
    ASSERT_EQ(keys.size(), 4);
    EXPECT_EQ(keys.last().value(QStringLiteral("key")), QStringLiteral("Right"));
    ASSERT_TRUE(window->scriptOverviewMove(second, nullptr, 1, state).isEmpty());
    EXPECT_EQ(state.value(QStringLiteral("highlighted")).toUInt(), first->id());
    ASSERT_TRUE(window->scriptOverview(QStringLiteral("script@1"), second, QStringLiteral("choose"), state).isEmpty());
    EXPECT_FALSE(state.value(QStringLiteral("open")).toBool());
    EXPECT_EQ(window->focusedPane(), first);
    // Ending the mode, or the script going away, returns the keys.
    window->clearScriptOwner(QStringLiteral("script@1"));
    EXPECT_TRUE(chrome->keyMode().isEmpty());
    const int seen = keys.size();
    ASSERT_TRUE(pressKey(window, Qt::Key_Left));
    EXPECT_EQ(keys.size(), seen);
    // Pulses and tab switches. A pulse animates, for frames of its own; it
    // lasts 400 ms, and then the window stops asking for frames.
    expectIdle(window);
    const auto beforePulse = window->submittedFrames();
    EXPECT_TRUE(window->pulsePane(first, 2, 400, 1).isEmpty());
    EXPECT_TRUE(waitFor([&] { return window->submittedFrames() >= beforePulse + 3; })) << window->submittedFrames() - beforePulse;
    expectIdle(window);
    QObject::connect(window, &TerminalWindow::tabSwitched, &listener, [&](quint32 from, quint32 to, int direction) {
        EXPECT_EQ(direction, 1);
        switches.append({from, to});
    });
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    ASSERT_TRUE(waitFor([&] { return !switches.isEmpty(); }));
    ASSERT_TRUE(settleQueued());
    ASSERT_EQ(switches.size(), 1);
    EXPECT_NE(switches[0].first, switches[0].second);
}

// Finds the visible chrome item whose text property is @p text.
QQuickItem* chromeItemWithText(ChromeLayer* layer, const QString& text) {
    for (auto* item : visualDescendants(layer->rootItem()))
        if (item->isVisible() && item->property("text").toString() == text) return item;
    return nullptr;
}

TEST_F(NativeWindow, ReturnOnAFocusedButtonAnswersThatButton) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    int confirmed = -1;
    QObject listener;
    QObject::connect(chrome, &ChromeController::confirmAnswered, &listener, [&](bool accepted) { confirmed = accepted; });
    chrome->askConfirm(QStringLiteral("Close tab?"), QStringLiteral("Body"), QStringLiteral("Close"));
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    auto* cancel = chromeItemWithText(layer, QStringLiteral("Cancel"));
    ASSERT_NE(cancel, nullptr);
    cancel->forceActiveFocus();
    sendKey(window, Qt::Key_Return);
    EXPECT_FALSE(chrome->confirmPromptOpen());
    EXPECT_EQ(confirmed, 0);
}

TEST_F(NativeWindow, APanelThatOpensBelowAPromptLeavesItsFocusedButton) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    QList<bool> answers;
    QObject listener;
    QObject::connect(chrome, &ChromeController::pasteAnswered, &listener, [&](bool paste) { answers << paste; });
    chrome->askPaste(QStringLiteral("one\ntwo"), 2);
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    auto* cancel = chromeItemWithText(layer, QStringLiteral("Cancel"));
    ASSERT_NE(cancel, nullptr);
    cancel->forceActiveFocus();
    // Should anything open the search under the prompt (keys, afterglow ctl
    // and scripts cannot), the keys must not go back to the panel, where
    // Return pastes.
    chrome->setSearchOpen(true);
    // The search shows at the next frame and asks for the focus after it.
    ASSERT_TRUE(nextFrame(window));
    ASSERT_TRUE(settleQueued());
    EXPECT_TRUE(cancel->hasActiveFocus());
    sendKey(window, Qt::Key_Return);
    EXPECT_FALSE(chrome->pasteConfirmOpen());
    EXPECT_EQ(answers, QList<bool>{false});
}

TEST_F(NativeWindow, ReturnInThePastePreviewDoesNotPaste) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    chrome->askPaste(QStringLiteral("one\ntwo"), 2);
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    QQuickItem* preview = nullptr;
    for (auto* item : visualDescendants(layer->rootItem()))
        if (item->property("readOnly").toBool() && item->property("text").toString() == QStringLiteral("one\ntwo")) preview = item;
    ASSERT_NE(preview, nullptr);
    preview->forceActiveFocus();
    sendKey(window, Qt::Key_Return);
    EXPECT_TRUE(chrome->pasteConfirmOpen());
    sendKey(window, Qt::Key_Escape);
    EXPECT_FALSE(chrome->pasteConfirmOpen());
}

TEST_F(NativeWindow, ClipboardPromptDeniesOnTypedReturnAndAlwaysAllowsOnPlainA) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    QList<int> answers;
    QObject listener;
    QObject::connect(chrome, &ChromeController::clipboardAnswered, &listener, [&](int answer) { answers << answer; });
    // Return typed for the shell lands on Deny, which holds the focus.
    chrome->askClipboard(QStringLiteral("A program wants to read the clipboard"), QString());
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    sendKey(window, Qt::Key_Return);
    ASSERT_EQ(answers, QList<int>{0});
    // "a" always allows only when pressed on its own.
    chrome->askClipboard(QStringLiteral("A program wants to read the clipboard"), QStringLiteral("a&b <c>"));
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    EXPECT_NE(chromeItemWithText(layer, QStringLiteral("Always allow")), nullptr);
    sendKey(window, Qt::Key_A, Qt::ControlModifier, QStringLiteral("\x01"));
    sendKey(window, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"), true);
    EXPECT_TRUE(chrome->clipboardPromptOpen());
    sendKey(window, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    EXPECT_FALSE(chrome->clipboardPromptOpen());
    EXPECT_EQ(answers, (QList<int>{0, 2}));
}

TEST_F(NativeWindow, AReopenedClipboardPromptStartsOnDeny) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    QList<int> answers;
    QObject listener;
    QObject::connect(chrome, &ChromeController::clipboardAnswered, &listener, [&](int answer) { answers << answer; });
    chrome->askClipboard(QStringLiteral("A program wants to read the clipboard"), QString());
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    auto* allow = chromeItemWithText(layer, QStringLiteral("Allow once"));
    ASSERT_NE(allow, nullptr);
    allow->forceActiveFocus();
    sendKey(window, Qt::Key_Return);
    ASSERT_EQ(answers, QList<int>{1});
    // A hidden item keeps the active focus: the next request must not open
    // on the button the last one was answered with, where a Return typed for
    // the shell would allow it.
    chrome->askClipboard(QStringLiteral("A program wants to read the clipboard"), QString());
    auto* deny = chromeItemWithText(layer, QStringLiteral("Deny"));
    ASSERT_NE(deny, nullptr);
    ASSERT_TRUE(waitFor([&] { return deny->hasActiveFocus(); }));
    EXPECT_FALSE(allow->hasActiveFocus());
    sendKey(window, Qt::Key_Return);
    EXPECT_EQ(answers, (QList<int>{1, 0}));
}

TEST_F(NativeWindow, ClosingAPanelReturnsTheKeysToTheOneBelow) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    chrome->setSearchOpen(true);
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    chrome->openContextMenu({10, 10}, {QVariantMap{{QStringLiteral("id"), 1}, {QStringLiteral("title"), QStringLiteral("Item")}}});
    // contextMenuOpen() is true at once, and the menu takes the keys a turn
    // later. focusedItem() would find the search field all along, so the
    // wait reads the active focus item: the menu's key handler, a child of
    // the item that holds the menu's levels.
    ASSERT_TRUE(waitFor([&] {
        auto* root = layer->rootItem();
        auto* focus = root && root->window() ? root->window()->activeFocusItem() : nullptr;
        return focus && focus->parentItem() && focus->parentItem()->property("currents").isValid();
    }));
    chrome->closeContextMenu();
    // The search field keeps its scoped focus while the menu is open, so
    // focusedItem() finds it at once; the keys reach it only with active focus.
    ASSERT_TRUE(waitFor([&] {
        return chromeFocusReady(layer)
            && layer->focusedItem()->property("placeholderText").toString() == QStringLiteral("Search");
    }));
    sendKey(window, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
    EXPECT_TRUE(layer->focusedItem()->property("text").toString().endsWith(QLatin1Char('a')));
    sendKey(window, Qt::Key_Escape);
    EXPECT_FALSE(chrome->searchOpen());
}

TEST_F(NativeWindow, ReopenedSearchFindsTheKeptQueryAgain) {
    auto* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    pane->withTerminal([](AfterglowVt::Terminal& terminal) { terminal.feed("\x1b[2J\x1b[Hneedle"); });
    chrome->setSearchOpen(true);
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    layer->focusedItem()->setProperty("text", QStringLiteral("needle"));
    ASSERT_TRUE(waitFor([&] { return chrome->searchStatus() == QStringLiteral("1 of 1"); })) << chrome->searchStatus().toStdString();
    chrome->setSearchOpen(false);
    EXPECT_TRUE(chrome->searchStatus().isEmpty());
    chrome->setSearchOpen(true);
    EXPECT_TRUE(waitFor([&] { return chrome->searchStatus() == QStringLiteral("1 of 1"); })) << chrome->searchStatus().toStdString();
}

TEST_F(NativeWindow, TitlePromptFollowsANewRequest) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    chrome->askTitle(QStringLiteral("Tab title"), QStringLiteral("first"));
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    EXPECT_EQ(layer->focusedItem()->property("text").toString(), QStringLiteral("first"));
    chrome->askTitle(QStringLiteral("Split title"), QStringLiteral("second"));
    // The new request's field holds the keys again, with its own text.
    EXPECT_TRUE(waitFor([&] {
        return chromeFocusReady(layer) && layer->focusedItem()->property("text").toString() == QStringLiteral("second");
    }));
}

TEST_F(NativeWindow, PaneTitleLabelsSurviveTitleChanges) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    ASSERT_NE(window->splitPane(first, SplitTree::Direction::Right, launch, nullptr), nullptr);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    const auto labels = [&] {
        QList<QQuickItem*> found;
        if (!layer->rootItem()) return found; // the scene is made in the background and may not be ready yet
        for (auto* item : visualDescendants(layer->rootItem()))
            if (item->property("scrollbarSpace").isValid()) found.append(item);
        return found;
    };
    QList<QQuickItem*> before;
    ASSERT_TRUE(waitFor([&] {
        before = labels();
        return before.size() == 2;
    }));
    first->setUserTitle(QStringLiteral("renamed"));
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(waitFor([&] {
        const auto titles = chrome->paneTitles();
        return std::any_of(titles.begin(), titles.end(), [](const QVariant& entry) {
            return entry.toMap().value(QStringLiteral("title")).toString() == QStringLiteral("renamed");
        });
    }));
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(labels(), before);
}

TEST_F(NativeWindow, OverviewReopenedWhileClosingTurnsTheShortWay) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(window->splitPane(second, SplitTree::Direction::Down, launch, nullptr), nullptr);
    window->revealPane(first);
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    ASSERT_TRUE(settleQueued());
    // Seven steps round three cards: the position counts on past two turns.
    for (int step = 0; step < 7; ++step) {
        sendKey(window, Qt::Key_Right);
        drain(20);
    }
    // The deck reaches the seventh step before the close starts from it.
    ASSERT_TRUE(waitFor([&] {
        const auto [position, target] = window->overviewPosition();
        return std::abs(target - position) < 0.01;
    }));
    sendKey(window, Qt::Key_Escape);
    ASSERT_FALSE(window->overviewOpen());
    // Reopen within the closing motion: the deck turns at most half round.
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    const auto [position, target] = window->overviewPosition();
    EXPECT_LE(std::abs(target - position), 1.5) << position << " -> " << target;
    sendKey(window, Qt::Key_Escape);
    expectIdle(window);
}

TEST_F(NativeWindow, OverviewRestoresTheZoomOnTheTabItCameFrom) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    window->revealPane(first);
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(window->runAction(first, Action::TogglePaneZoom));
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    auto* other = window->focusedPane();
    ASSERT_NE(other, first);
    window->revealPane(first);
    ASSERT_TRUE(waitFor([&] { // zoomed on first
        return !window->accessiblePaneRect(first).isEmpty() && window->accessiblePaneRect(second).isEmpty();
    }));
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    // Every pane of the zoomed tab is a card while the overview shows.
    EXPECT_TRUE(waitFor([&] { return !window->accessiblePaneRect(second).isEmpty(); }));
    // Something else switches tabs while the overview is open (a notification
    // click, afterglow ctl focus); Escape returns to the first tab.
    window->revealPane(other);
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(window->overviewOpen());
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &escape);
    ASSERT_TRUE(settleQueued());
    EXPECT_FALSE(window->overviewOpen());
    // The first tab is back, with its zoom on the chosen pane.
    EXPECT_TRUE(waitFor([&] { return window->focusedPane() == first; }));
    EXPECT_TRUE(window->accessiblePaneRect(second).isEmpty()) << "the first tab lost its zoom";
    EXPECT_FALSE(window->accessiblePaneRect(first).isEmpty());
}

TEST_F(NativeWindow, OverviewWaitsWhileTheChromeHasTheKeyboard) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    chrome->setSearchOpen(true);
    ASSERT_TRUE(waitFor([&] { return chrome->wantsKeyboard(); }));
    EXPECT_FALSE(window->openOverview());
    EXPECT_FALSE(window->overviewOpen());
    chrome->setSearchOpen(false);
    ASSERT_TRUE(waitFor([&] { return !chrome->wantsKeyboard(); }));
    EXPECT_TRUE(window->openOverview());
    EXPECT_TRUE(window->overviewOpen());
}

TEST_F(NativeWindow, PaneCoalescesBellsAndRepeatedTitlesAndUserVars) {
    auto* pane = window->focusedPane();
    int bells = 0;
    QStringList titles;
    QStringList vars;
    QObject listener;
    QObject::connect(pane, &Pane::bellRang, &listener, [&] { ++bells; });
    QObject::connect(pane, &Pane::titleUpdated, &listener, [&](const QString& title) { titles.append(title); });
    QObject::connect(pane, &Pane::userVarUpdated, &listener, [&](const QString&, const QString& value) { vars.append(value); });
    // A burst of BEL bytes is one bell for the GUI thread, not one per byte.
    // The feed queues every delivery that it makes before it returns.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\a\a\a\a\a\a\a\a"); });
    ASSERT_TRUE(waitFor([&] { return bells > 0; }));
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(bells, 1);
    // cat writes the input back: the same title and user variable, twice,
    // then other ones that arrive after any repeat would.
    // The line end lets the canonical-mode PTY hand the line to cat.
    const std::string_view twice = "\x1b]2;same\x07\x1b]2;same\x07"
                                   "\x1b]1337;SetUserVar=k=dg==\x07\x1b]1337;SetUserVar=k=dg==\x07"
                                   "\x1b]2;after\x07\x1b]1337;SetUserVar=k=YWZ0ZXI=\x07\n";
    ASSERT_TRUE(pane->sendInput(twice));
    ASSERT_TRUE(waitFor([&] {
        return !titles.isEmpty() && titles.last() == QStringLiteral("after") && !vars.isEmpty()
            && vars.last() == QStringLiteral("after");
    })) << titles.join(QLatin1Char(',')).toStdString();
    ASSERT_TRUE(settleQueued());
    // A title or a value that a newer one replaced before the GUI thread got
    // it may be left out, and none comes twice.
    EXPECT_LE(titles.size(), 2) << titles.join(QLatin1Char(',')).toStdString();
    EXPECT_EQ(titles.removeDuplicates(), 0);
    EXPECT_LE(vars.size(), 2) << vars.join(QLatin1Char(',')).toStdString();
    EXPECT_EQ(vars.removeDuplicates(), 0);
    const qsizetype before = titles.size();
    pane->setUserTitle(QStringLiteral("mine"));
    pane->setUserTitle(QStringLiteral("mine"));
    EXPECT_EQ(titles.size(), before + 1);
    // A burst of titles reaches the GUI thread as one delivery of the
    // newest, not one for each title.
    pane->setUserTitle({});
    titles.clear();
    pane->withTerminal([](AfterglowVt::Terminal& t) {
        for (int i = 0; i < 1000; ++i) t.feed("\x1b]2;t" + std::to_string(i) + "\x07");
    });
    ASSERT_TRUE(waitFor([&] { return !titles.isEmpty() && titles.last() == QStringLiteral("t999"); }));
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(titles, QStringList{QStringLiteral("t999")});
}

// In this suite, not an offscreen one: the keys map through the keymap of the
// compositor's seat. Without a seat, mapKeyEvent takes its keycode table.
TEST(KeyMapping, KeepsKeypadAndShiftedKeysUnderCtrl) {
    QByteArray storage;
    // Ctrl turns keypad 2 into NUL: it is still keypad 2 (xkb keycode 88).
    const QKeyEvent keypad(QEvent::KeyPress, Qt::Key_2, Qt::ControlModifier | Qt::KeypadModifier, 88, 0, 0,
                           QString(QChar(0)));
    EXPECT_EQ(mapKeyEvent(&keypad, storage).key, AfterglowVt::Key::Keypad2);
    // Ctrl+Shift+A: the text is ^A, the shifted key is still A (keycode 38).
    const QKeyEvent letter(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier | Qt::ShiftModifier, 38, 0, 0,
                           QString(QChar(1)));
    const auto mapped = mapKeyEvent(&letter, storage);
    EXPECT_EQ(mapped.key, AfterglowVt::Key::Text);
    EXPECT_EQ(mapped.unshifted, U'a');
    EXPECT_EQ(mapped.shifted, U'A');
    // NumLock off: the keypad's navigation keys stay keypad keys.
    const QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, Qt::KeypadModifier);
    EXPECT_EQ(mapKeyEvent(&left, storage).key, AfterglowVt::Key::KeypadLeft);
    const QKeyEvent begin(QEvent::KeyPress, Qt::Key_Clear, Qt::KeypadModifier);
    EXPECT_EQ(mapKeyEvent(&begin, storage).key, AfterglowVt::Key::KeypadBegin);
    const QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    EXPECT_EQ(mapKeyEvent(&home, storage).key, AfterglowVt::Key::Home);
    const QKeyEvent separator(QEvent::KeyPress, Qt::Key_Comma, Qt::KeypadModifier, QStringLiteral(","));
    EXPECT_EQ(mapKeyEvent(&separator, storage).key, AfterglowVt::Key::KeypadSeparator);
    const QKeyEvent f13(QEvent::KeyPress, Qt::Key_F13, Qt::NoModifier);
    EXPECT_EQ(mapKeyEvent(&f13, storage).key, AfterglowVt::Key::F13);
    const QKeyEvent f35(QEvent::KeyPress, Qt::Key_F35, Qt::NoModifier);
    EXPECT_EQ(mapKeyEvent(&f35, storage).key, AfterglowVt::Key::F35);
}

// --- focus, keys, the overview's close, splits, menus, docks and control ---

TEST_F(NativeWindow, FocusStaysWhenAnUnfocusedPaneExitsOrABackgroundTabLeaves) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    auto* third = window->splitPane(second, SplitTree::Direction::Down, launch, nullptr);
    ASSERT_NE(third, nullptr);
    ASSERT_EQ(window->focusedPane(), third);
    // The first pane closes: the tab entered with it focused, but the
    // focus is on the third now and stays there.
    window->requestClosePane(first);
    ASSERT_TRUE(waitFor([&] { return window->panes().size() == 2; }));
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(window->focusedPane(), third);
    // A background tab leaves for another window: the focus stays too.
    auto* background = window->openTab(launch, nullptr);
    ASSERT_NE(background, nullptr);
    window->revealPane(third);
    ASSERT_EQ(window->focusedPane(), third);
    WindowOptions empty;
    empty.empty = true;
    auto* destination = application->newWindow(empty);
    ASSERT_NE(destination, nullptr);
    ASSERT_TRUE(window->detachTabTo(background, destination).isEmpty());
    EXPECT_TRUE(waitFor([&] { return destination->panes() == QList<Pane*>{background}; }));
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(window->focusedPane(), third);
}

TEST_F(NativeWindow, KeyRepeatsOfAKeyThatClosedAModeStayOutOfTheShell) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    window->revealPane(first);
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    ASSERT_TRUE(window->overviewOpen());
    // "2" chooses the second card and closes the overview. The key stays
    // down: Wayland repeats it as release and press pairs, then releases it.
    QKeyEvent down(QEvent::KeyPress, Qt::Key_2, Qt::NoModifier, QStringLiteral("2"));
    QCoreApplication::sendEvent(window, &down);
    ASSERT_FALSE(window->overviewOpen());
    ASSERT_EQ(window->focusedPane(), second);
    for (int i = 0; i < 3; ++i) {
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_2, Qt::NoModifier, QStringLiteral("2"), true);
        QCoreApplication::sendEvent(window, &up);
        QKeyEvent again(QEvent::KeyPress, Qt::Key_2, Qt::NoModifier, QStringLiteral("2"), true);
        QCoreApplication::sendEvent(window, &again);
    }
    QKeyEvent up(QEvent::KeyRelease, Qt::Key_2, Qt::NoModifier, QStringLiteral("2"));
    QCoreApplication::sendEvent(window, &up);
    // A new press does reach the shell.
    sendKey(window, Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    sendKey(window, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    EXPECT_TRUE(waitFor([&] { return screenOf(second).contains(QLatin1Char('y')); }));
    EXPECT_FALSE(screenOf(second).contains(QLatin1Char('2')));
}

TEST_F(NativeWindow, SearchDropsMatchesOfTextThatMovedAndStartsAgain) {
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* pane = window->splitPane(window->focusedPane(), SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(pane, nullptr);
    QStringList statuses;
    QObject listener;
    QObject::connect(pane, &Pane::searchStatusChanged, &listener, [&](const QString& status) { statuses << status; });
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[2J\x1b[Hone findme\r\ntwo findme\r\n"); });
    pane->search(QStringLiteral("findme"), 0);
    // A program inserts a line above both while the search takes its rows:
    // the text moves between rows inside the scroll region.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[H\x1b[L"); });
    const auto said = [&](const QString& text) {
        return std::any_of(statuses.begin(), statuses.end(), [&](const QString& s) { return s.contains(text); });
    };
    ASSERT_TRUE(waitFor([&] { return said(QStringLiteral("Text moved during the search")); }))
        << statuses.join(QLatin1Char('|')).toStdString();
    // The same query searches again, now over the moved text.
    statuses.clear();
    pane->search(QStringLiteral("findme"), 1);
    ASSERT_FALSE(statuses.isEmpty());
    EXPECT_EQ(statuses.first(), QStringLiteral("Searching…"));
    EXPECT_TRUE(waitFor([&] { return said(QStringLiteral("of 2")); })) << statuses.join(QLatin1Char('|')).toStdString();
    EXPECT_FALSE(said(QStringLiteral("moved")));
}

TEST_F(NativeWindow, KeyRepeatsStayWithThePaneThatGotThePress) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    // The word shows once echo is off: a key sent before would echo. It
    // holds none of the letters typed below.
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; printf started; exec cat")};
    auto* middle = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(middle, nullptr);
    auto* right = window->splitPane(middle, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(right, nullptr);
    for (Pane* pane : {middle, right}) {
        ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("started")); })) << screenOf(pane).toStdString();
    }
    window->revealPane(middle);
    ASSERT_TRUE(settleQueued());
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    QCoreApplication::sendEvent(window, &down);
    // The focus moves while the key is held (a click, afterglow ctl, a script).
    window->revealPane(right);
    for (int i = 0; i < 3; ++i) {
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"), true);
        QCoreApplication::sendEvent(window, &up);
        QKeyEvent again(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"), true);
        QCoreApplication::sendEvent(window, &again);
    }
    QKeyEvent up(QEvent::KeyRelease, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    QCoreApplication::sendEvent(window, &up);
    sendKey(window, Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    sendKey(window, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    EXPECT_TRUE(waitFor([&] { return screenOf(right).contains(QLatin1Char('y')); }));
    EXPECT_FALSE(screenOf(right).contains(QLatin1Char('q')));
    // The press reached the pane that got it. Its repeats after the focus
    // moved went to no pane: the user no longer types there.
    ASSERT_TRUE(middle->sendInput("\n"));
    EXPECT_TRUE(waitFor([&] { return screenOf(middle).contains(QLatin1Char('q')); }));
    EXPECT_FALSE(screenOf(middle).contains(QStringLiteral("qq")))
        << screenOf(middle).toStdString();
}

// Opens a second tab and the overview on it, and chooses the first tab's
// card with Left and Return. The chosen tab shows at once: the overview
// closes onto it, so the cards never jump back to the second tab.
void NativeWindow::closeOverviewOntoTheFirstTab(Pane*& first, Pane*& second) {
    first = window->focusedPane();
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); })); // the new tab's own slide
    second = window->focusedPane();
    ASSERT_NE(second, first);
    ASSERT_TRUE(window->runAction(second, Action::PaneOverview));
    // Frames open the overview. Choosing before any frame has opened it would
    // leave nothing for the close to draw.
    const auto opened = window->submittedFrames();
    ASSERT_TRUE(waitFor([&] { return window->submittedFrames() >= opened + 3; }));
    sendKey(window, Qt::Key_Left);
    ASSERT_EQ(window->overviewHighlight(), first);
    sendKey(window, Qt::Key_Return);
    ASSERT_FALSE(window->overviewOpen());
    ASSERT_EQ(window->focusedPane(), first);
}

TEST_F(NativeWindow, TypingWhileTheOverviewClosesReachesTheChosenTab) {
    Pane* first = nullptr;
    Pane* second = nullptr;
    closeOverviewOntoTheFirstTab(first, second);
    if (HasFatalFailure()) return;
    // The chosen tab is focused while the overview closes: a key typed now
    // reaches its pane, whose cat shows the line once it ends.
    sendKey(window, Qt::Key_Z, Qt::NoModifier, QStringLiteral("z"));
    EXPECT_EQ(window->focusedPane(), first);
    sendKey(window, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    EXPECT_TRUE(waitFor([&] { return screenOf(first).contains(QLatin1Char('z')); }))
        << screenOf(first).toStdString();
    expectIdle(window);
    EXPECT_EQ(window->focusedPane(), first);
}

TEST_F(NativeWindow, OverviewClosingOntoAnotherTabSwitchesWithoutATransition) {
    Pane* first = nullptr;
    Pane* second = nullptr;
    closeOverviewOntoTheFirstTab(first, second);
    if (HasFatalFailure()) return;
    // The switch happened while the overview was showing, so the tab
    // transition did not run, and no slide follows the close. The chosen
    // tab has the focus at once: the frames of the close are what to watch,
    // for longer than a slide lasts.
    ASSERT_EQ(window->focusedPane(), first);
    bool slid = false;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 1000) {
        drain(5);
        slid = slid || window->tabSwitching();
    }
    EXPECT_EQ(window->focusedPane(), first);
    EXPECT_FALSE(slid);
    EXPECT_FALSE(window->tabSwitching());
    expectIdle(window);
    // An ordinary switch afterwards still slides.
    window->revealPane(second);
    EXPECT_TRUE(window->tabSwitching());
}

TEST_F(NativeWindow, FocusWhileTheOverviewClosesWinsOverTheCard) {
    Pane* first = nullptr;
    Pane* second = nullptr;
    closeOverviewOntoTheFirstTab(first, second);
    if (HasFatalFailure()) return;
    // afterglow ctl focus on another pane while the overview closes wins over
    // the card the overview chose.
    window->revealPane(second);
    expectIdle(window);
    EXPECT_EQ(window->focusedPane(), second);
    EXPECT_TRUE(window->accessiblePaneRect(first).isEmpty());
}

TEST_F(NativeWindow, ASplitRefusedForRoomKeepsTheZoom) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* last = first;
    bool refused = false;
    for (int i = 0; i < 64 && !refused; ++i) {
        Pane* added = window->splitPane(last, SplitTree::Direction::Down, launch, nullptr);
        if (added) last = added;
        else refused = true;
    }
    ASSERT_TRUE(refused);
    ASSERT_NE(last, first);
    ASSERT_TRUE(window->runAction(last, Action::TogglePaneZoom));
    ASSERT_TRUE(waitFor([&] { // zoomed on last
        return window->accessiblePaneRect(first).isEmpty() && !window->accessiblePaneRect(last).isEmpty();
    }));
    QString error;
    EXPECT_EQ(window->splitPane(last, SplitTree::Direction::Down, launch, &error), nullptr);
    EXPECT_EQ(error, TerminalWindow::noRoomForPane());
    EXPECT_TRUE(window->accessiblePaneRect(first).isEmpty());
    EXPECT_FALSE(window->accessiblePaneRect(last).isEmpty());
    // afterglow ctl and scripts get the lowercase text of their other errors.
    SettingsStore settings(directory.filePath(QStringLiteral("control.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    quint32 created = 0;
    EXPECT_EQ(control.split(last->id(), QStringLiteral("down"), {QStringLiteral("/bin/cat")}, {}, created),
              QStringLiteral("not enough room for another pane"));
    EXPECT_EQ(created, 0u);
}

TEST_F(NativeWindow, ClipboardReadFromABackgroundTabAsksWithoutSwitching) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* background = window->openTab(launch, nullptr);
    ASSERT_NE(background, nullptr);
    window->revealPane(first);
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    const quint32 tab = window->activeTabId();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    // The request comes from the PTY reader thread and crosses to the GUI thread.
    requestClipboardRead(background);
    ASSERT_TRUE(waitFor([&] { return chrome->clipboardPromptOpen(); }));
    // The prompt names where the program is; the user's tab and focus stay.
    EXPECT_EQ(window->activeTabId(), tab);
    EXPECT_EQ(window->focusedPane(), first);
    EXPECT_TRUE(chrome->clipboardPromptText().contains(QStringLiteral("tab 2")));
    chrome->answerClipboard(0);
}

TEST_F(NativeWindow, ScriptKeyModeEndsWithItsReservedChord) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    int keys = 0;
    QObject listener;
    QObject::connect(window, &TerminalWindow::scriptKey, &listener, [&](const QString&, quint32, const QVariantMap&) { ++keys; });
    ASSERT_TRUE(window->startScriptKeyMode(QStringLiteral("script@1"), window->focusedPane(), QStringLiteral("Script keys")).isEmpty());
    // A modifier on its own is not a key for the script.
    sendKey(window, Qt::Key_Shift, Qt::ShiftModifier);
    EXPECT_EQ(keys, 0);
    sendKey(window, Qt::Key_Escape, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(keys, 0);
    EXPECT_TRUE(chrome->keyMode().isEmpty());
}

// The chrome scene is made in the background from the window's creation, so
// the first frame that shows chrome (here the pane titles of the first split)
// renders it and does not build it. Building it there held that frame for
// 215-280 ms.
TEST_F(NativeWindow, ChromeSceneIsMadeBeforeTheFirstFrameThatShowsIt) {
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(chrome->paneTitles().isEmpty());
    EXPECT_TRUE(waitFor([&] { return layer->rootItem() != nullptr; }));
    // Made, not rendered: no frame has shown chrome yet.
    EXPECT_EQ(layer->texture(), nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    ASSERT_NE(window->splitPane(window->focusedPane(), SplitTree::Direction::Right, launch, nullptr), nullptr);
    QElapsedTimer timer;
    timer.start();
    QEvent update(QEvent::UpdateRequest);
    QCoreApplication::sendEvent(window, &update);
    const qint64 elapsed = timer.elapsed();
    EXPECT_FALSE(chrome->paneTitles().isEmpty());
    EXPECT_NE(layer->texture(), nullptr);
    // A wall-clock bound would fail on a loaded machine: the time is reported.
    RecordProperty("firstChromeFrameMs", int(elapsed));
    std::fprintf(stderr, "first frame with chrome: %lld ms\n", static_cast<long long>(elapsed));
    expectIdle(window);
}

TEST_F(NativeWindow, SplitsKeepTheMinimumSizeAndStartShellsAtTheirSizeBeforeAFrame) {
    auto* first = window->focusedPane();
    PaneLaunchOptions narrow;
    narrow.command = {QStringLiteral("/bin/cat")};
    narrow.fraction = 0.0005;
    auto* second = window->splitPane(first, SplitTree::Direction::Right, narrow, nullptr);
    ASSERT_NE(second, nullptr);
    // No frame in between: the layout of the first split is not drawn yet.
    PaneLaunchOptions half;
    half.command = {QStringLiteral("/bin/cat")};
    auto* third = window->splitPane(second, SplitTree::Direction::Down, half, nullptr);
    ASSERT_NE(third, nullptr);
    EXPECT_GE(second->cols(), 4);
    EXPECT_GE(third->cols(), 4);
    EXPECT_GT(third->rows(), 2);
    // The next frame lays the splits out. The chrome scene is made in the
    // background and may not be ready yet, which takes longer than a fixed wait.
    ASSERT_TRUE(waitFor([&] { return !window->accessiblePaneRect(second).isEmpty(); }));
    // The rectangle is in pixels: the minimum is in cells.
    ASSERT_TRUE(nextFrame(window));
    EXPECT_GE(second->cols(), 4);
    EXPECT_GE(third->cols(), 4);
}

TEST_F(NativeWindow, ContextMenuActsOnThePaneItOpenedOn) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(waitFor([&] {
        return !window->accessiblePaneRect(first).isEmpty() && !window->accessiblePaneRect(second).isEmpty();
    }));
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    const QPointF at = window->mapFromGlobal(window->accessiblePaneRect(first).center());
    clickRight(window, at);
    ASSERT_TRUE(chrome->contextMenuOpen());
    // The focus moves while the menu is open (a script, afterglow ctl, a clipboard request).
    window->revealPane(second);
    chrome->activateMenuItem(int(Action::RenamePane));
    chrome->answerTitle(QStringLiteral("menu-pane"));
    EXPECT_EQ(first->userTitle(), QStringLiteral("menu-pane"));
    EXPECT_NE(second->userTitle(), QStringLiteral("menu-pane"));
}

TEST_F(NativeWindow, ATabThatClosesTakesItsTabMenuWithIt) {
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("read line")};
    auto* other = window->openTab(launch, nullptr);
    ASSERT_NE(other, nullptr);
    ASSERT_TRUE(settleQueued());
    ASSERT_EQ(window->tabs().size(), 2);
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    chrome->tabMenu(1, 50, 15);
    ASSERT_TRUE(chrome->contextMenuOpen());
    // Its program ends: the tab closes, and a menu left open would act on nothing.
    ASSERT_TRUE(other->sendInput("\n"));
    ASSERT_TRUE(waitFor([&] { return window->tabs().size() == 1; }));
    EXPECT_FALSE(chrome->contextMenuOpen());
}

TEST_F(NativeWindow, ATabThatClosesLeavesAPaneMenuOpen) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("read line")};
    auto* other = window->openTab(launch, nullptr);
    ASSERT_NE(other, nullptr);
    ASSERT_TRUE(settleQueued());
    ASSERT_EQ(window->tabs().size(), 2);
    window->revealPane(first);
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    // The tab menu of the background tab, dismissed without a choice.
    chrome->tabMenu(1, 50, 15);
    ASSERT_TRUE(chrome->contextMenuOpen());
    chrome->closeContextMenu();
    ASSERT_TRUE(waitFor([&] { return !window->accessiblePaneRect(first).isEmpty(); }));
    const QPointF at = window->mapFromGlobal(window->accessiblePaneRect(first).center());
    clickRight(window, at);
    ASSERT_TRUE(chrome->contextMenuOpen());
    // The other tab's program ends: its tab closes, and the pane menu, which
    // belongs to the first tab, stays open.
    ASSERT_TRUE(other->sendInput("\n"));
    ASSERT_TRUE(waitFor([&] { return window->tabs().size() == 1; }));
    EXPECT_TRUE(chrome->contextMenuOpen());
}

TEST_F(NativeWindow, ScriptDocksTakeCellsFromThePanesAndCanReplaceTheTabBar) {
    auto* pane = window->focusedPane();
    // The window settles at its size first: the grid is the baseline.
    expectIdle(window);
    const int rows = pane->rows(), cols = pane->cols();
    const QVariantList line{QVariantMap{{QStringLiteral("text"), QStringLiteral("one")}, {QStringLiteral("item"), QStringLiteral("first")}},
                            QVariantMap{{QStringLiteral("text"), QStringLiteral(" | ")}}};
    auto dock = [&](const QString& owner, const QString& id, const QString& edge, int size, bool tabBar = false) {
        return window->setScriptDock(owner, {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), id},
                                             {QStringLiteral("edge"), edge}, {QStringLiteral("size"), size},
                                             {QStringLiteral("tab_bar"), tabBar}, {QStringLiteral("lines"), QVariantList{line, line}}});
    };
    ASSERT_TRUE(dock(QStringLiteral("a@1"), QStringLiteral("status"), QStringLiteral("bottom"), 2).isEmpty());
    ASSERT_TRUE(dock(QStringLiteral("a@1"), QStringLiteral("side"), QStringLiteral("left"), 10).isEmpty());
    EXPECT_TRUE(waitFor([&] { return pane->rows() == rows - 2 && pane->cols() == cols - 10; }))
        << pane->rows() << "x" << pane->cols() << " from " << rows << "x" << cols;
    expectIdle(window);

    // A click on an item reaches its script. The rest of a dock is not the pane's.
    QList<QVariantMap> clicks;
    QObject listener;
    QObject::connect(window, &TerminalWindow::scriptDockEvent, &listener,
                     [&](const QString& owner, const QString& handler, quint32, const QVariantMap& click) {
                         if (owner == QLatin1String("a@1") && handler == QLatin1String("on_dock_click")) clicks.append(click);
                     });
    // The bottom dock spans the window; the left one fits above it.
    const qreal bottomRow = window->height() - 2;
    clickLeft(window, QPointF(2, bottomRow));
    clickLeft(window, QPointF(2, 2));
    ASSERT_EQ(clicks.size(), 2);
    EXPECT_EQ(clicks[0][QStringLiteral("dock")], QStringLiteral("status"));
    EXPECT_EQ(clicks[1][QStringLiteral("dock")], QStringLiteral("side"));
    EXPECT_EQ(clicks[1][QStringLiteral("item")], QStringLiteral("first"));
    EXPECT_EQ(clicks[1][QStringLiteral("button")], QStringLiteral("left"));
    clickLeft(window, QPointF(window->width() - 2, bottomRow)); // the status dock, past its text
    EXPECT_EQ(clicks.size(), 2);

    // With two tabs the tab bar shows, until a top dock takes its place.
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* other = window->openTab(launch, nullptr);
    ASSERT_NE(other, nullptr);
    window->revealPane(pane);
    ASSERT_TRUE(waitFor([&] { return chrome->tabBarHeight() > 0; }));
    ASSERT_TRUE(dock(QStringLiteral("b@1"), QStringLiteral("tabs"), QStringLiteral("top"), 1, true).isEmpty());
    EXPECT_TRUE(waitFor([&] { return chrome->tabBarHeight() == 0; }));
    // A script's docks go with it, and the space comes back.
    window->clearScriptOwner(QStringLiteral("b@1"));
    EXPECT_TRUE(waitFor([&] { return chrome->tabBarHeight() > 0; }));
    window->clearScriptOwner(QStringLiteral("a@1"));
    ASSERT_TRUE(other->sendInput("\x04")); // its cat ends, and the tab with it
    ASSERT_TRUE(waitFor([&] { return window->tabs().size() == 1; }));
    EXPECT_TRUE(waitFor([&] { return pane->rows() == rows && pane->cols() == cols; }))
        << pane->rows() << "x" << pane->cols();
}

TEST_F(NativeWindow, DockClicksReachTheScriptThroughControlAndNoPointerInputReachesThePanes) {
    SettingsStore settings(directory.filePath(QStringLiteral("docks.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    auto* pane = window->focusedPane();
    ASSERT_NE(pane, nullptr);
    pane->setScrollPolicy(false, true);
    pane->withTerminal([](AfterglowVt::Terminal& t) {
        for (int i = 0; i < 200; ++i) t.feed("line\r\n");
    });
    expectIdle(window);
    const int rows = pane->rows();
    const QVariantList line{QVariantMap{{QStringLiteral("text"), QStringLiteral("one")}, {QStringLiteral("item"), QStringLiteral("first")}}};
    // Through Control, as ScriptControl applies a script's dock effect.
    ASSERT_TRUE(control.scriptDock(QStringLiteral("a@1"), pane->id(),
                                   {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("status")},
                                    {QStringLiteral("edge"), QStringLiteral("bottom")}, {QStringLiteral("size"), 1},
                                    {QStringLiteral("lines"), QVariantList{line}}})
                    .isEmpty());
    ASSERT_TRUE(waitFor([&] { return pane->rows() == rows - 1; })) << pane->rows();
    // The click goes window, Control, then the script host's queued hop.
    QList<QVariantMap> clicks;
    QObject listener;
    QObject::connect(&control, &Control::scriptDockEvent, &listener,
                     [&](const QString& owner, const QString& handler, quint32 from, const QVariantMap& click) {
                         if (owner == QLatin1String("a@1") && handler == QLatin1String("on_dock_click") && from == pane->id())
                             clicks.append(click);
                     });
    const QPointF onItem(2, window->height() - 2);
    const QPointF onPane(window->width() / 2.0, window->height() / 2.0);
    // A drag from the item: the press clicks, and the drag selects nothing
    // in the pane, which would otherwise take the press as the start of a
    // selection.
    mouseAt(window, QEvent::MouseButtonPress, onItem, Qt::LeftButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseMove, onItem + QPointF(40, 0), Qt::NoButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseButtonRelease, onItem + QPointF(40, 0), Qt::LeftButton, Qt::NoButton);
    ASSERT_EQ(clicks.size(), 1);
    EXPECT_EQ(clicks[0][QStringLiteral("item")], QStringLiteral("first"));
    EXPECT_FALSE(pane->hasSelection());
    EXPECT_EQ(window->focusedPane(), pane);

    // The wheel over the dock scrolls no pane; over the pane it does.
    // A scroll eases from where it is: its first frame can still show the
    // bottom. Once nothing moves, any scroll would show.
    wheelAt(window, onItem);
    expectIdle(window);
    EXPECT_EQ(viewportOffset(pane), 0u);
    wheelAt(window, onPane);
    EXPECT_TRUE(waitFor([&] { return viewportOffset(pane) > 0; }));
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.scrollViewport(-int(t.screen().viewportOffset())); });

    // A right click on the dock while the left button selects in the pane:
    // the selection goes on, and the left release still ends it.
    mouseAt(window, QEvent::MouseButtonPress, onPane, Qt::LeftButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseMove, onPane + QPointF(40, 0), Qt::NoButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseButtonPress, onItem, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
    mouseAt(window, QEvent::MouseButtonRelease, onItem, Qt::RightButton, Qt::LeftButton);
    const QString before = pane->selectedText();
    mouseAt(window, QEvent::MouseMove, onPane + QPointF(0, 40), Qt::NoButton, Qt::LeftButton);
    EXPECT_NE(pane->selectedText(), before);
    mouseAt(window, QEvent::MouseButtonRelease, onPane + QPointF(0, 40), Qt::LeftButton, Qt::NoButton);
    ASSERT_EQ(clicks.size(), 2);
    EXPECT_EQ(clicks[1][QStringLiteral("button")], QStringLiteral("right"));
    pane->clearSelection();

    // A dock press whose release went elsewhere takes nothing from the next
    // gesture: a click on the pane focuses and selects as usual.
    // Away from the last click, so that this is no double click.
    const QPointF elsewhere = onPane - QPointF(100, 60);
    mouseAt(window, QEvent::MouseButtonPress, onItem, Qt::LeftButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseButtonPress, elsewhere, Qt::LeftButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseMove, elsewhere + QPointF(40, 0), Qt::NoButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseButtonRelease, elsewhere + QPointF(40, 0), Qt::LeftButton, Qt::NoButton);
    EXPECT_TRUE(pane->hasSelection());
    pane->clearSelection();
    // The click goes out on the press, so the lost release does not take it.
    ASSERT_EQ(clicks.size(), 3);

    // With the overview open, a click on the dock is the dock's: the
    // overview stays open.
    QVariantMap state;
    ASSERT_TRUE(window->scriptOverview(QStringLiteral("script@1"), pane, QStringLiteral("open"), state).isEmpty());
    mouseAt(window, QEvent::MouseButtonPress, onItem, Qt::LeftButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseButtonRelease, onItem, Qt::LeftButton, Qt::NoButton);
    EXPECT_TRUE(window->overviewOpen());
    ASSERT_EQ(clicks.size(), 4);
    ASSERT_TRUE(window->scriptOverview(QStringLiteral("script@1"), pane, QStringLiteral("close"), state).isEmpty());

    // A larger font places the dock again, and its item still takes clicks.
    ASSERT_TRUE(window->runAction(pane, Action::FontBigger));
    const int cols = pane->cols();
    EXPECT_TRUE(waitFor([&] { return pane->cols() < cols || pane->rows() < rows - 1; }));
    expectIdle(window);
    mouseAt(window, QEvent::MouseButtonPress, onItem, Qt::LeftButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseButtonRelease, onItem, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(clicks.size(), 5);
    ASSERT_TRUE(window->runAction(pane, Action::FontReset));
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, DocksTellTheirScriptOfTheirCellsTheHoveredItemAndTheWheel) {
    auto* pane = window->focusedPane();
    ASSERT_NE(pane, nullptr);
    expectIdle(window);
    DockEvents events(window);
    auto of = [&](const char* handler) {
        QList<QVariantMap> found;
        for (const DockEvent& e : events.of(handler)) found.append(e.event);
        return found;
    };
    auto dock = [&](int size, const QVariantList& runs) {
        return window->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("bar"), QStringLiteral("bottom"), size, runs));
    };
    const QVariantList line{dockRun(QStringLiteral("one"), QStringLiteral("first")), dockRun(QStringLiteral(" ")),
                            dockRun(QStringLiteral("two"), QStringLiteral("second"))};

    // The first placement tells the dock's cells: the window's width.
    const int windowCols = window->gridSize().width();
    ASSERT_GT(windowCols, 0);
    ASSERT_TRUE(dock(1, line).isEmpty());
    ASSERT_TRUE(waitFor([&] { return !of("on_dock_layout").isEmpty(); }));
    QVariantMap layout = of("on_dock_layout").constLast();
    EXPECT_EQ(layout[QStringLiteral("dock")], QStringLiteral("bar"));
    EXPECT_EQ(layout[QStringLiteral("scope")], QStringLiteral("window"));
    EXPECT_EQ(layout[QStringLiteral("window")].toUInt(), window->id());
    EXPECT_TRUE(layout[QStringLiteral("shown")].toBool());
    EXPECT_EQ(layout[QStringLiteral("cols")].toInt(), windowCols);
    EXPECT_EQ(layout[QStringLiteral("rows")].toInt(), 1);
    // New text in the same place tells nothing.
    expectIdle(window);
    const auto layouts = of("on_dock_layout").size();
    ASSERT_TRUE(dock(1, {dockRun(QStringLiteral("one"), QStringLiteral("first")), dockRun(QStringLiteral(" ")),
                         dockRun(QStringLiteral("two"), QStringLiteral("second")), dockRun(QStringLiteral("!"))})
                    .isEmpty());
    expectIdle(window);
    EXPECT_EQ(of("on_dock_layout").size(), layouts);
    // A new size does.
    ASSERT_TRUE(dock(2, line).isEmpty());
    ASSERT_TRUE(waitFor([&] { return of("on_dock_layout").size() > layouts; }));
    EXPECT_EQ(of("on_dock_layout").constLast()[QStringLiteral("rows")].toInt(), 2);
    expectIdle(window);

    // The pointer onto an item, along it, onto the next, and off the items.
    const qreal dpr = window->devicePixelRatio();
    auto dockOf = [&](const char* id) { return shownDock(window, id); };
    ASSERT_NE(dockOf("bar"), nullptr);
    ASSERT_EQ(dockOf("bar")->items().size(), 2u);
    const QRect dockRect = dockOf("bar")->rect();
    const QRect firstRect = dockOf("bar")->itemRect(0);
    const QRect secondRect = dockOf("bar")->itemRect(1);
    ASSERT_LT(firstRect.right(), secondRect.left());
    const qreal y = (dockRect.top() + 2) / dpr;
    auto move = [&](qreal x) { hoverAt(window, QPointF(x, y)); };
    move((firstRect.left() + 1) / dpr);
    move((firstRect.right() - 1) / dpr);
    ASSERT_EQ(of("on_dock_hover").size(), 1);
    EXPECT_EQ(of("on_dock_hover")[0][QStringLiteral("item")], QStringLiteral("first"));
    EXPECT_EQ(of("on_dock_hover")[0][QStringLiteral("dock")], QStringLiteral("bar"));
    move(QRectF(secondRect).center().x() / dpr);
    ASSERT_EQ(of("on_dock_hover").size(), 2);
    EXPECT_EQ(of("on_dock_hover")[1][QStringLiteral("item")], QStringLiteral("second"));
    move((firstRect.right() + secondRect.left()) / 2.0 / dpr); // the space between them
    ASSERT_EQ(of("on_dock_hover").size(), 3);
    EXPECT_FALSE(of("on_dock_hover")[2].contains(QStringLiteral("item")));
    // A chrome panel that opens under a pointer that does not move takes it
    // off the item, and gives it back when it closes.
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(waitFor([&] { return layer->rootItem() != nullptr; }));
    move(QRectF(firstRect).center().x() / dpr);
    ASSERT_EQ(of("on_dock_hover").size(), 4);
    chrome->setPaletteOpen(true);
    ASSERT_TRUE(waitFor([&] { return of("on_dock_hover").size() == 5; }));
    EXPECT_FALSE(of("on_dock_hover")[4].contains(QStringLiteral("item")));
    chrome->setPaletteOpen(false);
    ASSERT_TRUE(waitFor([&] { return of("on_dock_hover").size() == 6; }));
    EXPECT_EQ(of("on_dock_hover")[5][QStringLiteral("item")], QStringLiteral("first"));
    move((firstRect.right() + secondRect.left()) / 2.0 / dpr);
    ASSERT_EQ(of("on_dock_hover").size(), 7);
    // An item that comes under a pointer that stays where it is.
    ASSERT_TRUE(dock(2, {dockRun(QStringLiteral("one"), QStringLiteral("first")), dockRun(QStringLiteral("X"), QStringLiteral("middle")),
                         dockRun(QStringLiteral("two"), QStringLiteral("second"))})
                    .isEmpty());
    ASSERT_EQ(of("on_dock_hover").size(), 8);
    EXPECT_EQ(of("on_dock_hover")[7][QStringLiteral("item")], QStringLiteral("middle"));
    // Leaving the window leaves the item.
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(window, &leave);
    ASSERT_EQ(of("on_dock_hover").size(), 9);
    EXPECT_FALSE(of("on_dock_hover")[8].contains(QStringLiteral("item")));
    // A pointer that enters and stays still is on the item under it.
    const QPointF onFirst(QRectF(firstRect).center().x() / dpr, y);
    QEnterEvent enter(onFirst, onFirst, window->mapToGlobal(onFirst));
    QCoreApplication::sendEvent(window, &enter);
    ASSERT_EQ(of("on_dock_hover").size(), 10);
    EXPECT_EQ(of("on_dock_hover")[9][QStringLiteral("item")], QStringLiteral("first"));

    // Straight from an item of one area onto an item of another: the first
    // hears that the pointer left it, then the second hears of its item.
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("upper")},
                                                            {QStringLiteral("edge"), QStringLiteral("bottom")}, {QStringLiteral("size"), 1},
                                                            {QStringLiteral("lines"), QVariantList{QVariantList{dockRun(QStringLiteral("up"), QStringLiteral("above"))}}}})
                    .isEmpty());
    ASSERT_TRUE(waitFor([&] { return dockOf("upper") && !dockOf("upper")->rect().isEmpty(); }));
    expectIdle(window);
    ASSERT_NE(dockOf("bar"), nullptr);
    const QRect barNow = dockOf("bar")->itemRect(0);
    const QRect upperNow = dockOf("upper")->itemRect(0);
    ASSERT_FALSE(barNow.isEmpty());
    ASSERT_FALSE(upperNow.isEmpty());
    const auto hovers = of("on_dock_hover").size();
    hoverAt(window, QRectF(barNow).center() / dpr);
    hoverAt(window, QRectF(upperNow).center() / dpr);
    const auto all = of("on_dock_hover");
    ASSERT_GE(all.size(), hovers + 2);
    EXPECT_EQ(all[all.size() - 2][QStringLiteral("dock")], QStringLiteral("bar"));
    EXPECT_FALSE(all[all.size() - 2].contains(QStringLiteral("item")));
    EXPECT_EQ(all.last()[QStringLiteral("dock")], QStringLiteral("upper"));
    EXPECT_EQ(all.last()[QStringLiteral("item")], QStringLiteral("above"));
    // An undock under the pointer tells its script first.
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("undock")}, {QStringLiteral("id"), QStringLiteral("upper")}})
                    .isEmpty());
    EXPECT_EQ(of("on_dock_hover").last()[QStringLiteral("dock")], QStringLiteral("upper"));
    EXPECT_FALSE(of("on_dock_hover").last().contains(QStringLiteral("item")));
    expectIdle(window);
    QCoreApplication::sendEvent(window, &leave);

    // Whole notches go to the script, in steps; part of one waits.
    auto wheel = [&](qreal x, QPoint delta) {
        const QPointF at(x, y);
        QWheelEvent event(at, window->mapToGlobal(at), QPoint(), delta, Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window, &event);
    };
    const qreal onFirstX = QRectF(firstRect).center().x() / dpr;
    const qreal onSecondX = QRectF(secondRect).center().x() / dpr;
    const qreal offItemsX = (dockRect.right() - 2) / dpr;
    wheel(onFirstX, QPoint(0, 60));
    EXPECT_TRUE(of("on_dock_wheel").isEmpty());
    wheel(onFirstX, QPoint(0, 300));
    ASSERT_EQ(of("on_dock_wheel").size(), 1);
    EXPECT_EQ(of("on_dock_wheel")[0][QStringLiteral("direction")], QStringLiteral("up"));
    EXPECT_EQ(of("on_dock_wheel")[0][QStringLiteral("steps")].toInt(), 3);
    EXPECT_EQ(of("on_dock_wheel")[0][QStringLiteral("item")], QStringLiteral("first"));
    EXPECT_EQ(of("on_dock_wheel")[0][QStringLiteral("dock")], QStringLiteral("bar"));
    // Half a notch waits over "first". Over no item, it is gone: a whole
    // notch down is one step, not half of one.
    wheel(onFirstX, QPoint(0, 60));
    EXPECT_EQ(of("on_dock_wheel").size(), 1);
    wheel(offItemsX, QPoint(0, -120));
    ASSERT_EQ(of("on_dock_wheel").size(), 2);
    EXPECT_EQ(of("on_dock_wheel")[1][QStringLiteral("direction")], QStringLiteral("down"));
    EXPECT_EQ(of("on_dock_wheel")[1][QStringLiteral("steps")].toInt(), 1);
    EXPECT_FALSE(of("on_dock_wheel")[1].contains(QStringLiteral("item")));
    // A turn back over one item takes from what waits.
    wheel(onSecondX, QPoint(0, 60));
    wheel(onSecondX, QPoint(0, -120));
    EXPECT_EQ(of("on_dock_wheel").size(), 2);
    // A horizontal turn.
    wheel(onSecondX, QPoint(120, 0));
    ASSERT_EQ(of("on_dock_wheel").size(), 3);
    EXPECT_EQ(of("on_dock_wheel")[2][QStringLiteral("direction")], QStringLiteral("left"));
    EXPECT_EQ(of("on_dock_wheel")[2][QStringLiteral("item")], QStringLiteral("second"));

    auto last = [&](const char* id) {
        QVariantMap found;
        for (const QVariantMap& e : of("on_dock_layout")) if (e[QStringLiteral("dock")] == QLatin1String(id)) found = e;
        return found;
    };
    // An area that does not fit when it is first placed tells that too.
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("wide")},
                                                            {QStringLiteral("edge"), QStringLiteral("left")}, {QStringLiteral("size"), 4000}})
                    .isEmpty());
    ASSERT_TRUE(waitFor([&] { return !last("wide").isEmpty(); }));
    EXPECT_FALSE(last("wide")[QStringLiteral("shown")].toBool());
    EXPECT_EQ(last("wide")[QStringLiteral("cols")].toInt(), 0);
    EXPECT_EQ(last("wide")[QStringLiteral("rows")].toInt(), 0);
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("undock")}, {QStringLiteral("id"), QStringLiteral("wide")}})
                    .isEmpty());

    // A larger font tells the new width, and a dock that no longer fits
    // tells that it went. The side dock leaves the panes 10 columns: a few
    // pixels more in each of its cells take more than that.
    const int sideCols = windowCols - 10;
    ASSERT_GT(sideCols, 20);
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("side")},
                                                            {QStringLiteral("edge"), QStringLiteral("left")}, {QStringLiteral("size"), sideCols}})
                    .isEmpty());
    ASSERT_TRUE(waitFor([&] { return last("side")[QStringLiteral("shown")].toBool(); }));
    EXPECT_EQ(last("side")[QStringLiteral("cols")].toInt(), sideCols);
    for (int step = 0; step < 4; ++step) ASSERT_TRUE(window->runAction(pane, Action::FontBigger));
    ASSERT_TRUE(waitFor([&] { return last("bar")[QStringLiteral("cols")].toInt() < windowCols; }));
    EXPECT_EQ(last("bar")[QStringLiteral("cols")].toInt(), window->gridSize().width());
    ASSERT_TRUE(waitFor([&] { return !last("side")[QStringLiteral("shown")].toBool(); }));
    EXPECT_EQ(last("side")[QStringLiteral("cols")].toInt(), 0);
    EXPECT_EQ(last("side")[QStringLiteral("rows")].toInt(), 0);
    ASSERT_TRUE(window->runAction(pane, Action::FontReset));
    EXPECT_TRUE(waitFor([&] { return last("side")[QStringLiteral("shown")].toBool() && last("bar")[QStringLiteral("cols")].toInt() == windowCols; }));
    // Only the wheel counts toward the callback limit: a hover or a layout
    // that the script did not hear would leave it with an old state.
    for (const DockEvent& e : events.all) EXPECT_EQ(e.limited, e.handler == QLatin1String("on_dock_wheel")) << e.handler.toStdString();
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, ADragOnADockItemTellsItsScriptOfEachNewItemOrCell) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    // The pane under the area reports each pointer move (DECSET 1003), and
    // cat -v shows the reports: a move that reached it shows as text.
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty raw -echo; printf ready; exec cat -v")};
    auto* reporting = window->splitPane(first, SplitTree::Direction::Down, launch, nullptr);
    ASSERT_NE(reporting, nullptr);
    ASSERT_TRUE(waitFor([&] { return screenOf(reporting).contains(QLatin1String("ready")); }));
    reporting->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?1003h\x1b[?1006h"); });
    window->revealPane(reporting);
    DockEvents events(window);
    const auto drags = [&] { return events.of("on_dock_drag"); };
    // Only the moves count toward the callback limit: a dropped start, end
    // or cancel would leave the script in a drag that is over.
    const auto expectLimits = [&] {
        for (const DockEvent& e : drags())
            EXPECT_EQ(e.limited, e.event[QStringLiteral("phase")] == QLatin1String("move")) << e.event[QStringLiteral("phase")].toString().toStdString();
    };
    const QVariantList runs{dockRun(QStringLiteral("one"), QStringLiteral("first")), dockRun(QStringLiteral("    ")),
                            dockRun(QStringLiteral("two"), QStringLiteral("second"))};
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("bar"), QStringLiteral("bottom"), 1, runs)).isEmpty());
    const DockArea* bar = nullptr;
    ASSERT_TRUE(waitFor([&] {
        bar = shownDock(window, "bar");
        return bar && bar->items().size() == 2;
    }));
    expectIdle(window);
    const qreal dpr = window->devicePixelRatio();
    const QRect dockRect = bar->rect();
    const QRect firstRect = bar->itemRect(0);
    // "one" takes 3 cells, and the area 1 row.
    const int cell = firstRect.width() / 3;
    const auto at = [&](double col) { return QPointF((firstRect.left() + col * cell) / dpr, QRectF(dockRect).center().y() / dpr); };
    const QPointF onFirst = at(1.5);
    const QPointF onSecond = QRectF(bar->itemRect(1)).center() / dpr;
    const QPointF between = at(4.5); // the fifth cell, between the items
    const int distance = QGuiApplication::styleHints()->startDragDistance();

    // A click with no drag between press and release is only a click.
    pressLeft(window, onFirst);
    dragLeft(window, onFirst + QPointF(1, 0));
    releaseLeft(window, onFirst + QPointF(1, 0));
    EXPECT_TRUE(drags().isEmpty());
    EXPECT_EQ(events.of("on_dock_click").size(), 1);

    // The drag starts at the drag distance and not before, at the cell under
    // the pointer: from the middle of the first cell, still in "one".
    const QPointF from = at(0.5);
    pressLeft(window, from);
    dragLeft(window, from + QPointF(distance - 1, 0));
    EXPECT_TRUE(drags().isEmpty());
    dragLeft(window, from + QPointF(distance, 0));
    ASSERT_EQ(drags().size(), 1);
    const int startCol = int(std::floor((std::floor((from.x() + distance) * dpr) - firstRect.left()) / cell));
    ASSERT_LT(startCol, 3);
    const QVariantMap start = drags()[0].event;
    EXPECT_EQ(start[QStringLiteral("phase")], QStringLiteral("start"));
    EXPECT_EQ(start[QStringLiteral("item")], QStringLiteral("first"));
    EXPECT_EQ(start[QStringLiteral("over")], QStringLiteral("first"));
    EXPECT_EQ(start[QStringLiteral("dock")], QStringLiteral("bar"));
    EXPECT_EQ(start[QStringLiteral("scope")], QStringLiteral("window"));
    EXPECT_EQ(start[QStringLiteral("window")].toUInt(), window->id());
    EXPECT_EQ(start[QStringLiteral("col")].toInt(), startCol);
    EXPECT_EQ(start[QStringLiteral("row")].toInt(), 0);
    // Each new cell or item tells. Another pixel of the same cell does not.
    dragLeft(window, between);
    ASSERT_EQ(drags().size(), 2);
    EXPECT_EQ(drags()[1].event[QStringLiteral("phase")], QStringLiteral("move"));
    EXPECT_FALSE(drags()[1].event.contains(QStringLiteral("over")));
    EXPECT_EQ(drags()[1].event[QStringLiteral("col")].toInt(), 4);
    dragLeft(window, between + QPointF(cell / 4.0 / dpr, 0));
    EXPECT_EQ(drags().size(), 2);
    // The item under the pointer, which the hover tells during the drag too.
    dragLeft(window, onSecond);
    ASSERT_EQ(drags().size(), 3);
    EXPECT_EQ(drags()[2].event[QStringLiteral("over")], QStringLiteral("second"));
    EXPECT_EQ(drags()[2].event[QStringLiteral("item")], QStringLiteral("first"));
    EXPECT_EQ(events.last("on_dock_hover").event[QStringLiteral("item")], QStringLiteral("second"));
    // Above the area, over the pane, the drag goes on with rows above it,
    // over no item.
    const QPointF above(onSecond.x(), (dockRect.top() - 3 * dockRect.height()) / dpr);
    ASSERT_TRUE(window->accessiblePaneRect(reporting).contains(window->mapToGlobal(above).toPoint()));
    dragLeft(window, above);
    ASSERT_EQ(drags().size(), 4);
    EXPECT_LE(drags()[3].event[QStringLiteral("row")].toInt(), -3);
    EXPECT_FALSE(drags()[3].event.contains(QStringLiteral("over")));
    EXPECT_FALSE(events.last("on_dock_hover").event.contains(QStringLiteral("item")));
    // Outside the window too, and a release there ends it over no item.
    const QPointF outside(onSecond.x(), -20);
    dragLeft(window, outside);
    ASSERT_EQ(drags().size(), 5);
    const int outsideRow = drags()[4].event[QStringLiteral("row")].toInt();
    EXPECT_LT(outsideRow, drags()[3].event[QStringLiteral("row")].toInt());
    EXPECT_FALSE(drags()[4].event.contains(QStringLiteral("over")));
    releaseLeft(window, outside);
    ASSERT_EQ(drags().size(), 6);
    EXPECT_EQ(drags()[5].event[QStringLiteral("phase")], QStringLiteral("end"));
    EXPECT_FALSE(drags()[5].event.contains(QStringLiteral("over")));
    EXPECT_EQ(drags()[5].event[QStringLiteral("row")].toInt(), outsideRow);
    EXPECT_FALSE(first->hasSelection());
    EXPECT_FALSE(reporting->hasSelection());
    // A release over an item ends the drag there.
    pressLeft(window, onFirst);
    dragLeft(window, onSecond);
    releaseLeft(window, onSecond);
    ASSERT_EQ(drags().size(), 8);
    EXPECT_EQ(drags()[7].event[QStringLiteral("phase")], QStringLiteral("end"));
    EXPECT_EQ(drags()[7].event[QStringLiteral("over")], QStringLiteral("second"));
    expectLimits();

    // A release that went elsewhere cancels it, at the next pointer move.
    events.all.clear();
    pressLeft(window, onFirst);
    dragLeft(window, onSecond);
    ASSERT_EQ(drags().size(), 1);
    hoverAt(window, onSecond);
    ASSERT_EQ(drags().size(), 2);
    EXPECT_EQ(drags()[1].event[QStringLiteral("phase")], QStringLiteral("cancel"));
    EXPECT_EQ(drags()[1].event[QStringLiteral("over")], QStringLiteral("second"));
    // So does a new first press, whose release went elsewhere too.
    pressLeft(window, onFirst);
    dragLeft(window, onSecond);
    ASSERT_EQ(drags().size(), 3);
    pressLeft(window, onFirst);
    ASSERT_EQ(drags().size(), 4);
    EXPECT_EQ(drags()[3].event[QStringLiteral("phase")], QStringLiteral("cancel"));
    EXPECT_EQ(drags()[3].event[QStringLiteral("item")], QStringLiteral("first"));
    releaseLeft(window, onFirst);
    EXPECT_EQ(drags().size(), 4);
    expectLimits();

    // An area that stops showing before the drag starts has no cells to go
    // by: the drag tells the cell of the press.
    events.all.clear();
    pressLeft(window, onFirst);
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("bar"), QStringLiteral("left"), 4000, runs)).isEmpty());
    ASSERT_TRUE(waitFor([&] { return bar->rect().isEmpty(); }));
    dragLeft(window, onFirst + QPointF(distance, 0));
    releaseLeft(window, onFirst + QPointF(distance, 0));
    ASSERT_EQ(drags().size(), 2);
    for (const DockEvent& e : drags()) {
        EXPECT_EQ(e.event[QStringLiteral("col")].toInt(), 1);
        EXPECT_EQ(e.event[QStringLiteral("row")].toInt(), 0);
        EXPECT_FALSE(e.event.contains(QStringLiteral("over")));
    }
    EXPECT_EQ(drags()[0].event[QStringLiteral("phase")], QStringLiteral("start"));
    EXPECT_EQ(drags()[1].event[QStringLiteral("phase")], QStringLiteral("end"));
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("bar"), QStringLiteral("bottom"), 1, runs)).isEmpty());
    ASSERT_TRUE(waitFor([&] { return bar->rect() == dockRect; }));

    // An undock of the area cancels the drag before the area goes. The rest
    // of the gesture is still the area's: no pane selects.
    events.all.clear();
    pressLeft(window, onFirst);
    dragLeft(window, onSecond);
    ASSERT_EQ(drags().size(), 1);
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), undockEffect(QStringLiteral("bar"))).isEmpty());
    ASSERT_EQ(drags().size(), 2);
    EXPECT_EQ(drags()[1].event[QStringLiteral("phase")], QStringLiteral("cancel"));
    EXPECT_EQ(drags()[1].event[QStringLiteral("item")], QStringLiteral("first"));
    dragLeft(window, onFirst);
    releaseLeft(window, onFirst);
    EXPECT_EQ(drags().size(), 2);
    EXPECT_FALSE(first->hasSelection());
    EXPECT_FALSE(reporting->hasSelection());
    expectLimits();

    // The pane reports a move that is its own, once: none of the moves above
    // reached it.
    // A marker goes through the same input after anything the moves sent,
    // so once it shows, every report they made shows too.
    ASSERT_TRUE(reporting->sendInput("@marker@"));
    ASSERT_TRUE(waitFor([&] { return screenOf(reporting).contains(QLatin1String("@marker@")); }));
    EXPECT_EQ(screenOf(reporting).count(QStringLiteral("^[[<")), 0) << screenOf(reporting).toStdString();
    const QPointF onPane = window->mapFromGlobal(QPointF(window->accessiblePaneRect(reporting).center()));
    hoverAt(window, onPane);
    // A move with no button held: SGR button code 35.
    ASSERT_TRUE(waitFor([&] { return screenOf(reporting).contains(QLatin1String("^[[<35;")); }));
    // A second marker: a later copy of the report would show before it.
    ASSERT_TRUE(reporting->sendInput("@marker2@"));
    ASSERT_TRUE(waitFor([&] { return screenOf(reporting).contains(QLatin1String("@marker2@")); }));
    EXPECT_EQ(screenOf(reporting).count(QStringLiteral("^[[<")), 1) << screenOf(reporting).toStdString();
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, MoveTabMovesATabWithoutShowingItAndRefusesAPlaceThatIsNoTab) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    auto* second = window->focusedPane();
    ASSERT_TRUE(window->runAction(second, Action::NewTab));
    auto* third = window->focusedPane();
    ASSERT_EQ(window->tabIndexOf(first), 0);
    ASSERT_EQ(window->tabIndexOf(third), 2);
    // Titles that tell the tabs apart, also in the chrome.
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    const std::pair<Pane*, QString> names[] = {{first, QStringLiteral("one")}, {second, QStringLiteral("two")}, {third, QStringLiteral("three")}};
    for (const auto& [pane, name] : names) {
        ASSERT_TRUE(window->runAction(pane, Action::RenameTab));
        ASSERT_TRUE(chrome->titlePromptOpen());
        chrome->answerTitle(name);
    }
    const auto titles = [&] {
        QStringList found;
        for (const auto& tab : window->tabs()) found << tab.title;
        return found;
    };
    const auto chromeShows = [&](const QStringList& order) {
        const QStringList shown = chrome->tabTitles();
        if (shown.size() != order.size()) return false;
        for (int i = 0; i < order.size(); ++i)
            if (!shown[i].contains(order[i])) return false;
        return true;
    };
    const QStringList start{QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")};
    const QStringList moved{QStringLiteral("two"), QStringLiteral("three"), QStringLiteral("one")};
    ASSERT_EQ(titles(), start);
    int moves = 0;
    QObject listener;
    QObject::connect(chrome, &ChromeController::tabsMoved, &listener, [&] { ++moves; });

    // The first tab goes last. The current tab stays on screen, at its new
    // place, and the tab bar follows.
    EXPECT_TRUE(window->moveTabOf(first, 2).isEmpty());
    EXPECT_EQ(titles(), moved);
    EXPECT_EQ(window->focusedPane(), third);
    EXPECT_EQ(window->tabIndexOf(third), 1);
    EXPECT_EQ(moves, 1);
    EXPECT_TRUE(waitFor([&] { return chromeShows(moved) && chrome->currentTab() == 1; })) << chrome->tabTitles().join(QLatin1Char('|')).toStdString();
    // The tab's own place succeeds and moves nothing.
    EXPECT_TRUE(window->moveTabOf(first, 2).isEmpty());
    EXPECT_EQ(titles(), moved);
    EXPECT_EQ(moves, 1);
    // A place that is no tab fails and moves nothing.
    EXPECT_FALSE(window->moveTabOf(first, 3).isEmpty());
    EXPECT_FALSE(window->moveTabOf(first, -1).isEmpty());
    EXPECT_EQ(titles(), moved);
    EXPECT_EQ(moves, 1);

    // Through the script callbacks, with the code of a refusal. Control
    // finds no pane for an id that is not one.
    SettingsStore settings(directory.filePath(QStringLiteral("move.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    const auto callbacks = scriptCallbacks(control);
    auto apply = [&](quint32 pane, int index) {
        return callbacks.applyOwned(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("move_tab")}, {QStringLiteral("pane"), pane},
                                                           {QStringLiteral("index"), double(index)}}, {});
    };
    EXPECT_TRUE(apply(first->id(), 0).error.isEmpty());
    EXPECT_EQ(titles(), start);
    const auto refused = apply(first->id(), 9);
    EXPECT_FALSE(refused.error.isEmpty());
    EXPECT_EQ(refused.code, QStringLiteral("operation_failed"));
    EXPECT_EQ(control.moveTab(999999, 0), QStringLiteral("no pane 999999"));
    EXPECT_EQ(titles(), start);
}

TEST_F(NativeWindow, MoveTabTellsOfTheMovedPanesAndKeepsTheOverviewOnItsCard) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* beside = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(beside, nullptr);
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    auto* second = window->focusedPane();
    ASSERT_TRUE(window->runAction(second, Action::NewTab));
    auto* third = window->focusedPane();
    SettingsStore settings(directory.filePath(QStringLiteral("moved.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    QList<quint32> moved, announced;
    QObject listener;
    QObject::connect(window, &TerminalWindow::paneMoved, &listener, [&](Pane* pane) { moved << pane->id(); });
    QObject::connect(&control, &Control::paneMoved, &listener, [&](quint32 pane) { announced << pane; });

    // Scripts and control clients hear of each pane of the moved tab, and
    // of the tabs that it passed, which are at another index now.
    ASSERT_TRUE(window->moveTabOf(beside, 2).isEmpty());
    QList<quint32> expected{first->id(), beside->id(), second->id(), third->id()};
    std::sort(expected.begin(), expected.end());
    std::sort(moved.begin(), moved.end());
    std::sort(announced.begin(), announced.end());
    EXPECT_EQ(moved, expected);
    EXPECT_EQ(announced, expected);
    // A move to its own place moves no pane.
    moved.clear();
    announced.clear();
    ASSERT_TRUE(window->moveTabOf(beside, 2).isEmpty());
    EXPECT_TRUE(moved.isEmpty());
    EXPECT_TRUE(announced.isEmpty());
    // One place back, past the third tab alone: the second keeps its index.
    ASSERT_TRUE(window->moveTabOf(beside, 1).isEmpty());
    expected = {first->id(), beside->id(), third->id()};
    std::sort(expected.begin(), expected.end());
    std::sort(moved.begin(), moved.end());
    std::sort(announced.begin(), announced.end());
    EXPECT_EQ(moved, expected);
    EXPECT_EQ(announced, expected);
    ASSERT_TRUE(window->moveTabOf(beside, 2).isEmpty());

    // With the overview open, the card in front stays in front: the deck
    // counts its position in the new order at once, and does not slide.
    ASSERT_TRUE(window->runAction(third, Action::PaneOverview));
    ASSERT_TRUE(window->overviewOpen());
    ASSERT_EQ(window->overviewHighlight(), third);
    const auto cardOf = [&](Pane* pane) {
        return int(window->overviewState().value(QStringLiteral("cards")).toList().indexOf(QVariant(pane->id())));
    };
    // second, third, then first and beside.
    ASSERT_EQ(cardOf(third), 1);
    EXPECT_NEAR(window->overviewPosition().first, 1.0, 1e-3);
    ASSERT_TRUE(window->moveTabOf(second, 2).isEmpty());
    ASSERT_EQ(cardOf(third), 0);
    EXPECT_NEAR(window->overviewPosition().first, 0.0, 1e-3);
    EXPECT_NEAR(window->overviewPosition().second, 0.0, 1e-3);
    // The frames that follow keep it there.
    ASSERT_TRUE(nextFrame(window));
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(window->overviewHighlight(), third);
    EXPECT_NEAR(window->overviewPosition().first, 0.0, 1e-3);
    EXPECT_NEAR(window->overviewPosition().second, 0.0, 1e-3);
    QVariantMap state;
    ASSERT_TRUE(window->scriptOverview(QStringLiteral("script@1"), third, QStringLiteral("close"), state).isEmpty());
    EXPECT_TRUE(waitFor([&] { return !window->overviewOpen(); }));
}

TEST_F(NativeWindow, PaneDocksTakeCellsFromTheirPaneAndTellTheirScriptOfThem) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    expectIdle(window);
    ASSERT_EQ(first->rows(), second->rows());
    const int rows = first->rows(), cols = second->cols();
    DockEvents events(window);
    auto dock = [&](Pane* pane, const QString& id, const QString& edge, int size) {
        return window->setScriptDock(QStringLiteral("a@1"), dockEffect(id, edge, size, {dockRun(QStringLiteral("title"), QStringLiteral("t"))}, QStringLiteral("pane")), pane);
    };

    // A top dock takes a row from its pane only, at the pane's top left
    // corner, and as wide as the pane.
    ASSERT_TRUE(dock(first, QStringLiteral("title"), QStringLiteral("top"), 1).isEmpty());
    ASSERT_TRUE(waitFor([&] { return first->rows() == rows - 1; }));
    EXPECT_EQ(second->rows(), rows);
    const DockArea* title = paneDock(first, "title");
    ASSERT_NE(title, nullptr);
    ASSERT_FALSE(title->rect().isEmpty());
    ASSERT_EQ(title->items().size(), 1u);
    // "title" takes 5 cells.
    const QSize cell(title->itemRect(0).width() / 5, title->itemRect(0).height());
    EXPECT_EQ(title->rect().topLeft(), QPoint(0, 0));
    EXPECT_EQ(title->rect().height(), cell.height());
    const QRect grid = window->accessiblePaneRect(first);
    const QRect dockGlobal = window->accessibleDockRect(title);
    EXPECT_LE(dockGlobal.bottom(), grid.top());
    EXPECT_LE(dockGlobal.left(), grid.left());
    EXPECT_GE(dockGlobal.right(), grid.right());
    EXPECT_LT(dockGlobal.right(), window->accessiblePaneRect(second).left());
    EXPECT_TRUE(window->accessibleDocks().contains(title));
    // Its script hears of the cells of the pane's width, not the window's.
    ASSERT_TRUE(waitFor([&] { return !events.last("on_dock_layout", "title").handler.isEmpty(); }));
    const DockEvent layout = events.last("on_dock_layout", "title");
    EXPECT_EQ(layout.pane, first->id());
    EXPECT_FALSE(layout.limited);
    EXPECT_EQ(layout.event[QStringLiteral("scope")], QStringLiteral("pane"));
    EXPECT_EQ(layout.event[QStringLiteral("window")].toUInt(), window->id());
    EXPECT_TRUE(layout.event[QStringLiteral("shown")].toBool());
    EXPECT_EQ(layout.event[QStringLiteral("cols")].toInt(), title->rect().width() / cell.width());
    EXPECT_LT(layout.event[QStringLiteral("cols")].toInt(), window->gridSize().width());
    EXPECT_EQ(layout.event[QStringLiteral("rows")].toInt(), 1);

    // A left and a right dock take their size in columns from their pane,
    // the left one at its left edge and the right one at its right.
    ASSERT_TRUE(dock(second, QStringLiteral("side"), QStringLiteral("left"), 4).isEmpty());
    ASSERT_TRUE(waitFor([&] { return second->cols() == cols - 4; })) << second->cols();
    ASSERT_TRUE(dock(second, QStringLiteral("edge"), QStringLiteral("right"), 3).isEmpty());
    ASSERT_TRUE(waitFor([&] { return second->cols() == cols - 7; })) << second->cols();
    EXPECT_EQ(second->rows(), rows);
    const DockArea* side = paneDock(second, "side");
    const DockArea* edge = paneDock(second, "edge");
    ASSERT_NE(side, nullptr);
    ASSERT_NE(edge, nullptr);
    const QRect secondGrid = window->accessiblePaneRect(second);
    EXPECT_LE(window->accessibleDockRect(side).right(), secondGrid.left());
    EXPECT_GE(window->accessibleDockRect(edge).left(), secondGrid.right());
    for (const char* id : {"side", "edge"}) {
        const DockEvent placed = events.last("on_dock_layout", id);
        EXPECT_EQ(placed.pane, second->id()) << id;
        EXPECT_TRUE(placed.event[QStringLiteral("shown")].toBool()) << id;
        EXPECT_EQ(placed.event[QStringLiteral("rows")].toInt(), paneDock(second, id)->rect().height() / cell.height()) << id;
        EXPECT_GE(placed.event[QStringLiteral("rows")].toInt(), rows) << id;
    }
    EXPECT_EQ(events.last("on_dock_layout", "side").event[QStringLiteral("cols")].toInt(), 4);
    EXPECT_EQ(events.last("on_dock_layout", "edge").event[QStringLiteral("cols")].toInt(), 3);

    // A dock at a new edge gives back what it took, and takes from there.
    ASSERT_TRUE(dock(second, QStringLiteral("side"), QStringLiteral("top"), 1).isEmpty());
    ASSERT_TRUE(waitFor([&] { return second->cols() == cols - 3 && second->rows() == rows - 1; }))
        << second->cols() << "x" << second->rows();
    const DockEvent moved = events.last("on_dock_layout", "side");
    EXPECT_EQ(moved.event[QStringLiteral("rows")].toInt(), 1);
    EXPECT_EQ(moved.event[QStringLiteral("cols")].toInt(), side->rect().width() / cell.width());
    EXPECT_GT(moved.event[QStringLiteral("cols")].toInt(), 3);

    // A new window width gives the pane, and its dock, new cells.
    const int titleCols = layout.event[QStringLiteral("cols")].toInt();
    window->resize(window->width() + 200, window->height());
    ASSERT_TRUE(waitFor([&] { return events.last("on_dock_layout", "title").event[QStringLiteral("cols")].toInt() > titleCols; }));
    EXPECT_EQ(events.last("on_dock_layout", "title").event[QStringLiteral("cols")].toInt(), title->rect().width() / cell.width());
    EXPECT_EQ(first->rows(), rows - 1);

    // An undock gives the row back, and the other pane keeps its docks.
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), undockEffect(QStringLiteral("title"), QStringLiteral("pane")), first).isEmpty());
    ASSERT_TRUE(waitFor([&] { return first->rows() == rows; }));
    EXPECT_EQ(paneDock(first, "title"), nullptr);
    EXPECT_NE(paneDock(second, "side"), nullptr);
    expectIdle(window);
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, PaneDockClicksReachTheirScriptWithTheirPaneAndNotThePane) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    window->revealPane(first);
    DockEvents events(window);
    const qreal dpr = window->devicePixelRatio();
    const int rows = first->rows();
    auto dock = [&](Pane* pane, const QString& edge, int size, const QString& text) {
        return window->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("title"), edge, size, {dockRun(text, QStringLiteral("t"))}, QStringLiteral("pane")), pane);
    };
    ASSERT_TRUE(dock(first, QStringLiteral("top"), 1, QStringLiteral("title")).isEmpty());
    const DockArea* title = nullptr;
    ASSERT_TRUE(waitFor([&] {
        title = shownDock(window, "title", first->id());
        return title && !title->items().empty();
    }));
    expectIdle(window);

    // A click on its item goes to its script, with its pane. Not to the
    // pane: the focus stays with the other one, and nothing selects.
    window->revealPane(second);
    const QPointF onItem = QRectF(title->itemRect(0)).center() / dpr;
    clickLeft(window, onItem);
    ASSERT_EQ(events.of("on_dock_click").size(), 1);
    const DockEvent click = events.of("on_dock_click")[0];
    EXPECT_EQ(click.pane, first->id());
    EXPECT_EQ(click.event[QStringLiteral("item")], QStringLiteral("t"));
    EXPECT_EQ(click.event[QStringLiteral("dock")], QStringLiteral("title"));
    EXPECT_EQ(click.event[QStringLiteral("scope")], QStringLiteral("pane"));
    EXPECT_FALSE(click.limited);
    EXPECT_EQ(window->focusedPane(), second);
    EXPECT_FALSE(first->hasSelection());

    // The same id at another pane is another area, with clicks of its own.
    ASSERT_TRUE(dock(second, QStringLiteral("left"), 4, QStringLiteral("abcd")).isEmpty());
    const DockArea* side = nullptr;
    ASSERT_TRUE(waitFor([&] {
        side = shownDock(window, "title", second->id());
        return side && !side->items().empty();
    }));
    EXPECT_EQ(second->rows(), rows);
    EXPECT_EQ(first->rows(), rows - 1);
    expectIdle(window);
    clickLeft(window, QRectF(side->itemRect(0)).center() / dpr);
    ASSERT_EQ(events.of("on_dock_click").size(), 2);
    EXPECT_EQ(events.of("on_dock_click")[1].pane, second->id());

    // At the pane's edge, the divider's grab comes first: a press there
    // drags the divider, and its item hears no click.
    const int firstCols = first->cols();
    const QPointF atDivider((side->rect().left() + 1) / dpr, QRectF(side->itemRect(0)).center().y() / dpr);
    pressLeft(window, atDivider);
    dragLeft(window, atDivider + QPointF(40, 0));
    ASSERT_TRUE(waitFor([&] { return first->cols() > firstCols; })) << first->cols();
    releaseLeft(window, atDivider + QPointF(40, 0));
    EXPECT_EQ(events.of("on_dock_click").size(), 2);
    EXPECT_TRUE(events.of("on_dock_drag").isEmpty());
    expectIdle(window);
    // Past the grab, the item takes the click.
    clickLeft(window, QRectF(side->itemRect(0)).center() / dpr);
    ASSERT_EQ(events.of("on_dock_click").size(), 3);
    EXPECT_EQ(events.of("on_dock_click")[2].pane, second->id());

    // The overview draws cards: a press where the dock was is the
    // overview's, which closes on the card in front.
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    EXPECT_FALSE(window->accessibleDocks().contains(title));
    clickLeft(window, QRectF(title->itemRect(0)).center() / dpr);
    EXPECT_EQ(events.of("on_dock_click").size(), 3);
    ASSERT_TRUE(waitFor([&] { return !window->overviewOpen(); }));
    EXPECT_TRUE(window->accessibleDocks().contains(title));

    // An undock gives the row back.
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), undockEffect(QStringLiteral("title"), QStringLiteral("pane")), first).isEmpty());
    ASSERT_TRUE(waitFor([&] { return first->rows() == rows; }));
    EXPECT_NE(paneDock(second, "title"), nullptr);
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, PaneDocksInABackgroundTabLayOutAtOnce) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    auto* second = window->focusedPane();
    ASSERT_NE(window->tabIndexOf(second), window->tabIndexOf(first));
    expectIdle(window);
    DockEvents events(window);
    auto dock = [&](const QString& edge, int size) {
        return window->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("status"), edge, size, {dockRun(QStringLiteral("status"))}, QStringLiteral("pane")), first);
    };
    // In the last layout of its tab, which has the tab bar of two tabs as
    // the tab that shows does: its script and its shell hear of the new
    // size before the tab shows.
    const int rows = second->rows();
    ASSERT_TRUE(dock(QStringLiteral("bottom"), 2).isEmpty());
    ASSERT_EQ(events.of("on_dock_layout").size(), 1);
    const DockEvent layout = events.of("on_dock_layout")[0];
    EXPECT_EQ(layout.pane, first->id());
    EXPECT_TRUE(layout.event[QStringLiteral("shown")].toBool());
    EXPECT_EQ(layout.event[QStringLiteral("rows")].toInt(), 2);
    EXPECT_EQ(first->rows(), rows - 2);
    // An undock there gives the rows back at once too.
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), undockEffect(QStringLiteral("status"), QStringLiteral("pane")), first).isEmpty());
    EXPECT_EQ(first->rows(), rows);
    // The tab shows: the pane keeps the rows of the other tab's pane, less
    // its dock.
    ASSERT_TRUE(dock(QStringLiteral("bottom"), 2).isEmpty());
    EXPECT_EQ(first->rows(), rows - 2);
    window->revealPane(first);
    // After the switch, in a frame of the tab that shows.
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(first->rows(), rows - 2);
    EXPECT_EQ(second->rows(), rows);
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, PaneDocksCloseWithTheirPaneAndGoWithItToAnotherWindow) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    expectIdle(window);
    const int rows = first->rows();
    DockEvents events(window);
    const qreal dpr = window->devicePixelRatio();
    auto dock = [&](TerminalWindow* in, Pane* pane) {
        return in->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("title"), QStringLiteral("top"), 1, {dockRun(QStringLiteral("title"), QStringLiteral("t"))}, QStringLiteral("pane")), pane);
    };
    ASSERT_TRUE(dock(window, first).isEmpty());
    ASSERT_TRUE(dock(window, second).isEmpty());
    const DockArea* closing = nullptr;
    ASSERT_TRUE(waitFor([&] {
        closing = shownDock(window, "title", second->id());
        return closing && !closing->items().empty() && shownDock(window, "title", first->id());
    }));
    expectIdle(window);
    // The pointer over the item of the pane that closes.
    hoverAt(window, QRectF(closing->itemRect(0)).center() / dpr);
    ASSERT_EQ(events.last("on_dock_hover").pane, second->id());
    ASSERT_EQ(events.last("on_dock_hover").event[QStringLiteral("item")], QStringLiteral("t"));

    // A pane that closes takes its docks: its script hears that the pointer
    // left the item, and nothing points at them after.
    const quint32 secondId = second->id();
    QPointer<Pane> gone(second);
    window->requestClosePane(second);
    EXPECT_EQ(events.last("on_dock_hover").pane, secondId);
    EXPECT_FALSE(events.last("on_dock_hover").event.contains(QStringLiteral("item")));
    ASSERT_TRUE(waitDeleted(gone));
    EXPECT_EQ(shownDock(window, "title", secondId), nullptr);
    EXPECT_NE(shownDock(window, "title", first->id()), nullptr);
    EXPECT_EQ(first->docks().size(), 1u);
    expectIdle(window);
    // The script's end takes the rest.
    window->clearScriptOwner(QStringLiteral("a@1"));
    EXPECT_TRUE(first->docks().empty());
    EXPECT_TRUE(window->accessibleDocks().isEmpty());
    ASSERT_TRUE(waitFor([&] { return first->rows() == rows; }));

    // A pane's docks go with it to another window, and take their cells
    // from it there.
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    auto* third = window->focusedPane();
    expectIdle(window);
    const int alone = third->rows();
    ASSERT_TRUE(dock(window, third).isEmpty());
    ASSERT_TRUE(waitFor([&] { return third->rows() == alone - 1; }));
    ASSERT_TRUE(window->runAction(third, Action::DetachTab));
    TerminalWindow* detached = nullptr;
    ASSERT_TRUE(waitFor([&] {
        detached = qobject_cast<TerminalWindow*>(third->parent());
        return detached && detached != window;
    }));
    ASSERT_EQ(third->docks().size(), 1u);
    const DockArea* moved = third->docks().front().get();
    ASSERT_TRUE(waitFor([&] { return detached->isExposed() && detached->accessibleDocks().contains(moved); }));
    EXPECT_FALSE(window->accessibleDocks().contains(moved));
    const QRect grid = detached->accessiblePaneRect(third);
    const QRect dockGlobal = detached->accessibleDockRect(moved);
    EXPECT_LE(dockGlobal.bottom(), grid.top());
    EXPECT_LE(dockGlobal.left(), grid.left());
    EXPECT_GE(dockGlobal.right(), grid.right());
    // Its script hears of its cells from there.
    events.watch(detached);
    events.all.clear();
    // A split there narrows the pane, and its top dock with it.
    const int cell = moved->itemRect(0).width() / 5;
    const int before = moved->rect().width() / cell;
    ASSERT_NE(detached->splitPane(third, SplitTree::Direction::Right, launch, nullptr), nullptr);
    ASSERT_TRUE(waitFor([&] {
        const int cols = events.last("on_dock_layout").event[QStringLiteral("cols")].toInt();
        return cols > 0 && cols < before;
    }));
    const DockEvent there = events.last("on_dock_layout");
    EXPECT_EQ(there.pane, third->id());
    EXPECT_EQ(there.event[QStringLiteral("window")].toUInt(), detached->id());
    EXPECT_EQ(there.event[QStringLiteral("cols")].toInt(), moved->rect().width() / cell);
    detached->clearScriptOwner(QStringLiteral("a@1"));
    EXPECT_TRUE(third->docks().empty());
}

TEST_F(NativeWindow, PaneDocksHaveLimitsAndHideWhenTheyDoNotFitOrTheirPaneIsHidden) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    expectIdle(window);
    const int rows = first->rows(), cols = second->cols();
    DockEvents events(window);
    auto dock = [&](const QString& owner, Pane* pane, const QString& id, const QString& edge, int size) {
        return window->setScriptDock(owner, dockEffect(id, edge, size, {dockRun(QStringLiteral("x"))}, QStringLiteral("pane")), pane);
    };

    // A dock that leaves its pane too little does not show, and tells so.
    ASSERT_TRUE(dock(QStringLiteral("a@1"), second, QStringLiteral("wide"), QStringLiteral("left"), 4000).isEmpty());
    ASSERT_TRUE(waitFor([&] { return !events.last("on_dock_layout", "wide").handler.isEmpty(); }));
    const DockEvent wide = events.last("on_dock_layout", "wide");
    EXPECT_EQ(wide.pane, second->id());
    EXPECT_FALSE(wide.event[QStringLiteral("shown")].toBool());
    EXPECT_EQ(wide.event[QStringLiteral("cols")].toInt(), 0);
    EXPECT_EQ(wide.event[QStringLiteral("rows")].toInt(), 0);
    expectIdle(window);
    EXPECT_EQ(second->cols(), cols);
    ASSERT_NE(paneDock(second, "wide"), nullptr);
    EXPECT_TRUE(paneDock(second, "wide")->rect().isEmpty());
    EXPECT_EQ(shownDock(window, "wide", second->id()), nullptr);

    // A zoom hides the other pane and its docks: their script hears that
    // they do not show, and the pane's shell keeps its size. They show
    // again when the zoom ends.
    ASSERT_TRUE(dock(QStringLiteral("a@1"), first, QStringLiteral("title"), QStringLiteral("top"), 1).isEmpty());
    ASSERT_TRUE(waitFor([&] { return events.last("on_dock_layout", "title").event[QStringLiteral("shown")].toBool(); }));
    ASSERT_TRUE(waitFor([&] { return first->rows() == rows - 1; }));
    ASSERT_TRUE(window->runAction(second, Action::TogglePaneZoom));
    ASSERT_TRUE(waitFor([&] { return !events.last("on_dock_layout", "title").event[QStringLiteral("shown")].toBool(); }));
    EXPECT_EQ(events.last("on_dock_layout", "title").pane, first->id());
    EXPECT_EQ(events.last("on_dock_layout", "title").event[QStringLiteral("cols")].toInt(), 0);
    EXPECT_EQ(first->rows(), rows - 1);
    EXPECT_EQ(shownDock(window, "title", first->id()), nullptr);
    ASSERT_TRUE(window->runAction(second, Action::TogglePaneZoom));
    ASSERT_TRUE(waitFor([&] { return events.last("on_dock_layout", "title").event[QStringLiteral("shown")].toBool(); }));
    EXPECT_EQ(events.last("on_dock_layout", "title").event[QStringLiteral("rows")].toInt(), 1);
    EXPECT_NE(shownDock(window, "title", first->id()), nullptr);
    EXPECT_EQ(first->rows(), rows - 1);

    // A script has at most 4 docks at a pane, and a pane at most 8 of all
    // its scripts together.
    for (const char* id : {"b", "c", "d"}) ASSERT_TRUE(dock(QStringLiteral("a@1"), first, QString::fromLatin1(id), QStringLiteral("right"), 1).isEmpty());
    EXPECT_EQ(dock(QStringLiteral("a@1"), first, QStringLiteral("e"), QStringLiteral("right"), 1),
              QStringLiteral("too many docks: a script has at most 4 in a pane, and a pane at most 8"));
    EXPECT_TRUE(dock(QStringLiteral("a@1"), first, QStringLiteral("title"), QStringLiteral("bottom"), 1).isEmpty());
    EXPECT_TRUE(dock(QStringLiteral("a@1"), second, QStringLiteral("d"), QStringLiteral("right"), 1).isEmpty());
    for (const char* id : {"a", "b", "c", "d"}) ASSERT_TRUE(dock(QStringLiteral("b@1"), first, QString::fromLatin1(id), QStringLiteral("right"), 1).isEmpty());
    EXPECT_FALSE(dock(QStringLiteral("c@1"), first, QStringLiteral("a"), QStringLiteral("right"), 1).isEmpty());
    window->clearScriptOwner(QStringLiteral("b@1"));
    EXPECT_TRUE(dock(QStringLiteral("c@1"), first, QStringLiteral("a"), QStringLiteral("right"), 1).isEmpty());
    window->clearScriptOwner(QStringLiteral("c@1"));
    window->clearScriptOwner(QStringLiteral("a@1"));
    EXPECT_TRUE(first->docks().empty());
    EXPECT_TRUE(second->docks().empty());
}

TEST_F(NativeWindow, ControlDocksAtAPaneAndFindsNoPaneOnceItClosed) {
    SettingsStore settings(directory.filePath(QStringLiteral("panedock.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    const auto callbacks = scriptCallbacks(control);
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    quint32 created = 0;
    ASSERT_TRUE(control.split(first->id(), QStringLiteral("right"), {QStringLiteral("/bin/cat")}, {}, created).isEmpty());
    Pane* second = nullptr;
    for (auto* pane : window->panes())
        if (pane->id() == created) second = pane;
    ASSERT_NE(second, nullptr);
    expectIdle(window);
    const int rows = second->rows(), firstRows = first->rows();
    const QVariantMap title = dockEffect(QStringLiteral("title"), QStringLiteral("top"), 1, {dockRun(QStringLiteral("title"), QStringLiteral("t"))}, QStringLiteral("pane"));
    const QVariantMap untitle = undockEffect(QStringLiteral("title"), QStringLiteral("pane"));
    // As ScriptControl applies a script's effect.
    auto apply = [&](QVariantMap effect, quint32 pane) {
        effect.insert(QStringLiteral("pane"), pane);
        return callbacks.applyOwned(QStringLiteral("a@1"), effect, {});
    };

    // The area is at the pane named, in its window, and at no other pane.
    const auto docked = apply(title, created);
    ASSERT_TRUE(docked.error.isEmpty()) << docked.error.toStdString();
    ASSERT_NE(paneDock(second, "title"), nullptr);
    EXPECT_TRUE(waitFor([&] { return second->rows() == rows - 1; }));
    EXPECT_EQ(first->rows(), firstRows);
    EXPECT_TRUE(first->docks().empty());
    // A pane in another window has its area there.
    WindowOptions options;
    options.command = {QStringLiteral("/bin/cat")};
    auto* elsewhere = application->newWindow(options);
    ASSERT_NE(elsewhere, nullptr);
    control.attachWindow(elsewhere);
    auto* resident = elsewhere->focusedPane();
    ASSERT_NE(resident, nullptr);
    ASSERT_TRUE(apply(title, resident->id()).error.isEmpty());
    EXPECT_NE(paneDock(resident, "title"), nullptr);
    // An undock acts at its pane alone.
    ASSERT_TRUE(apply(untitle, resident->id()).error.isEmpty());
    EXPECT_EQ(paneDock(resident, "title"), nullptr);
    EXPECT_NE(paneDock(second, "title"), nullptr);

    // A pane that closes takes its area. A pane area keeps no record to
    // undock through, so its pane is not found.
    QPointer<Pane> gone(second);
    ASSERT_TRUE(second->sendInput("\x04")); // its cat ends, and the pane with it
    ASSERT_TRUE(waitDeleted(gone));
    const auto undocked = apply(untitle, created);
    EXPECT_FALSE(undocked.error.isEmpty());
    EXPECT_EQ(undocked.code, QStringLiteral("no_pane"));
    const auto redocked = apply(title, created);
    EXPECT_FALSE(redocked.error.isEmpty());
    EXPECT_EQ(redocked.code, QStringLiteral("no_pane"));
    EXPECT_TRUE(first->docks().empty());
    control.clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, AccessiblePaneDocksAreKeptApartByPaneAndGoWithIt) {
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    auto dock = [&](Pane* pane, const QString& text) {
        return window->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("title"), QStringLiteral("top"), 1, {dockRun(text, QStringLiteral("t"))}, QStringLiteral("pane")), pane);
    };
    auto areaOf = [](TerminalWindow* in, const char* text) -> QAccessibleInterface* {
        QAccessibleInterface* root = in->accessibleRoot();
        for (int i = 0; root && i < root->childCount(); ++i)
            if (auto* c = root->child(i); c && c->object() == nullptr && c->text(QAccessible::Name).contains(QLatin1String(text))) return c;
        return nullptr;
    };
    // The same id at two panes is two areas.
    ASSERT_TRUE(dock(first, QStringLiteral("FirstTitle")).isEmpty());
    ASSERT_TRUE(dock(second, QStringLiteral("SecondTitle")).isEmpty());
    ASSERT_TRUE(waitFor([&] { return areaOf(window, "FirstTitle") && areaOf(window, "SecondTitle"); }));
    EXPECT_NE(areaOf(window, "FirstTitle"), areaOf(window, "SecondTitle"));
    const QAccessible::Id secondArea = QAccessible::uniqueId(areaOf(window, "SecondTitle"));

    // A pane that closes takes its area out of the tree.
    QPointer<Pane> gone(second);
    window->requestClosePane(second);
    ASSERT_TRUE(waitDeleted(gone));
    EXPECT_EQ(QAccessible::accessibleInterface(secondArea), nullptr);
    EXPECT_TRUE(waitFor([&] { return areaOf(window, "SecondTitle") == nullptr; }));
    EXPECT_NE(areaOf(window, "FirstTitle"), nullptr);

    // A pane that goes to another window takes its area there.
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    auto* third = window->focusedPane();
    ASSERT_TRUE(dock(third, QStringLiteral("ThirdTitle")).isEmpty());
    ASSERT_TRUE(waitFor([&] { return areaOf(window, "ThirdTitle") != nullptr; }));
    const QAccessible::Id thirdArea = QAccessible::uniqueId(areaOf(window, "ThirdTitle"));
    ASSERT_TRUE(window->runAction(third, Action::DetachTab));
    TerminalWindow* detached = nullptr;
    ASSERT_TRUE(waitFor([&] {
        detached = qobject_cast<TerminalWindow*>(third->parent());
        return detached && detached != window;
    }));
    EXPECT_EQ(QAccessible::accessibleInterface(thirdArea), nullptr);
    EXPECT_EQ(areaOf(window, "ThirdTitle"), nullptr);
    EXPECT_TRUE(waitFor([&] { return areaOf(detached, "ThirdTitle") != nullptr; }));
    detached->clearScriptOwner(QStringLiteral("a@1"));
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, ADragOnAPaneDockTellsItsScriptWithItsPane) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    window->revealPane(first);
    DockEvents events(window);
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("tabs"), QStringLiteral("top"), 1,
                                                                          {dockRun(QStringLiteral("one"), QStringLiteral("a")), dockRun(QStringLiteral("    ")),
                                                                           dockRun(QStringLiteral("two"), QStringLiteral("b"))},
                                                                          QStringLiteral("pane")),
                                      second)
                    .isEmpty());
    const DockArea* tabs = nullptr;
    ASSERT_TRUE(waitFor([&] {
        tabs = shownDock(window, "tabs", second->id());
        return tabs && tabs->items().size() == 2;
    }));
    expectIdle(window);
    const qreal dpr = window->devicePixelRatio();
    const QPointF onOne = QRectF(tabs->itemRect(0)).center() / dpr;
    const QPointF onTwo = QRectF(tabs->itemRect(1)).center() / dpr;
    const DockArea::Item two = tabs->items()[1];
    const auto drags = [&] { return events.of("on_dock_drag"); };

    // The events name the pane, and only the moves count toward the limit.
    pressLeft(window, onOne);
    dragLeft(window, onOne + QPointF(QGuiApplication::styleHints()->startDragDistance(), 0));
    ASSERT_EQ(drags().size(), 1);
    EXPECT_EQ(drags()[0].pane, second->id());
    EXPECT_FALSE(drags()[0].limited);
    EXPECT_EQ(drags()[0].event[QStringLiteral("phase")], QStringLiteral("start"));
    EXPECT_EQ(drags()[0].event[QStringLiteral("scope")], QStringLiteral("pane"));
    EXPECT_EQ(drags()[0].event[QStringLiteral("dock")], QStringLiteral("tabs"));
    EXPECT_EQ(drags()[0].event[QStringLiteral("item")], QStringLiteral("a"));
    EXPECT_EQ(drags()[0].event[QStringLiteral("window")].toUInt(), window->id());
    EXPECT_EQ(drags()[0].event[QStringLiteral("row")].toInt(), 0);
    dragLeft(window, onTwo);
    ASSERT_EQ(drags().size(), 2);
    EXPECT_EQ(drags()[1].pane, second->id());
    EXPECT_TRUE(drags()[1].limited);
    EXPECT_EQ(drags()[1].event[QStringLiteral("phase")], QStringLiteral("move"));
    EXPECT_EQ(drags()[1].event[QStringLiteral("over")], QStringLiteral("b"));
    EXPECT_EQ(drags()[1].event[QStringLiteral("col")].toInt(), (two.from + two.to) / 2);
    releaseLeft(window, onTwo);
    ASSERT_EQ(drags().size(), 3);
    EXPECT_EQ(drags()[2].pane, second->id());
    EXPECT_FALSE(drags()[2].limited);
    EXPECT_EQ(drags()[2].event[QStringLiteral("phase")], QStringLiteral("end"));
    EXPECT_EQ(drags()[2].event[QStringLiteral("over")], QStringLiteral("b"));
    // No pane took the gesture: the focus stayed, and nothing selects.
    EXPECT_EQ(window->focusedPane(), first);
    EXPECT_FALSE(first->hasSelection());
    EXPECT_FALSE(second->hasSelection());

    // A new first press, whose release went elsewhere, cancels the drag.
    pressLeft(window, onOne);
    dragLeft(window, onTwo);
    ASSERT_EQ(drags().size(), 4);
    const QPointF onFirst = window->mapFromGlobal(QPointF(window->accessiblePaneRect(first).center()));
    pressLeft(window, onFirst);
    ASSERT_EQ(drags().size(), 5);
    EXPECT_EQ(drags()[4].pane, second->id());
    EXPECT_FALSE(drags()[4].limited);
    EXPECT_EQ(drags()[4].event[QStringLiteral("phase")], QStringLiteral("cancel"));
    EXPECT_EQ(drags()[4].event[QStringLiteral("item")], QStringLiteral("a"));
    EXPECT_EQ(drags()[4].event[QStringLiteral("over")], QStringLiteral("b"));
    releaseLeft(window, onFirst);
    EXPECT_EQ(drags().size(), 5);
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, APaneDockDragEndsWhenItsPaneClosesOrStopsShowing) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    DockEvents events(window);
    auto dock = [&](Pane* pane) {
        return window->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("tabs"), QStringLiteral("top"), 1, {dockRun(QStringLiteral("tab one"), QStringLiteral("a"))}, QStringLiteral("pane")), pane);
    };
    // A drag held on the item of @p pane's area, which shows.
    QPointF item;
    auto startDrag = [&](Pane* pane) {
        const DockArea* area = nullptr;
        if (!waitFor([&] {
                area = shownDock(window, "tabs", pane->id());
                return area && !area->items().empty();
            }))
            return false;
        item = QRectF(area->itemRect(0)).center() / window->devicePixelRatio();
        const auto before = events.of("on_dock_drag").size();
        pressLeft(window, item);
        dragLeft(window, item + QPointF(QGuiApplication::styleHints()->startDragDistance(), 0));
        return events.of("on_dock_drag").size() == before + 1;
    };
    auto expectCancel = [&](quint32 pane) {
        const DockEvent last = events.last("on_dock_drag");
        EXPECT_EQ(last.event[QStringLiteral("phase")], QStringLiteral("cancel"));
        EXPECT_EQ(last.event[QStringLiteral("item")], QStringLiteral("a"));
        EXPECT_EQ(last.pane, pane);
    };

    // The pane closes: the drag ends with "cancel" at once, and the item
    // under the pointer hears that the pointer left it.
    auto* closing = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(closing, nullptr);
    ASSERT_TRUE(dock(closing).isEmpty());
    ASSERT_TRUE(startDrag(closing));
    ASSERT_EQ(events.last("on_dock_hover").pane, closing->id());
    ASSERT_EQ(events.last("on_dock_hover").event[QStringLiteral("item")], QStringLiteral("a"));
    const quint32 closingId = closing->id();
    auto drags = events.of("on_dock_drag").size();
    window->requestClosePane(closing);
    ASSERT_EQ(events.of("on_dock_drag").size(), drags + 1);
    expectCancel(closingId);
    EXPECT_EQ(events.last("on_dock_hover").pane, closingId);
    EXPECT_FALSE(events.last("on_dock_hover").event.contains(QStringLiteral("item")));
    // The rest of the gesture tells nothing more.
    dragLeft(window, item + QPointF(30, 0));
    releaseLeft(window, item + QPointF(30, 0));
    EXPECT_EQ(events.of("on_dock_drag").size(), drags + 1);

    // Its tab goes to the background: the next pointer move cancels.
    auto* hidden = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(hidden, nullptr);
    ASSERT_TRUE(dock(hidden).isEmpty());
    ASSERT_TRUE(startDrag(hidden));
    drags = events.of("on_dock_drag").size();
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    ASSERT_NE(window->tabIndexOf(window->focusedPane()), window->tabIndexOf(hidden));
    dragLeft(window, item + QPointF(20, 0));
    ASSERT_EQ(events.of("on_dock_drag").size(), drags + 1);
    expectCancel(hidden->id());
    releaseLeft(window, item + QPointF(20, 0));
    EXPECT_EQ(events.of("on_dock_drag").size(), drags + 1);

    // A zoom hides its pane: the same. The tab lays out again for the tab
    // bar of two tabs first.
    window->revealPane(hidden);
    expectIdle(window);
    ASSERT_TRUE(startDrag(hidden));
    drags = events.of("on_dock_drag").size();
    ASSERT_TRUE(window->runAction(first, Action::TogglePaneZoom));
    ASSERT_TRUE(waitFor([&] { return window->accessiblePaneRect(hidden).isEmpty(); }));
    dragLeft(window, item + QPointF(20, 0));
    ASSERT_EQ(events.of("on_dock_drag").size(), drags + 1);
    expectCancel(hidden->id());
    releaseLeft(window, item + QPointF(20, 0));
    ASSERT_TRUE(window->runAction(first, Action::TogglePaneZoom));

    // The overview opens: its cards take the pointer.
    ASSERT_TRUE(startDrag(hidden));
    drags = events.of("on_dock_drag").size();
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    dragLeft(window, item + QPointF(20, 0));
    ASSERT_EQ(events.of("on_dock_drag").size(), drags + 1);
    expectCancel(hidden->id());
    releaseLeft(window, item + QPointF(20, 0));
    QVariantMap state;
    ASSERT_TRUE(window->scriptOverview(QStringLiteral("script@1"), first, QStringLiteral("close"), state).isEmpty());
    EXPECT_TRUE(waitFor([&] { return !window->overviewOpen(); }));
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, APaneDockDragEndsWhenItsWindowCloses) {
    WindowOptions options;
    options.command = {QStringLiteral("/bin/cat")};
    QPointer<TerminalWindow> other(application->newWindow(options));
    ASSERT_FALSE(other.isNull());
    other->resize(600, 400);
    other->show();
    ASSERT_TRUE(waitFor([&] { return other->isExposed() && other->submittedFrames() > 0; }));
    auto* pane = other->focusedPane();
    ASSERT_NE(pane, nullptr);
    const quint32 paneId = pane->id();
    DockEvents events(other);
    ASSERT_TRUE(other->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("tabs"), QStringLiteral("top"), 1, {dockRun(QStringLiteral("tab one"), QStringLiteral("a"))}, QStringLiteral("pane")), pane)
                    .isEmpty());
    const DockArea* dock = nullptr;
    ASSERT_TRUE(waitFor([&] {
        dock = shownDock(other, "tabs", paneId);
        return dock && !dock->items().empty();
    }));
    const QPointF item = QRectF(dock->itemRect(0)).center() / other->devicePixelRatio();
    pressLeft(other, item);
    dragLeft(other, item + QPointF(QGuiApplication::styleHints()->startDragDistance(), 0));
    ASSERT_EQ(events.of("on_dock_drag").size(), 1);
    // The script still runs: it hears that the drag ended and that the
    // pointer left the item. A pane whose program runs asks first.
    other->close();
    ASSERT_TRUE(waitFor([&] {
        if (auto* chrome = other ? other->findChild<ChromeController*>() : nullptr; chrome && chrome->confirmPromptOpen())
            chrome->answerConfirm(true);
        return events.of("on_dock_drag").size() == 2;
    }));
    const DockEvent cancel = events.last("on_dock_drag");
    EXPECT_EQ(cancel.event[QStringLiteral("phase")], QStringLiteral("cancel"));
    EXPECT_EQ(cancel.event[QStringLiteral("item")], QStringLiteral("a"));
    EXPECT_EQ(cancel.pane, paneId);
    EXPECT_EQ(events.last("on_dock_hover").pane, paneId);
    EXPECT_FALSE(events.last("on_dock_hover").event.contains(QStringLiteral("item")));
}

TEST_F(NativeWindow, APointerThatNeverEnteredAWindowIsOnNoItemOfItsDocks) {
    WindowOptions options;
    options.command = {QStringLiteral("/bin/cat")};
    std::unique_ptr<TerminalWindow> fresh(application->newWindow(options));
    ASSERT_NE(fresh, nullptr);
    // The compositor's own pointer may be over the new window: then it
    // enters, and the hover is real.
    struct EnterWatch : QObject {
        bool entered = false;
        bool eventFilter(QObject*, QEvent* e) override {
            if (e->type() == QEvent::Enter) entered = true;
            return false;
        }
    } watch;
    fresh->installEventFilter(&watch);
    QStringList hovers;
    int layouts = 0;
    QObject listener;
    QObject::connect(fresh.get(), &TerminalWindow::scriptDockEvent, &listener,
                     [&](const QString&, const QString& handler, quint32, const QVariantMap& event, bool) {
                         if (handler == QLatin1String("on_dock_hover")) hovers << event.value(QStringLiteral("item")).toString();
                         if (handler == QLatin1String("on_dock_layout")) ++layouts;
                     });
    fresh->resize(600, 400);
    fresh->show();
    ASSERT_TRUE(waitFor([&] { return fresh->isExposed() && fresh->submittedFrames() > 0; }));
    // A top area whose item starts in the window's top left cell.
    ASSERT_TRUE(fresh->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("top")},
                                                           {QStringLiteral("edge"), QStringLiteral("top")}, {QStringLiteral("tab_bar"), true},
                                                           {QStringLiteral("lines"), QVariantList{QVariantList{QVariantMap{
                                                               {QStringLiteral("text"), QStringLiteral("corner")}, {QStringLiteral("item"), QStringLiteral("corner")}}}}}})
                    .isEmpty());
    ASSERT_TRUE(waitFor([&] { return layouts > 0; }));
    expectIdle(fresh.get());
    fresh->clearScriptOwner(QStringLiteral("a@1"));
    if (watch.entered) GTEST_SKIP() << "the compositor's pointer entered the new window";
    EXPECT_TRUE(hovers.isEmpty()) << hovers.join(QLatin1Char(',')).toStdString();
}

TEST_F(NativeWindow, TabActionsOfAPaneActOnItsTabWithoutShowingItAndScriptsOpenItsMenu) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->openTab(launch, nullptr);
    auto* third = window->openTab(launch, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(third, nullptr);
    window->revealPane(first);
    expectIdle(window);
    ASSERT_EQ(window->tabs().size(), 3u);
    const quint32 secondTab = window->tabs()[1].id;
    const quint32 thirdTab = window->tabs()[2].id;
    // The third tab moves left of the second, and the current tab stays.
    ASSERT_TRUE(window->runAction(third, Action::MoveTabLeft));
    EXPECT_EQ(window->tabs()[1].id, thirdTab);
    EXPECT_EQ(window->focusedPane(), first);
    // Its cat still runs, so the window asks first.
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(window->runAction(second, Action::CloseTab));
    ASSERT_TRUE(chrome->confirmPromptOpen());
    EXPECT_EQ(window->focusedPane(), first);
    chrome->answerConfirm(true);
    ASSERT_TRUE(waitFor([&] { return window->tabs().size() == 2u; }));
    for (const auto& tab : window->tabs()) EXPECT_NE(tab.id, secondTab);
    EXPECT_EQ(window->focusedPane(), first);

    // A rename and a move to the right act on the tab where it is.
    ASSERT_TRUE(window->runAction(third, Action::RenameTab));
    ASSERT_TRUE(chrome->titlePromptOpen());
    chrome->answerTitle(QStringLiteral("renamed"));
    EXPECT_EQ(window->tabs()[1].title, QStringLiteral("renamed"));
    EXPECT_EQ(window->focusedPane(), first);
    auto* fourth = window->openTab(launch, nullptr);
    ASSERT_NE(fourth, nullptr);
    window->revealPane(first);
    expectIdle(window);
    ASSERT_EQ(window->tabs().size(), 3u);
    // The new tab opens beside the current one: first, fourth, third.
    const quint32 fourthTab = window->tabIdOf(fourth);
    ASSERT_EQ(window->tabs()[1].id, fourthTab);
    ASSERT_TRUE(window->runAction(fourth, Action::MoveTabRight));
    EXPECT_EQ(window->tabs()[2].id, fourthTab);
    EXPECT_EQ(window->tabs()[1].id, thirdTab);
    EXPECT_EQ(window->focusedPane(), first);
    // A detach takes the tab to a new window, focused on the pane named,
    // and leaves the current tab.
    auto* beside = window->splitPane(fourth, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(beside, nullptr);
    window->revealPane(first);
    expectIdle(window);
    ASSERT_EQ(window->tabIdOf(beside), fourthTab);
    ASSERT_TRUE(window->runAction(fourth, Action::DetachTab));
    ASSERT_TRUE(waitFor([&] { return fourth->parent() != window; }));
    EXPECT_EQ(window->focusedPane(), first);
    ASSERT_EQ(window->tabs().size(), 2u);
    auto* detached = qobject_cast<TerminalWindow*>(fourth->parent());
    ASSERT_NE(detached, nullptr);
    EXPECT_EQ(beside->parent(), detached);
    EXPECT_EQ(detached->focusedPane(), fourth);

    // A script opens the tab menu of a pane's tab, at the dock click before
    // it, through Control as ScriptControl routes the effect.
    SettingsStore settings(directory.filePath(QStringLiteral("menu.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    const auto callbacks = scriptCallbacks(control);
    auto tabMenu = [&](quint32 pane) {
        return callbacks.applyOwned(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("tab_menu")}, {QStringLiteral("pane"), pane}}, {});
    };
    ASSERT_FALSE(chrome->contextMenuOpen());
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("bar")},
                                                            {QStringLiteral("edge"), QStringLiteral("bottom")},
                                                            {QStringLiteral("lines"), QVariantList{QVariantList{QVariantMap{
                                                                {QStringLiteral("text"), QStringLiteral("tab")}, {QStringLiteral("item"), QStringLiteral("tab")}}}}}})
                    .isEmpty());
    expectIdle(window);
    const QPointF onItem(6, window->height() - 4);
    clickRight(window, onItem);
    // The pointer moves on before the script answers: the menu opens at the
    // click, not at the pointer.
    hoverAt(window, QPointF(window->width() / 2.0, window->height() / 2.0));
    const auto opened = tabMenu(third->id());
    ASSERT_TRUE(opened.error.isEmpty()) << opened.error.toStdString();
    ASSERT_TRUE(chrome->contextMenuOpen());
    EXPECT_NEAR(chrome->contextMenu().value(QStringLiteral("x")).toDouble(), onItem.x(), 1.0);
    EXPECT_NEAR(chrome->contextMenu().value(QStringLiteral("y")).toDouble(), onItem.y(), 1.0);
    // A menu that is open gives way. A prompt or the palette does not: the
    // menu would answer for it.
    ASSERT_TRUE(tabMenu(third->id()).error.isEmpty());
    ASSERT_TRUE(chrome->contextMenuOpen());
    chrome->setPaletteOpen(true);
    const auto again = tabMenu(third->id());
    EXPECT_FALSE(again.error.isEmpty());
    EXPECT_EQ(again.code, QStringLiteral("busy"));
    chrome->setPaletteOpen(false);
    ASSERT_TRUE(tabMenu(third->id()).error.isEmpty());
    ASSERT_TRUE(chrome->contextMenuOpen());
    // A pane that does not exist.
    EXPECT_EQ(tabMenu(999999).code, QStringLiteral("no_pane"));
    // Its items act on that tab: a rename, while another tab is current.
    int renameId = -1;
    for (const auto& item : chrome->contextMenu().value(QStringLiteral("items")).toList()) {
        if (item.toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Change Tab Title\u2026"))
            renameId = item.toMap().value(QStringLiteral("id")).toInt();
    }
    ASSERT_GE(renameId, 0);
    chrome->activateMenuItem(renameId);
    chrome->answerTitle(QStringLiteral("menu-tab"));
    EXPECT_EQ(window->tabs()[1].title, QStringLiteral("menu-tab"));
    EXPECT_EQ(window->focusedPane(), first);
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, ControlUndocksThroughAPaneThatMovedOrClosed) {
    SettingsStore settings(directory.filePath(QStringLiteral("undock.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    // Three tabs, so that the tab bar stays when one pane moves away.
    ASSERT_NE(window->openTab(launch, nullptr), nullptr);
    ASSERT_NE(window->openTab(launch, nullptr), nullptr);
    auto* moving = window->focusedPane();
    ASSERT_NE(moving, nullptr);
    expectIdle(window);
    const qreal height = chrome->paneArea().height();
    const QVariantMap bar{{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("bar")},
                          {QStringLiteral("edge"), QStringLiteral("bottom")}, {QStringLiteral("size"), 1}};
    const QVariantMap undock{{QStringLiteral("kind"), QStringLiteral("undock")}, {QStringLiteral("id"), QStringLiteral("bar")}};
    ASSERT_TRUE(control.scriptDock(QStringLiteral("a@1"), moving->id(), bar).isEmpty());
    ASSERT_TRUE(waitFor([&] { return chrome->paneArea().height() < height; }));

    // The pane moves to another window: the undock still removes the area
    // that it put in this one.
    WindowOptions options;
    options.command = {QStringLiteral("/bin/cat")};
    auto* destination = application->newWindow(options);
    ASSERT_NE(destination, nullptr);
    destination->show();
    control.attachWindow(destination);
    auto* resident = destination->focusedPane();
    ASSERT_NE(resident, nullptr);
    auto* destinationChrome = destination->findChild<ChromeController*>();
    ASSERT_NE(destinationChrome, nullptr);
    ASSERT_TRUE(window->movePaneTo(moving, destination, resident, SplitTree::Direction::Right).isEmpty());
    ASSERT_TRUE(waitFor([&] { return destinationChrome->paneArea().height() > 0; }));
    expectIdle(destination);
    // The same area in the new window, through a pane that was there.
    const qreal destinationHeight = destinationChrome->paneArea().height();
    ASSERT_TRUE(control.scriptDock(QStringLiteral("a@1"), resident->id(), bar).isEmpty());
    ASSERT_TRUE(waitFor([&] { return destinationChrome->paneArea().height() < destinationHeight; }));
    // An undock removes one area: the one in the pane's window first, then
    // the one the pane left behind.
    EXPECT_TRUE(control.scriptDock(QStringLiteral("a@1"), moving->id(), undock).isEmpty());
    EXPECT_TRUE(waitFor([&] { return destinationChrome->paneArea().height() == destinationHeight; }))
        << destinationChrome->paneArea().height();
    // A frame after the undock: an area that it removed here would be gone.
    ASSERT_TRUE(nextFrame(window));
    EXPECT_LT(chrome->paneArea().height(), height);
    EXPECT_TRUE(control.scriptDock(QStringLiteral("a@1"), moving->id(), undock).isEmpty());
    EXPECT_TRUE(waitFor([&] { return chrome->paneArea().height() == height; })) << chrome->paneArea().height();

    // The pane closes: the undock through its id still finds the area.
    quint32 created = 0;
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(control.split(first->id(), QStringLiteral("right"), {QStringLiteral("/bin/cat")}, {}, created).isEmpty());
    Pane* closing = nullptr;
    for (auto* pane : window->panes())
        if (pane->id() == created) closing = pane;
    ASSERT_NE(closing, nullptr);
    ASSERT_TRUE(control.scriptDock(QStringLiteral("a@1"), created, bar).isEmpty());
    ASSERT_TRUE(waitFor([&] { return chrome->paneArea().height() < height; }));
    QPointer<Pane> gone(closing);
    ASSERT_TRUE(closing->sendInput("\x04")); // its cat ends, and the pane with it
    ASSERT_TRUE(waitDeleted(gone));
    // The area stays with its pane gone, until the undock.
    ASSERT_TRUE(nextFrame(window));
    EXPECT_LT(chrome->paneArea().height(), height);
    EXPECT_TRUE(control.scriptDock(QStringLiteral("a@1"), created, undock).isEmpty());
    EXPECT_TRUE(waitFor([&] { return chrome->paneArea().height() == height; })) << chrome->paneArea().height();
    // An id that never docked is still not a pane.
    EXPECT_FALSE(control.scriptDock(QStringLiteral("a@1"), created + 1000, undock).isEmpty());
    // Its close would ask first, for the programs that still run.
    delete destination;
}

TEST_F(NativeWindow, ControlRefusesWhatItCannotDoInFull) {
    SettingsStore settings(directory.filePath(QStringLiteral("refuse.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    auto* pane = window->focusedPane();
    ASSERT_NE(pane, nullptr);
    // A copy over the limit is refused, not cut.
    QGuiApplication::clipboard()->setText(QStringLiteral("kept"));
    EXPECT_FALSE(control.copy(QString(Control::MaxCopyLength + 1, u'x')).isEmpty());
    EXPECT_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("kept"));
    // A split size must be a number, not a string that reads as one.
    quint32 created = 0;
    EXPECT_FALSE(control.split(pane->id(), QStringLiteral("right"), {QStringLiteral("/bin/cat")},
                               {{QStringLiteral("size"), QStringLiteral("0.5")}}, created)
                     .isEmpty());
    EXPECT_EQ(created, 0u);
    EXPECT_TRUE(control.split(pane->id(), QStringLiteral("right"), {QStringLiteral("/bin/cat")}, {{QStringLiteral("size"), 0.5}}, created)
                    .isEmpty());
    EXPECT_NE(created, 0u);
    // At most 8 docks for one script in a window, and 32 for all its
    // scripts together.
    const auto dock = [&](const QString& owner, int i, int size = 1) {
        return control.scriptDock(owner, pane->id(),
                                  {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("d%1").arg(i)},
                                   {QStringLiteral("edge"), QStringLiteral("left")}, {QStringLiteral("size"), size}});
    };
    const QString tooMany = QStringLiteral("too many docks: a script has at most 8 in a window, and a window at most 32");
    const QStringList owners{QStringLiteral("a@1"), QStringLiteral("b@1"), QStringLiteral("c@1"), QStringLiteral("d@1")};
    for (int i = 0; i < 8; ++i) ASSERT_TRUE(dock(owners[0], i).isEmpty());
    EXPECT_EQ(dock(owners[0], 8), tooMany);
    for (const QString& owner : owners.mid(1))
        for (int i = 0; i < 8; ++i) ASSERT_TRUE(dock(owner, i).isEmpty()) << owner.toStdString() << " " << i;
    // The window is full: a script with no dock gets none, and one that has
    // its 8 still changes them.
    EXPECT_EQ(dock(QStringLiteral("e@1"), 0), tooMany);
    EXPECT_TRUE(dock(owners[0], 0, 2).isEmpty());
    // A dock that goes makes room for another script's.
    window->clearScriptOwner(owners[3]);
    EXPECT_TRUE(dock(QStringLiteral("e@1"), 0).isEmpty());
    for (const QString& owner : owners) window->clearScriptOwner(owner);
    window->clearScriptOwner(QStringLiteral("e@1"));
}

TEST_F(NativeWindow, ControlSendsStatusAndContextOnlyWhenTheyChange) {
    SettingsStore settings(directory.filePath(QStringLiteral("status.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    auto* pane = window->focusedPane();
    ASSERT_NE(pane, nullptr);
    QList<QPair<QString, QString>> states;
    QStringList contexts;
    QObject listener;
    QObject::connect(&control, &Control::statusChanged, &listener, [&](quint32 from, const QString& state, const QString& text) {
        if (from == pane->id()) states.append({state, text});
    });
    QObject::connect(&control, &Control::contextChanged, &listener, [&](quint32 from, const QString& context) {
        if (from == pane->id()) contexts.append(context);
    });
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b]7501;state=working\x1b\\"); });
    ASSERT_TRUE(waitFor([&] { return !states.isEmpty() && states.last().first == QLatin1String("working"); }));
    // The same state again, in a delivery of its own: no new event.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b]7501;state=working\x1b\\"); });
    // The feed queues its delivery before it returns.
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(states.count(qMakePair(QStringLiteral("working"), QString())), 1);
    // A reset clears the last record: the state is idle, not empty.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.reset(); });
    ASSERT_TRUE(waitFor([&] { return states.last().first != QLatin1String("working"); }));
    EXPECT_EQ(states.last(), qMakePair(QStringLiteral("idle"), QString()));

    pane->withTerminal([](AfterglowVt::Terminal& t) {
        t.feed("\x1b]3008;start=b2;type=elevate;user=fuddles;targetuser=root;hostname=box\x1b\\");
    });
    ASSERT_TRUE(waitFor([&] { return !contexts.isEmpty(); }));
    // The context ends and starts again the same: no new event.
    pane->withTerminal([](AfterglowVt::Terminal& t) {
        t.feed("\x1b]3008;end=b2;exit=success\x1b\\");
        t.feed("\x1b]3008;start=b3;type=elevate;user=fuddles;targetuser=root;hostname=box\x1b\\");
    });
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(contexts.size(), 1) << contexts.join(QLatin1Char(',')).toStdString();
}

TEST_F(NativeWindow, ADividerDragKeepsTheSplitCursorThroughItsFrames) {
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    // Two tabs, so that a tab bar shows above the split.
    ASSERT_NE(window->openTab(launch, nullptr), nullptr);
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(waitFor([&] { return chrome->tabBarHeight() > 0; }));
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    expectIdle(window);
    const QPoint left = window->mapFromGlobal(window->accessiblePaneRect(first).topRight());
    const QPoint right = window->mapFromGlobal(window->accessiblePaneRect(second).topLeft());
    const QPointF divider((left.x() + right.x()) / 2.0, window->mapFromGlobal(window->accessiblePaneRect(first).center()).y());
    mouseAt(window, QEvent::MouseMove, divider, Qt::NoButton, Qt::NoButton);
    EXPECT_EQ(window->cursor().shape(), Qt::SplitHCursor);
    // Each drag move lays the panes out again in the next frame.
    const int cols = first->cols();
    mouseAt(window, QEvent::MouseButtonPress, divider, Qt::LeftButton, Qt::LeftButton);
    mouseAt(window, QEvent::MouseMove, divider + QPointF(10, 0), Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(nextFrame(window));
    mouseAt(window, QEvent::MouseMove, divider + QPointF(30, 0), Qt::NoButton, Qt::LeftButton);
    EXPECT_TRUE(waitFor([&] { return first->cols() > cols; })) << first->cols(); // the frames laid the panes out
    EXPECT_EQ(window->cursor().shape(), Qt::SplitHCursor);
    // Over the tab bar too, while the drag lasts.
    mouseAt(window, QEvent::MouseMove, QPointF(divider.x() + 30, chrome->tabBarHeight() / 2.0), Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(window->cursor().shape(), Qt::SplitHCursor);
    // A tab switch from the keyboard ends the drag: its divider is the other
    // tab's.
    ASSERT_TRUE(window->runAction(first, Action::PreviousTab));
    ASSERT_TRUE(settleQueued());
    mouseAt(window, QEvent::MouseMove, QPointF(divider.x() + 40, chrome->tabBarHeight() / 2.0), Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(nextFrame(window));
    EXPECT_NE(window->cursor().shape(), Qt::SplitHCursor);
    mouseAt(window, QEvent::MouseButtonRelease, QPointF(divider.x() + 40, chrome->tabBarHeight() / 2.0), Qt::LeftButton, Qt::NoButton);
    expectIdle(window);
}

TEST_F(NativeWindow, ATabBarDockThatDoesNotFitLeavesTheTabBarInPlace) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    ASSERT_NE(window->openTab(launch, nullptr), nullptr);
    ASSERT_TRUE(waitFor([&] { return chrome->tabBarHeight() > 0; }));
    expectIdle(window);
    // Two rows more than the panes have. The dock takes the tab bar's place,
    // but then the panes' 2 rows do not fit below it. (A resize is no use
    // here: the compositor can give the height back.)
    const int rows = window->focusedPane()->rows();
    const auto frames = window->submittedFrames();
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("b@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("tabs")},
                                                            {QStringLiteral("edge"), QStringLiteral("top")}, {QStringLiteral("size"), rows + 2},
                                                            {QStringLiteral("tab_bar"), true}})
                    .isEmpty());
    // The frame after the dock lays it out.
    ASSERT_TRUE(waitFor([&] { return window->submittedFrames() > frames; }));
    // The dock does not show, so the tab bar stays, and so do the panes.
    EXPECT_GT(chrome->tabBarHeight(), 0);
    EXPECT_EQ(window->focusedPane()->rows(), rows);
    window->clearScriptOwner(QStringLiteral("b@1"));
}

TEST_F(NativeWindow, ATabBarDockThatAFontChangeHidesGivesTheTabBarBack) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    ASSERT_NE(window->openTab(launch, nullptr), nullptr);
    ASSERT_TRUE(waitFor([&] { return chrome->tabBarHeight() > 0; }));
    expectIdle(window);
    const int rows = window->focusedPane()->rows();
    auto dock = [&](int size) {
        const auto frames = window->submittedFrames();
        EXPECT_TRUE(window->setScriptDock(QStringLiteral("b@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("tabs")},
                                                                {QStringLiteral("edge"), QStringLiteral("top")}, {QStringLiteral("size"), size},
                                                                {QStringLiteral("tab_bar"), true}})
                        .isEmpty());
        EXPECT_TRUE(waitFor([&] { return window->submittedFrames() > frames; }));
    };
    // The largest dock that still leaves the panes their 2 rows: a larger
    // font then leaves them less. (A resize is no use here: the compositor
    // can give the height back.)
    int size = rows + 2;
    for (; size > 0; --size) {
        dock(size);
        if (chrome->tabBarHeight() == 0) break;
    }
    ASSERT_GT(size, 0);
    // The font changes without a dock effect: only the layout of the next
    // frame finds that the dock no longer fits, and the bar comes back.
    for (int step = 0; step < 4; ++step) ASSERT_TRUE(window->runAction(window->focusedPane(), Action::FontBigger));
    EXPECT_TRUE(waitFor([&] { return chrome->tabBarHeight() > 0; }));
    ASSERT_TRUE(window->runAction(window->focusedPane(), Action::FontReset));
    EXPECT_TRUE(waitFor([&] { return chrome->tabBarHeight() == 0; }));
    window->clearScriptOwner(QStringLiteral("b@1"));
    EXPECT_TRUE(waitFor([&] { return chrome->tabBarHeight() > 0; }));
}

TEST_F(NativeWindow, ASplitRightAfterADockCountsTheRoomTheDockTakes) {
    auto* pane = window->focusedPane();
    expectIdle(window);
    const int cols = pane->cols();
    ASSERT_GT(cols, 12);
    // A left dock that leaves the pane 6 columns, too few for two panes of
    // 4. The split comes before the frame that lays the dock out.
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("side")},
                                                            {QStringLiteral("edge"), QStringLiteral("left")}, {QStringLiteral("size"), cols - 6}})
                    .isEmpty());
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    QString error;
    Pane* second = window->splitPane(pane, SplitTree::Direction::Right, launch, &error);
    EXPECT_EQ(second, nullptr);
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(waitFor([&] { return pane->cols() == 6; })) << pane->cols();
    for (auto* each : window->panes()) EXPECT_GE(each->cols(), 4);
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, ASplitRightAfterABiggerFontStartsAtTheSizeItKeeps) {
    // A left dock of 8 columns grows with the font. The split comes before
    // the frame that lays the bigger font out: the new pane counts the room
    // that the dock takes now, and its program starts at the size it keeps.
    auto* pane = window->focusedPane();
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("side")},
                                                            {QStringLiteral("edge"), QStringLiteral("left")}, {QStringLiteral("size"), 8}})
                    .isEmpty());
    expectIdle(window);
    for (int step = 0; step < 4; ++step) ASSERT_TRUE(window->runAction(pane, Action::FontBigger));
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    QString error;
    Pane* second = window->splitPane(pane, SplitTree::Direction::Right, launch, &error);
    ASSERT_NE(second, nullptr) << error.toStdString();
    const int cols = second->cols();
    const int rows = second->rows();
    expectIdle(window);
    EXPECT_EQ(second->cols(), cols);
    EXPECT_EQ(second->rows(), rows);
    window->clearScriptOwner(QStringLiteral("a@1"));
}

// --- pane callbacks, reported directories, replies, selections and the overview's key mode ---

TEST_F(NativeWindow, StatusAndContextChangesOnTheGuiThreadDoNotDeadlock) {
    auto* pane = window->focusedPane();
    int statuses = 0;
    int contexts = 0;
    QObject listener;
    QObject::connect(pane, &Pane::statusChanged, &listener, [&] { ++statuses; });
    QObject::connect(pane, &Pane::contextUpdated, &listener, [&] { ++contexts; });
    // The receivers read status() and contextKind(), which lock the pane:
    // a direct emission under withTerminal would never return.
    pane->withTerminal([](AfterglowVt::Terminal& t) {
        t.feed("\x1b]7501;state=working:progress=40\x1b\\");
        t.feed("\x1b]3008;start=w2;type=remote;user=someone;hostname=far-away-host\x1b\\");
    });
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.reset(); });
    EXPECT_TRUE(waitFor([&] { return statuses > 0 && contexts > 0; }));
    ASSERT_TRUE(settleQueued());
    // A burst is one delivery.
    EXPECT_EQ(statuses, 1);
    EXPECT_EQ(pane->contextKind(), QStringLiteral("remote"));
}

// Window.workingDirectory names a directory that does not exist: a new tab
// inherits the directory of the pane that it comes from, as the setting's
// own description says, not the home directory. A split takes the same path
// (inheritedDirectory).
TEST_F(NativeWindow, AWorkingDirectoryThatIsNoDirectoryInherits) {
    restartApplication(QStringLiteral("gone.conf"),
        testSettings({{QStringLiteral("Window"), QJsonObject{{QStringLiteral("workingDirectory"), directory.filePath(QStringLiteral("gone"))}}}}),
        {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    ASSERT_NE(pane, nullptr);
    const QString here = QDir(directory.path()).canonicalPath();
    feedFromReader(pane, "\x1b]1337;CurrentDir=" + here.toStdString() + "\x07");
    ASSERT_TRUE(waitFor([&] { return pane->workingDirectory() == here; })) << pane->workingDirectory().toStdString();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* tab = window->openTab(launch, nullptr, pane);
    ASSERT_NE(tab, nullptr);
    EXPECT_TRUE(waitFor([&] { return tab->workingDirectory() == here; })) << tab->workingDirectory().toStdString();
}

TEST_F(NativeWindow, ReportedDirectoryWinsWhenLocalAndANewTabStartsThere) {
    SettingsStore settings(directory.filePath(QStringLiteral("dirs.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    auto* pane = window->focusedPane();
    const QString spaced = directory.filePath(QStringLiteral("with space"));
    ASSERT_TRUE(QDir().mkpath(spaced));
    const QString fromProc = pane->workingDirectory();
    ASSERT_NE(fromProc, spaced);
    const auto report = [pane](const QByteArray& osc) {
        pane->withTerminal([&osc](AfterglowVt::Terminal& t) { t.feed(std::string_view(osc.constData(), std::size_t(osc.size()))); });
    };
    // OSC 7 is a file:// URI, percent encoded; a local host is accepted.
    report("\x1b]7;file://localhost" + QUrl::toPercentEncoding(spaced, "/") + "\x07");
    EXPECT_EQ(pane->workingDirectory(), spaced);
    // Another host's directory is not here: /proc answers again.
    report("\x1b]7;file://far-away-host/srv\x07");
    EXPECT_EQ(pane->workingDirectory(), fromProc);
    // OSC 1337 CurrentDir gives a plain path.
    report("\x1b]1337;CurrentDir=" + spaced.toUtf8() + "\x07");
    EXPECT_EQ(pane->workingDirectory(), spaced);
    // A directory that does not exist here (a container's) falls back too.
    report("\x1b]1337;CurrentDir=/afterglow-test-not-a-directory\x07");
    EXPECT_EQ(pane->workingDirectory(), fromProc);
    report("\x1b]1337;CurrentDir=" + spaced.toUtf8() + "\x07");
    // newTab from a pane starts where that pane is.
    quint32 created = 0;
    ASSERT_TRUE(control.newTab(pane->id(), {}, {QStringLiteral("/bin/cat")}, created).isEmpty());
    Pane* added = nullptr;
    for (Pane* p : window->panes()) if (p->id() == created) added = p;
    ASSERT_NE(added, nullptr);
    EXPECT_TRUE(waitFor([&] { return added->workingDirectory() == spaced; }));
}

TEST_F(NativeWindow, ProtocolRepliesLeaveTheViewWhereItIs) {
    auto* pane = window->focusedPane();
    ASSERT_NE(pane, nullptr);
    pane->setScrollPolicy(false, true);
    pane->withTerminal([](AfterglowVt::Terminal& t) {
        for (int i = 0; i < 200; ++i) t.feed("line\r\n");
        t.scrollViewport(10);
    });
    ASSERT_GT(viewportOffset(pane), 0u);
    EXPECT_TRUE(pane->sendReply("\x1b[I"));
    EXPECT_GT(viewportOffset(pane), 0u);
    // Typing snaps back to the bottom.
    EXPECT_TRUE(pane->sendInput("x"));
    EXPECT_EQ(viewportOffset(pane), 0u);
}

TEST_F(NativeWindow, SelectionFollowsLinesThatInsertAndDeleteMove) {
    auto* pane = window->focusedPane();
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[2J\x1b[Hzero\r\none\r\ntwo\r\nthree\r\n"); });
    pane->startSelection(pane->selectionPointAt(0, 2), AfterglowVt::SelectionMode::Normal, true);
    pane->extendSelection(pane->selectionPointAt(2, 2));
    ASSERT_EQ(pane->selectedText(), QStringLiteral("two"));
    // DL at the first row: "two" moves up a row, and the selection with it.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[1;1H\x1b[M"); });
    EXPECT_TRUE(pane->hasSelection());
    EXPECT_EQ(pane->selectedText(), QStringLiteral("two"));
    // IL above it moves it down again.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[1;1H\x1b[L"); });
    EXPECT_EQ(pane->selectedText(), QStringLiteral("two"));
    // A region scroll that pushes it out of the region clears it.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[1;4r\x1b[1;1H\x1b[3M\x1b[r"); });
    EXPECT_FALSE(pane->hasSelection());
}

TEST_F(NativeWindow, ClosingTheOverviewEndsTheScriptKeyModeItDrove) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    ASSERT_NE(window->splitPane(first, SplitTree::Direction::Right, launch, nullptr), nullptr);
    ASSERT_TRUE(settleQueued());
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    // A key mode from before the overview stays when it closes.
    ASSERT_TRUE(window->startScriptKeyMode(QStringLiteral("script@1"), first, QStringLiteral("Before")).isEmpty());
    QVariantMap state;
    ASSERT_TRUE(window->scriptOverview(QStringLiteral("script@1"), first, QStringLiteral("open"), state).isEmpty());
    ASSERT_TRUE(window->scriptOverview(QStringLiteral("script@1"), first, QStringLiteral("close"), state).isEmpty());
    EXPECT_EQ(chrome->keyMode(), QStringLiteral("Before"));
    window->clearScriptOwner(QStringLiteral("script@1"));
    // One started while the overview is open ends with it: closeOverview is
    // where a click, a script and the keys all close it.
    ASSERT_TRUE(window->scriptOverview(QStringLiteral("script@1"), first, QStringLiteral("open"), state).isEmpty());
    ASSERT_TRUE(window->startScriptKeyMode(QStringLiteral("script@1"), first, QStringLiteral("Overview keys")).isEmpty());
    EXPECT_EQ(chrome->keyMode(), QStringLiteral("Overview keys"));
    ASSERT_TRUE(window->scriptOverview(QStringLiteral("script@1"), first, QStringLiteral("choose"), state).isEmpty());
    EXPECT_FALSE(window->overviewOpen());
    EXPECT_TRUE(chrome->keyMode().isEmpty());
    // The next key reaches the shell, not the script.
    int keys = 0;
    QObject listener;
    QObject::connect(window, &TerminalWindow::scriptKey, &listener, [&](const QString&, quint32, const QVariantMap&) { ++keys; });
    auto* chosen = window->focusedPane();
    ASSERT_NE(chosen, nullptr);
    sendKey(window, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    sendKey(window, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    EXPECT_TRUE(waitFor([&] { return screenOf(chosen).contains(QLatin1Char('q')); }));
    EXPECT_EQ(keys, 0);
}

// --- overlays: programs that run over a pane and answer whoever opened them ---

// What an overlay answered: once, from PaneOverlay's reply.
struct OverlayAnswer {
    bool done = false;
    QString error;
    QString code;
    QVariantMap result;
};
// Opens @p argv over @p pane with @p options (those of afterglow.overlay);
// the answer arrives in the returned record. @p refusal gets the
// window's refusal, empty when the overlay opened.
std::shared_ptr<OverlayAnswer> runOverlay(TerminalWindow* window, Pane* pane, const QStringList& argv, const QVariantMap& options = {},
                                          QString* refusal = nullptr, QString* refusalCode = nullptr,
                                          const QString& owner = QStringLiteral("test@1"),
                                          const QString& request = QStringLiteral("afterglow.await:1:1")) {
    auto answer = std::make_shared<OverlayAnswer>();
    QString error;
    const auto spec = OverlaySpec::read(argv, options, &error);
    if (!spec) {
        ADD_FAILURE() << error.toStdString();
        return answer;
    }
    QString code;
    error = window->openOverlay(owner, request, pane, *spec,
        [answer](const QString& error, const QString& code, const QVariantMap& result) {
            answer->done = true;
            answer->error = error;
            answer->code = code;
            answer->result = result;
        }, code);
    if (refusal) *refusal = error;
    if (refusalCode) *refusalCode = code;
    return answer;
}
void typeText(TerminalWindow* window, const QString& text) {
    for (const QChar c : text) {
        const int key = c == QLatin1Char('\r') ? int(Qt::Key_Return) : c.isLetter() ? int(Qt::Key_A) + (c.toLower().unicode() - u'a') : int(c.unicode());
        sendKey(window, key, Qt::NoModifier, QString(c));
    }
}
// The overlay over @p pane has faded all the way in or out.
bool overlaySettled(TerminalWindow* window, const Pane* pane) {
    return waitFor([&] { return !window->overlayFade(pane).second; });
}

TEST_F(NativeWindow, AnOverlayOverOnePaneTakesItsInputAndReturnsItsResult) {
    Pane* pane = window->focusedPane();
    ASSERT_NE(pane, nullptr);
    QString refusal;
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                            QStringLiteral("stty -echo; printf 'overlay-screen-text' > /dev/tty; read line; printf 'picked:%s' \"$line\"")},
                             {{QStringLiteral("title"), QStringLiteral("Pick a line")}}, &refusal);
    ASSERT_TRUE(refusal.isEmpty()) << refusal.toStdString();
    ASSERT_NE(pane->overlay(), nullptr);
    Pane* program = pane->overlay()->pane();
    // It fades in, has the focus with its host, and is no pane of its own.
    EXPECT_TRUE(window->overlayFade(pane).second);
    EXPECT_EQ(window->focusedPane(), pane);
    EXPECT_FALSE(window->panes().contains(program));
    EXPECT_EQ(window->title(), QStringLiteral("Pick a line"));
    EXPECT_EQ(program->cols(), pane->cols());
    EXPECT_EQ(program->rows(), pane->rows());
    ASSERT_TRUE(waitFor([&] { return screenOf(program).contains(QStringLiteral("overlay-screen-text")); }));
    // Assistive technology reads the overlay's text in place of the pane's.
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    auto* accessible = QAccessible::queryAccessibleInterface(pane);
    ASSERT_NE(accessible, nullptr);
    EXPECT_TRUE(accessible->text(QAccessible::Value).contains(QStringLiteral("overlay-screen-text")));
    // Its selection is the overlay's too.
    ASSERT_TRUE(window->runAction(pane, Action::SelectAll));
    EXPECT_TRUE(program->hasSelection());
    EXPECT_FALSE(pane->hasSelection());
    program->clearSelection();
    ASSERT_TRUE(overlaySettled(window, pane));
    EXPECT_EQ(window->overlayFade(pane).first, 1.0);
    expectIdle(window);

    // Connected after the window's own handler of the end: this one sees the
    // fade that the end starts before a frame can advance it. After the wait
    // below, frames may have finished a short fade already.
    bool fadingOut = false;
    QObject listener;
    QObject::connect(pane->overlay(), &PaneOverlay::ended, &listener, [&] { fadingOut = window->overlayFade(pane).second; });
    typeText(window, QStringLiteral("ab\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_TRUE(answer->error.isEmpty()) << answer->error.toStdString();
    EXPECT_EQ(answer->result.value(QStringLiteral("exit_code")).toInt(), 0);
    EXPECT_EQ(answer->result.value(QStringLiteral("stdout")).toString(), QStringLiteral("picked:ab"));
    EXPECT_FALSE(answer->result.value(QStringLiteral("truncated")).toBool());
    // The overlay is off its host at once, and fades out on its last frame.
    EXPECT_EQ(pane->overlay(), nullptr);
    EXPECT_TRUE(fadingOut);
    EXPECT_FALSE(accessible->text(QAccessible::Value).contains(QStringLiteral("overlay-screen-text")));
    ASSERT_TRUE(overlaySettled(window, pane));
    EXPECT_EQ(window->overlayFade(pane).first, 0.0);
    expectIdle(window);
    // The host has the input again, and got none of the overlay's keys:
    // they would show before its own.
    EXPECT_NE(window->title(), QStringLiteral("Pick a line"));
    typeText(window, QStringLiteral("zz\r"));
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("zz")); })) << screenOf(pane).toStdString();
    EXPECT_FALSE(screenOf(pane).contains(QStringLiteral("ab"))) << screenOf(pane).toStdString();
}

TEST_F(NativeWindow, AnOverlayOverOneOfTwoPanesFollowsTheFocus) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(window->focusedPane(), second);
    // Over the pane without the focus: the focus moves to it.
    auto answer = runOverlay(window, first, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                             QStringLiteral("stty -echo; read a; read b; printf '%s-%s' \"$a\" \"$b\"")});
    ASSERT_NE(first->overlay(), nullptr);
    EXPECT_EQ(window->focusedPane(), first);
    typeText(window, QStringLiteral("one\r"));
    // The focus can go elsewhere; the overlay waits, and gets the keys again
    // when its pane has the focus back.
    window->revealPane(second);
    ASSERT_EQ(window->focusedPane(), second);
    typeText(window, QStringLiteral("side\r"));
    EXPECT_TRUE(waitFor([&] { return screenOf(second).contains(QStringLiteral("side")); }));
    EXPECT_FALSE(answer->done);
    window->revealPane(first);
    typeText(window, QStringLiteral("two\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_EQ(answer->result.value(QStringLiteral("stdout")).toString(), QStringLiteral("one-two"));
    EXPECT_EQ(window->focusedPane(), first);
    // A marker goes through each pane's input after the keys: a key that
    // reached a pane shows before it.
    for (Pane* pane : {first, second}) {
        ASSERT_TRUE(pane->sendInput("@marker@\n"));
        ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("@marker@")); })) << screenOf(pane).toStdString();
    }
    EXPECT_FALSE(screenOf(first).contains(QStringLiteral("one"))) << screenOf(first).toStdString();
    EXPECT_FALSE(screenOf(second).contains(QStringLiteral("two"))) << screenOf(second).toStdString();
    ASSERT_TRUE(overlaySettled(window, first));
    expectIdle(window);
}

TEST_F(NativeWindow, ClosingTheHostPaneCancelsItsOverlay) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    const QString pidFile = directory.filePath(QStringLiteral("overlay.pid"));
    auto answer = runOverlay(window, second, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                              QStringLiteral("echo $$ > '%1'; exec sleep 30").arg(pidFile)},
                             {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(second->overlay(), nullptr);
    ASSERT_TRUE(waitFor([&] { return QFileInfo(pidFile).size() > 0; }));
    QFile file(pidFile);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const pid_t pid = pid_t(file.readAll().trimmed().toInt());
    ASSERT_GT(pid, 0);
    window->requestClosePane(second);
    ASSERT_TRUE(answer->done);
    EXPECT_EQ(answer->code, QStringLiteral("cancelled"));
    // The program got SIGHUP, and is gone.
    EXPECT_TRUE(waitDeleting([&] {
        return ::kill(pid, 0) != 0;
    }));
    EXPECT_EQ(window->focusedPane(), first);
    expectIdle(window);
}

TEST_F(NativeWindow, TheCancelOverlayActionEndsTheFocusedPanesOverlay) {
    Pane* pane = window->focusedPane();
    const QStringList sleeper{QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("exec sleep 30")};
    // Its keys reach the window while the overlay has the keyboard: the
    // program does not get them.
    auto keyed = runOverlay(window, pane, sleeper, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(pane->overlay(), nullptr);
    sendKey(window, Qt::Key_Escape, Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    ASSERT_TRUE(waitFor([&] { return keyed->done; }));
    EXPECT_EQ(keyed->code, QStringLiteral("cancelled"));
    EXPECT_EQ(pane->overlay(), nullptr);
    // The action as afterglow ctl and scripts run it.
    auto run = runOverlay(window, pane, sleeper, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(pane->overlay(), nullptr);
    EXPECT_TRUE(window->runAction(pane, Action::CancelOverlay));
    ASSERT_TRUE(waitFor([&] { return run->done; }));
    EXPECT_EQ(run->code, QStringLiteral("cancelled"));
    EXPECT_EQ(pane->overlay(), nullptr);
    ASSERT_TRUE(overlaySettled(window, pane));
    expectIdle(window);
}

TEST_F(NativeWindow, MovingAPaneOrItsTabKeepsItsOverlay) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    auto answer = runOverlay(window, second, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                              QStringLiteral("stty -echo; read a; read b; printf '%s-%s' \"$a\" \"$b\"")});
    ASSERT_NE(second->overlay(), nullptr);
    // To a tab of its own in the same window.
    ASSERT_TRUE(window->runAction(second, Action::MovePaneToTab));
    ASSERT_EQ(window->tabs().size(), 2);
    ASSERT_NE(second->overlay(), nullptr);
    ASSERT_EQ(window->focusedPane(), second);
    typeText(window, QStringLiteral("here\r"));
    // Its whole tab to a new window.
    ASSERT_TRUE(window->runAction(second, Action::DetachTab));
    ASSERT_TRUE(waitFor([&] { return second->parent() != window; }));
    auto* moved = qobject_cast<TerminalWindow*>(second->parent());
    ASSERT_NE(moved, nullptr);
    ASSERT_NE(second->overlay(), nullptr);
    EXPECT_EQ(moved->focusedPane(), second);
    ASSERT_TRUE(waitFor([&] { return moved->isExposed() && moved->submittedFrames() > 0; }));
    typeText(moved, QStringLiteral("there\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_EQ(answer->result.value(QStringLiteral("stdout")).toString(), QStringLiteral("here-there"));
    // A marker goes through the host's input after the keys: a key that
    // reached it shows before it.
    ASSERT_TRUE(second->sendInput("@marker@\n"));
    ASSERT_TRUE(waitFor([&] { return screenOf(second).contains(QStringLiteral("@marker@")); })) << screenOf(second).toStdString();
    EXPECT_FALSE(screenOf(second).contains(QStringLiteral("here"))) << screenOf(second).toStdString();
    expectIdle(moved);
}

TEST_F(NativeWindow, AResizeReachesTheOverlayAndItsHost) {
    Pane* pane = window->focusedPane();
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; read x; stty size")});
    ASSERT_NE(pane->overlay(), nullptr);
    Pane* program = pane->overlay()->pane();
    const int cols = pane->cols();
    // A split takes half of the pane: the layout gives both the new size.
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    ASSERT_NE(window->splitPane(pane, SplitTree::Direction::Right, launch, nullptr), nullptr);
    ASSERT_TRUE(waitFor([&] { return pane->cols() < cols; }));
    EXPECT_EQ(program->cols(), pane->cols());
    EXPECT_EQ(program->rows(), pane->rows());
    window->revealPane(pane);
    typeText(window, QStringLiteral("\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    // Its program saw the new size on its terminal.
    EXPECT_EQ(answer->result.value(QStringLiteral("stdout")).toString(),
              QStringLiteral("%1 %2\n").arg(pane->rows()).arg(pane->cols()));
}

TEST_F(NativeWindow, AnOverlayOverAZoomedPaneHasTheZoomedSize) {
    Pane* pane = window->focusedPane();
    const int alone = pane->cols();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* other = window->splitPane(pane, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(other, nullptr);
    window->revealPane(pane);
    ASSERT_TRUE(waitFor([&] { return pane->cols() < alone; }));
    const int half = pane->cols();
    ASSERT_TRUE(window->runAction(pane, Action::TogglePaneZoom));
    ASSERT_TRUE(waitFor([&] { return pane->cols() > half && window->accessiblePaneRect(other).isEmpty(); })) << pane->cols();
    // The zoom animates: the grid has its size once nothing moves.
    expectIdle(window);
    const int cols = pane->cols(), rows = pane->rows();
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; read x; stty size")});
    ASSERT_NE(pane->overlay(), nullptr);
    Pane* program = pane->overlay()->pane();
    // The overlay takes the zoomed grid, and the zoom stays.
    EXPECT_EQ(program->cols(), cols);
    EXPECT_EQ(program->rows(), rows);
    ASSERT_TRUE(overlaySettled(window, pane));
    expectIdle(window);
    EXPECT_EQ(pane->cols(), cols);
    EXPECT_TRUE(window->accessiblePaneRect(other).isEmpty());
    typeText(window, QStringLiteral("\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_EQ(answer->result.value(QStringLiteral("stdout")).toString(), QStringLiteral("%1 %2\n").arg(rows).arg(cols));
}

TEST_F(NativeWindow, AnOverlayOnAPaneInAnotherTabShowsThatTabAndOpensAtOnce) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")};
    Pane* background = window->openTab(launch, nullptr);
    ASSERT_NE(background, nullptr);
    window->revealPane(first);
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    expectIdle(window);
    const quint32 tab = window->tabIdOf(background);
    ASSERT_NE(window->activeTabId(), tab);
    auto answer = runOverlay(window, background, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                                  QStringLiteral("stty -echo; read x; printf 'got:%s' \"$x\"")});
    ASSERT_NE(background->overlay(), nullptr);
    // Its tab shows, with the focus on its pane, and slides in.
    EXPECT_EQ(window->activeTabId(), tab);
    EXPECT_EQ(window->focusedPane(), background);
    EXPECT_TRUE(window->tabSwitching());
    // The tab transition draws the change: the overlay is there at once,
    // with no fade of its own over the slide, also once the slide ends.
    EXPECT_EQ(window->overlayFade(background), std::make_pair(qreal(1), false));
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    EXPECT_EQ(window->overlayFade(background), std::make_pair(qreal(1), false));
    expectIdle(window);
    typeText(window, QStringLiteral("yes\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_EQ(answer->result.value(QStringLiteral("stdout")).toString(), QStringLiteral("got:yes"));
    // A marker goes through each pane's input after the keys: a key that
    // reached a pane shows before it.
    for (Pane* pane : {background, first}) {
        ASSERT_TRUE(pane->sendInput("@marker@\n"));
        ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("@marker@")); })) << screenOf(pane).toStdString();
    }
    EXPECT_FALSE(screenOf(background).contains(QStringLiteral("yes"))) << screenOf(background).toStdString();
    EXPECT_FALSE(screenOf(first).contains(QStringLiteral("yes"))) << screenOf(first).toStdString();
    // In a tab that has settled, the end fades out and stops.
    ASSERT_TRUE(overlaySettled(window, background));
    expectIdle(window);
}

TEST_F(NativeWindow, AnOverlayStartsInItsDirectoryOrInHome) {
    // A home of the test's own, which the window reads from $HOME: no
    // program of a test starts in the user's home. It is not the test's
    // directory, so that the two starts below differ.
    const QString homeDirectory = directory.filePath(QStringLiteral("home"));
    ASSERT_TRUE(QDir().mkpath(homeDirectory));
    const Saved home("HOME", QFile::encodeName(homeDirectory));
    Pane* pane = window->focusedPane();
    const QStringList pwd{QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("pwd -P")};
    // A path that a remote shell reported is no directory here.
    auto remote = runOverlay(window, pane, pwd, {{QStringLiteral("cwd"), QStringLiteral("/nonexistent/afterglow/remote")}});
    ASSERT_TRUE(waitFor([&] { return remote->done; }));
    EXPECT_EQ(remote->result.value(QStringLiteral("stdout")).toString(), QDir(homeDirectory).canonicalPath() + QLatin1Char('\n'));
    ASSERT_TRUE(waitFor([&] { return pane->overlay() == nullptr; }));
    auto local = runOverlay(window, pane, pwd, {{QStringLiteral("cwd"), directory.path()}});
    ASSERT_TRUE(waitFor([&] { return local->done; }));
    EXPECT_EQ(local->result.value(QStringLiteral("stdout")).toString(), QDir(directory.path()).canonicalPath() + QLatin1Char('\n'));
}

TEST_F(NativeWindow, AnOverlayReadsItsStdinAndAnswersThroughItsResultFile) {
    Pane* pane = window->focusedPane();
    // fzf's way: the list on stdin, the choice in a file, the host's ID,
    // and the mark of an overlay's terminal.
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                            QStringLiteral("head -n1 > \"$AFTERGLOW_RESULT\"; echo \"$AFTERGLOW_PANE_ID\" >> \"$AFTERGLOW_RESULT\"; "
                                                           "echo \"$AFTERGLOW_OVERLAY\" >> \"$AFTERGLOW_RESULT\"; "
                                                           "echo not-captured")},
                             {{QStringLiteral("stdin"), QStringLiteral("first\nsecond\n")}, {QStringLiteral("capture"), QStringLiteral("file")}});
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_EQ(answer->result.value(QStringLiteral("stdout")).toString(), QStringLiteral("first\n%1\n1\n").arg(pane->id()));
    EXPECT_EQ(answer->result.value(QStringLiteral("exit_code")).toInt(), 0);
    // A program that fails to start is refused at once.
    QString refusal, code;
    runOverlay(window, pane, {QStringLiteral("/nonexistent/afterglow-program")}, {}, &refusal, &code);
    EXPECT_EQ(code, QStringLiteral("spawn_failed"));
    EXPECT_FALSE(refusal.isEmpty());
    EXPECT_EQ(pane->overlay(), nullptr);
}

TEST_F(NativeWindow, AnOverlaysResultFileIsTheUsersAloneAndGoesWithTheOverlay) {
    Pane* pane = window->focusedPane();
    // The program tells where its result file is, and waits.
    const QString named = directory.filePath(QStringLiteral("result-path"));
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                            QStringLiteral("stty -echo; printf '%s' \"$AFTERGLOW_RESULT\" > '%1'; read x; "
                                                           "echo kept > \"$AFTERGLOW_RESULT\"").arg(named)},
                             {{QStringLiteral("capture"), QStringLiteral("file")}});
    ASSERT_NE(pane->overlay(), nullptr);
    ASSERT_TRUE(waitFor([&] { return QFileInfo(named).size() > 0; }));
    const QByteArray result = fileContent(named);
    // While the program runs, only the user can read or write it.
    struct stat info = {};
    ASSERT_EQ(::stat(result.constData(), &info), 0) << result.toStdString();
    EXPECT_EQ(info.st_mode & 07777, mode_t(0600));
    EXPECT_EQ(info.st_uid, ::getuid());
    typeText(window, QStringLiteral("\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_EQ(answer->result.value(QStringLiteral("stdout")).toString(), QStringLiteral("kept\n"));
    // It goes with the overlay, which the window deletes later.
    EXPECT_TRUE(waitDeleting([&] {
        return !QFileInfo::exists(QFile::decodeName(result));
    })) << result.toStdString();
}

TEST_F(NativeWindow, OnlyAnOverlayThatAnswersThroughAFileGetsAfterglowResult) {
    Pane* pane = window->focusedPane();
    // Neither the spec's env nor what Afterglow inherited names a result
    // file that nobody reads.
    const Saved inherited("AFTERGLOW_RESULT", "/inherited");
    const QString fromEnv = directory.filePath(QStringLiteral("named-by-env"));
    const QVariantMap env{{QStringLiteral("AFTERGLOW_RESULT"), fromEnv}};
    const QString print = QStringLiteral("printf '%s' \"${AFTERGLOW_RESULT-unset}\"");
    auto captured = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), print}, {{QStringLiteral("env"), env}});
    ASSERT_TRUE(waitFor([&] { return captured->done; }));
    EXPECT_EQ(captured->result.value(QStringLiteral("stdout")).toString(), QStringLiteral("unset"));
    const QString shown = directory.filePath(QStringLiteral("shown"));
    auto none = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), print + QStringLiteral(" > '%1'").arg(shown)},
                           {{QStringLiteral("env"), env}, {QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_TRUE(waitFor([&] { return none->done; }));
    EXPECT_EQ(fileContent(shown), QByteArray("unset"));
    // With a result file, the variable names that file, whatever env says.
    auto file = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("echo answered > \"$AFTERGLOW_RESULT\"")},
                           {{QStringLiteral("env"), env}, {QStringLiteral("capture"), QStringLiteral("file")}});
    ASSERT_TRUE(waitFor([&] { return file->done; }));
    EXPECT_EQ(file->result.value(QStringLiteral("stdout")).toString(), QStringLiteral("answered\n"));
    EXPECT_FALSE(QFileInfo::exists(fromEnv));
}

TEST_F(NativeWindow, OverlaysAndScriptDialogsRefuseEachOther) {
    Pane* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    // Through the script callbacks too, as ScriptControl applies the
    // effects: a script sees the code of each refusal.
    SettingsStore settings(directory.filePath(QStringLiteral("refuse-overlay.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    const auto callbacks = scriptCallbacks(control);
    const auto ignore = [](const ScriptHost::EffectResult&) {};
    const QVariantMap prompt{{QStringLiteral("kind"), QStringLiteral("prompt")}, {QStringLiteral("pane"), pane->id()},
                             {QStringLiteral("title"), QStringLiteral("Name")},
                             {QStringLiteral("request_id"), QStringLiteral("afterglow.await:2:1")}};
    // ScriptHost hands ScriptControl the overlay as a spec it has read.
    QString specError;
    const auto spec = OverlaySpec::read({QStringLiteral("/bin/true")}, {}, &specError);
    ASSERT_TRUE(spec) << qPrintable(specError);
    const QVariantMap overlay{{QStringLiteral("kind"), QStringLiteral("overlay")}, {QStringLiteral("pane"), pane->id()},
                              {QStringLiteral("spec"), QVariant::fromValue(*spec)},
                              {QStringLiteral("request_id"), QStringLiteral("afterglow.await:3:1")}};
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; read x")});
    ASSERT_NE(pane->overlay(), nullptr);
    // One overlay for each pane.
    QString refusal, code;
    runOverlay(window, pane, {QStringLiteral("/bin/true")}, {}, &refusal, &code);
    EXPECT_EQ(code, QStringLiteral("busy"));
    EXPECT_EQ(callbacks.applyOwned(QStringLiteral("a@1"), overlay, ignore).code, QStringLiteral("busy"));
    // No dialog while the overlay has the focus.
    const auto dialog = callbacks.applyOwned(QStringLiteral("a@1"), prompt, ignore);
    EXPECT_FALSE(dialog.error.isEmpty());
    EXPECT_EQ(dialog.code, QStringLiteral("busy"));
    EXPECT_FALSE(chrome->scriptDialog().value(QStringLiteral("open")).toBool());
    typeText(window, QStringLiteral("\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    // No overlay while a dialog is open.
    ASSERT_TRUE(window->showScriptDialog(QStringLiteral("a@1"), pane, prompt, [](const QString&, const QVariantMap&) {}).isEmpty());
    runOverlay(window, pane, {QStringLiteral("/bin/true")}, {}, &refusal, &code);
    EXPECT_EQ(code, QStringLiteral("busy"));
    const auto refused = callbacks.applyOwned(QStringLiteral("a@1"), overlay, ignore);
    EXPECT_FALSE(refused.error.isEmpty());
    EXPECT_EQ(refused.code, QStringLiteral("busy"));
    EXPECT_EQ(pane->overlay(), nullptr);
    window->closeRunWaits(QStringLiteral("a@1"), QStringLiteral("afterglow.await:2:"));
    // Nor while the command palette is open, where an action would answer
    // for it. The dialog is gone first: it alone would refuse.
    ASSERT_TRUE(waitFor([&] { return !chrome->scriptDialog().value(QStringLiteral("open")).toBool(); }));
    chrome->setPaletteOpen(true);
    runOverlay(window, pane, {QStringLiteral("/bin/true")}, {}, &refusal, &code);
    EXPECT_EQ(code, QStringLiteral("busy"));
    EXPECT_EQ(pane->overlay(), nullptr);
    chrome->setPaletteOpen(false);
}

TEST_F(NativeWindow, TheEndOfTheRunThatOpenedAnOverlayClosesIt) {
    // Only the overlays of the owner's run close: another run's stays.
    Pane* pane = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->splitPane(pane, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    const QStringList sleeper{QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("exec sleep 30")};
    auto mine = runOverlay(window, pane, sleeper, {}, nullptr, nullptr, QStringLiteral("a@1"), QStringLiteral("afterglow.await:31:1"));
    auto other = runOverlay(window, second, sleeper, {}, nullptr, nullptr, QStringLiteral("a@1"), QStringLiteral("afterglow.await:3:1"));
    ASSERT_NE(pane->overlay(), nullptr);
    ASSERT_NE(second->overlay(), nullptr);
    window->closeRunWaits(QStringLiteral("b@2"), QStringLiteral("afterglow.await:31:"));
    window->closeRunWaits(QStringLiteral("a@1"), QStringLiteral("afterglow.await:4:"));
    EXPECT_FALSE(mine->done || other->done);
    window->closeRunWaits(QStringLiteral("a@1"), QStringLiteral("afterglow.await:31:"));
    ASSERT_TRUE(mine->done);
    EXPECT_EQ(mine->code, QStringLiteral("cancelled"));
    EXPECT_EQ(pane->overlay(), nullptr);
    EXPECT_FALSE(other->done);
    // A retired script's overlays all close.
    window->clearScriptOwner(QStringLiteral("a@1"));
    ASSERT_TRUE(other->done);
    EXPECT_EQ(other->code, QStringLiteral("cancelled"));
    EXPECT_EQ(second->overlay(), nullptr);
    ASSERT_TRUE(overlaySettled(window, pane));
    ASSERT_TRUE(overlaySettled(window, second));
    expectIdle(window);
}

TEST_F(NativeWindow, CancellingTheCommandRunThatOpenedAnOverlayClosesItAndTheHandlerGetsCancelled) {
    ASSERT_TRUE(QDir().mkpath(directory.filePath(QStringLiteral("scripts"))));
    const QString pidFile = directory.filePath(QStringLiteral("picker.pid"));
    QFile script(directory.filePath(QStringLiteral("scripts/picker.luau")));
    ASSERT_TRUE(script.open(QIODevice::WriteOnly));
    script.write(QStringLiteral(R"(
return {
    metadata = { commands = { pick = { summary = "Pick", palette = "Pick a line" } } },
    run_command = function(command, ev)
        local _, err = afterglow.await(afterglow.overlay(ev.pane, {"/bin/sh", "-c", "echo $$ > '%1'; exec sleep 30"}))
        return afterglow.set_title(ev.pane, "after:" .. (err and err.code or "none"))
    end,
}
)").arg(pidFile).toUtf8());
    script.close();
    // The run's time limit cancels it: as a cancel, a client that went away
    // or the output limit, the run ends.
    restartApplication(QStringLiteral("picker.conf"),
        testSettings({{QStringLiteral("Scripts"), QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("commandTimeout"), 1000}}}}),
        {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    Pane* pane = window->focusedPane();
    // The palette's entry point.
    int id = -1;
    ASSERT_TRUE(waitFor([&] {
        for (const QVariant& entry : chrome->actions())
            if (entry.toMap().value(QStringLiteral("title")).toString() == QLatin1String("picker: Pick a line"))
                id = entry.toMap().value(QStringLiteral("id")).toInt();
        return id >= 0;
    }));
    chrome->runAction(id);
    // The overlay closes first, then the handler resumes with "cancelled".
    // The time limit runs from the start of the run: the listener connects
    // as soon as the overlay shows, before a wait that the limit can outlast.
    auto titleWhenClosed = std::make_shared<std::optional<QString>>();
    bool listening = false;
    ASSERT_TRUE(waitFor([&] {
        if (!listening && pane->overlay()) {
            QObject::connect(pane->overlay(), &PaneOverlay::ended, pane, [titleWhenClosed, pane] { *titleWhenClosed = pane->userTitle(); });
            listening = true;
        }
        return listening;
    }));
    // The file stays after its program has gone.
    ASSERT_TRUE(waitFor([&] { return QFileInfo(pidFile).size() > 0; }));
    QFile file(pidFile);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const pid_t pid = pid_t(file.readAll().trimmed().toInt());
    ASSERT_GT(pid, 0);
    ASSERT_TRUE(waitFor([&] { return pane->userTitle().startsWith(QLatin1String("after:")); }, 5000));
    ASSERT_TRUE(titleWhenClosed->has_value());
    EXPECT_FALSE((*titleWhenClosed)->startsWith(QLatin1String("after:")));
    EXPECT_EQ(pane->userTitle(), QStringLiteral("after:cancelled"));
    EXPECT_EQ(pane->overlay(), nullptr);
    EXPECT_TRUE(waitDeleting([&] {
        return ::kill(pid, 0) != 0;
    }));
    ASSERT_TRUE(overlaySettled(window, pane));
    expectIdle(window);
}

// An overlay with Shaders.overlayTransition set to @p transition, which
// draws no fade (as Shaders.tabTransition): it switches at once, in and out,
// and asks for no frames after.
void NativeWindow::overlaySwitchesAtOnce(const QString& transition) {
    restartApplication(QStringLiteral("no-fade.conf"),
        testSettings({{QStringLiteral("Shaders"), QJsonObject{{QStringLiteral("overlayTransition"), transition}}}}),
        {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                            QStringLiteral("stty -echo; printf 'shown-at-once' > /dev/tty; read x")});
    ASSERT_NE(pane->overlay(), nullptr);
    EXPECT_EQ(window->overlayFade(pane), std::make_pair(qreal(1), false));
    ASSERT_TRUE(waitFor([&] { return screenOf(pane->overlay()->pane()).contains(QStringLiteral("shown-at-once")); }));
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(window->overlayFade(pane), std::make_pair(qreal(1), false));
    expectIdle(window);
    typeText(window, QStringLiteral("\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_EQ(window->overlayFade(pane), std::make_pair(qreal(0), false));
    expectIdle(window);
}

TEST_F(NativeWindow, WithoutAnOverlayTransitionAnOverlaySwitchesAtOnce) {
    overlaySwitchesAtOnce(QString());
}

TEST_F(NativeWindow, AnOverlayTransitionThatDoesNotLoadSwitchesAtOnce) {
    overlaySwitchesAtOnce(QStringLiteral("afterglow-test-no-such-pack"));
}

// --- overlays: where their input, prompts and effects go ---


TEST_F(NativeWindow, AFileDroppedOnAnOverlayFocusesItsHostAndReachesItsProgram) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    auto answer = runOverlay(window, first, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                             QStringLiteral("stty -echo; read line; printf 'got:%s' \"$line\"")});
    ASSERT_NE(first->overlay(), nullptr);
    window->revealPane(second);
    ASSERT_EQ(window->focusedPane(), second);
    ASSERT_TRUE(waitFor([&] { return !window->accessiblePaneRect(first).isEmpty() && !window->accessiblePaneRect(second).isEmpty(); }));
    const QString path = directory.filePath(QStringLiteral("dropped.txt"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    const QPointF at = window->mapFromGlobal(QPointF(window->accessiblePaneRect(first).center()));
    QDropEvent drop(at, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier, QEvent::Drop);
    QCoreApplication::sendEvent(window, &drop);
    EXPECT_TRUE(drop.isAccepted());
    // The pane in the tab has the focus, not the overlay's own terminal,
    // which is in no tab.
    EXPECT_EQ(window->focusedPane(), first);
    typeText(window, QStringLiteral("\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_TRUE(answer->result.value(QStringLiteral("stdout")).toString().contains(QStringLiteral("dropped.txt")))
        << answer->result.value(QStringLiteral("stdout")).toString().toStdString();
    // With the overlay gone, the keys reach its host.
    typeText(window, QStringLiteral("zz\r"));
    EXPECT_TRUE(waitFor([&] { return screenOf(first).contains(QStringLiteral("zz")); })) << screenOf(first).toStdString();
    // A marker goes through the other pane's input after the keys: a key
    // that reached it shows before it.
    ASSERT_TRUE(second->sendInput("@marker@\n"));
    ASSERT_TRUE(waitFor([&] { return screenOf(second).contains(QStringLiteral("@marker@")); })) << screenOf(second).toStdString();
    EXPECT_FALSE(screenOf(second).contains(QStringLiteral("zz"))) << screenOf(second).toStdString();
}
TEST_F(NativeWindow, TextDroppedOnAnOverlayReachesItsProgram) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    auto answer = runOverlay(window, first, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                             QStringLiteral("stty -echo; read line; printf 'got:%s' \"$line\"")});
    ASSERT_NE(first->overlay(), nullptr);
    window->revealPane(second);
    ASSERT_EQ(window->focusedPane(), second);
    ASSERT_TRUE(waitFor([&] { return !window->accessiblePaneRect(first).isEmpty(); }));
    QMimeData mime;
    mime.setText(QStringLiteral("overlay words"));
    const QPointF at = window->mapFromGlobal(QPointF(window->accessiblePaneRect(first).center()));
    QDropEvent drop(at, Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier, QEvent::Drop);
    QCoreApplication::sendEvent(window, &drop);
    EXPECT_TRUE(drop.isAccepted());
    // The pane in the tab has the focus. Its overlay's program got the text.
    EXPECT_EQ(window->focusedPane(), first);
    typeText(window, QStringLiteral("\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_TRUE(answer->result.value(QStringLiteral("stdout")).toString().contains(QStringLiteral("got:overlay words")))
        << answer->result.value(QStringLiteral("stdout")).toString().toStdString();
}

TEST_F(NativeWindow, AConfirmedPasteReachesTheOverlaysProgram) {
    Pane* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                            QStringLiteral("stty -echo; read a; read b; printf '%s-%s' \"$a\" \"$b\"")});
    ASSERT_NE(pane->overlay(), nullptr);
    // Lines that a shell would run, to a program without bracketed paste:
    // the window asks first (Terminal.confirmUnsafePaste).
    QGuiApplication::clipboard()->setText(QStringLiteral("first\nsecond\n"));
    ASSERT_TRUE(waitFor([] { return QGuiApplication::clipboard()->text() == QStringLiteral("first\nsecond\n"); }));
    ASSERT_TRUE(window->runAction(pane, Action::Paste));
    ASSERT_TRUE(chrome->pasteConfirmOpen());
    chrome->answerPaste(true);
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_EQ(answer->result.value(QStringLiteral("stdout")).toString(), QStringLiteral("first-second"));
    // A marker goes through the host's input after the paste: a paste that
    // reached it shows before it.
    ASSERT_TRUE(pane->sendInput("@marker@\n"));
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("@marker@")); })) << screenOf(pane).toStdString();
    EXPECT_FALSE(screenOf(pane).contains(QStringLiteral("first"))) << screenOf(pane).toStdString();
}

TEST_F(NativeWindow, AButtonHeldWhenAnOverlayOpensReleasesOnItsPane) {
    // cat -v shows the reports that the pane's program gets.
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty raw -echo; printf ready; exec cat -v")};
    Pane* pane = window->splitPane(window->focusedPane(), SplitTree::Direction::Down, launch, nullptr);
    ASSERT_NE(pane, nullptr);
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("ready")); }));
    // Button-event tracking: drags report too. SGR coordinates.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?1002h\x1b[?1006h"); });
    ASSERT_EQ(window->focusedPane(), pane);
    ASSERT_TRUE(waitFor([&] { return !window->accessiblePaneRect(pane).isEmpty(); }));
    const QPointF at = window->mapFromGlobal(QPointF(window->accessiblePaneRect(pane).center()));
    pressLeft(window, at);
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("^[[<0;")); })) << screenOf(pane).toStdString();
    // The overlay opens while the button is down.
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; read x")});
    ASSERT_NE(pane->overlay(), nullptr);
    dragLeft(window, at + QPointF(40, 0));
    dragLeft(window, at + QPointF(80, 0));
    releaseLeft(window, at + QPointF(80, 0));
    // A marker goes through the same input after what the gesture sent.
    ASSERT_TRUE(pane->sendInput("@marker@"));
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("@marker@")); }));
    const QString screen = screenOf(pane);
    // The program that got the press hears its release, and no drag over
    // cells that are the overlay's now.
    EXPECT_TRUE(QRegularExpression(QStringLiteral("\\^\\[\\[<0;\\d+;\\d+m")).match(screen).hasMatch()) << screen.toStdString();
    EXPECT_EQ(screen.count(QStringLiteral("^[[<32;")), 0) << screen.toStdString();
    typeText(window, QStringLiteral("\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
}

TEST_F(NativeWindow, AnOverlaysClipboardPromptNamesTheTabOfItsHost) {
    Pane* first = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* background = window->openTab(launch, nullptr);
    ASSERT_NE(background, nullptr);
    auto answer = runOverlay(window, background, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(background->overlay(), nullptr);
    Pane* program = background->overlay()->pane();
    // The user goes back to the first tab, and the overlay goes on in the second.
    window->revealPane(first);
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    const quint32 tab = window->activeTabId();
    requestClipboardRead(program);
    ASSERT_TRUE(waitFor([&] { return chrome->clipboardPromptOpen(); }));
    EXPECT_TRUE(chrome->clipboardPromptText().contains(QStringLiteral("tab 2"))) << chrome->clipboardPromptText().toStdString();
    EXPECT_EQ(window->activeTabId(), tab);
    EXPECT_EQ(window->focusedPane(), first);
    chrome->answerClipboard(0);
    EXPECT_FALSE(answer->done);
}

TEST_F(NativeWindow, ClosingATabClosesThePromptOfItsOverlaysProgram) {
    // A close that does not ask: the prompt of the program is the question.
    restartApplication(QStringLiteral("close-tab.conf"),
        testSettings({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("confirmClose"), false}}}}), {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* closing = window->openTab(launch, nullptr);
    ASSERT_NE(closing, nullptr);
    auto answer = runOverlay(window, closing, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(closing->overlay(), nullptr);
    requestClipboardRead(closing->overlay()->pane());
    ASSERT_TRUE(waitFor([&] { return chrome->clipboardPromptOpen(); }));
    // The tab's close button: the panes go at their deferred delete, and
    // the prompt with the tab, at once.
    QPointer<Pane> gone(closing);
    chrome->closeTab(window->tabIndexOf(closing));
    ASSERT_EQ(window->tabs().size(), 1);
    EXPECT_FALSE(chrome->clipboardPromptOpen());
    EXPECT_TRUE(waitDeleting([&] {
        return gone.isNull() && answer->done;
    }));
    EXPECT_EQ(answer->code, QStringLiteral("cancelled"));
}

TEST_F(NativeWindow, AnOverlaysBellFlashesItsHost) {
    // A red flash at full strength that fades slowly and evenly: a
    // screenshot soon after the bell shows it.
    restartApplication(QStringLiteral("bell.conf"),
        testSettings({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("bell"), QStringLiteral("visual")}}},
                      {QStringLiteral("Animation"), QJsonObject{{QStringLiteral("bellCurve"), QStringLiteral("0,0,1,1")},
                                                                {QStringLiteral("bellDuration"), 3000}}},
                      {QStringLiteral("Colors"), QJsonObject{{QStringLiteral("visualBell"), QStringLiteral("#ff0000")},
                                                             {QStringLiteral("visualBellIntensity"), 1.0}}}}),
        {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    SettingsStore settings(directory.filePath(QStringLiteral("bell-capture.conf")),
                           QJsonObject{{QStringLiteral("Control"), QJsonObject{{QStringLiteral("allowReadText"), true}}}});
    Control control(settings, {});
    control.attachWindow(window);
    Pane* pane = window->focusedPane();
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(pane->overlay(), nullptr);
    Pane* program = pane->overlay()->pane();
    ASSERT_TRUE(overlaySettled(window, pane));
    expectIdle(window);
    const Shot before = captureShot(control, pane, CaptureScope::Window);
    ASSERT_FALSE(before.image.isNull()) << qPrintable(before.error);
    EXPECT_FALSE(reddish(pixelAt(before.image, 0.5, 0.5)));
    // The flash is drawn over the pane in the tab: it animates, for frames
    // of its own, and then stops. A bell's text changes nothing to draw.
    const auto frames = window->submittedFrames();
    program->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\a"); });
    EXPECT_TRUE(waitFor([&] { return window->submittedFrames() >= frames + 3; })) << window->submittedFrames() - frames;
    const Shot flashed = captureShot(control, pane, CaptureScope::Window);
    ASSERT_FALSE(flashed.image.isNull()) << qPrintable(flashed.error);
    EXPECT_TRUE(reddish(pixelAt(flashed.image, 0.5, 0.5))) << std::hex << pixelAt(flashed.image, 0.5, 0.5);
    expectIdle(window);
    EXPECT_FALSE(answer->done);
}

TEST_F(NativeWindow, AnOverlaysNotificationDoesNotShowWhileTheUserLooksAtIt) {
    RegisteredNotificationServer notices;
    ASSERT_TRUE(notices.registered);
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    window->revealPane(first);
    window->requestActivate();
    if (!waitFor([&] { return window->isActive(); }, 2000)) GTEST_SKIP() << "the compositor did not activate the window";
    auto answer = runOverlay(window, first, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(first->overlay(), nullptr);
    Pane* program = first->overlay()->pane();
    // The user looks at the overlay: nothing shows.
    feedFromReader(program, "\x1b]9;while looked at\x07");
    ASSERT_TRUE(settleQueued());
    // With the focus on the other pane, the next one shows. Notices of one
    // connection arrive in order, so the first would have come before it.
    window->revealPane(second);
    feedFromReader(program, "\x1b]9;while away\x07");
    ASSERT_TRUE(waitFor([&] { return notices.server.count >= 1; }));
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(notices.server.count, 1);
    // The notice that showed is the one sent while the user was away.
    EXPECT_EQ(notices.server.bodies, QStringList{QStringLiteral("while away")});
    EXPECT_FALSE(answer->done);
}

TEST_F(NativeWindow, AnOverlayKnowsWhetherItsTabShows) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* other = window->openTab(launch, nullptr);
    ASSERT_NE(other, nullptr);
    window->revealPane(first);
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    // The program asks for visibility reports (mode 2033), and cat -v shows them.
    auto answer = runOverlay(window, first, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                             QStringLiteral("stty raw -echo; printf '\\033[?2033h'; printf ready; exec cat -v")},
                             {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(first->overlay(), nullptr);
    Pane* program = first->overlay()->pane();
    ASSERT_TRUE(waitFor([&] { return screenOf(program).contains(QStringLiteral("ready")); }));
    const auto reports = [&](const char* state) { return screenOf(program).count(QLatin1String(state)); };
    // The mode answers at once that the terminal shows. cat echoes that
    // answer only once it runs, after "ready".
    ASSERT_TRUE(waitFor([&] { return reports("^[[?999;1n") == 1; })) << screenOf(program).toStdString();
    window->revealPane(other);
    EXPECT_TRUE(waitFor([&] { return reports("^[[?999;2n") == 1; })) << screenOf(program).toStdString();
    window->revealPane(first);
    EXPECT_TRUE(waitFor([&] { return reports("^[[?999;1n") == 2; })) << screenOf(program).toStdString();
    window->closeRunWaits(QStringLiteral("test@1"), QStringLiteral("afterglow.await:1:"));
    EXPECT_TRUE(answer->done);
}

TEST_F(NativeWindow, AnOverlayFadeThatNoFrameDrawsSwitchesAtOnce) {
    // A fade far longer than the test: one that ran would still run.
    restartApplication(QStringLiteral("long-fade.conf"),
        testSettings({{QStringLiteral("Animation"), QJsonObject{{QStringLiteral("overlayDuration"), 5000}}}}),
        {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    window->revealPane(first);
    expectIdle(window);
    const QStringList sleeper{QStringLiteral("/bin/sleep"), QStringLiteral("30")};
    const QVariantMap none{{QStringLiteral("capture"), QStringLiteral("none")}};

    // The overview has the tab's image: an overlay opens with no fade.
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    const auto opened = window->submittedFrames();
    ASSERT_TRUE(waitFor([&] { return window->submittedFrames() >= opened + 3; }));
    auto onCard = runOverlay(window, second, sleeper, none);
    ASSERT_NE(second->overlay(), nullptr);
    EXPECT_EQ(window->overlayFade(second), std::make_pair(qreal(1), false));
    EXPECT_FALSE(onCard->done);
    expectIdle(window);

    // A zoom of the other pane while the overlay fades in takes its host off
    // screen: no frame would draw the fade, which switches at the next frame.
    auto fading = runOverlay(window, first, sleeper, none, nullptr, nullptr, QStringLiteral("test@1"), QStringLiteral("afterglow.await:2:1"));
    ASSERT_NE(first->overlay(), nullptr);
    ASSERT_TRUE(window->overlayFade(first).second);
    window->revealPane(second);
    ASSERT_TRUE(window->runAction(second, Action::TogglePaneZoom));
    ASSERT_TRUE(waitFor([&] { return window->accessiblePaneRect(first).isEmpty(); }));
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(window->overlayFade(first), std::make_pair(qreal(1), false));
    EXPECT_FALSE(fading->done);
    expectIdle(window);
}

// The fade of @p pane's overlay after each frame that this renders and
// waits for, until the fade ends: one value for each such frame.
QList<qreal> fadeFrames(TerminalWindow* window, const Pane* pane) {
    QList<qreal> values;
    QElapsedTimer clock;
    clock.start();
    while (window->overlayFade(pane).second && clock.elapsed() < 5000) {
        if (!nextFrame(window)) break;
        values.append(window->overlayFade(pane).first);
    }
    return values;
}

TEST_F(NativeWindow, AnOverlayFadeWithASpringEndsTheFirstTimeItReachesItsEnd) {
    // An underdamped spring swings past its end and back.
    restartApplication(QStringLiteral("spring-fade.conf"),
        testSettings({{QStringLiteral("Animation"), QJsonObject{{QStringLiteral("overlayCurve"), QStringLiteral("spring:13,0.3")}}}}),
        {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; read x")});
    ASSERT_NE(pane->overlay(), nullptr);
    ASSERT_TRUE(window->overlayFade(pane).second);
    // The frame that reaches 1 is the last one of the fade-in: no frame
    // shows 1 and then less again.
    const QList<qreal> in = fadeFrames(window, pane);
    ASSERT_FALSE(in.isEmpty());
    EXPECT_FALSE(window->overlayFade(pane).second);
    EXPECT_EQ(in.last(), 1.0);
    for (qsizetype i = 0; i + 1 < in.size(); ++i) EXPECT_LT(in[i], 1.0) << "frame " << i << " of " << in.size();
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(window->overlayFade(pane), std::make_pair(qreal(1), false));
    // The fade-out the same, at 0.
    typeText(window, QStringLiteral("\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    ASSERT_TRUE(window->overlayFade(pane).second);
    const QList<qreal> out = fadeFrames(window, pane);
    ASSERT_FALSE(out.isEmpty());
    EXPECT_FALSE(window->overlayFade(pane).second);
    EXPECT_EQ(out.last(), 0.0);
    for (qsizetype i = 0; i + 1 < out.size(); ++i) EXPECT_GT(out[i], 0.0) << "frame " << i << " of " << out.size();
    expectIdle(window);
}

TEST_F(NativeWindow, AnOverlaysClipboardReadIsDeniedWhenItsTabGoesToAnotherWindow) {
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* moving = window->openTab(launch, nullptr);
    ASSERT_NE(moving, nullptr);
    // cat -v shows the answers that the program gets.
    auto answer = runOverlay(window, moving, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                              QStringLiteral("stty raw -echo; printf ready; exec cat -v")},
                             {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(moving->overlay(), nullptr);
    Pane* program = moving->overlay()->pane();
    ASSERT_TRUE(waitFor([&] { return screenOf(program).contains(QStringLiteral("ready")); }));
    requestClipboardRead(program);
    ASSERT_TRUE(waitFor([&] { return chrome->clipboardPromptOpen(); }));
    WindowOptions empty;
    empty.empty = true;
    auto* destination = application->newWindow(empty);
    ASSERT_NE(destination, nullptr);
    ASSERT_TRUE(window->detachTabTo(moving, destination).isEmpty());
    // The prompt of this window cannot answer for it there: the program
    // hears no (an OSC 52 answer with no data), not nothing.
    EXPECT_FALSE(chrome->clipboardPromptOpen());
    EXPECT_TRUE(waitFor([&] { return screenOf(program).contains(QStringLiteral("^[]52;")); })) << screenOf(program).toStdString();
    EXPECT_NE(moving->overlay(), nullptr);
    EXPECT_FALSE(answer->done);
}

TEST_F(NativeWindow, UnicodeInputOverAnOverlayEndsWithIt) {
    Pane* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    const auto unicodeOpen = [&] { return chrome->unicodeInput().value(QStringLiteral("active")).toBool(); };
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(pane->overlay(), nullptr);
    ASSERT_TRUE(window->runAction(pane, Action::UnicodeInput));
    ASSERT_TRUE(unicodeOpen());
    // U+0041 typed for the overlay, which ends before the Return.
    typeText(window, QStringLiteral("41"));
    window->closeRunWaits(QStringLiteral("test@1"), QStringLiteral("afterglow.await:1:"));
    ASSERT_TRUE(answer->done);
    EXPECT_FALSE(unicodeOpen());
    // The Return is the host's own key, and no "A" goes with it.
    sendKey(window, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    ASSERT_TRUE(pane->sendInput("marker\n"));
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("marker")); }));
    EXPECT_FALSE(screenOf(pane).contains(QLatin1Char('A'))) << screenOf(pane).toStdString();
}

TEST_F(NativeWindow, CancelOverlayPassesTheScriptsKeyModeAndIsInThePaletteOnlyOverAnOverlay) {
    Pane* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    const auto listed = [&] {
        for (const QVariant& entry : chrome->actions())
            if (entry.toMap().value(QStringLiteral("title")).toString() == Keybinds::title(Action::CancelOverlay)) return true;
        return false;
    };
    ASSERT_TRUE(window->runAction(pane, Action::CommandPalette));
    ASSERT_TRUE(chrome->paletteOpen());
    EXPECT_FALSE(listed());
    chrome->setPaletteOpen(false);
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}},
                             nullptr, nullptr, QStringLiteral("a@1"), QStringLiteral("afterglow.await:5:1"));
    ASSERT_NE(pane->overlay(), nullptr);
    ASSERT_TRUE(window->runAction(pane, Action::CommandPalette));
    ASSERT_TRUE(chrome->paletteOpen());
    EXPECT_TRUE(listed());
    chrome->setPaletteOpen(false);
    // The overlay's own script takes the keys over it. The user's way out
    // still works, and the script does not get the chord.
    ASSERT_TRUE(window->startScriptKeyMode(QStringLiteral("a@1"), pane, QStringLiteral("Picker keys")).isEmpty());
    int keys = 0;
    QObject listener;
    QObject::connect(window, &TerminalWindow::scriptKey, &listener, [&](const QString&, quint32, const QVariantMap&) { ++keys; });
    // Held: its repeats come after the overlay has gone, and are the
    // shortcut's too. They would reach the script as keys of its mode.
    holdKey(window, Qt::Key_Escape, Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier, {}, 3);
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_EQ(answer->code, QStringLiteral("cancelled"));
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(keys, 0);
    EXPECT_EQ(chrome->keyMode(), QStringLiteral("Picker keys"));
    window->endScriptKeyMode(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, AnOverlayWaitsForAMenuOrATitlePromptAndClosesASearch) {
    Pane* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    const QStringList quick{QStringLiteral("/bin/true")};
    QString refusal, code;
    // The user's answer to them must not go to the overlay.
    chrome->openContextMenu({10, 10}, {QVariantMap{{QStringLiteral("id"), 1}, {QStringLiteral("title"), QStringLiteral("Item")}}});
    ASSERT_TRUE(chrome->contextMenuOpen());
    runOverlay(window, pane, quick, {}, &refusal, &code);
    EXPECT_EQ(code, QStringLiteral("busy"));
    EXPECT_EQ(pane->overlay(), nullptr);
    chrome->closeContextMenu();
    chrome->askTitle(QStringLiteral("Tab title"), QStringLiteral("current"));
    ASSERT_TRUE(chrome->titlePromptOpen());
    runOverlay(window, pane, quick, {}, &refusal, &code);
    EXPECT_EQ(code, QStringLiteral("busy"));
    EXPECT_EQ(pane->overlay(), nullptr);
    chrome->cancelTitle();
    ASSERT_FALSE(chrome->titlePromptOpen());
    // A search is no question: it closes, and the overlay opens.
    chrome->setSearchOpen(true);
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}},
                             &refusal, &code);
    EXPECT_TRUE(refusal.isEmpty()) << refusal.toStdString();
    EXPECT_NE(pane->overlay(), nullptr);
    EXPECT_FALSE(chrome->searchOpen());
    EXPECT_FALSE(answer->done);
}

TEST_F(NativeWindow, AResultFileThatIsNoPlainFileGivesNoAnswer) {
    Pane* pane = window->focusedPane();
    // A FIFO would hold the GUI thread in its open until a writer came.
    auto fifo = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                          QStringLiteral("rm -f \"$AFTERGLOW_RESULT\" && mkfifo \"$AFTERGLOW_RESULT\"")},
                           {{QStringLiteral("capture"), QStringLiteral("file")}});
    ASSERT_TRUE(waitFor([&] { return fifo->done; }));
    EXPECT_EQ(fifo->result.value(QStringLiteral("exit_code")).toInt(), 0);
    EXPECT_TRUE(fifo->result.value(QStringLiteral("stdout")).toString().isEmpty());
    // A link would read another file of the user's.
    const QString secret = directory.filePath(QStringLiteral("secret"));
    QFile file(secret);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("secret");
    file.close();
    // The program tells the test where its result path is.
    const QString named = directory.filePath(QStringLiteral("result-path"));
    auto link = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                          QStringLiteral("printf %s \"$AFTERGLOW_RESULT\" > '%2' && "
                                                         "rm -f \"$AFTERGLOW_RESULT\" && ln -s '%1' \"$AFTERGLOW_RESULT\"").arg(secret, named)},
                           {{QStringLiteral("capture"), QStringLiteral("file")}});
    ASSERT_TRUE(waitFor([&] { return link->done; }));
    EXPECT_EQ(link->result.value(QStringLiteral("exit_code")).toInt(), 0);
    EXPECT_TRUE(link->result.value(QStringLiteral("stdout")).toString().isEmpty())
        << link->result.value(QStringLiteral("stdout")).toString().toStdString();
    // The overlay removes its result path when it goes: the link, not what
    // it names.
    const QString resultLink = QString::fromLocal8Bit(fileContent(named));
    ASSERT_FALSE(resultLink.isEmpty());
    EXPECT_TRUE(waitDeleting([&] { return !QFileInfo(resultLink).isSymLink() && !QFileInfo::exists(resultLink); }))
        << "the result link stays";
    EXPECT_EQ(fileContent(secret), QByteArray("secret"));
}

TEST_F(NativeWindow, AnOverlayStartsInADirectoryUnderHome) {
    // The test's directory is home for the window, which reads $HOME.
    const Saved home("HOME", QFile::encodeName(directory.path()));
    ASSERT_TRUE(QDir().mkpath(directory.filePath(QStringLiteral("inside"))));
    Pane* pane = window->focusedPane();
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("pwd -P")},
                             {{QStringLiteral("cwd"), QStringLiteral("~/inside")}});
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_EQ(answer->result.value(QStringLiteral("stdout")).toString(),
              QDir(directory.filePath(QStringLiteral("inside"))).canonicalPath() + QLatin1Char('\n'));
}

TEST_F(NativeWindow, AnOverlaysTitleNamesItsTabAndItsSplit) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    window->revealPane(first);
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    const QString title = QStringLiteral("Picker title");
    const auto labelled = [&] {
        const QVariantList labels = chrome->paneTitles();
        return std::any_of(labels.begin(), labels.end(), [&](const QVariant& label) {
            return label.toMap().value(QStringLiteral("title")).toString() == title;
        });
    };
    auto answer = runOverlay(window, first, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; read x")},
                             {{QStringLiteral("title"), title}});
    ASSERT_NE(first->overlay(), nullptr);
    EXPECT_TRUE(waitFor([&] { return chrome->tabTitles().value(0) == title && labelled(); }))
        << chrome->tabTitles().join(QLatin1Char('|')).toStdString();
    typeText(window, QStringLiteral("\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_TRUE(waitFor([&] { return chrome->tabTitles().value(0) != title && !labelled(); }))
        << chrome->tabTitles().join(QLatin1Char('|')).toStdString();
}

TEST_F(NativeWindow, ClosingAFocusedHostSendsItNoFocusAndLeavesNoFade) {
    Pane* first = window->focusedPane();
    // The host asks for focus reports (mode 1004), and keeps what it gets.
    const QString reports = directory.filePath(QStringLiteral("focus-reports"));
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                      QStringLiteral("stty raw -echo; printf '\\033[?1004h'; printf ready; exec cat -v > '%1'").arg(reports)};
    Pane* host = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(host, nullptr);
    ASSERT_EQ(window->focusedPane(), host);
    ASSERT_TRUE(waitFor([&] { return screenOf(host).contains(QStringLiteral("ready")); }));
    // Only a window with the focus sends focus reports.
    window->requestActivate();
    if (!waitFor([&] { return window->isActive(); }, 2000)) GTEST_SKIP() << "the compositor did not activate the window";
    auto answer = runOverlay(window, host, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(host->overlay(), nullptr);
    // The window tells the host that the overlay took the focus.
    ASSERT_TRUE(waitFor([&] { return fileContent(reports).contains("^[[O"); })) << fileContent(reports).toStdString();
    const qsizetype beforeClose = fileContent(reports).size();
    QPointer<Pane> gone(host);
    window->requestClosePane(host);
    ASSERT_TRUE(answer->done);
    EXPECT_EQ(answer->code, QStringLiteral("cancelled"));
    // The overlay goes in the closing effect of its host: no fade-out of its own.
    EXPECT_EQ(window->overlayFade(host), std::make_pair(qreal(0), false));
    // A report and the pane's input share the PTY's write queue, and the PTY
    // runs until the deferred delete: once a marker sent now is in the
    // file, a focus report sent at the close is there too.
    ASSERT_TRUE(host->sendInput("@marker@"));
    ASSERT_TRUE(waitFor([&] { return fileContent(reports).contains("@marker@"); })) << fileContent(reports).toStdString();
    // The window can have reported a focus-in before the overlay took
    // the focus. The overlay's focus-out is the last report: the close
    // added none, not even a focus-in that a focus-out followed.
    const QByteArray got = fileContent(reports);
    const QByteArray reported = got.left(got.indexOf("@marker@"));
    EXPECT_TRUE(reported.endsWith("^[[O")) << got.toStdString();
    EXPECT_EQ(reported.size(), beforeClose) << got.toStdString();
    ASSERT_TRUE(waitDeleted(gone));
    EXPECT_EQ(window->focusedPane(), first);
    expectIdle(window);
}

TEST_F(NativeWindow, ClosingAWindowAsksAboutTheProgramOfAnOverlay) {
    // An idle shell, which asks nothing on its own.
    restartApplication(QStringLiteral("close-asks.conf"),
        testSettings({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("shell"), QStringLiteral("/bin/sh")},
                                                               {QStringLiteral("confirmClose"), true}}}}),
        {});
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(pane->overlay(), nullptr);
    QPointer<TerminalWindow> alive = window;
    QCloseEvent close;
    QCoreApplication::sendEvent(window, &close);
    if (!alive) window = nullptr;
    ASSERT_TRUE(alive);
    EXPECT_FALSE(close.isAccepted());
    ASSERT_TRUE(chrome->confirmPromptOpen());
    EXPECT_TRUE(chrome->confirmPrompt().value(QStringLiteral("body")).toString().contains(QStringLiteral("sleep")))
        << chrome->confirmPrompt().value(QStringLiteral("body")).toString().toStdString();
    chrome->answerConfirm(false);
    EXPECT_TRUE(alive);
    EXPECT_NE(pane->overlay(), nullptr);
    EXPECT_FALSE(answer->done);
}

TEST_F(NativeWindow, AnotherScriptsKeyModeWaitsForAnOverlay) {
    Pane* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}},
                             nullptr, nullptr, QStringLiteral("a@1"), QStringLiteral("afterglow.await:6:1"));
    ASSERT_NE(pane->overlay(), nullptr);
    // Its keys would go to the other script, not to the overlay's program.
    EXPECT_EQ(window->startScriptKeyMode(QStringLiteral("b@2"), pane, QStringLiteral("Other keys")), QStringLiteral("An overlay has the keyboard"));
    EXPECT_TRUE(chrome->keyMode().isEmpty()) << chrome->keyMode().toStdString();
    // The overlay's own script can: it asked for both.
    EXPECT_TRUE(window->startScriptKeyMode(QStringLiteral("a@1"), pane, QStringLiteral("Own keys")).isEmpty());
    window->endScriptKeyMode(QStringLiteral("a@1"));
    EXPECT_FALSE(answer->done);
}

// A control socket of the test's own, on @p path, that acts on @p window,
// and one client of it, as afterglow ctl is. Its settings are the defaults
// of a new file, path.conf, which a test can write to. @p control overrides
// keys of the Control group, and wins over what the test writes to them.
class ControlConnection {
public:
    ControlConnection(const QString& path, TerminalWindow* window, const QJsonObject& control = {})
        : m_settings(path + QStringLiteral(".conf"), QJsonObject{{QStringLiteral("Control"), control}})
        , m_control(m_settings, {})
    {
        m_control.attachWindow(window);
        if (m_instance.listen(path) != InstanceSocket::Listen::Listening) return;
        m_socket = std::make_unique<ControlSocket>(m_control, m_settings, m_instance.router());
        client.connectToServer(path);
        waitFor([this] { return client.state() == QLocalSocket::ConnectedState; });
    }
    bool connected() const { return m_socket && client.state() == QLocalSocket::ConnectedState; }
    SettingsStore& settings() { return m_settings; }
    // The overlay call of afterglow ctl overlay: it answers when it ends.
    void overlay(quint32 pane, const QStringList& argv, const QJsonObject& options = {}) {
        call(QStringLiteral("overlay"), QJsonArray{double(pane), QJsonArray::fromStringList(argv), options});
    }
    // Sends the call @p fn of the control target, with @p args.
    void call(const QString& fn, const QJsonArray& args) {
        const QJsonObject frame{{QStringLiteral("type"), QStringLiteral("call")}, {QStringLiteral("id"), ++m_id},
                                {QStringLiteral("target"), QStringLiteral("control")}, {QStringLiteral("fn"), fn},
                                {QStringLiteral("args"), args}};
        client.write(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');
        client.flush();
    }
    // The next frame that the server sent, or none within @p ms.
    std::optional<QJsonObject> next(int ms = 3000) {
        if (!waitFor([this] { return client.canReadLine(); }, ms)) return std::nullopt;
        return QJsonDocument::fromJson(client.readLine()).object();
    }

private:
    SettingsStore m_settings;
    Control m_control;
    InstanceSocket m_instance;
    std::unique_ptr<ControlSocket> m_socket;
    qint64 m_id = 0;

public:
    // Goes first: its close reaches a server that is still there.
    QLocalSocket client;
};

TEST_F(NativeWindow, AControlClientThatGoesTakesItsOverlay) {
    ControlConnection ctl(directory.filePath(QStringLiteral("gone.sock")), window);
    ASSERT_TRUE(ctl.connected());
    Pane* pane = window->focusedPane();
    // One overlay that ends on its own, then another on the same connection.
    ctl.overlay(pane->id(), {QStringLiteral("/bin/true")});
    const auto first = ctl.next(5000);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->value(QStringLiteral("type")).toString(), QStringLiteral("reply"));
    ASSERT_TRUE(waitFor([&] { return pane->overlay() == nullptr; }));
    ctl.overlay(pane->id(), {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_TRUE(waitFor([&] { return pane->overlay() != nullptr; }));
    EXPECT_TRUE(pane->overlay()->owner().startsWith(QLatin1String("control:"))) << pane->overlay()->owner().toStdString();
    // Nobody waits for its answer now.
    ctl.client.disconnectFromServer();
    EXPECT_TRUE(waitFor([&] { return pane->overlay() == nullptr; }));
    ASSERT_TRUE(overlaySettled(window, pane));
    expectIdle(window);
}

TEST_F(NativeWindow, AControlClientGetsAnOverlaysOutputOnlyWithReadAccess) {
    Pane* pane = window->focusedPane();
    const QStringList print{QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("printf printed; exit 3")};
    for (const bool allowed : {false, true}) {
        SCOPED_TRACE(allowed ? "Control.allowReadText on" : "Control.allowReadText off");
        ControlConnection ctl(directory.filePath(allowed ? QStringLiteral("read.sock") : QStringLiteral("noread.sock")), window,
                              {{QStringLiteral("allowReadText"), allowed}});
        EXPECT_TRUE(ctl.connected());
        if (!ctl.connected()) continue;
        ctl.overlay(pane->id(), print);
        const auto frame = ctl.next(5000);
        EXPECT_TRUE(frame.has_value());
        if (!frame) continue;
        EXPECT_EQ(frame->value(QStringLiteral("type")).toString(), QStringLiteral("reply"));
        const QJsonObject result = frame->value(QStringLiteral("result")).toObject();
        EXPECT_EQ(result.value(QStringLiteral("exit_code")).toInt(), 3);
        EXPECT_TRUE(result.contains(QStringLiteral("truncated")));
        EXPECT_EQ(result.contains(QStringLiteral("stdout")), allowed);
        if (allowed) {
            EXPECT_EQ(result.value(QStringLiteral("stdout")).toString(), QStringLiteral("printed"));
        }
        EXPECT_TRUE(waitFor([&] { return pane->overlay() == nullptr; }));
    }
}

TEST_F(NativeWindow, AControlClientGetsAScreenshotAsAPngWhileReadAccessHolds) {
    // A tab switch longer than the bound on a frame (2 s): a screenshot waits
    // for its end all the same, and the wait is long enough to change a
    // setting in.
    restartApplication(QStringLiteral("long-switch.conf"),
        testSettings({{QStringLiteral("Animation"), QJsonObject{{QStringLiteral("tabCurve"), QStringLiteral("0.2,0,0,1")},
                                                                 {QStringLiteral("tabDuration"), 3000}}}}),
        {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    ControlConnection ctl(directory.filePath(QStringLiteral("shot.sock")), window);
    ASSERT_TRUE(ctl.connected());
    ASSERT_TRUE(ctl.settings().write(QStringLiteral("Control"), QStringLiteral("allowReadText"), true));
    Pane* pane = window->focusedPane();
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?25l\x1b[48;2;220;20;20m\x1b[2J"); });
    ASSERT_TRUE(nextFrame(window));
    ctl.call(QStringLiteral("screenshot"), {double(pane->id()), QStringLiteral("pane")});
    const auto reply = ctl.next(5000);
    ASSERT_TRUE(reply.has_value());
    ASSERT_EQ(reply->value(QStringLiteral("type")).toString(), QStringLiteral("reply")) << qPrintable(reply->value(QStringLiteral("message")).toString());
    const QJsonObject result = reply->value(QStringLiteral("result")).toObject();
    const QImage image = QImage::fromData(QByteArray::fromBase64(result.value(QStringLiteral("png")).toString().toLatin1()), "PNG");
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.size(), QSize(result.value(QStringLiteral("width")).toInt(), result.value(QStringLiteral("height")).toInt()));
    // An opaque window gives an opaque picture.
    EXPECT_FALSE(image.hasAlphaChannel());
    EXPECT_TRUE(reddish(pixelAt(image, 0.5, 0.5)));

    // During the switch: the answer comes when it ends, past 2 s.
    PaneLaunchOptions idle;
    idle.command = {QStringLiteral("/bin/cat")};
    Pane* other = window->openTab(idle, nullptr);
    ASSERT_NE(other, nullptr);
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }, 6000));
    window->revealPane(pane);
    ASSERT_TRUE(window->tabSwitching());
    QElapsedTimer waited;
    waited.start();
    ctl.call(QStringLiteral("screenshot"), {double(pane->id()), QStringLiteral("pane")});
    const auto switched = ctl.next(6000);
    ASSERT_TRUE(switched.has_value());
    EXPECT_EQ(switched->value(QStringLiteral("type")).toString(), QStringLiteral("reply"))
        << qPrintable(switched->value(QStringLiteral("message")).toString());
    EXPECT_GT(waited.elapsed(), 2000);
    EXPECT_FALSE(window->tabSwitching());

    // Read access that goes while the screenshot waits for its frame.
    window->revealPane(other);
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }, 6000));
    window->revealPane(pane);
    ASSERT_TRUE(window->tabSwitching());
    ctl.call(QStringLiteral("screenshot"), {double(pane->id()), QStringLiteral("pane")});
    // Its reply comes once the call before it has gone on to wait.
    ctl.call(QStringLiteral("focusedPane"), {});
    const auto focused = ctl.next();
    ASSERT_TRUE(focused.has_value());
    ASSERT_EQ(focused->value(QStringLiteral("type")).toString(), QStringLiteral("reply"));
    ASSERT_TRUE(window->tabSwitching()) << "the switch ended before the screenshot could wait for it";
    ASSERT_TRUE(ctl.settings().write(QStringLiteral("Control"), QStringLiteral("allowReadText"), false));
    const auto late = ctl.next(6000);
    ASSERT_TRUE(late.has_value());
    EXPECT_EQ(late->value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    EXPECT_EQ(late->value(QStringLiteral("code")).toString(), QStringLiteral("denied"));
    expectIdle(window);
}

TEST_F(NativeWindow, AControlClientsInputPartsJoinAsTheOverlaysStdin) {
    Pane* pane = window->focusedPane();
    ControlConnection ctl(directory.filePath(QStringLiteral("parts.sock")), window, {{QStringLiteral("allowReadText"), true}});
    ASSERT_TRUE(ctl.connected());
    // Two parts of `afterglow ctl overlay`'s standard input, the second
    // with a character of two UTF-8 bytes.
    const QStringList parts{QStringLiteral("first\n"), QString::fromUtf8("second \xc3\xa9\n")};
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const QJsonObject frame{{QStringLiteral("type"), QStringLiteral("call")}, {QStringLiteral("id"), 1000 + i},
                                {QStringLiteral("target"), QStringLiteral("control")}, {QStringLiteral("fn"), QStringLiteral("overlayInput")},
                                {QStringLiteral("args"), QJsonArray{QStringLiteral("list"), parts[i], i == parts.size() - 1}}};
        ctl.client.write(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');
        const auto answer = ctl.next();
        ASSERT_TRUE(answer.has_value());
        ASSERT_EQ(answer->value(QStringLiteral("result")), QJsonValue(true)) << QJsonDocument(*answer).toJson().toStdString();
    }
    ctl.overlay(pane->id(), {QStringLiteral("/bin/cat")}, {{QStringLiteral("input"), QStringLiteral("list")}});
    const auto frame = ctl.next(5000);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->value(QStringLiteral("type")).toString(), QStringLiteral("reply"));
    const QJsonObject result = frame->value(QStringLiteral("result")).toObject();
    EXPECT_EQ(result.value(QStringLiteral("exit_code")).toInt(-1), 0);
    EXPECT_EQ(result.value(QStringLiteral("stdout")).toString(), parts.join(QString()));
    EXPECT_TRUE(waitFor([&] { return pane->overlay() == nullptr; }));
    ASSERT_TRUE(overlaySettled(window, pane));
}

TEST_F(NativeWindow, TurningControlOffCancelsTheOverlaysOfItsClients) {
    ControlConnection ctl(directory.filePath(QStringLiteral("off.sock")), window);
    ASSERT_TRUE(ctl.connected());
    Pane* pane = window->focusedPane();
    ctl.overlay(pane->id(), {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_TRUE(waitFor([&] { return pane->overlay() != nullptr; }));
    // A script's overlay elsewhere is no client's: it stays.
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* other = window->splitPane(pane, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(other, nullptr);
    auto script = runOverlay(window, other, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(other->overlay(), nullptr);
    ASSERT_TRUE(ctl.settings().write(QStringLiteral("Control"), QStringLiteral("enabled"), false));
    EXPECT_TRUE(waitFor([&] { return pane->overlay() == nullptr; }));
    EXPECT_NE(other->overlay(), nullptr);
    EXPECT_FALSE(script->done);
}

TEST_F(NativeWindow, AFadeOutGoesOnWhileATabSwitchOrTheOverviewStillShowsItsHost) {
    // A fade that lasts longer than a tab switch or the overview's close.
    restartApplication(QStringLiteral("tab-fade.conf"),
        testSettings({{QStringLiteral("Animation"), QJsonObject{{QStringLiteral("overlayDuration"), 3000}}}}),
        {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    Pane* host = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* other = window->openTab(launch, nullptr);
    ASSERT_NE(other, nullptr);
    window->revealPane(host);
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    // Ends the overlay over the host once its fade-in is well on: the
    // fade-out then has seconds to run.
    const auto fadeOut = [&](const QString& run) {
        auto answer = runOverlay(window, host, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}},
                                 nullptr, nullptr, QStringLiteral("test@1"), run + QStringLiteral("1"));
        if (!host->overlay() || !waitFor([&] { return window->overlayFade(host).first >= 0.6; }, 4000)) return false;
        window->closeRunWaits(QStringLiteral("test@1"), run);
        return answer->done && window->overlayFade(host).second;
    };
    // The frames while @p showing holds: the fade runs in each, and falls.
    const auto followWhile = [&](const std::function<bool()>& showing) {
        QList<qreal> values;
        QElapsedTimer clock;
        clock.start();
        while (showing() && clock.elapsed() < 3000) {
            if (!nextFrame(window) || !showing()) break;
            const auto [progress, fading] = window->overlayFade(host);
            EXPECT_TRUE(fading) << "frame " << values.size();
            if (!values.isEmpty()) {
                EXPECT_LE(progress, values.last()) << "frame " << values.size();
            }
            values.append(progress);
        }
        return values;
    };

    // The switch draws the tab it leaves: the fade goes on there, and
    // switches once that tab no longer shows.
    ASSERT_TRUE(fadeOut(QStringLiteral("afterglow.await:7:")));
    window->revealPane(other);
    ASSERT_TRUE(window->tabSwitching());
    const QList<qreal> switching = followWhile([&] { return window->tabSwitching(); });
    ASSERT_FALSE(switching.isEmpty());
    EXPECT_LT(switching.last(), switching.first());
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(window->overlayFade(host), std::make_pair(qreal(0), false));
    expectIdle(window);

    // The overview draws the host's card, also while it closes onto the
    // other tab.
    window->revealPane(host);
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    ASSERT_TRUE(fadeOut(QStringLiteral("afterglow.await:8:")));
    ASSERT_TRUE(window->runAction(host, Action::PaneOverview));
    const auto opened = window->submittedFrames();
    ASSERT_TRUE(waitFor([&] { return window->submittedFrames() >= opened + 3; }));
    EXPECT_TRUE(window->overlayFade(host).second);
    ASSERT_TRUE(pressKey(window, Qt::Key_Right));
    ASSERT_EQ(window->overviewHighlight(), other);
    ASSERT_TRUE(pressKey(window, Qt::Key_Return));
    ASSERT_FALSE(window->overviewOpen());
    ASSERT_TRUE(nextFrame(window));
    EXPECT_TRUE(window->overlayFade(host).second);
    // The close takes far less than what is left of the fade: the fade
    // ends with it, from well above zero, and does not run out on its own.
    const QList<qreal> closing = followWhile([&] { return window->overlayFade(host).second; });
    ASSERT_FALSE(closing.isEmpty());
    EXPECT_GT(closing.last(), 0.1) << "the fade ran out after " << closing.size() << " frames";
    EXPECT_EQ(window->overlayFade(host), std::make_pair(qreal(0), false));
    EXPECT_EQ(window->focusedPane(), other);
    expectIdle(window);
}

TEST_F(NativeWindow, AFadeThatNoFrameAdvancesEndsAtOnceAlsoInTheTabASwitchLeaves) {
    // A fade far longer than the switch.
    restartApplication(QStringLiteral("hidden-fade.conf"),
        testSettings({{QStringLiteral("Animation"), QJsonObject{{QStringLiteral("overlayDuration"), 3000}}}}),
        {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    Pane* host = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* zoomed = window->splitPane(host, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(zoomed, nullptr);
    Pane* elsewhere = window->openTab(launch, nullptr);
    ASSERT_NE(elsewhere, nullptr);
    window->revealPane(host);
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    expectIdle(window);
    // With no frame between them: the overlay starts to fade in, a zoom of
    // the other pane hides its host, and a switch leaves the tab. The switch
    // draws that tab, zoomed: no frame advances the fade, which ends with
    // the first one, while the switch still runs.
    auto answer = runOverlay(window, host, {QStringLiteral("/bin/sleep"), QStringLiteral("30")}, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(host->overlay(), nullptr);
    ASSERT_TRUE(window->overlayFade(host).second);
    window->revealPane(zoomed);
    ASSERT_TRUE(window->runAction(zoomed, Action::TogglePaneZoom));
    window->revealPane(elsewhere);
    ASSERT_TRUE(window->tabSwitching());
    ASSERT_TRUE(nextFrame(window));
    ASSERT_TRUE(window->tabSwitching());
    EXPECT_EQ(window->overlayFade(host), std::make_pair(qreal(1), false));
    EXPECT_FALSE(answer->done);
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); }));
    expectIdle(window);
}

TEST_F(NativeWindow, APressWaitsForTheButtonsThatAnotherPanesProgramHolds) {
    // cat -v shows the reports that each program gets, in SGR coordinates.
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty raw -echo; printf ready; exec cat -v")};
    Pane* host = window->splitPane(window->focusedPane(), SplitTree::Direction::Down, launch, nullptr);
    ASSERT_NE(host, nullptr);
    ASSERT_TRUE(waitFor([&] { return screenOf(host).contains(QStringLiteral("ready")); }));
    host->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?1000h\x1b[?1006h"); });
    ASSERT_TRUE(waitFor([&] { return !window->accessiblePaneRect(host).isEmpty(); }));
    const QPointF at = window->mapFromGlobal(QPointF(window->accessiblePaneRect(host).center()));
    pressLeft(window, at);
    ASSERT_TRUE(waitFor([&] { return screenOf(host).contains(QStringLiteral("^[[<0;")); })) << screenOf(host).toStdString();
    auto answer = runOverlay(window, host, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                            QStringLiteral("stty raw -echo; printf '\\033[?1000h\\033[?1006h'; printf ready; exec cat -v")},
                             {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(host->overlay(), nullptr);
    Pane* program = host->overlay()->pane();
    ASSERT_TRUE(waitFor([&] { return screenOf(program).contains(QStringLiteral("ready")); }));
    // A right click on the overlay while the left button that the host's
    // program got is down: its release would go to the host.
    mouseAt(window, QEvent::MouseButtonPress, at, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
    mouseAt(window, QEvent::MouseButtonRelease, at, Qt::RightButton, Qt::LeftButton);
    releaseLeft(window, at);
    // Markers go through the same inputs after what the gesture sent.
    const auto marked = [](Pane* pane) {
        const qsizetype before = screenOf(pane).count(QStringLiteral("@marker@"));
        if (!pane->sendInput("@marker@")) return false;
        return waitFor([pane, before] { return screenOf(pane).count(QStringLiteral("@marker@")) == before + 1; });
    };
    ASSERT_TRUE(marked(host));
    ASSERT_TRUE(marked(program));
    const auto count = [](Pane* pane, const char* pattern) {
        return screenOf(pane).count(QRegularExpression(QLatin1String(pattern)));
    };
    // The host's program: its press and its release, once each, and no
    // button of the overlay's.
    EXPECT_EQ(count(host, "\\^\\[\\[<0;\\d+;\\d+M"), 1) << screenOf(host).toStdString();
    EXPECT_EQ(count(host, "\\^\\[\\[<0;\\d+;\\d+m"), 1) << screenOf(host).toStdString();
    EXPECT_EQ(count(host, "\\^\\[\\[<2;"), 0) << screenOf(host).toStdString();
    // The overlay's program: nothing of that click.
    EXPECT_EQ(count(program, "\\^\\[\\[<"), 0) << screenOf(program).toStdString();
    // With every button up, a click reaches it whole.
    clickRight(window, at);
    EXPECT_TRUE(waitFor([&] { return count(program, "\\^\\[\\[<2;\\d+;\\d+M") == 1 && count(program, "\\^\\[\\[<2;\\d+;\\d+m") == 1; }))
        << screenOf(program).toStdString();
    ASSERT_TRUE(marked(host));
    EXPECT_EQ(count(host, "\\^\\[\\[<2;"), 0) << screenOf(host).toStdString();
    window->closeRunWaits(QStringLiteral("test@1"), QStringLiteral("afterglow.await:1:"));
    EXPECT_TRUE(answer->done);
}

OverlaySpec popupSpec(const QVariantMap& options) {
    QString error;
    const auto spec = OverlaySpec::read({QStringLiteral("x")}, options, &error);
    EXPECT_TRUE(spec) << error.toStdString();
    return spec.value_or(OverlaySpec{});
}
// A program for a popup that reports clicks (SGR) into @p log, and says
// "popup-ready" once it does.
QStringList reportingClicks(const QString& log) {
    return {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
            QStringLiteral("stty raw -echo; printf '\\033[?1000h\\033[?1006hpopup-ready' > /dev/tty; exec cat > '%1'").arg(log)};
}

TEST_F(NativeWindow, APopupOverOnePaneTakesThePointerInItsOwnCellsOnly) {
    Pane* pane = window->focusedPane();
    ASSERT_NE(pane, nullptr);
    // History in the pane, which a wheel that reached it would scroll.
    pane->withTerminal([](AfterglowVt::Terminal& t) {
        for (int i = 0; i < 200; ++i) t.feed("history\r\n");
    });
    const QString log = directory.filePath(QStringLiteral("mouse.log"));
    const QVariantMap options{{QStringLiteral("width"), QStringLiteral("50%")}, {QStringLiteral("height"), QStringLiteral("50%")},
                              {QStringLiteral("title"), QStringLiteral("Popup")}, {QStringLiteral("capture"), QStringLiteral("none")}};
    auto answer = runOverlay(window, pane, reportingClicks(log), options);
    ASSERT_NE(pane->overlay(), nullptr);
    Pane* program = pane->overlay()->pane();
    // Half of the pane, in its middle, with a border.
    const OverlaySpec::Placement placed = popupSpec(options).place(pane->cols(), pane->rows());
    EXPECT_TRUE(placed.border);
    EXPECT_TRUE(pane->overlay()->popup());
    EXPECT_EQ(pane->overlay()->placement(), placed);
    EXPECT_EQ(program->cols(), placed.grid.width());
    EXPECT_EQ(program->rows(), placed.grid.height());
    ASSERT_TRUE(waitFor([&] { return screenOf(program).contains(QStringLiteral("popup-ready")); }));
    EXPECT_EQ(window->focusedPane(), pane);
    EXPECT_EQ(window->title(), QStringLiteral("Popup"));
    ASSERT_TRUE(overlaySettled(window, pane));
    expectIdle(window);
    const QRectF host = gridOf(window, pane), grid = gridOf(window, program);
    const QSizeF cell(grid.width() / program->cols(), grid.height() / program->rows());
    // Around it, on its border too, the pointer reaches no program: the
    // pane neither selects nor scrolls, and the popup stays open.
    for (const QPointF& at : {host.topLeft() + QPointF(3, 3), host.bottomRight() - QPointF(3, 3),
                              QPointF(grid.left() - cell.width() / 2, grid.center().y()),
                              QPointF(grid.center().x(), grid.top() - cell.height() / 2)}) {
        clickLeft(window, at);
        wheelAt(window, at);
    }
    // A drag across the pane around it.
    mouseAt(window, QEvent::MouseButtonPress, host.topLeft() + QPointF(3, 3), Qt::LeftButton, Qt::LeftButton);
    dragLeft(window, host.topLeft() + QPointF(host.width() / 3, 3));
    mouseAt(window, QEvent::MouseButtonRelease, host.topLeft() + QPointF(host.width() / 3, 3), Qt::LeftButton, Qt::NoButton);
    // Any scroll that the wheel started has eased to its end, and frames stop.
    expectIdle(window);
    EXPECT_FALSE(answer->done);
    EXPECT_FALSE(pane->hasSelection());
    EXPECT_FALSE(program->hasSelection());
    EXPECT_EQ(viewportOffset(pane), 0u);
    EXPECT_EQ(window->focusedPane(), pane);
    // In it, its own cells: column 3, row 2 from 1. The reports arrive in
    // order, so one from outside would come first.
    clickLeft(window, grid.topLeft() + QPointF(cell.width() * 2.5, cell.height() * 1.5));
    const QByteArray expected("\x1b[<0;3;2M\x1b[<0;3;2m");
    EXPECT_TRUE(waitFor([&] { return fileContent(log) == expected; })) << fileContent(log).toStdString();
    // It closes; the pane has the keys again, and frames stop.
    window->closeOverlays(QStringLiteral("test@1"), QString(), QStringLiteral("done"));
    ASSERT_TRUE(answer->done);
    EXPECT_EQ(answer->code, QStringLiteral("cancelled"));
    EXPECT_EQ(pane->overlay(), nullptr);
    EXPECT_EQ(window->focusedPane(), pane);
    ASSERT_TRUE(overlaySettled(window, pane));
    expectIdle(window);
    typeText(window, QStringLiteral("zz\r"));
    EXPECT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("zz")); })) << screenOf(pane).toStdString();
}

TEST_F(NativeWindow, APopupOverOneOfTwoPanesIsPlacedAgainByEachLayout) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(window->focusedPane(), second);
    const QVariantMap options{{QStringLiteral("width"), 30}, {QStringLiteral("height"), QStringLiteral("50%")},
                              {QStringLiteral("position"), QStringLiteral("bottom-left")}};
    auto answer = runOverlay(window, first, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; read x; stty size")},
                             options);
    ASSERT_NE(first->overlay(), nullptr);
    Pane* program = first->overlay()->pane();
    const OverlaySpec spec = popupSpec(options);
    EXPECT_EQ(window->focusedPane(), first);
    EXPECT_EQ(first->overlay()->placement(), spec.place(first->cols(), first->rows()));
    // 30 cells, its border included, in a pane that has them.
    ASSERT_GE(first->cols(), 30);
    EXPECT_EQ(program->cols(), 28);
    // A click on the other pane takes the focus there; one on the pane around
    // the popup brings it back, and reaches nothing. The chrome scene, made
    // in the background, takes the pointer first once it is there.
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(waitFor([&] { return layer->rootItem() != nullptr; }));
    ASSERT_TRUE(nextFrame(window));
    clickLeft(window, gridOf(window, second).center());
    EXPECT_EQ(window->focusedPane(), second) << gridOf(window, second).x() << "," << gridOf(window, second).y() << " "
                                             << gridOf(window, second).width() << "x" << gridOf(window, second).height();
    const QRectF host = gridOf(window, first);
    clickLeft(window, host.topRight() + QPointF(-3, 3));
    EXPECT_EQ(window->focusedPane(), first);
    EXPECT_FALSE(answer->done);
    // A split of the host: the popup is placed in its new grid, and its
    // program hears of its new size.
    const int rows = first->rows();
    ASSERT_NE(window->splitPane(first, SplitTree::Direction::Down, launch, nullptr), nullptr);
    ASSERT_TRUE(waitFor([&] { return first->rows() < rows; }));
    const OverlaySpec::Placement placed = spec.place(first->cols(), first->rows());
    EXPECT_EQ(first->overlay()->placement(), placed);
    EXPECT_EQ(program->cols(), placed.grid.width());
    EXPECT_EQ(program->rows(), placed.grid.height());
    window->revealPane(first);
    typeText(window, QStringLiteral("\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    EXPECT_EQ(answer->result.value(QStringLiteral("stdout")).toString(),
              QStringLiteral("%1 %2\n").arg(placed.grid.height()).arg(placed.grid.width()));
    EXPECT_EQ(window->focusedPane(), first);
    ASSERT_TRUE(overlaySettled(window, first));
    expectIdle(window);
}

TEST_F(NativeWindow, APopupTooBigForItsPaneFillsTheGridBesideThePanesDock) {
    Pane* pane = window->focusedPane();
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), dockEffect(QStringLiteral("bar"), QStringLiteral("top"), 1,
        {dockRun(QStringLiteral("dock"), QStringLiteral("d"))}, QStringLiteral("pane")), pane).isEmpty());
    // The next layout gives it its row, and the pane its grid under it.
    ASSERT_TRUE(waitFor([&] { return shownDock(window, "bar", pane->id()) != nullptr; }));
    const QString log = directory.filePath(QStringLiteral("mouse.log"));
    // Bigger than the pane, and a border alone: each fills the pane's grid,
    // its border on the pane's edge cells. A failed check ends its own
    // case only.
    const auto fills = [&](const QVariantMap& options) {
        QFile::remove(log);
        // A case that fails part way leaves no overlay to the next one.
        const auto close = qScopeGuard([&] {
            if (pane->overlay()) window->closeOverlays(QStringLiteral("test@1"), QString(), QStringLiteral("done"));
        });
        auto answer = runOverlay(window, pane, reportingClicks(log), options);
        ASSERT_NE(pane->overlay(), nullptr);
        Pane* program = pane->overlay()->pane();
        EXPECT_TRUE(pane->overlay()->popup());
        EXPECT_EQ(pane->overlay()->placement(), (OverlaySpec::Placement{QRect(1, 1, pane->cols() - 2, pane->rows() - 2), true}));
        EXPECT_EQ(program->cols(), pane->cols() - 2);
        EXPECT_EQ(program->rows(), pane->rows() - 2);
        ASSERT_TRUE(waitFor([&] { return screenOf(program).contains(QStringLiteral("popup-ready")); }));
        ASSERT_TRUE(overlaySettled(window, pane));
        // The dock and the popup's border reach no program; the popup's
        // first cell is its program's 1;1, one cell in from the pane's grid,
        // which starts under the dock.
        const DockArea* dock = shownDock(window, "bar", pane->id());
        ASSERT_NE(dock, nullptr);
        const QRectF host = gridOf(window, pane);
        const QSizeF cell(host.width() / pane->cols(), host.height() / pane->rows());
        clickLeft(window, QRectF(dock->rect()).center() / window->devicePixelRatio());
        clickLeft(window, host.topLeft() + QPointF(cell.width() / 2, cell.height() / 2));
        clickLeft(window, host.topLeft() + QPointF(cell.width() * 1.5, cell.height() * 1.5));
        const QByteArray expected("\x1b[<0;1;1M\x1b[<0;1;1m");
        EXPECT_TRUE(waitFor([&] { return fileContent(log) == expected; })) << fileContent(log).toStdString();
        window->closeOverlays(QStringLiteral("test@1"), QString(), QStringLiteral("done"));
        ASSERT_TRUE(answer->done);
        ASSERT_TRUE(overlaySettled(window, pane));
    };
    for (const QVariantMap& options : {QVariantMap{{QStringLiteral("width"), 500}, {QStringLiteral("height"), 500}},
                                       QVariantMap{{QStringLiteral("border"), true}}}) {
        SCOPED_TRACE(QJsonDocument(QJsonObject::fromVariantMap(options)).toJson(QJsonDocument::Compact).toStdString());
        fills(options);
    }
    expectIdle(window);
}

TEST_F(NativeWindow, ADropReachesAPopupInItsOwnCellsOnly) {
    Pane* pane = window->focusedPane();
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                                            QStringLiteral("stty -echo; read line; printf 'got:%s' \"$line\"")},
                             {{QStringLiteral("width"), QStringLiteral("50%")}, {QStringLiteral("height"), QStringLiteral("50%")}});
    ASSERT_NE(pane->overlay(), nullptr);
    Pane* program = pane->overlay()->pane();
    ASSERT_TRUE(waitFor([&] { return !window->accessiblePaneRect(program).isEmpty(); }));
    const QString path = directory.filePath(QStringLiteral("dropped.txt"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    // Beside the popup: no program takes it.
    QDropEvent outside(gridOf(window, pane).topLeft() + QPointF(3, 3), Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier,
                       QEvent::Drop);
    QCoreApplication::sendEvent(window, &outside);
    EXPECT_FALSE(outside.isAccepted());
    // In it: its program gets the path.
    QDropEvent inside(gridOf(window, program).center(), Qt::CopyAction, &mime, Qt::NoButton, Qt::NoModifier, QEvent::Drop);
    QCoreApplication::sendEvent(window, &inside);
    EXPECT_TRUE(inside.isAccepted());
    typeText(window, QStringLiteral("\r"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }));
    const QString got = answer->result.value(QStringLiteral("stdout")).toString();
    EXPECT_TRUE(got.startsWith(QStringLiteral("got:")) && got.contains(QStringLiteral("dropped.txt"))) << got.toStdString();
    ASSERT_TRUE(overlaySettled(window, pane));
    expectIdle(window);
}

TEST_F(NativeWindow, ClosingTheHostOfAPopupCancelsItAndFramesStop) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    auto answer = runOverlay(window, second, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("exec sleep 30")},
                             {{QStringLiteral("capture"), QStringLiteral("none")}, {QStringLiteral("width"), QStringLiteral("50%")},
                              {QStringLiteral("title"), QStringLiteral("Gone")}});
    ASSERT_NE(second->overlay(), nullptr);
    ASSERT_TRUE(second->overlay()->popup());
    ASSERT_TRUE(overlaySettled(window, second));
    window->requestClosePane(second);
    ASSERT_TRUE(answer->done);
    EXPECT_EQ(answer->code, QStringLiteral("cancelled"));
    EXPECT_EQ(window->focusedPane(), first);
    EXPECT_EQ(window->panes().size(), 1);
    expectIdle(window);
}

// --- window and pane lifetimes, events and input ---

TEST_F(NativeWindow, ClosingTheLastTabFreesItsPanesOutOfAnyTab) {
    // The last tab's panes go with deleteLater before their window does.
    // While each one is freed, the window lists no pane of that tab: the
    // handlers of its destroyed signal (Control's paneClosed, the refresh of
    // a remote socket) walk the window's panes. Each pane is announced once,
    // with its tab.
    restartApplication(QStringLiteral("last-tab.conf"),
        testSettings({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("confirmClose"), false}}}}), {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    const quint32 tab = window->tabIdOf(first);
    const quint32 firstId = first->id(), secondId = second->id();
    QPointer<TerminalWindow> closing = window;
    QList<qsizetype> listed;
    QHash<quint32, QList<quint32>> announced;
    QObject listener;
    QObject::connect(window, &TerminalWindow::paneClosing, &listener,
                     [&](quint32 pane, quint32 inTab, quint32) { announced[pane] << inTab; });
    for (Pane* pane : {first, second})
        QObject::connect(pane, &QObject::destroyed, &listener, [&] { if (closing) listed << closing->panes().size(); });
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    chrome->closeTab(0);
    ASSERT_TRUE(waitDeleted(closing));
    window = nullptr;
    EXPECT_EQ(listed, (QList<qsizetype>{0, 0}));
    EXPECT_EQ(announced.value(firstId), QList<quint32>{tab});
    EXPECT_EQ(announced.value(secondId), QList<quint32>{tab});
}

TEST_F(NativeWindow, APaneAloneInItsTabIsAnnouncedOnceWhenItsProgramEnds) {
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("read line")};
    Pane* other = window->openTab(launch, nullptr);
    ASSERT_NE(other, nullptr);
    ASSERT_TRUE(settleQueued());
    const quint32 tab = window->tabIdOf(other), id = other->id();
    QList<quint32> announced;
    QObject listener;
    QObject::connect(window, &TerminalWindow::paneClosing, &listener, [&](quint32 pane, quint32 inTab, quint32) {
        if (pane == id) announced << inTab;
    });
    ASSERT_TRUE(other->sendInput("\n"));
    ASSERT_TRUE(waitFor([&] { return window->tabs().size() == 1; }));
    EXPECT_EQ(announced, QList<quint32>{tab});
}

TEST_F(NativeWindow, AWindowDeletedWhileItShowsReachesNoSlotOfItsOwn) {
    // ~QWindow hides a window that still shows, and its visibilityChanged
    // reached updateAudioCapture after the window's members were gone. Only
    // a build with AddressSanitizer (the debug preset) can show that read of
    // freed memory: elsewhere this test mostly passes without the fix.
    WindowOptions options;
    options.command = {QStringLiteral("/bin/cat")};
    auto* shown = application->newWindow(options);
    ASSERT_NE(shown, nullptr);
    shown->resize(400, 300);
    shown->show();
    ASSERT_TRUE(waitFor([&] { return shown->isExposed(); }));
    bool hidden = false;
    QObject listener;
    QObject::connect(shown, &QWindow::visibilityChanged, &listener, [&](QWindow::Visibility visibility) {
        hidden = hidden || visibility == QWindow::Hidden;
    });
    delete shown;
    // The signal that reached the window's slot came, to this one too.
    EXPECT_TRUE(hidden);
    drainDeferredDeletes();
}

TEST_F(NativeWindow, TerminalEventsReachTheGuiThreadInOrderAndBounded) {
    // No desktop notifications: the window takes the events and shows none.
    restartApplication(QStringLiteral("events.conf"),
        testSettings({{QStringLiteral("Notifications"), QJsonObject{{QStringLiteral("enabled"), false}}}}), {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    QStringList bodies;
    bool offThread = false;
    QObject listener;
    QObject::connect(pane, &Pane::notificationRequested, &listener, [&](const QString&, const QString& body) {
        offThread = offThread || QThread::currentThread() != QCoreApplication::instance()->thread();
        bodies << body;
    }, Qt::DirectConnection);
    // More notifications in one read than the pane queues: the rest go.
    std::string burst;
    for (int i = 0; i < 300; ++i) burst += "\x1b]9;n" + std::to_string(i) + "\x07";
    feedFromReader(pane, burst);
    ASSERT_TRUE(settleQueued());
    EXPECT_FALSE(offThread);
    ASSERT_EQ(bodies.size(), 256);
    EXPECT_EQ(bodies.front(), QStringLiteral("n0"));
    EXPECT_EQ(bodies.back(), QStringLiteral("n255"));
    // Once the window took them, the next one goes through.
    feedFromReader(pane, "\x1b]9;later\x07");
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(bodies.back(), QStringLiteral("later"));
    EXPECT_FALSE(offThread);
}

TEST_F(NativeWindow, ThePaletteClosesWhenTheFocusMovesToAnotherPane) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(window->runAction(second, Action::CommandPalette));
    ASSERT_TRUE(chrome->paletteOpen());
    // A script or a control client moves the focus: an action chosen now
    // would act on a pane that the palette did not open on.
    window->revealPane(first);
    EXPECT_FALSE(chrome->paletteOpen());
}

TEST_F(NativeWindow, APointerOverAViewScrolledByPartOfARowIsOnTheRowDrawnUnderIt) {
    Pane* pane = window->focusedPane();
    pane->withTerminal([](AfterglowVt::Terminal& t) {
        std::string text;
        for (int i = 0; i < 200; ++i) text += "row " + std::to_string(i) + " https://example.com/" + std::to_string(i) + "\r\n";
        t.feed(text);
    });
    expectIdle(window);
    ASSERT_FALSE(window->accessiblePaneRect(pane).isEmpty());
    // Where the rows are while the view is at the bottom.
    const QRect top = window->accessibleCellRect(pane, pane->selectionPointAt(0, 0));
    const QRect tenth = window->accessibleCellRect(pane, pane->selectionPointAt(0, 10));
    const double row = (tenth.y() - top.y()) / 10.0;
    ASSERT_GT(row, 4.0);
    // A trackpad scrolls by pixels: about half a row back. Qt marks its
    // events as synthesized by the system, which a wheel's are not.
    const QPointF at = window->mapFromGlobal(window->accessiblePaneRect(pane).center());
    QWheelEvent wheel(at, window->mapToGlobal(at), QPoint(0, int(row / 2)), QPoint(0, 0), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false, Qt::MouseEventSynthesizedBySystem);
    QCoreApplication::sendEvent(window, &wheel);
    ASSERT_TRUE(nextFrame(window));
    ASSERT_EQ(viewportOffset(pane), 1u);
    // The terminal shows a whole row back, drawn up by the rest of a row: a
    // point three quarters of a row below the top is on the second row drawn.
    const auto cell = window->accessibleCellAt(pane, top.topLeft() + QPoint(1, int(row * 0.75)));
    ASSERT_TRUE(cell.has_value());
    EXPECT_TRUE(*cell == pane->selectionPointAt(0, 1)) << cell->row << " " << pane->selectionPointAt(0, 1).row;
    // The hint labels sit on the rows as drawn: the first row's is above
    // the top of the grid by the shift.
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(window->runAction(pane, Action::UrlsOpen));
    ASSERT_TRUE(waitFor([&] { return !chrome->urlHints().isEmpty(); }));
    double highest = std::numeric_limits<double>::max();
    for (const QVariant& hint : chrome->urlHints()) highest = std::min(highest, hint.toMap().value(QStringLiteral("y")).toDouble());
    const double gridTop = window->mapFromGlobal(top.topLeft()).y();
    EXPECT_LT(highest, gridTop - row * 0.3) << highest << " " << gridTop;
    EXPECT_GT(highest, gridTop - row * 0.7) << highest << " " << gridTop;
}

TEST_F(NativeWindow, HintsThatOpenWhileTheViewEasesSitOnTheRowsWhereItStops) {
    // A scroll that eases for long: the hints open while it moves.
    restartApplication(QStringLiteral("scroll.conf"),
        testSettings({{QStringLiteral("Animation"), QJsonObject{{QStringLiteral("scroll"), true}, {QStringLiteral("scrollDuration"), 5000}}}}),
        {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    pane->withTerminal([](AfterglowVt::Terminal& t) {
        std::string text;
        for (int i = 0; i < 200; ++i) text += "row " + std::to_string(i) + " https://example.com/" + std::to_string(i) + "\r\n";
        t.feed(text);
    });
    expectIdle(window);
    const double gridTop = window->mapFromGlobal(window->accessibleCellRect(pane, pane->selectionPointAt(0, 0)).topLeft()).y();
    const auto page = std::size_t(std::max(1, pane->rows() - 1));
    ASSERT_TRUE(window->runAction(pane, Action::ScrollPageUp));
    ASSERT_TRUE(nextFrame(window));
    ASSERT_LT(viewportOffset(pane), page);
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(window->runAction(pane, Action::UrlsCopy));
    // The view stops a page back at once, and the labels sit on its rows:
    // the highest on the top row.
    EXPECT_EQ(viewportOffset(pane), page);
    ASSERT_FALSE(chrome->urlHints().isEmpty());
    QVariantMap highest;
    for (const QVariant& hint : chrome->urlHints())
        if (highest.isEmpty() || hint.toMap().value(QStringLiteral("y")).toDouble() < highest.value(QStringLiteral("y")).toDouble())
            highest = hint.toMap();
    EXPECT_NEAR(highest.value(QStringLiteral("y")).toDouble(), gridTop, 1.0);
    const QString top = screenOf(pane).section(QLatin1Char('\n'), 0, 0);
    const QString url = top.section(QLatin1Char(' '), 2, 2);
    ASSERT_TRUE(url.startsWith(QLatin1String("https://"))) << top.toStdString();
    // Frames go on, and the view stays under the labels.
    ASSERT_TRUE(nextFrame(window));
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(viewportOffset(pane), page);
    // The label of the top row copies that row's URL.
    for (const QChar letter : highest.value(QStringLiteral("label")).toString())
        sendKey(window, Qt::Key_A + (letter.toLower().unicode() - u'a'), Qt::NoModifier, QString(letter));
    EXPECT_TRUE(waitFor([&] { return QGuiApplication::clipboard()->text() == url; }))
        << QGuiApplication::clipboard()->text().toStdString() << " " << url.toStdString();
}

TEST_F(NativeWindow, LinkLabelsOnAHeldFrameStayUntilTheHoldEnds) {
    Pane* pane = window->focusedPane();
    const auto generation = [pane] { return pane->withTerminal([](AfterglowVt::Terminal& t) { return t.generation(); }); };
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[H\x1b[2Jhttps://example.com/drawn\r\n"); });
    ASSERT_TRUE(nextFrame(window));
    ASSERT_EQ(pane->snapshot().generation, generation());
    // A program begins an update (DEC 2026) and writes into it: the pane
    // keeps drawing the frame before it.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?2026h\x1b[H\x1b[2Jhttps://example.com/next\r\n"); });
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(window->runAction(pane, Action::UrlsCopy));
    ASSERT_EQ(chrome->urlHints().size(), 1);
    ASSERT_NE(pane->snapshot().generation, generation());
    // The labels sit on the text drawn: a letter takes its link, and does
    // not close the labels because the terminal went on behind the frame.
    const QString label = chrome->urlHints().front().toMap().value(QStringLiteral("label")).toString();
    for (const QChar letter : label)
        sendKey(window, Qt::Key_A + (letter.toLower().unicode() - u'a'), Qt::NoModifier, QString(letter));
    EXPECT_TRUE(waitFor([&] { return QGuiApplication::clipboard()->text() == QLatin1String("https://example.com/drawn"); }))
        << QGuiApplication::clipboard()->text().toStdString();
    EXPECT_TRUE(chrome->urlHints().isEmpty());
    // The update ends, and the next one begins.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?2026l"); });
    ASSERT_TRUE(nextFrame(window));
    ASSERT_EQ(pane->snapshot().generation, generation());
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?2026h\x1b[H\x1b[2Jhttps://example.com/last\r\n"); });
    ASSERT_TRUE(window->runAction(pane, Action::UrlsCopy));
    ASSERT_EQ(chrome->urlHints().size(), 1);
    // Labels found on a held frame close with the first frame taken after the
    // hold, before the chrome draws them over its text.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\x1b[?2026l"); });
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(pane->snapshot().generation, generation());
    EXPECT_TRUE(chrome->urlHints().isEmpty());
}

TEST_F(NativeWindow, ASplitWhoseShellDoesNotStartKeepsTheZoom) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(window->runAction(second, Action::TogglePaneZoom));
    ASSERT_TRUE(waitFor([&] { return window->accessiblePaneRect(first).isEmpty() && !window->accessiblePaneRect(second).isEmpty(); }));
    PaneLaunchOptions failing;
    failing.command = {QStringLiteral("/nonexistent/afterglow-test-program")};
    QString error;
    EXPECT_EQ(window->splitPane(second, SplitTree::Direction::Down, failing, &error), nullptr);
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(window->accessiblePaneRect(first).isEmpty());
    EXPECT_FALSE(window->accessiblePaneRect(second).isEmpty());
}

TEST_F(NativeWindow, TheKeyThatClosesAHeldPaneDoesNotRepeatIntoTheNextPane) {
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("read line")};
    Pane* held = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(held, nullptr);
    held->setHold(true);
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(held->sendInput("\n"));
    ASSERT_TRUE(waitFor([&] { return held->isHeld(); }));
    window->revealPane(held);
    QPointer<Pane> gone(held);
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    QCoreApplication::sendEvent(window, &down);
    ASSERT_EQ(window->focusedPane(), first);
    // The key is held: its repeats, then its release.
    for (int i = 0; i < 3; ++i) {
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"), true);
        QCoreApplication::sendEvent(window, &up);
        QKeyEvent again(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"), true);
        QCoreApplication::sendEvent(window, &again);
    }
    QKeyEvent up(QEvent::KeyRelease, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    QCoreApplication::sendEvent(window, &up);
    sendKey(window, Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
    sendKey(window, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
    EXPECT_TRUE(waitFor([&] { return screenOf(first).contains(QLatin1Char('y')); })) << screenOf(first).toStdString();
    EXPECT_FALSE(screenOf(first).contains(QLatin1Char('q'))) << screenOf(first).toStdString();
    EXPECT_TRUE(waitDeleted(gone));
}

// The two dialogs that ask over a pane's window: the file transfer prompt
// and the Save dialog of a download. A dialog that a test left unanswered,
// also one that the platform hid, is refused at the end: it would ask over
// the next test's window, and a Save dialog keeps the one download that
// Pane::downloadFile takes at a time. A dialog that closed has no
// WA_DeleteOnClose any more (QWidgetPrivate::handleClose).
struct PaneDialogs {
    QPointer<QMessageBox> prompt;
    QPointer<QFileDialog> save;
    QString sent; ///< the directory that a Yes to the prompt makes
    PaneDialogs() = default;
    PaneDialogs(const PaneDialogs&) = delete;
    PaneDialogs& operator=(const PaneDialogs&) = delete;
    ~PaneDialogs() {
        if (prompt && prompt->testAttribute(Qt::WA_DeleteOnClose)) prompt->reject();
        if (save && save->testAttribute(Qt::WA_DeleteOnClose)) save->reject();
        drainDeferredDeletes();
    }
};
// The program of @p asking asks to send the directory @p sent and sends a
// download: both dialogs open. An approval makes the directory at once, so
// its absence shows that nothing ran before the answer.
void openPaneDialogs(Pane* asking, const QString& sent, PaneDialogs& dialogs) {
    dialogs.sent = sent;
    // A session of its own each time: the sessions here stay open, and the id
    // of an open session is taken.
    static int sessions = 0;
    const QByteArray id = "dialogs" + QByteArray::number(++sessions);
    asking->fileTransfers()->handle("ac=send;id=" + id);
    asking->fileTransfers()->handle("ac=file;id=" + id + ";fid=f;ft=directory;n=" + sent.toUtf8().toBase64());
    feedFromReader(asking, "\x1b]1337;File=name=" + QByteArray("notes.txt").toBase64().toStdString() + ";size=5;inline=0:"
                               + QByteArray("hello").toBase64().toStdString() + "\x07");
    dialogs.prompt = waitForDialog<QMessageBox>(QStringLiteral("File transfer"));
    dialogs.save = waitForDialog<QFileDialog>(QStringLiteral("Save file received from"));
    // A save that should not happen would land beside the directory.
    if (dialogs.save) dialogs.save->setDirectory(QFileInfo(sent).absolutePath());
}
// A window on the screen with a title that starts with @p title, over
// @p window: what the user sees of a dialog. A native dialog (Plasma's file
// dialog) is a window of its own, which the dialog object only stands for.
bool shownOver(const QString& title, QWindow* window) {
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->isVisible() && !widget->testAttribute(Qt::WA_DontShowOnScreen) && widget->windowTitle().startsWith(title)
            && widget->windowHandle() && widget->windowHandle()->transientParent() == window)
            return true;
    return false;
}
// Both dialogs still ask, over @p window.
void expectDialogsOver(const PaneDialogs& dialogs, QWindow* window) {
    ASSERT_TRUE(dialogs.prompt) << "the prompt was refused";
    ASSERT_TRUE(dialogs.save) << "the Save dialog closed";
    EXPECT_TRUE(dialogs.prompt->isVisible());
    EXPECT_TRUE(dialogs.save->isVisible());
    EXPECT_TRUE(shownOver(QStringLiteral("File transfer"), window));
    // The GTK file dialog and the desktop portal's are no windows of Qt:
    // ANativeGtkSaveDialogKeepsItsNameWhenItsWindowCloses tests the GTK one.
    const QByteArray theme = qgetenv("QT_QPA_PLATFORMTHEME");
    if (theme != "gtk3" && theme != "xdgdesktopportal") {
        EXPECT_TRUE(shownOver(QStringLiteral("Save file received from"), window));
    }
}
// The prompt's Yes still reaches the transfer.
void expectPromptAnswers(PaneDialogs& dialogs) {
    ASSERT_TRUE(dialogs.prompt);
    EXPECT_FALSE(QFileInfo::exists(dialogs.sent));
    dialogs.prompt->button(QMessageBox::Yes)->click();
    EXPECT_TRUE(waitFor([&] { return QFileInfo(dialogs.sent).isDir(); }));
}

TEST_F(NativeWindow, APanesDialogsMoveWithItAndOutliveTheWindowItLeft) {
    // The pane moves to another window, and the window it left closes
    // later: its dialogs then ask over the pane's new window. Qt's Wayland
    // platform hides the transient children of a window whose surface it
    // destroys.
    WindowOptions options; options.command = {QStringLiteral("/bin/cat")};
    QPointer<TerminalWindow> source = application->newWindow(options); ASSERT_TRUE(source);
    source->show();
    auto* destination = application->newWindow(options); ASSERT_NE(destination, nullptr);
    destination->show();
    ASSERT_TRUE(waitFor([&] { return source->isExposed() && destination->isExposed(); }));
    QPointer<Pane> pane = source->focusedPane(); ASSERT_TRUE(pane);
    PaneLaunchOptions launch; launch.command = {QStringLiteral("/bin/cat")};
    QPointer<Pane> stays = source->splitPane(pane, SplitTree::Direction::Right, launch, nullptr); ASSERT_TRUE(stays);
    PaneDialogs dialogs;
    openPaneDialogs(pane, directory.filePath(QStringLiteral("sent-moved")), dialogs);
    ASSERT_TRUE(dialogs.prompt && dialogs.save);
    EXPECT_EQ(dialogs.prompt->windowHandle()->transientParent(), source.data());
    EXPECT_EQ(dialogs.save->windowHandle()->transientParent(), source.data());
    ASSERT_TRUE(source->movePaneTo(pane, destination, destination->focusedPane(), SplitTree::Direction::Right).isEmpty());
    ASSERT_TRUE(source);
    // The dialogs stay over the window that shows while it is there
    // (keepDialogOverPane).
    ASSERT_NO_FATAL_FAILURE(expectDialogsOver(dialogs, source));
    // The pane that stayed ends: its window closes.
    ASSERT_TRUE(stays->sendInput("\x04"));
    ASSERT_TRUE(waitDeleted(source));
    ASSERT_TRUE(settleQueued());
    ASSERT_NO_FATAL_FAILURE(expectDialogsOver(dialogs, destination));
    ASSERT_NO_FATAL_FAILURE(expectPromptAnswers(dialogs));
}

TEST_F(NativeWindow, APanesDialogsFollowItToANewWindow) {
    // Moved alone or with its tab, the only pane of a window goes to a new
    // window, which shows only after the window it left closed. A failed
    // check ends its own case only.
    const auto follows = [&](Action action) {
        WindowOptions options; options.command = {QStringLiteral("/bin/cat")};
        QPointer<TerminalWindow> source = application->newWindow(options); ASSERT_TRUE(source);
        QPointer<TerminalWindow> moved;
        // A case that fails part way leaves no window, and no dialog, to the
        // next one.
        const auto close = qScopeGuard([&] {
            delete moved.data();
            delete source.data();
            settleQueued();
        });
        source->show();
        ASSERT_TRUE(waitFor([&] { return source->isExposed(); }));
        QPointer<Pane> pane = source->focusedPane(); ASSERT_TRUE(pane);
        PaneDialogs dialogs;
        openPaneDialogs(pane, directory.filePath(QStringLiteral("sent-%1").arg(int(action))), dialogs);
        ASSERT_TRUE(dialogs.prompt && dialogs.save);
        ASSERT_TRUE(source->runAction(pane, action));
        ASSERT_TRUE(waitDeleting([&] {
            moved = pane ? qobject_cast<TerminalWindow*>(pane->parent()) : nullptr;
            return moved && moved != source && source.isNull();
        }));
        ASSERT_TRUE(waitFor([&] { return moved && moved->isExposed(); }));
        ASSERT_TRUE(settleQueued());
        ASSERT_NO_FATAL_FAILURE(expectDialogsOver(dialogs, moved));
        ASSERT_NO_FATAL_FAILURE(expectPromptAnswers(dialogs));
        // Its last pane closes, and the window with it.
        ASSERT_TRUE(moved && pane);
        moved->requestClosePane(pane);
        ASSERT_TRUE(waitDeleted(moved));
    };
    for (const Action action : {Action::MovePaneToWindow, Action::DetachTab}) {
        SCOPED_TRACE(Keybinds::title(action).toStdString());
        follows(action);
    }
}

TEST_F(NativeWindow, AnOverlaysDialogsMoveWithItsHost) {
    // The dialogs of an overlay's program ask over its host's window, and
    // go with the host.
    WindowOptions options; options.command = {QStringLiteral("/bin/cat")};
    QPointer<TerminalWindow> source = application->newWindow(options); ASSERT_TRUE(source);
    source->show();
    auto* destination = application->newWindow(options); ASSERT_NE(destination, nullptr);
    destination->show();
    ASSERT_TRUE(waitFor([&] { return source->isExposed() && destination->isExposed(); }));
    QPointer<Pane> host = source->focusedPane(); ASSERT_TRUE(host);
    PaneLaunchOptions launch; launch.command = {QStringLiteral("/bin/cat")};
    QPointer<Pane> stays = source->splitPane(host, SplitTree::Direction::Right, launch, nullptr); ASSERT_TRUE(stays);
    auto answer = runOverlay(source, host, {QStringLiteral("/bin/cat")}, {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(host->overlay(), nullptr);
    PaneDialogs dialogs;
    openPaneDialogs(host->overlay()->pane(), directory.filePath(QStringLiteral("sent-from-overlay")), dialogs);
    ASSERT_TRUE(dialogs.prompt && dialogs.save);
    EXPECT_EQ(dialogs.prompt->windowHandle()->transientParent(), source.data());
    EXPECT_EQ(dialogs.save->windowHandle()->transientParent(), source.data());
    ASSERT_TRUE(source->movePaneTo(host, destination, destination->focusedPane(), SplitTree::Direction::Right).isEmpty());
    ASSERT_TRUE(source);
    ASSERT_NO_FATAL_FAILURE(expectDialogsOver(dialogs, source));
    ASSERT_TRUE(stays->sendInput("\x04"));
    ASSERT_TRUE(waitDeleted(source));
    ASSERT_TRUE(settleQueued());
    ASSERT_NO_FATAL_FAILURE(expectDialogsOver(dialogs, destination));
    ASSERT_NO_FATAL_FAILURE(expectPromptAnswers(dialogs));
    ASSERT_TRUE(host);
    if (host->overlay()) {
        EXPECT_TRUE(host->overlay()->pane()->sendInput("\x04"));
    }
}

TEST_F(NativeWindow, APanesDialogsCloseWithTheWindowItIsIn) {
    // Nobody is left to answer: the transfer is refused, and the download
    // is not saved.
    restartApplication(QStringLiteral("close-dialogs.conf"),
        testSettings({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("confirmClose"), false}}}}), {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    PaneDialogs dialogs;
    openPaneDialogs(window->focusedPane(), directory.filePath(QStringLiteral("sent-closed")), dialogs);
    ASSERT_TRUE(dialogs.prompt && dialogs.save);
    QPointer<TerminalWindow> closing = window;
    window->close();
    ASSERT_TRUE(waitDeleted(closing));
    window = nullptr;
    ASSERT_TRUE(waitDeleting([&] { return dialogs.prompt.isNull() && dialogs.save.isNull(); }));
    ASSERT_TRUE(settleQueued());
    EXPECT_FALSE(QFileInfo::exists(dialogs.sent));
    EXPECT_FALSE(QFileInfo::exists(QFileInfo(dialogs.sent).absolutePath() + QStringLiteral("/notes.txt")));
}

TEST_F(NativeWindow, TheQuickTerminalSlidesOutForItsPanesDialogs) {
    // A layer surface is no parent for a dialog: the dialog would open
    // under the quick terminal, modal to it. The quick terminal slides out
    // until its last dialog closes, and then slides in again.
    WindowOptions options; options.command = {QStringLiteral("/bin/cat")}; options.quick = true;
    QPointer<TerminalWindow> quick = application->newWindow(options); ASSERT_TRUE(quick);
    quick->toggleQuick();
    ASSERT_TRUE(waitFor([&] { return quick && quick->isExposed(); }));
    PaneDialogs dialogs;
    openPaneDialogs(quick->focusedPane(), directory.filePath(QStringLiteral("sent-from-quick")), dialogs);
    ASSERT_TRUE(dialogs.prompt && dialogs.save);
    // The quick terminal slides out in a queued call.
    ASSERT_TRUE(settleQueued());
    EXPECT_FALSE(quick->quickShown());
    EXPECT_EQ(dialogs.prompt->windowHandle()->transientParent(), nullptr);
    EXPECT_EQ(dialogs.save->windowHandle()->transientParent(), nullptr);
    // One dialog left: the quick terminal stays out.
    dialogs.save->reject();
    ASSERT_TRUE(settleQueued());
    EXPECT_FALSE(quick->quickShown());
    ASSERT_NO_FATAL_FAILURE(expectPromptAnswers(dialogs));
    EXPECT_TRUE(waitFor([&] { return quick && quick->quickShown() && quick->isExposed(); }));
    // The rejected Save dialog goes, and with it the one download that
    // Pane::downloadFile takes at a time.
    drainDeferredDeletes();
    // The user brings it back over two dialogs: an answer to one of them
    // does not slide it out again.
    PaneDialogs again;
    openPaneDialogs(quick->focusedPane(), directory.filePath(QStringLiteral("sent-again")), again);
    ASSERT_TRUE(again.prompt && again.save);
    ASSERT_TRUE(waitFor([&] { return !quick->quickShown(); }));
    quick->toggleQuick();
    again.save->reject();
    ASSERT_TRUE(settleQueued());
    EXPECT_TRUE(quick->quickShown());
}

TEST_F(NativeWindow, TheQuickTerminalSlidesInWhenThePaneOfItsDialogCloses) {
    // The pane's dialogs go with it: the quick terminal slid out for them
    // and comes back.
    WindowOptions options; options.command = {QStringLiteral("/bin/cat")}; options.quick = true;
    QPointer<TerminalWindow> quick = application->newWindow(options); ASSERT_TRUE(quick);
    quick->toggleQuick();
    ASSERT_TRUE(waitFor([&] { return quick && quick->isExposed(); }));
    PaneLaunchOptions launch; launch.command = {QStringLiteral("/bin/cat")};
    QPointer<Pane> closing = quick->splitPane(quick->focusedPane(), SplitTree::Direction::Right, launch, nullptr);
    ASSERT_TRUE(closing);
    PaneDialogs dialogs;
    openPaneDialogs(closing, directory.filePath(QStringLiteral("sent-closing")), dialogs);
    ASSERT_TRUE(dialogs.prompt && dialogs.save);
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(quick && closing);
    EXPECT_FALSE(quick->quickShown());
    quick->requestClosePane(closing);
    ASSERT_TRUE(waitDeleted(closing));
    EXPECT_TRUE(waitDeleting([&] { return quick && quick->quickShown(); }));
    EXPECT_FALSE(dialogs.prompt);
    EXPECT_FALSE(dialogs.save);
}

TEST_F(NativeWindow, ARequestFromAClosedPaneOpensNoDialog) {
    // A transfer request, or a clipboard write that asks, that the pane
    // sent before it closed: nobody is left to answer it.
    restartApplication(QStringLiteral("closed-requests.conf"),
        testSettings({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("clipboardWrite"), QStringLiteral("ask")}}}}),
        {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    PaneLaunchOptions launch; launch.command = {QStringLiteral("/bin/cat")};
    Pane* first = window->focusedPane();
    QPointer<Pane> closing = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr); ASSERT_TRUE(closing);
    auto* chrome = window->findChild<ChromeController*>(); ASSERT_NE(chrome, nullptr);
    const QString sent = directory.filePath(QStringLiteral("sent-after-close"));
    closing->fileTransfers()->handle("ac=send;id=closed");
    closing->fileTransfers()->handle("ac=file;id=closed;fid=f;ft=directory;n=" + sent.toUtf8().toBase64());
    feedFromReader(closing, "\x1b]52;c;" + QByteArray("written").toBase64().toStdString() + "\x07");
    // Closed before the requests reach the window: the pane is deleted
    // later, so they still find it.
    window->requestClosePane(closing);
    ASSERT_TRUE(closing);
    // The transfer's worker thread posts its request to the window, and the
    // clipboard write comes through the pane's queue: both take a few
    // milliseconds. A second is the bound for a dialog that would show.
    EXPECT_TRUE(waitForDialog<QMessageBox>(QStringLiteral("File transfer"), 1000).isNull());
    EXPECT_FALSE(chrome->clipboardPromptOpen());
    EXPECT_FALSE(QFileInfo::exists(sent));
    // The other pane still asks.
    feedFromReader(first, "\x1b]52;c;" + QByteArray("written").toBase64().toStdString() + "\x07");
    EXPECT_TRUE(waitFor([&] { return chrome->clipboardPromptOpen(); }));
    chrome->answerClipboard(0);
}

TEST_F(NativeWindow, ADownloadKeepsTheCharactersOfItsNameOutsideTheBmp) {
    // Two UTF-16 units each, and neither one printable alone.
    Pane* pane = window->focusedPane();
    const QString name = QStringLiteral("\U0001F4F7 photo \U00020000.txt");
    feedFromReader(pane, "\x1b]1337;File=name=" + name.toUtf8().toBase64().toStdString() + ";size=5;inline=0:"
                             + QByteArray("hello").toBase64().toStdString() + "\x07");
    QPointer<QFileDialog> save = waitForDialog<QFileDialog>(QStringLiteral("Save file received from"));
    ASSERT_TRUE(save);
    const auto refuse = qScopeGuard([&save] {
        if (save && save->testAttribute(Qt::WA_DeleteOnClose)) save->reject();
        drainDeferredDeletes();
    });
    ASSERT_EQ(save->selectedFiles().size(), 1);
    EXPECT_EQ(QFileInfo(save->selectedFiles().front()).fileName(), name);
}

TEST_F(NativeWindow, ADownloadsNameFitsAFileName) {
    // NAME_MAX is 255 bytes: a long name is cut short of it in UTF-8,
    // keeps its extension, and keeps whole characters (an accent stays
    // with its letter).
    Pane* pane = window->focusedPane();
    const QString accented = QStringLiteral("e\u0301");
    const std::pair<QString, QString> names[] = {
        {QString(300, QChar(0x00E9)) + QStringLiteral(".txt"), QString(98, QChar(0x00E9)) + QStringLiteral(".txt")},
        {accented.repeated(100) + QStringLiteral(".txt"), accented.repeated(65) + QStringLiteral(".txt")},
    };
    for (const auto& [name, expected] : names) {
        SCOPED_TRACE(expected.toStdString());
        feedFromReader(pane, "\x1b]1337;File=name=" + name.toUtf8().toBase64().toStdString() + ";size=5;inline=0:"
                                 + QByteArray("hello").toBase64().toStdString() + "\x07");
        QPointer<QFileDialog> save = waitForDialog<QFileDialog>(QStringLiteral("Save file received from"));
        EXPECT_TRUE(save);
        if (!save) continue;
        const auto refuse = qScopeGuard([&save] {
            if (save && save->testAttribute(Qt::WA_DeleteOnClose)) save->reject();
            // The dialog goes, and with it the one download at a time.
            drainDeferredDeletes();
        });
        EXPECT_EQ(save->selectedFiles().size(), 1);
        if (save->selectedFiles().size() != 1) continue;
        EXPECT_TRUE(QFileInfo(save->selectedFiles().front()).fileName() == expected)
            << QFileInfo(save->selectedFiles().front()).fileName().toStdString();
    }
}

TEST_F(NativeWindow, TheScrollBarAtTheBottomShowsTheLiveScreen) {
    // The bottom of the scroll bar is 1 - size. For some history sizes its
    // product with the row count falls just short of the live view's top
    // row in floating point: truncated, it left the view one row up.
    Pane* pane = window->focusedPane();
    const auto [history, cursor] = pane->withTerminal([](AfterglowVt::Terminal& t) {
        return std::pair{t.screen().historySize(), t.screen().cursor.y};
    });
    const int rows = pane->rows();
    std::size_t target = history + 1;
    const auto shortOfTop = [rows](std::size_t h) {
        const double total = double(std::size_t(rows) + h);
        return (1.0 - double(rows) / total) * total < double(h);
    };
    while (target < history + 100000 && !shortOfTop(target)) ++target;
    ASSERT_TRUE(shortOfTop(target));
    feedFromReader(pane, std::string(std::size_t(rows - 1 - cursor) + (target - history), '\n'));
    ASSERT_EQ(historySize(pane), target);
    pane->scrollHistory(0.0);
    const double size = pane->historyPosition().value(QStringLiteral("size")).toDouble();
    pane->scrollHistory(1.0 - size);
    EXPECT_TRUE(pane->withTerminal([](AfterglowVt::Terminal& t) { return t.screen().viewportTop() == t.screen().historySize(); }));
}

TEST_F(NativeWindow, AHeldFrameIsDrawnWhenItsHoldTimesOutAndNotBefore) {
    // A program that begins a synchronized update (mode 2026) and goes
    // quiet holds its pane's frame for a second. The window looks again
    // then, and draws no frames before.
    Pane* pane = window->focusedPane();
    expectIdle(window);
    const auto before = window->submittedFrames();
    QElapsedTimer held; held.start();
    // Through the program, as the PTY reader tells the window of output.
    ASSERT_TRUE(pane->sendInput("\x1b[?2026hheld\n"));
    drain(700);
    const auto during = window->submittedFrames();
    EXPECT_LE(during - before, 1u) << "frames while the frame was held";
    EXPECT_TRUE(waitFor([&] { return window->submittedFrames() > during; }, 2000));
    EXPECT_GE(held.elapsed(), 900);
    ASSERT_TRUE(pane->sendInput("\x1b[?2026l\n"));
    expectIdle(window);
}

TEST_F(NativeWindow, AMoveBesideAPaneWithNoRoomIsRefused) {
    // As a split of that pane: both panes keep the least size, or nothing
    // moves.
    WindowOptions options; options.command = {QStringLiteral("/bin/cat")};
    auto* destination = application->newWindow(options); ASSERT_NE(destination, nullptr);
    destination->resize(900, 600);
    destination->show();
    ASSERT_TRUE(waitFor([&] { return destination->isExposed() && destination->submittedFrames() > 0; }));
    PaneLaunchOptions launch; launch.command = {QStringLiteral("/bin/cat")};
    Pane* narrow = destination->focusedPane();
    QString error;
    for (int i = 0; i < 64 && error.isEmpty(); ++i) {
        if (Pane* added = destination->splitPane(narrow, SplitTree::Direction::Right, launch, &error)) narrow = added;
        ASSERT_TRUE(nextFrame(destination));
    }
    ASSERT_EQ(error, TerminalWindow::noRoomForPane());
    const auto panes = destination->panes().size();
    Pane* moving = window->splitPane(window->focusedPane(), SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(moving, nullptr);
    EXPECT_EQ(window->movePaneTo(moving, destination, narrow, SplitTree::Direction::Right),
              QStringLiteral("There is not enough room beside that pane"));
    EXPECT_EQ(moving->parent(), window);
    EXPECT_EQ(window->panes().size(), 2);
    EXPECT_EQ(destination->panes().size(), panes);
    // Zoomed, the pane shows at the whole window's size: its room is still
    // its own.
    ASSERT_TRUE(destination->runAction(narrow, Action::TogglePaneZoom));
    EXPECT_EQ(window->movePaneTo(moving, destination, narrow, SplitTree::Direction::Right),
              QStringLiteral("There is not enough room beside that pane"));
    ASSERT_TRUE(destination->runAction(narrow, Action::TogglePaneZoom));
    // Below it there is room.
    EXPECT_TRUE(window->movePaneTo(moving, destination, narrow, SplitTree::Direction::Down).isEmpty());
    EXPECT_EQ(moving->parent(), destination);
}

// The platform theme of the run: generic (the widget dialog of Qt), or the
// one that headless_native.py --platform-theme names.
QByteArray platformTheme() {
    return qgetenv("QT_QPA_PLATFORMTHEME");
}
// Why the tests that type in the save dialog cannot do it in this run, or
// empty. The GTK dialog has a test of its own.
std::string untypableDialog() {
    const QByteArray theme = platformTheme();
    if (theme == "gtk3") return "the GTK file dialog is no Qt widget: ANativeGtkSaveDialogKeepsItsNameWhenItsWindowCloses tests it";
    if (theme == "xdgdesktopportal") return "the file dialog of the desktop portal runs in another process";
    return {};
}
// With --platform-theme kde, the native KDE dialog shows in place of the
// widget dialog of Qt. The widget dialog there means that the KDE theme did
// not load (plasma-integration), and the run would test no native dialog.
bool themeDialogShows(QFileDialog* dialog) {
    return platformTheme() != "kde" || dialog->testAttribute(Qt::WA_DontShowOnScreen);
}
// The widget that the user sees for @p dialog: the dialog itself, or the
// native dialog of the platform theme (the KDE file dialog), which shows in a
// window of its own while the dialog's window stays hidden.
QWidget* shownDialog(QFileDialog* dialog) {
    if (!dialog->testAttribute(Qt::WA_DontShowOnScreen)) return dialog;
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget != dialog && widget->isVisible() && qobject_cast<QDialog*>(widget)) return widget;
    return nullptr;
}
// The line edit in @p widget that holds @p text: the file name field.
QLineEdit* editHolding(QWidget* widget, const QString& text) {
    for (auto* edit : widget->findChildren<QLineEdit*>())
        if (edit->isVisible() && edit->text() == text) return edit;
    return nullptr;
}
// Saves @p name in @p folder through @p dialog as the user would: in the
// name field that holds @p typed, and through the Save button of a native
// dialog, which reads its own name field.
void saveThrough(QFileDialog* dialog, const QString& typed, const QString& folder, const QString& name) {
    QWidget* const shown = shownDialog(dialog);
    ASSERT_NE(shown, nullptr);
    QLineEdit* const edit = editHolding(shown, typed);
    ASSERT_NE(edit, nullptr) << "no name field holds " << typed.toStdString();
    dialog->setDirectory(folder);
    edit->setText(name);
    if (shown == dialog) {
        // QFileDialog::accept is protected: the QDialog slot reaches it.
        static_cast<QDialog*>(dialog)->accept();
        return;
    }
    for (auto* button : shown->findChildren<QPushButton*>())
        if (button->isVisible() && button->isDefault()) { button->click(); return; }
    ADD_FAILURE() << "the native dialog has no default button";
}

// The visible save dialog of a download, once @p ready holds for it.
QPointer<QFileDialog> downloadDialog(const std::function<bool(QFileDialog*)>& ready) {
    QPointer<QFileDialog> dialog;
    const bool shown = waitFor([&] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* box = qobject_cast<QFileDialog*>(widget); box && box->isVisible()) dialog = box;
        return !dialog.isNull() && ready(dialog);
    });
    return shown ? dialog : QPointer<QFileDialog>();
}
// A window whose program sends a file (OSC 1337 File, named moved.txt), and
// the save dialog that the download opens over it, once its name field
// shows the name. The dialog then shows @p folder, where a save that should
// not happen would land. With @p quick, the window is a quick terminal,
// slid in.
QPointer<QFileDialog> openDownloadDialog(Application& application, QPointer<TerminalWindow>& source, const QString& folder,
                                         const std::function<bool(QFileDialog*)>& ready = {}, bool quick = false) {
    WindowOptions options; options.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral("printf '\\033]1337;File=name=bW92ZWQudHh0;size=2:aGk=\\007'; exec cat")};
    options.quick = quick;
    source = application.newWindow(options);
    if (!source) return {};
    if (quick) source->toggleQuick();
    else source->show();
    // The dialog moves to @p folder once, before its name is checked: a
    // native dialog could answer the move with another name.
    QPointer<QFileDialog> moved;
    return downloadDialog([&](QFileDialog* dialog) {
        if (moved != dialog) {
            dialog->setDirectory(folder);
            moved = dialog;
        }
        if (ready) return ready(dialog);
        return shownDialog(dialog) && editHolding(shownDialog(dialog), QStringLiteral("moved.txt"));
    });
}

// Closes @p window and waits until it is gone.
bool closeAndWait(QPointer<TerminalWindow> window) {
    if (window) window->close();
    const bool gone = waitFor([&] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return window.isNull();
    });
    drain(50);
    return gone;
}

TEST_F(NativeWindow, ADownloadsSaveDialogMovesWithItsPaneWhenItsWindowCloses) {
    // The save dialog of a download waits over its pane's window. The pane
    // moves to another window, and its first window closes: the dialog
    // moves too, with the name that the user typed in it, and is not
    // cancelled. The default run tests the widget dialog of Qt, and a run
    // with --platform-theme kde the native KDE dialog, which moves by a
    // hide and a show.
    if (const std::string why = untypableDialog(); !why.empty()) GTEST_SKIP() << why;
    QPointer<TerminalWindow> source;
    QPointer<QFileDialog> dialog = openDownloadDialog(*application, source, directory.path());
    const auto rejectAll = qScopeGuard([&dialog] {
        if (dialog) dialog->reject();
        drainDeferredDeletes();
    });
    ASSERT_TRUE(source);
    ASSERT_TRUE(dialog);
    ASSERT_TRUE(themeDialogShows(dialog)) << "the KDE platform theme did not load: no native dialog";
    WindowOptions plain; plain.command = {QStringLiteral("/bin/cat")};
    auto* destination = application->newWindow(plain); ASSERT_NE(destination, nullptr);
    destination->show();
    QPointer<Pane> pane = source->focusedPane(); ASSERT_TRUE(pane);
    const QPointer<QWidget> shown = shownDialog(dialog);
    ASSERT_TRUE(shown);
    EXPECT_EQ(shown->windowHandle()->transientParent(), source.data());
    QLineEdit* const name = editHolding(shown, QStringLiteral("moved.txt"));
    ASSERT_NE(name, nullptr);
    name->setText(QStringLiteral("typed.txt"));
    ASSERT_TRUE(source->movePaneTo(pane, destination, destination->focusedPane(), SplitTree::Direction::Right).isEmpty());
    ASSERT_TRUE(closeAndWait(source));
    ASSERT_TRUE(dialog) << "the dialog was cancelled";
    EXPECT_TRUE(dialog->isVisible());
    ASSERT_TRUE(shown) << "the native dialog went";
    EXPECT_EQ(shownDialog(dialog), shown.data());
    EXPECT_TRUE(shown->isVisible());
    EXPECT_EQ(shown->windowHandle()->transientParent(), destination);
    EXPECT_NE(editHolding(shown, QStringLiteral("typed.txt")), nullptr) << "the typed name is lost";
    const QString saved = directory.filePath(QStringLiteral("typed.txt"));
    ASSERT_NO_FATAL_FAILURE(saveThrough(dialog, QStringLiteral("typed.txt"), directory.path(), QStringLiteral("typed.txt")));
    EXPECT_TRUE(waitFor([&] { return QFileInfo(saved).size() == 2; }));
}

TEST_F(NativeWindow, ADownloadsSaveDialogMovesWithItsPaneTwice) {
    // The window that the dialog moved to closes in its turn, after the
    // pane moved on: the dialog follows it again.
    if (const std::string why = untypableDialog(); !why.empty()) GTEST_SKIP() << why;
    // The windows close with their programs running, with no question.
    restartApplication(QStringLiteral("no-confirm.conf"),
        testSettings({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("confirmClose"), false}}}}), {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    QPointer<TerminalWindow> source;
    QPointer<QFileDialog> dialog = openDownloadDialog(*application, source, directory.path());
    const auto rejectAll = qScopeGuard([&dialog] {
        if (dialog) dialog->reject();
        drainDeferredDeletes();
    });
    ASSERT_TRUE(source);
    ASSERT_TRUE(dialog);
    ASSERT_TRUE(themeDialogShows(dialog)) << "the KDE platform theme did not load: no native dialog";
    WindowOptions plain; plain.command = {QStringLiteral("/bin/cat")};
    QPointer<TerminalWindow> middle = application->newWindow(plain); ASSERT_TRUE(middle);
    middle->show();
    auto* destination = application->newWindow(plain); ASSERT_NE(destination, nullptr);
    destination->show();
    QPointer<Pane> pane = source->focusedPane(); ASSERT_TRUE(pane);
    ASSERT_TRUE(source->movePaneTo(pane, middle, middle->focusedPane(), SplitTree::Direction::Right).isEmpty());
    ASSERT_TRUE(closeAndWait(source));
    ASSERT_TRUE(dialog) << "the dialog was cancelled at the first move";
    ASSERT_NE(shownDialog(dialog), nullptr);
    EXPECT_EQ(shownDialog(dialog)->windowHandle()->transientParent(), middle.data());
    ASSERT_TRUE(pane);
    ASSERT_TRUE(middle->movePaneTo(pane, destination, destination->focusedPane(), SplitTree::Direction::Right).isEmpty());
    ASSERT_TRUE(closeAndWait(middle));
    ASSERT_TRUE(dialog) << "the dialog was cancelled at the second move";
    ASSERT_NE(shownDialog(dialog), nullptr);
    EXPECT_TRUE(shownDialog(dialog)->isVisible());
    EXPECT_EQ(shownDialog(dialog)->windowHandle()->transientParent(), destination);
    const QString saved = directory.filePath(QStringLiteral("moved.txt"));
    ASSERT_NO_FATAL_FAILURE(saveThrough(dialog, QStringLiteral("moved.txt"), directory.path(), QStringLiteral("moved.txt")));
    EXPECT_TRUE(waitFor([&] { return QFileInfo(saved).size() == 2; }));
}

TEST_F(NativeWindow, ADownloadsSaveDialogGoesWithItsPanesWindow) {
    // The pane closes with its window: its dialog is cancelled, and saves
    // nothing. Two paths cancel it, and each one alone passes this test:
    // the dialog keeper, when the window's surface goes with the pane
    // still in it, and the pane's own destroyed signal (Pane.cpp).
    if (const std::string why = untypableDialog(); !why.empty()) GTEST_SKIP() << why;
    // The windows close with their programs running, with no question.
    restartApplication(QStringLiteral("no-confirm.conf"),
        testSettings({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("confirmClose"), false}}}}), {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    QPointer<TerminalWindow> source;
    QPointer<QFileDialog> dialog = openDownloadDialog(*application, source, directory.path());
    const auto rejectAll = qScopeGuard([&dialog] {
        if (dialog) dialog->reject();
        drainDeferredDeletes();
    });
    ASSERT_TRUE(source);
    ASSERT_TRUE(dialog);
    ASSERT_TRUE(themeDialogShows(dialog)) << "the KDE platform theme did not load: no native dialog";
    const QPointer<QWidget> shown = shownDialog(dialog);
    ASSERT_TRUE(closeAndWait(source));
    EXPECT_TRUE(waitFor([&] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return dialog.isNull();
    })) << "the dialog of a closed pane stays";
    EXPECT_TRUE(!shown || !shown->isVisible());
    EXPECT_TRUE(QDir(directory.path()).entryList({QStringLiteral("moved.txt")}, QDir::Files).isEmpty());
}

TEST_F(NativeWindow, ADownloadsSaveDialogStaysWhileItsWindowHides) {
    // A window that hides keeps its surface: the dialog over it stays,
    // with the name that the user typed in it, and is over the window
    // again when it shows. A layer surface is no parent for a dialog: the
    // quick terminal slides out on its own while the dialog asks, hides
    // when it has slid out, and slides in again when the dialog closes.
    if (const std::string why = untypableDialog(); !why.empty()) GTEST_SKIP() << why;
    // A failed check ends its own case only.
    const auto stays = [&](bool quick) {
        QPointer<TerminalWindow> source;
        QPointer<QFileDialog> dialog = openDownloadDialog(*application, source, directory.path(), {}, quick);
        const auto rejectAll = qScopeGuard([&dialog] {
            if (dialog) dialog->reject();
            drainDeferredDeletes();
        });
        ASSERT_TRUE(source);
        ASSERT_TRUE(dialog);
        ASSERT_TRUE(themeDialogShows(dialog)) << "the KDE platform theme did not load: no native dialog";
        const QPointer<QWidget> shown = shownDialog(dialog);
        ASSERT_TRUE(shown);
        QLineEdit* const name = editHolding(shown, QStringLiteral("moved.txt"));
        ASSERT_NE(name, nullptr);
        name->setText(QStringLiteral("typed.txt"));
        if (!quick) {
            expectIdle(source);
            source->hide();
        }
        ASSERT_TRUE(waitFor([&] { return source && !source->isVisible(); }, 5000));
        // A cancel is the keeper's reject, at once when the surface goes, or
        // a close that the platform posts to the windows transient for it.
        // Once the calls posted before now ran, either has run, and the
        // reject's deleteLater is sent here: outside an event loop nothing
        // else sends it.
        ASSERT_TRUE(settleQueued());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ASSERT_TRUE(dialog) << "the dialog was cancelled";
        ASSERT_TRUE(shown);
        EXPECT_TRUE(shown->isVisible());
        if (!quick) {
            source->show();
            ASSERT_TRUE(waitFor([&] { return source && source->isExposed(); }, 5000));
        }
        ASSERT_TRUE(dialog) << "the dialog was cancelled when its window showed";
        EXPECT_EQ(shownDialog(dialog), shown.data());
        EXPECT_EQ(shown->windowHandle()->transientParent(), quick ? nullptr : source.data());
        const QString saved = directory.filePath(quick ? QStringLiteral("quick.txt") : QStringLiteral("window.txt"));
        ASSERT_NO_FATAL_FAILURE(saveThrough(dialog, QStringLiteral("typed.txt"), directory.path(), QFileInfo(saved).fileName()));
        EXPECT_TRUE(waitFor([&] { return QFileInfo(saved).size() == 2; }));
        if (quick) {
            EXPECT_TRUE(waitFor([&] { return source && source->quickShown() && source->isExposed(); }, 5000));
        }
    };
    for (const bool quick : {false, true}) {
        SCOPED_TRACE(quick ? "quick terminal" : "window");
        stays(quick);
    }
}

TEST_F(NativeWindow, AQuickTerminalToggledBackMidSlideEndsWhereItWasSent) {
    // A slide starts where the last one is. Toggled out before its slide
    // in has moved, the quick terminal has no distance to go: it still
    // hides. Toggled in again while it slides out, it stays. A slide in
    // starts at the edge, where no frame callback comes to a surface that
    // is wholly off screen: it still moves.
    using PhosphorWayland::LayerSurface;
    const auto openQuick = [this] {
        WindowOptions options;
        options.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("exec cat")};
        options.quick = true;
        return QPointer<TerminalWindow>(application->newWindow(options));
    };
    const auto slidOut = [](const QPointer<TerminalWindow>& quick) {
        return waitFor([&] { return quick && !quick->isVisible(); }, 5000);
    };
    // Each scenario's window goes before the next one opens, also one that
    // slid out: the delete needs no surface, as a close does.
    const auto gone = [](const QPointer<TerminalWindow>& quick) {
        if (quick) quick->deleteLater();
        return waitDeleted(quick);
    };
    {
        SCOPED_TRACE("out before any frame");
        QPointer<TerminalWindow> quick = openQuick();
        ASSERT_TRUE(quick);
        quick->toggleQuick();
        quick->toggleQuick();
        EXPECT_TRUE(slidOut(quick)) << "the quick terminal stays shown";
        EXPECT_TRUE(gone(quick));
    }
    {
        SCOPED_TRACE("out after the first frame");
        QPointer<TerminalWindow> quick = openQuick();
        ASSERT_TRUE(quick);
        quick->toggleQuick();
        ASSERT_TRUE(waitFor([&] { return quick && quick->isExposed() && quick->submittedFrames() > 0; }));
        quick->toggleQuick();
        EXPECT_TRUE(slidOut(quick)) << "the quick terminal stays shown";
        EXPECT_TRUE(gone(quick));
    }
    {
        SCOPED_TRACE("out while it slides in");
        QPointer<TerminalWindow> quick = openQuick();
        ASSERT_TRUE(quick);
        quick->toggleQuick();
        const int hidden = LayerSurface::get(quick)->margins().top();
        ASSERT_LT(hidden, 0);
        ASSERT_TRUE(waitFor([&] { return quick && LayerSurface::get(quick)->margins().top() > hidden / 2; }))
            << "the slide in stays at " << LayerSurface::get(quick)->margins().top();
        quick->toggleQuick();
        EXPECT_TRUE(slidOut(quick)) << "the quick terminal stays shown";
        EXPECT_TRUE(gone(quick));
    }
    {
        SCOPED_TRACE("in again before its slide out moved");
        QPointer<TerminalWindow> quick = openQuick();
        ASSERT_TRUE(quick);
        quick->toggleQuick();
        expectIdle(quick);
        ASSERT_EQ(LayerSurface::get(quick)->margins().top(), 0);
        quick->toggleQuick();
        quick->toggleQuick();
        expectIdle(quick);
        EXPECT_TRUE(quick->isVisible());
        EXPECT_EQ(LayerSurface::get(quick)->margins().top(), 0);
        EXPECT_TRUE(gone(quick));
    }
    {
        // With no slide to go in, the slide out stops where it is: the
        // quick terminal must still show at its edge.
        SCOPED_TRACE("in with no slide while it slides out");
        QPointer<TerminalWindow> quick = openQuick();
        ASSERT_TRUE(quick);
        quick->toggleQuick();
        expectIdle(quick);
        ASSERT_EQ(LayerSurface::get(quick)->margins().top(), 0);
        SettingsStore file(directory.filePath(QStringLiteral("afterglow.conf")));
        ASSERT_TRUE(file.write(QStringLiteral("QuickTerminal"), QStringLiteral("duration"), 5000));
        ASSERT_TRUE(quick->runAction(quick->focusedPane(), Action::ReloadConfig));
        quick->toggleQuick();
        ASSERT_TRUE(waitFor([&] { return quick && LayerSurface::get(quick)->margins().top() < -10; }))
            << "the slide out stays at " << LayerSurface::get(quick)->margins().top();
        ASSERT_TRUE(file.write(QStringLiteral("QuickTerminal"), QStringLiteral("animate"), false));
        ASSERT_TRUE(quick->runAction(quick->focusedPane(), Action::ReloadConfig));
        quick->toggleQuick();
        expectIdle(quick);
        EXPECT_TRUE(quick->isVisible());
        EXPECT_EQ(LayerSurface::get(quick)->margins().top(), 0);
        EXPECT_TRUE(gone(quick));
    }
}

// A program in @p directory that shows each byte it gets as text (cat -v
// in raw mode), after "ready": a program to run by its path, as a shell
// is. Empty when it could not be written.
QString catVScript(const QTemporaryDir& directory) {
    const QString program = directory.filePath(QStringLiteral("cat-v.sh"));
    QFile script(program);
    if (!script.open(QIODevice::WriteOnly) || script.write("#!/bin/sh\nstty raw -echo; printf ready; exec cat -v\n") < 0) return {};
    script.close();
    if (!QFile::setPermissions(program, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) return {};
    return program;
}

TEST_F(NativeWindow, AHeldChordWithNothingToDoRepeatsIntoTheProgram) {
    // Fullscreen has nothing to do in a quick terminal: F11 reaches the
    // program, which must get its repeats too. cat -v in raw mode shows
    // each F11 as "^[[23~".
    const QString program = catVScript(directory);
    ASSERT_FALSE(program.isEmpty());
    WindowOptions options;
    options.command = {program};
    options.quick = true;
    QPointer<TerminalWindow> quick = application->newWindow(options);
    ASSERT_TRUE(quick);
    quick->toggleQuick();
    Pane* pane = quick->focusedPane();
    ASSERT_NE(pane, nullptr);
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("ready")); }));
    holdKey(quick, Qt::Key_F11, Qt::NoModifier, {}, 3);
    ASSERT_TRUE(settleQueued());
    const QString key = QStringLiteral("^[[23~");
    EXPECT_TRUE(waitFor([&] { return screenOf(pane).count(key) >= 4; })) << screenOf(pane).toStdString();
    // A marker goes through the same input after the keys: a fifth F11
    // shows before it.
    ASSERT_TRUE(pane->sendInput("@marker@"));
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("@marker@")); })) << screenOf(pane).toStdString();
    EXPECT_EQ(screenOf(pane).count(key), 4) << screenOf(pane).toStdString();
    EXPECT_FALSE(quick->windowStates().testFlag(Qt::WindowFullScreen));
}

TEST_F(NativeWindow, AQuickTerminalMovesToTheScreenNamedWhileItShowedAtItsNextShow) {
    // A shown layer surface keeps its output: a QuickTerminal.screen set
    // while it shows takes effect when it shows again, and meanwhile it
    // keeps the size of the output that it is on.
    using PhosphorWayland::LayerSurface;
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.size() < 2) GTEST_SKIP() << "needs two outputs (kwin_wayland --output-count 2)";
    QScreen* const first = QGuiApplication::primaryScreen();
    QScreen* const second = screens.value(0) == first ? screens.value(1) : screens.value(0);
    WindowOptions options;
    options.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("exec cat")};
    options.quick = true;
    QPointer<TerminalWindow> quick = application->newWindow(options);
    ASSERT_TRUE(quick);
    quick->toggleQuick();
    ASSERT_TRUE(waitFor([&] { return quick && quick->isExposed() && quick->submittedFrames() > 0; }));
    expectIdle(quick);
    ASSERT_EQ(LayerSurface::get(quick)->screen(), first);
    const int height = quick->height();
    SettingsStore file(directory.filePath(QStringLiteral("afterglow.conf")));
    ASSERT_TRUE(file.write(QStringLiteral("QuickTerminal"), QStringLiteral("screen"), second->name()));
    ASSERT_TRUE(quick->runAction(quick->focusedPane(), Action::ReloadConfig));
    EXPECT_EQ(LayerSurface::get(quick)->screen(), first);
    // The outputs of the runner have one size, so this guards the height: it
    // cannot tell the height of the new screen from the old one.
    EXPECT_EQ(quick->height(), height);
    quick->toggleQuick();
    ASSERT_TRUE(waitFor([&] { return quick && !quick->isVisible(); }, 5000));
    quick->toggleQuick();
    ASSERT_TRUE(waitFor([&] { return quick && quick->isExposed(); }));
    EXPECT_EQ(LayerSurface::get(quick)->screen(), second);
    EXPECT_TRUE(waitFor([&] { return quick && quick->screen() == second; })) << "the quick terminal shows on " << (quick && quick->screen() ? quick->screen()->name() : QString()).toStdString();
    expectIdle(quick);
    EXPECT_EQ(LayerSurface::get(quick)->margins().top(), 0);
}

// The file chooser of the gtk3 platform theme, which is no Qt widget: the
// test reaches it through the GTK that the theme loaded.
class GtkChooser
{
public:
    GtkChooser()
        : m_gtk(dlopen("libgtk-3.so.0", RTLD_NOLOAD | RTLD_LAZY)), m_glib(dlopen("libglib-2.0.so.0", RTLD_NOLOAD | RTLD_LAZY)),
          m_gobject(dlopen("libgobject-2.0.so.0", RTLD_NOLOAD | RTLD_LAZY))
    {
    }
    ~GtkChooser()
    {
        for (void* library : {m_gtk, m_glib, m_gobject})
            if (library) dlclose(library);
    }
    GtkChooser(const GtkChooser&) = delete;
    GtkChooser& operator=(const GtkChooser&) = delete;
    /// The visible file chooser dialogs.
    std::vector<void*> shown() const
    {
        struct List { void* data; List* next; List* previous; };
        const auto list = symbol<List* (*)()>(m_gtk, "gtk_window_list_toplevels");
        const auto chooserType = symbol<std::size_t (*)()>(m_gtk, "gtk_file_chooser_get_type");
        const auto isA = symbol<int (*)(void*, std::size_t)>(m_gobject, "g_type_check_instance_is_a");
        const auto visible = symbol<int (*)(void*)>(m_gtk, "gtk_widget_get_visible");
        const auto release = symbol<void (*)(List*)>(m_glib, "g_list_free");
        std::vector<void*> out;
        if (!list || !chooserType || !isA || !visible || !release) return out;
        List* const all = list();
        for (List* each = all; each; each = each->next)
            if (isA(each->data, chooserType()) && visible(each->data)) out.push_back(each->data);
        release(all);
        return out;
    }
    QString name(void* chooser) const
    {
        const auto get = symbol<char* (*)(void*)>(m_gtk, "gtk_file_chooser_get_current_name");
        const auto release = symbol<void (*)(void*)>(m_glib, "g_free");
        if (!get || !release) return {};
        char* const text = get(chooser);
        const QString out = QString::fromUtf8(text);
        release(text);
        return out;
    }
    /// Types @p name in the name field, as the user would.
    void setName(void* chooser, const QString& name) const
    {
        if (const auto set = symbol<void (*)(void*, const char*)>(m_gtk, "gtk_file_chooser_set_current_name"))
            set(chooser, name.toUtf8().constData());
    }
    /// The Save button (GTK_RESPONSE_OK, which the theme gives it).
    void save(void* chooser) const
    {
        if (const auto respond = symbol<void (*)(void*, int)>(m_gtk, "gtk_dialog_response")) respond(chooser, -5);
    }

private:
    template <class Function>
    static Function symbol(void* library, const char* name)
    {
        return library ? reinterpret_cast<Function>(dlsym(library, name)) : nullptr;
    }
    void* m_gtk;
    void* m_glib;
    void* m_gobject;
};

TEST_F(NativeWindow, ANativeGtkSaveDialogKeepsItsNameWhenItsWindowCloses) {
    // The GTK dialog shows in a window that is not Qt's, and the window
    // that closes does not close it. It stays as it is: shown again, it
    // would take back the first name, and what the user typed would go.
    if (platformTheme() != "gtk3") GTEST_SKIP() << "runs with --platform-theme gtk3";
    const GtkChooser gtk;
    QPointer<TerminalWindow> source;
    QPointer<QFileDialog> dialog = openDownloadDialog(*application, source, directory.path(), [&](QFileDialog*) {
        const auto shown = gtk.shown();
        return shown.size() == 1 && gtk.name(shown.front()) == QStringLiteral("moved.txt");
    });
    const auto rejectAll = qScopeGuard([&dialog] {
        if (dialog) dialog->reject();
        drainDeferredDeletes();
    });
    ASSERT_TRUE(source);
    ASSERT_TRUE(dialog) << "no GTK file chooser shows the name";
    void* const chooser = gtk.shown().front();
    gtk.setName(chooser, QStringLiteral("typed.txt"));
    WindowOptions plain; plain.command = {QStringLiteral("/bin/cat")};
    auto* destination = application->newWindow(plain); ASSERT_NE(destination, nullptr);
    destination->show();
    QPointer<Pane> pane = source->focusedPane(); ASSERT_TRUE(pane);
    ASSERT_TRUE(source->movePaneTo(pane, destination, destination->focusedPane(), SplitTree::Direction::Right).isEmpty());
    ASSERT_TRUE(closeAndWait(source));
    // The GTK chooser runs in GTK's loop: a response that cancels it would
    // come from GTK's handling of the compositor's events, which no call
    // posted to Qt's queue orders, so no barrier shows that none came.
    // The drain gives GTK that time. The reject's deleteLater is sent
    // after it: outside an event loop nothing else sends it.
    drain(200);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    ASSERT_TRUE(dialog) << "the dialog was cancelled";
    EXPECT_EQ(dialog->windowHandle()->transientParent(), destination);
    ASSERT_EQ(gtk.shown(), std::vector<void*>{chooser});
    EXPECT_EQ(gtk.name(chooser).toStdString(), "typed.txt") << "the typed name is lost";
    const QString saved = directory.filePath(QStringLiteral("typed.txt"));
    dialog->setDirectory(directory.path());
    ASSERT_TRUE(waitFor([&] { return dialog->selectedFiles() == QStringList{saved}; }))
        << dialog->selectedFiles().join(QLatin1Char(',')).toStdString();
    gtk.save(chooser);
    EXPECT_TRUE(waitFor([&] { return QFileInfo(saved).size() == 2; }));
}

TEST_F(NativeWindow, NewTabFromTheMenuOfABackgroundTabSwitchesTabsOnce) {
    // The tab menu's New Tab on a tab that does not show opens the new tab
    // beside that tab, with one switch: from the current tab to the new
    // one, not through the menu's tab first.
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->openTab(launch, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(window->focusedPane(), second);
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(settleQueued());
    const quint32 current = window->activeTabId();
    QList<std::pair<quint32, quint32>> switches;
    QObject listener;
    QObject::connect(window, &TerminalWindow::tabSwitched, &listener,
                     [&](quint32 from, quint32 to, int) { switches.append({from, to}); });
    chrome->tabMenu(0, 50, 15);
    int newTab = -1;
    for (const auto& item : chrome->contextMenu().value(QStringLiteral("items")).toList())
        if (item.toMap().value(QStringLiteral("title")).toString() == QStringLiteral("New Tab"))
            newTab = item.toMap().value(QStringLiteral("id")).toInt();
    ASSERT_GE(newTab, 0);
    chrome->activateMenuItem(newTab);
    ASSERT_TRUE(waitFor([&] { return window->tabs().size() == 3; }));
    ASSERT_TRUE(settleQueued());
    ASSERT_EQ(switches.size(), 1);
    EXPECT_EQ(switches[0].first, current);
    EXPECT_EQ(switches[0].second, window->activeTabId());
    EXPECT_EQ(window->tabIndexOf(window->focusedPane()), 1);
}

TEST_F(NativeWindow, AProgramWhoseTabClosedShowsAndWritesNothing) {
    // What a program sends while its tab closes arrives after the close: a
    // new notice, a status that asks for attention, a bell and a clipboard
    // write from a program that is ending reach nothing, and the close of a
    // notice it showed still goes through.
    RegisteredNotificationServer notices;
    ASSERT_TRUE(notices.registered);
    // The bell's sound is a pw-play of the test's own, which counts the
    // bells that play.
    const QString bin = directory.filePath(QStringLiteral("bin"));
    const QString bells = directory.filePath(QStringLiteral("bells"));
    ASSERT_TRUE(QDir().mkpath(bin));
    {
        QFile player(bin + QStringLiteral("/pw-play"));
        ASSERT_TRUE(player.open(QIODevice::WriteOnly));
        player.write(QStringLiteral("#!/bin/sh\necho \"${AFTERGLOW_TEST_BELL:-closed}\" >> '%1'\n").arg(bells).toUtf8());
    }
    ASSERT_TRUE(QFile::setPermissions(bin + QStringLiteral("/pw-play"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const Saved path("PATH", QFile::encodeName(bin) + ':' + qgetenv("PATH"));
    const auto played = [&] {
        QFile file(bells);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    restartApplication(QStringLiteral("closed-tab.conf"),
        testSettings({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("confirmClose"), false},
                                                               {QStringLiteral("clipboardWrite"), QStringLiteral("ask")},
                                                               {QStringLiteral("bell"), QStringLiteral("visual")},
                                                               {QStringLiteral("bellSound"), directory.filePath(QStringLiteral("bell.oga"))},
                                                               {QStringLiteral("bellSoundInterval"), 0}}}}),
        {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    QPointer<Pane> first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* second = window->openTab(launch, nullptr);
    ASSERT_NE(second, nullptr);
    feedFromReader(first, "\x1b]99;i=shown;before\x1b\\");
    ASSERT_TRUE(waitFor([&] { return notices.server.count == 1; }));
    ASSERT_TRUE(window->runAction(first, Action::CloseTab));
    ASSERT_EQ(window->tabs().size(), 1);
    // The pane lives until its deferred delete, which the test holds back.
    ASSERT_TRUE(first);
    ASSERT_EQ(notices.server.closes, 0);
    feedFromReader(first, "\x1b]99;i=late;after\x1b\\" "\x1b]9;after\x07" "\x1b]7501;state=error\x1b\\" "\a"
                          "\x1b]52;c;bGF0ZQ==\x07" "\x1b]99;i=shown:p=close;\x1b\\");
    EXPECT_TRUE(waitFor([&] { return notices.server.closes == 1; })) << "the close of the notice did not arrive";
    ASSERT_TRUE(settleQueued());
    // The notices arrive in the order sent: a notice of the closed tab
    // arrives before a marker sent after it.
    NotificationCenter::instance().showNotice(nullptr, QStringLiteral("Test"), QStringLiteral("last"));
    ASSERT_TRUE(waitFor([&] { return notices.server.bodies.contains(QStringLiteral("last")); }));
    EXPECT_EQ(notices.server.count, 2) << notices.server.bodies.join(QLatin1Char('|')).toStdString();
    EXPECT_FALSE(chrome->clipboardPromptOpen());
    // The same write and bell from a program in a tab ask and ring. The
    // bell of the closed tab would have started first, with no
    // AFTERGLOW_TEST_BELL: each bell's player writes the value that it
    // inherited.
    const Saved bell("AFTERGLOW_TEST_BELL", "open");
    feedFromReader(second, "\a");
    EXPECT_TRUE(waitFor([&] { return played().contains("open"); })) << "no bell plays";
    EXPECT_EQ(played(), QByteArray("open\n")) << "the bell of the closed tab played";
    feedFromReader(second, "\x1b]52;c;bGF0ZQ==\x07");
    EXPECT_TRUE(waitFor([&] { return chrome->clipboardPromptOpen(); }));
    // A player of the closed tab is a process of its own: by now, after the
    // prompt's round trip, it would have written too.
    EXPECT_FALSE(played().contains("closed"));
}

TEST_F(NativeWindow, AHeldChordSplitsOpensAndClosesOnce) {
    // Each auto-repeat of a held chord would split, open or close one more,
    // or turn a toggle back. The repeats are taken: no program gets one of
    // them. Every pane runs cat -v in raw mode, which shows a chord that
    // reached it as "^[^R". A setting that no group has makes each reload
    // of the configuration show its problems again.
    RegisteredNotificationServer notices;
    ASSERT_TRUE(notices.registered);
    const QString program = catVScript(directory);
    ASSERT_FALSE(program.isEmpty());
    restartApplication(QStringLiteral("held.conf"),
        testSettings({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("confirmClose"), false}, {QStringLiteral("shell"), program}}},
                      {QStringLiteral("Keybinds"), QJsonObject{{QStringLiteral("bindings"), QJsonObject{
                          {QStringLiteral("split_right"), QStringLiteral("Ctrl+Alt+R")},
                          {QStringLiteral("new_tab"), QStringLiteral("Ctrl+Alt+N")},
                          {QStringLiteral("close_pane"), QStringLiteral("Ctrl+Alt+W")},
                          {QStringLiteral("toggle_pane_zoom"), QStringLiteral("Ctrl+Alt+Z")},
                          {QStringLiteral("reload_config"), QStringLiteral("Ctrl+Alt+L")}}}}},
                      {QStringLiteral("NoSuchGroup"), QJsonObject{{QStringLiteral("key"), 1}}}}),
        {});
    if (HasFatalFailure()) return;
    // The panes of every tab, each once its program is ready for keys.
    const auto ready = [&] {
        return waitFor([&] {
            for (Pane* pane : window->panes())
                if (!screenOf(pane).contains(QStringLiteral("ready"))) return false;
            return true;
        });
    };
    // A marker goes through each pane's input after the keys: a key that
    // reached a program shows before it.
    int markers = 0;
    const auto noKeyReached = [&] {
        const QString marker = QStringLiteral("@marker%1@").arg(++markers);
        for (Pane* pane : window->panes()) {
            ASSERT_TRUE(pane->sendInput(marker.toStdString()));
            ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(marker); })) << screenOf(pane).toStdString();
            EXPECT_FALSE(screenOf(pane).contains(QLatin1Char('^'))) << screenOf(pane).toStdString();
        }
    };
    ASSERT_TRUE(ready());
    holdKey(window, Qt::Key_R, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("r"), 4);
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(window->panes().size(), 2);
    ASSERT_TRUE(ready());
    holdKey(window, Qt::Key_N, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("n"), 4);
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(window->tabs().size(), 2);
    ASSERT_TRUE(ready());
    noKeyReached();
    // The new tab's pane closes, and its tab with it: the split stays.
    holdKey(window, Qt::Key_W, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("w"), 4);
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(window->tabs().size(), 1);
    ASSERT_EQ(window->panes().size(), 2);
    // A held toggle turns once: four presses would turn it back.
    Pane* const zoomed = window->focusedPane();
    Pane* const other = window->panes().value(0) == zoomed ? window->panes().value(1) : window->panes().value(0);
    holdKey(window, Qt::Key_Z, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("z"), 3);
    ASSERT_TRUE(settleQueued());
    EXPECT_TRUE(waitFor([&] { return window->accessiblePaneRect(other).isEmpty() && !window->accessiblePaneRect(zoomed).isEmpty(); }))
        << "the zoom turned back";
    noKeyReached();
    // A held reload shows the problems once. The notices arrive in the
    // order sent: all of them are in once a marker sent after them is.
    const auto noticesIn = [&](const QString& marker) {
        NotificationCenter::instance().showNotice(nullptr, QStringLiteral("Test"), marker);
        return waitFor([&] { return notices.server.bodies.contains(marker); });
    };
    ASSERT_TRUE(noticesIn(QStringLiteral("before")));
    const int shown = notices.server.count;
    holdKey(window, Qt::Key_L, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("l"), 3);
    ASSERT_TRUE(settleQueued());
    EXPECT_TRUE(waitFor([&] { return notices.server.count > shown; })) << "the reload showed no problems";
    ASSERT_TRUE(noticesIn(QStringLiteral("after")));
    // The problems once, and the marker.
    EXPECT_EQ(notices.server.count, shown + 2) << notices.server.bodies.join(QLatin1Char('|')).toStdString();
    noKeyReached();
}

TEST_F(NativeWindow, ASplitFromAfterglowCtlDoesNotShowItsWindow) {
    // afterglow ctl and scripts split a pane where it is: its tab shows and
    // the new pane takes the focus, and the window is not shown (so it is
    // not active either).
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    ASSERT_NE(window->openTab(launch, nullptr), nullptr);
    window->hide();
    ASSERT_TRUE(waitFor([&] { return !window->isVisible(); }));
    QString error;
    Pane* added = window->splitPane(first, SplitTree::Direction::Right, launch, &error);
    ASSERT_NE(added, nullptr) << error.toStdString();
    ASSERT_TRUE(settleQueued());
    EXPECT_FALSE(window->isVisible());
    EXPECT_EQ(window->tabIndexOf(added), window->tabIndexOf(first));
    EXPECT_EQ(window->tabIndexOf(window->focusedPane()), window->tabIndexOf(first));
    EXPECT_EQ(window->focusedPane(), added);
}

TEST_F(NativeWindow, AProjectSavesTheTabOfTheLastOutputAsAShell) {
    // The pager of show_last_output reads a temporary file that goes with
    // its tab: a project saves that tab as a shell, not as the pager.
    restartApplication(QStringLiteral("pager.conf"),
        testSettings({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("pager"), QStringLiteral("/usr/bin/tail -f")}}}}),
        {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    Pane* pane = window->focusedPane();
    feedFromReader(pane, "\x1b]133;A\x1b\\$ \x1b]133;B\x1b\\echo test\x1b]133;C\x1b\\result\r\n\x1b]133;D;0\x1b\\");
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(window->runAction(pane, Action::ShowLastOutput));
    ASSERT_EQ(window->tabs().size(), 2);
    Pane* pager = window->focusedPane();
    ASSERT_NE(pager, pane);
    ASSERT_TRUE(waitFor([&] { return screenOf(pager).contains(QStringLiteral("result")); })) << screenOf(pager).toStdString();
    bool found = false;
    for (const auto tab : window->projectLayout().value(QStringLiteral("tabs")).toArray()) {
        const QJsonObject saved = tab.toObject().value(QStringLiteral("tree")).toObject().value(QStringLiteral("pane")).toObject();
        if (saved.value(QStringLiteral("id")).toString() != QString::number(pager->id())) continue;
        found = true;
        EXPECT_TRUE(saved.value(QStringLiteral("command")).toArray().isEmpty())
            << QJsonDocument(saved).toJson(QJsonDocument::Compact).toStdString();
    }
    EXPECT_TRUE(found);
}

TEST_F(NativeWindow, AWindowWhoseJobNamesNothingClosesWithNoQuestion) {
    // A job whose name is only characters that disguise text names nothing
    // in the question: the window closes at once, as with no job.
    const QString program = directory.filePath(QString(QChar(0x200B)));
    ASSERT_TRUE(QFile::link(QStringLiteral("/bin/cat"), program));
    WindowOptions options;
    options.command = {program};
    QPointer<TerminalWindow> other = application->newWindow(options);
    ASSERT_TRUE(other);
    other->show();
    ASSERT_TRUE(waitFor([&] { return other->isExposed(); }));
    auto* chrome = other->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    EXPECT_TRUE(other->close());
    EXPECT_FALSE(chrome->confirmPromptOpen());
    EXPECT_TRUE(waitFor([&] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return other.isNull();
    })) << "the window stayed";
}

TEST_F(NativeWindow, AHeldPaneSaysWhichSignalEndedItsProgram) {
    // A program killed by a signal did not exit with a status: the note
    // names the signal, not the 128 + N of a shell's $?.
    Pane* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("read line; kill -9 $$")};
    Pane* held = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(held, nullptr);
    held->setHold(true);
    ASSERT_TRUE(settleQueued());
    ASSERT_TRUE(held->sendInput("\n"));
    ASSERT_TRUE(waitFor([&] { return held->isHeld(); }));
    ASSERT_TRUE(waitFor([&] { return screenOf(held).contains(QStringLiteral("Press any key")); })) << screenOf(held).toStdString();
    EXPECT_TRUE(screenOf(held).contains(QStringLiteral("killed by signal 9"))) << screenOf(held).toStdString();
    EXPECT_FALSE(screenOf(held).contains(QStringLiteral("137"))) << screenOf(held).toStdString();
}

TEST_F(NativeWindow, AProjectPaneWhoseProgramCannotStartGetsAShellInItsDirectory) {
    // The program of a restored pane exists but cannot run (its interpreter
    // is missing): the pane holds a plain sh, in the pane's directory.
    const QString place = directory.filePath(QStringLiteral("project-place"));
    ASSERT_TRUE(QDir().mkpath(place));
    const QString broken = directory.filePath(QStringLiteral("broken-program"));
    {
        QFile file(broken);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("#!/nonexistent/afterglow-interpreter\n");
    }
    ASSERT_TRUE(QFile::setPermissions(broken, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    const QJsonObject paneData{{QStringLiteral("id"), QStringLiteral("p")}, {QStringLiteral("cwd"), place},
                               {QStringLiteral("title"), QString()}, {QStringLiteral("command"), QJsonArray{broken}}};
    const QJsonObject document{{QStringLiteral("version"), 1}, {QStringLiteral("active_tab"), QStringLiteral("t")},
        {QStringLiteral("tabs"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("t")}, {QStringLiteral("title"), QString()},
            {QStringLiteral("active_pane"), QStringLiteral("p")}, {QStringLiteral("tree"), QJsonObject{{QStringLiteral("pane"), paneData}}}}}}};
    WindowOptions options;
    options.empty = true;
    QPointer<TerminalWindow> restored = application->newWindow(options);
    ASSERT_TRUE(restored);
    const QVariantMap result = restored->restoreProject(document);
    ASSERT_TRUE(result.value(QStringLiteral("error")).toString().isEmpty()) << result.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(result.value(QStringLiteral("warnings")).toStringList().join(QLatin1Char('\n')).contains(QStringLiteral("/bin/sh recovery")));
    restored->show();
    Pane* pane = restored->focusedPane();
    ASSERT_NE(pane, nullptr);
    EXPECT_TRUE(waitFor([&] { return pane->workingDirectory() == place; })) << pane->workingDirectory().toStdString();
}

TEST_F(NativeWindow, AnOverlaysProgramGetsTheRemoteSocketOfItsHost) {
    // An overlay's program has its host's AFTERGLOW_PANE_ID: it answers as
    // the host, unless an ssh session runs in the overlay itself.
    Pane* pane = window->focusedPane();
    const AfterglowProto::Remote::Answer before{"/run/user/1000/afterglow/0123456789abcdef.sock", AfterglowProto::Remote::Error::Off};
    const AfterglowProto::Remote::Answer after{"/run/user/1000/afterglow/fedcba9876543210.sock", AfterglowProto::Remote::Error::Off};
    const std::string reply = AfterglowProto::Remote::reply(after);
    ASSERT_NE(reply.find("path="), std::string::npos) << reply;
    pane->setRemoteSocket(before);
    const QString file = directory.filePath(QStringLiteral("remote-reply"));
    const QString id = directory.filePath(QStringLiteral("remote-pane-id"));
    // The program keeps its AFTERGLOW_PANE_ID, asks once it gets a byte,
    // which the test sends after the answer changed, and keeps the reply.
    auto answer = runOverlay(window, pane, {QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral("printf %s \"$AFTERGLOW_PANE_ID\" > '%3'; "
                       "stty raw -echo; head -c 1 > /dev/null; printf '\\033]24569;q=remote\\033\\\\'; head -c %1 > '%2'")
            .arg(reply.size()).arg(file, id)},
        {{QStringLiteral("capture"), QStringLiteral("none")}});
    ASSERT_NE(pane->overlay(), nullptr);
    Pane* program = pane->overlay()->pane();
    EXPECT_TRUE(program->remoteSocketAnswer() == before);
    // An ssh session in the overlay answers there, and the pane keeps its own.
    const AfterglowProto::Remote::Answer own{"/run/user/1000/afterglow/1111111111111111.sock", AfterglowProto::Remote::Error::Off};
    pane->setRemoteSocket(before, own);
    EXPECT_TRUE(program->remoteSocketAnswer() == own);
    EXPECT_TRUE(pane->remoteSocketAnswer() == before);
    pane->setRemoteSocket(after);
    // Sent before stty runs, the byte waits in the line discipline's queue:
    // raw mode then lets head read it.
    ASSERT_TRUE(program->sendInput("g"));
    ASSERT_TRUE(waitFor([&] { return answer->done; }, 5000));
    EXPECT_EQ(fileContent(file).toStdString(), reply);
    EXPECT_EQ(QString::fromLatin1(fileContent(id)), QString::number(pane->id()));
}

TEST_F(NativeWindow, ARefusedMoveToANewTabKeepsItsKeyAndSaysWhy) {
    // cat -v shows each byte that reaches the program: the chord would
    // come as ESC and CR.
    restartApplication(QStringLiteral("move.conf"),
        testSettings({{QStringLiteral("Keybinds"), QJsonObject{
            {QStringLiteral("bindings"), QJsonObject{{QStringLiteral("move_pane_to_tab"), QStringLiteral("Ctrl+Alt+M")}}}}}}),
        {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty raw -echo; printf ready; exec cat -v")});
    if (HasFatalFailure()) return;
    RegisteredNotificationServer notices;
    ASSERT_TRUE(notices.registered);
    Pane* lone = window->focusedPane();
    ASSERT_TRUE(waitFor([&] { return screenOf(lone).contains(QStringLiteral("ready")); })) << screenOf(lone).toStdString();
    const quint32 tab = window->tabIdOf(lone);
    // A pane alone in its tab: the move would only replace the tab. The
    // chord is held: its repeats do not try again.
    holdKey(window, Qt::Key_M, Qt::ControlModifier | Qt::AltModifier, QStringLiteral("m"), 4);
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(window->tabIdOf(lone), tab);
    EXPECT_EQ(window->tabs().size(), 1);
    // The next key is the first that the program gets: it shows right
    // after "ready".
    sendKey(window, Qt::Key_Z, Qt::NoModifier, QStringLiteral("z"));
    EXPECT_TRUE(waitFor([&] { return screenOf(lone).contains(QLatin1Char('z')); })) << screenOf(lone).toStdString();
    EXPECT_TRUE(screenOf(lone).contains(QStringLiteral("readyz"))) << screenOf(lone).toStdString();
    // afterglow ctl and scripts hear that it failed, and the user hears why
    // once more.
    EXPECT_FALSE(window->runAction(lone, Action::MovePaneToTab));
    EXPECT_EQ(window->tabs().size(), 1);
    // The notices arrive in the order sent: all of them are in once the
    // last one is.
    NotificationCenter::instance().showNotice(nullptr, QStringLiteral("Test"), QStringLiteral("last"));
    ASSERT_TRUE(waitFor([&] { return notices.server.bodies.contains(QStringLiteral("last")); }));
    const QString refused = QStringLiteral("The pane is alone in its tab already");
    EXPECT_EQ(notices.server.bodies, (QStringList{refused, refused, QStringLiteral("last")}));
}

TEST_F(NativeWindow, TerminalEventsReachTheGuiThreadInTheOrderTheyHappened) {
    Pane* pane = window->focusedPane();
    QStringList order;
    QObject listener;
    QObject::connect(pane, &Pane::commandBegan, &listener, [&](const QString&) { order << QStringLiteral("began"); });
    QObject::connect(pane, &Pane::userVarUpdated, &listener, [&](const QString& name, const QString&) { order << name; });
    QObject::connect(pane, &Pane::commandEnded, &listener, [&](const QString&, int, qint64) { order << QStringLiteral("ended"); });
    // A command that sets a user variable while it runs, all in one read.
    feedFromReader(pane, "\x1b]133;A\x1b\\$ \x1b]133;B\x1b\\echo test\x1b]133;C\x1b\\"
                         "\x1b]1337;SetUserVar=during=dg==\x07"
                         "result\x1b]133;D;0\x1b\\");
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(order, (QStringList{QStringLiteral("began"), QStringLiteral("during"), QStringLiteral("ended")}));
}

TEST_F(NativeWindow, QueuedClipboardWritesAreBoundedByTheirSize) {
    // A program that reads nothing: only the line discipline echoes the
    // terminal's replies, as text with ESC shown as ^[, so none comes back
    // as a sequence.
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sleep"), QStringLiteral("30")};
    Pane* pane = window->splitPane(window->focusedPane(), SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(pane, nullptr);
    // The window would put 65 MiB on the desktop's clipboard: only the
    // pane's queue is under test.
    QObject::disconnect(pane, &Pane::clipboardRequested, window, nullptr);
    int writes = 0;
    QObject listener;
    QObject::connect(pane, &Pane::clipboardRequested, &listener, [&](const AfterglowVt::ClipboardRequest&) { ++writes; });
    AfterglowVt::ClipboardRequest write;
    write.kind = AfterglowVt::ClipboardRequest::Kind::Write;
    write.data = {{"text/plain", std::string(65u * 1024 * 1024, 'a')}};
    // Two writes in one read hold more than the queue takes: the second
    // gets the answer of a terminal with no clipboard support.
    std::string sequence = AfterglowProto::clipboardWriteRequest(write);
    write.data.clear();
    sequence += sequence;
    feedFromReader(pane, sequence);
    sequence.clear();
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(writes, 1);
    // Once the window took it, another write goes through.
    write.data = {{"text/plain", std::string("small")}};
    feedFromReader(pane, AfterglowProto::clipboardWriteRequest(write));
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(writes, 2);
}

} // namespace

using MuxTest::sessionNamed;
using MuxTest::shellSession;

namespace {

/// Shows session @p name of @p daemon in a new window of @p application.
QPointer<TerminalWindow> showSession(Application& application, MuxTest::TestDaemon& daemon, const QString& name, QString* error) {
    WindowOptions options;
    options.session = MuxClient::attach(name, true, error, daemon.sockets(), MuxSettings{}.eventQueue,
                                        AfterglowVt::TerminalOptions{}.maxScrollback);
    if (!options.session) return nullptr;
    QPointer<TerminalWindow> shown = application.newWindow(options);
    if (shown) shown->show();
    return shown;
}

/// A session's window, and its pane once the shell showed its prompt ("$").
struct ShownSession
{
    QPointer<TerminalWindow> window;
    Pane* pane = nullptr;
};

/// showSession, then the wait for the window's first frame and the shell's
/// prompt. A null pane, with @p error, when one does not come: the window
/// is closed then.
ShownSession showSessionAtPrompt(Application& application, MuxTest::TestDaemon& daemon, const QString& name, QString* error) {
    ShownSession shown{showSession(application, daemon, name, error), nullptr};
    if (!shown.window) return shown;
    if (waitFor([&] { return shown.window->isExposed() && shown.window->submittedFrames() > 0; })) shown.pane = shown.window->focusedPane();
    if (!shown.pane || !waitFor([&] { return screenOf(shown.pane).startsWith(QLatin1Char('$')); }, 5000)) {
        *error = shown.pane ? QStringLiteral("no prompt: ") + screenOf(shown.pane) : QStringLiteral("the window shows no pane");
        delete shown.window;
        return {};
    }
    return shown;
}

} // namespace

// A window shows a session of a daemon of the test's own (never the user's):
// the shell's screen, typed input, the window's own size, and the end of the
// session when the window closes (Mux.onClose kill, the default).
TEST_F(NativeWindow, AMuxSessionShowsInAWindowAndEndsWithIt) {
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "live")) << daemon.lastError();
    QString error;
    QPointer<TerminalWindow> shown = showSession(*application, daemon, QStringLiteral("live"), &error);
    ASSERT_NE(shown, nullptr) << error.toStdString();
    // Far narrower than the 80 columns of the session's start: a size that
    // only the window's measure gives.
    shown->resize(400, 420);
    ASSERT_TRUE(waitFor([&] { return shown->isExposed() && shown->submittedFrames() > 0; }));
    Pane* pane = shown->focusedPane();
    ASSERT_NE(pane, nullptr);
    EXPECT_TRUE(pane->isSessionPane());
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).startsWith(QLatin1Char('$')); }, 5000)) << screenOf(pane).toStdString();
    // The window measured the pane, and the program got that size.
    ASSERT_LT(pane->cols(), 60);
    ASSERT_TRUE(pane->sendInput("stty size\n"));
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("%1 %2").arg(pane->rows()).arg(pane->cols())); }, 5000))
        << screenOf(pane).toStdString();
    // A key typed into the window reaches the program, and its output the window.
    const auto frames = shown->submittedFrames();
    ASSERT_TRUE(pane->sendInput("echo li''ve\n"));
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("\nlive")); }, 5000)) << screenOf(pane).toStdString();
    EXPECT_TRUE(waitFor([&] { return shown->submittedFrames() > frames; }));
    ASSERT_TRUE(waitFor([&] {
        const auto info = sessionNamed(daemon, "live");
        return info && info->attached;
    }));
    delete shown;
    // The daemon still answers, with only its own session.
    EXPECT_TRUE(waitFor([&] { return MuxTest::names(daemon.sessions()) == std::vector<std::string>{"keep"}; }, 5000));
}

// The calls of a session pane's terminal reach the window, as from a program
// of its own: a bell, a user variable and an OSC 52 write to the clipboard.
TEST_F(NativeWindow, AMuxSessionsBellsUserVarsAndClipboardWritesReachTheWindow) {
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "calls")) << daemon.lastError();
    QString error;
    auto [shown, pane] = showSessionAtPrompt(*application, daemon, QStringLiteral("calls"), &error);
    ASSERT_NE(pane, nullptr) << error.toStdString();
    int bells = 0;
    QStringList vars;
    QObject listener;
    QObject::connect(pane, &Pane::bellRang, &listener, [&] { ++bells; });
    QObject::connect(pane, &Pane::userVarUpdated, &listener, [&](const QString&, const QString& value) { vars.append(value); });
    QGuiApplication::clipboard()->setText(QStringLiteral("before"));
    // "done" and "from session" in base64.
    ASSERT_TRUE(pane->sendInput("printf '\\a\\033]1337;SetUserVar=mark=ZG9uZQ==\\a\\033]52;c;ZnJvbSBzZXNzaW9u\\a'\n"));
    EXPECT_TRUE(waitFor([&] { return bells > 0; }, 5000));
    EXPECT_TRUE(waitFor([&] { return vars.contains(QStringLiteral("done")); }, 5000)) << vars.join(QLatin1Char(',')).toStdString();
    EXPECT_TRUE(waitFor([] { return QGuiApplication::clipboard()->text() == QStringLiteral("from session"); }, 5000))
        << QGuiApplication::clipboard()->text().toStdString();
    delete shown;
}

// A session pane has what a pane of the window has, through the entry
// points that use it: the last output in the pager (Action::ShowLastOutput)
// and the regions of shell integration, the status of a program with the
// signal that the tab bar and scripts hear, a shell context, and the
// directory of a shell that reports none, which the daemon reads for a new
// tab.
TEST_F(NativeWindow, AMuxSessionsMarksStatusAndDirectoryReachTheWindow) {
    // The pager of the fixture may be missing: tail is not.
    restartApplication(QStringLiteral("pager.conf"),
        testSettings({{QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("pager"), QStringLiteral("/usr/bin/tail -f")}}}}),
        {QStringLiteral("/bin/cat")});
    if (HasFatalFailure()) return;
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "marks")) << daemon.lastError();
    QString error;
    auto [shown, pane] = showSessionAtPrompt(*application, daemon, QStringLiteral("marks"), &error);
    ASSERT_NE(pane, nullptr) << error.toStdString();
    int statusChanges = 0;
    QObject listener;
    QObject::connect(pane, &Pane::statusChanged, &listener, [&] { ++statusChanges; });
    ASSERT_TRUE(pane->sendInput("cd / && printf '\\033]133;C\\aout1\\nout2\\n\\033]133;D;4\\a\\033]7501;state=working:app=build:progress=30\\a"
                                "\\033]3008;start=c1;type=remote;user=u;hostname=far-away-host\\033\\\\'\n"));
    EXPECT_TRUE(waitFor([&] { return pane->text(AfterglowVt::Terminal::TextScope::LastOutput) == QStringLiteral("out1\nout2\n"); }, 5000))
        << pane->text(AfterglowVt::Terminal::TextScope::LastOutput).toStdString();
    // The status reaches the window as a change, which the tab bar and
    // scripts hear of.
    EXPECT_TRUE(waitFor([&] { return statusChanges > 0; }, 5000));
    const QList<PaneStatusRecord> records = pane->status();
    ASSERT_EQ(records.size(), 1);
    EXPECT_EQ(records[0].app, QStringLiteral("build"));
    EXPECT_EQ(records[0].progress, 30);
    EXPECT_EQ(records[0].state, AfterglowVt::StatusState::Working);
    EXPECT_TRUE(waitFor([&] { return pane->contextKind() == QStringLiteral("remote"); }, 5000)) << pane->contextKind().toStdString();
    // The D mark came in the frame of the status or before it: the printf
    // wrote it first.
    const QVariantList regions = pane->semanticRegions().value(QStringLiteral("regions")).toList();
    EXPECT_TRUE(std::any_of(regions.begin(), regions.end(), [](const QVariant& r) {
        const QVariantMap region = r.toMap();
        return region.value(QStringLiteral("kind")) == QStringLiteral("output") && region.value(QStringLiteral("exit_code")).toInt() == 4;
    })) << QJsonDocument(QJsonArray::fromVariantList(regions)).toJson(QJsonDocument::Compact).toStdString();
    // The shell (ENV=/dev/null, PS1 "$ ") reports no directory: scripts and
    // afterglow ctl get none, and a new tab asks the daemon for the shell's.
    EXPECT_TRUE(pane->knownDirectory().isEmpty()) << pane->knownDirectory().toStdString();
    EXPECT_EQ(pane->workingDirectory(), QStringLiteral("/"));
    SettingsStore settings(directory.filePath(QStringLiteral("marks.conf")));
    Control control(settings, {});
    control.attachWindow(shown);
    quint32 created = 0;
    ASSERT_TRUE(control.newTab(pane->id(), {}, {QStringLiteral("/bin/cat")}, created).isEmpty());
    Pane* added = nullptr;
    for (Pane* p : shown->panes()) if (p->id() == created) added = p;
    ASSERT_NE(added, nullptr);
    EXPECT_TRUE(waitFor([&] { return added->workingDirectory() == QStringLiteral("/"); })) << added->workingDirectory().toStdString();
    // The last output in the pager, as for a pane of the window's own.
    shown->revealPane(pane);
    const int tabs = int(shown->tabs().size());
    ASSERT_TRUE(shown->runAction(pane, Action::ShowLastOutput));
    ASSERT_EQ(int(shown->tabs().size()), tabs + 1);
    Pane* pager = shown->focusedPane();
    ASSERT_NE(pager, pane);
    EXPECT_TRUE(waitFor([&] { return screenOf(pager).contains(QStringLiteral("out2")); }, 5000)) << screenOf(pager).toStdString();
    delete shown;
}

// The window tells the daemon whether the user can see a session pane, and
// the daemon's terminal reports each change to the program (mode 2033): at
// the attach, when another tab shows, and when the pane shows again.
TEST_F(NativeWindow, ASessionPaneTellsItsProgramWhetherItCanBeSeen) {
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    // The program turns the mode on and reads the daemon's first report
    // ("cannot be seen", from before any window), then says so in a file.
    const std::string on = daemon.dir() + "/on";
    auto options = daemon.options(
        {"/bin/sh", "-c",
         "stty raw -echo; printf '\\033[?2033h'; dd bs=1 count=9 2>/dev/null | cat -v; touch '" + on + "'; printf ready; exec cat -v"},
        "seen");
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(QString::fromStdString(on)); }, 5000));
    QString error;
    QPointer<TerminalWindow> shown = showSession(*application, daemon, QStringLiteral("seen"), &error);
    ASSERT_NE(shown, nullptr) << error.toStdString();
    ASSERT_TRUE(waitFor([&] { return shown->isExposed() && shown->submittedFrames() > 0; }));
    Pane* pane = shown->focusedPane();
    ASSERT_NE(pane, nullptr);
    const auto reports = [&](const char* state) { return screenOf(pane).count(QLatin1String(state)); };
    // The program's first report is the daemon's, from before any window
    // ("cannot be seen"); then the window says that it shows the pane.
    ASSERT_TRUE(waitFor([&] { return reports("^[[?999;2n") == 1 && reports("^[[?999;1n") == 1; }, 5000)) << screenOf(pane).toStdString();
    // A tab of its own hides the pane: the switch tells the daemon.
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    Pane* other = shown->openTab(launch, nullptr);
    ASSERT_NE(other, nullptr);
    shown->revealPane(other);
    EXPECT_TRUE(waitFor([&] { return reports("^[[?999;2n") == 2; }, 5000)) << screenOf(pane).toStdString();
    shown->revealPane(pane);
    EXPECT_TRUE(waitFor([&] { return reports("^[[?999;1n") == 2; }, 5000)) << screenOf(pane).toStdString();
    delete shown;
}

// A window that attaches to a session shows what the session holds already:
// its history, the last output, the jumps between prompts. Its status
// records are no news to the window: a Done or a Failed from before the
// attach shows no notification.
TEST_F(NativeWindow, AnAttachShowsTheSessionsStateAndNotifiesNothingOld) {
    RegisteredNotificationServer notices;
    ASSERT_TRUE(notices.registered);
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    // The program writes it all, then says so in a file, before any window
    // comes.
    const std::string written = daemon.dir() + "/written";
    auto options = daemon.options(
        {"/bin/sh", "-c",
         "seq 300; printf '\\033]133;A\\a$ \\033]133;B\\a\\033]133;C\\a'; seq 100; printf '\\033]133;D;4\\a"
         "\\033]7501;state=error:app=build\\a\\033]7501;id=x:state=done:app=test\\a'; touch '" + written + "'; exec /bin/sh"},
        "old");
    options.launch.environment.emplace_back("PS1", "$ ");
    options.launch.environment.emplace_back("ENV", "/dev/null");
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return QFileInfo::exists(QString::fromStdString(written)); }, 5000));
    QString error;
    QPointer<TerminalWindow> shown = showSession(*application, daemon, QStringLiteral("old"), &error);
    ASSERT_NE(shown, nullptr) << error.toStdString();
    ASSERT_TRUE(waitFor([&] { return shown->isExposed() && shown->submittedFrames() > 0; }));
    Pane* pane = shown->focusedPane();
    ASSERT_NE(pane, nullptr);
    EXPECT_TRUE(waitFor([&] {
        const QString output = pane->text(AfterglowVt::Terminal::TextScope::LastOutput);
        return output.startsWith(QStringLiteral("1\n2\n")) && output.endsWith(QStringLiteral("\n100\n"));
    }, 5000)) << pane->text(AfterglowVt::Terminal::TextScope::LastOutput).toStdString();
    EXPECT_TRUE(waitFor([&] { return pane->status().size() == 2; }, 5000));
    // The jump to the prompt before lands on it, in the history that came.
    EXPECT_TRUE(waitFor([&] { return historySize(pane) > 250; }, 5000));
    ASSERT_TRUE(shown->runAction(pane, Action::JumpPromptUp));
    EXPECT_GT(viewportOffset(pane), 0u);
    // The notices arrive in the order sent: none came before this one.
    NotificationCenter::instance().showNotice(nullptr, QStringLiteral("Test"), QStringLiteral("last"));
    ASSERT_TRUE(waitFor([&] { return notices.server.bodies.contains(QStringLiteral("last")); }));
    EXPECT_EQ(notices.server.bodies, QStringList{QStringLiteral("last")});
    delete shown;
}

// A selection in a region that the session's program scrolls moves with its
// text, as in a pane of the window's own: the frames carry the scrolls. The
// extra cursors of the program show too.
TEST_F(NativeWindow, ASelectionInASessionPaneFollowsTheScrollsOfItsProgram) {
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    auto options = daemon.options(
        {"/bin/sh", "-c",
         "stty -echo; printf '\\033[2J\\033[Hzero\\r\\none\\r\\ntwo\\r\\nthree\\r\\n\\033[>29;2:6:3 q'; read a; "
         "printf '\\033[1;1H\\033[M'; read b; printf '\\033[1;4r\\033[1;1H\\033[3M\\033[r'; read c"},
        "scrolls");
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    QString error;
    QPointer<TerminalWindow> shown = showSession(*application, daemon, QStringLiteral("scrolls"), &error);
    ASSERT_NE(shown, nullptr) << error.toStdString();
    ASSERT_TRUE(waitFor([&] { return shown->isExposed() && shown->submittedFrames() > 0; }));
    Pane* pane = shown->focusedPane();
    ASSERT_NE(pane, nullptr);
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).contains(QStringLiteral("three")); }, 5000)) << screenOf(pane).toStdString();
    EXPECT_TRUE(waitFor([&] {
        pane->refreshSnapshot();
        return !pane->snapshot().extraCursors.empty();
    }, 5000));
    pane->startSelection(pane->selectionPointAt(0, 2), AfterglowVt::SelectionMode::Normal, true);
    pane->extendSelection(pane->selectionPointAt(2, 2));
    ASSERT_EQ(pane->selectedText(), QStringLiteral("two"));
    // DL at the first row: "two" moves up a row, and the selection with it.
    ASSERT_TRUE(pane->sendInput("\n"));
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).startsWith(QStringLiteral("one")); }, 5000)) << screenOf(pane).toStdString();
    EXPECT_TRUE(pane->hasSelection());
    EXPECT_EQ(pane->selectedText(), QStringLiteral("two"));
    // A region scroll that pushes it out of the region clears it.
    ASSERT_TRUE(pane->sendInput("\n"));
    ASSERT_TRUE(waitFor([&] { return !screenOf(pane).contains(QStringLiteral("two")); }, 5000)) << screenOf(pane).toStdString();
    EXPECT_FALSE(pane->hasSelection());
    delete shown;
}

// The requests of a session's programs reach the window, and its answers
// reach them back through the daemon: an OSC 52 read gets the clipboard once
// the user allows it. The commands of shell integration come in order, with
// the time that the daemon measured.
TEST_F(NativeWindow, AMuxSessionsReadsAndCommandsReachTheWindowAndBack) {
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "back")) << daemon.lastError();
    QString error;
    auto [shown, pane] = showSessionAtPrompt(*application, daemon, QStringLiteral("back"), &error);
    ASSERT_NE(pane, nullptr) << error.toStdString();
    auto* chrome = shown->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);

    QStringList order;
    qint64 ran = -1;
    QObject listener;
    QObject::connect(pane, &Pane::commandBegan, &listener, [&](const QString&) { order << QStringLiteral("began"); });
    QObject::connect(pane, &Pane::userVarUpdated, &listener, [&](const QString& name, const QString&) { order << name; });
    QObject::connect(pane, &Pane::commandEnded, &listener, [&](const QString&, int, qint64 ms) {
        order << QStringLiteral("ended");
        ran = ms;
    });
    // A command that sets a user variable while it runs, for 300 ms.
    ASSERT_TRUE(pane->sendInput("printf '\\033]133;A\\033\\\\$ \\033]133;B\\033\\\\echo test\\033]133;C\\033\\\\"
                                "\\033]1337;SetUserVar=during=dg==\\a'; sleep 0.3; printf 'result\\033]133;D;0\\033\\\\'\n"));
    ASSERT_TRUE(waitFor([&] { return order.contains(QStringLiteral("began")); }, 5000)) << order.join(QLatin1Char(',')).toStdString();
    // A window that is busy gets the end late: the time is still the daemon's.
    // The stall is far longer than the command, so that a time the window
    // measured fails however slow the machine is.
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    ASSERT_TRUE(waitFor([&] { return order.contains(QStringLiteral("ended")); }, 5000)) << order.join(QLatin1Char(',')).toStdString();
    EXPECT_EQ(order, (QStringList{QStringLiteral("began"), QStringLiteral("during"), QStringLiteral("ended")}));
    EXPECT_GE(ran, 250);
    EXPECT_LT(ran, 1500);

    // The reply goes to the program as its input, which dd saves.
    QGuiApplication::clipboard()->setText(QStringLiteral("for the session"));
    const QString reply = QString::fromStdString(daemon.dir()) + QStringLiteral("/reply");
    AfterglowVt::ClipboardRequest read;
    read.osc52 = true;
    read.osc52Bel = true;
    read.osc52Selection = "c";
    const std::string expected = read.osc52Reply("for the session");
    ASSERT_TRUE(pane->sendInput(QStringLiteral("stty raw -echo; printf '\\033]52;c;?\\a'; dd bs=1 count=%1 of=%2 2>/dev/null; stty sane\n")
                                    .arg(expected.size())
                                    .arg(reply)
                                    .toStdString()));
    ASSERT_TRUE(waitFor([&] { return chrome->clipboardPromptOpen(); }, 5000));
    chrome->answerClipboard(1);
    ASSERT_TRUE(waitFor([&] { return QFileInfo(reply).size() == qint64(expected.size()); }, 5000));
    QFile file(reply);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll().toStdString(), expected);
    delete shown;
}

// A drag waits for the program's answer on the GUI thread
// (Terminal.dragAnswerWait). A session pane's commands come on the session's
// stream, which only that thread reads: the wait reads it, and ends when they
// come.
TEST_F(NativeWindow, AProtocolWaitOfASessionPaneReadsItsStream) {
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "wait")) << daemon.lastError();
    QString error;
    QPointer<TerminalWindow> shown = showSession(*application, daemon, QStringLiteral("wait"), &error);
    ASSERT_NE(shown, nullptr) << error.toStdString();
    Pane* pane = shown->focusedPane();
    ASSERT_NE(pane, nullptr);
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).startsWith(QLatin1Char('$')); }, 5000)) << screenOf(pane).toStdString();
    int commands = 0;
    QObject listener;
    QObject::connect(pane, &Pane::terminalProtocolRequested, &listener, [&](int code, const QByteArray&) { commands += code == 5113; });
    ASSERT_TRUE(pane->sendInput("sleep 0.3; printf '\\033]5113;ac=cancel;id=x\\033\\\\'\n"));
    QElapsedTimer waited;
    waited.start();
    pane->deliverProtocolCommands(4000);
    EXPECT_EQ(commands, 1);
    EXPECT_LT(waited.elapsed(), 3000);
    delete shown;
}

// A clipboard read that waits for the user when the window closes, while the
// session keeps running (Mux.onClose detach), gets the denial: its program
// does not wait for good.
TEST_F(NativeWindow, AReadOfASessionThatOutlivesItsWindowGetsTheDenial) {
    restartApplication(QStringLiteral("mux.conf"),
                       testSettings({{QStringLiteral("Mux"), QJsonObject{{QStringLiteral("onClose"), QStringLiteral("detach")}}}}),
                       {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "kept")) << daemon.lastError();
    QString error;
    QPointer<TerminalWindow> shown = showSession(*application, daemon, QStringLiteral("kept"), &error);
    ASSERT_NE(shown, nullptr) << error.toStdString();
    Pane* pane = shown->focusedPane();
    ASSERT_NE(pane, nullptr);
    auto* chrome = shown->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).startsWith(QLatin1Char('$')); }, 5000)) << screenOf(pane).toStdString();
    const QString reply = QString::fromStdString(daemon.dir()) + QStringLiteral("/reply");
    AfterglowVt::ClipboardRequest read;
    read.osc52 = true;
    read.osc52Bel = true;
    read.osc52Selection = "c";
    const std::string denial = read.refusal("EPERM");
    ASSERT_TRUE(pane->sendInput(QStringLiteral("stty raw -echo; printf '\\033]52;c;?\\a'; dd bs=1 count=%1 of=%2 2>/dev/null; stty sane\n")
                                    .arg(denial.size())
                                    .arg(reply)
                                    .toStdString()));
    ASSERT_TRUE(waitFor([&] { return chrome->clipboardPromptOpen(); }, 5000));
    delete shown;
    ASSERT_TRUE(waitFor([&] { return QFileInfo(reply).size() == qint64(denial.size()); }, 5000));
    QFile file(reply);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll().toStdString(), denial);
}

// The program of the session exits: the pane closes as a pane whose program
// exited, with its status, and the window with it.
TEST_F(NativeWindow, AMuxSessionWhoseProgramExitsClosesItsPane) {
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "ends")) << daemon.lastError();
    QString error;
    QPointer<TerminalWindow> shown = showSession(*application, daemon, QStringLiteral("ends"), &error);
    ASSERT_NE(shown, nullptr) << error.toStdString();
    QPointer<Pane> pane = shown->focusedPane();
    ASSERT_NE(pane, nullptr);
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).startsWith(QLatin1Char('$')); }, 5000)) << screenOf(pane).toStdString();
    int status = -100;
    QString last;
    // The context ends with these locals, also when an ASSERT returns early
    // and the pane closes later.
    QObject context;
    QObject::connect(pane, &Pane::closed, &context, [&, raw = pane.data()] {
        status = raw->exitCode();
        last = screenOf(raw);
    });
    ASSERT_TRUE(pane->sendInput("echo good''bye; exit 3\n"));
    ASSERT_TRUE(waitFor([&] { return !shown; }, 5000));
    EXPECT_EQ(status, 3);
    // The last output reached the window before the exit did.
    EXPECT_TRUE(last.contains(QStringLiteral("\ngoodbye"))) << last.toStdString();
    EXPECT_TRUE(waitFor([&] { return !sessionNamed(daemon, "ends"); }, 5000));
}

namespace {

/// One case of AWindowThatLosesItsSessionLeavesItRunning: a failed check
/// ends this case only.
void loseSession(Application& application, MuxTest::TestDaemon& daemon, bool takeover) {
    const std::string name = takeover ? "taken" : "detached";
    ASSERT_TRUE(shellSession(daemon, name)) << daemon.lastError();
    QString error;
    QPointer<TerminalWindow> shown = showSession(application, daemon, QString::fromStdString(name), &error);
    ASSERT_NE(shown, nullptr) << error.toStdString();
    Pane* pane = shown->focusedPane();
    ASSERT_NE(pane, nullptr);
    ASSERT_TRUE(pane->closeEndsProgram());
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).startsWith(QLatin1Char('$')); }, 5000)) << screenOf(pane).toStdString();
    if (takeover) {
        // This process asks for the session as another window would.
        ASSERT_TRUE(daemon.call(AfterglowMux::Fn::Attach,
                                MuxTest::Json::Array{AfterglowMux::ProtocolVersion, name, MuxTest::Json::Object{{AfterglowMux::AttachOption::Takeover, true}}}))
            << daemon.lastError();
    } else {
        ASSERT_TRUE(daemon.call(AfterglowMux::Fn::Detach, MuxTest::Json::Array{name})) << daemon.lastError();
    }
    ASSERT_TRUE(waitFor([&] { return !shown; }, 5000));
    ASSERT_TRUE(settleQueued());
    EXPECT_TRUE(sessionNamed(daemon, name));
}

} // namespace

// A window that loses its session (a detach, another window's attach) closes
// its pane and kills nothing, also with Mux.onClose kill.
TEST_F(NativeWindow, AWindowThatLosesItsSessionLeavesItRunning) {
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    for (const bool takeover : {false, true}) {
        SCOPED_TRACE(takeover ? "a takeover" : "a detach");
        loseSession(*application, daemon, takeover);
    }
}

// A launch of a session passes Mux.eventQueue of the configuration, in bytes.
TEST_F(NativeWindow, ALaunchOfASessionPassesItsEventQueue) {
    restartApplication(QStringLiteral("queue.conf"), testSettings({{QStringLiteral("Mux"), QJsonObject{{QStringLiteral("eventQueue"), 1}}}}),
                       {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "queue")) << daemon.lastError();
    const Saved runtime("XDG_RUNTIME_DIR", QByteArray::fromStdString(daemon.runtime()));
    const Saved state("XDG_STATE_HOME", QByteArray::fromStdString(daemon.stateHome()));
    const int before = int(TerminalWindow::windows().size());
    // In the test's directory: no program of a test starts in the user's home.
    ASSERT_EQ(application->openLaunch(directory.path(), {}, {{QStringLiteral("session"), QStringLiteral("queue")}}), QString());
    ASSERT_EQ(int(TerminalWindow::windows().size()), before + 1);
    QPointer<TerminalWindow> shown = TerminalWindow::windows().last();
    Pane* const pane = shown->focusedPane();
    ASSERT_NE(pane, nullptr);
    ASSERT_NE(pane->session(), nullptr);
    EXPECT_EQ(pane->session()->eventQueue(), std::size_t(1) << 20);
    delete shown;
}

// A launch with --mux-attach (Application::openLaunch) finds the daemon in
// its environment: the test points that at its own daemon, never the user's.
// A second launch of a session that a window of this process shows brings
// that window to the front, and opens no other.
TEST_F(NativeWindow, ALaunchOfASessionThatThisProcessShowsOpensNoOtherWindow) {
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "launched")) << daemon.lastError();
    const Saved runtime("XDG_RUNTIME_DIR", QByteArray::fromStdString(daemon.runtime()));
    const Saved state("XDG_STATE_HOME", QByteArray::fromStdString(daemon.stateHome()));
    // In the test's directory: no program of a test starts in the user's home.
    const auto launch = [&](const QString& name, bool takeover) {
        return application->openLaunch(directory.path(), {}, {{QStringLiteral("session"), name}, {QStringLiteral("takeover"), takeover}});
    };
    const int before = int(TerminalWindow::windows().size());
    ASSERT_EQ(launch(QStringLiteral("launched"), true), QString());
    ASSERT_EQ(int(TerminalWindow::windows().size()), before + 1);
    QPointer<TerminalWindow> shown = TerminalWindow::windows().last();
    Pane* const sessionPane = shown->focusedPane();
    ASSERT_NE(sessionPane, nullptr);
    ASSERT_TRUE(sessionPane->isSessionPane());
    ASSERT_TRUE(waitFor([&] {
        const auto info = sessionNamed(daemon, "launched");
        return info && info->attached;
    }, 5000));
    // Another tab in front: the launch brings the session's pane back.
    PaneLaunchOptions other;
    other.command = {QStringLiteral("/bin/cat")};
    ASSERT_NE(shown->openTab(other, nullptr), nullptr);
    ASSERT_NE(shown->focusedPane(), sessionPane);
    EXPECT_EQ(launch(QStringLiteral("launched"), false), QString());
    EXPECT_EQ(int(TerminalWindow::windows().size()), before + 1);
    EXPECT_EQ(shown->focusedPane(), sessionPane);
    // A session that does not run: the launch fails, and says why.
    EXPECT_TRUE(launch(QStringLiteral("none"), true).contains(QStringLiteral("no session named 'none'")));
    EXPECT_EQ(int(TerminalWindow::windows().size()), before + 1);
    delete shown;
}

// Input larger than a frame of the attach stream goes in parts, all of them
// in order.
TEST_F(NativeWindow, ALargePasteReachesTheProgramOfASessionWhole) {
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "paste")) << daemon.lastError();
    QString error;
    QPointer<TerminalWindow> shown = showSession(*application, daemon, QStringLiteral("paste"), &error);
    ASSERT_NE(shown, nullptr) << error.toStdString();
    Pane* pane = shown->focusedPane();
    ASSERT_NE(pane, nullptr);
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).startsWith(QLatin1Char('$')); }, 5000)) << screenOf(pane).toStdString();
    const std::string file = daemon.dir() + "/pasted";
    ASSERT_TRUE(pane->sendInput("stty -echo; cat > " + file + "\n"));
    // Lines of 100 bytes, as a terminal in canonical mode takes them: 2000 of
    // them are three parts and more.
    std::string text;
    for (int i = 0; i < 2000; ++i) {
        std::string line = std::to_string(i);
        line.resize(99, '.');
        text += line + '\n';
    }
    ASSERT_TRUE(pane->sendInput(text));
    ASSERT_TRUE(waitFor([&] { return MuxTest::readFile(file).size() == text.size(); }, 10000)) << MuxTest::readFile(file).size();
    EXPECT_TRUE(MuxTest::readFile(file) == text);
    ASSERT_TRUE(pane->sendInput("\x04"));
    delete shown;
}

// With Mux.onClose detach, the session outlives its window.
TEST_F(NativeWindow, AMuxSessionCanOutliveItsWindow) {
    restartApplication(QStringLiteral("mux.conf"),
                       testSettings({{QStringLiteral("Mux"), QJsonObject{{QStringLiteral("onClose"), QStringLiteral("detach")}}}}),
                       {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")});
    if (HasFatalFailure()) return;
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "kept")) << daemon.lastError();
    QString error;
    QPointer<TerminalWindow> shown = showSession(*application, daemon, QStringLiteral("kept"), &error);
    ASSERT_NE(shown, nullptr) << error.toStdString();
    Pane* pane = shown->focusedPane();
    ASSERT_NE(pane, nullptr);
    EXPECT_FALSE(pane->closeEndsProgram());
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).startsWith(QLatin1Char('$')); }, 5000)) << screenOf(pane).toStdString();
    delete shown;
    ASSERT_TRUE(waitFor([&] {
        const auto info = sessionNamed(daemon, "kept");
        return info && !info->attached;
    }, 5000));
}

// Mux.onClose applies live: a pane that opened with kill detaches once the
// configuration says detach.
TEST_F(NativeWindow, MuxOnCloseAppliesToAnOpenSessionPane) {
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "live")) << daemon.lastError();
    QString error;
    QPointer<TerminalWindow> shown = showSession(*application, daemon, QStringLiteral("live"), &error);
    ASSERT_NE(shown, nullptr) << error.toStdString();
    Pane* pane = shown->focusedPane();
    ASSERT_NE(pane, nullptr);
    ASSERT_TRUE(pane->closeEndsProgram());
    ASSERT_TRUE(waitFor([&] { return screenOf(pane).startsWith(QLatin1Char('$')); }, 5000)) << screenOf(pane).toStdString();
    QFile config(directory.filePath(QStringLiteral("afterglow.conf")));
    ASSERT_TRUE(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
    config.write(R"({"Mux": {"onClose": "detach"}})");
    config.close();
    ASSERT_TRUE(shown->runAction(pane, Action::ReloadConfig));
    ASSERT_TRUE(waitFor([&] { return !pane->closeEndsProgram(); }, 5000));
    delete shown;
    ASSERT_TRUE(waitFor([&] {
        const auto info = sessionNamed(daemon, "live");
        return info && !info->attached;
    }, 5000));
}

int main(int argc, char** argv)
{
    // These tests open windows and send desktop-wide D-Bus signals (the
    // launcher entry). Run them only inside the nested desktop of a runner.
    if (qEnvironmentVariableIsEmpty("AFTERGLOW_NATIVE_ISOLATED")) {
        std::fprintf(stderr, "Run these tests through native_window_acceptance.py or the ag-verify-live "
                             "headless_native.py runner, which isolate the compositor and the session bus.\n");
        return 2;
    }
    // A runner started from a pane of the user's Afterglow, a mux session or
    // tmux hands down their identity: the tests' panes, overlays and dialogs
    // must not reach the user's terminal through it. main.cpp unsets the
    // same, and TMUX would make aglow ask the user's tmux.
    for (const char* name : {"AFTERGLOW_SOCKET", "AFTERGLOW_PANE_ID", "AFTERGLOW_SESSION", "TMUX", "XDG_ACTIVATION_TOKEN"})
        qunsetenv(name);
    previousHandler = qInstallMessageHandler(recordQmlErrors);
    // The plugin paths, the layer-shell integration and the names, as afterglow sets them.
    prepareApplication();
    QApplication app(argc, argv);
    if (argc > 1 && QByteArray(argv[1]) == "--drop-target") return runDropTarget(QCoreApplication::arguments());
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "NativeWindowTest.moc"
