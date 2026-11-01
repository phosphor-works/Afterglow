// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libFuzzer target for the terminal core. The first byte picks the options
// (bidi, ambiguous width, scrollback, screen readback, file transfer) and how
// much the host supports. The rest is a byte stream fed to a Terminal, with
// control records that do what the application does between reads: resize
// (odd cell sizes and very large grids too), scroll the viewport, copy a
// selection, search the text as the search bar does, advance animations,
// reset, clear, read text, reload options, end the program, change
// visibility, encode a paste, and read the images as the renderer does.
//
//   clang++ -std=c++20 -g -O1 -fsanitize=fuzzer,address,undefined ...

#include <AfterglowVt/Input.h>
#include <AfterglowVt/Selection.h>
#include <AfterglowVt/Terminal.h>
#include <AfterglowVt/TextSearch.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// A host that either supports everything (resizes, clipboard, images,
/// notifications) or nothing, so both sides of each host check are fuzzed.
struct Host : AfterglowVt::ITerminalHost
{
    bool permissive = false;

    void writeToPty(std::string_view) override { }
    bool requestResize(int, int) override { return permissive; }
    bool screenSize(int& width, int& height) override
    {
        width = 1920;
        height = 1080;
        return permissive;
    }
    bool clipboardRequest(const AfterglowVt::ClipboardRequest&) override { return permissive; }
    std::string notificationCapabilities() override { return permissive ? "a=focus,report:o=always:p=title,body" : ""; }
    bool decodeImage(std::string_view, std::uint32_t& width, std::uint32_t& height,
                     std::vector<std::uint8_t>& rgba) override
    {
        if (!permissive) {
            return false;
        }
        width = 2;
        height = 2;
        rgba.assign(16, 0x80);
        return true;
    }
    bool decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height,
                   std::vector<std::uint8_t>& rgba) override
    {
        return decodeImage(png, width, height, rgba);
    }
};

/// With bracketed paste on, the paste is wrapped once and cannot end early.
/// In both modes the text holds no control but HT and CR (LF becomes CR, as
/// encodePaste folds it), no DEL and no C1.
void checkPaste(std::string_view text, bool bracketed)
{
    AfterglowVt::Modes modes;
    modes.bracketedPaste = bracketed;
    const std::string out = AfterglowVt::encodePaste(text, modes);
    std::string_view inner = out;
    if (bracketed) {
        constexpr std::string_view Begin = "\x1b[200~";
        constexpr std::string_view End = "\x1b[201~";
        if (!out.starts_with(Begin) || !out.ends_with(End) || out.find(End) != out.size() - End.size()) {
            __builtin_trap();
        }
        inner = inner.substr(Begin.size(), inner.size() - Begin.size() - End.size());
    }
    for (std::size_t i = 0; i < inner.size(); ++i) {
        const auto c = static_cast<unsigned char>(inner[i]);
        const bool control = (c < 0x20 && c != '\t' && c != '\r') || c == 0x7F;
        const bool c1 = c == 0xC2 && i + 1 < inner.size() && static_cast<unsigned char>(inner[i + 1]) >= 0x80
            && static_cast<unsigned char>(inner[i + 1]) <= 0x9F;
        if (control || c1) {
            __builtin_trap();
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0) {
        return 0;
    }
    // Byte 0: bits 0-1 bidi, 2 ambiguous width, 3-4 scrollback (64, 0, 1,
    // 64), 5 a permissive host, 6 screen readback, 7 kitty file transfer
    // (t=f, t=t and t=s open real paths on the fuzzing machine).
    const std::uint8_t config = data[0];
    ++data;
    --size;
    Host host;
    host.permissive = (config & 0x20) != 0;
    AfterglowVt::TerminalOptions opts;
    opts.bidi = AfterglowVt::BidiMode(config & 3);
    opts.ambiguousWide = (config & 4) != 0;
    constexpr std::size_t Scrollbacks[] = {64, 0, 1, 64};
    opts.maxScrollback = Scrollbacks[(config >> 3) & 3];
    opts.allowScreenReadback = (config & 0x40) != 0;
    opts.graphicsFileTransfer = (config & 0x80) != 0;
    AfterglowVt::Terminal t(20, 8, host, opts);
    t.resize(20, 8, 8, 16);
    std::uint64_t now = 0;

    // 0xFF introduces a control record: [0xFF, op, a, b]. Op 13 feeds a raw
    // 0xFF, which the record marker would otherwise keep out of the stream.
    std::size_t start = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] != 0xFF || i + 3 >= size) {
            continue;
        }
        t.feed(std::string_view(reinterpret_cast<const char*>(data + start), i - start));
        const std::uint8_t op = data[i + 1];
        const int a = data[i + 2];
        const int b = data[i + 3];
        switch (op % 16) {
        case 0: {
            // Cell sizes the pixel reports must survive: usual, zero, one,
            // and the largest, which overflows 32 bits across a grid.
            constexpr int CellSizes[][2] = {{8, 16}, {0, 0}, {1, 1}, {65535, 65535}};
            const auto* cell = CellSizes[(op / 16) % 4];
            if (op / 64 == 3) {
                t.resize(2 + a * 16, 1 + b * 4, cell[0], cell[1]); // up to the column and cell limits
            } else {
                t.resize(1 + a % 60, 1 + b % 30, cell[0], cell[1]);
            }
            break;
        }
        case 1:
            t.scrollViewport(long(a) - 128);
            break;
        case 2: {
            const auto& s = t.screen();
            AfterglowVt::Selection sel;
            sel.anchor = {s.rowsDropped() + std::uint64_t(a % (s.totalRows() + 1)), a % s.cols()};
            sel.head = {s.rowsDropped() + std::uint64_t(b % (s.totalRows() + 1)), b % s.cols()};
            sel.mode = AfterglowVt::SelectionMode((op / 16) % 4);
            sel.layoutEpoch = s.layoutEpoch();
            sel.active = true;
            (void)sel.text(s);
            break;
        }
        case 3: {
            // As the search bar: a snapshot of every row (multicell blocks,
            // clusters, wide tails), then a literal or a regular expression.
            const auto& s = t.screen();
            AfterglowVt::TextSnapshot snapshot;
            for (std::size_t row = 0; row < s.totalRows(); ++row) {
                if (!AfterglowVt::appendTextRow(snapshot, s, row)) {
                    break;
                }
            }
            static constexpr std::u32string_view Patterns[] = {U"a", U"ab", U"\\w+", U"(.)\\1", U"^\\s*$", U"中"};
            const std::atomic<bool> cancelled{false};
            const auto pattern = Patterns[std::size_t(a) % std::size(Patterns)];
            (void)AfterglowVt::searchText(snapshot, pattern, (b & 1) != 0, cancelled, 1000, (b & 2) != 0);
            break;
        }
        case 4:
            now += std::uint64_t(a) * 7 + std::uint64_t(b);
            (void)t.advanceAnimations(now);
            break;
        case 5:
            t.reset();
            break;
        case 6: {
            (void)t.text(AfterglowVt::Terminal::TextScope::Screen, std::size_t(a) * 64 + 1);
            (void)t.text(AfterglowVt::Terminal::TextScope::All);
            (void)t.text(AfterglowVt::Terminal::TextScope::LastOutput);
            const auto regions = t.semanticRegions();
            if (!regions.empty()) {
                (void)t.semanticRegionText(regions[std::size_t(b) % regions.size()].id, std::size_t(a) * 16 + 1);
            }
            break;
        }
        case 7: {
            // A live reload, as the window does for every pane.
            AfterglowVt::TerminalOptions o = t.options();
            o.ambiguousWide = (a & 1) != 0;
            o.bidi = AfterglowVt::BidiMode((a >> 1) & 3);
            o.maxScrollback = Scrollbacks[(a >> 3) & 3];
            o.cursorShape = AfterglowVt::CursorShape(b % 3);
            o.graphicsQuota = std::size_t(1 + b) << 16;
            o.graphicsFileTransfer = (a & 0x20) != 0;
            t.setOptions(o);
            break;
        }
        case 8:
            t.clearScreenAndHistory();
            break;
        case 9:
            t.programExited();
            break;
        case 10:
            t.setVisible((a & 1) != 0);
            break;
        case 11: {
            // The next chunk as a paste.
            std::size_t end = i + 4;
            while (end < size && data[end] != 0xFF) {
                ++end;
            }
            checkPaste(std::string_view(reinterpret_cast<const char*>(data + i + 4), end - (i + 4)), (a & 1) != 0);
            break;
        }
        case 12: {
            // As the renderer does each frame: the visible images, and the
            // slices placeholder cells show.
            const int cw = a == 255 ? 65535 : a;
            const int ch = b == 255 ? 65535 : b;
            const auto placements = t.graphics().visible(t.screen(), cw, ch, (op / 16) % 8);
            for (const auto& placement : placements) {
                (void)t.graphics().placeholderSlice(placement.imageId, std::uint32_t(b), a % 300, b % 300, cw, ch, cw,
                                                    ch);
            }
            // visible() leaves virtual placements out, and those are what
            // placeholder cells usually show.
            for (const auto& placement : t.graphics().placements()) {
                if (placement.isVirtual) {
                    (void)t.graphics().placeholderSlice(placement.imageId, (b & 1) ? placement.placementId : 0, a % 300,
                                                        b % 300, cw, ch, cw, ch);
                }
            }
            break;
        }
        case 13:
            t.feed("\xff");
            break;
        case 14:
            t.resetViewport();
            break;
        case 15:
            t.setViewportOffset(std::size_t(a) * 4);
            break;
        }
        start = i + 4;
        i += 3;
    }
    if (start < size) {
        t.feed(std::string_view(reinterpret_cast<const char*>(data + start), size - start));
    }
    return 0;
}
