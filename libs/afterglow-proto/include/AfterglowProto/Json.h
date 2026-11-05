// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

/// A small JSON reader and writer for the clients of the library, which have
/// no Qt: the phosphor-ipc wire format of `afterglow run` (Ipc.h). Numbers are
/// doubles, as in JSON. Objects keep their keys in order.
namespace AfterglowProto::Json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

class Value
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) { }
    Value(bool value)
        : m_data(value)
    {
    }
    Value(double value)
        : m_data(value)
    {
    }
    Value(int value)
        : m_data(double(value))
    {
    }
    Value(std::int64_t value)
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
    Value(const char* value)
        : m_data(std::string(value))
    {
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
    /// A whole number in the range of int64_t, else nothing.
    std::optional<std::int64_t> toInteger() const;
    /// Empty for a value of another type.
    const std::string& toString() const;
    const Json::Array& toArray() const;
    const Json::Object& toObject() const;

    /// The member @p key of an object, or nullptr.
    const Value* find(std::string_view key) const;
    /// The member @p key of an object, or a null value.
    const Value& operator[](std::string_view key) const;

    bool operator==(const Value&) const = default;

private:
    std::variant<std::nullptr_t, bool, double, std::string, Json::Array, Json::Object> m_data;
};

/// Nesting deeper than this is refused: the reader recurses.
inline constexpr int MaxDepth = 128;

/// Reads one JSON value, with only white space around it. @p error gets the
/// reason and the byte offset when the text is not JSON. A string escape of
/// a lone surrogate reads as U+FFFD.
std::optional<Value> parse(std::string_view text, std::string* error = nullptr);
/// Compact JSON. Bytes of a string that are not valid UTF-8 are written as
/// U+FFFD, so the output is always valid JSON.
std::string write(const Value& value);
/// Appends @p text as a JSON string, quotes included, as write() does.
void appendString(std::string& out, std::string_view text);
/// The bytes that appendString() writes for @p text.
std::size_t stringSize(std::string_view text);

} // namespace AfterglowProto::Json
