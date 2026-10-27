// Opt-in presentation capture. No RPC or per-picture disk writes during a path.
#include "FrameBench.h"
#include <openrct2/paint/Paint.Timing.h>
#include "ShimHost.h"
#include <openrct2/core/FrameTiming.h>
#include <openrct2/entity/EntityTweener.h>
#include <openrct2/Context.h>
#include <openrct2/GameState.h>
#include <openrct2/interface/Window.h>
#include <openrct2/interface/WindowBase.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2-ui/interface/ViewportInteraction.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <array>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <stdexcept>
#ifdef OPENRCT2_PAK_VFS
#include "ClassicAudio.h"
#endif
#ifdef OPENRCT2_CLASSIC_MAC
#include "ClassicVfs.h"
#include <OSUtils.h>
#endif
using namespace OpenRCT2;
namespace ShimHost
{
    namespace
    {
        using Row = std::array<uint64_t, 109>;
        const char* columns[] = {"at_us", "interval_us", "tick", "ticks", "tick_calls", "sim_us", "wait_us", "draw_us",
            "messages_us", "paint_us", "present_us", "pump_us", "columns", "reads", "read_bytes", "read_us",
            "writes", "write_bytes", "write_us", "autosaves", "yield_calls", "yield_us", "yield_max_us", "coarse_yields",
            "heap_calls", "heap_us", "calendar_us", "remote_commands", "log_us", "base_picture_us", "baseline_us", "weather", "frame", "native_tick", "vsync_wait_us", "vsync_mirror_us", "vsync_palette_us", "vsync_line", "auto_us", "input_us", "windows_us", "sounds_us", "palette_us", "hover_query_us", "hover_nearest_us", "hover_queries", "hover_nearest_calls", "hover_visits", "hover_tested", "hover_tiles", "hover_sessions", "hover_cache_hits", "hover_cache_misses", "hover_cache_checks", "hover_cache_mismatches", "hover_nearest_hits", "vsync_flush_us", "vsync_publish_us", "vsync_mirror_pixels", "vsync_mirror_rects", "vsync_mirror_full", "vsync_mirror_partial", "vsync_late", "vsync_queued", "vsync_submitted", "vsync_ready_line", "vsync_phase_us", "vsync_refresh_us", "median_us", "hitch", "build_us", "patch_us", "shift_us", "patches", "shifts", "events", "days", "weeks", "months", "opened", "window_class", "weather_changes", "news", "lightning", "allocations", "allocation_bytes", "view_x", "view_y", "pan_active", "month", "month_tick", "weather_gloom", "calendar_week_us", "calendar_award_us", "score_scan_us", "scenario_end_us", "audio_voices", "audio_ima", "audio_pcm", "audio_mono", "audio_stereo", "audio_22050", "audio_44100", "audio_started", "audio_refused", "audio_callbacks", "audio_prepare_reads", "audio_demand_reads", "audio_pump_us"};
        static_assert(std::size(columns)==std::tuple_size_v<Row>);
        Ui::HoverPickingStats previousHover{};
#ifdef OPENRCT2_PAK_VFS
        Audio::ClassicAudioFrame previousAudio{};
#endif
#ifdef OPENRCT2_CLASSIC_MAC
        VSyncTiming previousVSync{};
#endif
        std::vector<Row> rows;
        InterpolationStats tweenStart{}, tweenEnd{};
        bool tweenCapture = false, detailCapture = false;
        ProfileTotals detailStart{}, detailEnd{};
        uint64_t detailFirstUs = 0, detailLastUs = 0, detailFirstTick = 0, detailLastTick = 0;
        std::array<uint64_t, Drawing::PaintTiming::count> paintFirst{}, paintLast{}, callsFirst{}, callsLast{};
#if !defined(OPENRCT2_CLASSIC_MAC) || defined(OPENRCT2_PAINT_DETAIL_PROBES)
        std::array<uint64_t, Drawing::PaintTiming::counterCount> countsFirst{}, countsLast{};
#endif
        auto TweenCounts(const InterpolationStats& s) {
            return nlohmann::json{{"calls",s.calls},{"micros",s.micros},{"scanned",s.scanned},
                {"selected",s.selected},{"restore_moves",s.restoreMoves},{"tween_moves",s.tweenMoves},
                {"invalidations",s.invalidations},{"damage_pixels",s.damagePixels}};
        }
        bool active = false, done = false, overflow = false;
        uint64_t start = 0, warm = 0, duration = 0, period = 0, panStart = 0;
        int originX = 0, originY = 0, radius = 96;
        std::array<int,2> direction{1,0};
        bool keepEnd = false;
        std::string path = "still", output;
        bool exitAfter = false;
        void Complete()
        {
            active = false; done = true; FrameTiming::observe = nullptr;
            if (!keepEnd) if (auto* w = WindowGetMain()) w->savedViewPos = {originX, originY};
            if (!output.empty())
            {
                const auto filename =
#ifdef OPENRCT2_CLASSIC_MAC
                    ClassicVfs::HostPath(output);
#else
                    output;
#endif
                if (auto* f = std::fopen(filename.c_str(), "wb"))
                {
                    for (unsigned i = 0; i < std::size(columns); i++) std::fprintf(f, "%s%s", i ? "," : "", columns[i]);
                    std::fputc('\n', f);
                    for (const auto& row : rows)
                    {
                        // Classic's C library does not implement %llu. JSON's
                        // integer formatter already handles these exact rows.
                        const auto line = nlohmann::json(row).dump();
                        std::fwrite(line.data() + 1, 1, line.size() - 2, f);
                        std::fputc('\n', f);
                    }
                    std::fclose(f);
                }
                else std::fprintf(stderr, "framepath: cannot write %s\n", output.c_str());
            }
#ifndef OPENRCT2_CLASSIC_MAC
            if (exitAfter) GetContext()->Finish();
#endif
        }
        void Picture(const FrameTiming::Frame& f)
        {
            if (!active || !f.presentedAtUs) return;
            if (!start) start = f.presentedAtUs;
            const auto elapsed = f.presentedAtUs - start;
#ifdef OPENRCT2_CLASSIC_MAC
            const auto vsync = VSyncCosts();
            const auto previous = previousVSync; previousVSync = vsync;
#endif
            if (elapsed >= warm && elapsed < warm + duration)
            {
                if (rows.size() == rows.capacity()) { overflow = true; Complete(); return; }
                auto* main = WindowGetMain();
#ifdef OPENRCT2_PAK_VFS
                const auto audio = Audio::ClassicAudioGetFrame();
#endif
                rows.push_back({elapsed - warm, f.us, f.tick, f.ticks, f.tickCalls,
                    f.stage[FrameTiming::simulation], f.stage[FrameTiming::wait], f.stage[FrameTiming::draw],
                    f.stage[FrameTiming::messages], f.paintUs, f.presentUs, f.pumpUs, f.columns,
                    f.reads, f.readBytes, f.readUs, f.writes, f.writeBytes, f.writeUs, f.autosaves,
                    f.yieldCalls, f.yieldUs, f.yieldMaxUs, f.coarseYields, f.heapCalls, f.heapUs,
                    f.stage[FrameTiming::calendar], f.commands, f.stage[FrameTiming::log], f.basePictureUs, f.baselineUs, f.weather,
                    f.number,
#ifdef OPENRCT2_CLASSIC_MAC
                    TickCount(), vsync.waitUs - previous.waitUs, vsync.mirrorUs - previous.mirrorUs,
                    vsync.paletteUs - previous.paletteUs, vsync.frame == f.number ? vsync.line : 0
#else
                    0, 0, 0, 0, 0
#endif
                    , f.stage[FrameTiming::autosave], f.stage[FrameTiming::input], f.stage[FrameTiming::windows],
                    f.stage[FrameTiming::sounds], f.paletteUs,
                    Ui::gHoverPickingStats.queryMicros-previousHover.queryMicros,
                    Ui::gHoverPickingStats.closestMicros-previousHover.closestMicros,
                    Ui::gHoverPickingStats.queryCalls-previousHover.queryCalls,
                    Ui::gHoverPickingStats.closestCalls-previousHover.closestCalls,
                    Ui::gHoverPickingStats.visited-previousHover.visited,
                    Ui::gHoverPickingStats.tested-previousHover.tested,
                    Ui::gHoverPickingStats.tiles-previousHover.tiles,
                    Ui::gHoverPickingStats.paintSessions-previousHover.paintSessions,
                    Ui::gHoverPickingStats.cacheHits-previousHover.cacheHits,
                    Ui::gHoverPickingStats.cacheMisses-previousHover.cacheMisses,
                    Ui::gHoverPickingStats.cacheChecks-previousHover.cacheChecks,
                    Ui::gHoverPickingStats.cacheMismatches-previousHover.cacheMismatches,
                    Ui::gHoverPickingStats.nearestHits-previousHover.nearestHits,
#ifdef OPENRCT2_CLASSIC_MAC
                    vsync.flushUs-previous.flushUs,vsync.publishUs-previous.publishUs,
                    vsync.mirrorPixels-previous.mirrorPixels,vsync.mirrorRects-previous.mirrorRects,
                    vsync.mirrorFull-previous.mirrorFull,vsync.mirrorPartial-previous.mirrorPartial,
                    vsync.late-previous.late,vsync.queued-previous.queued,vsync.submitted-previous.submitted,
                    vsync.readyLine,vsync.phaseUs,vsync.refreshUs
#else
                    0,0,0,0,0,0,0,0,0,0,0,0
#endif
                    ,f.medianUs,f.hitch,f.columnBuildUs,f.patchPictureUs,f.columnShiftUs,
                    f.patches,f.shifts,f.eventMask,f.days,f.weeks,f.months,f.opened,f.windowClass,
                    f.weatherChanges,f.news,f.lightning,f.allocations,f.allocationBytes,
                    main ? static_cast<uint64_t>(main->savedViewPos.x) : 0,
                    main ? static_cast<uint64_t>(main->savedViewPos.y) : 0,
                    path == "mixed" ? elapsed >= warm + panStart : path != "still",f.month,f.monthTick,getGameState().weatherCurrent.weatherGloom,
                    f.calendarWeekUs,f.calendarAwardUs,f.scoreScanUs,f.scenarioEndUs,
#ifdef OPENRCT2_PAK_VFS
                    audio.voices,audio.ima,audio.pcm,audio.mono,audio.stereo,audio.rate22050,audio.rate44100,
                    audio.started-previousAudio.started,audio.refused-previousAudio.refused,
                    audio.callbacks-previousAudio.callbacks,audio.prepareReads-previousAudio.prepareReads,
                    audio.demandReads-previousAudio.demandReads,
#else
                    0,0,0,0,0,0,0,0,0,0,0,0,
#endif
                    f.audioPumpUs
                });
                previousHover = Ui::gHoverPickingStats;
#ifdef OPENRCT2_PAK_VFS
                previousAudio = audio;
#endif
            }
            // Probe bounds are exactly the CSV rows after the first boundary row.
            // No probe clocks/counters are sampled in the primary capture.
            if (tweenCapture && elapsed >= warm && elapsed < warm + duration)
            {
                if (rows.size() == 1) tweenStart = gInterpolationStats;
                tweenEnd = gInterpolationStats;
            }
            if (detailCapture && elapsed >= warm && elapsed < warm + duration)
            {
                detailEnd = Profile(); detailLastUs = f.presentedAtUs; detailLastTick = TimeBase();
                std::copy_n(Drawing::PaintTiming::ticks, paintLast.size(), paintLast.begin());
                std::copy_n(Drawing::PaintTiming::calls, callsLast.size(), callsLast.begin());
#if !defined(OPENRCT2_CLASSIC_MAC) || defined(OPENRCT2_PAINT_DETAIL_PROBES)
                std::copy_n(Drawing::PaintTiming::counters, countsLast.size(), countsLast.begin());
#endif
                if (rows.size() == 1)
                {
                    detailStart = detailEnd; detailFirstUs = detailLastUs; detailFirstTick = detailLastTick;
                    paintFirst = paintLast; callsFirst = callsLast;
#if !defined(OPENRCT2_CLASSIC_MAC) || defined(OPENRCT2_PAINT_DETAIL_PROBES)
                    countsFirst = countsLast;
#endif
                }
            }
            if (elapsed >= warm + duration) { Complete(); return; }
            if (auto* w = WindowGetMain())
            {
                const bool panning = path != "mixed" || elapsed >= warm + panStart;
                const double phase = static_cast<double>(path == "mixed" ? (panning ? elapsed - warm - panStart : 0) : elapsed) / period;
                const double angle = phase * 6.283185307179586;
                int x = 0, y = 0;
                if (path == "circle" || (path == "mixed" && panning)) { x = std::lround(radius * (std::cos(angle) - 1)); y = std::lround(radius * std::sin(angle)); }
                if (path == "straight")
                {
                    const int distance=std::lround(radius * (1 - std::abs(2 * (phase - std::floor(phase)) - 1)));
                    x=direction[0]*distance; y=direction[1]*distance;
                }
                w->savedViewPos = {originX + x, originY + y};
            }
        }
    }
    std::string FrameBenchCommand(const std::string& text)
    {
        using nlohmann::json;
        const auto c = json::parse(text);
        if (c.value("stop", false) && active) Complete();
        if (c.value("start", false))
        {
            auto* w = WindowGetMain();
            if (!w || !w->viewport) throw std::runtime_error("no main viewport");
            if (active) throw std::runtime_error("capture already active");
            output = c.value("output", "");
#ifdef OPENRCT2_CLASSIC_MAC
            // Native diagnostic captures may write only their private test area.
            if (!output.empty() && (output.rfind("/RCT/user/Perf/", 0) != 0 || output.find("..") != std::string::npos))
                throw std::invalid_argument("native capture output must be under /RCT/user/Perf/");
#else
            exitAfter = c.value("exit", false);
#endif
            path = c.value("path", "still");
            if (path != "circle" && path != "straight" && path != "still" && path != "mixed") throw std::invalid_argument("path: circle, straight, still, mixed");
            direction=c.value("direction",std::array<int,2>{1,0});
            if (std::abs(direction[0])>1 || std::abs(direction[1])>1 || (direction[0]==0 && direction[1]==0))
                throw std::invalid_argument("direction: nonzero pair of -1, 0 or 1");
            keepEnd=c.value("keep_end",false);
            duration = static_cast<uint64_t>(std::clamp(c.value("seconds", 8.0), 0.5, 1800.0) * 1000000);
            period = static_cast<uint64_t>(std::clamp(c.value("period_seconds", duration / 1000000.0), 0.5, 60.0) * 1000000);
            warm = static_cast<uint64_t>(std::clamp(c.value("warmup", 2.0), 0.0, 30.0) * 1000000);
            panStart = static_cast<uint64_t>(std::clamp(c.value("pan_start_seconds", duration / 2000000.0), 0.0, duration / 1000000.0) * 1000000);
            radius = std::clamp(c.value("radius", 96), 0, 512);
            originX = w->savedViewPos.x; originY = w->savedViewPos.y;
            tweenCapture = gInterpolationProbe; tweenStart = {}; tweenEnd = {};
            detailCapture = c.value("detail", false); detailStart = {}; detailEnd = {};
            detailFirstUs = detailLastUs = detailFirstTick = detailLastTick = 0;
            previousHover = Ui::gHoverPickingStats;
#ifdef OPENRCT2_PAK_VFS
            previousAudio = Audio::ClassicAudioGetFrame();
#endif
            rows.clear(); rows.reserve(std::clamp(c.value("capacity", 8192u), 128u, 131072u));
#ifdef OPENRCT2_CLASSIC_MAC
            previousVSync = VSyncCosts();
#endif
            start = 0; active = true; done = overflow = false;
            FrameTiming::observe = Picture;
        }
        json reply = {{"active", active}, {"done", done}, {"overflow", overflow}, {"count", rows.size()},
            {"direction",direction},{"keep_end",keepEnd},
            {"path", path}, {"pan_start_seconds",panStart/1000000.0}, {"seconds", duration / 1000000.0}, {"period_seconds", period / 1000000.0}, {"warmup", warm / 1000000.0}, {"columns", columns}};
        if (tweenCapture) { reply["tween_start"] = TweenCounts(tweenStart); reply["tween_end"] = TweenCounts(tweenEnd); }
        if (!active && detailCapture && c.value("detail", false) && rows.size() > 1)
        {
            auto& d = reply["detail"];
            d["frames"] = rows.size() - 1;
            d["elapsed_us"] = detailLastUs - detailFirstUs;
            d["timebase_ticks"] = detailLastTick - detailFirstTick;
            for (unsigned i = 0; i < paintFirst.size(); ++i)
            {
                d["paint_ticks"].push_back(paintLast[i] - paintFirst[i]);
                d["paint_calls"].push_back(callsLast[i] - callsFirst[i]);
            }
#if !defined(OPENRCT2_CLASSIC_MAC) || defined(OPENRCT2_PAINT_DETAIL_PROBES)
            for (unsigned i = 0; i < countsFirst.size(); ++i)
                d["paint_counts"].push_back(countsLast[i] - countsFirst[i]);
#endif
            for (unsigned i = 0; i < 4; ++i)
                d["world_us"].push_back(detailEnd.world[i] - detailStart.world[i]);
            for (unsigned i = 0; i < 8; ++i)
                d["paint_cost_us"].push_back(detailEnd.paintCosts[i] - detailStart.paintCosts[i]);
            d["widgets_us"] = detailEnd.widgetMicros - detailStart.widgetMicros;
            d["damage_us"] = detailEnd.damageMicros - detailStart.damageMicros;
            d["guest_pose_us"] = detailEnd.guestPoseMicros - detailStart.guestPoseMicros;
            d["entity_flush_us"] = detailEnd.entityFlushMicros - detailStart.entityFlushMicros;
            d["audio_us"] = detailEnd.audioPumpMicros - detailStart.audioPumpMicros;
        }
        if (!active && !c.value("compact", false))
        {
            const size_t first = std::min(c.value("first", size_t{0}), rows.size());
            const size_t end = std::min(rows.size(), first + std::clamp(c.value("limit", 6u), 1u, 6u));
            reply["first"] = first; reply["rows"] = json::array();
            for (size_t i = first; i < end; i++) reply["rows"].push_back(rows[i]);
        }
        if (c.value("compact", false)) reply.erase("columns");
        if (c.value("release", false) && !active) std::vector<Row>().swap(rows);
        return reply.dump();
    }
}
