#pragma once
#include "aoip/system.hpp"
#include <filesystem>
namespace aoip {
Json read_configuration(const std::filesystem::path&, std::size_t max_bytes = 4 * 1024 * 1024);
void write_configuration(const std::filesystem::path&, const Json&);
std::filesystem::path engine_endpoint_file();
} // namespace aoip
