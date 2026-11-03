#pragma once

#include "jobs/job_manager.hpp"
#include "shell/shell.hpp"
#include "terminal/terminal_view.hpp"

#include <cstddef>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CommandRunner {
public:
    explicit CommandRunner(std::vector<Tab>& tabs, TerminalView& view,
                           bool useTabThreads = true);
    ~CommandRunner();

    void ensureWorkers(std::size_t tabCount);
    void submit(std::size_t tabIndex);
    void moveHistory(std::size_t tabIndex, int direction);
    void closeTab(std::size_t tabIndex);
    void updateJobs();
    std::vector<int> outputFds() const;
    void signalForeground(int signalNumber, std::size_t tabIndex);
    void shutdown() const;

private:
    struct TabWorker {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<std::function<void()>> tasks;
        bool stopping = false;
        std::thread thread;
    };
    void submitOnWorker(std::size_t tabIndex);
    bool handleBuiltin(const shell::Pipeline& pipeline, const std::string& raw,
                       std::size_t tabIndex);
    bool executeCommand(std::size_t tabIndex, const std::string& command,
                        bool echo);
    void appendOutput(std::size_t tabIndex, const std::string& text,
                      const std::string& label = {}) const;

    std::vector<Tab>& tabs_;
    TerminalView& view_;
    JobManager jobs_;
    int sourceDepth_ = 0;
    bool useTabThreads_;
    std::string* builtinCapture_ = nullptr;
    std::vector<std::unique_ptr<TabWorker>> workers_;
};
