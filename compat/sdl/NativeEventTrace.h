// Opt-in native attribution of cooperative event-call stalls. No disk writes
// or allocation in the pump; Process Manager CPU counters have tick resolution.
#pragma once
#include <Processes.h>
#include <Disks.h>
#include <AEObjects.h>
#include <AERegistry.h>
#include <nlohmann/json.hpp>

namespace ShimHost::NativeEventTrace
{
    constexpr unsigned maxProcesses = 16, maxStalls = 256;
    struct Process { ProcessSerialNumber psn{}; char name[64]{}; };
    struct Stall
    {
        uint64_t at = 0;
        uint64_t frame = 0;
        uint32_t nativeTick = 0, us = 0, cpu[maxProcesses]{};
        uint16_t method = 0, sleep = 0, event = 0, previous = 0;
        bool available = false, suspended = false;
    };
    struct Method { uint64_t calls = 0, us = 0; uint32_t maximum = 0; };
    inline bool enabled = false, splitSystemTask = false;
    inline unsigned pollMethod = 1; // Diagnostic alternatives; default keeps 0.3.4 behavior.
    inline Process processes[maxProcesses]{};
    inline Stall stalls[maxStalls]{};
    inline Method methods[6]{};
    inline unsigned processCount = 0, stallCount = 0, dropped = 0;
    inline uint64_t epoch = 0, overheadUs = 0, snapshots = 0;
    inline uint16_t previousEvent = 0;
    inline OSErr Object(DescType desired, const AEDesc& container, DescType form, const AEDesc& key, AEDesc& out)
    {
        AERecord record{};
        auto error = AECreateList(nullptr, 0, true, &record);
        if (error == noErr) error = AEPutKeyPtr(&record, keyAEDesiredClass, typeType, &desired, sizeof(desired));
        if (error == noErr) error = AEPutKeyDesc(&record, keyAEContainer, &container);
        if (error == noErr) error = AEPutKeyPtr(&record, keyAEKeyForm, typeEnumerated, &form, sizeof(form));
        if (error == noErr) error = AEPutKeyDesc(&record, keyAEKeyData, &key);
        if (error == noErr) { out = record; out.descriptorType = typeObjectSpecifier; }
        else AEDisposeDesc(&record);
        return error;
    }
    inline void Cpu(uint32_t* ticks)
    {
        const auto start = Microseconds();
        for (unsigned i = 0; i < processCount; ++i)
        {
            ProcessInfoRec info{};
            info.processInfoLength = sizeof(info);
            ticks[i] = GetProcessInformation(&processes[i].psn, &info) == noErr ? info.processActiveTime : UINT32_MAX;
        }
        overheadUs += Microseconds() - start;
        ++snapshots;
    }
    inline void Reset()
    {
        processCount = stallCount = dropped = 0;
        overheadUs = snapshots = previousEvent = 0;
        for (auto& method : methods) method = {};
        ProcessSerialNumber psn{0, kNoProcess};
        while (processCount < maxProcesses && GetNextProcess(&psn) == noErr)
        {
            ProcessInfoRec info{}; Str255 name{};
            info.processInfoLength = sizeof(info); info.processName = name;
            if (GetProcessInformation(&psn, &info) != noErr) continue;
            auto& p = processes[processCount++]; p = {}; p.psn = psn;
            std::memcpy(p.name, name + 1, std::min<unsigned>(name[0], sizeof(p.name) - 1));
        }
        epoch = Microseconds();
    }
    inline bool Call(unsigned method, uint32_t sleep, EventRecord& event, bool suspended)
    {
        uint32_t before[maxProcesses]{};
        if (enabled) Cpu(before);
        const auto start = enabled ? Microseconds() : 0;
        const auto nativeTick = enabled ? TickCount() : 0;
        bool available = false;
        switch (method)
        {
            case 0: available = WaitNextEvent(everyEvent, &event, sleep, nullptr); break;
            case 1: available = EventAvail(everyEvent, &event); break;
            case 2: available = GetNextEvent(everyEvent, &event); break;
            case 3: available = OSEventAvail(everyEvent, &event); break;
            case 4: event = {}; SystemTask(); break;
        }
        if (!enabled) return available;
        const auto us = static_cast<uint32_t>(Microseconds() - start);
        auto& m = methods[method]; ++m.calls; m.us += us; m.maximum = std::max(m.maximum, us);
        if (us >= 20000)
        {
            uint32_t after[maxProcesses]{}; Cpu(after);
            if (stallCount < maxStalls)
            {
                auto& s = stalls[stallCount++]; s = {};
                s.at = start - epoch; s.us = us; s.method = method; s.sleep = sleep;
                s.frame = OpenRCT2::FrameTiming::stats.frames + 1; s.nativeTick = nativeTick;
                s.event = event.what; s.previous = previousEvent; s.available = available; s.suspended = suspended;
                for (unsigned i = 0; i < processCount; ++i)
                    s.cpu[i] = before[i] != UINT32_MAX && after[i] != UINT32_MAX ? after[i] - before[i] : UINT32_MAX;
            }
            else ++dropped;
        }
        if (available && method != 1 && method != 3) previousEvent = event.what;
        return available;
    }
    inline uint32_t yieldBefore[maxProcesses]{};
    inline uint64_t yieldStart = 0;
    inline uint32_t yieldTick = 0;
    inline void YieldObserver(bool before, uint32_t sleep)
    {
        if (!enabled) return;
        if (before) { Cpu(yieldBefore); yieldStart = Microseconds(); yieldTick = TickCount(); return; }
        const auto us = static_cast<uint32_t>(Microseconds() - yieldStart);
        auto& m = methods[5]; ++m.calls; m.us += us; m.maximum = std::max(m.maximum, us);
        if (us < 20000) return;
        uint32_t after[maxProcesses]{}; Cpu(after);
        if (stallCount == maxStalls) { ++dropped; return; }
        auto& s = stalls[stallCount++]; s = {};
        s.at = yieldStart - epoch; s.us = us; s.method = 5; s.sleep = sleep;
        s.frame = OpenRCT2::FrameTiming::stats.frames + 1; s.nativeTick = yieldTick;
        s.previous = previousEvent;
        for (unsigned i = 0; i < processCount; ++i)
            s.cpu[i] = yieldBefore[i] != UINT32_MAX && after[i] != UINT32_MAX ? after[i] - yieldBefore[i] : UINT32_MAX;
    }
    inline std::string Command(const std::string& text)
    {
        using nlohmann::json;
        const auto c = json::parse(text);
        if (c.value("drivers", false))
        {
            json drivers = json::array(), drives = json::array();
            const auto total = std::clamp<int>(LMGetUnitTableEntryCount(), 0, 512);
            for (int i = c.value("first", 0); i < std::min(total, c.value("first", 0) + 12); ++i)
            {
                const short ref = -i - 1;
                const auto handle = GetDCtlEntry(ref);
                if (!handle || !*handle) continue;
                const auto& d = **handle;
                drivers.push_back({{"ref",ref},{"flags",static_cast<uint16_t>(d.dCtlFlags)},
                    {"delay",d.dCtlDelay},{"tick",d.dCtlCurTicks}});
            }
            if (c.value("drive_status", false))
            {
                auto entry = reinterpret_cast<DrvQElPtr>(GetDrvQHdr()->qHead);
                for (unsigned i = 0; entry && i < 16; ++i, entry = reinterpret_cast<DrvQElPtr>(entry->qLink))
                {
                    CntrlParam block{}; block.ioCRefNum = entry->dQRefNum; block.ioVRefNum = entry->dQDrive; block.csCode = 8;
                    const auto start = Microseconds();
                    const auto error = PBStatusSync(reinterpret_cast<ParmBlkPtr>(&block));
                    const auto us = Microseconds() - start;
                    DrvSts status{}; std::memcpy(&status, block.csParam, sizeof(status));
                    CntrlParam gestalt{}; gestalt.ioCRefNum = entry->dQRefNum; gestalt.csCode = 43;
                    const OSType selector = 'devt'; std::memcpy(gestalt.csParam,&selector,4);
                    const auto ge = PBStatusSync(reinterpret_cast<ParmBlkPtr>(&gestalt));
                    uint32_t type = 0; if (ge == noErr) std::memcpy(&type,&gestalt.csParam[2],4);
                    drives.push_back({{"drive",entry->dQDrive},{"ref",entry->dQRefNum},{"error",error},
                        {"us",us},{"media",status.diskInPlace},{"gestalt_error",ge},{"type",type}});
                }
            }
            return json{{"tick",TickCount()},{"total",total},{"drivers",drivers},{"drives",drives}}.dump();
        }
        if (c.value("finder_windows", false))
        {
            // Read-only Finder object-model query, outside any frame capture.
            const OSType signature = 'MACS', nameProperty = 'pnam';
            const DescType windowClass = cWindow;
            const OSType all = kAEAll;
            AEAddressDesc target{}; AppleEvent request{}, reply{};
            AEDesc root{typeNull, nullptr}, ordinal{}, windows{}, property{}, names{}, answer{};
            OSErr error = AECreateDesc(typeApplSignature, &signature, sizeof(signature), &target);
            if (error == noErr) error = AECreateDesc(typeAbsoluteOrdinal, &all, sizeof(all), &ordinal);
            if (error == noErr) error = Object(windowClass, root, formAbsolutePosition, ordinal, windows);
            if (error == noErr) error = AECreateDesc(typeType, &nameProperty, sizeof(nameProperty), &property);
            if (error == noErr) error = Object(cProperty, windows, formPropertyID, property, names);
            if (error == noErr) error = AECreateAppleEvent(kAECoreSuite, kAEGetData, &target,
                kAutoGenerateReturnID, kAnyTransactionID, &request);
            if (error == noErr) error = AEPutParamDesc(&request, keyDirectObject, &names);
            if (error == noErr) error = AESend(&request, &reply, kAEWaitReply | kAENeverInteract,
                kAENormalPriority, 120, nullptr, nullptr);
            if (error == noErr)
            {
                long finderError = 0; DescType type = typeNull; Size size = 0;
                if (AEGetParamPtr(&reply,keyErrorNumber,typeLongInteger,&type,&finderError,sizeof(finderError),&size) == noErr)
                    error = static_cast<OSErr>(finderError);
            }
            if (error == noErr) error = AEGetParamDesc(&reply, keyDirectObject, typeAEList, &answer);
            json result = {{"error", error}, {"names_hex", json::array()}};
            long count = 0;
            if (error == noErr) error = AECountItems(&answer, &count);
            for (long i = 1; error == noErr && i <= std::min(count, 32L); ++i)
            {
                char bytes[512]; DescType type = typeNull; AEKeyword key = 0; Size size = 0;
                error = AEGetNthPtr(&answer, i, typeChar, &key, &type, bytes, sizeof(bytes), &size);
                if (error == noErr)
                {
                    std::string hex;
                    for (Size j = 0; j < size; ++j)
                    { const auto byte = static_cast<uint8_t>(bytes[j]); hex += "0123456789abcdef"[byte >> 4]; hex += "0123456789abcdef"[byte & 15]; }
                    result["names_hex"].push_back(hex);
                }
            }
            result["error"] = error; result["count"] = count;
            AEDisposeDesc(&answer); AEDisposeDesc(&names); AEDisposeDesc(&property); AEDisposeDesc(&windows);
            AEDisposeDesc(&ordinal); AEDisposeDesc(&reply); AEDisposeDesc(&request); AEDisposeDesc(&target);
            return result.dump();
        }
        if (c.contains("quit_test_app"))
        {
            // Restore only applications explicitly opened for an A/B
            // test. AgentChat and system/background services are never targets.
            const auto psn = ProcessSerialNumber{c.value("high",0u),c.at("low").get<uint32_t>()};
            ProcessInfoRec info{}; info.processInfoLength = sizeof(info);
            auto error = GetProcessInformation(&psn, &info);
            if (error == noErr && info.processSignature != 'USBP' && info.processSignature != 'notz'
                && info.processSignature != 'ogle') error = paramErr;
            AEAddressDesc target{}; AppleEvent event{}, reply{};
            if (error == noErr) error = AECreateDesc(typeProcessSerialNumber, &psn, sizeof(psn), &target);
            if (error == noErr) error = AECreateAppleEvent(kCoreEventClass,kAEQuitApplication,&target,
                kAutoGenerateReturnID,kAnyTransactionID,&event);
            if (error == noErr) error = AESend(&event,&reply,kAENoReply | kAENeverInteract,kAENormalPriority,1,nullptr,nullptr);
            AEDisposeDesc(&reply); AEDisposeDesc(&event); AEDisposeDesc(&target);
            return json{{"error",error}}.dump();
        }
        if (c.value("reset", false)) Reset();
        if (c.contains("on")) enabled = c["on"].get<bool>();
        if (c.contains("split_system_task")) splitSystemTask = c["split_system_task"].get<bool>();
        if (c.contains("poll_method")) pollMethod = std::clamp(c["poll_method"].get<unsigned>(), 1u, 3u);
        json result = {{"on",enabled},{"count",stallCount},{"dropped",dropped},
            {"overhead_us",overheadUs},{"cpu_snapshots",snapshots},{"mask",everyEvent},{"mouse_region","null"},
            {"methods",json::array()},{"processes",json::array()},{"stalls",json::array()}};
        result["split_system_task"] = splitSystemTask;
        result["poll_method"] = pollMethod;
        const char* names[] = {"WaitNextEvent","EventAvail","GetNextEvent","OSEventAvail","SystemTask","ClassicHostYield(mask0)"};
        for (unsigned i = 0; i < 6; ++i)
            result["methods"].push_back({names[i],methods[i].calls,methods[i].us,methods[i].maximum});
        for (unsigned i = 0; i < processCount; ++i) result["processes"].push_back(processes[i].name);
        const auto first = std::min(c.value("first",0u),stallCount);
        const auto end = std::min(stallCount,first + std::clamp(c.value("limit",2u),1u,2u));
        for (auto i = first; i < end; ++i)
        {
            const auto& s = stalls[i]; json cpu = json::array();
            for (unsigned j = 0; j < processCount; ++j) cpu.push_back(s.cpu[j]);
            result["stalls"].push_back({{"at_us",s.at},{"frame",s.frame},{"native_tick",s.nativeTick},{"us",s.us},{"method",names[s.method]},
                {"sleep",s.sleep},{"event",s.event},{"previous",s.previous},{"available",s.available},
                {"suspended",s.suspended},{"cpu_ticks",cpu}});
        }
        return result.dump();
    }
}
