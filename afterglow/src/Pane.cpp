// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Pane.h"

#include <AfterglowVt/Unicode.h>

#include <QByteArray>
#include <QFile>
#include <QStringList>
#include <QImage>

#include <algorithm>
#include <chrono>
#include <QLoggingCategory>

#include <cstring>

#include <sys/wait.h>

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

Pane::Pane(const AfterglowVt::TerminalOptions& options, QObject* parent)
    : QObject(parent)
    , m_terminal(InitialCols, InitialRows, *this, options)
{
    m_animationTimer.setSingleShot(true);
    m_animationTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_animationTimer, &QTimer::timeout, this, &Pane::tickAnimations);
}

Pane::~Pane()
{
    // Stop the pty thread before the terminal it feeds goes away.
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
    m_pty = AfterglowPty::Pty::spawn(opts, *this, &err);
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

void Pane::sendInput(std::string_view bytes)
{
    if (bytes.empty() || !m_pty) {
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_terminal.resetViewport();
    }
    m_pty->write(bytes);
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
        if (m_searchActive) {
            highlight.matches = &m_searchMatches;
            highlight.current = m_searchCurrent;
            highlight.matchColor = AfterglowRender::packRgba(0xe5, 0xc0, 0x7b);
            highlight.currentColor = AfterglowRender::packRgba(0xff, 0x8c, 0x42);
        }
        m_snapshot.capture(m_terminal, selectionHere ? &m_selection : nullptr, highlight, extraRows);
        applyPreedit();
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
    return QString::fromStdString(m_selection.text(m_terminal.screen()));
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

Pane::SearchResult Pane::search(const QString& query, int direction)
{
    std::lock_guard lock(m_mutex);
    const std::u32string needle = query.toStdU32String();
    const AfterglowVt::Screen& scr = m_terminal.screen();
    if (direction == 0 || needle != m_searchQuery || !m_searchActive) {
        m_searchQuery = needle;
        m_searchMatches = AfterglowVt::findAll(scr, needle);
        m_searchActive = true;
        // Start from the newest match at or above the bottom of the viewport.
        const std::uint64_t bottom = scr.rowsDropped() + scr.viewportTop() + std::size_t(scr.rows()) - 1;
        m_searchCurrent = -1;
        for (int i = int(m_searchMatches.size()) - 1; i >= 0; --i) {
            if (m_searchMatches[std::size_t(i)].start.row <= bottom) {
                m_searchCurrent = i;
                break;
            }
        }
        if (m_searchCurrent < 0 && !m_searchMatches.empty()) {
            m_searchCurrent = int(m_searchMatches.size()) - 1;
        }
    } else if (!m_searchMatches.empty()) {
        const int n = int(m_searchMatches.size());
        m_searchCurrent = ((m_searchCurrent + direction) % n + n) % n;
    }
    if (m_searchCurrent >= 0) {
        const long long abs = scr.absoluteIndexOf(m_searchMatches[std::size_t(m_searchCurrent)].start.row);
        if (abs >= 0) {
            revealRow(m_terminal, std::size_t(abs));
        }
    }
    ++m_selectionSerial; // highlights changed: recapture
    return {m_searchCurrent, int(m_searchMatches.size())};
}

void Pane::clearSearch()
{
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
    std::vector<std::pair<char32_t, int>> cells;
    int total = 0;
    int caretCol = -1;
    for (std::size_t i = 0; i < m_preedit.size(); ++i) {
        if (int(i) == m_preeditCursor) {
            caretCol = total;
        }
        const int w = AfterglowVt::Unicode::width(m_preedit[i]);
        if (w == 0) {
            continue;
        }
        cells.emplace_back(m_preedit[i], w);
        total += w;
    }
    if (caretCol < 0) {
        caretCol = total;
    }
    const int start = std::max(0, std::min(s.cursorX, s.cols - std::min(total + 1, s.cols)));
    int x = start;
    for (const auto& [cp, w] : cells) {
        if (x + w > s.cols) {
            break;
        }
        AfterglowRender::SnapshotCell& c = s.cells[std::size_t(s.cursorY) * std::size_t(s.cols) + std::size_t(x)];
        c.cp = cp;
        c.width = std::uint8_t(w);
        c.fg = s.defaultFg;
        c.bg = s.defaultBg;
        c.underline = AfterglowVt::UnderlineStyle::Single;
        c.underlineColor = s.defaultFg;
        c.grapheme = 0;
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
    return m_title;
}

QString Pane::workingDirectory() const
{
    return m_pty ? QString::fromStdString(m_pty->currentWorkingDirectory()) : QString();
}

void Pane::writeToPty(std::string_view bytes)
{
    if (m_pty) {
        m_pty->write(bytes);
    }
}

void Pane::titleChanged(std::string_view title)
{
    m_title = QString::fromUtf8(title.data(), qsizetype(title.size()));
    Q_EMIT titleUpdated(m_title);
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

bool Pane::decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height, std::vector<std::uint8_t>& rgba)
{
    QImage img = QImage::fromData(reinterpret_cast<const uchar*>(png.data()), int(png.size()), "PNG");
    if (img.isNull()) {
        return false;
    }
    img.convertTo(QImage::Format_RGBA8888);
    width = std::uint32_t(img.width());
    height = std::uint32_t(img.height());
    rgba.resize(std::size_t(width) * height * 4);
    for (int y = 0; y < img.height(); ++y) {
        std::memcpy(rgba.data() + std::size_t(y) * width * 4, img.constScanLine(y), std::size_t(width) * 4);
    }
    return true;
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
    }
    Q_EMIT closed();
}

} // namespace Afterglow
