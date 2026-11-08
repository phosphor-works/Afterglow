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
        if (key == 'a' || key == 't' || key == 'd' || key == 'o' || key == 'e') {
            const char ch = value.empty() ? 0 : value[0];
            char& flag = key == 'a' ? cmd.action
                : key == 't'         ? cmd.medium
                : key == 'd'         ? cmd.del
                : key == 'o'         ? cmd.compression
                                     : cmd.feature;
            flag = ch;
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
        case 'N': cmd.hints = u; break;
        default: break;
        }
    }
    return cmd;
}

bool GraphicsCommand::flagsAreLetters() const
{
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    return (!(hasAction || action != 't') || letter(action)) && (medium == 'd' || letter(medium))
        && (del == 'a' || letter(del)) && (!compression || letter(compression)) && (!feature || letter(feature));
}

std::string GraphicsCommand::keys() const
{
    // Leaving the flag out would change what the command does: d=, for one,
    // would delete every image.
    if (!flagsAreLetters()) {
        return {};
    }
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
    if (feature) add('e', feature);
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
    // Yi is Y read as an int (a=c's source row): it stands in when Y is unset.
    if (Y) add('Y', Y);
    else if (Yi) add('Y', Yi);
    if (c) add('c', c);
    if (r) add('r', r);
    if (C) add('C', C);
    if (z) add('z', z);
    if (U) add('U', U);
    if (P) add('P', P);
    if (Q) add('Q', Q);
    if (H) add('H', H);
    if (V) add('V', V);
    if (hints) add('N', hints);
    return out;
}

std::string writeGraphics(const GraphicsCommand& command, std::string_view payload)
{
    if (!command.flagsAreLetters()) {
        return {};
    }
    const std::string encoded = base64Encode(payload);
    std::string out = "\x1b_G";
    if (encoded.size() <= GraphicsChunk) {
        // One command is the whole transmission: an m=1 from the caller
        // would leave the terminal waiting for chunks that never come.
        GraphicsCommand whole = command;
        whole.more = 0;
        out += whole.keys();
        if (!encoded.empty()) {
            out += ';';
            out += encoded;
        }
        out += "\x1b\\";
        return out;
    }
    GraphicsCommand first = command;
    first.more = 1;
    // The protocol asks a frame's later chunks for a=f too: a terminal tells
    // them from the chunks of an image by it.
    const std::string frame = command.action == 'f' ? "a=f," : "";
    const std::string quiet = command.quiet ? ",q=" + std::to_string(command.quiet) : std::string();
    for (std::size_t at = 0; at < encoded.size(); at += GraphicsChunk) {
        const bool last = at + GraphicsChunk >= encoded.size();
        if (at) {
            out += "\x1b_G";
            out += frame;
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
    const std::string_view keys = payload.substr(0, semi);
    const auto command = GraphicsCommand::parse(keys);
    reply.id = command.id;
    reply.number = command.number;
    reply.placement = command.placement;
    reply.status = payload.substr(semi + 1);
    // Everything but i, I and p, as written.
    for (std::size_t pos = 0; pos < keys.size();) {
        const auto comma = keys.find(',', pos);
        const std::string_view kv = keys.substr(pos, comma == std::string_view::npos ? comma : comma - pos);
        pos = comma == std::string_view::npos ? keys.size() : comma + 1;
        if (kv.empty() || (kv.size() >= 2 && kv[1] == '=' && (kv[0] == 'i' || kv[0] == 'I' || kv[0] == 'p'))) {
            continue;
        }
        if (!reply.keys.empty()) reply.keys += ',';
        reply.keys += kv;
    }
    return reply;
}

std::string GraphicsReply::write() const
{
    std::string out = "\x1b_G";
    bool first = true;
    auto add = [&](std::string_view key, std::uint32_t value) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += key;
        out += std::to_string(value);
    };
    if (id) add("i=", id);
    if (number) add("I=", number);
    if (placement) add("p=", placement);
    if (!keys.empty()) {
        if (!first) out += ',';
        out += keys;
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
