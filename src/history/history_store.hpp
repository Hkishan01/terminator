#pragma once

#include <string>
#include <vector>

namespace history_store {
void initialize();
void shutdown();
void add(const std::string& command);
std::vector<std::string> snapshot();
}
