#include "shell/command_runner.hpp"
#include "history/history_store.hpp"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <future>
#include <cstring>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <cctype>
#include <fcntl.h>

using namespace std;

namespace {
string trim(string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

string unquoteWatchCommand(string command) {
    command = trim(std::move(command));
    if (command.size() >= 2 &&
        (command.front() == '\'' || command.front() == '"') &&
        command.back() == command.front())
        return command.substr(1, command.size() - 2);
    return command;
}

vector<string> splitWatchList(const string& list) {
    vector<string> commands;
    string current;
    char quote = 0;
    bool escaped = false;
    for (char ch : list) {
        if (escaped) { current += ch; escaped = false; continue; }
        if (ch == '\\' && quote != '\'') { escaped = true; current += ch; continue; }
        if (quote) {
            current += ch;
            if (ch == quote) quote = 0;
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
            current += ch;
        } else if (ch == ',') {
            commands.push_back(unquoteWatchCommand(current));
            current.clear();
        } else current += ch;
    }
    string command = unquoteWatchCommand(current);
    if (!command.empty()) commands.push_back(command);
    return commands;
}

vector<string> watchCommands(const string& raw) {
    const auto space = raw.find_first_of(" \t");
    if (space == string::npos) return {};
    const string rest = trim(raw.substr(space + 1));
    if (rest.find(":::") != string::npos) {
        vector<string> commands;
        size_t start = 0;
        for (;;) {
            const size_t separator = rest.find(":::", start);
            commands.push_back(trim(rest.substr(start,
                separator == string::npos ? separator : separator - start)));
            if (separator == string::npos) break;
            start = separator + 3;
        }
        return commands;
    }
    if (rest.size() < 2 || rest.front() != '[' || rest.back() != ']') return {};
    return splitWatchList(rest.substr(1, rest.size() - 2));
}

void continueInput(Tab& tab) {
    tab.input.insert(tab.inputCursor, 1, '\n');
    ++tab.inputCursor;
}

void resetSubmittedInput(Tab& tab) {
    tab.input.clear();
    tab.inputCursor = 0;
    tab.horizontalOffset = 0;
}

void recordSubmittedCommand(const string& command, Tab& tab) {
    if (command.find_first_not_of(" \t\n") != string::npos)
        history_store::add(command);
    tab.inputCursor = 0;
    tab.historyCursor = history_store::snapshot().size();
    tab.historyDraft.clear();
}
} 

CommandRunner::CommandRunner(vector<Tab>& tabs, TerminalView& view,
                             bool useTabThreads)
    : tabs_(tabs), view_(view), useTabThreads_(useTabThreads) {
    history_store::initialize();
}

CommandRunner::~CommandRunner() {
    for (auto& worker : workers_) {
        if (!worker) continue;
        {
            lock_guard<mutex> lock(worker->mutex);
            worker->stopping = true;
        }
        worker->ready.notify_one();
    }
    for (auto& worker : workers_)
        if (worker && worker->thread.joinable()) worker->thread.join();
}

void CommandRunner::ensureWorkers(size_t tabCount) {
    while (workers_.size() < tabCount) {
        auto worker = make_unique<TabWorker>();
        TabWorker* slot = worker.get();
        slot->thread = thread([slot] {
            for (;;) {
                function<void()> task;
                {
                    unique_lock<mutex> lock(slot->mutex);
                    slot->ready.wait(lock, [slot] {
                        return slot->stopping || !slot->tasks.empty();
                    });
                    if (slot->stopping && slot->tasks.empty()) return;
                    task = std::move(slot->tasks.front());
                    slot->tasks.pop_front();
                }
                task();
            }
        });
        workers_.push_back(std::move(worker));
    }
}

void CommandRunner::appendOutput(size_t tabIndex, const string& text,
                                 const string& label) const {
    if (builtinCapture_ != nullptr) {
        builtinCapture_->append(text);
        return;
    }
    if (tabIndex < tabs_.size())
        view_.appendOutput(tabs_[tabIndex], text, label);
}

bool CommandRunner::handleBuiltin(const shell::Pipeline& pipeline,
                                  const string& raw,
                                  size_t tabIndex) {
    if (pipeline.commands.size() != 1 || pipeline.commands[0].arguments.empty())
        return false;

    const auto& arguments = pipeline.commands[0].arguments;
    const string& name = arguments[0];
    const JobManager::Output output = [this](size_t index,
        const string& text, const string& label) {
        appendOutput(index, text, label);
    };

    if (jobs_.handleBuiltin(pipeline, tabIndex, output)) return true;

    if (name == "exit") {
        if (arguments.size() > 1) {
            try { tabs_[tabIndex].lastExitStatus = stoi(arguments[1]); }
            catch (...) { tabs_[tabIndex].lastExitStatus = 2; }
        }
        tabs_[tabIndex].exitRequested = true;
        return true;
    }

    if (name == "cd") {
        const char* destination = arguments.size() > 1
            ? arguments[1].c_str() : getenv("HOME");
        if (destination == nullptr) {
            appendOutput(tabIndex, "cd: HOME is not set\n");
            tabs_[tabIndex].lastExitStatus = 1;
        } else {
            string destinationPath(destination);
            if (destinationPath == "~") destinationPath = getenv("HOME") ? getenv("HOME") : destinationPath;
            else if (destinationPath.rfind("~/", 0) == 0 && getenv("HOME"))
                destinationPath = string(getenv("HOME")) + destinationPath.substr(1);
            filesystem::path target(destinationPath);
            if (target.is_relative()) target = filesystem::path(tabs_[tabIndex].workingDirectory) / target;
            error_code ec;
            const auto resolved = filesystem::canonical(target, ec);
            if (ec || !filesystem::is_directory(resolved, ec)) {
                appendOutput(tabIndex, "cd: " + destinationPath + ": " + strerror(ENOENT) + "\n");
                tabs_[tabIndex].lastExitStatus = 1;
            } else {
                for (Tab& tab : tabs_) tab.workingDirectory = resolved.string();
                if (!useTabThreads_ && chdir(resolved.c_str()) == -1) {
                    appendOutput(tabIndex, "cd: " + string(strerror(errno)) + "\n");
                    tabs_[tabIndex].lastExitStatus = 1;
                    return true;
                }
                tabs_[tabIndex].lastExitStatus = 0;
            }
        }
        return true;
    }

    if (name == "history") {
        const auto history = history_store::snapshot();
        const size_t begin = history.size() > 1000 ? history.size() - 1000 : 0;
        for (size_t i = begin; i < history.size(); ++i)
            appendOutput(tabIndex, to_string(i + 1) + "  " + history[i] + "\n");
        return true;
    }

    if (name == "source") {
        if (arguments.size() < 2) {
            appendOutput(tabIndex, "source: filename required\n");
            tabs_[tabIndex].lastExitStatus = 2;
            return true;
        }
        if (sourceDepth_ >= 32) {
            appendOutput(tabIndex, "source: maximum nesting depth exceeded\n");
            tabs_[tabIndex].lastExitStatus = 2;
            return true;
        }
        filesystem::path path(arguments[1]);
        if (path.is_relative()) path = filesystem::path(tabs_[tabIndex].workingDirectory) / path;
        ifstream script(path);
        if (!script) {
            appendOutput(tabIndex, "source: " + path.string() + ": " + strerror(errno) + "\n");
            tabs_[tabIndex].lastExitStatus = 1;
            return true;
        }
        ++sourceDepth_;
        string pending;
        string line;
        while (getline(script, line)) {
            if (pending.empty()) {
                const string candidate = trim(line);
                if (candidate.empty() || candidate[0] == '#') continue;
            }
            if (!pending.empty()) pending.push_back('\n');
            pending += line;
            const auto parsed = shell::parse(pending, tabs_[tabIndex].lastExitStatus,
                                             true, tabs_[tabIndex].workingDirectory);
            if (parsed.state == shell::ParseState::Incomplete) continue;
            executeCommand(tabIndex, pending, false);
            pending.clear();
            if (tabs_[tabIndex].exitRequested) break;
        }
        if (!pending.empty()) {
            appendOutput(tabIndex, "source: syntax error: incomplete command at end of file\n");
            tabs_[tabIndex].lastExitStatus = 2;
        }
        --sourceDepth_;
        return true;
    }

    if (name == "multiWatch") {
        const auto commands = watchCommands(raw);
        if (commands.empty()) {
            appendOutput(tabIndex, "usage: multiWatch [\"command 1\", \"command 2\", ...]\n");
            return true;
        }
        for (const string& command : commands) {
            shell::ParseResult parsed = shell::parse(command,
                tabs_[tabIndex].lastExitStatus, true, tabs_[tabIndex].workingDirectory);
            if (parsed.state == shell::ParseState::Complete &&
                !parsed.pipeline.commands.empty()) {
                jobs_.startWatch(tabIndex, parsed.pipeline, command, output,
                                 tabs_[tabIndex].workingDirectory);
            } else {
                appendOutput(tabIndex,
                    "multiWatch: invalid command: " + command + "\n");
            }
        }
        return true;
    }
    return false;
}

void CommandRunner::submit(size_t tabIndex) {
    if (!useTabThreads_) {
        submitOnWorker(tabIndex);
        return;
    }
    ensureWorkers(tabIndex + 1);
    auto task = make_shared<packaged_task<void()>>(
        [this, tabIndex] { submitOnWorker(tabIndex); });
    future<void> done = task->get_future();
    TabWorker& worker = *workers_[tabIndex];
    {
        lock_guard<mutex> lock(worker.mutex);
        worker.tasks.emplace_back([task] { (*task)(); });
    }
    worker.ready.notify_one();
    done.get();
}

void CommandRunner::submitOnWorker(size_t tabIndex) {
    if (tabIndex >= tabs_.size()) return;
    Tab& tab = tabs_[tabIndex];
    const string command = tab.input;
    tab.scrollBack = 0;
    shell::ParseResult parsed = shell::parse(command, tab.lastExitStatus,
                                             true, tab.workingDirectory);
    if (parsed.state == shell::ParseState::Incomplete) {
        continueInput(tab);
        return;
    }

    resetSubmittedInput(tab);
    if (parsed.state == shell::ParseState::Invalid) {
        appendOutput(tabIndex, "syntax error: " + parsed.error + "\n");
        return;
    }
    if (parsed.pipeline.commands.empty()) return;

    recordSubmittedCommand(command, tab);
    executeCommand(tabIndex, command, true);
}

bool CommandRunner::executeCommand(size_t tabIndex,
                                   const string& command, bool echo) {
    if (tabIndex >= tabs_.size()) return false;
    Tab& tab = tabs_[tabIndex];
    shell::ParseResult parsed = shell::parse(command, tab.lastExitStatus,
                                             true, tab.workingDirectory);
    if (parsed.state != shell::ParseState::Complete) {
        appendOutput(tabIndex, "syntax error: " +
            (parsed.error.empty() ? "incomplete command" : parsed.error) + "\n");
        tab.lastExitStatus = 2;
        return false;
    }
    if (parsed.pipeline.commands.empty()) return true;
    if (echo) appendOutput(tabIndex, "> " + command + "\n");
    string captured;
    string* previousCapture = builtinCapture_;
    builtinCapture_ = &captured;
    const bool builtin = handleBuiltin(parsed.pipeline, command, tabIndex);
    builtinCapture_ = previousCapture;
    if (builtin) {
        const shell::Redirection* outputRedirection = nullptr;
        for (const auto& redirection : parsed.pipeline.commands.front().redirections)
            if (redirection.descriptor == STDOUT_FILENO ||
                redirection.descriptor == STDERR_FILENO)
                outputRedirection = &redirection;
        if (outputRedirection != nullptr) {
            filesystem::path path(outputRedirection->path);
            if (path.is_relative()) path =
                filesystem::path(tab.workingDirectory) / path;
            const int flags = O_WRONLY | O_CREAT |
                (outputRedirection->append ? O_APPEND : O_TRUNC);
            const int fd = open(path.c_str(), flags, 0666);
            if (fd == -1) {
                appendOutput(tabIndex, "redirection " + path.string() + ": " +
                             strerror(errno) + "\n");
                tab.lastExitStatus = 1;
            } else {
                size_t written = 0;
                while (written < captured.size()) {
                    const ssize_t count = write(fd, captured.data() + written,
                                                captured.size() - written);
                    if (count > 0) written += static_cast<size_t>(count);
                    else if (count == -1 && errno == EINTR) continue;
                    else break;
                }
                close(fd);
            }
        } else if (!captured.empty()) {
            appendOutput(tabIndex, captured);
        }
        return true;
    }
    const JobManager::Output output = [this](size_t index,
        const string& text, const string& label) {
        appendOutput(index, text, label);
    };
    const bool launched = jobs_.launch(tabIndex, parsed.pipeline, command, output, {},
                                       tab.workingDirectory);
    if (!launched) tab.lastExitStatus = 1;
    return launched;
}

void CommandRunner::moveHistory(size_t tabIndex, int direction) {
    const auto history = history_store::snapshot();
    if (tabIndex >= tabs_.size() || history.empty()) return;
    Tab& tab = tabs_[tabIndex];
    if (tab.historyCursor > history.size()) tab.historyCursor = history.size();
    if (direction < 0) {
        if (tab.historyCursor >= history.size()) {
            tab.historyCursor = history.size();
            tab.historyDraft = tab.input;
        }
        if (tab.historyCursor > 0) --tab.historyCursor;
        tab.input = history[tab.historyCursor];
    } else if (tab.historyCursor < history.size()) {
        ++tab.historyCursor;
        tab.input = tab.historyCursor == history.size()
            ? tab.historyDraft : history[tab.historyCursor];
    }
    tab.inputCursor = tab.input.size();
    view_.keepInputCursorVisible(tab);
}

void CommandRunner::closeTab(size_t tabIndex) {
    jobs_.closeTab(tabIndex);
    if (tabIndex >= workers_.size() || !workers_[tabIndex]) return;
    TabWorker& worker = *workers_[tabIndex];
    {
        lock_guard<mutex> lock(worker.mutex);
        worker.stopping = true;
    }
    worker.ready.notify_one();
    if (worker.thread.joinable()) worker.thread.join();
    workers_[tabIndex].reset();
}

void CommandRunner::updateJobs() {
    const JobManager::Output output = [this](size_t index,
        const string& text, const string& label) {
        appendOutput(index, text, label);
    };
    jobs_.update(output);
    for (size_t i = 0; i < tabs_.size(); ++i) {
        const int status = jobs_.lastStatus(i);
        if (status >= 0) tabs_[i].lastExitStatus = status;
    }
}

vector<int> CommandRunner::outputFds() const {
    return jobs_.outputFds();
}

void CommandRunner::signalForeground(int signalNumber, size_t tabIndex) {
    jobs_.signalForeground(signalNumber, tabIndex);
}

void CommandRunner::shutdown() const { jobs_.shutdown(); }
