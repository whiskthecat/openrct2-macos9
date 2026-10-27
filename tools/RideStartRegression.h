// Opt-in simulation diagnostics for the newly built flat-ride failure.
// Used by rctsim and the explicit private-copy native qualification command.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/footpath/FootpathPlaceAction.h>
#include <openrct2/actions/ride/RideCreateAction.h>
#include <openrct2/actions/ride/RideEntranceExitPlaceAction.h>
#include <openrct2/actions/ride/RideSetPriceAction.h>
#include <openrct2/actions/ride/RideSetSettingAction.h>
#include <openrct2/actions/ride/RideSetStatusAction.h>
#include <openrct2/actions/terraform/ClearAction.h>
#include <openrct2/actions/track/TrackPlaceAction.h>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/object/ObjectRepository.h>
#include <openrct2/object/RideObject.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/RideManager.hpp>
#include <openrct2/ride/TrackData.h>
#include <openrct2/ride/TrainManager.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/ride/ted/TrackElementDescriptor.h>
#include <openrct2/world/Map.h>
#include <openrct2/world/TileElementsView.h>
#include <openrct2/world/tile_element/PathElement.h>
#include <openrct2/world/tile_element/SurfaceElement.h>
#include <stdexcept>
#include <vector>

namespace RideStartRegression {
using namespace OpenRCT2;
inline bool enabled() { return std::getenv("RCTSIM_RIDE_START") != nullptr; }
inline uint16_t stationLimit(const Ride &ride) {
#ifdef RCT_RIDE_REGRESSION_PINNED
  return 0xFFFF;
#else
  return ride.stationScanLimit;
#endif
}
inline GameActions::Result apply(GameActions::GameAction &action) {
  action.SetFlags({GameActions::CommandFlag::allowDuringPaused,
                   GameActions::CommandFlag::noSpend});
  auto result = GameActions::Execute(&action, getGameState());
  if (result.error != GameActions::Status::ok)
    throw std::runtime_error(std::string(action.GetName()) + ": " +
                             result.getErrorMessage());
  return result;
}
inline ObjectEntryIndex firstObject(ObjectType type) {
  auto &manager = GetContext()->GetObjectManager();
  for (ObjectEntryIndex i = 0; i < 256; ++i)
    if (manager.GetLoadedObject(type, i))
      return i;
  throw std::runtime_error("fixture object missing");
}
inline void path(CoordsXYZ at, bool queue) {
  GameActions::FootpathPlaceAction a(at, {FootpathSlopeType::flat, 0},
                                     firstObject(ObjectType::footpathSurface),
                                     firstObject(ObjectType::footpathRailings),
                                     kInvalidDirection,
                                     queue ? PathConstructFlag::IsQueue : 0);
  apply(a);
}
inline CoordsXYZD
connectedSite(const TrackMetadata::TrackElementDescriptor &ted) {
  int minX = 0, maxX = 0, minY = 0, maxY = 0;
  for (unsigned i = 0; i < ted.sequenceData.numSequences; ++i) {
    const auto &c = ted.sequenceData.sequences[i].clearance;
    minX = std::min(minX, int(c.x));
    maxX = std::max(maxX, int(c.x));
    minY = std::min(minY, int(c.y));
    maxY = std::max(maxY, int(c.y));
  }
  CoordsXYZD best{};
  int bestScore = -1;
  const auto size = GetMapSizeUnits();
  for (int y = 256; y < size.y - 96; y += 32)
    for (int x = 96; x < size.x - 160; x += 32)
      for (auto *p : TileElementsView<PathElement>(CoordsXY{x, y})) {
        if (p->isQueue() || p->isSloped())
          continue;
        const int z = p->getBaseZ();
        CoordsXYZD origin{x - minX, y - maxY - 96, z, 0};
        bool okay = true;
        for (int dy = minY; dy <= maxY + 64 && okay; dy += 32)
          for (int dx = minX; dx <= maxX && okay; dx += 32) {
            const CoordsXY loc{origin.x + dx, origin.y + dy};
            const auto *s = MapGetSurfaceElementAt(loc);
            if (!s || s->getSlope() != 0 || s->getBaseZ() != z ||
                s->getWaterHeight() != 0) {
              okay = false;
              break;
            }
            for (auto *e : TileElementsView<>(loc))
              if (e->getType() == TileElementType::track ||
                  e->getType() == TileElementType::entrance ||
                  e->getType() == TileElementType::path) {
                okay = false;
                break;
              }
          }
        if (!okay)
          continue;
        int score = 0;
        for (int dy = -64; dy <= 64; dy += 32)
          for (int dx = -64; dx <= 64; dx += 32)
            for (auto id : getGameState().entities.getEntityTileList(
                     CoordsXY{x + dx, y + dy}))
              if (getGameState().entities.tryGetEntity<Guest>(id))
                ++score;
        if (score > bestScore) {
          bestScore = score;
          best = origin;
        }
      }
  if (bestScore < 0)
    throw std::runtime_error("no flat connected fixture site");
  return best;
}
inline nlohmann::json constructCase(const nlohmann::json &spec) {
  using nlohmann::json;
  auto &state = getGameState();
  state.cheats.sandboxMode = true;
  state.cheats.disableClearanceChecks = true;
  state.cheats.disableSupportLimits = true;
  state.cheats.ignoreResearchStatus = true;
  auto &manager = GetContext()->GetObjectManager();
  auto &repository = GetContext()->GetObjectRepository();
  const ride_type_t type = spec.value("type", RIDE_TYPE_TWIST);
  Object *object = nullptr;
  if (spec.contains("object")) {
    object = manager.LoadObject(spec.at("object").get<std::string>());
    if (!object) throw std::runtime_error("requested fixture object unavailable");
  }
  const auto *items = repository.GetObjects();
  for (size_t i = 0; i < repository.GetNumObjects() && !object; ++i)
    if (items[i].Type == ObjectType::ride)
      for (auto candidate : items[i].RideInfo.RideType)
        if (candidate == type) {
          object = manager.LoadObject(items[i].Identifier);
          break;
        }
  if (!object)
    throw std::runtime_error("ride object unavailable");
  const auto station = firstObject(ObjectType::station);
  GameActions::RideCreateAction create(
      type, manager.GetLoadedObjectEntryIndex(object), 0, 0, station,
      RideInspection::every30Minutes);
  const auto id = apply(create).getData<RideId>();
  auto *ride = GetRide(id);
  for (unsigned i = 0; i < spec.value("delay", 1u); ++i)
    gameStateUpdateLogic();
  const auto before = stationLimit(*ride);
  CoordsXYZD origin{768, 768, 0, 0};
  const auto *surface = MapGetSurfaceElementAt(CoordsXY{origin.x, origin.y});
  if (!surface)
    throw std::runtime_error("fixture surface missing");
  origin.z = surface->getBaseZ() + 64;
  const auto &descriptor = GetRideTypeDescriptor(type);
  const auto &ted =
      TrackMetadata::GetTrackElementDescriptor(descriptor.StartTrackPiece);
  const bool guests = spec.value("guests", false);
  const bool shop = descriptor.flags.has(RtdFlag::isShopOrFacility);
  if (guests) {
    origin = connectedSite(ted);
    GameActions::ClearAction clear(
        {origin.x - 96, origin.y - 96, origin.x + 192, origin.y + 192},
        {GameActions::ClearableItem::smallScenery,
         GameActions::ClearableItem::largeScenery,
         GameActions::ClearableItem::walls});
    apply(clear);
  }
  GameActions::TrackPlaceAction track(id, descriptor.StartTrackPiece, type,
                                      origin, 0, 0, 0, {}, false);
  apply(track);
  const auto after = stationLimit(*ride);
#ifndef RCT_RIDE_REGRESSION_PINNED
  if (spec.value("repair", false))
    ride->stationScanLimit = Ride::kStationScanUnknown;
#endif
  json connections = json::array();
  if (guests && !shop) {
    int minX = 0, maxX = 0, maxY = 0;
    for (unsigned i = 0; i < ted.sequenceData.numSequences; ++i) {
      const auto &c = ted.sequenceData.sequences[i].clearance;
      minX = std::min(minX, int(c.x));
      maxX = std::max(maxX, int(c.x));
      maxY = std::max(maxY, int(c.y));
    }
    const int y = origin.y + maxY + 32;
    std::vector<CoordsXY> entrances;
    for (int x = origin.x + minX; x <= origin.x + maxX; x += 32) {
      GameActions::RideEntranceExitPlaceAction a(
          {x, y}, 3, id, StationIndex::FromUnderlying(0), false);
      if (a.Query(state, state.park).error == GameActions::Status::ok)
        entrances.push_back({x, y});
    }
    if (entrances.size() < 2)
      throw std::runtime_error("fixture needs two entrance/exit sites");
    for (unsigned i = 0; i < 2; ++i) {
      const auto loc = i == 0 ? entrances.front() : entrances.back();
      GameActions::RideEntranceExitPlaceAction a(
          loc, 3, id, StationIndex::FromUnderlying(0), i == 1);
      apply(a);
      path({loc.x, loc.y + 32, origin.z}, i == 0);
      connections.push_back({loc.x, loc.y, 3});
    }
    for (int x = entrances.front().x; x <= entrances.back().x; x += 32)
      path({x, y + 64, origin.z}, false);
    GameActions::RideSetPriceAction price(id, 0, true);
    apply(price);
    GameActions::RideSetSettingAction depart(
        id, GameActions::RideSetSetting::departure,
        RIDE_DEPART_WAIT_FOR_MINIMUM_LENGTH |
            RIDE_DEPART_WAIT_FOR_MAXIMUM_LENGTH);
    apply(depart);
    GameActions::RideSetSettingAction minWait(
        id, GameActions::RideSetSetting::minWaitingTime, 3);
    apply(minWait);
    GameActions::RideSetSettingAction maxWait(
        id, GameActions::RideSetSetting::maxWaitingTime, 10);
    apply(maxWait);
  }
  if (guests && shop) {
    // Join every side to the same public path: no queue or ride entrance
    // is needed by a shop. All changes still use footpath actions.
    for (int dy = -32; dy <= 32; dy += 32)
      for (int dx = -32; dx <= 32; dx += 32)
        if (dx || dy)
          path({origin.x + dx, origin.y + dy, origin.z}, false);
    for (int dy = 64; dy <= 96; dy += 32)
      path({origin.x, origin.y + dy, origin.z}, false);
  }
  GameActions::RideSetStatusAction start(id, guests ? RideStatus::open
                                                    : RideStatus::simulating);
  apply(start);
  return json{{"id", id.ToUnderlying()},
              {"type", type},
              {"cache_before", before},
              {"cache_after", after},
              {"repair", spec.value("repair", false)},
              {"origin", {origin.x, origin.y, origin.z}},
              {"connections", connections}};
}
inline void construct() {
  if (const auto *argument = std::getenv("RCTSIM_BUILD_RIDE"))
    std::printf("constructed %s\n",
                constructCase(nlohmann::json::parse(argument)).dump().c_str());
}
inline void configure() {
#if !defined(RCT_RIDE_REGRESSION_PINNED) &&                                    \
    !defined(RCT_RIDE_START_NO_CONTROL_API) && !defined(OPENRCT2_CLASSIC_MAC)
  if (auto *value = std::getenv("RCTSIM_ORDERED_IDS"))
    gUseOrderedEntityLists = std::atoi(value) != 0;
  if (auto *value = std::getenv("RCTSIM_TRAIN_ORDERED"))
    TrainManager::gUseOrderedTrainHeads = std::atoi(value) != 0;
  if (auto *value = std::getenv("RCTSIM_CROWD_FUSED"))
    gUseFusedCrowdNoise = std::atoi(value) != 0;
  if (auto *value = std::getenv("RCTSIM_VEHICLE_LOCAL"))
    gUseVehicleLocalMetadata = std::atoi(value) != 0;
#endif
}
inline nlohmann::json describe(const Ride &ride) {
  using nlohmann::json;
  json r{
      {"id", ride.id.ToUnderlying()},     {"type", ride.type},
      {"name", ride.getName()},           {"status", EnumValue(ride.status)},
      {"mode", EnumValue(ride.mode)},     {"riders", ride.numRiders},
      {"customers", ride.totalCustomers}, {"station_limit", stationLimit(ride)},
      {"depart", ride.departFlags},       {"min_wait", ride.minWaitingTime},
      {"max_wait", ride.maxWaitingTime},  {"stations", json::array()},
      {"vehicles", json::array()},        {"guests", json::array()}};
  for (auto &s : ride.getStations())
    if (!s.start.isNull())
      r["stations"].push_back(
          {{"start", {s.start.x, s.start.y, s.height}},
           {"entrance",
            {s.entrance.x, s.entrance.y, s.entrance.z, s.entrance.direction}},
           {"exit", {s.exit.x, s.exit.y, s.exit.z, s.exit.direction}},
           {"depart", s.depart},
           {"train", s.trainAtStation},
           {"queue", s.queueLength},
           {"last", s.lastPeepInQueue.ToUnderlying()}});
  for (auto *v : EntityList<Vehicle>())
    if (v->ride == ride.id)
      r["vehicles"].push_back({{"id", v->id.ToUnderlying()},
                               {"head", v->IsHead()},
                               {"state", EnumValue(v->status)},
                               {"peeps", v->num_peeps},
                               {"seats", v->num_seats},
                               {"restraints", v->restraints_position},
                               {"waiting", v->time_waiting},
                               {"pos", {v->x, v->y, v->z}},
                               {"frame", v->flatRideAnimationFrame}});
  for (auto *g : EntityList<Guest>())
    if (g->currentRide == ride.id &&
        (g->state == PeepState::queuingFront ||
         g->state == PeepState::enteringRide || g->state == PeepState::onRide))
      if (r["guests"].size() < 12)
        r["guests"].push_back(
            {{"id", g->id.ToUnderlying()},
             {"state", EnumValue(g->state)},
             {"sub", g->subState},
             {"pos", {g->x, g->y, g->z}},
             {"station", g->currentRideStation.ToUnderlying()}});
  return r;
}
inline void observe(int elapsed) {
  if (!enabled())
    return;
  nlohmann::json result{{"elapsed", elapsed},
                        {"rides", nlohmann::json::array()}};
  for (auto &ride : RideManager(getGameState())) {
    result["rides"].push_back(describe(ride));
  }
  std::printf("ride-start %s\n", result.dump().c_str());
}
} // namespace RideStartRegression
