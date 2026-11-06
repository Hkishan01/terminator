#include "jobs/job_manager.hpp"

#include <cerrno>
#include <csignal>
#include <string>
#include <ctime>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

using namespace std;

bool JobManager::launch(size_t tabIndex, const shell::Pipeline& pipeline,
                        const string& command, const Output& output,
                        const string& label,
                        const string& workingDirectory) {
    process::Job processJob;
    string error;
    if (!process::launch(pipeline, processJob, error, workingDirectory)) {
        output(tabIndex, error + "\n", {});
        return false;
    }

    if (tabIndex >= foregroundByTab_.size()) foregroundByTab_.resize(tabIndex + 1, -1);
    if (!pipeline.background) foregroundByTab_[tabIndex] = static_cast<int>(jobs_.size());
    Entry entry;
    entry.process = move(processJob);
    entry.tabIndex = tabIndex;
    entry.command = command;
    entry.label = label;
    entry.background = pipeline.background;
    entry.workingDirectory = workingDirectory;
    entry.pipeline = pipeline;
    if (tabIndex >= nextJobIdByTab_.size()) nextJobIdByTab_.resize(tabIndex + 1, 1);
    entry.jobId = nextJobIdByTab_[tabIndex]++;
    const int jobId = entry.jobId;
    jobs_.push_back(move(entry));
    output(tabIndex, "[" + to_string(jobId) + "] started " + command + "\n", {});
    return true;
}

bool JobManager::startWatch(size_t tabIndex, const shell::Pipeline& pipeline,
                            const string& command, const Output& output,
                            const string& workingDirectory) {
    process::Job child;
    string error;
    if (!process::launch(pipeline, child, error, workingDirectory)) {
        output(tabIndex, error + "\n", {});
        return false;
    }
    const size_t index = jobs_.size();
    if (tabIndex >= foregroundByTab_.size()) foregroundByTab_.resize(tabIndex + 1, -1);
    const int current = foregroundByTab_[tabIndex];
    const bool watchAlreadyActive = current >= 0 &&
        static_cast<size_t>(current) < jobs_.size() &&
        jobs_[static_cast<size_t>(current)].watchMode;
    if (!watchAlreadyActive) foregroundByTab_[tabIndex] = static_cast<int>(index);
    Entry entry;
    entry.process = move(child);
    entry.tabIndex = tabIndex;
    entry.command = command;
    entry.background = false;
    entry.repeating = true;
    entry.watchMode = true;
    entry.workingDirectory = workingDirectory;
    entry.pipeline = pipeline;
    if (tabIndex >= nextJobIdByTab_.size()) nextJobIdByTab_.resize(tabIndex + 1, 1);
    entry.jobId = nextJobIdByTab_[tabIndex]++;
    jobs_.push_back(move(entry));
    const auto now = time(nullptr);
    output(tabIndex, "\"" + command + "\", " + to_string(now) + ":\n" +
           string(52, '-') + "\n", {});
    return true;
}

bool JobManager::handleBuiltin(const shell::Pipeline& pipeline,
                               size_t tabIndex, const Output& output) {
    if (pipeline.commands.size() != 1 || pipeline.commands[0].arguments.empty())
        return false;
    const auto& arguments = pipeline.commands[0].arguments;
    const string& name = arguments[0];
    if (name == "jobs") {
        for (size_t i = 0; i < jobs_.size(); ++i) {
            const Entry& job = jobs_[i];
            if (job.tabIndex == tabIndex &&
                (!job.process.finished || job.waitingToRepeat)) {
                const string state = job.process.stopped ? "stopped " :
                    job.background ? "background " : "foreground ";
                output(tabIndex, "[" + to_string(job.jobId) + "] " + state +
                       job.command + "\n", {});
            }
        }
        return true;
    }

    if (name != "fg" && name != "bg") return false;
    int index = -1;
    if (arguments.size() > 1) {
        try {
            string requested = arguments[1];
            if (!requested.empty() && requested.front() == '%') requested.erase(0, 1);
            const int requestedId = stoi(requested);
            index = -1;
            for (size_t i = 0; i < jobs_.size(); ++i)
                if (jobs_[i].jobId == requestedId && jobs_[i].tabIndex == tabIndex)
                    index = static_cast<int>(i);
        } catch (...) {
            index = -1;
        }
    } else {
        for (int i = static_cast<int>(jobs_.size()) - 1; i >= 0; --i) {
            if (jobs_[static_cast<size_t>(i)].tabIndex == tabIndex &&
                !jobs_[static_cast<size_t>(i)].process.finished) {
                index = i;
                break;
            }
        }
    }

    if (index < 0 || static_cast<size_t>(index) >= jobs_.size() ||
        jobs_[static_cast<size_t>(index)].process.finished) {
        output(tabIndex, name + ": no such active job\n", {});
        return true;
    }

    Entry& job = jobs_[static_cast<size_t>(index)];
    kill(-job.process.processGroup, SIGCONT);
    job.process.stopped = false;
    job.stopReported = false;
    job.background = name == "bg";
    if (tabIndex >= foregroundByTab_.size()) foregroundByTab_.resize(tabIndex + 1, -1);
    foregroundByTab_[tabIndex] = job.background ? -1 : index;
    return true;
}

void JobManager::drain(Entry& entry, const Output& output) {
    if (entry.process.outputFd == -1) return;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(entry.process.outputFd, buffer, sizeof(buffer));
        if (count > 0) {
            entry.outputAtLineStart = buffer[count - 1] == '\n';
            output(entry.tabIndex,
                   string(buffer, static_cast<size_t>(count)), entry.label);
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

bool JobManager::startNextRun(Entry& job, const Output& output) {
    process::Job child;
    string error;
    if (!process::launch(job.pipeline, child, error, job.workingDirectory)) {
        output(job.tabIndex, error + "\n", {});
        job.waitingToRepeat = false;
        job.repeating = false;
        return false;
    }

    job.process = move(child);
    job.waitingToRepeat = false;
    output(job.tabIndex, "\"" + job.command + "\", " +
           to_string(time(nullptr)) + ":\n" + string(52, '-') + "\n", {});
    return true;
}

void JobManager::reportStopped(size_t index, Entry& job, const Output& output) {
    if (!job.process.stopped || job.stopReported) return;
    job.stopReported = true;
    job.background = true;
    if (job.tabIndex < foregroundByTab_.size() &&
        foregroundByTab_[job.tabIndex] == static_cast<int>(index))
        foregroundByTab_[job.tabIndex] = -1;
    output(job.tabIndex, "[" + to_string(job.jobId) + "]+ Stopped " +
           job.command + "\n", {});
}

void JobManager::finishJob(size_t index, Entry& job,
                           chrono::steady_clock::time_point now,
                           const Output& output) {
    drain(job, output);
    if (job.process.outputFd != -1) {
        close(job.process.outputFd);
        job.process.outputFd = -1;
    }

    const int status = job.process.lastStatus;
    if (!job.background) {
        if (job.tabIndex >= lastStatusByTab_.size())
            lastStatusByTab_.resize(job.tabIndex + 1, -1);
        lastStatusByTab_[job.tabIndex] = WIFEXITED(status)
            ? WEXITSTATUS(status)
            : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 128;
    }

    const string result = WIFEXITED(status)
        ? "exit " + to_string(WEXITSTATUS(status))
        : WIFSIGNALED(status) ? "signal " + to_string(WTERMSIG(status)) : "stopped";
    if (job.repeating) {
        output(job.tabIndex, (job.outputAtLineStart ? "" : "\n") +
               string(52, '-') + "\n", {});
        job.waitingToRepeat = true;
        job.nextRun = now + chrono::seconds(2);
    } else if (job.watchMode) {
        if (job.tabIndex < foregroundByTab_.size() &&
            foregroundByTab_[job.tabIndex] == static_cast<int>(index))
            foregroundByTab_[job.tabIndex] = -1;
    } else {
        output(job.tabIndex, "[job " + to_string(job.jobId) +
               " finished: " + result + "]\n", {});
        if (job.tabIndex < foregroundByTab_.size() &&
            foregroundByTab_[job.tabIndex] == static_cast<int>(index))
            foregroundByTab_[job.tabIndex] = -1;
    }
}

void JobManager::update(const Output& output) {
    const auto now = chrono::steady_clock::now();
    for (size_t i = 0; i < jobs_.size(); ++i) {
        Entry& job = jobs_[i];
        if (job.waitingToRepeat && now >= job.nextRun && !startNextRun(job, output))
            continue;
        if (job.waitingToRepeat || job.process.finished) continue;
        if (!process::reap(job.process)) {
            drain(job, output);
            reportStopped(i, job, output);
            continue;
        }
        finishJob(i, job, now, output);
    }
}

int JobManager::lastStatus(size_t tabIndex) const {
    return tabIndex < lastStatusByTab_.size() ? lastStatusByTab_[tabIndex] : -1;
}

vector<int> JobManager::outputFds() const {
    vector<int> descriptors;
    for (const Entry& job : jobs_)
        if (job.process.outputFd >= 0) descriptors.push_back(job.process.outputFd);
    return descriptors;
}

void JobManager::closeTab(size_t tabIndex) const {
    for (const Entry& job : jobs_) {
        if (job.tabIndex == tabIndex && !job.process.finished)
            kill(-job.process.processGroup, SIGHUP);
    }
}

void JobManager::signalForeground(int signalNumber, size_t tabIndex) {
    if (tabIndex >= foregroundByTab_.size()) return;
    const int foregroundIndex = foregroundByTab_[tabIndex];
    if (foregroundIndex < 0 || static_cast<size_t>(foregroundIndex) >= jobs_.size()) return;
    const Entry& job = jobs_[static_cast<size_t>(foregroundIndex)];
    if (job.repeating && signalNumber == SIGINT) {
        for (Entry& entry : jobs_) {
            if (entry.watchMode && entry.tabIndex == job.tabIndex) {
                if (!entry.process.finished) kill(-entry.process.processGroup, signalNumber);
                entry.repeating = false;
                entry.waitingToRepeat = false;
            }
        }
        foregroundByTab_[tabIndex] = -1;
        return;
    }
    if (!job.process.finished) kill(-job.process.processGroup, signalNumber);
}

void JobManager::shutdown() const {
    for (const Entry& job : jobs_) {
        if (!job.process.finished) kill(-job.process.processGroup, SIGHUP);
        if (job.process.outputFd != -1) close(job.process.outputFd);
    }
}
