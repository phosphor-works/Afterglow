// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/StateSync.h>

#include <AfterglowVt/Terminal.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <optional>
#include <set>

namespace AfterglowVt {

namespace {

// A frame: a version byte, then sections. A section: a tag byte, its length
// as a varint, its payload. Integers are LEB128 varints, strings a varint
// length and the bytes.
constexpr std::uint8_t Version = 1;

enum Tag : std::uint8_t {
    ModesTag = 1, ///< the modes
    ColorsTag = 2, ///< the palette, the default and protocol colors
    TextTag = 3, ///< the title and the working directory
    ResetTag = 4, ///< a screen starts over: size and the next history id
    StylesTag = 5, ///< style definitions of a screen
    LinksTag = 6, ///< hyperlink definitions of a screen
    RowsTag = 7, ///< visible rows of a screen
    AppendTag = 8, ///< history rows, oldest first, and the oldest id that exists
    PrependTag = 9, ///< older history rows, newest first
    CursorTag = 10, ///< the cursor of a screen
    ShellTag = 11, ///< OSC 133: the zone of the cursor, the marks and the semantic regions
    StatusTag = 12, ///< the program status records (OSC 7501)
    ContextsTag = 13, ///< the shell contexts (OSC 3008, iTerm2 RemoteHost)
    ExtraCursorsTag = 14, ///< the extra cursors and their colors
    ScrollsTag = 15, ///< the region scrolls since the last frame, oldest first
};

// The flags of a semantic region: its kind in the low bits.
constexpr std::uint8_t RegionOpen = 4;
constexpr std::uint8_t RegionExitCode = 8;

// The row flags.
constexpr std::uint8_t RowWrapped = 1;
constexpr std::uint8_t RowPromptStart = 2;
constexpr int RowLineSizeShift = 2;

class Out
{
public:
    explicit Out(std::string& s)
        : m_s(s)
    {
    }
    void byte(std::uint8_t v) { m_s.push_back(char(v)); }
    void varint(std::uint64_t v)
    {
        while (v >= 0x80) {
            m_s.push_back(char((v & 0x7f) | 0x80));
            v >>= 7;
        }
        m_s.push_back(char(v));
    }
    /// A signed integer, zigzag encoded.
    void signedVarint(std::int64_t v) { varint((std::uint64_t(v) << 1) ^ std::uint64_t(v >> 63)); }
    void u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) m_s.push_back(char((v >> (8 * i)) & 0xff));
    }
    void string(std::string_view v)
    {
        varint(v.size());
        m_s.append(v);
    }
    void rgb(Rgb c)
    {
        byte(c.r);
        byte(c.g);
        byte(c.b);
    }
    void optionalRgb(const Rgb* c)
    {
        byte(c ? 1 : 0);
        if (c) rgb(*c);
    }

private:
    std::string& m_s;
};

class In
{
public:
    explicit In(std::string_view s)
        : m_s(s)
    {
    }
    bool ok() const { return m_ok; }
    bool done() const { return m_at == m_s.size(); }
    bool fail()
    {
        m_ok = false;
        return false;
    }
    std::uint8_t byte()
    {
        if (m_at >= m_s.size()) {
            fail();
            return 0;
        }
        return std::uint8_t(m_s[m_at++]);
    }
    std::uint64_t varint()
    {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const std::uint8_t b = byte();
            if (!m_ok) return 0;
            v |= std::uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        fail();
        return 0;
    }
    /// A varint that must not exceed @p max.
    std::uint64_t bounded(std::uint64_t max)
    {
        const std::uint64_t v = varint();
        if (v > max) fail();
        return m_ok ? v : 0;
    }
    std::int64_t signedVarint()
    {
        const std::uint64_t v = varint();
        return std::int64_t(v >> 1) ^ -std::int64_t(v & 1);
    }
    /// A signed varint within [@p min, @p max].
    std::int64_t signedBounded(std::int64_t min, std::int64_t max)
    {
        const std::int64_t v = signedVarint();
        if (v < min || v > max) fail();
        return m_ok ? v : 0;
    }
    std::uint32_t u32()
    {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= std::uint32_t(byte()) << (8 * i);
        return v;
    }
    std::string_view bytes(std::size_t n)
    {
        if (n > m_s.size() - m_at) {
            fail();
            return {};
        }
        const std::string_view v = m_s.substr(m_at, n);
        m_at += n;
        return v;
    }
    std::string_view string(std::size_t max) { return bytes(bounded(max)); }
    Rgb rgb()
    {
        const std::uint8_t r = byte();
        const std::uint8_t g = byte();
        const std::uint8_t b = byte();
        return Rgb{r, g, b};
    }
    std::optional<Rgb> optionalRgb()
    {
        const std::uint8_t has = byte();
        if (has > 1) fail();
        if (!has) return std::nullopt;
        return rgb();
    }

private:
    std::string_view m_s;
    std::size_t m_at = 0;
    bool m_ok = true;
};

void section(std::string& out, Tag tag, std::string_view payload)
{
    out.push_back(char(tag));
    Out(out).varint(payload.size());
    out.append(payload);
}

std::uint64_t fnv(std::string_view bytes)
{
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : bytes) {
        h ^= std::uint8_t(c);
        h *= 1099511628211ull;
    }
    return h;
}

// The longest strings that a reader takes: the title and the directory as
// the terminal bounds them, and an OSC 8 link.
constexpr std::size_t MaxText = 64 * 1024;
constexpr std::size_t MaxLinkText = StyleTable::MaxLinkBytes;
/// The fields of a status record and of a context: the terminal takes none
/// from a sequence longer than this.
constexpr std::size_t MaxFieldText = 4096;

using StyleDefinitions = std::unordered_map<std::uint32_t, Style>;
using LinkDefinitions = std::unordered_map<std::uint32_t, std::pair<std::string, std::string>>;

/// Encodes @p row and adds the style ids that its cells use to @p styles.
void encodeRow(std::string& out, const Row& row, std::set<std::uint32_t>& styles)
{
    Out o(out);
    o.byte(std::uint8_t((row.wrapped ? RowWrapped : 0) | (row.promptStart ? RowPromptStart : 0)
                        | (std::uint8_t(row.lineSize) << RowLineSizeShift)));
    o.varint(row.cells.size());
    for (const Cell& c : row.cells) {
        // The raw fields: the reader checks each part and maps the style id.
        o.varint(c.rawCodepoint());
        o.varint(c.rawPacked());
        if (c.styleId() != StyleTable::DefaultId) styles.insert(c.styleId());
    }
    if (row.graphemes) {
        // In column order, so that equal rows encode to equal bytes.
        std::vector<std::pair<std::uint16_t, const std::u32string*>> entries;
        for (const auto& [x, extra] : *row.graphemes) entries.emplace_back(x, &extra);
        std::sort(entries.begin(), entries.end());
        o.varint(entries.size());
        for (const auto& [x, extra] : entries) {
            o.varint(x);
            o.varint(extra->size());
            for (const char32_t cp : *extra) o.varint(cp);
        }
    } else {
        o.varint(0);
    }
    if (row.multicell) {
        std::vector<std::pair<std::uint16_t, MulticellParams>> entries(row.multicell->begin(), row.multicell->end());
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        o.varint(entries.size());
        for (const auto& [x, p] : entries) {
            o.varint(x);
            for (const std::uint8_t v : {p.scale, p.width, p.num, p.den, p.valign, p.halign}) o.byte(v);
        }
    } else {
        o.varint(0);
    }
}

std::string encodeStyle(const Style& s)
{
    std::string out;
    Out o(out);
    o.u32(s.fg.bits());
    o.u32(s.bg.bits());
    o.u32(s.underlineColor.bits());
    o.varint(s.attrs);
    o.byte(std::uint8_t(s.underline));
    o.varint(s.link);
    return out;
}

/// The style and link definitions that a frame carries for one screen,
/// ahead of the rows that use them.
class Definitions
{
public:
    /// @p styles and @p links are the definitions that the replica has.
    Definitions(const StyleTable& table, StyleDefinitions& styles, LinkDefinitions& links)
        : m_table(table)
        , m_known(styles)
        , m_knownLinks(links)
    {
    }

    /// Adds style @p id and its link, when the replica lacks them or has
    /// them with another meaning.
    void define(std::uint32_t id)
    {
        const Style& style = m_table.get(id);
        if (style.link) {
            std::pair<std::string, std::string> link{std::string(m_table.linkUri(style.link)),
                                                     std::string(m_table.linkId(style.link))};
            const auto known = m_knownLinks.find(style.link);
            if (known == m_knownLinks.end() || known->second != link) {
                Out o(m_links);
                o.varint(style.link);
                o.string(link.first);
                o.string(link.second);
                ++m_linkCount;
                m_knownLinks[style.link] = std::move(link);
            }
        }
        const auto known = m_known.find(id);
        if (known == m_known.end() || !(known->second == style)) {
            Out(m_styles).varint(id);
            m_styles += encodeStyle(style);
            ++m_styleCount;
            m_known[id] = style;
        }
    }

    /// The bytes of the definitions that wait for emit().
    std::size_t bytes() const { return m_styles.size() + m_links.size(); }

    /// The sections of screen @p which for the definitions added since the
    /// last call: the links first, which the styles name.
    void emit(std::string& out, int which)
    {
        if (m_linkCount) {
            std::string payload(1, char(which));
            Out(payload).varint(m_linkCount);
            section(out, LinksTag, payload + m_links);
            m_links.clear();
            m_linkCount = 0;
        }
        if (m_styleCount) {
            std::string payload(1, char(which));
            Out(payload).varint(m_styleCount);
            section(out, StylesTag, payload + m_styles);
            m_styles.clear();
            m_styleCount = 0;
        }
    }

private:
    const StyleTable& m_table;
    StyleDefinitions& m_known;
    LinkDefinitions& m_knownLinks;
    std::string m_styles;
    std::string m_links;
    std::size_t m_styleCount = 0;
    std::size_t m_linkCount = 0;
};

std::optional<Color> colorOf(std::uint32_t bits)
{
    const std::uint32_t kind = bits >> 24;
    if (kind > std::uint32_t(Color::Kind::Rgb)) return std::nullopt;
    if (kind == std::uint32_t(Color::Kind::Default)) return Color();
    if (kind == std::uint32_t(Color::Kind::Palette)) return Color::palette(std::uint8_t(bits & 0xff));
    return Color::rgb(std::uint8_t((bits >> 16) & 0xff), std::uint8_t((bits >> 8) & 0xff), std::uint8_t(bits & 0xff));
}

std::optional<Style> decodeStyle(In& in)
{
    Style s;
    const auto fg = colorOf(in.u32());
    const auto bg = colorOf(in.u32());
    const auto ul = colorOf(in.u32());
    s.attrs = std::uint16_t(in.bounded(0xffff));
    const std::uint8_t underline = in.byte();
    s.link = std::uint32_t(in.bounded(0xffffffffu));
    if (!in.ok() || !fg || !bg || !ul || underline > std::uint8_t(UnderlineStyle::Dashed)) return std::nullopt;
    s.fg = *fg;
    s.bg = *bg;
    s.underlineColor = *ul;
    s.underline = UnderlineStyle(underline);
    return s;
}

// The modes, in a fixed order. A new mode goes at the end, with a new codec.
// Modes is 34 flags and 4 bytes: a new field changes its size, and must be
// added here or to encodeModes().
static_assert(sizeof(Modes) == 38, "a field of Modes that the codec does not carry");
std::array<bool*, 34> modeFlags(Modes& m)
{
    return {&m.insert, &m.newline, &m.cursorKeysApp, &m.keypadApp, &m.reverseVideo, &m.originMode, &m.autowrap,
            &m.cursorVisible, &m.cursorBlink, &m.altScreen, &m.leftRightMargins, &m.bracketedPaste, &m.focusEvents,
            &m.synchronizedOutput, &m.graphemeClustering, &m.altScroll, &m.reverseWrap, &m.reverseWrapExtended,
            &m.moreFix, &m.vt52, &m.pasteEvents, &m.themeReports, &m.visibilityReports, &m.sizeReports, &m.sixelDisplay,
            &m.sixelPrivateColors, &m.sixelCursorRight, &m.allow132, &m.columns132, &m.noClearOnColumnChange,
            &m.backarrowSendsBs, &m.smoothScroll, &m.printFormFeed, &m.printExtent};
}

std::string encodeModes(Modes m)
{
    std::uint64_t bits = 0;
    const auto flags = modeFlags(m);
    for (std::size_t i = 0; i < flags.size(); ++i) bits |= std::uint64_t(*flags[i]) << i;
    std::string out;
    Out o(out);
    o.varint(bits);
    o.byte(m.modifyOtherKeys);
    o.byte(std::uint8_t(m.mouseTracking));
    o.byte(std::uint8_t(m.mouseFormat));
    o.byte(m.kittyKeyboard);
    return out;
}

bool decodeModes(In& in, Modes& m)
{
    Modes decoded = m;
    const auto flags = modeFlags(decoded);
    const std::uint64_t bits = in.varint();
    if (bits >> flags.size()) return false;
    for (std::size_t i = 0; i < flags.size(); ++i) *flags[i] = (bits >> i) & 1;
    decoded.modifyOtherKeys = in.byte();
    const std::uint8_t tracking = in.byte();
    const std::uint8_t format = in.byte();
    decoded.kittyKeyboard = in.byte();
    if (!in.ok() || tracking > std::uint8_t(MouseTracking::Any) || format > std::uint8_t(MouseFormat::SgrPixels)) return false;
    decoded.mouseTracking = MouseTracking(tracking);
    decoded.mouseFormat = MouseFormat(format);
    m = decoded;
    return true;
}

} // namespace

// ---------------------------------------------------------------- writer ---

void RegionScrolls::add(bool altScreen, const RegionScroll& scroll)
{
    if (m_lost || scroll.delta == 0) return;
    if (!m_entries.empty()) {
        Entry& last = m_entries.back();
        const RegionScroll& l = last.scroll;
        // Two scrolls of one region in one direction are one longer scroll:
        // a row that the first moved and kept is on its way out, or to where
        // the second takes it.
        const long long delta = (long long)l.delta + scroll.delta;
        if (last.altScreen == altScreen && l.top == scroll.top && l.bottom == scroll.bottom && l.left == scroll.left
            && l.right == scroll.right && l.fullWidth == scroll.fullWidth && (l.delta < 0) == (scroll.delta < 0)
            && delta >= INT_MIN && delta <= INT_MAX) {
            last.scroll.delta = int(delta);
            return;
        }
    }
    if (m_entries.size() >= Max) {
        m_entries.clear();
        m_lost = true;
        return;
    }
    m_entries.push_back(Entry{altScreen, scroll});
}

void RegionScrolls::clear()
{
    m_entries.clear();
    m_lost = false;
}

void StateWriter::reset()
{
    m_screens[0] = ScreenState();
    m_screens[1] = ScreenState();
    m_modes.clear();
    m_colors.clear();
    m_text.clear();
    m_cursor.clear();
    m_shell.clear();
    m_status.clear();
    m_contexts.clear();
    m_extraCursors.clear();
    m_started = false;
}

bool StateWriter::pending() const
{
    return m_screens[0].historyPending || m_screens[1].historyPending || m_screens[0].rowsPending || m_screens[1].rowsPending;
}

std::string StateWriter::update(const Terminal& t, std::size_t maxBytes, const RegionScrolls* scrolls)
{
    std::string out(1, char(Version));
    const bool first = !m_started;
    m_started = true;

    std::string payload;
    // The scrolls go first, before the modes that may switch away from the
    // screen that scrolled.
    if (!first && scrolls && !scrolls->empty()) {
        Out o(payload);
        o.byte(scrolls->m_lost ? 1 : 0);
        o.varint(scrolls->m_entries.size());
        for (const RegionScrolls::Entry& e : scrolls->m_entries) {
            o.byte(e.altScreen ? 1 : 0);
            o.varint(e.scroll.top);
            o.varint(e.scroll.bottom);
            o.varint(std::uint64_t(e.scroll.left));
            o.varint(std::uint64_t(e.scroll.right));
            o.byte(e.scroll.fullWidth ? 1 : 0);
            o.signedVarint(e.scroll.delta);
        }
        section(out, ScrollsTag, payload);
    }

    payload = encodeModes(t.m_modes);
    // The updates that programs began (mode 2026): a renderer gives each its
    // own timeout, also one that begins right after another ended.
    Out(payload).varint(t.m_synchronizedOutputSerial);
    if (first || payload != m_modes) {
        section(out, ModesTag, payload);
        m_modes = std::move(payload);
    }

    payload.clear();
    {
        Out o(payload);
        for (const Rgb& c : t.m_palette) o.rgb(c);
        o.rgb(t.m_defaultFg);
        o.rgb(t.m_defaultBg);
        o.optionalRgb(t.cursorColor());
        o.optionalRgb(t.cursorTextColor());
        o.optionalRgb(t.selectionForeground());
        o.optionalRgb(t.selectionBackground());
        o.optionalRgb(t.visualBellColor());
        for (const auto& transparent : t.m_protocolColors.transparent) {
            o.byte(transparent ? 1 : 0);
            if (!transparent) continue;
            o.rgb(transparent->color);
            o.varint(std::uint64_t(std::lround(std::clamp(transparent->opacity, 0.0, 1.0) * 65535.0)));
        }
        o.byte(std::uint8_t(t.m_cursorShape));
    }
    if (first || payload != m_colors) {
        section(out, ColorsTag, payload);
        m_colors = std::move(payload);
    }

    payload.clear();
    Out(payload).string(std::string_view(t.m_title).substr(0, MaxText));
    Out(payload).string(std::string_view(t.m_cwd).substr(0, MaxText));
    if (first || payload != m_text) {
        section(out, TextTag, payload);
        m_text = std::move(payload);
    }

    // OSC 133, for the last output, a click on the prompt and the regions
    // that `afterglow ctl` lists. The rows are stable ids of the primary
    // screen, which the replica's are.
    payload.clear();
    {
        Out o(payload);
        o.byte(std::uint8_t(t.m_semanticZone));
        o.byte(t.m_promptClickEvents ? 1 : 0);
        o.byte(t.m_commandRunning ? 1 : 0);
        for (const Terminal::Mark* m : {&t.m_inputStart, &t.m_outputStart, &t.m_outputEnd}) {
            o.byte(m->valid ? 1 : 0);
            if (!m->valid) continue;
            o.varint(m->row);
            o.varint(std::uint64_t(m->col));
        }
        o.varint(t.m_nextSemanticId);
        o.varint(t.m_semanticRegions.size());
        for (const SemanticRegion& r : t.m_semanticRegions) {
            o.varint(r.id);
            o.byte(std::uint8_t(std::uint8_t(r.kind) | (r.open ? RegionOpen : 0) | (r.exitCode ? RegionExitCode : 0)));
            o.varint(r.start.row);
            o.varint(std::uint64_t(r.start.col));
            o.varint(r.end.row);
            o.varint(std::uint64_t(r.end.col));
            if (r.exitCode) o.signedVarint(*r.exitCode);
        }
    }
    if (first || payload != m_shell) {
        section(out, ShellTag, payload);
        m_shell = std::move(payload);
    }

    payload.clear();
    {
        Out o(payload);
        o.varint(t.m_programStatus.records().size());
        for (const StatusRecord& r : t.m_programStatus.records()) {
            o.string(r.id);
            o.byte(std::uint8_t(r.state));
            o.byte(std::uint8_t(r.kind));
            o.signedVarint(r.progress);
            o.string(r.app);
            o.string(r.title);
            o.string(r.msg);
            o.varint(r.serial);
        }
    }
    if (first || payload != m_status) {
        section(out, StatusTag, payload);
        m_status = std::move(payload);
    }

    payload.clear();
    {
        Out o(payload);
        o.varint(t.m_contexts.stack().size());
        for (const ShellContext& c : t.m_contexts.stack()) {
            o.string(c.id);
            o.byte(c.iterm2RemoteHost ? 1 : 0);
            for (const std::string* field : {&c.type, &c.user, &c.hostname, &c.targetUser, &c.targetHost, &c.container, &c.vm}) {
                o.string(*field);
            }
        }
    }
    if (first || payload != m_contexts) {
        section(out, ContextsTag, payload);
        m_contexts = std::move(payload);
    }

    payload.clear();
    {
        Out o(payload);
        for (const ExtraCursorColor* c : {&t.m_extraCursorColor, &t.m_extraCursorTextColor}) {
            o.byte(std::uint8_t(c->mode));
            o.u32(c->color.bits());
        }
        o.varint(t.m_extraCursors.size());
        for (const auto& [point, shape] : t.m_extraCursors) {
            o.varint(std::uint64_t(point.first));
            o.varint(std::uint64_t(point.second));
            o.varint(std::uint64_t(shape));
        }
    }
    if (first || payload != m_extraCursors) {
        section(out, ExtraCursorsTag, payload);
        m_extraCursors = std::move(payload);
    }

    payload.clear();
    for (const Screen* s : {t.m_primary.get(), t.m_alternate.get()}) {
        Out o(payload);
        o.varint(std::uint64_t(s->cursor.x));
        o.varint(std::uint64_t(s->cursor.y));
        o.byte(s->cursor.pendingWrap ? 1 : 0);
    }
    // The visible rows of both screens first, then their history in the
    // bytes that are left.
    bool reset = writeRows(out, t, 0, m_screens[0], maxBytes);
    reset = writeRows(out, t, 1, m_screens[1], maxBytes) || reset;
    writeHistory(out, t, 0, m_screens[0], maxBytes);
    writeHistory(out, t, 1, m_screens[1], maxBytes);
    // The cursors go after the rows, which the reader checks them against.
    // A screen that started over lost its cursor on the replica.
    if (first || reset || payload != m_cursor) {
        section(out, CursorTag, payload);
        m_cursor = std::move(payload);
    }
    return out.size() == 1 ? std::string() : out;
}

bool StateWriter::writeRows(std::string& out, const Terminal& t, int which, ScreenState& st, std::size_t maxBytes)
{
    bool reset = false;
    const Screen& s = which == 0 ? *t.m_primary : *t.m_alternate;
    const std::uint64_t total = s.rowsDropped() + s.historySize();
    if (!st.synced || st.cols != s.cols() || st.rows != s.rows() || st.epoch != s.layoutEpoch() || total < st.to) {
        // A new size or layout rebuilt the rows: the replica starts over,
        // with the history from the newest row back.
        st = ScreenState();
        st.synced = true;
        st.cols = s.cols();
        st.rows = s.rows();
        st.epoch = s.layoutEpoch();
        st.from = st.to = total;
        st.rowHashes.assign(std::size_t(s.rows()), 0);
        std::string payload;
        Out o(payload);
        o.byte(std::uint8_t(which));
        o.varint(std::uint64_t(s.cols()));
        o.varint(std::uint64_t(s.rows()));
        o.varint(total);
        section(out, ResetTag, payload);
        reset = true;
    }

    // The rows that changed, with the styles and links that they use. Rows
    // past about maxBytes keep their old hash and go in the next frames, as
    // history does: a screen of rows that are long to encode (many
    // combining marks, links) never makes a frame too large for its
    // channel. Each frame carries at least one row.
    std::vector<std::pair<int, std::string>> changed;
    std::set<std::uint32_t> used;
    std::size_t bytes = out.size();
    st.rowsPending = false;
    // From the first row that the last frame left out: rows at the top that
    // change in every frame do not keep the others waiting.
    const int start = st.rowStart < s.rows() ? st.rowStart : 0;
    st.rowStart = 0;
    for (int i = 0; i < s.rows(); ++i) {
        const int y = (start + i) % s.rows();
        std::string row;
        std::set<std::uint32_t> rowStyles;
        encodeRow(row, s.row(y), rowStyles);
        // What the row means, not only its bytes: between two frames a
        // style id can be freed and taken again by another style, and a
        // row rewritten with the same characters and that id.
        std::string meaning = row;
        for (const std::uint32_t id : rowStyles) {
            const Style& style = s.styles().get(id);
            meaning += encodeStyle(style);
            if (style.link) {
                Out o(meaning);
                o.string(s.styles().linkUri(style.link));
                o.string(s.styles().linkId(style.link));
            }
        }
        const std::uint64_t h = fnv(meaning) | 1; // never 0, the mark of a missing row
        if (st.rowHashes[std::size_t(y)] == h) continue;
        // The meaning holds the row and its definitions: about what the row
        // adds to the frame.
        if (!changed.empty() && bytes + meaning.size() > maxBytes) {
            if (!st.rowsPending) st.rowStart = y;
            st.rowsPending = true;
            continue;
        }
        bytes += meaning.size();
        st.rowHashes[std::size_t(y)] = h;
        used.insert(rowStyles.begin(), rowStyles.end());
        changed.emplace_back(y, std::move(row));
    }

    // The definitions that the replica lacks, or has with another meaning:
    // a row that uses a changed one has a new hash, so it goes again too.
    Definitions definitions(s.styles(), st.styles, st.links);
    for (const std::uint32_t id : used) definitions.define(id);
    definitions.emit(out, which);
    if (!changed.empty()) {
        std::string payload(1, char(which));
        Out o(payload);
        o.varint(changed.size());
        for (const auto& [y, row] : changed) {
            o.varint(std::uint64_t(y));
            payload += row;
        }
        section(out, RowsTag, payload);
    }
    return reset;
}

void StateWriter::writeHistory(std::string& out, const Terminal& t, int which, ScreenState& st, std::size_t maxBytes)
{
    const Screen& s = which == 0 ? *t.m_primary : *t.m_alternate;
    const std::uint64_t oldest = s.rowsDropped();
    const std::uint64_t total = oldest + s.historySize();

    // The rows that scrolled in since the last frame, oldest first, then
    // older rows that the replica never had, newest first.
    const bool gap = st.to < oldest;
    if (gap) {
        // Rows scrolled in and out again before they could be sent: the
        // replica's history ends there, and nothing older remains.
        st.from = st.to = oldest;
    }
    if (st.from < oldest) st.from = oldest;
    Definitions definitions(s.styles(), st.styles, st.links);
    std::string appends;
    std::size_t appendCount = 0;
    std::string prepends;
    std::size_t prependCount = 0;
    std::string rowBytes;
    // A row goes when the frame, with the definitions that its rows need,
    // stays within about maxBytes. The first row always goes, so the
    // history advances even past a row larger than maxBytes.
    const auto fits = [&](std::size_t bytes) {
        return appendCount + prependCount == 0
            || out.size() + definitions.bytes() + appends.size() + prepends.size() + bytes + 64 <= maxBytes;
    };
    while (st.to < total) {
        rowBytes.clear();
        std::set<std::uint32_t> rowStyles;
        encodeRow(rowBytes, s.absoluteRow(std::size_t(st.to - oldest)), rowStyles);
        if (!fits(rowBytes.size() + 10)) break;
        for (const std::uint32_t id : rowStyles) definitions.define(id);
        Out o(appends);
        o.varint(st.to);
        appends += rowBytes;
        ++appendCount;
        ++st.to;
    }
    while (st.to == total && st.from > oldest) {
        rowBytes.clear();
        std::set<std::uint32_t> rowStyles;
        encodeRow(rowBytes, s.absoluteRow(std::size_t(st.from - 1 - oldest)), rowStyles);
        if (!fits(rowBytes.size() + 10)) break;
        for (const std::uint32_t id : rowStyles) definitions.define(id);
        Out o(prepends);
        o.varint(st.from - 1);
        prepends += rowBytes;
        ++prependCount;
        --st.from;
    }
    // History rows take their definitions when the replica installs them,
    // so a later change of an id leaves them as they were.
    definitions.emit(out, which);
    if (appendCount || gap || oldest != st.oldestSent) {
        std::string payload(1, char(which));
        Out o(payload);
        o.varint(oldest);
        o.varint(appendCount);
        payload += appends;
        section(out, AppendTag, payload);
        st.oldestSent = oldest;
    }
    if (prependCount) {
        std::string payload(1, char(which));
        Out(payload).varint(prependCount);
        payload += prepends;
        section(out, PrependTag, payload);
    }
    st.historyPending = st.to < total || st.from > oldest;
}

// ---------------------------------------------------------------- reader ---

bool StateReader::apply(Terminal& t, std::string_view frame, std::string* error)
{
    const auto fail = [error](std::string text) {
        if (error) *error = std::move(text);
        return false;
    };
    if (frame.empty()) return true;
    if (std::uint8_t(frame[0]) != Version) {
        return fail("the state frame has version " + std::to_string(std::uint8_t(frame[0])) + ", and this end reads "
                    + std::to_string(Version));
    }
    In in(frame.substr(1));
    const auto screenOf = [&t](std::uint8_t which) -> Screen* {
        return which == 0 ? t.m_primary.get() : which == 1 ? t.m_alternate.get() : nullptr;
    };

    // A row from the frame, its styles taken in the replica's table.
    const auto decodeRow = [&](In& r, Screen& s, ScreenState& st, Row& row, bool history) {
        const std::uint8_t flags = r.byte();
        if (flags >> 4) return r.fail();
        row.wrapped = flags & RowWrapped;
        row.promptStart = flags & RowPromptStart;
        row.lineSize = LineSize((flags >> RowLineSizeShift) & 3);
        // A history row can be shorter: it drops its trailing blank cells.
        const std::uint64_t count = r.bounded(std::uint64_t(s.cols()));
        if (!r.ok() || (!history && count != std::uint64_t(s.cols()))) return r.fail();
        row.cells.resize(std::size_t(count));
        bool plain = true;
        for (Cell& c : row.cells) {
            const std::uint32_t cp = std::uint32_t(r.bounded(0xffffffffu));
            const std::uint32_t packed = std::uint32_t(r.bounded(0xffffffffu));
            if (!r.ok()) return false;
            c = Cell::fromRaw(cp, packed);
            const std::uint32_t theirs = c.styleId();
            // The writer's id is no reference in the replica's table: the
            // cell holds none until it takes its own below, so a row that
            // fails half way releases only what it took.
            c.setStyleId(StyleTable::DefaultId);
            if (c.codepoint() > 0x10FFFF || c.width() > 2 || (packed >> 28) != 0) return r.fail();
            std::uint32_t ours = StyleTable::DefaultId;
            if (theirs != StyleTable::DefaultId) {
                const auto def = st.styles.find(theirs);
                if (def == st.styles.end()) return r.fail();
                Style style = def->second;
                if (style.link) {
                    const auto link = st.links.find(style.link);
                    if (link == st.links.end()) return r.fail();
                    // A cached link of the replica holds only while it still
                    // names the same link: a freed index can be taken again.
                    std::uint32_t& cached = st.linkCache[style.link];
                    if (!cached || s.m_styles.linkUri(cached) != link->second.first
                        || s.m_styles.linkId(cached) != link->second.second) {
                        cached = s.m_styles.internLink(link->second.first, link->second.second);
                    }
                    style.link = cached;
                }
                ours = s.m_styles.acquire(style);
                plain = false;
            }
            c.setStyleId(ours);
        }
        // A cell keeps Screen::MaxGraphemeExtra codepoints, and the text of a
        // sizing block up to that many for each column of its row. The text
        // stays whole on one cell when a narrower width flattens the block.
        constexpr std::uint64_t MaxExtra = std::uint64_t(Screen::MaxColumns) * Screen::MaxGraphemeExtra;
        const std::uint64_t graphemes = r.bounded(count);
        for (std::uint64_t i = 0; i < graphemes && r.ok(); ++i) {
            const std::uint64_t x = r.bounded(count ? count - 1 : 0);
            const std::uint64_t n = r.bounded(MaxExtra);
            if (!r.ok() || count == 0) return r.fail();
            std::u32string extra;
            for (std::uint64_t k = 0; k < n && r.ok(); ++k) {
                const std::uint64_t cp = r.bounded(0x10FFFF);
                extra.push_back(char32_t(cp));
            }
            if (!row.graphemes) row.graphemes = std::make_unique<GraphemeMap>();
            (*row.graphemes)[std::uint16_t(x)] = std::move(extra);
        }
        const std::uint64_t multicells = r.bounded(count);
        for (std::uint64_t i = 0; i < multicells && r.ok(); ++i) {
            const std::uint64_t x = r.bounded(count ? count - 1 : 0);
            MulticellParams p;
            p.scale = r.byte();
            p.width = r.byte();
            p.num = r.byte();
            p.den = r.byte();
            p.valign = r.byte();
            p.halign = r.byte();
            if (!r.ok() || count == 0 || p.scale < 1 || p.scale > 7 || p.width < 1 || p.width > 7 || p.num > 15
                || p.den > 15 || p.valign > 2 || p.halign > 2) {
                return r.fail();
            }
            if (!row.multicell) row.multicell = std::make_unique<MulticellMap>();
            (*row.multicell)[std::uint16_t(x)] = p;
        }
        // Only history rows keep the plain mark (Row::plain).
        row.plain = history && plain;
        return r.ok();
    };
    // A row that failed half way holds references: release them.
    const auto discard = [](Screen& s, Row& row) {
        row.plain = false;
        s.releaseRow(row);
    };

    bool cursorsSeen = false;
    std::string cursors;
    while (!in.done()) {
        const std::uint8_t tag = in.byte();
        const std::string_view payload = in.bytes(std::size_t(in.bounded(frame.size())));
        if (!in.ok()) return fail("the state frame is cut short");
        In p(payload);
        switch (tag) {
        case ModesTag: {
            if (!decodeModes(p, t.m_modes)) return fail("the state frame has bad modes");
            const std::uint64_t serial = p.varint();
            if (!p.ok() || !p.done()) return fail("the state frame has bad modes");
            t.m_synchronizedOutputSerial = serial;
            t.m_active = t.m_modes.altScreen ? t.m_alternate.get() : t.m_primary.get();
            break;
        }
        case ColorsTag: {
            std::array<Rgb, 256> palette;
            for (Rgb& c : palette) c = p.rgb();
            const Rgb fg = p.rgb();
            const Rgb bg = p.rgb();
            const auto cursor = p.optionalRgb();
            const auto cursorText = p.optionalRgb();
            const auto selectionFg = p.optionalRgb();
            const auto selectionBg = p.optionalRgb();
            const auto bell = p.optionalRgb();
            std::array<std::optional<Terminal::TransparentColor>, 8> transparent;
            for (auto& entry : transparent) {
                const std::uint8_t has = p.byte();
                if (has > 1) return fail("the state frame has bad colors");
                if (!has) continue;
                const Rgb c = p.rgb();
                const double opacity = double(p.bounded(65535)) / 65535.0;
                entry = Terminal::TransparentColor{c, opacity};
            }
            const std::uint8_t shape = p.byte();
            if (!p.ok() || !p.done() || shape > std::uint8_t(CursorShape::Bar)) return fail("the state frame has bad colors");
            t.m_palette = palette;
            t.m_defaultFg = fg;
            t.m_defaultBg = bg;
            // The getters of the replica then give what the writer's gave:
            // a dynamic flag stands for "none", whatever the options say.
            t.m_protocolColors.cursorDynamic = !cursor;
            t.m_hasCursorColor = cursor.has_value();
            if (cursor) t.m_cursorColor = *cursor;
            t.m_protocolColors.cursorTextSet = true;
            t.m_protocolColors.cursorText = cursorText;
            t.m_protocolColors.selectionFgDynamic = !selectionFg;
            t.m_hasSelectionFg = selectionFg.has_value();
            if (selectionFg) t.m_selectionFg = *selectionFg;
            t.m_protocolColors.selectionBgDynamic = !selectionBg;
            t.m_hasSelectionBg = selectionBg.has_value();
            if (selectionBg) t.m_selectionBg = *selectionBg;
            t.m_protocolColors.bell = bell;
            t.m_protocolColors.transparent = transparent;
            t.m_cursorShape = CursorShape(shape);
            break;
        }
        case TextTag: {
            const std::string_view title = p.string(MaxText);
            const std::string_view cwd = p.string(MaxText);
            if (!p.ok() || !p.done()) return fail("the state frame has bad text");
            t.m_title = title;
            t.m_cwd = cwd;
            break;
        }
        case ResetTag: {
            Screen* s = screenOf(p.byte());
            const std::uint64_t cols = p.bounded(std::uint64_t(Screen::MaxColumns));
            const std::uint64_t rows = p.bounded(Screen::MaxCells);
            const std::uint64_t next = p.varint();
            if (!s || !p.ok() || !p.done() || cols < 2 || rows < 1 || cols * rows > Screen::MaxCells) {
                return fail("the state frame has a bad screen reset");
            }
            ScreenState& st = m_screens[s == t.m_primary.get() ? 0 : 1];
            s->clearHistory();
            for (int y = 0; y < s->rows(); ++y) {
                discard(*s, s->row(y));
                s->row(y) = Row(s->cols());
            }
            if (int(cols) != s->cols() || int(rows) != s->rows()) {
                s->resize(int(cols), int(rows), false);
                // A resize can move rows into the history.
                s->clearHistory();
            }
            st = ScreenState();
            st.next = next;
            st.from = next;
            s->m_rowsDropped = next;
            s->m_viewportOffset = 0;
            break;
        }
        case StylesTag:
        case LinksTag: {
            const std::uint8_t which = p.byte();
            if (which > 1) return fail("the state frame names no screen");
            ScreenState& st = m_screens[which];
            const std::uint64_t count = p.bounded(payload.size());
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                const std::uint32_t id = std::uint32_t(p.bounded(tag == StylesTag ? StyleTable::MaxId : 0xffffffffu));
                if (tag == StylesTag) {
                    const auto style = decodeStyle(p);
                    if (!style || id == StyleTable::DefaultId) return fail("the state frame has a bad style");
                    st.styles[id] = *style;
                } else {
                    const std::string_view uri = p.string(MaxLinkText);
                    const std::string_view linkId = p.string(MaxLinkText);
                    // An OSC 8 link always has a URI. The link cache also
                    // needs one: a freed link of the replica has an empty URI.
                    if (!p.ok() || id == 0 || uri.empty()) return fail("the state frame has a bad link");
                    st.links[id] = {std::string(uri), std::string(linkId)};
                }
            }
            if (!p.ok() || !p.done()) return fail("the state frame has bad definitions");
            break;
        }
        case RowsTag: {
            const std::uint8_t which = p.byte();
            Screen* s = screenOf(which);
            if (!s) return fail("the state frame names no screen");
            ScreenState& st = m_screens[which];
            const std::uint64_t count = p.bounded(std::uint64_t(s->rows()));
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                const std::uint64_t y = p.bounded(std::uint64_t(s->rows() - 1));
                Row row;
                if (!p.ok() || !decodeRow(p, *s, st, row, false)) {
                    discard(*s, row);
                    return fail("the state frame has a bad row");
                }
                Row& target = s->row(int(y));
                discard(*s, target);
                target = std::move(row);
            }
            if (!p.ok() || !p.done()) return fail("the state frame has bad rows");
            break;
        }
        case AppendTag: {
            const std::uint8_t which = p.byte();
            Screen* s = screenOf(which);
            if (!s) return fail("the state frame names no screen");
            ScreenState& st = m_screens[which];
            const std::uint64_t oldest = p.varint();
            const std::uint64_t count = p.bounded(payload.size());
            // A view scrolled back into the history stays on its rows, as it
            // does when a terminal scrolls rows into its own history.
            const std::size_t viewport = s->m_viewportOffset;
            std::size_t added = 0;
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                const std::uint64_t id = p.varint();
                Row row;
                if (!p.ok() || id < st.next || !decodeRow(p, *s, st, row, true)) {
                    discard(*s, row);
                    return fail("the state frame has a bad history row");
                }
                if (id > st.next) {
                    // The rows in between are gone on the writer's side, and
                    // with them every older row.
                    s->clearHistory();
                    st.next = id;
                    st.from = id;
                }
                if (s->m_maxScrollback == 0) {
                    discard(*s, row);
                } else {
                    s->m_rows.insert(s->m_rows.begin() + std::ptrdiff_t(s->historySize()), std::move(row));
                    ++added;
                }
                st.next = id + 1;
            }
            if (!p.ok() || !p.done()) return fail("the state frame has bad history");
            if (oldest > st.next) {
                // Every row that the replica has is gone on the writer's
                // side, and so are some that it never sent: a screen without
                // history drops each row that scrolls off.
                s->clearHistory();
                st.next = oldest;
                st.from = oldest;
            }
            // The writer sends no row older than the oldest that exists.
            st.from = std::max(st.from, oldest);
            // The rows that the writer dropped go here too, and the replica
            // keeps no more than its own limit.
            while (s->historySize() > 0 && (st.next - s->historySize() < oldest || s->historySize() > s->m_maxScrollback)) {
                s->releaseRow(s->m_rows.front());
                s->m_rows.pop_front();
            }
            // The stable ids of the replica are the writer's.
            s->m_rowsDropped = st.next - s->historySize();
            s->m_viewportOffset = viewport ? std::min(viewport + added, s->historySize()) : 0;
            break;
        }
        case PrependTag: {
            const std::uint8_t which = p.byte();
            Screen* s = screenOf(which);
            if (!s) return fail("the state frame names no screen");
            ScreenState& st = m_screens[which];
            const std::uint64_t count = p.bounded(payload.size());
            // Each row is one older than the oldest that the writer sent
            // before, in this frame or an earlier one. A row past the
            // replica's own limit is read and dropped.
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                const std::uint64_t id = p.varint();
                Row row;
                if (!p.ok() || id + 1 != st.from || !decodeRow(p, *s, st, row, true)) {
                    discard(*s, row);
                    return fail("the state frame has a bad older history row");
                }
                st.from = id;
                if (s->historySize() >= s->m_maxScrollback) {
                    discard(*s, row);
                    continue;
                }
                s->m_rows.push_front(std::move(row));
                s->m_rowsDropped = id;
            }
            if (!p.ok() || !p.done()) return fail("the state frame has bad older history");
            break;
        }
        case CursorTag:
            cursors.assign(payload);
            cursorsSeen = true;
            break;
        case ScrollsTag: {
            const std::uint8_t lost = p.byte();
            const std::uint64_t count = p.bounded(RegionScrolls::Max);
            std::vector<std::pair<bool, RegionScroll>> scrolls;
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                const std::uint8_t alt = p.byte();
                RegionScroll s;
                s.top = p.varint();
                s.bottom = p.varint();
                s.left = int(p.bounded(std::uint64_t(Screen::MaxColumns - 1)));
                s.right = int(p.bounded(std::uint64_t(Screen::MaxColumns - 1)));
                const std::uint8_t full = p.byte();
                s.delta = int(p.signedBounded(INT_MIN, INT_MAX));
                if (!p.ok() || alt > 1 || full > 1 || s.top > s.bottom || s.left > s.right || s.delta == 0) {
                    return fail("the state frame has a bad region scroll");
                }
                s.fullWidth = full;
                scrolls.emplace_back(alt == 1, s);
            }
            if (!p.ok() || !p.done() || lost > 1) return fail("the state frame has bad region scrolls");
            // The replica's host hears of each scroll as the writer's did,
            // with the screen that scrolled active.
            const bool altScreen = t.m_modes.altScreen;
            for (const auto& [alt, scroll] : scrolls) {
                t.m_modes.altScreen = alt;
                t.m_host.regionScrolled(scroll);
            }
            if (lost) {
                // Too many to carry: every row of each screen left its region.
                for (const bool alt : {false, true}) {
                    const Screen& s = alt ? *t.m_alternate : *t.m_primary;
                    const std::size_t rows = std::min<std::size_t>(s.totalRows(), INT_MAX);
                    if (rows == 0) continue;
                    RegionScroll all;
                    all.top = s.rowsDropped();
                    all.bottom = all.top + rows - 1;
                    all.right = s.cols() - 1;
                    all.delta = -int(rows);
                    t.m_modes.altScreen = alt;
                    t.m_host.regionScrolled(all);
                }
            }
            t.m_modes.altScreen = altScreen;
            break;
        }
        case ShellTag: {
            const std::uint8_t zone = p.byte();
            const std::uint8_t clicks = p.byte();
            const std::uint8_t running = p.byte();
            Terminal::Mark marks[3];
            for (Terminal::Mark& m : marks) {
                const std::uint8_t valid = p.byte();
                if (valid > 1) return fail("the state frame has a bad shell mark");
                if (!valid) continue;
                m.row = p.varint();
                // A mark after a pending wrap is one past the last column.
                m.col = int(p.bounded(std::uint64_t(Screen::MaxColumns)));
                m.valid = true;
            }
            const std::uint64_t nextId = p.varint();
            const std::uint64_t count = p.bounded(Terminal::MaxSemanticRegions);
            std::vector<SemanticRegion> regions;
            std::uint64_t lastRow = 0;
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                SemanticRegion r;
                r.id = p.varint();
                const std::uint8_t flags = p.byte();
                r.start.row = p.varint();
                r.start.col = int(p.bounded(std::uint64_t(Screen::MaxColumns)));
                r.end.row = p.varint();
                r.end.col = int(p.bounded(std::uint64_t(Screen::MaxColumns)));
                const std::uint8_t kind = flags & 3;
                if (!p.ok() || kind > std::uint8_t(SemanticZoneKind::Output) || (flags & ~(3 | RegionOpen | RegionExitCode))
                    || r.id >= nextId) {
                    return fail("the state frame has a bad semantic region");
                }
                r.kind = SemanticZoneKind(kind);
                r.open = flags & RegionOpen;
                if (flags & RegionExitCode) r.exitCode = int(p.signedBounded(INT_MIN, INT_MAX));
                lastRow = std::max({lastRow, r.start.row, r.end.row});
                regions.push_back(r);
            }
            if (!p.ok() || !p.done() || zone > std::uint8_t(Terminal::SemanticZone::Input) || clicks > 1 || running > 1) {
                return fail("the state frame has bad shell marks");
            }
            t.m_semanticZone = Terminal::SemanticZone(zone);
            t.m_promptClickEvents = clicks;
            t.m_commandRunning = running;
            t.m_inputStart = marks[0];
            t.m_outputStart = marks[1];
            t.m_outputEnd = marks[2];
            t.m_nextSemanticId = nextId;
            t.m_semanticRegions = std::move(regions);
            t.m_semanticRegionsLastRow = lastRow;
            break;
        }
        case StatusTag: {
            const std::uint64_t count = p.bounded(ProgramStatus::MaxRecords);
            std::vector<StatusRecord> records;
            std::uint64_t serial = 0;
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                StatusRecord r;
                r.id = p.string(MaxFieldText);
                const std::uint8_t state = p.byte();
                const std::uint8_t kind = p.byte();
                r.progress = int(p.signedBounded(-1, 100));
                r.app = p.string(MaxFieldText);
                r.title = p.string(MaxFieldText);
                r.msg = p.string(MaxFieldText);
                r.serial = p.varint();
                if (!p.ok() || state > std::uint8_t(StatusState::Error) || kind > std::uint8_t(BlockKind::Auth)) {
                    return fail("the state frame has a bad status record");
                }
                r.state = StatusState(state);
                r.kind = BlockKind(kind);
                serial = std::max(serial, r.serial);
                records.push_back(std::move(r));
            }
            if (!p.ok() || !p.done()) return fail("the state frame has bad status records");
            ProgramStatus& status = t.m_programStatus;
            const bool changed = !std::equal(records.begin(), records.end(), status.m_records.begin(), status.m_records.end(),
                                             [](const StatusRecord& a, const StatusRecord& b) { return a.serial == b.serial && a.id == b.id; });
            status.m_records = std::move(records);
            status.m_serial = std::max(status.m_serial, serial);
            if (changed) t.m_host.programStatusChanged();
            break;
        }
        case ContextsTag: {
            const std::uint64_t count = p.bounded(ShellContexts::MaxDepth + 1);
            std::vector<ShellContext> stack;
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                ShellContext c;
                c.id = p.string(MaxFieldText);
                const std::uint8_t remote = p.byte();
                for (std::string* field : {&c.type, &c.user, &c.hostname, &c.targetUser, &c.targetHost, &c.container, &c.vm}) {
                    *field = p.string(MaxFieldText);
                }
                if (!p.ok() || remote > 1) return fail("the state frame has a bad shell context");
                c.iterm2RemoteHost = remote;
                stack.push_back(std::move(c));
            }
            if (!p.ok() || !p.done()) return fail("the state frame has bad shell contexts");
            if (stack != t.m_contexts.m_stack) {
                t.m_contexts.m_stack = std::move(stack);
                t.m_host.contextChanged();
            }
            break;
        }
        case ExtraCursorsTag: {
            ExtraCursorColor colors[2];
            for (ExtraCursorColor& c : colors) {
                const std::uint8_t mode = p.byte();
                const auto color = colorOf(p.u32());
                if (!p.ok() || !color
                    || (mode != std::uint8_t(ExtraCursorColor::Mode::Main) && mode != std::uint8_t(ExtraCursorColor::Mode::Reverse)
                        && mode != std::uint8_t(ExtraCursorColor::Mode::Rgb) && mode != std::uint8_t(ExtraCursorColor::Mode::Palette))) {
                    return fail("the state frame has a bad extra cursor color");
                }
                c.mode = ExtraCursorColor::Mode(mode);
                c.color = *color;
            }
            const std::uint64_t count = p.bounded(Terminal::MaxExtraCursors);
            std::map<std::pair<int, int>, int> cursors;
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                // The renderer skips a cursor past the screen, as a size
                // that changed since can leave one.
                const int y = int(p.bounded(std::uint64_t(Screen::MaxCells)));
                const int x = int(p.bounded(std::uint64_t(Screen::MaxColumns - 1)));
                const int shape = int(p.bounded(29));
                if (!p.ok() || (shape != 1 && shape != 2 && shape != 3 && shape != 29)) {
                    return fail("the state frame has a bad extra cursor");
                }
                cursors[{y, x}] = shape;
            }
            if (!p.ok() || !p.done()) return fail("the state frame has bad extra cursors");
            t.m_extraCursorColor = colors[0];
            t.m_extraCursorTextColor = colors[1];
            t.m_extraCursors = std::move(cursors);
            break;
        }
        default:
            return fail("the state frame has a section of unknown type " + std::to_string(tag));
        }
    }
    if (cursorsSeen) {
        In p(cursors);
        for (Screen* s : {t.m_primary.get(), t.m_alternate.get()}) {
            const std::uint64_t x = p.bounded(std::uint64_t(s->cols() - 1));
            const std::uint64_t y = p.bounded(std::uint64_t(s->rows() - 1));
            const std::uint8_t pendingWrap = p.byte();
            if (!p.ok() || pendingWrap > 1) return fail("the state frame has a bad cursor");
            s->cursor.x = int(x);
            s->cursor.y = int(y);
            s->cursor.pendingWrap = pendingWrap;
        }
        if (!p.done()) return fail("the state frame has a bad cursor");
    }
    t.touch();
    return true;
}

} // namespace AfterglowVt
