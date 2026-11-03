#include "terminal/clipboard.hpp"

#include <X11/Xatom.h>

#include <algorithm>

using namespace std;

Clipboard::Clipboard(Display* display, Window window)
    : display_(display), window_(window),
      clipboard_(XInternAtom(display, "CLIPBOARD", False)),
      targets_(XInternAtom(display, "TARGETS", False)),
      utf8String_(XInternAtom(display, "UTF8_STRING", False)),
      pasteProperty_(XInternAtom(display, "TERMINATOR_CLIPBOARD", False)),
      requestedSelection_(None), requestedTarget_(None) {}

void Clipboard::copy(const string& text) {
    copiedText_ = text;
    XSetSelectionOwner(display_, clipboard_, window_, CurrentTime);
    XSetSelectionOwner(display_, XA_PRIMARY, window_, CurrentTime);
    XFlush(display_);
}

void Clipboard::requestTarget(Atom selection, Atom target) {
    requestedSelection_ = selection;
    requestedTarget_ = target;
    waitingForPaste_ = true;
    XConvertSelection(display_, selection, target, pasteProperty_, window_, CurrentTime);
    XFlush(display_);
}

void Clipboard::requestPaste(bool primary) {
    requestTarget(primary ? XA_PRIMARY : clipboard_, utf8String_);
}

void Clipboard::sendSelection(const XSelectionRequestEvent& request) {
    XSelectionEvent response{};
    response.type = SelectionNotify;
    response.display = request.display;
    response.requestor = request.requestor;
    response.selection = request.selection;
    response.target = request.target;
    response.time = request.time;
    response.property = request.property == None ? request.target : request.property;

    if (request.target == targets_) {
        const Atom supported[] = {targets_, utf8String_, XA_STRING};
        XChangeProperty(display_, request.requestor, response.property, XA_ATOM, 32,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char*>(supported), 3);
    } else if (request.target == utf8String_ || request.target == XA_STRING) {
        XChangeProperty(display_, request.requestor, response.property,
                        request.target, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(copiedText_.data()),
                        static_cast<int>(copiedText_.size()));
    } else {
        response.property = None;
    }

    XSendEvent(display_, request.requestor, False, 0,
               reinterpret_cast<XEvent*>(&response));
    XFlush(display_);
}

bool Clipboard::handleEvent(const XEvent& event, string& pastedText) {
    if (event.type == SelectionRequest) {
        sendSelection(event.xselectionrequest);
        return true;
    }
    if (event.type != SelectionNotify || !waitingForPaste_) return false;
    return handleSelectionNotify(event.xselection, pastedText);
}

bool Clipboard::handleSelectionNotify(const XSelectionEvent& selection,
                                      string& pastedText) {
    if (selection.property == None) {
        if (requestedTarget_ == utf8String_) {
            requestTarget(requestedSelection_, XA_STRING);
        } else {
            waitingForPaste_ = false;
        }
        return true;
    }

    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesRemaining = 0;
    unsigned char* value = nullptr;
    const int status = XGetWindowProperty(display_, window_, pasteProperty_, 0,
        262144, True, AnyPropertyType, &actualType, &actualFormat,
        &itemCount, &bytesRemaining, &value);

    if (status == Success && value != nullptr && actualFormat == 8) {
        pastedText.assign(reinterpret_cast<const char*>(value), itemCount);
        pastedText.erase(remove(pastedText.begin(), pastedText.end(), '\0'),
                         pastedText.end());
    }
    if (value != nullptr) XFree(value);
    waitingForPaste_ = false;
    return true;
}
