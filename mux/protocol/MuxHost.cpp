// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxHost.h"

#include <vector>

namespace AfterglowMux::Host {

namespace {

// Integers are LEB128 varints, a signed one zigzag first; strings and lists
// a varint count and their items.
class Out
{
public:
    explicit Out(std::string& s)
        : m_s(s)
    {
    }
    void byte(std::uint8_t v) { m_s.push_back(char(v)); }
    void varint(std::uint64_t v)
    {
        while (v >= 0x80) {
            m_s.push_back(char((v & 0x7f) | 0x80));
            v >>= 7;
        }
        m_s.push_back(char(v));
    }
    void signedInt(int v) { varint((std::uint32_t(v) << 1) ^ std::uint32_t(v >> 31)); }
    void string(std::string_view v)
    {
        varint(v.size());
        m_s.append(v);
    }
    void strings(const std::vector<std::string>& v)
    {
        varint(v.size());
        for (const auto& s : v) string(s);
    }
    void pairs(const std::vector<std::pair<std::string, std::string>>& v)
    {
        varint(v.size());
        for (const auto& [a, b] : v) {
            string(a);
            string(b);
        }
    }

private:
    std::string& m_s;
};

class In
{
public:
    explicit In(std::string_view s)
        : m_s(s)
    {
    }
    bool ok() const { return m_ok; }
    bool done() const { return m_at == m_s.size(); }
    std::uint8_t byte()
    {
        if (m_at >= m_s.size()) {
            m_ok = false;
            return 0;
        }
        return std::uint8_t(m_s[m_at++]);
    }
    std::uint64_t varint()
    {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64 && m_ok; shift += 7) {
            const std::uint8_t b = byte();
            v |= std::uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        m_ok = false;
        return 0;
    }
    int signedInt()
    {
        const std::uint64_t v = varint();
        if (v > 0xffffffffull) m_ok = false;
        const std::uint32_t u = std::uint32_t(v);
        return int(std::int32_t((u >> 1) ^ (0u - (u & 1))));
    }
    bool boolean()
    {
        const std::uint8_t v = byte();
        if (v > 1) m_ok = false;
        return v == 1;
    }
    std::string string()
    {
        const std::uint64_t n = varint();
        if (!m_ok || n > m_s.size() - m_at) {
            m_ok = false;
            return {};
        }
        std::string v(m_s.substr(m_at, std::size_t(n)));
        m_at += std::size_t(n);
        return v;
    }
    std::vector<std::string> strings()
    {
        std::vector<std::string> v;
        const std::uint64_t n = varint();
        // Each item takes at least its length byte.
        if (n > m_s.size() - m_at) m_ok = false;
        for (std::uint64_t i = 0; i < n && m_ok; ++i) v.push_back(string());
        return v;
    }
    std::vector<std::pair<std::string, std::string>> pairs()
    {
        std::vector<std::pair<std::string, std::string>> v;
        const std::uint64_t n = varint();
        if (n > m_s.size() - m_at) m_ok = false;
        for (std::uint64_t i = 0; i < n && m_ok; ++i) {
            std::string a = string();
            std::string b = string();
            v.emplace_back(std::move(a), std::move(b));
        }
        return v;
    }

private:
    std::string_view m_s;
    std::size_t m_at = 0;
    bool m_ok = true;
};

void encodeNotification(Out& o, const AfterglowVt::NotificationRequest& n)
{
    o.byte(std::uint8_t(n.kind));
    o.string(n.id);
    o.string(n.title);
    o.string(n.body);
    o.strings(n.buttons);
    o.strings(n.iconNames);
    o.string(n.iconData);
    o.string(n.iconCacheId);
    o.string(n.appName);
    o.strings(n.types);
    o.string(n.sound);
    o.byte(n.focusOnActivate);
    o.byte(n.reportActivation);
    o.byte(n.reportClose);
    o.byte(std::uint8_t(n.occasion));
    o.signedInt(n.urgency);
    o.signedInt(n.expireMs);
}

bool decodeNotification(In& in, AfterglowVt::NotificationRequest& n)
{
    const std::uint8_t kind = in.byte();
    n.id = in.string();
    n.title = in.string();
    n.body = in.string();
    n.buttons = in.strings();
    n.iconNames = in.strings();
    n.iconData = in.string();
    n.iconCacheId = in.string();
    n.appName = in.string();
    n.types = in.strings();
    n.sound = in.string();
    n.focusOnActivate = in.boolean();
    n.reportActivation = in.boolean();
    n.reportClose = in.boolean();
    const std::uint8_t occasion = in.byte();
    n.urgency = in.signedInt();
    n.expireMs = in.signedInt();
    if (!in.ok() || kind > std::uint8_t(AfterglowVt::NotificationRequest::Kind::Alive)
        || occasion > std::uint8_t(AfterglowVt::NotificationRequest::Occasion::Invisible)) {
        return false;
    }
    n.kind = AfterglowVt::NotificationRequest::Kind(kind);
    n.occasion = AfterglowVt::NotificationRequest::Occasion(occasion);
    return true;
}

void encodeClipboard(Out& o, const AfterglowVt::ClipboardRequest& c)
{
    o.byte(std::uint8_t(c.kind));
    o.byte(c.primary);
    o.string(c.id);
    o.string(c.password);
    o.string(c.name);
    o.strings(c.mimes);
    o.pairs(c.data);
    o.pairs(c.aliases);
    o.byte(c.osc52);
    o.byte(c.osc52Bel);
    o.string(c.osc52Selection);
}

bool decodeClipboard(In& in, AfterglowVt::ClipboardRequest& c)
{
    const std::uint8_t kind = in.byte();
    c.primary = in.boolean();
    c.id = in.string();
    c.password = in.string();
    c.name = in.string();
    c.mimes = in.strings();
    c.data = in.pairs();
    c.aliases = in.pairs();
    c.osc52 = in.boolean();
    c.osc52Bel = in.boolean();
    c.osc52Selection = in.string();
    if (!in.ok() || kind > std::uint8_t(AfterglowVt::ClipboardRequest::Kind::Write)) return false;
    c.kind = AfterglowVt::ClipboardRequest::Kind(kind);
    return true;
}

} // namespace

std::string encode(const Call& call)
{
    std::string out;
    Out o(out);
    o.byte(std::uint8_t(call.kind));
    switch (call.kind) {
    case Kind::Bell:
    case Kind::Flash:
        break;
    case Kind::Notify:
    case Kind::ClipboardWrite:
    case Kind::UserVar:
    case Kind::Download:
        o.string(call.title);
        o.string(call.text);
        break;
    case Kind::DesktopNotification:
        encodeNotification(o, call.notification);
        break;
    case Kind::ClipboardRequest:
        encodeClipboard(o, call.clipboard);
        break;
    case Kind::TerminalProtocol:
        o.signedInt(call.number);
        o.string(call.text);
        break;
    case Kind::PointerShape:
    case Kind::CommandStarted:
        o.string(call.text);
        break;
    case Kind::CommandFinished:
        o.signedInt(call.number);
        break;
    }
    return out;
}

std::optional<Call> decode(std::string_view bytes)
{
    In in(bytes);
    Call call;
    const std::uint8_t kind = in.byte();
    if (kind < std::uint8_t(Kind::Bell) || kind > std::uint8_t(Kind::Download)) return std::nullopt;
    call.kind = Kind(kind);
    switch (call.kind) {
    case Kind::Bell:
    case Kind::Flash:
        break;
    case Kind::Notify:
    case Kind::ClipboardWrite:
    case Kind::UserVar:
    case Kind::Download:
        call.title = in.string();
        call.text = in.string();
        break;
    case Kind::DesktopNotification:
        if (!decodeNotification(in, call.notification)) return std::nullopt;
        break;
    case Kind::ClipboardRequest:
        if (!decodeClipboard(in, call.clipboard)) return std::nullopt;
        break;
    case Kind::TerminalProtocol:
        call.number = in.signedInt();
        call.text = in.string();
        break;
    case Kind::PointerShape:
    case Kind::CommandStarted:
        call.text = in.string();
        break;
    case Kind::CommandFinished:
        call.number = in.signedInt();
        break;
    }
    if (!in.ok() || !in.done()) return std::nullopt;
    return call;
}

} // namespace AfterglowMux::Host
