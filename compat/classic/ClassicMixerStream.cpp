#include "ClassicMixerStream.h"
#include <algorithm>
#include <cstring>

namespace ClassicSoftwareMixer
{
    Stream::Stream(uint32_t rate, unsigned channels, unsigned bits, unsigned frames)
        : _mixer(rate, channels), _frames(frames), _bits(bits),
          _target(0)
    {
        SetHardwareReserve(0);
        for (unsigned i = 0; i < _blocks.size(); ++i) _blocks[i].slot = i;
        for (unsigned i = 0; i < Blocks; ++i) _order[i] = &_blocks[i];
    }
    bool Stream::SetHardwareReserve(unsigned frames)
    {
        const unsigned lookahead = std::max(frames,
            unsigned((uint64_t(_mixer.Rate()) * MinLookaheadAt44100 + 44099) / 44100));
        const unsigned target = (_mixer.Rate() * ProducerBudgetMs / 1000 + lookahead + _frames - 1) / _frames + 1;
        if (target >= Blocks) return false;
        _target = target; _lookahead = lookahead; return true;
    }
    void Stream::Mix(Block& block, bool silence)
    {
        block.gainTag = _gainTag;
        block.before = _mixer.SaveTimeline();
        for (unsigned done = 0; done < _frames; done += MaxBlockFrames)
            _mixer.Render(block.samples + done * _mixer.Channels(), std::min(MaxBlockFrames, _frames - done));
        block.after = _mixer.SaveTimeline(); block.ends = !_mixer.HasVoices(); _mixedBlocks++;
        if (_bits == 8)
        {
            auto* bytes = reinterpret_cast<uint8_t*>(block.samples);
            for (unsigned i = 0; i < _frames * _mixer.Channels(); ++i)
                bytes[i] = uint8_t((int(block.samples[i]) >> 8) + 128);
        }
        if (silence) std::memset(block.samples, _bits == 8 ? 128 : 0, _frames * _mixer.Channels() * (_bits / 8));
    }
    void Stream::BuildReplacement(bool silence)
    {
        _controlTimeline = _mixer.SaveTimeline();
        const auto submitted = _submitted.load(std::memory_order_acquire);
        const auto published = _published.load(std::memory_order_acquire);
        _replacementSequence = submitted; _replacementCount = 0;
        if (submitted != published) _mixer.RestoreTimeline(_order[submitted % Blocks]->before, true);
        // One bank's worth of private blocks is always free. No live/queued
        // PCM is written, even when a callback fires during this entire mix.
        for (auto& block : _blocks)
        {
            if (!_mixer.HasVoices() || submitted + _replacementCount - _completed.load(std::memory_order_acquire) >= _target) break;
            bool mapped = false;
            for (const auto* existing : _order) mapped = mapped || existing == &block;
            if (mapped) continue;
            block.sequence = submitted + _replacementCount;
            Mix(block, silence);
            _replacement[_replacementCount++] = &block;
        }
    }
    bool Stream::CommitReplacement()
    {
        if (_submitted.load(std::memory_order_acquire) != _replacementSequence) return false;
        for (unsigned i = 0; i < _replacementCount; ++i)
            _order[(_replacementSequence + i) % Blocks] = _replacement[i];
        _published.store(_replacementSequence + _replacementCount, std::memory_order_release);
        return true;
    }
    void Stream::CancelReplacement() { _mixer.RestoreTimeline(_controlTimeline); }
    bool Stream::Prepare(bool silence)
    {
        const auto published = _published.load(std::memory_order_relaxed);
        const auto completed = _completed.load(std::memory_order_acquire);
        // Leave the last completed block intact for task-time retirement.
        if (published - completed >= _target || published - completed >= Blocks - 1 || !_mixer.HasVoices()) return false;
        auto& block = *_order[published % Blocks];
        block.sequence = published; Mix(block, silence);
        _published.store(published + 1, std::memory_order_release);
        return true;
    }
    Stream::Block* Stream::TakeReady()
    {
        const auto submitted = _submitted.load(std::memory_order_relaxed);
        if (submitted == _published.load(std::memory_order_acquire)) return nullptr;
        auto* block = _order[submitted % Blocks];
        _submitted.store(submitted + 1, std::memory_order_release);
        return block;
    }
    Mixer::Timeline Stream::CompletedTimeline() const
    {
        const auto completed = _completed.load(std::memory_order_acquire);
        return completed ? _order[(completed - 1) % Blocks]->after : Mixer::Timeline{};
    }
}
