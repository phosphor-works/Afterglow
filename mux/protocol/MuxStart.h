// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxConnect.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

// How a client starts the mux daemon when none answers: `afterglow mux new`
// and `attach`; the projects and new sessions of afterglow, which fork the
// daemon from a worker thread of the GUI process (MuxSessions); and
// `aglow-mux proxy --start` on another host. No Qt: the proxy runs where
// only the Qt-free build is.

namespace AfterglowMux {

/// How long a client that started the daemon waits for its answer by
/// default: the default of the Afterglow setting Mux.startTimeout, which
/// ConfigTest checks. The wait ends at the answer: a daemon that starts in
/// milliseconds on an idle computer can take seconds on a busy one.
inline constexpr std::chrono::milliseconds DefaultStartWait{15000};

/// A connection to the daemon of @p sockets, which this starts from
/// @p daemon (the path of aglow-mux) when none answers: in its own
/// systemd user unit when the user manager outlives the logins, else in a
/// session of its own. Waits up to DefaultStartWait for it to answer, and a
/// last look at the unit and at the sockets after that up to 1 second more.
/// Nothing, with @p error, when it does not. SIGCHLD must have its default
/// action: the start waits for its children. The daemon gets the default
/// action of each signal and an empty signal mask, not those of the calling
/// thread.
Reached connectOrStart(const AfterglowProto::MuxSockets& sockets, const std::string& daemon, std::string* error);

/// connectOrStart() with @p environment in place of this process's, which
/// the start reads and passes on, its PATH included: for a thread other
/// than the one that changes the environment. Waits up to @p wait.
Reached connectOrStart(const AfterglowProto::MuxSockets& sockets, const std::string& daemon,
                       const Environment& environment, std::string* error,
                       std::chrono::milliseconds wait = DefaultStartWait);

/// The systemd user manager as connectOrStart() uses it. A test gives its
/// own, to run the start through a unit without the user's manager.
struct UserManager
{
    /// The daemon starts in a unit of the user manager: it outlives the
    /// logins, and the runtime directory is the user's own.
    bool runs = false;
    /// Runs @p argv (systemd-run, systemctl) with its standard streams at
    /// /dev/null, or its standard output in @p output, up to @p deadline,
    /// when it is killed: its exit status; TimedOut when it was killed at
    /// the deadline; -1 when it did not run or a signal ended it.
    std::function<int(const std::vector<std::string>& argv, std::chrono::steady_clock::time_point deadline,
                      std::string* output)>
        run;
    /// What run() returns for a program that it killed at the deadline: the
    /// user manager did not answer, which tells nothing about the unit.
    static constexpr int TimedOut = -2;
};
/// The user manager of @p environment, whose programs run in it.
UserManager userManagerOf(const Environment& environment);
/// connectOrStart() with @p manager in place of userManagerOf(@p environment).
Reached connectOrStart(const AfterglowProto::MuxSockets& sockets, const std::string& daemon,
                       const Environment& environment, const UserManager& manager, std::string* error,
                       std::chrono::milliseconds wait = DefaultStartWait);

} // namespace AfterglowMux
