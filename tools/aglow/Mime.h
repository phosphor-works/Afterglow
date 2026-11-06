// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

// One table of file name extensions and MIME types, for aglow clipboard,
// which guesses the type of a file, and aglow dnd, which names a dropped file.
namespace Aglow::Mime {

// The first extension of a type is the one that a new file of that type gets.
inline constexpr std::pair<std::string_view, std::string_view> Extensions[] = {
    { "txt", "text/plain" }, { "text", "text/plain" }, { "log", "text/plain" }, { "md", "text/markdown" },
    { "rst", "text/x-rst" }, { "html", "text/html" }, { "htm", "text/html" }, { "css", "text/css" },
    { "csv", "text/csv" }, { "xml", "application/xml" }, { "json", "application/json" },
    { "js", "text/javascript" }, { "rtf", "text/rtf" }, { "uri", "text/uri-list" }, { "uris", "text/uri-list" },
    { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "gif", "image/gif" },
    { "webp", "image/webp" }, { "bmp", "image/bmp" }, { "svg", "image/svg+xml" }, { "tif", "image/tiff" },
    { "tiff", "image/tiff" }, { "avif", "image/avif" }, { "pdf", "application/pdf" },
    { "zip", "application/zip" }, { "sh", "text/x-shellscript" }, { "py", "text/x-python" }, { "c", "text/x-c" },
    { "h", "text/x-c" }, { "cpp", "text/x-c++" },
};

/// The MIME type of a file from the extension of @p name (any case), or empty.
inline std::string typeOfName(std::string_view name)
{
    if (const auto slash = name.rfind('/'); slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    std::string extension(name.substr(dot + 1));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    const auto it = std::find_if(std::begin(Extensions), std::end(Extensions),
                                 [&](const auto& entry) { return entry.first == extension; });
    return it == std::end(Extensions) ? std::string() : std::string(it->second);
}

/// The extension, with its dot, of a new file of type @p mime, or empty.
inline std::string extensionOf(std::string_view mime)
{
    const auto it = std::find_if(std::begin(Extensions), std::end(Extensions),
                                 [&](const auto& entry) { return entry.second == mime; });
    return it == std::end(Extensions) ? std::string() : "." + std::string(it->first);
}

} // namespace Aglow::Mime
