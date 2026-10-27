// The game's UI library is written against SDL. Instead of SDL we provide a
// small set of SDL-named functions (SdlShim.cpp) on top of this interface,
// which each platform implements: a Toolbox version for Mac OS 9 and a
// file-writing version for automated tests on a development host.
#pragma once
#include <algorithm>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

union SDL_Event;

namespace ShimHost
{
    extern bool gWidePresent, gBatchCursor;
    extern bool gPresentAlignedCopy;
    extern unsigned gPresentPrefetchRows; // RAM source row distance; 0 disables.
    struct Colour
    {
        uint8_t red, green, blue;
    };

    // Opens the display. Returns false if it cannot.
    bool Open(int requestedWidth, int requestedHeight, int& width, int& height);
    void Close();
    struct DisplayMode { int width, height; uint32_t mode = 0, depth = 0; };
    const std::vector<DisplayMode>& DisplayModes();
    std::string DisplayDiagnostics(bool verify);
#ifdef OPENRCT2_CLASSIC_MAC
    bool ResizeDisplay(int width, int height);
    void MarkPictureInput();
    bool VSyncAvailable();
    void SetVSync(bool on);
    std::string VSyncDiagnostics(const std::string& command);
    bool PaintFullCopy(std::vector<uint8_t>& output, long frame, const std::string& path);
    struct VSyncTiming { uint64_t waitUs, mirrorUs, paletteUs, frame; uint32_t line;
        uint64_t flushUs, publishUs, mirrorPixels, mirrorRects, mirrorFull, mirrorPartial, late, queued, submitted, readyLine, phaseUs, refreshUs; };
    VSyncTiming VSyncCosts();
#endif
#ifdef OPENRCT2_CLASSIC_MAC
    bool SystemMenuBarHidden();
    std::string CursorDiagnostics(bool reset, const std::string& options = "{}");
#endif
#if defined(OPENRCT2_CLASSIC_MAC) && defined(OPENRCT2_PERF_METRICS)
    // Observe external writes in the existing clock-repair rectangle, without changing repair.
    bool ClockDamaged();
    std::string ClockDiagnostics(bool reset);
#endif
    // The 256-colour palette for the indexed pixels passed to Present.
    void SetPalette(const Colour colours[256]);
    // Shows a rectangle of the indexed frame. `bits` addresses pixel (0,0) of the full frame.
    void Present(const uint8_t* bits, int pitch, int left, int top, int right, int bottom);
    // The same, writing only the pixels that differ from `shown` (the caller's copy of what the
    // display holds, same pitch), and bringing `shown` up to date. Writes to video memory are
    // the slow part of presenting; a repainted rectangle mostly holds unchanged pixels.
    void PresentChanged(const uint8_t* bits, uint8_t* shown, int pitch, int left, int top, int right, int bottom);
    extern uint64_t gPresentChangedPixels, gPresentComparedPixels;
    // Calls write(y, x0, x1) for each run of changed pixels in the rectangle and copies it to shown.
    template<typename TWrite>
    inline void PresentChangedRuns(const uint8_t* bits, uint8_t* shown, int pitch, int left, int top, int right, int bottom,
                                   TWrite&& write)
    {
        for (int y = top; y < bottom; y++)
        {
            const uint8_t* a = bits + static_cast<size_t>(y) * pitch;
            uint8_t* s = shown + static_cast<size_t>(y) * pitch;
            int x = left;
            while (x < right)
            {
                while (x + 8 <= right && __builtin_memcmp(a + x, s + x, 8) == 0)
                    x += 8;
                while (x < right && a[x] == s[x])
                    x++;
                if (x >= right)
                    break;
                const int start = x;
                int end = x + 1;
                for (int i = x + 1; i < right; i++)
                {
                    if (a[i] != s[i])
                        end = i + 1;
                    else if (i - end >= 12)
                        break;
                }
                // Whole 8-byte words where the rectangle allows: video memory dislikes byte stores.
                const int from = std::max(left, start & ~7), to = std::min(right, (end + 7) & ~7);
                write(y, from, to);
                __builtin_memcpy(s + from, a + from, static_cast<size_t>(to - from));
                gPresentChangedPixels += static_cast<uint64_t>(to - from);
                x = to;
            }
        }
        gPresentComparedPixels += static_cast<uint64_t>(right - left) * (bottom - top);
    }
    // Recover the stale rectangle of the displayed indexed frame after a lazy scroll.
    bool ReadPixels(uint8_t* bits, int pitch, int width, int height, int left, int top, int right, int bottom);
    extern uint64_t gIdleReadbackCalls, gIdleReadbackUs;
    extern uint64_t gDamageMicros, gScreenScrollMicros, gScreenScrollWaitMicros, gPointerPixels;
    // Moves already-presented pixels: the block that will end at (x,y) sized
    // width x height comes from (x - dx, y - dy). Returns false if the host
    // cannot do this itself; the caller then presents the block normally.
    bool Scroll(int x, int y, int width, int height, int dx, int dy);
    // Clipped physical rows of equally spaced rain/snow pixels (or scaled blocks).
    struct WeatherSpan
    {
        uint16_t x, y, count, step;
        uint8_t colour, width;
    };
    struct WeatherPixels
    {
        std::vector<uint32_t> pixels; // (host display offset << 8) | previous palette index
        size_t count = 0;
        int left = 0, right = 0, top = 0, bottom = 0; // pointer exclusion at draw time
    };
    // Save exactly the pixels overwritten; clean RAM is optional after lazy scrolls.
    void ShowWeather(const WeatherSpan* spans, size_t count, WeatherPixels& saved, const uint8_t* clean, int pitch);
    void RestoreWeather(const WeatherSpan* spans, size_t count, WeatherPixels& saved);
    // Indexed native displays are expensive to read; Linux can emulate this policy for pictures.
    bool WeatherNeedsCleanCanvas();
    struct WeatherTraffic
    {
        uint64_t displayReadBytes, displayWriteBytes, ramReadBytes, readbackBytes;
    };
    extern WeatherTraffic gWeatherTraffic;
    // The pointer's shape: 16 rows of 16 pixels, most significant bit first. A mask bit marks an
    // opaque pixel; a data bit makes it black, otherwise white.
    void SetPointerShape(const uint16_t data[16], const uint16_t mask[16], int hotX, int hotY);
    // Large scaled cursors use a display overlay; coordinates and hot spot are physical pixels.
    bool SoftwarePointer(uint16_t data[16], uint16_t mask[16], int& hotX, int& hotY);
    // Mac: free application memory in bytes, or the largest single free block.
    long FreeMemory(bool largestBlock);
    // Whether Scroll will work (then the frame kept in memory need not be moved as well).
    bool CanScroll();
    // Called once per drawn frame after all presents.
    void EndFrame();
    // Translates pending native input into SDL events via PushEvent.
    void PumpEvents();
    uint32_t Milliseconds();
    void ShowCursor(bool visible);
    // memmove by the host's fastest routine (overlap allowed).
    void MoveMemory(void* to, const void* from, size_t bytes);
    // Moves the real pointer if the host can; returns false if it cannot.
    bool WarpPointer(int x, int y);
    // Shim internals used by the SDL-named functions.
    void WarpTo(int x, int y);
    void PointerShown();
    void Message(const char* text);

    // Provided by the shim for hosts to call.
    void PushEvent(const SDL_Event& event);
    void SetMouse(int x, int y, uint32_t sdlButtonMask);
    void SetKey(int sdlScancode, bool down);
    bool TextInputActive();

    // Frame profile, printed every few seconds: where the time of the real
    // game loop goes (painting, getting pixels to the screen, the OS, the rest).
    // Optional native event-pump attribution. Off in ordinary play.
    struct PumpProfile
    {
        uint64_t calls, waitCalls, waitMicros, eventMicros, inputMicros;
        uint64_t nullReturns, interceptedReturns, events[24], waitBins[6];
        uint64_t pollCalls, pollMicros;
    };
    extern bool gTracePump;
    extern PumpProfile gPumpProfile;
    extern uint64_t gAudioPumpMicros;
#ifdef OPENRCT2_CLASSIC_MAC
    std::string EventCallProbe();
    void SetEventPolling(bool on);
    bool EventPolling();
    std::string EventDiagnostics(const std::string& command);
#endif
    enum class Phase { paint, present, pump, count };
    uint64_t Microseconds();
    uint64_t PaintMicroseconds();
    void SetFastPaintClock(bool enabled);
    void AddPhaseTime(Phase phase, uint64_t microseconds);
    void FrameDone(uint32_t gameTicks);
    struct ProfileTotals // since start-up
    {
        uint64_t frames, microseconds, ticks, phase[static_cast<int>(Phase::count)];
        // Inside "paint": world painter list building, sorting, drawing; column sessions and repaint rectangles.
        uint64_t world[4], columns, builds, patches, shifts, shiftTiles, shiftChecks, rects, rectPixels;
        uint64_t loop[4], paletteMicros, paletteSets, widgetMicros, widgetDraws, shiftMicros;
        uint64_t entityEntries, mapEntries, nodesCollected, nodesSorted;
        uint64_t paintCosts[8], paintWork[13], damageMicros, screenScrollMicros, scrollRects, scrollPixels;
        uint64_t guestPoseMicros, guestPoseVisits, entityFlushMicros;
        uint64_t scrollSourcePixels[3], scrollGeometricPixels, entityDamagePixels, pointerPixels;
        uint64_t columnBuildMicros, columnShiftMicros;
        PumpProfile pump;
        uint64_t audioPumpMicros;
        WeatherTraffic weather;
    };
    void CountRepaint(int width, int height);
    void CountPalette(uint64_t microseconds);
    void CountShift(uint64_t microseconds);
    void SetDirtyCells(int width, int height);
    void SetHardwareScroll(bool on);
    void SetDeferredPalette(bool on);
    void SetAlignedCells(bool on);
    void SetWordRectangles(bool on);
    void SetIdleRefresh(bool on);
    // JSON: the scrolls and rectangles the last frame sent to the screen (x, y, width, height).
    std::string LastFrameRects();
    void SetRowShiftOnly(bool on);
    // Free-running processor counter (PowerPC time base), for cheap fine-grained timing.
    uint64_t TimeBase();
    // Mac: times small QuickDraw blits through the video driver on the live screen; JSON text.
    std::string BlitProbe(int engineStage, int sourceMode, uint32_t sourceValue);
    // Mac: verify direct palette writes; fullRange also permits gradual broad changes.
    std::string PrepareDirectPalette(bool enable, bool fullRange = true);
    ProfileTotals Profile();

    // Remote control (RemoteControl.cpp): a JSON command in, a JSON reply out.
    std::string RemoteCommand(const std::string& json);
    void RemoteFrame();
    // Pointer position set by remote control; it holds until the real mouse moves.
    void RemoteMouse(int x, int y, uint32_t sdlButtonMask);
    struct PhaseTimer
    {
        Phase phase;
        uint64_t start = Microseconds();
        ~PhaseTimer()
        {
            AddPhaseTime(phase, Microseconds() - start);
        }
    };
} // namespace ShimHost
