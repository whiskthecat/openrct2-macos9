#pragma once
#include "ClassicVfs.h"
#include <algorithm>
#include <cstdint>

namespace ClassicPreload
{
    constexpr uint64_t MiB = 1024 * 1024;
    struct Plan { uint64_t bytes = 0; bool full = false; };
    // Includes working/rendering growth and save compression, not just the
    // retained snapshot. Never probe by allocating a succession of large blocks.
    inline Plan Choose(const ClassicHost::MemoryInfo& memory, unsigned mode, uint64_t payload)
    {
        const uint64_t reserve = 32 * MiB;
        const bool machineFits = memory.physical >= memory.partition + 48 * MiB;
        if (mode != 0 && (mode == 1 || machineFits) && memory.free >= payload + reserve
            && memory.largest >= payload + 4 * MiB)
            return {payload, true};
        const uint64_t workingReserve = 16 * MiB;
        const uint64_t available = std::min(memory.free > workingReserve ? memory.free - workingReserve : 0,
            memory.largest > 4 * MiB ? memory.largest - 4 * MiB : 0);
        return {std::min(8 * MiB, available) / MiB * MiB, false};
    }
}
