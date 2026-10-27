// Headless driver for the real engine: loads a park, advances the simulation
// and prints state digests. The digests (random state, counts, and a hash of
// the exported park file) must be identical on little- and big-endian builds,
// which makes this the oracle for byte-order work and a timing harness.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include "BuildIdentity.h"
#include <openrct2/core/OrcaStream.hpp>
#include <openrct2/Context.h>
#include <openrct2/core/ScopeProbe.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/PlatformEnvironment.h>
#include <openrct2/core/Crypt.h>
#include <openrct2/core/MemoryStream.h>
#include <openrct2/core/File.h>
#include <openrct2/core/Path.hpp>
#include <openrct2/localisation/LocalisationService.h>
#include <openrct2/entity/EntityRegistry.h>
#include <openrct2/park/ParkFile.h>
#include <openrct2/platform/Platform.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/ride/RideManager.hpp>
#include <openrct2/rct2/RCT2.h>
#include <openrct2/scenario/Scenario.h>
#include <openrct2/scenario/ScenarioRepository.h>
#include <openrct2/util/Util.h>
#ifdef OPENRCT2_PAK_VFS
    #include "ClassicVfs.h"
#endif
#ifndef OPENRCT2_CLASSIC_MAC
#include "RideStartRegression.h"
#endif

#ifdef OPENRCT2_CLASSIC_MAC
    #include "ClassicVfs.h"

    #include <MacMemory.h>
    #include <string>
    #include <vector>
#endif

using namespace OpenRCT2;

static const char* gSavePrefix = nullptr;
static const char* gFinalSavePrefix = nullptr;
static bool gProbes = false;
static const char* gDumpPrefix = nullptr;
static FILE* gFieldFile = nullptr;

#if defined(OPENRCT2_PAK_VFS) && !defined(OPENRCT2_CLASSIC_MAC)
// Opt-in integration checks, run only with the test's isolated writable user
// directory. Game assets are read from the unchanged packs.
static void scenarioIndexChecks()
{
    auto* repo = GetContext()->GetScenarioRepository();
    const auto language = LocalisationService_GetCurrentLanguage();
    const char* name = "rct-g3rain-index-fixture.sc6";
    const auto directory = GetContext()->GetPlatformEnvironment().GetDirectoryPath(DirBase::user, DirId::scenarios);
    const auto path = Path::Combine(directory, name);
    const char* missing = "rct-g3rain-missing-fixture.sea";
    auto require = [](bool condition, const char* message) { if (!condition) throw std::runtime_error(message); };
    require(!File::Exists(path), "fixture directory is not clean");
    repo->Scan(language);
    const auto count = repo->GetCount();
    require(!repo->TryRecordHighscore(language, missing, 9000123, "G3 fixture"), "missing scenario recorded");
    Path::CreateDirectory(Path::GetDirectory(path));
    auto bytes = DecryptSea(fs::u8path("/RCT/game/Assets/fiasco forest.sea"));
    File::WriteAllBytes(path,bytes.data(),bytes.size());
    require(repo->TryRecordHighscore(language, name, 9000123, "G3 fixture"), "fresh addition not found");
    require(repo->GetCount() == count + 1, "addition changed count incorrectly");
    const auto oldName = repo->GetByFilename(name)->InternalName;
    require(!repo->TryRecordHighscore(language, name, 9000123, "G3 fixture"), "equal named score overwritten");
    auto other = CreateScenarioRepository(GetContext()->GetPlatformEnvironment());
    require(other->TryRecordHighscore(language, name, 9000125, "Other writer"), "other repository did not record");
    require(!repo->TryRecordHighscore(language, name, 9000124, "G3 fixture"), "fresh external score ignored");
    require(repo->GetByFilename(name)->Highscore && repo->GetByFilename(name)->Highscore->company_value == 9000125,
        "score attachment stale");
    bytes = DecryptSea(fs::u8path("/RCT/game/Assets/six flags magic mountain.sea"));
    File::WriteAllBytes(path,bytes.data(),bytes.size());
    (void)repo->TryRecordHighscore(language, missing, 1, nullptr);
    require(repo->GetByFilename(name) && repo->GetByFilename(name)->InternalName != oldName, "replacement metadata stale");
    require(File::Delete(path), "fixture removal failed");
    require(!repo->TryRecordHighscore(language, name, 9000126, "G3 fixture"), "removed scenario recorded");
    require(!repo->GetByFilename(name) && repo->GetCount() == count, "removed scenario still indexed");
    repo->Scan(language == 1 ? 2 : 1);
    require(!repo->TryRecordHighscore(language, missing, 1, nullptr), "language refresh changed missing lookup");
    std::puts("PASS scenario index: unchanged list, add, external score, replacement, removal, language refresh");
}
#endif

static void traceField(uint32_t chunk, uint64_t offset, uint64_t size, const char* kind, std::source_location source)
{
    std::fprintf(gFieldFile, "%lx\t%lu\t%lu\t%s\t%s\t%u\n", static_cast<unsigned long>(chunk),
        static_cast<unsigned long>(offset), static_cast<unsigned long>(size), kind, source.file_name(), source.line());
}

static void saveBytes(const char* path, const void* data, size_t size)
{
    FILE* file = std::fopen(path, "wb");
    if (file == nullptr)
        throw std::runtime_error(std::string("Cannot open state output: ") + path);
    const bool failed = std::fwrite(data, 1, size, file) != size;
    if (std::fclose(file) != 0 || failed)
        throw std::runtime_error(std::string("Cannot write state output: ") + path);
}

static void digest(const char* label, bool hashPark)
{
    auto& gameState = getGameState();
    const auto& rand = ScenarioRandState();
    // Plain long arguments: the Classic C library does not format long long.
    std::printf("%s tick=%lu rand=%08lx:%08lx guests=%lu cash=%ld rating=%lu entities=", label,
        static_cast<unsigned long>(gameState.currentTicks), static_cast<unsigned long>(rand.s0),
        static_cast<unsigned long>(rand.s1), static_cast<unsigned long>(gameState.park.numGuestsInPark),
        static_cast<long>(gameState.park.cash), static_cast<unsigned long>(gameState.park.rating));
    for (int type = 0; type < static_cast<int>(EntityType::count); type++)
        std::printf("%s%u", type ? "," : "", static_cast<unsigned>(gameState.entities.getEntityListCount(static_cast<EntityType>(type))));
    if (hashPark || gSavePrefix != nullptr || gDumpPrefix != nullptr)
    {
        MemoryStream stream;
        ParkFileExporter exporter;
        char dumpPath[512];
        if (gDumpPrefix != nullptr)
        {
            std::snprintf(dumpPath, sizeof(dumpPath), "%s-%u.fields.tsv", gDumpPrefix,
                static_cast<unsigned>(gameState.currentTicks));
            gFieldFile = std::fopen(dumpPath, "w");
            if (gFieldFile == nullptr)
                throw std::runtime_error(std::string("Cannot open field output: ") + dumpPath);
            std::fprintf(gFieldFile, "# rctsim field schema v1: chunk(hex), offset, size, kind, file, line\n");
            std::fprintf(gFieldFile, "# source=%s build=%s\n", RctBuild::Source() + 14, RctBuild::Id() + 13);
            OrcaStream::writeTrace = traceField;
        }
        exporter.Export(gameState, stream, 0);
        if (gFieldFile != nullptr)
        {
            OrcaStream::writeTrace = nullptr;
            const bool failed = std::ferror(gFieldFile) != 0;
            if (std::fclose(gFieldFile) != 0 || failed)
                throw std::runtime_error(std::string("Cannot write field output: ") + dumpPath);
            gFieldFile = nullptr;
        }
        // Hash every chunk except the authoring one, which holds save timestamps.
        const auto* data = static_cast<const uint8_t*>(stream.GetData());
        const auto le32 = [data](size_t at) {
            return uint32_t(data[at]) | (uint32_t(data[at + 1]) << 8) | (uint32_t(data[at + 2]) << 16)
                | (uint32_t(data[at + 3]) << 24);
        };
        const uint32_t chunkCount = le32(12);
        const size_t body = 64 + size_t(chunkCount) * 20;
        auto hasher = Crypt::CreateFNV1a();
        for (uint32_t i = 0; i < chunkCount; i++)
        {
            const size_t entry = 64 + size_t(i) * 20;
            if (le32(entry) != 1)
                hasher->Update(data + body + le32(entry + 4), le32(entry + 12));
        }
        const auto hash = hasher->Finish();
        if (gSavePrefix != nullptr)
        {
            // Keep the exported park so differing builds can be compared chunk by chunk.
            char path[512];
            std::snprintf(path, sizeof(path), "%s-%u.park", gSavePrefix, static_cast<unsigned>(gameState.currentTicks));
            saveBytes(path, stream.GetData(), static_cast<size_t>(stream.GetLength()));
        }
        if (gDumpPrefix != nullptr)
        {
            std::snprintf(dumpPath, sizeof(dumpPath), "%s-%u.park", gDumpPrefix,
                static_cast<unsigned>(gameState.currentTicks));
            saveBytes(dumpPath, stream.GetData(), static_cast<size_t>(stream.GetLength()));
        }
        if (hashPark)
        {
            std::printf(" park_bytes=%lu park_fnv=", static_cast<unsigned long>(stream.GetLength()));
            for (const auto byte : hash)
                std::printf("%02x", byte);
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}

int main(int argc, char** argv)
{
    // Make sure the C++ stream library is initialised on every target.
    static std::ios_base::Init streamInit;
#ifdef OPENRCT2_CLASSIC_MAC
    // No command line on Mac OS 9: arguments come from RCTSim.txt beside the
    // application (one per line: park path, ticks, digest interval, options)
    // and output goes to RCTSim.log.
    MaxApplZone();
    for (int i = 0; i < 16; i++)
        MoreMasters();
    const auto directory = ClassicHost::ApplicationDirectory();
    std::freopen((directory + "RCTSim.log").c_str(), "w", stdout);
    std::freopen((directory + "RCTSim.err").c_str(), "w", stderr);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    static std::vector<std::string> lines{ "rctsim" };
    if (FILE* file = std::fopen((directory + "RCTSim.txt").c_str(), "rb"))
    {
        // Lines may end in CR (Mac), LF or both.
        std::string text;
        for (int c = std::fgetc(file);; c = std::fgetc(file))
        {
            if (c == EOF || c == '\n' || c == '\r')
            {
                while (!text.empty() && text.back() == ' ')
                    text.pop_back();
                if (!text.empty())
                    lines.push_back(text);
                text.clear();
                if (c == EOF)
                    break;
            }
            else
                text.push_back(static_cast<char>(c));
        }
        std::fclose(file);
    }
    static std::vector<char*> pointers;
    for (auto& text : lines)
        pointers.push_back(text.data());
    argc = static_cast<int>(pointers.size());
    argv = pointers.data();
    std::printf("rctsim on Mac OS 9: app dir %s, free heap %ld, %d arguments\n", directory.c_str(), static_cast<long>(FreeMem()),
        argc - 1);
    {
        // Check the stream library before the engine depends on it.
        std::ostringstream probe;
        probe << "streams " << 42 << ' ' << 1.5;
        std::printf("%s\n", probe.str().c_str());
    }
    ClassicVfs::Initialise();
    if (!ClassicVfs::Problems().empty())
        std::printf("file system: %s", ClassicVfs::Problems().c_str());
#endif
    if (!RctBuild::Start())
        return 125;
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: rctsim <park file> <ticks> [digest interval] [--no-park-hash] [--final-park-hash] [--final-save prefix] [--save prefix]"
            " [--dump-state prefix] [--keep-rng]\n");
        return 2;
    }
    const int ticks = std::atoi(argv[2]);
    const int every = argc > 3 && argv[3][0] != '-' ? std::atoi(argv[3]) : 0;
    bool hashPark = true;
    bool finalHashOnly = false;
    bool keepRng = false;
    for (int i = 3; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--probes") == 0)
            gProbes = true;
        if (std::strcmp(argv[i], "--no-park-hash") == 0)
            hashPark = false;
        if (std::strcmp(argv[i], "--final-park-hash") == 0)
        {
            finalHashOnly = true;
            hashPark = false;
        }
        if (std::strcmp(argv[i], "--final-save") == 0 && i + 1 < argc)
            gFinalSavePrefix = argv[++i];
        else if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc)
            gSavePrefix = argv[++i];
        else if (std::strcmp(argv[i], "--dump-state") == 0 && i + 1 < argc)
            gDumpPrefix = argv[++i];
        else if (std::strcmp(argv[i], "--keep-rng") == 0)
            keepRng = true;
    }

    gOpenRCT2Headless = true;
    gOpenRCT2NoGraphics = true;
    std::unique_ptr<IContext> context(CreateContext());
    if (!context->Initialise())
    {
        std::fprintf(stderr, "rctsim: context initialisation failed\n");
        return 1;
    }
    if (!context->LoadParkFromFile(argv[1]))
    {
        std::fprintf(stderr, "rctsim: unable to load %s\n", argv[1]);
        return 1;
    }
    // Starting a scenario seeds the generator from the clock; pin it so runs
    // are reproducible and comparable across builds.
    if (!keepRng)
        ScenarioRandSeed(0x1234567F, 0x789FABCD);
    // Utility randomness (e.g. carousel tune choice) is not saved or part of
    // ScenarioRandState, but its results can enter exported ride state. Pin it
    // separately so even cosmetic fields compare reproducibly. The game keeps
    // its normal entropy-seeded utility generator.
    UtilRandSeed(0x1234567F);
#if defined(OPENRCT2_PAK_VFS) && !defined(OPENRCT2_CLASSIC_MAC)
    if (const auto* reuse = std::getenv("RCTSIM_SCENARIO_REUSE")) gScenarioReuseIndex = reuse[0] != '0';
    if (const auto* cache = std::getenv("RCTSIM_SCENARIO_PACK_CENSUS")) ClassicVfs::gCachePackCensus = cache[0] != '0';
    if (std::getenv("RCTSIM_SCENARIO_INDEX_CHECKS")) scenarioIndexChecks();
    if (std::getenv("RCTSIM_MONTH_EDGE"))
    {
        getGameState().date.monthsElapsed = 23;
        getGameState().date.monthTicks = 65520;
    }
#endif
    if (std::getenv("RCTSIM_RIDES") != nullptr)
    {
        for (auto& ride : RideManager(getGameState()))
        {
            auto& station = ride.getStations()[0];
            std::printf("ride %u start=%d,%d entrance=%d,%d,%d,%d exit=%d,%d,%d,%d boat=%d,%d overall=%d,%d\n",
                unsigned(ride.id.ToUnderlying()), int(station.start.x), int(station.start.y), int(station.entrance.x),
                int(station.entrance.y), int(station.entrance.z), int(station.entrance.direction), int(station.exit.x),
                int(station.exit.y), int(station.exit.z), int(station.exit.direction), int(ride.boatHireReturnPosition.x),
                int(ride.boatHireReturnPosition.y), int(ride.overallView.x), int(ride.overallView.y));
        }
    }
#ifndef OPENRCT2_CLASSIC_MAC
    RideStartRegression::configure();
    RideStartRegression::construct();
    RideStartRegression::observe(0);
#endif
    digest("loaded", hashPark);
#ifdef OPENRCT2_CLASSIC_MAC
    // Time only the simulation, with the Toolbox microsecond clock, and let
    // other applications run between batches.
    long long micros = 0;
    OpenRCT2::Probe::gLastStamp = OpenRCT2::Probe::Stamp();
    OpenRCT2::Probe::gEnabled = gProbes;
    for (int i = 1; i <= ticks; i++)
    {
        const auto before = ClassicHost::Microseconds();
        gameStateUpdateLogic();
        micros += static_cast<long long>(ClassicHost::Microseconds() - before);
        if (i % 20 == 0)
            ClassicHost::Yield();
        if (every > 0 && i % every == 0 && i != ticks)
        {
            std::printf("progress ticks=%d total_us=%ld\n", i, static_cast<long>(micros));
            digest("step", hashPark);
        }
    }
#else
    const auto started = std::chrono::steady_clock::now();
    OpenRCT2::Probe::gLastStamp = OpenRCT2::Probe::Stamp();
    OpenRCT2::Probe::gEnabled = gProbes;
    for (int i = 1; i <= ticks; i++)
    {
        gameStateUpdateLogic();
        if (i <= 8 || i % 400 == 0) RideStartRegression::observe(i);
        if (every > 0 && i % every == 0 && i != ticks)
            digest("step", hashPark);
    }
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
#endif
    if (gFinalSavePrefix != nullptr)
        gSavePrefix = gFinalSavePrefix;
    digest("final", hashPark || finalHashOnly);
#ifdef OPENRCT2_CLASSIC_MAC
    std::printf("free heap after run %ld\n", static_cast<long>(FreeMem()));
#endif
    std::printf("timing ticks=%d total_us=%ld per_tick_us=%ld\n", ticks, static_cast<long>(micros),
        static_cast<long>(ticks ? micros / ticks : 0));
    if (OpenRCT2::Probe::gEnabled)
    {
        // Where the tick time went, by probed function (exclusive of the probed functions it calls).
        OpenRCT2::Probe::gEnabled = false;
        uint64_t sum = 0;
        for (int i = 1; i < OpenRCT2::Probe::Count; i++) // [0] is everything outside the game logic
            sum += OpenRCT2::Probe::gTicks[i];
        for (int i = 1; i < OpenRCT2::Probe::Count && sum > 0 && ticks > 0; i++)
            std::printf("probe %-28s %5.1f%%  %7.0f us/tick  %7.1f calls/tick\n", OpenRCT2::Probe::kNames[i],
                100.0 * static_cast<double>(OpenRCT2::Probe::gTicks[i]) / static_cast<double>(sum),
                static_cast<double>(micros) / ticks * static_cast<double>(OpenRCT2::Probe::gTicks[i]) / static_cast<double>(sum),
                static_cast<double>(OpenRCT2::Probe::gCalls[i]) / ticks);
    }
    return 0;
}
