// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

/// Kitty's file transfer protocol (OSC 5113): the key/value messages and the
/// status codes. https://sw.kovidgoyal.net/kitty/file-transfer-protocol/
namespace AfterglowProto {

/// The fields of one message, by key. The values of the encoded keys
/// (transferEncodedKey()) are held decoded.
using TransferFields = std::map<std::string, std::string>;

/// The longest message payload, after "5113;". The protocol fixes it.
inline constexpr std::size_t TransferMaxCommand = 16384;
/// The most file data that one data message carries, before base64.
inline constexpr std::size_t TransferMaxData = 4096;

/// Status values (the st key of ac=status). An error is an errno name.
namespace TransferStatus {
inline constexpr std::string_view Ok = "OK";
inline constexpr std::string_view Started = "STARTED";
inline constexpr std::string_view Progress = "PROGRESS";
inline constexpr std::string_view Canceled = "CANCELED";
} // namespace TransferStatus

/// The keys whose values travel in base64: names, statuses, data, passwords.
bool transferEncodedKey(std::string_view key);
/// A session or file id: 1 to 128 of [A-Za-z0-9_:./@-].
bool transferSafeId(std::string_view id);
/// Reads the payload after "5113;". Unknown keys are skipped. nullopt for a
/// message that is too long, repeats a key, has bad base64, or has no valid
/// id (or an invalid fid).
std::optional<TransferFields> decodeTransfer(std::string_view payload);
/// The complete OSC sequence, the keys in order.
std::string encodeTransfer(const TransferFields& fields);
/// The session id of a cancel request, read without decoding the base64
/// fields; empty when @p payload is not a cancel.
std::string transferCancelId(std::string_view payload);
/// The pw value that bypasses the permission prompt: "sha256:" and the hex
/// SHA-256 of "<session id>;<password>".
std::string transferPasswordHash(std::string_view id, std::string_view password);
/// The status for errno value @p error; EIO for those the protocol does not name.
std::string_view transferErrorName(int error);
/// OK, STARTED, PROGRESS and CANCELED: the statuses that a quiet session (q=1) omits.
bool transferSuccess(std::string_view status);

} // namespace AfterglowProto
