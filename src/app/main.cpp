#include "app/app.hpp"
#include "history/history_store.hpp"

#include <string>

using namespace std;

int main(int argc, char** argv) {
    bool useProcessMode = false;
    if (argc > 1 && string(argv[1]) == "useProcess=True") useProcessMode = true;
    const int result = runTerminalApp(useProcessMode);
    history_store::shutdown();
    return result;
}
