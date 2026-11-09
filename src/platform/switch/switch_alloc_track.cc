// Reuse of large graphics buffers.
//
// Every frame, the Switch graphics driver (mesa nouveau) uploads the screen
// texture through a brand new staging buffer of about 1.6MB, allocated with
// page alignment and freed a frame or two later. The C library's heap can't
// reuse those aligned holes cleanly, so it grows by hundreds of MB per
// minute until it hits the memory limit (about 95 seconds in Fallout et tu).
// After that the driver quietly fails its uploads and old pictures stay on
// screen.
//
// These wrappers (linked with -Wl,--wrap) keep freed large aligned blocks
// and hand the same memory back on the next request of the same size, so
// the heap stays flat.

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "platform/switch/switch_platform.h"

extern "C" {
void* __real_memalign(size_t alignment, size_t size);
void* __real_aligned_alloc(size_t alignment, size_t size);
int __real_posix_memalign(void** out, size_t alignment, size_t size);
void __real_free(void* ptr);

void* __wrap_memalign(size_t alignment, size_t size);
void* __wrap_aligned_alloc(size_t alignment, size_t size);
int __wrap_posix_memalign(void** out, size_t alignment, size_t size);
void __wrap_free(void* ptr);
}

namespace {

// Only blocks at least this big are handled here; everything else goes
// straight to the C library.
constexpr size_t kMinBlockSize = 512 * 1024;

// Large aligned blocks currently handed out.
constexpr int kLiveSlots = 64;

// Freed blocks kept for reuse.
constexpr int kCacheSlots = 8;
constexpr size_t kCacheMaxBytes = 64 * 1024 * 1024;

struct Block {
    void* ptr;
    size_t size;
    size_t alignment;
};

Mutex gLock;

Block gLive[kLiveSlots];
int gLiveCount = 0;

Block gCache[kCacheSlots];
int gCacheCount = 0;
size_t gCacheBytes = 0;

unsigned long long gWindowRequests = 0;
unsigned long long gWindowReused = 0;
unsigned long long gWindowFresh = 0;
unsigned long long gTotalReused = 0;

void addLive(void* ptr, size_t size, size_t alignment)
{
    if (gLiveCount < kLiveSlots) {
        gLive[gLiveCount].ptr = ptr;
        gLive[gLiveCount].size = size;
        gLive[gLiveCount].alignment = alignment;
        gLiveCount++;
    }
}

void* allocateAligned(size_t alignment, size_t size, void* (*real)(size_t, size_t))
{
    if (size < kMinBlockSize) {
        return real(alignment, size);
    }

    mutexLock(&gLock);
    gWindowRequests++;

    for (int index = 0; index < gCacheCount; index++) {
        if (gCache[index].size == size && gCache[index].alignment == alignment) {
            void* ptr = gCache[index].ptr;
            gCacheBytes -= size;
            gCache[index] = gCache[gCacheCount - 1];
            gCacheCount--;

            addLive(ptr, size, alignment);
            gWindowReused++;
            gTotalReused++;
            mutexUnlock(&gLock);
            return ptr;
        }
    }

    void* ptr = real(alignment, size);
    if (ptr != nullptr) {
        addLive(ptr, size, alignment);
        gWindowFresh++;
    }

    mutexUnlock(&gLock);
    return ptr;
}

} // namespace

extern "C" {

void* __wrap_memalign(size_t alignment, size_t size)
{
    return allocateAligned(alignment, size, __real_memalign);
}

void* __wrap_aligned_alloc(size_t alignment, size_t size)
{
    return allocateAligned(alignment, size, __real_aligned_alloc);
}

int __wrap_posix_memalign(void** out, size_t alignment, size_t size)
{
    if (size < kMinBlockSize) {
        return __real_posix_memalign(out, alignment, size);
    }

    void* ptr = allocateAligned(alignment, size, __real_memalign);
    if (ptr == nullptr) {
        return ENOMEM;
    }

    *out = ptr;
    return 0;
}

void __wrap_free(void* ptr)
{
    if (ptr == nullptr) {
        return;
    }

    mutexLock(&gLock);

    for (int index = 0; index < gLiveCount; index++) {
        if (gLive[index].ptr != ptr) {
            continue;
        }

        Block block = gLive[index];
        gLive[index] = gLive[gLiveCount - 1];
        gLiveCount--;

        if (gCacheCount < kCacheSlots && gCacheBytes + block.size <= kCacheMaxBytes) {
            gCache[gCacheCount++] = block;
            gCacheBytes += block.size;
            mutexUnlock(&gLock);
            return;
        }

        break;
    }

    mutexUnlock(&gLock);
    __real_free(ptr);
}

} // extern "C"

namespace fallout {

void switchAllocTrackWrite(FILE* stream)
{
    mutexLock(&gLock);
    unsigned long long requests = gWindowRequests;
    unsigned long long reused = gWindowReused;
    unsigned long long fresh = gWindowFresh;
    unsigned long long totalReused = gTotalReused;
    int cached = gCacheCount;
    size_t cachedBytes = gCacheBytes;
    int live = gLiveCount;
    gWindowRequests = 0;
    gWindowReused = 0;
    gWindowFresh = 0;
    mutexUnlock(&gLock);

    fprintf(stream,
        "  gpu_buffers: requests=%llu reused=%llu fresh=%llu total_reused=%llu cached=%d (%zuMB) live=%d\n",
        requests,
        reused,
        fresh,
        totalReused,
        cached,
        cachedBytes / (1024 * 1024),
        live);
}

} // namespace fallout
