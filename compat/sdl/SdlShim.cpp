#ifdef OPENRCT2_PAK_VFS
#include "ClassicDiskIo.h"
#endif
// SDL-named functions used by the game's UI library, implemented on ShimHost.
// Only what the UI calls is provided; there is no general SDL here.
#include "ShimHost.h"
#include <openrct2/core/FrameTiming.h>
#include <openrct2/entity/EntityInvalidation.h>

#include <SDL.h>
#include <openrct2/Context.h>
#include <openrct2/config/Config.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/interface/Window.h>
#include <openrct2/paint/Paint.h>
#include <openrct2/paint/Paint.Cache.h>
#include <openrct2/drawing/SpriteRasterCache.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>

struct SDL_Window
{
    int width = 0, height = 0;
    uint32_t flags = 0;
};
struct SDL_Cursor
{
    int unused = 0;
};

namespace
{
    SDL_Window gWindow;
    bool gWindowOpen = false;
    std::deque<SDL_Event> gEvents;
    int gMouseX = 0, gMouseY = 0;
    uint32_t gMouseButtons = 0;
    uint8_t gKeys[SDL_NUM_SCANCODES]{};
    bool gTextInput = false;
    SDL_Cursor gCursor;

    struct KeyName
    {
        SDL_Keycode key;
        SDL_Scancode scancode;
        const char* name;
    };
    const KeyName kKeys[] = {
        { SDLK_RETURN, SDL_SCANCODE_RETURN, "Return" },       { SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, "Escape" },
        { SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE, "Backspace" }, { SDLK_TAB, SDL_SCANCODE_TAB, "Tab" },
        { SDLK_SPACE, SDL_SCANCODE_SPACE, "Space" },          { SDLK_DELETE, SDL_SCANCODE_DELETE, "Delete" },
        { SDLK_UP, SDL_SCANCODE_UP, "Up" },                   { SDLK_DOWN, SDL_SCANCODE_DOWN, "Down" },
        { SDLK_LEFT, SDL_SCANCODE_LEFT, "Left" },             { SDLK_RIGHT, SDL_SCANCODE_RIGHT, "Right" },
        { SDLK_HOME, SDL_SCANCODE_HOME, "Home" },             { SDLK_END, SDL_SCANCODE_END, "End" },
        { SDLK_PAGEUP, SDL_SCANCODE_PAGEUP, "PageUp" },       { SDLK_PAGEDOWN, SDL_SCANCODE_PAGEDOWN, "PageDown" },
        { SDLK_INSERT, SDL_SCANCODE_INSERT, "Insert" },       { SDLK_LSHIFT, SDL_SCANCODE_LSHIFT, "Left Shift" },
        { SDLK_RSHIFT, SDL_SCANCODE_RSHIFT, "Right Shift" },  { SDLK_LCTRL, SDL_SCANCODE_LCTRL, "Left Ctrl" },
        { SDLK_RCTRL, SDL_SCANCODE_RCTRL, "Right Ctrl" },     { SDLK_LALT, SDL_SCANCODE_LALT, "Left Alt" },
        { SDLK_RALT, SDL_SCANCODE_RALT, "Right Alt" },        { SDLK_LGUI, SDL_SCANCODE_LGUI, "Left GUI" },
        { SDLK_RGUI, SDL_SCANCODE_RGUI, "Right GUI" },        { SDLK_MINUS, SDL_SCANCODE_MINUS, "-" },
        { SDLK_EQUALS, SDL_SCANCODE_EQUALS, "=" },            { SDLK_LEFTBRACKET, SDL_SCANCODE_LEFTBRACKET, "[" },
        { SDLK_RIGHTBRACKET, SDL_SCANCODE_RIGHTBRACKET, "]" }, { SDLK_BACKSLASH, SDL_SCANCODE_BACKSLASH, "\\" },
        { SDLK_SEMICOLON, SDL_SCANCODE_SEMICOLON, ";" },      { SDLK_QUOTE, SDL_SCANCODE_APOSTROPHE, "'" },
        { SDLK_BACKQUOTE, SDL_SCANCODE_GRAVE, "`" },          { SDLK_COMMA, SDL_SCANCODE_COMMA, "," },
        { SDLK_PERIOD, SDL_SCANCODE_PERIOD, "." },            { SDLK_SLASH, SDL_SCANCODE_SLASH, "/" },
        { SDLK_CAPSLOCK, SDL_SCANCODE_CAPSLOCK, "CapsLock" }, { SDLK_PAUSE, SDL_SCANCODE_PAUSE, "Pause" },
    };
    std::string gKeyNameBuffer;
} // namespace

namespace ShimHost
{
    // The game drags the view by reading how far the pointer moved and then putting it back
    // where the drag started. Where the host cannot move the real pointer, the shim keeps the
    // distance between the real and the reported position instead (until the pointer is shown again).
    static int gWarpX = 0, gWarpY = 0;
    void PushEvent(const SDL_Event& event)
    {
        if (gEvents.size() >= 512)
            return;
        gEvents.push_back(event);
        auto& stored = gEvents.back();
        if (stored.type == SDL_MOUSEMOTION)
        {
            stored.motion.x -= gWarpX;
            stored.motion.y -= gWarpY;
        }
        else if (stored.type == SDL_MOUSEBUTTONDOWN || stored.type == SDL_MOUSEBUTTONUP)
        {
            stored.button.x -= gWarpX;
            stored.button.y -= gWarpY;
        }
    }
    static bool gRemoteMouse = false;
    static int gHostMouseX = -1, gHostMouseY = -1;
    void RemoteMouse(int x, int y, uint32_t buttons)
    {
        gRemoteMouse = true;
        gMouseX = x - gWarpX;
        gMouseY = y - gWarpY;
        gMouseButtons = buttons;
    }
    void SetMouse(int x, int y, uint32_t buttons)
    {
        const bool moved = x != gHostMouseX || y != gHostMouseY;
        gHostMouseX = x;
        gHostMouseY = y;
        if (gRemoteMouse && !moved && buttons == 0)
            return;
        if (gRemoteMouse)
            gWarpX = gWarpY = 0;
        gRemoteMouse = false;
        gMouseX = x - gWarpX;
        gMouseY = y - gWarpY;
        gMouseButtons = buttons;
    }
    void WarpTo(int x, int y)
    {
        if (!gRemoteMouse && WarpPointer(x, y))
        {
            gWarpX = gWarpY = 0;
            gHostMouseX = x;
            gHostMouseY = y;
        }
        else
        {
            gWarpX += gMouseX - x;
            gWarpY += gMouseY - y;
        }
        gMouseX = x;
        gMouseY = y;
    }
    void PointerShown()
    {
        gMouseX += gWarpX;
        gMouseY += gWarpY;
        gWarpX = gWarpY = 0;
    }
    void SetKey(int scancode, bool down)
    {
        if (scancode >= 0 && scancode < SDL_NUM_SCANCODES)
            gKeys[scancode] = down ? 1 : 0;
    }
    bool TextInputActive()
    {
        return gTextInput;
    }
    uint64_t TimeBase()
    {
#if defined(__powerpc__) || defined(__POWERPC__) || defined(__ppc__)
        // 64-bit time base: read upper, lower, upper again until consistent.
        uint32_t upper, lower, check;
        do
        {
            __asm__ volatile("mftbu %0" : "=r"(upper));
            __asm__ volatile("mftb %0" : "=r"(lower));
            __asm__ volatile("mftbu %0" : "=r"(check));
        } while (upper != check);
        return (static_cast<uint64_t>(upper) << 32) | lower;
#elif defined(__x86_64__) || defined(__i386__)
        return __builtin_ia32_rdtsc();
#else
        return Microseconds();
#endif
    }
    static bool gFastPaintClock = true;
    static uint64_t gPaintClockAnchor = 0;
    static uint32_t gPaintTickAnchor = 0, gPaintTickReciprocal = 0;
    static uint64_t gPaintCalibrationMicros = 0, gPaintCalibrationTicks = 0;
    void SetFastPaintClock(bool enabled) { gFastPaintClock = enabled; }
    uint64_t PaintMicroseconds()
    {
#ifdef OPENRCT2_CLASSIC_MAC
        uint32_t ticks;
        __asm__ volatile("mftb %0" : "=r"(ticks));
        return gPaintClockAnchor + ((uint64_t(ticks - gPaintTickAnchor) * gPaintTickReciprocal) >> 32);
#else
        return Microseconds();
#endif
    }
    static void AnchorPaintClock()
    {
#ifdef OPENRCT2_CLASSIC_MAC
        const auto micros = Microseconds(), ticks = TimeBase();
        if (gPaintCalibrationMicros == 0)
        {
            gPaintCalibrationMicros = micros;
            gPaintCalibrationTicks = ticks;
            gPaintTickReciprocal = uint32_t((uint64_t{ 1 } << 32) / 25);
        }
        else if (micros - gPaintCalibrationMicros >= 1000000)
        {
            gPaintTickReciprocal = uint32_t(((micros - gPaintCalibrationMicros) << 32) / (ticks - gPaintCalibrationTicks));
            gPaintCalibrationMicros = micros;
            gPaintCalibrationTicks = ticks;
        }
        gPaintClockAnchor = micros;
        gPaintTickAnchor = static_cast<uint32_t>(ticks);
#endif
    }
    bool gTracePump = false;
    PumpProfile gPumpProfile{};
    uint64_t gAudioPumpMicros = 0;
    WeatherTraffic gWeatherTraffic{};
    static ProfileTotals gTotals{};
    ProfileTotals Profile()
    {
        return gTotals;
    }
    void CountShift(uint64_t microseconds)
    {
        gTotals.shiftMicros += microseconds;
    }
    void CountPalette(uint64_t microseconds)
    {
        gTotals.paletteSets++;
        gTotals.paletteMicros += microseconds;
    }
    void CountRepaint(int width, int height)
    {
        gTotals.rects++;
        gTotals.rectPixels += static_cast<uint64_t>(width) * height;
    }
    void AddPhaseTime(Phase phase, uint64_t microseconds)
    {
        gTotals.phase[static_cast<int>(phase)] += microseconds;
    }
    void SampleFrameTiming(OpenRCT2::FrameTiming::Frame&);
    void FrameDone(uint32_t gameTicks)
    {
        static const uint64_t start = Microseconds();
        static const uint32_t firstTick = gameTicks;
        gTotals.frames++;
        AnchorPaintClock();
        OpenRCT2::gPaintClock = gFastPaintClock ? PaintMicroseconds : Microseconds;
        for (int i = 0; i < 4; i++)
            gTotals.world[i] = OpenRCT2::gPaintPhaseMicros[i];
        for (int i = 0; i < 4; i++)
            gTotals.loop[i] = OpenRCT2::gLoopPhaseMicros[i];
        gTotals.widgetMicros = OpenRCT2::gWindowWidgetMicros;
        gTotals.widgetDraws = OpenRCT2::gWindowWidgetDraws;
        gTotals.columns = OpenRCT2::gPaintColumns;
        gTotals.builds = OpenRCT2::gPaintColumnBuilds;
        gTotals.patches = OpenRCT2::gPaintColumnPatches;
        gTotals.shifts = OpenRCT2::gPaintColumnShifts;
        gTotals.shiftTiles = OpenRCT2::gPaintColumnShiftTiles;
        gTotals.shiftChecks = OpenRCT2::gPaintColumnShiftChecks;
        gTotals.entityEntries = gPaintDrawnEntityEntries;
        gTotals.mapEntries = gPaintDrawnMapEntries;
        gTotals.nodesCollected = gPaintNodesCollected;
        gTotals.nodesSorted = gPaintNodesSorted;
        const auto& raster = OpenRCT2::Drawing::gSpriteRasterCacheStats;
        const uint64_t costs[] = { raster.baseMicros, raster.patchMicros, raster.ownerMicros, raster.checkMicros,
            raster.localFilterMicros, raster.localSortMicros, raster.tilePatchMicros, raster.entityGenerateMicros };
        const uint64_t work[] = { raster.patchHits, raster.patchMisses, raster.selectionHits, raster.selectionMisses,
            raster.ownerPasses, raster.ownerFallbacks, raster.ownerRestores, raster.basePixels,
            raster.pictureBuilds, raster.pictureRepairs, raster.pictureDeferrals, raster.pictureRepairDeferrals,
            raster.pictureFallbackPixels };
        std::copy_n(costs, 8, gTotals.paintCosts);
        std::copy_n(work, 13, gTotals.paintWork);
        gTotals.damageMicros = gDamageMicros;
        gTotals.guestPoseMicros = OpenRCT2::gPaintDisplayedGuestMicros;
        gTotals.guestPoseVisits = OpenRCT2::gPaintDisplayedGuestVisits;
        gTotals.entityFlushMicros = OpenRCT2::gPaintEntityFlushMicros;
        gTotals.screenScrollMicros = gScreenScrollMicros + gScreenScrollWaitMicros;
        gTotals.scrollRects = OpenRCT2::gPaintScrollRects;
        gTotals.scrollPixels = OpenRCT2::gPaintScrollPixels;
        std::copy_n(OpenRCT2::gPaintScrollSourcePixels.data(), 3, gTotals.scrollSourcePixels);
        gTotals.scrollGeometricPixels = OpenRCT2::gPaintScrollGeometricPixels;
        gTotals.entityDamagePixels = OpenRCT2::gPaintEntityDamagePixels;
        gTotals.pointerPixels = gPointerPixels;
        gTotals.columnBuildMicros = OpenRCT2::gPaintColumnBuildMicros;
        gTotals.columnShiftMicros = OpenRCT2::gPaintColumnShiftMicros;
        gTotals.pump = gPumpProfile;
        gTotals.audioPumpMicros = gAudioPumpMicros;
        gTotals.weather = gWeatherTraffic;
        gTotals.microseconds = Microseconds() - start;
        static uint64_t previousFrame = 0;
        static const bool allowAutomatic = std::getenv("RCT_FIXED_STEP") == nullptr;
        OpenRCT2::ViewportsUpdateFarEntities(
            static_cast<uint32_t>(std::min<uint64_t>(gTotals.microseconds - previousFrame, UINT32_MAX)), allowAutomatic,
            OpenRCT2::FrameTiming::current.tickCalls != 0);
        previousFrame = gTotals.microseconds;
        gTotals.ticks = gameTicks - firstTick;
        OpenRCT2::FrameTiming::sample = SampleFrameTiming;
        OpenRCT2::FrameTiming::presented = true;
        RemoteFrame();
    }
} // namespace ShimHost

extern "C" {
int SDL_Init(Uint32)
{
    return 0;
}
void SDL_QuitSubSystem(Uint32)
{
    if (gWindowOpen)
        ShimHost::Close();
    gWindowOpen = false;
}
const char* SDL_GetError(void)
{
    return "";
}
SDL_bool SDL_SetHint(const char*, const char*)
{
    return SDL_TRUE;
}
void SDL_GetVersion(SDL_version* version)
{
    version->major = 2;
    version->minor = 30;
    version->patch = 9;
}
void SDL_free(void* memory)
{
    std::free(memory);
}
Uint32 SDL_GetTicks(void)
{
    return ShimHost::Milliseconds();
}

// --- window ------------------------------------------------------------------
SDL_Window* SDL_CreateWindow(const char*, int, int, int width, int height, Uint32 flags)
{
    if (!gWindowOpen && !ShimHost::Open(width, height, gWindow.width, gWindow.height))
        return nullptr;
    if (!OpenRCT2::FrameTiming::clock)
    {
        OpenRCT2::FrameTiming::clock = ShimHost::Microseconds;
        OpenRCT2::FrameTiming::Reset();
    }
    gWindowOpen = true;
    gWindow.flags = (flags & ~(SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) | SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS
        | SDL_WINDOW_MOUSE_FOCUS;
    return &gWindow;
}
void SDL_DestroyWindow(SDL_Window*)
{
    if (gWindowOpen)
        ShimHost::Close();
    gWindowOpen = false;
}
void SDL_GetWindowSize(SDL_Window*, int* width, int* height)
{
    if (width != nullptr)
        *width = gWindow.width;
    if (height != nullptr)
        *height = gWindow.height;
}
void SDL_SetWindowSize(SDL_Window*, int width, int height)
{
#ifdef OPENRCT2_CLASSIC_MAC
    if (gWindowOpen && ShimHost::ResizeDisplay(width,height))
    {
        gWindow.width = width; gWindow.height = height;
        SDL_Event event{}; event.type = SDL_WINDOWEVENT;
        event.window.event = SDL_WINDOWEVENT_RESIZED;
        event.window.data1 = width; event.window.data2 = height;
        ShimHost::PushEvent(event);
    }
#endif
}
void SDL_SetWindowMinimumSize(SDL_Window*, int, int)
{
}
Uint32 SDL_GetWindowFlags(SDL_Window*)
{
    return gWindow.flags;
}
void SDL_GetWindowPosition(SDL_Window*, int* x, int* y)
{
    if (x != nullptr)
        *x = 0;
    if (y != nullptr)
        *y = 0;
}
int SDL_SetWindowFullscreen(SDL_Window*, Uint32 flags)
{
#ifdef OPENRCT2_CLASSIC_MAC
    // Classic keeps its native fullscreen window for all presentation choices.
    // Report the requested SDL state so fullscreen resizes do not overwrite
    // the separately saved windowed dimensions.
    gWindow.flags = (gWindow.flags & ~SDL_WINDOW_FULLSCREEN_DESKTOP) | (flags & SDL_WINDOW_FULLSCREEN_DESKTOP);
#else
    (void)flags;
#endif
    return 0;
}
void SDL_SetWindowGrab(SDL_Window*, SDL_bool)
{
}
int SDL_GetWindowDisplayIndex(SDL_Window*)
{
    return 0;
}
int SDL_FlashWindow(SDL_Window*, SDL_FlashOperation)
{
    return 0;
}
SDL_Renderer* SDL_GetRenderer(SDL_Window*)
{
    return nullptr;
}
int SDL_GetRendererOutputSize(SDL_Renderer*, int*, int*)
{
    return -1;
}
int SDL_GetNumDisplayModes(int)
{
    return ShimHost::DisplayModes().size();
}
int SDL_GetDesktopDisplayMode(int, SDL_DisplayMode* mode)
{
    std::memset(mode, 0, sizeof(*mode));
    mode->w = gWindow.width ? gWindow.width : 800;
    mode->h = gWindow.height ? gWindow.height : 600;
    mode->refresh_rate = 60;
    return 0;
}
int SDL_GetDisplayMode(int, int index, SDL_DisplayMode* mode)
{
    const auto& modes = ShimHost::DisplayModes();
    if (index < 0 || index >= static_cast<int>(modes.size())) return -1;
    std::memset(mode, 0, sizeof(*mode));
    mode->w = modes[index].width; mode->h = modes[index].height;
    mode->refresh_rate = 60;
    return 0;
}
int SDL_ShowMessageBox(const SDL_MessageBoxData* data, int* button)
{
    if (data != nullptr && data->message != nullptr)
        ShimHost::Message(data->message);
    if (button != nullptr)
        *button = 0;
    return 0;
}
void SDL_EnableScreenSaver(void)
{
}
void SDL_DisableScreenSaver(void)
{
}

// --- cursor --------------------------------------------------------------------
// The game's pointers are 32x32; a classic Mac pointer is 16x16. The 16x16 window that holds the
// hot spot and as much of the shape as possible is kept (the arrow and most tools fit).
namespace
{
    const void* gSelectedCursor = nullptr;
    bool gCursorShown = true;
    struct ShimCursor
    {
        uint16_t data[16], mask[16];
        int hotX, hotY;
    };
} // namespace
SDL_Cursor* SDL_CreateCursor(const Uint8* data, const Uint8* mask, int w, int h, int hotX, int hotY)
{
    auto* cursor = new ShimCursor{};
    const int pitch = (w + 7) / 8;
    const auto bit = [pitch](const Uint8* bits, int x, int y) { return (bits[y * pitch + x / 8] >> (7 - (x & 7))) & 1; };
    int left = w, top = h, right = 0, bottom = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (bit(mask, x, y))
            {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x + 1);
                bottom = std::max(bottom, y + 1);
            }
    if (right <= left)
        left = top = 0;
    if (right - left <= 16 && bottom - top <= 16)
    {
        // Fits: start at the shape's corner, but never so that the hot spot falls outside.
        const int originX = std::clamp(std::min(left, hotX), std::max(0, hotX - 15), std::max(0, hotX));
        const int originY = std::clamp(std::min(top, hotY), std::max(0, hotY - 15), std::max(0, hotY));
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
            {
                const int sx = originX + x, sy = originY + y;
                if (sx >= w || sy >= h || !bit(mask, sx, sy))
                    continue;
                cursor->mask[y] |= static_cast<uint16_t>(0x8000u >> x);
                if (bit(data, sx, sy))
                    cursor->data[y] |= static_cast<uint16_t>(0x8000u >> x);
            }
        cursor->hotX = hotX - originX;
        cursor->hotY = hotY - originY;
    }
    else
    {
        // Too large (most of the tool pointers are 30 pixels or so): the whole shape at half
        // size rather than a 16-pixel corner of it. A small pixel is opaque if any of its four
        // is, and black if at least two of them are (outlines survive, fills stay white).
        const int step = std::max((w + 15) / 16, (h + 15) / 16);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
            {
                int opaque = 0, black = 0;
                for (int dy = 0; dy < step; dy++)
                    for (int dx = 0; dx < step; dx++)
                    {
                        const int sx = x * step + dx, sy = y * step + dy;
                        if (sx < w && sy < h && bit(mask, sx, sy))
                        {
                            opaque++;
                            black += bit(data, sx, sy);
                        }
                    }
                if (opaque == 0)
                    continue;
                cursor->mask[y] |= static_cast<uint16_t>(0x8000u >> x);
                if (black * 2 >= step * step || black == opaque)
                    cursor->data[y] |= static_cast<uint16_t>(0x8000u >> x);
            }
        cursor->hotX = std::min(15, hotX / step);
        cursor->hotY = std::min(15, hotY / step);
    }
    return reinterpret_cast<SDL_Cursor*>(cursor);
}
SDL_Cursor* SDL_CreateSystemCursor(SDL_SystemCursor id)
{
    // Our own arrow and pointing hand (white with a black outline), not the system's.
    static const ShimCursor arrow{ { 0x8000, 0xC000, 0xA000, 0x9000, 0x8800, 0x8400, 0x8200, 0x8100, 0x8080, 0x8040, 0x83E0, 0x9200, 0xA900, 0xC900, 0x8480, 0x0700 },
                                   { 0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00, 0xFF80, 0xFFC0, 0xFFE0, 0xFE00, 0xEF00, 0xCF00, 0x8780, 0x0700 }, 0, 0 };
    static const ShimCursor hand{ { 0x0600, 0x0900, 0x0900, 0x0900, 0x09C0, 0x0938, 0x6924, 0x9926, 0x8805, 0x4801, 0x2801, 0x2001, 0x1001, 0x1002, 0x0802, 0x0FFE },
                                  { 0x0600, 0x0F00, 0x0F00, 0x0F00, 0x0FC0, 0x0FF8, 0x6FFC, 0xFFFE, 0xFFFF, 0x7FFF, 0x3FFF, 0x3FFF, 0x1FFF, 0x1FFE, 0x0FFE, 0x0FFE }, 5, 0 };
    return reinterpret_cast<SDL_Cursor*>(const_cast<ShimCursor*>(id == SDL_SYSTEM_CURSOR_HAND ? &hand : &arrow));
}
void SDL_FreeCursor(SDL_Cursor* cursor)
{
    // (The built-in arrow and hand are never freed by the game's cursor table in practice; a
    // pointer from SDL_CreateCursor is ours to delete.)
    (void)cursor;
}
void SDL_SetCursor(SDL_Cursor* cursor)
{
    if (cursor == nullptr || cursor == &gCursor)
        return;
    const auto* shape = reinterpret_cast<const ShimCursor*>(cursor);
    static float selectedScale = 0;
    const float currentScale = OpenRCT2::Config::Get().general.windowScale;
    if (gSelectedCursor == shape && selectedScale == currentScale) return;
    selectedScale = currentScale;
    gSelectedCursor = shape;
    const float scale = OpenRCT2::Config::Get().general.windowScale;
    uint16_t data[16]{}, mask[16]{};
    for (int y = 0; y < 16 && y < 16 * scale; y++)
        for (int x = 0; x < 16 && x < 16 * scale; x++)
        {
            const int sx = x / scale, sy = y / scale;
            if (shape->mask[sy] & (0x8000u >> sx)) mask[y] |= 0x8000u >> x;
            if (shape->data[sy] & (0x8000u >> sx)) data[y] |= 0x8000u >> x;
        }
    ShimHost::SetPointerShape(data, mask, static_cast<int>(std::ceil(shape->hotX * scale)),
                              static_cast<int>(std::ceil(shape->hotY * scale)));
    ShimHost::ShowCursor(gCursorShown && OpenRCT2::Config::Get().general.windowScale <= 1.0f);
}
int SDL_ShowCursor(int toggle)
{
    if (toggle == SDL_ENABLE || toggle == SDL_DISABLE)
    {
        gCursorShown = toggle == SDL_ENABLE;
        ShimHost::ShowCursor(gCursorShown && OpenRCT2::Config::Get().general.windowScale <= 1.0f);
    }
    if (toggle == SDL_ENABLE)
        ShimHost::PointerShown();
    return SDL_ENABLE;
}
void SDL_WarpMouseInWindow(SDL_Window*, int x, int y)
{
    ShimHost::WarpTo(x, y);
}

// --- input ---------------------------------------------------------------------
int SDL_PollEvent(SDL_Event* event)
{
    if (gEvents.empty())
    {
        ShimHost::PhaseTimer timer{ ShimHost::Phase::pump };
        ShimHost::PumpEvents();
    }
    if (gEvents.empty())
        return 0;
    if (event != nullptr)
    {
        *event = gEvents.front();
        gEvents.pop_front();
    }
    return 1;
}
Uint32 SDL_GetMouseState(int* x, int* y)
{
    if (x != nullptr)
        *x = gMouseX;
    if (y != nullptr)
        *y = gMouseY;
    return gMouseButtons;
}
const Uint8* SDL_GetKeyboardState(int* count)
{
    if (count != nullptr)
        *count = SDL_NUM_SCANCODES;
    return gKeys;
}
SDL_Keymod SDL_GetModState(void)
{
    int mod = KMOD_NONE;
    if (gKeys[SDL_SCANCODE_LSHIFT] || gKeys[SDL_SCANCODE_RSHIFT])
        mod |= KMOD_LSHIFT;
    if (gKeys[SDL_SCANCODE_LCTRL] || gKeys[SDL_SCANCODE_RCTRL])
        mod |= KMOD_LCTRL;
    if (gKeys[SDL_SCANCODE_LALT] || gKeys[SDL_SCANCODE_RALT])
        mod |= KMOD_LALT;
    if (gKeys[SDL_SCANCODE_LGUI] || gKeys[SDL_SCANCODE_RGUI])
        mod |= KMOD_LGUI;
    return static_cast<SDL_Keymod>(mod);
}
SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode)
{
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
        return SDLK_a + (scancode - SDL_SCANCODE_A);
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
        return SDLK_1 + (scancode - SDL_SCANCODE_1);
    if (scancode == SDL_SCANCODE_0)
        return SDLK_0;
    for (const auto& key : kKeys)
        if (key.scancode == scancode)
            return key.key;
    return SDL_SCANCODE_TO_KEYCODE(scancode);
}
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key)
{
    if (key >= SDLK_a && key <= SDLK_z)
        return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (key - SDLK_a));
    if (key >= SDLK_1 && key <= SDLK_9)
        return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (key - SDLK_1));
    if (key == SDLK_0)
        return SDL_SCANCODE_0;
    for (const auto& entry : kKeys)
        if (entry.key == key)
            return entry.scancode;
    if (key & SDLK_SCANCODE_MASK)
        return static_cast<SDL_Scancode>(key & ~SDLK_SCANCODE_MASK);
    return SDL_SCANCODE_UNKNOWN;
}
const char* SDL_GetKeyName(SDL_Keycode key)
{
    for (const auto& entry : kKeys)
        if (entry.key == key)
            return entry.name;
    if (key >= SDLK_F1 && key <= SDLK_F12)
    {
        gKeyNameBuffer = "F" + std::to_string(key - SDLK_F1 + 1);
        return gKeyNameBuffer.c_str();
    }
    if (key >= 0x20 && key < 0x7F)
    {
        gKeyNameBuffer.assign(1, static_cast<char>(key >= 'a' && key <= 'z' ? key - 32 : key));
        return gKeyNameBuffer.c_str();
    }
    return "";
}
SDL_Keycode SDL_GetKeyFromName(const char* name)
{
    if (name == nullptr || name[0] == 0)
        return SDLK_UNKNOWN;
    // Any letter case, as in SDL: the game's default shortcuts say "UP", "ESCAPE", ...
    const auto same = [](const char* a, const char* b) {
        for (; *a != 0 && *b != 0; a++, b++)
            if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
                return false;
        return *a == *b;
    };
    for (const auto& entry : kKeys)
        if (same(entry.name, name))
            return entry.key;
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9')
    {
        const int number = std::atoi(name + 1);
        if (number >= 1 && number <= 12)
            return SDLK_F1 + (number - 1);
    }
    if (name[1] == 0)
    {
        const char c = name[0];
        return c >= 'A' && c <= 'Z' ? c + 32 : c;
    }
    return SDLK_UNKNOWN;
}
void SDL_StartTextInput(void)
{
    gTextInput = true;
}
void SDL_StopTextInput(void)
{
    gTextInput = false;
}
SDL_bool SDL_IsTextInputActive(void)
{
    return gTextInput ? SDL_TRUE : SDL_FALSE;
}
void SDL_SetTextInputRect(const SDL_Rect*)
{
}

// --- clipboard and controllers: none --------------------------------------------
int SDL_SetClipboardText(const char*)
{
    return -1;
}
char* SDL_GetClipboardText(void)
{
    char* empty = static_cast<char*>(std::malloc(1));
    empty[0] = 0;
    return empty;
}
SDL_bool SDL_HasClipboardText(void)
{
    return SDL_FALSE;
}
int SDL_NumJoysticks(void)
{
    return 0;
}
SDL_bool SDL_IsGameController(int)
{
    return SDL_FALSE;
}
SDL_GameController* SDL_GameControllerOpen(int)
{
    return nullptr;
}
SDL_Joystick* SDL_GameControllerGetJoystick(SDL_GameController*)
{
    return nullptr;
}
Sint16 SDL_GameControllerGetAxis(SDL_GameController*, SDL_GameControllerAxis)
{
    return 0;
}
int SDL_JoystickNumHats(SDL_Joystick*)
{
    return 0;
}
Uint8 SDL_JoystickGetHat(SDL_Joystick*, int)
{
    return 0;
}
Uint8 SDL_JoystickGetButton(SDL_Joystick*, int)
{
    return 0;
}
}

namespace ShimHost
{
    bool SoftwarePointer(uint16_t data[16], uint16_t mask[16], int& hotX, int& hotY)
    {
        if (!gCursorShown || gSelectedCursor == nullptr || OpenRCT2::Config::Get().general.windowScale <= 1.0f)
            return false;
        const auto* shape = static_cast<const ShimCursor*>(gSelectedCursor);
        std::memcpy(data, shape->data, sizeof(shape->data));
        std::memcpy(mask, shape->mask, sizeof(shape->mask));
        hotX = shape->hotX;
        hotY = shape->hotY;
        return true;
    }
}
