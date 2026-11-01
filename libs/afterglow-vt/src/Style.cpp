// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Style.h>

namespace AfterglowVt {

StyleTable::StyleTable()
{
    m_styles.emplace_back();
    m_refs.push_back(0);
}

std::uint32_t StyleTable::acquire(const Style& style)
{
    if (style == Style{}) {
        return DefaultId;
    }
    if (auto it = m_lookup.find(style); it != m_lookup.end()) {
        ++m_refs[it->second];
        return it->second;
    }

    std::uint32_t id;
    if (!m_free.empty()) {
        id = m_free.back();
        m_free.pop_back();
        m_styles[id] = style;
        m_refs[id] = 1;
    } else {
        if (m_styles.size() > MaxId) {
            // 16M distinct live styles: degrade to the default rather than wrap.
            return DefaultId;
        }
        id = std::uint32_t(m_styles.size());
        m_styles.push_back(style);
        m_refs.push_back(1);
    }
    m_lookup.emplace(style, id);
    retainLink(style.link);
    return id;
}

void StyleTable::releaseCounted(std::uint32_t id) noexcept
{
    if (--m_refs[id] == 0) {
        releaseLink(m_styles[id].link);
        m_lookup.erase(m_styles[id]);
        m_free.push_back(id);
    }
}

std::uint32_t StyleTable::internLink(std::string_view uri, std::string_view id)
{
    // Without an id, every OSC 8 sequence is its own link even for equal URIs.
    std::string key;
    if (!id.empty()) {
        key.reserve(id.size() + uri.size() + 1);
        key.append(id).push_back('\x1f');
        key.append(uri);
        if (auto it = m_linkLookup.find(key); it != m_linkLookup.end()) {
            return it->second;
        }
    }
    if (uri.size() + id.size() > MaxLinkBytes - m_linkBytes) {
        return 0;
    }
    std::uint32_t index;
    if (!m_freeLinks.empty()) {
        index = m_freeLinks.back();
        m_freeLinks.pop_back();
    } else {
        index = std::uint32_t(m_links.size());
        m_links.emplace_back();
    }
    m_links[index] = Link{std::string(uri), std::string(id), 0};
    m_linkBytes += uri.size() + id.size();
    if (key.empty()) {
        key = "\x1e" + std::to_string(index);
    }
    m_linkLookup.emplace(std::move(key), index);
    return index;
}

std::string_view StyleTable::linkUri(std::uint32_t link) const noexcept
{
    return link < m_links.size() ? std::string_view(m_links[link].uri) : std::string_view{};
}

void StyleTable::retainLink(std::uint32_t link) noexcept
{
    if (link != 0 && link < m_links.size()) {
        ++m_links[link].refs;
    }
}

void StyleTable::releaseLink(std::uint32_t link) noexcept
{
    if (link == 0 || link >= m_links.size() || --m_links[link].refs != 0) {
        return;
    }
    const Link& l = m_links[link];
    m_linkBytes -= l.uri.size() + l.id.size();
    m_linkLookup.erase(l.id.empty() ? "\x1e" + std::to_string(link) : l.id + '\x1f' + l.uri);
    m_links[link] = Link{};
    m_freeLinks.push_back(link);
}

} // namespace AfterglowVt
