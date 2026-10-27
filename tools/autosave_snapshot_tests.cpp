// A live simulation continues throughout preparation/writing. Compare every
// persisted field at capture, then every tick after loading that exact capture.
#include "BuildIdentity.h"
#include "ClassicAutosave.h"
#include "ClassicVfs.h"
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/core/MemoryStream.h>
#include <openrct2/core/Compression.h>
#include <openrct2/core/Crypt.h>
#include <openrct2/park/ParkFile.h>
#include <openrct2/scenario/Scenario.h>
#include <openrct2/util/Util.h>
#include <cstdio>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>
using namespace OpenRCT2;
static MemoryStream Export()
{
    ParkFileExporter exporter; exporter.SkipPreviewImages = true;
    MemoryStream bytes; exporter.Export(getGameState(), bytes, 0); return bytes;
}
static uint64_t LE(const uint8_t* p, unsigned n)
{
    uint64_t v = 0; for (unsigned i = 0; i < n; i++) v |= uint64_t(p[i]) << (8 * i); return v;
}
using Chunks = std::map<uint32_t, std::vector<uint8_t>>;
static Chunks ReadChunks(const MemoryStream& bytes)
{
    const auto* b = static_cast<const uint8_t*>(bytes.GetData());
    const auto n = LE(b + 12, 4); const auto start = 64 + n * 20;
    MemoryStream decoded;
    const auto compression = LE(b + 24, 4);
    const auto* payload = b + start;
    if (compression)
    {
        MemoryStream input(const_cast<uint8_t*>(payload), bytes.GetLength() - start, MemoryAccess::read);
        bool ok = compression == 2
            ? Compression::zstdDecompress(input, LE(b + 28, 8), decoded, LE(b + 16, 8))
            : compression == 1 && Compression::zlibDecompress(input, LE(b + 28, 8), decoded,
                LE(b + 16, 8), Compression::ZlibHeaderType::gzip);
        if (!ok) throw std::runtime_error("snapshot decompression failed");
        payload = static_cast<const uint8_t*>(decoded.GetData());
    }
    const size_t payloadLength = compression ? decoded.GetLength() : bytes.GetLength() - start;
    Chunks result;
    for (unsigned i = 0; i < n; i++)
    {
        const auto* entry = b + 64 + 20 * i;
        const auto id = LE(entry, 4), at = LE(entry + 4, 8), len = LE(entry + 12, 8);
        if (at + len > payloadLength) throw std::runtime_error("invalid export table");
        if (id != 1) result[id] = std::vector<uint8_t>(payload + at, payload + at + len);
    }
    return result;
}
using Signatures = std::map<uint32_t, std::pair<uint64_t, Crypt::FNV1aAlgorithm::Result>>;
static Signatures Signature(const MemoryStream& bytes)
{
    Signatures result;
    for (const auto& [id, data] : ReadChunks(bytes)) result[id] = {data.size(), Crypt::FNV1a(data.data(), data.size())};
    return result;
}
static void Write(const std::string& path, const MemoryStream& bytes)
{
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) throw std::runtime_error("cannot write evidence");
    const bool ok = std::fwrite(bytes.GetData(), 1, bytes.GetLength(), f) == bytes.GetLength();
    if (std::fclose(f) || !ok) throw std::runtime_error("evidence write failed");
}
// Snapshot publication streams the stock 1001-square technical grid, whereas
// synchronous native exports keep 256 squares. Expand the reference exactly:
// no tile byte or other field from the publication is discarded/normalized.
static std::vector<uint8_t> PublishedTiles(const std::vector<uint8_t>& compact)
{
    constexpr unsigned native = 256, stock = 1001;
    std::vector<uint8_t> result(compact.begin(), compact.begin() + 12);
    const uint32_t count = uint32_t(LE(compact.data() + 8, 4)) + stock*stock - native*native;
    for (unsigned i = 0; i < 4; ++i) result[8+i] = uint8_t(count >> (i*8));
    const std::array<uint8_t,16> padding{0,128,14,14};
    size_t cursor = 12;
    for (unsigned y = 0; y < stock; ++y) for (unsigned x = 0; x < stock; ++x)
    {
        if (x >= native || y >= native) result.insert(result.end(), padding.begin(), padding.end());
        else
        {
            bool last;
            do {
                if (cursor + 16 > compact.size()) throw std::runtime_error("compact tile reference truncated");
                last = (compact[cursor+1] & 128) != 0;
                result.insert(result.end(), compact.begin()+cursor, compact.begin()+cursor+16); cursor += 16;
            } while (!last);
        }
    }
    if (cursor != compact.size()) throw std::runtime_error("compact tile reference has trailing state");
    return result;
}
static bool Same(const MemoryStream& expected, const MemoryStream& actual, const std::string& prefix, unsigned tick)
{
    auto a = ReadChunks(expected); const auto b = ReadChunks(actual);
    if (a.contains(48) && b.contains(48) && a[48].size() != b.at(48).size())
        a[48] = PublishedTiles(a[48]);
    if (a == b) return true;
    Write(prefix + "-expected-" + std::to_string(tick) + ".park", expected);
    Write(prefix + "-actual-" + std::to_string(tick) + ".park", actual);
    for (const auto& [id, value] : a)
    {
        const auto it = b.find(id);
        if (it == b.end()) { std::printf("missing chunk %u\n", id); continue; }
        if (value == it->second) continue;
        size_t offset = 0; while (offset < value.size() && offset < it->second.size() && value[offset] == it->second[offset]) offset++;
        std::printf("DIFF continued_tick=%u chunk=%u offset=%zu sizes=%zu/%zu\n", tick, id, offset, value.size(), it->second.size());
    }
    return false;
}
int main(int argc, char** argv)
{
    if (!RctBuild::Start()) return 125;
    if (argc != 4) return 2;
    ClassicVfs::Initialise();
    gOpenRCT2Headless = gOpenRCT2NoGraphics = true;
    std::unique_ptr<IContext> context(CreateContext());
    if (!context->Initialise() || !context->LoadParkFromFile(argv[1])) return 1;
    ScenarioRandSeed(0x1234567F, 0x789FABCD); UtilRandSeed(0x1234567F);
    for (unsigned i = 0; i < 100; i++) gameStateUpdateLogic();
    if (std::strcmp(argv[3], "failure") == 0)
    {
        auto before = Export();
        const auto tick = getGameState().currentTicks;
        ClassicAutosave::BeginExport("/RCT/user/autosave-tests/failure.park", [](MemoryStream&, MemoryStream&) -> bool {
            throw std::runtime_error("injected preparation failure before load");
        }, 0, 0, tick, true);
        if (context->LoadParkFromFile(argv[1]) || getGameState().currentTicks != tick
            || !Same(before, Export(), argv[2], 0)) return 1;
        std::puts("PASS failed save cancels park load; every persisted live field remains exact");
        return 0;
    }
    const bool manual = std::strcmp(argv[3], "manual") == 0;
    gIsAutosave = !manual;
    const uint32_t tick = getGameState().currentTicks;
    UtilRandSeed(0x1234567F);
    auto atCapture = Export();
    const std::string directory = "/RCT/user/autosave-tests/";
    ClassicVfs::MakeDirectories(directory);
    const std::string baseline = directory + "synchronous.park";
    const std::string path = directory + "snapshot.park";
    Write(ClassicVfs::HostPath(baseline), atCapture);
    ClassicVfs::Remove(path);
    if (manual) Write(ClassicVfs::HostPath(path), atCapture);

    // First establish uninterrupted, NO-SAVE execution. The asynchronous save
    // below must match this, including stale valuation caches, at every tick.
    std::vector<MemoryStream> uninterrupted;
    std::vector<Signatures> uninterruptedSignatures;
    for (unsigned i = 0; i < 400; ++i)
    {
        gameStateUpdateLogic(); auto expected = Export();
        uninterruptedSignatures.push_back(Signature(expected));
        if (i < 50) uninterrupted.push_back(std::move(expected));
    }
    // Restore the original fixture, not the captured PARK: loading that PARK
    // correctly refreshes valuations and is intentionally a different state.
    if (!context->LoadParkFromFile(argv[1])) return 1;
    ScenarioRandSeed(0x1234567F, 0x789FABCD); UtilRandSeed(0x1234567F);
    for (unsigned i = 0; i < 100; ++i) gameStateUpdateLogic();
    UtilRandSeed(0x1234567F);
    if (!Same(atCapture, Export(), argv[2], 0)) return 1;
    auto pure = [&](auto operation)
    {
        std::vector<uint8_t> before(sizeof(getGameState()));
        std::memcpy(before.data(), &getGameState(), before.size());
        auto fields = Export(); operation();
        if (std::memcmp(before.data(), &getGameState(), before.size())
            || !Same(fields, Export(), argv[2], 0))
            throw std::runtime_error("save capture/pump changed live state");
    };
    ParkFileExporter exporter;
    pure([&] { exporter.ExportSnapshot(getGameState(), path, manual); });
    if (!ClassicAutosave::Pending()) return 1;
    unsigned frames = 0; bool observedPublication = false;
    while (frames < 400 || (ClassicAutosave::Pending() && frames < 10000))
    {
        gameStateUpdateLogic();
        if (frames < 400)
        {
            auto actual = Export();
            if (frames < uninterrupted.size() && !Same(uninterrupted[frames], actual, argv[2], frames + 1)) return 1;
            if (Signature(actual) != uninterruptedSignatures[frames])
                throw std::runtime_error("autosave disturbed uninterrupted simulation");
        }
        pure([] { ClassicAutosave::Pump(); });
        ++frames;
        if (!manual && !observedPublication && ClassicVfs::Query(path).exists)
        {
            // Publication may precede the final directory flush/cleanup while
            // Pending() remains true. Any visible file must already be whole.
            FILE* visible = std::fopen(ClassicVfs::HostPath(path).c_str(), "rb");
            if (!visible) throw std::runtime_error("visible save cannot be opened");
            MemoryStream publication; std::array<uint8_t, 16384> part;
            for (size_t n; (n = std::fread(part.data(), 1, part.size(), visible));) publication.Write(part.data(), n);
            std::fclose(visible);
            if (!Same(atCapture, publication, argv[2], 0)) return 1;
            observedPublication = true;
        }
    }
    if (ClassicAutosave::Pending()) return 1;
    FILE* file = std::fopen(ClassicVfs::HostPath(path).c_str(), "rb"); if (!file) return 1;
    MemoryStream saved; std::array<uint8_t, 16384> buffer;
    for (size_t n; (n = std::fread(buffer.data(), 1, buffer.size(), file));) saved.Write(buffer.data(), n);
    std::fclose(file);
    // Compare the actual compressed publication BEFORE any load can normalize
    // it. No valuation, measurement, patrol or other persisted field is masked.
    if (!Same(atCapture, saved, argv[2], 0)) return 1;
    Write(std::string(argv[2]) + "-capture.park", atCapture);
    Write(std::string(argv[2]) + "-snapshot.park", saved);
    std::printf("PASS raw capture/publication and untouched live state; capture=%u live=%u pumps=%u\n",
        tick, getGameState().currentTicks, frames);

    // The reference is a NORMAL synchronous save/load, with upstream's load
    // valuation refresh. Keep the former 50 exact/400 digest continuation tests,
    // comparing like load operations rather than comparing a load with no load.
    if (!context->LoadParkFromFile(baseline)) return 1;
    auto expectedLoaded = Export();
    std::vector<MemoryStream> continued;
    std::vector<Signatures> signatures;
    UtilRandSeed(0x1234567F);
    for (unsigned i = 0; i < 400; ++i)
    {
        gameStateUpdateLogic(); auto expected = Export();
        signatures.push_back(Signature(expected));
        if (i < 50) continued.push_back(std::move(expected));
    }
    if (!context->LoadParkFromFile(path)) return 1;
    auto loaded = Export();
    if (!Same(expectedLoaded, loaded, argv[2], 0)) return 1;
    Write(std::string(argv[2]) + "-loaded.park", loaded);
    UtilRandSeed(0x1234567F);
    for (unsigned i = 0; i < signatures.size(); ++i)
    {
        gameStateUpdateLogic(); auto actual = Export();
        if (i < continued.size() && !Same(continued[i], actual, argv[2], i + 1)) return 1;
        if (signatures[i] != Signature(actual)) throw std::runtime_error("continued load differs from synchronous reference");
    }
    std::printf("PASS upstream-style save/load: 50 byte-exact and all 400 continued ticks; mode=%s\n", argv[3]);
}
