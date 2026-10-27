#include "ClassicVfs.h"

#include "ClassicDiskIo.h"
#include "ClassicLoadProgress.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace ClassicVfs
{
    namespace
    {
        constexpr std::string_view kRoot = "/RCT";
        constexpr std::string_view kUserRoot = "/RCT/user";

        struct PackEntry
        {
            std::string name; // original spelling of the last component
            uint32_t offset = 0;
            uint32_t size = 0;
            bool isDirectory = false;
            mutable std::vector<uint8_t> resident;
        };
        struct Pack
        {
            std::string mount; // lower-case virtual directory, no trailing slash
            std::FILE* file = nullptr;
            std::map<std::string, PackEntry> entries; // lower-case path relative to mount
        };
        std::vector<Pack> gPacks;
        std::mutex gPackReadLock;
        std::string gUserHostRoot;
        std::string gProblems;
        bool gInitialised = false;
        bool gPreloadEnabled = true, gTracksPreloaded = false;
        uint64_t gTrackBytes = 0;

        std::string Normalise(std::string_view path)
        {
            // Lower case, forward slashes, no duplicate or trailing slash.
            std::string out;
            out.reserve(path.size());
            for (char c : path)
            {
                if (c == '\\')
                    c = '/';
                if (c == '/' && !out.empty() && out.back() == '/')
                    continue;
                out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c);
            }
            while (out.size() > 1 && out.back() == '/')
                out.pop_back();
            return out;
        }
        bool Under(const std::string& path, std::string_view root)
        {
            return path.size() >= root.size() && path.compare(0, root.size(), root) == 0
                && (path.size() == root.size() || path[root.size()] == '/');
        }
        uint32_t Read32(std::FILE* file)
        {
            unsigned char b[4]{};
            if (std::fread(b, 1, 4, file) != 4)
                return 0;
            return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
        }
        void MountPack(const std::string& mount, const std::string& hostPath)
        {
            std::FILE* file = std::fopen(hostPath.c_str(), "rb");
            if (file == nullptr)
            {
                gProblems += "Missing pack: " + hostPath + "\n";
                return;
            }
            char magic[8]{};
            if (std::fread(magic, 1, 8, file) != 8 || std::memcmp(magic, "RCTPAK1", 8) != 0)
            {
                gProblems += "Not a pack file: " + hostPath + "\n";
                std::fclose(file);
                return;
            }
            Pack pack;
            pack.mount = mount;
            pack.file = file;
            const uint32_t count = Read32(file);
            for (uint32_t i = 0; i < count; i++)
            {
                PackEntry entry;
                entry.offset = Read32(file);
                entry.size = Read32(file);
                const uint32_t nameLength = Read32(file);
                std::string path(nameLength, '\0');
                if (nameLength > 1024 || std::fread(path.data(), 1, nameLength, file) != nameLength)
                {
                    gProblems += "Damaged pack index: " + hostPath + "\n";
                    break;
                }
                // Register every parent directory as well.
                for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
                {
                    const auto parent = path.substr(0, slash);
                    const auto key = Normalise(parent);
                    if (pack.entries.find(key) == pack.entries.end())
                    {
                        PackEntry directory;
                        const auto last = parent.rfind('/');
                        directory.name = last == std::string::npos ? parent : parent.substr(last + 1);
                        directory.isDirectory = true;
                        pack.entries.emplace(key, std::move(directory));
                    }
                }
                const auto last = path.rfind('/');
                entry.name = last == std::string::npos ? path : path.substr(last + 1);
                pack.entries[Normalise(path)] = std::move(entry);
            }
            // From here on the pack is read a window (256 KB) at a time: straight from the file
            // system, not through the C library's 1 KB buffer (hundreds of slow calls per window).
            std::setvbuf(file, nullptr, _IONBF, 0);
            gPacks.push_back(std::move(pack));
        }
        // Several packs can share a mount point (large data is split so each
        // part stays under the installer's per-file limit). Returns whether
        // any pack is mounted over `path` and sets the key inside the mount.
        bool MountedKey(const std::string& path, std::string& key, std::string& mount)
        {
            for (const auto& pack : gPacks)
            {
                if (Under(path, pack.mount))
                {
                    mount = pack.mount;
                    key = path.size() == pack.mount.size() ? std::string() : path.substr(pack.mount.size() + 1);
                    return true;
                }
            }
            return false;
        }
        const PackEntry* FindEntry(const std::string& mount, const std::string& key, const Pack** owner = nullptr)
        {
            for (const auto& pack : gPacks)
            {
                if (pack.mount != mount)
                    continue;
                const auto found = pack.entries.find(key);
                if (found != pack.entries.end())
                {
                    if (owner != nullptr)
                        *owner = &pack;
                    return &found->second;
                }
            }
            return nullptr;
        }
        std::string UserHostPath(const std::string& normalised, std::string_view original)
        {
            // Keep the caller's spelling for the part below the user root.
            std::string tail;
            {
                std::string cleaned;
                for (char c : original)
                {
                    if (c == '\\')
                        c = '/';
                    if (c == '/' && !cleaned.empty() && cleaned.back() == '/')
                        continue;
                    cleaned.push_back(c);
                }
                while (cleaned.size() > 1 && cleaned.back() == '/')
                    cleaned.pop_back();
                tail = cleaned.size() > kUserRoot.size() ? cleaned.substr(kUserRoot.size() + 1) : std::string();
            }
            (void)normalised;
            std::string host = gUserHostRoot;
            size_t start = 0;
            while (start <= tail.size() && !tail.empty())
            {
                auto end = tail.find('/', start);
                if (end == std::string::npos)
                    end = tail.size();
                std::string component = tail.substr(start, end - start);
                if (component == "." || component.empty())
                {
                    // "save/." is the folder itself (and must never become a file called ".").
                    start = end + 1;
                    continue;
                }
                if (ClassicHost::kSeparator == ':')
                {
                    // Classic limits: 31 characters, no colons.
                    for (auto& c : component)
                        if (c == ':')
                            c = '-';
                    if (component.size() > 31)
                    {
                        uint32_t hash = 2166136261u;
                        for (unsigned char c : component)
                            hash = (hash ^ c) * 16777619u;
                        const auto dot = component.rfind('.');
                        const std::string extension = dot != std::string::npos && component.size() - dot <= 6
                            ? component.substr(dot)
                            : std::string();
                        char tag[16];
                        std::snprintf(tag, sizeof(tag), "#%06x", static_cast<unsigned>(hash & 0xFFFFFF));
                        component = component.substr(0, 31 - 7 - extension.size()) + tag + extension;
                    }
                }
                if (host.back() != ClassicHost::kSeparator)
                    host.push_back(ClassicHost::kSeparator);
                host += component;
                start = end + 1;
            }
            return host;
        }
    } // namespace

    // The engine reads files a few bytes at a time. Each native read is a
    // slow File Manager call on the Mac, so a handle keeps a window of its
    // file in memory (the whole file when it is small).
    struct Handle
    {
        std::FILE* file = nullptr;
        bool ownsFile = false;
        uint64_t base = 0;
        uint64_t size = 0;
        uint64_t position = 0;
        std::vector<uint8_t> window;
        uint64_t windowStart = 0;
        std::string path;
        const std::vector<uint8_t>* resident = nullptr;
    };
    constexpr uint64_t kWindowSize = 256 * 1024;

    void Initialise()
    {
        if (gInitialised)
            return;
        gInitialised = true;
        const auto directory = ClassicHost::ApplicationDirectory();
        MountPack("/rct/openrct2", directory + "openrct2.pak");
        // Optional additions to the engine's own data (title sequence scripts).
        if (std::FILE* probe = std::fopen((directory + "title.pak").c_str(), "rb"))
        {
            std::fclose(probe);
            MountPack("/rct/openrct2", directory + "title.pak");
        }
        MountPack("/rct/game", directory + "game.pak");
        for (int part = 2; part < 32; part++)
        {
            const auto name = directory + "game" + std::to_string(part) + ".pak";
            if (std::FILE* probe = std::fopen(name.c_str(), "rb"))
            {
                std::fclose(probe);
                MountPack("/rct/game", name);
            }
            else
                break;
        }
        // Optional RollerCoaster Tycoon 1 data (rct1.pak, rct1-2.pak, ...): RCT1 scenarios,
        // graphics and music for OpenRCT2's RCT1 support.
        for (int part = 1; part < 32; part++)
        {
            const auto name = directory + (part == 1 ? std::string("rct1.pak") : "rct1-" + std::to_string(part) + ".pak");
            std::FILE* probe = std::fopen(name.c_str(), "rb");
            if (probe == nullptr)
                break;
            std::fclose(probe);
            MountPack("/rct/rct1", name);
        }
        gUserHostRoot = directory + "User";
        ClassicHost::MakeDirectory(gUserHostRoot);
    }

    void SetPreloadEnabled(bool enabled) { gPreloadEnabled = enabled; }
    uint64_t TrackDesignCacheBytes() { return gTrackBytes; }
    uint64_t TrackDesignBytes()
    {
        Initialise(); uint64_t bytes = 0;
        for (const auto& pack : gPacks)
            for (const auto& [key, entry] : pack.entries)
                if (!entry.isDirectory && (key.ends_with(".td6") || key.ends_with(".td4"))) bytes += entry.size;
        return bytes;
    }
    uint64_t ReadPack(std::string_view original, uint64_t offset, void* buffer, uint64_t length)
    {
        Initialise(); const auto path = Normalise(original);
        std::string key, mount;
        if (!MountedKey(path, key, mount)) return 0;
        const Pack* pack = nullptr; const auto* entry = FindEntry(mount, key, &pack);
        if (!entry || entry->isDirectory || offset > entry->size) return 0;
        length = std::min<uint64_t>(length, entry->size - offset);
        uint64_t count = 0;
        while (count < length)
        {
            const auto want = ClassicLoadProgress::Active() ? std::min<uint64_t>(256 * 1024, length - count) : length - count;
            size_t n = 0;
            {
                ClassicDiskIo::Access access(ClassicDiskIo::Op::read, original);
                std::lock_guard<std::mutex> guard(gPackReadLock);
                // Reposition after yielding: other open entries share this FILE.
                const int seek = std::fseek(pack->file, entry->offset + offset + count, SEEK_SET);
                n = seek == 0 ? std::fread(static_cast<uint8_t*>(buffer) + count, 1, want, pack->file) : 0;
                access.End(n, offset + count, n != want);
            }
            count += n;
            ClassicLoadProgress::ReadProgress(offset + count, entry->size);
            if (n != want)
                break;
        }
        ClassicDiskIo::LogicalRead(count);
        return count;
    }
    bool HasRCT1()
    {
        Initialise();
        return std::any_of(gPacks.begin(), gPacks.end(), [](const Pack& pack) { return pack.mount == "/rct/rct1"; });
    }
    std::vector<std::string> PackPaths(std::string_view extension)
    {
        Initialise();
        std::vector<std::string> paths;
        for (const auto& pack : gPacks)
            for (const auto& [key, entry] : pack.entries)
                if (!entry.isDirectory && key.size() >= extension.size() && key.compare(key.size() - extension.size(), extension.size(), extension) == 0)
                    paths.push_back(pack.mount + "/" + key);
        return paths;
    }
    void PreloadTrackDesigns()
    {
#ifndef OPENRCT2_CLASSIC_MAC
        if (const char* flag = std::getenv("RCT_PRELOAD"); flag && flag[0] == '0') gPreloadEnabled = false;
#endif
        if (!gPreloadEnabled || gTracksPreloaded) return;
        Initialise(); ClassicDiskIo::Scope io("track-preload");
        const auto totalBytes = TrackDesignBytes();
        for (const auto& pack : gPacks)
            for (const auto& [key, entry] : pack.entries)
                if (!entry.isDirectory && (key.ends_with(".td6") || key.ends_with(".td4")))
                {
                    if (entry.resident.size() == entry.size) continue;
                    auto& bytes = entry.resident; bytes.resize(entry.size);
                    const auto path = pack.mount + "/" + key;
                    ClassicDiskIo::Access access(ClassicDiskIo::Op::read, path);
                    const int seek = std::fseek(pack.file, entry.offset, SEEK_SET);
                    const size_t n = seek == 0 ? std::fread(bytes.data(), 1, bytes.size(), pack.file) : 0;
                    access.End(n, 0, n != bytes.size());
                    if (n != bytes.size()) bytes.clear();
                    gTrackBytes += bytes.size();
                    ClassicLoadProgress::Report(gTrackBytes, totalBytes);
                }
        gTracksPreloaded = true;
    }

    const std::string& Problems()
    {
        return gProblems;
    }

    Info Query(std::string_view original)
    {
        Initialise();
        const auto path = Normalise(original);
        Info info;
        if (path == "/" || path == "/rct")
        {
            info.exists = info.isDirectory = true;
            return info;
        }
        if (Under(path, "/rct/user"))
            {
            ClassicDiskIo::Access access(ClassicDiskIo::Op::query, original);
            auto info = ClassicHost::Query(UserHostPath(path, original));
            access.End();
            return info;
        }
        std::string key, mount;
        if (MountedKey(path, key, mount))
        {
            if (key.empty())
            {
                info.exists = info.isDirectory = true;
                return info;
            }
            if (const auto* entry = FindEntry(mount, key))
            {
                info.exists = true;
                info.isDirectory = entry->isDirectory;
                info.size = entry->size;
                info.modified = 1; // constant: packs never change while running
            }
        }
        return info;
    }

    bool gCachePackCensus = true;
    bool IsPackedPath(std::string_view original)
    {
        Initialise();
        std::string key, mount;
        return MountedKey(Normalise(original), key, mount)
            && std::any_of(gPacks.begin(), gPacks.end(), [&](const Pack& pack) { return pack.mount == mount; });
    }

    std::vector<Entry> List(std::string_view original)
    {
        Initialise();
        const auto path = Normalise(original);
        std::vector<Entry> result;
        if (path == "/rct")
        {
            for (const char* name : { "openrct2", "game", "user" })
                result.push_back({ name, true, 0, 0 });
            if (HasRCT1())
                result.push_back({ "rct1", true, 0, 0 });
            return result;
        }
        if (Under(path, "/rct/user"))
            {
            ClassicDiskIo::Access access(ClassicDiskIo::Op::list, original);
            auto result = ClassicHost::List(UserHostPath(path, original));
            access.End();
            return result;
        }
        std::string key, mount;
        if (MountedKey(path, key, mount))
        {
            const std::string prefix = key.empty() ? std::string() : key + "/";
            std::map<std::string, Entry> merged; // a directory can appear in several parts
            for (const auto& pack : gPacks)
            {
                if (pack.mount != mount)
                    continue;
                for (auto it = pack.entries.lower_bound(prefix); it != pack.entries.end(); ++it)
                {
                    if (it->first.compare(0, prefix.size(), prefix) != 0)
                        break;
                    if (it->first.find('/', prefix.size()) != std::string::npos)
                        continue; // deeper level
                    merged.emplace(it->first, Entry{ it->second.name, it->second.isDirectory, it->second.size, 1 });
                }
            }
            for (auto& item : merged)
                result.push_back(std::move(item.second));
        }
        return result;
    }

    Handle* Open(std::string_view original)
    {
        Initialise();
        const auto path = Normalise(original);
        if (Under(path, "/rct/user"))
        {
            const auto host = UserHostPath(path, original);
            ClassicDiskIo::Access query(ClassicDiskIo::Op::query, original);
            const auto info = ClassicHost::Query(host); query.End();
            if (!info.exists || info.isDirectory)
                return nullptr;
            ClassicDiskIo::Access access(ClassicDiskIo::Op::open, original);
            std::FILE* file = std::fopen(host.c_str(), "rb");
            access.End(0, 0, file == nullptr);
            if (file == nullptr)
                return nullptr;
            auto* handle = new Handle();
            handle->path = std::string(original);
            handle->file = file;
            handle->ownsFile = true;
            std::setvbuf(file, nullptr, _IONBF, 0);
            ClassicDiskIo::Access seek(ClassicDiskIo::Op::seek, original);
            std::fseek(file, 0, SEEK_END);
            handle->size = static_cast<uint64_t>(std::ftell(file));
            std::fseek(file, 0, SEEK_SET);
            seek.End();
            return handle;
        }
        std::string key, mount;
        if (MountedKey(path, key, mount))
        {
            const Pack* pack = nullptr;
            const auto* entry = FindEntry(mount, key, &pack);
            if (entry == nullptr || entry->isDirectory)
                return nullptr;
            // A design browser often recreates its handle. Retain immutable
            // designs on first use too: at most 1 MiB, without filling the bank
            // on constrained machines. Mutable user designs never enter here.
            if (entry->resident.empty() && entry->size <= 64 * 1024
                && (key.ends_with(".td6") || key.ends_with(".td4"))
                && gTrackBytes + entry->size <= 1024 * 1024)
            {
                const auto memory = ClassicHost::Memory();
                if (memory.free >= 16 * 1024 * 1024 + entry->size && memory.largest >= entry->size)
                {
                    ClassicDiskIo::Scope scope("track-first-use");
                    auto& bytes = entry->resident; bytes.resize(entry->size);
                    if (ReadPack(path, 0, bytes.data(), bytes.size()) != bytes.size()) bytes.clear();
                    gTrackBytes += bytes.size();
                }
            }
            auto* handle = new Handle();
            handle->path = std::string(original);
            handle->file = pack->file;
            handle->base = entry->offset;
            handle->size = entry->size;
            handle->resident = &entry->resident;
            return handle;
        }
        return nullptr;
    }

    void Close(Handle* handle)
    {
        if (handle == nullptr)
            return;
        if (handle->ownsFile)
        {
            ClassicDiskIo::Access access(ClassicDiskIo::Op::close, handle->path);
            const int error = std::fclose(handle->file);
            access.End(0, 0, error != 0);
        }
        delete handle;
    }

    uint64_t Size(const Handle* handle)
    {
        return handle->size;
    }

    uint64_t Position(const Handle* handle)
    {
        return handle->position;
    }

    void Seek(Handle* handle, int64_t offset, int origin)
    {
        int64_t target = offset;
        if (origin == SEEK_CUR)
            target += static_cast<int64_t>(handle->position);
        else if (origin == SEEK_END)
            target += static_cast<int64_t>(handle->size);
        if (target < 0)
            target = 0;
        if (static_cast<uint64_t>(target) > handle->size)
            target = static_cast<int64_t>(handle->size);
        handle->position = static_cast<uint64_t>(target);
    }

    uint64_t Read(Handle* handle, void* buffer, uint64_t length)
    {
        const uint64_t available = handle->size - handle->position;
        if (length > available)
            length = available;
        auto* out = static_cast<uint8_t*>(buffer);
        if (handle->resident && !handle->resident->empty())
        {
            std::memcpy(out, handle->resident->data() + handle->position, static_cast<size_t>(length));
            handle->position += length; ClassicDiskIo::LogicalRead(length);
            return length;
        }
        // Large sequential reads already have a final buffer. Reuse the window's
        // prefix, then read the remainder directly instead of refilling/copying it.
        if (length >= kWindowSize && !handle->ownsFile)
        {
            uint64_t copied = 0;
            if (handle->position >= handle->windowStart && handle->position < handle->windowStart + handle->window.size())
            {
                const auto at = handle->position - handle->windowStart;
                copied = std::min<uint64_t>(length, handle->window.size() - at);
                std::memcpy(out, handle->window.data() + at, copied);
            }
            const auto count = copied + ReadPack(handle->path, handle->position + copied, out + copied, length - copied);
            handle->position += count; ClassicDiskIo::LogicalRead(copied);
            return count;
        }
        uint64_t done = 0;
        while (done < length)
        {
            const uint64_t at = handle->position;
            if (at < handle->windowStart || at >= handle->windowStart + handle->window.size())
            {
                // Refill the window at the current position.
                uint64_t want = std::min<uint64_t>(kWindowSize, handle->size - at);
                // Audio metadata reads only a header, often without ever reading the samples.
                if (at == 0 && handle->window.empty())
                    want = std::min(want, std::max<uint64_t>(4096, length));
                handle->window.resize(static_cast<size_t>(want));
                handle->windowStart = at;
                size_t count = 0;
                {
                    ClassicDiskIo::Access access(ClassicDiskIo::Op::read, handle->path);
                    // Packs share one file between all open entries: always reposition.
                    // (The lock only matters on hosts with threads, when testing.)
                    std::lock_guard<std::mutex> guard(gPackReadLock);
                    std::fseek(handle->file, static_cast<long>(handle->base + at), SEEK_SET);
                    count = std::fread(handle->window.data(), 1, handle->window.size(), handle->file);
                    access.End(count, at, count != want);
                }
                handle->window.resize(count);
                if (count == 0)
                    break;
            }
            const uint64_t offset = at - handle->windowStart;
            const uint64_t chunk = std::min<uint64_t>(length - done, handle->window.size() - offset);
            std::memcpy(out + done, handle->window.data() + offset, static_cast<size_t>(chunk));
            done += chunk;
            handle->position += chunk;
            ClassicLoadProgress::ReadProgress(handle->position, handle->size);
        }
        ClassicDiskIo::LogicalRead(done);
        return done;
    }

    std::string HostPath(std::string_view original)
    {
        Initialise();
        const auto path = Normalise(original);
        if (!Under(path, "/rct/user"))
            return {};
        return UserHostPath(path, original);
    }

    bool MakeDirectories(std::string_view original)
    {
        Initialise();
        const auto path = Normalise(original);
        if (!Under(path, "/rct/user"))
            return Query(original).isDirectory;
        // Create each level in turn.
        std::string partial(original);
        std::vector<std::string> levels;
        while (partial.size() > kUserRoot.size())
        {
            levels.push_back(partial);
            const auto slash = partial.find_last_of("/\\");
            if (slash == std::string::npos)
                break;
            partial.erase(slash);
        }
        bool ok = true;
        for (auto it = levels.rbegin(); it != levels.rend(); ++it)
        {
            const auto host = UserHostPath(Normalise(*it), *it);
            ClassicDiskIo::Access query(ClassicDiskIo::Op::query, *it);
            const auto info = ClassicHost::Query(host); query.End();
            if (!info.exists)
            {
                ClassicDiskIo::Access create(ClassicDiskIo::Op::mkdir, *it);
                const bool made = ClassicHost::MakeDirectory(host); create.End(0, 0, !made);
                ok = made && ok;
            }
        }
        return ok;
    }

    bool Remove(std::string_view path)
    {
        const auto host = HostPath(path);
        ClassicDiskIo::Access access(ClassicDiskIo::Op::remove, path);
        bool ok = !host.empty() && ClassicHost::Remove(host);
        access.End(0, 0, !ok);
        return ok;
    }

    bool Publish(std::string_view from, std::string_view to, bool replace)
    {
        const auto hostFrom = HostPath(from), hostTo = HostPath(to);
        ClassicDiskIo::Access access(ClassicDiskIo::Op::rename, from);
        const bool ok = !hostFrom.empty() && !hostTo.empty() && ClassicHost::PublishFile(hostFrom, hostTo, replace);
        access.End(0, 0, !ok);
        return ok;
    }

    bool Rename(std::string_view from, std::string_view to)
    {
        const auto hostFrom = HostPath(from), hostTo = HostPath(to);
        if (hostFrom.empty() || hostTo.empty())
            return false;
        ClassicHost::Remove(hostTo);
        ClassicDiskIo::Access access(ClassicDiskIo::Op::rename, from);
        bool ok = std::rename(hostFrom.c_str(), hostTo.c_str()) == 0;
        access.End(0, 0, !ok);
        return ok;
    }
} // namespace ClassicVfs
