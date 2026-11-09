// Heap growth tracker (diagnostic build).
//
// Every allocation call goes through these wrappers (linked with
// -Wl,--wrap). Each call checks whether the heap's end moved. When it did,
// the call stack is recorded, so the perf log can name exactly which code
// keeps making the heap bigger.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <new>

#include <switch.h>
#include <unwind.h>

#include "platform/switch/switch_platform.h"

extern "C" {
void* __real_malloc(size_t size);
void* __real_calloc(size_t count, size_t size);
void* __real_realloc(void* ptr, size_t size);
void* __real_memalign(size_t alignment, size_t size);
void* __real_aligned_alloc(size_t alignment, size_t size);
int __real_posix_memalign(void** out, size_t alignment, size_t size);

void* __wrap_malloc(size_t size);
void* __wrap_calloc(size_t count, size_t size);
void* __wrap_realloc(void* ptr, size_t size);
void* __wrap_memalign(size_t alignment, size_t size);
void* __wrap_aligned_alloc(size_t alignment, size_t size);
int __wrap_posix_memalign(void** out, size_t alignment, size_t size);
void* __wrap__Znwm(size_t size);
void* __wrap__Znam(size_t size);
}

namespace {

constexpr int kFrames = 8;
constexpr int kEntries = 128;
constexpr int kBigSizes = 16;
constexpr size_t kBigRequest = 256 * 1024;

struct GrowthEntry {
    uint64_t key;
    uintptr_t frames[kFrames];
    int frameCount;
    const char* kind;
    size_t lastSize;
    size_t lastAlignment;
    uint64_t windowCalls;
    uint64_t windowGrowth;
    uint64_t totalCalls;
    uint64_t totalGrowth;
};

struct BigSize {
    size_t size;
    uint64_t windowCount;
};

RMutex gLock;
int gDepth = 0;

GrowthEntry gEntries[kEntries];
int gEntryCount = 0;
uint64_t gWindowGrowth = 0;
uint64_t gTotalGrowth = 0;
uint64_t gDroppedGrowth = 0;

BigSize gBigSizes[kBigSizes];
int gBigSizeCount = 0;
uint64_t gBigOther = 0;

// Copies for printing, so the file write happens without holding the lock.
GrowthEntry gSnapshot[kEntries];
BigSize gBigSnapshot[kBigSizes];

struct UnwindState {
    uintptr_t frames[kFrames + 16];
    int count;
};

_Unwind_Reason_Code unwindCallback(struct _Unwind_Context* context, void* argument)
{
    UnwindState* state = static_cast<UnwindState*>(argument);
    if (state->count >= kFrames + 16) {
        return _URC_END_OF_STACK;
    }

    uintptr_t ip = static_cast<uintptr_t>(_Unwind_GetIP(context));
    if (ip == 0) {
        return _URC_END_OF_STACK;
    }

    state->frames[state->count++] = ip;
    return _URC_NO_REASON;
}

uintptr_t heapEnd()
{
    return reinterpret_cast<uintptr_t>(sbrk(0));
}

// Returns true when this is the outermost allocation call on this thread,
// meaning it should be measured. Inner calls (an allocator calling another
// allocator) just pass through.
bool enter(uintptr_t* before)
{
    rmutexLock(&gLock);
    gDepth++;
    if (gDepth > 1) {
        return false;
    }

    *before = heapEnd();
    return true;
}

void recordBig(size_t size)
{
    for (int index = 0; index < gBigSizeCount; index++) {
        if (gBigSizes[index].size == size) {
            gBigSizes[index].windowCount++;
            return;
        }
    }

    if (gBigSizeCount < kBigSizes) {
        gBigSizes[gBigSizeCount].size = size;
        gBigSizes[gBigSizeCount].windowCount = 1;
        gBigSizeCount++;
        return;
    }

    gBigOther++;
}

void recordGrowth(uint64_t growth, const char* kind, size_t size, size_t alignment, uintptr_t caller)
{
    gWindowGrowth += growth;
    gTotalGrowth += growth;

    UnwindState state;
    state.count = 0;
    _Unwind_Backtrace(unwindCallback, &state);

    // Start the stack at the code that called the allocator, skipping the
    // tracker's own frames.
    uintptr_t frames[kFrames];
    int frameCount = 0;
    int start = -1;
    for (int index = 0; index < state.count; index++) {
        if (state.frames[index] == caller) {
            start = index;
            break;
        }
    }

    if (start == -1) {
        frames[0] = caller;
        frameCount = 1;
    } else {
        for (int index = start; index < state.count && frameCount < kFrames; index++) {
            frames[frameCount++] = state.frames[index];
        }
    }

    uint64_t key = 1469598103934665603ULL;
    for (int index = 0; index < frameCount; index++) {
        key ^= frames[index];
        key *= 1099511628211ULL;
    }

    for (int index = 0; index < gEntryCount; index++) {
        GrowthEntry* entry = &gEntries[index];
        if (entry->key == key) {
            entry->lastSize = size;
            entry->lastAlignment = alignment;
            entry->windowCalls++;
            entry->windowGrowth += growth;
            entry->totalCalls++;
            entry->totalGrowth += growth;
            return;
        }
    }

    if (gEntryCount >= kEntries) {
        gDroppedGrowth += growth;
        return;
    }

    GrowthEntry* entry = &gEntries[gEntryCount++];
    entry->key = key;
    memcpy(entry->frames, frames, sizeof(uintptr_t) * frameCount);
    entry->frameCount = frameCount;
    entry->kind = kind;
    entry->lastSize = size;
    entry->lastAlignment = alignment;
    entry->windowCalls = 1;
    entry->windowGrowth = growth;
    entry->totalCalls = 1;
    entry->totalGrowth = growth;
}

void leave(bool measured, uintptr_t before, const char* kind, size_t size, size_t alignment, uintptr_t caller)
{
    if (measured) {
        if (size >= kBigRequest) {
            recordBig(size);
        }

        uintptr_t after = heapEnd();
        if (after > before) {
            recordGrowth(after - before, kind, size, alignment, caller);
        }
    }

    gDepth--;
    rmutexUnlock(&gLock);
}

#define CALLER reinterpret_cast<uintptr_t>(__builtin_return_address(0))

void* trackedMalloc(size_t size, const char* kind, uintptr_t caller)
{
    uintptr_t before = 0;
    bool measured = enter(&before);
    void* result = __real_malloc(size);
    leave(measured, before, kind, size, 0, caller);
    return result;
}

void writeTop(FILE* stream, const char* label, int count, bool window, uintptr_t base)
{
    // Pick the biggest entries without sorting the whole table.
    bool used[kEntries] = {};
    for (int rank = 0; rank < 5; rank++) {
        int best = -1;
        uint64_t bestGrowth = 0;
        for (int index = 0; index < count; index++) {
            uint64_t growth = window ? gSnapshot[index].windowGrowth : gSnapshot[index].totalGrowth;
            if (!used[index] && growth > bestGrowth) {
                best = index;
                bestGrowth = growth;
            }
        }

        if (best == -1) {
            return;
        }

        used[best] = true;
        GrowthEntry* entry = &gSnapshot[best];
        fprintf(stream,
            "  %s %d: grew=%.1fMB calls=%llu kind=%s size=%zu align=%zu stack=",
            label,
            rank + 1,
            bestGrowth / (1024.0 * 1024.0),
            static_cast<unsigned long long>(window ? entry->windowCalls : entry->totalCalls),
            entry->kind,
            entry->lastSize,
            entry->lastAlignment);
        for (int frame = 0; frame < entry->frameCount; frame++) {
            fprintf(stream, "%s0x%llx", frame == 0 ? "" : ",", static_cast<unsigned long long>(entry->frames[frame] - base));
        }
        fprintf(stream, "\n");
    }
}

} // namespace

extern "C" {

void* __wrap_malloc(size_t size)
{
    return trackedMalloc(size, "malloc", CALLER);
}

void* __wrap_calloc(size_t count, size_t size)
{
    uintptr_t before = 0;
    bool measured = enter(&before);
    void* result = __real_calloc(count, size);
    leave(measured, before, "calloc", count * size, 0, CALLER);
    return result;
}

void* __wrap_realloc(void* ptr, size_t size)
{
    uintptr_t before = 0;
    bool measured = enter(&before);
    void* result = __real_realloc(ptr, size);
    leave(measured, before, "realloc", size, 0, CALLER);
    return result;
}

void* __wrap_memalign(size_t alignment, size_t size)
{
    uintptr_t before = 0;
    bool measured = enter(&before);
    void* result = __real_memalign(alignment, size);
    leave(measured, before, "memalign", size, alignment, CALLER);
    return result;
}

void* __wrap_aligned_alloc(size_t alignment, size_t size)
{
    uintptr_t before = 0;
    bool measured = enter(&before);
    void* result = __real_aligned_alloc(alignment, size);
    leave(measured, before, "aligned_alloc", size, alignment, CALLER);
    return result;
}

int __wrap_posix_memalign(void** out, size_t alignment, size_t size)
{
    uintptr_t before = 0;
    bool measured = enter(&before);
    int result = __real_posix_memalign(out, alignment, size);
    leave(measured, before, "posix_memalign", size, alignment, CALLER);
    return result;
}

// C++ new goes straight to malloc here, so the recorded caller is the game
// code that used new, not the C++ library.
void* __wrap__Znwm(size_t size)
{
    void* result = trackedMalloc(size != 0 ? size : 1, "new", CALLER);
    if (result == nullptr) {
        throw std::bad_alloc();
    }
    return result;
}

void* __wrap__Znam(size_t size)
{
    void* result = trackedMalloc(size != 0 ? size : 1, "new[]", CALLER);
    if (result == nullptr) {
        throw std::bad_alloc();
    }
    return result;
}

} // extern "C"

namespace fallout {

void switchAllocTrackWrite(FILE* stream)
{
    rmutexLock(&gLock);
    gDepth++;

    int count = gEntryCount;
    memcpy(gSnapshot, gEntries, sizeof(GrowthEntry) * count);
    int bigCount = gBigSizeCount;
    memcpy(gBigSnapshot, gBigSizes, sizeof(BigSize) * bigCount);
    uint64_t windowGrowth = gWindowGrowth;
    uint64_t totalGrowth = gTotalGrowth;
    uint64_t droppedGrowth = gDroppedGrowth;
    uint64_t bigOther = gBigOther;

    for (int index = 0; index < gEntryCount; index++) {
        gEntries[index].windowCalls = 0;
        gEntries[index].windowGrowth = 0;
    }
    gBigSizeCount = 0;
    gBigOther = 0;
    gWindowGrowth = 0;

    gDepth--;
    rmutexUnlock(&gLock);

    // Stack addresses are printed relative to this function, so they can be
    // matched against the ELF from the same build.
    uintptr_t base = reinterpret_cast<uintptr_t>(&switchAllocTrackWrite);

    fprintf(stream,
        "  heap_growth: window=%.1fMB total=%.1fMB untracked=%.1fMB stacks=%d big_requests:",
        windowGrowth / (1024.0 * 1024.0),
        totalGrowth / (1024.0 * 1024.0),
        droppedGrowth / (1024.0 * 1024.0),
        count);
    for (int index = 0; index < bigCount; index++) {
        fprintf(stream, " %zux%llu", gBigSnapshot[index].size, static_cast<unsigned long long>(gBigSnapshot[index].windowCount));
    }
    if (bigOther > 0) {
        fprintf(stream, " other_sizes=%llu", static_cast<unsigned long long>(bigOther));
    }
    fprintf(stream, "\n");

    if (windowGrowth > 0) {
        writeTop(stream, "grew_now", count, true, base);
    }
    writeTop(stream, "grew_total", count, false, base);
}

} // namespace fallout
