#include "terminal/terminal_view.hpp"

#include <algorithm>
#include <cstdlib>
#include <unistd.h>
#include <utility>

using namespace std;

namespace {
constexpr int kTabHeight = 36;
constexpr int kTabWidth = 120;
constexpr int kLineHeight = 18;
constexpr int kOutputLeft = 16;
constexpr int kOutputTopOffset = 23;
constexpr size_t kMaxOutputLines = 3000;
}

Tab::Tab(string tabTitle)
    : title(std::move(tabTitle)),
      output{"Terminator: enter a command and press Return."} {
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) workingDirectory = cwd;
}

TerminalView::TerminalView(Display* display, Window window, GC gc,
                           XFontStruct* font, int width, int height)
    : display_(display), window_(window), gc_(gc), font_(font),
      width_(width), height_(height) {}

void TerminalView::resize(int width, int height) {
    width_ = width;
    height_ = height;
}

int TerminalView::width() const { return width_; }
int TerminalView::height() const { return height_; }

void TerminalView::drawText(int x, int y, const string& text) const {
    XDrawString(display_, window_, gc_, x, y, text.c_str(),
                static_cast<int>(text.size()));
}

int TerminalView::textWidth(const string& text) const {
    if (font_ == nullptr) return static_cast<int>(text.size()) * 8;
    return XTextWidth(font_, text.c_str(), static_cast<int>(text.size()));
}

vector<string> TerminalView::inputLines(const string& input) const {
    vector<string> lines;
    size_t start = 0;
    for (;;) {
        const size_t newline = input.find('\n', start);
        lines.push_back(input.substr(start, newline == string::npos
                                                ? string::npos
                                                : newline - start));
        if (newline == string::npos) break;
        start = newline + 1;
    }
    return lines;
}

string TerminalView::primaryPrompt() const {
    const char* username = getenv("USER");
    return string(username != nullptr && *username != '\0' ? username : "user") +
           "@terminator>: ";
}

void TerminalView::draw(const vector<Tab>& tabs, size_t activeTab) const {
    XSetForeground(display_, gc_, 0x202124);
    XFillRectangle(display_, window_, gc_, 0, 0,
                   static_cast<unsigned int>(width_),
                   static_cast<unsigned int>(height_));
    drawTabs(tabs, activeTab);
    if (activeTab >= tabs.size()) return;
    drawOutput(tabs[activeTab]);
    drawInput(tabs[activeTab]);
}

void TerminalView::drawTabs(const vector<Tab>& tabs, size_t activeTab) const {
    constexpr unsigned long background = 0x202124;
    constexpr unsigned long inactiveTabColor = 0x3c4043;
    constexpr unsigned long activeTabColor = 0x8ab4f8;
    constexpr unsigned long foreground = 0xffffff;

    size_t visibleIndex = 0;
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].closed) continue;
        const int x = static_cast<int>(visibleIndex++) * kTabWidth;
        const bool selected = i == activeTab;
        XSetForeground(display_, gc_, selected ? activeTabColor : inactiveTabColor);
        XFillRectangle(display_, window_, gc_, x, 0, kTabWidth - 2, kTabHeight);
        XSetForeground(display_, gc_, selected ? background : foreground);
        drawText(x + 10, 23, tabs[i].title);
        drawText(x + kTabWidth - 22, 23, "x");
    }
    const int addX = static_cast<int>(visibleIndex) * kTabWidth;
    XSetForeground(display_, gc_, inactiveTabColor);
    XFillRectangle(display_, window_, gc_, addX, 0, 34, kTabHeight);
    XSetForeground(display_, gc_, foreground);
    drawText(addX + 12, 23, "+");
}

void TerminalView::drawOutput(const Tab& tab) const {
    const size_t inputLineCount = inputLines(tab.input).size();
    const int outputTop = kTabHeight + kOutputTopOffset;
    const int promptTop = height_ - 8 - kLineHeight * static_cast<int>(inputLineCount);
    const int availableLines = (promptTop - outputTop) / kLineHeight;
    const int visibleLines = max(0, availableLines);
    const int lastLine = max(0, static_cast<int>(tab.output.size()) - tab.scrollBack);
    const int firstLine = max(0, lastLine - visibleLines);

    XSetForeground(display_, gc_, 0xffffff);
    for (int i = firstLine; i < lastLine; ++i) {
        const string& line = tab.output[static_cast<size_t>(i)];
        const int baseline = outputTop + (i - firstLine) * kLineHeight;
        drawText(kOutputLeft - tab.horizontalOffset, baseline, line);
        drawSelection(tab, static_cast<size_t>(i), line, baseline);
    }
}

void TerminalView::drawInput(const Tab& tab) const {
    const vector<string> lines = inputLines(tab.input);
    const int promptTop = height_ - 8 - kLineHeight * static_cast<int>(lines.size());
    XSetForeground(display_, gc_, 0x8ab4f8);

    size_t inputOffset = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const string prompt = i == 0 ? primaryPrompt() : "> ";
        const int baseline = promptTop + static_cast<int>(i) * kLineHeight;
        const string displayedInput = prompt + lines[i];
        drawText(kOutputLeft - tab.horizontalOffset, baseline, displayedInput);
        if (tab.inputCursor >= inputOffset &&
            tab.inputCursor <= inputOffset + lines[i].size()) {
            const size_t column = min(tab.inputCursor - inputOffset, lines[i].size());
            const int x = kOutputLeft - tab.horizontalOffset +
                textWidth(prompt + lines[i].substr(0, column));
            XDrawLine(display_, window_, gc_, x, baseline - 13, x, baseline + 2);
        }
        inputOffset += lines[i].size() + (i + 1 < lines.size() ? 1 : 0);
    }
}

void TerminalView::appendOutput(Tab& tab, const string& text,
                                const string& label) const {
    bool atLineStart = tab.output.empty() || tab.output.back().empty();
    for (char ch : text) {
        if (atLineStart && !label.empty()) {
            if (tab.output.empty()) tab.output.emplace_back();
            tab.output.back() += label;
            atLineStart = false;
        }
        if (ch == '\n') {
            tab.output.emplace_back();
            atLineStart = true;
        } else if (ch != '\r') {
            if (tab.output.empty()) tab.output.emplace_back();
            tab.output.back().push_back(ch);
            atLineStart = false;
        }
    }
    if (tab.output.size() > kMaxOutputLines) {
        const auto removeCount = tab.output.size() - kMaxOutputLines;
        tab.output.erase(tab.output.begin(), tab.output.begin() +
                         static_cast<ptrdiff_t>(removeCount));
    }
}

void TerminalView::scrollVertical(Tab& tab, int rows) const {
    const int outputTop = kTabHeight + kOutputTopOffset;
    const int promptTop = height_ - 8 -
        kLineHeight * static_cast<int>(inputLines(tab.input).size());
    const int visible = max(0, (promptTop - outputTop) / kLineHeight);
    const int maximum = max(0, static_cast<int>(tab.output.size()) - visible);
    tab.scrollBack = clamp(tab.scrollBack + rows, 0, maximum);
}

void TerminalView::scrollHorizontal(Tab& tab, int pixels) const {
    int widest = width_ - 32;
    for (const string& line : tab.output)
        widest = max(widest, textWidth(line));
    for (const string& line : inputLines(tab.input))
        widest = max(widest, textWidth(line) + 24);
    const int maximum = max(0, widest - (width_ - 32));
    tab.horizontalOffset = clamp(tab.horizontalOffset + pixels, 0, maximum);
}

void TerminalView::keepInputCursorVisible(Tab& tab) const {
    const vector<string> lines = inputLines(tab.input);
    size_t offset = 0;
    size_t row = 0;
    size_t column = 0;
    for (; row + 1 < lines.size(); ++row) {
        const size_t next = offset + lines[row].size() + 1;
        if (tab.inputCursor < next) break;
        offset = next;
    }
    column = min(tab.inputCursor - min(offset, tab.inputCursor), lines[row].size());
    const string prompt = row == 0 ? primaryPrompt() : "> ";
    const int cursorX = kOutputLeft + textWidth(prompt + lines[row].substr(0, column));
    const int rightEdge = width_ - 24;
    if (cursorX - tab.horizontalOffset > rightEdge)
        tab.horizontalOffset = cursorX - rightEdge;
    else if (cursorX - tab.horizontalOffset < kOutputLeft)
        tab.horizontalOffset = max(0, cursorX - kOutputLeft);
    scrollHorizontal(tab, 0);
}

void TerminalView::drawSelection(const Tab& tab, size_t lineIndex,
                                 const string& line, int baseline) const {
    if (!tab.hasSelection) return;

    size_t startLine = tab.selectionStartLine;
    size_t startColumn = tab.selectionStartColumn;
    size_t endLine = tab.selectionEndLine;
    size_t endColumn = tab.selectionEndColumn;
    if (endLine < startLine || (endLine == startLine && endColumn < startColumn)) {
        swap(startLine, endLine);
        swap(startColumn, endColumn);
    }
    if (lineIndex < startLine || lineIndex > endLine) return;

    const size_t begin = lineIndex == startLine ? startColumn : 0;
    const size_t end = lineIndex == endLine ? endColumn : line.size();
    const size_t from = min(begin, line.size());
    const size_t to = min(max(from, end), line.size());
    if (to <= from) return;

    const string prefix = line.substr(0, from);
    const string selected = line.substr(from, to - from);
    const int x = kOutputLeft - tab.horizontalOffset + textWidth(prefix);
    const int selectedWidth = max(1, textWidth(selected));
    const int ascent = font_ == nullptr ? 13 : font_->ascent;
    const int height = font_ == nullptr ? 17 : font_->ascent + font_->descent;
    XSetForeground(display_, gc_, 0x5f4b8b);
    XFillRectangle(display_, window_, gc_, x, baseline - ascent,
                   static_cast<unsigned int>(selectedWidth),
                   static_cast<unsigned int>(height));
    XSetForeground(display_, gc_, 0xffffff);
    drawText(x, baseline, selected);
}

bool TerminalView::mousePosition(const Tab& tab, int x, int y,
                                 size_t& line, size_t& column) const {
    const int outputTop = kTabHeight + kOutputTopOffset;
    const int promptTop = height_ - 8 -
        kLineHeight * static_cast<int>(inputLines(tab.input).size());
    const int visible = max(0, (promptTop - outputTop) / kLineHeight);
    const int last = max(0, static_cast<int>(tab.output.size()) - tab.scrollBack);
    const int first = max(0, last - visible);
    const int top = outputTop - (font_ == nullptr ? 13 : font_->ascent);
    const int row = (y - top) / kLineHeight;
    if (row < 0 || row >= last - first) return false;

    line = static_cast<size_t>(first + row);
    const string& text = tab.output[line];
    const int localX = max(0, x - kOutputLeft + tab.horizontalOffset);
    column = text.size();
    for (size_t i = 0; i < text.size(); ++i) {
        const int left = textWidth(text.substr(0, i));
        const int right = textWidth(text.substr(0, i + 1));
        if (localX < left + (right - left) / 2) {
            column = i;
            break;
        }
    }
    return true;
}

bool TerminalView::beginSelection(Tab& tab, int x, int y) {
    size_t line = 0;
    size_t column = 0;
    if (!mousePosition(tab, x, y, line, column)) {
        selecting_ = false;
        tab.hasSelection = false;
        return false;
    }
    tab.hasSelection = true;
    tab.selectionStartLine = line;
    tab.selectionStartColumn = column;
    tab.selectionEndLine = line;
    tab.selectionEndColumn = column;
    selecting_ = true;
    return true;
}

void TerminalView::updateSelection(Tab& tab, int x, int y) {
    if (!selecting_) return;
    size_t line = 0;
    size_t column = 0;
    if (!mousePosition(tab, x, y, line, column)) {
        const int outputTop = kTabHeight + kOutputTopOffset;
        if (tab.output.empty()) return;
        const int promptTop = height_ - 8 -
            kLineHeight * static_cast<int>(inputLines(tab.input).size());
        const int visible = max(0, (promptTop - outputTop) / kLineHeight);
        const int last = max(0, static_cast<int>(tab.output.size()) - tab.scrollBack);
        const int first = max(0, last - visible);
        const bool above = y < outputTop - (font_ == nullptr ? 13 : font_->ascent);
        line = static_cast<size_t>(above ? first : max(first, last - 1));
        line = min(line, tab.output.size() - 1);
        column = above ? 0 : tab.output[line].size();
    }
    tab.selectionEndLine = line;
    tab.selectionEndColumn = column;
}

void TerminalView::endSelection() { selecting_ = false; }

string TerminalView::selectedText(const Tab& tab) const {
    if (!tab.hasSelection || tab.output.empty()) return {};
    size_t startLine = tab.selectionStartLine;
    size_t startColumn = tab.selectionStartColumn;
    size_t endLine = tab.selectionEndLine;
    size_t endColumn = tab.selectionEndColumn;
    if (endLine < startLine || (endLine == startLine && endColumn < startColumn)) {
        swap(startLine, endLine);
        swap(startColumn, endColumn);
    }
    endLine = min(endLine, tab.output.size() - 1);
    startLine = min(startLine, endLine);

    string result;
    for (size_t line = startLine; line <= endLine; ++line) {
        const string& text = tab.output[line];
        const size_t from = line == startLine
            ? min(startColumn, text.size()) : 0;
        const size_t to = line == endLine
            ? min(endColumn, text.size()) : text.size();
        result.append(text, from, to > from ? to - from : 0);
        if (line < endLine) result.push_back('\n');
    }
    return result;
}
