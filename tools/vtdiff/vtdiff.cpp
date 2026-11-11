// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// afterglow_vtdiff: feeds the same bytes to Afterglow's terminal core and to
// libghostty-vt, and compares what they show. The fuzzers find crashes and
// esctest checks a fixed set of xterm cases; this finds a screen that one of
// the two gets wrong.
//
//   afterglow_vtdiff [--cols N] [--rows N] [--chunk N] [--replies]
//                    [--expected FILE] [--minimize] FILE|DIR...
//   afterglow_vtdiff [same options] --random SEED --count N [--length N]
//
// Each input is fed in chunks of --chunk bytes (4096 by default), and the
// screens are compared after each chunk: every cell's text, width and style,
// the cursor (position, pending wrap, visibility), the active screen and the
// last rows of the scrollback. With --replies, what each terminal writes back
// to the program is compared at the end too.
//
// --expected names a file of known differences, one a line with its reason
// after the first space: an input name, or /REGEX/ for a class of difference.
// A pattern matches the minimized input as --minimize prints it (bytes outside
// ASCII as \xHH), so random inputs can be expected by what makes them differ.
// A second /REGEX/ after it must match the difference of that input too, as
// the run prints it ("cell 1,0: ..."). A '/' in a pattern is written "\/".
// An input that differs and is not expected fails the run, and so does a
// named input that no longer differs.
// --minimize cuts a differing input down, by removing ever smaller pieces,
// to the shortest input that still differs in the same kind of way (a cell's
// text, a cell's look, the cursor, the screen, the scrollback, the replies),
// and prints it.
// --random makes COUNT inputs of LENGTH tokens (512 by default) from a list
// of sequences that terminals parse, starting at SEED: input "random-SEED".
//
// Exit status: 0 when every difference is expected, 1 when one is not, 2 on
// a usage error.

#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Style.h>
#include <AfterglowVt/Terminal.h>

#include <ghostty/vt.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using AfterglowVt::Color;

struct Options
{
    int cols = 80;
    int rows = 24;
    std::size_t chunk = 4096;
    bool replies = false;
    bool minimize = false;
    std::string expected;
    std::optional<std::uint64_t> seed;
    int count = 1;
    int length = 512;
};

/// One cell as both terminals can say it: the text (base and extras), the
/// width (0 for the tail of a wide character), and the style.
struct CellView
{
    std::u32string text;
    int width = 1;
    // Colours: 0 default, then a palette index + 1, or 0x1000000 | RGB.
    std::uint32_t fg = 0, bg = 0, underlineColor = 0;
    std::uint16_t attrs = 0; ///< AfterglowVt::Attr bits, Protected left out
    int underline = 0; ///< AfterglowVt::UnderlineStyle order, as GhosttySgrUnderline
    bool operator==(const CellView&) const = default;
};

struct State
{
    int cols = 0, rows = 0;
    std::vector<std::vector<CellView>> screen; ///< the active area, row by row
    std::vector<std::u32string> history; ///< the last rows of the scrollback, oldest first
    int cursorX = 0, cursorY = 0;
    bool pendingWrap = false;
    bool cursorVisible = true;
    bool alternate = false;
};

constexpr std::size_t HistoryRows = 200;

std::uint32_t colorKey(const Color& c)
{
    switch (c.kind()) {
    case Color::Kind::Default: return 0;
    case Color::Kind::Palette: return std::uint32_t(c.index()) + 1;
    case Color::Kind::Rgb: return 0x1000000u | (std::uint32_t(c.r()) << 16) | (std::uint32_t(c.g()) << 8) | c.b();
    }
    return 0;
}

std::uint32_t colorKey(const GhosttyStyleColor& c)
{
    switch (c.tag) {
    case GHOSTTY_STYLE_COLOR_PALETTE: return std::uint32_t(c.value.palette) + 1;
    case GHOSTTY_STYLE_COLOR_RGB:
        return 0x1000000u | (std::uint32_t(c.value.rgb.r) << 16) | (std::uint32_t(c.value.rgb.g) << 8) | c.value.rgb.b;
    default: return 0;
    }
}

std::string utf8(std::u32string_view text)
{
    std::string out;
    for (char32_t c : text) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += {char(0xC0 | (c >> 6)), char(0x80 | (c & 0x3F))};
        else if (c < 0x10000) out += {char(0xE0 | (c >> 12)), char(0x80 | ((c >> 6) & 0x3F)), char(0x80 | (c & 0x3F))};
        else out += {char(0xF0 | (c >> 18)), char(0x80 | ((c >> 12) & 0x3F)), char(0x80 | ((c >> 6) & 0x3F)), char(0x80 | (c & 0x3F))};
    }
    return out;
}

// --- Afterglow --------------------------------------------------------------

struct Host : AfterglowVt::ITerminalHost
{
    std::string written;
    void writeToPty(std::string_view bytes) override { written.append(bytes); }
};

class AfterglowSide
{
public:
    AfterglowSide(int cols, int rows)
        : m_terminal(cols, rows, m_host)
    {
    }
    void feed(std::string_view bytes) { m_terminal.feed(bytes); }
    const std::string& replies() const { return m_host.written; }

    State state() const
    {
        const AfterglowVt::Screen& scr = m_terminal.screen();
        State s;
        s.cols = scr.cols();
        s.rows = scr.rows();
        for (int y = 0; y < s.rows; ++y) {
            const AfterglowVt::Row& row = scr.row(y);
            std::vector<CellView> line;
            for (int x = 0; x < s.cols; ++x) line.push_back(cell(scr, row, x));
            s.screen.push_back(std::move(line));
        }
        const std::size_t history = scr.historySize();
        for (std::size_t i = history - std::min(history, HistoryRows); i < history; ++i)
            s.history.push_back(rowText(scr, scr.absoluteRow(i)));
        s.cursorX = scr.cursor.x;
        s.cursorY = scr.cursor.y;
        s.pendingWrap = scr.cursor.pendingWrap;
        s.cursorVisible = m_terminal.modes().cursorVisible;
        s.alternate = m_terminal.modes().altScreen;
        return s;
    }

private:
    static CellView cell(const AfterglowVt::Screen& scr, const AfterglowVt::Row& row, int x)
    {
        const AfterglowVt::Cell& c = row.at(x);
        CellView v;
        v.width = c.isSpacerHead() ? 1 : c.width();
        if (!c.isEmpty() && !c.isTail() && !c.isSpacerHead()) {
            v.text += c.codepoint();
            v.text += scr.graphemeExtra(row, x);
        }
        const AfterglowVt::Style& st = scr.styles().get(c.styleId());
        v.fg = colorKey(st.fg);
        v.bg = colorKey(st.bg);
        v.underlineColor = colorKey(st.underlineColor);
        v.attrs = std::uint16_t(st.attrs & 0xFF);
        v.underline = int(st.underline);
        return v;
    }
    static std::u32string rowText(const AfterglowVt::Screen& scr, const AfterglowVt::Row& row)
    {
        std::u32string out;
        for (int x = 0; x < scr.cols(); ++x) {
            const AfterglowVt::Cell& c = row.at(x);
            if (c.isTail()) continue;
            if (c.isEmpty() || c.isSpacerHead()) out += U' ';
            else {
                out += c.codepoint();
                out += scr.graphemeExtra(row, x);
            }
        }
        while (!out.empty() && out.back() == U' ') out.pop_back();
        return out;
    }

    Host m_host;
    AfterglowVt::Terminal m_terminal;
};

// --- libghostty-vt ------------------------------------------------------------

class GhosttySide
{
public:
    GhosttySide(int cols, int rows)
    {
        if (ghostty_terminal_new(nullptr, &m_terminal, std::uint16_t(cols), std::uint16_t(rows)) != GHOSTTY_SUCCESS) {
            std::fprintf(stderr, "afterglow_vtdiff: libghostty-vt made no terminal\n");
            std::exit(2);
        }
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, this);
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY, reinterpret_cast<const void*>(&GhosttySide::written));
        // Grapheme clusters (mode 2027): Afterglow joins them by default, as
        // kitty does, and libghostty only when the mode is set.
        feed("\x1b[?2027h");
    }
    ~GhosttySide() { ghostty_terminal_free(m_terminal); }
    GhosttySide(const GhosttySide&) = delete;
    GhosttySide& operator=(const GhosttySide&) = delete;

    void feed(std::string_view bytes)
    {
        ghostty_terminal_vt_write(m_terminal, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    }
    const std::string& replies() const { return m_replies; }

    State state() const
    {
        State s;
        std::uint16_t cols = 0, rows = 0, x = 0, y = 0;
        bool wrap = false, visible = true;
        GhosttyTerminalScreen active = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
        std::size_t history = 0;
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_COLS, &cols);
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_ROWS, &rows);
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_X, &x);
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &y);
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_PENDING_WRAP, &wrap);
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_VISIBLE, &visible);
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &active);
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &history);
        s.cols = cols;
        s.rows = rows;
        s.cursorX = x;
        s.cursorY = y;
        s.pendingWrap = wrap;
        s.cursorVisible = visible;
        s.alternate = active == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
        for (int row = 0; row < s.rows; ++row) {
            std::vector<CellView> line;
            for (int col = 0; col < s.cols; ++col) line.push_back(cell(GHOSTTY_POINT_TAG_ACTIVE, col, std::uint32_t(row)));
            s.screen.push_back(std::move(line));
        }
        for (std::size_t i = history - std::min(history, HistoryRows); i < history; ++i) {
            std::u32string text;
            for (int col = 0; col < s.cols; ++col) {
                const CellView v = cell(GHOSTTY_POINT_TAG_HISTORY, col, std::uint32_t(i));
                if (v.width == 0) continue;
                text += v.text.empty() ? std::u32string(U" ") : v.text;
            }
            while (!text.empty() && text.back() == U' ') text.pop_back();
            s.history.push_back(std::move(text));
        }
        return s;
    }

private:
    static void written(GhosttyTerminal, void* self, const std::uint8_t* data, std::size_t len)
    {
        static_cast<GhosttySide*>(self)->m_replies.append(reinterpret_cast<const char*>(data), len);
    }

    CellView cell(GhosttyPointTag tag, int col, std::uint32_t row) const
    {
        CellView v;
        GhosttyPoint point{};
        point.tag = tag;
        point.value.coordinate.x = std::uint16_t(col);
        point.value.coordinate.y = row;
        GhosttyGridRef ref{};
        if (ghostty_terminal_grid_ref(m_terminal, point, &ref) != GHOSTTY_SUCCESS) return v;
        GhosttyCell cell{};
        ghostty_grid_ref_cell(&ref, &cell);
        GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
        ghostty_cell_get(cell, GHOSTTY_CELL_DATA_WIDE, &wide);
        v.width = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : wide == GHOSTTY_CELL_WIDE_SPACER_TAIL ? 0 : 1;
        GhosttyCellContentTag content = GHOSTTY_CELL_CONTENT_CODEPOINT;
        ghostty_cell_get(cell, GHOSTTY_CELL_DATA_CONTENT_TAG, &content);
        if (v.width != 0 && wide != GHOSTTY_CELL_WIDE_SPACER_HEAD
            && (content == GHOSTTY_CELL_CONTENT_CODEPOINT || content == GHOSTTY_CELL_CONTENT_CODEPOINT_GRAPHEME)) {
            std::uint32_t buf[64];
            std::size_t n = 0;
            if (ghostty_grid_ref_graphemes(&ref, buf, std::size(buf), &n) == GHOSTTY_SUCCESS)
                for (std::size_t i = 0; i < n; ++i) v.text += char32_t(buf[i]);
        }
        GhosttyStyle style{};
        ghostty_grid_ref_style(&ref, &style);
        v.fg = colorKey(style.fg_color);
        v.bg = colorKey(style.bg_color);
        v.underlineColor = colorKey(style.underline_color);
        // GhosttyStyleFlags in Afterglow's Attr order.
        const std::uint16_t f = style.flags;
        using namespace AfterglowVt;
        v.attrs = std::uint16_t(((f & GHOSTTY_STYLE_FLAG_BOLD) ? Attr::Bold : 0) | ((f & GHOSTTY_STYLE_FLAG_FAINT) ? Attr::Faint : 0)
            | ((f & GHOSTTY_STYLE_FLAG_ITALIC) ? Attr::Italic : 0) | ((f & GHOSTTY_STYLE_FLAG_BLINK) ? Attr::Blink : 0)
            | ((f & GHOSTTY_STYLE_FLAG_INVERSE) ? Attr::Inverse : 0) | ((f & GHOSTTY_STYLE_FLAG_INVISIBLE) ? Attr::Invisible : 0)
            | ((f & GHOSTTY_STYLE_FLAG_STRIKETHROUGH) ? Attr::Strikethrough : 0)
            | ((f & GHOSTTY_STYLE_FLAG_OVERLINE) ? Attr::Overline : 0));
        v.underline = (f & GHOSTTY_STYLE_UNDERLINE_MASK) >> GHOSTTY_STYLE_UNDERLINE_SHIFT;
        // An erased cell can hold only a background colour, with no style:
        // Afterglow gives the same cell a style with that background.
        if (content == GHOSTTY_CELL_CONTENT_BG_COLOR_PALETTE) {
            std::uint8_t index = 0;
            ghostty_cell_get(cell, GHOSTTY_CELL_DATA_COLOR_PALETTE, &index);
            v.bg = std::uint32_t(index) + 1;
        } else if (content == GHOSTTY_CELL_CONTENT_BG_COLOR_RGB) {
            GhosttyColorRgb rgb{};
            ghostty_cell_get(cell, GHOSTTY_CELL_DATA_COLOR_RGB, &rgb);
            v.bg = 0x1000000u | (std::uint32_t(rgb.r) << 16) | (std::uint32_t(rgb.g) << 8) | rgb.b;
        }
        return v;
    }

    GhosttyTerminal m_terminal = nullptr;
    std::string m_replies;
};

// --- Comparison ---------------------------------------------------------------

std::string describe(const CellView& c)
{
    std::ostringstream out;
    out << '"' << utf8(c.text) << "\" w" << c.width;
    if (c.fg) out << " fg" << std::hex << c.fg << std::dec;
    if (c.bg) out << " bg" << std::hex << c.bg << std::dec;
    if (c.underlineColor) out << " ul" << std::hex << c.underlineColor << std::dec;
    if (c.attrs) out << " attrs" << c.attrs;
    if (c.underline) out << " underline" << c.underline;
    return out.str();
}

std::string rowString(const std::vector<CellView>& row)
{
    std::u32string text;
    for (const CellView& c : row) {
        if (c.width == 0) continue;
        text += c.text.empty() ? std::u32string(U" ") : c.text;
    }
    while (!text.empty() && text.back() == U' ') text.pop_back();
    return utf8(text);
}

/// The first difference, or nothing.
std::optional<std::string> compare(const State& a, const State& g)
{
    std::ostringstream out;
    if (a.cols != g.cols || a.rows != g.rows) {
        out << "size " << a.cols << "x" << a.rows << " / " << g.cols << "x" << g.rows;
        return out.str();
    }
    if (a.alternate != g.alternate) return std::string("active screen ") + (a.alternate ? "alternate" : "primary") + " / "
        + (g.alternate ? "alternate" : "primary");
    for (int y = 0; y < a.rows; ++y) {
        for (int x = 0; x < a.cols; ++x) {
            if (a.screen[std::size_t(y)][std::size_t(x)] == g.screen[std::size_t(y)][std::size_t(x)]) continue;
            out << "cell " << x << "," << y << ": " << describe(a.screen[std::size_t(y)][std::size_t(x)]) << " / "
                << describe(g.screen[std::size_t(y)][std::size_t(x)]) << "\n  afterglow row: "
                << rowString(a.screen[std::size_t(y)]) << "\n  libghostty row: " << rowString(g.screen[std::size_t(y)]);
            return out.str();
        }
    }
    if (a.cursorX != g.cursorX || a.cursorY != g.cursorY || a.pendingWrap != g.pendingWrap) {
        out << "cursor " << a.cursorX << "," << a.cursorY << (a.pendingWrap ? " wrap" : "") << " / " << g.cursorX << ","
            << g.cursorY << (g.pendingWrap ? " wrap" : "");
        return out.str();
    }
    if (a.cursorVisible != g.cursorVisible)
        return std::string("cursor ") + (a.cursorVisible ? "shown" : "hidden") + " / " + (g.cursorVisible ? "shown" : "hidden");
    // The scrollback may keep a different number of rows: its last rows match.
    const std::size_t n = std::min(a.history.size(), g.history.size());
    for (std::size_t i = 1; i <= n; ++i) {
        const auto& ra = a.history[a.history.size() - i];
        const auto& rg = g.history[g.history.size() - i];
        if (ra == rg) continue;
        out << "scrollback row -" << i << ": \"" << utf8(ra) << "\" / \"" << utf8(rg) << '"';
        return out.str();
    }
    return std::nullopt;
}

struct Divergence
{
    std::size_t offset = 0; ///< bytes fed when it showed
    std::string what;
};

std::optional<Divergence> run(const std::string& input, const Options& o)
{
    AfterglowSide a(o.cols, o.rows);
    GhosttySide g(o.cols, o.rows);
    for (std::size_t pos = 0; pos < input.size(); pos += o.chunk) {
        const std::string_view part = std::string_view(input).substr(pos, o.chunk);
        a.feed(part);
        g.feed(part);
        if (auto what = compare(a.state(), g.state())) return Divergence{pos + part.size(), *what};
    }
    if (input.empty())
        if (auto what = compare(a.state(), g.state())) return Divergence{0, *what};
    if (o.replies && a.replies() != g.replies()) {
        auto shown = [](const std::string& s) {
            std::string out;
            for (unsigned char c : s) {
                if (c >= 0x20 && c < 0x7F) out += char(c);
                else {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\x%02x", c);
                    out += buf;
                }
            }
            return out;
        };
        return Divergence{input.size(), "replies \"" + shown(a.replies()) + "\" / \"" + shown(g.replies()) + '"'};
    }
    return std::nullopt;
}

bool validUtf8(std::string_view s)
{
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (std::size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

/// What kind of difference @p d is: the first word of its description, and
/// for a cell whether its text or only its look differs. Minimizing keeps
/// the kind, or it can wander off to a different difference that a shorter
/// input also has, and the input no longer shows what it was minimized for.
std::string kindOf(const Divergence& d)
{
    const std::string_view what = d.what;
    std::string kind(what.substr(0, what.find(' ')));
    if (kind == "cell") {
        // cell X,Y: "A" ... / "B" ...
        const auto text = [&](std::size_t from) {
            const auto open = what.find('"', from);
            const auto close = open == std::string_view::npos ? open : what.find('"', open + 1);
            return close == std::string_view::npos ? std::string_view() : what.substr(open, close - open + 1);
        };
        const auto split = what.find(" / ");
        kind += split != std::string_view::npos && text(0) == text(split) ? " look" : " text";
    }
    return kind;
}

/// The shortest input that still differs in the same kind of way, by
/// removing pieces of halving size. An input of valid UTF-8 loses whole
/// characters only: a cut through a character makes a difference of its own,
/// in the decoding of broken bytes, and two halves of characters can join
/// into a third character.
std::string minimize(const std::string& input, const Options& o)
{
    Options one = o;
    one.chunk = std::max<std::size_t>(input.size(), 1);
    const bool keepUtf8 = validUtf8(input);
    const auto first = run(input, one);
    if (!first) return input; // it differs only where a chunk ends
    const std::string kind = kindOf(*first);
    std::vector<std::string_view> units;
    for (std::size_t i = 0; i < input.size();) {
        const auto c = static_cast<unsigned char>(input[i]);
        const std::size_t n = !keepUtf8 || c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        units.push_back(std::string_view(input).substr(i, n));
        i += n;
    }
    const auto joined = [](const std::vector<std::string_view>& parts) {
        std::string out;
        for (auto part : parts) out += part;
        return out;
    };
    for (std::size_t piece = units.size() / 2; piece >= 1; piece /= 2) {
        for (std::size_t at = 0; at + piece <= units.size();) {
            std::vector<std::string_view> shorter(units.begin(), units.begin() + std::ptrdiff_t(at));
            shorter.insert(shorter.end(), units.begin() + std::ptrdiff_t(at + piece), units.end());
            const auto d = run(joined(shorter), one);
            if (d && kindOf(*d) == kind) units = std::move(shorter);
            else at += piece;
        }
    }
    return joined(units);
}

std::string escaped(const std::string& s)
{
    std::string out;
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c >= 0x20 && c < 0x7F) out += char(c);
        else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        }
    }
    return out;
}

// Sequences that terminals parse, for --random: text, controls, and CSI,
// OSC and ESC sequences with parameters in the ranges that matter.
std::string randomInput(std::uint64_t seed, int length)
{
    std::mt19937_64 rng(seed);
    auto pick = [&](int n) { return int(rng() % std::uint64_t(n)); };
    auto num = [&] { return std::to_string(pick(4) == 0 ? pick(200) : pick(12)); };
    static const char* const csiFinals = "@ABCDEFGHIJKLMPSTXZ`abcdefghlmnrsu";
    static const char* const privateModes[] = {"1", "6", "7", "25", "47", "69", "1047", "1048", "1049", "2004", "2026"};
    static const char* const texts[] = {"a", "hello ", "x", "é", "é", "中", "\U0001f600", "\U0001f468‍\U0001f469",
                                        "─", "  ", "א", "​", "abcdefghij"};
    std::string out;
    for (int i = 0; i < length; ++i) {
        switch (pick(12)) {
        case 0:
        case 1:
        case 2:
        case 3: out += texts[pick(int(std::size(texts)))]; break;
        case 4: out += "\r\n\t\b\x0b\x0c"[pick(6)]; break;
        case 5: { // CSI with up to three parameters
            out += "\x1b[";
            const int params = pick(4);
            for (int p = 0; p < params; ++p) out += (p ? ";" : "") + num();
            out += csiFinals[pick(int(std::strlen(csiFinals)))];
            break;
        }
        case 6: { // SGR
            static const char* const sgr[] = {"0", "1", "2", "3", "4", "4:3", "5", "7", "8", "9", "21", "22", "23", "24", "27",
                                              "53", "31", "42", "38;5;196", "48;2;10;20;30", "58;5;3", "39", "49", "91", "107"};
            out += "\x1b[";
            out += sgr[pick(int(std::size(sgr)))];
            out += 'm';
            break;
        }
        case 7: // private modes
            out += std::string("\x1b[?") + privateModes[pick(int(std::size(privateModes)))] + (pick(2) ? 'h' : 'l');
            break;
        case 8: // scroll regions and margins
            out += "\x1b[" + num() + ";" + num() + (pick(2) ? "r" : "s");
            break;
        case 9: { // ESC
            static const char* const esc[] = {"7", "8", "D", "E", "M", "c", "#8", "(0", "(B", "=", ">", "H"};
            out += std::string("\x1b") + esc[pick(int(std::size(esc)))];
            break;
        }
        case 10: // insert mode, tabs
            out += std::string("\x1b[") + (pick(2) ? "4h" : "4l");
            break;
        case 11: // OSC title and hyperlinks, which change no cell but must not eat text
            out += pick(2) ? "\x1b]0;title\x07" : "\x1b]8;;https://x\x1b\\";
            break;
        }
    }
    return out;
}

void usage()
{
    std::fprintf(stderr, "Usage: afterglow_vtdiff [--cols N] [--rows N] [--chunk N] [--replies] [--expected FILE] [--minimize]\n"
                         "                        FILE|DIR... | --random SEED [--count N] [--length N]\n");
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--cols") o.cols = std::atoi(value());
        else if (arg == "--rows") o.rows = std::atoi(value());
        else if (arg == "--chunk") o.chunk = std::size_t(std::max(1, std::atoi(value())));
        else if (arg == "--replies") o.replies = true;
        else if (arg == "--minimize") o.minimize = true;
        else if (arg == "--expected") o.expected = value();
        else if (arg == "--random") o.seed = std::strtoull(value(), nullptr, 10);
        else if (arg == "--count") o.count = std::max(1, std::atoi(value()));
        else if (arg == "--length") o.length = std::max(1, std::atoi(value()));
        else if (arg.starts_with("--")) {
            usage();
            return 2;
        } else paths.emplace_back(arg);
    }
    if (o.cols < 2 || o.rows < 2 || o.cols > 1000 || o.rows > 1000 || (paths.empty() && !o.seed)) {
        usage();
        return 2;
    }

    std::map<std::string, std::string> expected;
    struct Pattern
    {
        std::regex input;
        std::optional<std::regex> difference;
        std::string text; ///< for the report
    };
    std::vector<Pattern> patterns;
    // "/REGEX/" from @p at: the regex, and where the text after it starts.
    // A "/" in the regex is escaped as "\/".
    const auto slashed = [](const std::string& line, std::size_t at) -> std::optional<std::pair<std::string, std::size_t>> {
        if (at >= line.size() || line[at] != '/') return std::nullopt;
        for (std::size_t i = at + 1; i < line.size(); ++i) {
            if (line[i] == '\\') ++i;
            else if (line[i] == '/' && (i + 1 == line.size() || line[i + 1] == ' '))
                return std::pair{line.substr(at + 1, i - at - 1), std::min(i + 2, line.size())};
        }
        return std::nullopt;
    };
    if (!o.expected.empty()) {
        std::ifstream file(o.expected);
        if (!file) {
            std::fprintf(stderr, "afterglow_vtdiff: cannot read %s\n", o.expected.c_str());
            return 2;
        }
        for (std::string line; std::getline(file, line);) {
            if (line.empty() || line[0] == '#') continue;
            if (line[0] == '/') {
                const auto input = slashed(line, 0);
                const auto difference = input ? slashed(line, input->second) : std::nullopt;
                if (!input) {
                    std::fprintf(stderr, "afterglow_vtdiff: bad line %s\n", line.c_str());
                    return 2;
                }
                const std::string text = '/' + input->first + '/' + (difference ? " /" + difference->first + '/' : "");
                try {
                    patterns.push_back({std::regex(input->first),
                                        difference ? std::optional(std::regex(difference->first)) : std::nullopt, text});
                } catch (const std::regex_error&) {
                    std::fprintf(stderr, "afterglow_vtdiff: bad pattern %s\n", text.c_str());
                    return 2;
                }
                continue;
            }
            const auto space = line.find(' ');
            expected[line.substr(0, space)] = space == std::string::npos ? "" : line.substr(space + 1);
        }
    }

    std::vector<std::pair<std::string, std::string>> inputs; // name, bytes
    for (const std::string& path : paths) {
        std::vector<std::filesystem::path> files;
        if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::directory_iterator(path))
                if (entry.is_regular_file()) files.push_back(entry.path());
            std::sort(files.begin(), files.end());
        } else {
            files.emplace_back(path);
        }
        for (const auto& file : files) {
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "afterglow_vtdiff: cannot read %s\n", file.c_str());
                return 2;
            }
            inputs.emplace_back(file.filename().string(), std::string(std::istreambuf_iterator<char>(in), {}));
        }
    }
    if (o.seed)
        for (int i = 0; i < o.count; ++i)
            inputs.emplace_back("random-" + std::to_string(*o.seed + std::uint64_t(i)), randomInput(*o.seed + std::uint64_t(i), o.length));

    int unexpected = 0, same = 0, known = 0;
    std::map<std::string, int> matched; // pattern -> inputs
    for (const auto& [name, bytes] : inputs) {
        const auto difference = run(bytes, o);
        const bool listed = expected.count(name) != 0;
        if (!difference) {
            ++same;
            if (listed) {
                std::printf("%s: no longer differs; remove it from the expected differences\n", name.c_str());
                ++unexpected;
            }
            continue;
        }
        if (listed) {
            ++known;
            continue;
        }
        std::string shortest, shortestWhat;
        if (o.minimize || !patterns.empty()) {
            const std::string bytesShortest = minimize(bytes, o);
            shortest = escaped(bytesShortest);
            Options one = o;
            one.chunk = std::max<std::size_t>(bytesShortest.size(), 1);
            const auto d = run(bytesShortest, one);
            shortestWhat = d ? d->what : difference->what;
        }
        const auto pattern = std::find_if(patterns.begin(), patterns.end(), [&](const Pattern& p) {
            return std::regex_search(shortest, p.input) && (!p.difference || std::regex_search(shortestWhat, *p.difference));
        });
        if (pattern != patterns.end()) {
            ++known;
            ++matched[pattern->text];
            continue;
        }
        ++unexpected;
        std::printf("%s: differs after %zu bytes: %s\n", name.c_str(), difference->offset, difference->what.c_str());
        if (!shortest.empty())
            std::printf("  shortest input: \"%s\"\n  its difference: %s\n", shortest.c_str(), shortestWhat.c_str());
    }
    for (const auto& [pattern, n] : matched) std::printf("expected %s: %d\n", pattern.c_str(), n);
    std::printf("afterglow_vtdiff: %d same, %d expected differences, %d unexpected\n", same, known, unexpected);
    return unexpected ? 1 : 0;
}
