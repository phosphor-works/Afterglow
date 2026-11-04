// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Graphics.h>
#include <AfterglowProto/Zlib.h>

#include <gtest/gtest.h>

using namespace AfterglowProto;

namespace {

struct Received
{
    GraphicsCommand command;
    std::string payload;
    int chunks = 0;
};

/// Reads a command as the terminal does: the first chunk's keys, every
/// chunk's base64 data, until a chunk without m=1.
Received read(std::string_view stream)
{
    Received out;
    while (!stream.empty()) {
        EXPECT_TRUE(stream.starts_with("\x1b_G"));
        const auto end = stream.find("\x1b\\");
        EXPECT_NE(end, std::string_view::npos);
        const std::string_view body = stream.substr(3, end - 3);
        stream.remove_prefix(end + 2);
        const auto semi = body.find(';');
        const auto keys = GraphicsCommand::parse(body.substr(0, semi));
        if (out.chunks++ == 0) {
            out.command = keys;
        } else {
            // Later chunks carry only m (and q).
            EXPECT_FALSE(keys.hasAction || keys.hasId || keys.hasNumber);
            EXPECT_EQ(keys.compression, 0);
        }
        // Each chunk but the last holds whole base64 groups.
        const auto data = semi == std::string_view::npos ? std::string_view{} : body.substr(semi + 1);
        EXPECT_LE(data.size(), GraphicsChunk);
        EXPECT_EQ(data.size() % 4, 0u);
        out.payload += *base64Decode(data, Base64Check::Strict);
        if (keys.more != 1) {
            EXPECT_TRUE(stream.empty());
            break;
        }
    }
    out.command.more = 0;
    return out;
}

} // namespace

TEST(Graphics, KeysRoundTrip)
{
    for (std::string_view keys : {"a=T,f=100,t=f,i=31,I=7,p=2,q=2,x=-5,y=3,w=10,h=20,X=1,Y=4294967295,c=80,r=24,C=1,z=-1073741825,U=1",
                                  "a=d,d=r,x=4000000000,y=4294967295", "a=a,s=3,v=2,r=1,z=40", "a=c,r=2,c=1,x=0,Y=-1,H=-3,V=4,P=9,Q=8",
                                  "a=f,s=4,v=4,S=64,O=8,o=z,i=1", "i=0", "m=1,q=1"}) {
        const auto cmd = GraphicsCommand::parse(keys);
        EXPECT_EQ(GraphicsCommand::parse(cmd.keys()), cmd) << keys << " -> " << cmd.keys();
    }
    // Defaults are not written.
    EXPECT_EQ(GraphicsCommand{}.keys(), "");
    GraphicsCommand place;
    place.action = 'p';
    place.id = 5;
    EXPECT_EQ(place.keys(), "a=p,i=5");
}

TEST(Graphics, SmallPayloadIsOneCommand)
{
    GraphicsCommand cmd;
    cmd.action = 'T';
    cmd.id = 9;
    cmd.format = 24;
    cmd.s = 1;
    cmd.v = 1;
    const std::string out = writeGraphics(cmd, "\x01\x02\x03");
    EXPECT_EQ(out, "\x1b_Ga=T,f=24,s=1,v=1,i=9;AQID\x1b\\");
    const auto received = read(out);
    EXPECT_EQ(received.chunks, 1);
    EXPECT_EQ(received.payload, "\x01\x02\x03");
    EXPECT_EQ(writeGraphics(GraphicsCommand::parse("a=d,d=a"), ""), "\x1b_Ga=d\x1b\\");
}

TEST(Graphics, ChunkedCompressedRoundTrip)
{
    // 64 x 64 RGBA, a gradient with a noisy channel: it compresses, and still spans several chunks.
    std::string pixels;
    std::uint32_t seed = 1;
    for (int i = 0; i < 64 * 64; ++i) {
        seed = seed * 1664525u + 1013904223u;
        pixels += char(i % 64 * 4);
        pixels += char(i / 64 * 4);
        pixels += char(seed >> 24);
        pixels += char(255);
    }
    GraphicsCommand cmd = GraphicsCommand::parse("a=T,f=32,s=64,v=64,i=3,q=1,o=z");
    const std::string compressed = zlibCompress(pixels);
    ASSERT_GT(compressed.size(), 2 * GraphicsChunk);
    const std::string stream = writeGraphics(cmd, compressed);
    const auto received = read(stream);
    EXPECT_GT(received.chunks, 2);
    EXPECT_EQ(received.command, cmd);
    EXPECT_EQ(received.payload, compressed);
    std::string inflated;
    ASSERT_TRUE(graphicsInflate(received.payload, inflated, pixels.size()));
    EXPECT_EQ(inflated, pixels);
    EXPECT_FALSE(graphicsInflate(received.payload, inflated, pixels.size() - 1));
    // The quiet level goes on every chunk, so the terminal keeps it.
    EXPECT_NE(stream.find("\x1b_Gm=1,q=1;"), std::string::npos);
    EXPECT_NE(stream.find("\x1b_Gm=0,q=1;"), std::string::npos);
}

TEST(Graphics, RepliesRoundTrip)
{
    for (const GraphicsReply& reply : {GraphicsReply{3, 0, 0, "OK"}, GraphicsReply{0, 7, 0, "ENOENT:no such image"},
                                       GraphicsReply{3, 7, 2, "EINVAL:bad"}}) {
        const std::string out = reply.write();
        ASSERT_TRUE(out.starts_with("\x1b_G"));
        ASSERT_TRUE(out.ends_with("\x1b\\"));
        EXPECT_EQ(GraphicsReply::parse(std::string_view(out).substr(3, out.size() - 5)), reply) << out;
    }
    EXPECT_EQ((GraphicsReply{0, 7, 2, "OK"}.write()), "\x1b_GI=7,p=2;OK\x1b\\");
    EXPECT_TRUE(GraphicsReply::parse("i=1;OK")->ok());
    EXPECT_FALSE(GraphicsReply::parse("i=1"));
}
