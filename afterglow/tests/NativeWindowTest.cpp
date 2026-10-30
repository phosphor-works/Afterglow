// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Application.h"
#include "chrome/ChromeController.h"
#include "chrome/ChromeLayer.h"
#include "config/Keybinds.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QInputMethodEvent>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QFile>
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
#include <gtest/gtest.h>
using namespace Afterglow;
namespace {
void drain(int ms = 100) {
    QElapsedTimer timer; timer.start();
    do { QCoreApplication::processEvents(); QThread::msleep(2); } while (timer.elapsed() < ms);
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
public Q_SLOTS:
    uint Notify(const QString&, uint replaces, const QString&, const QString&, const QString&,
                const QStringList&, const QVariantMap& hints, int) {
        ++count;
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
