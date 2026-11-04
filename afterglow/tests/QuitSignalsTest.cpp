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
        EXPECT_FALSE(isDefault(sig));
        int received = 0;
        QObject::connect(&quit, &QuitSignals::received, [&received](int s) { received = s; });
        // The fallback ends a loop that the signal did not quit.
        bool timedOut = false;
        QTimer::singleShot(0, [sig] { raise(sig); });
        QTimer::singleShot(3000, [&timedOut] {
            timedOut = true;
            QCoreApplication::quit();
        });
        QCoreApplication::exec();
        EXPECT_FALSE(timedOut) << sig;
        EXPECT_EQ(received, sig);
        // A second signal would take the default action: a hung shutdown still ends.
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

TEST(QuitSignals, AForkedChildDiesAsWithoutTheHandler)
{
    QuitSignals quit;
    bool received = false;
    QObject::connect(&quit, &QuitSignals::received, [&received] { received = true; });
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
    EXPECT_FALSE(received);
    EXPECT_FALSE(isDefault(SIGTERM));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
