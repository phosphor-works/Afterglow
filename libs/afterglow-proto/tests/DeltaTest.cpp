// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Delta.h>

#include <gtest/gtest.h>

#include <cstring>

#include <xxhash.h>

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

/// The operations of a delta, by type, and the bytes the literals carry.
struct Ops
{
    int blocks = 0;
    std::size_t literalBytes = 0;
    int hashes = 0;
};

Ops count(std::string_view delta)
{
    Ops ops;
    for (std::size_t at = 0; at < delta.size();) {
        const auto type = std::uint8_t(delta[at]);
        if (type == 0) {
            ++ops.blocks;
            at += 9;
        } else if (type == 1) {
            std::uint32_t length = 0;
            std::memcpy(&length, delta.data() + at + 1, 4);
            ops.literalBytes += length;
            at += 5 + length;
        } else {
            ++ops.hashes;
            at += 19;
        }
    }
    return ops;
}

/// The receiver's basis and the sender's file, through the signature, the
/// delta and back.
std::string roundTrip(std::string_view basis, std::string_view file, Ops* ops = nullptr)
{
    const std::string signature = makeSignature(basis);
    const auto delta = makeDelta(signature, file);
    EXPECT_TRUE(delta);
    if (!delta) {
        return {};
    }
    if (ops) {
        *ops = count(*delta);
    }
    const auto rebuilt = applyDelta(basis, deltaBlockSize(basis.size()), *delta);
    EXPECT_TRUE(rebuilt);
    return rebuilt.value_or(std::string{});
}

} // namespace

TEST(Delta, BlockSizeIsTheSquareRootWithAFloor)
{
    EXPECT_EQ(deltaBlockSize(0), 64u);
    EXPECT_EQ(deltaBlockSize(4096), 64u);
    EXPECT_EQ(deltaBlockSize(4097), 65u);
    EXPECT_EQ(deltaBlockSize(1000000), 1000u);
    EXPECT_EQ(deltaBlockSize(1000001), 1001u);
}

TEST(Delta, SignatureLayout)
{
    const std::string basis = noise(200, 1);
    const std::string signature = makeSignature(basis);
    // A 12-byte header and one 20-byte record for each of the 4 blocks (64, 64, 64, 8).
    ASSERT_EQ(signature.size(), SignatureHeaderSize + 4 * SignatureRecordSize);
    EXPECT_EQ(signature.substr(0, 8), std::string(8, '\0'));
    EXPECT_EQ(signatureBlockSize(signature, 1 << 24), 64u);
    const std::string last = basis.substr(192);
    const std::string record = signature.substr(SignatureHeaderSize + 3 * SignatureRecordSize);
    std::uint64_t index = 0;
    std::uint32_t weak = 0;
    std::uint64_t strong = 0;
    std::memcpy(&index, record.data(), 8);
    std::memcpy(&weak, record.data() + 8, 4);
    std::memcpy(&strong, record.data() + 12, 8);
    EXPECT_EQ(index, 3u);
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    for (unsigned char c : last) {
        a += c;
        b += a;
    }
    EXPECT_EQ(weak, (a & 0xffffu) | (b << 16));
    EXPECT_EQ(strong, XXH3_64bits(last.data(), last.size()));
}

TEST(Delta, MalformedSignaturesAreRefused)
{
    const std::string good = makeSignature("x");
    EXPECT_FALSE(signatureBlockSize(good.substr(0, 11), 1 << 24));
    EXPECT_FALSE(signatureBlockSize(good + "x", 1 << 24));
    std::string versioned = good;
    versioned[0] = 1;
    EXPECT_FALSE(signatureBlockSize(versioned, 1 << 24));
    std::string zero = good;
    std::memset(zero.data() + 8, 0, 4);
    EXPECT_FALSE(signatureBlockSize(zero, 1 << 24));
    EXPECT_FALSE(signatureBlockSize(good, 63));
    EXPECT_FALSE(makeDelta(zero, "file"));
}

TEST(Delta, EmptyFile)
{
    Ops ops;
    EXPECT_EQ(roundTrip("", "", &ops), "");
    EXPECT_EQ(ops.blocks, 0);
    EXPECT_EQ(ops.hashes, 1);
    EXPECT_EQ(roundTrip("old contents", "", &ops), "");
    EXPECT_EQ(roundTrip("", "new contents", &ops), "new contents");
    EXPECT_EQ(ops.literalBytes, 12u);
}

TEST(Delta, OneBlock)
{
    const std::string block = noise(64, 2);
    Ops ops;
    EXPECT_EQ(roundTrip(block, block, &ops), block);
    EXPECT_EQ(ops.blocks, 1);
    EXPECT_EQ(ops.literalBytes, 0u);
}

TEST(Delta, SizeNotAMultipleOfTheBlockSize)
{
    // 10,000 bytes: blocks of 100, and the file grows by a 37-byte tail.
    const std::string basis = noise(10000, 3);
    const std::string file = basis + noise(37, 4);
    Ops ops;
    EXPECT_EQ(roundTrip(basis, file, &ops), file);
    EXPECT_EQ(ops.blocks, 100);
    EXPECT_EQ(ops.literalBytes, 37u);
    // A short last block of the basis matches too, at the end of the file.
    const std::string shortBasis = noise(10050, 5);
    EXPECT_EQ(roundTrip(shortBasis, shortBasis, &ops), shortBasis);
    EXPECT_EQ(ops.literalBytes, 0u);
}

TEST(Delta, EveryByteChanged)
{
    const std::string basis = noise(50000, 6);
    std::string file = basis;
    for (char& c : file) {
        c = char(~c);
    }
    Ops ops;
    EXPECT_EQ(roundTrip(basis, file, &ops), file);
    EXPECT_EQ(ops.blocks, 0);
    EXPECT_EQ(ops.literalBytes, file.size());
}

TEST(Delta, InsertionsAndChangesKeepMostBlocks)
{
    // Larger than a few produce() reads: an insertion, a changed run and a new tail.
    const std::string basis = noise(512 * 1024, 7);
    std::string file = basis.substr(0, 200000) + "inserted" + basis.substr(200000);
    file.replace(300000, 5000, std::string(5000, 'z'));
    file += noise(1536 * 1024, 8);
    Ops ops;
    EXPECT_EQ(roundTrip(basis, file, &ops), file);
    EXPECT_GT(ops.blocks, 600);
}

TEST(Delta, GeneratorWorksInBoundedSteps)
{
    const std::string basis = noise(300000, 9);
    std::string file = basis;
    file.insert(1000, "x");
    const std::string signature = makeSignature(basis);
    DeltaGenerator generator(signature);
    MemorySource source(file);
    std::string all;
    int calls = 0;
    while (!generator.done()) {
        std::string out;
        ASSERT_TRUE(generator.produce(source, out, 4096, 65536));
        // One step stops after the bytes asked for, plus at most one operation.
        EXPECT_LE(out.size(), 4096u + 5 + 65536);
        all += out;
        ++calls;
    }
    EXPECT_GT(calls, 3);
    EXPECT_EQ(applyDelta(basis, deltaBlockSize(basis.size()), all), file);
}

TEST(Delta, ApplierTakesOperationsInAnyPieces)
{
    const std::string basis = noise(40000, 10);
    std::string file = basis.substr(5000) + noise(3000, 11);
    const auto delta = makeDelta(makeSignature(basis), file);
    ASSERT_TRUE(delta);
    struct Target : DeltaTarget
    {
        std::string_view basis;
        std::uint32_t blockSize = 0;
        std::string out;
        bool readBlock(std::uint64_t index, std::string& block) override
        {
            block += basis.substr(std::size_t(index) * blockSize, blockSize);
            return true;
        }
        bool write(std::string_view bytes) override
        {
            out += bytes;
            return true;
        }
    } target;
    target.basis = basis;
    target.blockSize = deltaBlockSize(basis.size());
    DeltaApplier applier(target.blockSize, basis.size(), 1 << 24);
    std::string pending;
    for (std::size_t at = 0; at < delta->size(); at += 3) {
        pending += delta->substr(at, 3);
        const auto used = applier.apply(pending, target);
        ASSERT_TRUE(used);
        pending.erase(0, *used);
    }
    EXPECT_TRUE(pending.empty());
    EXPECT_TRUE(applier.finished());
    EXPECT_EQ(target.out, file);
}

TEST(Delta, InvalidOperationsAreRefused)
{
    const std::string basis = noise(1000, 12); // 16 blocks of 64
    const auto hash = [](std::string_view bytes) {
        const auto digest = fileHash(bytes);
        return std::string("\x02\x10\x00", 3) + std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
    };
    const auto block = [](std::uint64_t index) {
        std::string op(9, '\0');
        std::memcpy(op.data() + 1, &index, 8);
        return op;
    };
    const auto range = [](std::uint64_t first, std::uint32_t countLess1) {
        std::string op(13, '\0');
        op[0] = 3;
        std::memcpy(op.data() + 1, &first, 8);
        std::memcpy(op.data() + 9, &countLess1, 4);
        return op;
    };
    EXPECT_EQ(applyDelta(basis, 64, block(15) + hash(basis.substr(960))), basis.substr(960));
    EXPECT_EQ(applyDelta(basis, 64, range(0, 15) + hash(basis)), basis);
    EXPECT_FALSE(applyDelta(basis, 64, block(16) + hash("")));
    EXPECT_FALSE(applyDelta(basis, 64, range(10, 6) + hash("")));
    EXPECT_FALSE(applyDelta(basis, 64, std::string("\x07", 1)));
    EXPECT_FALSE(applyDelta(basis, 64, block(0) + hash("wrong")));
    EXPECT_FALSE(applyDelta(basis, 64, hash("") + block(0)));
    EXPECT_FALSE(applyDelta(basis, 64, block(0))); // no hash: incomplete
    // A literal over the receiver's limit is refused before its bytes arrive.
    struct Sink : DeltaTarget
    {
        bool readBlock(std::uint64_t, std::string&) override { return false; }
        bool write(std::string_view) override { return true; }
    } sink;
    DeltaApplier applier(64, basis.size(), 100);
    EXPECT_FALSE(applier.apply(std::string("\x01\x65\x00\x00\x00", 5), sink));
    // A target that stops ends the delta.
    DeltaApplier stopped(64, basis.size(), 100);
    EXPECT_FALSE(stopped.apply(block(0), sink));
}

TEST(Delta, FileHashIsCanonicalXxh3)
{
    const std::string text = "the whole file";
    XXH128_canonical_t canonical{};
    XXH128_canonicalFromHash(&canonical, XXH3_128bits(text.data(), text.size()));
    const auto digest = fileHash(text);
    EXPECT_EQ(std::memcmp(digest.data(), canonical.digest, 16), 0);
    EXPECT_EQ(blockHash(text), XXH3_64bits(text.data(), text.size()));
}
