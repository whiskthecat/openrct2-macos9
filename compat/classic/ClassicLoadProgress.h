// Synchronous loading progress for the single-threaded pack builds.
#pragma once
#include <cstdint>

namespace ClassicLoadProgress
{
    // Diagnostic controls are per launch and never saved in the user's config.
    void Configure(bool enabled, bool trace = false, bool redraw = true);
    bool Enabled();
    bool Active();
    bool Redraw();
    bool Publishing();
    bool Tracing();
    // Service the cooperative scheduler; poll the full UI only for queued input.
    bool NeedsMessages();

    class Session
    {
        bool _owner = false;

    public:
        explicit Session(const char* name);
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
    };

    // A nested piece of actual work. Fractions are relative to the enclosing
    // range. Destruction restores its parent, including during a failed load.
    class Range
    {
        double _begin = 0, _end = 1;
        const char* _name = nullptr;
        bool _active = false;
        const char* _timedName = nullptr;
        uint64_t _started = 0;

    public:
        Range(double begin, double end, const char* name = nullptr);
        ~Range();
        Range(const Range&) = delete;
        Range& operator=(const Range&) = delete;
    };
    void Report(uint64_t done, uint64_t total);
    void Report(double fraction);
    void Pulse();
    void ReadProgress(uint64_t done, uint64_t total);
    // Called only by Context around a real repaint and by the progress widget
    // after calculating its real sprite position. No disk I/O in these hooks.
    void Presented(uint32_t current, uint32_t total, int32_t x, int32_t width);
    void RepaintCost(uint64_t messagesUs, uint64_t drawUs);
    void Finish();
    // Work forecasts use measured PPC stage costs, then the real newly required
    // object list and the selected sound-memory policy.
    void PlanPark(double parseMs, double readyMs, double importMs, double soundMs);
    class ParkPhase : public Range
    {
    public:
        explicit ParkPhase(unsigned phase); // 0 decode, 1 objects, 2 import, 3 audio
    };
} // namespace ClassicLoadProgress
