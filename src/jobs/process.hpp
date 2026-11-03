#pragma once

#include "shell/shell.hpp"

#include <string>
#include <vector>

namespace process {

struct Job {
    int outputFd = -1;
    int processGroup = -1;
    std::vector<int> children;
    int lastStatus = 0;
    bool finished = false;
    bool stopped = false;
};

bool launch(const shell::Pipeline& pipeline, Job& job, std::string& error,
            const std::string& workingDirectory = {});
bool reap(Job& job);

} 
