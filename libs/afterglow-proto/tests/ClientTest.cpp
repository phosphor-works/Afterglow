// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The client side that the library adds for aglow: delta classes over file
// descriptors, the transfer path rules, whole transfer sessions against a
// fake terminal (and the listings that a hostile terminal could send), and
// the helpers of remote drag and drop trees.

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/DndTree.h>
#include <AfterglowProto/Fd.h>
#include <AfterglowProto/FdDelta.h>
#include <AfterglowProto/TransferClient.h>
#include <AfterglowProto/TransferPath.h>
#include <AfterglowProto/Zlib.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <pthread.h>
#include <set>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace AfterglowProto;

namespace {

std::string noise(std::size_t size, std::uint32_t seed)
{
    std::string out(size, '\0');
    for (char& c : out) {
        seed = seed * 1664525u + 1013904223u;
        c = char(seed >> 24);
    }
    return out;
}

struct TempDir
{
    std::filesystem::path path;
    TempDir()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "aglow-proto-XXXXXX").string();
        // gtest reports the exception as the test's failure.
        if (!::mkdtemp(pattern.data())) {
            throw std::runtime_error("mkdtemp failed: " + std::string(std::strerror(errno)));
        }
        path = pattern;
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::string write(const std::string& name, const std::string& bytes) const
    {
        const auto file = path / name;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
        return file.string();
    }
};

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// The decoded messages of an output stream, in order.
std::vector<TransferFields> messages(const std::string& stream)
{
    std::vector<TransferFields> out;
    std::size_t at = 0;
    while ((at = stream.find("\x1b]5113;", at)) != std::string::npos) {
        const auto end = stream.find("\x1b\\", at);
        const auto decoded = decodeTransfer(std::string_view(stream).substr(at + 7, end - at - 7));
        EXPECT_TRUE(decoded);
        if (decoded) {
            out.push_back(*decoded);
        }
        at = end;
    }
    return out;
}

std::string reply(const std::string& id, std::vector<std::pair<std::string, std::string>> fields)
{
    TransferFields f { { "id", id } };
    for (auto& [key, value] : fields) {
        f[key] = value;
    }
    const auto encoded = encodeTransfer(f);
    return encoded.substr(7, encoded.size() - 9);
}

int g_testWake = -1;

extern "C" void wakeOnSignal(int)
{
    const char byte = 1;
    [[maybe_unused]] const ssize_t ignored = ::write(g_testWake, &byte, 1);
}

TEST(Client, AWakeEndsAWriteThatASignalCutShort)
{
    // A pipe of one page takes half of the bytes, and the write blocks. The
    // signal then makes write(2) return the half that went, not EINTR.
    int pipeFds[2];
    int wakeFds[2];
    ASSERT_EQ(::pipe2(pipeFds, O_CLOEXEC), 0);
    ASSERT_EQ(::pipe2(wakeFds, O_CLOEXEC | O_NONBLOCK), 0);
    ASSERT_GT(::fcntl(pipeFds[1], F_SETPIPE_SZ, 4096), 0);
    const auto room = std::size_t(::fcntl(pipeFds[1], F_GETPIPE_SZ));
    g_testWake = wakeFds[1];
    struct sigaction action { };
    action.sa_handler = wakeOnSignal;
    sigemptyset(&action.sa_mask);
    struct sigaction old { };
    ::sigaction(SIGUSR1, &action, &old);
    const pthread_t writer = ::pthread_self();
    std::atomic<bool> done { false };
    std::thread helper([&] {
        // The pipe is full only while the writer is inside its write(2).
        int queued = 0;
        while (!done && (::ioctl(pipeFds[0], FIONREAD, &queued) != 0 || std::size_t(queued) < room)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ::pthread_kill(writer, SIGUSR1);
        // A write that missed the wake would block for good: let it end.
        for (int i = 0; i < 200 && !done; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::string sink(room * 2, '\0');
        while (!done && ::read(pipeFds[0], sink.data(), sink.size()) > 0) {
        }
    });
    const bool wrote = writeAll(pipeFds[1], std::string(room * 2, 'x'), wakeFds[0]);
    const int failure = errno;
    done = true;
    ::close(pipeFds[1]);
    helper.join();
    ::sigaction(SIGUSR1, &old, nullptr);
    g_testWake = -1;
    for (const int fd : { pipeFds[0], wakeFds[0], wakeFds[1] }) {
        ::close(fd);
    }
    EXPECT_FALSE(wrote);
    EXPECT_EQ(failure, EINTR);
}

TEST(Client, FdSourceStopsAtItsLimit)
{
    TempDir dir;
    const auto path = dir.write("source", noise(10000, 1));
    const int fd = ::open(path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    FdSource source(fd, 6000);
    std::string out;
    while (!source.atEnd()) {
        ASSERT_TRUE(source.read(out, 4096));
    }
    ::close(fd);
    EXPECT_EQ(out, noise(10000, 1).substr(0, 6000));
    EXPECT_EQ(source.consumed(), 6000u);
}

TEST(Client, DeltaOverDescriptorsRebuildsTheFile)
{
    TempDir dir;
    const auto basis = noise(300000, 2);
    auto changed = basis;
    changed.insert(1000, "inserted bytes");
    changed.replace(150000, 5000, noise(5000, 3));
    changed += "a tail that is new";
    const auto basisPath = dir.write("basis", basis);
    const auto newPath = dir.write("new", changed);
    const int newFd = ::open(newPath.c_str(), O_RDONLY);
    ASSERT_GE(newFd, 0);
    FdSource source(newFd);
    DeltaGenerator generator(makeSignature(basis));
    std::string delta;
    while (!generator.done()) {
        ASSERT_TRUE(generator.produce(source, delta, 65536, 1 << 20));
    }
    ::close(newFd);
    EXPECT_LT(delta.size(), changed.size() / 4);

    const int basisFd = ::open(basisPath.c_str(), O_RDONLY);
    ASSERT_GE(basisFd, 0);
    const auto outPath = (dir.path / "out").string();
    const int outFd = ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(outFd, 0);
    const auto block = deltaBlockSize(basis.size());
    FdDeltaTarget target(basisFd, outFd, block);
    DeltaApplier applier(block, basis.size(), 1 << 24);
    // Fed in uneven pieces, as data messages arrive.
    std::string pending;
    for (std::size_t at = 0; at < delta.size(); at += 777) {
        pending += delta.substr(at, 777);
        const auto used = applier.apply(pending, target);
        ASSERT_TRUE(used);
        pending.erase(0, *used);
    }
    ::close(basisFd);
    ::close(outFd);
    EXPECT_TRUE(applier.finished());
    EXPECT_EQ(readFile(outPath), changed);
    EXPECT_EQ(target.written(), changed.size());
}

TEST(Client, DeltaTargetStopsAtItsLimit)
{
    TempDir dir;
    const auto basisPath = dir.write("basis", std::string(1000, 'a'));
    const int basisFd = ::open(basisPath.c_str(), O_RDONLY);
    ASSERT_GE(basisFd, 0);
    const auto outPath = (dir.path / "out").string();
    const int outFd = ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT_GE(outFd, 0);
    FdDeltaTarget target(basisFd, outFd, 64, 100);
    std::string block;
    ASSERT_TRUE(target.readBlock(0, block));
    EXPECT_TRUE(target.write(block));
    errno = 0;
    EXPECT_FALSE(target.write(block));
    // The limit says why it stopped: a receiver tells it from a write error.
    EXPECT_EQ(errno, EFBIG);
    // A block that starts at a valid offset and ends past the largest one.
    FdDeltaTarget wide(basisFd, outFd, 100);
    EXPECT_FALSE(wide.readBlock(std::uint64_t(std::numeric_limits<off_t>::max()) / 100, block));
    EXPECT_EQ(errno, EINVAL);
    ::close(basisFd);
    ::close(outFd);
    EXPECT_EQ(target.written(), 64u);
}

TEST(Client, PathsFollowGo)
{
    using namespace TransferPath;
    EXPECT_EQ(clean("a//b/./c/.."), "a/b");
    EXPECT_EQ(clean("/../x"), "/x");
    EXPECT_EQ(clean("../x"), "../x");
    EXPECT_EQ(clean(""), ".");
    EXPECT_EQ(join("/dest/", "name"), "/dest/name");
    EXPECT_EQ(join("~/a", "b"), "~/a/b");
    EXPECT_EQ(baseName("/a/b/"), "b");
    EXPECT_EQ(baseName("file"), "file");
    EXPECT_EQ(dirName("/a/b"), "/a");
    EXPECT_EQ(dirName("b"), ".");
    EXPECT_EQ(relative("/a/b", "/a/c/d"), "../c/d");
    EXPECT_EQ(relative("/a", "/a"), ".");
    EXPECT_EQ(absolute("~/x", "/home/u", "/w"), "/home/u/x");
    EXPECT_EQ(absolute("x/y", "/home/u", "/w"), "/w/x/y");
    EXPECT_EQ(absolute("/abs/", "/home/u", "/w"), "/abs");
    // ~ is a root that ".." does not remove: only the other computer knows it.
    EXPECT_EQ(clean("~/../srv"), "~/../srv");
    EXPECT_EQ(clean("~//a/./b/.."), "~/a");
    EXPECT_EQ(clean("~/."), "~");
    EXPECT_EQ(join("~/../srv/", "name"), "~/../srv/name");
    EXPECT_EQ(absolute("~/../srv", "/home/u", "/w"), "/home/srv");
}

TransferClientOptions optionsIn(const TempDir& dir)
{
    TransferClientOptions options;
    options.home = dir.path.string();
    options.cwd = dir.path.string();
    return options;
}

using Pairs = std::vector<std::pair<std::string, std::string>>;

// A terminal that answers a session as the test tells it to, including with
// listings that a hostile terminal could send.
struct FakeTerminal
{
    FakeTerminal(TransferClient& c, std::string h)
        : client(c)
        , home(std::move(h))
    {
    }
    TransferClient& client;
    std::string home;
    bool answerRequests = true;
    std::vector<Pairs> listing;
    std::map<std::string, std::string> contents; // data for the requests, by remote path
    std::map<std::string, std::string> started; // extra STARTED fields for a send, by remote path
    std::set<std::string> refused; // a send's files that get EPERM instead of STARTED
    std::set<std::string> silent; // a send's files whose end gets no answer, as a link that waits for its target
    std::map<std::string, std::string> paths; // a send's remote paths, by file id
    std::vector<TransferFields> sent;
    bool finished = false;

    void say(Pairs fields) { client.receive(reply(client.id(), std::move(fields))); }
    // Data in packets of the size that the protocol allows, as a terminal sends it.
    void sendData(const std::string& fid, const std::string& data)
    {
        std::size_t at = 0;
        do {
            const auto take = std::min(data.size() - at, TransferMaxData);
            const bool last = at + take == data.size();
            say({ { "ac", last ? "end_data" : "data" }, { "fid", fid }, { "d", data.substr(at, take) } });
            at += take;
        } while (at < data.size());
    }
    // One round: what the client has to send, and the answers to it.
    void step()
    {
        std::string out;
        client.pull(out, 1 << 20);
        for (const auto& m : messages(out)) {
            sent.push_back(m);
            const auto& action = m.at("ac");
            if (action == "receive" || action == "send") {
                specs = action == "receive" ? std::stoul(m.at("sz")) : 0;
                if (!specs) {
                    say({ { "ac", "status" }, { "st", "OK" } });
                }
            } else if (action == "cancel") {
                say({ { "ac", "status" }, { "st", "CANCELED" } });
            } else if (action == "finish") {
                finished = true;
            } else if (action == "file" && specs > 0) {
                if (--specs == 0) {
                    say({ { "ac", "status" }, { "st", "OK" } });
                    for (auto entry : listing) {
                        entry.emplace_back("ac", "file");
                        say(entry);
                    }
                    say({ { "ac", "status" }, { "st", "OK" }, { "n", home } });
                }
            } else if (action == "file" && m.contains("ft")) {
                const auto& name = m.at("n");
                paths[m.at("fid")] = name;
                Pairs answer { { "ac", "status" }, { "fid", m.at("fid") },
                    { "st", refused.contains(name) ? "EPERM:refused" : m.at("ft") == "directory" ? "OK" : "STARTED" } };
                if (started.contains(name)) {
                    answer.emplace_back("tt", started.at(name));
                }
                say(answer);
            } else if (action == "end_data" && paths.contains(m.at("fid"))) {
                if (!silent.contains(paths.at(m.at("fid")))) {
                    say({ { "ac", "status" }, { "fid", m.at("fid") }, { "st", "OK" } });
                }
            } else if (action == "file" && answerRequests) {
                sendData(m.at("fid"), contents.contains(m.at("n")) ? contents.at(m.at("n")) : std::string());
            }
        }
    }
    // Runs the session until the client stops or @p rounds pass.
    void run(int rounds = 200)
    {
        for (int round = 0; round < rounds && !client.over(); ++round) {
            step();
        }
    }
    std::size_t specs = 0;
};

TEST(Client, SenderAsksFirstWithThePasswordHash)
{
    TempDir dir;
    dir.write("file", "data");
    auto options = optionsIn(dir);
    options.password = "secret";
    std::string error;
    auto sender = makeTransferSender(options, { "file", "/remote/dest" }, error);
    ASSERT_TRUE(sender) << error;
    std::string out;
    sender->pull(out, 65536);
    const auto sent = messages(out);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].at("ac"), "send");
    EXPECT_EQ(sent[0].at("pw"), transferPasswordHash(sender->id(), "secret"));
    EXPECT_EQ(sender->state(), TransferClient::State::Asking);
}

// The metadata of every file that a sender announces, by remote path.
std::map<std::string, TransferFields> announced(TransferClient& sender)
{
    std::string out;
    sender.pull(out, 65536);
    sender.receive(reply(sender.id(), { { "ac", "status" }, { "st", "OK" } }));
    out.clear();
    sender.pull(out, 1 << 20);
    std::map<std::string, TransferFields> byName;
    for (const auto& m : messages(out)) {
        if (m.at("ac") == "file") {
            byName[m.at("n")] = m;
        }
    }
    return byName;
}

TEST(Client, SenderMapsATreeAsKittenDoes)
{
    TempDir dir;
    dir.write("tree/a.txt", noise(5000, 4));
    dir.write("tree/sub/b", "b");
    ASSERT_EQ(::symlink("a.txt", (dir.path / "tree/link").c_str()), 0);
    ASSERT_EQ(::link((dir.path / "tree/a.txt").c_str(), (dir.path / "tree/hard").c_str()), 0);
    std::string error;
    auto sender = makeTransferSender(optionsIn(dir), { "tree", "/dest/" }, error);
    ASSERT_TRUE(sender) << error;
    const auto byName = announced(*sender);
    ASSERT_EQ(byName.size(), 6u);
    EXPECT_EQ(byName.at("/dest/tree/sub").at("ft"), "directory");
    EXPECT_EQ(byName.at("/dest/tree").at("ft"), "directory");
    EXPECT_EQ(byName.at("/dest/tree/a.txt").at("zip"), "zlib");
    EXPECT_EQ(byName.at("/dest/tree/a.txt").at("sz"), "5000");
    EXPECT_EQ(byName.at("/dest/tree/link").at("ft"), "symlink");
    // Sorted by name, "a.txt" comes before "hard": the second name of the inode is the link.
    EXPECT_EQ(byName.at("/dest/tree/hard").at("ft"), "link");
    EXPECT_EQ(byName.at("/dest/tree/sub/b").at("ft"), "regular");
    EXPECT_EQ(sender->items()[0].local, (dir.path / "tree").string());
}

TEST(Client, ADirectorySentToAPathWithoutASlashBecomesThatPath)
{
    TempDir dir;
    dir.write("tree/a", "a");
    std::string error;
    auto sender = makeTransferSender(optionsIn(dir), { "tree", "/dest" }, error);
    ASSERT_TRUE(sender) << error;
    const auto byName = announced(*sender);
    // As cp -r: the children go into the directory's own path.
    EXPECT_TRUE(byName.contains("/dest"));
    EXPECT_TRUE(byName.contains("/dest/a"));
    EXPECT_FALSE(byName.contains("/dest/tree/a"));
}

TEST(Client, AMirrorOfTheHomeDirectoryGoesToTheOtherHome)
{
    TempDir dir;
    dir.write("file", "x");
    auto options = optionsIn(dir);
    options.mode = TransferClientOptions::Mode::Mirror;
    std::string error;
    auto sender = makeTransferSender(options, { "~" }, error);
    ASSERT_TRUE(sender) << error;
    const auto byName = announced(*sender);
    EXPECT_TRUE(byName.contains("~"));
    EXPECT_TRUE(byName.contains("~/file"));
}

TEST(Client, CompressionSkipsCompressedFormats)
{
    TempDir dir;
    for (const char* name : { "notes.txt", "Makefile", "photo.JPG", "archive.tar.gz", "song.flac" }) {
        dir.write(std::string("files/") + name, std::string(5000, 'a'));
    }
    dir.write("files/small.txt", "small");
    using C = TransferClientOptions::Compress;
    const auto zipped = [&](C strategy) {
        auto options = optionsIn(dir);
        options.compress = strategy;
        std::string error;
        auto sender = makeTransferSender(options, { "files", "/d/" }, error);
        std::set<std::string> out;
        for (const auto& [name, m] : announced(*sender)) {
            if (m.contains("zip")) {
                out.insert(TransferPath::baseName(name));
            }
        }
        return out;
    };
    EXPECT_EQ(zipped(C::Auto), (std::set<std::string> { "Makefile", "notes.txt" }));
    // A small file goes whole under every strategy.
    EXPECT_EQ(zipped(C::Always), (std::set<std::string> { "Makefile", "archive.tar.gz", "notes.txt", "photo.JPG", "song.flac" }));
    EXPECT_TRUE(zipped(C::Never).empty());
}

TEST(Client, EmptyPathsAndRelativeDirectoriesAreRefused)
{
    TempDir dir;
    dir.write("file", "x");
    std::string error;
    EXPECT_FALSE(makeTransferSender(optionsIn(dir), { "file", "" }, error));
    EXPECT_FALSE(makeTransferSender(optionsIn(dir), { "", "/dest" }, error));
    EXPECT_FALSE(makeTransferReceiver(optionsIn(dir), { "/r/a", "" }, error));
    auto options = optionsIn(dir);
    options.home = "relative";
    EXPECT_FALSE(makeTransferReceiver(options, { "/r/a", "x" }, error));
}

TEST(Client, ReceiverListsItsSpecsAfterTheStart)
{
    TempDir dir;
    std::string error;
    auto receiver = makeTransferReceiver(optionsIn(dir), { "/remote/a", "~/b", "local" }, error);
    ASSERT_TRUE(receiver) << error;
    std::string out;
    receiver->pull(out, 65536);
    const auto sent = messages(out);
    ASSERT_EQ(sent.size(), 3u);
    EXPECT_EQ(sent[0].at("ac"), "receive");
    EXPECT_EQ(sent[0].at("sz"), "2");
    EXPECT_EQ(sent[1].at("n"), "/remote/a");
    EXPECT_EQ(sent[1].at("fid"), "0");
    EXPECT_EQ(sent[2].at("n"), "~/b");
    receiver->receive(reply(receiver->id(), { { "ac", "status" }, { "st", "EPERM:refused" } }));
    EXPECT_EQ(receiver->state(), TransferClient::State::Failed);
}

TEST(Client, CancelWaitsForTheTerminal)
{
    TempDir dir;
    dir.write("file", "data");
    std::string error;
    auto sender = makeTransferSender(optionsIn(dir), { "file", "dest" }, error);
    ASSERT_TRUE(sender);
    sender->cancel();
    std::string out;
    sender->pull(out, 65536);
    EXPECT_EQ(messages(out).back().at("ac"), "cancel");
    EXPECT_EQ(sender->state(), TransferClient::State::Canceling);
    EXPECT_FALSE(sender->over());
    // A file's CANCELED is not the session's.
    sender->receive(reply(sender->id(), { { "ac", "status" }, { "fid", "1" }, { "st", "CANCELED" } }));
    EXPECT_FALSE(sender->over());
    sender->receive(reply(sender->id(), { { "ac", "status" }, { "st", "CANCELED" } }));
    EXPECT_TRUE(sender->over());
}

TEST(Client, ACanceledAfterTheFinishLeavesTheSessionDone)
{
    TempDir dir;
    dir.write("file", "data");
    std::string error;
    auto sender = makeTransferSender(optionsIn(dir), { "file", "/dest" }, error);
    ASSERT_TRUE(sender) << error;
    FakeTerminal terminal { *sender, "/home/r" };
    terminal.run();
    ASSERT_EQ(sender->state(), TransferClient::State::Done) << sender->error();
    EXPECT_TRUE(terminal.finished);
    sender->receive(reply(sender->id(), { { "ac", "status" }, { "st", "CANCELED" } }));
    EXPECT_EQ(sender->state(), TransferClient::State::Done);
}

TEST(Client, SenderFinishesWhenTheTargetOfALinkFails)
{
    TempDir dir;
    dir.write("tree/b", "target");
    ASSERT_EQ(::symlink("b", (dir.path / "tree/a").c_str()), 0);
    std::string error;
    auto sender = makeTransferSender(optionsIn(dir), { "tree", "/d/" }, error);
    ASSERT_TRUE(sender) << error;
    FakeTerminal terminal { *sender, "/home/r" };
    // Afterglow holds a link whose target it never got, without an answer.
    terminal.refused = { "/d/tree/b" };
    terminal.silent = { "/d/tree/a" };
    terminal.run();
    EXPECT_EQ(sender->state(), TransferClient::State::Done) << sender->error();
    EXPECT_TRUE(terminal.finished);
    for (const auto& item : sender->items()) {
        if (item.type == TransferItem::Type::Symlink) {
            EXPECT_FALSE(item.error.empty());
        }
    }
}

TEST(Client, ALinkWaitsForTheMetadataOfItsTarget)
{
    TempDir dir;
    for (int i = 0; i < 40; ++i) {
        dir.write("tree/f" + std::to_string(100 + i), "x");
    }
    dir.write("tree/z", "target");
    // "a" sorts first, its target last: 40 files wait for the cap on open files between them.
    ASSERT_EQ(::symlink("z", (dir.path / "tree/a").c_str()), 0);
    std::string error;
    auto sender = makeTransferSender(optionsIn(dir), { "tree", "/d/" }, error);
    ASSERT_TRUE(sender) << error;
    FakeTerminal terminal { *sender, "/home/r" };
    terminal.run();
    ASSERT_EQ(sender->state(), TransferClient::State::Done) << sender->error();
    std::string linkFid;
    std::size_t targetAt = SIZE_MAX;
    std::size_t linkEnd = SIZE_MAX;
    for (std::size_t i = 0; i < terminal.sent.size(); ++i) {
        const auto& m = terminal.sent[i];
        if (m.at("ac") == "file" && m.at("n") == "/d/tree/a") {
            linkFid = m.at("fid");
        }
        if (m.at("ac") == "file" && m.at("n") == "/d/tree/z") {
            targetAt = i;
        }
        if (m.at("ac") == "end_data" && m.at("fid") == linkFid) {
            linkEnd = i;
        }
    }
    ASSERT_NE(targetAt, SIZE_MAX);
    ASSERT_NE(linkEnd, SIZE_MAX);
    EXPECT_GT(linkEnd, targetAt);
}

TEST(Client, AFileThatShrankAfterTheScanFails)
{
    TempDir dir;
    const auto path = dir.write("file", noise(10000, 9));
    std::string error;
    auto sender = makeTransferSender(optionsIn(dir), { "file", "/d/file" }, error);
    ASSERT_TRUE(sender) << error;
    std::filesystem::resize_file(path, 100);
    FakeTerminal terminal { *sender, "/home/r" };
    terminal.run();
    ASSERT_EQ(sender->items().size(), 1u);
    EXPECT_NE(sender->items()[0].error.find("shrank"), std::string::npos) << sender->items()[0].error;
}

TEST(Client, ADeltaOfAnUnchangedFileReadsInSteps)
{
    TempDir dir;
    const auto bytes = noise(40 << 20, 10);
    dir.write("big", bytes);
    auto options = optionsIn(dir);
    options.deltas = true;
    std::string error;
    auto sender = makeTransferSender(options, { "big", "/d/big" }, error);
    ASSERT_TRUE(sender) << error;
    const auto byName = announced(*sender);
    ASSERT_EQ(byName.at("/d/big").at("tt"), "rsync");
    const auto fid = byName.at("/d/big").at("fid");
    sender->receive(reply(sender->id(), { { "ac", "status" }, { "fid", fid }, { "st", "STARTED" }, { "tt", "rsync" } }));
    const auto signature = makeSignature(bytes);
    for (std::size_t at = 0; at < signature.size(); at += TransferMaxData) {
        const bool last = at + TransferMaxData >= signature.size();
        sender->receive(reply(sender->id(), { { "ac", last ? "end_data" : "data" }, { "fid", fid },
            { "d", signature.substr(at, TransferMaxData) } }));
    }
    // The delta is a few copy operations: the output budget would not stop the reads.
    std::string out;
    sender->pull(out, 1 << 20);
    EXPECT_TRUE(sender->hasWork());
    int pulls = 1;
    bool ended = false;
    while (!ended && pulls < 1000) {
        for (const auto& m : messages(out)) {
            ended = ended || m.at("ac") == "end_data";
        }
        out.clear();
        sender->pull(out, 1 << 20);
        ++pulls;
    }
    EXPECT_TRUE(ended);
    EXPECT_GT(pulls, 2);
}

TEST(Client, TheSignaturesOfASessionStopAtTheirMemory)
{
    // Each signature is below the limit of one file. Together they pass the session's.
    TempDir dir;
    dir.write("a", noise(10000, 12));
    dir.write("b", noise(10000, 13));
    auto options = optionsIn(dir);
    options.deltas = true;
    options.signatureMemory = 6000;
    std::string error;
    auto sender = makeTransferSender(options, { "a", "b", "/d/" }, error);
    ASSERT_TRUE(sender) << error;
    const auto byName = announced(*sender);
    const std::string part(TransferMaxData, 's');
    for (const auto* name : { "/d/a", "/d/b" }) {
        ASSERT_EQ(byName.at(name).at("tt"), "rsync");
        const auto fid = byName.at(name).at("fid");
        sender->receive(reply(sender->id(), { { "ac", "status" }, { "fid", fid }, { "st", "STARTED" }, { "tt", "rsync" } }));
        EXPECT_EQ(sender->state(), TransferClient::State::Running);
        sender->receive(reply(sender->id(), { { "ac", "data" }, { "fid", fid }, { "d", part } }));
    }
    EXPECT_EQ(sender->state(), TransferClient::State::Canceling);
    EXPECT_EQ(sender->error(), "The terminal sent more signatures than this side can hold");
}

TEST(Client, AReceiverSignsALargeFileInSteps)
{
    // The signature of a file that is there already is small output for the
    // bytes that it reads: the output budget alone would read all 40 MiB at once.
    TempDir dir;
    const std::size_t size = 40 << 20;
    dir.write("f", std::string(size, 'a'));
    auto options = optionsIn(dir);
    options.deltas = true;
    std::string error;
    auto client = makeTransferReceiver(options, { "/r/f", (dir.path / "f").string() }, error);
    ASSERT_TRUE(client) << error;
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = { { { "fid", "0" }, { "n", "/r/f" }, { "st", "1" }, { "sz", std::to_string(size) } } };
    terminal.answerRequests = false;
    const auto sentSince = [&](std::size_t from, const std::string& action) {
        return std::any_of(terminal.sent.begin() + std::ptrdiff_t(from), terminal.sent.end(),
                           [&](const TransferFields& m) { return m.at("ac") == action; });
    };
    std::size_t from = 0;
    bool requested = false;
    for (int round = 0; round < 20 && !requested; ++round) {
        from = terminal.sent.size();
        terminal.step();
        requested = std::any_of(terminal.sent.begin() + std::ptrdiff_t(from), terminal.sent.end(), [](const TransferFields& m) {
            return m.at("ac") == "file" && m.contains("tt") && m.at("tt") == "rsync";
        });
    }
    ASSERT_TRUE(requested);
    EXPECT_TRUE(client->hasWork());
    EXPECT_FALSE(sentSince(from, "end_data"));
    int pulls = 1;
    while (client->hasWork() && pulls < 100) {
        terminal.step();
        ++pulls;
    }
    EXPECT_TRUE(sentSince(0, "end_data"));
    EXPECT_GT(pulls, 2);
}

// A delta of a literal, one range that copies all of @p basis, a literal
// and the hash of the file that it makes.
std::string rangeDelta(const std::string& basis, const std::string& head, const std::string& tail)
{
    const auto literal = [](const std::string& bytes) {
        std::string op(1, '\x01');
        for (int byte = 0; byte < 4; ++byte) {
            op += char(std::uint8_t(bytes.size() >> (8 * byte)));
        }
        return op + bytes;
    };
    const auto block = deltaBlockSize(basis.size());
    const auto countLess1 = std::uint32_t((basis.size() + block - 1) / block - 1);
    std::string range(13, '\0');
    range[0] = 3;
    for (int byte = 0; byte < 4; ++byte) {
        range[std::size_t(9 + byte)] = char(std::uint8_t(countLess1 >> (8 * byte)));
    }
    const auto digest = fileHash(head + basis + tail);
    return literal(head) + range + literal(tail) + std::string("\x02\x10\x00", 3)
        + std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

// A receiver whose basis is @p size bytes gets a delta that copies all of
// it with one range: the copy goes in steps, and @p cancel stops it after
// the first one.
void receiveLargeDelta(TransferClientOptions::Compress compress, bool cancel)
{
    TempDir dir;
    const std::size_t size = 40 << 20;
    const auto basis = noise(size, 21);
    const std::string changed = "head" + basis + "tail";
    const auto path = dir.write("f", basis);
    auto options = optionsIn(dir);
    options.deltas = true;
    options.compress = compress;
    std::string error;
    auto client = makeTransferReceiver(options, { "/r/f", path }, error);
    ASSERT_TRUE(client) << error;
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = { { { "fid", "0" }, { "n", "/r/f" }, { "st", "1" }, { "sz", std::to_string(changed.size()) } } };
    terminal.answerRequests = false;
    // The request, then the signature in steps.
    std::string fid;
    bool signatureSent = false;
    for (int round = 0; round < 100 && !signatureSent; ++round) {
        terminal.step();
        for (const auto& m : terminal.sent) {
            if (m.at("ac") == "file" && m.contains("tt") && m.at("tt") == "rsync") {
                fid = m.at("fid");
            }
            signatureSent = signatureSent || (m.at("ac") == "end_data" && m.at("fid") == fid);
        }
    }
    ASSERT_TRUE(signatureSent);
    const auto delta = rangeDelta(basis, "head", "tail");
    terminal.sendData(fid, compress == TransferClientOptions::Compress::Always ? zlibCompress(delta) : delta);
    // One receive() copied one step, not the whole basis.
    EXPECT_TRUE(client->hasWork());
    ASSERT_EQ(client->items().size(), 1u);
    EXPECT_FALSE(client->items()[0].finished);
    EXPECT_GT(client->items()[0].done, 0u);
    EXPECT_LT(client->items()[0].done, size / 2);
    if (cancel) {
        client->cancel("Canceled by an interrupt");
        terminal.run();
        EXPECT_EQ(client->state(), TransferClient::State::Canceled);
        EXPECT_LT(client->items()[0].done, size / 2);
        // Not EXPECT_EQ: gtest would print a line diff of 40 MiB of noise.
        EXPECT_TRUE(readFile(path) == basis);
        client.reset();
        // No temporary file is left next to the destination.
        EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir.path), std::filesystem::directory_iterator()), 1);
        return;
    }
    int pulls = 0;
    while (client->hasWork() && pulls < 100) {
        terminal.step();
        ++pulls;
    }
    EXPECT_GE(pulls, 2);
    terminal.run();
    EXPECT_EQ(client->state(), TransferClient::State::Done);
    EXPECT_TRUE(terminal.finished);
    EXPECT_TRUE(client->items()[0].error.empty()) << client->items()[0].error;
    EXPECT_TRUE(readFile(path) == changed);
}

TEST(Client, AReceiverAppliesALargeDeltaInSteps)
{
    receiveLargeDelta(TransferClientOptions::Compress::Never, false);
}

TEST(Client, AReceiverInflatesALargeDeltaInSteps)
{
    receiveLargeDelta(TransferClientOptions::Compress::Always, false);
}

TEST(Client, AReceiverStopsBetweenTheStepsOfADelta)
{
    receiveLargeDelta(TransferClientOptions::Compress::Never, true);
}

struct Hostile : testing::Test
{
    TempDir dir;
    std::string victim;
    mode_t savedMask = 0;
    void SetUp() override
    {
        savedMask = ::umask(022);
        victim = (dir.path / "victim").string();
        std::filesystem::create_directories(victim);
        ::chmod(victim.c_str(), 0700);
    }
    void TearDown() override { ::umask(savedMask); }
    std::unique_ptr<TransferClient> receiver(std::vector<std::string> args, TransferClientOptions::Mode mode = TransferClientOptions::Mode::Normal,
                                             TransferClientOptions options = {})
    {
        if (options.home.empty()) {
            options = optionsIn(dir);
        }
        options.mode = mode;
        std::string error;
        auto client = makeTransferReceiver(options, args, error);
        // An exception, not a null client that the test then reads.
        if (!client) {
            throw std::runtime_error(error);
        }
        return client;
    }
    bool victimTouched() const
    {
        struct stat st { };
        return ::stat(victim.c_str(), &st) != 0 || (st.st_mode & 07777) != 0700
            || std::filesystem::directory_iterator(victim) != std::filesystem::directory_iterator();
    }
    static bool ended(const TransferClient& client)
    {
        return client.state() == TransferClient::State::Canceled || client.state() == TransferClient::State::Failed;
    }
    static mode_t modeOf(const std::filesystem::path& path)
    {
        struct stat st { };
        return ::lstat(path.c_str(), &st) == 0 ? st.st_mode & 07777 : mode_t(~0u);
    }
};

TEST_F(Hostile, AnEntryUnderASymlinkOfTheSameTransferIsRefused)
{
    // A link x to the victim, then a hard link x/k: written through x, it
    // would land in the victim's directory. x is not a directory of the
    // listing, so x/k has no parent to go in.
    auto client = receiver({ "/r/tree", (dir.path / "dest").string() + "/" });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/tree" }, { "st", "1" }, { "ft", "directory" } },
        { { "fid", "0" }, { "n", "/r/tree/a" }, { "st", "2" }, { "pr", "1" }, { "sz", "5" } },
        { { "fid", "0" }, { "n", "/r/tree/x" }, { "st", "3" }, { "pr", "1" }, { "ft", "symlink" } },
        { { "fid", "0" }, { "n", "/r/tree/x/k" }, { "st", "4" }, { "pr", "3" }, { "ft", "link" }, { "d", "2" } },
    };
    terminal.contents = { { "/r/tree/a", "hello" }, { "/r/tree/x", victim } };
    terminal.run();
    EXPECT_TRUE(ended(*client));
    EXPECT_FALSE(victimTouched());
}

TEST_F(Hostile, AHardLinkToASymlinkIsRefused)
{
    // A second name for the link x would let a later write through it reach the victim.
    auto client = receiver({ "/r/tree", (dir.path / "dest").string() + "/" });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/tree" }, { "st", "1" }, { "ft", "directory" } },
        { { "fid", "0" }, { "n", "/r/tree/x" }, { "st", "2" }, { "pr", "1" }, { "ft", "symlink" } },
        { { "fid", "0" }, { "n", "/r/tree/k" }, { "st", "3" }, { "pr", "1" }, { "ft", "link" }, { "d", "2" } },
    };
    terminal.contents = { { "/r/tree/x", victim } };
    terminal.run();
    EXPECT_EQ(client->state(), TransferClient::State::Done) << client->error();
    ASSERT_EQ(client->items().size(), 3u);
    EXPECT_FALSE(client->items()[2].error.empty());
    EXPECT_FALSE(std::filesystem::exists(dir.path / "dest/tree/k"));
    EXPECT_FALSE(victimTouched());
}

TEST_F(Hostile, ADuplicateNameCannotChangeTheModeThroughALink)
{
    // A link s to the victim, then a directory s with mode 0777.
    auto client = receiver({ "/r/tree", (dir.path / "dest").string() + "/" });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/tree" }, { "st", "1" }, { "ft", "directory" } },
        { { "fid", "0" }, { "n", "/r/tree/s" }, { "st", "2" }, { "pr", "1" }, { "ft", "symlink" } },
        { { "fid", "0" }, { "n", "/r/tree/s" }, { "st", "3" }, { "pr", "1" }, { "ft", "directory" }, { "prm", "511" } },
    };
    terminal.contents = { { "/r/tree/s", victim } };
    terminal.run();
    EXPECT_TRUE(ended(*client));
    EXPECT_FALSE(victimTouched());
}

TEST_F(Hostile, AFileUnderALinkThatWasThereBeforeIsNotWrittenThroughIt)
{
    const auto dest = dir.path / "dest";
    std::filesystem::create_directories(dest);
    std::filesystem::create_directory_symlink(victim, dest / "tree");
    auto client = receiver({ "/r/tree", dest.string() + "/" });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/tree" }, { "st", "1" }, { "ft", "directory" } },
        { { "fid", "0" }, { "n", "/r/tree/f" }, { "st", "2" }, { "pr", "1" }, { "sz", "3" } },
    };
    terminal.contents = { { "/r/tree/f", "bad" } };
    terminal.run();
    EXPECT_FALSE(victimTouched());
    ASSERT_EQ(client->items().size(), 2u);
    EXPECT_FALSE(client->items()[1].error.empty());
}

TEST_F(Hostile, TheRootMustBeTheSourceThatWasAskedFor)
{
    // Received into a directory, the root's name would name the file here: .bashrc, .ssh.
    const auto dest = dir.path / "dest";
    auto client = receiver({ "/r/tree", dest.string() + "/" });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/.bashrc" }, { "st", "1" }, { "sz", "3" } },
    };
    terminal.contents = { { "/r/.bashrc", "bad" } };
    terminal.run();
    EXPECT_TRUE(ended(*client));
    EXPECT_FALSE(std::filesystem::exists(dest / ".bashrc"));
    EXPECT_FALSE(std::filesystem::exists(dest));
}

TEST_F(Hostile, MirrorModeWritesOnlyThePathsThatWereAskedFor)
{
    const auto asked = (dir.path / "asked").string();
    auto client = receiver({ asked }, TransferClientOptions::Mode::Mirror);
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", victim }, { "st", "1" }, { "ft", "directory" } },
        { { "fid", "0" }, { "n", victim + "/planted" }, { "st", "2" }, { "pr", "1" }, { "sz", "3" } },
    };
    terminal.contents = { { victim + "/planted", "bad" } };
    terminal.run();
    EXPECT_TRUE(ended(*client));
    EXPECT_FALSE(victimTouched());
}

TEST_F(Hostile, AnAbsoluteMirrorPathIgnoresTheRemoteHome)
{
    // The terminal says that its home holds the path: mapped to the home
    // here, the file would replace dir/f (as ~/.bashrc).
    const auto remoteHome = (dir.path / "remote").string();
    const auto asked = remoteHome + "/f";
    auto client = receiver({ asked }, TransferClientOptions::Mode::Mirror);
    FakeTerminal terminal { *client, remoteHome };
    terminal.listing = { { { "fid", "0" }, { "n", asked }, { "st", "1" }, { "sz", "2" } } };
    terminal.contents = { { asked, "ok" } };
    terminal.run();
    EXPECT_EQ(client->state(), TransferClient::State::Done) << client->error();
    EXPECT_FALSE(std::filesystem::exists(dir.path / "f"));
    EXPECT_EQ(readFile(asked), "ok");
}

TEST_F(Hostile, AHomeMirrorPathGoesUnderTheLocalHome)
{
    // With another source outside the remote home, the remote home of ~/f
    // would be the local path.
    const auto remoteHome = (dir.path / "rhome").string();
    const auto other = (dir.path / "abs/g").string();
    auto client = receiver({ "~/f", other }, TransferClientOptions::Mode::Mirror);
    FakeTerminal terminal { *client, remoteHome };
    terminal.listing = {
        { { "fid", "0" }, { "n", remoteHome + "/f" }, { "st", "1" }, { "sz", "1" } },
        { { "fid", "1" }, { "n", other }, { "st", "2" }, { "sz", "1" } },
    };
    terminal.contents = { { remoteHome + "/f", "f" }, { other, "g" } };
    terminal.run();
    EXPECT_EQ(client->state(), TransferClient::State::Done) << client->error();
    EXPECT_FALSE(std::filesystem::exists(remoteHome + "/f"));
    EXPECT_EQ(readFile((dir.path / "f").string()), "f");
    EXPECT_EQ(readFile(other), "g");
}

TEST_F(Hostile, ADirectoryThatWasThereBeforeKeepsItsMode)
{
    // A mirror of ~ must not make the home directory 0777.
    auto options = optionsIn(dir);
    options.home = (dir.path / "home").string();
    std::filesystem::create_directories(options.home);
    ::chmod(options.home.c_str(), 0700);
    auto client = receiver({ "~" }, TransferClientOptions::Mode::Mirror, options);
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/home/r" }, { "st", "1" }, { "ft", "directory" }, { "prm", "511" } },
        { { "fid", "0" }, { "n", "/home/r/sub" }, { "st", "2" }, { "pr", "1" }, { "ft", "directory" }, { "prm", "511" } },
        { { "fid", "0" }, { "n", "/home/r/f" }, { "st", "3" }, { "pr", "1" }, { "sz", "1" } },
    };
    terminal.contents = { { "/home/r/f", "f" } };
    terminal.run();
    EXPECT_EQ(client->state(), TransferClient::State::Done) << client->error();
    EXPECT_EQ(modeOf(options.home), 0700u);
    // The directory that the transfer made gets the listed mode, under the umask.
    EXPECT_EQ(modeOf(options.home + "/sub"), 0755u);
    EXPECT_EQ(readFile(options.home + "/f"), "f");
}

TEST_F(Hostile, OverlappingMirrorSourcesAreRefused)
{
    auto client = receiver({ "/r/a", "/r/a/b" }, TransferClientOptions::Mode::Mirror);
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/a" }, { "st", "1" }, { "ft", "directory" } },
        { { "fid", "1" }, { "n", "/r/a/b" }, { "st", "2" }, { "sz", "1" } },
    };
    terminal.run();
    EXPECT_TRUE(ended(*client));
    EXPECT_NE(client->error().find("overlap"), std::string::npos) << client->error();
}

TEST_F(Hostile, SourcesWithTheSameNameAreRefused)
{
    auto client = receiver({ "/r/a/x", "/r/b/x", (dir.path / "dest").string() + "/" });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/a/x" }, { "st", "1" }, { "sz", "1" } },
        { { "fid", "1" }, { "n", "/r/b/x" }, { "st", "2" }, { "sz", "1" } },
    };
    terminal.run();
    EXPECT_TRUE(ended(*client));
    EXPECT_NE(client->error().find("same name"), std::string::npos) << client->error();
}

TEST_F(Hostile, ADirectoryReceivedToANewPathBecomesThatPath)
{
    // As cp -r, whatever the directory holds: the decision does not depend on the listing.
    const auto dest = dir.path / "copy";
    auto client = receiver({ "/r/tree", dest.string() });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/tree" }, { "st", "1" }, { "ft", "directory" } },
        { { "fid", "0" }, { "n", "/r/tree/f" }, { "st", "2" }, { "pr", "1" }, { "sz", "1" } },
    };
    terminal.contents = { { "/r/tree/f", "f" } };
    terminal.run();
    EXPECT_EQ(client->state(), TransferClient::State::Done) << client->error();
    EXPECT_EQ(readFile((dest / "f").string()), "f");
}

TEST_F(Hostile, AChildWhosePathIsNotUnderItsParentIsRefused)
{
    // Short paths for a deep chain: the local paths would grow with the
    // square of the depth while the listing stays small.
    auto client = receiver({ "/r/tree", (dir.path / "dest").string() + "/" });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/tree" }, { "st", "1" }, { "ft", "directory" } },
        { { "fid", "0" }, { "n", "/x" }, { "st", "2" }, { "pr", "1" }, { "ft", "directory" } },
        { { "fid", "0" }, { "n", "/x" }, { "st", "3" }, { "pr", "2" }, { "ft", "directory" } },
    };
    terminal.run();
    EXPECT_TRUE(ended(*client));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "dest"));
}

TEST_F(Hostile, AParentListedWithAnUncleanPathComesBeforeItsChild)
{
    // /r/tree/./././d is longer than its child /r/tree/d/c. The child must
    // still take its base and its local path from that parent.
    auto client = receiver({ "/r/tree", (dir.path / "dest").string() + "/" });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/tree" }, { "st", "1" }, { "ft", "directory" } },
        { { "fid", "0" }, { "n", "/r/tree/./././././././d" }, { "st", "2" }, { "pr", "1" }, { "ft", "directory" } },
        { { "fid", "0" }, { "n", "/r/tree/d/c" }, { "st", "3" }, { "pr", "2" }, { "sz", "1" } },
    };
    terminal.contents = { { "/r/tree/d/c", "x" } };
    terminal.run();
    ASSERT_EQ(client->state(), TransferClient::State::Done) << client->error();
    const auto tree = dir.path / "dest/tree";
    ASSERT_EQ(client->items().size(), 3u);
    EXPECT_EQ(client->items()[1].local, (tree / "d").string());
    EXPECT_EQ(client->items()[2].local, (tree / "d/c").string());
    EXPECT_EQ(readFile((tree / "d/c").string()), "x");
}

TEST_F(Hostile, APathLongerThanPathMaxIsRefused)
{
    const std::string deep = "/r/tree/" + std::string(250, 'a') + std::string(4000, '/');
    auto client = receiver({ "/r/tree", (dir.path / "dest").string() + "/" });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/tree" }, { "st", "1" }, { "ft", "directory" } },
        { { "fid", "0" }, { "n", deep }, { "st", "2" }, { "pr", "1" }, { "sz", "1" } },
    };
    terminal.run();
    EXPECT_TRUE(ended(*client));
}

TEST_F(Hostile, ARelativeListingPathIsRefused)
{
    auto client = receiver({ "tree" }, TransferClientOptions::Mode::Mirror);
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = { { { "fid", "0" }, { "n", "tree" }, { "st", "1" }, { "sz", "1" } } };
    terminal.run(50);
    EXPECT_TRUE(ended(*client));
}

TEST_F(Hostile, AFileWithoutASizeIsRefused)
{
    auto client = receiver({ "/r/f", (dir.path / "f").string() });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = { { { "fid", "0" }, { "n", "/r/f" }, { "st", "1" } } };
    terminal.contents = { { "/r/f", "anything" } };
    terminal.run();
    EXPECT_TRUE(ended(*client));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "f"));
}

TEST_F(Hostile, AStatusForAFileBeforeItsRequestEndsTheSession)
{
    // Unanswered, the request would count against the open files for good:
    // the session would wait with nothing in flight.
    std::vector<Pairs> listing { { { "fid", "0" }, { "n", "/r/tree" }, { "st", "1" }, { "ft", "directory" } } };
    for (int i = 0; i < 40; ++i) {
        listing.push_back({ { "fid", "0" }, { "n", "/r/tree/f" + std::to_string(i) }, { "st", std::to_string(i + 2) }, { "pr", "1" }, { "sz", "1" } });
    }
    auto client = receiver({ "/r/tree", (dir.path / "dest").string() + "/" });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = listing;
    terminal.answerRequests = false;
    terminal.run(4);
    // Entry 40 (f39) waits behind the 32 requests in flight.
    terminal.say({ { "ac", "status" }, { "fid", "r40" }, { "st", "EIO" } });
    terminal.run();
    EXPECT_TRUE(ended(*client));
}

TEST_F(Hostile, AnEmptyStatusIsAnError)
{
    auto client = receiver({ "/r/f", (dir.path / "f").string() });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = { { { "fid", "0" }, { "n", "/r/f" }, { "st", "1" }, { "sz", "1" } } };
    terminal.answerRequests = false;
    terminal.run(4);
    terminal.say({ { "ac", "status" }, { "fid", "r0" }, { "st", "" } });
    terminal.run();
    ASSERT_EQ(client->items().size(), 1u);
    EXPECT_FALSE(client->items()[0].error.empty());
    EXPECT_FALSE(std::filesystem::exists(dir.path / "f"));
}

TEST_F(Hostile, AnErrorOfTheSessionAfterTheListingEndsIt)
{
    auto client = receiver({ "/r/f", (dir.path / "f").string() });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = { { { "fid", "0" }, { "n", "/r/f" }, { "st", "1" }, { "sz", "1" } } };
    terminal.answerRequests = false;
    terminal.run(4);
    terminal.say({ { "ac", "status" }, { "st", "ENOMEM" } });
    EXPECT_EQ(client->state(), TransferClient::State::Failed);
    EXPECT_TRUE(client->over());
}

TEST_F(Hostile, AnAnswerBeforeThePermissionCancels)
{
    auto client = receiver({ "/r/f", (dir.path / "f").string() });
    std::string out;
    client->pull(out, 65536);
    client->receive(reply(client->id(), { { "ac", "file" }, { "fid", "0" }, { "n", "/r/f" }, { "st", "1" } }));
    EXPECT_EQ(client->state(), TransferClient::State::Canceling);
    out.clear();
    client->pull(out, 65536);
    EXPECT_EQ(messages(out).back().at("ac"), "cancel");
}

TEST_F(Hostile, ADeclinedConfirmationLeavesNoDirectory)
{
    auto options = optionsIn(dir);
    options.confirmPaths = true;
    const auto dest = dir.path / "new/dest";
    auto client = receiver({ "/r/f", dest.string() + "/" }, TransferClientOptions::Mode::Normal, options);
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = { { { "fid", "0" }, { "n", "/r/f" }, { "st", "1" }, { "sz", "1" } } };
    terminal.run(4);
    ASSERT_EQ(client->state(), TransferClient::State::Confirm);
    client->confirm(false);
    terminal.run();
    EXPECT_TRUE(ended(*client));
    EXPECT_FALSE(std::filesystem::exists(dir.path / "new"));
}

TEST_F(Hostile, ModesLoseSetIdAndStickyBits)
{
    auto client = receiver({ "/r/f", (dir.path / "f").string() });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = { { { "fid", "0" }, { "n", "/r/f" }, { "st", "1" }, { "sz", "2" }, { "prm", std::to_string(06777) } } };
    terminal.contents = { { "/r/f", "ok" } };
    terminal.run();
    EXPECT_EQ(client->state(), TransferClient::State::Done);
    // Under the umask of 022 that SetUp() sets.
    EXPECT_EQ(modeOf(dir.path / "f"), 0755u);
}

TEST_F(Hostile, ReceivedModesFollowTheUmask)
{
    ::umask(077);
    auto client = receiver({ "/r/tree", (dir.path / "dest").string() + "/" });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/tree" }, { "st", "1" }, { "ft", "directory" }, { "prm", "493" } },
        { { "fid", "0" }, { "n", "/r/tree/f" }, { "st", "2" }, { "pr", "1" }, { "sz", "1" }, { "prm", "420" } },
        { { "fid", "0" }, { "n", "/r/tree/g" }, { "st", "3" }, { "pr", "1" }, { "sz", "1" } },
    };
    terminal.contents = { { "/r/tree/f", "f" }, { "/r/tree/g", "g" } };
    terminal.run();
    EXPECT_EQ(client->state(), TransferClient::State::Done) << client->error();
    EXPECT_EQ(modeOf(dir.path / "dest/tree"), 0700u);
    EXPECT_EQ(modeOf(dir.path / "dest/tree/f"), 0600u);
    EXPECT_EQ(modeOf(dir.path / "dest/tree/g"), 0600u);
}

TEST_F(Hostile, ATreeArrivesWithItsMetadata)
{
    const std::int64_t modified = 1600000000LL * 1000000000LL + 5;
    auto client = receiver({ "/r/tree", (dir.path / "dest").string() + "/" });
    FakeTerminal terminal { *client, "/home/r" };
    const auto text = noise(9000, 11);
    terminal.listing = {
        { { "fid", "0" }, { "n", "/r/tree" }, { "st", "1" }, { "ft", "directory" }, { "prm", "488" }, { "mod", std::to_string(modified) } },
        { { "fid", "0" }, { "n", "/r/tree/f.txt" }, { "st", "2" }, { "pr", "1" }, { "sz", "9000" }, { "prm", "416" }, { "mod", std::to_string(modified) } },
        { { "fid", "0" }, { "n", "/r/tree/l" }, { "st", "3" }, { "pr", "1" }, { "ft", "symlink" }, { "d", "2" } },
        { { "fid", "0" }, { "n", "/r/tree/h" }, { "st", "4" }, { "pr", "1" }, { "ft", "link" }, { "d", "2" } },
    };
    // Compressed, in packets of 4 KiB.
    terminal.contents = { { "/r/tree/f.txt", zlibCompress(text) }, { "/r/tree/l", "/r/tree/f.txt" } };
    terminal.run();
    ASSERT_EQ(client->state(), TransferClient::State::Done) << client->error();
    for (const auto& item : client->items()) {
        EXPECT_TRUE(item.error.empty()) << item.local << ": " << item.error;
    }
    const auto tree = dir.path / "dest/tree";
    EXPECT_EQ(readFile((tree / "f.txt").string()), text);
    EXPECT_EQ(modeOf(tree), 0750u);
    EXPECT_EQ(modeOf(tree / "f.txt"), 0640u);
    EXPECT_EQ(std::filesystem::read_symlink(tree / "l"), tree / "f.txt");
    EXPECT_TRUE(std::filesystem::equivalent(tree / "h", tree / "f.txt"));
    struct stat st { };
    ASSERT_EQ(::stat((tree / "f.txt").c_str(), &st), 0);
    EXPECT_EQ(st.st_mtim.tv_sec, 1600000000);
    EXPECT_EQ(st.st_mtim.tv_nsec, 5);
    ASSERT_EQ(::stat(tree.c_str(), &st), 0);
    EXPECT_EQ(st.st_mtim.tv_sec, 1600000000);
}

TEST_F(Hostile, DataPastTheListedSizeIsRefused)
{
    auto client = receiver({ "/r/f", (dir.path / "f").string() });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = { { { "fid", "0" }, { "n", "/r/f" }, { "st", "1" }, { "sz", "2" } } };
    terminal.contents = { { "/r/f", "much more than two bytes" } };
    terminal.run();
    EXPECT_FALSE(std::filesystem::exists(dir.path / "f"));
    ASSERT_EQ(client->items().size(), 1u);
    EXPECT_TRUE(client->items()[0].error.starts_with("EFBIG")) << client->items()[0].error;
}

TEST_F(Hostile, DataAfterTheEndOfTheZlibStreamIsRefused)
{
    auto client = receiver({ "/r/f.txt", (dir.path / "f.txt").string() });
    FakeTerminal terminal { *client, "/home/r" };
    const auto text = noise(5000, 7);
    terminal.listing = { { { "fid", "0" }, { "n", "/r/f.txt" }, { "st", "1" }, { "sz", "5000" } } };
    // The whole file, then bytes that would be written raw after it. The
    // packets are of the allowed size: the error is the tail, not the size.
    terminal.contents = { { "/r/f.txt", zlibCompress(text) + "tail" } };
    terminal.run();
    EXPECT_FALSE(std::filesystem::exists(dir.path / "f.txt"));
    ASSERT_EQ(client->items().size(), 1u);
    EXPECT_TRUE(client->items()[0].error.starts_with("EINVAL")) << client->items()[0].error;
}

TEST_F(Hostile, AnEndedSessionLeavesNoTemporaryFile)
{
    auto client = receiver({ "/r/f", (dir.path / "out" / "f").string() });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = { { { "fid", "0" }, { "n", "/r/f" }, { "st", "1" }, { "sz", "100" } } };
    terminal.answerRequests = false;
    terminal.run(3);
    // One packet of the file arrives, then the session ends without its end.
    std::string fid;
    for (const auto& m : terminal.sent) {
        if (m.at("ac") == "file" && m.at("fid").starts_with('r')) {
            fid = m.at("fid");
        }
    }
    ASSERT_FALSE(fid.empty());
    terminal.say({ { "ac", "data" }, { "fid", fid }, { "d", "partial" } });
    ASSERT_TRUE(std::filesystem::exists(dir.path / "out"));
    EXPECT_FALSE(std::filesystem::is_empty(dir.path / "out"));
    client.reset();
    EXPECT_TRUE(std::filesystem::is_empty(dir.path / "out"));
}

TEST_F(Hostile, ASignatureForAFileSentWholeIsRefused)
{
    dir.write("big", noise(10000, 8));
    auto options = optionsIn(dir);
    std::string error;
    auto client = makeTransferSender(options, { "big", "/remote/big" }, error);
    ASSERT_TRUE(client) << error;
    FakeTerminal terminal { *client, "/home/r" };
    terminal.started = { { "/remote/big", "rsync" } };
    terminal.run();
    EXPECT_TRUE(ended(*client));
}

TEST(Client, RemoteEntriesNameTheirParents)
{
    EXPECT_EQ(dndRemoteEntry(2, 0, 0, DndFileKind), (DndMeta { { "t", "k" }, { "x", "2" }, { "X", "0" } }));
    EXPECT_EQ(dndRemoteEntry(1, 5, 3, 7), (DndMeta { { "t", "k" }, { "x", "1" }, { "X", "7" }, { "Y", "5" }, { "y", "3" } }));
    EXPECT_EQ(dndReadEntry(4, 0, 2), (DndMeta { { "t", "r" }, { "x", "4" }, { "y", "2" } }));
    EXPECT_EQ(dndReadEntry(0, 9, 1), (DndMeta { { "t", "r" }, { "Y", "9" }, { "x", "1" } }));
    EXPECT_EQ(dndReadEntry(0, 9, 0), (DndMeta { { "t", "r" }, { "Y", "9" } }));
}

TEST(Client, DataPacketsDecodeAloneAndEndEmpty)
{
    const auto bytes = noise(DndDataChunk * 2 + 10, 5);
    const auto packets = dndDataPackets({ { "t", "e" }, { "y", "0" } }, bytes);
    ASSERT_EQ(packets.size(), 4u);
    std::string joined;
    DndBase64Stream stream;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const auto& packet = packets[i];
        const auto body = std::string_view(packet).substr(5, packet.size() - 7);
        const auto [meta, payload] = splitDnd(body);
        EXPECT_LE(payload.size(), DndMaxPacket);
        EXPECT_EQ(meta, "m=0:t=e:y=0");
        EXPECT_EQ(payload.empty(), i == 3);
        // One stream: padding may only end it, in the last data packet.
        if (i < 2) {
            EXPECT_EQ(payload.find('='), std::string_view::npos);
        }
        ASSERT_TRUE(stream.feed(payload, joined));
    }
    ASSERT_TRUE(stream.finish(joined));
    EXPECT_EQ(joined, bytes);
}

TEST(Client, Base64StreamJoinsGroupsAcrossPackets)
{
    const auto bytes = noise(1000, 6);
    const auto text = base64Encode(bytes, false);
    DndBase64Stream stream;
    std::string out;
    for (std::size_t at = 0; at < text.size(); at += 13) {
        ASSERT_TRUE(stream.feed(text.substr(at, 13), out));
    }
    ASSERT_TRUE(stream.finish(out));
    EXPECT_EQ(out, bytes);
    EXPECT_FALSE(stream.feed("@@@@", out));
    // One character left at the end decodes to nothing: the data was cut.
    DndBase64Stream cut;
    out.clear();
    ASSERT_TRUE(cut.feed("QUJD" "R", out));
    EXPECT_EQ(out, "ABC");
    EXPECT_FALSE(cut.finish(out));
}

TEST(Client, ListingsRefuseUnsafeNames)
{
    EXPECT_EQ(dndSplitNames(dndJoinNames({ "a", "b c", "ü" })), (std::vector<std::string> { "a", "b c", "ü" }));
    EXPECT_EQ(dndSplitNames(""), std::vector<std::string>());
    EXPECT_FALSE(dndSplitNames(std::string("a\0..", 4)));
    EXPECT_FALSE(dndSplitNames(std::string("a\0a", 3)));
    EXPECT_FALSE(dndSplitNames("x/y"));
    EXPECT_FALSE(dndSplitNames(std::string("a\0", 2)));
    EXPECT_FALSE(dndSplitNames("\xff"));
    EXPECT_FALSE(dndSplitNames("."));
    EXPECT_FALSE(dndSplitNames(".."));
    EXPECT_FALSE(dndSplitNames(std::string("a\0\0b", 4)));
    EXPECT_FALSE(dndSplitNames(std::string("\0a", 2)));
    EXPECT_TRUE(dndSplitNames(std::string(255, 'n')));
    EXPECT_FALSE(dndSplitNames(std::string(256, 'n')));
    // A truncated UTF-8 sequence at the end of a name.
    EXPECT_FALSE(dndSplitNames("\xc3"));
}

} // namespace
