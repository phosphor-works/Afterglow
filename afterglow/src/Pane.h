// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "desktop/DragDrop.h"
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
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

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
class DockArea;
class PaneOverlay;

/// Applies a launch's own variables to @p options (`env` of a script or a
/// control call): a string sets the name, false unsets it. The launch wins
/// over what @p options had, and each name is set or unset once.
void applyLaunchEnvironment(AfterglowPty::SpawnOptions& options, const QVariantMap& environment);

/// One terminal: the emulator state, the child process on its pty, and the
/// snapshot the window renders from. The pty thread feeds the terminal under
/// m_mutex, and a captured standard output (an overlay's) under
/// m_outputMutex; everything else runs on the GUI thread.
class Pane : public QObject, private AfterglowVt::ITerminalHost, private AfterglowPty::IPtyListener
{
    Q_OBJECT

public:
    explicit Pane(const AfterglowVt::TerminalOptions& options, QObject* parent = nullptr);
    ~Pane() override;

    /// Sizes the terminal and starts the shell on a pty of that size.
    /// @p term is the TERM given to the shell. @p options moves into the
    /// pty: its standard input can be large.
    bool start(AfterglowPty::SpawnOptions options, const QString& term, int cols, int rows, int cellWidth,
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
    /// Terminal.dragAnswerWait, for the drag and drop handler now and later.
    void setDragAnswerWait(int ms);
    /// GUI thread: delivers the program's protocol commands that wait for the
    /// GUI thread, and no other events. With @p waitMs above 0 and nothing to
    /// deliver, first waits up to that long for the next one. Runs no event loop.
    void deliverProtocolCommands(int waitMs = 0);
    /// The program that runs over this pane (an overlay), or null. It
    /// takes the pane's place on screen and its input while it runs. The
    /// overlay sets and clears it.
    PaneOverlay* overlay() const noexcept { return m_overlay; }
    void setOverlay(PaneOverlay* overlay) noexcept { m_overlay = overlay; }
    /// For an overlay's own pane: the pane it runs over. It is no pane of
    /// its own for scripts and afterglow ctl, and its program gets the
    /// host's AFTERGLOW_PANE_ID. Set before start().
    Pane* overlayHost() const noexcept { return m_overlayHost; }
    void setOverlayHost(Pane* host) noexcept { m_overlayHost = host; }
    /// Keeps at most @p limit bytes of what the program writes to a
    /// captured standard output (SpawnOptions::captureOutput). Set before start().
    void setOutputLimit(std::size_t limit) { m_outputLimit = limit; }
    /// What the program wrote to its captured standard output, and whether
    /// it wrote more than the limit. Complete once closed() came.
    std::string takeOutput(bool* truncated);
    /// The program's exit status once closed() came: its exit code, 128
    /// plus the signal that ended it, or -1 when it is unknown.
    int exitCode() const noexcept { return m_exitCode.load(); }
    /// The areas that scripts dock at the pane's edges. The window lays them
    /// out and draws them. They go where the pane goes, and close with it.
    std::vector<std::unique_ptr<DockArea>>& docks() noexcept { return m_docks; }
    const std::vector<std::unique_ptr<DockArea>>& docks() const noexcept { return m_docks; }
    /// Changes when what assistive technology reads changes: the terminal's
    /// generation and the selection serial.
    std::pair<std::uint64_t, std::uint64_t> accessibleSerial() const;

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
    /// Hands the snapshot out with its identity, for the last frame of a
    /// pane that draws no more (an overlay that ended, fading out). The next
    /// refresh captures a new one.
    AfterglowRender::PaneSnapshot takeSnapshot();

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
    /// At most `limit` bytes of text. The caller bounds the limit: scripts
    /// to their max_bytes range, show_last_output to Terminal.lastOutputLimit.
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

    /// What the pane answers to `aglow run`'s query for the socket of its
    /// ssh session (OSC 24569). RemoteSockets sets it, and the pane passes
    /// it on to its overlay's program, or @p overlay when an ssh session
    /// runs in the overlay itself. GUI thread.
    void setRemoteSocket(const AfterglowProto::Remote::Answer& answer,
                         const std::optional<AfterglowProto::Remote::Answer>& overlay = std::nullopt);
    AfterglowProto::Remote::Answer remoteSocketAnswer() const;

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
    /// Shell integration (OSC 133). Like the notifications and clipboard
    /// requests below, emitted on the GUI thread (queueEvent).
    void commandBegan(const QString& command);
    void commandEnded(const QString& command, int exitCode, qint64 durationMs);
    /// An OSC 99 desktop notification request.
    void desktopNotificationRequested(const AfterglowVt::NotificationRequest& request);
    /// An OSC 5522 or OSC 52 clipboard read, or an OSC 5522 write.
    void clipboardRequested(const AfterglowVt::ClipboardRequest& request);
    /// An OSC 72 or OSC 5113 command, or a terminal reset (code 0).
    void terminalProtocolRequested(int code, const QByteArray& payload);
    /// Commands were dropped: the program wrote them faster than they were
    /// handled. @p lost tells what the dropped commands held for drag and drop.
    /// @p reset: they held a terminal reset, which ended every file transfer.
    void terminalProtocolLost(DragDrop::Lost lost, bool reset);
    /// @p paths holds at most the first 100 of @p total paths: see FileTransfer::permissionRequested.
    void fileTransferPermissionRequested(const QString& token, bool writing, const QStringList& paths, int total,
        bool othersRefused);
    void closed();

private:
    // ITerminalHost (pty thread, m_mutex held)
    void writeToPty(std::string_view bytes) override;
    void reportInputFailure();
    /// An inputRejected is queued or was emitted, and no input got through since.
    std::atomic_bool m_inputFailureReported{false};
    /// m_mutex held, on the pty thread or on the GUI thread (a reset through
    /// withTerminal): queues @p deliver for the GUI thread unless one of its
    /// kind (@p queued) waits already. It waits in the queue of queueEvent,
    /// behind the events queued before it. A delivery that waits already
    /// keeps its place: what it delivers then is the latest state, which
    /// can be newer than events queued after it.
    void queueOnce(std::atomic_bool& queued, std::function<void()> deliver);
    /// pty thread, m_mutex held: queues @p event, which emits one signal, for
    /// the GUI thread, where the pane emits it to the receivers it has then
    /// (a moved pane's new window). @p bytes is about what the event holds:
    /// the memory of its fields that a program sizes (text, data), not of
    /// the small fixed ones. False, and nothing queued, when MaxQueuedEvents
    /// wait already, or when the waiting ones would hold more than
    /// MaxQueuedEventBytes with it: a program that writes them faster than
    /// the window takes them loses the rest. One event alone is never
    /// refused for its size.
    bool queueEvent(std::function<void()> event, std::size_t bytes = 0);
    /// Posts one deliverEvents() unless one is posted already.
    void postEvents();
    /// GUI thread: emits the queued events in order.
    void deliverEvents();
    static constexpr std::size_t MaxQueuedEvents = 256;
    /// As much as one clipboard write may carry.
    static constexpr std::size_t MaxQueuedEventBytes = AfterglowVt::ClipboardAssembler::MaxWrite;
    std::deque<std::function<void()>> m_events; ///< guarded by m_mutex
    std::size_t m_queuedEvents = 0; ///< the queueEvent ones in m_events, guarded by m_mutex
    std::size_t m_queuedEventBytes = 0; ///< guarded by m_mutex
    std::size_t m_droppedEvents = 0; ///< guarded by m_mutex
    std::atomic_bool m_eventsPosted{false};
    /// GUI thread: titleUpdated when the title to show changed.
    void announceTitle();
    std::atomic_bool m_bellQueued{false};
    std::atomic_bool m_titleQueued{false};
    std::atomic_bool m_userVarsQueued{false};
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
    int m_dragAnswerWait = DragDrop::DefaultAnswerWaitMs;
    std::vector<std::unique_ptr<DockArea>> m_docks;
    std::atomic<qsizetype> m_protocolBytes{0};
    std::atomic<quint64> m_protocolGeneration{0};
    // The commands dropped after an overflow form one gap in what the
    // program wrote: those that waited for the GUI thread (stale, from an
    // older generation), then those that came while the pane was blocked.
    bool m_protocolBlocked = false; ///< guarded by m_mutex
    DragDrop::Lost m_protocolLost = DragDrop::Lost::Nothing; ///< guarded by m_mutex: the blocked commands
    DragDrop::Lost m_protocolStale = DragDrop::Lost::Nothing; ///< GUI thread: the stale commands
    bool m_protocolLostReset = false; ///< guarded by m_mutex
    bool m_protocolStaleReset = false; ///< GUI thread
    /// Posts @p command to m_protocolCommands and wakes deliverProtocolCommands.
    /// The caller holds m_mutex.
    void postProtocolCommand(std::function<void()> command);
    /// GUI thread: writes reports that waited, and frees their budget.
    void writeReports(const std::string& reports);
    /// GUI thread: the file transfer worker wrote the replies of one command.
    void endAnswerHold();
    // The posted protocol commands that have not run, and the file transfer
    // commands whose replies are not written. While one waits, the terminal's
    // own reports wait behind it: a program reads a DA1 answer as the fence
    // after the replies to what it wrote before the query. Commands do not
    // wait: a drag event takes OSC 72 answers while a file transfer works.
    std::atomic<std::size_t> m_protocolQueued{0};
    // The reports that wait behind protocol commands. One posted closure
    // writes each batch. A command posted after a batch closes it, and the
    // next report starts a new one. The bytes count in m_protocolBytes.
    std::shared_ptr<std::string> m_openReports; ///< guarded by m_mutex
    // The file transfer commands taken and answered. Each parked batch waits
    // until the commands taken before it are answered.
    std::uint64_t m_answersBegun = 0; ///< GUI thread
    std::uint64_t m_answersEnded = 0; ///< GUI thread
    std::deque<std::pair<std::uint64_t, std::string>> m_parkedReports; ///< GUI thread
    // Counts the posted protocol commands, so that a drag event can wait for
    // the program's answer without an event loop.
    std::mutex m_postedMutex;
    std::condition_variable m_posted;
    std::uint64_t m_postedSerial = 0; ///< guarded by m_postedMutex
    std::uint64_t m_deliveredSerial = 0; ///< GUI thread: the serial that the last delivery read
    // The protocol commands are posted to this object, not to the pane, so a
    // drag event can deliver them, and only them, before it answers
    // (DragDrop::setCommandDelivery).
    QObject m_protocolCommands;
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
    std::optional<AfterglowProto::Remote::Answer> remoteSocket() override { return m_remoteSocket; }
    bool clipboardRequest(const AfterglowVt::ClipboardRequest& request) override;
    bool decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height,
                   std::vector<std::uint8_t>& rgba) override;

    /// Advances image animations and arms the timer for the next frame.
    /// True when a frame changed.
    bool advanceAnimations();
    /// advanceAnimations(), and a contentChanged() for a frame that changed.
    void tickAnimations();

    // IPtyListener (pty thread)
    void ptyData(std::string_view bytes) override;
    void ptyOutput(std::string_view bytes) override;
    void ptyClosed(int status) override;

    PaneOverlay* m_overlay = nullptr;
    Pane* m_overlayHost = nullptr;
    std::atomic<int> m_exitCode{-1};
    std::mutex m_outputMutex;
    std::string m_output; ///< guarded by m_outputMutex
    bool m_outputTruncated = false; ///< guarded by m_outputMutex
    std::size_t m_outputLimit = 0;

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
    static constexpr qsizetype MaxUserVars = 256;
    bool m_userVarsFull = false; ///< the warning about MaxUserVars was given; pty thread
    /// Changed user variables that wait for the GUI thread, guarded by m_mutex.
    std::vector<std::pair<QString, QString>> m_pendingUserVars;
    Signals m_signals{};
    QString m_command; ///< guarded by m_mutex
    AfterglowProto::Remote::Answer m_remoteSocket; ///< guarded by m_mutex
    std::chrono::steady_clock::time_point m_commandStart;
    AfterglowVt::Terminal m_terminal;
    QString m_title;
    QString m_shownTitle; ///< the title to show, guarded by m_mutex
    QString m_announcedTitle; ///< the last titleUpdated, GUI thread

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
    static constexpr std::uint64_t NoRow = std::numeric_limits<std::uint64_t>::max();
    /// The first row (a stable row id) that a region scroll moved since the
    /// search started, guarded by m_mutex. Matches from there on are stale.
    std::uint64_t m_movedFrom = NoRow;
    /// The last search lost matches to moved text: the next search with the
    /// same query starts again. GUI thread.
    bool m_searchStale = false;
    bool m_searchAlt = false;
    QString m_searchNotice;
    std::vector<AfterglowVt::SearchMatch> m_searchMatches;
    std::u32string m_searchQuery;
    int m_searchCurrent = -1;
    bool m_searchActive = false;
    /// Set by the window from the Colors.search* settings when it creates
    /// the pane (setSearchColors), so the defaults live in one place.
    struct SearchColors
    {
        std::uint32_t match = 0;
        std::uint32_t current = 0;
        std::uint32_t text = 0;
    } m_searchColors; ///< guarded by m_mutex
    std::u32string m_preedit;
    int m_preeditCursor = 0;
    void applyPreedit();

    AfterglowRender::PaneSnapshot m_snapshot;
    std::uint64_t m_snapshotGeneration = ~0ull;
    std::size_t m_animationViewportTop = 0;
    std::uint64_t m_snapshotSelectionSerial = ~0ull;
    int m_snapshotExtraRows = 0;
    QElapsedTimer m_syncTimer;
    std::uint64_t m_syncSerial = 0; ///< the synchronized update m_syncTimer times
    QElapsedTimer m_syncHoldTimer; ///< since synchronized updates began to hold the frames
    QElapsedTimer m_promptTimer;

    std::atomic<bool> m_notifyPending{false};
    QTimer m_animationTimer;
    // Declared last so it is destroyed first: its thread calls into the terminal.
    std::unique_ptr<AfterglowPty::Pty> m_pty;
};

} // namespace Afterglow

Q_DECLARE_METATYPE(AfterglowVt::NotificationRequest)
Q_DECLARE_METATYPE(AfterglowVt::ClipboardRequest)
