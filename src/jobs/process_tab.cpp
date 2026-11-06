#include "jobs/process_tab.hpp"

#include "shell/command_runner.hpp"
#include "shell/shell.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std;

namespace {

bool writeAll(int fd, const void* data, size_t size) {
    const char* bytes = static_cast<const char*>(data);
    while (size) {
        const ssize_t n = write(fd, bytes, size);
        if (n > 0) { bytes += n; size -= static_cast<size_t>(n); }
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

bool readAll(int fd, void* data, size_t size) {
    char* bytes = static_cast<char*>(data);
    while (size) {
        const ssize_t n = read(fd, bytes, size);
        if (n > 0) { bytes += n; size -= static_cast<size_t>(n); }
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

bool writeFrame(int fd, char type, const void* data, uint32_t size) {
    char header[5] = {type, 0, 0, 0, 0};
    memcpy(header + 1, &size, sizeof(size));
    return writeAll(fd, header, sizeof(header)) && (!size || writeAll(fd, data, size));
}

void flushOutput(const Tab& tab, int fd, size_t& lineIndex,
                 size_t& charsSent) {
    while (lineIndex < tab.output.size()) {
        const string& line = tab.output[lineIndex];
        if (line.size() > charsSent) {
            const string fragment = line.substr(charsSent);
            if (!writeFrame(fd, 'O', fragment.data(), static_cast<uint32_t>(fragment.size()))) return;
            charsSent = line.size();
        }
        if (lineIndex + 1 < tab.output.size()) {
            const char newline = '\n';
            if (!writeFrame(fd, 'O', &newline, 1)) return;
            ++lineIndex;
            charsSent = 0;
        } else break;
    }
}

[[noreturn]] void childLoop(int commandFd, int outputFd, const string& title,
                            const string& workingDirectory) {
    fcntl(commandFd, F_SETFD, FD_CLOEXEC);
    fcntl(outputFd, F_SETFD, FD_CLOEXEC);
    if (!workingDirectory.empty()) chdir(workingDirectory.c_str());
    vector<Tab> tabs;
    tabs.emplace_back(title);
    tabs[0].workingDirectory = workingDirectory;
    TerminalView headless(nullptr, 0, 0, nullptr, 900, 620);
    CommandRunner runner(tabs, headless, false);
    size_t sentLine = 0;
    size_t sentChars = tabs[0].output.empty() ? 0 : tabs[0].output[0].size();
    string lastReportedDirectory = tabs[0].workingDirectory;
    bool running = true;
    while (running) {
        vector<pollfd> fds{{commandFd, POLLIN | POLLHUP, 0}};
        for (int fd : runner.outputFds()) fds.push_back({fd, POLLIN | POLLHUP, 0});
        poll(fds.data(), fds.size(), 100);
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char type = 0;
            uint32_t length = 0;
            if (!readAll(commandFd, &type, 1) || !readAll(commandFd, &length, sizeof(length))) break;
            if (length > 4 * 1024 * 1024) break;
            string payload(length, '\0');
            if (length && !readAll(commandFd, payload.data(), length)) break;
            if (type == 'Q') break;
            if (type == 'D') {
                tabs[0].workingDirectory = move(payload);
                continue;
            }
            if (type == 'S' && length == sizeof(int)) {
                int signalNumber = 0;
                memcpy(&signalNumber, payload.data(), sizeof(signalNumber));
                runner.signalForeground(signalNumber, 0);
            } else if (type == 'C') {
                tabs[0].input = move(payload);
                tabs[0].inputCursor = tabs[0].input.size();
                runner.submit(0);
            }
        }
        runner.updateJobs();
        if (tabs[0].workingDirectory != lastReportedDirectory) {
            lastReportedDirectory = tabs[0].workingDirectory;
            writeFrame(outputFd, 'D', lastReportedDirectory.data(),
                       static_cast<uint32_t>(lastReportedDirectory.size()));
        }
        flushOutput(tabs[0], outputFd, sentLine, sentChars);
        if (tabs[0].exitRequested) {
            writeFrame(outputFd, 'X', nullptr, 0);
            break;
        }
    }
    runner.shutdown();
    close(commandFd);
    close(outputFd);
    _exit(0);
}

bool createWorkerPipes(int commands[2], int outputs[2]) {
    if (pipe(commands) == -1) return false;
    if (pipe(outputs) == -1) {
        ::close(commands[0]);
        ::close(commands[1]);
        return false;
    }
    return true;
}

} 

ProcessTabManager::~ProcessTabManager() { shutdown(); }

bool ProcessTabManager::start(size_t index, const Tab& tab, int xConnectionFd) {
    ::signal(SIGPIPE, SIG_IGN);
    if (index >= workers_.size()) workers_.resize(index + 1);
    Worker& worker = workers_[index];
    if (worker.pid > 0) return true;
    int commands[2];
    int outputs[2];
    if (!createWorkerPipes(commands, outputs)) return false;
    const pid_t pid = fork();
    if (pid == -1) {
        ::close(commands[0]); ::close(commands[1]);
        ::close(outputs[0]); ::close(outputs[1]);
        return false;
    }
    if (pid == 0) runChild(commands, outputs, xConnectionFd, tab);
    ::close(commands[0]);
    ::close(outputs[1]);
    const int flags = fcntl(outputs[0], F_GETFL, 0);
    if (flags >= 0) fcntl(outputs[0], F_SETFL, flags | O_NONBLOCK);
    worker.commandFd = commands[1];
    worker.outputFd = outputs[0];
    worker.pid = static_cast<int>(pid);
    worker.closed = false;
    return true;
}

[[noreturn]] void ProcessTabManager::runChild(const int commands[2],
                                              const int outputs[2],
                                              int xConnectionFd,
                                              const Tab& tab) {
    ::close(commands[1]);
    ::close(outputs[0]);
    ::close(xConnectionFd);
    for (const Worker& existing : workers_) {
        if (existing.commandFd >= 0) ::close(existing.commandFd);
        if (existing.outputFd >= 0) ::close(existing.outputFd);
    }
    childLoop(commands[0], outputs[1], tab.title, tab.workingDirectory);
}

bool ProcessTabManager::submit(size_t index, Tab& tab) {
    if (index >= workers_.size() || workers_[index].pid <= 0) return false;
    const string command = tab.input;
    if (shell::parse(command, tab.lastExitStatus, false,
                     tab.workingDirectory).state == shell::ParseState::Incomplete) {
        tab.input.insert(tab.inputCursor, 1, '\n');
        ++tab.inputCursor;
        return true;
    }
    tab.input.clear();
    tab.inputCursor = 0;
    tab.scrollBack = 0;
    const uint32_t length = static_cast<uint32_t>(command.size());
    const uint32_t directoryLength = static_cast<uint32_t>(tab.workingDirectory.size());
    char directoryHeader[5] = {'D', 0, 0, 0, 0};
    memcpy(directoryHeader + 1, &directoryLength, sizeof(directoryLength));
    if (!writeAll(workers_[index].commandFd, directoryHeader, sizeof(directoryHeader)) ||
        !writeAll(workers_[index].commandFd, tab.workingDirectory.data(), directoryLength))
        return false;
    char header[5] = {'C', 0, 0, 0, 0};
    memcpy(header + 1, &length, sizeof(length));
    return writeAll(workers_[index].commandFd, header, sizeof(header)) &&
           writeAll(workers_[index].commandFd, command.data(), command.size());
}

void ProcessTabManager::signal(size_t index, int signalNumber) {
    if (index >= workers_.size() || workers_[index].pid <= 0) return;
    const uint32_t length = sizeof(signalNumber);
    char header[5] = {'S', 0, 0, 0, 0};
    memcpy(header + 1, &length, sizeof(length));
    writeAll(workers_[index].commandFd, header, sizeof(header));
    writeAll(workers_[index].commandFd, &signalNumber, sizeof(signalNumber));
}

void ProcessTabManager::update(vector<Tab>& tabs, const TerminalView& view) {
    for (size_t i = 0; i < workers_.size() && i < tabs.size(); ++i) {
        Worker& worker = workers_[i];
        if (worker.outputFd < 0) continue;
        char buffer[8192];
        for (;;) {
            const ssize_t count = read(worker.outputFd, buffer, sizeof(buffer));
            if (count > 0) worker.pending.append(buffer, static_cast<size_t>(count));
            else if (count == -1 && errno == EINTR) continue;
            else if (count == 0) {
                worker.closed = true;
                tabs[i].exitRequested = true;
                break;
            }
            else break;
        }
        processPending(i, worker, tabs, view);
    }
}

void ProcessTabManager::processPending(size_t index, Worker& worker,
                                       vector<Tab>& tabs,
                                       const TerminalView& view) {
    size_t consumed = 0;
    while (worker.pending.size() - consumed >= 5) {
        const char type = worker.pending[consumed];
        uint32_t length = 0;
        memcpy(&length, worker.pending.data() + consumed + 1, sizeof(length));
        if (length > 4 * 1024 * 1024) {
            worker.closed = true;
            break;
        }
        if (worker.pending.size() - consumed < 5 + length) break;
        if (type == 'O') {
            view.appendOutput(tabs[index], worker.pending.substr(consumed + 5, length));
        } else if (type == 'D') {
            const string directory = worker.pending.substr(consumed + 5, length);
            for (Tab& tab : tabs) tab.workingDirectory = directory;
        } else if (type == 'X') {
            tabs[index].exitRequested = true;
        }
        consumed += 5 + length;
    }
    if (consumed > 0) worker.pending.erase(0, consumed);
}

vector<int> ProcessTabManager::outputFds() const {
    vector<int> fds;
    for (const Worker& worker : workers_)
        if (worker.outputFd >= 0) fds.push_back(worker.outputFd);
    return fds;
}

void ProcessTabManager::close(size_t index) {
    if (index >= workers_.size()) return;
    Worker& worker = workers_[index];
    if (worker.pid <= 0) return;
    writeFrame(worker.commandFd, 'Q', nullptr, 0);
    int status = 0;
    pid_t result = 0;
    const auto deadline = chrono::steady_clock::now() + chrono::seconds(2);
    char discard[8192];
    while (chrono::steady_clock::now() < deadline) {
        while (worker.outputFd >= 0 && read(worker.outputFd, discard, sizeof(discard)) > 0) {}
        result = waitpid(worker.pid, &status, WNOHANG);
        if (result == worker.pid || (result == -1 && errno == ECHILD)) break;
        pollfd ready{worker.outputFd, POLLIN | POLLHUP, 0};
        poll(&ready, 1, 20);
    }
    if (result == 0) {
        kill(worker.pid, SIGTERM);
        waitpid(worker.pid, &status, 0);
    }
    if (worker.commandFd >= 0) ::close(worker.commandFd);
    if (worker.outputFd >= 0) ::close(worker.outputFd);
    worker = Worker{};
}

void ProcessTabManager::shutdown() {
    for (size_t i = 0; i < workers_.size(); ++i) close(i);
}
