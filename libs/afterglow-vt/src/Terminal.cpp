// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Input.h>
#include <AfterglowVt/Terminal.h>
#include <AfterglowVt/Sixel.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

/// One "#" component: per XParseColor its digits are the high-order bits,
/// so "#fff" is f0/f0/f0, not white.
bool parseHashComponent(std::string_view s, std::uint8_t& out)
{
    unsigned v = 0;
    for (char c : s) {
        const int d = hexDigit(c);
        if (d < 0) {
            return false;
        }
        v = v * 16 + unsigned(d);
    }
    const int bits = 4 * int(s.size());
    out = std::uint8_t(bits >= 8 ? v >> (bits - 8) : v << (8 - bits));
    return true;
}

/// An "rgbi:" intensity in [0, 1].
bool parseIntensity(std::string_view s, std::uint8_t& out)
{
    const std::string text(s);
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size() || !(v >= 0.0 && v <= 1.0)) {
        return false;
    }
    out = std::uint8_t(std::lround(v * 255.0));
    return true;
}

/// X11 colour spec: "rgb:r/g/b", "rgbi:r/g/b" or "#rgb" / "#rrggbb" /
/// "#rrrgggbbb" / "#rrrrggggbbbb". (The device-independent CIE and TekHVC
/// spaces of Xcms are not supported.)
bool parseColorSpec(std::string_view spec, Rgb& out)
{
    if (spec.starts_with("rgbi:")) {
        spec.remove_prefix(5);
        const auto s1 = spec.find('/');
        const auto s2 = s1 == std::string_view::npos ? s1 : spec.find('/', s1 + 1);
        if (s2 == std::string_view::npos) {
            return false;
        }
        return parseIntensity(spec.substr(0, s1), out.r) && parseIntensity(spec.substr(s1 + 1, s2 - s1 - 1), out.g)
            && parseIntensity(spec.substr(s2 + 1), out.b);
    }
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
        return parseHashComponent(spec.substr(0, n), out.r) && parseHashComponent(spec.substr(n, n), out.g)
            && parseHashComponent(spec.substr(2 * n, n), out.b);
    }
    return false;
}

void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
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
    if (m_modes.themeReports) {
        reportTheme(false); // OSC 11 and friends may have changed the background
    }
}

bool Terminal::backgroundIsDark() const noexcept
{
    const double luma = 0.2126 * m_defaultBg.r + 0.7152 * m_defaultBg.g + 0.0722 * m_defaultBg.b;
    return luma < 128.0;
}

void Terminal::reportTheme(bool force)
{
    // CSI ? 997 ; 1 (dark) | 2 (light) n, judged by the default background.
    const int dark = backgroundIsDark() ? 1 : 0;
    if (!force && dark == m_reportedDark) {
        return;
    }
    m_reportedDark = dark;
    reply(dark ? "\x1b[?997;1n" : "\x1b[?997;2n");
}

void Terminal::reportVisibility()
{
    reply(m_visible ? "\x1b[?999;1n" : "\x1b[?999;2n");
}

void Terminal::setVisible(bool visible)
{
    if (visible == m_visible) {
        return;
    }
    m_visible = visible;
    if (m_modes.visibilityReports) {
        reportVisibility();
    }
}

void Terminal::reportSize()
{
    char buf[64];
    const int n = std::snprintf(buf, sizeof buf, "\x1b[48;%d;%d;%d;%dt", rows(), cols(), rows() * m_cellHeight,
                                cols() * m_cellWidth);
    reply(std::string_view(buf, std::size_t(n)));
}

void Terminal::changeRectAttributes(const Params& p, bool reverse)
{
    // DECCARA (set) / DECRARA (toggle): Pt;Pl;Pb;Pr;Ps... over a rectangle,
    // or with DECSACE in stream mode, the text from (Pt,Pl) to (Pb,Pr).
    int top, left, bottom, right;
    if (!rectFromParams(p, 0, top, left, bottom, right, m_attributeChangeExtent != 2)) {
        return;
    }
    std::uint16_t setAttrs = 0;
    std::uint16_t clearAttrs = 0;
    bool setUnderline = false;
    bool clearUnderline = false;
    for (int i = 4; i < std::max(p.count, 5); ++i) {
        switch (p.at(i)) {
        case 0:
            if (reverse) {
                setAttrs |= Attr::Bold | Attr::Blink | Attr::Inverse;
                setUnderline = true;
            } else {
                clearAttrs |= Attr::Bold | Attr::Blink | Attr::Inverse;
                clearUnderline = true;
            }
            break;
        case 1: setAttrs |= Attr::Bold; break;
        case 4: setUnderline = true; break;
        case 5: setAttrs |= Attr::Blink; break;
        case 7: setAttrs |= Attr::Inverse; break;
        case 22: if (!reverse) clearAttrs |= Attr::Bold; break;
        case 24: if (!reverse) clearUnderline = true; break;
        case 25: if (!reverse) clearAttrs |= Attr::Blink; break;
        case 27: if (!reverse) clearAttrs |= Attr::Inverse; break;
        default: break;
        }
    }
    StyleTable& styles = m_active->styles();
    auto apply = [&](int y, int x) {
        Cell& c = m_active->row(y).cells[std::size_t(x)];
        Style st = styles.get(c.styleId());
        if (reverse) {
            st.attrs ^= setAttrs;
            if (setUnderline) {
                st.underline = st.underline == UnderlineStyle::None ? UnderlineStyle::Single : UnderlineStyle::None;
            }
        } else {
            st.attrs = std::uint16_t((st.attrs | setAttrs) & ~clearAttrs);
            if (setUnderline) {
                st.underline = UnderlineStyle::Single;
            }
            if (clearUnderline) {
                st.underline = UnderlineStyle::None;
            }
        }
        const std::uint32_t id = styles.acquire(st);
        styles.release(c.styleId());
        c.setStyleId(id);
    };
    for (int y = top; y <= bottom; ++y) {
        int x0 = left;
        int x1 = right;
        if (m_attributeChangeExtent != 2) { // stream: whole lines in between
            x0 = y == top ? left : 0;
            x1 = y == bottom ? right : cols() - 1;
        }
        for (int x = x0; x <= x1; ++x) {
            apply(y, x);
        }
    }
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
    if (m_modes.themeReports) {
        reportTheme(false);
    }
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
    if (m_modes.sizeReports) {
        reportSize();
    }
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

    if (m_vt52Address > 0) {
        // VT52 ESC Y: row, then column, each offset by 32. A coordinate
        // off the screen leaves that one unchanged (xterm).
        if (m_vt52Address-- == 2) {
            m_vt52Row = int(cp) - 32;
        } else {
            const int col = int(cp) - 32;
            if (m_vt52Row >= 0 && m_vt52Row < rows()) {
                cur.y = m_vt52Row;
            }
            if (col >= 0 && col < cols()) {
                cur.x = col;
            }
            cur.pendingWrap = false;
        }
        return;
    }

    if (cp < 0x80) {
        const Charset set = m_charsets[m_singleShift ? m_singleShift : m_gl];
        m_singleShift = 0;
        switch (set) {
        case Charset::DecSpecialGraphics:
            if (cp >= 0x5F && cp <= 0x7E) {
                cp = DecSpecialGraphics[cp - 0x5F];
            }
            break;
        case Charset::British:
            if (cp == '#') {
                cp = U'\u00A3';
            }
            break;
        case Charset::Latin1Supplemental:
            if (cp >= 0x20) {
                cp += 0x80;
            }
            break;
        case Charset::Ascii:
            break;
        }
    } else {
        m_singleShift = 0;
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

    const int right = lineRight((cur.x <= m_margins.right) ? m_margins.right : scr.cols() - 1);
    // Text sizing: writing into a multicell block (kitty's rules). The top
    // left cell erases it, the rest of its top row turns it into spaces, and
    // in its lower rows the text goes after the block.
    while (scr.row(cur.y).cells[std::size_t(cur.x)].isMulticell()) {
        Row& row = scr.row(cur.y);
        const Cell& c = row.cells[std::size_t(cur.x)];
        if (c.multicellDy() == 0) {
            scr.eraseMulticell(cur.y, cur.x, c.multicellDx() != 0);
            break;
        }
        const MulticellParams* p = scr.multicellParams(row, cur.x);
        const int next = cur.x - c.multicellDx() + (p ? p->columns() : 1);
        if (next <= right) {
            cur.x = next;
        } else if (m_modes.autowrap) {
            row.wrapped = true;
            carriageReturn();
            index();
        } else {
            scr.eraseMulticell(cur.y, cur.x);
            break;
        }
    }
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

void Terminal::printAscii(const char* text, std::size_t length)
{
    // Charsets and insert mode change what printing does: take the full path.
    if (m_charsets[m_gl] != Charset::Ascii || m_singleShift != 0 || m_modes.insert || m_vt52Address > 0) {
        for (std::size_t i = 0; i < length; ++i) {
            print(char32_t(static_cast<unsigned char>(text[i])));
        }
        return;
    }
    // The first character might join the previous cluster (after a Prepend,
    // say). An ASCII character after an ASCII one never joins anything, so
    // the rest skip segmentation and go straight into the row.
    print(char32_t(static_cast<unsigned char>(text[0])));
    Screen& scr = *m_active;
    StyleTable& styles = scr.styles();
    Cursor& cur = cursor();
    std::size_t i = 1;
    while (i < length) {
        if (cur.pendingWrap || scr.row(cur.y).cells[std::size_t(cur.x)].isMulticell()
            || scr.row(cur.y).lineSize != LineSize::Single) {
            // Wrapping, multicell blocks and double-width lines are the full path's job.
            print(char32_t(static_cast<unsigned char>(text[i++])));
            continue;
        }
        const int right = (cur.x <= m_margins.right) ? m_margins.right : scr.cols() - 1;
        Row& row = scr.row(cur.y);
        const std::uint32_t pen = cur.penId;
        while (i < length) {
            const auto ch = char32_t(static_cast<unsigned char>(text[i]));
            Cell& c = row.cells[std::size_t(cur.x)];
            if (c.isMulticell()) {
                break;
            }
            if (c.width() != 1 || c.hasGrapheme() || c.isSpacerHead()) {
                scr.writeCell(row, cur.x, ch, pen, 1); // breaks up a wide character
            } else {
                if (c.styleId() != pen) {
                    styles.release(c.styleId());
                    styles.retain(pen);
                    c.setStyleId(pen);
                }
                c.setCodepoint(ch);
            }
            m_lastX = cur.x;
            m_lastY = cur.y;
            ++i;
            if (cur.x >= right) {
                cur.pendingWrap = true;
                break;
            }
            ++cur.x;
        }
    }
    const auto last = char32_t(static_cast<unsigned char>(text[length - 1]));
    m_lastPrinted = last;
    if (length > 1 && m_modes.graphemeClustering) {
        m_segmenter.reset();
        m_segmenter.breaksBefore(Unicode::props(last));
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
    clampToLine();
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
    clampToLine();
}

void Terminal::backspace()
{
    Cursor& cur = cursor();
    const bool reverse = m_modes.autowrap && (m_modes.reverseWrap || m_modes.reverseWrapExtended);
    if (cur.pendingWrap) {
        cur.pendingWrap = false;
        if (reverse) {
            return; // undoes the pending wrap, as xterm does
        }
    }
    const int left = cur.x >= m_margins.left ? m_margins.left : 0;
    if (cur.x > left) {
        --cur.x;
        return;
    }
    if (reverse) {
        reverseWrapStep();
    }
}

void Terminal::reverseWrapStep()
{
    Cursor& cur = cursor();
    // Reverse wrap: to the right margin of the line above. Mode 45 only
    // follows soft wraps (editing a long wrapped line); 1045 crosses any
    // line and wraps from the top margin to the bottom one.
    const int top = cur.y >= m_margins.top ? m_margins.top : 0;
    const int bottom = cur.y <= m_margins.bottom ? m_margins.bottom : rows() - 1;
    if (cur.y > top) {
        if (m_modes.reverseWrapExtended || m_active->row(cur.y - 1).wrapped) {
            --cur.y;
            cur.x = m_margins.right;
        }
    } else if (m_modes.reverseWrapExtended) {
        cur.y = bottom;
        cur.x = m_margins.right;
    }
}

void Terminal::horizontalTab(int count)
{
    Cursor& cur = cursor();
    if (cur.pendingWrap) {
        if (!m_modes.moreFix || !m_modes.autowrap) {
            return; // the wrap stays pending (xterm)
        }
        // Mode 41: wrap first, so the tab lands on the next line.
        m_active->row(cur.y).wrapped = true;
        carriageReturn();
        index();
    }
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
    // Unlike forward tabs, CBT is not bounded by the left margin (xterm).
    Cursor& cur = cursor();
    while (count-- > 0 && cur.x > 0) {
        --cur.x;
        while (cur.x > 0 && !m_tabStops[std::size_t(cur.x)]) {
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
    clampToLine();
}

int Terminal::lineRight(int right) const
{
    if (m_active->row(m_active->cursor.y).lineSize == LineSize::Single) {
        return right;
    }
    return std::min(right, std::max(1, cols() / 2) - 1);
}

void Terminal::clampToLine()
{
    Cursor& cur = cursor();
    cur.x = std::min(cur.x, lineRight(cols() - 1));
}

void Terminal::setLineSize(LineSize size)
{
    // DECDWL / DECDHL / DECSWL. Blocks on the line cannot be doubled.
    Row& row = m_active->row(cursor().y);
    if (row.multicell) {
        for (int x = 0; x < cols(); ++x) {
            if (row.cells[std::size_t(x)].isMulticell()) {
                m_active->eraseMulticell(cursor().y, x);
            }
        }
    }
    row.lineSize = size;
    clampToLine();
}

void Terminal::textSizing(std::string_view args)
{
    // kitty's OSC 66 ; metadata ; text.
    const auto semi = args.find(';');
    if (semi == std::string_view::npos) {
        return;
    }
    const std::string_view metadata = args.substr(0, semi);
    const std::string_view text = args.substr(semi + 1);
    if (text.size() > 4096) {
        return;
    }
    int s = 1, w = 0, n = 0, d = 0, v = 0, h = 0;
    std::size_t pos = 0;
    while (pos <= metadata.size()) {
        const auto colon = metadata.find(':', pos);
        std::string_view kv = metadata.substr(pos, colon == std::string_view::npos ? colon : colon - pos);
        while (!kv.empty() && kv.front() == ' ') {
            kv.remove_prefix(1);
        }
        while (!kv.empty() && kv.back() == ' ') {
            kv.remove_suffix(1);
        }
        if (kv.size() >= 3 && kv[1] == '=') {
            int value = -1;
            std::from_chars(kv.data() + 2, kv.data() + kv.size(), value);
            auto set = [&](int& target, int lo, int hi) {
                if (value >= lo && value <= hi) {
                    target = value;
                }
            };
            switch (kv[0]) {
            case 's': set(s, 1, 7); break;
            case 'w': set(w, 0, 7); break;
            case 'n': set(n, 0, 15); break;
            case 'd': set(d, 0, 15); break;
            case 'v': set(v, 0, 2); break;
            case 'h': set(h, 0, 2); break;
            default: break;
            }
        }
        if (colon == std::string_view::npos) {
            break;
        }
        pos = colon + 1;
    }
    if (d != 0 && n >= d) {
        n = d = 0; // the fraction must be below one
    }

    // Decode the text; ill-formed sequences become U+FFFD, controls and NUL go.
    std::u32string cps;
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        const int extra = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
        char32_t cp = Unicode::ReplacementCharacter;
        int used = 1;
        if (extra >= 0 && i + std::size_t(extra) < text.size() + (extra == 0 ? 1 : 0)) {
            char32_t value = extra == 0 ? b : b & (0x3F >> extra);
            bool ok = true;
            for (int k = 1; k <= extra; ++k) {
                if (i + std::size_t(k) >= text.size()) {
                    ok = false;
                    break;
                }
                const auto c = static_cast<unsigned char>(text[i + std::size_t(k)]);
                if ((c & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
                value = (value << 6) | (c & 0x3F);
            }
            if (ok && value <= 0x10FFFF && !(value >= 0xD800 && value <= 0xDFFF)) {
                cp = value;
                used = extra + 1;
            }
        }
        i += std::size_t(used);
        if (cp >= 0x20 && cp != 0x7F && !(cp >= 0x80 && cp < 0xA0)) {
            cps.push_back(cp);
        }
    }
    if (cps.empty()) {
        cps.push_back(U' '); // "w=2;" with no text: a space in the block
    }

    if (s == 1 && w == 0 && d == 0) {
        for (char32_t cp : cps) { // nothing to size: ordinary text
            print(cp);
        }
        return;
    }
    MulticellParams params{std::uint8_t(s), 1, std::uint8_t(n), std::uint8_t(d), std::uint8_t(v), std::uint8_t(h)};
    if (w > 0) {
        // Everything in one block of s*w by s cells.
        params.width = std::uint8_t(w);
        placeMulticell(cps[0], std::u32string_view(cps).substr(1), params);
    } else {
        // Cell by cell: each grapheme cluster gets a block as wide as it is.
        Unicode::GraphemeSegmenter seg;
        std::size_t start = 0;
        auto flush = [&](std::size_t end) {
            if (end <= start) {
                return;
            }
            const Unicode::Props props = Unicode::props(cps[start]);
            int width = props.width;
            const std::u32string_view cluster = std::u32string_view(cps).substr(start, end - start);
            if (width == 1 && props.widensWithVs16 && cluster.find(Unicode::VariationSelector16) != std::u32string_view::npos) {
                width = 2;
            }
            if (width > 0) {
                params.width = std::uint8_t(width);
                placeMulticell(cluster[0], cluster.substr(1), params);
            }
        };
        for (std::size_t i = 0; i < cps.size(); ++i) {
            if (seg.breaksBefore(Unicode::props(cps[i])) && i > start) {
                flush(i);
                start = i;
            }
        }
        flush(cps.size());
    }
    m_lastX = -1;
}

void Terminal::placeMulticell(char32_t cp, std::u32string_view extra, const MulticellParams& params)
{
    Cursor& cur = cursor();
    const int width = params.columns();
    const int height = params.scale;
    if (width > cols() || height > rows()) {
        return; // never fits: discarded
    }
    if (cur.pendingWrap) {
        if (m_modes.autowrap) {
            m_active->row(cur.y).wrapped = true;
            carriageReturn();
            index();
        }
        cur.pendingWrap = false;
    }
    const int right = cur.x <= m_margins.right ? m_margins.right : cols() - 1;
    const int left = cur.x >= m_margins.left ? m_margins.left : 0;
    if (cur.x + width - 1 > right) {
        if (m_modes.autowrap && right - left + 1 >= width) {
            m_active->row(cur.y).wrapped = true;
            carriageReturn();
            index();
        } else {
            cur.x = std::max(0, right - width + 1); // back up until it fits
        }
    }
    // Rows below the cursor for the block, scrolling if it would overhang.
    const bool inRegion = cur.y >= m_margins.top && cur.y <= m_margins.bottom;
    const int top = inRegion ? m_margins.top : 0;
    const int bottom = inRegion ? m_margins.bottom : rows() - 1;
    if (bottom - top + 1 < height) {
        return;
    }
    while (cur.y + height - 1 > bottom) {
        Region r = inRegion ? m_margins : Region{0, rows() - 1, 0, cols() - 1};
        const std::uint32_t fill = acquireFill();
        m_active->scrollUp(r, 1, fill, r.top == 0 && r.left == 0 && r.right == cols() - 1);
        releaseFill(fill);
        --cur.y;
    }
    m_active->writeMulticell(cur.y, cur.x, cp, extra, cur.penId, params);
    cur.x += width;
    if (cur.x > right) {
        cur.x = right;
        cur.pendingWrap = true;
    }
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
    clampToLine();
}

// --- erasing / editing -----------------------------------------------------

bool Terminal::spares(bool selective) const noexcept
{
    // DEC protection only stops the selective erases; ISO protection only
    // stops the ordinary ones.
    return selective ? m_protection == Protection::Dec : m_protection == Protection::Iso;
}

void Terminal::eraseSpan(Row& row, int x0, int x1, std::uint32_t fill, bool selective)
{
    Screen& scr = *m_active;
    if (!spares(selective)) {
        scr.eraseCells(row, x0, x1, fill);
        return;
    }
    // Erase the runs of unprotected cells between protected ones.
    const StyleTable& styles = scr.styles();
    x1 = std::min(x1, cols());
    int x = std::max(x0, 0);
    while (x < x1) {
        while (x < x1 && styles.get(row.cells[std::size_t(x)].styleId()).has(Attr::Protected)) {
            ++x;
        }
        const int start = x;
        while (x < x1 && !styles.get(row.cells[std::size_t(x)].styleId()).has(Attr::Protected)) {
            ++x;
        }
        if (x > start) {
            scr.eraseCells(row, start, x, fill);
        }
    }
}

void Terminal::eraseWholeRow(Row& row, std::uint32_t fill, bool selective)
{
    // Clears the row's flags too (wrap, prompt marks) unless cells survive.
    if (spares(selective)) {
        eraseSpan(row, 0, cols(), fill, selective);
    } else {
        m_active->eraseRow(row, fill);
    }
}

void Terminal::setProtection(bool on, bool iso)
{
    Style pen = cursor().pen;
    pen.attrs = std::uint16_t(on ? (pen.attrs | Attr::Protected) : (pen.attrs & ~Attr::Protected));
    m_active->setPen(pen);
    if (on) {
        m_protection = iso ? Protection::Iso : Protection::Dec;
    }
}

void Terminal::eraseDisplay(int mode, bool selective)
{
    Screen& scr = *m_active;
    Cursor& cur = cursor();
    const std::uint32_t fill = acquireFill();
    switch (mode) {
    case 0:
        eraseSpan(scr.row(cur.y), cur.x, cols(), fill, selective);
        scr.row(cur.y).wrapped = false;
        for (int y = cur.y + 1; y < rows(); ++y) {
            eraseWholeRow(scr.row(y), fill, selective);
        }
        break;
    case 1:
        for (int y = 0; y < cur.y; ++y) {
            eraseWholeRow(scr.row(y), fill, selective);
        }
        eraseSpan(scr.row(cur.y), 0, cur.x + 1, fill, selective);
        break;
    case 2:
        for (int y = 0; y < rows(); ++y) {
            eraseWholeRow(scr.row(y), fill, selective);
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

void Terminal::eraseLine(int mode, bool selective)
{
    Screen& scr = *m_active;
    Cursor& cur = cursor();
    Row& row = scr.row(cur.y);
    const std::uint32_t fill = acquireFill();
    switch (mode) {
    case 0:
        eraseSpan(row, cur.x, cols(), fill, selective);
        row.wrapped = false;
        break;
    case 1:
        eraseSpan(row, 0, cur.x + 1, fill, selective);
        break;
    case 2:
        eraseSpan(row, 0, cols(), fill, selective);
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

void Terminal::insertColumns(int n)
{
    // DECIC: columns inserted at the cursor in every line of the scroll region.
    Cursor& cur = cursor();
    if (cur.x < m_margins.left || cur.x > m_margins.right || cur.y < m_margins.top || cur.y > m_margins.bottom) {
        return;
    }
    const std::uint32_t fill = acquireFill();
    for (int y = m_margins.top; y <= m_margins.bottom; ++y) {
        m_active->insertCells(m_active->row(y), cur.x, m_margins.right, n, fill);
    }
    releaseFill(fill);
    cur.pendingWrap = false;
}

void Terminal::deleteColumns(int n)
{
    // DECDC: columns deleted at the cursor in every line of the scroll region.
    Cursor& cur = cursor();
    if (cur.x < m_margins.left || cur.x > m_margins.right || cur.y < m_margins.top || cur.y > m_margins.bottom) {
        return;
    }
    const std::uint32_t fill = acquireFill();
    for (int y = m_margins.top; y <= m_margins.bottom; ++y) {
        m_active->deleteCells(m_active->row(y), cur.x, m_margins.right, n, fill);
    }
    releaseFill(fill);
    cur.pendingWrap = false;
}

void Terminal::backIndex()
{
    // DECBI: left one column; at the left margin the region scrolls right.
    Cursor& cur = cursor();
    cur.pendingWrap = false;
    if (cur.x == m_margins.left && cur.y >= m_margins.top && cur.y <= m_margins.bottom) {
        const std::uint32_t fill = acquireFill();
        for (int y = m_margins.top; y <= m_margins.bottom; ++y) {
            m_active->insertCells(m_active->row(y), m_margins.left, m_margins.right, 1, fill);
        }
        releaseFill(fill);
    } else if (cur.x > 0) {
        --cur.x;
    }
}

void Terminal::forwardIndex()
{
    // DECFI: right one column; at the right margin the region scrolls left.
    Cursor& cur = cursor();
    cur.pendingWrap = false;
    if (cur.x == m_margins.right && cur.y >= m_margins.top && cur.y <= m_margins.bottom) {
        const std::uint32_t fill = acquireFill();
        for (int y = m_margins.top; y <= m_margins.bottom; ++y) {
            m_active->deleteCells(m_active->row(y), m_margins.left, m_margins.right, 1, fill);
        }
        releaseFill(fill);
    } else if (cur.x < cols() - 1) {
        ++cur.x;
    }
}

void Terminal::eraseChars(int n)
{
    Cursor& cur = cursor();
    const std::uint32_t fill = acquireFill();
    eraseSpan(m_active->row(cur.y), cur.x, std::min(cols(), cur.x + n), fill, false);
    releaseFill(fill);
    cur.pendingWrap = false;
}

bool Terminal::rectFromParams(const Params& p, int first, int& top, int& left, int& bottom, int& right, bool stream) const
{
    // Pt ; Pl ; Pb ; Pr starting at @p first: 1-based, relative to the
    // margins in origin mode, defaulting to the whole page, clipped to it.
    const bool origin = m_modes.originMode;
    const int top0 = origin ? m_margins.top : 0;
    const int left0 = origin ? m_margins.left : 0;
    const int bottomMax = origin ? m_margins.bottom : rows() - 1;
    const int rightMax = origin ? m_margins.right : cols() - 1;
    top = top0 + p.atOr(first, 1) - 1;
    left = left0 + p.atOr(first + 1, 1) - 1;
    bottom = p.at(first + 2) == 0 ? bottomMax : std::min(bottomMax, top0 + p.at(first + 2) - 1);
    right = p.at(first + 3) == 0 ? rightMax : std::min(rightMax, left0 + p.at(first + 3) - 1);
    return top <= bottom && (left <= right || (stream && top < bottom)) && top <= bottomMax && left <= rightMax;
}

void Terminal::fillRect(const Params& p)
{
    // DECFRA: Pch ; Pt ; Pl ; Pb ; Pr $ x, in the current rendition.
    const int ch = p.at(0);
    if (!((ch >= 32 && ch <= 126) || (ch >= 160 && ch <= 255))) {
        return;
    }
    int top, left, bottom, right;
    if (!rectFromParams(p, 1, top, left, bottom, right)) {
        return;
    }
    const std::uint32_t pen = cursor().penId;
    for (int y = top; y <= bottom; ++y) {
        Row& row = m_active->row(y);
        for (int x = left; x <= right; ++x) {
            m_active->writeCell(row, x, char32_t(ch), pen, 1);
        }
    }
}

void Terminal::eraseRect(const Params& p, bool selective)
{
    // DECERA / DECSERA: Pt ; Pl ; Pb ; Pr $ z / $ {
    int top, left, bottom, right;
    if (!rectFromParams(p, 0, top, left, bottom, right)) {
        return;
    }
    const std::uint32_t fill = selective ? StyleTable::DefaultId : acquireFill();
    for (int y = top; y <= bottom; ++y) {
        eraseSpan(m_active->row(y), left, right + 1, fill, selective);
    }
    if (!selective) {
        releaseFill(fill);
    }
}

void Terminal::copyRect(const Params& p)
{
    // DECCRA: Pts ; Pls ; Pbs ; Prs ; Pps ; Ptd ; Pld ; Ppd $ v. One page,
    // so the page numbers are ignored. Copied through a buffer, so source
    // and destination may overlap.
    int top, left, bottom, right;
    if (!rectFromParams(p, 0, top, left, bottom, right)) {
        return;
    }
    const bool origin = m_modes.originMode;
    const int destTop = (origin ? m_margins.top : 0) + p.atOr(5, 1) - 1;
    const int destLeft = (origin ? m_margins.left : 0) + p.atOr(6, 1) - 1;
    const int bottomMax = origin ? m_margins.bottom : rows() - 1;
    const int rightMax = origin ? m_margins.right : cols() - 1;

    struct Copied
    {
        char32_t cp;
        std::uint32_t style;
        int width;
        std::u32string extra;
    };
    const int w = right - left + 1;
    const int h = bottom - top + 1;
    std::vector<Copied> buffer;
    buffer.reserve(std::size_t(w) * std::size_t(h));
    StyleTable& styles = m_active->styles();
    for (int y = top; y <= bottom; ++y) {
        const Row& row = m_active->row(y);
        for (int x = left; x <= right; ++x) {
            const Cell& c = row.cells[std::size_t(x)];
            Copied cell{c.codepoint(), c.styleId(), c.width(), std::u32string(m_active->graphemeExtra(row, x))};
            // Halves of wide characters cut by the rectangle become blanks.
            if (c.isTail() ? x == left : (c.width() == 2 && x == right)) {
                cell = Copied{0, c.styleId(), 1, {}};
            }
            styles.retain(cell.style); // the source may be overwritten before we write
            buffer.push_back(std::move(cell));
        }
    }
    for (int dy = 0; dy < h; ++dy) {
        const int y = destTop + dy;
        for (int dx = 0; dx < w; ++dx) {
            const int x = destLeft + dx;
            const Copied& c = buffer[std::size_t(dy) * std::size_t(w) + std::size_t(dx)];
            if (y <= bottomMax && x <= rightMax && c.width != 0) {
                Row& row = m_active->row(y);
                const int width = (c.width == 2 && x + 1 > rightMax) ? 1 : c.width;
                m_active->writeCell(row, x, c.width == 2 && width == 1 ? char32_t(0) : c.cp, c.style, width);
                for (char32_t e : c.extra) {
                    m_active->appendGrapheme(row, x, e);
                }
            }
            styles.release(c.style);
        }
    }
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
        } else {
            // Without 1049's save and restore the screens share one cursor.
            m_primary->cursor.x = m_alternate->cursor.x;
            m_primary->cursor.y = m_alternate->cursor.y;
        }
    }
    m_lastX = -1;
}

void Terminal::fullReset()
{
    if (m_modes.altScreen) {
        setAltScreen(false, false, false);
    }
    if (m_modes.columns132 && cols() != 80 && m_host.requestResize(80, rows())) {
        resize(80, rows(), m_cellWidth, m_cellHeight);
    }
    m_modes = Modes{};
    m_modes.cursorBlink = m_options.cursorBlink;
    m_conformance = 5;
    m_savedDecModes.clear();
    m_inputStart = m_outputStart = m_outputEnd = {};
    m_commandRunning = false;
    m_hasSelectionFg = m_hasSelectionBg = false;
    m_reportedDark = -1;
    m_colorStack.clear();
    m_vt52Address = 0;
    m_protection = Protection::Off;
    m_attributeChangeExtent = 0;
    m_kittyStack[0].clear();
    m_kittyStack[1].clear();
    m_notifications.reset();
    m_clipboard.reset();
    if (!m_programStatus.records().empty()) {
        m_programStatus.reset();
        m_host.programStatusChanged();
    } else {
        m_programStatus.reset();
    }
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
    m_singleShift = 0;
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
        row.lineSize = LineSize::Single;
    }
    setCursor(0, 0);
}

// --- ESC -------------------------------------------------------------------

void Terminal::escDispatch(const Intermediates& inter, std::uint8_t final)
{
    m_lastX = -1;
    if (m_modes.vt52) {
        if (inter.count == 0) {
            vt52Dispatch(final);
        }
        return;
    }
    if (inter.count == 1) {
        const char i = inter.bytes[0];
        if (i == '(' || i == ')' || i == '*' || i == '+') {
            // 94-character sets into G0-G3.
            const int slot = i == '(' ? 0 : i == ')' ? 1 : i == '*' ? 2 : 3;
            m_charsets[slot] = final == '0' ? Charset::DecSpecialGraphics
                : final == 'A'              ? Charset::British
                                            : Charset::Ascii;
        } else if (i == '-' || i == '.' || i == '/') {
            // 96-character sets into G1-G3; ISO Latin-1 is the one there is.
            const int slot = i == '-' ? 1 : i == '.' ? 2 : 3;
            if (final == 'A') {
                m_charsets[slot] = Charset::Latin1Supplemental;
            }
        } else if (i == '#' && final == '8') {
            decaln();
        } else if (i == '#' && final >= '3' && final <= '6') {
            setLineSize(final == '3'   ? LineSize::DoubleTop
                        : final == '4' ? LineSize::DoubleBottom
                        : final == '6' ? LineSize::DoubleWidth
                                       : LineSize::Single);
        }
        return;
    }
    if (inter.count != 0) {
        return;
    }
    switch (final) {
    case '6':
        backIndex();
        break;
    case '7':
        saveCursor();
        break;
    case '8':
        restoreCursor();
        break;
    case '9':
        forwardIndex();
        break;
    case 'Z': // DECID
        reportDeviceAttributes();
        break;
    case 'V': // SPA: start of ISO-protected area
        setProtection(true, true);
        break;
    case 'W': // EPA
        setProtection(false, true);
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
    case 'N': // SS2
        m_singleShift = 2;
        break;
    case 'O': // SS3
        m_singleShift = 3;
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
            if (m_modes.autowrap && (m_modes.reverseWrap || m_modes.reverseWrapExtended)) {
                // With reverse wrap, CUB steps back like that many backspaces.
                for (int n = p.atOr(0, 1); n > 0; --n) {
                    backspace();
                }
            } else {
                moveCursorRelative(-p.atOr(0, 1), 0);
            }
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
                reportDeviceAttributes();
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
            if (m_modes.leftRightMargins && m_conformance >= 4) {
                setLeftRightMargins(p.at(0), p.at(1));
            } else {
                saveCursor();
            }
            return;
        case 't':
            windowOp(p);
            return;
        case 'x': // DECREQTPARM: no parity, 8 bits, 38400 baud, clock 1
            if (p.at(0) <= 1) {
                char buf[32];
                const int n = std::snprintf(buf, sizeof buf, "\x1b[%d;1;1;128;128;1;0x", p.at(0) + 2);
                reply(std::string_view(buf, std::size_t(n)));
            }
            return;
        case 'u':
            restoreCursor();
            return;
        default:
            return;
        }
    }

    if (inter.marker == '=' && inter.count == 0 && final == 'c' && p.at(0) == 0) {
        reply("\x1bP!|00000000\x1b\\"); // DA3: unit ID
        return;
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
        case 's':
            saveDecModes(p);
            return;
        case 'S': { // XTSMGRAPHICS: Pi (1 colours, 2 sixel geometry) ; Pa (1 read, 2 reset, 3 set, 4 max)
            char buf[48];
            int n = 0;
            if (p.at(0) == 1) {
                n = std::snprintf(buf, sizeof buf, "\x1b[?1;0;1024S");
            } else if (p.at(0) == 2) {
                n = std::snprintf(buf, sizeof buf, "\x1b[?2;0;%d;%dS", std::max(1, cols() * m_cellWidth),
                                  std::max(1, rows() * m_cellHeight));
            } else {
                n = std::snprintf(buf, sizeof buf, "\x1b[?%d;1;0S", int(p.at(0))); // ReGIS: not supported
            }
            reply(std::string_view(buf, std::size_t(n)));
            return;
        }
        case 'm': { // XTQMODKEYS
            const int resource = p.at(0);
            const int value = resource == 4 ? m_modes.modifyOtherKeys : resource == 0 ? 0 : 2;
            if (resource <= 2 || resource == 4) {
                char buf[32];
                const int n = std::snprintf(buf, sizeof buf, "\x1b[>%d;%dm", resource, value);
                reply(std::string_view(buf, std::size_t(n)));
            }
            return;
        }
        case 'r':
            restoreDecModes(p);
            return;
        case 'J':
            eraseDisplay(p.at(0), true);
            return;
        case 'K':
            eraseLine(p.at(0), true);
            return;
        default:
            return;
        }
    }

    if (inter.count == 1 && inter.marker == 0) {
        const char i = inter.bytes[0];
        if (i == '"' && final == 'q') { // DECSCA
            setProtection(p.at(0) == 1, false);
            return;
        }
        if (i == '$') {
            switch (final) {
            case 'v':
                copyRect(p);
                return;
            case 'r':
                changeRectAttributes(p, false);
                return;
            case 't':
                changeRectAttributes(p, true);
                return;
            case 'x':
                fillRect(p);
                return;
            case 'z':
                eraseRect(p, false);
                return;
            case '{':
                eraseRect(p, true);
                return;
            default:
                break;
            }
        }
        if (i == '#' && (final == 'P' || final == 'Q' || final == 'R')) {
            if (final == 'P') { // XTPUSHCOLORS: push, or store in slot 1-10
                pushColors(p.at(0));
            } else if (final == 'Q') { // XTPOPCOLORS: pop, or restore slot 1-10
                popColors(p.at(0));
            } else { // XTREPORTCOLORS: current level and how many are stored
                char buf[32];
                const int n = std::snprintf(buf, sizeof buf, "\x1b[%zu;%zu#Q", m_colorStack.size(), m_colorStack.size());
                reply(std::string_view(buf, std::size_t(n)));
            }
            return;
        }
        if (i == '$' && final == 'w') { // DECRQPSR: cursor information or tab stops
            reportPresentationState(p.at(0));
            return;
        }
        if (i == '"' && final == 'v') { // DECRQDE: the displayed extent of the page
            char buf[48];
            const int n = std::snprintf(buf, sizeof buf, "\x1b[%d;%d;1;1;1\"w", rows(), cols());
            reply(std::string_view(buf, std::size_t(n)));
            return;
        }
        if (i == '&' && final == 'u') { // DECRQUPSS: the user-preferred supplemental set is ISO Latin-1
            reply("\x1bP1!uA\x1b\\");
            return;
        }
        if (i == '$' && final == '|') { // DECSCPP: 80 or 132 columns per page
            const int width = p.at(0) == 132 ? 132 : 80;
            if (width != cols() && m_host.requestResize(width, rows())) {
                resize(width, rows(), m_cellWidth, m_cellHeight);
            }
            return;
        }
        if (i == '*' && final == '|') { // DECSNLS: lines per screen
            const int lines = p.at(0);
            if (lines >= 1 && lines != rows() && m_host.requestResize(cols(), lines)) {
                resize(cols(), lines, m_cellWidth, m_cellHeight);
            }
            return;
        }
        if (i == '*' && final == 'x') { // DECSACE
            m_attributeChangeExtent = p.at(0) == 2 ? 2 : 0;
            return;
        }
    }

    if (inter.marker == '>' && inter.count == 0 && (final == 'm' || final == 'n')) {
        // XTMODKEYS (CSI > 4 ; Pv m) and its reset (CSI > 4 n); only
        // modifyOtherKeys is settable, cursor and function keys always carry
        // their modifiers.
        if (p.at(0) == 4) {
            m_modes.modifyOtherKeys = final == 'm' ? std::uint8_t(std::min<int>(p.at(1), 2)) : 0;
        }
        return;
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
        if (m_conformance >= 3) { // DECRQM came with the VT300s
            reportMode(p, priv);
        }
        return;
    }

    if (inter.count == 1 && inter.bytes[0] == '"' && inter.marker == 0 && final == 'p') {
        // DECSCL: conformance level 61-65; a soft reset comes with it.
        const int level = p.atOr(0, 65);
        if (level >= 61 && level <= 65) {
            softReset();
            m_conformance = level - 60;
        }
        return;
    }

    if (inter.count == 1 && inter.bytes[0] == '\'' && inter.marker == 0 && (final == '}' || final == '~')) {
        if (final == '}') {
            insertColumns(p.atOr(0, 1));
        } else {
            deleteColumns(p.atOr(0, 1));
        }
        return;
    }

    if (inter.count == 1 && inter.bytes[0] == '*' && inter.marker == 0 && final == 'y') {
        reportRectChecksum(p);
        return;
    }

    if (inter.count == 1 && inter.bytes[0] == '!' && inter.marker == 0 && final == 'p') {
        softReset();
        return;
    }
}

void Terminal::vt52Dispatch(std::uint8_t final)
{
    Cursor& cur = cursor();
    switch (final) {
    case 'A':
        cur.y = std::max(cur.y - 1, 0);
        cur.pendingWrap = false;
        break;
    case 'B':
        cur.y = std::min(cur.y + 1, rows() - 1);
        cur.pendingWrap = false;
        break;
    case 'C':
        cur.x = std::min(cur.x + 1, cols() - 1);
        cur.pendingWrap = false;
        break;
    case 'D':
        cur.x = std::max(cur.x - 1, 0);
        cur.pendingWrap = false;
        break;
    case 'F': // graphics character set
        m_charsets[m_gl] = Charset::DecSpecialGraphics;
        break;
    case 'G':
        m_charsets[m_gl] = Charset::Ascii;
        break;
    case 'H':
        setCursor(0, 0);
        break;
    case 'I':
        reverseIndex();
        break;
    case 'J':
        eraseDisplay(0);
        break;
    case 'K':
        eraseLine(0);
        break;
    case 'Y':
        m_vt52Address = 2;
        break;
    case 'Z':
        reply("\x1b/Z");
        break;
    case '=':
        m_modes.keypadApp = true;
        break;
    case '>':
        m_modes.keypadApp = false;
        break;
    case '<': // back to ANSI mode
        m_modes.vt52 = false;
        m_vt52Address = 0;
        break;
    default:
        break;
    }
}

void Terminal::reportPresentationState(int which)
{
    const Cursor& cur = cursor();
    std::string r;
    if (which == 1) {
        // DECCIR: DCS 1 $ u Pr;Pc;Pp;Srend;Satt;Sflag;Pgl;Pgr;Scss;Sdesig ST
        const Style& pen = cur.pen;
        const char rend = char(0x40 | (pen.has(Attr::Bold) ? 1 : 0) | (pen.underline != UnderlineStyle::None ? 2 : 0)
                               | (pen.has(Attr::Blink) ? 4 : 0) | (pen.has(Attr::Inverse) ? 8 : 0));
        const char att = char(0x40 | (pen.has(Attr::Protected) && m_protection == Protection::Dec ? 1 : 0));
        const char flag = char(0x40 | (m_modes.originMode ? 1 : 0) | (m_singleShift == 2 ? 2 : 0)
                               | (m_singleShift == 3 ? 4 : 0) | (cur.pendingWrap ? 8 : 0));
        int sizes = 0;
        std::string designators;
        for (int g = 0; g < 4; ++g) {
            switch (m_charsets[g]) {
            case Charset::DecSpecialGraphics:
                designators += '0';
                break;
            case Charset::British:
                designators += 'A';
                break;
            case Charset::Latin1Supplemental:
                designators += 'A';
                sizes |= 1 << g; // a 96-character set
                break;
            case Charset::Ascii:
                designators += 'B';
                break;
            }
        }
        r = "\x1bP1$u" + std::to_string(cur.y + 1) + ';' + std::to_string(cur.x + 1) + ";1;" + rend + ';' + att + ';'
            + flag + ';' + std::to_string(m_gl) + ";2;" + char(0x40 | sizes) + ';' + designators + "\x1b\\";
    } else if (which == 2) {
        // DECTABSR: DCS 2 $ u Pt/Pt/... ST, 1-based columns.
        r = "\x1bP2$u";
        bool first = true;
        for (std::size_t x = 0; x < m_tabStops.size(); ++x) {
            if (m_tabStops[x]) {
                if (!first) {
                    r += '/';
                }
                r += std::to_string(x + 1);
                first = false;
            }
        }
        r += "\x1b\\";
    } else {
        return;
    }
    reply(r);
}

void Terminal::sixel(const Params& p, std::string_view data)
{
    // DCS P1 ; P2 ; P3 q <sixel data> ST. P2 = 1 keeps unpainted pixels clear.
    SixelPalette fresh;
    SixelPalette* palette = &fresh;
    if (m_modes.sixelPrivateColors) {
        fresh = defaultSixelPalette();
    } else {
        if (!m_sixelPaletteReady) {
            m_sixelPalette = defaultSixelPalette();
            m_sixelPaletteReady = true;
        }
        palette = &m_sixelPalette;
    }
    ImageFrame frame;
    const std::uint32_t background = (std::uint32_t(m_defaultBg.r) << 16) | (std::uint32_t(m_defaultBg.g) << 8) | m_defaultBg.b;
    if (!decodeSixel(data, p.at(1), background, *palette, frame) || m_cellWidth <= 0 || m_cellHeight <= 0) {
        return;
    }
    Cursor& cur = cursor();
    GraphicsStore& store = m_graphics[m_modes.altScreen ? 1 : 0];
    const int cellsWide = int((frame.width + std::uint32_t(m_cellWidth) - 1) / std::uint32_t(m_cellWidth));
    const int cellsHigh = int((frame.height + std::uint32_t(m_cellHeight) - 1) / std::uint32_t(m_cellHeight));
    if (m_modes.sixelDisplay) {
        // DECSDM: at the top left of the page; nothing scrolls, the cursor stays.
        store.addSixel(std::move(frame), *m_active, 0, 0, m_cellWidth, m_cellHeight);
        return;
    }
    const int col = cur.x;
    store.addSixel(std::move(frame), *m_active, cur.y, col, m_cellWidth, m_cellHeight);
    // As foot: down to the last row the image touches (scrolling as needed,
    // bounded), at its left column or right of it with mode 8452.
    const int feeds = std::min(cellsHigh - 1, rows() * 2);
    for (int i = 0; i < feeds; ++i) {
        index();
    }
    cur.pendingWrap = false;
    cur.x = m_modes.sixelCursorRight ? std::min(col + cellsWide, cols() - 1) : col;
}

void Terminal::softReset()
{
    // DECSTR, plus what xterm also resets: reverse wrap and DECLRMM.
    m_modes.insert = false;
    m_modes.originMode = false;
    m_modes.autowrap = true;
    m_modes.cursorVisible = true;
    m_modes.cursorKeysApp = false;
    m_modes.keypadApp = false;
    m_modes.reverseWrap = false;
    m_modes.reverseWrapExtended = false;
    m_modes.leftRightMargins = false;
    m_margins = Region{0, rows() - 1, 0, cols() - 1};
    std::fill(std::begin(m_charsets), std::end(m_charsets), Charset::Ascii);
    m_gl = 0;
    m_singleShift = 0;
    m_active->setPen(Style{}); // DECSCA protection off too
    m_protection = Protection::Off;
    m_active->hasSavedCursor = false;
}

void Terminal::setColumnMode(bool wide)
{
    // DECCOLM: 132 or 80 columns when allowed (mode 40). The page is cleared
    // (unless DECNCSM at level 5), margins reset and the cursor homed.
    if (!m_modes.allow132) {
        return;
    }
    m_modes.columns132 = wide;
    const int width = wide ? 132 : 80;
    if (width != cols() && m_host.requestResize(width, rows())) {
        resize(width, rows(), m_cellWidth, m_cellHeight);
    }
    m_margins = Region{0, rows() - 1, 0, cols() - 1};
    m_modes.leftRightMargins = false;
    if (!(m_modes.noClearOnColumnChange && m_conformance >= 5)) {
        for (int y = 0; y < rows(); ++y) {
            m_active->eraseRow(m_active->row(y), StyleTable::DefaultId);
        }
    }
    setCursor(0, 0);
}

void Terminal::saveDecModes(const Params& p)
{
    for (int i = 0; i < p.count; ++i) {
        const int state = decModeState(p.values[i]);
        if (state == 1 || state == 2) {
            m_savedDecModes[p.values[i]] = state == 1;
        }
    }
}

void Terminal::restoreDecModes(const Params& p)
{
    for (int i = 0; i < p.count; ++i) {
        if (auto it = m_savedDecModes.find(p.values[i]); it != m_savedDecModes.end()) {
            setDecMode(it->first, it->second);
        }
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
    s.attrs = std::uint16_t((s.attrs & ~Attr::Protected) | (cursor().pen.attrs & Attr::Protected));
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
    case 2: // DECANM: reset enters VT52 mode; ESC < leaves it
        if (!on) {
            m_modes.vt52 = true;
            m_charsets[0] = m_charsets[1] = Charset::Ascii;
            m_gl = 0;
        }
        break;
    case 3:
        setColumnMode(on);
        break;
    case 4:
        m_modes.smoothScroll = on;
        break;
    case 5:
        m_modes.reverseVideo = on;
        break;
    case 18:
        m_modes.printFormFeed = on;
        break;
    case 19:
        m_modes.printExtent = on;
        break;
    case 40:
        m_modes.allow132 = on;
        break;
    case 41:
        m_modes.moreFix = on;
        break;
    case 67:
        m_modes.backarrowSendsBs = on;
        break;
    case 95:
        m_modes.noClearOnColumnChange = on;
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
    case 45:
        m_modes.reverseWrap = on;
        break;
    case 47:
        setAltScreen(on, false, false);
        break;
    case 66: // DECNKM
        m_modes.keypadApp = on;
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
    case 1045:
        m_modes.reverseWrapExtended = on;
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
    case 5522:
        m_modes.pasteEvents = on;
        break;
    case 2031:
        m_modes.themeReports = on;
        if (on) {
            m_reportedDark = backgroundIsDark() ? 1 : 0; // later changes are reported
        }
        break;
    case 2033:
        m_modes.visibilityReports = on;
        if (on) {
            reportVisibility();
        }
        break;
    case 80:
        m_modes.sixelDisplay = on;
        break;
    case 1070:
        m_modes.sixelPrivateColors = on;
        break;
    case 8452:
        m_modes.sixelCursorRight = on;
        break;
    case 2048:
        m_modes.sizeReports = on;
        if (on) {
            reportSize();
        }
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
    case 45:
        return b(m_modes.reverseWrap);
    case 47:
    case 1047:
    case 1049:
        return b(m_modes.altScreen);
    case 66:
        return b(m_modes.keypadApp);
    case 69:
        return b(m_modes.leftRightMargins);
    case 1045:
        return b(m_modes.reverseWrapExtended);
    case 2:
        return b(!m_modes.vt52);
    case 3:
        return b(m_modes.columns132);
    case 4:
        return b(m_modes.smoothScroll);
    case 18:
        return b(m_modes.printFormFeed);
    case 19:
        return b(m_modes.printExtent);
    case 40:
        return b(m_modes.allow132);
    case 41:
        return b(m_modes.moreFix);
    case 67:
        return b(m_modes.backarrowSendsBs);
    case 95:
        return b(m_modes.noClearOnColumnChange);
    case 35: // DECHEBM: no Hebrew keyboard mapping
    case 42: // DECNRCM: no national replacement sets
    case 1048: // save/restore cursor: an action, not a state
        return 2;
    case 1036: // Alt always sends ESC
    case 1046: // switching to the alternate screen is always allowed
        return 3; // permanently set
    // Modes afterglow knows but does not have: hardware features of DEC
    // terminals and xterm's own window options.
    case 8: // DECARM: auto-repeat is the compositor's
    case 10: // DECEDM / xterm toolbar
    case 11: // DECLTM
    case 13: // DECSCFDM / xterm blink option
    case 14: // DECTEM / xterm XOR blink
    case 16: // DECEKEM
    case 30: // xterm scrollbar
    case 38: // DECTEK
    case 43: // DECGEPM
    case 44: // DECGPCM / xterm margin bell
    case 46: // DECGPBM / xterm logging
    case 53: // DEC131TM
    case 59: // DECKKDM
    case 61: // DECVCCM
    case 64: // DECPCCM
    case 68: // DECKBUM
    case 73: // DECXRLM
    case 81: // DECKPM
    case 1010: // scroll on output
    case 1011: // scroll on key
    case 1014: // fast scroll
    case 1034: // 8-bit input
    case 1035: // num lock modifiers
    case 1037: // Delete sends DEL
    case 1040: // keep selection
    case 1041: // select to clipboard
    case 1042: // bell urgency
    case 1043: // raise on bell
    case 2001: // readline mouse modes
    case 2002:
    case 2003:
    case 2005:
    case 2006:
        return 4; // permanently reset
    case 60: // DECHCCM
        return 4;
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
    case 5522:
        return b(m_modes.pasteEvents);
    case 2031:
        return b(m_modes.themeReports);
    case 2033:
        return b(m_modes.visibilityReports);
    case 2048:
        return b(m_modes.sizeReports);
    case 80:
        return b(m_modes.sixelDisplay);
    case 1070:
        return b(m_modes.sixelPrivateColors);
    case 8452:
        return b(m_modes.sixelCursorRight);
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
    case 2: // KAM: the keyboard is never locked
    case 12: // SRM: no local echo
        return 2;
    case 1: // GATM
    case 5: // SRTM
    case 7: // VEM
    case 10: // HEM
    case 11: // PUM
    case 13: // FEAM
    case 14: // FETM
    case 15: // MATM
    case 16: // TTM
    case 17: // SATM
    case 18: // TSM
    case 19: // EBM
        return 4; // permanently reset
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

void Terminal::reportRectChecksum(const Params& p)
{
    // DECRQCRA: CSI Pid ; Pp ; Pt ; Pl ; Pb ; Pr * y. Coordinates follow
    // origin mode. The checksum is the 16-bit sum of the characters, blanks
    // counting as spaces, as modern xterm reports it (test suites read the
    // screen one cell at a time through this).
    const bool origin = m_modes.originMode;
    const int top0 = origin ? m_margins.top : 0;
    const int left0 = origin ? m_margins.left : 0;
    const int bottomMax = origin ? m_margins.bottom : rows() - 1;
    const int rightMax = origin ? m_margins.right : cols() - 1;
    const int top = top0 + p.atOr(2, 1) - 1;
    const int left = left0 + p.atOr(3, 1) - 1;
    const int bottom = std::min(bottomMax, top0 + p.atOr(4, std::uint16_t(bottomMax - top0 + 1)) - 1);
    const int right = std::min(rightMax, left0 + p.atOr(5, std::uint16_t(rightMax - left0 + 1)) - 1);
    std::uint32_t sum = 0;
    for (int y = top; y <= bottom; ++y) {
        const Row& row = m_active->row(y);
        for (int x = left; x <= right; ++x) {
            const Cell& c = row.at(x);
            if (c.isTail()) {
                continue;
            }
            sum += c.codepoint() == 0 ? U' ' : c.codepoint();
        }
    }
    char buf[32];
    const int n = std::snprintf(buf, sizeof buf, "\x1bP%d!~%04X\x1b\\", int(p.at(0)), unsigned(sum & 0xFFFF));
    reply(std::string_view(buf, std::size_t(n)));
}

Terminal::Mark Terminal::cursorMark() const
{
    const Screen& scr = *m_primary;
    return Mark{scr.rowsDropped() + scr.historySize() + std::uint64_t(scr.cursor.y), scr.cursor.x, true};
}

void Terminal::finishCommand(int exitCode)
{
    m_commandRunning = false;
    m_host.commandFinished(exitCode);
}

std::string Terminal::textBetween(const Mark& from, const Mark& to) const
{
    const Screen& scr = *m_primary;
    const long long first = scr.absoluteIndexOf(from.row);
    const long long last = scr.absoluteIndexOf(to.row);
    if (last < 0) {
        return {};
    }
    const long long start = first < 0 ? 0 : first; // the start scrolled out of history
    const int startCol = first < 0 ? 0 : from.col;
    std::string out;
    for (long long i = start; i <= last; ++i) {
        const Row& row = scr.absoluteRow(std::size_t(i));
        const int c0 = i == start ? startCol : 0;
        const int c1 = i == last ? to.col : scr.cols();
        std::string line;
        for (int x = c0; x < c1 && x < scr.cols(); ++x) {
            const Cell& c = row.at(x);
            if (c.isTail() || c.isSpacerHead() || (c.isMulticell() && (c.multicellDx() || c.multicellDy()))) {
                continue;
            }
            const char32_t cp = c.isEmpty() ? U' ' : c.codepoint();
            appendUtf8(line, cp);
            if (c.hasGrapheme()) {
                for (char32_t e : scr.graphemeExtra(row, x)) {
                    appendUtf8(line, e);
                }
            }
        }
        const bool continues = row.wrapped && i < last;
        if (!continues) {
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
        }
        out += line;
        if (!continues && i < last) {
            out += '\n';
        }
    }
    return out;
}

std::string Terminal::text(TextScope scope) const
{
    switch (scope) {
    case TextScope::LastOutput:
        if (!m_outputStart.valid) {
            return {};
        }
        return textBetween(m_outputStart, m_outputEnd.valid ? m_outputEnd : cursorMark());
    case TextScope::All: {
        const Screen& scr = *m_primary;
        const Mark top{scr.rowsDropped(), 0, true};
        const Mark end{scr.rowsDropped() + scr.historySize() + std::uint64_t(scr.rows() - 1), scr.cols(), true};
        std::string s = textBetween(top, end);
        while (!s.empty() && s.back() == '\n') {
            s.pop_back();
        }
        return s;
    }
    case TextScope::Screen:
        break;
    }
    // The active screen's visible rows.
    const Screen& scr = *m_active;
    std::string out;
    for (int y = 0; y < scr.rows(); ++y) {
        const Row& row = scr.row(y);
        std::string line;
        for (int x = 0; x < scr.cols(); ++x) {
            const Cell& c = row.at(x);
            if (c.isTail() || c.isSpacerHead() || (c.isMulticell() && (c.multicellDx() || c.multicellDy()))) {
                continue;
            }
            appendUtf8(line, c.isEmpty() ? U' ' : c.codepoint());
            if (c.hasGrapheme()) {
                for (char32_t e : scr.graphemeExtra(row, x)) {
                    appendUtf8(line, e);
                }
            }
        }
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        out += line;
        out += '\n';
    }
    while (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

void Terminal::programExited()
{
    if (m_programStatus.endActivity()) {
        m_host.programStatusChanged();
    }
}

void Terminal::reportDeviceAttributes()
{
    // VT500 level with what afterglow implements: sixel (4), selective erase
    // (6), colour (22), rectangular editing (28) and clipboard access (52).
    reply("\x1b[?65;4;6;22;28;52c");
}

void Terminal::deviceStatusReport(const Params& p, bool priv)
{
    char buf[48];
    if (priv) {
        // DEC status reports for hardware afterglow does not have.
        switch (p.at(0)) {
        case 15: // printer: none
            reply("\x1b[?13n");
            return;
        case 25: // user-defined keys: locked
            reply("\x1b[?21n");
            return;
        case 26: // keyboard: North American, ready, LK401 (the last two from level 4)
            reply(m_conformance >= 4 ? "\x1b[?27;1;0;1n" : m_conformance == 3 ? "\x1b[?27;1;0n" : "\x1b[?27;1n");
            return;
        case 55: // locator: none
            reply("\x1b[?50n");
            return;
        case 56: // locator type: unknown
            reply("\x1b[?57;0n");
            return;
        case 62: // DECMSR: no macro space
            reply("\x1b[0*{");
            return;
        case 63: { // DECCKSR: macro memory checksum
            const int n = std::snprintf(buf, sizeof buf, "\x1bP%d!~0000\x1b\\", int(p.at(1)));
            reply(std::string_view(buf, std::size_t(n)));
            return;
        }
        case 75: // data integrity: no errors
            reply("\x1b[?70n");
            return;
        case 85: // multiple sessions: not supported
            reply("\x1b[?83n");
            return;
        case 996: // the colour theme: dark or light
            reportTheme(true);
            return;
        case 998: // visibility
            reportVisibility();
            return;
        default:
            break;
        }
    }
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
    case 4: // resize to height;width in pixels
    case 8: { // resize to rows;cols
        // Omitted keeps the current size; 0 means as large as the display.
        int screenW = 0;
        int screenH = 0;
        const bool known = m_host.screenSize(screenW, screenH) && m_cellWidth > 0 && m_cellHeight > 0;
        auto cells = [&](int index, int current, int screenPx, int cellPx) {
            if (!p.given(index)) {
                return current;
            }
            const int value = p.at(index);
            if (value == 0) {
                return known ? screenPx / cellPx : current;
            }
            return p.at(0) == 4 ? (cellPx > 0 ? value / cellPx : current) : value;
        };
        const int r = std::max(1, cells(1, rows(), screenH, m_cellHeight));
        const int c = std::max(2, cells(2, cols(), screenW, m_cellWidth));
        if ((r != rows() || c != cols()) && m_host.requestResize(c, r)) {
            resize(c, r, m_cellWidth, m_cellHeight);
        }
        return;
    }
    case 11: // window state: never iconified as far as programs can tell
        n = std::snprintf(buf, sizeof buf, "\x1b[1t");
        break;
    case 13: // window position: unknown on Wayland
        n = std::snprintf(buf, sizeof buf, "\x1b[3;0;0t");
        break;
    case 15:
    case 19: {
        int w = 0;
        int h = 0;
        if (!m_host.screenSize(w, h)) {
            return;
        }
        if (p.at(0) == 15) {
            n = std::snprintf(buf, sizeof buf, "\x1b[5;%d;%dt", h, w);
        } else if (m_cellWidth > 0 && m_cellHeight > 0) {
            n = std::snprintf(buf, sizeof buf, "\x1b[9;%d;%dt", h / m_cellHeight, w / m_cellWidth);
        } else {
            return;
        }
        break;
    }
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
        // DECSLPP: 24 or more sets the lines per page.
        if (p.at(0) >= 24 && p.at(0) != rows() && m_host.requestResize(cols(), p.at(0))) {
            resize(cols(), p.at(0), m_cellWidth, m_cellHeight);
        }
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
        // iTerm2 notification. "9;4;st;pr" is ConEmu's progress report, which
        // feeds program status until a program speaks OSC 7501.
        if (rest.starts_with("4;") || rest == "4") {
            int st = 0;
            int pr = 0;
            const std::string_view args = rest.size() > 2 ? rest.substr(2) : std::string_view{};
            const auto semi2 = args.find(';');
            std::from_chars(args.data(), args.data() + (semi2 == std::string_view::npos ? args.size() : semi2), st);
            if (semi2 != std::string_view::npos) {
                std::from_chars(args.data() + semi2 + 1, args.data() + args.size(), pr);
            }
            if (m_programStatus.applyLegacyProgress(st, pr)) {
                m_host.programStatusChanged();
            }
        } else if (!rest.empty()) {
            m_host.notify({}, rest);
        }
        break;
    case 66:
        textSizing(rest);
        break;
    case 99: {
        NotificationRequest request;
        switch (m_notifications.feed(rest, request)) {
        case NotificationAssembler::Result::Ready:
            m_host.desktopNotification(request);
            break;
        case NotificationAssembler::Result::Query:
            if (const std::string caps = m_host.notificationCapabilities(); !caps.empty()) {
                reply(NotificationAssembler::queryReply(request.id, caps));
            }
            break;
        case NotificationAssembler::Result::Pending:
        case NotificationAssembler::Result::Ignored:
            break;
        }
        break;
    }
    case 5522: {
        ClipboardRequest request;
        std::string error;
        switch (m_clipboard.feed(rest, request, error)) {
        case ClipboardAssembler::Result::Ready:
            if (!m_host.clipboardRequest(request)) {
                reply(request.kind == ClipboardRequest::Kind::Read
                          ? ClipboardAssembler::readStatus("ENOSYS", request)
                          : ClipboardAssembler::writeStatus("ENOSYS", request.id));
            }
            break;
        case ClipboardAssembler::Result::Error:
            reply(error);
            break;
        case ClipboardAssembler::Result::Pending:
        case ClipboardAssembler::Result::Ignored:
            break;
        }
        break;
    }
    case 7501:
        switch (m_programStatus.apply(rest)) {
        case ProgramStatus::Result::Changed:
            m_host.programStatusChanged();
            break;
        case ProgramStatus::Result::Query: {
            std::string r = "\x1b]7501;";
            r += ProgramStatus::queryReply();
            r += bel ? "\x07" : "\x1b\\";
            reply(r);
            break;
        }
        case ProgramStatus::Result::Ignored:
            break;
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
    case 21:
        oscColorControl(rest, bel);
        break;
    case 17: // selection background
    case 19: { // selection foreground
        Rgb& target = num == 17 ? m_selectionBg : m_selectionFg;
        bool& has = num == 17 ? m_hasSelectionBg : m_hasSelectionFg;
        if (rest == "?") {
            // Unset: the inverse of the default colours, as drawn.
            const Rgb value = has ? target : (num == 17 ? m_defaultFg : m_defaultBg);
            reply(formatColorReply(num == 17 ? "17;" : "19;", value, bel));
        } else if (Rgb c; parseColorSpec(rest, c)) {
            target = c;
            has = true;
        }
        break;
    }
    case 117:
        m_hasSelectionBg = false;
        break;
    case 119:
        m_hasSelectionFg = false;
        break;
    case 555:
        m_host.flash();
        break;
    case 30001: // kitty: push colours
        pushColors(0);
        break;
    case 30101: // kitty: pop colours
        popColors(0);
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
        if (m_commandRunning) {
            m_outputEnd = cursorMark();
            finishCommand(-1); // no D: the exit code is unknown
        }
        // Back at the shell: whatever a program reported in flight is over.
        if (m_programStatus.endActivity()) {
            m_host.programStatusChanged();
        }
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
        if (!m_modes.altScreen) {
            m_inputStart = cursorMark();
        }
        break;
    case 'C': // command output begins
        m_semanticZone = SemanticZone::Output;
        m_promptRedrawPending = false;
        if (!m_modes.altScreen) {
            // The command is what was typed between B and here.
            std::string command = m_inputStart.valid ? textBetween(m_inputStart, cursorMark()) : std::string{};
            while (!command.empty() && (command.back() == '\n' || command.back() == ' ')) {
                command.pop_back();
            }
            m_outputStart = cursorMark();
            m_outputEnd = {};
            m_commandRunning = true;
            m_host.commandStarted(command);
        }
        break;
    case 'D': { // command finished, D ; exit-code
        m_semanticZone = SemanticZone::Output;
        m_promptRedrawPending = false;
        int code = -1;
        if (args.size() > 2 && args[1] == ';') {
            const std::string_view rest = args.substr(2);
            const auto end = rest.find(';');
            std::from_chars(rest.data(), rest.data() + (end == std::string_view::npos ? rest.size() : end), code);
        }
        if (m_commandRunning && !m_modes.altScreen) {
            m_outputEnd = cursorMark();
            finishCommand(code);
        }
        break;
    }
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

void Terminal::pushColors(int slot)
{
    const SavedColors saved{m_palette, m_defaultFg, m_defaultBg, m_cursorColor, m_hasCursorColor};
    if (slot <= 0) {
        if (m_colorStack.size() < MaxColorStack) {
            m_colorStack.push_back(saved);
        }
    } else if (std::size_t(slot) <= MaxColorStack) {
        if (m_colorStack.size() < std::size_t(slot)) {
            m_colorStack.resize(std::size_t(slot), saved);
        }
        m_colorStack[std::size_t(slot) - 1] = saved;
    }
}

void Terminal::popColors(int slot)
{
    const SavedColors* from = nullptr;
    if (slot <= 0) {
        from = m_colorStack.empty() ? nullptr : &m_colorStack.back();
    } else if (std::size_t(slot) <= m_colorStack.size()) {
        from = &m_colorStack[std::size_t(slot) - 1];
    }
    if (!from) {
        return;
    }
    m_palette = from->palette;
    m_defaultFg = from->foreground;
    m_defaultBg = from->background;
    m_cursorColor = from->cursor;
    m_hasCursorColor = from->hasCursor;
    if (slot <= 0) {
        m_colorStack.pop_back();
    }
}

void Terminal::oscColorControl(std::string_view args, bool bel)
{
    // kitty OSC 21: key=value pairs separated by ';'. "key=?" queries,
    // "key=spec" sets, "key=" makes the colour dynamic and a bare "key"
    // restores the default. Keys: 0-255 and named colours.
    std::string reply21;
    auto append = [&reply21](std::string_view key, const Rgb* c) {
        reply21 += ';';
        reply21 += key;
        reply21 += '=';
        if (c) {
            char buf[32];
            const int n = std::snprintf(buf, sizeof buf, "rgb:%02x/%02x/%02x", c->r, c->g, c->b);
            reply21.append(buf, std::size_t(n));
        }
    };
    const std::array<Rgb, 256> defaults = buildPalette(m_options.basePalette);
    std::size_t pos = 0;
    while (pos <= args.size()) {
        const auto next = args.find(';', pos);
        const std::string_view item = args.substr(pos, next == std::string_view::npos ? next : next - pos);
        pos = next == std::string_view::npos ? args.size() + 1 : next + 1;
        if (item.empty()) {
            continue;
        }
        const auto eq = item.find('=');
        const std::string_view key = item.substr(0, eq);
        const bool bare = eq == std::string_view::npos;
        std::string_view value = bare ? std::string_view{} : item.substr(eq + 1);
        if (const auto at = value.find('@'); at != std::string_view::npos) {
            value = value.substr(0, at); // per-colour alpha is not supported
        }
        const bool query = value == "?";

        int index = -1;
        Rgb* target = nullptr;
        Rgb fallback{};
        bool known = true;
        if (std::from_chars(key.data(), key.data() + key.size(), index).ec == std::errc{} && index >= 0 && index < 256) {
            target = &m_palette[std::size_t(index)];
            fallback = defaults[std::size_t(index)];
        } else if (key == "foreground") {
            target = &m_defaultFg;
            fallback = m_options.defaultForeground;
        } else if (key == "background") {
            target = &m_defaultBg;
            fallback = m_options.defaultBackground;
        } else if (key == "cursor") {
            if (query) {
                append(key, m_hasCursorColor ? &m_cursorColor : nullptr); // empty: follows the text
            } else if (bare || value.empty()) {
                m_hasCursorColor = false;
            } else if (Rgb c; parseColorSpec(value, c)) {
                m_cursorColor = c;
                m_hasCursorColor = true;
            }
            continue;
        } else if (key == "selection_background" || key == "selection_foreground") {
            Rgb& sel = key == "selection_background" ? m_selectionBg : m_selectionFg;
            bool& has = key == "selection_background" ? m_hasSelectionBg : m_hasSelectionFg;
            if (query) {
                append(key, has ? &sel : nullptr);
            } else if (bare || value.empty()) {
                has = false;
            } else if (Rgb c; parseColorSpec(value, c)) {
                sel = c;
                has = true;
            }
            continue;
        } else if (key == "cursor_text" || key == "visual_bell" || key.starts_with("transparent_background_color")) {
            // Derived from the other colours in afterglow: always dynamic.
            if (query) {
                append(key, nullptr);
            }
            continue;
        } else {
            known = false;
        }
        if (!known) {
            if (query) {
                // Unknown keys come back as unknown=<base64 of the key, unpadded>.
                static constexpr char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::string enc;
                const std::string_view k = key.substr(0, 64);
                for (std::size_t j = 0; j < k.size(); j += 3) {
                    std::uint32_t v = std::uint32_t(std::uint8_t(k[j])) << 16;
                    if (j + 1 < k.size()) {
                        v |= std::uint32_t(std::uint8_t(k[j + 1])) << 8;
                    }
                    if (j + 2 < k.size()) {
                        v |= std::uint8_t(k[j + 2]);
                    }
                    enc += B64[(v >> 18) & 63];
                    enc += B64[(v >> 12) & 63];
                    if (j + 1 < k.size()) {
                        enc += B64[(v >> 6) & 63];
                    }
                    if (j + 2 < k.size()) {
                        enc += B64[v & 63];
                    }
                }
                reply21 += ";unknown=" + enc;
            }
            continue;
        }
        if (query) {
            append(key, target);
        } else if (bare || value.empty()) {
            *target = fallback;
        } else if (Rgb c; parseColorSpec(value, c)) {
            *target = c;
        }
    }
    if (!reply21.empty()) {
        reply("\x1b]21" + reply21 + (bel ? "\x07" : "\x1b\\"));
    }
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

void Terminal::dcsDispatch(const Params& params, const Intermediates& inter, std::uint8_t final, std::string_view data)
{
    m_lastX = -1;
    if (inter.marker == 0 && inter.count == 0 && final == 'q') {
        sixel(params, data);
        return;
    }
    if (inter.marker == '=' && inter.count == 0 && final == 's') {
        // The older form of synchronized updates: 1 begins, 2 ends.
        if (params.at(0) == 1 || params.at(0) == 2) {
            m_modes.synchronizedOutput = params.at(0) == 1;
        }
        return;
    }
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
        } else if (data == "\"q") {
            value = cursor().pen.has(Attr::Protected) && m_protection == Protection::Dec ? "1\"q" : "0\"q";
        } else if (data == "\"p") {
            value = std::to_string(60 + m_conformance) + ";1\"p";
        } else if (data == "*x") {
            value = std::to_string(m_attributeChangeExtent) + "*x";
        } else if (data == "$}") {
            value = "0$}"; // the main display is active
        } else if (data == "$~") {
            value = "0$~"; // no status line
        } else if (data == "t") {
            value = std::to_string(rows()) + 't';
        } else if (data == "*|") {
            value = std::to_string(rows()) + "*|";
        } else if (data == "$|") {
            value = std::to_string(cols()) + "$|";
        } else if (data == ">0m" || data == ">1m" || data == ">2m" || data == ">4m") {
            const int resource = data[1] - '0';
            const int level = resource == 4 ? m_modes.modifyOtherKeys : resource == 0 ? 0 : 2;
            value = ">" + std::to_string(resource) + ";" + std::to_string(level) + "m";
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
