// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Terminal throughput benchmark: feeds generated workloads through
// AfterglowVt::Terminal and reports MB/s. Run a Release build.
//
// With files (recorded output, such as kitten __benchmark__'s), replays
// each and reports throughput in CPU time, the best of three runs, which
// holds steady on a busy machine.

#include <AfterglowVt/Terminal.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cstdio>
#include <cstring>
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

double cpuSeconds()
{
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

void replay(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(f)), {});
    if (data.empty()) {
        std::fprintf(stderr, "cannot read %s\n", path);
        return;
    }
    double best = 1e9;
    for (int round = 0; round < 3; ++round) {
        NullHost host;
        AfterglowVt::Terminal t(160, 45, host);
        t.resize(160, 45, 8, 16);
        constexpr std::size_t Chunk = 65536;
        const double start = cpuSeconds();
        for (std::size_t i = 0; i < data.size(); i += Chunk) {
            t.feed(std::string_view(data).substr(i, Chunk));
        }
        best = std::min(best, cpuSeconds() - start);
    }
    const char* name = std::strrchr(path, '/') ? std::strrchr(path, '/') + 1 : path;
    std::printf("%-26s %8.1f MB/s  (%zu MB, %.3f s CPU)\n", name, data.size() / best / 1e6, data.size() >> 20, best);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            replay(argv[i]);
        }
        return 0;
    }
    const std::size_t size = std::size_t(64) << 20;
    run("ascii", asciiLines(size), 10000);
    run("ascii, no scrollback", asciiLines(size), 0);
    run("sgr colours", colouredLines(size), 10000);
    run("unicode", unicodeLines(size), 10000);
    return 0;
}
