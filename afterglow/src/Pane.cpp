// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Pane.h"
#include "desktop/FileTransfer.h"
#include "desktop/DragDrop.h"

#include "app/NotificationCenter.h"

#include <AfterglowVt/Unicode.h>

#include <QByteArray>
#include <QFile>
#include <QFileDialog>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDir>
#include <QMessageBox>
#include <QStringList>
#include <QImage>
#include <QImageReader>
#include <QBuffer>
#include <QTextBoundaryFinder>
#include <QSysInfo>

#include <algorithm>
#include <chrono>
#include <QLoggingCategory>

#include <cstring>

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
    if (m_searchJob) m_searchJob->cancelled = true;
    // Stop the pty thread before the terminal it feeds goes away.
    if (m_pty) m_pty->stop();
    m_dragDrop.reset();
    m_fileTransfers.reset();
    m_pty.reset();
}

bool Pane::start(const AfterglowPty::SpawnOptions& options, const QString& term, int cols, int rows, int cellWidth,
                 int cellHeight, QString* error)
{
    // Size the terminal before the shell starts: programs run from the shell's
    // startup (fastfetch drawing its logo with kitty graphics) query the cell
    // size immediately, and a zero answer makes them fall back to text.
    {
        std::lock_guard lock(m_mutex);
        m_terminal.resize(cols, rows, cellWidth, cellHeight);
    }
    AfterglowPty::SpawnOptions opts = options;
    // Lets `afterglow ctl` run in the pane act on that pane by default.
    opts.environment.emplace_back("AFTERGLOW_PANE_ID", std::to_string(m_id));
    opts.cols = cols;
    opts.rows = rows;
    opts.pixelWidth = cols * cellWidth;
    opts.pixelHeight = rows * cellHeight;
    opts.environment.emplace_back("TERM", term.toStdString());
    if (term == QLatin1String("afterglow")) {
        // Our own entry: point ncurses at it, ahead of the system databases
        // (the trailing ':' keeps the defaults).
        QStringList dirs;
        for (const char* d : {AFTERGLOW_BUILD_TERMINFO_DIR, AFTERGLOW_INSTALLED_TERMINFO_DIR}) {
            if (QFile::exists(QString::fromUtf8(d) + QStringLiteral("/a/afterglow"))) {
                dirs << QString::fromUtf8(d);
            }
        }
        if (const QByteArray existing = qgetenv("TERMINFO_DIRS"); !existing.isEmpty()) {
            dirs << QString::fromLocal8Bit(existing);
        }
        opts.environment.emplace_back("TERMINFO_DIRS", (dirs.join(QLatin1Char(':')) + QLatin1Char(':')).toStdString());
    }
    opts.environment.emplace_back("COLORTERM", "truecolor");
    opts.environment.emplace_back("TERM_PROGRAM", "afterglow");
    opts.environment.emplace_back("TERM_PROGRAM_VERSION", AFTERGLOW_VERSION);
    std::string err;
    opts.deferStart = true;
    if (m_pty) m_pty->stop();
    m_pty = AfterglowPty::Pty::spawn(opts, *this, &err);
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

bool Pane::sendInput(std::string_view bytes)
{
    if (bytes.empty() || !m_pty || m_held.load()) {
        return bytes.empty();
    }
    if (m_scrollOnInput) {
        std::lock_guard lock(m_mutex);
        m_terminal.resetViewport();
    }
    const bool accepted = m_pty->write(bytes);
    if (!accepted) reportInputFailure();
    return accepted;
}

bool Pane::refreshSnapshot(int extraRows)
{
    std::lock_guard lock(m_mutex);
    if (m_terminal.modes().synchronizedOutput && m_snapshotGeneration != ~0ull) {
        if (!m_syncTimer.isValid()) {
            m_syncTimer.start();
        }
        if (m_syncTimer.elapsed() < SyncTimeoutMs) {
            return false;
        }
    } else {
        m_syncTimer.invalidate();
    }
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
    if (m_terminal.generation() != m_snapshotGeneration || m_selectionSerial != m_snapshotSelectionSerial
        || extraRows != m_snapshotExtraRows) {
        const bool selectionHere = m_selection.active && m_selectionOnAltScreen == m_terminal.modes().altScreen;
        AfterglowRender::SearchHighlight highlight;
        if (m_searchActive && m_searchEpoch == m_terminal.screen().layoutEpoch() && m_searchAlt == m_terminal.modes().altScreen) {
            highlight.matches = &m_searchMatches;
            highlight.current = m_searchCurrent;
            highlight.matchColor = AfterglowRender::packRgba(0xe5, 0xc0, 0x7b);
            highlight.currentColor = AfterglowRender::packRgba(0xff, 0x8c, 0x42);
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
    m_selection.anchor = anchor;
    m_selection.head = anchor;
    m_selection.mode = mode;
    m_selection.layoutEpoch = m_terminal.screen().layoutEpoch();
    m_selection.active = activeNow;
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
    std::lock_guard lock(m_mutex);
    if (!m_selection.active || m_selectionOnAltScreen != m_terminal.modes().altScreen) {
        return {};
    }
    return QString::fromStdString(m_selection.text(m_terminal.screen(), m_terminal.options().wordBoundaries));
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
    m_selection = {m_copyCursor, m_copyCursor, AfterglowVt::SelectionMode::Normal, screen.layoutEpoch(), true};
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
    m_selection.mode = mode;
    m_selection.anchor = m_copyCursor;
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
        Q_EMIT searchStatusChanged(tr("Layout changed; search again"));
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
        {
            std::lock_guard lock(m_mutex);
            if (m_terminal.screen().layoutEpoch() != job->epoch || m_terminal.modes().altScreen != job->alt) {
                result.matches.clear(); result.error = "Layout changed; search again";
            }
            std::erase_if(result.matches, [this](const auto& match) { return m_terminal.screen().absoluteIndexOf(match.start.row) < 0; });
            m_searchMatches = std::move(result.matches);
            m_searchCurrent = int(m_searchMatches.size()) - 1;
            m_searchNotice = QString::fromStdString(result.error);
            if (result.truncated) m_searchNotice += tr(" (limited results)");
            if (m_searchCurrent >= 0) revealRow(m_terminal, std::size_t(m_terminal.screen().absoluteIndexOf(m_searchMatches.back().start.row)));
            ++m_selectionSerial;
        }
        search(QString::fromStdU32String(m_searchQuery), 0, m_searchRegex);
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
    if (layoutChanged || needle != m_searchQuery || regex != m_searchRegex || !m_searchActive) {
        clearSearch();
        m_searchActive = true; m_searchRegex = regex; m_searchQuery = needle;
        m_searchNotice.clear();
        auto job = std::make_shared<SearchJob>();
        {
            std::lock_guard lock(m_mutex);
            const auto& screen = m_terminal.screen();
            job->epoch = m_searchEpoch = screen.layoutEpoch(); job->alt = m_searchAlt = m_terminal.modes().altScreen;
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
            s.graphemes.push_back(cluster);
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
            // Past the live screen's top is the live screen itself.
            const long target = std::min<long>(long(abs), long(scr.historySize()));
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

void Pane::setUserTitle(const QString& title)
{
    QString shown;
    {
        std::lock_guard lock(m_mutex);
        m_userTitle = title;
        shown = m_userTitle.isEmpty() ? m_title : m_userTitle;
    }
    Q_EMIT titleUpdated(shown);
}

QString Pane::text(AfterglowVt::Terminal::TextScope scope)
{
    std::lock_guard lock(m_mutex);
    return QString::fromStdString(m_terminal.text(scope));
}

QVariantMap Pane::readText(AfterglowVt::Terminal::TextScope scope, std::size_t limit)
{
    std::lock_guard lock(m_mutex);
    bool truncated = false;
    const auto text = m_terminal.text(scope, std::min(limit, std::size_t(1024 * 1024)), &truncated);
    return {{QStringLiteral("text"), QString::fromStdString(text)}, {QStringLiteral("truncated"), truncated},
            {QStringLiteral("available"), scope != AfterglowVt::Terminal::TextScope::LastOutput || m_terminal.hasCommandOutput()}};
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
    Q_EMIT commandBegan(m_command);
}

void Pane::commandFinished(int exitCode)
{
    const auto elapsed = std::chrono::steady_clock::now() - m_commandStart;
    const qint64 ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const QString command = std::exchange(m_command, QString());
    Q_EMIT commandEnded(command, exitCode, ms);
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
    return m_pty ? QString::fromStdString(m_pty->currentWorkingDirectory()) : QString();
}

void Pane::reportInputFailure()
{
    if (m_inputFailureQueued.exchange(true)) return;
    QMetaObject::invokeMethod(this, [this] {
        m_inputFailureQueued = false;
        Q_EMIT inputRejected();
    }, Qt::QueuedConnection);
}

void Pane::writeToPty(std::string_view bytes)
{
    if (m_pty && !m_pty->write(bytes)) reportInputFailure();
}

void Pane::titleChanged(std::string_view title)
{
    m_title = QString::fromUtf8(title.data(), qsizetype(title.size()));
    Q_EMIT titleUpdated(m_userTitle.isEmpty() ? m_title : m_userTitle);
}

void Pane::bell()
{
    Q_EMIT bellRang();
}

void Pane::clipboardWrite(std::string_view selection, std::string_view data)
{
    const QByteArray decoded = QByteArray::fromBase64(QByteArray(data.data(), qsizetype(data.size())));
    const QString text = QString::fromUtf8(decoded);
    // xterm targets: 'c' clipboard, 'p'/'s' primary selection. Empty means the clipboard.
    if (selection.empty() || selection.find('c') != std::string_view::npos) {
        Q_EMIT clipboardWriteRequested(text, false);
    }
    if (selection.find_first_of("ps") != std::string_view::npos) {
        Q_EMIT clipboardWriteRequested(text, true);
    }
}

namespace {

bool toRgba(QImage img, std::uint32_t& width, std::uint32_t& height, std::vector<std::uint8_t>& rgba)
{
    if (img.isNull() || img.width() > 10000 || img.height() > 10000
        || std::uint64_t(img.width()) * img.height() * 4 > AfterglowVt::GraphicsStore::DefaultQuota) return false;
    img.convertTo(QImage::Format_RGBA8888);
    if (img.isNull()) return false;
    width = std::uint32_t(img.width());
    height = std::uint32_t(img.height());
    rgba.resize(std::size_t(width) * height * 4);
    for (int y = 0; y < img.height(); ++y) {
        std::memcpy(rgba.data() + std::size_t(y) * width * 4, img.constScanLine(y), std::size_t(width) * 4);
    }
    return true;
}

bool decodeBoundedImage(std::string_view data, const char* format, std::uint32_t& width,
                        std::uint32_t& height, std::vector<std::uint8_t>& rgba)
{
    if (data.size() > AfterglowVt::GraphicsStore::DefaultQuota) return false;
    QByteArray bytes = QByteArray::fromRawData(data.data(), qsizetype(data.size()));
    QBuffer input(&bytes);
    if (!input.open(QIODevice::ReadOnly)) return false;
    QImageReader reader(&input, format);
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty() || size.width() > 10000 || size.height() > 10000
        || std::uint64_t(size.width()) * size.height() * 4 > AfterglowVt::GraphicsStore::DefaultQuota) return false;
    return toRgba(reader.read(), width, height, rgba);
}

QString localUser()
{
    const passwd* pw = getpwuid(getuid());
    return pw ? QString::fromLocal8Bit(pw->pw_name) : QString();
}

} // namespace

bool Pane::decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height, std::vector<std::uint8_t>& rgba)
{
    return decodeBoundedImage(png, "PNG", width, height, rgba);
}

bool Pane::decodeImage(std::string_view data, std::uint32_t& width, std::uint32_t& height, std::vector<std::uint8_t>& rgba)
{
    // Whatever Qt reads; an animation shows its first frame.
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
    if (label != m_contextLabel || kind != m_contextKind) {
        m_contextLabel = label;
        m_contextKind = label.isEmpty() ? QString() : kind;
        Q_EMIT contextUpdated();
    }
}

void Pane::userVarChanged(std::string_view name, std::string_view value)
{
    const QString n = QString::fromUtf8(name.data(), qsizetype(name.size()));
    const QString v = QString::fromUtf8(value.data(), qsizetype(value.size()));
    if (m_userVars.size() >= 256 && !m_userVars.contains(n)) {
        return; // enough
    }
    m_userVars.insert(n, v);
    Q_EMIT userVarUpdated(n, v);
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

QHash<QString, QString> Pane::userVars() const
{
    std::lock_guard lock(m_mutex);
    return m_userVars;
}

void Pane::tickAnimations()
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
    if (changed && sender() == &m_animationTimer) {
        Q_EMIT contentChanged();
    }
}

void Pane::notify(std::string_view title, std::string_view body)
{
    Q_EMIT notificationRequested(QString::fromUtf8(title.data(), qsizetype(title.size())),
                                 QString::fromUtf8(body.data(), qsizetype(body.size())));
}

void Pane::pointerShapeChanged(std::string_view shape)
{
    Q_EMIT pointerShapeRequested(QString::fromUtf8(shape.data(), qsizetype(shape.size())));
}

void Pane::desktopNotification(const AfterglowVt::NotificationRequest& request)
{
    Q_EMIT desktopNotificationRequested(request);
}

bool Pane::clipboardRequest(const AfterglowVt::ClipboardRequest& request)
{
    Q_EMIT clipboardRequested(request);
    return true;
}

std::string Pane::notificationCapabilities()
{
    return NotificationCenter::capabilities();
}

void Pane::flash()
{
    Q_EMIT flashRequested();
}

void Pane::programStatusChanged()
{
    Q_EMIT statusChanged(); // queued to the GUI thread, which reads status()
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
                tickAnimations();
                Q_EMIT contentChanged();
            },
            Qt::QueuedConnection);
    }
}

void Pane::ptyClosed(int status)
{
    if (status == -1) {
        qCInfo(lcPane) << "shell closed the terminal before exiting";
    } else if (WIFSIGNALED(status)) {
        qCWarning(lcPane) << "shell killed by signal" << WTERMSIG(status) << (WCOREDUMP(status) ? "(core dumped)" : "");
    } else {
        qCInfo(lcPane) << "shell exited with status" << WEXITSTATUS(status);
    }
    {
        std::lock_guard lock(m_mutex);
        m_terminal.programExited();
        const int code = status == -1 ? 0 : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
        if (m_hold.load() || (m_holdOnFailure.load() && code != 0)) {
            const std::string note = "\r\n\x1b[0;2m[exited with status " + std::to_string(code)
                + "; press any key to close]\x1b[0m\x1b[?25l";
            m_terminal.feed(note);
            m_held.store(true);
        }
    }
    if (m_held.load()) {
        QMetaObject::invokeMethod(this, [this] { Q_EMIT contentChanged(); }, Qt::QueuedConnection);
        return;
    }
    Q_EMIT closed();
}

void Pane::holdStartFailure()
{
    {
        std::lock_guard lock(m_mutex);
        m_terminal.feed("\r\n[Unable to start a process; check the project/configuration. Press any key to close.]\r\n");
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
    if (busy.exchange(true)) return;
    auto lease = std::shared_ptr<void>(nullptr, [](void*) { busy.store(false); });
    const QByteArray bytes(data.data(), qsizetype(data.size()));
    QString suggested = QString::fromUtf8(name.data(), qsizetype(name.size()));
    suggested.replace(QLatin1Char('\\'), QLatin1Char('/'));
    suggested = suggested.section(QLatin1Char('/'), -1);
    QString clean;
    for (const QChar c : suggested) if (!c.isNull() && c.isPrint() && c.category() != QChar::Other_Format) clean += c;
    if (clean.isEmpty() || clean == QLatin1String(".") || clean == QLatin1String("..")) clean = QStringLiteral("download.bin");
    clean = clean.left(180);
    QMetaObject::invokeMethod(this, [this, bytes, clean, lease] {
        auto* dialog = new QFileDialog(nullptr, tr("Save file received from %1").arg(title().left(100)));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setAcceptMode(QFileDialog::AcceptSave);
        dialog->setFileMode(QFileDialog::AnyFile);
        dialog->setDirectory(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        dialog->selectFile(clean);
        // Keep the lease until the dialog is destroyed, including on cancellation.
        connect(dialog, &QObject::destroyed, [lease] {});
        connect(this, &QObject::destroyed, dialog, &QDialog::reject);
        connect(dialog, &QFileDialog::fileSelected, dialog, [bytes, dialog](const QString& path) {
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
                QMessageBox::warning(dialog, tr("Download failed"), file.errorString());
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
        connect(m_fileTransfers.get(), &FileTransfer::permissionRequested, this, &Pane::fileTransferPermissionRequested);
        connect(m_fileTransfers.get(), &FileTransfer::reply, this, [this](const QByteArray& bytes) {
            if (!sendInput(std::string_view(bytes.constData(), std::size_t(bytes.size())))) m_fileTransfers->reset();
        });
    }
    return m_fileTransfers.get();
}
DragDrop* Pane::dragDrop()
{
    if (!m_dragDrop) {
        m_dragDrop = std::make_unique<DragDrop>();
        connect(m_dragDrop.get(), &DragDrop::reply, this, [this](const QByteArray& bytes) {
            if (!sendInput(std::string_view(bytes.constData(), std::size_t(bytes.size())))) m_dragDrop->reset();
        });
    }
    return m_dragDrop.get();
}
void Pane::terminalProtocol(int code, std::string_view payload)
{
    const QByteArray bytes(payload.data(), qsizetype(payload.size()));
    if (code == 72) {
        // Capability replies must precede a DA query in the same PTY read.
        const auto response = DragDrop::queryReply(bytes);
        if (!response.isEmpty()) { writeToPty(std::string_view(response.constData(), std::size_t(response.size()))); return; }
    }
    if (m_protocolBlocked.load()) return;
    const auto charge = bytes.size() + 128;
    if (m_protocolBytes.fetch_add(charge) + charge > 16 * 1024 * 1024) {
        m_protocolBytes.fetch_sub(charge);
        if (m_protocolBlocked.exchange(true)) return;
        m_protocolGeneration.fetch_add(1);
        QMetaObject::invokeMethod(this, [this] { Q_EMIT terminalProtocolRequested(-1, {}); m_protocolBlocked.store(false); }, Qt::QueuedConnection);
        return;
    }
    const auto generation = m_protocolGeneration.load();
    QMetaObject::invokeMethod(this, [this, code, bytes, generation, charge] {
        m_protocolBytes.fetch_sub(charge);
        if (generation == m_protocolGeneration.load()) Q_EMIT terminalProtocolRequested(code, bytes);
    }, Qt::QueuedConnection);
}
} // namespace Afterglow
