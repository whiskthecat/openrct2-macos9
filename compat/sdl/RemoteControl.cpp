#include <openrct2/paint/PerfMetrics.h>
#include "../../tools/RideStartRegression.h"
#include "../../tools/AudioWorkProbe.h"
// Remote control of the running game: one JSON command in, one JSON reply out.
// On the Mac the commands arrive as AgentChat application commands (Apple
// events), so the physical machine can be driven and measured from the
// development host; on a development host they come from a script file.
//
// Coordinate contract: mouse/drag/inspect use physical display pixels (same as native SDL events).
// pick/options/openride/viewport rects and paint "rects" use logical canvas pixels.
// view/pan use projected world pixels, before viewport zoom; focus uses map/world coordinates.
// Display pixels sample canvas floor(pixel / window_scale); a canvas edge maps to ceil(edge * scale).
//   {"op":"status"}                          park, date, speed, view, profile since the last mark
//   {"op":"profile","pan":true}              frozen pan profile only, within the native reply limit
//   {"op":"status","mark":true}              ... and start a new profile interval
//   {"op":"mouse","x":400,"y":300,"action":"click","button":"left"}   action: move|down|up|click
//   {"op":"drag","x":..,"y":..,"x2":..,"y2":..,"button":"right","frames":20}
//   {"op":"key","key":"Escape"}              SDL key name or one character; "mods":["shift","ctrl","alt","gui"]
//   {"op":"text","text":"hello"}             typed into the focused text box
//   {"op":"pan","dx":8,"dy":4,"frames":200}  scroll the main view each frame; result appears in status as "pan"
//   {"op":"view","x":..,"y":..,"zoom":1,"rotation":1}  set/read the main view
//   {"op":"pick","x":..,"y":..,"width":1,"height":1,"step":1}  grid of sprite/terrain picks
//   {"op":"rotate","direction":1}           rotate all linked viewports
//   {"op":"focus","x":..,"y":..,"z":..}     centre the main view on a map location
//   {"op":"rides"}                           ride locations and integral vehicle animation state
//   {"op":"openride","id":33}                open a ride window; rotate accepts "ride":33
//   {"op":"trackdesign","path":"/RCT/user/maze.td6"}  open the real design placement tool
//   {"op":"options"}                         open the display options (returns window position)
//   {"op":"uiwindows","class":18,"widgets":true}  logical window/widget geometry for layout fixtures
//   {"op":"uiopen","class":18}                open through the normal window manager
//   {"op":"uiwidget","class":18,"widget":15}  queue real input at a named widget (hover:true only moves)
//   {"op":"uimove","class":18,"x":0,"y":30,"snap":5}  ordinary clamped/snap movement
//   {"op":"uiconstruct","ride":0,"new":true}  construction fixture through the normal ride action
//   {"op":"uidetail","detail":2,"id":0}       context-specific window through openDetails
//   {"op":"uitext"}, {"op":"uierror"}          transient dialog fixtures; no callbacks or file writes
//   {"op":"balloon","x":..,"y":..,"z":..}   create a balloon (test fixture only)
//   {"op":"console","line":"get money"}      the game's own console; output is returned
//   {"op":"load","path":"/RCT/game/Assets/forest frontiers.sea"}
//   {"op":"speed","value":1}                 1 normal .. 4 fastest; {"op":"pause","value":true}
//   {"op":"cells","width":32,"height":16}    size of the change-tracking cells (experiment)
//   {"op":"cache","on":false}                 main-view column cache on/off; {"op":"redraw"} repaints everything
//   {"op":"hidefar","mode":"auto"}           hide entities two steps out
//   {"op":"close"}                            close all floating windows
//   {"op":"seed","util":123}                  fixed scenario seed; optional utility RNG seed
//   {"op":"hwscroll","on":false}             view scrolling by the video card on/off
//   {"op":"probes","on":true}               start scope capture; on:false stops and reports exclusive tick shares
//   {"op":"sprites"}                          per-frame sprite counts, sizes and time by kind since the last call
//   {"op":"blitprobe"}                        Mac: time small QuickDraw blits on the live screen
//   {"op":"dacpalette","on":true,"full":true} Mac: verify direct palette writes (full:false compares the old small-range policy)
//   {"op":"sound","on":false}                 stop all sound / resume (to measure what sound costs)
//   {"op":"weather","type":4}                 force the weather (4 = heavy rain, 0 = sunny)
//   {"op":"moveinto","path":"Disk:Item","folder":"Disk:Folder"}   Mac: move a file or folder
//   {"op":"rects"}                            what the last frame sent to the screen (scrolls, rectangles)
//   {"op":"quit"}
extern bool gPresentChangedOnly;
extern int gPresentChangedState;
extern "C" unsigned long ClassicAllocatorBytes(void);
extern "C" int ClassicAllocatorLargeBlocks(unsigned long*, unsigned long*, int, unsigned long*);
extern "C" int ClassicAllocatorClasses(unsigned long*, unsigned long*, unsigned long*, int);
extern "C" unsigned long ClassicAllocatorLargeCalls(void);
#if defined(__linux__)
extern "C" void ColdAllocationStats(uint64_t*) __attribute__((weak));
#endif
#include "ShimHost.h"
#include "FrameBench.h"
#include "BuildIdentity.h"
#ifdef OPENRCT2_PAK_VFS
    #include "ClassicAudio.h"
    #include "ClassicAutosave.h"
    #include "ClassicVfs.h"
    #include "ClassicDiskIo.h"
#endif

#include <SDL.h>
#include <openrct2-ui/interface/Window.h>
#include <openrct2-ui/interface/Widget.h>
#include <openrct2-ui/windows/Windows.h>
#include <openrct2-ui/windows/OriginalHud.h>
#ifndef OPENRCT2_CLASSIC_MAC
#include <openrct2/scenes/SceneManager.h>
#endif
#include <openrct2-ui/interface/Theme.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <nlohmann/json.hpp>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/footpath/FootpathPlaceAction.h>
#include <openrct2/world/Map.h>
#include <openrct2/audio/AudioChannel.h>
#include <openrct2/audio/AudioContext.h>
#include <openrct2/audio/AudioMixer.h>
#include <openrct2/audio/AudioSource.h>
#include <openrct2/core/MemoryStream.h>
#include <bit>
#include <openrct2/interface/Screenshot.h>
#include <openrct2/drawing/NewDrawing.h>
#include <openrct2/GameState.h>
#include <openrct2/object/Object.h>
#include <openrct2/world/Banner.h>
#include <openrct2/world/tile_element/BannerElement.h>
#include <openrct2/object/BannerSceneryEntry.h>
#include <openrct2/object/ObjectEntryManager.h>
#include <openrct2/Input.h>
#include <openrct2-ui/interface/ViewportInteraction.h>
#include <openrct2/audio/Audio.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/File.h>
#include <openrct2/core/ScopeProbe.h>
#include <openrct2/core/FrameTiming.h>
#include <openrct2/paint/Paint.Timing.h>
#include <openrct2/world/Weather.h>
#include <openrct2/drawing/Drawing.Screen.h>
#include <openrct2/drawing/Drawing.Sprite.h>
#include <openrct2/drawing/SpriteRasterCache.h>
#include <openrct2/drawing/G1Element.h>
#include <openrct2/drawing/Colour.h>
#include <openrct2/drawing/ImageImporter.h>
#include <openrct2/drawing/IDrawingContext.h>
#include <openrct2/drawing/IDrawingEngine.h>
#include <openrct2/entity/Balloon.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/entity/EntityTweener.h>
#include <openrct2/entity/Staff.h>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/EntityInvalidation.h>
#include <openrct2/paint/Paint.Cache.h>
#include <openrct2/drawing/ScrollingText.h>
#include <openrct2/interface/InteractiveConsole.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/interface/Window.h>
#include <openrct2/interface/WindowBase.h>
#include <openrct2/interface/Widget.h>
#include <openrct2/localisation/Language.h>
#include <openrct2/paint/Paint.h>
#include <openrct2/ride/RideManager.hpp>
#include <openrct2/ride/RideConstruction.h>
#include <openrct2-ui/ride/Construction.h>
#ifndef OPENRCT2_CLASSIC_MAC
#include "../../tools/ScenesFixture.h"
#endif
#include <openrct2/ride/RideTypes.h>
#include <openrct2/ride/TrackDesignRepository.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/ride/TrainManager.h>
#include <openrct2/scenario/Scenario.h>
#include <openrct2/scenario/ScenarioRepository.h>
#include <openrct2/ui/WindowManager.h>
#include <openrct2/ui/UiContext.h>
#include <openrct2/world/Park.h>
#include <openrct2/world/Map.h>
#include <openrct2/util/Util.h>
#include <openrct2/audio/AudioChannel.h>
#include <openrct2/object/PeepAnimationsObject.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/object/WallObject.h>
#include <openrct2/world/tile_element/WallElement.h>
#include <openrct2/world/tile_element/TileElement.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
#include <string>
#ifndef MACINTOSH
    #include "../../tools/PlacementFixture.h"
    #include "../../tools/ColdHistoryFixture.h"
#endif

#ifdef OPENRCT2_PERF_METRICS
namespace OpenRCT2::Ui::Windows { std::array<uint64_t, 4> MainHudOcclusionMetrics(bool reset); }
#endif

using namespace OpenRCT2;
namespace OpenRCT2 { extern bool gPaintEntranceOverlays; }
namespace OpenRCT2::Drawing { extern bool gPaletteFilterUnroll; }
using json = nlohmann::json;
#ifdef OPENRCT2_RIDEFPS_FIXTURES
json RideFpsFixture(const json& command);
#endif

namespace
{
    struct Step
    {
        enum class Kind { move, down, up, key, keyDown, keyUp, text } kind;
        int x = 0, y = 0, button = SDL_BUTTON_LEFT;
        SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
        Uint16 mods = 0;
        char character = 0;
    };
    std::deque<Step> gSteps;
    int gMouseX = 0, gMouseY = 0;
    uint32_t gButtons = 0;

    struct Pan
    {
        int dx = 0, dy = 0, framesLeft = 0, frames = 0;
        bool aligned = false, startPending = false;
        int warmFramesLeft = 0;
        uint32_t firstTick = 0;
        ShimHost::ProfileTotals start{};
    } gPan;
    // Retain diagnostic loops through Context teardown, so quit really disposes playing buffers.
    std::vector<std::shared_ptr<Audio::IAudioChannel>> gAudioTestLoops;
    json gPanResult;
    ShimHost::ProfileTotals gMark{};
    ShimHost::ProfileTotals gProbeStart{};

#ifdef OPENRCT2_PERF_METRICS
    json gPanPerfStart, gPanPerfResult;
    json PerfCapture()
    {
        return {{"frames",Perf::tracedFrames}, {"dirty_calls",Perf::dirtyCalls}, {"dirty_pixels",Perf::dirtyPixels},
            {"guest_changes",Perf::guestChanges}, {"sprite_calls",Perf::spriteCalls}, {"sprite_boxes",Perf::spriteBoxes},
            {"sprite_ticks",Perf::spriteTicks}, {"writes",Perf::writes}, {"canvas_writes",Perf::canvasWrites},
            {"repeated_writes",Perf::repeatedWrites}, {"scroll_windows",Perf::scrollWindows},
            {"scroll_submit_us",ShimHost::gScreenScrollMicros}, {"scroll_wait_us",ShimHost::gScreenScrollWaitMicros}};
    }
    json PerfDifference(const json& before)
    {
        auto result = PerfCapture();
        for (auto& item : result.items())
            if (item.value().is_array())
                for (size_t i=0; i<item.value().size(); ++i)
                    item.value()[i] = item.value()[i].get<uint64_t>() - before[item.key()][i].get<uint64_t>();
            else item.value() = item.value().get<uint64_t>() - before[item.key()].get<uint64_t>();
        return result;
    }
#endif

    json ProfileSince(const ShimHost::ProfileTotals& from)
    {
        const auto now = ShimHost::Profile();
        const double frames = static_cast<double>(now.frames - from.frames);
        const double ms = static_cast<double>(now.microseconds - from.microseconds) / 1000.0;
        json result;
        result["frames"] = now.frames - from.frames;
        result["seconds"] = ms / 1000.0;
        if (frames > 0 && ms > 0)
        {
            const double paint = (now.phase[0] - from.phase[0]) / 1000.0, present = (now.phase[1] - from.phase[1]) / 1000.0,
                         system = (now.phase[2] - from.phase[2]) / 1000.0;
            const auto round = [](double value) { return static_cast<long>(value * 1000.0 + 0.5) / 1000.0; };
            result["fps"] = round(frames * 1000.0 / ms);
            result["ticks_per_second"] = round((now.ticks - from.ticks) * 1000.0 / ms);
            result["paint_ms"] = round(paint / frames);
            result["present_ms"] = round(present / frames);
            result["system_ms"] = round(system / frames);
            result["game_ms"] = round((ms - paint - present - system) / frames);
            if (now.pump.calls != from.pump.calls)
            {
                const auto& p = now.pump;
                const auto& f = from.pump;
                auto perFrame = [&](uint64_t a, uint64_t b) { return round(double(a - b) / frames); };
                auto perFrameMs = [&](uint64_t a, uint64_t b) { return round(double(a - b) / 1000.0 / frames); };
                auto& pump = result["pump"];
                pump["calls"] = perFrame(p.calls, f.calls);
                pump["wait_calls"] = perFrame(p.waitCalls, f.waitCalls);
                pump["wait_ms"] = perFrameMs(p.waitMicros, f.waitMicros);
                pump["poll_calls"] = perFrame(p.pollCalls, f.pollCalls);
                pump["poll_ms"] = perFrameMs(p.pollMicros, f.pollMicros);
                pump["event_ms"] = perFrameMs(p.eventMicros, f.eventMicros);
                pump["input_ms"] = perFrameMs(p.inputMicros, f.inputMicros);
                pump["null_returns"] = perFrame(p.nullReturns, f.nullReturns);
                pump["intercepted_returns"] = perFrame(p.interceptedReturns, f.interceptedReturns);
                pump["events"] = json::array();
                pump["wait_bins"] = json::array();
                for (int i = 0; i < 24; i++) pump["events"].push_back(p.events[i] - f.events[i]);
                for (int i = 0; i < 6; i++) pump["wait_bins"].push_back(p.waitBins[i] - f.waitBins[i]);
                pump["audio_ms"] = perFrameMs(now.audioPumpMicros, from.audioPumpMicros);
            }
            result["lists_ms"] = round((now.world[0] - from.world[0]) / 1000.0 / frames);
            result["sort_ms"] = round((now.world[1] - from.world[1]) / 1000.0 / frames);
            result["draw_ms"] = round((now.world[2] - from.world[2]) / 1000.0 / frames);
            result["world_ms"] = round((now.world[3] - from.world[3]) / 1000.0 / frames);
            result["column_patches"] = round((now.patches - from.patches) / frames);
            result["column_builds"] = round((now.builds - from.builds) / frames);
            result["column_shifts"] = round((now.shifts - from.shifts) / frames);
            result["column_shift_tiles"] = round((now.shiftTiles - from.shiftTiles) / frames);
            result["column_shift_checks"] = now.shiftChecks - from.shiftChecks;
            result["tick_ms"] = round((now.loop[0] - from.loop[0]) / 1000.0 / frames);
            result["input_ms"] = round((now.loop[1] - from.loop[1]) / 1000.0 / frames);
            result["window_update_ms"] = round((now.loop[2] - from.loop[2]) / 1000.0 / frames);
            result["draw_total_ms"] = round((now.loop[3] - from.loop[3]) / 1000.0 / frames);
            result["widgets_ms"] = round((now.widgetMicros - from.widgetMicros) / 1000.0 / frames);
            result["widget_draws"] = round((now.widgetDraws - from.widgetDraws) / frames);
            result["shift_ms"] = round((now.shiftMicros - from.shiftMicros) / 1000.0 / frames);
            result["weather"] = {
                {"display_read_bytes", round((now.weather.displayReadBytes - from.weather.displayReadBytes) / frames)},
                {"display_write_bytes", round((now.weather.displayWriteBytes - from.weather.displayWriteBytes) / frames)},
                {"ram_read_bytes", round((now.weather.ramReadBytes - from.weather.ramReadBytes) / frames)},
                {"readback_bytes", round((now.weather.readbackBytes - from.weather.readbackBytes) / frames)}};
            result["palette_ms"] = round((now.paletteMicros - from.paletteMicros) / 1000.0 / frames);
            result["palette_sets"] = round((now.paletteSets - from.paletteSets) / frames);
            result["rects"] = round((now.rects - from.rects) / frames);
            result["columns"] = round((now.columns - from.columns) / frames);
            result["repaint_pixels"] = static_cast<long>((now.rectPixels - from.rectPixels) / frames);
            result["entity_entries"] = round((now.entityEntries - from.entityEntries) / frames);
            result["map_entries"] = round((now.mapEntries - from.mapEntries) / frames);
            result["nodes_collected"] = round((now.nodesCollected - from.nodesCollected) / frames);
            result["nodes_sorted"] = round((now.nodesSorted - from.nodesSorted) / frames);
            const char* costs[] = { "base_ms", "patch_picture_ms", "owner_ms", "check_ms", "local_filter_ms",
                "local_sort_ms", "tile_patch_ms", "entity_generate_ms" };
            const char* work[] = { "patch_hits", "patch_misses", "selection_hits", "selection_misses",
                "owner_passes", "owner_fallbacks", "owner_restores", "base_pixels",
                "picture_builds", "picture_repairs", "picture_deferrals", "picture_repair_deferrals", "picture_fallback_pixels" };
            for (int i = 0; i < 8; i++)
            {
                result["detail"][costs[i]] = round((now.paintCosts[i] - from.paintCosts[i]) / 1000.0 / frames);
            }
            for (int i = 0; i < 13; i++)
            {
                result["detail"][work[i]] = round((now.paintWork[i] - from.paintWork[i]) / frames);
            }
            result["detail"]["damage_ms"] = round((now.damageMicros - from.damageMicros) / 1000.0 / frames);
            result["detail"]["guest_pose_ms"] = round((now.guestPoseMicros - from.guestPoseMicros) / 1000.0 / frames);
            result["detail"]["guest_pose_visits"] = round((now.guestPoseVisits - from.guestPoseVisits) / frames);
            result["detail"]["entity_flush_ms"] = round((now.entityFlushMicros - from.entityFlushMicros) / 1000.0 / frames);
            result["detail"]["screen_scroll_ms"] = round((now.screenScrollMicros - from.screenScrollMicros) / 1000.0 / frames);
            result["detail"]["scroll_rects"] = round((now.scrollRects - from.scrollRects) / frames);
            result["detail"]["scroll_pixels"] = round((now.scrollPixels - from.scrollPixels) / frames);
            const char* sources[] = {"exposed_strip_pixels", "toolbar_pixels", "window_pixels"};
            for (int i = 0; i < 3; i++)
                result["detail"][sources[i]] = round((now.scrollSourcePixels[i] - from.scrollSourcePixels[i]) / frames);
            result["detail"]["geometric_strip_pixels"] = round((now.scrollGeometricPixels - from.scrollGeometricPixels) / frames);
            result["detail"]["entity_damage_pixels"] = round((now.entityDamagePixels - from.entityDamagePixels) / frames);
            result["detail"]["pointer_pixels"] = round((now.pointerPixels - from.pointerPixels) / frames);
            result["detail"]["column_build_ms"] = round((now.columnBuildMicros - from.columnBuildMicros) / 1000.0 / frames);
            result["detail"]["column_shift_ms"] = round((now.columnShiftMicros - from.columnShiftMicros) / 1000.0 / frames);
        }
        return result;
    }

    class CaptureConsole final : public InteractiveConsole
    {
    public:
        std::string output;
        void Clear() override
        {
        }
        void Close() override
        {
        }
        void Hide() override
        {
        }
        void WriteLine(const std::string& text, FormatToken) override
        {
            if (output.size() < 3000)
                output += text + "\n";
        }
    };

    void PushMouse(Uint32 type, int x, int y, int button)
    {
        SDL_Event event{};
        if (type == SDL_MOUSEMOTION)
        {
            event.type = type;
            event.motion.x = x;
            event.motion.y = y;
            event.motion.xrel = x - gMouseX;
            event.motion.yrel = y - gMouseY;
            event.motion.state = gButtons;
        }
        else
        {
            event.type = type;
            event.button.button = static_cast<Uint8>(button);
            event.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
            event.button.x = x;
            event.button.y = y;
        }
        ShimHost::PushEvent(event);
        gMouseX = x;
        gMouseY = y;
        ShimHost::RemoteMouse(x, y, gButtons);
    }

    int ButtonOf(const json& command)
    {
        return command.value("button", "left") == "right" ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
    }

    json FrameRecord(const FrameTiming::Frame& f)
    {
        return {{"frame", f.number}, {"presented_at_us", f.presentedAtUs}, {"yield", {f.yieldCalls, f.yieldUs, f.yieldMaxUs, f.coarseYields}}, {"event_mask", f.eventMask}, {"us", f.us}, {"median_us", f.medianUs}, {"baseline_us", f.baselineUs}, {"weather", f.weather}, {"tick", f.tick},
            {"ticks", f.ticks}, {"tick_calls", f.tickCalls}, {"stage_us", f.stage},
            {"paint_us", f.paintUs}, {"present_us", f.presentUs}, {"pump_us", f.pumpUs},
            {"columns", {f.columns, f.patches, f.shifts}}, {"palette", {f.paletteSets, f.paletteUs}},
            {"cache_us", {f.columnBuildUs, f.columnShiftUs, f.basePictureUs, f.patchPictureUs}},
            {"news", f.news}, {"lightning", f.lightning}, {"readback_us", f.readbackUs}, {"disk_available", f.diskAvailable},
            {"reads", {f.reads, f.readBytes, f.readUs}}, {"writes", {f.writes, f.writeBytes, f.writeUs}},
            {"allocations_available", f.allocationsAvailable}, {"allocations", {f.allocations, f.allocationBytes}},
            {"heap", {f.heapCalls, f.heapUs}}, {"autosaves", f.autosaves},
            {"date", {f.month, f.monthTick}}, {"date_events", {f.days, f.weeks, f.months}},
            {"opened", {f.opened, f.windowClass}}, {"remote", {f.commands, f.command}},
            {"actions", f.actions}, {"weather_changes", f.weatherChanges}, {"zoom", static_cast<int32_t>(f.zoom)},
            {"hitch", f.hitch}};
    }
    const char* PacingMode()
    {
        if (Config::Get().general.steadyFramePacing) return "steady20";
        if (Config::Get().general.uncapFPS) return "unlimited";
#ifdef OPENRCT2_CLASSIC_MAC
        if (ClassicHost::gCoarseFrameWait) return "legacy40";
#endif
        return "adaptive";
    }
    json FrameSummary()
    {
        const auto& t = FrameTiming::stats;
        const double mean = t.frames ? static_cast<double>(t.sumUs) / t.frames : 0;
        const double variance = t.frames ? t.sumSquaredUs / t.frames - mean * mean : 0;
        json result = {{"frames", t.frames}, {"hitches", t.hitches}, {"median_us", t.medianUs},
            {"median_no_tick_call_us", t.medianByTicks[0]}, {"median_tick_call_us", t.medianByTicks[1]}, {"mean_us", mean}, {"stddev_us", std::sqrt(std::max(0.0, variance))}, {"max_us", t.maximumUs},
            {"bin_upper_us", FrameTiming::binUpperUs}, {"histogram", t.histogram}, {"tick_calls", t.ticksHistogram},
            {"mode", PacingMode()}, {"worst", json::array()}};
        for (unsigned i = 0; i < 3 && t.worst[i].number; i++)
        {
            const auto& f = t.worst[i];
            result["worst"].push_back({f.number, f.us, f.tickCalls, f.stage[FrameTiming::simulation],
                f.stage[FrameTiming::draw], f.stage[FrameTiming::messages], f.autosaves, f.commands});
        }
        return result;
    }

    json Status(bool mark, bool audioDetail = false)
    {
        json result;
        const auto& state = getGameState();
        result["park"] = state.park.name;
        result["weather"] = static_cast<int>(state.weatherCurrent.weatherType);
        result["day_night"] = gDayNightCycle;
        result["ticks"] = state.currentTicks;
        result["guests"] = state.park.numGuestsInPark;
        result["rating"] = state.park.rating;
        result["cash"] = static_cast<double>(state.park.cash) / 10.0;
        result["month"] = state.date.GetMonthsElapsed();
        result["paused"] = GameIsPaused();
        result["speed"] = gGameSpeed;
        result["display_modes"] = json::array();
        for (const auto& mode : ShimHost::DisplayModes())
            result["display_modes"].push_back({mode.width, mode.height});
        result["next_resolution"] = {Config::Get().general.fullscreenWidth, Config::Get().general.fullscreenHeight};
        result["window_scale"] = Config::Get().general.windowScale;
        result["canvas"] = { ContextGetWidth(), ContextGetHeight() };
        result["solid_bars"] = Config::Get().interface.originalSolidBars;
        result["lens"] = {{"on", Config::Get().interface.seeThroughLens},
            {"radius", Config::Get().interface.seeThroughLensRadius},
            {"dither", Config::Get().interface.ditheredLensEdge}};
        result["solid_bars_active"] = Ui::Windows::OriginalHudEnabled();
        {
            int x, y;
            const auto buttons = SDL_GetMouseState(&x, &y);
            result["mouse"] = { { "x", x }, { "y", y }, { "buttons", buttons } };
            if (const auto* cursor = GetContext()->GetUiContext().GetCursorState())
                result["mouse"]["ui"] = { cursor->position.x, cursor->position.y };
        }
        static constexpr const char* farEntityModes[] = { "show", "auto", "hide" };
        result["hidefar_mode"] = farEntityModes[EnumValue(Config::Get().general.farEntityMode)];
        result["hidefar"] = false;
        if (auto* window = WindowGetMain(); window != nullptr && window->viewport != nullptr)
        {
            result["hidefar"] = window->viewport->AreFarEntitiesHidden();
            const auto& far = window->viewport->farEntities;
            result["far_auto"] = {{"mean_us",far.meanMicros},{"slow_us",far.slowMicros},
                {"healthy_us",far.healthyMicros},{"shown_us",far.shownMicros},
                {"hidden_us",far.hiddenMicros},{"hidden_entities",far.hiddenEntities},
                {"entities",far.visibleEntities},{"hides",far.hides},{"releases",far.releases}};
            result["view"] = { { "x", window->savedViewPos.x },
                               { "screen_x", window->viewport->pos.x },
                               { "screen_y", window->viewport->pos.y },
                               { "y", window->savedViewPos.y },
                               { "width", window->viewport->width },
                               { "height", window->viewport->height },
                               { "rotation", window->viewport->rotation },
                               { "zoom", static_cast<int>(static_cast<int8_t>(window->viewport->zoom)) } };
        }
        result["profile"] = ProfileSince(gMark);
        result["frame_times"] = FrameSummary();
        if (!gPanResult.is_null())
            result["pan"] = gPanResult;
        if (gPan.framesLeft > 0)
            result["pan_frames_left"] = gPan.framesLeft;
        result["pending_input"] = gSteps.size();
        result["card_scroll"] = ShimHost::CanScroll(); // false while something covers part of the window
#ifdef OPENRCT2_PAK_VFS
        {
            result["diskio"] = json::parse(ClassicDiskIo::Summary());
            result["autosave"] = json::parse(ClassicAutosave::Status());
            const auto audio = Audio::ClassicAudioGetStats();
            #ifdef OPENRCT2_CLASSIC_MAC
            result["memory"] = { { "in_use", ClassicAllocatorBytes() }, { "free", ShimHost::FreeMemory(false) },
                                 { "largest", ShimHost::FreeMemory(true) } };
            {
                unsigned long sizes[256], orders[256], chunkBytes = 0;
                const int count = ClassicAllocatorLargeBlocks(sizes, orders, 256, &chunkBytes);
                auto blocks = json::array();
                for (int i = 0; i < count; i++)
                    blocks.push_back({ orders[i], sizes[i] });
                result["memory"]["small_pools"] = chunkBytes;
                result["memory"]["large_calls"] = ClassicAllocatorLargeCalls();
                result["memory"]["blocks"] = blocks; // [order of allocation, bytes]
                unsigned long size[64], live[64], carved[64];
                const int classes = ClassicAllocatorClasses(size, live, carved, 64);
                auto list = json::array();
                for (int i = 0; i < classes; i++)
                    list.push_back({ size[i], live[i], carved[i] });
                result["memory"]["classes"] = list; // [block size, blocks in use, blocks ever cut]
            }
#endif
            static constexpr const char* preloadModes[]{"off", "on", "automatic"};
            result["audio_cache"] = {{"mode", preloadModes[audio.preloadMode]}, {"full", audio.fullPreload},
                {"capacity", audio.cacheCapacity}, {"evictions", audio.cacheEvictions}, {"prepare_reads", audio.prepareReads}, {"demand_reads", audio.demandReads},
                {"physical", audio.admission.physical}, {"logical", audio.admission.logical}, {"partition", audio.admission.partition},
                {"free_at_admission", audio.admission.free}, {"largest_at_admission", audio.admission.largest}};
            result["audio"] = { { "cache_bytes", audio.cacheBytes }, {"cache_entries", audio.cacheEntries}, {"cache_hits", audio.cacheHits}, {"preload_complete", audio.preloadComplete}, {"track_cache_bytes", ClassicVfs::TrackDesignCacheBytes()}, { "resident_bytes", audio.residentBytes }, {"pending_bytes", audio.pendingBytes}, {"pending_sources", audio.pendingSources}, {"music_channels", audio.musicChannels}, {"effect_channels", audio.effectChannels}, { "voices", audio.playing }, { "started", audio.started }, { "refused", audio.refused },
                                { "probe_mode", audio.probeMode }, { "mixer", audio.mixerBackend }, { "mixer_rate", audio.mixerRate },
                                { "ready_blocks", audio.readyBlocks }, { "target_blocks", audio.targetBlocks },
                                { "lookahead_frames", audio.lookaheadFrames }, { "minimum_ready_blocks", audio.minimumReadyBlocks },
                                { "producer_gap_us_max", audio.maxProducerGapMicros }, { "replacement_deferrals", audio.replacementDeferrals },
                                { "mixer_bits", audio.mixerBits }, { "block_frames", audio.mixerBlockFrames },
                                { "buffered_blocks", audio.bufferedBlocks }, { "rebuilt_blocks", audio.rebuiltBlocks }, { "underruns", audio.underruns }, { "mix_us", audio.mixMicros },
                                { "mixer_channels", audio.mixerChannels }, { "mixed_blocks", audio.mixedBlocks },
                                { "mixed_frames", audio.mixedFrames }, { "queue_errors", audio.queueErrors },
                                { "callbacks", audio.callbacks }, { "silent_test", audio.silentPlaybackTest },
                                { "sources", audio.sourcesLoaded }, { "sources_failed", audio.sourcesFailed },
                                { "play_requests", audio.playRequests },
                                { "channels_allocated", audio.channelsAllocated }, { "channels_disposed", audio.channelsDisposed },
                                { "available", Audio::IsAvailable() }, { "sounds_off", Audio::gGameSoundsOff },
                                { "output_limit_percent", audio.outputLimitPercent },
                                { "system_volume", { audio.systemVolumeLeft, audio.systemVolumeRight } } };
            if (audioDetail)
            {
            result["audio"]["hardware"] = {{"error",audio.hardwareQueryError},{"frames",audio.hardwareFrames},
                {"rate_fixed",audio.hardwareRateFixed},{"bits",audio.hardwareBits},{"channels",audio.hardwareChannels}};
            result["audio"]["callback_trace"] = {{"on",audio.audioTrace},{"samples",audio.callbackSamples},{"ticks",audio.callbackTicks},
                {"max_ticks",audio.maxCallbackTicks},{"execution_level",audio.callbackLevel},{"sr_mask",audio.callbackMask},
                {"intervals",audio.callbackIntervals},{"interval_ticks",audio.callbackIntervalTicks},
                {"min_interval_ticks",audio.callbackIntervalMin},{"max_interval_ticks",audio.callbackIntervalMax}};
            result["audio"]["safety"] = {{"peak_gain_units",audio.peakGainUnits},{"volume_errors",audio.volumeCommandErrors},{"silent_starts",audio.silentStarts}};
            result["audio"]["output"] = {{"rate_fixed",audio.outputRateFixed},{"rate_error",audio.outputRateError},{"quality",audio.quality},{"quality_error",audio.qualityError}};
            result["audio"]["voice_details"] = json::array();
            for (const auto& voice : audio.voices) if (voice.playing)
                result["audio"]["voice_details"].push_back({{"format",voice.compressed ? "ima4" : "twos"},
                    {"rate",voice.rate},{"channels",voice.channels},{"multiplier",voice.multiplier},
                    {"gain",{voice.left,voice.right}},{"queried_volume",voice.queriedVolume},{"volume_query_error",voice.volumeQueryError}});
            }
        }
#endif
        if (mark)
            gMark = ShimHost::Profile();
        return result;
    }

    json Run(const json& command)
    {
        const std::string op = command.value("op", "");
        FrameTiming::Command(op.c_str());
#ifdef OPENRCT2_PAK_VFS
        ClassicDiskIo::Scope diskScope(op.c_str());
#endif
        json result = json::object();
        if (op == "build")
            result = { { "source", RctBuild::Source() + 14 }, { "build", RctBuild::Id() + 13 }, { "lane", RctBuild::Lane() } };
        else if (op == "profile")
            result = command.value("pan", false) ? gPanResult : ProfileSince(gMark);
        else if (op == "status")
        {
            result = Status(command.value("mark", false));
            result["build"] = { { "source", RctBuild::Source() + 14 }, { "build", RctBuild::Id() + 13 }, { "lane", RctBuild::Lane() } };
        }
#ifdef OPENRCT2_PAK_VFS
        else if (op == "audio")
            result = Status(false, true).at("audio");
        else if (op == "diskio")
        {
            if (command.value("reset", false)) ClassicDiskIo::Reset();
            result = json::parse(ClassicDiskIo::Detail(command.value("events", false),
                command.value("first", 0u), command.value("limit", 6u)));
        }
#endif
        else if (op == "framepath")
            result = json::parse(ShimHost::FrameBenchCommand(command.dump()));
        else if (op == "frames")
        {
            if (command.value("reset", false)) FrameTiming::Reset();
            result = FrameSummary();
            if (command.contains("hitch_mode"))
            {
                const auto mode = command.at("hitch_mode").get<std::string>();
                if (mode != "off" && mode != "limited" && mode != "immediate")
                    throw std::runtime_error("hitch_mode must be off, limited or immediate");
                FrameTiming::SetHitchReporting(mode == "off" ? FrameTiming::HitchReporting::off
                    : mode == "immediate" ? FrameTiming::HitchReporting::immediate : FrameTiming::HitchReporting::limited);
            }
            const auto reporting = FrameTiming::GetHitchReportStatistics();
            result["hitch_reports"] = {{"printed", reporting.printed}, {"coalesced", reporting.coalesced},
                {"report_us", reporting.reportUs}, {"pending", reporting.pending}};
            result["stage_names"] = FrameTiming::names;
            result["records"] = json::array();
            const auto& t = FrameTiming::stats;
            const uint64_t oldest = t.frames > FrameTiming::historySize ? t.frames - FrameTiming::historySize + 1 : 1;
            result["oldest"] = oldest;
            const unsigned limit = std::clamp(command.value("limit", 2u), 1u, 2u);
            if (command.value("worst", false))
            {
                const unsigned first = std::min(command.value("first", 0u), FrameTiming::worstSize);
                for (unsigned i = first; i < FrameTiming::worstSize && i < first + limit; i++)
                    if (t.worst[i].number) result["records"].push_back(FrameRecord(t.worst[i]));
            }
            else
            {
                const uint64_t first = std::max(command.value("first", oldest), oldest);
                for (uint64_t i = first; i <= t.frames && i < first + limit; i++)
                    result["records"].push_back(FrameRecord(t.history[(i - 1) % FrameTiming::historySize]));
            }
        }
        else if (op == "pacing")
        {
            if (command.contains("mode"))
            {
                const auto mode = command.at("mode").get<std::string>();
                if (mode != "adaptive" && mode != "steady20" && mode != "unlimited" && mode != "legacy40")
                    throw std::invalid_argument("mode: adaptive, steady20, unlimited or legacy40");
                Config::Get().general.uncapFPS = mode == "unlimited";
#ifdef OPENRCT2_CLASSIC_MAC
                ClassicHost::gCoarseFrameWait = mode == "legacy40";
#endif
                Config::Get().general.steadyFramePacing = mode == "steady20";
                if (command.value("save", false)) Config::Save();
            }
            result["mode"] = PacingMode();
        }
        else if (op == "interpolation")
        {
            if (command.contains("mode"))
            {
                const auto name = command.at("mode").get<std::string>();
                const auto mode = name == "current" ? InterpolationMode::current
                    : name == "final" ? InterpolationMode::finalTick : InterpolationMode::bypass;
                if (name != "current" && name != "final" && name != "bypass")
                    throw std::invalid_argument("mode: current, final or bypass");
                if (mode != gInterpolationMode)
                {
                    // Both interpolating modes share endpoint semantics. Keep
                    // them on zero-tick pictures; bypass transitions restore
                    // canonical positions and discard those endpoints.
                    if (mode == InterpolationMode::bypass || gInterpolationMode == InterpolationMode::bypass)
                    {
                        EntityTweener::get().restore();
                        EntityTweener::get().reset();
                    }
                    gInterpolationMode = mode;
                }
            }
            if (command.contains("probe")) gInterpolationProbe = command["probe"].get<bool>();
            if (command.value("reset", false)) { gInterpolationStats = {}; InterpolationClearDamage(); }
#ifndef OPENRCT2_CLASSIC_MAC
            // Deterministic graphical checks only; never changes native cadence.
            if (command.contains("delta_ms")) gFixedFrameDelta = std::clamp(command["delta_ms"].get<float>(), 0.001f, 300.f) / 1000.f;
#endif
            const auto& s = gInterpolationStats;
            result["mode"] = gInterpolationMode == InterpolationMode::current ? "current"
                : gInterpolationMode == InterpolationMode::finalTick ? "final" : "bypass";
            result["probe"] = gInterpolationProbe;
            result["calls"] = s.calls; result["micros"] = s.micros;
            result["scanned"] = s.scanned; result["selected"] = s.selected;
            result["restore_moves"] = s.restoreMoves; result["tween_moves"] = s.tweenMoves;
            result["invalidations"] = s.invalidations;
            result["damage_pixels"] = s.damagePixels;
        }
        else if (op == "autosave")
        {
            GameAutosave();
#ifdef OPENRCT2_PAK_VFS
            result["autosave"] = json::parse(ClassicAutosave::Status());
#endif
        }
        else if (op == "buildpath")
        {
            const CoordsXY xy{command.at("x").get<int>(), command.at("y").get<int>()};
            const CoordsXYZ xyz{xy.x, xy.y, command.value("z", TileElementHeight(xy))};
            GameActions::FootpathPlaceAction action(xyz, FootpathSlope{}, command.value("type", 0), command.value("railings", 0));
            const auto placed = GameActions::Execute(&action, getGameState());
            result["error"] = EnumValue(placed.error); result["cost"] = static_cast<double>(placed.cost) / 10.0;
        }
        else if (op == "clockbench")
        {
            const int calls = std::clamp(command.value("calls", 100000), 1000, 1000000);
            uint64_t checksum = 0;
            auto start = ShimHost::Microseconds();
            for (int i = 0; i < calls; i++) checksum += ShimHost::Microseconds();
            result["toolbox_us_each"] = double(ShimHost::Microseconds() - start) / calls;
            start = ShimHost::Microseconds();
            for (int i = 0; i < calls; i++) checksum += ShimHost::TimeBase();
            result["timebase_us_each"] = double(ShimHost::Microseconds() - start) / calls;
            start = ShimHost::Microseconds();
            const auto fastStart = ShimHost::PaintMicroseconds();
            for (int i = 0; i < calls; i++) checksum += ShimHost::PaintMicroseconds();
            result["fast_elapsed_us"] = ShimHost::PaintMicroseconds() - fastStart;
            result["toolbox_elapsed_us"] = ShimHost::Microseconds() - start;
            result["fast_us_each"] = result["toolbox_elapsed_us"].get<double>() / calls;
            result["checksum"] = checksum;
        }
        else if (op == "probes")
        {
            Probe::gEnabled = false;
            if (command.value("on", false))
            {
                std::memset(Probe::gTicks, 0, sizeof(Probe::gTicks));
                std::memset(Probe::gCalls, 0, sizeof(Probe::gCalls));
                Probe::gCurrent = Probe::Other;
                gProbeStart = ShimHost::Profile();
                Probe::gLastStamp = Probe::Stamp();
                Probe::gEnabled = true;
            }
            else
            {
                const auto now = ShimHost::Profile();
                const auto ticks = Probe::gCalls[Probe::GameLogicRest];
                uint64_t sum = 0;
                for (int i = 1; i < Probe::Count; i++)
                    sum += Probe::gTicks[i];
                result["ticks"] = ticks;
                result["tick_us"] = ticks ? static_cast<double>(now.loop[0] - gProbeStart.loop[0]) / ticks : 0;
                result["frames"] = now.frames - gProbeStart.frames;
                result["seconds"] = static_cast<double>(now.microseconds - gProbeStart.microseconds) / 1000000;
                auto scopes = json::array();
                for (int i = 1; i < Probe::Count && sum > 0 && ticks > 0; i++)
                    if (Probe::gCalls[i] > 0)
                        scopes.push_back({ { "name", Probe::kNames[i] },
                                       { "fraction", static_cast<double>(Probe::gTicks[i]) / sum },
                                       { "calls_per_tick", static_cast<double>(Probe::gCalls[i]) / ticks } });
                result["scopes"] = scopes;
            }
        }
        else if (op == "mouse")
        {
#ifdef OPENRCT2_CLASSIC_MAC
            if (command.value("native",false)) ShimHost::WarpPointer(command.at("x").get<int>(),command.at("y").get<int>());
#endif
            const std::string action = command.value("action", "click");
            Step step{ Step::Kind::move, command.at("x").get<int>(), command.at("y").get<int>(), ButtonOf(command) };
            gSteps.push_back(step);
            if (action == "down" || action == "click")
                step.kind = Step::Kind::down, gSteps.push_back(step);
            if (action == "up" || action == "click")
                step.kind = Step::Kind::up, gSteps.push_back(step);
        }
        else if (op == "wheel")
        {
            SDL_Event event{};
            event.type = SDL_MOUSEWHEEL;
            event.wheel.y = command.value("dy", 1);
            ShimHost::PushEvent(event);
        }
        else if (op == "window")
        {
            const std::string kind = command.at("kind").get<std::string>();
            const WindowClass cls = kind == "options" ? WindowClass::options : kind == "ride" ? WindowClass::ride
                : kind == "dropdown" ? WindowClass::dropdown : kind == "tooltip" ? WindowClass::tooltip
                : kind == "text" ? WindowClass::textinput : kind == "shortcuts" ? WindowClass::keyboardShortcutList
                : kind == "themes" ? WindowClass::themes : kind == "currency" ? WindowClass::customCurrencyConfig
                : kind == "assets" ? WindowClass::assetPacks : WindowClass::mainWindow;
            auto* window = Ui::GetWindowManager()->FindByClass(cls);
            result["found"] = window != nullptr;
            if (window != nullptr)
                result["rect"] = { window->windowPos.x, window->windowPos.y, window->windowPos.x + window->width,
                                   window->windowPos.y + window->height };
        }
        else if (op == "uiwindows")
        {
            result["canvas"] = { ContextGetWidth(), ContextGetHeight() };
            const auto bounds = Ui::Windows::WindowGetUsableBounds();
            result["usable"] = { bounds.getLeft(), bounds.getTop(), bounds.getRight(), bounds.getBottom() };
            result["windows"] = json::array();
            WindowVisitEach([&](WindowBase* w) {
                if (command.contains("class") && EnumValue(w->classification) != command.at("class").get<int>())
                    return;
                json entry = { { "class", EnumValue(w->classification) },
                               { "number", w->number },
                               { "rect",
                                 { w->windowPos.x, w->windowPos.y, w->windowPos.x + w->width, w->windowPos.y + w->height } },
                               { "min", { w->minWidth, w->minHeight } },
                               { "max", { w->maxWidth, w->maxHeight } },
                               { "page", w->page } };
                if (command.value("title_controls", false))
                {
                    if (w->viewport != nullptr)
                    {
                        entry["viewport"] = { w->viewport->pos.x, w->viewport->pos.y,
                                              w->viewport->width, w->viewport->height };
                        entry["viewport_suspended"] = w->viewport->isSuspended;
                        entry["viewport_visible"] = w->viewport->isVisible;
                        entry["viewport_camera"] = { w->viewport->viewPos.x, w->viewport->viewPos.y };
                        entry["viewport_storage"] = ViewportStorageFor(w->viewport);
                        entry["viewport_zoom"] = static_cast<int8_t>(w->viewport->zoom);
                        if (const auto* target = getGameState().entities.tryGetEntity(w->viewportTargetSprite))
                            entry["viewport_target"] = { target->id.ToUnderlying(), EnumValue(target->type), target->x,
                                                         target->y, target->z };
                    }
                    entry["shaded"] = w->windowShaded;
                    entry["zoomed"] = w->windowZoomed;
                    entry["sticky"] = w->flags.hasAny(WindowFlag::stickToBack, WindowFlag::stickToFront);
                    const auto draw = WindowGetDrawRect(*w);
                    entry["draw_rect"] = { draw.getLeft(), draw.getTop(), draw.getRight(), draw.getBottom() };
                    entry["title_controls"] = json::object();
                    for (const auto control : { Ui::Windows::TitleBarControl::shade, Ui::Windows::TitleBarControl::zoom })
                        if (const auto rect = Ui::Windows::WindowGetTitleBarControlRect(*w, control))
                            entry["title_controls"][control == Ui::Windows::TitleBarControl::shade ? "shade" : "zoom"] = {
                                rect->getLeft(), rect->getTop(), rect->getRight(), rect->getBottom()
                            };
                }
                if (command.value("widgets", false))
                {
                    entry["widgets"] = json::array();
                    for (size_t i = 0; i < w->widgets.size(); i++)
                    {
                        const auto& widget = w->widgets[i];
                        entry["widgets"].push_back(
                            { { "index", i },
                              { "type", EnumValue(widget.type) },
                              { "rect", { widget.left, widget.top, widget.right + 1, widget.bottom + 1 } },
                              { "visible", widget.isVisible() },
                              { "pressed", Ui::widgetIsPressed(*w, i) },
                              { "disabled", Ui::widgetIsDisabled(*w, i) } });
                    }
                }
                if (command.value("scrolls", false))
                {
                    entry["scrolls"] = json::array();
                    for (const auto& scroll : w->scrolls)
                        entry["scrolls"].push_back({ { "offset", { scroll.contentOffsetX, scroll.contentOffsetY } },
                            { "content", { scroll.contentWidth, scroll.contentHeight } } });
                }
                result["windows"].push_back(std::move(entry));
            });
        }
        else if (op == "uiopen")
        {
            auto cls = static_cast<WindowClass>(command.at("class").get<int>());
            auto* wm = Ui::GetWindowManager();
            WindowBase* w;
            if (cls == WindowClass::scenarioSelect || cls == WindowClass::loadsave || cls == WindowClass::peep
                || cls == WindowClass::firePrompt || cls == WindowClass::trackDesignList || cls == WindowClass::trackDesignPlace
                || cls == WindowClass::manageTrackDesign || cls == WindowClass::installTrack)
            {
                Intent intent(cls);
                if (cls == WindowClass::loadsave)
                {
                    intent.PutEnumExtra(
                        INTENT_EXTRA_LOADSAVE_ACTION,
                        command.value("save", false) ? LoadSaveAction::save : LoadSaveAction::load);
                    intent.PutEnumExtra(INTENT_EXTRA_LOADSAVE_TYPE, LoadSaveType::park);
                }
                if (cls == WindowClass::peep || cls == WindowClass::firePrompt)
                {
                    Peep* peep = nullptr;
                    if (command.value("staff", false) || cls == WindowClass::firePrompt)
                        for (auto* p : EntityList<Staff>())
                        {
                            if (command.contains("staff_type")
                                && EnumValue(p->assignedStaffType) != command.at("staff_type").get<int>())
                                continue;
                            peep = p;
                            break;
                        }
                    else
                        for (auto* p : EntityList<Guest>())
                        {
                            peep = p;
                            break;
                        }
                    if (peep == nullptr)
                        throw std::runtime_error("no peep for fixture");
                    intent.PutExtra(INTENT_EXTRA_PEEP, peep);
                }
                if (cls == WindowClass::trackDesignList)
                {
                    auto* ride = GetRide(RideId::FromUnderlying(command.at("ride").get<int>()));
                    if (ride == nullptr)
                        throw std::runtime_error("no ride for fixture");
                    intent.PutExtra(INTENT_EXTRA_RIDE_TYPE, ride->type);
                    intent.PutExtra(INTENT_EXTRA_RIDE_ENTRY_INDEX, ride->subtype);
                }
                if (cls == WindowClass::trackDesignPlace || cls == WindowClass::manageTrackDesign)
                {
                    // The ordinary windows retain the reference beyond this call.
                    static TrackDesignFileRef track;
                    track.path = command.at("path").get<std::string>();
                    track.name = GetNameFromTrackPath(track.path);
                    intent.PutExtra(INTENT_EXTRA_TRACK_DESIGN, &track);
                }
                if (cls == WindowClass::installTrack)
                    intent.PutExtra(INTENT_EXTRA_PATH, command.at("path").get<std::string>());
                w = wm->OpenIntent(&intent);
            }
            else if (cls == WindowClass::loadsaveOverwritePrompt)
                w = Ui::Windows::WindowOverwritePromptOpen(
                    "Layout fixture", "/RCT/user/layout-fixture.park", LoadSaveAction::save, LoadSaveType::park, nullptr);
            else if (cls == WindowClass::objectLoadError)
            {
                static const ObjectEntryDescriptor missing(ObjectType::smallScenery, "ui640.missing.scenery");
                // The normal dialog copies the descriptors; no pack is changed.
                char path[] = "Layout fixture.park";
                w = Ui::Windows::ObjectLoadErrorOpen(path, 1, &missing);
            }
            else if (cls == WindowClass::progressWindow)
                w = Ui::Windows::ProgressWindowOpen("Layout fixture");
            else if (cls == WindowClass::patrolArea)
            {
                auto staff = EntityList<Staff>();
                auto first = staff.begin();
                if (first == staff.end())
                    throw std::runtime_error("no staff for patrol fixture");
                w = Ui::Windows::PatrolAreaOpen((*first)->id);
            }
            else
                w = wm->OpenWindow(cls);
            if (w == nullptr)
                throw std::runtime_error("window did not open");
            if (cls == WindowClass::scenery || cls == WindowClass::land || cls == WindowClass::water
                || cls == WindowClass::clearScenery || cls == WindowClass::landRights)
                if (!isToolActive(cls))
                    ToolSet(*w, 0, Tool::arrow);
        }
        else if (op == "uidetail")
        {
            auto* w = Ui::GetWindowManager()->openDetails(
                static_cast<WindowDetail>(command.at("detail").get<int>()), command.at("id").get<int>());
            if (w == nullptr)
                throw std::runtime_error("detail window did not open");
        }
        else if (op == "uibanners")
        {
            result["banners"] = json::array();
            for (const auto& banner : getGameState().banners)
                if (!banner.isNull())
                {
                    result["banners"].push_back(
                        { { "id", banner.id.ToUnderlying() },
                          { "detail", banner.flags.has(BannerFlag::isWall) ? EnumValue(WindowDetail::signSmall)
                                : banner.flags.has(BannerFlag::isLargeScenery) ? EnumValue(WindowDetail::sign)
                                                                             : EnumValue(WindowDetail::banner) } });
                    if (command.value("positions",false))
                    {
                        const auto* main=WindowGetMain();
                        if (main==nullptr || main->viewport==nullptr) throw std::runtime_error("no main view");
                        auto& item=result["banners"].back();
                        const CoordsXY tile{banner.position.x*32,banner.position.y*32};
                        const auto* element=BannerGetTileElement(banner.id);
                        const int32_t z=element!=nullptr ? element->getBaseZ() : TileElementHeight(tile);
                        const auto point=Translate3DTo2DWithZ(main->viewport->rotation,{tile.x+16,tile.y+16,z});
                        item["position"]={tile.x,tile.y,z};item["screen"]={point.x,point.y};
                        if (element!=nullptr && element->asBanner()!=nullptr)
                            item["direction"]=element->asBanner()->getPosition();
                        if (!banner.flags.has(BannerFlag::isWall) && !banner.flags.has(BannerFlag::isLargeScenery))
                            if (const auto* entry=OpenRCT2::ObjectEntryManager::GetObjectEntry<BannerSceneryEntry>(banner.type))
                                item["scrolling_mode"]=entry->scrolling_mode;
                    }
                }
        }
        else if (op == "uitext")
            Ui::Windows::WindowTextInputOpen(
                "Layout fixture", "Enter a name", "", 32, [](std::string_view) {}, []() {});
        else if (op == "uierror")
            Ui::GetWindowManager()->ShowError("Layout fixture", "A normal error message");
        else if (op == "uiconstruct")
        {
            auto* ride = GetRide(RideId::FromUnderlying(command.at("ride").get<int>()));
            if (ride == nullptr)
                throw std::runtime_error("no ride for fixture");
            if (command.value("new", false))
                RideConstructNew({ ride->type, ride->subtype });
            else
                RideConstructionStart(*ride);
        }
        else if (op == "uimove")
        {
            auto* w = Ui::GetWindowManager()->FindByClass(static_cast<WindowClass>(command.at("class").get<int>()));
            if (w == nullptr)
                throw std::runtime_error("window not found");
            Ui::Windows::WindowMoveAndSnap(
                *w, { command.at("x").get<int>(), command.at("y").get<int>() }, command.value("snap", 0));
        }
        else if (op == "uiwidget")
        {
            auto* wm = Ui::GetWindowManager();
            auto cls = static_cast<WindowClass>(command.at("class").get<int>());
            auto* w = command.contains("number")
                ? wm->FindByNumber(cls, static_cast<WindowNumber>(command.at("number").get<int>()))
                : wm->FindByClass(cls);
            const auto index = command.at("widget").get<size_t>();
            if (w == nullptr || index >= w->widgets.size())
                throw std::runtime_error("widget not found");
            const auto& widget = w->widgets[index];
            const float scale = Config::Get().general.windowScale;
            Step step;
            step.x = static_cast<int>(std::ceil((w->windowPos.x + (widget.left + widget.right) / 2) * scale));
            step.y = static_cast<int>(std::ceil((w->windowPos.y + (widget.top + widget.bottom) / 2) * scale));
            step.kind = Step::Kind::move;
            gSteps.push_back(step);
            if (!command.value("hover", false))
            {
                step.kind = Step::Kind::down;
                gSteps.push_back(step);
                step.kind = Step::Kind::up;
                gSteps.push_back(step);
            }
            result["display"] = { step.x, step.y };
        }
        else if (op == "screenshot_timer")
        {
            if (command.contains("delay_ms"))
                for (unsigned i = 0; i < std::clamp(command.value("count", 1u), 1u, 17u); ++i)
                    ScreenshotRequest(std::clamp<uint32_t>(command.at("delay_ms").get<uint32_t>(), 0, 60000));
            result["remaining_ms"] = ScreenshotDelayRemaining();
            result["frames_pending"] = gScreenshotCountdown;
            result["pending_requests"] = ScreenshotPendingRequests();
        }
        else if (op == "shot")
        {
            if (gDrawingMemoryStale)
                throw std::runtime_error("redraw before capturing a stale frame");
            result["path"] = ScreenshotDump();
        }
        else if (op == "entity_bounds")
        {
            result["entities"] = json::array();
            for (const auto& id : command.at("ids"))
            {
                const auto* entity = getGameState().entities.getEntity(EntityId::FromUnderlying(id.get<uint16_t>()));
                if (entity == nullptr)
                    continue;
                json entry = { { "id", entity->id.ToUnderlying() }, { "type", EnumValue(entity->type) },
                               { "location", { entity->x, entity->y, entity->z } },
                               { "bounds", { entity->spriteData.width, entity->spriteData.heightMin,
                                               entity->spriteData.heightMax } } };
                if (entity->type == EntityType::guest || entity->type == EntityType::staff)
                {
                    const auto* peep = static_cast<const Peep*>(entity);
                    const auto type = peep->action == PeepActionType::idle ? peep->nextAnimationType : peep->animationType;
                    const auto* object = GetContext()->GetObjectManager().GetLoadedObject<PeepAnimationsObject>(
                        peep->animationObjectIndex);
                    if (object != nullptr)
                    {
                        const auto& bounds = object->GetSpriteBounds(peep->animationGroup, type);
                        entry["inferred"] = { bounds.spriteWidth, bounds.spriteHeightNegative, bounds.spriteHeightPositive };
                    }
                    entry["pose"] = { EnumValue(peep->animationGroup), EnumValue(type), peep->animationImageIdOffset,
                                      peep->orientation };
                    entry["animation"] = { EnumValue(peep->animationType), EnumValue(peep->nextAnimationType), EnumValue(peep->action) };
                }
                result["entities"].push_back(std::move(entry));
            }
        }
        else if (op == "inspect")
        {
            const float scale = Config::Get().general.windowScale;
            const ScreenCoordsXY point{ static_cast<int>(command.at("x").get<int>() / scale),
                                        static_cast<int>(command.at("y").get<int>() / scale) };
            result["ui"] = { point.x, point.y };
            if (auto* window = Ui::GetWindowManager()->FindFromPoint(point))
            {
                result["class"] = EnumValue(window->classification);
                result["number"] = window->number;
                result["widget"] = Ui::GetWindowManager()->FindWidgetFromPoint(*window, point);
            }
            result["tool"] = EnumValue(gCurrentToolId);
            result["tool_widget"] = gCurrentToolWidget.widgetIndex;
            result["tool_active"] = gInputFlags.has(InputFlag::toolActive);
        }
        else if (op == "drag")
        {
            const int x = command.at("x").get<int>(), y = command.at("y").get<int>();
            const int x2 = command.at("x2").get<int>(), y2 = command.at("y2").get<int>();
            int frames = command.value("frames", 10);
            frames = frames < 1 ? 1 : frames > 400 ? 400 : frames;
            Step step{ Step::Kind::move, x, y, ButtonOf(command) };
            gSteps.push_back(step);
            step.kind = Step::Kind::down, gSteps.push_back(step);
            for (int i = 1; i <= frames; i++)
            {
                step.kind = Step::Kind::move;
                step.x = x + (x2 - x) * i / frames;
                step.y = y + (y2 - y) * i / frames;
                gSteps.push_back(step);
            }
            step.kind = Step::Kind::up, gSteps.push_back(step);
        }
        else if (op == "lens")
        {
            auto& config = Config::Get().interface;
            config.seeThroughLens = command.value("on", config.seeThroughLens);
            config.ditheredLensEdge = command.value("dither", config.ditheredLensEdge);
            config.seeThroughLensRadius = std::clamp(command.value("radius", config.seeThroughLensRadius), 16, 128);
            result["on"] = config.seeThroughLens;
            result["radius"] = config.seeThroughLensRadius;
            result["dither"] = config.ditheredLensEdge;
        }
        else if (op == "key")
        {
            const std::string name = command.at("key").get<std::string>();
            Step step{ Step::Kind::key };
            const auto action = command.value("action", std::string("press"));
            if (action == "down") step.kind = Step::Kind::keyDown;
            else if (action == "up") step.kind = Step::Kind::keyUp;
            else if (action != "press") throw std::invalid_argument("key action must be press, down or up");
            step.scancode = SDL_GetScancodeFromKey(SDL_GetKeyFromName(name.c_str()));
            if (step.scancode == SDL_SCANCODE_UNKNOWN && name.size() == 1)
                step.scancode = SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(name[0] | 0x20));
            if (step.scancode == SDL_SCANCODE_UNKNOWN)
                throw std::invalid_argument("unknown key name");
            if (command.contains("mods"))
                for (const auto& mod : command["mods"])
                {
                    const auto text = mod.get<std::string>();
                    step.mods |= text == "shift" ? KMOD_LSHIFT : text == "ctrl" ? KMOD_LCTRL : text == "alt" ? KMOD_LALT
                        : text == "gui" ? KMOD_LGUI : 0;
                }
            gSteps.push_back(step);
        }
        else if (op == "text")
        {
            for (const char c : command.at("text").get<std::string>())
            {
                Step step{ Step::Kind::text };
                step.character = c;
                gSteps.push_back(step);
            }
        }
        else if (op == "pan")
        {
            gPan.dx = command.value("dx", 0);
            gPan.dy = command.value("dy", 0);
            gPan.frames = gPan.framesLeft = command.value("frames", 100);
            gPan.aligned = command.value("aligned", false);
            gPan.startPending = gPan.aligned;
            gPan.warmFramesLeft = gPan.aligned ? std::clamp(command.value("warm_frames", 0), 0, 120) : 0;
            gPan.start = ShimHost::Profile();
#ifdef OPENRCT2_PERF_METRICS
            gPanPerfStart = PerfCapture();
            gPanPerfResult = nullptr;
#endif
            gPanResult = nullptr;
        }
        else if (op == "view")
        {
#ifdef OPENRCT2_CLASSIC_MAC
            if (command.value("measure_latency",false)) ShimHost::MarkPictureInput();
#endif
            auto* window = WindowGetMain();
            if (command.contains("class"))
            {
                auto* wm = Ui::GetWindowManager();
                const auto cls = static_cast<WindowClass>(command.at("class").get<int>());
                window = command.contains("number") ? wm->FindByNumber(cls, command.at("number").get<int>())
                                                    : wm->FindByClass(cls);
            }
            if (window == nullptr || window->viewport == nullptr)
                throw std::runtime_error("no requested view");
            if (command.contains("follow_ride"))
            {
                const auto* ride = GetRide(RideId::FromUnderlying(command.at("follow_ride").get<int>()));
                if (ride == nullptr || ride->vehicles[0].IsNull())
                    throw std::runtime_error("ride has no vehicle");
                WindowFollowSprite(*window, ride->vehicles[0]);
            }
            if (command.contains("zoom"))
                WindowZoomSet(*window, ZoomLevel{ static_cast<int8_t>(command.at("zoom").get<int>()) }, false);
            if (command.contains("rotation"))
                ViewportRotateAll((command.at("rotation").get<int>() - window->viewport->rotation + 4) & 3);
            if (command.contains("x"))
                window->savedViewPos.x = command["x"].get<int>();
            if (command.contains("y"))
                window->savedViewPos.y = command["y"].get<int>();
            result["x"] = window->savedViewPos.x;
            result["y"] = window->savedViewPos.y;
        }
        else if (op == "hoverprofile")
        {
            if (command.contains("verify_cache")) Ui::gVerifyHoverCache = command.at("verify_cache").get<bool>();
            if (command.contains("probe")) Ui::gHoverPickingProbe = command.at("probe").get<bool>();
            if (command.value("reset",false)) Ui::gHoverPickingStats = {};
            const auto& s = Ui::gHoverPickingStats;
            result["probe"] = Ui::gHoverPickingProbe;
            result["spatial"] = Ui::gSpatialPeepPicking;
            result["reuse"] = Ui::gReuseHoverPicking;
            result["paint_sessions"] = Ui::gHoverPickingStats.paintSessions;
            result["frames"] = FrameTiming::stats.frames;
            result["query_calls"] = s.queryCalls; result["query_us"] = s.queryMicros;
            result["closest_calls"] = s.closestCalls; result["closest_us"] = s.closestMicros;
            result["visited"] = s.visited; result["tested"] = s.tested; result["tiles"] = s.tiles;
            result["cache"] = Ui::gCacheHoverPicking; result["verify_cache"] = Ui::gVerifyHoverCache;
            result["cache_hits"] = s.cacheHits; result["cache_misses"] = s.cacheMisses;
            result["cache_checks"] = s.cacheChecks; result["cache_mismatches"] = s.cacheMismatches;
            result["nearest_hits"] = s.nearestHits;
        }
        else if (op == "hoveraudit")
        {
            const int left = command.at("x").get<int>(), top = command.at("y").get<int>();
            const int width = std::clamp(command.value("width",1),1,ContextGetWidth());
            const int height = std::clamp(command.value("height",1),1,ContextGetHeight());
            const int step = std::clamp(command.value("step",1),1,4096);
            result["samples"] = json::array();
            for (int y = top; y < top + height; y += step)
                for (int x = left; x < left + width; x += step)
                {
                    const auto ids = Ui::ViewportInteractionCompareNearest({x,y});
                    auto sample = json::array({x,y,ids[0],ids[1]});
                    if (command.value("queries",false))
                    {
                        const auto matches = Ui::ViewportInteractionCompareQueries({x,y});
                        sample.push_back(matches[0]); sample.push_back(matches[1]);
                    }
                    result["samples"].push_back(std::move(sample));
                }
            result["tick"] = getGameState().currentTicks;
        }
        else if (op == "pick")
        {
            const int left = command.at("x").get<int>(), top = command.at("y").get<int>();
            const int width = std::clamp(command.value("width", 1), 1, GetContext()->GetUiContext().GetWidth());
            const int height = std::clamp(command.value("height", 1), 1, GetContext()->GetUiContext().GetHeight());
            const int step = std::clamp(command.value("step", 1), 1, 4096);
            auto samples = json::array();
            for (int y = top; y < top + height; y += step)
                for (int x = left; x < left + width; x += step)
                {
                    const ScreenCoordsXY point{ x, y };
                    const auto info = GetMapCoordinatesFromPos(point, kViewportInteractionItemAll);
                    const auto precise = ScreenGetMapXY(point, nullptr);
                    uint8_t quadrant = 255, side = 255, quadrantZ = 255;
                    const auto tile = ScreenGetMapXYQuadrant(point, &quadrant);
                    const auto sideTile = ScreenGetMapXYSide(point, &side);
                    int z = precise ? TileElementHeight(*precise) : 0;
                    const auto tileZ = ScreenGetMapXYQuadrantWithZ(point, z, &quadrantZ);
                    auto sample = json::array({ x, y, EnumValue(info.interactionType), info.Loc.x, info.Loc.y,
                        info.Entity ? static_cast<int>(info.Entity->id.ToUnderlying()) : -1,
                        info.Entity ? static_cast<int>(EnumValue(info.Entity->type)) : -1,
                        info.Element ? static_cast<int>(EnumValue(info.Element->getType())) : -1,
                        precise ? precise->x : -1, precise ? precise->y : -1,
                        tile ? quadrant : -1, sideTile ? side : -1, z,
                        tileZ ? tileZ->x : -1, tileZ ? tileZ->y : -1, tileZ ? quadrantZ : -1 });
                    if (precise)
                    {
                        const auto* surface = MapGetSurfaceElementAt(*precise);
                        for (int corner = 0; corner < 4; corner++)
                            sample.push_back(surface ? TileElementGetCornerHeight(surface, corner) : -1);
                    }
                    samples.push_back(std::move(sample));
                }
            result["ticks"] = getGameState().currentTicks;
            result["samples"] = std::move(samples);
        }
        else if (op == "rlecheck")
        {
            Image image;
            image.Width = image.Stride = 32;
            image.Height = 20;
            image.Depth = 8;
            image.Pixels.resize(image.Width * image.Height);
            for (size_t i = 0; i < image.Pixels.size(); i++)
                image.Pixels[i] = i % 7 == 0 ? 0 : static_cast<uint8_t>(1 + i % 200);
            Drawing::ImageImportMeta meta;
            meta.palette = Drawing::Palette::keepIndices;
            const auto imported = Drawing::ImageImporter().Import(image, meta);
            result["data"] = imported.Buffer;
        }
        else if (op == "rotate")
        {
            const int direction = command.contains("rotation")
                ? ((command.at("rotation").get<int>() - GetCurrentRotation()) & 3)
                : command.value("direction", 1);
            if (command.contains("ride"))
            {
                const auto id = command.at("ride").get<int>();
                auto* window = Ui::GetWindowManager()->FindByNumber(WindowClass::ride, id);
                if (window == nullptr || window->viewport == nullptr)
                    throw std::runtime_error("no ride view");
                auto* viewport = window->viewport;
                const int rideDirection = command.contains("rotation")
                    ? ((command.at("rotation").get<int>() - viewport->rotation) & 3) : direction;
                // RideWindow's rotation hook recreates its viewport with the main rotation.
                // Keep this test view independent and centre it on the integral vehicle.
                const auto* ride = GetRide(RideId::FromUnderlying(id));
                const auto* vehicle = ride ? getGameState().entities.tryGetEntity<Vehicle>(ride->vehicles[0]) : nullptr;
                if (vehicle == nullptr)
                    throw std::runtime_error("ride has no vehicle");
                viewport->flags.set(ViewportFlag::independentRotation);
                viewport->rotation = (viewport->rotation + rideDirection) & 3;
                if (auto centre = centre2dCoordinates(vehicle->getLocation(), viewport))
                    window->savedViewPos = viewport->viewPos = *centre;
                window->invalidate();
            }
            else
                ViewportRotateAll(direction);
        }
        else if (op == "trackdesign")
        {
            TrackDesignFileRef design{ "", command.at("path").get<std::string>() };
            Intent intent(WindowClass::trackDesignPlace);
            intent.PutExtra(INTENT_EXTRA_TRACK_DESIGN, &design);
            auto* window = Ui::GetWindowManager()->OpenIntent(&intent);
            if (window == nullptr)
                throw std::runtime_error("could not open track design placement tool");
        }
        else if (op == "openride")
        {
            Intent intent(WindowClass::ride);
            intent.PutExtra(INTENT_EXTRA_RIDE_ID, command.at("id").get<int>());
            auto* window = Ui::GetWindowManager()->OpenIntent(&intent);
            if (window != nullptr && command.contains("x") && command.contains("y"))
                Ui::Windows::WindowSetPosition(*window, { command.at("x").get<int>(), command.at("y").get<int>() });
        }
        else if (op == "options")
        {
            auto* window = Ui::GetWindowManager()->OpenWindow(WindowClass::options);
            if (window == nullptr)
                throw std::runtime_error("no options window");
            if (command.contains("tab"))
            {
                const int tab = command.at("tab").get<int>();
                if (tab < 0 || tab >= 8) throw std::runtime_error("options tab must be 0..7");
                window->onMouseUp(4 + tab);
            }
            if (command.contains("scroll")) window->onMouseUp(command.at("scroll").get<int>() < 0 ? 12 : 13);
            window->onPrepareDraw();
            result["x"] = window->windowPos.x;
            result["y"] = window->windowPos.y;
            result["page"] = window->page;
            result["scale"] = Config::Get().general.windowScale;
            result["size"] = { window->width, window->height };
            if (command.value("widgets", false))
            {
                result["widgets"] = json::array();
                for (size_t i = 0; i < window->widgets.size(); ++i)
                {
                    if (command.contains("widget") && command.at("widget").get<size_t>() != i) continue;
                    const auto& w = window->widgets[i];
                    const bool textWidget = w.type == WidgetType::label || w.type == WidgetType::checkbox
                        || w.type == WidgetType::button || w.type == WidgetType::dropdownMenu || w.type == WidgetType::groupbox;
                    const char* text = textWidget ? (w.flags.has(WidgetFlag::textIsString) ? w.string : LanguageGetString(w.text)) : "";
                    result["widgets"].push_back({ {"index", i}, {"type", EnumValue(w.type)},
                        {"rect", {window->windowPos.x + w.left, window->windowPos.y + w.top, w.width(), w.height()}},
                        {"hidden", w.isHidden()}, {"disabled", w.flags.has(WidgetFlag::isDisabled)},
                        {"pressed", w.flags.has(WidgetFlag::isPressed)}, {"text", text ? text : ""},
                        {"reason", w.flags.has(WidgetFlag::tooltipIsString) && w.sztooltip ? w.sztooltip : ""} });
                }
            }
        }
        else if (op == "gueststate")
        {
            result["ticks"] = getGameState().currentTicks;
            result["guests"] = json::array();
            for (auto* guest : EntityList<Guest>())
            {
                if (command.contains("id") && guest->id.ToUnderlying() != command["id"].get<int>()) continue;
                json projections = json::array();
                for (const auto& viewport : GetAllViewports())
                    if (viewport.isVisible)
                    {
                        const auto pos = Translate3DTo2DWithZ(viewport.rotation, guest->getLocation());
                        projections.push_back({ viewport.pos.x + viewport.zoom.ApplyInversedTo(pos.x - viewport.viewPos.x),
                            viewport.pos.y + viewport.zoom.ApplyInversedTo(pos.y - viewport.viewPos.y) });
                    }
                result["guests"].push_back({ {"id", guest->id.ToUnderlying()}, {"peep_id", guest->peepId},
                    {"position", {guest->x,guest->y,guest->z}}, {"orientation", guest->orientation},
                    {"state", EnumValue(guest->state)}, {"action", EnumValue(guest->action)},
                    {"animation", {EnumValue(guest->animationType), EnumValue(guest->nextAnimationType),
                        guest->animationImageIdOffset, guest->animationFrameNum, guest->walkingAnimationFrameNum}},
                    {"screen", std::move(projections)} });
            }
        }
        else if (op == "views")
        {
            auto views = json::array();
            for (const auto& viewport : GetAllViewports())
                if (viewport.isVisible)
                    views.push_back({ { "rotation", viewport.rotation }, { "zoom", static_cast<int8_t>(viewport.zoom) },
                        { "rect", { viewport.pos.x, viewport.pos.y, viewport.pos.x + viewport.width, viewport.pos.y + viewport.height } },
                        { "world", { viewport.viewPos.x, viewport.viewPos.y } } });
            result["views"] = std::move(views);
        }
        else if (op == "focus")
        {
            auto* window = WindowGetMain();
            if (window == nullptr)
                throw std::runtime_error("no main view");
            const CoordsXY xy{ command.at("x").get<int>(), command.at("y").get<int>() };
            window->setViewportLocation({ xy.x, xy.y, command.value("z", TileElementHeight(xy)) });
        }
#ifdef OPENRCT2_RIDEFPS_FIXTURES
        else if (op == "ridefps")
            result.update(RideFpsFixture(command));
#endif
        else if (op == "ridestart")
        {
            if (!command.value("private_copy",false)) throw std::runtime_error("ride regression requires a private park copy");
            if (command.contains("create")) result.update(RideStartRegression::constructCase(command.at("create")));
            else
            {
                const auto* ride=GetRide(RideId::FromUnderlying(command.at("id").get<uint16_t>()));
                if (!ride) throw std::runtime_error("ride regression target missing");
                result["ride"]=RideStartRegression::describe(*ride);
            }
        }
        else if (op == "rides")
        {
            auto rides = json::array();
            for (const auto& ride : RideManager(getGameState()))
            {
                if (command.contains("type") && ride.type != command.at("type").get<int>())
                    continue;
                json item = { { "id", ride.id.ToUnderlying() }, { "type", ride.type }, { "name", ride.getName() },
                    { "x", ride.overallView.x }, { "y", ride.overallView.y }, {"music", ride.flags.has(RideFlag::music)}, {"tune", ride.musicTuneId} };
                if (const auto* vehicle = getGameState().entities.tryGetEntity<Vehicle>(ride.vehicles[0]))
                    item["vehicle"] = { { "x", vehicle->x }, { "y", vehicle->y }, { "z", vehicle->z },
                        { "frame", vehicle->flatRideAnimationFrame }, { "state", EnumValue(vehicle->status) },
                        { "rect", { vehicle->spriteData.spriteRect.getLeft(), vehicle->spriteData.spriteRect.getTop(),
                            vehicle->spriteData.spriteRect.getRight(), vehicle->spriteData.spriteRect.getBottom() } } };
                rides.push_back(std::move(item));
            }
            result["rides"] = std::move(rides);
        }
#ifndef OPENRCT2_CLASSIC_MAC
        else if (op == "tileobjects")
        {
            result["walls"] = json::array();
            auto* element = MapGetFirstElementAt(CoordsXY{command.at("x").get<int>(),command.at("y").get<int>()});
            if (element != nullptr) do
            {
                if (auto* wall = element->asWall())
                {
                    auto* object = GetContext()->GetObjectManager().GetLoadedObject<WallObject>(wall->getEntryIndex());
                    result["walls"].push_back({{"entry",wall->getEntryIndex()}, {"direction",wall->getDirection()},
                        {"height",wall->getBaseZ()}, {"identifier",object ? std::string(object->GetIdentifier()) : ""},
                        {"name",object ? object->GetName() : ""}});
                }
            } while (!(element++)->isLastForTile());
        }
        else if (op == "scenesurvey")
            result.update(ScenesFixture::run(command));
#endif
        else if (op == "balloon")
            Balloon::create({ command.at("x").get<int>(), command.at("y").get<int>(), command.at("z").get<int>() },
                Drawing::Colour::brightRed, false);
        else if (op == "balloons")
        {
            auto balloons = json::array();
            for (const auto* balloon : EntityList<Balloon>())
                balloons.push_back({ balloon->id.ToUnderlying(), balloon->x, balloon->y, balloon->z, balloon->popped });
            result["balloons"] = std::move(balloons);
        }
        else if (op == "console")
        {
            CaptureConsole console;
            console.Execute(command.at("line").get<std::string>());
            result["output"] = console.output;
        }
        else if (op == "load")
            result["loaded"] = GetContext()->LoadParkFromFile(
                command.at("path").get<std::string>(), false, command.value("scenario", false));
        else if (op == "speed")
            gGameSpeed = static_cast<uint8_t>(command.at("value").get<int>());
        else if (op == "pause")
        {
            if (command.value("value", true) != GameIsPaused())
                PauseToggle();
        }
        else if (op == "cold")
        {
            result["view_history"] = gPaintViewHistory;
            result["small_zoom3"] = gPaintSmallZoom3;
            result["viewport_damage"] = gPaintViewportDamage;
            result["scroll_ahead"] = gPaintScrollAhead;
            result["horizontal_cold"] = gPaintHorizontalCold;
            result["zoom_ahead"] = gPaintZoomAhead;
            result["window_clip"] = gPaintWindowClip;
            result["compact_cold_snapshot"] = gPaintCompactColdSnapshot;
            result["compact_cold_index"] = gPaintCompactColdIndex;
            result["cold_one_pass"] = gPaintColdOnePass;
            result["compact_entries"] = gPaintCompactEntries;
            result["partial_zoom_pictures"] = gPaintPartialZoomPictures;
            result["column_budget"] = gPaintColumnBuildBudget;
            result["cold_row_grid"] = gPaintColdRowGrid;
            result["cold_cull"] = gPaintColdCull;
            result["history_bytes"] = gPaintHistoryBytes;
            result["history_sets"] = gPaintHistorySets;
            result["storage"] = ViewportColdStorageMetrics();
            result["column_memory"] = ViewportColdColumnMemory();
#if defined(__linux__)
            if (ColdAllocationStats != nullptr)
            {
                std::array<uint64_t, 8> heap{};
                ColdAllocationStats(heap.data());
                result["heap"] = heap;
            }
#endif
            result["column_deferrals"] = gPaintColumnBuildDeferrals;
            result["viewport_skips"] = gPaintViewportDamageSkips;
            result["columns"] = ViewportColumnCacheMetrics();
        }
        else if (op == "cache")
        {
            if (command.contains("entity_peep_guard")) gEntityPeepTightGuard = command["entity_peep_guard"].get<bool>();
            result["entity_peep_guard"] = gEntityPeepTightGuard;
            if (command.contains("present_aligned_copy")) ShimHost::gPresentAlignedCopy = command["present_aligned_copy"].get<bool>();
            result["present_aligned_copy"] = ShimHost::gPresentAlignedCopy;

            if (command.contains("input_cache_hover")) Ui::gCacheHoverPicking = command["input_cache_hover"].get<bool>();
            result["input_cache_hover"] = Ui::gCacheHoverPicking;
            if (command.contains("input_spatial_peeps"))
                Ui::gSpatialPeepPicking = command.at("input_spatial_peeps").get<bool>();
            result["input_spatial_peeps"] = Ui::gSpatialPeepPicking;
            if (command.contains("input_reuse_hover")) Ui::gReuseHoverPicking = command["input_reuse_hover"].get<bool>();
            result["input_reuse_hover"] = Ui::gReuseHoverPicking;
            if (command.contains("flatride_legacy"))
                gPaintFlatrideLegacy = command.at("flatride_legacy").get<bool>();
            if (command.contains("flatride_lookup_legacy"))
                gPaintFlatrideLookupLegacy = command.at("flatride_lookup_legacy").get<bool>();
            if (command.contains("sim_ordered_ids"))
                gUseOrderedEntityLists = command.at("sim_ordered_ids").get<bool>();
            result["sim_ordered_ids"] = gUseOrderedEntityLists;
            if (command.contains("sim_crowd_fused"))
                gUseFusedCrowdNoise = command.at("sim_crowd_fused").get<bool>();
            result["sim_crowd_fused"] = gUseFusedCrowdNoise;
            if (command.contains("sim_train_ordered"))
                TrainManager::gUseOrderedTrainHeads = command.at("sim_train_ordered").get<bool>();
            result["sim_train_ordered"] = TrainManager::gUseOrderedTrainHeads;
            if (command.contains("sim_vehicle_local")) gUseVehicleLocalMetadata = command.at("sim_vehicle_local").get<bool>();
            result["sim_vehicle_local"] = gUseVehicleLocalMetadata;
            if (command.contains("interp_ordered_ids"))
                gInterpolationOrderedIds = command.at("interp_ordered_ids").get<bool>();
            result["interp_ordered_ids"] = gInterpolationOrderedIds;
            if (command.contains("interp_clip_bounds"))
                gInterpolationClipBounds = command.at("interp_clip_bounds").get<bool>();
            result["interp_clip_bounds"] = gInterpolationClipBounds;
            if (command.contains("entity_flush_views"))
                gEntityFlushPreparedViews = command.at("entity_flush_views").get<bool>();
            result["entity_flush_views"] = gEntityFlushPreparedViews;
#ifdef OPENRCT2_LEGACY_GUEST_LAYOUT
            result["sim_guest_hot"] = false;
#else
            result["sim_guest_hot"] = true;
#endif

            if (command.contains("merge_tiles"))
                gPaintMergeEntityTiles = command.at("merge_tiles").get<bool>();
            if (command.contains("stable_signs"))
                OpenRCT2::Drawing::ScrollingText::gScrollingTextStable = command.at("stable_signs").get<bool>();
            if (command.contains("animation_interval"))
                gPaintAnimationInterval = command.at("animation_interval").get<int>();
            if (command.contains("present_changed"))
                ::gPresentChangedOnly = command.at("present_changed").get<bool>();
            result["present_changed"] = { ShimHost::gPresentChangedPixels, ShimHost::gPresentComparedPixels, static_cast<uint64_t>(::gPresentChangedState) };
            if (command.contains("compact_moving"))
                gPaintCompactMoving = command.at("compact_moving").get<bool>();
            if (command.contains("compact_local_lists"))
                gPaintCompactLocalLists = command.at("compact_local_lists").get<bool>();
            if (command.contains("bmp_local_state"))
                OpenRCT2::Drawing::gSpriteBmpLocalState = command.at("bmp_local_state").get<bool>();
            if (command.contains("shared_moving_arena"))
                gPaintSharedMovingArena = command.at("shared_moving_arena").get<bool>();
            if (command.contains("retain_sign_sites"))
                gPaintRetainSignSites = command.at("retain_sign_sites").get<bool>();
            if (command.contains("entrance_overlays"))
            {
                const bool enabled = command.at("entrance_overlays").get<bool>();
                if (gPaintEntranceOverlays != enabled) Drawing::GfxInvalidateScreen();
                gPaintEntranceOverlays = enabled;
            }
            if (command.contains("byte_owners"))
                gPaintByteOwners = command.at("byte_owners").get<bool>();
            if (command.contains("row_copy_batch"))
                gPaintRowCopyBatch = command.at("row_copy_batch").get<bool>();
            if (command.contains("row_prefetch_source"))
                gPaintRowPrefetchSource = std::clamp(command.at("row_prefetch_source").get<int>(), 0, 16);
            if (command.contains("row_prefetch_target"))
                gPaintRowPrefetchTarget = std::clamp(command.at("row_prefetch_target").get<int>(), 0, 16);
            if (command.contains("owner_prefetch_rows"))
                OpenRCT2::Drawing::gSpriteOwnerPrefetchRows = std::clamp(command.at("owner_prefetch_rows").get<int>(), 0, 16);
            if (command.contains("present_prefetch_rows"))
                ShimHost::gPresentPrefetchRows = std::clamp(command.at("present_prefetch_rows").get<int>(), 0, 16);
            if (command.contains("peep_entries"))
                gPaintPeepEntries = command.at("peep_entries").get<bool>();
            if (command.contains("entity_batch_checks"))
                gPaintEntityBatchChecks = command.at("entity_batch_checks").get<bool>();
            if (command.contains("direct_rank_reuse"))
                gPaintDirectRankReuse = command.at("direct_rank_reuse").get<bool>();
            result["painter_controls"] = { { "flatride_legacy", gPaintFlatrideLegacy },
                { "flatride_lookup_legacy", gPaintFlatrideLookupLegacy },
                { "entrance_overlays", gPaintEntranceOverlays },
                { "compact_moving", gPaintCompactMoving }, { "direct_rank_reuse", gPaintDirectRankReuse },
                { "compact_local_lists", gPaintCompactLocalLists },
                { "bmp_local_state", OpenRCT2::Drawing::gSpriteBmpLocalState },
                { "shared_moving_arena", gPaintSharedMovingArena }, { "byte_owners", gPaintByteOwners }, { "retain_sign_sites", gPaintRetainSignSites }, { "row_copy_batch", gPaintRowCopyBatch }, { "peep_entries", gPaintPeepEntries },
                { "row_prefetch_source", gPaintRowPrefetchSource }, { "row_prefetch_target", gPaintRowPrefetchTarget },
                { "present_prefetch_rows", ShimHost::gPresentPrefetchRows },
                { "owner_prefetch_rows", OpenRCT2::Drawing::gSpriteOwnerPrefetchRows } };
            if (command.contains("direct_owners"))
                gPaintDirectOwners = command.at("direct_owners").get<bool>();
            result["direct"] = { gPaintDirectPasses, gPaintDirectFallbacks, gPaintDirectOwnerTests };
            for (int type = 0; type < 128; type++)
                for (int station = 0; station < 2; station++)
                    if (gPaintCensusRide[type][station][0] != 0)
                        result["census_ride"].push_back({ type, station, gPaintCensusRide[type][station][0], gPaintCensusRide[type][station][1] });
            result["patch_kinds"] = { gPaintPatchKinds[0], gPaintPatchKinds[1], gPaintPatchKinds[2] };
            for (int kind = 0; kind < 12; kind++)
                result["census"].push_back({ gPaintCensus[kind][0], gPaintCensus[kind][1], gPaintCensus[kind][2] });
            if (command.contains("sprite_catalogue"))
                gPaintFastSpriteCatalogue = command.at("sprite_catalogue").get<bool>();
            if (command.contains("weather_reuse_columns"))
                Drawing::gWeatherReuseColumns = command.at("weather_reuse_columns").get<bool>();
            result["weather_reuse_columns"] = Drawing::gWeatherReuseColumns;
            if (command.contains("canvas_reuse_columns")) Drawing::gCanvasReuseColumns = command["canvas_reuse_columns"].get<bool>();
            result["canvas_reuse_columns"] = Drawing::gCanvasReuseColumns;
#ifdef OPENRCT2_PAK_VFS
            if (command.contains("scenario_reuse_index")) gScenarioReuseIndex = command["scenario_reuse_index"].get<bool>();
            result["scenario_reuse_index"] = gScenarioReuseIndex;
            if (command.contains("scenario_pack_census")) ClassicVfs::gCachePackCensus = command["scenario_pack_census"].get<bool>();
            result["scenario_pack_census"] = ClassicVfs::gCachePackCensus;
            result["scenario_index_us"] = {gScenarioIndexTiming.censusUs,gScenarioIndexTiming.metadataUs,
                gScenarioIndexTiming.sortUs,gScenarioIndexTiming.scoresUs,gScenarioIndexTiming.attachUs};
#endif
            if (command.contains("wide_present"))
                ShimHost::gWidePresent = command.at("wide_present").get<bool>();
            if (command.contains("batch_cursor"))
                ShimHost::gBatchCursor = command.at("batch_cursor").get<bool>();
            if (command.contains("damage_rows"))
                gPaintDamageEntityRows = command.at("damage_rows").get<bool>();
            if (command.contains("damage_mask"))
                gPaintDamageEntityMask = command.at("damage_mask").get<bool>();
            if (command.contains("idle_refresh"))
                ShimHost::SetIdleRefresh(command.at("idle_refresh").get<bool>());
            if (command.contains("smooth_scroll_recovery"))
                Config::Get().general.smoothScrollRecovery = command.at("smooth_scroll_recovery").get<bool>();
            result["smooth_scroll_recovery"] = Config::Get().general.smoothScrollRecovery;
            if (command.contains("stream_paint_columns"))
                Config::Get().general.streamPaintColumns = command.at("stream_paint_columns").get<bool>();
            result["stream_paint_columns"] = Config::Get().general.streamPaintColumns;
            if (command.contains("word_rects"))
                ShimHost::SetWordRectangles(command.at("word_rects").get<bool>());
            if (command.contains("partial_patches"))
                gPaintPartialPatches = command.at("partial_patches").get<bool>();
            if (command.contains("defer_picture_copy"))
                gPaintDeferPictureCopy = command.at("defer_picture_copy").get<bool>();
            if (command.contains("align_cells"))
                ShimHost::SetAlignedCells(command.at("align_cells").get<bool>());
            if (command.contains("animation_scan"))
                gPaintCacheAnimationTiles = command.at("animation_scan").get<bool>();
            if (command.contains("sparse_init"))
                gPaintSparseInit = command.at("sparse_init").get<bool>();
            if (command.contains("visible_entities"))
                gPaintVisibleEntities = command.at("visible_entities").get<bool>();
            if (command.contains("frame_scan"))
                gPaintFrameScan = command.at("frame_scan").get<bool>();
            if (command.contains("skip_unchanged_roots"))
                gPaintSkipUnchangedRoots = command.at("skip_unchanged_roots").get<bool>();
            if (command.contains("memo_lookup"))
                gPaintMemoLookup = command.at("memo_lookup").get<bool>();
            if (command.contains("sparse_ranks"))
                gPaintSparseRanks = command.at("sparse_ranks").get<bool>();
            if (command.contains("fast_clock"))
                ShimHost::SetFastPaintClock(command.at("fast_clock").get<bool>());
            if (command.contains("fast_visibility"))
                gPaintFastVisibility = command.at("fast_visibility").get<bool>();
            if (command.contains("refresh_columns"))
                gPaintRefreshColumns = command.at("refresh_columns").get<bool>();
            if (command.contains("patch_slots"))
                gPaintPatchSlots = std::clamp<uint32_t>(command.at("patch_slots").get<uint32_t>(), 16, 256);
            if (command.contains("sparse_entities"))
                gPaintSparseEntities = command.at("sparse_entities").get<bool>();
            if (command.contains("entity_projection"))
                gPaintEntityProjection = command.at("entity_projection").get<bool>();
            if (command.contains("memo_maps"))
                gPaintMemoMaps = command.at("memo_maps").get<bool>();
            if (command.contains("column_lookup"))
                gPaintColumnLookup = command.at("column_lookup").get<bool>();
            if (command.contains("patch_scratch"))
                gPaintRetainPatchScratch = command.at("patch_scratch").get<bool>();
            if (command.contains("fast_copy"))
                gPaintFastCopy = command.at("fast_copy").get<bool>();
            if (command.contains("ordered_blends"))
                gPaintOrderedBlends = command.at("ordered_blends").get<bool>();
            if (command.contains("blend_coverage"))
                gPaintBlendCoverage = command.at("blend_coverage").get<bool>();
            if (command.contains("sort_halo1"))
                gPaintSortHalo1 = command.at("sort_halo1").get<int32_t>();
            if (command.contains("sort_halo2"))
                gPaintSortHalo2 = command.at("sort_halo2").get<int32_t>();
            if (command.contains("sort_halo"))
                gPaintSortHalo = command.at("sort_halo").get<int32_t>();
            if (command.contains("selected_sort"))
                gPaintSelectedSort = command.at("selected_sort").get<bool>();
            if (command.contains("animated_maps"))
                gPaintAnimatedMaps = command.at("animated_maps").get<bool>();
            if (command.contains("owners"))
                gPaintOwnerPictures = command.at("owners").get<bool>();
            if (command.contains("occlusion_rle"))
                Drawing::gSpriteOcclusionEnabled = command.at("occlusion_rle").get<bool>();
            if (command.contains("owner_rle"))
                Drawing::gSpriteOwnerRle = command.at("owner_rle").get<bool>();
            if (command.contains("defer_palette"))
                ShimHost::SetDeferredPalette(command.at("defer_palette").get<bool>());
            if (command.contains("coalesce"))
            {
                FlushEntityInvalidations();
                gCoalesceEntityInvalidation = command.at("coalesce").get<bool>();
            }
            if (command.contains("picture_budget")) gPaintPictureBuildBudget = std::clamp(command.at("picture_budget").get<unsigned>(), 0u, 128u);
            if (command.contains("view_history")) gPaintViewHistory = command.at("view_history").get<bool>();
            if (command.contains("small_zoom3")) gPaintSmallZoom3 = command.at("small_zoom3").get<bool>();
            if (command.contains("viewport_damage")) gPaintViewportDamage = command.at("viewport_damage").get<bool>();
            if (command.contains("scroll_ahead")) gPaintScrollAhead = command.at("scroll_ahead").get<bool>();
            if (command.contains("horizontal_cold")) gPaintHorizontalCold = command.at("horizontal_cold").get<bool>();
            if (command.contains("zoom_ahead")) gPaintZoomAhead = std::clamp(command.at("zoom_ahead").get<int>(), -1, 2);
            if (command.contains("window_clip")) gPaintWindowClip = command.at("window_clip").get<bool>();
            if (command.contains("compact_cold_snapshot")) gPaintCompactColdSnapshot = command.at("compact_cold_snapshot").get<bool>();
            if (command.contains("compact_cold_index")) gPaintCompactColdIndex = command.at("compact_cold_index").get<bool>();
            if (command.contains("cold_one_pass")) gPaintColdOnePass = command.at("cold_one_pass").get<bool>();
            if (command.contains("compact_entries")) gPaintCompactEntries = command.at("compact_entries").get<bool>();
            if (command.contains("partial_zoom_pictures")) gPaintPartialZoomPictures = command.at("partial_zoom_pictures").get<bool>();
            if (command.contains("cold_cull")) gPaintColdCull = command.at("cold_cull").get<bool>();
            if (command.contains("history_sets")) gPaintHistorySets = std::clamp(command.at("history_sets").get<unsigned>(), 1u, 8u);
            if (command.contains("history_bytes"))
                gPaintHistoryBytes = std::min<uint64_t>(command.at("history_bytes").get<uint64_t>(), 64 * 1024 * 1024);
            if (command.contains("column_budget")) gPaintColumnBuildBudget = std::clamp(command.at("column_budget").get<unsigned>(), 0u, 128u);
            if (command.contains("cold_row_grid"))
            {
                const int32_t grid = command.at("cold_row_grid").get<int32_t>();
                if (grid != 32 && grid != 64 && grid != 128) throw std::runtime_error("cold_row_grid must be 32, 64 or 128");
                gPaintColdRowGrid = grid;
            }
            result["cold_controls"] = { {"view_history", gPaintViewHistory}, {"small_zoom3", gPaintSmallZoom3},
                {"scroll_ahead", gPaintScrollAhead}, {"horizontal_cold", gPaintHorizontalCold}, {"zoom_ahead", gPaintZoomAhead}, {"window_clip", gPaintWindowClip},
                {"viewport_damage", gPaintViewportDamage}, {"compact_cold_snapshot", gPaintCompactColdSnapshot},
                {"compact_cold_index", gPaintCompactColdIndex}, {"cold_one_pass", gPaintColdOnePass}, {"compact_entries", gPaintCompactEntries},
                {"partial_zoom_pictures", gPaintPartialZoomPictures}, {"column_budget", gPaintColumnBuildBudget},
                {"cold_row_grid", gPaintColdRowGrid}, {"column_deferrals", gPaintColumnBuildDeferrals},
                {"viewport_skips", gPaintViewportDamageSkips} };
            gPaintColumnCache = command.value("on", true);
            if (command.contains("visibility"))
                gPaintVisibilityCache = command.at("visibility").get<bool>();
            result["visibility"] = gPaintVisibilityCache;
            if (command.contains("visibility_repairs"))
                gPaintVisibilityRepairs = command.at("visibility_repairs").get<bool>();
            result["visibility_repairs"] = gPaintVisibilityRepairs;
            if (command.contains("sign_regions"))
            {
                const auto mask = command.at("sign_regions").get<uint32_t>();
                if (mask > 7) throw std::runtime_error("sign_regions must be 0..7");
                gPaintSignRegions = mask;
            }
            result["sign_regions"] = gPaintSignRegions;
            result["sign_region_updates"] = {gPaintSignRegionRefreshes[0], gPaintSignRegionRefreshes[1], gPaintSignRegionRefreshes[2]};
            result["sign_region_pixels"] = gPaintSignRegionPixels;
            result["sign_region_entries"] = gPaintSignRegionEntries;
            gPaintMapPictures = command.value("pictures", true);
            ViewportColumnCacheReset();
            result["column_builds"] = gPaintColumnBuilds;

        }
#ifndef MACINTOSH
        else if (op == "cold_fixture")
            result = ColdHistoryFixture::run(command);
        else if (op == "placement")
            result = PlacementFixture::run(command);
#endif
        else if (op == "seed")
        {
            ScenarioRandSeed(command.value("a", 0x1234567Fu), command.value("b", 0x789FABCDu));
            if (command.contains("util"))
                UtilRandSeed(command.at("util").get<uint32_t>());
        }
#ifdef OPENRCT2_PAK_VFS
        else if (op == "moveinto")
        {
            // Housekeeping on the Mac: native paths, e.g. "Disk:Old Folder" into "Disk:Archive".
            result["error_code"] = ClassicHost::MoveInto(
                command.at("path").get<std::string>(), command.at("folder").get<std::string>());
        }
        else if (op == "file")
        {
            // Does the game's file layer see this path, and how big is it?
            const auto path = command.at("path").get<std::string>();
            result["exists"] = File::Exists(path);
            if (File::Exists(path))
                result["size"] = File::ReadAllBytes(path).size();
        }
#endif
        else if (op == "rects")
            result["frame"] = json::parse(ShimHost::LastFrameRects());
        else if (op == "lightning")
        {
            // Host fixture: use the real palette flash without waiting for UtilRand.
            Weather::gLightningFlash = 1;
        }
        else if (op == "weather")
        {
            // 0 sunny, 1 partially cloudy, 2 cloudy, 3 rain, 4 heavy rain, 5 thunder, ...
            if (command.contains("filter_unroll"))
                Drawing::gPaletteFilterUnroll = command.at("filter_unroll").get<bool>();
            if (command.contains("type") || (!command.contains("filter_unroll") && !command.contains("gloom")))
                Weather::forceWeather(static_cast<Weather::Type>(command.value("type", 4)));
            if (command.contains("gloom"))
            {
                getGameState().weatherCurrent.weatherGloom = std::clamp(command.at("gloom").get<int>(), 0, 2);
                Drawing::GfxInvalidateScreenForWeather();
            }
            result["gloom"] = getGameState().weatherCurrent.weatherGloom;
            result["filter_unroll"] = Drawing::gPaletteFilterUnroll;
        }
#ifdef OPENRCT2_CLASSIC_MAC
        else if (op == "vsync")
            result["vsync"] = json::parse(ShimHost::VSyncDiagnostics(command.dump()));
        else if (op == "yield")
        {
            if (command.contains("poll")) ShimHost::SetEventPolling(command["poll"].get<bool>());
            result["poll"] = ShimHost::EventPolling();
            if (command.contains("trace")) result["trace"] = json::parse(ShimHost::EventDiagnostics(command["trace"].dump()));
        }
#endif
        else if (op == "pumptrace")
        {
            ShimHost::gTracePump = command.value("on", false);
            result["on"] = ShimHost::gTracePump;
#ifdef OPENRCT2_CLASSIC_MAC
            if (command.value("probe", false))
                result["call_us"] = json::parse(ShimHost::EventCallProbe());
#endif
        }
        else if (op == "sound")
        {
            // Off stops every sound (the game's own pause); on resumes.
            // "limit": our safety limit on output in percent (0 mutes completely).
#ifdef OPENRCT2_PAK_VFS
            if (command.contains("mixer"))
                result["mixer_changed"] = Audio::ClassicAudioSetMixerBackend(command.at("mixer").get<unsigned>(),
                    command.value("bits", 16u), command.value("block_frames", 1024u));
            if (command.contains("probe_mode"))
                result["probe_changed"] = Audio::ClassicAudioSetProbeMode(command.at("probe_mode").get<unsigned>());
            if (command.contains("trace")) Audio::ClassicAudioTrace(command.at("trace").get<bool>());
            if (command.contains("stall_ms")) result["stall_checksum"] = Audio::ClassicAudioStallForProbe(command.at("stall_ms").get<unsigned>());
            if (command.contains("limit"))
                Audio::ClassicAudioSetOutputLimit(command["limit"].get<int>() / 100.0f);
#endif
            if (command.value("on", true))
                Audio::Resume();
            else
                Audio::Pause();
        }
        else if (op == "effect")
        {
            const auto id = static_cast<Audio::SoundId>(command.at("id").get<int>());
            auto channel = Audio::CreateAudioChannel(id, false);
            result["created"] = channel != nullptr;
        }
        else if (op == "silentaudio")
        {
#ifdef OPENRCT2_PAK_VFS
            result["ok"] = Audio::ClassicAudioSetSilentPlaybackTest(command.value("on", false));
#else
            result["ok"] = false;
            result["error"] = "Classic audio is unavailable in this build";
#endif
        }
        else if (op == "audioloop")
        {
            if (command.value("stop", false)) gAudioTestLoops.clear();
            else
            {
                auto channel = Audio::CreateAudioChannel(Audio::SoundId::liftClassic, true);
                if (channel == nullptr) { result["ok"] = false; result["error"] = "sound source unavailable"; }
                else
                {
                    if (command.contains("rate")) channel->SetRate(command.at("rate").get<double>());
                    if (command.contains("pan")) channel->SetPan(command.at("pan").get<float>());
                    if (command.contains("offset")) result["offset_set"] = channel->SetOffset(command.at("offset").get<uint64_t>());
                    gAudioTestLoops.push_back(std::move(channel));
                }
            }
        }
        else if (op == "audiocpu")
        {
#ifdef OPENRCT2_PAK_VFS
            const auto limit = Audio::ClassicAudioGetStats().outputLimitPercent;
            if (limit != 0 && limit != 1) throw std::runtime_error("CPU audio probe requires mute or the 1% launch limit");
            const auto mode = command.value("mode", std::string("none"));
            const unsigned voices = std::clamp(command.value("voices", 0u), 0u, 16u);
            const double multiplier = std::clamp(command.value("rate",1.0),.1,2.0);
            const bool compressed = mode == "ima22";
            if (mode != "none" && mode != "pcm22" && mode != "pcm44" && !compressed)
                throw std::runtime_error("unknown CPU audio probe format");
            if (voices && limit == 0 && !Audio::ClassicAudioGetStats().silentPlaybackTest) throw std::runtime_error("voice probe requires the verified 1% launch limit");
            const auto device = command.value("device", std::string("keep"));
            if (device != "keep" && device != "closed" && device != "idle" && device != "clock" && device != "playing")
                throw std::runtime_error("unknown audio probe device state");
            if (device != "keep" && device != "playing" && voices) throw std::runtime_error("nonplaying device requires zero voices");
            const unsigned bytes = command.value("work_bytes", 0u);
            if (bytes != 0 && bytes != 65536 && bytes != 2097152) throw std::runtime_error("audio work bytes must be 0, 65536 or 2097152");
            AudioWorkProbe::Work work(bytes);
            const unsigned samples = std::clamp(command.value("samples", 1u), 1u, 5u);
            const bool wasOff = Audio::gGameSoundsOff;
            Audio::Pause();
            auto& audio = GetContext()->GetAudioContext();
            auto* mixer = audio.GetMixer();
            Audio::ClassicAudioStopAllForProbe();
            mixer->Close();
            Audio::ClassicAudioTick();
            if (device != "keep")
            {
                const unsigned probe = device == "closed" ? 1 : device == "idle" ? 2 : device == "clock" ? 3 : 0;
                if (!Audio::ClassicAudioSetProbeMode(probe)) throw std::runtime_error("audio device isolation refused");
            }
            std::vector<std::shared_ptr<Audio::IAudioChannel>> channels;
            Audio::IAudioSource* source = nullptr;
            if (voices && mode != "none")
            {
                const unsigned rate = mode == "pcm44" ? 44100 : 22050;
                const unsigned frames = command.value("source_frames", (rate + 63) / 64 * 64);
                if (!frames || frames > rate * 2 || frames % 64) throw std::runtime_error("invalid diagnostic source frames");
                std::vector<uint8_t> bytes(20 + (compressed ? frames / 64 * 34 : frames * 2), 0);
                std::memcpy(bytes.data(), "RCTA", 4);
                std::memcpy(bytes.data()+4, compressed ? "ima4" : "twos", 4);
                for (unsigned j=0;j<4;j++) { bytes[8+j] = rate >> (24-j*8); bytes[16+j] = frames >> (24-j*8); }
                bytes[13]=1; bytes[15]=16;
                // Zero PCM and valid zero-predictor IMA packets: no audible
                // test signal. They exercise the normal Sound Manager path.
                source = audio.CreateStreamFromWAV(std::make_unique<MemoryStream>(bytes));
                if (!source) throw std::runtime_error("synthetic source unavailable");
                for (unsigned j=0;j<voices;j++)
                {
                    auto ch=mixer->Play(source,1,false);
                    ch->SetGroup(Audio::MixerGroup::sound); ch->SetVolume(Audio::kMixerVolumeMax);
                    ch->SetPan(.5f); ch->SetRate(multiplier); ch->UpdateOldVolume(); channels.push_back(std::move(ch));
                }
                Audio::ClassicAudioTick();
            }
            const auto deviceStats = Audio::ClassicAudioGetStats();
            if (deviceStats.audioTrace) Audio::ClassicAudioTrace(true); // resolve/restart after physical reopen
            const auto before = Audio::ClassicAudioGetFrame();
            const unsigned iterations=std::clamp(command.value("iterations",10000000u),10000u,100000000u);
            std::vector<uint64_t> elapsedSamples; elapsedSamples.reserve(samples);
            std::vector<unsigned long> callbackSamples; callbackSamples.reserve(samples);
            uint32_t value = 0;
            for (unsigned sample = 0; sample < samples; ++sample)
            {
                Audio::ClassicAudioTick(); // producer outside timing, then warm the measured working set
                work.Warm();
                const auto first = Audio::ClassicAudioGetFrame();
                const auto began = ShimHost::Microseconds();
                value = work.Run(iterations);
                elapsedSamples.push_back(ShimHost::Microseconds() - began);
                callbackSamples.push_back(Audio::ClassicAudioGetFrame().callbacks - first.callbacks);
            }
            result={{"mode",mode},{"rate",multiplier},{"voices",before.voices},{"ima",before.ima},{"pcm",before.pcm},
                {"iterations",iterations},{"checksum",value},{"elapsed_us",elapsedSamples.front()},
                {"samples_us",elapsedSamples},{"callbacks_timed",callbackSamples},{"device",device},{"work_bytes",bytes},
                {"probe_mode",deviceStats.probeMode},{"live_channels",deviceStats.channelsAllocated-deviceStats.channelsDisposed},
                {"output_limit_percent",deviceStats.outputLimitPercent},{"hardware_frames",deviceStats.hardwareFrames}};
            for (auto& ch:channels) ch->Stop();
            channels.clear(); if (source) source->Release(); Audio::ClassicAudioTick();
            if (device != "keep") Audio::ClassicAudioSetProbeMode(0);
            if (!wasOff) Audio::Resume();
#else
            throw std::runtime_error("Classic audio unavailable");
#endif
        }
        else if (op == "solidbars")
        {
            Config::Get().interface.originalSolidBars = command.value("on", true);
            Ui::Windows::RefreshOriginalHud();
            if (command.value("save", false)) Config::Save();
            result["on"] = Config::Get().interface.originalSolidBars;
        }
#ifndef OPENRCT2_CLASSIC_MAC
        else if (op == "nomoney")
        {
            // Host fixture: exercise toolbar visibility without the remote
            // console's asynchronous action callback retaining a temporary.
            getGameState().park.flags.set(ParkFlag::noMoney, command.value("on", true));
            Ui::GetWindowManager()->InvalidateByClass(WindowClass::topToolbar);
            Ui::GetWindowManager()->InvalidateByClass(WindowClass::parkInfoPanel);
        }
        else if (op == "saveview")
        {
            // Host fixture: use the ordinary save path, in the test's private user directory.
            PrepareMapForSave();
            SaveGameWithName(command.at("path").get<std::string>());
            const auto& state = getGameState();
            result["centre"] = {state.savedView.x, state.savedView.y};
            result["zoom"] = static_cast<int8_t>(state.savedViewZoom);
            result["rotation"] = state.savedViewRotation;
        }
        else if (op == "scene")
        {
            auto* scenes = GetContext()->GetSceneManager();
            const auto kind = command.at("kind").get<std::string>();
            if (kind == "title") scenes->setActiveScene(scenes->getTitleScene());
            else if (kind == "editor") scenes->setActiveScene(scenes->getScenarioEditorScene());
            else if (kind == "designer") scenes->setActiveScene(scenes->getTrackDesignerScene());
            else if (kind == "manager") scenes->setActiveScene(scenes->getTrackManagerScene());
            else throw std::runtime_error("unknown fixture scene");
        }
        else if (op == "locate")
        {
            const CoordsXY xy{command.at("x").get<int>(), command.at("y").get<int>()};
            WindowScrollToLocation(*WindowGetMain(), {xy.x,xy.y,command.value("z",TileElementHeight(xy))});
        }
#endif
        else if (op == "close")
            Ui::GetWindowManager()->CloseAllExceptFlags({ WindowFlag::stickToBack, WindowFlag::stickToFront });
        else if (op == "uiframe")
        {
            // Deterministic comparison fixture: stop tab sprites changing between frozen-world shots.
            const auto frame = command.value("value", 0);
            WindowVisitEach([frame](WindowBase* window) {
                if (window == WindowGetMain()) return;
                window->currentFrame = frame;
                window->invalidate();
            });
        }
#ifdef OPENRCT2_CLASSIC_MAC
        else if (op == "cursor")
            result = json::parse(ShimHost::CursorDiagnostics(command.value("reset", false),command.dump()));
#endif
        else if (op == "display")
            result = json::parse(ShimHost::DisplayDiagnostics(command.value("verify", false)));
        else if (op == "resolution")
        {
            const int width = command.value("width", 0), height = command.value("height", 0);
            const auto& modes = ShimHost::DisplayModes();
            if (std::none_of(modes.begin(), modes.end(), [=](const auto& mode) {
                return mode.width == width && mode.height == height;
            })) throw std::runtime_error("resolution unavailable at 8-bit");
            auto& config = Config::Get().general;
            config.fullscreenWidth = width;
            config.fullscreenHeight = height;
            if (command.value("now",false))
            {
                ContextSetFullscreenMode(static_cast<int32_t>(Ui::FullscreenMode::fullscreen));
                GetContext()->GetUiContext().TriggerResize();
            }
            Config::Save();
            result = {{"next_launch", {width, height}},{"canvas",{ContextGetWidth(),ContextGetHeight()}}};
        }
        else if (op == "redraw")
            Drawing::GfxInvalidateScreen();
        else if (op == "hidefar")
        {
            const auto mode = command.value("mode", command.value("on", true) ? "hide" : "show");
            if (mode != "show" && mode != "auto" && mode != "hide")
                throw std::runtime_error("hidefar mode must be show, auto or hide");
            const auto setting = mode == "show" ? FarEntityMode::show
                : mode == "auto"               ? FarEntityMode::automatic
                                              : FarEntityMode::hide;
            if (Config::Get().general.farEntityMode != setting)
            {
                Config::Get().general.farEntityMode = setting;
                ViewportsResetFarEntities();
            }
            Drawing::GfxInvalidateScreen();
            result = Status(false);
        }
#ifdef OPENRCT2_PERF_METRICS
        else if (op == "perf")
        {
            result = command.value("pan",false) && !gPanPerfResult.is_null() ? gPanPerfResult : PerfCapture();
            result["instrumentation"] = "measurement only; version 0.3.1";
            if (command.value("reset", false))
            {
                std::fill_n(Perf::scrollWindows, 256, 0); std::fill_n(Perf::dirtyCalls, Perf::sourceCount, 0); std::fill_n(Perf::dirtyPixels, Perf::sourceCount, 0);
                std::fill_n(Perf::guestChanges, 4, 0); std::fill_n(Perf::spriteCalls, Perf::kindCount, 0);
                std::fill_n(Perf::spriteBoxes, Perf::kindCount, 0); std::fill_n(Perf::spriteTicks, Perf::kindCount, 0); std::fill_n(Perf::writes, Perf::kindCount, 0);
                std::fill_n(Perf::canvasWrites, Perf::kindCount, 0); Perf::repeatedWrites = Perf::tracedFrames = 0;
                std::fill_n(Perf::hudWidgetCalls, 256, 0);
                Perf::tooltipVisits = Perf::tooltipSame = 0;
                for (unsigned i=0; i<256; ++i)
                {
                    std::fill_n(Perf::hudCalls[i], Perf::hudReasonCount, 0);
                    std::fill_n(Perf::hudPixels[i], Perf::hudReasonCount, 0);
                }
            }
            Perf::enabled = command.value("on", Perf::enabled);
            Perf::pixelsEnabled = Perf::enabled && command.value("pixels", false);
            result["on"] = Perf::enabled;
        }
        else if (op == "hud")
        {
            result["frames"] = Perf::tracedFrames;
            result["theme_flags"] = Ui::ThemeGetFlags();
            result["sounds_off"] = Audio::gGameSoundsOff;
            result["tooltip_visits"] = Perf::tooltipVisits;
            result["main_occlusion"] = Ui::Windows::MainHudOcclusionMetrics(command.value("reset_occlusion", false));
            result["tooltip_same"] = Perf::tooltipSame;
            result["reasons"] = {"other", "tooltip", "climate", "guest_count", "rating", "date", "cash"};
            result["windows"] = json::array();
            for (unsigned i=0; i<256; ++i)
            {
                uint64_t total = 0;
                for (auto count : Perf::hudCalls[i]) total += count;
                if (total)
                    result["windows"].push_back({{"class",i}, {"calls",Perf::hudCalls[i]},
                        {"pixels",Perf::hudPixels[i]}, {"widget_calls",Perf::hudWidgetCalls[i]}});
            }
            // Count only complete opaque button fills guaranteed by Widget.cpp.
            // Text/sprite pixels and opaque strips in translucent panels are omitted.
            const int width = ContextGetWidth(), height = ContextGetHeight();
            std::vector<uint8_t> hud(size_t(width)*height), opaque(hud.size());
            result["opaque_buttons"] = json::array();
            result["toolbar_images"] = json::array();
            WindowVisitEach([&](WindowBase* window) {
                const auto cls = window->classification;
                if (!window->isVisible || (cls != WindowClass::topToolbar && cls != WindowClass::gameStatusBar
                    && cls != WindowClass::parkInfoPanel && cls != WindowClass::dateInfoPanel)) return;
                window->onPrepareDraw();
                if (cls == WindowClass::topToolbar)
                    for (const auto& widget : window->widgets)
                        if (widget.isVisible() && widget.type == WidgetType::trnBtn && widget.image.HasValue())
                            if (auto* g1 = GfxGetG1Element(widget.image))
                                result["toolbar_images"].push_back({{"image",widget.image.GetIndex()},
                                    {"rle",g1->flags.has(G1Flag::hasRLECompression)}, {"width",g1->width}, {"height",g1->height}});
                auto mark = [&](std::vector<uint8_t>& mask, int l, int t, int r, int b) {
                    for (int y=std::max(0,t); y<std::min(height,b); ++y)
                        for (int x=std::max(0,l); x<std::min(width,r); ++x) mask[size_t(y)*width+x]=1;
                };
                mark(hud,window->windowPos.x,window->windowPos.y,
                     window->windowPos.x+window->width,window->windowPos.y+window->height);
                for (const auto& widget : window->widgets)
                {
                    if (!widget.isVisible() || widget.colour >= std::size(window->colours)
                        || window->colours[widget.colour].flags.has(ColourFlag::translucent)) continue;
                    if (widget.type != WidgetType::imgBtn && !(widget.type == WidgetType::flatBtn
                        && (Ui::ThemeGetFlags() & Ui::UITHEME_FLAG_USE_3D_IMAGE_BUTTONS))) continue;
                    const int l=window->windowPos.x+widget.left, t=window->windowPos.y+widget.top;
                    const int r=window->windowPos.x+widget.right+1, b=window->windowPos.y+widget.bottom+1;
                    mark(opaque,l,t,r,b);
                    result["opaque_buttons"].push_back({l,t,r,b});
                }
            });
            result["hud_area"] = std::count(hud.begin(),hud.end(),1);
            result["opaque_button_area"] = std::count(opaque.begin(),opaque.end(),1);
#ifndef OPENRCT2_CLASSIC_MAC
            if (command.value("opacity", false))
            {
                // Diagnostic only, outside timing: a pixel is opaque iff the HUD
                // gives the same result for every possible underlying palette index.
                const bool counters = Perf::enabled;
                Perf::enabled = false;
                std::vector<uint8_t> candidate(hud.size()), reference(hud.size()), invariant = hud;
                auto rt = DrawingEngineGetRT();
                rt.DrawingEngine->BeginDraw();
                rt.bits = reinterpret_cast<Drawing::PaletteIndex*>(candidate.data());
                rt.x = rt.y = 0; rt.width = width; rt.height = height; rt.pitch = 0;
                for (unsigned background=0; background<256; ++background)
                {
                    std::fill(candidate.begin(),candidate.end(),background);
                    WindowVisitEach([&](WindowBase* window) {
                        const auto cls = window->classification;
                        if (window->isVisible && (cls == WindowClass::topToolbar || cls == WindowClass::gameStatusBar
                            || cls == WindowClass::parkInfoPanel || cls == WindowClass::dateInfoPanel)) window->onDraw(rt);
                    });
                    if (!background) reference = candidate;
                    else for (size_t i=0; i<candidate.size(); ++i)
                        invariant[i] &= candidate[i] == reference[i];
                }
                result["opaque_actual_area"] = std::count(invariant.begin(),invariant.end(),1);
                auto maskPath = command.value("mask", std::string{});
                if (!maskPath.empty())
                {
                    if (auto* file = std::fopen(maskPath.c_str(),"wb"))
                    {
                        std::fprintf(file,"P5\n%d %d\n255\n",width,height);
                        for (auto pixel : invariant) std::fputc(pixel ? 255 : 0,file);
                        std::fclose(file);
                    }
                }
                result["opaque_actual_by_class"] = json::array();
                WindowVisitEach([&](WindowBase* window) {
                    const auto cls = window->classification;
                    if (cls != WindowClass::topToolbar && cls != WindowClass::gameStatusBar
                        && cls != WindowClass::parkInfoPanel && cls != WindowClass::dateInfoPanel) return;
                    uint64_t count = 0;
                    for (int y=std::max(0,int(window->windowPos.y)); y<std::min(height,int(window->windowPos.y+window->height)); ++y)
                        for (int x=std::max(0,int(window->windowPos.x)); x<std::min(width,int(window->windowPos.x+window->width)); ++x)
                            count += invariant[size_t(y)*width+x];
                    result["opaque_actual_by_class"].push_back({{"class",int(cls)},{"pixels",count}});
                });
                Perf::enabled = counters;
                rt.DrawingEngine->EndDraw();
            }
#endif
        }
#endif
#ifndef OPENRCT2_CLASSIC_MAC
        else if (op == "hudtheme")
        {
            // Host-only fixture: exercise the full status bar used on the G3.
            Ui::ThemeManagerSetActiveAvailableTheme(0);
            Ui::GetWindowManager()->CloseByClass(WindowClass::mapTooltip);
            Ui::GetWindowManager()->OpenWindow(WindowClass::gameStatusBar);
            result["flags"] = Ui::ThemeGetFlags();
        }
#endif
#if defined(OPENRCT2_CLASSIC_MAC) && defined(OPENRCT2_PERF_METRICS)
        else if (op == "clock")
        {
            result = json::parse(ShimHost::ClockDiagnostics(command.value("reset", false)));
            result["ok"] = true;
        }
#endif
        else if (op == "sprites")
        {
            result["column_cache"] = ViewportColumnCacheMetrics();
            result["visibility_repair_boxes"] = gPaintVisibilityRepairBoxes;
            result["visibility_repair_pixels"] = gPaintVisibilityRepairPixels;
            result["idle_readback"] = {ShimHost::gIdleReadbackCalls, ShimHost::gIdleReadbackUs};
            result["raster_cache"] = { { "hits", Drawing::gSpriteRasterCacheStats.hits },
                { "misses", Drawing::gSpriteRasterCacheStats.misses }, { "pixels", Drawing::gSpriteRasterCacheStats.pixels },
                { "bytes", Drawing::gSpriteRasterCacheStats.bytes }, { "owner_passes", Drawing::gSpriteRasterCacheStats.ownerPasses },
                { "owner_fallbacks", Drawing::gSpriteRasterCacheStats.ownerFallbacks },
                { "owner_restores", Drawing::gSpriteRasterCacheStats.ownerRestores },
                                       { "patch_hits", Drawing::gSpriteRasterCacheStats.patchHits },
                                       { "patch_total_pixels", Drawing::gSpriteRasterCacheStats.patchTotalPixels },
                                       { "patch_repair_pixels", Drawing::gSpriteRasterCacheStats.patchRepairPixels },
                { "order_hits", Drawing::gSpriteRasterCacheStats.orderHits },
                { "order_nodes_reused", Drawing::gSpriteRasterCacheStats.orderNodesReused },
                { "order_misses", Drawing::gSpriteRasterCacheStats.orderMisses },
                                       { "patch_misses", Drawing::gSpriteRasterCacheStats.patchMisses },
                { "base_us", Drawing::gSpriteRasterCacheStats.baseMicros },
                { "patch_us", Drawing::gSpriteRasterCacheStats.patchMicros },
                { "owner_us", Drawing::gSpriteRasterCacheStats.ownerMicros },
                { "check_us", Drawing::gSpriteRasterCacheStats.checkMicros },
                { "local_filter_us", Drawing::gSpriteRasterCacheStats.localFilterMicros },
                { "local_sort_us", Drawing::gSpriteRasterCacheStats.localSortMicros },
                { "tile_patch_us", Drawing::gSpriteRasterCacheStats.tilePatchMicros },
                { "selection_hits", Drawing::gSpriteRasterCacheStats.selectionHits },
                { "selection_misses", Drawing::gSpriteRasterCacheStats.selectionMisses },
                { "entity_generate_us", Drawing::gSpriteRasterCacheStats.entityGenerateMicros } };
            result["tile_sources"] = json::array();
            for (unsigned type = 0; type < 256; type++)
                if (gPaintTileSourceCalls[type] != 0)
                    result["tile_sources"].push_back({ { "ride_type", type }, { "us", gPaintTileSourceMicros[type] },
                                                      { "calls", gPaintTileSourceCalls[type] } });
            // Sprite statistics since the previous call, per frame. Ticks are processor time-base
            // ticks; "ticks_per_ms" (measured against the clock) converts them.
            static Drawing::SpriteStats last{};
            static ShimHost::ProfileTotals lastTotals{};
            static uint64_t lastClock = 0, lastTick = 0;
            const auto nowTotals = ShimHost::Profile();
            const uint64_t clock = ShimHost::Microseconds(), tick = ShimHost::TimeBase();
            const double frames = static_cast<double>(nowTotals.frames - lastTotals.frames);
            const double ticksPerMs = clock > lastClock ? static_cast<double>(tick - lastTick) * 1000.0 / (clock - lastClock)
                                                        : 0.0;
            static uint64_t lastEntityEntries = 0, lastMapEntries = 0;
            if (frames > 0)
            {
                result["entity_entries_per_frame"] = (gPaintDrawnEntityEntries - lastEntityEntries) / frames;
                result["fallbacks_total"] = gPaintMovingFallbacks;
                result["map_entries_per_frame"] = (gPaintDrawnMapEntries - lastMapEntries) / frames;
            }
            lastEntityEntries = gPaintDrawnEntityEntries;
            lastMapEntries = gPaintDrawnMapEntries;
            static const char* const names[] = { "rejected", "plain", "recoloured", "blended", "masked" };
            if (Drawing::gSpriteStatsOn && frames > 0)
            {
                for (int i = 0; i < Drawing::SpriteStats::kinds; i++)
                {
                    const double count = static_cast<double>(Drawing::gSpriteStats.count[i] - last.count[i]);
                    result[names[i]] = {
                        { "per_frame", count / frames },
                        { "pixels_each", count > 0 ? (Drawing::gSpriteStats.pixels[i] - last.pixels[i]) / count : 0.0 },
                        { "ms_per_frame",
                          ticksPerMs > 0 ? (Drawing::gSpriteStats.ticks[i] - last.ticks[i]) / ticksPerMs / frames : 0.0 },
                        { "us_each",
                          ticksPerMs > 0 && count > 0
                              ? (Drawing::gSpriteStats.ticks[i] - last.ticks[i]) / ticksPerMs * 1000.0 / count
                              : 0.0 },
                    };
                }
                result["frames"] = frames;
                result["ticks_per_ms"] = ticksPerMs;
            }
            result["animation_tiles"] = {gPaintAnimationTilesScanned, gPaintAnimationTilesVisited};
            result["gloom"] = getGameState().weatherCurrent.weatherGloom;
            result["paint_probes"] = json::array();
            static const char* probeNames[]{"palette", "sprite_setup_inclusive", "blit", "entity_tile_inclusive", "guest", "create_entry", "extent", "collect", "occupied_inclusive", "copy_rows", "gloom", "local_list", "owned_patch", "mover_draw", "mover_ranks", "insert_selected", "patch_tile", "patch_structural", "column_collect", "patch_call", "patch_compare", "patch_layout", "patch_snapshot", "column_arrange", "column_owners", "column_bands", "selection_clear", "ride_redraw", "patch_lookup",
#if !defined(OPENRCT2_CLASSIC_MAC) || defined(OPENRCT2_PAINT_DETAIL_PROBES)
                "owner_check", "owner_setup", "entity_ranges",
#endif
            };
            for (int i = 0; i < Drawing::PaintTiming::count; i++)
                result["paint_probes"].push_back({ { "name", probeNames[i] },
                    { "ticks", Drawing::PaintTiming::ticks[i] }, { "calls", Drawing::PaintTiming::calls[i] } });
#if !defined(OPENRCT2_CLASSIC_MAC) || defined(OPENRCT2_PAINT_DETAIL_PROBES)
            static const char* counterNames[]{ "entity_range_tests", "entity_range_projections", "selected_maps",
                "owner_list_entries", "owner_draw_entries", "owner_order_entries", "owner_rank_entries", "copied_rows", "copied_pixels",
                "guest_visits", "guest_unique", "entity_eligibility_checks" };
            for (int i = 0; i < Drawing::PaintTiming::counterCount; i++)
                result["paint_counts"][counterNames[i]] = Drawing::PaintTiming::counters[i];
#endif
            Drawing::gSpriteStatsOn = command.value("on", true);
            last = Drawing::gSpriteStats;
            lastTotals = nowTotals;
            lastClock = clock;
            lastTick = tick;
            if (command.value("compact", false))
            {
                // Keep detail probes inside the native 4096-byte reply limit.
                // Full cache inventories remain available through a separate call.
                for (const auto* field : { "raster_cache", "tile_sources", "column_cache", "animation_tiles", "idle_readback" })
                    result.erase(field);
            }
        }
        else if (op == "blitprobe")
            result["probe"] = json::parse(ShimHost::BlitProbe(command.value("engine", 0), command.value("source_mode", 0), command.value("source", 0u)));
        else if (op == "dacpalette")
            result["report"] = json::parse(ShimHost::PrepareDirectPalette(command.value("on", true), command.value("full", true)));
        else if (op == "hwscroll")
            ShimHost::SetHardwareScroll(command.value("on", true));
        else if (op == "cells")
            ShimHost::SetDirtyCells(command.at("width").get<int>(), command.at("height").get<int>());
        else if (op == "exit")
            std::exit(command.value("code", 1)); // exercise native atexit restoration without context destruction
        else if (op == "quit")
            GetContext()->Finish(); // no save prompt
        else
            throw std::invalid_argument("unknown op");
        result["ok"] = true;
        return result;
    }
} // namespace

namespace ShimHost
{
    std::string RemoteCommand(const std::string& text)
    {
        OpenRCT2::FrameTiming::Scope timing(OpenRCT2::FrameTiming::remote);
        json reply;
        try
        {
            reply = Run(json::parse(text));
        }
        catch (const std::exception& error)
        {
            reply = { { "ok", false }, { "error", error.what() } };
        }
        // Replies travel as 7-bit text of bounded size.
        std::string out = reply.dump(-1, ' ', true, json::error_handler_t::replace);
#ifdef OPENRCT2_CLASSIC_MAC
        // Keep the useful per-frame counters inside the native Apple-event reply bound.
        // Large allocator inventories remain available on a status without a pan result.
        if (out.size() > 4000 && reply.contains("memory"))
        {
            reply["memory"].erase("classes");
            reply["memory"].erase("blocks");
            out = reply.dump(-1, ' ', true, json::error_handler_t::replace);
        }
        if (out.size() > 4000 && reply.contains("pan") && reply.contains("frame_times"))
        {
            // Keep the completed interval. Its optional frame summary is also
            // available through the separate frames command.
            reply.erase("frame_times");
            reply["compact"] = true;
            out = reply.dump(-1, ' ', true, json::error_handler_t::replace);
        }
        if (out.size() > 4000 && reply.contains("pan") && reply.contains("profile"))
        {
            // Frozen pan attribution is the relevant interval; omit its live duplicate.
            reply["profile"].erase("pump");
            out = reply.dump(-1, ' ', true, json::error_handler_t::replace);
        }
        for (const auto* name : { "profile", "pan" })
        {
            if (out.size() > 4000 && reply.contains(name))
            {
                reply[name].erase("detail");
                out = reply.dump(-1, ' ', true, json::error_handler_t::replace);
            }
        }
        // Disk and autosave summaries must remain available after a long play
        // interval. A completed pan can otherwise duplicate a large profile.
        for (const auto* name : { "profile", "pan" })
        {
            if (out.size() <= 4000 || !reply.contains(name)) continue;
            json compact{{"compact", true}};
            for (const auto* field : { "frames", "seconds", "fps", "ticks_per_second", "tick_ms", "paint_ms",
                "lists_ms", "sort_ms", "draw_ms", "present_ms", "system_ms", "game_ms", "dx", "dy" })
                if (reply[name].contains(field)) compact[field] = reply[name][field];
            reply[name] = std::move(compact);
            out = reply.dump(-1, ' ', true, json::error_handler_t::replace);
        }
        if (out.size() > 4000 && reply.contains("pan"))
        {
            // Keep the completed interval and its traffic/timing budget. New
            // diagnostics can make two full detail inventories exceed the wire bound.
            reply["pan"].erase("detail");
            reply["pan"].erase("pump");
            out = reply.dump(-1, ' ', true, json::error_handler_t::replace);
        }
        if (out.size() > 4000 && reply.contains("frame_times"))
        {
            // Full worst-picture records are also available through "frames".
            reply["frame_times"].erase("worst");
            out = reply.dump(-1, ' ', true, json::error_handler_t::replace);
        }
        if (out.size() > 4000 && reply.contains("profile"))
        {
            reply["profile"].erase("pump");
            reply.erase("pan");
            reply["compact"] = true;
            out = reply.dump(-1, ' ', true, json::error_handler_t::replace);
        }
        if (out.size() > 4000)
            out = "{\"ok\":false,\"error\":\"reply too long\"}";
#endif
        return out;
    }

    void RemoteFrame()
    {
        if (gPan.framesLeft > 0)
        {
            bool count = true;
            if (gPan.aligned)
            {
                // Begin at a completed picture, after native RPC dispatch and optional
                // moving warm-up. The command's partly elapsed idle frame is excluded.
                if (gPan.startPending || gPan.warmFramesLeft > 0)
                {
                    if (gPan.startPending) gPan.startPending = false;
                    else gPan.warmFramesLeft--;
                    gPan.start = ShimHost::Profile();
                    gPan.firstTick = getGameState().currentTicks;
#ifdef OPENRCT2_PERF_METRICS
                    gPanPerfStart = PerfCapture();
#endif
                    count = false;
                }
            }
            if (count && --gPan.framesLeft == 0)
            {
                gPanResult = ProfileSince(gPan.start);
#ifdef OPENRCT2_PERF_METRICS
                gPanPerfResult = PerfDifference(gPanPerfStart);
#endif
                gPanResult["dx"] = gPan.dx;
                gPanResult["dy"] = gPan.dy;
                if (gPan.aligned)
                {
                    gPanResult["start_tick"] = gPan.firstTick;
                    gPanResult["end_tick"] = getGameState().currentTicks;
                    gPanResult["aligned"] = true;
                }
            }
            if (!gPan.aligned || gPan.framesLeft > 0)
            {
                if (auto* window = WindowGetMain())
                {
                    window->savedViewPos.x += gPan.dx;
                    window->savedViewPos.y += gPan.dy;
                }
            }
        }
        if (gSteps.empty())
            return;
        const Step step = gSteps.front();
        gSteps.pop_front();
        switch (step.kind)
        {
            case Step::Kind::move:
                PushMouse(SDL_MOUSEMOTION, step.x, step.y, step.button);
                break;
            case Step::Kind::down:
                gButtons |= SDL_BUTTON(step.button);
                PushMouse(SDL_MOUSEBUTTONDOWN, step.x, step.y, step.button);
                break;
            case Step::Kind::up:
                gButtons &= ~SDL_BUTTON(step.button);
                PushMouse(SDL_MOUSEBUTTONUP, step.x, step.y, step.button);
                break;
            case Step::Kind::key:
            case Step::Kind::keyDown:
            case Step::Kind::keyUp:
                for (const auto type : { SDL_KEYDOWN, SDL_KEYUP })
                {
                    if ((step.kind == Step::Kind::keyDown && type == SDL_KEYUP)
                        || (step.kind == Step::Kind::keyUp && type == SDL_KEYDOWN)) continue;
                    SDL_Event event{};
                    event.type = type;
                    event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
                    event.key.keysym.scancode = step.scancode;
                    event.key.keysym.sym = SDL_GetKeyFromScancode(step.scancode);
                    event.key.keysym.mod = step.mods;
                    // The shim's queue carries events; physical keyboard polling
                    // normally supplies its state separately. Held remote keys
                    // must supply both so the ordinary shortcut hold path runs.
                    ShimHost::SetKey(step.scancode, type == SDL_KEYDOWN);
                    ShimHost::PushEvent(event);
                }
                break;
            case Step::Kind::text:
            {
                SDL_Event event{};
                event.type = SDL_TEXTINPUT;
                event.text.text[0] = step.character;
                ShimHost::PushEvent(event);
                break;
            }
        }
    }
} // namespace ShimHost
