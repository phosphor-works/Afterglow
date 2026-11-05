// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/DragDrop.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The chunks of a remote directory tree in kitty's drag and drop protocol
/// (OSC 72): the files, links and directories that a drag source sends
/// (t=k), and the reads of a drop receiver (t=r with an X kind).
///
/// Every entry has a kind (the X key): 0 for a regular file, 1 for a
/// symbolic link (its data is the link's target), and a number from 2 up for
/// a directory. That number is the directory's handle, and its data is the
/// names of its entries, separated by NUL bytes. A negative kind is no entry.
/// The children of a directory name it with Y and their 1-based index in its
/// listing.
namespace AfterglowProto {

/// The raw bytes in one data message: their base64 fills one packet.
inline constexpr std::size_t DndDataChunk = DndMaxPacket / 4 * 3;
/// The kinds of an entry. A directory's kind is its handle, from DndFirstHandle up.
inline constexpr std::int64_t DndFileKind = 0;
inline constexpr std::int64_t DndLinkKind = 1;
inline constexpr std::int64_t DndFirstHandle = 2;

/// Whether @p kind names a directory: its handle.
constexpr bool dndIsDirectoryKind(std::int64_t kind)
{
    return kind >= DndFirstHandle;
}

/// The metadata of an entry that a drag source sends (t=k). @p root is the
/// 1-based index of the entry's tree in text/uri-list. A child also names its
/// directory's handle @p parent and its 1-based index @p child there; a root
/// entry has neither.
DndMeta dndRemoteEntry(std::int64_t root, std::int64_t parent, std::int64_t child, std::int64_t kind);
/// The metadata of a drop receiver's read of a remote entry (t=r): a root
/// entry is entry @p child of text/uri-list, the type at 1-based index
/// @p mime of the drop; a child is entry @p child of the directory
/// @p parent. With @p child 0, the read of a root entry (@p parent 0) reads
/// the data of the type itself, and the read of a child closes the handle
/// of the directory @p parent.
DndMeta dndReadEntry(std::int64_t mime, std::int64_t parent, std::int64_t child);

/// The packets of one data item (t=p, t=e or t=k): @p bytes in messages of
/// DndDataChunk bytes, each base64 that decodes alone, and then an empty
/// message that ends the item.
std::vector<std::string> dndDataPackets(const DndMeta& meta, std::string_view bytes);

/// A directory listing: @p names joined with NUL bytes.
std::string dndJoinNames(const std::vector<std::string>& names);
/// Whether a listing can name @p name: it is not empty, has at most 255
/// bytes, is UTF-8, holds no '/' or NUL, and is not "." or "..". It can hold
/// control characters, as a file name can: a program that prints one must
/// make it printable first.
bool dndValidName(std::string_view name);
/// The names of a listing; nullopt when one is not valid (dndValidName) or
/// repeats. An empty listing has no names.
std::optional<std::vector<std::string>> dndSplitNames(std::string_view listing);

/// Decodes the base64 of one item as its packets arrive, whether each packet
/// decodes alone (Afterglow) or a group of four characters spans two
/// packets. A packet that decodes alone must pad its last group with '=':
/// without the padding, its characters are read as the start of a group that
/// the next packet ends, and no decoder can tell the two apart.
class DndBase64Stream
{
public:
    /// Appends the bytes that the characters so far decode to. False for
    /// characters that are not base64.
    bool feed(std::string_view text, std::string& out);
    /// Decodes what is left at the end of the item. False when it is not
    /// base64 or a whole item was cut short.
    bool finish(std::string& out);
    void reset() { m_carry.clear(); }

private:
    std::string m_carry;
};

} // namespace AfterglowProto
