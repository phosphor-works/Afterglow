// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Transfer.h>

#include <gtest/gtest.h>

#include <cerrno>

using namespace AfterglowProto;

namespace {

/// The payload of an encoded message: what a terminal reads after "5113;".
std::string payload(const std::string& message)
{
    if (!message.starts_with("\x1b]5113;") || !message.ends_with("\x1b\\") || message.size() < 9) {
        ADD_FAILURE() << "not an OSC 5113 message";
        return {};
    }
    return message.substr(7, message.size() - 9);
}

} // namespace

TEST(Transfer, MessagesRoundTrip)
{
    const TransferFields fields{{"ac", "file"}, {"id", "session-1"}, {"fid", "f:1"}, {"n", "/tmp/a file;with=odd\nname"},
                                {"sz", "11"}, {"prm", "420"}, {"mod", "-1500000000"}, {"ft", "regular"}, {"zip", "zlib"},
                                {"tt", "rsync"}, {"pw", "secret"}, {"st", "OK"}, {"pr", "3"}, {"q", "1"}};
    const std::string message = encodeTransfer(fields);
    // Names, statuses, data and passwords go in base64 without padding, so they cannot break the fields.
    EXPECT_NE(message.find(";n=L3RtcC9hIGZpbGU7d2l0aD1vZGQKbmFtZQ;"), std::string::npos);
    EXPECT_EQ(decodeTransfer(payload(message)), fields);
    // Data of any bytes, as in a data packet.
    std::string data;
    for (int i = 0; i < 256; ++i) {
        data += char(i);
    }
    const TransferFields packet{{"ac", "data"}, {"id", "s"}, {"fid", "f"}, {"d", data}};
    EXPECT_EQ(decodeTransfer(payload(encodeTransfer(packet))), packet);
}

TEST(Transfer, KeysAreWrittenInOrder)
{
    // A map: sorted keys.
    EXPECT_EQ(encodeTransfer(TransferFields{{"st", "OK"}, {"id", "x"}, {"ac", "status"}}),
              "\x1b]5113;ac=status;id=x;st=T0s\x1b\\");
    // Views: the order given.
    const std::pair<std::string_view, std::string_view> views[] = {{"st", "OK"}, {"id", "x"}};
    EXPECT_EQ(encodeTransfer(views), "\x1b]5113;st=T0s;id=x\x1b\\");
}

TEST(Transfer, InvalidMessagesAreRefused)
{
    EXPECT_FALSE(decodeTransfer("ac=send"));
    EXPECT_FALSE(decodeTransfer("ac=send;id="));
    EXPECT_FALSE(decodeTransfer("ac=send;id=bad id"));
    EXPECT_FALSE(decodeTransfer("ac=send;id=" + std::string(129, 'a')));
    EXPECT_FALSE(decodeTransfer("ac=send;id=x;fid=bad$"));
    EXPECT_FALSE(decodeTransfer("ac=send;id=x;ac=receive"));
    EXPECT_FALSE(decodeTransfer("ac=file;id=x;n=QQ="));
    EXPECT_FALSE(decodeTransfer("id=x;d=" + std::string(TransferMaxCommand, 'A')));
    // Unknown keys and fields without a key are skipped, also when they repeat.
    const auto f = decodeTransfer("ac=send;;=x;nothing;zz=1;zz=2;id=" + std::string(128, 'a'));
    ASSERT_TRUE(f);
    EXPECT_EQ(f->size(), 2u);
    // Padded and unpadded base64 both read.
    for (const char* name : {"id=x;n=QQ==", "id=x;n=QQ"}) {
        const auto padded = decodeTransfer(name);
        if (!padded) {
            ADD_FAILURE() << name;
            continue;
        }
        EXPECT_EQ(padded->at("n"), "A") << name;
    }
}

TEST(Transfer, LongestMessageIsRead)
{
    // 16 bytes of head, so the base64 is a whole number of groups.
    const std::string head = "id=xy;ac=data;d=";
    const std::string longest = head + std::string(TransferMaxCommand - head.size(), 'A');
    ASSERT_EQ(longest.size(), TransferMaxCommand);
    const auto f = decodeTransfer(longest);
    ASSERT_TRUE(f);
    EXPECT_EQ(f->at("d").size(), (TransferMaxCommand - head.size()) / 4 * 3);
    // One more byte, in a field that is skipped: refused for its length alone.
    EXPECT_FALSE(decodeTransfer(longest + ";"));
}

TEST(Transfer, CancelIdIsReadWithoutDecoding)
{
    EXPECT_EQ(transferCancelId("ac=cancel;id=abc;d=!!!not base64"), "abc");
    EXPECT_EQ(transferCancelId("id=abc;ac=cancel"), "abc");
    EXPECT_EQ(transferCancelId("ac=finish;id=abc"), "");
    EXPECT_EQ(transferCancelId("ac=cancel;id=a b"), "");
    EXPECT_EQ(transferCancelId("ac=cancel;id=first;id=second"), "first");
}

TEST(Transfer, PasswordHashIsSha256OfIdAndPassword)
{
    // sha256("abc;secret"), as kitten transfer --permissions-bypass sends it.
    EXPECT_EQ(transferPasswordHash("abc", "secret"),
              "sha256:b601ae571c3bbd7579c17c83dd3c57858265dd4ed677caf8f489d4aa9bbad56e");
}

TEST(Transfer, StatusNames)
{
    EXPECT_EQ(transferErrorName(EACCES), "EPERM");
    EXPECT_EQ(transferErrorName(EPERM), "EPERM");
    EXPECT_EQ(transferErrorName(ENOENT), "ENOENT");
    EXPECT_EQ(transferErrorName(ENOSPC), "ENOSPC");
    EXPECT_EQ(transferErrorName(EEXIST), "EEXIST");
    EXPECT_EQ(transferErrorName(ENOTDIR), "ENOTDIR");
    EXPECT_EQ(transferErrorName(ELOOP), "ELOOP");
    EXPECT_EQ(transferErrorName(EFBIG), "EFBIG");
    EXPECT_EQ(transferErrorName(EINVAL), "EINVAL");
    EXPECT_EQ(transferErrorName(EXDEV), "EIO");
    for (std::string_view ok : {"OK", "STARTED", "PROGRESS", "CANCELED"}) {
        EXPECT_TRUE(transferSuccess(ok));
    }
    EXPECT_FALSE(transferSuccess("EPERM"));
    EXPECT_TRUE(transferSafeId("a-Z_0.9:/@"));
    EXPECT_FALSE(transferSafeId("a;b"));
}
