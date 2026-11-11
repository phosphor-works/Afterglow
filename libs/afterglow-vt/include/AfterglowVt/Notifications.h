// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/afterglowvt_export.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AfterglowVt {

/// A desktop notification request (kitty's OSC 99), assembled from its chunks.
/// https://sw.kovidgoyal.net/kitty/desktop-notifications/
struct NotificationRequest
{
    enum class Kind : std::uint8_t {
        Show, ///< show, or replace the one with the same id
        Close, ///< close the one with this id
        Alive, ///< report which ids are still open
    };
    enum class Occasion : std::uint8_t { Always, Unfocused, Invisible };
    /// The most icon names, and the most types, that a request keeps.
    static constexpr std::size_t MaxNames = 16;
    /// The most bytes of title, body and buttons together.
    static constexpr std::size_t MaxText = 64 * 1024;

    Kind kind = Kind::Show;
    std::string id; ///< [A-Za-z0-9_+.-] only; empty when the program gave none
    std::string title; ///< UTF-8 without control characters
    std::string body; ///< UTF-8 without control characters except newlines
    std::vector<std::string> buttons;
    std::vector<std::string> iconNames; ///< first one available wins
    std::string iconData; ///< PNG, JPEG or GIF bytes; anything else is dropped
    std::string iconCacheId; ///< 'g': icon data to remember, or to reuse when iconData is empty
    std::string appName; ///< 'f'
    std::vector<std::string> types; ///< 't'
    std::string sound = "system"; ///< 's'
    bool focusOnActivate = true; ///< a=focus (default) / a=-focus
    bool reportActivation = false; ///< a=report
    bool reportClose = false; ///< c=1
    Occasion occasion = Occasion::Always;
    int urgency = 1; ///< 0 low, 1 normal, 2 critical
    int expireMs = -1; ///< -1 system default, 0 never, otherwise milliseconds

    /// The answer to an Alive query: @p open lists the ids that are still
    /// open, joined by commas.
    std::string aliveReply(std::string_view open = {}) const
    {
        return "\x1b]99;i=" + (id.empty() ? std::string("0") : id) + ":p=alive;" + std::string(open) + "\x1b\\";
    }
};

/// Assembles OSC 99 sequences into notification requests.
class AFTERGLOWVT_EXPORT NotificationAssembler
{
public:
    enum class Result : std::uint8_t { Pending, Ready, Query, Ignored };

    /// Feeds the OSC payload after "99;". On Ready, @p out holds the request;
    /// on Query, @p out.id holds the id to answer with.
    Result feed(std::string_view payload, NotificationRequest& out);
    void reset();

    /// The reply to a capability query, given the host's capability list
    /// ("a=focus,report:c=1:...").
    static std::string queryReply(std::string_view id, std::string_view capabilities);

private:
    enum class Payload : std::uint8_t { Title, Body, Buttons, Icon };
    struct Pending
    {
        NotificationRequest request;
        std::string buttons; ///< U+2028-separated, split when complete
        /// Base64 characters short of a whole group, carried to the next
        /// chunk: a sender may cut the encoded text anywhere.
        std::string carry;
        Payload carryPayload = Payload::Title;
        std::uint64_t serial = 0;
    };
    using PendingMap = std::unordered_map<std::string, Pending>;

    static std::string& target(Pending& p, Payload payload);
    void erase(PendingMap::iterator it);

    PendingMap m_pending;
    /// Ids whose chunk failed: their later chunks are ignored up to the last
    /// one, so the tail of a broken notification is not shown on its own.
    std::vector<std::string> m_failed;
    std::size_t m_pendingIconBytes = 0;
    std::uint64_t m_serial = 0;
};

} // namespace AfterglowVt
