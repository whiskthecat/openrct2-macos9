#include "ClassicLoadProgress.h"

#include "ClassicVfs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#ifdef OPENRCT2_CLASSIC_MAC
    #include <Events.h>
#endif
#include <openrct2/Context.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/core/Console.hpp>
#include <openrct2/localisation/StringIds.h>

namespace ClassicLoadProgress
{
    namespace
    {
        bool enabled = true, tracing = false, redraw = true, active = false, publishing = false;
        double begin = 0, end = 1, highWater = 0;
        const char *session = "", *stage = "";
        uint64_t started = 0, lastPaint = 0, messages = 0, drawing = 0;
#ifdef OPENRCT2_CLASSIC_MAC
        uint64_t lastMessages = 0;
#endif
        uint32_t paints = 0;
        struct Timing
        {
            const char* name = nullptr;
            uint64_t us = 0;
        };
        std::array<Timing, 32> timings{};
        struct Picture
        {
            uint32_t us, percent;
            int32_t x, width;
            const char* stage;
        };
        std::array<Picture, 2048> pictures{};
        size_t count = 0;
        bool overflow = false;
        std::array<double, 5> parkEdges{ 0, .03, .72, .74, .99 };
        uint64_t interval = 100000;
        double paintCost = 1000;
#ifdef OPENRCT2_CLASSIC_MAC
        uint32_t nextCheckTick = 0;
#endif
        bool Due()
        {
            if (!lastPaint)
                return true;
#ifdef OPENRCT2_CLASSIC_MAC
            return static_cast<int32_t>(static_cast<uint32_t>(TickCount()) - nextCheckTick) >= 0;
#else
            return ClassicHost::Microseconds() - lastPaint >= interval;
#endif
        }
    } // namespace
    void Configure(bool value, bool trace, bool draw)
    {
        enabled = value;
        tracing = trace;
        redraw = draw;
    }
    bool Enabled()
    {
        return enabled;
    }
    bool Active()
    {
        return active;
    }
    bool Redraw()
    {
        return redraw;
    }
    bool Tracing()
    {
        return tracing;
    }
    bool NeedsMessages()
    {
#ifdef OPENRCT2_CLASSIC_MAC
        const auto now = ClassicHost::Microseconds();
        // Drawing the vehicle needs no Toolbox input dispatch. Give the other
        // applications time at least once a second without granting an entire
        // Process Manager time slice for every small movement of the vehicle.
        if (lastMessages && now - lastMessages < 1000000)
            return false;
        lastMessages = now;
        EventRecord event{};
        if (EventAvail(everyEvent, &event))
            return true;
        // No application input to dispatch while its state is being rebuilt.
        // Mask zero still enters the Process Manager so Finder/AgentChat run.
        ClassicHost::Yield(0);
        return false;
#else
        return true;
#endif
    }
    bool Publishing()
    {
        return publishing;
    }
    Session::Session(const char* name)
    {
        if (!name || !enabled || gOpenRCT2Headless || active)
            return;
        _owner = active = true;
        session = name;
        stage = name;
        begin = highWater = 0;
        end = 1;
        started = ClassicHost::Microseconds();
        lastPaint = 0;
        interval = 100000;
        paintCost = 1000;
#ifdef OPENRCT2_CLASSIC_MAC
        nextCheckTick = 0;
        lastMessages = 0;
#endif
        messages = drawing = paints = count = 0;
        parkEdges = { 0, .03, .72, .74, .99 };
        overflow = false;
        timings = {};
    }
    Session::~Session()
    {
        if (!_owner)
            return;
        const auto elapsed = ClassicHost::Microseconds() - started;
        active = false;
        OpenRCT2::Console::WriteLine(
            "loading: %s elapsed_us=%lu paints=%u messages_us=%lu draw_us=%lu "
            "overflow=%u",
            session, static_cast<unsigned long>(elapsed), paints, static_cast<unsigned long>(messages),
            static_cast<unsigned long>(drawing), overflow);
        if (tracing)
            for (const auto& t : timings)
                if (t.name)
                    OpenRCT2::Console::WriteLine("loadstage: %s us=%lu", t.name, static_cast<unsigned long>(t.us));
        if (tracing)
            for (size_t i = 0; i < count; ++i)
            {
                const auto& p = pictures[i];
                OpenRCT2::Console::WriteLine(
                    "loadbar: %s us=%u percent=%u x=%d width=%d stage=%s", session, p.us, p.percent, p.x, p.width, p.stage);
            }
    }
    Range::Range(double from, double to, const char* name)
    {
        if (!active)
            return;
        _active = true;
        _begin = begin;
        _end = end;
        _name = stage;
        const auto span = end - begin;
        end = begin + span * std::clamp(to, 0.0, 1.0);
        begin += span * std::clamp(from, 0.0, 1.0);
        if (name)
        {
            stage = name;
            if (tracing)
            {
                _timedName = name;
                _started = ClassicHost::Microseconds();
            }
        }
    }
    Range::~Range()
    {
        if (_active)
        {
            if (_timedName)
                for (auto& t : timings)
                    if (!t.name || std::strcmp(t.name, _timedName) == 0)
                    {
                        t.name = _timedName;
                        t.us += ClassicHost::Microseconds() - _started;
                        break;
                    }
            begin = _begin;
            end = _end;
            stage = _name;
        }
    }
    void PlanPark(double parseMs, double readyMs, double importMs, double soundMs)
    {
        if (std::strcmp(session, "title") == 0)
            soundMs = 0;
        const double objects = parseMs + readyMs;
        const double total = std::max(1.0, objects + importMs + soundMs);
        parkEdges[2] = .03 + .96 * objects / total;
        parkEdges[3] = parkEdges[2] + .96 * importMs / total;
    }
    ParkPhase::ParkPhase(unsigned phase)
        : Range(
            parkEdges[std::min(phase, 3u)], parkEdges[std::min(phase, 3u) + 1],
            std::array<const char*, 4>{ "park-decode", "objects", "park-import", "sound-preload" }[std::min(phase, 3u)])
    {
    }
    void Report(uint64_t done, uint64_t total)
    {
        Report(total ? static_cast<double>(done) / total : 1.0);
    }
    void Report(double fraction)
    {
        if (!active)
            return;
        highWater = std::max(highWater, begin + (end - begin) * std::clamp(fraction, 0.0, 1.0));
        Pulse();
    }
    void ReadProgress(uint64_t done, uint64_t total)
    {
        // FileStream deserialisation performs hundreds of thousands of small
        // scalar reads. Gate before creating ranges or converting 64-bit byte
        // counts to doubles, rather than doing that work before Pulse's gate.
        if (!active || !Due())
            return;
        // Cache headers are inspected before the startup plan exists. Finishing
        // a tiny header must not imply that half of the entire load is done.
        // Root-level reads still service the system, without inventing a share.
        if (std::strcmp(stage, session) == 0)
        {
            Pulse();
            return;
        }
        const bool direct = std::strncmp(stage, "sound-", 6) == 0 || std::strcmp(stage, "park-decode") == 0;
        Range range(0, direct ? 1 : .55, "pack-read");
        Report(done, total);
    }
    void Pulse()
    {
        if (!active)
            return;
#ifdef OPENRCT2_CLASSIC_MAC
        // TickCount reads the low-memory tick counter. Most checkpoints need no
        // expensive Microseconds trap, floating point formatting, or redraw.
        const auto tick = static_cast<uint32_t>(TickCount());
        if (lastPaint && static_cast<int32_t>(tick - nextCheckTick) < 0)
            return;
#endif
        const auto now = ClassicHost::Microseconds();
        if (lastPaint && now - lastPaint < interval)
            return;
        lastPaint = now;
#ifdef OPENRCT2_CLASSIC_MAC
        nextCheckTick = tick + static_cast<uint32_t>((interval + 16666) / 16667);
#endif
        if (redraw)
        {
            publishing = true;
            struct RestorePublishing
            {
                ~RestorePublishing()
                {
                    publishing = false;
                }
            } restore;
            OpenRCT2::GetContext()->SetProgress(
                static_cast<uint32_t>(std::floor(highWater * 10000)), 10000, STR_STRING_M_PERCENT);
            publishing = false;
        }
    }
    void Finish()
    {
        if (!active)
            return;
        highWater = 1;
        lastPaint = 0;
        Pulse();
    }
    void Presented(uint32_t current, uint32_t total, int32_t x, int32_t width)
    {
        if (!active || !tracing || !total)
            return;
        if (count && pictures[count - 1].percent == current * 100 / total && pictures[count - 1].x == x)
            return;
        if (count == pictures.size())
        {
            overflow = true;
            return;
        }
        pictures[count++] = { static_cast<uint32_t>(ClassicHost::Microseconds() - started), current * 100 / total, x, width,
                              stage };
    }
    void RepaintCost(uint64_t messagesUs, uint64_t drawUs)
    {
        if (!active)
            return;
        ++paints;
        messages += messagesUs;
        drawing += drawUs;
        // Aim for <=1% direct repaint/event overhead. A slow display gets fewer
        // pictures rather than a slower load; bound gaps to half a second. A
        // scheduler/foreground-switch outlier must not depress the entire load.
        const auto cost = messagesUs + drawUs;
        if (cost <= 20000)
            paintCost = .75 * paintCost + .25 * cost;
        // Warm repositories finish in roughly a second: retain ten pictures/sec
        // there so a foreground-switch cost cannot reduce startup to two steps.
        interval = std::strcmp(session, "startup") == 0
            ? 100000
            : std::clamp<uint64_t>(static_cast<uint64_t>(paintCost * 100), 100000, 500000);
        lastPaint = ClassicHost::Microseconds();
#ifdef OPENRCT2_CLASSIC_MAC
        // Update the cheap deadline after measuring the picture, including a
        // changed cadence. Otherwise checkpoints hit an expired tick deadline
        // and repeatedly call Microseconds until the longer interval ends.
        nextCheckTick = static_cast<uint32_t>(TickCount()) + static_cast<uint32_t>((interval + 16666) / 16667);
#endif
    }
} // namespace ClassicLoadProgress
