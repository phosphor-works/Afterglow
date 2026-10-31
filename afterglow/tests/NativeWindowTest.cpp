// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Application.h"
#include "desktop/DragDrop.h"
#include "desktop/FileTransfer.h"
#include <QMessageBox>
#include <QJsonArray>
#include "chrome/ChromeController.h"
#include "chrome/ChromeLayer.h"
#include "config/Keybinds.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QInputMethodEvent>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QFile>
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
#include <thread>
#include <gtest/gtest.h>
using namespace Afterglow;
namespace {
void drain(int ms = 100) {
    QElapsedTimer timer; timer.start();
    do { QCoreApplication::processEvents(); QThread::msleep(2); } while (timer.elapsed() < ms);
}
QList<QQuickItem*> visualDescendants(QQuickItem* root) {
    QList<QQuickItem*> result;
    for (auto* child : root->childItems()) {
        result.append(child);
        result.append(visualDescendants(child));
    }
    return result;
}
class NativeWindow : public ::testing::Test {
protected:
    QTemporaryDir directory;
    std::unique_ptr<Application> application;
    TerminalWindow* window = nullptr;
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        application = std::make_unique<Application>(directory.filePath(QStringLiteral("afterglow.conf")),
            QJsonObject{{QStringLiteral("Window"), QJsonObject{{QStringLiteral("paneTitles"), true}}}});
        WindowOptions options;
        options.command = {QStringLiteral("/bin/sh"), QStringLiteral("-c"), QStringLiteral("stty -echo; cat")};
        window = application->newWindow(options);
        ASSERT_NE(window, nullptr);
        window->resize(900, 600);
        window->show();
        drain(200);
    }
    void TearDown() override { application.reset(); drain(); }
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
    drain(150);
    EXPECT_TRUE(original->text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("detached-process-alive")));
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
    drain(150);
    auto* added = window->focusedPane();
    EXPECT_EQ(added->id(), created);
    EXPECT_EQ(added->workingDirectory(), directory.path());
    EXPECT_TRUE(added->text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("native-options|unset")));
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
    for (auto* pane : {first, second}) {
        window->revealPane(pane);
        ASSERT_TRUE(window->runAction(pane, Action::TogglePaneZoom));
        drain();
        auto* chrome = window->findChild<ChromeController*>();
        ASSERT_NE(chrome, nullptr);
        EXPECT_EQ(chrome->paneTitles().size(), 1);
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
    drain();
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(layer->focusedItem(), nullptr);
    EXPECT_TRUE(layer->focusedItem()->isVisible());
    window->revealPane(second);
    QKeyEvent accept(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &accept);
    EXPECT_FALSE(chrome->pasteConfirmOpen());
    drain(150);
    EXPECT_TRUE(first->text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("original-pane-line-one")));
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
    int literalLabels = 0;
    for (auto* item : visualDescendants(layer->rootItem())) {
        if (item->property("text").toString() == title && item->property("textFormat").isValid()) {
            EXPECT_EQ(item->property("textFormat").toInt(), 0); // Text.PlainText
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
        EXPECT_EQ(item->property("text").toString(), QStringLiteral("<span>") + title.toHtmlEscaped() + QStringLiteral("</span>"));
        auto* accessible = QAccessible::queryAccessibleInterface(item);
        ASSERT_NE(accessible, nullptr);
        EXPECT_EQ(accessible->text(QAccessible::Name), title);
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
    drain(200);
    first->refreshSnapshot();
    ASSERT_EQ(chrome->searchStatus(), QStringLiteral("1 of 1"));
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
    ASSERT_EQ(chrome->paneTitles().size(), 1);
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
    drain();
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &up);
    auto* layer = window->findChild<ChromeLayer*>();
    ASSERT_NE(layer->rootItem(), nullptr);
    QString selected;
    for (auto* item : visualDescendants(layer->rootItem()))
        if (item->property("literalTitle").isValid() && item->property("highlighted").toBool())
            selected = item->property("literalTitle").toString();
    EXPECT_EQ(selected, QStringLiteral("2"));
}

TEST_F(NativeWindow, LazyChromeInitializesOverlayFocusAndTitle) {
    for (const auto& kind : {QStringLiteral("title"), QStringLiteral("palette"), QStringLiteral("search"),
                             QStringLiteral("paste"), QStringLiteral("clipboard"), QStringLiteral("confirm")}) {
        SCOPED_TRACE(kind.toStdString());
        WindowOptions options;
        options.command = {QStringLiteral("/bin/cat")};
        auto* fresh = application->newWindow(options);
        ASSERT_NE(fresh, nullptr);
        drain();
        auto* chrome = fresh->findChild<ChromeController*>();
        if (kind == QLatin1String("title")) chrome->askTitle(QStringLiteral("Title"), QStringLiteral("existing title"));
        else if (kind == QLatin1String("palette")) chrome->setPaletteOpen(true);
        else if (kind == QLatin1String("search")) chrome->setSearchOpen(true);
        else if (kind == QLatin1String("paste")) chrome->askPaste(QStringLiteral("one\ntwo"), 2);
        else if (kind == QLatin1String("clipboard")) chrome->askClipboard(QStringLiteral("Read clipboard"), QString());
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
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(fresh, &escape);
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
    drain(200);
    ASSERT_FALSE(observer.updates.isEmpty());
    EXPECT_TRUE(observer.updates.last().value(QStringLiteral("progress-visible")).toBool());
    observer.updates.clear();
    delete contributor;
    drain();
    ASSERT_FALSE(observer.updates.isEmpty());
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
    EXPECT_TRUE(std::any_of(snapshot.graphemes.begin(), snapshot.graphemes.end(), [](const auto& g) { return g == U"é"; }));
    EXPECT_TRUE(std::any_of(snapshot.graphemes.begin(), snapshot.graphemes.end(), [](const auto& g) { return g == U"👩‍💻"; }));
    QInputMethodEvent commit; commit.setCommitString(QStringLiteral("native-ime-accepted\n"));
    QCoreApplication::sendEvent(window, &commit);
    drain(200);
    EXPECT_TRUE(pane->text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("native-ime-accepted")));
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
TEST_F(NativeWindow, AccessibleChromeAndZoomVisibility) {
    QAccessible::setActive(true);
    auto* chrome = window->findChild<ChromeController*>();
    ASSERT_NE(chrome, nullptr);
    chrome->setSearchOpen(true);
    drain();
    auto* root = window->accessibleRoot();
    ASSERT_NE(root, nullptr);
    ASSERT_NE(window->accessibleChrome(), nullptr);
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
    center.handle(first, window, request); drain(60);
    ASSERT_EQ(server.count, 1);
    EXPECT_EQ(server.appName, QStringLiteral("Afterglow"));
    EXPECT_EQ(server.appIcon, QStringLiteral("com.phosphorworks.Afterglow"));
    EXPECT_EQ(server.desktopEntry, QStringLiteral("com.phosphorworks.Afterglow"));
    EXPECT_LE(server.images.last().height(), 512);
    request.expireMs = 2000;
    request.iconData.clear();
    center.handle(first, window, request); drain(60);
    EXPECT_FALSE(server.images.last().isEmpty());
    PaneLaunchOptions launch;
    launch.command = {QStringLiteral("/bin/cat")};
    auto* second = window->splitPane(first, SplitTree::Direction::Down, launch, nullptr);
    ASSERT_NE(second, nullptr);
    request.id = "another-pane";
    center.handle(second, window, request); drain(160);
    EXPECT_TRUE(server.images.last().isEmpty());
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
    const QString ready = qEnvironmentVariable("AFTERGLOW_NATIVE_DND_READY");
    ASSERT_FALSE(ready.isEmpty()) << "Run through native_window_acceptance.py for real compositor input";
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
    drain(200);
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
    drain(150);
    QList<QMessageBox*> prompts;
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible() && box->windowTitle() == QStringLiteral("File transfer")) prompts.append(box);
    ASSERT_EQ(prompts.size(), 1);
    EXPECT_EQ(prompts.front()->windowHandle()->transientParent(), destination);
    prompts.front()->reject(); drain(50);
}

TEST_F(NativeWindow, KittyDragAndDropClient) {
    const QString ready = qEnvironmentVariable("AFTERGLOW_NATIVE_DND_READY");
    ASSERT_FALSE(ready.isEmpty());
    const QString source = directory.filePath(QStringLiteral("source.txt"));
    const QString destination = directory.filePath(QStringLiteral("dropped.txt"));
    QFile file(source); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.write("Native Kitty drag and drop\n"); file.close();
    PaneLaunchOptions options;
    options.command = {QStringLiteral("kitten"), QStringLiteral("dnd"), QStringLiteral("--drag"), QStringLiteral("text/plain:") + source,
                       QStringLiteral("--drop"), QStringLiteral("text/plain:") + destination, QStringLiteral("--drop-anywhere=copy")};
    auto* pane = window->openTab(options, nullptr); ASSERT_NE(pane, nullptr);
    PaneLaunchOptions receiverOptions;
    receiverOptions.command = {QStringLiteral("kitten"), QStringLiteral("dnd"), QStringLiteral("--drop"),
                               QStringLiteral("text/plain:") + destination, QStringLiteral("--drop-anywhere=copy")};
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
    const QString ready = qEnvironmentVariable("AFTERGLOW_NATIVE_DND_READY");
    ASSERT_FALSE(ready.isEmpty());
    const QString source = directory.filePath(QStringLiteral("remote"));
    const QString destination = directory.filePath(QStringLiteral("received"));
    ASSERT_TRUE(QDir().mkpath(source + QStringLiteral("/nested")));
    ASSERT_TRUE(QDir().mkpath(destination));
    QFile file(source + QStringLiteral("/nested/file")); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.write("remote file data"); file.close();
    QFile empty(source + QStringLiteral("/empty")); ASSERT_TRUE(empty.open(QIODevice::WriteOnly)); empty.close();
    ASSERT_EQ(::symlink("nested/file", QFile::encodeName(source + QStringLiteral("/link")).constData()), 0);
    PaneLaunchOptions options;
    options.command = {QStringLiteral("kitten"), QStringLiteral("dnd"), source};
    auto* pane = window->openTab(options, nullptr); ASSERT_NE(pane, nullptr);
    PaneLaunchOptions receiverOptions;
    receiverOptions.command = {QStringLiteral("kitten"), QStringLiteral("dnd"), QStringLiteral("--drop-dest"), destination, QStringLiteral("--drop-anywhere=copy")};
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

}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "NativeWindowTest.moc"

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
    EXPECT_TRUE(chrome->paneTitles().isEmpty());
    // Every pane is a card while the overview is open, zoom or not.
    EXPECT_FALSE(window->accessiblePaneRect(third).isEmpty());
    press(Qt::Key_Right);
    EXPECT_EQ(window->overviewHighlight(), second);
    press(Qt::Key_Right);
    press(Qt::Key_Right);
    EXPECT_EQ(window->overviewHighlight(), first); // wraps
    press(Qt::Key_Left);
    EXPECT_EQ(window->overviewHighlight(), third);
    // Keys never reach the shells while the overview is open.
    press(Qt::Key_X, QStringLiteral("x"));
    press(Qt::Key_Return);
    EXPECT_FALSE(window->overviewOpen());
    EXPECT_EQ(window->focusedPane(), third);
    // The tab was zoomed: it returns zoomed, on the chosen pane.
    EXPECT_TRUE(window->accessiblePaneRect(first).isEmpty());
    EXPECT_FALSE(window->accessiblePaneRect(third).isEmpty());
    EXPECT_TRUE(chrome->keyMode().isEmpty());
    // Escape cancels back to the focused pane; a digit chooses directly.
    ASSERT_TRUE(window->runAction(third, Action::PaneOverview));
    press(Qt::Key_Right);
    press(Qt::Key_Escape);
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
    EXPECT_EQ(window->focusedPane(), first);
    EXPECT_FALSE(window->accessiblePaneRect(first).isEmpty());
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
    // Switching back runs again; the overview suppresses it.
    window->revealPane(first);
    EXPECT_TRUE(window->tabSwitching());
    while (window->tabSwitching() && clock.elapsed() < 6000) drain();
    ASSERT_TRUE(window->runAction(first, Action::PaneOverview));
    window->revealPane(second);
    EXPECT_FALSE(window->tabSwitching());
}
