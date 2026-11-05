// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Delta transfers of kitty's file transfer protocol (tt=rsync).
///
/// The receiver has an old copy of the file (the basis) and sends its
/// signature: a 12-byte header (8 zero bytes, then the block size as u32)
/// and a 20-byte record for each block (index u64, rolling checksum u32,
/// XXH3-64 u64). The sender answers with operations that rebuild the new
/// file from blocks of the basis and literal bytes, and ends with the
/// XXH3-128 of the whole file. Every integer is little-endian.
///
/// | Operation | Bytes |
/// | --- | --- |
/// | 0: one block | type, index u64 |
/// | 1: literal | type, length u32, the bytes |
/// | 2: file hash | type, length u16 (16), the canonical XXH3-128 |
/// | 3: block range | type, first index u64, count - 1 as u32 |
///
/// makeSignature(), makeDelta(), applyDelta() and MemorySource work on whole
/// buffers in memory. They are the client side of the protocol, used by
/// tests and by clients of the library. A terminal streams with
/// SignatureBuilder, DeltaGenerator and DeltaApplier.
namespace AfterglowProto {

inline constexpr std::size_t SignatureHeaderSize = 12;
inline constexpr std::size_t SignatureRecordSize = 20;
/// The largest block size that kitty accepts in a signature (the square root of 1 TiB).
inline constexpr std::uint32_t DeltaMaxBlockSize = 1024 * 1024;

/// The block size for a basis of @p fileSize bytes: ceil(sqrt(size)), from
/// 64 to DeltaMaxBlockSize.
std::uint32_t deltaBlockSize(std::uint64_t fileSize);
/// The rolling checksum of @p block: the low 16 bits of the byte sum, and
/// above them the low 16 bits of the sum of the running sums.
std::uint32_t rollingChecksum(std::string_view block);
/// XXH3-64 of @p block, the strong hash of a signature record.
std::uint64_t blockHash(std::string_view block);
/// XXH3-128 of @p bytes in canonical (big-endian) form.
std::array<std::uint8_t, 16> fileHash(std::string_view bytes);

/// Builds a signature, one block at a time.
class SignatureBuilder
{
public:
    explicit SignatureBuilder(std::uint64_t fileSize);
    std::uint32_t blockSize() const { return m_blockSize; }
    std::string header() const;
    /// The record of the next block. Each block but the last is blockSize() long.
    std::string record(std::string_view block);

private:
    std::uint32_t m_blockSize;
    std::uint64_t m_index = 0;
};

/// The whole signature of @p basis (a whole-buffer helper).
std::string makeSignature(std::string_view basis);
/// The block size of a signature, if @p signature is well formed and its
/// block size is from 1 to @p maxBlockSize.
std::optional<std::uint32_t> signatureBlockSize(std::string_view signature, std::uint32_t maxBlockSize);

/// The bytes of a file, read in pieces.
class ByteSource
{
public:
    virtual ~ByteSource() = default;
    /// Appends at most @p max bytes to @p out. False on a read error.
    virtual bool read(std::string& out, std::size_t max) = 0;
    virtual bool atEnd() const = 0;
};

/// A ByteSource over bytes in memory (a whole-buffer helper).
class MemorySource : public ByteSource
{
public:
    explicit MemorySource(std::string_view bytes)
        : m_bytes(bytes)
    {
    }
    bool read(std::string& out, std::size_t max) override;
    bool atEnd() const override { return m_bytes.empty(); }

private:
    std::string_view m_bytes;
};

/// Makes the operations that turn the receiver's basis into the file, as
/// the caller asks for them: a large file needs no copy, and a caller can
/// do other work between steps.
class DeltaGenerator
{
public:
    /// @p signature is read on the first produce(), so a generator that
    /// waits in a queue holds no tables. Check it first with
    /// signatureBlockSize(), with a bound that the caller can hold: a
    /// refill reads up to two blocks of the file.
    explicit DeltaGenerator(std::string signature);
    ~DeltaGenerator();
    DeltaGenerator(const DeltaGenerator&) = delete;
    DeltaGenerator& operator=(const DeltaGenerator&) = delete;

    /// False when the hash state could not be allocated. produce() then fails.
    bool valid() const;

    /// Appends operations to @p out until it holds @p want bytes, @p step
    /// bytes were scanned, or the delta ends. False on a read error or a
    /// malformed signature.
    ///
    /// Scanned bytes bound the work of one call. The window moves one byte
    /// for each byte scanned, and each block that is hashed counts as its
    /// length, whether it matches or not. One call can go past @p step by
    /// one block.
    bool produce(ByteSource& file, std::string& out, std::size_t want, std::size_t step);
    bool done() const { return m_done; }

private:
    struct Record
    {
        std::uint32_t weak;
        std::uint64_t strong;
        std::uint64_t index;
    };

    void parse();
    bool mayHold(std::uint32_t weak) const;
    void flushLiteral(std::string& out);

    std::string m_signature;
    std::uint32_t m_blockSize = 0;
    // Sorted by both hashes, so that many blocks with one weak hash cost a
    // binary search, not a scan. The tables cost about the signature's size.
    std::vector<Record> m_records;
    // One bit for each hashed weak value: most positions miss without a search.
    std::vector<std::uint64_t> m_weakFilter;
    unsigned m_filterShift = 0;
    struct Hash;
    std::unique_ptr<Hash> m_hash;
    std::string m_buffer;
    std::string m_literal;
    std::size_t m_position = 0;
    std::uint32_t m_a = 0;
    std::uint32_t m_b = 0;
    bool m_fresh = true;
    bool m_parsed = false;
    bool m_done = false;
};

/// What applying a delta reads and writes.
class DeltaTarget
{
public:
    virtual ~DeltaTarget() = default;
    /// Appends block @p index of the basis to @p out (the last block can be
    /// short, never empty: an empty block fails the delta). False on a read
    /// error, or to stop.
    virtual bool readBlock(std::uint64_t index, std::string& out) = 0;
    /// Appends @p bytes to the new file. False to stop.
    virtual bool write(std::string_view bytes) = 0;
};

/// Applies operations as they arrive, and checks the file hash at the end.
class DeltaApplier
{
public:
    /// @p maxLiteral bounds one literal operation, which the caller must buffer whole.
    DeltaApplier(std::uint32_t blockSize, std::uint64_t basisSize, std::size_t maxLiteral);
    ~DeltaApplier();
    DeltaApplier(const DeltaApplier&) = delete;
    DeltaApplier& operator=(const DeltaApplier&) = delete;

    /// False when the hash state could not be allocated. apply() then fails.
    bool valid() const;

    /// Applies the complete operations at the start of @p pending and
    /// returns how many bytes they took; the rest waits for more data.
    /// nullopt for an invalid operation, a wrong hash, data after the
    /// hash, or a target that stopped.
    ///
    /// One call has no step bound: a block range of 13 bytes can copy the
    /// whole basis. The target bounds the work. It stops the delta when it
    /// returns false.
    std::optional<std::size_t> apply(std::string_view pending, DeltaTarget& target);
    /// The file hash arrived and matched.
    bool finished() const { return m_finished; }

private:
    bool output(DeltaTarget& target, std::string_view bytes);

    std::uint64_t m_blockCount;
    std::size_t m_maxLiteral;
    struct Hash;
    std::unique_ptr<Hash> m_hash;
    bool m_finished = false;
};

/// The whole delta of @p file against @p signature (a whole-buffer helper);
/// nullopt for a malformed signature, or when the hash state could not be
/// allocated.
std::optional<std::string> makeDelta(std::string_view signature, std::string_view file);
/// The file that @p delta builds from @p basis (a whole-buffer helper);
/// nullopt when the delta is invalid or incomplete, or when the hash state
/// could not be allocated.
std::optional<std::string> applyDelta(std::string_view basis, std::uint32_t blockSize, std::string_view delta);

} // namespace AfterglowProto
