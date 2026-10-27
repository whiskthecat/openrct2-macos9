// Replacement C allocator for Mac OS 9.
//
// The toolchain's malloc asks the Mac Memory Manager for every block. The
// engine makes hundreds of thousands of small allocations, and the classic
// heap slows to a crawl with that many blocks. This allocator takes a few
// large blocks from the Memory Manager and serves small requests from
// size-class free lists; only big requests go to the Memory Manager directly.
// Defining every entry point of the library's malloc module replaces it.
#ifdef OPENRCT2_CLASSIC_MAC
    #include <MacMemory.h>
    #include "ClassicVfs.h"
    #include <cerrno>
    #include <cstddef>
    #include <cstdint>
    #include <cstring>
    #include <reent.h>

namespace
{
    constexpr size_t kChunkSize = 4 * 1024 * 1024;
    // Payload bytes. Blocks above this go to the Memory Manager and are given back when freed;
    // smaller ones stay in this allocator's pools for their size class for good. (It was 128 KB:
    // paint lists and per-object image blocks of 20-100 KB then stayed held after a zoom or park
    // change.)
    constexpr size_t kLargeThreshold = 16 * 1024;
    constexpr uint32_t kSmallMagic = 0x5AC00000u;  // low bits: size class
    constexpr uint32_t kLargeMagic = 0x1A26E000u;
    constexpr uint32_t kAlignedMagic = 0xA116E000u;
    constexpr int kClassCount = 64;

    size_t gClassSize[kClassCount];
    void* gFreeList[kClassCount];
    int gClasses = 0;
    uint8_t* gBump = nullptr;
    uint8_t* gBumpEnd = nullptr;
    size_t gChunks = 0, gLargeBytes = 0;

    void Initialise()
    {
        // 16, 24, 32, then four steps per doubling up to the large threshold.
        size_t size = 16;
        while (gClasses < kClassCount)
        {
            gClassSize[gClasses++] = size;
            if (size >= kLargeThreshold)
                break;
            const size_t step = size < 32 ? 8 : (size_t(1) << (31 - __builtin_clz(static_cast<unsigned>(size)))) / 4;
            size += step;
        }
    }

    int ClassFor(size_t size)
    {
        int low = 0, high = gClasses - 1;
        while (low < high)
        {
            const int middle = (low + high) / 2;
            if (gClassSize[middle] >= size)
                high = middle;
            else
                low = middle + 1;
        }
        return low;
    }

    // Live blocks of a megabyte or more, to see what the memory is used for.
    constexpr size_t kTrackedSize = 1024 * 1024;
    struct Tracked
    {
        void* pointer;
        uint32_t size, order;
    };
    Tracked gTracked[256]{};
    uint32_t gTrackedOrder = 0;

    // For the log when memory runs out: the request that could not be met.
    unsigned long gFailedSize = 0, gFailedFree = 0, gFailedLargest = 0, gFailures = 0;
    void NoteFailure(size_t size)
    {
        gFailures++;
        gFailedSize = static_cast<unsigned long>(size);
        gFailedFree = static_cast<unsigned long>(FreeMem());
        gFailedLargest = static_cast<unsigned long>(MaxBlock());
    }

    unsigned long gLargeCalls = 0;
    uint32_t gAllocationCalls = 0, gAllocationBytes = 0, gHeapCalls = 0, gHeapUs = 0;
    Ptr TimedNewPtr(Size size)
    {
        const auto start = ClassicHost::Microseconds();
        Ptr result = NewPtr(size);
        gHeapCalls++;
        gHeapUs += static_cast<uint32_t>(ClassicHost::Microseconds() - start);
        return result;
    }

    void* LargeAllocate(size_t size)
    {
        gLargeCalls++;
        Ptr raw = TimedNewPtr(static_cast<Size>(size + 24));
        if (raw == nullptr)
        {
            NoteFailure(size);
            return nullptr;
        }
        auto* payload = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(raw) + 16 + 7) & ~uintptr_t(7));
        auto* header = reinterpret_cast<uint32_t*>(payload);
        header[-3] = static_cast<uint32_t>(payload - reinterpret_cast<uint8_t*>(raw));
        header[-2] = kLargeMagic;
        header[-1] = static_cast<uint32_t>(size);
        gLargeBytes += size;
        if (size >= kTrackedSize)
            for (auto& slot : gTracked)
                if (slot.pointer == nullptr)
                {
                    slot.pointer = payload;
                    slot.size = static_cast<uint32_t>(size);
                    slot.order = ++gTrackedOrder;
                    break;
                }
        return payload;
    }

    // Per size class: blocks handed out and not freed, and blocks ever cut from the pools (the
    // difference sits on the free list, usable only for that size).
    uint32_t gLive[kClassCount]{}, gCarved[kClassCount]{};

    void* SmallAllocate(int sizeClass)
    {
        gLive[sizeClass]++;
        if (void* block = gFreeList[sizeClass])
        {
            gFreeList[sizeClass] = *static_cast<void**>(block);
            return block;
        }
        gCarved[sizeClass]++;
        const size_t total = gClassSize[sizeClass] + 8;
        if (gBump == nullptr || static_cast<size_t>(gBumpEnd - gBump) < total)
        {
            Ptr chunk = TimedNewPtr(static_cast<Size>(kChunkSize));
            if (chunk == nullptr)
            {
                NoteFailure(kChunkSize);
                return nullptr;
            }
            gChunks++;
            gBump = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(chunk) + 7) & ~uintptr_t(7));
            gBumpEnd = reinterpret_cast<uint8_t*>(chunk) + kChunkSize;
        }
        auto* header = reinterpret_cast<uint32_t*>(gBump);
        header[0] = kSmallMagic | static_cast<uint32_t>(sizeClass);
        header[1] = 0;
        gBump += total;
        return header + 2;
    }

    size_t Capacity(void* pointer)
    {
        const auto* header = static_cast<const uint32_t*>(pointer);
        if ((header[-2] & 0xFFFFF000u) == kSmallMagic)
            return gClassSize[header[-2] & 0xFFF];
        if (header[-2] == kLargeMagic)
            return header[-1];
        return 0;
    }
} // namespace

extern "C" {
void* malloc(size_t size)
{
    gAllocationCalls++;
    gAllocationBytes += static_cast<uint32_t>(size);
    if (gClasses == 0)
        Initialise();
    if (size == 0)
        size = 1;
    void* result = size > kLargeThreshold ? LargeAllocate(size) : SmallAllocate(ClassFor(size));
    if (result == nullptr)
        errno = ENOMEM;
    return result;
}

void free(void* pointer)
{
    if (pointer == nullptr)
        return;
    auto* header = static_cast<uint32_t*>(pointer);
    if ((header[-2] & 0xFFFFF000u) == kSmallMagic)
    {
        const int sizeClass = static_cast<int>(header[-2] & 0xFFF);
        gLive[sizeClass]--;
        *static_cast<void**>(pointer) = gFreeList[sizeClass];
        gFreeList[sizeClass] = pointer;
    }
    else if (header[-2] == kLargeMagic)
    {
        gLargeBytes -= header[-1];
        if (header[-1] >= kTrackedSize)
            for (auto& slot : gTracked)
                if (slot.pointer == pointer)
                {
                    slot.pointer = nullptr;
                    break;
                }
        header[-2] = 0;
        DisposePtr(reinterpret_cast<Ptr>(static_cast<uint8_t*>(pointer) - header[-3]));
    }
    else if (header[-2] == kAlignedMagic)
    {
        header[-2] = 0;
        free(static_cast<uint8_t*>(pointer) - header[-1]);
    }
    // Anything else did not come from this allocator; leave it alone.
}

void* calloc(size_t count, size_t size)
{
    const size_t total = count * size;
    if (size != 0 && total / size != count)
        return nullptr;
    void* result = malloc(total);
    if (result != nullptr)
        std::memset(result, 0, total);
    return result;
}

void* realloc(void* pointer, size_t size)
{
    if (pointer == nullptr)
        return malloc(size);
    if (size == 0)
    {
        free(pointer);
        return nullptr;
    }
    const size_t capacity = Capacity(pointer);
    if (capacity >= size && (capacity <= kLargeThreshold || size > capacity / 2))
        return pointer;
    void* result = malloc(size);
    if (result != nullptr)
    {
        std::memcpy(result, pointer, capacity < size ? capacity : size);
        free(pointer);
    }
    return result;
}

void* memalign(size_t alignment, size_t size)
{
    if (alignment <= 8)
        return malloc(size);
    auto* base = static_cast<uint8_t*>(malloc(size + alignment + 8));
    if (base == nullptr)
        return nullptr;
    auto* aligned = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(base) + 8 + alignment - 1) & ~(uintptr_t(alignment) - 1));
    auto* header = reinterpret_cast<uint32_t*>(aligned);
    header[-2] = kAlignedMagic;
    header[-1] = static_cast<uint32_t>(aligned - base);
    return aligned;
}

void* _malloc_r(struct _reent*, size_t size)
{
    return malloc(size);
}
void _free_r(struct _reent*, void* pointer)
{
    free(pointer);
}
void* _calloc_r(struct _reent*, size_t count, size_t size)
{
    return calloc(count, size);
}
void* _realloc_r(struct _reent*, void* pointer, size_t size)
{
    return realloc(pointer, size);
}
// The C runtime calls this to pull in its own malloc module; ours replaces it.
void referenceMyMalloc(void)
{
}
// For logs: the last request that failed (size, free memory and largest free block then).
void ClassicAllocatorFailure(unsigned long* count, unsigned long* size, unsigned long* freeBytes, unsigned long* largest)
{
    *count = gFailures;
    *size = gFailedSize;
    *freeBytes = gFailedFree;
    *largest = gFailedLargest;
}
// Live blocks of a megabyte or more: fills sizes[] and orders[] (order of allocation), returns
// how many; *chunkBytes is what the small-block pools hold.
int ClassicAllocatorLargeBlocks(unsigned long* sizes, unsigned long* orders, int capacity, unsigned long* chunkBytes)
{
    int count = 0;
    for (const auto& slot : gTracked)
        if (slot.pointer != nullptr && count < capacity)
        {
            sizes[count] = slot.size;
            orders[count++] = slot.order;
        }
    *chunkBytes = static_cast<unsigned long>(gChunks * kChunkSize);
    return count;
}
// Small-block statistics per size class: fills size[], live[] and carved[]; returns the count.
int ClassicAllocatorClasses(unsigned long* size, unsigned long* live, unsigned long* carved, int capacity)
{
    int count = 0;
    for (int i = 0; i < gClasses && count < capacity; i++)
        if (gCarved[i] != 0)
        {
            size[count] = static_cast<unsigned long>(gClassSize[i]);
            live[count] = gLive[i];
            carved[count++] = gCarved[i];
        }
    return count;
}
// How many blocks have been asked of the Memory Manager so far (each call is slow).
unsigned long ClassicAllocatorLargeCalls(void)
{
    return gLargeCalls;
}
// For logs: memory taken from the Memory Manager by this allocator.
void ClassicAllocatorActivity(uint32_t* calls, uint32_t* bytes, uint32_t* heapCalls, uint32_t* heapUs)
{
    *calls = gAllocationCalls; *bytes = gAllocationBytes; *heapCalls = gHeapCalls; *heapUs = gHeapUs;
}
unsigned long ClassicAllocatorBytes(void)
{
    return static_cast<unsigned long>(gChunks * kChunkSize + gLargeBytes);
}
}
#endif
