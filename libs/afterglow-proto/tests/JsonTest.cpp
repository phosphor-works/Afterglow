// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Json.h>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <type_traits>

using namespace AfterglowProto;

namespace {

/// The string that @p json reads as, or nothing.
std::optional<std::string> stringOf(std::string_view json)
{
    const auto value = Json::parse(json);
    if (!value || !value->isString()) return std::nullopt;
    return value->toString();
}

std::string errorOf(std::string_view json)
{
    std::string error;
    EXPECT_FALSE(Json::parse(json, &error)) << json;
    return error;
}

std::string nested(int depth, std::string_view inner = {})
{
    return std::string(std::size_t(depth), '[') + std::string(inner) + std::string(std::size_t(depth), ']');
}

// A pointer is no bool, and every integer type is a number.
static_assert(!std::is_constructible_v<Json::Value, int*>);
static_assert(!std::is_constructible_v<Json::Value, const void*>);
static_assert(std::is_constructible_v<Json::Value, long>);
static_assert(std::is_constructible_v<Json::Value, unsigned>);
static_assert(std::is_constructible_v<Json::Value, std::size_t>);
static_assert(!std::is_constructible_v<Json::Value, char>);

} // namespace

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

TEST(Json, WritesNumbersAsJson)
{
    const double big = Json::MaxExactInteger;
    EXPECT_EQ(Json::write(1e15), "1000000000000000");
    EXPECT_EQ(Json::write(big - 1), "9007199254740991");
    EXPECT_EQ(Json::write(-(big - 1)), "-9007199254740991");
    EXPECT_EQ(Json::write(1e21), "1e+21");
    EXPECT_EQ(Json::write(0.1), "0.1");
    EXPECT_EQ(Json::write(-5), "-5");
    EXPECT_EQ(Json::write(-0.0), "0");
    // JSON has no infinity or NaN.
    EXPECT_EQ(Json::write(std::numeric_limits<double>::infinity()), "null");
    EXPECT_EQ(Json::write(std::numeric_limits<double>::quiet_NaN()), "null");
    // The longest number still reads back as itself.
    const double longest = -2.2250738585072014e-308;
    EXPECT_EQ(Json::parse(Json::write(longest)), Json::Value(longest));
}

TEST(Json, IntegersAreExactBelowTwoToThe53)
{
    const double big = Json::MaxExactInteger;
    EXPECT_EQ(Json::Value(big - 1).toInteger(), std::int64_t(big) - 1);
    EXPECT_EQ(Json::Value(-(big - 1)).toInteger(), -(std::int64_t(big) - 1));
    // 2^53 can be the rounding of 2^53 + 1.
    EXPECT_FALSE(Json::Value(big).toInteger());
    EXPECT_FALSE(Json::Value(1.5).toInteger());
    EXPECT_FALSE(Json::Value("1").toInteger());
    EXPECT_EQ(Json::Value(std::size_t(3)).toInteger(), 3);
}

TEST(Json, ConstructorsPickTheTypeOfTheArgument)
{
    EXPECT_TRUE(Json::Value(true).isBool());
    EXPECT_TRUE(Json::Value(3u).isNumber());
    EXPECT_TRUE(Json::Value("text").isString());
    EXPECT_TRUE(Json::Value(static_cast<const char*>(nullptr)).isNull());
    EXPECT_TRUE(Json::Value(nullptr).isNull());
}

TEST(Json, StringsRoundTrip)
{
    const std::string text = std::string("quote \" backslash \\ tab \t nl \n cr \r bell \x07 nul ") + '\0'
        + " é 漢 😀 / end \xFF";
    const std::string written = Json::write(Json::Value(text));
    EXPECT_EQ(written.find('\n'), std::string::npos); // one line on the wire
    // The pieces cost what the writer writes.
    std::size_t size = 2;
    std::size_t at = 0;
    while (const std::size_t piece = Json::nextStringPiece(text, at)) size += piece;
    EXPECT_EQ(at, text.size());
    EXPECT_EQ(size, written.size());
    const auto read = Json::parse(written);
    ASSERT_TRUE(read);
    EXPECT_EQ(read->toString(), text.substr(0, text.size() - 1) + "\xEF\xBF\xBD");
}

TEST(Json, ReadsEscapesAndSurrogatePairs)
{
    EXPECT_EQ(stringOf(R"("\u00e9\ud83d\ude00\/\b\f")"), "é😀/\b\f");
    // A lone surrogate is not text: it reads as U+FFFD.
    EXPECT_EQ(stringOf(R"("a\ud800b")"), "a\xEF\xBF\xBD" "b");
    EXPECT_EQ(stringOf(R"("\udc00\u0041")"), "\xEF\xBF\xBD" "A");
    // A high surrogate before an escape that is no low one: the escape is
    // read again on its own.
    EXPECT_EQ(stringOf(R"("\ud800\u0041")"), "\xEF\xBF\xBD" "A");
    EXPECT_EQ(stringOf(R"("\ud800\ud83d\ude00")"), "\xEF\xBF\xBD" "😀");
    EXPECT_FALSE(stringOf(R"("\ud800\u12")"));
}

TEST(Json, WritesInvalidUtf8AsReplacement)
{
    EXPECT_EQ(stringOf(Json::write(Json::Value(std::string("a\xFF" "b\xC3")))), "a\xEF\xBF\xBD" "b\xEF\xBF\xBD");
    // Two keys that differ in such bytes only come out alike.
    EXPECT_EQ(Json::write(Json::Object{{"a\xFF", 1}, {"a\xFE", 2}}), "{\"a\xEF\xBF\xBD\":1,\"a\xEF\xBF\xBD\":2}");
}

TEST(Json, RefusesWhatIsNotJson)
{
    for (const char* text : {"", " ", "{", "[1,]", "[1 2]", "{\"a\":1,}", "{\"a\" 1}", "01", "-01", "1.", ".5", "1.e3", "-", "1e",
                             "1e+", "+1", "nan", "Infinity", "0x10", "\"\x01\"", "\"\\x\"", "\"\xFF\"", "\"\xC3\"", "[1] 2",
                             "tru", "{1:2}", "1e999", "-1e999", "\"\\u12\"", "\"abc"}) {
        std::string error;
        EXPECT_FALSE(Json::parse(text, &error)) << text;
        EXPECT_FALSE(error.empty()) << text;
    }
}

TEST(Json, SaysWhereTheTextStopsBeingJson)
{
    EXPECT_EQ(errorOf("[1, x]"), "an invalid value at byte 4");
    EXPECT_EQ(errorOf("  [1] 2"), "text after the value at byte 6");
    EXPECT_EQ(errorOf("[1,   "), "the text ends before a value at byte 6");
    EXPECT_EQ(errorOf(R"("ab\q")"), "an invalid escape at byte 3");
    EXPECT_EQ(errorOf(R"("ab\u12G4")"), "an invalid \\u escape at byte 3");
    EXPECT_EQ(errorOf("[\"a\x01\"]"), "a control character in a string at byte 3");
    EXPECT_EQ(errorOf("[\"a\xFF\"]"), "a string that is not UTF-8 at byte 3");
    EXPECT_EQ(errorOf("{\"a\" 1}"), "an object key without : at byte 5");
    EXPECT_EQ(errorOf("[2, 1e999]"), "a number out of range at byte 4");
}

TEST(Json, ReadsANumberBelowTheSmallestDoubleAsZero)
{
    const auto tiny = Json::parse("1e-400");
    ASSERT_TRUE(tiny);
    EXPECT_EQ(tiny->toNumber(-1), 0.0);
    const auto negative = Json::parse("-1e-400");
    ASSERT_TRUE(negative);
    EXPECT_TRUE(std::signbit(negative->toNumber()));
    const auto digits = Json::parse("0." + std::string(400, '0') + "1");
    ASSERT_TRUE(digits);
    EXPECT_EQ(digits->toNumber(-1), 0.0);
    EXPECT_FALSE(Json::parse("1" + std::string(400, '0')));
    EXPECT_FALSE(Json::parse("0.001e400"));
    // More digits than any exponent clamp: 1e99998 is too large, 1e-100000 too small.
    EXPECT_FALSE(Json::parse("0." + std::string(100001, '0') + "1e200000"));
    const auto longTiny = Json::parse("1" + std::string(100000, '0') + "e-200000");
    ASSERT_TRUE(longTiny);
    EXPECT_EQ(longTiny->toNumber(-1), 0.0);
}

TEST(Json, LimitsTheNesting)
{
    EXPECT_TRUE(Json::parse(nested(Json::MaxDepth)));
    EXPECT_TRUE(Json::parse(nested(Json::MaxDepth, "1")));
    EXPECT_TRUE(Json::parse(nested(Json::MaxDepth - 1, "{\"a\":1}")));
    EXPECT_EQ(errorOf(nested(Json::MaxDepth + 1)), "nesting too deep at byte " + std::to_string(Json::MaxDepth));
    EXPECT_FALSE(Json::parse(nested(Json::MaxDepth, "{}")));
}

TEST(Json, LimitsTheValues)
{
    // The array is a value too.
    std::string fits = "[0";
    for (std::size_t i = 2; i < Json::MaxValues; ++i) fits += ",0";
    fits += ']';
    EXPECT_TRUE(Json::parse(fits));
    fits.insert(1, "0,");
    EXPECT_NE(errorOf(fits).find("too many values"), std::string::npos);
}

TEST(Json, TheLastOfMembersWithOneKeyCounts)
{
    // As in QJsonDocument, which reads the same text at the other end.
    const auto value = Json::parse(R"({"a": 1, "b": 2, "a": 3})");
    ASSERT_TRUE(value);
    EXPECT_EQ((*value)["a"].toInteger(), 3);
    EXPECT_EQ(value->toObject().size(), 3u);
}

TEST(Json, EqualityIsOfTypeValueAndOrder)
{
    EXPECT_EQ(Json::Value(1), Json::Value(1.0));
    EXPECT_NE(Json::Value(1), Json::Value("1"));
    EXPECT_NE(Json::Value(std::numeric_limits<double>::quiet_NaN()), Json::Value(std::numeric_limits<double>::quiet_NaN()));
    EXPECT_NE((Json::Value(Json::Object{{"a", 1}, {"b", 2}})), (Json::Value(Json::Object{{"b", 2}, {"a", 1}})));
}
