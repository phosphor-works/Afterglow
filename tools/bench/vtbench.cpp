// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Terminal throughput benchmark: feeds generated workloads through
// AfterglowVt::Terminal and reports MB/s. Run a Release build.

#include <AfterglowVt/Terminal.h>

#include <chrono>
#include <cstdio>
#include <random>
#include <string>

namespace {

struct NullHost : AfterglowVt::ITerminalHost
{
    void writeToPty(std::string_view) override { }
};

std::string asciiLines(std::size_t bytes)
{
    std::string out;
    std::mt19937 rng(1);
    while (out.size() < bytes) {
        const int len = 20 + int(rng() % 100);
        for (int i = 0; i < len; ++i) {
            out.push_back(char(' ' + rng() % 95));
        }
        out += "\r\n";
    }
    return out;
}

std::string colouredLines(std::size_t bytes)
{
    std::string out;
    std::mt19937 rng(2);
    while (out.size() < bytes) {
        for (int w = 0; w < 12; ++w) {
            out += "\x1b[38;5;" + std::to_string(rng() % 256) + "m";
            for (int i = 0; i < 6; ++i) {
                out.push_back(char('a' + rng() % 26));
            }
            out += "\x1b[0m ";
        }
        out += "\r\n";
    }
    return out;
}

std::string unicodeLines(std::size_t bytes)
{
    const std::string words[] = {"日本語", "中文字符", "한국어", "😀", "👨‍👩‍👧", "é", "ñ", "naïve", "ascii"};
    std::string out;
    std::mt19937 rng(3);
    while (out.size() < bytes) {
        for (int i = 0; i < 10; ++i) {
            out += words[rng() % 9] + " ";
        }
        out += "\r\n";
    }
    return out;
}

void run(const char* name, const std::string& data, std::size_t scrollback)
{
    NullHost host;
    AfterglowVt::TerminalOptions opts;
    opts.maxScrollback = scrollback;
    AfterglowVt::Terminal t(200, 60, host, opts);
    // Feed in pty-sized chunks, as the I/O thread does.
    constexpr std::size_t Chunk = 65536;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < data.size(); i += Chunk) {
        t.feed(std::string_view(data).substr(i, Chunk));
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%-22s %8.1f MB/s  (%zu MB in %.3f s)\n", name, data.size() / secs / 1e6, data.size() >> 20, secs);
}

} // namespace

int main()
{
    const std::size_t size = std::size_t(64) << 20;
    run("ascii", asciiLines(size), 10000);
    run("ascii, no scrollback", asciiLines(size), 0);
    run("sgr colours", colouredLines(size), 10000);
    run("unicode", unicodeLines(size), 10000);
    return 0;
}
