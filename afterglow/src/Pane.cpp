// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Pane.h"
#include "desktop/FileTransfer.h"
#include "desktop/DragDrop.h"
#include "desktop/Accessibility.h"
#include "DockArea.h"
#include "ImageDecode.h"
#include "PaneOverlay.h"
#include "ShownText.h"

#include "app/InstanceSocket.h"
#include "app/NotificationCenter.h"
#include "app/Startup.h"

#include <AfterglowProto/Graphics.h>
#include <AfterglowVt/Unicode.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDir>
#include <QWindow>
#include <QStringList>
#include <QTextBoundaryFinder>
#include <QSysInfo>

#include <algorithm>
#include <chrono>
#include <QLoggingCategory>

#include <utility>

#include <pwd.h>
#include <unistd.h>

#include <sys/wait.h>
#include <QFutureWatcher>
#include <QtConcurrentRun>

Q_LOGGING_CATEGORY(lcPane, "afterglow.pane", QtInfoMsg)

namespace Afterglow {

namespace {
constexpr int InitialCols = 80;
constexpr int InitialRows = 24;
/// How long synchronized output may hold a frame before we draw anyway.
constexpr qint64 SyncTimeoutMs = 1000;
/// How long updates that follow each other may hold frames together: a
/// program that is always inside one when a frame is due still shows.
constexpr qint64 SyncHoldLimitMs = 2 * SyncTimeoutMs;
/// How long to keep the last frame while the shell repaints a resized prompt.
constexpr qint64 PromptRedrawTimeoutMs = 150;
} // namespace

namespace {
std::atomic<quint32> s_nextPaneId{1};
}

Pane::Pane(const AfterglowVt::TerminalOptions& options, QObject* parent)
    : QObject(parent)
    , m_id(s_nextPaneId++)
    , m_terminal(InitialCols, InitialRows, *this, options)
{
    qRegisterMetaType<AfterglowVt::NotificationRequest>();
    qRegisterMetaType<AfterglowVt::ClipboardRequest>();
    m_animationTimer.setSingleShot(true);
    m_animationTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_animationTimer, &QTimer::timeout, this, &Pane::tickAnimations);
}

Pane::~Pane()
{
    // Its overlay answers "cancelled" while this pane is whole, and its
    // program gets SIGHUP.
    delete m_overlay;
    if (m_searchJob) m_searchJob->cancelled = true;
    // Stop the pty thread before the terminal it feeds goes away.
    if (m_pty) m_pty->stop();
    m_dragDrop.reset();
    m_fileTransfers.reset();
    m_pty.reset();
    for (const auto& dock : m_docks) releaseDockAccessibility(dock.get());
}

void applyLaunchEnvironment(AfterglowPty::SpawnOptions& options, const QVariantMap& environment)
{
    for (auto it = environment.cbegin(); it != environment.cend(); ++it) {
        const std::string name = it.key().toStdString();
        std::erase_if(options.environment, [&name](const auto& entry) { return entry.first == name; });
        std::erase(options.unsetEnvironment, name);
        if (it.value().metaType().id() == QMetaType::Bool) options.unsetEnvironment.push_back(name);
        else options.environment.emplace_back(name, it.value().toString().toStdString());
    }
}

bool Pane::start(AfterglowPty::SpawnOptions opts, const QString& term, int cols, int rows, int cellWidth,
                 int cellHeight, QString* error)
{
    // Size the terminal before the shell starts: programs run from the shell's
    // startup (fastfetch drawing its logo with kitty graphics) query the cell
    // size immediately, and a zero answer makes them fall back to text.
    {
        std::lock_guard lock(m_mutex);
        m_terminal.resize(cols, rows, cellWidth, cellHeight);
    }
    // The configured and launch values win over afterglow's own, and a name
    // they unset stays unset; each name appears once.
    const auto named = [&opts](const std::string& name) {
        return std::any_of(opts.environment.begin(), opts.environment.end(), [&](const auto& kv) { return kv.first == name; })
            || std::find(opts.unsetEnvironment.begin(), opts.unsetEnvironment.end(), name) != opts.unsetEnvironment.end();
    };
    const auto offer = [&](const std::string& name, const std::string& value) {
        if (!named(name)) opts.environment.emplace_back(name, value);
    };
    // Lets `afterglow ctl` run in the pane act on that pane by default: always ours.
    std::erase_if(opts.environment, [](const auto& kv) { return kv.first == "AFTERGLOW_PANE_ID"; });
    std::erase(opts.unsetEnvironment, std::string("AFTERGLOW_PANE_ID"));
    // An overlay's program acts on the pane it runs over.
    opts.environment.emplace_back("AFTERGLOW_PANE_ID", std::to_string(m_overlayHost ? m_overlayHost->id() : m_id));
    // Marks the overlay's own terminal: an `afterglow ssh` there gets an
    // OSC 24569 answer of its own, apart from the pane's (RemoteSockets).
    std::erase_if(opts.environment, [](const auto& kv) { return kv.first == "AFTERGLOW_OVERLAY"; });
    std::erase(opts.unsetEnvironment, std::string("AFTERGLOW_OVERLAY"));
    if (m_overlayHost) opts.environment.emplace_back("AFTERGLOW_OVERLAY", "1");
    else opts.unsetEnvironment.emplace_back("AFTERGLOW_OVERLAY");
    // The pane ID means something only to this process: `afterglow ctl` in a
    // second instance's pane must reach that instance, not the first.
    std::erase_if(opts.environment, [](const auto& kv) { return kv.first == "AFTERGLOW_SOCKET"; });
    std::erase(opts.unsetEnvironment, std::string("AFTERGLOW_SOCKET"));
    if (const QString socket = InstanceSocket::processPath(); !socket.isEmpty()) {
        opts.environment.emplace_back("AFTERGLOW_SOCKET", QFile::encodeName(socket).toStdString());
    } else {
        opts.unsetEnvironment.emplace_back("AFTERGLOW_SOCKET");
    }
    opts.cols = cols;
    opts.rows = rows;
    opts.pixelWidth = cols * cellWidth;
    opts.pixelHeight = rows * cellHeight;
    offer("TERM", term.toStdString());
    if (term == QLatin1String("afterglow")) {
        // Our own entry: point ncurses at it, ahead of the system databases
        // (the trailing ':' keeps the defaults).
        QStringList dirs;
        for (const char* d : {AFTERGLOW_BUILD_TERMINFO_DIR, AFTERGLOW_INSTALLED_TERMINFO_DIR}) {
            // The build tree's entry is for the build's own binary.
            if (d == std::string_view(AFTERGLOW_BUILD_TERMINFO_DIR) && !runningFromBuildTree()) continue;
            if (QFile::exists(QString::fromUtf8(d) + QStringLiteral("/a/afterglow"))) {
                dirs << QString::fromUtf8(d);
            }
        }
        if (const QByteArray existing = qgetenv("TERMINFO_DIRS"); !existing.isEmpty()) {
            dirs << QString::fromLocal8Bit(existing);
        }
        offer("TERMINFO_DIRS", (dirs.join(QLatin1Char(':')) + QLatin1Char(':')).toStdString());
    }
    offer("COLORTERM", "truecolor");
    offer("TERM_PROGRAM", "afterglow");
    offer("TERM_PROGRAM_VERSION", AFTERGLOW_VERSION);
    std::string err;
    opts.deferStart = true;
    if (m_pty) m_pty->stop();
    m_pty = AfterglowPty::Pty::spawn(std::move(opts), *this, &err);
    if (m_pty && !m_pty->start(&err)) {
        m_pty->stop();
        m_pty.reset();
    }
    if (!m_pty && error) {
        *error = QString::fromStdString(err);
    }
    return m_pty != nullptr;
}

void Pane::setOptions(const AfterglowVt::TerminalOptions& options)
{
    std::lock_guard lock(m_mutex);
    m_terminal.setOptions(options);
}

void Pane::resize(int cols, int rows, int cellWidth, int cellHeight)
{
    {
        std::lock_guard lock(m_mutex);
        if (cols == m_terminal.cols() && rows == m_terminal.rows() && cellWidth == m_terminal.cellWidth()
            && cellHeight == m_terminal.cellHeight()) {
            return;
        }
        m_terminal.resize(cols, rows, cellWidth, cellHeight);
    }
    if (m_pty) {
        m_pty->resize(cols, rows, cols * cellWidth, rows * cellHeight);
    }
}

int Pane::cols() const
{
    std::lock_guard lock(m_mutex);
    return m_terminal.cols();
}

int Pane::rows() const
{
    std::lock_guard lock(m_mutex);
    return m_terminal.rows();
}

bool Pane::sendInput(std::string_view bytes, bool reportFailure)
{
    if (bytes.empty() || !m_pty || m_held.load()) {
        return bytes.empty();
    }
    if (m_scrollOnInput) {
        std::lock_guard lock(m_mutex);
        if (m_terminal.screen().viewportOffset() != 0) {
            m_terminal.resetViewport();
            // The snap shows now, not with the next output: input a program
            // does not echo draws nothing else.
            QMetaObject::invokeMethod(this, [this] { Q_EMIT contentChanged(); }, Qt::QueuedConnection);
        }
    }
    const bool accepted = m_pty->write(bytes);
    if (accepted) m_inputFailureReported.store(false);
    else if (reportFailure) reportInputFailure();
    return accepted;
}

bool Pane::refreshSnapshot(int extraRows)
{
    std::lock_guard lock(m_mutex);
    if (m_terminal.modes().synchronizedOutput && m_snapshotGeneration != ~0ull) {
        // Each update a program begins gets its own timeout: one that ends
        // and the next that begins between two frames must not inherit it.
        if (m_terminal.synchronizedOutputSerial() != m_syncSerial) {
            m_syncSerial = m_terminal.synchronizedOutputSerial();
            m_syncTimer.start();
        }
        if (!m_syncTimer.isValid()) {
            m_syncTimer.start();
        }
        if (!m_syncHoldTimer.isValid()) {
            m_syncHoldTimer.start();
        }
        if (m_syncTimer.elapsed() < SyncTimeoutMs && m_syncHoldTimer.elapsed() < SyncHoldLimitMs) {
            return false;
        }
    } else {
        m_syncTimer.invalidate();
    }
    // A frame is taken below: the next hold starts its own limit.
    m_syncHoldTimer.invalidate();
    if (m_terminal.promptRedrawPending() && m_snapshotGeneration != ~0ull) {
        // The timer starts with the first resize of a drag and is not
        // restarted by later ones, so a continuous drag still updates.
        if (!m_promptTimer.isValid()) {
            m_promptTimer.start();
        }
        if (m_promptTimer.elapsed() < PromptRedrawTimeoutMs) {
            return false;
        }
    } else {
        m_promptTimer.invalidate();
    }
    // Animations run only for images the viewport draws, and only PTY output
    // ticks them, so a viewport that moves must tick them again. Queued: the
    // tick takes m_mutex.
    if (const std::size_t top = m_terminal.screen().viewportTop(); top != m_animationViewportTop) {
        m_animationViewportTop = top;
        QMetaObject::invokeMethod(this, &Pane::tickAnimations, Qt::QueuedConnection);
    }
    if (m_terminal.generation() != m_snapshotGeneration || m_selectionSerial != m_snapshotSelectionSerial
        || extraRows != m_snapshotExtraRows) {
        const bool selectionHere = m_selection.active && m_selectionOnAltScreen == m_terminal.modes().altScreen;
        AfterglowRender::SearchHighlight highlight;
        if (m_searchActive && m_searchEpoch == m_terminal.screen().layoutEpoch() && m_searchAlt == m_terminal.modes().altScreen) {
            highlight.matches = &m_searchMatches;
            highlight.current = m_searchCurrent;
            highlight.matchColor = m_searchColors.match;
            highlight.currentColor = m_searchColors.current;
            highlight.textColor = m_searchColors.text;
        }
        m_snapshot.capture(m_terminal, selectionHere ? &m_selection : nullptr, highlight, extraRows, false);
        applyPreedit();
        m_snapshot.applyBidi(m_terminal);
        m_snapshotSelectionSerial = m_selectionSerial;
        m_snapshotExtraRows = extraRows;
        qCDebug(lcPane) << "snapshot generation" << m_terminal.generation() << "images" << m_snapshot.images.size();
        m_snapshotGeneration = m_terminal.generation();
    }
    return true;
}

AfterglowRender::PaneSnapshot Pane::takeSnapshot()
{
    std::lock_guard lock(m_mutex);
    m_snapshotGeneration = ~0ull;
    // The move keeps the identity; the snapshot left here takes a new one.
    return std::exchange(m_snapshot, {});
}

AfterglowVt::SelectionPoint Pane::selectionPointAt(int col, int row) const
{
    std::lock_guard lock(m_mutex);
    const AfterglowVt::Screen& scr = m_terminal.screen();
    return {scr.rowsDropped() + scr.viewportTop() + std::size_t(std::clamp(row, 0, scr.rows() - 1)),
            std::clamp(col, 0, scr.cols() - 1)};
}

void Pane::selectAll()
{
    AfterglowVt::SelectionPoint first;
    AfterglowVt::SelectionPoint last;
    {
        std::lock_guard lock(m_mutex);
        const AfterglowVt::Screen& scr = m_terminal.screen();
        first = {scr.rowsDropped(), 0};
        last = {scr.rowsDropped() + scr.historySize() + std::size_t(std::max(0, scr.cursor.y)), scr.cols() - 1};
    }
    startSelection(first, AfterglowVt::SelectionMode::Normal, true);
    extendSelection(last);
}

void Pane::startSelection(AfterglowVt::SelectionPoint anchor, AfterglowVt::SelectionMode mode, bool activeNow)
{
    std::lock_guard lock(m_mutex);
    m_selection.start(anchor, mode, m_terminal.screen().layoutEpoch(), activeNow);
    m_selectionOnAltScreen = m_terminal.modes().altScreen;
    ++m_selectionSerial;
}

void Pane::extendSelection(AfterglowVt::SelectionPoint head)
{
    std::lock_guard lock(m_mutex);
    if (head == m_selection.head && m_selection.active) {
        return;
    }
    m_selection.head = head;
    // A plain click becomes a selection once the pointer leaves the first cell.
    if (!m_selection.active && head != m_selection.anchor) {
        m_selection.active = true;
    }
    ++m_selectionSerial;
}

void Pane::clearSelection()
{
    std::lock_guard lock(m_mutex);
    if (m_selection.active) {
        m_selection.active = false;
        ++m_selectionSerial;
    }
}

bool Pane::hasSelection() const
{
    std::lock_guard lock(m_mutex);
    return m_selection.active && m_selectionOnAltScreen == m_terminal.modes().altScreen
        && m_selection.validFor(m_terminal.screen());
}

QString Pane::selectedText() const
{
    std::string text;
    {
        std::lock_guard lock(m_mutex);
        if (!m_selection.active || m_selectionOnAltScreen != m_terminal.modes().altScreen) {
            return {};
        }
        text = m_selection.text(m_terminal.screen(), m_terminal.options().wordBoundaries);
    }
    // The reader thread waits on the lock, and a selection can be megabytes.
    return QString::fromStdString(text);
}

namespace {
/// Scrolls so absolute row @p abs is shown, near the middle when it was off screen.
void revealRow(AfterglowVt::Terminal& t, std::size_t abs)
{
    const AfterglowVt::Screen& scr = t.screen();
    const std::size_t top = scr.viewportTop();
    if (abs >= top && abs < top + std::size_t(scr.rows())) {
        return;
    }
    const long desiredTop = std::max(0L, long(abs) - scr.rows() / 2);
    t.scrollViewport(long(top) - desiredTop);
}
} // namespace

void Pane::beginCopyMode()
{
    std::lock_guard lock(m_mutex);
    const auto& screen = m_terminal.screen();
    m_copyCursor = {screen.rowsDropped() + (screen.viewportOffset() ? screen.viewportTop() : screen.historySize() + std::size_t(screen.cursor.y)),
                    screen.viewportOffset() ? 0 : screen.cursor.x};
    m_copyCursor = AfterglowVt::moveSelectionCursor(screen, m_copyCursor, 0, 0);
    m_copyMode = true; m_copySelecting = false;
    m_selection = {};
    m_selection.anchor = m_selection.head = m_copyCursor;
    m_selection.layoutEpoch = screen.layoutEpoch();
    m_selection.active = true;
    m_selectionOnAltScreen = m_terminal.modes().altScreen;
    ++m_selectionSerial;
}
bool Pane::moveCopyCursor(int dx, int dy, int edge)
{
    std::lock_guard lock(m_mutex);
    const auto& screen = m_terminal.screen();
    if (!m_copyMode || !m_selection.validFor(screen) || m_selectionOnAltScreen != m_terminal.modes().altScreen) return false;
    if (edge == 1) m_copyCursor.col = 0;
    if (edge == 2) m_copyCursor.col = screen.cols() - 1;
    if (edge == 3) m_copyCursor = {screen.rowsDropped(), 0};
    if (edge == 4) m_copyCursor = {screen.rowsDropped() + screen.totalRows() - 1, 0};
    m_copyCursor = AfterglowVt::moveSelectionCursor(screen, m_copyCursor, dx, dy);
    if (!m_copySelecting) m_selection.anchor = m_copyCursor;
    m_selection.head = m_copyCursor;
    revealRow(m_terminal, std::size_t(m_copyCursor.row - screen.rowsDropped()));
    ++m_selectionSerial;
    return true;
}
void Pane::toggleCopySelection(AfterglowVt::SelectionMode mode)
{
    std::lock_guard lock(m_mutex);
    m_copySelecting = !m_copySelecting || m_selection.mode != mode;
    m_selection.start(m_copyCursor, mode, m_selection.layoutEpoch, m_selection.active);
    ++m_selectionSerial;
}
void Pane::endCopyMode()
{
    std::lock_guard lock(m_mutex);
    m_copyMode = false; m_copySelecting = false; m_selection.active = false;
    ++m_selectionSerial;
}
QVariantMap Pane::historyPosition() const
{
    std::lock_guard lock(m_mutex);
    const auto& screen = m_terminal.screen();
    return {{QStringLiteral("position"), double(screen.viewportTop()) / double(screen.totalRows())},
            {QStringLiteral("size"), double(screen.rows()) / double(screen.totalRows())}};
}
void Pane::scrollHistory(double fraction)
{
    std::lock_guard lock(m_mutex);
    const auto& screen = m_terminal.screen();
    const long top = long(std::clamp(fraction, 0.0, 1.0) * double(screen.totalRows()));
    m_terminal.scrollViewport(long(screen.viewportTop()) - top);
}

namespace {
/// The vt library's search errors, in the user's language.
QString searchError(const AfterglowVt::TextSearchResult& result)
{
    using AfterglowVt::SearchError;
    switch (result.errorKind) {
    case SearchError::None:
        return {};
    case SearchError::PatternTooLong:
        return Pane::tr("Pattern exceeds 4096 characters");
    case SearchError::NoMemory:
        return Pane::tr("Not enough memory to search");
    case SearchError::TimeLimit:
        return Pane::tr("Search time limit reached");
    case SearchError::ComplexityLimit:
        return Pane::tr("Search complexity limit reached");
    case SearchError::InvalidPattern:
        return Pane::tr("Invalid regular expression at character %1").arg(result.errorPosition);
    }
    return QString::fromStdString(result.error);
}
} // namespace

void Pane::captureSearch(const std::shared_ptr<SearchJob>& job, std::u32string query, bool regex)
{
    if (job != m_searchJob || job->cancelled) return;
    bool more = false;
    {
        std::lock_guard lock(m_mutex);
        const auto& screen = m_terminal.screen();
        if (screen.layoutEpoch() != job->epoch || m_terminal.modes().altScreen != job->alt) {
            job->cancelled = true;
        } else {
            const auto begin = std::max(job->nextRow, screen.rowsDropped());
            const auto end = std::min({begin + 128, job->lastRow, screen.rowsDropped() + screen.totalRows()});
            for (auto row = begin; row < end; ++row) {
                if (!AfterglowVt::appendTextRow(job->text, screen, std::size_t(row - screen.rowsDropped()))) break;
            }
            job->nextRow = end;
            more = end < std::min(job->lastRow, screen.rowsDropped() + screen.totalRows()) && !job->text.truncated;
        }
    }
    if (job->cancelled) {
        m_searchJob.reset();
        // The screen can be back as it was by the next search: that one
        // starts again rather than show the empty result of this one.
        m_searchStale = true;
        Q_EMIT searchStatusChanged(tr("Layout changed. Search again"));
        return;
    }
    if (more) {
        QTimer::singleShot(0, this, [this, job, query = std::move(query), regex] { captureSearch(job, query, regex); });
        return;
    }
    auto* watcher = new QFutureWatcher<AfterglowVt::TextSearchResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, job] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (job != m_searchJob || result.cancelled) return;
        m_searchJob.reset();
        bool scrolled = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_terminal.screen().layoutEpoch() != job->epoch || m_terminal.modes().altScreen != job->alt) {
                // search() below sees the change and starts again.
                result.matches.clear();
            }
            // Text moved inside a scroll region while the search ran: from
            // that row on, a match can name a row that holds other text now,
            // and rows can be missing or found twice. The rows above stand.
            // A program that keeps scrolling would restart a search forever,
            // so the next search starts again instead.
            const std::size_t found = result.matches.size();
            std::erase_if(result.matches, [this](const auto& match) { return match.end.row >= m_movedFrom; });
            scrolled = result.matches.size() != found || m_movedFrom != NoRow;
            std::erase_if(result.matches, [this](const auto& match) { return m_terminal.screen().absoluteIndexOf(match.start.row) < 0; });
            m_searchMatches = std::move(result.matches);
            m_searchCurrent = int(m_searchMatches.size()) - 1;
            m_searchNotice = scrolled ? tr("Text moved during the search. Search again") : searchError(result);
            if (result.truncated) m_searchNotice += tr(" (limited results)");
            if (m_searchCurrent >= 0) revealRow(m_terminal, std::size_t(m_terminal.screen().absoluteIndexOf(m_searchMatches.back().start.row)));
            ++m_selectionSerial;
        }
        search(QString::fromStdU32String(m_searchQuery), 0, m_searchRegex);
        // A search that started again just now (the layout changed) takes
        // the moved text fresh: only a result that stays is stale.
        m_searchStale = scrolled && !m_searchJob;
        Q_EMIT contentChanged();
    });
    watcher->setFuture(QtConcurrent::run([job, query = std::move(query), regex] {
        return AfterglowVt::searchText(job->text, query, regex, job->cancelled);
    }));
}

void Pane::search(const QString& query, int direction, bool regex)
{
    const auto needle = query.toStdU32String();
    bool layoutChanged = false;
    {
        std::lock_guard lock(m_mutex);
        layoutChanged = m_searchEpoch != m_terminal.screen().layoutEpoch() || m_searchAlt != m_terminal.modes().altScreen;
    }
    if (layoutChanged || m_searchStale || needle != m_searchQuery || regex != m_searchRegex || !m_searchActive) {
        clearSearch();
        m_searchStale = false;
        m_searchActive = true; m_searchRegex = regex; m_searchQuery = needle;
        m_searchNotice.clear();
        auto job = std::make_shared<SearchJob>();
        {
            std::lock_guard lock(m_mutex);
            const auto& screen = m_terminal.screen();
            job->epoch = m_searchEpoch = screen.layoutEpoch(); job->alt = m_searchAlt = m_terminal.modes().altScreen;
            m_movedFrom = NoRow;
            job->lastRow = screen.rowsDropped() + screen.totalRows();
            job->nextRow = screen.rowsDropped();
        }
        m_searchJob = job;
        Q_EMIT searchStatusChanged(tr("Searching…"));
        captureSearch(job, needle, regex);
        return;
    }
    if (m_searchJob) return;
    QString status;
    {
        std::lock_guard lock(m_mutex);
        if (!m_searchMatches.empty()) {
            const int count = int(m_searchMatches.size());
            m_searchCurrent = (m_searchCurrent + direction + count) % count;
            const auto row = m_terminal.screen().absoluteIndexOf(m_searchMatches[std::size_t(m_searchCurrent)].start.row);
            if (row >= 0) revealRow(m_terminal, std::size_t(row));
            status = tr("%1 of %2").arg(m_searchCurrent + 1).arg(count);
        } else status = tr("No matches");
        ++m_selectionSerial;
    }
    Q_EMIT searchStatusChanged(m_searchNotice.isEmpty() ? status : status + QStringLiteral(" · ") + m_searchNotice);
}

void Pane::clearSearch()
{
    if (m_searchJob) { m_searchJob->cancelled = true; m_searchJob.reset(); }
    std::lock_guard lock(m_mutex);
    if (!m_searchActive) {
        return;
    }
    m_searchActive = false;
    m_searchMatches.clear();
    m_searchQuery.clear();
    m_searchCurrent = -1;
    ++m_selectionSerial;
}

void Pane::setPreedit(const QString& text, int cursor)
{
    std::lock_guard lock(m_mutex);
    const std::u32string preedit = text.toStdU32String();
    cursor = int(text.left(std::clamp(cursor, 0, int(text.size()))).toStdU32String().size());
    if (preedit == m_preedit && cursor == m_preeditCursor) {
        return;
    }
    m_preedit = preedit;
    m_preeditCursor = cursor;
    ++m_selectionSerial; // recapture with the new overlay
}

void Pane::applyPreedit()
{
    // As foot does: the composition replaces the cells at the cursor,
    // underlined, shifted left if it would run past the edge, with the
    // cursor drawn as a bar at the input method's caret.
    AfterglowRender::PaneSnapshot& s = m_snapshot;
    if (m_preedit.empty() || !s.cursorVisible || s.cursorY >= s.rows) {
        return;
    }
    std::vector<std::pair<std::u32string, int>> cells;
    int total = 0;
    int caretCol = -1;
    const QString text = QString::fromStdU32String(m_preedit);
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, text);
    int startOffset = 0, codepointOffset = 0;
    for (int end = boundaries.toNextBoundary(); end >= 0; end = boundaries.toNextBoundary()) {
        auto cluster = text.mid(startOffset, end - startOffset).toStdU32String();
        int width = 0;
        for (const char32_t cp : cluster) width = std::max(width, m_terminal.charWidth(AfterglowVt::Unicode::props(cp)));
        width = std::clamp(width, 1, 2);
        if (m_preeditCursor >= codepointOffset && m_preeditCursor < codepointOffset + int(cluster.size())) caretCol = total;
        codepointOffset += int(cluster.size());
        cells.emplace_back(std::move(cluster), width);
        total += width;
        startOffset = end;
    }
    if (caretCol < 0) {
        caretCol = total;
    }
    const int start = std::max(0, std::min(s.cursorX, s.cols - std::min(total + 1, s.cols)));
    const auto cellAt = [&s](int col) -> AfterglowRender::SnapshotCell& {
        return s.cells[std::size_t(s.cursorY) * std::size_t(s.cols) + std::size_t(col)];
    };
    // A wide character that the composition covers half of goes blank: its
    // other half would draw over the composition, or be a tail with no head.
    const auto blank = [](AfterglowRender::SnapshotCell& c) {
        c.cp = U' ';
        c.width = 1;
        c.grapheme = 0;
    };
    if (start > 0 && cellAt(start).width == 0) blank(cellAt(start - 1));
    int x = start;
    for (const auto& [cluster, w] : cells) {
        if (x + w > s.cols) {
            break;
        }
        AfterglowRender::SnapshotCell& c = s.cells[std::size_t(s.cursorY) * std::size_t(s.cols) + std::size_t(x)];
        c = {};
        c.logicalColumn = x;
        c.cp = cluster.front();
        c.width = std::uint8_t(w);
        c.fg = s.defaultFg;
        c.bg = s.defaultBg;
        c.underline = AfterglowVt::UnderlineStyle::Single;
        c.underlineColor = s.defaultFg;
        if (cluster.size() > 1) {
            // Graphemes hold the codepoints after cp, as captured cells do.
            s.graphemes.push_back(cluster.substr(1));
            c.grapheme = std::uint32_t(s.graphemes.size());
        }
        c.link = 0;
        if (w == 2 && x + 1 < s.cols) {
            AfterglowRender::SnapshotCell& tail = s.cells[std::size_t(s.cursorY) * std::size_t(s.cols) + std::size_t(x + 1)];
            tail = c;
            tail.cp = 0;
            tail.width = 0;
        }
        x += w;
    }
    if (x > start && x < s.cols && cellAt(x).width == 0) blank(cellAt(x));
    s.cursorX = std::min(start + caretCol, s.cols - 1);
    s.cursorShape = AfterglowVt::CursorShape::Bar;
}

bool Pane::jumpToPrompt(int direction)
{
    std::lock_guard lock(m_mutex);
    const AfterglowVt::Screen& scr = m_terminal.screen();
    const std::size_t top = scr.viewportTop();
    if (direction < 0) {
        for (std::size_t abs = top; abs-- > 0;) {
            if (scr.absoluteRow(abs).promptStart) {
                m_terminal.scrollViewport(long(top) - long(abs));
                return true;
            }
        }
        return false;
    }
    for (std::size_t abs = top + 1; abs < scr.totalRows(); ++abs) {
        if (scr.absoluteRow(abs).promptStart) {
            // Past the live screen's top is the live screen itself: from
            // there, a prompt on the live screen is no place to move to.
            const long target = std::min<long>(long(abs), long(scr.historySize()));
            if (target == long(top)) return false;
            m_terminal.scrollViewport(long(top) - target);
            return true;
        }
    }
    return false;
}

QString Pane::title() const
{
    std::lock_guard lock(m_mutex);
    return m_userTitle.isEmpty() ? m_title : m_userTitle;
}

QString Pane::userTitle() const
{
    std::lock_guard lock(m_mutex);
    return m_userTitle;
}

void Pane::setSignal(int channel, float value)
{
    if (channel < 0 || channel >= SignalCount) {
        return;
    }
    setSignals({{channel, value}}, std::chrono::steady_clock::now());
}

void Pane::setSignals(const QList<QPair<int, float>>& values, std::chrono::steady_clock::time_point time)
{
    for (const auto& [channel, value] : values) {
        if (channel >= 0 && channel < SignalCount) {
            m_signals[std::size_t(channel)] = {value, time};
        }
    }
    Q_EMIT shaderSignalsChanged();
}

void Pane::setSearchColors(std::uint32_t match, std::uint32_t current, std::uint32_t text)
{
    std::lock_guard lock(m_mutex);
    if (m_searchColors.match == match && m_searchColors.current == current && m_searchColors.text == text) return;
    m_searchColors = {match, current, text};
    ++m_selectionSerial; // the next snapshot draws the highlights again
}

void Pane::setUserTitle(const QString& title)
{
    {
        std::lock_guard lock(m_mutex);
        m_userTitle = title;
        m_shownTitle = m_userTitle.isEmpty() ? m_title : m_userTitle;
    }
    announceTitle();
}

void Pane::announceTitle()
{
    QString shown;
    {
        std::lock_guard lock(m_mutex);
        shown = m_shownTitle;
    }
    if (shown == m_announcedTitle) return;
    m_announcedTitle = shown;
    Q_EMIT titleUpdated(shown);
}

QString Pane::text(AfterglowVt::Terminal::TextScope scope)
{
    std::string text;
    {
        std::lock_guard lock(m_mutex);
        text = m_terminal.text(scope);
    }
    // Converted after the lock: the reader thread waits on it.
    return QString::fromStdString(text);
}

QVariantMap Pane::readText(AfterglowVt::Terminal::TextScope scope, std::size_t limit)
{
    std::string text;
    bool truncated = false;
    bool available = true;
    {
        std::lock_guard lock(m_mutex);
        text = m_terminal.text(scope, limit, &truncated);
        available = scope != AfterglowVt::Terminal::TextScope::LastOutput || m_terminal.hasCommandOutput();
    }
    // Converted after the lock: the text can be many megabytes, and the
    // reader thread waits on the lock.
    return {{QStringLiteral("text"), QString::fromStdString(text)}, {QStringLiteral("truncated"), truncated},
            {QStringLiteral("available"), available}};
}

QString Pane::runningCommand() const
{
    std::lock_guard lock(m_mutex);
    return m_command;
}

void Pane::commandStarted(std::string_view command)
{
    // pty thread, terminal locked
    m_command = QString::fromUtf8(command.data(), qsizetype(command.size()));
    m_commandStart = std::chrono::steady_clock::now();
    queueEvent([this, command = m_command] { Q_EMIT commandBegan(command); }, std::size_t(m_command.size()) * sizeof(QChar));
}

void Pane::commandFinished(int exitCode)
{
    const auto elapsed = std::chrono::steady_clock::now() - m_commandStart;
    const qint64 ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const QString command = std::exchange(m_command, QString());
    queueEvent([this, command, exitCode, ms] { Q_EMIT commandEnded(command, exitCode, ms); }, std::size_t(command.size()) * sizeof(QChar));
}

QString Pane::foregroundJob() const
{
    if (!m_pty || m_held.load()) {
        return {};
    }
    if (const std::string job = m_pty->foregroundProcess(); !job.empty()) {
        return QString::fromStdString(job);
    }
    return m_launchedProgram;
}

QString Pane::workingDirectory() const
{
    QString reported;
    {
        std::lock_guard lock(m_mutex);
        reported = m_reportedDirectory;
    }
    // A shell in a container can report a path that is not on this machine.
    if (!reported.isEmpty() && QFileInfo(reported).isDir()) return reported;
    return m_pty ? QFile::decodeName(QByteArray::fromStdString(m_pty->currentWorkingDirectory())) : QString();
}

void Pane::reportInputFailure()
{
    // One notice for each run of rejected input, not one for each key typed
    // at a program that stopped reading: the next comes only after some
    // input got through.
    if (m_inputFailureReported.exchange(true)) return;
    QMetaObject::invokeMethod(this, [this] { Q_EMIT inputRejected(); }, Qt::QueuedConnection);
}

namespace {
/// The bytes that the pane holds for the GUI thread: the protocol commands,
/// and the reports that wait behind them.
constexpr qsizetype ProtocolBudget = 16 * 1024 * 1024;
} // namespace

void Pane::writeToPty(std::string_view bytes)
{
    // m_mutex held, on the pty thread or the GUI thread (a resize). The
    // replies to protocol commands are written on the GUI thread when the
    // commands run, and for file transfer when the worker has handled them.
    // A report that the terminal answers at once must not pass the replies to
    // commands that the program wrote before it. What a command sends later
    // is not ordered: file data, an answer after a permission dialog, the
    // drag data that the pump timer sends after an "r" request. kitty sends
    // these later too, and the client waits for them anyway. The reply to an
    // OSC 72 command after a report can come before a report that waits for
    // file transfer. Each aglow command uses only one of the two protocols.
    if (m_protocolQueued.load() > 0) {
        const auto size = qsizetype(bytes.size());
        if (m_protocolBytes.fetch_add(size) + size > ProtocolBudget) {
            m_protocolBytes.fetch_sub(size);
            reportInputFailure();
            return;
        }
        // One closure for each run of reports, however many the program asks for.
        if (!m_openReports) {
            auto batch = std::make_shared<std::string>();
            postProtocolCommand([this, batch] {
                std::string reports;
                {
                    std::lock_guard lock(m_mutex);
                    reports = std::move(*batch);
                    if (m_openReports == batch) m_openReports.reset();
                }
                // The file transfer commands before this batch have begun
                // their holds, and no command after it has run.
                if (m_answersEnded != m_answersBegun) {
                    if (!m_parkedReports.empty() && m_parkedReports.back().first == m_answersBegun)
                        m_parkedReports.back().second += reports;
                    else
                        m_parkedReports.emplace_back(m_answersBegun, std::move(reports));
                    return;
                }
                writeReports(reports);
            });
            m_openReports = std::move(batch);
        }
        m_openReports->append(bytes);
        return;
    }
    if (!m_pty) return;
    if (m_pty->write(bytes)) m_inputFailureReported.store(false);
    else reportInputFailure();
}

void Pane::writeReports(const std::string& reports)
{
    m_protocolBytes.fetch_sub(qsizetype(reports.size()));
    if (m_pty && !m_pty->write(reports)) reportInputFailure();
}

bool Pane::sendReply(std::string_view bytes)
{
    if (bytes.empty()) return true;
    if (!m_pty || m_held.load()) return false;
    return m_pty->write(bytes);
}

void Pane::titleChanged(std::string_view title)
{
    // pty thread, terminal locked. Programs set the same title at every
    // prompt, and a loop can set thousands in a second: the GUI thread gets
    // the newest one.
    m_title = QString::fromUtf8(title.data(), qsizetype(title.size()));
    const QString shown = m_userTitle.isEmpty() ? m_title : m_userTitle;
    if (shown == m_shownTitle) return;
    m_shownTitle = shown;
    queueOnce(m_titleQueued, [this] { announceTitle(); });
}

void Pane::workingDirectoryChanged(std::string_view directory)
{
    // pty thread, terminal locked. OSC 7 sends file://host/path, percent
    // encoded; the others send a plain path. A directory on another host
    // leaves /proc to answer, which knows the local end of the session.
    QByteArray path;
    if (directory.starts_with("file://")) {
        const std::string_view rest = directory.substr(7);
        const std::size_t slash = rest.find('/');
        const QString host = QString::fromUtf8(rest.substr(0, slash == std::string_view::npos ? rest.size() : slash));
        static const QString here = QSysInfo::machineHostName();
        const bool local = host.isEmpty() || host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0
            || host.compare(here, Qt::CaseInsensitive) == 0;
        if (local && slash != std::string_view::npos) {
            const std::string_view encoded = rest.substr(slash);
            path = QByteArray::fromPercentEncoding(QByteArray(encoded.data(), qsizetype(encoded.size())));
        }
    } else if (directory.starts_with('/') && m_contextKind != QLatin1String("remote")) {
        path = QByteArray(directory.data(), qsizetype(directory.size()));
    }
    // Decoded, the path may hold a control byte that the URI hid.
    if (std::any_of(path.cbegin(), path.cend(), [](char c) { return (unsigned char)c < 0x20 || c == 0x7F; })) path.clear();
    m_reportedDirectory = QFile::decodeName(path);
}

void Pane::queueOnce(std::atomic_bool& queued, std::function<void()> deliver)
{
    // One delivery of each kind at a time, so a burst of output (a binary
    // file full of BEL bytes) cannot flood the GUI thread. These are few
    // (one for each kind), so the limits of queueEvent leave them alone.
    if (queued.exchange(true)) return;
    m_events.push_back([&queued, deliver = std::move(deliver)] {
        queued = false;
        deliver();
    });
    postEvents();
}

bool Pane::queueEvent(std::function<void()> event, std::size_t bytes)
{
    // Each of these events matters on its own, so they are not coalesced as
    // a title is: bounds keep a flood of them (a program that writes OSC 9
    // in a loop, or clipboard writes of 128 MiB each) from growing without end.
    if (m_queuedEvents >= MaxQueuedEvents || (m_queuedEventBytes > 0 && m_queuedEventBytes + bytes > MaxQueuedEventBytes)) {
        ++m_droppedEvents;
        return false;
    }
    ++m_queuedEvents;
    m_queuedEventBytes += bytes;
    m_events.push_back(std::move(event));
    postEvents();
    return true;
}

void Pane::postEvents()
{
    if (m_eventsPosted.exchange(true)) return;
    QMetaObject::invokeMethod(this, [this] { deliverEvents(); }, Qt::QueuedConnection);
}

void Pane::deliverEvents()
{
    std::deque<std::function<void()>> events;
    std::size_t dropped = 0;
    {
        std::lock_guard lock(m_mutex);
        // Cleared under the lock: an event queued after the swap posts
        // another delivery.
        m_eventsPosted = false;
        events.swap(m_events);
        m_queuedEvents = 0;
        m_queuedEventBytes = 0;
        dropped = std::exchange(m_droppedEvents, 0);
    }
    if (dropped > 0) qCWarning(lcPane) << "dropped" << dropped << "notifications, clipboard requests or command events that came faster than the window took them";
    for (const auto& event : events) event();
}

void Pane::setRemoteSocket(const AfterglowProto::Remote::Answer& answer, const std::optional<AfterglowProto::Remote::Answer>& overlay)
{
    {
        std::lock_guard lock(m_mutex);
        m_remoteSocket = answer;
    }
    // The overlay's program has the pane's AFTERGLOW_PANE_ID. Its terminal
    // answers for an ssh session that it runs itself, else as the pane does.
    if (m_overlay) m_overlay->pane()->setRemoteSocket(overlay.value_or(answer));
}

AfterglowProto::Remote::Answer Pane::remoteSocketAnswer() const
{
    std::lock_guard lock(m_mutex);
    return m_remoteSocket;
}

void Pane::bell()
{
    queueOnce(m_bellQueued, [this] { Q_EMIT bellRang(); });
}

void Pane::clipboardWrite(std::string_view selection, std::string_view data)
{
    // pty thread, terminal locked. A burst of writes sets the clipboard once,
    // to the last text, instead of once for each sequence.
    const QByteArray decoded = QByteArray::fromBase64(QByteArray(data.data(), qsizetype(data.size())));
    const QString text = QString::fromUtf8(decoded);
    // xterm targets: 'c' clipboard, 'p'/'s' primary selection. Empty means the clipboard.
    if (selection.empty() || selection.find('c') != std::string_view::npos) {
        m_pendingClipboard = text;
    }
    if (selection.find_first_of("ps") != std::string_view::npos) {
        m_pendingPrimary = text;
    }
    queueOnce(m_clipboardQueued, [this] {
        std::optional<QString> clipboard;
        std::optional<QString> primary;
        {
            std::lock_guard lock(m_mutex);
            clipboard.swap(m_pendingClipboard);
            primary.swap(m_pendingPrimary);
        }
        if (clipboard) Q_EMIT clipboardWriteRequested(*clipboard, false);
        if (primary) Q_EMIT clipboardWriteRequested(*primary, true);
    });
}

namespace {

QString localUser()
{
    // The reentrant lookup: this runs on a reader thread, and getpwuid's
    // shared buffer is the GUI thread's too (Pty::defaultShell).
    passwd entry{};
    passwd* found = nullptr;
    std::vector<char> buffer(16384);
    if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found) != 0 || !found) return {};
    return QString::fromLocal8Bit(found->pw_name);
}

} // namespace

bool Pane::decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height, std::vector<std::uint8_t>& rgba)
{
    return decodeBoundedImage(png, "PNG", width, height, rgba);
}

bool Pane::decodeImage(std::string_view data, std::uint32_t& width, std::uint32_t& height, std::vector<std::uint8_t>& rgba)
{
    // The common formats by their content; an animation shows its first frame.
    return decodeBoundedImage(data, "", width, height, rgba);
}

void Pane::contextChanged()
{
    // pty thread, terminal locked. The innermost context says who and where.
    static const QString hereHost = QSysInfo::machineHostName();
    static const QString hereUser = localUser();
    QString user;
    QString host;
    QString kind;
    for (const AfterglowVt::ShellContext& c : m_terminal.contexts().stack()) {
        auto str = [](const std::string& v) { return QString::fromStdString(v); };
        if (!c.user.empty()) {
            user = str(c.user);
        }
        if (!c.hostname.empty()) {
            host = str(c.hostname);
        }
        if (c.type == "elevate" || c.type == "chpriv") {
            user = c.targetUser.empty() ? QStringLiteral("root") : str(c.targetUser);
        } else if (c.type == "remote" && !c.targetHost.empty()) {
            host = str(c.targetHost);
            if (!c.targetUser.empty()) {
                user = str(c.targetUser);
            }
        } else if (c.type == "container" && !c.container.empty()) {
            host = str(c.container);
            kind = QStringLiteral("container");
        } else if (c.type == "vm" && !c.vm.empty()) {
            host = str(c.vm);
            kind = QStringLiteral("container");
        }
    }
    const bool otherUser = !user.isEmpty() && user != hereUser;
    const bool otherHost = !host.isEmpty() && host != hereHost;
    QString label;
    if (otherUser && otherHost) {
        label = user + QLatin1Char('@') + host;
    } else if (otherHost) {
        label = host;
    } else if (otherUser) {
        label = user;
    }
    if (kind.isEmpty() && !label.isEmpty()) {
        kind = otherHost ? QStringLiteral("remote") : QStringLiteral("elevated");
    }
    // A container named like this host has no label, and then no kind.
    if (label.isEmpty()) kind.clear();
    if (label != m_contextLabel || kind != m_contextKind) {
        m_contextLabel = label;
        m_contextKind = kind;
        queueOnce(m_contextQueued, [this] { Q_EMIT contextUpdated(); });
    }
}

void Pane::regionScrolled(const AfterglowVt::RegionScroll& scroll)
{
    // pty thread, terminal locked. What the pane keeps by stable row id on
    // the active screen moves with the rows; what leaves the region goes.
    const bool altScreen = m_terminal.modes().altScreen;
    bool changed = false;
    // A running search may have taken rows before the text moved between
    // them. The other screen's rows have ids of their own.
    if (altScreen == m_searchAlt) m_movedFrom = std::min(m_movedFrom, scroll.top);
    if (m_copyMode && m_selectionOnAltScreen == altScreen && scroll.contains(m_copyCursor)) {
        // A cursor whose row left stays at the edge the text went past.
        if (const auto moved = scroll.map(m_copyCursor)) m_copyCursor = *moved;
        else m_copyCursor.row = scroll.delta < 0 ? scroll.top : scroll.bottom;
        changed = true;
    }
    if (m_selection.active && m_selectionOnAltScreen == altScreen) {
        if (!m_selection.followScroll(scroll)) {
            m_copySelecting = false;
            if (m_copyMode) {
                // Copy mode keeps its cursor: the selection starts there again.
                m_selection.start(m_copyCursor, m_selection.mode, m_selection.layoutEpoch, true);
            } else {
                m_selection.active = false;
            }
        }
        changed = true;
    }
    if (!m_searchMatches.empty() && m_searchAlt == altScreen) {
        // Matches move with their text; a match that left the region, or
        // that a narrower region cut, is no longer on the screen as found.
        const std::size_t current = m_searchCurrent >= 0 ? std::size_t(m_searchCurrent) : m_searchMatches.size();
        std::vector<AfterglowVt::SearchMatch> kept;
        kept.reserve(m_searchMatches.size());
        int newCurrent = -1;
        // The last match before the current one that stays, in the new list.
        int keptBefore = -1;
        for (std::size_t i = 0; i < m_searchMatches.size(); ++i) {
            AfterglowVt::SearchMatch match = m_searchMatches[i];
            if (scroll.contains(match.start) || scroll.contains(match.end)) {
                const auto start = scroll.map(match.start);
                const auto end = scroll.map(match.end);
                if (!scroll.fullWidth || !start || !end || scroll.contains(match.start) != scroll.contains(match.end)) {
                    if (i == current) keptBefore = int(kept.size()) - 1;
                    continue;
                }
                match = {*start, *end};
                changed = true;
            }
            if (i == current) newCurrent = int(kept.size());
            kept.push_back(match);
        }
        if (kept.size() != m_searchMatches.size()) {
            changed = true;
            // The current match went: the nearest one before it that stays
            // is current, else the first one after it.
            if (newCurrent < 0 && m_searchCurrent >= 0 && !kept.empty()) newCurrent = std::max(0, keptBefore);
        }
        m_searchMatches = std::move(kept);
        m_searchCurrent = newCurrent;
    }
    if (changed) ++m_selectionSerial;
}

void Pane::userVarChanged(std::string_view name, std::string_view value)
{
    const QString n = QString::fromUtf8(name.data(), qsizetype(name.size()));
    const QString v = QString::fromUtf8(value.data(), qsizetype(value.size()));
    if (const auto it = m_userVars.constFind(n); it != m_userVars.constEnd() && *it == v) {
        return; // shells set the same values at every prompt
    }
    if (m_userVars.size() >= MaxUserVars && !m_userVars.contains(n)) {
        // Enough: the pane keeps its variables for its whole life. Said once,
        // so a program that wonders where its variable went can find out.
        if (!std::exchange(m_userVarsFull, true))
            qCWarning(lcPane) << "pane" << m_id << "has" << MaxUserVars << "user variables: new ones are ignored";
        return;
    }
    m_userVars.insert(n, v);
    // Only the newest value of each name reaches the GUI thread, in the order
    // of the last changes.
    std::erase_if(m_pendingUserVars, [&n](const auto& p) { return p.first == n; });
    m_pendingUserVars.emplace_back(n, v);
    queueOnce(m_userVarsQueued, [this] {
        std::vector<std::pair<QString, QString>> changed;
        {
            std::lock_guard lock(m_mutex);
            changed.swap(m_pendingUserVars);
        }
        for (const auto& [name, value] : changed) Q_EMIT userVarUpdated(name, value);
    });
}

QString Pane::contextLabel() const
{
    std::lock_guard lock(m_mutex);
    return m_contextLabel;
}

QString Pane::contextKind() const
{
    std::lock_guard lock(m_mutex);
    return m_contextKind;
}

bool Pane::advanceAnimations()
{
    using namespace std::chrono;
    const auto now = std::uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    std::uint64_t next = 0;
    bool changed = false;
    {
        std::lock_guard lock(m_mutex);
        const std::uint64_t before = m_terminal.generation();
        next = m_terminal.advanceAnimations(now);
        changed = m_terminal.generation() != before;
    }
    if (next != 0) {
        m_animationTimer.start(int(std::max<std::uint64_t>(1, next - std::min(next, now))));
    } else {
        m_animationTimer.stop();
    }
    return changed;
}

void Pane::tickAnimations()
{
    if (advanceAnimations()) {
        Q_EMIT contentChanged();
    }
}

void Pane::notify(std::string_view title, std::string_view body)
{
    QString heading = QString::fromUtf8(title.data(), qsizetype(title.size()));
    QString text = QString::fromUtf8(body.data(), qsizetype(body.size()));
    const std::size_t bytes = std::size_t(heading.size() + text.size()) * sizeof(QChar);
    queueEvent([this, heading = std::move(heading), text = std::move(text)] { Q_EMIT notificationRequested(heading, text); }, bytes);
}

void Pane::pointerShapeChanged(std::string_view shape)
{
    // pty thread, terminal locked: only the last shape of a burst matters.
    m_pendingPointerShape = QString::fromUtf8(shape.data(), qsizetype(shape.size()));
    queueOnce(m_pointerShapeQueued, [this] {
        QString shape;
        {
            std::lock_guard lock(m_mutex);
            shape = std::exchange(m_pendingPointerShape, QString());
        }
        Q_EMIT pointerShapeRequested(shape);
    });
}

void Pane::desktopNotification(const AfterglowVt::NotificationRequest& request)
{
    // The icon data is what can be large.
    queueEvent([this, request] { Q_EMIT desktopNotificationRequested(request); },
               request.title.size() + request.body.size() + request.iconData.size());
}

bool Pane::clipboardRequest(const AfterglowVt::ClipboardRequest& request)
{
    // A request that finds the queue full gets the answer of a terminal
    // with no clipboard support, as the program expects some answer.
    std::size_t bytes = 0;
    for (const auto& [mime, data] : request.data) bytes += mime.size() + data.size();
    return queueEvent([this, request] { Q_EMIT clipboardRequested(request); }, bytes);
}

std::string Pane::notificationCapabilities()
{
    return NotificationCenter::capabilities();
}

void Pane::flash()
{
    queueOnce(m_flashQueued, [this] { Q_EMIT flashRequested(); });
}

void Pane::programStatusChanged()
{
    // Called on the pty thread, or on the GUI thread by a reset through
    // withTerminal: either way m_mutex is held, and the receivers read status().
    queueOnce(m_statusQueued, [this] { Q_EMIT statusChanged(); });
}

QList<PaneStatusRecord> Pane::status() const
{
    std::lock_guard lock(m_mutex);
    const AfterglowVt::ProgramStatus& ps = m_terminal.programStatus();
    QList<PaneStatusRecord> out;
    out.reserve(qsizetype(ps.records().size()));
    auto str = [](std::string_view s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); };
    for (const AfterglowVt::StatusRecord& r : ps.records()) {
        out.append(PaneStatusRecord{str(r.id), r.state, r.kind, r.progress, str(ps.effectiveApp(r)), str(r.title),
                                    str(r.msg), r.serial});
    }
    return out;
}

void Pane::ptyData(std::string_view bytes)
{
    {
        std::lock_guard lock(m_mutex);
        m_terminal.feed(bytes);
        if (m_scrollOnOutput.load(std::memory_order_relaxed)) {
            m_terminal.resetViewport();
        }
    }
    // One queued notification at a time. The flag is cleared on the GUI thread
    // when the notification is delivered, so output arriving after that point
    // always produces another one, whether or not a frame was drawn.
    if (!m_notifyPending.exchange(true)) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                m_notifyPending.store(false);
                advanceAnimations();
                Q_EMIT contentChanged();
            },
            Qt::QueuedConnection);
    }
}

void Pane::ptyOutput(std::string_view bytes)
{
    std::lock_guard lock(m_outputMutex);
    const std::size_t room = m_outputLimit > m_output.size() ? m_outputLimit - m_output.size() : 0;
    // Read on to the end, so the program never blocks on a full pipe.
    if (bytes.size() > room) m_outputTruncated = true;
    m_output.append(bytes.substr(0, room));
}

std::string Pane::takeOutput(bool* truncated)
{
    std::lock_guard lock(m_outputMutex);
    if (truncated) *truncated = m_outputTruncated;
    return std::exchange(m_output, {});
}

void Pane::ptyClosed(int status)
{
    // -1 when the status is unknown, which is no failure to hold for.
    const int code = status == -1 ? -1 : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
    m_exitCode.store(code);
    if (status == -1) {
        qCInfo(lcPane) << "shell exit status unknown";
    } else if (WIFSIGNALED(status)) {
        qCWarning(lcPane) << "shell killed by signal" << WTERMSIG(status) << (WCOREDUMP(status) ? "(core dumped)" : "");
    } else {
        qCInfo(lcPane) << "shell exited with status" << WEXITSTATUS(status);
    }
    {
        std::lock_guard lock(m_mutex);
        m_terminal.programExited();
        if (m_hold.load() || (m_holdOnFailure.load() && code > 0)) {
            const std::string note = "\r\n\x1b[0;2m[exited with status " + std::to_string(std::max(code, 0))
                + ". Press any key to close]\x1b[0m\x1b[?25l";
            m_terminal.feed(note);
            m_held.store(true);
        }
    }
    if (m_held.load()) {
        QMetaObject::invokeMethod(this, [this] { Q_EMIT contentChanged(); }, Qt::QueuedConnection);
        return;
    }
    // On the GUI thread, after the events queued before it, and to the
    // window the pane is in by then.
    QMetaObject::invokeMethod(this, [this] { Q_EMIT closed(); }, Qt::QueuedConnection);
}

void Pane::holdStartFailure()
{
    {
        std::lock_guard lock(m_mutex);
        m_terminal.feed("\r\n[Unable to start a process. Check the project or the configuration. Press any key to close.]\r\n");
        m_held.store(true);
    }
    Q_EMIT contentChanged();
}

} // namespace Afterglow

namespace Afterglow {
TerminalText Pane::accessibleText(bool cursorRow) const
{
    std::lock_guard lock(m_mutex);
    return TerminalText::capture(m_terminal.screen(),
        m_selectionOnAltScreen == m_terminal.modes().altScreen ? &m_selection : nullptr, cursorRow, m_terminal.options().wordBoundaries);
}
}

namespace Afterglow {
void Pane::downloadFile(std::string_view name, std::string_view data)
{
    // Bound the entire application's pending payloads, including queued GUI work.
    static std::atomic_bool busy{false};
    static std::atomic_bool dropNoticed{false};
    if (busy.exchange(true)) {
        // Say so once while the dialog is open, not once for each file.
        if (!dropNoticed.exchange(true)) {
            QMetaObject::invokeMethod(this, [this] {
                NotificationCenter::instance().showNotice(this, tr("Download not saved"),
                    tr("A program sent a file while another file was waiting to be saved."));
            }, Qt::QueuedConnection);
        }
        return;
    }
    auto lease = std::shared_ptr<void>(nullptr, [](void*) {
        dropNoticed.store(false);
        busy.store(false);
    });
    const QByteArray bytes(data.data(), qsizetype(data.size()));
    QString suggested = QString::fromUtf8(name.data(), qsizetype(name.size()));
    suggested.replace(QLatin1Char('\\'), QLatin1Char('/'));
    suggested = suggested.section(QLatin1Char('/'), -1);
    QString clean;
    for (const QChar c : suggested) if (!c.isNull() && c.isPrint() && c.category() != QChar::Other_Format) clean += c;
    if (clean.isEmpty() || clean == QLatin1String(".") || clean == QLatin1String("..")) clean = QStringLiteral("download.bin");
    clean = clean.left(180);
    QMetaObject::invokeMethod(this, [this, bytes, clean, lease] {
        auto* dialog = new QFileDialog(nullptr, tr("Save file received from %1").arg(sanitizedText(title(), 100)));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setAcceptMode(QFileDialog::AcceptSave);
        dialog->setFileMode(QFileDialog::AnyFile);
        dialog->setDirectory(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        dialog->selectFile(clean);
        // On Wayland a dialog with no transient parent is not attached to the
        // window. An overlay's pane is its host's child, in the host's window.
        const Pane* place = m_overlayHost ? m_overlayHost : this;
        if (auto* window = qobject_cast<QWindow*>(place->parent())) {
            dialog->winId();
            dialog->windowHandle()->setTransientParent(window);
        }
        // Keep the lease until the dialog is destroyed, including on cancellation.
        connect(dialog, &QObject::destroyed, [lease] {});
        connect(this, &QObject::destroyed, dialog, &QDialog::reject);
        connect(dialog, &QFileDialog::fileSelected, dialog, [bytes](const QString& path) {
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
                // Not a modal box parented to the dialog: the dialog deletes
                // itself when the pane closes, which would end a nested loop
                // inside it.
                NotificationCenter::instance().showNotice(nullptr, tr("Download failed"),
                                                          QStringLiteral("%1: %2").arg(path, file.errorString()));
            }
        });
        dialog->open();
    }, Qt::QueuedConnection);
}
}

namespace Afterglow {
FileTransfer* Pane::fileTransfers()
{
    if (!m_fileTransfers) {
        m_fileTransfers = std::make_unique<FileTransfer>();
        // A program that reads slowly holds the transfer back before the
        // input queue fills and a refused reply ends it.
        m_fileTransfers->setInputBacklog([this] { return m_pty ? m_pty->pendingInput() : std::size_t(0); });
        connect(m_fileTransfers.get(), &FileTransfer::permissionRequested, this, &Pane::fileTransferPermissionRequested);
        // The worker writes the replies of a command after its command
        // closure returned. Until then the reports after it wait.
        connect(m_fileTransfers.get(), &FileTransfer::answering, this, [this] {
            ++m_answersBegun;
            m_protocolQueued.fetch_add(1);
        });
        connect(m_fileTransfers.get(), &FileTransfer::answered, this, &Pane::endAnswerHold);
        connect(m_fileTransfers.get(), &FileTransfer::reply, this, [this](const QByteArray& bytes) {
            if (sendReply(std::string_view(bytes.constData(), std::size_t(bytes.size())))) return;
            // A held pane's program has exited: no input failed, nothing reads it.
            if (!isHeld()) reportInputFailure();
            m_fileTransfers->reset();
        });
    }
    return m_fileTransfers.get();
}
std::pair<std::uint64_t, std::uint64_t> Pane::accessibleSerial() const
{
    std::lock_guard lock(m_mutex);
    return {m_terminal.generation(), m_selectionSerial};
}

DragDrop* Pane::dragDrop()
{
    if (!m_dragDrop) {
        m_dragDrop = std::make_unique<DragDrop>();
        m_dragDrop->setInputBacklog([this] { return m_pty ? m_pty->pendingInput() : std::size_t(0); });
        m_dragDrop->setCommandDelivery([this](int waitMs) { deliverProtocolCommands(waitMs); });
        m_dragDrop->setAnswerWait(m_dragAnswerWait);
        connect(m_dragDrop.get(), &DragDrop::reply, this, [this](const QByteArray& bytes) {
            if (sendReply(std::string_view(bytes.constData(), std::size_t(bytes.size())))) return;
            // A held pane's program has exited: no input failed, nothing reads it.
            if (!isHeld()) reportInputFailure();
            // Not a terminal reset: the chunks that the program still sends are dropped.
            m_dragDrop->end();
        });
    }
    return m_dragDrop.get();
}
void Pane::setDragAnswerWait(int ms)
{
    m_dragAnswerWait = ms;
    if (m_dragDrop)
        m_dragDrop->setAnswerWait(ms);
}
namespace {
/// What a gap holds for drag and drop after it also took a command of
/// protocol @p code, which the program wrote after the others.
DragDrop::Lost lostAfter(DragDrop::Lost lost, int code)
{
    return code == 72 ? DragDrop::Lost::Packets : code == 0 ? DragDrop::Lost::Reset : lost;
}
} // namespace
void Pane::terminalProtocol(int code, std::string_view payload)
{
    const QByteArray bytes(payload.data(), qsizetype(payload.size()));
    if (code == 72) {
        // Capability replies must precede a DA query in the same PTY read.
        const auto response = DragDrop::queryReply(bytes);
        if (!response.isEmpty()) { writeToPty(std::string_view(response.constData(), std::size_t(response.size()))); return; }
    }
    if (m_protocolBlocked) {
        m_protocolLost = lostAfter(m_protocolLost, code);
        m_protocolLostReset = m_protocolLostReset || code == 0;
        return;
    }
    const auto charge = bytes.size() + 128;
    if (m_protocolBytes.fetch_add(charge) + charge > ProtocolBudget) {
        m_protocolBytes.fetch_sub(charge);
        m_protocolBlocked = true;
        m_protocolLost = lostAfter(DragDrop::Lost::Nothing, code);
        m_protocolLostReset = code == 0;
        m_protocolGeneration.fetch_add(1);
        // Queued after every stale command, so it reads the whole gap. The
        // lock keeps a command from being dropped after the gap was read.
        postProtocolCommand([this] {
            DragDrop::Lost lost;
            bool reset;
            {
                std::lock_guard lock(m_mutex);
                lost = m_protocolLost == DragDrop::Lost::Nothing ? m_protocolStale : m_protocolLost;
                reset = m_protocolLostReset || m_protocolStaleReset;
                m_protocolLost = DragDrop::Lost::Nothing;
                m_protocolLostReset = false;
                m_protocolBlocked = false;
            }
            m_protocolStale = DragDrop::Lost::Nothing;
            m_protocolStaleReset = false;
            Q_EMIT terminalProtocolLost(lost, reset);
        });
        return;
    }
    const auto generation = m_protocolGeneration.load();
    postProtocolCommand([this, code, bytes, generation, charge] {
        m_protocolBytes.fetch_sub(charge);
        if (generation == m_protocolGeneration.load()) Q_EMIT terminalProtocolRequested(code, bytes);
        else {
            m_protocolStale = lostAfter(m_protocolStale, code);
            m_protocolStaleReset = m_protocolStaleReset || code == 0;
        }
    });
}
void Pane::postProtocolCommand(std::function<void()> command)
{
    m_protocolQueued.fetch_add(1);
    // A report after this command follows its replies.
    m_openReports.reset();
    // The count drops after the command and the replies that it wrote.
    QMetaObject::invokeMethod(&m_protocolCommands, [this, command = std::move(command)] {
        command();
        m_protocolQueued.fetch_sub(1);
    }, Qt::QueuedConnection);
    {
        std::lock_guard lock(m_postedMutex);
        ++m_postedSerial;
    }
    m_posted.notify_all();
}
void Pane::endAnswerHold()
{
    // The worker answers the commands in order.
    ++m_answersEnded;
    while (!m_parkedReports.empty() && m_parkedReports.front().first <= m_answersEnded) {
        writeReports(m_parkedReports.front().second);
        m_parkedReports.pop_front();
    }
    // Last: a report that the program writes meanwhile waits for the parked ones.
    m_protocolQueued.fetch_sub(1);
}
void Pane::deliverProtocolCommands(int waitMs)
{
    std::uint64_t seen;
    {
        std::lock_guard lock(m_postedMutex);
        seen = m_postedSerial;
    }
    QCoreApplication::sendPostedEvents(&m_protocolCommands, QEvent::MetaCall);
    // Commands came since the last call: the caller reads them before it waits.
    const bool delivered = std::exchange(m_deliveredSerial, seen) != seen;
    if (waitMs <= 0 || delivered)
        return;
    {
        // A command posted after the delivery above ends the wait at once.
        std::unique_lock lock(m_postedMutex);
        if (!m_posted.wait_for(lock, std::chrono::milliseconds(waitMs), [&] { return m_postedSerial != seen; }))
            return;
        m_deliveredSerial = m_postedSerial;
    }
    QCoreApplication::sendPostedEvents(&m_protocolCommands, QEvent::MetaCall);
}
} // namespace Afterglow
