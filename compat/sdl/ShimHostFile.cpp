#include <openrct2/core/ScrollTrace.h>
#include <openrct2/core/OgTrace.h>
#include "WeatherRaster.h"
#ifdef OPENRCT2_PAK_VFS
#include "ClassicLoadProgress.h"
#endif
// Test display for development hosts: no window. Frames listed in
// RCT_SHOT_FRAMES (comma separated) are written as PPM images named
// $RCT_SHOT_PREFIX-<frame>.ppm, then the game is asked to quit. Input can be
// scripted with RCT_SCRIPT: "frame:x,y,button;..." clicks, or with RCT_REMOTE:
// a file of "<frame> <json command>" lines for the remote-control layer.
// RCT_FIXED_STEP=1 advances exactly one game tick per frame.
#ifndef OPENRCT2_CLASSIC_MAC
    #include "ShimHost.h"
#include <openrct2/core/CachePrefetch.h>
#include <openrct2/config/Config.h>

    #include <SDL.h>
    #include <openrct2/Context.h>
    #include <openrct2/scenes/SceneManager.h>
    #include <openrct2/interface/Viewport.h>
    #include <openrct2/drawing/NewDrawing.h>
    #include <openrct2/drawing/RenderTarget.h>
    #include <algorithm>
    #include <chrono>
    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    #include <string>
    #include <vector>

namespace ShimHost
{
    bool PaintFullCopy(std::vector<uint8_t>& output, long frame, const std::string& path);
    namespace
    {
        int gWidth = 800, gHeight = 600;
        std::vector<uint8_t> gScreen;
        Colour gPalette[256]{};
        long gFrame = 0;
        std::vector<long> gShots;
        struct Click
        {
            long frame;
            int x, y;
            int button;
            bool done;
        };
        std::vector<Click> gClicks;
        uint64_t gScrolls = 0, gPresentedPixels = 0;
        struct Remote
        {
            long frame;
            std::string command;
        };
        std::vector<Remote> gRemote;

        void WriteShot()
        {
            const char* prefix = std::getenv("RCT_SHOT_PREFIX");
            const std::string path = std::string(prefix != nullptr ? prefix : "shot") + "-" + std::to_string(gFrame) + ".ppm";
            const bool indicesOnly = std::getenv("RCT_SHOT_INDICES_ONLY") != nullptr;
            if (FILE* file = indicesOnly ? nullptr : std::fopen(path.c_str(), "wb"))
            {
                std::fprintf(file, "P6\n%d %d\n255\n", gWidth, gHeight);
                for (const auto index : gScreen)
                    std::fwrite(&gPalette[index], 1, 3, file);
                std::fclose(file);
            }
            std::fprintf(stderr, "shot frame %ld -> %s (scrolls %llu, presented pixels %llu)\n", gFrame, path.c_str(),
                static_cast<unsigned long long>(gScrolls), static_cast<unsigned long long>(gPresentedPixels));
            if (std::getenv("RCT_SCENES_TEST") != nullptr)
                if (FILE* file = std::fopen((path + ".indices").c_str(), "wb"))
                {
                    std::fwrite(gScreen.data(), 1, gScreen.size(), file);
                    std::fclose(file);
                }
            if (std::getenv("RCT_PAINT_COMPARE") != nullptr)
            {
                if (std::getenv("RCT_SHOT_RAM") != nullptr)
                    if (FILE* file = std::fopen((path + ".ram.indices").c_str(), "wb"))
                    {
                        const auto& rt = DrawingEngineGetRT();
                        for (int y = 0; y < rt.height; ++y)
                            std::fwrite(rt.bits + y * (rt.width + rt.pitch), 1, rt.width, file);
                        std::fclose(file);
                    }
                std::vector<uint8_t> full;
                if (!PaintFullCopy(full, gFrame, path)) std::abort();
                if (std::getenv("RCT_SCENES_TEST") != nullptr)
                    if (FILE* file = std::fopen((path + ".full.indices").c_str(), "wb"))
                    {
                        std::fwrite(full.data(), 1, full.size(), file);
                        std::fclose(file);
                    }
                if (FILE* file = indicesOnly ? nullptr : std::fopen((path + ".full.ppm").c_str(), "wb"))
                {
                    std::fprintf(file, "P6\n%d %d\n255\n", gWidth, gHeight);
                    for (const auto index : full) std::fwrite(&gPalette[index], 1, 3, file);
                    std::fclose(file);
                }
                // Repeat without advancing the state: ambiguous order must be
                // stable too. A sidecar is kept only when the repeat differs.
                std::remove((path + ".unstable.ppm").c_str());
                std::vector<uint8_t> repeat;
                if (!PaintFullCopy(repeat, gFrame, path)) std::abort();
                if (repeat != full)
                    if (FILE* file = std::fopen((path + ".unstable.ppm").c_str(), "wb"))
                    {
                        std::fprintf(file, "P6\n%d %d\n255\n", gWidth, gHeight);
                        for (const auto index : repeat) std::fwrite(&gPalette[index], 1, 3, file);
                        std::fclose(file);
                    }
                if (FILE* file = std::fopen((path + ".palette").c_str(), "wb"))
                {
                    for (const auto& colour : gPalette) std::fwrite(&colour, 1, 3, file);
                    std::fclose(file);
                }
            }
        }
    } // namespace

    std::string DisplayDiagnostics(bool) { return "{\"fallback\":false,\"verified\":false}"; }

    bool Open(int requestedWidth, int requestedHeight, int& width, int& height)
    {
        gWidth = 800; gHeight = 600;
        auto& settings = OpenRCT2::Config::Get().general;
        const bool chooseDefault = settings.fullscreenWidth <= 0 || settings.fullscreenHeight <= 0;
        for (const auto& mode : DisplayModes())
            if (mode.width == (chooseDefault ? 640 : settings.fullscreenWidth)
                && mode.height == (chooseDefault ? 480 : settings.fullscreenHeight))
            {
                gWidth = mode.width; gHeight = mode.height;
                if (chooseDefault) { settings.fullscreenWidth = 640; settings.fullscreenHeight = 480; }
                break;
            }
        if (const char* size = std::getenv("RCT_SIZE"); size != nullptr && *size != 0)
        {
            int w = 0, h = 0;
            if (std::sscanf(size, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0 || w > 4096 || h > 4096)
                return false;
            gWidth = w; gHeight = h;
        }
        (void)requestedWidth;
        (void)requestedHeight;
        width = gWidth;
        height = gHeight;
        gScreen.assign(static_cast<size_t>(gWidth) * gHeight, 0);
        if (const char* shots = std::getenv("RCT_SHOT_FRAMES"))
            for (const char* p = shots; *p != 0;)
            {
                gShots.push_back(std::strtol(p, const_cast<char**>(&p), 10));
                if (*p == ',')
                    p++;
            }
        if (const char* script = std::getenv("RCT_SCRIPT"))
            for (const char* p = script; *p != 0;)
            {
                Click click{};
                int consumed = 0;
                if (std::sscanf(p, "%ld:%d,%d,%d%n", &click.frame, &click.x, &click.y, &click.button, &consumed) < 4)
                    break;
                gClicks.push_back(click);
                p += consumed;
                if (*p == ';')
                    p++;
            }
        // One game tick per frame regardless of real time: runs become repeatable.
        // (RCT_FIXED_STEP=3: three ticks per frame, as on a machine that only manages 13 frames/s.)
        if (const char* step = std::getenv("RCT_FIXED_STEP"))
            OpenRCT2::gFixedFrameDelta = 0.02501f * static_cast<float>(std::max(1, std::atoi(step)));
        if (std::getenv("RCT_ROW_SHIFT") != nullptr)
            SetRowShiftOnly(true);
        if (std::getenv("RCT_NO_PICTURES") != nullptr)
            OpenRCT2::gPaintMapPictures = false;
        if (std::getenv("RCT_NO_CACHE") != nullptr)
            OpenRCT2::gPaintColumnCache = false;
        if (const char* remote = std::getenv("RCT_REMOTE"))
            if (FILE* file = std::fopen(remote, "r"))
            {
                char line[1100];
                while (std::fgets(line, sizeof(line), file) != nullptr)
                {
                    char* rest = nullptr;
                    const long frame = std::strtol(line, &rest, 10);
                    if (rest != line && std::strchr(rest, '{') != nullptr)
                        gRemote.push_back({ frame, std::strchr(rest, '{') });
                }
                std::fclose(file);
            }
        return true;
    }

    const std::vector<DisplayMode>& DisplayModes()
    {
        static const std::vector<DisplayMode> modes{ {640,480}, {800,600}, {1024,768} };
        return modes;
    }

    void Close()
    {
    }

    void SetPalette(const Colour colours[256])
    {
        std::memcpy(gPalette, colours, sizeof(gPalette));
    }

    uint64_t gPresentChangedPixels = 0, gPresentComparedPixels = 0;
    void PresentChanged(const uint8_t* bits, uint8_t* shown, int pitch, int left, int top, int right, int bottom)
    {
        PresentChangedRuns(bits, shown, pitch, left, top, right, bottom, [bits, pitch](int y, int x0, int x1) {
            std::memcpy(&gScreen[static_cast<size_t>(y) * gWidth + x0], bits + static_cast<size_t>(y) * pitch + x0,
                static_cast<size_t>(x1 - x0));
        });
        gPresentedPixels += static_cast<uint64_t>(right - left) * (bottom - top);
    }

    void Present(const uint8_t* bits, int pitch, int left, int top, int right, int bottom)
    {
        SC_SCOPE(present);
        OG_SCOPE(present);
        const unsigned ahead = gPresentPrefetchRows;
        for (int y = top; y < bottom; y++)
        {
            if (ahead != 0 && static_cast<unsigned>(bottom - y) > ahead)
                OpenRCT2::CachePrefetchRange(bits + static_cast<size_t>(y + ahead) * pitch + left, right - left);
            std::memcpy(&gScreen[static_cast<size_t>(y) * gWidth + left], bits + static_cast<size_t>(y) * pitch + left,
                static_cast<size_t>(right - left));
        }
        gPresentedPixels += static_cast<uint64_t>(right - left) * (bottom - top);
        static const bool trace = std::getenv("RCT_TRACE_RECTS") != nullptr; // which rectangles each frame repaints
        if (trace && gFrame >= 300 && gFrame < 306)
            std::fprintf(stderr, "frame %ld rect %d,%d %dx%d\n", gFrame, left, top, right - left, bottom - top);
    }

    void ShowWeather(const WeatherSpan* spans, size_t count, WeatherPixels& saved, const uint8_t* clean, int pitch)
    {
        OG_SCOPE_NUMBER(11);
        PrepareWeather(spans, count, saved);
        auto* next = saved.pixels.data();
        for (size_t i = 0; i < count; i++)
        {
            const auto& span = spans[i];
            const auto* under = clean ? clean + size_t(span.y) * pitch : gScreen.data() + size_t(span.y) * gWidth;
            next = WeatherRow8<false>(span, next, gScreen.data(), gWidth, under, 0, 0);
        }
        if (count != 0) saved.count = next - saved.pixels.data();
        (clean ? gWeatherTraffic.ramReadBytes : gWeatherTraffic.displayReadBytes) += saved.count;
        gWeatherTraffic.displayWriteBytes += saved.count;
    }

    bool WeatherNeedsCleanCanvas()
    {
        static const bool emulateVideoMemory = std::getenv("RCT_WEATHER_RAM") != nullptr;
        return emulateVideoMemory;
    }

    void RestoreWeather(const WeatherSpan*, size_t, WeatherPixels& saved)
    {
        OG_SCOPE_NUMBER(11);
        RestoreWeather8(saved, gScreen.data());
        gWeatherTraffic.displayWriteBytes += saved.count;
        saved.count = 0;
    }

    bool ReadPixels(uint8_t* bits, int pitch, int width, int height, int left, int top, int right, int bottom)
    {
        if (width != gWidth || height != gHeight)
            return false;
        left = std::clamp(left, 0, width); right = std::clamp(right, left, width);
        top = std::clamp(top, 0, height); bottom = std::clamp(bottom, top, height);
        gWeatherTraffic.readbackBytes += uint64_t(right - left) * (bottom - top);
        for (int y = top; y < bottom; y++)
            std::memcpy(bits + static_cast<size_t>(y) * pitch + left, gScreen.data() + static_cast<size_t>(y) * gWidth + left, right - left);
        return true;
    }

    bool CanScroll()
    {
        return std::getenv("RCT_NO_SCROLL") == nullptr;
    }

    bool Scroll(int x, int y, int width, int height, int dx, int dy)
    {
        SC_SCOPE(present);
        OG_SCOPE(present);
        // Same result as a video-memory block move.
        std::vector<uint8_t> block(static_cast<size_t>(width) * height);
        for (int row = 0; row < height; row++)
            std::memcpy(&block[static_cast<size_t>(row) * width], &gScreen[static_cast<size_t>(y - dy + row) * gWidth + (x - dx)],
                static_cast<size_t>(width));
        for (int row = 0; row < height; row++)
            std::memcpy(&gScreen[static_cast<size_t>(y + row) * gWidth + x], &block[static_cast<size_t>(row) * width],
                static_cast<size_t>(width));
        gScrolls++;
        return std::getenv("RCT_NO_SCROLL") == nullptr;
    }

    void EndFrame()
    {
#ifdef OPENRCT2_PAK_VFS
        // Progress presents are not game frames. Do not shift scripts/screenshots
        // or dispatch a load/quit while the loader holds a partially built park.
        if (ClassicLoadProgress::Active())
            return;
#endif
        // Frames are counted from the first one after start-up has finished: the start-up scene
        // runs its jobs on a thread and lasts a varying number of frames, which would shift every
        // scripted command and screenshot against the game's ticks (seen as runs one tick apart).
        auto* scenes = OpenRCT2::GetContext()->GetSceneManager();
        if (scenes != nullptr && scenes->getActiveScene() == scenes->getPreloaderScene())
            return;
        gFrame++;
        for (const auto shot : gShots)
            if (shot == gFrame)
                WriteShot();
        if (!gShots.empty() && gFrame >= gShots.back())
            OpenRCT2::GetContext()->Finish(); // leave without the save prompt
    }

    void PumpEvents()
    {
        // A command may pump Toolbox/SDL messages itself (notably load).
        // Keep the remaining scheduled commands outside that recursive pump.
        static bool pumping = false;
        if (pumping)
            return;
        pumping = true;
        struct RestorePump
        {
            ~RestorePump() { pumping = false; }
        } restorePump;
#ifdef OPENRCT2_PAK_VFS
        if (ClassicLoadProgress::Active())
            return;
#endif
        for (auto& remote : gRemote)
            if (remote.frame >= 0 && gFrame >= remote.frame)
            {
                remote.frame = -1;
                std::fprintf(stderr, "remote %s-> %s\n", remote.command.c_str(), RemoteCommand(remote.command).c_str());
            }
        for (auto& click : gClicks)
        {
            if (click.done || gFrame < click.frame)
                continue;
            click.done = true;
            SDL_Event move{};
            move.type = SDL_MOUSEMOTION;
            move.motion.x = click.x;
            move.motion.y = click.y;
            PushEvent(move);
            for (const auto type : { SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP })
            {
                SDL_Event button{};
                button.type = type;
                button.button.button = static_cast<Uint8>(click.button);
                button.button.x = click.x;
                button.button.y = click.y;
                PushEvent(button);
            }
            SetMouse(click.x, click.y, 0);
        }
    }

    uint64_t Microseconds()
    {
        static const auto start = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    }

    uint32_t Milliseconds()
    {
        static const auto start = std::chrono::steady_clock::now();
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    }

    void ShowCursor(bool)
    {
    }

    void SetPointerShape(const uint16_t*, const uint16_t*, int, int)
    {
    }

    std::string PrepareDirectPalette(bool, bool)
    {
        return "{\"enabled\":false}";
    }

    std::string BlitProbe(int, int, uint32_t)
    {
        return "{\"note\":\"only on the Mac\"}";
    }

    void MoveMemory(void* to, const void* from, size_t bytes)
    {
        std::memmove(to, from, bytes);
    }

    bool WarpPointer(int, int)
    {
        return false;
    }

    void Message(const char* text)
    {
        std::fprintf(stderr, "message: %s\n", text);
    }
} // namespace ShimHost
#endif
