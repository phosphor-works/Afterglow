// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/QuitSignals.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>

#include <csignal>

#include <sys/wait.h>
#include <unistd.h>

using namespace Afterglow;

namespace {

bool isDefault(int sig)
{
    struct sigaction current{};
    sigaction(sig, nullptr, &current);
    return current.sa_handler == SIG_DFL;
}

TEST(QuitSignals, ASignalQuitsTheEventLoopOnce)
{
    for (const int sig : {SIGTERM, SIGINT, SIGHUP}) {
        QuitSignals quit;
        // A failed install would let the raise below end the test process.
        ASSERT_FALSE(isDefault(sig));
        // The fallback ends a loop that the signal did not quit.
        QObject guard;
        bool timedOut = false;
        QTimer::singleShot(0, &guard, [sig] { raise(sig); });
        QTimer::singleShot(3000, &guard, [&timedOut] {
            timedOut = true;
            QCoreApplication::quit();
        });
        QCoreApplication::exec();
        EXPECT_FALSE(timedOut) << sig;
        EXPECT_EQ(quit.signal(), sig);
        // A second signal would take the earlier action: a hung shutdown still ends.
        for (const int other : {SIGTERM, SIGINT, SIGHUP}) EXPECT_TRUE(isDefault(other)) << other;
    }
}

TEST(QuitSignals, DestructorRestoresTheDefaults)
{
    {
        QuitSignals quit;
        EXPECT_FALSE(isDefault(SIGTERM));
    }
    for (const int sig : {SIGTERM, SIGINT, SIGHUP}) EXPECT_TRUE(isDefault(sig)) << sig;
}

TEST(QuitSignals, AnIgnoredSignalStaysIgnored)
{
    // nohup ignores SIGHUP: closing the terminal that started Afterglow must
    // not end it.
    std::signal(SIGHUP, SIG_IGN);
    {
        QuitSignals quit;
        struct sigaction current{};
        sigaction(SIGHUP, nullptr, &current);
        EXPECT_EQ(current.sa_handler, SIG_IGN);
        EXPECT_FALSE(isDefault(SIGTERM));
        raise(SIGHUP);
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 100) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        EXPECT_EQ(quit.signal(), 0);
    }
    struct sigaction after{};
    sigaction(SIGHUP, nullptr, &after);
    EXPECT_EQ(after.sa_handler, SIG_IGN);
    std::signal(SIGHUP, SIG_DFL);
}

TEST(QuitSignals, AForkedChildDiesAsWithoutTheHandler)
{
    QuitSignals quit;
    // As a pty child between fork and its signal reset.
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        raise(SIGTERM);
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGTERM);
    // Nothing reached this process's event loop.
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 100) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    EXPECT_EQ(quit.signal(), 0);
    EXPECT_FALSE(isDefault(SIGTERM));
}

} // namespace

int main(int argc, char** argv)
{
    // The tests expect the default actions: a run under nohup, or as a
    // background job of a script, starts with some of them ignored.
    for (const int sig : {SIGTERM, SIGINT, SIGHUP}) std::signal(sig, SIG_DFL);
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
