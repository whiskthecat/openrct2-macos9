#include "ClassicDiskIo.h"
#include "ClassicVfs.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <nlohmann/json.hpp>

namespace ClassicDiskIo
{
    namespace
    {
        using json = nlohmann::json;
        constexpr size_t kRows = 384, kEvents = 128;
        constexpr const char* kNames[]{"read", "write", "open", "close", "query", "list", "mkdir", "remove", "rename", "flush", "seek"};
        struct Counter { uint64_t calls = 0, bytes = 0, us = 0, maxUs = 0, failures = 0; };
        struct Row { char path[192]{}, caller[40]{}; Op op{}; uintptr_t site = 0; Counter counter; };
        struct Event { uint64_t id = 0, at = 0, offset = 0, bytes = 0, us = 0; uint32_t tick = 0; unsigned row = 0; bool failed = false; };
        std::array<Counter, static_cast<size_t>(Op::count)> gTotal;
        std::array<Row, kRows> gRows;
        std::array<Event, kEvents> gEvents;
        unsigned gRowsUsed = 0;
        uint64_t gSequence = 0, gOverflow = 0, gReadCalls = 0, gReadBytes = 0, gEpoch = 0;
        uint32_t gTick = 0;
        const char* gCaller = "engine";
        uintptr_t gSite = 0;
        void Add(Counter& c, uint64_t bytes, uint64_t us, bool failed)
        {
            c.calls++; c.bytes += bytes; c.us += us; c.maxUs = std::max(c.maxUs, us); c.failures += failed;
        }
        json Counts(const Counter& c)
        {
            return {{"calls", c.calls}, {"bytes", c.bytes}, {"us", c.us}, {"max_us", c.maxUs}, {"failures", c.failures}};
        }
    }
    const char* Caller() { return gCaller; }
    Scope::Scope(const char* caller) : _previous(gCaller) { gCaller = caller; }
    Scope::~Scope() { gCaller = _previous; }
    Site::Site(uintptr_t address) : _previous(gSite) { gSite = address; }
    Site::~Site() { gSite = _previous; }
    Access::Access(Op op, std::string_view path) : _op(op), _path(path), _caller(gCaller), _start(ClassicHost::Microseconds()), _site(gSite)
    {
        if (!gEpoch) gEpoch = _start;
    }
    void Access::End(uint64_t bytes, uint64_t offset, bool failed)
    {
        const uint64_t us = ClassicHost::Microseconds() - _start;
        Add(gTotal[static_cast<size_t>(_op)], bytes, us, failed);
        unsigned row = 0;
        // Paths and labels are truncated consistently, with bounded recording storage.
        for (; row < gRowsUsed; row++)
            if (gRows[row].op == _op && gRows[row].site == _site && std::string_view(gRows[row].path) == _path.substr(0, sizeof(Row::path) - 1)
                && std::strncmp(gRows[row].caller, _caller, sizeof(Row::caller) - 1) == 0) break;
        if (row == gRowsUsed)
        {
            if (gRowsUsed == kRows) { gOverflow++; return; }
            gRowsUsed++;
            auto& r = gRows[row]; r.op = _op; r.site = _site;
            const size_t n = std::min(_path.size(), sizeof(r.path) - 1);
            std::memcpy(r.path, _path.data(), n); r.path[n] = 0;
            std::strncpy(r.caller, _caller, sizeof(r.caller) - 1);
        }
        Add(gRows[row].counter, bytes, us, failed);
        auto& event = gEvents[gSequence % kEvents];
        event = {++gSequence, _start - gEpoch, offset, bytes, us, gTick, row, failed};
    }
    void LogicalRead(uint64_t bytes) { gReadCalls++; gReadBytes += bytes; }
    void Frame(uint32_t tick) { gTick = tick; }
    void Reset()
    {
        gTotal = {}; gRows = {}; gEvents = {}; gRowsUsed = 0; gSequence = gOverflow = gReadCalls = gReadBytes = 0;
        gEpoch = ClassicHost::Microseconds();
    }
    FrameCounters Counters()
    {
        const auto& r = gTotal[static_cast<size_t>(Op::read)];
        const auto& w = gTotal[static_cast<size_t>(Op::write)];
        return {r.calls, r.bytes, r.us, w.calls, w.bytes, w.us, gEpoch};
    }
    std::string Summary()
    {
        json out{{"elapsed_us", gEpoch ? ClassicHost::Microseconds() - gEpoch : 0}, {"logical_reads", gReadCalls},
            {"logical_bytes", gReadBytes}, {"rows", gRowsUsed}, {"events", gSequence}, {"row_overflow", gOverflow},
            {"recording_bytes", sizeof(gRows) + sizeof(gEvents) + sizeof(gTotal)}};
        for (size_t i = 0; i < gTotal.size(); i++) if (gTotal[i].calls) out[kNames[i]] = Counts(gTotal[i]);
        return out.dump();
    }
    std::string Detail(bool events, unsigned first, unsigned limit)
    {
        json out{{"summary", json::parse(Summary())}, {"items", json::array()}};
        limit = std::min(limit, 8u);
        if (events)
        {
            const uint64_t oldest = gSequence > kEvents ? gSequence - kEvents : 0;
            out["oldest"] = oldest; out["total"] = gSequence;
            // first=0 means start at the oldest retained event; otherwise a zero-based sequence.
            const uint64_t begin = first ? std::max<uint64_t>(first, oldest) : oldest;
            for (uint64_t i = begin; i < gSequence && i < begin + limit; i++)
            {
                const auto& e = gEvents[i % kEvents]; const auto& r = gRows[e.row];
                out["items"].push_back({{"id", e.id}, {"at_us", e.at}, {"tick", e.tick}, {"op", kNames[static_cast<size_t>(r.op)]},
                    {"path", r.path}, {"caller", r.caller}, {"site", r.site}, {"offset", e.offset}, {"bytes", e.bytes}, {"us", e.us}, {"failed", e.failed}});
            }
        }
        else
        {
            out["total"] = gRowsUsed;
            for (unsigned i = first; i < gRowsUsed && i < first + limit; i++)
            {
                const auto& r = gRows[i]; auto item = Counts(r.counter);
                item["op"] = kNames[static_cast<size_t>(r.op)]; item["path"] = r.path; item["caller"] = r.caller; item["site"] = r.site;
                out["items"].push_back(std::move(item));
            }
        }
        return out.dump();
    }
}
