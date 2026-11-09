// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/DragDrop.h>
#include <AfterglowProto/MachineId.h>
#include <AfterglowProto/Sha256.h>

#include "Fields.h"

#include <algorithm>
#include <limits>

namespace AfterglowProto {

namespace {

bool space(char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

std::string_view trimmed(std::string_view s)
{
    while (!s.empty() && space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

bool inRange(std::int64_t n)
{
    return n >= INT32_MIN && n <= std::int64_t(UINT32_MAX);
}

/// A packet with more chunks after it. kitty reads m as a number and goes on while it is not 0.
bool hasMore(const DndMeta& packet)
{
    const auto more = packet.find("m");
    return more != packet.end() && parseDndInteger(more->second).value_or(0) != 0;
}

std::string valueOf(const DndMeta& meta, const char* key)
{
    const auto it = meta.find(key);
    return it == meta.end() ? std::string() : it->second;
}

} // namespace

std::optional<std::int64_t> parseDndInteger(std::string_view text)
{
    text = trimmed(text);
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    // Accumulated negative, so that INT64_MIN fits.
    std::int64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        if (value < (std::numeric_limits<std::int64_t>::min() + (c - '0')) / 10) {
            return std::nullopt;
        }
        value = value * 10 - (c - '0');
    }
    if (!negative) {
        if (value == std::numeric_limits<std::int64_t>::min()) {
            return std::nullopt;
        }
        value = -value;
    }
    return value;
}

std::pair<std::string_view, std::string_view> splitDnd(std::string_view command)
{
    const auto separator = command.find(';');
    if (separator == std::string_view::npos) {
        return {command, {}};
    }
    return {command.substr(0, separator), command.substr(separator + 1)};
}

std::optional<DndMeta> parseDndMeta(std::string_view raw)
{
    DndMeta meta;
    bool valid = true;
    detail::eachField(raw, ':', [&](std::string_view field) {
        if (!valid || field.size() < 2 || field[1] != '=') {
            return;
        }
        const std::string key(1, field[0]);
        const auto value = field.substr(2);
        if (std::string_view("tixyXYom").find(field[0]) == std::string_view::npos) {
            return;
        }
        if (meta.contains(key)) {
            valid = false;
            return;
        }
        if (key == "t") {
            valid = value.size() == 1;
            if (valid) {
                meta.emplace(key, value);
            }
            return;
        }
        // Plain decimal, so that values compare as text: m=01 is m=1, i=+5 is i=5.
        const auto n = parseDndInteger(value);
        valid = n && inRange(*n);
        if (valid) {
            meta.emplace(key, std::to_string(*n));
        }
    });
    if (!valid) {
        return std::nullopt;
    }
    return meta;
}

std::string dndType(std::string_view raw)
{
    std::string type;
    bool found = false;
    detail::eachField(raw, ':', [&](std::string_view field) {
        if (!found && field.starts_with("t=")) {
            type = field.substr(2);
            found = true;
        }
    });
    return type;
}

bool dndMore(std::string_view raw)
{
    std::optional<std::int64_t> more;
    bool found = false;
    detail::eachField(raw, ':', [&](std::string_view field) {
        if (!found && field.starts_with("m=")) {
            more = parseDndInteger(field.substr(2));
            found = true;
        }
    });
    // Like hasMore: a value that is not a number reads as 0.
    return more.value_or(0) != 0;
}

bool dndReceiverType(std::string_view type)
{
    return type == "a" || type == "A" || type == "m" || type == "r";
}

bool dndDataType(std::string_view type)
{
    return type == "p" || type == "e" || type == "k";
}

std::vector<std::string> writeDnd(DndMeta meta, std::string_view payload)
{
    std::vector<std::string> packets;
    std::size_t offset = 0;
    do {
        const auto part = payload.substr(offset, DndMaxPacket);
        offset += part.size();
        meta["m"] = offset < payload.size() ? "1" : "0";
        std::string bytes = "\x1b]72;";
        bool first = true;
        for (const auto& [key, value] : meta) {
            if (!first) {
                bytes += ':';
            }
            first = false;
            bytes += key;
            bytes += '=';
            bytes += value;
        }
        bytes += ';';
        bytes += part;
        bytes += "\x1b\\";
        packets.push_back(std::move(bytes));
    } while (offset < payload.size());
    return packets;
}

std::string dndQueryReply(std::string_view command)
{
    const auto [raw, payload] = splitDnd(command);
    if (command.size() > DndMaxCommand || payload.size() > DndMaxPacket) {
        return {};
    }
    const auto meta = parseDndMeta(raw);
    if (!meta) {
        return {};
    }
    const auto type = valueOf(*meta, "t");
    if (type != "q" && type != "Q") {
        return {};
    }
    std::string out = "\x1b]72;t=" + type;
    if (const auto id = meta->find("i"); id != meta->end()) {
        out += ":i=";
        out += id->second;
    }
    out += ";\x1b\\";
    return out;
}

bool dndValidMime(std::string_view mime)
{
    return !mime.empty() && mime.find('/') != std::string_view::npos && mime.size() <= 255
        && std::all_of(mime.begin(), mime.end(), [](unsigned char c) { return c > 32 && c < 127; });
}

std::string dndMachineId(std::string_view machineIdContents)
{
    // A directory or an empty file reads as nothing. Its hash would be the same on every such computer.
    const auto id = trimmed(machineIdContents);
    if (id.empty()) {
        return {};
    }
    return "1:" + toHex(hmacSha256("tty-dnd-protocol-machine-id", id));
}

std::string dndMachineId()
{
    return dndMachineId(machineIdContents());
}

DndAssembler::DndAssembler(std::size_t maxData, std::size_t maxControl)
    : m_maxData(maxData)
    , m_maxControl(maxControl)
{
}

std::size_t DndAssembler::limit(std::string_view type) const
{
    return dndDataType(type) ? m_maxData : m_maxControl;
}

std::string DndAssembler::type() const
{
    const auto it = m_meta.find("t");
    return it == m_meta.end() ? std::string("a") : it->second;
}

void DndAssembler::reset()
{
    m_meta.clear();
    // Frees the buffer: a dropped data message can hold tens of MiB.
    std::string().swap(m_payload);
    m_discarding = false;
}

void DndAssembler::discard(bool more)
{
    reset();
    m_discarding = more;
}

void DndAssembler::abandon()
{
    discard(active() || m_discarding);
}

DndAssembler::Result DndAssembler::feed(const DndMeta& packet, std::string_view data, Message& message)
{
    const bool more = hasMore(packet);
    if (m_discarding) {
        m_discarding = more;
        return Result::Dropped;
    }
    if (!active()) {
        const auto type = valueOf(packet, "t");
        if (data.size() > limit(type.empty() ? "a" : type)) {
            discard(more);
            return Result::TooLarge;
        }
        if (!more) {
            message.meta = packet;
            message.payload = data;
            message.emptyEnd = data.empty();
            return Result::Ready;
        }
        m_meta = packet;
        m_payload = data;
        return Result::Pending;
    }
    // Chunks after the first need not repeat the type or the id, but must not
    // name others. kitty takes any packet as the next chunk; a request read
    // as a chunk of a MIME list is a client error that must not pass unseen.
    const auto chunkType = type();
    const auto t = packet.find("t");
    const auto id = packet.find("i");
    const auto firstId = m_meta.find("i");
    if ((t != packet.end() && t->second != chunkType)
        || (id != packet.end() && (firstId == m_meta.end() || firstId->second != id->second))) {
        discard(more);
        return Result::Mismatch;
    }
    if (m_payload.size() + data.size() > limit(chunkType)) {
        discard(more);
        return Result::TooLarge;
    }
    m_payload += data;
    if (more) {
        return Result::Pending;
    }
    message.meta = std::move(m_meta);
    if (const auto last = packet.find("m"); last != packet.end()) {
        message.meta["m"] = last->second;
    } else {
        message.meta.erase("m");
    }
    message.payload = std::move(m_payload);
    message.emptyEnd = data.empty();
    reset();
    return Result::Ready;
}

} // namespace AfterglowProto
