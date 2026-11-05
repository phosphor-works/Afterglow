// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Json.h>
#include <AfterglowProto/Text.h>

#include <charconv>
#include <cmath>

namespace AfterglowProto::Json {

namespace {

const std::string EmptyString;
const Json::Array EmptyArray;
const Json::Object EmptyObject;
const Value NullValue;

void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

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
        skipSpace();
        if (value && m_at != m_text.size()) {
            fail("text after the value");
            value.reset();
        }
        if (!value && error) *error = m_error + " at byte " + std::to_string(m_at);
        return value;
    }

private:
    bool fail(const char* why)
    {
        if (m_error.empty()) m_error = why;
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

    std::optional<Value> read(int depth)
    {
        if (depth > MaxDepth) {
            fail("nesting too deep");
            return std::nullopt;
        }
        skipSpace();
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
            return array(depth);
        case '{':
            return object(depth);
        default:
            return number();
        }
    }

    std::optional<Value> number()
    {
        // JSON's grammar, checked before from_chars, which also takes "inf",
        // "nan", hexadecimal and a leading "+".
        const std::size_t start = m_at;
        auto digits = [this] {
            const std::size_t from = m_at;
            while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9') ++m_at;
            return m_at > from;
        };
        if (m_at < m_text.size() && m_text[m_at] == '-') ++m_at;
        if (m_at < m_text.size() && m_text[m_at] == '0') {
            ++m_at;
        } else if (!digits()) {
            fail("an invalid value");
            return std::nullopt;
        }
        if (m_at < m_text.size() && m_text[m_at] == '.') {
            ++m_at;
            if (!digits()) {
                fail("a number with no digits after the point");
                return std::nullopt;
            }
        }
        if (m_at < m_text.size() && (m_text[m_at] == 'e' || m_text[m_at] == 'E')) {
            ++m_at;
            if (m_at < m_text.size() && (m_text[m_at] == '+' || m_text[m_at] == '-')) ++m_at;
            if (!digits()) {
                fail("a number with no digits in its exponent");
                return std::nullopt;
            }
        }
        double value = 0;
        const auto [end, ec] = std::from_chars(m_text.data() + start, m_text.data() + m_at, value);
        if (ec != std::errc() || end != m_text.data() + m_at || !std::isfinite(value)) {
            fail("a number out of range");
            return std::nullopt;
        }
        return Value(value);
    }

    std::optional<char32_t> hex4()
    {
        if (m_text.size() - m_at < 4) return std::nullopt;
        char32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_at++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= char32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= char32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= char32_t(c - 'A' + 10);
            else return std::nullopt;
        }
        return v;
    }

    bool string(std::string& out)
    {
        ++m_at; // the opening quote
        while (true) {
            if (m_at >= m_text.size()) return fail("an unterminated string");
            const auto c = static_cast<unsigned char>(m_text[m_at]);
            if (c == '"') {
                ++m_at;
                return true;
            }
            if (c < 0x20) return fail("a control character in a string");
            if (c != '\\') {
                // Valid UTF-8 passes through, anything else is refused.
                std::size_t next = m_at;
                char32_t cp = 0;
                if (!nextUtf8(m_text, next, cp)) return fail("a string that is not UTF-8");
                out.append(m_text.substr(m_at, next - m_at));
                m_at = next;
                continue;
            }
            if (++m_at >= m_text.size()) return fail("an unterminated string");
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
                if (!cp) return fail("an invalid \\u escape");
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
                return fail("an invalid escape");
            }
        }
    }

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
            auto item = read(depth + 1);
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
            auto value = read(depth + 1);
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
    std::string m_error;
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
        char buffer[32];
        // Whole numbers as integers: the shortest form of 1e21 is "1e+21".
        const auto [end, ec] = std::abs(d) < 9007199254740992.0 && d == std::floor(d)
            ? std::to_chars(buffer, buffer + sizeof buffer, std::int64_t(d))
            : std::to_chars(buffer, buffer + sizeof buffer, d);
        if (ec == std::errc()) out.append(buffer, end);
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

/// Calls @p emit with each piece of @p text as appendString() writes it.
template<typename Emit>
void escape(std::string_view text, Emit&& emit)
{
    static constexpr char Hex[] = "0123456789abcdef";
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        char32_t cp = 0;
        if (!nextUtf8(text, i, cp)) {
            emit(std::string_view("\xEF\xBF\xBD")); // U+FFFD for the byte
            i = start + 1;
            continue;
        }
        switch (cp) {
        case '"': emit(std::string_view("\\\"")); break;
        case '\\': emit(std::string_view("\\\\")); break;
        case '\n': emit(std::string_view("\\n")); break;
        case '\r': emit(std::string_view("\\r")); break;
        case '\t': emit(std::string_view("\\t")); break;
        default:
            if (cp < 0x20) {
                const char escaped[] = {'\\', 'u', '0', '0', Hex[cp >> 4], Hex[cp & 0xF]};
                emit(std::string_view(escaped, sizeof escaped));
            } else {
                emit(text.substr(start, i - start));
            }
        }
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
    if (d != std::floor(d) || std::abs(d) > 9007199254740992.0) return std::nullopt;
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
    for (const auto& [name, value] : toObject()) {
        if (name == key) return &value;
    }
    return nullptr;
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
    escape(text, [&out](std::string_view piece) { out += piece; });
    out += '"';
}

std::size_t stringSize(std::string_view text)
{
    std::size_t size = 2;
    escape(text, [&size](std::string_view piece) { size += piece.size(); });
    return size;
}

} // namespace AfterglowProto::Json
