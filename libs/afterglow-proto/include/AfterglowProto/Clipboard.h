// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// Kitty's clipboard protocol (OSC 5522) and xterm's OSC 52.
namespace AfterglowProto {

/// A clipboard request (kitty's OSC 5522): reads and writes of any MIME type.
/// https://sw.kovidgoyal.net/kitty/clipboard/
///
/// The terminal fills it from what a program sends (ClipboardAssembler, or
/// its OSC 52 parser). A client fills kind, primary, id, password, name,
/// mimes, data and aliases for the request writers below, which read
/// nothing else. The osc52 fields are the terminal's own: how to answer an
/// OSC 52 read.
struct ClipboardRequest
{
    enum class Kind : std::uint8_t { Read, Write };
    Kind kind = Kind::Read;
    bool primary = false; ///< loc=primary: the primary selection
    std::string id; ///< echoed in every reply; sanitized
    std::string password; ///< decoded 'pw'
    std::string name; ///< decoded human-friendly name
    /// Read: the MIME types asked for; "." alone asks for the list of types.
    std::vector<std::string> mimes;
    /// Write: decoded data per MIME type, in the order sent.
    std::vector<std::pair<std::string, std::string>> data;
    /// Write: alias -> MIME type whose data it shares.
    std::vector<std::pair<std::string, std::string>> aliases;
    /// An OSC 52 read ("?") rather than OSC 5522: answer as OSC 52 with the
    /// text, for @ref osc52Selection, ending as the request did.
    bool osc52 = false;
    bool osc52Bel = false;
    std::string osc52Selection;

    /// The OSC 52 answer: @p text (raw bytes) for the selection asked about.
    std::string osc52Reply(std::string_view text) const;
    /// The answer that refuses the request with @p status (EPERM, ENOSYS).
    /// An OSC 52 read gets an empty answer: its protocol has no status. An
    /// OSC 52 write gets none: empty.
    std::string refusal(std::string_view status) const;

    bool operator==(const ClipboardRequest&) const = default;
};

/// The terminal side: reads requests, putting write chunks back together,
/// and writes the replies.
class ClipboardAssembler
{
public:
    /// Writes may carry this much in total (the protocol requires at least 64 MB).
    static constexpr std::size_t MaxWrite = 128u * 1024 * 1024;
    /// The most data bytes, before base64, in one DATA or wdata packet (the protocol's limit).
    static constexpr std::size_t MaxChunk = 4096;
    /// The data bytes that the writers put in each packet of a type but the
    /// last: whole groups of three, so that each packet decodes alone and the
    /// packets of a type also join into one stream with padding only at its end.
    static constexpr std::size_t DataChunk = MaxChunk / 3 * 3;
    /// The most MIME types in one request, and the most aliases in a write.
    static constexpr std::size_t MaxParts = 64;

    enum class Result : std::uint8_t { Pending, Ready, Ignored, Error };

    /// Feeds the OSC payload after "5522;". On Ready, @p out holds the
    /// request; on Error, @p reply holds the error packet to send back.
    Result feed(std::string_view payload, ClipboardRequest& out, std::string& reply);
    void reset();

    // Replies, as complete OSC sequences.
    static std::string readStatus(std::string_view status, const ClipboardRequest& request, std::string_view oneTimePassword = {});
    /// The DATA packets for one MIME type, in chunks of DataChunk bytes.
    static std::string readData(std::string_view mime, std::string_view bytes, const ClipboardRequest& request,
                                std::string_view oneTimePassword = {});
    static std::string writeStatus(std::string_view status, std::string_view id);

private:
    Result fail(std::string_view status, std::string& reply);
    bool flush(std::string& reply);

    bool m_writing = false;
    bool m_failed = false; ///< after an error, packets are ignored until the next write
    ClipboardRequest m_write;
    std::string m_mime; ///< the type being received
    std::string m_part; ///< its data so far, decoded as it arrives
    std::string m_carry; ///< base64 characters short of a whole group
    std::size_t m_decoded = 0; ///< data of the types before it
};

// The client side of the protocol: what a program writes to the terminal,
// and how it reads the replies. The tests use it to drive the terminal, and
// clients of the library use it to talk to one. The writers send the id as
// sanitizeId() leaves it, and only the types and aliases that are type names
// (printable ASCII without spaces): a data part or an alias with another name
// is left out. They do not check that the password and the name are clean
// text, or the limits on sizes (MaxWrite, a field of 4096 base64 characters,
// the number of types). A terminal refuses or cuts a request that breaks them.

/// A read request for @p request's mimes ("." alone asks for the list of types).
std::string clipboardReadRequest(const ClipboardRequest& request);
/// A whole write: the data of each type in chunks, the aliases, and the end packet.
std::string clipboardWriteRequest(const ClipboardRequest& request);

/// One reply of the terminal (after "5522;").
struct ClipboardReply
{
    std::string type; ///< "read" or "write"
    std::string status; ///< OK, DATA, DONE or an error such as EPERM
    std::string id; ///< sanitized, as the request's
    std::string mime; ///< DATA: the type of the data
    bool primary = false;
    std::string password; ///< a one-time password for later reads
    std::string data; ///< DATA: decoded; a type's data may come in several replies

    /// nullopt when a field or the data is not valid base64.
    static std::optional<ClipboardReply> parse(std::string_view payload);
    bool operator==(const ClipboardReply&) const = default;
};

/// xterm's selection letters (c, p, q, s, 0-7) among the first 16 bytes of
/// @p selection. Only these are safe to echo in an OSC 52 sequence.
std::string osc52Letters(std::string_view selection);

/// An OSC 52 message: a write, a read ("?"), or a terminal's answer to a
/// read. Client side: parse() and write() keep only osc52Letters().
struct Osc52Message
{
    std::string selection; ///< xterm's letters: c, p, q, s, 0-7
    bool query = false;
    std::string text; ///< decoded

    /// Reads the text after "52;"; nullopt without the ';' after the selection.
    static std::optional<Osc52Message> parse(std::string_view payload);
    /// The complete sequence, ending with ST.
    std::string write() const;
    bool operator==(const Osc52Message&) const = default;
};

} // namespace AfterglowProto
