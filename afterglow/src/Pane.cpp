// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Pane.h"

#include <QByteArray>
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

bool Pane::refreshSnapshot()
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
    if (m_terminal.generation() != m_snapshotGeneration || m_selectionSerial != m_snapshotSelectionSerial) {
        const bool selectionHere = m_selection.active && m_selectionOnAltScreen == m_terminal.modes().altScreen;
        m_snapshot.capture(m_terminal, selectionHere ? &m_selection : nullptr);
        m_snapshotSelectionSerial = m_selectionSerial;
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
    Q_EMIT closed();
}

} // namespace Afterglow
