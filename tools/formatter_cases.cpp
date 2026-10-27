// Real legacy decoder fixtures; compile and run on both host byte orders.
#include <openrct2-ui/UiStringIds.h>
#include <openrct2-ui/interface/Objective.h>
#include <openrct2/Context.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/SpriteIds.h>
#include <openrct2/TrackImporter.h>
#include <openrct2/core/MemoryStream.h>
#include <openrct2/rct1/RCT1.h>
#include <openrct2/rct2/RCT2.h>
#include <openrct2/ride/ted/TrackElemType.h>
#include <openrct2/sawyer_coding/SawyerChunkWriter.h>
#include <openrct2/config/Config.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/entity/Staff.h>
#include <openrct2/interface/Widget.h>
#include <openrct2/localisation/CurrencyTypes.h>
#include <openrct2/localisation/Formatter.h>
#include <openrct2/localisation/Formatting.h>
#include <openrct2/localisation/StringIds.h>
#include <openrct2/peep/PeepActionFormat.h>
#include <openrct2/peep/PeepThoughts.h>
#include <openrct2/management/Marketing.h>
#include <openrct2/management/Research.h>
#include <openrct2/ride/Ride.h>
#include <openrct2/scenario/ScenarioObjective.h>
#include <openrct2/windows/Intent.h>
#include <openrct2/world/Banner.h>
#include "ClassicVfs.h"
#include <cassert>
#include <cstdio>
#include <memory>
#include <string_view>

using namespace OpenRCT2;
static std::unique_ptr<TrackDesign> ImportBytes(const std::vector<uint8_t>& bytes, bool td4 = false)
{
    MemoryStream stream;
    SawyerCoding::SawyerChunkWriter writer(&stream);
    writer.WriteChunkTrack(bytes.data(), bytes.size());
    stream.SetPosition(0);
    auto importer = td4 ? TrackImporter::CreateTD4() : TrackImporter::CreateTD6();
    assert(importer->LoadFromStream(&stream));
    auto result = importer->Import();
    assert(result);
    return result;
}
static void Show(const char* site, StringId id, const Formatter& ft)
{
    std::printf("CASE\t%s\t%u\t%s\n", site, id, FormatStringIDLegacy(id, ft.Data()).c_str());
}
#include "BuildIdentity.h"

int main(int argc, char** argv)
{
    if (!RctBuild::Start())
        return 125;
    ClassicVfs::Initialise();
    gOpenRCT2Headless = true;
    gOpenRCT2NoGraphics = false;
    std::unique_ptr<IContext> context(CreateContext());
    if (!context->Initialise()) return 1;
    Config::Get().general.currencyFormat = CurrencyType::dollars;
    Config::Get().general.showRealNamesOfGuests = false;
    Config::Get().general.showRealNamesOfStaff = false;
    if (argc > 1 && std::string_view(argv[1]) == "--old-track-layout")
    {
        const uint8_t entry[16] = {3,0,0,0x10,'T','E','S','T',' ',' ',' ',' ',0x78,0x56,0x34,0x12};
        RCTObjectEntry oldEntry{};
        std::memcpy(&oldEntry, entry, sizeof(entry));
        const uint8_t typeBytes[] = {4,1}, mazeBytes[] = {0x34,0x12};
        uint16_t oldType{}, oldMaze{};
        std::memcpy(&oldType, typeBytes, sizeof(oldType));
        std::memcpy(&oldMaze, mazeBytes, sizeof(oldMaze));
        std::printf("CASE\tT6Importer BEFORE raw object\tflags=%08x type=%u checksum=%08x\n",
            oldEntry.flags, EnumValue(oldEntry.GetType()), oldEntry.checksum);
        std::printf("CASE\tTD7 BEFORE raw type\t%u\nCASE\tTD4/TD6 BEFORE raw maze mask\t%u\n", oldType, oldMaze);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--old-singular")
    {
        for (auto id : {STR_PARK_INFO_PANEL_NUM_GUESTS_STABLE_SINGULAR,
                 STR_PARK_INFO_PANEL_NUM_GUESTS_DECREASE_SINGULAR, STR_PARK_INFO_PANEL_NUM_GUESTS_INCREASE_SINGULAR})
        {
            Formatter before;
            before.Add<uint32_t>(1);
            Show("ParkInfoPanel.cpp:112 BEFORE", id, before);
        }
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--old-status")
    {
        Ride ride{};
        ride.customName = "Test ride 23";
        Formatter before;
        before.Increment(6);
        ride.formatNameTo(before);
        Show("RideSetStatusAction.cpp:135 BEFORE", STR_CANT_OPEN, before);
        return 0;
    }
#ifdef OPENRCT2_FORMATTER_AUDIT
    if (argc > 1 && std::string_view(argv[1]) == "--negative")
    {
        Formatter bad;
        bad.Add<uint16_t>(23);
        Show("negative queue COMMA32", STR_QUEUE_PEOPLE, bad);
        return 0;
    }
#endif
    for (bool tracked : {false, true})
    {
        Guest guest{};
        guest.peepId = 23;
        guest.happiness = 128;
        guest.state = PeepState::walking;
        guest.action = PeepActionType::walking;
        Formatter ft;
        ft.Add<StringId>(tracked ? STR_TRACKED_GUEST_MAP_TIP : STR_GUEST_MAP_TIP);
        ft.Add<uint32_t>(GetPeepFaceSpriteSmall(&guest));
        guest.formatNameTo(ft);
        formatPeepActionTo(guest, ft);
        Show("Peep.cpp:1441", STR_MAP_TOOLTIP_STRINGID, ft);
    }
    for (const char* name : {"", "Custom guest"})
    {
        Guest guest{};
        guest.peepId = 12345;
        guest.name = *name ? const_cast<char*>(name) : nullptr;
        Formatter ft;
        guest.formatNameTo(ft);
        Show("Peep.cpp:formatNameTo", STR_STRINGID, ft);
    }
    for (unsigned type = 0; type < 174; ++type)
    {
        if (kPeepThoughtIds[type] == kStringIdNone) continue;
        PeepThought thought{};
        thought.type = static_cast<PeepThoughtType>(type);
        thought.shopItem = ShopItem::burger;
        Formatter ft;
        PeepThoughtSetFormatArgs(&thought, ft);
        Show("Guest.cpp:PeepThoughtSetFormatArgs", STR_STRINGID, ft);
    }
    for (auto status : {RideStatus::closed, RideStatus::open, RideStatus::testing, RideStatus::simulating})
        for (uint16_t riders : {uint16_t{0}, uint16_t{1}, uint16_t{23}})
        {
            Ride ride{};
            ride.type = RIDE_TYPE_MERRY_GO_ROUND;
            ride.status = status;
            ride.numRiders = riders;
            Formatter ft;
            ride.formatStatusTo(ft);
            Show("Ride.cpp:formatStatusTo", STR_STRINGID, ft);
        }
    for (int mode = 0; mode != 3; ++mode)
    {
        Banner banner{};
        if (mode == 1) banner.flags.set(BannerFlag::noEntry);
        if (mode == 2) banner.text = "Test sign 123";
        Formatter ft;
        banner.formatTextWithColourTo(ft);
        Show("Banner.cpp:55", STR_STRINGID, ft);
    }
    for (bool enlarged : {false, true})
        for (bool selected : {false, true})
            for (const auto& weather : std::to_array<std::array<uint32_t, 3>>({
                {SPR_G2_WEATHER_SUN_SMALL, SPR_WEATHER_SUN, STR_SUNNY},
                {SPR_G2_WEATHER_SUN_CLOUD_SMALL, SPR_WEATHER_SUN_CLOUD, STR_PARTIALLY_CLOUDY},
                {SPR_G2_WEATHER_CLOUD_SMALL, SPR_WEATHER_CLOUD, STR_CLOUDY},
                {SPR_G2_WEATHER_LIGHT_RAIN_SMALL, SPR_WEATHER_LIGHT_RAIN, STR_RAIN},
                {SPR_G2_WEATHER_HEAVY_RAIN_SMALL, SPR_WEATHER_HEAVY_RAIN, STR_HEAVY_RAIN},
                {SPR_G2_WEATHER_STORM_SMALL, SPR_WEATHER_STORM, STR_THUNDERSTORM},
                {SPR_G2_WEATHER_SNOW_SMALL, SPR_G2_WEATHER_SNOW, STR_SNOW},
                {SPR_G2_WEATHER_HEAVY_SNOW_SMALL, SPR_G2_WEATHER_HEAVY_SNOW, STR_HEAVY_SNOW},
                {SPR_G2_WEATHER_BLIZZARD_SMALL, SPR_G2_WEATHER_BLIZZARD, STR_BLIZZARD}}))
            {
                Formatter ft;
                if (!enlarged) ft.Add<StringId>(STR_WEATHER_DROPDOWN_ITEM);
                ft.Add<uint32_t>(weather[enlarged ? 1 : 0]);
                ft.Add<StringId>(weather[2]);
                const auto id = enlarged ? (selected ? STR_WEATHER_DROPDOWN_ITEM_ENLARGED_SELECTED : STR_WEATHER_DROPDOWN_ITEM_ENLARGED)
                    : (selected ? STR_DROPDOWN_MENU_LABEL_SELECTED : STR_DROPDOWN_MENU_LABEL);
                Show("Cheats.cpp:1002", id, ft);
            }
    for (int type = 0; type != 12; ++type)
    {
        Scenario::Objective objective{};
        objective.Type = static_cast<Scenario::ObjectiveType>(type);
        objective.NumGuests = 1234;
        objective.Year = 3;
        objective.Currency = 123450;
        if (objective.Type == Scenario::ObjectiveType::tenRollercoastersLength) objective.MinimumLength = 1234;
        if (objective.Type == Scenario::ObjectiveType::finishFiveRollercoasters) objective.MinimumExcitement = 670;
        Formatter ft;
        ft.Add<StringId>(Ui::kObjectiveNames[type]);
        Ui::formatObjective(ft, objective);
        Show("Objective.cpp:formatObjective", STR_OBJECTIVE, ft);
        ft = Formatter();
        StringId id;
        switch (objective.Type)
        {
            case Scenario::ObjectiveType::guestsBy:
            case Scenario::ObjectiveType::guestsAndRating:
                id = STR_WINDOW_COLOUR_2_COMMA32; ft.Add<int32_t>(objective.NumGuests); break;
            case Scenario::ObjectiveType::parkValueBy:
            case Scenario::ObjectiveType::repayLoanAndParkValue:
            case Scenario::ObjectiveType::monthlyRideIncome:
            case Scenario::ObjectiveType::monthlyFoodIncome:
                id = STR_CURRENCY_FORMAT_LABEL; ft.Add<money64>(objective.Currency); break;
            case Scenario::ObjectiveType::tenRollercoastersLength:
                id = STR_WINDOW_COLOUR_2_LENGTH; ft.Add<uint16_t>(objective.MinimumLength); break;
            case Scenario::ObjectiveType::finishFiveRollercoasters:
                id = STR_WINDOW_COLOUR_2_COMMA2DP32; ft.Add<uint32_t>(static_cast<uint16_t>(objective.MinimumExcitement)); break;
            default:
                id = STR_WINDOW_COLOUR_2_COMMA2DP32; ft.Add<int32_t>(objective.Currency); break;
        }
        Show("EditorScenarioOptions.cpp:1130", id, ft);
    }
    for (money64 cash : {money64{12345}, money64{-12345}, money64{1234567890123}})
    {
        Formatter ft;
        ft.Add<money64>(cash);
        Show("Finances.cpp:624", cash >= 0 ? STR_CASH_LABEL : STR_CASH_NEGATIVE_LABEL, ft);
    }
    for (auto id : {STR_PARK_INFO_PANEL_NUM_GUESTS_STABLE_SINGULAR,
             STR_PARK_INFO_PANEL_NUM_GUESTS_DECREASE_SINGULAR, STR_PARK_INFO_PANEL_NUM_GUESTS_INCREASE_SINGULAR})
    {
        Formatter after;
        after.Add<uint16_t>(1);
        Show("ParkInfoPanel.cpp:112 AFTER", id, after);
    }
    for (StringId unit : {STR_SIZE_BYTE, STR_SIZE_KILOBYTE, STR_SIZE_MEGABYTE})
    {
        Formatter ft;
        ft.Add<StringId>(STR_FILEBROWSER_FILE_SIZE_VALUE);
        ft.Add<uint32_t>(12345);
        ft.Add<StringId>(unit);
        Show("LoadSave.cpp:1122", STR_WINDOW_COLOUR_2_STRINGID, ft);
    }
    {
        Ride ride{};
        ride.customName = "Test ride 23";
        Formatter after;
        ride.formatNameTo(after);
        Show("RideSetStatusAction.cpp:135 AFTER", STR_CANT_OPEN, after);
    }
    Widget widget{};
    widget.content = STR_CASH_LABEL;
    assert(widget.text == STR_CASH_LABEL);
    widget.text = STR_CASH_NEGATIVE_LABEL;
    assert((widget.content & 0xffff) == STR_CASH_NEGATIVE_LABEL);
    Intent intent(WindowClass::ride);
    intent.PutExtra(INTENT_EXTRA_RIDE_ID, int32_t{23});
    assert(intent.GetSIntExtra(INTENT_EXTRA_RIDE_ID) == 23);
    std::printf("PROOF\tWidget low-half alias and Intent integer variant agree\n");
    Scenario::Objective objective{};
    objective.Currency = 670;
    assert(objective.MinimumExcitement == 670);
    std::printf("PROOF\tObjective Currency low-half reads MinimumExcitement 670\n");
    MarketingCampaign campaign{};
    campaign.rideId = RideId::FromUnderlying(23);
    assert(static_cast<unsigned>(campaign.shopItemType) == 23);
    campaign.shopItemType = ShopItem::burger;
    assert(campaign.rideId.ToUnderlying() == static_cast<unsigned>(ShopItem::burger));
    const ResearchItem research{0x01020017, ResearchCategory::gentle, 0};
    assert(EnumValue(research.type) == 1 && research.baseRideType == 2 && research.entryIndex == 23);
    std::printf("PROOF\tMarketing union low-byte and Research packed type/base/entry agree\n");
    // Same raw-byte protocol as PaintFloatingMoneyEffect; no painter code changes.
    for (const auto amount : {money64{12345}, money64{1234567890123}})
    {
        uint32_t raw[4]{};
        std::memcpy(raw, &amount, sizeof(amount));
        for (const auto id : {STR_MONEY_EFFECT_SPEND, STR_MONEY_EFFECT_RECEIVE_HIGHP})
        {
            Formatter typed;
            typed.Add<money64>(amount);
            const auto text = FormatStringIDLegacy(id, raw);
            assert(text == FormatStringIDLegacy(id, typed.Data()));
            std::printf("PROOF\tPaint.cpp:1894 raw currency8\t%u\t%s\n", id, text.c_str());
        }
    }
    {
        Formatter date;
        date.Add<uint16_t>(23);
        Show("Formatting.cpp:FormatMonthYear", static_cast<StringId>(1845), date);
    }
    {
        // Stored bytes, not a native struct: catches missing conversion at the raw importer boundary.
        std::vector<uint8_t> bytes(0xa3, 0);
        bytes[0] = RIDE_TYPE_WOODEN_ROLLER_COASTER;
        bytes[7] = 3 << 2;
        const auto vehicle = offsetof(RCT2::TD6Track, VehicleObject);
        const uint8_t entry[16] = {0, 0, 0, 0x10, 'T','E','S','T',' ',' ',' ',' ', 0x78,0x56,0x34,0x12};
        std::memcpy(bytes.data() + vehicle, entry, sizeof(entry));
        bytes.insert(bytes.end(), {4, 1, 0, 0xff, 0xff, 0xff}); // TD7 type 260; track and entrance terminators.
        auto scenery = std::vector<uint8_t>(entry, entry + 16);
        scenery[0] = EnumValue(ObjectType::walls);
        scenery.resize(sizeof(RCT2::TD6SceneryElement), 0);
        bytes.insert(bytes.end(), scenery.begin(), scenery.end());
        bytes.push_back(0xff);
        const auto design = ImportBytes(bytes);
        assert(design->trackElements.at(0).type == TrackElemType::flatTrack2x4);
        assert(design->trackAndVehicle.vehicleObject.Entry.flags == 0x10000000);
        assert(design->trackAndVehicle.vehicleObject.Entry.checksum == 0x12345678);
        assert(design->sceneryElements.at(0).sceneryObject.Entry.GetType() == ObjectType::walls);
        assert(design->sceneryElements.at(0).sceneryObject.Entry.checksum == 0x12345678);
        std::printf("PROOF\tT6Importer stored vehicle/scenery flags+checksum and TD7 type 260 agree\n");
        bytes.resize(0xa3);
        bytes[0] = RIDE_TYPE_MAZE;
        bytes[7] = 2 << 2;
        bytes.insert(bytes.end(), {1,2,0x34,0x12, 0,0,0,0, 0xff});
        const auto maze = ImportBytes(bytes);
        assert(maze->mazeElements.at(0).mazeEntry == 0x1234);
        std::printf("PROOF\tT6Importer stored maze bitmask 4660 agrees\n");
        std::vector<uint8_t> td4(sizeof(RCT1::TD4), 0);
        td4[0] = EnumValue(RCT1::RideType::hedgeMaze);
        td4.insert(td4.end(), {1,2,0x34,0x12, 0,0,0,0});
        const auto maze4 = ImportBytes(td4, true);
        assert(maze4->mazeElements.at(0).mazeEntry == 0x1234);
        std::printf("PROOF\tT4Importer stored maze bitmask 4660 agrees\n");
    }
}
