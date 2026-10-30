// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/ShellContext.h>

#include <algorithm>

namespace AfterglowVt {

namespace {

constexpr std::size_t MaxField = 256;
constexpr std::string_view RemoteHostId = "iterm2-remote-host";

std::string field(std::string_view value)
{
    return std::string(value.substr(0, MaxField));
}

void eraseFrom(std::vector<ShellContext>& stack, std::string_view id)
{
    const auto it = std::find_if(stack.begin(), stack.end(), [&](const ShellContext& c) { return c.id == id; });
    stack.erase(it, stack.end());
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
    if (end) {
        const std::size_t before = m_stack.size();
        eraseFrom(m_stack, c.id);
        return m_stack.size() != before;
    }
    // Starting an open context again updates it in place.
    if (const auto it = std::find_if(m_stack.begin(), m_stack.end(), [&](const ShellContext& s) { return s.id == c.id; });
        it != m_stack.end()) {
        *it = std::move(c);
        return true;
    }
    if (m_stack.size() >= MaxDepth) {
        m_stack.erase(m_stack.begin());
    }
    m_stack.push_back(std::move(c));
    return true;
}

bool ShellContexts::setRemoteHost(std::string_view userAtHost)
{
    const std::size_t before = m_stack.size();
    eraseFrom(m_stack, RemoteHostId);
    if (userAtHost.empty()) {
        return m_stack.size() != before;
    }
    if (m_stack.size() >= MaxDepth) return m_stack.size() != before;
    ShellContext c;
    c.id = std::string(RemoteHostId);
    c.type = "remote";
    const auto at = userAtHost.find('@');
    c.user = field(at == std::string_view::npos ? std::string_view{} : userAtHost.substr(0, at));
    c.hostname = field(at == std::string_view::npos ? userAtHost : userAtHost.substr(at + 1));
    c.targetHost = c.hostname;
    m_stack.push_back(std::move(c));
    return true;
}

} // namespace AfterglowVt
