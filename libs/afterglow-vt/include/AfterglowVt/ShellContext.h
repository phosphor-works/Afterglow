// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/afterglowvt_export.h>

#include <string>
#include <string_view>
#include <vector>

namespace AfterglowVt {

/// One context a pane's output comes from (systemd's OSC 3008): a shell, a
/// command, an elevated (sudo, run0) or remote (ssh) session, a container.
/// https://uapi-group.org/specifications/specs/osc_context/
struct ShellContext
{
    std::string id;
    std::string type; ///< boot, container, vm, elevate, chpriv, subcontext, remote, shell, command, app, service, session
    std::string user;
    std::string hostname;
    std::string targetUser; ///< elevate, chpriv: who the context becomes
    std::string targetHost; ///< remote: where it connects
    std::string container;
    std::string vm;
};

/// The open contexts, outermost first. Contexts nest: ending one ends any
/// opened inside it.
class AFTERGLOWVT_EXPORT ShellContexts
{
public:
    static constexpr std::size_t MaxDepth = 64;

    /// Applies an OSC 3008 payload. Returns true if the contexts changed.
    bool apply(std::string_view payload);
    /// iTerm2's RemoteHost (OSC 1337): user@host, empty to clear.
    bool setRemoteHost(std::string_view userAtHost);
    void clear() { m_stack.clear(); }

    const std::vector<ShellContext>& stack() const noexcept { return m_stack; }

private:
    std::vector<ShellContext> m_stack;
};

} // namespace AfterglowVt
