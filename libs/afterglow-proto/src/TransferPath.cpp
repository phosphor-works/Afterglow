// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/TransferPath.h>

#include <algorithm>
#include <vector>

namespace AfterglowProto {

namespace TransferPath {

namespace {

    // Go's Clean, for a path that is rooted at / or not rooted at all.
    std::string cleanPlain(std::string_view path)
    {
        const bool rooted = !path.empty() && path.front() == '/';
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        while (start <= path.size()) {
            const auto end = std::min(path.find('/', start), path.size());
            const auto part = path.substr(start, end - start);
            start = end + 1;
            if (part.empty() || part == ".") {
                continue;
            }
            if (part == "..") {
                if (!parts.empty() && parts.back() != "..") {
                    parts.pop_back();
                } else if (!rooted) {
                    parts.push_back(part);
                }
                continue;
            }
            parts.push_back(part);
        }
        std::string out = rooted ? "/" : "";
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) {
                out += '/';
            }
            out += parts[i];
        }
        return out.empty() ? "." : out;
    }

} // namespace

std::string clean(std::string_view path)
{
    if (path.empty()) {
        return ".";
    }
    // ~ stands for a directory that only the other computer knows, so a
    // ".." after it stays: folding it into ~ would name a path under the home.
    if (path == "~" || path.starts_with("~/")) {
        auto rest = path.substr(1);
        while (rest.starts_with('/')) {
            rest.remove_prefix(1);
        }
        const auto cleaned = cleanPlain(rest);
        return cleaned == "." ? "~" : "~/" + cleaned;
    }
    return cleanPlain(path);
}

std::string join(std::string_view a, std::string_view b)
{
    if (a.empty()) {
        return b.empty() ? std::string() : clean(b);
    }
    if (b.empty()) {
        return clean(a);
    }
    return clean(std::string(a) + "/" + std::string(b));
}

std::string baseName(std::string_view path)
{
    while (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }
    if (path.empty()) {
        return ".";
    }
    if (path == "/") {
        return "/";
    }
    const auto at = path.rfind('/');
    return std::string(at == std::string_view::npos ? path : path.substr(at + 1));
}

std::string dirName(std::string_view path)
{
    const auto at = path.rfind('/');
    if (at == std::string_view::npos) {
        return ".";
    }
    return clean(path.substr(0, at + 1));
}

std::string relative(std::string_view base, std::string_view target)
{
    const auto b = clean(base);
    const auto t = clean(target);
    auto split = [](const std::string& p) {
        std::vector<std::string> parts;
        std::size_t start = 1;
        while (start < p.size()) {
            const auto end = std::min(p.find('/', start), p.size());
            parts.emplace_back(p.substr(start, end - start));
            start = end + 1;
        }
        return parts;
    };
    const auto bp = split(b);
    const auto tp = split(t);
    std::size_t common = 0;
    while (common < bp.size() && common < tp.size() && bp[common] == tp[common]) {
        ++common;
    }
    std::string out;
    for (std::size_t i = common; i < bp.size(); ++i) {
        out += out.empty() ? ".." : "/..";
    }
    for (std::size_t i = common; i < tp.size(); ++i) {
        if (!out.empty()) {
            out += '/';
        }
        out += tp[i];
    }
    return out.empty() ? "." : out;
}

std::string absolute(std::string_view path, std::string_view home, std::string_view cwd)
{
    if (path == "~") {
        return clean(home);
    }
    if (path.starts_with("~/")) {
        return join(home, path.substr(2));
    }
    if (path.starts_with("/")) {
        return clean(path);
    }
    return join(cwd, path);
}

} // namespace TransferPath

} // namespace AfterglowProto
