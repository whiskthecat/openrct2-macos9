#pragma once
// Strict adapter for two documented port capacity limits. No represented field
// is discarded. The oracle's unavailable capacity must contain exact defaults.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>
namespace ParityEncoding {
using Bytes = std::vector<uint8_t>;
inline uint32_t u32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
template <class T> void integer(Bytes &out, T v) {
    const uint64_t bits = static_cast<uint64_t>(v);
    for (unsigned i = 0; i < (sizeof(T) > 4 ? 8 : 4); ++i)
        out.push_back(uint8_t(bits >> (8 * i)));
}
inline void array(Bytes &out, uint32_t count, uint32_t size) {
    integer(out, count);
    integer(out, size);
}
inline void require(bool v, const char *why) {
    if (!v)
        throw std::runtime_error(why);
}
inline Bytes tiles(const uint8_t *p, size_t size, int grass) {
    require(size >= 12, "short tile chunk");
    const auto count = u32(p + 8);
    require(size == 12 + 16 * size_t(count), "tile chunk length");
    // Port capacity is 256²; stock's technical grid is 1001². Logical map sizes
    // and every represented tile byte remain unchanged, including outside-map
    // tiles.
    if (count < 1001 * 1001)
        return Bytes(p, p + size);
    Bytes out(p, p + 12);
    size_t at = 12;
    uint32_t kept = 0;
    uint8_t expected[16] = {0, 128, 2, 2, 0, 0, 0, uint8_t(grass)};
    for (unsigned y = 0; y < 1001; ++y)
        for (unsigned x = 0; x < 1001; ++x) {
            bool last = false;
            do {
                require(at + 16 <= size, "tile grid truncated");
                last = (p[at + 1] & 128) != 0;
                if (x < 256 && y < 256) {
                    out.insert(out.end(), p + at, p + at + 16);
                    ++kept;
                } else
                    require(last && std::memcmp(p + at, expected, 16) == 0,
                            "oracle has state outside the documented 256-square capacity");
                at += 16;
            } while (!last);
        }
    require(at == size, "tile grid trailing state");
    for (unsigned i = 0; i < 4; ++i)
        out[8 + i] = uint8_t(kept >> (i * 8));
    return out;
}
template <class R> Bytes ridePrefix(const R &r) {
    Bytes b;
    integer(b, r.id.ToUnderlying());
    integer(b, r.type);
    integer(b, r.subtype);
    integer(b, r.mode);
    integer(b, r.status);
    integer(b, r.departFlags);
    integer(b, r.flags.holder);
    b.insert(b.end(), r.customName.begin(), r.customName.end());
    b.push_back(0);
    integer(b, r.defaultNameNumber);
    array(b, std::size(r.price), 8);
    for (auto v : r.price)
        integer(b, v);
    integer(b, r.entranceStyle);
    integer(b, r.vehicleColourSettings);
    array(b, std::size(r.trackColours), 12);
    for (auto c : r.trackColours) {
        integer(b, c.main);
        integer(b, c.additional);
        integer(b, c.supports);
    }
    array(b, std::size(r.vehicleColours), 12);
    for (auto c : r.vehicleColours) {
        integer(b, c.Body);
        integer(b, c.Trim);
        integer(b, c.Tertiary);
    }
    integer(b, r.numStations);
    return b;
}
template <class Range>
Bytes rides(const uint8_t *p, size_t size, Range &&rides, const std::map<unsigned, uint32_t> &extraDepart) {
    require(size >= 8, "short ride chunk");
    const auto count = u32(p);
    require(count == rides.size(), "ride record count mismatch");
    // Each station consists of exactly 19 four-byte Orca values at v62.
    Bytes empty;
    for (int32_t v : {-32768, 0, 0, 0, 0, 255, -32768, 0, 0, 255, -32768, 0, 0, 255, 0, 0, 0, 0, 65535})
        integer(empty, v);
    Bytes out;
    size_t consumed = 0;
    unsigned matched = 0;
    for (const auto &r : rides) {
        auto expectedEmpty = empty;
        const auto found = extraDepart.find(r.id.ToUnderlying());
        const uint32_t depart = found == extraDepart.end() ? 0 : found->second;
        // initialiseVehicles sets depart=1 in every empty slot too. Track that
        // exact transition from the action history, without accepting arbitrary
        // bytes.
        for (unsigned j = 0; j < 4; ++j)
            expectedEmpty[16 + j] = uint8_t(depart >> (8 * j));
        const auto prefix = ridePrefix(r);
        auto start = std::search(p + consumed, p + size, prefix.begin(), prefix.end());
        require(start != p + size, "ride schema prefix mismatch");
        const size_t at = start - p + prefix.size();
        require(at + 8 <= size, "station array truncated");
        const auto n = u32(p + at), stride = u32(p + at + 4);
        require((n == 4 || n == 255) && stride == 76, "station schema/capacity changed");
        require(at + 8 + size_t(n) * stride <= size, "station array length");
        if (n == 255) {
            for (unsigned j = 4; j < n; ++j)
                require(std::equal(expectedEmpty.begin(), expectedEmpty.end(), p + at + 8 + j * stride),
                        "oracle has nonempty station above the documented four-station "
                        "capacity");
            out.insert(out.end(), p + consumed, p + at);
            array(out, 4, 76);
            out.insert(out.end(), p + at + 8, p + at + 8 + 4 * stride);
            consumed = at + 8 + n * stride;
        } else {
            out.insert(out.end(), p + consumed, p + at + 8 + n * stride);
            consumed = at + 8 + n * stride;
        }
        ++matched;
    }
    require(matched == count, "station adapter count");
    out.insert(out.end(), p + consumed, p + size);
    // The outer ride array has a fixed stride only when all records have equal
    // length; dropping the same station capacity from each record preserves that.
    if (count && u32(out.data() + 4)) {
        const uint32_t stride = u32(out.data() + 4) - (size == out.size() ? 0 : (255 - 4) * 76);
        for (unsigned i = 0; i < 4; ++i)
            out[4 + i] = uint8_t(stride >> (8 * i));
    }
    return out;
}
} // namespace ParityEncoding
