#include "jobs/process.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace std;

namespace process {
namespace {

bool hasRedirection(const shell::Command& command, int descriptor) {
    for (const shell::Redirection& redirection : command.redirections) {
        if (redirection.descriptor == descriptor) return true;
    }
    return false;
}

void applyRedirections(const shell::Command& command) {
    for (const shell::Redirection& redirection : command.redirections) {
        int flags = O_RDONLY;
        if (redirection.descriptor != STDIN_FILENO) {
            flags = O_WRONLY | O_CREAT |
                    (redirection.append ? O_APPEND : O_TRUNC);
        }
        const int file = open(redirection.path.c_str(), flags, 0666);
        if (file == -1 || dup2(file, redirection.descriptor) == -1) {
            const string message = "redirection " + redirection.path +
                                        ": " + strerror(errno) + "\n";
            write(STDERR_FILENO, message.data(), message.size());
            _exit(1);
        }
        close(file);
    }
}

void connectDescriptor(int source, int destination) {
    if (source == destination) return;
    if (dup2(source, destination) == -1) {
        const string message = "pipe: " + string(strerror(errno)) + "\n";
        write(STDERR_FILENO, message.data(), message.size());
        _exit(126);
    }
}

[[noreturn]] void executeStage(const shell::Pipeline& pipeline, size_t index,
                               pid_t processGroup, int previousInput,
                               const int nextPipe[2], const int capture[2],
                               const string& workingDirectory) {
    signal(SIGPIPE, SIG_DFL);
    if (processGroup == -1) setpgid(0, 0);
    else setpgid(0, processGroup);
    if (!workingDirectory.empty() && chdir(workingDirectory.c_str()) == -1) {
        const string message = "cd: " + string(strerror(errno)) + "\\n";
        write(STDERR_FILENO, message.data(), message.size());
        _exit(126);
    }
    if (previousInput != -1) connectDescriptor(previousInput, STDIN_FILENO);
    if (nextPipe[1] != -1) connectDescriptor(nextPipe[1], STDOUT_FILENO);
    else if (!hasRedirection(pipeline.commands[index], STDOUT_FILENO))
        connectDescriptor(capture[1], STDOUT_FILENO);
    if (!hasRedirection(pipeline.commands[index], STDERR_FILENO))
        connectDescriptor(capture[1], STDERR_FILENO);

    if (previousInput != -1) close(previousInput);
    if (nextPipe[0] != -1) close(nextPipe[0]);
    if (nextPipe[1] != -1) close(nextPipe[1]);
    close(capture[0]);
    close(capture[1]);
    applyRedirections(pipeline.commands[index]);

    const auto& arguments = pipeline.commands[index].arguments;
    vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const string& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    const string message = errno == ENOENT
        ? "terminator: " + string(argv[0]) + ": command not found\n"
        : string(argv[0]) + ": " + strerror(errno) + "\n";
    write(STDERR_FILENO, message.data(), message.size());
    _exit(127);
}

} 

bool launch(const shell::Pipeline& pipeline, Job& job, string& error,
            const string& workingDirectory) {
    const size_t count = pipeline.commands.size();
    if (count == 0) return true;

    int capture[2];
    if (pipe(capture) == -1) {
        error = "pipe: " + string(strerror(errno));
        return false;
    }

    int previousInput = -1;
    for (size_t index = 0; index < count; ++index) {
        int nextPipe[2] = {-1, -1};
        if (index + 1 < count && pipe(nextPipe) == -1) {
            error = "pipe: " + string(strerror(errno));
            break;
        }

        const pid_t child = fork();
        if (child == -1) {
            error = "fork: " + string(strerror(errno));
            if (nextPipe[0] != -1) {
                close(nextPipe[0]);
                close(nextPipe[1]);
            }
            break;
        }

        if (child == 0)
            executeStage(pipeline, index, job.processGroup, previousInput,
                         nextPipe, capture, workingDirectory);

        if (job.processGroup == -1) job.processGroup = child;
        setpgid(child, job.processGroup);
        job.children.push_back(child);
        if (previousInput != -1) close(previousInput);
        if (nextPipe[1] != -1) close(nextPipe[1]);
        previousInput = nextPipe[0];
    }

    if (previousInput != -1) close(previousInput);
    close(capture[1]);
    if (!error.empty()) {
        close(capture[0]);
        if (job.processGroup != -1) kill(-job.processGroup, SIGTERM);
        return false;
    }
    const int oldFlags = fcntl(capture[0], F_GETFL, 0);
    fcntl(capture[0], F_SETFL, oldFlags | O_NONBLOCK);
    job.outputFd = capture[0];
    return true;
}

bool reap(Job& job) {
    if (job.finished) return true;
    bool allFinished = true;
    for (size_t i = 0; i < job.children.size(); ++i) {
        int status = 0;
        const pid_t result = waitpid(job.children[i], &status,
                                     WNOHANG | WUNTRACED | WCONTINUED);
        if (result == 0) {
            allFinished = false;
        } else if (result == job.children[i]) {
            if (WIFSTOPPED(status)) {
                job.stopped = true;
                if (i + 1 == job.children.size()) job.lastStatus = status;
                allFinished = false;
            } else if (WIFCONTINUED(status)) {
                job.stopped = false;
                allFinished = false;
            } else if (i + 1 == job.children.size()) {
                job.lastStatus = status;
            }
        } else if (result == -1 && errno == ECHILD) {
            continue;
        }
    }
    job.finished = allFinished;
    return job.finished;
}

} 
