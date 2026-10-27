#include "ClassicDesktop.h"
#include "ClassicVersion.h"
#include "ClassicVfs.h"
#include "../BuildIdentity.h"

#include <Aliases.h>
#include <Files.h>
#include <Finder.h>
#include <Folders.h>
#include <Icons.h>
#include <MacMemory.h>
#include <Processes.h>
#include <Resources.h>
#include <Script.h>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>

namespace
{
    constexpr OSType kCreator = 'ORCG';

    std::string Path(const FSSpec& spec)
    {
        std::string path(reinterpret_cast<const char*>(spec.name + 1), spec.name[0]);
        long directory = spec.parID;
        while (directory != fsRtParID)
        {
            Str255 name;
            CInfoPBRec pb{};
            pb.dirInfo.ioNamePtr = name;
            pb.dirInfo.ioVRefNum = spec.vRefNum;
            pb.dirInfo.ioFDirIndex = -1;
            pb.dirInfo.ioDrDirID = directory;
            if (PBGetCatInfoSync(&pb) != noErr)
                return {};
            path = std::string(reinterpret_cast<const char*>(name + 1), name[0]) + ":" + path;
            directory = pb.dirInfo.ioDrParID;
        }
        return path;
    }

    OSErr Register(const FSSpec& app)
    {
        FInfo info;
        OSErr error = FSpGetFInfo(&app, &info);
        if (error != noErr)
            return error;
        info.fdType = 'APPL';
        info.fdCreator = kCreator;
        info.fdFlags = (info.fdFlags | kHasBundle) & ~kHasBeenInited;
        if ((error = FSpSetFInfo(&app, &info)) != noErr)
            return error;
        DTPBRec pb{};
        pb.ioVRefNum = app.vRefNum;
        if ((error = PBDTGetPath(&pb)) != noErr)
            return error;
        const OSType resources[] = { 'ICN#', 'icl8', 'ics#', 'ics8' };
        const UInt8 types[] = { kLargeIcon, kLarge8BitIcon, kSmallIcon, kSmall8BitIcon };
        for (int i = 0; i < 4 && error == noErr; ++i)
        {
            Handle icon = Get1Resource(resources[i], 128);
            if (!icon)
            {
                error = resNotFound;
                break;
            }
            HLock(icon);
            pb.ioDTBuffer = *icon;
            pb.ioDTReqCount = GetHandleSize(icon);
            pb.ioIconType = types[i];
            pb.ioFileCreator = kCreator;
            pb.ioFileType = 'APPL';
            pb.ioTagInfo = 0;
            error = PBDTAddIconSync(&pb);
            HUnlock(icon);
        }
        if (error == noErr)
        {
            pb.ioNamePtr = const_cast<StringPtr>(app.name);
            pb.ioFileCreator = kCreator;
            pb.ioDirID = app.parID;
            pb.ioTagInfo = 0;
            error = PBDTAddAPPLSync(&pb);
        }
        const auto flush = PBDTFlushSync(&pb);
        const auto close = PBDTCloseDown(&pb);
        if (error == noErr)
            error = flush != noErr ? flush : close;
        IconRef icon = nullptr;
        (void)FlushIconRefs(kCreator, 'APPL');
        if (GetIconRef(app.vRefNum, kCreator, 'APPL', &icon) == noErr)
        {
            (void)UpdateIconRef(icon);
            (void)ReleaseIconRef(icon);
        }
        return error;
    }

    OSErr Alias(const FSSpec& app, FSSpec& alias, FSSpec& resolved)
    {
        short volume;
        long directory;
        OSErr error = FindFolder(app.vRefNum, kDesktopFolderType, kCreateFolder, &volume, &directory);
        if (error != noErr)
            return error;
        // Keep the public desktop name stable across versions.
        Str31 aliasName{};
        const char* label = "RollerCoaster Tycoon";
        aliasName[0] = std::strlen(label);
        std::memcpy(aliasName + 1, label, aliasName[0]);
        error = FSMakeFSSpec(volume, directory, aliasName, &alias);
        if (error == noErr)
        {
            FInfo old;
            if ((error = FSpGetFInfo(&alias, &old)) != noErr)
                return error;
            // Do not replace an unrelated desktop item with the same name.
            if (!(old.fdFlags & kIsAlias) || old.fdCreator != kCreator)
                return dupFNErr;
        }
        else if (error == fnfErr)
        {
            FSpCreateResFile(&alias, kCreator, 'adrp', smSystemScript);
            if ((error = ResError()) != noErr)
                return error;
        }
        else
            return error;
        AliasHandle target = nullptr;
        if ((error = NewAlias(&alias, &app, &target)) != noErr)
            return error;
        const short previous = CurResFile();
        const short fork = FSpOpenResFile(&alias, fsRdWrPerm);
        if (fork == -1)
        {
            DisposeHandle(reinterpret_cast<Handle>(target));
            return ResError();
        }
        Handle old = Get1Resource('alis', 0);
        if (old)
        {
            RemoveResource(old);
            error = ResError();
            DisposeHandle(old);
        }
        if (error == noErr)
        {
            const unsigned char emptyName[] = { 0 };
            AddResource(reinterpret_cast<Handle>(target), 'alis', 0, emptyName);
            error = ResError();
            if (error == noErr)
            {
                WriteResource(reinterpret_cast<Handle>(target));
                error = ResError();
                UpdateResFile(fork);
                if (error == noErr)
                    error = ResError();
            }
            else
                DisposeHandle(reinterpret_cast<Handle>(target));
        }
        else
            DisposeHandle(reinterpret_cast<Handle>(target));
        CloseResFile(fork);
        UseResFile(previous);
        if (error != noErr)
            return error;
        FInfo info;
        if ((error = FSpGetFInfo(&alias, &info)) != noErr)
            return error;
        info.fdType = 'adrp';
        info.fdCreator = kCreator;
        info.fdFlags = (info.fdFlags | kIsAlias) & ~(kHasBeenInited | kHasBundle);
        if ((error = FSpSetFInfo(&alias, &info)) != noErr)
            return error;
        resolved = alias;
        Boolean folder = false, wasAlias = false;
        error = ResolveAliasFileWithMountFlags(&resolved, true, &folder, &wasAlias, kResolveAliasFileNoUI);
        if (error == noErr && (!wasAlias || folder || Path(resolved) != Path(app)))
            error = paramErr;
        return error;
    }
}

int ClassicDesktop::Install()
{
    ProcessSerialNumber process;
    ProcessInfoRec info{};
    FSSpec app{}, alias{}, resolved{};
    OSErr error = GetCurrentProcess(&process);
    info.processInfoLength = sizeof(info);
    info.processAppSpec = &app;
    if (error == noErr)
        error = GetProcessInformation(&process, &info);
    if (error == noErr)
        error = Register(app);
    if (error == noErr)
        error = Alias(app, alias, resolved);
    nlohmann::json volumes = nlohmann::json::array();
    for (short index = 1;; ++index)
    {
        Str255 name;
        XVolumeParam volume{};
        volume.ioNamePtr = name;
        volume.ioVolIndex = index;
        const auto result = PBXGetVolInfoSync(&volume);
        if (result == nsvErr)
            break;
        if (result != noErr)
        {
            if (error == noErr)
                error = result;
            break;
        }
        volumes.push_back({ { "name", std::string(reinterpret_cast<const char*>(name + 1), name[0]) },
            { "total_bytes", static_cast<uint64_t>(volume.ioVTotalBytes) },
            { "free_bytes", static_cast<uint64_t>(volume.ioVFreeBytes) } });
    }
    const nlohmann::json report = {
        { "ok", error == noErr }, { "error", error }, { "version", RCT_PORT_VERSION },
        { "source", RctBuild::Source() + 14 }, { "build", RctBuild::Id() + 13 },
        { "application", Path(app) }, { "alias", Path(alias) }, { "resolved", Path(resolved) },
        { "volumes", volumes }
    };
    const auto text = report.dump() + "\n";
    FILE* file = std::fopen((ClassicHost::ApplicationDirectory() + "RCT.desktop.json").c_str(), "wb");
    if (!file)
        return 1;
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool closed = std::fclose(file) == 0;
    return error == noErr && written && closed ? 0 : 1;
}
