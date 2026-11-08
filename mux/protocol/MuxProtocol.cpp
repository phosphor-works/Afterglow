// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxProtocol.h"

#include <AfterglowProto/Text.h>

#include <algorithm>
#include <cmath>

namespace AfterglowMux {

namespace Json = AfterglowProto::Json;

namespace {

// Each key is read once, and a value of the wrong type is an error, not a
// default: the client and the daemon come from one build.
class Reader
{
public:
    Reader(const Json::Value& object, std::string* error)
        : m_object(object)
        , m_error(error)
    {
    }

    bool failed() const { return m_failed; }

    const Json::Value* get(std::string_view key, Json::Value::Type type)
    {
        const Json::Value* value = m_object.find(key);
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

    /// Always false, to return from the readers.
    bool fail(std::string_view key, std::string_view what)
    {
        if (!m_failed && m_error) *m_error = "'" + std::string(key) + "' " + std::string(what);
        m_failed = true;
        return false;
    }

private:
    const Json::Value& m_object;
    std::string* m_error;
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
bool optionalColor(Reader& reader, const Json::Value& object, std::string_view key, std::optional<AfterglowVt::Rgb>& out)
{
    const Json::Value* value = object.find(key);
    if (!value) return reader.fail(key, "is missing");
    if (value->isNull()) {
        out.reset();
        return true;
    }
    out = readColor(*value);
    return out ? true : reader.fail(key, "is not a color");
}

bool requiredColor(Reader& reader, const Json::Value& object, std::string_view key, AfterglowVt::Rgb& out)
{
    const Json::Value* value = object.find(key);
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
        {"graphicsQuota", t.graphicsQuota},
        {"graphicsFileTransfer", t.graphicsFileTransfer},
    };
}

bool readTerminal(Reader& outer, const Json::Value& object, AfterglowVt::TerminalOptions& t, std::string* error)
{
    if (!object.isObject()) return outer.fail("terminal", "is not an object");
    Reader r(object, error);
    if (const auto n = r.integer("scrollback", 0, 1000000)) t.maxScrollback = std::size_t(*n);
    requiredColor(r, object, "foreground", t.defaultForeground);
    requiredColor(r, object, "background", t.defaultBackground);
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
    optionalColor(r, object, "cursorColor", t.cursorColor);
    optionalColor(r, object, "cursorTextColor", t.cursorTextColor);
    optionalColor(r, object, "selectionForeground", t.selectionForeground);
    optionalColor(r, object, "selectionBackground", t.selectionBackground);
    r.boolean("boldIsBright", t.boldIsBright);
    if (const Json::Value* contrast = r.get("minimumContrast", Json::Value::Type::Number)) {
        const double v = contrast->toNumber();
        if (!std::isfinite(v) || v < 1 || v > 21) r.fail("minimumContrast", "is out of range");
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
    // The 4096 MiB limit of Graphics.quota.
    if (const auto n = r.integer("graphicsQuota", 0, std::int64_t(4096) << 20)) t.graphicsQuota = std::size_t(*n);
    r.boolean("graphicsFileTransfer", t.graphicsFileTransfer);
    if (r.failed()) return outer.fail("terminal", "is not valid");
    return true;
}

} // namespace

bool validSessionName(std::string_view name)
{
    if (name.empty() || name.size() > MaxSessionName) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

Json::Value toJson(const SessionOptions& o)
{
    Json::Array argv(o.launch.argv.begin(), o.launch.argv.end());
    Json::Array environment;
    for (const auto& [name, value] : o.launch.environment) environment.push_back(Json::Array{name, value});
    Json::Array unset(o.launch.unsetEnvironment.begin(), o.launch.unsetEnvironment.end());
    return Json::Object{
        {"name", o.name},
        {"argv", std::move(argv)},
        {"cwd", o.launch.workingDirectory},
        {"environment", std::move(environment)},
        {"unset", std::move(unset)},
        {"launchedProgram", o.launch.launchedProgram},
        {"cols", o.size.cols},
        {"rows", o.size.rows},
        {"cellWidth", o.size.cellWidth},
        {"cellHeight", o.size.cellHeight},
        {"terminal", toJson(o.terminal)},
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
        r.fail("name", "is not 1 to 64 letters, digits, underscores or hyphens");
    }
    r.strings("argv", o.launch.argv);
    r.string("cwd", o.launch.workingDirectory);
    if (const Json::Value* environment = r.get("environment", Json::Value::Type::Array)) {
        for (const Json::Value& entry : environment->toArray()) {
            const bool pair = entry.isArray() && entry.toArray().size() == 2 && entry.toArray()[0].isString()
                && entry.toArray()[1].isString();
            // A name with '=' or a NUL would set another variable.
            if (!pair || entry.toArray()[0].toString().empty()
                || entry.toArray()[0].toString().find_first_of(std::string_view("=\0", 2)) != std::string::npos) {
                r.fail("environment", "has an item that is not a [name, value] pair");
                break;
            }
            o.launch.environment.emplace_back(entry.toArray()[0].toString(), entry.toArray()[1].toString());
        }
    }
    r.strings("unset", o.launch.unsetEnvironment);
    r.string("launchedProgram", o.launch.launchedProgram);
    if (const auto n = r.integer("cols", 2, 4096)) o.size.cols = int(*n);
    if (const auto n = r.integer("rows", 1, 1048576 / 2)) o.size.rows = int(*n);
    if (const auto n = r.integer("cellWidth", 0, 65535)) o.size.cellWidth = int(*n);
    if (const auto n = r.integer("cellHeight", 0, 65535)) o.size.cellHeight = int(*n);
    if (const Json::Value* terminal = value.find("terminal")) readTerminal(r, *terminal, o.terminal, error);
    else r.fail("terminal", "is missing");
    if (r.failed()) return std::nullopt;
    return o;
}

Json::Value toJson(const SessionInfo& info)
{
    return Json::Object{
        {"name", info.name},
        {"attached", info.attached},
        {"windows", info.windows},
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
    if (const auto n = r.integer("windows", 0, 1 << 20)) info.windows = int(*n);
    if (const auto n = r.integer("panes", 0, 1 << 20)) info.panes = int(*n);
    r.strings("jobs", info.jobs);
    if (r.failed()) return std::nullopt;
    return info;
}

} // namespace AfterglowMux
