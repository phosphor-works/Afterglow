// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxStream.h"

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
    std::uint64_t varint(std::uint64_t max)
    {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64 && m_ok; shift += 7) {
            const std::uint8_t b = byte();
            v |= std::uint64_t(b & 0x7f) << shift;
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
    // A status is never negative (a wait status), and 0 for the events
    // that have none.
    varint(out, std::uint64_t(event.status < 0 ? 0 : event.status));
    return out;
}

std::optional<Event> decodeEvent(std::string_view frame)
{
    Reader r(frame);
    Event event;
    const std::uint8_t type = r.byte();
    event.pane = std::uint32_t(r.varint(0xffffffffu));
    event.status = int(r.varint(0x7fffffff));
    if (!r.ok() || !r.done() || type < std::uint8_t(EventType::PaneOpened) || type > std::uint8_t(EventType::Detached)) {
        return std::nullopt;
    }
    event.type = EventType(type);
    return event;
}

std::string encode(const Input& input)
{
    std::string out(1, char(input.type));
    varint(out, input.pane);
    if (input.type == InputType::Bytes) {
        out += input.bytes;
    } else {
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
    if (type != std::uint8_t(InputType::Resize)) return std::nullopt;
    input.type = InputType::Resize;
    // The bounds of a terminal (Terminal::resize) and of a cell in pixels.
    input.size.cols = int(r.varint(4096));
    input.size.rows = int(r.varint(1048576));
    input.size.cellWidth = int(r.varint(65535));
    input.size.cellHeight = int(r.varint(65535));
    if (!r.ok() || !r.done() || input.size.cols < 2 || input.size.rows < 1) return std::nullopt;
    return input;
}

} // namespace AfterglowMux::Attach
