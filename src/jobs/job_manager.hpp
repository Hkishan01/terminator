#pragma once

#include "jobs/process.hpp"

#include <cstddef>
#include <functional>
#include <chrono>
#include <string>
#include <vector>

class JobManager {
public:
    using Output = std::function<void(std::size_t, const std::string&,
                                      const std::string&)>;

    bool launch(std::size_t tabIndex, const shell::Pipeline& pipeline,
                const std::string& command, const Output& output,
                const std::string& label = {},
                const std::string& workingDirectory = {});
    bool startWatch(std::size_t tabIndex, const shell::Pipeline& pipeline,
                    const std::string& command, const Output& output,
                    const std::string& workingDirectory);
    bool handleBuiltin(const shell::Pipeline& pipeline, std::size_t tabIndex,
                       const Output& output);
    void update(const Output& output);
    int lastStatus(std::size_t tabIndex) const;
    std::vector<int> outputFds() const;
    void closeTab(std::size_t tabIndex) const;
    void signalForeground(int signalNumber, std::size_t tabIndex);
    void shutdown() const;

private:
    struct Entry {
        process::Job process;
        std::size_t tabIndex;
        std::string command;
        std::string label;
        bool background;
        bool repeating = false;
        bool watchMode = false;
        bool waitingToRepeat = false;
        std::chrono::steady_clock::time_point nextRun{};
        std::string workingDirectory;
        shell::Pipeline pipeline;
        bool stopReported = false;
        int jobId = 0;
        bool outputAtLineStart = true;
    };

    void drain(Entry& entry, const Output& output);
    bool startNextRun(Entry& job, const Output& output);
    void reportStopped(std::size_t index, Entry& job, const Output& output);
    void finishJob(std::size_t index, Entry& job,
                   std::chrono::steady_clock::time_point now,
                   const Output& output);

    std::vector<Entry> jobs_;
    std::vector<int> foregroundByTab_;
    std::vector<int> nextJobIdByTab_;
    std::vector<int> lastStatusByTab_;
};
