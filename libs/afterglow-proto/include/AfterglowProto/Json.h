// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

/// A small JSON reader and writer for the clients of the library, which have
/// no Qt: the phosphor-ipc wire format of `aglow run` (Ipc.h). Numbers are
/// doubles, as in JavaScript and QJsonValue: an integer past 2^53 loses its
/// low bits. Objects keep their members in order.
namespace AfterglowProto::Json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

/// The largest magnitude below which every whole double is exact. toInteger()
/// and write() treat a whole number as an integer only below it: a double of
/// 2^53 can be the rounding of 2^53 + 1.
inline constexpr double MaxExactInteger = 9007199254740992.0;

class Value
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) { }
    /// Only a bool: a pointer or a number does not become one.
    template<typename T>
        requires std::same_as<T, bool>
    Value(T value)
        : m_data(value)
    {
    }
    Value(double value)
        : m_data(value)
    {
    }
    /// Any integer type but bool and char, as a double: past 2^53 in
    /// magnitude it loses its low bits. A char is text, not a number.
    Value(char) = delete;
    template<std::integral T>
        requires(!std::same_as<T, bool> && !std::same_as<T, char>)
    Value(T value)
        : m_data(double(value))
    {
    }
    Value(std::string value)
        : m_data(std::move(value))
    {
    }
    Value(std::string_view value)
        : m_data(std::string(value))
    {
    }
    /// A null pointer is a null value.
    Value(const char* value)
    {
        if (value) m_data = std::string(value);
    }
    Value(Json::Array value)
        : m_data(std::move(value))
    {
    }
    Value(Json::Object value)
        : m_data(std::move(value))
    {
    }

    Type type() const { return Type(m_data.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isNumber() const { return type() == Type::Number; }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    /// The value, or @p fallback when it has another type.
    bool toBool(bool fallback = false) const;
    double toNumber(double fallback = 0) const;
    /// A whole number below MaxExactInteger in magnitude, else nothing.
    std::optional<std::int64_t> toInteger() const;
    /// Empty for a value of another type.
    const std::string& toString() const;
    const Json::Array& toArray() const;
    const Json::Object& toObject() const;

    /// The member @p key of an object, or nullptr. Of members with the same
    /// key, the last, as in QJsonDocument.
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    /// The member @p key of an object, or a null value.
    const Value& operator[](std::string_view key) const;

    /// The same type and value. Objects compare their members in order, and
    /// a NaN equals nothing, itself included.
    bool operator==(const Value&) const = default;

private:
    using Data = std::variant<std::nullptr_t, bool, double, std::string, Json::Array, Json::Object>;
    // type() reads the index of the alternative as a Type.
    static_assert(std::is_same_v<std::variant_alternative_t<std::size_t(Type::Null), Data>, std::nullptr_t>);
    static_assert(std::is_same_v<std::variant_alternative_t<std::size_t(Type::Bool), Data>, bool>);
    static_assert(std::is_same_v<std::variant_alternative_t<std::size_t(Type::Number), Data>, double>);
    static_assert(std::is_same_v<std::variant_alternative_t<std::size_t(Type::String), Data>, std::string>);
    static_assert(std::variant_size_v<Data> == std::size_t(Type::Object) + 1);

    Data m_data;
};

/// Containers nested deeper than this are refused: the reader recurses.
/// write() recurses too, so it is for values that the program builds or
/// that parse() read.
inline constexpr int MaxDepth = 128;
/// A document of more values than this is refused. A value takes some tens
/// of bytes in memory, more than its text: "[0,0,...]" grows twenty times.
inline constexpr std::size_t MaxValues = 1 << 18;

/// Reads one JSON value, with only white space around it. @p error gets the
/// reason and the byte offset where the text stops being JSON. A string
/// escape of a lone surrogate reads as U+FFFD. A number too small for a
/// double reads as 0, one too large is refused.
std::optional<Value> parse(std::string_view text, std::string* error = nullptr);
/// Compact JSON. Bytes of a string that are not valid UTF-8 are written as
/// U+FFFD, so the output is always valid JSON: two keys that differ only in
/// such bytes come out alike. NaN and infinity are written as null.
std::string write(const Value& value);
/// Appends @p text as a JSON string, quotes included, as write() does.
void appendString(std::string& out, std::string_view text);
/// Advances @p at past the next piece of @p text that appendString() writes
/// on its own, a code point or a byte that is not UTF-8, and returns the
/// bytes that it writes for it. 0, with @p at unchanged, at the end.
std::size_t nextStringPiece(std::string_view text, std::size_t& at);

} // namespace AfterglowProto::Json
