// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace Afterglow::Aglow {

/// The program @p name in the directory of this executable: the aglow or
/// aglow-mux of the same build or package, never another program of
/// that name on PATH. Empty with @p error when this executable cannot be
/// found.
std::string siblingExecutable(const char* name, std::string* error);

} // namespace Afterglow::Aglow
