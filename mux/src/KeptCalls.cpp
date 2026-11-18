// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "KeptCalls.h"

#include <utility>

namespace AfterglowMux {

void KeptCalls::setLimit(std::size_t count)
{
    m_limit = count;
    while (m_calls.size() > m_limit)
        popFront();
}

void KeptCalls::ring(Host::Kind kind)
{
    if (kind == Host::Kind::Bell)
        m_bell = true;
    else if (kind == Host::Kind::Flash)
        m_flash = true;
}

void KeptCalls::popFront()
{
    m_bytes -= m_calls.front().encoded.size();
    m_calls.pop_front();
}

void KeptCalls::keep(const Host::Call& call, std::string encoded)
{
    using Request = AfterglowVt::NotificationRequest;
    std::string id;
    if (call.kind == Host::Kind::DesktopNotification) {
        if (call.notification.kind == Request::Kind::Alive)
            return;
        if (!call.notification.id.empty()) {
            for (auto it = m_calls.begin(); it != m_calls.end();) {
                if (it->kind == Host::Kind::DesktopNotification && it->id == call.notification.id) {
                    m_bytes -= it->encoded.size();
                    it = m_calls.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (call.notification.kind == Request::Kind::Close)
            return;
        id = call.notification.id;
    } else if (call.kind != Host::Kind::Notify) {
        return;
    }
    if (m_limit == 0 || encoded.size() > MaxBytes)
        return;
    while (!m_calls.empty() && (m_calls.size() >= m_limit || m_bytes + encoded.size() > MaxBytes))
        popFront();
    const std::size_t bytes = encoded.size();
    m_calls.push_back(Call{call.kind, std::move(encoded), std::move(id)});
    m_bytes += bytes;
}

} // namespace AfterglowMux
