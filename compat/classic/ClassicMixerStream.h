// Prepared PCM ring: main task owns the mixer; interrupt consumer owns no voice data.
#pragma once
#include "ClassicSoftwareMixer.h"
#include <atomic>

namespace ClassicSoftwareMixer
{
    // A prepared buffer already includes its capture limit. Never amplify it
    // when a new limit rises, and attenuate it immediately when the limit falls.
    constexpr unsigned TransportGain(unsigned limit, unsigned captured)
    { return !limit ? 0 : !captured || limit >= captured ? 256 : limit * 256 / captured; }
    class Stream
    {
    public:
        static constexpr unsigned Blocks = 32, MaxFrames = 4096;
        // Cover a 150-ms picture plus up to 50 ms since the last service.
        static constexpr unsigned ProducerBudgetMs = 200;
        // siHardwareFormat describes DMA, not the Sound Manager's queued
        // conversion/mix pipeline. Retain the conservative two-4096-frame
        // fallback even when the device reports a smaller DMA quantum.
        static constexpr unsigned MinLookaheadAt44100 = 8192;
        struct Block
        {
            alignas(4) int16_t samples[MaxFrames * 2]{};
            Mixer::Timeline before{}, after{};
            uint32_t sequence = 0;
            unsigned slot = 0, gainTag = 0;
            bool ends = false;
            const uint8_t* Bytes() const { return reinterpret_cast<const uint8_t*>(samples); }
        };
    private:
        Mixer _mixer;
        unsigned _frames, _bits, _target = 0, _lookahead = 0, _gainTag = 0;
        std::array<Block, Blocks * 2> _blocks{};
        std::array<Block*, Blocks> _order{}, _replacement{};
        Mixer::Timeline _controlTimeline{};
        uint32_t _replacementSequence = 0;
        unsigned _replacementCount = 0, _mixedBlocks = 0;
        void Mix(Block& block, bool silence);
        // Each has one writer; acquire/release keeps payload outside IRQ metadata.
        std::atomic<uint32_t> _published{0}, _submitted{0}, _completed{0};
    public:
        Stream(uint32_t rate, unsigned channels, unsigned bits, unsigned frames);
        Mixer& Core() { return _mixer; }
        void SetGainTag(unsigned value) { _gainTag = value; }
        unsigned MixedBlocks() const { return _mixedBlocks; }
        unsigned Frames() const { return _frames; }
        unsigned Bits() const { return _bits; }
        unsigned Target() const { return _target; }
        unsigned LookaheadFrames() const { return _lookahead; }
        unsigned Ready() const { return _published.load() - _submitted.load(); }
        unsigned Buffered() const { return _published.load() - _completed.load(); }
        Block& At(unsigned slot) { return _blocks[slot]; }
        // Main task only. Commit and TakeReady must not overlap.
        // Build away from the published ring; commit only metadata under the
        // adapter's short mask. A missed commit leaves old audio available.
        void BuildReplacement(bool silence = false);
        bool CommitReplacement();
        void CancelReplacement();
        bool Prepare(bool silence = false);
        bool SetHardwareReserve(unsigned frames);
        // Consumer only (or initial kick while output is stopped).
        Block* TakeReady();
        void Complete(const Block& block) { _completed.store(block.sequence + 1, std::memory_order_release); }
        Mixer::Timeline CompletedTimeline() const;
    };
}
