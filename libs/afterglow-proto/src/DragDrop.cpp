// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/DragDrop.h>
#include <AfterglowProto/Sha256.h>

#include <algorithm>
#include <climits>
#include <fstream>
#include <iterator>
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

/// Calls @p f with each ':'-separated field of @p meta.
template<typename F>
void eachField(std::string_view meta, F f)
{
    std::size_t pos = 0;
    while (true) {
        const auto colon = meta.find(':', pos);
        f(meta.substr(pos, colon == std::string_view::npos ? colon : colon - pos));
        if (colon == std::string_view::npos) {
            return;
        }
        pos = colon + 1;
    }
}

bool inRange(std::int64_t n)
{
    return n >= INT32_MIN && n <= std::int64_t(UINT32_MAX);
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
    eachField(raw, [&](std::string_view field) {
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
        } else {
            const auto n = parseDndInteger(value);
            valid = n && inRange(*n);
        }
        if (valid) {
            meta.emplace(key, value);
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
    eachField(raw, [&](std::string_view field) {
        if (!found && field.starts_with("t=")) {
            type = field.substr(2);
            found = true;
        }
    });
    return type;
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
    std::string_view type;
    std::string_view id;
    bool valid = true;
    eachField(splitDnd(command).first, [&](std::string_view field) {
        if (field.starts_with("t=")) {
            type = field.substr(2);
        }
        if (field.starts_with("i=")) {
            const auto n = parseDndInteger(field.substr(2));
            valid = valid && n && inRange(*n);
            id = field.substr(2);
        }
    });
    if (!valid || (type != "q" && type != "Q")) {
        return {};
    }
    std::string out = "\x1b]72;t=";
    out += type;
    if (!id.empty()) {
        out += ":i=";
        out += id;
    }
    out += ";\x1b\\";
    return out;
}

bool dndValidMime(std::string_view mime)
{
    return !mime.empty() && mime.find('/') != std::string_view::npos && mime.size() <= 255
        && std::all_of(mime.begin(), mime.end(), [](unsigned char c) { return c > 32 && c < 127; });
}

std::string dndMachineId(std::string_view machineIdFile)
{
    return "1:" + toHex(hmacSha256("tty-dnd-protocol-machine-id", trimmed(machineIdFile)));
}

std::string dndMachineId()
{
    std::ifstream file("/etc/machine-id", std::ios::binary);
    if (!file) {
        return {};
    }
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    return dndMachineId(contents);
}

DndAssembler::DndAssembler(std::size_t maxData, std::size_t maxControl)
    : m_maxData(maxData)
    , m_maxControl(maxControl)
{
}

std::string DndAssembler::type() const
{
    const auto it = m_meta.find("t");
    return it == m_meta.end() ? std::string("a") : it->second;
}

void DndAssembler::reset()
{
    m_meta.clear();
    m_payload.clear();
}

DndAssembler::Result DndAssembler::feed(const DndMeta& message, std::string_view data, DndMeta& meta, std::string& payload)
{
    const auto more = message.find("m");
    const bool last = more == message.end() || more->second != "1";
    if (!active()) {
        if (last) {
            meta = message;
            payload = data;
            return Result::Ready;
        }
        m_meta = message;
        m_payload = data;
        return Result::Pending;
    }
    // Chunks after the first need not repeat the id, but must not name another.
    if (const auto id = message.find("i"); id != message.end()) {
        const auto first = m_meta.find("i");
        if (first == m_meta.end() || first->second != id->second) {
            return Result::Mismatch;
        }
    }
    const auto chunkType = type();
    const bool dataType = chunkType == "p" || chunkType == "e" || chunkType == "k";
    if (m_payload.size() + data.size() > (dataType ? m_maxData : m_maxControl)) {
        return Result::TooLarge;
    }
    m_payload += data;
    if (!last) {
        return Result::Pending;
    }
    meta = std::move(m_meta);
    payload = std::move(m_payload);
    reset();
    if (data.empty()) {
        meta["_end"] = "1";
    }
    return Result::Ready;
}

} // namespace AfterglowProto
