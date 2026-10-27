#include "ClassicSoftwareMixer.h"
#include <algorithm>
#include <cstring>

namespace ClassicSoftwareMixer
{
    void DecodeIma4Packet(const uint8_t* packet, int16_t* output, unsigned stride)
    {
        static constexpr int steps[89] = {
            7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,
            118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,
            876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,
            4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,
            18500,20350,22385,24623,27086,29794,32767};
        static constexpr int change[8] = {-1,-1,-1,-1,2,4,6,8};
        const unsigned header = (unsigned(packet[0]) << 8) | packet[1];
        int predictor = int16_t(header & 0xff80), index = std::min(int(header & 127), 88);
        for (unsigned i = 0; i < 64; ++i)
        {
            const unsigned nibble = (packet[2 + i / 2] >> ((i & 1) * 4)) & 15;
            const int step = steps[index];
            int delta = step >> 3;
            if (nibble & 1) delta += step >> 2;
            if (nibble & 2) delta += step >> 1;
            if (nibble & 4) delta += step;
            predictor = std::clamp(predictor + ((nibble & 8) ? -delta : delta), -32768, 32767);
            index = std::clamp(index + change[nibble & 7], 0, 88);
            output[i * stride] = int16_t(predictor);
        }
    }

    bool Mixer::Start(unsigned slot, Source source, uint32_t startFrame, bool loop)
    {
        if (slot >= MaxVoices || !source.bytes || !source.frames || source.frames > 0x7fffffffu
            || !source.rate || (source.channels != 1 && source.channels != 2)
            || (_channels != 1 && _channels != 2) || !_rate) return false;
        const uint64_t required = source.encoding == Encoding::Ima4
            ? ((uint64_t(source.frames) + 63) / 64) * 34 * source.channels
            : uint64_t(source.frames) * source.channels * 2;
        if (required > source.size) return false;
        auto& voice = _voices[slot];
        const auto generation = voice.generation + 1;
        voice = Voice{};
        voice.generation = generation;
        voice.enabled = true;
        voice.source = source;
        voice.loop = loop;
        voice.frame = std::min(startFrame, source.frames);
        if (loop && voice.frame == source.frames) voice.frame = 0;
        voice.originFrame = voice.frame;
        voice.playing = voice.frame < source.frames;
        SetRate(slot, 65536);
        return true;
    }
    void Mixer::SetGain(unsigned slot, unsigned left, unsigned right)
    {
        _voices[slot].left = std::min(left, 256u);
        _voices[slot].right = std::min(right, 256u);
    }
    void Mixer::SetRate(unsigned slot, uint32_t multiplierQ16)
    {
        auto& voice = _voices[slot];
        // Control time only: no division or floating point in the sample loop.
        const uint64_t step = uint64_t(voice.source.rate) * std::max(multiplierQ16, 1u) / _rate;
        voice.stepWhole = uint32_t(std::min(step >> 16, uint64_t(0xffffffffu)));
        voice.stepFraction = uint32_t(step) & 65535;
        voice.loopStepWhole = voice.source.frames ? voice.stepWhole % voice.source.frames : 0;
    }
    void Mixer::LoadImaPacket(Voice& voice, uint32_t packet)
    {
        const unsigned bank = packet & 1;
        for (unsigned c = 0; c < voice.source.channels; ++c)
            DecodeIma4Packet(voice.source.bytes + (size_t(packet) * voice.source.channels + c) * 34,
                voice.decoded[bank] + c, voice.source.channels);
        voice.packets[bank] = packet;
    }
    void Mixer::AdvanceSilent(Voice& voice, unsigned frames)
    {
        const uint64_t fraction = uint64_t(voice.fraction) + uint64_t(voice.stepFraction) * frames;
        const uint64_t end = voice.frame + uint64_t(voice.stepWhole) * frames + (fraction >> 16);
        voice.fraction = uint32_t(fraction) & 65535;
        if (voice.loop) voice.frame = uint32_t(end % voice.source.frames);
        else
        {
            voice.frame = uint32_t(std::min(end, uint64_t(voice.source.frames)));
            voice.playing = end < voice.source.frames;
        }
    }
    Mixer::Timeline Mixer::SaveTimeline() const
    {
        Timeline timeline{};
        for (unsigned i = 0; i < MaxVoices; ++i)
            timeline[i] = {_voices[i].frame, _voices[i].fraction, _voices[i].generation, _voices[i].playing};
        return timeline;
    }
    void Mixer::RestoreTimeline(const Timeline& timeline, bool rewindNew)
    {
        for (unsigned i = 0; i < MaxVoices; ++i)
        {
            auto& v = _voices[i];
            if (v.enabled && v.generation == timeline[i].generation)
            { v.frame = timeline[i].frame; v.fraction = timeline[i].fraction; v.playing = timeline[i].playing; }
            else if (v.enabled && rewindNew)
            {
                // The first unsubmitted block predates this voice. Even if a
                // deferred update already mixed it into a later tail block,
                // none of its samples has reached the consumer yet.
                v.frame = v.originFrame; v.fraction = 0; v.playing = v.frame < v.source.frames;
            }
        }
    }
    bool Mixer::HasVoices() const
    {
        for (const auto& voice : _voices) if (voice.playing) return true;
        return false;
    }
    template<bool Ima, bool SourceStereo, bool OutputStereo>
    void Mixer::MixVoice(Voice& voice, unsigned frames)
    {
        // Resolve immutable metadata once per voice/block, and keep the cursor
        // in registers. The common PCM loop does not call the IMA dispatcher.
        const uint8_t* bytes = voice.source.bytes;
        const uint32_t length = voice.source.frames, whole = voice.stepWhole, loopWhole = voice.loopStepWhole;
        const uint32_t stepFraction = voice.stepFraction;
        const bool loop = voice.loop;
        const int gainL = int(voice.left), gainR = int(voice.right);
        uint32_t frame = voice.frame, fraction = voice.fraction;
        auto sample = [&](uint32_t at, unsigned channel) {
            if constexpr (Ima)
            {
                const uint32_t packet = at >> 6, bank = packet & 1;
                if (voice.packets[bank] != packet) LoadImaPacket(voice, packet);
                return int(voice.decoded[bank][(at & 63) * (SourceStereo ? 2 : 1) + channel]);
            }
            else
            {
                const uint8_t* p = bytes + (size_t(at) * (SourceStereo ? 2 : 1) + channel) * 2;
                return int(int16_t((unsigned(p[0]) << 8) | p[1]));
            }
        };
        bool playing = true;
        for (unsigned i = 0; i < frames; ++i)
        {
            int left = sample(frame, 0), right = left;
            if constexpr (SourceStereo) right = sample(frame, 1);
            if (fraction)
            {
                uint32_t next = frame + 1;
                if (next == length) next = loop ? 0 : frame;
                const int f = int(fraction >> 8);
                left += ((sample(next, 0) - left) * f) >> 8;
                if constexpr (SourceStereo) right += ((sample(next, 1) - right) * f) >> 8;
                else right = left;
            }
            const int l = left * gainL, r = right * gainR;
            if constexpr (OutputStereo) { _sum[i * 2] += l >> 8; _sum[i * 2 + 1] += r >> 8; }
            else _sum[i] += (l + r) >> 9;
            fraction += stepFraction;
            const uint32_t carry = fraction >> 16;
            fraction &= 65535;
            if (loop)
            {
                frame += loopWhole + carry;
                if (frame >= length) frame -= length;
            }
            else
            {
                // No overflowing whole+carry, or 64-bit comparison, per sample.
                if (whole >= length - frame) { frame = length; playing = false; break; }
                frame += whole + carry;
                if (frame == length) { playing = false; break; }
            }
        }
        voice.frame = frame; voice.fraction = fraction; voice.playing = playing;
    }
    void Mixer::Render(int16_t* output, unsigned frames)
    {
        if (frames > MaxBlockFrames) return;
        std::memset(_sum.data(), 0, frames * _channels * sizeof(int32_t));
        for (auto& voice : _voices)
        {
            if (!voice.playing) continue;
            if (!(voice.left | voice.right)) { AdvanceSilent(voice, frames); continue; }
            const unsigned kind = (voice.source.encoding == Encoding::Ima4 ? 4 : 0)
                | (voice.source.channels == 2 ? 2 : 0) | (_channels == 2 ? 1 : 0);
            switch (kind)
            {
                case 0: MixVoice<false,false,false>(voice,frames); break;
                case 1: MixVoice<false,false,true>(voice,frames); break;
                case 2: MixVoice<false,true,false>(voice,frames); break;
                case 3: MixVoice<false,true,true>(voice,frames); break;
                case 4: MixVoice<true,false,false>(voice,frames); break;
                case 5: MixVoice<true,false,true>(voice,frames); break;
                case 6: MixVoice<true,true,false>(voice,frames); break;
                case 7: MixVoice<true,true,true>(voice,frames); break;
            }
        }
        for (unsigned i = 0; i < frames * _channels; ++i)
            output[i] = int16_t(std::clamp(_sum[i], -32768, 32767));
    }
}
