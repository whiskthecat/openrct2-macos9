// Asset-free checks for stored flag layout and the shared RCT1 byte/word aliases.
#include <openrct2/rct1/RCT1.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/util/Util.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include "BuildIdentity.h"

using namespace OpenRCT2;

static_assert(offsetof(RCT1::S4, parkFlags) == 0x19883C);
static_assert(offsetof(RCT1::S4, GameVersion) == 0x199C14);
static_assert(sizeof(decltype(RCT1::S4::parkFlags)) == 4);
static_assert(!kHostIsBigEndian || kIsEndianNeutral<decltype(RCT1::S4::parkFlags)>);
static_assert(sizeof(BannerFlags) == 1 && sizeof(FountainFlags) == 1);

static bool check(bool condition, const char* label)
{
    if (!condition)
        std::fprintf(stderr, "FAIL %s\n", label);
    return condition;
}

int main()
{
    if (!RctBuild::Start())
        return 125;
    bool pass = true;
    auto s4 = std::make_unique<RCT1::S4>();
    for (unsigned bit = 0; bit < 32; bit++)
    {
        const uint32_t expected = uint32_t{ 1 } << bit;
        uint8_t bytes[4];
        for (unsigned i = 0; i < 4; i++)
            bytes[i] = static_cast<uint8_t>(expected >> (8 * i));
        std::memcpy(&s4->parkFlags, bytes, sizeof(bytes));
        const RCT1::ParkFlags flags{ static_cast<uint32_t>(s4->parkFlags) };
        pass &= check(flags.holder == expected, "every stored park flag bit");
    }
    RCT1::Peep source{};
    const uint8_t flagsBytes[] = { 0x17, 0x23, 0x89, 0xAB };
    std::memcpy(&source.peepFlagsLL, flagsBytes, sizeof(flagsBytes));
    pass &= check(source.getPeepFlags(true) == 0xAB892317, "LL peep flags");
    pass &= check(source.getPeepFlags(false) == 0x2317 && source.GetItemFlags(true) == 0xAB89, "base/AA split flags");
    source.CurrentCar = 0x12;
    source.CurrentSeat = 0x34;
    Guest guest{};
    guest.currentCar = source.CurrentCar;
    guest.currentSeat = source.CurrentSeat;
    pass &= check(guest.timeToSitdown == 0x3412, "peep byte/word alias");

    RCT1::Vehicle vehicleSource{};
    vehicleSource.FerrisWheelVar0 = 0x12;
    vehicleSource.FerrisWheelVar1 = 0x34;
    Vehicle vehicle{};
    vehicle.SwingPosition = vehicleSource.SwingPosition;
    pass &= check(vehicle.ferris_wheel_var_0 == 0x12 && vehicle.ferris_wheel_var_1 == 0x34, "vehicle byte/word alias");
    RCT12ResearchItem research{};
    research.EntryIndex = 0x17;
    research.BaseRideType = 0x23;
    research.Type = 1;
    research.Flags = 0x42;
    pass &= check(research.RawValue == 0x42012317, "research byte/word alias");
    UtilRandSeed(0x1234567F);
    const auto random = UtilRand();
    UtilRandSeed(0x1234567F);
    pass &= check(random == UtilRand(), "utility RNG replay seed");
    std::printf("%s stored S4 offsets, all 32 park flag bits, base/AA/LL peep flags and shared aliases\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
