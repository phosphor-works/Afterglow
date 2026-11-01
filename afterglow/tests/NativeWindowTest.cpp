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
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QInputMethodEvent>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
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
#include <QScopeGuard>
#include <QStandardPaths>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
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
// needs it, if the background has not finished it yet.
bool chromeFocusReady(ChromeLayer* layer) {
    auto* focus = layer->focusedItem();
    return focus && focus->hasActiveFocus() && focus->isVisible() && focus != focus->window()->contentItem();
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
        drain(200);
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
    drain();
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_GT(chrome->tabBarHeight(), 0);
    auto pointer = [&](QEvent::Type type, QPointF position, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent event(type, position, QPointF(window->mapToGlobal(position.toPoint())), button, held, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &event);
    };
    const QPointF start(50, chrome->tabBarHeight()/2.0), outside(50, 150);
    pointer(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    pointer(QEvent::MouseMove, outside, Qt::NoButton, Qt::LeftButton);
    pointer(QEvent::MouseMove, start, Qt::NoButton, Qt::LeftButton);
    pointer(QEvent::MouseButtonRelease, start, Qt::LeftButton, Qt::NoButton);
    drain(50);
    ASSERT_EQ(original->parent(), window);
    ASSERT_EQ(window->tabs().size(), 2);
    pointer(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    pointer(QEvent::MouseMove, outside, Qt::NoButton, Qt::LeftButton);
    pointer(QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
    drain(250);
    auto* detached = qobject_cast<TerminalWindow*>(original->parent());
    ASSERT_NE(detached, nullptr);
    ASSERT_NE(detached, window);
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
    drain(200);
    EXPECT_NE(background->parent(), window);
    EXPECT_EQ(other->parent(), window);

    application.reset();
    application = std::make_unique<Application>(directory.filePath(QStringLiteral("single-tab.conf")),
        QJsonObject{{QStringLiteral("Window"), QJsonObject{{QStringLiteral("tabBar"), QStringLiteral("always")}}}});
    WindowOptions singleOptions;
    singleOptions.command = {QStringLiteral("/bin/cat")};
    window = application->newWindow(singleOptions);
    ASSERT_NE(window, nullptr);
    window->resize(900, 600);
    drain(200);
    QPointer<Pane> lastPane = window->focusedPane();
    QPointer<TerminalWindow> source = window;
    pointer(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    pointer(QEvent::MouseMove, QPointF(-50,150), Qt::NoButton, Qt::LeftButton);
    pointer(QEvent::MouseButtonRelease, QPointF(-50,150), Qt::LeftButton, Qt::NoButton);
    drain(200);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(source.isNull());
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
    drain(1000);
    ASSERT_GT(source->submittedFrames(), 0u);
    ASSERT_GT(survivor->submittedFrames(), 0u);
    QPointer<Pane> pane = source->focusedPane();
    ASSERT_TRUE(pane);
    Q_EMIT source->transferRequested(pane, true);
    drain(1000);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    ASSERT_TRUE(source.isNull());
    ASSERT_TRUE(pane);
    auto* destination = qobject_cast<TerminalWindow*>(pane->parent());
    ASSERT_TRUE(destination);
    ASSERT_GT(destination->submittedFrames(), 0u);
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
    EXPECT_GT(destination->submittedFrames(), destinationBefore);
    EXPECT_GT(survivor->submittedFrames(), before);
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
    drain();
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(layer->rootItem(), nullptr);
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
    drain();
    bool found = false;
    for (auto* item : visualDescendants(layer->rootItem())) {
        if (item->property("literalTitle").toString() != title) continue;
        found = true;
        // Each entity carries an extra "&" for the label's mnemonic step.
        QString escaped = title;
        escaped.replace(QLatin1Char('&'), QStringLiteral("&&amp;"))
            .replace(QLatin1Char('<'), QStringLiteral("&&lt;"))
            .replace(QLatin1Char('>'), QStringLiteral("&&gt;"));
        EXPECT_EQ(item->property("text").toString(), QStringLiteral("<span>") + escaped + QStringLiteral("</span>"));
        auto* accessible = QAccessible::queryAccessibleInterface(item);
        ASSERT_NE(accessible, nullptr);
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
    EXPECT_TRUE(found);
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
    drain(300);
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
    drain();
    first->refreshSnapshot();
    EXPECT_EQ(first->snapshot().at(0, 0).bg, original);
    // A search that has not completed must also remain retired after its callback.
    window->revealPane(first);
    chrome->setSearchOpen(true);
    chrome->search(QStringLiteral("needle"), 0);
    window->revealPane(second);
    // Let the search finish and its watcher report, then check.
    QThreadPool::globalInstance()->waitForDone(2000);
    drain();
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
    drain();
    auto* chrome = window->findChild<ChromeController*>();
    // The titles follow the next frame, which has no fixed time.
    ASSERT_TRUE(waitFor([&] { return chrome->paneTitles().size() == 1; }));
    const auto zoomed = window->accessiblePaneRect(first);
    EXPECT_FALSE(window->swapPane(first, SplitTree::Neighbour::Left));
    drain();
    EXPECT_EQ(chrome->paneTitles().size(), 1);
    EXPECT_EQ(window->accessiblePaneRect(first), zoomed);
    ASSERT_TRUE(window->swapPane(first, SplitTree::Neighbour::Right));
    drain();
    EXPECT_EQ(chrome->paneTitles().size(), 2);
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
    drain();
    const QRect slots[] = {window->accessiblePaneRect(first), window->accessiblePaneRect(second),
                           window->accessiblePaneRect(third)};
    ASSERT_TRUE(window->runAction(first, Action::TogglePaneZoom));
    drain();
    ASSERT_TRUE(window->runAction(first, Action::RotatePanesClockwise));
    drain();
    EXPECT_EQ(window->accessiblePaneRect(third), slots[0]);
    EXPECT_EQ(window->accessiblePaneRect(first), slots[1]);
    EXPECT_EQ(window->accessiblePaneRect(second), slots[2]);
    EXPECT_EQ(window->findChild<ChromeController*>()->paneTitles().size(), 3);
    EXPECT_EQ(window->focusedPane(), first);
    ASSERT_TRUE(window->runAction(first, Action::RotatePanesCounterclockwise));
    drain();
    EXPECT_EQ(window->accessiblePaneRect(first), slots[0]);
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
        auto* fresh = application->newWindow(options);
        ASSERT_NE(fresh, nullptr);
        if (settled) drain();
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
        auto* layer = fresh->findChild<ChromeLayer*>();
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
        ASSERT_TRUE(ready());
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
            QCoreApplication::sendEvent(fresh, &escape);
        }
        EXPECT_FALSE(chrome->wantsKeyboard());
        delete fresh;
    }
}

class LauncherObserver : public QObject {
    Q_OBJECT
public:
    QList<QVariantMap> updates;
public Q_SLOTS:
    void update(const QString&, const QVariantMap& properties) { updates.append(properties); }
};
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
    ASSERT_TRUE(waitFor([&] { return !observer.updates.isEmpty(); }));
    EXPECT_TRUE(observer.updates.last().value(QStringLiteral("progress-visible")).toBool());
    observer.updates.clear();
    delete contributor;
    ASSERT_TRUE(waitFor([&] { return !observer.updates.isEmpty(); }));
    EXPECT_FALSE(observer.updates.last().value(QStringLiteral("progress-visible")).toBool());
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
    drain();
    QInputMethodQueryEvent query(Qt::ImEnabled | Qt::ImCursorRectangle | Qt::ImSurroundingText);
    QCoreApplication::sendEvent(window, &query);
    EXPECT_TRUE(query.value(Qt::ImEnabled).toBool());
    EXPECT_FALSE(query.value(Qt::ImCursorRectangle).toRectF().isEmpty());
    QInputMethodEvent commit; commit.setCommitString(QString::fromUtf8("検索"));
    QCoreApplication::sendEvent(window, &commit);
    drain();
    QInputMethodQueryEvent text(Qt::ImSurroundingText);
    QCoreApplication::sendEvent(window, &text);
    EXPECT_EQ(text.value(Qt::ImSurroundingText).toString(), QString::fromUtf8("検索"));
}
TEST_F(NativeWindow, ChromeButtonsReceiveKeyReleases) {
    auto* chrome = window->findChild<ChromeController*>();
    chrome->setSearchOpen(true);
    drain();
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
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
    drain();
    auto* root = window->accessibleRoot();
    ASSERT_NE(root, nullptr);
    EXPECT_GE(root->childCount(), 2);
    EXPECT_EQ(window->accessibleChrome()->parent(), root);
    EXPECT_GT(window->accessibleChrome()->childCount(), 0);
    auto* focus = root->focusChild();
    ASSERT_NE(focus, nullptr);
    EXPECT_TRUE(focus->state().editable);
    EXPECT_TRUE(root->rect().contains(focus->rect().center()));
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
    ASSERT_TRUE(bus.registerObject(QStringLiteral("/org/freedesktop/Notifications"), &server, QDBusConnection::ExportAllSlots));
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
    bus.unregisterObject(QStringLiteral("/org/freedesktop/Notifications"));
    bus.unregisterService(QStringLiteral("org.freedesktop.Notifications"));
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
    drain(300);
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
    QList<QMessageBox*> prompts;
    waitFor([&] {
        prompts.clear();
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible() && box->windowTitle() == QStringLiteral("File transfer")) prompts.append(box);
        return !prompts.isEmpty();
    });
    ASSERT_EQ(prompts.size(), 1);
    EXPECT_EQ(prompts.front()->windowHandle()->transientParent(), destination);
    prompts.front()->reject(); drain(50);
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
    QObject::connect(pane->dragDrop(), &DragDrop::reply, window, [&](const QByteArray& bytes) {
        protocol += bytes;
        if (bytes.contains("t=e") && bytes.contains("x=4")) complete = true;
    });
    const auto incoming = QObject::connect(pane, &Pane::terminalProtocolRequested, window, [&](int code, const QByteArray& bytes) { if (code == 72) protocol += "CLIENT: " + bytes + "\n"; });
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
    QObject::connect(pane->dragDrop(), &DragDrop::reply, window, [&](const QByteArray& bytes) {
        protocol += bytes;
        if (bytes.contains("t=e") && bytes.contains("x=4")) complete = true;
    });
    const auto incoming = QObject::connect(pane, &Pane::terminalProtocolRequested, window, [&](int code, const QByteArray& bytes) { if (code == 72) protocol += "CLIENT: " + bytes + "\n"; });
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
    // Keys never reach the shells while the overview is open. The second and
    // third panes echo, so a key that got through would show at once.
    press(Qt::Key_X, QStringLiteral("x"));
    EXPECT_FALSE(second->text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('x')));
    EXPECT_FALSE(third->text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('x')));
    press(Qt::Key_Return);
    EXPECT_FALSE(window->overviewOpen());
    EXPECT_EQ(window->focusedPane(), third);
    // The tab was zoomed: it returns zoomed, on the chosen pane.
    EXPECT_TRUE(window->accessiblePaneRect(first).isEmpty());
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
    QObject::connect(window, &TerminalWindow::scriptKey, window, [&](const QString& owner, quint32, const QVariantMap& key) {
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
    QList<std::pair<quint32, quint32>> switches;
    QObject::connect(window, &TerminalWindow::tabSwitched, window, [&](quint32 from, quint32 to, int direction) {
        EXPECT_EQ(direction, 1);
        switches.append({from, to});
    });
    ASSERT_TRUE(window->runAction(first, Action::NewTab));
    drain();
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
    QObject::connect(chrome, &ChromeController::confirmAnswered, window, [&](bool accepted) { confirmed = accepted; });
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
    QObject::connect(chrome, &ChromeController::pasteAnswered, window, [&](bool paste) { answers << paste; });
    chrome->askPaste(QStringLiteral("one\ntwo"), 2);
    ASSERT_TRUE(waitFor([&] { return chromeFocusReady(layer); }));
    auto* cancel = chromeItemWithText(layer, QStringLiteral("Cancel"));
    ASSERT_NE(cancel, nullptr);
    cancel->forceActiveFocus();
    // A keybind, D-Bus or a script opens the search under the prompt: the
    // keys must not go back to the panel, where Return pastes.
    chrome->setSearchOpen(true);
    drain();
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
    QObject::connect(chrome, &ChromeController::clipboardAnswered, window, [&](int answer) { answers << answer; });
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
    drain();
    EXPECT_EQ(labels(), before);
}

} // namespace

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
    drain();
    ASSERT_TRUE(window->accessiblePaneRect(second).isEmpty()); // zoomed on first
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    drain();
    // Every pane of the zoomed tab is a card while the overview shows.
    EXPECT_FALSE(window->accessiblePaneRect(second).isEmpty());
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
    drain();
    EXPECT_FALSE(window->openOverview());
    EXPECT_FALSE(window->overviewOpen());
    chrome->setSearchOpen(false);
    drain();
    EXPECT_TRUE(window->openOverview());
    EXPECT_TRUE(window->overviewOpen());
}

TEST_F(NativeWindow, PaneCoalescesBellsAndRepeatedTitlesAndUserVars) {
    auto* pane = window->focusedPane();
    int bells = 0;
    int titles = 0;
    int vars = 0;
    QObject::connect(pane, &Pane::bellRang, pane, [&] { ++bells; });
    QObject::connect(pane, &Pane::titleUpdated, pane, [&] { ++titles; });
    QObject::connect(pane, &Pane::userVarUpdated, pane, [&] { ++vars; });
    // A burst of BEL bytes is one bell for the GUI thread, not one per byte.
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.feed("\a\a\a\a\a\a\a\a"); });
    drain();
    EXPECT_EQ(bells, 1);
    // cat writes the input back: the same title and user variable, twice.
    // The line end lets the canonical-mode PTY hand the line to cat.
    const std::string_view twice = "\x1b]2;same\x07\x1b]2;same\x07"
                                   "\x1b]1337;SetUserVar=k=dg==\x07\x1b]1337;SetUserVar=k=dg==\x07\n";
    ASSERT_TRUE(pane->sendInput(twice));
    ASSERT_TRUE(waitFor([&] { return pane->title() == QStringLiteral("same") && vars > 0; }));
    drain(200);
    EXPECT_EQ(titles, 1);
    EXPECT_EQ(vars, 1);
    pane->setUserTitle(QStringLiteral("mine"));
    pane->setUserTitle(QStringLiteral("mine"));
    EXPECT_EQ(titles, 2);
}

TEST_F(NativeWindow, KeyMappingKeepsKeypadAndShiftedKeysUnderCtrl) {
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

// --- window-core: focus, keys, clipboard prompts, splits and menus ---------

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
    drain();
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
    drain();
    EXPECT_EQ(window->focusedPane(), third);
    EXPECT_EQ(destination->panes(), QList<Pane*>{background});
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
    auto* left = window->splitPane(first, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(left, nullptr);
    auto* right = window->splitPane(left, SplitTree::Direction::Right, launch, nullptr);
    ASSERT_NE(right, nullptr);
    window->revealPane(left);
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
    drain();
    ASSERT_TRUE(window->accessiblePaneRect(first).isEmpty()); // zoomed on last
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
    QObject::connect(window, &TerminalWindow::scriptKey, window, [&](const QString&, quint32, const QVariantMap&) { ++keys; });
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
    drain(300);
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

// --- window wave 2: pane callbacks, directories, replies, overview ---------

TEST_F(NativeWindow, StatusAndContextChangesOnTheGuiThreadDoNotDeadlock) {
    auto* pane = window->focusedPane();
    int statuses = 0;
    int contexts = 0;
    QObject::connect(pane, &Pane::statusChanged, pane, [&] { ++statuses; });
    QObject::connect(pane, &Pane::contextUpdated, pane, [&] { ++contexts; });
    // The receivers read status() and contextKind(), which lock the pane:
    // a direct emission under withTerminal would never return.
    pane->withTerminal([](AfterglowVt::Terminal& t) {
        t.feed("\x1b]7501;state=working:progress=40\x1b\\");
        t.feed("\x1b]3008;start=w2;type=remote;user=someone;hostname=far-away-host\x1b\\");
    });
    pane->withTerminal([](AfterglowVt::Terminal& t) { t.reset(); });
    EXPECT_TRUE(waitFor([&] { return statuses > 0 && contexts > 0; }));
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

TEST_F(NativeWindow, SelectionAndSearchFollowLinesThatInsertAndDeleteMove) {
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
    QObject::connect(window, &TerminalWindow::scriptKey, window, [&](const QString&, quint32, const QVariantMap&) { ++keys; });
    sendKey(window, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"));
    EXPECT_EQ(keys, 0);
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
    previousHandler = qInstallMessageHandler(recordQmlErrors);
    // The plugin paths, the layer-shell integration and the names, as afterglow sets them.
    prepareApplication();
    QApplication app(argc, argv);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "NativeWindowTest.moc"
