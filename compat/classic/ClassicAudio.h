// Sound for the Mac OS 9 build: the engine's audio interface on top of the Sound Manager.
#pragma once

#include <memory>
#include <cstdint>
#include "ClassicVfs.h"
#include <array>

namespace OpenRCT2::Audio
{
    struct IAudioContext;
    [[nodiscard]] std::unique_ptr<IAudioContext> CreateClassicAudioContext();
    // Once per frame: apply volume settings that changed and release finished voices.
    void ClassicAudioBeginFrame(); // marks the budget for optional idle reads
    void ClassicAudioTick();
    void ClassicAudioSetPreloadEnabled(bool enabled);
    void ClassicAudioSetPreloadMode(unsigned mode); // 0 off, 1 on, 2 automatic
    void ClassicAudioBeforeParkLoad();
    void ClassicAudioPreload();
    double ClassicAudioPreloadEstimateMs();
    // For measurements: {"voices": playing now, "started": total, "refused": no free voice}
    struct ClassicAudioStats
    {
        struct VoiceInfo
        {
            bool playing, compressed;
            uint32_t rate;
            unsigned channels, left, right;
            double multiplier;
            int volumeQueryError;
            uint32_t queriedVolume;
        };
        std::array<VoiceInfo, 16> voices{};
        unsigned peakGainUnits, volumeCommandErrors, silentStarts;
        unsigned probeMode; // opt-in isolation: 0 normal, 1 refuse, 2 open idle, 3 zero-PCM clock
        uint32_t outputRateFixed;
        int outputRateError, qualityError;
        uint32_t quality;
        int playing;
        unsigned readyBlocks, targetBlocks, lookaheadFrames, minimumReadyBlocks, replacementDeferrals;
        uint64_t maxProducerGapMicros;
        unsigned mixerBackend, mixerRate, mixerChannels, mixerBits, mixerBlockFrames, bufferedBlocks;
        unsigned long underruns, rebuiltBlocks;
        uint32_t hardwareFrames, hardwareRateFixed, callbackTicks, maxCallbackTicks, callbackSamples;
        uint64_t callbackIntervalTicks;
        uint32_t callbackIntervals, callbackIntervalMin, callbackIntervalMax;
        unsigned hardwareBits, hardwareChannels, callbackLevel, callbackMask;
        int hardwareQueryError;
        bool audioTrace;
        uint64_t mixMicros;
        unsigned long mixedBlocks, mixedFrames, queueErrors;
        uint64_t residentBytes, pendingBytes, cacheBytes, cacheHits;
        unsigned cacheEntries;
        bool preloadComplete;
        unsigned pendingSources, musicChannels, effectChannels;
        uint64_t cacheCapacity, cacheEvictions, prepareReads, demandReads;
        unsigned preloadMode;
        bool fullPreload;
        ClassicHost::MemoryInfo admission;
        unsigned long callbacks;
        bool silentPlaybackTest;
        unsigned long channelsAllocated, channelsDisposed;
        unsigned long started, refused, sourcesLoaded, sourcesFailed, playRequests;
        int outputLimitPercent;                  // our safety limit (0 = muted)
        int systemVolumeLeft, systemVolumeRight; // the Mac's own output volume, 256 = full, -1 = unknown
    };
    // Safety limit on our output, 0..1 (0 = no sound at all). Default 0.25.
    void ClassicAudioSetOutputLimit(float limit);
    // Opt-in hardware validation while the output limit remains exactly zero.
    bool ClassicAudioSetSilentPlaybackTest(bool enabled);
    // Stops current voices; 0 legacy, 1 app 22050 stereo, 2 app 22050 mono, 3 app 44100 stereo.
    bool ClassicAudioSetMixerBackend(unsigned backend, unsigned bits = 16, unsigned blockFrames = 1024);
    void ClassicAudioTrace(bool enabled);
    // Diagnostics only: stops/closes previous output. Modes 2/3 require an app backend;
    // mode 3 submits only immutable zero PCM (unity transport at 1%, zero when muted); no voices.
    bool ClassicAudioSetProbeMode(unsigned mode);
    uint32_t ClassicAudioStallForProbe(unsigned milliseconds);
    ClassicAudioStats ClassicAudioGetStats();
    struct ClassicAudioFrame
    {
        unsigned voices, ima, pcm, mono, stereo, rate22050, rate44100;
        unsigned long started, refused, callbacks;
        uint64_t prepareReads, demandReads;
    };
    // Fixed-cost snapshot for quiet captures; no Sound Manager queries or allocations.
    ClassicAudioFrame ClassicAudioGetFrame();
    // Explicit diagnostic isolation, including engine-retained channels.
    void ClassicAudioStopAllForProbe();
} // namespace OpenRCT2::Audio
