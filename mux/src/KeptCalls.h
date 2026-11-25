// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxHost.h"
#include "MuxProtocol.h"

#include <cstddef>
#include <deque>
#include <string>

namespace AfterglowMux {

/// What a pane keeps for the next window while no window shows it
/// (MuxPane::setKeptNotifications()): whether a bell and a flash rang, the
/// newest notifications, encoded, oldest first, and whether the program
/// waits for the answer to an upload that no window gave. Not thread-safe:
/// the pane guards it with its lock of host calls.
class KeptCalls
{
public:
    /// The most bytes of the notifications, encoded: one over it keeps no
    /// icon, and one that is still over it goes. A quarter of the smallest
    /// Mux.eventQueue: the next window takes them in its queue, and the
    /// answers of the program's queries still fit there.
    static constexpr std::size_t MaxBytes = std::size_t(MinEventQueue) / 4;

    struct Call
    {
        Host::Kind kind = Host::Kind::Notify;
        std::string encoded;
        std::string id; ///< of an OSC 99 notification, which a later one replaces
    };

    /// The most notifications, 0 for none: the oldest go first.
    void setLimit(std::size_t count);
    /// Keeps @p call, a notification or an OSC 99 close, as @p encoded (as
    /// the next window gets it). An OSC 99 notification with the id of a
    /// kept one replaces it, and a close of that id takes it away, as on a
    /// screen. A query of the notifications on screen keeps nothing.
    void keep(const Host::Call& call, std::string encoded);
    /// A bell or a flash rang: the next window gets one.
    void ring(Host::Kind kind);
    /// The program waits for files that the window that went never gave:
    /// the next window asks again. Nobody answers for the user meanwhile,
    /// not even with Ctrl+C (ITerminalHost::uploadRequested).
    void askUpload()
    {
        m_upload = true;
    }
    /// The command that asked ended, or an answer came after all: no window
    /// asks.
    void dropUpload()
    {
        m_upload = false;
    }

    /// What the next window gets, in its order: the bell, the flash, the
    /// notifications, then the upload's question. Each take leaves what is
    /// kept once the caller queued it.
    bool bell() const
    {
        return m_bell;
    }
    void takeBell()
    {
        m_bell = false;
    }
    bool flash() const
    {
        return m_flash;
    }
    void takeFlash()
    {
        m_flash = false;
    }
    bool empty() const
    {
        return m_calls.empty();
    }
    const Call& front() const
    {
        return m_calls.front();
    }
    void popFront();
    bool upload() const
    {
        return m_upload;
    }
    void takeUpload()
    {
        m_upload = false;
    }

private:
    std::size_t m_limit = std::size_t(DefaultKeptNotifications);
    std::deque<Call> m_calls;
    std::size_t m_bytes = 0; ///< of m_calls
    bool m_bell = false;
    bool m_flash = false;
    bool m_upload = false;
};

} // namespace AfterglowMux
