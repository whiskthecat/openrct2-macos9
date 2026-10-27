// Fixed-format application mixer. No OS services, allocation, or floating point.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace ClassicSoftwareMixer
{
    constexpr unsigned MaxVoices = 32;
    constexpr unsigned MaxBlockFrames = 512;
    enum class Encoding { Pcm16BigEndian, Ima4 };
    struct Source
    {
        const uint8_t* bytes = nullptr;
        size_t size = 0;
        uint32_t frames = 0, rate = 22050;
        unsigned channels = 1;
        Encoding encoding = Encoding::Pcm16BigEndian;
    };
    // Caller owns source storage until Stop()/Finished(); controls and Render()
    // must be serialized. The Mac adapter uses a short interrupt mask for controls.
    class Mixer
    {
        struct Voice
        {
            Source source{};
            bool playing = false, enabled = false, loop = false;
            uint32_t generation = 0;
            uint32_t originFrame = 0, frame = 0, fraction = 0, stepWhole = 1, stepFraction = 0;
            uint32_t loopStepWhole = 0;
            unsigned left = 0, right = 0; // Q8, unity = 256
            uint32_t packets[2] = {~0u, ~0u};
            int16_t decoded[2][128]{};
        };
        std::array<Voice, MaxVoices> _voices{};
        std::array<int32_t, MaxBlockFrames * 2> _sum{};
        uint32_t _rate;
        unsigned _channels;
        void LoadImaPacket(Voice& voice, uint32_t packet);
        template<bool Ima, bool SourceStereo, bool OutputStereo> void MixVoice(Voice& voice, unsigned frames);
        static void AdvanceSilent(Voice& voice, unsigned frames);
    public:
        struct Cursor { uint32_t frame, fraction, generation; bool playing; };
        using Timeline = std::array<Cursor, MaxVoices>;
        Timeline SaveTimeline() const;
        void RestoreTimeline(const Timeline& timeline, bool rewindNew = false);
        explicit Mixer(uint32_t rate = 22050, unsigned channels = 2) : _rate(rate), _channels(channels) {}
        bool Start(unsigned slot, Source source, uint32_t startFrame, bool loop);
        void Stop(unsigned slot) { _voices[slot].enabled = _voices[slot].playing = false; }
        void SetGain(unsigned slot, unsigned left, unsigned right);
        void SetRate(unsigned slot, uint32_t multiplierQ16);
        bool Finished(unsigned slot) const { return !_voices[slot].playing; }
        uint32_t Position(unsigned slot) const { return _voices[slot].frame; }
        bool HasVoices() const;
        // Native-endian signed PCM16 output, interleaved if stereo; <=512 frames.
        void Render(int16_t* output, unsigned frames);
        uint32_t Rate() const { return _rate; }
        unsigned Channels() const { return _channels; }
    };
    void DecodeIma4Packet(const uint8_t* packet, int16_t* output, unsigned stride = 1);
}
