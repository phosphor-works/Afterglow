// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Clipboard.h>
#include <AfterglowProto/Text.h>

#include <algorithm>
#include <iterator>
#include <optional>

namespace AfterglowProto {

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

constexpr std::size_t MaxMetadata = 16384;
constexpr std::size_t MaxParts = 64; // MIME types, and aliases

/// The value of @p key in "k=v:k=v" metadata.
std::string_view metadataValue(std::string_view metadata, std::string_view key)
{
    std::size_t pos = 0;
    while (pos <= metadata.size()) {
        const auto colon = metadata.find(':', pos);
        const std::string_view kv = metadata.substr(pos, colon == std::string_view::npos ? colon : colon - pos);
        if (kv.size() > key.size() && kv.starts_with(key) && kv[key.size()] == '=') {
            return kv.substr(key.size() + 1);
        }
        if (colon == std::string_view::npos) {
            break;
        }
        pos = colon + 1;
    }
    return {};
}

/// A MIME type or X11 target name: printable ASCII without spaces. These
/// names reach a permission prompt and other programs' clipboards.
bool isTypeName(std::string_view s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c > 0x20 && c < 0x7F; });
}

} // namespace

void ClipboardAssembler::reset()
{
    m_writing = false;
    m_failed = false;
    m_write = {};
    m_mime.clear();
    m_part.clear();
    m_carry.clear();
    m_padded = false;
    m_decoded = 0;
}

ClipboardAssembler::Result ClipboardAssembler::fail(std::string_view status, std::string& reply)
{
    reply = writeStatus(status, m_write.id);
    reset();
    m_writing = m_failed = true;
    return Result::Error;
}

bool ClipboardAssembler::flush(std::string& reply)
{
    if (m_mime.empty()) {
        return true;
    }
    // Only the whole stream of a type must be padded, so a remainder here is
    // base64 that ends short of a group.
    if (!m_carry.empty()) {
        fail("EINVAL", reply);
        return false;
    }
    if (m_write.data.size() >= MaxParts) {
        fail("EFBIG", reply);
        return false;
    }
    m_decoded += m_part.size();
    m_write.data.emplace_back(std::move(m_mime), std::move(m_part));
    m_mime.clear();
    m_part.clear();
    m_padded = false;
    return true;
}

ClipboardAssembler::Result ClipboardAssembler::feed(std::string_view payload, ClipboardRequest& out, std::string& reply)
{
    const auto semi = payload.find(';');
    const std::string_view metadata = payload.substr(0, semi);
    const std::string_view data = semi == std::string_view::npos ? std::string_view{} : payload.substr(semi + 1);

    if (metadata.size() > MaxMetadata) {
        // Too long to parse. Only a write hears about it: a rejected read
        // must simply be ignored.
        const std::string_view type = metadataValue(metadata, "type");
        if (type == "write") {
            reset();
            return fail("EFBIG", reply);
        }
        if ((type == "wdata" || type == "walias") && m_writing && !m_failed) {
            return fail("EFBIG", reply);
        }
        return Result::Ignored;
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
            auto decode = [&](std::string& target, bool typeName) {
                if (value.size() > 4096) {
                    badValue = true;
                    return;
                }
                auto d = base64Decode(value, Base64Check::Strict);
                if (!d || (typeName ? !isTypeName(*d) : !isCleanText(*d))) {
                    badValue = true;
                    return;
                }
                target = std::move(*d);
            };
            if (key == "type") {
                type = value;
            } else if (key == "mime") {
                decode(mime, true);
            } else if (key == "pw") {
                decode(password, false);
            } else if (key == "name") {
                decode(name, false);
            } else if (key == "loc") {
                primary = value == "primary";
            } else if (key == "id") {
                id = sanitizeId(value);
            }
        }
        if (colon == std::string_view::npos) {
            break;
        }
        pos = colon + 1;
    }
    // A password without a name counts as no password (the spec).
    if (name.empty()) {
        password.clear();
    }

    if (type == "read") {
        // A read has no way to report bad base64: it is ignored.
        if (data.size() > 65536) {
            return Result::Ignored;
        }
        const auto list = base64Decode(data, Base64Check::Strict);
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
            if (!t.empty()) {
                if (!isTypeName(t)) {
                    return Result::Ignored;
                }
                if (out.mimes.size() < MaxParts) {
                    out.mimes.emplace_back(t);
                }
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
        if (badValue) {
            return fail("EINVAL", reply);
        }
        if (type == "walias") {
            if (data.size() > 65536) {
                return fail("EFBIG", reply);
            }
            const auto aliases = base64Decode(data, Base64Check::Strict);
            if (mime.empty() || !aliases) {
                return fail("EINVAL", reply);
            }
            std::string_view list = *aliases;
            std::size_t apos = 0;
            while (apos < list.size()) {
                const auto space = list.find(' ', apos);
                const std::string_view alias = list.substr(apos, space == std::string_view::npos ? space : space - apos);
                if (!alias.empty()) {
                    if (!isTypeName(alias)) {
                        return fail("EINVAL", reply);
                    }
                    if (m_write.aliases.size() < MaxParts) {
                        m_write.aliases.emplace_back(alias, mime);
                    }
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
                return Result::Error;
            }
            out = std::move(m_write);
            reset();
            return Result::Ready;
        }
        if (mime.empty()) {
            return fail("EINVAL", reply);
        }
        if (mime != m_mime) {
            if (!flush(reply)) {
                return Result::Error;
            }
            // All the chunks of a type come in one run (the spec). A type sent
            // again would replace its earlier data.
            if (std::any_of(m_write.data.begin(), m_write.data.end(), [&](const auto& part) { return part.first == mime; })) {
                return fail("EINVAL", reply);
            }
            m_mime = mime;
        }
        if (data.empty()) {
            return Result::Pending;
        }
        // Decoded as it arrives, so the encoded stream is never held whole.
        // Only the concatenation of a type's chunks must be padded, so a
        // chunk may end inside a group, and padding ends the stream.
        if (m_padded) {
            return fail("EINVAL", reply);
        }
        std::string joined = m_carry;
        joined += data;
        // Whole groups only: a stream may be cut between its two padding
        // characters. Malformed padding stays in the carry, and flush()
        // refuses it.
        const std::size_t whole = joined.size() / 4 * 4;
        std::size_t padding = 0;
        while (padding < 2 && padding < whole && joined[whole - 1 - padding] == '=') {
            ++padding;
        }
        // The size is known before the decode, so one large chunk cannot
        // grow the data past the limit first.
        if (m_part.size() + (whole / 4 * 3 - padding) > MaxWrite - m_decoded) {
            return fail("EFBIG", reply);
        }
        if (!base64DecodeAppend(m_part, std::string_view(joined).substr(0, whole), Base64Check::Strict)) {
            return fail("EINVAL", reply);
        }
        m_padded = padding != 0;
        m_carry = joined.substr(whole);
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
        r += ":pw=" + base64Encode(oneTimePassword);
    }
    r += "\x1b\\";
    return r;
}

std::string ClipboardAssembler::readData(std::string_view mime, std::string_view bytes, const ClipboardRequest& request,
                                         std::string_view oneTimePassword)
{
    std::string out;
    const std::string prefix = header("read", "DATA", request.id) + ":mime=" + base64Encode(mime)
        + (oneTimePassword.empty() ? std::string{} : ":pw=" + base64Encode(oneTimePassword)) + ';';
    std::size_t pos = 0;
    do {
        const std::string_view chunk = bytes.substr(pos, ReadChunk);
        out += prefix;
        out += base64Encode(chunk);
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
    r += base64Encode(text);
    r += osc52Bel ? "\x07" : "\x1b\\";
    return r;
}

namespace {

/// The metadata that a read and the start of a write share.
std::string requestHeader(std::string_view type, const ClipboardRequest& request)
{
    std::string r = "\x1b]5522;type=";
    r += type;
    if (request.primary) {
        r += ":loc=primary";
    }
    if (!request.id.empty()) {
        r += ":id=" + request.id;
    }
    // The terminal ignores a password that comes without a name.
    if (!request.password.empty() && !request.name.empty()) {
        r += ":pw=" + base64Encode(request.password) + ":name=" + base64Encode(request.name);
    }
    return r;
}

std::string joined(const std::vector<std::string>& parts)
{
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) {
            out += ' ';
        }
        out += part;
    }
    return out;
}

} // namespace

std::string clipboardReadRequest(const ClipboardRequest& request)
{
    return requestHeader("read", request) + ';' + base64Encode(joined(request.mimes)) + "\x1b\\";
}

std::string clipboardWriteRequest(const ClipboardRequest& request)
{
    // Whole groups of three bytes in each chunk but the last, so that only
    // the end of a type's stream is padded.
    constexpr std::size_t Chunk = ClipboardAssembler::ReadChunk / 4 * 3;
    std::string out = requestHeader("write", request) + "\x1b\\";
    for (const auto& [mime, bytes] : request.data) {
        const std::string prefix = "\x1b]5522;type=wdata:mime=" + base64Encode(mime) + ';';
        std::size_t pos = 0;
        do {
            out += prefix;
            out += base64Encode(std::string_view(bytes).substr(pos, Chunk));
            out += "\x1b\\";
            pos += Chunk;
        } while (pos < bytes.size());
    }
    // Aliases are sent for each type they share, in the order given.
    std::vector<std::pair<std::string, std::vector<std::string>>> aliases;
    for (const auto& [alias, mime] : request.aliases) {
        auto it = std::find_if(aliases.begin(), aliases.end(), [&](const auto& entry) { return entry.first == mime; });
        if (it == aliases.end()) {
            aliases.emplace_back(mime, std::vector<std::string>{});
            it = std::prev(aliases.end());
        }
        it->second.push_back(alias);
    }
    for (const auto& [mime, names] : aliases) {
        out += "\x1b]5522;type=walias:mime=" + base64Encode(mime) + ';' + base64Encode(joined(names)) + "\x1b\\";
    }
    out += "\x1b]5522;type=wdata\x1b\\";
    return out;
}

std::optional<ClipboardReply> ClipboardReply::parse(std::string_view payload)
{
    const auto semi = payload.find(';');
    const std::string_view metadata = payload.substr(0, semi);
    const std::string_view data = semi == std::string_view::npos ? std::string_view{} : payload.substr(semi + 1);
    ClipboardReply reply;
    bool valid = true;
    std::size_t pos = 0;
    while (pos <= metadata.size()) {
        const auto colon = metadata.find(':', pos);
        const std::string_view kv = metadata.substr(pos, colon == std::string_view::npos ? colon : colon - pos);
        const auto eq = kv.find('=');
        if (eq != std::string_view::npos) {
            const std::string_view key = kv.substr(0, eq);
            const std::string_view value = kv.substr(eq + 1);
            auto decode = [&](std::string& target) {
                auto decoded = base64Decode(value, Base64Check::Strict);
                valid = valid && decoded;
                target = decoded.value_or(std::string{});
            };
            if (key == "type") {
                reply.type = value;
            } else if (key == "status") {
                reply.status = value;
            } else if (key == "id") {
                reply.id = value;
            } else if (key == "mime") {
                decode(reply.mime);
            } else if (key == "pw") {
                decode(reply.password);
            } else if (key == "loc") {
                reply.primary = value == "primary";
            }
        }
        if (colon == std::string_view::npos) {
            break;
        }
        pos = colon + 1;
    }
    auto decoded = base64Decode(data, Base64Check::Strict);
    if (!valid || !decoded) {
        return std::nullopt;
    }
    reply.data = std::move(*decoded);
    return reply;
}

std::optional<Osc52Message> Osc52Message::parse(std::string_view payload)
{
    const auto semi = payload.find(';');
    if (semi == std::string_view::npos) {
        return std::nullopt;
    }
    Osc52Message message;
    message.selection = payload.substr(0, semi);
    const std::string_view data = payload.substr(semi + 1);
    message.query = data == "?";
    if (!message.query) {
        message.text = base64Decode(data);
    }
    return message;
}

std::string Osc52Message::write() const
{
    return "\x1b]52;" + selection + ';' + (query ? std::string("?") : base64Encode(text)) + "\x1b\\";
}

} // namespace AfterglowProto
