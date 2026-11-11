// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxHost.h"

#include "MuxCodec.h"

#include <AfterglowProto/Clipboard.h>
#include <AfterglowProto/Text.h>

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
    void varint(std::uint64_t v) { Codec::varint(m_s, v); }
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

class In : public Codec::Reader
{
public:
    using Codec::Reader::Reader;
    std::uint64_t varint() { return Codec::Reader::varint(~std::uint64_t(0)); }
    int signedInt()
    {
        const std::uint32_t u = std::uint32_t(Codec::Reader::varint(0xffffffffu));
        return int(std::int32_t((u >> 1) ^ (0u - (u & 1))));
    }
    bool boolean()
    {
        const std::uint8_t v = byte();
        if (v > 1) fail();
        return v == 1;
    }
    std::string string() { return std::string(take(std::size_t(Codec::Reader::varint(left())))); }
    /// A list of at most @p most items, which the parser's limits bound.
    std::vector<std::string> strings(std::size_t most)
    {
        std::vector<std::string> v;
        const std::uint64_t n = Codec::Reader::varint(most);
        for (std::uint64_t i = 0; i < n && ok(); ++i) v.push_back(string());
        return v;
    }
    std::vector<std::pair<std::string, std::string>> pairs(std::size_t most)
    {
        std::vector<std::pair<std::string, std::string>> v;
        const std::uint64_t n = Codec::Reader::varint(most);
        for (std::uint64_t i = 0; i < n && ok(); ++i) {
            std::string a = string();
            std::string b = string();
            v.emplace_back(std::move(a), std::move(b));
        }
        return v;
    }
};

/// What the terminal's parser lets through: an id it would not strip, and
/// text that it would not refuse.
bool cleanId(std::string_view id)
{
    return AfterglowProto::sanitizeId(id) == id;
}

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
    n.buttons = in.strings(AfterglowVt::NotificationRequest::MaxText);
    n.iconNames = in.strings(AfterglowVt::NotificationRequest::MaxNames);
    n.iconData = in.string();
    n.iconCacheId = in.string();
    n.appName = in.string();
    n.types = in.strings(AfterglowVt::NotificationRequest::MaxNames);
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
    using AfterglowProto::isCleanText;
    namespace Allow = AfterglowProto::TextAllow;
    std::size_t text = n.title.size() + n.body.size();
    for (const std::string& button : n.buttons) {
        if (button.empty() || !isCleanText(button, Allow::LineSeparators)) return false;
        text += button.size();
    }
    if (!cleanId(n.id) || !cleanId(n.iconCacheId) || n.urgency < 0 || n.urgency > 2 || text > AfterglowVt::NotificationRequest::MaxText
        || !isCleanText(n.title, Allow::LineSeparators) || !isCleanText(n.body, Allow::Newline | Allow::LineSeparators)) {
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
    c.mimes = in.strings(AfterglowVt::ClipboardAssembler::MaxParts);
    c.data = in.pairs(AfterglowVt::ClipboardAssembler::MaxParts);
    c.aliases = in.pairs(AfterglowVt::ClipboardAssembler::MaxParts);
    c.osc52 = in.boolean();
    c.osc52Bel = in.boolean();
    c.osc52Selection = in.string();
    if (!in.ok() || kind > std::uint8_t(AfterglowVt::ClipboardRequest::Kind::Write) || !cleanId(c.id)) return false;
    // The parser makes OSC 52 reads only, of the selections that the answer
    // echoes; OSC 5522 has none of the OSC 52 fields.
    if (c.osc52 ? kind != std::uint8_t(AfterglowVt::ClipboardRequest::Kind::Read) || c.osc52Selection.empty()
                      || AfterglowProto::osc52Letters(c.osc52Selection) != c.osc52Selection
                : c.osc52Bel || !c.osc52Selection.empty()) {
        return false;
    }
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
    case Kind::Report:
        o.string(call.text);
        break;
    case Kind::CommandFinished:
    case Kind::ProtocolLost:
        o.signedInt(call.number);
        o.varint(call.value);
        break;
    }
    return out;
}

std::optional<Call> decode(std::string_view bytes)
{
    In in(bytes);
    Call call;
    const std::uint8_t kind = in.byte();
    if (kind < std::uint8_t(Kind::Bell) || kind > std::uint8_t(Kind::ProtocolLost)) return std::nullopt;
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
        // The codes that the terminal passes to its host.
        if (call.number != 0 && call.number != 72 && call.number != 5113) return std::nullopt;
        break;
    case Kind::PointerShape:
    case Kind::CommandStarted:
    case Kind::Report:
        call.text = in.string();
        break;
    case Kind::CommandFinished:
        call.number = in.signedInt();
        call.value = in.varint();
        break;
    case Kind::ProtocolLost:
        call.number = in.signedInt();
        call.value = in.varint();
        if (call.number < LostNothing || call.number > LostPackets || call.value > 1) return std::nullopt;
        break;
    }
    if (!in.ok() || !in.done()) return std::nullopt;
    // The text of OSC 9 and OSC 777, as the parser lets it through.
    if (call.kind == Kind::Notify
        && (!AfterglowProto::isCleanText(call.title, AfterglowProto::TextAllow::LineSeparators)
            || !AfterglowProto::isCleanText(call.text, AfterglowProto::TextAllow::LineSeparators))) {
        return std::nullopt;
    }
    return call;
}

} // namespace AfterglowMux::Host
