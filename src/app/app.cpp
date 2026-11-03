#include "app/app.hpp"

#include "terminal/clipboard.hpp"
#include "shell/command_runner.hpp"
#include "config/config.hpp"
#include "terminal/terminal_view.hpp"
#include "jobs/process_tab.hpp"
#include "history/history_store.hpp"
#include "shell/shell.hpp"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <string>
#include <vector>

using namespace std;

namespace {

constexpr int kInitialWidth = 900;
constexpr int kInitialHeight = 620;
constexpr int kTabHeight = 36;
constexpr int kTabWidth = 120;

vector<Tab> tabs{Tab("Tab 1")};
size_t activeTab = 0;
Display* display = nullptr;
Window window = 0;
GC gc = 0;
XFontStruct* fontInfo = nullptr;
unique_ptr<TerminalView> terminalView;
unique_ptr<CommandRunner> commandRunner;
unique_ptr<ProcessTabManager> processTabs;
bool processMode = false;
int xConnectionFd = -1;
unique_ptr<Clipboard> clipboard;
size_t pendingPasteTab = numeric_limits<size_t>::max();
Atom closeMessage = None;
vector<Hotkey> hotkeys;
vector<string> hotkeyWarnings;
string hotkeysPath;

void openTab() {
    size_t number = 1;
    for (;; ++number) {
        const string title = "Tab " + to_string(number);
        const bool inUse = any_of(tabs.begin(), tabs.end(), [&](const Tab& tab) {
            return !tab.closed && tab.title == title;
        });
        if (!inUse) {
            tabs.emplace_back(title);
            break;
        }
    }
    activeTab = tabs.size() - 1;
    if (processMode) {
        if (!processTabs->start(activeTab, tabs[activeTab], xConnectionFd)) {
            terminalView->appendOutput(tabs[activeTab], "terminator: could not start tab process\n");
        }
    } else if (commandRunner) commandRunner->ensureWorkers(tabs.size());
}

void moveTabHistory(size_t index, int direction) {
    if (commandRunner) {
        commandRunner->moveHistory(index, direction);
        return;
    }
    if (index >= tabs.size()) return;
    const auto history = history_store::snapshot();
    if (history.empty()) return;
    Tab& tab = tabs[index];
    if (direction < 0) {
        if (tab.historyCursor >= history.size()) {
            tab.historyCursor = history.size();
            tab.historyDraft = tab.input;
        }
        if (tab.historyCursor > 0) tab.input = history[--tab.historyCursor];
    } else if (tab.historyCursor < history.size()) {
        ++tab.historyCursor;
        tab.input = tab.historyCursor == history.size()
            ? tab.historyDraft : history[tab.historyCursor];
    }
    tab.inputCursor = tab.input.size();
    terminalView->keepInputCursorVisible(tab);
}

void switchTab(int direction) {
    if (tabs.empty()) return;
    for (size_t step = 1; step <= tabs.size(); ++step) {
        const size_t index = direction > 0
            ? (activeTab + step) % tabs.size()
            : (activeTab + tabs.size() - (step % tabs.size())) % tabs.size();
        if (!tabs[index].closed) { activeTab = index; return; }
    }
}

void closeTab(size_t index, bool& running) {
    if (index >= tabs.size() || tabs[index].closed) return;
    if (processMode) processTabs->close(index);
    else commandRunner->closeTab(index);
    tabs[index].closed = true;
    if (index == activeTab) switchTab(1);
    const bool anyOpen = any_of(tabs.begin(), tabs.end(),
        [](const Tab& tab) { return !tab.closed; });
    if (!anyOpen) running = false;
}

int findHotkey(const XKeyEvent& event) {
    for (size_t i = 0; i < hotkeys.size(); ++i) {
        if (matchesHotkey(hotkeys[i], event)) return static_cast<int>(i);
    }
    return -1;
}

void handleAppHotkey(const string& action, bool& running) {
    if (action == "quit") running = false;
    else if (action == "new_tab") openTab();
    else if (action == "next_tab") switchTab(1);
    else if (action == "previous_tab") switchTab(-1);
    else if (action == "interrupt" || action == "suspend") {
        const int signal = action == "interrupt" ? SIGINT : SIGTSTP;
        if (processMode) processTabs->signal(activeTab, signal);
        else commandRunner->signalForeground(signal, activeTab);
        terminalView->appendOutput(tabs[activeTab],
            action == "interrupt" ? "^C\n" : "^Z\n");
        if (action == "interrupt") {
            tabs[activeTab].input.clear();
            tabs[activeTab].inputCursor = 0;
        }
    }
}

void moveCursorToLine(Tab& tab, bool toStart) {
    const size_t line = toStart
        ? (tab.inputCursor == 0 ? string::npos : tab.input.rfind('\n', tab.inputCursor - 1))
        : tab.input.find('\n', tab.inputCursor);
    tab.inputCursor = toStart
        ? (line == string::npos ? 0 : line + 1)
        : (line == string::npos ? tab.input.size() : line);
    terminalView->keepInputCursorVisible(tab);
}

void deleteToLineEdge(Tab& tab, bool toStart) {
    const size_t line = toStart
        ? (tab.inputCursor == 0 ? string::npos : tab.input.rfind('\n', tab.inputCursor - 1))
        : tab.input.find('\n', tab.inputCursor);
    const size_t from = toStart ? (line == string::npos ? 0 : line + 1) : tab.inputCursor;
    const size_t to = toStart ? tab.inputCursor
        : (line == string::npos ? tab.input.size() : line);
    tab.input.erase(from, to - from);
    tab.inputCursor = from;
    terminalView->keepInputCursorVisible(tab);
}

void deletePreviousWord(Tab& tab) {
    size_t from = tab.inputCursor;
    while (from > 0 && tab.input[from - 1] == ' ') --from;
    while (from > 0 && tab.input[from - 1] != ' ' && tab.input[from - 1] != '\n') --from;
    tab.input.erase(from, tab.inputCursor - from);
    tab.inputCursor = from;
    terminalView->keepInputCursorVisible(tab);
}

bool handleCursorHotkey(const string& action, Tab& tab) {
    if (action == "cursor_left") {
        if (tab.inputCursor > 0) --tab.inputCursor;
        terminalView->keepInputCursorVisible(tab);
        return true;
    } else if (action == "cursor_right") {
        if (tab.inputCursor < tab.input.size()) ++tab.inputCursor;
        terminalView->keepInputCursorVisible(tab);
        return true;
    } else if (action == "cursor_line_start" || action == "cursor_line_end") {
        moveCursorToLine(tab, action == "cursor_line_start");
        return true;
    }
    return false;
}

bool handleInputEditHotkey(const string& action, Tab& tab) {
    if (action == "history_previous") {
        moveTabHistory(activeTab, -1);
        return true;
    } else if (action == "history_next") {
        moveTabHistory(activeTab, 1);
        return true;
    } else if (action == "delete_to_start" || action == "delete_to_end") {
        deleteToLineEdge(tab, action == "delete_to_start");
        return true;
    } else if (action == "delete_previous_word") {
        deletePreviousWord(tab);
        return true;
    } else if (action == "clear_screen") {
        tab.output.clear();
        tab.scrollBack = 0;
        return true;
    } else if (action == "eof") {
        if (tab.input.empty()) tab.exitRequested = true;
        else if (tab.inputCursor < tab.input.size()) tab.input.erase(tab.inputCursor, 1);
        terminalView->keepInputCursorVisible(tab);
        return true;
    }
    return false;
}

bool handleClipboardHotkey(const string& action, Tab& tab) {
    if (action == "copy") {
        string text = terminalView->selectedText(tab);
        if (text.empty()) text = tab.input;
        clipboard->copy(text);
    } else if (action == "paste") {
        pendingPasteTab = activeTab;
        clipboard->requestPaste();
    } else return false;
    return true;
}

void handleTabHotkey(const string& action, Tab& tab) {
    if (handleCursorHotkey(action, tab)) return;
    if (handleInputEditHotkey(action, tab)) return;
    handleClipboardHotkey(action, tab);
}

KeySym lookupKey(XKeyEvent& key) {
    char text[16];
    KeySym symbol = NoSymbol;
    XLookupString(&key, text, sizeof(text), &symbol, nullptr);
    return symbol == NoSymbol ? XLookupKeysym(&key, 0) : symbol;
}

bool handleSubmitKey(XKeyEvent& key, KeySym symbol, Tab& tab, bool& changed) {
    if (symbol != XK_Return && symbol != XK_KP_Enter) return false;
    if (key.state & ShiftMask) {
        tab.input.insert(tab.inputCursor, 1, '\n');
        ++tab.inputCursor;
    } else if (processMode) {
        processTabs->submit(activeTab, tab);
    } else {
        commandRunner->submit(activeTab);
    }
    changed = true;
    return true;
}

bool handleDeleteKey(KeySym symbol, Tab& tab, bool& changed) {
    if (symbol == XK_BackSpace) {
        if (tab.inputCursor > 0) {
            tab.input.erase(tab.inputCursor - 1, 1);
            --tab.inputCursor;
            changed = true;
        }
        return true;
    }
    if (symbol == XK_Delete) {
        if (tab.inputCursor < tab.input.size()) {
            tab.input.erase(tab.inputCursor, 1);
            changed = true;
        }
        return true;
    }
    return false;
}

bool handleScrollKey(XKeyEvent& key, KeySym symbol, Tab& tab) {
    if (symbol != XK_Page_Up && symbol != XK_Page_Down) return false;
    const bool up = symbol == XK_Page_Up;
    if (key.state & ControlMask)
        terminalView->scrollHorizontal(tab, up ? -80 : 80);
    else
        terminalView->scrollVertical(tab,
            (up ? 1 : -1) * max(1, terminalView->height() / 18 - 4));
    return true;
}

bool insertTypedText(XKeyEvent& key, Tab& tab) {
    if (key.state & ControlMask) return false;
    char text[16];
    KeySym translated;
    const int length = XLookupString(&key, text, sizeof(text), &translated, nullptr);
    if (length <= 0) return false;
    tab.input.insert(tab.inputCursor, text, static_cast<size_t>(length));
    tab.inputCursor += static_cast<size_t>(length);
    return true;
}

void handleInputKey(XKeyEvent& key, KeySym symbol, Tab& tab) {
    bool changed = false;
    if (!handleSubmitKey(key, symbol, tab, changed) &&
        !handleDeleteKey(symbol, tab, changed) &&
        !handleScrollKey(key, symbol, tab))
        changed = insertTypedText(key, tab);
    if (changed) terminalView->keepInputCursorVisible(tab);
}

void handleKey(XKeyEvent& key, bool& running) {
    const KeySym symbol = lookupKey(key);
    const int hotkey = findHotkey(key);
    if (hotkey >= 0) {
        const string& action = hotkeys[static_cast<size_t>(hotkey)].action;
        handleAppHotkey(action, running);
        handleTabHotkey(action, tabs[activeTab]);
        return;
    }
    handleInputKey(key, symbol, tabs[activeTab]);
}

void handleTabClick(const XButtonEvent& button, bool& running) {
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    const size_t slot = static_cast<size_t>(button.x / kTabWidth);
    size_t visible = 0;
    bool selected = false;
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].closed) continue;
        if (visible == slot) {
            if (button.x % kTabWidth >= kTabWidth - 28) closeTab(i, running);
            else activeTab = i;
            selected = true;
            break;
        }
        ++visible;
    }
    if (!selected && button.x >= static_cast<int>(visible) * kTabWidth) openTab();
}

void handlePaste(const XEvent& event) {
    string pasted;
    clipboard->handleEvent(event, pasted);
    if (pasted.empty() || pendingPasteTab >= tabs.size()) return;

    string normalized;
    normalized.reserve(pasted.size());
    for (size_t i = 0; i < pasted.size(); ++i) {
        if (pasted[i] == '\r') {
            if (i + 1 < pasted.size() && pasted[i + 1] == '\n') ++i;
            normalized.push_back('\n');
        } else {
            normalized.push_back(pasted[i]);
        }
    }
    Tab& tab = tabs[pendingPasteTab];
    tab.input.insert(tab.inputCursor, normalized);
    tab.inputCursor += normalized.size();
    terminalView->keepInputCursorVisible(tab);
    pendingPasteTab = numeric_limits<size_t>::max();
}

bool handleScrollButton(const XButtonEvent& button) {
    if (button.button == Button4) {
        if (button.state & ShiftMask)
            terminalView->scrollHorizontal(tabs[activeTab], -48);
        else terminalView->scrollVertical(tabs[activeTab], 3);
    } else if (button.button == Button5) {
        if (button.state & ShiftMask)
            terminalView->scrollHorizontal(tabs[activeTab], 48);
        else terminalView->scrollVertical(tabs[activeTab], -3);
    } else if (button.button == 6) {
        terminalView->scrollHorizontal(tabs[activeTab], -48);
    } else if (button.button == 7) {
        terminalView->scrollHorizontal(tabs[activeTab], 48);
    } else {
        return false;
    }
    return true;
}

void handleMouseEvent(XEvent& event, bool& running) {
    if (event.type == ButtonPress && handleScrollButton(event.xbutton)) return;
    if (event.type == ButtonPress && event.xbutton.button == Button2) {
        XSetInputFocus(display, window, RevertToParent, CurrentTime);
        pendingPasteTab = activeTab;
        clipboard->requestPaste(true);
    } else if (event.type == ButtonPress && event.xbutton.y < kTabHeight) {
        handleTabClick(event.xbutton, running);
    } else if (event.type == ButtonPress && event.xbutton.button == Button1) {
        XSetInputFocus(display, window, RevertToParent, CurrentTime);
        terminalView->beginSelection(tabs[activeTab], event.xbutton.x,
                                     event.xbutton.y);
    } else if (event.type == MotionNotify) {
        terminalView->updateSelection(tabs[activeTab], event.xmotion.x,
                                      event.xmotion.y);
    } else if (event.type == ButtonRelease && event.xbutton.button == Button1) {
        terminalView->updateSelection(tabs[activeTab], event.xbutton.x,
                                      event.xbutton.y);
        terminalView->endSelection();
        const string selected = terminalView->selectedText(tabs[activeTab]);
        if (!selected.empty()) clipboard->copy(selected);
    } else if (event.type == ButtonPress) {
        XSetInputFocus(display, window, RevertToParent, CurrentTime);
    }
}

void handleEvent(XEvent& event, bool& running) {
    if (event.type == Expose && event.xexpose.count == 0) {
        terminalView->draw(tabs, activeTab);
    } else if (event.type == MapNotify) {
        XSetInputFocus(display, window, RevertToParent, CurrentTime);
    } else if (event.type == ConfigureNotify) {
        terminalView->resize(event.xconfigure.width, event.xconfigure.height);
        terminalView->keepInputCursorVisible(tabs[activeTab]);
        terminalView->draw(tabs, activeTab);
    } else if (event.type == ButtonPress || event.type == ButtonRelease ||
               event.type == MotionNotify) {
        handleMouseEvent(event, running);
    } else if (event.type == KeyPress) {
        handleKey(event.xkey, running);
    } else if (event.type == SelectionRequest || event.type == SelectionNotify) {
        handlePaste(event);
    } else if (event.type == ClientMessage &&
               static_cast<Atom>(event.xclient.data.l[0]) == closeMessage) {
        running = false;
    }
}

vector<pollfd> watchedFds(int xConnection) {
    vector<pollfd> watched{{xConnection, POLLIN, 0}};
    const auto outputFds = processMode
        ? processTabs->outputFds() : commandRunner->outputFds();
    for (const int fd : outputFds)
        watched.push_back({fd, static_cast<short>(POLLIN | POLLHUP), 0});
    return watched;
}

void updateTabs(bool& running) {
    if (processMode) processTabs->update(tabs, *terminalView);
    else commandRunner->updateJobs();
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (!tabs[i].closed && tabs[i].exitRequested) closeTab(i, running);
    }
}

void createXWindow() {
    const int screen = DefaultScreen(display);
    window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0,
                                 kInitialWidth, kInitialHeight, 0,
                                 BlackPixel(display, screen), 0x202124);
    XStoreName(display, window, "Terminator");
    XSelectInput(display, window, ExposureMask | KeyPressMask | ButtonPressMask |
                                  ButtonReleaseMask | PointerMotionMask |
                                  StructureNotifyMask);
    XWMHints* hints = XAllocWMHints();
    if (hints != nullptr) {
        hints->flags = InputHint;
        hints->input = True;
        XSetWMHints(display, window, hints);
        XFree(hints);
    }
    closeMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &closeMessage, 1);
}

void createTerminalView() {
    gc = XCreateGC(display, window, 0, nullptr);
    fontInfo = XQueryFont(display, XGContextFromGC(gc));
    terminalView = make_unique<TerminalView>(
        display, window, gc, fontInfo, kInitialWidth, kInitialHeight);
    for (const auto& warning : hotkeyWarnings)
        terminalView->appendOutput(tabs.front(), "hotkeys.conf: " + warning + "\n");
}

bool initializeWindow() {
    display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        cerr << "terminator: cannot open X display; start XQuartz and set DISPLAY\n";
        return false;
    }
    hotkeys = loadHotkeys(hotkeysPath, &hotkeyWarnings);
    createXWindow();
    createTerminalView();
    return true;
}

bool initializeWorkers() {
    if (processMode) {
        processTabs = make_unique<ProcessTabManager>();
        xConnectionFd = ConnectionNumber(display);
        if (!processTabs->start(0, tabs[0], xConnectionFd)) {
            cerr << "terminator: could not start tab process\n";
            return false;
        }
    } else {
        commandRunner = make_unique<CommandRunner>(tabs, *terminalView);
        commandRunner->ensureWorkers(tabs.size());
    }
    clipboard = make_unique<Clipboard>(display, window);
    XMapWindow(display, window);
    return true;
}

bool initializeApp() {
    return initializeWindow() && initializeWorkers();
}

} 

int runTerminalApp(bool useProcessMode) {
    processMode = useProcessMode;
    history_store::initialize();
    if (!initializeApp()) return 1;

    bool running = true;
    const int xConnection = ConnectionNumber(display);
    while (running) {
        vector<pollfd> watched = watchedFds(xConnection);
        poll(watched.data(), watched.size(), 60);
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            handleEvent(event, running);
        }
        updateTabs(running);
        terminalView->draw(tabs, activeTab);
    }

    if (processMode) processTabs->shutdown();
    else commandRunner->shutdown();
    processTabs.reset();
    commandRunner.reset();
    if (fontInfo != nullptr) XFreeFontInfo(nullptr, fontInfo, 1);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
