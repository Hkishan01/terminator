#pragma once

#include <X11/Xlib.h>

#include <string>
#include <vector>

struct Tab {
    explicit Tab(std::string tabTitle = "Tab 1");

    std::string title;
    std::vector<std::string> output;
    std::string input;
    std::size_t inputCursor = 0;
    std::size_t historyCursor = 0;
    std::string historyDraft;
    int lastExitStatus = 0;
    bool exitRequested = false;
    bool closed = false;
    std::string workingDirectory;
    int scrollBack = 0;
    int horizontalOffset = 0;
    bool hasSelection = false;
    std::size_t selectionStartLine = 0;
    std::size_t selectionStartColumn = 0;
    std::size_t selectionEndLine = 0;
    std::size_t selectionEndColumn = 0;
};

class TerminalView {
public:
    TerminalView(Display* display, Window window, GC gc, XFontStruct* font,
                 int width, int height);

    void resize(int width, int height);
    int width() const;
    int height() const;
    void draw(const std::vector<Tab>& tabs, std::size_t activeTab) const;
    void appendOutput(Tab& tab, const std::string& text,
                      const std::string& label = {}) const;
    void scrollVertical(Tab& tab, int rows) const;
    void scrollHorizontal(Tab& tab, int pixels) const;
    void keepInputCursorVisible(Tab& tab) const;
    bool beginSelection(Tab& tab, int x, int y);
    void updateSelection(Tab& tab, int x, int y);
    void endSelection();
    std::string selectedText(const Tab& tab) const;

private:
    int textWidth(const std::string& text) const;
    std::vector<std::string> inputLines(const std::string& input) const;
    void drawTabs(const std::vector<Tab>& tabs, std::size_t activeTab) const;
    void drawOutput(const Tab& tab) const;
    void drawInput(const Tab& tab) const;
    bool mousePosition(const Tab& tab, int x, int y,
                       std::size_t& line, std::size_t& column) const;
    void drawSelection(const Tab& tab, std::size_t lineIndex,
                       const std::string& line, int baseline) const;
    std::string primaryPrompt() const;
    void drawText(int x, int y, const std::string& text) const;

    Display* display_;
    Window window_;
    GC gc_;
    XFontStruct* font_;
    int width_;
    int height_;
    bool selecting_ = false;
};
