// Host-only scene census/configuration. Actions still use the ordinary remote input.
#pragma once
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/interface/ViewportFlags.h>
#include <openrct2/interface/WindowBase.h>
#include <openrct2/world/TileElementsView.h>
#include <openrct2/world/MapSelection.h>
#include <openrct2/world/tile_element/TrackElement.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
#include <openrct2-ui/interface/LandTool.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/terraform/LandRaiseAction.h>
#include <map>

namespace ScenesFixture
{
    inline nlohmann::json run(const nlohmann::json& command)
    {
        using namespace OpenRCT2;
        using json = nlohmann::json;
        if (std::getenv("RCT_SCENES_TEST") == nullptr)
            throw std::runtime_error("scene survey requires explicit host test mode");
        auto* main = WindowGetMain();
        if (!main || !main->viewport) throw std::runtime_error("no main viewport");
        if (command.contains("edge")) Config::Get().general.edgeScrolling = command.at("edge").get<bool>();
        if (command.contains("brush")) gLandToolSize = std::clamp(command.at("brush").get<int>(), 1, 64);
        if (command.contains("flags"))
        {
            const std::map<std::string, ViewportFlag> flags = {
                {"underground", ViewportFlag::undergroundInside}, {"rides", ViewportFlag::hideRides},
                {"scenery", ViewportFlag::hideScenery}, {"supports", ViewportFlag::hideSupports},
                {"vegetation", ViewportFlag::hideVegetation},
                {"invisible_supports", ViewportFlag::invisibleSupports},
                {"paths", ViewportFlag::hidePaths}, {"gridlines", ViewportFlag::gridlines},
                {"land_heights", ViewportFlag::landHeights}, {"track_heights", ViewportFlag::trackHeights},
                {"path_heights", ViewportFlag::pathHeights}, {"ownership", ViewportFlag::landOwnership}};
            for (const auto& [name, flag] : flags) main->viewport->flags.set(flag, false);
            for (const auto& name : command.at("flags")) main->viewport->flags.set(flags.at(name.get<std::string>()));
            main->invalidate();
        }
        json result{{"flags", main->viewport->flags.holder}, {"brush", gLandToolSize},
            {"edge", Config::Get().general.edgeScrolling}, {"selection", gMapSelectFlags.has(MapSelectFlag::enable)},
            {"arrow", gMapSelectFlags.has(MapSelectFlag::enableArrow)}, {"tool", gInputFlags.has(InputFlag::toolActive)}};
        if (command.value("terrain", false))
        {
            result["terrain"] = json::array();
            const auto size = getGameState().mapSize;
            for (int y = 0; y < size.y; y++) for (int x = 0; x < size.x; x++)
                if (const auto* surface = MapGetSurfaceElementAt(CoordsXY{x*32, y*32}))
                    result["terrain"].push_back({x*32, y*32, surface->getBaseZ(),
                                                 surface->getSlope(), surface->getWaterHeight()});
            result["cash_raw"] = getGameState().park.cash;
            result["range"] = {{gMapSelectPositionA.x, gMapSelectPositionA.y},
                                {gMapSelectPositionB.x, gMapSelectPositionB.y}};
        }
        if (command.value("raise_query", false))
        {
            if (!gMapSelectFlags.has(MapSelectFlag::enable))
                throw std::runtime_error("raise query requires an active land selection");
            const CoordsXY centre{(gMapSelectPositionA.x + gMapSelectPositionB.x)/2 + 16,
                                   (gMapSelectPositionA.y + gMapSelectPositionB.y)/2 + 16};
            GameActions::LandRaiseAction action(centre, getMapSelectRange(), gMapSelectType);
            const auto query = GameActions::Query(&action, getGameState());
            result["raise_query"] = {{"error", EnumValue(query.error)}, {"cost_raw", query.cost},
                                     {"message", query.getErrorMessage()}};
        }
        if (command.contains("entity_ids"))
        {
            result["entities"] = json::array();
            for (const auto& id : command["entity_ids"])
                if (const auto* entity = getGameState().entities.tryGetEntity(EntityId::FromUnderlying(id.get<uint16_t>())))
                    result["entities"].push_back({{"id",entity->id.ToUnderlying()}, {"type",EnumValue(entity->type)},
                        {"position",{entity->x,entity->y,entity->z}}, {"bounds",entity->sampledBounds()}});
        }
        if (!command.value("census", false)) return result;
        const auto rotation = main->viewport->rotation;
        std::vector<CoordsXY> guests;
        for (auto* guest : EntityList<Guest>())
            if (guest->x != kLocationNull)
            {
                const auto p = Translate3DTo2DWithZ(rotation, {guest->x,guest->y,guest->z});
                guests.push_back({p.x,p.y});
            }
        struct Candidate { CoordsXYZ loc; CoordsXY view; int guests = 0, water = 0, elements = 0; };
        std::vector<Candidate> candidates;
        std::map<uint16_t,int> tracks, vehicles;
        const auto size = getGameState().mapSize;
        for (int y = 1; y < size.y - 1; y++) for (int x = 1; x < size.x - 1; x++)
            for (auto* element : TileElementsView<>(CoordsXY{x*32,y*32}))
                if (auto* track = element->asTrack()) tracks[track->getRideIndex().ToUnderlying()]++;
        for (auto* vehicle : EntityList<Vehicle>()) vehicles[vehicle->ride.ToUnderlying()]++;
        for (int y = 8; y < size.y - 8; y += 8) for (int x = 8; x < size.x - 8; x += 8)
        {
            Candidate c; c.loc = {x*32,y*32,TileElementHeight({x*32,y*32})};
            const auto centre = Translate3DTo2DWithZ(rotation, c.loc); c.view = {centre.x-320,centre.y-208};
            for (const auto& guest : guests)
                if (guest.x >= c.view.x && guest.x < c.view.x+640 && guest.y >= c.view.y && guest.y < c.view.y+416) c.guests++;
            for (int yy = y-8; yy <= y+8; yy++) for (int xx = x-8; xx <= x+8; xx++)
                for (auto* element : TileElementsView<>(CoordsXY{xx*32,yy*32}))
                {
                    c.elements++;
                    if (auto* surface = element->asSurface(); surface && surface->getWaterHeight()) c.water++;
                }
            candidates.push_back(c);
        }
        auto encode = [](const Candidate& c) { return json{{"map",{c.loc.x,c.loc.y,c.loc.z}},
            {"view",{c.view.x,c.view.y}}, {"guests",c.guests}, {"water_tiles",c.water}, {"elements",c.elements}}; };
        result["crowd"] = encode(*std::max_element(candidates.begin(),candidates.end(),[](const auto&a,const auto&b){return a.guests<b.guests;}));
        result["water"] = encode(*std::max_element(candidates.begin(),candidates.end(),[](const auto&a,const auto&b){return a.water<b.water;}));
        result["empty"] = encode(*std::min_element(candidates.begin(),candidates.end(),[](const auto&a,const auto&b){return std::tie(a.guests,a.water,a.elements)<std::tie(b.guests,b.water,b.elements);}));
        result["rides"] = json::array();
        for (const auto& [id, count] : tracks) result["rides"].push_back({{"id",id},{"track_elements",count},{"vehicles",vehicles[id]}});
        result["guest_count"] = guests.size();
        return result;
    }
}
