// Optional Linux test fixture. Build actual rides through the engine actions;
// the native application and normal simulation builds never contain this hook.
#include <nlohmann/json.hpp>
#include <openrct2/Context.h>
#include <openrct2/GameState.h>
#include <openrct2/actions/ride/RideCreateAction.h>
#include <openrct2/actions/ride/RideSetStatusAction.h>
#include <openrct2/actions/ride/RideSetSettingAction.h>
#include <openrct2/actions/terraform/ClearAction.h>
#include <openrct2/actions/track/TrackPlaceAction.h>
#include <openrct2/actions/scenery/SmallSceneryPlaceAction.h>
#include <openrct2/actions/scenery/LargeSceneryPlaceAction.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/object/ObjectRepository.h>
#include <openrct2/object/SmallSceneryObject.h>
#include <openrct2/object/LargeSceneryObject.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/RideManager.hpp>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/world/Banner.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
#include <openrct2/ride/Track.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/Park.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/interface/Window.h>

using namespace OpenRCT2;
using json = nlohmann::json;

static GameActions::Result Apply(GameActions::GameAction& action)
{
    auto& state = getGameState();
    auto result = action.Query(state, state.park);
    if (result.error == GameActions::Status::ok)
        result = action.Execute(state, state.park);
    if (result.error != GameActions::Status::ok)
        throw std::runtime_error(result.getErrorTitle() + ": " + result.getErrorMessage());
    return result;
}

json RideFpsFixture(const json& command)
{
    auto& state = getGameState();
    auto& context = *GetContext();
    auto& manager = context.GetObjectManager();
    auto& repository = context.GetObjectRepository();
    json result;
    if (command.contains("follow"))
    {
        const auto* ride = GetRide(RideId::FromUnderlying(command.at("follow").get<int>()));
        const auto* vehicle = ride ? state.entities.tryGetEntity<Vehicle>(ride->vehicles[0]) : nullptr;
        if (!vehicle) throw std::runtime_error("fixture train has no vehicle");
        WindowScrollToLocation(*WindowGetMain(), {vehicle->x, vehicle->y, vehicle->z});
        result["focus"] = {vehicle->x, vehicle->y, vehicle->z};
        return result;
    }
    if (command.value("list", false))
    {
        result["flats"] = json::array();
        result["scenery"] = json::array();
        for (ride_type_t type = 0; type < RIDE_TYPE_COUNT; ++type)
        {
            const auto& descriptor = GetRideTypeDescriptor(type);
            if (descriptor.flags.has(RtdFlag::isFlatRide) && !descriptor.flags.has(RtdFlag::isShopOrFacility))
                result["flats"].push_back({{"type", type}, {"name", descriptor.Name}});
        }
        const auto* items = repository.GetObjects();
        for (size_t i = 0; i < repository.GetNumObjects(); ++i)
        {
            const auto& item = items[i];
            if (item.Type != ObjectType::smallScenery && item.Type != ObjectType::largeScenery)
                continue;
            auto object = repository.LoadObject(&item, false);
            if (!object) continue;
            const bool small = item.Type == ObjectType::smallScenery;
            const auto* s = small ? static_cast<SmallSceneryEntry*>(object->GetLegacyData()) : nullptr;
            const auto* l = small ? nullptr : static_cast<LargeSceneryEntry*>(object->GetLegacyData());
            if ((small && s->flags.has(SmallSceneryFlag::isAnimated)) || (!small && l->flags.has(LargeSceneryFlag::isAnimated)))
                result["scenery"].push_back({{"object", item.Identifier}, {"name", item.Name},
                    {"large", !small}, {"height", small ? s->height : 0},
                    {"fountain", small && s->flags.hasAny(SmallSceneryFlag::isFountain, SmallSceneryFlag::isCupidFountain)}});
        }
        return result;
    }
    state.cheats.sandboxMode = true;
    state.cheats.disableClearanceChecks = true;
    state.cheats.disableSupportLimits = true;
    state.park.flags.set(ParkFlag::noMoney);
    const int x = command.value("x", 2400), y = command.value("y", 2400);
    const int z = command.value("z", TileElementHeight({x,y}) + 16);
    const Direction direction = command.value("direction", Direction{0});
    const int clearRadius = command.value("clear", 192);
    GameActions::ClearAction clear({x-clearRadius,y-clearRadius,x+clearRadius,y+clearRadius},
        {GameActions::ClearableItem::smallScenery, GameActions::ClearableItem::largeScenery, GameActions::ClearableItem::walls});
    Apply(clear);
    if (command.value("water", false))
        for (int ty = y-192; ty <= y+192; ty += 32)
            for (int tx = x-192; tx <= x+192; tx += 32)
                if (auto* surface = MapGetSurfaceElementAt(CoordsXY{tx,ty}))
                {
                    surface->setWaterHeight(z);
                    MapInvalidateTileFull({tx,ty});
                }
    if (command.contains("object"))
    {
        auto* object = manager.LoadObject(command.at("object").get<std::string>());
        if (!object) throw std::runtime_error("cannot load fixture scenery object");
        const auto index = manager.GetLoadedObjectEntryIndex(object);
        if (object->GetObjectType() == ObjectType::smallScenery)
        {
            GameActions::SmallSceneryPlaceAction place({x,y,z,direction}, 0, index,
                Drawing::Colour::brightRed, Drawing::Colour::yellow, Drawing::Colour::white);
            Apply(place);
        }
        else
        {
            GameActions::LargeSceneryPlaceAction place({x,y,z,direction}, index,
                Drawing::Colour::brightRed, Drawing::Colour::yellow, Drawing::Colour::white);
            const auto placed = Apply(place).getData<GameActions::LargeSceneryPlaceActionResult>();
            if (auto* banner = GetBanner(placed.bannerId))
            {
                banner->flags.unset(BannerFlag::linkedToRide);
                banner->text = "Ride frame rate test: scrolling animated scenery";
            }
        }
        result["focus"] = {x+16,y+16,z+40};
        WindowScrollToLocation(*WindowGetMain(), {x+16,y+16,z+40});
        return result;
    }
    const auto type = command.at("type").get<ride_type_t>();
    const auto& descriptor = GetRideTypeDescriptor(type);
    const auto* items = repository.GetObjects();
    Object* object = nullptr;
    for (size_t i = 0; i < repository.GetNumObjects(); ++i)
        if (items[i].Type == ObjectType::ride)
            for (const auto candidate : items[i].RideInfo.RideType)
                if (candidate == type && object == nullptr)
                    object = manager.LoadObject(items[i].Identifier);
    if (!object) throw std::runtime_error("no fixture ride object for type " + std::to_string(type));
    GameActions::RideCreateAction create(type, manager.GetLoadedObjectEntryIndex(object), 0, 0,
        0, RideInspection::every30Minutes);
    const auto id = Apply(create).getData<RideId>();
    GameActions::TrackPlaceAction track(id, descriptor.StartTrackPiece, type, {x,y,z,direction}, 0, 0, 0, {}, false);
    Apply(track);
    if (type == RIDE_TYPE_SPIRAL_SLIDE && command.value("active", false))
    {
        // Exercise the real slide update/paint state for one descent. A guest
        // entity is unnecessary: the update tolerates a null peep at completion.
        auto* ride = GetRide(id);
        ride->slideInUse = 1;
        ride->spiralSlideProgress = 0;
        ride->slidePeep = EntityId::GetNull();
        ride->slidePeepTShirtColour = Drawing::Colour::brightRed;
    }
    if (type == RIDE_TYPE_RIVER_RAPIDS)
    {
        GameActions::TrackPlaceAction waterfall(id, TrackElemType::waterfall, type, {x,y+64,z,direction}, 0, 0, 0, {}, false);
        Apply(waterfall);
    }
    else if (type == RIDE_TYPE_OBSERVATION_TOWER)
    {
        for (int h = 32; h <= 224; h += 32)
        {
            GameActions::TrackPlaceAction section(id, TrackElemType::towerSection, type, {x,y,z+h,direction}, 0, 0, 0, {}, false);
            Apply(section);
        }
    }
    if (!descriptor.flags.has(RtdFlag::noVehicles) && type != RIDE_TYPE_RIVER_RAPIDS)
    {
        GameActions::RideSetSettingAction mode(id, GameActions::RideSetSetting::mode, EnumValue(descriptor.DefaultMode));
        Apply(mode);
        GameActions::RideSetStatusAction start(id, RideStatus::simulating);
        Apply(start);
    }
    result["id"] = id.ToUnderlying();
    result["name"] = descriptor.Name;
    result["focus"] = {x,y,z + (type == RIDE_TYPE_OBSERVATION_TOWER ? 160 : 56)};
    WindowScrollToLocation(*WindowGetMain(), {x,y,z + (type == RIDE_TYPE_OBSERVATION_TOWER ? 160 : 56)});
    return result;
}
