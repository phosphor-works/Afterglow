// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/DndUri.h>

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/TransferPath.h>

#include <algorithm>
#include <cstring>

namespace AfterglowProto {

namespace {

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool asciiAlnum(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

DndUri fileUri(std::string_view rest)
{
    // rest is what follows "file:". "//host/path" names a host, "/path" none.
    DndUri uri;
    std::string_view path = rest;
    if (rest.starts_with("//")) {
        const auto slash = rest.find('/', 2);
        if (slash == std::string_view::npos) {
            return uri;
        }
        const auto host = rest.substr(2, slash - 2);
        if (!host.empty() && host != "localhost") {
            return uri;
        }
        path = rest.substr(slash);
    }
    if (!path.starts_with('/')) {
        return uri;
    }
    // A file URI has no query or fragment. A '?' or '#' here is part of a
    // name that the source did not escape. Cut at it, the path would name
    // another file, which a move would delete: such a URI names no file here.
    if (path.find_first_of("?#") != std::string_view::npos) {
        return uri;
    }
    const auto decoded = dndPercentDecode(path);
    if (!decoded) {
        return uri;
    }
    uri.kind = DndUri::Kind::File;
    uri.path = TransferPath::clean(*decoded);
    return uri;
}

DndUri dataUri(std::string_view rest)
{
    DndUri uri;
    const auto comma = rest.find(',');
    if (comma == std::string_view::npos) {
        return uri;
    }
    std::string_view header = rest.substr(0, comma);
    const bool base64 = header.ends_with(";base64");
    if (base64) {
        header.remove_suffix(7);
    }
    uri.mime = std::string(header.substr(0, std::min(header.find(';'), header.size())));
    if (uri.mime.empty()) {
        uri.mime = "text/plain";
    }
    const auto payload = rest.substr(comma + 1);
    if (base64) {
        auto bytes = base64Decode(payload, Base64Check::PaddingOptional);
        if (!bytes) {
            return uri;
        }
        uri.data = std::move(*bytes);
    } else {
        auto bytes = dndPercentDecode(payload, true);
        uri.data = std::move(*bytes);
    }
    uri.kind = DndUri::Kind::Data;
    return uri;
}

} // namespace

std::optional<std::string> dndPercentDecode(std::string_view text, bool allowNul)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const int high = text[i] == '%' && i + 2 < text.size() ? hexValue(text[i + 1]) : -1;
        const int low = high >= 0 ? hexValue(text[i + 2]) : -1;
        if (low < 0) {
            out += text[i];
            continue;
        }
        const char byte = char(high * 16 + low);
        if (byte == '\0' && !allowNul) {
            return std::nullopt;
        }
        out += byte;
        i += 2;
    }
    return out;
}

std::string dndPercentEncode(std::string_view path)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (const char ch : path) {
        const auto c = static_cast<unsigned char>(ch);
        if (asciiAlnum(c) || (c != '\0' && std::strchr("/-._~!$&'()*+,;=:@", c))) {
            out += ch;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string dndFileUri(std::string_view path, bool directory)
{
    return "file://" + dndPercentEncode(path) + (directory && path != "/" ? "/" : "");
}

std::vector<DndUri> parseDndUriList(std::string_view text)
{
    std::vector<DndUri> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = std::min(text.find('\n', start), text.size());
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }
        line.remove_prefix(std::min(line.find_first_not_of(" \t"), line.size()));
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        if (line.starts_with("file:")) {
            out.push_back(fileUri(line.substr(5)));
        } else if (line.starts_with("data:")) {
            out.push_back(dataUri(line.substr(5)));
        } else {
            out.emplace_back();
        }
    }
    return out;
}

} // namespace AfterglowProto
