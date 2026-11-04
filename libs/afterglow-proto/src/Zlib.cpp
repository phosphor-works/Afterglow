// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Zlib.h>

#include <zlib.h>

namespace AfterglowProto {

struct ZlibStream::State
{
    z_stream z{};
    Mode mode;
    bool valid = false;
};

ZlibStream::ZlibStream(Mode mode)
    : m_state(std::make_unique<State>())
{
    m_state->mode = mode;
    m_state->valid = (mode == Mode::Inflate ? inflateInit(&m_state->z) : deflateInit(&m_state->z, Z_DEFAULT_COMPRESSION)) == Z_OK;
}

ZlibStream::~ZlibStream()
{
    if (m_state->valid) {
        if (m_state->mode == Mode::Inflate) {
            inflateEnd(&m_state->z);
        } else {
            deflateEnd(&m_state->z);
        }
    }
}

bool ZlibStream::valid() const
{
    return m_state->valid;
}

void ZlibStream::setInput(std::string_view bytes)
{
    m_state->z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
    m_state->z.avail_in = uInt(bytes.size());
}

std::size_t ZlibStream::inputLeft() const
{
    return m_state->z.avail_in;
}

ZlibStream::Result ZlibStream::step(char* out, std::size_t capacity, std::size_t& produced, bool finish)
{
    produced = 0;
    if (!m_state->valid) {
        return Result::Error;
    }
    auto& z = m_state->z;
    z.next_out = reinterpret_cast<Bytef*>(out);
    z.avail_out = uInt(capacity);
    const int result = m_state->mode == Mode::Inflate ? inflate(&z, Z_NO_FLUSH) : deflate(&z, finish ? Z_FINISH : Z_NO_FLUSH);
    produced = capacity - z.avail_out;
    return result == Z_STREAM_END ? Result::End : result == Z_OK ? Result::Ok : Result::Error;
}

bool zlibInflate(std::string_view in, std::string& out, std::size_t limit)
{
    ZlibStream stream(ZlibStream::Mode::Inflate);
    stream.setInput(in);
    const std::size_t start = out.size();
    char buffer[65536];
    auto result = ZlibStream::Result::Ok;
    while (result == ZlibStream::Result::Ok) {
        std::size_t produced = 0;
        result = stream.step(buffer, sizeof buffer, produced);
        // Checked before the append: a small stream can expand without bound.
        if (produced > limit - (out.size() - start)) {
            result = ZlibStream::Result::Error;
            break;
        }
        out.append(buffer, produced);
    }
    if (result != ZlibStream::Result::End) {
        out.resize(start);
        return false;
    }
    return true;
}

std::string zlibCompress(std::string_view in)
{
    std::string out(compressBound(uLong(in.size())), '\0');
    uLongf size = uLongf(out.size());
    if (compress(reinterpret_cast<Bytef*>(out.data()), &size, reinterpret_cast<const Bytef*>(in.data()), uLong(in.size())) != Z_OK) {
        return {};
    }
    out.resize(size);
    return out;
}

} // namespace AfterglowProto
