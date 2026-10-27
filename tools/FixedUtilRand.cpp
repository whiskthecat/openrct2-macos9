// Opt-in Linux rctsim oracle only. Production UtilRand and its seed are untouched.
// Ride tune IDs are serialized but use UtilRand, separate from ScenarioRandState.
#include <cstdint>
#include <random>

extern "C" uint32_t __wrap__Z8UtilRandv()
{
    static std::mt19937 random(0x53494D43);
    return static_cast<uint32_t>(random());
}
