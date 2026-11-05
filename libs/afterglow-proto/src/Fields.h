// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <string_view>

namespace AfterglowProto::detail {

/// Calls @p f with each field of @p text between the @p separator bytes, empty fields included.
template<typename F>
void eachField(std::string_view text, char separator, F f)
{
    std::size_t pos = 0;
    while (true) {
        const auto end = text.find(separator, pos);
        f(text.substr(pos, end == std::string_view::npos ? end : end - pos));
        if (end == std::string_view::npos) {
            return;
        }
        pos = end + 1;
    }
}

} // namespace AfterglowProto::detail
