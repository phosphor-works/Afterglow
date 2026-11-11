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
// visibility, encode a paste, and read the images as the renderer does. The
// host that supports everything answers aglow's remote socket query too. One
// record gives the rest of the input to the reader of the mux state codec
// (StateSync.h), as a frame for a fresh replica.
//
//   clang++ -std=c++20 -g -O1 -fsanitize=fuzzer,address,undefined ...

#include <AfterglowVt/Input.h>
#include <AfterglowVt/Selection.h>
#include <AfterglowVt/StateSync.h>
#include <AfterglowVt/Terminal.h>
#include <AfterglowVt/TextSearch.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// A host that either supports everything (resizes, clipboard, images,
/// notifications) or nothing, so both sides of each host check are fuzzed.
struct Host : AfterglowVt::ITerminalHost
{
    bool permissive = false;

    /// A remote socket reply reads back as the answer that remoteSocket()
    /// gave for it: the path with a control byte must have become error=off.
    void writeToPty(std::string_view bytes) override
    {
        constexpr std::string_view Introducer = "\x1b]24569;";
        if (!bytes.starts_with(Introducer)) {
            return;
        }
        const std::size_t end = bytes.ends_with('\x07') ? bytes.size() - 1 : bytes.size() - 2;
        const auto answer = AfterglowProto::Remote::parseReply(bytes.substr(Introducer.size(), end - Introducer.size()));
        const auto expected = std::exchange(expectedReply, std::nullopt);
        if (!answer || !expected || *answer != *expected) {
            __builtin_trap();
        }
    }
    bool requestResize(int, int) override { return permissive; }
    bool screenSize(int& width, int& height) override
    {
        width = 1920;
        height = 1080;
        return permissive;
    }
    bool clipboardRequest(const AfterglowVt::ClipboardRequest&) override { return permissive; }
    /// The answers of a pane with remote commands, in turn: a socket path, a
    /// reason for none, and a path the reply must refuse (a control byte).
    std::optional<AfterglowProto::Remote::Answer> remoteSocket() override
    {
        using AfterglowProto::Remote::Error;
        if (!permissive) {
            return std::nullopt;
        }
        switch (remoteAnswers++ % 3) {
        case 0:
            expectedReply = AfterglowProto::Remote::Answer{"/run/user/1000/afterglow/0123456789abcdef.sock", Error::Off};
            return expectedReply;
        case 1:
            expectedReply = AfterglowProto::Remote::Answer{{}, Error::Forward};
            return expectedReply;
        default:
            expectedReply = AfterglowProto::Remote::Answer{{}, Error::Off};
            return AfterglowProto::Remote::Answer{"/tmp/a\x1b]2;x\x07.sock", Error::Off};
        }
    }
    unsigned remoteAnswers = 0;
    /// What the reply to the last answer must parse back to.
    std::optional<AfterglowProto::Remote::Answer> expectedReply;
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

/// A replica's host, and the host of the terminal that the replica copies:
/// neither has a program to answer.
struct QuietHost : AfterglowVt::ITerminalHost
{
    void writeToPty(std::string_view) override {}
};

/// As a window's session pane does after each frame: the replica animates
/// its images on its own clock, and the renderer draws them.
void showReplica(AfterglowVt::Terminal& replica)
{
    for (const std::uint64_t now : {std::uint64_t(1000), std::uint64_t(1100), std::uint64_t(5000)}) {
        (void)replica.advanceAnimations(now);
    }
    const auto placements = replica.graphics().visible(replica.screen(), 8, 16, 1);
    for (const auto& placement : replica.graphics().placements()) {
        if (placement.isVirtual) (void)replica.graphics().placeholderSlice(placement.imageId, 0, 0, 0, 0, 0, 8, 16);
    }
    (void)placements;
}

/// @p frame to the StateReader of a fresh replica, as a window applies what a
/// session sends. A refused frame must leave the reader able to take the first
/// frame of a writer that starts over, since that is how a window recovers
/// (StateSyncTest's ReaderRefusesBrokenFrames).
void checkStateFrame(std::string_view frame, int cols, int rows, std::size_t scrollback)
{
    QuietHost replicaHost;
    AfterglowVt::TerminalOptions opts;
    opts.maxScrollback = scrollback;
    AfterglowVt::Terminal replica(cols, rows, replicaHost, opts);
    AfterglowVt::StateReader reader;
    std::string error;
    if (reader.apply(replica, frame, &error)) {
        showReplica(replica);
        return;
    }
    QuietHost sourceHost;
    AfterglowVt::TerminalOptions sourceOpts;
    sourceOpts.maxScrollback = 10;
    AfterglowVt::Terminal source(20, 4, sourceHost, sourceOpts);
    source.feed("\x1b[31mred\x1b[0m\r\nmore");
    AfterglowVt::StateWriter writer;
    if (!reader.apply(replica, writer.update(source, 1 << 20), &error)) {
        __builtin_trap();
    }
    using Scope = AfterglowVt::Terminal::TextScope;
    if (replica.text(Scope::Screen) != source.text(Scope::Screen)) {
        __builtin_trap();
    }
}

/// A replica of the fuzzed terminal, as a window of a session keeps one:
/// a frame of the writer after each control record, so that images are
/// replaced and dropped while their pixels go. Each frame that a writer makes
/// must apply.
struct Mirror
{
    QuietHost host;
    AfterglowVt::Terminal replica;
    AfterglowVt::StateWriter writer;
    AfterglowVt::StateReader reader;

    explicit Mirror(const AfterglowVt::TerminalOptions& options)
        : replica(20, 8, host, options)
    {
    }

    /// Small frames for a small screen, with config's scrollback bits, so
    /// that rows, history and images wait. A screen of thousands of cells at
    /// that budget takes thousands of frames, too slow an input to fuzz.
    static std::size_t budget(const AfterglowVt::Terminal& source, bool small)
    {
        const bool smallScreen = std::size_t(source.cols()) * std::size_t(source.rows()) <= 4096;
        return small && smallScreen ? 4096 : std::size_t(1) << 20;
    }

    void step(const AfterglowVt::Terminal& source, bool small)
    {
        std::string error;
        if (!reader.apply(replica, writer.update(source, budget(source, small)), &error)) {
            __builtin_trap();
        }
    }

    /// The rest, then the replica shows what the source does.
    void finish(const AfterglowVt::Terminal& source, bool small)
    {
        int guard = 0;
        for (; guard < 4096; ++guard) {
            const std::string frame = writer.update(source, budget(source, small));
            if (frame.empty() && !writer.pending()) break;
            std::string error;
            if (!reader.apply(replica, frame, &error)) {
                __builtin_trap();
            }
        }
        // A writer that never catches up, at a budget that its content fits
        // many times over, is stuck.
        if (guard == 4096 && budget(source, small) > 4096) {
            __builtin_trap();
        }
        using Scope = AfterglowVt::Terminal::TextScope;
        if (guard < 4096 && replica.text(Scope::Screen) != source.text(Scope::Screen)) {
            __builtin_trap();
        }
        if (guard < 4096) {
            for (const bool alternate : {false, true}) {
                const auto& a = source.graphics(alternate).images();
                const auto& b = replica.graphics(alternate).images();
                if (a.size() != b.size()) __builtin_trap();
                for (const auto& [id, image] : a) {
                    const auto it = b.find(id);
                    if (it == b.end() || it->second.frames.size() != image.frames.size()) __builtin_trap();
                }
            }
        }
        showReplica(replica);
    }
};

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
    Mirror mirror(opts);
    const bool smallFrames = (config & 0x18) != 0;

    // 0xFF introduces a control record: [0xFF, op, a, b]. Op 13 feeds a raw
    // 0xFF, which the record marker would otherwise keep out of the stream.
    // Op 11 with bit 4 set ends the stream: the rest of the input, 0xFF bytes
    // included, is a state frame.
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
            if ((op / 16) & 1) {
                // An a by b replica, with the scrollback of bits 5-6.
                checkStateFrame(std::string_view(reinterpret_cast<const char*>(data + i + 4), size - (i + 4)),
                                1 + a % 60, 1 + b % 30, Scrollbacks[(op / 32) & 3]);
                return 0;
            }
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
        mirror.step(t, smallFrames);
        start = i + 4;
        i += 3;
    }
    if (start < size) {
        t.feed(std::string_view(reinterpret_cast<const char*>(data + start), size - start));
    }
    // The codec carries no viewport: the replica's shows the live rows.
    t.resetViewport();
    mirror.finish(t, smallFrames);
    return 0;
}
