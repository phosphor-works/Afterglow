// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Json.h>
#include <AfterglowProto/Text.h>

#include <algorithm>
#include <charconv>
#include <cmath>

namespace AfterglowProto::Json {

namespace {

const std::string EmptyString;
const Json::Array EmptyArray;
const Json::Object EmptyObject;
const Value NullValue;

constexpr std::string_view Replacement = "\xEF\xBF\xBD"; // U+FFFD

class Reader
{
public:
    explicit Reader(std::string_view text)
        : m_text(text)
    {
    }

    std::optional<Value> document(std::string* error)
    {
        auto value = read(0);
        if (value) {
            skipSpace();
            if (m_at != m_text.size()) {
                fail("text after the value");
                value.reset();
            }
        }
        if (!value && error) *error = m_error + " at byte " + std::to_string(m_errorAt);
        return value;
    }

private:
    /// The first failure is the one reported, where it happened.
    bool fail(const char* why)
    {
        if (m_error.empty()) {
            m_error = why;
            m_errorAt = m_at;
        }
        return false;
    }

    void skipSpace()
    {
        while (m_at < m_text.size()
               && (m_text[m_at] == ' ' || m_text[m_at] == '\t' || m_text[m_at] == '\n' || m_text[m_at] == '\r'))
            ++m_at;
    }

    bool literal(std::string_view word)
    {
        if (m_text.substr(m_at, word.size()) != word) return fail("an unknown literal");
        m_at += word.size();
        return true;
    }

    /// A value inside @p depth containers.
    std::optional<Value> read(int depth)
    {
        skipSpace();
        if (++m_values > MaxValues) {
            fail("too many values");
            return std::nullopt;
        }
        if (m_at >= m_text.size()) {
            fail("the text ends before a value");
            return std::nullopt;
        }
        switch (m_text[m_at]) {
        case 'n':
            if (literal("null")) return Value();
            return std::nullopt;
        case 't':
            if (literal("true")) return Value(true);
            return std::nullopt;
        case 'f':
            if (literal("false")) return Value(false);
            return std::nullopt;
        case '"': {
            std::string s;
            if (string(s)) return Value(std::move(s));
            return std::nullopt;
        }
        case '[':
        case '{':
            if (depth >= MaxDepth) {
                fail("nesting too deep");
                return std::nullopt;
            }
            return m_text[m_at] == '[' ? array(depth + 1) : object(depth + 1);
        default:
            return number();
        }
    }

    std::optional<Value> number()
    {
        // JSON's grammar, checked before from_chars, which also takes "inf",
        // "nan", leading zeros, "1." and ".5". Floating-point from_chars
        // needs libstdc++ 11 or libc++ 20. strtod would follow LC_NUMERIC,
        // which a Qt application sets from the user's locale.
        const std::size_t start = m_at;
        auto digits = [this] {
            const std::size_t from = m_at;
            while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9') ++m_at;
            return m_at > from;
        };
        const bool negative = m_at < m_text.size() && m_text[m_at] == '-';
        if (negative) ++m_at;
        const std::size_t intStart = m_at;
        if (m_at < m_text.size() && m_text[m_at] == '0') {
            ++m_at;
        } else if (!digits()) {
            fail("an invalid value");
            return std::nullopt;
        }
        const std::size_t intEnd = m_at;
        std::size_t fracStart = m_at, fracEnd = m_at;
        if (m_at < m_text.size() && m_text[m_at] == '.') {
            ++m_at;
            fracStart = m_at;
            if (!digits()) {
                fail("a number with no digits after the point");
                return std::nullopt;
            }
            fracEnd = m_at;
        }
        long exponent = 0;
        if (m_at < m_text.size() && (m_text[m_at] == 'e' || m_text[m_at] == 'E')) {
            ++m_at;
            const bool down = m_at < m_text.size() && m_text[m_at] == '-';
            if (m_at < m_text.size() && (m_text[m_at] == '+' || m_text[m_at] == '-')) ++m_at;
            const std::size_t from = m_at;
            if (!digits()) {
                fail("a number with no digits in its exponent");
                return std::nullopt;
            }
            // Past the range of a double for any count of digits that this
            // text can hold, so that underflows() still tells small from large.
            const long cap = long(m_text.size()) + 400;
            for (std::size_t i = from; i < m_at; ++i) exponent = std::min(exponent * 10 + (m_text[i] - '0'), cap);
            if (down) exponent = -exponent;
        }
        double value = 0;
        const auto [end, ec] = std::from_chars(m_text.data() + start, m_text.data() + m_at, value);
        if (ec == std::errc::result_out_of_range && underflows(intStart, intEnd, fracStart, fracEnd, exponent)) {
            // Below the smallest double: the nearest one is zero.
            return Value(negative ? -0.0 : 0.0);
        }
        if (ec != std::errc() || end != m_text.data() + m_at || !std::isfinite(value)) {
            m_at = start;
            fail("a number out of range");
            return std::nullopt;
        }
        return Value(value);
    }

    /// Whether the number of these digits and @p exponent is below 1, so
    /// that out of range means too small rather than too large.
    bool underflows(std::size_t intStart, std::size_t intEnd, std::size_t fracStart, std::size_t fracEnd, long exponent) const
    {
        long magnitude = 0; // the power of ten of the first digit that is not 0
        const auto isZero = [](char c) { return c == '0'; };
        const auto digit = std::find_if_not(m_text.begin() + std::ptrdiff_t(intStart), m_text.begin() + std::ptrdiff_t(intEnd), isZero);
        if (digit != m_text.begin() + std::ptrdiff_t(intEnd)) {
            magnitude = long(m_text.begin() + std::ptrdiff_t(intEnd) - digit) - 1;
        } else {
            const auto fraction = std::find_if_not(m_text.begin() + std::ptrdiff_t(fracStart),
                                                   m_text.begin() + std::ptrdiff_t(fracEnd), isZero);
            magnitude = -long(fraction - (m_text.begin() + std::ptrdiff_t(fracStart))) - 1;
        }
        return magnitude + exponent < 0;
    }

    /// The four hex digits at m_at, which it then passes. Nothing, with
    /// m_at unchanged, when they are not.
    std::optional<char32_t> hex4()
    {
        if (m_text.size() - m_at < 4) return std::nullopt;
        char32_t v = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const char c = m_text[m_at + i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= char32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= char32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= char32_t(c - 'A' + 10);
            else return std::nullopt;
        }
        m_at += 4;
        return v;
    }

    bool string(std::string& out)
    {
        ++m_at; // the opening quote
        while (true) {
            // Text that needs no decoding goes in at once. Valid UTF-8
            // passes through, anything else is refused below.
            const std::size_t from = m_at;
            while (m_at < m_text.size()) {
                const auto c = static_cast<unsigned char>(m_text[m_at]);
                if (c == '"' || c == '\\' || c < 0x20) break;
                if (c < 0x80) {
                    ++m_at;
                    continue;
                }
                char32_t cp = 0;
                if (!nextUtf8(m_text, m_at, cp)) break;
            }
            out.append(m_text.substr(from, m_at - from));
            if (m_at >= m_text.size()) return fail("an unterminated string");
            const auto c = static_cast<unsigned char>(m_text[m_at]);
            if (c == '"') {
                ++m_at;
                return true;
            }
            if (c < 0x20) return fail("a control character in a string");
            if (c != '\\') return fail("a string that is not UTF-8");
            // A bad escape is reported at its backslash.
            const std::size_t escape = m_at;
            const auto invalid = [this, escape](const char* why) {
                m_at = escape;
                return fail(why);
            };
            if (++m_at >= m_text.size()) return invalid("an unterminated string");
            const char e = m_text[m_at++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto cp = hex4();
                if (!cp) return invalid("an invalid \\u escape");
                if (*cp >= 0xD800 && *cp <= 0xDBFF && m_text.substr(m_at, 2) == "\\u") {
                    const std::size_t back = m_at;
                    m_at += 2;
                    const auto low = hex4();
                    if (low && *low >= 0xDC00 && *low <= 0xDFFF) {
                        *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*low - 0xDC00);
                    } else {
                        m_at = back; // the next escape is read on its own
                    }
                }
                appendUtf8(out, *cp >= 0xD800 && *cp <= 0xDFFF ? char32_t(0xFFFD) : *cp);
                break;
            }
            default:
                return invalid("an invalid escape");
            }
        }
    }

    /// The members of an array that is inside @p depth containers, itself
    /// included.
    std::optional<Value> array(int depth)
    {
        ++m_at;
        Json::Array items;
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == ']') {
            ++m_at;
            return Value(std::move(items));
        }
        while (true) {
            auto item = read(depth);
            if (!item) return std::nullopt;
            items.push_back(std::move(*item));
            skipSpace();
            if (m_at < m_text.size() && m_text[m_at] == ',') {
                ++m_at;
                continue;
            }
            if (m_at < m_text.size() && m_text[m_at] == ']') {
                ++m_at;
                return Value(std::move(items));
            }
            fail("an array without , or ]");
            return std::nullopt;
        }
    }

    std::optional<Value> object(int depth)
    {
        ++m_at;
        Json::Object members;
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == '}') {
            ++m_at;
            return Value(std::move(members));
        }
        while (true) {
            skipSpace();
            std::string key;
            if (m_at >= m_text.size() || m_text[m_at] != '"') {
                fail("an object key that is not a string");
                return std::nullopt;
            }
            if (!string(key)) return std::nullopt;
            skipSpace();
            if (m_at >= m_text.size() || m_text[m_at] != ':') {
                fail("an object key without :");
                return std::nullopt;
            }
            ++m_at;
            auto value = read(depth);
            if (!value) return std::nullopt;
            members.emplace_back(std::move(key), std::move(*value));
            skipSpace();
            if (m_at < m_text.size() && m_text[m_at] == ',') {
                ++m_at;
                continue;
            }
            if (m_at < m_text.size() && m_text[m_at] == '}') {
                ++m_at;
                return Value(std::move(members));
            }
            fail("an object without , or }");
            return std::nullopt;
        }
    }

    std::string_view m_text;
    std::size_t m_at = 0;
    std::size_t m_values = 0;
    std::string m_error;
    std::size_t m_errorAt = 0;
};

void writeValue(std::string& out, const Value& value)
{
    switch (value.type()) {
    case Value::Type::Null:
        out += "null";
        break;
    case Value::Type::Bool:
        out += value.toBool() ? "true" : "false";
        break;
    case Value::Type::Number: {
        const double d = value.toNumber();
        if (!std::isfinite(d)) {
            out += "null"; // JSON has no infinity or NaN
            break;
        }
        // The longest double, "-2.2250738585072014e-308", has 24 bytes.
        char buffer[32];
        // Whole numbers as integers: the shortest form of 1e15 is "1e+15".
        const auto [end, ec] = std::abs(d) < MaxExactInteger && d == std::floor(d)
            ? std::to_chars(buffer, buffer + sizeof buffer, std::int64_t(d))
            : std::to_chars(buffer, buffer + sizeof buffer, d);
        // The buffer holds any number, and the output stays JSON regardless.
        out += ec == std::errc() ? std::string_view(buffer, std::size_t(end - buffer)) : std::string_view("null");
        break;
    }
    case Value::Type::String:
        appendString(out, value.toString());
        break;
    case Value::Type::Array: {
        out += '[';
        bool first = true;
        for (const Value& item : value.toArray()) {
            if (!first) out += ',';
            first = false;
            writeValue(out, item);
        }
        out += ']';
        break;
    }
    case Value::Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& [key, item] : value.toObject()) {
            if (!first) out += ',';
            first = false;
            appendString(out, key);
            out += ':';
            writeValue(out, item);
        }
        out += '}';
        break;
    }
    }
}

/// The escape of @p cp in a JSON string, or empty when it goes as it is.
/// The one rule of appendString() and nextStringPiece().
std::string_view escapeOf(char32_t cp, char (&buffer)[6])
{
    static constexpr char Hex[] = "0123456789abcdef";
    switch (cp) {
    case '"': return "\\\"";
    case '\\': return "\\\\";
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    default:
        if (cp >= 0x20) return {};
        buffer[0] = '\\';
        buffer[1] = 'u';
        buffer[2] = '0';
        buffer[3] = '0';
        buffer[4] = Hex[cp >> 4];
        buffer[5] = Hex[cp & 0xF];
        return {buffer, sizeof buffer};
    }
}

} // namespace

bool Value::toBool(bool fallback) const
{
    return isBool() ? std::get<bool>(m_data) : fallback;
}

double Value::toNumber(double fallback) const
{
    return isNumber() ? std::get<double>(m_data) : fallback;
}

std::optional<std::int64_t> Value::toInteger() const
{
    if (!isNumber()) return std::nullopt;
    const double d = std::get<double>(m_data);
    if (d != std::floor(d) || !(std::abs(d) < MaxExactInteger)) return std::nullopt;
    return std::int64_t(d);
}

const std::string& Value::toString() const
{
    return isString() ? std::get<std::string>(m_data) : EmptyString;
}

const Json::Array& Value::toArray() const
{
    return isArray() ? std::get<Json::Array>(m_data) : EmptyArray;
}

const Json::Object& Value::toObject() const
{
    return isObject() ? std::get<Json::Object>(m_data) : EmptyObject;
}

const Value* Value::find(std::string_view key) const
{
    const Json::Object& members = toObject();
    const auto it = std::find_if(members.rbegin(), members.rend(), [key](const auto& member) { return member.first == key; });
    return it == members.rend() ? nullptr : &it->second;
}

Value* Value::find(std::string_view key)
{
    return const_cast<Value*>(std::as_const(*this).find(key));
}

const Value& Value::operator[](std::string_view key) const
{
    const Value* value = find(key);
    return value ? *value : NullValue;
}

std::optional<Value> parse(std::string_view text, std::string* error)
{
    return Reader(text).document(error);
}

std::string write(const Value& value)
{
    std::string out;
    writeValue(out, value);
    return out;
}

void appendString(std::string& out, std::string_view text)
{
    out += '"';
    std::size_t plain = 0; // the start of the text that goes out as it is
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        char32_t cp = 0;
        char buffer[6];
        std::string_view piece;
        if (!nextUtf8(text, i, cp)) {
            i = start + 1;
            piece = Replacement;
        } else {
            piece = escapeOf(cp, buffer);
        }
        if (piece.empty()) continue;
        out.append(text.substr(plain, start - plain));
        out += piece;
        plain = i;
    }
    out.append(text.substr(plain));
    out += '"';
}

std::size_t nextStringPiece(std::string_view text, std::size_t& at)
{
    if (at >= text.size()) return 0;
    const std::size_t start = at;
    char32_t cp = 0;
    if (!nextUtf8(text, at, cp)) {
        at = start + 1;
        return Replacement.size();
    }
    char buffer[6];
    const std::string_view escaped = escapeOf(cp, buffer);
    return escaped.empty() ? at - start : escaped.size();
}

} // namespace AfterglowProto::Json
