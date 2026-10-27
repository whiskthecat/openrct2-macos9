// Toolbox backend for the virtual file system on Mac OS 9.
#ifdef OPENRCT2_CLASSIC_MAC
    #include "ClassicVfs.h"

    #include <openrct2/core/FrameTiming.h>
    #include <algorithm>
    #include <Events.h>
    #include <Files.h>
    #include <Processes.h>
    #include <Gestalt.h>
    #include <MacMemory.h>
    #include <Script.h>
    #include <Timer.h>
    #include <cstring>

namespace ClassicHost
{
    const char kSeparator = ':';

    MemoryInfo Memory()
    {
        MemoryInfo result;
        long value = 0;
        if (Gestalt(gestaltPhysicalRAMSize, &value) == noErr) result.physical = static_cast<uint32_t>(value);
        if (Gestalt(gestaltLogicalRAMSize, &value) == noErr) result.logical = static_cast<uint32_t>(value);
        ProcessSerialNumber process{0, kCurrentProcess};
        ProcessInfoRec info{}; info.processInfoLength = sizeof(info);
        if (GetProcessInformation(&process, &info) == noErr) result.partition = info.processSize;
        result.free = static_cast<uint32_t>(FreeMem());
        result.largest = static_cast<uint32_t>(MaxBlock());
        return result;
    }

    namespace
    {
        // Seconds between the Mac epoch (1904) and the Unix epoch (1970).
        constexpr uint32_t kMacEpochOffset = 2082844800u;

        bool MakeSpec(const std::string& hostPath, FSSpec& spec)
        {
            if (hostPath.empty() || hostPath.size() > 255)
                return false;
            Str255 name;
            name[0] = static_cast<unsigned char>(hostPath.size());
            std::memcpy(name + 1, hostPath.data(), hostPath.size());
            const OSErr error = FSMakeFSSpec(0, 0, name, &spec);
            return error == noErr || error == fnfErr;
        }
        bool CatalogInfo(const FSSpec& spec, CInfoPBRec& block)
        {
            Str255 name;
            std::memcpy(name, spec.name, spec.name[0] + 1);
            std::memset(&block, 0, sizeof(block));
            block.hFileInfo.ioNamePtr = name;
            block.hFileInfo.ioVRefNum = spec.vRefNum;
            block.hFileInfo.ioDirID = spec.parID;
            block.hFileInfo.ioFDirIndex = 0;
            return PBGetCatInfoSync(&block) == noErr;
        }
    } // namespace

    std::string ApplicationDirectory()
    {
        ProcessSerialNumber process{ 0, kCurrentProcess };
        ProcessInfoRec info{};
        FSSpec application{};
        info.processInfoLength = sizeof(info);
        info.processAppSpec = &application;
        if (GetProcessInformation(&process, &info) != noErr)
            return ":";
        // Walk up the parent chain to build "Volume:Folder:...:".
        std::string path;
        long directory = application.parID;
        while (true)
        {
            Str255 name{};
            CInfoPBRec block{};
            block.dirInfo.ioNamePtr = name;
            block.dirInfo.ioVRefNum = application.vRefNum;
            block.dirInfo.ioDrDirID = directory;
            block.dirInfo.ioFDirIndex = -1;
            if (PBGetCatInfoSync(&block) != noErr)
                break;
            path = std::string(reinterpret_cast<const char*>(name + 1), name[0]) + ":" + path;
            if (directory == fsRtDirID)
                break;
            directory = block.dirInfo.ioDrParID;
        }
        return path;
    }

    ClassicVfs::Info Query(const std::string& hostPath)
    {
        ClassicVfs::Info info;
        FSSpec spec{};
        CInfoPBRec block{};
        if (!MakeSpec(hostPath, spec) || !CatalogInfo(spec, block))
            return info;
        info.exists = true;
        info.isDirectory = (block.hFileInfo.ioFlAttrib & kioFlAttribDirMask) != 0;
        if (!info.isDirectory)
        {
            info.size = static_cast<uint64_t>(block.hFileInfo.ioFlLgLen);
            info.modified = static_cast<uint64_t>(block.hFileInfo.ioFlMdDat) - kMacEpochOffset;
        }
        return info;
    }

    std::vector<ClassicVfs::Entry> List(const std::string& hostDirectory)
    {
        std::vector<ClassicVfs::Entry> result;
        FSSpec spec{};
        CInfoPBRec block{};
        if (!MakeSpec(hostDirectory, spec) || !CatalogInfo(spec, block)
            || (block.hFileInfo.ioFlAttrib & kioFlAttribDirMask) == 0)
            return result;
        const long directory = block.dirInfo.ioDrDirID;
        for (short index = 1;; index++)
        {
            Str255 name{};
            CInfoPBRec child{};
            child.hFileInfo.ioNamePtr = name;
            child.hFileInfo.ioVRefNum = spec.vRefNum;
            child.hFileInfo.ioDirID = directory;
            child.hFileInfo.ioFDirIndex = index;
            if (PBGetCatInfoSync(&child) != noErr)
                break;
            ClassicVfs::Entry entry;
            entry.name.assign(reinterpret_cast<const char*>(name + 1), name[0]);
            entry.isDirectory = (child.hFileInfo.ioFlAttrib & kioFlAttribDirMask) != 0;
            if (!entry.isDirectory)
            {
                entry.size = static_cast<uint64_t>(child.hFileInfo.ioFlLgLen);
                entry.modified = static_cast<uint64_t>(child.hFileInfo.ioFlMdDat) - kMacEpochOffset;
            }
            result.push_back(std::move(entry));
        }
        return result;
    }

    bool Remove(const std::string& hostPath)
    {
        // (The C library's remove() does nothing here.)
        FSSpec spec{};
        return MakeSpec(hostPath, spec) && FSpDelete(&spec) == noErr;
    }

    int MoveInto(const std::string& hostPath, const std::string& hostFolder)
    {
        FSSpec item{}, folder{};
        CInfoPBRec block{};
        if (!MakeSpec(hostPath, item) || !MakeSpec(hostFolder, folder))
            return bdNamErr;
        if (!CatalogInfo(folder, block) || (block.hFileInfo.ioFlAttrib & kioFlAttribDirMask) == 0)
            return dirNFErr;
        if (item.vRefNum != folder.vRefNum)
            return diffVolErr;
        return CatMove(item.vRefNum, item.parID, item.name, block.dirInfo.ioDrDirID, nullptr);
    }

    bool MakeDirectory(const std::string& hostPath)
    {
        FSSpec spec{};
        if (!MakeSpec(hostPath, spec))
            return false;
        long created = 0;
        const OSErr error = FSpDirCreate(&spec, smSystemScript, &created);
        return error == noErr || error == dupFNErr;
    }

    bool PublishFile(const std::string& from, const std::string& to, bool replace)
    {
        FSSpec source{}, target{};
        if (!MakeSpec(from, source) || !MakeSpec(to, target)) return false;
        if (source.vRefNum != target.vRefNum || source.parID != target.parID) return false;
        if (replace && Query(to).exists)
            return FSpExchangeFiles(&source, &target) == noErr;
        // FSpRename rejects duplicate names and preserves forks/metadata. Never
        // use the old Rename wrapper, which deletes its destination first.
        return FSpRename(&source, target.name) == noErr;
    }
    bool SyncFile(std::FILE*, const std::string& hostPath)
    {
        FSSpec spec{};
        return MakeSpec(hostPath, spec) && FlushVol(nullptr, spec.vRefNum) == noErr;
    }
    bool SyncDirectory(const std::string& hostPath) { return SyncFile(nullptr, hostPath); }

    namespace
    {
        struct MacFlush final : Flush
        {
            ParamBlockRec block{};
            explicit MacFlush(const std::string& path)
            {
                FSSpec spec{};
                if (!MakeSpec(path, spec)) { block.ioParam.ioResult = fnfErr; return; }
                block.ioParam.ioVRefNum = spec.vRefNum;
                block.ioParam.ioResult = 1;
                // No completion routine: the File Manager updates ioResult.
                // This owned parameter block survives every intervening frame.
                const auto error = PBFlushVolAsync(&block);
                if (error != noErr) block.ioParam.ioResult = error;
            }
            OSErr Result() const { return *reinterpret_cast<const volatile OSErr*>(&block.ioParam.ioResult); }
            bool Ready() const override { return Result() <= 0; }
            bool Good() const override { return Result() == noErr; }
            ~MacFlush() override { while (!Ready()) ClassicHost::Yield(0); }
        };
    }
    std::unique_ptr<Flush> BeginFlush(std::FILE*, const std::string& path)
    {
        return std::make_unique<MacFlush>(path);
    }

    uint64_t Microseconds()
    {
        UnsignedWide value;
        ::Microseconds(&value);
        return (static_cast<uint64_t>(value.hi) << 32) | value.lo;
    }

    bool gCoarseFrameWait = false;
    void (*gYieldObserver)(bool, uint32_t) = nullptr;
    void Yield(uint32_t sleepTicks)
    {
        EventRecord event;
        if (gYieldObserver) gYieldObserver(true, sleepTicks);
        const auto start = Microseconds();
        WaitNextEvent(0, &event, sleepTicks, nullptr);
        auto& frame = OpenRCT2::FrameTiming::current;
        const auto us = static_cast<uint32_t>(Microseconds() - start);
        frame.yieldCalls++; frame.yieldUs += us; frame.yieldMaxUs = std::max(frame.yieldMaxUs, us);
        frame.coarseYields += sleepTicks != 0;
        if (gYieldObserver) gYieldObserver(false, sleepTicks);
    }
} // namespace ClassicHost

ClassicClock::time_point ClassicClock::now() noexcept
{
    return time_point(duration(static_cast<rep>(ClassicHost::Microseconds())));
}
#endif
