#pragma once

#include "terminal/terminal_view.hpp"

#include <cstddef>
#include <string>
#include <vector>

class ProcessTabManager {
public:
    ProcessTabManager() = default;
    ~ProcessTabManager();

    bool start(std::size_t index, const Tab& tab, int xConnectionFd);
    bool submit(std::size_t index, Tab& tab);
    void signal(std::size_t index, int signalNumber);
    void update(std::vector<Tab>& tabs, const TerminalView& view);
    std::vector<int> outputFds() const;
    void close(std::size_t index);
    void shutdown();

private:
    struct Worker {
        int commandFd = -1;
        int outputFd = -1;
        int pid = -1;
        std::string pending;
        bool closed = false;
    };
    [[noreturn]] void runChild(const int commands[2], const int outputs[2],
                               int xConnectionFd, const Tab& tab);
    void processPending(std::size_t index, Worker& worker, std::vector<Tab>& tabs,
                        const TerminalView& view);
    std::vector<Worker> workers_;
};
