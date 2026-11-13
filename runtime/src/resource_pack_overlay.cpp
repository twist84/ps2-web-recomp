#include "ps2/resource_pack_overlay.hpp"

#include <mutex>
#include <utility>

namespace ps2::resource_pack_overlay
{
namespace
{
std::mutex g_mutex;
std::vector<std::string> g_lines;
}

void set_lines(std::vector<std::string> lines)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lines = std::move(lines);
}

std::vector<std::string> get_lines()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_lines;
}

void clear()
{
    set_lines({});
}
}
