// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/DndTree.h>
#include <AfterglowProto/Text.h>

#include <algorithm>
#include <set>

namespace AfterglowProto {

DndMeta dndRemoteEntry(std::int64_t root, std::int64_t parent, std::int64_t child, std::int64_t kind)
{
    DndMeta meta { { "t", "k" }, { "x", std::to_string(root) }, { "X", std::to_string(kind) } };
    if (parent != 0) {
        meta["Y"] = std::to_string(parent);
        meta["y"] = std::to_string(child);
    }
    return meta;
}

DndMeta dndReadEntry(std::int64_t mime, std::int64_t parent, std::int64_t child)
{
    if (parent != 0) {
        DndMeta meta { { "t", "r" }, { "Y", std::to_string(parent) } };
        if (child != 0) {
            meta["x"] = std::to_string(child);
        }
        return meta;
    }
    return { { "t", "r" }, { "x", std::to_string(mime) }, { "y", std::to_string(child) } };
}

std::vector<std::string> dndDataPackets(const DndMeta& meta, std::string_view bytes)
{
    std::vector<std::string> packets;
    for (std::size_t offset = 0; offset < bytes.size(); offset += DndDataChunk) {
        for (auto& packet : writeDnd(meta, base64Encode(bytes.substr(offset, DndDataChunk)))) {
            packets.push_back(std::move(packet));
        }
    }
    for (auto& packet : writeDnd(meta, {})) {
        packets.push_back(std::move(packet));
    }
    return packets;
}

std::string dndJoinNames(const std::vector<std::string>& names)
{
    std::string out;
    for (const auto& name : names) {
        if (!out.empty()) {
            out += '\0';
        }
        out += name;
    }
    return out;
}

bool dndValidName(std::string_view name)
{
    if (name.empty() || name.size() > 255 || name == "." || name == ".."
        || name.find_first_of(std::string_view("/\0", 2)) != std::string_view::npos) {
        return false;
    }
    std::size_t i = 0;
    char32_t cp = 0;
    while (i < name.size() && nextUtf8(name, i, cp)) {
    }
    return i == name.size();
}

std::optional<std::vector<std::string>> dndSplitNames(std::string_view listing)
{
    std::vector<std::string> names;
    if (listing.empty()) {
        return names;
    }
    std::set<std::string_view> seen;
    std::size_t start = 0;
    while (start <= listing.size()) {
        const auto end = std::min(listing.find('\0', start), listing.size());
        const auto name = listing.substr(start, end - start);
        start = end + 1;
        if (!dndValidName(name) || !seen.insert(name).second) {
            return std::nullopt;
        }
        names.emplace_back(name);
    }
    return names;
}

bool DndBase64Stream::feed(std::string_view text, std::string& out)
{
    m_carry += text;
    // Whole groups of four only: the rest waits for the next packet. A
    // group with padding ends where it is, as each Afterglow packet does.
    const std::size_t whole = m_carry.size() / 4 * 4;
    if (!base64DecodeAppend(out, std::string_view(m_carry).substr(0, whole), Base64Check::Strict)) {
        return false;
    }
    m_carry.erase(0, whole);
    return true;
}

bool DndBase64Stream::finish(std::string& out)
{
    // Padding is optional at the end. One character left is six bits, which
    // no encoder sends: the item was cut, and it fails rather than lose a byte.
    const bool ok = m_carry.empty() || base64DecodeAppend(out, m_carry, Base64Check::PaddingOptional);
    m_carry.clear();
    return ok;
}

} // namespace AfterglowProto
