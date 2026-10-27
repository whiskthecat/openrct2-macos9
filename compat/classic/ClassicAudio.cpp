// Sound for the Mac OS 9 build.
//
// The engine plays a sound by asking a mixer for a channel on a source and then setting volume,
// pan and rate on the channel. On other platforms a software mixer turns that into one stream.
// The legacy backend uses one Sound Manager channel per voice. The optional application
// backend mixes fixed-point PCM/IMA4 into two short buffers on one output channel.
//
// Sources come from the pack already converted (tools/mkpak.py --audio-cache): 'twos' (16-bit
// big-endian samples) or 'ima4' (mono), behind a small 'RCTA' header. Plain WAV and the original
// sound bank format are accepted too. Ogg/FLAC are not: such a source is simply silent.
//
// On other hosts the same code runs without a device, so the logic can be exercised there.
#include "ClassicAudio.h"
#include "ClassicSoftwareMixer.h"
#include "ClassicMixerStream.h"

#include "ClassicDiskIo.h"
#include "ClassicLoadProgress.h"
#include "ClassicPreload.h"
#include "ClassicVfs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <openrct2/Context.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/audio/AudioChannel.h>
#include <openrct2/audio/AudioContext.h>
#include <openrct2/audio/AudioMixer.h>
#include <openrct2/audio/AudioSource.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/FileStream.h>
#include <openrct2/core/FrameTiming.h>
#include <openrct2/core/IStream.hpp>
#include <openrct2/core/ScopeProbe.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/interface/Window.h>
#include <openrct2/interface/WindowBase.h>
#include <openrct2/object/MusicObject.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/ride/RideManager.hpp>
#include <span>
#include <vector>

#ifdef OPENRCT2_CLASSIC_MAC
    #include "ClassicVfs.h"

    #include "../sdl/ClassicInterruptMask.h"
    #include <Sound.h>
    #include <CodeFragments.h>
    #include <DriverServices.h>
#else
    #include <chrono>
#endif

namespace OpenRCT2::Audio
{
    namespace
    {
#ifdef OPENRCT2_SERVICES_AUDIO_COUNTS
        uint64_t gPanCalls = 0, gPanRepeated = 0, gOffsetCalls = 0;
#endif
        uint64_t NowMicroseconds()
        {
#ifdef OPENRCT2_CLASSIC_MAC
            return ClassicHost::Microseconds();
#else
            static const auto start = std::chrono::steady_clock::now();
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
#endif
        }

        uint32_t ReadBE32(const uint8_t* p)
        {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }
        uint32_t ReadLE32(const uint8_t* p)
        {
            return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
        }
        uint16_t ReadLE16(const uint8_t* p)
        {
            return static_cast<uint16_t>((p[1] << 8) | p[0]);
        }

        struct SampleArena { std::unique_ptr<uint8_t[]> bytes; size_t capacity; };
        struct SampleBlock
        {
            std::shared_ptr<SampleArena> arena;
            size_t offset = 0, size = 0, filled = 0;
            uint8_t* Bytes() const { return arena->bytes.get() + offset; }
        };
        struct CachedSample
        {
            std::shared_ptr<SampleBlock> block;
            bool compressed = false;
            uint32_t rate = 0, frames = 0;
            uint16_t channels = 0;
            size_t size = 0;
            uint64_t lastUse = 0;
            bool failed = false;
        };
        std::map<std::string, CachedSample> gSampleCache;
        std::shared_ptr<SampleArena> gSampleArena;
        std::deque<std::string> gPrepareQueue;
        bool gPreloadComplete = false, gPreloading = false, gFullPreload = false;
        int gModeOverride = -1;
        unsigned gPreloadMode = 2;
        uint64_t gCacheBytes = 0, gCacheHits = 0, gAge = 0, gEvictions = 0, gPrepareReads = 0, gDemandReads = 0;
        ClassicHost::MemoryInfo gAdmission;
        uint64_t gFrameStart = 0;
        uint32_t gLastScanTick = 0;
        std::string CacheKey(std::string key)
        {
            for (auto& c : key)
            {
                if (c == '\\') c = '/';
                if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            }
            if (!key.starts_with("/rct/game/") && !key.starts_with("/rct/openrct2/") && !key.starts_with("/rct/rct1/")) key.clear();
            return key;
        }
        bool Ready(const CachedSample& sample) { return sample.block && sample.block->filled == sample.size; }
        std::shared_ptr<SampleBlock> Reserve(CachedSample& sample, bool evict)
        {
            if (sample.block) return sample.block;
            if (!gSampleArena || sample.size > gSampleArena->capacity) return nullptr;
            while (true)
            {
                std::array<std::pair<size_t, size_t>, 256> ranges{}; size_t count = 0;
                for (const auto& [key, entry] : gSampleCache)
                    if (entry.block)
                    {
                        if (count == ranges.size()) return nullptr;
                        ranges[count++] = {entry.block->offset, entry.block->offset + entry.block->size};
                    }
                std::sort(ranges.begin(), ranges.begin() + count);
                size_t start = 0;
                for (size_t i = 0; i < count; i++)
                {
                    if (ranges[i].first - start >= sample.size) break;
                    start = ranges[i].second;
                }
                if (sample.size <= gSampleArena->capacity - start)
                {
                    sample.block = std::make_shared<SampleBlock>(SampleBlock{gSampleArena, start, sample.size, 0});
                    sample.lastUse = ++gAge; return sample.block;
                }
                if (!evict) return nullptr;
                auto victim = gSampleCache.end();
                for (auto it = gSampleCache.begin(); it != gSampleCache.end(); ++it)
                    if (it->second.block && it->second.block.use_count() == 1
                        && (victim == gSampleCache.end() || it->second.lastUse < victim->second.lastUse)) victim = it;
                if (victim == gSampleCache.end()) return nullptr; // Sound Manager references are pinned.
                gCacheBytes -= victim->second.block->filled; victim->second.block.reset(); gEvictions++;
            }
        }
        bool Fill(const std::string& key, CachedSample& sample, size_t limit, bool evict)
        {
            if (sample.failed) return false;
            if (Ready(sample)) { sample.lastUse = ++gAge; return true; }
            auto block = Reserve(sample, evict); if (!block) return false;
            const size_t bytes = std::min(limit, sample.size - block->filled);
            ClassicDiskIo::Scope scope(gPreloading ? "audio-prepare" : "audio-first-play");
            const size_t n = ClassicVfs::ReadPack(key, 20 + block->filled, block->Bytes() + block->filled, bytes);
            if (gPreloading) gPrepareReads++; else gDemandReads++;
            block->filled += n; gCacheBytes += n; sample.lastUse = ++gAge;
            if (n != bytes) { gCacheBytes -= block->filled; sample.block.reset(); sample.failed = true; return false; }
            return Ready(sample);
        }
        bool Register(const std::string& key, const uint8_t* head, size_t size)
        {
            if (key.empty() || size <= 20 || std::memcmp(head, "RCTA", 4) != 0) return false;
            auto& sample = gSampleCache[key];
            sample.compressed = std::memcmp(head + 4, "ima4", 4) == 0;
            sample.rate = ReadBE32(head + 8); sample.channels = 0;
            sample.channels = static_cast<uint16_t>((head[12] << 8) | head[13]);
            sample.frames = ReadBE32(head + 16); sample.size = size - 20;
            return sample.rate != 0 && sample.channels != 0 && sample.frames != 0;
        }
        void Queue(const std::string& key)
        {
            auto found = gSampleCache.find(key);
            if (found == gSampleCache.end() || found->second.failed || Ready(found->second) || !gSampleArena || found->second.size > gSampleArena->capacity) return;
            if (std::find(gPrepareQueue.begin(), gPrepareQueue.end(), key) == gPrepareQueue.end()) gPrepareQueue.push_back(key);
        }

        class ClassicSource final : public IAudioSource
        {
        public:
            std::vector<uint8_t> data;
            bool compressed = false;
            uint32_t rate = 22050;
            uint16_t channels = 1;
            uint32_t frames = 0;
            bool released = false;
            mutable std::unique_ptr<IStream> pending;
            mutable std::shared_ptr<SampleBlock> cachedData;
            std::string cacheKey;
            std::span<const uint8_t> Bytes() const
            {
                if (cachedData && cachedData->filled == cachedData->size) return {cachedData->Bytes(), cachedData->size};
                return data;
            }
            void EnsureData() const
            {
                if (cachedData || !data.empty())
                {
                    if (auto found = gSampleCache.find(cacheKey); found != gSampleCache.end()) found->second.lastUse = ++gAge;
                    return;
                }
                if (auto found = gSampleCache.find(cacheKey); found != gSampleCache.end())
                {
                    if (Ready(found->second)) gCacheHits++;
                    if (Fill(cacheKey, found->second, found->second.size, true))
                    {
                        cachedData = found->second.block; pending.reset(); return;
                    }
                    if (found->second.failed) { pending.reset(); return; }
                    // A sample larger than the bounded cache still plays normally. Its
                    // buffer exists only for the source/voice lifetime, as before.
                    auto& bytes = const_cast<std::vector<uint8_t>&>(data); bytes.resize(found->second.size);
                    ClassicDiskIo::Scope io("audio-first-play"); gDemandReads++;
                    if (ClassicVfs::ReadPack(cacheKey, 20, bytes.data(), bytes.size()) != bytes.size())
                    { bytes.clear(); found->second.failed = true; }
                    pending.reset(); return;
                }
                if (!pending) return;
                ClassicDiskIo::Scope io("audio-first-play"); gDemandReads++;
                auto& bytes = const_cast<std::vector<uint8_t>&>(data);
                bytes.resize(pending->GetLength() > 20 ? pending->GetLength() - 20 : 0);
                pending->SetPosition(20);
                if (!bytes.empty() && pending->TryRead(bytes.data(), bytes.size()) != bytes.size()) bytes.clear();
                pending.reset();
            }

            void Release() override
            {
                released = true;
            }
            bool IsReleased() const override
            {
                return released;
            }
            // The engine measures positions in bytes of 16-bit samples.
            int32_t GetBytesPerSecond() const override
            {
                return static_cast<int32_t>(rate * channels * 2);
            }
            uint64_t GetLength() const override
            {
                return static_cast<uint64_t>(frames) * channels * 2;
            }
            size_t Read(void*, uint64_t, size_t) override
            {
                return 0; // nothing here mixes in software
            }

            // 16-bit little-endian samples (WAV, sound bank) into our big-endian form.
            void TakeLittleEndian(const uint8_t* samples, size_t bytes, int bits)
            {
                if (bits == 8)
                {
                    data.resize(bytes * 2);
                    for (size_t i = 0; i < bytes; i++)
                    {
                        data[i * 2] = static_cast<uint8_t>(samples[i] ^ 0x80);
                        data[i * 2 + 1] = 0;
                    }
                    frames = static_cast<uint32_t>(bytes / channels);
                    return;
                }
                data.resize(bytes & ~size_t(1));
                for (size_t i = 0; i + 1 < bytes; i += 2)
                {
                    data[i] = samples[i + 1];
                    data[i + 1] = samples[i];
                }
                frames = static_cast<uint32_t>(bytes / 2 / channels);
            }
        };

        std::unique_ptr<ClassicSource> ParseSource(const std::vector<uint8_t>& file)
        {
            auto source = std::make_unique<ClassicSource>();
            const uint8_t* p = file.data();
            const size_t size = file.size();
            if (size >= 20 && std::memcmp(p, "RCTA", 4) == 0)
            {
                source->compressed = std::memcmp(p + 4, "ima4", 4) == 0;
                source->rate = ReadBE32(p + 8);
                source->channels = static_cast<uint16_t>((p[12] << 8) | p[13]);
                source->frames = ReadBE32(p + 16);
                source->data.assign(p + 20, p + size);
                return source;
            }
            if (size >= 44 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WAVE", 4) == 0)
            {
                int bits = 16;
                for (size_t at = 12; at + 8 <= size;)
                {
                    const uint32_t chunkSize = ReadLE32(p + at + 4);
                    if (std::memcmp(p + at, "fmt ", 4) == 0 && at + 24 <= size)
                    {
                        if (ReadLE16(p + at + 8) != 1)
                            return nullptr; // only plain PCM
                        source->channels = ReadLE16(p + at + 10);
                        source->rate = ReadLE32(p + at + 12);
                        bits = ReadLE16(p + at + 22);
                    }
                    else if (std::memcmp(p + at, "data", 4) == 0)
                    {
                        const size_t bytes = std::min<size_t>(chunkSize, size - at - 8);
                        source->TakeLittleEndian(p + at + 8, bytes, bits);
                        return source;
                    }
                    at += 8 + chunkSize + (chunkSize & 1);
                }
            }
            return nullptr;
        }

        std::vector<uint8_t> ReadAll(IStream& stream)
        {
            std::vector<uint8_t> bytes(static_cast<size_t>(stream.GetLength()));
            stream.SetPosition(0);
            if (!bytes.empty() && stream.TryRead(bytes.data(), bytes.size()) != bytes.size())
                bytes.clear();
            return bytes;
        }

        // --- voices: Sound Manager channels, created once and reused -----------------------
        constexpr int kVoiceCount = 16;
        struct Voice
        {
            bool busy = false;
            bool compressed = false;
            uint32_t rate = 0;
            unsigned channels = 0, left = 0, right = 0;
            double multiplier = 1;
            uint32_t mixGeneration = 0, mixLeft = ~0u, mixRight = ~0u, mixRate = ~0u;
            volatile long generation = 0; // changes whenever the voice is stopped or reused
            volatile long finished = 0;
            volatile long looping = 0;
#ifdef OPENRCT2_CLASSIC_MAC
            SndChannelPtr channel = nullptr;
            union Header
            {
                ExtSoundHeader plain;
                CmpSoundHeader compressed;
            } first, whole; // "first" may start part-way in; "whole" is what a loop repeats
#endif
        };
        Voice gVoices[kVoiceCount];
        bool gDeviceOpen = false;
        volatile bool gClosing = false;
        unsigned long gAllocated = 0, gDisposed = 0;
        unsigned long gStarted = 0, gRefused = 0, gSourcesLoaded = 0, gSourcesFailed = 0, gPlayRequests = 0;
        // Safety limit on everything we send to the Sound Manager, on top of the game's own volume
        // settings: some machines are very loud at low system volume. 0 means no sound at all (no
        // voice is ever started). Set from the argument file: --volume=NN (percent) or --mute.
        float gOutputLimit = 0.25f;
        volatile bool gSilentPlaybackTest = false;
        uint64_t gSilentPlaybackDeadline = 0;
        volatile unsigned long gCallbacks = 0;
        unsigned gPeakGainUnits = 0, gVolumeCommandErrors = 0, gSilentStarts = 0;
        unsigned gMixerBackend = 0; // 0 legacy; 1 22050 stereo; 2 22050 mono; 3 44100 stereo
        unsigned gMixerBits = 16, gMixerBlockFrames = 1024;
        unsigned gProbeMode = 0;
        std::unique_ptr<ClassicSoftwareMixer::Stream> gSoftwareStream;
        volatile unsigned long gMixedBlocks = 0, gMixedFrames = 0, gQueueErrors = 0, gUnderruns = 0;
        uint64_t gMixMicros = 0;
        unsigned long gRebuiltBlocks = 0, gReplacementDeferrals = 0;
        uint64_t gLastServiceEnd = 0, gMaxProducerGap = 0;
        volatile unsigned gMinimumReadyBlocks = ~0u;
        bool gTraceAudio = false;
        volatile uint32_t gCallbackTicks = 0, gMaxCallbackTicks = 0, gCallbackSamples = 0;
        volatile uint32_t gCallbackLastEntry = 0, gCallbackIntervals = 0;
        volatile uint32_t gCallbackIntervalMin = ~0u, gCallbackIntervalMax = 0;
        volatile uint64_t gCallbackIntervalTicks = 0;
        volatile unsigned gCallbackLevel = 0, gCallbackMask = 0;
        uint32_t gHardwareFrames = 0, gHardwareRateFixed = 0;
        unsigned gHardwareBits = 0, gHardwareChannels = 0;
        int gHardwareQueryError = -1;
        volatile bool gOutputMuted = false, gStreamWanted = false;
        volatile unsigned gLimitQ16 = 16384, gSubmittedLimitQ16 = 0, gTransportGain = 0;
        bool gRingDirty = false;
#ifdef OPENRCT2_CLASSIC_MAC
        CFragConnectionID gAudioDriverServices = 0;
        UInt32 (*gAudioExecutionLevel)() = nullptr;
        SndChannelPtr gOutputChannel = nullptr;
        ExtSoundHeader gOutputHeaders[ClassicSoftwareMixer::Stream::Blocks * 2]{};
        ExtSoundHeader gSilenceHeader{};
        std::array<int16_t, ClassicSoftwareMixer::Stream::MaxFrames * 2> gSilenceSamples{};
        volatile bool gOutputQueued = false;
        volatile short gOutputGeneration = 0;
        SndCallBackUPP gOutputCallbackUPP = nullptr;
        struct Pin { void* address; unsigned long size; } gOutputPins[4]{};
        unsigned gOutputPinCount = 0;
        void UnpinOutput()
        {
            while (gOutputPinCount)
            { const auto& pin = gOutputPins[--gOutputPinCount]; UnholdMemory(pin.address, pin.size); }
        }
        bool PinOutput()
        {
            const Pin pins[] = {{gSoftwareStream.get(), sizeof(ClassicSoftwareMixer::Stream)},
                {gOutputHeaders, sizeof(gOutputHeaders)}, {&gSilenceHeader, sizeof(gSilenceHeader)},
                {gSilenceSamples.data(), sizeof(gSilenceSamples)}};
            for (const auto& pin : pins)
            {
                if (HoldMemory(pin.address, pin.size) != noErr) { UnpinOutput(); return false; }
                gOutputPins[gOutputPinCount++] = pin;
            }
            return true;
        }
        using MixerControlGuard = ClassicInterruptMask::Guard;
#else
        struct MixerControlGuard {};
#endif
        ClassicSoftwareMixer::Mixer* SoftwareMixer()
        { return gSoftwareStream ? &gSoftwareStream->Core() : nullptr; }

#ifdef OPENRCT2_CLASSIC_MAC
        uint32_t BeginCallbackTrace()
        {
            if (!gTraceAudio) return 0;
            uint32_t began; __asm__ volatile("mftb %0" : "=r"(began));
            if (gCallbackSamples)
            {
                const uint32_t interval = began - gCallbackLastEntry;
                gCallbackIntervalTicks += interval; gCallbackIntervals++;
                gCallbackIntervalMin = std::min(uint32_t(gCallbackIntervalMin), interval);
                gCallbackIntervalMax = std::max(uint32_t(gCallbackIntervalMax), interval);
            }
            gCallbackLastEntry = began;
            gCallbackLevel = gAudioExecutionLevel ? gAudioExecutionLevel() : 0;
            gCallbackMask = ClassicInterruptMask::Get();
            return began;
        }
        void EndCallbackTrace(uint32_t began)
        {
            if (!gTraceAudio) return;
            uint32_t ended; __asm__ volatile("mftb %0" : "=r"(ended));
            const uint32_t ticks = ended - began;
            gCallbackTicks += ticks; gMaxCallbackTicks = std::max(uint32_t(gMaxCallbackTicks), ticks); gCallbackSamples++;
        }
        bool SetTransportGain(unsigned captured)
        {
            // The explicit clock probe can submit only the immutable silence header:
            // every VoiceStart is refused, TakeReady is bypassed, and OpenDevice
            // filled this storage with signed zero (or unsigned 8-bit midpoint).
            // Unity over zeros exercises the normal transport without a zero-gain
            // shortcut in Sound Manager. No source samples can reach this path.
            const unsigned level = gSilentPlaybackTest ? 0 : gProbeMode == 3
                ? (gLimitQ16 ? 256 : 0) : ClassicSoftwareMixer::TransportGain(gLimitQ16, captured);
            if (level == gTransportGain) return true;
            SndCommand command{volumeCmd, 0, static_cast<long>((level << 16) | level)};
            if (SndDoImmediate(gOutputChannel, &command) != noErr) { gVolumeCommandErrors++; return false; }
            gTransportGain = level; return true;
        }
        void SubmitOutput(bool allowSilence = false)
        {
            if (!gStreamWanted && gProbeMode != 3) return;
            if (allowSilence && gProbeMode != 3)
                gMinimumReadyBlocks = std::min(unsigned(gMinimumReadyBlocks), gSoftwareStream->Ready());
            auto* block = gProbeMode == 3 ? nullptr : gSoftwareStream->TakeReady();
            if (!block && !allowSilence && gProbeMode != 3) return; // idle: no recurring application callbacks
            if (!block && gProbeMode != 3) gUnderruns++; // intentional clock silence is not starvation
            const auto* header = block ? &gOutputHeaders[block->slot] : &gSilenceHeader;
            gSubmittedLimitQ16 = block ? block->gainTag : 0;
            if (!SetTransportGain(gSubmittedLimitQ16)) { gClosing = true; return; }
            gOutputQueued = true;
            SndCommand command{bufferCmd, 0, reinterpret_cast<long>(header)};
            if (SndDoCommand(gOutputChannel, &command, true) != noErr)
            { gQueueErrors++; gOutputQueued = false; gClosing = true; return; }
            command = {callBackCmd, gOutputGeneration, reinterpret_cast<long>(block)};
            if (SndDoCommand(gOutputChannel, &command, true) != noErr)
            { gQueueErrors++; gClosing = true; } // retain all storage until quiet/dispose
        }
        pascal void OutputCallback(SndChannelPtr, SndCommand* command)
        {
            if (gClosing || command->param1 != gOutputGeneration) return;
            // Interrupt time: only queue metadata/headers, no mixer, PCM, source,
            // decoder, allocation, file I/O, or voice traversal.
            const uint32_t began = BeginCallbackTrace();
            const auto* block = reinterpret_cast<ClassicSoftwareMixer::Stream::Block*>(command->param2);
            if (block) gSoftwareStream->Complete(*block);
            gOutputQueued = false;
            gCallbacks++;
            SubmitOutput(!block || !block->ends);
            EndCallbackTrace(began);
        }
        void StartOutput()
        {
            if (!gMixerBackend || !gOutputChannel || gClosing || gOutputQueued || (!gStreamWanted && gProbeMode != 3)) return;
            SubmitOutput();
        }
#else
        void StartOutput() {}
#endif
        void ServiceOutput()
        {
            if (gProbeMode) return; // diagnostic clock has no producer or source/voice work
            if (!gSoftwareStream || gClosing) return;
            // Retire after consumption, not when ahead mixing predicts the end.
            const auto completed = gSoftwareStream->CompletedTimeline();
            for (unsigned i = 0; i < kVoiceCount; ++i)
                if (gVoices[i].busy && completed[i].generation == gVoices[i].mixGeneration && !completed[i].playing)
                    gVoices[i].finished = 1;
            const auto began = NowMicroseconds();
            if (gLastServiceEnd) gMaxProducerGap = std::max(gMaxProducerGap, began - gLastServiceEnd);
            const unsigned beforeBlocks = gSoftwareStream->MixedBlocks();
            gSoftwareStream->SetGainTag(gLimitQ16);
            bool wanted = false;
            for (const auto& voice : gVoices) wanted = wanted || (voice.busy && !voice.finished);
            gStreamWanted = wanted; // publish once; a callback must never see a temporary false
            if (gRingDirty)
            {
                // Keep published old PCM available throughout task-time mixing.
                // Publish only if no callback claimed the first replacement.
                // Retry once, then defer controls; refill the reserve below either way.
                for (unsigned attempt = 0; attempt < 2; ++attempt)
                {
                    gSoftwareStream->BuildReplacement(gOutputMuted || gSilentPlaybackTest);
                    bool committed;
                    { MixerControlGuard guard; committed = gSoftwareStream->CommitReplacement(); }
                    if (committed) { gRingDirty = false; break; }
                    gSoftwareStream->CancelReplacement();
                }
                gRebuiltBlocks += gSoftwareStream->MixedBlocks() - beforeBlocks;
            }
            if (gRingDirty) gReplacementDeferrals++;
            // A deferred control replacement must not defer reserve maintenance.
            // CancelReplacement restored the old tail cursor; append from it,
            // retaining pending controls for the next replacement attempt.
            while (gSoftwareStream->Prepare(gOutputMuted || gSilentPlaybackTest)) {}
            const unsigned blocks = gSoftwareStream->MixedBlocks() - beforeBlocks;
            gMixedBlocks += blocks; gMixedFrames += blocks * gMixerBlockFrames;
            gLastServiceEnd = NowMicroseconds();
            gMixMicros += gLastServiceEnd - began;
            StartOutput();
        }


#ifdef OPENRCT2_CLASSIC_MAC
        // Optional read-only diagnostics. Resolve the newer SoundLib entry points
        // without making them a launch-time requirement of older systems.
        struct SoundQueries
        {
            CFragConnectionID connection = 0;
            OSErr (*channel)(SndChannelPtr, OSType, void*) = nullptr;
            OSErr (*output)(Component, OSType, void*) = nullptr;
            SoundQueries()
            {
                Ptr mainAddress = nullptr; Str255 message{};
                if (GetSharedLibrary(reinterpret_cast<const unsigned char*>("\010SoundLib"), kPowerPCCFragArch,
                    kLoadCFrag, &connection, &mainAddress, message) != noErr) return;
                Ptr address = nullptr; CFragSymbolClass kind = 0;
                if (FindSymbol(connection, reinterpret_cast<const unsigned char*>("\012SndGetInfo"), &address, &kind) == noErr
                    && kind == kTVectorCFragSymbol) channel = reinterpret_cast<decltype(channel)>(address);
                address = nullptr;
                if (FindSymbol(connection, reinterpret_cast<const unsigned char*>("\022GetSoundOutputInfo"), &address, &kind) == noErr
                    && kind == kTVectorCFragSymbol) output = reinterpret_cast<decltype(output)>(address);
            }
            ~SoundQueries() { if (connection) CloseConnection(&connection); }
        };
        SoundQueries& Queries() { static SoundQueries queries; return queries; }
        pascal void VoiceCallback(SndChannelPtr channel, SndCommand* command)
        {
            // Interrupt time: no allocation, only Sound Manager commands.
            auto* voice = reinterpret_cast<Voice*>(command->param2);
            if (gClosing || voice == nullptr || static_cast<short>(voice->generation) != command->param1)
                return;
            const uint32_t began = BeginCallbackTrace();
            gCallbacks++;
            if (voice->looping)
            {
                SndCommand next;
                next.cmd = bufferCmd;
                next.param1 = 0;
                next.param2 = reinterpret_cast<long>(&voice->whole);
                SndDoCommand(channel, &next, true);
                next.cmd = callBackCmd;
                next.param1 = static_cast<short>(voice->generation);
                next.param2 = reinterpret_cast<long>(voice);
                SndDoCommand(channel, &next, true);
            }
            else
                voice->finished = 1;
            EndCallbackTrace(began);
        }
        SndCallBackUPP gCallbackUPP = nullptr;

        void FillHeader(Voice::Header& header, const ClassicSource& source, uint32_t startFrame)
        {
            std::memset(&header, 0, sizeof(header));
            if (source.compressed)
            {
                const uint32_t packet = std::min(startFrame / 64, source.frames / 64);
                auto& h = header.compressed;
                h.samplePtr = reinterpret_cast<Ptr>(const_cast<uint8_t*>(source.Bytes().data()) + static_cast<size_t>(packet) * 34);
                h.numChannels = source.channels;
                h.sampleRate = static_cast<UnsignedFixed>(source.rate) << 16;
                h.encode = cmpSH;
                h.baseFrequency = 60;
                h.numFrames = source.frames / 64 - packet;
                h.format = 'ima4';
                h.compressionID = fixedCompression;
                h.sampleSize = 16;
            }
            else
            {
                startFrame = std::min(startFrame, source.frames);
                auto& h = header.plain;
                h.samplePtr = reinterpret_cast<Ptr>(const_cast<uint8_t*>(source.Bytes().data()) + static_cast<size_t>(startFrame) * source.channels * 2);
                h.numChannels = source.channels;
                h.sampleRate = static_cast<UnsignedFixed>(source.rate) << 16;
                h.encode = extSH;
                h.baseFrequency = 60;
                h.numFrames = source.frames - startFrame;
                h.sampleSize = 16;
            }
        }
#endif

        // quietNow cancels queued buffers and callbacks; do not wait for a loop to drain.
        // Sources and header storage remain alive until every channel has relinquished them.
        bool CloseDevice()
        {
            const auto start = NowMicroseconds();
            gClosing = true;
            int playing = 0, remaining = 0;
            for (auto& voice : gVoices)
            {
                playing += voice.busy;
                voice.generation++;
                voice.looping = 0;
                voice.busy = false;
            }
#ifdef OPENRCT2_CLASSIC_MAC
            if (gOutputChannel)
            {
                gOutputGeneration++;
                const OSErr error = SndDisposeChannel(gOutputChannel, true);
                if (error == noErr) { gOutputChannel = nullptr; gOutputQueued = false; gDisposed++; }
                else { remaining++; std::printf("audio output dispose error: %d\n", error); }
            }
            if (!gOutputChannel && gOutputCallbackUPP)
            { DisposeSndCallBackUPP(gOutputCallbackUPP); gOutputCallbackUPP = nullptr; }
            for (auto& voice : gVoices)
                if (voice.channel != nullptr)
                {
                    const OSErr error = SndDisposeChannel(voice.channel, true);
                    if (error == noErr) { voice.channel = nullptr; gDisposed++; }
                    else { remaining++; std::printf("audio dispose error: %d\n", error); }
                }
            // Never free a UPP which a channel still owns, even after a driver error.
            if (!remaining) UnpinOutput();
            if (!remaining && gAudioDriverServices)
            { CloseConnection(&gAudioDriverServices); gAudioDriverServices = 0; gAudioExecutionLevel = nullptr; }
            if (remaining == 0 && gCallbackUPP != nullptr)
            {
                DisposeSndCallBackUPP(gCallbackUPP);
                gCallbackUPP = nullptr;
            }
#endif
            if (!remaining) gSoftwareStream.reset();
            gDeviceOpen = remaining != 0;
            std::printf("audio close: playing %d allocated %lu disposed %lu remaining %d us %lu\n",
                playing, gAllocated, gDisposed, remaining,
                static_cast<unsigned long>(NowMicroseconds() - start));
            return remaining == 0;
        }
        void ExitAudio() { CloseDevice(); }

        void OpenDevice()
        {
            if (gDeviceOpen || (gOutputLimit <= 0 && !gSilentPlaybackTest && gProbeMode < 2))
                return;
            gDeviceOpen = true;
            gClosing = false;
            static bool registeredExit = false;
            if (!registeredExit) { std::atexit(ExitAudio); registeredExit = true; }
            if (gMixerBackend)
            {
                gLastServiceEnd = gMaxProducerGap = 0; gMinimumReadyBlocks = ~0u;
                gSoftwareStream = std::make_unique<ClassicSoftwareMixer::Stream>(gMixerBackend == 3 ? 44100 : 22050,
                    gMixerBackend == 2 ? 1 : 2, gMixerBits, gMixerBlockFrames);
#ifdef OPENRCT2_CLASSIC_MAC
                if (!ClassicInterruptMask::Prepare()) { gDeviceOpen = false; gSoftwareStream.reset(); return; }
                gOutputCallbackUPP = NewSndCallBackUPP(OutputCallback);
                if (!gOutputCallbackUPP || SndNewChannel(&gOutputChannel, sampledSynth, initStereo, gOutputCallbackUPP) != noErr)
                {
                    if (gOutputCallbackUPP) DisposeSndCallBackUPP(gOutputCallbackUPP);
                    gOutputCallbackUPP = nullptr; gOutputChannel = nullptr; gDeviceOpen = false; return;
                }
                gAllocated++; gTransportGain = 0; gSubmittedLimitQ16 = 0;
                Ptr mainAddress = nullptr; Str255 message{};
                if (GetSharedLibrary(reinterpret_cast<const unsigned char*>("\021DriverServicesLib"), kPowerPCCFragArch,
                    kLoadCFrag, &gAudioDriverServices, &mainAddress, message) == noErr)
                {
                    Ptr address = nullptr; CFragSymbolClass kind = 0;
                    if (FindSymbol(gAudioDriverServices, reinterpret_cast<const unsigned char*>("\025CurrentExecutionLevel"),
                        &address, &kind) == noErr && kind == kTVectorCFragSymbol)
                        gAudioExecutionLevel = reinterpret_cast<decltype(gAudioExecutionLevel)>(address);
                }
                SndCommand silence{volumeCmd, 0, 0};
                if (SndDoImmediate(gOutputChannel, &silence) != noErr)
                { gVolumeCommandErrors++; CloseDevice(); return; }
                auto initHeader = [](ExtSoundHeader& header, const void* samples)
                {
                    header = {};
                    header.samplePtr = reinterpret_cast<Ptr>(const_cast<void*>(samples));
                    header.numChannels = SoftwareMixer()->Channels();
                    header.sampleRate = static_cast<UnsignedFixed>(SoftwareMixer()->Rate()) << 16;
                    header.encode = extSH; header.baseFrequency = 60;
                    header.numFrames = gMixerBlockFrames; header.sampleSize = gMixerBits;
                };
                for (unsigned i = 0; i < ClassicSoftwareMixer::Stream::Blocks * 2; ++i)
                    initHeader(gOutputHeaders[i], gSoftwareStream->At(i).Bytes());
                std::memset(gSilenceSamples.data(), gMixerBits == 8 ? 128 : 0, sizeof(gSilenceSamples));
                initHeader(gSilenceHeader, gSilenceSamples.data());
                // Preconfigure at task time, including converter state. All IRQ
                // buffer commands thereafter keep this exact same format.
                if (!PinOutput()) { std::printf("audio mixer output pin failed\n"); CloseDevice(); return; }
                SndCommand configure{soundCmd, 0, reinterpret_cast<long>(&gOutputHeaders[0])};
                if (SndDoImmediate(gOutputChannel, &configure) != noErr)
                { gQueueErrors++; CloseDevice(); return; }
                SoundComponentData hardware{};
                gHardwareQueryError = Queries().output ? Queries().output(nullptr, siHardwareFormat, &hardware) : -1;
                if (gHardwareQueryError == noErr)
                {
                    gHardwareFrames = std::max(0L, hardware.sampleCount); gHardwareRateFixed = hardware.sampleRate;
                    gHardwareBits = hardware.sampleSize; gHardwareChannels = hardware.numChannels;
                }
                // Conservative SM4/VM fallback: two 4096-frame DMA buffers at
                // the observed 44100-Hz device rate. Stream keeps this floor even
                // on a small-DMA device, then adds its producer-absence budget.
                const unsigned deviceRate = gHardwareRateFixed ? gHardwareRateFixed >> 16 : 44100;
                const unsigned frames = gHardwareFrames ? gHardwareFrames : 4096;
                if (!gSoftwareStream->SetHardwareReserve(uint64_t(frames) * 2 * SoftwareMixer()->Rate() / deviceRate))
                { std::printf("audio mixer block too small for hardware reserve\n"); CloseDevice(); return; }
#endif
                return;
            }
#ifdef OPENRCT2_CLASSIC_MAC
            gCallbackUPP = NewSndCallBackUPP(VoiceCallback);
            if (gCallbackUPP == nullptr) { gDeviceOpen = false; return; }
            for (auto& voice : gVoices)
            {
                voice.channel = nullptr;
                if (SndNewChannel(&voice.channel, sampledSynth, initStereo, gCallbackUPP) != noErr)
                    voice.channel = nullptr;
                else gAllocated++;
            }
#endif
        }

        void VoiceQuiet(Voice& voice)
        {
            if (SoftwareMixer())
            {
                SoftwareMixer()->Stop(unsigned(&voice - gVoices));
                gRingDirty = true;
            }
            voice.generation++;
            voice.looping = 0;
#ifdef OPENRCT2_CLASSIC_MAC
            if (voice.channel != nullptr)
            {
                SndCommand command{ flushCmd, 0, 0 };
                SndDoImmediate(voice.channel, &command);
                command.cmd = quietCmd;
                SndDoImmediate(voice.channel, &command);
            }
#endif
        }

        Voice* VoiceStart(const ClassicSource& source, uint32_t startFrame, bool loop)
        {
            if (gProbeMode) { gRefused++; return nullptr; }
            if (gClosing || (gOutputLimit <= 0.0f && !gSilentPlaybackTest))
                return nullptr; // muted: the game runs as if every voice were taken
            OpenDevice();
            Voice* voice = nullptr;
            for (auto& candidate : gVoices)
                if (!candidate.busy)
                {
                    voice = &candidate;
                    break;
                }
            if (voice == nullptr)
            {
                gRefused++;
                return nullptr;
            }
#ifdef OPENRCT2_CLASSIC_MAC
            if ((!gMixerBackend && voice->channel == nullptr) || (gMixerBackend && !gOutputChannel))
            {
                gRefused++;
                return nullptr;
            }
#endif
            source.EnsureData();
            if (source.Bytes().empty())
                return nullptr;
            voice->busy = true;
            voice->compressed = source.compressed;
            voice->rate = source.rate;
            voice->channels = source.channels;
            voice->left = voice->right = 0;
            voice->multiplier = 1;
            voice->mixLeft = voice->mixRight = voice->mixRate = ~0u;
            voice->generation++;
            voice->finished = 0;
            voice->looping = loop ? 1 : 0;
            if (gMixerBackend)
            {
                // Preserve the legacy IMA packet rounding and whole-packet tail.
                const auto frames = source.compressed ? source.frames / 64 * 64 : source.frames;
                if (source.compressed) startFrame = startFrame / 64 * 64;
                const auto bytes = source.Bytes();
                if (!SoftwareMixer()->Start(unsigned(voice - gVoices),
                    {bytes.data(), bytes.size(), frames, source.rate, source.channels,
                        source.compressed ? ClassicSoftwareMixer::Encoding::Ima4 : ClassicSoftwareMixer::Encoding::Pcm16BigEndian},
                    startFrame, loop)) { voice->busy = false; return nullptr; }
                voice->mixGeneration = SoftwareMixer()->SaveTimeline()[voice - gVoices].generation;
                voice->finished = SoftwareMixer()->Finished(unsigned(voice - gVoices)) ? 1 : 0;
                gRingDirty = true;
                gStarted++; gSilentStarts++;
                return voice;
            }
            gStarted++;
#ifdef OPENRCT2_CLASSIC_MAC
            FillHeader(voice->first, source, startFrame);
            FillHeader(voice->whole, source, 0);
            SndCommand command;
            // Silent until the channel's own volume is applied (never a moment at full volume).
            command.cmd = volumeCmd;
            command.param1 = 0;
            command.param2 = 0;
            if (SndDoImmediate(voice->channel, &command) != noErr)
            {
                // Never enqueue samples if the initial silence command failed.
                gVolumeCommandErrors++;
                voice->busy = false;
                return nullptr;
            }
            gSilentStarts++;
            command.cmd = bufferCmd;
            command.param1 = 0;
            command.param2 = reinterpret_cast<long>(&voice->first);
            SndDoCommand(voice->channel, &command, true);
            command.cmd = callBackCmd;
            command.param1 = static_cast<short>(voice->generation);
            command.param2 = reinterpret_cast<long>(voice);
            SndDoCommand(voice->channel, &command, true);
            if (loop)
            {
                // Keep one repeat queued ahead so the loop has no gap.
                command.cmd = bufferCmd;
                command.param1 = 0;
                command.param2 = reinterpret_cast<long>(&voice->whole);
                SndDoCommand(voice->channel, &command, true);
                command.cmd = callBackCmd;
                command.param1 = static_cast<short>(voice->generation);
                command.param2 = reinterpret_cast<long>(voice);
                SndDoCommand(voice->channel, &command, true);
            }
#else
            (void)source;
            (void)startFrame;
#endif
            return voice;
        }

        void VoiceSetGain(Voice& voice, float left, float right)
        {
            if (gSilentPlaybackTest) left = right = 0.0f;
            // Enforce the launch safety limit at the final command, too.
            left = std::clamp(left, 0.0f, gOutputLimit);
            right = std::clamp(right, 0.0f, gOutputLimit);
            voice.left = static_cast<unsigned>(left * 256.0f);
            voice.right = static_cast<unsigned>(right * 256.0f);
            gPeakGainUnits = std::max({gPeakGainUnits, voice.left, voice.right});
            if (SoftwareMixer())
            {
                // Explicit zero-output test exercises the full decoder/mixer. Both
                // prepared PCM and the transport volume remain zero throughout.
                const unsigned l = gSilentPlaybackTest ? 256 : voice.left, r = gSilentPlaybackTest ? 256 : voice.right;
                if (voice.mixLeft != l || voice.mixRight != r)
                {
                    SoftwareMixer()->SetGain(unsigned(&voice - gVoices), l, r);
                    voice.mixLeft = l; voice.mixRight = r; gRingDirty = true;
                }
                return;
            }
#ifdef OPENRCT2_CLASSIC_MAC
            const long l = std::clamp(static_cast<long>(left * 256.0f), 0L, 256L);
            const long r = std::clamp(static_cast<long>(right * 256.0f), 0L, 256L);
            SndCommand command{ volumeCmd, 0, (r << 16) | l };
            if (SndDoImmediate(voice.channel, &command) != noErr) gVolumeCommandErrors++;
#else
            (void)voice;
            (void)left;
            (void)right;
#endif
        }

        void VoiceSetRate(Voice& voice, double rate)
        {
            voice.multiplier = rate;
            if (SoftwareMixer())
            {
                const auto fixed = static_cast<uint32_t>(std::clamp(rate * 65536.0, 1.0, 2147483647.0));
                if (voice.mixRate != fixed)
                {
                    SoftwareMixer()->SetRate(unsigned(&voice - gVoices), fixed);
                    voice.mixRate = fixed; gRingDirty = true;
                }
                return;
            }
#ifdef OPENRCT2_CLASSIC_MAC
            SndCommand command{ rateMultiplierCmd, 0, static_cast<long>(rate * 65536.0) };
            SndDoImmediate(voice.channel, &command);
#else
            (void)voice;
            (void)rate;
#endif
        }

        float gMasterVolume = 1.0f; // IAudioMixer::SetVolume

        // The volume sliders' curve, worked out again only when a slider moves.
        float SliderCurve(int percent, int& cachedPercent, float& cachedValue)
        {
            if (percent != cachedPercent)
            {
                cachedPercent = percent;
                cachedValue = std::pow(static_cast<float>(percent) / 100.0f, 10.0f / 6.0f);
            }
            return cachedValue;
        }

        float GroupGain(MixerGroup group)
        {
            // The same arithmetic as the software mixer on other platforms.
            const auto& sound = Config::Get().sound;
            float gain = gOutputLimit * gMasterVolume
                * (sound.masterSoundEnabled ? static_cast<float>(sound.masterVolume) / 100.0f : 0.0f);
            if (group == MixerGroup::sound)
            {
                if (!sound.soundEnabled)
                    return 0.0f;
                static int percent = -1;
                static float value = 0.0f;
                gain *= SliderCurve(sound.soundVolume, percent, value);
                if (gLegacyScene == LegacyScene::titleSequence)
                    gain = std::min(gain, 0.75f);
            }
            else
            {
                static int percent = -1;
                static float value = 0.0f;
                gain *= SliderCurve(sound.rideMusicVolume, percent, value);
            }
            return gain;
        }

        class ClassicChannel final : public IAudioChannel
        {
            ClassicSource* _source = nullptr;
            Voice* _voice = nullptr;
            long _voiceGeneration = 0;
            MixerGroup _group = MixerGroup::sound;
            double _rate = 1.0;
            int32_t _loop = 0;
            int32_t _volume = kMixerVolumeMax, _oldVolume = kMixerVolumeMax;
            float _pan = 0.5f, _left = 1.0f, _right = 1.0f;
            bool _stopping = false, _done = true, _deleteOnDone = false;
            bool _started = false;
            uint64_t _startOffset = 0, _startTime = 0;
            float _appliedLeft = -1.0f, _appliedRight = -1.0f;
            double _appliedRate = -1.0;

            bool VoiceIsOurs() const
            {
                return _voice != nullptr && _voice->busy && _voice->generation == _voiceGeneration;
            }

        public:
            ~ClassicChannel() override
            {
                Stop();
            }

            // The engine sets volume, pan and rate right after Play: the sound itself starts
            // here, once they are known.
            MixerGroup Group() const { return _group; }

            void Start()
            {
                if (_started || _source == nullptr || _done)
                    return;
                _started = true;
                const uint32_t bytesPerFrame = static_cast<uint32_t>(_source->channels) * 2;
                _voice = VoiceStart(*_source, static_cast<uint32_t>(_startOffset / bytesPerFrame), _loop != 0);
                _startTime = NowMicroseconds();
                if (_voice == nullptr)
                {
                    // No free voice: a short sound is simply not heard. A loop stays "playing"
                    // without a voice so the game does not ask for it again every tick.
                    if (_loop == 0)
                        _done = true;
                    return;
                }
                _voiceGeneration = _voice->generation;
                _appliedLeft = _appliedRight = -1.0f;
                _appliedRate = -1.0;
                Apply();
            }

            void Apply()
            {
                if (!VoiceIsOurs())
                    return;
                const float gain = GroupGain(_group) * static_cast<float>(_volume) / static_cast<float>(kMixerVolumeMax);
                const float left = gain * _left, right = gain * _right;
                if (left != _appliedLeft || right != _appliedRight)
                {
                    VoiceSetGain(*_voice, left, right);
                    _appliedLeft = left;
                    _appliedRight = right;
                }
                if (_rate != _appliedRate)
                {
                    VoiceSetRate(*_voice, _rate);
                    _appliedRate = _rate;
                }
            }

            // Once per frame. Returns false when the mixer should forget this channel.
            bool Tick(bool orphaned)
            {
                if (!_started)
                    Start();
                if (_source == nullptr || _source->released)
                {
                    Stop();
                    return false;
                }
                if (VoiceIsOurs() && _voice->finished)
                {
                    ReleaseVoice();
                    _done = true;
                }
                else if (_voice == nullptr && _loop == 0 && !_done)
                    _done = true;
                if (_stopping)
                {
                    Stop();
                    return false;
                }
                // A loop nobody holds any more can never be stopped by the game: end it.
                if (orphaned && _loop != 0)
                {
                    Stop();
                    return false;
                }
                if (_done && _deleteOnDone)
                    return false;
                Apply();
                return true;
            }

            void ReleaseVoice()
            {
                if (VoiceIsOurs())
                {
                    VoiceQuiet(*_voice);
                    _voice->busy = false;
                }
                _voice = nullptr;
            }

            IAudioSource* GetSource() const override
            {
                return _source;
            }
            MixerGroup GetGroup() const override
            {
                return _group;
            }
            void SetGroup(MixerGroup group) override
            {
                _group = group;
            }
            double GetRate() const override
            {
                return _rate;
            }
            void SetRate(double rate) override
            {
                _rate = std::max(0.001, rate);
            }
            uint64_t GetOffset() const override
            {
#ifdef OPENRCT2_SERVICES_AUDIO_COUNTS
                gOffsetCalls++;
#endif
                // Nobody tells us where the Sound Manager is in the sound: estimate from time.
                if (_source == nullptr || !_started)
                    return _startOffset;
                const uint64_t length = _source->GetLength();
                if (length == 0)
                    return 0;
                const double played = static_cast<double>(NowMicroseconds() - _startTime) / 1000000.0 * _source->GetBytesPerSecond()
                    * _rate;
                uint64_t offset = _startOffset + static_cast<uint64_t>(played);
                if (_loop != 0)
                    offset %= length;
                else
                    offset = std::min(offset, length);
                return offset & ~uint64_t(3);
            }
            bool SetOffset(uint64_t offset) override
            {
                if (_source == nullptr || offset >= _source->GetLength())
                    return false;
                _startOffset = offset;
                if (_started)
                {
                    // Already sounding: start again from there.
                    ReleaseVoice();
                    _started = false;
                    _done = false;
                    Start();
                }
                return true;
            }
            int32_t GetLoop() const override
            {
                return _loop;
            }
            void SetLoop(int32_t value) override
            {
                _loop = value;
            }
            int32_t GetVolume() const override
            {
                return _volume;
            }
            float GetVolumeL() const override
            {
                return _left;
            }
            float GetVolumeR() const override
            {
                return _right;
            }
            float GetOldVolumeL() const override
            {
                return _left;
            }
            float GetOldVolumeR() const override
            {
                return _right;
            }
            int32_t GetOldVolume() const override
            {
                return _oldVolume;
            }
            void SetVolume(int32_t volume) override
            {
                _volume = std::clamp(volume, 0, kMixerVolumeMax);
            }
            float GetPan() const override
            {
                return _pan;
            }
            void SetPan(float pan) override
            {
                PROBE_SCOPE(SoundPan);
#ifdef OPENRCT2_SERVICES_AUDIO_COUNTS
                gPanCalls++;
                gPanRepeated += _pan == std::clamp(pan, 0.0f, 1.0f);
#endif
                _pan = std::clamp(pan, 0.0f, 1.0f);
                const double decibels = (std::abs(_pan - 0.5) * 2.0) * 100.0;
                const float other = static_cast<float>(1.0 / std::pow(10.0, decibels / 20.0));
                _left = _pan <= 0.5f ? 1.0f : other;
                _right = _pan <= 0.5f ? other : 1.0f;
            }
            bool IsStopping() const override
            {
                return _stopping;
            }
            void SetStopping(bool value) override
            {
                _stopping = value;
            }
            bool IsDone() const override
            {
                return _done;
            }
            void SetDone(bool value) override
            {
                _done = value;
            }
            bool DeleteOnDone() const override
            {
                return _deleteOnDone;
            }
            void SetDeleteOnDone(bool value) override
            {
                _deleteOnDone = value;
            }
            bool IsPlaying() const override
            {
                return !_done;
            }
            void Play(IAudioSource* source, int32_t loop) override
            {
                ReleaseVoice();
                _source = static_cast<ClassicSource*>(source);
                _loop = loop;
                _startOffset = 0;
                _started = false;
                _done = _source == nullptr;
            }
            void Stop() override
            {
                ReleaseVoice();
                _done = true;
                _stopping = true;
            }
            void UpdateOldVolume() override
            {
                _oldVolume = _volume;
                Start(); // the last call of the engine's "create a channel" sequence
            }
            size_t Read(void*, size_t) override
            {
                return 0;
            }
        };

        class ClassicMixer final : public IAudioMixer
        {
            std::shared_ptr<ClassicChannel> _failedChannel;

        public:
            std::vector<std::shared_ptr<ClassicChannel>> channels;
            std::vector<std::unique_ptr<ClassicSource>> sources;

            void Init(const char*) override
            {
                OpenDevice();
            }
            void Close() override
            {
                channels.clear();
            }
            void Lock() override
            {
            }
            void Unlock() override
            {
            }
            bool CanPlay() const override
            {
                if (gProbeMode) return true; // force normal engine selection but refuse at VoiceStart
                if (gOutputLimit <= 0.0f && !gSilentPlaybackTest)
                    return false;
                OpenDevice();
                for (const auto& voice : gVoices)
                    if (!voice.busy)
                    {
#ifdef OPENRCT2_CLASSIC_MAC
                        return gMixerBackend ? gOutputChannel != nullptr : voice.channel != nullptr;
#else
                        return true;
#endif
                    }
                return false;
            }
            std::shared_ptr<IAudioChannel> Play(IAudioSource* source, int32_t loop, bool deleteOnDone) override
            {
                gPlayRequests++;
                if (source == nullptr)
                    return nullptr;
                if (loop == 0 && !CanPlay())
                {
                    // Callers remember failed one-shot channels too; keep that result without allocating each retry.
                    if (_failedChannel == nullptr)
                        _failedChannel = std::make_shared<ClassicChannel>();
                    return _failedChannel;
                }
                auto channel = std::make_shared<ClassicChannel>();
                channel->Play(source, loop);
                channel->SetDeleteOnDone(deleteOnDone);
                channels.push_back(channel);
                return channel;
            }
            void SetVolume(float volume) override
            {
                gMasterVolume = volume;
            }
            void Tick()
            {
                for (size_t i = 0; i < channels.size();)
                {
                    if (channels[i]->Tick(channels[i].use_count() == 1))
                        i++;
                    else
                    {
                        channels[i] = channels.back();
                        channels.pop_back();
                    }
                }
                // Sources nobody plays any more.
                for (size_t i = 0; i < sources.size();)
                {
                    bool used = false;
                    if (sources[i]->released)
                        for (const auto& channel : channels)
                            used = used || channel->GetSource() == sources[i].get();
                    if (sources[i]->released && !used)
                    {
                        sources[i] = std::move(sources.back());
                        sources.pop_back();
                    }
                    else
                    {
                        if (sources[i]->cachedData)
                        {
                            bool playing = false;
                            for (const auto& channel : channels)
                                if (channel->GetSource() == sources[i].get() && channel->IsPlaying()) { playing = true; break; }
                            if (!playing) sources[i]->cachedData.reset();
                        }
                        i++;
                    }
                }
            }
        };
        ClassicMixer* gMixer = nullptr;

        class ClassicAudioContext final : public IAudioContext
        {
            ClassicMixer _mixer;

            IAudioSource* Add(std::unique_ptr<ClassicSource> source)
            {
                if (source == nullptr || source->frames == 0)
                {
                    gSourcesFailed++;
                    return nullptr;
                }
                gSourcesLoaded++;
                _mixer.sources.push_back(std::move(source));
                return _mixer.sources.back().get();
            }

        public:
            ClassicAudioContext()
            {
                gMixer = &_mixer;
#ifndef OPENRCT2_CLASSIC_MAC
                if (const char* flag = std::getenv("RCT_PRELOAD"))
                    ClassicAudioSetPreloadMode(flag[0] == '0' || std::strcmp(flag, "off") == 0 ? 0 : flag[0] == '1' || std::strcmp(flag, "on") == 0 ? 1 : 2);
#endif
            }
            ~ClassicAudioContext() override
            {
#ifdef OPENRCT2_SERVICES_AUDIO_COUNTS
                std::fprintf(stderr, "services-audio pan=%lu repeated=%lu offsets=%lu\n", static_cast<unsigned long>(gPanCalls),
                    static_cast<unsigned long>(gPanRepeated), static_cast<unsigned long>(gOffsetCalls));
#endif
                // Device teardown must precede source destruction. Failed drivers keep
                // their source buffers and UPP until the process dies; the exit hook retries once.
                const bool closed = CloseDevice();
                _mixer.channels.clear();
                if (!closed)
                    for (auto& source : _mixer.sources) source.release();
                gMixer = nullptr;
            }
            IAudioMixer* GetMixer() override
            {
                return &_mixer;
            }
            std::vector<std::string> GetOutputDevices() override
            {
                return {};
            }
            void SetOutputDevice(const std::string&) override
            {
                OpenDevice();
            }
            IAudioSource* CreateStreamFromCSS(std::unique_ptr<IStream> stream, uint32_t index) override
            {
                ClassicDiskIo::Scope io("audio-bank");
                // Sound bank: count, offsets, then per sound a length, a WAVEFORMATEX and the samples.
                if (stream == nullptr)
                    return nullptr;
                // Read only the requested sound: the whole bank is several megabytes and the
                // engine asks for its sounds one at a time.
                const uint64_t size = stream->GetLength();
                uint8_t word[4];
                stream->SetPosition(0);
                if (size < 8 || stream->TryRead(word, 4) != 4 || index >= ReadLE32(word) || size < 4 + (index + 1) * 4ull)
                    return nullptr;
                stream->SetPosition(4 + index * 4ull);
                if (stream->TryRead(word, 4) != 4)
                    return nullptr;
                const uint64_t at = ReadLE32(word);
                uint8_t header[22];
                stream->SetPosition(at);
                if (at + 22 > size || stream->TryRead(header, sizeof(header)) != sizeof(header))
                    return nullptr;
                const size_t length = ReadLE32(header);
                auto source = std::make_unique<ClassicSource>();
                source->channels = ReadLE16(header + 6);
                source->rate = ReadLE32(header + 8);
                const int bits = ReadLE16(header + 18);
                if (at + 22 + length > size || (bits != 8 && bits != 16) || source->channels == 0)
                    return nullptr;
                std::vector<uint8_t> samples(length);
                if (stream->TryRead(samples.data(), length) != length)
                    return nullptr;
                source->TakeLittleEndian(samples.data(), length, bits);
                return Add(std::move(source));
            }
            IAudioSource* CreateStreamFromWAV(std::unique_ptr<IStream> stream) override
            {
                if (stream == nullptr)
                    return nullptr;
                ClassicDiskIo::Scope io("audio-header");
                auto* file = dynamic_cast<FileStream*>(stream.get());
                const std::string key = file ? CacheKey(file->GetPath()) : std::string();
                if (auto found = gSampleCache.find(key); found != gSampleCache.end())
                {
                    const auto& cached = found->second;
                    auto source = std::make_unique<ClassicSource>();
                    source->compressed = cached.compressed; source->rate = cached.rate;
                    source->channels = cached.channels; source->frames = cached.frames; source->cacheKey = key;
                    if (Ready(cached)) { source->cachedData = cached.block; found->second.lastUse = ++gAge; gCacheHits++; }
                    return Add(std::move(source));
                }
                uint8_t head[20];
                stream->SetPosition(0);
                if (stream->GetLength() > 20 && stream->TryRead(head, sizeof(head)) == sizeof(head)
                    && std::memcmp(head, "RCTA", 4) == 0)
                {
                    auto source = std::make_unique<ClassicSource>();
                    source->compressed = std::memcmp(head + 4, "ima4", 4) == 0;
                    source->rate = ReadBE32(head + 8);
                    source->channels = static_cast<uint16_t>((head[12] << 8) | head[13]);
                    source->frames = ReadBE32(head + 16);
                    source->cacheKey = key;
                    if (!Register(key, head, stream->GetLength())) source->pending = std::move(stream);
                    return Add(std::move(source));
                }
                return Add(ParseSource(ReadAll(*stream)));
            }
            void StartTitleMusic() override
            {
            }
            void ToggleAllSounds() override
            {
            }
            void PauseSounds() override
            {
            }
            void UnpauseSounds() override
            {
            }
            void StopAll() override
            {
            }
            void StopCrowdSound() override
            {
            }
            void StopRideMusic() override
            {
            }
            void StopTitleMusic() override
            {
            }
            void StopVehicleSounds() override
            {
            }
        };
    } // namespace

    std::unique_ptr<IAudioContext> CreateClassicAudioContext()
    {
        return std::make_unique<ClassicAudioContext>();
    }

    void ClassicAudioSetPreloadEnabled(bool enabled) { ClassicAudioSetPreloadMode(enabled ? 1 : 0); }
    void ClassicAudioSetPreloadMode(unsigned mode) { gModeOverride = mode <= 2 ? static_cast<int>(mode) : 2; }

    void ClassicAudioBeforeParkLoad()
    {
        if (!gMixer) return;
        // Reclaim the optional arena before object import, not after a large
        // park has had to compete with the previous park's retained sounds.
        for (const auto& channel : gMixer->channels) channel->Stop();
        for (const auto& source : gMixer->sources) source->cachedData.reset();
        for (auto& [key, sample] : gSampleCache) sample.block.reset();
        gPrepareQueue.clear(); gSampleArena.reset(); gCacheBytes = 0; gPreloadComplete = false;
        gMixer->Tick();
    }

    namespace
    {
        void QueueParkMusic(bool all)
        {
            if (!gMixer || !gSampleArena) return;
            auto* window = WindowGetMain(); auto* viewport = window ? window->viewport : nullptr;
            auto& manager = GetContext()->GetObjectManager();
            for (const auto& ride : RideManager(getGameState()))
            {
                if (!ride.flags.has(RideFlag::music)) continue;
                if (!all)
                {
                    if (!viewport) continue;
                    const auto start = ride.getStation().getStart(); if (start.isNull()) continue;
                    const auto pos = Translate3DTo2DWithZ(viewport->rotation, start);
                    // Beyond the engine's audible bounds: prepare before a pan
                    // reaches the ride. A teleport can still be a demand miss.
                    const int margin = viewport->ViewWidth() * 3;
                    if (pos.x < viewport->viewPos.x - margin || pos.y < viewport->viewPos.y - margin
                        || pos.x > viewport->viewPos.x + viewport->ViewWidth() + margin
                        || pos.y > viewport->viewPos.y + viewport->ViewHeight() + margin) continue;
                }
                const auto* music = manager.GetLoadedObject<MusicObject>(ride.music);
                if (!music) continue;
                const auto tune = ride.musicTuneId == 255 && music->GetTrackCount() == 1 ? 0 : ride.musicTuneId;
                const auto* track = music->GetTrack(tune); if (!track) continue;
                const auto key = CacheKey(track->Asset.GetPath());
                Queue(key);
            }
        }
    }
    double ClassicAudioPreloadEstimateMs()
    {
        if (!gMixer)
            return 0;
        const auto mode = gModeOverride >= 0 ? gModeOverride : Config::Get().sound.classicPreload;
        if (mode == 0)
            return 25;
        const auto memory = ClassicHost::Memory();
        // The bank is 80.61 MiB. Automatic's real admission still occurs after
        // import; this forecast only chooses the progress budget, never memory.
        if (mode == 1 || (memory.physical > memory.partition && memory.free > 140 * 1024 * 1024))
            return 9500;
        return 600;
    }
    void ClassicAudioPreload()
    {
        if (gPreloadComplete || !gMixer) return;
        gPreloadMode = gModeOverride >= 0 ? gModeOverride : Config::Get().sound.classicPreload;
        ClassicDiskIo::Scope scope("audio-metadata");
        uint64_t payload = 0;
        // Cache headers once. Recreated music sources need no small disk read,
        // even with zero space for retaining the payload.
        const auto paths = ClassicVfs::PackPaths(".ogg");
        size_t metadataDone = 0;
        {
            ClassicLoadProgress::Range progress(0, .02, "sound-metadata");
            for (const auto& path : paths)
            {
                const auto key = CacheKey(path);
                if (!gSampleCache.contains(key))
                {
                    uint8_t head[20];
                    const auto size = ClassicVfs::Query(path).size;
                    if (ClassicVfs::ReadPack(path, 0, head, sizeof(head)) == sizeof(head))
                        Register(key, head, size);
                }
                if (auto found = gSampleCache.find(key); found != gSampleCache.end())
                    payload += found->second.size;
                ClassicLoadProgress::Report(++metadataDone, paths.size());
            }
        }
        gAdmission = ClassicHost::Memory();
        const auto tracks = ClassicVfs::TrackDesignBytes();
        const auto plan = ClassicPreload::Choose(gAdmission, gPreloadMode, payload + tracks);
        gFullPreload = plan.full;
        if (plan.bytes)
        {
            auto bytes = std::unique_ptr<uint8_t[]>(new(std::nothrow) uint8_t[plan.full ? payload : plan.bytes]);
            if (bytes) gSampleArena = std::make_shared<SampleArena>(SampleArena{std::move(bytes), static_cast<size_t>(plan.full ? payload : plan.bytes)});
            else gFullPreload = false; // No repeated allocation probes or partial all-bank cache.
        }
        gPreloading = true;
        if (gFullPreload)
        {
            // Most time is bulk sample I/O. Retained designs need only final
            // bookkeeping on later park loads; otherwise give the small entries
            // their own range, without rescanning the pack index for each one.
            const double samplesEnd = ClassicVfs::TrackDesignCacheBytes() ? .995 : .98;
            uint64_t filled = 0;
            for (auto& [key, sample] : gSampleCache)
            {
                ClassicLoadProgress::Range progress(
                    .02 + (samplesEnd - .02) * filled / std::max<uint64_t>(1, payload),
                    .02 + (samplesEnd - .02) * (filled + sample.size) / std::max<uint64_t>(1, payload), "sound-samples");
                Fill(key, sample, sample.size, false);
                filled += sample.size;
                ClassicLoadProgress::Report(1.0);
                if (!ClassicLoadProgress::Active())
                    ClassicHost::Yield();
            }
            ClassicVfs::SetPreloadEnabled(true);
            ClassicLoadProgress::Range progress(samplesEnd, 1, "sound-designs");
            ClassicVfs::PreloadTrackDesigns();
        }
        else
        {
            ClassicVfs::SetPreloadEnabled(false);
            if (gPreloadMode != 0)
            {
                // Only tunes this park currently selects, before interaction.
                // Do not evict a warmed tune to fill the arena with another one.
                QueueParkMusic(true);
                uint64_t queuedBytes = 0, completedBytes = 0;
                for (const auto& key : gPrepareQueue)
                    queuedBytes += gSampleCache.at(key).size;
                while (!gPrepareQueue.empty())
                {
                    auto key = std::move(gPrepareQueue.front()); gPrepareQueue.pop_front();
                    auto& sample = gSampleCache.at(key);
                    ClassicLoadProgress::Range progress(
                        .02 + .90 * completedBytes / std::max<uint64_t>(1, queuedBytes),
                        .02 + .90 * (completedBytes + sample.size) / std::max<uint64_t>(1, queuedBytes), "sound-warm");
                    Fill(key, sample, sample.size, false);
                    completedBytes += sample.size;
                    ClassicLoadProgress::Report(1.0);
                    if (!ClassicLoadProgress::Active())
                        ClassicHost::Yield();
                }
                // Small loaded effects fill the remainder. Large or unadmitted
                // sounds stay demand-loaded, never partially preloaded in bulk.
                size_t effects = 0;
                for (const auto& source : gMixer->sources)
                    if (!source->released)
                        if (auto found = gSampleCache.find(source->cacheKey); found != gSampleCache.end()
                            && !Ready(found->second) && found->second.size <= 64 * 1024 && effects + found->second.size <= 1024 * 1024)
                            if (Fill(found->first, found->second, found->second.size, false)) effects += found->second.size;
            }
        }
        gPreloading = false; gPreloadComplete = true;
        ClassicLoadProgress::Report(1.0);
    }

    bool ClassicAudioSetSilentPlaybackTest(bool enabled)
    {
        if (enabled && gOutputLimit != 0.0f) return false;
        if (!enabled && gSilentPlaybackTest)
            for (auto& voice : gVoices)
            {
                VoiceQuiet(voice);
                voice.busy = false;
            }
        gSilentPlaybackTest = enabled;
        if (enabled)
        {
            gCallbacks = 0;
            gSilentPlaybackDeadline = NowMicroseconds() + 30000000;
        }
        return true;
    }

    void ClassicAudioBeginFrame()
    {
        if (!gFullPreload && gPreloadMode != 0) gFrameStart = NowMicroseconds();
    }

    void ClassicAudioTick()
    {
        FrameTiming::DetailScope timing(FrameTiming::current.audioPumpUs);
        if (gSilentPlaybackTest && NowMicroseconds() >= gSilentPlaybackDeadline)
            ClassicAudioSetSilentPlaybackTest(false);
        if (gMixer != nullptr) gMixer->Tick();
        ServiceOutput();
        if (gFullPreload || gPreloadMode == 0 || !gSampleArena) return;
        // A busy frame has no room for optional I/O. Keep mixer servicing
        // unconditional, but leave preparation for a frame with spare time.
        if (!gFrameStart || NowMicroseconds() - gFrameStart >= 20000) return;
        const auto tick = getGameState().currentTicks;
        if (tick - gLastScanTick >= 16) { gLastScanTick = tick; QueueParkMusic(false); }
        if (gPrepareQueue.empty()) return;
        // One bounded transfer after painting, never the whole music file here.
        // This remains synchronous I/O; counters make its residual cost visible.
        gPreloading = true;
        auto& sample = gSampleCache.at(gPrepareQueue.front());
        const bool complete = Fill(gPrepareQueue.front(), sample, 64 * 1024, false);
        gPreloading = false;
        if (complete || !sample.block) gPrepareQueue.pop_front();
    }

    void ClassicAudioSetOutputLimit(float limit)
    {
        if (gSilentPlaybackTest && limit > 0.0f) return;
        gOutputLimit = std::clamp(limit, 0.0f, 1.0f);
        gOutputMuted = gOutputLimit == 0;
        gLimitQ16 = static_cast<unsigned>(gOutputLimit * 65536.0f);
#ifdef OPENRCT2_CLASSIC_MAC
        if (gOutputChannel) SetTransportGain(gSubmittedLimitQ16);
#endif
    }

    bool ClassicAudioSetMixerBackend(unsigned backend, unsigned bits, unsigned blockFrames)
    {
        if (backend > 3 || (bits != 8 && bits != 16)
            || (blockFrames != 1024 && blockFrames != 2048 && blockFrames != 4096)) return false;
        if (backend == gMixerBackend && bits == gMixerBits && blockFrames == gMixerBlockFrames) return true;
        ClassicAudioStopAllForProbe();
        if (!CloseDevice()) return false;
        gClosing = false; gMixerBackend = backend; gMixerBits = bits; gMixerBlockFrames = blockFrames;
        gStreamWanted = false; gRingDirty = false;
        return true; // lazy open; selecting a backend never starts output by itself
    }

    void ClassicAudioTrace(bool enabled)
    {
        gTraceAudio = false;
        gCallbackTicks = gMaxCallbackTicks = gCallbackSamples = 0;
        gMaxProducerGap = 0; gMinimumReadyBlocks = ~0u;
        gCallbackLastEntry = gCallbackIntervals = 0;
        gCallbackIntervalTicks = 0; gCallbackIntervalMin = ~0u; gCallbackIntervalMax = 0;
#ifdef OPENRCT2_CLASSIC_MAC
        if (enabled)
        {
            if (!ClassicInterruptMask::Prepare()) return;
            if (!gAudioDriverServices)
            {
                Ptr mainAddress = nullptr; Str255 message{};
                if (GetSharedLibrary(reinterpret_cast<const unsigned char*>("\021DriverServicesLib"), kPowerPCCFragArch,
                    kLoadCFrag, &gAudioDriverServices, &mainAddress, message) == noErr)
                {
                    Ptr address = nullptr; CFragSymbolClass kind = 0;
                    if (FindSymbol(gAudioDriverServices, reinterpret_cast<const unsigned char*>("\025CurrentExecutionLevel"),
                        &address, &kind) == noErr && kind == kTVectorCFragSymbol)
                        gAudioExecutionLevel = reinterpret_cast<decltype(gAudioExecutionLevel)>(address);
                }
            }
        }
#endif
        gTraceAudio = enabled;
    }
    bool ClassicAudioSetProbeMode(unsigned mode)
    {
        if (mode > 3 || (mode >= 2 && !gMixerBackend)
            || (mode && gOutputLimit != 0.0f && gOutputLimit != 0.01f)) return false;
        ClassicAudioStopAllForProbe();
        if (!CloseDevice()) return false;
        gClosing = false; gStreamWanted = false; gRingDirty = false; gProbeMode = mode;
        if (mode >= 2)
        {
            OpenDevice();
            if (!gDeviceOpen) { gProbeMode = 0; return false; }
            if (mode == 3) StartOutput();
        }
        return true;
    }
    uint32_t ClassicAudioStallForProbe(unsigned milliseconds)
    {
        // Private functional/fixed-work aid: no gain/system-volume change.
        milliseconds = std::min(milliseconds, 200u);
        const uint64_t end = NowMicroseconds() + milliseconds * 1000;
        uint32_t value = 0x12345678;
        while (NowMicroseconds() < end)
        { value = (value << 7 | value >> 25) ^ (value * 1664525u + 1013904223u); __asm__ volatile("" : "+r"(value)); }
        return value;
    }

    ClassicAudioStats ClassicAudioGetStats()
    {
        ClassicAudioStats stats{};
        stats.peakGainUnits = gPeakGainUnits;
        stats.probeMode = gProbeMode;
        stats.volumeCommandErrors = gVolumeCommandErrors;
        stats.silentStarts = gSilentStarts;
        for (unsigned i = 0; i < std::size(gVoices); i++)
        {
            const auto& voice = gVoices[i];
            auto& info = stats.voices[i];
            info = {voice.busy, voice.compressed, voice.rate, voice.channels, voice.left, voice.right, voice.multiplier, -1, 0};
#ifdef OPENRCT2_CLASSIC_MAC
            if (voice.busy && voice.channel && Queries().channel)
                info.volumeQueryError = Queries().channel(voice.channel, siVolume, &info.queriedVolume);
#endif
        }
        stats.cacheBytes = gCacheBytes; stats.cacheHits = gCacheHits;
        for (const auto& [key, sample] : gSampleCache) if (Ready(sample)) stats.cacheEntries++;
        stats.preloadComplete = gPreloadComplete;
        stats.cacheCapacity = gSampleArena ? gSampleArena->capacity : 0;
        stats.cacheEvictions = gEvictions; stats.prepareReads = gPrepareReads; stats.demandReads = gDemandReads;
        stats.preloadMode = gPreloadMode; stats.fullPreload = gFullPreload; stats.admission = gAdmission;
        stats.residentBytes = gCacheBytes;
        if (gMixer != nullptr)
        {
            for (const auto& source : gMixer->sources)
            {
                stats.residentBytes += source->data.size();
                if (source->pending) { stats.pendingSources++; stats.pendingBytes += source->pending->GetLength() - 20; }
            }
            for (const auto& channel : gMixer->channels)
                if (channel->IsPlaying())
                {
                    if (channel->Group() == MixerGroup::rideMusic) stats.musicChannels++;
                    else stats.effectChannels++;
                }
        }
        stats.mixerBackend = gMixerBackend;
        stats.mixerRate = gMixerBackend == 3 ? 44100 : gMixerBackend ? 22050 : 0;
        stats.mixerChannels = gMixerBackend == 2 ? 1 : gMixerBackend ? 2 : 0;
        stats.hardwareFrames = gHardwareFrames; stats.hardwareRateFixed = gHardwareRateFixed;
        stats.hardwareBits = gHardwareBits; stats.hardwareChannels = gHardwareChannels; stats.hardwareQueryError = gHardwareQueryError;
        stats.callbackTicks = gCallbackTicks; stats.maxCallbackTicks = gMaxCallbackTicks; stats.callbackSamples = gCallbackSamples;
        stats.callbackLevel = gCallbackLevel; stats.callbackMask = gCallbackMask; stats.audioTrace = gTraceAudio;
        stats.callbackIntervalTicks = gCallbackIntervalTicks; stats.callbackIntervals = gCallbackIntervals;
        stats.callbackIntervalMin = gCallbackIntervals ? gCallbackIntervalMin : 0;
        stats.callbackIntervalMax = gCallbackIntervalMax;
        stats.rebuiltBlocks = gRebuiltBlocks;
        stats.mixerBits = gMixerBits; stats.mixerBlockFrames = gMixerBlockFrames;
        stats.readyBlocks = gSoftwareStream ? gSoftwareStream->Ready() : 0;
        stats.targetBlocks = gSoftwareStream ? gSoftwareStream->Target() : 0;
        stats.lookaheadFrames = gSoftwareStream ? gSoftwareStream->LookaheadFrames() : 0;
        stats.minimumReadyBlocks = gMinimumReadyBlocks == ~0u ? 0 : unsigned(gMinimumReadyBlocks);
        stats.maxProducerGapMicros = gMaxProducerGap; stats.replacementDeferrals = gReplacementDeferrals;
        stats.bufferedBlocks = gSoftwareStream ? gSoftwareStream->Buffered() : 0;
        stats.underruns = gUnderruns; stats.mixMicros = gMixMicros;
        stats.mixedBlocks = gMixedBlocks; stats.mixedFrames = gMixedFrames; stats.queueErrors = gQueueErrors;
        stats.started = gStarted;
        stats.callbacks = gCallbacks;
        stats.silentPlaybackTest = gSilentPlaybackTest;
        stats.channelsAllocated = gAllocated;
        stats.channelsDisposed = gDisposed;
        stats.refused = gRefused;
        stats.sourcesLoaded = gSourcesLoaded;
        stats.sourcesFailed = gSourcesFailed;
        stats.playRequests = gPlayRequests;
        for (const auto& voice : gVoices)
            stats.playing += voice.busy ? 1 : 0;
        stats.outputLimitPercent = static_cast<int>(gOutputLimit * 100.0f + 0.5f);
        stats.systemVolumeLeft = stats.systemVolumeRight = -1;
#ifdef OPENRCT2_CLASSIC_MAC
        stats.outputRateError = stats.qualityError = -1;
        if (Queries().output)
        {
            stats.outputRateError = Queries().output(nullptr, siSampleRate, &stats.outputRateFixed);
            stats.qualityError = Queries().output(nullptr, siQuality, &stats.quality);
        }
        long level = 0;
        if (GetDefaultOutputVolume(&level) == noErr)
        {
            // 256 is full volume.
            stats.systemVolumeLeft = static_cast<int>(level & 0xFFFF);
            stats.systemVolumeRight = static_cast<int>((level >> 16) & 0xFFFF);
        }
#endif
        return stats;
    }
    ClassicAudioFrame ClassicAudioGetFrame()
    {
        ClassicAudioFrame frame{};
        for (const auto& voice : gVoices) if (voice.busy)
        {
            frame.voices++;
            if (voice.compressed) frame.ima++; else frame.pcm++;
            if (voice.channels == 1) frame.mono++; else frame.stereo++;
            if (voice.rate == 22050) frame.rate22050++;
            if (voice.rate == 44100) frame.rate44100++;
        }
        frame.started = gStarted; frame.refused = gRefused; frame.callbacks = gCallbacks;
        frame.prepareReads = gPrepareReads; frame.demandReads = gDemandReads;
        return frame;
    }
    void ClassicAudioStopAllForProbe()
    {
        if (gMixer) for (auto& channel : gMixer->channels) channel->Stop();
        for (auto& voice : gVoices) { VoiceQuiet(voice); voice.busy = false; }
    }
} // namespace OpenRCT2::Audio
