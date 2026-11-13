#pragma once

#include <string>
#include <vector>

namespace ps2::resource_pack_overlay
{

// Thread-safe snapshot passed from the SDL input/UI thread to WebGPU
// presentation. Renderer never owns or mutates the pack selection.
void set_lines(std::vector<std::string> lines);
std::vector<std::string> get_lines();
void clear();

}
