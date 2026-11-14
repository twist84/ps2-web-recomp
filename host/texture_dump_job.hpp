#pragma once

#include <string>

namespace sdbz::texture_dump_job
{
struct s_status
{
    bool running = false;
    bool complete = false;
    int exit_code = 0;
    std::string message;
};

// Starts one non-blocking export. Paths are discovered from the active disc
// root and the executable directory; output is a new folder resource pack.
bool start(const std::string &disc_path, const std::string &executable_directory);
s_status status();
}
