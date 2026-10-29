// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowPty/Pty.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Search.h>
#include <AfterglowVt/Selection.h>
#include <AfterglowVt/Terminal.h>

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QString>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

namespace Afterglow {

/// A program status record (OSC 7501) as the GUI uses it.
struct PaneStatusRecord
{
    QString id;
    AfterglowVt::StatusState state = AfterglowVt::StatusState::Idle;
    AfterglowVt::BlockKind kind = AfterglowVt::BlockKind::None;
    int progress = -1;
    QString app; ///< inherited from ancestors when the record has none
    QString title;
    QString message;
    quint64 serial = 0;
};

/// One terminal: the emulator state, the child process on its pty, and the
/// snapshot the window renders from. The pty thread feeds the terminal under
/// m_mutex; everything else runs on the GUI thread.
class Pane : public QObject, private AfterglowVt::ITerminalHost, private AfterglowPty::IPtyListener
{
    Q_OBJECT

public:
    explicit Pane(const AfterglowVt::TerminalOptions& options, QObject* parent = nullptr);
    ~Pane() override;

    /// Sizes the terminal and starts the shell on a pty of that size.
    /// @p term is the TERM given to the shell.
    bool start(const AfterglowPty::SpawnOptions& options, const QString& term, int cols, int rows, int cellWidth,
               int cellHeight, QString* error);
    /// Applies changed config to the running terminal.
    void setOptions(const AfterglowVt::TerminalOptions& options);

    void resize(int cols, int rows, int cellWidth, int cellHeight);
    int cols() const;
    int rows() const;

    /// Sends bytes to the child and snaps the viewport back to the bottom.
    void sendInput(std::string_view bytes);

    /// Runs @p fn with the terminal locked.
    template<typename Fn>
    auto withTerminal(Fn&& fn)
    {
        std::lock_guard lock(m_mutex);
        return fn(m_terminal);
    }

    /// Refreshes the snapshot if the terminal changed. Returns false while the
    /// previous frame is held: synchronized output (mode 2026), or a resized
    /// prompt the shell has not repainted yet.
    /// @p extraRows below the viewport are captured for fractional scrolling.
    bool refreshSnapshot(int extraRows = 0);
    const AfterglowRender::PaneSnapshot& snapshot() const noexcept { return m_snapshot; }

    /// Unique for the life of the process; scripts and afterglow ctl use it.
    quint32 id() const noexcept { return m_id; }
    /// The program's title, or the one set through the control interface.
    QString title() const;
    /// Overrides the title; empty returns to the program's.
    void setUserTitle(const QString& title);
    QString workingDirectory() const;
    QString text(AfterglowVt::Terminal::TextScope scope);
    /// The command running now (OSC 133), empty at the prompt.
    QString runningCommand() const;

    // Search (GUI thread).
    struct SearchResult
    {
        int current = -1; ///< 0-based, -1 for no match
        int count = 0;
    };
    /// Runs or steps a search and scrolls the match into view. @p direction:
    /// 0 searches afresh from the bottom of the viewport, 1 next, -1 previous.
    SearchResult search(const QString& query, int direction);
    void clearSearch();

    /// Text being composed by an input method, drawn over the grid at the
    /// cursor until it is committed. @p cursor is its caret, in characters.
    void setPreedit(const QString& text, int cursor);

    /// Program status records, copied (GUI thread).
    QList<PaneStatusRecord> status() const;

    /// Scrolls to the previous (@p direction < 0) or next shell prompt.
    /// Returns false when there is none that way.
    bool jumpToPrompt(int direction);

    // Selection (GUI thread). Points are cells in the current viewport.
    AfterglowVt::SelectionPoint selectionPointAt(int col, int row) const;
    void startSelection(AfterglowVt::SelectionPoint anchor, AfterglowVt::SelectionMode mode, bool activeNow);
    void extendSelection(AfterglowVt::SelectionPoint head);
    void clearSelection();
    bool hasSelection() const;
    QString selectedText() const;

Q_SIGNALS:
    /// The terminal changed. Emitted on the GUI thread, coalesced.
    void contentChanged();
    void titleUpdated(const QString& title);
    void bellRang();
    void clipboardWriteRequested(const QString& text, bool primarySelection);
    void notificationRequested(const QString& title, const QString& body);
    void pointerShapeRequested(const QString& shape);
    /// Program status records changed; read them with status().
    void statusChanged();
    /// OSC 555: flash the pane.
    void flashRequested();
    /// Shell integration (OSC 133).
    void commandBegan(const QString& command);
    void commandEnded(const QString& command, int exitCode, qint64 durationMs);
    /// An OSC 99 desktop notification request (queued to the GUI thread).
    void desktopNotificationRequested(const AfterglowVt::NotificationRequest& request);
    /// An OSC 5522 clipboard read or write (queued to the GUI thread).
    void clipboardRequested(const AfterglowVt::ClipboardRequest& request);
    void closed();

private:
    // ITerminalHost (pty thread, m_mutex held)
    void writeToPty(std::string_view bytes) override;
    void titleChanged(std::string_view title) override;
    void bell() override;
    void clipboardWrite(std::string_view selection, std::string_view data) override;
    void notify(std::string_view title, std::string_view body) override;
    void pointerShapeChanged(std::string_view shape) override;
    void programStatusChanged() override;
    void flash() override;
    void commandStarted(std::string_view command) override;
    void commandFinished(int exitCode) override;
    void desktopNotification(const AfterglowVt::NotificationRequest& request) override;
    std::string notificationCapabilities() override;
    bool clipboardRequest(const AfterglowVt::ClipboardRequest& request) override;
    bool decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height,
                   std::vector<std::uint8_t>& rgba) override;

    /// Advances image animations and arms the timer for the next frame.
    void tickAnimations();

    // IPtyListener (pty thread)
    void ptyData(std::string_view bytes) override;
    void ptyClosed(int status) override;

    mutable std::mutex m_mutex;
    const quint32 m_id;
    QString m_userTitle;
    QString m_command; ///< guarded by m_mutex
    std::chrono::steady_clock::time_point m_commandStart;
    AfterglowVt::Terminal m_terminal;
    QString m_title;

    AfterglowVt::Selection m_selection;
    bool m_selectionOnAltScreen = false;
    std::uint64_t m_selectionSerial = 0;

    std::vector<AfterglowVt::SearchMatch> m_searchMatches;
    std::u32string m_searchQuery;
    int m_searchCurrent = -1;
    bool m_searchActive = false;
    std::u32string m_preedit;
    int m_preeditCursor = 0;
    void applyPreedit();

    AfterglowRender::PaneSnapshot m_snapshot;
    std::uint64_t m_snapshotGeneration = ~0ull;
    std::uint64_t m_snapshotSelectionSerial = ~0ull;
    int m_snapshotExtraRows = 0;
    QElapsedTimer m_syncTimer;
    QElapsedTimer m_promptTimer;

    std::atomic<bool> m_notifyPending{false};
    QTimer m_animationTimer;
    // Declared last so it is destroyed first: its thread calls into the terminal.
    std::unique_ptr<AfterglowPty::Pty> m_pty;
};

} // namespace Afterglow

Q_DECLARE_METATYPE(AfterglowVt::NotificationRequest)
Q_DECLARE_METATYPE(AfterglowVt::ClipboardRequest)
