#pragma once

#include <X11/Xlib.h>

#include <string>

class Clipboard {
public:
    Clipboard(Display* display, Window window);

    void copy(const std::string& text);
    void requestPaste(bool primary = false);
    bool handleEvent(const XEvent& event, std::string& pastedText);

private:
    void requestTarget(Atom selection, Atom target);
    void sendSelection(const XSelectionRequestEvent& request);
    bool handleSelectionNotify(const XSelectionEvent& selection,
                               std::string& pastedText);

    Display* display_;
    Window window_;
    Atom clipboard_;
    Atom targets_;
    Atom utf8String_;
    Atom pasteProperty_;
    std::string copiedText_;
    Atom requestedSelection_;
    Atom requestedTarget_;
    bool waitingForPaste_ = false;
};
