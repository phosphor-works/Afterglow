// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libFuzzer target for the protocol library: the readers of what a remote
// program sends, called as the application calls them, outside a Terminal.
// The first byte picks the reader. The rest is its input. Readers that take
// several messages split the input at NUL bytes, one message each.
//
//   0 file transfer messages (decodeTransfer, transferCancelId)
//   1 drag and drop commands, put back together by a DndAssembler, with
//     the ends of the protocol (abort, end, reset) between them
//   2 a delta, applied in pieces against a small basis
//   3 a delta signature, and a delta made against it
//   4 graphics replies, and command keys parsed, written and parsed again
//   5 clipboard replies, OSC 52 messages and clipboard requests
//   6 zlib data, inflated whole with a limit and in steps
//   7 the text checks that sanitize ids and names, and printable text
//   8 JSON for the phosphor-ipc client, written and read again, and the
//     stdin parts of `aglow run`
//   9 X11 colour specs (parseColorSpec), and the colour read written again

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Clipboard.h>
#include <AfterglowProto/Color.h>
#include <AfterglowProto/Delta.h>
#include <AfterglowProto/DragDrop.h>
#include <AfterglowProto/Graphics.h>
#include <AfterglowProto/Json.h>
#include <AfterglowProto/Run.h>
#include <AfterglowProto/Text.h>
#include <AfterglowProto/Transfer.h>
#include <AfterglowProto/Zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::string_view> messages(std::string_view input)
{
    std::vector<std::string_view> out;
    for (std::size_t start = 0;;) {
        const std::size_t end = input.find('\0', start);
        out.push_back(input.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
            return out;
        }
        start = end + 1;
    }
}

/// A basis of @p size bytes in blocks of @p blockSize. A block index past
/// the basis is a bug in the applier, which must refuse it first.
struct Basis : AfterglowProto::DeltaTarget
{
    std::string bytes;
    std::uint32_t blockSize = 1;
    std::size_t written = 0;

    bool readBlock(std::uint64_t index, std::string& out) override
    {
        if (index >= (bytes.size() + blockSize - 1) / blockSize) {
            __builtin_trap();
        }
        const std::size_t at = std::size_t(index) * blockSize;
        out.append(bytes, at, blockSize);
        return true;
    }
    bool write(std::string_view part) override
    {
        written += part.size();
        return written <= (1u << 20);
    }
};

void transfer(std::string_view input)
{
    for (const auto message : messages(input)) {
        (void)AfterglowProto::decodeTransfer(message);
        (void)AfterglowProto::transferCancelId(message);
    }
}

void dragAndDrop(std::string_view input)
{
    // Smaller limits than the application's, so that the fuzzer reaches
    // TooLarge. The rest is what DragDrop::handle() does with each command.
    AfterglowProto::DndAssembler assembler(4096, 512);
    for (const auto command : messages(input)) {
        // A message of one byte 1, 2 or 3 is no packet: it stands for what
        // DragDrop does to the assembler when the protocol ends between packets.
        if (command.size() == 1 && command[0] >= 1 && command[0] <= 3) {
            if (command[0] == 3) {
                // A terminal reset, or an abort whose gap ends with one.
                assembler.reset();
                continue;
            }
            // end(), and an abort: the message being put together is dropped.
            assembler.abandon();
            if (command[0] == 2) {
                // The abort lost OSC 72 packets: what follows is the rest of a message.
                assembler.discard(true);
            }
            continue;
        }
        (void)AfterglowProto::dndQueryReply(command);
        const auto [raw, payload] = AfterglowProto::splitDnd(command);
        const auto meta = AfterglowProto::parseDndMeta(raw);
        if (command.size() > AfterglowProto::DndMaxCommand || payload.size() > AfterglowProto::DndMaxPacket) {
            (void)AfterglowProto::dndType(raw);
            (void)assembler.type();
            assembler.discard(AfterglowProto::dndMore(raw));
            continue;
        }
        if (!meta) {
            // The packet's own m tells whether chunks follow it.
            assembler.discard(AfterglowProto::dndMore(raw));
            continue;
        }
        if (assembler.active()) {
            (void)AfterglowProto::dndReceiverType(assembler.type());
        }
        AfterglowProto::DndAssembler::Message message;
        switch (assembler.feed(*meta, payload, message)) {
        case AfterglowProto::DndAssembler::Result::Pending:
        case AfterglowProto::DndAssembler::Result::Dropped:
        case AfterglowProto::DndAssembler::Result::TooLarge:
            break;
        case AfterglowProto::DndAssembler::Result::Mismatch:
            // The protocol ends, and the rest of the message stays dropped.
            assembler.abandon();
            break;
        case AfterglowProto::DndAssembler::Result::Ready: {
            // The application decodes the payload of a whole message.
            const auto type = message.meta.find("t");
            if (type != message.meta.end() && AfterglowProto::dndDataType(type->second)) {
                (void)AfterglowProto::base64Decode(message.payload, AfterglowProto::Base64Check::StrictOrUnpadded);
            }
            break;
        }
        }
    }
}

void delta(std::string_view input)
{
    // Two bytes give the block size and the basis length; the rest arrives
    // in pieces cut at NUL bytes, as data messages arrive.
    if (input.size() < 2) {
        return;
    }
    Basis basis;
    basis.blockSize = 1 + std::uint8_t(input[0]) % 64;
    basis.bytes.assign(std::size_t(std::uint8_t(input[1])) * 4, 'b');
    AfterglowProto::DeltaApplier applier(basis.blockSize, basis.bytes.size(), 4096);
    std::string pending;
    for (const auto piece : messages(input.substr(2))) {
        pending.append(piece);
        const auto used = applier.apply(pending, basis);
        if (!used) {
            return;
        }
        if (*used > pending.size()) {
            __builtin_trap();
        }
        pending.erase(0, *used);
    }
    (void)applier.finished();
}

void signature(std::string_view input)
{
    // The signature that a receiver sends, and a file that is a copy of it,
    // so that blocks match.
    if (!AfterglowProto::signatureBlockSize(input, 4096)) {
        return;
    }
    (void)AfterglowProto::makeDelta(input, input.substr(0, 8192));
}

/// writeGraphics() as a terminal reads it: each command parses back to the
/// keys it was written from, and the chunks join to @p payload.
void checkWritten(const AfterglowProto::GraphicsCommand& command, std::string_view payload)
{
    using AfterglowProto::GraphicsCommand;
    const std::string written = AfterglowProto::writeGraphics(command, payload);
    const bool chunked = AfterglowProto::base64Encode(payload).size() > AfterglowProto::GraphicsChunk;
    std::string joined;
    for (std::size_t at = 0; at < written.size();) {
        const std::size_t end = written.find("\x1b\\", at);
        if (written.compare(at, 3, "\x1b_G") != 0 || end == std::string::npos) {
            __builtin_trap();
        }
        const std::string_view body = std::string_view(written).substr(at + 3, end - at - 3);
        const std::size_t semi = body.find(';');
        const GraphicsCommand read = GraphicsCommand::parse(body.substr(0, semi));
        const bool last = end + 2 == written.size();
        // The first command carries the keys. The others carry only m and q,
        // and a=f for a frame.
        GraphicsCommand expected = at == 0 ? command : GraphicsCommand{};
        if (at != 0 && command.action == 'f') {
            expected.action = 'f';
            expected.hasAction = true;
        }
        expected.more = chunked && !last ? 1 : 0;
        expected.quiet = command.quiet;
        if (read != expected) {
            __builtin_trap();
        }
        if (semi != std::string_view::npos) {
            joined.append(body.substr(semi + 1));
        }
        at = end + 2;
    }
    if (AfterglowProto::base64Decode(joined, AfterglowProto::Base64Check::Strict) != payload) {
        __builtin_trap();
    }
}

void graphics(std::string_view input)
{
    for (const auto message : messages(input)) {
        (void)AfterglowProto::GraphicsReply::parse(message);
        const std::size_t semi = message.find(';');
        const auto command = AfterglowProto::GraphicsCommand::parse(message.substr(0, semi));
        // keys() writes what parse() reads back. A flag that is not a letter
        // could end the command, so nothing is written for it.
        const std::string keys = command.keys();
        if (!command.flagsAreLetters()) {
            if (!keys.empty() || !AfterglowProto::writeGraphics(command, {}).empty()) {
                __builtin_trap();
            }
            continue;
        }
        if (AfterglowProto::GraphicsCommand::parse(keys) != command) {
            __builtin_trap();
        }
        checkWritten(command, semi == std::string_view::npos ? std::string_view{} : message.substr(semi + 1));
    }
}

void clipboard(std::string_view input)
{
    AfterglowProto::ClipboardAssembler assembler;
    for (const auto message : messages(input)) {
        (void)AfterglowProto::ClipboardReply::parse(message);
        (void)AfterglowProto::Osc52Message::parse(message);
        AfterglowProto::ClipboardRequest request;
        std::string reply;
        (void)assembler.feed(message, request, reply);
    }
}

void inflate(std::string_view input)
{
    // Byte 0 sets a limit of 0 to 255 KiB, as the image store passes the
    // declared image size. Byte 1 sets the piece size of the stepped
    // stream, as a compressed file transfer feeds it one data message at a time.
    if (input.size() < 2) {
        return;
    }
    const std::size_t limit = std::size_t(std::uint8_t(input[0])) << 10;
    const std::size_t piece = 1 + std::uint8_t(input[1]);
    const std::string_view stream = input.substr(2);
    std::string out;
    if (AfterglowProto::zlibInflate(stream, out, limit) && out.size() > limit) {
        __builtin_trap();
    }

    AfterglowProto::ZlibStream steps(AfterglowProto::ZlibStream::Mode::Inflate);
    char buffer[61];
    std::size_t total = 0;
    for (std::size_t at = 0; at < stream.size() && steps.valid(); at += piece) {
        steps.setInput(stream.substr(at, piece));
        auto result = AfterglowProto::ZlibStream::Result::Ok;
        while (result == AfterglowProto::ZlibStream::Result::Ok) {
            std::size_t produced = 0;
            result = steps.step(buffer, sizeof buffer, produced);
            if (produced > sizeof buffer) {
                __builtin_trap();
            }
            total += produced;
            // Space left over with no input left: zlib waits for the next piece.
            if (total > (1u << 20) || (produced < sizeof buffer && steps.inputLeft() == 0)) {
                break;
            }
        }
        if (result != AfterglowProto::ZlibStream::Result::Ok) {
            break;
        }
    }
}

void text(std::string_view input)
{
    for (const auto message : messages(input)) {
        (void)AfterglowProto::sanitizeId(message);
        (void)AfterglowProto::isCleanText(message);
        // What a client prints passes the check of clean text.
        if (!AfterglowProto::isCleanText(AfterglowProto::printable(message))) {
            __builtin_trap();
        }
        // A code point moves past one to four bytes; a bad sequence moves
        // nothing, and the caller steps over a byte.
        std::size_t at = 0;
        char32_t cp = 0;
        while (at < message.size()) {
            const std::size_t before = at;
            if (AfterglowProto::nextUtf8(message, at, cp)) {
                if (at <= before || at > before + 4 || cp > 0x10FFFF) {
                    __builtin_trap();
                }
            } else if (at != before) {
                __builtin_trap();
            } else {
                ++at;
            }
        }
    }
}

void json(std::string_view input)
{
    namespace Json = AfterglowProto::Json;
    // A document that reads writes as JSON that reads as the same value.
    if (const auto value = Json::parse(input)) {
        const auto again = Json::parse(Json::write(*value));
        if (!again || *again != *value) {
            __builtin_trap();
        }
    }
    // Any bytes as a string: JSON, of the size that its pieces add up to.
    const std::string written = Json::write(Json::Value(input));
    std::size_t size = 2;
    std::size_t at = 0;
    while (const std::size_t piece = Json::nextStringPiece(input, at)) {
        size += piece;
    }
    if (at != input.size() || size != written.size() || !Json::parse(written)) {
        __builtin_trap();
    }
    // A stdin part is a start of the input, under the limit once written,
    // and ends on a piece: never inside a UTF-8 sequence. The first byte
    // gives the limit, so that short input fills parts too.
    if (input.empty()) return;
    namespace Run = AfterglowProto::Run;
    const std::size_t limit = 8 + std::uint8_t(input[0]) % 64;
    const std::string_view data = input.substr(1);
    for (const bool eof : {false, true}) {
        const std::size_t part = Run::inputPart(data, eof, limit);
        // The walk gives the written size too: two quotes and the pieces.
        std::size_t end = 0;
        std::size_t bytes = 2;
        while (end < part) {
            const std::size_t piece = Json::nextStringPiece(data, end);
            if (!piece) break;
            bytes += piece;
        }
        if (part > data.size() || end != part || bytes > limit) {
            __builtin_trap();
        }
        // A part that ends before the data is full: its next piece does
        // not fit. The first piece always fits, so the end of the input
        // gives a part.
        std::size_t after = part;
        if ((part > 0 && part < data.size() && bytes + Json::nextStringPiece(data, after) <= limit)
            || (eof && part == 0 && !data.empty())) {
            __builtin_trap();
        }
        // No part yet: the pieces fit, but for the last three bytes, which
        // can be a sequence that more data completes.
        if (!eof && part == 0) {
            std::size_t position = 0;
            std::size_t total = 2;
            while (data.size() - position >= 4) total += Json::nextStringPiece(data, position);
            if (total > limit) {
                __builtin_trap();
            }
        }
        // A buffer that grows gives the part that one scan of all of it gives.
        Run::InputParts parts(limit);
        const std::size_t first = parts.next(data.substr(0, data.size() / 2), false);
        if ((first ? first : parts.next(data, eof)) != part) {
            __builtin_trap();
        }
    }
}

void color(std::string_view input)
{
    // A spec that fails leaves the colour as it was.
    std::uint8_t r = 1, g = 2, b = 3;
    if (!AfterglowProto::parseColorSpec(input, r, g, b)) {
        if (r != 1 || g != 2 || b != 3) {
            __builtin_trap();
        }
        return;
    }
    // One that reads gives a colour that reads back the same in the two
    // exact forms: 8-bit "#" digits and 2-digit rgb: components.
    char hex[8];
    char rgb[16];
    std::snprintf(hex, sizeof hex, "#%02x%02x%02x", r, g, b);
    std::snprintf(rgb, sizeof rgb, "rgb:%02x/%02x/%02x", r, g, b);
    for (const char* spec : {hex, rgb}) {
        std::uint8_t r2 = 0, g2 = 0, b2 = 0;
        if (!AfterglowProto::parseColorSpec(spec, r2, g2, b2) || r2 != r || g2 != g || b2 != b) {
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
    const std::string_view input(reinterpret_cast<const char*>(data + 1), size - 1);
    switch (data[0] % 10) {
    case 0:
        transfer(input);
        break;
    case 1:
        dragAndDrop(input);
        break;
    case 2:
        delta(input);
        break;
    case 3:
        signature(input);
        break;
    case 4:
        graphics(input);
        break;
    case 5:
        clipboard(input);
        break;
    case 6:
        inflate(input);
        break;
    case 7:
        text(input);
        break;
    case 8:
        json(input);
        break;
    case 9:
        color(input);
        break;
    }
    return 0;
}
