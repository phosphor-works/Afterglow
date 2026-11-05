// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <string_view>

// The protocol library takes and gives std::string. These convert at the edge of the desktop services.
namespace Afterglow::ProtoBytes {
inline std::string_view view(const QByteArray& bytes) { return { bytes.constData(), std::size_t(bytes.size()) }; }
inline QByteArray bytesOf(std::string_view bytes) { return { bytes.data(), qsizetype(bytes.size()) }; }
}
