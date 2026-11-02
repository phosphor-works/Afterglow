// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Style.h>

#include <bit>
#include <utility>

namespace AfterglowVt {

StyleTable::StyleTable()
{
    m_styles.emplace_back();
    m_refs.push_back(0);
}

std::uint32_t StyleTable::hashOf(const Style& style) noexcept
{
    const std::uint64_t h = StyleHash{}(style);
    return std::uint32_t(h ^ (h >> 32));
}

std::size_t StyleTable::Index::home(std::uint32_t hash) const noexcept
{
    // Fibonacci hashing: the top bits of the product take every input bit.
    return std::size_t(std::uint32_t(hash * 0x9E3779B9u) >> m_shift);
}

std::uint32_t StyleTable::Index::find(const Style& style, std::uint32_t hash, const std::vector<Style>& styles) const noexcept
{
    if (m_slots.empty()) {
        return DefaultId;
    }
    const std::size_t mask = m_slots.size() - 1;
    for (std::size_t i = home(hash);; i = (i + 1) & mask) {
        const Slot slot = m_slots[i];
        if (slot.id == DefaultId || (slot.hash == hash && styles[slot.id] == style)) {
            return slot.id;
        }
    }
}

void StyleTable::Index::insert(std::uint32_t hash, std::uint32_t id)
{
    // At most half full, so that a probe ends soon.
    if ((m_size + 1) * 2 > m_slots.size()) {
        grow();
    }
    const std::size_t mask = m_slots.size() - 1;
    std::size_t i = home(hash);
    while (m_slots[i].id != DefaultId) {
        i = (i + 1) & mask;
    }
    m_slots[i] = {hash, id};
    ++m_size;
}

void StyleTable::Index::erase(std::uint32_t hash, std::uint32_t id) noexcept
{
    if (m_slots.empty()) {
        return;
    }
    const std::size_t mask = m_slots.size() - 1;
    std::size_t i = home(hash);
    while (m_slots[i].id != DefaultId && m_slots[i].id != id) {
        i = (i + 1) & mask;
    }
    if (m_slots[i].id == DefaultId) {
        return;
    }
    // Backward shift: move each later entry of the run into the hole when
    // the hole lies between its home and its slot, so no probe stops short.
    for (std::size_t j = (i + 1) & mask; m_slots[j].id != DefaultId; j = (j + 1) & mask) {
        const std::size_t want = home(m_slots[j].hash);
        if (((j - want) & mask) >= ((j - i) & mask)) {
            m_slots[i] = m_slots[j];
            i = j;
        }
    }
    m_slots[i] = {};
    --m_size;
}

void StyleTable::Index::grow()
{
    const std::size_t capacity = m_slots.empty() ? 64 : m_slots.size() * 2;
    std::vector<Slot> old = std::exchange(m_slots, std::vector<Slot>(capacity));
    m_shift = 32 - std::countr_zero(capacity);
    m_size = 0;
    for (const Slot& slot : old) {
        if (slot.id != DefaultId) {
            insert(slot.hash, slot.id);
        }
    }
}

std::uint32_t StyleTable::acquire(const Style& style)
{
    if (style == Style{}) {
        return DefaultId;
    }
    const std::uint32_t hash = hashOf(style);
    if (const std::uint32_t found = m_lookup.find(style, hash, m_styles); found != DefaultId) {
        if (m_refs[found]++ == 0) {
            --m_idleCount;
        }
        return found;
    }

    if (m_free.empty() && m_styles.size() > MaxId) {
        sweepIdle();
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
    m_lookup.insert(hash, id);
    retainLink(style.link);
    return id;
}

void StyleTable::releaseCounted(std::uint32_t id) noexcept
{
    if (--m_refs[id] != 0) {
        return;
    }
    // A link counts against MaxLinkBytes while its style lives: it goes at once.
    if (m_styles[id].link != 0) {
        forget(id);
        return;
    }
    ++m_idleCount;
    m_idle.push_back(id);
    if (m_idleCount > MaxIdle) {
        sweepIdle();
    }
}

void StyleTable::forget(std::uint32_t id) noexcept
{
    releaseLink(m_styles[id].link);
    m_lookup.erase(hashOf(m_styles[id]), id);
    m_free.push_back(id);
}

void StyleTable::sweepIdle() noexcept
{
    for (const std::uint32_t id : m_idle) {
        // Skip a style in use again, and a second entry for one already freed.
        if (m_refs[id] == 0 && m_lookup.find(m_styles[id], hashOf(m_styles[id]), m_styles) == id) {
            forget(id);
        }
    }
    m_idle.clear();
    m_idleCount = 0;
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
