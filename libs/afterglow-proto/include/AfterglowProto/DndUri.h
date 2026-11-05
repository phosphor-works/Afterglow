// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The text/uri-list of a drag (RFC 2483), as a drag source writes it and a
/// drop receiver reads it: file URIs of this computer and data URIs.
namespace AfterglowProto {

/// One line of a text/uri-list.
struct DndUri
{
    enum class Kind : std::uint8_t {
        File, ///< a file of this computer: file:///path, file://localhost/path or file:/path
        Data, ///< a data: URI with its bytes
        Other, ///< anything else: another scheme, a file of another host, a path with a NUL
    };
    Kind kind = Kind::Other;
    /// File: the absolute, clean path.
    std::string path;
    /// Data: the MIME type (text/plain when the URI names none) and the bytes.
    std::string mime;
    std::string data;
};

/// Decodes %XX escapes. A '%' that two hex digits do not follow stays as it
/// is. nullopt when an escape decodes to a NUL byte and @p allowNul is false:
/// a path with one would end early at the first system call.
std::optional<std::string> dndPercentDecode(std::string_view text, bool allowNul = false);
/// Escapes every byte of @p path but the unreserved characters, the
/// sub-delimiters, ':', '@' and '/'.
std::string dndPercentEncode(std::string_view path);
/// The file URI of the absolute path @p path. A directory's URI ends with a
/// slash, as the drag and drop protocol requires.
std::string dndFileUri(std::string_view path, bool directory);
/// The URIs of a text/uri-list, one for each line that is not empty and not
/// a comment, in their order. Lines end with CRLF or LF. A file URI with a
/// host other than "localhost" names a file of another computer: Kind::Other.
std::vector<DndUri> parseDndUriList(std::string_view text);

} // namespace AfterglowProto
