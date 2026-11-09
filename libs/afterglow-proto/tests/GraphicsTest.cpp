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
/// chunk's base64 data, until a chunk without m=1. A malformed stream is a
/// failure, and reading stops there.
Received read(std::string_view stream)
{
    Received out;
    while (!stream.empty()) {
        const auto end = stream.find("\x1b\\");
        if (!stream.starts_with("\x1b_G") || end == std::string_view::npos) {
            ADD_FAILURE() << "not a whole APC G command: " << stream.substr(0, 16);
            break;
        }
        const std::string_view body = stream.substr(3, end - 3);
        stream.remove_prefix(end + 2);
        const auto semi = body.find(';');
        const auto keys = GraphicsCommand::parse(body.substr(0, semi));
        if (out.chunks++ == 0) {
            out.command = keys;
        } else {
            // Later chunks carry only m (and q), and a=f for a frame.
            EXPECT_EQ(keys.hasAction, out.command.action == 'f');
            if (keys.hasAction) {
                EXPECT_EQ(keys.action, 'f');
            }
            EXPECT_FALSE(keys.hasId || keys.hasNumber);
            EXPECT_EQ(keys.compression, 0);
        }
        // Every chunk holds whole base64 groups: the writer cuts at a
        // multiple of four, and pads the last.
        const auto data = semi == std::string_view::npos ? std::string_view{} : body.substr(semi + 1);
        EXPECT_LE(data.size(), GraphicsChunk);
        EXPECT_EQ(data.size() % 4, 0u);
        const auto decoded = base64Decode(data, Base64Check::Strict);
        if (!decoded) {
            ADD_FAILURE() << "chunk " << out.chunks << " is not valid base64";
            break;
        }
        out.payload += *decoded;
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
                                  "a=f,s=4,v=4,S=64,O=8,o=z,i=1", "i=0", "m=1,q=1", "a=q,i=31,e=T,U=1,C=1",
                                  "a=t,i=2,N=1", "a=t,f=0", "a=t,f=-1"}) {
        const auto cmd = GraphicsCommand::parse(keys);
        EXPECT_EQ(GraphicsCommand::parse(cmd.keys()), cmd) << keys << " -> " << cmd.keys();
    }
    // Defaults are not written.
    EXPECT_EQ(GraphicsCommand{}.keys(), "");
    EXPECT_EQ(GraphicsCommand::parse("f=0").format, 32u);
    EXPECT_EQ(GraphicsCommand::parse("f=-1").format, 0xFFFFFFFFu);
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

TEST(Graphics, FrameChunksCarryTheFrameAction)
{
    // The protocol asks every chunk of a=f data for a=f, so a terminal does
    // not take them for the chunks of a new image.
    GraphicsCommand cmd = GraphicsCommand::parse("a=f,f=32,s=64,v=64,i=3,r=2,q=2");
    const std::string pixels(64 * 64 * 4, 'p');
    const std::string stream = writeGraphics(cmd, pixels);
    const auto received = read(stream);
    EXPECT_GT(received.chunks, 2);
    EXPECT_EQ(received.command, cmd);
    EXPECT_EQ(received.payload, pixels);
    EXPECT_NE(stream.find("\x1b_Ga=f,m=1,q=2;"), std::string::npos);
    EXPECT_NE(stream.find("\x1b_Ga=f,m=0,q=2;"), std::string::npos);
    // An image's chunks carry no action.
    cmd.action = 't';
    EXPECT_EQ(writeGraphics(cmd, pixels).find("\x1b_Ga="), 0u);
    EXPECT_EQ(writeGraphics(cmd, pixels).find("\x1b_Ga=", 1), std::string::npos);
}

TEST(Graphics, RepliesRoundTrip)
{
    for (const GraphicsReply& reply : {GraphicsReply{3, 0, 0, "OK"}, GraphicsReply{0, 7, 0, "ENOENT:no such image"},
                                       GraphicsReply{3, 7, 2, "EINVAL:bad"}, GraphicsReply{0, 0, 2, "OK"},
                                       GraphicsReply{3, 0, 0, "OK", "r=2"},
                                       GraphicsReply{31, 0, 0, "OK", "e=T,U=1,C=1"},
                                       GraphicsReply{0, 0, 0, "OK", "e=q"}}) {
        const std::string out = reply.write();
        if (!out.starts_with("\x1b_G") || !out.ends_with("\x1b\\")) {
            ADD_FAILURE() << "not a whole APC G reply: " << out;
            continue;
        }
        EXPECT_EQ(GraphicsReply::parse(std::string_view(out).substr(3, out.size() - 5)), reply) << out;
    }
    EXPECT_EQ((GraphicsReply{0, 7, 2, "OK"}.write()), "\x1b_GI=7,p=2;OK\x1b\\");
    // A placement without an id or a number gets no comma before it.
    EXPECT_EQ((GraphicsReply{0, 0, 2, "OK"}.write()), "\x1b_Gp=2;OK\x1b\\");
    // The other keys follow i, I and p, and read back apart from them.
    EXPECT_EQ((GraphicsReply{1, 0, 0, "OK", "r=3"}.write()), "\x1b_Gi=1,r=3;OK\x1b\\");
    const auto feature = GraphicsReply::parse("i=31,e=f,r=1;OK");
    ASSERT_TRUE(feature);
    EXPECT_EQ(feature->id, 31u);
    EXPECT_EQ(feature->keys, "e=f,r=1");
    const auto ok = GraphicsReply::parse("i=1;OK");
    ASSERT_TRUE(ok);
    EXPECT_TRUE(ok->ok());
    EXPECT_FALSE(GraphicsReply::parse("i=1"));
}

TEST(Graphics, FlagsThatAreNotLettersRefuseTheCommand)
{
    // A ';' or ESC in a flag would end the keys or the command, and a
    // command without the flag would do something else.
    GraphicsCommand cmd;
    cmd.action = 'p';
    cmd.medium = 'f';
    cmd.compression = 'z';
    cmd.id = 4;
    EXPECT_TRUE(cmd.flagsAreLetters());
    EXPECT_EQ(cmd.keys(), "a=p,t=f,o=z,i=4");
    for (const char bad : {';', ',', '\x1b', '\0', '1'}) {
        for (char GraphicsCommand::*flag : {&GraphicsCommand::action, &GraphicsCommand::medium, &GraphicsCommand::del,
                                            &GraphicsCommand::compression, &GraphicsCommand::feature}) {
            GraphicsCommand broken = cmd;
            broken.*flag = bad;
            // A zero o is no compression, and a zero e no feature query:
            // both are valid commands.
            const bool valid
                = bad == '\0' && (flag == &GraphicsCommand::compression || flag == &GraphicsCommand::feature);
            EXPECT_EQ(broken.flagsAreLetters(), valid) << int(bad);
            EXPECT_EQ(broken.keys().empty(), !valid) << int(bad);
            EXPECT_EQ(writeGraphics(broken, "data").empty(), !valid) << int(bad);
        }
    }
    // A delete whose d= is not a letter would delete every image without it.
    GraphicsCommand del = GraphicsCommand::parse("a=d,d=;");
    EXPECT_EQ(writeGraphics(del, ""), "");
}

TEST(Graphics, KeysWriteYiWhenYIsUnset)
{
    // a=c reads Y as the source row: a caller that sets only Yi still sends it.
    GraphicsCommand compose;
    compose.action = 'c';
    compose.Yi = -3;
    EXPECT_EQ(compose.keys(), "a=c,Y=-3");
    EXPECT_EQ(GraphicsCommand::parse(compose.keys()).Yi, -3);
}

TEST(Graphics, OneChunkPayloadNeverAsksForMore)
{
    // 3,072 bytes are exactly one chunk of base64: one command, and an m=1
    // from the caller is dropped, or the terminal would wait for more.
    GraphicsCommand cmd = GraphicsCommand::parse("a=t,f=32,s=32,v=24,i=2,m=1");
    const std::string fits(GraphicsChunk / 4 * 3, 'x');
    const std::string one = writeGraphics(cmd, fits);
    EXPECT_EQ(one.find("m="), std::string::npos);
    auto received = read(one);
    EXPECT_EQ(received.chunks, 1);
    EXPECT_EQ(received.payload, fits);
    // One byte more takes a second chunk, which ends the transmission.
    const std::string over(fits.size() + 1, 'y');
    const std::string two = writeGraphics(cmd, over);
    received = read(two);
    EXPECT_EQ(received.chunks, 2);
    EXPECT_EQ(received.payload, over);
    EXPECT_NE(two.find("\x1b_Gm=0;"), std::string::npos);
    EXPECT_EQ(two.find("\x1b_Gm=1;"), std::string::npos);
}

TEST(Graphics, PlacementWithoutPayloadIsOneCommand)
{
    const std::string out = writeGraphics(GraphicsCommand::parse("a=p,i=3,p=7,c=10,r=5,C=1,q=2"), "");
    EXPECT_EQ(out, "\x1b_Ga=p,i=3,p=7,q=2,c=10,r=5,C=1\x1b\\");
    const auto received = read(out);
    EXPECT_EQ(received.chunks, 1);
    EXPECT_TRUE(received.payload.empty());
}
