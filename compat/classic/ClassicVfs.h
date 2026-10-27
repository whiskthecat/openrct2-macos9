// A small virtual file system for the Mac OS 9 build.
//
// The engine sees POSIX-style paths under /RCT. Read-only data lives in pack
// files (one big file each, because Classic file names are limited to 31
// characters and thousands of small files are slow to install):
//   /RCT/openrct2  the OpenRCT2 data directory   (openrct2.pak)
//   /RCT/game      the original game files       (game.pak)
//   /RCT/rct1      optional RCT1 files           (rct1.pak)
// and /RCT/user is a real, writable folder beside the application.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ClassicVfs
{
    struct Info
    {
        bool exists = false;
        bool isDirectory = false;
        uint64_t size = 0;
        uint64_t modified = 0;
    };
    struct Entry
    {
        std::string name;
        bool isDirectory = false;
        uint64_t size = 0;
        uint64_t modified = 0;
    };
    struct Handle;

    // Mounts the packs and the user folder found beside the application.
    // Safe to call more than once.
    void Initialise();
    const std::string& Problems();
    bool HasRCT1();
    std::vector<std::string> PackPaths(std::string_view extension);
    void PreloadTrackDesigns();
    uint64_t TrackDesignCacheBytes();
    uint64_t TrackDesignBytes();
    void SetPreloadEnabled(bool enabled);
    // Immutable pack data directly into its final destination; no window allocation/copy.
    uint64_t ReadPack(std::string_view path, uint64_t offset, void* buffer, uint64_t length);

    Info Query(std::string_view path);
    std::vector<Entry> List(std::string_view directory);
    // Mounted packs have fixed entries/metadata for the process lifetime.
    // Writable /RCT/user directories must always be enumerated afresh.
    bool IsPackedPath(std::string_view path);
    extern bool gCachePackCensus; // diagnostic override, not persisted

    Handle* Open(std::string_view path);
    void Close(Handle* handle);
    uint64_t Size(const Handle* handle);
    uint64_t Position(const Handle* handle);
    void Seek(Handle* handle, int64_t offset, int origin);
    uint64_t Read(Handle* handle, void* buffer, uint64_t length);

    // Native path for a file under /RCT/user, or empty if the path is elsewhere.
    std::string HostPath(std::string_view path);
    bool MakeDirectories(std::string_view path);
    bool Remove(std::string_view path);
    bool Rename(std::string_view from, std::string_view to);
    // Publish a finished file atomically; replace permits manual-save replacement.
    bool Publish(std::string_view from, std::string_view to, bool replace = false);
} // namespace ClassicVfs

// Operating-system backend (Toolbox on the Mac, POSIX for host testing).
namespace ClassicHost
{
    struct MemoryInfo
    {
        uint64_t physical = 0, logical = 0, partition = 0, free = 0, largest = 0;
    };
    MemoryInfo Memory();
    extern const char kSeparator;
    std::string ApplicationDirectory(); // ends with a separator
    ClassicVfs::Info Query(const std::string& hostPath);
    std::vector<ClassicVfs::Entry> List(const std::string& hostDirectory);
    bool MakeDirectory(const std::string& hostPath);
    bool Remove(const std::string& hostPath);
    // Housekeeping by remote control: moves a file or folder into another folder on the same
    // volume. Returns the Mac OS error code (0 = done).
    int MoveInto(const std::string& hostPath, const std::string& hostFolder);
    bool PublishFile(const std::string& from, const std::string& to, bool replace = false);
    bool SyncFile(std::FILE* file, const std::string& hostPath);
    bool SyncDirectory(const std::string& hostPath);
    struct Flush
    {
        virtual ~Flush() = default;
        virtual bool Ready() const = 0;
        virtual bool Good() const = 0;
    };
    // Flush the containing volume on Classic; null file means directory metadata.
    std::unique_ptr<Flush> BeginFlush(std::FILE* file, const std::string& hostPath);
    uint64_t Microseconds();
    void Yield(uint32_t sleepTicks = 1);
#ifdef OPENRCT2_CLASSIC_MAC
    // Optional diagnostics, null in normal play; the platform library owns no UI dependency.
    extern void (*gYieldObserver)(bool before, uint32_t sleepTicks);
#endif
    extern bool gCoarseFrameWait; // diagnostic A/B control; never persisted
} // namespace ClassicHost
