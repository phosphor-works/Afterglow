// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/afterglowvt_export.h>

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

    Kind kind = Kind::Show;
    std::string id; ///< [A-Za-z0-9_+.-] only; empty when the program gave none
    std::string title; ///< UTF-8 without control characters
    std::string body;
    std::vector<std::string> buttons;
    std::vector<std::string> iconNames; ///< first one available wins
    std::string iconData; ///< PNG, JPEG or GIF bytes
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
};

/// Assembles OSC 99 sequences into notification requests.
class AFTERGLOWVT_EXPORT NotificationAssembler
{
public:
    enum class Result : std::uint8_t { Pending, Ready, Query, Ignored };

    /// Feeds the OSC payload after "99;". On Ready, @p out holds the request;
    /// on Query, @p out.id holds the id to answer with.
    Result feed(std::string_view payload, NotificationRequest& out);
    void reset() { m_pending.clear(); }

    /// The reply to a capability query, given the host's capability list
    /// ("a=focus,report:c=1:...").
    static std::string queryReply(std::string_view id, std::string_view capabilities);
    /// Strips an identifier down to the characters the protocol allows, so
    /// it is safe to echo back.
    static std::string sanitizeId(std::string_view id);

private:
    struct Pending
    {
        NotificationRequest request;
        std::string buttons; ///< U+2028-separated, split when complete
        std::size_t bytes = 0;
        std::uint64_t serial = 0;
    };
    std::unordered_map<std::string, Pending> m_pending;
    std::uint64_t m_serial = 0;
};

} // namespace AfterglowVt
