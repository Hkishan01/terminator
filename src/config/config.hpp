#pragma once

#include <X11/Xlib.h>

#include <string>
#include <vector>

struct Hotkey {
    std::string action;
    KeySym symbol;
    unsigned int modifiers;
};

std::vector<Hotkey> loadHotkeys(std::string& loadedFrom,
                                std::vector<std::string>* warnings = nullptr);
bool matchesHotkey(const Hotkey& hotkey, const XKeyEvent& event);
