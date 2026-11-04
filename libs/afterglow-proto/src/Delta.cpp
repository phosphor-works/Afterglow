// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Delta.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <xxhash.h>

namespace AfterglowProto {

namespace {

template<typename T>
T readLittle(const char* at)
{
    T value = 0;
    for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
        value |= T(std::uint8_t(at[byte])) << (8 * byte);
    }
    return value;
}

template<typename T>
void appendLittle(std::string& out, T value)
{
    for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
        out += char(std::uint8_t(value >> (8 * byte)));
    }
}

/// A streaming XXH3-128. A failed state allocation is reported by valid(),
/// not thrown: the transfer service answers ENOMEM, and an exception would
/// end the process from its worker thread.
class StreamHash
{
public:
    StreamHash()
    {
        if (state) {
            XXH3_128bits_reset(state);
        }
    }
    ~StreamHash() { XXH3_freeState(state); }
    bool valid() const { return state != nullptr; }
    StreamHash(const StreamHash&) = delete;
    StreamHash& operator=(const StreamHash&) = delete;
    void update(std::string_view bytes) { XXH3_128bits_update(state, bytes.data(), bytes.size()); }
    std::array<std::uint8_t, 16> digest() const
    {
        XXH128_canonical_t canonical{};
        XXH128_canonicalFromHash(&canonical, XXH3_128bits_digest(state));
        std::array<std::uint8_t, 16> out;
        std::memcpy(out.data(), canonical.digest, out.size());
        return out;
    }

private:
    XXH3_state_t* state = XXH3_createState();
};

} // namespace

struct DeltaGenerator::Hash : StreamHash
{
};
struct DeltaApplier::Hash : StreamHash
{
};

std::uint32_t deltaBlockSize(std::uint64_t fileSize)
{
    return std::uint32_t(std::max(64.0, std::ceil(std::sqrt(double(fileSize)))));
}

std::uint32_t rollingChecksum(std::string_view block)
{
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    for (const char c : block) {
        a += std::uint8_t(c);
        b += a;
    }
    return (a & 0xffffu) | (b << 16);
}

std::uint64_t blockHash(std::string_view block)
{
    return XXH3_64bits(block.data(), block.size());
}

std::array<std::uint8_t, 16> fileHash(std::string_view bytes)
{
    XXH128_canonical_t canonical{};
    XXH128_canonicalFromHash(&canonical, XXH3_128bits(bytes.data(), bytes.size()));
    std::array<std::uint8_t, 16> out;
    std::memcpy(out.data(), canonical.digest, out.size());
    return out;
}

SignatureBuilder::SignatureBuilder(std::uint64_t fileSize)
    : m_blockSize(deltaBlockSize(fileSize))
{
}

std::string SignatureBuilder::header() const
{
    std::string out(8, '\0');
    appendLittle(out, m_blockSize);
    return out;
}

std::string SignatureBuilder::record(std::string_view block)
{
    std::string out;
    out.reserve(SignatureRecordSize);
    appendLittle(out, m_index++);
    appendLittle(out, rollingChecksum(block));
    appendLittle(out, blockHash(block));
    return out;
}

std::string makeSignature(std::string_view basis)
{
    SignatureBuilder builder(basis.size());
    std::string out = builder.header();
    for (std::size_t at = 0; at < basis.size(); at += builder.blockSize()) {
        out += builder.record(basis.substr(at, builder.blockSize()));
    }
    return out;
}

std::optional<std::uint32_t> signatureBlockSize(std::string_view signature, std::uint32_t maxBlockSize)
{
    if (signature.size() < SignatureHeaderSize || (signature.size() - SignatureHeaderSize) % SignatureRecordSize != 0
        || signature.substr(0, 8) != std::string_view("\0\0\0\0\0\0\0\0", 8)) {
        return std::nullopt;
    }
    const auto blockSize = readLittle<std::uint32_t>(signature.data() + 8);
    if (blockSize == 0 || blockSize > maxBlockSize) {
        return std::nullopt;
    }
    return blockSize;
}

bool MemorySource::read(std::string& out, std::size_t max)
{
    const auto part = m_bytes.substr(0, max);
    out += part;
    m_bytes.remove_prefix(part.size());
    return true;
}

DeltaGenerator::DeltaGenerator(std::string signature)
    : m_signature(std::move(signature))
    , m_hash(std::make_unique<Hash>())
{
}

DeltaGenerator::~DeltaGenerator() = default;

bool DeltaGenerator::valid() const
{
    return m_hash->valid();
}

void DeltaGenerator::parse()
{
    m_blockSize = signatureBlockSize(m_signature, UINT32_MAX).value_or(0);
    for (std::size_t at = SignatureHeaderSize; at + SignatureRecordSize <= m_signature.size(); at += SignatureRecordSize) {
        const auto index = readLittle<std::uint64_t>(m_signature.data() + at);
        const auto weak = readLittle<std::uint32_t>(m_signature.data() + at + 8);
        const auto strong = readLittle<std::uint64_t>(m_signature.data() + at + 12);
        m_weakHashes.insert(weak);
        m_blocks.try_emplace({weak, strong}, index);
    }
    m_signature = std::string();
    m_parsed = true;
}

void DeltaGenerator::flushLiteral(std::string& out)
{
    if (m_literal.empty()) {
        return;
    }
    out += char(1);
    appendLittle(out, std::uint32_t(m_literal.size()));
    m_hash->update(m_literal);
    out += m_literal;
    m_literal.clear();
}

bool DeltaGenerator::produce(ByteSource& file, std::string& out, std::size_t want, std::size_t step)
{
    if (!valid()) {
        return false;
    }
    if (!m_parsed) {
        parse();
    }
    if (!m_blockSize) {
        return false; // a malformed signature
    }
    const std::size_t blockSize = m_blockSize;
    std::size_t scanned = 0;
    while (!m_done && out.size() < want && scanned < step) {
        if (m_buffer.size() - m_position <= blockSize && !file.atEnd()) {
            m_buffer.erase(0, m_position);
            m_position = 0;
            if (!file.read(m_buffer, std::max<std::size_t>(65536, 2 * blockSize))) {
                return false;
            }
            m_fresh = true;
        }
        const std::size_t length = std::min(blockSize, m_buffer.size() - m_position);
        if (!length) {
            flushLiteral(out);
            out += char(2);
            appendLittle(out, std::uint16_t(16));
            const auto digest = m_hash->digest();
            out.append(reinterpret_cast<const char*>(digest.data()), digest.size());
            m_done = true;
            break;
        }
        const auto* data = reinterpret_cast<const std::uint8_t*>(m_buffer.data());
        if (m_fresh) {
            m_a = m_b = 0;
            for (std::size_t n = 0; n < length; ++n) {
                m_a += data[m_position + n];
                m_b += m_a;
            }
            m_a &= 0xffffu;
            m_b &= 0xffffu;
        }
        std::optional<std::uint64_t> match;
        const std::uint32_t weak = m_a | (m_b << 16);
        if (m_weakHashes.contains(weak)) {
            const auto strong = XXH3_64bits(data + m_position, length);
            if (const auto found = m_blocks.find({weak, strong}); found != m_blocks.end()) {
                match = found->second;
            }
        }
        if (match) {
            flushLiteral(out);
            out += char(0);
            appendLittle(out, *match);
            m_hash->update(std::string_view(m_buffer).substr(m_position, length));
            m_position += length;
            scanned += length;
            m_fresh = true;
        } else {
            m_literal += char(data[m_position]);
            const std::uint32_t old = data[m_position];
            if (m_position + length < m_buffer.size()) {
                const std::uint32_t next = data[m_position + length];
                m_a = (m_a - old + next) & 0xffffu;
                m_b = (m_b - std::uint32_t(length) * old + m_a) & 0xffffu;
                m_fresh = false;
            } else if (file.atEnd()) {
                // At the end of the file the window shrinks: roll the old byte out only.
                m_a = (m_a - old) & 0xffffu;
                m_b = (m_b - std::uint32_t(length) * old) & 0xffffu;
                m_fresh = false;
            } else {
                m_fresh = true; // the next pass refills the buffer
            }
            ++m_position;
            ++scanned;
            if (m_literal.size() >= 65536) {
                flushLiteral(out);
            }
        }
    }
    return true;
}

DeltaApplier::DeltaApplier(std::uint32_t blockSize, std::uint64_t basisSize, std::size_t maxLiteral)
    : m_blocks(blockSize ? (basisSize + blockSize - 1) / blockSize : 0)
    , m_maxLiteral(maxLiteral)
    , m_hash(std::make_unique<Hash>())
{
}

DeltaApplier::~DeltaApplier() = default;

bool DeltaApplier::valid() const
{
    return m_hash->valid();
}

bool DeltaApplier::output(DeltaTarget& target, std::string_view bytes)
{
    if (!target.write(bytes)) {
        return false;
    }
    m_hash->update(bytes);
    return true;
}

std::optional<std::size_t> DeltaApplier::apply(std::string_view pending, DeltaTarget& target)
{
    if (!valid()) {
        return std::nullopt;
    }
    std::size_t at = 0;
    while (at < pending.size()) {
        const std::string_view rest = pending.substr(at);
        if (m_finished) {
            return std::nullopt; // nothing may follow the hash
        }
        const auto type = std::uint8_t(rest[0]);
        std::size_t used = 0;
        if (type == 1) {
            if (rest.size() < 5) {
                break;
            }
            const auto size = readLittle<std::uint32_t>(rest.data() + 1);
            if (size > m_maxLiteral) {
                return std::nullopt;
            }
            if (rest.size() < 5 + std::size_t(size)) {
                break;
            }
            if (!output(target, rest.substr(5, size))) {
                return std::nullopt;
            }
            used = 5 + std::size_t(size);
        } else if (type == 0 || type == 3) {
            const std::size_t length = type == 0 ? 9 : 13;
            if (rest.size() < length) {
                break;
            }
            const auto first = readLittle<std::uint64_t>(rest.data() + 1);
            const std::uint64_t count = type == 0 ? 1 : std::uint64_t(readLittle<std::uint32_t>(rest.data() + 9)) + 1;
            if (first >= m_blocks || count > m_blocks - first) {
                return std::nullopt;
            }
            std::string block;
            for (std::uint64_t i = 0; i < count; ++i) {
                block.clear();
                if (!target.readBlock(first + i, block) || block.empty() || !output(target, block)) {
                    return std::nullopt;
                }
            }
            used = length;
        } else if (type == 2) {
            if (rest.size() < 3) {
                break;
            }
            if (readLittle<std::uint16_t>(rest.data() + 1) != 16) {
                return std::nullopt;
            }
            if (rest.size() < 19) {
                break;
            }
            const auto digest = m_hash->digest();
            if (std::memcmp(digest.data(), rest.data() + 3, digest.size()) != 0) {
                return std::nullopt;
            }
            m_finished = true;
            used = 19;
        } else {
            return std::nullopt;
        }
        at += used;
    }
    return at;
}

std::optional<std::string> makeDelta(std::string_view signature, std::string_view file)
{
    if (!signatureBlockSize(signature, UINT32_MAX)) {
        return std::nullopt;
    }
    DeltaGenerator generator{std::string(signature)};
    MemorySource source(file);
    std::string out;
    while (!generator.done()) {
        if (!generator.produce(source, out, SIZE_MAX, SIZE_MAX)) {
            return std::nullopt;
        }
    }
    return out;
}

std::optional<std::string> applyDelta(std::string_view basis, std::uint32_t blockSize, std::string_view delta)
{
    struct Target : DeltaTarget
    {
        std::string_view basis;
        std::uint32_t blockSize;
        std::string out;
        bool readBlock(std::uint64_t index, std::string& block) override
        {
            block += basis.substr(std::size_t(index) * blockSize, blockSize);
            return true;
        }
        bool write(std::string_view bytes) override
        {
            out += bytes;
            return true;
        }
    } target;
    target.basis = basis;
    target.blockSize = blockSize;
    DeltaApplier applier(blockSize, basis.size(), SIZE_MAX);
    const auto used = applier.apply(delta, target);
    if (!used || *used != delta.size() || !applier.finished()) {
        return std::nullopt;
    }
    return std::move(target.out);
}

} // namespace AfterglowProto
