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

#include <gtest/gtest.h>

#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
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
