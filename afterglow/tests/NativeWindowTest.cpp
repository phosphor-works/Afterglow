// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Application.h"
#include "app/Startup.h"
#include "desktop/DragDrop.h"
#include "desktop/FileTransfer.h"
#include <QMessageBox>
#include <QJsonArray>
#include "chrome/ChromeController.h"
#include "chrome/ChromeLayer.h"
#include "config/Keybinds.h"
#include "KeyMapping.h"
#include "DockArea.h"
#include "control/Control.h"
#include "scripting/ScriptControl.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QInputMethodEvent>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QStyleHints>
#include <QFile>
#include <QFontMetricsF>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
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
#include <QThread>
#include <QThreadPool>
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
#include <gtest/gtest.h>
using namespace Afterglow;
namespace {
void drain(int ms = 100) {
    QElapsedTimer timer; timer.start();
    do { QCoreApplication::processEvents(); QThread::msleep(2); } while (timer.elapsed() < ms);
}
// Waits until @p condition holds or @p ms pass, and returns the condition.
// A PTY echo, a search or a D-Bus round trip has no fixed time.
bool waitFor(const std::function<bool()>& condition, int ms = 3000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < ms) drain(10);
    return condition();
}
// Runs the event loop until every call posted before this one has run:
// queued calls and zero timers run in the order they were posted. A call
// that one of them posts in turn runs in the drain that follows.
bool settleQueued() {
    bool ran = false;
    QTimer::singleShot(0, qApp, [&ran] { ran = true; });
    return waitFor([&ran] { return ran; });
}
// Expects the window to stop asking for frames once nothing moves: 300 ms
// with no frame. Springs advance at most 100 ms of motion per frame, so a
// loaded machine that gives few frames stretches them in wall time. The
// deadline allows for that; an animation that never stops still fails.
void expectIdle(TerminalWindow* window) {
    drain(1500);
    QElapsedTimer deadline; deadline.start();
    auto frames = window->submittedFrames();
    bool idle = false;
    while (!idle && deadline.elapsed() < 5000) {
        frames = window->submittedFrames();
        drain(300);
        idle = window->submittedFrames() == frames;
    }
    EXPECT_TRUE(idle) << "the window still renders with nothing moving";
}
// Renders a frame and waits until it is submitted. A check that nothing
// changed then sees the layout and the chrome of a frame after the change.
bool nextFrame(TerminalWindow* window) {
    const auto frames = window->submittedFrames();
    QEvent update(QEvent::UpdateRequest);
    QCoreApplication::sendEvent(window, &update);
    return waitFor([&] { return window->submittedFrames() > frames; });
}
// QML errors print a warning and leave the test green. The handler keeps them,
// and every test fails that produced one.
std::mutex qmlErrorsMutex;
QStringList qmlErrors;
QtMessageHandler previousHandler = nullptr;
void recordQmlErrors(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const bool fromQml = message.contains(QLatin1String(".qml:"))
        || (context.file && QByteArray(context.file).endsWith(".qml"));
    if (type == QtWarningMsg && fromQml
        && (message.contains(QLatin1String("TypeError")) || message.contains(QLatin1String("ReferenceError"))
            || message.contains(QLatin1String("Cannot assign")))) {
        std::lock_guard lock(qmlErrorsMutex);
        qmlErrors.append(message);
    }
    if (previousHandler) previousHandler(type, context, message);
}
QStringList takeQmlErrors() {
    std::lock_guard lock(qmlErrorsMutex);
    return std::exchange(qmlErrors, {});
}
// The drag and drop tests need real compositor input and kitten.
#define REQUIRE_NATIVE_DND(ready)                                                                                 \
    const QString ready = qEnvironmentVariable("AFTERGLOW_NATIVE_DND_READY");                                    \
    if (ready.isEmpty()) GTEST_SKIP() << "Run through native_window_acceptance.py for real compositor input"
#define REQUIRE_KITTEN()                                                                                          \
    if (QStandardPaths::findExecutable(QStringLiteral("kitten")).isEmpty()) GTEST_SKIP() << "kitten is not installed"
QList<QQuickItem*> visualDescendants(QQuickItem* root) {
    QList<QQuickItem*> result;
    for (auto* child : root->childItems()) {
        result.append(child);
        result.append(visualDescendants(child));
    }
    return result;
}
// An overlay takes the keyboard one event-loop turn after it shows. The
// chrome is made in the background and finished by the first frame that
// needs it, if the background has not finished it yet. The chrome root holds
// the keys after an overlay closed, until the next one takes them.
bool chromeFocusReady(ChromeLayer* layer) {
    auto* focus = layer->focusedItem();
    return focus && focus->hasActiveFocus() && focus->isVisible() && focus != focus->window()->contentItem()
        && focus != layer->rootItem();
}
class NativeWindow : public ::testing::Test {
protected:
    QTemporaryDir directory;
    std::unique_ptr<Application> application;
    TerminalWindow* window = nullptr;
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        takeQmlErrors();
        // No cursor blink, so a window with nothing moving renders no frames.
        application = std::make_unique<Application>(directory.filePath(QStringLiteral("afterglow.conf")),
            QJsonObject{{QStringLiteral("Window"), QJsonObject{{QStringLiteral("paneTitles"), true}}},
                        {QStringLiteral("Terminal"), QJsonObject{{QStringLiteral("cursorBlink"), false}}}});
        WindowOptions options;
        options.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")};
        window = application->newWindow(options);
        ASSERT_NE(window, nullptr);
        window->resize(900, 600);
        window->show();
        ASSERT_TRUE(waitFor([this] { return window->isExposed() && window->submittedFrames() > 0; }));
    }
    void TearDown() override {
        application.reset();
        drain();
        const auto errors = takeQmlErrors();
        EXPECT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
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
    ASSERT_TRUE(waitFor([&] { return chrome->tabBarHeight() > 0; }));
    // The tab bar takes the drag only once the chrome scene, made in the
    // background, exists and has drawn the two tabs. Before that the first
    // drag below would pass for having detached nothing.
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(waitFor([&] { return layer->rootItem() != nullptr; }));
    ASSERT_TRUE(nextFrame(window));
    auto pointer = [&](QEvent::Type type, QPointF position, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent event(type, position, QPointF(window->mapToGlobal(position.toPoint())), button, held, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
    };
    const QPointF start(50, chrome->tabBarHeight()/2.0), outside(50, 150);
    pointer(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    pointer(QEvent::MouseMove, outside, Qt::NoButton, Qt::LeftButton);
    pointer(QEvent::MouseMove, start, Qt::NoButton, Qt::LeftButton);
    pointer(QEvent::MouseButtonRelease, start, Qt::LeftButton, Qt::NoButton);
    // A detach would run from a call that the release queued.
    ASSERT_TRUE(settleQueued());
    ASSERT_EQ(original->parent(), window);
    ASSERT_EQ(window->tabs().size(), 2);
    pointer(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    pointer(QEvent::MouseMove, outside, Qt::NoButton, Qt::LeftButton);
    pointer(QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
    ASSERT_TRUE(waitFor([&] { return original->parent() != window; }));
    auto* detached = qobject_cast<TerminalWindow*>(original->parent());
    ASSERT_NE(detached, nullptr);
    EXPECT_EQ(split->parent(), detached);
    EXPECT_EQ(detached->tabIdOf(original), tabId);
    EXPECT_EQ(window->panes(), QList<Pane*>{other});
    original->sendInput("printf 'detached-process-alive\\n'\n");
    EXPECT_TRUE(waitFor([&] {
        return original->text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("detached-process-alive"));
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

    application.reset();
    chrome = nullptr;
    application = std::make_unique<Application>(directory.filePath(QStringLiteral("single-tab.conf")),
        QJsonObject{{QStringLiteral("Window"), QJsonObject{{QStringLiteral("tabBar"), QStringLiteral("always")}}}});
    WindowOptions singleOptions;
    singleOptions.command = {QStringLiteral("/bin/cat")};
    window = application->newWindow(singleOptions);
    ASSERT_NE(window, nullptr);
    window->resize(900, 600);
    window->show();
    chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    ASSERT_TRUE(waitFor([&] { return window->isExposed() && chrome->tabBarHeight() > 0; }));
    QPointer<Pane> lastPane = window->focusedPane();
    QPointer<TerminalWindow> source = window;
    pointer(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    pointer(QEvent::MouseMove, QPointF(-50,150), Qt::NoButton, Qt::LeftButton);
    pointer(QEvent::MouseButtonRelease, QPointF(-50,150), Qt::LeftButton, Qt::NoButton);
    // The window that the last tab left goes with deleteLater, which only
    // a running event loop or an explicit send delivers.
    EXPECT_TRUE(waitFor([&] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return source.isNull();
    }));
    window = source.data();
    ASSERT_FALSE(lastPane.isNull());
    ASSERT_NE(qobject_cast<TerminalWindow*>(lastPane->parent()), nullptr);
    EXPECT_FALSE(lastPane->isHeld());
}

class NativeRenderer : public ::testing::TestWithParam<bool> {};
TEST_P(NativeRenderer, SurvivesWindowTransferResizeAndSourceDestruction) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    Application application(directory.filePath(QStringLiteral("mesh.conf")),
        QJsonObject{{QStringLiteral("Shaders"), QJsonObject{{QStringLiteral("background"), GetParam() ? QJsonArray{QStringLiteral("mesh-reactor")} : QJsonArray{}}}},
                    {QStringLiteral("Audio"), QJsonObject{{QStringLiteral("enabled"), false}}},
                    {QStringLiteral("Window"), QJsonObject{{QStringLiteral("backgroundOpacity"), 0.9}}}});
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
    ASSERT_TRUE(waitFor([&] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return source.isNull();
    }));
    ASSERT_TRUE(pane);
    auto* destination = qobject_cast<TerminalWindow*>(pane->parent());
    ASSERT_TRUE(destination);
    ASSERT_TRUE(waitFor([&] { return destination->submittedFrames() > 0; }));
    EXPECT_TRUE(destination->isExposed());
    EXPECT_TRUE(survivor->isExposed());
    const auto before = survivor->submittedFrames();
    const auto destinationBefore = destination->submittedFrames();
    for (int i = 0; i < 8; ++i) {
        destination->resize(i % 2 ? QSize(500, 350) : QSize(300, 200));
        QEvent update(QEvent::UpdateRequest);
        QCoreApplication::sendEvent(destination, &update);
        QCoreApplication::sendEvent(survivor, &update);
        drain(50);
    }
    EXPECT_TRUE(waitFor([&] { return destination->submittedFrames() > destinationBefore; }));
    EXPECT_TRUE(waitFor([&] { return survivor->submittedFrames() > before; }));
}

INSTANTIATE_TEST_SUITE_P(Shaders, NativeRenderer, ::testing::Bool());

TEST_F(NativeWindow, FailedLaunchDoesNotAddPaneOrTab) {
    const int panes = window->panes().size(), tabs = window->tabs().size();
    PaneLaunchOptions options; options.command = {QStringLiteral("/nonexistent/afterglow-test-program")};
    QString error;
    EXPECT_EQ(window->openTab(options, &error), nullptr);
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(window->tabs().size(), tabs);
    EXPECT_EQ(window->splitPane(window->focusedPane(), SplitTree::Direction::Left, options, &error), nullptr);
    EXPECT_EQ(window->panes().size(), panes);
}
TEST_F(NativeWindow, LaunchOptionsAndClosedIdentity) {
    SettingsStore settings(directory.filePath(QStringLiteral("control.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    auto* original = window->focusedPane();
    quint32 created = 0;
    const QVariantMap env{{QStringLiteral("AFTERGLOW_LAUNCH_TEST"), QStringLiteral("native-options")},
                          {QStringLiteral("AFTERGLOW_REMOVE_TEST"), false}};
    const QVariantMap options{{QStringLiteral("cwd"), directory.path()}, {QStringLiteral("env"), env},
                              {QStringLiteral("size"), .25}};
    qputenv("AFTERGLOW_REMOVE_TEST", "inherited");
    const QStringList command{QStringLiteral("/bin/sh"), QStringLiteral("-c"),
        QStringLiteral("printf '%s|%s\\n' \"$AFTERGLOW_LAUNCH_TEST\" \"${AFTERGLOW_REMOVE_TEST-unset}\"; exec cat")};
    const auto error = control.split(original->id(), QStringLiteral("left"), command, options, created);
    qunsetenv("AFTERGLOW_REMOVE_TEST");
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_NE(created, 0u);
    auto* added = window->focusedPane();
    EXPECT_EQ(added->id(), created);
    EXPECT_TRUE(waitFor([&] {
        return added->text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("native-options|unset"));
    }));
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
        // wait for the label to reach the zoomed size.
        // The labels change at the next frame: until then they are the last
        // zoom's, which also had one label as wide as the window.
        ASSERT_TRUE(waitFor([&] {
            return chrome->paneTitles().size() == 1
                && chrome->paneTitles().front().toMap().value(QStringLiteral("title")).toString() == pane->title()
                && chrome->paneTitles().front().toMap().value(QStringLiteral("width")).toDouble() > window->width() * 0.9;
        }));
        const auto entry = chrome->paneTitles().front().toMap();
        EXPECT_EQ(entry.value(QStringLiteral("title")).toString(), pane->title());
        // The zoomed pane fills the window, the label's rectangle with it.
        EXPECT_GT(entry.value(QStringLiteral("width")).toDouble(), window->width() * 0.9);
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
        return first->text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("original-pane-line-one"));
    }));
    EXPECT_FALSE(second->text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("original-pane-line-one")));
}
TEST_F(NativeWindow, PasteConfirmsRawLineTerminatorsAndHonorsBracketedMode) {
    auto* pane = window->focusedPane();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    for (const auto& text : {QStringLiteral("command\n"), QStringLiteral("command\r"),
                            QStringLiteral("first\rsecond"), QStringLiteral("command\r\n")}) {
        QGuiApplication::clipboard()->setText(text);
        ASSERT_TRUE(window->runAction(pane, Action::Paste));
        EXPECT_TRUE(chrome->pasteConfirmOpen());
        chrome->answerPaste(false);
        EXPECT_FALSE(chrome->pasteConfirmOpen());
    }
    pane->withTerminal([](AfterglowVt::Terminal& terminal) { terminal.feed("\x1b[?2004h"); });
    QGuiApplication::clipboard()->setText(QStringLiteral("bracketed\n"));
    ASSERT_TRUE(window->runAction(pane, Action::Paste));
    EXPECT_FALSE(chrome->pasteConfirmOpen());
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
    ASSERT_TRUE(waitFor([&] {
        if (!layer->rootItem()) return false;
        const auto items = visualDescendants(layer->rootItem());
        return std::any_of(items.begin(), items.end(), [&title](const QQuickItem* item) {
            return item->property("text").toString() == title && item->property("textFormat").isValid();
        });
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
    int literalLabels = 0;
    for (auto* item : visualDescendants(layer->rootItem())) {
        if (item->property("text").toString() == title && item->property("textFormat").isValid()) {
            EXPECT_EQ(item->property("textFormat").toInt(), 0); // Text.PlainText
            EXPECT_TRUE(drawsLiterally(item));
            ++literalLabels;
        }
    }
    EXPECT_GT(literalLabels, 0);
    auto* chrome = window->findChild<ChromeController*>();
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
    EXPECT_TRUE(waitFor([&] { return window->findChild<ChromeController*>()->paneTitles().size() == 3; }));
    EXPECT_EQ(window->focusedPane(), first);
    ASSERT_TRUE(window->runAction(first, Action::RotatePanesCounterclockwise));
    EXPECT_TRUE(waitFor([&] { return window->accessiblePaneRect(first) == slots[0]; }));
}

TEST_F(NativeWindow, InitialMenuUpSelectsLastItem) {
    auto* chrome = window->findChild<ChromeController*>();
    QVariantList entries;
    for (int i = 0; i < 3; ++i)
        entries.append(QVariantMap{{QStringLiteral("id"), 40000 + i}, {QStringLiteral("title"), QString::number(i)}});
    chrome->openContextMenu({10, 10}, entries);
    auto* layer = window->findChild<ChromeLayer*>();
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

TEST_F(NativeWindow, LazyChromeInitializesOverlayFocusAndTitle) {
    // Each overlay opens after the scene is made, and while it is still being
    // made in the background: the overlay is then open when the scene completes.
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
        if (settled) {
            EXPECT_TRUE(waitFor([&] { return layer->rootItem() != nullptr; }));
        }
        auto* chrome = fresh->findChild<ChromeController*>();
        if (kind == QLatin1String("title")) chrome->askTitle(QStringLiteral("Title"), QStringLiteral("existing title"));
        else if (kind == QLatin1String("palette")) chrome->setPaletteOpen(true);
        else if (kind == QLatin1String("search")) chrome->setSearchOpen(true);
        else if (kind == QLatin1String("paste")) chrome->askPaste(QStringLiteral("one\ntwo"), 2);
        else if (kind == QLatin1String("clipboard")) chrome->askClipboard(QStringLiteral("Read clipboard"), QString());
        else if (kind == QLatin1String("script"))
            chrome->setScriptDialog({{QStringLiteral("open"), true}, {QStringLiteral("kind"), QStringLiteral("prompt")},
                                     {QStringLiteral("title"), QStringLiteral("Script")}});
        else chrome->askConfirm(QStringLiteral("Confirm"), QStringLiteral("Body"), QStringLiteral("Accept"));
        const auto ready = [&] {
            auto* focus = layer->focusedItem();
            return focus && focus->hasActiveFocus() && focus->isVisible()
                && focus != focus->window()->contentItem();
        };
        QElapsedTimer deadline;
        deadline.start();
        while (!ready() && deadline.elapsed() < 2000) {
            // Run queued QML initialization and focus callbacks on real event-loop turns.
            QEventLoop loop;
            QTimer::singleShot(20, &loop, &QEventLoop::quit);
            loop.exec();
        }
        EXPECT_TRUE(ready());
        if (!ready()) continue;
        EXPECT_TRUE(layer->focusedItem()->isVisible());
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
    application = std::make_unique<Application>(directory.filePath(QStringLiteral("afterglow.conf")), QJsonObject{});
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
    // Terminal status originates on the PTY thread; its GUI receiver takes the pane mutex.
    std::thread statusOutput([pane = contributor->focusedPane()] {
        pane->withTerminal([](AfterglowVt::Terminal& terminal) {
            terminal.feed("\x1b]7501;state=working:progress=50\x1b\\");
        });
    });
    statusOutput.join();
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
        return pane->text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("native-ime-accepted"));
    }));
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
    ASSERT_TRUE(waitFor([&] { return root->focusChild() != nullptr && root->focusChild()->state().editable; }));
    EXPECT_GE(root->childCount(), 2);
    EXPECT_EQ(window->accessibleChrome()->parent(), root);
    EXPECT_GT(window->accessibleChrome()->childCount(), 0);
    auto* focus = root->focusChild();
    ASSERT_NE(focus, nullptr);
    EXPECT_TRUE(focus->state().editable);
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
    AccessibleEvents() {
        current() = this;
        QAccessible::installUpdateHandler([](QAccessibleEvent* event) {
            QAccessibleInterface* iface = event->accessibleInterface();
            current()->events.append({event->type(), iface, iface && iface->isValid()});
            if (event->type() == QAccessible::ObjectDestroyed && iface && iface->isValid() && iface->role() == QAccessible::Button)
                if (QAccessibleInterface* parent = iface->parent()) current()->parentsOfRemoved.append(QAccessible::uniqueId(parent));
        });
    }
    ~AccessibleEvents() { QAccessible::installUpdateHandler(nullptr); current() = nullptr; }
    int count(QAccessible::Event type, QAccessibleInterface* iface) const {
        return int(std::count_if(events.cbegin(), events.cend(), [&](const Event& e) { return e.type == type && e.iface == iface; }));
    }
};
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
    EXPECT_TRUE(actions->keyBindingsForAction(QStringLiteral("next_tab")).isEmpty());

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

    // The menu opens at the pane's cursor, which a pointer need not be near.
    window->revealPane(other);
    ASSERT_TRUE(nextFrame(window));
    ASSERT_FALSE(chrome->contextMenuOpen());
    actions->doAction(QAccessibleActionInterface::showMenuAction());
    ASSERT_TRUE(chrome->contextMenuOpen());
    EXPECT_EQ(window->focusedPane(), original);
    const QPointF at(chrome->contextMenu().value(QStringLiteral("x")).toDouble(), chrome->contextMenu().value(QStringLiteral("y")).toDouble());
    // While the menu holds the keyboard, no action runs: not another
    // menu, not a split, not a focus change.
    QAccessible::queryAccessibleInterface(other)->actionInterface()->doAction(QStringLiteral("Split Down"));
    QAccessible::queryAccessibleInterface(other)->actionInterface()->doAction(QAccessibleActionInterface::setFocusAction());
    ASSERT_TRUE(settleQueued());
    EXPECT_EQ(window->panes().size(), 2);
    EXPECT_EQ(window->focusedPane(), original);
    EXPECT_FALSE(accessible->state().focused); // the menu has the focus
    chrome->closeContextMenu();
    // The open menu answers the input method itself.
    ASSERT_TRUE(waitFor([&] { return !chrome->wantsKeyboard(); }));
    // The pane has the focus again: it was the menu that held it.
    EXPECT_EQ(accessible->state().focused, window->isActive());
    QInputMethodQueryEvent query(Qt::ImCursorRectangle);
    QCoreApplication::sendEvent(window, &query);
    const QRectF cursor = query.value(Qt::ImCursorRectangle).toRectF();
    EXPECT_TRUE(cursor.contains(at)) << at.x() << "," << at.y() << " outside " << cursor.x() << "," << cursor.y() << " "
                                     << cursor.width() << "x" << cursor.height();

    // The focus action moves the keyboard to the pane.
    QAccessible::queryAccessibleInterface(other)->actionInterface()->doAction(QAccessibleActionInterface::setFocusAction());
    EXPECT_EQ(window->focusedPane(), other);

    // Tokens and the palette's other titles run nothing: not the window's
    // close, not the tab's.
    // One at a time: a close confirmation from one would make the rest
    // refuse for another reason.
    const QPointer<TerminalWindow> alive = window;
    for (const QString& name : {QStringLiteral("close_window"), Keybinds::title(Action::CloseWindow), Keybinds::title(Action::CloseTab),
                                Keybinds::title(Action::CommandPalette), Keybinds::title(Action::PaneOverview)}) {
        SCOPED_TRACE(name.toStdString());
        actions->doAction(name);
        ASSERT_TRUE(settleQueued());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ASSERT_TRUE(alive);
        EXPECT_FALSE(chrome->wantsKeyboard());
        EXPECT_TRUE(window->isVisible());
        EXPECT_EQ(window->panes().size(), 2);
        EXPECT_EQ(window->tabs().size(), 1);
    }
}
TEST_F(NativeWindow, AccessibleDockItemsAreButtonsThatSendTheirClick) {
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    auto dock = [&](const QVariantList& line) {
        return window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("bar")},
                                                             {QStringLiteral("edge"), QStringLiteral("bottom")}, {QStringLiteral("size"), 1},
                                                             {QStringLiteral("lines"), QVariantList{line}}});
    };
    auto run = [](const QString& text, const QString& item = {}) {
        QVariantMap map{{QStringLiteral("text"), text}};
        if (!item.isEmpty()) map.insert(QStringLiteral("item"), item);
        return QVariant(map);
    };
    // Two runs of one item make one button.
    ASSERT_TRUE(dock({run(QStringLiteral("["), QStringLiteral("build")), run(QStringLiteral("Build]"), QStringLiteral("build")),
                      run(QStringLiteral(" | ")), run(QStringLiteral("Deploy"), QStringLiteral("deploy"))}).isEmpty());
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
    ASSERT_TRUE(dock({run(QStringLiteral("Building"), QStringLiteral("build")), run(QStringLiteral(" | ")),
                      run(QStringLiteral("Deploy"), QStringLiteral("deploy"))}).isEmpty());
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
    ASSERT_TRUE(dock({run(QStringLiteral("Deploy"), QStringLiteral("deploy"))}).isEmpty());
    EXPECT_EQ(QAccessible::accessibleInterface(buildId), nullptr);
    EXPECT_EQ(QAccessible::accessibleInterface(deployId), nullptr);
    const auto removed = std::count_if(heard.events.cbegin(), heard.events.cend(), [](const auto& e) {
        return e.type == QAccessible::ObjectDestroyed && e.valid;
    });
    EXPECT_EQ(removed, 2);
    EXPECT_EQ(heard.count(QAccessible::ObjectReorder, area), 1);
    ASSERT_EQ(area->childCount(), 1);
    deploy = area->child(0);
    EXPECT_EQ(deploy->text(QAccessible::Name), QStringLiteral("Deploy"));
    deploy->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
    ASSERT_EQ(clicks.size(), 2);
    EXPECT_EQ(clicks[1].click[QStringLiteral("item")], QStringLiteral("deploy"));

    // Without items, the area is text again.
    heard.events.clear();
    ASSERT_TRUE(dock({run(QStringLiteral("Deploy done"))}).isEmpty());
    EXPECT_EQ(area->role(), QAccessible::StaticText);
    EXPECT_EQ(heard.count(QAccessible::RoleChanged, area), 1);
    ASSERT_TRUE(dock({run(QStringLiteral("Deploy"), QStringLiteral("deploy"))}).isEmpty());
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
class NotificationServer : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    int count = 0, closes = 0;
    uint next = 0;
    QList<QSize> images;
    QString appName, appIcon, desktopEntry;
public Q_SLOTS:
    uint Notify(const QString& name, uint replaces, const QString& icon, const QString&, const QString&,
                const QStringList&, const QVariantMap& hints, int) {
        ++count;
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
TEST_F(NativeWindow, NotificationReplacementAndPaneScopedImageCache) {
    NotificationServer server;
    auto bus = QDBusConnection::sessionBus();
    ASSERT_TRUE(bus.registerService(QStringLiteral("org.freedesktop.Notifications")));
    const auto unregisterService = qScopeGuard([&bus] { bus.unregisterService(QStringLiteral("org.freedesktop.Notifications")); });
    ASSERT_TRUE(bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &server, QDBusConnection::ExportAllSlots));
    const auto unregisterObject = qScopeGuard([&bus] { bus.unregisterObject(QStringLiteral("/org/freedesktop/Notifications")); });
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
    // Past the 200 ms expiry of the notice that was replaced.
    drain(250);
    EXPECT_EQ(server.closes, 0);
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
    window->showFullScreen();
    // The marker below holds positions in the full-screen window.
    ASSERT_TRUE(waitFor([&] { return window->isExposed() && window->size() == window->screen()->size(); }));
    const QString path = directory.filePath(QString::fromUtf8("drag's café.txt"));
    QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.close();
    DragSender sender; sender.url = QUrl::fromLocalFile(path);
    sender.pressedMarker = ready + QStringLiteral(".pressed");
    window->installEventFilter(&sender);
    QFile marker(ready); ASSERT_TRUE(marker.open(QIODevice::WriteOnly));
    const QPoint from = window->mapToGlobal(QPoint(window->width() / 4, window->height() / 3));
    const QPoint to = window->mapToGlobal(QPoint(window->width() * 3 / 4, window->height() * 2 / 3));
    marker.write(QByteArray::number(from.x()) + " " + QByteArray::number(from.y()) + " "
                 + QByteArray::number(to.x()) + " " + QByteArray::number(to.y()));
    marker.close();
    QElapsedTimer timer; timer.start();
    while (!sender.completed && timer.elapsed() < 15000) drain(20);
    window->removeEventFilter(&sender);
    EXPECT_TRUE(sender.started);
    ASSERT_TRUE(sender.completed);
    EXPECT_EQ(sender.result, Qt::CopyAction);
    ASSERT_TRUE(window->focusedPane()->sendInput("\n"));
    waitFor([&] { return window->focusedPane()->text(AfterglowVt::Terminal::TextScope::Screen).contains(QString::fromUtf8("café.txt")); });
    const auto text = window->focusedPane()->text(AfterglowVt::Terminal::TextScope::Screen);
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
    original->fileTransfers()->handle("ac=send;id=moved");
    QList<QPointer<QMessageBox>> prompts;
    const auto rejectAll = qScopeGuard([&prompts] {
        for (const auto& box : prompts)
            if (box) box->reject();
        drain(50);
    });
    waitFor([&] {
        prompts.clear();
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible() && box->windowTitle() == QStringLiteral("File transfer")) prompts.append(box);
        return !prompts.isEmpty();
    });
    ASSERT_EQ(prompts.size(), 1);
    EXPECT_EQ(prompts.front()->windowHandle()->transientParent(), destination);
}

TEST_F(NativeWindow, KittyDragAndDropClient) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_KITTEN();
    const QString source = directory.filePath(QStringLiteral("source.txt"));
    const QString destination = directory.filePath(QStringLiteral("dropped.txt"));
    QFile file(source); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.write("Native Kitty drag and drop\n"); file.close();
    PaneLaunchOptions options;
    options.command = {QStringLiteral("kitten"), QStringLiteral("dnd"), QStringLiteral("--drag"), QStringLiteral("text/plain:") + source,
                       QStringLiteral("--drop"), QStringLiteral("text/plain:") + destination, QStringLiteral("--drop-anywhere=copy")};
    // kitten makes a temporary directory where it runs: keep it in the fixture's.
    options.cwd = directory.path();
    auto* pane = window->openTab(options, nullptr); ASSERT_NE(pane, nullptr);
    PaneLaunchOptions receiverOptions;
    receiverOptions.command = {QStringLiteral("kitten"), QStringLiteral("dnd"), QStringLiteral("--drop"),
                               QStringLiteral("text/plain:") + destination, QStringLiteral("--drop-anywhere=copy")};
    receiverOptions.cwd = directory.path();
    auto* receiver = window->splitPane(pane, SplitTree::Direction::Right, receiverOptions, nullptr);
    ASSERT_NE(receiver, nullptr);
    window->showFullScreen();
    QElapsedTimer timer; timer.start();
    while (!pane->dragDrop()->offersDrags() && timer.elapsed() < 5000) drain(20);
    ASSERT_TRUE(pane->dragDrop()->offersDrags()) << pane->text(AfterglowVt::Terminal::TextScope::Screen).toStdString();
    ASSERT_TRUE(receiver->dragDrop()->acceptsDrops());
    bool complete = false; QByteArray protocol;
    // Made after the locals that its connections write to, so that the
    // connections end before them: the window outlives the test body.
    QObject listener;
    QObject::connect(pane->dragDrop(), &DragDrop::reply, &listener, [&](const QByteArray& bytes) {
        protocol += bytes;
        if (bytes.contains("t=e") && bytes.contains("x=4")) complete = true;
    });
    const auto incoming = QObject::connect(pane, &Pane::terminalProtocolRequested, &listener, [&](int code, const QByteArray& bytes) { if (code == 72) protocol += "CLIENT: " + bytes + "\n"; });
    PressMarker input; input.path = ready + QStringLiteral(".pressed"); window->installEventFilter(&input);
    QFile marker(ready); ASSERT_TRUE(marker.open(QIODevice::WriteOnly));
    const QPoint from = window->mapToGlobal(QPoint(window->width() / 4, window->height() / 3));
    const QPoint to = window->mapToGlobal(QPoint(window->width() * 3 / 4, window->height() * 2 / 3));
    marker.write(QByteArray::number(from.x()) + " " + QByteArray::number(from.y()) + " " + QByteArray::number(to.x()) + " " + QByteArray::number(to.y())); marker.close();
    timer.restart(); while (!complete && timer.elapsed() < 15000) drain(20);
    window->removeEventFilter(&input);
    QObject::disconnect(incoming);
    EXPECT_TRUE(complete) << protocol.toStdString() << pane->text(AfterglowVt::Terminal::TextScope::Screen).toStdString();
    QFile result(destination); ASSERT_TRUE(result.open(QIODevice::ReadOnly)) << protocol.toStdString();
    EXPECT_EQ(result.readAll(), QByteArray("Native Kitty drag and drop\n"));
}


TEST_F(NativeWindow, KittyRemoteDragDirectory) {
    REQUIRE_NATIVE_DND(ready);
    REQUIRE_KITTEN();
    const QString source = directory.filePath(QStringLiteral("remote"));
    const QString destination = directory.filePath(QStringLiteral("received"));
    ASSERT_TRUE(QDir().mkpath(source + QStringLiteral("/nested")));
    ASSERT_TRUE(QDir().mkpath(destination));
    QFile file(source + QStringLiteral("/nested/file")); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.write("remote file data"); file.close();
    QFile empty(source + QStringLiteral("/empty")); ASSERT_TRUE(empty.open(QIODevice::WriteOnly)); empty.close();
    ASSERT_EQ(::symlink("nested/file", QFile::encodeName(source + QStringLiteral("/link")).constData()), 0);
    PaneLaunchOptions options;
    options.command = {QStringLiteral("kitten"), QStringLiteral("dnd"), source};
    // kitten makes a temporary directory where it runs: keep it in the fixture's.
    options.cwd = directory.path();
    auto* pane = window->openTab(options, nullptr); ASSERT_NE(pane, nullptr);
    PaneLaunchOptions receiverOptions;
    receiverOptions.command = {QStringLiteral("kitten"), QStringLiteral("dnd"), QStringLiteral("--drop-dest"), destination, QStringLiteral("--drop-anywhere=copy")};
    receiverOptions.cwd = directory.path();
    auto* receiver = window->splitPane(pane, SplitTree::Direction::Right, receiverOptions, nullptr);
    ASSERT_NE(receiver, nullptr);
    window->showFullScreen();
    QElapsedTimer timer; timer.start();
    while (!pane->dragDrop()->offersDrags() && timer.elapsed() < 5000) drain(20);
    ASSERT_TRUE(pane->dragDrop()->offersDrags()) << pane->text(AfterglowVt::Terminal::TextScope::Screen).toStdString();
    ASSERT_TRUE(receiver->dragDrop()->acceptsDrops());
    pane->dragDrop()->handle("t=o:x=1;1:remote-test-machine");
    bool complete = false; QByteArray protocol;
    // Made after the locals that its connections write to, so that the
    // connections end before them: the window outlives the test body.
    QObject listener;
    QObject::connect(pane->dragDrop(), &DragDrop::reply, &listener, [&](const QByteArray& bytes) {
        protocol += bytes;
        if (bytes.contains("t=e") && bytes.contains("x=4")) complete = true;
    });
    const auto incoming = QObject::connect(pane, &Pane::terminalProtocolRequested, &listener, [&](int code, const QByteArray& bytes) { if (code == 72) protocol += "CLIENT: " + bytes + "\n"; });
    PressMarker input; input.path = ready + QStringLiteral(".pressed"); window->installEventFilter(&input);
    QFile marker(ready); ASSERT_TRUE(marker.open(QIODevice::WriteOnly));
    const QPoint from = window->mapToGlobal(QPoint(window->width() / 4, window->height() / 3));
    const QPoint to = window->mapToGlobal(QPoint(window->width() * 3 / 4, window->height() * 2 / 3));
    marker.write(QByteArray::number(from.x()) + " " + QByteArray::number(from.y()) + " " + QByteArray::number(to.x()) + " " + QByteArray::number(to.y())); marker.close();
    timer.restart(); while (!complete && timer.elapsed() < 15000) drain(20);
    window->removeEventFilter(&input);
    QObject::disconnect(incoming);
    EXPECT_TRUE(complete) << protocol.toStdString() << pane->text(AfterglowVt::Terminal::TextScope::Screen).toStdString();
    QFile result(destination + QStringLiteral("/remote/nested/file")); ASSERT_TRUE(result.open(QIODevice::ReadOnly)) << protocol.toStdString();
    EXPECT_EQ(result.readAll(), QByteArray("remote file data"));
    EXPECT_TRUE(QFileInfo::exists(destination + QStringLiteral("/remote/empty")));
    EXPECT_TRUE(QFileInfo(destination + QStringLiteral("/remote/link")).isSymLink());
}


TEST_F(NativeWindow, ScriptQueriesAreBoundedAndRespectTextPolicy) {
    SettingsStore settings(directory.filePath(QStringLiteral("query.conf")));
    Control control(settings, {}); control.attachWindow(window);
    auto* pane = window->focusedPane();
    pane->withTerminal([](AfterglowVt::Terminal& terminal) {
        terminal.feed("\x1b[2J\x1b[H\x1b]133;A\x1b\\$ \x1b]133;B\x1b\\echo test\x1b]133;C\x1b\\result\x1b]133;D;0\x1b\\");
    });
    pane->startSelection(pane->selectionPointAt(0,0),AfterglowVt::SelectionMode::Normal,true);
    pane->extendSelection(pane->selectionPointAt(5,0));
    QVariantMap data;
    ASSERT_TRUE(control.readTextBounded(pane->id(),QStringLiteral("selection"),3,data).isEmpty());
    EXPECT_TRUE(data[QStringLiteral("available")].toBool());
    EXPECT_TRUE(data[QStringLiteral("truncated")].toBool());
    EXPECT_LE(data[QStringLiteral("text")].toString().toUtf8().size(),3);
    ASSERT_TRUE(control.semanticRegions(pane->id(),data).isEmpty());
    const auto regions=data[QStringLiteral("regions")].toList();
    ASSERT_GE(regions.size(),3);
    QString output;
    for(const auto& item:regions) if(item.toMap()[QStringLiteral("kind")]==QStringLiteral("output"))
        output=item.toMap()[QStringLiteral("id")].toString();
    ASSERT_FALSE(output.isEmpty());
    ASSERT_TRUE(control.readRegion(pane->id(),output,3,data).isEmpty());
    EXPECT_EQ(data[QStringLiteral("text")].toString(),QStringLiteral("res"));
    EXPECT_TRUE(data[QStringLiteral("truncated")].toBool());
    SettingsStore denied(directory.filePath(QStringLiteral("denied.conf")),
        QJsonObject{{QStringLiteral("Scripts"),QJsonObject{{QStringLiteral("allowReadText"),false}}}});
    Control blocked(denied,{}); blocked.attachWindow(window);
    EXPECT_FALSE(blocked.readTextBounded(pane->id(),QStringLiteral("screen"),100,data).isEmpty());
    EXPECT_FALSE(blocked.readTextBounded(pane->id(),QStringLiteral("selection"),100,data).isEmpty());
    EXPECT_FALSE(blocked.semanticRegions(pane->id(),data).isEmpty());
    EXPECT_FALSE(blocked.readRegion(pane->id(),output,100,data).isEmpty());
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
    drain();
    auto press = [&](int key, const QString& text = {}) {
        QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier, text);
        QCoreApplication::sendEvent(window, &down);
        QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier, text);
        QCoreApplication::sendEvent(window, &up);
        drain();
    };
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_TRUE(window->runAction(first, Action::TogglePaneZoom));
    drain();
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    drain();
    ASSERT_TRUE(window->overviewOpen());
    EXPECT_EQ(window->overviewHighlight(), first);
    EXPECT_TRUE(chrome->keyMode().startsWith(QStringLiteral("Panes")));
    // The titles follow the frame that draws the cards, which has no fixed time.
    EXPECT_TRUE(waitFor([&] { return chrome->paneTitles().isEmpty(); }));
    // Every pane is a card while the overview is open, zoom or not.
    EXPECT_FALSE(window->accessiblePaneRect(third).isEmpty());
    press(Qt::Key_Right);
    EXPECT_EQ(window->overviewHighlight(), second);
    press(Qt::Key_Right);
    press(Qt::Key_Right);
    EXPECT_EQ(window->overviewHighlight(), first); // wraps
    press(Qt::Key_Left);
    EXPECT_EQ(window->overviewHighlight(), third);
    // Keys never reach the shells while the overview is open. A key that got
    // through would go to the focused pane, the first, whose cat holds it
    // until a line ends.
    press(Qt::Key_X, QStringLiteral("x"));
    press(Qt::Key_Return);
    EXPECT_FALSE(window->overviewOpen());
    EXPECT_EQ(window->focusedPane(), third);
    // A line through the same input ends what it held: an "x" shows with it.
    ASSERT_TRUE(first->sendInput("overview-closed\n"));
    ASSERT_TRUE(waitFor([&] {
        return first->text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("overview-closed"));
    }));
    EXPECT_FALSE(first->text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('x')))
        << first->text(AfterglowVt::Terminal::TextScope::Screen).toStdString();
    // The tab was zoomed: it returns zoomed, on the chosen pane.
    EXPECT_TRUE(waitFor([&] { return window->accessiblePaneRect(first).isEmpty(); }));
    EXPECT_FALSE(window->accessiblePaneRect(third).isEmpty());
    EXPECT_TRUE(chrome->keyMode().isEmpty());
    expectIdle(window);
    // Escape cancels back to the focused pane; a digit chooses directly.
    ASSERT_TRUE(window->runAction(third, Action::PaneOverview));
    press(Qt::Key_Right);
    press(Qt::Key_Escape);
    EXPECT_FALSE(window->overviewOpen());
    EXPECT_EQ(window->focusedPane(), third);
    ASSERT_TRUE(window->runAction(third, Action::PaneOverview));
    press(Qt::Key_2, QStringLiteral("2"));
    EXPECT_FALSE(window->overviewOpen());
    EXPECT_EQ(window->focusedPane(), second);
}

TEST_F(NativeWindow, PaneOverviewSpansTabsAndSwitchesToTheChosenTab) {
    auto* first = window->focusedPane();
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    drain();
    auto* second = window->focusedPane();
    ASSERT_NE(second, first);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* third = window->splitPane(second, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(third, nullptr);
    window->revealPane(second);
    drain();
    auto press = [&](int key) {
        QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &down);
        QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &up);
        drain();
    };
    ASSERT_TRUE(window->runAction(second, Action::PaneOverview));
    drain();
    EXPECT_EQ(window->overviewHighlight(), second);
    // Cards run in tab order: Left from the second tab's first pane reaches the first tab.
    press(Qt::Key_Left);
    EXPECT_EQ(window->overviewHighlight(), first);
    press(Qt::Key_Right);
    press(Qt::Key_Right);
    EXPECT_EQ(window->overviewHighlight(), third);
    // Up and Down jump to the first pane of the previous or next tab.
    press(Qt::Key_Up);
    EXPECT_EQ(window->overviewHighlight(), first);
    press(Qt::Key_Down);
    EXPECT_EQ(window->overviewHighlight(), second);
    press(Qt::Key_Up);
    press(Qt::Key_Return);
    EXPECT_FALSE(window->overviewOpen());
    // The chosen tab shows once the close has finished, so the cards never
    // jump with the current tab.
    EXPECT_TRUE(waitFor([&] { return window->focusedPane() == first; }));
    EXPECT_FALSE(window->accessiblePaneRect(first).isEmpty());
    expectIdle(window);
}

TEST_F(NativeWindow, TabSwitchAnimatesAndSettles) {
    auto* first = window->focusedPane();
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    EXPECT_TRUE(window->tabSwitching());
    auto* second = window->focusedPane();
    EXPECT_NE(second, first);
    QElapsedTimer clock;
    clock.start();
    while (window->tabSwitching() && clock.elapsed() < 3000) drain();
    EXPECT_FALSE(window->tabSwitching());
    EXPECT_EQ(window->focusedPane(), second);
    expectIdle(window);
    // Switching back runs again; the overview suppresses it.
    window->revealPane(first);
    EXPECT_TRUE(window->tabSwitching());
    clock.restart(); // expectIdle took its own time
    while (window->tabSwitching() && clock.elapsed() < 3000) drain();
    EXPECT_FALSE(window->tabSwitching());
    expectIdle(window);
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    window->revealPane(second);
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
    drain();
    QList<QVariantMap> keys;
    QList<std::pair<quint32, quint32>> switches;
    QObject listener;
    QObject::connect(window, &TerminalWindow::scriptKey, &listener, [&](const QString& owner, quint32, const QVariantMap& key) {
        if (owner == QLatin1String("script@1")) keys << key;
    });
    auto press = [&](int key, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = {}) {
        QKeyEvent down(QEvent::KeyPress, key, mods, text);
        QCoreApplication::sendEvent(window, &down);
        QKeyEvent up(QEvent::KeyRelease, key, mods, text);
        QCoreApplication::sendEvent(window, &up);
        drain();
    };
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_TRUE(window->startScriptKeyMode(QStringLiteral("script@1"), second, QStringLiteral("Script keys")).isEmpty());
    EXPECT_EQ(chrome->keyMode(), QStringLiteral("Script keys"));
    EXPECT_FALSE(window->startScriptKeyMode(QStringLiteral("other@2"), second, {}).isEmpty());
    press(Qt::Key_Left);
    press(Qt::Key_A, Qt::ControlModifier, QStringLiteral("\x01"));
    press(Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    ASSERT_EQ(keys.size(), 3);
    EXPECT_EQ(keys[0].value(QStringLiteral("key")), QStringLiteral("Left"));
    EXPECT_EQ(keys[1].value(QStringLiteral("key")), QStringLiteral("a"));
    EXPECT_TRUE(keys[1].value(QStringLiteral("ctrl")).toBool());
    EXPECT_EQ(keys[1].value(QStringLiteral("text")), QString());
    EXPECT_EQ(keys[2].value(QStringLiteral("text")), QStringLiteral("q"));
    // The script drives the overview while it holds the keys.
    QVariantMap state;
    ASSERT_TRUE(window->scriptOverview(second, QStringLiteral("open"), state).isEmpty());
    EXPECT_TRUE(state.value(QStringLiteral("open")).toBool());
    EXPECT_EQ(state.value(QStringLiteral("cards")).toList().size(), 2);
    press(Qt::Key_Right); // to the script, not the overview
    EXPECT_EQ(window->overviewHighlight(), second);
    ASSERT_EQ(keys.size(), 4);
    EXPECT_EQ(keys.last().value(QStringLiteral("key")), QStringLiteral("Right"));
    ASSERT_TRUE(window->scriptOverviewMove(second, nullptr, 1, state).isEmpty());
    EXPECT_EQ(state.value(QStringLiteral("highlighted")).toUInt(), first->id());
    ASSERT_TRUE(window->scriptOverview(second, QStringLiteral("choose"), state).isEmpty());
    EXPECT_FALSE(state.value(QStringLiteral("open")).toBool());
    EXPECT_EQ(window->focusedPane(), first);
    // Ending the mode, or the script going away, returns the keys.
    window->clearScriptOwner(QStringLiteral("script@1"));
    EXPECT_TRUE(chrome->keyMode().isEmpty());
    const int seen = keys.size();
    press(Qt::Key_Left);
    EXPECT_EQ(keys.size(), seen);
    // Pulses and tab switches.
    EXPECT_TRUE(window->pulsePane(first, 2, 400, 1).isEmpty());
    // The pulse lasts 400 ms; then the window stops asking for frames.
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
void sendKey(TerminalWindow* window, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
             const QString& text = {}, bool autoRepeat = false) {
    QKeyEvent down(QEvent::KeyPress, key, modifiers, text, autoRepeat);
    QCoreApplication::sendEvent(window, &down);
    QKeyEvent up(QEvent::KeyRelease, key, modifiers, text, autoRepeat);
    QCoreApplication::sendEvent(window, &up);
}

TEST_F(NativeWindow, ReturnOnAFocusedButtonAnswersThatButton) {
    auto* chrome = window->findChild<ChromeController*>();
    auto* layer = window->findChild<ChromeLayer*>();
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

TEST_F(NativeWindow, AnOverlayThatOpensBelowAPromptLeavesItsFocusedButton) {
    auto* chrome = window->findChild<ChromeController*>();
    auto* layer = window->findChild<ChromeLayer*>();
    QList<bool> answers;
    QObject listener;
    QObject::connect(chrome, &ChromeController::pasteAnswered, &listener, [&](bool paste) { answers << paste; });
    chrome->askPaste(QStringLiteral("one\ntwo"), 2);
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    auto* cancel = chromeItemWithText(layer, QStringLiteral("Cancel"));
    ASSERT_NE(cancel, nullptr);
    cancel->forceActiveFocus();
    // A keybind, D-Bus or a script opens the search under the prompt: the
    // keys must not go back to the panel, where Return pastes.
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
    auto* layer = window->findChild<ChromeLayer*>();
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

TEST_F(NativeWindow, ClipboardPromptDeniesOnTypedReturnAndPlainA) {
    auto* chrome = window->findChild<ChromeController*>();
    auto* layer = window->findChild<ChromeLayer*>();
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
    auto* layer = window->findChild<ChromeLayer*>();
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

TEST_F(NativeWindow, ClosingAnOverlayReturnsTheKeysToTheOneBelow) {
    auto* chrome = window->findChild<ChromeController*>();
    auto* layer = window->findChild<ChromeLayer*>();
    chrome->setSearchOpen(true);
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    chrome->openContextMenu({10, 10}, {QVariantMap{{QStringLiteral("id"), 1}, {QStringLiteral("title"), QStringLiteral("Item")}}});
    drain();
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
    auto* layer = window->findChild<ChromeLayer*>();
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
    auto* layer = window->findChild<ChromeLayer*>();
    chrome->askTitle(QStringLiteral("Tab title"), QStringLiteral("first"));
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    EXPECT_EQ(layer->focusedItem()->property("text").toString(), QStringLiteral("first"));
    chrome->askTitle(QStringLiteral("Split title"), QStringLiteral("second"));
    EXPECT_TRUE(waitFor([&] { return layer->focusedItem()->property("text").toString() == QStringLiteral("second"); }));
}

TEST_F(NativeWindow, PaneTitleLabelsSurviveTitleChanges) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    ASSERT_NE(window->splitPane(first, SplitTree::Direction::Right, launch, nullptr), nullptr);
    auto* layer = window->findChild<ChromeLayer*>();
    const auto labels = [&] {
        QList<QQuickItem*> found;
        if (!layer->rootItem()) return found; // the scene is made in the background and may not be ready yet
        for (auto* item : visualDescendants(layer->rootItem()))
            if (item->property("scrollbarSpace").isValid()) found.append(item);
        return found;
    };
    ASSERT_TRUE(waitFor([&] { return labels().size() == 2; }));
    const auto before = labels();
    ASSERT_EQ(before.size(), 2);
    first->setUserTitle(QStringLiteral("renamed"));
    auto* chrome = window->findChild<ChromeController*>();
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
    drain();
    auto send = [&](int key) {
        QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &down);
        QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &up);
    };
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    drain();
    // Seven steps round three cards: the position counts on past two turns.
    for (int step = 0; step < 7; ++step) {
        send(Qt::Key_Right);
        drain(20);
    }
    drain(400);
    send(Qt::Key_Escape);
    ASSERT_FALSE(window->overviewOpen());
    // Reopen within the closing motion: the deck turns at most half round.
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    const auto [position, target] = window->overviewPosition();
    EXPECT_LE(std::abs(target - position), 1.5) << position << " -> " << target;
    send(Qt::Key_Escape);
    expectIdle(window);
}

TEST_F(NativeWindow, OverviewRestoresTheZoomOnTheTabItCameFrom) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    window->revealPane(first);
    drain();
    ASSERT_TRUE(window->runAction(first, Action::TogglePaneZoom));
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    drain();
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
    drain();
    ASSERT_TRUE(window->overviewOpen());
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &escape);
    drain();
    EXPECT_FALSE(window->overviewOpen());
    // The first tab shows once the close has finished.
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
        return !titles.isEmpty() && titles.last() == QStringLiteral("after") && vars.size() >= 2
            && vars.last() != vars.first();
    })) << titles.join(QLatin1Char(',')).toStdString();
    EXPECT_EQ(titles, (QStringList{QStringLiteral("same"), QStringLiteral("after")}));
    EXPECT_EQ(vars.size(), 2) << vars.join(QLatin1Char(',')).toStdString();
    pane->setUserTitle(QStringLiteral("mine"));
    pane->setUserTitle(QStringLiteral("mine"));
    EXPECT_EQ(titles.size(), 3);
}

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
    // The first pane's program ends: the tab entered with it focused, but
    // the focus is on the third now and stays there.
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
    drain();
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
    EXPECT_TRUE(waitFor([&] { return second->text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('y')); }));
    EXPECT_FALSE(second->text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('2')));
}

TEST_F(NativeWindow, KeyRepeatsStayWithThePaneThatGotThePress) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")};
    auto* middle = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(middle, nullptr);
    auto* right = window->splitPane(middle, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(right, nullptr);
    window->revealPane(middle);
    drain();
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    QCoreApplication::sendEvent(window, &down);
    // The focus moves while the key is held (a click, D-Bus, a script).
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
    EXPECT_TRUE(waitFor([&] { return right->text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('y')); }));
    EXPECT_FALSE(right->text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('q')));
}

TEST_F(NativeWindow, TypingWhileTheOverviewClosesReachesTheChosenTab) {
    auto* first = window->focusedPane();
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    drain();
    auto* second = window->focusedPane();
    ASSERT_NE(second, first);
    ASSERT_TRUE(window->runAction(second, Action::PaneOverview));
    // Frames open the overview. Closed before any frame has opened it, it
    // would switch the tab at once, with no close to wait for.
    const auto opened = window->submittedFrames();
    ASSERT_TRUE(waitFor([&] { return window->submittedFrames() >= opened + 3; }));
    sendKey(window, Qt::Key_Left);
    ASSERT_EQ(window->overviewHighlight(), first);
    sendKey(window, Qt::Key_Return);
    ASSERT_FALSE(window->overviewOpen());
    // Still closing: the tab switches once the close has finished.
    ASSERT_EQ(window->focusedPane(), second);
    // A key typed now switches first, so it reaches the chosen pane.
    sendKey(window, Qt::Key_Shift);
    EXPECT_EQ(window->focusedPane(), first);
    expectIdle(window);
    EXPECT_EQ(window->focusedPane(), first);
}

TEST_F(NativeWindow, OverviewClosingOntoAnotherTabSwitchesWithoutATransition) {
    auto* first = window->focusedPane();
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    auto* second = window->focusedPane();
    ASSERT_NE(second, first);
    ASSERT_TRUE(waitFor([&] { return !window->tabSwitching(); })); // the new tab's own slide
    ASSERT_TRUE(window->runAction(second, Action::PaneOverview));
    // Frames open the overview. Closed before any frame has opened it, it
    // would switch the tab at once, with no close to wait for.
    const auto opened = window->submittedFrames();
    ASSERT_TRUE(waitFor([&] { return window->submittedFrames() >= opened + 3; }));
    sendKey(window, Qt::Key_Left);
    ASSERT_EQ(window->overviewHighlight(), first);
    sendKey(window, Qt::Key_Return);
    ASSERT_EQ(window->focusedPane(), second);
    // The close has carried the motion onto the card: the tab switch at its
    // end must not slide the panes a second time.
    bool slid = false;
    QElapsedTimer clock;
    clock.start();
    while (window->focusedPane() != first && clock.elapsed() < 3000) {
        drain(5);
        slid = slid || window->tabSwitching();
    }
    ASSERT_EQ(window->focusedPane(), first);
    EXPECT_FALSE(slid);
    EXPECT_FALSE(window->tabSwitching());
    expectIdle(window);
    // An ordinary switch afterwards still slides.
    window->revealPane(second);
    EXPECT_TRUE(window->tabSwitching());
}

TEST_F(NativeWindow, FocusWhileTheOverviewClosesWinsOverTheCard) {
    auto* first = window->focusedPane();
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    drain();
    auto* second = window->focusedPane();
    ASSERT_NE(second, first);
    ASSERT_TRUE(window->runAction(second, Action::PaneOverview));
    // Frames open the overview. Closed before any frame has opened it, it
    // would switch the tab at once, with no close to wait for.
    const auto opened = window->submittedFrames();
    ASSERT_TRUE(waitFor([&] { return window->submittedFrames() >= opened + 3; }));
    sendKey(window, Qt::Key_Left);
    ASSERT_EQ(window->overviewHighlight(), first);
    sendKey(window, Qt::Key_Return);
    ASSERT_EQ(window->focusedPane(), second);
    // afterglow ctl focus on the pane that already has the focus.
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
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(window->accessiblePaneRect(first).isEmpty());
    EXPECT_FALSE(window->accessiblePaneRect(last).isEmpty());
}

TEST_F(NativeWindow, ClipboardReadFromABackgroundTabAsksWithoutSwitching) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* background = window->openTab(launch, nullptr);
    ASSERT_NE(background, nullptr);
    window->revealPane(first);
    drain();
    const quint32 tab = window->activeTabId();
    auto* chrome = window->findChild<ChromeController*>();
    // The request originates on the PTY thread; its GUI receiver reads the pane title under the pane mutex.
    std::thread request([background] {
        background->withTerminal([](AfterglowVt::Terminal& terminal) { terminal.feed("\x1b]52;c;?\x07"); });
    });
    request.join();
    ASSERT_TRUE(waitFor([&] { return chrome->clipboardPromptOpen(); }));
    // The prompt names where the program is; the user's tab and focus stay.
    EXPECT_EQ(window->activeTabId(), tab);
    EXPECT_EQ(window->focusedPane(), first);
    EXPECT_TRUE(chrome->clipboardPromptText().contains(QStringLiteral("tab 2")));
    chrome->answerClipboard(0);
}

TEST_F(NativeWindow, ScriptKeyModeEndsWithItsReservedChord) {
    auto* chrome = window->findChild<ChromeController*>();
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
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(layer, nullptr);
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
    EXPECT_GE(window->accessiblePaneRect(second).width(), 4);
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
    const QPointF at = window->mapFromGlobal(window->accessiblePaneRect(first).center());
    QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &release);
    ASSERT_TRUE(chrome->contextMenuOpen());
    // The focus moves while the menu is open (a script, D-Bus, a clipboard request).
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
    drain();
    ASSERT_EQ(window->tabs().size(), 2);
    auto* chrome = window->findChild<ChromeController*>();
    chrome->tabMenu(1, 50, 15);
    ASSERT_TRUE(chrome->contextMenuOpen());
    // Its program ends: the tab closes, and a menu left open would act on nothing.
    other->sendInput("\n");
    ASSERT_TRUE(waitFor([&] { return window->tabs().size() == 1; }));
    EXPECT_FALSE(chrome->contextMenuOpen());
}

TEST_F(NativeWindow, ATabThatClosesLeavesAPaneMenuOpen) {
    auto* first = window->focusedPane();
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("read line")};
    auto* other = window->openTab(launch, nullptr);
    ASSERT_NE(other, nullptr);
    drain();
    ASSERT_EQ(window->tabs().size(), 2);
    window->revealPane(first);
    drain(300);
    auto* chrome = window->findChild<ChromeController*>();
    // The tab menu of the background tab, dismissed without a choice.
    chrome->tabMenu(1, 50, 15);
    ASSERT_TRUE(chrome->contextMenuOpen());
    chrome->closeContextMenu();
    ASSERT_TRUE(waitFor([&] { return !window->accessiblePaneRect(first).isEmpty(); }));
    const QPointF at = window->mapFromGlobal(window->accessiblePaneRect(first).center());
    QMouseEvent press(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &release);
    ASSERT_TRUE(chrome->contextMenuOpen());
    // The other tab's program ends: its tab closes, and the pane menu, which
    // belongs to the first tab, stays open.
    other->sendInput("\n");
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
    auto press = [&](const QPointF& at) {
        QMouseEvent down(QEvent::MouseButtonPress, at, window->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &down);
        QMouseEvent up(QEvent::MouseButtonRelease, at, window->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &up);
    };
    // The bottom dock spans the window; the left one fits above it.
    const qreal bottomRow = window->height() - 2;
    press(QPointF(2, bottomRow));
    press(QPointF(2, 2));
    ASSERT_EQ(clicks.size(), 2);
    EXPECT_EQ(clicks[0][QStringLiteral("dock")], QStringLiteral("status"));
    EXPECT_EQ(clicks[1][QStringLiteral("dock")], QStringLiteral("side"));
    EXPECT_EQ(clicks[1][QStringLiteral("item")], QStringLiteral("first"));
    EXPECT_EQ(clicks[1][QStringLiteral("button")], QStringLiteral("left"));
    press(QPointF(window->width() - 2, bottomRow)); // the status dock, past its text
    EXPECT_EQ(clicks.size(), 2);

    // With two tabs the tab bar shows, until a top dock takes its place.
    auto* chrome = window->findChild<ChromeController*>();
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
    other->sendInput("\x04"); // its cat ends, and the tab with it
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
    auto mouse = [&](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent event(type, at, window->mapToGlobal(at), button, held, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
    };
    // A drag from the item: the press clicks, and the drag selects nothing
    // in the pane, which would otherwise take the press as the start of a
    // selection.
    mouse(QEvent::MouseButtonPress, onItem, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, onItem + QPointF(40, 0), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, onItem + QPointF(40, 0), Qt::LeftButton, Qt::NoButton);
    ASSERT_EQ(clicks.size(), 1);
    EXPECT_EQ(clicks[0][QStringLiteral("item")], QStringLiteral("first"));
    EXPECT_FALSE(pane->hasSelection());
    EXPECT_EQ(window->focusedPane(), pane);

    // The wheel over the dock scrolls no pane; over the pane it does.
    const auto offset = [pane] { return pane->withTerminal([](AfterglowVt::Terminal& t) { return t.screen().viewportOffset(); }); };
    auto wheel = [&](const QPointF& at) {
        QWheelEvent event(at, window->mapToGlobal(at), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window, &event);
    };
    wheel(onItem);
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(offset(), 0u);
    wheel(onPane);
    EXPECT_TRUE(waitFor([&] { return offset() > 0; }));
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.scrollViewport(-int(t.screen().viewportOffset())); });

    // A right click on the dock while the left button selects in the pane:
    // the selection goes on, and the left release still ends it.
    mouse(QEvent::MouseButtonPress, onPane, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, onPane + QPointF(40, 0), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonPress, onItem, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
    mouse(QEvent::MouseButtonRelease, onItem, Qt::RightButton, Qt::LeftButton);
    const QString before = pane->selectedText();
    mouse(QEvent::MouseMove, onPane + QPointF(0, 40), Qt::NoButton, Qt::LeftButton);
    EXPECT_NE(pane->selectedText(), before);
    mouse(QEvent::MouseButtonRelease, onPane + QPointF(0, 40), Qt::LeftButton, Qt::NoButton);
    ASSERT_EQ(clicks.size(), 2);
    EXPECT_EQ(clicks[1][QStringLiteral("button")], QStringLiteral("right"));
    pane->clearSelection();

    // A dock press whose release went elsewhere takes nothing from the next
    // gesture: a click on the pane focuses and selects as usual.
    // Away from the last click, so that this is no double click.
    const QPointF elsewhere = onPane - QPointF(100, 60);
    mouse(QEvent::MouseButtonPress, onItem, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonPress, elsewhere, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, elsewhere + QPointF(40, 0), Qt::NoButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, elsewhere + QPointF(40, 0), Qt::LeftButton, Qt::NoButton);
    EXPECT_TRUE(pane->hasSelection());
    pane->clearSelection();
    // The click goes out on the press, so the lost release does not take it.
    ASSERT_EQ(clicks.size(), 3);

    // With the overview open, a click on the dock is the dock's: the
    // overview stays open.
    QVariantMap state;
    ASSERT_TRUE(window->scriptOverview(pane, QStringLiteral("open"), state).isEmpty());
    mouse(QEvent::MouseButtonPress, onItem, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, onItem, Qt::LeftButton, Qt::NoButton);
    EXPECT_TRUE(window->overviewOpen());
    ASSERT_EQ(clicks.size(), 4);
    ASSERT_TRUE(window->scriptOverview(pane, QStringLiteral("close"), state).isEmpty());

    // A larger font places the dock again, and its item still takes clicks.
    ASSERT_TRUE(window->runAction(pane, Action::FontBigger));
    const int cols = pane->cols();
    EXPECT_TRUE(waitFor([&] { return pane->cols() < cols || pane->rows() < rows - 1; }));
    expectIdle(window);
    mouse(QEvent::MouseButtonPress, onItem, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, onItem, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(clicks.size(), 5);
    ASSERT_TRUE(window->runAction(pane, Action::FontReset));
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, DocksTellTheirScriptOfTheirCellsTheHoveredItemAndTheWheel) {
    auto* pane = window->focusedPane();
    ASSERT_NE(pane, nullptr);
    expectIdle(window);
    struct Event { QString handler; QVariantMap event; bool limited; };
    QList<Event> events;
    QObject listener;
    QObject::connect(window, &TerminalWindow::scriptDockEvent, &listener,
                     [&](const QString& owner, const QString& handler, quint32, const QVariantMap& event, bool limited) {
                         if (owner == QLatin1String("a@1")) events.append({handler, event, limited});
                     });
    auto of = [&](const char* handler) {
        QList<QVariantMap> found;
        for (const Event& e : events) if (e.handler == QLatin1String(handler)) found.append(e.event);
        return found;
    };
    auto run = [](const QString& text, const QString& item = {}) {
        QVariantMap map{{QStringLiteral("text"), text}};
        if (!item.isEmpty()) map.insert(QStringLiteral("item"), item);
        return QVariant(map);
    };
    auto dock = [&](int size, const QVariantList& line) {
        return window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("bar")},
                                                             {QStringLiteral("edge"), QStringLiteral("bottom")}, {QStringLiteral("size"), size},
                                                             {QStringLiteral("lines"), QVariantList{line}}});
    };
    const QVariantList line{run(QStringLiteral("one"), QStringLiteral("first")), run(QStringLiteral(" ")),
                            run(QStringLiteral("two"), QStringLiteral("second"))};

    // The first placement tells the dock's cells: the window's width.
    const int windowCols = window->gridSize().width();
    ASSERT_GT(windowCols, 0);
    ASSERT_TRUE(dock(1, line).isEmpty());
    ASSERT_TRUE(waitFor([&] { return !of("on_dock_layout").isEmpty(); }));
    QVariantMap layout = of("on_dock_layout").constLast();
    EXPECT_EQ(layout[QStringLiteral("dock")], QStringLiteral("bar"));
    EXPECT_EQ(layout[QStringLiteral("window")].toUInt(), window->id());
    EXPECT_TRUE(layout[QStringLiteral("shown")].toBool());
    EXPECT_EQ(layout[QStringLiteral("cols")].toInt(), windowCols);
    EXPECT_EQ(layout[QStringLiteral("rows")].toInt(), 1);
    // New text in the same place tells nothing.
    expectIdle(window);
    const auto layouts = of("on_dock_layout").size();
    ASSERT_TRUE(dock(1, {run(QStringLiteral("one"), QStringLiteral("first")), run(QStringLiteral(" ")),
                         run(QStringLiteral("two"), QStringLiteral("second")), run(QStringLiteral("!"))})
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
    auto dockOf = [&](const char* id) -> const DockArea* {
        for (const DockArea* d : window->accessibleDocks()) if (d->id() == QLatin1String(id)) return d;
        return nullptr;
    };
    ASSERT_NE(dockOf("bar"), nullptr);
    ASSERT_EQ(dockOf("bar")->items().size(), 2u);
    const QRect dockRect = dockOf("bar")->rect();
    const QRect firstRect = dockOf("bar")->itemRect(0);
    const QRect secondRect = dockOf("bar")->itemRect(1);
    ASSERT_LT(firstRect.right(), secondRect.left());
    const qreal y = (dockRect.top() + 2) / dpr;
    auto moveTo = [&](const QPointF& at) {
        QMouseEvent event(QEvent::MouseMove, at, window->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
    };
    auto move = [&](qreal x) { moveTo(QPointF(x, y)); };
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
    // An overlay that opens under a pointer that does not move takes it off
    // the item, and gives it back when it closes.
    auto* chrome = window->findChild<ChromeController*>();
    auto* layer = window->findChild<ChromeLayer*>();
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
    ASSERT_TRUE(dock(2, {run(QStringLiteral("one"), QStringLiteral("first")), run(QStringLiteral("X"), QStringLiteral("middle")),
                         run(QStringLiteral("two"), QStringLiteral("second"))})
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
                                                            {QStringLiteral("lines"), QVariantList{QVariantList{run(QStringLiteral("up"), QStringLiteral("above"))}}}})
                    .isEmpty());
    ASSERT_TRUE(waitFor([&] { return dockOf("upper") && !dockOf("upper")->rect().isEmpty(); }));
    expectIdle(window);
    const QRect barNow = dockOf("bar")->itemRect(0);
    const QRect upperNow = dockOf("upper")->itemRect(0);
    ASSERT_FALSE(barNow.isEmpty());
    ASSERT_FALSE(upperNow.isEmpty());
    const auto hovers = of("on_dock_hover").size();
    moveTo(QRectF(barNow).center() / dpr);
    moveTo(QRectF(upperNow).center() / dpr);
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
    for (const Event& e : events) {
        if (e.handler != QLatin1String("on_dock_wheel")) {
            EXPECT_FALSE(e.limited) << e.handler.toStdString();
        }
    }

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
    for (const Event& e : events) {
        if (e.handler == QLatin1String("on_dock_wheel")) {
            EXPECT_TRUE(e.limited);
        }
    }

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
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, ADragOnADockItemTellsItsScriptAndMoveTabMovesATab) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    QList<QVariantMap> drags;
    int clicks = 0;
    QObject listener;
    QObject::connect(window, &TerminalWindow::scriptDockEvent, &listener,
                     [&](const QString& owner, const QString& handler, quint32, const QVariantMap& event, bool limited) {
                         if (owner != QLatin1String("a@1")) return;
                         if (handler == QLatin1String("on_dock_click")) ++clicks;
                         if (handler != QLatin1String("on_dock_drag")) return;
                         EXPECT_FALSE(limited);
                         drags.append(event);
                     });
    auto run = [](const QString& text, const QString& item = {}) {
        QVariantMap map{{QStringLiteral("text"), text}};
        if (!item.isEmpty()) map.insert(QStringLiteral("item"), item);
        return QVariant(map);
    };
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("bar")},
                                                            {QStringLiteral("edge"), QStringLiteral("bottom")}, {QStringLiteral("size"), 1},
                                                            {QStringLiteral("lines"), QVariantList{QVariantList{
                                                                run(QStringLiteral("one"), QStringLiteral("first")), run(QStringLiteral("    ")),
                                                                run(QStringLiteral("two"), QStringLiteral("second"))}}}})
                    .isEmpty());
    const DockArea* bar = nullptr;
    ASSERT_TRUE(waitFor([&] {
        for (const DockArea* d : window->accessibleDocks()) if (d->id() == QLatin1String("bar")) bar = d;
        return bar && bar->items().size() == 2;
    }));
    expectIdle(window);
    const qreal dpr = window->devicePixelRatio();
    const QRect dockRect = bar->rect();
    const QPointF onFirst = QRectF(bar->itemRect(0)).center() / dpr;
    const QPointF onSecond = QRectF(bar->itemRect(1)).center() / dpr;
    // "one" takes 3 cells, and the area 1 row. Between the items: the
    // middle of the fifth cell.
    const int cell = bar->itemRect(0).width() / 3;
    const QPointF between((bar->itemRect(0).left() + 4.5 * cell) / dpr, onFirst.y());
    auto send = [&](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, at, window->mapToGlobal(at), button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
    };
    auto press = [&](const QPointF& at) { send(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton); };
    auto move = [&](const QPointF& at) { send(QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton); };
    auto release = [&](const QPointF& at) { send(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton); };

    // A click with no drag between press and release is only a click.
    press(onFirst);
    move(onFirst + QPointF(1, 0));
    release(onFirst + QPointF(1, 0));
    EXPECT_TRUE(drags.isEmpty());
    EXPECT_EQ(clicks, 1);

    // Past the drag distance it starts, then tells each new item or cell.
    press(onFirst);
    const int distance = QGuiApplication::styleHints()->startDragDistance();
    move(onFirst + QPointF(distance + 1, 0));
    ASSERT_EQ(drags.size(), 1);
    EXPECT_EQ(drags[0][QStringLiteral("phase")], QStringLiteral("start"));
    EXPECT_EQ(drags[0][QStringLiteral("item")], QStringLiteral("first"));
    EXPECT_EQ(drags[0][QStringLiteral("dock")], QStringLiteral("bar"));
    EXPECT_EQ(drags[0][QStringLiteral("window")].toUInt(), window->id());
    EXPECT_EQ(drags[0][QStringLiteral("row")].toInt(), 0);
    move(between);
    ASSERT_EQ(drags.size(), 2);
    EXPECT_EQ(drags[1][QStringLiteral("phase")], QStringLiteral("move"));
    EXPECT_FALSE(drags[1].contains(QStringLiteral("over")));
    EXPECT_EQ(drags[1][QStringLiteral("col")].toInt(), 4);
    // The same cell again tells nothing.
    move(between + QPointF(0, 0.2));
    EXPECT_EQ(drags.size(), 2);
    move(onSecond);
    ASSERT_EQ(drags.size(), 3);
    EXPECT_EQ(drags[2][QStringLiteral("over")], QStringLiteral("second"));
    EXPECT_EQ(drags[2][QStringLiteral("item")], QStringLiteral("first"));
    // Above the area, the drag goes on with rows above it, and no pane
    // hears of it.
    const QPointF above(onSecond.x(), (dockRect.top() - 3 * dockRect.height()) / dpr);
    move(above);
    ASSERT_EQ(drags.size(), 4);
    EXPECT_LT(drags[3][QStringLiteral("row")].toInt(), 0);
    EXPECT_FALSE(drags[3].contains(QStringLiteral("over")));
    EXPECT_FALSE(first->hasSelection());
    release(onSecond);
    ASSERT_EQ(drags.size(), 6);
    EXPECT_EQ(drags[4][QStringLiteral("phase")], QStringLiteral("move"));
    EXPECT_EQ(drags[5][QStringLiteral("phase")], QStringLiteral("end"));
    EXPECT_EQ(drags[5][QStringLiteral("over")], QStringLiteral("second"));

    // A release that went elsewhere cancels it.
    drags.clear();
    press(onFirst);
    move(onSecond);
    ASSERT_EQ(drags.size(), 1);
    send(QEvent::MouseMove, onSecond, Qt::NoButton, Qt::NoButton);
    ASSERT_EQ(drags.size(), 2);
    EXPECT_EQ(drags[1][QStringLiteral("phase")], QStringLiteral("cancel"));
    // So does an undock of the area, before it goes.
    drags.clear();
    press(onFirst);
    move(onSecond);
    ASSERT_EQ(drags.size(), 1);
    ASSERT_TRUE(window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("undock")}, {QStringLiteral("id"), QStringLiteral("bar")}})
                    .isEmpty());
    ASSERT_EQ(drags.size(), 2);
    EXPECT_EQ(drags[1][QStringLiteral("phase")], QStringLiteral("cancel"));
    EXPECT_EQ(drags[1][QStringLiteral("item")], QStringLiteral("first"));
    move(onFirst);
    release(onFirst);
    EXPECT_EQ(drags.size(), 2);

    // move_tab moves the pane's tab and leaves the current tab on screen.
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    auto* second = window->focusedPane();
    ASSERT_TRUE(window->runAction(second, Action::NewTab));
    auto* third = window->focusedPane();
    ASSERT_EQ(window->tabIndexOf(first), 0);
    ASSERT_EQ(window->tabIndexOf(third), 2);
    EXPECT_TRUE(window->moveTabOf(first, 2).isEmpty());
    EXPECT_EQ(window->tabIndexOf(first), 2);
    EXPECT_EQ(window->tabIndexOf(second), 0);
    EXPECT_EQ(window->tabIndexOf(third), 1);
    EXPECT_EQ(window->focusedPane(), third);
    EXPECT_FALSE(window->moveTabOf(first, 3).isEmpty());
    EXPECT_EQ(window->tabIndexOf(first), 2);
    // Through the script callbacks, with its refusal.
    SettingsStore settings(directory.filePath(QStringLiteral("move.conf")));
    Control control(settings, {});
    control.attachWindow(window);
    const auto callbacks = scriptCallbacks(control);
    auto apply = [&](quint32 pane, int index) {
        return callbacks.applyOwned(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("move_tab")}, {QStringLiteral("pane"), pane},
                                                           {QStringLiteral("index"), double(index)}}, {});
    };
    EXPECT_TRUE(apply(first->id(), 0).error.isEmpty());
    EXPECT_EQ(window->tabIndexOf(first), 0);
    const auto refused = apply(first->id(), 9);
    EXPECT_FALSE(refused.error.isEmpty());
    EXPECT_EQ(refused.code, QStringLiteral("operation_failed"));
    window->clearScriptOwner(QStringLiteral("a@1"));
}

TEST_F(NativeWindow, PaneDocksTakeCellsFromTheirPaneAndSpeakOfIt) {
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    expectIdle(window);
    ASSERT_EQ(first->rows(), second->rows());
    const int rows = first->rows();
    struct Event { QString handler; quint32 pane; QVariantMap event; };
    QList<Event> events;
    QObject listener;
    QObject::connect(window, &TerminalWindow::scriptDockEvent, &listener,
                     [&](const QString& owner, const QString& handler, quint32 pane, const QVariantMap& event, bool) {
                         if (owner == QLatin1String("a@1")) events.append({handler, pane, event});
                     });
    auto of = [&](const char* handler) {
        QList<Event> found;
        for (const Event& e : events) if (e.handler == QLatin1String(handler)) found.append(e);
        return found;
    };
    auto dock = [&](Pane* pane, const QString& id, const QString& edge, int size) {
        return window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), id},
                                                            {QStringLiteral("scope"), QStringLiteral("pane")},
                                                            {QStringLiteral("edge"), edge}, {QStringLiteral("size"), size},
                                                            {QStringLiteral("lines"), QVariantList{QVariantList{QVariantMap{
                                                                {QStringLiteral("text"), QStringLiteral("title")}, {QStringLiteral("item"), QStringLiteral("t")}}}}}},
                                       pane);
    };
    auto undock = [&](Pane* pane, const QString& id) {
        return window->setScriptDock(QStringLiteral("a@1"), {{QStringLiteral("kind"), QStringLiteral("undock")}, {QStringLiteral("id"), id},
                                                            {QStringLiteral("scope"), QStringLiteral("pane")}},
                                       pane);
    };
    auto paneDock = [&](Pane* pane, const char* id) -> const DockArea* {
        for (const auto& d : pane->docks()) if (d->id() == QLatin1String(id)) return d.get();
        return nullptr;
    };

    // A top dock takes a row from its pane only, and tells its script the
    // pane's width.
    ASSERT_TRUE(dock(first, QStringLiteral("title"), QStringLiteral("top"), 1).isEmpty());
    ASSERT_TRUE(waitFor([&] { return first->rows() == rows - 1; }));
    EXPECT_EQ(second->rows(), rows);
    ASSERT_TRUE(waitFor([&] { return !of("on_dock_layout").isEmpty(); }));
    const Event layout = of("on_dock_layout").constLast();
    EXPECT_EQ(layout.pane, first->id());
    EXPECT_EQ(layout.event[QStringLiteral("scope")], QStringLiteral("pane"));
    EXPECT_EQ(layout.event[QStringLiteral("dock")], QStringLiteral("title"));
    EXPECT_TRUE(layout.event[QStringLiteral("shown")].toBool());
    EXPECT_GE(layout.event[QStringLiteral("cols")].toInt(), first->cols());
    EXPECT_EQ(layout.event[QStringLiteral("rows")].toInt(), 1);
    const DockArea* title = paneDock(first, "title");
    ASSERT_NE(title, nullptr);
    ASSERT_FALSE(title->rect().isEmpty());
    // Inside the pane, above its grid.
    const qreal dpr = window->devicePixelRatio();
    const QRect grid = window->accessiblePaneRect(first);
    const QRect dockGlobal = window->accessibleDockRect(title);
    EXPECT_LE(dockGlobal.bottom(), grid.top());
    EXPECT_GE(dockGlobal.left(), window->mapToGlobal(QPoint(0, 0)).x());
    EXPECT_TRUE(window->accessibleDocks().contains(title));
    expectIdle(window);

    // A click on its item goes to its script, with its pane, and not to
    // the pane.
    const QPointF onItem = QRectF(title->itemRect(0)).center() / dpr;
    QMouseEvent down(QEvent::MouseButtonPress, onItem, window->mapToGlobal(onItem), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &down);
    QMouseEvent up(QEvent::MouseButtonRelease, onItem, window->mapToGlobal(onItem), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &up);
    ASSERT_EQ(of("on_dock_click").size(), 1);
    EXPECT_EQ(of("on_dock_click")[0].pane, first->id());
    EXPECT_EQ(of("on_dock_click")[0].event[QStringLiteral("item")], QStringLiteral("t"));
    EXPECT_EQ(of("on_dock_click")[0].event[QStringLiteral("scope")], QStringLiteral("pane"));
    EXPECT_FALSE(first->hasSelection());
    // The same id at another pane is another area.
    ASSERT_TRUE(dock(second, QStringLiteral("title"), QStringLiteral("left"), 4).isEmpty());
    ASSERT_TRUE(waitFor([&] { return paneDock(second, "title") && !paneDock(second, "title")->rect().isEmpty(); }));
    EXPECT_EQ(second->rows(), rows);
    EXPECT_EQ(first->rows(), rows - 1);
    // The overview draws cards: their docks take no pointer there.
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    EXPECT_FALSE(window->accessibleDocks().contains(title));
    QVariantMap state;
    ASSERT_TRUE(window->scriptOverview(first, QStringLiteral("close"), state).isEmpty());
    ASSERT_TRUE(waitFor([&] { return !window->overviewOpen(); }));
    EXPECT_TRUE(window->accessibleDocks().contains(title));
    // An undock gives the row back.
    ASSERT_TRUE(undock(first, QStringLiteral("title")).isEmpty());
    ASSERT_TRUE(waitFor([&] { return first->rows() == rows; }));
    EXPECT_NE(paneDock(second, "title"), nullptr);

    // A pane in another tab lays its docks out at once, where it is.
    ASSERT_TRUE(window->runAction(second, Action::NewTab));
    auto* third = window->focusedPane();
    ASSERT_NE(window->tabIndexOf(third), window->tabIndexOf(first));
    events.clear();
    ASSERT_TRUE(dock(first, QStringLiteral("status"), QStringLiteral("bottom"), 2).isEmpty());
    ASSERT_EQ(of("on_dock_layout").size(), 1);
    EXPECT_TRUE(of("on_dock_layout")[0].event[QStringLiteral("shown")].toBool());
    EXPECT_EQ(of("on_dock_layout")[0].event[QStringLiteral("rows")].toInt(), 2);
    // Its row count follows when its tab shows.
    window->revealPane(first);
    // The tab bar shows now: the pane beside it, with a left dock only,
    // has the rows of the tab.
    ASSERT_TRUE(waitFor([&] { return first->rows() == second->rows() - 2; })) << first->rows() << " " << second->rows();
    // Too many at one pane.
    for (const char* id : {"b", "c"}) ASSERT_TRUE(dock(first, QString::fromLatin1(id), QStringLiteral("right"), 1).isEmpty());
    ASSERT_TRUE(dock(first, QStringLiteral("d"), QStringLiteral("right"), 1).isEmpty());
    EXPECT_FALSE(dock(first, QStringLiteral("e"), QStringLiteral("right"), 1).isEmpty());
    // A pane that closes takes its docks: the closing frame draws them,
    // and nothing points at them after.
    window->requestClosePane(second);
    expectIdle(window);
    // The script's end takes the rest.
    window->clearScriptOwner(QStringLiteral("a@1"));
    EXPECT_TRUE(first->docks().empty());
    ASSERT_TRUE(waitFor([&] { return first->rows() == third->rows(); }));
    EXPECT_TRUE(window->accessibleDocks().isEmpty());

    // A pane's docks go with it to another window, and take their cells
    // from it there.
    window->revealPane(third);
    expectIdle(window);
    const int alone = third->rows();
    ASSERT_TRUE(dock(third, QStringLiteral("title"), QStringLiteral("top"), 1).isEmpty());
    ASSERT_TRUE(waitFor([&] { return third->rows() == alone - 1; }));
    ASSERT_TRUE(window->runAction(third, Action::DetachTab));
    TerminalWindow* detached = nullptr;
    ASSERT_TRUE(waitFor([&] {
        detached = qobject_cast<TerminalWindow*>(third->parent());
        return detached && detached != window;
    }));
    ASSERT_EQ(third->docks().size(), 1u);
    const DockArea* moved = third->docks().front().get();
    ASSERT_TRUE(waitFor([&] { return detached->accessibleDocks().contains(moved); }));
    EXPECT_FALSE(window->accessibleDocks().contains(moved));
    detached->clearScriptOwner(QStringLiteral("a@1"));
    EXPECT_TRUE(third->docks().empty());
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
    QMouseEvent down(QEvent::MouseButtonPress, onItem, window->mapToGlobal(onItem), Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &down);
    QMouseEvent up(QEvent::MouseButtonRelease, onItem, window->mapToGlobal(onItem), Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &up);
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
    EXPECT_LT(chrome->paneArea().height(), height);
    EXPECT_TRUE(control.scriptDock(QStringLiteral("a@1"), moving->id(), undock).isEmpty());
    EXPECT_TRUE(waitFor([&] { return chrome->paneArea().height() == height; })) << chrome->paneArea().height();

    // The pane closes: the undock through its id still finds the area.
    quint32 created = 0;
    auto* first = window->focusedPane();
    ASSERT_TRUE(control.split(first->id(), QStringLiteral("right"), {QStringLiteral("/bin/cat")}, {}, created).isEmpty());
    Pane* closing = nullptr;
    for (auto* pane : window->panes())
        if (pane->id() == created) closing = pane;
    ASSERT_NE(closing, nullptr);
    ASSERT_TRUE(control.scriptDock(QStringLiteral("a@1"), created, bar).isEmpty());
    ASSERT_TRUE(waitFor([&] { return chrome->paneArea().height() < height; }));
    QPointer<Pane> gone(closing);
    closing->sendInput("\x04"); // its cat ends, and the pane with it
    ASSERT_TRUE(waitFor([&] { return gone.isNull(); }));
    EXPECT_TRUE(control.scriptDock(QStringLiteral("a@1"), created, undock).isEmpty());
    EXPECT_TRUE(waitFor([&] { return chrome->paneArea().height() == height; })) << chrome->paneArea().height();
    // An id that never docked is still not a pane.
    EXPECT_FALSE(control.scriptDock(QStringLiteral("a@1"), created + 1000, undock).isEmpty());
    destination->close();
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
    // At most 8 docks for one script in a window.
    for (int i = 0; i < 8; ++i)
        ASSERT_TRUE(control.scriptDock(QStringLiteral("a@1"), pane->id(),
                                       {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("d%1").arg(i)},
                                        {QStringLiteral("edge"), QStringLiteral("left")}, {QStringLiteral("size"), 1}})
                        .isEmpty());
    EXPECT_FALSE(control.scriptDock(QStringLiteral("a@1"), pane->id(),
                                    {{QStringLiteral("kind"), QStringLiteral("dock")}, {QStringLiteral("id"), QStringLiteral("d8")},
                                     {QStringLiteral("edge"), QStringLiteral("left")}, {QStringLiteral("size"), 1}})
                     .isEmpty());
    window->clearScriptOwner(QStringLiteral("a@1"));
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
    ASSERT_TRUE(waitFor([&] { return chrome->tabBarHeight() > 0; }));
    auto* first = window->focusedPane();
    ASSERT_NE(first, nullptr);
    auto* second = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(second, nullptr);
    expectIdle(window);
    const QPoint left = window->mapFromGlobal(window->accessiblePaneRect(first).topRight());
    const QPoint right = window->mapFromGlobal(window->accessiblePaneRect(second).topLeft());
    const QPointF divider((left.x() + right.x()) / 2.0, window->mapFromGlobal(window->accessiblePaneRect(first).center()).y());
    auto mouse = [&](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent event(type, at, window->mapToGlobal(at), button, held, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
    };
    mouse(QEvent::MouseMove, divider, Qt::NoButton, Qt::NoButton);
    EXPECT_EQ(window->cursor().shape(), Qt::SplitHCursor);
    // Each drag move lays the panes out again in the next frame.
    const int cols = first->cols();
    mouse(QEvent::MouseButtonPress, divider, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseMove, divider + QPointF(10, 0), Qt::NoButton, Qt::LeftButton);
    drain(100);
    mouse(QEvent::MouseMove, divider + QPointF(30, 0), Qt::NoButton, Qt::LeftButton);
    EXPECT_TRUE(waitFor([&] { return first->cols() > cols; })) << first->cols(); // the frames laid the panes out
    EXPECT_EQ(window->cursor().shape(), Qt::SplitHCursor);
    // Over the tab bar too, while the drag lasts.
    mouse(QEvent::MouseMove, QPointF(divider.x() + 30, chrome->tabBarHeight() / 2.0), Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(nextFrame(window));
    EXPECT_EQ(window->cursor().shape(), Qt::SplitHCursor);
    // A tab switch from the keyboard ends the drag: its divider is the other
    // tab's.
    ASSERT_TRUE(window->runAction(first, Action::PreviousTab));
    drain(100);
    mouse(QEvent::MouseMove, QPointF(divider.x() + 40, chrome->tabBarHeight() / 2.0), Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(nextFrame(window));
    EXPECT_NE(window->cursor().shape(), Qt::SplitHCursor);
    mouse(QEvent::MouseButtonRelease, QPointF(divider.x() + 40, chrome->tabBarHeight() / 2.0), Qt::LeftButton, Qt::NoButton);
    expectIdle(window);
}

TEST_F(NativeWindow, ATabBarDockThatDoesNotFitLeavesTheTabBarInPlace) {
    auto* chrome = window->findChild<ChromeController*>();
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
    // new_tab from a pane starts where that pane is.
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
    const auto offset = [pane] { return pane->withTerminal([](AfterglowVt::Terminal& t) { return t.screen().viewportOffset(); }); };
    ASSERT_GT(offset(), 0u);
    EXPECT_TRUE(pane->sendReply("\x1b[I"));
    EXPECT_GT(offset(), 0u);
    // Typing snaps back to the bottom.
    EXPECT_TRUE(pane->sendInput("x"));
    EXPECT_EQ(offset(), 0u);
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
    drain();
    auto* chrome = window->findChild<ChromeController*>();
    // A key mode from before the overview stays when it closes.
    ASSERT_TRUE(window->startScriptKeyMode(QStringLiteral("script@1"), first, QStringLiteral("Before")).isEmpty());
    QVariantMap state;
    ASSERT_TRUE(window->scriptOverview(first, QStringLiteral("open"), state).isEmpty());
    ASSERT_TRUE(window->scriptOverview(first, QStringLiteral("close"), state).isEmpty());
    EXPECT_EQ(chrome->keyMode(), QStringLiteral("Before"));
    window->clearScriptOwner(QStringLiteral("script@1"));
    // One started while the overview is open ends with it: closeOverview is
    // where a click, a script and the keys all close it.
    ASSERT_TRUE(window->scriptOverview(first, QStringLiteral("open"), state).isEmpty());
    ASSERT_TRUE(window->startScriptKeyMode(QStringLiteral("script@1"), first, QStringLiteral("Overview keys")).isEmpty());
    EXPECT_EQ(chrome->keyMode(), QStringLiteral("Overview keys"));
    ASSERT_TRUE(window->scriptOverview(first, QStringLiteral("choose"), state).isEmpty());
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
    EXPECT_TRUE(waitFor([&] { return chosen->text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('q')); }));
    EXPECT_EQ(keys, 0);
}

} // namespace

int main(int argc, char** argv)
{
    // These tests open windows and send desktop-wide D-Bus signals (the
    // launcher entry). Run them only inside the nested desktop of a runner.
    if (qEnvironmentVariableIsEmpty("AFTERGLOW_NATIVE_ISOLATED")) {
        std::fprintf(stderr, "Run these tests through native_window_acceptance.py or the ag-verify-live "
                             "headless_native.py runner, which isolate the compositor and the session bus.\n");
        return 2;
    }
    previousHandler = qInstallMessageHandler(recordQmlErrors);
    // The plugin paths, the layer-shell integration and the names, as afterglow sets them.
    prepareApplication();
    QApplication app(argc, argv);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "NativeWindowTest.moc"
