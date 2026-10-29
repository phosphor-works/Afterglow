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
    return id;
}

void StyleTable::retain(std::uint32_t id) noexcept
{
    if (id != DefaultId) {
        ++m_refs[id];
    }
}

void StyleTable::release(std::uint32_t id) noexcept
{
    if (id == DefaultId) {
        return;
    }
    if (--m_refs[id] == 0) {
        m_lookup.erase(m_styles[id]);
        m_free.push_back(id);
    }
}

} // namespace AfterglowVt
