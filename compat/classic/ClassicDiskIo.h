// Bounded, allocation-free recording on the synchronous file path. Times describe this
// host only; a PAK refill includes repositioning and the unbuffered stdio read.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace ClassicDiskIo
{
    enum class Op { read, write, open, close, query, list, mkdir, remove, rename, flush, seek, count };
    const char* Caller();
    class Scope
    {
        const char* _previous;
    public:
        explicit Scope(const char* caller);
        ~Scope();
    };
    class Site
    {
        uintptr_t _previous;
    public:
        explicit Site(uintptr_t address);
        ~Site();
    };
    class Access
    {
        Op _op;
        std::string_view _path;
        const char* _caller;
        uint64_t _start;
        uintptr_t _site;
    public:
        Access(Op op, std::string_view path);
        void End(uint64_t bytes = 0, uint64_t offset = 0, bool failed = false);
    };
    void LogicalRead(uint64_t bytes);
    void Frame(uint32_t tick);
    void Reset();
    struct FrameCounters
    {
        uint64_t reads, readBytes, readUs, writes, writeBytes, writeUs, epoch;
    };
    FrameCounters Counters(); // No allocation; cumulative until Reset().
    // Summary is small enough for status. Detail is paged separately for Apple-event limits.
    std::string Summary();
    std::string Detail(bool events, unsigned first, unsigned limit);
}
