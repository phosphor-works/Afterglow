// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "desktop/TerminalText.h"

#include <AfterglowPty/Pty.h>
#include <AfterglowRender/GridRenderer.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Search.h>
#include <AfterglowVt/Selection.h>
#include <AfterglowVt/Terminal.h>

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <AfterglowVt/TextSearch.h>
#include <QTimer>
#include <QString>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

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

class FileTransfer;
class DragDrop;

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
    /// A refusal shows the input-rejected notice unless @p reportFailure is
    /// false (a caller that reports the failure itself).
    bool sendInput(std::string_view bytes, bool reportFailure = true);
    /// Writes a protocol reply (a clipboard, notification, focus, file
    /// transfer or drag and drop answer). It is not typing: the view stays
    /// where it is, and a refusal shows no notice. False when the pty refused it.
    bool sendReply(std::string_view bytes);
    /// Creates the protocol handlers on first use.
    FileTransfer* fileTransfers();
    DragDrop* dragDrop();
    bool hasFileTransfers() const noexcept { return m_fileTransfers != nullptr; }
    bool hasDragDrop() const noexcept { return m_dragDrop != nullptr; }

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
    QString userTitle() const;
    /// The directory the shell reported last (OSC 7, 9;9, 633, 1337) when it
    /// is local and exists, else the foreground job's directory from /proc.
    QString workingDirectory() const;
    QString text(AfterglowVt::Terminal::TextScope scope);
    QVariantMap readText(AfterglowVt::Terminal::TextScope scope, std::size_t limit);
    /// The command running now (OSC 133), empty at the prompt.
    QString runningCommand() const;
    /// The program closing the pane would end: the shell's foreground job, or
    /// the program the pane was opened for. Empty while a shell sits idle.
    QString foregroundJob() const;
    /// Names the program the pane was opened to run (afterglow -e), instead of a shell.
    const QStringList& launchCommand() const { return m_launchCommand; }
    const QString& launchDirectory() const { return m_launchDirectory; }
    void setLaunch(const QString& cwd, const QStringList& command) { m_launchDirectory = cwd; m_launchCommand = command; }
    void setLaunchedProgram(const QString& name) { m_launchedProgram = name; }
    /// Keeps the pane open after an unsuccessful exit, until a key is
    /// pressed (the fallback panes of a restored project).
    void setHoldOnFailure(bool hold) { m_holdOnFailure = hold; }
    /// Keeps the pane open whenever its program exits, until a key is
    /// pressed (afterglow --hold).
    void setHold(bool hold) { m_hold = hold; }
    /// No process could start: shows a notice and holds the pane open until
    /// a key is pressed.
    void holdStartFailure();
    bool isHeld() const { return m_held.load(); }
    /// Whether output and input jump a scrolled-back pane to the bottom.
    void setScrollPolicy(bool onOutput, bool onInput)
    {
        m_scrollOnOutput.store(onOutput);
        m_scrollOnInput = onInput;
    }

    /// Where the pane's shell runs when that is not here as you (OSC 3008,
    /// iTerm2 RemoteHost): "root", "deploy@prod-1", a container's name.
    /// Empty for a local shell as the local user.
    QString contextLabel() const;
    /// What kind of context the label is: elevated, remote, container, or empty.
    QString contextKind() const;

    /// Values for this pane's shaders (uSignal), set by scripts and the
    /// control interface. GUI thread.
    using Signals = decltype(AfterglowRender::FrameBuilder::PaneParams::signals);
    static constexpr int SignalCount = int(std::tuple_size_v<Signals>);
    void setSignal(int channel, float value);
    void setSignals(const QList<QPair<int, float>>& values, std::chrono::steady_clock::time_point time);
    const Signals& shaderSignals() const noexcept { return m_signals; }

    // Search (GUI thread).
    /// Runs or steps a search and scrolls the match into view. A new query
    /// selects the last match in the buffer; @p direction 0 with the same
    /// query stays on the current match, 1 goes to the next, -1 the previous.
    void search(const QString& query, int direction, bool regex = false);
    void clearSearch();
    /// Colours of search matches, the current match and the text in a match,
    /// packed RGBA (Colors.searchMatch and its neighbours).
    void setSearchColors(std::uint32_t match, std::uint32_t current, std::uint32_t text);

    /// Text being composed by an input method, drawn over the grid at the
    /// cursor until it is committed. @p cursor is its caret, in UTF-16 code units.
    void setPreedit(const QString& text, int cursor);

    /// Program status records, copied (GUI thread).
    QList<PaneStatusRecord> status() const;

    /// Scrolls to the previous (@p direction < 0) or next shell prompt.
    /// Returns false when there is none that way.
    bool jumpToPrompt(int direction);

    // Selection (GUI thread). Points are absolute rows (history included);
    // selectionPointAt turns a cell of the current viewport into one.
    AfterglowVt::SelectionPoint selectionPointAt(int col, int row) const;
    void startSelection(AfterglowVt::SelectionPoint anchor, AfterglowVt::SelectionMode mode, bool activeNow);
    void extendSelection(AfterglowVt::SelectionPoint head);
    void clearSelection();
    /// Selects all the history and the screen down to the cursor's row.
    void selectAll();
    void beginCopyMode();
    bool moveCopyCursor(int dx, int dy, int edge = 0);
    void toggleCopySelection(AfterglowVt::SelectionMode mode);
    void endCopyMode();
    QVariantMap historyPosition() const;
    void scrollHistory(double fraction);
    bool hasSelection() const;
    QString selectedText() const;
    QVariantMap readSelection(std::size_t limit) const;
    QVariantMap semanticRegions() const;
    QVariantMap readRegion(std::uint64_t id, std::size_t limit) const;
    TerminalText accessibleText(bool cursorRow = false) const;

Q_SIGNALS:
    /// The terminal changed. Emitted on the GUI thread, coalesced.
    void contentChanged();
    void searchStatusChanged(const QString& status);
    void titleUpdated(const QString& title);
    void contextUpdated();
    void userVarUpdated(const QString& name, const QString& value);
    void shaderSignalsChanged();
    void bellRang();
    void clipboardWriteRequested(const QString& text, bool primarySelection);
    void notificationRequested(const QString& title, const QString& body);
    void inputRejected();
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
    void terminalProtocolRequested(int code, const QByteArray& payload);
    void fileTransferPermissionRequested(const QString& token, bool writing, const QStringList& paths);
    void closed();

private:
    // ITerminalHost (pty thread, m_mutex held)
    void writeToPty(std::string_view bytes) override;
    void reportInputFailure();
    std::atomic_bool m_inputFailureQueued{false};
    /// Queues @p deliver to the GUI thread unless one is already queued.
    void queueOnce(std::atomic_bool& queued, std::function<void()> deliver);
    std::atomic_bool m_bellQueued{false};
    std::atomic_bool m_flashQueued{false};
    std::atomic_bool m_clipboardQueued{false};
    std::atomic_bool m_pointerShapeQueued{false};
    // Status and context also change on the GUI thread (a reset through
    // withTerminal): queued, so no receiver reads them under m_mutex.
    std::atomic_bool m_statusQueued{false};
    std::atomic_bool m_contextQueued{false};
    std::optional<QString> m_pendingClipboard; ///< guarded by m_mutex
    std::optional<QString> m_pendingPrimary; ///< guarded by m_mutex
    QString m_pendingPointerShape; ///< guarded by m_mutex
    void titleChanged(std::string_view title) override;
    void workingDirectoryChanged(std::string_view directory) override;
    void bell() override;
    void clipboardWrite(std::string_view selection, std::string_view data) override;
    void notify(std::string_view title, std::string_view body) override;
    void pointerShapeChanged(std::string_view shape) override;
    void terminalProtocol(int code, std::string_view payload) override;
    std::unique_ptr<FileTransfer> m_fileTransfers;
    std::unique_ptr<DragDrop> m_dragDrop;
    std::atomic<qsizetype> m_protocolBytes{0};
    std::atomic<quint64> m_protocolGeneration{0};
    std::atomic<bool> m_protocolBlocked{false};
    void programStatusChanged() override;
    void flash() override;
    void commandStarted(std::string_view command) override;
    void commandFinished(int exitCode) override;
    void contextChanged() override;
    void regionScrolled(const AfterglowVt::RegionScroll& scroll) override;
    void userVarChanged(std::string_view name, std::string_view value) override;
    void downloadFile(std::string_view name, std::string_view data) override;
    bool decodeImage(std::string_view data, std::uint32_t& width, std::uint32_t& height,
                     std::vector<std::uint8_t>& rgba) override;
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
    QString m_launchedProgram;
    QStringList m_launchCommand;
    QString m_launchDirectory;
    std::atomic<bool> m_scrollOnOutput{false}; ///< read on the pty thread
    std::atomic<bool> m_hold{false};
    std::atomic<bool> m_holdOnFailure{false};
    std::atomic<bool> m_held{false}; ///< exited and held open
    bool m_scrollOnInput = true;
    QString m_contextLabel;
    QString m_contextKind;
    QString m_reportedDirectory; ///< local, absolute; guarded by m_mutex
    QHash<QString, QString> m_userVars;
    Signals m_signals{};
    QString m_command; ///< guarded by m_mutex
    std::chrono::steady_clock::time_point m_commandStart;
    AfterglowVt::Terminal m_terminal;
    QString m_title;
    QString m_shownTitle; ///< the last title announced, guarded by m_mutex

    bool m_copyMode = false;
    bool m_copySelecting = false;
    AfterglowVt::SelectionPoint m_copyCursor;
    AfterglowVt::Selection m_selection;
    bool m_selectionOnAltScreen = false;
    std::uint64_t m_selectionSerial = 0;

    struct SearchJob {
        AfterglowVt::TextSnapshot text;
        std::atomic<bool> cancelled{false};
        std::uint64_t epoch = 0, nextRow = 0, lastRow = 0;
        bool alt = false;
    };
    std::shared_ptr<SearchJob> m_searchJob;
    void captureSearch(const std::shared_ptr<SearchJob>& job, std::u32string query, bool regex);
    bool m_searchRegex = false;
    std::uint64_t m_searchEpoch = 0;
    bool m_searchAlt = false;
    QString m_searchNotice;
    std::vector<AfterglowVt::SearchMatch> m_searchMatches;
    std::u32string m_searchQuery;
    int m_searchCurrent = -1;
    bool m_searchActive = false;
    struct SearchColors
    {
        std::uint32_t match = AfterglowRender::packRgba(0xe5, 0xc0, 0x7b);
        std::uint32_t current = AfterglowRender::packRgba(0xff, 0x8c, 0x42);
        std::uint32_t text = AfterglowRender::packRgba(0x10, 0x10, 0x10);
    } m_searchColors; ///< guarded by m_mutex
    std::u32string m_preedit;
    int m_preeditCursor = 0;
    void applyPreedit();

    AfterglowRender::PaneSnapshot m_snapshot;
    std::uint64_t m_snapshotGeneration = ~0ull;
    std::uint64_t m_snapshotSelectionSerial = ~0ull;
    int m_snapshotExtraRows = 0;
    QElapsedTimer m_syncTimer;
    std::uint64_t m_syncSerial = 0; ///< the synchronized update m_syncTimer times
    QElapsedTimer m_promptTimer;

    std::atomic<bool> m_notifyPending{false};
    QTimer m_animationTimer;
    // Declared last so it is destroyed first: its thread calls into the terminal.
    std::unique_ptr<AfterglowPty::Pty> m_pty;
};

} // namespace Afterglow

Q_DECLARE_METATYPE(AfterglowVt::NotificationRequest)
Q_DECLARE_METATYPE(AfterglowVt::ClipboardRequest)
