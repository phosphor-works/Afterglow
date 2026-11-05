// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Hex.h>
#include <AfterglowProto/Random.h>

#include <cerrno>
#include <sys/random.h>

namespace AfterglowProto {

bool randomBytes(void* out, std::size_t size)
{
    auto* at = static_cast<char*>(out);
    while (size) {
        // Reads of more than 256 bytes can return short, and a signal can interrupt any read.
        const ssize_t n = ::getrandom(at, size, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        at += n;
        size -= std::size_t(n);
    }
    return true;
}

std::optional<std::string> randomHex(std::size_t size)
{
    std::string bytes(size, '\0');
    if (!randomBytes(bytes.data(), size)) {
        return std::nullopt;
    }
    return toHex(bytes);
}

} // namespace AfterglowProto
