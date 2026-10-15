#pragma once

#include <string>

namespace klsp
{

int run_daemon(const std::string& session, const std::string& fifo_path, bool debug);

} // namespace klsp
