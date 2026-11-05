// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Sha256.h>
#include <AfterglowProto/Transfer.h>

#include "Fields.h"

#include <algorithm>
#include <cerrno>
#include <vector>

namespace AfterglowProto {

namespace {

constexpr std::string_view Keys[] = {"ac", "id", "fid", "n", "st", "d", "pw", "sz", "q", "ft", "zip", "tt", "prm", "mod", "pr"};

} // namespace

bool transferEncodedKey(std::string_view key)
{
    return key == "n" || key == "st" || key == "d" || key == "pw";
}

bool transferSafeId(std::string_view id)
{
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || std::string_view("_:./@-").find(char(c)) != std::string_view::npos;
    });
}

std::optional<TransferFields> decodeTransfer(std::string_view payload)
{
    if (payload.size() > TransferMaxCommand) {
        return std::nullopt;
    }
    TransferFields fields;
    bool valid = true;
    detail::eachField(payload, ';', [&](std::string_view field) {
        const auto at = field.find('=');
        if (!valid || at == 0 || at == std::string_view::npos) {
            return;
        }
        const auto key = field.substr(0, at);
        const auto value = field.substr(at + 1);
        if (std::find(std::begin(Keys), std::end(Keys), key) == std::end(Keys)) {
            return;
        }
        if (fields.contains(std::string(key))) {
            valid = false;
            return;
        }
        if (transferEncodedKey(key)) {
            auto decoded = base64Decode(value, Base64Check::StrictOrUnpadded);
            if (!decoded) {
                valid = false;
                return;
            }
            fields.emplace(key, std::move(*decoded));
        } else {
            fields.emplace(key, value);
        }
    });
    if (!valid) {
        return std::nullopt;
    }
    const auto id = fields.find("id");
    const auto fid = fields.find("fid");
    if (id == fields.end() || !transferSafeId(id->second) || (fid != fields.end() && !transferSafeId(fid->second))) {
        return std::nullopt;
    }
    return fields;
}

std::string encodeTransfer(const TransferFields& fields)
{
    std::vector<std::pair<std::string_view, std::string_view>> views;
    views.reserve(fields.size());
    for (const auto& [key, value] : fields) {
        views.emplace_back(key, value);
    }
    return encodeTransfer(views);
}

std::string encodeTransfer(std::span<const std::pair<std::string_view, std::string_view>> fields)
{
    std::string out = "\x1b]5113;";
    bool first = true;
    for (const auto& [key, value] : fields) {
        if (!first) {
            out += ';';
        }
        first = false;
        out += key;
        out += '=';
        out += transferEncodedKey(key) ? base64Encode(value, false) : value;
    }
    out += "\x1b\\";
    return out;
}

std::string transferCancelId(std::string_view payload)
{
    std::string_view action;
    std::string_view id;
    detail::eachField(payload, ';', [&](std::string_view field) {
        if (action.empty() && field.starts_with("ac=")) {
            action = field.substr(3);
        } else if (id.empty() && field.starts_with("id=")) {
            id = field.substr(3);
        }
    });
    return action == "cancel" && transferSafeId(id) ? std::string(id) : std::string();
}

std::string transferPasswordHash(std::string_view id, std::string_view password)
{
    Sha256 hash;
    hash.update(id);
    hash.update(";");
    hash.update(password);
    return "sha256:" + toHex(hash.finish());
}

std::string_view transferErrorName(int error)
{
    switch (error) {
    case EACCES:
    case EPERM:
        return "EPERM";
    case ENOENT:
        return "ENOENT";
    case ENOSPC:
        return "ENOSPC";
    case EEXIST:
        return "EEXIST";
    case ENOTDIR:
        return "ENOTDIR";
    case ELOOP:
        return "ELOOP";
    default:
        return "EIO";
    }
}

bool transferSuccess(std::string_view status)
{
    return status == TransferStatus::Ok || status == TransferStatus::Started || status == TransferStatus::Progress
        || status == TransferStatus::Canceled;
}

} // namespace AfterglowProto
