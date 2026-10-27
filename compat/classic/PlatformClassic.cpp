// Platform services for the pack-file build: Mac OS 9, and Linux when the
// virtual file system is enabled for host testing.
#include "ClassicVfs.h"

#include <cstdio>
#include <ctime>
#include <openrct2/core/Path.hpp>
#include <openrct2/core/String.hpp>
#include <openrct2/localisation/Language.h>
#include <openrct2/platform/Platform.h>

namespace OpenRCT2::Platform
{
    // --- files -------------------------------------------------------------
    AssetCheckResult CheckAssetDirectoryExists(u8string_view path)
    {
        return ClassicVfs::Query(path).isDirectory ? AssetCheckResult::found : AssetCheckResult::notFound;
    }

    AssetCheckResult CheckAssetExists(u8string_view path)
    {
        const auto info = ClassicVfs::Query(path);
        return info.exists && !info.isDirectory ? AssetCheckResult::found : AssetCheckResult::notFound;
    }

    AssetFileOpenResult OpenAssetFile(u8string_view path)
    {
        auto* handle = ClassicVfs::Open(path);
        if (handle == nullptr)
            return AssetFileOpenResult{ AssetCheckResult::notFound, nullptr, 0 };
        return AssetFileOpenResult{ AssetCheckResult::found, handle, ClassicVfs::Size(handle) };
    }

    void CloseAssetFile(AssetHandle handle)
    {
        ClassicVfs::Close(static_cast<ClassicVfs::Handle*>(handle));
    }

    uint64_t GetAssetPosition(AssetHandle handle)
    {
        return ClassicVfs::Position(static_cast<ClassicVfs::Handle*>(handle));
    }

    void SeekAsset(AssetHandle handle, int64_t offset, int32_t origin)
    {
        ClassicVfs::Seek(static_cast<ClassicVfs::Handle*>(handle), offset, origin);
    }

    uint64_t ReadAsset(AssetHandle handle, void* buffer, uint64_t length)
    {
        return ClassicVfs::Read(static_cast<ClassicVfs::Handle*>(handle), buffer, length);
    }

    uint64_t TryReadAsset(AssetHandle handle, void* buffer, uint64_t length)
    {
        return ClassicVfs::Read(static_cast<ClassicVfs::Handle*>(handle), buffer, length);
    }

    u8string GetAssetPath()
    {
        return "/RCT";
    }

    std::string GetFolderPath(SpecialFolder)
    {
        return "/RCT/user";
    }

    std::string GetInstallPath()
    {
        return "/RCT/openrct2";
    }

    std::string GetDocsPath()
    {
        return "/RCT/openrct2";
    }

    std::string GetCurrentExecutablePath()
    {
        return "/RCT/RCT";
    }

    bool ShouldIgnoreCase()
    {
        return true;
    }

    bool IsPathSeparator(char c)
    {
        return c == '/' || c == '\\';
    }

    uint64_t GetLastModified(std::string_view path)
    {
        return ClassicVfs::Query(path).modified;
    }

    uint64_t GetFileSize(std::string_view path)
    {
        return ClassicVfs::Query(path).size;
    }

    std::string ResolveCasing(std::string_view path, bool)
    {
        // Lookups ignore case already.
        return ClassicVfs::Query(path).exists ? std::string(path) : std::string();
    }

    std::vector<std::string> GetSearchablePathsRCT1()
    {
        if (ClassicVfs::HasRCT1())
            return { "/RCT/rct1" };
        return {};
    }

    std::vector<std::string> GetSearchablePathsRCT2()
    {
        return { "/RCT/game" };
    }

    // --- environment ---------------------------------------------------------
    std::string GetEnvironmentVariable(std::string_view)
    {
        return {};
    }

    uint16_t GetLocaleLanguage()
    {
        return LANGUAGE_ENGLISH_US;
    }

    CurrencyType GetLocaleCurrency()
    {
        return GetCurrencyValue("USD");
    }

    MeasurementFormat GetLocaleMeasurementFormat()
    {
        return MeasurementFormat::imperial;
    }

    uint8_t GetLocaleDateFormat()
    {
        return 1; // month, day, year
    }

    TemperatureUnit GetLocaleTemperatureFormat()
    {
        return TemperatureUnit::fahrenheit;
    }

    bool FindApp(std::string_view, std::string*)
    {
        return false;
    }

    int32_t Execute(const char*[], std::string*)
    {
        return -1;
    }

    bool ProcessIsElevated()
    {
        return false;
    }

    std::string GetUsername()
    {
        return "Player";
    }

    SteamPaths GetSteamPaths()
    {
        return {};
    }

    bool IsRunningInWine()
    {
        return false;
    }

    bool IsColourTerminalSupported()
    {
        return false;
    }

    bool HandleSpecialCommandLineArgument(const char*)
    {
        return false;
    }

    u8string StrDecompToPrecomp(u8string_view input)
    {
        return u8string(input);
    }

    bool RequireNewWindow(bool)
    {
        return true;
    }

    int32_t GetDrives()
    {
        return 0;
    }

    time_t FileGetModifiedTime(u8string_view path)
    {
        return static_cast<time_t>(ClassicVfs::Query(path).modified);
    }

    datetime64 GetDatetimeNowUTC()
    {
        // .NET-style ticks: 100 ns units since year 1.
        const datetime64 epochAsTicks = 621355968000000000ULL;
        return epochAsTicks + static_cast<datetime64>(std::time(nullptr)) * 10000000ULL;
    }

    std::string FormatShortDate(std::time_t timestamp)
    {
        char buffer[32]{};
        if (const auto* local = std::localtime(&timestamp))
            std::strftime(buffer, sizeof(buffer), "%m/%d/%Y", local);
        return buffer;
    }

    std::string FormatTime(std::time_t timestamp)
    {
        char buffer[32]{};
        if (const auto* local = std::localtime(&timestamp))
            std::strftime(buffer, sizeof(buffer), "%H:%M", local);
        return buffer;
    }

#ifdef OPENRCT2_CLASSIC_MAC
    void Sleep(uint32_t ms)
    {
        const auto until = ClassicHost::Microseconds() + uint64_t(ms) * 1000;
        do
        {
            // WNE sleep is in 1/60 s ticks, not milliseconds. A 1 ms engine
            // wait must not sleep an entire system tick. WNE(0) still gives
            // other cooperative applications their turn; measure that cost.
            const auto now = ClassicHost::Microseconds();
            const bool wholeTickFits = now < until && until - now >= 18000;
            ClassicHost::Yield(ClassicHost::gCoarseFrameWait || wholeTickFits ? 1 : 0);
        } while (ClassicHost::Microseconds() < until);
    }

    uint32_t GetTicks()
    {
        static const uint64_t start = ClassicHost::Microseconds();
        return static_cast<uint32_t>((ClassicHost::Microseconds() - start) / 1000);
    }
#endif
} // namespace OpenRCT2::Platform
