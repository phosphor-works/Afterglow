// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Graphics.h>
#include <AfterglowProto/Zlib.h>

#include <algorithm>
#include <charconv>
#include <type_traits>

namespace AfterglowProto {

GraphicsCommand GraphicsCommand::parse(std::string_view keys)
{
    GraphicsCommand cmd;
    std::size_t pos = 0;
    while (pos < keys.size()) {
        const auto comma = keys.find(',', pos);
        const std::string_view kv = keys.substr(pos, comma == std::string_view::npos ? comma : comma - pos);
        pos = comma == std::string_view::npos ? keys.size() : comma + 1;
        if (kv.size() < 3 || kv[1] != '=') {
            continue;
        }
        const char key = kv[0];
        const std::string_view value = kv.substr(2);
        if (key == 'a' || key == 't' || key == 'd' || key == 'o') {
            const char ch = value.empty() ? 0 : value[0];
            (key == 'a' ? cmd.action : key == 't' ? cmd.medium : key == 'd' ? cmd.del : cmd.compression) = ch;
            cmd.hasAction = cmd.hasAction || key == 'a';
            continue;
        }
        std::int64_t n = 0;
        if (std::from_chars(value.data(), value.data() + value.size(), n).ec != std::errc{}) {
            continue;
        }
        const auto u = std::uint32_t(std::clamp<std::int64_t>(n, 0, 0xFFFFFFFF));
        const auto i = int(std::clamp<std::int64_t>(n, -0x7FFFFFFF, 0x7FFFFFFF));
        switch (key) {
        case 'f': cmd.format = u; break;
        case 's': cmd.s = u; break;
        case 'v': cmd.v = u; break;
        case 'S': cmd.size = u; break;
        case 'O': cmd.offset = u; break;
        case 'i': cmd.id = u; cmd.hasId = true; break;
        case 'I': cmd.number = u; cmd.hasNumber = true; break;
        case 'p': cmd.placement = u; break;
        case 'm': cmd.more = i; break;
        case 'q': cmd.quiet = i; break;
        case 'x': cmd.x = i; cmd.ux = u; break;
        case 'y': cmd.y = i; cmd.uy = u; break;
        case 'w': cmd.w = i; break;
        case 'h': cmd.h = i; break;
        case 'X': cmd.X = i; break;
        case 'Y': cmd.Y = n; cmd.Yi = i; break;
        case 'c': cmd.c = i; break;
        case 'r': cmd.r = i; break;
        case 'C': cmd.C = i; break;
        case 'z': cmd.z = i; break;
        case 'U': cmd.U = i; break;
        case 'P': cmd.P = u; break;
        case 'Q': cmd.Q = u; break;
        case 'H': cmd.H = i; break;
        case 'V': cmd.V = i; break;
        default: break;
        }
    }
    return cmd;
}

std::string GraphicsCommand::keys() const
{
    std::string out;
    auto add = [&out](char key, auto value) {
        if (!out.empty()) {
            out += ',';
        }
        out += key;
        out += '=';
        if constexpr (std::is_same_v<decltype(value), char>) {
            out += value;
        } else {
            out += std::to_string(value);
        }
    };
    if (hasAction || action != 't') add('a', action);
    if (medium != 'd') add('t', medium);
    if (del != 'a') add('d', del);
    if (compression) add('o', compression);
    if (format != 32) add('f', format);
    if (s) add('s', s);
    if (v) add('v', v);
    if (size) add('S', size);
    if (offset) add('O', offset);
    if (hasId || id) add('i', id);
    if (hasNumber || number) add('I', number);
    if (placement) add('p', placement);
    if (more) add('m', more);
    if (quiet) add('q', quiet);
    // ux and uy hold what x and y cannot: values past INT_MAX.
    if (ux > 0x7FFFFFFFu || (x == 0 && ux)) add('x', ux);
    else if (x) add('x', x);
    if (uy > 0x7FFFFFFFu || (y == 0 && uy)) add('y', uy);
    else if (y) add('y', y);
    if (w) add('w', w);
    if (h) add('h', h);
    if (X) add('X', X);
    if (Y) add('Y', Y);
    if (c) add('c', c);
    if (r) add('r', r);
    if (C) add('C', C);
    if (z) add('z', z);
    if (U) add('U', U);
    if (P) add('P', P);
    if (Q) add('Q', Q);
    if (H) add('H', H);
    if (V) add('V', V);
    return out;
}

std::string writeGraphics(const GraphicsCommand& command, std::string_view payload)
{
    const std::string encoded = base64Encode(payload);
    std::string out = "\x1b_G";
    if (encoded.size() <= GraphicsChunk) {
        out += command.keys();
        if (!encoded.empty()) {
            out += ';';
            out += encoded;
        }
        out += "\x1b\\";
        return out;
    }
    GraphicsCommand first = command;
    first.more = 1;
    const std::string quiet = command.quiet ? ",q=" + std::to_string(command.quiet) : std::string();
    for (std::size_t at = 0; at < encoded.size(); at += GraphicsChunk) {
        const bool last = at + GraphicsChunk >= encoded.size();
        if (at) {
            out += "\x1b_G";
            out += last ? "m=0" : "m=1";
            out += quiet;
        } else {
            out += first.keys();
        }
        out += ';';
        out.append(encoded, at, GraphicsChunk);
        out += "\x1b\\";
    }
    return out;
}

std::optional<GraphicsReply> GraphicsReply::parse(std::string_view payload)
{
    const auto semi = payload.find(';');
    if (semi == std::string_view::npos) {
        return std::nullopt;
    }
    GraphicsReply reply;
    const auto command = GraphicsCommand::parse(payload.substr(0, semi));
    reply.id = command.id;
    reply.number = command.number;
    reply.placement = command.placement;
    reply.status = payload.substr(semi + 1);
    return reply;
}

std::string GraphicsReply::write() const
{
    std::string out = "\x1b_G";
    if (id) {
        out += "i=" + std::to_string(id);
    }
    if (number) {
        out += (id ? ",I=" : "I=") + std::to_string(number);
    }
    if (placement) {
        out += ",p=" + std::to_string(placement);
    }
    out += ';';
    out += status;
    out += "\x1b\\";
    return out;
}

bool graphicsInflate(std::string_view compressed, std::string& out, std::size_t limit)
{
    return zlibInflate(compressed, out, limit);
}

} // namespace AfterglowProto
