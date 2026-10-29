// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Notifications.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>

namespace AfterglowVt {

namespace {

constexpr std::size_t MaxChunkEncoded = 4096;
constexpr std::size_t MaxText = 64 * 1024; // title, body and buttons together
constexpr std::size_t MaxIcon = 4 * 1024 * 1024;
constexpr std::size_t MaxPending = 32;

/// Standard base64; padding optional (the last chunk may lack it).
std::optional<std::string> decodeBase64(std::string_view in)
{
    static constexpr auto table = [] {
        std::array<std::int8_t, 256> t{};
        t.fill(-1);
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            t[static_cast<unsigned char>(chars[i])] = std::int8_t(i);
        }
        return t;
    }();
    while (!in.empty() && in.back() == '=') {
        in.remove_suffix(1);
    }
    std::string out;
    out.reserve(in.size() * 3 / 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        const std::int8_t v = table[static_cast<unsigned char>(ch)];
        if (v < 0) {
            return std::nullopt;
        }
        acc = (acc << 6) | std::uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(char((acc >> bits) & 0xFF));
        }
    }
    return out;
}

/// Escape-code safe UTF-8: valid, and no C0, DEL or C1 controls.
bool isSafeText(std::string_view s)
{
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        const int extra = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0) {
            return false;
        }
        char32_t cp = extra == 0 ? b : b & (0x3F >> extra);
        for (int k = 1; k <= extra; ++k) {
            if (i + std::size_t(k) >= s.size()) {
                return false;
            }
            const auto c = static_cast<unsigned char>(s[i + std::size_t(k)]);
            if ((c & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (c & 0x3F);
        }
        static constexpr char32_t MinForLength[] = {0, 0x80, 0x800, 0x10000};
        if (cp < MinForLength[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
            return false;
        }
        i += std::size_t(extra) + 1;
    }
    return true;
}

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

} // namespace

std::string NotificationAssembler::sanitizeId(std::string_view id)
{
    std::string out;
    for (char c : id.substr(0, 128)) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'
            || c == '+' || c == '.') {
            out += c;
        }
    }
    return out;
}

std::string NotificationAssembler::queryReply(std::string_view id, std::string_view capabilities)
{
    std::string r = "\x1b]99;i=";
    r += id.empty() ? std::string_view("0") : id;
    r += ":p=?;";
    r += capabilities;
    r += "\x1b\\";
    return r;
}

NotificationAssembler::Result NotificationAssembler::feed(std::string_view payload, NotificationRequest& out)
{
    const auto semi = payload.find(';');
    if (semi == std::string_view::npos) {
        return Result::Ignored;
    }
    const std::string_view metadata = payload.substr(0, semi);
    const std::string_view data = payload.substr(semi + 1);
    if (data.size() > MaxChunkEncoded * 2) {
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
                id = sanitizeId(value);
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
            default:
                keys.emplace_back(key, value);
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
    const bool known = type == "title" || type == "body" || type == "buttons" || type == "icon";
    if (!known) {
        return Result::Ignored;
    }

    auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        if (m_pending.size() >= MaxPending) {
            // Too many half-sent notifications: drop the oldest.
            auto oldest = std::min_element(m_pending.begin(), m_pending.end(),
                                           [](const auto& a, const auto& b) { return a.second.serial < b.second.serial; });
            m_pending.erase(oldest);
        }
        it = m_pending.emplace(id, Pending{}).first;
        it->second.request.id = id;
    }
    Pending& p = it->second;
    p.serial = ++m_serial;
    NotificationRequest& r = p.request;

    // Metadata of every chunk applies; later values win.
    for (const auto& [key, value] : keys) {
        auto decoded = [&value]() { return decodeBase64(value).value_or(std::string{}); };
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
            r.reportClose = value == "1";
            break;
        case 'f':
            if (std::string s = decoded(); isSafeText(s)) {
                r.appName = s.substr(0, 256);
            }
            break;
        case 'g':
            r.iconCacheId = sanitizeId(value);
            break;
        case 'n':
            if (std::string s = decoded(); isSafeText(s) && !s.empty() && r.iconNames.size() < 16) {
                r.iconNames.push_back(s.substr(0, 256));
            }
            break;
        case 'o':
            r.occasion = value == "unfocused" ? NotificationRequest::Occasion::Unfocused
                : value == "invisible"        ? NotificationRequest::Occasion::Invisible
                                              : NotificationRequest::Occasion::Always;
            break;
        case 's':
            if (std::string s = decoded(); isSafeText(s) && !s.empty()) {
                r.sound = s.substr(0, 256);
            }
            break;
        case 't':
            if (std::string s = decoded(); isSafeText(s) && !s.empty() && r.types.size() < 16) {
                r.types.push_back(s.substr(0, 256));
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
    std::string text;
    if (encoded || type == "icon") {
        auto decoded = decodeBase64(data);
        if (!decoded) {
            m_pending.erase(it);
            return Result::Ignored;
        }
        text = std::move(*decoded);
    } else {
        text = std::string(data);
    }
    p.bytes += text.size();
    if (type == "icon") {
        if (r.iconData.size() + text.size() > MaxIcon) {
            m_pending.erase(it);
            return Result::Ignored;
        }
        r.iconData += text;
    } else {
        if (!isSafeText(text) || p.bytes > MaxText + MaxIcon) {
            m_pending.erase(it);
            return Result::Ignored;
        }
        std::string& target = type == "title" ? r.title : type == "body" ? r.body : p.buttons;
        if (target.size() + text.size() > MaxText) {
            m_pending.erase(it);
            return Result::Ignored;
        }
        target += text;
    }

    if (!done) {
        return Result::Pending;
    }
    out = std::move(r);
    out.buttons = splitButtons(p.buttons);
    m_pending.erase(it);
    if (out.title.empty()) {
        out.title = std::move(out.body); // no title: the body becomes it
        out.body.clear();
    }
    if (out.title.empty()) {
        return Result::Ignored; // neither title nor body
    }
    return Result::Ready;
}

} // namespace AfterglowVt
