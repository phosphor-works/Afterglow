// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Notifications.h>

#include "Base64.h"
#include "Utf8.h"

#include <algorithm>
#include <charconv>

namespace AfterglowVt {

namespace {

// The spec caps a chunk at 4096 encoded bytes. Twice that is accepted, to be
// lenient with senders that miscount.
constexpr std::size_t MaxChunkData = 2 * 4096;
constexpr std::size_t MaxMetadata = 4096;
constexpr std::size_t MaxText = 64 * 1024; // title, body and buttons together
constexpr std::size_t MaxIcon = 4 * 1024 * 1024;
constexpr std::size_t MaxPending = 32;
// Icon data that all half-sent notifications hold together.
constexpr std::size_t MaxPendingIcon = 8 * 1024 * 1024;
constexpr std::size_t MaxField = 256; // app name, icon names, sound, types

std::vector<std::string> splitButtons(std::string_view s)
{
    std::vector<std::string> out;
    static constexpr std::string_view Separator = "\xE2\x80\xA8"; // U+2028
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const auto sep = s.find(Separator, pos);
        const std::string_view label = s.substr(pos, sep == std::string_view::npos ? sep : sep - pos);
        if (!label.empty()) {
            out.emplace_back(label);
        }
        if (sep == std::string_view::npos) {
            break;
        }
        pos = sep + Separator.size();
    }
    return out;
}

/// Decodes an encoded chunk that continues @p carry onto @p out. The spec
/// lets a sender cut the encoded text anywhere, so characters short of a
/// whole group stay in @p carry for the next chunk, unless the chunk ends
/// with padding.
bool decodeChunk(std::string& carry, std::string_view data, std::string& out)
{
    std::string joined = carry;
    joined += data;
    const std::size_t whole = joined.ends_with('=') ? joined.size() : joined.size() / 4 * 4;
    if (!detail::base64DecodeAppend(out, std::string_view(joined).substr(0, whole), detail::Base64Check::PaddingOptional)) {
        return false;
    }
    carry = joined.substr(whole);
    return true;
}

/// The formats the protocol allows for icon data. Anything else would reach
/// whatever image plugin the desktop side has installed.
bool isIconFormat(std::string_view data)
{
    return data.starts_with("\x89PNG\r\n\x1a\n") || data.starts_with("\xFF\xD8\xFF") || data.starts_with("GIF87a")
        || data.starts_with("GIF89a");
}

} // namespace

std::string NotificationAssembler::queryReply(std::string_view id, std::string_view capabilities)
{
    std::string r = "\x1b]99;i=";
    r += id.empty() ? std::string_view("0") : id;
    r += ":p=?;";
    r += capabilities;
    r += "\x1b\\";
    return r;
}

void NotificationAssembler::reset()
{
    m_pending.clear();
    m_failed.clear();
    m_pendingIconBytes = 0;
}

std::string& NotificationAssembler::target(Pending& p, Payload payload)
{
    switch (payload) {
    case Payload::Title:
        return p.request.title;
    case Payload::Body:
        return p.request.body;
    case Payload::Buttons:
        return p.buttons;
    case Payload::Icon:
        break;
    }
    return p.request.iconData;
}

void NotificationAssembler::erase(PendingMap::iterator it)
{
    m_pendingIconBytes -= it->second.request.iconData.size();
    m_pending.erase(it);
}

NotificationAssembler::Result NotificationAssembler::feed(std::string_view payload, NotificationRequest& out)
{
    if (payload.size() > MaxMetadata + 1 + MaxChunkData) {
        return Result::Ignored;
    }
    const auto semi = payload.find(';');
    if (semi == std::string_view::npos || semi > MaxMetadata) {
        return Result::Ignored;
    }
    const std::string_view metadata = payload.substr(0, semi);
    const std::string_view data = payload.substr(semi + 1);
    if (data.size() > MaxChunkData) {
        return Result::Ignored;
    }

    // First pass: the id, so the chunk joins the right pending notification.
    std::string id;
    std::string_view type = "title";
    bool done = true;
    bool encoded = false;
    std::size_t pos = 0;
    std::vector<std::pair<char, std::string_view>> keys;
    while (pos <= metadata.size()) {
        const auto colon = metadata.find(':', pos);
        const std::string_view kv = metadata.substr(pos, colon == std::string_view::npos ? colon : colon - pos);
        if (kv.size() >= 2 && kv[1] == '=') {
            const char key = kv[0];
            const std::string_view value = kv.substr(2);
            switch (key) {
            case 'i':
                id = detail::sanitizeId(value);
                break;
            case 'p':
                type = value;
                break;
            case 'd':
                done = value != "0";
                break;
            case 'e':
                encoded = value == "1";
                break;
            case 'a': case 'c': case 'f': case 'g': case 'n': case 'o':
            case 's': case 't': case 'u': case 'w':
                keys.emplace_back(key, value);
                break;
            default:
                break;
            }
        }
        if (colon == std::string_view::npos) {
            break;
        }
        pos = colon + 1;
    }

    if (type == "?") {
        out = NotificationRequest{};
        out.id = id;
        return Result::Query;
    }
    if (type == "close" || type == "alive") {
        out = NotificationRequest{};
        out.kind = type == "close" ? NotificationRequest::Kind::Close : NotificationRequest::Kind::Alive;
        out.id = id;
        if (out.kind == NotificationRequest::Kind::Close && id.empty()) {
            return Result::Ignored; // closing without an id does nothing
        }
        return Result::Ready;
    }
    Payload kind = Payload::Title;
    if (type == "title") {
        kind = Payload::Title;
    } else if (type == "body") {
        kind = Payload::Body;
    } else if (type == "buttons") {
        kind = Payload::Buttons;
    } else if (type == "icon") {
        kind = Payload::Icon;
    } else {
        return Result::Ignored;
    }

    if (const auto failed = std::find(m_failed.begin(), m_failed.end(), id); failed != m_failed.end()) {
        if (done) {
            m_failed.erase(failed);
        }
        return Result::Ignored;
    }

    auto it = m_pending.find(id);
    Pending* existing = it == m_pending.end() ? nullptr : &it->second;
    // A bad chunk ends its own notification and nothing else, and the rest
    // of it is ignored rather than shown as a new one.
    auto reject = [&]() {
        if (it != m_pending.end()) {
            erase(it);
        }
        if (!done) {
            if (m_failed.size() >= MaxPending) {
                m_failed.erase(m_failed.begin());
            }
            m_failed.push_back(id);
        }
        return Result::Ignored;
    };

    // Check the chunk before it changes any state.
    const bool base64 = encoded || kind == Payload::Icon;
    std::string carry;
    std::string flushed; // what an earlier payload left in the carry, completed now
    if (existing && !existing->carry.empty()) {
        if (base64 && existing->carryPayload == kind) {
            carry = existing->carry;
        } else if (!detail::base64DecodeAppend(flushed, existing->carry, detail::Base64Check::PaddingOptional)) {
            return reject();
        }
    }
    std::string text;
    if (base64) {
        if (!decodeChunk(carry, data, text)) {
            return reject();
        }
    } else {
        text = std::string(data);
    }
    const Payload flushedKind = existing ? existing->carryPayload : kind;
    auto added = [&](Payload p) { return (flushedKind == p ? flushed.size() : 0) + (kind == p ? text.size() : 0); };
    const std::size_t iconBytes = (existing ? existing->request.iconData.size() : 0) + added(Payload::Icon);
    const std::size_t textBytes =
        (existing ? existing->request.title.size() + existing->request.body.size() + existing->buttons.size() : 0)
        + added(Payload::Title) + added(Payload::Body) + added(Payload::Buttons);
    if (iconBytes > MaxIcon || textBytes > MaxText
        || (!done && m_pendingIconBytes + added(Payload::Icon) > MaxPendingIcon)) {
        return reject();
    }

    // A notification complete in one chunk is never stored, so it cannot
    // evict a half-sent one.
    Pending single;
    Pending* p = existing;
    if (!p) {
        if (done) {
            p = &single;
        } else {
            if (m_pending.size() >= MaxPending) {
                // Too many half-sent notifications: drop the oldest.
                erase(std::min_element(m_pending.begin(), m_pending.end(),
                                       [](const auto& a, const auto& b) { return a.second.serial < b.second.serial; }));
            }
            it = m_pending.emplace(id, Pending{}).first;
            p = &it->second;
        }
        p->request.id = id;
    }
    p->serial = ++m_serial;
    NotificationRequest& r = p->request;

    // Metadata of every chunk applies; later values win.
    for (const auto& [key, value] : keys) {
        auto decoded = [&value]() { return detail::base64Decode(value, detail::Base64Check::PaddingOptional).value_or(std::string{}); };
        switch (key) {
        case 'a': {
            std::size_t vpos = 0;
            while (vpos <= value.size()) {
                const auto comma = value.find(',', vpos);
                const std::string_view action = value.substr(vpos, comma == std::string_view::npos ? comma : comma - vpos);
                if (action == "focus") {
                    r.focusOnActivate = true;
                } else if (action == "-focus") {
                    r.focusOnActivate = false;
                } else if (action == "report") {
                    r.reportActivation = true;
                } else if (action == "-report") {
                    r.reportActivation = false;
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                vpos = comma + 1;
            }
            break;
        }
        case 'c':
            r.reportClose = !value.empty() && value != "0";
            break;
        case 'f':
            if (std::string s = decoded(); !s.empty() && detail::isCleanText(s)) {
                r.appName = detail::truncateUtf8(s, MaxField);
            }
            break;
        case 'g':
            r.iconCacheId = detail::sanitizeId(value);
            break;
        case 'n':
            if (std::string s = decoded(); !s.empty() && detail::isCleanText(s) && r.iconNames.size() < 16) {
                r.iconNames.emplace_back(detail::truncateUtf8(s, MaxField));
            }
            break;
        case 'o':
            r.occasion = value == "unfocused" ? NotificationRequest::Occasion::Unfocused
                : value == "invisible"        ? NotificationRequest::Occasion::Invisible
                                              : NotificationRequest::Occasion::Always;
            break;
        case 's':
            if (std::string s = decoded(); !s.empty() && detail::isCleanText(s)) {
                r.sound = detail::truncateUtf8(s, MaxField);
            }
            break;
        case 't':
            if (std::string s = decoded(); !s.empty() && detail::isCleanText(s) && r.types.size() < 16) {
                r.types.emplace_back(detail::truncateUtf8(s, MaxField));
            }
            break;
        case 'u': {
            int u = 1;
            std::from_chars(value.data(), value.data() + value.size(), u);
            r.urgency = std::clamp(u, 0, 2);
            break;
        }
        case 'w': {
            int w = -1;
            std::from_chars(value.data(), value.data() + value.size(), w);
            r.expireMs = std::max(w, -1);
            break;
        }
        default:
            break; // unknown keys are ignored
        }
    }

    // The chunk's payload.
    target(*p, flushedKind) += flushed;
    target(*p, kind) += text;
    if (p != &single) {
        m_pendingIconBytes += added(Payload::Icon);
    }
    p->carry = std::move(carry);
    p->carryPayload = kind;
    if (!done) {
        return Result::Pending;
    }

    // The last chunk. A chunk may end inside a character, so the text is
    // checked whole, after the carry is complete.
    if (!p->carry.empty()) {
        std::string tail;
        if (!detail::base64DecodeAppend(tail, p->carry, detail::Base64Check::PaddingOptional)) {
            return reject();
        }
        target(*p, p->carryPayload) += tail;
        if (p != &single && p->carryPayload == Payload::Icon) {
            m_pendingIconBytes += tail.size(); // counted in, as the subtraction below takes the whole icon
        }
    }
    if (p != &single) {
        m_pendingIconBytes -= r.iconData.size();
    }
    out = std::move(r);
    const std::string buttons = std::move(p->buttons);
    if (p != &single) {
        m_pending.erase(it);
    }
    // Tabs in a body become spaces; newlines stay, the body is shown on several lines.
    std::replace(out.body.begin(), out.body.end(), '\t', ' ');
    if (!detail::isCleanText(out.title, detail::TextAllow::LineSeparators)
        || !detail::isCleanText(out.body, detail::TextAllow::Newline | detail::TextAllow::LineSeparators)
        || !detail::isCleanText(buttons, detail::TextAllow::LineSeparators)) {
        return Result::Ignored;
    }
    out.buttons = splitButtons(buttons);
    if (!isIconFormat(out.iconData)) {
        out.iconData.clear();
    }
    if (out.title.empty()) {
        // No title: the body becomes it, on one line.
        out.title = std::move(out.body);
        out.body.clear();
        std::replace(out.title.begin(), out.title.end(), '\n', ' ');
    }
    if (out.title.empty()) {
        return Result::Ignored; // neither title nor body
    }
    return Result::Ready;
}

} // namespace AfterglowVt
