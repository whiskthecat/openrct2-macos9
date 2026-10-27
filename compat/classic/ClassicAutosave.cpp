#include "ClassicAutosave.h"
#include "ClassicDiskIo.h"
#include "ClassicVfs.h"
#include <openrct2/core/Console.hpp>
#include <openrct2/core/MemoryStream.h>
#include <openrct2/core/Crypt.h>
#include <openrct2/core/Endianness.h>
#include <zstd.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

namespace ClassicAutosave
{
    namespace
    {
        constexpr uint64_t kChunk = 16 * 1024;
        struct Job
        {
            OpenRCT2::MemoryStream bytes, prefix, encoded;
            std::function<bool(OpenRCT2::MemoryStream&, OpenRCT2::MemoryStream&)> prepare;
            std::unique_ptr<OpenRCT2::Crypt::FNV1aAlgorithm> hash;
            ZSTD_CCtx* compressor = nullptr;
            uint64_t inputAt = 0, inputLength = 0;
            std::function<size_t(const OpenRCT2::MemoryStream&, uint64_t, uint8_t*, size_t)> read;
            bool replace = false, published = false;
            std::unique_ptr<ClassicHost::Flush> flush;
            uint64_t flushStarted = 0;
            ~Job() { if (compressor) ZSTD_freeCCtx(compressor); }
            std::string path, temporary;
            std::FILE* file = nullptr;
            uint64_t written = 0;
            std::function<void(const std::string&)> completed, failed;
        };
        std::unique_ptr<Job> gJob;
        std::function<void()> gNext;
        bool gEnabled = true, gPumping = false;
        uint64_t gCompleted = 0, gFailures = 0, gSerializeUs = 0, gMaxChunkUs = 0, gLastBytes = 0, gCapacity = 0;
        uint64_t gFlushWaitUs = 0, gPeakBytes = 0, gCaptureBytes = 0, gPrepareUs = 0, gEncodeUs = 0, gMaxSliceUs = 0;
        uint32_t gSnapshotTick = 0;
        std::string gLastPath, gError;
        void Close(Job& job)
        {
            if (!job.file) return;
            ClassicDiskIo::Access access(ClassicDiskIo::Op::close, job.temporary);
            const int error = std::fclose(job.file); job.file = nullptr;
            access.End(0, 0, error != 0);
            if (error != 0) throw std::runtime_error("autosave close failed");
        }
        void Fail(const char* error)
        {
            gFailures++; gError = error;
            try { Close(*gJob); } catch (...) {}
            if (!gJob->published) ClassicVfs::Remove(gJob->temporary);
            auto callback = std::move(gJob->failed);
            gJob.reset();
            if (callback) callback(gError);
            OpenRCT2::Console::Error::WriteLine("Could not complete autosave: %s", gError.c_str());
        }
    }
    bool Enabled()
    {
#ifndef OPENRCT2_CLASSIC_MAC
        if (const char* flag = std::getenv("RCT_AUTOSAVE_SYNC"); flag && flag[0] == '1') return false;
#endif
        return gEnabled;
    }
    void SetEnabled(bool enabled) { if (!gJob) gEnabled = enabled; }
    bool Pumping() { return gPumping; }
    bool Pending() { return gJob != nullptr || bool(gNext); }
    void Queue(std::function<void()> request)
    {
        if (gNext) throw std::runtime_error("A manual save is already queued; please wait for it to finish");
        gNext = std::move(request);
    }
    void Begin(std::string path, OpenRCT2::MemoryStream&& bytes, uint64_t serializeUs)
    {
        if (gJob) throw std::runtime_error("autosave already pending");
        // Same-second saves must never overwrite a previous good save. Native
        // publication also rejects a destination created after this check.
        const std::string original = path;
        unsigned suffix = 0;
        while (ClassicVfs::Query(path).exists)
        {
            if (++suffix > 999) throw std::runtime_error("no unique autosave name");
            const auto dot = original.rfind('.');
            path = original.substr(0, dot) + "_" + std::to_string(suffix) + original.substr(dot);
        }
        auto job = std::make_unique<Job>();
        job->path = path; job->temporary = path + ".tmp";
        job->bytes = std::move(bytes);
        gSerializeUs = serializeUs; gLastBytes = job->bytes.GetLength(); gCapacity = job->bytes.GetCapacity();
        gLastPath = path; gError.clear(); gMaxChunkUs = 0;
        gJob = std::move(job);
    }
    void BeginExport(std::string path, std::function<bool(OpenRCT2::MemoryStream&, OpenRCT2::MemoryStream&)> prepare,
                     uint64_t captureBytes, uint64_t captureUs, uint32_t tick, bool replace,
                     std::function<size_t(const OpenRCT2::MemoryStream&, uint64_t, uint8_t*, size_t)> read)
    {
        // Begin's unique-name policy is for autosaves. Manual publication is an
        // atomic replacement at the requested name, preserving the old file up
        // to that point. Never drain another job in a play frame.
        if (gJob) throw std::runtime_error("A save is already in progress; please retry when it finishes");
        if (!replace) Begin(std::move(path), OpenRCT2::MemoryStream(), captureUs);
        else
        {
            auto job = std::make_unique<Job>(); job->path = path; job->temporary = path + ".tmp";
            gSerializeUs = captureUs; gLastBytes = gCapacity = gMaxChunkUs = 0;
            gLastPath = path; gError.clear(); gJob = std::move(job);
        }
        gJob->read = std::move(read); gJob->prepare = std::move(prepare); gJob->replace = replace;
        gPeakBytes = gCaptureBytes = captureBytes; gSnapshotTick = tick; gPrepareUs = gEncodeUs = gMaxSliceUs = gFlushWaitUs = 0;
    }
    void OnFailure(std::function<void(const std::string&)> callback) { if (gJob) gJob->failed = std::move(callback); }
    void OnSuccess(std::function<void(const std::string&)> callback) { if (gJob) gJob->completed = std::move(callback); }
    void Prune(const std::string& directory, unsigned keep, const std::string& newest)
    {
        std::vector<std::string> paths;
        for (const auto& entry : ClassicVfs::List(directory))
            if (!entry.isDirectory && entry.name.starts_with("autosave_") && entry.name.ends_with(".park"))
                paths.push_back(directory + "/" + entry.name);
        keep = std::max(1u, keep);
        if (paths.size() <= keep) return;
        std::sort(paths.begin(), paths.end());
        const auto protectedHost = ClassicVfs::HostPath(newest);
        const auto latest = std::find_if(paths.begin(), paths.end(), [&](const auto& path) {
            return ClassicVfs::HostPath(path) == protectedHost;
        });
        // Same-second suffixes, a clock adjustment, or HFS shortened names need
        // not sort last. Never prune the file that was just made durable.
        if (latest == paths.end()) return; // An unusual listing must not risk old saves.
        std::rotate(latest, latest + 1, paths.end());
        for (size_t i = 0; i < paths.size() - keep; i++)
            if (!ClassicVfs::Remove(paths[i]))
                OpenRCT2::Console::Error::WriteLine("Could not prune autosave: %s", paths[i].c_str());
    }
    void Pump()
    {
        struct PumpScope { PumpScope() { gPumping = true; } ~PumpScope() { gPumping = false; } } pumpScope;
        if (!gJob)
        {
            if (gNext)
            {
                auto request = std::move(gNext); gNext = {};
                try { request(); }
                catch (const std::exception& error)
                {
                    gFailures++; gError = error.what();
                    OpenRCT2::Console::Error::WriteLine("Queued save failed: %s", gError.c_str());
                }
            }
            return;
        }
        ClassicDiskIo::Scope io("autosave-chunk");
        auto& job = *gJob;
        try
        {
            if (job.prepare)
            {
                const auto start = ClassicHost::Microseconds();
                const bool done = job.prepare(job.bytes, job.prefix);
                const auto us = ClassicHost::Microseconds() - start;
                gPrepareUs += us; gMaxSliceUs = std::max(gMaxSliceUs, us);
                if (done)
                {
                    job.prepare = {}; // Releases every captured live input.
                    job.hash = OpenRCT2::Crypt::CreateFNV1a();
                    gPeakBytes = std::max(gPeakBytes, gCaptureBytes + job.bytes.GetCapacity());
                    gCapacity = job.bytes.GetCapacity();
                    job.prefix.SetPosition(16); job.inputLength = job.prefix.ReadValue<uint64_t>();
                }
                gPeakBytes = std::max(gPeakBytes, gCaptureBytes + (job.bytes.GetCapacity() > gCapacity ? job.bytes.GetCapacity() - gCapacity : 0));
                return;
            }
            if (job.hash)
            {
                const auto start = ClassicHost::Microseconds();
                const auto check = [](size_t result) {
                    if (ZSTD_isError(result)) throw std::runtime_error(ZSTD_getErrorName(result));
                };
                if (!job.compressor)
                {
                    job.compressor = ZSTD_createCCtx();
                    if (!job.compressor) throw std::bad_alloc();
                    // Fast compression with small independently flushed blocks
                    // bounds the work of a call. It remains ordinary PARK zstd.
                    check(ZSTD_CCtx_setParameter(job.compressor, ZSTD_c_compressionLevel, 1));
                    check(ZSTD_CCtx_setParameter(job.compressor, ZSTD_c_windowLog, 16));
                    check(ZSTD_CCtx_setParameter(job.compressor, ZSTD_c_contentSizeFlag, 0));
                    check(ZSTD_CCtx_setPledgedSrcSize(job.compressor, job.inputLength));
                    job.encoded = OpenRCT2::MemoryStream(ZSTD_compressBound(job.bytes.GetLength()) + job.inputLength / 8192 * 32 + 262144);
                }
                std::array<uint8_t, 65536> source;
                const auto size = job.read ? job.read(job.bytes, job.inputAt, source.data(), source.size())
                    : std::min<uint64_t>(8192, job.inputLength - job.inputAt);
                if (!size || size > job.inputLength - job.inputAt) throw std::runtime_error("Invalid save stream slice");
                const auto* at = job.read ? source.data() : static_cast<const uint8_t*>(job.bytes.GetData()) + job.inputAt;
                job.hash->Update(at, size);
                ZSTD_inBuffer input{at, static_cast<size_t>(size), 0};
                const bool last = job.inputAt + size == job.inputLength;
                const auto directive = last ? ZSTD_e_end : ZSTD_e_flush;
                size_t remaining;
                do
                {
                    std::array<uint8_t, 16384> buffer;
                    ZSTD_outBuffer output{buffer.data(), buffer.size(), 0};
                    remaining = ZSTD_compressStream2(job.compressor, &output, &input, directive); check(remaining);
                    job.encoded.Write(buffer.data(), output.pos);
                } while (input.pos < input.size || remaining);
                gPeakBytes = std::max<uint64_t>(gPeakBytes, job.bytes.GetCapacity() + job.encoded.GetCapacity()
                    + job.prefix.GetCapacity() + ZSTD_sizeof_CCtx(job.compressor) + 512 * 1024);
                job.inputAt += size;
                if (last)
                {
                    const auto hash = job.hash->Finish();
                    // PARK's fixed 64-byte header uses explicit little endian
                    // sizes/type. The directory and chunk bytes are unchanged.
                    const bool compressed = bool(job.read) || job.encoded.GetLength() < job.bytes.GetLength();
                    job.prefix.SetPosition(24); job.prefix.WriteValue(uint32_t(compressed ? 2 : 0));
                    job.prefix.WriteValue(uint64_t(compressed ? job.encoded.GetLength() : job.bytes.GetLength()));
                    job.prefix.Write(hash.data(), hash.size());
                    if (compressed) job.bytes = std::move(job.encoded);
                    else job.encoded = OpenRCT2::MemoryStream();
                    job.read = {}; job.hash.reset(); ZSTD_freeCCtx(job.compressor); job.compressor = nullptr;
                    gLastBytes = job.prefix.GetLength() + job.bytes.GetLength();
                }
                const auto us = ClassicHost::Microseconds() - start;
                gEncodeUs += us; gMaxSliceUs = std::max(gMaxSliceUs, us);
                return;
            }
            if (!job.file && !job.published)
            {
                const auto slash = job.temporary.find_last_of('/');
                if (!ClassicVfs::MakeDirectories(job.temporary.substr(0, slash))) throw std::runtime_error("autosave directory failed");
                const auto host = ClassicVfs::HostPath(job.temporary);
                ClassicDiskIo::Access access(ClassicDiskIo::Op::open, job.temporary);
                job.file = host.empty() ? nullptr : std::fopen(host.c_str(), "wb");
                access.End(0, 0, job.file == nullptr);
                if (!job.file) throw std::runtime_error("autosave open failed");
                // One library transfer per chunk, rather than Newlib's 1 KiB write buffer.
                std::setvbuf(job.file, nullptr, _IONBF, 0);
            }
            if (job.written < job.bytes.GetLength() + job.prefix.GetLength())
            {
                const auto bytes = std::min(kChunk, job.bytes.GetLength() + job.prefix.GetLength() - job.written);
                std::array<uint8_t, kChunk> transfer;
                const uint8_t* at;
                if (job.written < job.prefix.GetLength())
                {
                    const auto prefixBytes = std::min<uint64_t>(bytes, job.prefix.GetLength() - job.written);
                    std::memcpy(transfer.data(), static_cast<const uint8_t*>(job.prefix.GetData()) + job.written, prefixBytes);
                    std::memcpy(transfer.data() + prefixBytes, job.bytes.GetData(), bytes - prefixBytes);
                    at = transfer.data();
                }
                else at = static_cast<const uint8_t*>(job.bytes.GetData()) + job.written - job.prefix.GetLength();
                const auto start = ClassicHost::Microseconds();
                ClassicDiskIo::Access access(ClassicDiskIo::Op::write, job.temporary);
                const size_t count = std::fwrite(at, 1, static_cast<size_t>(bytes), job.file);
                access.End(count, job.written, count != bytes);
                gMaxChunkUs = std::max(gMaxChunkUs, ClassicHost::Microseconds() - start);
                if (count != bytes || std::ferror(job.file)) throw std::runtime_error("autosave write failed");
                job.written += count;
                return; // Never combine a data chunk with final flush/publication in one frame.
            }
            ClassicDiskIo::Scope publish("autosave-publish");
            if (!job.flush)
            {
                const auto& path = job.published ? job.path : job.temporary;
                ClassicDiskIo::Access access(ClassicDiskIo::Op::flush, path);
                if (!job.published && (std::fflush(job.file) != 0 || std::ferror(job.file)))
                    throw std::runtime_error("save buffered flush failed");
                job.flushStarted = ClassicHost::Microseconds();
                job.flush = ClassicHost::BeginFlush(job.published ? nullptr : job.file, ClassicVfs::HostPath(path));
                access.End(0, 0, !job.flush);
                if (!job.flush) throw std::runtime_error("save flush request failed");
                return;
            }
            if (!job.flush->Ready()) return;
            gFlushWaitUs += ClassicHost::Microseconds() - job.flushStarted;
            if (!job.flush->Good()) throw std::runtime_error("save durable flush failed");
            job.flush.reset();
            if (!job.published)
            {
                Close(job);
                if (!ClassicVfs::Publish(job.temporary, job.path, job.replace)) throw std::runtime_error("save publication failed");
                job.published = true;
                return; // Directory flush is a later asynchronous request.
            }
            // HFS exchange leaves the old complete file at the temporary name.
            // Remove it only after the new target's publication is durable.
            if (job.replace && ClassicVfs::Query(job.temporary).exists) ClassicVfs::Remove(job.temporary);
            auto callback = std::move(job.completed);
            auto publishedPath = std::move(job.path);
            gJob.reset(); gCompleted++;
            if (callback) callback(publishedPath); // Only after the new durable save is visible.
        }
        catch (const std::exception& e)
        {
            if (gJob) Fail(e.what());
            else { gError = e.what(); OpenRCT2::Console::Error::WriteLine("Autosave retention: %s", e.what()); }
        }
    }
    bool Drain()
    {
        const auto failures = gFailures;
        while (Pending()) { Pump(); ClassicHost::Yield(0); }
        return failures == gFailures;
    }
    std::string Status()
    {
        return nlohmann::json{{"pending", Pending()}, {"queued", bool(gNext)}, {"completed", gCompleted}, {"failures", gFailures},
            {"snapshot_tick", gSnapshotTick}, {"capture_bytes", gCaptureBytes}, {"peak_buffer_bytes", gPeakBytes}, {"prepare_us", gPrepareUs},
            {"encode_us", gEncodeUs}, {"max_slice_us", gMaxSliceUs}, {"flush_wait_us", gFlushWaitUs},
            {"phase", gJob ? (gJob->prepare ? "fields" : gJob->hash ? "encode" : (gJob->flush || gJob->published) ? "flush" : "write") : "idle"},
            {"snapshot_bytes", gLastBytes}, {"snapshot_capacity", gCapacity}, {"serialize_us", gSerializeUs},
            {"written", gJob ? gJob->written : 0}, {"chunk_bytes", kChunk}, {"max_chunk_us", gMaxChunkUs},
            {"path", gLastPath}, {"error", gError}}.dump();
    }
}
