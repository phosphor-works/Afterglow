// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxStream.h"

#include "MuxCodec.h"

#include <algorithm>

namespace AfterglowMux::Attach {

using Codec::varint;
using Reader = Codec::Reader;

std::string encode(const Event& event)
{
    std::string out(1, char(event.type));
    varint(out, event.pane);
    varint(out, event.channel);
    // A wait status is never negative: 0 carries -1, the status that is
    // unknown, and the others go up by one.
    varint(out, std::uint64_t(event.status < 0 ? 0 : std::int64_t(event.status) + 1));
    if (event.type == EventType::Host) {
        varint(out, event.frames);
        out += event.payload;
    } else if (event.type == EventType::Layout || event.type == EventType::Control || event.type == EventType::Commands) {
        out += event.payload;
    }
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
    if (type == std::uint8_t(EventType::Host) && r.ok()) {
        event.frames = r.varint(~std::uint64_t(0));
        if (r.ok()) event.payload = std::string(r.rest());
    } else if ((type == std::uint8_t(EventType::Layout) || type == std::uint8_t(EventType::Control)
                || type == std::uint8_t(EventType::Commands))
               && r.ok()) {
        event.payload = std::string(r.rest());
    }
    if (!r.ok() || !r.done() || type < std::uint8_t(EventType::PaneOpened) || type > std::uint8_t(EventType::Commands)) {
        return std::nullopt;
    }
    event.type = EventType(type);
    event.status = status == 0 ? -1 : int(status - 1);
    // Pane ids start at 1, and a pane's state never takes the channel of
    // the events.
    if (event.type == EventType::PaneOpened && (event.pane == 0 || event.channel == ControlChannel)) return std::nullopt;
    // A host call names its pane, and comes whole or in parts.
    if (event.type == EventType::Host && (event.pane == 0 || event.channel != 0 || (event.status != -1 && event.status != HostPart))) {
        return std::nullopt;
    }
    return event;
}

std::string encode(const Input& input)
{
    std::string out(1, char(input.type));
    varint(out, input.pane);
    if (input.type == InputType::Bytes || input.type == InputType::Layout || input.type == InputType::ControlReply) {
        out += input.bytes;
    } else if (input.type == InputType::Resize) {
        for (const int v : {input.size.cols, input.size.rows, input.size.cellWidth, input.size.cellHeight}) {
            varint(out, std::uint64_t(v < 0 ? 0 : v));
        }
    } else if (input.type == InputType::Visible) {
        out.push_back(input.visible ? 1 : 0);
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
    if (type == std::uint8_t(InputType::Bytes) || type == std::uint8_t(InputType::Layout) || type == std::uint8_t(InputType::ControlReply)) {
        input.type = InputType(type);
        input.bytes = std::string(r.rest());
        return input;
    }
    if (type == std::uint8_t(InputType::Reset) || type == std::uint8_t(InputType::Clear) || type == std::uint8_t(InputType::Kill)
        || type == std::uint8_t(InputType::ClosePane)) {
        if (!r.done()) return std::nullopt;
        input.type = InputType(type);
        return input;
    }
    if (type == std::uint8_t(InputType::Visible)) {
        const std::uint8_t visible = r.byte();
        if (!r.ok() || !r.done() || visible > 1) return std::nullopt;
        input.type = InputType::Visible;
        input.visible = visible;
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

std::string encode(const RelayFrame& frame)
{
    std::string out(1, char(frame.part));
    varint(out, frame.relay);
    if (frame.part == RelayPart::Bytes) {
        out += frame.bytes;
    } else if (frame.part == RelayPart::Open) {
        varint(out, frame.pane);
        out.push_back(char(frame.kind));
        if (frame.kind == RelayKind::Remote) out += frame.host;
    }
    return out;
}

std::optional<RelayFrame> decodeRelay(std::string_view bytes)
{
    Reader r(bytes);
    RelayFrame frame;
    const std::uint8_t part = r.byte();
    frame.relay = r.varint(~std::uint64_t(0));
    if (!r.ok() || frame.relay == 0) return std::nullopt;
    if (part == std::uint8_t(RelayPart::Bytes)) {
        frame.bytes = std::string(r.rest());
        if (frame.bytes.empty()) return std::nullopt;
    } else if (part == std::uint8_t(RelayPart::Open)) {
        frame.pane = std::uint32_t(r.varint(0xffffffffu));
        const std::uint8_t kind = r.byte();
        if (!r.ok() || frame.pane == 0 || kind > std::uint8_t(RelayKind::Remote)) return std::nullopt;
        frame.kind = RelayKind(kind);
        if (frame.kind == RelayKind::Remote) {
            frame.host = std::string(r.rest());
            if (frame.host.empty()) return std::nullopt;
        } else if (!r.done()) {
            return std::nullopt;
        }
    } else if (part != std::uint8_t(RelayPart::End) || !r.done()) {
        return std::nullopt;
    }
    frame.part = RelayPart(part);
    return frame;
}

} // namespace AfterglowMux::Attach
