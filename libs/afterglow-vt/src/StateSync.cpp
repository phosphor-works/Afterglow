// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/StateSync.h>

#include <AfterglowVt/Terminal.h>

#include "Utf8.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <map>
#include <optional>
#include <unordered_set>
#include <vector>

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
    FrameTag = 16, ///< a part of the pixels of an image frame
    ImagesTag = 17, ///< the images of a screen that went and that changed
    PlacementsTag = 18, ///< the placements of a screen
    DropFramesTag = 19, ///< frames that no image holds any more
    CellSizeTag = 20, ///< the size of a cell in pixels, which places the images
    ImagesResetTag = 21, ///< the images start over: the first frame of a writer
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
            // The tenth byte holds the 64th bit only: more would be cut.
            if (shift == 63 && b > 1) {
                fail();
                return 0;
            }
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
    std::string_view rest() { return bytes(m_s.size() - m_at); }
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

/// Puts section @p tag with @p payload in @p out when the payload is not what
/// the replica has (@p sent), or always with @p first, and keeps it as sent.
void sendIfChanged(std::string& out, Tag tag, std::string& payload, std::string& sent, bool first)
{
    if (!first && payload == sent) return;
    section(out, tag, payload);
    sent = std::move(payload);
}

/// FNV-1a of @p bytes, going on from @p h for more than one piece.
std::uint64_t fnv(std::string_view bytes, std::uint64_t h = 1469598103934665603ull)
{
    for (const char c : bytes) {
        h ^= std::uint8_t(c);
        h *= 1099511628211ull;
    }
    return h;
}

// The longest strings that a reader takes: the title and the directory as
// the terminal bounds them, and an OSC 8 link. The fields of a status record
// and of a context have the bounds of ProgramStatus and ShellContexts.
constexpr std::size_t MaxText = 64 * 1024;
constexpr std::size_t MaxLinkText = StyleTable::MaxLinkBytes;

// The codec carries every field of these by name, in the writer and in the
// reader: a new field must be added to both, as for Modes below. A field
// that is not a flag changes the size (a flag can fit in padding); the sizes
// are those of a 64-bit build with libstdc++, the build that runs the tests.
// The startTrimmed and available of a SemanticRegion are not state: the
// terminal computes them when it gives the regions (semanticRegions). The
// frameShownAtMs of an Image is the replica's clock, not the writer's.
#if defined(__GLIBCXX__) && __SIZEOF_POINTER__ == 8
static_assert(sizeof(SemanticRegion) == 64, "a field of SemanticRegion that the codec does not carry");
static_assert(sizeof(StatusRecord) == 144, "a field of StatusRecord that the codec does not carry");
static_assert(sizeof(ShellContext) == 264, "a field of ShellContext that the codec does not carry");
static_assert(sizeof(RegionScroll) == 32, "a field of RegionScroll that the codec does not carry");
static_assert(sizeof(Image) == 128, "a field of Image that the codec does not carry");
static_assert(sizeof(Placement) == 88, "a field of Placement that the codec does not carry");
static_assert(sizeof(ImageFrame) == 64, "a field of ImageFrame that the codec does not carry");
#endif

/// The bytes of @p v as a varint.
std::size_t varintSize(std::uint64_t v)
{
    std::size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

using StyleDefinitions = std::unordered_map<std::uint32_t, Style>;
using LinkDefinitions = std::unordered_map<std::uint32_t, std::pair<std::string, std::string>>;

/// Encodes @p row and adds the style ids that its cells use to @p styles,
/// sorted and each once.
void encodeRow(std::string& out, const Row& row, std::vector<std::uint32_t>& styles)
{
    Out o(out);
    o.byte(std::uint8_t((row.wrapped ? RowWrapped : 0) | (row.promptStart ? RowPromptStart : 0)
                        | (std::uint8_t(row.lineSize) << RowLineSizeShift)));
    o.varint(row.cells.size());
    for (const Cell& c : row.cells) {
        // The raw fields: the reader checks each part and maps the style id.
        o.varint(c.rawCodepoint());
        o.varint(c.rawPacked());
        if (c.styleId() != StyleTable::DefaultId) styles.push_back(c.styleId());
    }
    std::sort(styles.begin(), styles.end());
    styles.erase(std::unique(styles.begin(), styles.end()), styles.end());
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

void encodeStyle(std::string& out, const Style& s)
{
    Out o(out);
    o.u32(s.fg.bits());
    o.u32(s.bg.bits());
    o.u32(s.underlineColor.bits());
    o.varint(s.attrs);
    o.byte(std::uint8_t(s.underline));
    o.varint(s.link);
}

/// The bytes of encodeStyle(@p s).
std::size_t styleSize(const Style& s)
{
    return 3 * 4 + varintSize(s.attrs) + 1 + varintSize(s.link);
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

    /// The bytes that define() adds for @p ids: the definitions that the
    /// replica lacks, or has with another meaning, and that do not wait in
    /// this frame already.
    std::size_t cost(const std::vector<std::uint32_t>& ids) const
    {
        std::size_t bytes = 0;
        // Each link once, however many styles name it.
        m_unknownLinks.clear();
        for (const std::uint32_t id : ids) {
            const Style& style = m_table.get(id);
            if (style.link && !linkKnown(style.link)) m_unknownLinks.push_back(style.link);
            bytes += styleCost(id, style);
        }
        std::sort(m_unknownLinks.begin(), m_unknownLinks.end());
        m_unknownLinks.erase(std::unique(m_unknownLinks.begin(), m_unknownLinks.end()), m_unknownLinks.end());
        for (const std::uint32_t link : m_unknownLinks) bytes += linkBytes(link);
        return bytes;
    }

    /// cost() of style @p id alone, with its link.
    std::size_t cost(std::uint32_t id) const
    {
        const Style& style = m_table.get(id);
        return styleCost(id, style) + (style.link && !linkKnown(style.link) ? linkBytes(style.link) : 0);
    }

    /// Adds style @p id and its link, when the replica lacks them or has
    /// them with another meaning.
    void define(std::uint32_t id)
    {
        const Style& style = m_table.get(id);
        if (style.link && !linkKnown(style.link)) {
            const std::string_view uri = m_table.linkUri(style.link);
            const std::string_view linkId = m_table.linkId(style.link);
            Out o(m_links);
            o.varint(style.link);
            o.string(uri);
            o.string(linkId);
            ++m_linkCount;
            m_knownLinks[style.link] = {std::string(uri), std::string(linkId)};
        }
        if (!styleKnown(id, style)) {
            Out(m_styles).varint(id);
            encodeStyle(m_styles, style);
            ++m_styleCount;
            m_known[id] = style;
        }
    }

    /// Adds the definitions of @p ids while the bytes that wait stay within
    /// @p room, and at least one: for a row whose definitions are too many
    /// for one frame (a row of links), which waits for the next frame.
    void definePart(const std::vector<std::uint32_t>& ids, std::size_t room)
    {
        bool added = false;
        for (const std::uint32_t id : ids) {
            const std::size_t more = cost(id);
            if (more == 0) continue;
            if (added && bytes() + more > room) return;
            define(id);
            added = true;
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
    bool linkKnown(std::uint32_t link) const
    {
        const auto known = m_knownLinks.find(link);
        return known != m_knownLinks.end() && known->second.first == m_table.linkUri(link)
            && known->second.second == m_table.linkId(link);
    }
    bool styleKnown(std::uint32_t id, const Style& style) const
    {
        const auto known = m_known.find(id);
        return known != m_known.end() && known->second == style;
    }
    std::size_t styleCost(std::uint32_t id, const Style& style) const
    {
        return styleKnown(id, style) ? 0 : varintSize(id) + styleSize(style);
    }
    std::size_t linkBytes(std::uint32_t link) const
    {
        const std::size_t uri = m_table.linkUri(link).size();
        const std::size_t id = m_table.linkId(link).size();
        return varintSize(link) + varintSize(uri) + uri + varintSize(id) + id;
    }

    const StyleTable& m_table;
    StyleDefinitions& m_known;
    LinkDefinitions& m_knownLinks;
    std::string m_styles;
    std::string m_links;
    std::size_t m_styleCount = 0;
    std::size_t m_linkCount = 0;
    mutable std::vector<std::uint32_t> m_unknownLinks; ///< cost()'s scratch: the unknown links of a row
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
    if (scroll.delta == 0) return;
    // Each scroll counts for a loss too, the ones before it included.
    Span& span = m_spans[altScreen ? 1 : 0];
    span.top = span.any ? std::min(span.top, scroll.top) : scroll.top;
    span.bottom = span.any ? std::max(span.bottom, scroll.bottom) : scroll.bottom;
    span.any = true;
    if (m_lost) return;
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
    m_spans[0] = m_spans[1] = Span();
    m_lost = false;
}

void StateWriter::reset()
{
    const std::size_t replicaHistory = m_replicaHistory;
    *this = StateWriter();
    m_replicaHistory = replicaHistory;
}

bool StateWriter::pending() const
{
    return m_screens[0].historyPending || m_screens[1].historyPending || m_screens[0].rowsPending || m_screens[1].rowsPending
        || m_imagesPending;
}

namespace {
bool sendable(const Image& image);
} // namespace

std::string StateWriter::update(const Terminal& t, std::size_t maxBytes, const RegionScrolls* scrolls)
{
    std::string out(1, char(Version));
    const bool first = !m_started;
    m_started = true;
    // Every change of the terminal bumps its generation: without one, only
    // rows that the last frames left out can go.
    const bool changed = first || t.generation() != m_generation;
    m_generation = t.generation();

    std::string payload;
    // The scrolls go first: the reader keeps them for its host until the
    // frame applied.
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
        if (scrolls->m_lost) {
            for (const RegionScrolls::Span& span : scrolls->m_spans) {
                o.byte(span.any ? 1 : 0);
                if (!span.any) continue;
                // A scroll counts its rows in an int. Rows that far above the
                // lowest region are history, which no scroll moves.
                o.varint(std::max(span.top, span.bottom - std::min<std::uint64_t>(span.bottom, INT_MAX - 1)));
                o.varint(span.bottom);
            }
        }
        section(out, ScrollsTag, payload);
    }

    if (changed) {
        payload = encodeModes(t.m_modes);
        // The updates that programs began (mode 2026): a renderer gives each
        // its own timeout, also one that begins right after another ended.
        Out(payload).varint(t.m_synchronizedOutputSerial);
        sendIfChanged(out, ModesTag, payload, m_modes, first);

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
        sendIfChanged(out, ColorsTag, payload, m_colors, first);

        payload.clear();
        Out(payload).string(std::string_view(t.m_title).substr(0, MaxText));
        Out(payload).string(std::string_view(t.m_cwd).substr(0, MaxText));
        sendIfChanged(out, TextTag, payload, m_text, first);

        // OSC 133, for the last output, a click on the prompt and the regions
        // that `afterglow ctl` lists, and a resize that waits for the shell to
        // draw its prompt again. The rows are stable ids of the primary
        // screen, which the replica numbers as the writer does.
        payload.clear();
        {
            Out o(payload);
            o.byte(std::uint8_t(t.m_semanticZone));
            o.byte(t.m_promptClickEvents ? 1 : 0);
            o.byte(t.m_commandRunning ? 1 : 0);
            o.byte(t.m_promptRedrawPending ? 1 : 0);
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
        sendIfChanged(out, ShellTag, payload, m_shell, first);

        // Up to 256 records of 2.4 KB: encoded only when one changed, which
        // every change of the records shows in their serial.
        if (first || t.m_programStatus.serial() != m_statusSerial) {
            m_statusSerial = t.m_programStatus.serial();
            payload.clear();
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
            sendIfChanged(out, StatusTag, payload, m_status, first);
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
        sendIfChanged(out, ContextsTag, payload, m_contexts, first);

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
        sendIfChanged(out, ExtraCursorsTag, payload, m_extraCursors, first);
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
    Budget budget{maxBytes};
    bool reset = writeRows(out, t, 0, m_screens[0], budget, changed);
    reset = writeRows(out, t, 1, m_screens[1], budget, changed) || reset;
    // The cell size that the program's images were laid out at: the window
    // measured it, and the replica draws them at it. Before the images.
    {
        std::string cells;
        Out o(cells);
        o.varint(std::uint64_t(std::max(t.m_cellWidth, 0)));
        o.varint(std::uint64_t(std::max(t.m_cellHeight, 0)));
        if (first || cells != m_cellSize) {
            section(out, CellSizeTag, cells);
            m_cellSize = std::move(cells);
        }
    }
    // The images that show go before the history, the others after it. The
    // records of the versions whose pixels went go first: pixels that come
    // after them take the room of each frame.
    const bool images = prepareImages(out, t, first);
    if (images) {
        writeDrops(out, budget);
        writeRecords(out, t, budget);
        writePixels(out, t, budget, true);
        writeRecords(out, t, budget);
    }
    writeHistory(out, t, 0, m_screens[0], budget);
    writeHistory(out, t, 1, m_screens[1], budget);
    if (images) {
        writePixels(out, t, budget, false);
        writeRecords(out, t, budget);
        writeDrops(out, budget);
        // What waits for a later frame.
        bool incomplete = false;
        for (const GraphicsStore& store : t.m_graphics) {
            for (const auto& [id, image] : store.images()) incomplete = incomplete || (sendable(image) && !complete(image));
        }
        m_imagesPending = m_imagesPending || incomplete || m_transfer || !m_done.empty() || !m_drops.empty();
    }
    // The cursors go after the rows, which the reader checks them against.
    // A screen that started over lost its cursor on the replica.
    if (first || reset || payload != m_cursor) {
        section(out, CursorTag, payload);
        m_cursor = std::move(payload);
    }
    return out.size() == 1 ? std::string() : out;
}

bool StateWriter::writeRows(std::string& out, const Terminal& t, int which, ScreenState& st, Budget& budget, bool changed)
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
        o.varint(s.layoutEpoch());
        section(out, ResetTag, payload);
        st.totalSent = total;
        reset = true;
    }
    // Nothing changed since the frame that sent every row that changed.
    if (!changed && !reset && !st.rowsPending) return reset;

    // The rows that changed, with the styles and links that they use, while
    // the frame stays within its budget (Budget). Rows that wait keep their
    // old hash and go in the next frames, as history does: a screen of rows
    // that are long to encode (many combining marks, links) never makes a
    // frame too large for its channel.
    std::string rows;
    std::size_t rowCount = 0;
    Definitions definitions(s.styles(), st.styles, st.links);
    st.rowsPending = false;
    // From the first row that the last frame left out: rows at the top that
    // change in every frame do not keep the others waiting.
    const int start = st.rowStart < s.rows() ? st.rowStart : 0;
    st.rowStart = 0;
    std::string row;
    std::string style;
    std::vector<std::uint32_t> rowStyles;
    const auto wait = [&](int y) {
        if (!st.rowsPending) st.rowStart = y;
        st.rowsPending = true;
    };
    for (int i = 0; i < s.rows(); ++i) {
        const int y = (start + i) % s.rows();
        row.clear();
        rowStyles.clear();
        encodeRow(row, s.row(y), rowStyles);
        // What the row means, not only its bytes: between two frames a
        // style id can be freed and taken again by another style, and a
        // row rewritten with the same characters and that id.
        std::uint64_t h = fnv(row);
        for (const std::uint32_t id : rowStyles) {
            const Style& definition = s.styles().get(id);
            style.clear();
            encodeStyle(style, definition);
            h = fnv(style, h);
            if (definition.link) {
                // Each string with its length, so that no two links hash as one.
                style.clear();
                Out(style).string(s.styles().linkUri(definition.link));
                Out(style).string(s.styles().linkId(definition.link));
                h = fnv(style, h);
            }
        }
        h |= 1; // never 0, the mark of a missing row
        if (st.rowHashes[std::size_t(y)] == h) continue;
        if (budget.full) {
            wait(y);
            continue;
        }
        const std::size_t used = out.size() + definitions.bytes() + rows.size();
        // A row that does not fit by its own bytes waits without a look at
        // its definitions, which can be many.
        if (budget.anyRow && used + row.size() + 10 > budget.max) {
            wait(y);
            continue;
        }
        const std::size_t cost = definitions.cost(rowStyles);
        if (used + cost + row.size() + 10 > budget.max) {
            if (budget.anyRow) {
                wait(y);
                continue;
            }
            if (cost > 0 && used + cost > budget.max) {
                // The first row of the frame, with more definitions than
                // the frame has room for: they go ahead of it.
                definitions.definePart(rowStyles, budget.max > used ? budget.max - used : 0);
                budget.full = true;
                wait(y);
                continue;
            }
            // The first row of the frame goes past the budget by its own
            // bytes only.
        }
        for (const std::uint32_t id : rowStyles) definitions.define(id);
        st.rowHashes[std::size_t(y)] = h;
        Out(rows).varint(std::uint64_t(y));
        rows += row;
        ++rowCount;
        budget.anyRow = true;
    }

    // The definitions that the replica lacks, or has with another meaning:
    // a row that uses a changed one has a new hash, so it goes again too.
    definitions.emit(out, which);
    if (rowCount) {
        std::string payload(1, char(which));
        Out(payload).varint(rowCount);
        section(out, RowsTag, payload + rows);
    }
    return reset;
}

void StateWriter::writeHistory(std::string& out, const Terminal& t, int which, ScreenState& st, Budget& budget)
{
    const Screen& s = which == 0 ? *t.m_primary : *t.m_alternate;
    const std::uint64_t oldest = s.rowsDropped();
    const std::uint64_t total = oldest + s.historySize();
    // No row older than the replica keeps.
    const std::uint64_t floor = total - std::min<std::uint64_t>(total - oldest, m_replicaHistory);

    // The rows that scrolled in since the last frame, oldest first, then
    // older rows that the replica never had, newest first.
    const bool gap = st.to < floor;
    if (gap) {
        // Rows scrolled in and out again before they could be sent, or past
        // what the replica keeps: its history ends there, and holds nothing
        // older.
        st.from = st.to = floor;
    }
    if (st.from < floor) st.from = floor;
    Definitions definitions(s.styles(), st.styles, st.links);
    std::string appends;
    std::size_t appendCount = 0;
    std::string prepends;
    std::size_t prependCount = 0;
    std::string rowBytes;
    std::vector<std::uint32_t> rowStyles;
    // A row goes when the frame, with the definitions that it needs, stays
    // within the budget. The first row of the frame goes past it by its own
    // bytes at most, so the history advances even past a row larger than the
    // budget. Definitions too many for one frame go ahead of their row.
    const auto take = [&](const Row& row, std::string& to, std::uint64_t id) {
        if (budget.full) return false;
        rowBytes.clear();
        rowStyles.clear();
        encodeRow(rowBytes, row, rowStyles);
        const std::size_t used = out.size() + definitions.bytes() + appends.size() + prepends.size() + 64;
        if (budget.anyRow && used + rowBytes.size() + 10 > budget.max) return false;
        const std::size_t cost = definitions.cost(rowStyles);
        if (used + cost + rowBytes.size() + 10 > budget.max) {
            if (budget.anyRow) return false;
            if (cost > 0 && used + cost > budget.max) {
                definitions.definePart(rowStyles, budget.max > used ? budget.max - used : 0);
                budget.full = true;
                return false;
            }
        }
        for (const std::uint32_t styleId : rowStyles) definitions.define(styleId);
        Out(to).varint(id);
        to += rowBytes;
        budget.anyRow = true;
        return true;
    };
    while (st.to < total && take(s.absoluteRow(std::size_t(st.to - oldest)), appends, st.to)) {
        ++appendCount;
        ++st.to;
    }
    while (st.to == total && st.from > floor && take(s.absoluteRow(std::size_t(st.from - 1 - oldest)), prepends, st.from - 1)) {
        ++prependCount;
        --st.from;
    }
    // History rows take their definitions when the replica installs them,
    // so a later change of an id leaves them as they were.
    definitions.emit(out, which);
    // The first visible row's id goes along: the replica numbers its rows by
    // it, with blank rows for those that are still on their way.
    // The oldest row that the replica can still get: none below the floor
    // comes, so the replica keeps no blank place for one.
    if (appendCount || gap || floor != st.oldestSent || total != st.totalSent) {
        std::string payload(1, char(which));
        Out o(payload);
        o.varint(floor);
        o.varint(total);
        o.varint(appendCount);
        payload += appends;
        section(out, AppendTag, payload);
        st.oldestSent = floor;
        st.totalSent = total;
    }
    if (prependCount) {
        std::string payload(1, char(which));
        Out(payload).varint(prependCount);
        payload += prepends;
        section(out, PrependTag, payload);
    }
    st.historyPending = st.to < total || st.from > floor;
}

namespace {

/// The bytes of @p frame: RGBA, or the indices of an indexed frame.
std::string_view pixelsOf(const ImageFrame& frame)
{
    const std::vector<std::uint8_t>& bytes = frame.pixels();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// The smallest part of a frame's pixels worth a section: less room than
/// this waits for the next frame, unless the frame has nothing else.
constexpr std::size_t MinPixelPart = 4096;

/// The room that a frame leaves for the tag and the length of a section.
constexpr std::size_t SectionSlack = 64;

constexpr std::uint8_t ImageSixel = 1;
constexpr std::uint8_t ImageAnonymous = 2;
constexpr std::uint8_t ImageTransient = 4;

void encodePlacement(Out& o, const Placement& p)
{
    o.varint(p.imageId);
    o.varint(p.placementId);
    o.varint(p.row);
    for (const int v : {p.col, p.offsetX, p.offsetY, p.srcX, p.srcY, p.srcW, p.srcH, p.cols, p.rows}) o.signedVarint(v);
    o.signedVarint(p.z);
    o.varint(p.parentImage);
    o.varint(p.parentPlacement);
    o.signedVarint(p.relativeX);
    o.signedVarint(p.relativeY);
    o.byte(p.isVirtual ? 1 : 0);
    o.varint(p.displayWidth);
    o.varint(p.displayHeight);
}

} // namespace

bool StateWriter::complete(const Image& image) const
{
    return std::all_of(image.frames.begin(), image.frames.end(), [this](const FramePtr& frame) {
        const auto it = m_frames.find(frame.get());
        // A frame of no pixels never goes: no writer makes one.
        return it != m_frames.end() && it->second.sent > 0 && it->second.sent == pixelsOf(*frame).size();
    });
}

bool StateWriter::fits(const std::string& out, const Budget& budget, std::size_t bytes, bool first) const
{
    return out.size() + bytes + SectionSlack <= budget.max || (first && !budget.anyRow && !m_imagesWent);
}

namespace {

/// Whether the pixels of @p image can go: a frame of no pixels never does.
/// No writer makes one.
bool sendable(const Image& image)
{
    return std::none_of(image.frames.begin(), image.frames.end(), [](const FramePtr& frame) { return pixelsOf(*frame).empty(); });
}

} // namespace

bool StateWriter::prepareImages(std::string& out, const Terminal& t, bool first)
{
    // A writer that was reset starts the replica's images over, also those
    // of a reader that applied the frames of the writer before.
    if (first) section(out, ImagesResetTag, {});
    const bool changed = first || t.m_graphics[0].changes() != m_storeChanges[0] || t.m_graphics[1].changes() != m_storeChanges[1];
    m_imagesWent = false;
    m_storesChanged = false;
    if (!changed && !m_imagesPending) return false;
    m_storeChanges[0] = t.m_graphics[0].changes();
    m_storeChanges[1] = t.m_graphics[1].changes();
    m_storesChanged = changed;
    m_imagesPending = false;
    // A version whose image went, or whose id names another image now (a
    // clear, then a new image), is of no use: a transfer of it ends.
    const auto gone = [&t](const ImageVersion& v) {
        const auto& images = t.m_graphics[v.which].images();
        const auto it = images.find(v.image.id);
        return it == images.end() || it->second.order != v.image.order;
    };
    if (m_transfer && gone(*m_transfer)) endTransfer();
    std::erase_if(m_done, gone);
    // The frames that went: no image and no version holds them. The
    // replica drops those that it got pixels of. Only the transfer's frames
    // are ever in part on the replica, and the transfer holds them.
    for (auto it = m_frames.begin(); it != m_frames.end();) {
        if (!it->second.frame.expired()) {
            ++it;
            continue;
        }
        if (it->second.sent > 0) m_drops.push_back(it->second.id);
        it = m_frames.erase(it);
    }
    // The frames that are new get an id.
    for (const GraphicsStore& store : t.m_graphics) {
        for (const auto& [id, image] : store.images()) {
            for (const FramePtr& frame : image.frames) {
                auto [it, added] = m_frames.try_emplace(frame.get());
                if (added) it->second = SentFrame{frame, m_nextFrameId++, 0};
            }
        }
    }
    const int active = t.m_modes.altScreen ? 1 : 0;
    m_shown = t.m_graphics[active].shownImages(*t.m_active, t.m_cellWidth, t.m_cellHeight);
    return true;
}

void StateWriter::endTransfer()
{
    // The frame that the replica builds goes, also when another image holds
    // it still (a sixel that a kitty id moved): its entry goes, so that it
    // starts over under a new id. No frame starts before the replica
    // dropped it.
    for (const FramePtr& frame : m_transfer->image.frames) {
        const auto it = m_frames.find(frame.get());
        if (it == m_frames.end() || it->second.sent == 0 || it->second.sent == pixelsOf(*frame).size()) continue;
        m_drops.insert(m_drops.begin(), it->second.id);
        m_dropsBuilding = true;
        m_frames.erase(it);
    }
    m_transfer.reset();
}

void StateWriter::writeDrops(std::string& out, Budget& budget)
{
    if (m_drops.empty()) return;
    std::string ids;
    std::size_t count = 0;
    for (const std::uint64_t id : m_drops) {
        if (!fits(out, budget, ids.size() + varintSize(id) + 10, count == 0)) break;
        Out(ids).varint(id);
        ++count;
    }
    if (count == 0) {
        m_imagesPending = true;
        return;
    }
    std::string payload;
    Out(payload).varint(count);
    payload += ids;
    section(out, DropFramesTag, payload);
    m_imagesWent = true;
    m_drops.erase(m_drops.begin(), m_drops.begin() + std::ptrdiff_t(count));
    // The frame that the replica builds is the first of the drops.
    m_dropsBuilding = false;
    m_imagesPending = m_imagesPending || !m_drops.empty();
}

void StateWriter::writePixels(std::string& out, const Terminal& t, Budget& budget, bool shown)
{
    if (m_dropsBuilding) {
        m_imagesPending = true;
        return;
    }
    const int active = t.m_modes.altScreen ? 1 : 0;
    const auto shows = [&](int which, std::uint32_t id) { return which == active && m_shown.contains(id); };
    // The next part of @p f's pixels, if the budget leaves room for one.
    // True once all of them went.
    const auto sendPart = [&](SentFrame& f, const ImageFrame& frame) {
        const std::string_view pixels = pixelsOf(frame);
        if (f.sent == pixels.size()) return true;
        std::string header;
        Out o(header);
        o.varint(f.id);
        o.varint(f.sent);
        if (f.sent == 0) {
            o.varint(frame.width);
            o.varint(frame.height);
            o.byte(std::uint8_t(frame.format));
        }
        const std::size_t used = out.size() + header.size() + SectionSlack;
        std::size_t room = budget.max > used ? budget.max - used : 0;
        const std::size_t left = pixels.size() - f.sent;
        if (room < std::min(left, MinPixelPart)) {
            // A frame that has nothing else takes a part all the same, so
            // that the pixels move at any budget.
            if (budget.anyRow || m_imagesWent) return false;
            room = std::min(left, MinPixelPart);
        }
        const std::size_t part = std::min(room, left);
        // The pixels go straight into the frame.
        out.push_back(char(FrameTag));
        Out(out).varint(header.size() + part);
        out += header;
        out.append(pixels.substr(f.sent, part));
        f.sent += part;
        m_imagesWent = true;
        if (f.sent != pixels.size()) return false;
        // A frame that completes can complete an image other than the
        // transfer's: the store's own, while the version that goes keeps a
        // frame that the program deleted. Its record goes with these pixels.
        m_recordsWaiting = true;
        return true;
    };
    // The images whose pixels the replica lacks, once for the call: those
    // that show first, the active screen's first.
    std::vector<std::pair<int, const Image*>> next;
    bool listed = false;
    std::size_t at = 0;
    for (;;) {
        if (!m_transfer) {
            if (!listed) {
                listed = true;
                for (const int which : {active, 1 - active}) {
                    for (const auto& [id, image] : t.m_graphics[which].images()) {
                        if ((shown && !shows(which, id)) || !sendable(image) || complete(image)) continue;
                        next.emplace_back(which, &image);
                    }
                }
            }
            // One that an image before it completed (a shared frame) is done.
            while (at < next.size() && complete(*next[at].second)) ++at;
            if (at == next.size()) return;
            m_transfer = ImageVersion{next[at].first, *next[at].second};
            ++at;
        }
        if (shown && !shows(m_transfer->which, m_transfer->image.id)) return;
        // The store's own image of the id, when it is another version of
        // the same image (the program deleted a frame of it): the frames
        // that the replica lacks of it. Once they went, with this version's,
        // it shows with the next records, and this version must not replace
        // it later, also once a new frame leaves it incomplete again. No
        // more of this version's pixels go, and the replica drops its part
        // of a frame first.
        std::unordered_set<const ImageFrame*> liveMissing;
        bool watching = false;
        {
            const auto& images = t.m_graphics[m_transfer->which].images();
            const auto live = images.find(m_transfer->image.id);
            if (live != images.end() && live->second.order == m_transfer->image.order
                && live->second.frames != m_transfer->image.frames) {
                watching = true;
                for (const FramePtr& frame : live->second.frames) {
                    const auto it = m_frames.find(frame.get());
                    if (it == m_frames.end() || it->second.sent == 0 || it->second.sent != pixelsOf(*frame).size())
                        liveMissing.insert(frame.get());
                }
            }
        }
        const auto superseded = [&] {
            endTransfer();
            m_imagesPending = true;
        };
        if (watching && liveMissing.empty()) {
            superseded();
            return;
        }
        for (const FramePtr& frame : m_transfer->image.frames) {
            SentFrame& f = m_frames[frame.get()];
            if (f.id == 0) f = SentFrame{frame, m_nextFrameId++, 0};
            if (!sendPart(f, *frame)) {
                m_imagesPending = true;
                return;
            }
            if (watching && liveMissing.erase(frame.get()) && liveMissing.empty()) {
                superseded();
                return;
            }
        }
        // All of its pixels went: its record goes with the next records.
        m_done.push_back(std::move(*m_transfer));
        m_transfer.reset();
    }
}

void StateWriter::writeRecords(std::string& out, const Terminal& t, Budget& budget)
{
    // Nothing for a record to say while only pixels go: the stores did not
    // change, nothing waits, and no frame's pixels completed.
    if (!m_storesChanged && !m_recordsWaiting && m_done.empty()) return;
    m_recordsWaiting = false;
    for (int which = 0; which < 2; ++which) {
        const GraphicsStore& store = t.m_graphics[which];
        std::unordered_map<std::uint32_t, std::string>& sent = m_images[which];
        // An image shows once all of its frames came: the version in the
        // store, else the newest one whose pixels went. One that the replica
        // has keeps its record until then.
        const auto version = [&](std::uint32_t id, const Image& image) -> std::pair<const Image*, ImageVersion*> {
            if (complete(image)) return {&image, nullptr};
            for (auto it = m_done.rbegin(); it != m_done.rend(); ++it) {
                if (it->which == which && it->image.id == id) return {&it->image, &*it};
            }
            return {nullptr, nullptr};
        };
        std::vector<std::uint32_t> removed;
        for (const auto& [id, record] : sent) {
            if (!store.images().contains(id)) removed.push_back(id);
        }
        for (const std::uint32_t id : removed) sent.erase(id);
        std::string records;
        std::size_t recordCount = 0;
        for (const auto& [id, live] : store.images()) {
            const auto [image, done] = version(id, live);
            if (!image) continue;
            std::string record;
            Out o(record);
            o.varint(image->id);
            o.varint(image->number);
            o.byte(std::uint8_t((image->isSixel ? ImageSixel : 0) | (image->anonymous ? ImageAnonymous : 0)
                                | (image->transient ? ImageTransient : 0)));
            o.varint(image->width);
            o.varint(image->height);
            o.varint(image->frames.size());
            for (const FramePtr& frame : image->frames) o.varint(m_frames[frame.get()].id);
            o.varint(image->gaps.size());
            for (const std::uint32_t gap : image->gaps) o.varint(gap);
            o.byte(std::uint8_t(image->animation));
            o.varint(image->maxLoops);
            o.varint(image->loopsDone);
            o.varint(image->currentFrame);
            o.varint(image->stateCommands);
            o.varint(image->frameCommands);
            o.varint(image->sequence);
            o.varint(image->order);
            const auto it = sent.find(id);
            if (it != sent.end() && it->second == record) {
                if (done) done->recorded = true;
                continue;
            }
            if (!fits(out, budget, records.size() + record.size() + 16, recordCount == 0)) {
                m_imagesPending = true;
                m_recordsWaiting = true;
                break;
            }
            records += record;
            ++recordCount;
            sent[id] = std::move(record);
            if (done) done->recorded = true;
        }
        if (recordCount || !removed.empty()) {
            std::string payload(1, char(which));
            Out o(payload);
            o.varint(removed.size());
            for (const std::uint32_t id : removed) o.varint(id);
            o.varint(recordCount);
            payload += records;
            section(out, ImagesTag, payload);
            m_imagesWent = true;
        }
        // The placements of the images that the replica has.
        std::string placements;
        std::size_t count = 0;
        for (const Placement& p : store.placements()) {
            if (!sent.contains(p.imageId) || (p.parentImage && !sent.contains(p.parentImage))) continue;
            Out o(placements);
            encodePlacement(o, p);
            ++count;
        }
        std::string payload(1, char(which));
        Out(payload).varint(count);
        payload += placements;
        if (payload == m_placements[which]) continue;
        if (!fits(out, budget, payload.size(), true)) {
            m_imagesPending = true;
            m_recordsWaiting = true;
            continue;
        }
        section(out, PlacementsTag, payload);
        m_imagesWent = true;
        m_placements[which] = std::move(payload);
    }
    // A version is done once the replica has its record, once the store's
    // own went whole, and once a newer version of its image replaced it.
    std::unordered_map<std::uint64_t, std::size_t> newest;
    for (std::size_t i = 0; i < m_done.size(); ++i) newest[(std::uint64_t(m_done[i].which) << 32) | m_done[i].image.id] = i;
    std::vector<ImageVersion> keep;
    for (std::size_t i = 0; i < m_done.size(); ++i) {
        const ImageVersion& v = m_done[i];
        const bool newer = newest[(std::uint64_t(v.which) << 32) | v.image.id] != i;
        const auto& images = t.m_graphics[v.which].images();
        const auto live = images.find(v.image.id);
        if (v.recorded || newer || live == images.end() || complete(live->second)) continue;
        keep.push_back(std::move(m_done[i]));
    }
    const bool released = keep.size() < m_done.size();
    m_done = std::move(keep);
    // The frames that only those versions held went with them: their drops
    // go in the next frame, not after the next change of a store.
    if (released) {
        for (const auto& [frame, f] : m_frames) {
            if (f.sent > 0 && f.frame.expired()) {
                m_imagesPending = true;
                break;
            }
        }
    }
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
                    // is this link's: a freed index can be taken again, also by
                    // another link of the same text, which must stay another.
                    std::uint32_t& cached = st.linkCache[style.link];
                    const auto owner = cached ? st.linkOwner.find(cached) : st.linkOwner.end();
                    if (!cached || owner == st.linkOwner.end() || owner->second != style.link
                        || s.m_styles.linkUri(cached) != link->second.first || s.m_styles.linkId(cached) != link->second.second) {
                        cached = s.m_styles.internLink(link->second.first, link->second.second);
                        const auto [previous, added] = st.linkOwner.try_emplace(cached, style.link);
                        if (!added && previous->second != style.link) {
                            // The link that held this place last loses it,
                            // unless it took another since.
                            const auto stale = st.linkCache.find(previous->second);
                            if (stale != st.linkCache.end() && stale->second == cached) st.linkCache.erase(stale);
                            previous->second = style.link;
                        }
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
    // What the replica's host hears of once the whole frame applied.
    std::vector<std::pair<bool, RegionScroll>> scrolled;
    bool statusChanged = false;
    bool contextsChanged = false;
    bool touched = false;
    bool imagesTouched = false;
    m_screenChanged = false;
    while (!in.done()) {
        const std::uint8_t tag = in.byte();
        const std::string_view payload = in.bytes(std::size_t(in.bounded(frame.size())));
        if (!in.ok()) return fail("the state frame is cut short");
        In p(payload);
        // Pixels that come for an image that does not show yet change
        // nothing that the replica shows, and the images change no text.
        if (tag == ImagesTag || tag == PlacementsTag || tag == ImagesResetTag) {
            imagesTouched = true;
        } else if (tag != FrameTag && tag != DropFramesTag) {
            touched = true;
        }
        switch (tag) {
        case ModesTag: {
            Modes modes = t.m_modes;
            if (!decodeModes(p, modes)) return fail("the state frame has bad modes");
            const std::uint64_t serial = p.varint();
            if (!p.ok() || !p.done()) return fail("the state frame has bad modes");
            // The modes and the active screen change together.
            t.m_modes = modes;
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
            const std::uint64_t epoch = p.varint();
            if (!s || !p.ok() || !p.done() || cols < 2 || rows < 1 || cols * rows > Screen::MaxCells) {
                return fail("the state frame has a bad screen reset");
            }
            const int which = s == t.m_primary.get() ? 0 : 1;
            ScreenState& st = m_screens[which];
            const std::uint64_t layout = s->m_layoutEpoch;
            const std::uint64_t before = st.next;
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
            // A new layout of the writer numbers its rows anew, and so does
            // the replica: what the window keeps by row id (a selection, a
            // search) lets go. Ids that go back to an earlier number name
            // other rows too. The same layout in a new size (a height
            // change) keeps them, as a resize of the window's own does.
            Epoch& known = m_epochs[which];
            if ((!known.known || known.epoch != epoch || next < before) && s->m_layoutEpoch == layout) ++s->m_layoutEpoch;
            known = Epoch{true, epoch};
            st = ScreenState();
            st.next = st.hole = st.from = next;
            s->m_rowsDropped = next;
            s->m_viewportOffset = 0;
            m_screenChanged = true;
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
                    // A new meaning of the writer's id is another link.
                    st.linkCache.erase(id);
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
            m_screenChanged = m_screenChanged || count > 0;
            break;
        }
        case AppendTag: {
            const std::uint8_t which = p.byte();
            Screen* s = screenOf(which);
            if (!s) return fail("the state frame names no screen");
            ScreenState& st = m_screens[which];
            const std::uint64_t oldest = p.varint();
            const std::uint64_t total = p.varint();
            const std::uint64_t count = p.bounded(payload.size());
            if (!p.ok() || total < st.next || oldest > total) return fail("the state frame has bad history");
            // A view scrolled back into the history stays on its rows, as it
            // does when a terminal scrolls rows into its own history.
            const std::size_t viewport = s->m_viewportOffset;
            std::size_t added = 0;
            // The writer's first visible row is total: the rows before it
            // that did not come yet hold their places, blank, so that each
            // row of the replica has the writer's id.
            if (total > st.next) {
                const std::uint64_t blanks = std::min<std::uint64_t>(total - st.next, s->m_maxScrollback);
                // Past the replica's own limit, a view scrolled back ends on
                // the oldest row, as in the terminal that parsed the output.
                if (blanks < total - st.next) s->clearHistory();
                for (std::uint64_t i = 0; i < blanks; ++i) {
                    Row blank;
                    blank.plain = true;
                    s->m_rows.insert(s->m_rows.begin() + std::ptrdiff_t(s->historySize()), std::move(blank));
                }
                added += std::size_t(blanks);
                st.next = total;
                s->m_rowsDropped = st.next - s->historySize();
            }
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                const std::uint64_t id = p.varint();
                Row row;
                if (!p.ok() || id < st.hole || id >= st.next || !decodeRow(p, *s, st, row, true)) {
                    discard(*s, row);
                    return fail("the state frame has a bad history row");
                }
                // A row past the hole: the ones in between are gone on the
                // writer's side, and with them every older row. Their blank
                // places go below.
                st.hole = id + 1;
                if (id < s->m_rowsDropped) {
                    // Older than the replica keeps.
                    discard(*s, row);
                    continue;
                }
                if (id - s->m_rowsDropped >= s->historySize()) {
                    discard(*s, row);
                    return fail("the state frame has a history row that the replica has no place for");
                }
                Row& place = s->m_rows[std::size_t(id - s->m_rowsDropped)];
                discard(*s, place);
                place = std::move(row);
            }
            if (!p.ok() || !p.done()) return fail("the state frame has bad history");
            // The writer sends no row older than the oldest that exists.
            st.hole = std::max(st.hole, oldest);
            st.from = std::max(st.from, oldest);
            st.oldest = oldest;
            // The rows that the writer dropped go here too, and the replica
            // keeps no more than its own limit.
            while (s->historySize() > 0 && (s->m_rowsDropped < oldest || s->historySize() > s->m_maxScrollback)) {
                s->releaseRow(s->m_rows.front());
                s->m_rows.pop_front();
                ++s->m_rowsDropped;
            }
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
                if (!p.ok() || st.from == 0 || id + 1 != st.from || !decodeRow(p, *s, st, row, true)) {
                    discard(*s, row);
                    return fail("the state frame has a bad older history row");
                }
                // Only while the replica's oldest row is the writer's oldest
                // that came: a replica whose own limit dropped rows (a lower
                // scrollback since) has no place for older ones.
                const bool place = s->historySize() < s->m_maxScrollback && s->m_rowsDropped == st.from;
                st.from = id;
                if (!place) {
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
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                const std::uint8_t alt = p.byte();
                RegionScroll scroll;
                scroll.top = p.varint();
                scroll.bottom = p.varint();
                scroll.left = int(p.bounded(std::uint64_t(Screen::MaxColumns - 1)));
                scroll.right = int(p.bounded(std::uint64_t(Screen::MaxColumns - 1)));
                const std::uint8_t full = p.byte();
                scroll.delta = int(p.signedBounded(INT_MIN, INT_MAX));
                if (!p.ok() || alt > 1 || full > 1 || scroll.top > scroll.bottom || scroll.left > scroll.right || scroll.delta == 0) {
                    return fail("the state frame has a bad region scroll");
                }
                scroll.fullWidth = full;
                scrolled.emplace_back(alt == 1, scroll);
            }
            if (!p.ok() || lost > 1) return fail("the state frame has bad region scrolls");
            if (lost) {
                // Too many to carry: every row between the top of the highest
                // region that scrolled and the bottom of the lowest, on each
                // screen, left its region. History rows never move.
                for (const bool alt : {false, true}) {
                    const std::uint8_t any = p.byte();
                    if (any > 1) return fail("the state frame has bad region scrolls");
                    if (!any) continue;
                    RegionScroll all;
                    all.top = p.varint();
                    all.bottom = p.varint();
                    if (!p.ok() || all.top > all.bottom || all.bottom - all.top >= std::uint64_t(INT_MAX)) {
                        return fail("the state frame has bad region scrolls");
                    }
                    all.right = Screen::MaxColumns - 1;
                    all.delta = -int(all.bottom - all.top + 1);
                    scrolled.emplace_back(alt, all);
                }
            }
            if (!p.ok() || !p.done()) return fail("the state frame has bad region scrolls");
            break;
        }
        case ShellTag: {
            const std::uint8_t zone = p.byte();
            const std::uint8_t clicks = p.byte();
            const std::uint8_t running = p.byte();
            const std::uint8_t redraw = p.byte();
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
            if (!p.ok() || !p.done() || zone > std::uint8_t(Terminal::SemanticZone::Input) || clicks > 1 || running > 1 || redraw > 1) {
                return fail("the state frame has bad shell marks");
            }
            t.m_semanticZone = Terminal::SemanticZone(zone);
            t.m_promptClickEvents = clicks;
            t.m_commandRunning = running;
            // The window holds its frames while the shell draws its prompt
            // after a resize, as for a program of its own.
            t.m_promptRedrawPending = redraw;
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
            // The fields as the terminal takes them: clean text in its bounds,
            // shown in labels. The records sorted by id, each once, as
            // ProgramStatus keeps them for its lookups.
            const auto field = [&p](std::size_t max) {
                const std::string_view v = p.string(max);
                if (!detail::isCleanText(v)) p.fail();
                return std::string(v);
            };
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                StatusRecord r;
                r.id = field(ProgramStatus::MaxId);
                const std::uint8_t state = p.byte();
                const std::uint8_t kind = p.byte();
                r.progress = int(p.signedBounded(-1, 100));
                r.app = field(ProgramStatus::MaxApp);
                r.title = field(ProgramStatus::MaxTitle);
                r.msg = field(ProgramStatus::MaxMessage);
                r.serial = p.varint();
                if (!p.ok() || state > std::uint8_t(StatusState::Error) || kind > std::uint8_t(BlockKind::Auth)
                    || (!records.empty() && !(records.back().id < r.id))) {
                    return fail("the state frame has a bad status record");
                }
                r.state = StatusState(state);
                r.kind = BlockKind(kind);
                serial = std::max(serial, r.serial);
                records.push_back(std::move(r));
            }
            if (!p.ok() || !p.done()) return fail("the state frame has bad status records");
            ProgramStatus& status = t.m_programStatus;
            // Every change of a record gives it a new serial, terminal-wide
            // (ProgramStatus::serial): a record with its old serial is the
            // same record, and one that went shows in the count.
            statusChanged = statusChanged
                || !std::equal(records.begin(), records.end(), status.m_records.begin(), status.m_records.end(),
                               [](const StatusRecord& a, const StatusRecord& b) { return a.serial == b.serial && a.id == b.id; });
            status.m_records = std::move(records);
            status.m_serial = std::max(status.m_serial, serial);
            break;
        }
        case ContextsTag: {
            const std::uint64_t count = p.bounded(ShellContexts::MaxDepth);
            std::vector<ShellContext> stack;
            bool remoteHost = false;
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                ShellContext c;
                const auto field = [&p](std::string& to) {
                    const std::string_view v = p.string(ShellContexts::MaxField);
                    if (!detail::isCleanText(v)) p.fail();
                    to = v;
                };
                field(c.id);
                const std::uint8_t remote = p.byte();
                for (std::string* f : {&c.type, &c.user, &c.hostname, &c.targetUser, &c.targetHost, &c.container, &c.vm}) field(*f);
                // One iTerm2 entry at most, without an id; every other one has
                // an id (ShellContexts).
                if (!p.ok() || remote > 1 || (remote && remoteHost) || (!remote && c.id.empty())) {
                    return fail("the state frame has a bad shell context");
                }
                remoteHost = remoteHost || remote;
                c.iterm2RemoteHost = remote;
                stack.push_back(std::move(c));
            }
            if (!p.ok() || !p.done()) return fail("the state frame has bad shell contexts");
            if (stack != t.m_contexts.m_stack) {
                t.m_contexts.m_stack = std::move(stack);
                contextsChanged = true;
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
        case ImagesResetTag: {
            // A writer that starts over: nothing that it named before stays.
            if (!p.done()) return fail("the state frame has a bad images reset");
            m_frames.clear();
            m_frameBytes = 0;
            m_building = 0;
            for (int which = 0; which < 2; ++which) {
                t.m_graphics[which].clear();
                m_animations[which].clear();
            }
            break;
        }
        case FrameTag: {
            const std::uint64_t id = p.varint();
            const std::uint64_t offset = p.varint();
            if (!p.ok() || id == 0) return fail("the state frame has a bad image frame");
            if (offset == 0) {
                const std::uint64_t width = p.bounded(AfterglowProto::GraphicsMaxSide);
                const std::uint64_t height = p.bounded(AfterglowProto::GraphicsMaxSide);
                const std::uint8_t format = p.byte();
                // The writer finishes or drops each frame before it starts
                // the next, and names each once.
                if (!p.ok() || width == 0 || height == 0 || format > std::uint8_t(PixelFormat::IndexedAlpha)
                    || m_building != 0 || m_frames.contains(id)) {
                    return fail("the state frame has a bad image frame");
                }
                auto frame = std::make_shared<ImageFrame>();
                frame->width = std::uint32_t(width);
                frame->height = std::uint32_t(height);
                frame->format = PixelFormat(format);
                const std::size_t size = std::size_t(width) * std::size_t(height) * bytesPerPixel(frame->format);
                // What both screens show and what comes for them: a writer
                // keeps no more.
                if (m_frames.size() >= 4 * GraphicsStore::MaxStoreFrames || m_frameBytes + size > 4 * GraphicsStore::MaxQuota) {
                    return fail("the state frame has more image frames than the quota takes");
                }
                frame->pixels().reserve(size);
                m_frames[id] = ReceivedFrame{std::move(frame), size, 0};
                m_frameBytes += size;
                m_building = id;
            } else if (id != m_building) {
                return fail("the state frame continues an image frame that it did not start");
            }
            ReceivedFrame& f = m_frames[id];
            const std::string_view part = p.rest();
            if (!p.ok() || offset != f.received || part.empty() || part.size() > f.size - f.received) {
                return fail("the state frame has a bad part of an image frame");
            }
            std::vector<std::uint8_t>& bytes = f.frame->pixels();
            bytes.insert(bytes.end(), part.begin(), part.end());
            f.received += part.size();
            if (f.complete()) m_building = 0;
            break;
        }
        case ImagesTag: {
            const std::uint8_t which = p.byte();
            if (which > 1) return fail("the state frame names no screen");
            GraphicsStore& store = t.m_graphics[which];
            std::unordered_map<std::uint32_t, Animation>& animations = m_animations[which];
            const std::uint64_t removed = p.bounded(GraphicsStore::MaxImages);
            for (std::uint64_t i = 0; i < removed && p.ok(); ++i) {
                const std::uint32_t id = std::uint32_t(p.bounded(0xffffffffu));
                store.m_images.erase(id);
                animations.erase(id);
            }
            // A placement of an image that went goes with it.
            std::erase_if(store.m_placements, [&store](const Placement& placement) {
                return !store.m_images.contains(placement.imageId)
                    || (placement.parentImage && !store.m_images.contains(placement.parentImage));
            });
            const std::uint64_t count = p.bounded(GraphicsStore::MaxImages);
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                Image image;
                image.id = std::uint32_t(p.bounded(0xffffffffu));
                image.number = std::uint32_t(p.bounded(0xffffffffu));
                const std::uint8_t flags = p.byte();
                image.width = std::uint32_t(p.bounded(AfterglowProto::GraphicsMaxSide));
                image.height = std::uint32_t(p.bounded(AfterglowProto::GraphicsMaxSide));
                const std::uint64_t frames = p.bounded(GraphicsStore::MaxFrames);
                for (std::uint64_t k = 0; k < frames && p.ok(); ++k) {
                    const auto it = m_frames.find(p.varint());
                    // A writer keeps each frame of an image at its size.
                    if (!p.ok() || it == m_frames.end() || !it->second.complete() || it->second.frame->width != image.width
                        || it->second.frame->height != image.height) {
                        return fail("the state frame has an image with a frame that did not come");
                    }
                    image.frames.push_back(it->second.frame);
                }
                // One gap for each frame, of what a command can give (z=, an
                // int): the replica's animation reads the gap of each frame.
                const std::uint64_t gaps = p.bounded(GraphicsStore::MaxFrames);
                for (std::uint64_t k = 0; k < gaps && p.ok(); ++k) image.gaps.push_back(std::uint32_t(p.bounded(INT_MAX)));
                const std::uint8_t state = p.byte();
                image.maxLoops = std::uint32_t(p.bounded(0xffffffffu));
                const std::uint32_t loopsDone = std::uint32_t(p.bounded(0xffffffffu));
                const std::size_t currentFrame = std::size_t(p.bounded(frames ? frames - 1 : 0));
                const Animation sent{std::uint32_t(p.bounded(0xffffffffu)), std::uint32_t(p.bounded(0xffffffffu)), p.varint()};
                image.sequence = sent.sequence;
                image.order = p.varint();
                if (!p.ok() || frames == 0 || gaps != frames || (flags & ~(ImageSixel | ImageAnonymous | ImageTransient))
                    || state > std::uint8_t(Image::Animation::Running)) {
                    return fail("the state frame has a bad image");
                }
                image.isSixel = flags & ImageSixel;
                image.anonymous = flags & ImageAnonymous;
                image.transient = flags & ImageTransient;
                image.stateCommands = sent.stateCommands;
                image.frameCommands = sent.frameCommands;
                // The replica's clock moves the animation. It takes the
                // writer's state and frame for a command of the program, and
                // all of the writer's animation for an image sent again.
                const auto old = store.m_images.find(image.id);
                const auto last = animations.find(image.id);
                const bool known = old != store.m_images.end() && last != animations.end() && last->second.sequence == sent.sequence;
                if (!known) {
                    image.animation = Image::Animation(state);
                    image.loopsDone = loopsDone;
                    image.currentFrame = currentFrame;
                    image.frameShownAtMs = 0;
                } else {
                    const Image& before = old->second;
                    // The frame that the replica shows, in the frames as they
                    // are now: one that a delete moved keeps showing, one
                    // that an edit replaced keeps its place, and one that
                    // went gives its place to the next, as the writer's
                    // store does.
                    const std::size_t shownIndex = std::min(before.currentFrame, before.frames.size() - 1);
                    const auto found = std::find(image.frames.begin(), image.frames.end(), before.frames[shownIndex]);
                    std::size_t current = shownIndex;
                    bool restart = false;
                    if (found != image.frames.end()) {
                        current = std::size_t(found - image.frames.begin());
                    } else if (image.frames.size() != before.frames.size()) {
                        current = std::min(shownIndex, image.frames.size() - 1);
                        restart = true;
                    }
                    if (sent.frameCommands != last->second.frameCommands && currentFrame != current) {
                        current = currentFrame;
                        restart = true;
                    }
                    if (sent.stateCommands != last->second.stateCommands) {
                        image.animation = Image::Animation(state);
                        image.loopsDone = loopsDone;
                        // A running animation keeps its clock when only its
                        // mode changes.
                        restart = restart || (before.animation == Image::Animation::Stopped && image.animation != Image::Animation::Stopped);
                    } else {
                        image.animation = before.animation;
                        image.loopsDone = before.loopsDone;
                    }
                    image.currentFrame = current;
                    image.frameShownAtMs = restart ? 0 : before.frameShownAtMs;
                }
                animations[image.id] = sent;
                store.m_images[image.id] = std::move(image);
            }
            if (!p.ok() || !p.done() || store.m_images.size() > GraphicsStore::MaxImages) {
                return fail("the state frame has bad images");
            }
            store.indexVirtual();
            // An image that a program sent, or changed, on the screen that
            // shows: output, as a local pane's scroll-on-output takes it.
            if (which == (t.m_modes.altScreen ? 1 : 0) && (removed > 0 || count > 0)) m_screenChanged = true;
            break;
        }
        case PlacementsTag: {
            const std::uint8_t which = p.byte();
            if (which > 1) return fail("the state frame names no screen");
            GraphicsStore& store = t.m_graphics[which];
            const std::uint64_t count = p.bounded(GraphicsStore::MaxPlacements);
            std::vector<Placement> placements;
            const auto value = [&p] { return int(p.signedBounded(INT_MIN, INT_MAX)); };
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                Placement pl;
                pl.imageId = std::uint32_t(p.bounded(0xffffffffu));
                pl.placementId = std::uint32_t(p.bounded(0xffffffffu));
                // A stable row id, which a placement multiplies by the cell
                // height: past this, the arithmetic of a row overflows.
                pl.row = p.bounded(std::uint64_t(INT64_MAX) / 65536);
                for (int* v : {&pl.col, &pl.offsetX, &pl.offsetY, &pl.srcX, &pl.srcY, &pl.srcW, &pl.srcH, &pl.cols, &pl.rows}) *v = value();
                pl.z = std::int32_t(value());
                pl.parentImage = std::uint32_t(p.bounded(0xffffffffu));
                pl.parentPlacement = std::uint32_t(p.bounded(0xffffffffu));
                pl.relativeX = value();
                pl.relativeY = value();
                const std::uint8_t isVirtual = p.byte();
                pl.displayWidth = std::uint32_t(p.bounded(0xffffffffu));
                pl.displayHeight = std::uint32_t(p.bounded(0xffffffffu));
                if (!p.ok() || isVirtual > 1 || !store.m_images.contains(pl.imageId)
                    || (pl.parentImage && !store.m_images.contains(pl.parentImage))) {
                    return fail("the state frame has a bad image placement");
                }
                pl.isVirtual = isVirtual;
                placements.push_back(pl);
            }
            if (!p.ok() || !p.done()) return fail("the state frame has bad image placements");
            store.m_placements = std::move(placements);
            store.indexVirtual();
            if (which == (t.m_modes.altScreen ? 1 : 0)) m_screenChanged = true;
            break;
        }
        case CellSizeTag: {
            const std::uint64_t width = p.bounded(0xffff);
            const std::uint64_t height = p.bounded(0xffff);
            if (!p.ok() || !p.done()) return fail("the state frame has a bad cell size");
            t.m_cellWidth = int(width);
            t.m_cellHeight = int(height);
            break;
        }
        case DropFramesTag: {
            const std::uint64_t count = p.bounded(m_frames.size());
            for (std::uint64_t i = 0; i < count && p.ok(); ++i) {
                const std::uint64_t id = p.varint();
                if (id == m_building) m_building = 0;
                if (const auto it = m_frames.find(id); it != m_frames.end()) {
                    m_frameBytes -= it->second.size;
                    m_frames.erase(it);
                }
            }
            if (!p.ok() || !p.done()) return fail("the state frame drops bad image frames");
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
    // The host hears of the scrolls as the writer's did, with the screen
    // that scrolled active, and with the frame applied: the rows that they
    // name have the writer's ids.
    const bool altScreen = t.m_modes.altScreen;
    Screen* const active = t.m_active;
    for (const auto& [alt, scroll] : scrolled) {
        t.m_modes.altScreen = alt;
        t.m_active = alt ? t.m_alternate.get() : t.m_primary.get();
        t.m_host.regionScrolled(scroll);
    }
    t.m_modes.altScreen = altScreen;
    t.m_active = active;
    if (statusChanged) t.m_host.programStatusChanged();
    if (contextsChanged) t.m_host.contextChanged();
    if (touched) {
        t.touch();
    } else if (imagesTouched) {
        t.touchImages();
    }
    return true;
}

} // namespace AfterglowVt
