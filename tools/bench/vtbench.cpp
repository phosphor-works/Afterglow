// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Terminal throughput benchmark: feeds generated workloads through
// AfterglowVt::Terminal and reports MB/s. Run a Release build.
//
// With files (recorded output, such as kitten __benchmark__'s), replays
// each and reports throughput in CPU time, the best of three runs, which
// holds steady on a busy machine.
//
// With --reflow, fills 100,000 history rows with generated text, then
// times repeated width and height changes of the primary screen.
//
// With --stress, times inputs that make one code path do the most work for
// each byte: line feeds in a scroll region with many retained OSC 133
// regions, and extra-cursor sequences on a screen smaller than the cursor
// limit. Each case comes with a baseline that skips the expensive part.

#include <AfterglowVt/Terminal.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

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
    std::printf("%-22s %8.1f MB/s  (%.0f MB in %.3f s)\n", name, data.size() / secs / 1e6, data.size() / 1e6, secs);
}

double cpuSeconds()
{
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

bool replay(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(f)), {});
    if (data.empty()) {
        std::fprintf(stderr, "cannot read %s\n", path);
        return false;
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
    std::printf("%-26s %8.1f MB/s  (%.0f MB, %.3f s CPU)\n", name, data.size() / best / 1e6, data.size() / 1e6, best);
    return true;
}


// Text for the reflow workloads. Long lines are soft-wrapped at every tested
// width. Styled runs, hyperlinks, wide and combining characters exercise the
// side tables and the wide-character boundary rule.
std::string wrappedText(std::mt19937& rng, int len, bool rich)
{
    static const char* const Wide[] = {"日", "本", "語", "界", "한", "😀"};
    std::string out;
    int cells = 0;
    while (cells < len) {
        const unsigned pick = rich ? rng() % 100 : 99;
        if (pick < 4) {
            out += Wide[rng() % 6];
            cells += 2;
        } else if (pick < 7) {
            out += "e\xcc\x81"; // e + combining acute
            ++cells;
        } else if (pick < 9) {
            out += "\x1b[38;5;" + std::to_string(rng() % 256) + ";1m";
        } else if (pick < 11) {
            out += "\x1b[0m";
        } else if (pick == 11) {
            out += "\x1b]8;;https://example.org/" + std::to_string(rng() % 64) + "\x1b\\link\x1b]8;;\x1b\\";
            cells += 4;
        } else {
            out.push_back(rng() % 7 == 0 ? ' ' : char('a' + rng() % 26));
            ++cells;
        }
    }
    return out + "\x1b[0m\r\n";
}

struct ReflowCase
{
    const char* name;
    int shortPercent; // lines of 20-120 cells; the rest are long
    int longMin;
    int longMax;
    bool rich;
};

std::string reflowData(const ReflowCase& c, std::size_t rows, int cols)
{
    std::mt19937 rng(7);
    std::string out;
    std::size_t made = 0;
    while (made < rows) {
        const bool isShort = int(rng() % 100) < c.shortPercent;
        const int len = isShort ? 20 + int(rng() % 101) : c.longMin + int(rng() % unsigned(c.longMax - c.longMin + 1));
        out += wrappedText(rng, len, c.rich);
        made += std::size_t((len + cols - 1) / cols);
    }
    return out;
}

double percentile(std::vector<double> v, double p)
{
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, std::size_t(p * double(v.size() - 1) + 0.5))];
}

void reflowBench(const ReflowCase& c, int repeats)
{
    constexpr std::size_t History = 100000;
    // Each resize changes the size by 20 columns and 5 rows, between 140 and 200
    // columns. These are not the native benchmark's steps, which move the window
    // 16 by 8 pixels at a time across a 160 by 80 pixel range.
    const std::pair<int, int> sizes[] = {{180, 45}, {160, 50}, {140, 45}, {160, 50}, {180, 45}, {200, 50}};
    const std::string data = reflowData(c, History + 50, 200);
    std::vector<double> medians;
    std::vector<double> p95s;
    for (int round = 0; round < repeats; ++round) {
        NullHost host;
        AfterglowVt::TerminalOptions opts;
        opts.maxScrollback = History;
        AfterglowVt::Terminal t(200, 50, host, opts);
        t.resize(200, 50, 8, 16);
        constexpr std::size_t Chunk = 65536;
        for (std::size_t i = 0; i < data.size(); i += Chunk) {
            t.feed(std::string_view(data).substr(i, Chunk));
        }
        const std::size_t filled = t.screen().historySize();
        std::vector<double> ms;
        for (int pass = 0; pass < 4; ++pass) {
            for (const auto& [cols, rows] : sizes) {
                // CPU time, as for replays: steadier than wall time on a busy machine.
                const double start = cpuSeconds();
                t.resize(cols, rows, 8, 16);
                ms.push_back((cpuSeconds() - start) * 1000.0);
            }
        }
        medians.push_back(percentile(ms, 0.5));
        p95s.push_back(percentile(ms, 0.95));
        if (round == 0) {
            std::printf("%-24s history %zu rows filled, %zu after resizes, %.0f MB input\n", c.name, filled,
                        t.screen().historySize(), data.size() / 1e6);
        }
    }
    std::printf("%-24s resize CPU median %8.2f ms  p95 %8.2f ms  (medians of %d runs, 24 resizes each)\n", c.name,
                percentile(medians, 0.5), percentile(p95s, 0.5), repeats);
}

struct StressCase
{
    const char* name;
    const char* unit; ///< what one repetition is
    int cols;
    int rows;
    std::string setup; ///< fed before the clock starts
    std::string repetition;
    int repeats;
    /// The state the work leaves, checked after each run. Without it a case
    /// could time something cheaper and look fast: the parser drops a
    /// sequence over its parameter limit whole, and the regions cap is what
    /// makes a case hold 1024 of them.
    bool (*check)(const AfterglowVt::Terminal&);
    const char* expect; ///< what check() looks for, for the error message
};

bool noRegions(const AfterglowVt::Terminal& t)
{
    return t.semanticRegions().empty();
}

bool allRegions(const AfterglowVt::Terminal& t)
{
    return t.semanticRegions().size() == AfterglowVt::Terminal::MaxSemanticRegions;
}

bool everyCell(const AfterglowVt::Terminal& t)
{
    return t.extraCursors().size() == std::size_t(t.cols()) * std::size_t(t.rows());
}

/// Prompt, input and output regions, then blank lines: every region is in
/// the history, above any scroll region of the screen, as when a program
/// sets margins after a long shell session.
std::string shellHistory(int commands, int rows)
{
    std::string out;
    for (int i = 0; i < commands; ++i) {
        out += "\x1b]133;A\x07$ \x1b]133;B\x07ls\r\n\x1b]133;C\x07file " + std::to_string(i) + "\r\n\x1b]133;D;0\x07";
    }
    return out + std::string(std::size_t(rows), '\n');
}

/// One extra-cursor sequence (shape 1, a block) with @p groups.
std::string extraCursors(const std::vector<std::string>& groups)
{
    std::string out = "\x1b[>1";
    for (const auto& group : groups) {
        out += ';' + group;
    }
    return out + " q";
}

std::vector<StressCase> stressCases()
{
    std::vector<StressCase> cases;
    // apt's progress bar: margins over all rows but the last, a line feed at
    // the bottom margin for each line of output.
    const std::string margins = "\x1b[1;23r\x1b[23;1H";
    const std::string line = "Unpacking libexample (1.2.3-4) over (1.2.3-3) ...\r\n";
    cases.push_back({"region lf, no regions", "line feed", 80, 24, margins, line, 200000, noRegions, "no OSC 133 region"});
    cases.push_back({"region lf, 1024 regions", "line feed", 80, 24, shellHistory(400, 24) + margins, line, 200000,
                     allRegions, "1024 OSC 133 regions"});
    // 80x24 holds 1,920 cells, fewer than the 4,096 cursor limit, so no
    // sequence stops at the limit. The same group repeated is the cheap
    // case; distinct overlapping rectangles each visit their cells. The
    // shape takes one parameter, so the groups fill the parser's limit.
    cases.push_back({"cursors 80x24, one group repeated", "sequence", 80, 24, {},
                     extraCursors(std::vector<std::string>(std::size_t(AfterglowVt::Params::Max - 1), "4")), 2000,
                     everyCell, "a cursor in every cell"});
    std::vector<std::string> rects;
    for (int a = 0; a < 28; ++a) {
        for (int b = 0; b < 29; ++b) {
            rects.push_back("4:1:" + std::to_string(1 + a) + ":24:" + std::to_string(80 - b));
        }
    }
    cases.push_back({"cursors 80x24, distinct rects", "sequence", 80, 24, {}, extraCursors(rects), 200, everyCell,
                     "a cursor in every cell"});
    return cases;
}

bool stressBench(const StressCase& c)
{
    std::string data;
    data.reserve(c.repetition.size() * std::size_t(c.repeats));
    for (int i = 0; i < c.repeats; ++i) {
        data += c.repetition;
    }
    double best = 1e9;
    for (int round = 0; round < 3; ++round) {
        NullHost host;
        AfterglowVt::TerminalOptions opts;
        opts.maxScrollback = 10000;
        AfterglowVt::Terminal t(c.cols, c.rows, host, opts);
        t.resize(c.cols, c.rows, 8, 16);
        t.feed(c.setup);
        constexpr std::size_t Chunk = 65536;
        const double start = cpuSeconds();
        for (std::size_t i = 0; i < data.size(); i += Chunk) {
            t.feed(std::string_view(data).substr(i, Chunk));
        }
        best = std::min(best, cpuSeconds() - start);
        if (!c.check(t)) {
            std::fprintf(stderr, "%s: the run did not leave %s, so its time measures something else\n", c.name,
                         c.expect);
            return false;
        }
    }
    std::printf("%-36s %10.3f us per %-9s %8.1f MB/s  (%d repetitions of %zu bytes, %.3f s CPU)\n", c.name,
                best * 1e6 / c.repeats, c.unit, data.size() / best / 1e6, c.repeats, c.repetition.size(), best);
    return true;
}

int usage()
{
    std::fprintf(stderr, "usage: afterglow_vtbench\n"
                         "       afterglow_vtbench FILE...\n"
                         "       afterglow_vtbench --reflow [RUNS] [CASE]\n"
                         "       afterglow_vtbench --stress [CASE]\n"
                         "RUNS is an integer from 1 to 1000 (default 5).\n"
                         "CASE is a case-sensitive part of a case name, such as \"long rich\" or \"cursors\".\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--reflow") == 0) {
        if (argc > 4) return usage();
        int repeats = 5;
        if (argc > 2) {
            char* end = nullptr;
            const long value = std::strtol(argv[2], &end, 10);
            if (end == argv[2] || *end != '\0' || value < 1 || value > 1000) return usage();
            repeats = int(value);
        }
        const ReflowCase cases[] = {
            {"short ascii 20-120", 100, 0, 0, false},
            {"long ascii 300-3000", 0, 300, 3000, false},
            {"long rich 300-3000", 0, 300, 3000, true},
            {"mixed rich 50% long", 50, 300, 3000, true},
        };
        int ran = 0;
        for (const auto& c : cases) {
            if (argc > 3 && !std::strstr(c.name, argv[3])) {
                continue;
            }
            reflowBench(c, repeats);
            ++ran;
        }
        if (ran == 0) {
            std::fprintf(stderr, "no reflow case matches \"%s\"\n", argv[3]);
            return 1;
        }
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--stress") == 0) {
        if (argc > 3) return usage();
        int ran = 0;
        bool ok = true;
        for (const auto& c : stressCases()) {
            if (argc > 2 && !std::strstr(c.name, argv[2])) {
                continue;
            }
            ok = stressBench(c) && ok;
            ++ran;
        }
        if (ran == 0) {
            std::fprintf(stderr, "no stress case matches \"%s\"\n", argv[2]);
            return 1;
        }
        return ok ? 0 : 1;
    }
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (argv[i][0] == '-') return usage();
        }
        bool ok = true;
        for (int i = 1; i < argc; ++i) {
            ok = replay(argv[i]) && ok;
        }
        return ok ? 0 : 1;
    }
    const std::size_t size = std::size_t(64) << 20;
    run("ascii", asciiLines(size), 10000);
    run("ascii, no scrollback", asciiLines(size), 0);
    run("sgr colours", colouredLines(size), 10000);
    run("unicode", unicodeLines(size), 10000);
    return 0;
}
