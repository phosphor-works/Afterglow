// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <string>
#include <system_error>
#include <type_traits>

#include <locale.h>
#if __has_include(<xlocale.h>)
#include <xlocale.h> // strtod_l of FreeBSD's libc
#endif

/// std::from_chars of a floating-point number, also with a standard library
/// that has none (libc++ before LLVM 20, the base compiler of FreeBSD 15).
namespace AfterglowProto {

namespace detail {

template<typename T>
inline constexpr bool HasFloatFromChars = requires(const char* p, T& value) { std::from_chars(p, p, value); };

inline bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

inline bool startsWithNoCase(const char* p, const char* last, const char* word)
{
    for (; *word; ++p, ++word) {
        if (p == last || (*p | 0x20) != *word)
            return false;
    }
    return true;
}

/// The end of the longest text from @p first that std::from_chars reads in
/// @p format, or @p first when none. Only general, fixed and scientific.
inline const char* scanFloat(const char* first, const char* last, std::chars_format format)
{
    const char* p = first;
    if (p != last && *p == '-')
        ++p;
    if (startsWithNoCase(p, last, "infinity"))
        return p + 8;
    if (startsWithNoCase(p, last, "inf"))
        return p + 3;
    if (startsWithNoCase(p, last, "nan")) {
        p += 3;
        // "nan(chars)": the parenthesis counts only when it closes.
        const char* q = p;
        if (q != last && *q == '(') {
            ++q;
            while (q != last && (isDigit(*q) || ((*q | 0x20) >= 'a' && (*q | 0x20) <= 'z') || *q == '_'))
                ++q;
            if (q != last && *q == ')')
                return q + 1;
        }
        return p;
    }
    const char* digits = p;
    while (p != last && isDigit(*p))
        ++p;
    bool any = p != digits;
    if (p != last && *p == '.') {
        const char* fraction = ++p;
        while (p != last && isDigit(*p))
            ++p;
        any = any || p != fraction;
    }
    if (!any)
        return first;
    const bool exponentAllowed = (format & std::chars_format::scientific) == std::chars_format::scientific;
    const bool exponentRequired = format == std::chars_format::scientific;
    if (exponentAllowed && p != last && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        if (q != last && (*q == '+' || *q == '-'))
            ++q;
        if (q != last && isDigit(*q)) {
            while (q != last && isDigit(*q))
                ++q;
            return q;
        }
    }
    return exponentRequired ? first : p;
}

inline locale_t cLocale()
{
    static const locale_t locale = ::newlocale(LC_ALL_MASK, "C", locale_t(nullptr));
    return locale;
}

} // namespace detail

/// As std::from_chars for float and double: the same grammar (no leading
/// space, no '+', a '.' whatever the locale), the same results.
/// std::chars_format::hex is not supported where the library lacks them.
template<typename T>
std::from_chars_result fromChars(const char* first, const char* last, T& value,
                                 std::chars_format format = std::chars_format::general)
{
    if constexpr (detail::HasFloatFromChars<T>) {
        return std::from_chars(first, last, value, format);
    } else {
        static_assert(std::is_same_v<T, double> || std::is_same_v<T, float>);
        const char* end = format == std::chars_format::hex ? first : detail::scanFloat(first, last, format);
        if (end == first)
            return {first, std::errc::invalid_argument};
        // strtod needs a terminated string, and reads in the C locale here.
        const std::string text(first, end);
        char* stop = nullptr;
        errno = 0;
        T parsed{};
        if constexpr (std::is_same_v<T, float>) {
            parsed = ::strtof_l(text.c_str(), &stop, detail::cLocale());
        } else {
            parsed = ::strtod_l(text.c_str(), &stop, detail::cLocale());
        }
        if (stop != text.c_str() + text.size())
            return {first, std::errc::invalid_argument};
        if (errno == ERANGE)
            return {end, std::errc::result_out_of_range};
        value = parsed;
        return {end, std::errc{}};
    }
}

} // namespace AfterglowProto
