#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ps2::gs
{

// Pure host-side diagnostic state: not part of emulated GS registers or
// savestates. Tracks a host-to-local transfer across multiple GIF IMAGE packs.
struct s_texture_upload_signature
{
    std::uint64_t serial = 0;
    std::uint64_t bitbltbuf = 0;
    std::uint64_t trxpos = 0;
    std::uint64_t trxreg = 0;
    std::uint64_t expected_bytes = 0;
    std::uint64_t received_bytes = 0;
    std::uint64_t fnv1a64 = 0xcbf29ce484222325ull;
    bool active = false;
};

class c_texture_upload_trace
{
public:
    void begin(std::uint64_t serial, std::uint64_t bitbltbuf,
        std::uint64_t trxpos, std::uint64_t trxreg)
    {
        m_upload = {};
        m_upload.serial = serial;
        m_upload.bitbltbuf = bitbltbuf;
        m_upload.trxpos = trxpos;
        m_upload.trxreg = trxreg;

        const std::uint32_t psm = static_cast<std::uint32_t>(bitbltbuf >> 56) & 0x3fu;
        std::uint32_t bpp = 0;
        switch (psm)
        {
        case 0x00: case 0x30:
            bpp = 32;
            break;
        case 0x01: case 0x31:
            bpp = 24;
            break;
        case 0x02: case 0x0a: case 0x32: case 0x3a:
            bpp = 16;
            break;
        case 0x13: case 0x1b:
            bpp = 8;
            break;
        case 0x14: case 0x24: case 0x2c:
            bpp = 4;
            break;
        default:
            return;
        }

        const std::uint64_t width = trxreg & 0xfffu;
        const std::uint64_t height = (trxreg >> 32) & 0xfffu;
        if (!width || !height) return;

        m_upload.expected_bytes = (width * height * bpp + 7u) / 8u;
        m_upload.active = true;
    }

    // Returns true once exactly the expected transfer payload has arrived.
    // Ignore any qword padding past the declared host-to-local pixel extent.
    bool append(const std::uint8_t *data, std::size_t size)
    {
        if (!m_upload.active || !data) return false;
        const std::size_t remaining = static_cast<std::size_t>(
            m_upload.expected_bytes - m_upload.received_bytes);
        const std::size_t count = std::min(size, remaining);
        for (std::size_t i = 0; i < count; ++i)
        {
            m_upload.fnv1a64 ^= data[i];
            m_upload.fnv1a64 *= 0x100000001b3ull;
        }
        m_upload.received_bytes += count;
        if (m_upload.received_bytes == m_upload.expected_bytes)
        {
            m_upload.active = false;
            return true;
        }
        return false;
    }

    const s_texture_upload_signature &current() const
    {
        return m_upload;
    }

    void reset()
    {
        m_upload = {};
    }

private:
    s_texture_upload_signature m_upload;
};

}
