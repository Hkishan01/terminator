#include "history/history_store.hpp"

#include <algorithm>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>

using namespace std;

namespace history_store {
namespace {
constexpr size_t kCapacity = 10000;
constexpr size_t kEntrySize = 2048;
struct SharedHistory {
    volatile int lock;
    size_t count;
    size_t first;
    char entries[kCapacity][kEntrySize];
};
SharedHistory* shared = nullptr;
}

void initialize() {
    if (shared) return;
    void* memory = mmap(nullptr, sizeof(SharedHistory), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return;
    shared = static_cast<SharedHistory*>(memory);
    memset(shared, 0, sizeof(SharedHistory));
}

void shutdown() {
    if (!shared) return;
    munmap(shared, sizeof(SharedHistory));
    shared = nullptr;
}

namespace {
void lockShared() {
    while (__sync_lock_test_and_set(&shared->lock, 1)) sched_yield();
}
void unlockShared() { __sync_lock_release(&shared->lock); }
}

void add(const string& command) {
    initialize();
    if (!shared) return;
    lockShared();
    size_t index;
    if (shared->count < kCapacity) {
        index = (shared->first + shared->count++) % kCapacity;
    } else {
        index = shared->first;
        shared->first = (shared->first + 1) % kCapacity;
    }
    const size_t size = min(command.size(), kEntrySize - 1);
    memcpy(shared->entries[index], command.data(), size);
    shared->entries[index][size] = '\0';
    unlockShared();
}

vector<string> snapshot() {
    initialize();
    vector<string> result;
    if (!shared) return result;
    lockShared();
    result.reserve(shared->count);
    for (size_t i = 0; i < shared->count; ++i) {
        const size_t index = (shared->first + i) % kCapacity;
        result.emplace_back(shared->entries[index]);
    }
    unlockShared();
    return result;
}
} 
