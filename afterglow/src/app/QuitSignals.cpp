// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "QuitSignals.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>

#include <fcntl.h>
#include <unistd.h>

Q_DECLARE_LOGGING_CATEGORY(lcApp)

namespace Afterglow {

namespace {

constexpr int Handled[] = {SIGTERM, SIGINT, SIGHUP};

// The handler may only make async-signal-safe calls: it hands the signal to
// the event loop through a pipe.
int g_pipe[2] = {-1, -1};
pid_t g_owner = 0;

void setDefault()
{
    struct sigaction fallback{};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    for (const int sig : Handled) sigaction(sig, &fallback, nullptr);
}

void handle(int sig)
{
    const int saved = errno;
    // A child between fork and its own signal reset runs this handler too.
    // It must act as if the handler were not there.
    if (getpid() != g_owner) {
        setDefault();
        raise(sig);
        errno = saved;
        return;
    }
    // The next signal ends the process, whatever the shutdown is doing.
    setDefault();
    const unsigned char byte = static_cast<unsigned char>(sig);
    [[maybe_unused]] const ssize_t written = write(g_pipe[1], &byte, 1);
    errno = saved;
}

} // namespace

QuitSignals::QuitSignals(QObject* parent)
    : QObject(parent)
{
    if (g_pipe[0] >= 0 || pipe2(g_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        qCWarning(lcApp) << "cannot follow termination signals: they end Afterglow without a clean exit";
        return;
    }
    g_owner = getpid();
    m_notifier = new QSocketNotifier(g_pipe[0], QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &QuitSignals::deliver);
    struct sigaction action{};
    action.sa_handler = handle;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (const int sig : Handled) sigaction(sig, &action, nullptr);
}

QuitSignals::~QuitSignals()
{
    if (!m_notifier) return;
    setDefault();
    close(g_pipe[0]);
    close(g_pipe[1]);
    g_pipe[0] = g_pipe[1] = -1;
}

void QuitSignals::deliver()
{
    unsigned char byte = 0;
    if (read(g_pipe[0], &byte, 1) != 1) return;
    qCInfo(lcApp) << "closing on signal" << int(byte);
    Q_EMIT received(int(byte));
    QCoreApplication::quit();
}

} // namespace Afterglow
