// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Kitty's drag and drop protocol (OSC 72): "ESC ] 72 ; k=v:k=v ; payload ST".
/// https://sw.kovidgoyal.net/kitty/dnd-protocol/
namespace AfterglowProto {

/// The metadata of one message, by key. Keys are single letters.
using DndMeta = std::map<std::string, std::string>;

/// The most payload bytes in one packet. Longer payloads go in chunks.
inline constexpr std::size_t DndMaxPacket = 4096;

/// A decimal integer as kitty's tools write it, with optional sign and
/// surrounding ASCII white space; nullopt for anything else or an overflow.
std::optional<std::int64_t> parseDndInteger(std::string_view text);
/// Splits a command (the text after "72;") at its first ';' into the
/// metadata and the payload.
std::pair<std::string_view, std::string_view> splitDnd(std::string_view command);
/// Reads the metadata. Only the keys t, i, x, y, X, Y, o and m count: t is
/// one character, the others are integers from INT32_MIN to UINT32_MAX.
/// nullopt for a repeated key or a bad value, which ends the protocol.
std::optional<DndMeta> parseDndMeta(std::string_view raw);
/// The t= value of metadata that may not parse, to report an error to the right role.
std::string dndType(std::string_view raw);
/// The packets for one message: the payload in chunks of DndMaxPacket
/// bytes, each with m=1 but the last, which has m=0.
std::vector<std::string> writeDnd(DndMeta meta, std::string_view payload);
/// The answer to a query (t=q or t=Q), or empty when @p command is none.
std::string dndQueryReply(std::string_view command);
/// A MIME type that may go into a message: printable ASCII with a '/', at most 255 bytes.
bool dndValidMime(std::string_view mime);
/// The machine ID of the protocol: "1:" and the hex HMAC-SHA256 of the
/// trimmed contents of /etc/machine-id, keyed with "tty-dnd-protocol-machine-id".
/// Both sides compare it to tell whether they share a file system.
std::string dndMachineId(std::string_view machineIdFile);
/// The machine ID of this computer; empty when /etc/machine-id cannot be read.
std::string dndMachineId();

/// Puts chunked messages (m=1 until the last) back together.
class DndAssembler
{
public:
    enum class Result : std::uint8_t {
        Pending, ///< a chunk was stored
        Ready, ///< a whole message is in the output
        Mismatch, ///< a chunk names another id: the protocol ends
        TooLarge, ///< the payload passed its limit
    };

    /// Payloads of the data types (t=p, e and k) may reach @p maxData
    /// bytes, all others @p maxControl.
    DndAssembler(std::size_t maxData, std::size_t maxControl);

    /// Feeds one message. On Ready, @p meta and @p payload hold the whole
    /// message. When the last chunk had no payload, the metadata gets
    /// "_end" = "1".
    Result feed(const DndMeta& message, std::string_view data, DndMeta& meta, std::string& payload);
    bool active() const { return !m_meta.empty(); }
    /// The type of the message being assembled ("a" when it gives none).
    std::string type() const;
    void reset();

private:
    std::size_t m_maxData;
    std::size_t m_maxControl;
    DndMeta m_meta;
    std::string m_payload;
};

} // namespace AfterglowProto
