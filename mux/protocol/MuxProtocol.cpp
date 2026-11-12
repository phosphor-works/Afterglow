// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxProtocol.h"

#include <AfterglowProto/Text.h>

#include <algorithm>
#include <cmath>

namespace AfterglowMux {

namespace Json = AfterglowProto::Json;

namespace {

// Each key is read once, and a missing key or a value of the wrong type is
// an error, not a default: the two ends agree on ProtocolVersion, which
// changes with the keys.
class Reader
{
public:
    /// @p prefix goes before each key in the error: "terminal." for the keys
    /// of the terminal object.
    Reader(const Json::Value& object, std::string* error, std::string prefix = {})
        : m_object(object)
        , m_error(error)
        , m_prefix(std::move(prefix))
    {
    }

    bool failed() const { return m_failed; }

    /// A reader of the object at @p key, whose errors are this reader's: the
    /// first error of either names its key.
    Reader nested(const Json::Value& object, std::string_view key) const
    {
        return Reader(object, m_failed ? nullptr : m_error, m_prefix + std::string(key) + ".");
    }
    /// Takes the failure of a reader from nested().
    bool absorb(const Reader& inner)
    {
        m_failed = m_failed || inner.m_failed;
        return !inner.m_failed;
    }

    /// The value of @p key, or nullptr. A key that no reader looks up is
    /// an error of noOtherKeys().
    const Json::Value* lookup(std::string_view key)
    {
        m_seen.emplace_back(key);
        return m_object.find(key);
    }
    const Json::Value* get(std::string_view key, Json::Value::Type type)
    {
        const Json::Value* value = lookup(key);
        if (!value) {
            fail(key, "is missing");
            return nullptr;
        }
        if (value->type() != type) {
            fail(key, "has the wrong type");
            return nullptr;
        }
        return value;
    }
    std::optional<std::int64_t> integer(std::string_view key, std::int64_t min, std::int64_t max)
    {
        const Json::Value* value = get(key, Json::Value::Type::Number);
        if (!value) return std::nullopt;
        const auto n = value->toInteger();
        if (!n || *n < min || *n > max) {
            fail(key, "is out of range");
            return std::nullopt;
        }
        return n;
    }
    bool boolean(std::string_view key, bool& out)
    {
        const Json::Value* value = get(key, Json::Value::Type::Bool);
        if (value) out = value->toBool();
        return value;
    }
    bool string(std::string_view key, std::string& out)
    {
        const Json::Value* value = get(key, Json::Value::Type::String);
        if (value) out = value->toString();
        return value;
    }
    bool strings(std::string_view key, std::vector<std::string>& out)
    {
        const Json::Value* value = get(key, Json::Value::Type::Array);
        if (!value) return false;
        for (const Json::Value& item : value->toArray()) {
            if (!item.isString()) return fail(key, "has an item that is not a string");
            out.push_back(item.toString());
        }
        return true;
    }

    /// A key of another protocol version, or of none, is an error: an end
    /// that dropped it would run the session without it.
    bool noOtherKeys()
    {
        for (const auto& [key, value] : m_object.toObject()) {
            if (std::find(m_seen.begin(), m_seen.end(), key) == m_seen.end()) return fail(key, "is not a key of this protocol");
        }
        return true;
    }

    /// Always false, to return from the readers.
    bool fail(std::string_view key, std::string_view what)
    {
        if (!m_failed && m_error) *m_error = "'" + m_prefix + std::string(key) + "' " + std::string(what);
        m_failed = true;
        return false;
    }

private:
    const Json::Value& m_object;
    std::string* m_error;
    std::string m_prefix;
    std::vector<std::string> m_seen;
    bool m_failed = false;
};

Json::Value color(const AfterglowVt::Rgb& c)
{
    return Json::Array{int(c.r), int(c.g), int(c.b)};
}

Json::Value color(const std::optional<AfterglowVt::Rgb>& c)
{
    return c ? color(*c) : Json::Value();
}

std::optional<AfterglowVt::Rgb> readColor(const Json::Value& value)
{
    if (!value.isArray() || value.toArray().size() != 3) return std::nullopt;
    std::uint8_t parts[3];
    for (std::size_t i = 0; i < 3; ++i) {
        const auto n = value.toArray()[i].toInteger();
        if (!n || *n < 0 || *n > 255) return std::nullopt;
        parts[i] = std::uint8_t(*n);
    }
    return AfterglowVt::Rgb{parts[0], parts[1], parts[2]};
}

/// A color that may be null (unset).
bool optionalColor(Reader& reader, std::string_view key, std::optional<AfterglowVt::Rgb>& out)
{
    const Json::Value* value = reader.lookup(key);
    if (!value) return reader.fail(key, "is missing");
    if (value->isNull()) {
        out.reset();
        return true;
    }
    out = readColor(*value);
    return out ? true : reader.fail(key, "is not a color");
}

bool requiredColor(Reader& reader, std::string_view key, AfterglowVt::Rgb& out)
{
    const Json::Value* value = reader.lookup(key);
    const auto c = value ? readColor(*value) : std::nullopt;
    if (!c) return reader.fail(key, "is not a color");
    out = *c;
    return true;
}

Json::Value toJson(const AfterglowVt::TerminalOptions& t)
{
    Json::Array palette;
    for (const auto& c : t.basePalette) palette.push_back(color(c));
    std::string boundaries;
    for (const char32_t cp : t.wordBoundaries) AfterglowProto::appendUtf8(boundaries, cp);
    return Json::Object{
        {"scrollback", t.maxScrollback},
        {"foreground", color(t.defaultForeground)},
        {"background", color(t.defaultBackground)},
        {"palette", std::move(palette)},
        {"cursorColor", color(t.cursorColor)},
        {"cursorTextColor", color(t.cursorTextColor)},
        {"selectionForeground", color(t.selectionForeground)},
        {"selectionBackground", color(t.selectionBackground)},
        {"boldIsBright", t.boldIsBright},
        {"minimumContrast", t.minimumContrast},
        {"wordBoundaries", std::move(boundaries)},
        {"ambiguousWide", t.ambiguousWide},
        {"bidi", int(t.bidi)},
        {"cursorShape", int(t.cursorShape)},
        {"cursorBlink", t.cursorBlink},
        {"allowScreenReadback", t.allowScreenReadback},
        {"graphicsQuota", t.graphicsQuota},
        {"graphicsFileTransfer", t.graphicsFileTransfer},
    };
}

bool readTerminal(Reader& outer, const Json::Value& object, AfterglowVt::TerminalOptions& t)
{
    if (!object.isObject()) return outer.fail("terminal", "is not an object");
    Reader r = outer.nested(object, "terminal");
    if (const auto n = r.integer("scrollback", 0, MaxScrollback)) t.maxScrollback = std::size_t(*n);
    requiredColor(r, "foreground", t.defaultForeground);
    requiredColor(r, "background", t.defaultBackground);
    if (const Json::Value* palette = r.get("palette", Json::Value::Type::Array)) {
        if (palette->toArray().size() != t.basePalette.size()) {
            r.fail("palette", "does not have 16 colors");
        } else {
            for (std::size_t i = 0; i < t.basePalette.size(); ++i) {
                const auto c = readColor(palette->toArray()[i]);
                if (!c) {
                    r.fail("palette", "has an item that is not a color");
                    break;
                }
                t.basePalette[i] = *c;
            }
        }
    }
    optionalColor(r, "cursorColor", t.cursorColor);
    optionalColor(r, "cursorTextColor", t.cursorTextColor);
    optionalColor(r, "selectionForeground", t.selectionForeground);
    optionalColor(r, "selectionBackground", t.selectionBackground);
    r.boolean("boldIsBright", t.boldIsBright);
    if (const Json::Value* contrast = r.get("minimumContrast", Json::Value::Type::Number)) {
        const double v = contrast->toNumber();
        if (!std::isfinite(v) || v < MinContrast || v > MaxContrast) r.fail("minimumContrast", "is out of range");
        else t.minimumContrast = v;
    }
    std::string boundaries;
    if (r.string("wordBoundaries", boundaries)) {
        t.wordBoundaries.clear();
        std::size_t i = 0;
        char32_t cp = 0;
        while (i < boundaries.size()) {
            if (!AfterglowProto::nextUtf8(boundaries, i, cp)) {
                r.fail("wordBoundaries", "is not UTF-8");
                break;
            }
            t.wordBoundaries.push_back(cp);
        }
    }
    r.boolean("ambiguousWide", t.ambiguousWide);
    if (const auto n = r.integer("bidi", 0, int(AfterglowVt::BidiMode::RightToLeft))) t.bidi = AfterglowVt::BidiMode(*n);
    if (const auto n = r.integer("cursorShape", 0, int(AfterglowVt::CursorShape::Bar))) t.cursorShape = AfterglowVt::CursorShape(*n);
    r.boolean("cursorBlink", t.cursorBlink);
    r.boolean("allowScreenReadback", t.allowScreenReadback);
    if (const auto n = r.integer("graphicsQuota", 0, MaxGraphicsQuota)) t.graphicsQuota = std::size_t(*n);
    r.boolean("graphicsFileTransfer", t.graphicsFileTransfer);
    r.noOtherKeys();
    return outer.absorb(r);
}

/// A name with '=' or a NUL would set another variable.
bool validVariableName(std::string_view name)
{
    return !name.empty() && name.find_first_of(std::string_view("=\0", 2)) == std::string_view::npos;
}

/// [name, value] pairs.
bool readEnvironment(Reader& r, std::string_view key, std::vector<std::pair<std::string, std::string>>& out)
{
    const Json::Value* environment = r.get(key, Json::Value::Type::Array);
    if (!environment) return false;
    for (const Json::Value& entry : environment->toArray()) {
        const bool pair = entry.isArray() && entry.toArray().size() == 2 && entry.toArray()[0].isString()
            && entry.toArray()[1].isString();
        if (!pair || !validVariableName(entry.toArray()[0].toString())) return r.fail(key, "has an item that is not a [name, value] pair");
        out.emplace_back(entry.toArray()[0].toString(), entry.toArray()[1].toString());
    }
    return true;
}

Json::Value environmentJson(const std::vector<std::pair<std::string, std::string>>& environment)
{
    Json::Array out;
    for (const auto& [name, value] : environment) out.push_back(Json::Array{name, value});
    return out;
}

bool readLaunch(Reader& outer, const Json::Value& object, Launch& launch)
{
    if (!object.isObject()) return outer.fail("launch", "is not an object");
    Reader r = outer.nested(object, "launch");
    r.strings("argv", launch.argv);
    r.string("cwd", launch.workingDirectory);
    readEnvironment(r, "environment", launch.environment);
    if (r.strings("unset", launch.unset)) {
        for (const std::string& name : launch.unset) {
            if (!validVariableName(name)) {
                r.fail("unset", "has an item that is not a variable name");
                break;
            }
        }
    }
    r.string("launchedProgram", launch.launchedProgram);
    r.noOtherKeys();
    return outer.absorb(r);
}

bool readSize(Reader& outer, const Json::Value& object, PaneSize& size)
{
    if (!object.isObject()) return outer.fail("size", "is not an object");
    Reader r = outer.nested(object, "size");
    if (const auto n = r.integer("cols", 2, 4096)) size.cols = int(*n);
    if (const auto n = r.integer("rows", 1, 1048576 / 2)) size.rows = int(*n);
    if (const auto n = r.integer("cellWidth", 0, 65535)) size.cellWidth = int(*n);
    if (const auto n = r.integer("cellHeight", 0, 65535)) size.cellHeight = int(*n);
    r.noOtherKeys();
    return outer.absorb(r);
}

/// Reads the object @p value whole with @p read, as the one key of an outer
/// reader: the errors name the keys inside it.
template<typename T, typename Read>
std::optional<T> readWhole(const Json::Value& value, std::string_view key, std::string* error, Read read)
{
    T out;
    const Json::Value holder = Json::Object{{std::string(key), value}};
    Reader r(holder, error);
    read(r, *holder.find(key), out);
    if (r.failed()) return std::nullopt;
    return out;
}

} // namespace

bool validSessionName(std::string_view name)
{
    if (name.empty() || name.size() > MaxSessionName || name.front() == '-') return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

Json::Value toJson(const Launch& launch)
{
    return Json::Object{
        {"argv", Json::Array(launch.argv.begin(), launch.argv.end())},
        {"cwd", launch.workingDirectory},
        {"environment", environmentJson(launch.environment)},
        {"unset", Json::Array(launch.unset.begin(), launch.unset.end())},
        {"launchedProgram", launch.launchedProgram},
    };
}

std::optional<Launch> launchFrom(const Json::Value& value, std::string* error)
{
    return readWhole<Launch>(value, "launch", error, readLaunch);
}

Json::Value toJson(const PaneSize& size)
{
    return Json::Object{{"cols", size.cols}, {"rows", size.rows}, {"cellWidth", size.cellWidth}, {"cellHeight", size.cellHeight}};
}

std::optional<PaneSize> paneSizeFrom(const Json::Value& value, std::string* error)
{
    return readWhole<PaneSize>(value, "size", error, readSize);
}

Json::Value toJson(const SessionOptions& o)
{
    return Json::Object{
        {"name", o.name},
        {"environment", environmentJson(o.environment)},
        {"callerEnvironment", o.callerEnvironment},
        {"umask", o.umask},
        {"launch", toJson(o.launch)},
        {"size", toJson(o.size)},
        {"terminal", toJson(o.terminal)},
        {"notificationCapabilities", o.notificationCapabilities},
        {"control", Json::Object{{"enabled", o.control.enabled}, {"allowReadText", o.control.allowReadText}}},
    };
}

std::optional<SessionOptions> sessionOptions(const Json::Value& value, std::string* error)
{
    if (!value.isObject()) {
        if (error) *error = "the session options are not an object";
        return std::nullopt;
    }
    SessionOptions o;
    Reader r(value, error);
    if (r.string("name", o.name) && !o.name.empty() && !validSessionName(o.name)) {
        r.fail("name", std::string("is not ") + SessionNameRule);
    }
    readEnvironment(r, "environment", o.environment);
    r.boolean("callerEnvironment", o.callerEnvironment);
    if (const auto n = r.integer("umask", -1, 0777)) o.umask = int(*n);
    if (const Json::Value* launch = r.lookup("launch")) readLaunch(r, *launch, o.launch);
    else r.fail("launch", "is missing");
    if (const Json::Value* size = r.lookup("size")) readSize(r, *size, o.size);
    else r.fail("size", "is missing");
    if (const Json::Value* terminal = r.lookup("terminal")) readTerminal(r, *terminal, o.terminal);
    else r.fail("terminal", "is missing");
    r.string("notificationCapabilities", o.notificationCapabilities);
    if (const Json::Value* control = r.get("control", Json::Value::Type::Object)) {
        Reader c = r.nested(*control, "control");
        c.boolean("enabled", o.control.enabled);
        c.boolean("allowReadText", o.control.allowReadText);
        c.noOtherKeys();
        r.absorb(c);
    }
    r.noOtherKeys();
    if (r.failed()) return std::nullopt;
    return o;
}

Json::Value toJson(const SessionInfo& info)
{
    return Json::Object{
        {"name", info.name},
        {"attached", info.attached},
        {"panes", info.panes},
        {"jobs", Json::Array(info.jobs.begin(), info.jobs.end())},
    };
}

std::optional<SessionInfo> sessionInfo(const Json::Value& value)
{
    if (!value.isObject()) return std::nullopt;
    SessionInfo info;
    Reader r(value, nullptr);
    r.string("name", info.name);
    r.boolean("attached", info.attached);
    if (const auto n = r.integer("panes", 0, 1 << 20)) info.panes = int(*n);
    r.strings("jobs", info.jobs);
    r.noOtherKeys();
    if (r.failed()) return std::nullopt;
    return info;
}

} // namespace AfterglowMux
