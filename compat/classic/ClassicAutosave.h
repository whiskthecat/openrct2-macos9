// Freeze one tick, then prepare, encode and transfer bounded batches per frame.
// Previous successful saves are untouched until a checked, durable new file is published.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
namespace OpenRCT2 { class MemoryStream; }
namespace ClassicAutosave
{
    bool Enabled();
    void SetEnabled(bool enabled);
    bool Pending();
    bool Pumping();
    void Queue(std::function<void()> request);
    void Begin(std::string path, OpenRCT2::MemoryStream&& bytes, uint64_t serializeUs);
    // prepare owns all inputs and returns canonical body plus PARK header/table.
    void BeginExport(std::string path, std::function<bool(OpenRCT2::MemoryStream&, OpenRCT2::MemoryStream&)> prepare,
                     uint64_t captureBytes, uint64_t captureUs, uint32_t tick, bool replace,
                     std::function<size_t(const OpenRCT2::MemoryStream&, uint64_t, uint8_t*, size_t)> read = {});
    void OnFailure(std::function<void(const std::string&)> callback);
    void OnSuccess(std::function<void(const std::string&)> callback);
    void Prune(const std::string& directory, unsigned keep, const std::string& newest);
    void Pump();
    // False means this drain failed; callers must keep the live park.
    bool Drain();
    std::string Status();
}
