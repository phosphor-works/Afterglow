// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/ShellContext.h>

#include "Utf8.h"

#include <algorithm>

namespace AfterglowVt {

namespace {

constexpr std::size_t MaxField = 256;

/// A field shown in labels and titles: one with control characters, line
/// separators or invalid UTF-8 could forge a second line, so it is dropped.
std::string field(std::string_view value)
{
    if (!detail::isCleanText(value)) {
        return {};
    }
    return std::string(detail::truncateUtf8(value, MaxField));
}

/// The OSC 3008 context with @p id; never the iTerm2 entry.
auto findContext(std::vector<ShellContext>& stack, std::string_view id)
{
    return std::find_if(stack.begin(), stack.end(), [&](const ShellContext& c) { return !c.iterm2RemoteHost && c.id == id; });
}

void push(std::vector<ShellContext>& stack, ShellContext c)
{
    if (stack.size() >= ShellContexts::MaxDepth) {
        stack.erase(stack.begin());
    }
    stack.push_back(std::move(c));
}

} // namespace

bool ShellContexts::apply(std::string_view payload)
{
    ShellContext c;
    bool start = false;
    bool end = false;
    std::size_t pos = 0;
    while (pos <= payload.size()) {
        const auto semi = payload.find(';', pos);
        const std::string_view pair = payload.substr(pos, semi == std::string_view::npos ? semi : semi - pos);
        const auto eq = pair.find('=');
        if (eq != std::string_view::npos) {
            const std::string_view key = pair.substr(0, eq);
            const std::string_view value = pair.substr(eq + 1);
            if (key == "start") {
                start = true;
                c.id = field(value);
            } else if (key == "end") {
                end = true;
                c.id = field(value);
            } else if (key == "type") {
                c.type = field(value);
            } else if (key == "user") {
                c.user = field(value);
            } else if (key == "hostname") {
                c.hostname = field(value);
            } else if (key == "targetuser") {
                c.targetUser = field(value);
            } else if (key == "targethost") {
                c.targetHost = field(value);
            } else if (key == "container") {
                c.container = field(value);
            } else if (key == "vm") {
                c.vm = field(value);
            }
        }
        if (semi == std::string_view::npos) {
            break;
        }
        pos = semi + 1;
    }
    if (c.id.empty() || start == end) {
        return false;
    }
    const auto it = findContext(m_stack, c.id);
    if (end) {
        const bool found = it != m_stack.end();
        m_stack.erase(it, m_stack.end());
        return found;
    }
    // Starting an open context again updates it in place, and ends the
    // contexts opened inside it (the spec). The iTerm2 entry is not one.
    if (it != m_stack.end()) {
        const auto inner = std::remove_if(it + 1, m_stack.end(), [](const ShellContext& s) { return !s.iterm2RemoteHost; });
        const bool ended = inner != m_stack.end();
        m_stack.erase(inner, m_stack.end());
        if (*it == c) {
            return ended;
        }
        *it = std::move(c);
        return true;
    }
    push(m_stack, std::move(c));
    return true;
}

bool ShellContexts::setRemoteHost(std::string_view userAtHost)
{
    if (!detail::isCleanText(userAtHost)) {
        return false;
    }
    ShellContext c;
    if (!userAtHost.empty()) {
        c.iterm2RemoteHost = true;
        c.type = "remote";
        const auto at = userAtHost.find('@');
        c.user = field(at == std::string_view::npos ? std::string_view{} : userAtHost.substr(0, at));
        c.hostname = field(at == std::string_view::npos ? userAtHost : userAtHost.substr(at + 1));
        c.targetHost = c.hostname;
    }
    const auto it = std::find_if(m_stack.begin(), m_stack.end(), [](const ShellContext& s) { return s.iterm2RemoteHost; });
    if (it != m_stack.end() && *it == c) {
        return false; // shell integration repeats it at every prompt
    }
    // Only the earlier RemoteHost goes: contexts opened after it, such as the
    // one `afterglow ssh` reports, stay open.
    const bool removed = it != m_stack.end();
    if (removed) {
        m_stack.erase(it);
    }
    if (userAtHost.empty()) {
        return removed;
    }
    push(m_stack, std::move(c));
    return true;
}

} // namespace AfterglowVt
