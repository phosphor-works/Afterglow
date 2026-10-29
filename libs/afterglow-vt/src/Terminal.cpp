// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Input.h>
#include <AfterglowVt/Terminal.h>

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace AfterglowVt {

namespace {

constexpr char32_t DecSpecialGraphics[32] = {
    0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B,
    0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C,
    0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

constexpr std::string_view Version = "0.1.0";

std::array<Rgb, 256> buildPalette(const std::array<Rgb, 16>& base)
{
    std::array<Rgb, 256> p{};
    for (int i = 0; i < 16; ++i) {
        p[std::size_t(i)] = base[std::size_t(i)];
    }
    constexpr std::uint8_t Levels[6] = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
    for (int i = 0; i < 216; ++i) {
        p[std::size_t(16 + i)] = Rgb{Levels[i / 36], Levels[(i / 6) % 6], Levels[i % 6]};
    }
    for (int i = 0; i < 24; ++i) {
        const auto v = std::uint8_t(8 + i * 10);
        p[std::size_t(232 + i)] = Rgb{v, v, v};
    }
    return p;
}

int hexDigit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// Parses one hex component of 1-4 digits and scales it to 8 bits.
bool parseHexComponent(std::string_view s, std::uint8_t& out)
{
    if (s.empty() || s.size() > 4) {
        return false;
    }
    unsigned v = 0;
    for (char c : s) {
        const int d = hexDigit(c);
        if (d < 0) {
            return false;
        }
        v = v * 16 + unsigned(d);
    }
    const unsigned max = (1u << (4 * s.size())) - 1;
    out = std::uint8_t((v * 255 + max / 2) / max);
    return true;
}

/// X11 colour spec: "rgb:r/g/b" or "#rgb" / "#rrggbb" / "#rrrgggbbb" / "#rrrrggggbbbb".
bool parseColorSpec(std::string_view spec, Rgb& out)
{
    if (spec.starts_with("rgb:")) {
        spec.remove_prefix(4);
        const auto s1 = spec.find('/');
        const auto s2 = s1 == std::string_view::npos ? s1 : spec.find('/', s1 + 1);
        if (s2 == std::string_view::npos) {
            return false;
        }
        return parseHexComponent(spec.substr(0, s1), out.r) && parseHexComponent(spec.substr(s1 + 1, s2 - s1 - 1), out.g)
            && parseHexComponent(spec.substr(s2 + 1), out.b);
    }
    if (spec.starts_with('#')) {
        spec.remove_prefix(1);
        if (spec.empty() || spec.size() % 3 != 0 || spec.size() > 12) {
            return false;
        }
        const std::size_t n = spec.size() / 3;
        return parseHexComponent(spec.substr(0, n), out.r) && parseHexComponent(spec.substr(n, n), out.g)
            && parseHexComponent(spec.substr(2 * n, n), out.b);
    }
    return false;
}

std::string formatColorReply(std::string_view prefix, Rgb c, bool bel)
{
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "rgb:%02x%02x/%02x%02x/%02x%02x", c.r, c.r, c.g, c.g, c.b, c.b);
    std::string out = "\x1b]";
    out += prefix;
    out.append(buf, std::size_t(n));
    out += bel ? "\x07" : "\x1b\\";
    return out;
}

} // namespace

Terminal::Terminal(int cols, int rows, ITerminalHost& host, const TerminalOptions& options)
    : m_host(host)
    , m_options(options)
    , m_parser(*this)
    , m_primary(std::make_unique<Screen>(cols, rows, options.maxScrollback))
    , m_alternate(std::make_unique<Screen>(cols, rows, 0))
    , m_active(m_primary.get())
    , m_palette(buildPalette(options.basePalette))
    , m_defaultFg(options.defaultForeground)
    , m_defaultBg(options.defaultBackground)
{
    m_cursorShape = options.cursorShape;
    m_modes.cursorBlink = options.cursorBlink;
    m_margins = Region{0, rows - 1, 0, cols - 1};
    m_tabStops.assign(std::size_t(cols), false);
    for (int x = 0; x < cols; x += 8) {
        m_tabStops[std::size_t(x)] = true;
    }
}

Terminal::~Terminal() = default;

void Terminal::feed(std::string_view bytes)
{
    if (bytes.empty()) {
        return;
    }
    m_parser.feed(bytes);
    touch();
}

void Terminal::setOptions(const TerminalOptions& options)
{
    // The primary screen keeps history only if it had any; the alternate never does.
    if (options.maxScrollback != m_options.maxScrollback) {
        m_primary->setMaxScrollback(options.maxScrollback);
    }
    m_options = options;
    m_palette = buildPalette(m_options.basePalette);
    m_defaultFg = m_options.defaultForeground;
    m_defaultBg = m_options.defaultBackground;
    m_hasCursorColor = false;
    m_cursorShape = m_options.cursorShape;
    m_modes.cursorBlink = m_options.cursorBlink;
    touch();
}

void Terminal::resize(int cols, int rows, int cellWidth, int cellHeight)
{
    cols = std::max(cols, 2);
    rows = std::max(rows, 1);
    m_cellWidth = cellWidth;
    m_cellHeight = cellHeight;
    if (cols == this->cols() && rows == this->rows()) {
        return;
    }
    const std::optional<std::size_t> prompt = m_modes.altScreen ? std::nullopt : redrawablePromptStart();
    const bool reflows = cols != m_primary->cols();
    if (prompt && reflows) {
        m_promptRedrawPending = true;
    }
    // Reflow renumbers rows; images follow the rows they are anchored to.
    std::vector<std::size_t> anchors = m_graphics[0].anchorRows(*m_primary);
    m_primary->resize(cols, rows, true, prompt, &anchors);
    m_graphics[0].retarget(*m_primary, anchors);
    m_alternate->resize(cols, rows, false);
    m_margins = Region{0, rows - 1, 0, cols - 1};

    const std::size_t oldCols = m_tabStops.size();
    m_tabStops.resize(std::size_t(cols), false);
    for (std::size_t x = oldCols; x < std::size_t(cols); ++x) {
        m_tabStops[x] = (x % 8) == 0;
    }
    m_lastX = m_lastY = -1;
    touch();
}

Rgb Terminal::resolve(Color color, bool foreground) const noexcept
{
    switch (color.kind()) {
    case Color::Kind::Default:
        return foreground ? m_defaultFg : m_defaultBg;
    case Color::Kind::Palette:
        return m_palette[color.index()];
    case Color::Kind::Rgb:
        return Rgb{color.r(), color.g(), color.b()};
    }
    return m_defaultFg;
}

void Terminal::scrollViewport(long rows)
{
    m_active->scrollViewport(rows);
    touch();
}

void Terminal::setViewportOffset(std::size_t rows)
{
    const long delta = long(rows) - long(m_active->viewportOffset());
    if (delta != 0) {
        m_active->scrollViewport(delta);
        touch();
    }
}

void Terminal::resetViewport()
{
    if (m_active->viewportOffset() != 0) {
        m_active->resetViewport();
        touch();
    }
}

// --- helpers ---------------------------------------------------------------

Region Terminal::scrollRegion() const noexcept
{
    return m_margins;
}

std::uint32_t Terminal::acquireFill()
{
    Style fill;
    fill.bg = cursor().pen.bg;
    return m_active->styles().acquire(fill);
}

void Terminal::releaseFill(std::uint32_t id)
{
    m_active->styles().release(id);
}

void Terminal::reply(std::string_view bytes)
{
    m_host.writeToPty(bytes);
}

// --- printing --------------------------------------------------------------

void Terminal::print(char32_t cp)
{
    Cursor& cur = cursor();
    Screen& scr = *m_active;

    if (cp >= 0x5F && cp <= 0x7E && m_charsets[m_gl] == Charset::DecSpecialGraphics) {
        cp = DecSpecialGraphics[cp - 0x5F];
    }

    const Unicode::Props props = Unicode::props(cp);

    // Join the previous cell's cluster when no grapheme boundary falls here.
    if (m_lastX >= 0) {
        const bool brk = m_modes.graphemeClustering ? m_segmenter.breaksBefore(props) : props.width != 0;
        if (!brk) {
            Row& row = scr.row(m_lastY);
            Cell& base = row.cells[std::size_t(m_lastX)];
            scr.appendGrapheme(row, m_lastX, cp);
            if (cp == Unicode::VariationSelector16 && base.width() == 1
                && Unicode::props(base.codepoint()).widensWithVs16 && m_lastX + 1 < scr.cols()) {
                const char32_t baseCp = base.codepoint();
                const std::u32string extra(scr.graphemeExtra(row, m_lastX));
                scr.writeCell(row, m_lastX, baseCp, base.styleId(), 2);
                for (char32_t e : extra) {
                    scr.appendGrapheme(row, m_lastX, e);
                }
                if (cur.y == m_lastY && cur.x == m_lastX + 1 && !cur.pendingWrap) {
                    if (cur.x + 1 > m_margins.right) {
                        cur.pendingWrap = true;
                    } else {
                        ++cur.x;
                    }
                }
            }
            touch();
            return;
        }
    } else if (m_modes.graphemeClustering) {
        m_segmenter.reset();
        m_segmenter.breaksBefore(props);
    }

    int width = props.width;
    if (width == 0) {
        // A combining mark with no cluster to join: attach to the cell before the cursor.
        const int x = cur.pendingWrap ? cur.x : cur.x - 1;
        if (x >= 0) {
            Row& row = scr.row(cur.y);
            int baseX = x;
            if (row.cells[std::size_t(baseX)].isTail() && baseX > 0) {
                --baseX;
            }
            scr.appendGrapheme(row, baseX, cp);
        }
        return;
    }

    if (cur.pendingWrap && m_modes.autowrap) {
        scr.row(cur.y).wrapped = true;
        carriageReturn();
        index();
    }
    cur.pendingWrap = false;

    const int right = (cur.x <= m_margins.right) ? m_margins.right : scr.cols() - 1;
    if (width == 2 && cur.x == right) {
        if (right == 0) {
            width = 1;
        } else if (m_modes.autowrap) {
            Row& row = scr.row(cur.y);
            scr.eraseCells(row, cur.x, cur.x + 1, StyleTable::DefaultId);
            row.cells[std::size_t(cur.x)].setSpacerHead(true);
            row.wrapped = true;
            carriageReturn();
            index();
        } else {
            width = 1;
            cp = Unicode::ReplacementCharacter;
        }
    }

    Row& row = scr.row(cur.y);
    if (m_modes.insert) {
        const std::uint32_t fill = StyleTable::DefaultId;
        scr.insertCells(row, cur.x, right, width, fill);
    }
    scr.writeCell(row, cur.x, cp, cur.penId, width);
    m_lastPrinted = cp;
    m_lastX = cur.x;
    m_lastY = cur.y;

    if (cur.x + width > right) {
        cur.x = right;
        cur.pendingWrap = true;
    } else {
        cur.x += width;
    }
}

void Terminal::repeatLast(int n)
{
    if (m_lastPrinted == 0) {
        return;
    }
    const char32_t cp = m_lastPrinted;
    n = std::min(n, 65535);
    for (int i = 0; i < n; ++i) {
        m_lastX = -1;
        print(cp);
    }
}

// --- C0 --------------------------------------------------------------------

void Terminal::execute(std::uint8_t c)
{
    m_lastX = -1;
    switch (c) {
    case 0x07:
        m_host.bell();
        break;
    case 0x08:
        backspace();
        break;
    case 0x09:
        horizontalTab(1);
        break;
    case 0x0A:
    case 0x0B:
    case 0x0C:
        lineFeed();
        break;
    case 0x0D:
        carriageReturn();
        break;
    case 0x0E:
        m_gl = 1;
        break;
    case 0x0F:
        m_gl = 0;
        break;
    default:
        break;
    }
}

void Terminal::carriageReturn()
{
    Cursor& cur = cursor();
    cur.x = (m_modes.originMode || cur.x >= m_margins.left) ? m_margins.left : 0;
    cur.pendingWrap = false;
}

void Terminal::lineFeed()
{
    index();
    if (m_modes.newline) {
        carriageReturn();
    }
}

void Terminal::index()
{
    Cursor& cur = cursor();
    cur.pendingWrap = false;
    if (cur.y == m_margins.bottom) {
        if (cur.x >= m_margins.left && cur.x <= m_margins.right) {
            const std::uint32_t fill = acquireFill();
            m_active->scrollUp(m_margins, 1, fill, true);
            releaseFill(fill);
        }
    } else if (cur.y < rows() - 1) {
        ++cur.y;
    }
}

void Terminal::reverseIndex()
{
    Cursor& cur = cursor();
    cur.pendingWrap = false;
    if (cur.y == m_margins.top) {
        if (cur.x >= m_margins.left && cur.x <= m_margins.right) {
            const std::uint32_t fill = acquireFill();
            m_active->scrollDown(m_margins, 1, fill);
            releaseFill(fill);
        }
    } else if (cur.y > 0) {
        --cur.y;
    }
}

void Terminal::backspace()
{
    Cursor& cur = cursor();
    if (cur.pendingWrap) {
        cur.pendingWrap = false;
        return;
    }
    const int left = cur.x >= m_margins.left ? m_margins.left : 0;
    if (cur.x > left) {
        --cur.x;
    }
}

void Terminal::horizontalTab(int count)
{
    Cursor& cur = cursor();
    const int right = cur.x <= m_margins.right ? m_margins.right : cols() - 1;
    while (count-- > 0 && cur.x < right) {
        ++cur.x;
        while (cur.x < right && !m_tabStops[std::size_t(cur.x)]) {
            ++cur.x;
        }
    }
    cur.pendingWrap = false;
}

void Terminal::backTab(int count)
{
    Cursor& cur = cursor();
    const int left = cur.x >= m_margins.left ? m_margins.left : 0;
    while (count-- > 0 && cur.x > left) {
        --cur.x;
        while (cur.x > left && !m_tabStops[std::size_t(cur.x)]) {
            --cur.x;
        }
    }
    cur.pendingWrap = false;
}

// --- cursor movement -------------------------------------------------------

void Terminal::setCursor(int row, int col)
{
    Cursor& cur = cursor();
    if (m_modes.originMode) {
        cur.y = std::clamp(m_margins.top + row, m_margins.top, m_margins.bottom);
        cur.x = std::clamp(m_margins.left + col, m_margins.left, m_margins.right);
    } else {
        cur.y = std::clamp(row, 0, rows() - 1);
        cur.x = std::clamp(col, 0, cols() - 1);
    }
    cur.pendingWrap = false;
}

void Terminal::moveCursorRelative(int dx, int dy)
{
    Cursor& cur = cursor();
    if (dy != 0) {
        // CUU/CUD stop at the margins when starting inside them.
        const int top = cur.y >= m_margins.top ? m_margins.top : 0;
        const int bottom = cur.y <= m_margins.bottom ? m_margins.bottom : rows() - 1;
        cur.y = std::clamp(cur.y + dy, top, bottom);
    }
    if (dx != 0) {
        const int left = cur.x >= m_margins.left ? m_margins.left : 0;
        const int right = cur.x <= m_margins.right ? m_margins.right : cols() - 1;
        cur.x = std::clamp(cur.x + dx, left, right);
    }
    cur.pendingWrap = false;
}

// --- erasing / editing -----------------------------------------------------

void Terminal::eraseDisplay(int mode)
{
    Screen& scr = *m_active;
    Cursor& cur = cursor();
    const std::uint32_t fill = acquireFill();
    switch (mode) {
    case 0:
        scr.eraseCells(scr.row(cur.y), cur.x, cols(), fill);
        scr.row(cur.y).wrapped = false;
        for (int y = cur.y + 1; y < rows(); ++y) {
            scr.eraseRow(scr.row(y), fill);
        }
        break;
    case 1:
        for (int y = 0; y < cur.y; ++y) {
            scr.eraseRow(scr.row(y), fill);
        }
        scr.eraseCells(scr.row(cur.y), 0, cur.x + 1, fill);
        break;
    case 2:
        for (int y = 0; y < rows(); ++y) {
            scr.eraseRow(scr.row(y), fill);
        }
        m_graphics[m_modes.altScreen ? 1 : 0].clearPlacements(scr, false);
        break;
    case 3:
        m_graphics[m_modes.altScreen ? 1 : 0].clearPlacements(scr, true);
        scr.clearHistory();
        break;
    default:
        break;
    }
    releaseFill(fill);
    cur.pendingWrap = false;
}

void Terminal::eraseLine(int mode)
{
    Screen& scr = *m_active;
    Cursor& cur = cursor();
    Row& row = scr.row(cur.y);
    const std::uint32_t fill = acquireFill();
    switch (mode) {
    case 0:
        scr.eraseCells(row, cur.x, cols(), fill);
        row.wrapped = false;
        break;
    case 1:
        scr.eraseCells(row, 0, cur.x + 1, fill);
        break;
    case 2:
        scr.eraseCells(row, 0, cols(), fill);
        row.wrapped = false;
        break;
    default:
        break;
    }
    releaseFill(fill);
    cur.pendingWrap = false;
}

void Terminal::insertLines(int n)
{
    Cursor& cur = cursor();
    if (cur.y < m_margins.top || cur.y > m_margins.bottom || cur.x < m_margins.left || cur.x > m_margins.right) {
        return;
    }
    Region r = m_margins;
    r.top = cur.y;
    const std::uint32_t fill = acquireFill();
    m_active->scrollDown(r, n, fill);
    releaseFill(fill);
    cur.x = m_margins.left;
    cur.pendingWrap = false;
}

void Terminal::deleteLines(int n)
{
    Cursor& cur = cursor();
    if (cur.y < m_margins.top || cur.y > m_margins.bottom || cur.x < m_margins.left || cur.x > m_margins.right) {
        return;
    }
    Region r = m_margins;
    r.top = cur.y;
    const std::uint32_t fill = acquireFill();
    m_active->scrollUp(r, n, fill);
    releaseFill(fill);
    cur.x = m_margins.left;
    cur.pendingWrap = false;
}

void Terminal::insertChars(int n)
{
    Cursor& cur = cursor();
    if (cur.x < m_margins.left || cur.x > m_margins.right) {
        return;
    }
    const std::uint32_t fill = acquireFill();
    m_active->insertCells(m_active->row(cur.y), cur.x, m_margins.right, n, fill);
    releaseFill(fill);
    cur.pendingWrap = false;
}

void Terminal::deleteChars(int n)
{
    Cursor& cur = cursor();
    if (cur.x < m_margins.left || cur.x > m_margins.right) {
        return;
    }
    const std::uint32_t fill = acquireFill();
    m_active->deleteCells(m_active->row(cur.y), cur.x, m_margins.right, n, fill);
    releaseFill(fill);
    cur.pendingWrap = false;
}

void Terminal::eraseChars(int n)
{
    Cursor& cur = cursor();
    const std::uint32_t fill = acquireFill();
    m_active->eraseCells(m_active->row(cur.y), cur.x, std::min(cols(), cur.x + n), fill);
    releaseFill(fill);
    cur.pendingWrap = false;
}

void Terminal::scrollUp(int n)
{
    const std::uint32_t fill = acquireFill();
    m_active->scrollUp(m_margins, n, fill);
    releaseFill(fill);
}

void Terminal::scrollDown(int n)
{
    const std::uint32_t fill = acquireFill();
    m_active->scrollDown(m_margins, n, fill);
    releaseFill(fill);
}

void Terminal::setTopBottomMargins(int top, int bottom)
{
    top = top <= 0 ? 0 : top - 1;
    bottom = (bottom <= 0 || bottom > rows()) ? rows() - 1 : bottom - 1;
    if (top >= bottom) {
        return;
    }
    m_margins.top = top;
    m_margins.bottom = bottom;
    setCursor(0, 0);
}

void Terminal::setLeftRightMargins(int left, int right)
{
    if (!m_modes.leftRightMargins) {
        return;
    }
    left = left <= 0 ? 0 : left - 1;
    right = (right <= 0 || right > cols()) ? cols() - 1 : right - 1;
    if (left >= right) {
        return;
    }
    m_margins.left = left;
    m_margins.right = right;
    setCursor(0, 0);
}

void Terminal::saveCursor()
{
    const Cursor& cur = cursor();
    SavedCursor& s = m_active->savedCursor;
    s.x = cur.x;
    s.y = cur.y;
    s.pendingWrap = cur.pendingWrap;
    s.originMode = m_modes.originMode;
    s.pen = cur.pen;
    std::copy(std::begin(m_charsets), std::end(m_charsets), std::begin(s.charsets));
    s.gl = m_gl;
    m_active->hasSavedCursor = true;
}

void Terminal::restoreCursor()
{
    Screen& scr = *m_active;
    if (!scr.hasSavedCursor) {
        m_modes.originMode = false;
        scr.setPen(Style{});
        setCursor(0, 0);
        return;
    }
    const SavedCursor& s = scr.savedCursor;
    scr.setPen(s.pen);
    m_modes.originMode = s.originMode;
    std::copy(std::begin(s.charsets), std::end(s.charsets), std::begin(m_charsets));
    m_gl = s.gl;
    Cursor& cur = cursor();
    cur.x = std::clamp(s.x, 0, cols() - 1);
    cur.y = std::clamp(s.y, 0, rows() - 1);
    cur.pendingWrap = s.pendingWrap;
}

void Terminal::setAltScreen(bool on, bool saveRestoreCursor, bool clear)
{
    if (on == m_modes.altScreen) {
        return;
    }
    if (on) {
        if (saveRestoreCursor) {
            saveCursor();
        }
        const Cursor primaryCursor = m_primary->cursor;
        m_active = m_alternate.get();
        m_modes.altScreen = true;
        syncKittyFlags();
        m_alternate->setPen(primaryCursor.pen);
        m_alternate->cursor.x = primaryCursor.x;
        m_alternate->cursor.y = primaryCursor.y;
        if (clear) {
            for (int y = 0; y < rows(); ++y) {
                m_alternate->eraseRow(m_alternate->row(y), StyleTable::DefaultId);
            }
        }
    } else {
        if (clear) {
            for (int y = 0; y < rows(); ++y) {
                m_alternate->eraseRow(m_alternate->row(y), StyleTable::DefaultId);
            }
        }
        m_active = m_primary.get();
        m_modes.altScreen = false;
        syncKittyFlags();
        if (saveRestoreCursor) {
            restoreCursor();
        }
    }
    m_lastX = -1;
}

void Terminal::fullReset()
{
    if (m_modes.altScreen) {
        setAltScreen(false, false, false);
    }
    m_modes = Modes{};
    m_modes.cursorBlink = m_options.cursorBlink;
    m_kittyStack[0].clear();
    m_kittyStack[1].clear();
    m_graphics[0].clear();
    m_graphics[1].clear();
    m_cursorShape = m_options.cursorShape;
    m_margins = Region{0, rows() - 1, 0, cols() - 1};
    std::fill(m_tabStops.begin(), m_tabStops.end(), false);
    for (std::size_t x = 0; x < m_tabStops.size(); x += 8) {
        m_tabStops[x] = true;
    }
    std::fill(std::begin(m_charsets), std::end(m_charsets), Charset::Ascii);
    m_gl = 0;
    m_palette = buildPalette(m_options.basePalette);
    m_defaultFg = m_options.defaultForeground;
    m_defaultBg = m_options.defaultBackground;
    m_hasCursorColor = false;
    m_titleStack.clear();
    m_semanticZone = SemanticZone::Output;
    m_shellRedrawsPrompt = true;
    m_promptRedrawPending = false;
    for (Screen* scr : {m_primary.get(), m_alternate.get()}) {
        scr->setPen(Style{});
        for (int y = 0; y < scr->rows(); ++y) {
            scr->eraseRow(scr->row(y), StyleTable::DefaultId);
        }
        scr->clearHistory();
        scr->cursor.x = scr->cursor.y = 0;
        scr->cursor.pendingWrap = false;
        scr->hasSavedCursor = false;
    }
    m_lastPrinted = 0;
    m_lastX = -1;
}

void Terminal::decaln()
{
    Screen& scr = *m_active;
    m_margins = Region{0, rows() - 1, 0, cols() - 1};
    for (int y = 0; y < rows(); ++y) {
        Row& row = scr.row(y);
        for (int x = 0; x < cols(); ++x) {
            scr.writeCell(row, x, U'E', StyleTable::DefaultId, 1);
        }
        row.wrapped = false;
    }
    setCursor(0, 0);
}

// --- ESC -------------------------------------------------------------------

void Terminal::escDispatch(const Intermediates& inter, std::uint8_t final)
{
    m_lastX = -1;
    if (inter.count == 1) {
        const char i = inter.bytes[0];
        if (i == '(' || i == ')' || i == '*' || i == '+') {
            const int slot = i == '(' ? 0 : i == ')' ? 1 : i == '*' ? 2 : 3;
            m_charsets[slot] = final == '0' ? Charset::DecSpecialGraphics : Charset::Ascii;
        } else if (i == '#' && final == '8') {
            decaln();
        }
        return;
    }
    if (inter.count != 0) {
        return;
    }
    switch (final) {
    case '7':
        saveCursor();
        break;
    case '8':
        restoreCursor();
        break;
    case 'D':
        index();
        break;
    case 'E':
        index();
        carriageReturn();
        break;
    case 'H':
        m_tabStops[std::size_t(cursor().x)] = true;
        break;
    case 'M':
        reverseIndex();
        break;
    case 'c':
        fullReset();
        break;
    case '=':
        m_modes.keypadApp = true;
        break;
    case '>':
        m_modes.keypadApp = false;
        break;
    case 'n':
        m_gl = 2;
        break;
    case 'o':
        m_gl = 3;
        break;
    default:
        break;
    }
}

// --- CSI -------------------------------------------------------------------

void Terminal::csiDispatch(const Params& p, const Intermediates& inter, std::uint8_t final)
{
    m_lastX = -1;
    const bool priv = inter.marker == '?';

    if (inter.marker == 0 && inter.count == 0) {
        switch (final) {
        case '@':
            insertChars(p.atOr(0, 1));
            return;
        case 'A':
            moveCursorRelative(0, -p.atOr(0, 1));
            return;
        case 'B':
        case 'e':
            moveCursorRelative(0, p.atOr(0, 1));
            return;
        case 'C':
        case 'a':
            moveCursorRelative(p.atOr(0, 1), 0);
            return;
        case 'D':
            moveCursorRelative(-p.atOr(0, 1), 0);
            return;
        case 'E':
            moveCursorRelative(0, p.atOr(0, 1));
            carriageReturn();
            return;
        case 'F':
            moveCursorRelative(0, -p.atOr(0, 1));
            carriageReturn();
            return;
        case 'G':
        case '`': {
            const int col = p.atOr(0, 1) - 1;
            Cursor& cur = cursor();
            cur.x = std::clamp((m_modes.originMode ? m_margins.left : 0) + col, 0, cols() - 1);
            cur.pendingWrap = false;
            return;
        }
        case 'H':
        case 'f':
            setCursor(p.atOr(0, 1) - 1, p.atOr(1, 1) - 1);
            return;
        case 'I':
            horizontalTab(p.atOr(0, 1));
            return;
        case 'J':
            eraseDisplay(p.at(0));
            return;
        case 'K':
            eraseLine(p.at(0));
            return;
        case 'L':
            insertLines(p.atOr(0, 1));
            return;
        case 'M':
            deleteLines(p.atOr(0, 1));
            return;
        case 'P':
            deleteChars(p.atOr(0, 1));
            return;
        case 'S':
            scrollUp(p.atOr(0, 1));
            return;
        case 'T':
            scrollDown(p.atOr(0, 1));
            return;
        case 'X':
            eraseChars(p.atOr(0, 1));
            return;
        case 'Z':
            backTab(p.atOr(0, 1));
            return;
        case 'b':
            repeatLast(p.atOr(0, 1));
            return;
        case 'c':
            if (p.at(0) == 0) {
                reply("\x1b[?62;22c");
            }
            return;
        case 'd': {
            const int row = p.atOr(0, 1) - 1;
            Cursor& cur = cursor();
            cur.y = std::clamp((m_modes.originMode ? m_margins.top : 0) + row, 0, rows() - 1);
            cur.pendingWrap = false;
            return;
        }
        case 'g':
            if (p.at(0) == 0) {
                m_tabStops[std::size_t(cursor().x)] = false;
            } else if (p.at(0) == 3) {
                std::fill(m_tabStops.begin(), m_tabStops.end(), false);
            }
            return;
        case 'h':
            setMode(p, false, true);
            return;
        case 'l':
            setMode(p, false, false);
            return;
        case 'm':
            selectGraphicRendition(p);
            return;
        case 'n':
            deviceStatusReport(p, false);
            return;
        case 'r':
            setTopBottomMargins(p.at(0), p.at(1));
            return;
        case 's':
            if (m_modes.leftRightMargins) {
                setLeftRightMargins(p.at(0), p.at(1));
            } else {
                saveCursor();
            }
            return;
        case 't':
            windowOp(p);
            return;
        case 'u':
            restoreCursor();
            return;
        default:
            return;
        }
    }

    if (final == 'u' && inter.count == 0 && (inter.marker == '?' || inter.marker == '>' || inter.marker == '<'
                                              || inter.marker == '=')) {
        kittyKeyboard(p, inter.marker);
        return;
    }

    if (priv && inter.count == 0) {
        switch (final) {
        case 'h':
            setMode(p, true, true);
            return;
        case 'l':
            setMode(p, true, false);
            return;
        case 'n':
            deviceStatusReport(p, true);
            return;
        case 'J':
            eraseDisplay(p.at(0));
            return;
        case 'K':
            eraseLine(p.at(0));
            return;
        default:
            return;
        }
    }

    if (inter.marker == '>' && inter.count == 0) {
        if (final == 'c' && p.at(0) == 0) {
            reply("\x1b[>1;10;0c");
        } else if (final == 'q' && p.at(0) == 0) {
            std::string r = "\x1bP>|afterglow ";
            r += Version;
            r += "\x1b\\";
            reply(r);
        }
        return;
    }

    if (inter.count == 1 && inter.bytes[0] == ' ' && inter.marker == 0 && final == 'q') {
        setCursorStyle(p.at(0));
        return;
    }

    if (inter.count == 1 && inter.bytes[0] == '$' && final == 'p') {
        reportMode(p, priv);
        return;
    }

    if (inter.count == 1 && inter.bytes[0] == '!' && inter.marker == 0 && final == 'p') {
        // DECSTR soft reset.
        m_modes.insert = false;
        m_modes.originMode = false;
        m_modes.autowrap = true;
        m_modes.cursorVisible = true;
        m_modes.cursorKeysApp = false;
        m_modes.keypadApp = false;
        m_margins = Region{0, rows() - 1, 0, cols() - 1};
        std::fill(std::begin(m_charsets), std::end(m_charsets), Charset::Ascii);
        m_gl = 0;
        m_active->setPen(Style{});
        m_active->hasSavedCursor = false;
        return;
    }
}

void Terminal::kittyKeyboard(const Params& p, char marker)
{
    std::vector<std::uint8_t>& stack = m_kittyStack[m_modes.altScreen ? 1 : 0];
    constexpr std::size_t MaxDepth = 64;
    switch (marker) {
    case '?': {
        char buf[16];
        const int n = std::snprintf(buf, sizeof buf, "\x1b[?%du", int(m_modes.kittyKeyboard));
        reply(std::string_view(buf, std::size_t(n)));
        return;
    }
    case '>':
        if (stack.size() >= MaxDepth) {
            stack.erase(stack.begin()); // the oldest entry falls off
        }
        stack.push_back(std::uint8_t(p.at(0) & KittyFlag::All));
        break;
    case '<': {
        const std::size_t n = std::min<std::size_t>(p.atOr(0, 1), stack.size());
        stack.resize(stack.size() - n);
        break;
    }
    case '=': {
        const auto flags = std::uint8_t(p.at(0) & KittyFlag::All);
        if (stack.empty()) {
            stack.push_back(0);
        }
        switch (p.atOr(1, 1)) {
        case 1:
            stack.back() = flags;
            break;
        case 2:
            stack.back() |= flags;
            break;
        case 3:
            stack.back() &= std::uint8_t(~flags);
            break;
        default:
            break;
        }
        break;
    }
    default:
        return;
    }
    syncKittyFlags();
}

void Terminal::syncKittyFlags()
{
    const std::vector<std::uint8_t>& stack = m_kittyStack[m_modes.altScreen ? 1 : 0];
    m_modes.kittyKeyboard = stack.empty() ? 0 : stack.back();
}

void Terminal::setCursorStyle(int ps)
{
    switch (ps) {
    case 0: // the user's configured cursor
        m_cursorShape = m_options.cursorShape;
        m_modes.cursorBlink = m_options.cursorBlink;
        break;
    case 1:
        m_cursorShape = CursorShape::Block;
        m_modes.cursorBlink = true;
        break;
    case 2:
        m_cursorShape = CursorShape::Block;
        m_modes.cursorBlink = false;
        break;
    case 3:
        m_cursorShape = CursorShape::Underline;
        m_modes.cursorBlink = true;
        break;
    case 4:
        m_cursorShape = CursorShape::Underline;
        m_modes.cursorBlink = false;
        break;
    case 5:
        m_cursorShape = CursorShape::Bar;
        m_modes.cursorBlink = true;
        break;
    case 6:
        m_cursorShape = CursorShape::Bar;
        m_modes.cursorBlink = false;
        break;
    default:
        break;
    }
}

// --- SGR -------------------------------------------------------------------

void Terminal::selectGraphicRendition(const Params& p)
{
    Style s = cursor().pen;
    // A hyperlink is not a graphic rendition: SGR resets leave it in place.
    const std::uint32_t link = s.link;
    if (p.count == 0) {
        s = Style{};
    }

    // Reads an extended colour (38/48/58) starting at index i; returns the
    // index of the last parameter consumed.
    auto extendedColor = [&p](int i, Color& out) -> int {
        if (p.isSub(i + 1)) {
            int end = i + 1;
            while (p.isSub(end + 1)) {
                ++end;
            }
            const int subs = end - i;
            const int kind = p.at(i + 1);
            if (kind == 5 && subs >= 2) {
                out = Color::palette(std::uint8_t(p.at(i + 2)));
            } else if (kind == 2 && subs >= 5) {
                out = Color::rgb(std::uint8_t(p.at(i + 3)), std::uint8_t(p.at(i + 4)), std::uint8_t(p.at(i + 5)));
            } else if (kind == 2 && subs == 4) {
                out = Color::rgb(std::uint8_t(p.at(i + 2)), std::uint8_t(p.at(i + 3)), std::uint8_t(p.at(i + 4)));
            }
            return end;
        }
        const int kind = p.at(i + 1);
        if (kind == 5 && i + 2 < p.count) {
            out = Color::palette(std::uint8_t(p.at(i + 2)));
            return i + 2;
        }
        if (kind == 2 && i + 4 < p.count) {
            out = Color::rgb(std::uint8_t(p.at(i + 2)), std::uint8_t(p.at(i + 3)), std::uint8_t(p.at(i + 4)));
            return i + 4;
        }
        return p.count;
    };

    for (int i = 0; i < p.count; ++i) {
        const int v = p.values[i];
        switch (v) {
        case 0:
            s = Style{};
            break;
        case 1:
            s.attrs |= Attr::Bold;
            break;
        case 2:
            s.attrs |= Attr::Faint;
            break;
        case 3:
            s.attrs |= Attr::Italic;
            break;
        case 4:
            if (p.isSub(i + 1)) {
                const int kind = p.at(++i);
                s.underline = kind <= 5 ? UnderlineStyle(kind) : UnderlineStyle::Single;
                while (p.isSub(i + 1)) {
                    ++i;
                }
            } else {
                s.underline = UnderlineStyle::Single;
            }
            break;
        case 5:
        case 6:
            s.attrs |= Attr::Blink;
            break;
        case 7:
            s.attrs |= Attr::Inverse;
            break;
        case 8:
            s.attrs |= Attr::Invisible;
            break;
        case 9:
            s.attrs |= Attr::Strikethrough;
            break;
        case 21:
            s.underline = UnderlineStyle::Double;
            break;
        case 22:
            s.attrs &= std::uint16_t(~(Attr::Bold | Attr::Faint));
            break;
        case 23:
            s.attrs &= std::uint16_t(~Attr::Italic);
            break;
        case 24:
            s.underline = UnderlineStyle::None;
            break;
        case 25:
            s.attrs &= std::uint16_t(~Attr::Blink);
            break;
        case 27:
            s.attrs &= std::uint16_t(~Attr::Inverse);
            break;
        case 28:
            s.attrs &= std::uint16_t(~Attr::Invisible);
            break;
        case 29:
            s.attrs &= std::uint16_t(~Attr::Strikethrough);
            break;
        case 38:
            i = extendedColor(i, s.fg);
            break;
        case 39:
            s.fg = Color{};
            break;
        case 48:
            i = extendedColor(i, s.bg);
            break;
        case 49:
            s.bg = Color{};
            break;
        case 53:
            s.attrs |= Attr::Overline;
            break;
        case 55:
            s.attrs &= std::uint16_t(~Attr::Overline);
            break;
        case 58:
            i = extendedColor(i, s.underlineColor);
            break;
        case 59:
            s.underlineColor = Color{};
            break;
        default:
            if (v >= 30 && v <= 37) {
                s.fg = Color::palette(std::uint8_t(v - 30));
            } else if (v >= 40 && v <= 47) {
                s.bg = Color::palette(std::uint8_t(v - 40));
            } else if (v >= 90 && v <= 97) {
                s.fg = Color::palette(std::uint8_t(v - 90 + 8));
            } else if (v >= 100 && v <= 107) {
                s.bg = Color::palette(std::uint8_t(v - 100 + 8));
            }
            break;
        }
        // Skip stray sub-parameters of codes that take none.
        while (p.isSub(i + 1)) {
            ++i;
        }
    }
    s.link = link;
    m_active->setPen(s);
}

// --- modes -----------------------------------------------------------------

void Terminal::setMode(const Params& p, bool priv, bool on)
{
    for (int i = 0; i < p.count; ++i) {
        const int mode = p.values[i];
        if (priv) {
            setDecMode(mode, on);
        } else if (mode == 4) {
            m_modes.insert = on;
        } else if (mode == 20) {
            m_modes.newline = on;
        }
    }
}

void Terminal::setDecMode(int mode, bool on)
{
    switch (mode) {
    case 1:
        m_modes.cursorKeysApp = on;
        break;
    case 5:
        m_modes.reverseVideo = on;
        break;
    case 6:
        m_modes.originMode = on;
        setCursor(0, 0);
        break;
    case 7:
        m_modes.autowrap = on;
        break;
    case 9:
        m_modes.mouseTracking = on ? MouseTracking::X10 : MouseTracking::None;
        break;
    case 12:
        m_modes.cursorBlink = on;
        break;
    case 25:
        m_modes.cursorVisible = on;
        break;
    case 47:
        setAltScreen(on, false, false);
        break;
    case 69:
        m_modes.leftRightMargins = on;
        if (!on) {
            m_margins.left = 0;
            m_margins.right = cols() - 1;
        }
        break;
    case 1000:
        m_modes.mouseTracking = on ? MouseTracking::Normal : MouseTracking::None;
        break;
    case 1002:
        m_modes.mouseTracking = on ? MouseTracking::Button : MouseTracking::None;
        break;
    case 1003:
        m_modes.mouseTracking = on ? MouseTracking::Any : MouseTracking::None;
        break;
    case 1004:
        m_modes.focusEvents = on;
        break;
    case 1005:
        m_modes.mouseFormat = on ? MouseFormat::Utf8 : MouseFormat::X10;
        break;
    case 1006:
        m_modes.mouseFormat = on ? MouseFormat::Sgr : MouseFormat::X10;
        break;
    case 1007:
        m_modes.altScroll = on;
        break;
    case 1015:
        m_modes.mouseFormat = on ? MouseFormat::Urxvt : MouseFormat::X10;
        break;
    case 1016:
        m_modes.mouseFormat = on ? MouseFormat::SgrPixels : MouseFormat::X10;
        break;
    case 1047:
        setAltScreen(on, false, !on);
        break;
    case 1048:
        if (on) {
            saveCursor();
        } else {
            restoreCursor();
        }
        break;
    case 1049:
        setAltScreen(on, true, true);
        break;
    case 2004:
        m_modes.bracketedPaste = on;
        break;
    case 2026:
        m_modes.synchronizedOutput = on;
        break;
    case 2027:
        m_modes.graphemeClustering = on;
        break;
    default:
        break;
    }
}

int Terminal::decModeState(int mode) const
{
    auto b = [](bool v) { return v ? 1 : 2; };
    switch (mode) {
    case 1:
        return b(m_modes.cursorKeysApp);
    case 5:
        return b(m_modes.reverseVideo);
    case 6:
        return b(m_modes.originMode);
    case 7:
        return b(m_modes.autowrap);
    case 9:
        return b(m_modes.mouseTracking == MouseTracking::X10);
    case 12:
        return b(m_modes.cursorBlink);
    case 25:
        return b(m_modes.cursorVisible);
    case 47:
    case 1047:
    case 1049:
        return b(m_modes.altScreen);
    case 69:
        return b(m_modes.leftRightMargins);
    case 1000:
        return b(m_modes.mouseTracking == MouseTracking::Normal);
    case 1002:
        return b(m_modes.mouseTracking == MouseTracking::Button);
    case 1003:
        return b(m_modes.mouseTracking == MouseTracking::Any);
    case 1004:
        return b(m_modes.focusEvents);
    case 1005:
        return b(m_modes.mouseFormat == MouseFormat::Utf8);
    case 1006:
        return b(m_modes.mouseFormat == MouseFormat::Sgr);
    case 1007:
        return b(m_modes.altScroll);
    case 1015:
        return b(m_modes.mouseFormat == MouseFormat::Urxvt);
    case 1016:
        return b(m_modes.mouseFormat == MouseFormat::SgrPixels);
    case 2004:
        return b(m_modes.bracketedPaste);
    case 2026:
        return b(m_modes.synchronizedOutput);
    case 2027:
        return b(m_modes.graphemeClustering);
    default:
        return 0;
    }
}

int Terminal::ansiModeState(int mode) const
{
    switch (mode) {
    case 4:
        return m_modes.insert ? 1 : 2;
    case 20:
        return m_modes.newline ? 1 : 2;
    default:
        return 0;
    }
}

void Terminal::reportMode(const Params& p, bool priv)
{
    const int mode = p.at(0);
    const int state = priv ? decModeState(mode) : ansiModeState(mode);
    char buf[48];
    const int n = std::snprintf(buf, sizeof buf, priv ? "\x1b[?%d;%d$y" : "\x1b[%d;%d$y", mode, state);
    reply(std::string_view(buf, std::size_t(n)));
}

void Terminal::deviceStatusReport(const Params& p, bool priv)
{
    char buf[48];
    switch (p.at(0)) {
    case 5:
        if (!priv) {
            reply("\x1b[0n");
        }
        break;
    case 6: {
        const Cursor& cur = cursor();
        const int y = m_modes.originMode ? cur.y - m_margins.top : cur.y;
        const int x = m_modes.originMode ? cur.x - m_margins.left : cur.x;
        const int n = std::snprintf(buf, sizeof buf, priv ? "\x1b[?%d;%dR" : "\x1b[%d;%dR", y + 1, x + 1);
        reply(std::string_view(buf, std::size_t(n)));
        break;
    }
    default:
        break;
    }
}

void Terminal::windowOp(const Params& p)
{
    char buf[64];
    int n = 0;
    switch (p.at(0)) {
    case 14:
        n = std::snprintf(buf, sizeof buf, "\x1b[4;%d;%dt", rows() * m_cellHeight, cols() * m_cellWidth);
        break;
    case 16:
        n = std::snprintf(buf, sizeof buf, "\x1b[6;%d;%dt", m_cellHeight, m_cellWidth);
        break;
    case 18:
        n = std::snprintf(buf, sizeof buf, "\x1b[8;%d;%dt", rows(), cols());
        break;
    case 22:
        if (m_titleStack.size() < 64) {
            m_titleStack.push_back(m_title);
        }
        return;
    case 23:
        if (!m_titleStack.empty()) {
            m_title = std::move(m_titleStack.back());
            m_titleStack.pop_back();
            m_host.titleChanged(m_title);
        }
        return;
    default:
        return;
    }
    reply(std::string_view(buf, std::size_t(n)));
}

// --- OSC / DCS / APC -------------------------------------------------------

void Terminal::oscDispatch(std::string_view payload, bool bel)
{
    m_lastX = -1;
    const auto semi = payload.find(';');
    const std::string_view numStr = payload.substr(0, semi);
    const std::string_view rest = semi == std::string_view::npos ? std::string_view{} : payload.substr(semi + 1);
    int num = -1;
    if (std::from_chars(numStr.data(), numStr.data() + numStr.size(), num).ec != std::errc{}) {
        return;
    }

    switch (num) {
    case 0:
    case 2:
        m_title.assign(rest);
        m_host.titleChanged(m_title);
        break;
    case 4:
        oscPalette(rest, bel);
        break;
    case 7:
        m_cwd.assign(rest);
        m_host.workingDirectoryChanged(m_cwd);
        break;
    case 10:
    case 11:
    case 12:
        oscColor(num, rest, bel);
        break;
    case 9:
        // iTerm2 notification. "9;4;..." is ConEmu's progress report, not a message.
        if (!rest.starts_with("4;") && !rest.empty()) {
            m_host.notify({}, rest);
        }
        break;
    case 777: {
        // urxvt: 777;notify;title;body
        if (rest.starts_with("notify;")) {
            const std::string_view args = rest.substr(7);
            const auto semi2 = args.find(';');
            m_host.notify(args.substr(0, semi2), semi2 == std::string_view::npos ? std::string_view{} : args.substr(semi2 + 1));
        }
        break;
    }
    case 22:
        m_host.pointerShapeChanged(rest);
        break;
    case 8: {
        // OSC 8 ; params ; URI. An empty URI ends the link.
        const auto semi2 = rest.find(';');
        if (semi2 == std::string_view::npos) {
            break;
        }
        const std::string_view params = rest.substr(0, semi2);
        const std::string_view uri = rest.substr(semi2 + 1);
        std::string_view id;
        for (std::size_t pos = 0; pos <= params.size();) {
            const auto colon = params.find(':', pos);
            const std::string_view kv = params.substr(pos, colon == std::string_view::npos ? colon : colon - pos);
            if (kv.starts_with("id=")) {
                id = kv.substr(3);
            }
            if (colon == std::string_view::npos) {
                break;
            }
            pos = colon + 1;
        }
        Style pen = cursor().pen;
        pen.link = uri.empty() ? 0 : m_active->styles().internLink(uri, id);
        m_active->setPen(pen);
        break;
    }
    case 52: {
        const auto s = rest.find(';');
        if (s == std::string_view::npos) {
            break;
        }
        const std::string_view data = rest.substr(s + 1);
        // Clipboard reads ("?") are not answered: a remote program must not
        // be able to read the local clipboard.
        if (data != "?") {
            m_host.clipboardWrite(rest.substr(0, s), data);
        }
        break;
    }
    case 133:
        semanticPrompt(rest);
        break;
    case 104:
        if (rest.empty()) {
            const auto fresh = buildPalette(m_options.basePalette);
            m_palette = fresh;
        } else {
            const auto fresh = buildPalette(m_options.basePalette);
            std::size_t pos = 0;
            while (pos <= rest.size()) {
                const auto next = rest.find(';', pos);
                const std::string_view item = rest.substr(pos, next - pos);
                int idx = -1;
                if (std::from_chars(item.data(), item.data() + item.size(), idx).ec == std::errc{} && idx >= 0
                    && idx < 256) {
                    m_palette[std::size_t(idx)] = fresh[std::size_t(idx)];
                }
                if (next == std::string_view::npos) {
                    break;
                }
                pos = next + 1;
            }
        }
        break;
    case 110:
        m_defaultFg = m_options.defaultForeground;
        break;
    case 111:
        m_defaultBg = m_options.defaultBackground;
        break;
    case 112:
        m_hasCursorColor = false;
        break;
    default:
        break;
    }
}

void Terminal::semanticPrompt(std::string_view args)
{
    // OSC 133 ; <kind> [; key=value ...]
    const char kind = args.empty() ? '\0' : args[0];
    std::size_t pos = args.find(';');
    while (pos != std::string_view::npos) {
        const auto next = args.find(';', pos + 1);
        const std::string_view opt = args.substr(pos + 1, next == std::string_view::npos ? next : next - pos - 1);
        if (opt == "redraw=0") {
            m_shellRedrawsPrompt = false;
        } else if (opt == "redraw=1") {
            m_shellRedrawsPrompt = true;
        }
        pos = next;
    }
    switch (kind) {
    case 'A': // prompt start
    case 'N': // new command: prompt start after implicitly finishing the last one
        m_semanticZone = SemanticZone::Prompt;
        if (!m_modes.altScreen) {
            m_active->row(cursor().y).promptStart = true;
        }
        break;
    case 'P': // continuation prompt line
        m_semanticZone = SemanticZone::Prompt;
        break;
    case 'B': // prompt end, user input begins
        m_semanticZone = SemanticZone::Input;
        m_promptRedrawPending = false;
        break;
    case 'C': // command output begins
    case 'D': // command finished
        m_semanticZone = SemanticZone::Output;
        m_promptRedrawPending = false;
        break;
    default:
        break;
    }
}

std::optional<std::size_t> Terminal::redrawablePromptStart() const
{
    if (!m_shellRedrawsPrompt || m_semanticZone == SemanticZone::Output) {
        return std::nullopt;
    }
    // Only a prompt start on screen counts. A shell that sends A but never C
    // would otherwise leave a stale mark far up whose rows we must not blank.
    const Screen& scr = *m_primary;
    for (int y = scr.cursor.y; y >= 0; --y) {
        if (scr.row(y).promptStart) {
            return scr.historySize() + std::size_t(y);
        }
    }
    return std::nullopt;
}

void Terminal::oscColor(int which, std::string_view spec, bool bel)
{
    // OSC 10;fg;bg;cursor: each further argument targets the next colour.
    std::size_t pos = 0;
    for (int target = which; target <= 12 && pos <= spec.size(); ++target) {
        const auto next = spec.find(';', pos);
        const std::string_view item = spec.substr(pos, next - pos);
        Rgb* dst = target == 10 ? &m_defaultFg : target == 11 ? &m_defaultBg : &m_cursorColor;
        if (item == "?") {
            const Rgb value = target == 12 && !m_hasCursorColor ? m_defaultFg : *dst;
            char prefix[8];
            const int n = std::snprintf(prefix, sizeof prefix, "%d;", target);
            reply(formatColorReply(std::string_view(prefix, std::size_t(n)), value, bel));
        } else if (Rgb c; parseColorSpec(item, c)) {
            *dst = c;
            if (target == 12) {
                m_hasCursorColor = true;
            }
        }
        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1;
    }
}

void Terminal::oscPalette(std::string_view args, bool bel)
{
    std::size_t pos = 0;
    while (pos < args.size()) {
        const auto s1 = args.find(';', pos);
        if (s1 == std::string_view::npos) {
            return;
        }
        const auto s2 = args.find(';', s1 + 1);
        const std::string_view idxStr = args.substr(pos, s1 - pos);
        const std::string_view spec = args.substr(s1 + 1, s2 == std::string_view::npos ? s2 : s2 - s1 - 1);
        int idx = -1;
        if (std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx).ec == std::errc{} && idx >= 0
            && idx < 256) {
            if (spec == "?") {
                char prefix[16];
                const int n = std::snprintf(prefix, sizeof prefix, "4;%d;", idx);
                reply(formatColorReply(std::string_view(prefix, std::size_t(n)), m_palette[std::size_t(idx)], bel));
            } else if (Rgb c; parseColorSpec(spec, c)) {
                m_palette[std::size_t(idx)] = c;
            }
        }
        if (s2 == std::string_view::npos) {
            return;
        }
        pos = s2 + 1;
    }
}

namespace {

std::string toHex(std::string_view s)
{
    static constexpr char Digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out.push_back(Digits[c >> 4]);
        out.push_back(Digits[c & 15]);
    }
    return out;
}

std::string fromHex(std::string_view s)
{
    std::string out;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = hexDigit(s[i]);
        const int lo = hexDigit(s[i + 1]);
        if (hi < 0 || lo < 0) {
            return {};
        }
        out.push_back(char(hi * 16 + lo));
    }
    return out;
}

/// Capabilities answered by XTGETTCAP, as terminfo would hold them (ESC as
/// a byte). An empty value marks a boolean capability.
const std::pair<std::string_view, std::string_view>* findCapability(std::string_view name)
{
    static const std::pair<std::string_view, std::string_view> Caps[] = {
        {"TN", "afterglow"},
        {"name", "afterglow"},
        {"Co", "256"},
        {"colors", "256"},
        {"RGB", "8/8/8"},
        {"Tc", ""},
        {"Su", ""},
        {"XT", ""},
        {"Smulx", "\x1b[4:%p1%dm"},
        {"Setulc", "\x1b[58:2::%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:%p1%{255}%&%d%;m"},
        {"Ss", "\x1b[%p1%d q"},
        {"Se", "\x1b[2 q"},
        {"Ms", "\x1b]52;%p1%s;%p2%s\x1b\\"},
        {"Sync", "\x1b[?2026%?%p1%{1}%-%tl%eh%;"},
        {"BE", "\x1b[?2004h"},
        {"BD", "\x1b[?2004l"},
        {"PS", "\x1b[200~"},
        {"PE", "\x1b[201~"},
        {"fe", "\x1b[?1004h"},
        {"fd", "\x1b[?1004l"},
        {"kxIN", "\x1b[I"},
        {"kxOUT", "\x1b[O"},
        {"setrgbf", "\x1b[38:2::%p1%d:%p2%d:%p3%dm"},
        {"setrgbb", "\x1b[48:2::%p1%d:%p2%d:%p3%dm"},
    };
    for (const auto& cap : Caps) {
        if (cap.first == name) {
            return &cap;
        }
    }
    return nullptr;
}

void appendColor(std::string& out, AfterglowVt::Color c, int base, int extended)
{
    using Kind = AfterglowVt::Color::Kind;
    switch (c.kind()) {
    case Kind::Default:
        return;
    case Kind::Palette:
        if (base != 0 && c.index() < 8) {
            out += ';' + std::to_string(base + c.index());
        } else if (base != 0 && c.index() < 16) {
            out += ';' + std::to_string(base + 60 + c.index() - 8);
        } else {
            out += ';' + std::to_string(extended) + ":5:" + std::to_string(c.index());
        }
        return;
    case Kind::Rgb:
        out += ';' + std::to_string(extended) + ":2::" + std::to_string(c.r()) + ':' + std::to_string(c.g()) + ':'
            + std::to_string(c.b());
        return;
    }
}

} // namespace

void Terminal::dcsDispatch(const Params&, const Intermediates& inter, std::uint8_t final, std::string_view data)
{
    m_lastX = -1;
    if (inter.count != 1 || final != 'q') {
        return;
    }
    if (inter.bytes[0] == '+') {
        // XTGETTCAP: each requested name gets its own reply.
        std::size_t pos = 0;
        while (pos <= data.size()) {
            const auto semi = data.find(';', pos);
            const std::string_view hexName = data.substr(pos, semi == std::string_view::npos ? semi : semi - pos);
            const auto* cap = findCapability(fromHex(hexName));
            std::string r;
            if (cap) {
                r = "\x1bP1+r" + std::string(hexName);
                if (!cap->second.empty()) {
                    r += '=' + toHex(cap->second);
                }
            } else {
                r = "\x1bP0+r" + std::string(hexName);
            }
            r += "\x1b\\";
            reply(r);
            if (semi == std::string_view::npos) {
                break;
            }
            pos = semi + 1;
        }
        return;
    }
    if (inter.bytes[0] == '$') {
        // DECRQSS: report a setting as the sequence that would set it.
        std::string value;
        if (data == "m") {
            const Style& st = cursor().pen;
            value = "0";
            static constexpr std::pair<std::uint16_t, int> Flags[] = {
                {Attr::Bold, 1}, {Attr::Faint, 2}, {Attr::Italic, 3}, {Attr::Blink, 5},
                {Attr::Inverse, 7}, {Attr::Invisible, 8}, {Attr::Strikethrough, 9}, {Attr::Overline, 53},
            };
            for (const auto& [flag, code] : Flags) {
                if (st.has(flag)) {
                    value += ';' + std::to_string(code);
                }
            }
            if (st.underline != UnderlineStyle::None) {
                value += st.underline == UnderlineStyle::Single ? std::string(";4")
                                                                : ";4:" + std::to_string(int(st.underline));
            }
            appendColor(value, st.fg, 30, 38);
            appendColor(value, st.bg, 40, 48);
            appendColor(value, st.underlineColor, 0, 58);
            value += 'm';
        } else if (data == "r") {
            value = std::to_string(m_margins.top + 1) + ';' + std::to_string(m_margins.bottom + 1) + 'r';
        } else if (data == "s") {
            value = std::to_string(m_margins.left + 1) + ';' + std::to_string(m_margins.right + 1) + 's';
        } else if (data == " q") {
            const int base = m_cursorShape == CursorShape::Block ? 1 : m_cursorShape == CursorShape::Underline ? 3 : 5;
            value = std::to_string(base + (m_modes.cursorBlink ? 0 : 1)) + " q";
        }
        reply(value.empty() ? std::string("\x1bP0$r\x1b\\") : "\x1bP1$r" + value + "\x1b\\");
    }
}

void Terminal::apcDispatch(std::string_view payload)
{
    m_lastX = -1;
    if (payload.empty() || payload[0] != 'G') {
        return;
    }
    GraphicsStore::Context ctx{
        *m_active,
        m_host,
        m_cellWidth,
        m_cellHeight,
        [this](std::string_view bytes) { reply(bytes); },
        [this] { index(); },
    };
    m_graphics[m_modes.altScreen ? 1 : 0].handle(payload.substr(1), ctx);
}

std::uint64_t Terminal::advanceAnimations(std::uint64_t nowMs)
{
    std::uint64_t next = 0;
    if (m_graphics[m_modes.altScreen ? 1 : 0].advanceAnimations(nowMs, next)) {
        touch();
    }
    return next;
}

} // namespace AfterglowVt
