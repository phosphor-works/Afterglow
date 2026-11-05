// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// Kitty's drag and drop protocol (OSC 72): "ESC ] 72 ; k=v:k=v ; payload ST".
/// https://sw.kovidgoyal.net/kitty/dnd-protocol/
namespace AfterglowProto {

/// The metadata of one message, by key. Keys are single letters.
using DndMeta = std::map<std::string, std::string>;

/// The most payload bytes in one packet. Longer payloads go in chunks.
inline constexpr std::size_t DndMaxPacket = 4096;
/// The longest command (the text after "72;") that a terminal takes: a full
/// packet and its metadata fit. A longer one gets EFBIG and ends the
/// protocol, so a terminal needs to keep only its first DndMaxCommand + 1 bytes.
inline constexpr std::size_t DndMaxCommand = 8192;

/// An integer value of the metadata: decimal digits with an optional sign,
/// and ASCII white space around them. kitty writes plain digits with an
/// optional '-'. nullopt for anything else and for a value outside int64.
std::optional<std::int64_t> parseDndInteger(std::string_view text);
/// Splits a command (the text after "72;") at its first ';' into the
/// metadata and the payload.
std::pair<std::string_view, std::string_view> splitDnd(std::string_view command);
/// Reads the metadata. Only the keys t, i, x, y, X, Y, o and m count: t is
/// one character, the others are integers from INT32_MIN to UINT32_MAX,
/// stored in plain decimal ("+05" reads as "5"). nullopt for a repeated key
/// or a bad value, which ends the protocol.
std::optional<DndMeta> parseDndMeta(std::string_view raw);
/// The t= value of metadata that may not parse, to report an error to the right role.
std::string dndType(std::string_view raw);
/// Whether chunks follow a packet: its first m= value is a number other
/// than 0. Reads metadata that may not parse, as dndType does, so that the
/// chunks after a malformed packet can be told from a new message.
bool dndMore(std::string_view raw);
/// Whether a drop receiver sends messages of @p type (a, A, m and r). A drag
/// source sends the others. An error goes back to the role that sent the message.
bool dndReceiverType(std::string_view type);
/// Whether @p type carries data (p, e and k). Its messages may be larger
/// than the control messages.
bool dndDataType(std::string_view type);
/// The packets for one message: the payload in chunks of DndMaxPacket
/// bytes, each with m=1 but the last, which has m=0. Keys and values are
/// written as they are: the caller gives keys of one letter and values
/// without ':', ';' or control characters.
std::vector<std::string> writeDnd(DndMeta meta, std::string_view payload);
/// The reply to a query (t=q or t=Q), with its id. Empty for every other
/// command, and for a query that breaks a rule that every message follows:
/// a command over DndMaxCommand, a payload over DndMaxPacket, or metadata
/// that parseDndMeta refuses (a bad id, for one). A terminal then handles
/// that command as any other message that breaks the rule: it may answer
/// with an error, or drop the command without a reply.
std::string dndQueryReply(std::string_view command);
/// A MIME type that may go into a message: ASCII from '!' to '~' (no space
/// and no control character) with a '/', at most 255 bytes.
bool dndValidMime(std::string_view mime);
/// The machine ID of the protocol: "1:" and the hex HMAC-SHA256 of the
/// trimmed contents of /etc/machine-id, keyed with "tty-dnd-protocol-machine-id".
/// Both sides compare it to tell whether they share a file system. Empty
/// when the trimmed contents are empty, so that no two computers share an ID by accident.
std::string dndMachineId(std::string_view machineIdContents);
/// The machine ID of this computer; empty when /etc/machine-id cannot be read or is empty.
std::string dndMachineId();

/// Puts chunked messages (m other than 0 until the last) back together.
class DndAssembler
{
public:
    enum class Result : std::uint8_t {
        Pending, ///< a chunk was stored
        Ready, ///< a whole message is in the output
        Dropped, ///< a chunk of a message that failed earlier was dropped
        Mismatch, ///< a chunk names another type or id: the protocol ends
        TooLarge, ///< the payload passed its limit
    };

    /// A whole message.
    struct Message {
        /// The metadata of the first packet, with the m of the last.
        DndMeta meta;
        std::string payload;
        /// The last packet had no payload. For the data types this ends the item.
        bool emptyEnd = false;
    };

    /// Payloads of the data types (dndDataType) may reach @p maxData
    /// bytes, all others @p maxControl. The limits apply to each message,
    /// chunked or not, from its first packet.
    DndAssembler(std::size_t maxData, std::size_t maxControl);

    /// Feeds one packet, with metadata as parseDndMeta gives it. On Ready,
    /// @p message holds the whole message. Packets after the first need not
    /// repeat the type or the id, as kitty clients write them. On Mismatch
    /// and TooLarge the message is dropped, and so are its later chunks up
    /// to the last one: they give Dropped, never the start of a new message.
    Result feed(const DndMeta& packet, std::string_view data, Message& message);
    /// Drops the message that a packet belongs to, for an error that the
    /// caller found in it. @p more is the m of that packet (dndMore): when
    /// it is set, the packets that follow are later chunks of the message
    /// and give Dropped, up to the one with m=0.
    void discard(bool more);
    /// Drops the message being put together, for an error that no packet
    /// of it carries (the protocol ended). Its later chunks give Dropped. A
    /// message that is being dropped stays so.
    void abandon();
    /// A message is being put together.
    bool active() const { return !m_meta.empty(); }
    /// The chunks of a dropped message are being dropped.
    bool discarding() const { return m_discarding; }
    /// The type of the message being put together: its t, or "a" when it
    /// gives none. "a" also when no message is being put together.
    std::string type() const;
    /// Forgets every chunk, also those of a message that is being dropped.
    void reset();

private:
    std::size_t limit(std::string_view type) const;

    std::size_t m_maxData;
    std::size_t m_maxControl;
    DndMeta m_meta;
    std::string m_payload;
    bool m_discarding = false;
};

} // namespace AfterglowProto
