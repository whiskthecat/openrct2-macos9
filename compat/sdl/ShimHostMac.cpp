#include "PixelCopy.h"
#include "WeatherRaster.h"
// Mac OS 9 display and input for the game.
//
// The screen runs in 256 colours with the game's palette in the hardware
// colour table, so the engine's indexed pixels are copied to video memory
// unchanged. Viewport scrolling is a screen-to-screen CopyBits, which the
// ATI driver performs with its 2D engine.
#ifdef OPENRCT2_CLASSIC_MAC
    #include "ShimHost.h"
#include <openrct2/core/CachePrefetch.h>
    #include <openrct2/core/FrameTiming.h>
    #include "IndexedDisplay.h"
    #include "PageDamage.h"

    #include "ClassicVfs.h"
    #include "ClassicLoadProgress.h"
    #include "ClassicAudio.h"

    #include <AppleEvents.h>
    #include <Dialogs.h>
    #include <Events.h>
    #include <Fonts.h>
    #include <LowMem.h>
    #include <NameRegistry.h>
    #include <Script.h>
    #include <TextUtils.h>
    #include <Menus.h>
    #include <TextEdit.h>
    #include <Video.h>
    #include <Devices.h>
    #include <Retrace.h>
    #include <Displays.h>
    #include <Gestalt.h>
    #include <cstdlib>
    #include <MacWindows.h>
    #include <Movies.h>
    #include <Palettes.h>
    #include <QDOffscreen.h>
    #include <Quickdraw.h>
    #include <SDL.h>
    #include <openrct2/interface/Screenshot.h>
    #include <openrct2/drawing/Drawing.Screen.h>
    #include <openrct2/drawing/Drawing.Sprite.h>
    #include <openrct2/drawing/Drawing.h>
    #include <openrct2/drawing/PaletteIndex.h>
    #include <openrct2/drawing/RenderTarget.h>
    #include <openrct2/entity/Guest.h>
    #include <openrct2/entity/EntityList.h>
    #include <openrct2/paint/entity/Paint.Peep.h>
    #include <openrct2/ride/Vehicle.h>
    #include <openrct2/ride/CarEntry.h>
    #include <openrct2/Context.h>
    #include <openrct2/config/Config.h>
    #include <cstdio>
    #include <algorithm>
    #include <cstring>
    #include <nlohmann/json.hpp>
    #include "NativeEventTrace.h"

namespace ShimHost
{
    namespace
    {
        bool IsClassicEnvironment()
        {
            long attributes = 0;
            return Gestalt(gestaltMacOSCompatibilityBoxAttr, &attributes) == noErr
                && (attributes & (1L << gestaltMacOSCompatibilityBoxPresent)) != 0;
        }
        namespace NativeVSync { void Written(int,int,int,int); bool Stop(); void End(); bool Drain(); void ResetMode(); CGrafPtr Port(unsigned); bool Scroll(const Rect&, const Rect&); void CursorWatch(bool,bool,bool); std::array<uint32_t,5> CursorWatchStats(); void CursorOperation(bool,bool); extern bool active, availabilityKnown, pendingPalette, flushingPalette; extern unsigned back; extern GWorldPtr world; }
        GDHandle gDevice = nullptr;
        short gOriginalDepth = 0;
        ColorSpec gOriginalClut[256]{};
        bool gHaveOriginalClut = false;
        OSErr ReadClut(ColorSpec* colours)
        {
            for (int i = 0; i < 256; i++) colours[i].value = i;
            VDSetEntryRecord entries{ colours, 0, 255 };
            CntrlParam block{};
            block.ioCRefNum = (**gDevice).gdRefNum;
            block.csCode = cscGetEntries;
            *reinterpret_cast<Ptr*>(&block.csParam[0]) = reinterpret_cast<Ptr>(&entries);
            return PBStatusSync(reinterpret_cast<ParmBlkPtr>(&block));
        }
        bool RestoreClut()
        {
            RestoreDeviceClut(gDevice); // this Toolbox API has no return value
            const QDErr quickdraw = QDError();
            ColorSpec current[256]{};
            OSErr read = ReadClut(current);
            auto matches = [&] {
                for (int i = 0; i < 256; i++)
                    if (std::memcmp(&current[i].rgb, &gOriginalClut[i].rgb, sizeof(RGBColor)) != 0) return false;
                return true;
            };
            OSErr set = noErr;
            if (read == noErr && gHaveOriginalClut && !matches())
            {
                VDSetEntryRecord entries{ gOriginalClut, 0, 255 };
                CntrlParam block{};
                block.ioCRefNum = (**gDevice).gdRefNum;
                block.csCode = cscSetEntries;
                *reinterpret_cast<Ptr*>(&block.csParam[0]) = reinterpret_cast<Ptr>(&entries);
                set = PBControlSync(reinterpret_cast<ParmBlkPtr>(&block));
                read = ReadClut(current);
            }
            const bool okay = quickdraw == noErr && set == noErr && read == noErr && gHaveOriginalClut && matches();
            std::printf("CLUT restore: QD %d set %d read %d verified %d\n", quickdraw, set, read, okay);
            return okay;
        }
        std::vector<DisplayMode> gModes;
        VDSwitchInfoRec gOriginalMode{};
        bool gHaveOriginalMode = false, gModeChanged = false;
        Handle gDisplayState = nullptr;
        DisplayMode gGameMode{};
        Rect gOriginalBounds{};

        pascal void CollectMode(void*, DMListIndexType, DMDisplayModeListEntryPtr entry)
        {
            if (entry == nullptr || entry->displayModeDepthBlockInfo == nullptr) return;
            const auto& block = *entry->displayModeDepthBlockInfo;
            if (block.depthVPBlock == nullptr || block.depthBlockCount > 64) return;
            for (unsigned long i = 0; i < block.depthBlockCount; i++)
            {
                const auto& depth = block.depthVPBlock[i];
                if (depth.depthVPBlock == nullptr || depth.depthSwitchInfo == nullptr) continue;
                const auto& pixels = *depth.depthVPBlock;
                const auto& mode = *depth.depthSwitchInfo;
                if (pixels.vpPixelSize != 8 || mode.csData == 0 || mode.csMode == 0 || mode.csPage != 0) continue;
                gModes.push_back({ pixels.vpBounds.right - pixels.vpBounds.left,
                                  pixels.vpBounds.bottom - pixels.vpBounds.top, mode.csData, mode.csMode });
            }
        }

        void EnumerateModes()
        {
            gModes.clear();
            long version = 0;
            if (Gestalt(gestaltDisplayMgrVers, &version) != noErr || version < 0x00020000) return;
            DisplayIDType id = 0;
            OSErr error = DMGetDisplayIDByGDevice(gDevice, &id, false);
            DMListType list = nullptr;
            DMListIndexType count = 0;
            if (error == noErr) error = DMNewDisplayModeList(id, kDMModeListPreferSafeModesMask, 0, &count, &list);
            const auto callback = NewDMDisplayModeListIteratorUPP(CollectMode);
            if (error == noErr && list != nullptr && count <= 4096 && callback != nullptr)
                for (DMListIndexType i = 0; i < count && error == noErr; i++)
                    error = DMGetIndexedDisplayModeFromList(list, i, 0, callback, nullptr);
            if (callback != nullptr) DisposeDMDisplayModeListIteratorUPP(callback);
            if (list != nullptr) DMDisposeList(list);
            if (error != noErr) { gModes.clear(); std::printf("display mode enumeration: %d\n", error); return; }
            gModes.erase(std::remove_if(gModes.begin(), gModes.end(), [](const auto& mode) {
                unsigned long flags = 0;
                Boolean usable = false;
                return mode.width <= 0 || mode.height <= 0 || DMCheckDisplayMode(gDevice, mode.mode, mode.depth,
                    &flags, 0, &usable) != noErr || !usable || !(flags & (1UL << kNoSwitchConfirmBit))
                    || (flags & ((1UL << kDepthNotAvailableBit) | (1UL << kModeNotResizeBit) | (1UL << kNeverShowModeBit)));
            }), gModes.end());
            std::stable_sort(gModes.begin(), gModes.end(), [](const auto& a, const auto& b) {
                return a.width * a.height < b.width * b.height;
            });
            gModes.erase(std::unique(gModes.begin(), gModes.end(), [](const auto& a, const auto& b) {
                return a.width == b.width && a.height == b.height;
            }), gModes.end());
            for (const auto& mode : gModes) std::printf("8-bit display mode %dx%d id %lu depth %lu\n",
                mode.width, mode.height, static_cast<unsigned long>(mode.mode), static_cast<unsigned long>(mode.depth));
        }

        bool SwitchGameMode()
        {
            if (gGameMode.mode == 0) return true;
            if (!gHaveOriginalMode) return false;
            // Keep the configuration transaction open until the original desktop is restored.
            // Finder receives the final original-to-original configuration, not the temporary game size.
            OSErr error = DMBeginConfigureDisplays(&gDisplayState);
            if (error != noErr) { std::printf("display begin: %d\n", error); return false; }
            unsigned long depth = gGameMode.depth;
            gModeChanged = true; // retain restoration even if a driver changes the display then reports an error
            error = DMSetDisplayMode(gDevice, gGameMode.mode, &depth, 0, gDisplayState);
            std::printf("display switch %dx%d: %d\n", gGameMode.width, gGameMode.height, error);
            return error == noErr;
        }

        bool RestoreMode()
        {
            bool okay = true;
            if (gModeChanged)
            {
                unsigned long depth = gOriginalMode.csMode;
                const OSErr error = DMSetDisplayMode(gDevice, gOriginalMode.csData, &depth, 0, gDisplayState);
                VDSwitchInfoRec current{};
                const OSErr read = DMGetDisplayMode(gDevice, &current);
                const auto& bounds = (**gDevice).gdRect;
                okay = error == noErr && read == noErr && current.csData == gOriginalMode.csData
                    && current.csMode == gOriginalMode.csMode && std::memcmp(&bounds, &gOriginalBounds, sizeof(Rect)) == 0;
                std::printf("display restore mode: set %d read %d verified %d\n", error, read, okay);
                if (okay) gModeChanged = false;
            }
            if (okay && gDisplayState != nullptr)
            {
                const OSErr error = DMEndConfigureDisplays(gDisplayState);
                std::printf("display end: %d\n", error);
                if (error == noErr) gDisplayState = nullptr;
                else okay = false;
            }
            return okay;
        }
        bool gDepthChanged = false, gMoviesEntered = false, gOpen = false;
        Ptr gRestoreState = nullptr;
        WindowRef gWindow = nullptr;
        uint8_t* gScreen = nullptr;
        int gStride = 0, gWidth = 0, gHeight = 0;
        Point gLastMouse{ -1, -1 };
        int gButtonDown = 0; // SDL button currently held, 0 if none
        uint8_t gLastKeys[16]{};
        bool gQuitRequested = false, gQuitNow = false;
        bool gScrollPending = false;
        int gProbeSourceMode = 0;
        uint32_t gProbeSourceValue = 0;
        bool gPointerVisible = true;
        // Another application is in front: its windows own the screen, so we draw nothing.
        bool gSuspended = false, gHavePalette = false;
        Cursor gPointerShape{};
        bool gHavePointerShape = false, gPointerSwapped = false;
        uint16_t gPointerData[16]{}, gPointerMask[16]{};
        int gPointerHotX = 0, gPointerHotY = 0;
        bool gHardwarePointer = false, gProtectHardwarePointer = false, gCursorProbe = false;
        VDHardwareCursorDrawStateRec gHardwarePointerState{};
        OSErr gCursorSupportError = -1, gCursorStateError = -1;
        uint32_t gCursorSamples[4]{}, gCursorOffSamples[4]{}, gCursorOffscreenSamples[4]{}, gCursorLastPosition[4]{};
        uint32_t gCursorPositionChanges = 0, gCursorPosition = 0;
        uint64_t gCursorHideStart = 0, gCursorHiddenUs = 0, gCursorHiddenMaxUs = 0;
        uint32_t gCursorProbeFrames = 0, gCursorProbeLimit = 0;
        struct CursorMotion { int x0{},y0{},x1{},y1{}; unsigned frames{},at{}; } gCursorMotion;
        void RefreshHardwarePointer();
        void SampleHardwarePointer(unsigned phase);
        nlohmann::json ConfigureCursorProbe(const nlohmann::json&);
        bool PointerColoursSwapped();
        void ApplyPointerShape();
        Colour gPalette[256]{};
        Colour gShown[256]{}; // what the card's colour table holds
        bool gShownValid = false;
        void (*gDirectPaletteSet)(int first, int last, const Colour* colours) = nullptr;
        bool gDirectPaletteChecked = false;
        bool gDirectPaletteFull = true;
        bool gDriverPaletteForSync = false;
        uint32_t gDirectPaletteSets = 0, gDriverPaletteSets = 0, gBroadPaletteSets = 0;
        bool gGreyRestore = false;
        // Fallback for displays without a 256-colour mode (the QEMU test
        // machine): stay in 32-bit and expand through a lookup table.
        bool gDirect32 = false, gRecolour = false;
        IndexedDisplay gIndexed;
        uint64_t gPaletteChanges = 0, gPaletteUnchanged = 0, gRecolouredPixels = 0;
        const uint8_t* gLastBits = nullptr;
        int gLastPitch = 0;
#ifdef OPENRCT2_PERF_METRICS
        uint8_t gClockExpected[144 * 24]{}, gClockKnown[144 * 24]{};
        uint64_t gClockChecks = 0, gClockChanges = 0, gClockCheckUs = 0;
        struct ClockEvent { uint32_t ms, pixels; };
        ClockEvent gClockEvents[128]{};
        unsigned gClockEventCount = 0;
        void RememberClockPixels(const uint8_t* bits, int pitch, int left, int top, int right, int bottom)
        {
            const int origin = std::max(0, gWidth - 144);
            left = std::max(left, origin); top = std::max(top, 0);
            right = std::min(right, gWidth); bottom = std::min(bottom, 24);
            if (left >= right || top >= bottom) return;
            for (int y = top; y < bottom; ++y)
            {
                auto* expected = gClockExpected + y * 144 + left - origin;
                std::memcpy(expected, bits + y * pitch + left, right - left);
                std::memset(gClockKnown + y * 144 + left - origin, 1, right - left);
            }
        }
#endif
        bool gPresentationHidden = false;
        // Trap calls, not guessed visibility. Direct writes use overlap-tested hides,
        // rather than ShieldCursor's QuickDraw region state. Counters are always available.
        enum class CursorCause { lifecycle, present, scroll, dots, readback, recolour, verify, visibility, count };
        CursorCause gPresentationCause = CursorCause::present;
        struct CursorCalls { uint64_t hide = 0, show = 0, shield = 0, shape = 0; };
        CursorCalls gCursorCalls[static_cast<int>(CursorCause::count)]{};
        CursorCalls gCursorFrame{}, gCursorLastFrame{};
        uint64_t gCursorFrames = 0;
        void HideNative(CursorCause cause = CursorCause::lifecycle)
        {
            ++gCursorCalls[static_cast<int>(cause)].hide; ++gCursorFrame.hide; ::HideCursor();
            if (gCursorProbe && cause != CursorCause::lifecycle && cause != CursorCause::visibility && !gCursorHideStart)
                gCursorHideStart = Microseconds();
            SampleHardwarePointer(1);
        }
        void ShowNative(CursorCause cause = CursorCause::lifecycle)
        {
            ++gCursorCalls[static_cast<int>(cause)].show; ++gCursorFrame.show; ::ShowCursor();
            if (gCursorProbe && gCursorHideStart)
            {
                const auto hidden = Microseconds() - gCursorHideStart;
                gCursorHiddenUs += hidden; gCursorHiddenMaxUs = std::max(gCursorHiddenMaxUs,hidden);
                gCursorHideStart = 0;
            }
            SampleHardwarePointer(2);
        }

        struct KeyMapEntry
        {
            uint8_t mac;
            SDL_Scancode scancode;
        };
        const KeyMapEntry kKeyMap[] = {
            { 0, SDL_SCANCODE_A },   { 1, SDL_SCANCODE_S },   { 2, SDL_SCANCODE_D },   { 3, SDL_SCANCODE_F },
            { 4, SDL_SCANCODE_H },   { 5, SDL_SCANCODE_G },   { 6, SDL_SCANCODE_Z },   { 7, SDL_SCANCODE_X },
            { 8, SDL_SCANCODE_C },   { 9, SDL_SCANCODE_V },   { 11, SDL_SCANCODE_B },  { 12, SDL_SCANCODE_Q },
            { 13, SDL_SCANCODE_W },  { 14, SDL_SCANCODE_E },  { 15, SDL_SCANCODE_R },  { 16, SDL_SCANCODE_Y },
            { 17, SDL_SCANCODE_T },  { 18, SDL_SCANCODE_1 },  { 19, SDL_SCANCODE_2 },  { 20, SDL_SCANCODE_3 },
            { 21, SDL_SCANCODE_4 },  { 22, SDL_SCANCODE_6 },  { 23, SDL_SCANCODE_5 },  { 24, SDL_SCANCODE_EQUALS },
            { 25, SDL_SCANCODE_9 },  { 26, SDL_SCANCODE_7 },  { 27, SDL_SCANCODE_MINUS }, { 28, SDL_SCANCODE_8 },
            { 29, SDL_SCANCODE_0 },  { 30, SDL_SCANCODE_RIGHTBRACKET }, { 31, SDL_SCANCODE_O }, { 32, SDL_SCANCODE_U },
            { 33, SDL_SCANCODE_LEFTBRACKET }, { 34, SDL_SCANCODE_I }, { 35, SDL_SCANCODE_P }, { 36, SDL_SCANCODE_RETURN },
            { 37, SDL_SCANCODE_L },  { 38, SDL_SCANCODE_J },  { 39, SDL_SCANCODE_APOSTROPHE }, { 40, SDL_SCANCODE_K },
            { 41, SDL_SCANCODE_SEMICOLON }, { 42, SDL_SCANCODE_BACKSLASH }, { 43, SDL_SCANCODE_COMMA },
            { 44, SDL_SCANCODE_SLASH }, { 45, SDL_SCANCODE_N }, { 46, SDL_SCANCODE_M }, { 47, SDL_SCANCODE_PERIOD },
            { 48, SDL_SCANCODE_TAB }, { 49, SDL_SCANCODE_SPACE }, { 50, SDL_SCANCODE_GRAVE }, { 51, SDL_SCANCODE_BACKSPACE },
            { 53, SDL_SCANCODE_ESCAPE }, { 55, SDL_SCANCODE_LGUI }, { 56, SDL_SCANCODE_LSHIFT },
            { 57, SDL_SCANCODE_CAPSLOCK }, { 58, SDL_SCANCODE_LALT }, { 59, SDL_SCANCODE_LCTRL },
            { 76, SDL_SCANCODE_KP_ENTER }, { 96, SDL_SCANCODE_F5 }, { 97, SDL_SCANCODE_F6 }, { 98, SDL_SCANCODE_F7 },
            { 99, SDL_SCANCODE_F3 }, { 100, SDL_SCANCODE_F8 }, { 101, SDL_SCANCODE_F9 }, { 103, SDL_SCANCODE_F11 },
            { 109, SDL_SCANCODE_F10 }, { 111, SDL_SCANCODE_F12 }, { 115, SDL_SCANCODE_HOME },
            { 116, SDL_SCANCODE_PAGEUP }, { 117, SDL_SCANCODE_DELETE }, { 118, SDL_SCANCODE_F4 },
            { 119, SDL_SCANCODE_END }, { 120, SDL_SCANCODE_F2 }, { 121, SDL_SCANCODE_PAGEDOWN },
            { 122, SDL_SCANCODE_F1 }, { 123, SDL_SCANCODE_LEFT }, { 124, SDL_SCANCODE_RIGHT },
            { 125, SDL_SCANCODE_DOWN }, { 126, SDL_SCANCODE_UP },
        };
        SDL_Scancode ScancodeFor(int mac)
        {
            for (const auto& entry : kKeyMap)
                if (entry.mac == mac)
                    return entry.scancode;
            return SDL_SCANCODE_UNKNOWN;
        }
        Uint16 Modifiers(UInt16 flags)
        {
            Uint16 mod = KMOD_NONE;
            if (flags & shiftKey)
                mod |= KMOD_LSHIFT;
            if (flags & controlKey)
                mod |= KMOD_LCTRL;
            if (flags & optionKey)
                mod |= KMOD_LALT;
            if (flags & cmdKey)
                mod |= KMOD_LGUI;
            if (flags & alphaLock)
                mod |= KMOD_CAPS;
            return mod;
        }
        void PushKey(bool down, int mac, UInt16 modifiers, bool repeat)
        {
            const auto scancode = ScancodeFor(mac);
            if (scancode == SDL_SCANCODE_UNKNOWN)
                return;
            SDL_Event event{};
            event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
            event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
            event.key.repeat = repeat ? 1 : 0;
            event.key.keysym.scancode = scancode;
            event.key.keysym.sym = SDL_GetKeyFromScancode(scancode);
            // The keyboard map is sampled after draining events. A brief chord
            // can already be released then; EventRecord retains its modifiers.
            event.key.keysym.mod = Modifiers(modifiers);
            PushEvent(event);
        }
        pascal OSErr QuitHandler(const AppleEvent*, AppleEvent*, long)
        {
            // A Quit event (Finder shutdown, remote control) must not wait on a save prompt.
            gQuitNow = true;
            return noErr;
        }
        // AgentChat's application command: JSON text in the direct object, JSON text back.
        pascal OSErr CommandHandler(const AppleEvent* event, AppleEvent* reply, long)
        {
            char command[1025];
            DescType type = typeNull;
            Size size = 0;
            OSErr error = AEGetParamPtr(event, keyDirectObject, typeChar, &type, command, sizeof(command) - 1, &size);
            if (error != noErr)
                return error;
            const std::string text = RemoteCommand(std::string(command, static_cast<size_t>(size)));
            if (reply == nullptr || reply->descriptorType == typeNull)
                return noErr;
            return AEPutParamPtr(reply, keyDirectObject, typeChar, text.data(), static_cast<Size>(text.size()));
        }
        // Copy to video memory. The C library's memcpy moves bytes; here the bulk goes eight bytes at
        // a time through the floating-point registers (the G3's widest move), or four when the two
        // pointers cannot both be aligned for that.
        inline void CopyRow(uint8_t* out, const uint8_t* in, int count)
        {
            PixelCopy::Row(out, in, count, gWidePresent);
        }
        Rect PointerBounds()
        {
            // Low-memory location is already global and is kept current by mouse interrupts.
            // Avoid two Toolbox calls for each of the many small damage rectangles.
            const Point mouse = LMGetMouseLocation();
            return { static_cast<short>(mouse.v - std::clamp(gPointerHotY, 0, 15)), static_cast<short>(mouse.h - std::clamp(gPointerHotX, 0, 15)),
                static_cast<short>(mouse.v - std::clamp(gPointerHotY, 0, 15) + 16), static_cast<short>(mouse.h - std::clamp(gPointerHotX, 0, 15) + 16) };
        }
        bool PointerTouches(int left, int top, int right, int bottom)
        {
            if (!gPointerVisible || gSuspended) return false;
            const auto pointer = PointerBounds();
            return pointer.left < right && pointer.right > left && pointer.top < bottom && pointer.bottom > top;
        }
        bool ProtectPointer(int left, int top, int right, int bottom, CursorCause cause)
        {
            SampleHardwarePointer(0);
            // A successfully installed hardware cursor is an independent overlay.
            // Neither framebuffer writes nor copying/flipping a video page can
            // overwrite it. Hiding it here blanks the *visible* cursor even while
            // we update a hidden page. Retain protection for software cursors.
            if (gHardwarePointer && !gProtectHardwarePointer) return false;
            if (gPresentationHidden || !PointerTouches(left, top, right, bottom)) return false;
            HideNative(cause);
            if (gBatchCursor) { gPresentationHidden = true; gPresentationCause = cause; }
            return !gBatchCursor; // caller balances the non-batched temporary hide
        }
        CGrafPtr ScrollPort()
        {
            return NativeVSync::active ? NativeVSync::Port(NativeVSync::back) : GetWindowPort(gWindow);
        }
        void WaitForScroll()
        {
            if (!gScrollPending) return;
            const auto start = Microseconds();
            while (gWindow != nullptr && !QDDone(reinterpret_cast<GrafPtr>(ScrollPort()))) {}
            gScrollPending = false;
            gScreenScrollWaitMicros += Microseconds() - start;
        }
    } // namespace

    namespace
    {
        void RefreshHardwarePointer()
        {
            gHardwarePointer = false;
            if (!gDevice || gSuspended) return;
            const auto status = [](short selector,void* record) {
                CntrlParam block{}; block.ioCRefNum = (**gDevice).gdRefNum; block.csCode = selector;
                *reinterpret_cast<Ptr*>(block.csParam) = static_cast<Ptr>(record);
                return PBStatusSync(reinterpret_cast<ParmBlkPtr>(&block));
            };
            VDSupportsHardwareCursorRec support{};
            gHardwarePointerState = {};
            gCursorSupportError = status(cscSupportsHardwareCursor,&support);
            gCursorStateError = status(cscGetHardwareCursorDrawState,&gHardwarePointerState);
            gHardwarePointer = gCursorSupportError == noErr && support.csSupportsHardwareCursor
                && gCursorStateError == noErr && gHardwarePointerState.csCursorSet;
        }

        // QuickDraw draws a pointer's "black" with the last palette entry and its "white" with
        // the first; the game's palette has those the other way round (entry 0 is black).
        bool PointerColoursSwapped()
        {
            const auto light = [](const Colour& c) { return c.red * 3 + c.green * 6 + c.blue; };
            return !gDirect32 && gHavePalette && light(gPalette[255]) > light(gPalette[0]);
        }

        // Our own black-and-white pointer, not the system's (a themed system can have a colour
        // arrow, whose colours mean nothing in the game's palette).
        void ApplyPointerShape()
        {
            if (!gHavePointerShape)
                return;
            gPointerSwapped = PointerColoursSwapped();
            Cursor cursor{};
            for (int row = 0; row < 16; row++)
            {
                cursor.mask[row] = static_cast<short>(gPointerMask[row]);
                cursor.data[row] = static_cast<short>(
                    gPointerSwapped ? static_cast<uint16_t>(~gPointerData[row] & gPointerMask[row]) : gPointerData[row]);
            }
            cursor.hotSpot.h = static_cast<short>(std::clamp(gPointerHotX, 0, 15));
            cursor.hotSpot.v = static_cast<short>(std::clamp(gPointerHotY, 0, 15));
            gPointerShape = cursor;
            ++gCursorCalls[0].shape; ++gCursorFrame.shape;
            if (gCursorProbe) NativeVSync::CursorOperation(true,false);
            ::SetCursor(&gPointerShape);
            RefreshHardwarePointer();
            if (gCursorProbe) NativeVSync::CursorOperation(false,false);
        }
    } // namespace

    namespace
    {
        bool RestoreDesktop()
        {
            if (!NativeVSync::Stop()) return false;
            if (gRestoreState != nullptr)
            {
                const OSErr error = EndFullScreen(gRestoreState, 0);
                std::printf("fullscreen restore: %d\n", error);
                if (error != noErr) return false; // retain the restore token for the exit handler
                gRestoreState = nullptr;
                gWindow = nullptr;
            }
            if (!RestoreMode()) return false;
            const auto depth = (**(**gDevice).gdPMap).pixelSize;
            if (depth != gOriginalDepth || gGreyRestore)
            {
                const OSErr error = SetDepth(gDevice, gOriginalDepth, 1 << gdDevType, gGreyRestore ? 0 : 1);
                std::printf("depth restore: %d, verified %d\n", error,
                    (**(**gDevice).gdPMap).pixelSize == gOriginalDepth);
                if (error != noErr || (**(**gDevice).gdPMap).pixelSize != gOriginalDepth) return false;
            }
            if (gOriginalDepth == 8 && !RestoreClut()) return false;
            gDepthChanged = gGreyRestore = false;
            return true;
        }

        bool EnterGameDisplay()
        {
            if (!SwitchGameMode()) return false;
            gDirect32 = false;
            const auto depth = (**(**gDevice).gdPMap).pixelSize;
            if (depth == 8 && !TestDeviceAttribute(gDevice, gdDevType))
            {
                const OSErr error = SetDepth(gDevice, 8, 1 << gdDevType, 1);
                if (error != noErr) return false;
                gGreyRestore = gOriginalDepth == 8;
            }
            if (depth != 8)
            {
                if (HasDepth(gDevice, 8, 1 << gdDevType, 1) && SetDepth(gDevice, 8, 1 << gdDevType, 1) == noErr)
                    gDepthChanged = true;
                else if ((**(**gDevice).gdPMap).pixelSize == 32)
                    gDirect32 = true;
                else return false;
            }
            if (gMoviesEntered)
            {
                short width = 0, height = 0;
                RGBColor black{0, 0, 0};
                const OSErr error = BeginFullScreen(&gRestoreState, gDevice, &width, &height, &gWindow,
                    &black, fullScreenAllowEvents);
                if (error != noErr) { gRestoreState = nullptr; std::printf("fullscreen enter: %d\n", error); }
            }
            const auto map = (**gDevice).gdPMap;
            const auto bounds = (**gDevice).gdRect;
            gWidth = bounds.right - bounds.left;
            gHeight = bounds.bottom - bounds.top;
            gStride = (**map).rowBytes & 0x3FFF;
            gScreen = reinterpret_cast<uint8_t*>((**map).baseAddr);
#ifdef OPENRCT2_PERF_METRICS
            std::memset(gClockKnown, 0, sizeof(gClockKnown));
#endif
            gIndexed.Resize(gDirect32 ? gWidth : 0, gDirect32 ? gHeight : 0);
            gShownValid = false;
            gDirectPaletteChecked = false;
            gDirectPaletteSet = nullptr;
            NativeVSync::availabilityKnown = false;
            InitCursor();
            gPointerVisible = true;
            if (OpenRCT2::Config::Get().general.windowScale > 1.0f)
            {
                HideNative();
                gPointerVisible = false;
            }
            if (gHavePointerShape) ApplyPointerShape();
            return true;
        }
    }

    bool Open(int, int, int& width, int& height)
    {
        if (gOpen)
        {
            width = gWidth;
            height = gHeight;
            return true;
        }
        // A Toolbox application must initialise the managers it uses.
        InitGraf(&qd.thePort);
        InitFonts();
        InitWindows();
        InitMenus();
        TEInit();
        InitDialogs(nullptr);
        InitCursor();
        ClassicHost::gYieldObserver = NativeEventTrace::YieldObserver;
        gPointerVisible = true;
        if (gHavePointerShape) ApplyPointerShape();
        ShowCursor(OpenRCT2::Config::Get().general.windowScale <= 1.0f);
        FlushEvents(everyEvent, 0);
        gDevice = GetMainDevice();
        if (gDevice == nullptr)
            return false;
        gOriginalDepth = (**(**gDevice).gdPMap).pixelSize;
        gOriginalBounds = (**gDevice).gdRect;
        gHaveOriginalClut = false;
        if (gOriginalDepth == 8)
        {
            const OSErr error = ReadClut(gOriginalClut);
            std::printf("CLUT capture: %d\n", error);
            if (error != noErr) return false;
            gHaveOriginalClut = true;
        }
        gHaveOriginalMode = DMGetDisplayMode(gDevice, &gOriginalMode) == noErr;
        EnumerateModes();
        gGameMode = {};
        auto& config = OpenRCT2::Config::Get().general;
        const bool chooseDefault = config.fullscreenWidth <= 0 || config.fullscreenHeight <= 0;
        for (const auto& mode : gModes)
            if (mode.width == (chooseDefault ? 640 : config.fullscreenWidth)
                && mode.height == (chooseDefault ? 480 : config.fullscreenHeight))
            {
                gGameMode = mode;
                if (chooseDefault) { config.fullscreenWidth = 640; config.fullscreenHeight = 480; }
            }
        gOpen = true;
        static bool registeredExit = false;
        if (!registeredExit) { std::atexit(Close); registeredExit = true; }
        if (EnterMovies() == noErr) gMoviesEntered = true;
        if (!EnterGameDisplay()) { Close(); Message("Unable to enter the selected display mode."); return false; }
        if (gGameMode.mode == 0)
        {
            auto& settings = OpenRCT2::Config::Get().general;
            settings.fullscreenWidth = gWidth;
            settings.fullscreenHeight = gHeight;
        }
        SetEventMask(everyEvent);
        AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, NewAEEventHandlerUPP(QuitHandler), 0, false);
        AEInstallEventHandler('AGCT', 'cmnd', NewAEEventHandlerUPP(CommandHandler), 0, false);
        width = gWidth;
        height = gHeight;
        gOpen = true;
        std::printf("display %dx%d, %s, row bytes %d, original depth %d\n", gWidth, gHeight,
            gDirect32 ? "32-bit fallback" : "8-bit", gStride, gOriginalDepth);
        return true;
    }

    const std::vector<DisplayMode>& DisplayModes() { return gModes; }

    bool ResizeDisplay(int width, int height)
    {
        if (!gOpen || gSuspended) return false;
        if (width == gWidth && height == gHeight) return true;
        const auto mode = std::find_if(gModes.begin(),gModes.end(),[=](const auto& m) {
            return m.width == width && m.height == height;
        });
        if (mode == gModes.end()) return false;
        const auto previous = gGameMode;
        if (!RestoreDesktop()) return false;
        gGameMode = *mode;
        NativeVSync::ResetMode();
        if (!EnterGameDisplay())
        {
            // Return to the previous usable mode before reporting failure.
            if (!RestoreDesktop()) { gQuitNow = true; return false; }
            gGameMode = previous;
            if (!EnterGameDisplay()) { gQuitNow = true; return false; }
            SetPalette(gPalette);
            return false;
        }
        SetPalette(gPalette);
        return true;
    }

    void Close()
    {
        if (!gOpen)
            return;
        gOpen = false;
        if (gPresentationHidden)
        {
            ShowNative();
            gPresentationHidden = false;
        }
        if (!RestoreDesktop())
        {
            std::printf("desktop restoration failed; retaining restore state for the exit handler\n");
            gOpen = true;
            return;
        }
        if (gMoviesEntered)
            ExitMovies();
        gMoviesEntered = false;
        InitCursor();
        ClassicHost::gYieldObserver = nullptr;
        gPointerVisible = true;
    }

    void SetPalette(const Colour colours[256])
    {
        if (!gOpen)
            return;
        if (colours != gPalette)
            std::memcpy(gPalette, colours, sizeof(gPalette));
        gHavePalette = true;
        if (gSuspended) return;
        if (NativeVSync::active && !NativeVSync::flushingPalette)
        {
            NativeVSync::pendingPalette = true;
            return;
        }
        if (gHavePointerShape && PointerColoursSwapped() != gPointerSwapped)
            ApplyPointerShape();
        if (gDirect32)
        {
            if (gIndexed.SetPalette(colours)) gPaletteChanges++;
            else gPaletteUnchanged++;
            return;
        }
        // Send only the entries that differ from what the card has: the game rotates a handful
        // (water, chain lifts) nearly every frame, and setting all 256 costs about 5 ms on the G3.
        int first = 256, last = -1;
        bool gradual = gShownValid;
        for (int i = 0; i < 256; i++)
            if (!gShownValid || colours[i].red != gShown[i].red || colours[i].green != gShown[i].green
                || colours[i].blue != gShown[i].blue)
            {
                if (first == 256)
                    first = i;
                last = i;
                // Water/chain colours cycle independently of the daylight fade.
                // Larger changes elsewhere (load, lightning, a time jump) keep the
                // driver's blank-aligned update when page-flip sync is off.
                if (i < static_cast<int>(OpenRCT2::Drawing::PaletteIndex::waterWaves0)
                    || i > static_cast<int>(OpenRCT2::Drawing::PaletteIndex::primaryRemap2))
                    if (std::abs(int(colours[i].red) - int(gShown[i].red)) > 2
                        || std::abs(int(colours[i].green) - int(gShown[i].green)) > 2
                        || std::abs(int(colours[i].blue) - int(gShown[i].blue)) > 2)
                        gradual = false;
            }
        if (last < 0)
            return;
        // Dawn/dusk changes most colours every tick, even in sunny weather. The driver's
        // SetEntries waits for blank; the verified DAC path also supports gradual
        // broad ranges. Synced lightning already uses it during blank. Keep the
        // driver fallback for abrupt changes and when verification fails, and
        // the old small-range policy as a diagnostic comparison.
        if (last - first >= 64) ++gBroadPaletteSets;
        if (gDirectPaletteSet != nullptr && gShownValid && !gDriverPaletteForSync
            && ((gDirectPaletteFull && gradual) || last - first < 64 || NativeVSync::flushingPalette))
        {
            ++gDirectPaletteSets;
            gDirectPaletteSet(first, last, colours);
            for (int i = first; i <= last; i++)
                gShown[i] = colours[i];
            if (last - first >= 64 || NativeVSync::flushingPalette)
            {
                // Keep QuickDraw's software colour table coherent with direct
                // DAC updates, including dawn/dusk and lightning.
                const auto table = (**(**gDevice).gdPMap).pmTable;
                for (int i = first; i <= last; ++i)
                {
                    auto& entry = (**table).ctTable[i]; entry.value = i;
                    entry.rgb = {static_cast<uint16_t>(colours[i].red<<8),
                        static_cast<uint16_t>(colours[i].green<<8),static_cast<uint16_t>(colours[i].blue<<8)};
                }
                (**table).ctSeed = GetCTSeed();
            }
            return;
        }
        ColorSpec specs[256];
        for (int i = first; i <= last; i++)
        {
            specs[i - first].value = static_cast<short>(i);
            // 8-bit value in the high byte only: the driver rounds the 16-bit value to pick a gamma
            // entry, and the usual "times 257" form rounds every value from 128 up one step too high.
            specs[i - first].rgb.red = static_cast<unsigned short>(colours[i].red << 8);
            specs[i - first].rgb.green = static_cast<unsigned short>(colours[i].green << 8);
            specs[i - first].rgb.blue = static_cast<unsigned short>(colours[i].blue << 8);
            gShown[i] = colours[i];
        }
        gShownValid = true;
        ++gDriverPaletteSets;
        const GDHandle saved = GetGDevice();
        SetGDevice(gDevice);
        SetEntries(static_cast<short>(first), static_cast<short>(last - first), specs);
        SetGDevice(saved);
        // After the first complete palette: check whether small changes may go straight to the card.
        if (!gDirectPaletteChecked && first == 0 && last == 255)
        {
            gDirectPaletteChecked = true;
            std::printf("direct palette: %s\n", PrepareDirectPalette(true).c_str());
        }
    }

    uint64_t gPresentChangedPixels = 0, gPresentComparedPixels = 0;
    void Present(const uint8_t* bits, int pitch, int left, int top, int right, int bottom);
    void PresentChanged(const uint8_t* bits, uint8_t* shown, int pitch, int left, int top, int right, int bottom)
    {
        if (right > gWidth)
            right = gWidth;
        if (bottom > gHeight)
            bottom = gHeight;
        if (left >= right || top >= bottom)
            return;
        if (!gOpen || gSuspended || gDirect32)
        {
            // Not the plain 8-bit display: present in full and keep the copy current.
            Present(bits, pitch, left, top, right, bottom);
            for (int y = top; y < bottom; y++)
                std::memcpy(shown + static_cast<size_t>(y) * pitch + left, bits + static_cast<size_t>(y) * pitch + left,
                    static_cast<size_t>(right - left));
            return;
        }
        if (gScrollPending)
        {
            const auto scrollWaitStart = Microseconds();
            while (gWindow != nullptr && !QDDone(reinterpret_cast<GrafPtr>(ScrollPort())))
            {
            }
            gScrollPending = false;
            gScreenScrollWaitMicros += Microseconds() - scrollWaitStart;
        }
        gLastBits = bits;
        gLastPitch = pitch;
        NativeVSync::Written(left,top,right,bottom);
        const bool nearPointer = ProtectPointer(left, top, right, bottom, CursorCause::present);
        PresentChangedRuns(bits, shown, pitch, left, top, right, bottom, [bits, pitch](int y, int x0, int x1) {
            CopyRow(gScreen + static_cast<size_t>(y) * gStride + x0, bits + static_cast<size_t>(y) * pitch + x0, x1 - x0);
        });
        if (nearPointer)
            ShowNative(CursorCause::present);
    }

    void Present(const uint8_t* bits, int pitch, int left, int top, int right, int bottom)
    {
        if (!gOpen)
            return;
        if (gSuspended)
        {
            gLastBits = bits;
            gLastPitch = pitch;
            return;
        }
        if (right > gWidth)
            right = gWidth;
        if (bottom > gHeight)
            bottom = gHeight;
        if (left >= right || top >= bottom)
            return;
        // Only after a scroll can the video card still be busy with the screen.
        if (gScrollPending)
        {
            const auto scrollWaitStart = Microseconds();
            while (gWindow != nullptr && !QDDone(reinterpret_cast<GrafPtr>(ScrollPort())))
            {
            }
            gScrollPending = false;
            gScreenScrollWaitMicros += Microseconds() - scrollWaitStart;
        }
        if (gLastBits == nullptr)
            std::printf("first present %d,%d-%d,%d\n", left, top, right, bottom);
        gLastBits = bits;
        gLastPitch = pitch;
        NativeVSync::Written(left,top,right,bottom);
        // Hiding and showing the pointer is slow; it is only needed where the pointer is.
        const bool nearPointer = ProtectPointer(left, top, right, bottom, CursorCause::present);
        if (gDirect32)
        {
            gIndexed.Present(bits, pitch, left, top, right, bottom);
            for (int y = top; y < bottom; y++)
            {
                auto* out = reinterpret_cast<uint32_t*>(gScreen + static_cast<size_t>(y) * gStride) + left;
                const auto* in = bits + static_cast<size_t>(y) * pitch + left;
                for (int x = left; x < right; x++)
                    *out++ = gIndexed.RGB(*in++);
            }
        }
        else
        {
            const int count = right - left;
            const unsigned ahead = gPresentPrefetchRows;
            if (ahead == 0)
            {
                if (gPresentAlignedCopy)
                    PixelCopy::Rect(gScreen + static_cast<size_t>(top) * gStride + left, gStride,
                        bits + static_cast<size_t>(top) * pitch + left, pitch, count, bottom - top, true, gWidePresent);
                else
                    for (int y = top; y < bottom; y++)
                        CopyRow(gScreen + static_cast<size_t>(y) * gStride + left, bits + static_cast<size_t>(y) * pitch + left, count);
            }
            else
            {
                for (int y = top; y < bottom; y++)
                {
                    if (static_cast<unsigned>(bottom - y) > ahead)
                        OpenRCT2::CachePrefetchRange(bits + static_cast<size_t>(y + ahead) * pitch + left, count);
                    CopyRow(gScreen + static_cast<size_t>(y) * gStride + left, bits + static_cast<size_t>(y) * pitch + left, count);
                }
            }
        }
#ifdef OPENRCT2_PERF_METRICS
        RememberClockPixels(bits, pitch, left, top, right, bottom);
#endif
        if (nearPointer)
            ShowNative(CursorCause::present);
    }

    bool SystemMenuBarHidden()
    {
        // BeginFullScreen hides the menu bar; EndFullScreen restores its prior state.
        // Keep the old repair if native full-screen entry failed or the bar is visible.
        return gRestoreState != nullptr && GetMBarHeight() == 0;
    }

#ifdef OPENRCT2_PERF_METRICS
    bool ClockDamaged()
    {
        if (!gOpen || gSuspended || gScreen == nullptr) return false;
        const auto started = Microseconds();
        ++gClockChecks;
        const int left = std::max(0, gWidth - 144), width = gWidth - left;
        if (gScrollPending)
        {
            while (gWindow != nullptr && !QDDone(reinterpret_cast<GrafPtr>(ScrollPort()))) {}
            gScrollPending = false;
        }
        // Hide only a pointer overlapping the probe; it is also drawn into video memory.
        Point mouse; GetMouse(&mouse); LocalToGlobal(&mouse);
        const bool cursor = mouse.h + 40 > left && mouse.v - 40 < 24;
        if (cursor) HideNative(CursorCause::verify);
        uint32_t changed = 0;
        for (int y = 0; y < std::min(24, gHeight); ++y)
            for (int x = 0; x < width; ++x)
            {
                const int at = y * 144 + x;
                if (!gClockKnown[at]) continue;
                const auto expected = gClockExpected[at];
                changed += gDirect32
                    ? reinterpret_cast<const volatile uint32_t*>(gScreen + y * gStride)[left + x] != gIndexed.RGB(expected)
                    : static_cast<const volatile uint8_t*>(gScreen)[y * gStride + left + x] != expected;
            }
        if (cursor) ShowNative(CursorCause::verify);
        gClockCheckUs += Microseconds() - started;
        if (changed)
        {
            ++gClockChanges;
            if (gClockEventCount < std::size(gClockEvents))
                gClockEvents[gClockEventCount++] = { Milliseconds(), changed };
        }
        return changed != 0;
    }

    std::string ClockDiagnostics(bool reset)
    {
        std::string result = "{\"menu_bar_height\":" + std::to_string(GetMBarHeight())
            + ",\"fullscreen_owned\":" + (gRestoreState != nullptr ? "true" : "false")
            + ",\"checks\":" + std::to_string(gClockChecks)
            + ",\"changes\":" + std::to_string(gClockChanges)
            + ",\"check_us\":" + std::to_string(gClockCheckUs) + ",\"events\":[";
        for (unsigned i = 0; i < gClockEventCount; ++i)
        {
            if (i) result += ',';
            result += '[' + std::to_string(gClockEvents[i].ms) + ',' + std::to_string(gClockEvents[i].pixels) + ']';
        }
        result += "]}";
        if (reset) { gClockChecks = gClockChanges = gClockCheckUs = 0; gClockEventCount = 0; }
        return result;
    }
#endif

    std::string DisplayDiagnostics(bool verify)
    {
        uint64_t differences = 0;
        const bool available = gOpen && !gSuspended && gDirect32;
        if (verify && available)
        {
            HideNative(CursorCause::verify);
            differences = gIndexed.Differences(gScreen, gStride);
            ShowNative(CursorCause::verify);
        }
        char text[512];
        std::snprintf(text, sizeof(text),
            "{\"fallback\":%s,\"classic\":%s,\"shadow_bytes\":%lu,\"palette_changes\":%lu,"
            "\"palette_unchanged\":%lu,\"recoloured_pixels\":%lu,\"verified\":%s,\"differences\":%lu}",
            gDirect32 ? "true" : "false", IsClassicEnvironment() ? "true" : "false",
            static_cast<unsigned long>(gIndexed.Bytes()), static_cast<unsigned long>(gPaletteChanges),
            static_cast<unsigned long>(gPaletteUnchanged), static_cast<unsigned long>(gRecolouredPixels),
            verify && available ? "true" : "false", static_cast<unsigned long>(differences));
        return text;
    }

    long FreeMemory(bool largestBlock)
    {
        return static_cast<long>(largestBlock ? MaxBlock() : FreeMem());
    }

    bool ReadPixels(uint8_t* bits, int pitch, int width, int height, int left, int top, int right, int bottom)
    {
        if (!gOpen || gSuspended || width != gWidth || height != gHeight)
            return false;
        left = std::clamp(left, 0, width); right = std::clamp(right, left, width);
        top = std::clamp(top, 0, height); bottom = std::clamp(bottom, top, height);
        if (gScrollPending)
        {
            while (gWindow != nullptr && !QDDone(reinterpret_cast<GrafPtr>(ScrollPort()))) {}
            gScrollPending = false;
        }
        // The software cursor must not become part of the recovered game picture.
        if (gDirect32)
        {
            for (int y = top; y < bottom; y++)
                std::memcpy(bits + size_t(y) * pitch + left, gIndexed.Pixels() + size_t(y) * width + left, right - left);
            return true;
        }
        gWeatherTraffic.readbackBytes += uint64_t(right - left) * (bottom - top);
        const bool hidden = ProtectPointer(left, top, right, bottom, CursorCause::readback);
        for (int y = top; y < bottom; y++)
            CopyRow(bits + static_cast<size_t>(y) * pitch + left, gScreen + static_cast<size_t>(y) * gStride + left, right - left);
        if (hidden) ShowNative(CursorCause::readback);
        return true;
    }

    void ShowWeather(const WeatherSpan* spans, size_t count, WeatherPixels& saved, const uint8_t* clean, int pitch)
    {
        saved.count = 0;
        if (!gOpen || gSuspended || count == 0) return;
        NativeVSync::Written(0,0,gWidth,gHeight);
        if (gScrollPending)
        {
            while (gWindow != nullptr && !QDDone(reinterpret_cast<GrafPtr>(ScrollPort()))) {}
            gScrollPending = false;
        }
        Point mouse;
        GetMouse(&mouse);
        LocalToGlobal(&mouse);
        saved.left = mouse.h - 36; saved.right = mouse.h + 36;
        saved.top = mouse.v - 36; saved.bottom = mouse.v + 36;
        PrepareWeather(spans, count, saved);
        auto* next = saved.pixels.data();
        for (size_t i = 0; i < count; i++)
        {
            const auto& span = spans[i];
            const bool near = span.y > saved.top && span.y < saved.bottom;
            if (gDirect32)
            {
                const auto* under = clean ? clean + size_t(span.y) * pitch : gIndexed.Pixels() + size_t(span.y) * gWidth;
                auto* row = reinterpret_cast<uint32_t*>(gScreen + size_t(span.y) * gStride);
                for (int n = 0, x = span.x; n < span.count; n++, x += span.step)
                    for (int dx = 0; dx < span.width; dx++)
                    {
                        const int px = x + dx;
                        if (near && px > saved.left && px < saved.right) continue;
                        *next++ = ((uint32_t(span.y) * gWidth + px) << 8) | under[px];
                        gIndexed.Set(px, span.y, span.colour);
                        row[px] = gIndexed.RGB(span.colour);
                    }
            }
            else
            {
                const auto* under = clean ? clean + size_t(span.y) * pitch : gScreen + size_t(span.y) * gStride;
                next = near ? WeatherRow8<true>(span, next, gScreen, gStride, under, saved.left, saved.right)
                            : WeatherRow8<false>(span, next, gScreen, gStride, under, 0, 0);
            }
        }
        saved.count = next - saved.pixels.data();
        (clean || gDirect32 ? gWeatherTraffic.ramReadBytes : gWeatherTraffic.displayReadBytes) += saved.count;
        gWeatherTraffic.displayWriteBytes += saved.count * (gDirect32 ? 4 : 1);
#ifdef OPENRCT2_PERF_METRICS
        const int clockLeft = std::max(0, gWidth - 144);
        for (size_t i = 0; i < count; i++)
        {
            const auto& span = spans[i];
            if (span.y >= 24) continue;
            for (int n = 0, x = span.x; n < span.count; n++, x += span.step)
                for (int dx = 0; dx < span.width; dx++) if (x + dx >= clockLeft)
                {
                    const int at = span.y * 144 + x + dx - clockLeft;
                    gClockExpected[at] = gDirect32 ? gIndexed.Pixels()[size_t(span.y) * gWidth + x + dx]
                                                  : gScreen[size_t(span.y) * gStride + x + dx];
                    gClockKnown[at] = 1;
                }
        }
#endif
    }

    void RestoreWeather(const WeatherSpan* spans, size_t count, WeatherPixels& saved)
    {
        if (!gOpen || gSuspended || saved.count == 0) { saved.count = 0; return; }
        NativeVSync::Written(0,0,gWidth,gHeight);
        gWeatherTraffic.displayWriteBytes += saved.count * (gDirect32 ? 4 : 1);
        // Test actual restored runs, excluding the previous frame's pointer gap.
        // A row's broad bounds can cover the cursor while every nearby pixel was skipped.
        const auto pointer = PointerBounds();
        const bool touched = WeatherTouches(spans, count, pointer.left, pointer.top,
                                            pointer.right, pointer.bottom, &saved);
        const bool hidden = touched && ProtectPointer(pointer.left, pointer.top, pointer.right,
                                                     pointer.bottom, CursorCause::dots);
        if (gDirect32)
            for (size_t i = saved.count; i != 0;)
            {
                const uint32_t pixel = saved.pixels[--i];
                const int offset = pixel >> 8, y = offset / gWidth, x = offset % gWidth;
                const uint8_t colour = static_cast<uint8_t>(pixel);
                gIndexed.Set(x, y, colour);
                reinterpret_cast<uint32_t*>(gScreen + size_t(y) * gStride)[x] = gIndexed.RGB(colour);
            }
        else RestoreWeather8(saved, gScreen);
#ifdef OPENRCT2_PERF_METRICS
        const int clockLeft = std::max(0, gWidth - 144);
        const int stride = gDirect32 ? gWidth : gStride;
        for (size_t i = saved.count; i != 0;)
        {
            const uint32_t pixel = saved.pixels[--i];
            const int offset = pixel >> 8, y = offset / stride, x = offset % stride;
            if (y < 24 && x >= clockLeft && x < gWidth)
            {
                const int at = y * 144 + x - clockLeft;
                gClockExpected[at] = static_cast<uint8_t>(pixel); gClockKnown[at] = 1;
            }
        }
#endif
        saved.count = 0;
        if (hidden) ShowNative(CursorCause::dots);
    }

    bool WeatherNeedsCleanCanvas()
    {
        return !gDirect32;
    }

    bool CanScroll()
    {
        if (!gOpen || gWindow == nullptr || gSuspended)
            return false;
        // The scroll is a QuickDraw copy inside our window, clipped to what the Window Manager
        // considers visible of it. If anything is in front (a system alert about a USB device,
        // a notification, another application's window), part of the screen would silently not
        // scroll while we repaint as if it had: only scroll while the whole window is visible.
        const auto port = GetWindowPort(gWindow);
        const RgnHandle visible = port->visRgn;
        if (visible == nullptr || (**visible).rgnSize != 10)
            return false; // not a plain rectangle
        const Rect& box = (**visible).rgnBBox;
        return box.left <= 0 && box.top <= 0 && box.right >= gWidth && box.bottom >= gHeight;
    }

    bool Scroll(int x, int y, int width, int height, int dx, int dy)
    {
        if (!gOpen || gWindow == nullptr || gSuspended)
            return gSuspended;
        if (!CanScroll())
            return false; // something is in front of part of the window: the caller sends the pixels
        if (gDirect32 && !gIndexed.Scroll(x, y, width, height, dx, dy)) return false;
        Rect target{ static_cast<short>(y), static_cast<short>(x), static_cast<short>(y + height),
                     static_cast<short>(x + width) };
        Rect source = target;
        OffsetRect(&source, static_cast<short>(-dx), static_cast<short>(-dy));
        Rect both;
        UnionRect(&source, &target, &both);
        const bool temporaryHide = ProtectPointer(both.left, both.top, both.right, both.bottom, CursorCause::scroll);
        if (NativeVSync::active)
        {
            const bool copied = NativeVSync::Scroll(source,target);
            if (temporaryHide) ShowNative(CursorCause::scroll);
            return copied; // a failed copy makes the caller send the complete RAM rectangle
        }
        GrafPtr saved;
        GetPort(&saved);
        const auto port = ScrollPort();
        SetPort(reinterpret_cast<GrafPtr>(port));
        // Plain copy: device black/white pixel values, so nothing is colourized.
        port->fgColor = 255;
        port->bkColor = 0;
        port->rgbFgColor = { 0, 0, 0 };
        port->rgbBkColor = { 0xffff, 0xffff, 0xffff };
        const auto* bitmap = &reinterpret_cast<GrafPtr>(port)->portBits;
        if (gCursorProbe) NativeVSync::CursorOperation(false,true);
        CopyBits(bitmap, bitmap, &source, &target, srcCopy, nullptr);
        if (gCursorProbe) NativeVSync::CursorOperation(false,false);
        gScrollPending = true;
        SetPort(saved);
        if (temporaryHide)
        {
            WaitForScroll(); // never restore the cursor over an unfinished asynchronous copy
            ShowNative(CursorCause::scroll);
        }
        return true;
    }

    void EndFrame()
    {
        if (gDirect32 && !gSuspended && gIndexed.Changed())
        {
            const auto pointer = PointerBounds();
            const bool hidden = !gPresentationHidden && gPointerVisible && (!gHardwarePointer || gProtectHardwarePointer)
                && gIndexed.RecolourTouches(pointer.left, pointer.top, pointer.right, pointer.bottom);
            if (hidden)
            {
                HideNative(CursorCause::recolour);
                if (gBatchCursor) { gPresentationHidden = true; gPresentationCause = CursorCause::recolour; }
            }
            gRecolouredPixels += gIndexed.Recolour(gScreen, gStride);
            if (hidden && !gBatchCursor) ShowNative(CursorCause::recolour);
        }
        if (gPresentationHidden)
        {
            WaitForScroll();
            ShowNative(gPresentationCause);
            gPresentationHidden = false;
        }
        // The frame in memory may be out of date (the screen is scrolled by itself), so the whole
        // screen is never simply copied from it: the drawing engine paints everything again.
        if (gRecolour)
        {
            gRecolour = false;
            gDrawingRefreshRequest = true;
        }
        NativeVSync::End();
        SampleHardwarePointer(3);
        if (gCursorProbe)
        {
            ++gCursorProbeFrames;
            if (gCursorMotion.at < gCursorMotion.frames)
            {
                const unsigned at = ++gCursorMotion.at;
                const unsigned span = std::max(1u,gCursorMotion.frames / 4);
                const unsigned phase = at % (span * 2);
                const unsigned fraction = phase <= span ? phase : span * 2 - phase;
                const int x = gCursorMotion.x0 + (gCursorMotion.x1-gCursorMotion.x0)*int(fraction)/int(span);
                const int y = gCursorMotion.y0 + (gCursorMotion.y1-gCursorMotion.y0)*int(fraction)/int(span);
                WarpPointer(x,y);
                SDL_Event event{}; event.type = SDL_MOUSEMOTION; event.motion.x = x; event.motion.y = y;
                PushEvent(event); RemoteMouse(x,y,0);
            }
            if (gCursorProbeLimit && gCursorProbeFrames >= gCursorProbeLimit) gCursorProbe = false;
        }
        NativeVSync::CursorWatch(gCursorProbe,gPointerVisible && !gSuspended,false);
        gCursorLastFrame = gCursorFrame;
        gCursorFrame = {};
        ++gCursorFrames;
    }

    namespace
    {
        bool gEventPolling = true;
        uint64_t gLastFullEventCall = 0;
    }
    void SetEventPolling(bool on) { gEventPolling = on; }
    bool EventPolling() { return gEventPolling; }
    std::string EventDiagnostics(const std::string& command) { return NativeEventTrace::Command(command); }

    void PumpEvents()
    {
        const bool trace = gTracePump;
        EventRecord event;
        // EventAvail participates in Process Manager scheduling. Peek first so a
        // null frame does not pay WaitNextEvent's more expensive null-event path.
        // Keep a bounded full-service watchdog and sleep when we are in the background.
        if (trace) gPumpProfile.calls++;
        while (true)
        {
            // Diagnostic isolation of periodic driver/desk-accessory work from
            // the event call's cooperative Process Manager handoff.
            if (NativeEventTrace::enabled && NativeEventTrace::splitSystemTask)
            {
                EventRecord diagnostic{};
                NativeEventTrace::Call(4, 0, diagnostic, gSuspended);
            }
            const auto now = Microseconds();
            const bool full = !gEventPolling || gSuspended || now - gLastFullEventCall >= 250000;
            const auto waitStart = trace ? now : 0;
            bool available;
            if (full)
            {
                available = NativeEventTrace::Call(0, gEventPolling && gSuspended ? 6 : 0, event, gSuspended);
                gLastFullEventCall = Microseconds();
                auto& frame = OpenRCT2::FrameTiming::current;
                const auto elapsed = static_cast<uint32_t>(gLastFullEventCall - now);
                frame.yieldCalls++; frame.yieldUs += elapsed;
                frame.yieldMaxUs = std::max(frame.yieldMaxUs, elapsed);
                frame.coarseYields += gEventPolling && gSuspended;
            }
            else
            {
                if (NativeEventTrace::pollMethod == 2)
                    available = NativeEventTrace::Call(2, 0, event, gSuspended);
                else
                    available = NativeEventTrace::Call(NativeEventTrace::pollMethod, 0, event, gSuspended)
                        && NativeEventTrace::Call(2, 0, event, gSuspended);
                // The 0.3.4 peek path also enters the cooperative scheduler.
                // Attribute its stalls even when detailed pump tracing is off.
                auto& frame = OpenRCT2::FrameTiming::current;
                const auto elapsed = static_cast<uint32_t>(Microseconds() - now);
                frame.yieldCalls++; frame.yieldUs += elapsed;
                frame.yieldMaxUs = std::max(frame.yieldMaxUs, elapsed);
            }
            if (trace)
            {
                const auto us = Microseconds() - waitStart;
                if (full)
                {
                    gPumpProfile.waitCalls++;
                    gPumpProfile.waitMicros += us;
                    const int bin = us < 10 ? 0 : us < 100 ? 1 : us < 1000 ? 2 : us < 5000 ? 3 : us < 20000 ? 4 : 5;
                    gPumpProfile.waitBins[bin]++;
                }
                else
                {
                    gPumpProfile.pollCalls++;
                    gPumpProfile.pollMicros += us;
                }
                if (!available)
                {
                    if (event.what == nullEvent) gPumpProfile.nullReturns++;
                    else gPumpProfile.interceptedReturns++;
                }
                else if (event.what >= 0 && event.what < 24) gPumpProfile.events[event.what]++;
            }
            if (!available) break;
            if (event.what >= 0 && event.what < 32)
                OpenRCT2::FrameTiming::current.eventMask |= 1u << event.what;
            const auto eventStart = trace ? Microseconds() : 0;
            switch (event.what)
            {
                case mouseDown:
                case mouseUp:
                {
                    SDL_Event button{};
                    const bool down = event.what == mouseDown;
                    if (down)
                        gButtonDown = (event.modifiers & controlKey) ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
                    if (gButtonDown == 0)
                        break;
                    button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                    button.button.button = static_cast<Uint8>(gButtonDown);
                    button.button.state = down ? SDL_PRESSED : SDL_RELEASED;
                    button.button.x = event.where.h;
                    button.button.y = event.where.v;
                    PushEvent(button);
                    if (!down)
                        gButtonDown = 0;
                    break;
                }
                case keyDown:
                case autoKey:
                {
                    const int mac = (event.message & keyCodeMask) >> 8;
                    const char character = static_cast<char>(event.message & charCodeMask);
                    if ((event.modifiers & cmdKey) && (character == 'q' || character == 'Q'))
                    {
                        // The first Cmd-Q asks (the game's own save prompt); a second one leaves.
                        if (gQuitRequested)
                            gQuitNow = true;
                        gQuitRequested = true;
                        break;
                    }
                    PushKey(true, mac, event.modifiers, event.what == autoKey);
                    if (TextInputActive() && !(event.modifiers & (controlKey | cmdKey))
                        && character >= 0x20 && character < 0x7F)
                    {
                        SDL_Event text{};
                        text.type = SDL_TEXTINPUT;
                        text.text.text[0] = character;
                        PushEvent(text);
                    }
                    break;
                }
                case keyUp:
                    PushKey(false, (event.message & keyCodeMask) >> 8, event.modifiers, false);
                    break;
                case updateEvt:
                    BeginUpdate(reinterpret_cast<WindowRef>(event.message));
                    EndUpdate(reinterpret_cast<WindowRef>(event.message));
                    // Something else drew over our screen (a screen saver, another application's
                    // window, a system alert): paint everything again.
                    gRecolour = true;
                    break;
                case kHighLevelEvent:
                    AEProcessAppleEvent(&event);
                    break;
                case osEvt:
                    if (((event.message >> 24) & 0xFF) == suspendResumeMessage)
                    {
                        const bool suspend = (event.message & resumeFlag) == 0;
                        if (suspend && !gSuspended)
                        {
                            gSuspended = true;
                            if (!RestoreDesktop()) gQuitNow = true;
                            gScreen = nullptr;
                            if (gPresentationHidden) { ShowNative(); gPresentationHidden = false; }
                            InitCursor();
                            gPointerVisible = true;
                        }
                        else if (!suspend && gSuspended)
                        {
                            const int width = gWidth, height = gHeight;
                            if (!EnterGameDisplay() || width != gWidth || height != gHeight)
                                gQuitNow = true;
                            else
                            {
                                gSuspended = false;
                                gRecolour = true;
                                OpenRCT2::Drawing::GfxInvalidateScreen();
                                if (gHavePalette) SetPalette(gPalette);
                            }
                        }
                        SDL_Event focus{};
                        focus.type = SDL_WINDOWEVENT;
                        focus.window.event = gSuspended ? SDL_WINDOWEVENT_FOCUS_LOST : SDL_WINDOWEVENT_FOCUS_GAINED;
                        PushEvent(focus);
                        std::printf(gSuspended ? "suspended\n" : "resumed\n");
                    }
                    break;
                default:
                    break;
            }
            if (trace) gPumpProfile.eventMicros += Microseconds() - eventStart;
        }
        const auto inputStart = trace ? Microseconds() : 0;
        // Held keys come from the keyboard map so they can never get stuck.
        KeyMap map;
        GetKeys(map);
        const auto* keys = reinterpret_cast<const uint8_t*>(map);
        for (const auto& entry : kKeyMap)
            SetKey(entry.scancode, ((keys[entry.mac >> 3] >> (entry.mac & 7)) & 1) != 0);
        std::memcpy(gLastKeys, keys, sizeof(gLastKeys));

        Point mouse;
        GetMouse(&mouse);
        LocalToGlobal(&mouse);
        Uint32 buttons = 0;
        if (gButtonDown != 0 && Button())
            buttons = SDL_BUTTON(gButtonDown);
        SetMouse(mouse.h, mouse.v, buttons);
        if (mouse.h != gLastMouse.h || mouse.v != gLastMouse.v)
        {
            SDL_Event motion{};
            motion.type = SDL_MOUSEMOTION;
            motion.motion.x = mouse.h;
            motion.motion.y = mouse.v;
            motion.motion.xrel = gLastMouse.h < 0 ? 0 : mouse.h - gLastMouse.h;
            motion.motion.yrel = gLastMouse.h < 0 ? 0 : mouse.v - gLastMouse.v;
            motion.motion.state = buttons;
            PushEvent(motion);
            gLastMouse = mouse;
        }
        static bool promptShown = false;
        if (gQuitNow)
        {
            gQuitNow = false;
            std::printf("quitting\n");
            OpenRCT2::GetContext()->Finish();
        }
        else if (gQuitRequested && !promptShown)
        {
            promptShown = true;
            std::printf("quit requested\n");
            SDL_Event quit{};
            quit.type = SDL_QUIT;
            PushEvent(quit);
        }
        if (trace) gPumpProfile.inputMicros += Microseconds() - inputStart;
    }

    std::string EventCallProbe()
    {
        // Mask zero: timed null-event service, never consume application input.
        // Batch timing excludes per-call timer reads and counter writes.
        std::string out = "{";
        const char* names[] = { "WaitNextEvent", "GetNextEvent", "EventAvail", "OSEventAvail" };
        for (int method = 0; method < 4; method++)
        {
            EventRecord event{};
            const auto start = Microseconds();
            for (int n = 0; n < 200; n++)
                switch (method)
                {
                    case 0: WaitNextEvent(0, &event, 0, nullptr); break;
                    case 1: GetNextEvent(0, &event); break;
                    case 2: EventAvail(0, &event); break;
                    case 3: OSEventAvail(0, &event); break;
                }
            char row[160];
            std::snprintf(row, sizeof(row), "%s\"%s\":%.3f", method ? "," : "", names[method],
                          double(Microseconds() - start) / 200.0);
            out += row;
        }
        const auto timerStart = Microseconds();
        for (int n = 0; n < 20000; n++) Microseconds();
        char timer[80];
        std::snprintf(timer, sizeof(timer), ",\"timer_us\":%.3f", double(Microseconds() - timerStart) / 20000.0);
        return out + timer + "}";
    }

    uint64_t Microseconds()
    {
        return ClassicHost::Microseconds();
    }

    uint32_t Milliseconds()
    {
        return static_cast<uint32_t>(ClassicHost::Microseconds() / 1000);
    }

    void SetPointerShape(const uint16_t data[16], const uint16_t mask[16], int hotX, int hotY)
    {
        if (gHavePointerShape && hotX == gPointerHotX && hotY == gPointerHotY
            && std::memcmp(gPointerData, data, sizeof(gPointerData)) == 0
            && std::memcmp(gPointerMask, mask, sizeof(gPointerMask)) == 0)
            return;
        std::memcpy(gPointerData, data, sizeof(gPointerData));
        std::memcpy(gPointerMask, mask, sizeof(gPointerMask));
        gPointerHotX = hotX;
        gPointerHotY = hotY;
        gHavePointerShape = true;
        if (gOpen && !gSuspended) ApplyPointerShape();
    }

    std::string CursorDiagnostics(bool reset, const std::string& options)
    {
        const auto c = nlohmann::json::parse(options);
        const auto probe = ConfigureCursorProbe(c);
        static const char* names[] = { "lifecycle", "present", "scroll", "dots", "readback", "recolour", "verify", "visibility" };
        auto calls = [](const CursorCalls& c) {
            return "{\"hide\":" + std::to_string(c.hide) + ",\"show\":" + std::to_string(c.show)
                + ",\"shield\":" + std::to_string(c.shield) + ",\"shape\":" + std::to_string(c.shape) + "}";
        };
        std::string out = "{\"frames\":" + std::to_string(gCursorFrames) + ",\"last\":" + calls(gCursorLastFrame) + ",\"causes\":{";
        for (int i = 0; i < static_cast<int>(CursorCause::count); ++i)
        {
            if (i) out += ',';
            out += "\"" + std::string(names[i]) + "\":" + calls(gCursorCalls[i]);
        }
        if (reset) { std::memset(gCursorCalls, 0, sizeof(gCursorCalls)); gCursorFrames = 0; }
        return out + "},\"probe\":" + probe.dump() + "}";
    }

    void ShowCursor(bool visible)
    {
        // The game hides the pointer while the view is dragged with the right button.
        if (gSuspended || visible == gPointerVisible)
            return;
        gPointerVisible = visible;
        if (visible)
            ShowNative(CursorCause::visibility);
        else
            HideNative(CursorCause::visibility);
    }

    // --- Rage 128 2D engine, driven directly -----------------------------------------
    // QuickDraw costs about 20 microseconds per CopyBits whatever the size. The engine itself
    // takes a blit as three register writes. This finds the card's register block through the
    // Name Registry and, right after QuickDraw has set the engine up for a transparent copy
    // from the off-screen world, repeats that copy by writing only source, destination and size.
    namespace
    {
        struct Rage128
        {
            volatile uint8_t* base = nullptr;
            const uint8_t* frameBuffer = nullptr; // start of the card's video memory in our address space
            bool swap = true;
            uint32_t Read(uint32_t offset) const
            {
                const uint32_t raw = *reinterpret_cast<volatile uint32_t*>(base + offset);
                __asm__ volatile("eieio");
                return swap ? __builtin_bswap32(raw) : raw;
            }
            void Write(uint32_t offset, uint32_t value) const
            {
                *reinterpret_cast<volatile uint32_t*>(base + offset) = swap ? __builtin_bswap32(value) : value;
                __asm__ volatile("eieio");
            }
        };
        constexpr uint32_t kSrcPitchOffset = 0x1428, kDstPitchOffset = 0x142C, kSrcYX = 0x1434, kDstYX = 0x1438,
                           kDstHeightWidth = 0x143C, kGuiMaster = 0x146C, kClrCmpCntl = 0x15C0, kClrCmpSrc = 0x15C4,
                           kClrCmpMask = 0x15CC, kDpCntl = 0x16C0, kDpDatatype = 0x16C4, kDpMix = 0x16C8,
                           kDefaultOffset = 0x16E0, kDefaultPitch = 0x16E4, kScTopLeft = 0x16EC,
                           kScBottomRight = 0x16F0, kGuiStat = 0x1740;

        bool FindRage128(Rage128& rage, std::string& out)
        {
            if (IsClassicEnvironment())
            {
                out += "\"classic\":true,";
                return false; // before even enumerating the registry, let alone reading MMIO
            }
            RegEntryIter cookie;
            if (RegistryEntryIterateCreate(&cookie) != noErr)
                return false;
            bool found = false;
            Boolean done = false;
            RegEntryID entry;
            for (int guard = 0; guard < 4000 && !found; guard++)
            {
                if (RegistryEntryIterate(&cookie, kRegIterContinue, &entry, &done) != noErr || done)
                    break;
                uint32_t vendor = 0, device = 0;
                RegPropertyValueSize size = 4;
                if (RegistryPropertyGet(&entry, "vendor-id", &vendor, &size) == noErr && size == 4 && vendor == 0x1002)
                {
                    size = 4;
                    const auto deviceError = RegistryPropertyGet(&entry, "device-id", &device, &size);
                    uint8_t assigned[240];
                    uint32_t logical[12];
                    RegPropertyValueSize assignedSize = sizeof(assigned), logicalSize = sizeof(logical);
                    if (deviceError == noErr && size == 4 && device == 0x5052 && RegistryPropertyGet(&entry, "assigned-addresses", assigned, &assignedSize) == noErr
                        && RegistryPropertyGet(&entry, "AAPL,address", logical, &logicalSize) == noErr
                        && assignedSize <= sizeof(assigned) && logicalSize <= sizeof(logical)
                        && assignedSize % 20 == 0 && logicalSize % 4 == 0)
                    {
                        for (unsigned i = 0; i < assignedSize / 20 && i < logicalSize / 4; i++)
                        {
                            if (logical[i] == 0 || (logical[i] & 3) != 0) continue;
                            if (assigned[i * 20 + 3] == 0x10) // video memory is base address register 0
                                rage.frameBuffer = reinterpret_cast<const uint8_t*>(logical[i]);
                            if (assigned[i * 20 + 3] != 0x18) // the register block is base address register 2
                                continue;
                            uint32_t physical;
                            std::memcpy(&physical, &assigned[i * 20 + 8], 4);
                            rage.base = reinterpret_cast<volatile uint8_t*>(logical[i]);
                            const uint32_t mirror = *reinterpret_cast<volatile uint32_t*>(rage.base + 0x0F18) & ~0xFu;
                            if (__builtin_bswap32(mirror) == physical || (__builtin_bswap32(mirror) & ~0xFu) == physical)
                                rage.swap = true, found = true;
                            else if (mirror == physical)
                                rage.swap = false, found = true;
                            char text[160];
                            std::snprintf(text, sizeof(text), "\"registers\":{\"logical\":%lu,\"physical\":%lu,\"mirror\":%lu,\"swapped\":%s,\"ok\":%s},",
                                static_cast<unsigned long>(logical[i]), static_cast<unsigned long>(physical),
                                static_cast<unsigned long>(mirror), rage.swap ? "true" : "false", found ? "true" : "false");
                            out += text;
                        }
                    }
                }
                RegistryEntryIDDispose(&entry);
            }
            RegistryEntryIterateDispose(&cookie);
            return found;
        }

        void DumpEngine(const Rage128& rage, std::string& out, const char* name)
        {
            char text[640];
            std::snprintf(text, sizeof(text),
                "\"%s\":{\"src_pitch_offset\":%lu,\"dst_pitch_offset\":%lu,\"src_y_x\":%lu,\"dst_y_x\":%lu,\"master\":%lu,"
                "\"clr_cmp_cntl\":%lu,\"clr_cmp_src\":%lu,\"clr_cmp_mask\":%lu,\"dp_cntl\":%lu,\"dp_datatype\":%lu,\"dp_mix\":%lu,"
                "\"default_offset\":%lu,\"default_pitch\":%lu,\"sc_top_left\":%lu,\"sc_bottom_right\":%lu,\"gui_stat\":%lu,"
                "\"pm4_buffer_cntl\":%lu,\"pm4_stat\":%lu,\"pm4_micro_cntl\":%lu,\"gen_reset_cntl\":%lu,\"bus_cntl\":%lu},",
                name, static_cast<unsigned long>(rage.Read(kSrcPitchOffset)), static_cast<unsigned long>(rage.Read(kDstPitchOffset)),
                static_cast<unsigned long>(rage.Read(kSrcYX)), static_cast<unsigned long>(rage.Read(kDstYX)),
                static_cast<unsigned long>(rage.Read(kGuiMaster)), static_cast<unsigned long>(rage.Read(kClrCmpCntl)),
                static_cast<unsigned long>(rage.Read(kClrCmpSrc)), static_cast<unsigned long>(rage.Read(kClrCmpMask)),
                static_cast<unsigned long>(rage.Read(kDpCntl)), static_cast<unsigned long>(rage.Read(kDpDatatype)),
                static_cast<unsigned long>(rage.Read(kDpMix)), static_cast<unsigned long>(rage.Read(kDefaultOffset)),
                static_cast<unsigned long>(rage.Read(kDefaultPitch)), static_cast<unsigned long>(rage.Read(kScTopLeft)),
                static_cast<unsigned long>(rage.Read(kScBottomRight)), static_cast<unsigned long>(rage.Read(kGuiStat)),
                static_cast<unsigned long>(rage.Read(0x0704)), static_cast<unsigned long>(rage.Read(0x07B8)),
                static_cast<unsigned long>(rage.Read(0x07FC)), static_cast<unsigned long>(rage.Read(0x00F0)),
                static_cast<unsigned long>(rage.Read(0x0030)));
            out += text;
        }

        bool WaitIdle(const Rage128& rage)
        {
            for (long spin = 0; spin < 20000000; spin++)
                if ((rage.Read(kGuiStat) & 0x80000000u) == 0)
                    return true;
            return false;
        }

        // stage 1: report the engine state QuickDraw leaves behind. stage 2: also drive it.
        void EngineProbe(std::string& out, int stage, CGrafPtr port, const BitMap* screenBits, GWorldPtr cardWorld)
        {
            Rage128 rage;
            if (!FindRage128(rage, out))
            {
                out += "\"engine\":\"no Rage 128 register block found\",";
                return;
            }
            const BitMap* cardBits = reinterpret_cast<const BitMap*>(*GetGWorldPixMap(cardWorld));
            struct Size
            {
                int width, height;
            };
            const Size sizes[] = { { 16, 24 }, { 64, 32 } };
            const uint8_t* cardBase = reinterpret_cast<const uint8_t*>(GetPixBaseAddr(GetGWorldPixMap(cardWorld)));
            for (const auto& size : sizes)
            {
                for (int mode = 0; mode < 2; mode++)
                {
                    // QuickDraw programs the engine for this kind of copy.
                    Rect from{ 0, 0, static_cast<short>(size.height), static_cast<short>(size.width) };
                    Rect to{ 100, 100, static_cast<short>(100 + size.height), static_cast<short>(100 + size.width) };
                    CopyBits(cardBits, screenBits, &from, &to, mode == 0 ? srcCopy : transparent, nullptr);
                    while (!QDDone(reinterpret_cast<GrafPtr>(port)))
                    {
                    }
                    char name[64];
                    std::snprintf(name, sizeof(name), "state_%s_%dx%d", mode == 0 ? "opaque" : "transparent", size.width, size.height);
                    DumpEngine(rage, out, name);
                    if (stage < 2)
                        continue;
                    if (stage >= 3)
                    {
                        // The driver keeps the card in command-processor mode: drawing commands are
                        // packets in a FIFO written as (even, odd) word pairs. A type-0 packet is
                        // "write these consecutive registers": source, destination, size = one blit.
                        const auto fifoFree = [&rage] { return rage.Read(0x07B8) & 0xFFFu; };
                        const auto idleNow = [&rage] {
                            for (long spin = 0; spin < 20000000; spin++)
                                if ((rage.Read(0x07B8) & 0x80010000u) == 0 && (rage.Read(kGuiStat) & 0x80000000u) == 0)
                                    return true;
                            return false;
                        };
                        if (!idleNow())
                        {
                            out += "\"engine\":\"command processor never idle\",";
                            return;
                        }
                        const int cardRow = (**GetGWorldPixMap(cardWorld)).rowBytes & 0x3FFF;
                        // Name the two surfaces ourselves rather than rely on what the driver left:
                        // (pitch in 8-pixel units << 21) | (offset in video memory >> 5); the 64 MB
                        // aperture shows video memory twice, so offsets are taken modulo 32 MB.
                        const uint32_t screenAt = static_cast<uint32_t>(gScreen - rage.frameBuffer) & 0x1FFFFFFu;
                        const uint32_t cardAt = static_cast<uint32_t>(cardBase - rage.frameBuffer) & 0x1FFFFFFu;
                        if (rage.frameBuffer == nullptr || (screenAt & 31) != 0 || (cardAt & 31) != 0 || (gStride & 7) != 0
                            || (cardRow & 7) != 0)
                        {
                            out += "\"engine\":\"surfaces do not fit the engine's units\",";
                            return;
                        }
                        rage.Write(0x1000, (1u << 16) | (kSrcPitchOffset >> 2));
                        rage.Write(0x1004, (static_cast<uint32_t>(cardRow / 8) << 21) | (cardAt >> 5));
                        rage.Write(0x1000, (static_cast<uint32_t>(gStride / 8) << 21) | (screenAt >> 5));
                        rage.Write(0x1004, 0x80000000u); // filler packet: keeps the stream in pairs
                        rage.Write(0x1000, (0u << 16) | (kDpCntl >> 2));
                        rage.Write(0x1004, 3); // left to right, top to bottom
                        const int count = stage == 3 ? 1 : 1000;
                        uint32_t seed = 12345;
                        int lastX = 500, lastY = 300;
                        bool stuck = false;
                        const uint64_t start = ClassicHost::Microseconds();
                        for (int i = 0; i < count && !stuck; i++)
                        {
                            if ((i & 3) == 0)
                            {
                                long spin = 0;
                                while (fifoFree() < 16)
                                    if (++spin > 20000000)
                                    {
                                        stuck = true;
                                        break;
                                    }
                                if (stuck)
                                    break;
                            }
                            if (count > 1)
                            {
                                seed = seed * 1103515245u + 12345u;
                                lastX = static_cast<int>((seed >> 8) % (gWidth - size.width + 1));
                                lastY = 40 + static_cast<int>((seed >> 20) % 480);
                            }
                            rage.Write(0x1000, (2u << 16) | (kSrcYX >> 2));
                            rage.Write(0x1004, 0); // source y, x in the off-screen world
                            rage.Write(0x1000, (static_cast<uint32_t>(lastY) << 16) | static_cast<uint32_t>(lastX));
                            rage.Write(0x1004, (static_cast<uint32_t>(size.height) << 16) | static_cast<uint32_t>(size.width));
                        }
                        const uint64_t submitted = ClassicHost::Microseconds();
                        const bool idle = idleNow();
                        const uint64_t done = ClassicHost::Microseconds();
                        int same = 0, checked = 0;
                        for (int y = 0; y < size.height; y++)
                            for (int x = 0; x < size.width; x++)
                            {
                                const uint8_t wanted = cardBase[y * cardRow + x];
                                if (wanted == 0 && mode == 1)
                                    continue;
                                checked++;
                                if (gScreen[static_cast<size_t>(lastY + y) * gStride + lastX + x] == wanted)
                                    same++;
                            }
                        char text[300];
                        std::snprintf(text, sizeof(text),
                            "\"packets_%s_%dx%d\":{\"blits\":%d,\"us_submit\":%.2f,\"us_done\":%.2f,\"idle\":%s,\"stuck\":%s,"
                            "\"pixels_right\":%d,\"pixels_checked\":%d,\"pm4_stat\":%lu},",
                            mode == 0 ? "opaque" : "transparent", size.width, size.height, count,
                            static_cast<double>(submitted - start) / count, static_cast<double>(done - start) / count,
                            idle ? "true" : "false", stuck ? "true" : "false", same, checked,
                            static_cast<unsigned long>(rage.Read(0x07B8)));
                        out += text;
                        if (stage == 3)
                            return; // one blit only
                        continue;
                    }
                    if (!WaitIdle(rage))
                    {
                        out += "\"engine\":\"engine never went idle\",";
                        return;
                    }
                    const uint32_t source = rage.Read(kSrcYX);
                    // Where the driver put the source: registers hold the position after the copy,
                    // so take the start from a fresh copy's destination convention instead.
                    const uint32_t sourceStart = source - (static_cast<uint32_t>(size.height) << 16); // y advanced by height
                    // Surfaces: destination is the screen, source the off-screen world, each as
                    // (pitch in 8-pixel units << 21) | (byte offset in video memory >> 5).
                    const int cardRowBytes = (**GetGWorldPixMap(cardWorld)).rowBytes & 0x3FFF;
                    // The 64 MB aperture shows video memory twice (the upper half is the byte-swapped
                    // view the Mac uses); the engine counts from the start of video memory.
                    const uint32_t screenOffset = static_cast<uint32_t>(gScreen - rage.frameBuffer) & 0x1FFFFFFu;
                    const uint32_t cardOffset = static_cast<uint32_t>(cardBase - rage.frameBuffer) & 0x1FFFFFFu;
                    const uint32_t cardX = cardOffset & 31;
                    {
                        char text[220];
                        std::snprintf(text, sizeof(text),
                            "\"surfaces\":{\"screen_offset\":%lu,\"screen_stride\":%d,\"card_offset\":%lu,\"card_stride\":%d},",
                            static_cast<unsigned long>(screenOffset), gStride, static_cast<unsigned long>(cardOffset), cardRowBytes);
                        if (out.find("\"surfaces\"") == std::string::npos)
                            out += text;
                    }
                    if (rage.frameBuffer == nullptr || (gStride & 7) != 0 || (cardRowBytes & 7) != 0 || (screenOffset & 31) != 0)
                    {
                        out += "\"engine\":\"surfaces do not fit the engine's pitch/offset units\",";
                        return;
                    }
                    rage.Write(kDstPitchOffset, (static_cast<uint32_t>(gStride / 8) << 21) | (screenOffset >> 5));
                    rage.Write(kSrcPitchOffset, (static_cast<uint32_t>(cardRowBytes / 8) << 21) | (cardOffset >> 5));
                    rage.Write(kScTopLeft, 0);
                    rage.Write(kScBottomRight, 0x1FFF1FFFu);
                    rage.Write(kDpCntl, 3); // left to right, top to bottom
                    gProbeSourceMode = 1;
                    gProbeSourceValue = cardX;
                    const int count = 1000;
                    uint32_t seed = 12345;
                    int lastX = 0, lastY = 0;
                    bool stuck = false;
                    const uint64_t start = ClassicHost::Microseconds();
                    for (int i = 0; i < count && !stuck; i++)
                    {
                        if ((i & 7) == 0)
                        {
                            long spin = 0;
                            while ((rage.Read(kGuiStat) & 0xFFFu) < 32)
                                if (++spin > 20000000)
                                {
                                    stuck = true;
                                    break;
                                }
                        }
                        seed = seed * 1103515245u + 12345u;
                        lastX = static_cast<int>((seed >> 8) % (gWidth - size.width + 1));
                        lastY = 40 + static_cast<int>((seed >> 20) % 480);
                        rage.Write(kSrcYX, gProbeSourceMode == 0 ? sourceStart : gProbeSourceValue);
                        rage.Write(kDstYX, (static_cast<uint32_t>(lastY) << 16) | static_cast<uint32_t>(lastX));
                        rage.Write(kDstHeightWidth, (static_cast<uint32_t>(size.height) << 16) | static_cast<uint32_t>(size.width));
                    }
                    const uint64_t submitted = ClassicHost::Microseconds();
                    const bool idle = WaitIdle(rage);
                    const uint64_t done = ClassicHost::Microseconds();
                    // Did the last copy arrive? Compare the screen with the pattern in the world.
                    int same = 0, opaque = 0;
                    const int cardRow = (**GetGWorldPixMap(cardWorld)).rowBytes & 0x3FFF;
                    for (int y = 0; y < size.height; y++)
                        for (int x = 0; x < size.width; x++)
                        {
                            const uint8_t wanted = cardBase[y * cardRow + x];
                            if (wanted == 0 && mode == 1)
                                continue;
                            opaque++;
                            if (gScreen[static_cast<size_t>(lastY + y) * gStride + lastX + x] == wanted)
                                same++;
                        }
                    char text[300];
                    std::snprintf(text, sizeof(text),
                        "\"engine_%s_%dx%d\":{\"us_submit\":%.2f,\"us_done\":%.2f,\"idle\":%s,\"stuck\":%s,\"pixels_right\":%d,"
                        "\"pixels_checked\":%d,\"source_after\":%lu},",
                        mode == 0 ? "opaque" : "transparent", size.width, size.height,
                        static_cast<double>(submitted - start) / count, static_cast<double>(done - start) / count,
                        idle ? "true" : "false", stuck ? "true" : "false", same, opaque, static_cast<unsigned long>(source));
                    out += text;
                }
            }
        }
    } // namespace

    // --- Colour table written directly ---------------------------------------------------
    namespace
    {
        Rage128 gPaletteCard;
        uint8_t gGammaTable[3][256];

        void DirectPaletteSet(int first, int last, const Colour* colours)
        {
            *reinterpret_cast<volatile uint8_t*>(gPaletteCard.base + 0x00B0) = static_cast<uint8_t>(first);
            __asm__ volatile("eieio");
            for (int i = first; i <= last; i++)
                gPaletteCard.Write(0x00B4, (static_cast<uint32_t>(gGammaTable[0][colours[i].red]) << 16)
                    | (static_cast<uint32_t>(gGammaTable[1][colours[i].green]) << 8) | gGammaTable[2][colours[i].blue]);
        }
    } // namespace

    // Finds the card and the display's gamma table, then checks the prediction "colour table
    // entry = gamma[colour]" against what the driver itself loaded for all 256 current colours.
    // Only an exact match turns the direct route on.
    std::string PrepareDirectPalette(bool enable, bool fullRange)
    {
        if (!NativeVSync::Drain())
            return "{\"enabled\":false,\"reason\":\"pending display job timed out\"}";
        gDirectPaletteSet = nullptr;
        gDirectPaletteFull = fullRange;
        std::string out = "{";
        if (!gOpen || gDirect32 || !gShownValid)
            return "{\"enabled\":false,\"reason\":\"needs the 256-colour screen with a palette set\"}";
        if (!FindRage128(gPaletteCard, out))
            return out + "\"enabled\":false,\"reason\":\"no Rage 128\"}";
        for (int channel = 0; channel < 3; channel++)
            for (int i = 0; i < 256; i++)
                gGammaTable[channel][i] = static_cast<uint8_t>(i);
        VDGammaRecord record{};
        CntrlParam block{};
        block.ioCRefNum = (**gDevice).gdRefNum;
        block.csCode = cscGetGamma;
        *reinterpret_cast<Ptr*>(&block.csParam[0]) = reinterpret_cast<Ptr>(&record);
        const OSErr gammaError = PBStatusSync(reinterpret_cast<ParmBlkPtr>(&block));
        int channels = 0, entries = 0, width = 0;
        if (gammaError == noErr && record.csGTable != nullptr)
        {
            const auto* table = reinterpret_cast<const GammaTbl*>(record.csGTable);
            channels = table->gChanCnt;
            entries = table->gDataCnt;
            width = table->gDataWidth;
            const auto* data = reinterpret_cast<const uint8_t*>(table->gFormulaData) + table->gFormulaSize;
            if (entries == 256 && width == 8 && (channels == 1 || channels == 3))
                for (int channel = 0; channel < 3; channel++)
                    for (int i = 0; i < 256; i++)
                        gGammaTable[channel][i] = data[(channels == 3 ? channel * 256 : 0) + i];
        }
        const uint32_t dacControl = gPaletteCard.Read(0x0058);
        int wrong = 0, firstWrong = -1;
        uint32_t firstWrongValue = 0;
        char map[257]{};
        std::string samples;
        gPaletteCard.Write(0x00B0, 0u << 16); // read index 0; reads advance it
        for (int i = 0; i < 256; i++)
        {
            const uint32_t value = gPaletteCard.Read(0x00B4) & 0xFFFFFFu;
            const uint32_t expected = (static_cast<uint32_t>(gGammaTable[0][gShown[i].red]) << 16)
                | (static_cast<uint32_t>(gGammaTable[1][gShown[i].green]) << 8) | gGammaTable[2][gShown[i].blue];
            map[i] = value != expected ? 'x' : '.';
            if (i % 37 == 16)
            {
                char sample[80];
                std::snprintf(sample, sizeof(sample), "[%d,%d,%d,%d,%lu,%lu],", i, gShown[i].red, gShown[i].green, gShown[i].blue,
                    static_cast<unsigned long>(value), static_cast<unsigned long>(expected));
                samples += sample;
            }
            if (value != expected)
            {
                if (wrong++ == 0)
                {
                    firstWrong = i;
                    firstWrongValue = value;
                }
            }
        }
        char text[400];
        std::snprintf(text, sizeof(text),
            " \"gamma\":{\"error\":%d,\"channels\":%d,\"entries\":%d,\"width\":%d},\"dac_cntl\":%lu,\"entries_wrong\":%d,"
            "\"first_wrong\":%d,\"first_wrong_card\":%lu,\"first_wrong_colour\":[%d,%d,%d],\"enabled\":%s}",
            static_cast<int>(gammaError), channels, entries, width, static_cast<unsigned long>(dacControl), wrong, firstWrong,
            static_cast<unsigned long>(firstWrongValue), firstWrong >= 0 ? gShown[firstWrong].red : 0,
            firstWrong >= 0 ? gShown[firstWrong].green : 0, firstWrong >= 0 ? gShown[firstWrong].blue : 0,
            wrong == 0 && enable ? "true" : "false");
        out += "\"map\":\"" + std::string(map) + "\",\"samples\":[" + samples + "[0]],";
        // Counters are read outside the timed interval; no per-picture logging.
        out += "\"full\":" + std::string(gDirectPaletteFull ? "true" : "false")
            + ",\"direct_sets\":" + std::to_string(gDirectPaletteSets)
            + ",\"driver_sets\":" + std::to_string(gDriverPaletteSets)
            + ",\"broad_sets\":" + std::to_string(gBroadPaletteSets) + ",";
        out += text + 1; // text starts with a quote-less field list after the brace
        if (wrong == 0 && enable)
            gDirectPaletteSet = DirectPaletteSet;
        return out;
    }

    // Times small blits on the live screen: QuickDraw CopyBits through the video driver from the
    // screen itself, from an off-screen world in main memory and from one in video memory (if the
    // driver grants it), opaque and with a transparent colour, against plain processor copies of
    // the same size. Answers whether the card could draw the game's sprites faster than we do.
    #include "RageCopyProbe.h"
    #include "NativeVSync.h"

    namespace
    {
        Rage128 gCursorCard;
        bool gCursorCardReady = false;
        void SampleHardwarePointer(unsigned phase)
        {
            if (!gCursorProbe || !gCursorCardReady || !gPointerVisible || gSuspended) return;
            // Read-only: ATI/X.org CRTC_GEN_CNTL.CRTC_CUR_EN and cursor position.
            const auto control = gCursorCard.Read(0x0050);
            ++gCursorSamples[phase];
            if (!(control & (1u << 16))) ++gCursorOffSamples[phase];
            const auto position = gCursorCard.Read(0x0264);
            gCursorLastPosition[phase] = position;
            // The ATI driver hides its overlay by moving it outside the raster,
            // while leaving CUR_EN set. Both forms of disappearance matter.
            if ((position >> 16) >= unsigned(gWidth) || (position & 0xffffu) >= unsigned(gHeight))
                ++gCursorOffscreenSamples[phase];
            if (position != gCursorPosition) { ++gCursorPositionChanges; gCursorPosition = position; }
        }
        nlohmann::json ConfigureCursorProbe(const nlohmann::json& c)
        {
            if (c.contains("protect_hardware")) gProtectHardwarePointer = c["protect_hardware"].get<bool>();
            if (c.value("reset",false))
            {
                std::fill(std::begin(gCursorSamples),std::end(gCursorSamples),0);
                std::fill(std::begin(gCursorOffSamples),std::end(gCursorOffSamples),0);
                std::fill(std::begin(gCursorOffscreenSamples),std::end(gCursorOffscreenSamples),0);
                std::fill(std::begin(gCursorLastPosition),std::end(gCursorLastPosition),0);
                gCursorPositionChanges = 0; gCursorHiddenUs = gCursorHiddenMaxUs = gCursorHideStart = 0;
                gCursorProbeFrames = 0;
            }
            if (c.contains("probe"))
            {
                gCursorProbe = c["probe"].get<bool>();
                if (gCursorProbe && !gCursorCardReady) { std::string registry; gCursorCardReady = FindRage128(gCursorCard,registry); }
            }
            if (c.contains("capture_frames")) gCursorProbeLimit = std::clamp(c["capture_frames"].get<int>(),1,1000);
            if (c.contains("motion"))
            {
                const auto& m = c["motion"];
                gCursorMotion = {std::clamp(m.value("x0",320),0,gWidth-1),std::clamp(m.value("y0",240),0,gHeight-1),
                    std::clamp(m.value("x1",400),0,gWidth-1),std::clamp(m.value("y1",260),0,gHeight-1),
                    unsigned(std::clamp(m.value("frames",240),0,1000)),0};
            }
            RefreshHardwarePointer();
            // A passive resident sampler also observes ordinary sync-off copies.
            if (!NativeVSync::active && NativeVSync::hardwareReady)
            {
                if (gCursorProbe && gHardwarePointer) NativeVSync::BlankCapability(true,true);
                else if (NativeVSync::blankProbe.installed) NativeVSync::BlankCapability(false,false);
            }
            NativeVSync::CursorWatch(gCursorProbe,gPointerVisible && !gSuspended,c.value("reset",false));
            const auto blank = NativeVSync::CursorWatchStats();
            nlohmann::json result{{"enabled",gCursorProbe},{"frames",gCursorProbeFrames},
                {"hardware_active",gHardwarePointer},{"protect_hardware",gProtectHardwarePointer},
                {"support_error",gCursorSupportError},{"state_error",gCursorStateError},
                {"driver",{gHardwarePointerState.csCursorSet,gHardwarePointerState.csCursorVisible,
                    gHardwarePointerState.csCursorX,gHardwarePointerState.csCursorY}},
                {"requested_visible",gPointerVisible},{"mmio",gCursorCardReady},
                {"samples",gCursorSamples},{"off_samples",gCursorOffSamples},
                {"offscreen_samples",gCursorOffscreenSamples},{"last_positions",gCursorLastPosition},
                {"blank_samples",blank},{"position_changes",gCursorPositionChanges},
                {"hidden_us",gCursorHiddenUs},{"max_hidden_us",gCursorHiddenMaxUs},
                {"motion_frames",gCursorMotion.at}};
            if (gCursorCardReady)
            {
                result["control"] = gCursorCard.Read(0x0050);
                result["cursor_offset"] = gCursorCard.Read(0x0260);
                result["position"] = gCursorCard.Read(0x0264);
            }
            return result;
        }
    }

    std::string BlitProbe(int engineStage, int sourceMode, uint32_t sourceValue)
    {
        if (engineStage >= 5)
            return RageCopyProbe(engineStage, sourceMode, sourceValue);
        gProbeSourceMode = sourceMode;
        gProbeSourceValue = sourceValue;
        if (!gOpen || gWindow == nullptr || gDirect32)
            return "{\"error\":\"needs the 256-colour screen\"}";
        std::string out = "{";
        GrafPtr saved;
        GetPort(&saved);
        const auto port = ScrollPort();
        SetPort(reinterpret_cast<GrafPtr>(port));
        port->fgColor = 255;
        port->bkColor = 0;
        port->rgbFgColor = { 0, 0, 0 };
        port->rgbBkColor = { 0xffff, 0xffff, 0xffff };
        HideNative(CursorCause::verify);

        Rect worldRect{ 0, 0, 256, 128 };
        const CTabHandle colours = (**(**gDevice).gdPMap).pmTable;
        GWorldPtr worlds[2] = { nullptr, nullptr };
        const QDErr errors[2] = { NewGWorld(&worlds[0], 8, &worldRect, colours, nullptr, keepLocal),
                                  NewGWorld(&worlds[1], 8, &worldRect, colours, nullptr, useDistantHdwrMem) };
        char text[512];
        for (int w = 0; w < 2; w++)
        {
            bool inCard = false;
            if (errors[w] == noErr && worlds[w] != nullptr)
            {
                const PixMapHandle pixels = GetGWorldPixMap(worlds[w]);
                LockPixels(pixels);
                auto* base = reinterpret_cast<uint8_t*>(GetPixBaseAddr(pixels));
                const int rowBytes = (**pixels).rowBytes & 0x3FFF;
                // A sprite-like pattern: about 40% transparent (index 0).
                for (int y = 0; y < 256; y++)
                    for (int x = 0; x < 128; x++)
                        base[y * rowBytes + x] = ((x * 7 + y * 13) % 10) < 4 ? 0 : static_cast<uint8_t>(16 + ((x + y) & 127));
                const auto distance = base > gScreen ? base - gScreen : gScreen - base;
                inCard = distance < 16 * 1024 * 1024;
            }
            std::snprintf(text, sizeof(text), "\"%s_world\":{\"error\":%d,\"in_video_memory\":%s},",
                w == 0 ? "ram" : "card", static_cast<int>(errors[w]), inCard ? "true" : "false");
            out += text;
        }

        const BitMap* screenBits = &reinterpret_cast<GrafPtr>(port)->portBits;
        struct Size
        {
            int width, height;
        };
        const Size sizes[] = { { 16, 24 }, { 64, 32 } };
        const int count = 1000;
        const char* const sourceNames[] = { "screen", "ram", "card" };
        for (const auto& size : sizes)
        {
            for (int source = 0; source < 3; source++)
            {
                const BitMap* bits = screenBits;
                Rect from{ 200, 300, static_cast<short>(200 + size.height), static_cast<short>(300 + size.width) };
                if (source > 0)
                {
                    if (errors[source - 1] != noErr || worlds[source - 1] == nullptr)
                        continue;
                    bits = reinterpret_cast<const BitMap*>(*GetGWorldPixMap(worlds[source - 1]));
                    from = Rect{ 0, 0, static_cast<short>(size.height), static_cast<short>(size.width) };
                }
                for (int mode = 0; mode < 2; mode++)
                {
                    uint32_t seed = 12345;
                    const uint64_t start = ClassicHost::Microseconds();
                    for (int i = 0; i < count; i++)
                    {
                        seed = seed * 1103515245u + 12345u;
                        const short x = static_cast<short>((seed >> 8) % (gWidth - size.width + 1)), y = static_cast<short>(40 + (seed >> 20) % (gHeight - 40 - size.height + 1));
                        Rect to{ y, x, static_cast<short>(y + size.height), static_cast<short>(x + size.width) };
                        CopyBits(bits, screenBits, &from, &to, mode == 0 ? srcCopy : transparent, nullptr);
                    }
                    const uint64_t returned = ClassicHost::Microseconds();
                    while (!QDDone(reinterpret_cast<GrafPtr>(port)))
                    {
                    }
                    const uint64_t done = ClassicHost::Microseconds();
                    std::snprintf(text, sizeof(text), "\"copybits_%s_%s_%dx%d\":{\"us_call\":%.2f,\"us_done\":%.2f},",
                        sourceNames[source], mode == 0 ? "opaque" : "transparent", size.width, size.height,
                        static_cast<double>(returned - start) / count, static_cast<double>(done - start) / count);
                    out += text;
                }
            }
            // The same amount of work by the processor, straight into video memory.
            static uint8_t sprite[64 * 32];
            for (int i = 0; i < 64 * 32; i++)
                sprite[i] = ((i * 7) % 10) < 4 ? 0 : static_cast<uint8_t>(16 + (i & 127));
            for (int mode = 0; mode < 2; mode++)
            {
                uint32_t seed = 12345;
                const uint64_t start = ClassicHost::Microseconds();
                for (int i = 0; i < count; i++)
                {
                    seed = seed * 1103515245u + 12345u;
                    const int x = (seed >> 8) % (gWidth - size.width + 1), y = 40 + (seed >> 20) % (gHeight - 40 - size.height + 1);
                    for (int row = 0; row < size.height; row++)
                    {
                        uint8_t* to = gScreen + static_cast<size_t>(y + row) * gStride + x;
                        const uint8_t* from = sprite + row * 64;
                        if (mode == 0)
                            CopyRow(to, from, size.width);
                        else
                            for (int column = 0; column < size.width; column++)
                                if (from[column] != 0)
                                    to[column] = from[column];
                    }
                }
                std::snprintf(text, sizeof(text), "\"processor_%s_%dx%d\":{\"us_done\":%.2f},",
                    mode == 0 ? "opaque" : "transparent", size.width, size.height,
                    static_cast<double>(ClassicHost::Microseconds() - start) / count);
                out += text;
            }
        }
        if (engineStage > 0 && errors[1] == noErr && worlds[1] != nullptr)
            EngineProbe(out, engineStage, port, screenBits, worlds[1]);
        for (auto* world : worlds)
            if (world != nullptr)
                DisposeGWorld(world);
        ShowNative();
        SetPort(saved);
        gRecolour = true; // repaint the whole frame over the test pattern
        out += "\"blits_per_case\":1000}";
        return out;
    }

    void MoveMemory(void* to, const void* from, size_t bytes)
    {
        BlockMoveData(from, to, static_cast<Size>(bytes));
    }

    bool WarpPointer(int x, int y)
    {
        // The classic way to place the pointer: write the low-memory mouse positions and flag them new.
        Point point{ static_cast<short>(y), static_cast<short>(x) };
        LMSetRawMouseLocation(point);
        LMSetMouseTemp(point);
        LMSetCursorNew(-1);
        gLastMouse = point;
        return true;
    }

    void Message(const char* text)
    {
        std::printf("message: %s\n", text);
    }
} // namespace ShimHost
#endif
