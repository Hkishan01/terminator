#pragma once

#include <string>
#include <vector>

namespace shell {

struct Redirection {
    int descriptor;
    std::string path;
    bool append;
};

struct Command {
    std::vector<std::string> arguments;
    std::vector<Redirection> redirections;
};

struct Pipeline {
    std::vector<Command> commands;
    bool background = false;
};

enum class ParseState { Complete, Incomplete, Invalid };

struct ParseResult {
    ParseState state = ParseState::Complete;
    Pipeline pipeline;
    std::string error;
};

ParseResult parse(const std::string& input, int lastExitStatus = 0,
                  bool evaluateSubstitutions = true,
                  const std::string& workingDirectory = {});

} 
