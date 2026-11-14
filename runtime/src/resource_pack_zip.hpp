#pragma once

#include "ps2/resource_pack.hpp"

namespace ps2::resource_pack::zip
{

// Resource packs stay small by default. Full GAME/ regional ZIPs can use
// higher limits without weakening restrictions for third-party packs.
// ZIP64, encrypted archives and unsupported compression remain rejected.
struct s_limits
{
    std::uint64_t m_max_archive = 512ull * 1024 * 1024;
    std::uint64_t m_max_expanded = 512ull * 1024 * 1024;
    std::uint32_t m_max_file = 64u * 1024 * 1024;
    std::uint32_t m_max_entries = 4096;
    std::uint32_t m_max_central = 8u * 1024 * 1024;
};

// Index without unpacking. Unsupported or dangerous ZIP features fail closed.
bool index(const std::filesystem::path &archive,
    std::vector<s_zip_entry> &entries, const s_limits &limits = {});

// Reads one verified central-directory entry, including ZIP deflate (method 8).
bool read(const std::filesystem::path &archive, const s_zip_entry &entry,
    std::vector<std::uint8_t> &output, const s_limits &limits = {});

}
