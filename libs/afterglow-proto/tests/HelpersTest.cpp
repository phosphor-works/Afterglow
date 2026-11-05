// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Random.h>
#include <AfterglowProto/Sha256.h>
#include <AfterglowProto/Zlib.h>

#include <gtest/gtest.h>

#include <string>

using namespace AfterglowProto;

namespace {

std::string hex(std::string_view text)
{
    return toHex(sha256(text));
}

std::string hmac(std::string_view key, std::string_view message)
{
    return toHex(hmacSha256(key, message));
}

} // namespace

// FIPS 180-4 examples (NIST CSRC "SHA256.pdf" and "SHA2_Additional.pdf").
TEST(Sha256, NistVectors)
{
    EXPECT_EQ(hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    EXPECT_EQ(hex(std::string(1000000, 'a')), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, UpdatesOfAnySizeGiveOneDigest)
{
    const std::string text(1000, 'q');
    for (std::size_t step : {1u, 3u, 55u, 56u, 63u, 64u, 65u, 999u}) {
        Sha256 hash;
        for (std::size_t at = 0; at < text.size(); at += step) {
            hash.update(std::string_view(text).substr(at, step));
        }
        EXPECT_EQ(toHex(hash.finish()), hex(text)) << step;
    }
    // finish() starts over.
    Sha256 hash;
    hash.update("abc");
    hash.finish();
    hash.update("abc");
    EXPECT_EQ(toHex(hash.finish()), hex("abc"));
    // An empty view holds a null pointer. It adds nothing, also into a partial block.
    // A memcpy from the null pointer is caught only by UBSan (the debug preset),
    // and that preset recovers: it prints a runtime error to the test log and
    // this test still passes. Read the log for the error.
    Sha256 partial;
    partial.update("ab");
    partial.update(std::string_view());
    partial.update("c");
    EXPECT_EQ(toHex(partial.finish()), hex("abc"));
}

// RFC 4231, test cases 1 to 7.
TEST(HmacSha256, Rfc4231Vectors)
{
    EXPECT_EQ(hmac(std::string(20, '\x0b'), "Hi There"), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    EXPECT_EQ(hmac("Jefe", "what do ya want for nothing?"),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    EXPECT_EQ(hmac(std::string(20, '\xaa'), std::string(50, '\xdd')),
              "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
    std::string key4;
    for (char c = 1; c <= 25; ++c) {
        key4 += c;
    }
    EXPECT_EQ(hmac(key4, std::string(50, '\xcd')), "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
    // Case 5 gives the first 128 bits only.
    EXPECT_EQ(hmac(std::string(20, '\x0c'), "Test With Truncation").substr(0, 32), "a3b6167473100ee06e0c796c2955552b");
    EXPECT_EQ(hmac(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First"),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    EXPECT_EQ(hmac(std::string(131, '\xaa'),
                   "This is a test using a larger than block-size key and a larger than block-size data. The key needs to "
                   "be hashed before being used by the HMAC algorithm."),
              "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
    // An empty key, also as a view with a null pointer. Only UBSan (the debug
    // preset) reports a memcpy from that pointer.
    EXPECT_EQ(hmac("", ""), "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
    EXPECT_EQ(hmac(std::string_view(), std::string_view()), hmac("", ""));
}

TEST(Random, FillsTheBufferWithDifferentBytes)
{
    std::string a(4096, '\0');
    std::string b(4096, '\0');
    ASSERT_TRUE(randomBytes(a.data(), a.size()));
    ASSERT_TRUE(randomBytes(b.data(), b.size()));
    EXPECT_NE(a, b);
    EXPECT_NE(a, std::string(4096, '\0'));
    const auto token = randomHex(16);
    ASSERT_TRUE(token);
    EXPECT_EQ(token->size(), 32u);
    EXPECT_EQ(token->find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(Zlib, StreamsInSmallSteps)
{
    std::string text;
    for (int i = 0; i < 20000; ++i) {
        text += std::to_string(i * 7919 % 1000);
    }
    // Deflate 100 bytes of input at a time into 50-byte steps, as a sender pumps packets.
    ZlibStream deflater(ZlibStream::Mode::Deflate);
    ASSERT_TRUE(deflater.valid());
    std::string compressed;
    std::size_t at = 0;
    bool end = false;
    for (int steps = 0; !end && steps < 100000; ++steps) {
        const bool last = at >= text.size();
        if (deflater.inputLeft() == 0 && !last) {
            deflater.setInput(std::string_view(text).substr(at, 100));
            at += 100;
        }
        char out[50];
        std::size_t produced = 0;
        const auto result = deflater.step(out, sizeof out, produced, at >= text.size());
        ASSERT_NE(result, ZlibStream::Result::Error);
        compressed.append(out, produced);
        end = result == ZlibStream::Result::End;
    }
    ASSERT_TRUE(end);
    EXPECT_LT(compressed.size(), text.size());
    std::string whole;
    ASSERT_TRUE(zlibInflate(compressed, whole, text.size()));
    EXPECT_EQ(whole, text);

    ZlibStream inflater(ZlibStream::Mode::Inflate);
    ASSERT_TRUE(inflater.valid());
    std::string back;
    bool ended = false;
    for (std::size_t from = 0; from < compressed.size() && !ended; from += 7) {
        inflater.setInput(std::string_view(compressed).substr(from, 7));
        for (int steps = 0; inflater.inputLeft() && steps < 100000; ++steps) {
            char out[64];
            std::size_t produced = 0;
            const auto result = inflater.step(out, sizeof out, produced);
            ASSERT_NE(result, ZlibStream::Result::Error);
            back.append(out, produced);
            if (result == ZlibStream::Result::End) {
                ended = true;
                break;
            }
        }
    }
    EXPECT_TRUE(ended);
    EXPECT_EQ(back, text);
}

TEST(Zlib, StreamReportsBadData)
{
    ZlibStream inflater(ZlibStream::Mode::Inflate);
    ASSERT_TRUE(inflater.valid());
    inflater.setInput("not zlib");
    char out[64];
    std::size_t produced = 0;
    EXPECT_EQ(inflater.step(out, sizeof out, produced), ZlibStream::Result::Error);
}

TEST(Zlib, InflateStopsAtTheLimitAndOnBadData)
{
    const std::string zeros(100000, '\0');
    const std::string compressed = zlibCompress(zeros);
    std::string out = "kept";
    EXPECT_TRUE(zlibInflate(compressed, out, zeros.size()));
    EXPECT_EQ(out, "kept" + zeros);
    out = "kept";
    EXPECT_FALSE(zlibInflate(compressed, out, zeros.size() - 1));
    EXPECT_EQ(out, "kept");
    EXPECT_FALSE(zlibInflate("not zlib", out, 100));
    EXPECT_FALSE(zlibInflate(compressed.substr(0, compressed.size() - 4), out, zeros.size()));
    EXPECT_EQ(out, "kept");
}
