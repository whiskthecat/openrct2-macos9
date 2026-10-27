// Linux-only rendering fixture. No saved parks or graphics are part of this test.
#pragma once
#include <openrct2-ui/windows/Windows.h>
#include <openrct2-ui/interface/LandTool.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/scenery/SmallSceneryPlaceAction.h>
#include <openrct2/actions/scenery/SmallSceneryRemoveAction.h>
#include <openrct2/actions/scenery/LargeSceneryPlaceAction.h>
#include <openrct2/actions/scenery/LargeSceneryRemoveAction.h>
#include <openrct2/actions/scenery/WallPlaceAction.h>
#include <openrct2/actions/scenery/WallRemoveAction.h>
#include <openrct2/actions/scenery/BannerPlaceAction.h>
#include <openrct2/actions/scenery/BannerRemoveAction.h>
#include <openrct2/actions/footpath/FootpathPlaceAction.h>
#include <openrct2/actions/footpath/FootpathRemoveAction.h>
#include <openrct2/actions/ride/RideCreateAction.h>
#include <openrct2/actions/ride/RideDemolishAction.h>
#include <openrct2/actions/ride/RideEntranceExitPlaceAction.h>
#include <openrct2/actions/ride/RideEntranceExitRemoveAction.h>
#include <openrct2/actions/track/TrackPlaceAction.h>
#include <openrct2/actions/track/TrackRemoveAction.h>
#include <openrct2/actions/track/TrackDesignAction.h>
#include <openrct2/object/LargeSceneryObject.h>
#include <openrct2/ride/ted/TrackElementDescriptor.h>
#include <openrct2/ride/TrackData.h>
#include <openrct2/rct2/T6Exporter.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/entity/EntityList.h>
#include <chrono>
#include <openrct2/actions/terraform/LandSetHeightAction.h>
#include <openrct2/actions/terraform/LandRaiseAction.h>
#include <openrct2/actions/terraform/LandLowerAction.h>
#include <openrct2/actions/terraform/SurfaceSetStyleAction.h>
#include <openrct2/actions/terraform/WaterSetHeightAction.h>
#include <openrct2/actions/park/LandSetRightsAction.h>
#include <openrct2/actions/terraform/ClearAction.h>
#include <openrct2/object/RideObject.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/Track.h>
#include <openrct2/world/MapSelection.h>
#include <openrct2/world/Scenery.h>
#include <openrct2/world/TileElementsView.h>
#include <openrct2/world/tile_element/LargeSceneryElement.h>
#include <openrct2/world/tile_element/PathElement.h>
#include <openrct2/world/tile_element/SmallSceneryElement.h>
#include <openrct2/world/tile_element/BannerElement.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <openrct2/paint/VirtualFloor.h>
#include <openrct2/management/Research.h>

namespace PlacementFixture
{
    using namespace OpenRCT2;
    using namespace OpenRCT2::GameActions;
    using json = nlohmann::json;
    inline RideId trackRide = RideId::GetNull(), shopRide = RideId::GetNull();
    inline ride_type_t trackType = kRideTypeNull, shopType = kRideTypeNull;
    inline CoordsXYZD origin{ 2208, 1856, 192, 0 };
    inline TrackDesign preview;
    inline RideId previewRide = RideId::GetNull();
    inline CoordsXYZD previewPosition;
    inline ObjectEntryIndex largeIndex = kObjectEntryIndexNull;
    inline int32_t fixtureRadius = 256;

    inline ObjectEntryIndex largestScenery()
    {
        size_t count = 0;
        auto& manager = GetContext()->GetObjectManager();
        for (ObjectEntryIndex i = 0; i < 128; i++)
            if (auto* loaded = manager.GetLoadedObject<LargeSceneryObject>(i))
            {
                auto* entry = static_cast<LargeSceneryEntry*>(loaded->GetLegacyData());
                if (entry->tiles.size() > count)
                {
                    count = entry->tiles.size();
                    largeIndex = i;
                }
            }
        if (largeIndex == kObjectEntryIndexNull)
            throw std::runtime_error("fixture large scenery unavailable");
        return largeIndex;
    }

    inline TrackDesign design(const std::string& kind)
    {
        auto& manager = GetContext()->GetObjectManager();
        ObjectEntryIndex best = kObjectEntryIndexNull;
        size_t footprint = 0;
        for (ObjectEntryIndex i = 0; i < 128; i++)
            if (const auto* loaded = manager.GetLoadedObject<RideObject>(i))
            {
                const auto type = loaded->GetEntry().GetFirstNonNullRideType();
                const auto& rtd = GetRideTypeDescriptor(type);
                const bool match = kind == "maze" ? rtd.specialType == RtdSpecialType::maze
                    : kind == "shopgroup" ? rtd.flags.has(RtdFlag::isShopOrFacility)
                    : rtd.RatingsData.Type == RatingsCalculationType::flatRide;
                const size_t tiles = TrackMetadata::GetTrackElementDescriptor(rtd.StartTrackPiece).sequenceData.numSequences;
                if (match && (best == kObjectEntryIndexNull || tiles > footprint))
                {
                    best = i;
                    footprint = tiles;
                }
            }
        if (best == kObjectEntryIndexNull)
            throw std::runtime_error("fixture preview ride unavailable: " + kind);
        const auto* loaded = manager.GetLoadedObject<RideObject>(best);
        TrackDesign td{};
        td.trackAndVehicle.rtdIndex = loaded->GetEntry().GetFirstNonNullRideType();
        td.trackAndVehicle.vehicleObject = loaded->GetDescriptor();
        td.operation.rideMode = GetRideTypeDescriptor(td.trackAndVehicle.rtdIndex).DefaultMode;
        td.operation.numCircuits = 1;
        td.gameStateData.name = "Ghost regression " + kind;
        td.trackAndVehicle.numberOfTrains = td.trackAndVehicle.numberOfCarsPerTrain = 1;
        if (kind == "maze")
        {
            td.appearance.trackColours[0].supports = static_cast<Drawing::Colour>(1); // Hedge maze style.
            for (int y = 0; y < 6; y++)
                for (int x = 0; x < 6; x++)
                    td.mazeElements.push_back({{x, y}, 0x7777});
        }
        else
        {
            TrackDesignTrackElement piece{};
            piece.type = GetRideTypeDescriptor(td.trackAndVehicle.rtdIndex).StartTrackPiece;
            td.trackElements.push_back(piece);
        }
        if (kind == "shopgroup")
        {
            TrackDesignSceneryElement scenery{};
            scenery.sceneryObject = manager.GetLoadedObject<LargeSceneryObject>(largestScenery())->GetDescriptor();
            scenery.loc = {64, 64, 0};
            td.sceneryElements.push_back(scenery);
        }
        return td;
    }

    inline Result apply(GameAction& action, bool ghost = false)
    {
        CommandFlags flags{ CommandFlag::allowDuringPaused, CommandFlag::noSpend };
        flags.set(CommandFlag::ghost, ghost);
        action.SetFlags(flags);
        auto result = GameActions::Execute(&action, getGameState());
        if (result.error != Status::ok)
            throw std::runtime_error(std::string(action.GetName()) + ": " + result.getErrorMessage());
        return result;
    }

    inline ObjectEntryIndex object(ObjectType type)
    {
        auto& manager = GetContext()->GetObjectManager();
        for (ObjectEntryIndex i = 0; i < 256; i++)
            if (manager.GetLoadedObject(type, i) != nullptr)
                return i;
        throw std::runtime_error("fixture object unavailable: " + std::to_string(static_cast<int>(type)));
    }

    inline void createRide(bool shop)
    {
        auto& id = shop ? shopRide : trackRide;
        auto& type = shop ? shopType : trackType;
        if (GetRide(id) != nullptr)
            return;
        auto& manager = GetContext()->GetObjectManager();
        for (ObjectEntryIndex i = 0; i < 128; i++)
        {
            const auto* loaded = manager.GetLoadedObject<RideObject>(i);
            if (loaded == nullptr)
                continue;
            type = loaded->GetEntry().GetFirstNonNullRideType();
            const auto& descriptor = GetRideTypeDescriptor(type);
            if (descriptor.flags.has(RtdFlag::isShopOrFacility) != shop
                || (!shop && (!descriptor.flags.has(RtdFlag::hasTrack)
                    || !descriptor.flags.has(RtdFlag::hasEntranceAndExit))))
                continue;
            RideCreateAction action(type, i, 0, 0, object(ObjectType::station), RideInspection::every30Minutes);
            id = apply(action).getData<RideId>();
            return;
        }
        throw std::runtime_error("fixture ride unavailable");
    }

    inline TileElement* find(TileElementType type, bool ghost, CoordsXY& loc)
    {
        for (int y = origin.y - fixtureRadius; y <= origin.y + fixtureRadius; y += 32)
            for (int x = origin.x - fixtureRadius; x <= origin.x + fixtureRadius; x += 32)
                for (auto* element : TileElementsView<>(CoordsXY{x, y}))
                    if (element->getType() == type && element->isGhost() == ghost)
                    {
                        loc = {x, y};
                        return element;
                    }
        throw std::runtime_error("fixture placed element missing");
    }

    inline json inventory()
    {
        json counts = json::object();
        for (int y = origin.y - fixtureRadius; y <= origin.y + fixtureRadius; y += 32)
            for (int x = origin.x - fixtureRadius; x <= origin.x + fixtureRadius; x += 32)
                for (auto* element : TileElementsView<>(CoordsXY{x, y}))
                {
                    const auto key = std::to_string(static_cast<int>(element->getType()))
                        + (element->isGhost() ? "ghost" : "real");
                    counts[key] = counts.value(key, 0) + 1;
                }
        return counts;
    }

    inline void path(const CoordsXYZ& loc, bool ghost, bool queue, bool sloped = false)
    {
        FootpathSlope slope{sloped ? FootpathSlopeType::sloped : FootpathSlopeType::flat, 0};
        FootpathPlaceAction action(loc, slope, object(ObjectType::footpathSurface), object(ObjectType::footpathRailings),
                                  kInvalidDirection, queue ? PathConstructFlag::IsQueue : 0);
        apply(action, ghost);
    }

    inline json run(const json& command)
    {
        if (std::getenv("RCT_PLACEMENT_TEST") == nullptr || std::getenv("RCT_FIXED_STEP") == nullptr)
            throw std::runtime_error("placement fixture requires explicit fixed-step test mode");
        const auto operation = command.at("stage").get<std::string>();
        const auto kind = command.value("kind", "small");
        const bool ghost = command.value("ghost", false);
        const auto before = inventory();
        const auto started = std::chrono::steady_clock::now();
        auto& state = getGameState();
        if (operation == "prepare")
        {
            if (auto* ride = GetRide(previewRide))
                TrackDesignPreviewRemoveGhosts(preview, *ride, previewPosition);
            previewRide = RideId::GetNull();
            largeIndex = kObjectEntryIndexNull;
            if (auto* ride = GetRide(trackRide)) ride->remove();
            if (auto* ride = GetRide(shopRide)) ride->remove();
            trackRide = shopRide = RideId::GetNull();
            origin.z = command.value("z", 192);
            origin.x = command.value("x", 2208);
            origin.y = command.value("y", 1856);
            origin.direction = command.value("direction", 0);
            fixtureRadius = std::clamp(command.value("radius", 256), 32, 1024);
            state.cheats.sandboxMode = true;
            state.cheats.disableClearanceChecks = true;
            state.cheats.disableSupportLimits = true;
            state.cheats.buildInPauseMode = true;
            state.cheats.ignoreResearchStatus = true;
            state.cheats.allowRegularPathAsQueue = true;
            state.cheats.allowTrackPlaceInvalidHeights = true;
            state.cheats.makeAllDestructible = true;
            // Keep unrelated HUD forecast transitions out of placement pictures.
            state.cheats.freezeWeather = true;
            gMapSelectFlags = {};
            VirtualFloorDisable();
            if (command.value("quiet_entities", false))
            {
                // Isolate exact terrain comparisons from unrelated crowd
                // sorting. Keep entity IDs/ride references intact; these
                // fixtures remain paused and never export this park.
                for (uint16_t i = 0; i < kMaxEntities; i++)
                    if (auto* entity = state.entities.getEntity(EntityId::FromUnderlying(i)); entity->type != EntityType::null)
                        entity->moveToAndUpdateSpatialIndex({64, 64, 128});
            }
            for (int y = origin.y - fixtureRadius; y <= origin.y + fixtureRadius; y += 32)
                for (int x = origin.x - fixtureRadius; x <= origin.x + fixtureRadius; x += 32)
                {
                    const CoordsXY tile{x, y};
                    for (;;)
                    {
                        TileElement* remove = nullptr;
                        for (auto* element : TileElementsView<>(tile))
                            if (element->getType() != TileElementType::surface)
                            {
                                remove = element;
                                break;
                            }
                        if (remove == nullptr)
                            break;
                        TileElementRemove(remove);
                    }
                    auto* surface = MapGetSurfaceElementAt(tile);
                    if (surface == nullptr)
                        continue;
                    surface->setBaseZ(command.value("ground", 128));
                    surface->setClearanceZ(command.value("ground", 128));
                    surface->setSlope(command.value("slope", 0));
                    surface->setWaterHeight(command.value("water", 0));
                    surface->setOwnership({OwnershipFlag::landOwned});
                    surface->setParkFences(0);
                    MapInvalidateTileFull(tile);
                }
            if (kind == "banner")
            {
                path({origin.x, origin.y, origin.z}, false, false);
                path({origin.x - 32, origin.y, origin.z}, false, false);
            }
            if (kind == "entrance" || kind == "exit")
            {
                createRide(false);
                TrackPlaceAction action(trackRide, TrackElemType::endStation, trackType, origin, 0, 0, 0, {}, false);
                apply(action);
            }
            Drawing::GfxInvalidateScreen(); // Fixture setup is outside the actions under test.
        }
        else if (operation == "view")
        {
            auto* window = WindowGetMain();
            window->viewport->flags.set(ViewportFlag::undergroundInside, command.value("underground", false));
            if (command.contains("floor_style"))
                Config::Get().general.virtualFloorStyle = static_cast<VirtualFloorStyles>(command.at("floor_style").get<int>());
            Drawing::GfxInvalidateScreen();
        }
        else if (operation == "modifiers")
        {
            // Exercise the real held-key state used by InputManager. The
            // host-only fixture must not rely on a floor that input dismisses.
            ShimHost::SetKey(SDL_SCANCODE_LCTRL, command.value("ctrl", false));
            ShimHost::SetKey(SDL_SCANCODE_LSHIFT, command.value("shift", false));
        }
        else if (operation == "floorheight")
        {
            // Exercise the normal height-change invalidation, without forcing
            // a redraw of either the old plane or the surrounding terrain.
            if (command.contains("dx"))
            {
                const CoordsXY tile = origin + CoordsXY{command.at("dx").get<int>(), 0};
                gMapSelectFlags.unset(MapSelectFlag::enable);
                gMapSelectFlags.set(MapSelectFlag::enableConstruct);
                // An L shape tests the union of floor areas, rather than an
                // assumption that every construction selection is rectangular.
                MapSelection::setSelectedTiles({tile, tile + CoordsXY{32, 0}, tile + CoordsXY{0, 32}});
            }
            VirtualFloorSetHeight(command.at("height").get<int16_t>());
            VirtualFloorInvalidate(false);
        }
        else if (operation == "window")
        {
            auto* manager = Ui::GetWindowManager();
            if (command.value("on", true))
            {
                auto* window = manager->OpenWindow(WindowClass::options);
                Ui::Windows::WindowSetPosition(*window, {480, 120});
            }
            else manager->CloseByClass(WindowClass::options);
        }
        else if (operation == "export")
        {
            const auto trackDesign = design(kind);
            RCT2::T6Exporter exporter(trackDesign);
            if (!exporter.SaveTrack(command.at("path").get<std::string>().c_str()))
                throw std::runtime_error("fixture design export failed");
        }
        else if (operation == "uitool")
        {
            SetAllSceneryItemsInvented();
            Ui::Windows::ToggleSceneryWindow();
            const ScenerySelection selection{ SCENERY_TYPE_LARGE, largestScenery() };
            const auto colour = Drawing::Colour::brightRed;
            Ui::Windows::WindowScenerySetSelectedItem(selection, colour, colour, colour, 0);
            if (Ui::Windows::WindowSceneryGetTabSelection() != selection)
                throw std::runtime_error("fixture could not select large scenery tool");
        }
        else if (operation == "inventory")
        {
            // Read-only verification after a measured real GUI pointer move.
        }
        else if (operation == "position")
        {
            origin.x = command.value("x", origin.x);
            origin.y = command.value("y", origin.y);
            origin.z = command.value("z", origin.z);
            origin.direction = command.value("direction", origin.direction);
        }
        else if (operation == "landtool")
        {
            Ui::Windows::ToggleLandWindow();
            gLandToolSize = std::clamp(command.value("size", 1), 1, 64);
        }
        else if (operation == "guest")
        {
            auto* guest = *EntityList<Guest>().begin();
            if (guest == nullptr)
                throw std::runtime_error("fixture requires an existing guest");
            // Exercise the real moving-entity damage/overlay path across the
            // placement tile. Normal simulation continues in live cases.
            guest->moveToAndUpdateSpatialIndex({origin.x + 8 + command.value("step", 0) * 4, origin.y + 16, origin.z});
        }
        else if (operation == "preview")
        {
            if (kind == "large")
            {
                if (largeIndex != kObjectEntryIndexNull)
                {
                    LargeSceneryRemoveAction remove(previewPosition, 0);
                    apply(remove, true);
                }
                previewPosition = origin;
                previewPosition.x += command.value("dx", 0);
                LargeSceneryPlaceAction action(previewPosition, largestScenery(), Drawing::Colour::brightRed,
                                              Drawing::Colour::brightRed, Drawing::Colour::brightRed);
                apply(action, true);
            }
            else
            {
                if (auto* ride = GetRide(previewRide))
                    TrackDesignPreviewRemoveGhosts(preview, *ride, previewPosition);
                preview = design(kind);
                previewPosition = origin;
                previewPosition.x += command.value("dx", 0);
                TrackDesignAction action(previewPosition, preview, true, RideInspection::every30Minutes);
                previewRide = apply(action, true).getData<RideId>();
                TrackDesignState selection{};
                TrackDesignPreviewDrawOutlines(selection, preview, RideGetTemporaryForPreview(), previewPosition, true);
            }
            gMapSelectPositionA = gMapSelectPositionB = previewPosition;
            VirtualFloorEnable();
            VirtualFloorSetHeight(origin.z);
            VirtualFloorInvalidate(true);
        }
        else if (operation == "selection")
        {
            MapInvalidateTileFull(origin);
            gMapSelectFlags = {};
            if (command.value("on", true))
            {
                gMapSelectFlags.set(MapSelectFlag::enable);
                gMapSelectType = MapSelectType::fullTerrainAndWater;
                gMapSelectPositionA = gMapSelectPositionB = origin;
                if (command.value("arrow", false))
                {
                    gMapSelectFlags.set(MapSelectFlag::enableArrow);
                    gMapSelectArrowPosition = origin;
                    gMapSelectArrowDirection = command.value("direction", origin.direction);
                }
            }
            else VirtualFloorDisable();
            if (command.value("floor", false))
            {
                VirtualFloorEnable();
                VirtualFloorSetHeight(origin.z);
                VirtualFloorInvalidate(true);
            }
            MapInvalidateTileFull(origin);
        }
        else if (operation == "place")
        {
            const auto colour = Drawing::Colour::brightRed;
            if (kind == "small")
            {
                SmallSceneryPlaceAction action(origin, 0, object(ObjectType::smallScenery), colour, colour, colour);
                apply(action, ghost);
            }
            else if (kind == "large")
            {
                LargeSceneryPlaceAction action(origin, object(ObjectType::largeScenery), colour, colour, colour);
                apply(action, ghost);
            }
            else if (kind == "wall")
            {
                WallPlaceAction action(object(ObjectType::walls), origin, origin.direction, colour, colour, colour);
                apply(action, ghost);
            }
            else if (kind == "path" || kind == "queue")
                path(origin, ghost, kind == "queue", command.value("sloped", false));
            else if (kind == "banner")
            {
                BannerPlaceAction action(origin, object(ObjectType::banners), colour);
                apply(action, ghost);
            }
            else if (kind == "entrance" || kind == "exit")
            {
                RideEntranceExitPlaceAction action({origin.x, origin.y - 32}, 1, trackRide,
                                                  StationIndex::FromUnderlying(0), kind == "exit");
                apply(action, ghost);
            }
            else if (kind == "track" || kind == "shop")
            {
                const bool shop = kind == "shop";
                createRide(shop);
                auto type = shop ? shopType : trackType;
                TrackPlaceAction action(shop ? shopRide : trackRide, GetRideTypeDescriptor(type).StartTrackPiece,
                                        type, origin, 0, 0, 0, {}, false);
                apply(action, ghost);
            }
            else throw std::runtime_error("unknown placement family");
        }
        else if (operation == "remove")
        {
            CoordsXY tile;
            if (kind == "small")
            {
                const auto* element = find(TileElementType::smallScenery, ghost, tile)->asSmallScenery();
                SmallSceneryRemoveAction action({tile, element->getBaseZ()}, element->getSceneryQuadrant(), element->getEntryIndex());
                apply(action, ghost);
            }
            else if (kind == "large")
            {
                const auto* element = find(TileElementType::largeScenery, ghost, tile)->asLargeScenery();
                LargeSceneryRemoveAction action({tile.x, tile.y, element->getBaseZ(), element->getDirection()}, element->getSequenceIndex());
                apply(action, ghost);
            }
            else if (kind == "wall")
            {
                const auto* element = find(TileElementType::wall, ghost, tile);
                WallRemoveAction action({tile.x, tile.y, element->getBaseZ(), element->getDirection()});
                apply(action, ghost);
            }
            else if (kind == "path" || kind == "queue")
            {
                const auto* element = find(TileElementType::path, ghost, tile);
                FootpathRemoveAction action({tile, element->getBaseZ()});
                apply(action, ghost);
            }
            else if (kind == "banner")
            {
                const auto* element = find(TileElementType::banner, ghost, tile)->asBanner();
                BannerRemoveAction action({tile.x, tile.y, element->getBaseZ(), element->getPosition()});
                apply(action, ghost);
            }
            else if (kind == "entrance" || kind == "exit")
            {
                find(TileElementType::entrance, ghost, tile);
                RideEntranceExitRemoveAction action(tile, trackRide, StationIndex::FromUnderlying(0), kind == "exit");
                apply(action, ghost);
            }
            else if (kind == "track" || kind == "shop")
            {
                const auto* element = find(TileElementType::track, ghost, tile)->asTrack();
                TrackRemoveAction action(element->getTrackType(), element->getSequenceIndex(),
                                         {tile.x, tile.y, element->getBaseZ(), element->getDirection()});
                apply(action, ghost);
            }
        }
        else if (operation == "land")
        {
            LandSetHeightAction action(origin + CoordsXY{command.value("dx", 0), command.value("dy", 0)},
                                       command.value("height", 18), command.value("slope", 0));
            apply(action);
        }
        else if (operation == "brush")
        {
            const CoordsXY first = origin + CoordsXY{command.value("dx", 0), command.value("dy", 0)};
            const int32_t length = (std::clamp(command.value("size", 1), 1, 64) - 1) * 32;
            const MapRange range{first, first + CoordsXY{length, length}};
            const auto selection = static_cast<MapSelectType>(command.value("selection", EnumValue(MapSelectType::full)));
            if (command.value("raise", false))
            {
                LandRaiseAction action(first, range, selection);
                apply(action);
            }
            else
            {
                LandLowerAction action(first, range, selection);
                apply(action);
            }
        }
        else if (operation == "style")
        {
            SurfaceSetStyleAction action({origin, origin}, command.value("surface", object(ObjectType::terrainSurface)),
                kObjectEntryIndexNull, Drawing::Colour::black, Drawing::Colour::black);
            apply(action);
        }
        else if (operation == "rights")
        {
            LandSetRightsAction action(origin, LandSetRightSetting::setOwnershipWithChecks,
                command.value("owned", true) ? OwnershipFlags{OwnershipFlag::landOwned} : kUnowned);
            apply(action);
        }
        else if (operation == "water")
        {
            WaterSetHeightAction action(origin, command.value("height", 20));
            apply(action);
        }
        else if (operation == "clear")
        {
            ClearAction action({{origin.x - 256, origin.y - 256}, {origin.x + 256, origin.y + 256}},
                {ClearableItem::smallScenery, ClearableItem::largeScenery, ClearableItem::walls,
                 ClearableItem::footpaths, ClearableItem::pathAdditions});
            apply(action);
        }
        else if (operation == "demolish")
        {
            RideDemolishAction action(kind == "shop" ? shopRide : trackRide, RideModifyType::demolish);
            apply(action);
        }
        else if (operation == "research")
            SetAllSceneryItemsInvented();
        else throw std::runtime_error("unknown fixture stage");
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
        json result{{"before", before}, {"after", inventory()}, {"kind", kind}, {"stage", operation},
                {"action_us", elapsed},
                {"column_builds", gPaintColumnBuilds}, {"column_patches", gPaintColumnPatches},
                {"ghost", ghost}, {"origin", {origin.x, origin.y, origin.z}},
                {"arrow", gMapSelectFlags.has(MapSelectFlag::enableArrow)}, {"ok", true}};
        result["floor_enabled"] = VirtualFloorIsEnabled();
        result["floor_height"] = VirtualFloorGetHeight();
        const auto* surface = MapGetSurfaceElementAt(origin);
        if (surface != nullptr)
            result["surface"] = {surface->getBaseZ(), surface->getSlope(), surface->getWaterHeight(),
                                  surface->getSurfaceObjectIndex(), surface->getParkFences()};
        result["selection"] = {{"on", gMapSelectFlags.has(MapSelectFlag::enable)},
            {"type", EnumValue(gMapSelectType)}, {"a", {gMapSelectPositionA.x, gMapSelectPositionA.y}},
            {"b", {gMapSelectPositionB.x, gMapSelectPositionB.y}}};
        if (operation == "floorheight")
        {
            result["floor_tiles"] = json::array();
            for (int y = origin.y - fixtureRadius; y <= origin.y + fixtureRadius; y += 32)
                for (int x = origin.x - fixtureRadius; x <= origin.x + fixtureRadius; x += 32)
                    if (VirtualFloorTileIsFloor({x, y})) result["floor_tiles"].push_back({x, y});
        }
        if (operation == "inventory")
        {
            result["ghost_tiles"] = json::array();
            for (int y = origin.y - fixtureRadius; y <= origin.y + fixtureRadius; y += 32)
                for (int x = origin.x - fixtureRadius; x <= origin.x + fixtureRadius; x += 32)
                    for (auto* element : TileElementsView<>(CoordsXY{x, y}))
                        if (element->isGhost())
                        {
                            result["ghost_tiles"].push_back({x, y});
                            break;
                        }
        }
        return result;
    }
}
