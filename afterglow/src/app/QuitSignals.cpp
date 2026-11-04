// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "QuitSignals.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QSocketNotifier>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>

#include <fcntl.h>
#include <unistd.h>

Q_DECLARE_LOGGING_CATEGORY(lcApp)

namespace Afterglow {

namespace {

constexpr int Handled[] = {SIGTERM, SIGINT, SIGHUP};
constexpr std::size_t HandledCount = std::size(Handled);

// The handler may only make async-signal-safe calls: it hands the signal to
// the event loop through a pipe. The pipe lives as long as the process, so a
// handler that runs on another thread while the instance goes still writes to
// it and never to a closed or reused descriptor.
int g_pipe[2] = {-1, -1};
static_assert(std::atomic<pid_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);
std::atomic<pid_t> g_owner{0};
// What each signal did before, and whether the instance handles it. Written
// before the handler is installed, and only read by it.
struct sigaction g_previous[HandledCount];
std::atomic<bool> g_handling[HandledCount];
bool g_active = false;

std::size_t indexOf(int sig)
{
    for (std::size_t i = 0; i < HandledCount; ++i) {
        if (Handled[i] == sig) return i;
    }
    return HandledCount;
}

void restore()
{
    for (std::size_t i = 0; i < HandledCount; ++i) {
        if (g_handling[i].exchange(false)) sigaction(Handled[i], &g_previous[i], nullptr);
    }
}

void handle(int sig)
{
    const int saved = errno;
    // A child between fork and its own signal reset runs this handler too.
    // It must act as if the handler were not there.
    if (getpid() != g_owner.load()) {
        restore();
        raise(sig);
        errno = saved;
        return;
    }
    // The next signal takes the earlier action, whatever the shutdown is doing.
    restore();
    const unsigned char byte = static_cast<unsigned char>(sig);
    [[maybe_unused]] const ssize_t written = write(g_pipe[1], &byte, 1);
    errno = saved;
}

} // namespace

QuitSignals::QuitSignals(QObject* parent)
    : QObject(parent)
{
    if (g_active || (g_pipe[0] < 0 && pipe2(g_pipe, O_CLOEXEC | O_NONBLOCK) != 0)) {
        qCWarning(lcApp) << "cannot follow termination signals: they end Afterglow without a clean exit";
        return;
    }
    // A signal that came after the last instance went.
    unsigned char stale = 0;
    while (read(g_pipe[0], &stale, 1) == 1) {
    }
    g_active = true;
    g_owner = getpid();
    m_notifier = new QSocketNotifier(g_pipe[0], QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &QuitSignals::deliver);
    struct sigaction action{};
    action.sa_handler = handle;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (std::size_t i = 0; i < HandledCount; ++i) {
        sigaction(Handled[i], nullptr, &g_previous[i]);
        if (g_previous[i].sa_handler == SIG_IGN) continue;
        g_handling[i] = true;
        sigaction(Handled[i], &action, nullptr);
    }
}

QuitSignals::~QuitSignals()
{
    if (!m_notifier) return;
    restore();
    g_active = false;
}

void QuitSignals::deliver()
{
    unsigned char byte = 0;
    if (read(g_pipe[0], &byte, 1) != 1 || indexOf(byte) == HandledCount) return;
    qCInfo(lcApp) << "closing on signal" << int(byte);
    if (m_signal == 0) m_signal = byte;
    QCoreApplication::quit();
}

} // namespace Afterglow
