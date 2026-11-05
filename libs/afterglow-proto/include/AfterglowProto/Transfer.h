// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

/// Kitty's file transfer protocol (OSC 5113): the key/value messages and the
/// status codes. https://sw.kovidgoyal.net/kitty/file-transfer-protocol/
namespace AfterglowProto {

/// The fields of one message, by key. The values of the encoded keys
/// (transferEncodedKey()) are held decoded.
using TransferFields = std::map<std::string, std::string>;

/// The longest message payload that Afterglow reads, after "5113;". The
/// protocol sets no limit: this one is Afterglow's. The terminal can pass on
/// a longer message cut to TransferMaxCommand + 1 bytes. A message of that
/// length is an error, never a message to read.
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
/// Reads the payload after "5113;". Unknown keys are skipped, also when they
/// repeat. nullopt for a message that is too long, repeats a known key, has
/// bad base64, or has no valid id (or an invalid fid).
std::optional<TransferFields> decodeTransfer(std::string_view payload);
/// The complete OSC sequence, with the keys in sorted order. The values of
/// the encoded keys go in base64. The other values are written as they are:
/// the caller gives only numbers, ids and names from the protocol there,
/// never a ';', '=' or a control character.
std::string encodeTransfer(const TransferFields& fields);
/// The same, for fields that the caller holds elsewhere. They are written
/// in the order given.
std::string encodeTransfer(std::span<const std::pair<std::string_view, std::string_view>> fields);
/// The session id of a cancel request, read without decoding the base64
/// fields; empty when @p payload is not a cancel.
std::string transferCancelId(std::string_view payload);
/// The pw value that bypasses the permission prompt: "sha256:" and the hex
/// SHA-256 of "<session id>;<password>".
std::string transferPasswordHash(std::string_view id, std::string_view password);
/// The status for errno value @p error. The protocol takes any errno name.
/// Afterglow names the errors in the table below and sends EIO for the others.
///
/// | errno | Status |
/// | --- | --- |
/// | EACCES, EPERM | EPERM |
/// | ENOENT, ENOSPC, EEXIST, ENOTDIR, ELOOP | the same name |
std::string_view transferErrorName(int error);
/// OK, STARTED, PROGRESS and CANCELED: the statuses that a quiet session (q=1) omits.
bool transferSuccess(std::string_view status);

} // namespace AfterglowProto
