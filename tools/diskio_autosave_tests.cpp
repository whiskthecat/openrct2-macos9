#ifdef NDEBUG
#undef NDEBUG
#endif
// Focus on failure boundaries: partial data must never replace a good file,
// publication rejects a race, and successful files contain the entire snapshot.
#include "ClassicAutosave.h"
#include "BuildIdentity.h"
#include "ClassicVfs.h"
#include <openrct2/core/MemoryStream.h>
#include <cassert>
#include <csignal>
#include <fstream>
#include <iterator>
#include <sys/resource.h>
#include <vector>
#include <stdexcept>
#include <array>
#include <zstd.h>
#include <unistd.h>
#include <sys/wait.h>

using OpenRCT2::MemoryStream;
static constexpr const char* path = "/RCT/user/diskio-tests/autosave.park";
static std::vector<uint8_t> Read(const char* name)
{
    std::ifstream f(ClassicVfs::HostPath(name), std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}
static MemoryStream Snapshot(const std::vector<uint8_t>& bytes) { return MemoryStream(bytes); }
static void StartExport(const std::vector<uint8_t>& body, bool fail = false)
{
    ClassicAutosave::BeginExport(path, [body, fail](MemoryStream& bytes, MemoryStream& prefix) {
        if (fail) throw std::runtime_error("injected preparation failure");
        bytes = MemoryStream(body);
        std::array<uint8_t, 64> header{}; prefix.Write(header.data(), header.size());
        prefix.SetPosition(16); prefix.WriteValue(uint64_t(body.size()));
        return true;
    }, body.size(), 0, 1, true);
}
static std::vector<uint8_t> Decode()
{
    auto bytes = Read(path); MemoryStream header(bytes); header.SetPosition(16);
    const auto size = header.ReadValue<uint64_t>(); const auto kind = header.ReadValue<uint32_t>();
    std::vector<uint8_t> raw(size);
    if (kind == 2) assert(ZSTD_decompress(raw.data(), raw.size(), bytes.data() + 64, bytes.size() - 64) == raw.size());
    else { assert(kind == 0 && bytes.size() == size + 64); std::copy(bytes.begin() + 64, bytes.end(), raw.begin()); }
    return raw;
}
int main()
{
    if (!RctBuild::Start()) return 125;
    bool allocationFailed = false;
    try { MemoryStream impossible(SIZE_MAX); } catch (const std::bad_alloc&) { allocationFailed = true; }
    assert(allocationFailed);
    ClassicVfs::Initialise();
    ClassicVfs::MakeDirectories("/RCT/user/diskio-tests");
    for (const auto& entry : ClassicVfs::List("/RCT/user/diskio-tests"))
        ClassicVfs::Remove(std::string("/RCT/user/diskio-tests/") + entry.name);
    std::vector<uint8_t> expected(3 * 16384 + 211);
    for (size_t i = 0; i < expected.size(); i++) expected[i] = (i * 17 + 5) & 255;
    bool pruned = false;
    ClassicAutosave::Begin(path, Snapshot(expected), 123);
    ClassicAutosave::OnSuccess([&](const std::string&) { pruned = true; });
    ClassicAutosave::Pump();
    assert(ClassicAutosave::Pending() && !ClassicVfs::Query(path).exists && !pruned);
    assert(ClassicVfs::Query(std::string(path) + ".tmp").size == 16384);
    ClassicAutosave::Drain();
    assert(!ClassicAutosave::Pending() && pruned && Read(path) == expected);
    // Simulated write failure: the kernel refuses growth beyond a partial chunk.
    struct rlimit original{}, limited{};
    assert(getrlimit(RLIMIT_FSIZE, &original) == 0);
    limited = original; limited.rlim_cur = 8192;
    std::signal(SIGXFSZ, SIG_IGN);
    assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
    pruned = false;
    ClassicAutosave::Begin(path, Snapshot(expected), 123);
    ClassicAutosave::OnSuccess([&](const std::string&) { pruned = true; });
    ClassicAutosave::Drain();
    assert(setrlimit(RLIMIT_FSIZE, &original) == 0);
    assert(!pruned && Read(path) == expected && !ClassicVfs::Query("/RCT/user/diskio-tests/autosave_1.park").exists);
    // A competing completed target appears after the name was chosen.
    ClassicAutosave::Begin(path, Snapshot(expected), 123);
    const auto collision = ClassicVfs::HostPath("/RCT/user/diskio-tests/autosave_1.park");
    { std::ofstream f(collision); f << "existing-good-save"; }
    ClassicAutosave::Drain();
    assert(Read("/RCT/user/diskio-tests/autosave_1.park") == std::vector<uint8_t>({'e','x','i','s','t','i','n','g','-','g','o','o','d','-','s','a','v','e'}));
    assert(Read(path) == expected);
    assert(!ClassicVfs::Query("/RCT/user/diskio-tests/autosave_1.park.tmp").exists);
    // Repeated same-second saves choose a new name and publish without changing either older file.
    ClassicAutosave::Begin(path, Snapshot(expected), 123);
    ClassicAutosave::Drain();
    assert(Read("/RCT/user/diskio-tests/autosave_2.park") == expected);
    assert(Read(path) == expected);
    // With one retained slot, a reused unsuffixed name sorts BEFORE the older
    // suffix. Pruning must protect the just-published save, not lexical order.
    const std::string dir = "/RCT/user/diskio-tests/retention";
    const std::string originalName = dir + "/autosave_20261008-000000.park";
    ClassicVfs::MakeDirectories(dir);
    for (const auto& entry : ClassicVfs::List(dir)) ClassicVfs::Remove(dir + "/" + entry.name);
    for (unsigned generation = 0; generation < 3; generation++)
    {
        expected[0] = static_cast<uint8_t>(generation);
        std::string published;
        ClassicAutosave::Begin(originalName, Snapshot(expected), 123);
        ClassicAutosave::OnSuccess([&](const std::string& latest) {
            published = latest; ClassicAutosave::Prune(dir, 1, latest);
        });
        ClassicAutosave::Drain();
        assert(Read(published.c_str()) == expected && ClassicVfs::List(dir).size() == 1);
        const auto expectedName = generation == 1 ? dir + "/autosave_20261008-000000_1.park" : originalName;
        assert(published == expectedName);
    }
    // Filename time order can move backwards too; it must not delete the new snapshot.
    const auto backwardName = dir + "/autosave_19991008-000000.park";
    ClassicAutosave::Begin(backwardName, Snapshot(expected), 123);
    ClassicAutosave::OnSuccess([&](const std::string& latest) { ClassicAutosave::Prune(dir, 0, latest); });
    ClassicAutosave::Drain();
    assert(Read(backwardName.c_str()) == expected && ClassicVfs::List(dir).size() == 1);
    // Preparation exceptions cannot replace an existing manual save.
    const auto previous = Read(path);
    bool failed = false;
    StartExport(expected, true); ClassicAutosave::OnFailure([&](const std::string&) { failed = true; });
    assert(!ClassicAutosave::Drain()); assert(failed && Read(path) == previous);
    // Incompressible data forces several actual write transfers.
    uint32_t random = 123;
    for (auto& byte : expected) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; byte = random; }
    StartExport(expected);
    while (ClassicAutosave::Status().find("\"phase\":\"write\"") == std::string::npos) ClassicAutosave::Pump();
    assert(Read(path) == previous);
    assert(setrlimit(RLIMIT_FSIZE, &limited) == 0); failed = false;
    ClassicAutosave::OnFailure([&](const std::string&) { failed = true; }); assert(!ClassicAutosave::Drain());
    assert(setrlimit(RLIMIT_FSIZE, &original) == 0);
    assert(failed && Read(path) == previous && !ClassicVfs::Query(std::string(path) + ".tmp").exists);
    // A process exits with a partial temporary file: the old target stays exact.
    const auto child = fork(); assert(child >= 0);
    if (!child)
    {
        StartExport(expected);
        while (ClassicAutosave::Status().find("\"phase\":\"write\"") == std::string::npos) ClassicAutosave::Pump();
        ClassicAutosave::Pump(); _exit(0);
    }
    int status = 0; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(Read(path) == previous && ClassicVfs::Query(std::string(path) + ".tmp").exists);
    // Normal quit drains both an active save and one explicitly queued request.
    unsigned callbacks = 0; StartExport(expected); ClassicAutosave::OnSuccess([&](const std::string&) { callbacks++; });
    auto next = expected; next[0] ^= 0xFF;
    ClassicAutosave::Queue([&] { assert(Decode() == expected); StartExport(next); ClassicAutosave::OnSuccess([&](const std::string&) { callbacks++; }); });
    ClassicAutosave::Drain(); assert(callbacks == 2 && Decode() == next && !ClassicAutosave::Pending());
    assert(!ClassicVfs::Query(std::string(path) + ".tmp").exists);
    std::printf("PASS manual atomic replacement, preparation failure, partial-write failure, process interruption, queued-quit drain\n");
    std::printf("PASS chunk visibility, exact bytes, checked drain, partial-write failure, publication race, same-name retention, newest-save protection\n");
}
