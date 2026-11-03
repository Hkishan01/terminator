#include "config/config.hpp"

#include <X11/keysym.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace std;

namespace {

string trim(string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

string lower(string value) {
    for (char& c : value) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return value;
}

unsigned int modifierMask(const string& name) {
    if (name == "ctrl" || name == "control") return ControlMask;
    if (name == "shift") return ShiftMask;
    if (name == "alt" || name == "meta") return Mod1Mask;
    return 0;
}

bool isModifier(const string& name) {
    return name == "ctrl" || name == "control" || name == "shift" ||
           name == "alt" || name == "meta";
}

KeySym namedKey(const string& name) {
    if (name.size() == 1) return XStringToKeysym(name.c_str());
    if (name == "tab") return XK_Tab;
    if (name == "enter" || name == "return") return XK_Return;
    if (name == "space") return XK_space;
    if (name == "escape" || name == "esc") return XK_Escape;
    if (name == "left") return XK_Left;
    if (name == "right") return XK_Right;
    if (name == "up") return XK_Up;
    if (name == "down") return XK_Down;
    if (name == "backspace") return XK_BackSpace;
    if (name == "delete") return XK_Delete;
    if (name == "page_up") return XK_Page_Up;
    if (name == "page_down") return XK_Page_Down;
    return NoSymbol;
}

vector<Hotkey> defaults() {
    return {
        {"quit", XK_q, ControlMask},
        {"new_tab", XK_T, ControlMask | ShiftMask},
        {"next_tab", XK_Tab, ControlMask},
        {"previous_tab", XK_Tab, ControlMask | ShiftMask},
        {"interrupt", XK_c, ControlMask},
        {"suspend", XK_z, ControlMask},
        {"cursor_left", XK_Left, 0},
        {"cursor_right", XK_Right, 0},
        {"cursor_line_start", XK_a, ControlMask},
        {"cursor_line_end", XK_e, ControlMask},
        {"history_previous", XK_Up, 0},
        {"history_next", XK_Down, 0},
        {"delete_to_start", XK_u, ControlMask},
        {"delete_to_end", XK_k, ControlMask},
        {"delete_previous_word", XK_w, ControlMask},
        {"clear_screen", XK_l, ControlMask},
        {"eof", XK_d, ControlMask},
        {"copy", XK_c, ControlMask | ShiftMask},
        {"paste", XK_v, ControlMask | ShiftMask},
    };
}

} 

vector<Hotkey> loadHotkeys(string& loadedFrom,
                                vector<string>* warnings) {
    const vector<Hotkey> base = defaults();
    vector<Hotkey> hotkeys = base;
    const char* home = getenv("HOME");
    if (home == nullptr) return hotkeys;
    const string path = string(home) + "/.config/terminator/hotkeys.conf";
    ifstream config(path);
    if (!config) return hotkeys;
    loadedFrom = path;

    string line;
    while (getline(config, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t equals = line.find('=');
        if (equals == string::npos) {
            if (warnings) warnings->push_back("invalid binding line: " + line);
            continue;
        }
        const string action = trim(line.substr(0, equals));
        const auto target = find_if(hotkeys.begin(), hotkeys.end(),
            [&](const Hotkey& hotkey) { return hotkey.action == action; });
        if (target == hotkeys.end()) {
            if (warnings) warnings->push_back("unknown hotkey action: " + action);
            continue;
        }

        istringstream parts(line.substr(equals + 1));
        string part;
        unsigned int modifiers = 0;
        KeySym symbol = NoSymbol;
        bool valid = true;
        while (getline(parts, part, '+')) {
            part = lower(trim(part));
            if (part.empty()) { valid = false; break; }
            if (isModifier(part)) {
                const unsigned int bit = modifierMask(part);
                if (modifiers & bit) { valid = false; break; }
                modifiers |= bit;
            } else {
                if (symbol != NoSymbol) { valid = false; break; }
                symbol = namedKey(part);
            }
        }
        if (!valid || symbol == NoSymbol) {
            if (warnings) warnings->push_back("invalid binding for action " + action + "; using default");
            continue;
        }
        target->symbol = symbol;
        target->modifiers = modifiers;
    }

    constexpr unsigned int relevant = ControlMask | ShiftMask | Mod1Mask;
    for (size_t i = 0; i < hotkeys.size(); ++i) {
        for (size_t j = i + 1; j < hotkeys.size(); ++j) {
            if (hotkeys[i].symbol == hotkeys[j].symbol &&
                (hotkeys[i].modifiers & relevant) == (hotkeys[j].modifiers & relevant)) {
                if (warnings) warnings->push_back("conflicting bindings for " +
                    hotkeys[i].action + " and " + hotkeys[j].action + "; using defaults");
                hotkeys[i] = base[i];
                hotkeys[j] = base[j];
            }
        }
    }
    return hotkeys;
}

bool matchesHotkey(const Hotkey& hotkey, const XKeyEvent& event) {
    char text[8];
    KeySym symbol = NoSymbol;
    XLookupString(const_cast<XKeyEvent*>(&event), text, sizeof(text), &symbol, nullptr);
    if (symbol == NoSymbol) symbol = XLookupKeysym(const_cast<XKeyEvent*>(&event), 0);
    const KeySym unshifted = XLookupKeysym(const_cast<XKeyEvent*>(&event), 0);
    constexpr unsigned int relevantModifiers = ControlMask | ShiftMask | Mod1Mask;
    return (symbol == hotkey.symbol || unshifted == hotkey.symbol) &&
           (event.state & relevantModifiers) == hotkey.modifiers;
}
