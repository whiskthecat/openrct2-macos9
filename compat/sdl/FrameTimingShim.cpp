#include "ShimHost.h"
#ifdef OPENRCT2_PAK_VFS
#include "ClassicDiskIo.h"
#endif
#include <openrct2/core/FrameTiming.h>
#include <openrct2/GameState.h>
#include <openrct2/interface/Window.h>
#include <openrct2/interface/WindowBase.h>
#include <openrct2/interface/Viewport.h>
#ifdef OPENRCT2_CLASSIC_MAC
extern "C" void ClassicAllocatorActivity(uint32_t*, uint32_t*, uint32_t*, uint32_t*);
#endif

namespace ShimHost
{
    void SampleFrameTiming(OpenRCT2::FrameTiming::Frame& frame)
    {
        static ProfileTotals last{};
        static uint64_t lastReadback = 0;
#ifdef OPENRCT2_CLASSIC_MAC
        static uint32_t lastAlloc[4]{};
#endif
        static int lastWeather = -1;
        const auto now = Profile();
        frame.paintUs = now.phase[0] - last.phase[0];
        frame.presentUs = now.phase[1] - last.phase[1];
        frame.pumpUs = now.phase[2] - last.phase[2];
        frame.paletteUs = now.paletteMicros - last.paletteMicros;
        frame.paletteSets = now.paletteSets - last.paletteSets;
        frame.columns = now.builds - last.builds;
        frame.columnBuildUs = now.columnBuildMicros - last.columnBuildMicros;
        frame.columnShiftUs = now.columnShiftMicros - last.columnShiftMicros;
        frame.basePictureUs = now.paintCosts[0] - last.paintCosts[0];
        frame.patchPictureUs = now.paintCosts[1] - last.paintCosts[1];
        frame.patches = now.patches - last.patches;
        frame.shifts = now.shifts - last.shifts;
        frame.readbackUs = gIdleReadbackUs - lastReadback;
        lastReadback = gIdleReadbackUs;
#ifdef OPENRCT2_CLASSIC_MAC
        uint32_t alloc[4];
        ClassicAllocatorActivity(&alloc[0], &alloc[1], &alloc[2], &alloc[3]);
        frame.allocationsAvailable = true;
        frame.allocations = alloc[0] - lastAlloc[0];
        frame.allocationBytes = alloc[1] - lastAlloc[1];
        frame.heapCalls = alloc[2] - lastAlloc[2];
        frame.heapUs = alloc[3] - lastAlloc[3];
        for (unsigned i = 0; i < 4; i++) lastAlloc[i] = alloc[i];
#endif
        const auto weather = static_cast<int>(OpenRCT2::getGameState().weatherCurrent.weatherType);
        frame.weather = weather;
        frame.weatherChanges = lastWeather != -1 && weather != lastWeather;
        lastWeather = weather;
        if (auto* window = OpenRCT2::WindowGetMain(); window && window->viewport)
            frame.zoom = static_cast<uint32_t>(static_cast<int8_t>(window->viewport->zoom));
#ifdef OPENRCT2_PAK_VFS
        static ClassicDiskIo::FrameCounters diskLast{};
        const auto disk = ClassicDiskIo::Counters();
        if (disk.epoch != diskLast.epoch) diskLast = {};
        frame.diskAvailable = true;
        frame.reads = disk.reads - diskLast.reads;
        frame.readBytes = disk.readBytes - diskLast.readBytes;
        frame.readUs = disk.readUs - diskLast.readUs;
        frame.writes = disk.writes - diskLast.writes;
        frame.writeBytes = disk.writeBytes - diskLast.writeBytes;
        frame.writeUs = disk.writeUs - diskLast.writeUs;
        diskLast = disk;
        ClassicDiskIo::Frame(OpenRCT2::getGameState().currentTicks);
#endif
        last = now;
    }
}
