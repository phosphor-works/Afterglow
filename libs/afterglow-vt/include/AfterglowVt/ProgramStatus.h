// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/afterglowvt_export.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AfterglowVt {

/// Program status (OSC 7501, revision 0.4): programs report what they are
/// doing (working, blocked on the user, done...) as records the terminal
/// shows outside the grid. https://www.superlogical.com/rex/docs/build/program-status
enum class StatusState : std::uint8_t { Idle, Working, Done, Blocked, Error };
enum class BlockKind : std::uint8_t { None, Permission, Question, Auth };

struct StatusRecord
{
    std::string id; ///< hierarchical, '/'-separated; empty for the root record
    StatusState state = StatusState::Idle;
    BlockKind kind = BlockKind::None; ///< with Blocked only
    int progress = -1; ///< 0-100 with Working or Blocked; -1 when indeterminate
    std::string app; ///< as reported; see ProgramStatus::effectiveApp()
    std::string title; ///< UTF-8, no control characters
    std::string msg; ///< UTF-8, no control characters
    std::uint64_t serial = 0; ///< increases with every update, terminal-wide
};

class AFTERGLOWVT_EXPORT ProgramStatus
{
public:
    static constexpr std::size_t MaxRecords = 256;

    enum class Result : std::uint8_t {
        Ignored, ///< malformed or over a limit: nothing applied
        Changed,
        Query, ///< the feature-detection query; answer with queryReply()
    };

    /// Applies one report: the OSC payload after "7501;".
    Result apply(std::string_view body);
    static std::string_view queryReply() { return "?"; }

    /// ConEmu progress (OSC 9;4;st;pr) mapped to the root record, until the
    /// first OSC 7501 report shows the program speaks the real protocol.
    /// Returns true if records changed.
    bool applyLegacyProgress(int state, int progress);

    /// RIS: every record goes, and legacy progress is mapped again.
    void reset();
    /// A new prompt began or the program exited: what was in flight is over.
    /// Working, blocked and idle records go; done and error stay to be seen.
    /// Returns true if records changed.
    bool endActivity();

    /// Records in id order (the root first).
    const std::vector<StatusRecord>& records() const noexcept { return m_records; }
    /// The record's app, or its nearest ancestor's.
    std::string_view effectiveApp(const StatusRecord& record) const;
    std::uint64_t generation() const noexcept { return m_serial; }

private:
    void upsert(StatusRecord record);
    void clear(std::string_view id, bool all);

    std::vector<StatusRecord> m_records;
    std::uint64_t m_serial = 0;
    bool m_nativeReports = false;
};

} // namespace AfterglowVt
