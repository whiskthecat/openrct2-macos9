// Native Rage 128 page flipping. Render/scroll into a hidden video
// page, latch the completed picture at vertical blank, and mirror it on-card
// into the next hidden page for incremental drawing. Never enter MMIO under
// Classic or without the native PCI identity and BAR mirror checks.
// ATI RRG-G04500-C: CRTC_OFFSET/CRTC_OFFSET_CNTL; GUI_TRIG_OFFSET clears when
// the new address becomes visible (at the end of blank, not its beginning).
#pragma once
#include <CodeFragments.h>
#include <DriverServices.h>
#include "ClassicInterruptMask.h"
namespace
{
    template<typename T> OSErr VideoStatus(short selector, T& value)
    {
        CntrlParam block{}; block.ioCRefNum = (**gDevice).gdRefNum; block.csCode = selector;
        *reinterpret_cast<Ptr*>(block.csParam) = reinterpret_cast<Ptr>(&value);
        return PBStatusSync(reinterpret_cast<ParmBlkPtr>(&block));
    }
}
namespace {
namespace NativeVSync
{
    struct DriverServices
    {
        CFragConnectionID connection = 0;
        OSStatus (*secondary)(SecondaryInterruptHandler2, ExceptionHandler, void*, void*) = nullptr;
        ExecutionLevel (*level)() = nullptr;
        DriverServices()
        {
            Ptr mainAddress = nullptr; Str255 message{};
            if (GetSharedLibrary(reinterpret_cast<const unsigned char*>("\021DriverServicesLib"), kPowerPCCFragArch,
                kLoadCFrag, &connection, &mainAddress, message) != noErr) return;
            Ptr address = nullptr; CFragSymbolClass kind = 0;
            if (FindSymbol(connection, reinterpret_cast<const unsigned char*>("\036CallSecondaryInterruptHandler2"), &address, &kind) == noErr
                && kind == kTVectorCFragSymbol) secondary = reinterpret_cast<decltype(secondary)>(address);
            address = nullptr;
            if (FindSymbol(connection, reinterpret_cast<const unsigned char*>("\025CurrentExecutionLevel"), &address, &kind) == noErr
                && kind == kTVectorCFragSymbol) level = reinterpret_cast<decltype(level)>(address);
        }
        ~DriverServices() { if (connection) CloseConnection(&connection); }
    };
    DriverServices& Services() { static DriverServices services; return services; }
    bool secondaryBlank = false;
    constexpr uint32_t offsetRegister = 0x0224, controlRegister = 0x0228;
    Rage128 card;
    bool pendingPalette = false, flushingPalette = false;
    bool requested = false, active = false, failed = false, availabilityKnown = false, hardwareReady = false;
    GWorldPtr world = nullptr, probeWorld = nullptr;
    uint8_t* pages[3]{};
    uint32_t offsets[3]{}, originalControl = 0;
    unsigned back = 1;
    uint32_t firstLineMin = UINT32_MAX, firstLineMax = 0, lastLine = 0;
    uint64_t lastFrame = 0;
    uint64_t earlyLines = 0, blankLines = 0, middleLines = 0, paletteFlushes = 0, paletteUs = 0;
    uint64_t flips = 0, waitUs = 0, mirrorUs = 0, maximumWait = 0, errors = 0, pendingFlips = 0, blankCompletions = 0, outsideBlank = 0;
    uint64_t scrollCopies = 0, scrollUs = 0;
    bool pipelineRequested = true, pipelineActive = false, pipelineFailed = false;
    // Timing misses are recoverable; hardware/ownership errors still require
    // a mode change or explicit toggle. Keep a broken callback on ordinary
    // sync between bounded probes, rather than repeatedly stalling each frame.
    struct RetrySchedule
    {
        static constexpr uint32_t initialDelay = 2000000, maximumDelay = 30000000;
        uint64_t due = 0;
        uint32_t delay = initialDelay;
        void Arm(uint64_t now) { due = now+delay; delay = std::min(delay*2,maximumDelay); }
        bool Ready(uint64_t now, bool loading) const { return due && now>=due && !loading; }
        void Reset() { due = 0; delay = initialDelay; }
    } pipelineRetry;
    bool timingFault = false;
    uint64_t pipelineRetries = 0, pipelineHealthySince = 0;
    bool damageMirror = true; // qualified hardware cursor; broad damage retains full mirror
    PageMirror::Damage pageDamage;
    uint64_t mirrorPixels=0, mirrorRects=0, mirrorFull=0, mirrorPartial=0, flushUs=0, publishUs=0;
    uint32_t readyLine=0, phaseUs=0, refreshUs=0;
    unsigned front = 0;
    double ticksPerUs = 0;
    uint64_t queuedFlips = 0, drainWaits = 0;
    uint64_t fallbacks = 0;
    uint32_t lateWindowJobs = 0, lateWindowCount = 0;
    std::string reason;
    uint32_t inputTick = 0;
    struct LatencySample { uint32_t inputTicks, readyTicks; bool pipeline; };
    LatencySample latency[128]{};
    unsigned latencyCount = 0;

    // All interrupt state comes through the task pointer. Code/data must be
    // resident; the callback never calls Toolbox or allocates. A ready job is
    // immutable until the main task acknowledges its completed display latch.
    struct BlankProbe
    {
        VBLTask task{};
        Rage128* device = nullptr;
        volatile uint32_t calls = 0, blank = 0, minLine = UINT32_MAX, maxLine = 0;
        volatile uint32_t lastTick = 0, minInterval = UINT32_MAX, maxInterval = 0;
        VBLUPP upp = nullptr;
        short slot = 0;
        bool installed = false, changedInterrupt = false;
        SInt8 originalInterrupt = 1;
        OSErr installError = noErr, removeError = noErr;
        volatile uint32_t jobState = 0; // 0 empty, 1 ready, 2 submitted, 3 handler error
        volatile bool suppressSubmission = false; // opt-in timeout recovery diagnostic
        volatile bool forceLate = false, forceOverrun = false; // opt-in safe fault injection
        OSStatus (*secondary)(SecondaryInterruptHandler2, ExceptionHandler, void*, void*) = nullptr;
        ExecutionLevel (*level)() = nullptr;
        volatile uint32_t callbackLevel = 0, commitLevel = 0, secondaryErrors = 0;
        volatile uint32_t overrunBegin = 0, overrunEnd = 0, overrunCheck = 0;
        volatile uint32_t overrunStartLine = 0, overrunEndLine = 0, overrunFinalLine = 0;
        struct Rejected { uint32_t tick, line, status, offset, gap, reason; };
        Rejected rejected[64]{};
        volatile uint32_t rejectedCount = 0, rejectedLastTick = 0;
        volatile uint32_t lateStreak = 0;
        uint32_t jobOffset = 0, totalLines = 0;
        unsigned jobPage = 0;
        int first = 256, last = -1;
        uint32_t packed[256]{};
        Colour colours[256]{};
        volatile uint32_t jobs = 0, late = 0, overran = 0, jobTicks = 0, paletteTicks = 0;
        uint32_t lastPaletteTicks = 0;
        uint32_t inputTick = 0, readyTick = 0;
        volatile uint32_t latchTick = 0, refreshTicks = 0;
        volatile uint32_t submitMin = UINT32_MAX, submitMax = 0, finishMax = 0, maximumJobTicks = 0;
        volatile bool cursorWatch = false, cursorExpected = false;
        volatile uint32_t cursorSamples = 0, cursorOff = 0, cursorOffscreen = 0;
        uint32_t cursorWidth = 0, cursorHeight = 0;
        volatile bool cursorShapeChanging = false, cursorScreenCopy = false;
        volatile uint32_t cursorShapeOff = 0, cursorCopyOff = 0;
    } blankProbe;

    inline uint32_t TimeBaseLow()
    {
        uint32_t tick; __asm__ volatile("mftb %0" : "=r"(tick)); return tick;
    }

    OSStatus CommitBlank(void* raw, void*)
    {
        auto& p = *static_cast<BlankProbe*>(raw);
        if (p.level) p.commitLevel = p.level();
        ClassicInterruptMask::Guard guard;
        // Sample after masking: even Sound Manager must not interrupt the bounded
        // DAC/offset commit after the remaining-blank check. No Toolbox here.
        // Sample again at the execution level that will perform the writes.
        const auto line = (p.device->Read(0x0210)>>16)&0x7ffu;
        const auto status = p.device->Read(0x005c), offset = p.device->Read(offsetRegister);
        const bool inBlank = (status&1) != 0;
        if (p.forceLate || !inBlank || line+20 >= p.totalLines || (offset&0x40000000u))
        {
            const auto tick = TimeBaseLow();
            p.rejected[p.rejectedCount%64] = {tick,line,status,offset,tick-p.rejectedLastTick,
                uint32_t(!inBlank)|(uint32_t(line+20>=p.totalLines)<<1)|(uint32_t((offset&0x40000000u)!=0)<<2)|(uint32_t(p.forceLate)<<3)};
            p.rejectedCount = p.rejectedCount+1; p.rejectedLastTick = tick;
            p.late = p.late+1; p.lateStreak = p.lateStreak+1;
            // This callback made no writes. Keep the immutable job ready for
            // the next safe blank, even across loading/driver transients. The
            // main task's 50 ms Drain budget detects actual lack of progress.
            return noErr;
        }
        const auto began = TimeBaseLow();
        if (p.last >= p.first)
        {
            *reinterpret_cast<volatile uint8_t*>(p.device->base+0x00b0) = uint8_t(p.first);
            __asm__ volatile("eieio");
            for (int i=p.first;i<=p.last;++i) p.device->Write(0x00b4,p.packed[i]);
        }
        const auto paletteEnd = TimeBaseLow();
        p.device->Write(offsetRegister,p.jobOffset);
        const auto finished = TimeBaseLow();
        const auto endLine = (p.device->Read(0x0210)>>16)&0x7ffu;
        p.latchTick = finished + (p.totalLines-endLine)*p.refreshTicks/p.totalLines;
        if (p.forceOverrun || !(p.device->Read(0x005c)&1))
        {
            p.overran = p.overran+1;
            p.overrunBegin = began; p.overrunEnd = finished; p.overrunCheck = TimeBaseLow();
            p.overrunStartLine = line; p.overrunEndLine = endLine;
            p.overrunFinalLine = (p.device->Read(0x0210)>>16)&0x7ffu;
        }
        p.lateStreak = 0;
        if (line < p.submitMin) p.submitMin = line;
        if (line > p.submitMax) p.submitMax = line;
        if (endLine > p.finishMax) p.finishMax = endLine;
        const auto elapsed = finished-began;
        p.jobTicks = p.jobTicks+elapsed;
        p.paletteTicks = p.paletteTicks+(paletteEnd-began);
        p.lastPaletteTicks = paletteEnd-began;
        if (elapsed > p.maximumJobTicks) p.maximumJobTicks = elapsed;
        p.jobs = p.jobs+1;
        __asm__ volatile("sync" ::: "memory");
        p.jobState = 2;
        return noErr;
    }
    // Task-time diagnostic for the exact production commit/early-return paths.
    // Its register aperture is private RAM: no card discovery, display writes,
    // global flip state or Sound Manager calls. This also runs in the private VM.
    std::string CommitPathsProbe()
    {
        using nlohmann::json;
        if (!ClassicInterruptMask::Prepare()) throw std::runtime_error("resident mask helper unavailable");
        alignas(4) uint8_t registers[0x300]{};
        Rage128 device; device.base = registers;
        auto storage = std::make_unique<BlankProbe>();
        auto& p = *storage;
        json rows = json::array();
        auto reset = [&](uint32_t line, uint32_t status = 1, uint32_t offset = 0) {
            p = BlankProbe{};
            p.device = &device; p.totalLines = 514; p.jobOffset = 0x123400;
            p.jobState = 1; p.first = 0; p.last = 255;
            for (unsigned i = 0; i < 256; ++i) p.packed[i] = i * 0x010101;
            device.Write(0x0210, line << 16); device.Write(0x005c, status);
            device.Write(offsetRegister, offset); device.Write(0x00b4, 0xabcdef);
        };
        auto commit = [&] {
            const auto before = ClassicInterruptMask::Get();
            const auto error = CommitBlank(&p, nullptr);
            if (error != noErr || ClassicInterruptMask::Get() != before)
                throw std::runtime_error("commit path did not restore interrupt mask");
        };
        auto record = [&](const char* name, bool expected) {
            if (!expected) throw std::runtime_error(std::string("commit path failed: ") + name);
            rows.push_back({{"case",name},{"state",uint32_t(p.jobState)},
                {"jobs",uint32_t(p.jobs)},{"late",uint32_t(p.late)},
                {"streak",uint32_t(p.lateStreak)},{"overruns",uint32_t(p.overran)},
                {"mask_restored",true}});
        };
        for (auto line : {480u, 493u})
        {
            reset(line); commit();
            record(line == 480 ? "blank-start" : "last-safe-line", p.jobState == 2 && p.jobs == 1
                && p.overran == 0 && device.Read(offsetRegister) == p.jobOffset
                && device.Read(0x00b4) == p.packed[255]);
        }
        reset(0, 0); commit();
        record("visible-no-write", p.jobState == 1 && p.late == 1 && device.Read(offsetRegister) == 0
            && device.Read(0x00b4) == 0xabcdef);
        reset(494); commit();
        record("reserve-no-write", p.jobState == 1 && p.late == 1 && device.Read(offsetRegister) == 0
            && device.Read(0x00b4) == 0xabcdef);
        reset(482, 1, 0x40000000); commit();
        record("pending-no-write", p.jobState == 1 && p.late == 1 && device.Read(offsetRegister) == 0x40000000
            && device.Read(0x00b4) == 0xabcdef);
        reset(482); p.forceLate = true;
        for (unsigned i = 0; i < 3; ++i) commit();
        record("three-misses-retry", p.jobState == 1 && p.lateStreak == 3 && p.jobs == 0
            && device.Read(offsetRegister) == 0 && device.Read(0x00b4) == 0xabcdef);
        for (unsigned i = 3; i < 64; ++i) commit();
        record("sustained-misses-no-write", p.jobState == 1 && p.lateStreak == 64 && p.jobs == 0
            && device.Read(offsetRegister) == 0 && device.Read(0x00b4) == 0xabcdef);
        p.forceLate = false; commit();
        record("transient-misses-resume", p.jobState == 2 && p.lateStreak == 0 && p.jobs == 1);
        reset(482); p.forceLate = true; commit(); p.forceLate = false; commit();
        record("isolated-miss-retry", p.jobState == 2 && p.late == 1 && p.lateStreak == 0 && p.jobs == 1);
        reset(482); p.forceOverrun = true; commit();
        record("overrun-recorded", p.jobState == 2 && p.overran == 1);
        RetrySchedule retry;
        uint64_t now = 1000;
        json delays = json::array();
        for (auto delay : {2000000u,4000000u,8000000u,16000000u,30000000u,30000000u})
        {
            retry.Arm(now);
            if (retry.due != now+delay || retry.Ready(retry.due-1,false)
                || retry.Ready(retry.due,true) || !retry.Ready(retry.due,false))
                throw std::runtime_error("pipeline retry deadline/backoff/loading guard failed");
            delays.push_back(delay); now = retry.due;
        }
        retry.Reset();
        if (retry.due || retry.delay != RetrySchedule::initialDelay || retry.Ready(now,false))
            throw std::runtime_error("pipeline retry reset failed");
        return json{{"private_ram",true},{"hardware_timing",false},{"paths",rows},
            {"retry_delays_us",delays},{"retry_loading_guard",true},{"retry_reset",true}}.dump();
    }
    pascal void BlankCallback(VBLTaskPtr task)
    {
        auto& p = *reinterpret_cast<BlankProbe*>(task);
        if (p.level) p.callbackLevel = p.level();
        const auto tick = TimeBaseLow();
        const auto line = (p.device->Read(0x0210)>>16)&0x7ffu;
        if (p.cursorWatch && p.cursorExpected)
        {
            p.cursorSamples = p.cursorSamples+1;
            if (!(p.device->Read(0x0050)&(1u<<16))) p.cursorOff = p.cursorOff+1;
            const auto position = p.device->Read(0x0264);
            if ((position >> 16) >= p.cursorWidth || (position & 0xffffu) >= p.cursorHeight)
                {
                p.cursorOffscreen = p.cursorOffscreen+1;
                if (p.cursorShapeChanging) p.cursorShapeOff = p.cursorShapeOff+1;
                else if (p.cursorScreenCopy) p.cursorCopyOff = p.cursorCopyOff+1;
            }
        }
        if (p.calls)
        {
            const uint32_t delta = tick-p.lastTick;
            p.refreshTicks = delta;
            if (delta < p.minInterval) p.minInterval = delta;
            if (delta > p.maxInterval) p.maxInterval = delta;
        }
        p.lastTick = tick;
        p.calls = p.calls+1;
        const bool inBlank = (p.device->Read(0x005c)&1) != 0;
        if (inBlank) p.blank = p.blank+1;
        if (line < p.minLine) p.minLine = line;
        if (line > p.maxLine) p.maxLine = line;
        if (p.jobState == 1 && !p.suppressSubmission)
        {
            __asm__ volatile("sync" ::: "memory");
            if (secondaryBlank && p.secondary)
            {
                const auto error = p.secondary(CommitBlank, nullptr, &p, nullptr);
                if (error != noErr) { p.secondaryErrors = p.secondaryErrors+1; p.jobState = 3; }
            }
            else CommitBlank(&p, nullptr);
        }
        task->vblCount = 1;
    }
    void CursorWatch(bool watch,bool expected,bool reset)
    {
        auto& p = blankProbe;
        if (!watch && !p.cursorWatch && !reset) return;
        p.cursorWatch = false;
        __asm__ volatile("sync" ::: "memory");
        if (reset) { p.cursorSamples = 0; p.cursorOff = 0; p.cursorOffscreen = 0; p.cursorShapeOff = 0; p.cursorCopyOff = 0; }
        p.cursorExpected = expected; p.cursorWidth = gWidth; p.cursorHeight = gHeight;
        __asm__ volatile("sync" ::: "memory");
        p.cursorWatch = watch;
    }
    void CursorOperation(bool shape, bool copying)
    {
        blankProbe.cursorShapeChanging = shape; blankProbe.cursorScreenCopy = copying;
        __asm__ volatile("sync" ::: "memory");
    }
    std::array<uint32_t,5> CursorWatchStats()
    {
        return {uint32_t(blankProbe.cursorSamples),uint32_t(blankProbe.cursorOff),uint32_t(blankProbe.cursorOffscreen),
            uint32_t(blankProbe.cursorShapeOff),uint32_t(blankProbe.cursorCopyOff)};
    }
    OSErr InterruptControl(SInt8 mode)
    {
        VDFlagRecord value{}; value.csMode = mode;
        CntrlParam block{}; block.ioCRefNum = (**gDevice).gdRefNum; block.csCode = cscSetInterrupt;
        *reinterpret_cast<Ptr*>(block.csParam) = reinterpret_cast<Ptr>(&value);
        return PBControlSync(reinterpret_cast<ParmBlkPtr>(&block));
    }
    bool RemoveBlankProbe()
    {
        auto& p = blankProbe;
        if (p.installed)
        {
            p.removeError = SlotVRemove(reinterpret_cast<QElemPtr>(&p.task),p.slot);
            if (p.removeError != noErr) return false;
            p.installed = false;
        }
        if (p.upp) { DisposeVBLUPP(p.upp); p.upp = nullptr; }
        if (p.changedInterrupt)
        {
            if (InterruptControl(p.originalInterrupt) != noErr) return false;
            p.changedInterrupt = false;
        }
        return true;
    }
    nlohmann::json BlankCapability(bool on, bool enable)
    {
        using nlohmann::json;
        auto& p = blankProbe;
        if (!on && pipelineActive) throw std::runtime_error("disable pipeline before removing its blank callback");
        VDFlagRecord interrupt{};
        const auto statusError = VideoStatus(cscGetInterrupt,interrupt);
        if (!on) RemoveBlankProbe();
        else if (!p.installed && (active || (gCursorProbe && gHardwarePointer && hardwareReady)))
        {
            long vm = 0;
            if (Gestalt(gestaltVMAttr,&vm) != noErr || (vm & (1L<<gestaltVMPresent)))
                throw std::runtime_error("blank probe requires resident code/data (virtual memory off)");
            const auto dce = GetDCtlEntry((**gDevice).gdRefNum);
            if (!dce || !*dce) throw std::runtime_error("no display driver DCE");
            p.slot = reinterpret_cast<AuxDCE*>(*dce)->dCtlSlot;
            p.device = &card;
            p.calls = 0; p.blank = 0; p.maxLine = 0; p.lastTick = 0; p.maxInterval = 0;
            p.minLine = UINT32_MAX; p.minInterval = UINT32_MAX;
            if (enable && statusError == noErr && interrupt.csMode != 0)
            {
                p.originalInterrupt = interrupt.csMode;
                if (InterruptControl(0) != noErr) throw std::runtime_error("cannot enable display interrupt");
                p.changedInterrupt = true;
            }
            p.upp = NewVBLUPP(BlankCallback);
            if (!p.upp) throw std::runtime_error("cannot allocate blank callback descriptor");
            p.task = {}; p.task.qType = vType; p.task.vblAddr = p.upp; p.task.vblCount = 1;
            p.installError = SlotVInstall(reinterpret_cast<QElemPtr>(&p.task),p.slot);
            p.installed = p.installError == noErr;
            if (!p.installed) RemoveBlankProbe();
        }
        return json{{"installed",p.installed},{"slot",p.slot},{"install_error",p.installError},
            {"remove_error",p.removeError},{"interrupt_status_error",statusError},{"interrupt_mode",interrupt.csMode},
            {"changed_interrupt",p.changedInterrupt},{"calls",uint32_t(p.calls)},{"blank",uint32_t(p.blank)},
            {"line_min",uint32_t(p.minLine)},{"line_max",uint32_t(p.maxLine)},
            {"interval_ticks_min",uint32_t(p.minInterval)},{"interval_ticks_max",uint32_t(p.maxInterval)}};
    }

    bool VideoOffset(const uint8_t* pointer, uint64_t bytes, uint32_t& offset)
    {
        // Mac QuickDraw may use the second (byte-order) 32 MiB aperture. Both
        // aliases address the same physical VRAM; CRTC takes the physical offset.
        const auto address = reinterpret_cast<uintptr_t>(pointer);
        const auto aperture = reinterpret_cast<uintptr_t>(card.frameBuffer);
        if (address < aperture || address-aperture >= 0x04000000u) return false;
        offset = (address-aperture)&0x01ffffffu;
        const auto memory = card.Read(0x00f8);
        return offset < memory && bytes <= memory-offset && !(offset&31);
    }
    bool Discover()
    {
        if (!gOpen || gSuspended || gDirect32 || !gWindow) { reason = "needs native foreground indexed display"; return false; }
        std::string registry;
        if (!FindRage128(card,registry) || !card.frameBuffer) { reason = "requires native Rage 128 (1002:5052)"; return false; }
        uint32_t screen = 0;
        if (!VideoOffset(reinterpret_cast<uint8_t*>((**(**gDevice).gdPMap).baseAddr),gStride*uint64_t(gHeight),screen)
            || (card.Read(offsetRegister)&0x81ffffffu) != screen
            || (card.Read(0x0704)>>28) != 7 || (card.Read(controlRegister)&0x8000u) || (card.Read(0x022c)&0x7ffu)*8 != unsigned(gStride))
        { reason = "unsupported display offset, pitch, or tiling"; return false; }
        VDSupportsHardwareCursorRec support{}; VDHardwareCursorDrawStateRec cursor{};
        if (VideoStatus(cscSupportsHardwareCursor,support) != noErr || !support.csSupportsHardwareCursor
            || VideoStatus(cscGetHardwareCursorDrawState,cursor) != noErr || !cursor.csCursorSet)
        { reason = "hardware cursor not active"; return false; }
        if (!gDirectPaletteSet) { reason = "verified direct palette unavailable"; return false; }
        reason.clear(); return true;
    }
    bool Flush()
    {
        const auto deadline = Microseconds()+100000;
        while ((card.Read(kGuiStat)&0x80000000u) || (card.Read(0x07b8)&0x80010000u))
            if (Microseconds() >= deadline) return false;
        card.Write(0x0184,(card.Read(0x0184)&~0xffu)|0xffu);
        while (card.Read(0x0184)&0x80000000u)
            if (Microseconds() >= deadline) return false;
        __asm__ volatile("sync" ::: "memory"); return true;
    }
    bool PictureFlush()
    {
        const auto start=Microseconds();const bool okay=Flush();flushUs+=Microseconds()-start;return okay;
    }
    bool Flip(uint32_t offset)
    {
        // FLIP_CNTL=0 latches at vertical blank; 1 means horizontal blank.
        // GUI_TRIG_OFFSET is read-only pending state, not a command bit.
        __asm__ volatile("sync" ::: "memory");
        card.Write(offsetRegister,offset);
        const auto pending = card.Read(offsetRegister)&0x40000000u;
        pendingFlips += pending != 0;
        const auto start = Microseconds(), deadline = start+50000;
        while (card.Read(offsetRegister)&0x40000000u)
        {
            if (pendingPalette && (card.Read(0x005c)&1))
            {
                const auto paletteStart = Microseconds();
                flushingPalette = true; SetPalette(gPalette); flushingPalette = false;
                pendingPalette = false; ++paletteFlushes; paletteUs += Microseconds()-paletteStart;
            }
            if (Microseconds() >= deadline) { ++errors; return false; }
        }
        const auto line = (card.Read(0x0210)>>16)&0x7ffu;
        lastLine = line;
        earlyLines += line <= 8; blankLines += line >= unsigned(gHeight);
        middleLines += line > 8 && line < unsigned(gHeight);
        firstLineMin = std::min(firstLineMin,line); firstLineMax = std::max(firstLineMax,line);
        const bool inBlank = (card.Read(0x005c)&1) != 0;
        blankCompletions += inBlank;
        outsideBlank += !inBlank;
        const auto us = Microseconds()-start;
        waitUs += us; maximumWait = std::max(maximumWait,us);
        return (card.Read(offsetRegister)&0x81ffffffu) == offset;
    }
    CGrafPtr Port(unsigned index) { return index==2 ? probeWorld : index==1 ? world : GetWindowPort(gWindow); }
    void Written(int left,int top,int right,int bottom)
    {
        if (active && damageMirror) pageDamage.Write(back,{left,top,right,bottom});
    }
    bool CopyBatch(unsigned from,unsigned to,const Rect* sources,const Rect* targets,unsigned count)
    {
        for (unsigned i=0;i<count;++i)
        {
            const auto& source=sources[i]; const auto& target=targets[i];
            const int width=target.right-target.left,height=target.bottom-target.top;
            if (width<=0 || height<=0 || source.right-source.left!=width || source.bottom-source.top!=height
                || source.left<0 || source.top<0 || source.right>gWidth || source.bottom>gHeight
                || target.left<0 || target.top<0 || target.right>gWidth || target.bottom>gHeight) return false;
        }
        if (!count) return true;
        RagePackets packets{card};
        if (!packets.CpuReady()) return false;
        uint32_t saved[std::size(kProbeSaved)];
        for (unsigned i=0;i<std::size(kProbeSaved);++i) saved[i]=card.Read(kProbeSaved[i]);
        packets.Register(kGuiMaster,0x32cc32dbu);
        packets.Pair(kSrcPitchOffset,(uint32_t(gStride/8)<<21)|(offsets[from]>>5),
            (uint32_t(gStride/8)<<21)|(offsets[to]>>5));
        packets.Register(0x16cc,0xffffffffu);
        packets.Register(kClrCmpCntl,0); packets.Register(kScTopLeft,0);
        packets.Register(kScBottomRight,(uint32_t(gHeight-1)<<16)|uint32_t(gWidth-1));
        uint32_t direction=UINT32_MAX;
        for (unsigned i=0;i<count;++i)
        {
            const auto& source=sources[i]; const auto& target=targets[i];
            const bool reverseX=from==to && target.left>source.left;
            const bool reverseY=from==to && target.top>source.top;
            const uint32_t next=(reverseX?0u:1u)|(reverseY?0u:2u);
            if (next!=direction) { packets.Register(kDpCntl,next); direction=next; }
            packets.Copy(reverseX?source.right-1:source.left,reverseY?source.bottom-1:source.top,
                reverseX?target.right-1:target.left,reverseY?target.bottom-1:target.top,
                target.right-target.left,target.bottom-target.top);
        }
        const bool copied=packets.CpuReady();
        for (unsigned i=0;i<std::size(kProbeSaved);++i) packets.Register(kProbeSaved[i],saved[i]);
        const bool restored=packets.CpuReady();
        return copied && restored;
    }
    bool Copy(unsigned from,unsigned to,const Rect& source,const Rect& target)
    {
        const bool okay=CopyBatch(from,to,&source,&target,1);
        if (okay && from==to && from==back) Written(target.left,target.top,target.right,target.bottom);
        return okay;
    }
    bool Mirror(unsigned from,unsigned to,bool allowDamage=false)
    {
        const auto start=Microseconds();
        bool okay;
        PageMirror::Damage::RegionsBuffer regions{};
        uint64_t pixels=uint64_t(gWidth)*gHeight;
        unsigned count=0;
        if (allowDamage && damageMirror && gHardwarePointer && pageDamage.enabled)
        {
            // The desk-accessory clock can write into primary video memory.
            if (to==0) pageDamage.Dirty(to,{std::max(0,gWidth-144),0,gWidth,32});
            count=pageDamage.Regions(to,regions,pixels);
        }
        if (allowDamage && damageMirror && gHardwarePointer && pageDamage.enabled && pixels*10<uint64_t(gWidth)*gHeight*9)
        {
            std::array<Rect,std::tuple_size_v<decltype(regions)>> rects{};
            for (unsigned i=0;i<count;++i) rects[i]={short(regions[i].top),short(regions[i].left),short(regions[i].bottom),short(regions[i].right)};
            okay=CopyBatch(from,to,rects.data(),rects.data(),count);
            ++mirrorPartial; mirrorRects+=count; mirrorPixels+=pixels;
        }
        else
        {
            const Rect whole{0,0,short(gHeight),short(gWidth)};
            okay=CopyBatch(from,to,&whole,&whole,1);
            ++mirrorFull; ++mirrorRects; mirrorPixels+=uint64_t(gWidth)*gHeight;
        }
        if (okay) pageDamage.Copied(from,to);
        mirrorUs+=Microseconds()-start;
        return okay;
    }
    bool Scroll(const Rect& source, const Rect& target)
    {
        const auto start=Microseconds();
        WaitForScroll();
        const bool okay=Copy(back,back,source,target);
        ++scrollCopies; scrollUs+=Microseconds()-start;
        return okay;
    }
    bool Drain()
    {
        timingFault = false;
        auto& p = blankProbe;
        if (!p.jobState) return true;
        const auto began = Microseconds(), deadline = began+50000;
        bool waited = false;
        while (p.jobState != 2 || (card.Read(offsetRegister)&0x40000000u))
        {
            waited = true;
            if (p.jobState == 3) { ++errors; reason = "secondary blank handler failed"; return false; }
            if (Microseconds() >= deadline)
            { ++errors; timingFault = true; reason = "asynchronous display latch timed out"; return false; }
        }
        __asm__ volatile("sync" ::: "memory");
        const auto us = Microseconds()-began;
        waitUs += us; maximumWait = std::max(maximumWait,us);
        drainWaits += waited;
        if ((card.Read(offsetRegister)&0x81ffffffu) != p.jobOffset)
        { ++errors; reason = "unexpected asynchronous display address"; return false; }
        front = p.jobPage;
        if (p.inputTick && latencyCount < std::size(latency))
            latency[latencyCount++] = {p.latchTick-p.inputTick,p.latchTick-p.readyTick,true};
        p.inputTick = 0;
        // The interrupt changed only the hardware. Acknowledge its exact
        // palette in the main task, where QuickDraw calls are permitted.
        if (p.last >= p.first)
        {
            ++gDirectPaletteSets;
            if (p.last-p.first >= 64) ++gBroadPaletteSets;
            const auto table = (**(**gDevice).gdPMap).pmTable;
            for (int i=p.first;i<=p.last;++i)
            {
                gShown[i] = p.colours[i];
                auto& entry = (**table).ctTable[i]; entry.value = i;
                entry.rgb = {uint16_t(p.colours[i].red<<8),uint16_t(p.colours[i].green<<8),uint16_t(p.colours[i].blue<<8)};
            }
            (**table).ctSeed = GetCTSeed();
            gShownValid = true;
            if (gHavePointerShape && PointerColoursSwapped() != gPointerSwapped) ApplyPointerShape();
            ++paletteFlushes;
        }
        paletteUs += uint64_t(p.lastPaletteTicks/ticksPerUs);
        lastLine = (card.Read(0x0210)>>16)&0x7ffu;
        p.jobState = 0;
        if (p.overran) { ++errors; reason = "asynchronous palette exceeded vertical blank"; return false; }
        // Sound Manager can delay isolated callbacks beyond blank. No writes
        // occur on those callbacks; the next blank retries the complete job.
        // Keep the latch-timeout and overrun guards. A callback miss is not a
        // hardware failure: loading may delay several consecutive interrupts.
        // A frequency threshold rejected working sync even when the spare page
        // absorbed every isolated delay (measured with voices on the G3).
        if (pipelineRetry.delay > RetrySchedule::initialDelay && began-pipelineHealthySince >= 10000000)
            pipelineRetry.Reset(); // ten healthy seconds restore the short retry delay
        return true;
    }
    bool Publish()
    {
        auto& p = blankProbe;
        const auto began=Microseconds();
        if (!Drain() || !gDirectPaletteSet || !p.installed) return false;
        p.first = 256; p.last = -1;
        std::memcpy(p.colours,gPalette,sizeof(gPalette));
        if (pendingPalette)
            for (int i=0;i<256;++i)
                if (!gShownValid || gPalette[i].red!=gShown[i].red || gPalette[i].green!=gShown[i].green || gPalette[i].blue!=gShown[i].blue)
                { p.first = std::min(p.first,i); p.last = i; }
        for (int i=p.first;i<=p.last;++i)
            p.packed[i] = (uint32_t(gGammaTable[0][gPalette[i].red])<<16)
                | (uint32_t(gGammaTable[1][gPalette[i].green])<<8) | gGammaTable[2][gPalette[i].blue];
        p.jobPage = back; p.jobOffset = offsets[back];
        p.inputTick = inputTick; inputTick = 0; p.readyTick = TimeBaseLow();
        pendingPalette = false;
        readyLine=(card.Read(0x0210)>>16)&0x7ffu;
        const uint32_t age=p.readyTick-p.lastTick;
        phaseUs=age<=p.refreshTicks*2 ? uint32_t(age/ticksPerUs):0;
        refreshUs=uint32_t(p.refreshTicks/ticksPerUs);
        __asm__ volatile("sync" ::: "memory");
        p.jobState = 1;
        publishUs+=Microseconds()-began;
        ++queuedFlips; ++flips;
        lastFrame = OpenRCT2::FrameTiming::stats.frames+1;
        return true;
    }
    nlohmann::json ThirdCapability()
    {
        using nlohmann::json;
        if (!active || pipelineActive) throw std::runtime_error("third-page probe needs ordinary native sync");
        WaitForScroll();
        if (!Flush() || (card.Read(offsetRegister)&0x40000000u))
            throw std::runtime_error("display busy before third-page probe");
        const uint32_t visible = card.Read(offsetRegister)&0x01ffffffu;
        unsigned front = visible==offsets[0]?0:1;
        if (visible != offsets[front]) throw std::runtime_error("unknown displayed page");
        const uint64_t bytes = uint64_t(gStride)*gHeight;
        OSErr error = noErr;
        if (!probeWorld)
        {
            Rect bounds{0,0,short(gHeight),short(gStride)};
            error = NewGWorld(&probeWorld,8,&bounds,(**(**gDevice).gdPMap).pmTable,nullptr,useDistantHdwrMem);
            if (error != noErr || !probeWorld) { probeWorld = nullptr; return json{{"allocated",false},{"error",error}}; }
            const auto map = GetGWorldPixMap(probeWorld);
            const bool locked = LockPixels(map);
            pages[2] = reinterpret_cast<uint8_t*>(GetPixBaseAddr(map));
            const bool compatible = locked && ((**map).rowBytes&0x3fff)==gStride && VideoOffset(pages[2],bytes,offsets[2])
                && !(offsets[2]<offsets[0]+bytes && offsets[2]+bytes>offsets[0])
                && !(offsets[2]<offsets[1]+bytes && offsets[2]+bytes>offsets[1]);
            if (!compatible)
            {
                if (locked) UnlockPixels(map);
                DisposeGWorld(probeWorld); probeWorld = nullptr;
                return json{{"allocated",true},{"compatible",false},{"error",error}};
            }
        }
        const bool copied = Mirror(front,2);
        if (!copied) return json{{"allocated",true},{"compatible",true},{"copied",false}};
        uint32_t differences = 0;
        for (int y=0;y<gHeight;++y) for (int x=0;x<gWidth;++x)
            differences += static_cast<volatile uint8_t*>(pages[front])[y*gStride+x]
                != static_cast<volatile uint8_t*>(pages[2])[y*gStride+x];
        const bool latched = differences==0 && Flip(offsets[2]);
        const bool restored = Flip(visible);
        if (!restored) { failed = true; reason = "third-page display restoration timed out"; Stop(); }
        return json{{"allocated",true},{"compatible",true},{"copied",copied},{"differences",differences},
            {"latched",latched},{"restored",restored},{"offset",offsets[2]},{"primary",offsets[0]},{"second",offsets[1]}};
    }
    bool Stop()
    {
        gDriverPaletteForSync = true; // drain/restore uses the blank-aligned driver path
        const bool wasPipeline = pipelineActive;
        const bool drained = Drain();
        if (!RemoveBlankProbe()) { reason = "cannot remove blank callback"; failed = true; ++errors; return false; }
        pipelineActive = false;
        blankProbe.jobState = 0;
        blankProbe.suppressSubmission = blankProbe.forceLate = blankProbe.forceOverrun = false;
        if (!drained) gShownValid = false;
        if (!world) { active = false; gDriverPaletteForSync = requested; return true; }
        WaitForScroll();
        if (wasPipeline && drained && front!=0 && !Mirror(front,0))
        { failed = true; reason = "primary-page copy timed out"; ++errors; }
        if (!Flush() || !Flip(offsets[0]))
        {
            // Do not free memory while the display might still scan it.
            active = false; failed = true; reason = "display restoration timed out"; ++errors;
            return false;
        }
        if ((card.Read(controlRegister)&~0x40000000u) != originalControl)
            card.Write(controlRegister,originalControl);
        gScreen = pages[0]; active = false;
        if (pendingPalette) { pendingPalette = false; SetPalette(gPalette); }
        UnlockPixels(GetGWorldPixMap(world)); DisposeGWorld(world); world = nullptr;
        if (probeWorld) { UnlockPixels(GetGWorldPixMap(probeWorld)); DisposeGWorld(probeWorld); probeWorld = nullptr; }
        gDriverPaletteForSync = requested;
        return true;
    }
    void ResetMode() { failed = pipelineFailed = false; pipelineRetry.Reset(); }
    bool Start()
    {
        if (active) return true;
        // Engine initialization can request a saved setting before display and
        // palette discovery. That is transient, not an unsupported card.
        if (!gOpen || gSuspended || !gHavePalette || !gDirectPaletteChecked) return false;
        if (failed) return false;
        if (!Discover()) { failed = true; return false; }
        if (!CanScroll()) return false;
        pages[0] = reinterpret_cast<uint8_t*>((**(**gDevice).gdPMap).baseAddr);
        VideoOffset(pages[0],uint64_t(gStride)*gHeight,offsets[0]);
        originalControl = card.Read(controlRegister);
        Rect bounds{0,0,static_cast<short>(gHeight),static_cast<short>(gStride)};
        const auto colours = (**(**gDevice).gdPMap).pmTable;
        const auto error = NewGWorld(&world,8,&bounds,colours,nullptr,useDistantHdwrMem);
        if (error != noErr || !world) { world = nullptr; failed = true; reason = "no off-screen video page"; return false; }
        const auto map = GetGWorldPixMap(world);
        const bool locked = LockPixels(map);
        pages[1] = reinterpret_cast<uint8_t*>(GetPixBaseAddr(map));
        const uint64_t bytes = uint64_t(gStride)*gHeight;
        if (!locked || ((**map).rowBytes&0x3fff) != gStride || !VideoOffset(pages[1],bytes,offsets[1])
            || (offsets[1] < offsets[0]+bytes && offsets[1]+bytes > offsets[0]))
        {
            if (locked) UnlockPixels(map); DisposeGWorld(world); world = nullptr;
            failed = true; reason = "off-screen page not compatible video memory"; return false;
        }
        pageDamage.Reset(gWidth,gHeight);
        if (!Mirror(0,1)) { UnlockPixels(map); DisposeGWorld(world); world = nullptr; reason = "initial video copy timed out"; failed = true; ++errors; return false; }
        card.Write(controlRegister,originalControl&~0x10000u);
        front = 0; back = 1; gScreen = pages[back]; active = true; gDriverPaletteForSync = true;
        // Discovery compares the primary device base. Once flipping starts,
        // the visible page can differ; cache the already verified capability.
        hardwareReady = availabilityKnown = true; reason.clear();
        if (pipelineRequested && !pipelineFailed)
        {
            // Keep ordinary sync as the fallback if the extra page or the
            // driver's slot callback is unavailable. Never enable in QEMU.
            try
            {
                // Updating OFFSET_CNTL can itself mark the primary offset
                // pending. Resolve that initial latch before probing a page.
                if (!Flush() || !Flip(offsets[0])) throw std::runtime_error("initial display latch timed out");
                const auto third = ThirdCapability();
                if (!third.value("latched",false) || !third.value("restored",false))
                    throw std::runtime_error("third video page unavailable");
                if (!ClassicInterruptMask::Prepare()) throw std::runtime_error("resident interrupt-mask helper unavailable");
                blankProbe.secondary = Services().secondary;
                blankProbe.level = Services().level;
                BlankCapability(true,true);
                if (!blankProbe.installed) throw std::runtime_error("slot blank callback unavailable");
                long bus = 0;
                if (Gestalt(gestaltBusClkSpeed,&bus)!=noErr || bus<=0) throw std::runtime_error("timebase frequency unavailable");
                ticksPerUs = bus/4000000.0;
                blankProbe.totalLines = (card.Read(0x0208)&0x7ffu)+1;
                if (blankProbe.totalLines <= unsigned(gHeight)+20) throw std::runtime_error("vertical blank too short");
                blankProbe.overran = 0;
                blankProbe.lateStreak = 0;
                lateWindowJobs = 0; lateWindowCount = blankProbe.late;
                pipelineActive = true; gDriverPaletteForSync = false;
                pipelineRetry.due = 0; pipelineHealthySince = Microseconds();
            }
            catch (const std::exception& e)
            {
                if (!RemoveBlankProbe()) { failed = true; reason = "cannot remove blank callback"; ++errors; return false; }
                pipelineFailed = true; reason = e.what(); ++fallbacks;
                pipelineRetry.due = 0; // unsupported setup: no automatic capability probes
                // Stop owns video-page disposal, after proving primary scanout
                // is restored. A failed restoration must retain those pages.
                std::printf("sync pipeline fallback %dx%d: %s\n",gWidth,gHeight,reason.c_str());
            }
        }
        return true;
    }
    void RecoverPipeline()
    {
        const auto fault = reason.empty() ? std::string("asynchronous video operation failed") : reason;
        const bool retry = timingFault && pipelineRequested;
        // An asynchronous-only failure must retain the user's sync request.
        // Remove the task and restore primary scanout before using ordinary
        // two-page sync. Timing failures re-arm after loading with capped
        // backoff; overruns, handler/driver errors and unsafe restoration do not.
        if (!Stop()) { failed = true; return; }
        blankProbe.suppressSubmission = false;
        pipelineFailed = true; failed = false;
        ++fallbacks;
        Start(); reason = fault+"; using ordinary page-flip sync";
        const uint32_t retryDelay = retry ? pipelineRetry.delay : 0;
        if (retry) pipelineRetry.Arm(Microseconds()); else pipelineRetry.due = 0;
        // Classic printf does not implement ll: use native-width log fields;
        // the JSON diagnostics retain the full 64-bit counters.
        std::printf("sync pipeline fallback %dx%d: %s (late %lu overruns %lu errors %lu retry_us %lu loading %u)\n",
            gWidth,gHeight,reason.c_str(),static_cast<unsigned long>(blankProbe.late),
            static_cast<unsigned long>(blankProbe.overran),static_cast<unsigned long>(errors),
            static_cast<unsigned long>(retryDelay),
            unsigned(::ClassicLoadProgress::Active()));
        OpenRCT2::Drawing::GfxInvalidateScreen();
    }
    void End()
    {
        if (!requested || gSuspended || !gOpen) return;
        if (!CanScroll()) { Stop(); return; }
        if (!active) { Start(); return; }
        if (pipelineRetry.due && pipelineRetry.Ready(Microseconds(),::ClassicLoadProgress::Active()))
        {
            pipelineRetry.due = 0;
            if (!Stop()) return; // never release pages without proving scanout restoration
            pipelineFailed = false; ++pipelineRetries;
            if (Start() && pipelineActive)
                std::printf("sync pipeline re-armed %dx%d after timing fallback\n",gWidth,gHeight);
            OpenRCT2::Drawing::GfxInvalidateScreen();
            return; // the frame drawn into the old back page needs a fresh repaint
        }
        WaitForScroll();
        if (pipelineActive)
        {
            if (!Drain() || !PictureFlush()) { ++errors; RecoverPipeline(); return; }
            const unsigned next = 3-front-back;
            if (next>=3 || next==front || next==back || !Publish() || !Mirror(back,next,true))
            { ++errors; RecoverPipeline(); return; }
            back = next; gScreen = pages[back];
            return;
        }
        const auto ready = TimeBaseLow();
        if (!PictureFlush() || !Flip(offsets[back])) { failed = true; Stop(); ++errors; return; }
        if (inputTick && latencyCount < std::size(latency))
        {
            const auto at = TimeBaseLow();
            latency[latencyCount++] = {at-inputTick,at-ready,false}; inputTick = 0;
        }
        front = back;
        ++flips; lastFrame = OpenRCT2::FrameTiming::stats.frames + 1;
        const unsigned next = back^1;
        if (!Mirror(back,next,true)) { failed = true; Stop(); ++errors; return; }
        back = next; gScreen = pages[back];
    }
    bool Verify()
    {
        if (!active) return false;
        if (!Drain()) return false;
        WaitForScroll(); if (!Flush()) return false;
        const unsigned other = pipelineActive ? front : back^1;
        for (int y = 0; y < gHeight; ++y)
            for (int x = 0; x < gWidth; ++x)
                if (static_cast<volatile uint8_t*>(pages[back])[y*gStride+x]
                    != static_cast<volatile uint8_t*>(pages[other])[y*gStride+x]) return false;
        return true;
    }

    // Opt-in diagnostics read the CRTC's displayed page, never an engine RAM
    // screenshot or an assumption about which page was most recently drawn.
    bool DisplayCopy(std::vector<uint8_t>& pixels, uint32_t& offset)
    {
        if (!Drain() || (!active && !Discover())) return false;
        WaitForScroll();
        if (!Flush()) return false;
        const auto value = card.Read(offsetRegister);
        if (value & 0x40000000u) return false;
        offset = value & 0x01ffffffu;
        const volatile uint8_t* source = nullptr;
        if (active)
        {
            if (offset == offsets[0]) source=pages[0];
            else if (offset == offsets[1]) source=pages[1];
            else if (probeWorld && offset == offsets[2]) source=pages[2];
        }
        else
        {
            auto* primary=reinterpret_cast<uint8_t*>((**(**gDevice).gdPMap).baseAddr);
            uint32_t physical=0;
            if (VideoOffset(primary,uint64_t(gStride)*gHeight,physical) && physical==offset) source=primary;
        }
        if (!source) return false;
        pixels.resize(size_t(gWidth) * gHeight);
        for (int y = 0; y < gHeight; ++y)
            for (int x = 0; x < gWidth; ++x)
                pixels[size_t(y)*gWidth+x] = source[size_t(y)*gStride+x];
        return (card.Read(offsetRegister)&0x41ffffffu) == offset;
    }
    void WritePicture(const std::string& path, const std::vector<uint8_t>& pixels)
    {
        if (path.rfind("/RCT/user/Perf/",0) != 0 || path.find("..") != std::string::npos)
            throw std::invalid_argument("readback output must be under /RCT/user/Perf/");
        const auto native = ClassicVfs::HostPath(path);
        auto* file = std::fopen(native.c_str(),"wb");
        if (!file) throw std::runtime_error("cannot open readback output");
        std::fprintf(file,"P5\n%d %d\n255\n",gWidth,gHeight);
        const auto written = std::fwrite(pixels.data(),1,pixels.size(),file);
        const auto error = std::fclose(file);
        if (written != pixels.size() || error) throw std::runtime_error("cannot write readback output");
    }
    nlohmann::json CompareDisplay(const nlohmann::json& command)
    {
        using nlohmann::json;
        std::vector<uint8_t> displayed, reference;
        uint32_t offset = 0;
        if (!DisplayCopy(displayed,offset) || !PaintFullCopy(reference,0,""))
            throw std::runtime_error("displayed-page comparison unavailable");
        if (displayed.size() != reference.size()) throw std::runtime_error("reference dimensions differ");
        const auto region = command.value("region",std::array<int,4>{0,0,gWidth,gHeight});
        if (region[0]<0 || region[1]<0 || region[2]>gWidth || region[3]>gHeight
            || region[0]>=region[2] || region[1]>=region[3]) throw std::invalid_argument("bad comparison region");
        uint64_t differences = 0, all = 0, hud = 0, topEdge = 0;
        json first = json::array();
        std::array<int,4> bounds{gWidth,gHeight,0,0};
        for (int y = 0; y < gHeight; ++y)
            for (int x = 0; x < gWidth; ++x)
            {
                const size_t index = size_t(y)*gWidth+x;
                if (displayed[index] == reference[index]) continue;
                ++all;
                if (y<30 || y>=gHeight-34) ++hud;
                if (y>=30 && y<46 && x>=140) ++topEdge;
                if (x<region[0] || x>=region[2] || y<region[1] || y>=region[3]) continue;
                ++differences;
                bounds[0]=std::min(bounds[0],x); bounds[1]=std::min(bounds[1],y);
                bounds[2]=std::max(bounds[2],x+1); bounds[3]=std::max(bounds[3],y+1);
                if (first.size()<8) first.push_back({x,y,displayed[index],reference[index]});
            }
        const auto prefix = command.value("output",std::string{});
        if (!prefix.empty())
        {
            WritePicture(prefix+"-display.pgm",displayed);
            WritePicture(prefix+"-reference.pgm",reference);
        }
        return {{"offset",offset},{"display_page",offset==offsets[2]?2:offset==offsets[1]?1:0},{"back",back},
            {"width",gWidth},{"height",gHeight},{"region",region},{"differences",differences},
            {"all_differences",all},{"hud_differences",hud},{"top_edge_differences",topEdge},
            {"bounds",bounds},{"first",first}};
    }
    nlohmann::json OverlapProbe(bool direct)
    {
        using nlohmann::json;
        if (!active) throw std::runtime_error("overlap probe needs a hidden page");
        if (!Drain()) throw std::runtime_error("pending display latch timed out");
        WaitForScroll();
        if (!Flush() || (card.Read(offsetRegister)&0x41ffffffu) == offsets[back])
            throw std::runtime_error("probe page is displayed or busy");
        constexpr int left=32, top=64, width=256, height=128;
        std::vector<uint8_t> saved(width*height), pattern(width*height), expected;
        auto* pixels = static_cast<volatile uint8_t*>(pages[back]);
        for (int y=0;y<height;++y) for (int x=0;x<width;++x)
        {
            saved[y*width+x]=pixels[(top+y)*gStride+left+x];
            pattern[y*width+x]=uint8_t((x*37+y*71+(x/11)*19+(y/7)*31)%251+1);
        }
        const auto port=Port(back);
        GrafPtr previous; GetPort(&previous); SetPort(reinterpret_cast<GrafPtr>(port));
        const auto fg=port->fgColor, bg=port->bkColor;
        const auto rgbFg=port->rgbFgColor, rgbBg=port->rgbBkColor;
        port->fgColor=255; port->bkColor=0;
        port->rgbFgColor={0,0,0}; port->rgbBkColor={0xffff,0xffff,0xffff};
        json tests=json::array();
        for (const auto delta:std::array<std::array<int,2>,8>{{{7,0},{-7,0},{0,7},{0,-7},{7,7},{-7,7},{7,-7},{-7,-7}}})
        {
            for (int y=0;y<height;++y) for (int x=0;x<width;++x)
                pixels[(top+y)*gStride+left+x]=pattern[y*width+x];
            __asm__ volatile("sync" ::: "memory");
            expected=pattern;
            Rect target{80,64,160,224}, source=target;
            OffsetRect(&source,short(-delta[0]),short(-delta[1]));
            for (int y=target.top;y<target.bottom;++y) for (int x=target.left;x<target.right;++x)
                expected[(y-top)*width+x-left]=pattern[(y-delta[1]-top)*width+x-delta[0]-left];
            const auto* bitmap=&reinterpret_cast<GrafPtr>(port)->portBits;
            bool copied=true;
            if (direct) copied=Scroll(source,target);
            else
            {
                CopyBits(bitmap,bitmap,&source,&target,srcCopy,nullptr);
                const auto deadline=Microseconds()+100000;
                while (!QDDone(reinterpret_cast<GrafPtr>(port)) && Microseconds()<deadline) {}
                copied=QDDone(reinterpret_cast<GrafPtr>(port));
            }
            const bool flushed=Flush() && copied;
            unsigned count=0; json first=json::array();
            for (int y=0;y<height;++y) for (int x=0;x<width;++x)
            {
                const auto actual=pixels[(top+y)*gStride+left+x];
                if (actual!=expected[y*width+x])
                {
                    ++count;
                    if (first.empty()) first={left+x,top+y,actual,expected[y*width+x]};
                }
            }
            tests.push_back({{"delta",delta},{"differences",count},{"first",first},{"flushed",flushed},
                {"dp_cntl",card.Read(kDpCntl)},{"src_pitch",card.Read(0x15b0)},{"dst_pitch",card.Read(0x1408)}});
        }
        for (int y=0;y<height;++y) for (int x=0;x<width;++x)
            pixels[(top+y)*gStride+left+x]=saved[y*width+x];
        __asm__ volatile("sync" ::: "memory");
        port->fgColor=fg; port->bkColor=bg; port->rgbFgColor=rgbFg; port->rgbBkColor=rgbBg;
        SetPort(previous);
        return {{"page",back},{"offset",offsets[back]},{"direct",direct},{"tests",tests}};
    }
}
} // anonymous namespace
void MarkPictureInput() { NativeVSync::inputTick = NativeVSync::TimeBaseLow(); }
void SetVSync(bool on)
{
    if (on && !NativeVSync::requested)
    { NativeVSync::failed = false; NativeVSync::pipelineFailed = false; NativeVSync::pipelineRetry.Reset(); }
    NativeVSync::requested = on;
    gDriverPaletteForSync = on;
    if (!on) NativeVSync::Stop();
    else NativeVSync::Start();
}
bool VSyncAvailable()
{
    if (!NativeVSync::availabilityKnown && gOpen && !gSuspended && gDirectPaletteChecked)
    {
        NativeVSync::hardwareReady = NativeVSync::Discover();
        NativeVSync::availabilityKnown = true;
    }
    return NativeVSync::active || NativeVSync::hardwareReady;
}

VSyncTiming VSyncCosts()
{
    return {NativeVSync::waitUs, NativeVSync::mirrorUs, NativeVSync::paletteUs,
        NativeVSync::lastFrame, NativeVSync::lastLine,NativeVSync::flushUs,NativeVSync::publishUs,
        NativeVSync::mirrorPixels,NativeVSync::mirrorRects,NativeVSync::mirrorFull,NativeVSync::mirrorPartial,
        uint32_t(NativeVSync::blankProbe.late),NativeVSync::queuedFlips,uint32_t(NativeVSync::blankProbe.jobs),
        NativeVSync::readyLine,NativeVSync::phaseUs,NativeVSync::refreshUs};
}

std::string VSyncDiagnostics(const std::string& text)
{
    using nlohmann::json;
    const auto c = json::parse(text);
    if (!NativeVSync::Drain()) throw std::runtime_error(NativeVSync::reason);
    if (c.contains("faults"))
    {
        const auto& p=NativeVSync::blankProbe;
        const uint32_t oldest=p.rejectedCount>64?p.rejectedCount-64:0;
        const auto first=std::max(oldest,c.value("first",0u));
        json rows=json::array();
        for (uint32_t i=first;i<p.rejectedCount && i<first+8;i++)
        {
            const auto& r=p.rejected[i%64]; rows.push_back({i,r.tick,r.line,r.status,r.offset,r.gap,r.reason});
        }
        return json{{"rejected",rows},{"oldest",oldest},{"total",uint32_t(p.rejectedCount)},
            {"total_lines",p.totalLines},{"refresh_ticks",uint32_t(p.refreshTicks)},{"ticks_per_us",NativeVSync::ticksPerUs}}.dump();
    }
    if (c.value("commit_paths_probe", false)) return NativeVSync::CommitPathsProbe();
    if (c.value("mask_probe", false))
    {
        if (!ClassicInterruptMask::Prepare()) throw std::runtime_error("resident mask helper unavailable");
        struct Probe { unsigned entered=0; UInt16 before=99,during=99,after=99; } probe;
        const auto handler = +[](void* raw,void*) -> OSStatus {
            auto& p=*static_cast<Probe*>(raw);p.entered++;
            p.before=ClassicInterruptMask::Get();
            { ClassicInterruptMask::Guard guard;p.during=ClassicInterruptMask::Get(); }
            p.after=ClassicInterruptMask::Get();return noErr;
        };
        const auto error=c.value("secondary",false) ?
            (NativeVSync::Services().secondary ? NativeVSync::Services().secondary(handler,nullptr,&probe,nullptr) : -1)
            : handler(&probe,nullptr);
        return json{{"error",error},{"entered",probe.entered},{"before",probe.before},
            {"during",probe.during},{"after",probe.after}}.dump();
    }
    if (c.value("secondary_probe", false))
    {
        struct Probe { NativeVSync::DriverServices* services; unsigned entered = 0, level = 0; } probe{&NativeVSync::Services()};
        const auto handler = +[](void* raw, void*) -> OSStatus {
            auto& p = *static_cast<Probe*>(raw); p.entered++;
            if (p.services->level) p.level = p.services->level();
            return noErr;
        };
        const auto before = probe.services->level ? probe.services->level() : 99;
        const auto error = probe.services->secondary ? probe.services->secondary(handler,nullptr,&probe,nullptr) : -1;
        return json{{"available",probe.services->secondary!=nullptr},{"error",error},{"entered",probe.entered},
            {"before",before},{"inside",probe.level},{"after",probe.services->level ? probe.services->level() : 99}}.dump();
    }
    if (c.contains("secondary_blank")) NativeVSync::secondaryBlank = c["secondary_blank"].get<bool>();
    if (c.value("latency_reset",false)) NativeVSync::latencyCount = 0;
    if (c.value("latency",false))
    {
        json samples = json::array();
        long bus = 0; Gestalt(gestaltBusClkSpeed,&bus);
        const double ticksPerUs = bus/4000000.0;
        if (ticksPerUs <= 0) throw std::runtime_error("timebase frequency unavailable");
        for (unsigned i=0;i<NativeVSync::latencyCount;++i)
        {
            const auto& s = NativeVSync::latency[i];
            samples.push_back({s.inputTicks/ticksPerUs,s.readyTicks/ticksPerUs,s.pipeline});
        }
        return json{{"latency_us",samples},{"pipeline_edge_estimated",true},
            {"refresh_us",NativeVSync::blankProbe.refreshTicks/ticksPerUs}}.dump();
    }
    if (c.contains("on")) SetVSync(c["on"].get<bool>());
    if (c.contains("damage_mirror"))
    {
        NativeVSync::damageMirror=c.at("damage_mirror").get<bool>();
        NativeVSync::pageDamage.Reset(gWidth,gHeight,NativeVSync::active?NativeVSync::back:0);
    }
    if (c.contains("pipeline") && (c["pipeline"].get<bool>() != NativeVSync::pipelineRequested
        || (c["pipeline"].get<bool>() && NativeVSync::pipelineFailed)))
    {
        if (!NativeVSync::Stop()) throw std::runtime_error(NativeVSync::reason);
        NativeVSync::pipelineRequested = c["pipeline"].get<bool>(); NativeVSync::failed = NativeVSync::pipelineFailed = false;
        NativeVSync::pipelineRetry.Reset();
        if (NativeVSync::requested) NativeVSync::Start();
    }
    if (c.value("stall_callback",false))
    {
        if (!NativeVSync::pipelineActive) throw std::runtime_error("timeout diagnostic needs active pipeline");
        NativeVSync::blankProbe.suppressSubmission = true;
    }
    if (c.value("late_callback",false) || c.value("overrun_callback",false))
    {
        if (!NativeVSync::pipelineActive) throw std::runtime_error("fault diagnostic needs active pipeline");
        NativeVSync::blankProbe.forceLate = c.value("late_callback",false);
        NativeVSync::blankProbe.forceOverrun = c.value("overrun_callback",false);
    }
    if (c.contains("blank_probe")) return json{{"blank_probe",NativeVSync::BlankCapability(c["blank_probe"].get<bool>(),c.value("enable_interrupt",false))}}.dump();
    if (c.value("third_probe",false)) return json{{"third_probe",NativeVSync::ThirdCapability()}}.dump();
    if (c.value("compare",false)) return json{{"comparison",NativeVSync::CompareDisplay(c)}}.dump();
    if (c.value("overlap_probe",false)) return json{{"overlap",NativeVSync::OverlapProbe(c.value("direct",false))}}.dump();
    json result = {{"available",false},{"requested",NativeVSync::requested},{"active",NativeVSync::active},
        {"reason",NativeVSync::reason},{"flips",NativeVSync::flips},{"wait_us",NativeVSync::waitUs},
        {"mirror_us",NativeVSync::mirrorUs},{"damage_mirror",NativeVSync::damageMirror},{"mirror_pixels",NativeVSync::mirrorPixels},{"mirror_rects",NativeVSync::mirrorRects},{"mirror_full",NativeVSync::mirrorFull},{"mirror_partial",NativeVSync::mirrorPartial},{"flush_us",NativeVSync::flushUs},{"publish_us",NativeVSync::publishUs},{"max_wait_us",NativeVSync::maximumWait},{"errors",NativeVSync::errors},{"pending_flips",NativeVSync::pendingFlips},{"blank_completions",NativeVSync::blankCompletions},{"outside_blank",NativeVSync::outsideBlank},{"first_line_min",NativeVSync::firstLineMin},{"first_line_max",NativeVSync::firstLineMax},{"phase_lines",{NativeVSync::earlyLines,NativeVSync::blankLines,NativeVSync::middleLines}},{"palette_flushes",NativeVSync::paletteFlushes},{"palette_us",NativeVSync::paletteUs}};
    result["scroll_copies"]=NativeVSync::scrollCopies; result["scroll_us"]=NativeVSync::scrollUs;
    const auto& p = NativeVSync::blankProbe;
    result["pipeline"] = {{"requested",NativeVSync::pipelineRequested},{"active",NativeVSync::pipelineActive},
        {"failed",NativeVSync::pipelineFailed},
        {"retry_pending",NativeVSync::pipelineRetry.due!=0},{"retries",NativeVSync::pipelineRetries},
        {"next_retry_delay_us",NativeVSync::pipelineRetry.delay},
        {"fallbacks",NativeVSync::fallbacks},{"late_streak",uint32_t(p.lateStreak)},
        {"queued",NativeVSync::queuedFlips},{"submitted",uint32_t(p.jobs)},{"drain_waits",NativeVSync::drainWaits},
        {"late_callbacks",uint32_t(p.late)},{"blank_overruns",uint32_t(p.overran)},
        {"submit_line_min",uint32_t(p.submitMin)},{"submit_line_max",uint32_t(p.submitMax)},
        {"finish_line_max",uint32_t(p.finishMax)},{"job_ticks",uint32_t(p.jobTicks)},
        {"palette_ticks",uint32_t(p.paletteTicks)},{"max_job_ticks",uint32_t(p.maximumJobTicks)},
        {"ticks_per_us",NativeVSync::ticksPerUs},{"callback_installed",p.installed},{"front",NativeVSync::front},
        {"secondary_blank",NativeVSync::secondaryBlank},{"secondary_available",p.secondary!=nullptr},
        {"callback_level",uint32_t(p.callbackLevel)},{"commit_level",uint32_t(p.commitLevel)},
        {"secondary_errors",uint32_t(p.secondaryErrors)},{"total_lines",p.totalLines},
        {"overrun_detail",{uint32_t(p.overrunBegin),uint32_t(p.overrunEnd),uint32_t(p.overrunCheck),
            uint32_t(p.overrunStartLine),uint32_t(p.overrunEndLine),uint32_t(p.overrunFinalLine)}}};
    if (c.value("verify",false)) result["identical_pages"] = NativeVSync::Verify();
    if (c.value("reset",false))
    {
        auto& p = NativeVSync::blankProbe;
        NativeVSync::queuedFlips = NativeVSync::drainWaits = 0;
        p.jobs = 0; p.late = 0; p.overran = 0; p.jobTicks = 0; p.paletteTicks = 0;
        NativeVSync::lateWindowJobs = NativeVSync::lateWindowCount = 0;
        p.submitMin = UINT32_MAX; p.submitMax = 0; p.finishMax = 0; p.maximumJobTicks = 0;
        NativeVSync::mirrorPixels=NativeVSync::mirrorRects=NativeVSync::mirrorFull=NativeVSync::mirrorPartial=0;
        NativeVSync::flushUs=NativeVSync::publishUs=0;
        NativeVSync::flips = NativeVSync::waitUs = NativeVSync::mirrorUs = NativeVSync::maximumWait = NativeVSync::errors = 0;
        NativeVSync::scrollCopies=NativeVSync::scrollUs=0;
        NativeVSync::pendingFlips = NativeVSync::blankCompletions = NativeVSync::outsideBlank = 0;
        NativeVSync::earlyLines = NativeVSync::blankLines = NativeVSync::middleLines = NativeVSync::paletteFlushes = NativeVSync::paletteUs = 0;
        NativeVSync::firstLineMin = UINT32_MAX; NativeVSync::firstLineMax = 0;
    }
    if (!gOpen || gSuspended || gDirect32) return result.dump();
    Rage128 card; std::string registry;
    if (!FindRage128(card, registry) || !card.frameBuffer) return result.dump();
    result["available"] = true;
    result["offset"] = card.Read(0x0224); result["offset_control"] = card.Read(0x0228);
    result["status"] = card.Read(0x005c); result["line"] = card.Read(0x0210);
    result["vertical"] = card.Read(0x0208); result["memory_bytes"] = card.Read(0x00f8);
    result["screen_offset"] = reinterpret_cast<uintptr_t>(gScreen) - reinterpret_cast<uintptr_t>(card.frameBuffer);
    result["stride"] = gStride; result["crtc_pitch"] = card.Read(0x022c);
    result["pages"] = {NativeVSync::offsets[0],NativeVSync::offsets[1],NativeVSync::back};
    result["third_offset"] = NativeVSync::probeWorld ? NativeVSync::offsets[2] : 0;
    VDSupportsHardwareCursorRec support{}; VDHardwareCursorDrawStateRec cursor{};
    const auto se = VideoStatus(cscSupportsHardwareCursor,support);
    const auto ce = VideoStatus(cscGetHardwareCursorDrawState,cursor);
    result["cursor"] = {se,support.csSupportsHardwareCursor,ce,cursor.csCursorSet,cursor.csCursorVisible,cursor.csCursorX,cursor.csCursorY};
    result["modes"] = json::array();
    VDResolutionInfoRec resolution{}; resolution.csPreviousDisplayModeID = kDisplayModeIDFindFirstResolution;
    for (unsigned i = 0; i < 64; ++i)
    {
        const auto error = VideoStatus(cscGetNextResolution,resolution);
        if (error != noErr || resolution.csDisplayModeID == kDisplayModeIDNoMoreResolutions) break;
        result["modes"].push_back({resolution.csDisplayModeID,resolution.csHorizontalPixels,
            resolution.csVerticalLines,resolution.csRefreshRate});
        if (resolution.csPreviousDisplayModeID == resolution.csDisplayModeID) break;
        resolution.csPreviousDisplayModeID = resolution.csDisplayModeID;
    }
    if (c.value("edges",0u))
    {
        const unsigned wanted = std::clamp(c["edges"].get<unsigned>(),2u,128u);
        result["edges_us"] = json::array(); result["blank_us"] = json::array();
        bool blank = (card.Read(0x005c) & 1) != 0;
        uint64_t previous = 0, entered = 0, start = Microseconds();
        while (result["edges_us"].size() < wanted && Microseconds() - start < 2000000)
        {
            const bool next = (card.Read(0x005c) & 1) != 0;
            if (next == blank) continue;
            const auto at = Microseconds(); blank = next;
            if (blank)
            {
                if (previous) result["edges_us"].push_back(at-previous);
                previous = entered = at;
            }
            else if (entered) result["blank_us"].push_back(at-entered);
        }
    }
    return result.dump();
}
