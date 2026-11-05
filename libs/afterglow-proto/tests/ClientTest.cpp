// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The client side that the library adds for aglow: delta classes over file
// descriptors, the transfer path rules, the session's first messages and
// the helpers of remote drag and drop trees.

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/DndTree.h>
#include <AfterglowProto/FdDelta.h>
#include <AfterglowProto/TransferClient.h>
#include <AfterglowProto/Version.h>
#include <AfterglowProto/Zlib.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sys/stat.h>
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
        path = ::mkdtemp(pattern.data());
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

TEST(Client, ProtocolVersionIsPositive)
{
    EXPECT_GE(ProtocolVersion, 1);
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
    FdSource source(newFd);
    DeltaGenerator generator(makeSignature(basis));
    std::string delta;
    while (!generator.done()) {
        ASSERT_TRUE(generator.produce(source, delta, 65536, 1 << 20));
    }
    ::close(newFd);
    EXPECT_LT(delta.size(), changed.size() / 4);

    const int basisFd = ::open(basisPath.c_str(), O_RDONLY);
    const auto outPath = (dir.path / "out").string();
    const int outFd = ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
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
    const auto outPath = (dir.path / "out").string();
    const int outFd = ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FdDeltaTarget target(basisFd, outFd, 64, 100);
    std::string block;
    ASSERT_TRUE(target.readBlock(0, block));
    EXPECT_TRUE(target.write(block));
    EXPECT_FALSE(target.write(block));
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
}

TEST(Client, CompressionSkipsCompressedFormats)
{
    using C = TransferClientOptions::Compress;
    EXPECT_TRUE(transferShouldCompress("/a/notes.txt", C::Auto));
    EXPECT_TRUE(transferShouldCompress("/a/Makefile", C::Auto));
    EXPECT_FALSE(transferShouldCompress("/a/photo.JPG", C::Auto));
    EXPECT_FALSE(transferShouldCompress("/a/archive.tar.gz", C::Auto));
    EXPECT_TRUE(transferShouldCompress("/a/photo.jpg", C::Always));
    EXPECT_FALSE(transferShouldCompress("/a/notes.txt", C::Never));
}

TransferClientOptions optionsIn(const TempDir& dir)
{
    TransferClientOptions options;
    options.home = dir.path.string();
    options.cwd = dir.path.string();
    return options;
}

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
    std::string out;
    sender->pull(out, 65536);
    sender->receive(reply(sender->id(), { { "ac", "status" }, { "st", "OK" } }));
    out.clear();
    sender->pull(out, 65536);
    std::map<std::string, TransferFields> byName;
    for (const auto& m : messages(out)) {
        ASSERT_EQ(m.at("ac"), "file");
        byName[m.at("n")] = m;
    }
    ASSERT_EQ(byName.size(), 6u);
    EXPECT_EQ(byName.at("/dest/tree/sub").at("ft"), "directory");
    EXPECT_EQ(byName.at("/dest/tree").at("ft"), "directory");
    EXPECT_EQ(byName.at("/dest/tree/a.txt").at("zip"), "zlib");
    EXPECT_EQ(byName.at("/dest/tree/a.txt").at("sz"), "5000");
    EXPECT_EQ(byName.at("/dest/tree/link").at("ft"), "symlink");
    // Sorted by name, "a.txt" comes before "hard": the second name of the inode is the link.
    EXPECT_EQ(byName.at("/dest/tree/hard").at("ft"), "link");
    EXPECT_EQ(byName.at("/dest/tree/sub/b").at("ft"), "regular");
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
    sender->receive(reply(sender->id(), { { "ac", "status" }, { "st", "CANCELED" } }));
    EXPECT_TRUE(sender->over());
}

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
    std::vector<std::vector<std::pair<std::string, std::string>>> listing;
    std::map<std::string, std::string> contents; // data for the requests, by remote path
    std::map<std::string, std::string> started; // extra STARTED fields for a send, by remote path
    std::vector<TransferFields> sent;
    bool finished = false;

    void say(std::vector<std::pair<std::string, std::string>> fields)
    {
        client.receive(reply(client.id(), std::move(fields)));
    }
    // Runs the session until the client stops or @p rounds pass.
    void run(int rounds = 200)
    {
        std::size_t specs = 0;
        for (int round = 0; round < rounds && !client.over(); ++round) {
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
                    std::vector<std::pair<std::string, std::string>> answer { { "ac", "status" }, { "fid", m.at("fid") },
                        { "st", m.at("ft") == "directory" ? "OK" : "STARTED" } };
                    if (started.contains(m.at("n"))) {
                        answer.emplace_back("tt", started.at(m.at("n")));
                    }
                    say(answer);
                } else if (action == "file" && answerRequests) {
                    const auto data = contents.contains(m.at("n")) ? contents.at(m.at("n")) : std::string();
                    say({ { "ac", "end_data" }, { "fid", m.at("fid") }, { "d", data } });
                }
            }
        }
    }
};

struct Hostile : testing::Test
{
    TempDir dir;
    std::string victim;
    void SetUp() override
    {
        victim = (dir.path / "victim").string();
        std::filesystem::create_directories(victim);
        ::chmod(victim.c_str(), 0700);
    }
    std::unique_ptr<TransferClient> receiver(std::vector<std::string> args, TransferClientOptions::Mode mode = TransferClientOptions::Mode::Normal)
    {
        auto options = optionsIn(dir);
        options.mode = mode;
        std::string error;
        auto client = makeTransferReceiver(options, args, error);
        EXPECT_TRUE(client) << error;
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
};

TEST_F(Hostile, AHardLinkThroughALinkOfTheSameTransferIsRefused)
{
    // A link x to the victim, then a hard link x/k: written through x, it
    // would land in the victim's directory.
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

TEST_F(Hostile, ARelativeListingPathIsRefused)
{
    auto client = receiver({ "tree" }, TransferClientOptions::Mode::Mirror);
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = { { { "fid", "0" }, { "n", "tree" }, { "st", "1" }, { "sz", "1" } } };
    terminal.run(50);
    EXPECT_TRUE(ended(*client));
}

TEST_F(Hostile, ModesLoseSetIdAndStickyBits)
{
    auto client = receiver({ "/r/f", (dir.path / "f").string() });
    FakeTerminal terminal { *client, "/home/r" };
    terminal.listing = { { { "fid", "0" }, { "n", "/r/f" }, { "st", "1" }, { "sz", "2" }, { "prm", std::to_string(06777) } } };
    terminal.contents = { { "/r/f", "ok" } };
    terminal.run();
    EXPECT_EQ(client->state(), TransferClient::State::Done);
    struct stat st { };
    ASSERT_EQ(::stat((dir.path / "f").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0777u);
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
    EXPECT_FALSE(client->items()[0].error.empty());
}

TEST_F(Hostile, DataAfterTheEndOfTheZlibStreamIsRefused)
{
    auto client = receiver({ "/r/f.txt", (dir.path / "f.txt").string() });
    FakeTerminal terminal { *client, "/home/r" };
    const auto text = noise(5000, 7);
    terminal.listing = { { { "fid", "0" }, { "n", "/r/f.txt" }, { "st", "1" }, { "sz", "5000" } } };
    // The whole file, then bytes that would be written raw after it.
    terminal.contents = { { "/r/f.txt", zlibCompress(text) + "tail" } };
    terminal.run();
    EXPECT_FALSE(std::filesystem::exists(dir.path / "f.txt"));
    ASSERT_EQ(client->items().size(), 1u);
    EXPECT_FALSE(client->items()[0].error.empty());
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
}

} // namespace
