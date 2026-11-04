// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace AfterglowProto {

/// Fills @p out with @p size bytes from the kernel's random source
/// (getrandom). False when the kernel gives none: a caller that needs an
/// unguessable value must fail, never fall back to a weaker source.
bool randomBytes(void* out, std::size_t size);
/// @p size random bytes, as lowercase hexadecimal.
std::optional<std::string> randomHex(std::size_t size);

} // namespace AfterglowProto
