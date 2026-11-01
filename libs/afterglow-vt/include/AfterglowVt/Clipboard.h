// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/afterglowvt_export.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AfterglowVt {

/// A clipboard request (kitty's OSC 5522): reads and writes of any MIME type.
/// https://sw.kovidgoyal.net/kitty/clipboard/
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
    AFTERGLOWVT_EXPORT std::string osc52Reply(std::string_view text) const;
};

class AFTERGLOWVT_EXPORT ClipboardAssembler
{
public:
    /// Writes may carry this much in total (the protocol requires at least 64 MB).
    static constexpr std::size_t MaxWrite = 128u * 1024 * 1024;
    static constexpr std::size_t ReadChunk = 4096;

    enum class Result : std::uint8_t { Pending, Ready, Ignored, Error };

    /// Feeds the OSC payload after "5522;". On Ready, @p out holds the
    /// request; on Error, @p reply holds the error packet to send back.
    Result feed(std::string_view payload, ClipboardRequest& out, std::string& reply);
    void reset();

    // Replies, as complete OSC sequences.
    static std::string readStatus(std::string_view status, const ClipboardRequest& request, std::string_view oneTimePassword = {});
    /// The DATA packets for one MIME type, chunked.
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
    bool m_padded = false; ///< its base64 ended with padding: no more data may follow
    std::size_t m_decoded = 0; ///< data of the types before it
};

} // namespace AfterglowVt
