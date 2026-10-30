// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Clipboard.h>

#include "Base64.h"

#include <AfterglowVt/Notifications.h>

#include <array>
#include <optional>

namespace AfterglowVt {

namespace {

std::string header(std::string_view type, std::string_view status, std::string_view id)
{
    std::string r = "\x1b]5522;type=";
    r += type;
    r += ":status=";
    r += status;
    if (!id.empty()) {
        r += ":id=";
        r += id;
    }
    return r;
}

} // namespace

void ClipboardAssembler::reset()
{
    m_writing = false;
    m_failed = false;
    m_write = {};
    m_mime.clear();
    m_encoded.clear();
    m_total = m_decoded = 0;
}

bool ClipboardAssembler::flush(std::string& reply)
{
    if (m_mime.empty()) {
        return true;
    }
    auto decoded = detail::base64Decode(m_encoded, detail::Base64Check::Strict);
    if (!decoded) {
        reply = writeStatus("EINVAL", m_write.id);
        reset(); m_writing = m_failed = true;
        return false;
    }
    if (decoded->size() > MaxWrite - m_decoded || m_write.data.size() >= 64) {
        reply = writeStatus("EFBIG", m_write.id);
        reset(); m_writing = m_failed = true;
        return false;
    }
    m_decoded += decoded->size();
    m_write.data.emplace_back(std::move(m_mime), std::move(*decoded));
    m_mime.clear();
    m_encoded.clear();
    return true;
}

ClipboardAssembler::Result ClipboardAssembler::feed(std::string_view payload, ClipboardRequest& out, std::string& reply)
{
    const auto semi = payload.find(';');
    const std::string_view metadata = payload.substr(0, semi);
    const std::string_view data = semi == std::string_view::npos ? std::string_view{} : payload.substr(semi + 1);

    if (metadata.size() > 16384) {
        reply = writeStatus("EFBIG", m_write.id);
        reset(); m_writing = m_failed = true;
        return Result::Error;
    }
    std::string_view type;
    std::string mime;
    std::string id;
    std::string password;
    std::string name;
    bool primary = false;
    bool badValue = false;
    std::size_t pos = 0;
    while (pos <= metadata.size()) {
        const auto colon = metadata.find(':', pos);
        const std::string_view kv = metadata.substr(pos, colon == std::string_view::npos ? colon : colon - pos);
        const auto eq = kv.find('=');
        if (eq != std::string_view::npos) {
            const std::string_view key = kv.substr(0, eq);
            const std::string_view value = kv.substr(eq + 1);
            auto decode = [&](std::string& target) {
                if (value.size() > 4096) { badValue = true; return; }
                if (auto d = detail::base64Decode(value, detail::Base64Check::Strict)) {
                    target = std::move(*d);
                } else {
                    badValue = true;
                }
            };
            if (key == "type") {
                type = value;
            } else if (key == "mime") {
                decode(mime);
            } else if (key == "pw") {
                decode(password);
            } else if (key == "name") {
                decode(name);
            } else if (key == "loc") {
                primary = value == "primary";
            } else if (key == "id") {
                id = NotificationAssembler::sanitizeId(value);
            }
        }
        if (colon == std::string_view::npos) {
            break;
        }
        pos = colon + 1;
    }

    if (type == "read") {
        // A read has no way to report bad base64: it is ignored.
        if (data.size() > 65536) return Result::Ignored;
        const auto list = detail::base64Decode(data, detail::Base64Check::Strict);
        if (badValue || !list) {
            return Result::Ignored;
        }
        out = ClipboardRequest{};
        out.kind = ClipboardRequest::Kind::Read;
        out.primary = primary;
        out.id = id;
        out.password = password;
        out.name = password.empty() ? std::string{} : name;
        std::string_view types = *list;
        std::size_t tpos = 0;
        while (tpos < types.size()) {
            const auto space = types.find_first_of(" \n", tpos);
            const std::string_view t = types.substr(tpos, space == std::string_view::npos ? space : space - tpos);
            if (!t.empty() && out.mimes.size() < 64) {
                out.mimes.emplace_back(t);
            }
            if (space == std::string_view::npos) {
                break;
            }
            tpos = space + 1;
        }
        if (out.mimes.empty()) {
            return Result::Ignored;
        }
        return Result::Ready;
    }

    if (type == "write") {
        reset();
        m_writing = true;
        m_write.kind = ClipboardRequest::Kind::Write;
        m_write.primary = primary;
        m_write.id = id;
        m_write.password = password;
        m_write.name = password.empty() ? std::string{} : name;
        if (badValue) {
            m_failed = true;
            reply = writeStatus("EINVAL", id);
            return Result::Error;
        }
        return Result::Pending;
    }

    if (type == "wdata" || type == "walias") {
        if (!m_writing || m_failed) {
            return Result::Ignored;
        }
        auto fail = [&](std::string_view status) {
            reply = writeStatus(status, m_write.id);
            reset(); m_writing = m_failed = true;
            return Result::Error;
        };
        if (badValue) {
            return fail("EINVAL");
        }
        if (type == "walias") {
            if (data.size() > 65536) return fail("EFBIG");
            const auto aliases = detail::base64Decode(data, detail::Base64Check::Strict);
            if (mime.empty() || !aliases) {
                return fail("EINVAL");
            }
            std::string_view list = *aliases;
            std::size_t apos = 0;
            while (apos < list.size()) {
                const auto space = list.find(' ', apos);
                const std::string_view alias = list.substr(apos, space == std::string_view::npos ? space : space - apos);
                if (!alias.empty() && m_write.aliases.size() < 64) {
                    m_write.aliases.emplace_back(alias, mime);
                }
                if (space == std::string_view::npos) {
                    break;
                }
                apos = space + 1;
            }
            return Result::Pending;
        }
        if (mime.empty() && data.empty()) {
            // The end of the transmission.
            if (!flush(reply)) {
                m_failed = true;
                return Result::Error;
            }
            out = std::move(m_write);
            reset();
            return Result::Ready;
        }
        if (mime.empty()) {
            return fail("EINVAL");
        }
        if (mime != m_mime) {
            if (!flush(reply)) {
                m_failed = true;
                return Result::Error;
            }
            m_mime = mime;
        }
        if (data.size() > ((MaxWrite + 2) / 3) * 4 - m_total) {
            return fail("EFBIG");
        }
        m_total += data.size();
        m_encoded += data;
        return Result::Pending;
    }
    return Result::Ignored;
}

std::string ClipboardAssembler::readStatus(std::string_view status, const ClipboardRequest& request,
                                           std::string_view oneTimePassword)
{
    std::string r = header("read", status, request.id);
    if (status == "OK" && request.primary) {
        r += ":loc=primary";
    }
    if (!oneTimePassword.empty()) {
        r += ":pw=" + detail::base64Encode(oneTimePassword);
    }
    r += "\x1b\\";
    return r;
}

std::string ClipboardAssembler::readData(std::string_view mime, std::string_view bytes, const ClipboardRequest& request,
                                         std::string_view oneTimePassword)
{
    std::string out;
    const std::string prefix = header("read", "DATA", request.id) + ":mime=" + detail::base64Encode(mime)
        + (oneTimePassword.empty() ? std::string{} : ":pw=" + detail::base64Encode(oneTimePassword)) + ';';
    std::size_t pos = 0;
    do {
        const std::string_view chunk = bytes.substr(pos, ReadChunk);
        out += prefix;
        out += detail::base64Encode(chunk);
        out += "\x1b\\";
        pos += ReadChunk;
    } while (pos < bytes.size());
    return out;
}

std::string ClipboardAssembler::writeStatus(std::string_view status, std::string_view id)
{
    return header("write", status, id) + "\x1b\\";
}

std::string ClipboardRequest::osc52Reply(std::string_view text) const
{
    std::string r = "\x1b]52;";
    r += osc52Selection;
    r += ';';
    r += detail::base64Encode(text);
    r += osc52Bel ? "\x07" : "\x1b\\";
    return r;
}

} // namespace AfterglowVt
