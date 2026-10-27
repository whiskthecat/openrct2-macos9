#ifdef OPENRCT2_PAK_VFS
    #include "ClassicLoadProgress.h"
#endif
// Read-only zip archive support built on zlib alone, replacing libzip.
// Object packages (.parkobj) and asset packs only need reading.
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <openrct2/core/FileStream.h>
#include <openrct2/core/IStream.hpp>
#include <openrct2/core/MemoryStream.h>
#include <openrct2/core/Zip.h>
#include <stdexcept>
#include <string>
#include <zlib.h>

using namespace OpenRCT2;

namespace
{
#ifdef OPENRCT2_SERVICES_ZIP_COUNTS
    struct PathCounts
    {
        uint64_t archives = 0, queries = 0, visits = 0;
        ~PathCounts()
        {
            std::fprintf(stderr, "services-zip archives=%lu queries=%lu visits=%lu\n", static_cast<unsigned long>(archives),
                static_cast<unsigned long>(queries), static_cast<unsigned long>(visits));
        }
    } pathCounts;
#endif
    uint16_t read16(const uint8_t* p)
    {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }
    uint32_t read32(const uint8_t* p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16)
            | (static_cast<uint32_t>(p[3]) << 24);
    }
    bool samePath(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++)
        {
            const char x = a[i] == '\\' ? '/' : a[i], y = b[i] == '\\' ? '/' : b[i];
            if (x != y)
                return false;
        }
        return true;
    }

    class ZipMini final : public IZipArchive
    {
        struct Entry
        {
            std::string name;
            uint32_t compressedSize{}, size{}, headerOffset{};
            uint16_t method{};
        };
        mutable FileStream _file;
        std::vector<Entry> _entries;

    public:
        explicit ZipMini(std::string_view path)
            : _file(std::string(path), FileMode::open)
        {
#ifdef OPENRCT2_SERVICES_ZIP_COUNTS
            pathCounts.archives++;
#endif
            const uint64_t length = _file.GetLength();
            const uint64_t tailSize = std::min<uint64_t>(length, 65557);
            std::vector<uint8_t> tail(static_cast<size_t>(tailSize));
            _file.SetPosition(length - tailSize);
            _file.Read(tail.data(), tail.size());
            size_t end = tail.size();
            for (size_t i = tail.size() >= 22 ? tail.size() - 22 : 0;; i--)
            {
                if (tail.size() >= 22 && read32(&tail[i]) == 0x06054b50)
                {
                    end = i;
                    break;
                }
                if (i == 0)
                    break;
            }
            if (end == tail.size())
                throw std::runtime_error("Unable to open zip file.");
            const uint16_t count = read16(&tail[end + 10]);
            const uint32_t directorySize = read32(&tail[end + 12]), directoryOffset = read32(&tail[end + 16]);
            std::vector<uint8_t> directory(directorySize);
            _file.SetPosition(directoryOffset);
            _file.Read(directory.data(), directory.size());
            size_t at = 0;
            for (uint16_t i = 0; i < count; i++)
            {
                if (at + 46 > directory.size() || read32(&directory[at]) != 0x02014b50)
                    throw std::runtime_error("Zip directory is damaged.");
                Entry entry;
                entry.method = read16(&directory[at + 10]);
                entry.compressedSize = read32(&directory[at + 20]);
                entry.size = read32(&directory[at + 24]);
                const uint16_t nameLength = read16(&directory[at + 28]);
                const uint16_t extraLength = read16(&directory[at + 30]), commentLength = read16(&directory[at + 32]);
                entry.headerOffset = read32(&directory[at + 42]);
                entry.name.assign(reinterpret_cast<const char*>(&directory[at + 46]), nameLength);
                at += size_t(46) + nameLength + extraLength + commentLength;
                _entries.push_back(std::move(entry));
            }
        }

        size_t GetNumFiles() const override
        {
            return _entries.size();
        }
        std::string GetFileName(size_t index) const override
        {
            return index < _entries.size() ? _entries[index].name : std::string();
        }
        uint64_t GetFileSize(size_t index) const override
        {
            return index < _entries.size() ? _entries[index].size : 0;
        }
        std::optional<size_t> FindPath(std::string_view path) const
        {
#ifdef OPENRCT2_SERVICES_ZIP_COUNTS
            pathCounts.queries++;
#endif
            for (size_t i = 0; i < _entries.size(); i++)
            {
#ifdef OPENRCT2_SERVICES_ZIP_COUNTS
                pathCounts.visits++;
#endif
                if (samePath(_entries[i].name, path)) return i;
            }
            return std::nullopt;
        }
        std::vector<uint8_t> GetFileData(std::string_view path) const override
        {
            std::vector<uint8_t> result;
            const auto index = FindPath(path);
            if (!index.has_value())
                return result;
            const auto& entry = _entries[*index];
            uint8_t header[30];
            _file.SetPosition(entry.headerOffset);
            _file.Read(header, sizeof(header));
            if (read32(header) != 0x04034b50)
                return result;
            _file.SetPosition(uint64_t(entry.headerOffset) + 30 + read16(header + 26) + read16(header + 28));
            std::vector<uint8_t> packed(entry.compressedSize);
            _file.Read(packed.data(), packed.size());
            if (entry.method == 0)
                return packed;
            if (entry.method != 8)
                return result;
            result.resize(entry.size);
            z_stream stream{};
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
                return {};
            stream.next_in = packed.data();
            stream.avail_in = static_cast<uInt>(packed.size());
            stream.next_out = result.data();
            int status;
#ifdef OPENRCT2_PAK_VFS
            ClassicLoadProgress::Range progress(.55, .75, "object-inflate");
            do
            {
                stream.next_out = result.data() + stream.total_out;
                stream.avail_out = static_cast<uInt>(std::min<size_t>(64 * 1024, result.size() - stream.total_out));
                status = inflate(&stream, Z_NO_FLUSH);
                ClassicLoadProgress::Report(stream.total_out, result.size());
            } while (status == Z_OK && stream.avail_out == 0 && stream.total_out < result.size());
#else
            stream.avail_out = static_cast<uInt>(result.size());
            status = inflate(&stream, Z_FINISH);
#endif
            inflateEnd(&stream);
            if (status != Z_STREAM_END)
                return {};
            return result;
        }
        std::unique_ptr<IStream> GetFileStream(std::string_view path) const override
        {
            auto data = GetFileData(path);
            auto stream = std::make_unique<MemoryStream>();
            if (!data.empty())
                stream->Write(data.data(), data.size());
            stream->SetPosition(0);
            return stream;
        }
        void SetFileData(std::string_view, std::vector<uint8_t>&&) override
        {
            throw std::runtime_error("Zip archives are read-only in this build.");
        }
        void DeleteFile(std::string_view) override
        {
            throw std::runtime_error("Zip archives are read-only in this build.");
        }
        void RenameFile(std::string_view, std::string_view) override
        {
            throw std::runtime_error("Zip archives are read-only in this build.");
        }
    };
} // namespace

std::optional<size_t> IZipArchive::GetIndexFromPath(std::string_view path) const
{
    // Keep the public by-value API and generic fallback for other implementations.
    // ZipMini can compare its stored names directly, without a temporary per visit.
    if (const auto* mini = dynamic_cast<const ZipMini*>(this)) return mini->FindPath(path);
    for (size_t i = 0; i < GetNumFiles(); i++)
        if (samePath(GetFileName(i), path))
            return i;
    return std::nullopt;
}

bool IZipArchive::Exists(std::string_view path) const
{
    return GetIndexFromPath(path).has_value();
}

namespace OpenRCT2::Zip
{
    std::unique_ptr<IZipArchive> Open(std::string_view path, ZipAccess access)
    {
        if (access != ZipAccess::read)
            throw std::runtime_error("Zip archives are read-only in this build.");
        return std::make_unique<ZipMini>(path);
    }
    std::unique_ptr<IZipArchive> TryOpen(std::string_view path, ZipAccess access)
    {
        try
        {
            return Open(path, access);
        }
        catch (const std::exception&)
        {
            return nullptr;
        }
    }
} // namespace OpenRCT2::Zip
