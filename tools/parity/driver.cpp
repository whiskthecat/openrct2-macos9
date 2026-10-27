// Identical action driver linked to the pinned, unmodified engine and the port.
// Test setup and action dispatch live here; engine simulation code stays
// intact.
#include <nlohmann/json.hpp>
#include <openrct2/Context.h>
#include <openrct2/Game.h>
#include <openrct2/GameState.h>
#include <openrct2/OpenRCT2.h>
#include <openrct2/actions/GameActionParameterVisitor.h>
#include <openrct2/actions/GameActionRegistry.h>
#include <openrct2/actions/GameActionRunner.h>
#include <openrct2/actions/peep/StaffHireNewAction.h>
#include <openrct2/actions/ride/RideCreateAction.h>
#include <openrct2/audio/Audio.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/Crypt.h>
#include <openrct2/core/MemoryStream.h>
#include <openrct2/entity/EntityList.h>
#include <openrct2/entity/Guest.h>
#include <openrct2/entity/Staff.h>
#include <openrct2/interface/Viewport.h>
#include <openrct2/interface/WindowBase.h>
#include <openrct2/localisation/Language.h>
#include <openrct2/management/Research.h>
#include <openrct2/object/ObjectManager.h>
#include <openrct2/object/ObjectRepository.h>
#include <openrct2/park/ParkFile.h>
#include <openrct2/ride/RideData.h>
#include <openrct2/ride/RideManager.hpp>
#include <openrct2/ride/TrackData.h>
#include <openrct2/ride/Vehicle.h>
#include <openrct2/ride/ted/TrackElementDescriptor.h>
#ifndef PARITY_ORACLE
#include <openrct2/ride/TrainManager.h>
#endif
#include "Encoding.h"
#include <fstream>
#include <iostream>
#include <map>
#include <openrct2/scenario/Scenario.h>
#include <openrct2/util/Util.h>
#include <set>
#ifndef PARITY_ORACLE
#include "BuildIdentity.h"
#endif
using namespace OpenRCT2;
using namespace OpenRCT2::TrackMetadata;
using json = nlohmann::json;
static json variables = json::object();
static int outerGrass = 0;
static std::map<unsigned, uint32_t> extraDepart;
static std::map<unsigned, bool> seenVehicles;
static json coverage = json::object();
static void observe() {
    for (auto *p : EntityList<Guest>()) {
        if (p->currentRide.IsNull())
            continue;
        auto &v = coverage[std::to_string(p->currentRide.ToUnderlying())];
        if (v.is_null())
            v = {{"queue", 0}, {"enter", 0}, {"on", 0}, {"leave", 0}, {"buy", 0}};
        const char *key = nullptr;
        switch (p->state) {
        case PeepState::queuing:
        case PeepState::queuingFront:
            key = "queue";
            break;
        case PeepState::enteringRide:
            key = "enter";
            break;
        case PeepState::onRide:
            key = "on";
            break;
        case PeepState::leavingRide:
            key = "leave";
            break;
        case PeepState::buying:
            key = "buy";
            break;
        default:
            break;
        }
        if (key)
            v[key] = v[key].get<uint64_t>() + 1;
    }
}
static WindowBase *testWindow = nullptr;
static void countView(int x = 1024, int y = 1024) {
    // A real core viewport/window, without a drawing engine or audio device.
    // This activates the production crowd guard and map invalidation paths.
    if (!testWindow) {
        auto window = std::make_unique<WindowBase>();
        window->classification = WindowClass::mainWindow;
        window->width = 640;
        window->height = 480;
        testWindow = window.get();
        gWindowList.push_back(std::move(window));
        ViewportCreate(*testWindow, {0, 0}, 640, 480, Focus(CoordsXYZ{x, y, 112}));
        if (!testWindow->viewport)
            throw std::runtime_error("headless viewport allocation failed");
        testWindow->viewport->flags.set(ViewportFlag::soundOn);
    }
    auto centre = Translate3DTo2DWithZ(0, {x, y, 112});
    testWindow->viewport->viewPos = {centre.x - 320, centre.y - 240};
    WindowUpdateViewportRideMusic();
    if (Audio::gGameSoundsOff || !Config::Get().sound.soundEnabled)
        throw std::runtime_error("count-only viewport fixture disabled by configuration");
}
static json catalog() {
    json c = {{"objects", json::array()}, {"types", json::array()}, {"track", json::array()}};
    auto &repo = GetContext()->GetObjectRepository();
    auto &manager = GetContext()->GetObjectManager();
    for (size_t i = 0; i < repo.GetNumObjects(); ++i) {
        const auto &o = repo.GetObjects()[i];
        json v = {{"id", o.Identifier},
                  {"name", o.Name},
                  {"type", o.Type},
                  {"slot", manager.GetLoadedObjectEntryIndex(o.Identifier)}};
        if (o.Type == ObjectType::ride) {
            v["rides"] = json::array();
            for (auto t : o.RideInfo.RideType)
                if (t != kRideTypeNull)
                    v["rides"].push_back(t);
        }
        c["objects"].push_back(v);
    }
    for (ride_type_t t = 0; t < RIDE_TYPE_COUNT; ++t) {
        const auto &d = GetRideTypeDescriptor(t);
        c["types"].push_back({{"type", t},
                              {"name", d.Name},
                              {"category", d.Category},
                              {"start", d.StartTrackPiece},
                              {"mode", d.DefaultMode},
                              {"flat", d.flags.has(RtdFlag::isFlatRide)},
                              {"shop", d.flags.has(RtdFlag::isShopOrFacility)},
                              {"modes", d.rideModes.holder}});
    }
    for (int t = 0; t < 350; ++t) {
        const auto &d = GetTrackElementDescriptor(static_cast<TrackElemType>(t));
        const auto &p = d.coordinates;
        json seq = json::array();
        for (int i = 0; i < d.sequenceData.numSequences; ++i) {
            const auto &q = d.sequenceData.sequences[i];
            seq.push_back({q.clearance.x, q.clearance.y, q.clearance.z, q.getEntranceConnectionSides()});
        }
        c["track"].push_back(
            {{"type", t}, {"coords", {p.x, p.y, p.zBegin, p.zEnd, p.rotationBegin, p.rotationEnd}}, {"seq", seq}});
    }
    return c;
}

struct Parameters final : GameActions::GameActionParameterVisitor {
    json values, seen = json::object();
    explicit Parameters(json v) : values(std::move(v)) {}
    void Visit(std::string_view n, bool &p) override {
        const std::string k(n);
        if (values.contains(k))
            p = values[k].get<bool>();
        seen[k] = p;
    }
    void Visit(std::string_view n, int32_t &p) override {
        const std::string k(n);
        if (values.contains(k)) {
            auto v = values[k];
            if (v.is_string()) {
                const auto str = v.get<std::string>();
                if (str[0] == '@')
                    p = GetContext()->GetObjectManager().GetLoadedObjectEntryIndex(str.substr(1));
                else
                    p = variables.at(str).get<int32_t>();
            } else
                p = v.get<int32_t>();
        }
        seen[k] = p;
    }
    void Visit(std::string_view n, std::string &p) override {
        const std::string k(n);
        if (values.contains(k))
            p = values[k].get<std::string>();
        seen[k] = p;
    }
    void check() {
        for (auto i = values.begin(); i != values.end(); ++i)
            if (!seen.contains(i.key()))
                throw std::runtime_error("unused action parameter: " + i.key());
    }
};
static json rides() {
    json a = json::array();
    for (auto &r : RideManager(getGameState())) {
        json stations = json::array();
        for (auto &s : r.getStations().first(4))
            stations.push_back({{"start", {s.start.x, s.start.y}},
                                {"height", s.height},
                                {"queue", s.queueLength},
                                {"depart", s.depart}});
        a.push_back({{"id", r.id.ToUnderlying()},
                     {"type", r.type},
                     {"status", r.status},
                     {"mode", r.mode},
                     {"numRiders", r.numRiders},
                     {"customers", r.totalCustomers},
                     {"trains", r.numTrains},
                     {"cars", r.numCarsPerTrain},
                     {"subtype", r.subtype},
                     {"blocks", r.numBlockBrakes},
                     {"stations", stations}});
    }
    return a;
}
static json guests() {
    json a = json::array();
    for (auto *p : EntityList<Guest>())
        a.push_back({p->id.ToUnderlying(), int(p->state), p->subState, p->x, p->y, p->z, p->currentRide.ToUnderlying(),
                     p->currentRideStation.ToUnderlying(), p->currentTrain, p->currentCar, p->currentSeat,
                     p->cashInPocket, p->happiness});
    return a;
}
static json staffAndVehicles() {
    json staff = json::array(), vehicles = json::array();
    for (auto *p : EntityList<Staff>())
        staff.push_back({{"id", p->id.ToUnderlying()},
                         {"type", p->assignedStaffType},
                         {"state", p->state},
                         {"sub", p->subState},
                         {"pos", {p->x, p->y, p->z}},
                         {"orders", p->staffOrders},
                         {"fixed", p->staffRidesFixed},
                         {"inspected", p->staffRidesInspected}});
    for (auto *v : EntityList<Vehicle>())
        vehicles.push_back({{"id", v->id.ToUnderlying()},
                            {"ride", v->ride.ToUnderlying()},
                            {"state", v->status},
                            {"sub", v->sub_state},
                            {"pos", {v->x, v->y, v->z}},
                            {"velocity", v->velocity},
                            {"acceleration", v->acceleration},
                            {"progress", v->track_progress},
                            {"station", v->current_station.ToUnderlying()},
                            {"passengers", v->num_peeps}});
    return {{"staff", staff}, {"vehicles", vehicles}};
}
static std::string hashBytes(const ParityEncoding::Bytes &bytes) {
    // The pinned incremental implementation underflows for a tiny next chunk.
    // One call on concatenated payloads is the same FNV, without that bug.
    std::string result;
    char b[3];
    for (auto v : Crypt::FNV1a(bytes.data(), bytes.size())) {
        std::snprintf(b, sizeof(b), "%02x", v);
        result += b;
    }
    return result;
}
#ifndef PARITY_ORACLE
static void auditIndices() {
    auto &registry = getGameState().entities;
    unsigned total = 0;
    for (unsigned type = 0; type < unsigned(EntityType::count); ++type) {
        const auto kind = static_cast<EntityType>(type);
        auto next = registry.getEntityListHead(kind);
        for (auto id : registry.getEntityList(kind)) {
            ParityEncoding::require(next == id, "ordered successor differs from registry list");
            auto *e = registry.getEntity(id);
            ParityEncoding::require(e && e->type == kind, "entity list contains wrong slot/type");
            next = registry.getNextEntityInList(id);
            ++total;
        }
        ParityEncoding::require(next.IsNull(), "ordered successor has trailing/cyclic entity");
    }
    ParityEncoding::require(total + registry.getNumFreeEntities() == kMaxEntities, "entity free/list accounting");
    const auto *occupied = registry.occupiedTiles();
    for (int x = 0; x < 256; ++x)
        for (int y = 0; y < 256; ++y) {
            const unsigned index = x * 256 + y;
            const auto &list = registry.getEntityTileList({x * 32, y * 32});
            ParityEncoding::require(bool((occupied[index >> 3] >> (index & 7)) & 1) == !list.empty(),
                                    "entity tile occupancy bit differs from spatial list");
            for (auto id : list) {
                auto *e = registry.getEntity(id);
                ParityEncoding::require(e && (e->spatialIndex & ~kSpatialIndexDirtyMask) == index,
                                        "entity spatial membership differs from stored index");
            }
        }
}
#endif
static bool finalOnly = false;
static void emit(const std::string &label, const json &actionResult = nullptr, bool endpoint = false) {
    auto &state = getGameState();
#ifndef PARITY_ORACLE
    auditIndices();
#endif
    for (auto &ride : RideManager(state)) {
        const auto id = ride.id.ToUnderlying();
        const bool present = !ride.vehicles[0].IsNull();
        if (seenVehicles.contains(id) && !seenVehicles[id] && present)
            extraDepart[id] = 1;
        seenVehicles[id] = present;
    }
    const auto &rand = ScenarioRandState();
    json result = {{"label", label},
                   {"tick", state.currentTicks},
                   {"rand", {rand.s0, rand.s1}},
                   {"rides", rides()},
                   {"guests", guests()},
                   {"coverage", coverage},
                   {"details", staffAndVehicles()},
                   {"action", actionResult},
                   {"fnv", nullptr},
                   {"canonical_fnv", nullptr},
                   {"chunks", nullptr},
                   {"canonical_chunks", nullptr}};
    result["calendar"] = {{"months", state.date.monthsElapsed},
                          {"month_ticks", state.date.monthTicks},
                          {"objective", state.scenarioOptions.objective.Type},
                          {"completed_value", state.scenarioCompletedCompanyValue},
                          {"park_value", state.park.value},
                          {"company_value", state.park.companyValue},
                          {"awards", json::array()},
                          {"research", {state.researchProgress, state.researchProgressStage}},
                          {"invented", state.researchItemsInvented.size()},
                          {"uninvented", state.researchItemsUninvented.size()}};
    for (const auto &award : state.park.currentAwards)
        result["calendar"]["awards"].push_back({award.time, award.type});
    if (!finalOnly || endpoint) {
        MemoryStream bytes;
        ParkFileExporter exporter;
        std::vector<uint8_t> before(sizeof(state));
        std::memcpy(before.data(), &state, sizeof(state));
        exporter.Export(state, bytes, 0);
        ParityEncoding::require(std::memcmp(before.data(), &state, sizeof(state)) == 0,
                                "checkpoint export mutated live GameState/cache bytes");
        if (const char *path = std::getenv("PARITY_DUMP")) {
            std::ofstream out(path, std::ios::binary);
            out.write(static_cast<const char *>(bytes.GetData()), bytes.GetLength());
        }
        const auto *p = static_cast<const uint8_t *>(bytes.GetData());
        auto le = [p](size_t i) { return ParityEncoding::u32(p + i); };
        const auto count = le(12);
        const size_t body = 64 + 20 * count;
        ParityEncoding::Bytes raw, canonical;
        json chunks = json::object(), canonicalChunks = json::object();
        for (unsigned i = 0; i < count; ++i) {
            const size_t entry = 64 + 20 * i;
            const auto kind = le(entry);
            if (kind == 1)
                continue;
            const size_t offset = le(entry + 4), size = le(entry + 12);
            ParityEncoding::require(body + offset + size <= bytes.GetLength(), "exported chunk out of range");
            const auto *data = p + body + offset;
            ParityEncoding::Bytes payload(data, data + size);
            raw.insert(raw.end(), payload.begin(), payload.end());
            chunks[std::to_string(kind)] = hashBytes(payload);
            auto adapted = kind == 48   ? ParityEncoding::tiles(data, size, outerGrass)
                           : kind == 50 ? ParityEncoding::rides(data, size, RideManager(state), extraDepart)
                                        : std::move(payload);
            canonical.insert(canonical.end(), adapted.begin(), adapted.end());
            canonicalChunks[std::to_string(kind)] = hashBytes(adapted);
        }
        result["fnv"] = hashBytes(raw);
        result["canonical_fnv"] = hashBytes(canonical);
        result["chunks"] = chunks;
        result["canonical_chunks"] = canonicalChunks;
    }
    std::cout << "PARITY " << result.dump() << std::endl;
}
int main(int argc, char **argv) try {
#ifndef PARITY_ORACLE
    if (!RctBuild::Start())
        return 125;
#endif
    if (argc < 3)
        throw std::runtime_error("usage: rctparity PARK SCRIPT.jsonl");
    if (const char *user = std::getenv("PARITY_USER"))
        gCustomUserDataPath = user;
    if (const char *p = std::getenv("PARITY_DATA")) {
        gCustomOpenRCT2DataPath = std::string(p) + "/open";
        gCustomRCT2DataPath = std::string(p) + "/game";
        if (!std::getenv("PARITY_USER"))
            gCustomUserDataPath = std::string(p) + "/user";
    }
    gOpenRCT2Headless = true;
    gOpenRCT2NoGraphics = true;
    std::unique_ptr<IContext> ctx(CreateContext());
    Config::Get().general.language = LANGUAGE_ENGLISH_US;
    if (!ctx->Initialise())
        throw std::runtime_error("context init failed");
    if (!ctx->LoadParkFromFile(argv[1]))
        throw std::runtime_error("load failed");
    ScenarioRandSeed(0x1234567F, 0x789FABCD);
    if (std::string(argv[2]) == "--catalog") {
        std::cout << "CATALOG " << catalog().dump() << std::endl;
        return 0;
    }
#ifndef PARITY_ORACLE
    std::cerr << "SWITCHES "
              << json({{"sim_ordered_ids", gUseOrderedEntityLists},
                       {"sim_crowd_fused", gUseFusedCrowdNoise},
                       {"sim_train_ordered", TrainManager::gUseOrderedTrainHeads},
                       {"sim_vehicle_local", gUseVehicleLocalMetadata},
#ifdef OPENRCT2_LEGACY_GUEST_LAYOUT
                       {"sim_guest_hot", false}
#else
                       {"sim_guest_hot", true}
#endif
                      })
                     .dump()
              << std::endl;
    if (const auto *control = std::getenv("PARITY_SWITCHES")) {
        auto v = json::parse(control);
        gUseOrderedEntityLists = v.value("sim_ordered_ids", gUseOrderedEntityLists);
        gUseFusedCrowdNoise = v.value("sim_crowd_fused", gUseFusedCrowdNoise);
        gUseVehicleLocalMetadata = v.value("sim_vehicle_local", gUseVehicleLocalMetadata);
    }
#endif
    finalOnly = std::getenv("PARITY_FINAL_ONLY") != nullptr;
    emit("loaded");
    std::ifstream script(argv[2]);
    if (!script)
        throw std::runtime_error("script missing");
    std::string line;
    unsigned step = 0;
    while (std::getline(script, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        auto row = json::parse(line);
        const auto label = std::to_string(++step) + ":" +
                           row.value("label", row.value("action", row.value("op", std::string("ticks"))));
        if (row.contains("ticks")) {
            int n = row["ticks"], every = row.value("every", 400);
            for (int t = 1; t <= n; ++t) {
                gameStateUpdateLogic();
                observe();
                if (t % every == 0 || t == n)
                    emit(label + ":" + std::to_string(t));
            }
            continue;
        }
        auto op = row.value("op", std::string());
        if (op == "newpark") {
            int size = row.value("size", 128);
            gameStateInitAll(getGameState(), {size, size});
            outerGrass = 1;
            coverage = json::object();
            extraDepart.clear();
            seenVehicles.clear();
            ScenarioRandSeed(0x1234567F, 0x789FABCD);
            emit(label);
            continue;
        }
        if (op == "research_fixture") {
            // Initial scenario fixture only, using the editor's population API.
            // Funding and subsequent progression use actual game actions/ticks.
            if (getGameState().currentTicks != 0)
                throw std::runtime_error("research fixture must precede construction/ticks");
            ResearchPopulateListRandom();
            ResearchResetCurrentItem();
            if (getGameState().researchItemsUninvented.empty())
                throw std::runtime_error("research fixture has no inventions left");
            emit(label);
            continue;
        }
        if (op == "loadobject") {
            auto *obj = ctx->GetObjectManager().LoadObject(row.at("id").get<std::string>());
            if (!obj)
                throw std::runtime_error("object load failed");
            emit(label, {{"object", ctx->GetObjectManager().GetLoadedObjectEntryIndex(obj)}});
            continue;
        }
        if (op == "viewport") {
            countView(row.value("x", 1024), row.value("y", 1024));
            emit(label);
            continue;
        }
        if (op == "remember") {
            variables[row.at("name").get<std::string>()] = variables.at(row.at("value").get<std::string>());
            emit(label);
            continue;
        }
        if (op == "selectguest") {
            Guest *selected = nullptr;
            for (auto *p : EntityList<Guest>())
                if (p->state == PeepState::walking && !p->outsideOfPark) {
                    selected = p;
                    break;
                }
            if (!selected)
                throw std::runtime_error("no walking guest for requested real-world edit");
            variables["$guest"] = selected->id.ToUnderlying();
            variables["$gx"] = selected->x & ~31;
            variables["$gy"] = selected->y & ~31;
            variables["$gz"] = selected->z;
            emit(label);
            continue;
        }
        if (op == "breakdown") {
            auto *ride = GetRide(RideId::FromUnderlying(variables.at("$ride").get<unsigned>()));
            if (!ride)
                throw std::runtime_error("breakdown target absent");
            // This is the very same entry point as Ride.cpp's maintenance
            // debug dropdown; upstream has no GameAction for a forced fault.
            RidePrepareBreakdown(*ride, static_cast<Breakdown>(row.value("reason", 0)));
            emit(label);
            continue;
        }
        if (op == "assert") {
            auto *ride = GetRide(RideId::FromUnderlying(variables.at("$ride").get<unsigned>()));
            if (row.at("condition") == "occupied_queue") {
                unsigned queue = 0;
                if (ride)
                    for (const auto &station : ride->getStations())
                        queue += station.queueLength;
                if (!ride || !ride->numRiders || !queue)
                    throw std::runtime_error("demolition precondition: guests must be on ride AND in its queue");
            } else if (row.at("condition") == "repaired") {
                unsigned repaired = 0;
                for (auto *staff : EntityList<Staff>())
                    if (staff->isMechanic())
                        repaired += staff->staffRidesFixed;
                if (!ride || ride->flags.hasAny(RideFlag::breakdownPending, RideFlag::brokenDown) || !repaired)
                    throw std::runtime_error("mechanic has not repaired forced breakdown");
            } else
                throw std::runtime_error("unknown assertion");
            emit(label);
            continue;
        }
        if (op == "reload") {
            const char *path = std::getenv("PARITY_SAVE");
            if (!path)
                throw std::runtime_error("PARITY_SAVE missing");
            const bool restoreView = testWindow != nullptr;
            if (testWindow) {
                ViewportRemove(testWindow->viewport);
                gWindowList.clear();
                testWindow = nullptr;
            }
            ParkFileExporter ex;
            ex.Export(getGameState(), path, 0);
            if (!ctx->LoadParkFromFile(path))
                throw std::runtime_error("reload failed");
            if (restoreView)
                countView();
            emit(label);
            continue;
        }
        const auto name = row.at("action").get<std::string>();
        GameActions::GameAction::Ptr action;
        for (int i = 0; i < int(GameCommand::count); ++i) {
            auto c = static_cast<GameCommand>(i);
            if (GameActions::IsValidId(i) && name == GameActions::GetName(c)) {
                action.reset((*GameActions::getFactory(c))());
                break;
            }
        }
        if (!action)
            throw std::runtime_error("unknown action: " + name);
        Parameters params(row.value("params", json::object()));
        action->AcceptParameters(params);
        params.check();
        GameActions::Result r;
        bool completed = false;
        action->SetCallback([&](const GameActions::GameAction *, const GameActions::Result *value) {
            r = *value;
            completed = true;
        });
        GameActions::Execute(action.get(), getGameState());
        // Player actions queue outside logic; drain that actual queue before
        // the checkpoint, and use the execution callback rather than the query.
        GameActions::ProcessQueue(getGameState());
        if (!completed)
            throw std::runtime_error("action execution callback missing");
        if (r.error == GameActions::Status::ok && name == "RideCreateAction") {
            const auto id = r.getData<RideId>().ToUnderlying();
            variables["$ride"] = id;
            extraDepart[id] = 0;
            seenVehicles[id] = false;
        }
        if (r.error == GameActions::Status::ok && name == "StaffHireNewAction")
            variables["$staff"] = r.getData<GameActions::StaffHireNewActionResult>().StaffEntityId.ToUnderlying();
        emit(label, {{"error", r.error}, {"cost", r.cost}, {"message", r.getErrorMessage()}, {"params", params.seen}});
    }
    emit("endpoint", nullptr, true);
    if (testWindow) {
        ViewportRemove(testWindow->viewport);
        gWindowList.clear();
        testWindow = nullptr;
    }
    return 0;
} catch (const std::exception &e) {
    std::cerr << "PARITY ERROR " << e.what() << std::endl;
    return 1;
}
