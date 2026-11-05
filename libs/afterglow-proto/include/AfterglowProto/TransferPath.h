// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <string>
#include <string_view>

namespace AfterglowProto {

/// Path helpers with the meaning of Go's path/filepath, which kitten
/// transfer uses to map paths: clean() folds "." and ".." and repeated
/// slashes, baseName() and dirName() split the cleaned path. Unlike Go, a
/// leading ~ is a root that ".." cannot remove: ~/../x names a directory
/// beside the home directory, which only the computer that has it can resolve.
namespace TransferPath {
std::string clean(std::string_view path);
std::string join(std::string_view a, std::string_view b);
std::string baseName(std::string_view path);
std::string dirName(std::string_view path);
/// @p target relative to @p base, both absolute and clean.
std::string relative(std::string_view base, std::string_view target);
/// ~ and ~/x under @p home; a relative path under @p cwd.
std::string absolute(std::string_view path, std::string_view home, std::string_view cwd);
} // namespace TransferPath

} // namespace AfterglowProto
