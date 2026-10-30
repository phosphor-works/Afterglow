// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/ProgramStatus.h>

#include "Base64.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>

namespace AfterglowVt {

namespace {

// Limits from the specification. A report over any of them is discarded.
constexpr std::size_t MaxSequence = 4096; // the whole OSC, introducer and terminator included
constexpr std::size_t SequenceOverhead = 9; // ESC ] 7 5 0 1 ; and ESC '\'
constexpr std::size_t MaxKey = 16;
constexpr std::size_t MaxMsgEncoded = 2732;
constexpr std::size_t MaxMsgDecoded = 2048;
constexpr std::size_t MaxTitleEncoded = 256;
constexpr std::size_t MaxTitleDecoded = 192;
constexpr std::size_t MaxApp = 32;
constexpr std::size_t MaxId = 128;
constexpr std::size_t MaxIdSegment = 32;
constexpr int MaxIdDepth = 8;

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

/// Valid UTF-8 with no C0, DEL or C1 control characters (newlines and tabs
/// included): the text is shown outside the grid, on one line.
bool isCleanUtf8(std::string_view s)
{
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        char32_t cp;
        int extra;
        if (b < 0x80) {
            cp = b;
            extra = 0;
        } else if ((b & 0xE0) == 0xC0) {
            cp = b & 0x1F;
            extra = 1;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0F;
            extra = 2;
        } else if ((b & 0xF8) == 0xF0) {
            cp = b & 0x07;
            extra = 3;
        } else {
            return false;
        }
        if (i + std::size_t(extra) >= s.size() && extra > 0) {
            return false; // truncated sequence
        }
        for (int k = 1; k <= extra; ++k) {
            const auto c = static_cast<unsigned char>(s[i + std::size_t(k)]);
            if ((c & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (c & 0x3F);
        }
        static constexpr char32_t MinForLength[] = {0, 0x80, 0x800, 0x10000};
        if (cp < MinForLength[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
            return false;
        }
        i += std::size_t(extra) + 1;
    }
    return true;
}

bool validId(std::string_view id)
{
    if (id.empty() || id.size() > MaxId) {
        return false;
    }
    int depth = 0;
    std::size_t pos = 0;
    while (pos <= id.size()) {
        const auto slash = id.find('/', pos);
        const std::string_view seg = id.substr(pos, slash == std::string_view::npos ? slash : slash - pos);
        if (seg.empty() || seg.size() > MaxIdSegment || ++depth > MaxIdDepth) {
            return false;
        }
        for (char c : seg) {
            if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7F) {
                return false;
            }
        }
        if (slash == std::string_view::npos) {
            break;
        }
        pos = slash + 1;
    }
    return true;
}

bool isDescendantOrSelf(std::string_view id, std::string_view ancestor)
{
    return id == ancestor
        || (id.size() > ancestor.size() && id.starts_with(ancestor) && id[ancestor.size()] == '/');
}

} // namespace

ProgramStatus::Result ProgramStatus::apply(std::string_view body)
{
    if (body.size() + SequenceOverhead > MaxSequence) {
        return Result::Ignored;
    }
    if (trim(body) == "?") {
        return Result::Query;
    }

    std::optional<std::string_view> state, id, kind, progress, app, title, msg;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        const auto colon = body.find(':', pos);
        const std::string_view pair = body.substr(pos, colon == std::string_view::npos ? colon : colon - pos);
        const auto eq = pair.find('=');
        if (eq != std::string_view::npos) {
            const std::string_view key = trim(pair.substr(0, eq));
            const std::string_view value = trim(pair.substr(eq + 1));
            if (key.size() > MaxKey) {
                return Result::Ignored;
            }
            // Repeated keys: the last one wins. Unknown keys are extensions.
            if (key == "state") {
                state = value;
            } else if (key == "id") {
                id = value;
            } else if (key == "kind") {
                kind = value;
            } else if (key == "progress") {
                progress = value;
            } else if (key == "app") {
                app = value;
            } else if (key == "title") {
                title = value;
            } else if (key == "msg") {
                msg = value;
            }
        } // a pair without '=' is malformed: skipped
        if (colon == std::string_view::npos) {
            break;
        }
        pos = colon + 1;
    }

    if (!state) {
        return Result::Ignored;
    }
    if (id && !validId(*id)) {
        return Result::Ignored; // must not fall through to the root record
    }
    if (*state == "clear") {
        m_nativeReports = true;
        clear(id.value_or(std::string_view{}), !id);
        return Result::Changed;
    }

    StatusRecord r;
    r.id = std::string(id.value_or(std::string_view{}));
    if (*state == "idle") {
        r.state = StatusState::Idle;
    } else if (*state == "working") {
        r.state = StatusState::Working;
    } else if (*state == "done") {
        r.state = StatusState::Done;
    } else if (*state == "blocked") {
        r.state = StatusState::Blocked;
    } else if (*state == "error") {
        r.state = StatusState::Error;
    } else {
        return Result::Ignored;
    }

    if (kind && r.state == StatusState::Blocked) {
        r.kind = *kind == "permission" ? BlockKind::Permission
            : *kind == "question"      ? BlockKind::Question
            : *kind == "auth"          ? BlockKind::Auth
                                       : BlockKind::None;
    }
    if (progress && (r.state == StatusState::Working || r.state == StatusState::Blocked)) {
        int v = -1;
        const auto [end, ec] = std::from_chars(progress->data(), progress->data() + progress->size(), v);
        if (ec == std::errc{} && end == progress->data() + progress->size() && v >= 0 && v <= 100) {
            r.progress = v;
        }
    }
    if (app) {
        if (app->size() > MaxApp || !isCleanUtf8(*app)) {
            return Result::Ignored;
        }
        r.app = std::string(*app);
    }
    if (title) {
        const auto decoded = title->size() <= MaxTitleEncoded ? detail::base64Decode(*title, detail::Base64Check::PaddingOptional) : std::nullopt;
        if (!decoded || decoded->size() > MaxTitleDecoded || !isCleanUtf8(*decoded)) {
            return Result::Ignored;
        }
        r.title = *decoded;
    }
    if (msg) {
        const auto decoded = msg->size() <= MaxMsgEncoded ? detail::base64Decode(*msg, detail::Base64Check::PaddingOptional) : std::nullopt;
        if (!decoded || decoded->size() > MaxMsgDecoded || !isCleanUtf8(*decoded)) {
            return Result::Ignored;
        }
        r.msg = *decoded;
    }
    m_nativeReports = true;
    upsert(std::move(r));
    return Result::Changed;
}

bool ProgramStatus::applyLegacyProgress(int state, int progress)
{
    if (m_nativeReports) {
        return false;
    }
    StatusRecord r;
    switch (state) {
    case 0: // remove
        if (m_records.empty() || !m_records.front().id.empty()) {
            return false;
        }
        m_records.erase(m_records.begin());
        ++m_serial;
        return true;
    case 1: // set
    case 4: // paused
        r.state = StatusState::Working;
        r.progress = std::clamp(progress, 0, 100);
        break;
    case 2:
        r.state = StatusState::Error;
        break;
    case 3: // indeterminate
        r.state = StatusState::Working;
        break;
    default:
        return false;
    }
    upsert(std::move(r));
    return true;
}

void ProgramStatus::reset()
{
    m_records.clear();
    m_nativeReports = false;
    ++m_serial;
}

bool ProgramStatus::endActivity()
{
    const auto before = m_records.size();
    std::erase_if(m_records, [](const StatusRecord& r) {
        return r.state == StatusState::Working || r.state == StatusState::Blocked || r.state == StatusState::Idle;
    });
    if (m_records.size() == before) {
        return false;
    }
    ++m_serial;
    return true;
}

std::string_view ProgramStatus::effectiveApp(const StatusRecord& record) const
{
    if (!record.app.empty()) {
        return record.app;
    }
    // The nearest ancestor with an app, ending at the root record.
    std::string_view id = record.id;
    while (!id.empty()) {
        const auto slash = id.rfind('/');
        id = slash == std::string_view::npos ? std::string_view{} : id.substr(0, slash);
        for (const StatusRecord& r : m_records) {
            if (r.id == id && !r.app.empty()) {
                return r.app;
            }
        }
    }
    return {};
}

void ProgramStatus::upsert(StatusRecord record)
{
    record.serial = ++m_serial;
    // One record per id; a report replaces it whole.
    const auto it = std::lower_bound(m_records.begin(), m_records.end(), record.id,
                                     [](const StatusRecord& r, const std::string& id) { return r.id < id; });
    if (it != m_records.end() && it->id == record.id) {
        *it = std::move(record);
        return;
    }
    if (m_records.size() >= MaxRecords) {
        // Full: the least recently updated record makes room.
        const auto oldest = std::min_element(m_records.begin(), m_records.end(),
                                             [](const StatusRecord& a, const StatusRecord& b) { return a.serial < b.serial; });
        const auto offset = (it - m_records.begin()) - (oldest < it ? 1 : 0);
        m_records.erase(oldest);
        m_records.insert(m_records.begin() + offset, std::move(record));
        return;
    }
    m_records.insert(it, std::move(record));
}

void ProgramStatus::clear(std::string_view id, bool all)
{
    if (all) {
        m_records.clear();
    } else {
        std::erase_if(m_records, [id](const StatusRecord& r) { return isDescendantOrSelf(r.id, id); });
    }
    ++m_serial;
}

} // namespace AfterglowVt
