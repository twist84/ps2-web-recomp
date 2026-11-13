#pragma once

#include "ps2/resource_pack.hpp"

namespace ps2::resource_pack::zip
{

// Index without unpacking. Unsupported or dangerous ZIP features fail closed.
bool index(const std::filesystem::path &archive,
    std::vector<s_zip_entry> &entries);

// Reads one verified central-directory entry, including ZIP deflate (method 8).
bool read(const std::filesystem::path &archive, const s_zip_entry &entry,
    std::vector<std::uint8_t> &output);

}
