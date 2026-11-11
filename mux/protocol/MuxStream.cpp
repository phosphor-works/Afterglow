// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxStream.h"

#include <algorithm>

namespace AfterglowMux::Attach {

namespace {

void varint(std::string& out, std::uint64_t v)
{
    while (v >= 0x80) {
        out.push_back(char((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(char(v));
}

class Reader
{
public:
    explicit Reader(std::string_view s)
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
    /// A varint in its shortest form, up to @p max.
    std::uint64_t varint(std::uint64_t max)
    {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64 && m_ok; shift += 7) {
            const std::uint8_t b = byte();
            const std::uint64_t part = b & 0x7f;
            // The tenth byte holds the 64th bit only. A last byte of 0 after
            // the first is a longer form of a shorter number.
            if ((shift == 63 && part > 1) || (!(b & 0x80) && shift > 0 && part == 0)) break;
            v |= part << shift;
            if (!(b & 0x80)) {
                if (v > max) m_ok = false;
                return m_ok ? v : 0;
            }
        }
        m_ok = false;
        return 0;
    }
    std::string_view rest()
    {
        const std::string_view r = m_s.substr(m_at);
        m_at = m_s.size();
        return r;
    }

private:
    std::string_view m_s;
    std::size_t m_at = 0;
    bool m_ok = true;
};

} // namespace

std::string encode(const Event& event)
{
    std::string out(1, char(event.type));
    varint(out, event.pane);
    varint(out, event.channel);
    // A wait status is never negative: 0 carries -1, the status that is
    // unknown, and the others go up by one.
    varint(out, std::uint64_t(event.status < 0 ? 0 : std::int64_t(event.status) + 1));
    if (event.type == EventType::Host) out += event.payload;
    return out;
}

std::optional<Event> decodeEvent(std::string_view frame)
{
    Reader r(frame);
    Event event;
    const std::uint8_t type = r.byte();
    event.pane = std::uint32_t(r.varint(0xffffffffu));
    event.channel = std::uint32_t(r.varint(MaxChannelId - 1));
    const std::uint64_t status = r.varint(std::uint64_t(0x7fffffff) + 1);
    // A host call takes the rest of the frame.
    if (type == std::uint8_t(EventType::Host) && r.ok()) event.payload = std::string(r.rest());
    if (!r.ok() || !r.done() || type < std::uint8_t(EventType::PaneOpened) || type > std::uint8_t(EventType::Host)) {
        return std::nullopt;
    }
    event.type = EventType(type);
    event.status = status == 0 ? -1 : int(status - 1);
    // Pane ids start at 1, and a pane's state never takes the channel of
    // the events.
    if (event.type == EventType::PaneOpened && (event.pane == 0 || event.channel == ControlChannel)) return std::nullopt;
    return event;
}

std::string encode(const Input& input)
{
    std::string out(1, char(input.type));
    varint(out, input.pane);
    if (input.type == InputType::Bytes) {
        out += input.bytes;
    } else if (input.type == InputType::Resize) {
        for (const int v : {input.size.cols, input.size.rows, input.size.cellWidth, input.size.cellHeight}) {
            varint(out, std::uint64_t(v < 0 ? 0 : v));
        }
    }
    return out;
}

std::optional<Input> decodeInput(std::string_view frame)
{
    Reader r(frame);
    Input input;
    const std::uint8_t type = r.byte();
    input.pane = std::uint32_t(r.varint(0xffffffffu));
    if (!r.ok()) return std::nullopt;
    if (type == std::uint8_t(InputType::Bytes)) {
        input.type = InputType::Bytes;
        input.bytes = std::string(r.rest());
        return input;
    }
    if (type == std::uint8_t(InputType::Reset) || type == std::uint8_t(InputType::Clear) || type == std::uint8_t(InputType::Kill)) {
        if (!r.done()) return std::nullopt;
        input.type = InputType(type);
        return input;
    }
    if (type != std::uint8_t(InputType::Resize)) return std::nullopt;
    input.type = InputType::Resize;
    // Any size the window measured: Terminal::resize clamps it to what a
    // terminal can be, and a cell's pixels to what a winsize holds.
    const auto value = [&r] { return int(std::min<std::uint64_t>(r.varint(0xffffffffu), 0x7fffffff)); };
    input.size.cols = value();
    input.size.rows = value();
    input.size.cellWidth = std::min(value(), 65535);
    input.size.cellHeight = std::min(value(), 65535);
    if (!r.ok() || !r.done()) return std::nullopt;
    return input;
}

} // namespace AfterglowMux::Attach
