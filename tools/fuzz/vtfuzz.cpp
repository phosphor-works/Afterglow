// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libFuzzer target for the terminal core. The input is a byte stream fed to
// a Terminal, with occasional control records that resize it or scroll the
// viewport, so reflow and wide-character handling get exercised too.
//
//   clang++ -std=c++20 -g -O1 -fsanitize=fuzzer,address,undefined ...

#include <AfterglowVt/Search.h>
#include <AfterglowVt/Selection.h>
#include <AfterglowVt/Terminal.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {
struct Host : AfterglowVt::ITerminalHost
{
    void writeToPty(std::string_view) override { }
};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Host host;
    AfterglowVt::TerminalOptions opts;
    opts.maxScrollback = 64;
    AfterglowVt::Terminal t(20, 8, host, opts);
    t.resize(20, 8, 8, 16);

    // 0xFF introduces a control record: [0xFF, op, a, b].
    std::size_t start = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] != 0xFF || i + 3 >= size) {
            continue;
        }
        t.feed(std::string_view(reinterpret_cast<const char*>(data + start), i - start));
        const std::uint8_t op = data[i + 1];
        const int a = data[i + 2];
        const int b = data[i + 3];
        switch (op % 4) {
        case 0:
            t.resize(1 + a % 60, 1 + b % 30, 8, 16);
            break;
        case 1:
            t.scrollViewport(long(a) - 128);
            break;
        case 2: {
            const auto& s = t.screen();
            AfterglowVt::Selection sel;
            sel.anchor = {s.rowsDropped() + std::uint64_t(a % (s.totalRows() + 1)), a % s.cols()};
            sel.head = {s.rowsDropped() + std::uint64_t(b % (s.totalRows() + 1)), b % s.cols()};
            sel.mode = AfterglowVt::SelectionMode(op % 4);
            sel.layoutEpoch = s.layoutEpoch();
            sel.active = true;
            (void)sel.text(s);
            break;
        }
        case 3:
            (void)AfterglowVt::findAll(t.screen(), U"a");
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
