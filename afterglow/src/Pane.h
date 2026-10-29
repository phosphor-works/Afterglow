// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowPty/Pty.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Selection.h>
#include <AfterglowVt/Terminal.h>

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>

namespace Afterglow {

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
    bool refreshSnapshot();
    const AfterglowRender::PaneSnapshot& snapshot() const noexcept { return m_snapshot; }

    QString title() const;
    QString workingDirectory() const;

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
    void closed();

private:
    // ITerminalHost (pty thread, m_mutex held)
    void writeToPty(std::string_view bytes) override;
    void titleChanged(std::string_view title) override;
    void bell() override;
    void clipboardWrite(std::string_view selection, std::string_view data) override;
    bool decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height,
                   std::vector<std::uint8_t>& rgba) override;

    /// Advances image animations and arms the timer for the next frame.
    void tickAnimations();

    // IPtyListener (pty thread)
    void ptyData(std::string_view bytes) override;
    void ptyClosed(int status) override;

    mutable std::mutex m_mutex;
    AfterglowVt::Terminal m_terminal;
    QString m_title;

    AfterglowVt::Selection m_selection;
    bool m_selectionOnAltScreen = false;
    std::uint64_t m_selectionSerial = 0;

    AfterglowRender::PaneSnapshot m_snapshot;
    std::uint64_t m_snapshotGeneration = ~0ull;
    std::uint64_t m_snapshotSelectionSerial = ~0ull;
    QElapsedTimer m_syncTimer;
    QElapsedTimer m_promptTimer;

    std::atomic<bool> m_notifyPending{false};
    QTimer m_animationTimer;
    // Declared last so it is destroyed first: its thread calls into the terminal.
    std::unique_ptr<AfterglowPty::Pty> m_pty;
};

} // namespace Afterglow
