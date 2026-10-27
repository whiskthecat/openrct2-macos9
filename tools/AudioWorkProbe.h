#pragma once
// Explicit sound diagnostic only. No engine RNG, assets, I/O, or timers inside the kernel.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <vector>

namespace AudioWorkProbe
{
    struct Work
    {
        std::vector<uint32_t> data;
        explicit Work(unsigned bytes)
        {
            if (!bytes) return;
            data.resize(bytes / sizeof(uint32_t));
            std::vector<uint32_t> nodes(bytes / 32);
            for (unsigned i = 0; i < nodes.size(); ++i) nodes[i] = i * 8;
            uint32_t random = 0x19840517;
            for (unsigned i = nodes.size() - 1; i; --i)
            {
                random = random * 1664525u + 1013904223u;
                std::swap(nodes[i], nodes[random % (i + 1)]);
            }
            for (unsigned i = 0; i < nodes.size(); ++i) data[nodes[i]] = nodes[(i + 1) % nodes.size()];
        }
        uint32_t Run(unsigned steps) const
        {
            uint32_t value = 0x12345678, at = 0;
            if (data.empty())
                for (unsigned i = 0; i < steps; ++i)
                {
                    value = std::rotl(value, 7) ^ (value * 1664525u + 1013904223u);
                    __asm__ volatile("" : "+r"(value));
                }
            else
                for (unsigned i = 0; i < steps; ++i)
                {
                    at = data[at];
                    value = std::rotl(value, 7) ^ at;
                    __asm__ volatile("" : "+r"(value), "+r"(at) : : "memory");
                }
            return value;
        }
        void Warm() const { if (!data.empty()) Run(unsigned(data.size() / 8) * 2); }
    };
}
