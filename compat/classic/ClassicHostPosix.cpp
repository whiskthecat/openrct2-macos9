// POSIX backend for the virtual file system, used to test the pack-file
// build on Linux. The "application directory" comes from RCT_PAK_DIR.
#ifndef OPENRCT2_CLASSIC_MAC
    #include "ClassicVfs.h"

    #include <cstdio>
#include <chrono>
    #include <cstdlib>
    #include <dirent.h>
    #include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace ClassicHost
{
    MemoryInfo Memory()
    {
        // Explicit controls for repeatable admission tests. Native uses Toolbox values.
        const auto read = [](const char* name, uint64_t fallback) {
            const char* value = std::getenv(name);
            return value ? std::strtoull(value, nullptr, 10) * 1024 * 1024 : fallback;
        };
        MemoryInfo result;
        result.physical = read("RCT_MEMORY_MB", 1024ull * 1024 * 1024);
        result.logical = result.physical;
        result.partition = read("RCT_PARTITION_MB", 256ull * 1024 * 1024);
        result.free = read("RCT_FREE_MB", 144ull * 1024 * 1024);
        result.largest = read("RCT_LARGEST_MB", result.free);
        return result;
    }
    const char kSeparator = '/';

    std::string ApplicationDirectory()
    {
        const char* directory = std::getenv("RCT_PAK_DIR");
        std::string result = directory != nullptr ? directory : ".";
        if (result.back() != '/')
            result.push_back('/');
        return result;
    }

    ClassicVfs::Info Query(const std::string& hostPath)
    {
        ClassicVfs::Info info;
        struct stat status{};
        if (stat(hostPath.c_str(), &status) == 0)
        {
            info.exists = true;
            info.isDirectory = S_ISDIR(status.st_mode);
            info.size = static_cast<uint64_t>(status.st_size);
            info.modified = static_cast<uint64_t>(status.st_mtime);
        }
        return info;
    }

    std::vector<ClassicVfs::Entry> List(const std::string& hostDirectory)
    {
        std::vector<ClassicVfs::Entry> result;
        if (DIR* directory = opendir(hostDirectory.c_str()))
        {
            while (const auto* node = readdir(directory))
            {
                const std::string name = node->d_name;
                if (name == "." || name == "..")
                    continue;
                const auto info = Query(hostDirectory + "/" + name);
                result.push_back({ name, info.isDirectory, info.size, info.modified });
            }
            closedir(directory);
        }
        return result;
    }

    bool Remove(const std::string& hostPath)
    {
        return std::remove(hostPath.c_str()) == 0;
    }

    int MoveInto(const std::string&, const std::string&)
    {
        return -1;
    }

    bool MakeDirectory(const std::string& hostPath)
    {
        return mkdir(hostPath.c_str(), 0755) == 0 || Query(hostPath).isDirectory;
    }

    bool PublishFile(const std::string& from, const std::string& to, bool replace)
    {
        if (replace) return ::rename(from.c_str(), to.c_str()) == 0;
        // link is atomic and rejects existing names, including racing publishers.
        // A crash between link/unlink leaves two complete files, never a partial park.
        return ::link(from.c_str(), to.c_str()) == 0 && ::unlink(from.c_str()) == 0;
    }
    bool SyncFile(std::FILE* file, const std::string&) { return ::fsync(::fileno(file)) == 0; }
    bool SyncDirectory(const std::string& hostPath)
    {
        const auto parent = hostPath.substr(0, hostPath.find_last_of('/'));
        const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) return false;
        const bool ok = ::fsync(fd) == 0;
        return ::close(fd) == 0 && ok;
    }

    namespace
    {
        struct PosixFlush final : Flush
        {
            bool good;
            explicit PosixFlush(bool value) : good(value) {}
            bool Ready() const override { return true; }
            bool Good() const override { return good; }
        };
    }
    std::unique_ptr<Flush> BeginFlush(std::FILE* file, const std::string& path)
    {
        return std::make_unique<PosixFlush>(file ? SyncFile(file, path) : SyncDirectory(path));
    }

    uint64_t Microseconds()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    bool gCoarseFrameWait = false;
    void Yield(uint32_t)
    {
    }
} // namespace ClassicHost
#endif
