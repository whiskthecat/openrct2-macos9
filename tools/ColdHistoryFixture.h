// Linux-only pose-completion fixture. Never writes a save or changes an asset.
#pragma once
#include <openrct2/GameState.h>
#include <openrct2/Context.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/object/ObjectRepository.h>
#include <openrct2/object/WallObject.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/scenery/WallPlaceAction.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/object/WallSceneryEntry.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/MapAnimation.h>
#include <openrct2/world/TileElementsView.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <openrct2/world/tile_element/WallElement.h>
#include <openrct2/ride/ted/TrackElementDescriptor.h>
#include <cstdlib>
#include <stdexcept>

namespace ColdHistoryFixture
{
    using namespace OpenRCT2;
    using json = nlohmann::json;
    inline TileElement* selected = nullptr;
    inline CoordsXYZ location{};
    inline std::string kind;

    inline bool matches(TileElement& element, const std::string& name)
    {
        if (name == "door")
        {
            if (element.getType() != TileElementType::wall) return false;
            const auto* entry = element.asWall()->getEntry();
            return entry != nullptr && entry->flags.has(WallSceneryFlag::isDoor);
        }
        if (element.getType() != TileElementType::track) return false;
        const auto* track = element.asTrack();
        if (name == "photo") return track->getTrackType() == TrackElemType::onRidePhoto;
        if (name == "signal") return track->getTrackType() == TrackElemType::endStation;
        throw std::runtime_error("unknown cold fixture kind");
    }

    inline json state()
    {
        if (selected == nullptr) return {{"found",false}};
        json result{{"found",true},{"kind",kind},{"location",{location.x,location.y,location.z}},
            {"needs_refresh",MapAnimations::TileNeedsPaintRefresh(TileCoordsXY{location})},
            {"ticks",getGameState().currentTicks}};
        if (kind == "door") result["pose"] = {selected->asWall()->getAnimationFrame(), selected->asWall()->isAnimating()};
        else if (kind == "photo") result["pose"] = {selected->asTrack()->getPhotoTimeout()};
        else result["pose"] = {selected->asTrack()->hasGreenLight()};
        return result;
    }

    inline json run(const json& command)
    {
        if (std::getenv("RCT_SCENES_TEST") == nullptr) throw std::runtime_error("cold fixture disabled");
        const auto stage = command.value("stage",std::string("state"));
        if (stage == "select")
        {
            kind = command.at("kind").get<std::string>();
            selected = nullptr;
            for (int32_t y = 32; y < getGameState().mapSize.y * 32 - 32 && selected == nullptr; y += 32)
                for (int32_t x = 32; x < getGameState().mapSize.x * 32 - 32 && selected == nullptr; x += 32)
                    for (auto* element : TileElementsView<>(CoordsXY{x,y}))
                        if (matches(*element,kind))
                        {
                            selected = element;
                            location = {x,y,element->getBaseZ()};
                            break;
                        }
        }
        else if (stage == "make_door")
        {
            auto& context = *GetContext();
            auto& repository = context.GetObjectRepository();
            auto& manager = context.GetObjectManager();
            ObjectEntryIndex door = kObjectEntryIndexNull;
            const auto* items = repository.GetObjects();
            for (size_t i = 0; i < repository.GetNumObjects() && door == kObjectEntryIndexNull; i++)
            {
                const auto& item = items[i];
                if (item.Type != ObjectType::walls) continue;
                auto object = manager.LoadTempObject(item.Identifier, false);
                if (object == nullptr) continue;
                const auto* entry = static_cast<const WallSceneryEntry*>(object->GetLegacyData());
                if (!entry->flags.has(WallSceneryFlag::isDoor)) continue;
                auto* loaded = manager.LoadRepositoryItem(item);
                if (loaded != nullptr) door = manager.GetLoadedObjectEntryIndex(loaded);
            }
            if (door == kObjectEntryIndexNull) throw std::runtime_error("original door object unavailable");
            CoordsXYZ site{};
            bool found = false;
            for (int32_t y = 320; y < getGameState().mapSize.y * 32 - 320 && !found; y += 32)
                for (int32_t x = 320; x < getGameState().mapSize.x * 32 - 320 && !found; x += 32)
                    if (const auto* element = MapGetFirstElementAt(CoordsXY{x,y}); element != nullptr
                        && element->getType() == TileElementType::surface && element->isLastForTile())
                    {site={x,y,element->getBaseZ()+16};found=true;}
            if (!found) throw std::runtime_error("no isolated door site");
            auto& cheats = getGameState().cheats;
            cheats.sandboxMode = true; cheats.disableClearanceChecks = true;
            cheats.buildInPauseMode = true; cheats.ignoreResearchStatus = true;
            GameActions::WallPlaceAction action(door,site,0,Drawing::Colour::brightRed,
                Drawing::Colour::brightRed,Drawing::Colour::brightRed);
            const auto result = GameActions::Execute(&action, getGameState());
            if (result.error != GameActions::Status::ok) throw std::runtime_error("door placement failed");
            kind = "door"; location=site; selected=nullptr;
            for (auto* element : TileElementsView<>(CoordsXY{site}))
                if (matches(*element,kind)) {selected=element;break;}
        }
        else if (stage == "prime")
        {
            if (selected == nullptr) throw std::runtime_error("no selected fixture element");
            if (kind == "door")
            {
                selected->asWall()->setAnimationFrame(5);
                selected->asWall()->setIsAnimating(true);
                MapAnimations::MarkTileForUpdate(TileCoordsXY{location});
            }
            else if (kind == "photo")
            {
                selected->asTrack()->setPhotoTimeout(3);
                MapAnimations::CreateTemporary(location,MapAnimations::TemporaryType::onRidePhoto);
            }
            else selected->asTrack()->setHasGreenLight(true);
            MapInvalidateTile({location,selected->getClearanceZ()});
        }
        else if (stage == "complete")
        {
            if (selected == nullptr) throw std::runtime_error("no selected fixture element");
            if (kind == "door") selected->asWall()->setAnimationFrame(6);
            if (kind == "signal")
            {
                selected->asTrack()->setHasGreenLight(false);
                MapInvalidateTile({location,selected->getClearanceZ()});
            }
            // Use the real updater/removal paths while paused. Only their even
            // tick guard advances; the simulation and RNG are not stepped.
            const auto ticks = getGameState().currentTicks;
            for (uint32_t i = 0; i < 32; i++)
            {
                getGameState().currentTicks = ticks + i * 2;
                MapAnimations::InvalidateAndUpdateAll();
            }
            getGameState().currentTicks = ticks;
        }
        else if (stage != "state") throw std::runtime_error("unknown cold fixture stage");
        return state();
    }
}
