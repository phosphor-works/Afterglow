// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Json.h>
#include <AfterglowProto/Run.h>

#include <gtest/gtest.h>

#include <string>

using namespace AfterglowProto;

TEST(Json, ReadsEveryKindOfValue)
{
    const auto value = Json::parse(R"( {"a": [1, -2.5e3, true, false, null], "b": {"c": "d"}, "e": 0.25} )");
    ASSERT_TRUE(value);
    const Json::Array& a = (*value)["a"].toArray();
    ASSERT_EQ(a.size(), 5u);
    EXPECT_EQ(a[0].toInteger(), 1);
    EXPECT_EQ(a[1].toNumber(), -2500);
    EXPECT_TRUE(a[2].toBool());
    EXPECT_FALSE(a[3].toBool(true));
    EXPECT_TRUE(a[4].isNull());
    EXPECT_EQ((*value)["b"]["c"].toString(), "d");
    EXPECT_EQ((*value)["e"].toNumber(), 0.25);
    EXPECT_FALSE((*value)["e"].toInteger());
    EXPECT_TRUE((*value)["missing"].isNull());
}

TEST(Json, WritesWhatItReads)
{
    const Json::Value value = Json::Object{{"type", "call"}, {"id", std::int64_t(1) << 40}, {"args", Json::Array{"x", 1.5, nullptr}},
                                           {"empty", Json::Object{}}, {"flag", false}};
    const std::string text = Json::write(value);
    EXPECT_EQ(text, R"({"type":"call","id":1099511627776,"args":["x",1.5,null],"empty":{},"flag":false})");
    EXPECT_EQ(Json::parse(text), value);
}

TEST(Json, StringsRoundTrip)
{
    const std::string text = std::string("quote \" backslash \\ tab \t nl \n cr \r bell \x07 nul ") + '\0'
        + " é 漢 😀 / end";
    const std::string written = Json::write(Json::Value(text));
    EXPECT_EQ(written.find('\n'), std::string::npos); // one line on the wire
    EXPECT_EQ(Json::stringSize(text), written.size());
    const auto read = Json::parse(written);
    ASSERT_TRUE(read);
    EXPECT_EQ(read->toString(), text);
}

TEST(Json, ReadsEscapesAndSurrogatePairs)
{
    EXPECT_EQ(Json::parse(R"("\u00e9\ud83d\ude00\/\b\f")")->toString(), "é😀/\b\f");
    // A lone surrogate is not text: it reads as U+FFFD.
    EXPECT_EQ(Json::parse(R"("a\ud800b")")->toString(), "a\xEF\xBF\xBD" "b");
    EXPECT_EQ(Json::parse(R"("\udc00\u0041")")->toString(), "\xEF\xBF\xBD" "A");
}

TEST(Json, WritesInvalidUtf8AsReplacement)
{
    const std::string written = Json::write(Json::Value(std::string("a\xFF" "b\xC3")));
    EXPECT_EQ(Json::parse(written)->toString(), "a\xEF\xBF\xBD" "b\xEF\xBF\xBD");
}

TEST(Json, RefusesWhatIsNotJson)
{
    for (const char* text : {"", "{", "[1,]", "{\"a\" 1}", "01", "1.", "+1", "nan", "Infinity", "0x10", "\"\x01\"",
                             "\"\\x\"", "\"\xFF\"", "[1] 2", "tru", "{1:2}", "1e999", "\"\\u12\""}) {
        std::string error;
        EXPECT_FALSE(Json::parse(text, &error)) << text;
        EXPECT_FALSE(error.empty()) << text;
    }
    std::string deep(Json::MaxDepth + 2, '[');
    deep += std::string(Json::MaxDepth + 2, ']');
    EXPECT_FALSE(Json::parse(deep));
    std::string fine(Json::MaxDepth, '[');
    fine += std::string(Json::MaxDepth, ']');
    EXPECT_TRUE(Json::parse(fine));
}

TEST(RunInput, PartsStayUnderTheLimitAndKeepCharactersWhole)
{
    // Control characters cost six bytes each once written.
    const std::string controls(Run::InputPartBytes, '\x01');
    const std::size_t part = Run::inputPart(controls, false);
    EXPECT_EQ(part, (Run::InputPartBytes - 2) / 6);
    EXPECT_LE(Json::stringSize(controls.substr(0, part)), Run::InputPartBytes);

    // A four-byte character across the limit goes whole into the next part.
    std::string text(Run::InputPartBytes - 4, 'a');
    text += "😀";
    text += std::string(100, 'b');
    const std::size_t first = Run::inputPart(text, false);
    EXPECT_EQ(first, Run::InputPartBytes - 4);
    EXPECT_EQ(Run::inputPart(std::string_view(text).substr(first), true), text.size() - first);
}

TEST(RunInput, WaitsForMoreUntilTheEnd)
{
    EXPECT_EQ(Run::inputPart("short", false), 0u);
    EXPECT_EQ(Run::inputPart("short", true), 5u);
    EXPECT_EQ(Run::inputPart("", true), 0u);
    // A sequence cut at the end waits for its other bytes, unless input ended.
    EXPECT_EQ(Run::inputPart("ab\xF0\x9F", true), 4u);
    const auto parsed = Json::parse(Json::write(Json::Value(std::string("ab\xF0\x9F"))));
    ASSERT_TRUE(parsed);
}
